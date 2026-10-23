#include "playback/MixedLiveProjectPreviewEngine.h"

#include "audio/GainLimits.h"
#include "midi/LiveMidiNoteQueue.h"
#include "midi/MidiRecordingMetronome.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <mutex>
#include <string>
#include <utility>

namespace mw::playback
{
    namespace
    {
        constexpr int kFallbackSampleRate = 48000;
        constexpr int kMinimumDeviceSampleRate = 8000;
        constexpr int kMaximumDeviceSampleRate = 384000;
        constexpr int kMinimumBlockSize = 16;
        constexpr int kMaximumBlockSize = 8192;
        constexpr int kMinimumOutputChannels = 1;
        constexpr int kMaximumOutputChannels = 32;
        constexpr int kMaximumPluginChannels = 64;
        constexpr int kMaximumConsecutiveBusyBlocks = 200;
        constexpr int kMaximumQueuedLiveMidiEventsPerBlock = 128;
        constexpr int kMaximumPanicNoteOffEvents = 16 * 128;
        constexpr int kMaximumCombinedMidiEventsPerBlock = kMaximumQueuedLiveMidiEventsPerBlock + kMaximumPanicNoteOffEvents + 512;

        int sanitizeOutputChannelCount(int value)
        {
            return std::clamp(value > 0 ? value : 2, kMinimumOutputChannels, kMaximumOutputChannels);
        }

        int noteChannelIndex(int midiChannel) noexcept
        {
            return std::clamp(midiChannel <= 0 ? 1 : midiChannel, 1, 16) - 1;
        }

        int noteKeyIndex(int key) noexcept
        {
            return std::clamp(key, 0, 127);
        }

        bool hasValidPluginOutputBuffer(int expectedFrames,
                                        int outputFrames,
                                        int outputChannels,
                                        std::size_t sampleCount) noexcept
        {
            if (expectedFrames <= 0
                || outputFrames != expectedFrames
                || outputChannels <= 0
                || outputChannels > kMaximumPluginChannels)
            {
                return false;
            }

            const auto required = static_cast<std::size_t>(expectedFrames)
                * static_cast<std::size_t>(outputChannels);
            return sampleCount >= required;
        }
    }

    class MixedLiveProjectPreviewEngine::MixedProjectAudioCallback final : public juce::AudioIODeviceCallback
    {
    public:
        MixedProjectAudioCallback(
            std::vector<mw::clap::ClapLiveDirectPreviewTrackRequest> clapRequests,
            std::vector<mw::vst::VstLiveDirectPreviewTrackRequest> vstRequests,
            std::vector<mw::clap::ClapLiveDirectPreviewAudioSourceRequest> audioSourceRequests,
            int requestedOutputChannelsIn,
            mw::midi::MidiRecordingMetronomeConfig recordingMetronomeIn,
            double recordingMetronomeProjectStartSecondsIn)
            : requestedOutputChannels(sanitizeOutputChannelCount(requestedOutputChannelsIn)),
              recordingMetronome(std::move(recordingMetronomeIn)),
              recordingMetronomeProjectStartSeconds(std::isfinite(recordingMetronomeProjectStartSecondsIn)
                  ? std::max(0.0, recordingMetronomeProjectStartSecondsIn)
                  : 0.0)
        {
            clapTracks.reserve(clapRequests.size());
            for (auto& request : clapRequests)
            {
                if (request.session == nullptr || request.sessionProcessMutex == nullptr
                    || (!request.liveMidiInput && request.notes.empty() && request.controllers.empty() && request.pitchBends.empty() && request.programChanges.empty() && request.channelPressures.empty() && request.polyPressures.empty()))
                    continue;

                ClapTrackState state;
                state.stableTrackId = request.stableTrackId;
                state.liveMidiInput = request.liveMidiInput;
                if (state.liveMidiInput)
                {
                    state.liveMidiQueue = std::make_unique<mw::midi::LiveMidiNoteQueue>();
                    state.liveMidiPanicRequested = std::make_unique<std::atomic<bool>>(false);
                    state.combinedRequest.noteEvents.reserve(static_cast<std::size_t>(kMaximumCombinedMidiEventsPerBlock));
                    state.combinedRequest.controllerEvents.reserve(static_cast<std::size_t>(kMaximumCombinedMidiEventsPerBlock));
                    state.combinedRequest.pitchBendEvents.reserve(static_cast<std::size_t>(kMaximumCombinedMidiEventsPerBlock));
                    state.combinedRequest.programChangeEvents.reserve(static_cast<std::size_t>(kMaximumCombinedMidiEventsPerBlock));
                    state.combinedRequest.channelPressureEvents.reserve(static_cast<std::size_t>(kMaximumCombinedMidiEventsPerBlock));
                }
                state.session = request.session;
                state.sessionMutex = request.sessionProcessMutex;
                state.notes = std::move(request.notes);
                state.controllers = std::move(request.controllers);
                state.pitchBends = std::move(request.pitchBends);
                state.programChanges = std::move(request.programChanges);
                state.channelPressures = std::move(request.channelPressures);
                state.polyPressures = std::move(request.polyPressures);
                state.config = std::move(request.config);
                state.outputGain = mw::audio::sanitizeMainUiGain(request.outputGain);
                if (state.config.trackIndex < 0)
                    state.config.trackIndex = request.trackIndex;
                if (state.config.trackName.empty())
                    state.config.trackName = request.trackName;
                state.config.outputChannelCount = requestedOutputChannels;

                state.effects.reserve(request.effects.size());
                for (const auto& effectRequest : request.effects)
                {
                    if (effectRequest.session == nullptr || effectRequest.sessionProcessMutex == nullptr)
                        continue;

                    ClapEffectState effect;
                    effect.slotIndex = effectRequest.slotIndex;
                    effect.displayName = effectRequest.displayName;
                    effect.session = effectRequest.session;
                    effect.sessionMutex = effectRequest.sessionProcessMutex;
                    state.effects.push_back(std::move(effect));
                }
                clapTracks.push_back(std::move(state));
            }

            vstTracks.reserve(vstRequests.size());
            for (auto& request : vstRequests)
            {
                if (request.session == nullptr || request.sessionProcessMutex == nullptr
                    || (!request.liveMidiInput && request.notes.empty() && request.controllers.empty() && request.pitchBends.empty() && request.programChanges.empty() && request.channelPressures.empty() && request.polyPressures.empty()))
                    continue;

                VstTrackState state;
                state.stableTrackId = request.stableTrackId;
                state.liveMidiInput = request.liveMidiInput;
                if (state.liveMidiInput)
                {
                    state.liveMidiQueue = std::make_unique<mw::midi::LiveMidiNoteQueue>();
                    state.liveMidiPanicRequested = std::make_unique<std::atomic<bool>>(false);
                    state.combinedRequest.noteEvents.reserve(static_cast<std::size_t>(kMaximumCombinedMidiEventsPerBlock));
                    state.combinedRequest.controllerEvents.reserve(static_cast<std::size_t>(kMaximumCombinedMidiEventsPerBlock));
                    state.combinedRequest.pitchBendEvents.reserve(static_cast<std::size_t>(kMaximumCombinedMidiEventsPerBlock));
                    state.combinedRequest.programChangeEvents.reserve(static_cast<std::size_t>(kMaximumCombinedMidiEventsPerBlock));
                    state.combinedRequest.channelPressureEvents.reserve(static_cast<std::size_t>(kMaximumCombinedMidiEventsPerBlock));
                }
                state.session = request.session;
                state.sessionMutex = request.sessionProcessMutex;
                state.notes = std::move(request.notes);
                state.controllers = std::move(request.controllers);
                state.pitchBends = std::move(request.pitchBends);
                state.programChanges = std::move(request.programChanges);
                state.channelPressures = std::move(request.channelPressures);
                state.polyPressures = std::move(request.polyPressures);
                state.config = std::move(request.config);
                state.outputGain = mw::audio::sanitizeMainUiGain(request.outputGain);
                if (state.config.trackIndex < 0)
                    state.config.trackIndex = request.trackIndex;
                if (state.config.trackName.empty())
                    state.config.trackName = request.trackName;
                state.config.outputChannelCount = requestedOutputChannels;

                state.effects.reserve(request.effects.size());
                for (const auto& effectRequest : request.effects)
                {
                    if (effectRequest.session == nullptr || effectRequest.sessionProcessMutex == nullptr)
                        continue;

                    VstEffectState effect;
                    effect.slotIndex = effectRequest.slotIndex;
                    effect.displayName = effectRequest.displayName;
                    effect.session = effectRequest.session;
                    effect.sessionMutex = effectRequest.sessionProcessMutex;
                    state.effects.push_back(std::move(effect));
                }
                vstTracks.push_back(std::move(state));
            }

            audioSources.reserve(audioSourceRequests.size());
            for (auto& request : audioSourceRequests)
            {
                const auto channels = request.sourceChannelCount;
                const bool exactLayout = channels > 0
                    && request.interleavedAudio.size() % static_cast<std::size_t>(channels) == 0;
                if (request.sourceSampleRate < kMinimumDeviceSampleRate
                    || request.sourceSampleRate > kMaximumDeviceSampleRate
                    || channels <= 0
                    || channels > kMaximumPluginChannels
                    || !exactLayout
                    || request.sourceFrameCount() <= 0)
                {
                    continue;
                }

                AudioSourceState source;
                source.displayName = std::move(request.displayName);
                source.sourceSampleRate = request.sourceSampleRate;
                source.sourceChannelCount = request.sourceChannelCount;
                source.startSample = std::max<std::int64_t>(0, request.startSample);
                source.interleavedAudio = std::move(request.interleavedAudio);
                source.outputGain = mw::audio::sanitizeMainUiGain(request.outputGain);
                source.sourceFrames = static_cast<std::int64_t>(source.interleavedAudio.size()
                    / static_cast<std::size_t>(source.sourceChannelCount));
                audioSources.push_back(std::move(source));
            }

            prepareAll(kFallbackSampleRate, 512);
        }

        void audioDeviceAboutToStart(juce::AudioIODevice* device) override
        {
            const auto rate = device != nullptr && device->getCurrentSampleRate() > 0.0
                ? device->getCurrentSampleRate()
                : static_cast<double>(kFallbackSampleRate);
            const auto block = device != nullptr && device->getCurrentBufferSizeSamples() > 0
                ? device->getCurrentBufferSizeSamples()
                : 512;

            const auto previousRate = std::max(1, sampleRate.load());
            const auto resumeSeconds = deviceHasStarted
                ? static_cast<double>(std::max<std::int64_t>(0, currentSample.load()))
                    / static_cast<double>(previousRate)
                : 0.0;

            if (!deviceHasStarted)
                resetStatistics();
            else
                finished.store(false);

            prepareAll(
                juce::jlimit(kMinimumDeviceSampleRate,
                             kMaximumDeviceSampleRate,
                             static_cast<int>(std::round(rate))),
                juce::jlimit(kMinimumBlockSize, kMaximumBlockSize, block),
                resumeSeconds);
            deviceHasStarted = true;
            finished.store(!prepared || failed.load());
        }

        void audioDeviceStopped() override
        {
            finished.store(true);
        }

        void audioDeviceIOCallback(const float* const* inputChannelData,
                                   int numInputChannels,
                                   float* const* outputChannelData,
                                   int numOutputChannels,
                                   int numSamples)
        {
            processAudio(inputChannelData, numInputChannels, outputChannelData, numOutputChannels, numSamples);
        }

        void audioDeviceIOCallbackWithContext(const float* const* inputChannelData,
                                              int numInputChannels,
                                              float* const* outputChannelData,
                                              int numOutputChannels,
                                              int numSamples,
                                              const juce::AudioIODeviceCallbackContext&) override
        {
            processAudio(inputChannelData, numInputChannels, outputChannelData, numOutputChannels, numSamples);
        }

        bool hasFailed() const noexcept { return failed.load(); }
        bool hasFinished() const noexcept { return finished.load(); }
        int getSampleRate() const noexcept { return sampleRate.load(); }
        int getBlockSize() const noexcept { return configuredBlockSize.load(); }
        int getTrackIndex() const noexcept { return primaryTrackIndex.load(); }
        int getTrackCount() const noexcept { return trackCount.load(); }
        bool hasLiveMidiInput() const noexcept
        {
            for (const auto& track : clapTracks)
                if (track.liveMidiInput)
                    return true;
            for (const auto& track : vstTracks)
                if (track.liveMidiInput)
                    return true;
            return false;
        }
        int getClapTrackCount() const noexcept { return clapTrackCount.load(); }
        int getVstTrackCount() const noexcept { return vstTrackCount.load(); }
        int getAudioSourceCount() const noexcept { return audioSourceCount.load(); }
        int getProcessedBlocks() const noexcept { return processedBlocks.load(); }
        int getSubmittedEvents() const noexcept { return submittedEvents.load(); }
        int getSkippedBlocks() const noexcept { return skippedBlocks.load(); }
        int getSkippedEffectBlocks() const noexcept { return skippedEffectBlocks.load(); }
        int getScheduledEvents() const noexcept { return scheduledEvents.load(); }
        int getMaxEventsPerBlock() const noexcept { return maxEventsPerBlock.load(); }
        int getLiveEffectSlotCount() const noexcept { return liveEffectSlotCount.load(); }
        int getProcessedEffectBlocks() const noexcept { return processedEffectBlocks.load(); }
        int getConsecutiveBusyBlockLimit() const noexcept { return kMaximumConsecutiveBusyBlocks; }
        int getEmergencyNoteOffsSubmitted() const noexcept { return emergencyNoteOffsSubmitted.load(); }
        int getStopFlushAttempted() const noexcept { return stopFlushAttempted.load(); }
        int getStopFlushSucceeded() const noexcept { return stopFlushSucceeded.load(); }
        int getStopFlushSkippedBusy() const noexcept { return stopFlushSkippedBusy.load(); }
        int getClippedSamples() const noexcept { return clippedSamples.load(); }
        std::int64_t getCurrentSample() const noexcept { return currentSample.load(); }
        std::int64_t getTotalSamples() const noexcept { return totalSamples.load(); }
        float getPeak() const noexcept { return outputPeak.load(); }
        juce::String getLastMessage() const
        {
            const juce::ScopedLock lock(messageLock);
            return lastMessage;
        }

        void setMidiRecordingMetronomeActive(bool active) noexcept
        {
            recordingMetronomeActive.store(active && recordingMetronome.enabled, std::memory_order_release);
        }

        bool submitLiveMidiMessage(mw::core::StableId trackId, const juce::MidiMessage& message) noexcept
        {
            auto submit = [&message, trackId](auto& tracks) noexcept
            {
                for (auto& track : tracks)
                {
                    if (!track.liveMidiInput || track.stableTrackId != trackId || track.liveMidiQueue == nullptr)
                        continue;
                    const bool queued = track.liveMidiQueue->push(message);
                    if (!queued && track.liveMidiPanicRequested != nullptr
                        && (message.isNoteOn() || message.isNoteOff()))
                    {
                        track.liveMidiPanicRequested->store(true, std::memory_order_release);
                    }
                    return queued;
                }
                return false;
            };

            if (submit(clapTracks))
                return true;
            return submit(vstTracks);
        }

        void requestLiveMidiPanic(mw::core::StableId trackId) noexcept
        {
            auto request = [trackId](auto& tracks) noexcept
            {
                for (auto& track : tracks)
                {
                    if (!track.liveMidiInput || track.stableTrackId != trackId || track.liveMidiPanicRequested == nullptr)
                        continue;
                    track.liveMidiPanicRequested->store(true, std::memory_order_release);
                    return true;
                }
                return false;
            };

            if (!request(clapTracks))
                request(vstTracks);
        }

        void flushActiveNotesForStop()
        {
            if (!prepared)
                return;

            for (auto& track : clapTracks)
                flushClapTrack(track);
            for (auto& track : vstTracks)
                flushVstTrack(track);
        }

    private:
        struct ClapEffectState
        {
            int slotIndex = -1;
            std::string displayName;
            mw::clap::ClapLiveEffectSession* session = nullptr;
            std::mutex* sessionMutex = nullptr;
        };

        struct ClapTrackState
        {
            mw::core::StableId stableTrackId = 0;
            bool liveMidiInput = false;
            std::unique_ptr<mw::midi::LiveMidiNoteQueue> liveMidiQueue;
            std::unique_ptr<std::atomic<bool>> liveMidiPanicRequested;
            mw::clap::ClapLiveProcessRequest combinedRequest;
            mw::clap::ClapLiveInstrumentSession* session = nullptr;
            std::mutex* sessionMutex = nullptr;
            std::vector<mw::clap::ClapLivePlaybackNote> notes;
            std::vector<mw::clap::ClapLivePlaybackControllerEvent> controllers;
            std::vector<mw::clap::ClapLivePlaybackPitchBendEvent> pitchBends;
            std::vector<mw::clap::ClapLivePlaybackProgramChangeEvent> programChanges;
            std::vector<mw::clap::ClapLivePlaybackChannelPressureEvent> channelPressures;
            std::vector<mw::clap::ClapLivePlaybackPolyPressureEvent> polyPressures;
            mw::clap::ClapLiveCallbackBridgeConfig config;
            mw::clap::ClapLiveCallbackBridge bridge;
            std::vector<ClapEffectState> effects;
            float outputGain = 1.0f;
            std::array<std::array<int, 128>, 16> activeNoteDepths {};
            int consecutiveSkippedBlocks = 0;
            bool prepared = false;
            bool finished = false;
            bool failed = false;
        };

        struct VstEffectState
        {
            int slotIndex = -1;
            std::string displayName;
            mw::vst::VstLiveEffectSession* session = nullptr;
            std::mutex* sessionMutex = nullptr;
        };

        struct VstTrackState
        {
            mw::core::StableId stableTrackId = 0;
            bool liveMidiInput = false;
            std::unique_ptr<mw::midi::LiveMidiNoteQueue> liveMidiQueue;
            std::unique_ptr<std::atomic<bool>> liveMidiPanicRequested;
            mw::clap::ClapLiveProcessRequest combinedRequest;
            mw::vst::VstLiveInstrumentSession* session = nullptr;
            std::mutex* sessionMutex = nullptr;
            std::vector<mw::clap::ClapLivePlaybackNote> notes;
            std::vector<mw::clap::ClapLivePlaybackControllerEvent> controllers;
            std::vector<mw::clap::ClapLivePlaybackPitchBendEvent> pitchBends;
            std::vector<mw::clap::ClapLivePlaybackProgramChangeEvent> programChanges;
            std::vector<mw::clap::ClapLivePlaybackChannelPressureEvent> channelPressures;
            std::vector<mw::clap::ClapLivePlaybackPolyPressureEvent> polyPressures;
            mw::clap::ClapLiveCallbackBridgeConfig config;
            mw::clap::ClapLiveCallbackBridge bridge;
            std::vector<VstEffectState> effects;
            juce::AudioBuffer<float> processBuffer;
            juce::MidiBuffer midi;
            float outputGain = 1.0f;
            std::array<std::array<int, 128>, 16> activeNoteDepths {};
            int consecutiveSkippedBlocks = 0;
            bool prepared = false;
            bool finished = false;
            bool failed = false;
        };

        struct AudioSourceState
        {
            std::string displayName;
            int sourceSampleRate = kFallbackSampleRate;
            int sourceChannelCount = 2;
            std::int64_t startSample = 0;
            std::int64_t sourceFrames = 0;
            std::vector<float> interleavedAudio;
            float outputGain = 1.0f;
            bool finished = false;
        };

        void resetStatistics()
        {
            processedBlocks.store(0);
            submittedEvents.store(0);
            skippedBlocks.store(0);
            skippedEffectBlocks.store(0);
            processedEffectBlocks.store(0);
            emergencyNoteOffsSubmitted.store(0);
            stopFlushAttempted.store(0);
            stopFlushSucceeded.store(0);
            stopFlushSkippedBusy.store(0);
            clippedSamples.store(0);
            outputPeak.store(0.0f);
            failed.store(false);
            finished.store(false);
            {
                const juce::ScopedLock lock(messageLock);
                lastMessage.clear();
            }
        }

        template <typename Track>
        static void clearActiveNotes(Track& track)
        {
            for (auto& channel : track.activeNoteDepths)
                channel.fill(0);
        }

        template <typename Track>
        static void updateActiveNotes(Track& track, const mw::clap::ClapLiveProcessRequest& request)
        {
            for (const auto& event : request.noteEvents)
            {
                const auto channel = noteChannelIndex(event.midiChannel);
                const auto key = noteKeyIndex(event.key);
                auto& depth = track.activeNoteDepths[static_cast<std::size_t>(channel)][static_cast<std::size_t>(key)];
                if (event.type == mw::clap::ClapLiveNoteEventType::NoteOn && event.velocity > 0)
                    depth = std::min(127, depth + 1);
                else if (depth > 0)
                    --depth;
            }
        }

        void setFailure(juce::String message)
        {
            failed.store(true);
            finished.store(true);
            const juce::ScopedLock lock(messageLock);
            lastMessage = std::move(message);
        }

        bool prepareClapTrack(ClapTrackState& track,
                              int deviceSampleRate,
                              int deviceBlockSize,
                              double resumeSeconds)
        {
            track.prepared = false;
            track.finished = false;
            track.failed = false;
            track.consecutiveSkippedBlocks = 0;
            clearActiveNotes(track);

            if (track.session == nullptr || track.sessionMutex == nullptr)
                return false;

            std::string error;
            {
                const std::lock_guard<std::mutex> lock(*track.sessionMutex);
                if (!track.session->prepareForPlayback(
                        deviceSampleRate,
                        deviceBlockSize,
                        requestedOutputChannels,
                        error))
                {
                    setFailure("Mixed project CLAP instrument preparation failed: " + juce::String(error));
                    track.failed = true;
                    return false;
                }
                if (track.session->info().latencySamples > 0)
                {
                    setFailure("Mixed project CLAP instrument reported non-zero latency after device preparation; rendered fallback is required.");
                    track.failed = true;
                    return false;
                }
            }

            for (auto& effect : track.effects)
            {
                if (effect.session == nullptr || effect.sessionMutex == nullptr)
                    continue;
                const std::lock_guard<std::mutex> effectLock(*effect.sessionMutex);
                if (!effect.session->prepareForPlayback(
                        deviceSampleRate,
                        deviceBlockSize,
                        requestedOutputChannels,
                        error))
                {
                    setFailure("Mixed project CLAP effect preparation failed: " + juce::String(error));
                    track.failed = true;
                    return false;
                }
                if (effect.session->info().latencySamples > 0)
                {
                    setFailure("Mixed project CLAP effect reported non-zero latency after device preparation; rendered fallback is required.");
                    track.failed = true;
                    return false;
                }
            }

            auto bridgeConfig = track.config;
            bridgeConfig.scheduler.sampleRate = deviceSampleRate;
            bridgeConfig.scheduler.blockSize = deviceBlockSize;
            bridgeConfig.scheduler.startTick += static_cast<std::int64_t>(std::llround(
                std::max(0.0, resumeSeconds)
                * static_cast<double>(std::max(1, bridgeConfig.scheduler.tempoBpm))
                * static_cast<double>(std::max<std::int64_t>(1, bridgeConfig.scheduler.ticksPerQuarterNote)) / 60.0));
            bridgeConfig.scheduler.startSample = 0;
            bridgeConfig.outputChannelCount = requestedOutputChannels;
            track.bridge.prepare(track.notes, track.controllers, track.pitchBends, track.programChanges, track.channelPressures, track.polyPressures, bridgeConfig);
            if (track.liveMidiInput)
            {
                const auto bridgeState = track.bridge.state();
                const auto requiredCapacity = static_cast<std::size_t>(
                    std::max(0, bridgeState.maxEventsPerBlock)
                    + kMaximumQueuedLiveMidiEventsPerBlock
                    + kMaximumPanicNoteOffEvents);
                if (track.combinedRequest.noteEvents.capacity() < requiredCapacity)
                    track.combinedRequest.noteEvents.reserve(requiredCapacity);
                if (track.combinedRequest.controllerEvents.capacity() < requiredCapacity)
                    track.combinedRequest.controllerEvents.reserve(requiredCapacity);
                if (track.combinedRequest.pitchBendEvents.capacity() < requiredCapacity)
                    track.combinedRequest.pitchBendEvents.reserve(requiredCapacity);
                if (track.combinedRequest.programChangeEvents.capacity() < requiredCapacity)
                    track.combinedRequest.programChangeEvents.reserve(requiredCapacity);
                if (track.combinedRequest.channelPressureEvents.capacity() < requiredCapacity)
                    track.combinedRequest.channelPressureEvents.reserve(requiredCapacity);
            }
            track.prepared = track.liveMidiInput || track.bridge.isPrepared();
            track.finished = !track.prepared || (!track.liveMidiInput && track.bridge.isFinished());
            return track.prepared;
        }

        bool prepareVstTrack(VstTrackState& track,
                             int deviceSampleRate,
                             int deviceBlockSize,
                             double resumeSeconds)
        {
            track.prepared = false;
            track.finished = false;
            track.failed = false;
            track.consecutiveSkippedBlocks = 0;
            clearActiveNotes(track);

            if (track.session == nullptr || track.sessionMutex == nullptr)
                return false;

            std::string error;
            {
                const std::lock_guard<std::mutex> lock(*track.sessionMutex);
                if (!track.session->prepareForPlayback(
                        static_cast<double>(deviceSampleRate),
                        deviceBlockSize,
                        requestedOutputChannels,
                        error))
                {
                    setFailure("Mixed project VST3 instrument preparation failed: " + juce::String(error));
                    track.failed = true;
                    return false;
                }
                if (track.session->info().latencySamples > 0)
                {
                    setFailure("Mixed project VST3 instrument reported non-zero latency after device preparation; rendered fallback is required.");
                    track.failed = true;
                    return false;
                }
            }

            for (auto& effect : track.effects)
            {
                if (effect.session == nullptr || effect.sessionMutex == nullptr)
                    continue;
                const std::lock_guard<std::mutex> effectLock(*effect.sessionMutex);
                if (!effect.session->prepareForPlayback(
                        static_cast<double>(deviceSampleRate),
                        deviceBlockSize,
                        requestedOutputChannels,
                        error))
                {
                    setFailure("Mixed project VST3 effect preparation failed: " + juce::String(error));
                    track.failed = true;
                    return false;
                }
                if (effect.session->info().latencySamples > 0)
                {
                    setFailure("Mixed project VST3 effect reported non-zero latency after device preparation; rendered fallback is required.");
                    track.failed = true;
                    return false;
                }
            }

            auto bridgeConfig = track.config;
            bridgeConfig.scheduler.sampleRate = deviceSampleRate;
            bridgeConfig.scheduler.blockSize = deviceBlockSize;
            bridgeConfig.scheduler.startTick += static_cast<std::int64_t>(std::llround(
                std::max(0.0, resumeSeconds)
                * static_cast<double>(std::max(1, bridgeConfig.scheduler.tempoBpm))
                * static_cast<double>(std::max<std::int64_t>(1, bridgeConfig.scheduler.ticksPerQuarterNote)) / 60.0));
            bridgeConfig.scheduler.startSample = 0;
            bridgeConfig.outputChannelCount = requestedOutputChannels;
            track.bridge.prepare(track.notes, track.controllers, track.pitchBends, track.programChanges, track.channelPressures, track.polyPressures, bridgeConfig);
            if (track.liveMidiInput)
            {
                const auto bridgeState = track.bridge.state();
                const auto requiredCapacity = static_cast<std::size_t>(
                    std::max(0, bridgeState.maxEventsPerBlock)
                    + kMaximumQueuedLiveMidiEventsPerBlock
                    + kMaximumPanicNoteOffEvents);
                if (track.combinedRequest.noteEvents.capacity() < requiredCapacity)
                    track.combinedRequest.noteEvents.reserve(requiredCapacity);
                if (track.combinedRequest.controllerEvents.capacity() < requiredCapacity)
                    track.combinedRequest.controllerEvents.reserve(requiredCapacity);
                if (track.combinedRequest.pitchBendEvents.capacity() < requiredCapacity)
                    track.combinedRequest.pitchBendEvents.reserve(requiredCapacity);
                if (track.combinedRequest.programChangeEvents.capacity() < requiredCapacity)
                    track.combinedRequest.programChangeEvents.reserve(requiredCapacity);
                if (track.combinedRequest.channelPressureEvents.capacity() < requiredCapacity)
                    track.combinedRequest.channelPressureEvents.reserve(requiredCapacity);
            }
            track.processBuffer.setSize(
                requestedOutputChannels,
                kMaximumBlockSize,
                false,
                true,
                false);
            track.processBuffer.clear();
            track.prepared = track.liveMidiInput || track.bridge.isPrepared();
            track.finished = !track.prepared || (!track.liveMidiInput && track.bridge.isFinished());
            return track.prepared;
        }

        void prepareAll(int deviceSampleRate, int deviceBlockSize, double resumeSeconds = 0.0)
        {
            configuredBlockSize.store(deviceBlockSize);
            const auto timelineStartSample = static_cast<std::int64_t>(std::llround(
                std::max(0.0, resumeSeconds) * static_cast<double>(deviceSampleRate)));
            int preparedClap = 0;
            int preparedVst = 0;
            int preparedSources = 0;
            int aggregateScheduled = 0;
            int aggregateMaxEvents = 0;
            int aggregateEffects = 0;
            int firstTrack = -1;
            std::int64_t aggregateTotal = 0;

            for (auto& track : clapTracks)
            {
                if (!prepareClapTrack(track, deviceSampleRate, deviceBlockSize, resumeSeconds))
                    continue;
                const auto state = track.bridge.state();
                ++preparedClap;
                if (firstTrack < 0)
                    firstTrack = state.trackIndex;
                aggregateScheduled += state.totalScheduledEvents;
                aggregateMaxEvents += state.maxEventsPerBlock;
                aggregateEffects += static_cast<int>(track.effects.size());
                aggregateTotal = std::max(aggregateTotal, timelineStartSample + state.totalSamples);
            }

            for (auto& track : vstTracks)
            {
                if (!prepareVstTrack(track, deviceSampleRate, deviceBlockSize, resumeSeconds))
                    continue;
                const auto state = track.bridge.state();
                ++preparedVst;
                if (firstTrack < 0)
                    firstTrack = state.trackIndex;
                aggregateScheduled += state.totalScheduledEvents;
                aggregateMaxEvents += state.maxEventsPerBlock;
                aggregateEffects += static_cast<int>(track.effects.size());
                aggregateTotal = std::max(aggregateTotal, timelineStartSample + state.totalSamples);
            }

            for (auto& source : audioSources)
            {
                const auto durationAtDeviceRate = static_cast<std::int64_t>(std::ceil(
                    static_cast<double>(source.sourceFrames)
                    * static_cast<double>(deviceSampleRate)
                    / static_cast<double>(std::max(1, source.sourceSampleRate))));
                const auto sourceEndSample = source.startSample + durationAtDeviceRate;
                source.finished = source.sourceFrames <= 0 || timelineStartSample >= sourceEndSample;
                if (source.finished)
                    continue;
                ++preparedSources;
                aggregateTotal = std::max(aggregateTotal, sourceEndSample);
            }

            prepared = !failed.load() && (preparedClap + preparedVst + preparedSources > 0);
            sampleRate.store(deviceSampleRate);
            primaryTrackIndex.store(firstTrack);
            clapTrackCount.store(preparedClap);
            vstTrackCount.store(preparedVst);
            trackCount.store(preparedClap + preparedVst);
            audioSourceCount.store(preparedSources);
            scheduledEvents.store(aggregateScheduled);
            maxEventsPerBlock.store(aggregateMaxEvents);
            liveEffectSlotCount.store(aggregateEffects);
            currentSample.store(timelineStartSample);
            totalSamples.store(aggregateTotal);
            finished.store(!prepared || failed.load());
        }

        bool flushClapTrack(ClapTrackState& track)
        {
            if (!track.prepared || track.session == nullptr || track.sessionMutex == nullptr)
                return false;

            mw::clap::ClapLiveProcessRequest release;
            release.frameCount = std::max(1, std::min(configuredBlockSize.load(), 512));
            for (int channel = 0; channel < 16; ++channel)
            {
                for (int key = 0; key < 128; ++key)
                {
                    const auto depth = track.activeNoteDepths[static_cast<std::size_t>(channel)][static_cast<std::size_t>(key)];
                    for (int duplicate = 0; duplicate < depth; ++duplicate)
                    {
                        mw::clap::ClapLiveNoteEvent event;
                        event.type = mw::clap::ClapLiveNoteEventType::NoteOff;
                        event.frameOffset = 0;
                        event.key = key;
                        event.velocity = 0;
                        event.midiChannel = channel + 1;
                        release.noteEvents.push_back(event);
                    }
                }
            }
            if (release.noteEvents.empty())
                return false;

            stopFlushAttempted.fetch_add(1);
            std::unique_lock<std::mutex> lock(*track.sessionMutex, std::try_to_lock);
            if (!lock.owns_lock())
            {
                stopFlushSkippedBusy.fetch_add(1);
                return false;
            }

            const auto result = track.session->processBlock(release);
            if (!result.success)
            {
                setFailure("Mixed project CLAP stop note-off flush failed: " + juce::String(result.message));
                return false;
            }

            emergencyNoteOffsSubmitted.fetch_add(static_cast<int>(release.noteEvents.size()));
            stopFlushSucceeded.fetch_add(1);
            clearActiveNotes(track);
            return true;
        }

        bool flushVstTrack(VstTrackState& track)
        {
            if (!track.prepared || track.session == nullptr || track.sessionMutex == nullptr)
                return false;

            juce::MidiBuffer midi;
            int noteOffCount = 0;
            for (int channel = 0; channel < 16; ++channel)
            {
                for (int key = 0; key < 128; ++key)
                {
                    const auto depth = track.activeNoteDepths[static_cast<std::size_t>(channel)][static_cast<std::size_t>(key)];
                    for (int duplicate = 0; duplicate < depth; ++duplicate)
                    {
                        midi.addEvent(juce::MidiMessage::noteOff(channel + 1, key), 0);
                        ++noteOffCount;
                    }
                }
                midi.addEvent(juce::MidiMessage::allNotesOff(channel + 1), 0);
            }
            if (noteOffCount <= 0)
                return false;

            stopFlushAttempted.fetch_add(1);
            std::unique_lock<std::mutex> lock(*track.sessionMutex, std::try_to_lock);
            if (!lock.owns_lock())
            {
                stopFlushSkippedBusy.fetch_add(1);
                return false;
            }

            track.processBuffer.setSize(
                requestedOutputChannels,
                std::max(1, std::min(configuredBlockSize.load(), 512)),
                false,
                false,
                true);
            track.processBuffer.clear();
            std::string error;
            if (!track.session->processBlock(track.processBuffer, midi, error))
            {
                setFailure("Mixed project VST3 stop note-off flush failed: " + juce::String(error));
                return false;
            }

            emergencyNoteOffsSubmitted.fetch_add(noteOffCount);
            stopFlushSucceeded.fetch_add(1);
            clearActiveNotes(track);
            return true;
        }

        void clearOutputs(float* const* outputChannelData, int numOutputChannels, int numSamples) const
        {
            for (int channel = 0; channel < numOutputChannels; ++channel)
                if (outputChannelData[channel] != nullptr)
                    juce::FloatVectorOperations::clear(outputChannelData[channel], numSamples);
        }

        void mixOutputSample(float* const* outputChannelData,
                             int channel,
                             int frame,
                             float sample,
                             float& localPeak)
        {
            if (channel < 0 || outputChannelData[channel] == nullptr)
                return;
            if (!std::isfinite(sample))
                sample = 0.0f;

            auto mixed = outputChannelData[channel][frame] + sample;
            if (mixed > 1.0f)
            {
                mixed = 1.0f;
                clippedSamples.fetch_add(1);
            }
            else if (mixed < -1.0f)
            {
                mixed = -1.0f;
                clippedSamples.fetch_add(1);
            }
            outputChannelData[channel][frame] = mixed;
            localPeak = std::max(localPeak, std::abs(mixed));
        }

        bool processClapEffects(ClapTrackState& track,
                                std::vector<float>& interleaved,
                                int& channelCount,
                                int frames)
        {
            for (auto& effect : track.effects)
            {
                if (effect.session == nullptr || effect.sessionMutex == nullptr)
                    continue;

                std::unique_lock<std::mutex> lock(*effect.sessionMutex, std::try_to_lock);
                if (!lock.owns_lock())
                {
                    skippedEffectBlocks.fetch_add(1);
                    continue;
                }

                mw::clap::ClapLiveEffectProcessRequest request;
                request.frameCount = frames;
                request.inputChannelCount = channelCount;
                request.interleavedInputAudio = interleaved;
                auto result = effect.session->processBlock(request);
                if (!result.success)
                {
                    setFailure("Mixed project CLAP effect failed: " + juce::String(result.message));
                    return false;
                }
                if (!hasValidPluginOutputBuffer(
                        frames,
                        result.outputFrameCount,
                        result.outputChannelCount,
                        result.interleavedAudio.size()))
                {
                    setFailure("Mixed project CLAP effect returned an invalid audio buffer.");
                    return false;
                }

                interleaved = std::move(result.interleavedAudio);
                channelCount = result.outputChannelCount;
                processedEffectBlocks.fetch_add(1);
            }
            return true;
        }

        bool processVstEffects(VstTrackState& track, std::string& error)
        {
            for (auto& effect : track.effects)
            {
                if (effect.session == nullptr || effect.sessionMutex == nullptr)
                    continue;

                std::unique_lock<std::mutex> lock(*effect.sessionMutex, std::try_to_lock);
                if (!lock.owns_lock())
                {
                    skippedEffectBlocks.fetch_add(1);
                    continue;
                }

                if (!effect.session->processBlock(track.processBuffer, error))
                    return false;
                processedEffectBlocks.fetch_add(1);
            }
            return true;
        }

        template <typename TrackState>
        const mw::clap::ClapLiveProcessRequest* nextTrackRequest(
            TrackState& track,
            int numSamples,
            bool& panicThisBlock,
            int& panicNoteOffCount)
        {
            panicThisBlock = false;
            panicNoteOffCount = 0;

            if (!track.liveMidiInput)
            {
                if (track.bridge.isFinished())
                    return nullptr;
                return &track.bridge.nextRequest();
            }

            auto& combined = track.combinedRequest;
            combined.frameCount = numSamples;
            combined.noteEvents.clear();
            combined.controllerEvents.clear();
            combined.pitchBendEvents.clear();
            combined.programChangeEvents.clear();
            combined.channelPressureEvents.clear();

            if (!track.bridge.isFinished())
            {
                const auto& scheduled = track.bridge.nextRequest();
                if (scheduled.frameCount > numSamples)
                    return nullptr;
                combined.controllerEvents.insert(
                    combined.controllerEvents.end(),
                    scheduled.controllerEvents.begin(),
                    scheduled.controllerEvents.end());
                combined.noteEvents.insert(
                    combined.noteEvents.end(),
                    scheduled.noteEvents.begin(),
                    scheduled.noteEvents.end());
                combined.programChangeEvents.insert(
                    combined.programChangeEvents.end(),
                    scheduled.programChangeEvents.begin(),
                    scheduled.programChangeEvents.end());
                combined.pitchBendEvents.insert(
                    combined.pitchBendEvents.end(),
                    scheduled.pitchBendEvents.begin(),
                    scheduled.pitchBendEvents.end());
                combined.channelPressureEvents.insert(
                    combined.channelPressureEvents.end(),
                    scheduled.channelPressureEvents.begin(),
                    scheduled.channelPressureEvents.end());
            }

            if (track.liveMidiQueue != nullptr)
            {
                std::array<mw::midi::LiveMidiNoteEvent, kMaximumQueuedLiveMidiEventsPerBlock> queuedEvents {};
                const int eventCount = track.liveMidiQueue->pop(
                    queuedEvents.data(),
                    static_cast<int>(queuedEvents.size()));
                for (int index = 0; index < eventCount; ++index)
                {
                    const auto& queued = queuedEvents[static_cast<std::size_t>(index)];
                    if (queued.type == mw::midi::LiveMidiEventType::Controller)
                    {
                        mw::clap::ClapLiveControllerEvent event;
                        event.frameOffset = 0; event.midiChannel = queued.channel;
                        event.controller = queued.controller; event.value = queued.controllerValue;
                        combined.controllerEvents.push_back(event);
                    }
                    else if (queued.type == mw::midi::LiveMidiEventType::PitchBend)
                    {
                        mw::clap::ClapLivePitchBendEvent event;
                        event.frameOffset = 0; event.midiChannel = queued.channel; event.value = queued.pitchBendValue;
                        combined.pitchBendEvents.push_back(event);
                    }
                    else if (queued.type == mw::midi::LiveMidiEventType::ProgramChange)
                    {
                        mw::clap::ClapLiveProgramChangeEvent event;
                        event.frameOffset = 0; event.midiChannel = queued.channel; event.program = queued.program;
                        combined.programChangeEvents.push_back(event);
                    }
                    else if (queued.type == mw::midi::LiveMidiEventType::ChannelPressure)
                    {
                        mw::clap::ClapLiveChannelPressureEvent event;
                        event.frameOffset = 0; event.midiChannel = queued.channel; event.pressure = queued.channelPressure;
                        combined.channelPressureEvents.push_back(event);
                    }
                    else
                    {
                        mw::clap::ClapLiveNoteEvent event;
                        event.type = queued.type == mw::midi::LiveMidiEventType::NoteOn
                            ? mw::clap::ClapLiveNoteEventType::NoteOn
                            : mw::clap::ClapLiveNoteEventType::NoteOff;
                        event.frameOffset = 0; event.key = queued.key; event.velocity = queued.velocity; event.midiChannel = queued.channel;
                        combined.noteEvents.push_back(event);
                    }
                }
            }

            panicThisBlock = track.liveMidiPanicRequested != nullptr
                && track.liveMidiPanicRequested->exchange(false, std::memory_order_acq_rel);
            if (panicThisBlock)
            {
                for (int channel = 0; channel < 16; ++channel)
                {
                    for (int key = 0; key < 128; ++key)
                    {
                        const auto depth = track.activeNoteDepths[static_cast<std::size_t>(channel)][static_cast<std::size_t>(key)];
                        if (depth <= 0)
                            continue;

                        for (int duplicate = 0; duplicate < depth; ++duplicate)
                        {
                            mw::clap::ClapLiveNoteEvent event;
                            event.type = mw::clap::ClapLiveNoteEventType::NoteOff;
                            event.frameOffset = 0;
                            event.key = key;
                            event.velocity = 0;
                            event.midiChannel = channel + 1;
                            combined.noteEvents.push_back(event);
                            ++panicNoteOffCount;
                        }
                    }
                }
            }

            return &combined;
        }

        bool processClapTrack(ClapTrackState& track,
                              float* const* outputChannelData,
                              int numOutputChannels,
                              int numSamples,
                              int& eventsThisCallback,
                              float& localPeak)
        {
            if (!track.prepared || track.failed || track.finished
                || (!track.liveMidiInput && track.bridge.isFinished()))
            {
                if (!track.liveMidiInput && track.bridge.isFinished())
                    track.finished = true;
                return false;
            }

            std::unique_lock<std::mutex> lock(*track.sessionMutex, std::try_to_lock);
            if (!lock.owns_lock())
            {
                skippedBlocks.fetch_add(1);
                ++track.consecutiveSkippedBlocks;
                if (track.consecutiveSkippedBlocks >= kMaximumConsecutiveBusyBlocks)
                    setFailure("Mixed project CLAP instrument stayed busy for too many consecutive audio blocks.");
                return false;
            }
            track.consecutiveSkippedBlocks = 0;

            bool panicThisBlock = false;
            int panicNoteOffCount = 0;
            const auto* requestPointer = nextTrackRequest(track, numSamples, panicThisBlock, panicNoteOffCount);
            if (requestPointer == nullptr || requestPointer->frameCount <= 0)
            {
                if (!track.liveMidiInput)
                    track.finished = true;
                return false;
            }
            const auto& request = *requestPointer;
            if (request.frameCount > numSamples)
            {
                setFailure("Mixed project CLAP scheduler block exceeded the device callback block.");
                track.failed = true;
                return false;
            }

            auto result = track.session->processBlock(request);
            if (!result.success)
            {
                setFailure("Mixed project CLAP instrument failed: " + juce::String(result.message));
                track.failed = true;
                return false;
            }
            updateActiveNotes(track, request);
            if (panicThisBlock)
            {
                emergencyNoteOffsSubmitted.fetch_add(panicNoteOffCount);
                clearActiveNotes(track);
            }

            auto audio = std::move(result.interleavedAudio);
            auto channels = result.outputChannelCount;
            if (!hasValidPluginOutputBuffer(
                    request.frameCount,
                    result.outputFrameCount,
                    channels,
                    audio.size()))
            {
                setFailure("Mixed project CLAP instrument returned an invalid audio buffer.");
                track.failed = true;
                return false;
            }

            if (!processClapEffects(track, audio, channels, request.frameCount))
            {
                track.failed = true;
                return false;
            }

            for (int frame = 0; frame < request.frameCount; ++frame)
            {
                for (int channel = 0; channel < numOutputChannels; ++channel)
                {
                    float sample = 0.0f;
                    if (channel < channels)
                    {
                        const auto index = static_cast<std::size_t>(frame * channels + channel);
                        if (index < audio.size())
                            sample = audio[index] * track.outputGain;
                    }
                    mixOutputSample(outputChannelData, channel, frame, sample, localPeak);
                }
            }

            eventsThisCallback += static_cast<int>(request.noteEvents.size() + request.controllerEvents.size() + request.programChangeEvents.size() + request.pitchBendEvents.size() + request.channelPressureEvents.size());
            if (!track.liveMidiInput && track.bridge.isFinished())
                track.finished = true;
            return true;
        }

        bool processVstTrack(VstTrackState& track,
                             float* const* outputChannelData,
                             int numOutputChannels,
                             int numSamples,
                             int& eventsThisCallback,
                             float& localPeak)
        {
            if (!track.prepared || track.failed || track.finished
                || (!track.liveMidiInput && track.bridge.isFinished()))
            {
                if (!track.liveMidiInput && track.bridge.isFinished())
                    track.finished = true;
                return false;
            }

            std::unique_lock<std::mutex> lock(*track.sessionMutex, std::try_to_lock);
            if (!lock.owns_lock())
            {
                skippedBlocks.fetch_add(1);
                ++track.consecutiveSkippedBlocks;
                if (track.consecutiveSkippedBlocks >= kMaximumConsecutiveBusyBlocks)
                    setFailure("Mixed project VST3 instrument stayed busy for too many consecutive audio blocks.");
                return false;
            }
            track.consecutiveSkippedBlocks = 0;

            bool panicThisBlock = false;
            int panicNoteOffCount = 0;
            const auto* requestPointer = nextTrackRequest(track, numSamples, panicThisBlock, panicNoteOffCount);
            if (requestPointer == nullptr || requestPointer->frameCount <= 0)
            {
                if (!track.liveMidiInput)
                    track.finished = true;
                return false;
            }
            const auto& request = *requestPointer;
            if (request.frameCount > numSamples || request.frameCount > kMaximumBlockSize)
            {
                setFailure("Mixed project VST3 scheduler block exceeded the device callback block.");
                track.failed = true;
                return false;
            }

            track.processBuffer.setSize(
                requestedOutputChannels,
                request.frameCount,
                false,
                false,
                true);
            track.processBuffer.clear();
            track.midi.clear();
            for (const auto& event : request.controllerEvents)
            {
                const auto channel = std::clamp(event.midiChannel, 1, 16);
                const auto controller = std::clamp(event.controller, 0, 127);
                const auto value = std::clamp(event.value, 0, 127);
                const auto offset = std::clamp(static_cast<int>(event.frameOffset), 0, request.frameCount - 1);
                track.midi.addEvent(juce::MidiMessage::controllerEvent(channel, controller, value), offset);
            }
            for (const auto& event : request.programChangeEvents)
            {
                const auto channel = std::clamp(event.midiChannel, 1, 16);
                const auto program = std::clamp(event.program, 0, 127);
                const auto offset = std::clamp(static_cast<int>(event.frameOffset), 0, request.frameCount - 1);
                track.midi.addEvent(juce::MidiMessage::programChange(channel, program), offset);
            }
            for (const auto& event : request.pitchBendEvents)
            {
                const auto channel = std::clamp(event.midiChannel, 1, 16);
                const auto raw = std::clamp(event.value + 8192, 0, 16383);
                const auto offset = std::clamp(static_cast<int>(event.frameOffset), 0, request.frameCount - 1);
                track.midi.addEvent(juce::MidiMessage::pitchWheel(channel, raw), offset);
            }
            for (const auto& event : request.channelPressureEvents)
            {
                const auto channel = std::clamp(event.midiChannel, 1, 16);
                const auto pressure = std::clamp(event.pressure, 0, 127);
                const auto offset = std::clamp(static_cast<int>(event.frameOffset), 0, request.frameCount - 1);
                track.midi.addEvent(juce::MidiMessage::channelPressureChange(channel, pressure), offset);
            }
            for (const auto& event : request.noteEvents)
            {
                const auto channel = std::clamp(event.midiChannel, 1, 16);
                const auto key = std::clamp(event.key, 0, 127);
                const auto offset = std::clamp(static_cast<int>(event.frameOffset), 0, request.frameCount - 1);
                if (event.type == mw::clap::ClapLiveNoteEventType::NoteOn && event.velocity > 0)
                {
                    track.midi.addEvent(
                        juce::MidiMessage::noteOn(
                            channel,
                            key,
                            static_cast<juce::uint8>(std::clamp(event.velocity, 1, 127))),
                        offset);
                }
                else
                {
                    track.midi.addEvent(juce::MidiMessage::noteOff(channel, key), offset);
                }
            }

            std::string error;
            if (!track.session->processBlock(track.processBuffer, track.midi, error))
            {
                setFailure("Mixed project VST3 instrument failed: " + juce::String(error));
                track.failed = true;
                return false;
            }
            updateActiveNotes(track, request);
            if (panicThisBlock)
            {
                emergencyNoteOffsSubmitted.fetch_add(panicNoteOffCount);
                clearActiveNotes(track);
            }

            if (!processVstEffects(track, error))
            {
                setFailure("Mixed project VST3 effect failed: " + juce::String(error));
                track.failed = true;
                return false;
            }

            const auto channelsToCopy = std::min({
                numOutputChannels,
                track.processBuffer.getNumChannels(),
                requestedOutputChannels,
                kMaximumPluginChannels });
            for (int frame = 0; frame < request.frameCount; ++frame)
            {
                for (int channel = 0; channel < channelsToCopy; ++channel)
                {
                    const auto sample = track.processBuffer.getSample(channel, frame) * track.outputGain;
                    mixOutputSample(outputChannelData, channel, frame, sample, localPeak);
                }
            }

            eventsThisCallback += static_cast<int>(request.noteEvents.size() + request.controllerEvents.size() + request.programChangeEvents.size() + request.pitchBendEvents.size() + request.channelPressureEvents.size());
            if (!track.liveMidiInput && track.bridge.isFinished())
                track.finished = true;
            return true;
        }

        bool mixAudioSources(float* const* outputChannelData,
                             int numOutputChannels,
                             int numSamples,
                             std::int64_t blockStartSample,
                             float& localPeak)
        {
            bool processed = false;
            const auto deviceRate = std::max(1, sampleRate.load());
            for (auto& source : audioSources)
            {
                if (source.finished || source.sourceFrames <= 0)
                    continue;

                for (int frame = 0; frame < numSamples; ++frame)
                {
                    const auto timelineSample = blockStartSample + frame;
                    if (timelineSample < source.startSample)
                        continue;

                    const auto sourcePosition = static_cast<double>(timelineSample - source.startSample)
                        * static_cast<double>(source.sourceSampleRate)
                        / static_cast<double>(deviceRate);
                    const auto frame0 = static_cast<std::int64_t>(std::floor(sourcePosition));
                    if (frame0 >= source.sourceFrames)
                    {
                        source.finished = true;
                        break;
                    }
                    const auto frame1 = std::min(source.sourceFrames - 1, frame0 + 1);
                    const auto fraction = static_cast<float>(sourcePosition - static_cast<double>(frame0));

                    for (int channel = 0; channel < numOutputChannels; ++channel)
                    {
                        const auto sourceChannel = std::min(channel, source.sourceChannelCount - 1);
                        const auto index0 = static_cast<std::size_t>(frame0 * source.sourceChannelCount + sourceChannel);
                        const auto index1 = static_cast<std::size_t>(frame1 * source.sourceChannelCount + sourceChannel);
                        if (index0 >= source.interleavedAudio.size() || index1 >= source.interleavedAudio.size())
                        {
                            setFailure("Mixed project rendered source had invalid buffer dimensions.");
                            return false;
                        }

                        const auto sample0 = source.interleavedAudio[index0];
                        const auto sample1 = source.interleavedAudio[index1];
                        const auto sample = (sample0 + ((sample1 - sample0) * fraction)) * source.outputGain;
                        mixOutputSample(outputChannelData, channel, frame, sample, localPeak);
                    }
                    processed = true;
                }
            }
            return processed;
        }

        bool mixMidiRecordingMetronome(float* const* outputChannelData,
                                          int numOutputChannels,
                                          int numSamples,
                                          std::int64_t blockStart,
                                          float& localPeak)
        {
            if (!recordingMetronomeActive.load(std::memory_order_acquire)
                || !recordingMetronome.enabled
                || numOutputChannels <= 0
                || numSamples <= 0)
            {
                return false;
            }

            const auto rate = std::max(1, sampleRate.load());
            bool mixedAny = false;
            for (int frame = 0; frame < numSamples; ++frame)
            {
                const double projectSeconds = recordingMetronomeProjectStartSeconds
                    + static_cast<double>(blockStart + frame) / static_cast<double>(rate);
                const float click = mw::midi::midiRecordingMetronomeSampleAtProjectTime(
                    recordingMetronome, projectSeconds);
                if (click == 0.0f)
                    continue;

                for (int channel = 0; channel < numOutputChannels; ++channel)
                    mixOutputSample(outputChannelData, channel, frame, click, localPeak);
                mixedAny = true;
            }
            return mixedAny;
        }

        bool allTracksFinished() const
        {
            for (const auto& track : clapTracks)
                if (track.prepared && !track.finished
                    && (track.liveMidiInput || !track.bridge.isFinished()))
                    return false;
            for (const auto& track : vstTracks)
                if (track.prepared && !track.finished
                    && (track.liveMidiInput || !track.bridge.isFinished()))
                    return false;
            return true;
        }

        bool allSourcesFinished() const
        {
            for (const auto& source : audioSources)
                if (!source.finished)
                    return false;
            return true;
        }

        void processAudio(const float* const*,
                          int,
                          float* const* outputChannelData,
                          int numOutputChannels,
                          int numSamples)
        {
            clearOutputs(outputChannelData, numOutputChannels, numSamples);
            if (!prepared || failed.load() || finished.load() || numOutputChannels <= 0 || numSamples <= 0)
                return;

            if (numSamples > kMaximumBlockSize || numSamples > configuredBlockSize.load())
            {
                setFailure("Mixed project audio callback received an unsupported block size.");
                return;
            }

            int eventsThisCallback = 0;
            bool processedAnyTrack = false;
            float localPeak = outputPeak.load();

            for (auto& track : clapTracks)
            {
                processedAnyTrack = processClapTrack(
                    track,
                    outputChannelData,
                    numOutputChannels,
                    numSamples,
                    eventsThisCallback,
                    localPeak) || processedAnyTrack;
                if (failed.load())
                    return;
            }

            for (auto& track : vstTracks)
            {
                processedAnyTrack = processVstTrack(
                    track,
                    outputChannelData,
                    numOutputChannels,
                    numSamples,
                    eventsThisCallback,
                    localPeak) || processedAnyTrack;
                if (failed.load())
                    return;
            }

            const auto blockStart = currentSample.load();
            const bool processedSource = mixAudioSources(
                outputChannelData,
                numOutputChannels,
                numSamples,
                blockStart,
                localPeak);
            if (failed.load())
                return;

            const bool processedMetronome = mixMidiRecordingMetronome(
                outputChannelData,
                numOutputChannels,
                numSamples,
                blockStart,
                localPeak);

            currentSample.store(blockStart + numSamples);
            outputPeak.store(localPeak);
            if (processedAnyTrack || processedSource || processedMetronome)
            {
                processedBlocks.fetch_add(1);
                submittedEvents.fetch_add(eventsThisCallback);
            }

            if (allTracksFinished() && allSourcesFinished())
                finished.store(true);
        }

        std::vector<ClapTrackState> clapTracks;
        std::vector<VstTrackState> vstTracks;
        std::vector<AudioSourceState> audioSources;
        int requestedOutputChannels = 2;
        mw::midi::MidiRecordingMetronomeConfig recordingMetronome;
        double recordingMetronomeProjectStartSeconds = 0.0;
        std::atomic<bool> recordingMetronomeActive { false };
        std::atomic<int> configuredBlockSize { 512 };
        bool prepared = false;
        bool deviceHasStarted = false;
        std::atomic<bool> finished { false };
        std::atomic<bool> failed { false };
        std::atomic<int> sampleRate { kFallbackSampleRate };
        std::atomic<int> primaryTrackIndex { -1 };
        std::atomic<int> trackCount { 0 };
        std::atomic<int> clapTrackCount { 0 };
        std::atomic<int> vstTrackCount { 0 };
        std::atomic<int> audioSourceCount { 0 };
        std::atomic<int> processedBlocks { 0 };
        std::atomic<int> submittedEvents { 0 };
        std::atomic<int> skippedBlocks { 0 };
        std::atomic<int> skippedEffectBlocks { 0 };
        std::atomic<int> scheduledEvents { 0 };
        std::atomic<int> maxEventsPerBlock { 0 };
        std::atomic<int> liveEffectSlotCount { 0 };
        std::atomic<int> processedEffectBlocks { 0 };
        std::atomic<int> emergencyNoteOffsSubmitted { 0 };
        std::atomic<int> stopFlushAttempted { 0 };
        std::atomic<int> stopFlushSucceeded { 0 };
        std::atomic<int> stopFlushSkippedBusy { 0 };
        std::atomic<int> clippedSamples { 0 };
        std::atomic<std::int64_t> currentSample { 0 };
        std::atomic<std::int64_t> totalSamples { 0 };
        std::atomic<float> outputPeak { 0.0f };
        mutable juce::CriticalSection messageLock;
        juce::String lastMessage;
    };

    MixedLiveProjectPreviewEngine::MixedLiveProjectPreviewEngine(mw::audio::ApplicationAudioService& audioService)
        : audioService_(audioService)
    {
    }

    MixedLiveProjectPreviewEngine::~MixedLiveProjectPreviewEngine()
    {
        stop();
    }

    double MixedLiveProjectStartResult::durationSeconds() const noexcept
    {
        const auto rate = sampleRate > 0 ? sampleRate : kFallbackSampleRate;
        return totalSamples > 0 ? static_cast<double>(totalSamples) / static_cast<double>(rate) : 0.0;
    }

    juce::String MixedLiveProjectStatus::toStatusMessage() const
    {
        if (!active)
            return "Mixed live project preview is not active.";

        juce::String text = finished
            ? "Mixed live project preview finished."
            : "Mixed live project preview is playing.";
        text << " CLAP tracks " << clapTrackCount
             << ", VST3 tracks " << vstTrackCount
             << ", prepared sources " << audioSourceCount
             << ", position samples " << currentSample << "/" << totalSamples
             << ", blocks " << processedBlocks
             << ", scheduled events " << scheduledEvents
             << ", submitted events " << submittedEvents
             << ", skipped busy blocks " << skippedBusyBlocks;
        if (liveEffectSlotCount > 0)
            text << ", live effect slots " << liveEffectSlotCount
                 << ", effect blocks " << processedEffectBlocks
                 << ", skipped effect busy blocks " << skippedEffectBusyBlocks;
        if (emergencyNoteOffsSubmitted > 0 || stopFlushAttempted > 0)
            text << ", emergency note-offs " << emergencyNoteOffsSubmitted
                 << ", stop flush attempts " << stopFlushAttempted
                 << ", stop flush success " << stopFlushSucceeded;
        if (clippedSamples > 0)
            text << ", clipped samples " << clippedSamples;
        text << ", peak " << juce::String(peak, 6) << ".";
        if (hadFailure && lastMessage.isNotEmpty())
            text << " Last callback message: " << lastMessage;
        return text;
    }

    juce::String MixedLiveProjectStopSummary::toLogMessage() const
    {
        if (!wasActive)
            return {};

        juce::String text = hadFailure
            ? "Mixed live project preview stopped after callback failure. "
            : "Mixed live project preview stopped. ";
        text << "CLAP tracks " << clapTrackCount
             << ", VST3 tracks " << vstTrackCount
             << ", prepared sources " << audioSourceCount
             << ", position samples " << currentSample << "/" << totalSamples
             << ", blocks " << processedBlocks
             << ", scheduled events " << scheduledEvents
             << ", submitted events " << submittedEvents
             << ", skipped busy blocks " << skippedBusyBlocks;
        if (liveEffectSlotCount > 0)
            text << ", live effect slots " << liveEffectSlotCount
                 << ", effect blocks " << processedEffectBlocks
                 << ", skipped effect busy blocks " << skippedEffectBusyBlocks;
        if (stopFlushAttempted > 0 || emergencyNoteOffsSubmitted > 0 || stopFlushSkippedBusy > 0)
            text << ", stop flush attempts " << stopFlushAttempted
                 << ", stop flush success " << stopFlushSucceeded
                 << ", emergency note-offs " << emergencyNoteOffsSubmitted
                 << ", skipped stop flushes " << stopFlushSkippedBusy;
        if (clippedSamples > 0)
            text << ", clipped samples " << clippedSamples;
        text << ", peak " << juce::String(peak, 6) << ".";
        if (hadFailure && lastMessage.isNotEmpty())
            text << " Last callback message: " << lastMessage;
        return text;
    }

    MixedLiveProjectStartResult MixedLiveProjectPreviewEngine::start(
        std::vector<mw::clap::ClapLiveDirectPreviewTrackRequest> clapTracks,
        std::vector<mw::vst::VstLiveDirectPreviewTrackRequest> vstTracks,
        std::vector<mw::clap::ClapLiveDirectPreviewAudioSourceRequest> audioSources,
        int outputChannelCount,
        mw::midi::MidiRecordingMetronomeConfig recordingMetronome,
        double recordingMetronomeProjectStartSeconds)
    {
        stop();

        MixedLiveProjectStartResult result;
        result.outputChannelCount = sanitizeOutputChannelCount(outputChannelCount);
        auto callback = std::make_unique<MixedProjectAudioCallback>(
            std::move(clapTracks),
            std::move(vstTracks),
            std::move(audioSources),
            result.outputChannelCount,
            std::move(recordingMetronome),
            recordingMetronomeProjectStartSeconds);

        auto copyStartFields = [&]
        {
            result.trackIndex = callback->getTrackIndex();
            result.trackCount = callback->getTrackCount();
            result.clapTrackCount = callback->getClapTrackCount();
            result.vstTrackCount = callback->getVstTrackCount();
            result.audioSourceCount = callback->getAudioSourceCount();
            result.sampleRate = callback->getSampleRate();
            result.blockSize = callback->getBlockSize();
            result.scheduledEvents = callback->getScheduledEvents();
            result.maxEventsPerBlock = callback->getMaxEventsPerBlock();
            result.liveEffectSlotCount = callback->getLiveEffectSlotCount();
            result.processedEffectBlocks = callback->getProcessedEffectBlocks();
            result.skippedBusyBlocks = callback->getSkippedBlocks();
            result.skippedEffectBusyBlocks = callback->getSkippedEffectBlocks();
            result.consecutiveBusyBlockLimit = callback->getConsecutiveBusyBlockLimit();
            result.emergencyNoteOffsSubmitted = callback->getEmergencyNoteOffsSubmitted();
            result.stopFlushAttempted = callback->getStopFlushAttempted();
            result.stopFlushSucceeded = callback->getStopFlushSucceeded();
            result.stopFlushSkippedBusy = callback->getStopFlushSkippedBusy();
            result.clippedSamples = callback->getClippedSamples();
            result.currentSample = callback->getCurrentSample();
            result.totalSamples = callback->getTotalSamples();
        };
        copyStartFields();

        if (callback->hasFailed())
        {
            result.errorMessage = callback->getLastMessage();
            return result;
        }
        const bool hasLiveMidiInput = callback->hasLiveMidiInput();
        if ((result.trackCount <= 0 && result.audioSourceCount <= 0)
            || (!hasLiveMidiInput && result.trackCount > 0 && result.scheduledEvents <= 0)
            || (!hasLiveMidiInput && result.totalSamples <= 0))
        {
            result.errorMessage = "No mixed live tracks or prepared audio sources produced playable content.";
            return result;
        }

        const auto activation = audioService_.activateOutputCallback(
            mw::audio::ApplicationAudioPlaybackOwner::MixedLiveProjectPreview,
            callback.get());
        if (!activation.success)
        {
            result.errorMessage = "Mixed project audio device did not open. " + activation.message;
            return result;
        }
        copyStartFields();
        if (callback->hasFailed())
        {
            audioService_.deactivateOutputCallback(
                mw::audio::ApplicationAudioPlaybackOwner::MixedLiveProjectPreview,
                callback.get());
            result.errorMessage = callback->getLastMessage();
            return result;
        }

        result.started = true;
        result.message = "One shared audio callback is mixing ";
        result.message << result.clapTrackCount << " live CLAP track(s), "
                       << result.vstTrackCount << " live VST3 track(s), and "
                       << result.audioSourceCount << " prepared source(s).";
        if (result.liveEffectSlotCount > 0)
            result.message << " Live effect slots active: " << result.liveEffectSlotCount << ".";
        result.message << " Busy-block watchdog, device re-preparation, finite-sample filtering, clipping containment, and stop note-off flushing are enabled.";

        activeTrackIndex_ = result.trackIndex;
        active_ = true;
        callback_ = std::move(callback);
        return result;
    }

    void MixedLiveProjectPreviewEngine::setMidiRecordingMetronomeActive(bool active) noexcept
    {
        if (callback_ != nullptr)
            callback_->setMidiRecordingMetronomeActive(active);
    }

    MixedLiveProjectStopSummary MixedLiveProjectPreviewEngine::stop()
    {
        MixedLiveProjectStopSummary summary;
        summary.wasActive = active_ || callback_ != nullptr;
        summary.trackIndex = activeTrackIndex_;

        if (callback_ != nullptr)
        {
            audioService_.deactivateOutputCallback(
                mw::audio::ApplicationAudioPlaybackOwner::MixedLiveProjectPreview,
                callback_.get());
            callback_->flushActiveNotesForStop();
            summary.hadFailure = callback_->hasFailed();
            summary.trackIndex = callback_->getTrackIndex();
            summary.trackCount = callback_->getTrackCount();
            summary.clapTrackCount = callback_->getClapTrackCount();
            summary.vstTrackCount = callback_->getVstTrackCount();
            summary.audioSourceCount = callback_->getAudioSourceCount();
            summary.sampleRate = callback_->getSampleRate();
            summary.blockSize = callback_->getBlockSize();
            summary.processedBlocks = callback_->getProcessedBlocks();
            summary.submittedEvents = callback_->getSubmittedEvents();
            summary.skippedBusyBlocks = callback_->getSkippedBlocks();
            summary.skippedEffectBusyBlocks = callback_->getSkippedEffectBlocks();
            summary.scheduledEvents = callback_->getScheduledEvents();
            summary.maxEventsPerBlock = callback_->getMaxEventsPerBlock();
            summary.liveEffectSlotCount = callback_->getLiveEffectSlotCount();
            summary.processedEffectBlocks = callback_->getProcessedEffectBlocks();
            summary.consecutiveBusyBlockLimit = callback_->getConsecutiveBusyBlockLimit();
            summary.emergencyNoteOffsSubmitted = callback_->getEmergencyNoteOffsSubmitted();
            summary.stopFlushAttempted = callback_->getStopFlushAttempted();
            summary.stopFlushSucceeded = callback_->getStopFlushSucceeded();
            summary.stopFlushSkippedBusy = callback_->getStopFlushSkippedBusy();
            summary.clippedSamples = callback_->getClippedSamples();
            summary.currentSample = callback_->getCurrentSample();
            summary.totalSamples = callback_->getTotalSamples();
            summary.peak = callback_->getPeak();
            summary.lastMessage = callback_->getLastMessage();
        }

        callback_.reset();
        activeTrackIndex_ = -1;
        active_ = false;
        return summary;
    }

    MixedLiveProjectStatus MixedLiveProjectPreviewEngine::status() const
    {
        MixedLiveProjectStatus result;
        result.active = active_ && callback_ != nullptr;
        result.trackIndex = activeTrackIndex_;
        if (callback_ == nullptr)
            return result;

        result.finished = callback_->hasFinished();
        result.hadFailure = callback_->hasFailed();
        result.trackIndex = callback_->getTrackIndex();
        result.trackCount = callback_->getTrackCount();
        result.clapTrackCount = callback_->getClapTrackCount();
        result.vstTrackCount = callback_->getVstTrackCount();
        result.audioSourceCount = callback_->getAudioSourceCount();
        result.sampleRate = callback_->getSampleRate();
        result.blockSize = callback_->getBlockSize();
        result.processedBlocks = callback_->getProcessedBlocks();
        result.submittedEvents = callback_->getSubmittedEvents();
        result.skippedBusyBlocks = callback_->getSkippedBlocks();
        result.skippedEffectBusyBlocks = callback_->getSkippedEffectBlocks();
        result.scheduledEvents = callback_->getScheduledEvents();
        result.maxEventsPerBlock = callback_->getMaxEventsPerBlock();
        result.liveEffectSlotCount = callback_->getLiveEffectSlotCount();
        result.processedEffectBlocks = callback_->getProcessedEffectBlocks();
        result.consecutiveBusyBlockLimit = callback_->getConsecutiveBusyBlockLimit();
        result.emergencyNoteOffsSubmitted = callback_->getEmergencyNoteOffsSubmitted();
        result.stopFlushAttempted = callback_->getStopFlushAttempted();
        result.stopFlushSucceeded = callback_->getStopFlushSucceeded();
        result.stopFlushSkippedBusy = callback_->getStopFlushSkippedBusy();
        result.clippedSamples = callback_->getClippedSamples();
        result.currentSample = callback_->getCurrentSample();
        result.totalSamples = callback_->getTotalSamples();
        result.peak = callback_->getPeak();
        result.lastMessage = callback_->getLastMessage();
        return result;
    }

    bool MixedLiveProjectPreviewEngine::isActive() const noexcept
    {
        return active_ && callback_ != nullptr;
    }

    int MixedLiveProjectPreviewEngine::activeTrackIndex() const noexcept
    {
        return activeTrackIndex_;
    }

    bool MixedLiveProjectPreviewEngine::submitLiveMidiMessage(
        mw::core::StableId trackId,
        const juce::MidiMessage& message) noexcept
    {
        return callback_ != nullptr && callback_->submitLiveMidiMessage(trackId, message);
    }

    void MixedLiveProjectPreviewEngine::requestLiveMidiPanic(mw::core::StableId trackId) noexcept
    {
        if (callback_ != nullptr)
            callback_->requestLiveMidiPanic(trackId);
    }
}
