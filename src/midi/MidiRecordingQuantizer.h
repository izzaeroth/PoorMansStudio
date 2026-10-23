#pragma once

#include "core/NoteEvent.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mw::midi
{
    bool isSupportedMidiRecordingQuantizeDivision(int division) noexcept;
    int sanitizeMidiRecordingQuantizeDivision(int division) noexcept;
    std::int64_t midiRecordingQuantizeGridTicks(int division, int ticksPerQuarterNote) noexcept;
    std::int64_t quantizeMidiRecordingStartTick(
        std::int64_t startTick,
        int division,
        int ticksPerQuarterNote) noexcept;

    // Quantizes only Note On/start positions for one newly-recorded take.
    // Pitch, velocity, MIDI channel, duration, and articulation are preserved.
    // Returns the number of note starts that changed.
    std::size_t quantizeMidiRecordingTake(
        std::vector<mw::core::NoteEvent>& notes,
        int division,
        int ticksPerQuarterNote) noexcept;
}
