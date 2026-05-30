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
            return a.t == b.t ? a.p < b.p : a.t < b.t;
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
    bool MidiExporter::exportToFile(const mw::core::Project& p, const std::filesystem::path& path)
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
                {0, txt(1, t.getInstrumentName()), 1},
                {0, controlChange(t.getInstrument().midiChannel, 7, static_cast<int>(std::clamp(t.getMixerSettings().volume, 0.0f, 1.0f) * 127.0f)), 2},

                // Helpful defaults for SFZ libraries that rely on expression/mod-wheel dynamics.
                // Many expressive string/brass libraries are silent or extremely quiet if CC1/CC11 are zero.
                {0, controlChange(t.getInstrument().midiChannel, 1, 100), 3},
                {0, controlChange(t.getInstrument().midiChannel, 11, 127), 4},

                {0, controlChange(t.getInstrument().midiChannel, 0, (t.getInstrument().midiBank >> 7) & 0x7F), 5},
                {0, controlChange(t.getInstrument().midiChannel, 32, t.getInstrument().midiBank & 0x7F), 6},
                {0, programChange(t.getInstrument().midiChannel, t.getInstrument().midiProgram), 7}
            };

            std::int64_t last = 0;

            for (const auto& n : t.getNotes())
            {
                ev.push_back({n.startTick, on(n.midiChannel, n.pitch, n.velocity), 10});
                ev.push_back({n.startTick + n.durationTicks, off(n.midiChannel, n.pitch), 5});
                last = std::max(last, n.startTick + n.durationTicks);
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
