#pragma once

#include <filesystem>
#include <string>

namespace mw::app
{
    struct UserPreferences
    {
        std::filesystem::path lastInputFolder;
        std::filesystem::path lastExportFolder;
        std::filesystem::path lastSoundFontPath;
        std::filesystem::path lastSfzPath;

        std::string lastBaseFileName = "rendered_score";
        int lastOutputFormatId = 1;
        int lastAudioClipFormatId = 1;
        int lastAudioClipQualityKbps = 320;
        int lastBackendId = 1;
        int lastSampleRate = 48000;
        int lastBitrateKbps = 192;
        int lastChannelCount = 2;
        int lastRenderWorkerCount = 0; // 0 = Auto
        int keepStemFilesMask = 3; // 0 = none, 1 = WAV stems, 2 = MIDI stems, 3 = WAV + MIDI
        bool suppressSfzRenderWarning = false;
        int themePresetId = 1;
        bool helperBubblesEnabled = true;
    };

    class UserPreferencesStore
    {
    public:
        static UserPreferences load();
        static bool save(const UserPreferences& preferences);
    };
}
