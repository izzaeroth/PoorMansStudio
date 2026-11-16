#include "playback/SampleAccurateLoopCoordinator.h"

#include <algorithm>
#include <limits>

namespace mw::playback
{
    void SampleAccurateLoopCoordinator::prepare(SampleAccurateLoopConfig config,
                                                 std::int64_t initialTimelineSample) noexcept
    {
        config.loopStartSample = std::max<std::int64_t>(0, config.loopStartSample);
        config.loopEndSample = std::max<std::int64_t>(0, config.loopEndSample);
        config.enabled = config.enabled && config.loopEndSample > config.loopStartSample;
        config_ = config;
        currentTimelineSample_ = std::max<std::int64_t>(0, initialTimelineSample);
        wrapCount_ = 0;

        // A loop is meaningful only when playback begins before Loop Out. If a
        // caller explicitly seeks to/after Loop Out, preserve the requested
        // position rather than silently jumping backward.
        if (config_.enabled && currentTimelineSample_ >= config_.loopEndSample)
            config_.enabled = false;
    }

    void SampleAccurateLoopCoordinator::reset(std::int64_t timelineSample) noexcept
    {
        currentTimelineSample_ = std::max<std::int64_t>(0, timelineSample);
        wrapCount_ = 0;
    }

    bool SampleAccurateLoopCoordinator::isEnabled() const noexcept
    {
        return config_.enabled;
    }

    std::int64_t SampleAccurateLoopCoordinator::currentTimelineSample() const noexcept
    {
        return currentTimelineSample_;
    }

    std::int64_t SampleAccurateLoopCoordinator::loopStartSample() const noexcept
    {
        return config_.loopStartSample;
    }

    std::int64_t SampleAccurateLoopCoordinator::loopEndSample() const noexcept
    {
        return config_.loopEndSample;
    }

    std::uint64_t SampleAccurateLoopCoordinator::wrapCount() const noexcept
    {
        return wrapCount_;
    }

    SampleAccurateLoopSlice SampleAccurateLoopCoordinator::nextSlice(int maximumFrames) noexcept
    {
        SampleAccurateLoopSlice slice;
        slice.timelineStartSample = currentTimelineSample_;
        slice.wrapCount = wrapCount_;
        if (maximumFrames <= 0)
            return slice;

        if (config_.enabled && currentTimelineSample_ >= config_.loopEndSample)
        {
            currentTimelineSample_ = config_.loopStartSample;
            ++wrapCount_;
            slice.timelineStartSample = currentTimelineSample_;
            slice.wrappedBefore = true;
            slice.wrapCount = wrapCount_;
        }

        auto frames = maximumFrames;
        if (config_.enabled && currentTimelineSample_ < config_.loopEndSample)
        {
            const auto untilBoundary = config_.loopEndSample - currentTimelineSample_;
            frames = static_cast<int>(std::min<std::int64_t>(frames, untilBoundary));
        }

        // Non-loop playback can reach INT64_MAX after an explicit long seek.
        // Saturate available frames instead of allowing signed overflow; an
        // exhausted timeline yields an empty slice and the native engine's
        // existing fail-closed callback error path.
        const auto room = std::numeric_limits<std::int64_t>::max() - currentTimelineSample_;
        slice.frameCount = static_cast<int>(std::max<std::int64_t>(0,
            std::min<std::int64_t>(frames, room)));
        currentTimelineSample_ += slice.frameCount;
        return slice;
    }
}
