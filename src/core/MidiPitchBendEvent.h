#pragma once

#include "core/StableId.h"

#include <cstdint>

namespace mw::core
{
    inline constexpr int midiPitchBendMinimum = -8192;
    inline constexpr int midiPitchBendCenter = 0;
    inline constexpr int midiPitchBendMaximum = 8191;

    struct MidiPitchBendEvent
    {
        StableId stableId = allocateStableId();
        std::int64_t tick = 0;
        int midiChannel = 1;
        int value = midiPitchBendCenter;

        MidiPitchBendEvent() = default;
        MidiPitchBendEvent(std::int64_t tickIn, int midiChannelIn, int valueIn)
            : stableId(allocateStableId()), tick(tickIn), midiChannel(midiChannelIn), value(valueIn) {}
        MidiPitchBendEvent(StableId stableIdIn, std::int64_t tickIn, int midiChannelIn, int valueIn)
            : stableId(stableIdIn), tick(tickIn), midiChannel(midiChannelIn), value(valueIn) {}
    };

    inline bool midiPitchBendEventOrder(const MidiPitchBendEvent& left, const MidiPitchBendEvent& right) noexcept
    {
        if (left.tick != right.tick) return left.tick < right.tick;
        if (left.midiChannel != right.midiChannel) return left.midiChannel < right.midiChannel;
        return left.stableId < right.stableId;
    }
}
