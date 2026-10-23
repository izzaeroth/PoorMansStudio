#include "midi/MidiRecordingQuantizer.h"

#include <algorithm>
#include <limits>

namespace mw::midi
{
    bool isSupportedMidiRecordingQuantizeDivision(int division) noexcept
    {
        switch (division)
        {
            case 0:
            case 4:
            case 8:
            case 16:
            case 32:
            case 64:
                return true;
            default:
                return false;
        }
    }

    int sanitizeMidiRecordingQuantizeDivision(int division) noexcept
    {
        return isSupportedMidiRecordingQuantizeDivision(division) ? division : 0;
    }

    std::int64_t midiRecordingQuantizeGridTicks(int division, int ticksPerQuarterNote) noexcept
    {
        const int safeDivision = sanitizeMidiRecordingQuantizeDivision(division);
        if (safeDivision == 0)
            return 0;

        const auto safeTicksPerQuarter = static_cast<std::int64_t>(std::max(1, ticksPerQuarterNote));
        const auto wholeNoteTicks = safeTicksPerQuarter * 4;
        const auto roundedGrid = (wholeNoteTicks + (safeDivision / 2)) / safeDivision;
        return std::max<std::int64_t>(1, roundedGrid);
    }

    std::int64_t quantizeMidiRecordingStartTick(
        std::int64_t startTick,
        int division,
        int ticksPerQuarterNote) noexcept
    {
        const auto gridTicks = midiRecordingQuantizeGridTicks(division, ticksPerQuarterNote);
        if (gridTicks <= 0)
            return startTick;

        const auto safeStartTick = std::max<std::int64_t>(0, startTick);
        auto quotient = safeStartTick / gridTicks;
        const auto remainder = safeStartTick % gridTicks;

        // Exact half-grid ties move forward to the later grid line.
        if (remainder * 2 >= gridTicks)
        {
            const auto maxQuotient = std::numeric_limits<std::int64_t>::max() / gridTicks;
            if (quotient < maxQuotient)
                ++quotient;
            else
                return maxQuotient * gridTicks;
        }

        return quotient * gridTicks;
    }

    std::size_t quantizeMidiRecordingTake(
        std::vector<mw::core::NoteEvent>& notes,
        int division,
        int ticksPerQuarterNote) noexcept
    {
        if (sanitizeMidiRecordingQuantizeDivision(division) == 0)
            return 0;

        std::size_t changed = 0;
        for (auto& note : notes)
        {
            const auto quantizedStart = quantizeMidiRecordingStartTick(
                note.startTick,
                division,
                ticksPerQuarterNote);
            if (quantizedStart != note.startTick)
            {
                note.startTick = quantizedStart;
                ++changed;
            }
        }
        return changed;
    }
}
