#include "midi/MidiRecordingCountIn.h"

#include <algorithm>
#include <cmath>

namespace mw::midi
{
    namespace
    {
        double sanitizeTempo(double tempoBpm) noexcept
        {
            if (!std::isfinite(tempoBpm) || tempoBpm <= 0.0)
                return 120.0;
            return std::clamp(tempoBpm, 20.0, 400.0);
        }

        int sanitizeNumerator(int numerator) noexcept
        {
            return numerator >= 1 && numerator <= 32 ? numerator : 4;
        }

        int sanitizeDenominator(int denominator) noexcept
        {
            switch (denominator)
            {
                case 1:
                case 2:
                case 4:
                case 8:
                case 16:
                case 32:
                    return denominator;
                default:
                    return 4;
            }
        }
    }

    MidiRecordingCountInPlan makeMidiRecordingCountInPlan(
        int bars,
        double tempoBpm,
        int timeSignatureNumerator,
        int timeSignatureDenominator)
    {
        MidiRecordingCountInPlan plan;
        plan.bars = std::clamp(bars, 0, 2);
        plan.tempoBpm = sanitizeTempo(tempoBpm);
        plan.numerator = sanitizeNumerator(timeSignatureNumerator);
        plan.denominator = sanitizeDenominator(timeSignatureDenominator);
        plan.secondsPerBeat = (60.0 / plan.tempoBpm) * (4.0 / static_cast<double>(plan.denominator));

        if (plan.bars <= 0)
        {
            plan.durationSeconds = 0.0;
            return plan;
        }

        const int totalBeats = plan.bars * plan.numerator;
        plan.beats.reserve(static_cast<std::size_t>(totalBeats));
        for (int beat = 0; beat < totalBeats; ++beat)
        {
            MidiRecordingCountInBeat scheduled;
            scheduled.barIndex = beat / plan.numerator;
            scheduled.beatIndex = beat % plan.numerator;
            scheduled.startSeconds = static_cast<double>(beat) * plan.secondsPerBeat;
            scheduled.accented = scheduled.beatIndex == 0;
            plan.beats.push_back(scheduled);
        }
        plan.durationSeconds = static_cast<double>(totalBeats) * plan.secondsPerBeat;
        return plan;
    }

    std::uint64_t MidiRecordingCountInState::nextGeneration() noexcept
    {
        ++generation_;
        if (generation_ == 0)
            ++generation_;
        return generation_;
    }

    std::uint64_t MidiRecordingCountInState::begin(
        int bars,
        double tempoBpm,
        int timeSignatureNumerator,
        int timeSignatureDenominator,
        double requestedStartSeconds)
    {
        nextGeneration();
        requestedStartSeconds_ = std::isfinite(requestedStartSeconds)
            ? std::max(0.0, requestedStartSeconds)
            : 0.0;
        plan_ = makeMidiRecordingCountInPlan(
            bars,
            tempoBpm,
            timeSignatureNumerator,
            timeSignatureDenominator);
        active_ = plan_.enabled();
        return generation_;
    }

    void MidiRecordingCountInState::cancel() noexcept
    {
        nextGeneration();
        active_ = false;
        plan_ = {};
    }

    bool MidiRecordingCountInState::complete(std::uint64_t generation) noexcept
    {
        if (!active_ || generation == 0 || generation != generation_)
            return false;

        active_ = false;
        return true;
    }
}
