#include "midi/MidiRecordingLoop.h"

namespace mw::midi
{
    void MidiRecordingLoopWrapTransaction::cancel() noexcept
    {
        generation_ = 0;
        passNumber_ = 0;
        inTick_ = -1;
        outTick_ = -1;
        stage_ = MidiRecordingLoopWrapStage::Idle;
    }

    bool MidiRecordingLoopWrapTransaction::begin(std::uint64_t generation,
                                                   int passNumber,
                                                   const MidiRecordingPunchRegion& region,
                                                   std::int64_t projectTick) noexcept
    {
        if (stage_ != MidiRecordingLoopWrapStage::Idle
            || generation == 0
            || passNumber <= 0
            || !region.enabled
            || !isValidMidiRecordingPunchRegion(region)
            || projectTick < region.outTick)
        {
            return false;
        }

        generation_ = generation;
        passNumber_ = passNumber;
        inTick_ = region.inTick;
        outTick_ = region.outTick;
        stage_ = MidiRecordingLoopWrapStage::Claimed;
        return true;
    }

    bool MidiRecordingLoopWrapTransaction::markSeeking(std::uint64_t generation, int passNumber) noexcept
    {
        if (!matches(generation, passNumber) || stage_ != MidiRecordingLoopWrapStage::Claimed)
            return false;
        stage_ = MidiRecordingLoopWrapStage::Seeking;
        return true;
    }

    bool MidiRecordingLoopWrapTransaction::markRestarted(std::uint64_t generation, int passNumber) noexcept
    {
        if (!matches(generation, passNumber) || stage_ != MidiRecordingLoopWrapStage::Seeking)
            return false;
        stage_ = MidiRecordingLoopWrapStage::Restarted;
        return true;
    }

    bool MidiRecordingLoopWrapTransaction::finish(std::uint64_t generation, int passNumber) noexcept
    {
        if (!matches(generation, passNumber) || stage_ != MidiRecordingLoopWrapStage::Restarted)
            return false;
        cancel();
        return true;
    }

    bool MidiRecordingLoopWrapTransaction::isActive() const noexcept
    {
        return stage_ != MidiRecordingLoopWrapStage::Idle;
    }

    bool MidiRecordingLoopWrapTransaction::matches(std::uint64_t generation, int passNumber) const noexcept
    {
        return isActive() && generation != 0 && generation == generation_ && passNumber == passNumber_;
    }

    MidiRecordingLoopWrapStage MidiRecordingLoopWrapTransaction::stage() const noexcept
    {
        return stage_;
    }

    std::int64_t MidiRecordingLoopWrapTransaction::inTick() const noexcept
    {
        return inTick_;
    }

    std::int64_t MidiRecordingLoopWrapTransaction::outTick() const noexcept
    {
        return outTick_;
    }

    std::uint64_t MidiRecordingLoopRunState::begin(bool enabled,
                                                    const MidiRecordingPunchRegion& region,
                                                    std::int64_t playbackStartTick) noexcept
    {
        ++generation_;
        if (generation_ == 0)
            ++generation_;

        region_ = region;
        active_ = enabled && region_.enabled && isValidMidiRecordingPunchRegion(region_)
            && midiRecordingPunchPositionForTick(region_, playbackStartTick)
                != MidiRecordingPunchPosition::AtOrAfterOut;
        wrapClaimed_ = false;
        passNumber_ = 0;

        if (active_
            && midiRecordingPunchPositionForTick(region_, playbackStartTick)
                == MidiRecordingPunchPosition::Inside)
        {
            passNumber_ = 1;
        }

        return generation_;
    }

    void MidiRecordingLoopRunState::cancel() noexcept
    {
        ++generation_;
        if (generation_ == 0)
            ++generation_;
        region_ = {};
        passNumber_ = 0;
        active_ = false;
        wrapClaimed_ = false;
    }

    bool MidiRecordingLoopRunState::isActive() const noexcept
    {
        return active_;
    }

    std::uint64_t MidiRecordingLoopRunState::generation() const noexcept
    {
        return generation_;
    }

    bool MidiRecordingLoopRunState::matches(std::uint64_t generation) const noexcept
    {
        return active_ && generation != 0 && generation == generation_;
    }

    int MidiRecordingLoopRunState::passNumber() const noexcept
    {
        return passNumber_;
    }

    bool MidiRecordingLoopRunState::wrapClaimed() const noexcept
    {
        return wrapClaimed_;
    }

    bool MidiRecordingLoopRunState::activateFirstPass(std::uint64_t generation,
                                                       std::int64_t projectTick) noexcept
    {
        if (!matches(generation) || passNumber_ != 0 || wrapClaimed_)
            return false;
        if (midiRecordingPunchPositionForTick(region_, projectTick) != MidiRecordingPunchPosition::Inside)
            return false;
        passNumber_ = 1;
        return true;
    }

    bool MidiRecordingLoopRunState::shouldWrap(std::uint64_t generation,
                                                std::int64_t projectTick) const noexcept
    {
        return matches(generation)
            && passNumber_ > 0
            && !wrapClaimed_
            && isValidMidiRecordingPunchRegion(region_)
            && projectTick >= region_.outTick;
    }

    bool MidiRecordingLoopRunState::claimWrap(std::uint64_t generation,
                                               std::int64_t projectTick) noexcept
    {
        if (!shouldWrap(generation, projectTick))
            return false;
        wrapClaimed_ = true;
        return true;
    }

    bool MidiRecordingLoopRunState::beginNextPass(std::uint64_t generation) noexcept
    {
        if (!matches(generation) || !wrapClaimed_ || passNumber_ <= 0)
            return false;
        ++passNumber_;
        wrapClaimed_ = false;
        return true;
    }
}
