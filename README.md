# Poor Man's Studio

## Version

v0.56.0

## Purpose

Poor Man's Studio is a local Windows desktop music workstation for practical score/MIDI import, track editing, sequence arranging, AudioClip recording/import, preview playback, and final rendering without requiring a full commercial DAW.

The app is built around a predictable workspace layout so projects, imported media, recordings, renders, settings, tools, SoundFonts, SFZ libraries, and temporary files stay organized.

## Quick Start For Users

- Open the app and use **Start From File** to import MusicXML/MXL, MIDI, an existing `.mwproj`, or supported audio.
- Use **Track Manager** for sequence/track organization, preview, and render actions.
- Use **Record AudioClip** to record custom audio into the active sequence.
- Use **Help > Open User Guide PDF** for the illustrated guide at `workspace/docs/PoorMansStudio_User_Guide.pdf`.

## Repository And Build Notes

This repository is intended to hold the app source, resources, and current-use documentation. Local user workspace content, downloaded instrument packs, generated builds, and large media outputs should stay outside Git or remain ignored.

A full local JUCE build requires `external/JUCE` to be present locally. That folder is intentionally treated as a local build dependency and is not part of the packaged source bundle.



## GitHub Repository Notes

For first-time GitHub setup, create an empty repository and leave GitHub's optional README, `.gitignore`, and license choices off. This bundle provides `README.md`, and the import helper script can create/update `.gitignore` without causing first-push merge conflicts. Add a `LICENSE` file later once you decide the correct license for the app and any redistributed tools.

The ChatGPT continuation markdown is a separate helper file for future project context. It is not part of the main app bundle and is normally not committed to the public source repository.

## Binary And Runtime Packaging

The recommended source repository keeps local build outputs and runnable binary packages out of Git. Put local runnable files in a `program/` folder when needed, but publish the finished runnable ZIP through GitHub Releases rather than committing large binaries directly to the source history.

If you intentionally want a self-contained repository that tracks `program/`, use the import helper's `-IncludeProgramFolder` option and verify that each bundled dependency/tool can legally be redistributed.

## Current Feature Set

- Import MusicXML, MXL, MIDI, `.mwproj`, WAV, MP3, FLAC, and OGG.
- Edit MIDI notes with the Piano Roll.
- Open multiple Piano Roll windows, limited to one window per track.
- Use the Window menu to jump to open app/tool windows, with multiple open Piano Rolls grouped in a submenu.
- Move tracks between sequences without closing open Piano Roll windows; unsaved Piano Roll note edits and pending track settings are preserved.
- Keep Track Manager consoles and the Change Track Seq picker synchronized with current sequence membership, showing empty sequences explicitly.
- Organize tracks and sequences with Track Manager.
- Assign SF2/SF3/SFZ instruments to MIDI tracks from the main UI or Piano Roll windows.
- Assign new blank MIDI tracks to the first available instrument in the project default library when possible.
- Show each Piano Roll track's current or pending instrument in the Piano Roll header.
- Split Piano Roll pitch labels into separate octave and note columns, with accidental icons on sharp/flat-style rows.
- Scroll the Piano Roll pitch view smoothly with the mouse wheel or scrollbar while keeping note up/down controls step-based.
- Import or record audio as AudioClip tracks.
- Use Record Test for a temporary mic check: 3-second countdown, 5-second capture, automatic playback, then automatic cleanup.
- Adjust AudioClip Recorder Mic Gain as software input recording gain from -24.0 dB to +24.0 dB.
- Treat each imported or recorded audio file as one AudioClip and one AudioClip track.
- Use Custom Audio as the built-in identity for AudioClip tracks.
- Preview tracks, sequences, or the project, including AudioClip content.
- Render final audio with FluidSynth, sfizz-render, and FFmpeg.
- Choose Parallel Stem Renders as Auto (safe), 1, 2, 4, 6, 8, 12, or 16. This controls how many independent MIDI stem render jobs may run at the same time; it is not a literal CPU-core selector.
- Export MIDI separately from audio rendering.
- Save/load `.mwproj` project metadata.
- Prompt to save, discard, or cancel before Start From File replaces a dirty project.
- Store large AudioClip media files beside the project instead of inside `.mwproj`.
- Use helper bubbles on controls after about 2 seconds of hover; they are enabled by default and can be toggled from Help > Helper Bubbles, and the choice is saved for the next launch.

## Workspace Layout

Recommended runtime structure:

```text
workspace/
  input/
  exports/
  projects/
  soundfonts/
  fluidsynth/
  ffmpeg/
  sfz/
  sfizz/
  settings/
  temp/
  themes/
```

Recommended project structure:

```text
workspace/
  projects/
    Project Name/
      Project Name.mwproj
      input/
        midi/
        mxl/
        audio/
          imported/
          recorded/
          temp/
      output/
      renders/
```

## Project File Model

`.mwproj` stores metadata, not bulk audio data.

Project identity is normalized on save/open so the project folder name, `.mwproj` filename, project name, and base name stay aligned. The default save path is `workspace/projects/<Project Name>/<Project Name>.mwproj`; if the user intentionally chooses or saves a different export folder, that choice is respected.

Project metadata includes:

- tracks
- note events
- sequences
- sequence colors
- sequence thoughts/notes
- project defaults
- per-track instrument assignments
- render settings
- AudioClip metadata
- project-relative media paths

AudioClip files live in the project folder under `input/audio`.

## Track Types

### MIDI Track

A MIDI track contains note events and uses an assigned instrument library.

Supported MIDI track instrument sources:

- SF2
- SF3
- SFZ

MIDI tracks are editable in the Piano Roll. Multiple Piano Roll windows can be open at once, but each track is limited to one Piano Roll window. Reopening a track that already has a Piano Roll open focuses that existing window instead of reloading the roll. Piano Roll windows also show the track sound library, including the active SF2/SFZ backend in the library text, allow Change Library for SF2/SF3/SFZ sources, and include an Instrument dropdown. Piano Roll library and instrument selections are staged until Apply Track Settings is clicked, so changing those controls does not reload the roll or discard unsaved note edits.

### AudioClip Track

An AudioClip track contains one imported or recorded audio file.

Current rule:

```text
1 imported/recorded audio file = 1 AudioClip = 1 AudioClip track
```

AudioClip tracks use the built-in `Custom Audio` identity. They are managed like normal tracks in Track Manager and are included in preview/render paths.

## AudioClip Import

Import Audio supports:

- WAV
- MP3
- FLAC
- OGG

Imported audio is copied or converted into the current project folder and stored as an AudioClip track. The `.mwproj` stores metadata and relative paths only.

Saved AudioClip formats include:

- WAV
- FLAC
- MP3
- OGG

WAV/FLAC are lossless options. MP3/OGG use high-quality compressed defaults when selected.

## AudioClip Recorder

The recorder opens without auto-recording.

Recorder behavior:

- Input device dropdown applies immediately.
- Refresh Devices rescans audio input devices.
- Mic Gain applies software recording gain inside Poor Man's Studio before the WAV writer; use small boosts to avoid added noise or clipping.
- Record Test runs a fixed 3-second countdown, records a 5-second temporary mic check, automatically plays it back once, and deletes the temporary audio.
- Record Test does not create an AudioClip track and does not show Save/Keep/Delete controls.
- Record starts after a 0.25 second safety delay.
- Record With Delay supports 3, 5, 10, or custom delay up to 99 seconds.
- Countdown overlay uses a semi-transparent black background and white text.
- Countdown ends with `GO!` instead of `0`.
- Pause fully pauses without writing a silent gap.
- Record or Record With Delay resumes/appends to the same take after pause.
- Stop ends capture and leaves the take available.
- Save / Apply commits the take as an AudioClip track.
- Redo From Top discards the current temp take and starts over from the same placement.
- Discard Take cleans up temp audio.
- Closing with a dirty take offers Save Take and Close, Discard and Close, or Cancel.

## Track Manager

Track Manager is the main control area for track and sequence organization. Main render actions live on the main UI.

It handles:

- selecting tracks
- selecting sequences
- adding/importing tracks into the currently selected sequence
- start beat edits
- sequence assignment changes
- Duplicate Beat
- Preview Track / Preview Seq / Preview Project
- Edit Thoughts for sequences
- map/view-bar display
- AudioClip tracks as normal tracks

The map/view bar should account for full MIDI and AudioClip duration, repaint after selection/edit/undo changes, and recalculate after resize/maximize. The View bar should stay plain black until a sequence is focused; then it should show a single focused-sequence highlight whose position and length match that sequence within the full project.

## Sequence Thoughts

Each sequence can store thoughts/notes.

- Main UI shows Thoughts read-only.
- Track Manager Edit Thoughts opens the editable text popup.
- Dirty-state prompts prevent accidental discard.
- Thoughts save as sequence `notes` metadata in `.mwproj`.

## Active Sequence Picker

The main UI Active Seq field is read-only with a Chg Seq button. When a track is selected, Chg Seq changes that selected track's sequence assignment and commits the membership immediately so changing away and back to the track retains the new sequence.

The picker uses a scrollable list so long sequence names are not hidden.

- OK applies the selected existing sequence to the selected track when a track is active.
- Create Blank Seq creates an empty sequence in the picker only; it does not create a blank track.
- Cancel closes without changes.

## Preview And Render

Preview and render paths support mixed MIDI and AudioClip projects. Sequence preview/render trims and rebases the selected sequence range into a short render snapshot so sequence previews do not render silent time from the larger project. Preview buttons are disabled while a render/preview is already running.

Console/log panes support Ctrl+C when focused and a right-click Copy Console Text menu.

Rendering tools:

- FluidSynth for SF2/SF3 MIDI tracks
- sfizz-render for SFZ MIDI tracks
- FFmpeg for encoding, compressed output, and mixdown
- project-local AudioClip media for imported/recorded audio tracks

Parallel Stem Renders:

- This setting controls how many independent MIDI stem render jobs may run at the same time. It is not a literal CPU-core selector.
- Auto (safe) chooses a conservative limit from the CPU hardware-thread count, then also limits by the number of eligible independent stems:
  - 2-3 CPU threads: 1 parallel stem render
  - 4-7 CPU threads: 2 parallel stem renders
  - 8-11 CPU threads: 4 parallel stem renders
  - 12-15 CPU threads: 6 parallel stem renders
  - 16+ CPU threads: 8 parallel stem renders
- Manual choices are 1, 2, 4, 6, 8, 12, and 16. Manual 12 or 16 is a hardcore override and should only be used when the computer can handle more simultaneous SoundFont/SFZ render jobs.
- Parallelism is limited to independent stem rendering. Final FFmpeg mix/encode stays serial.
- Cancel Render requests cancellation for all active parallel stem renders and terminates FluidSynth/sfizz-render child processes where supported.

Render Settings:

- The Render Settings popout controls which stem files are kept after successful user renders.
- Click Render Settings, choose WAV audio stems and/or MIDI stem files, then click OK. Cancel closes without changing the saved setting.
- The global settings file stores this as keepStemFiles=0, 1, 2, or 1,2:
  - 0: keep no stem files
  - 1: keep WAV audio stems only
  - 2: keep MIDI stem files only
  - 1,2: keep both WAV and MIDI stems
- Kept stems are organized under the render stem folder as audio/ for WAV files and midi/ for MIDI files.
- Preview/temp renders still clean up their generated stems.

Supported output formats:

- WAV
- FLAC
- MP3
- OGG
- MIDI export

## Window Headers And Icons

Tool windows use a consistent light-gray custom header with the existing icon artwork, title text, blue `-` minimize, green `+` maximize/restore, and red `x` close controls.

The AudioClip Recorder uses a dark microphone icon so it is visible in the title/header area. The Sequence Color window uses a color-wheel icon. Smaller utility/confirmation dialogs can continue using the default app icon.


## Helper Bubbles

Helper bubbles are enabled by default and appear after about 2 seconds of hover. Use Help > Helper Bubbles to turn them on or off. The choice is saved as a user preference for the next launch.

They are for real controls such as buttons, dropdowns, input fields, and labels.

They should not appear on console panes, status/timer displays, or large read-only info areas.

The AudioClip Recorder has helper bubbles for its controls, including device selection and delay options.

## Temp And Storage Safety

The app should check free space before large import, conversion, and recording operations.

Temp cleanup should cover:

- failed audio imports/conversions
- canceled recordings
- discarded takes
- redo-from-top takes
- app-owned preview/render files

Temp cleanup should avoid deleting user-owned media outside managed app/project temp folders.

## Build Dependencies

Build dependencies:

- Visual Studio C++ build tools
- CMake
- JUCE in `external/JUCE`

Runtime/rendering dependencies:

- FluidSynth in `workspace/fluidsynth`
- FFmpeg in `workspace/ffmpeg`
- sfizz-render in `workspace/sfizz`
- SF2/SF3 libraries in `workspace/soundfonts`
- SFZ libraries in `workspace/sfz`

## Documentation

Detailed docs:

```text
workspace/docs/SETUP_AND_BUILD_GUIDE.txt
workspace/docs/PoorMansStudio_User_Guide.pdf
workspace/docs/USER_GUIDE.txt
```

Window headers
- Main UI and major tool windows use a compact darker light-gray custom header.
- Header controls use literal colored buttons: blue -, green +, and red x.
- The custom header height is 32 px, slightly shorter than the previous custom header but still taller than a default/native title bar.
- Edit Thoughts uses the note/pencil icon, Track Manager uses the track icon, AudioClip Recorder uses the microphone icon, and Sequence Color uses the color wheel icon.
- About and the main UI Chg Seq picker use simple title-barless dialog styling; the Chg Seq picker content remains scrollable.

### SFZ sidecar pack layout

Poor Man's Studio supports `workspace/sfz/Name.sfz` plus `workspace/sfz/Name/`. The `Name/` folder is the pack root and may contain instrument/category folders directly; a `samples/` subfolder is not required. The SFZ option list should present the pair as one public pack option, and SFZ preflight/render should resolve relative `sample=` and `#include` paths against the pack root when the paths are not found beside the `.sfz` file.


AudioClip Recorder updates in v0.56.0:
- Record Test: 3 second countdown, 5 second temporary capture, automatic playback, automatic cleanup.
- Mic Gain: software input gain with - / + controls and a 0 dB reset.

## License

No project license file has been selected yet. Choose and add a `LICENSE` file before inviting outside contributors or publishing this as an open-source project.
