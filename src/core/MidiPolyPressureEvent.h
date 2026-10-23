#pragma once

#include "core/StableId.h"

#include <cstdint>

namespace mw::core
{
    inline constexpr int midiPolyPressureMinimum = 0;
    inline constexpr int midiPolyPressureMaximum = 127;

    struct MidiPolyPressureEvent
    {
        StableId stableId = allocateStableId();
        std::int64_t tick = 0;
        int midiChannel = 1;
        int noteNumber = 60;
        int pressure = 0;

        MidiPolyPressureEvent() = default;
        MidiPolyPressureEvent(std::int64_t tickIn, int midiChannelIn, int noteNumberIn, int pressureIn)
            : stableId(allocateStableId()), tick(tickIn), midiChannel(midiChannelIn), noteNumber(noteNumberIn), pressure(pressureIn) {}
        MidiPolyPressureEvent(StableId stableIdIn, std::int64_t tickIn, int midiChannelIn, int noteNumberIn, int pressureIn)
            : stableId(stableIdIn), tick(tickIn), midiChannel(midiChannelIn), noteNumber(noteNumberIn), pressure(pressureIn) {}
    };

    inline bool midiPolyPressureEventOrder(const MidiPolyPressureEvent& left, const MidiPolyPressureEvent& right) noexcept
    {
        if (left.tick != right.tick) return left.tick < right.tick;
        if (left.midiChannel != right.midiChannel) return left.midiChannel < right.midiChannel;
        if (left.noteNumber != right.noteNumber) return left.noteNumber < right.noteNumber;
        return left.stableId < right.stableId;
    }
}
