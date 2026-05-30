#pragma once
#include <filesystem>
#include <optional>

#include "core/Project.h"

namespace mw::import_export
{
    class MusicXmlImporter
    {
    public:
        static std::optional<mw::core::Project> importFromFile(const std::filesystem::path& filePath);
    };
}
