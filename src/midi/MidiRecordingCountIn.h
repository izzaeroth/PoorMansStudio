#pragma once

#include <cstdint>
#include <vector>

namespace mw::midi
{
    struct MidiRecordingCountInBeat
    {
        int barIndex = 0;
        int beatIndex = 0;
        double startSeconds = 0.0;
        bool accented = false;
    };

    struct MidiRecordingCountInPlan
    {
        int bars = 0;
        int numerator = 4;
        int denominator = 4;
        double tempoBpm = 120.0;
        double secondsPerBeat = 0.5;
        double durationSeconds = 0.0;
        std::vector<MidiRecordingCountInBeat> beats;

        bool enabled() const noexcept { return bars > 0 && !beats.empty() && durationSeconds > 0.0; }
    };

    MidiRecordingCountInPlan makeMidiRecordingCountInPlan(
        int bars,
        double tempoBpm,
        int timeSignatureNumerator,
        int timeSignatureDenominator);

    class MidiRecordingCountInState final
    {
    public:
        std::uint64_t begin(
            int bars,
            double tempoBpm,
            int timeSignatureNumerator,
            int timeSignatureDenominator,
            double requestedStartSeconds);

        void cancel() noexcept;
        bool complete(std::uint64_t generation) noexcept;

        bool isActive() const noexcept { return active_; }
        std::uint64_t generation() const noexcept { return generation_; }
        double requestedStartSeconds() const noexcept { return requestedStartSeconds_; }
        const MidiRecordingCountInPlan& plan() const noexcept { return plan_; }

    private:
        std::uint64_t nextGeneration() noexcept;

        std::uint64_t generation_ = 0;
        bool active_ = false;
        double requestedStartSeconds_ = 0.0;
        MidiRecordingCountInPlan plan_;
    };
}
