#include "import_export/MidiImporter.h"

#include "core/InstrumentNameMatcher.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <array>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <tuple>
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

    struct PmsSetupControllerMarker
    {
        std::int64_t tick = 0;
        int midiChannel = 1;
        int controller = 0;
        int value = 0;
    };

    std::optional<PmsSetupControllerMarker> parsePmsSetupControllerMarker(
        const std::vector<std::uint8_t>& data, std::size_t pos, std::size_t length, std::size_t end, std::int64_t tick)
    {
        if (length != 9 || pos + length > end)
            return std::nullopt;

        if (data[pos] != static_cast<std::uint8_t>('P')
            || data[pos + 1] != static_cast<std::uint8_t>('M')
            || data[pos + 2] != static_cast<std::uint8_t>('S')
            || data[pos + 3] != static_cast<std::uint8_t>('C')
            || data[pos + 4] != static_cast<std::uint8_t>('C')
            || data[pos + 5] != 0x01U)
            return std::nullopt;

        const int channel = data[pos + 6];
        const int controller = data[pos + 7];
        const int value = data[pos + 8];
        if (channel < 1 || channel > 16 || controller > 127 || value > 127)
            return std::nullopt;

        return PmsSetupControllerMarker { tick, channel, controller, value };
    }

    struct PmsSetupProgramMarker
    {
        std::int64_t tick = 0;
        int midiChannel = 1;
        int program = 0;
    };

    std::optional<PmsSetupProgramMarker> parsePmsSetupProgramMarker(
        const std::vector<std::uint8_t>& data, std::size_t pos, std::size_t length, std::size_t end, std::int64_t tick)
    {
        if (length != 8 || pos + length > end)
            return std::nullopt;
        if (data[pos] != static_cast<std::uint8_t>('P')
            || data[pos + 1] != static_cast<std::uint8_t>('M')
            || data[pos + 2] != static_cast<std::uint8_t>('S')
            || data[pos + 3] != static_cast<std::uint8_t>('P')
            || data[pos + 4] != static_cast<std::uint8_t>('C')
            || data[pos + 5] != 0x01U)
            return std::nullopt;
        const int channel = data[pos + 6];
        const int program = data[pos + 7];
        if (channel < 1 || channel > 16 || program > 127)
            return std::nullopt;
        return PmsSetupProgramMarker { tick, channel, program };
    }

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

    struct RawCcEvent
    {
        std::int64_t tick = 0;
        int midiChannel = 1;
        int controller = 0;
        int value = 0;
        std::uint64_t order = 0;
        bool consumed = false;
        bool block14BitPairing = false;
    };

    struct ParameterDecodeState
    {
        enum class Kind { None, Rpn, Nrpn };
        Kind kind = Kind::None;
        int selectorMsb = -1;
        int selectorLsb = -1;
        std::size_t selectorMsbIndex = static_cast<std::size_t>(-1);
        std::size_t selectorLsbIndex = static_cast<std::size_t>(-1);
        int dataMsb = -1;
        std::size_t dataMsbIndex = static_cast<std::size_t>(-1);
        bool hadSemanticRpn = false;
    };

    void reconstructAdvancedControllers(
        std::vector<RawCcEvent>& raw,
        std::vector<mw::core::MidiControllerEvent>& controllerEvents,
        std::vector<mw::core::Midi14BitControllerEvent>& controller14BitEvents,
        std::vector<mw::core::MidiRpnEvent>& rpnEvents,
        std::vector<mw::core::MidiNrpnEvent>& nrpnEvents)
    {
        std::array<ParameterDecodeState, 16> states {};
        std::map<std::tuple<std::int64_t, int, int>, std::size_t> rpnIndex;
        std::map<std::tuple<std::int64_t, int, int>, std::size_t> nrpnIndex;

        for (std::size_t i = 0; i < raw.size(); ++i)
        {
            auto& event = raw[i];
            if (event.midiChannel < 1 || event.midiChannel > 16) continue;
            auto& state = states[static_cast<std::size_t>(event.midiChannel - 1)];
            const auto resetData = [&]() { state.dataMsb = -1; state.dataMsbIndex = static_cast<std::size_t>(-1); };

            if (event.controller == 101 || event.controller == 100)
            {
                if (state.kind != ParameterDecodeState::Kind::Rpn)
                {
                    state.kind = ParameterDecodeState::Kind::Rpn;
                    state.selectorMsb = state.selectorLsb = -1;
                    state.selectorMsbIndex = state.selectorLsbIndex = static_cast<std::size_t>(-1);
                    resetData();
                }
                if (event.controller == 101) { state.selectorMsb = event.value; state.selectorMsbIndex = i; }
                else { state.selectorLsb = event.value; state.selectorLsbIndex = i; }

                if (state.selectorMsb == 127 && state.selectorLsb == 127 && state.hadSemanticRpn)
                {
                    raw[state.selectorMsbIndex].consumed = true;
                    raw[state.selectorLsbIndex].consumed = true;
                }
                continue;
            }
            if (event.controller == 99 || event.controller == 98)
            {
                if (state.kind != ParameterDecodeState::Kind::Nrpn)
                {
                    state.kind = ParameterDecodeState::Kind::Nrpn;
                    state.selectorMsb = state.selectorLsb = -1;
                    state.selectorMsbIndex = state.selectorLsbIndex = static_cast<std::size_t>(-1);
                    resetData();
                }
                if (event.controller == 99) { state.selectorMsb = event.value; state.selectorMsbIndex = i; }
                else { state.selectorLsb = event.value; state.selectorLsbIndex = i; }
                continue;
            }
            if (event.controller == 6)
            {
                state.dataMsb = event.value; state.dataMsbIndex = i;
                continue;
            }
            if (event.controller != 38) continue;

            if (state.dataMsb < 0 || state.dataMsbIndex == static_cast<std::size_t>(-1)
                || state.selectorMsb < 0 || state.selectorLsb < 0
                || state.selectorMsbIndex == static_cast<std::size_t>(-1)
                || state.selectorLsbIndex == static_cast<std::size_t>(-1))
                continue;

            const int parameter = mw::core::combineMidi14Bit(state.selectorMsb, state.selectorLsb);
            const int value = mw::core::combineMidi14Bit(state.dataMsb, event.value);
            const auto semanticTick = event.tick;

            if (state.kind == ParameterDecodeState::Kind::Rpn && parameter == mw::core::midiRpnNullParameter)
            {
                // 127/127 is the RPN Null Function. Keep the full transaction as raw
                // CC and prevent its Data Entry pair from being reinterpreted as CC6/38.
                raw[state.selectorMsbIndex].block14BitPairing = true;
                raw[state.selectorLsbIndex].block14BitPairing = true;
                raw[state.dataMsbIndex].block14BitPairing = true;
                event.block14BitPairing = true;
                resetData();
                continue;
            }

            if (state.kind == ParameterDecodeState::Kind::Rpn)
            {
                const auto key = std::make_tuple(semanticTick, event.midiChannel, parameter);
                const auto existing = rpnIndex.find(key);
                if (existing == rpnIndex.end())
                {
                    rpnIndex.emplace(key, rpnEvents.size());
                    rpnEvents.emplace_back(semanticTick, event.midiChannel, parameter, value);
                }
                else rpnEvents[existing->second].value = value;
                state.hadSemanticRpn = true;
            }
            else if (state.kind == ParameterDecodeState::Kind::Nrpn)
            {
                const auto key = std::make_tuple(semanticTick, event.midiChannel, parameter);
                const auto existing = nrpnIndex.find(key);
                if (existing == nrpnIndex.end())
                {
                    nrpnIndex.emplace(key, nrpnEvents.size());
                    nrpnEvents.emplace_back(semanticTick, event.midiChannel, parameter, value);
                }
                else nrpnEvents[existing->second].value = value;
            }
            else continue;

            raw[state.selectorMsbIndex].consumed = true;
            raw[state.selectorLsbIndex].consumed = true;
            raw[state.dataMsbIndex].consumed = true;
            event.consumed = true;
            resetData();
        }

        // Reconstruct ordinary paired 14-bit CC only when both halves exist at the
        // same effective tick/channel. CC0/32 is intentionally excluded.
        using PairKey = std::tuple<std::int64_t, int, int>;
        struct PairState { std::vector<std::size_t> msbIndices; std::vector<std::size_t> lsbIndices; };
        std::map<PairKey, PairState> pairs;
        for (std::size_t i = 0; i < raw.size(); ++i)
        {
            const auto& event = raw[i];
            if (event.consumed || event.block14BitPairing) continue;
            if (event.controller >= 1 && event.controller <= 31)
                pairs[PairKey{event.tick, event.midiChannel, event.controller}].msbIndices.push_back(i);
            else if (event.controller >= 33 && event.controller <= 63)
                pairs[PairKey{event.tick, event.midiChannel, event.controller - 32}].lsbIndices.push_back(i);
        }
        for (auto& [key, pair] : pairs)
        {
            if (pair.msbIndices.empty() || pair.lsbIndices.empty()) continue;
            const auto mi = pair.msbIndices.back(); const auto li = pair.lsbIndices.back();
            const auto& m = raw[mi]; const auto& l = raw[li];
            controller14BitEvents.emplace_back(m.tick, m.midiChannel, m.controller, mw::core::combineMidi14Bit(m.value, l.value));
            for (auto idx : pair.msbIndices) raw[idx].consumed = true;
            for (auto idx : pair.lsbIndices) raw[idx].consumed = true;
        }

        // Everything not proven semantic remains ordinary CC with the established
        // same-lane/same-tick last-source-value rule.
        std::map<std::tuple<std::int64_t, int, int>, std::size_t> controllerIndex;
        for (const auto& event : raw)
        {
            if (event.consumed) continue;
            const auto key = std::make_tuple(event.tick, event.midiChannel, event.controller);
            const auto existing = controllerIndex.find(key);
            if (existing == controllerIndex.end())
            {
                controllerIndex.emplace(key, controllerEvents.size());
                controllerEvents.emplace_back(event.tick, event.midiChannel, event.controller, event.value);
            }
            else controllerEvents[existing->second].value = event.value;
        }
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
        (void) format;
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
            std::array<int, 16> channelProgramBanks;
            std::array<int, 16> initialChannelPrograms;
            std::array<int, 16> initialChannelProgramBanks;
            std::array<int, 16> channelBankMsb;
            std::array<int, 16> channelBankLsb;
            channelPrograms.fill(-1);
            channelProgramBanks.fill(0);
            initialChannelPrograms.fill(-1);
            initialChannelProgramBanks.fill(0);
            channelBankMsb.fill(0);
            channelBankLsb.fill(0);
            std::map<int, ActiveNote> activeNotes;
            std::vector<mw::core::NoteEvent> notes;
            std::vector<mw::core::MidiControllerEvent> controllerEvents;
            std::vector<RawCcEvent> rawControllerEvents;
            std::uint64_t rawControllerOrder = 0;
            std::vector<mw::core::Midi14BitControllerEvent> controller14BitEvents;
            std::vector<mw::core::MidiRpnEvent> rpnEvents;
            std::vector<mw::core::MidiNrpnEvent> nrpnEvents;
            std::vector<mw::core::MidiPitchBendEvent> pitchBendEvents;
            std::map<std::pair<std::int64_t, int>, std::size_t> pitchBendEventIndexByChannelTick;
            std::vector<mw::core::MidiProgramChangeEvent> programChangeEvents;
            std::map<std::pair<std::int64_t, int>, std::size_t> programChangeEventIndexByChannelTick;
            std::vector<mw::core::MidiChannelPressureEvent> channelPressureEvents;
            std::map<std::pair<std::int64_t, int>, std::size_t> channelPressureEventIndexByChannelTick;
            std::vector<mw::core::MidiPolyPressureEvent> polyPressureEvents;
            std::map<std::tuple<std::int64_t, int, int>, std::size_t> polyPressureEventIndexByChannelKeyTick;
            std::optional<PmsSetupControllerMarker> pendingPmsSetupControllerMarker;
            std::optional<PmsSetupProgramMarker> pendingPmsSetupProgramMarker;

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

                    if (metaType == 0x7F)
                    {
                        pendingPmsSetupControllerMarker = parsePmsSetupControllerMarker(data, tpos, length, trackEnd, currentTick);
                        pendingPmsSetupProgramMarker = parsePmsSetupProgramMarker(data, tpos, length, trackEnd, currentTick);
                    }
                    else
                    {
                        // PMS setup markers are one-shot and must be immediately followed
                        // by their exact matching MIDI event. Any intervening meta event
                        // invalidates the marker so legitimate external data is never suppressed.
                        pendingPmsSetupControllerMarker.reset();
                        pendingPmsSetupProgramMarker.reset();
                    }

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
                    pendingPmsSetupControllerMarker.reset();
                    pendingPmsSetupProgramMarker.reset();
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
                    pendingPmsSetupControllerMarker.reset();
                    pendingPmsSetupProgramMarker.reset();
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
                    pendingPmsSetupControllerMarker.reset();
                    if (tpos >= trackEnd)
                        break;
                    currentProgram = data[tpos++];
                    if (currentProgram > 0x7F)
                        break;
                    currentBank = (channelBankMsb[channel] << 7) | channelBankLsb[channel];
                    channelPrograms[channel] = currentProgram;
                    channelProgramBanks[channel] = currentBank;
                    if (initialChannelPrograms[channel] < 0)
                    {
                        initialChannelPrograms[channel] = currentProgram;
                        initialChannelProgramBanks[channel] = currentBank;
                    }

                    const bool generatedSetupProgram = pendingPmsSetupProgramMarker.has_value()
                        && pendingPmsSetupProgramMarker->tick == currentTick
                        && pendingPmsSetupProgramMarker->midiChannel == channel + 1
                        && pendingPmsSetupProgramMarker->program == currentProgram;
                    pendingPmsSetupProgramMarker.reset();
                    if (!generatedSetupProgram)
                    {
                        const auto laneTick = std::make_pair(currentTick, channel + 1);
                        const auto existing = programChangeEventIndexByChannelTick.find(laneTick);
                        if (existing != programChangeEventIndexByChannelTick.end())
                            programChangeEvents[existing->second].program = currentProgram;
                        else
                        {
                            programChangeEventIndexByChannelTick.emplace(laneTick, programChangeEvents.size());
                            programChangeEvents.emplace_back(currentTick, channel + 1, currentProgram);
                        }
                    }
                }
                else if (eventType == 0xD0)
                {
                    pendingPmsSetupControllerMarker.reset();
                    pendingPmsSetupProgramMarker.reset();
                    // Channel Pressure is a one-data-byte MIDI message. Reject a
                    // truncated or non-7-bit value rather than manufacturing state.
                    if (tpos >= trackEnd)
                        break;
                    const int pressure = data[tpos++];
                    if (pressure > 0x7F)
                        break;
                    const auto laneTick = std::make_pair(currentTick, channel + 1);
                    const auto existing = channelPressureEventIndexByChannelTick.find(laneTick);
                    if (existing != channelPressureEventIndexByChannelTick.end())
                        channelPressureEvents[existing->second].pressure = pressure;
                    else
                    {
                        channelPressureEventIndexByChannelTick.emplace(laneTick, channelPressureEvents.size());
                        channelPressureEvents.emplace_back(currentTick, channel + 1, pressure);
                    }
                }
                else if (eventType == 0xA0)
                {
                    pendingPmsSetupControllerMarker.reset();
                    pendingPmsSetupProgramMarker.reset();
                    if (tpos + 2 > trackEnd)
                        break;
                    const int noteNumber = data[tpos++];
                    const int pressure = data[tpos++];
                    if (noteNumber > 0x7F || pressure > 0x7F)
                        break;
                    const auto laneTick = std::make_tuple(currentTick, channel + 1, noteNumber);
                    const auto existing = polyPressureEventIndexByChannelKeyTick.find(laneTick);
                    if (existing != polyPressureEventIndexByChannelKeyTick.end())
                        polyPressureEvents[existing->second].pressure = pressure;
                    else
                    {
                        polyPressureEventIndexByChannelKeyTick.emplace(laneTick, polyPressureEvents.size());
                        polyPressureEvents.emplace_back(currentTick, channel + 1, noteNumber, pressure);
                    }
                }
                else if (eventType == 0xB0)
                {
                    pendingPmsSetupProgramMarker.reset();
                    // Control Change requires two complete 7-bit data bytes. Do not
                    // manufacture project automation from a truncated/malformed event.
                    if (tpos + 2 > trackEnd)
                        break;

                    const int controller = data[tpos++];
                    const int value = data[tpos++];
                    if (controller > 0x7F || value > 0x7F)
                        break;

                    if (controller == 0)
                        channelBankMsb[channel] = value;
                    else if (controller == 32)
                        channelBankLsb[channel] = value;

                    const bool generatedSetupController = pendingPmsSetupControllerMarker.has_value()
                        && pendingPmsSetupControllerMarker->tick == currentTick
                        && pendingPmsSetupControllerMarker->midiChannel == channel + 1
                        && pendingPmsSetupControllerMarker->controller == controller
                        && pendingPmsSetupControllerMarker->value == value;
                    pendingPmsSetupControllerMarker.reset();
                    if (generatedSetupController)
                        continue;

                    rawControllerEvents.push_back({currentTick, channel + 1, controller, value, ++rawControllerOrder, false, false});
                }
                else if (eventType == 0xE0)
                {
                    pendingPmsSetupControllerMarker.reset();
                    pendingPmsSetupProgramMarker.reset();
                    // MIDI Pitch Bend is a complete 14-bit value encoded LSB then MSB.
                    // Reject a truncated message rather than manufacturing a center/default event.
                    if (tpos + 2 > trackEnd)
                        break;
                    const int lsb = data[tpos++];
                    const int msb = data[tpos++];
                    if (lsb > 0x7F || msb > 0x7F)
                        break;
                    const int signedValue = (lsb | (msb << 7)) - 8192;
                    const auto laneTick = std::make_pair(currentTick, channel + 1);
                    const auto existing = pitchBendEventIndexByChannelTick.find(laneTick);
                    if (existing != pitchBendEventIndexByChannelTick.end())
                    {
                        pitchBendEvents[existing->second].value = signedValue;
                    }
                    else
                    {
                        pitchBendEventIndexByChannelTick.emplace(laneTick, pitchBendEvents.size());
                        pitchBendEvents.emplace_back(currentTick, channel + 1, signedValue);
                    }
                }
                else
                {
                    pendingPmsSetupControllerMarker.reset();
                    pendingPmsSetupProgramMarker.reset();
                    readByte();
                    readByte();
                }
            }

            reconstructAdvancedControllers(rawControllerEvents, controllerEvents, controller14BitEvents, rpnEvents, nrpnEvents);

            if (!notes.empty() || !controllerEvents.empty() || !controller14BitEvents.empty() || !rpnEvents.empty() || !nrpnEvents.empty()
                || !pitchBendEvents.empty() || !programChangeEvents.empty() || !channelPressureEvents.empty() || !polyPressureEvents.empty())
            {
                const int effectiveChannel = firstNoteChannel >= 0 ? firstNoteChannel : primaryChannel;
                int effectiveProgram = -1;
                int effectiveBank = 0;

                if (effectiveChannel >= 0 && effectiveChannel < 16)
                {
                    if (initialChannelPrograms[effectiveChannel] >= 0)
                    {
                        effectiveProgram = initialChannelPrograms[effectiveChannel];
                        effectiveBank = initialChannelProgramBanks[effectiveChannel];
                    }
                    else
                    {
                        effectiveBank = (channelBankMsb[effectiveChannel] << 7) | channelBankLsb[effectiveChannel];
                    }
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

                track.setMidiControllerEvents(std::move(controllerEvents));
                track.setMidiPitchBendEvents(std::move(pitchBendEvents));
                track.setMidiProgramChangeEvents(std::move(programChangeEvents));
                track.setMidiChannelPressureEvents(std::move(channelPressureEvents));
                track.setMidiPolyPressureEvents(std::move(polyPressureEvents));
                track.setMidi14BitControllerEvents(std::move(controller14BitEvents));
                track.setMidiRpnEvents(std::move(rpnEvents));
                track.setMidiNrpnEvents(std::move(nrpnEvents));
            }

            pos = trackEnd;
        }

        if (project.getTracks().empty())
            return std::nullopt;

        return project;
    }
}
