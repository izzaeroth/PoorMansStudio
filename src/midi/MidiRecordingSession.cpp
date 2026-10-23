#include "midi/MidiRecordingSession.h"
#include "midi/MidiAdvancedParameterDecoder.h"

#include <algorithm>
#include <cmath>

namespace mw::midi
{
    int MidiRecordingSession::sanitizeChannel(int channel) noexcept { return std::clamp(channel, 1, 16); }
    int MidiRecordingSession::sanitizePitch(int pitch) noexcept { return std::clamp(pitch, 0, 127); }
    int MidiRecordingSession::sanitizeVelocity(int velocity) noexcept { return std::clamp(velocity, 1, 127); }
    int MidiRecordingSession::sanitizeControllerValue(int value) noexcept { return std::clamp(value, 0, 127); }

    MidiRecordingSession::ActiveNoteKey MidiRecordingSession::makeKey(int channel, int pitch) noexcept
    {
        return (sanitizeChannel(channel) - 1) * 128 + sanitizePitch(pitch);
    }

    std::uint16_t MidiRecordingSession::channelMask(int channel) noexcept
    {
        const auto sanitized = sanitizeChannel(channel);
        return static_cast<std::uint16_t>(1u << static_cast<unsigned>(sanitized - 1));
    }

    bool MidiRecordingSession::start(const MidiRecordingConfig& requestedConfig)
    {
        if (requestedConfig.tempoBpm <= 0
            || requestedConfig.ticksPerQuarterNote <= 0
            || !std::isfinite(requestedConfig.startTimeSeconds))
        {
            return false;
        }

        std::scoped_lock lock(mutex_);
        config_ = requestedConfig;
        config_.tempoBpm = std::max(1, config_.tempoBpm);
        config_.ticksPerQuarterNote = std::max(1, config_.ticksPerQuarterNote);
        config_.startTick = std::max<std::int64_t>(0, config_.startTick);
        activeNotes_.clear();
        completedNotes_.clear();
        cc64Events_.clear();
        controllerEvents_.clear();
        pitchBendEvents_.clear();
        programChangeEvents_.clear();
        channelPressureEvents_.clear();
        polyPressureEvents_.clear();
        sustainDownMask_ = config_.sustainDownMaskAtStart;
        nextOrder_ = 1;
        nextCc64Order_ = 1;
        nextControllerOrder_ = 1;
        nextPitchBendOrder_ = 1;
        nextProgramChangeOrder_ = 1;
        nextChannelPressureOrder_ = 1;
        nextPolyPressureOrder_ = 1;
        recording_ = true;
        return true;
    }

    void MidiRecordingSession::cancel()
    {
        std::scoped_lock lock(mutex_);
        recording_ = false;
        activeNotes_.clear();
        completedNotes_.clear();
        cc64Events_.clear();
        controllerEvents_.clear();
        pitchBendEvents_.clear();
        programChangeEvents_.clear();
        channelPressureEvents_.clear();
        polyPressureEvents_.clear();
        sustainDownMask_ = 0;
        nextOrder_ = 1;
        nextCc64Order_ = 1;
        nextControllerOrder_ = 1;
        nextPitchBendOrder_ = 1;
        nextProgramChangeOrder_ = 1;
        nextChannelPressureOrder_ = 1;
        nextPolyPressureOrder_ = 1;
    }

    bool MidiRecordingSession::isRecording() const
    {
        std::scoped_lock lock(mutex_);
        return recording_;
    }

    MidiRecordingConfig MidiRecordingSession::config() const
    {
        std::scoped_lock lock(mutex_);
        return config_;
    }

    std::int64_t MidiRecordingSession::tickForTimeLocked(double eventTimeSeconds) const
    {
        if (!std::isfinite(eventTimeSeconds))
            eventTimeSeconds = config_.startTimeSeconds;

        const auto elapsedSeconds = std::max(0.0, eventTimeSeconds - config_.startTimeSeconds);
        const auto ticksPerSecond = static_cast<double>(config_.tempoBpm)
            * static_cast<double>(config_.ticksPerQuarterNote) / 60.0;
        const auto elapsedTicks = static_cast<std::int64_t>(std::llround(elapsedSeconds * ticksPerSecond));
        return config_.startTick + std::max<std::int64_t>(0, elapsedTicks);
    }

    std::int64_t MidiRecordingSession::tickForTime(double eventTimeSeconds) const
    {
        std::scoped_lock lock(mutex_);
        return tickForTimeLocked(eventTimeSeconds);
    }

    void MidiRecordingSession::noteOn(int midiChannel, int pitch, int velocity, double eventTimeSeconds)
    {
        std::scoped_lock lock(mutex_);
        if (!recording_)
            return;

        ActiveNote note;
        note.midiChannel = sanitizeChannel(midiChannel);
        note.pitch = sanitizePitch(pitch);
        note.velocity = sanitizeVelocity(velocity);
        note.startTick = tickForTimeLocked(eventTimeSeconds);
        note.order = nextOrder_++;
        activeNotes_[makeKey(note.midiChannel, note.pitch)].push_back(note);
    }

    void MidiRecordingSession::finalizeNoteLocked(const ActiveNote& active, std::int64_t endTick)
    {
        mw::core::NoteEvent note;
        note.pitch = active.pitch;
        note.velocity = active.velocity;
        note.midiChannel = active.midiChannel;
        note.startTick = std::max<std::int64_t>(0, active.startTick);
        note.durationTicks = std::max<std::int64_t>(1, endTick - note.startTick);
        note.articulation = mw::core::Articulation::Normal;
        completedNotes_.push_back(note);
    }

    void MidiRecordingSession::noteOff(int midiChannel, int pitch, double eventTimeSeconds)
    {
        std::scoped_lock lock(mutex_);
        if (!recording_)
            return;

        const auto key = makeKey(midiChannel, pitch);
        const auto it = activeNotes_.find(key);
        if (it == activeNotes_.end() || it->second.empty())
            return;

        const auto active = it->second.front();
        it->second.pop_front();
        if (it->second.empty())
            activeNotes_.erase(it);

        finalizeNoteLocked(active, tickForTimeLocked(eventTimeSeconds));
    }

    void MidiRecordingSession::controller(int midiChannel, int controllerNumber, int value, double eventTimeSeconds)
    {
        std::scoped_lock lock(mutex_);
        if (!recording_ || controllerNumber < 0 || controllerNumber > 127)
            return;

        const auto tick = tickForTimeLocked(eventTimeSeconds);
        const auto channel = sanitizeChannel(midiChannel);
        const auto controllerValue = sanitizeControllerValue(value);

        // CC64 always feeds the retained sustain semantic, even when general
        // project-controller capture is disabled.
        if (controllerNumber == 64)
        {
            mw::core::PersistentMidiCc64Event sustainEvent;
            sustainEvent.tick = tick;
            sustainEvent.midiChannel = channel;
            sustainEvent.value = controllerValue;
            sustainEvent.order = nextCc64Order_++;
            cc64Events_.push_back(sustainEvent);

            const auto mask = channelMask(channel);
            if (controllerValue >= 64)
                sustainDownMask_ = static_cast<std::uint16_t>(sustainDownMask_ | mask);
            else
                sustainDownMask_ = static_cast<std::uint16_t>(sustainDownMask_ & static_cast<std::uint16_t>(~mask));
        }

        if (config_.recordControllers || config_.recordAdvancedParameters)
        {
            mw::core::PersistentMidiControllerEvent controllerEvent;
            controllerEvent.tick = tick;
            controllerEvent.midiChannel = channel;
            controllerEvent.controller = controllerNumber;
            controllerEvent.value = controllerValue;
            controllerEvent.order = nextControllerOrder_++;
            controllerEvents_.push_back(controllerEvent);
        }
    }

    void MidiRecordingSession::controller64(int midiChannel, int value, double eventTimeSeconds)
    {
        controller(midiChannel, 64, value, eventTimeSeconds);
    }

    void MidiRecordingSession::pitchBend(int midiChannel, int value, double eventTimeSeconds)
    {
        std::scoped_lock lock(mutex_);
        if (!recording_ || !config_.recordPitchBend)
            return;
        mw::core::PersistentMidiPitchBendEvent event;
        event.tick = tickForTimeLocked(eventTimeSeconds);
        event.midiChannel = sanitizeChannel(midiChannel);
        event.value = std::clamp(value, mw::core::midiPitchBendMinimum, mw::core::midiPitchBendMaximum);
        event.order = nextPitchBendOrder_++;
        pitchBendEvents_.push_back(event);
    }

    void MidiRecordingSession::programChange(int midiChannel, int program, double eventTimeSeconds)
    {
        std::scoped_lock lock(mutex_);
        if (!recording_ || !config_.recordProgramChange)
            return;
        mw::core::PersistentMidiProgramChangeEvent event;
        event.tick = tickForTimeLocked(eventTimeSeconds);
        event.midiChannel = sanitizeChannel(midiChannel);
        event.program = std::clamp(program, mw::core::midiProgramChangeMinimum, mw::core::midiProgramChangeMaximum);
        event.order = nextProgramChangeOrder_++;
        programChangeEvents_.push_back(event);
    }

    void MidiRecordingSession::channelPressure(int midiChannel, int pressure, double eventTimeSeconds)
    {
        std::scoped_lock lock(mutex_);
        if (!recording_ || !config_.recordChannelPressure)
            return;
        mw::core::PersistentMidiChannelPressureEvent event;
        event.tick = tickForTimeLocked(eventTimeSeconds);
        event.midiChannel = sanitizeChannel(midiChannel);
        event.pressure = std::clamp(pressure, mw::core::midiChannelPressureMinimum, mw::core::midiChannelPressureMaximum);
        event.order = nextChannelPressureOrder_++;
        channelPressureEvents_.push_back(event);
    }

    void MidiRecordingSession::polyPressure(int midiChannel, int noteNumber, int pressure, double eventTimeSeconds)
    {
        std::scoped_lock lock(mutex_);
        if (!recording_ || !config_.recordPolyPressure)
            return;
        mw::core::PersistentMidiPolyPressureEvent event;
        event.tick = tickForTimeLocked(eventTimeSeconds);
        event.midiChannel = sanitizeChannel(midiChannel);
        event.noteNumber = sanitizePitch(noteNumber);
        event.pressure = std::clamp(pressure, mw::core::midiPolyPressureMinimum, mw::core::midiPolyPressureMaximum);
        event.order = nextPolyPressureOrder_++;
        polyPressureEvents_.push_back(event);
    }

    std::size_t MidiRecordingSession::releaseActiveNotesLocked(std::int64_t endTick)
    {
        std::vector<ActiveNote> active;
        for (auto& [key, queue] : activeNotes_)
        {
            (void) key;
            while (!queue.empty())
            {
                active.push_back(queue.front());
                queue.pop_front();
            }
        }
        activeNotes_.clear();

        std::stable_sort(active.begin(), active.end(), [](const ActiveNote& a, const ActiveNote& b)
        {
            return a.order < b.order;
        });

        for (const auto& note : active)
            finalizeNoteLocked(note, endTick);

        return active.size();
    }

    void MidiRecordingSession::forceSustainUpLocked(std::int64_t tick)
    {
        for (int channel = 1; channel <= 16; ++channel)
        {
            const auto mask = channelMask(channel);
            if ((sustainDownMask_ & mask) == 0)
                continue;
            cc64Events_.push_back({ tick, channel, 0, nextCc64Order_++ });
            if (config_.recordControllers)
                controllerEvents_.push_back({ tick, channel, 64, 0, nextControllerOrder_++ });
        }
        sustainDownMask_ = 0;
    }

    std::size_t MidiRecordingSession::releaseActiveNotes(double eventTimeSeconds)
    {
        std::scoped_lock lock(mutex_);
        if (!recording_)
            return 0;
        const auto tick = tickForTimeLocked(eventTimeSeconds);
        const auto count = releaseActiveNotesLocked(tick);
        forceSustainUpLocked(tick);
        return count;
    }

    MidiRecordingStopResult MidiRecordingSession::stop(double eventTimeSeconds)
    {
        std::scoped_lock lock(mutex_);
        MidiRecordingStopResult result;
        if (!recording_)
            return result;

        result.stopTick = tickForTimeLocked(eventTimeSeconds);
        result.finalizedHeldNoteCount = releaseActiveNotesLocked(result.stopTick);

        std::stable_sort(completedNotes_.begin(), completedNotes_.end(), [](const mw::core::NoteEvent& a, const mw::core::NoteEvent& b)
        {
            if (a.startTick != b.startTick)
                return a.startTick < b.startTick;
            if (a.midiChannel != b.midiChannel)
                return a.midiChannel < b.midiChannel;
            if (a.pitch != b.pitch)
                return a.pitch < b.pitch;
            if (a.velocity != b.velocity)
                return a.velocity < b.velocity;
            return a.durationTicks < b.durationTicks;
        });

        // Collapse exact same-tick controller tuples using source order: the
        // last real message at (tick, channel, controller) wins.
        std::stable_sort(controllerEvents_.begin(), controllerEvents_.end(), [](const auto& a, const auto& b)
        {
            if (a.tick != b.tick) return a.tick < b.tick;
            if (a.midiChannel != b.midiChannel) return a.midiChannel < b.midiChannel;
            if (a.controller != b.controller) return a.controller < b.controller;
            return a.order < b.order;
        });
        std::vector<mw::core::PersistentMidiControllerEvent> collapsedControllers;
        for (const auto& event : controllerEvents_)
        {
            if (!collapsedControllers.empty())
            {
                auto& previous = collapsedControllers.back();
                if (previous.tick == event.tick
                    && previous.midiChannel == event.midiChannel
                    && previous.controller == event.controller)
                {
                    previous = event;
                    continue;
                }
            }
            collapsedControllers.push_back(event);
        }

        std::stable_sort(pitchBendEvents_.begin(), pitchBendEvents_.end(), [](const auto& a, const auto& b)
        {
            if (a.tick != b.tick) return a.tick < b.tick;
            if (a.midiChannel != b.midiChannel) return a.midiChannel < b.midiChannel;
            return a.order < b.order;
        });
        std::vector<mw::core::PersistentMidiPitchBendEvent> collapsedPitchBends;
        for (const auto& event : pitchBendEvents_)
        {
            if (!collapsedPitchBends.empty() && collapsedPitchBends.back().tick == event.tick
                && collapsedPitchBends.back().midiChannel == event.midiChannel)
                collapsedPitchBends.back() = event;
            else
                collapsedPitchBends.push_back(event);
        }

        std::stable_sort(programChangeEvents_.begin(), programChangeEvents_.end(), [](const auto& a, const auto& b)
        {
            if (a.tick != b.tick) return a.tick < b.tick;
            if (a.midiChannel != b.midiChannel) return a.midiChannel < b.midiChannel;
            return a.order < b.order;
        });
        std::vector<mw::core::PersistentMidiProgramChangeEvent> collapsedProgramChanges;
        for (const auto& event : programChangeEvents_)
        {
            if (!collapsedProgramChanges.empty() && collapsedProgramChanges.back().tick == event.tick
                && collapsedProgramChanges.back().midiChannel == event.midiChannel)
                collapsedProgramChanges.back() = event;
            else
                collapsedProgramChanges.push_back(event);
        }

        std::stable_sort(channelPressureEvents_.begin(), channelPressureEvents_.end(), [](const auto& a, const auto& b)
        {
            if (a.tick != b.tick) return a.tick < b.tick;
            if (a.midiChannel != b.midiChannel) return a.midiChannel < b.midiChannel;
            return a.order < b.order;
        });
        std::vector<mw::core::PersistentMidiChannelPressureEvent> collapsedChannelPressure;
        for (const auto& event : channelPressureEvents_)
        {
            if (!collapsedChannelPressure.empty() && collapsedChannelPressure.back().tick == event.tick
                && collapsedChannelPressure.back().midiChannel == event.midiChannel)
                collapsedChannelPressure.back() = event;
            else
                collapsedChannelPressure.push_back(event);
        }

        std::stable_sort(polyPressureEvents_.begin(), polyPressureEvents_.end(), [](const auto& a, const auto& b)
        {
            if (a.tick != b.tick) return a.tick < b.tick;
            if (a.midiChannel != b.midiChannel) return a.midiChannel < b.midiChannel;
            if (a.noteNumber != b.noteNumber) return a.noteNumber < b.noteNumber;
            return a.order < b.order;
        });
        std::vector<mw::core::PersistentMidiPolyPressureEvent> collapsedPolyPressure;
        for (const auto& event : polyPressureEvents_)
        {
            if (!collapsedPolyPressure.empty() && collapsedPolyPressure.back().tick == event.tick
                && collapsedPolyPressure.back().midiChannel == event.midiChannel
                && collapsedPolyPressure.back().noteNumber == event.noteNumber)
                collapsedPolyPressure.back() = event;
            else
                collapsedPolyPressure.push_back(event);
        }

        result.notes = std::move(completedNotes_);
        result.cc64Events = std::move(cc64Events_);
        if (config_.recordAdvancedParameters)
        {
            const auto decoded = decodeAdvancedMidiParameters(collapsedControllers);
            if (config_.recordControllers)
                result.controllerEvents = decoded.ordinaryControllers;
            result.controller14BitEvents = decoded.controller14BitEvents;
            result.rpnEvents = decoded.rpnEvents;
            result.nrpnEvents = decoded.nrpnEvents;
        }
        else if (config_.recordControllers)
        {
            result.controllerEvents = std::move(collapsedControllers);
        }
        result.pitchBendEvents = std::move(collapsedPitchBends);
        result.programChangeEvents = std::move(collapsedProgramChanges);
        result.channelPressureEvents = std::move(collapsedChannelPressure);
        result.polyPressureEvents = std::move(collapsedPolyPressure);
        result.sustainDownMaskAtStart = config_.sustainDownMaskAtStart;
        result.sustainDownMaskAtStop = sustainDownMask_;

        recording_ = false;
        activeNotes_.clear();
        completedNotes_.clear();
        cc64Events_.clear();
        controllerEvents_.clear();
        pitchBendEvents_.clear();
        programChangeEvents_.clear();
        channelPressureEvents_.clear();
        polyPressureEvents_.clear();
        sustainDownMask_ = 0;
        nextOrder_ = 1;
        nextCc64Order_ = 1;
        nextControllerOrder_ = 1;
        nextPitchBendOrder_ = 1;
        nextProgramChangeOrder_ = 1;
        nextChannelPressureOrder_ = 1;
        nextPolyPressureOrder_ = 1;
        return result;
    }
}
