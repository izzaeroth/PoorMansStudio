#pragma once
#include <algorithm>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "core/Track.h"
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
        std::string projectFileExtension = ".mwproj";
        std::string projectFormatName = "MusicWorkstationProject";
        int projectFormatVersion = 7;
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

        // Project-level VST3 default. This is the source used when the project
        // backend is set to VST3 before importing/adding tracks. Each track still
        // receives its own independent plugin assignment and state after seeding.
        std::filesystem::path vst3PluginPath;
        std::string vst3PluginName;
        std::string vst3PluginVendor;
        std::string vst3PluginVersion;
        std::string vst3PluginCategory;
        std::string vst3PluginUid;
        std::string vst3PluginCompatibilitySummary;

        std::string baseFileName = "rendered_score";

        std::string metadataTitle;
        std::string metadataArtist;
        std::string metadataAlbum;
        std::string metadataTrackNumber;
        std::string metadataYear;

        int backendId = 1;      // 1 = SF2/FluidSynth, 2 = SFZ/sfizz-render, 3 = VST3 Plugin
        int outputFormatId = 1; // 1 = WAV, 2 = FLAC, 3 = MP3, 4 = OGG
        int audioClipFormatId = 1; // 1 = WAV, 2 = FLAC, 3 = MP3, 4 = OGG
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

        explicit Project(std::string n = "Untitled Project") : name(std::move(n)) {}

        Track& addTrack(std::string n)
        {
            tracks.emplace_back(std::move(n));
            return tracks.back();
        }

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

        int allocateNextAudioClipId() const
        {
            int maxId = 0;
            for (const auto& clip : audioClips)
                maxId = std::max(maxId, clip.id);
            return maxId + 1;
        }

    private:
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
