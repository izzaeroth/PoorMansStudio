#pragma once

#include "core/NoteEvent.h"

#include <cstdint>
#include <vector>

namespace mw::midi
{
    struct MidiRecordingPunchRegion
    {
        bool enabled = false;
        std::int64_t inTick = -1;
        std::int64_t outTick = -1;
    };

    enum class MidiRecordingPunchPosition
    {
        BeforeIn,
        Inside,
        AtOrAfterOut
    };

    enum class MidiRecordingPunchPhase
    {
        Inactive,
        PreRoll,
        Capture
    };

    bool isValidMidiRecordingPunchRegion(const MidiRecordingPunchRegion& region) noexcept;
    MidiRecordingPunchPosition midiRecordingPunchPositionForTick(
        const MidiRecordingPunchRegion& region,
        std::int64_t projectTick) noexcept;
    double midiRecordingProjectSecondsForTick(std::int64_t projectTick,
                                              int tempoBpm,
                                              int ticksPerQuarterNote) noexcept;
    std::size_t constrainMidiRecordingTakeToPunchRegion(
        std::vector<mw::core::NoteEvent>& notes,
        const MidiRecordingPunchRegion& region) noexcept;
    int midiRecordingPunchBoundaryDelayMilliseconds(double remainingSeconds,
                                                     bool punchOutBoundary) noexcept;

    class MidiRecordingPunchRunState final
    {
    public:
        std::uint64_t begin(const MidiRecordingPunchRegion& region,
                            std::int64_t playbackStartTick) noexcept;
        void cancel() noexcept;

        bool isActive() const noexcept;
        bool isPreRoll() const noexcept;
        bool isCapturePhase() const noexcept;
        MidiRecordingPunchPhase phase() const noexcept;
        const MidiRecordingPunchRegion& region() const noexcept;
        std::uint64_t generation() const noexcept;
        bool matches(std::uint64_t generation) const noexcept;

        bool activateCapture(std::uint64_t generation,
                             std::int64_t projectTick) noexcept;
        bool shouldPunchOut(std::uint64_t generation,
                            std::int64_t projectTick) const noexcept;
        bool claimPunchOut(std::uint64_t generation,
                           std::int64_t projectTick) noexcept;
        bool punchOutClaimed() const noexcept;
        bool finish(std::uint64_t generation) noexcept;

    private:
        MidiRecordingPunchRegion region_;
        MidiRecordingPunchPhase phase_ = MidiRecordingPunchPhase::Inactive;
        std::uint64_t generation_ = 0;
        bool punchOutClaimed_ = false;
    };
}
