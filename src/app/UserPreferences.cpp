#include "app/UserPreferences.h"

#include "app/AppPaths.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <string>
#include <system_error>
#include <set>
#include <vector>
#include <utility>

namespace
{
    std::string trim(std::string value)
    {
        const auto first = value.find_first_not_of(" \t\r\n");
        if (first == std::string::npos)
            return {};

        const auto last = value.find_last_not_of(" \t\r\n");
        return value.substr(first, last - first + 1);
    }

    std::map<std::string, std::string> readKeyValueFile(const std::filesystem::path& path)
    {
        std::map<std::string, std::string> values;
        std::ifstream file(path);

        if (!file)
            return values;

        std::string line;
        while (std::getline(file, line))
        {
            const auto equals = line.find('=');

            if (equals == std::string::npos)
                continue;

            const auto key = trim(line.substr(0, equals));
            const auto value = trim(line.substr(equals + 1));

            if (!key.empty())
                values[key] = value;
        }

        return values;
    }

    std::filesystem::path getPath(
        const std::map<std::string, std::string>& values,
        const std::string& key,
        const std::filesystem::path& fallback
    )
    {
        const auto it = values.find(key);

        if (it == values.end() || it->second.empty())
            return fallback;

        return std::filesystem::path(it->second);
    }

    std::string getString(
        const std::map<std::string, std::string>& values,
        const std::string& key,
        const std::string& fallback
    )
    {
        const auto it = values.find(key);

        if (it == values.end())
            return fallback;

        return it->second;
    }

    int getInt(
        const std::map<std::string, std::string>& values,
        const std::string& key,
        int fallback
    )
    {
        const auto it = values.find(key);

        if (it == values.end())
            return fallback;

        try
        {
            return std::stoi(it->second);
        }
        catch (...)
        {
            return fallback;
        }
    }

    int parseStemFileMask(const std::string& rawValue, int fallback)
    {
        const auto value = trim(rawValue);
        if (value.empty())
            return fallback;

        if (value == "0" || value == "none" || value == "None")
            return 0;

        if (value == "3")
            return 3;

        int mask = 0;
        std::size_t start = 0;
        while (start <= value.size())
        {
            const auto comma = value.find(',', start);
            const auto token = trim(value.substr(start, comma == std::string::npos ? std::string::npos : comma - start));

            if (token == "1" || token == "wav" || token == "WAV")
                mask |= 1;
            else if (token == "2" || token == "mid" || token == "MID" || token == "midi" || token == "MIDI")
                mask |= 2;

            if (comma == std::string::npos)
                break;

            start = comma + 1;
        }

        return std::clamp(mask, 0, 3);
    }

    std::string formatStemFileMask(int mask)
    {
        mask = std::clamp(mask, 0, 3);
        if (mask == 0)
            return "0";
        if (mask == 1)
            return "1";
        if (mask == 2)
            return "2";
        return "1,2";
    }
}

namespace mw::app
{
    UserPreferences UserPreferencesStore::load()
    {
        UserPreferences preferences;

        preferences.lastInputFolder = AppPaths::inputFolder();
        preferences.lastExportFolder = AppPaths::exportsFolder();
        preferences.lastSoundFontPath = AppPaths::findFirstSoundFont();
        preferences.lastSfzPath = AppPaths::sfzFolder();
        preferences.lastVst3Folder = AppPaths::vst3Folder();

        const auto values = readKeyValueFile(AppPaths::preferencesFilePath());

        preferences.lastInputFolder = getPath(values, "lastInputFolder", preferences.lastInputFolder);
        preferences.lastExportFolder = getPath(values, "lastExportFolder", preferences.lastExportFolder);
        preferences.lastSoundFontPath = getPath(values, "lastSoundFontPath", preferences.lastSoundFontPath);
        preferences.lastSfzPath = getPath(values, "lastSfzPath", preferences.lastSfzPath);
        preferences.lastVst3Folder = getPath(values, "lastVst3Folder", preferences.lastVst3Folder);

        preferences.lastOutputFormatId = getInt(values, "lastOutputFormatId", preferences.lastOutputFormatId);
        preferences.lastAudioClipFormatId = getInt(values, "lastAudioClipFormatId", preferences.lastAudioClipFormatId);
        preferences.lastAudioClipQualityKbps = getInt(values, "lastAudioClipQualityKbps", preferences.lastAudioClipQualityKbps);
        preferences.lastBackendId = getInt(values, "lastBackendId", preferences.lastBackendId);
        preferences.lastSampleRate = getInt(values, "lastSampleRate", preferences.lastSampleRate);
        preferences.lastBitrateKbps = getInt(values, "lastBitrateKbps", preferences.lastBitrateKbps);
        preferences.lastChannelCount = getInt(values, "lastChannelCount", preferences.lastChannelCount);
        preferences.lastRenderWorkerCount = getInt(values, "lastRenderWorkerCount", preferences.lastRenderWorkerCount);
        preferences.keepStemFilesMask = parseStemFileMask(getString(values, "keepStemFiles", formatStemFileMask(preferences.keepStemFilesMask)), preferences.keepStemFilesMask);
        preferences.suppressSfzRenderWarning = getInt(values, "suppressSfzRenderWarning", preferences.suppressSfzRenderWarning ? 1 : 0) != 0;
        preferences.themePresetId = getInt(values, "themePresetId", preferences.themePresetId);
        preferences.helperBubblesEnabled = getInt(values, "helperBubblesEnabled", preferences.helperBubblesEnabled ? 1 : 0) != 0;
        preferences.vstCompatibilityWarningsEnabled = getInt(values, "vstCompatibilityWarningsEnabled", preferences.vstCompatibilityWarningsEnabled ? 1 : 0) != 0;
        preferences.vstSafePluginUiMode = getInt(values, "vstSafePluginUiMode", preferences.vstSafePluginUiMode ? 1 : 0) != 0;
        preferences.vstWarningStyleId = getInt(values, "vstWarningStyleId", preferences.vstWarningStyleId);
        preferences.vstGraphicsProfileDetected = getInt(values, "vstGraphicsProfileDetected", preferences.vstGraphicsProfileDetected ? 1 : 0) != 0;
        preferences.vstGraphicsProfileSource = getString(values, "vstGraphicsProfileSource", preferences.vstGraphicsProfileSource);
        preferences.vstGraphicsProfileLastDetected = getString(values, "vstGraphicsProfileLastDetected", preferences.vstGraphicsProfileLastDetected);
        preferences.vstPreferredPluginGpuIndex = getInt(values, "vstPreferredPluginGpuIndex", preferences.vstPreferredPluginGpuIndex);
        preferences.vstPreferredPluginGpuId = getString(values, "vstPreferredPluginGpuId", preferences.vstPreferredPluginGpuId);
        const int loadedMaxOpenVstPluginWindows = getInt(values, "vstMaxOpenPluginWindows", preferences.vstMaxOpenPluginWindows);
        preferences.vstMaxOpenPluginWindows = loadedMaxOpenVstPluginWindows <= 0
            ? preferences.vstMaxOpenPluginWindows
            : std::clamp(loadedMaxOpenVstPluginWindows, 1, 12);
        preferences.vstGraphicsProfileSummary = getString(values, "vstGraphicsProfileSummary", preferences.vstGraphicsProfileSummary);
        preferences.vstExperimentalWarningAcknowledged = getInt(values, "vstExperimentalWarningAcknowledged", preferences.vstExperimentalWarningAcknowledged ? 1 : 0) != 0;

        return preferences;
    }

    bool UserPreferencesStore::saveValues(const std::vector<std::pair<std::string, std::string>>& updates)
    {
        if (updates.empty())
            return true;

        std::error_code ignored;
        std::filesystem::create_directories(AppPaths::settingsFolder(), ignored);

        const auto path = AppPaths::preferencesFilePath();
        std::vector<std::string> lines;

        {
            std::ifstream input(path);
            std::string line;
            while (std::getline(input, line))
            {
                const auto equals = line.find('=');
                const auto key = equals == std::string::npos ? std::string() : trim(line.substr(0, equals));

                if (key == "lastBaseFileName"
                    || key == "vstPreferredPluginGpuId"
                    || key == "vstGraphicsProfileAdapters")
                    continue;

                lines.push_back(line);
            }
        }

        std::set<std::string> writtenKeys;

        for (auto& line : lines)
        {
            const auto equals = line.find('=');
            if (equals == std::string::npos)
                continue;

            const auto key = trim(line.substr(0, equals));
            for (const auto& [updateKey, updateValue] : updates)
            {
                if (key == updateKey)
                {
                    line = updateKey + "=" + updateValue;
                    writtenKeys.insert(updateKey);
                    break;
                }
            }
        }

        for (const auto& [key, value] : updates)
        {
            if (key == "lastBaseFileName"
                || key == "vstPreferredPluginGpuId"
                || key == "vstGraphicsProfileAdapters")
                continue;

            if (!key.empty() && writtenKeys.find(key) == writtenKeys.end())
                lines.push_back(key + "=" + value);
        }

        const std::filesystem::path tempPath(path.string() + ".tmp");
        {
            std::ofstream output(tempPath, std::ios::trunc);
            if (!output)
                return false;

            for (const auto& line : lines)
                output << line << "\n";

            if (!output.good())
                return false;
        }

        std::error_code copyError;
        std::filesystem::copy_file(tempPath, path, std::filesystem::copy_options::overwrite_existing, copyError);
        std::filesystem::remove(tempPath, ignored);
        return !copyError;
    }

    bool UserPreferencesStore::saveValue(const std::string& key, const std::string& value)
    {
        return saveValues({ { key, value } });
    }

    bool UserPreferencesStore::saveBoolValue(const std::string& key, bool value)
    {
        return saveValue(key, value ? "1" : "0");
    }

    bool UserPreferencesStore::saveIntValue(const std::string& key, int value)
    {
        return saveValue(key, std::to_string(value));
    }

    bool UserPreferencesStore::save(const UserPreferences& preferences)
    {
        std::error_code ignored;
        std::filesystem::create_directories(AppPaths::settingsFolder(), ignored);

        std::ofstream file(AppPaths::preferencesFilePath());

        if (!file)
            return false;

        file << "lastInputFolder=" << preferences.lastInputFolder.string() << "\n";
        file << "lastExportFolder=" << preferences.lastExportFolder.string() << "\n";
        file << "lastSoundFontPath=" << preferences.lastSoundFontPath.string() << "\n";
        file << "lastSfzPath=" << preferences.lastSfzPath.string() << "\n";
        file << "lastVst3Folder=" << preferences.lastVst3Folder.string() << "\n";
        file << "lastOutputFormatId=" << preferences.lastOutputFormatId << "\n";
        file << "lastAudioClipFormatId=" << preferences.lastAudioClipFormatId << "\n";
        file << "lastAudioClipQualityKbps=" << preferences.lastAudioClipQualityKbps << "\n";
        file << "lastBackendId=" << preferences.lastBackendId << "\n";
        file << "lastSampleRate=" << preferences.lastSampleRate << "\n";
        file << "lastBitrateKbps=" << preferences.lastBitrateKbps << "\n";
        file << "lastChannelCount=" << preferences.lastChannelCount << "\n";
        file << "lastRenderWorkerCount=" << preferences.lastRenderWorkerCount << "\n";
        file << "keepStemFiles=" << formatStemFileMask(preferences.keepStemFilesMask) << "\n";
        file << "suppressSfzRenderWarning=" << (preferences.suppressSfzRenderWarning ? 1 : 0) << "\n";
        file << "themePresetId=" << preferences.themePresetId << "\n";
        file << "helperBubblesEnabled=" << (preferences.helperBubblesEnabled ? 1 : 0) << "\n";
        file << "vstCompatibilityWarningsEnabled=" << (preferences.vstCompatibilityWarningsEnabled ? 1 : 0) << "\n";
        file << "vstSafePluginUiMode=" << (preferences.vstSafePluginUiMode ? 1 : 0) << "\n";
        file << "vstWarningStyleId=" << preferences.vstWarningStyleId << "\n";
        file << "vstGraphicsProfileDetected=" << (preferences.vstGraphicsProfileDetected ? 1 : 0) << "\n";
        file << "vstGraphicsProfileSource=" << preferences.vstGraphicsProfileSource << "\n";
        file << "vstGraphicsProfileLastDetected=" << preferences.vstGraphicsProfileLastDetected << "\n";
        file << "vstPreferredPluginGpuIndex=" << std::max(1, preferences.vstPreferredPluginGpuIndex) << "\n";
        file << "vstMaxOpenPluginWindows=" << std::clamp(preferences.vstMaxOpenPluginWindows, 1, 12) << "\n";
        file << "vstGraphicsProfileSummary=" << preferences.vstGraphicsProfileSummary << "\n";
        file << "vstExperimentalWarningAcknowledged=" << (preferences.vstExperimentalWarningAcknowledged ? 1 : 0) << "\n";

        return file.good();
    }
}
