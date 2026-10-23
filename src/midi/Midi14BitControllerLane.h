#pragma once

#include "core/Project.h"

#include <cstdint>
#include <vector>

namespace mw::midi
{
    inline constexpr const char midi14BitControllerLaneTooltip[] =
        "MIDI 14-bit Controller Lane - Edits semantic paired CC values for controllers 1-31 (LSB 33-63). CC0/32 remains Bank Select.";
    inline constexpr const char midi14BitControllerChannelTooltip[] =
        "Channel - Chooses MIDI channel 1-16 for the displayed 14-bit Controller Lane. Changing the channel changes only the view.";
    inline constexpr const char midi14BitControllerControllerTooltip[] =
        "Controller MSB - Chooses CC1-31. The matching LSB is CC33-63. CC0/32 remains reserved for Bank Select.";
    inline constexpr const char midi14BitControllerValueTooltip[] =
        "Value - Exact paired-controller value 0 through 16383.";
    inline constexpr const char midi14BitControllerGraphTooltip[] =
        "Graphical 14-bit Controller Lane - Displays discrete paired-CC state as staircase holds. Drag one point to edit tick/value; one completed drag is one Undo 14-bit CC transaction.";

    bool isValidMidi14BitControllerEvent(const mw::core::Midi14BitControllerEvent& event) noexcept;
    void sortMidi14BitControllerEvents(std::vector<mw::core::Midi14BitControllerEvent>& events);
    std::vector<mw::core::Midi14BitControllerEvent> midi14BitControllerLaneEvents(
        const mw::core::Track& track, int midiChannel, int controllerMsb);
    std::vector<mw::core::Midi14BitControllerEvent> midi14BitControllerStateAtOrBeforeTick(
        const mw::core::Track& track, std::int64_t startTick);
    std::size_t commitRecordedMidi14BitControllerEvents(
        mw::core::Project& project, mw::core::StableId trackId,
        const std::vector<mw::core::PersistentMidi14BitControllerEvent>& recordedEvents);
    bool midi14BitControllerEventVectorsEqual(
        const std::vector<mw::core::Midi14BitControllerEvent>& left,
        const std::vector<mw::core::Midi14BitControllerEvent>& right) noexcept;

    enum class Midi14BitSnapGrid { Off, Quarter, Eighth, Sixteenth, ThirtySecond, SixtyFourth };
    std::int64_t midi14BitSnapGridTicks(int ticksPerQuarterNote, Midi14BitSnapGrid grid) noexcept;
    std::int64_t snapMidi14BitTick(std::int64_t rawTick, int ticksPerQuarterNote, Midi14BitSnapGrid grid) noexcept;
    double midi14BitValueToY(int value, double top, double height) noexcept;
    int midi14BitYToValue(double y, double top, double height) noexcept;

    // Graph/UI helpers retain the concise core names above while exposing lane-specific aliases.
    using Midi14BitControllerSnapGrid = Midi14BitSnapGrid;
    std::int64_t midi14BitControllerSnapGridTicks(int ticksPerQuarterNote, Midi14BitControllerSnapGrid grid) noexcept;
    std::int64_t snapMidi14BitControllerTick(std::int64_t rawTick, int ticksPerQuarterNote, Midi14BitControllerSnapGrid grid) noexcept;
    double midi14BitControllerTickToX(std::int64_t tick, std::int64_t viewStartTick, double ticksPerPixel) noexcept;
    std::int64_t midi14BitControllerXToTick(double x, std::int64_t viewStartTick, double ticksPerPixel) noexcept;
    double midi14BitControllerValueToY(int value, double top, double height) noexcept;
    int midi14BitControllerYToValue(double y, double top, double height) noexcept;
    mw::core::StableId hitTestMidi14BitControllerPoint(const std::vector<mw::core::Midi14BitControllerEvent>& events,
                                                        std::int64_t viewStartTick, double ticksPerPixel, double top, double height,
                                                        double x, double y, double radiusPixels) noexcept;

    struct Midi14BitControllerStairStep
    {
        std::int64_t startTick = 0;
        std::int64_t endTick = 0;
        int value = 0;
        mw::core::StableId sourceEventId = 0;
    };
    std::vector<Midi14BitControllerStairStep> midi14BitControllerStaircaseSegments(
        const mw::core::Track& track, int midiChannel, int controllerMsb,
        std::int64_t viewStartTick, std::int64_t viewEndTick);

    struct Midi14BitEditResult
    {
        bool changed = false;
        bool replacedExisting = false;
        mw::core::StableId eventId = 0;
    };

    class Midi14BitControllerEditHistory final
    {
    public:
        Midi14BitEditResult addAtTick(mw::core::Project& project, mw::core::StableId trackId,
                                      std::int64_t tick, int midiChannel, int controllerMsb, int value);
        bool moveToTick(mw::core::Project& project, mw::core::StableId trackId,
                        mw::core::StableId eventId, std::int64_t tick);
        bool moveAndSetValue(mw::core::Project& project, mw::core::StableId trackId,
                             mw::core::StableId eventId, std::int64_t tick, int value);
        bool setValue(mw::core::Project& project, mw::core::StableId trackId,
                      mw::core::StableId eventId, int value);
        bool deleteEvent(mw::core::Project& project, mw::core::StableId trackId,
                         mw::core::StableId eventId);
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
            std::vector<mw::core::Midi14BitControllerEvent> before;
            std::vector<mw::core::Midi14BitControllerEvent> after;
        };
        static mw::core::Track* findTrack(mw::core::Project& project, mw::core::StableId trackId);
        void push(Transaction transaction);
        bool restore(mw::core::Project& project, const Transaction& transaction, bool after);
        std::vector<Transaction> undoStack_;
        std::vector<Transaction> redoStack_;
        mw::core::StableId lastAffectedTrackId_ = 0;
    };
}
