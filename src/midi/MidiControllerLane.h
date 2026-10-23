#pragma once

#include "core/Project.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace mw::midi
{
    inline constexpr const char midiControllerLaneTooltip[] =
        "MIDI Controller Lane - Edits discrete MIDI Control Change events stored on the selected MIDI track. Choose a controller and MIDI channel to view its events. Controller events affect instrument playback but do not change recorded NoteEvent duration.";
    inline constexpr const char midiControllerSelectorTooltip[] =
        "Controller - Chooses the MIDI Control Change number shown by this lane. All CC numbers 0-127 are available; common controllers also show friendly names.";
    inline constexpr const char midiControllerChannelTooltip[] =
        "Channel - Chooses MIDI channel 1-16 for the displayed controller lane. Changing this selector only changes the view and does not edit or dirty the project.";
    inline constexpr const char midiControllerValueTooltip[] =
        "Value - MIDI Control Change value 0-127 used by Add at Preview or Set Value. Changing the value control alone does not edit the project.";
    inline constexpr const char midiControllerEventListTooltip[] =
        "Controller events - Discrete events for the selected track, channel, and controller, ordered by project tick. Event identity is stable even when a point moves and reorders.";
    inline constexpr const char midiControllerAddTooltip[] =
        "Add at Preview - Stores the selected controller/channel/value at the current Preview/transport tick. If that lane already has an event at the exact tick, its value is replaced without creating a duplicate point.";
    inline constexpr const char midiControllerMoveTooltip[] =
        "Move to Preview - Moves only the selected stable controller event to the current Preview/transport tick. Exact-tick lane collisions are resolved deterministically while retaining the moved event identity.";
    inline constexpr const char midiControllerSetValueTooltip[] =
        "Set Value - Changes only the selected controller event value to 0-127. Tick, channel, controller number, and stable event identity are retained.";
    inline constexpr const char midiControllerDeleteTooltip[] =
        "Delete - Removes only the selected stable controller event from this MIDI track.";
    inline constexpr const char midiControllerUndoTooltip[] =
        "Undo CC - Reverses the most recent controller Add, Move, Set Value, or Delete edit. This history is independent from Undo Take / Redo Take.";
    inline constexpr const char midiControllerRedoTooltip[] =
        "Redo CC - Reapplies the most recently undone controller edit. This history is independent from Undo Take / Redo Take.";
    inline constexpr const char midiControllerCc64Tooltip[] =
        "Project CC64 Sustain is ordinary MIDI controller playback data. It sends sustain messages to the instrument and does not rewrite stored NoteEvent duration. TAKE REVIEW Sustain Pedal = Apply CC64 remains separate retained-pass comp-duration interpretation.";
    inline constexpr const char midiControllerGraphTooltip[] =
        "Graphical Controller Lane - Displays discrete MIDI CC state as staircase holds. Click a point to select its stable event ID, then drag one point to edit tick/value. One completed drag is one Undo CC transaction; empty-canvas clicks only change selection.";
    inline constexpr const char midiControllerSnapTooltip[] =
        "Snap - Off preserves the exact integer project tick under the mouse. 1/4 through 1/64 snap only horizontal drag timing to the nearest musical grid; exact half-grid ties go forward. Changing Snap does not edit or dirty the project.";
    inline constexpr const char midiControllerZoomTooltip[] =
        "Zoom - Changes only the graphical horizontal time scale. Zooming and scrolling are view operations and do not edit or dirty the project.";

    std::string midiControllerDisplayName(int controllerNumber);
    bool isValidMidiControllerEvent(const mw::core::MidiControllerEvent& event) noexcept;
    void sortMidiControllerEvents(std::vector<mw::core::MidiControllerEvent>& events);
    std::vector<mw::core::MidiControllerEvent> controllerLaneEvents(const mw::core::Track& track, int midiChannel, int controllerNumber);
    std::vector<mw::core::MidiControllerEvent> controllerStateAtOrBeforeTick(const mw::core::Track& track, std::int64_t startTick);
    std::size_t commitRecordedMidiControllerEvents(
        mw::core::Project& project,
        mw::core::StableId trackId,
        const std::vector<mw::core::PersistentMidiControllerEvent>& recordedEvents);
    bool midiControllerEventVectorsEqual(
        const std::vector<mw::core::MidiControllerEvent>& left,
        const std::vector<mw::core::MidiControllerEvent>& right) noexcept;

    enum class MidiControllerLaneUiAction
    {
        OpenWindow,
        CloseWindow,
        SelectTrack,
        SelectController,
        SelectChannel,
        SelectEvent,
        Scroll,
        HoverHelp,
        SelectGraphPoint,
        PanGraph,
        ZoomGraph,
        SelectSnap,
        CancelGraphDrag,
        GraphDragCommit,
        AddAtPreview,
        MoveToPreview,
        SetValue,
        DeleteEvent,
        UndoCc,
        RedoCc
    };


    enum class MidiControllerSnapGrid
    {
        Off,
        Quarter,
        Eighth,
        Sixteenth,
        ThirtySecond,
        SixtyFourth
    };

    struct MidiControllerStairStep
    {
        std::int64_t startTick = 0;
        std::int64_t endTick = 0;
        int value = 0;
        mw::core::StableId sourceEventId = 0;
    };

    std::int64_t midiControllerSnapGridTicks(int ticksPerQuarterNote, MidiControllerSnapGrid grid) noexcept;
    std::int64_t snapMidiControllerTick(std::int64_t rawTick, int ticksPerQuarterNote, MidiControllerSnapGrid grid) noexcept;
    double midiControllerTickToX(std::int64_t tick, std::int64_t viewStartTick, double ticksPerPixel) noexcept;
    std::int64_t midiControllerXToTick(double x, std::int64_t viewStartTick, double ticksPerPixel) noexcept;
    double midiControllerValueToY(int value, double top, double height) noexcept;
    int midiControllerYToValue(double y, double top, double height) noexcept;
    mw::core::StableId hitTestMidiControllerPoint(const std::vector<mw::core::MidiControllerEvent>& events,
                                                   std::int64_t viewStartTick, double ticksPerPixel, double top, double height,
                                                   double x, double y, double radiusPixels) noexcept;
    std::vector<MidiControllerStairStep> controllerStaircaseSegments(const mw::core::Track& track, int midiChannel, int controllerNumber,
                                                                     std::int64_t viewStartTick, std::int64_t viewEndTick);

    bool midiControllerLaneUiActionMayDirty(MidiControllerLaneUiAction action) noexcept;
    mw::core::StableId reconcileMidiControllerLaneSelection(const mw::core::Track& track, int midiChannel, int controllerNumber, mw::core::StableId selectedEventId) noexcept;

    struct MidiControllerEditResult
    {
        bool changed = false;
        bool replacedExisting = false;
        mw::core::StableId eventId = 0;
    };

    class MidiControllerEditHistory final
    {
    public:
        MidiControllerEditResult addAtTick(mw::core::Project& project, mw::core::StableId trackId, std::int64_t tick, int midiChannel, int controllerNumber, int value);
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
            std::vector<mw::core::MidiControllerEvent> before;
            std::vector<mw::core::MidiControllerEvent> after;
        };
        static mw::core::Track* findEditableMidiTrack(mw::core::Project& project, mw::core::StableId trackId);
        void pushTransaction(Transaction transaction);
        bool restoreTransaction(mw::core::Project& project, const Transaction& transaction, bool useAfter);
        std::vector<Transaction> undoStack_;
        std::vector<Transaction> redoStack_;
        mw::core::StableId lastAffectedTrackId_ = 0;
    };
}
