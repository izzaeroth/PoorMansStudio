#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include <atomic>
#include <cstdint>

#include "audio/ApplicationAudioService.h"
#include "midi/MidiRecordingCountIn.h"

namespace mw::midi
{
    class MidiRecordingCountInPlayer final : private juce::AudioIODeviceCallback
    {
    public:
        explicit MidiRecordingCountInPlayer(mw::audio::ApplicationAudioService& audioService);
        ~MidiRecordingCountInPlayer() override;

        MidiRecordingCountInPlayer(const MidiRecordingCountInPlayer&) = delete;
        MidiRecordingCountInPlayer& operator=(const MidiRecordingCountInPlayer&) = delete;

        bool start(const MidiRecordingCountInPlan& plan, juce::String& errorMessage);
        void stop();
        bool isActive() const noexcept { return active_.load(); }

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
                                              const juce::AudioIODeviceCallbackContext& context) override;
        void render(float* const* outputChannelData, int numOutputChannels, int numSamples);

        mw::audio::ApplicationAudioService& audioService_;
        MidiRecordingCountInPlan plan_;
        std::atomic<bool> active_ { false };
        double sampleRate_ = 48000.0;
        std::atomic<std::int64_t> samplePosition_ { 0 };
    };
}
