#include "vst/VstLiveDirectPreviewEngine.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <utility>

namespace mw::vst
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

        int sanitizeOutputChannelCount(int value)
        {
            return std::clamp(value > 0 ? value : 2, kMinimumOutputChannels, kMaximumOutputChannels);
        }

        int midiChannelIndex(int channel) noexcept
        {
            return std::clamp(channel <= 0 ? 1 : channel, 1, 16) - 1;
        }

        int midiKeyIndex(int key) noexcept
        {
            return std::clamp(key, 0, 127);
        }
    }

    class VstLiveDirectPreviewEngine::DirectPreviewAudioCallback final : public juce::AudioIODeviceCallback
    {
    public:
        DirectPreviewAudioCallback(VstLiveDirectPreviewTrackRequest requestIn,
                                   int requestedOutputChannelsIn)
            : request(std::move(requestIn)),
              requestedOutputChannels(sanitizeOutputChannelCount(requestedOutputChannelsIn))
        {
            request.outputGain = mw::audio::sanitizeMainUiGain(request.outputGain);
            request.config.outputChannelCount = requestedOutputChannels;
            prepare(kFallbackSampleRate, 512);
        }

        void audioDeviceAboutToStart(juce::AudioIODevice* device) override
        {
            const auto deviceSampleRate = device != nullptr && device->getCurrentSampleRate() > 0.0
                ? device->getCurrentSampleRate()
                : static_cast<double>(kFallbackSampleRate);
            const auto deviceBlockSize = device != nullptr && device->getCurrentBufferSizeSamples() > 0
                ? device->getCurrentBufferSizeSamples()
                : 512;

            processedBlocks.store(0);
            processedEffectBlocks.store(0);
            submittedEvents.store(0);
            skippedBusyBlocks.store(0);
            skippedEffectBusyBlocks.store(0);
            clippedSamples.store(0);
            emergencyNoteOffsSubmitted.store(0);
            outputPeak.store(0.0f);
            consecutiveBusyBlocks = 0;
            failed.store(false);
            finished.store(!prepared);
            {
                const juce::ScopedLock lock(messageLock);
                lastMessage.clear();
            }

            prepare(
                juce::jlimit(kMinimumDeviceSampleRate,
                             kMaximumDeviceSampleRate,
                             static_cast<int>(std::round(deviceSampleRate))),
                juce::jlimit(kMinimumBlockSize, kMaximumBlockSize, deviceBlockSize));
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
        int getTrackIndex() const noexcept { return request.trackIndex; }
        int getProcessedBlocks() const noexcept { return processedBlocks.load(); }
        int getProcessedEffectBlocks() const noexcept { return processedEffectBlocks.load(); }
        int getSubmittedEvents() const noexcept { return submittedEvents.load(); }
        int getSkippedBusyBlocks() const noexcept { return skippedBusyBlocks.load(); }
        int getSkippedEffectBusyBlocks() const noexcept { return skippedEffectBusyBlocks.load(); }
        int getScheduledEvents() const noexcept { return scheduledEvents.load(); }
        int getMaxEventsPerBlock() const noexcept { return maxEventsPerBlock.load(); }
        int getLiveEffectSlotCount() const noexcept { return static_cast<int>(request.effects.size()); }
        int getClippedSamples() const noexcept { return clippedSamples.load(); }
        int getEmergencyNoteOffsSubmitted() const noexcept { return emergencyNoteOffsSubmitted.load(); }
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
            if (!prepared || request.session == nullptr || request.sessionProcessMutex == nullptr)
                return;

            juce::MidiBuffer midi;
            int noteOffCount = 0;
            for (int channel = 0; channel < 16; ++channel)
            {
                for (int key = 0; key < 128; ++key)
                {
                    const auto depth = activeNoteDepths[static_cast<std::size_t>(channel)][static_cast<std::size_t>(key)];
                    for (int duplicate = 0; duplicate < depth; ++duplicate)
                    {
                        midi.addEvent(juce::MidiMessage::noteOff(channel + 1, key), 0);
                        ++noteOffCount;
                    }
                }
                midi.addEvent(juce::MidiMessage::allNotesOff(channel + 1), 0);
            }

            if (noteOffCount <= 0)
                return;

            std::unique_lock<std::mutex> sessionLock(*request.sessionProcessMutex, std::try_to_lock);
            if (!sessionLock.owns_lock())
                return;

            processBuffer.setSize(requestedOutputChannels,
                                  std::min(512, std::max(1, configuredBlockSize)),
                                  false,
                                  false,
                                  true);
            processBuffer.clear();
            std::string processError;
            if (request.session->processBlock(processBuffer, midi, processError))
            {
                emergencyNoteOffsSubmitted.fetch_add(noteOffCount);
                clearActiveNotes();
            }
            else
            {
                failed.store(true);
                const juce::ScopedLock lock(messageLock);
                lastMessage = juce::String(processError);
            }
        }

    private:
        void prepare(int deviceSampleRate, int deviceBlockSize)
        {
            configuredBlockSize = deviceBlockSize;
            prepared = false;

            if (request.session == nullptr || request.sessionProcessMutex == nullptr)
            {
                failed.store(true);
                const juce::ScopedLock lock(messageLock);
                lastMessage = "VST3 live preview has no instrument session.";
                finished.store(true);
                return;
            }

            std::string prepareError;
            {
                const std::lock_guard<std::mutex> sessionLock(*request.sessionProcessMutex);
                if (!request.session->prepareForPlayback(
                        static_cast<double>(deviceSampleRate),
                        deviceBlockSize,
                        requestedOutputChannels,
                        prepareError))
                {
                    failed.store(true);
                    const juce::ScopedLock lock(messageLock);
                    lastMessage = juce::String(prepareError);
                    finished.store(true);
                    return;
                }
            }

            for (auto& effect : request.effects)
            {
                if (effect.session == nullptr || effect.sessionProcessMutex == nullptr)
                    continue;

                const std::lock_guard<std::mutex> effectLock(*effect.sessionProcessMutex);
                if (!effect.session->prepareForPlayback(
                        static_cast<double>(deviceSampleRate),
                        deviceBlockSize,
                        requestedOutputChannels,
                        prepareError))
                {
                    failed.store(true);
                    const juce::ScopedLock lock(messageLock);
                    lastMessage = juce::String(prepareError);
                    finished.store(true);
                    return;
                }
            }

            request.config.scheduler.sampleRate = deviceSampleRate;
            request.config.scheduler.blockSize = deviceBlockSize;
            request.config.outputChannelCount = requestedOutputChannels;
            bridge.prepare(request.notes, request.config);
            prepared = bridge.isPrepared();

            processBuffer.setSize(requestedOutputChannels,
                                  kMaximumBlockSize,
                                  false,
                                  true,
                                  false);
            processBuffer.clear();
            clearActiveNotes();

            const auto state = bridge.state();
            sampleRate.store(deviceSampleRate);
            scheduledEvents.store(state.totalScheduledEvents);
            maxEventsPerBlock.store(state.maxEventsPerBlock);
            currentSample.store(0);
            totalSamples.store(state.totalSamples);
            finished.store(!prepared);
        }

        void clearActiveNotes()
        {
            for (auto& channel : activeNoteDepths)
                channel.fill(0);
        }

        void updateActiveNotes(const mw::clap::ClapLiveProcessRequest& blockRequest)
        {
            for (const auto& event : blockRequest.noteEvents)
            {
                const auto channel = midiChannelIndex(event.midiChannel);
                const auto key = midiKeyIndex(event.key);
                auto& depth = activeNoteDepths[static_cast<std::size_t>(channel)][static_cast<std::size_t>(key)];
                if (event.type == mw::clap::ClapLiveNoteEventType::NoteOn && event.velocity > 0)
                    depth = std::min(127, depth + 1);
                else if (depth > 0)
                    --depth;
            }
        }

        void clearOutputs(float* const* outputChannelData,
                          int numOutputChannels,
                          int numSamples) const
        {
            for (int channel = 0; channel < numOutputChannels; ++channel)
                if (outputChannelData[channel] != nullptr)
                    juce::FloatVectorOperations::clear(outputChannelData[channel], numSamples);
        }

        bool processEffects(std::string& errorMessage)
        {
            for (auto& effect : request.effects)
            {
                if (effect.session == nullptr || effect.sessionProcessMutex == nullptr)
                    continue;

                std::unique_lock<std::mutex> effectLock(*effect.sessionProcessMutex, std::try_to_lock);
                if (!effectLock.owns_lock())
                {
                    skippedEffectBusyBlocks.fetch_add(1);
                    continue;
                }

                if (!effect.session->processBlock(processBuffer, errorMessage))
                    return false;

                processedEffectBlocks.fetch_add(1);
            }

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

            if (numSamples > kMaximumBlockSize || numSamples != configuredBlockSize)
            {
                failed.store(true);
                finished.store(true);
                const juce::ScopedLock lock(messageLock);
                lastMessage = "VST3 live preview received an unexpected audio-device block size.";
                return;
            }

            std::unique_lock<std::mutex> sessionLock(*request.sessionProcessMutex, std::try_to_lock);
            if (!sessionLock.owns_lock())
            {
                skippedBusyBlocks.fetch_add(1);
                ++consecutiveBusyBlocks;
                if (consecutiveBusyBlocks >= kMaximumConsecutiveBusyBlocks)
                {
                    failed.store(true);
                    finished.store(true);
                    const juce::ScopedLock lock(messageLock);
                    lastMessage = "VST3 live preview stopped because the instrument stayed busy for too many consecutive audio blocks.";
                }
                return;
            }
            consecutiveBusyBlocks = 0;

            if (bridge.isFinished())
            {
                finished.store(true);
                return;
            }

            const auto& blockRequest = bridge.nextRequest();
            if (blockRequest.frameCount <= 0)
            {
                finished.store(true);
                return;
            }

            if (blockRequest.frameCount > numSamples)
            {
                failed.store(true);
                finished.store(true);
                const juce::ScopedLock lock(messageLock);
                lastMessage = "VST3 live scheduler block size exceeded the audio-device callback block.";
                return;
            }

            const auto framesToProcess = blockRequest.frameCount;
            processBuffer.setSize(requestedOutputChannels,
                                  framesToProcess,
                                  false,
                                  false,
                                  true);
            processBuffer.clear();
            midi.clear();

            for (const auto& event : blockRequest.noteEvents)
            {
                const auto channel = std::clamp(event.midiChannel, 1, 16);
                const auto key = std::clamp(event.key, 0, 127);
                const auto offset = std::clamp(static_cast<int>(event.frameOffset), 0, framesToProcess - 1);
                if (event.type == mw::clap::ClapLiveNoteEventType::NoteOn && event.velocity > 0)
                {
                    midi.addEvent(
                        juce::MidiMessage::noteOn(
                            channel,
                            key,
                            static_cast<juce::uint8>(std::clamp(event.velocity, 1, 127))),
                        offset);
                }
                else
                {
                    midi.addEvent(juce::MidiMessage::noteOff(channel, key), offset);
                }
            }

            std::string processError;
            if (!request.session->processBlock(processBuffer, midi, processError))
            {
                failed.store(true);
                finished.store(true);
                const juce::ScopedLock lock(messageLock);
                lastMessage = juce::String(processError);
                return;
            }

            updateActiveNotes(blockRequest);
            submittedEvents.fetch_add(static_cast<int>(blockRequest.noteEvents.size()));

            if (!processEffects(processError))
            {
                failed.store(true);
                finished.store(true);
                const juce::ScopedLock lock(messageLock);
                lastMessage = juce::String(processError);
                return;
            }

            float localPeak = outputPeak.load();
            const auto channelsToCopy = std::min({ numOutputChannels,
                                                   requestedOutputChannels,
                                                   processBuffer.getNumChannels(),
                                                   kMaximumPluginChannels });
            for (int frame = 0; frame < framesToProcess; ++frame)
            {
                for (int channel = 0; channel < channelsToCopy; ++channel)
                {
                    if (outputChannelData[channel] == nullptr)
                        continue;

                    auto sample = processBuffer.getSample(channel, frame) * request.outputGain;
                    if (!std::isfinite(sample))
                        sample = 0.0f;
                    if (sample > 1.0f)
                    {
                        sample = 1.0f;
                        clippedSamples.fetch_add(1);
                    }
                    else if (sample < -1.0f)
                    {
                        sample = -1.0f;
                        clippedSamples.fetch_add(1);
                    }
                    outputChannelData[channel][frame] = sample;
                    localPeak = std::max(localPeak, std::abs(sample));
                }
            }

            outputPeak.store(localPeak);
            processedBlocks.fetch_add(1);
            currentSample.store(bridge.state().currentSample);
            if (bridge.isFinished())
                finished.store(true);
        }

        VstLiveDirectPreviewTrackRequest request;
        int requestedOutputChannels = 2;
        int configuredBlockSize = 512;
        mw::clap::ClapLiveCallbackBridge bridge;
        juce::AudioBuffer<float> processBuffer;
        juce::MidiBuffer midi;
        std::array<std::array<int, 128>, 16> activeNoteDepths {};
        int consecutiveBusyBlocks = 0;
        bool prepared = false;
        std::atomic<bool> finished { false };
        std::atomic<bool> failed { false };
        std::atomic<int> sampleRate { kFallbackSampleRate };
        std::atomic<int> processedBlocks { 0 };
        std::atomic<int> processedEffectBlocks { 0 };
        std::atomic<int> submittedEvents { 0 };
        std::atomic<int> skippedBusyBlocks { 0 };
        std::atomic<int> skippedEffectBusyBlocks { 0 };
        std::atomic<int> scheduledEvents { 0 };
        std::atomic<int> maxEventsPerBlock { 0 };
        std::atomic<int> clippedSamples { 0 };
        std::atomic<int> emergencyNoteOffsSubmitted { 0 };
        std::atomic<std::int64_t> currentSample { 0 };
        std::atomic<std::int64_t> totalSamples { 0 };
        std::atomic<float> outputPeak { 0.0f };
        mutable juce::CriticalSection messageLock;
        juce::String lastMessage;
    };

    VstLiveDirectPreviewEngine::VstLiveDirectPreviewEngine() = default;

    VstLiveDirectPreviewEngine::~VstLiveDirectPreviewEngine()
    {
        stop();
    }

    double VstLiveDirectPreviewStartResult::durationSeconds() const noexcept
    {
        const auto sr = sampleRate > 0 ? sampleRate : kFallbackSampleRate;
        return totalSamples > 0 ? static_cast<double>(totalSamples) / static_cast<double>(sr) : 0.0;
    }

    juce::String VstLiveDirectPreviewStatus::toStatusMessage() const
    {
        if (!active)
            return "VST3 Live Preview is not active.";

        juce::String text = finished
            ? juce::String("VST3 Live Preview finished.")
            : juce::String("VST3 Live Preview is playing.");
        text << " Position samples " << currentSample << "/" << totalSamples
             << ", blocks " << processedBlocks
             << ", scheduled events " << scheduledEvents
             << ", submitted events " << submittedEvents
             << ", skipped busy blocks " << skippedBusyBlocks;
        if (liveEffectSlotCount > 0)
            text << ", live effect slots " << liveEffectSlotCount
                 << ", effect blocks " << processedEffectBlocks
                 << ", skipped effect busy blocks " << skippedEffectBusyBlocks;
        if (emergencyNoteOffsSubmitted > 0)
            text << ", emergency note-offs " << emergencyNoteOffsSubmitted;
        if (clippedSamples > 0)
            text << ", clipped samples " << clippedSamples;
        text << ", peak " << juce::String(peak, 6) << ".";
        if (hadFailure && lastMessage.isNotEmpty())
            text << " Last callback message: " << lastMessage;
        return text;
    }

    juce::String VstLiveDirectPreviewStopSummary::toLogMessage() const
    {
        if (!wasActive)
            return {};
        juce::String text = hadFailure
            ? juce::String("VST3 Live Preview stopped after callback failure. ")
            : juce::String("VST3 Live Preview stopped. ");
        text << "Position samples " << currentSample << "/" << totalSamples
             << ", blocks " << processedBlocks
             << ", scheduled events " << scheduledEvents
             << ", submitted events " << submittedEvents
             << ", skipped busy blocks " << skippedBusyBlocks;
        if (liveEffectSlotCount > 0)
            text << ", live effect slots " << liveEffectSlotCount
                 << ", effect blocks " << processedEffectBlocks
                 << ", skipped effect busy blocks " << skippedEffectBusyBlocks;
        if (emergencyNoteOffsSubmitted > 0)
            text << ", emergency note-offs " << emergencyNoteOffsSubmitted;
        if (clippedSamples > 0)
            text << ", clipped samples " << clippedSamples;
        text << ", peak " << juce::String(peak, 6) << ".";
        if (hadFailure && lastMessage.isNotEmpty())
            text << " Last callback message: " << lastMessage;
        return text;
    }

    VstLiveDirectPreviewStartResult VstLiveDirectPreviewEngine::start(
        VstLiveDirectPreviewTrackRequest request,
        int outputChannelCount)
    {
        stop();

        VstLiveDirectPreviewStartResult result;
        result.outputChannelCount = sanitizeOutputChannelCount(outputChannelCount);
        auto callback = std::make_unique<DirectPreviewAudioCallback>(std::move(request), result.outputChannelCount);

        result.trackIndex = callback->getTrackIndex();
        result.sampleRate = callback->getSampleRate();
        result.scheduledEvents = callback->getScheduledEvents();
        result.maxEventsPerBlock = callback->getMaxEventsPerBlock();
        result.liveEffectSlotCount = callback->getLiveEffectSlotCount();
        result.totalSamples = callback->getTotalSamples();

        if (result.scheduledEvents <= 0 || result.totalSamples <= 0)
        {
            result.errorMessage = "No VST3 live notes produced playable content.";
            return result;
        }

        const auto initError = deviceManager_.initialise(0, result.outputChannelCount, nullptr, true);
        if (initError.isNotEmpty())
        {
            result.errorMessage = "VST3 live audio device did not open. " + initError;
            deviceManager_.closeAudioDevice();
            return result;
        }

        deviceManager_.addAudioCallback(callback.get());
        result.sampleRate = callback->getSampleRate();
        result.totalSamples = callback->getTotalSamples();
        result.scheduledEvents = callback->getScheduledEvents();
        result.maxEventsPerBlock = callback->getMaxEventsPerBlock();

        if (callback->hasFailed() || result.totalSamples <= 0 || result.scheduledEvents <= 0)
        {
            result.errorMessage = callback->getLastMessage().isNotEmpty()
                ? callback->getLastMessage()
                : juce::String("VST3 live callback could not prepare the selected track.");
            deviceManager_.removeAudioCallback(callback.get());
            deviceManager_.closeAudioDevice();
            return result;
        }

        callback_ = std::move(callback);
        activeTrackIndex_ = result.trackIndex;
        active_ = true;
        result.started = true;
        result.message = "VST3 live selected-track audio callback started.";
        return result;
    }

    VstLiveDirectPreviewStopSummary VstLiveDirectPreviewEngine::stop()
    {
        VstLiveDirectPreviewStopSummary summary;
        summary.wasActive = active_ || callback_ != nullptr;
        summary.trackIndex = activeTrackIndex_;

        if (callback_ != nullptr)
        {
            callback_->flushActiveNotesForStop();
            summary.hadFailure = callback_->hasFailed();
            summary.processedBlocks = callback_->getProcessedBlocks();
            summary.processedEffectBlocks = callback_->getProcessedEffectBlocks();
            summary.submittedEvents = callback_->getSubmittedEvents();
            summary.skippedBusyBlocks = callback_->getSkippedBusyBlocks();
            summary.skippedEffectBusyBlocks = callback_->getSkippedEffectBusyBlocks();
            summary.scheduledEvents = callback_->getScheduledEvents();
            summary.maxEventsPerBlock = callback_->getMaxEventsPerBlock();
            summary.liveEffectSlotCount = callback_->getLiveEffectSlotCount();
            summary.clippedSamples = callback_->getClippedSamples();
            summary.emergencyNoteOffsSubmitted = callback_->getEmergencyNoteOffsSubmitted();
            summary.currentSample = callback_->getCurrentSample();
            summary.totalSamples = callback_->getTotalSamples();
            summary.peak = callback_->getPeak();
            summary.lastMessage = callback_->getLastMessage();
            deviceManager_.removeAudioCallback(callback_.get());
        }

        deviceManager_.closeAudioDevice();
        callback_.reset();
        activeTrackIndex_ = -1;
        active_ = false;
        return summary;
    }

    VstLiveDirectPreviewStatus VstLiveDirectPreviewEngine::status() const
    {
        VstLiveDirectPreviewStatus status;
        status.active = active_ && callback_ != nullptr;
        status.trackIndex = activeTrackIndex_;
        if (callback_ == nullptr)
            return status;

        status.finished = callback_->hasFinished();
        status.hadFailure = callback_->hasFailed();
        status.processedBlocks = callback_->getProcessedBlocks();
        status.processedEffectBlocks = callback_->getProcessedEffectBlocks();
        status.submittedEvents = callback_->getSubmittedEvents();
        status.skippedBusyBlocks = callback_->getSkippedBusyBlocks();
        status.skippedEffectBusyBlocks = callback_->getSkippedEffectBusyBlocks();
        status.scheduledEvents = callback_->getScheduledEvents();
        status.maxEventsPerBlock = callback_->getMaxEventsPerBlock();
        status.liveEffectSlotCount = callback_->getLiveEffectSlotCount();
        status.clippedSamples = callback_->getClippedSamples();
        status.emergencyNoteOffsSubmitted = callback_->getEmergencyNoteOffsSubmitted();
        status.currentSample = callback_->getCurrentSample();
        status.totalSamples = callback_->getTotalSamples();
        status.peak = callback_->getPeak();
        status.lastMessage = callback_->getLastMessage();
        return status;
    }

    bool VstLiveDirectPreviewEngine::isActive() const noexcept
    {
        return active_;
    }

    int VstLiveDirectPreviewEngine::activeTrackIndex() const noexcept
    {
        return activeTrackIndex_;
    }
}
