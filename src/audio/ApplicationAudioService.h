#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include <mutex>

namespace mw::audio
{
    enum class ApplicationAudioPlaybackOwner
    {
        None,
        VstLiveSelectedTrackPreview,
        ClapLiveSelectedTrackPreview,
        MixedLiveProjectPreview,
        RecordTestPlayback,
        MidiRecordingCountIn,
        LiveMidiMonitoring,
        AudioClipRecorder
    };

    enum class ApplicationAudioInputMode
    {
        Mono = 1,
        StereoAdjacent = 2
    };

    struct ApplicationAudioConfiguration
    {
        juce::String deviceTypeName;
        juce::String inputDeviceName;
        juce::String outputDeviceName;
        double sampleRate = 0.0;
        int bufferSize = 0;
        ApplicationAudioInputMode inputMode = ApplicationAudioInputMode::Mono;
        int inputChannelIndex = 0;
    };

    struct ApplicationAudioDeviceOptions
    {
        ApplicationAudioConfiguration currentConfiguration;
        juce::StringArray deviceTypeNames;
        juce::StringArray inputDeviceNames;
        juce::StringArray outputDeviceNames;
        juce::StringArray inputChannelNames;
        juce::StringArray outputChannelNames;
        juce::Array<double> availableSampleRates;
        juce::Array<int> availableBufferSizes;
        int inputLatencySamples = 0;
        int outputLatencySamples = 0;
        int dropoutCount = 0;
        juce::String deviceSummary;
    };

    struct ApplicationAudioApplyResult
    {
        bool success = false;
        bool usedFallback = false;
        juce::String message;
        ApplicationAudioConfiguration acceptedConfiguration;
    };

    struct ApplicationAudioActivationResult
    {
        bool success = false;
        juce::String message;
    };

    struct ApplicationAudioRecorderActivationResult
    {
        bool success = false;
        juce::String message;
        double sampleRate = 48000.0;
        int blockSize = 512;
        int activeInputChannels = 0;
        int activeOutputChannels = 0;
        int firstInputChannelIndex = 0;
        ApplicationAudioInputMode inputMode = ApplicationAudioInputMode::Mono;
    };

    // Application-lifetime owner of the physical JUCE audio device used by
    // real-time preview and AudioClip recording callbacks. Musical processing,
    // WAV writing, plugin sessions, and scheduling remain in their existing
    // owners; this service owns hardware configuration and callback attachment.
    class ApplicationAudioService final
    {
    public:
        ApplicationAudioService() = default;
        ~ApplicationAudioService();

        ApplicationAudioService(const ApplicationAudioService&) = delete;
        ApplicationAudioService& operator=(const ApplicationAudioService&) = delete;

        juce::String initialiseOutput();
        ApplicationAudioApplyResult applyConfiguration(const ApplicationAudioConfiguration& requestedConfiguration);
        ApplicationAudioConfiguration currentConfiguration() const;
        ApplicationAudioDeviceOptions getDeviceOptions(const juce::String& requestedDeviceTypeName = {});
        int configuredRecordingChannelCount() const;
        juce::String configuredInputRoutingSummary() const;

        ApplicationAudioActivationResult activateOutputCallback(
            ApplicationAudioPlaybackOwner owner,
            juce::AudioIODeviceCallback* callback);

        void deactivateOutputCallback(
            ApplicationAudioPlaybackOwner owner,
            juce::AudioIODeviceCallback* callback);

        ApplicationAudioRecorderActivationResult activateRecorderCallback(
            juce::AudioIODeviceCallback* callback,
            int requestedInputChannels,
            bool monitorOutputRequested,
            const juce::String& preferredInputDeviceName);

        void deactivateRecorderCallback(juce::AudioIODeviceCallback* callback);

        juce::StringArray getAvailableInputDeviceNames();
        juce::String currentInputDeviceSummary(const juce::String& preferredInputDeviceName = {}) const;

        bool isOutputOpen() const;
        bool isRecorderActive() const;
        ApplicationAudioPlaybackOwner activePlaybackOwner() const;
        juce::String activePlaybackOwnerName() const;
        juce::String currentDeviceSummary() const;

        void shutdown();

    private:
        juce::String initialiseOutputLocked();
        juce::String configureOutputOnlyLocked(const ApplicationAudioConfiguration& requestedConfiguration);
        juce::String configureRecorderDeviceLocked(
            int requestedInputChannels,
            int requestedOutputChannels,
            const juce::String& preferredInputDeviceName);
        juce::String selectDeviceTypeLocked(const juce::String& requestedDeviceTypeName);
        ApplicationAudioConfiguration captureAcceptedConfigurationLocked(
            const ApplicationAudioConfiguration& requestedConfiguration) const;
        juce::BigInteger buildInputChannelMaskLocked(int requestedInputChannels, int& acceptedFirstChannelIndex) const;
        juce::BigInteger buildOutputChannelMaskLocked(int requestedOutputChannels) const;
        void restoreOutputOnlyLocked();
        static juce::String ownerName(ApplicationAudioPlaybackOwner owner);
        static juce::String inputModeName(ApplicationAudioInputMode mode);

        mutable std::mutex stateMutex_;
        juce::AudioDeviceManager deviceManager_;
        juce::AudioIODeviceCallback* activeCallback_ = nullptr;
        ApplicationAudioPlaybackOwner activeOwner_ = ApplicationAudioPlaybackOwner::None;
        ApplicationAudioConfiguration configuration_;
        juce::String lastInitialiseError_;
    };
}
