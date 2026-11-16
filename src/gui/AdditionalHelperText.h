#pragma once

// JUCE-independent, verified helper-bubble text for v0.68.7 GUI polish.
// Keep descriptions paired with the actual controls in MainComponent.
namespace mw::gui
{
    inline constexpr char pluginGraphicsAdapterHelp[] =
        "Choose the preferred graphics adapter for VST3 and CLAP plugin editor windows. Some plugins or drivers may ignore this preference; it does not select the audio device.";
    inline constexpr char pluginEditorWindowLimitHelp[] =
        "Limit how many plugin editor windows may be open at once. Four is recommended; the hard cap is sixteen. This setting does not limit inserted audio effects.";
    inline constexpr char pluginRefreshAdapterHelp[] =
        "Refresh the available graphics-adapter list for plugin windows. This does not scan installed VST3 or CLAP plugins.";
    inline constexpr char audioDriverHelp[] =
        "Select the Windows audio driver type used by the shared JUCE audio service. Changing drivers can replace available input/output devices and requires applying the configuration.";
    inline constexpr char audioOutputDeviceHelp[] =
        "Select where playback and monitoring audio is sent. Apply / Save to activate the device; changing it safely stops active realtime playback or recording.";
    inline constexpr char audioInputDeviceHelp[] =
        "Select the physical device used for microphone or line input recording. Set the recording mode and input assignment below before applying.";
    inline constexpr char audioSampleRateHelp[] =
        "Set the shared device sample rate in Hz. A supported value depends on the driver; changing it requires stopping and repreparing realtime audio.";
    inline constexpr char audioBufferSizeHelp[] =
        "Set the audio callback buffer length in samples. Smaller buffers may lower monitoring latency but raise CPU/dropout risk; apply with playback stopped.";
    inline constexpr char audioInputModeHelp[] =
        "Choose mono input recording or a stereo adjacent pair. This is for physical audio capture, not MIDI channel filtering.";
    inline constexpr char audioInputAssignmentHelp[] =
        "Choose which physical input channel, or adjacent stereo channel pair, will be recorded from the selected input device.";
    inline constexpr char audioRefreshDevicesHelp[] =
        "Refresh the currently available driver, input, and output devices. This does not change your saved configuration until Apply / Save.";
    inline constexpr char audioApplyHelp[] =
        "Apply and save the selected audio driver, devices, sample rate, buffer size, and recording input routing. Active realtime audio may be stopped safely.";
    inline constexpr char audioCancelHelp[] =
        "Close Audio Settings without applying the current unsaved selections.";
    inline constexpr char midiInputDeviceHelp[] =
        "Choose the physical MIDI input device for monitoring and recording. If it is disconnected, refresh the device list before starting a take.";
    inline constexpr char midiInputChannelHelp[] =
        "Omni monitors and records all MIDI channels. Channel 1 through 16 filters incoming channel-voice messages to only the selected channel.";
    inline constexpr char midiSetPunchInHelp[] =
        "Set Loop/Punch In at the current Preview Player position. Events at the In tick are included; choose an In earlier than Out.";
    inline constexpr char midiSetPunchOutHelp[] =
        "Set Loop/Punch Out at the current Preview Player position. The Out tick is excluded from this region: [In, Out).";
    inline constexpr char midiClearPunchHelp[] =
        "Clear the stored In/Out recording region. Set a new valid region before using Punch or Loop Record again.";
    inline constexpr char midiUndoTakeHelp[] =
        "Undo the most recently committed MIDI recording or Take Review keep/comp as one atomic take. Later manual MIDI lane edits can block restoring stale take history.";
    inline constexpr char midiRedoTakeHelp[] =
        "Redo a previously undone MIDI take if its take-history guard remains valid. Redo cannot overwrite newer conflicting manual MIDI edits.";
    inline constexpr char midiStopMonitorHelp[] =
        "Stop forwarding live MIDI input to the monitored instrument. This does not discard a retained Take Review.";
    inline constexpr char midiRefreshDevicesHelp[] =
        "Refresh the available physical MIDI input devices. Recheck the selected device if a controller was unplugged or reconnected.";
    inline constexpr char midiApplyInputHelp[] =
        "Apply the selected MIDI input device and channel for the next monitor/record operation. Already armed or recording targets stay protected by their stable identities.";
    inline constexpr char previewGenerateHelp[] =
        "Refresh the current preview scope, capturing open plugin-editor changes before playback where supported. Rendering and live plugin playback may behave differently.";
    inline constexpr char previewPlayPauseHelp[] =
        "Play, pause, or resume a rendered preview. Live VST3/CLAP preview cannot be paused here; use Stop or the seek controls instead.";
    inline constexpr char previewStopHelp[] =
        "Stop the current preview or live plugin playback. This is separate from Stop Recording in MIDI Input settings.";
    inline constexpr char previewCloseHelp[] =
        "Close the Preview Player window; it does not discard the project or its saved MIDI edits.";
}
