#pragma once

#include "core/Project.h"

#include <cstdint>
#include <vector>

namespace mw::midi
{
    inline constexpr const char midiPolyPressureLaneTooltip[] =
        "MIDI Poly Pressure Lane - Edits discrete Poly Pressure events on the selected MIDI track. Pressures use the exact MIDI range 0 through 127.";
    inline constexpr const char midiPolyPressureChannelTooltip[] =
        "Channel - Chooses MIDI channel 1-16 for the displayed Poly Pressure lane. Changing the channel only changes the view.";
    inline constexpr const char midiPolyPressureKeyTooltip[] =
        "Key - Chooses MIDI note number 0-127 for the displayed Poly Pressure lane. Poly Pressure is channel+key performance data, not tied to a NoteEvent stable ID.";
    inline constexpr const char midiPolyPressurePressureTooltip[] =
        "Pressure - Exact MIDI Poly Pressure value 0 through 127. Zero is the released/baseline value.";
    inline constexpr const char midiPolyPressureGraphTooltip[] =
        "Graphical Poly Pressure Lane - Displays discrete pressure state as staircase holds. Drag one point to edit tick/pressure; one completed drag is one Undo Poly Pressure transaction.";

    bool isValidMidiPolyPressureEvent(const mw::core::MidiPolyPressureEvent& event) noexcept;
    void sortMidiPolyPressureEvents(std::vector<mw::core::MidiPolyPressureEvent>& events);
    std::vector<mw::core::MidiPolyPressureEvent> polyPressureLaneEvents(const mw::core::Track& track, int midiChannel, int noteNumber);
    std::vector<mw::core::MidiPolyPressureEvent> polyPressureStateAtOrBeforeTick(const mw::core::Track& track, std::int64_t startTick);
    std::size_t commitRecordedMidiPolyPressureEvents(mw::core::Project& project, mw::core::StableId trackId,
                                                       const std::vector<mw::core::PersistentMidiPolyPressureEvent>& recordedEvents);
    bool midiPolyPressureEventVectorsEqual(const std::vector<mw::core::MidiPolyPressureEvent>& left,
                                             const std::vector<mw::core::MidiPolyPressureEvent>& right) noexcept;

    enum class MidiPolyPressureSnapGrid { Off, Quarter, Eighth, Sixteenth, ThirtySecond, SixtyFourth };
    std::int64_t midiPolyPressureSnapGridTicks(int ticksPerQuarterNote, MidiPolyPressureSnapGrid grid) noexcept;
    std::int64_t snapMidiPolyPressureTick(std::int64_t rawTick, int ticksPerQuarterNote, MidiPolyPressureSnapGrid grid) noexcept;
    double midiPolyPressureTickToX(std::int64_t tick, std::int64_t viewStartTick, double ticksPerPixel) noexcept;
    std::int64_t midiPolyPressureXToTick(double x, std::int64_t viewStartTick, double ticksPerPixel) noexcept;
    double midiPolyPressurePressureToY(int pressure, double top, double height) noexcept;
    int midiPolyPressureYToPressure(double y, double top, double height) noexcept;
    mw::core::StableId hitTestMidiPolyPressurePoint(const std::vector<mw::core::MidiPolyPressureEvent>& events,
                                                      std::int64_t viewStartTick, double ticksPerPixel, double top, double height,
                                                      double x, double y, double radiusPixels) noexcept;

    struct MidiPolyPressureStairStep
    {
        std::int64_t startTick = 0;
        std::int64_t endTick = 0;
        int pressure = 0;
        mw::core::StableId sourceEventId = 0;
    };
    std::vector<MidiPolyPressureStairStep> polyPressureStaircaseSegments(const mw::core::Track& track, int midiChannel, int noteNumber,
                                                                           std::int64_t viewStartTick, std::int64_t viewEndTick);

    struct MidiPolyPressureEditResult
    {
        bool changed = false;
        bool replacedExisting = false;
        mw::core::StableId eventId = 0;
    };

    class MidiPolyPressureEditHistory final
    {
    public:
        MidiPolyPressureEditResult addAtTick(mw::core::Project& project, mw::core::StableId trackId, std::int64_t tick, int midiChannel, int noteNumber, int pressure);
        bool moveToTick(mw::core::Project& project, mw::core::StableId trackId, mw::core::StableId eventId, std::int64_t tick);
        bool moveAndSetPressure(mw::core::Project& project, mw::core::StableId trackId, mw::core::StableId eventId, std::int64_t tick, int pressure);
        bool setPressure(mw::core::Project& project, mw::core::StableId trackId, mw::core::StableId eventId, int pressure);
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
            std::vector<mw::core::MidiPolyPressureEvent> before;
            std::vector<mw::core::MidiPolyPressureEvent> after;
        };
        static mw::core::Track* findEditableMidiTrack(mw::core::Project& project, mw::core::StableId trackId);
        void pushTransaction(Transaction transaction);
        bool restoreTransaction(mw::core::Project& project, const Transaction& transaction, bool useAfter);
        std::vector<Transaction> undoStack_;
        std::vector<Transaction> redoStack_;
        mw::core::StableId lastAffectedTrackId_ = 0;
    };
}
