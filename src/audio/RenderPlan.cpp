#include "audio/RenderPlan.h"

namespace mw::audio
{
    ProjectRenderPlan RenderPlanBuilder::buildRenderPlan(
        const mw::core::Project& project,
        const RenderSettings& settings,
        const std::filesystem::path& targetAudioPath
    )
    {
        ProjectRenderPlan plan;
        plan.projectName = project.getName();
        plan.targetAudioPath = targetAudioPath;
        plan.audioFormat = settings.audioFormat;
        plan.sampleRate = settings.sampleRate;
        plan.bitDepth = settings.bitDepth;

        for (const auto& track : project.getTracks())
        {
            const auto& instrument = track.getInstrument();

            TrackRenderPlan trackPlan;
            trackPlan.trackName = track.getName();
            trackPlan.normalizedInstrumentName = instrument.normalizedName;
            trackPlan.backendType = settings.preferredBackend;
            trackPlan.midiChannel = instrument.midiChannel;
            trackPlan.midiBank = instrument.midiBank;
            trackPlan.midiProgram = instrument.midiProgram;
            trackPlan.presetName = instrument.presetName;

            switch (settings.preferredBackend)
            {
                case mw::core::SampleBackendType::SF2:
                    trackPlan.sampleLibraryPath = settings.defaultSf2Path;
                    break;

                case mw::core::SampleBackendType::SFZ:
                    trackPlan.sampleLibraryPath = settings.defaultSfzFolder;
                    break;

                case mw::core::SampleBackendType::WAV:
                    trackPlan.sampleLibraryPath = settings.defaultWavSampleFolder;
                    break;

                default:
                    trackPlan.sampleLibraryPath.clear();
                    break;
            }

            trackPlan.needsUserSampleAssignment = trackPlan.sampleLibraryPath.empty();
            plan.tracks.push_back(trackPlan);
        }

        return plan;
    }
}
