#pragma once

#include <filesystem>
#include <string>

#include "core/InstrumentAssignment.h"

namespace mw::audio
{
    enum class AudioExportFormat
    {
        Wav,
        Flac,
        Mp3,
        Ogg,
        M4a
    };

    inline std::string audioExportFormatToString(AudioExportFormat format)
    {
        switch (format)
        {
            case AudioExportFormat::Wav: return "WAV";
            case AudioExportFormat::Flac: return "FLAC";
            case AudioExportFormat::Mp3: return "MP3";
            case AudioExportFormat::Ogg: return "OGG";
            case AudioExportFormat::M4a: return "M4A";
            default: return "Unknown";
        }
    }

    struct RenderSettings
    {
        mw::core::SampleBackendType preferredBackend = mw::core::SampleBackendType::SF2;

        std::filesystem::path defaultSf2Path;
        std::filesystem::path defaultSfzFolder;
        std::filesystem::path defaultWavSampleFolder;

        AudioExportFormat audioFormat = AudioExportFormat::Wav;

        int sampleRate = 48000;
        int bitDepth = 24;
        int renderWorkerCount = 0; // 0 = Auto

        bool normalizeOutput = true;
        bool applyBasicLimiter = true;
        bool renderMidiAlso = true;

        float masterVolume = 1.0f;
        float defaultReverbAmount = 0.15f;
    };
}
