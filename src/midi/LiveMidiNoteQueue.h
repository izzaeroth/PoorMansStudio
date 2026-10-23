#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include <array>
#include <atomic>
#include <cstdint>

namespace mw::midi
{
    enum class LiveMidiEventType : std::uint8_t
    {
        NoteOn,
        NoteOff,
        Controller,
        PitchBend,
        ProgramChange,
        ChannelPressure,
        PolyPressure
    };

    struct LiveMidiNoteEvent
    {
        LiveMidiEventType type = LiveMidiEventType::NoteOff;
        bool noteOn = false; // compatibility convenience for existing note paths
        std::uint8_t channel = 1;
        std::uint8_t key = 60;
        std::uint8_t velocity = 0;
        std::uint8_t controller = 0;
        std::uint8_t controllerValue = 0;
        std::int16_t pitchBendValue = 0; // signed -8192..+8191
        std::uint8_t program = 0;
        std::uint8_t channelPressure = 0;
        std::uint8_t polyPressure = 0;
    };

    // Single-producer/single-consumer fixed-capacity queue used to move supported
    // channel MIDI messages from JUCE's MIDI input callback to the audio callback.
    // No heap allocation is performed by push/pop.
    class LiveMidiNoteQueue final
    {
    public:
        static constexpr int capacity = 512;

        bool push(const juce::MidiMessage& message) noexcept
        {
            LiveMidiNoteEvent event;
            event.channel = static_cast<std::uint8_t>(juce::jlimit(1, 16, message.getChannel()));

            if (message.isNoteOn())
            {
                event.type = LiveMidiEventType::NoteOn;
                event.noteOn = true;
                event.key = static_cast<std::uint8_t>(juce::jlimit(0, 127, message.getNoteNumber()));
                event.velocity = static_cast<std::uint8_t>(juce::jlimit(1, 127, static_cast<int>(message.getVelocity())));
            }
            else if (message.isNoteOff())
            {
                event.type = LiveMidiEventType::NoteOff;
                event.noteOn = false;
                event.key = static_cast<std::uint8_t>(juce::jlimit(0, 127, message.getNoteNumber()));
                event.velocity = 0;
            }
            else if (message.isController())
            {
                event.type = LiveMidiEventType::Controller;
                event.controller = static_cast<std::uint8_t>(juce::jlimit(0, 127, message.getControllerNumber()));
                event.controllerValue = static_cast<std::uint8_t>(juce::jlimit(0, 127, message.getControllerValue()));
            }
            else if (message.isPitchWheel())
            {
                event.type = LiveMidiEventType::PitchBend;
                event.pitchBendValue = static_cast<std::int16_t>(juce::jlimit(-8192, 8191, message.getPitchWheelValue() - 8192));
            }
            else if (message.isProgramChange())
            {
                event.type = LiveMidiEventType::ProgramChange;
                event.program = static_cast<std::uint8_t>(juce::jlimit(0, 127, message.getProgramChangeNumber()));
            }
            else if (message.isChannelPressure())
            {
                event.type = LiveMidiEventType::ChannelPressure;
                event.channelPressure = static_cast<std::uint8_t>(juce::jlimit(0, 127, message.getChannelPressureValue()));
            }
            else if (message.isAftertouch())
            {
                event.type = LiveMidiEventType::PolyPressure;
                event.key = static_cast<std::uint8_t>(juce::jlimit(0, 127, message.getNoteNumber()));
                event.polyPressure = static_cast<std::uint8_t>(juce::jlimit(0, 127, message.getAfterTouchValue()));
            }
            else
            {
                return false;
            }

            int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
            fifo_.prepareToWrite(1, start1, size1, start2, size2);
            if (size1 <= 0)
            {
                dropped_.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            events_[static_cast<std::size_t>(start1)] = event;
            fifo_.finishedWrite(1);
            return true;
        }

        int pop(LiveMidiNoteEvent* destination, int maximumEvents) noexcept
        {
            if (destination == nullptr || maximumEvents <= 0) return 0;
            int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
            fifo_.prepareToRead(maximumEvents, start1, size1, start2, size2);
            int copied = 0;
            for (int i = 0; i < size1; ++i) destination[copied++] = events_[static_cast<std::size_t>(start1 + i)];
            for (int i = 0; i < size2; ++i) destination[copied++] = events_[static_cast<std::size_t>(start2 + i)];
            if (copied > 0) fifo_.finishedRead(copied);
            return copied;
        }

        int droppedEventCount() const noexcept { return dropped_.load(std::memory_order_relaxed); }

    private:
        std::array<LiveMidiNoteEvent, static_cast<std::size_t>(capacity)> events_ {};
        juce::AbstractFifo fifo_ { capacity };
        std::atomic<int> dropped_ { 0 };
    };
}
