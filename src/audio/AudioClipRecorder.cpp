#include "audio/AudioClipRecorder.h"

#include "clap/ClapLiveEffectSession.h"
#include "vst/VstInstrumentHost.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <system_error>

namespace mw::audio
{
    AudioClipRecorder::AudioClipRecorder()
    {
        backgroundThread.startThread();
    }

    AudioClipRecorder::~AudioClipRecorder()
    {
        stop();
        backgroundThread.stopThread(1500);
    }

    AudioClipRecorderStartResult AudioClipRecorder::startRecording(const std::filesystem::path& outputWavPath,
                                                                     int requestedChannels,
                                                                     int requestedBitDepth,
                                                                     const AudioClipRecorderLiveEffectOptions& liveEffectOptions)
    {
        AudioClipRecorderStartResult result;
        result.recordEffectRequested = liveEffectOptions.writeEffectToOutputFile;
        result.liveEffectMonitorRequested = liveEffectOptions.monitorEnabled;

        stop();

        channelCount = std::clamp(requestedChannels, 1, 2);
        bitDepth = requestedBitDepth == 16 ? 16 : 24;
        outputPath = outputWavPath;
        samplesWritten = 0;
        clearLiveEffectMonitor();
        writeEffectToOutputFile = liveEffectOptions.writeEffectToOutputFile;
        liveEffectMonitorGain = std::isfinite(static_cast<double>(liveEffectOptions.monitorOutputGain))
            ? std::clamp(liveEffectOptions.monitorOutputGain, 0.0f, 8.0f)
            : 1.0f;

        std::error_code ec;
        std::filesystem::create_directories(outputPath.parent_path(), ec);

        const int requestedOutputChannels = liveEffectOptions.monitorEnabled ? 2 : 0;
        auto initError = deviceManager.initialise(channelCount, requestedOutputChannels, nullptr, true, preferredInputDeviceName);
        if (initError.isNotEmpty())
        {
            result.message = initError.toStdString();
            return result;
        }

        if (preferredInputDeviceName.isNotEmpty())
        {
            juce::AudioDeviceManager::AudioDeviceSetup setup;
            deviceManager.getAudioDeviceSetup(setup);
            setup.inputDeviceName = preferredInputDeviceName;
            setup.useDefaultInputChannels = true;
            const auto setupError = deviceManager.setAudioDeviceSetup(setup, true);
            if (setupError.isNotEmpty())
            {
                result.message = setupError.toStdString();
                return result;
            }
        }

        int availableOutputChannels = 0;
        int currentDeviceBlockSize = 512;
        if (auto* device = deviceManager.getCurrentAudioDevice())
        {
            currentSampleRate = device->getCurrentSampleRate();
            if (currentSampleRate <= 0.0)
                currentSampleRate = 48000.0;
            availableOutputChannels = device->getActiveOutputChannels().countNumberOfSetBits();
            currentDeviceBlockSize = juce::jlimit(16, 8192, device->getCurrentBufferSizeSamples());
        }

        const bool effectProcessingRequested = liveEffectOptions.enabled
            && (liveEffectOptions.monitorEnabled || liveEffectOptions.writeEffectToOutputFile);
        if (effectProcessingRequested)
        {
            const auto effectName = liveEffectOptions.effect.name.empty()
                ? liveEffectOptions.effect.bundlePath.filename().string()
                : liveEffectOptions.effect.name;
            const bool monitorRequestedWithOutput = liveEffectOptions.monitorEnabled && availableOutputChannels > 0;

            auto reportUnavailable = [&](const std::string& message)
            {
                if (liveEffectOptions.writeEffectToOutputFile)
                {
                    result.recordEffectMessage = message;
                    recordEffectSummary = message;
                }
                if (liveEffectOptions.monitorEnabled)
                {
                    result.liveEffectMessage = message;
                    liveEffectMonitorSummary = message;
                }
            };

            if (!liveEffectOptions.unavailableMessage.empty())
            {
                reportUnavailable(liveEffectOptions.unavailableMessage);
            }
            else if ((liveEffectOptions.backendType != mw::core::EffectSlotBackendType::VST3
                      && liveEffectOptions.backendType != mw::core::EffectSlotBackendType::CLAP)
                     || !liveEffectOptions.effect.hasPluginIdentity()
                     || liveEffectOptions.effect.bundlePath.empty())
            {
                reportUnavailable("Track Live Effect requested, but the target track does not have a supported VST3 or CLAP Effect Slot assignment.");
            }
            else if (liveEffectOptions.effect.bypassed)
            {
                reportUnavailable("Track Live Effect requested, but the target track effect is bypassed.");
            }
            else
            {
                const int requestedProcessChannels = monitorRequestedWithOutput
                    ? juce::jlimit(1, 2, std::max(channelCount, std::min(2, availableOutputChannels)))
                    : juce::jlimit(1, 2, channelCount);

                bool effectLoaded = false;
                std::string loadMessage;
                if (liveEffectOptions.backendType == mw::core::EffectSlotBackendType::VST3)
                {
                    auto load = mw::vst::VstInstrumentHost::loadPluginAssignment(liveEffectOptions.effect,
                                                                                currentSampleRate,
                                                                                currentDeviceBlockSize,
                                                                                requestedProcessChannels,
                                                                                requestedProcessChannels);
                    if (load.success && load.instance != nullptr)
                    {
                        liveEffectInstance = std::move(load.instance);
                        liveEffectBackend = mw::core::EffectSlotBackendType::VST3;
                        effectLoaded = true;
                    }
                    else
                    {
                        loadMessage = load.message;
                    }
                }
                else
                {
                    auto session = std::make_unique<mw::clap::ClapLiveEffectSession>();
                    mw::clap::ClapLiveEffectSessionConfig config;
                    config.pluginPath = liveEffectOptions.effect.bundlePath;
                    config.pluginUid = liveEffectOptions.effect.uid;
                    config.pluginName = liveEffectOptions.effect.name;
                    config.stateBase64 = liveEffectOptions.effect.stateBase64;
                    config.sampleRate = static_cast<int>(std::lround(currentSampleRate));
                    config.channelCount = requestedProcessChannels;
                    config.blockSize = currentDeviceBlockSize;

                    if (session->open(config, loadMessage))
                    {
                        const auto sessionInfo = session->info();
                        if (sessionInfo.inputChannelCount <= 0)
                        {
                            session->close();
                            loadMessage = "CLAP effect does not expose an audio input port for microphone monitoring.";
                        }
                        else
                        {
                            liveClapEffectSession = std::move(session);
                            liveEffectBackend = mw::core::EffectSlotBackendType::CLAP;
                            effectLoaded = true;
                        }
                    }
                }

                if (effectLoaded)
                {
                    effectProcessChannels = requestedProcessChannels;
                    effectProcessingActive = true;
                    const auto backendName = liveEffectBackend == mw::core::EffectSlotBackendType::CLAP ? "CLAP" : "VST3";

                    if (liveEffectOptions.writeEffectToOutputFile)
                    {
                        result.recordEffectActive = true;
                        result.recordEffectMessage = std::string("Record Test temporary audition will use ") + backendName + ": " + effectName
                            + (liveEffectOptions.trackName.empty() ? std::string() : (" on " + liveEffectOptions.trackName));
                        recordEffectSummary = result.recordEffectMessage;
                    }

                    if (liveEffectOptions.monitorEnabled)
                    {
                        if (monitorRequestedWithOutput)
                        {
                            liveEffectMonitorChannels = requestedProcessChannels;
                            liveEffectMonitorActive = true;
                            result.liveEffectMonitorActive = true;
                            result.liveEffectMessage = std::string("Track Live Effect monitor active using ") + backendName + ": " + effectName
                                + (liveEffectOptions.trackName.empty() ? std::string() : (" on " + liveEffectOptions.trackName));
                            liveEffectMonitorSummary = result.liveEffectMessage;
                        }
                        else
                        {
                            result.liveEffectMessage = "Track Live Effect requested, but no audio output device/channel is available. The project take still records dry.";
                            liveEffectMonitorSummary = result.liveEffectMessage;
                        }
                    }
                }
                else
                {
                    const auto message = "Track Live Effect could not load the assigned Effect Slot; dry capture continues: " + loadMessage;
                    reportUnavailable(message);
                    effectProcessChannels = 0;
                    liveEffectBackend = mw::core::EffectSlotBackendType::None;
                }
            }
        }

        juce::WavAudioFormat wavFormat;
        auto file = juce::File(outputPath.string());
        std::unique_ptr<juce::FileOutputStream> stream(file.createOutputStream());

        if (!stream)
        {
            result.message = "Could not create recording file: " + outputPath.string();
            return result;
        }

        std::unique_ptr<juce::AudioFormatWriter> writer(
            wavFormat.createWriterFor(stream.get(), currentSampleRate, static_cast<unsigned int>(channelCount), bitDepth, {}, 0)
        );

        if (!writer)
        {
            result.message = "Could not create WAV writer for recording.";
            return result;
        }

        stream.release();
        threadedWriter = std::make_unique<juce::AudioFormatWriter::ThreadedWriter>(writer.release(), backgroundThread, 32768);
        scratchBuffer = std::make_unique<juce::AudioBuffer<float>>(channelCount, juce::jmax(4096, currentDeviceBlockSize));
        if (effectProcessingActive.load())
            effectBuffer = std::make_unique<juce::AudioBuffer<float>>(juce::jmax(1, effectProcessChannels), juce::jmax(4096, currentDeviceBlockSize));
        activeWriter = threadedWriter.get();
        paused = false;
        recording = true;
        deviceManager.addAudioCallback(this);

        result.success = true;
        result.message = "Recording started.";
        result.sampleRate = currentSampleRate;
        result.channelCount = channelCount;
        return result;
    }

    juce::StringArray AudioClipRecorder::getAvailableInputDeviceNames()
    {
        juce::StringArray names;

        auto initError = deviceManager.initialise(1, 0, nullptr, true);
        if (initError.isNotEmpty())
            return names;

        auto& types = deviceManager.getAvailableDeviceTypes();
        for (auto* type : types)
        {
            if (type == nullptr)
                continue;

            type->scanForDevices();
            names.addArray(type->getDeviceNames(true));
        }

        names.removeDuplicates(false);
        names.sort(false);
        return names;
    }

    void AudioClipRecorder::setPreferredInputDeviceName(const juce::String& deviceName)
    {
        preferredInputDeviceName = deviceName.trim();

        if (recording.load())
            return;

        auto initError = deviceManager.initialise(preferredInputDeviceName.isNotEmpty() ? 1 : 0, 0, nullptr, true, preferredInputDeviceName);
        if (initError.isNotEmpty())
            return;

        if (preferredInputDeviceName.isNotEmpty())
        {
            juce::AudioDeviceManager::AudioDeviceSetup setup;
            deviceManager.getAudioDeviceSetup(setup);
            setup.inputDeviceName = preferredInputDeviceName;
            setup.useDefaultInputChannels = true;
            deviceManager.setAudioDeviceSetup(setup, true);
        }
    }

    void AudioClipRecorder::pause()
    {
        paused = true;
    }

    void AudioClipRecorder::resume()
    {
        paused = false;
    }

    void AudioClipRecorder::stop()
    {
        if (recording.load())
            deviceManager.removeAudioCallback(this);

        recording = false;
        paused = false;
        activeWriter = nullptr;
        threadedWriter.reset();
        scratchBuffer.reset();
        effectBuffer.reset();
        clearLiveEffectMonitor();
    }

    void AudioClipRecorder::clearLiveEffectMonitor()
    {
        liveEffectMonitorActive = false;
        effectProcessingActive = false;
        writeEffectToOutputFile = false;
        liveEffectMonitorChannels = 0;
        effectProcessChannels = 0;
        liveEffectMonitorGain = 1.0f;
        if (liveEffectInstance)
        {
            liveEffectInstance->releaseResources();
            liveEffectInstance.reset();
        }
        if (liveClapEffectSession)
        {
            liveClapEffectSession->close();
            liveClapEffectSession.reset();
        }
        liveEffectBackend = mw::core::EffectSlotBackendType::None;
        liveEffectMonitorSummary.clear();
        recordEffectSummary.clear();
    }

    juce::String AudioClipRecorder::getCurrentDeviceSummary() const
    {
        if (auto* device = deviceManager.getCurrentAudioDevice())
            return device->getName() + " @ " + juce::String(device->getCurrentSampleRate(), 0) + " Hz";

        return preferredInputDeviceName.isNotEmpty() ? preferredInputDeviceName : juce::String("Default audio input");
    }

    void AudioClipRecorder::setInputGainDb(double gainDb)
    {
        const auto limitedDb = juce::jlimit(-24.0, 24.0, gainDb);
        inputGainDb.store(static_cast<float>(limitedDb));
        inputGainLinear.store(static_cast<float>(std::pow(10.0, limitedDb / 20.0)));
    }

    void AudioClipRecorder::audioDeviceAboutToStart(juce::AudioIODevice* device)
    {
        if (device != nullptr)
        {
            currentSampleRate = device->getCurrentSampleRate();
            if (currentSampleRate <= 0.0)
                currentSampleRate = 48000.0;
        }
    }

    void AudioClipRecorder::audioDeviceStopped()
    {
    }

    void AudioClipRecorder::audioDeviceIOCallback(const float* const* inputChannelData,
                                                  int numInputChannels,
                                                  float* const* outputChannelData,
                                                  int numOutputChannels,
                                                  int numSamples)
    {
        processAudioBlock(inputChannelData, numInputChannels, outputChannelData, numOutputChannels, numSamples);
    }

    void AudioClipRecorder::audioDeviceIOCallbackWithContext(const float* const* inputChannelData,
                                                             int numInputChannels,
                                                             float* const* outputChannelData,
                                                             int numOutputChannels,
                                                             int numSamples,
                                                             const juce::AudioIODeviceCallbackContext&)
    {
        processAudioBlock(inputChannelData, numInputChannels, outputChannelData, numOutputChannels, numSamples);
    }

    void AudioClipRecorder::processAudioBlock(const float* const* inputChannelData,
                                               int numInputChannels,
                                               float* const* outputChannelData,
                                               int numOutputChannels,
                                               int numSamples)
    {
        for (int channel = 0; channel < numOutputChannels; ++channel)
            if (outputChannelData[channel] != nullptr)
                juce::FloatVectorOperations::clear(outputChannelData[channel], numSamples);

        auto* writer = activeWriter.load();
        if (writer == nullptr || !recording.load() || paused.load())
            return;

        if (!scratchBuffer || scratchBuffer->getNumSamples() < numSamples || scratchBuffer->getNumChannels() < channelCount)
            return;

        const auto gain = inputGainLinear.load();
        for (int ch = 0; ch < channelCount; ++ch)
        {
            auto* dest = scratchBuffer->getWritePointer(ch);
            if (ch < numInputChannels && inputChannelData[ch] != nullptr)
                juce::FloatVectorOperations::copy(dest, inputChannelData[ch], numSamples);
            else if (numInputChannels > 0 && inputChannelData[0] != nullptr)
                juce::FloatVectorOperations::copy(dest, inputChannelData[0], numSamples);
            else
                juce::FloatVectorOperations::clear(dest, numSamples);

            if (gain != 1.0f)
                juce::FloatVectorOperations::multiply(dest, gain, numSamples);
        }

        const float* const* writerData = scratchBuffer->getArrayOfReadPointers();
        if (effectProcessingActive.load() && effectBuffer
            && effectBuffer->getNumSamples() >= numSamples)
        {
            const int processChannels = juce::jlimit(1, effectBuffer->getNumChannels(), juce::jmax(1, effectProcessChannels));
            effectBuffer->setSize(processChannels, numSamples, false, false, true);
            effectBuffer->clear(0, numSamples);
            for (int ch = 0; ch < processChannels; ++ch)
            {
                auto* dest = effectBuffer->getWritePointer(ch);
                if (ch < channelCount)
                    juce::FloatVectorOperations::copy(dest, scratchBuffer->getReadPointer(ch), numSamples);
                else if (channelCount > 0)
                    juce::FloatVectorOperations::copy(dest, scratchBuffer->getReadPointer(0), numSamples);
                else
                    juce::FloatVectorOperations::clear(dest, numSamples);
            }

            bool processingSucceeded = false;
            try
            {
                if (liveEffectBackend == mw::core::EffectSlotBackendType::VST3 && liveEffectInstance)
                {
                    juce::MidiBuffer emptyMidi;
                    liveEffectInstance->processBlock(*effectBuffer, emptyMidi);
                    processingSucceeded = true;
                }
                else if (liveEffectBackend == mw::core::EffectSlotBackendType::CLAP && liveClapEffectSession)
                {
                    const auto clapResult = liveClapEffectSession->processPlanarBlock(effectBuffer->getArrayOfReadPointers(),
                                                                                     processChannels,
                                                                                     effectBuffer->getArrayOfWritePointers(),
                                                                                     processChannels,
                                                                                     numSamples);
                    processingSucceeded = clapResult.success;
                }
            }
            catch (...)
            {
                processingSucceeded = false;
            }

            if (processingSucceeded)
            {
                if (writeEffectToOutputFile.load())
                    writerData = effectBuffer->getArrayOfReadPointers();

                if (liveEffectMonitorActive.load() && numOutputChannels > 0)
                {
                    const int monitorChannels = juce::jlimit(1, processChannels, juce::jmax(1, liveEffectMonitorChannels));
                    for (int ch = 0; ch < numOutputChannels; ++ch)
                    {
                        if (outputChannelData[ch] == nullptr)
                            continue;

                        if (ch < monitorChannels)
                        {
                            juce::FloatVectorOperations::copy(outputChannelData[ch], effectBuffer->getReadPointer(ch), numSamples);
                            const auto monitorGain = liveEffectMonitorGain.load();
                            if (monitorGain != 1.0f)
                                juce::FloatVectorOperations::multiply(outputChannelData[ch], monitorGain, numSamples);
                        }
                        else
                        {
                            juce::FloatVectorOperations::clear(outputChannelData[ch], numSamples);
                        }
                    }
                }
            }
            else
            {
                effectProcessingActive = false;
                liveEffectMonitorActive = false;
                recordEffectSummary = "Record Test effect failed; temporary playback uses the dry capture.";
                liveEffectMonitorSummary = "Track Live Effect failed during monitoring; the project take continued recording dry.";
                for (int ch = 0; ch < numOutputChannels; ++ch)
                    if (outputChannelData[ch] != nullptr)
                        juce::FloatVectorOperations::clear(outputChannelData[ch], numSamples);
            }
        }

        writer->write(writerData, numSamples);
        samplesWritten += numSamples;
    }
}

