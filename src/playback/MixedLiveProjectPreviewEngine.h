#pragma once

#include "audio/ApplicationAudioService.h"
#include "midi/MidiRecordingMetronome.h"
#include "playback/SampleAccurateLoopCoordinator.h"
#include "playback/LoopWrapAcknowledgement.h"
#include "midi/MidiLoopCaptureBridge.h"

#include "clap/ClapLiveDirectPreviewEngine.h"
#include "vst/VstLiveDirectPreviewEngine.h"

#include <juce_audio_utils/juce_audio_utils.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace mw::playback
{
    struct MixedLiveProjectLoopConfig
    {
        bool enabled = false;
        int tempoBpm = 120;
        std::int64_t ticksPerQuarterNote = 960;
        std::int64_t playbackStartTick = 0;
        std::int64_t loopStartTick = 0;
        std::int64_t loopEndTick = 0;
        std::shared_ptr<mw::midi::MidiLoopCaptureFrameClock> captureClock;
    };

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
        bool sampleAccurateLoopActive = false;
        std::uint64_t loopWrapCount = 0;
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
        bool sampleAccurateLoopActive = false;
        std::uint64_t loopWrapCount = 0;
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
        bool sampleAccurateLoopActive = false;
        std::uint64_t loopWrapCount = 0;
        std::int64_t currentSample = 0;
        std::int64_t totalSamples = 0;
        float peak = 0.0f;
        juce::String lastMessage;

        juce::String toLogMessage() const;
    };

    class MixedLiveProjectPreviewEngine final
    {
    public:
        explicit MixedLiveProjectPreviewEngine(mw::audio::ApplicationAudioService& audioService);
        ~MixedLiveProjectPreviewEngine();

        MixedLiveProjectPreviewEngine(const MixedLiveProjectPreviewEngine&) = delete;
        MixedLiveProjectPreviewEngine& operator=(const MixedLiveProjectPreviewEngine&) = delete;

        MixedLiveProjectStartResult start(
            std::vector<mw::clap::ClapLiveDirectPreviewTrackRequest> clapTracks,
            std::vector<mw::vst::VstLiveDirectPreviewTrackRequest> vstTracks,
            std::vector<mw::clap::ClapLiveDirectPreviewAudioSourceRequest> audioSources,
            int outputChannelCount = 2,
            mw::midi::MidiRecordingMetronomeConfig recordingMetronome = {},
            double recordingMetronomeProjectStartSeconds = 0.0,
            MixedLiveProjectLoopConfig loopConfig = {});

        void setMidiRecordingMetronomeActive(bool active) noexcept;
        // Control-thread readout; a stale or overrun notice must never be used
        // to advance a recording pass. This does not itself enable loop recording.
        LoopWrapPollResult pollLoopWrapAcknowledgement(
            std::uint64_t& consumedSerial, LoopWrapAcknowledgement& notice) const noexcept;
        MixedLiveProjectStopSummary stop();
        MixedLiveProjectStatus status() const;
        bool isActive() const noexcept;
        int activeTrackIndex() const noexcept;
        bool submitLiveMidiMessage(mw::core::StableId trackId, const juce::MidiMessage& message) noexcept;
        void requestLiveMidiPanic(mw::core::StableId trackId) noexcept;

    private:
        class MixedProjectAudioCallback;

        mw::audio::ApplicationAudioService& audioService_;
        std::unique_ptr<MixedProjectAudioCallback> callback_;
        int activeTrackIndex_ = -1;
        bool active_ = false;
    };
}
