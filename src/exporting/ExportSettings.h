#pragma once
#include <filesystem>
#include <string>

namespace mw::exporting
{
    struct ExportSettings
    {
        std::filesystem::path outputFolder;
        std::string baseFileName = "render";

    };

    class ExportPathBuilder
    {
    public:
        static bool ensureOutputFolderExists(const ExportSettings& s);
        static std::filesystem::path buildMidiPath(const ExportSettings& s);
        static std::filesystem::path buildWavPath(const ExportSettings& s);
    };
}
