#pragma once

#include "midi/MidiRecordingPunch.h"

#include <cstdint>

namespace mw::midi
{
    enum class MidiRecordingLoopWrapStage
    {
        Idle,
        Claimed,
        Seeking,
        Restarted
    };

    class MidiRecordingLoopWrapTransaction final
    {
    public:
        void cancel() noexcept;

        bool begin(std::uint64_t generation,
                   int passNumber,
                   const MidiRecordingPunchRegion& region,
                   std::int64_t projectTick) noexcept;
        bool markSeeking(std::uint64_t generation, int passNumber) noexcept;
        bool markRestarted(std::uint64_t generation, int passNumber) noexcept;
        bool finish(std::uint64_t generation, int passNumber) noexcept;

        bool isActive() const noexcept;
        bool matches(std::uint64_t generation, int passNumber) const noexcept;
        MidiRecordingLoopWrapStage stage() const noexcept;
        std::int64_t inTick() const noexcept;
        std::int64_t outTick() const noexcept;

    private:
        std::uint64_t generation_ = 0;
        int passNumber_ = 0;
        std::int64_t inTick_ = -1;
        std::int64_t outTick_ = -1;
        MidiRecordingLoopWrapStage stage_ = MidiRecordingLoopWrapStage::Idle;
    };

    class MidiRecordingLoopRunState final
    {
    public:
        std::uint64_t begin(bool enabled,
                            const MidiRecordingPunchRegion& region,
                            std::int64_t playbackStartTick) noexcept;
        void cancel() noexcept;

        bool isActive() const noexcept;
        std::uint64_t generation() const noexcept;
        bool matches(std::uint64_t generation) const noexcept;
        int passNumber() const noexcept;
        bool wrapClaimed() const noexcept;

        bool activateFirstPass(std::uint64_t generation,
                               std::int64_t projectTick) noexcept;
        bool shouldWrap(std::uint64_t generation,
                        std::int64_t projectTick) const noexcept;
        bool claimWrap(std::uint64_t generation,
                       std::int64_t projectTick) noexcept;
        bool beginNextPass(std::uint64_t generation) noexcept;

    private:
        MidiRecordingPunchRegion region_;
        std::uint64_t generation_ = 0;
        int passNumber_ = 0;
        bool active_ = false;
        bool wrapClaimed_ = false;
    };
}
