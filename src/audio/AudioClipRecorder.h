#pragma once

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>

#include <juce_core/juce_core.h>

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include "core/InstrumentAssignment.h"

namespace mw::audio
{
    struct AudioClipRecorderStartResult
    {
        bool success = false;
        std::string message;
        double sampleRate = 48000.0;
        int channelCount = 1;
        bool liveEffectMonitorRequested = false;
        bool liveEffectMonitorActive = false;
        std::string liveEffectMessage;
    };

    struct AudioClipRecorderLiveEffectOptions
    {
        bool enabled = false;
        mw::core::VstPluginAssignment effect;
        std::string trackName;
        std::string unavailableMessage;
    };

    class AudioClipRecorder final : private juce::AudioIODeviceCallback
    {
    public:
        AudioClipRecorder();
        ~AudioClipRecorder() override;

        AudioClipRecorderStartResult startRecording(const std::filesystem::path& outputWavPath,
                                                    int requestedChannels = 1,
                                                    int bitDepth = 24,
                                                    const AudioClipRecorderLiveEffectOptions& liveEffectOptions = {});
        juce::StringArray getAvailableInputDeviceNames();
        void setPreferredInputDeviceName(const juce::String& deviceName);
        juce::String getPreferredInputDeviceName() const { return preferredInputDeviceName; }
        void pause();
        void resume();
        void stop();
        bool isRecording() const { return recording.load(); }
        bool isPaused() const { return paused.load(); }
        double getSampleRate() const { return currentSampleRate; }
        int getChannelCount() const { return channelCount; }
        long long getSamplesWritten() const { return samplesWritten.load(); }
        std::filesystem::path getOutputPath() const { return outputPath; }
        juce::String getCurrentDeviceSummary() const;
        void setInputGainDb(double gainDb);
        double getInputGainDb() const { return static_cast<double>(inputGainDb.load()); }
        bool isLiveEffectMonitorActive() const { return liveEffectMonitorActive.load(); }
        juce::String getLiveEffectMonitorSummary() const { return liveEffectMonitorSummary; }

    private:
        void audioDeviceAboutToStart(juce::AudioIODevice* device) override;
        void audioDeviceStopped() override;
        void audioDeviceIOCallback(const float* const* inputChannelData,
                                   int numInputChannels,
                                   float* const* outputChannelData,
                                   int numOutputChannels,
                                   int numSamples);
        void audioDeviceIOCallbackWithContext(const float* const* inputChannelData,
                                              int numInputChannels,
                                              float* const* outputChannelData,
                                              int numOutputChannels,
                                              int numSamples,
                                              const juce::AudioIODeviceCallbackContext& context);
        void processAudioBlock(const float* const* inputChannelData,
                               int numInputChannels,
                               float* const* outputChannelData,
                               int numOutputChannels,
                               int numSamples);

        void clearLiveEffectMonitor();

        juce::AudioDeviceManager deviceManager;
        juce::TimeSliceThread backgroundThread { "AudioClip Recorder Writer" };
        std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> threadedWriter;
        std::unique_ptr<juce::AudioBuffer<float>> scratchBuffer;
        std::unique_ptr<juce::AudioBuffer<float>> monitorBuffer;
        std::unique_ptr<juce::AudioPluginInstance> liveEffectInstance;
        std::atomic<juce::AudioFormatWriter::ThreadedWriter*> activeWriter { nullptr };
        std::atomic<bool> recording { false };
        std::atomic<bool> paused { false };
        std::atomic<long long> samplesWritten { 0 };
        std::atomic<float> inputGainDb { 0.0f };
        std::atomic<float> inputGainLinear { 1.0f };
        std::atomic<bool> liveEffectMonitorActive { false };
        std::filesystem::path outputPath;
        juce::String preferredInputDeviceName;
        double currentSampleRate = 48000.0;
        int channelCount = 1;
        int bitDepth = 24;
        int liveEffectMonitorChannels = 0;
        juce::String liveEffectMonitorSummary;
    };
}
