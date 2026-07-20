#include "clap/ClapLiveCallbackBridge.h"

#include <algorithm>
#include <sstream>
#include <utility>

namespace mw::clap
{
    namespace
    {
        constexpr int kDefaultSampleRate = 48000;
        constexpr int kDefaultBlockSize = 512;
        constexpr int kDefaultOutputChannels = 2;
    }

    void ClapLiveCallbackBridge::prepare(std::vector<ClapLivePlaybackNote> notes, ClapLiveCallbackBridgeConfig config)
    {
        config.scheduler.sampleRate = config.scheduler.sampleRate > 0 ? config.scheduler.sampleRate : kDefaultSampleRate;
        config.scheduler.blockSize = config.scheduler.blockSize > 0 ? config.scheduler.blockSize : kDefaultBlockSize;
        config.outputChannelCount = std::clamp(config.outputChannelCount > 0 ? config.outputChannelCount : kDefaultOutputChannels, 1, 32);
        config_ = std::move(config);

        scheduler_.prepare(std::move(notes), config_.scheduler);
        reusableRequest_.frameCount = 0;
        reusableRequest_.noteEvents.clear();
        reusableRequest_.noteEvents.reserve(static_cast<std::size_t>(std::max(0, scheduler_.maxEventsPerBlock())));

        state_ = {};
        state_.prepared = scheduler_.totalScheduledEvents() > 0;
        state_.directOutputEnabled = false;
        state_.trackIndex = config_.trackIndex;
        state_.trackName = config_.trackName;
        state_.sampleRate = config_.scheduler.sampleRate;
        state_.blockSize = config_.scheduler.blockSize;
        state_.outputChannelCount = config_.outputChannelCount;
        state_.totalScheduledEvents = scheduler_.totalScheduledEvents();
        state_.maxEventsPerBlock = scheduler_.maxEventsPerBlock();
        state_.currentSample = scheduler_.currentSample();
        state_.totalSamples = scheduler_.totalSamples();

        std::ostringstream message;
        if (state_.prepared)
        {
            message << "CLAP preview prepared for track " << (state_.trackIndex + 1)
                    << " with " << state_.totalScheduledEvents << " scheduled note events"
                    << " across " << state_.totalSamples << " samples.";
        }
        else
        {
            message << "CLAP preview has no schedulable note events.";
        }
        state_.message = message.str();
    }

    bool ClapLiveCallbackBridge::isPrepared() const
    {
        return state_.prepared;
    }

    bool ClapLiveCallbackBridge::isFinished() const
    {
        return scheduler_.isFinished();
    }

    const ClapLiveProcessRequest& ClapLiveCallbackBridge::nextRequest()
    {
        scheduler_.nextBlockInto(reusableRequest_);
        if (reusableRequest_.frameCount > 0)
        {
            ++state_.emittedBlocks;
            state_.emittedEvents += static_cast<int>(reusableRequest_.noteEvents.size());
            state_.currentSample = scheduler_.currentSample();
        }
        return reusableRequest_;
    }

    const ClapLiveCallbackBridgeState& ClapLiveCallbackBridge::state() const
    {
        return state_;
    }
}
