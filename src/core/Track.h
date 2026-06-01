#pragma once
#include <string>
#include <utility>
#include <vector>

#include "core/InstrumentAssignment.h"
#include "core/NoteEvent.h"

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
        explicit Track(std::string n = "Untitled Track") : name(std::move(n))
        {
            instrument.displayName = name;
            instrument.originalImportedName = name;
            instrument.normalizedName = name;
        }

        void addNote(const NoteEvent& note) { notes.push_back(note); }

        const std::string& getName() const { return name; }
        void setName(std::string n) { name = std::move(n); }

        const std::vector<NoteEvent>& getNotes() const { return notes; }
        std::vector<NoteEvent>& getNotes() { return notes; }

        const MixerSettings& getMixerSettings() const { return mixer; }
        MixerSettings& getMixerSettings() { return mixer; }

        const InstrumentAssignment& getInstrument() const { return instrument; }
        InstrumentAssignment& getInstrument() { return instrument; }

        const VstEffectsAssignment& getVstEffects() const { return vstEffects; }
        VstEffectsAssignment& getVstEffects() { return vstEffects; }
        void setVstEffects(VstEffectsAssignment effects) { vstEffects = std::move(effects); }

        const std::string& getInstrumentName() const { return instrument.displayName; }
        void setInstrumentName(std::string n) { instrument.displayName = std::move(n); }
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
                instrument = makeCustomAudioInstrumentAssignment();
        }
        bool isAudioClipTrack() const { return trackType == TrackType::AudioClip; }

    private:
        std::string name;
        InstrumentAssignment instrument;
        bool muted = false;
        bool solo = false;
        TrackType trackType = TrackType::Midi;
        std::vector<NoteEvent> notes;
        MixerSettings mixer;
        VstEffectsAssignment vstEffects;
    };
}
