#include "import_export/MidiImporter.h"

#include "core/InstrumentNameMatcher.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <array>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    struct MidiData
    {
        std::vector<std::uint8_t> bytes;
        std::size_t pos = 0;
    };

    struct ActiveNote
    {
        int pitch = 60;
        int velocity = 100;
        std::int64_t startTick = 0;
    };

    std::vector<std::uint8_t> readFile(const std::filesystem::path& path)
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

    std::uint16_t readBE16(const std::vector<std::uint8_t>& data, std::size_t pos)
    {
        if (pos + 2 > data.size()) return 0;
        return static_cast<std::uint16_t>((data[pos] << 8) | data[pos + 1]);
    }

    std::uint32_t readBE32(const std::vector<std::uint8_t>& data, std::size_t pos)
    {
        if (pos + 4 > data.size()) return 0;
        return (static_cast<std::uint32_t>(data[pos]) << 24)
             | (static_cast<std::uint32_t>(data[pos + 1]) << 16)
             | (static_cast<std::uint32_t>(data[pos + 2]) << 8)
             | static_cast<std::uint32_t>(data[pos + 3]);
    }

    std::uint32_t readVariableLength(const std::vector<std::uint8_t>& data, std::size_t& pos, std::size_t end)
    {
        std::uint32_t value = 0;

        for (int i = 0; i < 4 && pos < end; ++i)
        {
            const auto byte = data[pos++];
            value = (value << 7) | (byte & 0x7F);

            if ((byte & 0x80) == 0)
                break;
        }

        return value;
    }

    std::string makeTrackName(int trackNumber, int channel, int program)
    {
        std::ostringstream name;
        name << "MIDI Track " << trackNumber;

        if (channel >= 0)
            name << " Ch " << (channel + 1);

        if (program >= 0)
            name << " Program " << program;

        return name.str();
    }

    std::string guessInstrumentNameFromProgram(int program)
    {
        if (program >= 0 && program <= 7) return "Piano";
        if (program >= 24 && program <= 31) return "Guitar";
        if (program >= 32 && program <= 39) return "Bass";
        if (program == 40) return "Violin";
        if (program == 41) return "Viola";
        if (program == 42) return "Cello";
        if (program == 43) return "Contrabass";
        if (program >= 48 && program <= 51) return "Strings";
        if (program >= 56 && program <= 63) return "Brass";
        if (program >= 64 && program <= 71) return "Sax/Woodwind";
        if (program >= 72 && program <= 79) return "Flute/Woodwind";
        if (program >= 80 && program <= 103) return "Synth";
        return "MIDI Instrument";
    }

    bool textLooksLikePercussion(const std::string& text)
    {
        std::string lowered = text;
        for (auto& c : lowered)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        return lowered.find("drum") != std::string::npos
            || lowered.find("percussion") != std::string::npos
            || lowered.find("kit") != std::string::npos;
    }

    std::string readMetaText(const std::vector<std::uint8_t>& data, std::size_t pos, std::size_t length, std::size_t end)
    {
        std::string text;

        for (std::size_t i = 0; i < length && pos + i < end; ++i)
        {
            const char c = static_cast<char>(data[pos + i]);

            if (c != '\0')
                text.push_back(c);
        }

        return text;
    }
}

namespace mw::import_export
{
    std::optional<mw::core::Project> MidiImporter::importFromFile(const std::filesystem::path& filePath)
    {
        const auto data = readFile(filePath);

        if (data.size() < 14)
            return std::nullopt;

        if (std::string(reinterpret_cast<const char*>(&data[0]), reinterpret_cast<const char*>(&data[4])) != "MThd")
            return std::nullopt;

        const auto headerSize = readBE32(data, 4);
        const auto format = readBE16(data, 8);
        const auto trackCount = readBE16(data, 10);
        const auto division = readBE16(data, 12);

        if (headerSize < 6 || trackCount == 0 || division == 0 || (division & 0x8000))
            return std::nullopt;

        const double tickScale =
            static_cast<double>(mw::core::Project::ticksPerQuarterNote) /
            static_cast<double>(division);

        mw::core::Project project(filePath.stem().string());

        std::size_t pos = 8 + headerSize;

        for (int trackIndex = 0; trackIndex < static_cast<int>(trackCount) && pos + 8 <= data.size(); ++trackIndex)
        {
            if (std::string(reinterpret_cast<const char*>(&data[pos]), reinterpret_cast<const char*>(&data[pos + 4])) != "MTrk")
                break;

            const auto trackSize = readBE32(data, pos + 4);
            const auto trackStart = pos + 8;
            const auto trackEnd = std::min<std::size_t>(trackStart + trackSize, data.size());

            std::int64_t currentTick = 0;
            std::uint8_t runningStatus = 0;
            int currentProgram = -1;
            int currentBank = 0;
            int primaryChannel = -1;
            int firstNoteChannel = -1;
            std::string trackName;
            std::string instrumentName;
            std::array<int, 16> channelPrograms;
            std::array<int, 16> channelBankMsb;
            std::array<int, 16> channelBankLsb;
            channelPrograms.fill(-1);
            channelBankMsb.fill(0);
            channelBankLsb.fill(0);
            std::map<int, ActiveNote> activeNotes;
            std::vector<mw::core::NoteEvent> notes;

            std::size_t tpos = trackStart;

            while (tpos < trackEnd)
            {
                const auto delta = readVariableLength(data, tpos, trackEnd);
                currentTick += static_cast<std::int64_t>(static_cast<double>(delta) * tickScale);

                if (tpos >= trackEnd)
                    break;

                std::uint8_t status = data[tpos++];

                if (status < 0x80)
                {
                    if (runningStatus == 0)
                        break;

                    --tpos;
                    status = runningStatus;
                }
                else if (status < 0xF0)
                {
                    runningStatus = status;
                }

                if (status == 0xFF)
                {
                    if (tpos >= trackEnd) break;

                    const auto metaType = data[tpos++];
                    const auto length = readVariableLength(data, tpos, trackEnd);

                    if (metaType == 0x03)
                    {
                        trackName = readMetaText(data, tpos, length, trackEnd);
                    }
                    else if (metaType == 0x04)
                    {
                        instrumentName = readMetaText(data, tpos, length, trackEnd);
                    }
                    else if (metaType == 0x51 && length == 3 && tpos + 3 <= trackEnd)
                    {
                        const auto microsecondsPerQuarter =
                            (static_cast<int>(data[tpos]) << 16)
                            | (static_cast<int>(data[tpos + 1]) << 8)
                            | static_cast<int>(data[tpos + 2]);

                        if (microsecondsPerQuarter > 0)
                            project.setTempoBpm(60000000 / microsecondsPerQuarter);
                    }
                    else if (metaType == 0x58 && length >= 2 && tpos + 2 <= trackEnd)
                    {
                        const int numerator = data[tpos];
                        const int denominator = 1 << data[tpos + 1];

                        if (numerator > 0 && denominator > 0)
                            project.setTimeSignature({numerator, denominator});
                    }

                    tpos += length;
                    continue;
                }

                if (status == 0xF0 || status == 0xF7)
                {
                    const auto length = readVariableLength(data, tpos, trackEnd);
                    tpos += length;
                    continue;
                }

                const int eventType = status & 0xF0;
                const int channel = status & 0x0F;

                if (primaryChannel < 0)
                    primaryChannel = channel;

                auto readByte = [&]() -> int
                {
                    if (tpos >= trackEnd) return 0;
                    return data[tpos++];
                };

                if (eventType == 0x80 || eventType == 0x90)
                {
                    const int pitch = readByte();
                    const int velocity = readByte();
                    const int key = channel * 128 + pitch;

                    if (eventType == 0x90 && velocity > 0)
                    {
                        if (firstNoteChannel < 0)
                            firstNoteChannel = channel;

                        activeNotes[key] = {pitch, velocity, currentTick};
                    }
                    else
                    {
                        const auto activeIt = activeNotes.find(key);

                        if (activeIt != activeNotes.end())
                        {
                            const auto& active = activeIt->second;
                            const auto duration = std::max<std::int64_t>(1, currentTick - active.startTick);

                            notes.emplace_back(
                                active.pitch,
                                active.velocity,
                                active.startTick,
                                duration,
                                channel + 1,
                                mw::core::Articulation::Normal
                            );

                            activeNotes.erase(activeIt);
                        }
                    }
                }
                else if (eventType == 0xC0)
                {
                    currentProgram = readByte();
                    currentBank = (channelBankMsb[channel] << 7) | channelBankLsb[channel];
                    channelPrograms[channel] = currentProgram;
                }
                else if (eventType == 0xD0)
                {
                    readByte();
                }
                else
                {
                    const int firstData = readByte();
                    const int secondData = readByte();

                    if (eventType == 0xB0)
                    {
                        if (firstData == 0)
                            channelBankMsb[channel] = std::clamp(secondData, 0, 127);
                        else if (firstData == 32)
                            channelBankLsb[channel] = std::clamp(secondData, 0, 127);
                    }
                }
            }

            if (!notes.empty())
            {
                const int effectiveChannel = firstNoteChannel >= 0 ? firstNoteChannel : primaryChannel;
                int effectiveProgram = currentProgram;
                int effectiveBank = currentBank;

                if (effectiveChannel >= 0 && effectiveChannel < 16)
                {
                    if (channelPrograms[effectiveChannel] >= 0)
                        effectiveProgram = channelPrograms[effectiveChannel];

                    effectiveBank = (channelBankMsb[effectiveChannel] << 7) | channelBankLsb[effectiveChannel];
                }

                const bool percussionTrack = effectiveChannel == 9
                    || textLooksLikePercussion(instrumentName)
                    || textLooksLikePercussion(trackName);

                // General MIDI drum parts are conventionally carried on MIDI channel 10.
                // Many SoundFonts expose the matching drum kits on bank 128 even when the
                // imported MIDI file only sends bank 0, so preserve that intent in the
                // assignment instead of letting channel-10 program 0 look like piano.
                if (percussionTrack && effectiveBank == 0)
                    effectiveBank = 128;

                std::string resolvedInstrumentName = instrumentName.empty() ? trackName : instrumentName;

                if (resolvedInstrumentName.empty())
                {
                    resolvedInstrumentName =
                        percussionTrack
                            ? "Standard Drum Kit"
                            : (effectiveProgram >= 0 ? guessInstrumentNameFromProgram(effectiveProgram) : "MIDI Instrument");
                }

                if (trackName.empty())
                    trackName = makeTrackName(trackIndex + 1, effectiveChannel, effectiveProgram);

                auto& track = project.addTrack(trackName);

                auto assignment = mw::core::InstrumentNameMatcher::createDefaultAssignment(resolvedInstrumentName, trackIndex);
                assignment.displayName = resolvedInstrumentName;
                assignment.normalizedName = mw::core::InstrumentNameMatcher::normalizeInstrumentName(resolvedInstrumentName);
                assignment.presetName = resolvedInstrumentName;
                assignment.midiBank = effectiveBank;

                if (effectiveProgram >= 0)
                    assignment.midiProgram = effectiveProgram;

                if (effectiveChannel >= 0)
                    assignment.midiChannel = effectiveChannel + 1;

                if (percussionTrack)
                {
                    assignment.normalizedName = "percussion";
                    assignment.wasAutoMatched = true;
                    assignment.matchConfidence = 0.90f;
                }

                track.setInstrumentAssignment(assignment);

                for (const auto& note : notes)
                    track.addNote(note);
            }

            pos = trackEnd;
        }

        if (project.getTracks().empty())
            return std::nullopt;

        return project;
    }
}
