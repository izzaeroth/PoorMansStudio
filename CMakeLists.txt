cmake_minimum_required(VERSION 3.24)

project(PoorMansStudio VERSION 0.56.0)

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
    VERSION "0.56.0"
    ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/resources/app_icon_512.png"
    ICON_SMALL "${CMAKE_CURRENT_SOURCE_DIR}/resources/app_icon_256.png"
)

juce_add_binary_data(PoorMansStudioAssets
    SOURCES
        resources/splash_static.png
        resources/app_icon_512.png
)

target_sources(PoorMansStudio PRIVATE
    src/main.cpp

    src/app/AppPaths.cpp
    src/app/UserPreferences.cpp
    src/app/TempCleaner.cpp

    src/audio/RenderSettings.cpp
    src/audio/RenderPlan.cpp
    src/audio/RenderJob.cpp
    src/audio/ExternalFluidSynthRenderer.cpp
    src/audio/ExternalFfmpegEncoder.cpp
    src/audio/ExternalFfmpegMixer.cpp
    src/audio/ExternalSfizzRenderer.cpp
    src/audio/SfzValidator.cpp
    src/audio/SoundFontPresetReader.cpp
    src/audio/AudioClipImporter.cpp
    src/audio/AudioClipRecorder.cpp

    src/core/InstrumentNameMatcher.cpp

    src/exporting/ExportSettings.cpp

    src/import_export/MusicXmlImporter.cpp
    src/import_export/MidiImporter.cpp

    src/midi/MidiExporter.cpp

    src/serialization/ProjectSerializer.cpp

    src/gui/MainComponent.cpp
    src/gui/PianoRollComponent.cpp
)

target_include_directories(PoorMansStudio PRIVATE
    src
)

target_compile_definitions(PoorMansStudio PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:PoorMansStudio,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:PoorMansStudio,JUCE_VERSION>"
)

target_link_libraries(PoorMansStudio PRIVATE
    juce::juce_gui_extra
    juce::juce_audio_utils
    PoorMansStudioAssets
)

if (WIN32)
    target_link_libraries(PoorMansStudio PRIVATE winmm)
endif()

message(STATUS "PoorMansStudio v0.56.0 configured successfully")


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
