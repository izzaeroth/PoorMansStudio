#pragma once

#include <filesystem>
#include <string>

namespace mw::app
{
    class AppPaths
    {
    public:
        static std::filesystem::path detectProjectRoot();

        static std::filesystem::path workspaceFolder();
        static std::filesystem::path exportsFolder();
        static std::filesystem::path inputFolder();
        static std::filesystem::path soundFontsFolder();
        static std::filesystem::path fluidSynthFolder();
        static std::filesystem::path ffmpegFolder();
        static std::filesystem::path sfzFolder();
        static std::filesystem::path vst3Folder();
        static std::filesystem::path vstHostFolder();
        static std::filesystem::path sfizzFolder();
        static std::filesystem::path projectsFolder();
        static std::filesystem::path projectFolderForName(const std::string& projectName);
        static std::filesystem::path projectFileForName(const std::string& projectName);
        static std::filesystem::path tempFolder();
        static std::filesystem::path previewFolder();
        static std::filesystem::path settingsFolder();
        static std::filesystem::path themesFolder();

        static std::filesystem::path defaultFluidSynthExePath();
        static std::filesystem::path defaultFfmpegExePath();
        static std::filesystem::path defaultSfizzRenderExePath();
        static std::filesystem::path findFirstSoundFont();
        static std::filesystem::path preferencesFilePath();
    };
}
