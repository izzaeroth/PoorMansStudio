#pragma once

#include "core/MidiTakeReview.h"
#include "core/NoteEvent.h"

#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <vector>

namespace mw::midi
{
    struct MidiRecordingConfig
    {
        int tempoBpm = 120;
        int ticksPerQuarterNote = 960;
        std::int64_t startTick = 0;
        double startTimeSeconds = 0.0;
        std::uint16_t sustainDownMaskAtStart = 0;
        bool recordControllers = false;
        bool recordPitchBend = false;
        bool recordProgramChange = false;
        bool recordChannelPressure = false;
        bool recordPolyPressure = false;
        bool recordAdvancedParameters = false;
    };

    struct MidiRecordingStopResult
    {
        std::vector<mw::core::NoteEvent> notes;
        std::vector<mw::core::PersistentMidiCc64Event> cc64Events;
        std::vector<mw::core::PersistentMidiControllerEvent> controllerEvents;
        std::vector<mw::core::PersistentMidiPitchBendEvent> pitchBendEvents;
        std::vector<mw::core::PersistentMidiProgramChangeEvent> programChangeEvents;
        std::vector<mw::core::PersistentMidiChannelPressureEvent> channelPressureEvents;
        std::vector<mw::core::PersistentMidiPolyPressureEvent> polyPressureEvents;
        std::vector<mw::core::PersistentMidi14BitControllerEvent> controller14BitEvents;
        std::vector<mw::core::PersistentMidiRpnEvent> rpnEvents;
        std::vector<mw::core::PersistentMidiNrpnEvent> nrpnEvents;
        std::int64_t stopTick = 0;
        std::size_t finalizedHeldNoteCount = 0;
        std::uint16_t sustainDownMaskAtStart = 0;
        std::uint16_t sustainDownMaskAtStop = 0;
    };

    class MidiRecordingSession final
    {
    public:
        bool start(const MidiRecordingConfig& config);
        void cancel();

        bool isRecording() const;
        MidiRecordingConfig config() const;
        std::int64_t tickForTime(double eventTimeSeconds) const;

        void noteOn(int midiChannel, int pitch, int velocity, double eventTimeSeconds);
        void noteOff(int midiChannel, int pitch, double eventTimeSeconds);
        void controller(int midiChannel, int controllerNumber, int value, double eventTimeSeconds);
        void controller64(int midiChannel, int value, double eventTimeSeconds);
        void pitchBend(int midiChannel, int value, double eventTimeSeconds);
        void programChange(int midiChannel, int program, double eventTimeSeconds);
        void channelPressure(int midiChannel, int pressure, double eventTimeSeconds);
        void polyPressure(int midiChannel, int noteNumber, int pressure, double eventTimeSeconds);

        // Finalizes every currently-held note at the supplied time while leaving the
        // recording session active. Panic also forces any captured CC64 sustain state
        // Up at this same project tick so later notes cannot inherit a stuck pedal.
        std::size_t releaseActiveNotes(double eventTimeSeconds);

        MidiRecordingStopResult stop(double eventTimeSeconds);

    private:
        struct ActiveNote
        {
            int midiChannel = 1;
            int pitch = 60;
            int velocity = 100;
            std::int64_t startTick = 0;
            std::uint64_t order = 0;
        };

        using ActiveNoteKey = int;

        static int sanitizeChannel(int channel) noexcept;
        static int sanitizePitch(int pitch) noexcept;
        static int sanitizeVelocity(int velocity) noexcept;
        static int sanitizeControllerValue(int value) noexcept;
        static ActiveNoteKey makeKey(int channel, int pitch) noexcept;
        static std::uint16_t channelMask(int channel) noexcept;

        std::int64_t tickForTimeLocked(double eventTimeSeconds) const;
        void finalizeNoteLocked(const ActiveNote& active, std::int64_t endTick);
        std::size_t releaseActiveNotesLocked(std::int64_t endTick);
        void forceSustainUpLocked(std::int64_t tick);

        mutable std::mutex mutex_;
        bool recording_ = false;
        MidiRecordingConfig config_;
        std::map<ActiveNoteKey, std::deque<ActiveNote>> activeNotes_;
        std::vector<mw::core::NoteEvent> completedNotes_;
        std::vector<mw::core::PersistentMidiCc64Event> cc64Events_;
        std::vector<mw::core::PersistentMidiControllerEvent> controllerEvents_;
        std::vector<mw::core::PersistentMidiPitchBendEvent> pitchBendEvents_;
        std::vector<mw::core::PersistentMidiProgramChangeEvent> programChangeEvents_;
        std::vector<mw::core::PersistentMidiChannelPressureEvent> channelPressureEvents_;
        std::vector<mw::core::PersistentMidiPolyPressureEvent> polyPressureEvents_;
        std::uint16_t sustainDownMask_ = 0;
        std::uint64_t nextOrder_ = 1;
        std::uint64_t nextCc64Order_ = 1;
        std::uint64_t nextControllerOrder_ = 1;
        std::uint64_t nextPitchBendOrder_ = 1;
        std::uint64_t nextProgramChangeOrder_ = 1;
        std::uint64_t nextChannelPressureOrder_ = 1;
        std::uint64_t nextPolyPressureOrder_ = 1;
    };
}
