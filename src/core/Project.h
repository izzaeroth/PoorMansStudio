#pragma once
#include <algorithm>
#include <filesystem>
#include <string>
#include <utility>
#include <unordered_set>
#include <vector>

#include "core/Track.h"
#include "core/StableId.h"
#include "core/AudioClip.h"

namespace mw::core
{
    struct TimeSignature
    {
        int numerator = 4;
        int denominator = 4;
    };

    struct ProjectMetadata
    {
        std::string projectFormatName = "MusicWorkstationProject";
        int projectFormatVersion = 22;
    };

    struct ProjectSequenceMetadata
    {
        int id = 0;
        int number = 0;
        std::string type = "sequence";
        std::string name;
        std::filesystem::path sourceFile;
        std::string createdBy = "manual";
        std::string notes;
        bool locked = false;
        long long startTick = 0;
        long long endTick = 0;
        std::vector<int> tracks;
        std::string color;
    };

    struct ProjectUserSettings
    {
        std::filesystem::path sourceInputPath;
        std::filesystem::path exportFolder;
        std::filesystem::path soundFontPath;
        std::filesystem::path fluidSynthPath;
        std::filesystem::path ffmpegPath;
        std::filesystem::path sfzPath;
        std::filesystem::path sfizzRenderPath;

        // Project-level plugin defaults. These are the sources used when the project
        // backend is set to VST3 or CLAP before importing/adding tracks. Each track
        // still receives its own independent plugin assignment and state after seeding.
        std::filesystem::path vst3PluginPath;
        std::string vst3PluginName;
        std::string vst3PluginVendor;
        std::string vst3PluginVersion;
        std::string vst3PluginCategory;
        std::string vst3PluginUid;
        std::string vst3PluginCompatibilitySummary;

        std::filesystem::path clapPluginPath;
        std::string clapPluginName;
        std::string clapPluginVendor;
        std::string clapPluginVersion;
        std::string clapPluginCategory;
        std::string clapPluginUid;
        std::string clapPluginCompatibilitySummary;

        std::string baseFileName = "rendered_score";

        std::string metadataTitle;
        std::string metadataArtist; // contributing artist/performer tag
        std::string metadataAlbumArtist;
        std::string metadataAlbum;
        std::string metadataGenre;
        std::string metadataTrackNumber;
        std::string metadataYear;

        bool albumArtEnabled = false;
        std::filesystem::path albumArtPath;

        int backendId = 1;      // 1 = SF2/FluidSynth, 2 = SFZ/sfizz-render, 3 = VST3 Plugin, 4 = CLAP Plugin
        int outputFormatId = 1; // 1 = WAV, 2 = FLAC, 3 = MP3, 4 = OGG, 5 = M4A
        int audioClipFormatId = 1; // 1 = WAV, 2 = FLAC, 3 = MP3, 4 = OGG, 5 = M4A
        int audioClipQualityKbps = 320;
        int sampleRate = 48000;
        int bitrateKbps = 192;
        int channelCount = 2;
        int renderWorkerCount = 0; // 0 = Auto, 1/2/4/6/8/12/16 = fixed manual parallel stem count

        int sfzKeySwitch = 24;
        int sfzCc1 = 100;
        int sfzCc11 = 127;

        float masterVolume = 1.0f;
    };

    class Project
    {
    public:
        static constexpr int ticksPerQuarterNote = 960;
        static constexpr int maxTrackCount = 300;
        static constexpr int maxSequenceCount = 300;

        explicit Project(std::string n = "Untitled Project") : stableId(allocateStableId()), name(std::move(n)) {}

        Track& addTrack(std::string n)
        {
            tracks.emplace_back(std::move(n));
            return tracks.back();
        }

        StableId getStableId() const noexcept { return stableId; }
        void setStableId(StableId id) noexcept { stableId = id > 0 ? id : allocateStableId(); }

        const std::string& getName() const { return name; }
        void setName(std::string n) { name = std::move(n); }

        int getTempoBpm() const { return tempoBpm; }
        void setTempoBpm(int bpm) { tempoBpm = bpm; }

        const TimeSignature& getTimeSignature() const { return timeSignature; }
        void setTimeSignature(TimeSignature ts) { timeSignature = ts; }

        const std::vector<Track>& getTracks() const { return tracks; }
        std::vector<Track>& getTracks() { return tracks; }

        const ProjectMetadata& getMetadata() const { return metadata; }

        const ProjectUserSettings& getUserSettings() const { return userSettings; }
        ProjectUserSettings& getUserSettings() { return userSettings; }
        void setUserSettings(ProjectUserSettings settings) { userSettings = std::move(settings); }

        const std::vector<ProjectSequenceMetadata>& getSequences() const { return sequences; }
        std::vector<ProjectSequenceMetadata>& getSequences() { return sequences; }
        void setSequences(std::vector<ProjectSequenceMetadata> newSequences) { sequences = std::move(newSequences); }

        const std::vector<AudioClip>& getAudioClips() const { return audioClips; }
        std::vector<AudioClip>& getAudioClips() { return audioClips; }
        void setAudioClips(std::vector<AudioClip> newAudioClips) { audioClips = std::move(newAudioClips); }


        void ensureUniqueStableIds()
        {
            if (stableId == 0)
                stableId = allocateStableId();

            std::unordered_set<StableId> seen;
            for (auto& track : tracks)
            {
                auto id = track.getStableId();
                if (id == 0 || !seen.insert(id).second)
                {
                    do
                    {
                        id = allocateStableId();
                    }
                    while (!seen.insert(id).second);
                    track.setStableId(id);
                }
            }

            std::unordered_set<StableId> seenControllerEventIds;
            for (auto& track : tracks)
            {
                if (track.isAudioClipTrack())
                {
                    track.clearMidiControllerEvents();
                    continue;
                }

                for (auto& event : track.getMidiControllerEvents())
                {
                    auto eventId = event.stableId;
                    if (eventId == 0 || !seenControllerEventIds.insert(eventId).second)
                    {
                        do
                        {
                            eventId = allocateStableId();
                        }
                        while (!seenControllerEventIds.insert(eventId).second);
                        event.stableId = eventId;
                    }
                }
                std::sort(track.getMidiControllerEvents().begin(), track.getMidiControllerEvents().end(), midiControllerEventOrder);
            }

            std::unordered_set<StableId> seenPitchBendEventIds;
            for (auto& track : tracks)
            {
                if (track.isAudioClipTrack())
                {
                    track.clearMidiPitchBendEvents();
                    continue;
                }

                for (auto& event : track.getMidiPitchBendEvents())
                {
                    auto eventId = event.stableId;
                    if (eventId == 0 || !seenPitchBendEventIds.insert(eventId).second)
                    {
                        do
                        {
                            eventId = allocateStableId();
                        }
                        while (!seenPitchBendEventIds.insert(eventId).second);
                        event.stableId = eventId;
                    }
                }
                std::sort(track.getMidiPitchBendEvents().begin(), track.getMidiPitchBendEvents().end(), midiPitchBendEventOrder);
            }

            std::unordered_set<StableId> seenProgramChangeEventIds;
            for (auto& track : tracks)
            {
                if (track.isAudioClipTrack())
                {
                    track.clearMidiProgramChangeEvents();
                    continue;
                }

                for (auto& event : track.getMidiProgramChangeEvents())
                {
                    auto eventId = event.stableId;
                    if (eventId == 0 || !seenProgramChangeEventIds.insert(eventId).second)
                    {
                        do
                        {
                            eventId = allocateStableId();
                        }
                        while (!seenProgramChangeEventIds.insert(eventId).second);
                        event.stableId = eventId;
                    }
                }
                std::sort(track.getMidiProgramChangeEvents().begin(), track.getMidiProgramChangeEvents().end(), midiProgramChangeEventOrder);
            }

            std::unordered_set<StableId> seenChannelPressureEventIds;
            for (auto& track : tracks)
            {
                if (track.isAudioClipTrack())
                {
                    track.clearMidiChannelPressureEvents();
                    continue;
                }

                for (auto& event : track.getMidiChannelPressureEvents())
                {
                    auto eventId = event.stableId;
                    if (eventId == 0 || !seenChannelPressureEventIds.insert(eventId).second)
                    {
                        do { eventId = allocateStableId(); } while (!seenChannelPressureEventIds.insert(eventId).second);
                        event.stableId = eventId;
                    }
                }
                std::sort(track.getMidiChannelPressureEvents().begin(), track.getMidiChannelPressureEvents().end(), midiChannelPressureEventOrder);
            }

            std::unordered_set<StableId> seenPolyPressureEventIds;
            for (auto& track : tracks)
            {
                if (track.isAudioClipTrack())
                {
                    track.clearMidiPolyPressureEvents();
                    continue;
                }

                for (auto& event : track.getMidiPolyPressureEvents())
                {
                    auto eventId = event.stableId;
                    if (eventId == 0 || !seenPolyPressureEventIds.insert(eventId).second)
                    {
                        do { eventId = allocateStableId(); } while (!seenPolyPressureEventIds.insert(eventId).second);
                        event.stableId = eventId;
                    }
                }
                std::sort(track.getMidiPolyPressureEvents().begin(), track.getMidiPolyPressureEvents().end(), midiPolyPressureEventOrder);
            }

            std::unordered_set<StableId> seen14BitEventIds;
            for (auto& track : tracks)
            {
                if (track.isAudioClipTrack()) { track.clearMidi14BitControllerEvents(); continue; }
                for (auto& event : track.getMidi14BitControllerEvents())
                {
                    auto eventId = event.stableId;
                    if (eventId == 0 || !seen14BitEventIds.insert(eventId).second)
                    {
                        do { eventId = allocateStableId(); } while (!seen14BitEventIds.insert(eventId).second);
                        event.stableId = eventId;
                    }
                }
                std::sort(track.getMidi14BitControllerEvents().begin(), track.getMidi14BitControllerEvents().end(), midi14BitControllerEventOrder);
            }

            std::unordered_set<StableId> seenRpnEventIds;
            for (auto& track : tracks)
            {
                if (track.isAudioClipTrack()) { track.clearMidiRpnEvents(); continue; }
                for (auto& event : track.getMidiRpnEvents())
                {
                    auto eventId = event.stableId;
                    if (eventId == 0 || !seenRpnEventIds.insert(eventId).second)
                    {
                        do { eventId = allocateStableId(); } while (!seenRpnEventIds.insert(eventId).second);
                        event.stableId = eventId;
                    }
                }
                std::sort(track.getMidiRpnEvents().begin(), track.getMidiRpnEvents().end(), midiRpnEventOrder);
            }

            std::unordered_set<StableId> seenNrpnEventIds;
            for (auto& track : tracks)
            {
                if (track.isAudioClipTrack()) { track.clearMidiNrpnEvents(); continue; }
                for (auto& event : track.getMidiNrpnEvents())
                {
                    auto eventId = event.stableId;
                    if (eventId == 0 || !seenNrpnEventIds.insert(eventId).second)
                    {
                        do { eventId = allocateStableId(); } while (!seenNrpnEventIds.insert(eventId).second);
                        event.stableId = eventId;
                    }
                }
                std::sort(track.getMidiNrpnEvents().begin(), track.getMidiNrpnEvents().end(), midiNrpnEventOrder);
            }
        }

        int allocateNextAudioClipId() const
        {
            int maxId = 0;
            for (const auto& clip : audioClips)
                maxId = std::max(maxId, clip.id);
            return maxId + 1;
        }

    private:
        StableId stableId = 0;
        std::string name;
        int tempoBpm = 120;
        TimeSignature timeSignature {};
        ProjectMetadata metadata {};
        ProjectUserSettings userSettings {};
        std::vector<ProjectSequenceMetadata> sequences;
        std::vector<AudioClip> audioClips;
        std::vector<Track> tracks;
    };
}
