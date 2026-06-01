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
        bool savedStateApplied = false;
        bool savedStateRestoreFailed = false;
        std::string savedStateMessage;
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

    struct VstEffectProcessRequest
    {
        mw::core::Track track;
        std::filesystem::path inputWavPath;
        std::filesystem::path outputWavPath;
        int blockSize = 512;
        double tailSeconds = 2.0;
        // -1 means process every enabled effect slot in order. 0/1 processes
        // one slot only, which is useful for Test Effect from an editor window.
        int effectSlotIndex = -1;
        std::atomic<bool>* cancelRequested = nullptr;
    };

    struct VstEffectProcessResult
    {
        bool success = false;
        bool cancelled = false;
        bool effectApplied = false;
        std::string message;
        std::filesystem::path wavPath;
    };

    class VstInstrumentHost
    {
    public:
        static VstLoadResult loadPluginAssignment(const mw::core::VstPluginAssignment& pluginAssignment, double sampleRate, int blockSize, int inputChannels, int outputChannels);
        static VstLoadResult loadInstrumentForTrack(const mw::core::Track& track, double sampleRate, int blockSize);
        static VstRenderResult renderTrackToWav(const VstRenderRequest& request);
        static VstEffectProcessResult processWavWithTrackEffectChain(const VstEffectProcessRequest& request);
        static VstEffectProcessResult processWavWithFirstTrackEffect(const VstEffectProcessRequest& request);
        static bool trackHasVstPlugin(const mw::core::Track& track);
        static bool trackHasEnabledVstEffect(const mw::core::Track& track);
        static juce::MidiBuffer buildMidiForBlock(const mw::core::Track& track, std::int64_t blockStartSample, int blockNumSamples, double samplesPerTick);
    };
}
