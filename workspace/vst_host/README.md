# VST Host Helper Folder

This folder is reserved for the out-of-process VST3 host helper:

```text
workspace/vst_host/PoorMansStudioVstHost.exe
```

VST3 instruments are currently hosted in-process by the main app. Because VST3 support is experimental, a plugin that crashes inside its own native code can also close the main app.

The helper host is intended to provide crash isolation later: a bad plugin should be able to close its helper process without taking down the main Poor Man's Studio app.
