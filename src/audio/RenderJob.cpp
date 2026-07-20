#include "audio/RenderJob.h"

#include "audio/GainLimits.h"

#include "app/AppPaths.h"
#include "audio/ExternalFfmpegEncoder.h"
#include "audio/ExternalFfmpegMixer.h"
#include "audio/ExternalFluidSynthRenderer.h"
#include "audio/ExternalSfizzRenderer.h"
#include "audio/SfzValidator.h"
#include "clap/ClapInstrumentHost.h"
#include "vst/VstInstrumentHost.h"
#include "exporting/ExportSettings.h"
#include "midi/MidiExporter.h"
#include "serialization/ProjectSerializer.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <vector>

namespace
{
    void log(const mw::audio::RenderJobCallbacks& callbacks, const std::string& message)
    {
        if (callbacks.log)
            callbacks.log(message);
    }

    void status(const mw::audio::RenderJobCallbacks& callbacks, const std::string& message)
    {
        if (callbacks.status)
            callbacks.status(message);
    }

    mw::audio::EncodedAudioFormat toEncodedFormat(mw::audio::RenderOutputFormat format)
    {
        switch (format)
        {
            case mw::audio::RenderOutputFormat::Flac: return mw::audio::EncodedAudioFormat::Flac;
            case mw::audio::RenderOutputFormat::Mp3: return mw::audio::EncodedAudioFormat::Mp3;
            case mw::audio::RenderOutputFormat::Ogg: return mw::audio::EncodedAudioFormat::Ogg;
            case mw::audio::RenderOutputFormat::M4a: return mw::audio::EncodedAudioFormat::M4a;
            case mw::audio::RenderOutputFormat::Wav:
            default: return mw::audio::EncodedAudioFormat::Wav;
        }
    }

    bool isCancelled(std::atomic<bool>& cancelRequested, const mw::audio::RenderJobCallbacks& callbacks)
    {
        if (!cancelRequested)
            return false;

        log(callbacks, "Render cancelled.");
        return true;
    }

    bool hasRenderableMidi(const mw::core::Project& project)
    {
        return std::any_of(
            project.getTracks().begin(),
            project.getTracks().end(),
            [](const auto& track)
            {
                return !track.isAudioClipTrack() && !track.getMuted() && !track.getNotes().empty();
            }
        );
    }

    struct AudioClipRenderInput
    {
        std::filesystem::path path;
        double startOffsetSeconds = 0.0;
        double intendedDurationSeconds = 0.0;
        double gain = 1.0;
    };

    constexpr double kEffectSlotTailOverscanSeconds = 12.0;
    double ticksToSeconds(std::int64_t ticks, int tempoBpm)
    {
        const auto safeTempo = tempoBpm > 0 ? tempoBpm : 120;
        const auto safeTicks = std::max<std::int64_t>(0, ticks);
        const auto beats = static_cast<double>(safeTicks) / static_cast<double>(mw::core::Project::ticksPerQuarterNote);
        return beats * 60.0 / static_cast<double>(safeTempo);
    }

    bool hasAnyAudioClipStartOffset(const std::vector<AudioClipRenderInput>& inputs)
    {
        return std::any_of(
            inputs.begin(),
            inputs.end(),
            [](const auto& input) { return input.startOffsetSeconds > 0.0005; });
    }

    std::filesystem::path resolveAudioClipPath(const mw::audio::RenderJob& job, const mw::core::AudioClip& clip)
    {
        if (clip.projectRelativePath.empty())
            return {};

        if (clip.projectRelativePath.is_absolute())
            return clip.projectRelativePath;

        if (!job.projectFolder.empty())
            return job.projectFolder / clip.projectRelativePath;

        return clip.projectRelativePath;
    }

    std::string sanitizeAudioClipTempName(std::string value)
    {
        std::string cleaned;
        for (char c : value)
        {
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_')
                cleaned.push_back(c);
            else
                cleaned.push_back('_');
        }

        return cleaned.empty() ? std::string("clip") : cleaned;
    }

    double samplesToSeconds(long long samples, double sampleRate)
    {
        if (samples <= 0 || sampleRate <= 0.0)
            return 0.0;

        return static_cast<double>(samples) / sampleRate;
    }

    double intendedAudioClipDurationSeconds(const mw::core::AudioClip& clip)
    {
        if (clip.durationSamples <= 0 || clip.sampleRate <= 0.0)
            return 0.0;

        const auto trimmedSamples = mw::core::audioClipTrimmedDurationSamples(clip);
        const auto durationSamples = trimmedSamples > 0 ? trimmedSamples : clip.durationSamples;
        return samplesToSeconds(durationSamples, clip.sampleRate);
    }

    std::filesystem::path audioClipEffectTempFolderFor(const mw::audio::RenderJob& job)
    {
        const auto safeBaseName = sanitizeAudioClipTempName(job.baseFileName.empty() ? std::string("render") : job.baseFileName);
        return mw::app::AppPaths::tempFolder() / "audioclip_render" / safeBaseName;
    }

    bool capAudioClipPreparedWavToIntendedDuration(
        const mw::audio::RenderJob& job,
        const std::filesystem::path& wavPath,
        double intendedDurationSeconds,
        const mw::audio::RenderJobCallbacks& callbacks,
        std::string& errorMessage)
    {
        if (wavPath.empty() || intendedDurationSeconds <= 0.0 || !std::isfinite(intendedDurationSeconds))
            return true;

        const auto folder = audioClipEffectTempFolderFor(job);
        std::error_code ec;
        std::filesystem::create_directories(folder, ec);
        if (ec)
        {
            errorMessage = "Failed to create AudioClip duration-cap temp folder: " + ec.message();
            return false;
        }

        const auto cappedWav = folder / (sanitizeAudioClipTempName(wavPath.stem().string()) + "_duration_cap.wav");

        mw::audio::FfmpegTrimRequest trimRequest;
        trimRequest.ffmpegExePath = job.ffmpegPath;
        trimRequest.inputPath = wavPath;
        trimRequest.outputWavPath = cappedWav;
        trimRequest.trimStartSeconds = 0.0;
        trimRequest.trimDurationSeconds = intendedDurationSeconds;
        trimRequest.outputChannels = job.channelCount;

        const auto trimResult = mw::audio::ExternalFfmpegEncoder::trimToWav(trimRequest);
        log(callbacks, "FFmpeg AudioClip trim-duration cap command: " + trimResult.commandLine);
        log(callbacks, trimResult.message);

        if (!trimResult.success || !std::filesystem::exists(cappedWav))
        {
            errorMessage = "Failed to cap trimmed AudioClip preview/render length.";
            return false;
        }

        ec.clear();
        std::filesystem::remove(wavPath, ec);

        ec.clear();
        std::filesystem::rename(cappedWav, wavPath, ec);
        if (ec)
        {
            ec.clear();
            std::filesystem::copy_file(cappedWav, wavPath, std::filesystem::copy_options::overwrite_existing, ec);
            if (ec)
            {
                errorMessage = "Failed to replace AudioClip duration-capped WAV: " + ec.message();
                return false;
            }

            std::error_code ignored;
            std::filesystem::remove(cappedWav, ignored);
        }

        log(callbacks, "Trimmed AudioClip preview/render capped to kept source duration: " + std::to_string(intendedDurationSeconds) + " sec.");
        return true;
    }

    void cleanupAudioClipEffectTempFolderAfterSuccessfulRender(
        const mw::audio::RenderJob& job,
        const mw::audio::RenderJobCallbacks& callbacks)
    {
        const auto folder = audioClipEffectTempFolderFor(job);
        if (folder.empty())
            return;

        std::error_code ignored;
        if (std::filesystem::exists(folder, ignored))
        {
            std::filesystem::remove_all(folder, ignored);
            log(callbacks, "AudioClip render temp cleanup: removed temporary prepared/processed clip files.");
        }
    }

    bool shouldRenderAudioClipForTrack(const mw::audio::RenderJob& job, const mw::core::Track* track)
    {
        const bool anySolo = std::any_of(
            job.project.getTracks().begin(),
            job.project.getTracks().end(),
            [](const auto& t) { return t.getSolo(); });

        if (track == nullptr)
            return !anySolo;

        if (track->getMuted())
            return false;

        if (anySolo && !track->getSolo())
            return false;

        return true;
    }

    double audioClipRenderGainForTrack(const mw::audio::RenderJob& job, const mw::core::AudioClip& clip, const mw::core::Track* track)
    {
        const double trackVolume = track != nullptr ? static_cast<double>(track->getMixerSettings().volume) : 1.0;
        const double clipGain = std::isfinite(static_cast<double>(clip.gain)) ? static_cast<double>(clip.gain) : 1.0;
        const double masterVolume = std::isfinite(static_cast<double>(job.masterVolume)) ? static_cast<double>(job.masterVolume) : 1.0;
        // Match the existing MIDI render convention: track volume and master volume
        // combine into one final render gain capped at the app's main UI slider ceiling.
        return mw::audio::sanitizeMainUiGain(trackVolume * clipGain * masterVolume);
    }

    std::vector<AudioClipRenderInput> collectAudioClipInputs(
        const mw::audio::RenderJob& job,
        std::atomic<bool>& cancelRequested,
        const mw::audio::RenderJobCallbacks& callbacks,
        std::string& errorMessage)
    {
        std::vector<AudioClipRenderInput> inputs;

        auto lowerExtension = [](std::filesystem::path path)
        {
            auto ext = path.extension().string();
            for (auto& c : ext)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return ext;
        };

        int clipIndex = 0;
        for (const auto& clip : job.project.getAudioClips())
        {
            if (cancelRequested)
            {
                errorMessage = "Render cancelled while preparing AudioClip media.";
                return {};
            }

            const auto path = resolveAudioClipPath(job, clip);
            if (path.empty())
            {
                ++clipIndex;
                continue;
            }

            if (!std::filesystem::exists(path))
            {
                log(callbacks, "WARNING: AudioClip media not found for render/preview: " + path.string());
                ++clipIndex;
                continue;
            }

            const auto& tracks = job.project.getTracks();
            const bool hasEffectTrack = clip.trackIndex >= 0 && clip.trackIndex < static_cast<int>(tracks.size());
            const auto* track = hasEffectTrack ? &tracks[static_cast<std::size_t>(clip.trackIndex)] : nullptr;

            if (!shouldRenderAudioClipForTrack(job, track))
            {
                ++clipIndex;
                continue;
            }

            const auto renderGain = audioClipRenderGainForTrack(job, clip, track);
            if (renderGain <= 0.000001)
            {
                ++clipIndex;
                continue;
            }

            const auto sourceKind = mw::core::audioClipSourceTypeToString(clip.sourceType);
            const auto sourceKindTitle = sourceKind.empty()
                ? std::string("AudioClip")
                : std::string(1, static_cast<char>(std::toupper(static_cast<unsigned char>(sourceKind.front())))) + sourceKind.substr(1);

            const auto clipStem = sanitizeAudioClipTempName(!clip.name.empty() ? clip.name : path.stem().string());
            const auto effectFolder = audioClipEffectTempFolderFor(job);
            const auto startOffsetSeconds = ticksToSeconds(clip.startTick, job.project.getTempoBpm());
            const auto intendedDurationSeconds = intendedAudioClipDurationSeconds(clip);
            std::filesystem::path preparedSource = path;

            const bool hasActiveTrim = mw::core::audioClipHasActiveTrim(clip);
            if (hasActiveTrim)
            {
                if (clip.sampleRate <= 0.0)
                {
                    errorMessage = "Cannot apply AudioClip trim during render because sample rate is invalid for clip: " + clip.name;
                    return {};
                }

                const auto trimStartSamples = mw::core::audioClipTrimStartSamples(clip);
                const auto trimDurationSamples = mw::core::audioClipTrimmedDurationSamples(clip);
                if (trimDurationSamples <= 0)
                {
                    log(callbacks, "WARNING: AudioClip trim produced an empty kept range and was skipped: " + clip.name);
                    ++clipIndex;
                    continue;
                }

                std::error_code ec;
                std::filesystem::create_directories(effectFolder, ec);
                if (ec)
                {
                    errorMessage = "Failed to create AudioClip render temp folder: " + ec.message();
                    return {};
                }

                const auto trimmedWav = effectFolder / ("clip_" + std::to_string(clipIndex + 1) + "_" + clipStem + "_trim.wav");
                mw::audio::FfmpegTrimRequest trimRequest;
                trimRequest.ffmpegExePath = job.ffmpegPath;
                trimRequest.inputPath = path;
                trimRequest.outputWavPath = trimmedWav;
                trimRequest.trimStartSeconds = samplesToSeconds(trimStartSamples, clip.sampleRate);
                trimRequest.trimDurationSeconds = samplesToSeconds(trimDurationSamples, clip.sampleRate);
                trimRequest.outputChannels = job.channelCount;

                const auto trimResult = mw::audio::ExternalFfmpegEncoder::trimToWav(trimRequest);
                log(callbacks, "FFmpeg " + sourceKind + " AudioClip trim command: " + trimResult.commandLine);
                log(callbacks, trimResult.message);
                if (!trimResult.success || !std::filesystem::exists(trimmedWav))
                {
                    errorMessage = "Failed to prepare trimmed " + sourceKind + " AudioClip media for playback/render.";
                    return {};
                }

                preparedSource = trimmedWav;
                log(callbacks, sourceKindTitle + " AudioClip trim honored non-destructively. Source media unchanged: " + path.string());
            }

            if (track == nullptr || !mw::vst::VstInstrumentHost::trackHasEnabledVstEffect(*track))
            {
                if (hasActiveTrim
                    && !capAudioClipPreparedWavToIntendedDuration(job, preparedSource, intendedDurationSeconds, callbacks, errorMessage))
                    return {};

                inputs.push_back({ preparedSource, startOffsetSeconds, intendedDurationSeconds, renderGain });
                ++clipIndex;
                continue;
            }

            status(callbacks, "Processing " + sourceKind + " AudioClip track Effect Slot chain: " + track->getName());
            log(callbacks, sourceKindTitle + " AudioClip Effect Slot source stays dry/unchanged: " + path.string());

            std::error_code ec;
            std::filesystem::create_directories(effectFolder, ec);
            if (ec)
            {
                errorMessage = "Failed to create AudioClip render temp folder: " + ec.message();
                return {};
            }

            std::filesystem::path sourceWav = preparedSource;
            std::filesystem::path convertedWav = effectFolder / ("clip_" + std::to_string(clipIndex + 1) + "_" + clipStem + "_source.wav");
            const auto ext = lowerExtension(preparedSource);
            if (ext != ".wav")
            {
                mw::audio::FfmpegEncodeRequest convertRequest;
                convertRequest.ffmpegExePath = job.ffmpegPath;
                convertRequest.inputWavPath = preparedSource;
                convertRequest.outputPath = convertedWav;
                convertRequest.format = mw::audio::EncodedAudioFormat::Wav;
                convertRequest.outputChannels = job.channelCount;

                const auto convertResult = mw::audio::ExternalFfmpegEncoder::encodeFromWav(convertRequest);
                log(callbacks, "FFmpeg " + sourceKind + " AudioClip effect source command: " + convertResult.commandLine);
                log(callbacks, convertResult.message);
                if (!convertResult.success || !std::filesystem::exists(convertedWav))
                {
                    errorMessage = "Failed to prepare " + sourceKind + " AudioClip media for Effect Slot processing.";
                    return {};
                }
                sourceWav = convertedWav;
            }

            auto processedWav = effectFolder / ("clip_" + std::to_string(clipIndex + 1) + "_" + clipStem + "_vstfx.wav");
            mw::vst::VstEffectProcessRequest effectRequest;
            effectRequest.track = *track;
            effectRequest.inputWavPath = sourceWav;
            effectRequest.outputWavPath = processedWav;
            effectRequest.blockSize = 512;
            effectRequest.tailSeconds = kEffectSlotTailOverscanSeconds;
            effectRequest.cancelRequested = &cancelRequested;

            const auto effectResult = mw::vst::VstInstrumentHost::processWavWithTrackEffectChain(effectRequest);
            log(callbacks, effectResult.message);
            if (effectResult.cancelled)
            {
                errorMessage = sourceKindTitle + " AudioClip Effect Slot processing was cancelled.";
                return {};
            }
            if (!effectResult.success || !std::filesystem::exists(processedWav))
            {
                errorMessage = "Failed to process " + sourceKind + " AudioClip through Effect Slot chain for track: " + track->getName();
                return {};
            }

            if (hasActiveTrim)
            {
                log(callbacks, sourceKindTitle + " AudioClip Effect Slot tail preservation enabled: processed clip was not capped back to the trimmed source duration, so plugin tail/length extension can remain audible.");
            }

            inputs.push_back({ processedWav, startOffsetSeconds, intendedDurationSeconds + kEffectSlotTailOverscanSeconds, renderGain });
            log(callbacks, sourceKindTitle + " AudioClip Effect Slot processed for track: " + track->getName() + " -> " + processedWav.string());
            ++clipIndex;
        }

        return inputs;
    }

    bool buildAudioClipOnlyWav(
        const mw::audio::RenderJob& job,
        const std::vector<AudioClipRenderInput>& audioInputs,
        const std::filesystem::path& outputWav,
        const mw::audio::RenderJobCallbacks& callbacks,
        std::string& errorMessage)
    {
        if (audioInputs.empty())
        {
            errorMessage = "No AudioClip media was available.";
            return false;
        }

        if (audioInputs.size() == 1 && !hasAnyAudioClipStartOffset(audioInputs) && std::abs(audioInputs.front().gain - 1.0) <= 0.0005)
        {
            mw::audio::FfmpegEncodeRequest encodeRequest;
            encodeRequest.ffmpegExePath = job.ffmpegPath;
            encodeRequest.inputWavPath = audioInputs.front().path;
            encodeRequest.outputPath = outputWav;
            encodeRequest.format = mw::audio::EncodedAudioFormat::Wav;
            encodeRequest.outputChannels = job.channelCount;
            encodeRequest.metadataTitle = job.metadataTitle;
            encodeRequest.metadataArtist = job.metadataArtist;
            encodeRequest.metadataAlbumArtist = job.metadataAlbumArtist;
            encodeRequest.metadataAlbum = job.metadataAlbum;
            encodeRequest.metadataGenre = job.metadataGenre;
            encodeRequest.metadataTrackNumber = job.metadataTrackNumber;
            encodeRequest.metadataYear = job.metadataYear;
            encodeRequest.albumArtPath = job.albumArtPath;

            const auto encodeResult = mw::audio::ExternalFfmpegEncoder::encodeFromWav(encodeRequest);
            log(callbacks, "FFmpeg AudioClip preview/render command: " + encodeResult.commandLine);
            log(callbacks, encodeResult.message);
            if (!encodeResult.success)
            {
                errorMessage = "Failed to prepare AudioClip media for playback/render.";
                return false;
            }

            return true;
        }

        mw::audio::FfmpegMixRequest mixRequest;
        mixRequest.ffmpegExePath = job.ffmpegPath;
        for (const auto& input : audioInputs)
        {
            mixRequest.inputWavPaths.push_back(input.path);
            mixRequest.inputStartOffsetsSeconds.push_back(input.startOffsetSeconds);
            mixRequest.inputGains.push_back(input.gain);
        }
        mixRequest.outputWavPath = outputWav;

        const auto mixResult = mw::audio::ExternalFfmpegMixer::mixWavFiles(mixRequest);
        log(callbacks, "FFmpeg AudioClip mix command: " + mixResult.commandLine);
        log(callbacks, mixResult.message);
        if (!mixResult.success)
        {
            errorMessage = "Failed to mix AudioClip media.";
            return false;
        }

        return true;
    }

    bool mixAudioClipsWithRenderedWav(
        const mw::audio::RenderJob& job,
        const std::vector<AudioClipRenderInput>& audioInputs,
        const std::filesystem::path& renderedWav,
        const mw::audio::RenderJobCallbacks& callbacks,
        std::string& errorMessage)
    {
        if (audioInputs.empty())
            return true;

        const auto tempFolder = audioClipEffectTempFolderFor(job);
        std::error_code tempEc;
        std::filesystem::create_directories(tempFolder, tempEc);
        if (tempEc)
        {
            errorMessage = "Failed to create AudioClip render temp folder: " + tempEc.message();
            return false;
        }

        auto mixedPath = tempFolder / (renderedWav.stem().string() + "_with_audioclips.wav");

        mw::audio::FfmpegMixRequest mixRequest;
        mixRequest.ffmpegExePath = job.ffmpegPath;
        mixRequest.inputWavPaths.push_back(renderedWav);
        mixRequest.inputStartOffsetsSeconds.push_back(0.0);
        mixRequest.inputGains.push_back(1.0);
        for (const auto& input : audioInputs)
        {
            mixRequest.inputWavPaths.push_back(input.path);
            mixRequest.inputStartOffsetsSeconds.push_back(input.startOffsetSeconds);
            mixRequest.inputGains.push_back(input.gain);
        }
        mixRequest.outputWavPath = mixedPath;

        const auto mixResult = mw::audio::ExternalFfmpegMixer::mixWavFiles(mixRequest);
        log(callbacks, "FFmpeg AudioClip mix command: " + mixResult.commandLine);
        log(callbacks, mixResult.message);
        if (!mixResult.success)
        {
            errorMessage = "Failed to mix AudioClip media into rendered WAV.";
            return false;
        }

        std::error_code ec;
        std::filesystem::copy_file(mixedPath, renderedWav, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec)
        {
            errorMessage = "Failed to replace rendered WAV with AudioClip mix from temp: " + ec.message();
            return false;
        }

        log(callbacks, "Mixed AudioClip media into WAV: " + renderedWav.string());
        return true;
    }

}


    std::filesystem::path sanitizeStemName(const std::string& name)
    {
        std::string cleaned;

        for (char c : name)
        {
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_')
                cleaned.push_back(c);
            else
                cleaned.push_back('_');
        }

        if (cleaned.empty())
            cleaned = "track";

        return cleaned;
    }

    mw::core::SampleBackendType fallbackBackendForJob(const mw::audio::RenderJob& job)
    {
        return job.backend == mw::audio::RenderBackend::SFZ
            ? mw::core::SampleBackendType::SFZ
            : mw::core::SampleBackendType::SF2;
    }

    mw::core::SampleBackendType resolveTrackRenderBackend(const mw::audio::RenderJob& job, const mw::core::Track& track)
    {
        const auto backend = track.getInstrument().backendType;

        if (backend == mw::core::SampleBackendType::SF2
            || backend == mw::core::SampleBackendType::SFZ
            || backend == mw::core::SampleBackendType::VST3
            || backend == mw::core::SampleBackendType::CLAP)
            return backend;

        return fallbackBackendForJob(job);
    }

    bool projectHasClapInstrumentTracks(const mw::audio::RenderJob& job)
    {
        return std::any_of(
            job.project.getTracks().begin(),
            job.project.getTracks().end(),
            [](const auto& track)
            {
                return !track.isAudioClipTrack()
                    && !track.getMuted()
                    && !track.getNotes().empty()
                    && track.getInstrument().backendType == mw::core::SampleBackendType::CLAP;
            }
        );
    }

    bool projectHasPerTrackBackends(const mw::audio::RenderJob& job)
    {
        std::set<mw::core::SampleBackendType> assigned;

        for (const auto& track : job.project.getTracks())
        {
            if (track.isAudioClipTrack() || track.getMuted() || track.getNotes().empty())
                continue;

            assigned.insert(resolveTrackRenderBackend(job, track));
        }

        return assigned.size() > 1;
    }

    bool projectHasEnabledVstEffectChains(const mw::audio::RenderJob& job)
    {
        return std::any_of(
            job.project.getTracks().begin(),
            job.project.getTracks().end(),
            [](const auto& track)
            {
                return !track.isAudioClipTrack()
                    && !track.getMuted()
                    && !track.getNotes().empty()
                    && mw::vst::VstInstrumentHost::trackHasEnabledVstEffect(track);
            }
        );
    }

    int countEligibleStemTracks(const mw::audio::RenderJob& job)
    {
        const bool anySolo =
            std::any_of(
                job.project.getTracks().begin(),
                job.project.getTracks().end(),
                [](const auto& t) { return t.getSolo(); }
            );

        int count = 0;
        for (const auto& track : job.project.getTracks())
        {
            if (track.isAudioClipTrack() || track.getMuted())
                continue;

            if (anySolo && !track.getSolo())
                continue;

            if (track.getNotes().empty())
                continue;

            const auto backend = resolveTrackRenderBackend(job, track);
            if (backend == mw::core::SampleBackendType::SF2
                || backend == mw::core::SampleBackendType::SFZ
                || backend == mw::core::SampleBackendType::VST3
                || backend == mw::core::SampleBackendType::CLAP)
                ++count;
        }

        return count;
    }

    int conservativeAutoStemWorkerLimit(unsigned int hardwareThreads)
    {
        const unsigned int threads = std::max(1u, hardwareThreads);

        if (threads < 4u)
            return 1;

        if (threads < 8u)
            return 2;

        if (threads < 12u)
            return 4;

        if (threads < 16u)
            return 6;

        return 8;
    }

    int resolveRenderWorkerCount(const mw::audio::RenderJob& job, int eligibleStemCount)
    {
        if (eligibleStemCount <= 0)
            return 1;

        if (job.renderWorkerCount > 0)
            return std::clamp(job.renderWorkerCount, 1, eligibleStemCount);

        const unsigned int hardwareThreads = std::max(1u, std::thread::hardware_concurrency());
        const int autoStemLimit = conservativeAutoStemWorkerLimit(hardwareThreads);
        return std::clamp(std::min(autoStemLimit, eligibleStemCount), 1, eligibleStemCount);
    }

    std::string renderWorkerDescription(const mw::audio::RenderJob& job, int eligibleStemCount, int workers)
    {
        if (job.renderWorkerCount > 0)
            return "Parallel stem renders: fixed " + std::to_string(workers) + " for " + std::to_string(eligibleStemCount) + " eligible stem(s).";

        const unsigned int hardwareThreads = std::max(1u, std::thread::hardware_concurrency());
        const int autoStemLimit = conservativeAutoStemWorkerLimit(hardwareThreads);
        return "Parallel stem renders: Auto resolved to " + std::to_string(workers)
            + " (CPU threads " + std::to_string(hardwareThreads)
            + ", Auto stem limit " + std::to_string(autoStemLimit)
            + ", eligible stems " + std::to_string(eligibleStemCount)
            + ").";
    }

    void cleanupStemFilesAfterSuccessfulRender(
        const mw::audio::RenderJob& job,
        const std::filesystem::path& stemFolder,
        const mw::audio::RenderJobCallbacks& callbacks)
    {
        const int mask = std::clamp(job.keepStemFilesMask, 0, 3);
        const auto midiFolder = stemFolder / "midi";
        const auto audioFolder = stemFolder / "audio";

        std::error_code ignored;

        if (mask == 0)
        {
            std::filesystem::remove_all(stemFolder, ignored);
            log(callbacks, "Stem cleanup: removed MIDI and WAV stem files because Keep Stem Files is set to None.");
            return;
        }

        if ((mask & 1) == 0)
        {
            std::filesystem::remove_all(audioFolder, ignored);
            log(callbacks, "Stem cleanup: removed WAV stem files.");
        }
        else
        {
            log(callbacks, "Stem cleanup: kept WAV stem files in " + audioFolder.string());
        }

        if ((mask & 2) == 0)
        {
            std::filesystem::remove_all(midiFolder, ignored);
            log(callbacks, "Stem cleanup: removed MIDI stem files.");
        }
        else
        {
            log(callbacks, "Stem cleanup: kept MIDI stem files in " + midiFolder.string());
        }

        if (!std::filesystem::exists(audioFolder, ignored) && !std::filesystem::exists(midiFolder, ignored))
            std::filesystem::remove_all(stemFolder, ignored);
    }

    bool shouldKeepWavSidecar(const mw::audio::RenderJob& job)
    {
        return (std::clamp(job.keepStemFilesMask, 0, 3) & 1) != 0;
    }

    bool shouldKeepMidiSidecar(const mw::audio::RenderJob& job)
    {
        return (std::clamp(job.keepStemFilesMask, 0, 3) & 2) != 0;
    }

    void cleanupEncodedSourceWavAfterSuccessfulRender(
        const mw::audio::RenderJob& job,
        const std::filesystem::path& wavPath,
        const mw::audio::RenderJobCallbacks& callbacks)
    {
        if (wavPath.empty())
            return;

        std::error_code ignored;
        if (shouldKeepWavSidecar(job))
            log(callbacks, "Render cleanup: kept WAV sidecar/source file: " + wavPath.string());
        else
        {
            std::filesystem::remove(wavPath, ignored);
            log(callbacks, "Render cleanup: removed WAV sidecar/source file.");
        }
    }

    void cleanupMidiSidecarAfterSuccessfulRender(
        const mw::audio::RenderJob& job,
        const std::filesystem::path& midiPath,
        const mw::audio::RenderJobCallbacks& callbacks)
    {
        if (midiPath.empty())
            return;

        std::error_code ignored;
        if (shouldKeepMidiSidecar(job))
            log(callbacks, "Render cleanup: kept MIDI sidecar file: " + midiPath.string());
        else
        {
            std::filesystem::remove(midiPath, ignored);
            log(callbacks, "Render cleanup: removed MIDI sidecar file.");
        }
    }

    mw::core::Project makeSingleTrackProject(const mw::core::Project& source, const mw::core::Track& track)
    {
        mw::core::Project result(source.getName() + "_" + track.getName());
        result.setTempoBpm(source.getTempoBpm());
        result.setTimeSignature(source.getTimeSignature());
        result.getTracks().push_back(track);
        return result;
    }

    struct StemRenderTask
    {
        int stemIndex = 0;
        std::string trackName;
        mw::core::SampleBackendType backend = mw::core::SampleBackendType::SF2;
        mw::core::Project stemProject { "Stem" };
        std::filesystem::path stemMidi;
        std::filesystem::path stemWav;
        std::filesystem::path libraryPath;
    };

    struct StemRenderTaskResult
    {
        bool success = false;
        bool cancelled = false;
        std::filesystem::path stemWav;
        std::string errorMessage;
    };

    std::vector<StemRenderTask> buildStemRenderTasks(
        const mw::audio::RenderJob& job,
        const std::filesystem::path& stemFolder,
        float masterVolume)
    {
        std::vector<StemRenderTask> tasks;

        const bool anySolo =
            std::any_of(
                job.project.getTracks().begin(),
                job.project.getTracks().end(),
                [](const auto& t) { return t.getSolo(); }
            );

        int stemIndex = 1;
        for (const auto& track : job.project.getTracks())
        {
            if (track.isAudioClipTrack() || track.getMuted())
                continue;

            if (anySolo && !track.getSolo())
                continue;

            if (track.getNotes().empty())
                continue;

            const auto trackBackend = resolveTrackRenderBackend(job, track);
            if (trackBackend != mw::core::SampleBackendType::SF2
                && trackBackend != mw::core::SampleBackendType::SFZ
                && trackBackend != mw::core::SampleBackendType::VST3
                && trackBackend != mw::core::SampleBackendType::CLAP)
                continue;

            auto stemProject = makeSingleTrackProject(job.project, track);
            for (auto& stemTrack : stemProject.getTracks())
                stemTrack.getMixerSettings().volume = mw::audio::sanitizeMainUiGain(stemTrack.getMixerSettings().volume * static_cast<float>(masterVolume));

            if (trackBackend == mw::core::SampleBackendType::SFZ)
            {
                auto& stemTrack = stemProject.getTracks().front();
                if (job.sfzKeySwitch >= 0 && job.sfzKeySwitch <= 127)
                {
                    stemTrack.addNote(
                        mw::core::NoteEvent(
                            job.sfzKeySwitch,
                            1,
                            0,
                            10,
                            stemTrack.getInstrument().midiChannel,
                            mw::core::Articulation::Normal
                        )
                    );
                }
            }

            const auto stemBase = std::to_string(stemIndex) + "_" + sanitizeStemName(track.getName()).string();

            StemRenderTask task;
            task.stemIndex = stemIndex;
            task.trackName = track.getName();
            task.backend = trackBackend;
            task.stemProject = std::move(stemProject);
            task.stemMidi = stemFolder / "midi" / (stemBase + ".mid");
            task.stemWav = stemFolder / "audio" / (stemBase + ".wav");
            task.libraryPath = !track.getInstrument().sampleLibraryPath.empty()
                ? track.getInstrument().sampleLibraryPath
                : (trackBackend == mw::core::SampleBackendType::SFZ ? job.sfzPath : job.soundFontPath);

            tasks.push_back(std::move(task));
            ++stemIndex;
        }

        return tasks;
    }

    StemRenderTaskResult renderStemTask(
        const mw::audio::RenderJob& job,
        const StemRenderTask& task,
        std::atomic<bool>& cancelRequested,
        const mw::audio::RenderJobCallbacks& callbacks)
    {
        StemRenderTaskResult result;
        result.stemWav = task.stemWav;

        if (cancelRequested)
        {
            result.cancelled = true;
            return result;
        }

        status(callbacks, "Exporting stem MIDI: " + task.trackName);

        if (!mw::midi::MidiExporter::exportToFile(task.stemProject, task.stemMidi))
        {
            result.errorMessage = "Failed to export stem MIDI for track: " + task.trackName;
            return result;
        }

        if (cancelRequested)
        {
            result.cancelled = true;
            return result;
        }

        bool stemRendered = false;

        if (task.backend == mw::core::SampleBackendType::VST3)
        {
            status(callbacks, "Rendering VST3 stem: " + task.trackName);

            mw::vst::VstRenderRequest request;
            request.track = task.stemProject.getTracks().front();
            request.tempoBpm = task.stemProject.getTempoBpm();
            request.sampleRate = job.sampleRate;
            request.channelCount = job.channelCount;
            request.blockSize = 512;
            request.wavOutputPath = task.stemWav;
            request.cancelRequested = &cancelRequested;

            const auto vstResult = mw::vst::VstInstrumentHost::renderTrackToWav(request);
            log(callbacks, vstResult.message);
            if (vstResult.cancelled)
            {
                result.cancelled = true;
                return result;
            }
            stemRendered = vstResult.success;
        }
        else if (task.backend == mw::core::SampleBackendType::CLAP)
        {
            status(callbacks, "Rendering CLAP instrument stem: " + task.trackName);

            mw::clap::ClapInstrumentRenderRequest request;
            request.track = task.stemProject.getTracks().front();
            request.tempoBpm = task.stemProject.getTempoBpm();
            request.sampleRate = job.sampleRate;
            request.channelCount = job.channelCount;
            request.blockSize = 512;
            request.tailSeconds = 4.0;
            request.wavOutputPath = task.stemWav;
            request.cancelRequested = &cancelRequested;

            const auto clapResult = mw::clap::ClapInstrumentHost::renderTrackToWav(request);
            log(callbacks, clapResult.message);
            if (clapResult.cancelled)
            {
                result.cancelled = true;
                return result;
            }
            stemRendered = clapResult.success;
        }
        else if (task.backend == mw::core::SampleBackendType::SFZ)
        {
            const auto validation = mw::audio::SfzValidator::validateSampleReferences(task.libraryPath);
            log(callbacks, validation.message);

            if (!validation.ok)
            {
                result.errorMessage = "Missing SFZ samples for track: " + task.trackName;
                return result;
            }

            status(callbacks, "Rendering SFZ stem: " + task.trackName);

            mw::audio::SfizzRenderRequest request;
            request.sfizzRenderExePath = job.sfizzRenderPath;
            request.sfzPath = task.libraryPath;
            request.midiInputPath = task.stemMidi;
            request.wavOutputPath = task.stemWav;
            request.sampleRate = job.sampleRate;
            request.cancelRequested = &cancelRequested;

            const auto sfizzResult = mw::audio::ExternalSfizzRenderer::renderMidiToWav(request);
            log(callbacks, "sfizz-render stem command: " + sfizzResult.commandLine);
            log(callbacks, sfizzResult.message);
            stemRendered = sfizzResult.success;
        }
        else
        {
            status(callbacks, "Rendering SF2 stem: " + task.trackName);

            mw::audio::FluidSynthRenderRequest request;
            request.fluidSynthExePath = job.fluidSynthPath;
            request.soundFontPath = task.libraryPath;
            request.midiInputPath = task.stemMidi;
            request.wavOutputPath = task.stemWav;
            request.sampleRate = job.sampleRate;
            request.cancelRequested = &cancelRequested;

            const auto fluidResult = mw::audio::ExternalFluidSynthRenderer::renderMidiToWav(request);
            log(callbacks, "FluidSynth stem command: " + fluidResult.commandLine);
            log(callbacks, fluidResult.message);
            stemRendered = fluidResult.success;
        }

        if (cancelRequested)
        {
            result.cancelled = true;
            return result;
        }

        if (!stemRendered)
        {
            result.errorMessage = "Failed to render stem for track: " + task.trackName;
            return result;
        }

        const auto& renderedTrack = task.stemProject.getTracks().front();
        if (mw::vst::VstInstrumentHost::trackHasEnabledVstEffect(renderedTrack))
        {
            status(callbacks, "Processing Effect Slot chain: " + task.trackName);

            mw::vst::VstEffectProcessRequest effectRequest;
            effectRequest.track = renderedTrack;
            effectRequest.inputWavPath = task.stemWav;
            effectRequest.outputWavPath = task.stemWav;
            effectRequest.blockSize = 512;
            effectRequest.tailSeconds = kEffectSlotTailOverscanSeconds;
            effectRequest.cancelRequested = &cancelRequested;

            const auto effectResult = mw::vst::VstInstrumentHost::processWavWithTrackEffectChain(effectRequest);
            log(callbacks, effectResult.message);
            if (effectResult.cancelled)
            {
                result.cancelled = true;
                return result;
            }
            if (!effectResult.success)
            {
                result.errorMessage = "Failed to process Effect Slot chain for track: " + task.trackName;
                return result;
            }
        }

        result.success = true;
        log(callbacks, "Rendered stem: " + task.stemWav.string());
        return result;
    }


namespace mw::audio
{
    RenderJobResult RenderJobRunner::run(
        const RenderJob& job,
        std::atomic<bool>& cancelRequested,
        const RenderJobCallbacks& callbacks
    )
    {
        RenderJobResult result;

        status(callbacks, "Preparing render...");
        log(callbacks, "RenderJob started.");

        mw::exporting::ExportSettings exportSettings;
        exportSettings.outputFolder = job.exportFolder;
        exportSettings.baseFileName = job.baseFileName;

        if (!mw::exporting::ExportPathBuilder::ensureOutputFolderExists(exportSettings))
        {
            result.message = "Failed to access export folder.";
            log(callbacks, "ERROR: " + result.message);
            return result;
        }

        auto renderProject = job.project;

        for (auto& track : renderProject.getTracks())
            track.getMixerSettings().volume = mw::audio::sanitizeMainUiGain(track.getMixerSettings().volume * static_cast<float>(job.masterVolume));

        // Render actions create audio/MIDI exports only. .mwproj files are written only by Save Project / Save As.
        result.projectPath.clear();
        result.midiPath = mw::exporting::ExportPathBuilder::buildMidiPath(exportSettings);
        result.wavPath = mw::exporting::ExportPathBuilder::buildWavPath(exportSettings);

        if (isCancelled(cancelRequested, callbacks))
        {
            result.cancelled = true;
            return result;
        }

        const int eligibleStemCount = countEligibleStemTracks(job);
        const int prospectiveWorkerCount = resolveRenderWorkerCount(job, eligibleStemCount);
        const bool hasTrackVstEffectChains = projectHasEnabledVstEffectChains(job);
        const bool hasClapInstrumentTracks = projectHasClapInstrumentTracks(job);
        const bool useStemRendering = hasTrackVstEffectChains || hasClapInstrumentTracks || projectHasPerTrackBackends(job) || (prospectiveWorkerCount > 1 && eligibleStemCount > 1);

        if (useStemRendering)
        {
            status(callbacks, "Preparing mixed stem render...");
            log(callbacks, hasTrackVstEffectChains
                ? "Enabled Effect Slot chains detected. Rendering stems so effects remain track-owned."
                : (hasClapInstrumentTracks
                    ? "CLAP instrument assignments detected. Rendering stems so CLAP instruments remain track-owned."
                    : (projectHasPerTrackBackends(job)
                        ? "Per-track backend assignments detected. Rendering stems."
                        : "Multiple renderable tracks detected. Rendering independent stems with the Parallel Stem Renders setting.")));

            const auto stemFolder = job.exportFolder / (job.baseFileName + "_stems");
            std::error_code ignored;
            std::filesystem::create_directories(stemFolder / "midi", ignored);
            std::filesystem::create_directories(stemFolder / "audio", ignored);

            const auto stemTasks = buildStemRenderTasks(job, stemFolder, job.masterVolume);
            const int workerCount = resolveRenderWorkerCount(job, static_cast<int>(stemTasks.size()));
            log(callbacks, renderWorkerDescription(job, static_cast<int>(stemTasks.size()), workerCount));

            std::vector<std::filesystem::path> stemWavs(stemTasks.size());
            std::vector<StemRenderTaskResult> stemResults(stemTasks.size());

            if (stemTasks.empty())
            {
                result.message = "No assigned, unmuted tracks were available for stem rendering.";
                log(callbacks, "ERROR: " + result.message);
                return result;
            }

            if (workerCount <= 1 || stemTasks.size() <= 1)
            {
                for (int i = 0; i < static_cast<int>(stemTasks.size()); ++i)
                {
                    if (isCancelled(cancelRequested, callbacks))
                    {
                        result.cancelled = true;
                        return result;
                    }

                    stemResults[static_cast<std::size_t>(i)] = renderStemTask(job, stemTasks[static_cast<std::size_t>(i)], cancelRequested, callbacks);

                    if (stemResults[static_cast<std::size_t>(i)].cancelled)
                    {
                        result.cancelled = true;
                        return result;
                    }

                    if (!stemResults[static_cast<std::size_t>(i)].success)
                    {
                        result.message = stemResults[static_cast<std::size_t>(i)].errorMessage;
                        log(callbacks, "ERROR: " + result.message);
                        return result;
                    }

                    stemWavs[static_cast<std::size_t>(i)] = stemResults[static_cast<std::size_t>(i)].stemWav;
                }
            }
            else
            {
                status(callbacks, "Rendering stems with " + std::to_string(workerCount) + " parallel stem render job(s)...");

                std::atomic<int> nextTaskIndex { 0 };
                std::vector<std::thread> workers;
                workers.reserve(static_cast<std::size_t>(workerCount));

                for (int workerIndex = 0; workerIndex < workerCount; ++workerIndex)
                {
                    workers.emplace_back(
                        [&]
                        {
                            while (!cancelRequested)
                            {
                                const int taskIndex = nextTaskIndex.fetch_add(1);
                                if (taskIndex >= static_cast<int>(stemTasks.size()))
                                    break;

                                stemResults[static_cast<std::size_t>(taskIndex)] = renderStemTask(job, stemTasks[static_cast<std::size_t>(taskIndex)], cancelRequested, callbacks);
                            }
                        }
                    );
                }

                for (auto& worker : workers)
                {
                    if (worker.joinable())
                        worker.join();
                }

                if (cancelRequested)
                {
                    result.cancelled = true;
                    log(callbacks, "Render cancelled. Active parallel stem render processes were stopped where supported.");
                    return result;
                }

                for (int i = 0; i < static_cast<int>(stemResults.size()); ++i)
                {
                    const auto& stemResult = stemResults[static_cast<std::size_t>(i)];
                    if (stemResult.cancelled)
                    {
                        result.cancelled = true;
                        return result;
                    }

                    if (!stemResult.success)
                    {
                        result.message = stemResult.errorMessage.empty()
                            ? "Failed to render stem for track: " + stemTasks[static_cast<std::size_t>(i)].trackName
                            : stemResult.errorMessage;
                        log(callbacks, "ERROR: " + result.message);
                        return result;
                    }

                    stemWavs[static_cast<std::size_t>(i)] = stemResult.stemWav;
                }
            }

            std::string stemAudioClipError;
            const auto stemAudioClipInputs = collectAudioClipInputs(job, cancelRequested, callbacks, stemAudioClipError);
            if (!stemAudioClipError.empty())
            {
                result.message = stemAudioClipError;
                log(callbacks, "ERROR: " + result.message);
                return result;
            }
            std::vector<double> stemStartOffsetsSeconds(stemWavs.size(), 0.0);
            std::vector<double> stemInputGains(stemWavs.size(), 1.0);
            for (const auto& audioClipInput : stemAudioClipInputs)
            {
                stemWavs.push_back(audioClipInput.path);
                stemStartOffsetsSeconds.push_back(audioClipInput.startOffsetSeconds);
                stemInputGains.push_back(audioClipInput.gain);
                log(callbacks, "Added AudioClip media to stem mix with track/master volume: " + audioClipInput.path.string());
            }

            for (int i = static_cast<int>(stemWavs.size()) - 1; i >= 0; --i)
            {
                if (stemWavs[static_cast<std::size_t>(i)].empty())
                {
                    stemWavs.erase(stemWavs.begin() + i);
                    stemStartOffsetsSeconds.erase(stemStartOffsetsSeconds.begin() + i);
                    stemInputGains.erase(stemInputGains.begin() + i);
                }
            }

            if (stemWavs.empty())
            {
                result.message = "No assigned, unmuted tracks or AudioClips were available for stem rendering.";
                log(callbacks, "ERROR: " + result.message);
                return result;
            }

            if (isCancelled(cancelRequested, callbacks))
            {
                result.cancelled = true;
                return result;
            }

            status(callbacks, "Mixing stems and AudioClips...");

            const bool singleStemCanCopy = stemWavs.size() == 1
                && (stemStartOffsetsSeconds.empty() || stemStartOffsetsSeconds.front() <= 0.0005)
                && (stemInputGains.empty() || std::abs(stemInputGains.front() - 1.0) <= 0.0005);

            if (singleStemCanCopy)
            {
                std::filesystem::copy_file(stemWavs.front(), result.wavPath, std::filesystem::copy_options::overwrite_existing, ignored);
                log(callbacks, "Copied single stem to final WAV: " + result.wavPath.string());
            }
            else
            {
                mw::audio::FfmpegMixRequest mixRequest;
                mixRequest.ffmpegExePath = job.ffmpegPath;
                mixRequest.inputWavPaths = stemWavs;
                mixRequest.inputStartOffsetsSeconds = stemStartOffsetsSeconds;
                mixRequest.inputGains = stemInputGains;
                mixRequest.outputWavPath = result.wavPath;

                const auto mixResult = mw::audio::ExternalFfmpegMixer::mixWavFiles(mixRequest);
                log(callbacks, "FFmpeg mix command: " + mixResult.commandLine);
                log(callbacks, mixResult.message);

                if (!mixResult.success)
                {
                    result.message = "Failed to mix stems.";
                    log(callbacks, "ERROR: " + result.message);
                    return result;
                }
            }

            log(callbacks, "Mixed WAV: " + result.wavPath.string());

            const auto requestedEncodedFormat = toEncodedFormat(job.outputFormat);

            if (requestedEncodedFormat == mw::audio::EncodedAudioFormat::Wav && job.channelCount == 2)
            {
                result.finalAudioPath = result.wavPath;
                result.success = true;
                result.message = "Mixed stem render completed successfully.";
                cleanupAudioClipEffectTempFolderAfterSuccessfulRender(job, callbacks);
                cleanupStemFilesAfterSuccessfulRender(job, stemFolder, callbacks);
                status(callbacks, "Render complete.");
                return result;
            }

            status(callbacks, requestedEncodedFormat == mw::audio::EncodedAudioFormat::Wav ? "Converting final WAV channels..." : "Encoding final audio...");

            auto encodedPath = result.wavPath;
            encodedPath.replace_extension(mw::audio::encodedAudioFormatToExtension(requestedEncodedFormat));

            auto encodeInputPath = result.wavPath;
            if (requestedEncodedFormat == mw::audio::EncodedAudioFormat::Wav && job.channelCount != 2)
            {
                encodeInputPath = result.wavPath;
                encodeInputPath.replace_filename(result.wavPath.stem().string() + "_render_source.wav");
                std::filesystem::rename(result.wavPath, encodeInputPath, ignored);
                if (ignored)
                {
                    result.message = "Failed to prepare WAV channel conversion.";
                    log(callbacks, "ERROR: " + result.message);
                    return result;
                }
            }

            mw::audio::FfmpegEncodeRequest encodeRequest;
            encodeRequest.ffmpegExePath = job.ffmpegPath;
            encodeRequest.inputWavPath = encodeInputPath;
            encodeRequest.outputPath = encodedPath;
            encodeRequest.format = requestedEncodedFormat;
            encodeRequest.mp3BitrateKbps = job.bitrateKbps;
            encodeRequest.outputChannels = job.channelCount;
            encodeRequest.metadataTitle = job.metadataTitle;
            encodeRequest.metadataArtist = job.metadataArtist;
            encodeRequest.metadataAlbumArtist = job.metadataAlbumArtist;
            encodeRequest.metadataAlbum = job.metadataAlbum;
            encodeRequest.metadataGenre = job.metadataGenre;
            encodeRequest.metadataTrackNumber = job.metadataTrackNumber;
            encodeRequest.metadataYear = job.metadataYear;
            encodeRequest.albumArtPath = job.albumArtPath;

            const auto encodeResult = mw::audio::ExternalFfmpegEncoder::encodeFromWav(encodeRequest);
            log(callbacks, "FFmpeg command: " + encodeResult.commandLine);
            log(callbacks, encodeResult.message);

            if (!encodeResult.success)
            {
                result.message = "Encoding failed. Mixed WAV kept for troubleshooting: " + result.wavPath.string();
                log(callbacks, "ERROR: " + result.message);
                return result;
            }

            if (requestedEncodedFormat != mw::audio::EncodedAudioFormat::Wav)
                cleanupEncodedSourceWavAfterSuccessfulRender(job, result.wavPath, callbacks);
            if (encodeInputPath != result.wavPath)
                std::filesystem::remove(encodeInputPath, ignored);

            result.finalAudioPath = encodedPath;
            result.success = true;
            result.message = "Mixed stem render completed successfully.";
            log(callbacks, "Encoded output: " + encodedPath.string());
            cleanupAudioClipEffectTempFolderAfterSuccessfulRender(job, callbacks);
            cleanupStemFilesAfterSuccessfulRender(job, stemFolder, callbacks);
            status(callbacks, "Render complete.");
            return result;
        }

        if (job.backend == RenderBackend::SFZ)
        {
            if (!job.sfzPath.empty())
            {
                status(callbacks, "Checking SFZ samples...");
                const auto validation = mw::audio::SfzValidator::validateSampleReferences(job.sfzPath);
                log(callbacks, validation.message);

                if (!validation.ok)
                {
                    result.message = "SFZ render cancelled because required samples are missing.";
                    log(callbacks, "ERROR: " + result.message);
                    return result;
                }
            }

            if (job.sfzKeySwitch >= 0 && job.sfzKeySwitch <= 127 && !renderProject.getTracks().empty())
            {
                auto& firstTrack = renderProject.getTracks().front();
                firstTrack.addNote(
                    mw::core::NoteEvent(
                        job.sfzKeySwitch,
                        1,
                        0,
                        10,
                        firstTrack.getInstrument().midiChannel,
                        mw::core::Articulation::Normal
                    )
                );

                log(callbacks, "SFZ startup keyswitch sent: MIDI note " + std::to_string(job.sfzKeySwitch));
            }
        }

        if (isCancelled(cancelRequested, callbacks))
        {
            result.cancelled = true;
            return result;
        }

        std::string audioClipErrorMessage;
        const auto audioClipInputs = collectAudioClipInputs(job, cancelRequested, callbacks, audioClipErrorMessage);
        if (!audioClipErrorMessage.empty())
        {
            result.message = audioClipErrorMessage;
            log(callbacks, "ERROR: " + result.message);
            return result;
        }
        const bool hasMidiContent = hasRenderableMidi(renderProject);

        if (!hasMidiContent && !audioClipInputs.empty())
        {
            status(callbacks, "Preparing AudioClip preview/render...");
            std::string audioClipError;
            if (!buildAudioClipOnlyWav(job, audioClipInputs, result.wavPath, callbacks, audioClipError))
            {
                result.message = audioClipError;
                log(callbacks, "ERROR: " + result.message);
                return result;
            }

            log(callbacks, "Prepared AudioClip WAV: " + result.wavPath.string());

            if (isCancelled(cancelRequested, callbacks))
            {
                result.cancelled = true;
                return result;
            }

            const auto requestedEncodedFormat = toEncodedFormat(job.outputFormat);

            if (requestedEncodedFormat == mw::audio::EncodedAudioFormat::Wav && job.channelCount == 2)
            {
                result.finalAudioPath = result.wavPath;
                result.success = true;
                result.message = "AudioClip render completed successfully.";
                cleanupAudioClipEffectTempFolderAfterSuccessfulRender(job, callbacks);
                status(callbacks, "Render complete.");
                return result;
            }

            status(callbacks, requestedEncodedFormat == mw::audio::EncodedAudioFormat::Wav ? "Converting final WAV channels..." : "Encoding final audio...");

            auto encodedPath = result.wavPath;
            encodedPath.replace_extension(mw::audio::encodedAudioFormatToExtension(requestedEncodedFormat));

            mw::audio::FfmpegEncodeRequest encodeRequest;
            encodeRequest.ffmpegExePath = job.ffmpegPath;
            encodeRequest.inputWavPath = result.wavPath;
            encodeRequest.outputPath = encodedPath;
            encodeRequest.format = requestedEncodedFormat;
            encodeRequest.mp3BitrateKbps = job.bitrateKbps;
            encodeRequest.outputChannels = job.channelCount;
            encodeRequest.metadataTitle = job.metadataTitle;
            encodeRequest.metadataArtist = job.metadataArtist;
            encodeRequest.metadataAlbumArtist = job.metadataAlbumArtist;
            encodeRequest.metadataAlbum = job.metadataAlbum;
            encodeRequest.metadataGenre = job.metadataGenre;
            encodeRequest.metadataTrackNumber = job.metadataTrackNumber;
            encodeRequest.metadataYear = job.metadataYear;
            encodeRequest.albumArtPath = job.albumArtPath;

            const auto encodeResult = mw::audio::ExternalFfmpegEncoder::encodeFromWav(encodeRequest);
            log(callbacks, "FFmpeg command: " + encodeResult.commandLine);
            log(callbacks, encodeResult.message);

            if (!encodeResult.success)
            {
                result.message = "Encoding failed. Temporary WAV kept for troubleshooting: " + result.wavPath.string();
                log(callbacks, "ERROR: " + result.message);
                return result;
            }

            if (requestedEncodedFormat != mw::audio::EncodedAudioFormat::Wav)
                cleanupEncodedSourceWavAfterSuccessfulRender(job, result.wavPath, callbacks);

            result.finalAudioPath = encodedPath;
            result.success = true;
            result.message = "AudioClip render completed successfully.";
            log(callbacks, "Encoded output: " + encodedPath.string());
            cleanupAudioClipEffectTempFolderAfterSuccessfulRender(job, callbacks);
            status(callbacks, "Render complete.");
            return result;
        }

        status(callbacks, "Exporting MIDI...");
        if (!mw::midi::MidiExporter::exportToFile(renderProject, result.midiPath))
        {
            result.message = "Failed to export MIDI.";
            log(callbacks, "ERROR: " + result.message);
            return result;
        }

        log(callbacks, "Exported MIDI: " + result.midiPath.string());

        if (isCancelled(cancelRequested, callbacks))
        {
            result.cancelled = true;
            return result;
        }

        status(callbacks, "Rendering WAV...");

        bool renderSuccess = false;

        if (job.backend == RenderBackend::SFZ)
        {
            mw::audio::SfizzRenderRequest request;
            request.sfizzRenderExePath = job.sfizzRenderPath;
            request.sfzPath = job.sfzPath;
            request.midiInputPath = result.midiPath;
            request.wavOutputPath = result.wavPath;
            request.sampleRate = job.sampleRate;

            const auto sfizzResult = mw::audio::ExternalSfizzRenderer::renderMidiToWav(request);
            log(callbacks, "sfizz-render command: " + sfizzResult.commandLine);
            log(callbacks, sfizzResult.message);
            renderSuccess = sfizzResult.success;
        }
        else
        {
            mw::audio::FluidSynthRenderRequest request;
            request.fluidSynthExePath = job.fluidSynthPath;
            request.soundFontPath = job.soundFontPath;
            request.midiInputPath = result.midiPath;
            request.wavOutputPath = result.wavPath;
            request.sampleRate = job.sampleRate;

            const auto fluidResult = mw::audio::ExternalFluidSynthRenderer::renderMidiToWav(request);
            log(callbacks, "FluidSynth command: " + fluidResult.commandLine);
            log(callbacks, fluidResult.message);
            renderSuccess = fluidResult.success;
        }

        if (!renderSuccess)
        {
            result.message = "Audio render failed.";
            log(callbacks, "ERROR: " + result.message);
            return result;
        }

        log(callbacks, "Rendered WAV: " + result.wavPath.string());

        if (!audioClipInputs.empty())
        {
            status(callbacks, "Mixing AudioClips into preview/render...");
            std::string audioClipMixError;
            if (!mixAudioClipsWithRenderedWav(job, audioClipInputs, result.wavPath, callbacks, audioClipMixError))
            {
                result.message = audioClipMixError;
                log(callbacks, "ERROR: " + result.message);
                return result;
            }
        }

        if (isCancelled(cancelRequested, callbacks))
        {
            result.cancelled = true;
            return result;
        }

        const auto requestedEncodedFormat = toEncodedFormat(job.outputFormat);

        if (requestedEncodedFormat == mw::audio::EncodedAudioFormat::Wav && job.channelCount == 2)
        {
            result.finalAudioPath = result.wavPath;
            result.success = true;
            result.message = "Render completed successfully.";
            cleanupAudioClipEffectTempFolderAfterSuccessfulRender(job, callbacks);
            cleanupMidiSidecarAfterSuccessfulRender(job, result.midiPath, callbacks);
            status(callbacks, "Render complete.");
            return result;
        }

        status(callbacks, requestedEncodedFormat == mw::audio::EncodedAudioFormat::Wav ? "Converting final WAV channels..." : "Encoding final audio...");

        auto encodedPath = result.wavPath;
        encodedPath.replace_extension(mw::audio::encodedAudioFormatToExtension(requestedEncodedFormat));

        auto encodeInputPath = result.wavPath;
        if (requestedEncodedFormat == mw::audio::EncodedAudioFormat::Wav && job.channelCount != 2)
        {
            encodeInputPath = result.wavPath;
            encodeInputPath.replace_filename(result.wavPath.stem().string() + "_render_source.wav");
            std::error_code renameIgnored;
            std::filesystem::rename(result.wavPath, encodeInputPath, renameIgnored);
            if (renameIgnored)
            {
                result.message = "Failed to prepare WAV channel conversion.";
                log(callbacks, "ERROR: " + result.message);
                return result;
            }
        }

        mw::audio::FfmpegEncodeRequest encodeRequest;
        encodeRequest.ffmpegExePath = job.ffmpegPath;
        encodeRequest.inputWavPath = encodeInputPath;
        encodeRequest.outputPath = encodedPath;
        encodeRequest.format = requestedEncodedFormat;
        encodeRequest.mp3BitrateKbps = job.bitrateKbps;
        encodeRequest.outputChannels = job.channelCount;
        encodeRequest.metadataTitle = job.metadataTitle;
        encodeRequest.metadataArtist = job.metadataArtist;
        encodeRequest.metadataAlbumArtist = job.metadataAlbumArtist;
        encodeRequest.metadataAlbum = job.metadataAlbum;
        encodeRequest.metadataGenre = job.metadataGenre;
        encodeRequest.metadataTrackNumber = job.metadataTrackNumber;
        encodeRequest.metadataYear = job.metadataYear;
        encodeRequest.albumArtPath = job.albumArtPath;

        const auto encodeResult = mw::audio::ExternalFfmpegEncoder::encodeFromWav(encodeRequest);

        log(callbacks, "FFmpeg command: " + encodeResult.commandLine);
        log(callbacks, encodeResult.message);

        if (!encodeResult.success)
        {
            result.message = "Encoding failed. Temporary WAV kept for troubleshooting: " + result.wavPath.string();
            log(callbacks, "ERROR: " + result.message);
            return result;
        }

        std::error_code ignored;
        if (requestedEncodedFormat != mw::audio::EncodedAudioFormat::Wav)
            cleanupEncodedSourceWavAfterSuccessfulRender(job, result.wavPath, callbacks);
        if (encodeInputPath != result.wavPath)
            std::filesystem::remove(encodeInputPath, ignored);
        cleanupMidiSidecarAfterSuccessfulRender(job, result.midiPath, callbacks);

        result.finalAudioPath = encodedPath;
        result.success = true;
        result.message = "Render completed successfully.";
        log(callbacks, "Encoded output: " + encodedPath.string());
        cleanupAudioClipEffectTempFolderAfterSuccessfulRender(job, callbacks);
        log(callbacks, "Render cleanup completed after encoding.");
        status(callbacks, "Render complete.");

        return result;
    }
}
