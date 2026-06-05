#include "audio/AudioClipRecorder.h"

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
        result.recordEffectRequested = liveEffectOptions.enabled;
        result.liveEffectMonitorRequested = liveEffectOptions.monitorEnabled;

        stop();

        channelCount = std::clamp(requestedChannels, 1, 2);
        bitDepth = requestedBitDepth == 16 ? 16 : 24;
        outputPath = outputWavPath;
        samplesWritten = 0;
        clearLiveEffectMonitor();

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
        if (auto* device = deviceManager.getCurrentAudioDevice())
        {
            currentSampleRate = device->getCurrentSampleRate();
            if (currentSampleRate <= 0.0)
                currentSampleRate = 48000.0;
            availableOutputChannels = device->getActiveOutputChannels().countNumberOfSetBits();
        }

        if (liveEffectOptions.enabled)
        {
            const auto effectName = liveEffectOptions.effect.name.empty()
                ? liveEffectOptions.effect.bundlePath.filename().string()
                : liveEffectOptions.effect.name;

            const bool monitorRequestedWithOutput = liveEffectOptions.monitorEnabled && availableOutputChannels > 0;

            if (!liveEffectOptions.unavailableMessage.empty())
            {
                result.recordEffectMessage = liveEffectOptions.unavailableMessage;
                recordEffectSummary = result.recordEffectMessage;
                if (liveEffectOptions.monitorEnabled)
                {
                    result.liveEffectMessage = liveEffectOptions.unavailableMessage;
                    liveEffectMonitorSummary = result.liveEffectMessage;
                }
            }
            else if (!liveEffectOptions.effect.hasPluginIdentity() || liveEffectOptions.effect.bundlePath.empty())
            {
                result.recordEffectMessage = "Track effect recording requested, but the target track does not have a VST3 effect assignment.";
                recordEffectSummary = result.recordEffectMessage;
                if (liveEffectOptions.monitorEnabled)
                {
                    result.liveEffectMessage = "Track Live Effect requested, but the target track does not have a VST3 effect assignment.";
                    liveEffectMonitorSummary = result.liveEffectMessage;
                }
            }
            else if (liveEffectOptions.effect.bypassed)
            {
                result.recordEffectMessage = "Track effect recording requested, but the target track effect is bypassed.";
                recordEffectSummary = result.recordEffectMessage;
                if (liveEffectOptions.monitorEnabled)
                {
                    result.liveEffectMessage = "Track Live Effect requested, but the target track effect is bypassed.";
                    liveEffectMonitorSummary = result.liveEffectMessage;
                }
            }
            else
            {
                const int requestedProcessChannels = monitorRequestedWithOutput
                    ? juce::jlimit(1, 2, std::max(channelCount, std::min(2, availableOutputChannels)))
                    : juce::jlimit(1, 2, channelCount);

                auto load = mw::vst::VstInstrumentHost::loadPluginAssignment(liveEffectOptions.effect, currentSampleRate, 512, requestedProcessChannels, requestedProcessChannels);
                if (load.success && load.instance != nullptr)
                {
                    liveEffectInstance = std::move(load.instance);
                    recordEffectProcessChannels = requestedProcessChannels;
                    recordEffectActive = true;
                    result.recordEffectActive = true;
                    result.recordEffectMessage = "Track VST effect will be printed into the recorded take: " + effectName
                        + (liveEffectOptions.trackName.empty() ? std::string() : (" on " + liveEffectOptions.trackName));
                    recordEffectSummary = result.recordEffectMessage;

                    if (liveEffectOptions.monitorEnabled)
                    {
                        if (monitorRequestedWithOutput)
                        {
                            liveEffectMonitorChannels = requestedProcessChannels;
                            liveEffectMonitorActive = true;
                            result.liveEffectMonitorActive = true;
                            result.liveEffectMessage = "Track Live Effect monitor active: " + effectName
                                + (liveEffectOptions.trackName.empty() ? std::string() : (" on " + liveEffectOptions.trackName));
                            liveEffectMonitorSummary = result.liveEffectMessage;
                        }
                        else
                        {
                            result.liveEffectMessage = "Track Live Effect requested, but no audio output device/channel is available for monitoring. Wet recording continues.";
                            liveEffectMonitorSummary = result.liveEffectMessage;
                        }
                    }
                }
                else
                {
                    result.recordEffectMessage = "Track VST effect could not load for wet recording; dry recording continues: " + load.message;
                    recordEffectSummary = result.recordEffectMessage;
                    recordEffectProcessChannels = 0;
                    if (liveEffectOptions.monitorEnabled)
                    {
                        result.liveEffectMessage = "Track Live Effect could not load the VST3 effect for live monitoring: " + load.message;
                        liveEffectMonitorSummary = result.liveEffectMessage;
                        liveEffectMonitorChannels = 0;
                    }
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
        scratchBuffer = std::make_unique<juce::AudioBuffer<float>>(channelCount, 4096);
        if (recordEffectActive.load())
            effectBuffer = std::make_unique<juce::AudioBuffer<float>>(juce::jmax(1, recordEffectProcessChannels), 4096);
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
        recordEffectActive = false;
        liveEffectMonitorChannels = 0;
        recordEffectProcessChannels = 0;
        if (liveEffectInstance)
        {
            liveEffectInstance->releaseResources();
            liveEffectInstance.reset();
        }
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

        if (recordEffectActive.load() && liveEffectInstance && effectBuffer
            && effectBuffer->getNumSamples() >= numSamples)
        {
            const int processChannels = juce::jlimit(1, effectBuffer->getNumChannels(), juce::jmax(1, recordEffectProcessChannels));
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

            juce::MidiBuffer emptyMidi;
            try
            {
                liveEffectInstance->processBlock(*effectBuffer, emptyMidi);

                for (int ch = 0; ch < channelCount; ++ch)
                {
                    if (ch < processChannels)
                        juce::FloatVectorOperations::copy(scratchBuffer->getWritePointer(ch), effectBuffer->getReadPointer(ch), numSamples);
                    else if (processChannels > 0)
                        juce::FloatVectorOperations::copy(scratchBuffer->getWritePointer(ch), effectBuffer->getReadPointer(0), numSamples);
                }

                if (liveEffectMonitorActive.load() && numOutputChannels > 0)
                {
                    const int monitorChannels = juce::jlimit(1, processChannels, juce::jmax(1, liveEffectMonitorChannels));
                    for (int ch = 0; ch < numOutputChannels; ++ch)
                    {
                        if (outputChannelData[ch] == nullptr)
                            continue;

                        if (ch < monitorChannels)
                            juce::FloatVectorOperations::copy(outputChannelData[ch], effectBuffer->getReadPointer(ch), numSamples);
                        else
                            juce::FloatVectorOperations::clear(outputChannelData[ch], numSamples);
                    }
                }
            }
            catch (...)
            {
                recordEffectActive = false;
                liveEffectMonitorActive = false;
                recordEffectSummary = "Track VST effect failed while printing into the take; dry recording continued.";
                liveEffectMonitorSummary = "Track Live Effect failed during monitoring; dry recording continued.";
                for (int ch = 0; ch < numOutputChannels; ++ch)
                    if (outputChannelData[ch] != nullptr)
                        juce::FloatVectorOperations::clear(outputChannelData[ch], numSamples);
            }
        }

        writer->write(scratchBuffer->getArrayOfReadPointers(), numSamples);
        samplesWritten += numSamples;
    }
}
