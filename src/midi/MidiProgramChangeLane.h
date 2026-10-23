#pragma once

#include "core/Project.h"

#include <cstdint>
#include <vector>

namespace mw::midi
{
    inline constexpr const char midiProgramChangeLaneTooltip[] =
        "MIDI Program Change Lane - Edits discrete channel Program Change events on the selected MIDI track. Programs use the exact MIDI range 0 through 127.";
    inline constexpr const char midiProgramChangeChannelTooltip[] =
        "Channel - Chooses MIDI channel 1-16 for the displayed Program Change lane. Changing the channel only changes the view.";
    inline constexpr const char midiProgramChangeProgramTooltip[] =
        "Program - Exact MIDI Program Change value 0 through 127. Some devices display these as patches 1 through 128; Poor Man's Studio stores the actual MIDI value.";
    inline constexpr const char midiProgramChangeGraphTooltip[] =
        "Graphical Program Change Lane - Displays discrete program state as staircase holds. Drag one point to edit tick/program; one completed drag is one Undo Program transaction.";

    bool isValidMidiProgramChangeEvent(const mw::core::MidiProgramChangeEvent& event) noexcept;
    void sortMidiProgramChangeEvents(std::vector<mw::core::MidiProgramChangeEvent>& events);
    std::vector<mw::core::MidiProgramChangeEvent> programChangeLaneEvents(const mw::core::Track& track, int midiChannel);
    std::vector<mw::core::MidiProgramChangeEvent> programChangeStateAtOrBeforeTick(const mw::core::Track& track, std::int64_t startTick);
    std::size_t commitRecordedMidiProgramChangeEvents(mw::core::Project& project, mw::core::StableId trackId,
                                                       const std::vector<mw::core::PersistentMidiProgramChangeEvent>& recordedEvents);
    bool midiProgramChangeEventVectorsEqual(const std::vector<mw::core::MidiProgramChangeEvent>& left,
                                             const std::vector<mw::core::MidiProgramChangeEvent>& right) noexcept;

    enum class MidiProgramChangeSnapGrid { Off, Quarter, Eighth, Sixteenth, ThirtySecond, SixtyFourth };
    std::int64_t midiProgramChangeSnapGridTicks(int ticksPerQuarterNote, MidiProgramChangeSnapGrid grid) noexcept;
    std::int64_t snapMidiProgramChangeTick(std::int64_t rawTick, int ticksPerQuarterNote, MidiProgramChangeSnapGrid grid) noexcept;
    double midiProgramChangeTickToX(std::int64_t tick, std::int64_t viewStartTick, double ticksPerPixel) noexcept;
    std::int64_t midiProgramChangeXToTick(double x, std::int64_t viewStartTick, double ticksPerPixel) noexcept;
    double midiProgramChangeProgramToY(int program, double top, double height) noexcept;
    int midiProgramChangeYToProgram(double y, double top, double height) noexcept;
    mw::core::StableId hitTestMidiProgramChangePoint(const std::vector<mw::core::MidiProgramChangeEvent>& events,
                                                      std::int64_t viewStartTick, double ticksPerPixel, double top, double height,
                                                      double x, double y, double radiusPixels) noexcept;

    struct MidiProgramChangeStairStep
    {
        std::int64_t startTick = 0;
        std::int64_t endTick = 0;
        int program = 0;
        mw::core::StableId sourceEventId = 0;
    };
    std::vector<MidiProgramChangeStairStep> programChangeStaircaseSegments(const mw::core::Track& track, int midiChannel,
                                                                           std::int64_t viewStartTick, std::int64_t viewEndTick);

    struct MidiProgramChangeEditResult
    {
        bool changed = false;
        bool replacedExisting = false;
        mw::core::StableId eventId = 0;
    };

    class MidiProgramChangeEditHistory final
    {
    public:
        MidiProgramChangeEditResult addAtTick(mw::core::Project& project, mw::core::StableId trackId, std::int64_t tick, int midiChannel, int program);
        bool moveToTick(mw::core::Project& project, mw::core::StableId trackId, mw::core::StableId eventId, std::int64_t tick);
        bool moveAndSetProgram(mw::core::Project& project, mw::core::StableId trackId, mw::core::StableId eventId, std::int64_t tick, int program);
        bool setProgram(mw::core::Project& project, mw::core::StableId trackId, mw::core::StableId eventId, int program);
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
            std::vector<mw::core::MidiProgramChangeEvent> before;
            std::vector<mw::core::MidiProgramChangeEvent> after;
        };
        static mw::core::Track* findEditableMidiTrack(mw::core::Project& project, mw::core::StableId trackId);
        void pushTransaction(Transaction transaction);
        bool restoreTransaction(mw::core::Project& project, const Transaction& transaction, bool useAfter);
        std::vector<Transaction> undoStack_;
        std::vector<Transaction> redoStack_;
        mw::core::StableId lastAffectedTrackId_ = 0;
    };
}
