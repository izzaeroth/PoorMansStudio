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
- VST3 Plugin Manager with supported/unsupported panes, search, filtering, details, and horizontal scrolling for long plugin names.
- Manual override for plugins that are safe instruments but detected as unknown/unsupported.
- One VST plugin editor window per track.
- Compact floating **Apply Changes** palette for plugin UI edits.
- Applying track settings closes any open VST editor for that track so stale plugin UIs do not remain attached after a plugin/library change.
- Apply-and-go VST state handling.
- VST state excluded from normal host-side note/edit undo-redo history.
- One-time experimental dragon warning before VST3 becomes active through defaults, track assignment, plugin UI, preview, or render.

Until a future out-of-process helper host is added, VST3 instruments are hosted in-process. That means a plugin crash can still close the main app.

### Preview, Render, And Export

- Preview a selected track, selected sequence, Piano Roll area, or the full project.
- Render the full project, selected track, selected sequence, or stems depending on render settings.
- Export WAV, FLAC, MP3, and OGG audio.
- Export MIDI for use in other tools.
- Configure sample rate, bitrate, channels, output format, and render metadata.
- Use FFmpeg for audio conversion/mixing/compressed output support.
- Use project-relative output folders for safer project sharing.

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
  exports/      rendered audio and exported MIDI
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

AudioClip media is stored beside the project and is not embedded into the `.mwproj` file. Move or share the whole project folder, not just the `.mwproj` file.

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
Version:        0.57.11
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

## Documentation

- Illustrated guide: `workspace/docs/PoorMansStudio_User_Guide.pdf`
- Plain-text user guide: `workspace/docs/USER_GUIDE.txt`
- Detailed setup/build guide: `workspace/docs/SETUP_AND_BUILD_GUIDE.txt`

Use the in-app Help menu to open the guides when running the app.

### VST Plugin Graphics Adapter

Poor Man's Studio can list the graphics adapters reported by Windows for VST plugin editor windows. The default choice is **System Default / Auto**, which lets Windows and the plugin choose the rendering path.

If multiple adapters are available, you may choose one manually. Adapters are labeled as **Hardware** or **Software** when that information is available. Poor Man's Studio does not classify adapters as integrated or dedicated, and it does not use video memory size to make that decision. Use **Refresh Adapter List** when hardware, drivers, monitors, or external GPUs change and you want the adapter list rebuilt.
