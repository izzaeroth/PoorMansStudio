#pragma once

#include "clap/ClapLiveDirectPreviewEngine.h"
#include "core/StableId.h"

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace mw::clap
{
    struct ClapLiveManagedEffectConfig
    {
        int slotIndex = -1;
        std::string displayName;
        ClapLiveEffectSessionConfig sessionConfig;
    };

    struct ClapLiveManagedTrackConfig
    {
        mw::core::StableId stableTrackId = 0;
        int trackIndex = -1;
        std::string trackName;
        ClapLiveInstrumentSessionConfig sessionConfig;
        std::vector<ClapLivePlaybackNote> notes;
        std::vector<ClapLivePlaybackControllerEvent> controllers;
        std::vector<ClapLivePlaybackPitchBendEvent> pitchBends;
        std::vector<ClapLivePlaybackProgramChangeEvent> programChanges;
        std::vector<ClapLivePlaybackChannelPressureEvent> channelPressures;
        std::vector<ClapLivePlaybackPolyPressureEvent> polyPressures;
        std::vector<ClapLivePlayback14BitControllerEvent> controller14Bits;
        std::vector<ClapLivePlaybackRpnEvent> rpns;
        std::vector<ClapLivePlaybackNrpnEvent> nrpns;
        ClapLiveCallbackBridgeConfig bridgeConfig;
        std::vector<ClapLiveManagedEffectConfig> effects;
        float outputGain = 1.0f;
        bool liveMidiInput = false;
    };

    struct ClapLiveTrackSessionPrepareResult
    {
        bool success = false;
        int preparedTrackCount = 0;
        int preparedEffectCount = 0;
        int rejectedLatencySamples = 0;
        std::string message;
    };

    class ClapLiveTrackSessionManager final
    {
    public:
        ClapLiveTrackSessionManager();
        ~ClapLiveTrackSessionManager();

        ClapLiveTrackSessionManager(const ClapLiveTrackSessionManager&) = delete;
        ClapLiveTrackSessionManager& operator=(const ClapLiveTrackSessionManager&) = delete;

        ClapLiveTrackSessionPrepareResult prepare(std::vector<ClapLiveManagedTrackConfig> configs);
        std::vector<ClapLiveDirectPreviewTrackRequest> makeDirectPreviewRequests();
        std::vector<mw::core::StableId> stableTrackIds() const;
        int trackCount() const noexcept;
        void close();

    private:
        struct EffectSlot;
        struct TrackSlot;
        std::vector<std::unique_ptr<TrackSlot>> tracks_;
    };
}
