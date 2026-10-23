#pragma once

#include "core/Project.h"

#include <cstdint>
#include <vector>

namespace mw::midi
{
    enum class MidiParameterMode { Rpn, Nrpn };

    inline constexpr const char midiParameterLaneTooltip[] =
        "MIDI Parameter Lane - Edits semantic RPN or NRPN parameter/value transactions as one project event. Playback/export expands them to standard CC selection and Data Entry messages.";
    inline constexpr const char midiParameterModeTooltip[] =
        "Mode - Chooses RPN or NRPN. RPN 16383 (127/127) is the MIDI Null Function and is not stored as an authored RPN parameter.";
    inline constexpr const char midiParameterChannelTooltip[] =
        "Channel - Chooses MIDI channel 1-16 for the displayed Parameter Lane. Changing the channel changes only the view.";
    inline constexpr const char midiParameterParameterTooltip[] =
        "Parameter - Exact 14-bit parameter number 0 through 16383. RPN 16383 is reserved as the Null Function; NRPN may use the full range.";
    inline constexpr const char midiParameterValueTooltip[] =
        "Value - Exact 14-bit Data Entry value 0 through 16383.";

    bool isValidMidiRpnEvent(const mw::core::MidiRpnEvent& event) noexcept;
    bool isValidMidiNrpnEvent(const mw::core::MidiNrpnEvent& event) noexcept;
    std::vector<mw::core::MidiRpnEvent> midiRpnLaneEvents(const mw::core::Track& track, int midiChannel);
    std::vector<mw::core::MidiNrpnEvent> midiNrpnLaneEvents(const mw::core::Track& track, int midiChannel);
    std::vector<mw::core::MidiRpnEvent> midiRpnStateAtOrBeforeTick(const mw::core::Track& track, std::int64_t startTick);
    std::vector<mw::core::MidiNrpnEvent> midiNrpnStateAtOrBeforeTick(const mw::core::Track& track, std::int64_t startTick);
    bool midiRpnEventVectorsEqual(const std::vector<mw::core::MidiRpnEvent>& a,
                                  const std::vector<mw::core::MidiRpnEvent>& b) noexcept;
    bool midiNrpnEventVectorsEqual(const std::vector<mw::core::MidiNrpnEvent>& a,
                                   const std::vector<mw::core::MidiNrpnEvent>& b) noexcept;
    std::size_t commitRecordedMidiRpnEvents(mw::core::Project& project, mw::core::StableId trackId,
                                            const std::vector<mw::core::PersistentMidiRpnEvent>& events);
    std::size_t commitRecordedMidiNrpnEvents(mw::core::Project& project, mw::core::StableId trackId,
                                             const std::vector<mw::core::PersistentMidiNrpnEvent>& events);


    enum class MidiParameterSnapGrid { Off, Quarter, Eighth, Sixteenth, ThirtySecond, SixtyFourth };
    std::int64_t midiParameterSnapGridTicks(int ticksPerQuarterNote, MidiParameterSnapGrid grid) noexcept;
    std::int64_t snapMidiParameterTick(std::int64_t rawTick, int ticksPerQuarterNote, MidiParameterSnapGrid grid) noexcept;

    struct MidiParameterEditResult { bool changed = false; bool replacedExisting = false; mw::core::StableId eventId = 0; };

    class MidiParameterEditHistory final
    {
    public:
        MidiParameterEditResult addAtTick(mw::core::Project& project, mw::core::StableId trackId, MidiParameterMode mode,
                                          std::int64_t tick, int midiChannel, int parameter, int value);
        bool moveToTick(mw::core::Project& project, mw::core::StableId trackId, MidiParameterMode mode,
                        mw::core::StableId eventId, std::int64_t tick);
        bool setParameter(mw::core::Project& project, mw::core::StableId trackId, MidiParameterMode mode,
                          mw::core::StableId eventId, int parameter);
        bool setValue(mw::core::Project& project, mw::core::StableId trackId, MidiParameterMode mode,
                      mw::core::StableId eventId, int value);
        bool deleteEvent(mw::core::Project& project, mw::core::StableId trackId, MidiParameterMode mode,
                         mw::core::StableId eventId);
        bool undo(mw::core::Project& project);
        bool redo(mw::core::Project& project);
        bool canUndo() const noexcept { return !undoStack_.empty(); }
        bool canRedo() const noexcept { return !redoStack_.empty(); }
        void clear() noexcept;
    private:
        struct Transaction
        {
            mw::core::StableId trackId = 0;
            MidiParameterMode mode = MidiParameterMode::Rpn;
            std::vector<mw::core::MidiRpnEvent> rpnBefore, rpnAfter;
            std::vector<mw::core::MidiNrpnEvent> nrpnBefore, nrpnAfter;
        };
        static mw::core::Track* findTrack(mw::core::Project& project, mw::core::StableId trackId);
        void push(Transaction transaction);
        bool restore(mw::core::Project& project, const Transaction& transaction, bool after);
        std::vector<Transaction> undoStack_, redoStack_;
    };
}
