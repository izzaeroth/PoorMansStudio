#include "audio/SoundFontPresetReader.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace
{
    std::uint32_t readU32(const std::vector<std::uint8_t>& data, std::size_t pos)
    {
        if (pos + 4 > data.size())
            return 0;

        return static_cast<std::uint32_t>(data[pos])
            | (static_cast<std::uint32_t>(data[pos + 1]) << 8)
            | (static_cast<std::uint32_t>(data[pos + 2]) << 16)
            | (static_cast<std::uint32_t>(data[pos + 3]) << 24);
    }

    std::uint16_t readU16(const std::vector<std::uint8_t>& data, std::size_t pos)
    {
        if (pos + 2 > data.size())
            return 0;

        return static_cast<std::uint16_t>(data[pos])
            | static_cast<std::uint16_t>(data[pos + 1] << 8);
    }

    std::string readFixedString(const std::vector<std::uint8_t>& data, std::size_t pos, std::size_t length)
    {
        std::string value;

        for (std::size_t i = 0; i < length && pos + i < data.size(); ++i)
        {
            const char c = static_cast<char>(data[pos + i]);

            if (c == '\0')
                break;

            value.push_back(c);
        }

        while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r' || value.back() == '\n'))
            value.pop_back();

        return value;
    }

    std::vector<std::uint8_t> readBinaryFile(const std::filesystem::path& path)
    {
        std::ifstream file(path, std::ios::binary);

        if (!file)
            return {};

        file.seekg(0, std::ios::end);
        const auto size = file.tellg();
        file.seekg(0, std::ios::beg);

        if (size <= 0)
            return {};

        std::vector<std::uint8_t> data(static_cast<std::size_t>(size));
        file.read(reinterpret_cast<char*>(data.data()), size);

        return data;
    }

    std::vector<mw::audio::SoundFontPreset> parsePhdr(const std::vector<std::uint8_t>& data, std::size_t phdrDataStart, std::size_t phdrSize)
    {
        std::vector<mw::audio::SoundFontPreset> presets;

        constexpr std::size_t presetHeaderSize = 38;

        if (phdrSize < presetHeaderSize)
            return presets;

        const auto count = phdrSize / presetHeaderSize;

        // Last record is usually the terminal EOP record.
        for (std::size_t i = 0; i + 1 < count; ++i)
        {
            const auto offset = phdrDataStart + i * presetHeaderSize;

            mw::audio::SoundFontPreset preset;
            preset.name = readFixedString(data, offset, 20);
            preset.program = static_cast<int>(readU16(data, offset + 20));
            preset.bank = static_cast<int>(readU16(data, offset + 22));

            if (!preset.name.empty() && preset.name != "EOP")
                presets.push_back(preset);
        }

        std::sort(
            presets.begin(),
            presets.end(),
            [](const auto& a, const auto& b)
            {
                if (a.bank != b.bank) return a.bank < b.bank;
                if (a.program != b.program) return a.program < b.program;
                return a.name < b.name;
            }
        );

        return presets;
    }
}

namespace mw::audio
{
    std::vector<SoundFontPreset> SoundFontPresetReader::readPresets(const std::filesystem::path& sf2Path)
    {
        const auto data = readBinaryFile(sf2Path);

        if (data.size() < 12)
            return {};

        // SF2 is RIFF-based. We look for the "phdr" chunk in the pdta list.
        for (std::size_t pos = 12; pos + 8 <= data.size(); )
        {
            const std::string chunkId(
                reinterpret_cast<const char*>(&data[pos]),
                reinterpret_cast<const char*>(&data[pos + 4])
            );

            const auto chunkSize = static_cast<std::size_t>(readU32(data, pos + 4));
            const auto chunkDataStart = pos + 8;

            if (chunkId == "phdr")
                return parsePhdr(data, chunkDataStart, chunkSize);

            auto next = chunkDataStart + chunkSize;

            if (chunkSize % 2 != 0)
                ++next;

            if (next <= pos)
                break;

            pos = next;
        }

        // Fallback: scan byte-by-byte for "phdr" in case the simple chunk walk misses nested LIST chunks.
        for (std::size_t pos = 0; pos + 8 <= data.size(); ++pos)
        {
            if (data[pos] == 'p' && data[pos + 1] == 'h' && data[pos + 2] == 'd' && data[pos + 3] == 'r')
            {
                const auto chunkSize = static_cast<std::size_t>(readU32(data, pos + 4));
                return parsePhdr(data, pos + 8, chunkSize);
            }
        }

        return {};
    }

}
