# VST3 Host Helper

This folder is the local runtime copy location for the Poor Man's Studio VST3 host helper:

```text
workspace/vst_host/PoorMansStudioVstHost.exe
```

A successful local build copies the helper executable here.

## Commands

```text
PoorMansStudioVstHost.exe --help
PoorMansStudioVstHost.exe --version
PoorMansStudioVstHost.exe --ping
PoorMansStudioVstHost.exe --scan "C:\Path\To\Plugin.vst3"
PoorMansStudioVstHost.exe --scan-json "C:\Path\To\Plugin.vst3"
PoorMansStudioVstHost.exe --validate-instance "C:\Path\To\Plugin.vst3"
PoorMansStudioVstHost.exe --validate-instance-json "C:\Path\To\Plugin.vst3"
PoorMansStudioVstHost.exe --validate-activation "C:\Path\To\Plugin.vst3"
PoorMansStudioVstHost.exe --validate-activation-json "C:\Path\To\Plugin.vst3"
PoorMansStudioVstHost.exe --validate-process "C:\Path\To\Plugin.vst3"
PoorMansStudioVstHost.exe --validate-process-json "C:\Path\To\Plugin.vst3"
PoorMansStudioVstHost.exe --validate-state "C:\Path\To\Plugin.vst3"
PoorMansStudioVstHost.exe --validate-state-json "C:\Path\To\Plugin.vst3"
```

Validation commands also accept an optional zero-based descriptor selector:

```text
--plugin-index 1
```

## Current Scope

The scan commands inspect one outer VST3 bundle and report detected plugin metadata in text or JSON form. The validation commands use the same JUCE VST3 hosting path as the main application to create a selected instance, inspect buses and capabilities, prepare the application-supported instrument or effect layout, run one silent processing block with finite-sample checks, and test a disposable state save-and-restore round trip.

The helper does not open plugin editors, assign project tracks or Effect Slots, modify saved project plugin state, render project audio, or provide live VST3 playback. Normal VST3 instruments, effects, editors, recording monitors, previews, rendering, and project playback remain hosted by the main application.

## Executable Size

The VST3 helper statically links JUCE audio-processor and VST3 hosting code. Those frameworks can make the executable noticeably larger than the CLAP helper even when both helpers expose comparable validation commands. Executable size should not be treated as a direct measure of validation coverage.

## Runtime Files

Do not commit generated `.exe`, `.dll`, `.pdb`, log, cache, or temporary helper files to Git. Do not place third-party plugin bundles or user plugin data in this folder unless they are intentionally part of a local test.
