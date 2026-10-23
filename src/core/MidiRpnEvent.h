#pragma once

#include "core/StableId.h"

#include <cstdint>

namespace mw::core
{
    inline constexpr int midiParameterMinimum = 0;
    inline constexpr int midiParameterMaximum = 16383;
    inline constexpr int midiRpnNullParameter = 16383; // 127/127 is the MIDI Null Function, not a writable RPN.

    struct MidiRpnEvent
    {
        StableId stableId = allocateStableId();
        std::int64_t tick = 0;
        int midiChannel = 1;
        int parameter = 0;
        int value = 0;

        MidiRpnEvent() = default;
        MidiRpnEvent(std::int64_t tickIn, int midiChannelIn, int parameterIn, int valueIn)
            : stableId(allocateStableId()), tick(tickIn), midiChannel(midiChannelIn), parameter(parameterIn), value(valueIn) {}
        MidiRpnEvent(StableId stableIdIn, std::int64_t tickIn, int midiChannelIn, int parameterIn, int valueIn)
            : stableId(stableIdIn), tick(tickIn), midiChannel(midiChannelIn), parameter(parameterIn), value(valueIn) {}
    };

    inline bool midiRpnEventOrder(const MidiRpnEvent& left, const MidiRpnEvent& right) noexcept
    {
        if (left.tick != right.tick) return left.tick < right.tick;
        if (left.midiChannel != right.midiChannel) return left.midiChannel < right.midiChannel;
        if (left.parameter != right.parameter) return left.parameter < right.parameter;
        return left.stableId < right.stableId;
    }
}
