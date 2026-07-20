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
```

## Current Scope

The scan commands inspect one CLAP candidate and report descriptor metadata in text or JSON form. The validation commands can create and initialize a plugin instance, test activation and capability queries, and run one silent processing block before shutting the instance down cleanly.

The helper does not currently assign project Effect Slots, open plugin editors, save project plugin state, render project audio, or provide live CLAP playback for the main application. Normal CLAP instruments, effects, editors, recording monitors, previews, rendering, and project playback remain hosted by the main application.

## Executable Size

The CLAP helper talks to the CLAP ABI through a relatively small direct interface and does not link JUCE's full audio-processor/VST3 hosting stack. It may therefore remain much smaller than the VST3 helper even though it currently exposes more validation commands.

## Runtime Files

Do not commit generated `.exe`, `.dll`, `.pdb`, log, cache, or temporary helper files to Git. Do not place third-party plugin bundles or user plugin data in this folder unless they are intentionally part of a local test.
