#include "app/AppPaths.h"

#include <juce_core/juce_core.h>

#include <cctype>
#include <string>

namespace mw::app
{
    namespace
    {
        std::string sanitizeProjectFolderName(std::string name)
        {
            if (name.empty())
                name = "Untitled Project";

            for (auto& c : name)
            {
                const bool bad = c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' || c == '|' || c == '?' || c == '*';
                if (bad || static_cast<unsigned char>(c) < 32)
                    c = '_';
            }

            while (!name.empty() && (name.back() == '.' || std::isspace(static_cast<unsigned char>(name.back()))))
                name.pop_back();

            return name.empty() ? "Untitled Project" : name;
        }
    }

    std::filesystem::path AppPaths::detectProjectRoot()
    {
        // Prefer the executable location so the runtime can be moved as a folder.
        //
        // Supported development layout:
        //   PoorMansStudio/
        //     workspace/
        //
        // Supported runtime layout:
        //   PoorMansStudio/
        //     workspace/
        //       program/
        //         Poor Man's Studio.exe
        //
        // Important:
        // If the app is launched from inside "workspace" itself, the project root
        // must be workspace's parent. Otherwise workspaceFolder() would become
        // workspace/workspace.
        const auto exeFile = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
        auto exeFolder = std::filesystem::path(exeFile.getParentDirectory().getFullPathName().toStdString());

        if (exeFolder.filename() == "program" && exeFolder.parent_path().filename() == "workspace")
            return exeFolder.parent_path().parent_path();

        if (exeFolder.filename() == "workspace")
            return exeFolder.parent_path();

        auto probe = exeFolder;

        for (int i = 0; i < 8; ++i)
        {
            if (probe.filename() == "workspace")
                return probe.parent_path();

            if (std::filesystem::exists(probe / "workspace"))
                return probe;

            if (!probe.has_parent_path())
                break;

            probe = probe.parent_path();
        }

        // Fallback to current working directory with the same workspace guard.
        auto cwd = std::filesystem::current_path();

        if (cwd.filename() == "workspace")
            return cwd.parent_path();

        if (cwd.filename() == "program" && cwd.parent_path().filename() == "workspace")
            return cwd.parent_path().parent_path();

        if (cwd.filename() == "builds" || cwd.filename() == "Debug" || cwd.filename() == "Release")
        {
            if (cwd.filename() == "Debug" || cwd.filename() == "Release")
                cwd = cwd.parent_path();

            if (cwd.filename() == "builds")
                return cwd.parent_path();
        }

        return cwd;
    }

    std::filesystem::path AppPaths::workspaceFolder()
    {
        return detectProjectRoot() / "workspace";
    }

    std::filesystem::path AppPaths::exportsFolder()
    {
        return workspaceFolder() / "exports";
    }

    std::filesystem::path AppPaths::inputFolder()
    {
        return workspaceFolder() / "input";
    }

    std::filesystem::path AppPaths::soundFontsFolder()
    {
        return workspaceFolder() / "soundfonts";
    }

    std::filesystem::path AppPaths::fluidSynthFolder()
    {
        return workspaceFolder() / "fluidsynth";
    }

    std::filesystem::path AppPaths::ffmpegFolder()
    {
        return workspaceFolder() / "ffmpeg";
    }

    std::filesystem::path AppPaths::sfzFolder()
    {
        return workspaceFolder() / "sfz";
    }

    std::filesystem::path AppPaths::sfizzFolder()
    {
        return workspaceFolder() / "sfizz";
    }

    std::filesystem::path AppPaths::vst3Folder()
    {
        return workspaceFolder() / "vst3";
    }

    std::filesystem::path AppPaths::vstHostFolder()
    {
        return workspaceFolder() / "vst_host";
    }

    std::filesystem::path AppPaths::projectsFolder()
    {
        return workspaceFolder() / "projects";
    }

    std::filesystem::path AppPaths::projectFolderForName(const std::string& projectName)
    {
        return projectsFolder() / sanitizeProjectFolderName(projectName);
    }

    std::filesystem::path AppPaths::projectFileForName(const std::string& projectName)
    {
        const auto folder = projectFolderForName(projectName);
        return folder / (folder.filename().string() + ".mwproj");
    }


    std::filesystem::path AppPaths::tempFolder()
    {
        return workspaceFolder() / "temp";
    }

    std::filesystem::path AppPaths::previewFolder()
    {
        return tempFolder() / "previews";
    }

    std::filesystem::path AppPaths::settingsFolder()
    {
        return workspaceFolder() / "settings";
    }

    std::filesystem::path AppPaths::themesFolder()
    {
        return workspaceFolder() / "themes";
    }

    std::filesystem::path AppPaths::defaultFluidSynthExePath()
    {
        return fluidSynthFolder() / "fluidsynth.exe";
    }

    std::filesystem::path AppPaths::defaultFfmpegExePath()
    {
        return ffmpegFolder() / "ffmpeg.exe";
    }

    std::filesystem::path AppPaths::defaultSfizzRenderExePath()
    {
        const auto folder = sfizzFolder();

        const std::filesystem::path candidates[]
        {
            folder / "bin" / "Release" / "sfizz_render.exe",
            folder / "bin" / "Release" / "sfizz-render.exe",
            folder / "bin" / "sfizz_render.exe",
            folder / "bin" / "sfizz-render.exe",
            folder / "sfizz_render.exe",
            folder / "sfizz-render.exe"
        };

        for (const auto& candidate : candidates)
        {
            if (std::filesystem::exists(candidate))
                return candidate;
        }

        // Prefer the user's confirmed build layout as the default even if it
        // does not exist yet, so the GUI shows the expected target location.
        return folder / "bin" / "Release" / "sfizz_render.exe";
    }

    std::filesystem::path AppPaths::preferencesFilePath()
    {
        return settingsFolder() / "user_preferences.txt";
    }

    std::filesystem::path AppPaths::findFirstSoundFont()
    {
        const auto folder = soundFontsFolder();

        if (!std::filesystem::exists(folder))
            return {};

        for (const auto& entry : std::filesystem::directory_iterator(folder))
        {
            if (!entry.is_regular_file())
                continue;

            auto ext = entry.path().extension().string();

            for (auto& c : ext)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

            if (ext == ".sf2" || ext == ".sf3")
                return entry.path();
        }

        return {};
    }
}
