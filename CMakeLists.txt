cmake_minimum_required(VERSION 3.24)

project(PoorMansStudio VERSION 0.67.44)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(JUCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/JUCE")

if (NOT EXISTS "${JUCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "JUCE was not found.\n"
        "Expected JUCE here:\n"
        "  ${JUCE_DIR}\n\n"
        "Download JUCE and place it at:\n"
        "  external/JUCE\n\n"
        "See workspace/docs/SETUP_AND_BUILD_GUIDE.txt"
    )
endif()

add_subdirectory(${JUCE_DIR} external/JUCE-build)

juce_add_gui_app(PoorMansStudio
    PRODUCT_NAME "Poor Man's Studio"
    COMPANY_NAME "PoorMansStudio"
    VERSION "0.67.44"
    ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/resources/app_icon_512.png"
    ICON_SMALL "${CMAKE_CURRENT_SOURCE_DIR}/resources/app_icon_256.png"
)

juce_add_binary_data(PoorMansStudioAssets
    SOURCES
        resources/splash_static.png
        resources/app_icon_512.png
        resources/vst_warning_dragon.png
)

target_sources(PoorMansStudio PRIVATE
    src/main.cpp
    src/app/AppVersion.h

    src/app/AppPaths.cpp
    src/app/UserPreferences.cpp
    src/app/TempCleaner.cpp

    src/audio/ApplicationAudioService.cpp
    src/audio/TailPolicy.cpp
    src/audio/ProjectContentEnding.cpp
    src/audio/RenderJob.cpp
    src/audio/ExternalFluidSynthRenderer.cpp
    src/audio/ExternalFfmpegEncoder.cpp
    src/audio/ExternalFfmpegMixer.cpp
    src/audio/ExternalSfizzRenderer.cpp
    src/audio/SfzValidator.cpp
    src/audio/SoundFontPresetReader.cpp
    src/audio/AudioClipImporter.cpp
    src/audio/AudioClipRecorder.cpp

    src/vst/VstPluginTypes.cpp
    src/vst/VstPluginScanner.cpp

    src/clap/ClapPluginTypes.cpp
    src/clap/ClapPluginScanner.cpp
    src/clap/ClapEffectHost.cpp
    src/clap/ClapInstrumentHost.cpp
    src/clap/ClapLiveInstrumentSession.cpp
    src/clap/ClapLiveEffectSession.cpp
    src/clap/ClapLivePlaybackScheduler.cpp
    src/clap/ClapLiveCallbackBridge.cpp
    src/clap/ClapLiveDirectPreviewEngine.cpp
    src/clap/ClapLiveTrackSessionManager.cpp
    src/clap/ClapEffectEditorHost.cpp
    src/clap/ClapSnapshotStore.cpp
    src/vst/VstGraphicsProfile.cpp
    src/vst/VstInstrumentHost.cpp
    src/vst/VstLiveInstrumentSession.cpp
    src/vst/VstLiveEffectSession.cpp
    src/vst/VstLiveDirectPreviewEngine.cpp
    src/vst/VstLiveTrackSessionManager.cpp
    src/vst/VstSnapshotStore.cpp

    src/core/InstrumentNameMatcher.cpp
    src/core/TrackSelectionPolicy.cpp

    src/playback/PlaybackTransportCoordinator.cpp
    src/playback/MixedLiveProjectPreviewEngine.cpp

    src/exporting/ExportSettings.cpp

    src/import_export/MusicXmlImporter.cpp
    src/import_export/MidiImporter.cpp

    src/midi/MidiExporter.cpp
    src/midi/MidiControllerLane.cpp
    src/midi/MidiPitchBendLane.cpp
    src/midi/MidiProgramChangeLane.cpp
    src/midi/MidiChannelPressureLane.cpp
    src/midi/MidiPolyPressureLane.cpp
    src/midi/Midi14BitControllerLane.cpp
    src/midi/MidiAdvancedParameterDecoder.cpp
    src/midi/MidiParameterLane.cpp
    src/midi/MidiInputCoordinator.cpp
    src/midi/MidiRecordingSession.cpp
    src/midi/MidiRecordingTransportSync.cpp
    src/midi/MidiRecordingCountIn.cpp
    src/midi/MidiRecordingCountInPlayer.cpp
    src/midi/MidiRecordingQuantizer.cpp
    src/midi/MidiRecordingMetronome.cpp
    src/midi/MidiRecordingMetronomeAudio.cpp
    src/midi/MidiRecordingControlPolicy.cpp
    src/midi/MidiRecordingPunch.cpp
    src/midi/MidiRecordingLoop.cpp
    src/midi/MidiRecordingTakeReview.cpp
    src/midi/MidiRecordingComp.cpp
    src/midi/MidiPendingTakeReviewManager.cpp

    src/simulation/SimulationRegressionSuite.cpp

    src/serialization/ProjectSerializer.cpp

    src/gui/MainComponent.cpp
    src/gui/MidiControllerLaneComponent.cpp
    src/gui/MidiPitchBendLaneComponent.cpp
    src/gui/MidiProgramChangeLaneComponent.cpp
    src/gui/MidiChannelPressureLaneComponent.cpp
    src/gui/MidiPolyPressureLaneComponent.cpp
    src/gui/Midi14BitControllerLaneComponent.cpp
    src/gui/MidiParameterLaneComponent.cpp
    src/gui/AudioClipEditorComponent.cpp
    src/gui/PianoRollComponent.cpp
)

target_include_directories(PoorMansStudio PRIVATE
    src
)

target_compile_definitions(PoorMansStudio PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_AU=0
    JUCE_PLUGINHOST_AAX=0
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:PoorMansStudio,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:PoorMansStudio,JUCE_VERSION>"
)

target_link_libraries(PoorMansStudio PRIVATE
    juce::juce_gui_extra
    juce::juce_audio_utils
    juce::juce_audio_processors
    PoorMansStudioAssets
    ${CMAKE_DL_LIBS}
)


# Hardware/plugin-free deterministic regression runner. This target intentionally
# uses only standard C++ plus the JUCE-independent MIDI recording core so it can
# exercise Stages A-E contracts without physical devices or third-party plugins.
add_executable(PoorMansStudioSim
    src/simulation/PoorMansStudioSimMain.cpp
    src/simulation/SimulationRegressionSuite.cpp
    src/audio/TailPolicy.cpp
    src/audio/ProjectContentEnding.cpp
    src/core/TrackSelectionPolicy.cpp
    src/core/InstrumentNameMatcher.cpp
    src/import_export/MidiImporter.cpp
    src/midi/MidiExporter.cpp
    src/playback/PlaybackTransportCoordinator.cpp
    src/midi/MidiRecordingSession.cpp
    src/midi/MidiRecordingTransportSync.cpp
    src/midi/MidiRecordingCountIn.cpp
    src/midi/MidiRecordingQuantizer.cpp
    src/midi/MidiRecordingMetronome.cpp
    src/midi/MidiRecordingControlPolicy.cpp
    src/midi/MidiRecordingPunch.cpp
    src/midi/MidiRecordingLoop.cpp
    src/midi/MidiRecordingTakeReview.cpp
    src/midi/MidiRecordingComp.cpp
    src/midi/MidiPendingTakeReviewManager.cpp
    src/midi/MidiControllerLane.cpp
    src/midi/MidiPitchBendLane.cpp
    src/midi/MidiProgramChangeLane.cpp
    src/midi/MidiChannelPressureLane.cpp
    src/midi/MidiPolyPressureLane.cpp
    src/midi/Midi14BitControllerLane.cpp
    src/midi/MidiAdvancedParameterDecoder.cpp
    src/midi/MidiParameterLane.cpp
    src/clap/ClapLivePlaybackScheduler.cpp
    src/serialization/ProjectSerializer.cpp
)

target_include_directories(PoorMansStudioSim PRIVATE
    src
)

if (MSVC)
    target_compile_options(PoorMansStudioSim PRIVATE /W4 /permissive-)
else()
    target_compile_options(PoorMansStudioSim PRIVATE -Wall -Wextra -Wpedantic)
endif()

juce_add_console_app(PoorMansStudioVstHost
    PRODUCT_NAME "PoorMansStudioVstHost"
    COMPANY_NAME "PoorMansStudio"
    VERSION "0.67.44"
)

target_sources(PoorMansStudioVstHost PRIVATE
    src/vst_host_helper/PoorMansStudioVstHostMain.cpp
    src/app/AppVersion.h

    src/app/AppPaths.cpp

    src/vst/VstPluginTypes.cpp
    src/vst/VstPluginScanner.cpp
    src/vst/VstHostProbe.cpp
)

target_include_directories(PoorMansStudioVstHost PRIVATE
    src
)

target_compile_definitions(PoorMansStudioVstHost PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_AU=0
    JUCE_PLUGINHOST_AAX=0
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:PoorMansStudioVstHost,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:PoorMansStudioVstHost,JUCE_VERSION>"
)

target_link_libraries(PoorMansStudioVstHost PRIVATE
    juce::juce_core
    juce::juce_audio_processors
)



juce_add_console_app(PoorMansStudioClapHost
    PRODUCT_NAME "PoorMansStudioClapHost"
    COMPANY_NAME "PoorMansStudio"
    VERSION "0.67.44"
)

target_sources(PoorMansStudioClapHost PRIVATE
    src/clap_host_helper/PoorMansStudioClapHostMain.cpp
    src/app/AppVersion.h

    src/app/AppPaths.cpp

    src/clap/ClapPluginTypes.cpp
    src/clap/ClapPluginScanner.cpp
    src/clap/ClapAbiProbe.cpp
)

target_include_directories(PoorMansStudioClapHost PRIVATE
    src
)

target_compile_definitions(PoorMansStudioClapHost PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:PoorMansStudioClapHost,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:PoorMansStudioClapHost,JUCE_VERSION>"
)

target_link_libraries(PoorMansStudioClapHost PRIVATE
    juce::juce_core
    ${CMAKE_DL_LIBS}
)

if (WIN32)
    target_link_libraries(PoorMansStudio PRIVATE winmm dxgi)
    target_link_libraries(PoorMansStudioVstHost PRIVATE winmm)
    target_link_libraries(PoorMansStudioClapHost PRIVATE winmm)
endif()

message(STATUS "PoorMansStudio v0.67.44 configured successfully")


# Copy the built executable to a stable runtime location.
# This keeps the normal JUCE/CMake artefact output intact while also creating:
#   workspace/program/Poor Man's Studio.exe
add_custom_command(TARGET PoorMansStudio POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/workspace/program"

    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:PoorMansStudio>"
        "${CMAKE_CURRENT_SOURCE_DIR}/workspace/program/$<TARGET_FILE_NAME:PoorMansStudio>"
)


# Copy the simulation/regression runner to:
#   workspace/simulation/PoorMansStudioSim.exe
add_custom_command(TARGET PoorMansStudioSim POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/workspace/simulation"

    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:PoorMansStudioSim>"
        "${CMAKE_CURRENT_SOURCE_DIR}/workspace/simulation/$<TARGET_FILE_NAME:PoorMansStudioSim>"
)

# Copy the VST host helper to its reserved runtime/helper location:
#   workspace/vst_host/PoorMansStudioVstHost.exe
add_custom_command(TARGET PoorMansStudioVstHost POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/workspace/vst_host"

    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:PoorMansStudioVstHost>"
        "${CMAKE_CURRENT_SOURCE_DIR}/workspace/vst_host/$<TARGET_FILE_NAME:PoorMansStudioVstHost>"
)


# Copy the CLAP host helper to its reserved runtime/helper location:
#   workspace/clap_host/PoorMansStudioClapHost.exe
add_custom_command(TARGET PoorMansStudioClapHost POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/workspace/clap_host"

    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:PoorMansStudioClapHost>"
        "${CMAKE_CURRENT_SOURCE_DIR}/workspace/clap_host/$<TARGET_FILE_NAME:PoorMansStudioClapHost>"
)
