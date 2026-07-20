#pragma once

#include "clap/ClapLiveDirectPreviewEngine.h"
#include "vst/VstLiveDirectPreviewEngine.h"

#include <juce_audio_utils/juce_audio_utils.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace mw::playback
{
    struct MixedLiveProjectStartResult
    {
        bool started = false;
        int trackIndex = -1;
        int trackCount = 0;
        int clapTrackCount = 0;
        int vstTrackCount = 0;
        int audioSourceCount = 0;
        int sampleRate = 48000;
        int blockSize = 512;
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

    struct MixedLiveProjectStatus
    {
        bool active = false;
        bool finished = false;
        bool hadFailure = false;
        int trackIndex = -1;
        int trackCount = 0;
        int clapTrackCount = 0;
        int vstTrackCount = 0;
        int audioSourceCount = 0;
        int sampleRate = 48000;
        int blockSize = 512;
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

    struct MixedLiveProjectStopSummary
    {
        bool wasActive = false;
        bool hadFailure = false;
        int trackIndex = -1;
        int trackCount = 0;
        int clapTrackCount = 0;
        int vstTrackCount = 0;
        int audioSourceCount = 0;
        int sampleRate = 48000;
        int blockSize = 512;
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

        juce::String toLogMessage() const;
    };

    class MixedLiveProjectPreviewEngine final
    {
    public:
        MixedLiveProjectPreviewEngine();
        ~MixedLiveProjectPreviewEngine();

        MixedLiveProjectPreviewEngine(const MixedLiveProjectPreviewEngine&) = delete;
        MixedLiveProjectPreviewEngine& operator=(const MixedLiveProjectPreviewEngine&) = delete;

        MixedLiveProjectStartResult start(
            std::vector<mw::clap::ClapLiveDirectPreviewTrackRequest> clapTracks,
            std::vector<mw::vst::VstLiveDirectPreviewTrackRequest> vstTracks,
            std::vector<mw::clap::ClapLiveDirectPreviewAudioSourceRequest> audioSources,
            int outputChannelCount = 2);

        MixedLiveProjectStopSummary stop();
        MixedLiveProjectStatus status() const;
        bool isActive() const noexcept;
        int activeTrackIndex() const noexcept;

    private:
        class MixedProjectAudioCallback;

        juce::AudioDeviceManager deviceManager_;
        std::unique_ptr<MixedProjectAudioCallback> callback_;
        int activeTrackIndex_ = -1;
        bool active_ = false;
    };
}
