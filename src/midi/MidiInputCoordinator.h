#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_events/juce_events.h>

#include <functional>
#include <memory>
#include <mutex>

namespace mw::midi
{
    struct MidiInputSelection
    {
        juce::String deviceIdentifier;
        juce::String deviceName;
        int channel = 0; // 0 = Omni, 1-16 = exact channel
    };

    struct MidiInputOpenResult
    {
        bool success = false;
        juce::String message;
        MidiInputSelection acceptedSelection;
    };

    class MidiInputCoordinator final : private juce::MidiInputCallback,
                                       private juce::Timer
    {
    public:
        using MessageCallback = std::function<void(const juce::MidiMessage&)>;
        using DisconnectCallback = std::function<void(const juce::String&)>;

        MidiInputCoordinator();
        ~MidiInputCoordinator() override;

        MidiInputCoordinator(const MidiInputCoordinator&) = delete;
        MidiInputCoordinator& operator=(const MidiInputCoordinator&) = delete;

        juce::Array<juce::MidiDeviceInfo> availableDevices() const;
        MidiInputOpenResult open(const MidiInputSelection& requestedSelection);
        void close();

        void setMessageCallback(MessageCallback callback);
        void setDisconnectCallback(DisconnectCallback callback);

        bool isOpen() const;
        MidiInputSelection selection() const;
        juce::String statusSummary() const;

    private:
        void handleIncomingMidiMessage(juce::MidiInput* source, const juce::MidiMessage& message) override;
        void timerCallback() override;
        static int sanitizeChannel(int channel) noexcept;

        mutable std::mutex stateMutex_;
        std::unique_ptr<juce::MidiInput> input_;
        MidiInputSelection selection_;
        MessageCallback messageCallback_;
        DisconnectCallback disconnectCallback_;
    };
}
