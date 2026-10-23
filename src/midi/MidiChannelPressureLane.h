#pragma once

#include "core/Project.h"

#include <cstdint>
#include <vector>

namespace mw::midi
{
    inline constexpr const char midiChannelPressureLaneTooltip[] =
        "MIDI Channel Pressure Lane - Edits discrete Channel Pressure events on the selected MIDI track. Pressures use the exact MIDI range 0 through 127.";
    inline constexpr const char midiChannelPressureChannelTooltip[] =
        "Channel - Chooses MIDI channel 1-16 for the displayed Channel Pressure lane. Changing the channel only changes the view.";
    inline constexpr const char midiChannelPressurePressureTooltip[] =
        "Pressure - Exact MIDI Channel Pressure value 0 through 127. Zero is the released/baseline value.";
    inline constexpr const char midiChannelPressureGraphTooltip[] =
        "Graphical Channel Pressure Lane - Displays discrete pressure state as staircase holds. Drag one point to edit tick/pressure; one completed drag is one Undo Pressure transaction.";

    bool isValidMidiChannelPressureEvent(const mw::core::MidiChannelPressureEvent& event) noexcept;
    void sortMidiChannelPressureEvents(std::vector<mw::core::MidiChannelPressureEvent>& events);
    std::vector<mw::core::MidiChannelPressureEvent> channelPressureLaneEvents(const mw::core::Track& track, int midiChannel);
    std::vector<mw::core::MidiChannelPressureEvent> channelPressureStateAtOrBeforeTick(const mw::core::Track& track, std::int64_t startTick);
    std::size_t commitRecordedMidiChannelPressureEvents(mw::core::Project& project, mw::core::StableId trackId,
                                                       const std::vector<mw::core::PersistentMidiChannelPressureEvent>& recordedEvents);
    bool midiChannelPressureEventVectorsEqual(const std::vector<mw::core::MidiChannelPressureEvent>& left,
                                             const std::vector<mw::core::MidiChannelPressureEvent>& right) noexcept;

    enum class MidiChannelPressureSnapGrid { Off, Quarter, Eighth, Sixteenth, ThirtySecond, SixtyFourth };
    std::int64_t midiChannelPressureSnapGridTicks(int ticksPerQuarterNote, MidiChannelPressureSnapGrid grid) noexcept;
    std::int64_t snapMidiChannelPressureTick(std::int64_t rawTick, int ticksPerQuarterNote, MidiChannelPressureSnapGrid grid) noexcept;
    double midiChannelPressureTickToX(std::int64_t tick, std::int64_t viewStartTick, double ticksPerPixel) noexcept;
    std::int64_t midiChannelPressureXToTick(double x, std::int64_t viewStartTick, double ticksPerPixel) noexcept;
    double midiChannelPressurePressureToY(int pressure, double top, double height) noexcept;
    int midiChannelPressureYToPressure(double y, double top, double height) noexcept;
    mw::core::StableId hitTestMidiChannelPressurePoint(const std::vector<mw::core::MidiChannelPressureEvent>& events,
                                                      std::int64_t viewStartTick, double ticksPerPixel, double top, double height,
                                                      double x, double y, double radiusPixels) noexcept;

    struct MidiChannelPressureStairStep
    {
        std::int64_t startTick = 0;
        std::int64_t endTick = 0;
        int pressure = 0;
        mw::core::StableId sourceEventId = 0;
    };
    std::vector<MidiChannelPressureStairStep> channelPressureStaircaseSegments(const mw::core::Track& track, int midiChannel,
                                                                           std::int64_t viewStartTick, std::int64_t viewEndTick);

    struct MidiChannelPressureEditResult
    {
        bool changed = false;
        bool replacedExisting = false;
        mw::core::StableId eventId = 0;
    };

    class MidiChannelPressureEditHistory final
    {
    public:
        MidiChannelPressureEditResult addAtTick(mw::core::Project& project, mw::core::StableId trackId, std::int64_t tick, int midiChannel, int pressure);
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
            std::vector<mw::core::MidiChannelPressureEvent> before;
            std::vector<mw::core::MidiChannelPressureEvent> after;
        };
        static mw::core::Track* findEditableMidiTrack(mw::core::Project& project, mw::core::StableId trackId);
        void pushTransaction(Transaction transaction);
        bool restoreTransaction(mw::core::Project& project, const Transaction& transaction, bool useAfter);
        std::vector<Transaction> undoStack_;
        std::vector<Transaction> redoStack_;
        mw::core::StableId lastAffectedTrackId_ = 0;
    };
}
