#pragma once

#include <algorithm>
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
        Ogg = 4,
        M4a = 5
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
            case AudioClipSavedFormat::M4a: return "m4a";
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
            case 5: return AudioClipSavedFormat::M4a;
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
            case AudioClipSavedFormat::M4a: return 5;
            case AudioClipSavedFormat::Wav:
            default: return 1;
        }
    }

    inline AudioClipSavedFormat audioClipSavedFormatFromString(const std::string& value)
    {
        if (value == "flac") return AudioClipSavedFormat::Flac;
        if (value == "mp3") return AudioClipSavedFormat::Mp3;
        if (value == "ogg") return AudioClipSavedFormat::Ogg;
        if (value == "m4a" || value == "aac") return AudioClipSavedFormat::M4a;
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
        // Non-destructive source trim range in source samples.
        // startTick remains timeline placement; trim only describes which
        // portion of the source media should be kept by AudioClip tools.
        long long sourceTrimStartSamples = 0;
        long long sourceTrimEndSamples = 0; // 0 means full source duration until normalized
        double sampleRate = 48000.0;
        int channelCount = 2;
        int bitDepth = 24;
        float gain = 1.0f;
        float pan = 0.0f;
        std::uintmax_t sizeBytes = 0;
        std::string notes;
        bool missingMedia = false;
    };

    inline long long audioClipTrimStartSamples(const AudioClip& clip)
    {
        if (clip.durationSamples <= 0)
            return 0;

        return std::clamp<long long>(clip.sourceTrimStartSamples, 0, clip.durationSamples);
    }

    inline long long audioClipTrimEndSamples(const AudioClip& clip)
    {
        if (clip.durationSamples <= 0)
            return 0;

        const auto requestedEnd = clip.sourceTrimEndSamples > 0
            ? clip.sourceTrimEndSamples
            : clip.durationSamples;

        const auto start = audioClipTrimStartSamples(clip);
        return std::clamp<long long>(requestedEnd, start, clip.durationSamples);
    }

    inline long long audioClipTrimmedDurationSamples(const AudioClip& clip)
    {
        return std::max<long long>(0, audioClipTrimEndSamples(clip) - audioClipTrimStartSamples(clip));
    }

    inline bool audioClipHasActiveTrim(const AudioClip& clip)
    {
        if (clip.durationSamples <= 0)
            return false;

        return audioClipTrimStartSamples(clip) > 0
            || audioClipTrimEndSamples(clip) < clip.durationSamples;
    }

    inline void normalizeAudioClipTrim(AudioClip& clip)
    {
        if (clip.durationSamples <= 0)
        {
            clip.sourceTrimStartSamples = 0;
            clip.sourceTrimEndSamples = 0;
            return;
        }

        clip.sourceTrimStartSamples = audioClipTrimStartSamples(clip);
        clip.sourceTrimEndSamples = audioClipTrimEndSamples(clip);
    }
}

