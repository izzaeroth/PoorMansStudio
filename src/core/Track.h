#pragma once
#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "core/InstrumentAssignment.h"
#include "core/StableId.h"
#include "core/NoteEvent.h"
#include "core/MidiControllerEvent.h"
#include "core/MidiPitchBendEvent.h"
#include "core/MidiProgramChangeEvent.h"
#include "core/MidiChannelPressureEvent.h"
#include "core/MidiPolyPressureEvent.h"
#include "core/Midi14BitControllerEvent.h"
#include "core/MidiRpnEvent.h"
#include "core/MidiNrpnEvent.h"
#include "core/MidiTakeReview.h"

namespace mw::core
{
    struct MixerSettings
    {
        float volume = 1.0f;
        float pan = 0.0f;
        float reverbSend = 0.0f;
        float humanizeAmount = 0.0f;
    };

    enum class TrackType
    {
        Midi,
        AudioClip
    };

    inline std::string trackTypeToString(TrackType type)
    {
        switch (type)
        {
            case TrackType::AudioClip: return "AudioClip";
            case TrackType::Midi:
            default: return "MIDI";
        }
    }

    inline TrackType trackTypeFromString(const std::string& value)
    {
        if (value == "AudioClip" || value == "audioClip" || value == "audio")
            return TrackType::AudioClip;

        return TrackType::Midi;
    }

    class Track
    {
    public:
        explicit Track(std::string n = "Untitled Track") : stableId(allocateStableId()), name(std::move(n))
        {
            instrument.displayName = name;
            instrument.originalImportedName = name;
            instrument.normalizedName = name;
        }

        void addNote(const NoteEvent& note) { notes.push_back(note); }

        StableId getStableId() const noexcept { return stableId; }
        void setStableId(StableId id) noexcept { stableId = id > 0 ? id : allocateStableId(); }

        const std::string& getName() const { return name; }
        void setName(std::string n) { name = std::move(n); }

        const std::vector<NoteEvent>& getNotes() const { return notes; }
        std::vector<NoteEvent>& getNotes() { return notes; }

        const std::vector<MidiControllerEvent>& getMidiControllerEvents() const noexcept { return midiControllerEvents; }
        std::vector<MidiControllerEvent>& getMidiControllerEvents() noexcept { return midiControllerEvents; }
        void setMidiControllerEvents(std::vector<MidiControllerEvent> events)
        {
            if (trackType == TrackType::AudioClip)
            {
                midiControllerEvents.clear();
                midiPitchBendEvents.clear();
                midiProgramChangeEvents.clear();
                midiChannelPressureEvents.clear();
                midiPolyPressureEvents.clear();
                midi14BitControllerEvents.clear();
                midiRpnEvents.clear();
                midiNrpnEvents.clear();
                return;
            }
            midiControllerEvents = std::move(events);
            std::sort(midiControllerEvents.begin(), midiControllerEvents.end(), midiControllerEventOrder);
        }
        void clearMidiControllerEvents() noexcept { midiControllerEvents.clear(); }

        const std::vector<MidiPitchBendEvent>& getMidiPitchBendEvents() const noexcept { return midiPitchBendEvents; }
        std::vector<MidiPitchBendEvent>& getMidiPitchBendEvents() noexcept { return midiPitchBendEvents; }
        void setMidiPitchBendEvents(std::vector<MidiPitchBendEvent> events)
        {
            if (trackType == TrackType::AudioClip)
            {
                midiPitchBendEvents.clear();
                return;
            }
            midiPitchBendEvents = std::move(events);
            std::sort(midiPitchBendEvents.begin(), midiPitchBendEvents.end(), midiPitchBendEventOrder);
        }
        void clearMidiPitchBendEvents() noexcept { midiPitchBendEvents.clear(); }

        const std::vector<MidiProgramChangeEvent>& getMidiProgramChangeEvents() const noexcept { return midiProgramChangeEvents; }
        std::vector<MidiProgramChangeEvent>& getMidiProgramChangeEvents() noexcept { return midiProgramChangeEvents; }
        void setMidiProgramChangeEvents(std::vector<MidiProgramChangeEvent> events)
        {
            if (trackType == TrackType::AudioClip)
            {
                midiProgramChangeEvents.clear();
                return;
            }
            midiProgramChangeEvents = std::move(events);
            std::sort(midiProgramChangeEvents.begin(), midiProgramChangeEvents.end(), midiProgramChangeEventOrder);
        }
        void clearMidiProgramChangeEvents() noexcept { midiProgramChangeEvents.clear(); }

        const std::vector<MidiChannelPressureEvent>& getMidiChannelPressureEvents() const noexcept { return midiChannelPressureEvents; }
        std::vector<MidiChannelPressureEvent>& getMidiChannelPressureEvents() noexcept { return midiChannelPressureEvents; }
        void setMidiChannelPressureEvents(std::vector<MidiChannelPressureEvent> events)
        {
            if (trackType == TrackType::AudioClip)
            {
                midiChannelPressureEvents.clear();
                return;
            }
            midiChannelPressureEvents = std::move(events);
            std::sort(midiChannelPressureEvents.begin(), midiChannelPressureEvents.end(), midiChannelPressureEventOrder);
        }
        void clearMidiChannelPressureEvents() noexcept { midiChannelPressureEvents.clear(); }

        const std::vector<MidiPolyPressureEvent>& getMidiPolyPressureEvents() const noexcept { return midiPolyPressureEvents; }
        std::vector<MidiPolyPressureEvent>& getMidiPolyPressureEvents() noexcept { return midiPolyPressureEvents; }
        void setMidiPolyPressureEvents(std::vector<MidiPolyPressureEvent> events)
        {
            if (trackType == TrackType::AudioClip)
            {
                midiPolyPressureEvents.clear();
                midi14BitControllerEvents.clear();
                midiRpnEvents.clear();
                midiNrpnEvents.clear();
                return;
            }
            midiPolyPressureEvents = std::move(events);
            std::sort(midiPolyPressureEvents.begin(), midiPolyPressureEvents.end(), midiPolyPressureEventOrder);
        }
        void clearMidiPolyPressureEvents() noexcept { midiPolyPressureEvents.clear(); }

        const std::vector<Midi14BitControllerEvent>& getMidi14BitControllerEvents() const noexcept { return midi14BitControllerEvents; }
        std::vector<Midi14BitControllerEvent>& getMidi14BitControllerEvents() noexcept { return midi14BitControllerEvents; }
        void setMidi14BitControllerEvents(std::vector<Midi14BitControllerEvent> events)
        {
            if (trackType == TrackType::AudioClip) { midi14BitControllerEvents.clear(); return; }
            midi14BitControllerEvents = std::move(events);
            std::sort(midi14BitControllerEvents.begin(), midi14BitControllerEvents.end(), midi14BitControllerEventOrder);
        }
        void clearMidi14BitControllerEvents() noexcept { midi14BitControllerEvents.clear(); }

        const std::vector<MidiRpnEvent>& getMidiRpnEvents() const noexcept { return midiRpnEvents; }
        std::vector<MidiRpnEvent>& getMidiRpnEvents() noexcept { return midiRpnEvents; }
        void setMidiRpnEvents(std::vector<MidiRpnEvent> events)
        {
            if (trackType == TrackType::AudioClip) { midiRpnEvents.clear(); return; }
            midiRpnEvents = std::move(events);
            std::sort(midiRpnEvents.begin(), midiRpnEvents.end(), midiRpnEventOrder);
        }
        void clearMidiRpnEvents() noexcept { midiRpnEvents.clear(); }

        const std::vector<MidiNrpnEvent>& getMidiNrpnEvents() const noexcept { return midiNrpnEvents; }
        std::vector<MidiNrpnEvent>& getMidiNrpnEvents() noexcept { return midiNrpnEvents; }
        void setMidiNrpnEvents(std::vector<MidiNrpnEvent> events)
        {
            if (trackType == TrackType::AudioClip) { midiNrpnEvents.clear(); return; }
            midiNrpnEvents = std::move(events);
            std::sort(midiNrpnEvents.begin(), midiNrpnEvents.end(), midiNrpnEventOrder);
        }
        void clearMidiNrpnEvents() noexcept { midiNrpnEvents.clear(); }

        const PersistentMidiTakeReview& getPendingMidiTakeReview() const noexcept { return pendingMidiTakeReview; }
        PersistentMidiTakeReview& getPendingMidiTakeReview() noexcept { return pendingMidiTakeReview; }
        void setPendingMidiTakeReview(PersistentMidiTakeReview state)
        {
            if (state.active)
            {
                if (state.reviewSessionId == 0
                    || std::any_of(archivedMidiTakeReviews.begin(), archivedMidiTakeReviews.end(), [&](const auto& archived)
                    {
                        return archived.reviewSessionId == state.reviewSessionId;
                    }))
                    state.reviewSessionId = allocateStableId();
                if (state.reviewName.empty())
                    state.reviewName = "Take Review " + std::to_string(archivedMidiTakeReviews.size() + 1U);
            }
            pendingMidiTakeReview = std::move(state);
        }
        void clearPendingMidiTakeReview() noexcept { pendingMidiTakeReview.clear(); }

        const std::vector<PersistentMidiTakeReview>& getArchivedMidiTakeReviews() const noexcept { return archivedMidiTakeReviews; }
        std::vector<PersistentMidiTakeReview>& getArchivedMidiTakeReviews() noexcept { return archivedMidiTakeReviews; }
        void setArchivedMidiTakeReviews(std::vector<PersistentMidiTakeReview> reviews) { archivedMidiTakeReviews = std::move(reviews); }
        void clearArchivedMidiTakeReviews() noexcept { archivedMidiTakeReviews.clear(); }

        const MixerSettings& getMixerSettings() const { return mixer; }
        MixerSettings& getMixerSettings() { return mixer; }

        const InstrumentAssignment& getInstrument() const { return instrument; }
        InstrumentAssignment& getInstrument() { return instrument; }

        const VstEffectsAssignment& getVstEffects() const { return vstEffects; }
        VstEffectsAssignment& getVstEffects() { return vstEffects; }
        void setVstEffects(VstEffectsAssignment effects) { vstEffects = std::move(effects); }

        const std::string& getInstrumentName() const { return instrument.displayName; }
        void setInstrumentAssignment(InstrumentAssignment a)
        {
            instrument = (trackType == TrackType::AudioClip)
                ? makeCustomAudioInstrumentAssignment()
                : std::move(a);
        }

        bool getMuted() const { return muted; }
        void setMuted(bool shouldMute) { muted = shouldMute; }

        bool getSolo() const { return solo; }
        void setSolo(bool shouldSolo) { solo = shouldSolo; }

        TrackType getTrackType() const { return trackType; }
        void setTrackType(TrackType type)
        {
            trackType = type;
            if (trackType == TrackType::AudioClip)
            {
                instrument = makeCustomAudioInstrumentAssignment();
                pendingMidiTakeReview.clear();
                archivedMidiTakeReviews.clear();
                midiControllerEvents.clear();
                midiPitchBendEvents.clear();
                midiProgramChangeEvents.clear();
                midiChannelPressureEvents.clear();
                midiPolyPressureEvents.clear();
                midi14BitControllerEvents.clear();
                midiRpnEvents.clear();
                midiNrpnEvents.clear();
            }
        }
        bool isAudioClipTrack() const { return trackType == TrackType::AudioClip; }

    private:
        StableId stableId = 0;
        std::string name;
        InstrumentAssignment instrument;
        bool muted = false;
        bool solo = false;
        TrackType trackType = TrackType::Midi;
        std::vector<NoteEvent> notes;
        std::vector<MidiControllerEvent> midiControllerEvents;
        std::vector<MidiPitchBendEvent> midiPitchBendEvents;
        std::vector<MidiProgramChangeEvent> midiProgramChangeEvents;
        std::vector<MidiChannelPressureEvent> midiChannelPressureEvents;
        std::vector<MidiPolyPressureEvent> midiPolyPressureEvents;
        std::vector<Midi14BitControllerEvent> midi14BitControllerEvents;
        std::vector<MidiRpnEvent> midiRpnEvents;
        std::vector<MidiNrpnEvent> midiNrpnEvents;
        PersistentMidiTakeReview pendingMidiTakeReview;
        std::vector<PersistentMidiTakeReview> archivedMidiTakeReviews;
        MixerSettings mixer;
        VstEffectsAssignment vstEffects;
    };
}
