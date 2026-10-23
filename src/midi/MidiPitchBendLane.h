#pragma once

#include "core/Project.h"

#include <cstdint>
#include <vector>

namespace mw::midi
{
    inline constexpr const char midiPitchBendLaneTooltip[] =
        "MIDI Pitch Bend Lane - Edits discrete channel Pitch Bend events on the selected MIDI track. Values use the exact signed MIDI range -8192 through +8191; 0 is center.";
    inline constexpr const char midiPitchBendChannelTooltip[] =
        "Channel - Chooses MIDI channel 1-16 for the displayed Pitch Bend lane. Changing the channel only changes the view.";
    inline constexpr const char midiPitchBendValueTooltip[] =
        "Value - Exact signed Pitch Bend value -8192 through +8191. 0 is center. Bend range in semitones is instrument/RPN dependent and is not interpreted here.";
    inline constexpr const char midiPitchBendGraphTooltip[] =
        "Graphical Pitch Bend Lane - Displays discrete bend state as staircase holds around the center line. Drag one point to edit tick/value; one completed drag is one Undo Bend transaction.";

    bool isValidMidiPitchBendEvent(const mw::core::MidiPitchBendEvent& event) noexcept;
    void sortMidiPitchBendEvents(std::vector<mw::core::MidiPitchBendEvent>& events);
    std::vector<mw::core::MidiPitchBendEvent> pitchBendLaneEvents(const mw::core::Track& track, int midiChannel);
    std::vector<mw::core::MidiPitchBendEvent> pitchBendStateAtOrBeforeTick(const mw::core::Track& track, std::int64_t startTick);
    std::size_t commitRecordedMidiPitchBendEvents(mw::core::Project& project, mw::core::StableId trackId,
                                                   const std::vector<mw::core::PersistentMidiPitchBendEvent>& recordedEvents);
    bool midiPitchBendEventVectorsEqual(const std::vector<mw::core::MidiPitchBendEvent>& left,
                                         const std::vector<mw::core::MidiPitchBendEvent>& right) noexcept;

    enum class MidiPitchBendSnapGrid { Off, Quarter, Eighth, Sixteenth, ThirtySecond, SixtyFourth };
    std::int64_t midiPitchBendSnapGridTicks(int ticksPerQuarterNote, MidiPitchBendSnapGrid grid) noexcept;
    std::int64_t snapMidiPitchBendTick(std::int64_t rawTick, int ticksPerQuarterNote, MidiPitchBendSnapGrid grid) noexcept;
    double midiPitchBendTickToX(std::int64_t tick, std::int64_t viewStartTick, double ticksPerPixel) noexcept;
    std::int64_t midiPitchBendXToTick(double x, std::int64_t viewStartTick, double ticksPerPixel) noexcept;
    double midiPitchBendValueToY(int value, double top, double height) noexcept;
    int midiPitchBendYToValue(double y, double top, double height) noexcept;
    mw::core::StableId hitTestMidiPitchBendPoint(const std::vector<mw::core::MidiPitchBendEvent>& events,
                                                  std::int64_t viewStartTick, double ticksPerPixel, double top, double height,
                                                  double x, double y, double radiusPixels) noexcept;

    struct MidiPitchBendStairStep
    {
        std::int64_t startTick = 0;
        std::int64_t endTick = 0;
        int value = 0;
        mw::core::StableId sourceEventId = 0;
    };
    std::vector<MidiPitchBendStairStep> pitchBendStaircaseSegments(const mw::core::Track& track, int midiChannel,
                                                                   std::int64_t viewStartTick, std::int64_t viewEndTick);

    struct MidiPitchBendEditResult
    {
        bool changed = false;
        bool replacedExisting = false;
        mw::core::StableId eventId = 0;
    };

    class MidiPitchBendEditHistory final
    {
    public:
        MidiPitchBendEditResult addAtTick(mw::core::Project& project, mw::core::StableId trackId, std::int64_t tick, int midiChannel, int value);
        bool moveToTick(mw::core::Project& project, mw::core::StableId trackId, mw::core::StableId eventId, std::int64_t tick);
        bool moveAndSetValue(mw::core::Project& project, mw::core::StableId trackId, mw::core::StableId eventId, std::int64_t tick, int value);
        bool setValue(mw::core::Project& project, mw::core::StableId trackId, mw::core::StableId eventId, int value);
        bool deleteEvent(mw::core::Project& project, mw::core::StableId trackId, mw::core::StableId eventId);
        bool undo(mw::core::Project& project);
        bool redo(mw::core::Project& project);
        bool canUndo() const noexcept { return !undoStack_.empty(); }
        bool canRedo() const noexcept { return !redoStack_.empty(); }
        mw::core::StableId lastAffectedTrackId() const noexcept { return lastAffectedTrackId_; }
        void clear() noexcept;

    private:
        struct Transaction
        {
            mw::core::StableId trackId = 0;
            mw::core::StableId eventId = 0;
            std::vector<mw::core::MidiPitchBendEvent> before;
            std::vector<mw::core::MidiPitchBendEvent> after;
        };
        static mw::core::Track* findEditableMidiTrack(mw::core::Project& project, mw::core::StableId trackId);
        void pushTransaction(Transaction transaction);
        bool restoreTransaction(mw::core::Project& project, const Transaction& transaction, bool useAfter);
        std::vector<Transaction> undoStack_;
        std::vector<Transaction> redoStack_;
        mw::core::StableId lastAffectedTrackId_ = 0;
    };
}
