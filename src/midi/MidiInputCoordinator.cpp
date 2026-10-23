#include "midi/MidiInputCoordinator.h"
#include "midi/MidiInputFilter.h"

#include <algorithm>

namespace mw::midi
{
    MidiInputCoordinator::MidiInputCoordinator()
    {
        startTimer(1000);
    }

    MidiInputCoordinator::~MidiInputCoordinator()
    {
        stopTimer();
        close();
    }

    int MidiInputCoordinator::sanitizeChannel(int channel) noexcept
    {
        return sanitizeMidiInputChannelFilter(channel);
    }

    juce::Array<juce::MidiDeviceInfo> MidiInputCoordinator::availableDevices() const
    {
        return juce::MidiInput::getAvailableDevices();
    }

    MidiInputOpenResult MidiInputCoordinator::open(const MidiInputSelection& requestedSelection)
    {
        MidiInputOpenResult result;
        const auto devices = availableDevices();
        if (devices.isEmpty())
        {
            result.message = "No MIDI input devices are available.";
            return result;
        }

        const auto requestedIdentifier = requestedSelection.deviceIdentifier.trim();
        const auto requestedName = requestedSelection.deviceName.trim();
        const juce::MidiDeviceInfo* selectedDevice = nullptr;

        if (requestedIdentifier.isNotEmpty())
        {
            for (const auto& device : devices)
            {
                if (device.identifier == requestedIdentifier)
                {
                    selectedDevice = &device;
                    break;
                }
            }
        }

        if (selectedDevice == nullptr && requestedName.isNotEmpty())
        {
            for (const auto& device : devices)
            {
                if (device.name == requestedName)
                {
                    selectedDevice = &device;
                    break;
                }
            }
        }

        if (selectedDevice == nullptr)
        {
            if (requestedIdentifier.isNotEmpty() || requestedName.isNotEmpty())
            {
                result.message = "The saved MIDI input device is not currently available.";
                result.acceptedSelection = requestedSelection;
                result.acceptedSelection.channel = sanitizeChannel(requestedSelection.channel);
                return result;
            }
            selectedDevice = &devices.getReference(0);
        }

        const int acceptedChannel = sanitizeChannel(requestedSelection.channel);
        {
            const std::scoped_lock lock(stateMutex_);
            if (input_ != nullptr && selection_.deviceIdentifier == selectedDevice->identifier)
            {
                selection_.deviceName = selectedDevice->name;
                selection_.channel = acceptedChannel;
                result.success = true;
                result.acceptedSelection = selection_;
                result.message = "MIDI input updated: " + selectedDevice->name
                    + (acceptedChannel == 0 ? " / Omni." : " / Channel " + juce::String(acceptedChannel) + ".");
                return result;
            }
        }

        // Close the previous device before opening a different one. Some Windows MIDI
        // drivers do not permit a second open, even from the same process.
        std::unique_ptr<juce::MidiInput> previousInput;
        MidiInputSelection previousSelection;
        {
            const std::scoped_lock lock(stateMutex_);
            previousInput = std::move(input_);
            previousSelection = selection_;
        }
        if (previousInput != nullptr)
            previousInput->stop();
        previousInput.reset();

        auto opened = juce::MidiInput::openDevice(selectedDevice->identifier, this);
        if (opened == nullptr)
        {
            // Best-effort rollback to the prior device so a failed settings change does
            // not unnecessarily tear down a working MIDI input.
            if (previousSelection.deviceIdentifier.isNotEmpty())
            {
                auto restored = juce::MidiInput::openDevice(previousSelection.deviceIdentifier, this);
                if (restored != nullptr)
                {
                    restored->start();
                    const std::scoped_lock lock(stateMutex_);
                    input_ = std::move(restored);
                    selection_ = previousSelection;
                }
            }

            result.message = "Could not open MIDI input device '" + selectedDevice->name + "'.";
            return result;
        }

        MidiInputSelection acceptedSelection;
        acceptedSelection.deviceIdentifier = selectedDevice->identifier;
        acceptedSelection.deviceName = selectedDevice->name;
        acceptedSelection.channel = acceptedChannel;
        {
            const std::scoped_lock lock(stateMutex_);
            input_ = std::move(opened);
            selection_ = acceptedSelection;
        }

        // start() is deliberately outside stateMutex_: the MIDI thread may begin its
        // callback immediately and the callback also takes stateMutex_.
        juce::MidiInput* inputToStart = nullptr;
        {
            const std::scoped_lock lock(stateMutex_);
            inputToStart = input_.get();
        }
        if (inputToStart != nullptr)
            inputToStart->start();

        result.success = true;
        result.acceptedSelection = acceptedSelection;
        result.message = "MIDI input opened: " + selectedDevice->name
            + (acceptedSelection.channel == 0
                ? " / Omni."
                : " / Channel " + juce::String(acceptedSelection.channel) + ".");
        return result;
    }

    void MidiInputCoordinator::close()
    {
        std::unique_ptr<juce::MidiInput> inputToClose;
        {
            const std::scoped_lock lock(stateMutex_);
            inputToClose = std::move(input_);
        }

        if (inputToClose != nullptr)
            inputToClose->stop();
    }

    void MidiInputCoordinator::setMessageCallback(MessageCallback callback)
    {
        const std::scoped_lock lock(stateMutex_);
        messageCallback_ = std::move(callback);
    }

    void MidiInputCoordinator::setDisconnectCallback(DisconnectCallback callback)
    {
        const std::scoped_lock lock(stateMutex_);
        disconnectCallback_ = std::move(callback);
    }

    bool MidiInputCoordinator::isOpen() const
    {
        const std::scoped_lock lock(stateMutex_);
        return input_ != nullptr;
    }

    MidiInputSelection MidiInputCoordinator::selection() const
    {
        const std::scoped_lock lock(stateMutex_);
        return selection_;
    }

    juce::String MidiInputCoordinator::statusSummary() const
    {
        const std::scoped_lock lock(stateMutex_);
        if (input_ == nullptr)
            return "MIDI input is closed.";

        return "MIDI input: " + selection_.deviceName
            + (selection_.channel == 0
                ? " / Omni"
                : " / Channel " + juce::String(selection_.channel));
    }

    void MidiInputCoordinator::handleIncomingMidiMessage(juce::MidiInput*, const juce::MidiMessage& message)
    {
        if (!(message.isNoteOn() || message.isNoteOff() || message.isController() || message.isPitchWheel() || message.isProgramChange() || message.isChannelPressure() || message.isAftertouch()))
            return;

        MessageCallback callback;
        int channelFilter = 0;
        {
            const std::scoped_lock lock(stateMutex_);
            channelFilter = selection_.channel;
            callback = messageCallback_;
        }

        if (!midiInputChannelAccepted(channelFilter, message.getChannel()))
            return;

        if (callback)
            callback(message);
    }

    void MidiInputCoordinator::timerCallback()
    {
        DisconnectCallback disconnectCallback;
        juce::String disconnectedName;
        juce::String selectedIdentifier;

        {
            const std::scoped_lock lock(stateMutex_);
            if (input_ == nullptr || selection_.deviceIdentifier.isEmpty())
                return;
            selectedIdentifier = selection_.deviceIdentifier;
            disconnectedName = selection_.deviceName;
        }

        bool stillAvailable = false;
        const auto devices = juce::MidiInput::getAvailableDevices();
        for (const auto& device : devices)
        {
            if (device.identifier == selectedIdentifier)
            {
                stillAvailable = true;
                break;
            }
        }

        if (stillAvailable)
            return;

        std::unique_ptr<juce::MidiInput> disconnectedInput;
        {
            const std::scoped_lock lock(stateMutex_);
            if (input_ == nullptr || selection_.deviceIdentifier != selectedIdentifier)
                return;
            disconnectedInput = std::move(input_);
            disconnectCallback = disconnectCallback_;
        }

        if (disconnectedInput != nullptr)
            disconnectedInput->stop();

        if (disconnectCallback)
            disconnectCallback(disconnectedName);
    }
}
