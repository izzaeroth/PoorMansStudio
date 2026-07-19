#pragma once

#include "core/StableId.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace mw::playback
{
    enum class TransportState
    {
        Stopped,
        Preparing,
        Playing,
        Stopping,
        Seeking,
        Failed
    };

    const char* transportStateName(TransportState state) noexcept;

    struct PlaybackTransportSnapshot
    {
        TransportState state = TransportState::Stopped;
        std::uint64_t generation = 0;
        mw::core::StableId projectId = 0;
        std::int64_t currentSample = 0;
        std::int64_t totalSamples = 0;
        int sampleRate = 48000;
        int blockSize = 512;
        std::vector<mw::core::StableId> activeTrackIds;
        std::string failureMessage;

        bool isActive() const noexcept;
        double currentSeconds() const noexcept;
        double durationSeconds() const noexcept;
    };

    class PlaybackTransportCoordinator final
    {
    public:
        std::uint64_t beginPreparing(mw::core::StableId projectId,
                                     std::int64_t startSample,
                                     int sampleRate,
                                     int blockSize,
                                     std::vector<mw::core::StableId> trackIds);

        bool beginPlaying(std::uint64_t generation, std::int64_t totalSamples);
        bool beginSeeking(std::uint64_t generation, std::int64_t targetSample);
        bool updatePosition(std::uint64_t generation, std::int64_t currentSample, std::int64_t totalSamples);
        bool beginStopping(std::uint64_t generation);
        bool finishStopped(std::uint64_t generation);
        bool fail(std::uint64_t generation, std::string message);
        void reset();

        PlaybackTransportSnapshot snapshot() const;
        bool matches(std::uint64_t generation, mw::core::StableId projectId) const;

    private:
        mutable std::mutex mutex_;
        PlaybackTransportSnapshot state_;
    };
}
