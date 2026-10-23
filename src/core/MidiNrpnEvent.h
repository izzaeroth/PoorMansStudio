#pragma once

#include "core/MidiRpnEvent.h"

#include <cstdint>

namespace mw::core
{
    struct MidiNrpnEvent
    {
        StableId stableId = allocateStableId();
        std::int64_t tick = 0;
        int midiChannel = 1;
        int parameter = 0;
        int value = 0;

        MidiNrpnEvent() = default;
        MidiNrpnEvent(std::int64_t tickIn, int midiChannelIn, int parameterIn, int valueIn)
            : stableId(allocateStableId()), tick(tickIn), midiChannel(midiChannelIn), parameter(parameterIn), value(valueIn) {}
        MidiNrpnEvent(StableId stableIdIn, std::int64_t tickIn, int midiChannelIn, int parameterIn, int valueIn)
            : stableId(stableIdIn), tick(tickIn), midiChannel(midiChannelIn), parameter(parameterIn), value(valueIn) {}
    };

    inline bool midiNrpnEventOrder(const MidiNrpnEvent& left, const MidiNrpnEvent& right) noexcept
    {
        if (left.tick != right.tick) return left.tick < right.tick;
        if (left.midiChannel != right.midiChannel) return left.midiChannel < right.midiChannel;
        if (left.parameter != right.parameter) return left.parameter < right.parameter;
        return left.stableId < right.stableId;
    }
}
