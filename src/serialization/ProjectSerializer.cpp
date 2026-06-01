#include "serialization/ProjectSerializer.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace
{
    std::string readTextFile(const std::filesystem::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file) return {};
        std::ostringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
    }

    std::string escapeJsonString(const std::string& input)
    {
        std::string output;

        for (char c : input)
        {
            if (c == '\\') output += "\\\\";
            else if (c == '"') output += "\\\"";
            else if (c == '\n') output += "\\n";
            else if (c == '\r') output += "\\r";
            else if (c == '\t') output += "\\t";
            else output += c;
        }

        return output;
    }

    std::string unescapeJsonString(const std::string& input)
    {
        std::string output;

        for (std::size_t i = 0; i < input.size(); ++i)
        {
            if (input[i] == '\\' && i + 1 < input.size())
            {
                const char n = input[++i];

                if (n == 'n') output += '\n';
                else if (n == 'r') output += '\r';
                else if (n == 't') output += '\t';
                else output += n;
            }
            else
            {
                output += input[i];
            }
        }

        return output;
    }

    std::size_t findMatching(const std::string& text, std::size_t openPos, char openChar, char closeChar)
    {
        int depth = 0;
        bool inString = false;
        bool escaping = false;

        for (std::size_t i = openPos; i < text.size(); ++i)
        {
            const char c = text[i];

            if (escaping)
            {
                escaping = false;
                continue;
            }

            if (inString && c == '\\')
            {
                escaping = true;
                continue;
            }

            if (c == '"')
            {
                inString = !inString;
                continue;
            }

            if (inString)
                continue;

            if (c == openChar)
                ++depth;
            else if (c == closeChar)
            {
                --depth;
                if (depth == 0)
                    return i;
            }
        }

        return std::string::npos;
    }

    std::string extractObject(const std::string& text, const std::string& key)
    {
        const auto pattern = "\"" + key + "\":";
        auto pos = text.find(pattern);
        if (pos == std::string::npos) return {};

        pos = text.find('{', pos);
        if (pos == std::string::npos) return {};

        const auto end = findMatching(text, pos, '{', '}');
        if (end == std::string::npos) return {};

        return text.substr(pos, end - pos + 1);
    }

    std::string extractArray(const std::string& text, const std::string& key)
    {
        const auto pattern = "\"" + key + "\":";
        auto pos = text.find(pattern);
        if (pos == std::string::npos) return {};

        pos = text.find('[', pos);
        if (pos == std::string::npos) return {};

        const auto end = findMatching(text, pos, '[', ']');
        if (end == std::string::npos) return {};

        return text.substr(pos, end - pos + 1);
    }

    std::string extractLastArray(const std::string& text, const std::string& key)
    {
        const auto pattern = "\"" + key + "\":";
        auto pos = text.rfind(pattern);

        if (pos == std::string::npos)
            return {};

        pos = text.find('[', pos);

        if (pos == std::string::npos)
            return {};

        const auto end = findMatching(text, pos, '[', ']');

        if (end == std::string::npos)
            return {};

        return text.substr(pos, end - pos + 1);
    }

    std::vector<std::string> splitTopLevelObjects(const std::string& arrayText)
    {
        std::vector<std::string> objects;

        std::size_t pos = 0;
        while ((pos = arrayText.find('{', pos)) != std::string::npos)
        {
            const auto end = findMatching(arrayText, pos, '{', '}');
            if (end == std::string::npos) break;

            objects.push_back(arrayText.substr(pos, end - pos + 1));
            pos = end + 1;
        }

        return objects;
    }

    std::string getString(const std::string& text, const std::string& key, const std::string& fallback = {})
    {
        const auto pattern = "\"" + key + "\":";
        auto pos = text.find(pattern);
        if (pos == std::string::npos) return fallback;

        pos = text.find('"', pos + pattern.size());
        if (pos == std::string::npos) return fallback;

        std::string value;
        bool escaping = false;

        for (std::size_t i = pos + 1; i < text.size(); ++i)
        {
            const char c = text[i];

            if (escaping)
            {
                value += '\\';
                value += c;
                escaping = false;
                continue;
            }

            if (c == '\\')
            {
                escaping = true;
                continue;
            }

            if (c == '"')
                return unescapeJsonString(value);

            value += c;
        }

        return fallback;
    }

    int getInt(const std::string& text, const std::string& key, int fallback = 0)
    {
        const auto pattern = "\"" + key + "\":";
        auto pos = text.find(pattern);
        if (pos == std::string::npos) return fallback;

        pos += pattern.size();

        while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos])))
            ++pos;

        try
        {
            return std::stoi(text.substr(pos));
        }
        catch (...)
        {
            return fallback;
        }
    }

    long long getLongLong(const std::string& text, const std::string& key, long long fallback = 0)
    {
        const auto pattern = "\"" + key + "\":";
        auto pos = text.find(pattern);
        if (pos == std::string::npos) return fallback;

        pos += pattern.size();

        while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos])))
            ++pos;

        try
        {
            return std::stoll(text.substr(pos));
        }
        catch (...)
        {
            return fallback;
        }
    }

    float getFloat(const std::string& text, const std::string& key, float fallback = 0.0f)
    {
        const auto pattern = "\"" + key + "\":";
        auto pos = text.find(pattern);
        if (pos == std::string::npos) return fallback;

        pos += pattern.size();

        while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos])))
            ++pos;

        try
        {
            return std::stof(text.substr(pos));
        }
        catch (...)
        {
            return fallback;
        }
    }

    std::vector<int> getIntArray(const std::string& text, const std::string& key)
    {
        std::vector<int> values;
        const auto array = extractArray(text, key);

        if (array.empty())
            return values;

        std::size_t pos = 0;

        while (pos < array.size())
        {
            while (pos < array.size()
                && !std::isdigit(static_cast<unsigned char>(array[pos]))
                && array[pos] != '-')
                ++pos;

            if (pos >= array.size())
                break;

            try
            {
                values.push_back(std::stoi(array.substr(pos)));
            }
            catch (...)
            {
            }

            while (pos < array.size()
                && (std::isdigit(static_cast<unsigned char>(array[pos])) || array[pos] == '-'))
                ++pos;
        }

        return values;
    }


    std::vector<std::string> getStringArray(const std::string& text, const std::string& key)
    {
        std::vector<std::string> values;
        const auto array = extractArray(text, key);

        if (array.empty())
            return values;

        std::size_t pos = 0;
        while ((pos = array.find('"', pos)) != std::string::npos)
        {
            std::string value;
            bool escaping = false;
            bool foundEnd = false;

            for (std::size_t i = pos + 1; i < array.size(); ++i)
            {
                const char c = array[i];

                if (escaping)
                {
                    value += '\\';
                    value += c;
                    escaping = false;
                    continue;
                }

                if (c == '\\')
                {
                    escaping = true;
                    continue;
                }

                if (c == '"')
                {
                    values.push_back(unescapeJsonString(value));
                    pos = i + 1;
                    foundEnd = true;
                    break;
                }

                value += c;
            }

            if (!foundEnd)
                break;
        }

        return values;
    }

    bool getBool(const std::string& text, const std::string& key, bool fallback = false)
    {
        const auto pattern = "\"" + key + "\":";
        auto pos = text.find(pattern);
        if (pos == std::string::npos) return fallback;

        pos += pattern.size();

        while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos])))
            ++pos;

        if (text.compare(pos, 4, "true") == 0) return true;
        if (text.compare(pos, 5, "false") == 0) return false;
        return fallback;
    }

    mw::core::VstPluginAssignment parseVstPluginAssignmentObject(const std::string& objectText)
    {
        mw::core::VstPluginAssignment plugin;
        plugin.bundlePath = getString(objectText, "bundlePath");
        plugin.name = getString(objectText, "name");
        plugin.vendor = getString(objectText, "vendor");
        plugin.version = getString(objectText, "version");
        plugin.category = getString(objectText, "category");
        plugin.uid = getString(objectText, "uid");
        plugin.stateBase64 = getString(objectText, "stateBase64");
        plugin.bypassed = getBool(objectText, "bypassed", false);
        plugin.compatibilityWarningSeen = getBool(objectText, "compatibilityWarningSeen", false);
        plugin.compatibilitySummary = getString(objectText, "compatibilitySummary");
        return plugin;
    }

    void writeVstPluginAssignmentObject(std::ofstream& file, const mw::core::VstPluginAssignment& plugin, const std::string& indent)
    {
        file << indent << "{\n";
        file << indent << "  \"bundlePath\": \"" << escapeJsonString(plugin.bundlePath.string()) << "\",\n";
        file << indent << "  \"name\": \"" << escapeJsonString(plugin.name) << "\",\n";
        file << indent << "  \"vendor\": \"" << escapeJsonString(plugin.vendor) << "\",\n";
        file << indent << "  \"version\": \"" << escapeJsonString(plugin.version) << "\",\n";
        file << indent << "  \"category\": \"" << escapeJsonString(plugin.category) << "\",\n";
        file << indent << "  \"uid\": \"" << escapeJsonString(plugin.uid) << "\",\n";
        file << indent << "  \"stateBase64\": \"" << escapeJsonString(plugin.stateBase64) << "\",\n";
        file << indent << "  \"bypassed\": " << (plugin.bypassed ? "true" : "false") << ",\n";
        file << indent << "  \"compatibilityWarningSeen\": " << (plugin.compatibilityWarningSeen ? "true" : "false") << ",\n";
        file << indent << "  \"compatibilitySummary\": \"" << escapeJsonString(plugin.compatibilitySummary) << "\"\n";
        file << indent << "}";
    }

    mw::core::SampleBackendType backendFromString(const std::string& value)
    {
        if (value == "SF2") return mw::core::SampleBackendType::SF2;
        if (value == "SFZ") return mw::core::SampleBackendType::SFZ;
        if (value == "WAV") return mw::core::SampleBackendType::WAV;
        if (value == "VST3") return mw::core::SampleBackendType::VST3;
        return mw::core::SampleBackendType::None;
    }

    void writeStringField(std::ofstream& file, const std::string& key, const std::string& value, bool comma = true)
    {
        file << "    \"" << key << "\": \"" << escapeJsonString(value) << "\"" << (comma ? "," : "") << "\n";
    }
}

namespace mw::serialization
{
    bool ProjectSerializer::saveToFile(const mw::core::Project& p, const std::filesystem::path& fp)
    {
        std::ofstream file(fp, std::ios::binary);
        if (!file) return false;

        const auto& settings = p.getUserSettings();

        file << "{\n";
        file << "  \"format\": \"" << p.getMetadata().projectFormatName << "\",\n";
        file << "  \"version\": " << p.getMetadata().projectFormatVersion << ",\n";
        file << "  \"name\": \"" << escapeJsonString(p.getName()) << "\",\n";
        file << "  \"tempoBpm\": " << p.getTempoBpm() << ",\n";
        file << "  \"ticksPerQuarterNote\": " << mw::core::Project::ticksPerQuarterNote << ",\n";

        file << "  \"projectSettings\": {\n";
        writeStringField(file, "sourceInputPath", settings.sourceInputPath.string());
        writeStringField(file, "exportFolder", settings.exportFolder.string());
        writeStringField(file, "soundFontPath", settings.soundFontPath.string());
        writeStringField(file, "projectDefaultSoundFontPath", settings.soundFontPath.string());
        writeStringField(file, "projectDefaultSoundFontName", settings.soundFontPath.filename().string());
        writeStringField(file, "fluidSynthPath", settings.fluidSynthPath.string());
        writeStringField(file, "ffmpegPath", settings.ffmpegPath.string());
        writeStringField(file, "sfzPath", settings.sfzPath.string());
        writeStringField(file, "projectDefaultSFZPath", settings.sfzPath.string());
        writeStringField(file, "projectDefaultSFZName", settings.sfzPath.filename().string());
        writeStringField(file, "sfizzRenderPath", settings.sfizzRenderPath.string());
        writeStringField(file, "projectDefaultVST3Path", settings.vst3PluginPath.string());
        writeStringField(file, "projectDefaultVST3Name", settings.vst3PluginName);
        writeStringField(file, "projectDefaultVST3Vendor", settings.vst3PluginVendor);
        writeStringField(file, "projectDefaultVST3Version", settings.vst3PluginVersion);
        writeStringField(file, "projectDefaultVST3Category", settings.vst3PluginCategory);
        writeStringField(file, "projectDefaultVST3Uid", settings.vst3PluginUid);
        writeStringField(file, "projectDefaultVST3Compatibility", settings.vst3PluginCompatibilitySummary);
        writeStringField(file, "baseFileName", settings.baseFileName);
        writeStringField(file, "metadataTitle", settings.metadataTitle);
        writeStringField(file, "metadataArtist", settings.metadataArtist);
        writeStringField(file, "metadataAlbum", settings.metadataAlbum);
        writeStringField(file, "metadataTrackNumber", settings.metadataTrackNumber);
        writeStringField(file, "metadataYear", settings.metadataYear);
        file << "    \"backendId\": " << settings.backendId << ",\n";
        file << "    \"projectDefaultBackendId\": " << settings.backendId << ",\n";
        file << "    \"outputFormatId\": " << settings.outputFormatId << ",\n";
        file << "    \"audioClipFormatId\": " << settings.audioClipFormatId << ",\n";
        file << "    \"audioClipQualityKbps\": " << settings.audioClipQualityKbps << ",\n";
        file << "    \"sampleRate\": " << settings.sampleRate << ",\n";
        file << "    \"bitrateKbps\": " << settings.bitrateKbps << ",\n";
        file << "    \"channelCount\": " << settings.channelCount << ",\n";
        file << "    \"renderWorkerCount\": " << settings.renderWorkerCount << ",\n";
        file << "    \"sfzKeySwitch\": " << settings.sfzKeySwitch << ",\n";
        file << "    \"sfzCc1\": " << settings.sfzCc1 << ",\n";
        file << "    \"sfzCc11\": " << settings.sfzCc11 << ",\n";
        file << "    \"masterVolume\": " << settings.masterVolume << "\n";
        file << "  },\n";

        file << "  \"timeSignature\": {\n";
        file << "    \"numerator\": " << p.getTimeSignature().numerator << ",\n";
        file << "    \"denominator\": " << p.getTimeSignature().denominator << "\n";
        file << "  },\n";

        file << "  \"sequences\": [\n";
        const auto& sequences = p.getSequences();

        for (std::size_t i = 0; i < sequences.size(); ++i)
        {
            const auto& sequence = sequences[i];

            file << "    {\n";
            file << "      \"id\": " << sequence.id << ",\n";
            file << "      \"number\": " << sequence.number << ",\n";
            file << "      \"type\": \"" << escapeJsonString(sequence.type) << "\",\n";
            file << "      \"name\": \"" << escapeJsonString(sequence.name) << "\",\n";
            file << "      \"sourceFile\": \"" << escapeJsonString(sequence.sourceFile.string()) << "\",\n";
            file << "      \"createdBy\": \"" << escapeJsonString(sequence.createdBy) << "\",\n";
            file << "      \"notes\": \"" << escapeJsonString(sequence.notes) << "\",\n";
            file << "      \"locked\": " << (sequence.locked ? "true" : "false") << ",\n";
            file << "      \"startTick\": " << sequence.startTick << ",\n";
            file << "      \"endTick\": " << sequence.endTick << ",\n";
            file << "      \"tracks\": [";

            for (std::size_t t = 0; t < sequence.tracks.size(); ++t)
                file << sequence.tracks[t] << (t + 1 < sequence.tracks.size() ? ", " : "");

            file << "],\n";
            file << "      \"color\": \"" << escapeJsonString(sequence.color) << "\"\n";
            file << "    }" << (i + 1 < sequences.size() ? "," : "") << "\n";
        }

        file << "  ],\n";

        file << "  \"audioClips\": [\n";
        const auto& audioClips = p.getAudioClips();

        for (std::size_t i = 0; i < audioClips.size(); ++i)
        {
            const auto& clip = audioClips[i];

            file << "    {\n";
            file << "      \"id\": " << clip.id << ",\n";
            file << "      \"name\": \"" << escapeJsonString(clip.name) << "\",\n";
            file << "      \"trackIndex\": " << clip.trackIndex << ",\n";
            file << "      \"sequenceNumber\": " << clip.sequenceNumber << ",\n";
            file << "      \"sourceType\": \"" << escapeJsonString(mw::core::audioClipSourceTypeToString(clip.sourceType)) << "\",\n";
            file << "      \"savedFormat\": \"" << escapeJsonString(mw::core::audioClipSavedFormatToString(clip.savedFormat)) << "\",\n";
            file << "      \"projectRelativePath\": \"" << escapeJsonString(clip.projectRelativePath.string()) << "\",\n";
            file << "      \"originalSourcePath\": \"" << escapeJsonString(clip.originalSourcePath.string()) << "\",\n";
            file << "      \"startTick\": " << clip.startTick << ",\n";
            file << "      \"durationSamples\": " << clip.durationSamples << ",\n";
            file << "      \"sampleRate\": " << clip.sampleRate << ",\n";
            file << "      \"channelCount\": " << clip.channelCount << ",\n";
            file << "      \"bitDepth\": " << clip.bitDepth << ",\n";
            file << "      \"gain\": " << clip.gain << ",\n";
            file << "      \"pan\": " << clip.pan << ",\n";
            file << "      \"sizeBytes\": " << static_cast<unsigned long long>(clip.sizeBytes) << ",\n";
            file << "      \"notes\": \"" << escapeJsonString(clip.notes) << "\",\n";
            file << "      \"missingMedia\": " << (clip.missingMedia ? "true" : "false") << "\n";
            file << "    }" << (i + 1 < audioClips.size() ? "," : "") << "\n";
        }

        file << "  ],\n";

        file << "  \"tracks\": [\n";
        const auto& tracks = p.getTracks();

        for (std::size_t i = 0; i < tracks.size(); ++i)
        {
            const auto& t = tracks[i];
            const auto& instrument = t.getInstrument();
            const auto& mixer = t.getMixerSettings();
            auto serializedVst3 = instrument.vst3;
            if (instrument.backendType != mw::core::SampleBackendType::VST3)
                serializedVst3 = {};
            file << "    {\n";
            file << "      \"name\": \"" << escapeJsonString(t.getName()) << "\",\n";
            file << "      \"trackType\": \"" << escapeJsonString(mw::core::trackTypeToString(t.getTrackType())) << "\",\n";
            file << "      \"muted\": " << (t.getMuted() ? "true" : "false") << ",\n";
            file << "      \"solo\": " << (t.getSolo() ? "true" : "false") << ",\n";
            file << "      \"mixer\": {\n";
            file << "        \"volume\": " << mixer.volume << ",\n";
            file << "        \"pan\": " << mixer.pan << ",\n";
            file << "        \"reverbSend\": " << mixer.reverbSend << ",\n";
            file << "        \"humanizeAmount\": " << mixer.humanizeAmount << "\n";
            file << "      },\n";
            file << "      \"instrument\": {\n";
            file << "        \"displayName\": \"" << escapeJsonString(instrument.displayName) << "\",\n";
            file << "        \"originalImportedName\": \"" << escapeJsonString(instrument.originalImportedName) << "\",\n";
            file << "        \"normalizedName\": \"" << escapeJsonString(instrument.normalizedName) << "\",\n";
            file << "        \"backendType\": \"" << mw::core::sampleBackendTypeToString(instrument.backendType) << "\",\n";
            file << "        \"sampleLibraryPath\": \"" << escapeJsonString(instrument.sampleLibraryPath.string()) << "\",\n";
            file << "        \"sampleLibraryDisplayName\": \"" << escapeJsonString(instrument.sampleLibraryDisplayName.empty() ? instrument.sampleLibraryPath.filename().string() : instrument.sampleLibraryDisplayName) << "\",\n";
            file << "        \"midiChannel\": " << instrument.midiChannel << ",\n";
            file << "        \"midiBank\": " << instrument.midiBank << ",\n";
            file << "        \"midiProgram\": " << instrument.midiProgram << ",\n";
            file << "        \"presetName\": \"" << escapeJsonString(instrument.presetName) << "\",\n";
            file << "        \"articulationMap\": \"" << escapeJsonString(instrument.articulationMap) << "\",\n";
            file << "        \"wasAutoMatched\": " << (instrument.wasAutoMatched ? "true" : "false") << ",\n";
            file << "        \"matchConfidence\": " << instrument.matchConfidence << ",\n";
            file << "        \"vst3\": {\n";
            file << "          \"bundlePath\": \"" << escapeJsonString(serializedVst3.bundlePath.string()) << "\",\n";
            file << "          \"name\": \"" << escapeJsonString(serializedVst3.name) << "\",\n";
            file << "          \"vendor\": \"" << escapeJsonString(serializedVst3.vendor) << "\",\n";
            file << "          \"version\": \"" << escapeJsonString(serializedVst3.version) << "\",\n";
            file << "          \"category\": \"" << escapeJsonString(serializedVst3.category) << "\",\n";
            file << "          \"uid\": \"" << escapeJsonString(serializedVst3.uid) << "\",\n";
            file << "          \"stateBase64\": \"" << escapeJsonString(serializedVst3.stateBase64) << "\",\n";
            file << "          \"bypassed\": " << (serializedVst3.bypassed ? "true" : "false") << ",\n";
            file << "          \"compatibilityWarningSeen\": " << (serializedVst3.compatibilityWarningSeen ? "true" : "false") << ",\n";
            file << "          \"compatibilitySummary\": \"" << escapeJsonString(serializedVst3.compatibilitySummary) << "\"\n";
            file << "        }\n";
            file << "      },\n";

            const auto& vstEffects = t.getVstEffects();
            bool anyVstEffectSlotEnabled = false;
            for (const auto& effectSlot : vstEffects.slots)
            {
                if (effectSlot.enabled && effectSlot.plugin.hasPluginIdentity())
                {
                    anyVstEffectSlotEnabled = true;
                    break;
                }
            }
            file << "      \"vstEffects\": {\n";
            file << "        \"enabled\": " << (anyVstEffectSlotEnabled ? "true" : "false") << ",\n";
            file << "        \"slots\": [\n";
            for (std::size_t effectSlotIndex = 0; effectSlotIndex < vstEffects.slots.size(); ++effectSlotIndex)
            {
                const auto& effectSlot = vstEffects.slots[effectSlotIndex];
                file << "          {\n";
                file << "            \"enabled\": " << (effectSlot.enabled ? "true" : "false") << ",\n";
                file << "            \"plugin\": ";
                writeVstPluginAssignmentObject(file, effectSlot.plugin, "            ");
                file << "\n";
                file << "          }" << (effectSlotIndex + 1 < vstEffects.slots.size() ? "," : "") << "\n";
            }
            file << "        ]\n";
            file << "      },\n";

            file << "      \"notes\": [\n";

            const auto& ns = t.getNotes();

            for (std::size_t n = 0; n < ns.size(); ++n)
            {
                const auto& x = ns[n];

                file << "        { \"pitch\": " << x.pitch
                     << ", \"velocity\": " << x.velocity
                     << ", \"startTick\": " << x.startTick
                     << ", \"durationTicks\": " << x.durationTicks
                     << ", \"midiChannel\": " << x.midiChannel
                     << ", \"articulation\": \"" << mw::core::articulationToString(x.articulation) << "\" }"
                     << (n + 1 < ns.size() ? "," : "") << "\n";
            }

            file << "      ]\n";
            file << "    }" << (i + 1 < tracks.size() ? "," : "") << "\n";
        }

        file << "  ]\n";
        file << "}\n";

        return true;
    }

    std::optional<mw::core::Project> ProjectSerializer::loadFromFile(const std::filesystem::path& filePath)
    {
        const auto text = readTextFile(filePath);

        if (text.empty())
            return std::nullopt;

        mw::core::Project project(getString(text, "name", filePath.stem().string()));
        project.setTempoBpm(getInt(text, "tempoBpm", 120));

        const auto timeSignatureObject = extractObject(text, "timeSignature");
        if (!timeSignatureObject.empty())
        {
            project.setTimeSignature(
                {
                    getInt(timeSignatureObject, "numerator", 4),
                    getInt(timeSignatureObject, "denominator", 4)
                }
            );
        }

        const auto settingsObject = extractObject(text, "projectSettings");
        if (!settingsObject.empty())
        {
            auto settings = project.getUserSettings();
            settings.sourceInputPath = getString(settingsObject, "sourceInputPath");
            settings.exportFolder = getString(settingsObject, "exportFolder");
            settings.soundFontPath = getString(settingsObject, "soundFontPath", getString(settingsObject, "projectDefaultSoundFontPath"));
            settings.fluidSynthPath = getString(settingsObject, "fluidSynthPath");
            settings.ffmpegPath = getString(settingsObject, "ffmpegPath");
            settings.sfzPath = getString(settingsObject, "sfzPath", getString(settingsObject, "projectDefaultSFZPath"));
            settings.sfizzRenderPath = getString(settingsObject, "sfizzRenderPath");
            settings.vst3PluginPath = getString(settingsObject, "projectDefaultVST3Path");
            settings.vst3PluginName = getString(settingsObject, "projectDefaultVST3Name");
            settings.vst3PluginVendor = getString(settingsObject, "projectDefaultVST3Vendor");
            settings.vst3PluginVersion = getString(settingsObject, "projectDefaultVST3Version");
            settings.vst3PluginCategory = getString(settingsObject, "projectDefaultVST3Category");
            settings.vst3PluginUid = getString(settingsObject, "projectDefaultVST3Uid");
            settings.vst3PluginCompatibilitySummary = getString(settingsObject, "projectDefaultVST3Compatibility");
            settings.baseFileName = getString(settingsObject, "baseFileName", project.getName());
            settings.metadataTitle = getString(settingsObject, "metadataTitle");
            settings.metadataArtist = getString(settingsObject, "metadataArtist");
            settings.metadataAlbum = getString(settingsObject, "metadataAlbum");
            settings.metadataTrackNumber = getString(settingsObject, "metadataTrackNumber");
            settings.metadataYear = getString(settingsObject, "metadataYear");
            settings.backendId = getInt(settingsObject, "backendId", getInt(settingsObject, "projectDefaultBackendId", 1));
            settings.outputFormatId = getInt(settingsObject, "outputFormatId", 1);
            settings.audioClipFormatId = getInt(settingsObject, "audioClipFormatId", 1);
            settings.audioClipQualityKbps = getInt(settingsObject, "audioClipQualityKbps", 320);
            settings.sampleRate = getInt(settingsObject, "sampleRate", 48000);
            settings.bitrateKbps = getInt(settingsObject, "bitrateKbps", 192);
            settings.channelCount = getInt(settingsObject, "channelCount", 2);
            settings.renderWorkerCount = getInt(settingsObject, "renderWorkerCount", 0);
            settings.sfzKeySwitch = getInt(settingsObject, "sfzKeySwitch", 24);
            settings.sfzCc1 = getInt(settingsObject, "sfzCc1", 100);
            settings.sfzCc11 = getInt(settingsObject, "sfzCc11", 127);
            settings.masterVolume = getFloat(settingsObject, "masterVolume", 1.0f);
            project.setUserSettings(settings);
        }

        const auto sequencesArray = extractArray(text, "sequences");
        const auto sequenceObjects = splitTopLevelObjects(sequencesArray);

        if (!sequenceObjects.empty())
        {
            std::vector<mw::core::ProjectSequenceMetadata> sequences;

            for (const auto& sequenceObject : sequenceObjects)
            {
                mw::core::ProjectSequenceMetadata sequence;
                sequence.id = getInt(sequenceObject, "id", getInt(sequenceObject, "number", static_cast<int>(sequences.size()) + 1));
                sequence.number = getInt(sequenceObject, "number", static_cast<int>(sequences.size()) + 1);
                sequence.type = getString(sequenceObject, "type", "sequence");
                sequence.name = getString(sequenceObject, "name", "Sequence");
                sequence.sourceFile = getString(sequenceObject, "sourceFile");
                sequence.createdBy = getString(sequenceObject, "createdBy", "manual");
                sequence.notes = getString(sequenceObject, "notes");
                sequence.locked = getBool(sequenceObject, "locked", false);
                sequence.startTick = getLongLong(sequenceObject, "startTick", 0);
                sequence.endTick = getLongLong(sequenceObject, "endTick", sequence.startTick);
                sequence.tracks = getIntArray(sequenceObject, "tracks");
                sequence.color = getString(sequenceObject, "color");

                std::sort(sequence.tracks.begin(), sequence.tracks.end());
                sequence.tracks.erase(std::unique(sequence.tracks.begin(), sequence.tracks.end()), sequence.tracks.end());

                sequences.push_back(std::move(sequence));
            }

            project.setSequences(std::move(sequences));
        }

        const auto audioClipsArray = extractArray(text, "audioClips");
        const auto audioClipObjects = splitTopLevelObjects(audioClipsArray);

        if (!audioClipObjects.empty())
        {
            std::vector<mw::core::AudioClip> clips;

            for (const auto& clipObject : audioClipObjects)
            {
                mw::core::AudioClip clip;
                clip.id = getInt(clipObject, "id", static_cast<int>(clips.size()) + 1);
                clip.name = getString(clipObject, "name", "AudioClip");
                clip.trackIndex = getInt(clipObject, "trackIndex", 0);
                clip.sequenceNumber = getInt(clipObject, "sequenceNumber", 1);
                clip.sourceType = mw::core::audioClipSourceTypeFromString(getString(clipObject, "sourceType", "imported"));
                clip.savedFormat = mw::core::audioClipSavedFormatFromString(getString(clipObject, "savedFormat", "wav"));
                clip.projectRelativePath = getString(clipObject, "projectRelativePath");
                clip.originalSourcePath = getString(clipObject, "originalSourcePath");
                clip.startTick = getLongLong(clipObject, "startTick", 0);
                clip.durationSamples = getLongLong(clipObject, "durationSamples", 0);
                clip.sampleRate = getFloat(clipObject, "sampleRate", 48000.0f);
                clip.channelCount = getInt(clipObject, "channelCount", 2);
                clip.bitDepth = getInt(clipObject, "bitDepth", 24);
                clip.gain = getFloat(clipObject, "gain", 1.0f);
                clip.pan = getFloat(clipObject, "pan", 0.0f);
                clip.sizeBytes = static_cast<std::uintmax_t>(std::max<long long>(0, getLongLong(clipObject, "sizeBytes", 0)));
                clip.notes = getString(clipObject, "notes");
                clip.missingMedia = getBool(clipObject, "missingMedia", false);
                clips.push_back(std::move(clip));
            }

            project.setAudioClips(std::move(clips));
        }

        const auto tracksArray = extractLastArray(text, "tracks");
        const auto trackObjects = splitTopLevelObjects(tracksArray);

        for (const auto& trackObject : trackObjects)
        {
            auto& track = project.addTrack(getString(trackObject, "name", "Loaded Track"));
            track.setTrackType(mw::core::trackTypeFromString(getString(trackObject, "trackType", "MIDI")));

            track.setMuted(getBool(trackObject, "muted", false));
            track.setSolo(getBool(trackObject, "solo", false));

            const auto mixerObject = extractObject(trackObject, "mixer");
            if (!mixerObject.empty())
            {
                auto& mixer = track.getMixerSettings();
                mixer.volume = getFloat(mixerObject, "volume", 1.0f);
                mixer.pan = getFloat(mixerObject, "pan", 0.0f);
                mixer.reverbSend = getFloat(mixerObject, "reverbSend", 0.0f);
                mixer.humanizeAmount = getFloat(mixerObject, "humanizeAmount", 0.0f);
            }

            const auto instrumentObject = extractObject(trackObject, "instrument");
            if (!instrumentObject.empty())
            {
                auto instrument = track.getInstrument();

                instrument.displayName = getString(instrumentObject, "displayName", track.getName());
                instrument.originalImportedName = getString(instrumentObject, "originalImportedName", track.getName());
                instrument.normalizedName = getString(instrumentObject, "normalizedName", instrument.displayName);
                instrument.backendType = backendFromString(getString(instrumentObject, "backendType", "None"));
                instrument.sampleLibraryPath = getString(instrumentObject, "sampleLibraryPath");
                instrument.sampleLibraryDisplayName = getString(instrumentObject, "sampleLibraryDisplayName", instrument.sampleLibraryPath.filename().string());
                instrument.midiChannel = getInt(instrumentObject, "midiChannel", 1);
                instrument.midiBank = getInt(instrumentObject, "midiBank", 0);
                instrument.midiProgram = getInt(instrumentObject, "midiProgram", 0);
                instrument.presetName = getString(instrumentObject, "presetName", instrument.displayName);
                instrument.articulationMap = getString(instrumentObject, "articulationMap", "Default");
                instrument.wasAutoMatched = getBool(instrumentObject, "wasAutoMatched", false);
                instrument.matchConfidence = getFloat(instrumentObject, "matchConfidence", 0.0f);

                const auto vstObject = extractObject(instrumentObject, "vst3");
                if (!vstObject.empty())
                    instrument.vst3 = parseVstPluginAssignmentObject(vstObject);

                if (instrument.backendType != mw::core::SampleBackendType::VST3)
                    instrument.vst3 = {};

                track.setInstrumentAssignment(instrument);
            }

            const auto vstEffectsObject = extractObject(trackObject, "vstEffects");
            if (!vstEffectsObject.empty())
            {
                mw::core::VstEffectsAssignment vstEffects;
                vstEffects.enabled = getBool(vstEffectsObject, "enabled", false);

                const auto effectSlotsArray = extractArray(vstEffectsObject, "slots");
                const auto effectSlotObjects = splitTopLevelObjects(effectSlotsArray);
                for (const auto& effectSlotObject : effectSlotObjects)
                {
                    if (vstEffects.slots.size() >= mw::core::maxVstEffectSlots)
                        break;

                    mw::core::VstEffectSlotAssignment slot;
                    const bool legacyEnabled = vstEffects.enabled && vstEffects.slots.empty();
                    slot.enabled = getBool(effectSlotObject, "enabled", legacyEnabled);
                    const auto effectPluginObject = extractObject(effectSlotObject, "plugin");
                    if (!effectPluginObject.empty())
                        slot.plugin = parseVstPluginAssignmentObject(effectPluginObject);
                    vstEffects.slots.push_back(std::move(slot));
                }

                if (vstEffects.enabled && vstEffects.slots.empty())
                    vstEffects.ensureFirstSlot().enabled = true;

                vstEffects.updateLegacyEnabledMirror();
                track.setVstEffects(std::move(vstEffects));
            }

            const auto notesArray = extractArray(trackObject, "notes");
            const auto noteObjects = splitTopLevelObjects(notesArray);

            for (const auto& noteObject : noteObjects)
            {
                track.addNote(
                    mw::core::NoteEvent(
                        getInt(noteObject, "pitch", 60),
                        getInt(noteObject, "velocity", 100),
                        getLongLong(noteObject, "startTick", 0),
                        getLongLong(noteObject, "durationTicks", 960),
                        getInt(noteObject, "midiChannel", track.getInstrument().midiChannel),
                        mw::core::articulationFromString(getString(noteObject, "articulation", "Normal"))
                    )
                );
            }
        }

        return project;
    }
}
