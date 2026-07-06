#pragma once

#include "clap/ClapPluginTypes.h"

#include <filesystem>
#include <vector>

namespace mw::clap
{
    struct ClapScanOptions
    {
        std::vector<std::filesystem::path> scanFolders;
        bool includeSystemFolders = true;
        bool includeWorkspaceFolder = true;
    };

    class ClapPluginScanner
    {
    public:
        static std::vector<std::filesystem::path> defaultScanFolders(bool includeWorkspaceFolder = true, bool includeSystemFolders = true);
        static std::vector<ClapPluginDescriptor> scan(const ClapScanOptions& options);
        static std::vector<ClapPluginDescriptor> scanFolders(const std::vector<std::filesystem::path>& folders);
        static ClapPluginDescriptor inspectPluginPath(const std::filesystem::path& pluginPath);
        static bool isOuterClapPlugin(const std::filesystem::path& path);
    };
}
