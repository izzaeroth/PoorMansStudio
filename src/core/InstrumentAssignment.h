#pragma once
#include <filesystem>
#include <string>

namespace mw::core
{
    enum class SampleBackendType
    {
        None,
        SF2,
        SFZ,
        WAV
    };

    inline std::string sampleBackendTypeToString(SampleBackendType type)
    {
        switch (type)
        {
            case SampleBackendType::SF2: return "SF2";
            case SampleBackendType::SFZ: return "SFZ";
            case SampleBackendType::WAV: return "WAV";
            default: return "None";
        }
    }

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
