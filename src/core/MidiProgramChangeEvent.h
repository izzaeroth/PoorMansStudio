#pragma once

#include "core/StableId.h"

#include <cstdint>

namespace mw::core
{
    inline constexpr int midiProgramChangeMinimum = 0;
    inline constexpr int midiProgramChangeMaximum = 127;

    struct MidiProgramChangeEvent
    {
        StableId stableId = allocateStableId();
        std::int64_t tick = 0;
        int midiChannel = 1;
        int program = 0;

        MidiProgramChangeEvent() = default;
        MidiProgramChangeEvent(std::int64_t tickIn, int midiChannelIn, int programIn)
            : stableId(allocateStableId()), tick(tickIn), midiChannel(midiChannelIn), program(programIn) {}
        MidiProgramChangeEvent(StableId stableIdIn, std::int64_t tickIn, int midiChannelIn, int programIn)
            : stableId(stableIdIn), tick(tickIn), midiChannel(midiChannelIn), program(programIn) {}
    };

    inline bool midiProgramChangeEventOrder(const MidiProgramChangeEvent& left, const MidiProgramChangeEvent& right) noexcept
    {
        if (left.tick != right.tick) return left.tick < right.tick;
        if (left.midiChannel != right.midiChannel) return left.midiChannel < right.midiChannel;
        return left.stableId < right.stableId;
    }
}
