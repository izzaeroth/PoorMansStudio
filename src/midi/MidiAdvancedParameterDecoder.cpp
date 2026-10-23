#include "midi/MidiAdvancedParameterDecoder.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <utility>

namespace mw::midi
{
    namespace
    {
        enum class ParameterKind { None, Rpn, Nrpn };

        struct ChannelState
        {
            ParameterKind kind = ParameterKind::None;
            int parameterMsb = -1;
            int parameterLsb = -1;
            std::optional<std::size_t> selectorMsbIndex;
            std::optional<std::size_t> selectorLsbIndex;
            int dataMsb = -1;
            std::optional<std::size_t> dataMsbIndex;
            bool emittedSemantic = false;
        };

        bool validController(const mw::core::PersistentMidiControllerEvent& event)
        {
            return event.tick >= 0 && event.midiChannel >= 1 && event.midiChannel <= 16
                && event.controller >= 0 && event.controller <= 127
                && event.value >= 0 && event.value <= 127;
        }
    }

    MidiAdvancedParameterDecodeResult decodeAdvancedMidiParameters(
        const std::vector<mw::core::PersistentMidiControllerEvent>& source)
    {
        MidiAdvancedParameterDecodeResult result;
        if (source.empty())
            return result;

        std::vector<mw::core::PersistentMidiControllerEvent> events = source;
        std::stable_sort(events.begin(), events.end(), [](const auto& a, const auto& b)
        {
            if (a.order != b.order) return a.order < b.order;
            if (a.tick != b.tick) return a.tick < b.tick;
            if (a.midiChannel != b.midiChannel) return a.midiChannel < b.midiChannel;
            return a.controller < b.controller;
        });

        std::vector<bool> consumed(events.size(), false);
        std::vector<bool> block14Bit(events.size(), false);
        std::array<ChannelState, 16> states {};

        for (std::size_t i = 0; i < events.size(); ++i)
        {
            const auto& event = events[i];
            if (!validController(event))
                continue;
            auto& state = states[static_cast<std::size_t>(event.midiChannel - 1)];

            const auto setSelector = [&](ParameterKind kind, bool msb)
            {
                if (state.kind != kind)
                {
                    state = {};
                    state.kind = kind;
                }
                if (msb)
                {
                    state.parameterMsb = event.value;
                    state.selectorMsbIndex = i;
                }
                else
                {
                    state.parameterLsb = event.value;
                    state.selectorLsbIndex = i;
                }
                state.dataMsb = -1;
                state.dataMsbIndex.reset();
            };

            if (event.controller == 101) { setSelector(ParameterKind::Rpn, true); continue; }
            if (event.controller == 100)
            {
                setSelector(ParameterKind::Rpn, false);
                if (state.parameterMsb == 127 && state.parameterLsb == 127 && state.emittedSemantic)
                {
                    if (state.selectorMsbIndex) consumed[*state.selectorMsbIndex] = true;
                    if (state.selectorLsbIndex) consumed[*state.selectorLsbIndex] = true;
                }
                continue;
            }
            if (event.controller == 99) { setSelector(ParameterKind::Nrpn, true); continue; }
            if (event.controller == 98) { setSelector(ParameterKind::Nrpn, false); continue; }

            if (event.controller == 6 && state.kind != ParameterKind::None
                && state.parameterMsb >= 0 && state.parameterLsb >= 0)
            {
                state.dataMsb = event.value;
                state.dataMsbIndex = i;
                block14Bit[i] = true;
                continue;
            }

            if (event.controller == 38 && state.kind != ParameterKind::None
                && state.parameterMsb >= 0 && state.parameterLsb >= 0 && state.dataMsb >= 0)
            {
                block14Bit[i] = true;
                if (state.dataMsbIndex) block14Bit[*state.dataMsbIndex] = true;
                const int parameter = (state.parameterMsb << 7) | state.parameterLsb;
                const int value = (state.dataMsb << 7) | event.value;
                const bool rpnNull = state.kind == ParameterKind::Rpn && parameter == 16383;
                if (!rpnNull)
                {
                    if (state.selectorMsbIndex) consumed[*state.selectorMsbIndex] = true;
                    if (state.selectorLsbIndex) consumed[*state.selectorLsbIndex] = true;
                    if (state.dataMsbIndex) consumed[*state.dataMsbIndex] = true;
                    consumed[i] = true;
                    if (state.kind == ParameterKind::Rpn)
                        result.rpnEvents.push_back({ event.tick, event.midiChannel, parameter, value, event.order });
                    else
                        result.nrpnEvents.push_back({ event.tick, event.midiChannel, parameter, value, event.order });
                    state.emittedSemantic = true;
                }
                state.dataMsb = -1;
                state.dataMsbIndex.reset();
                continue;
            }
        }

        // Pair remaining ordinary 14-bit controller MSB/LSB messages in source
        // order. Controller 0/32 is intentionally protected Bank Select state.
        std::array<std::array<std::optional<std::size_t>, 32>, 16> pendingMsb {};
        for (std::size_t i = 0; i < events.size(); ++i)
        {
            const auto& event = events[i];
            if (consumed[i] || block14Bit[i] || !validController(event))
                continue;
            const auto channelIndex = static_cast<std::size_t>(event.midiChannel - 1);
            if (event.controller >= 1 && event.controller <= 31)
            {
                pendingMsb[channelIndex][static_cast<std::size_t>(event.controller)] = i;
                continue;
            }
            if (event.controller >= 33 && event.controller <= 63)
            {
                const int controllerMsb = event.controller - 32;
                auto& pending = pendingMsb[channelIndex][static_cast<std::size_t>(controllerMsb)];
                if (!pending)
                    continue;
                const auto msbIndex = *pending;
                if (consumed[msbIndex] || block14Bit[msbIndex])
                {
                    pending.reset();
                    continue;
                }
                const auto& msb = events[msbIndex];
                result.controller14BitEvents.push_back({ event.tick, event.midiChannel, controllerMsb,
                    (msb.value << 7) | event.value, event.order });
                consumed[msbIndex] = true;
                consumed[i] = true;
                pending.reset();
            }
        }

        for (std::size_t i = 0; i < events.size(); ++i)
            if (!consumed[i]) result.ordinaryControllers.push_back(events[i]);
        return result;
    }
}
