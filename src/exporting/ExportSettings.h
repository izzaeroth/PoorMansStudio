#pragma once
#include <filesystem>
#include <string>

namespace mw::exporting
{
    enum class AudioFormat { Wav, Flac, Mp3, Ogg };

    struct ExportSettings
    {
        std::filesystem::path outputFolder;
        std::string baseFileName = "render";

        bool exportProjectFile = true;
        bool exportMidi = true;
        bool exportWav = false;
        bool exportFlac = false;
        bool exportMp3 = false;
        bool exportOgg = false;

        int sampleRate = 48000;
        int bitDepth = 24;
        bool overwriteExistingFiles = true;
    };

    class ExportPathBuilder
    {
    public:
        static bool ensureOutputFolderExists(const ExportSettings& s);
        static std::filesystem::path buildProjectPath(const ExportSettings& s);
        static std::filesystem::path buildMidiPath(const ExportSettings& s);
        static std::filesystem::path buildAudioPath(const ExportSettings& s, AudioFormat f);
        static std::string audioFormatToExtension(AudioFormat f);
    };
}
