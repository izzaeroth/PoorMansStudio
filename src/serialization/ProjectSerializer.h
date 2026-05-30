#pragma once
#include <filesystem>
#include <optional>

#include "core/Project.h"

namespace mw::serialization
{
    class ProjectSerializer
    {
    public:
        static bool saveToFile(const mw::core::Project& project, const std::filesystem::path& filePath);
        static std::optional<mw::core::Project> loadFromFile(const std::filesystem::path& filePath);
    };
}
