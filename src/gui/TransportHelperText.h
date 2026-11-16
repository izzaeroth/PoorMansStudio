#pragma once

// Centralized plain-language descriptions for recording controls. Keep this
// header independent of JUCE so deterministic tests can guard the actual text.
namespace mw::gui
{
    inline constexpr char midiRecordLoopHelp[] =
        "Loop Record repeats the selected [In, Out) region and keeps passes separate until Merge or Take Review. "
        "Mixed-live backing may wrap inside an audio callback; rendered playback uses its established restart path. "
        "If MIDI timestamps, wrap notices, or device timing become uncertain, capture stops safely rather than assigning an event to the wrong pass.";
    inline constexpr char midiRecordPunchHelp[] =
        "Punch records events from the exact In tick up to, but not including, the Out tick. "
        "Existing material outside the region stays unchanged; Count-In does not record before capture begins.";
    inline constexpr char midiRecordCountInHelp[] =
        "Count-In plays 0, 1 or 2 bars of metronome lead-in before recording. "
        "MIDI can still be monitored during the lead-in but is not captured until recording starts.";
    inline constexpr char midiRecordQuantizeHelp[] =
        "Note Quantize adjusts recorded note starts to the selected grid after capture. "
        "Off keeps exact ticks; Controller, Bend, Pressure, Program and Advanced Parameter events keep their exact capture timing.";
    inline constexpr char midiRecordMetronomeHelp[] =
        "Metronome clicks follow the project beat/bar grid during MIDI recording. "
        "The first beat of each bar is accented. This option does not alter recorded MIDI timing.";
    inline constexpr char midiRecordStopHelp[] =
        "Stop Recording closes incoming MIDI capture, drains safely ordered messages, and finalizes the take. "
        "If a callback wrap or timestamp cannot be verified, it stops rather than guessing pass ownership.";
    inline constexpr char midiRecordStartMonitorHelp[] =
        "Start Monitor routes live MIDI to the selected VST3/CLAP instrument without recording. "
        "Use Arm Selected and Record MIDI to capture a take.";
    inline constexpr char midiRecordPanicHelp[] =
        "Panic requests note-off for held notes. It does not automatically discard a Take Review or stop a running recording.";
    inline constexpr char midiRecordArmHelp[] =
        "Arm Selected fixes the stable MIDI track targeted by the next recording. "
        "Changing the visible selection later does not silently retarget an active take.";
    inline constexpr char midiRecordStartHelp[] =
        "Record MIDI begins a take on the armed track. Loop Record needs a valid [In, Out) region. "
        "Without a MIDI device, supported backing-wrap diagnostics do not create notes or Undo history.";
}
