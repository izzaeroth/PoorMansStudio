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
        ClapLiveCallbackBridgeConfig bridgeConfig;
        std::vector<std::unique_ptr<EffectSlot>> effects;
        float outputGain = 1.0f;
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
            if (config.stableTrackId == 0 || config.trackIndex < 0 || config.notes.empty())
                continue;

            auto track = std::make_unique<TrackSlot>();
            track->stableTrackId = config.stableTrackId;
            track->trackIndex = config.trackIndex;
            track->trackName = std::move(config.trackName);
            track->notes = std::move(config.notes);
            track->bridgeConfig = std::move(config.bridgeConfig);
            track->outputGain = config.outputGain;
            track->session = std::make_unique<ClapLiveInstrumentSession>();

            std::string openError;
            if (!track->session->open(config.sessionConfig, openError))
            {
                result.message = "Failed to open CLAP live instrument for track "
                    + std::to_string(config.trackIndex + 1) + ": " + openError;
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
            request.config = track->bridgeConfig;
            request.outputGain = track->outputGain;
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
