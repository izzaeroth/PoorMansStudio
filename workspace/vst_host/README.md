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
```

## Current Scope

The helper performs isolated inspection of one outer VST3 bundle and reports detected plugin metadata in text or JSON form. It uses the JUCE VST3 hosting/scanning foundation in a separate process so risky inspection work does not have to run inside the main application process.

The helper does not currently activate plugins, process audio, open plugin editors, capture plugin state, or provide live VST3 playback. Normal VST3 instruments, effects, editors, recording monitors, previews, rendering, and project playback remain hosted by the main application.

## Executable Size

The VST3 helper links JUCE audio-processor and VST3 hosting code. Those statically linked frameworks can make the executable noticeably larger even though the helper currently exposes a compact command set. Executable size should not be treated as a direct measure of the number of supported helper commands.

## Runtime Files

Do not commit generated `.exe`, `.dll`, `.pdb`, log, cache, or temporary helper files to Git. Do not place third-party plugin bundles or user plugin data in this folder unless they are intentionally part of a local test.
