#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <string>

#include "core/Project.h"

namespace mw::audio
{
    enum class RenderBackend
    {
        SF2,
        SFZ
    };

    enum class RenderOutputFormat
    {
        Wav,
        Flac,
        Mp3,
        Ogg,
        M4a
    };

    struct RenderJob
    {
        mw::core::Project project {"Render Job"};

        std::filesystem::path sourceInputPath;
        std::filesystem::path exportFolder;
        std::filesystem::path projectFolder;
        std::string baseFileName = "render";

        std::string metadataTitle;
        std::string metadataArtist;
        std::string metadataAlbum;
        std::string metadataTrackNumber;
        std::string metadataYear;
        std::filesystem::path albumArtPath;

        RenderBackend backend = RenderBackend::SF2;
        RenderOutputFormat outputFormat = RenderOutputFormat::Wav;

        std::filesystem::path soundFontPath;
        std::filesystem::path fluidSynthPath;
        std::filesystem::path ffmpegPath;
        std::filesystem::path sfzPath;
        std::filesystem::path sfizzRenderPath;

        int sampleRate = 48000;
        int bitDepth = 24;
        int bitrateKbps = 192;
        int channelCount = 2;
        int renderWorkerCount = 0; // 0 = Auto
        int keepStemFilesMask = 3; // 0 = none, 1 = keep WAV stems, 2 = keep MIDI stems, 3 = keep WAV + MIDI stems

        int sfzKeySwitch = 24;
        int sfzCc1 = 100;
        int sfzCc11 = 127;

        float masterVolume = 1.0f;
    };

    struct RenderJobResult
    {
        bool success = false;
        bool cancelled = false;
        std::filesystem::path projectPath;
        std::filesystem::path midiPath;
        std::filesystem::path wavPath;
        std::filesystem::path finalAudioPath;
        std::string message;
    };

    struct RenderJobCallbacks
    {
        std::function<void(const std::string&)> log;
        std::function<void(const std::string&)> status;
    };

    class RenderJobRunner
    {
    public:
        static RenderJobResult run(
            const RenderJob& job,
            std::atomic<bool>& cancelRequested,
            const RenderJobCallbacks& callbacks
        );
    };
}
