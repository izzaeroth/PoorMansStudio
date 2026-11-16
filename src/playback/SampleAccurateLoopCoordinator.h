#pragma once

#include <cstdint>

namespace mw::playback
{
    struct SampleAccurateLoopConfig
    {
        bool enabled = false;
        std::int64_t loopStartSample = 0;
        std::int64_t loopEndSample = 0;
    };

    struct SampleAccurateLoopSlice
    {
        std::int64_t timelineStartSample = 0;
        int frameCount = 0;
        bool wrappedBefore = false;
        std::uint64_t wrapCount = 0;
    };

    // A loop can be only one frame long. Without an explicit work limit, a
    // normal device block would then invoke every instrument/effect thousands
    // of times in one realtime callback. Abort rather than block the device.
    class SampleAccurateLoopWorkBudget final
    {
    public:
        static constexpr int maximumSlicesPerCallback = 256;

        bool charge(int frameCount) noexcept
        {
            if (frameCount <= 0 || slices_ >= maximumSlicesPerCallback)
                return false;
            ++slices_;
            return true;
        }

        int used() const noexcept { return slices_; }

    private:
        int slices_ = 0;
    };

    class SampleAccurateLoopCoordinator final
    {
    public:
        void prepare(SampleAccurateLoopConfig config, std::int64_t initialTimelineSample) noexcept;
        void reset(std::int64_t timelineSample = 0) noexcept;

        bool isEnabled() const noexcept;
        std::int64_t currentTimelineSample() const noexcept;
        std::int64_t loopStartSample() const noexcept;
        std::int64_t loopEndSample() const noexcept;
        std::uint64_t wrapCount() const noexcept;

        SampleAccurateLoopSlice nextSlice(int maximumFrames) noexcept;

    private:
        SampleAccurateLoopConfig config_;
        std::int64_t currentTimelineSample_ = 0;
        std::uint64_t wrapCount_ = 0;
    };
}
