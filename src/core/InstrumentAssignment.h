#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace mw::core
{
    enum class SampleBackendType
    {
        None,
        SF2,
        SFZ,
        WAV,
        VST3
    };

    inline std::string sampleBackendTypeToString(SampleBackendType type)
    {
        switch (type)
        {
            case SampleBackendType::SF2: return "SF2";
            case SampleBackendType::SFZ: return "SFZ";
            case SampleBackendType::WAV: return "WAV";
            case SampleBackendType::VST3: return "VST3";
            default: return "None";
        }
    }

    struct VstPluginAssignment
    {
        std::filesystem::path bundlePath;
        std::string name;
        std::string vendor;
        std::string version;
        std::string category;
        std::string uid;
        std::string stateBase64;
        // Legacy state-history fields are kept for project-file compatibility only.
        // The plugin editor now uses apply-and-go state capture and clears these values.
        std::vector<std::string> stateHistoryBase64;
        std::vector<std::string> stateRedoBase64;
        bool bypassed = false;
        bool compatibilityWarningSeen = false;
        std::string compatibilitySummary;
    };

    struct InstrumentAssignment
    {
        std::string originalImportedName;
        std::string normalizedName;
        std::string displayName = "Default Instrument";

        SampleBackendType backendType = SampleBackendType::None;
        std::filesystem::path sampleLibraryPath;
        std::string sampleLibraryDisplayName;

        int midiChannel = 1;
        int midiBank = 0;
        int midiProgram = 0;

        std::string presetName = "Default";
        std::string articulationMap = "Default";

        bool wasAutoMatched = false;
        float matchConfidence = 0.0f;

        // VST3 is track-instance state, not a global plugin selection. Multiple
        // tracks may point at the same bundle path but keep independent stateBase64.
        VstPluginAssignment vst3;
    };

    inline InstrumentAssignment makeCustomAudioInstrumentAssignment()
    {
        InstrumentAssignment assignment;
        assignment.originalImportedName = "Custom Audio";
        assignment.normalizedName = "custom audio";
        assignment.displayName = "Custom Audio";
        assignment.backendType = SampleBackendType::WAV;
        assignment.sampleLibraryDisplayName = "AudioClip media";
        assignment.presetName = "Custom Audio";
        assignment.articulationMap = "AudioClip";
        assignment.wasAutoMatched = false;
        assignment.matchConfidence = 1.0f;
        return assignment;
    }
}
