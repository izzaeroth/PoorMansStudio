#include "audio/AudioClipRecorder.h"

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

    AudioClipRecorderStartResult AudioClipRecorder::startRecording(const std::filesystem::path& outputWavPath, int requestedChannels, int requestedBitDepth)
    {
        AudioClipRecorderStartResult result;

        stop();

        channelCount = std::clamp(requestedChannels, 1, 2);
        bitDepth = requestedBitDepth == 16 ? 16 : 24;
        outputPath = outputWavPath;
        samplesWritten = 0;

        std::error_code ec;
        std::filesystem::create_directories(outputPath.parent_path(), ec);

        auto initError = deviceManager.initialise(channelCount, 0, nullptr, true, preferredInputDeviceName);
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

        if (auto* device = deviceManager.getCurrentAudioDevice())
        {
            currentSampleRate = device->getCurrentSampleRate();
            if (currentSampleRate <= 0.0)
                currentSampleRate = 48000.0;
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

        writer->write(scratchBuffer->getArrayOfReadPointers(), numSamples);
        samplesWritten += numSamples;
    }
}
