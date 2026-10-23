#pragma once

#include "midi/MidiRecordingSession.h"
#include "playback/PlaybackTransportCoordinator.h"

#include <cstdint>

namespace mw::midi
{
    std::int64_t projectTickForTransportSeconds(double transportSeconds,
                                                int tempoBpm,
                                                int ticksPerQuarterNote) noexcept;

    MidiRecordingConfig makeTransportSynchronizedRecordingConfig(
        const mw::playback::PlaybackTransportSnapshot& transport,
        int tempoBpm,
        int ticksPerQuarterNote) noexcept;
}
