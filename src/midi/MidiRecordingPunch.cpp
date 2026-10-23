#include "midi/MidiRecordingPunch.h"

#include <algorithm>
#include <cmath>

namespace mw::midi
{
    bool isValidMidiRecordingPunchRegion(const MidiRecordingPunchRegion& region) noexcept
    {
        if (!region.enabled)
            return true;
        return region.inTick >= 0 && region.outTick > region.inTick;
    }

    MidiRecordingPunchPosition midiRecordingPunchPositionForTick(
        const MidiRecordingPunchRegion& region,
        std::int64_t projectTick) noexcept
    {
        if (!region.enabled || !isValidMidiRecordingPunchRegion(region))
            return MidiRecordingPunchPosition::Inside;

        const auto tick = std::max<std::int64_t>(0, projectTick);
        if (tick < region.inTick)
            return MidiRecordingPunchPosition::BeforeIn;
        if (tick >= region.outTick)
            return MidiRecordingPunchPosition::AtOrAfterOut;
        return MidiRecordingPunchPosition::Inside;
    }

    double midiRecordingProjectSecondsForTick(std::int64_t projectTick,
                                              int tempoBpm,
                                              int ticksPerQuarterNote) noexcept
    {
        const auto safeTick = std::max<std::int64_t>(0, projectTick);
        const auto safeTempo = std::max(1, tempoBpm);
        const auto safeTicksPerQuarter = std::max(1, ticksPerQuarterNote);
        const auto ticksPerSecond = static_cast<double>(safeTempo)
            * static_cast<double>(safeTicksPerQuarter) / 60.0;
        return static_cast<double>(safeTick) / ticksPerSecond;
    }

    std::size_t constrainMidiRecordingTakeToPunchRegion(
        std::vector<mw::core::NoteEvent>& notes,
        const MidiRecordingPunchRegion& region) noexcept
    {
        if (!region.enabled || !isValidMidiRecordingPunchRegion(region))
            return 0;

        std::size_t changed = 0;
        const auto latestStart = std::max<std::int64_t>(region.inTick, region.outTick - 1);
        for (auto& note : notes)
        {
            const auto originalStart = note.startTick;
            const auto originalDuration = note.durationTicks;
            note.startTick = std::clamp(note.startTick, region.inTick, latestStart);
            const auto unclampedEnd = note.startTick + std::max<std::int64_t>(1, note.durationTicks);
            const auto endTick = std::min(region.outTick, unclampedEnd);
            note.durationTicks = std::max<std::int64_t>(1, endTick - note.startTick);
            if (note.startTick != originalStart || note.durationTicks != originalDuration)
                ++changed;
        }
        return changed;
    }

    int midiRecordingPunchBoundaryDelayMilliseconds(double remainingSeconds,
                                                     bool punchOutBoundary) noexcept
    {
        if (!std::isfinite(remainingSeconds) || remainingSeconds <= 0.0)
            return 1;

        const auto remainingMilliseconds = std::max(1, static_cast<int>(std::ceil(remainingSeconds * 1000.0)));
        if (!punchOutBoundary)
            return remainingMilliseconds;

        // Enter a short final-window poll before Punch Out. This keeps normal long waits cheap
        // while avoiding reliance on a single UI/message-loop callback at the exact boundary.
        constexpr int finalWindowMilliseconds = 20;
        constexpr int finalPollMilliseconds = 4;
        if (remainingMilliseconds > finalWindowMilliseconds)
            return std::max(1, remainingMilliseconds - finalWindowMilliseconds);
        return std::min(finalPollMilliseconds, remainingMilliseconds);
    }

    std::uint64_t MidiRecordingPunchRunState::begin(const MidiRecordingPunchRegion& region,
                                                     std::int64_t playbackStartTick) noexcept
    {
        ++generation_;
        if (generation_ == 0)
            ++generation_;

        region_ = region;
        phase_ = MidiRecordingPunchPhase::Inactive;
        punchOutClaimed_ = false;
        if (!region_.enabled || !isValidMidiRecordingPunchRegion(region_))
            return generation_;

        switch (midiRecordingPunchPositionForTick(region_, playbackStartTick))
        {
            case MidiRecordingPunchPosition::BeforeIn:
                phase_ = MidiRecordingPunchPhase::PreRoll;
                break;
            case MidiRecordingPunchPosition::Inside:
                phase_ = MidiRecordingPunchPhase::Capture;
                break;
            case MidiRecordingPunchPosition::AtOrAfterOut:
            default:
                phase_ = MidiRecordingPunchPhase::Inactive;
                break;
        }
        return generation_;
    }

    void MidiRecordingPunchRunState::cancel() noexcept
    {
        ++generation_;
        if (generation_ == 0)
            ++generation_;
        phase_ = MidiRecordingPunchPhase::Inactive;
        region_ = {};
        punchOutClaimed_ = false;
    }

    bool MidiRecordingPunchRunState::isActive() const noexcept
    {
        return phase_ != MidiRecordingPunchPhase::Inactive;
    }

    bool MidiRecordingPunchRunState::isPreRoll() const noexcept
    {
        return phase_ == MidiRecordingPunchPhase::PreRoll;
    }

    bool MidiRecordingPunchRunState::isCapturePhase() const noexcept
    {
        return phase_ == MidiRecordingPunchPhase::Capture;
    }

    MidiRecordingPunchPhase MidiRecordingPunchRunState::phase() const noexcept
    {
        return phase_;
    }

    const MidiRecordingPunchRegion& MidiRecordingPunchRunState::region() const noexcept
    {
        return region_;
    }

    std::uint64_t MidiRecordingPunchRunState::generation() const noexcept
    {
        return generation_;
    }

    bool MidiRecordingPunchRunState::matches(std::uint64_t generation) const noexcept
    {
        return generation != 0 && generation == generation_;
    }

    bool MidiRecordingPunchRunState::activateCapture(std::uint64_t generation,
                                                      std::int64_t projectTick) noexcept
    {
        if (!matches(generation) || phase_ != MidiRecordingPunchPhase::PreRoll)
            return false;
        if (midiRecordingPunchPositionForTick(region_, projectTick) != MidiRecordingPunchPosition::Inside)
            return false;
        phase_ = MidiRecordingPunchPhase::Capture;
        return true;
    }

    bool MidiRecordingPunchRunState::shouldPunchOut(std::uint64_t generation,
                                                     std::int64_t projectTick) const noexcept
    {
        return matches(generation)
            && isActive()
            && !punchOutClaimed_
            && isValidMidiRecordingPunchRegion(region_)
            && projectTick >= region_.outTick;
    }

    bool MidiRecordingPunchRunState::claimPunchOut(std::uint64_t generation,
                                                    std::int64_t projectTick) noexcept
    {
        if (!shouldPunchOut(generation, projectTick))
            return false;
        punchOutClaimed_ = true;
        return true;
    }

    bool MidiRecordingPunchRunState::punchOutClaimed() const noexcept
    {
        return punchOutClaimed_;
    }

    bool MidiRecordingPunchRunState::finish(std::uint64_t generation) noexcept
    {
        if (!matches(generation) || !isActive())
            return false;
        phase_ = MidiRecordingPunchPhase::Inactive;
        return true;
    }
}
