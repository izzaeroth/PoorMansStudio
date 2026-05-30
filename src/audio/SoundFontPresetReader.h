#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace mw::audio
{
    struct SoundFontPreset
    {
        std::string name;
        int bank = 0;
        int program = 0;
    };

    class SoundFontPresetReader
    {
    public:
        static std::vector<SoundFontPreset> readPresets(const std::filesystem::path& sf2Path);
        static std::optional<SoundFontPreset> findPresetByProgram(
            const std::vector<SoundFontPreset>& presets,
            int bank,
            int program
        );
    };
}
