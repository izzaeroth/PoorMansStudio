#include "midi/MidiRecordingCountInPlayer.h"

#include <algorithm>
#include <cmath>

namespace mw::midi
{
    MidiRecordingCountInPlayer::MidiRecordingCountInPlayer(mw::audio::ApplicationAudioService& audioService)
        : audioService_(audioService)
    {
    }

    MidiRecordingCountInPlayer::~MidiRecordingCountInPlayer()
    {
        stop();
    }

    bool MidiRecordingCountInPlayer::start(const MidiRecordingCountInPlan& plan, juce::String& errorMessage)
    {
        stop();
        errorMessage.clear();
        if (!plan.enabled())
            return true;

        plan_ = plan;
        samplePosition_.store(0);
        active_.store(true);
        const auto activation = audioService_.activateOutputCallback(
            mw::audio::ApplicationAudioPlaybackOwner::MidiRecordingCountIn,
            this);
        if (!activation.success)
        {
            active_.store(false);
            errorMessage = activation.message;
            return false;
        }
        return true;
    }

    void MidiRecordingCountInPlayer::stop()
    {
        if (active_.exchange(false))
        {
            audioService_.deactivateOutputCallback(
                mw::audio::ApplicationAudioPlaybackOwner::MidiRecordingCountIn,
                this);
        }
        samplePosition_.store(0);
    }

    void MidiRecordingCountInPlayer::audioDeviceAboutToStart(juce::AudioIODevice* device)
    {
        sampleRate_ = device != nullptr && device->getCurrentSampleRate() > 0.0
            ? device->getCurrentSampleRate()
            : 48000.0;
        samplePosition_.store(0);
    }

    void MidiRecordingCountInPlayer::audioDeviceStopped()
    {
        samplePosition_.store(0);
    }

    void MidiRecordingCountInPlayer::audioDeviceIOCallback(const float* const*,
                                                            int,
                                                            float* const* outputChannelData,
                                                            int numOutputChannels,
                                                            int numSamples)
    {
        render(outputChannelData, numOutputChannels, numSamples);
    }

    void MidiRecordingCountInPlayer::audioDeviceIOCallbackWithContext(const float* const* inputChannelData,
                                                                       int numInputChannels,
                                                                       float* const* outputChannelData,
                                                                       int numOutputChannels,
                                                                       int numSamples,
                                                                       const juce::AudioIODeviceCallbackContext&)
    {
        audioDeviceIOCallback(inputChannelData, numInputChannels, outputChannelData, numOutputChannels, numSamples);
    }

    void MidiRecordingCountInPlayer::render(float* const* outputChannelData, int numOutputChannels, int numSamples)
    {
        for (int channel = 0; channel < numOutputChannels; ++channel)
            if (outputChannelData[channel] != nullptr)
                juce::FloatVectorOperations::clear(outputChannelData[channel], numSamples);

        if (!active_.load() || numSamples <= 0 || sampleRate_ <= 0.0)
            return;

        const double clickLengthSeconds = std::clamp(plan_.secondsPerBeat * 0.40, 0.004, 0.028);
        constexpr double twoPi = 6.283185307179586476925286766559;
        const auto startSample = samplePosition_.fetch_add(numSamples);

        for (int sample = 0; sample < numSamples; ++sample)
        {
            const auto absoluteSample = startSample + sample;
            const double timeSeconds = static_cast<double>(absoluteSample) / sampleRate_;
            double mixed = 0.0;

            for (const auto& beat : plan_.beats)
            {
                const double age = timeSeconds - beat.startSeconds;
                if (age < 0.0 || age >= clickLengthSeconds)
                    continue;

                const double normalized = age / clickLengthSeconds;
                const double envelope = (1.0 - normalized) * (1.0 - normalized);
                const double frequency = beat.accented ? 1760.0 : 1100.0;
                const double amplitude = beat.accented ? 0.42 : 0.26;
                mixed += std::sin(twoPi * frequency * age) * envelope * amplitude;
            }

            const float value = static_cast<float>(std::clamp(mixed, -0.8, 0.8));
            for (int channel = 0; channel < numOutputChannels; ++channel)
                if (outputChannelData[channel] != nullptr)
                    outputChannelData[channel][sample] = value;
        }

    }
}
