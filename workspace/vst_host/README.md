# VST Host Helper Folder

This folder is reserved for the out-of-process VST3 host helper runtime:

```text
workspace/vst_host/PoorMansStudioVstHost.exe
```

Starting with Phase 3, the source tree includes a small helper executable target named `PoorMansStudioVstHost`. A local build copies the helper here after it is built.

Current helper foundation:

```text
PoorMansStudioVstHost.exe --help
PoorMansStudioVstHost.exe --version
PoorMansStudioVstHost.exe --ping
PoorMansStudioVstHost.exe --scan "C:\Path\To\Plugin.vst3"
PoorMansStudioVstHost.exe --scan-json "C:\Path\To\Plugin.vst3"
```

This is Phase 3 groundwork only. The main app still hosts VST3 instruments in-process for normal use. The helper is being introduced first for isolated/risky actions such as plugin inspection and scanning.

Longer term, the helper can become the owner for plugin editor windows, plugin state capture, offline helper rendering, and eventually shared-memory live processing. The goal is crash isolation: a bad plugin should be able to close its helper process without taking down the main Poor Man's Studio app.

Do not place third-party plugin bundles or user plugin data in this folder unless they are intentionally part of a local test. Built helper/runtime files belong in release packages, not necessarily in source control.
