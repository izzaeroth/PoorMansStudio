# CLAP Host Helper

This folder is the local runtime copy location for the Poor Man's Studio CLAP host helper:

```text
workspace/clap_host/PoorMansStudioClapHost.exe
```

A successful local build copies the helper executable here.

## Commands

```text
PoorMansStudioClapHost.exe --help
PoorMansStudioClapHost.exe --version
PoorMansStudioClapHost.exe --ping
PoorMansStudioClapHost.exe --scan "C:\Path\To\Plugin.clap"
PoorMansStudioClapHost.exe --scan-json "C:\Path\To\Plugin.clap"
PoorMansStudioClapHost.exe --validate-instance "C:\Path\To\Plugin.clap"
PoorMansStudioClapHost.exe --validate-instance-json "C:\Path\To\Plugin.clap"
PoorMansStudioClapHost.exe --validate-activation "C:\Path\To\Plugin.clap"
PoorMansStudioClapHost.exe --validate-activation-json "C:\Path\To\Plugin.clap"
PoorMansStudioClapHost.exe --validate-process "C:\Path\To\Plugin.clap"
PoorMansStudioClapHost.exe --validate-process-json "C:\Path\To\Plugin.clap"
PoorMansStudioClapHost.exe --validate-state "C:\Path\To\Plugin.clap"
PoorMansStudioClapHost.exe --validate-state-json "C:\Path\To\Plugin.clap"
```

Scan and validation commands also accept an optional zero-based descriptor selector:

```text
--plugin-index 1
```

## Current Scope

The scan commands inspect one CLAP candidate and report descriptor metadata in text or JSON form. The validation commands can create and initialize a selected plugin instance, test activation, inspect audio ports, note ports, latency, tail, parameters, state and GUI-extension availability, run one silent processing block with finite-sample checks, and test a disposable state save-and-restore round trip.

The helper provides the small `clap.thread-check` host extension for lifecycle validation. It intentionally does not provide `clap.log`. It does not assign project tracks or Effect Slots, open plugin editors, modify saved project plugin state, render project audio, or provide live CLAP playback. Normal CLAP instruments, effects, editors, recording monitors, previews, rendering, and project playback remain hosted by the main application.

## Executable Size

The CLAP helper talks directly to the CLAP ABI and does not link JUCE's audio-processor/VST3 hosting stack. Keeping optional logging out of the helper also avoids adding a logging path that is not needed for the current validation scope. It may therefore remain much smaller than the VST3 helper even when both helpers expose comparable lifecycle and state checks.

## Runtime Files

Do not commit generated `.exe`, `.dll`, `.pdb`, log, cache, or temporary helper files to Git. Do not place third-party plugin bundles or user plugin data in this folder unless they are intentionally part of a local test.
