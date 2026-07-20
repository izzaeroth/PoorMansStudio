# workspace/clap

Place user-supplied CLAP plug-ins or test CLAP bundles here for local scanning.

Do not commit third-party `.clap` plug-ins, generated binaries, or copied runtime files to Git. This folder is kept in the repository by this README so the app has a stable local CLAP drop location after checkout.

Current behavior:

- The CLAP Plugin Manager scans this folder and common system CLAP folders.
- The CLAP host helper can scan and validate selected candidate paths when it has been built and copied to `workspace/clap_host`.
- The main application supports CLAP instrument and Effect Slot assignment, plugin editors and state, live preview, recording-monitor effects, rendered fallback, and project rendering/export workflows.
- Helper validation remains separate from normal project playback and does not take ownership of plugin editors or live audio.
