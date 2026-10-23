#pragma once

#include "core/MidiTakeReview.h"
#include "core/StableId.h"

#include <cstdint>
#include <utility>
#include <vector>

namespace mw::midi
{
    enum class MidiRecordingSustainPedalMode
    {
        Ignore = 1,
        ApplyCc64 = 2
    };

    inline constexpr const char midiRecordingSustainPedalTooltip[] =
        "Sustain Pedal controls CC64 interpretation for retained TAKE REVIEW passes. Ignore keeps recorded Note Off durations unchanged. Apply CC64 lets pedal-down notes continue until pedal release, subject to the current comp boundary rule and Loop Out.";
    inline constexpr const char midiRecordingSustainIgnoreTooltip[] =
        "Ignore keeps the retained pass Note On/Off durations exactly as recorded and preserves the v0.67.25 review/comp result.";
    inline constexpr const char midiRecordingSustainApplyCc64Tooltip[] =
        "Apply CC64 extends a real note when its key is released while sustain is down on the same MIDI channel. Pedal release, a pass-change boundary, Cut mode, or Loop Out can still stop the note; no Note On is invented.";

    using MidiRecordingCc64Event = mw::core::PersistentMidiCc64Event;
    using MidiRecordingControllerEvent = mw::core::PersistentMidiControllerEvent;
    using MidiRecordingPitchBendEvent = mw::core::PersistentMidiPitchBendEvent;
    using MidiRecordingProgramChangeEvent = mw::core::PersistentMidiProgramChangeEvent;
    using MidiRecordingChannelPressureEvent = mw::core::PersistentMidiChannelPressureEvent;
    using MidiRecordingPolyPressureEvent = mw::core::PersistentMidiPolyPressureEvent;
    using MidiRecording14BitControllerEvent = mw::core::PersistentMidi14BitControllerEvent;
    using MidiRecordingRpnEvent = mw::core::PersistentMidiRpnEvent;
    using MidiRecordingNrpnEvent = mw::core::PersistentMidiNrpnEvent;

    inline constexpr const char midiRecordingRecordControllersTooltip[] =
        "Record Controllers captures incoming MIDI Control Change messages CC 0-127 into the armed MIDI track at exact project ticks. Off preserves note-only project-controller capture while CC64 can still feed TAKE REVIEW Sustain Pedal semantics. All CC records ordinary project controller data; Note Quantize and graphical Controller Lane Snap do not change recorded CC timing.";

    struct MidiRecordingLoopTakePass
    {
        MidiRecordingLoopTakePass() = default;
        MidiRecordingLoopTakePass(int passNumberIn,
                                  std::vector<mw::core::NoteEvent> notesIn,
                                  std::uint16_t sustainDownMaskAtStartIn = 0,
                                  std::vector<MidiRecordingCc64Event> cc64EventsIn = {},
                                  std::vector<MidiRecordingControllerEvent> controllerEventsIn = {},
                                  std::vector<MidiRecordingPitchBendEvent> pitchBendEventsIn = {},
                                  std::vector<MidiRecordingProgramChangeEvent> programChangeEventsIn = {},
                                  std::vector<MidiRecordingChannelPressureEvent> channelPressureEventsIn = {},
                                  std::vector<MidiRecordingPolyPressureEvent> polyPressureEventsIn = {},
                                  std::vector<MidiRecording14BitControllerEvent> controller14BitEventsIn = {},
                                  std::vector<MidiRecordingRpnEvent> rpnEventsIn = {},
                                  std::vector<MidiRecordingNrpnEvent> nrpnEventsIn = {})
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
        std::vector<mw::core::NoteEvent> notes;
        std::uint16_t sustainDownMaskAtStart = 0;
        std::vector<MidiRecordingCc64Event> cc64Events;
        std::vector<MidiRecordingControllerEvent> controllerEvents;
        std::vector<MidiRecordingPitchBendEvent> pitchBendEvents;
        std::vector<MidiRecordingProgramChangeEvent> programChangeEvents;
        std::vector<MidiRecordingChannelPressureEvent> channelPressureEvents;
        std::vector<MidiRecordingPolyPressureEvent> polyPressureEvents;
        std::vector<MidiRecording14BitControllerEvent> controller14BitEvents;
        std::vector<MidiRecordingRpnEvent> rpnEvents;
        std::vector<MidiRecordingNrpnEvent> nrpnEvents;
    };

    std::vector<MidiRecordingControllerEvent> normalizeMidiRecordingControllerEvents(
        std::vector<MidiRecordingControllerEvent> events);
    std::vector<MidiRecordingControllerEvent> mergeMidiRecordingControllerPasses(
        const std::vector<MidiRecordingLoopTakePass>& passes);
    std::vector<MidiRecordingPitchBendEvent> normalizeMidiRecordingPitchBendEvents(
        std::vector<MidiRecordingPitchBendEvent> events);
    std::vector<MidiRecordingPitchBendEvent> mergeMidiRecordingPitchBendPasses(
        const std::vector<MidiRecordingLoopTakePass>& passes);
    std::vector<MidiRecordingProgramChangeEvent> normalizeMidiRecordingProgramChangeEvents(
        std::vector<MidiRecordingProgramChangeEvent> events);
    std::vector<MidiRecordingProgramChangeEvent> mergeMidiRecordingProgramChangePasses(
        const std::vector<MidiRecordingLoopTakePass>& passes);
    std::vector<MidiRecordingChannelPressureEvent> normalizeMidiRecordingChannelPressureEvents(
        std::vector<MidiRecordingChannelPressureEvent> events);
    std::vector<MidiRecordingChannelPressureEvent> mergeMidiRecordingChannelPressurePasses(
        const std::vector<MidiRecordingLoopTakePass>& passes);
    std::vector<MidiRecordingPolyPressureEvent> normalizeMidiRecordingPolyPressureEvents(
        std::vector<MidiRecordingPolyPressureEvent> events);
    std::vector<MidiRecordingPolyPressureEvent> mergeMidiRecordingPolyPressurePasses(
        const std::vector<MidiRecordingLoopTakePass>& passes);
    std::vector<MidiRecording14BitControllerEvent> normalizeMidiRecording14BitControllerEvents(
        std::vector<MidiRecording14BitControllerEvent> events);
    std::vector<MidiRecording14BitControllerEvent> mergeMidiRecording14BitControllerPasses(
        const std::vector<MidiRecordingLoopTakePass>& passes);
    std::vector<MidiRecordingRpnEvent> normalizeMidiRecordingRpnEvents(std::vector<MidiRecordingRpnEvent> events);
    std::vector<MidiRecordingRpnEvent> mergeMidiRecordingRpnPasses(const std::vector<MidiRecordingLoopTakePass>& passes);
    std::vector<MidiRecordingNrpnEvent> normalizeMidiRecordingNrpnEvents(std::vector<MidiRecordingNrpnEvent> events);
    std::vector<MidiRecordingNrpnEvent> mergeMidiRecordingNrpnPasses(const std::vector<MidiRecordingLoopTakePass>& passes);
    std::vector<mw::core::NoteEvent> buildMidiRecordingTakePassNotes(
        const MidiRecordingLoopTakePass& pass,
        MidiRecordingSustainPedalMode sustainMode,
        std::int64_t regionOutTick);

    class MidiRecordingLoopTakeReviewState final
    {
    public:
        std::uint64_t begin(mw::core::StableId projectId,
                            mw::core::StableId trackId,
                            std::uint64_t recordingGeneration) noexcept;
        void cancel() noexcept;

        bool addPass(std::uint64_t generation,
                     int passNumber,
                     std::vector<mw::core::NoteEvent> notes,
                     std::uint16_t sustainDownMaskAtStart = 0,
                     std::vector<MidiRecordingCc64Event> cc64Events = {},
                     std::vector<MidiRecordingControllerEvent> controllerEvents = {},
                     std::vector<MidiRecordingPitchBendEvent> pitchBendEvents = {},
                     std::vector<MidiRecordingProgramChangeEvent> programChangeEvents = {},
                     std::vector<MidiRecordingChannelPressureEvent> channelPressureEvents = {},
                     std::vector<MidiRecordingPolyPressureEvent> polyPressureEvents = {},
                     std::vector<MidiRecording14BitControllerEvent> controller14BitEvents = {},
                     std::vector<MidiRecordingRpnEvent> rpnEvents = {},
                     std::vector<MidiRecordingNrpnEvent> nrpnEvents = {});
        bool finishCollection(std::uint64_t generation) noexcept;
        bool restoreReview(mw::core::StableId projectId,
                           mw::core::StableId trackId,
                           std::uint64_t generation,
                           std::vector<MidiRecordingLoopTakePass> passes,
                           int selectedPassNumber);
        bool selectPass(std::uint64_t generation, int passNumber) noexcept;

        bool isCollecting() const noexcept;
        bool isReviewActive() const noexcept;
        bool matches(std::uint64_t generation) const noexcept;
        bool matchesProjectTrack(mw::core::StableId projectId,
                                 mw::core::StableId trackId) const noexcept;
        std::uint64_t generation() const noexcept;
        mw::core::StableId projectId() const noexcept;
        mw::core::StableId trackId() const noexcept;
        int selectedPassNumber() const noexcept;
        std::size_t passCount() const noexcept;
        std::size_t totalNoteCount() const noexcept;
        std::size_t totalControllerEventCount() const noexcept;
        std::size_t totalPitchBendEventCount() const noexcept;
        std::size_t totalProgramChangeEventCount() const noexcept;
        std::size_t totalChannelPressureEventCount() const noexcept;
        std::size_t totalPolyPressureEventCount() const noexcept;
        std::size_t total14BitControllerEventCount() const noexcept;
        std::size_t totalRpnEventCount() const noexcept;
        std::size_t totalNrpnEventCount() const noexcept;
        const std::vector<MidiRecordingLoopTakePass>& passes() const noexcept;
        const MidiRecordingLoopTakePass* selectedPass() const noexcept;
        std::vector<mw::core::NoteEvent> mergedNotes() const;
        std::vector<MidiRecordingControllerEvent> mergedControllerEvents() const;
        std::vector<MidiRecordingPitchBendEvent> mergedPitchBendEvents() const;
        std::vector<MidiRecordingProgramChangeEvent> mergedProgramChangeEvents() const;
        std::vector<MidiRecordingChannelPressureEvent> mergedChannelPressureEvents() const;
        std::vector<MidiRecordingPolyPressureEvent> mergedPolyPressureEvents() const;
        std::vector<MidiRecording14BitControllerEvent> merged14BitControllerEvents() const;
        std::vector<MidiRecordingRpnEvent> mergedRpnEvents() const;
        std::vector<MidiRecordingNrpnEvent> mergedNrpnEvents() const;
        std::vector<mw::core::NoteEvent> mergedNotes(MidiRecordingSustainPedalMode sustainMode,
                                                     std::int64_t regionOutTick) const;

    private:
        const MidiRecordingLoopTakePass* findPass(int passNumber) const noexcept;

        std::uint64_t generation_ = 0;
        mw::core::StableId projectId_ = 0;
        mw::core::StableId trackId_ = 0;
        int selectedPassNumber_ = 0;
        bool collecting_ = false;
        bool reviewActive_ = false;
        std::vector<MidiRecordingLoopTakePass> passes_;
    };
}
