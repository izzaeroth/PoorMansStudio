#pragma once

#include "audio/ApplicationAudioService.h"
#include "audio/GainLimits.h"
#include "clap/ClapLiveCallbackBridge.h"
#include "core/StableId.h"
#include "vst/VstLiveEffectSession.h"
#include "vst/VstLiveInstrumentSession.h"

#include <juce_audio_utils/juce_audio_utils.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace mw::vst
{
    struct VstLiveDirectPreviewEffectRequest
    {
        int slotIndex = -1;
        std::string displayName;
        VstLiveEffectSession* session = nullptr;
        std::mutex* sessionProcessMutex = nullptr;
    };

    struct VstLiveDirectPreviewTrackRequest
    {
        mw::core::StableId stableTrackId = 0;
        int trackIndex = -1;
        std::string trackName;
        VstLiveInstrumentSession* session = nullptr;
        std::mutex* sessionProcessMutex = nullptr;
        std::vector<mw::clap::ClapLivePlaybackNote> notes;
        std::vector<mw::clap::ClapLivePlaybackControllerEvent> controllers;
        std::vector<mw::clap::ClapLivePlaybackPitchBendEvent> pitchBends;
        std::vector<mw::clap::ClapLivePlaybackProgramChangeEvent> programChanges;
        std::vector<mw::clap::ClapLivePlaybackChannelPressureEvent> channelPressures;
        std::vector<mw::clap::ClapLivePlaybackPolyPressureEvent> polyPressures;
        std::vector<mw::clap::ClapLivePlayback14BitControllerEvent> controller14Bits;
        std::vector<mw::clap::ClapLivePlaybackRpnEvent> rpns;
        std::vector<mw::clap::ClapLivePlaybackNrpnEvent> nrpns;
        mw::clap::ClapLiveCallbackBridgeConfig config;
        std::vector<VstLiveDirectPreviewEffectRequest> effects;
        float outputGain = 1.0f;
        bool liveMidiInput = false;
    };

    struct VstLiveDirectPreviewStartResult
    {
        bool started = false;
        int trackIndex = -1;
        int sampleRate = 48000;
        int outputChannelCount = 2;
        int scheduledEvents = 0;
        int maxEventsPerBlock = 0;
        int liveEffectSlotCount = 0;
        int processedBlocks = 0;
        int processedEffectBlocks = 0;
        int skippedBusyBlocks = 0;
        int skippedEffectBusyBlocks = 0;
        int clippedSamples = 0;
        int emergencyNoteOffsSubmitted = 0;
        std::int64_t currentSample = 0;
        std::int64_t totalSamples = 0;
        juce::String message;
        juce::String errorMessage;

        double durationSeconds() const noexcept;
    };

    struct VstLiveDirectPreviewStatus
    {
        bool active = false;
        bool finished = false;
        bool hadFailure = false;
        int trackIndex = -1;
        int processedBlocks = 0;
        int processedEffectBlocks = 0;
        int submittedEvents = 0;
        int skippedBusyBlocks = 0;
        int skippedEffectBusyBlocks = 0;
        int scheduledEvents = 0;
        int maxEventsPerBlock = 0;
        int liveEffectSlotCount = 0;
        int clippedSamples = 0;
        int emergencyNoteOffsSubmitted = 0;
        std::int64_t currentSample = 0;
        std::int64_t totalSamples = 0;
        float peak = 0.0f;
        juce::String lastMessage;

        bool isTerminal() const noexcept { return active && finished; }
        juce::String toStatusMessage() const;
    };

    struct VstLiveDirectPreviewStopSummary
    {
        bool wasActive = false;
        bool hadFailure = false;
        int trackIndex = -1;
        int processedBlocks = 0;
        int processedEffectBlocks = 0;
        int submittedEvents = 0;
        int skippedBusyBlocks = 0;
        int skippedEffectBusyBlocks = 0;
        int scheduledEvents = 0;
        int maxEventsPerBlock = 0;
        int liveEffectSlotCount = 0;
        int clippedSamples = 0;
        int emergencyNoteOffsSubmitted = 0;
        std::int64_t currentSample = 0;
        std::int64_t totalSamples = 0;
        float peak = 0.0f;
        juce::String lastMessage;

        juce::String toLogMessage() const;
    };

    class VstLiveDirectPreviewEngine final
    {
    public:
        explicit VstLiveDirectPreviewEngine(mw::audio::ApplicationAudioService& audioService);
        ~VstLiveDirectPreviewEngine();

        VstLiveDirectPreviewEngine(const VstLiveDirectPreviewEngine&) = delete;
        VstLiveDirectPreviewEngine& operator=(const VstLiveDirectPreviewEngine&) = delete;

        VstLiveDirectPreviewStartResult start(VstLiveDirectPreviewTrackRequest request,
                                              int outputChannelCount = 2);
        VstLiveDirectPreviewStopSummary stop();
        VstLiveDirectPreviewStatus status() const;
        bool isActive() const noexcept;
        int activeTrackIndex() const noexcept;
        bool submitLiveMidiMessage(const juce::MidiMessage& message) noexcept;
        void requestLiveMidiPanic() noexcept;

    private:
        class DirectPreviewAudioCallback;

        mw::audio::ApplicationAudioService& audioService_;
        std::unique_ptr<DirectPreviewAudioCallback> callback_;
        mutable std::mutex callbackAccessMutex_;
        int activeTrackIndex_ = -1;
        bool active_ = false;
        mw::audio::ApplicationAudioPlaybackOwner activeOwner_ = mw::audio::ApplicationAudioPlaybackOwner::None;
    };
}
