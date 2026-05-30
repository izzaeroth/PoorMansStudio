#pragma once
#include <cstdint>
#include <string>

namespace mw::core
{
    enum class Articulation { Normal, Staccato, Legato, Accent };

    struct NoteEvent
    {
        int pitch = 60;
        int velocity = 100;
        int midiChannel = 1;
        std::int64_t startTick = 0;
        std::int64_t durationTicks = 960;
        Articulation articulation = Articulation::Normal;

        NoteEvent() = default;

        NoteEvent(int p, int v, std::int64_t s, std::int64_t d, int ch = 1, Articulation a = Articulation::Normal)
            : pitch(p), velocity(v), midiChannel(ch), startTick(s), durationTicks(d), articulation(a) {}
    };

    inline std::string articulationToString(Articulation a)
    {
        switch (a)
        {
            case Articulation::Staccato: return "Staccato";
            case Articulation::Legato: return "Legato";
            case Articulation::Accent: return "Accent";
            default: return "Normal";
        }
    }

    inline Articulation articulationFromString(const std::string& v)
    {
        if (v == "Staccato") return Articulation::Staccato;
        if (v == "Legato") return Articulation::Legato;
        if (v == "Accent") return Articulation::Accent;
        return Articulation::Normal;
    }
}
