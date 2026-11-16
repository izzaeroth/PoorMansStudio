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
        prepare(std::move(notes), {}, {}, {}, {}, {}, std::move(config));
    }

    void ClapLiveCallbackBridge::prepare(std::vector<ClapLivePlaybackNote> notes, std::vector<ClapLivePlaybackControllerEvent> controllerEvents, ClapLiveCallbackBridgeConfig config)
    {
        prepare(std::move(notes), std::move(controllerEvents), {}, {}, {}, {}, std::move(config));
    }

    void ClapLiveCallbackBridge::prepare(std::vector<ClapLivePlaybackNote> notes, std::vector<ClapLivePlaybackControllerEvent> controllerEvents,
                                         std::vector<ClapLivePlaybackPitchBendEvent> pitchBendEvents, ClapLiveCallbackBridgeConfig config)
    {
        prepare(std::move(notes), std::move(controllerEvents), std::move(pitchBendEvents), {}, {}, {}, std::move(config));
    }

    void ClapLiveCallbackBridge::prepare(std::vector<ClapLivePlaybackNote> notes, std::vector<ClapLivePlaybackControllerEvent> controllerEvents,
                                         std::vector<ClapLivePlaybackPitchBendEvent> pitchBendEvents,
                                         std::vector<ClapLivePlaybackProgramChangeEvent> programChangeEvents, ClapLiveCallbackBridgeConfig config)
    {
        prepare(std::move(notes), std::move(controllerEvents), std::move(pitchBendEvents), std::move(programChangeEvents), {}, {}, std::move(config));
    }

    void ClapLiveCallbackBridge::prepare(std::vector<ClapLivePlaybackNote> notes, std::vector<ClapLivePlaybackControllerEvent> controllerEvents,
                                         std::vector<ClapLivePlaybackPitchBendEvent> pitchBendEvents,
                                         std::vector<ClapLivePlaybackProgramChangeEvent> programChangeEvents,
                                         std::vector<ClapLivePlaybackChannelPressureEvent> channelPressureEvents, ClapLiveCallbackBridgeConfig config)
    {
        prepare(std::move(notes), std::move(controllerEvents), std::move(pitchBendEvents), std::move(programChangeEvents), std::move(channelPressureEvents), {}, std::move(config));
    }

    void ClapLiveCallbackBridge::prepare(std::vector<ClapLivePlaybackNote> notes, std::vector<ClapLivePlaybackControllerEvent> controllerEvents,
                                         std::vector<ClapLivePlaybackPitchBendEvent> pitchBendEvents,
                                         std::vector<ClapLivePlaybackProgramChangeEvent> programChangeEvents,
                                         std::vector<ClapLivePlaybackChannelPressureEvent> channelPressureEvents,
                                         std::vector<ClapLivePlaybackPolyPressureEvent> polyPressureEvents, ClapLiveCallbackBridgeConfig config)
    {
        prepare(std::move(notes), std::move(controllerEvents), std::move(pitchBendEvents), std::move(programChangeEvents),
                std::move(channelPressureEvents), std::move(polyPressureEvents), {}, {}, {}, std::move(config));
    }

    void ClapLiveCallbackBridge::prepare(std::vector<ClapLivePlaybackNote> notes, std::vector<ClapLivePlaybackControllerEvent> controllerEvents,
                                         std::vector<ClapLivePlaybackPitchBendEvent> pitchBendEvents,
                                         std::vector<ClapLivePlaybackProgramChangeEvent> programChangeEvents,
                                         std::vector<ClapLivePlaybackChannelPressureEvent> channelPressureEvents,
                                         std::vector<ClapLivePlaybackPolyPressureEvent> polyPressureEvents,
                                         std::vector<ClapLivePlayback14BitControllerEvent> controller14BitEvents,
                                         std::vector<ClapLivePlaybackRpnEvent> rpnEvents,
                                         std::vector<ClapLivePlaybackNrpnEvent> nrpnEvents, ClapLiveCallbackBridgeConfig config)
    {
        config.scheduler.sampleRate = config.scheduler.sampleRate > 0 ? config.scheduler.sampleRate : kDefaultSampleRate;
        config.scheduler.blockSize = config.scheduler.blockSize > 0 ? config.scheduler.blockSize : kDefaultBlockSize;
        config.outputChannelCount = std::clamp(config.outputChannelCount > 0 ? config.outputChannelCount : kDefaultOutputChannels, 1, 32);
        config_ = std::move(config);

        scheduler_.prepare(std::move(notes), std::move(controllerEvents), std::move(pitchBendEvents), std::move(programChangeEvents),
                           std::move(channelPressureEvents), std::move(polyPressureEvents), std::move(controller14BitEvents),
                           std::move(rpnEvents), std::move(nrpnEvents), config_.scheduler);
        reusableRequest_.frameCount = 0;
        reusableRequest_.noteEvents.clear();
        reusableRequest_.controllerEvents.clear();
        reusableRequest_.pitchBendEvents.clear();
        reusableRequest_.programChangeEvents.clear();
        reusableRequest_.channelPressureEvents.clear();
        reusableRequest_.polyPressureEvents.clear();
        reusableRequest_.noteEvents.reserve(static_cast<std::size_t>(std::max(0, scheduler_.maxEventsPerBlock())));
        reusableRequest_.controllerEvents.reserve(static_cast<std::size_t>(std::max(0, scheduler_.maxEventsPerBlock())));
        reusableRequest_.pitchBendEvents.reserve(static_cast<std::size_t>(std::max(0, scheduler_.maxEventsPerBlock())));
        reusableRequest_.programChangeEvents.reserve(static_cast<std::size_t>(std::max(0, scheduler_.maxEventsPerBlock())));
        reusableRequest_.channelPressureEvents.reserve(static_cast<std::size_t>(std::max(0, scheduler_.maxEventsPerBlock())));
        reusableRequest_.polyPressureEvents.reserve(static_cast<std::size_t>(std::max(0, scheduler_.maxEventsPerBlock())));

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
                    << " with " << state_.totalScheduledEvents << " scheduled MIDI events"
                    << " across " << state_.totalSamples << " samples.";
        }
        else
        {
            message << "CLAP preview has no schedulable MIDI events.";
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

    void ClapLiveCallbackBridge::rewind() noexcept
    {
        scheduler_.rewind();
        state_.currentSample = scheduler_.currentSample();
    }

    const ClapLiveProcessRequest& ClapLiveCallbackBridge::nextRequest(int maximumFrames)
    {
        scheduler_.nextFramesInto(reusableRequest_, maximumFrames);
        if (reusableRequest_.frameCount > 0)
        {
            ++state_.emittedBlocks;
            state_.emittedEvents += static_cast<int>(reusableRequest_.noteEvents.size() + reusableRequest_.controllerEvents.size() + reusableRequest_.programChangeEvents.size() + reusableRequest_.pitchBendEvents.size() + reusableRequest_.channelPressureEvents.size() + reusableRequest_.polyPressureEvents.size());
            state_.currentSample = scheduler_.currentSample();
        }
        return reusableRequest_;
    }

    const ClapLiveProcessRequest& ClapLiveCallbackBridge::nextRequest()
    {
        return nextRequest(scheduler_.blockSize());
    }

    const ClapLiveCallbackBridgeState& ClapLiveCallbackBridge::state() const
    {
        return state_;
    }
}
