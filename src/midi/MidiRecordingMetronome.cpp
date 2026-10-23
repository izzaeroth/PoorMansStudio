#include "midi/MidiRecordingMetronome.h"

#include <algorithm>
#include <cmath>
#include <limits>

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

        std::int64_t safeBeatIndex(double value) noexcept
        {
            if (!std::isfinite(value) || value <= 0.0)
                return 0;
            const auto limit = static_cast<double>(std::numeric_limits<std::int64_t>::max() / 4);
            return static_cast<std::int64_t>(std::min(value, limit));
        }

        MidiRecordingMetronomeBeat makeBeat(
            const MidiRecordingMetronomeConfig& config,
            std::int64_t globalBeatIndex) noexcept
        {
            MidiRecordingMetronomeBeat beat;
            beat.globalBeatIndex = std::max<std::int64_t>(0, globalBeatIndex);
            beat.barIndex = beat.globalBeatIndex / std::max(1, config.numerator);
            beat.beatIndex = static_cast<int>(beat.globalBeatIndex % std::max(1, config.numerator));
            beat.startSeconds = static_cast<double>(beat.globalBeatIndex) * config.secondsPerBeat;
            beat.accented = beat.beatIndex == 0;
            return beat;
        }
    }

    MidiRecordingMetronomeConfig makeMidiRecordingMetronomeConfig(
        bool enabled,
        double tempoBpm,
        int timeSignatureNumerator,
        int timeSignatureDenominator) noexcept
    {
        MidiRecordingMetronomeConfig config;
        config.enabled = enabled;
        config.tempoBpm = sanitizeTempo(tempoBpm);
        config.numerator = sanitizeNumerator(timeSignatureNumerator);
        config.denominator = sanitizeDenominator(timeSignatureDenominator);
        config.secondsPerBeat = (60.0 / config.tempoBpm) * (4.0 / static_cast<double>(config.denominator));
        config.clickLengthSeconds = std::clamp(config.secondsPerBeat * 0.40, 0.004, 0.028);
        return config;
    }

    MidiRecordingMetronomeBeat midiRecordingMetronomeBeatAtOrBefore(
        const MidiRecordingMetronomeConfig& config,
        double projectSeconds) noexcept
    {
        if (!config.enabled || config.secondsPerBeat <= 0.0 || !std::isfinite(projectSeconds))
            return {};

        const auto safeSeconds = std::max(0.0, projectSeconds);
        const auto ratio = safeSeconds / config.secondsPerBeat;
        return makeBeat(config, safeBeatIndex(std::floor(ratio + 1.0e-10)));
    }

    MidiRecordingMetronomeBeat midiRecordingMetronomeNextBeatAtOrAfter(
        const MidiRecordingMetronomeConfig& config,
        double projectSeconds) noexcept
    {
        if (!config.enabled || config.secondsPerBeat <= 0.0 || !std::isfinite(projectSeconds))
            return {};

        const auto safeSeconds = std::max(0.0, projectSeconds);
        const auto ratio = safeSeconds / config.secondsPerBeat;
        const auto rounded = std::round(ratio);
        const auto beatIndex = std::abs(ratio - rounded) <= 1.0e-10
            ? safeBeatIndex(rounded)
            : safeBeatIndex(std::ceil(ratio));
        return makeBeat(config, beatIndex);
    }

    float midiRecordingMetronomeSampleAtProjectTime(
        const MidiRecordingMetronomeConfig& config,
        double projectSeconds) noexcept
    {
        if (!config.enabled || config.secondsPerBeat <= 0.0 || config.clickLengthSeconds <= 0.0
            || !std::isfinite(projectSeconds) || projectSeconds < 0.0)
        {
            return 0.0f;
        }

        const auto beat = midiRecordingMetronomeBeatAtOrBefore(config, projectSeconds);
        const double age = projectSeconds - beat.startSeconds;
        if (age < -1.0e-10 || age >= config.clickLengthSeconds)
            return 0.0f;

        constexpr double twoPi = 6.283185307179586476925286766559;
        const double normalized = std::clamp(age / config.clickLengthSeconds, 0.0, 1.0);
        const double envelope = (1.0 - normalized) * (1.0 - normalized);
        const double frequency = beat.accented ? 1760.0 : 1100.0;
        const double amplitude = beat.accented ? 0.42 : 0.26;
        const double sample = std::sin(twoPi * frequency * std::max(0.0, age)) * envelope * amplitude;
        return static_cast<float>(std::clamp(sample, -0.8, 0.8));
    }

    bool MidiRecordingMetronomeRunState::begin(
        const MidiRecordingMetronomeConfig& config,
        double projectStartSeconds) noexcept
    {
        config_ = config;
        projectStartSeconds_ = std::isfinite(projectStartSeconds)
            ? std::max(0.0, projectStartSeconds)
            : 0.0;
        active_ = config_.enabled;
        return active_;
    }

    void MidiRecordingMetronomeRunState::stop() noexcept
    {
        active_ = false;
        projectStartSeconds_ = 0.0;
        config_ = {};
    }
}
