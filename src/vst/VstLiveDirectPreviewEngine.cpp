#include "vst/VstLiveDirectPreviewEngine.h"
#include "midi/LiveMidiNoteQueue.h"
#include "playback/RealtimeAudioSafety.h"

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
        constexpr int kMaximumQueuedLiveMidiEventsPerBlock = 128;
        constexpr int kMaximumPanicNoteOffEvents = 16 * 128;
        constexpr int kMaximumLiveMidiEventsPerBlock = kMaximumQueuedLiveMidiEventsPerBlock + kMaximumPanicNoteOffEvents;
        constexpr std::size_t kMaximumCallbackMidiEvents = 4096;

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
            liveMidiRequest.noteEvents.reserve(static_cast<std::size_t>(kMaximumLiveMidiEventsPerBlock));
            liveMidiRequest.controllerEvents.reserve(static_cast<std::size_t>(kMaximumLiveMidiEventsPerBlock));
            liveMidiRequest.pitchBendEvents.reserve(static_cast<std::size_t>(kMaximumLiveMidiEventsPerBlock));
            liveMidiRequest.programChangeEvents.reserve(static_cast<std::size_t>(kMaximumLiveMidiEventsPerBlock));
            liveMidiRequest.channelPressureEvents.reserve(static_cast<std::size_t>(kMaximumLiveMidiEventsPerBlock));
            liveMidiRequest.polyPressureEvents.reserve(static_cast<std::size_t>(kMaximumLiveMidiEventsPerBlock));
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
            callbackFault.reset();
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
        bool isLiveMidiInput() const noexcept { return request.liveMidiInput; }
        bool submitLiveMidiMessage(const juce::MidiMessage& message) noexcept
        {
            if (!request.liveMidiInput)
                return false;
            const bool queued = liveMidiQueue.push(message);
            if (!queued && (message.isNoteOn() || message.isNoteOff()))
                liveMidiPanicRequested.store(true, std::memory_order_release);
            return queued;
        }
        void requestLiveMidiPanic() noexcept
        {
            if (request.liveMidiInput)
                liveMidiPanicRequested.store(true, std::memory_order_release);
        }
        int getClippedSamples() const noexcept { return clippedSamples.load(); }
        int getEmergencyNoteOffsSubmitted() const noexcept { return emergencyNoteOffsSubmitted.load(); }
        std::int64_t getCurrentSample() const noexcept { return currentSample.load(); }
        std::int64_t getTotalSamples() const noexcept { return totalSamples.load(); }
        float getPeak() const noexcept { return outputPeak.load(); }
        juce::String getLastMessage() const
        {
            const auto fault = callbackFault.read();
            if (fault != mw::playback::DirectPreviewFault::None)
                return juce::String(mw::playback::directPreviewFaultText(fault));
            const juce::ScopedLock lock(messageLock); // Preparation/stop text only.
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

            if (request.liveMidiInput)
            {
                prepared = true;
                scheduledEvents.store(0);
                maxEventsPerBlock.store(0);
                currentSample.store(0);
                totalSamples.store(0);
            }
            else
            {
                bridge.prepare(request.notes, request.controllers, request.pitchBends, request.programChanges, request.channelPressures, request.polyPressures, request.controller14Bits, request.rpns, request.nrpns, request.config);
                prepared = bridge.isPrepared();
                const auto state = bridge.state();
                scheduledEvents.store(state.totalScheduledEvents);
                maxEventsPerBlock.store(state.maxEventsPerBlock);
                currentSample.store(0);
                totalSamples.store(state.totalSamples);
            }

            processBuffer.setSize(requestedOutputChannels,
                                  kMaximumBlockSize,
                                  false,
                                  true,
                                  false);
            processBuffer.clear();
            midi.ensureSize(kMaximumCallbackMidiEvents * 16u); // Before any audio callback.
            clearActiveNotes();
            sampleRate.store(deviceSampleRate);
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

        void failRealtime(mw::playback::DirectPreviewFault code) noexcept
        {
            callbackFault.publish(code);
            failed.store(true);
            finished.store(true);
        }

        bool processEffects()
        {
            for (auto& effect : request.effects)
            {
                if (effect.session == nullptr || effect.sessionProcessMutex == nullptr)
                {
                    callbackFault.publish(mw::playback::DirectPreviewFault::VstEffectMissing);
                    return false;
                }

                std::unique_lock<std::mutex> effectLock(*effect.sessionProcessMutex, std::try_to_lock);
                if (!effectLock.owns_lock())
                {
                    skippedEffectBusyBlocks.fetch_add(1);
                    callbackFault.publish(mw::playback::DirectPreviewFault::VstEffectBusy);
                    return false;
                }

                if (!effect.session->processBlockRealtime(processBuffer))
                {
                    callbackFault.publish(mw::playback::DirectPreviewFault::VstEffect);
                    return false;
                }

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
                failRealtime(mw::playback::DirectPreviewFault::VstInvalidBlock);
                return;
            }

            std::unique_lock<std::mutex> sessionLock(*request.sessionProcessMutex, std::try_to_lock);
            if (!sessionLock.owns_lock())
            {
                skippedBusyBlocks.fetch_add(1);
                ++consecutiveBusyBlocks;
                if (consecutiveBusyBlocks >= kMaximumConsecutiveBusyBlocks)
                {
                    failRealtime(mw::playback::DirectPreviewFault::VstBusy);
                }
                return;
            }
            consecutiveBusyBlocks = 0;

            const mw::clap::ClapLiveProcessRequest* blockRequestPointer = nullptr;
            bool panicThisBlock = false;
            int panicNoteOffCount = 0;

            if (request.liveMidiInput)
            {
                liveMidiRequest.frameCount = numSamples;
                liveMidiRequest.noteEvents.clear();
                liveMidiRequest.controllerEvents.clear();
                liveMidiRequest.pitchBendEvents.clear();
                liveMidiRequest.programChangeEvents.clear();
                liveMidiRequest.channelPressureEvents.clear();
                liveMidiRequest.polyPressureEvents.clear();
                std::array<mw::midi::LiveMidiNoteEvent, kMaximumQueuedLiveMidiEventsPerBlock> queuedEvents {};
                const int eventCount = liveMidiQueue.pop(queuedEvents.data(), static_cast<int>(queuedEvents.size()));
                for (int index = 0; index < eventCount; ++index)
                {
                    const auto& queued = queuedEvents[static_cast<std::size_t>(index)];
                    if (queued.type == mw::midi::LiveMidiEventType::Controller)
                    {
                        mw::clap::ClapLiveControllerEvent event;
                        event.frameOffset = 0; event.midiChannel = queued.channel;
                        event.controller = queued.controller; event.value = queued.controllerValue;
                        liveMidiRequest.controllerEvents.push_back(event);
                    }
                    else if (queued.type == mw::midi::LiveMidiEventType::PitchBend)
                    {
                        mw::clap::ClapLivePitchBendEvent event;
                        event.frameOffset = 0; event.midiChannel = queued.channel; event.value = queued.pitchBendValue;
                        liveMidiRequest.pitchBendEvents.push_back(event);
                    }
                    else if (queued.type == mw::midi::LiveMidiEventType::ProgramChange)
                    {
                        mw::clap::ClapLiveProgramChangeEvent event;
                        event.frameOffset = 0; event.midiChannel = queued.channel; event.program = queued.program;
                        liveMidiRequest.programChangeEvents.push_back(event);
                    }
                    else if (queued.type == mw::midi::LiveMidiEventType::ChannelPressure)
                    {
                        mw::clap::ClapLiveChannelPressureEvent event;
                        event.frameOffset = 0; event.midiChannel = queued.channel; event.pressure = queued.channelPressure;
                        liveMidiRequest.channelPressureEvents.push_back(event);
                    }
                    else if (queued.type == mw::midi::LiveMidiEventType::PolyPressure)
                    {
                        mw::clap::ClapLivePolyPressureEvent event;
                        event.frameOffset = 0; event.midiChannel = queued.channel; event.noteNumber = queued.key; event.pressure = queued.polyPressure;
                        liveMidiRequest.polyPressureEvents.push_back(event);
                    }
                    else
                    {
                        mw::clap::ClapLiveNoteEvent event;
                        event.type = queued.type == mw::midi::LiveMidiEventType::NoteOn
                            ? mw::clap::ClapLiveNoteEventType::NoteOn
                            : mw::clap::ClapLiveNoteEventType::NoteOff;
                        event.frameOffset = 0; event.key = queued.key; event.velocity = queued.velocity; event.midiChannel = queued.channel;
                        liveMidiRequest.noteEvents.push_back(event);
                    }
                }

                panicThisBlock = liveMidiPanicRequested.exchange(false, std::memory_order_acq_rel);
                if (panicThisBlock)
                {
                    // One explicit Note Off per active key plus MIDI All Notes Off below is
                    // sufficient for emergency release and keeps the callback work bounded.
                    for (int channel = 0; channel < 16; ++channel)
                    {
                        for (int key = 0; key < 128; ++key)
                        {
                            const auto depth = activeNoteDepths[static_cast<std::size_t>(channel)][static_cast<std::size_t>(key)];
                            if (depth <= 0)
                                continue;

                            mw::clap::ClapLiveNoteEvent event;
                            event.type = mw::clap::ClapLiveNoteEventType::NoteOff;
                            event.frameOffset = 0;
                            event.key = key;
                            event.velocity = 0;
                            event.midiChannel = channel + 1;
                            liveMidiRequest.noteEvents.push_back(event);
                            ++panicNoteOffCount;
                        }
                    }
                }
                blockRequestPointer = &liveMidiRequest;
            }
            else
            {
                if (bridge.isFinished())
                {
                    finished.store(true);
                    return;
                }

                const auto& scheduledBlockRequest = bridge.nextRequest();
                if (scheduledBlockRequest.frameCount <= 0)
                {
                    finished.store(true);
                    return;
                }
                blockRequestPointer = &scheduledBlockRequest;
            }

            const auto& blockRequest = *blockRequestPointer;
            if (blockRequest.frameCount > numSamples)
            {
                failRealtime(mw::playback::DirectPreviewFault::VstScheduler);
                return;
            }

            const auto midiEventCount = blockRequest.controllerEvents.size()
                + blockRequest.programChangeEvents.size() + blockRequest.pitchBendEvents.size()
                + blockRequest.channelPressureEvents.size() + blockRequest.noteEvents.size()
                + blockRequest.polyPressureEvents.size() + (panicThisBlock ? 16u : 0u);
            if (!mw::playback::realtimeCallbackEventCountAllows(midiEventCount, kMaximumCallbackMidiEvents))
            {
                failRealtime(mw::playback::DirectPreviewFault::VstScheduler);
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

            for (const auto& event : blockRequest.controllerEvents)
            {
                const auto channel = std::clamp(event.midiChannel, 1, 16);
                const auto controller = std::clamp(event.controller, 0, 127);
                const auto value = std::clamp(event.value, 0, 127);
                const auto offset = std::clamp(static_cast<int>(event.frameOffset), 0, framesToProcess - 1);
                midi.addEvent(juce::MidiMessage::controllerEvent(channel, controller, value), offset);
            }

            for (const auto& event : blockRequest.programChangeEvents)
            {
                const int channel = juce::jlimit(1, 16, event.midiChannel);
                const int program = juce::jlimit(0, 127, event.program);
                const auto offset = std::clamp(static_cast<int>(event.frameOffset), 0, framesToProcess - 1);
                midi.addEvent(juce::MidiMessage::programChange(channel, program), offset);
            }

            for (const auto& event : blockRequest.pitchBendEvents)
            {
                const auto channel = std::clamp(event.midiChannel, 1, 16);
                const auto raw = std::clamp(event.value + 8192, 0, 16383);
                const auto offset = std::clamp(static_cast<int>(event.frameOffset), 0, framesToProcess - 1);
                midi.addEvent(juce::MidiMessage::pitchWheel(channel, raw), offset);
            }

            for (const auto& event : blockRequest.channelPressureEvents)
            {
                const auto channel = std::clamp(event.midiChannel, 1, 16);
                const auto pressure = std::clamp(event.pressure, 0, 127);
                const auto offset = std::clamp(static_cast<int>(event.frameOffset), 0, framesToProcess - 1);
                midi.addEvent(juce::MidiMessage::channelPressureChange(channel, pressure), offset);
            }

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
            if (panicThisBlock)
                for (int channel = 1; channel <= 16; ++channel)
                    midi.addEvent(juce::MidiMessage::allNotesOff(channel), 0);

            for (const auto& event : blockRequest.polyPressureEvents)
            {
                const auto channel = std::clamp(event.midiChannel, 1, 16);
                const auto key = std::clamp(event.noteNumber, 0, 127);
                const auto pressure = std::clamp(event.pressure, 0, 127);
                const auto offset = std::clamp(static_cast<int>(event.frameOffset), 0, framesToProcess - 1);
                midi.addEvent(juce::MidiMessage::aftertouchChange(channel, key, pressure), offset);
            }

            if (!request.session->processBlockRealtime(processBuffer, midi))
            {
                failRealtime(mw::playback::DirectPreviewFault::VstInstrument);
                return;
            }

            updateActiveNotes(blockRequest);
            if (panicThisBlock)
            {
                emergencyNoteOffsSubmitted.fetch_add(panicNoteOffCount);
                clearActiveNotes();
            }
            submittedEvents.fetch_add(static_cast<int>(blockRequest.noteEvents.size() + blockRequest.controllerEvents.size() + blockRequest.programChangeEvents.size() + blockRequest.pitchBendEvents.size() + blockRequest.channelPressureEvents.size() + blockRequest.polyPressureEvents.size()));

            if (!processEffects())
            {
                failRealtime(mw::playback::DirectPreviewFault::VstEffect);
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
            if (request.liveMidiInput)
                currentSample.fetch_add(framesToProcess);
            else
            {
                currentSample.store(bridge.state().currentSample);
                if (bridge.isFinished())
                    finished.store(true);
            }
        }

        VstLiveDirectPreviewTrackRequest request;
        int requestedOutputChannels = 2;
        int configuredBlockSize = 512;
        mw::clap::ClapLiveCallbackBridge bridge;
        mw::midi::LiveMidiNoteQueue liveMidiQueue;
        mw::clap::ClapLiveProcessRequest liveMidiRequest;
        std::atomic<bool> liveMidiPanicRequested { false };
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
        mw::playback::DirectPreviewFaultLatch callbackFault;
        mutable juce::CriticalSection messageLock;
        juce::String lastMessage;
    };

    VstLiveDirectPreviewEngine::VstLiveDirectPreviewEngine(mw::audio::ApplicationAudioService& audioService)
        : audioService_(audioService)
    {
    }

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
        const bool liveMidiInput = callback->isLiveMidiInput();
        result.sampleRate = callback->getSampleRate();
        result.scheduledEvents = callback->getScheduledEvents();
        result.maxEventsPerBlock = callback->getMaxEventsPerBlock();
        result.liveEffectSlotCount = callback->getLiveEffectSlotCount();
        result.totalSamples = callback->getTotalSamples();

        if (!liveMidiInput && (result.scheduledEvents <= 0 || result.totalSamples <= 0))
        {
            result.errorMessage = "No VST3 live notes produced playable content.";
            return result;
        }

        const auto owner = liveMidiInput
            ? mw::audio::ApplicationAudioPlaybackOwner::LiveMidiMonitoring
            : mw::audio::ApplicationAudioPlaybackOwner::VstLiveSelectedTrackPreview;
        const auto activation = audioService_.activateOutputCallback(owner, callback.get());
        if (!activation.success)
        {
            result.errorMessage = "VST3 live audio device did not open. " + activation.message;
            return result;
        }
        result.sampleRate = callback->getSampleRate();
        result.totalSamples = callback->getTotalSamples();
        result.scheduledEvents = callback->getScheduledEvents();
        result.maxEventsPerBlock = callback->getMaxEventsPerBlock();

        if (callback->hasFailed()
            || (!liveMidiInput && (result.totalSamples <= 0 || result.scheduledEvents <= 0)))
        {
            result.errorMessage = callback->getLastMessage().isNotEmpty()
                ? callback->getLastMessage()
                : juce::String("VST3 live callback could not prepare the selected track.");
            audioService_.deactivateOutputCallback(owner, callback.get());
            return result;
        }

        callback_ = std::move(callback);
        activeTrackIndex_ = result.trackIndex;
        activeOwner_ = owner;
        active_ = true;
        result.started = true;
        result.message = liveMidiInput
            ? juce::String("VST3 live MIDI monitoring audio callback started.")
            : juce::String("VST3 live selected-track audio callback started.");
        return result;
    }

    VstLiveDirectPreviewStopSummary VstLiveDirectPreviewEngine::stop()
    {
        const std::scoped_lock callbackLock(callbackAccessMutex_);
        VstLiveDirectPreviewStopSummary summary;
        summary.wasActive = active_ || callback_ != nullptr;
        summary.trackIndex = activeTrackIndex_;

        if (callback_ != nullptr)
        {
            // Detach from the hardware callback before inspecting/releasing active notes.
            // This prevents the audio thread from mutating the live-note state while the
            // stop flush is being constructed.
            audioService_.deactivateOutputCallback(activeOwner_, callback_.get());
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
        }

        callback_.reset();
        activeTrackIndex_ = -1;
        activeOwner_ = mw::audio::ApplicationAudioPlaybackOwner::None;
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

    bool VstLiveDirectPreviewEngine::submitLiveMidiMessage(const juce::MidiMessage& message) noexcept
    {
        const std::scoped_lock callbackLock(callbackAccessMutex_);
        return callback_ != nullptr && callback_->submitLiveMidiMessage(message);
    }

    void VstLiveDirectPreviewEngine::requestLiveMidiPanic() noexcept
    {
        const std::scoped_lock callbackLock(callbackAccessMutex_);
        if (callback_ != nullptr)
            callback_->requestLiveMidiPanic();
    }
}
