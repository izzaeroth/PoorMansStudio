# Poor Man's Studio

**Poor Man's Studio** is a lightweight local Windows music workstation for importing, arranging, editing, previewing, saving, and rendering music projects without needing a full commercial DAW.

It is intentionally practical: bring in MIDI or score material, clean up tracks, organize sections, assign instruments, preview ideas quickly, and render usable audio or MIDI output from a self-contained workspace. It has just enough DAW energy to be useful, without trying to become a spaceship cockpit.

## Quick Feature Tour

### Import, Arrange, And Edit

- Import MusicXML, compressed MXL, MIDI, Poor Man's Studio `.mwproj`, WAV, MP3, FLAC, and OGG files.
- Create MIDI tracks from score/MIDI files.
- Create AudioClip tracks from imported or recorded audio.
- Edit notes in the main editor and in dedicated Piano Roll windows.
- Keep one Piano Roll window per track so track edits stay understandable.
- Organize track groups, song sections, sequence assignments, ordering, names, and track layout through the Track Manager.
- Save projects as `.mwproj` files with project-relative media paths.

### Instruments And Sound Sources

- Use SF2/SF3 SoundFonts through FluidSynth.
- Use SFZ instruments through sfizz-render.
- Use experimental VST3 instrument plugins through JUCE VST3 hosting.
- Set project defaults for future imported/new MIDI tracks.
- Apply per-track settings intentionally so existing tracks are not overwritten accidentally.
- Switch a track between SF2/SF3/SFZ/VST3 without deleting its MIDI notes.
- Preserve notes when changing libraries; the instrument assignment changes, the music stays.

### VST3 Plugin Support

VST3 support is experimental. Some plugins are perfectly calm. Some plugins are dragons wearing DLL costumes.

Current VST3 support includes:

- VST3 scanning from `workspace/vst3` and standard Windows VST3 locations.
- VST3 Plugin Manager with **All Plugins**, **Supported Instruments**, **Supported Effects**, and **Unsupported** filters.
- Two-slot VST Effect chain controls in the main track inspector: choose Supported Effects for slot 1 and slot 2, enable/disable or bypass each slot, save the assignments in `.mwproj` files, and process the chain during offline preview/render paths for MIDI and AudioClip tracks.
- Fixed effect-chain order: source track -> VST Effect 1 -> VST Effect 2 -> preview/export output. Unassigned, disabled, or bypassed slots are skipped.
- VST editor host windows use a small toolbar under the title bar for **Apply Changes**; tiny plugin UIs get a wider minimum host size while remaining resizable.
- Manual override for plugins that are safe instruments or effect candidates but detected as unknown/unsupported.
- One VST plugin editor window per track instrument assignment and one VST effect editor window per track/effect slot.
- Track-owned VST editor toolbar with **Apply Changes**, a compact target label such as `Track #1 - S1 - Reverb`, and **Test Effect** for effect editors.
- Per-track VST state save/restore so two tracks using the same plugin can keep different sounds.
- VST state excluded from normal host-side note/edit undo-redo history.
- VST Host Helper Status from the VST Plugins menu, backed by the helper executable.
- One-time experimental dragon warning before VST3 becomes active through defaults, track assignment, plugin UI, preview, or render.

Normal VST3 instrument hosting is still mostly in-process, so a plugin crash can still close the main app. The helper executable provides isolated checks and groundwork for future risky plugin work.

### Preview, Render, And Export

- Preview a selected track, selected sequence, Piano Roll area, or the full project.
- Render the full project, selected track, selected sequence, or stems depending on render settings.
- Export WAV, FLAC, MP3, and OGG audio.
- Export MIDI for use in other tools.
- Configure sample rate, bitrate, channels, output format, and render metadata.
- Use FFmpeg for audio conversion/mixing/compressed output support.
- Use `workspace/exports` as the default user-facing render/export destination.


### Export Folder Behavior

Poor Man's Studio keeps user-facing renders in `workspace/exports` by default instead of auto-switching exports into each project folder. Saving or opening a `.mwproj` no longer retargets the export box to `workspace/projects/<project>/renders`; older project-local `renders`/`output` paths are reset back to the workspace export folder unless you manually choose another custom export folder.

Render Settings retention applies consistently to non-WAV exports too. When **Keep WAV audio stems** is enabled, the source WAV sidecar used for MP3/FLAC/OGG encoding is kept next to the final export; when **Keep MIDI stem files** is enabled, generated MIDI sidecars/stems are kept. Preview/temp renders still clean themselves under `workspace/temp`.

### Usability And Project Safety

- Helper bubbles for in-app guidance.
- Plain key/value settings file that can be inspected by hand.
- Settings save behavior separates explicit full settings saves from narrow one-key acknowledgements.
- Dirty-state prompts so unapplied editor changes are not silently discarded.
- Workspace cleanup for temporary files.
- Theme preset support.
- Included PDF and text guides under `workspace/docs`.

## Normal User Workflow

1. Choose a project backend in Project Defaults.
2. Choose the matching library, preset, SFZ file, or VST3 plugin.
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
  workspace/
    docs/
    exports/
    ffmpeg/
    fluidsynth/
    input/
    projects/
    settings/
    sfizz/
    sfz/
    soundfonts/
    temp/
    themes/
    vst3/
  external/
    JUCE/
```

The `external/JUCE` folder is required for building from source. It is not bundled as a runtime dependency for end users.

## Workspace Layout

```text
workspace/
  input/        import staging area
  exports/      default render/export destination for audio, MIDI, and kept WAV sidecars/stems
  projects/     .mwproj project folders
  soundfonts/   user SF2/SF3 files
  fluidsynth/   optional FluidSynth runtime/tool folder
  ffmpeg/       optional FFmpeg runtime/tool folder
  sfz/          user SFZ instruments and sample packs
  vst3/         portable/test VST3 bundles
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

AudioClip media is not embedded into the `.mwproj` file. Newly imported files and Save / Apply recorded takes stage first under the active `workspace/temp/recordings/rec_...` session. **Save Project** is the commit point that moves attached staged media into the saved project folder under `input/audio/imported` or `input/audio/recorded`. Import Audio should not create a new folder under `workspace/projects` until the project is actually saved, and it should not leave unreferenced media inside an already saved project folder if the user later discards changes. Choosing **Discard** from New/Open/Start From File/Exit unsaved-change prompts removes the active staged AudioClip session so abandoned imports and takes do not carry into the next project. Move or share the whole saved project folder, not just the `.mwproj` file.

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
- CMake 3.24 or newer.
- Git for Windows, recommended for obtaining/updating JUCE.
- JUCE source tree at `external/JUCE`.
- C++20-capable compiler.

The project CMake file currently requires:

```cmake
cmake_minimum_required(VERSION 3.24)
set(CMAKE_CXX_STANDARD 20)
```

JUCE's own CMake support requires CMake 3.22 or newer, but this project requires 3.24 or newer.

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
| CMake 3.24+ | Configures/generates builds | The project uses CMake directly. |
| Git | Cloning/updating source and JUCE | Optional if JUCE is copied manually, recommended otherwise. |
| JUCE source tree | C++ app framework | Must exist at `external/JUCE`. Build-time source dependency, not a separate runtime install. |

## Runtime Dependencies And Resources

These are used when running the built app or rendering projects:

| Runtime item | Purpose | Bundled? |
| --- | --- | --- |
| FluidSynth | SF2/SF3 SoundFont rendering | May be placed/configured in workspace; not a JUCE dependency. |
| FFmpeg | Audio conversion, mixing, compressed exports | May be placed/configured in workspace. |
| sfizz-render | SFZ rendering | May be placed/configured in workspace. |
| SoundFonts / SF3 files | User instrument libraries | User supplied. |
| SFZ sample packs | User instrument libraries | User supplied. |
| VST3 plugins | Experimental plugin instruments | User supplied or installed system-wide. |
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
Version:        0.60.6
Configuration:  Release
Generator:      Visual Studio 18 2026
Platform:       x64
Parallel Jobs:  8
Started:        2026-05-30 14:08:12
Finished:       2026-05-30 14:11:46
Elapsed:        3 min 34 sec
Result:         SUCCESS
Output:         C:\Dev\PoorMansStudio\workspace\program\Poor Man's Studio.exe
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

Normal VST3 instrument hosting and the Track Live Effect recorder monitor path remain in-process. The helper is groundwork for risky scanning, plugin editor isolation, state capture, offline helper rendering, and later shared-memory live processing. The app checks the helper once at startup and exposes the cached result through **VST Plugins > VST Host Helper Status...**.

## Documentation

- Comprehensive PDF user guide: `workspace/docs/PoorMansStudio_User_Guide.pdf`
- Plain-text user guide: `workspace/docs/USER_GUIDE.txt`
- Detailed setup/build guide: `workspace/docs/SETUP_AND_BUILD_GUIDE.txt`

Use the in-app Help menu to open the guides when running the app.

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

Imported and recorded AudioClip source media stays dry. During offline preview/export, enabled non-bypassed slots are applied to temporary processed WAV files and those temporary files are mixed into the render. Imported and recorded AudioClip tracks use the same workflow. New AudioClip media stages in the active `workspace/temp/recordings/rec_...` session; **Save Project** moves only applied/attached staged clips into the saved project folder, while **Discard** removes the current staging session instead of leaving its imported/recorded media for a later project.

### AudioClip Recorder Track Live Effect

The AudioClip Recorder includes a **Track Live Effect** checkbox. When enabled, the recorder tries to monitor the live input through the selected target track's first active VST effect slot while the recorded WAV remains dry. This is monitor-only and is not the final effect-render path. Offline preview/export remains the commit path for applying the track's VST Effect chain. Recording targets the selected existing track instead of auto-creating a new track. Create/select the track first, assign its effects, then use Track Live Effect to monitor while the saved clip remains dry.

### VST Editor Host Windows

Track-owned VST Instrument and VST Effect editor windows enforce a minimum host-window size while remaining resizable from the window corner. Very small plugin editors may leave extra empty host-window space, but the editor toolbar under the title bar keeps **Apply Changes** usable instead of crowding it into the title bar. VST effect toolbars use compact target wording such as `Track #1 - S1 - Reverb`; VST instrument custom title bars include the owning track, for example `Track: #1 - Lead Piano | Instrument | Plugin: Dexed`. Status feedback stays in tooltips/logs instead of a trailing status text label. The **Window** menu also includes side submenus for **VST Instrument** and **VST Effect** whenever those editor windows are open, so you can bring a specific track/slot editor to the front like the Piano Roll window list. When changing a MIDI track to a scanned VST3 instrument, **Choose scanned VST3 plugin** now opens a clear chooser dialog instead of a tiny follow-up popup.

The editor toolbar also includes **Snapshot** slots. Each VST instrument and VST effect editor has five host-side snapshot slots with **Load**, **Save**, **Clear Current**, and **Clear All** buttons. Clear actions ask for confirmation before deleting snapshot files. Snapshots save the plugin state blob for the exact plugin identity and editor role outside the project under `workspace/vst3/snapshots`, so a favorite instrument or effect setting can be reused on another track or project using the same plugin. Snapshot loading is blocked when the saved snapshot does not match the current plugin/role.

### VST Plugin Graphics Adapter

Poor Man's Studio can list the graphics adapters reported by Windows for VST plugin editor windows. The default choice is **System Default / Auto**, which lets Windows and the plugin choose the rendering path.

If multiple adapters are available, you may choose one manually. The selected adapter is a preferred graphics adapter for VST plugin editor windows, not a guarantee; some plugins, drivers, or Windows graphics settings may still choose a different rendering path. This setting does not affect audio render quality. Adapters are labeled as **Hardware** or **Software** when that information is available. Poor Man's Studio does not classify adapters as integrated or dedicated, and it does not use video memory size to make that decision. The adapter list is cached from the last manual refresh and reused on future launches; it is only replaced when you use **Refresh Adapter List** again. The cached adapter rows are stored separately in `workspace/settings/vst_graphics_adapters.txt`; the normal user preferences file keeps only the compact numeric preferred adapter option.
