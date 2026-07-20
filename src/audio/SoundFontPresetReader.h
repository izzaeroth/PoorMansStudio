#pragma once

#include <filesystem>
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
    };
}
