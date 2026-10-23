#pragma once

#include "core/StableId.h"

#include <cstdint>

namespace mw::core
{
    struct MidiControllerEvent
    {
        StableId stableId = allocateStableId();
        std::int64_t tick = 0;
        int midiChannel = 1;
        int controller = 0;
        int value = 0;

        MidiControllerEvent() = default;
        MidiControllerEvent(std::int64_t tickIn, int midiChannelIn, int controllerIn, int valueIn)
            : stableId(allocateStableId()), tick(tickIn), midiChannel(midiChannelIn), controller(controllerIn), value(valueIn) {}
        MidiControllerEvent(StableId stableIdIn, std::int64_t tickIn, int midiChannelIn, int controllerIn, int valueIn)
            : stableId(stableIdIn), tick(tickIn), midiChannel(midiChannelIn), controller(controllerIn), value(valueIn) {}
    };

    inline bool midiControllerEventOrder(const MidiControllerEvent& left, const MidiControllerEvent& right) noexcept
    {
        if (left.tick != right.tick) return left.tick < right.tick;
        if (left.midiChannel != right.midiChannel) return left.midiChannel < right.midiChannel;
        if (left.controller != right.controller) return left.controller < right.controller;
        return left.stableId < right.stableId;
    }
}
