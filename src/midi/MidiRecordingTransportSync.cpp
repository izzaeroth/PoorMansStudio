#include "midi/MidiRecordingTransportSync.h"

#include <algorithm>
#include <cmath>

namespace mw::midi
{
    std::int64_t projectTickForTransportSeconds(double transportSeconds,
                                                int tempoBpm,
                                                int ticksPerQuarterNote) noexcept
    {
        if (!std::isfinite(transportSeconds))
            transportSeconds = 0.0;

        const auto safeSeconds = std::max(0.0, transportSeconds);
        const auto safeTempo = std::max(1, tempoBpm);
        const auto safeTicksPerQuarter = std::max(1, ticksPerQuarterNote);
        const auto ticksPerSecond = static_cast<double>(safeTempo)
            * static_cast<double>(safeTicksPerQuarter) / 60.0;
        return std::max<std::int64_t>(0,
            static_cast<std::int64_t>(std::llround(safeSeconds * ticksPerSecond)));
    }

    MidiRecordingConfig makeTransportSynchronizedRecordingConfig(
        const mw::playback::PlaybackTransportSnapshot& transport,
        int tempoBpm,
        int ticksPerQuarterNote) noexcept
    {
        MidiRecordingConfig config;
        config.tempoBpm = std::max(1, tempoBpm);
        config.ticksPerQuarterNote = std::max(1, ticksPerQuarterNote);
        config.startTimeSeconds = std::max(0.0, transport.currentSeconds());
        config.startTick = projectTickForTransportSeconds(
            config.startTimeSeconds,
            config.tempoBpm,
            config.ticksPerQuarterNote);
        return config;
    }
}
