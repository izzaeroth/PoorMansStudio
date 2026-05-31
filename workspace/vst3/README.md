# VST3 Plugins

Place local VST3 plugin bundles here for Poor Man's Studio to scan during development or personal use.

Example:

```text
workspace/vst3/MySynth.vst3
```

Poor Man's Studio also scans the standard Windows VST3 folder:

```text
C:\Program Files\Common Files\VST3
```

VST3 support is experimental. A plugin can crash inside its own native code. Save work before testing unfamiliar plugins.

A VST3 plugin may be a bundle folder ending in `.vst3`; keep the outer bundle intact. Do not point the app at inner binaries under `Contents\x86_64-win`.

Use `VST Plugins > Scan VST3 Plugins` after adding or installing plugins. Use `VST Plugins > VST3 Plugin Manager...` to review classification, failed/blocked status, and which plugins appear in the Instrument dropdown.

Do not commit or redistribute third-party VST3 plugins unless their license explicitly allows it. Release packages should include this README only by default.
