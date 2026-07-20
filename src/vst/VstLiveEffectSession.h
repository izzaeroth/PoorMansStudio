#pragma once

#include "core/InstrumentAssignment.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <memory>
#include <string>

namespace mw::vst
{
    struct VstLiveEffectSessionConfig
    {
        mw::core::VstPluginAssignment plugin;
        double sampleRate = 48000.0;
        int channelCount = 2;
        int blockSize = 512;
    };

    struct VstLiveEffectSessionInfo
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

    class VstLiveEffectSession final
    {
    public:
        VstLiveEffectSession();
        ~VstLiveEffectSession();

        VstLiveEffectSession(const VstLiveEffectSession&) = delete;
        VstLiveEffectSession& operator=(const VstLiveEffectSession&) = delete;

        bool open(const VstLiveEffectSessionConfig& config, std::string& errorMessage);
        void close();
        bool isOpen() const noexcept;
        VstLiveEffectSessionInfo info() const;
        bool prepareForPlayback(double sampleRate,
                                int blockSize,
                                int channelCount,
                                std::string& errorMessage) noexcept;

        bool processBlock(juce::AudioBuffer<float>& audio,
                          std::string& errorMessage) noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
