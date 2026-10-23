#pragma once
#include <filesystem>
#include "core/Project.h"
namespace mw::midi
{
    enum class MidiExportPurpose
    {
        LegacyNotesOnly,
        InternalRender,
        UserMidiExport
    };

    struct MidiExportOptions
    {
        MidiExportPurpose purpose = MidiExportPurpose::LegacyNotesOnly;
    };

    class MidiExporter
    {
    public:
        static bool exportToFile(const mw::core::Project& project, const std::filesystem::path& filePath, MidiExportOptions options = {});
    };
}
