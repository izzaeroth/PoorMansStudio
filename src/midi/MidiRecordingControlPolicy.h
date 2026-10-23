#pragma once

namespace mw::midi
{
    enum class MidiRecordingControlState
    {
        Idle,
        CountIn,
        Preparing,
        PunchPreRoll,
        Recording
    };

    struct MidiRecordingControlContext
    {
        MidiRecordingControlState state = MidiRecordingControlState::Idle;
        bool armedTrackAvailable = false;
        bool midiInputOpen = false;
        bool liveMonitoringActive = false;
        bool deviceSelectionAvailable = false;
        bool canUndoTake = false;
        bool canRedoTake = false;
        bool recordingConfigurationValid = true;
        bool punchPositionAvailable = false;
        bool allowLoopWrapTestWithoutMidi = false;
    };

    struct MidiRecordingControlAvailability
    {
        bool deviceSelectorEnabled = false;
        bool channelSelectorEnabled = false;
        bool refreshDevicesEnabled = false;
        bool applySelectionEnabled = false;
        bool startMonitorEnabled = false;
        bool stopMonitorEnabled = false;
        bool armSelectedEnabled = false;
        bool recordMidiEnabled = false;
        bool stopRecordingEnabled = false;
        bool panicEnabled = false;
        bool countInEnabled = false;
        bool quantizeEnabled = false;
        bool metronomeEnabled = false;
        bool punchModeEnabled = false;
        bool loopModeEnabled = false;
        bool punchSetInEnabled = false;
        bool punchSetOutEnabled = false;
        bool punchClearEnabled = false;
        bool undoTakeEnabled = false;
        bool redoTakeEnabled = false;
        bool closeEnabled = false;
    };

    MidiRecordingControlState midiRecordingControlState(
        bool recording,
        bool preparing,
        bool countingIn,
        bool punchPreRoll = false) noexcept;

    bool isMidiRecordingControlBusy(MidiRecordingControlState state) noexcept;
    const char* midiRecordingControlStateLabel(MidiRecordingControlState state) noexcept;

    MidiRecordingControlAvailability makeMidiRecordingControlAvailability(
        const MidiRecordingControlContext& context) noexcept;
}
