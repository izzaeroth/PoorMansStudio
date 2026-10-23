#include "midi/MidiExporter.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <vector>

namespace
{
    struct E
    {
        std::int64_t t;
        std::vector<std::uint8_t> b;
        int p;
        int q = 1;
    };

    void be16(std::vector<std::uint8_t>& d, std::uint16_t v)
    {
        d.push_back(static_cast<std::uint8_t>(v >> 8));
        d.push_back(static_cast<std::uint8_t>(v & 255));
    }

    void be32(std::vector<std::uint8_t>& d, std::uint32_t v)
    {
        d.push_back(static_cast<std::uint8_t>(v >> 24));
        d.push_back(static_cast<std::uint8_t>(v >> 16));
        d.push_back(static_cast<std::uint8_t>(v >> 8));
        d.push_back(static_cast<std::uint8_t>(v & 255));
    }

    void vlq(std::vector<std::uint8_t>& d, std::uint32_t v)
    {
        std::uint8_t b[5] {};
        int c = 0;

        b[c++] = static_cast<std::uint8_t>(v & 127);
        v >>= 7;

        while (v)
        {
            b[c++] = static_cast<std::uint8_t>((v & 127) | 128);
            v >>= 7;
        }

        for (int i = c - 1; i >= 0; --i)
            d.push_back(b[i]);
    }

    std::vector<std::uint8_t> txt(std::uint8_t type, const std::string& s)
    {
        std::vector<std::uint8_t> e {255, type};
        vlq(e, static_cast<std::uint32_t>(s.size()));
        for (char c : s) e.push_back(static_cast<std::uint8_t>(c));
        return e;
    }

    std::vector<std::uint8_t> tempo(int bpm)
    {
        const auto m = static_cast<std::uint32_t>(60000000 / (bpm > 0 ? bpm : 120));
        return {255, 81, 3, static_cast<std::uint8_t>(m >> 16), static_cast<std::uint8_t>(m >> 8), static_cast<std::uint8_t>(m)};
    }

    std::vector<std::uint8_t> ts(int n, int d)
    {
        int p = 0;
        while (d > 1) { d /= 2; ++p; }
        return {255, 88, 4, static_cast<std::uint8_t>(n), static_cast<std::uint8_t>(p), 24, 8};
    }

    std::vector<std::uint8_t> end()
    {
        return {255, 47, 0};
    }

    std::vector<std::uint8_t> pmsGeneratedSetupControllerMarker(int ch, int controller, int value)
    {
        ch = std::clamp(ch, 1, 16);
        controller = std::clamp(controller, 0, 127);
        value = std::clamp(value, 0, 127);

        // Sequencer-specific meta event. External MIDI consumers can ignore it;
        // Poor Man's Studio uses the exact PMSCC/v1 payload to distinguish
        // exporter-generated setup CC from real project Controller Lane data.
        return {
            0xFF, 0x7F, 0x09,
            static_cast<std::uint8_t>('P'), static_cast<std::uint8_t>('M'),
            static_cast<std::uint8_t>('S'), static_cast<std::uint8_t>('C'),
            static_cast<std::uint8_t>('C'), 0x01,
            static_cast<std::uint8_t>(ch),
            static_cast<std::uint8_t>(controller),
            static_cast<std::uint8_t>(value)
        };
    }

    std::vector<std::uint8_t> pmsGeneratedSetupProgramMarker(int ch, int program)
    {
        ch = std::clamp(ch, 1, 16);
        program = std::clamp(program, 0, 127);
        return {
            0xFF, 0x7F, 0x08,
            static_cast<std::uint8_t>('P'), static_cast<std::uint8_t>('M'),
            static_cast<std::uint8_t>('S'), static_cast<std::uint8_t>('P'),
            static_cast<std::uint8_t>('C'), 0x01,
            static_cast<std::uint8_t>(ch),
            static_cast<std::uint8_t>(program)
        };
    }

    std::vector<std::uint8_t> controlChange(int ch, int controller, int value)
    {
        ch = std::clamp(ch, 1, 16);
        controller = std::clamp(controller, 0, 127);
        value = std::clamp(value, 0, 127);

        return {
            static_cast<std::uint8_t>(176 | ((ch - 1) & 15)),
            static_cast<std::uint8_t>(controller),
            static_cast<std::uint8_t>(value)
        };
    }

    std::vector<std::uint8_t> pitchBend(int ch, int signedValue)
    {
        ch = std::clamp(ch, 1, 16);
        const int raw = std::clamp(signedValue + 8192, 0, 16383);
        return {
            static_cast<std::uint8_t>(0xE0 | ((ch - 1) & 15)),
            static_cast<std::uint8_t>(raw & 0x7F),
            static_cast<std::uint8_t>((raw >> 7) & 0x7F)
        };
    }

    std::vector<std::uint8_t> on(int ch, int p, int v)
    {
        ch = std::clamp(ch, 1, 16);
        return {
            static_cast<std::uint8_t>(144 | ((ch - 1) & 15)),
            static_cast<std::uint8_t>(std::clamp(p, 0, 127)),
            static_cast<std::uint8_t>(std::clamp(v, 0, 127))
        };
    }

    std::vector<std::uint8_t> programChange(int ch, int program)
    {
        ch = std::clamp(ch, 1, 16);
        program = std::clamp(program, 0, 127);
        return {
            static_cast<std::uint8_t>(192 | ((ch - 1) & 15)),
            static_cast<std::uint8_t>(program)
        };
    }

    std::vector<std::uint8_t> channelPressure(int ch, int pressure)
    {
        ch = std::clamp(ch, 1, 16);
        pressure = std::clamp(pressure, 0, 127);
        return {
            static_cast<std::uint8_t>(0xD0 | ((ch - 1) & 15)),
            static_cast<std::uint8_t>(pressure)
        };
    }

    std::vector<std::uint8_t> polyPressure(int ch, int noteNumber, int pressure)
    {
        ch = std::clamp(ch, 1, 16);
        noteNumber = std::clamp(noteNumber, 0, 127);
        pressure = std::clamp(pressure, 0, 127);
        return {
            static_cast<std::uint8_t>(0xA0 | ((ch - 1) & 15)),
            static_cast<std::uint8_t>(noteNumber),
            static_cast<std::uint8_t>(pressure)
        };
    }

    std::vector<std::uint8_t> off(int ch, int p)
    {
        ch = std::clamp(ch, 1, 16);
        return {
            static_cast<std::uint8_t>(128 | ((ch - 1) & 15)),
            static_cast<std::uint8_t>(std::clamp(p, 0, 127)),
            0
        };
    }

    std::vector<std::uint8_t> chunk(std::vector<E> ev)
    {
        std::sort(ev.begin(), ev.end(), [](const auto& a, const auto& b)
        {
            if (a.t != b.t) return a.t < b.t;
            if (a.p != b.p) return a.p < b.p;
            return a.q < b.q;
        });

        std::vector<std::uint8_t> td;
        std::int64_t prev = 0;

        for (const auto& e : ev)
        {
            vlq(td, static_cast<std::uint32_t>(std::max<std::int64_t>(0, e.t - prev)));
            td.insert(td.end(), e.b.begin(), e.b.end());
            prev = e.t;
        }

        std::vector<std::uint8_t> c {'M', 'T', 'r', 'k'};
        be32(c, static_cast<std::uint32_t>(td.size()));
        c.insert(c.end(), td.begin(), td.end());
        return c;
    }
}

namespace mw::midi
{
    bool MidiExporter::exportToFile(const mw::core::Project& p, const std::filesystem::path& path, MidiExportOptions options)
    {
        std::vector<std::vector<std::uint8_t>> chunks;

        chunks.push_back(
            chunk({
                {0, txt(3, p.getName()), 0},
                {0, tempo(p.getTempoBpm()), 1},
                {0, ts(p.getTimeSignature().numerator, p.getTimeSignature().denominator), 2},
                {0, end(), 99}
            })
        );

        bool hasSoloTrack = false;
        for (const auto& t : p.getTracks())
        {
            if (t.getSolo())
            {
                hasSoloTrack = true;
                break;
            }
        }

        for (const auto& t : p.getTracks())
        {
            if (t.getMuted())
                continue;

            if (hasSoloTrack && !t.getSolo())
                continue;

            std::vector<E> ev {
                {0, txt(3, t.getName()), 0},
                {0, txt(1, t.getInstrumentName()), 1}
            };

            const bool includeProjectExpressionEvents = options.purpose != MidiExportPurpose::LegacyNotesOnly;
            const bool markGeneratedSetupControllers = options.purpose == MidiExportPurpose::UserMidiExport;
            const int setupChannel = t.getInstrument().midiChannel;
            const auto addGeneratedSetupController = [&](int controller, int value, int priority)
            {
                if (markGeneratedSetupControllers)
                    ev.push_back({0, pmsGeneratedSetupControllerMarker(setupChannel, controller, value), priority, 0});
                ev.push_back({0, controlChange(setupChannel, controller, value), priority, 1});
            };

            addGeneratedSetupController(7, static_cast<int>(std::clamp(t.getMixerSettings().volume, 0.0f, 1.0f) * 127.0f), 2);

            // Helpful defaults for SFZ libraries that rely on expression/mod-wheel dynamics.
            // Many expressive string/brass libraries are silent or extremely quiet if CC1/CC11 are zero.
            addGeneratedSetupController(1, 100, 3);
            addGeneratedSetupController(11, 127, 4);
            addGeneratedSetupController(0, (t.getInstrument().midiBank >> 7) & 0x7F, 5);
            addGeneratedSetupController(32, t.getInstrument().midiBank & 0x7F, 6);
            if (markGeneratedSetupControllers)
                ev.push_back({0, pmsGeneratedSetupProgramMarker(setupChannel, t.getInstrument().midiProgram), 7, 0});
            ev.push_back({0, programChange(setupChannel, t.getInstrument().midiProgram), 7, 1});

            std::int64_t last = 0;

            if (includeProjectExpressionEvents)
            {
                for (const auto& controller : t.getMidiControllerEvents())
                {
                    if (controller.tick < 0 || controller.midiChannel < 1 || controller.midiChannel > 16
                        || controller.controller < 0 || controller.controller > 127 || controller.value < 0 || controller.value > 127)
                        continue;
                    ev.push_back({ controller.tick, controlChange(controller.midiChannel, controller.controller, controller.value), 8 });
                    last = std::max(last, controller.tick);
                }
            }

            if (includeProjectExpressionEvents)
            {
                // Semantic 14-bit controllers are emitted as standard MSB/LSB CC pairs.
                // They sort after ordinary raw CC at the same tick so the semantic lane
                // is authoritative without mutating the raw Controller Lane. CC0/32 is
                // deliberately excluded by the project model because it remains Bank Select.
                for (const auto& controller : t.getMidi14BitControllerEvents())
                {
                    if (controller.tick < 0 || controller.midiChannel < 1 || controller.midiChannel > 16
                        || controller.controllerMsb < mw::core::midi14BitControllerMinimum
                        || controller.controllerMsb > mw::core::midi14BitControllerMaximum
                        || controller.value < mw::core::midi14BitMinimum || controller.value > mw::core::midi14BitMaximum)
                        continue;
                    ev.push_back({ controller.tick, controlChange(controller.midiChannel, controller.controllerMsb, mw::core::midi14BitMsb(controller.value)), 8, 20 });
                    ev.push_back({ controller.tick, controlChange(controller.midiChannel, controller.controllerMsb + 32, mw::core::midi14BitLsb(controller.value)), 8, 21 });
                    last = std::max(last, controller.tick);
                }

                for (const auto& parameter : t.getMidiRpnEvents())
                {
                    if (parameter.tick < 0 || parameter.midiChannel < 1 || parameter.midiChannel > 16
                        || parameter.parameter < mw::core::midiParameterMinimum || parameter.parameter >= mw::core::midiRpnNullParameter
                        || parameter.value < mw::core::midiParameterMinimum || parameter.value > mw::core::midiParameterMaximum)
                        continue;
                    ev.push_back({ parameter.tick, controlChange(parameter.midiChannel, 101, mw::core::midi14BitMsb(parameter.parameter)), 8, 30 });
                    ev.push_back({ parameter.tick, controlChange(parameter.midiChannel, 100, mw::core::midi14BitLsb(parameter.parameter)), 8, 31 });
                    ev.push_back({ parameter.tick, controlChange(parameter.midiChannel, 6, mw::core::midi14BitMsb(parameter.value)), 8, 32 });
                    ev.push_back({ parameter.tick, controlChange(parameter.midiChannel, 38, mw::core::midi14BitLsb(parameter.value)), 8, 33 });
                    // Deterministic RPN deselection is generated only on the wire; it is
                    // never stored as a phantom project event.
                    ev.push_back({ parameter.tick, controlChange(parameter.midiChannel, 101, 127), 8, 34 });
                    ev.push_back({ parameter.tick, controlChange(parameter.midiChannel, 100, 127), 8, 35 });
                    last = std::max(last, parameter.tick);
                }

                for (const auto& parameter : t.getMidiNrpnEvents())
                {
                    if (parameter.tick < 0 || parameter.midiChannel < 1 || parameter.midiChannel > 16
                        || parameter.parameter < mw::core::midiParameterMinimum || parameter.parameter > mw::core::midiParameterMaximum
                        || parameter.value < mw::core::midiParameterMinimum || parameter.value > mw::core::midiParameterMaximum)
                        continue;
                    ev.push_back({ parameter.tick, controlChange(parameter.midiChannel, 99, mw::core::midi14BitMsb(parameter.parameter)), 8, 40 });
                    ev.push_back({ parameter.tick, controlChange(parameter.midiChannel, 98, mw::core::midi14BitLsb(parameter.parameter)), 8, 41 });
                    ev.push_back({ parameter.tick, controlChange(parameter.midiChannel, 6, mw::core::midi14BitMsb(parameter.value)), 8, 42 });
                    ev.push_back({ parameter.tick, controlChange(parameter.midiChannel, 38, mw::core::midi14BitLsb(parameter.value)), 8, 43 });
                    last = std::max(last, parameter.tick);
                }
            }

            if (includeProjectExpressionEvents)
            {
                for (const auto& program : t.getMidiProgramChangeEvents())
                {
                    if (program.tick < 0 || program.midiChannel < 1 || program.midiChannel > 16
                        || program.program < 0 || program.program > 127)
                        continue;
                    ev.push_back({ program.tick, programChange(program.midiChannel, program.program), 9 });
                    last = std::max(last, program.tick);
                }
            }

            if (includeProjectExpressionEvents)
            {
                for (const auto& bend : t.getMidiPitchBendEvents())
                {
                    if (bend.tick < 0 || bend.midiChannel < 1 || bend.midiChannel > 16
                        || bend.value < mw::core::midiPitchBendMinimum || bend.value > mw::core::midiPitchBendMaximum)
                        continue;
                    ev.push_back({ bend.tick, pitchBend(bend.midiChannel, bend.value), 10 });
                    last = std::max(last, bend.tick);
                }
            }

            if (includeProjectExpressionEvents)
            {
                for (const auto& pressure : t.getMidiChannelPressureEvents())
                {
                    if (pressure.tick < 0 || pressure.midiChannel < 1 || pressure.midiChannel > 16
                        || pressure.pressure < mw::core::midiChannelPressureMinimum || pressure.pressure > mw::core::midiChannelPressureMaximum)
                        continue;
                    ev.push_back({ pressure.tick, channelPressure(pressure.midiChannel, pressure.pressure), 11 });
                    last = std::max(last, pressure.tick);
                }
            }

            for (const auto& n : t.getNotes())
            {
                ev.push_back({n.startTick, on(n.midiChannel, n.pitch, n.velocity), 12});
                ev.push_back({n.startTick + n.durationTicks, off(n.midiChannel, n.pitch), 5});
                last = std::max(last, n.startTick + n.durationTicks);
            }

            if (includeProjectExpressionEvents)
            {
                for (const auto& pressure : t.getMidiPolyPressureEvents())
                {
                    if (pressure.tick < 0 || pressure.midiChannel < 1 || pressure.midiChannel > 16
                        || pressure.noteNumber < 0 || pressure.noteNumber > 127
                        || pressure.pressure < mw::core::midiPolyPressureMinimum || pressure.pressure > mw::core::midiPolyPressureMaximum)
                        continue;
                    ev.push_back({ pressure.tick, polyPressure(pressure.midiChannel, pressure.noteNumber, pressure.pressure), 13 });
                    last = std::max(last, pressure.tick);
                }
            }

            ev.push_back({last + 1, end(), 99});
            chunks.push_back(chunk(ev));
        }

        std::vector<std::uint8_t> d {'M', 'T', 'h', 'd'};
        be32(d, 6);
        be16(d, 1);
        be16(d, static_cast<std::uint16_t>(chunks.size()));
        be16(d, static_cast<std::uint16_t>(mw::core::Project::ticksPerQuarterNote));

        for (const auto& c : chunks)
            d.insert(d.end(), c.begin(), c.end());

        std::ofstream f(path, std::ios::binary);
        if (!f) return false;

        f.write(reinterpret_cast<const char*>(d.data()), static_cast<std::streamsize>(d.size()));
        return f.good();
    }
}
