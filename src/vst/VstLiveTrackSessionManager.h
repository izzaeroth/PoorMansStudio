#pragma once

#include "clap/ClapLivePlaybackScheduler.h"
#include "clap/ClapLiveCallbackBridge.h"
#include "core/StableId.h"
#include "vst/VstLiveDirectPreviewEngine.h"
#include "vst/VstLiveEffectSession.h"
#include "vst/VstLiveInstrumentSession.h"

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace mw::vst
{
    struct VstLiveManagedEffectConfig
    {
        int slotIndex = -1;
        std::string displayName;
        VstLiveEffectSessionConfig sessionConfig;
    };

    struct VstLiveManagedTrackConfig
    {
        mw::core::StableId stableTrackId = 0;
        int trackIndex = -1;
        std::string trackName;
        VstLiveInstrumentSessionConfig sessionConfig;
        std::vector<mw::clap::ClapLivePlaybackNote> notes;
        mw::clap::ClapLiveCallbackBridgeConfig bridgeConfig;
        std::vector<VstLiveManagedEffectConfig> effects;
        float outputGain = 1.0f;
    };

    struct VstLiveTrackSessionPrepareResult
    {
        bool success = false;
        int preparedTrackCount = 0;
        int preparedEffectCount = 0;
        int rejectedLatencySamples = 0;
        std::string message;
    };

    class VstLiveTrackSessionManager final
    {
    public:
        VstLiveTrackSessionManager();
        ~VstLiveTrackSessionManager();

        VstLiveTrackSessionManager(const VstLiveTrackSessionManager&) = delete;
        VstLiveTrackSessionManager& operator=(const VstLiveTrackSessionManager&) = delete;

        VstLiveTrackSessionPrepareResult prepare(std::vector<VstLiveManagedTrackConfig> configs);
        std::vector<VstLiveDirectPreviewTrackRequest> makeDirectPreviewRequests();
        std::vector<mw::core::StableId> stableTrackIds() const;
        int trackCount() const noexcept;
        void close();

    private:
        struct EffectSlot;
        struct TrackSlot;
        std::vector<std::unique_ptr<TrackSlot>> tracks_;
    };
}
