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
        bool pluginExperimentalWarningAcknowledged = false;

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

        bool clapCompatibilityWarningsEnabled = true;
        bool clapSafePluginUiMode = false;
        int clapMaxOpenPluginWindows = 4;

        std::string audioDeviceTypeName;
        std::string audioInputDeviceName;
        std::string audioOutputDeviceName;
        int audioSampleRate = 0; // 0 = device default
        int audioBufferSize = 0; // 0 = device default
        int audioInputMode = 1; // 1 = mono, 2 = adjacent stereo pair
        int audioInputChannelIndex = 0; // zero-based physical channel/pair start

        std::string midiInputDeviceIdentifier;
        std::string midiInputDeviceName;
        int midiInputChannel = 0; // 0 = Omni, 1-16 = exact channel
        int midiRecordingCountInBars = 0; // 0 = Off, 1 = one bar, 2 = two bars
        int midiRecordingQuantizeDivision = 0; // 0 = Off; otherwise 4, 8, 16, 32, or 64
        bool midiRecordingMetronomeEnabled = false; // continuous click only while MIDI recording is active
        bool midiRecordingRecordControllers = false; // Off by default; when On capture incoming CC 0-127 into project/take review
        bool midiRecordingRecordPitchBend = false; // Off by default; when On capture channel Pitch Bend into project/take review
        bool midiRecordingRecordProgramChange = false; // Off by default; when On capture channel Program Change into project/take review
        bool midiRecordingRecordChannelPressure = false; // Off by default; when On capture channel pressure into project/take review
        bool midiRecordingRecordPolyPressure = false; // Off by default; when On capture per-key poly aftertouch into project/take review
        bool midiRecordingRecordAdvancedParameters = false; // Off by default; when On decode 14-bit CC/RPN/NRPN transactions
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
