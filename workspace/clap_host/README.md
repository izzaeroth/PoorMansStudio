# workspace/clap_host

This folder is the local runtime copy location for the Poor Man's Studio CLAP host helper.

After a successful local build, CMake copies the helper here, for example:

```text
workspace/clap_host/PoorMansStudioClapHost.exe
```

Do not commit generated `.exe`, `.dll`, `.pdb`, log, cache, or temporary helper runtime files to Git. This README keeps the folder present in fresh checkouts without requiring generated binaries to be tracked.

Current v0.66.0 behavior:

- The helper supports `--help`, `--version`, `--ping`, `--scan`, and `--scan-json`.
- It is scanner/helper groundwork only.
- It does not load the CLAP ABI, process audio, assign Effect Slots, open plugin UIs, or save CLAP plugin state yet.
