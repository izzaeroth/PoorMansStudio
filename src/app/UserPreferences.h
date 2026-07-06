#pragma once

#include <filesystem>
#include <string>
#include <vector>
#include <utility>

namespace mw::app
{
    struct UserPreferences
    {
        std::filesystem::path lastInputFolder;
        std::filesystem::path lastExportFolder;
        std::filesystem::path lastSoundFontPath;
        std::filesystem::path lastSfzPath;
        std::filesystem::path lastVst3Folder;

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

        bool vstCompatibilityWarningsEnabled = true;
        bool vstSafePluginUiMode = false;
        int vstWarningStyleId = 1; // 1 = Auto, 2 = Conservative, 3 = Minimal
        bool vstGraphicsProfileDetected = false;
        std::string vstGraphicsProfileSource;
        std::string vstGraphicsProfileLastDetected;
        int vstPreferredPluginGpuIndex = 1; // 1 = System Default / Auto, 2+ = cached adapter row
        std::string vstPreferredPluginGpuId = "auto"; // legacy read-only fallback
        int vstMaxOpenPluginWindows = 4;
        std::string vstGraphicsProfileSummary;
        bool vstExperimentalWarningAcknowledged = false;

        bool clapCompatibilityWarningsEnabled = true;
        bool clapExperimentalWarningAcknowledged = false;
        bool clapSafePluginUiMode = false;
        int clapMaxOpenPluginWindows = 4;
    };

    class UserPreferencesStore
    {
    public:
        static UserPreferences load();
        static bool save(const UserPreferences& preferences);
        static bool saveValue(const std::string& key, const std::string& value);
        static bool saveValues(const std::vector<std::pair<std::string, std::string>>& values);
        static bool saveBoolValue(const std::string& key, bool value);
        static bool saveIntValue(const std::string& key, int value);
    };
}
