#include "clap/ClapLiveTrackSessionManager.h"

#include <utility>

namespace mw::clap
{
    struct ClapLiveTrackSessionManager::EffectSlot
    {
        int slotIndex = -1;
        std::string displayName;
        std::unique_ptr<ClapLiveEffectSession> session;
        std::mutex processMutex;
    };

    struct ClapLiveTrackSessionManager::TrackSlot
    {
        mw::core::StableId stableTrackId = 0;
        int trackIndex = -1;
        std::string trackName;
        std::unique_ptr<ClapLiveInstrumentSession> session;
        std::mutex processMutex;
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
        std::vector<std::unique_ptr<EffectSlot>> effects;
        float outputGain = 1.0f;
        bool liveMidiInput = false;
    };

    ClapLiveTrackSessionManager::ClapLiveTrackSessionManager() = default;

    ClapLiveTrackSessionManager::~ClapLiveTrackSessionManager()
    {
        close();
    }

    ClapLiveTrackSessionPrepareResult ClapLiveTrackSessionManager::prepare(std::vector<ClapLiveManagedTrackConfig> configs)
    {
        close();

        ClapLiveTrackSessionPrepareResult result;
        for (auto& config : configs)
        {
            if (config.stableTrackId == 0 || config.trackIndex < 0
                || (!config.liveMidiInput && config.notes.empty() && config.controllers.empty() && config.pitchBends.empty() && config.programChanges.empty() && config.channelPressures.empty() && config.polyPressures.empty() && config.controller14Bits.empty() && config.rpns.empty() && config.nrpns.empty()))
                continue;

            auto track = std::make_unique<TrackSlot>();
            track->stableTrackId = config.stableTrackId;
            track->trackIndex = config.trackIndex;
            track->trackName = std::move(config.trackName);
            track->notes = std::move(config.notes);
            track->controllers = std::move(config.controllers);
            track->pitchBends = std::move(config.pitchBends);
            track->programChanges = std::move(config.programChanges);
            track->channelPressures = std::move(config.channelPressures);
            track->polyPressures = std::move(config.polyPressures);
            track->controller14Bits = std::move(config.controller14Bits);
            track->rpns = std::move(config.rpns);
            track->nrpns = std::move(config.nrpns);
            track->bridgeConfig = std::move(config.bridgeConfig);
            track->outputGain = config.outputGain;
            track->liveMidiInput = config.liveMidiInput;
            track->session = std::make_unique<ClapLiveInstrumentSession>();

            std::string openError;
            if (!track->session->open(config.sessionConfig, openError))
            {
                result.message = "Failed to open CLAP live instrument for track "
                    + std::to_string(config.trackIndex + 1) + ": " + openError;
                close();
                return result;
            }

            const auto instrumentInfo = track->session->info();
            if (instrumentInfo.latencySamples > 0)
            {
                result.rejectedLatencySamples = instrumentInfo.latencySamples;
                result.message = "CLAP live instrument on track "
                    + std::to_string(config.trackIndex + 1)
                    + " reports " + std::to_string(instrumentInfo.latencySamples)
                    + " latency samples; rendered fallback is required for safe project alignment.";
                close();
                return result;
            }

            for (auto& effectConfig : config.effects)
            {
                auto effect = std::make_unique<EffectSlot>();
                effect->slotIndex = effectConfig.slotIndex;
                effect->displayName = std::move(effectConfig.displayName);
                effect->session = std::make_unique<ClapLiveEffectSession>();

                std::string effectOpenError;
                if (!effect->session->open(effectConfig.sessionConfig, effectOpenError))
                {
                    result.message = "Failed to open CLAP live effect on track "
                        + std::to_string(config.trackIndex + 1) + ", slot "
                        + std::to_string(effectConfig.slotIndex + 1) + ": " + effectOpenError;
                    close();
                    return result;
                }

                const auto effectInfo = effect->session->info();
                if (effectInfo.latencySamples > 0)
                {
                    result.rejectedLatencySamples = effectInfo.latencySamples;
                    result.message = "CLAP live effect on track "
                        + std::to_string(config.trackIndex + 1) + ", slot "
                        + std::to_string(effectConfig.slotIndex + 1)
                        + " reports " + std::to_string(effectInfo.latencySamples)
                        + " latency samples; rendered fallback is required for safe project alignment.";
                    close();
                    return result;
                }

                track->effects.push_back(std::move(effect));
                ++result.preparedEffectCount;
            }

            tracks_.push_back(std::move(track));
            ++result.preparedTrackCount;
        }

        result.success = result.preparedTrackCount > 0;
        result.message = result.success
            ? "Prepared " + std::to_string(result.preparedTrackCount) + " CLAP live track session(s)."
            : "No CLAP live tracks were eligible for preparation.";
        return result;
    }

    std::vector<ClapLiveDirectPreviewTrackRequest> ClapLiveTrackSessionManager::makeDirectPreviewRequests()
    {
        std::vector<ClapLiveDirectPreviewTrackRequest> requests;
        requests.reserve(tracks_.size());

        for (auto& track : tracks_)
        {
            if (track == nullptr || track->session == nullptr || !track->session->isOpen())
                continue;

            ClapLiveDirectPreviewTrackRequest request;
            request.stableTrackId = track->stableTrackId;
            request.trackIndex = track->trackIndex;
            request.trackName = track->trackName;
            request.session = track->session.get();
            request.sessionProcessMutex = &track->processMutex;
            request.notes = track->notes;
            request.controllers = track->controllers;
            request.pitchBends = track->pitchBends;
            request.programChanges = track->programChanges;
            request.channelPressures = track->channelPressures;
            request.polyPressures = track->polyPressures;
            request.controller14Bits = track->controller14Bits;
            request.rpns = track->rpns;
            request.nrpns = track->nrpns;
            request.config = track->bridgeConfig;
            request.outputGain = track->outputGain;
            request.liveMidiInput = track->liveMidiInput;
            request.effects.reserve(track->effects.size());

            for (auto& effect : track->effects)
            {
                if (effect == nullptr || effect->session == nullptr || !effect->session->isOpen())
                    continue;

                ClapLiveDirectPreviewEffectRequest effectRequest;
                effectRequest.slotIndex = effect->slotIndex;
                effectRequest.displayName = effect->displayName;
                effectRequest.session = effect->session.get();
                effectRequest.sessionProcessMutex = &effect->processMutex;
                request.effects.push_back(std::move(effectRequest));
            }

            requests.push_back(std::move(request));
        }

        return requests;
    }

    std::vector<mw::core::StableId> ClapLiveTrackSessionManager::stableTrackIds() const
    {
        std::vector<mw::core::StableId> ids;
        ids.reserve(tracks_.size());
        for (const auto& track : tracks_)
            if (track != nullptr && track->stableTrackId != 0)
                ids.push_back(track->stableTrackId);
        return ids;
    }

    int ClapLiveTrackSessionManager::trackCount() const noexcept
    {
        return static_cast<int>(tracks_.size());
    }

    void ClapLiveTrackSessionManager::close()
    {
        for (auto& track : tracks_)
        {
            if (track == nullptr)
                continue;

            for (auto& effect : track->effects)
                if (effect != nullptr && effect->session != nullptr)
                    effect->session->close();

            if (track->session != nullptr)
                track->session->close();
        }
        tracks_.clear();
    }
}
