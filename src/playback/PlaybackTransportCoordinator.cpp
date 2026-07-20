#include "playback/PlaybackTransportCoordinator.h"

#include <algorithm>
#include <utility>

namespace mw::playback
{
    bool PlaybackTransportSnapshot::isActive() const noexcept
    {
        return state == TransportState::Preparing
            || state == TransportState::Playing
            || state == TransportState::Stopping
            || state == TransportState::Seeking;
    }

    double PlaybackTransportSnapshot::currentSeconds() const noexcept
    {
        return sampleRate > 0 ? static_cast<double>(currentSample) / static_cast<double>(sampleRate) : 0.0;
    }

    double PlaybackTransportSnapshot::durationSeconds() const noexcept
    {
        return sampleRate > 0 ? static_cast<double>(totalSamples) / static_cast<double>(sampleRate) : 0.0;
    }

    std::uint64_t PlaybackTransportCoordinator::beginPreparing(mw::core::StableId projectId,
                                                                std::int64_t startSample,
                                                                int sampleRate,
                                                                int blockSize,
                                                                std::vector<mw::core::StableId> trackIds)
    {
        std::scoped_lock lock(mutex_);
        ++state_.generation;
        if (state_.generation == 0)
            ++state_.generation;

        state_.state = TransportState::Preparing;
        state_.projectId = projectId;
        state_.currentSample = std::max<std::int64_t>(0, startSample);
        state_.totalSamples = state_.currentSample;
        state_.sampleRate = std::max(1, sampleRate);
        state_.blockSize = std::max(1, blockSize);
        state_.activeTrackIds = std::move(trackIds);
        state_.failureMessage.clear();
        return state_.generation;
    }

    bool PlaybackTransportCoordinator::beginPlaying(std::uint64_t generation, std::int64_t totalSamples)
    {
        std::scoped_lock lock(mutex_);
        if (generation != state_.generation || state_.state != TransportState::Preparing)
            return false;

        state_.state = TransportState::Playing;
        state_.totalSamples = std::max(state_.currentSample, totalSamples);
        return true;
    }

    bool PlaybackTransportCoordinator::beginPlayingWithAudioFormat(std::uint64_t generation,
                                                                    std::int64_t currentSample,
                                                                    std::int64_t totalSamples,
                                                                    int sampleRate,
                                                                    int blockSize)
    {
        std::scoped_lock lock(mutex_);
        if (generation != state_.generation || state_.state != TransportState::Preparing)
            return false;

        state_.state = TransportState::Playing;
        state_.sampleRate = std::max(1, sampleRate);
        state_.blockSize = std::max(1, blockSize);
        state_.currentSample = std::max<std::int64_t>(0, currentSample);
        state_.totalSamples = std::max(state_.currentSample, totalSamples);
        return true;
    }

    bool PlaybackTransportCoordinator::beginSeeking(std::uint64_t generation, std::int64_t targetSample)
    {
        std::scoped_lock lock(mutex_);
        if (generation != state_.generation
            || (state_.state != TransportState::Playing && state_.state != TransportState::Preparing))
        {
            return false;
        }

        state_.state = TransportState::Seeking;
        const auto maximumTarget = std::max<std::int64_t>(0, state_.totalSamples);
        state_.currentSample = std::clamp<std::int64_t>(targetSample, 0, maximumTarget);
        return true;
    }

    bool PlaybackTransportCoordinator::updatePosition(std::uint64_t generation,
                                                       std::int64_t currentSample,
                                                       std::int64_t totalSamples)
    {
        std::scoped_lock lock(mutex_);
        if (generation != state_.generation || !state_.isActive())
            return false;

        state_.currentSample = std::max<std::int64_t>(0, currentSample);
        state_.totalSamples = std::max({ state_.currentSample, state_.totalSamples, totalSamples });
        if (state_.state == TransportState::Seeking)
            state_.state = TransportState::Playing;
        return true;
    }

    bool PlaybackTransportCoordinator::updatePositionWithAudioFormat(std::uint64_t generation,
                                                                      std::int64_t currentSample,
                                                                      std::int64_t totalSamples,
                                                                      int sampleRate,
                                                                      int blockSize)
    {
        std::scoped_lock lock(mutex_);
        if (generation != state_.generation || !state_.isActive())
            return false;

        state_.sampleRate = std::max(1, sampleRate);
        state_.blockSize = std::max(1, blockSize);
        state_.currentSample = std::max<std::int64_t>(0, currentSample);
        state_.totalSamples = std::max({ state_.currentSample, totalSamples });
        if (state_.state == TransportState::Seeking)
            state_.state = TransportState::Playing;
        return true;
    }

    bool PlaybackTransportCoordinator::beginStopping(std::uint64_t generation)
    {
        std::scoped_lock lock(mutex_);
        if (generation != state_.generation)
            return false;

        if (state_.state == TransportState::Stopped)
            return true;

        state_.state = TransportState::Stopping;
        return true;
    }

    bool PlaybackTransportCoordinator::finishStopped(std::uint64_t generation)
    {
        std::scoped_lock lock(mutex_);
        if (generation != state_.generation)
            return false;

        state_.state = TransportState::Stopped;
        state_.currentSample = 0;
        state_.totalSamples = 0;
        state_.activeTrackIds.clear();
        state_.failureMessage.clear();
        return true;
    }

    bool PlaybackTransportCoordinator::fail(std::uint64_t generation, std::string message)
    {
        std::scoped_lock lock(mutex_);
        if (generation != state_.generation)
            return false;

        state_.state = TransportState::Failed;
        state_.failureMessage = std::move(message);
        return true;
    }

    void PlaybackTransportCoordinator::reset()
    {
        std::scoped_lock lock(mutex_);
        ++state_.generation;
        if (state_.generation == 0)
            ++state_.generation;
        state_.state = TransportState::Stopped;
        state_.projectId = 0;
        state_.currentSample = 0;
        state_.totalSamples = 0;
        state_.activeTrackIds.clear();
        state_.failureMessage.clear();
    }

    PlaybackTransportSnapshot PlaybackTransportCoordinator::snapshot() const
    {
        std::scoped_lock lock(mutex_);
        return state_;
    }

    bool PlaybackTransportCoordinator::matches(std::uint64_t generation, mw::core::StableId projectId) const
    {
        std::scoped_lock lock(mutex_);
        return state_.generation == generation && state_.projectId == projectId;
    }
}
