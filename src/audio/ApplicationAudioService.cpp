#include "audio/ApplicationAudioService.h"

#include <algorithm>
#include <cmath>

namespace mw::audio
{
    namespace
    {
        bool approximatelyEqual(double a, double b)
        {
            return std::abs(a - b) < 0.5;
        }

        template <typename ValueType>
        void addSortedUnique(juce::Array<ValueType>& values, ValueType value)
        {
            if (!values.contains(value))
                values.add(value);
            std::sort(values.begin(), values.end());
        }
    }

    ApplicationAudioService::~ApplicationAudioService()
    {
        shutdown();
    }

    juce::String ApplicationAudioService::ownerName(ApplicationAudioPlaybackOwner owner)
    {
        switch (owner)
        {
            case ApplicationAudioPlaybackOwner::VstLiveSelectedTrackPreview:
                return "VST3 selected-track preview";
            case ApplicationAudioPlaybackOwner::ClapLiveSelectedTrackPreview:
                return "CLAP selected-track preview";
            case ApplicationAudioPlaybackOwner::MixedLiveProjectPreview:
                return "mixed live project preview";
            case ApplicationAudioPlaybackOwner::RecordTestPlayback:
                return "AudioClip Record Test playback";
            case ApplicationAudioPlaybackOwner::MidiRecordingCountIn:
                return "MIDI recording count-in";
            case ApplicationAudioPlaybackOwner::LiveMidiMonitoring:
                return "live MIDI monitoring";
            case ApplicationAudioPlaybackOwner::AudioClipRecorder:
                return "AudioClip recording";
            case ApplicationAudioPlaybackOwner::None:
            default:
                return "none";
        }
    }

    juce::String ApplicationAudioService::inputModeName(ApplicationAudioInputMode mode)
    {
        return mode == ApplicationAudioInputMode::StereoAdjacent ? "Stereo adjacent pair" : "Mono";
    }

    juce::String ApplicationAudioService::selectDeviceTypeLocked(const juce::String& requestedDeviceTypeName)
    {
        const auto requested = requestedDeviceTypeName.trim();
        if (requested.isEmpty() || requested == deviceManager_.getCurrentAudioDeviceType())
            return {};

        auto& types = deviceManager_.getAvailableDeviceTypes();
        for (auto* type : types)
        {
            if (type != nullptr && type->getTypeName() == requested)
            {
                deviceManager_.setCurrentAudioDeviceType(requested, true);
                if (deviceManager_.getCurrentAudioDeviceType() != requested)
                    return "JUCE did not accept audio device type '" + requested + "'.";
                return {};
            }
        }

        return "Audio device type '" + requested + "' is not available.";
    }

    juce::BigInteger ApplicationAudioService::buildOutputChannelMaskLocked(int requestedOutputChannels) const
    {
        juce::BigInteger mask;
        auto* device = deviceManager_.getCurrentAudioDevice();
        const int availableChannels = device != nullptr ? device->getOutputChannelNames().size() : requestedOutputChannels;
        const int count = juce::jlimit(0, juce::jmax(0, availableChannels), requestedOutputChannels);
        for (int channel = 0; channel < count; ++channel)
            mask.setBit(channel);
        return mask;
    }

    juce::BigInteger ApplicationAudioService::buildInputChannelMaskLocked(
        int requestedInputChannels,
        int& acceptedFirstChannelIndex) const
    {
        juce::BigInteger mask;
        auto* device = deviceManager_.getCurrentAudioDevice();
        const int availableChannels = device != nullptr ? device->getInputChannelNames().size() : requestedInputChannels;
        if (availableChannels <= 0 || requestedInputChannels <= 0)
        {
            acceptedFirstChannelIndex = 0;
            return mask;
        }

        const int requestedCount = configuration_.inputMode == ApplicationAudioInputMode::StereoAdjacent
            ? 2
            : 1;
        const int count = juce::jlimit(1, juce::jmin(2, availableChannels), juce::jmin(requestedInputChannels, requestedCount));
        const int maximumFirst = juce::jmax(0, availableChannels - count);
        acceptedFirstChannelIndex = juce::jlimit(0, maximumFirst, configuration_.inputChannelIndex);

        for (int offset = 0; offset < count; ++offset)
            mask.setBit(acceptedFirstChannelIndex + offset);
        return mask;
    }

    ApplicationAudioConfiguration ApplicationAudioService::captureAcceptedConfigurationLocked(
        const ApplicationAudioConfiguration& requestedConfiguration) const
    {
        ApplicationAudioConfiguration accepted = requestedConfiguration;
        accepted.deviceTypeName = deviceManager_.getCurrentAudioDeviceType();

        juce::AudioDeviceManager::AudioDeviceSetup setup;
        deviceManager_.getAudioDeviceSetup(setup);
        accepted.inputDeviceName = setup.inputDeviceName.isNotEmpty()
            ? setup.inputDeviceName
            : requestedConfiguration.inputDeviceName;
        accepted.outputDeviceName = setup.outputDeviceName.isNotEmpty()
            ? setup.outputDeviceName
            : requestedConfiguration.outputDeviceName;
        accepted.sampleRate = setup.sampleRate;
        accepted.bufferSize = setup.bufferSize;

        if (auto* device = deviceManager_.getCurrentAudioDevice())
        {
            if (accepted.sampleRate <= 0.0)
                accepted.sampleRate = device->getCurrentSampleRate();
            if (accepted.bufferSize <= 0)
                accepted.bufferSize = device->getCurrentBufferSizeSamples();

            const int availableInputs = device->getInputChannelNames().size();
            const int channelCount = accepted.inputMode == ApplicationAudioInputMode::StereoAdjacent ? 2 : 1;
            accepted.inputChannelIndex = juce::jlimit(0, juce::jmax(0, availableInputs - channelCount), accepted.inputChannelIndex);
            if (accepted.inputMode == ApplicationAudioInputMode::StereoAdjacent && availableInputs < 2)
                accepted.inputMode = ApplicationAudioInputMode::Mono;
        }

        return accepted;
    }

    juce::String ApplicationAudioService::configureOutputOnlyLocked(
        const ApplicationAudioConfiguration& requestedConfiguration)
    {
        deviceManager_.closeAudioDevice();

        auto error = deviceManager_.initialise(0, 2, nullptr, true);
        if (error.isNotEmpty())
        {
            deviceManager_.closeAudioDevice();
            return error;
        }

        error = selectDeviceTypeLocked(requestedConfiguration.deviceTypeName);
        if (error.isNotEmpty())
        {
            deviceManager_.closeAudioDevice();
            return error;
        }

        juce::AudioDeviceManager::AudioDeviceSetup setup;
        deviceManager_.getAudioDeviceSetup(setup);
        // Keep physical input closed during normal output-only operation. The
        // selected input name remains in configuration_ and is applied only when
        // recording or input monitoring begins.
        setup.inputDeviceName.clear();
        if (requestedConfiguration.outputDeviceName.isNotEmpty())
            setup.outputDeviceName = requestedConfiguration.outputDeviceName;
        if (requestedConfiguration.sampleRate > 0.0)
            setup.sampleRate = requestedConfiguration.sampleRate;
        if (requestedConfiguration.bufferSize > 0)
            setup.bufferSize = requestedConfiguration.bufferSize;

        setup.useDefaultInputChannels = false;
        setup.inputChannels.clear();
        setup.useDefaultOutputChannels = false;
        setup.outputChannels = buildOutputChannelMaskLocked(2);
        if (setup.outputChannels.isZero())
            setup.useDefaultOutputChannels = true;

        error = deviceManager_.setAudioDeviceSetup(setup, true);
        if (error.isNotEmpty())
        {
            deviceManager_.closeAudioDevice();
            return error;
        }

        auto* device = deviceManager_.getCurrentAudioDevice();
        if (device == nullptr || device->getActiveOutputChannels().countNumberOfSetBits() <= 0)
        {
            deviceManager_.closeAudioDevice();
            return "The selected setup did not expose an active audio output channel.";
        }

        configuration_ = captureAcceptedConfigurationLocked(requestedConfiguration);
        return {};
    }

    juce::String ApplicationAudioService::initialiseOutputLocked()
    {
        if (activeOwner_ == ApplicationAudioPlaybackOwner::AudioClipRecorder)
            return "Shared audio hardware is currently configured for AudioClip recording.";

        if (deviceManager_.getCurrentAudioDevice() != nullptr)
        {
            lastInitialiseError_.clear();
            return {};
        }

        const auto error = configureOutputOnlyLocked(configuration_);
        if (error.isNotEmpty())
        {
            deviceManager_.closeAudioDevice();
            lastInitialiseError_ = error;
            return error;
        }

        lastInitialiseError_.clear();
        return {};
    }

    juce::String ApplicationAudioService::initialiseOutput()
    {
        const std::scoped_lock lock(stateMutex_);
        return initialiseOutputLocked();
    }

    ApplicationAudioApplyResult ApplicationAudioService::applyConfiguration(
        const ApplicationAudioConfiguration& requestedConfiguration)
    {
        ApplicationAudioApplyResult result;
        const std::scoped_lock lock(stateMutex_);

        if (activeCallback_ != nullptr)
        {
            result.message = "Stop " + ownerName(activeOwner_) + " before changing Audio Settings.";
            result.acceptedConfiguration = configuration_;
            return result;
        }

        auto sanitized = requestedConfiguration;
        sanitized.inputChannelIndex = juce::jmax(0, sanitized.inputChannelIndex);
        sanitized.bufferSize = sanitized.bufferSize > 0 ? juce::jlimit(16, 8192, sanitized.bufferSize) : 0;
        sanitized.sampleRate = sanitized.sampleRate > 0.0 ? sanitized.sampleRate : 0.0;

        const auto error = configureOutputOnlyLocked(sanitized);
        if (error.isNotEmpty())
        {
            lastInitialiseError_ = error;
            result.message = "Audio Settings were not applied. " + error;
            result.acceptedConfiguration = configuration_;
            restoreOutputOnlyLocked();
            return result;
        }

        lastInitialiseError_.clear();
        result.success = true;
        result.acceptedConfiguration = configuration_;
        result.usedFallback = (sanitized.deviceTypeName.isNotEmpty() && configuration_.deviceTypeName != sanitized.deviceTypeName)
            || (sanitized.inputDeviceName.isNotEmpty() && configuration_.inputDeviceName != sanitized.inputDeviceName)
            || (sanitized.outputDeviceName.isNotEmpty() && configuration_.outputDeviceName != sanitized.outputDeviceName)
            || (sanitized.sampleRate > 0.0 && !approximatelyEqual(configuration_.sampleRate, sanitized.sampleRate))
            || (sanitized.bufferSize > 0 && configuration_.bufferSize != sanitized.bufferSize)
            || configuration_.inputMode != sanitized.inputMode
            || configuration_.inputChannelIndex != sanitized.inputChannelIndex;

        result.message = result.usedFallback
            ? "Audio Settings applied with the device's accepted fallback values."
            : "Audio Settings applied.";
        return result;
    }

    ApplicationAudioConfiguration ApplicationAudioService::currentConfiguration() const
    {
        const std::scoped_lock lock(stateMutex_);
        return configuration_;
    }

    ApplicationAudioDeviceOptions ApplicationAudioService::getDeviceOptions(
        const juce::String& requestedDeviceTypeName)
    {
        const std::scoped_lock lock(stateMutex_);
        ApplicationAudioDeviceOptions options;

        if (deviceManager_.getAvailableDeviceTypes().isEmpty())
            initialiseOutputLocked();

        auto& types = deviceManager_.getAvailableDeviceTypes();
        for (auto* type : types)
        {
            if (type != nullptr)
                options.deviceTypeNames.addIfNotAlreadyThere(type->getTypeName());
        }
        options.deviceTypeNames.sort(false);

        auto requestedType = requestedDeviceTypeName.trim();
        if (requestedType.isEmpty())
            requestedType = deviceManager_.getCurrentAudioDeviceType();

        for (auto* type : types)
        {
            if (type == nullptr || type->getTypeName() != requestedType)
                continue;

            type->scanForDevices();
            options.inputDeviceNames = type->getDeviceNames(true);
            options.outputDeviceNames = type->getDeviceNames(false);
            options.inputDeviceNames.removeDuplicates(false);
            options.outputDeviceNames.removeDuplicates(false);
            options.inputDeviceNames.sort(false);
            options.outputDeviceNames.sort(false);
            break;
        }

        options.currentConfiguration = configuration_;
        if (auto* device = deviceManager_.getCurrentAudioDevice())
        {
            options.inputChannelNames = device->getInputChannelNames();
            options.outputChannelNames = device->getOutputChannelNames();
            options.availableSampleRates = device->getAvailableSampleRates();
            options.availableBufferSizes = device->getAvailableBufferSizes();
            options.inputLatencySamples = device->getInputLatencyInSamples();
            options.outputLatencySamples = device->getOutputLatencyInSamples();
            options.dropoutCount = juce::jmax(0, deviceManager_.getXRunCount());
        }

        if (options.availableSampleRates.isEmpty())
        {
            addSortedUnique(options.availableSampleRates, 44100.0);
            addSortedUnique(options.availableSampleRates, 48000.0);
        }
        else
        {
            std::sort(options.availableSampleRates.begin(), options.availableSampleRates.end());
        }

        if (options.availableBufferSizes.isEmpty())
        {
            addSortedUnique(options.availableBufferSizes, 256);
            addSortedUnique(options.availableBufferSizes, 512);
        }
        else
        {
            std::sort(options.availableBufferSizes.begin(), options.availableBufferSizes.end());
        }
        if (auto* device = deviceManager_.getCurrentAudioDevice())
        {
            const auto inputCount = device->getActiveInputChannels().countNumberOfSetBits();
            const auto outputCount = device->getActiveOutputChannels().countNumberOfSetBits();
            options.deviceSummary = deviceManager_.getCurrentAudioDeviceType()
                + " / " + device->getName()
                + " @ " + juce::String(device->getCurrentSampleRate(), 0) + " Hz"
                + " / " + juce::String(device->getCurrentBufferSizeSamples()) + " samples"
                + " / " + juce::String(inputCount) + " in, " + juce::String(outputCount) + " out";
        }
        else if (lastInitialiseError_.isNotEmpty())
        {
            options.deviceSummary = "Shared audio unavailable: " + lastInitialiseError_;
        }
        else
        {
            options.deviceSummary = "Shared audio device is closed.";
        }
        return options;
    }

    int ApplicationAudioService::configuredRecordingChannelCount() const
    {
        const std::scoped_lock lock(stateMutex_);
        return configuration_.inputMode == ApplicationAudioInputMode::StereoAdjacent ? 2 : 1;
    }

    juce::String ApplicationAudioService::configuredInputRoutingSummary() const
    {
        const std::scoped_lock lock(stateMutex_);
        juce::String summary = inputModeName(configuration_.inputMode) + " input ";
        const int first = configuration_.inputChannelIndex + 1;
        if (configuration_.inputMode == ApplicationAudioInputMode::StereoAdjacent)
            summary << first << "-" << (first + 1);
        else
            summary << first;
        return summary;
    }

    ApplicationAudioActivationResult ApplicationAudioService::activateOutputCallback(
        ApplicationAudioPlaybackOwner owner,
        juce::AudioIODeviceCallback* callback)
    {
        ApplicationAudioActivationResult result;
        if (owner == ApplicationAudioPlaybackOwner::None
            || owner == ApplicationAudioPlaybackOwner::AudioClipRecorder
            || callback == nullptr)
        {
            result.message = "Shared output callback activation received an invalid owner or callback.";
            return result;
        }

        const std::scoped_lock lock(stateMutex_);

        if (activeCallback_ == callback && activeOwner_ == owner)
        {
            result.success = true;
            result.message = "Shared output callback is already active.";
            return result;
        }

        if (activeCallback_ != nullptr)
        {
            result.message = "Shared audio hardware is already owned by " + ownerName(activeOwner_)
                + ". Stop that owner before starting " + ownerName(owner) + ".";
            return result;
        }

        const auto initError = initialiseOutputLocked();
        if (initError.isNotEmpty())
        {
            result.message = "Shared audio output did not open. " + initError;
            return result;
        }

        deviceManager_.addAudioCallback(callback);
        activeCallback_ = callback;
        activeOwner_ = owner;
        result.success = true;
        result.message = "Shared output callback activated for " + ownerName(owner) + ".";
        return result;
    }

    void ApplicationAudioService::deactivateOutputCallback(
        ApplicationAudioPlaybackOwner owner,
        juce::AudioIODeviceCallback* callback)
    {
        if (callback == nullptr)
            return;

        const std::scoped_lock lock(stateMutex_);
        if (activeCallback_ != callback || activeOwner_ != owner)
            return;

        deviceManager_.removeAudioCallback(callback);
        activeCallback_ = nullptr;
        activeOwner_ = ApplicationAudioPlaybackOwner::None;
    }

    juce::String ApplicationAudioService::configureRecorderDeviceLocked(
        int requestedInputChannels,
        int requestedOutputChannels,
        const juce::String& preferredInputDeviceName)
    {
        deviceManager_.closeAudioDevice();

        const int configuredInputs = configuration_.inputMode == ApplicationAudioInputMode::StereoAdjacent ? 2 : 1;
        const int initialInputCount = juce::jlimit(1, 2, juce::jmin(requestedInputChannels, configuredInputs));
        auto error = deviceManager_.initialise(
            initialInputCount,
            juce::jlimit(0, 2, requestedOutputChannels),
            nullptr,
            true);
        if (error.isNotEmpty())
        {
            deviceManager_.closeAudioDevice();
            return error;
        }

        error = selectDeviceTypeLocked(configuration_.deviceTypeName);
        if (error.isNotEmpty())
        {
            deviceManager_.closeAudioDevice();
            return error;
        }

        juce::AudioDeviceManager::AudioDeviceSetup setup;
        deviceManager_.getAudioDeviceSetup(setup);
        const auto preferredInput = preferredInputDeviceName.trim().isNotEmpty()
            ? preferredInputDeviceName.trim()
            : configuration_.inputDeviceName;
        if (preferredInput.isNotEmpty())
            setup.inputDeviceName = preferredInput;
        if (configuration_.outputDeviceName.isNotEmpty())
            setup.outputDeviceName = configuration_.outputDeviceName;
        if (configuration_.sampleRate > 0.0)
            setup.sampleRate = configuration_.sampleRate;
        if (configuration_.bufferSize > 0)
            setup.bufferSize = configuration_.bufferSize;
        setup.useDefaultInputChannels = true;
        setup.useDefaultOutputChannels = requestedOutputChannels > 0;

        error = deviceManager_.setAudioDeviceSetup(setup, true);
        if (error.isNotEmpty())
        {
            deviceManager_.closeAudioDevice();
            return error;
        }

        int acceptedFirstChannelIndex = configuration_.inputChannelIndex;
        deviceManager_.getAudioDeviceSetup(setup);
        setup.useDefaultInputChannels = false;
        setup.inputChannels = buildInputChannelMaskLocked(initialInputCount, acceptedFirstChannelIndex);
        setup.useDefaultOutputChannels = false;
        setup.outputChannels = buildOutputChannelMaskLocked(requestedOutputChannels);
        if (requestedOutputChannels > 0 && setup.outputChannels.isZero())
            setup.useDefaultOutputChannels = true;

        error = deviceManager_.setAudioDeviceSetup(setup, true);
        if (error.isNotEmpty())
        {
            deviceManager_.closeAudioDevice();
            return error;
        }

        configuration_ = captureAcceptedConfigurationLocked(configuration_);
        configuration_.inputChannelIndex = acceptedFirstChannelIndex;
        return {};
    }

    ApplicationAudioRecorderActivationResult ApplicationAudioService::activateRecorderCallback(
        juce::AudioIODeviceCallback* callback,
        int requestedInputChannels,
        bool monitorOutputRequested,
        const juce::String& preferredInputDeviceName)
    {
        ApplicationAudioRecorderActivationResult result;
        if (callback == nullptr)
        {
            result.message = "Shared recorder activation received a null callback.";
            return result;
        }

        const std::scoped_lock lock(stateMutex_);

        if (activeCallback_ == callback && activeOwner_ == ApplicationAudioPlaybackOwner::AudioClipRecorder)
        {
            if (auto* device = deviceManager_.getCurrentAudioDevice())
            {
                result.success = true;
                result.message = "Shared AudioClip recorder callback is already active.";
                result.sampleRate = device->getCurrentSampleRate();
                result.blockSize = device->getCurrentBufferSizeSamples();
                result.activeInputChannels = device->getActiveInputChannels().countNumberOfSetBits();
                result.activeOutputChannels = device->getActiveOutputChannels().countNumberOfSetBits();
                result.firstInputChannelIndex = configuration_.inputChannelIndex;
                result.inputMode = configuration_.inputMode;
            }
            return result;
        }

        if (activeCallback_ != nullptr)
        {
            result.message = "Shared audio hardware is already owned by " + ownerName(activeOwner_)
                + ". Stop that owner before starting AudioClip recording.";
            return result;
        }

        const int configuredInputs = configuration_.inputMode == ApplicationAudioInputMode::StereoAdjacent ? 2 : 1;
        auto error = configureRecorderDeviceLocked(
            juce::jmin(requestedInputChannels, configuredInputs),
            2,
            preferredInputDeviceName);
        if (error.isNotEmpty() && !monitorOutputRequested)
        {
            error = configureRecorderDeviceLocked(
                juce::jmin(requestedInputChannels, configuredInputs),
                0,
                preferredInputDeviceName);
        }

        if (error.isNotEmpty())
        {
            lastInitialiseError_ = error;
            restoreOutputOnlyLocked();
            result.message = "Shared recording device did not open. " + error;
            return result;
        }

        auto* device = deviceManager_.getCurrentAudioDevice();
        if (device == nullptr)
        {
            lastInitialiseError_ = "JUCE reported success without an active audio device.";
            restoreOutputOnlyLocked();
            result.message = lastInitialiseError_;
            return result;
        }

        const int activeInputs = device->getActiveInputChannels().countNumberOfSetBits();
        if (activeInputs <= 0)
        {
            lastInitialiseError_ = "The selected device did not expose an active input channel.";
            restoreOutputOnlyLocked();
            result.message = lastInitialiseError_;
            return result;
        }

        deviceManager_.addAudioCallback(callback);
        activeCallback_ = callback;
        activeOwner_ = ApplicationAudioPlaybackOwner::AudioClipRecorder;
        lastInitialiseError_.clear();

        juce::String routingSummary = inputModeName(configuration_.inputMode) + " input ";
        const int firstInput = configuration_.inputChannelIndex + 1;
        if (configuration_.inputMode == ApplicationAudioInputMode::StereoAdjacent)
            routingSummary << firstInput << "-" << (firstInput + 1);
        else
            routingSummary << firstInput;

        result.success = true;
        result.message = "Shared AudioClip recorder callback activated using " + routingSummary + ".";
        result.sampleRate = device->getCurrentSampleRate() > 0.0 ? device->getCurrentSampleRate() : 48000.0;
        result.blockSize = juce::jlimit(16, 8192, device->getCurrentBufferSizeSamples());
        result.activeInputChannels = activeInputs;
        result.activeOutputChannels = device->getActiveOutputChannels().countNumberOfSetBits();
        result.firstInputChannelIndex = configuration_.inputChannelIndex;
        result.inputMode = configuration_.inputMode;
        return result;
    }

    void ApplicationAudioService::restoreOutputOnlyLocked()
    {
        deviceManager_.closeAudioDevice();
        const auto restoreError = configureOutputOnlyLocked(configuration_);
        if (restoreError.isNotEmpty())
        {
            deviceManager_.closeAudioDevice();
            lastInitialiseError_ = restoreError;
        }
        else
        {
            lastInitialiseError_.clear();
        }
    }

    void ApplicationAudioService::deactivateRecorderCallback(juce::AudioIODeviceCallback* callback)
    {
        if (callback == nullptr)
            return;

        const std::scoped_lock lock(stateMutex_);
        if (activeCallback_ != callback || activeOwner_ != ApplicationAudioPlaybackOwner::AudioClipRecorder)
            return;

        deviceManager_.removeAudioCallback(callback);
        activeCallback_ = nullptr;
        activeOwner_ = ApplicationAudioPlaybackOwner::None;
        restoreOutputOnlyLocked();
    }

    juce::StringArray ApplicationAudioService::getAvailableInputDeviceNames()
    {
        juce::String typeName;
        {
            const std::scoped_lock lock(stateMutex_);
            typeName = configuration_.deviceTypeName;
        }
        return getDeviceOptions(typeName).inputDeviceNames;
    }

    juce::String ApplicationAudioService::currentInputDeviceSummary(const juce::String& preferredInputDeviceName) const
    {
        const std::scoped_lock lock(stateMutex_);
        const auto selectedInput = preferredInputDeviceName.trim().isNotEmpty()
            ? preferredInputDeviceName.trim()
            : configuration_.inputDeviceName;

        juce::String inputName = selectedInput.isNotEmpty() ? selectedInput : "System Default audio input";
        if (auto* device = deviceManager_.getCurrentAudioDevice())
        {
            juce::AudioDeviceManager::AudioDeviceSetup setup;
            deviceManager_.getAudioDeviceSetup(setup);
            if (setup.inputDeviceName.isNotEmpty())
                inputName = setup.inputDeviceName;
            return inputName
                + " / " + inputModeName(configuration_.inputMode)
                + " " + (configuration_.inputMode == ApplicationAudioInputMode::StereoAdjacent
                    ? juce::String(configuration_.inputChannelIndex + 1) + "-" + juce::String(configuration_.inputChannelIndex + 2)
                    : juce::String(configuration_.inputChannelIndex + 1))
                + " @ " + juce::String(device->getCurrentSampleRate(), 0) + " Hz";
        }

        juce::String routingSummary = inputModeName(configuration_.inputMode) + " input ";
        const int firstInput = configuration_.inputChannelIndex + 1;
        if (configuration_.inputMode == ApplicationAudioInputMode::StereoAdjacent)
            routingSummary << firstInput << "-" << (firstInput + 1);
        else
            routingSummary << firstInput;
        return inputName + " / " + routingSummary;
    }

    bool ApplicationAudioService::isOutputOpen() const
    {
        const std::scoped_lock lock(stateMutex_);
        if (auto* device = deviceManager_.getCurrentAudioDevice())
            return device->getActiveOutputChannels().countNumberOfSetBits() > 0;
        return false;
    }

    bool ApplicationAudioService::isRecorderActive() const
    {
        const std::scoped_lock lock(stateMutex_);
        return activeOwner_ == ApplicationAudioPlaybackOwner::AudioClipRecorder
            && activeCallback_ != nullptr;
    }

    ApplicationAudioPlaybackOwner ApplicationAudioService::activePlaybackOwner() const
    {
        const std::scoped_lock lock(stateMutex_);
        return activeOwner_;
    }

    juce::String ApplicationAudioService::activePlaybackOwnerName() const
    {
        const std::scoped_lock lock(stateMutex_);
        return ownerName(activeOwner_);
    }

    juce::String ApplicationAudioService::currentDeviceSummary() const
    {
        const std::scoped_lock lock(stateMutex_);
        if (auto* device = deviceManager_.getCurrentAudioDevice())
        {
            const auto inputCount = device->getActiveInputChannels().countNumberOfSetBits();
            const auto outputCount = device->getActiveOutputChannels().countNumberOfSetBits();
            return deviceManager_.getCurrentAudioDeviceType()
                + " / " + device->getName()
                + " @ " + juce::String(device->getCurrentSampleRate(), 0) + " Hz"
                + " / " + juce::String(device->getCurrentBufferSizeSamples()) + " samples"
                + " / " + juce::String(inputCount) + " in, " + juce::String(outputCount) + " out";
        }

        if (lastInitialiseError_.isNotEmpty())
            return "Shared audio unavailable: " + lastInitialiseError_;
        return "Shared audio device is closed.";
    }

    void ApplicationAudioService::shutdown()
    {
        const std::scoped_lock lock(stateMutex_);
        if (activeCallback_ != nullptr)
            deviceManager_.removeAudioCallback(activeCallback_);

        activeCallback_ = nullptr;
        activeOwner_ = ApplicationAudioPlaybackOwner::None;
        deviceManager_.closeAudioDevice();
    }
}
