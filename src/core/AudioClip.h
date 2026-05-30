#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace mw::core
{
    enum class AudioClipSourceType
    {
        Imported,
        Recorded
    };

    enum class AudioClipSavedFormat
    {
        Wav = 1,
        Flac = 2,
        Mp3 = 3,
        Ogg = 4
    };

    inline std::string audioClipSourceTypeToString(AudioClipSourceType type)
    {
        switch (type)
        {
            case AudioClipSourceType::Recorded: return "recorded";
            case AudioClipSourceType::Imported:
            default: return "imported";
        }
    }

    inline AudioClipSourceType audioClipSourceTypeFromString(const std::string& value)
    {
        if (value == "recorded") return AudioClipSourceType::Recorded;
        return AudioClipSourceType::Imported;
    }

    inline std::string audioClipSavedFormatToString(AudioClipSavedFormat format)
    {
        switch (format)
        {
            case AudioClipSavedFormat::Flac: return "flac";
            case AudioClipSavedFormat::Mp3: return "mp3";
            case AudioClipSavedFormat::Ogg: return "ogg";
            case AudioClipSavedFormat::Wav:
            default: return "wav";
        }
    }

    inline std::string audioClipSavedFormatToExtension(AudioClipSavedFormat format)
    {
        return "." + audioClipSavedFormatToString(format);
    }

    inline AudioClipSavedFormat audioClipSavedFormatFromId(int id)
    {
        switch (id)
        {
            case 2: return AudioClipSavedFormat::Flac;
            case 3: return AudioClipSavedFormat::Mp3;
            case 4: return AudioClipSavedFormat::Ogg;
            case 1:
            default: return AudioClipSavedFormat::Wav;
        }
    }

    inline int audioClipSavedFormatToId(AudioClipSavedFormat format)
    {
        switch (format)
        {
            case AudioClipSavedFormat::Flac: return 2;
            case AudioClipSavedFormat::Mp3: return 3;
            case AudioClipSavedFormat::Ogg: return 4;
            case AudioClipSavedFormat::Wav:
            default: return 1;
        }
    }

    inline AudioClipSavedFormat audioClipSavedFormatFromString(const std::string& value)
    {
        if (value == "flac") return AudioClipSavedFormat::Flac;
        if (value == "mp3") return AudioClipSavedFormat::Mp3;
        if (value == "ogg") return AudioClipSavedFormat::Ogg;
        return AudioClipSavedFormat::Wav;
    }

    struct AudioClip
    {
        int id = 0;
        std::string name;
        int trackIndex = 0;          // zero-based track index
        int sequenceNumber = 1;      // one-based sequence number shown in the UI
        AudioClipSourceType sourceType = AudioClipSourceType::Imported;
        AudioClipSavedFormat savedFormat = AudioClipSavedFormat::Wav;
        std::filesystem::path projectRelativePath;
        std::filesystem::path originalSourcePath;
        long long startTick = 0;
        long long durationSamples = 0;
        double sampleRate = 48000.0;
        int channelCount = 2;
        int bitDepth = 24;
        float gain = 1.0f;
        float pan = 0.0f;
        std::uintmax_t sizeBytes = 0;
        std::string notes;
        bool missingMedia = false;
    };
}
