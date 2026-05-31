#pragma once

#include "vst/VstPluginTypes.h"

#include <filesystem>
#include <vector>

namespace mw::vst
{
    struct VstScanOptions
    {
        std::vector<std::filesystem::path> scanFolders;
        bool includeSystemFolders = true;
        bool includeWorkspaceFolder = true;
    };

    class VstPluginScanner
    {
    public:
        static std::vector<std::filesystem::path> defaultScanFolders(bool includeWorkspaceFolder = true, bool includeSystemFolders = true);
        static std::vector<VstPluginDescriptor> scan(const VstScanOptions& options);
        static std::vector<VstPluginDescriptor> scanFolders(const std::vector<std::filesystem::path>& folders);
        static VstPluginDescriptor inspectBundle(const std::filesystem::path& outerBundlePath);
        static bool isOuterVst3Bundle(const std::filesystem::path& path);
    };
}
