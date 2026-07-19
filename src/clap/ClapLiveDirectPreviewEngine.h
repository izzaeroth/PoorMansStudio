#pragma once

#include "clap/ClapLiveCallbackBridge.h"
#include "clap/ClapLiveEffectSession.h"
#include "clap/ClapLiveInstrumentSession.h"
#include "core/StableId.h"

#include <juce_audio_utils/juce_audio_utils.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace mw::clap
{
    struct ClapLiveDirectPreviewEffectRequest
    {
        int slotIndex = -1;
        std::string displayName;
        ClapLiveEffectSession* session = nullptr;
        std::mutex* sessionProcessMutex = nullptr;
    };

    struct ClapLiveDirectPreviewTrackRequest
    {
        mw::core::StableId stableTrackId = 0;
        int trackIndex = -1;
        std::string trackName;
        ClapLiveInstrumentSession* session = nullptr;
        std::mutex* sessionProcessMutex = nullptr;
        std::vector<ClapLivePlaybackNote> notes;
        ClapLiveCallbackBridgeConfig config;
        std::vector<ClapLiveDirectPreviewEffectRequest> effects;
        float outputGain = 1.0f;
    };


    struct ClapLiveDirectPreviewAudioSourceRequest
    {
        std::string displayName;
        int sourceSampleRate = 48000;
        int sourceChannelCount = 2;
        std::int64_t startSample = 0;
        std::vector<float> interleavedAudio;
        float outputGain = 1.0f;

        std::int64_t sourceFrameCount() const noexcept
        {
            return sourceChannelCount > 0
                ? static_cast<std::int64_t>(interleavedAudio.size() / static_cast<std::size_t>(sourceChannelCount))
                : 0;
        }
    };

    struct ClapLiveDirectPreviewStartResult
    {
        bool started = false;
        int trackIndex = -1;
        int trackCount = 0;
        int audioSourceCount = 0;
        int sampleRate = 48000;
        int outputChannelCount = 2;
        int scheduledEvents = 0;
        int maxEventsPerBlock = 0;
        int liveEffectSlotCount = 0;
        int processedEffectBlocks = 0;
        int skippedBusyBlocks = 0;
        int skippedEffectBusyBlocks = 0;
        int consecutiveBusyBlockLimit = 0;
        int emergencyNoteOffsSubmitted = 0;
        int stopFlushAttempted = 0;
        int stopFlushSucceeded = 0;
        int stopFlushSkippedBusy = 0;
        int clippedSamples = 0;
        std::int64_t currentSample = 0;
        std::int64_t totalSamples = 0;
        juce::String message;
        juce::String errorMessage;

        double durationSeconds() const noexcept;
    };


    struct ClapLiveDirectPreviewStatus
    {
        bool active = false;
        bool finished = false;
        bool hadFailure = false;
        int trackIndex = -1;
        int trackCount = 0;
        int audioSourceCount = 0;
        int processedBlocks = 0;
        int submittedEvents = 0;
        int skippedBusyBlocks = 0;
        int skippedEffectBusyBlocks = 0;
        int scheduledEvents = 0;
        int maxEventsPerBlock = 0;
        int liveEffectSlotCount = 0;
        int processedEffectBlocks = 0;
        int consecutiveBusyBlockLimit = 0;
        int emergencyNoteOffsSubmitted = 0;
        int stopFlushAttempted = 0;
        int stopFlushSucceeded = 0;
        int stopFlushSkippedBusy = 0;
        int clippedSamples = 0;
        std::int64_t currentSample = 0;
        std::int64_t totalSamples = 0;
        float peak = 0.0f;
        juce::String lastMessage;

        bool isTerminal() const noexcept { return active && finished; }
        juce::String toStatusMessage() const;
    };

    struct ClapLiveDirectPreviewStopSummary
    {
        bool wasActive = false;
        bool hadFailure = false;
        int trackIndex = -1;
        int trackCount = 0;
        int audioSourceCount = 0;
        int processedBlocks = 0;
        int submittedEvents = 0;
        int skippedBusyBlocks = 0;
        int scheduledEvents = 0;
        int maxEventsPerBlock = 0;
        int liveEffectSlotCount = 0;
        int processedEffectBlocks = 0;
        int skippedEffectBusyBlocks = 0;
        int consecutiveBusyBlockLimit = 0;
        int emergencyNoteOffsSubmitted = 0;
        int stopFlushAttempted = 0;
        int stopFlushSucceeded = 0;
        int stopFlushSkippedBusy = 0;
        int clippedSamples = 0;
        std::int64_t currentSample = 0;
        std::int64_t totalSamples = 0;
        float peak = 0.0f;
        juce::String lastMessage;

        juce::String toLogMessage() const;
    };

    class ClapLiveDirectPreviewEngine final
    {
    public:
        ClapLiveDirectPreviewEngine();
        ~ClapLiveDirectPreviewEngine();

        ClapLiveDirectPreviewEngine(const ClapLiveDirectPreviewEngine&) = delete;
        ClapLiveDirectPreviewEngine& operator=(const ClapLiveDirectPreviewEngine&) = delete;

        ClapLiveDirectPreviewStartResult start(ClapLiveInstrumentSession& session,
                                               std::mutex& sessionProcessMutex,
                                               std::vector<ClapLivePlaybackNote> notes,
                                               ClapLiveCallbackBridgeConfig config);

        ClapLiveDirectPreviewStartResult start(std::vector<ClapLiveDirectPreviewTrackRequest> tracks,
                                               int outputChannelCount = 2);

        ClapLiveDirectPreviewStartResult start(std::vector<ClapLiveDirectPreviewTrackRequest> tracks,
                                               std::vector<ClapLiveDirectPreviewAudioSourceRequest> audioSources,
                                               int outputChannelCount = 2);

        ClapLiveDirectPreviewStopSummary stop();
        ClapLiveDirectPreviewStatus status() const;

        bool isActive() const noexcept;
        int activeTrackIndex() const noexcept;

    private:
        class DirectPreviewAudioCallback;

        juce::AudioDeviceManager deviceManager_;
        std::unique_ptr<DirectPreviewAudioCallback> callback_;
        int activeTrackIndex_ = -1;
        bool active_ = false;
    };
}
