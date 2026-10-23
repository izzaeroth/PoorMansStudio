#pragma once

#include "core/NoteEvent.h"
#include "core/MidiPitchBendEvent.h"
#include "core/MidiProgramChangeEvent.h"
#include "core/MidiChannelPressureEvent.h"
#include "core/MidiPolyPressureEvent.h"
#include "core/Midi14BitControllerEvent.h"
#include "core/MidiRpnEvent.h"
#include "core/MidiNrpnEvent.h"
#include "core/StableId.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace mw::core
{
    enum class PersistentMidiTakeReviewMode
    {
        WholePass = 1,
        Comp = 2
    };

    enum class PersistentMidiCompGrid
    {
        Beat = 1,
        Bar = 2,
        Custom = 3
    };

    enum class PersistentMidiCompBoundaryNoteMode
    {
        Cut = 1,
        PreserveSamePass = 2
    };

    enum class PersistentMidiSustainPedalMode
    {
        Ignore = 1,
        ApplyCc64 = 2
    };

    enum class PersistentMidiCompCrossPassNoteMode
    {
        Off = 1,
        BridgeExact = 2
    };

    struct PersistentMidiCc64Event
    {
        std::int64_t tick = 0;
        int midiChannel = 1;
        int value = 0;
        std::uint64_t order = 0;
    };

    // Retained Take Review controller performance. This deliberately has no
    // project stable ID: identity is allocated only when a retained event is
    // committed to the track's project Controller Lane.
    struct PersistentMidiControllerEvent
    {
        std::int64_t tick = 0;
        int midiChannel = 1;
        int controller = 0;
        int value = 0;
        std::uint64_t order = 0;
    };

    // Retained Take Review pitch-bend performance. Like retained CC data, this
    // has no project stable ID until Keep commits it to the project Pitch Bend Lane.
    struct PersistentMidiPitchBendEvent
    {
        std::int64_t tick = 0;
        int midiChannel = 1;
        int value = 0; // signed project representation: -8192..+8191
        std::uint64_t order = 0;
    };

    // Retained Take Review Program Change performance. It receives a project
    // stable ID only when Keep commits it to the project Program Change Lane.
    struct PersistentMidiProgramChangeEvent
    {
        std::int64_t tick = 0;
        int midiChannel = 1;
        int program = 0;
        std::uint64_t order = 0;
    };

    // Retained Take Review Channel Pressure performance. Project stable identity
    // is allocated only when Keep commits it to the project Channel Pressure Lane.
    struct PersistentMidiChannelPressureEvent
    {
        std::int64_t tick = 0;
        int midiChannel = 1;
        int pressure = 0;
        std::uint64_t order = 0;
    };

    // Retained Take Review Polyphonic Key Pressure performance. It remains
    // channel+key data and receives project stable identity only on Keep.
    struct PersistentMidiPolyPressureEvent
    {
        std::int64_t tick = 0;
        int midiChannel = 1;
        int noteNumber = 60;
        int pressure = 0;
        std::uint64_t order = 0;
    };

    struct PersistentMidi14BitControllerEvent
    {
        std::int64_t tick = 0;
        int midiChannel = 1;
        int controllerMsb = 1;
        int value = 0;
        std::uint64_t order = 0;
    };

    struct PersistentMidiRpnEvent
    {
        std::int64_t tick = 0;
        int midiChannel = 1;
        int parameter = 0;
        int value = 0;
        std::uint64_t order = 0;
    };

    struct PersistentMidiNrpnEvent
    {
        std::int64_t tick = 0;
        int midiChannel = 1;
        int parameter = 0;
        int value = 0;
        std::uint64_t order = 0;
    };

    struct PersistentMidiTakePass
    {
        PersistentMidiTakePass() = default;
        PersistentMidiTakePass(int passNumberIn,
                               std::vector<NoteEvent> notesIn,
                               std::uint16_t sustainDownMaskAtStartIn = 0,
                               std::vector<PersistentMidiCc64Event> cc64EventsIn = {},
                               std::vector<PersistentMidiControllerEvent> controllerEventsIn = {},
                               std::vector<PersistentMidiPitchBendEvent> pitchBendEventsIn = {},
                               std::vector<PersistentMidiProgramChangeEvent> programChangeEventsIn = {},
                               std::vector<PersistentMidiChannelPressureEvent> channelPressureEventsIn = {},
                               std::vector<PersistentMidiPolyPressureEvent> polyPressureEventsIn = {},
                               std::vector<PersistentMidi14BitControllerEvent> controller14BitEventsIn = {},
                               std::vector<PersistentMidiRpnEvent> rpnEventsIn = {},
                               std::vector<PersistentMidiNrpnEvent> nrpnEventsIn = {})
            : passNumber(passNumberIn),
              notes(std::move(notesIn)),
              sustainDownMaskAtStart(sustainDownMaskAtStartIn),
              cc64Events(std::move(cc64EventsIn)),
              controllerEvents(std::move(controllerEventsIn)),
              pitchBendEvents(std::move(pitchBendEventsIn)),
              programChangeEvents(std::move(programChangeEventsIn)),
              channelPressureEvents(std::move(channelPressureEventsIn)),
              polyPressureEvents(std::move(polyPressureEventsIn)),
              controller14BitEvents(std::move(controller14BitEventsIn)),
              rpnEvents(std::move(rpnEventsIn)),
              nrpnEvents(std::move(nrpnEventsIn))
        {
        }

        int passNumber = 0;
        std::vector<NoteEvent> notes;
        std::uint16_t sustainDownMaskAtStart = 0;
        std::vector<PersistentMidiCc64Event> cc64Events;
        std::vector<PersistentMidiControllerEvent> controllerEvents;
        std::vector<PersistentMidiPitchBendEvent> pitchBendEvents;
        std::vector<PersistentMidiProgramChangeEvent> programChangeEvents;
        std::vector<PersistentMidiChannelPressureEvent> channelPressureEvents;
        std::vector<PersistentMidiPolyPressureEvent> polyPressureEvents;
        std::vector<PersistentMidi14BitControllerEvent> controller14BitEvents;
        std::vector<PersistentMidiRpnEvent> rpnEvents;
        std::vector<PersistentMidiNrpnEvent> nrpnEvents;
    };

    struct PersistentMidiCompSegment
    {
        std::int64_t startTick = 0;
        std::int64_t endTick = 0;
        int selectedPassNumber = 0;
    };

    struct PersistentMidiTakeReview
    {
        bool active = false;
        StableId reviewSessionId = 0;
        std::string reviewName;
        std::vector<std::string> reviewTags;
        std::string reviewNotes;
        std::int64_t regionInTick = 0;
        std::int64_t regionOutTick = 0;
        int selectedPassNumber = 0;
        PersistentMidiTakeReviewMode reviewMode = PersistentMidiTakeReviewMode::WholePass;
        PersistentMidiCompGrid compGrid = PersistentMidiCompGrid::Beat;
        PersistentMidiCompBoundaryNoteMode boundaryNoteMode = PersistentMidiCompBoundaryNoteMode::Cut;
        PersistentMidiSustainPedalMode sustainPedalMode = PersistentMidiSustainPedalMode::Ignore;
        PersistentMidiCompCrossPassNoteMode crossPassNoteMode = PersistentMidiCompCrossPassNoteMode::Off;
        std::vector<PersistentMidiTakePass> passes;
        std::vector<PersistentMidiCompSegment> segments;

        void clear() noexcept
        {
            active = false;
            reviewSessionId = 0;
            reviewName.clear();
            reviewTags.clear();
            reviewNotes.clear();
            regionInTick = 0;
            regionOutTick = 0;
            selectedPassNumber = 0;
            reviewMode = PersistentMidiTakeReviewMode::WholePass;
            compGrid = PersistentMidiCompGrid::Beat;
            boundaryNoteMode = PersistentMidiCompBoundaryNoteMode::Cut;
            sustainPedalMode = PersistentMidiSustainPedalMode::Ignore;
            crossPassNoteMode = PersistentMidiCompCrossPassNoteMode::Off;
            passes.clear();
            segments.clear();
        }
    };
}
