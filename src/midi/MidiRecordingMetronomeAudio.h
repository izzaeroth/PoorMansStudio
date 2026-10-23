#pragma once

#include "midi/MidiRecordingMetronome.h"

#include <filesystem>
#include <string>

namespace mw::midi
{
    bool createMidiRecordingMetronomeBackingWav(
        const std::filesystem::path& sourceWavPath,
        const std::filesystem::path& outputWavPath,
        const MidiRecordingMetronomeConfig& config,
        double activeStartSeconds,
        double activeEndSeconds,
        std::string& errorMessage);
}
