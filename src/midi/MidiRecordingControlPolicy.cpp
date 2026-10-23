#include "midi/MidiRecordingControlPolicy.h"

namespace mw::midi
{
    MidiRecordingControlState midiRecordingControlState(
        bool recording,
        bool preparing,
        bool countingIn,
        bool punchPreRoll) noexcept
    {
        if (recording)
            return MidiRecordingControlState::Recording;
        if (countingIn)
            return MidiRecordingControlState::CountIn;
        if (preparing)
            return MidiRecordingControlState::Preparing;
        if (punchPreRoll)
            return MidiRecordingControlState::PunchPreRoll;
        return MidiRecordingControlState::Idle;
    }

    bool isMidiRecordingControlBusy(MidiRecordingControlState state) noexcept
    {
        return state != MidiRecordingControlState::Idle;
    }

    const char* midiRecordingControlStateLabel(MidiRecordingControlState state) noexcept
    {
        switch (state)
        {
            case MidiRecordingControlState::CountIn: return "COUNT-IN";
            case MidiRecordingControlState::Preparing: return "PREPARING PLAYBACK";
            case MidiRecordingControlState::PunchPreRoll: return "PUNCH PRE-ROLL";
            case MidiRecordingControlState::Recording: return "RECORDING";
            case MidiRecordingControlState::Idle:
            default: return "IDLE";
        }
    }

    MidiRecordingControlAvailability makeMidiRecordingControlAvailability(
        const MidiRecordingControlContext& context) noexcept
    {
        MidiRecordingControlAvailability result;
        const bool busy = isMidiRecordingControlBusy(context.state);

        if (busy)
        {
            // During count-in, playback preparation, and capture, the recording route
            // and take options are intentionally frozen. Panic and Stop Recording are
            // the two explicit safety controls that remain available.
            result.stopRecordingEnabled = true;
            result.panicEnabled = true;
            return result;
        }

        result.deviceSelectorEnabled = true;
        result.channelSelectorEnabled = true;
        result.refreshDevicesEnabled = true;
        result.applySelectionEnabled = context.deviceSelectionAvailable;
        result.startMonitorEnabled = context.deviceSelectionAvailable && !context.liveMonitoringActive;
        result.stopMonitorEnabled = context.liveMonitoringActive;
        result.armSelectedEnabled = true;
        result.recordMidiEnabled = context.armedTrackAvailable
            && (context.midiInputOpen || context.allowLoopWrapTestWithoutMidi)
            && context.recordingConfigurationValid;
        result.stopRecordingEnabled = false;
        result.panicEnabled = context.liveMonitoringActive;
        result.countInEnabled = true;
        result.quantizeEnabled = true;
        result.metronomeEnabled = true;
        result.punchModeEnabled = true;
        result.loopModeEnabled = true;
        result.punchSetInEnabled = context.punchPositionAvailable;
        result.punchSetOutEnabled = context.punchPositionAvailable;
        result.punchClearEnabled = true;
        result.undoTakeEnabled = context.canUndoTake;
        result.redoTakeEnabled = context.canRedoTake;
        result.closeEnabled = true;
        return result;
    }
}
