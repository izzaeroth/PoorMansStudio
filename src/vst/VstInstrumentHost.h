#pragma once

#include "core/Track.h"
#include "vst/VstPluginTypes.h"

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace mw::vst
{
    struct VstLoadResult
    {
        bool success = false;
        std::string message;
        std::unique_ptr<juce::AudioPluginInstance> instance;
        juce::PluginDescription description;
    };

    struct VstRenderRequest
    {
        mw::core::Track track;
        int tempoBpm = 120;
        int sampleRate = 48000;
        int channelCount = 2;
        int blockSize = 512;
        std::filesystem::path wavOutputPath;
        std::atomic<bool>* cancelRequested = nullptr;
    };

    struct VstRenderResult
    {
        bool success = false;
        bool cancelled = false;
        std::string message;
        std::filesystem::path wavPath;
    };

    class VstInstrumentHost
    {
    public:
        static VstLoadResult loadInstrumentForTrack(const mw::core::Track& track, double sampleRate, int blockSize);
        static VstRenderResult renderTrackToWav(const VstRenderRequest& request);
        static bool trackHasVstPlugin(const mw::core::Track& track);
        static juce::MidiBuffer buildMidiForBlock(const mw::core::Track& track, std::int64_t blockStartSample, int blockNumSamples, double samplesPerTick);
    };
}
