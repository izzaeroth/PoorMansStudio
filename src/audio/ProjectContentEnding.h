#pragma once

#include <cstdint>
#include <vector>

namespace mw::audio
{
    struct ContentEndingTrack
    {
        bool audioClipTrack = false;
        bool muted = false;
        bool solo = false;
        double volume = 1.0;
        std::int64_t contentEndTick = 0;
    };

    struct ContentEndingClip
    {
        int trackIndex = -1;
        std::int64_t startTick = 0;
        double keptDurationSeconds = 0.0;
        double gain = 1.0;
    };

    struct ProjectContentEndingInput
    {
        int tempoBpm = 120;
        double masterVolume = 1.0;
        std::vector<ContentEndingTrack> tracks;
        std::vector<ContentEndingClip> clips;
    };

    double contentEndingTicksToSeconds(std::int64_t ticks, int tempoBpm) noexcept;
    double calculateProjectContentEndingSeconds(const ProjectContentEndingInput& input) noexcept;
}
