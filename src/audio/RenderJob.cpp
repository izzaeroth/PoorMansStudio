#include "audio/RenderJob.h"

#include "audio/ExternalFfmpegEncoder.h"
#include "audio/ExternalFfmpegMixer.h"
#include "audio/ExternalFluidSynthRenderer.h"
#include "audio/ExternalSfizzRenderer.h"
#include "audio/SfzValidator.h"
#include "vst/VstInstrumentHost.h"
#include "exporting/ExportSettings.h"
#include "midi/MidiExporter.h"
#include "serialization/ProjectSerializer.h"

#include <algorithm>
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

    mw::exporting::AudioFormat toExportingFormat(mw::audio::RenderOutputFormat format)
    {
        switch (format)
        {
            case mw::audio::RenderOutputFormat::Flac: return mw::exporting::AudioFormat::Flac;
            case mw::audio::RenderOutputFormat::Mp3: return mw::exporting::AudioFormat::Mp3;
            case mw::audio::RenderOutputFormat::Ogg: return mw::exporting::AudioFormat::Ogg;
            case mw::audio::RenderOutputFormat::Wav:
            default: return mw::exporting::AudioFormat::Wav;
        }
    }

    mw::audio::EncodedAudioFormat toEncodedFormat(mw::audio::RenderOutputFormat format)
    {
        switch (format)
        {
            case mw::audio::RenderOutputFormat::Flac: return mw::audio::EncodedAudioFormat::Flac;
            case mw::audio::RenderOutputFormat::Mp3: return mw::audio::EncodedAudioFormat::Mp3;
            case mw::audio::RenderOutputFormat::Ogg: return mw::audio::EncodedAudioFormat::Ogg;
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

    std::vector<std::filesystem::path> collectAudioClipInputs(
        const mw::audio::RenderJob& job,
        const mw::audio::RenderJobCallbacks& callbacks)
    {
        std::vector<std::filesystem::path> inputs;

        for (const auto& clip : job.project.getAudioClips())
        {
            const auto path = resolveAudioClipPath(job, clip);
            if (path.empty())
                continue;

            if (!std::filesystem::exists(path))
            {
                log(callbacks, "WARNING: AudioClip media not found for render/preview: " + path.string());
                continue;
            }

            inputs.push_back(path);
        }

        return inputs;
    }

    bool buildAudioClipOnlyWav(
        const mw::audio::RenderJob& job,
        const std::vector<std::filesystem::path>& audioInputs,
        const std::filesystem::path& outputWav,
        const mw::audio::RenderJobCallbacks& callbacks,
        std::string& errorMessage)
    {
        if (audioInputs.empty())
        {
            errorMessage = "No AudioClip media was available.";
            return false;
        }

        if (audioInputs.size() == 1)
        {
            mw::audio::FfmpegEncodeRequest encodeRequest;
            encodeRequest.ffmpegExePath = job.ffmpegPath;
            encodeRequest.inputWavPath = audioInputs.front();
            encodeRequest.outputPath = outputWav;
            encodeRequest.format = mw::audio::EncodedAudioFormat::Wav;
            encodeRequest.outputChannels = job.channelCount;
            encodeRequest.metadataTitle = job.metadataTitle;
            encodeRequest.metadataArtist = job.metadataArtist;
            encodeRequest.metadataAlbum = job.metadataAlbum;
            encodeRequest.metadataTrackNumber = job.metadataTrackNumber;
            encodeRequest.metadataYear = job.metadataYear;

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
        mixRequest.inputWavPaths = audioInputs;
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
        const std::vector<std::filesystem::path>& audioInputs,
        const std::filesystem::path& renderedWav,
        const mw::audio::RenderJobCallbacks& callbacks,
        std::string& errorMessage)
    {
        if (audioInputs.empty())
            return true;

        std::vector<std::filesystem::path> mixInputs;
        mixInputs.push_back(renderedWav);
        mixInputs.insert(mixInputs.end(), audioInputs.begin(), audioInputs.end());

        auto mixedPath = renderedWav;
        mixedPath.replace_filename(renderedWav.stem().string() + "_with_audioclips.wav");

        mw::audio::FfmpegMixRequest mixRequest;
        mixRequest.ffmpegExePath = job.ffmpegPath;
        mixRequest.inputWavPaths = mixInputs;
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
        std::filesystem::remove(renderedWav, ec);
        ec.clear();
        std::filesystem::rename(mixedPath, renderedWav, ec);
        if (ec)
        {
            errorMessage = "Failed to replace rendered WAV with AudioClip mix: " + ec.message();
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
            || backend == mw::core::SampleBackendType::VST3)
            return backend;

        return fallbackBackendForJob(job);
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
                || backend == mw::core::SampleBackendType::VST3)
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
                && trackBackend != mw::core::SampleBackendType::VST3)
                continue;

            auto stemProject = makeSingleTrackProject(job.project, track);
            for (auto& stemTrack : stemProject.getTracks())
                stemTrack.getMixerSettings().volume = std::clamp(stemTrack.getMixerSettings().volume * masterVolume, 0.0f, 1.5f);

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
        exportSettings.sampleRate = job.sampleRate;
        exportSettings.bitDepth = job.bitDepth;

        if (!mw::exporting::ExportPathBuilder::ensureOutputFolderExists(exportSettings))
        {
            result.message = "Failed to access export folder.";
            log(callbacks, "ERROR: " + result.message);
            return result;
        }

        auto renderProject = job.project;

        for (auto& track : renderProject.getTracks())
            track.getMixerSettings().volume = std::clamp(track.getMixerSettings().volume * job.masterVolume, 0.0f, 1.5f);

        // Render actions create audio/MIDI exports only. .mwproj files are written only by Save Project / Save As.
        result.projectPath.clear();
        result.midiPath = mw::exporting::ExportPathBuilder::buildMidiPath(exportSettings);
        result.wavPath = mw::exporting::ExportPathBuilder::buildAudioPath(exportSettings, mw::exporting::AudioFormat::Wav);

        if (isCancelled(cancelRequested, callbacks))
        {
            result.cancelled = true;
            return result;
        }

        const int eligibleStemCount = countEligibleStemTracks(job);
        const int prospectiveWorkerCount = resolveRenderWorkerCount(job, eligibleStemCount);
        const bool useStemRendering = projectHasPerTrackBackends(job) || (prospectiveWorkerCount > 1 && eligibleStemCount > 1);

        if (useStemRendering)
        {
            status(callbacks, "Preparing mixed stem render...");
            log(callbacks, projectHasPerTrackBackends(job)
                ? "Per-track backend assignments detected. Rendering stems."
                : "Multiple renderable tracks detected. Rendering independent stems with the Parallel Stem Renders setting.");

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

            const auto stemAudioClipInputs = collectAudioClipInputs(job, callbacks);
            for (const auto& audioClipInput : stemAudioClipInputs)
            {
                stemWavs.push_back(audioClipInput);
                log(callbacks, "Added AudioClip media to stem mix: " + audioClipInput.string());
            }

            stemWavs.erase(
                std::remove_if(
                    stemWavs.begin(),
                    stemWavs.end(),
                    [](const auto& path) { return path.empty(); }
                ),
                stemWavs.end()
            );

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

            if (stemWavs.size() == 1)
            {
                std::filesystem::copy_file(stemWavs.front(), result.wavPath, std::filesystem::copy_options::overwrite_existing, ignored);
                log(callbacks, "Copied single stem to final WAV: " + result.wavPath.string());
            }
            else
            {
                mw::audio::FfmpegMixRequest mixRequest;
                mixRequest.ffmpegExePath = job.ffmpegPath;
                mixRequest.inputWavPaths = stemWavs;
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
            encodeRequest.metadataAlbum = job.metadataAlbum;
            encodeRequest.metadataTrackNumber = job.metadataTrackNumber;
            encodeRequest.metadataYear = job.metadataYear;

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
                std::filesystem::remove(result.wavPath, ignored);
            if (encodeInputPath != result.wavPath)
                std::filesystem::remove(encodeInputPath, ignored);

            result.finalAudioPath = encodedPath;
            result.success = true;
            result.message = "Mixed stem render completed successfully.";
            log(callbacks, "Encoded output: " + encodedPath.string());
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

        const auto audioClipInputs = collectAudioClipInputs(job, callbacks);
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
            encodeRequest.metadataAlbum = job.metadataAlbum;
            encodeRequest.metadataTrackNumber = job.metadataTrackNumber;
            encodeRequest.metadataYear = job.metadataYear;

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
                std::filesystem::remove(result.wavPath, ignored);

            result.finalAudioPath = encodedPath;
            result.success = true;
            result.message = "AudioClip render completed successfully.";
            log(callbacks, "Encoded output: " + encodedPath.string());
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
        encodeRequest.metadataAlbum = job.metadataAlbum;
        encodeRequest.metadataTrackNumber = job.metadataTrackNumber;
        encodeRequest.metadataYear = job.metadataYear;

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
            std::filesystem::remove(result.wavPath, ignored);
        if (encodeInputPath != result.wavPath)
            std::filesystem::remove(encodeInputPath, ignored);

        result.finalAudioPath = encodedPath;
        result.success = true;
        result.message = "Render completed successfully.";
        log(callbacks, "Encoded output: " + encodedPath.string());
        log(callbacks, "Temporary WAV removed after encoding.");
        status(callbacks, "Render complete.");

        return result;
    }
}
