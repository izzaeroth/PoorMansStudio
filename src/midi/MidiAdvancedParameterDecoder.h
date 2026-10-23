#pragma once

#include "core/MidiTakeReview.h"

#include <vector>

namespace mw::midi
{
    struct MidiAdvancedParameterDecodeResult
    {
        std::vector<mw::core::PersistentMidiControllerEvent> ordinaryControllers;
        std::vector<mw::core::PersistentMidi14BitControllerEvent> controller14BitEvents;
        std::vector<mw::core::PersistentMidiRpnEvent> rpnEvents;
        std::vector<mw::core::PersistentMidiNrpnEvent> nrpnEvents;
    };

    // Decodes complete standard MIDI advanced-controller transactions while
    // preserving every unproven/incomplete input message as ordinary CC data.
    // CC0/32 is deliberately excluded from 14-bit pairing because it remains
    // Poor Man's Studio Bank Select state.
    MidiAdvancedParameterDecodeResult decodeAdvancedMidiParameters(
        const std::vector<mw::core::PersistentMidiControllerEvent>& source);
}
