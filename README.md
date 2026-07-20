# Poor Man's Studio

**Poor Man's Studio** is a lightweight local Windows music workstation for importing, arranging, editing, previewing, saving, and rendering music projects without needing a full commercial DAW.

It is intentionally practical: bring in MIDI or score material, clean up tracks, organize sections, assign instruments, preview ideas quickly, and render usable audio or MIDI output from a self-contained workspace. It has just enough DAW energy to be useful, without trying to become a spaceship cockpit.

## Quick Feature Tour

### Import, Arrange, And Edit

- Import MusicXML, compressed MXL, MIDI, Poor Man's Studio `.mwproj`, WAV, MP3, FLAC, and OGG files.
- Create MIDI tracks from score/MIDI files.
- Create AudioClip tracks from imported, recorded, or rendered AudioClip arrangement audio.
- Edit notes in the main editor and in dedicated Piano Roll windows.
- Edit AudioClip source trim non-destructively, enhance the full AudioClip source into a new generated copy, freeze a kept range, arrange repeated or overlapping local clips, add editor-only Aux source trims into the arrangement, preview the arrangement, and render it into a new mixed AudioClip track.
- Keep one Piano Roll window per track so track edits stay understandable.
- Organize track groups, song sections, sequence assignments, ordering, names, and track layout through the Track Manager.
- Save projects as `.mwproj` files with project-relative media paths.

### Instruments And Sound Sources

- Use SF2/SF3 SoundFonts through FluidSynth.
- Use SFZ instruments through sfizz-render.
- Use experimental VST3 instrument plugins through JUCE VST3 hosting.
- Use experimental CLAP instruments and effects through the CLAP scanner/manager, editor windows, snapshots, offline render paths, and guarded direct/live preview paths.
- Set project defaults for future imported/new MIDI tracks.
- Apply per-track settings intentionally so existing tracks are not overwritten accidentally.
- Switch a track between SF2/SF3/SFZ/VST3/CLAP without deleting its MIDI notes.
- Preserve notes when changing libraries; the instrument assignment changes, the music stays.

### VST3 Plugin Support

VST3 support is experimental. Some plugins are perfectly calm. Some plugins are dragons wearing DLL costumes.

Current VST3 support includes:

- Native selected-track VST3 playback through a persistent JUCE audio callback session.
- Project-wide persistent VST3 instrument/effect sessions owned by stable track IDs during live playback.
- Mixes compatible live VST3 tracks, live CLAP tracks, and prepared rendered sources on the same timeline.
- Live VST3 Effect Slot 1 / Slot 2 processing in slot order when every enabled effect in that track's chain is VST3.
- Preview Player integration for live VST3 Stop and seek-by-safe-rebuild behavior; Pause remains available only for rendered previews.
- Device-rate/block-size preparation, callback busy protection, emergency note-off flushing, finite-sample filtering, clipping protection, and rendered fallback.
- VST3 scanning from `workspace/vst3` and standard Windows VST3 locations.
- VST3 Plugin Manager with **All Plugins**, **Supported Instruments**, **Supported Effects**, and **Unsupported** filters.
- Two-slot VST Effect chain controls in the main track inspector: choose Supported Effects for slot 1 and slot 2, enable/disable or bypass each slot, save the assignments in `.mwproj` files, and process the chain during offline preview/render paths for MIDI and AudioClip tracks.
- Fixed effect-chain order: source track -> VST Effect 1 -> VST Effect 2 -> preview/export output. Unassigned, disabled, or bypassed slots are skipped.
- VST editor host windows use a small toolbar under the title bar for **Apply Changes**; tiny plugin UIs get a wider minimum host size while remaining resizable.
- Manual override for plugins that are safe instruments or effect candidates but detected as unknown/unsupported.
- One VST plugin editor window per track instrument assignment and one VST effect editor window per track/effect slot.
- Track-owned VST editor toolbar with **Apply Changes**, **Test Instrument** for instrument editors, a compact target label such as `Track #1 - S1 - Reverb`, and **Test Effect** for effect editors.
- Per-track VST state save/restore so two tracks using the same plugin can keep different sounds.
- VST state excluded from normal host-side note/edit undo-redo history.
- VST Host Helper Status from the VST Plugins menu, backed by the helper executable.
- One-time experimental dragon warning before VST3 becomes active through defaults, track assignment, plugin UI, preview, or render.

VST3 instrument hosting remains in-process, so a plugin crash can still close the main app.

### CLAP Plugin Support

CLAP support is experimental and newer than the VST3 path. It is designed to preserve the existing offline render/export workflow while adding guarded direct/live preview when the project is safe for it.

Current CLAP support includes:

- CLAP scanning from `workspace/clap` and standard CLAP locations where available.
- CLAP Plugin Manager with instrument/effect cataloging and user-facing **CLAP Instrument** / **CLAP Effect** labels.
- CLAP Settings / Compatibility for plugin graphics adapter choice, maximum open CLAP windows, Safe CLAP Plugin UI Mode, and **Refresh Adapter List**.
- CLAP Host Helper Status from the CLAP Plugins menu, backed by the helper foundation under `workspace/clap_host`.
- CLAP instrument editor windows with **Apply Changes**, **Test Instrument**, snapshots, and explicit state capture.
- CLAP effect editor windows for track Effect Slot 1 / Slot 2 with **Apply Changes**, **Test Effect**, snapshots, and explicit state capture.
- Offline CLAP instrument rendering and CLAP effect processing for render/export and fallback preview paths.
- Direct CLAP preview for compatible selected-track and multi-track CLAP projects through the live scheduler/callback bridge.
- Guarded live CLAP Effect Slot 1 / Slot 2 routing during direct CLAP preview when the selected effects are CLAP effects.
- Compatible CLAP and VST3 instrument tracks remain live together while SF2/SF3, SFZ, AudioClip, cross-backend effect chains, latency-reporting plugins, and otherwise unsafe tracks use the prepared or full rendered fallback.
- Explicit transport lifecycle state and playback generation tracking protect project replacement, stop/restart, live seek restart, stale asynchronous preparation, and completion handling.
- Stable project and track identities are saved in `.mwproj` project format version 9 so live session ownership does not depend only on mutable track indexes.
- Full rendered/temp-WAV fallback remains available when hybrid preparation, plugin opening, the audio device, or live routing cannot run safely.

Direct CLAP preview is not the same as final export. Render/export paths still use the saved project state and offline processing so output remains repeatable and compatible with the existing render workflow.

The Help menu includes one combined **Plugin Compatibility Warnings** toggle for VST3 and CLAP warning popups. The app keeps the VST3 and CLAP acknowledgement state separate internally, but the user-facing Help menu stays compact with one shared warning control.

### Preview, Render, And Export

- Preview a selected track, selected sequence, Piano Roll area, enhanced AudioClip source preview, AudioClip arrangement, or the full project.
- Compatible VST3 and CLAP selected-track previews may play through their guarded direct/live paths. Project Preview uses one shared callback for compatible live VST3 and CLAP tracks plus prepared rendered sources; unsupported or unsafe cases still use the full rendered/temp-WAV fallback.
- Render the full project, selected track, selected sequence, or stems depending on render settings.
- Export WAV, FLAC, MP3, and OGG audio.
- Attach album art to MP3 renders when Output Format is MP3.
- Export MIDI for use in other tools.
- Configure sample rate, bitrate, channels, output format, render metadata, and MP3 album art where supported.
- Use FFmpeg for audio conversion/mixing/compressed output support.
- Use `workspace/exports` as the default user-facing render/export destination.


### MP3 Album Art For Renders

The main window includes **Attach Album Art** under **Edit Info** for final MP3 renders. The option is enabled only when **Output Format** is **MP3**. Checking it opens an image picker for PNG, JPG, or JPEG artwork. Saved projects keep the selected image under `input/album_art`; unsaved projects stage the image temporarily until Save Project or Save As commits it into the saved project folder. The option is for final MP3 renders only, not temporary previews, Render To Track outputs, MIDI export, or non-MP3 audio. Non-MP3 formats keep the album-art checkbox unavailable, so WAV, FLAC, OGG, and MIDI renders do not attempt to embed cover art.

### Export Folder Behavior

Poor Man's Studio keeps user-facing renders in `workspace/exports` by default instead of auto-switching exports into each project folder. Saving or opening a `.mwproj` no longer retargets the export box to `workspace/projects/<project>/renders`; older project-local `renders`/`output` paths are reset back to the workspace export folder unless you manually choose another custom export folder.

Render Settings retention applies consistently to non-WAV exports too. When **Keep WAV audio stems** is enabled, the source WAV sidecar used for MP3/FLAC/OGG encoding is kept next to the final export; when **Keep MIDI stem files** is enabled, generated MIDI sidecars/stems are kept. Preview/temp renders, including enhanced AudioClip previews, still clean themselves under `workspace/temp`.

### Usability And Project Safety

- Helper bubbles for in-app guidance.
- Plain key/value settings file that can be inspected by hand.
- Settings save behavior separates explicit full settings saves from narrow one-key acknowledgements.
- Dirty-state prompts so unapplied editor changes are not silently discarded. AudioClip Editor close prompts apply only to pending source-trim edits, not arrangement tracks already rendered with Render To Track.
- Workspace cleanup for temporary files.
- Theme preset support.
- Included PDF and text guides under `workspace/docs`.

## Normal User Workflow

1. Choose a project backend in Project Defaults.
2. Choose the matching library, preset, SFZ file, VST3 plugin, or CLAP plugin.
3. Click **Apply Project Defaults** before importing or adding new MIDI tracks.
4. Import MusicXML, MXL, MIDI, audio, or open an existing `.mwproj` project.
5. Organize tracks and sequences.
6. Edit notes in the main UI or Piano Roll.
7. Apply per-track instrument changes where needed.
8. Preview the track, sequence, Piano Roll, or full project.
9. Save the `.mwproj` project.
10. Render final audio or MIDI output.

Project Defaults affect future imported or newly added MIDI tracks. Existing track assignments are changed only through per-track controls or Piano Roll track settings.

## Source Tree Layout

Expected source layout:

```text
PoorMansStudio/
  CMakeLists.txt
  README.md
  THIRD_PARTY_NOTICES.txt
  resources/
  src/
    audio/
    clap/
    clap_host_helper/
    gui/
  workspace/
    docs/
    exports/
    ffmpeg/
    fluidsynth/
    input/
      album_art/
    projects/
    settings/
    sfizz/
    sfz/
    soundfonts/
    temp/
    themes/
    vst3/
    vst_host/
    clap/
    clap_host/
  external/
    JUCE/
```

The `external/JUCE` folder is required for building from source. It is not bundled as a runtime dependency for end users.

## Workspace Layout

```text
workspace/
  input/        import staging area, including project album_art metadata images when used
  exports/      default render/export destination for audio, MIDI, and kept WAV sidecars/stems
  projects/     .mwproj project folders
  soundfonts/   user SF2/SF3 files
  fluidsynth/   optional FluidSynth runtime/tool folder
  ffmpeg/       optional FFmpeg runtime/tool folder
  sfz/          user SFZ instruments and sample packs
  vst3/         portable/test VST3 bundles and shared plugin test WAV
  vst_host/     built VST helper executable/runtime helper location
  clap/         portable/test CLAP bundles and CLAP notes
  clap_host/    built CLAP helper executable/runtime helper location
  sfizz/        optional sfizz-render runtime/tool folder
  settings/     user settings
  temp/         temporary render/cache files
  themes/       theme presets
  docs/         PDF and text guides
```

Projects should live under:

```text
workspace/projects/<Project Name>/
```

AudioClip media and album-art image files are not embedded into the `.mwproj` file. Newly imported files, Save / Apply recorded takes, enhanced AudioClip copies, and rendered AudioClip arrangements for unsaved projects stage first under the active `workspace/temp/recordings/rec_...` session. **Save Project** is the commit point that moves attached staged media into the saved project folder under `input/audio/imported` or `input/audio/recorded`. Import Audio and Render To Track should not create a new folder under `workspace/projects` until the project is actually saved, and they should not leave unreferenced media inside an already saved project folder if the user later discards changes. Choosing **Discard** from New/Open/Start From File/Exit unsaved-change prompts removes the active staged AudioClip session so abandoned imports, takes, enhanced copies, and unsaved arrangement renders do not carry into the next project. Move or share the whole saved project folder, not just the `.mwproj` file.

## Build Environment Overview

Poor Man's Studio is currently a Windows/JUCE/C++ desktop app.

Recommended build environment:

- Windows 10 or Windows 11, 64-bit.
- Visual Studio Community 2026 / Visual Studio 18.x or newer, or Visual Studio 2022 with equivalent C++ support.
- **Desktop development with C++** workload installed in Visual Studio Installer.
- MSVC C++ x64/x86 build tools.
- Windows 10 or Windows 11 SDK.
- MSBuild.
- C++ CMake tools for Windows.
- CMake 3.24 or newer; current reference validation uses CMake 4.4.0.
- Git for Windows, recommended for obtaining/updating JUCE.
- JUCE source tree at `external/JUCE`; current reference validation uses JUCE 8.0.14.
- C++20-capable compiler.

The project CMake file currently requires:

```cmake
cmake_minimum_required(VERSION 3.24)
set(CMAKE_CXX_STANDARD 20)
```

JUCE's own CMake support requires CMake 3.22 or newer, but this project requires 3.24 or newer. The current development setup is using JUCE 8.0.14 and CMake 4.4.0.

## Visual Studio Community Setup

Install Visual Studio Community from Microsoft, then open **Visual Studio Installer** and modify the installation.

Required workload:

```text
Desktop development with C++
```

Make sure these components are installed under the workload or Individual Components tab:

```text
MSVC C++ x64/x86 build tools
Windows 10 SDK or Windows 11 SDK
C++ CMake tools for Windows
MSBuild
C++ core desktop features
Windows Universal CRT SDK
```

Recommended optional components:

```text
Git for Windows
C++ AddressSanitizer, useful for debugging memory issues
C++ profiling tools, useful for performance testing
```

Usually not required for this project:

```text
MFC
ATL
.NET desktop workload
Unity workload
Linux workload
Clang/LLVM toolchain
```

Those can be installed if your local environment needs them, but Poor Man's Studio does not intentionally depend on them.

## Build-Time Dependencies

These are needed to build the app from source:

| Tool / dependency | Purpose | Notes |
| --- | --- | --- |
| Windows 10/11 | Build OS | 64-bit Windows recommended. |
| Visual Studio Community | IDE, compiler, debugger, MSBuild | Use Community 2026 / VS 18.x or newer if available; Visual Studio 2022 with the C++ workload should also work. |
| Desktop development with C++ workload | Installs core C++ build support | Required because C++ tools are not installed by default. |
| MSVC C++ build tools | C++ compiler/linker | Required for compiling the JUCE/C++ app. |
| Windows SDK | Windows headers/libraries | Installed through the C++ workload/components. |
| MSBuild | Visual Studio build engine | Used by Visual Studio CMake generators. |
| CMake 3.24+ | Configures/generates builds | The project uses CMake directly. Current reference setup uses CMake 4.4.0. |
| Git | Cloning/updating source and JUCE | Optional if JUCE is copied manually, recommended otherwise. |
| JUCE source tree | C++ app framework | Must exist at `external/JUCE`. Current reference setup uses JUCE 8.0.14. Build-time source dependency, not a separate runtime install. |

## Runtime Dependencies And Resources

These are used when running the built app or rendering projects:

| Runtime item | Purpose | Bundled? |
| --- | --- | --- |
| FluidSynth | SF2/SF3 SoundFont rendering | Current reference setup uses FluidSynth 2.5.6. May be placed/configured in workspace; not a JUCE dependency. |
| FFmpeg | Audio conversion, mixing, compressed exports | Current reference setup uses a recent Windows FFmpeg build. May be placed/configured in workspace. |
| sfizz-render | SFZ rendering | May be placed/configured in workspace. |
| SoundFonts / SF3 files | User instrument libraries | User supplied. |
| SFZ sample packs | User instrument libraries | User supplied. |
| VST3 plugins | Experimental plugin instruments/effects | User supplied or installed system-wide. |
| CLAP plugins | Experimental plugin instruments/effects | User supplied or installed system-wide; direct preview support is guarded and falls back when unsafe. |
| Audio/MIDI drivers/devices | Playback, input, preview | Provided by the OS/audio interface. |

**JUCE is not a runtime dependency for users of the compiled app.** JUCE code is compiled into the application binary during the build.

## Getting The Tools

### Visual Studio Community

1. Download Visual Studio Community from Microsoft.
2. Run the installer.
3. Select **Desktop development with C++**.
4. Confirm the MSVC build tools, Windows SDK, MSBuild, and C++ CMake tools are selected.
5. Install or modify the installation.
6. Open **Developer PowerShell for Visual Studio** or **x64 Native Tools Command Prompt** before building from the command line.

### CMake

Visual Studio can install CMake tools through the C++ workload. Installing a current standalone CMake from Kitware is also acceptable and often easier to verify from the command line.

Check CMake with:

```powershell
cmake --version
```

### Git

Git is recommended for downloading JUCE and for source control.

Check Git with:

```powershell
git --version
```

### JUCE

Place JUCE at:

```text
PoorMansStudio/external/JUCE/
```

Example using Git from the project root:

```powershell
mkdir external
git clone https://github.com/juce-framework/JUCE.git external/JUCE
```

The important file must exist here:

```text
external/JUCE/CMakeLists.txt
```

If that file is missing, CMake will stop with a JUCE-not-found error.

## Configure And Build

Open **Developer PowerShell for Visual Studio** or **x64 Native Tools Command Prompt**, then `cd` to the `PoorMansStudio` source folder.

### Recommended: build wrapper with summary

The easiest build path is the included PowerShell wrapper. It runs CMake configure/build, times the build, checks common inputs, and prints a clean summary at the end.

```powershell
.\Build-PoorMansStudio.ps1
```

Example summary:

```text
Poor Man's Studio Build Summary
--------------------------------
Version:        0.66.4
Configuration:  Release
Generator:      Visual Studio 18 2026
Platform:       x64
Parallel Jobs:  16
Started:        2026-07-19 11:05:39
Finished:       2026-07-19 11:10:13
Elapsed:        4 min 33 sec
Result:         SUCCESS
Output:         C:\Dev\PoorMansStudio\workspace\program\Poor Man's Studio.exe
VST Host:       C:\Dev\PoorMansStudio\workspace\vst_host\PoorMansStudioVstHost.exe
```

Useful options:

```powershell
.\Build-PoorMansStudio.ps1 -Configuration Debug
.\Build-PoorMansStudio.ps1 -Parallel 16
.\Build-PoorMansStudio.ps1 -Generator "Visual Studio 17 2022" -Platform x64
.\Build-PoorMansStudio.ps1 -SkipConfigure
```

From x64 Native Tools Command Prompt, launch the same script through Windows PowerShell:

```bat
powershell -ExecutionPolicy Bypass -File .\Build-PoorMansStudio.ps1
powershell -ExecutionPolicy Bypass -File .\Build-PoorMansStudio.ps1 -Parallel 16
```

Manual CMake commands still work if you prefer them.

### Option A: Let CMake choose the Visual Studio generator

```powershell
cmake -S . -B builds -A x64
cmake --build builds --config Release
```

### Option B: Explicit Visual Studio 2022 generator

```powershell
cmake -S . -B builds -G "Visual Studio 17 2022" -A x64
cmake --build builds --config Release
```

### Option C: Visual Studio 2026 / VS 18.x generator

Run this first:

```powershell
cmake --help
```

Look under **Generators** for the exact Visual Studio generator name installed on your machine. If your CMake lists a Visual Studio 2026 / 18.x generator, use that exact spelling, for example:

```powershell
cmake -S . -B builds -G "Visual Studio 18 2026" -A x64
cmake --build builds --config Release
```

If CMake does not list that generator, update CMake or use the generator CMake does list.

### Debug Build

```powershell
cmake --build builds --config Debug
```

### Clean Reconfigure

Use this when changing JUCE versions, CMake versions, or Visual Studio toolchains:

```powershell
rmdir /s /q builds
cmake -S . -B builds -A x64
cmake --build builds --config Release
```

The post-build step copies the built executable to:

```text
workspace/program/
```

## Running After Build

Run the app from the built output or from:

```text
workspace/program/
```

Keep the `workspace` folder beside the source/app layout unless you intentionally configure paths elsewhere. The app expects the workspace folders for docs, settings, projects, tools, and libraries.

## External Runtime Tool Setup

### FluidSynth

Used for SF2/SF3 rendering.

Suggested setup:

```text
workspace/fluidsynth/
```

Place the FluidSynth executable/DLL files there, or install FluidSynth elsewhere and set the path in the app settings.

### FFmpeg

Used for conversion, compressed audio output, and some mixing/export operations.

Suggested setup:

```text
workspace/ffmpeg/
```

Place `ffmpeg.exe` and related files there, or install FFmpeg elsewhere and configure the path in the app settings.

### sfizz-render

Used for SFZ rendering.

Suggested setup:

```text
workspace/sfizz/
```

Place `sfizz_render.exe` / `sfizz-render.exe` and related files there, or configure the path in the app settings.

### VST3 Plugins

VST3 support is experimental.

Scan locations include:

```text
workspace/vst3/
C:\Program Files\Common Files\VST3
```

A VST3 plugin may be a `.vst3` bundle folder. Keep the bundle intact. Do not point the app at inner DLLs under `Contents\x86_64-win`.

### CLAP Plugins

CLAP support is experimental.

Portable/test CLAP bundles can live under:

```text
workspace/clap/
```

System CLAP install locations vary by plugin vendor. Keep `.clap` bundles intact and scan the folder that contains the plugin bundle. Use the CLAP Plugin Manager to scan, review supported instruments/effects, and open compatible editor windows. Use **CLAP Settings...** for graphics adapter and Safe CLAP Plugin UI Mode choices. Use **CLAP Plugins > CLAP Host Helper Status...** to check the helper foundation status.

## Troubleshooting Build Problems

### CMake says JUCE was not found

Confirm this exists:

```text
external/JUCE/CMakeLists.txt
```

If not, clone or copy JUCE into `external/JUCE`.

### CMake cannot find a compiler

Use **Developer PowerShell for Visual Studio** or **x64 Native Tools Command Prompt**. Also confirm the Visual Studio **Desktop development with C++** workload is installed.

### The Visual Studio generator is not available

Run:

```powershell
cmake --help
```

Use a generator listed on your system, or update CMake so it knows about your installed Visual Studio version.

### Windows SDK errors

Open Visual Studio Installer, modify the installation, and install a Windows 10 or Windows 11 SDK under the C++ components.

### `cl` is not recognized

You are probably in a normal terminal instead of a Visual Studio developer terminal, or MSVC was not installed. Open a Visual Studio developer prompt and try:

```powershell
cl
```

### Build works but the app cannot render SF2/SF3/SFZ/MP3/OGG

The app built correctly, but runtime tools are missing or not configured. Check FluidSynth, FFmpeg, and sfizz-render paths in the app settings.

### VST3 plugin crashes the app

The VST3 feature is experimental and hosted in-process. Save projects before testing unfamiliar plugins. Use **Apply Changes** after editing plugin UI state. If a plugin repeatedly crashes, remove it from the active assignment or mark it unavailable in the VST3 Plugin Manager.

### CLAP plugin crashes, freezes, or does not preview live

CLAP support is experimental. Save projects before testing unfamiliar plugins. Use **Apply Changes** after editing plugin UI state; direct CLAP preview uses the current saved/applied track state except for the explicit Test Instrument button, which captures the open editor state for the test. If direct CLAP preview cannot start, if the project contains unsupported routing, or if the plugin is unsafe for the guarded live path, Poor Man's Studio falls back to the rendered/temp-WAV preview path where possible.

### VST Host Helper Foundation

The source tree includes a small out-of-process helper target named `PoorMansStudioVstHost`. A local build copies it to:

```text
workspace/vst_host/PoorMansStudioVstHost.exe
```

The helper currently provides conservative command-line groundwork for isolated VST3 inspection/scanning:

```powershell
.\workspace\vst_host\PoorMansStudioVstHost.exe --help
.\workspace\vst_host\PoorMansStudioVstHost.exe --ping
.\workspace\vst_host\PoorMansStudioVstHost.exe --scan "C:\Path\To\Plugin.vst3"
.\workspace\vst_host\PoorMansStudioVstHost.exe --scan-json "C:\Path\To\Plugin.vst3"
```

Normal VST3 instrument hosting and the Track Live Effects recorder monitor path remain in-process. The helper is groundwork for risky scanning, plugin editor isolation, state capture, offline helper rendering, and later shared-memory live processing. The app checks the helper once at startup and exposes the cached result through **VST Plugins > VST Host Helper Status...**.

### CLAP Host Helper Foundation

The source tree also includes CLAP host/helper groundwork under `src/clap_host_helper`. A local build may copy helper output to:

```text
workspace/clap_host/
```

The app exposes the cached helper status through **CLAP Plugins > CLAP Host Helper Status...**. CLAP plugin editor windows, scan/manager workflows, offline render fallback, and guarded direct preview are kept separate from the visible helper status command so the normal CLAP menu stays focused on user-facing actions.

## Documentation

- Comprehensive PDF user guide: `workspace/docs/PoorMansStudio_User_Guide.pdf`
- Plain-text user guide: `workspace/docs/USER_GUIDE.txt`
- Detailed setup/build guide: `workspace/docs/SETUP_AND_BUILD_GUIDE.txt`

Use the in-app Help menu to open the guides when running the app.

### Current Plugin Menus And Window Menu

The top-level plugin menus are intentionally compact:

```text
VST Plugins
  Scan VST3 Plugins
  VST3 Plugin Manager...
  VST3 Settings...
  VST Host Helper Status...

CLAP Plugins
  Scan CLAP Plugins
  CLAP Plugin Manager...
  CLAP Settings...
  CLAP Host Helper Status...
```

Plugin editor cleanup lives in the **Window** menu as **Close All Plugin Windows**, near the regular **Close All Open Windows** command. **Refresh Adapter List** lives inside VST3 Settings and CLAP Settings, not as a top-level plugin-menu command.

### VST3 Plugin Manager Categories

The Plugin Manager separates detected plugins into **Supported Instruments**, **Supported Effects**, and **Unsupported**. Supported Instruments feed the current instrument workflow and appear in track instrument dropdowns. Supported Effects feed both track-owned VST Effect dropdowns in the main track inspector. Unsupported plugins remain inactive until moved to a supported category and applied. The move controls use visible text labels (**Add >** and **< Remove**) for reliable display on Windows font/rendering combinations.

The Piano Roll Preview Player **Preview** button always re-renders fresh preview audio before playback and captures open VST editor state first, so adjusted instrument/effect settings are picked up without closing the player.

### Two-slot VST Effect Chain

The main track inspector provides two effect rows:

```text
VST Effect 1: [dropdown] [Enable] [Bypass] [Open Slot 1]
VST Effect 2: [dropdown] [Enable] [Bypass] [Open Slot 2]
```

The dropdowns stay on the left so the selected effects line up with the rest of the track controls. Choose a Supported Effect for each slot, then use **Enable** to include that slot in offline preview/export processing and to make that slot's editor openable. **Bypass** temporarily skips that slot without clearing the selected plugin or saved state. Detailed ready/bypass/missing status is written to the log/output area instead of taking up main-window track-control space.

Processing order is always:

```text
source track -> VST Effect 1 -> VST Effect 2 -> preview/export
```

Unassigned, disabled, or bypassed slots are skipped. Open Slot 1/2 is unavailable unless that slot has an assigned plugin and its Enable checkbox is checked. If both slots are enabled and assigned, slot 2 receives the output from slot 1. The old single-effect project format is read as slot 1 for compatibility.

Use **Apply Track Settings** as the clear commit point for track-level effect choices: slot assignments, Enable, Bypass, mute/solo, volume, and other track controls. Use **Apply Changes** inside a VST editor only after changing plugin parameters; that editor button saves the plugin state for the owning track and slot.

Imported and recorded AudioClip source media stays dry. Track effects are applied non-destructively during preview and export rather than being printed into project recording media. During offline preview/export, enabled non-bypassed slots are applied to temporary processed WAV files and those temporary files are mixed into the render without rewriting the source media. AudioClip Enhancement / Repair creates temporary previews or generated enhanced copies without overwriting the original source. New AudioClip media and selected MP3 album-art images stage in the active `workspace/temp/recordings/rec_...` session when the project is unsaved; **Save Project** moves only applied/attached staged clips into the saved project folder, while **Discard** removes the current staging session instead of leaving abandoned media for a later project.

### AudioClip Editor Arrangement Workflow

The AudioClip Editor provides non-destructive source trimming and a local arrangement lane. The original imported or recorded source media is not rewritten, moved, or deleted by source-trim edits.

Use the top waveform to set the kept range. Drag only the green Start handle or red End handle, or type trim values directly, then use **Apply Trim** to commit source trim metadata. The close warning in the AudioClip Editor applies only to pending source-trim edits. Applying or discarding pending trim does not delete or roll back any new arrangement track already created with **Render To Track**.

Use **Freeze Trim** to lock the current kept range for arrangement. Then click in the arrangement lane to place it. Clicking again places another copy of the same frozen Main range, which is the intended loop/repeat workflow. **Append Clip** places the frozen Main trim at the current audible arrangement end. Use **Clip #**, **Clip Start**, **Move Clip**, and **Delete Clip** to place, move, and remove selected clips. Transparent colored clips make overlaps visible, and the selected clip is drawn on top so overlapping clips remain editable. **Extend +10s** adds workspace; the horizontal scroll is free-form.

**Load Aux Source** opens an editor-only Aux audio file trimmer. The Aux source is not added as a project track and is not saved as its own project media. Freeze a range in the Aux window and click **Append to Main** to add it to the main arrangement at the current audible end. Main clips are labeled like `#1 Main`; Aux clips are labeled like `#3 Aux`. Closing the Aux window does not remove appended Aux blocks, and loading another Aux file later does not overwrite earlier Aux blocks. Keep the original Aux source file available until Preview Arrangement or Render To Track completes.

**Preview Arrangement** auditions the local Main/Aux arrangement without creating a new track. **Render To Track** creates one new imported-style AudioClip track from the mixed arrangement. The rendered file length follows the last audible placed clip, not the visible arrangement-window length, so extra empty workspace does not add dead air. Track Volume and Master Volume apply to AudioClip material during preview/render/mix. Saved projects write the generated media under `input/audio/imported`; unsaved projects stage it in temp and migrate it into `input/audio/imported` when the project is saved.

### AudioClip Enhancement / Repair

The AudioClip Editor also includes a source-level enhancement section for rough imported or recorded clips. Choose an enhancement preset and Strength, then use **Preview Enhanced** to create a temporary preview or **Create Enhanced Copy** to generate a new AudioClip track from the full source. The original source file is not overwritten, and source trim handles or local arrangement clips are not baked into the enhancement.

Enhanced copies are written as editable generated WAV media. Saved projects store them under `input/audio/imported`; unsaved projects stage them temporarily and migrate them into the saved project folder when the project is saved. Use final render/export settings later when you want a delivery file such as MP3, FLAC, or OGG.

### AudioClip Recorder Dry Capture And Live Effect Behavior

The AudioClip Recorder always saves dry project audio. The selected blank target track's assigned, enabled, non-bypassed VST3 and/or CLAP effects remain on the track and are applied non-destructively during Track Manager preview, Project Preview, rendering, and export. This preserves the original microphone take, allows effect settings to be changed later, and prevents accidental double processing.

The **Track Live Effects** checkbox controls monitoring only. When checked, the recorder opens up to two eligible effects and processes them in fixed Slot 1 -> Slot 2 order while the WAV writer continues receiving the dry pre-fader microphone signal. VST3/VST3, CLAP/CLAP, VST3/CLAP, and CLAP/VST3 chains are supported. Monitoring follows the target track and master volume. Current open effect-editor state is used temporarily when available without changing the saved project; otherwise the last applied state or plugin default is used. If either stage cannot load or fails while processing, the complete wet monitor chain stops while dry recording continues. Record Test may create a disposable wet temporary WAV so automatic playback auditions the chain, but it never becomes project media. The monitor choice is tied to the blank target track and resets when that target changes, is removed, becomes nonblank, or loses its eligible chain.

### VST Editor Host Windows

Track-owned VST Instrument and VST Effect editor windows enforce a minimum host-window size while remaining resizable from the window corner. Very small plugin editors may leave extra empty host-window space, but the editor toolbar under the title bar keeps **Apply Changes** usable instead of crowding it into the title bar. VST effect toolbars use compact target wording such as `Track #1 - S1 - Reverb`; VST instrument custom title bars include the owning track, for example `Track: #1 - Lead Piano | Instrument | Plugin: Dexed`. Status feedback stays in tooltips/logs instead of a trailing status text label. The **Window** menu also includes side submenus for **VST Instrument** and **VST Effect** whenever those editor windows are open, so you can bring a specific track/slot editor to the front like the Piano Roll window list. When changing a MIDI track to a scanned VST3 instrument, **Choose scanned VST3 plugin** now opens a clear chooser dialog instead of a tiny follow-up popup.

The editor toolbar also includes **Snapshot** slots. Each VST instrument and VST effect editor has five host-side snapshot slots with **Load**, **Save**, **Clear Current**, and **Clear All** buttons. Clear actions ask for confirmation before deleting snapshot files. Snapshots save the plugin state blob for the exact plugin identity and editor role outside the project under `workspace/vst3/snapshots`, so a favorite instrument or effect setting can be reused on another track or project using the same plugin. Snapshot loading is blocked when the saved snapshot does not match the current plugin/role.

### VST Plugin Graphics Adapter

Poor Man's Studio can list the graphics adapters reported by Windows for VST plugin editor windows. The default choice is **System Default / Auto**, which lets Windows and the plugin choose the rendering path.

If multiple adapters are available, you may choose one manually. The selected adapter is a preferred graphics adapter for VST plugin editor windows, not a guarantee; some plugins, drivers, or Windows graphics settings may still choose a different rendering path. This setting does not affect audio render quality. Adapters are labeled as **Hardware** or **Software** when that information is available. Poor Man's Studio does not classify adapters as integrated or dedicated, and it does not use video memory size to make that decision. The adapter list is cached from the last manual refresh and reused on future launches; it is only replaced when you use **Refresh Adapter List** again. The cached adapter rows are stored separately in `workspace/settings/vst_graphics_adapters.txt`; the normal user preferences file keeps only the compact numeric preferred adapter option.

### CLAP Plugin Graphics Adapter And Compatibility

CLAP Settings mirrors the VST3 graphics-adapter pattern where practical. The default choice is **System Default / Auto**, which lets Windows and the plugin decide. **Refresh Adapter List** updates the cached adapter list. **Max Open CLAP Windows** limits how many CLAP editor windows can be open at once, and **Safe CLAP Plugin UI Mode** is available as a compatibility option for fragile plugin editors.

Open CLAP editor tweaks are not silently committed during arming or preview. Use **Apply Changes** in the CLAP editor when you want the plugin state saved to the project. **Test Instrument** captures the current open editor state for its one-off test without applying it to the project, while **Test Effect** processes the shared bundled test sample through the effect.

### Track And Master Gain Range

The main **Track Vol** and **Master Vol** sliders range from `0.00` to `3.00`. `1.00` is normal unity gain. Values above `1.00` boost the signal and may clip, especially when several tracks are mixed together. Render/export and guarded CLAP direct preview use the same shared gain limit so the UI, render path, and live-preview path agree.


Comprehensive user guide: https://github.com/izzaeroth/PoorMansStudio/blob/main/workspace/docs/PoorMansStudio_User_Guide.pdf
