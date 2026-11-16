#pragma once

#include "clap/ClapLivePlaybackScheduler.h"

#include <cstdint>
#include <string>
#include <vector>

namespace mw::clap
{
    struct ClapLiveCallbackBridgeConfig
    {
        int trackIndex = -1;
        std::string trackName;
        ClapLivePlaybackSchedulerConfig scheduler;
        int outputChannelCount = 2;
    };

    struct ClapLiveCallbackBridgeState
    {
        bool prepared = false;
        bool directOutputEnabled = false;
        int trackIndex = -1;
        std::string trackName;
        int sampleRate = 48000;
        int blockSize = 512;
        int outputChannelCount = 2;
        int totalScheduledEvents = 0;
        int maxEventsPerBlock = 0;
        int emittedBlocks = 0;
        int emittedEvents = 0;
        std::int64_t currentSample = 0;
        std::int64_t totalSamples = 0;
        std::string message;
    };

    class ClapLiveCallbackBridge final
    {
    public:
        ClapLiveCallbackBridge() = default;

        void prepare(std::vector<ClapLivePlaybackNote> notes, ClapLiveCallbackBridgeConfig config);
        void prepare(std::vector<ClapLivePlaybackNote> notes, std::vector<ClapLivePlaybackControllerEvent> controllerEvents, ClapLiveCallbackBridgeConfig config);
        void prepare(std::vector<ClapLivePlaybackNote> notes, std::vector<ClapLivePlaybackControllerEvent> controllerEvents,
                     std::vector<ClapLivePlaybackPitchBendEvent> pitchBendEvents, ClapLiveCallbackBridgeConfig config);
        void prepare(std::vector<ClapLivePlaybackNote> notes, std::vector<ClapLivePlaybackControllerEvent> controllerEvents,
                     std::vector<ClapLivePlaybackPitchBendEvent> pitchBendEvents,
                     std::vector<ClapLivePlaybackProgramChangeEvent> programChangeEvents, ClapLiveCallbackBridgeConfig config);
        void prepare(std::vector<ClapLivePlaybackNote> notes, std::vector<ClapLivePlaybackControllerEvent> controllerEvents,
                     std::vector<ClapLivePlaybackPitchBendEvent> pitchBendEvents,
                     std::vector<ClapLivePlaybackProgramChangeEvent> programChangeEvents,
                     std::vector<ClapLivePlaybackChannelPressureEvent> channelPressureEvents, ClapLiveCallbackBridgeConfig config);
        void prepare(std::vector<ClapLivePlaybackNote> notes, std::vector<ClapLivePlaybackControllerEvent> controllerEvents,
                     std::vector<ClapLivePlaybackPitchBendEvent> pitchBendEvents,
                     std::vector<ClapLivePlaybackProgramChangeEvent> programChangeEvents,
                     std::vector<ClapLivePlaybackChannelPressureEvent> channelPressureEvents,
                     std::vector<ClapLivePlaybackPolyPressureEvent> polyPressureEvents, ClapLiveCallbackBridgeConfig config);
        void prepare(std::vector<ClapLivePlaybackNote> notes, std::vector<ClapLivePlaybackControllerEvent> controllerEvents,
                     std::vector<ClapLivePlaybackPitchBendEvent> pitchBendEvents,
                     std::vector<ClapLivePlaybackProgramChangeEvent> programChangeEvents,
                     std::vector<ClapLivePlaybackChannelPressureEvent> channelPressureEvents,
                     std::vector<ClapLivePlaybackPolyPressureEvent> polyPressureEvents,
                     std::vector<ClapLivePlayback14BitControllerEvent> controller14BitEvents,
                     std::vector<ClapLivePlaybackRpnEvent> rpnEvents,
                     std::vector<ClapLivePlaybackNrpnEvent> nrpnEvents, ClapLiveCallbackBridgeConfig config);
        bool isPrepared() const;
        bool isFinished() const;
        void rewind() noexcept;
        const ClapLiveProcessRequest& nextRequest(int maximumFrames);
        const ClapLiveProcessRequest& nextRequest();
        const ClapLiveCallbackBridgeState& state() const;

    private:
        ClapLiveCallbackBridgeConfig config_;
        ClapLivePlaybackScheduler scheduler_;
        ClapLiveProcessRequest reusableRequest_;
        ClapLiveCallbackBridgeState state_;
    };
}
