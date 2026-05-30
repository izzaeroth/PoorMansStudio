#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "audio/RenderSettings.h"
#include "core/Project.h"

namespace mw::audio
{
    struct TrackRenderPlan
    {
        std::string trackName;
        std::string normalizedInstrumentName;
        mw::core::SampleBackendType backendType = mw::core::SampleBackendType::None;

        std::filesystem::path sampleLibraryPath;

        int midiChannel = 1;
        int midiBank = 0;
        int midiProgram = 0;

        std::string presetName;
        bool needsUserSampleAssignment = false;
    };

    struct ProjectRenderPlan
    {
        std::string projectName;
        std::filesystem::path targetAudioPath;
        AudioExportFormat audioFormat = AudioExportFormat::Wav;

        int sampleRate = 48000;
        int bitDepth = 24;

        std::vector<TrackRenderPlan> tracks;
    };

    class RenderPlanBuilder
    {
    public:
        static ProjectRenderPlan buildRenderPlan(
            const mw::core::Project& project,
            const RenderSettings& settings,
            const std::filesystem::path& targetAudioPath
        );
    };
}
