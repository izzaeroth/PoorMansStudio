#include "exporting/ExportSettings.h"
#include <system_error>

namespace mw::exporting
{
    bool ExportPathBuilder::ensureOutputFolderExists(const ExportSettings& s)
    {
        std::error_code e;
        if (s.outputFolder.empty()) return false;
        if (std::filesystem::exists(s.outputFolder, e))
            return std::filesystem::is_directory(s.outputFolder, e);

        return std::filesystem::create_directories(s.outputFolder, e);
    }

    std::filesystem::path ExportPathBuilder::buildProjectPath(const ExportSettings& s)
    {
        return s.outputFolder / (s.baseFileName + ".mwproj");
    }

    std::filesystem::path ExportPathBuilder::buildMidiPath(const ExportSettings& s)
    {
        return s.outputFolder / (s.baseFileName + ".mid");
    }

    std::filesystem::path ExportPathBuilder::buildAudioPath(const ExportSettings& s, AudioFormat f)
    {
        return s.outputFolder / (s.baseFileName + audioFormatToExtension(f));
    }

    std::string ExportPathBuilder::audioFormatToExtension(AudioFormat f)
    {
        switch (f)
        {
            case AudioFormat::Wav: return ".wav";
            case AudioFormat::Flac: return ".flac";
            case AudioFormat::Mp3: return ".mp3";
            case AudioFormat::Ogg: return ".ogg";
            default: return ".audio";
        }
    }
}
