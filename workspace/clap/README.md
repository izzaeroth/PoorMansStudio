# workspace/clap

Place user-supplied CLAP plug-ins or test CLAP bundles here for local scanning.

Do not commit third-party `.clap` plug-ins, generated binaries, or copied runtime files to Git. This folder is kept in the repository by this README so the app has a stable local CLAP drop location after checkout.

Current v0.66.0 behavior:

- The CLAP Plugin Manager scans this folder and common system CLAP folders.
- The CLAP host helper can validate candidate paths when it has been built and copied to `workspace/clap_host`.
- CLAP loading, Effect Slot assignment, UI hosting, state saving, and audio processing are not active yet.
