#pragma once

#include <cstdint>

namespace mw::midi
{
    struct MidiRecordingMetronomeConfig
    {
        bool enabled = false;
        int numerator = 4;
        int denominator = 4;
        double tempoBpm = 120.0;
        double secondsPerBeat = 0.5;
        double clickLengthSeconds = 0.028;
    };

    struct MidiRecordingMetronomeBeat
    {
        std::int64_t globalBeatIndex = 0;
        std::int64_t barIndex = 0;
        int beatIndex = 0;
        double startSeconds = 0.0;
        bool accented = false;
    };

    MidiRecordingMetronomeConfig makeMidiRecordingMetronomeConfig(
        bool enabled,
        double tempoBpm,
        int timeSignatureNumerator,
        int timeSignatureDenominator) noexcept;

    MidiRecordingMetronomeBeat midiRecordingMetronomeBeatAtOrBefore(
        const MidiRecordingMetronomeConfig& config,
        double projectSeconds) noexcept;

    MidiRecordingMetronomeBeat midiRecordingMetronomeNextBeatAtOrAfter(
        const MidiRecordingMetronomeConfig& config,
        double projectSeconds) noexcept;

    // Returns a mono click sample for an absolute project time. The phase is always
    // project-grid based; beginning a take at a nonzero position does not restart a bar.
    float midiRecordingMetronomeSampleAtProjectTime(
        const MidiRecordingMetronomeConfig& config,
        double projectSeconds) noexcept;

    class MidiRecordingMetronomeRunState final
    {
    public:
        bool begin(const MidiRecordingMetronomeConfig& config, double projectStartSeconds) noexcept;
        void stop() noexcept;

        bool isActive() const noexcept { return active_; }
        double projectStartSeconds() const noexcept { return projectStartSeconds_; }
        const MidiRecordingMetronomeConfig& config() const noexcept { return config_; }

    private:
        bool active_ = false;
        double projectStartSeconds_ = 0.0;
        MidiRecordingMetronomeConfig config_;
    };
}
