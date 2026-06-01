Poor Man's Studio - VST3 Workspace Notes
==========================================

VST3 support is experimental. Save work before testing unfamiliar plugins. Some plugins can crash in their own native code.

Scan Locations
==============

Poor Man's Studio scans:

- workspace\vst3
- C:\Program Files\Common Files\VST3

A .vst3 item can be a bundle folder. Keep the outer .vst3 bundle intact.

Plugin Manager
==============

Use VST Plugins > VST3 Plugin Manager to review detected plugins and classify them as:

- Supported Instruments
- Supported Effects
- Unsupported

Supported Instruments appear in VST3 instrument dropdowns and in the Change Library scanned-plugin chooser dialog. Supported Effects appear in both track VST Effect dropdowns. Unsupported plugins are hidden from normal assignment workflows. Open VST editor windows are also listed in the Window menu under VST Instrument and VST Effect side submenus.

Two-slot VST Effect Chain
=========================

Each track can own two VST effect slots:

VST Effect 1: [dropdown] [Enable] [Bypass] [Open Slot 1]
VST Effect 2: [dropdown] [Enable] [Bypass] [Open Slot 2]

Processing order is always source -> slot 1 -> slot 2 -> preview/export. Unassigned, disabled, or bypassed slots are skipped, and Open Slot 1/2 is unavailable until the slot has an assigned plugin and Enable is checked. The old single-effect project format loads as slot 1.

Use Apply Track Settings for dropdown/Enable/Bypass changes. Use Apply Changes inside a VST editor for plugin parameter changes. Test Effect in an effect editor uses the bundled short WAV at workspace\vst3\test\pms_vst_effect_test_sample.wav. Keep that file as a normal runtime file; it is not BinaryData.

AudioClip Tracks
================

Imported and recorded AudioClip tracks use the same two-slot effect-chain workflow as MIDI tracks. Source files stay dry. Offline preview/export creates temporary processed WAVs and mixes those into the render.

Snapshots
=========

VST editor snapshot files are stored under workspace\vst3\snapshots. They are matched to plugin identity and editor role.

Graphics Adapter Cache
======================

The cached adapter list is stored in workspace\settings\vst_graphics_adapters.txt. Normal preferences keep only the compact selected adapter value.
