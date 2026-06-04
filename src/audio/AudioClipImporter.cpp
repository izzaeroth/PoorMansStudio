#include "audio/AudioClipImporter.h"

#include "audio/ExternalFfmpegEncoder.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <memory>
#include <system_error>

namespace
{
    std::string lowerExtension(const std::filesystem::path& path)
    {
        auto ext = path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return ext;
    }

    std::string safeBaseName(std::string value)
    {
        if (value.empty())
            value = "audio_clip";

        for (auto& c : value)
        {
            const bool ok = std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_';
            if (!ok)
                c = '_';
        }

        while (value.find("__") != std::string::npos)
            value.erase(value.find("__"), 1);

        if (value.empty() || value == "_")
            value = "audio_clip";

        return value;
    }


    struct AudioFileInfo
    {
        long long durationSamples = 0;
        double sampleRate = 48000.0;
        int channelCount = 2;
        int bitDepth = 24;
    };

    AudioFileInfo readAudioFileInfo(const std::filesystem::path& path)
    {
        AudioFileInfo info;

        if (path.empty() || !std::filesystem::exists(path))
            return info;

        juce::AudioFormatManager formatManager;
        formatManager.registerBasicFormats();

        std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(juce::File(path.string())));
        if (reader == nullptr)
            return info;

        info.durationSamples = static_cast<long long>(reader->lengthInSamples > 0 ? reader->lengthInSamples : 0);
        info.sampleRate = reader->sampleRate > 0.0 ? reader->sampleRate : info.sampleRate;
        info.channelCount = std::max(1, static_cast<int>(reader->numChannels));
        info.bitDepth = reader->bitsPerSample > 0 ? static_cast<int>(reader->bitsPerSample) : info.bitDepth;
        return info;
    }

    mw::audio::EncodedAudioFormat toFfmpegFormat(mw::core::AudioClipSavedFormat format)
    {
        switch (format)
        {
            case mw::core::AudioClipSavedFormat::Flac: return mw::audio::EncodedAudioFormat::Flac;
            case mw::core::AudioClipSavedFormat::Mp3: return mw::audio::EncodedAudioFormat::Mp3;
            case mw::core::AudioClipSavedFormat::Ogg: return mw::audio::EncodedAudioFormat::Ogg;
            case mw::core::AudioClipSavedFormat::M4a: return mw::audio::EncodedAudioFormat::M4a;
            case mw::core::AudioClipSavedFormat::Wav:
            default: return mw::audio::EncodedAudioFormat::Wav;
        }
    }

    std::string formatDisplayName(mw::core::AudioClipSavedFormat format)
    {
        switch (format)
        {
            case mw::core::AudioClipSavedFormat::Flac: return "FLAC";
            case mw::core::AudioClipSavedFormat::Mp3: return "MP3";
            case mw::core::AudioClipSavedFormat::Ogg: return "OGG";
            case mw::core::AudioClipSavedFormat::M4a: return "M4A";
            case mw::core::AudioClipSavedFormat::Wav:
            default: return "WAV";
        }
    }
}

namespace mw::audio
{
    bool AudioClipImporter::isSupportedImportPath(const std::filesystem::path& path)
    {
        const auto ext = lowerExtension(path);
        return ext == ".wav" || ext == ".mp3" || ext == ".flac" || ext == ".ogg" || ext == ".m4a";
    }

    std::filesystem::path AudioClipImporter::audioFolderFor(const std::filesystem::path& projectFolder, bool imported)
    {
        return projectFolder / "input" / "audio" / (imported ? "imported" : "recorded");
    }

    std::filesystem::path AudioClipImporter::tempAudioFolderFor(const std::filesystem::path& projectFolder)
    {
        return projectFolder / "input" / "audio" / "temp";
    }

    std::filesystem::path AudioClipImporter::makeUniqueMediaPath(const std::filesystem::path& folder, const std::string& baseName, mw::core::AudioClipSavedFormat format)
    {
        const auto safeName = safeBaseName(baseName);
        const auto ext = mw::core::audioClipSavedFormatToExtension(format);

        for (int i = 1; i < 10000; ++i)
        {
            auto candidate = folder / (safeName + "_" + (i < 10 ? "00" : i < 100 ? "0" : "") + std::to_string(i) + ext);
            if (!std::filesystem::exists(candidate))
                return candidate;
        }

        return folder / (safeName + ext);
    }

    std::uintmax_t AudioClipImporter::estimateRequiredBytes(const std::filesystem::path& sourcePath, mw::core::AudioClipSavedFormat savedFormat)
    {
        std::error_code ec;
        const auto sourceBytes = std::filesystem::exists(sourcePath, ec) ? std::filesystem::file_size(sourcePath, ec) : 0;
        const auto base = std::max<std::uintmax_t>(sourceBytes, 4u * 1024u * 1024u);

        switch (savedFormat)
        {
            case mw::core::AudioClipSavedFormat::Wav: return base * 12u + 64u * 1024u * 1024u;
            case mw::core::AudioClipSavedFormat::Flac: return base * 6u + 32u * 1024u * 1024u;
            case mw::core::AudioClipSavedFormat::Mp3:
            case mw::core::AudioClipSavedFormat::Ogg:
            case mw::core::AudioClipSavedFormat::M4a: return base * 2u + 16u * 1024u * 1024u;
            default: return base * 4u + 32u * 1024u * 1024u;
        }
    }

    bool AudioClipImporter::hasEnoughFreeSpace(const std::filesystem::path& targetFolder, std::uintmax_t requiredBytes, std::string& message)
    {
        std::error_code ec;
        std::filesystem::create_directories(targetFolder, ec);
        const auto info = std::filesystem::space(targetFolder, ec);

        if (ec)
        {
            message = "Unable to check free space for " + targetFolder.string() + ".";
            return false;
        }

        const auto safetyMargin = static_cast<std::uintmax_t>(128u) * 1024u * 1024u;
        if (info.available < requiredBytes + safetyMargin)
        {
            message = "Not enough free space in " + targetFolder.string() + ". Needed about "
                + std::to_string((requiredBytes + safetyMargin) / (1024u * 1024u))
                + " MB, available about " + std::to_string(info.available / (1024u * 1024u)) + " MB.";
            return false;
        }

        return true;
    }

    AudioClipImportResult AudioClipImporter::importToProject(const AudioClipImportRequest& request)
    {
        AudioClipImportResult result;

        if (!std::filesystem::exists(request.sourcePath) || !isSupportedImportPath(request.sourcePath))
        {
            result.message = "Unsupported or missing audio file. Supported import formats: WAV, MP3, FLAC, OGG, M4A.";
            return result;
        }

        const auto targetFolder = audioFolderFor(request.projectFolder, request.imported);
        std::error_code ec;
        std::filesystem::create_directories(targetFolder, ec);

        std::string spaceMessage;
        auto requiredBytes = estimateRequiredBytes(request.sourcePath, request.savedFormat);
        if (request.fallbackToReadableWav)
            requiredBytes = std::max(requiredBytes, estimateRequiredBytes(request.sourcePath, mw::core::AudioClipSavedFormat::Wav));

        if (!hasEnoughFreeSpace(targetFolder, requiredBytes, spaceMessage))
        {
            result.message = spaceMessage;
            return result;
        }

        const auto baseName = request.preferredName.empty() ? request.sourcePath.stem().string() : request.preferredName;

        auto renderOrCopyToFormat = [&](mw::core::AudioClipSavedFormat format,
                                        bool forceTranscode,
                                        std::filesystem::path& outputPath,
                                        AudioFileInfo& importedInfo,
                                        std::string& errorMessage) -> bool
        {
            outputPath = makeUniqueMediaPath(targetFolder, baseName, format);

            const auto outputExt = lowerExtension(outputPath);
            const auto inputExt = lowerExtension(request.sourcePath);
            const bool copyOnly = !forceTranscode && inputExt == outputExt;

            if (copyOnly)
            {
                if (!std::filesystem::copy_file(request.sourcePath, outputPath, std::filesystem::copy_options::overwrite_existing, ec) || ec)
                {
                    errorMessage = "Failed to copy audio file into the project media folder.";
                    std::filesystem::remove(outputPath, ec);
                    return false;
                }
            }
            else
            {
                FfmpegEncodeRequest encode;
                encode.ffmpegExePath = request.ffmpegExePath;
                encode.inputWavPath = request.sourcePath;
                encode.outputPath = outputPath;
                encode.format = toFfmpegFormat(format);
                encode.mp3BitrateKbps = std::clamp(request.qualityKbps, 128, 320);
                encode.outputChannels = request.channelCount == 1 ? 1 : 2;
                encode.overwriteExistingFile = true;
                encode.timeoutSeconds = 900;

                const auto encodeResult = ExternalFfmpegEncoder::encodeFromWav(encode);
                if (!encodeResult.success)
                {
                    errorMessage = "Audio import conversion failed: " + encodeResult.message;
                    std::filesystem::remove(outputPath, ec);
                    return false;
                }
            }

            importedInfo = readAudioFileInfo(outputPath);
            return true;
        };

        auto actualFormat = request.savedFormat;
        std::filesystem::path outputPath;
        AudioFileInfo importedInfo;
        std::string importError;

        if (!renderOrCopyToFormat(actualFormat, request.forceTranscode, outputPath, importedInfo, importError))
        {
            result.message = importError;
            return result;
        }

        if (importedInfo.durationSamples <= 0)
        {
            std::error_code removeError;
            std::filesystem::remove(outputPath, removeError);

            if (request.fallbackToReadableWav && actualFormat != mw::core::AudioClipSavedFormat::Wav)
            {
                actualFormat = mw::core::AudioClipSavedFormat::Wav;
                if (!renderOrCopyToFormat(actualFormat, true, outputPath, importedInfo, importError))
                {
                    result.message = importError;
                    return result;
                }
            }
            else if (!request.forceTranscode)
            {
                // Same-extension copies can preserve an odd container/codec that
                // previews through FFmpeg but cannot feed the editor waveform
                // reader.  Try one normalized FFmpeg pass before giving up.
                if (!renderOrCopyToFormat(actualFormat, true, outputPath, importedInfo, importError))
                {
                    result.message = importError;
                    return result;
                }
            }
        }

        if (importedInfo.durationSamples <= 0)
        {
            std::filesystem::remove(outputPath, ec);
            result.message = "Audio import created media, but the normalized project file still has no readable duration/waveform data.";
            return result;
        }

        result.success = true;
        result.absolutePath = outputPath;
        result.savedFormat = actualFormat;
        result.relativePath = std::filesystem::relative(outputPath, request.projectFolder, ec);
        if (ec)
            result.relativePath = outputPath;

        const auto sourceInfo = importedInfo;
        result.durationSamples = sourceInfo.durationSamples;
        result.sampleRate = sourceInfo.sampleRate > 0.0 ? sourceInfo.sampleRate : result.sampleRate;
        result.channelCount = sourceInfo.channelCount > 0 ? sourceInfo.channelCount : result.channelCount;
        result.bitDepth = sourceInfo.bitDepth > 0 ? sourceInfo.bitDepth : result.bitDepth;

        result.sizeBytes = std::filesystem::exists(outputPath, ec) ? std::filesystem::file_size(outputPath, ec) : 0;
        result.message = "Imported AudioClip media: " + result.relativePath.string();
        if (actualFormat != request.savedFormat)
            result.message += " (normalized to readable " + formatDisplayName(actualFormat) + ")";
        return result;
    }
}
