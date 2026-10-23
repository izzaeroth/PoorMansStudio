#pragma once

#include "core/StableId.h"

#include <cstdint>

namespace mw::core
{
    inline constexpr int midi14BitMinimum = 0;
    inline constexpr int midi14BitMaximum = 16383;
    inline constexpr int midi14BitControllerMinimum = 1; // CC0/32 remains Bank Select.
    inline constexpr int midi14BitControllerMaximum = 31;

    struct Midi14BitControllerEvent
    {
        StableId stableId = allocateStableId();
        std::int64_t tick = 0;
        int midiChannel = 1;
        int controllerMsb = 1; // 1..31; LSB is controllerMsb + 32.
        int value = 0;         // 0..16383.

        Midi14BitControllerEvent() = default;
        Midi14BitControllerEvent(std::int64_t tickIn, int midiChannelIn, int controllerMsbIn, int valueIn)
            : stableId(allocateStableId()), tick(tickIn), midiChannel(midiChannelIn), controllerMsb(controllerMsbIn), value(valueIn) {}
        Midi14BitControllerEvent(StableId stableIdIn, std::int64_t tickIn, int midiChannelIn, int controllerMsbIn, int valueIn)
            : stableId(stableIdIn), tick(tickIn), midiChannel(midiChannelIn), controllerMsb(controllerMsbIn), value(valueIn) {}
    };

    inline bool midi14BitControllerEventOrder(const Midi14BitControllerEvent& left, const Midi14BitControllerEvent& right) noexcept
    {
        if (left.tick != right.tick) return left.tick < right.tick;
        if (left.midiChannel != right.midiChannel) return left.midiChannel < right.midiChannel;
        if (left.controllerMsb != right.controllerMsb) return left.controllerMsb < right.controllerMsb;
        return left.stableId < right.stableId;
    }

    inline int combineMidi14Bit(int msb, int lsb) noexcept
    {
        return ((msb & 0x7F) << 7) | (lsb & 0x7F);
    }

    inline int midi14BitMsb(int value) noexcept { return (value >> 7) & 0x7F; }
    inline int midi14BitLsb(int value) noexcept { return value & 0x7F; }
}
