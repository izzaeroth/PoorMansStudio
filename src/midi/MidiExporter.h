#pragma once
#include <filesystem>

#include "core/Project.h"

namespace mw::midi
{
    class MidiExporter
    {
    public:
        static bool exportToFile(const mw::core::Project& project, const std::filesystem::path& filePath);
    };
}
