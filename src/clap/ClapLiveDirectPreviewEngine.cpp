#include "clap/ClapLiveDirectPreviewEngine.h"
#include "audio/GainLimits.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <utility>

namespace mw::clap
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
        constexpr int kMaximumConsecutiveBusyBlocks = 200;
        constexpr int kMaximumPluginOutputChannels = 64;

        bool hasValidPluginOutputBuffer(int expectedFrames,
                                        int outputFrames,
                                        int outputChannels,
                                        std::size_t interleavedSampleCount) noexcept
        {
            if (expectedFrames <= 0
                || outputFrames != expectedFrames
                || outputChannels < kMinimumOutputChannels
                || outputChannels > kMaximumPluginOutputChannels)
            {
                return false;
            }

            const auto requiredSamples = static_cast<std::size_t>(expectedFrames)
                * static_cast<std::size_t>(outputChannels);
            return interleavedSampleCount >= requiredSamples;
        }

        int sanitizeOutputChannelCount(int value)
        {
            return std::clamp(value > 0 ? value : 2, kMinimumOutputChannels, kMaximumOutputChannels);
        }
    }

    class ClapLiveDirectPreviewEngine::DirectPreviewAudioCallback final : public juce::AudioIODeviceCallback
    {
    public:
        DirectPreviewAudioCallback(std::vector<ClapLiveDirectPreviewTrackRequest> requestsIn,
                                   std::vector<ClapLiveDirectPreviewAudioSourceRequest> audioSourcesIn,
                                   int requestedOutputChannelCount)
            : requestedOutputChannels(sanitizeOutputChannelCount(requestedOutputChannelCount))
        {
            tracks.reserve(requestsIn.size());
            for (auto& request : requestsIn)
            {
                if (request.session == nullptr || request.sessionProcessMutex == nullptr || request.notes.empty())
                    continue;

                TrackState track;
                track.session = request.session;
                track.sessionMutex = request.sessionProcessMutex;
                track.notes = std::move(request.notes);
                track.config = std::move(request.config);
                track.outputGain = mw::audio::sanitizeMainUiGain(request.outputGain);
                if (track.config.trackIndex < 0)
                    track.config.trackIndex = request.trackIndex;
                if (track.config.trackName.empty())
                    track.config.trackName = request.trackName;
                if (track.config.outputChannelCount <= 0)
                    track.config.outputChannelCount = requestedOutputChannels;

                track.effects.reserve(request.effects.size());
                for (const auto& effectRequest : request.effects)
                {
                    if (effectRequest.session == nullptr || effectRequest.sessionProcessMutex == nullptr)
                        continue;

                    EffectState effect;
                    effect.slotIndex = effectRequest.slotIndex;
                    effect.displayName = effectRequest.displayName;
                    effect.session = effectRequest.session;
                    effect.sessionMutex = effectRequest.sessionProcessMutex;
                    track.effects.push_back(effect);
                }

                tracks.push_back(std::move(track));
            }

            audioSources.reserve(audioSourcesIn.size());
            for (auto& request : audioSourcesIn)
            {
                const auto channelCount = request.sourceChannelCount;
                const bool hasExactFrameLayout = channelCount > 0
                    && (request.interleavedAudio.size() % static_cast<std::size_t>(channelCount)) == 0;
                if (request.sourceSampleRate < kMinimumDeviceSampleRate
                    || request.sourceSampleRate > kMaximumDeviceSampleRate
                    || channelCount <= 0
                    || channelCount > kMaximumPluginOutputChannels
                    || !hasExactFrameLayout
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

            prepareTracks(kFallbackSampleRate, 512);
        }

        void audioDeviceAboutToStart(juce::AudioIODevice* device) override
        {
            const auto deviceSampleRate = device != nullptr && device->getCurrentSampleRate() > 0.0
                ? device->getCurrentSampleRate()
                : static_cast<double>(kFallbackSampleRate);
            const auto deviceBlockSize = device != nullptr && device->getCurrentBufferSizeSamples() > 0
                ? device->getCurrentBufferSizeSamples()
                : 512;

            prepareTracks(juce::jlimit(kMinimumDeviceSampleRate,
                                       kMaximumDeviceSampleRate,
                                       static_cast<int>(std::round(deviceSampleRate))),
                          juce::jlimit(kMinimumBlockSize, kMaximumBlockSize, deviceBlockSize));
            processedBlocks.store(0);
            submittedEvents.store(0);
            skippedBlocks.store(0);
            skippedEffectBlocks.store(0);
            processedEffectBlocks.store(0);
            consecutiveBusyBlockLimit.store(kMaximumConsecutiveBusyBlocks);
            emergencyNoteOffsSubmitted.store(0);
            stopFlushAttempted.store(0);
            stopFlushSucceeded.store(0);
            stopFlushSkippedBusy.store(0);
            clippedSamples.store(0);
            outputPeak.store(0.0f);
            failed.store(false);
            finished.store(!prepared);
            {
                const juce::ScopedLock lock(messageLock);
                lastMessage.clear();
            }
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
        int getTrackIndex() const noexcept { return primaryTrackIndex.load(); }
        int getTrackCount() const noexcept { return trackCount.load(); }
        int getAudioSourceCount() const noexcept { return audioSourceCount.load(); }
        int getProcessedBlocks() const noexcept { return processedBlocks.load(); }
        int getSubmittedEvents() const noexcept { return submittedEvents.load(); }
        int getSkippedBlocks() const noexcept { return skippedBlocks.load(); }
        int getScheduledEvents() const noexcept { return scheduledEvents.load(); }
        int getMaxEventsPerBlock() const noexcept { return maxEventsPerBlock.load(); }
        int getLiveEffectSlotCount() const noexcept { return liveEffectSlotCount.load(); }
        int getProcessedEffectBlocks() const noexcept { return processedEffectBlocks.load(); }
        int getSkippedEffectBlocks() const noexcept { return skippedEffectBlocks.load(); }
        int getConsecutiveBusyBlockLimit() const noexcept { return consecutiveBusyBlockLimit.load(); }
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

        void flushActiveNotesForStop()
        {
            if (!prepared)
                return;

            for (auto& track : tracks)
                flushActiveNotesForTrack(track);
        }

    private:
        struct EffectState
        {
            int slotIndex = -1;
            std::string displayName;
            ClapLiveEffectSession* session = nullptr;
            std::mutex* sessionMutex = nullptr;
        };

        struct TrackState
        {
            ClapLiveInstrumentSession* session = nullptr;
            std::mutex* sessionMutex = nullptr;
            std::vector<ClapLivePlaybackNote> notes;
            ClapLiveCallbackBridgeConfig config;
            ClapLiveCallbackBridge bridge;
            std::vector<EffectState> effects;
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

        static int noteChannelIndex(int midiChannel) noexcept
        {
            return std::clamp(midiChannel <= 0 ? 1 : midiChannel, 1, 16) - 1;
        }

        static int noteKeyIndex(int key) noexcept
        {
            return std::clamp(key, 0, 127);
        }

        static void clearActiveNotes(TrackState& track)
        {
            for (auto& channel : track.activeNoteDepths)
                channel.fill(0);
        }

        static void updateActiveNotesFromRequest(TrackState& track, const ClapLiveProcessRequest& request)
        {
            for (const auto& event : request.noteEvents)
            {
                const auto channel = noteChannelIndex(event.midiChannel);
                const auto key = noteKeyIndex(event.key);
                auto& depth = track.activeNoteDepths[static_cast<std::size_t>(channel)][static_cast<std::size_t>(key)];
                if (event.type == ClapLiveNoteEventType::NoteOn && event.velocity > 0)
                    depth = std::min(depth + 1, 127);
                else if (depth > 0)
                    --depth;
            }
        }

        bool flushActiveNotesForTrack(TrackState& track)
        {
            if (track.session == nullptr || track.sessionMutex == nullptr || !track.prepared)
                return false;

            ClapLiveProcessRequest releaseRequest;
            releaseRequest.frameCount = std::max(1, std::min(track.config.scheduler.blockSize > 0 ? track.config.scheduler.blockSize : 512, 512));

            for (int channel = 0; channel < 16; ++channel)
            {
                for (int key = 0; key < 128; ++key)
                {
                    const auto depth = track.activeNoteDepths[static_cast<std::size_t>(channel)][static_cast<std::size_t>(key)];
                    if (depth <= 0)
                        continue;

                    for (int duplicate = 0; duplicate < depth; ++duplicate)
                    {
                        ClapLiveNoteEvent event;
                        event.type = ClapLiveNoteEventType::NoteOff;
                        event.frameOffset = 0;
                        event.key = key;
                        event.velocity = 0;
                        event.midiChannel = channel + 1;
                        releaseRequest.noteEvents.push_back(event);
                    }
                }
            }

            if (releaseRequest.noteEvents.empty())
                return false;

            stopFlushAttempted.fetch_add(1);

            std::unique_lock<std::mutex> sessionLock(*track.sessionMutex, std::try_to_lock);
            if (!sessionLock.owns_lock())
            {
                stopFlushSkippedBusy.fetch_add(1);
                return false;
            }

            const auto releaseResult = track.session->processBlock(releaseRequest);
            if (!releaseResult.success)
            {
                failed.store(true);
                const juce::ScopedLock lock(messageLock);
                lastMessage = "CLAP preview stop note-off flush failed: " + juce::String(releaseResult.message);
                return false;
            }

            emergencyNoteOffsSubmitted.fetch_add(static_cast<int>(releaseRequest.noteEvents.size()));
            stopFlushSucceeded.fetch_add(1);
            clearActiveNotes(track);
            return true;
        }

        void prepareTracks(int deviceSampleRate, int deviceBlockSize)
        {
            int aggregateScheduledEvents = 0;
            int aggregateMaxEventsPerBlock = 0;
            int aggregateLiveEffectSlots = 0;
            std::int64_t aggregateTotalSamples = 0;
            int preparedTrackCount = 0;
            int firstTrackIndex = -1;

            for (auto& track : tracks)
            {
                track.config.scheduler.sampleRate = deviceSampleRate;
                track.config.scheduler.blockSize = deviceBlockSize;
                track.config.outputChannelCount = requestedOutputChannels;
                track.bridge.prepare(track.notes, track.config);
                track.prepared = track.bridge.isPrepared();
                track.finished = !track.prepared || track.bridge.isFinished();
                track.failed = false;
                track.consecutiveSkippedBlocks = 0;
                clearActiveNotes(track);

                const auto state = track.bridge.state();
                if (track.prepared)
                {
                    ++preparedTrackCount;
                    if (firstTrackIndex < 0)
                        firstTrackIndex = state.trackIndex;
                    aggregateScheduledEvents += state.totalScheduledEvents;
                    aggregateMaxEventsPerBlock += state.maxEventsPerBlock;
                    aggregateLiveEffectSlots += static_cast<int>(track.effects.size());
                    aggregateTotalSamples = std::max<std::int64_t>(aggregateTotalSamples, state.totalSamples);
                }
            }

            int preparedAudioSourceCount = 0;
            for (auto& source : audioSources)
            {
                source.finished = source.sourceFrames <= 0;
                if (source.finished)
                    continue;

                ++preparedAudioSourceCount;
                const auto durationAtDeviceRate = static_cast<std::int64_t>(std::ceil(
                    static_cast<double>(source.sourceFrames)
                    * static_cast<double>(deviceSampleRate)
                    / static_cast<double>(std::max(1, source.sourceSampleRate))));
                aggregateTotalSamples = std::max(aggregateTotalSamples, source.startSample + durationAtDeviceRate);
            }

            prepared = preparedTrackCount > 0 || preparedAudioSourceCount > 0;
            sampleRate.store(deviceSampleRate);
            currentSample.store(0);
            primaryTrackIndex.store(firstTrackIndex);
            trackCount.store(preparedTrackCount);
            audioSourceCount.store(preparedAudioSourceCount);
            scheduledEvents.store(aggregateScheduledEvents);
            maxEventsPerBlock.store(aggregateMaxEventsPerBlock);
            liveEffectSlotCount.store(aggregateLiveEffectSlots);
            consecutiveBusyBlockLimit.store(kMaximumConsecutiveBusyBlocks);
            totalSamples.store(aggregateTotalSamples);
            finished.store(!prepared);
        }

        void clearOutputs(float* const* outputChannelData, int numOutputChannels, int numSamples) const
        {
            for (int channel = 0; channel < numOutputChannels; ++channel)
                if (outputChannelData[channel] != nullptr)
                    juce::FloatVectorOperations::clear(outputChannelData[channel], numSamples);
        }


        bool processLiveEffectsForTrack(TrackState& track,
                                        std::vector<float>& interleavedAudio,
                                        int& audioChannels,
                                        int frames)
        {
            if (track.effects.empty() || frames <= 0 || audioChannels <= 0)
                return true;

            for (auto& effect : track.effects)
            {
                if (effect.session == nullptr || effect.sessionMutex == nullptr)
                    continue;

                std::unique_lock<std::mutex> effectLock(*effect.sessionMutex, std::try_to_lock);
                if (!effectLock.owns_lock())
                {
                    skippedEffectBlocks.fetch_add(1);
                    continue;
                }

                ClapLiveEffectProcessRequest effectRequest;
                effectRequest.frameCount = frames;
                effectRequest.inputChannelCount = audioChannels;
                effectRequest.interleavedInputAudio = interleavedAudio;

                auto effectResult = effect.session->processBlock(effectRequest);
                if (!effectResult.success)
                {
                    failed.store(true);
                    finished.store(true);
                    const juce::ScopedLock lock(messageLock);
                    juce::String effectName(effect.displayName);
                    if (effectName.isEmpty())
                        effectName = effect.slotIndex >= 0 ? (juce::String("slot ") + juce::String(effect.slotIndex + 1)) : juce::String("effect");
                    lastMessage = "CLAP live effect failed (" + effectName + "): " + juce::String(effectResult.message);
                    return false;
                }

                if (!hasValidPluginOutputBuffer(frames,
                                                effectResult.outputFrameCount,
                                                effectResult.outputChannelCount,
                                                effectResult.interleavedAudio.size()))
                {
                    failed.store(true);
                    finished.store(true);
                    const juce::ScopedLock lock(messageLock);
                    lastMessage = "CLAP live effect returned an invalid audio buffer.";
                    return false;
                }

                interleavedAudio = std::move(effectResult.interleavedAudio);
                audioChannels = effectResult.outputChannelCount;
                processedEffectBlocks.fetch_add(1);
            }

            return true;
        }

        void mixOutputSample(float* const* outputChannelData,
                             int channel,
                             int frame,
                             float sample,
                             float& localPeak)
        {
            if (outputChannelData[channel] == nullptr)
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

                    const double sourcePosition = static_cast<double>(timelineSample - source.startSample)
                        * static_cast<double>(source.sourceSampleRate)
                        / static_cast<double>(deviceRate);
                    const auto sourceFrame0 = static_cast<std::int64_t>(std::floor(sourcePosition));
                    if (sourceFrame0 >= source.sourceFrames)
                    {
                        source.finished = true;
                        break;
                    }

                    const auto sourceFrame1 = std::min(source.sourceFrames - 1, sourceFrame0 + 1);
                    const auto fraction = static_cast<float>(sourcePosition - static_cast<double>(sourceFrame0));
                    for (int channel = 0; channel < numOutputChannels; ++channel)
                    {
                        const auto sourceChannel = std::min(channel, source.sourceChannelCount - 1);
                        const auto index0 = static_cast<std::size_t>(sourceFrame0 * source.sourceChannelCount + sourceChannel);
                        const auto index1 = static_cast<std::size_t>(sourceFrame1 * source.sourceChannelCount + sourceChannel);
                        if (index0 >= source.interleavedAudio.size() || index1 >= source.interleavedAudio.size())
                        {
                            failed.store(true);
                            finished.store(true);
                            const juce::ScopedLock lock(messageLock);
                            lastMessage = "Rendered fallback audio source had invalid buffer dimensions.";
                            return false;
                        }

                        const auto sample0 = source.interleavedAudio[index0];
                        const auto sample1 = source.interleavedAudio[index1];
                        const auto sampleValue = (sample0 + ((sample1 - sample0) * fraction)) * source.outputGain;
                        mixOutputSample(outputChannelData, channel, frame, sampleValue, localPeak);
                    }
                    processed = true;
                }
            }

            return processed;
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

            bool anyTrackStillActive = false;
            bool anyTrackProcessedThisBlock = false;
            int eventsThisCallback = 0;
            float localPeak = outputPeak.load();

            for (auto& track : tracks)
            {
                if (!track.prepared || track.failed)
                    continue;

                if (track.finished || track.bridge.isFinished())
                {
                    track.finished = true;
                    continue;
                }

                anyTrackStillActive = true;

                std::unique_lock<std::mutex> sessionLock(*track.sessionMutex, std::try_to_lock);
                if (!sessionLock.owns_lock())
                {
                    skippedBlocks.fetch_add(1);
                    ++track.consecutiveSkippedBlocks;
                    if (track.consecutiveSkippedBlocks >= kMaximumConsecutiveBusyBlocks)
                    {
                        track.failed = true;
                        failed.store(true);
                        finished.store(true);
                        const juce::ScopedLock lock(messageLock);
                        lastMessage = "CLAP preview stopped because the live instrument stayed busy for too many consecutive audio blocks.";
                        return;
                    }
                    continue;
                }

                track.consecutiveSkippedBlocks = 0;

                const auto& request = track.bridge.nextRequest();
                if (request.frameCount <= 0)
                {
                    track.finished = true;
                    continue;
                }

                auto result = track.session->processBlock(request);
                if (!result.success)
                {
                    track.failed = true;
                    failed.store(true);
                    finished.store(true);
                    const juce::ScopedLock lock(messageLock);
                    lastMessage = juce::String(result.message);
                    return;
                }

                updateActiveNotesFromRequest(track, request);

                auto blockAudio = std::move(result.interleavedAudio);
                auto outputChannels = result.outputChannelCount;
                const auto outputFrames = result.outputFrameCount;
                if (!hasValidPluginOutputBuffer(request.frameCount,
                                                outputFrames,
                                                outputChannels,
                                                blockAudio.size()))
                {
                    track.failed = true;
                    failed.store(true);
                    finished.store(true);
                    const juce::ScopedLock lock(messageLock);
                    lastMessage = "CLAP live instrument returned an invalid audio buffer.";
                    return;
                }

                const auto framesToCopy = std::min({ numSamples, request.frameCount, outputFrames });

                if (!processLiveEffectsForTrack(track, blockAudio, outputChannels, framesToCopy))
                    return;

                for (int frame = 0; frame < framesToCopy; ++frame)
                {
                    for (int ch = 0; ch < numOutputChannels; ++ch)
                    {
                        float sample = 0.0f;
                        if (ch < outputChannels)
                        {
                            const auto idx = static_cast<std::size_t>(frame * outputChannels + ch);
                            if (idx < blockAudio.size())
                                sample = blockAudio[idx] * track.outputGain;
                        }

                        mixOutputSample(outputChannelData, ch, frame, sample, localPeak);
                    }
                }

                anyTrackProcessedThisBlock = true;
                eventsThisCallback += static_cast<int>(request.noteEvents.size());
                if (track.bridge.isFinished())
                    track.finished = true;
            }

            const auto blockStartSample = currentSample.load();
            const bool anyAudioSourceProcessed = mixAudioSources(
                outputChannelData, numOutputChannels, numSamples, blockStartSample, localPeak);
            if (failed.load())
                return;

            currentSample.store(blockStartSample + numSamples);
            outputPeak.store(localPeak);
            if (anyTrackProcessedThisBlock || anyAudioSourceProcessed)
            {
                processedBlocks.fetch_add(1);
                submittedEvents.fetch_add(eventsThisCallback);
            }

            bool allPreparedTracksFinished = true;
            for (const auto& track : tracks)
            {
                if (track.prepared && !track.finished && !track.bridge.isFinished())
                {
                    allPreparedTracksFinished = false;
                    break;
                }
            }

            bool allAudioSourcesFinished = true;
            for (const auto& source : audioSources)
            {
                if (!source.finished)
                {
                    allAudioSourcesFinished = false;
                    break;
                }
            }

            const bool tracksFinished = tracks.empty() || !anyTrackStillActive || allPreparedTracksFinished;
            if (tracksFinished && allAudioSourcesFinished)
                finished.store(true);
        }

        std::vector<TrackState> tracks;
        std::vector<AudioSourceState> audioSources;
        int requestedOutputChannels = 2;
        bool prepared = false;
        std::atomic<bool> finished { false };
        std::atomic<bool> failed { false };
        std::atomic<int> sampleRate { kFallbackSampleRate };
        std::atomic<int> primaryTrackIndex { -1 };
        std::atomic<int> trackCount { 0 };
        std::atomic<int> audioSourceCount { 0 };
        std::atomic<int> processedBlocks { 0 };
        std::atomic<int> submittedEvents { 0 };
        std::atomic<int> skippedBlocks { 0 };
        std::atomic<int> skippedEffectBlocks { 0 };
        std::atomic<int> scheduledEvents { 0 };
        std::atomic<int> maxEventsPerBlock { 0 };
        std::atomic<int> liveEffectSlotCount { 0 };
        std::atomic<int> processedEffectBlocks { 0 };
        std::atomic<int> consecutiveBusyBlockLimit { kMaximumConsecutiveBusyBlocks };
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

    ClapLiveDirectPreviewEngine::ClapLiveDirectPreviewEngine() = default;

    double ClapLiveDirectPreviewStartResult::durationSeconds() const noexcept
    {
        const auto sr = sampleRate > 0 ? sampleRate : kFallbackSampleRate;
        return totalSamples > 0 ? static_cast<double>(totalSamples) / static_cast<double>(sr) : 0.0;
    }

    juce::String ClapLiveDirectPreviewStatus::toStatusMessage() const
    {
        if (!active)
            return "CLAP Preview is not active.";

        juce::String text = finished
            ? juce::String("CLAP Preview finished.")
            : juce::String("CLAP Preview is playing.");

        if (trackCount > 1)
            text << " Tracks " << trackCount << ".";
        if (audioSourceCount > 0)
            text << " Prepared audio sources " << audioSourceCount << ".";

        text << " Position samples " << currentSample << "/" << totalSamples
             << ", blocks " << processedBlocks
             << ", scheduled events " << scheduledEvents
             << ", submitted events " << submittedEvents
             << ", skipped busy blocks " << skippedBusyBlocks;

        if (liveEffectSlotCount > 0)
            text << ", live effect slots " << liveEffectSlotCount
                 << ", effect blocks " << processedEffectBlocks
                 << ", skipped effect busy blocks " << skippedEffectBusyBlocks;

        if (skippedBusyBlocks > 0 && consecutiveBusyBlockLimit > 0)
            text << ", busy watchdog limit " << consecutiveBusyBlockLimit;

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

    juce::String ClapLiveDirectPreviewStopSummary::toLogMessage() const
    {
        if (!wasActive)
            return {};

        juce::String text = hadFailure ? "CLAP Preview stopped after callback failure. " : "CLAP Preview stopped. ";
        if (trackCount > 1)
            text << "Tracks " << trackCount << ", ";
        if (audioSourceCount > 0)
            text << "prepared audio sources " << audioSourceCount << ", ";
        text << "position samples " << currentSample << "/" << totalSamples << ", blocks "
             << processedBlocks
             << ", scheduled events " << scheduledEvents
             << ", submitted events " << submittedEvents
             << ", skipped busy blocks " << skippedBusyBlocks;

        if (liveEffectSlotCount > 0)
            text << ", live effect slots " << liveEffectSlotCount
                 << ", effect blocks " << processedEffectBlocks
                 << ", skipped effect busy blocks " << skippedEffectBusyBlocks;

        if (skippedBusyBlocks > 0 && consecutiveBusyBlockLimit > 0)
            text << ", busy watchdog limit " << consecutiveBusyBlockLimit;

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

    ClapLiveDirectPreviewEngine::~ClapLiveDirectPreviewEngine()
    {
        stop();
    }

    ClapLiveDirectPreviewStartResult ClapLiveDirectPreviewEngine::start(ClapLiveInstrumentSession& session,
                                                                        std::mutex& sessionProcessMutex,
                                                                        std::vector<ClapLivePlaybackNote> notes,
                                                                        ClapLiveCallbackBridgeConfig config)
    {
        ClapLiveDirectPreviewTrackRequest track;
        track.trackIndex = config.trackIndex;
        track.trackName = config.trackName;
        track.session = &session;
        track.sessionProcessMutex = &sessionProcessMutex;
        track.notes = std::move(notes);
        track.config = std::move(config);

        const auto outputChannels = track.config.outputChannelCount > 0 ? track.config.outputChannelCount : 2;
        std::vector<ClapLiveDirectPreviewTrackRequest> tracks;
        tracks.push_back(std::move(track));
        return start(std::move(tracks), outputChannels);
    }

    ClapLiveDirectPreviewStartResult ClapLiveDirectPreviewEngine::start(std::vector<ClapLiveDirectPreviewTrackRequest> tracks,
                                                                        int outputChannelCount)
    {
        return start(std::move(tracks), {}, outputChannelCount);
    }

    ClapLiveDirectPreviewStartResult ClapLiveDirectPreviewEngine::start(
        std::vector<ClapLiveDirectPreviewTrackRequest> tracks,
        std::vector<ClapLiveDirectPreviewAudioSourceRequest> audioSources,
        int outputChannelCount)
    {
        stop();

        ClapLiveDirectPreviewStartResult result;
        result.outputChannelCount = sanitizeOutputChannelCount(outputChannelCount);

        auto callback = std::make_unique<DirectPreviewAudioCallback>(std::move(tracks), std::move(audioSources), result.outputChannelCount);

        result.trackIndex = callback->getTrackIndex();
        result.trackCount = callback->getTrackCount();
        result.audioSourceCount = callback->getAudioSourceCount();
        result.sampleRate = callback->getSampleRate();
        result.scheduledEvents = callback->getScheduledEvents();
        result.maxEventsPerBlock = callback->getMaxEventsPerBlock();
        result.liveEffectSlotCount = callback->getLiveEffectSlotCount();
        result.processedEffectBlocks = callback->getProcessedEffectBlocks();
        result.skippedEffectBusyBlocks = callback->getSkippedEffectBlocks();
        result.consecutiveBusyBlockLimit = callback->getConsecutiveBusyBlockLimit();
        result.emergencyNoteOffsSubmitted = callback->getEmergencyNoteOffsSubmitted();
        result.stopFlushAttempted = callback->getStopFlushAttempted();
        result.stopFlushSucceeded = callback->getStopFlushSucceeded();
        result.stopFlushSkippedBusy = callback->getStopFlushSkippedBusy();
        result.clippedSamples = callback->getClippedSamples();
        result.currentSample = callback->getCurrentSample();
        result.totalSamples = callback->getTotalSamples();

        if ((result.trackCount <= 0 && result.audioSourceCount <= 0)
            || (result.trackCount > 0 && result.scheduledEvents <= 0)
            || result.totalSamples <= 0)
        {
            result.errorMessage = "No live CLAP tracks or rendered audio sources produced playable content.";
            return result;
        }

        const auto initError = deviceManager_.initialise(0, result.outputChannelCount, nullptr, true);
        if (initError.isNotEmpty())
        {
            result.errorMessage = "Direct audio device did not open. " + initError;
            deviceManager_.closeAudioDevice();
            return result;
        }

        deviceManager_.addAudioCallback(callback.get());

        // The audio callback may have re-prepared against the actual device rate/block size.
        result.trackIndex = callback->getTrackIndex();
        result.trackCount = callback->getTrackCount();
        result.audioSourceCount = callback->getAudioSourceCount();
        result.sampleRate = callback->getSampleRate();
        result.scheduledEvents = callback->getScheduledEvents();
        result.maxEventsPerBlock = callback->getMaxEventsPerBlock();
        result.liveEffectSlotCount = callback->getLiveEffectSlotCount();
        result.processedEffectBlocks = callback->getProcessedEffectBlocks();
        result.skippedEffectBusyBlocks = callback->getSkippedEffectBlocks();
        result.consecutiveBusyBlockLimit = callback->getConsecutiveBusyBlockLimit();
        result.emergencyNoteOffsSubmitted = callback->getEmergencyNoteOffsSubmitted();
        result.stopFlushAttempted = callback->getStopFlushAttempted();
        result.stopFlushSucceeded = callback->getStopFlushSucceeded();
        result.stopFlushSkippedBusy = callback->getStopFlushSkippedBusy();
        result.clippedSamples = callback->getClippedSamples();
        result.currentSample = callback->getCurrentSample();
        result.totalSamples = callback->getTotalSamples();
        result.started = true;
        if (result.audioSourceCount > 0 && result.trackCount > 0)
            result.message = "Hybrid project preview is mixing live CLAP tracks with prepared rendered sources through one audio callback.";
        else if (result.trackCount > 1)
            result.message = "Direct multi-track CLAP project preview is using the audio callback path. Rendered preview remains available as fallback.";
        else if (result.trackCount == 1)
            result.message = "Direct selected-track CLAP preview is using the audio callback path. Temp-WAV preview remains available as fallback.";
        else
            result.message = "Prepared rendered preview audio is using the direct audio callback path.";
        if (result.audioSourceCount > 0)
            result.message << " Prepared audio sources active: " << result.audioSourceCount << ".";
        if (result.liveEffectSlotCount > 0)
            result.message << " Live CLAP effect slots active: " << result.liveEffectSlotCount << ".";
        result.message << " Busy-block watchdog and stop note-off flush are enabled.";

        activeTrackIndex_ = result.trackIndex;
        active_ = true;
        callback_ = std::move(callback);
        return result;
    }

    ClapLiveDirectPreviewStopSummary ClapLiveDirectPreviewEngine::stop()
    {
        ClapLiveDirectPreviewStopSummary summary;
        summary.wasActive = active_ || callback_ != nullptr;
        summary.trackIndex = activeTrackIndex_;

        if (callback_ != nullptr)
        {
            deviceManager_.removeAudioCallback(callback_.get());
            callback_->flushActiveNotesForStop();

            summary.hadFailure = callback_->hasFailed();
            summary.trackIndex = callback_->getTrackIndex();
            summary.trackCount = callback_->getTrackCount();
            summary.audioSourceCount = callback_->getAudioSourceCount();
            summary.processedBlocks = callback_->getProcessedBlocks();
            summary.submittedEvents = callback_->getSubmittedEvents();
            summary.skippedBusyBlocks = callback_->getSkippedBlocks();
            summary.scheduledEvents = callback_->getScheduledEvents();
            summary.maxEventsPerBlock = callback_->getMaxEventsPerBlock();
            summary.liveEffectSlotCount = callback_->getLiveEffectSlotCount();
            summary.processedEffectBlocks = callback_->getProcessedEffectBlocks();
            summary.skippedEffectBusyBlocks = callback_->getSkippedEffectBlocks();
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

            callback_.reset();
            deviceManager_.closeAudioDevice();
        }
        else if (active_)
        {
            deviceManager_.closeAudioDevice();
        }

        activeTrackIndex_ = -1;
        active_ = false;
        return summary;
    }

    ClapLiveDirectPreviewStatus ClapLiveDirectPreviewEngine::status() const
    {
        ClapLiveDirectPreviewStatus snapshot;
        snapshot.active = active_ || callback_ != nullptr;
        snapshot.trackIndex = activeTrackIndex_;

        if (callback_ != nullptr)
        {
            snapshot.finished = callback_->hasFinished();
            snapshot.hadFailure = callback_->hasFailed();
            snapshot.trackIndex = callback_->getTrackIndex();
            snapshot.trackCount = callback_->getTrackCount();
            snapshot.audioSourceCount = callback_->getAudioSourceCount();
            snapshot.processedBlocks = callback_->getProcessedBlocks();
            snapshot.submittedEvents = callback_->getSubmittedEvents();
            snapshot.skippedBusyBlocks = callback_->getSkippedBlocks();
            snapshot.scheduledEvents = callback_->getScheduledEvents();
            snapshot.maxEventsPerBlock = callback_->getMaxEventsPerBlock();
            snapshot.liveEffectSlotCount = callback_->getLiveEffectSlotCount();
            snapshot.processedEffectBlocks = callback_->getProcessedEffectBlocks();
            snapshot.skippedEffectBusyBlocks = callback_->getSkippedEffectBlocks();
            snapshot.consecutiveBusyBlockLimit = callback_->getConsecutiveBusyBlockLimit();
            snapshot.emergencyNoteOffsSubmitted = callback_->getEmergencyNoteOffsSubmitted();
            snapshot.stopFlushAttempted = callback_->getStopFlushAttempted();
            snapshot.stopFlushSucceeded = callback_->getStopFlushSucceeded();
            snapshot.stopFlushSkippedBusy = callback_->getStopFlushSkippedBusy();
            snapshot.clippedSamples = callback_->getClippedSamples();
            snapshot.currentSample = callback_->getCurrentSample();
            snapshot.totalSamples = callback_->getTotalSamples();
            snapshot.peak = callback_->getPeak();
            snapshot.lastMessage = callback_->getLastMessage();
        }

        return snapshot;
    }

    bool ClapLiveDirectPreviewEngine::isActive() const noexcept
    {
        return active_ || callback_ != nullptr;
    }

    int ClapLiveDirectPreviewEngine::activeTrackIndex() const noexcept
    {
        return activeTrackIndex_;
    }
}
