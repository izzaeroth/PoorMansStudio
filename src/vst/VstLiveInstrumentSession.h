#pragma once

#include "core/InstrumentAssignment.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <memory>
#include <string>

namespace mw::vst
{
    struct VstLiveInstrumentSessionConfig
    {
        mw::core::VstPluginAssignment plugin;
        double sampleRate = 48000.0;
        int channelCount = 2;
        int blockSize = 512;
    };

    struct VstLiveInstrumentSessionInfo
    {
        bool open = false;
        std::string pluginName;
        double sampleRate = 0.0;
        int channelCount = 0;
        int blockSize = 0;
        bool stateRestored = false;
        int reportedInputChannels = 0;
        int reportedOutputChannels = 0;
        int latencySamples = 0;
        std::string message;
    };

    class VstLiveInstrumentSession final
    {
    public:
        VstLiveInstrumentSession();
        ~VstLiveInstrumentSession();

        VstLiveInstrumentSession(const VstLiveInstrumentSession&) = delete;
        VstLiveInstrumentSession& operator=(const VstLiveInstrumentSession&) = delete;

        bool open(const VstLiveInstrumentSessionConfig& config, std::string& errorMessage);
        void close();
        bool isOpen() const noexcept;
        VstLiveInstrumentSessionInfo info() const;
        bool prepareForPlayback(double sampleRate,
                                int blockSize,
                                int channelCount,
                                std::string& errorMessage) noexcept;

        bool processBlock(juce::AudioBuffer<float>& audio,
                          juce::MidiBuffer& midi,
                          std::string& errorMessage) noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
