#pragma once

#include "core/StableId.h"

#include <cstdint>

namespace mw::core
{
    inline constexpr int midiChannelPressureMinimum = 0;
    inline constexpr int midiChannelPressureMaximum = 127;

    struct MidiChannelPressureEvent
    {
        StableId stableId = allocateStableId();
        std::int64_t tick = 0;
        int midiChannel = 1;
        int pressure = 0;

        MidiChannelPressureEvent() = default;
        MidiChannelPressureEvent(std::int64_t tickIn, int midiChannelIn, int pressureIn)
            : stableId(allocateStableId()), tick(tickIn), midiChannel(midiChannelIn), pressure(pressureIn) {}
        MidiChannelPressureEvent(StableId stableIdIn, std::int64_t tickIn, int midiChannelIn, int pressureIn)
            : stableId(stableIdIn), tick(tickIn), midiChannel(midiChannelIn), pressure(pressureIn) {}
    };

    inline bool midiChannelPressureEventOrder(const MidiChannelPressureEvent& left, const MidiChannelPressureEvent& right) noexcept
    {
        if (left.tick != right.tick) return left.tick < right.tick;
        if (left.midiChannel != right.midiChannel) return left.midiChannel < right.midiChannel;
        return left.stableId < right.stableId;
    }
}
