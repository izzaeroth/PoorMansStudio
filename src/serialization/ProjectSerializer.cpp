#include "serialization/ProjectSerializer.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <unordered_set>
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

    std::string extractArrayStrict(const std::string& text, const std::string& key)
    {
        const auto pattern = "\"" + key + "\":";
        auto pos = text.find(pattern);
        if (pos == std::string::npos)
            return {};
        pos += pattern.size();
        while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos])))
            ++pos;
        if (pos >= text.size() || text[pos] != '[')
            return {};
        const auto end = findMatching(text, pos, '[', ']');
        if (end == std::string::npos)
            return {};
        return text.substr(pos, end - pos + 1);
    }

    std::string getStringStrict(const std::string& text, const std::string& key, const std::string& fallback = {})
    {
        const auto pattern = "\"" + key + "\":";
        auto pos = text.find(pattern);
        if (pos == std::string::npos)
            return fallback;
        pos += pattern.size();
        while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos])))
            ++pos;
        if (pos >= text.size() || text[pos] != '"')
            return fallback;

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

    std::vector<std::string> parseStringArray(const std::string& arrayText)
    {
        std::vector<std::string> values;
        std::unordered_set<std::string> seen;
        bool inString = false;
        bool escaping = false;
        std::string current;
        for (char c : arrayText)
        {
            if (!inString)
            {
                if (c == '"')
                {
                    inString = true;
                    escaping = false;
                    current.clear();
                }
                continue;
            }

            if (escaping)
            {
                current += '\\';
                current += c;
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
                auto value = unescapeJsonString(current);
                auto first = value.begin();
                while (first != value.end() && std::isspace(static_cast<unsigned char>(*first)))
                    ++first;
                auto last = value.end();
                while (last != first && std::isspace(static_cast<unsigned char>(*(last - 1))))
                    --last;
                value = std::string(first, last);
                if (!value.empty())
                {
                    auto key = value;
                    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char ch)
                    {
                        return static_cast<char>(std::tolower(ch));
                    });
                    if (seen.insert(key).second)
                        values.push_back(std::move(value));
                }
                inString = false;
                continue;
            }
            current += c;
        }
        return values;
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
        if (value == "CLAP") return mw::core::SampleBackendType::CLAP;
        return mw::core::SampleBackendType::None;
    }

    mw::core::EffectSlotBackendType effectSlotBackendFromString(const std::string& value)
    {
        if (value == "VST3" || value == "vst3") return mw::core::EffectSlotBackendType::VST3;
        if (value == "CLAP" || value == "clap") return mw::core::EffectSlotBackendType::CLAP;
        return mw::core::EffectSlotBackendType::None;
    }

    void writeStringField(std::ofstream& file, const std::string& key, const std::string& value, bool comma = true)
    {
        file << "    \"" << key << "\": \"" << escapeJsonString(value) << "\"" << (comma ? "," : "") << "\n";
    }

    const char* persistentReviewModeToString(mw::core::PersistentMidiTakeReviewMode mode) noexcept
    {
        return mode == mw::core::PersistentMidiTakeReviewMode::Comp ? "Comp" : "WholePass";
    }

    mw::core::PersistentMidiTakeReviewMode persistentReviewModeFromString(const std::string& value) noexcept
    {
        return value == "Comp"
            ? mw::core::PersistentMidiTakeReviewMode::Comp
            : mw::core::PersistentMidiTakeReviewMode::WholePass;
    }

    const char* persistentCompGridToString(mw::core::PersistentMidiCompGrid grid) noexcept
    {
        switch (grid)
        {
            case mw::core::PersistentMidiCompGrid::Bar: return "Bar";
            case mw::core::PersistentMidiCompGrid::Custom: return "Custom";
            case mw::core::PersistentMidiCompGrid::Beat:
            default: return "Beat";
        }
    }

    mw::core::PersistentMidiCompGrid persistentCompGridFromString(const std::string& value) noexcept
    {
        if (value == "Bar") return mw::core::PersistentMidiCompGrid::Bar;
        if (value == "Custom") return mw::core::PersistentMidiCompGrid::Custom;
        return mw::core::PersistentMidiCompGrid::Beat;
    }

    const char* persistentCompBoundaryNoteModeToString(
        mw::core::PersistentMidiCompBoundaryNoteMode mode) noexcept
    {
        return mode == mw::core::PersistentMidiCompBoundaryNoteMode::PreserveSamePass
            ? "PreserveSamePass"
            : "Cut";
    }

    mw::core::PersistentMidiCompBoundaryNoteMode persistentCompBoundaryNoteModeFromString(
        const std::string& value) noexcept
    {
        return value == "PreserveSamePass"
            ? mw::core::PersistentMidiCompBoundaryNoteMode::PreserveSamePass
            : mw::core::PersistentMidiCompBoundaryNoteMode::Cut;
    }

    const char* persistentSustainPedalModeToString(
        mw::core::PersistentMidiSustainPedalMode mode) noexcept
    {
        return mode == mw::core::PersistentMidiSustainPedalMode::ApplyCc64
            ? "ApplyCc64"
            : "Ignore";
    }

    mw::core::PersistentMidiSustainPedalMode persistentSustainPedalModeFromString(
        const std::string& value) noexcept
    {
        return value == "ApplyCc64"
            ? mw::core::PersistentMidiSustainPedalMode::ApplyCc64
            : mw::core::PersistentMidiSustainPedalMode::Ignore;
    }

    const char* persistentCompCrossPassNoteModeToString(
        mw::core::PersistentMidiCompCrossPassNoteMode mode) noexcept
    {
        return mode == mw::core::PersistentMidiCompCrossPassNoteMode::BridgeExact
            ? "BridgeExact"
            : "Off";
    }

    mw::core::PersistentMidiCompCrossPassNoteMode persistentCompCrossPassNoteModeFromString(
        const std::string& value) noexcept
    {
        return value == "BridgeExact"
            ? mw::core::PersistentMidiCompCrossPassNoteMode::BridgeExact
            : mw::core::PersistentMidiCompCrossPassNoteMode::Off;
    }

    void writeNoteObject(std::ofstream& file, const mw::core::NoteEvent& note)
    {
        file << "{ \"pitch\": " << note.pitch
             << ", \"velocity\": " << note.velocity
             << ", \"startTick\": " << note.startTick
             << ", \"durationTicks\": " << note.durationTicks
             << ", \"midiChannel\": " << note.midiChannel
             << ", \"articulation\": \"" << mw::core::articulationToString(note.articulation) << "\" }";
    }

    void writePersistentMidiTakeReviewObject(std::ofstream& file,
                                                const mw::core::PersistentMidiTakeReview& pendingReview,
                                                const std::string& indent)
    {
        file << indent << "{\n";
        file << indent << "  \"active\": true,\n";
        file << indent << "  \"reviewSessionId\": " << pendingReview.reviewSessionId << ",\n";
        file << indent << "  \"reviewName\": \"" << escapeJsonString(pendingReview.reviewName) << "\",\n";
        file << indent << "  \"tags\": [";
        for (std::size_t tagIndex = 0; tagIndex < pendingReview.reviewTags.size(); ++tagIndex)
        {
            if (tagIndex > 0) file << ", ";
            file << "\"" << escapeJsonString(pendingReview.reviewTags[tagIndex]) << "\"";
        }
        file << "],\n";
        file << indent << "  \"reviewNotes\": \"" << escapeJsonString(pendingReview.reviewNotes) << "\",\n";
        file << indent << "  \"regionInTick\": " << pendingReview.regionInTick << ",\n";
        file << indent << "  \"regionOutTick\": " << pendingReview.regionOutTick << ",\n";
        file << indent << "  \"selectedPassNumber\": " << pendingReview.selectedPassNumber << ",\n";
        file << indent << "  \"reviewMode\": \"" << persistentReviewModeToString(pendingReview.reviewMode) << "\",\n";
        file << indent << "  \"compGrid\": \"" << persistentCompGridToString(pendingReview.compGrid) << "\",\n";
        file << indent << "  \"boundaryNoteMode\": \""
             << persistentCompBoundaryNoteModeToString(pendingReview.boundaryNoteMode) << "\",\n";
        file << indent << "  \"sustainPedalMode\": \""
             << persistentSustainPedalModeToString(pendingReview.sustainPedalMode) << "\",\n";
        file << indent << "  \"crossPassNoteMode\": \""
             << persistentCompCrossPassNoteModeToString(pendingReview.crossPassNoteMode) << "\",\n";
        file << indent << "  \"passes\": [\n";
        for (std::size_t passIndex = 0; passIndex < pendingReview.passes.size(); ++passIndex)
        {
            const auto& pass = pendingReview.passes[passIndex];
            file << indent << "    {\n";
            file << indent << "      \"passNumber\": " << pass.passNumber << ",\n";
            file << indent << "      \"sustainDownMaskAtStart\": " << pass.sustainDownMaskAtStart << ",\n";
            file << indent << "      \"cc64Events\": [\n";
            for (std::size_t ccIndex = 0; ccIndex < pass.cc64Events.size(); ++ccIndex)
            {
                const auto& event = pass.cc64Events[ccIndex];
                file << indent << "        { \"tick\": " << event.tick
                     << ", \"midiChannel\": " << event.midiChannel
                     << ", \"value\": " << event.value
                     << ", \"order\": " << event.order << " }"
                     << (ccIndex + 1 < pass.cc64Events.size() ? "," : "") << "\n";
            }
            file << indent << "      ],\n";
            file << indent << "      \"controllerEvents\": [\n";
            for (std::size_t controllerIndex = 0; controllerIndex < pass.controllerEvents.size(); ++controllerIndex)
            {
                const auto& event = pass.controllerEvents[controllerIndex];
                file << indent << "        { \"tick\": " << event.tick
                     << ", \"midiChannel\": " << event.midiChannel
                     << ", \"controller\": " << event.controller
                     << ", \"value\": " << event.value
                     << ", \"order\": " << event.order << " }"
                     << (controllerIndex + 1 < pass.controllerEvents.size() ? "," : "") << "\n";
            }
            file << indent << "      ],\n";
            file << indent << "      \"pitchBendEvents\": [\n";
            for (std::size_t bendIndex = 0; bendIndex < pass.pitchBendEvents.size(); ++bendIndex)
            {
                const auto& event = pass.pitchBendEvents[bendIndex];
                file << indent << "        { \"tick\": " << event.tick
                     << ", \"midiChannel\": " << event.midiChannel
                     << ", \"value\": " << event.value
                     << ", \"order\": " << event.order << " }"
                     << (bendIndex + 1 < pass.pitchBendEvents.size() ? "," : "") << "\n";
            }
            file << indent << "      ],\n";
            file << indent << "      \"programChangeEvents\": [\n";
            for (std::size_t programIndex = 0; programIndex < pass.programChangeEvents.size(); ++programIndex)
            {
                const auto& event = pass.programChangeEvents[programIndex];
                file << indent << "        { \"tick\": " << event.tick
                     << ", \"midiChannel\": " << event.midiChannel
                     << ", \"program\": " << event.program
                     << ", \"order\": " << event.order << " }"
                     << (programIndex + 1 < pass.programChangeEvents.size() ? "," : "") << "\n";
            }
            file << indent << "      ],\n";
            file << indent << "      \"channelPressureEvents\": [\n";
            for (std::size_t pressureIndex = 0; pressureIndex < pass.channelPressureEvents.size(); ++pressureIndex)
            {
                const auto& event = pass.channelPressureEvents[pressureIndex];
                file << indent << "        { \"tick\": " << event.tick
                     << ", \"midiChannel\": " << event.midiChannel
                     << ", \"pressure\": " << event.pressure
                     << ", \"order\": " << event.order << " }"
                     << (pressureIndex + 1 < pass.channelPressureEvents.size() ? "," : "") << "\n";
            }
            file << indent << "      ],\n";
            file << indent << "      \"polyPressureEvents\": [\n";
            for (std::size_t polyIndex = 0; polyIndex < pass.polyPressureEvents.size(); ++polyIndex)
            {
                const auto& event = pass.polyPressureEvents[polyIndex];
                file << indent << "        { \"tick\": " << event.tick
                     << ", \"midiChannel\": " << event.midiChannel
                     << ", \"noteNumber\": " << event.noteNumber
                     << ", \"pressure\": " << event.pressure
                     << ", \"order\": " << event.order << " }"
                     << (polyIndex + 1 < pass.polyPressureEvents.size() ? "," : "") << "\n";
            }
            file << indent << "      ],\n";
            file << indent << "      \"controller14BitEvents\": [\n";
            for (std::size_t eventIndex = 0; eventIndex < pass.controller14BitEvents.size(); ++eventIndex)
            {
                const auto& event = pass.controller14BitEvents[eventIndex];
                file << indent << "        { \"tick\": " << event.tick
                     << ", \"midiChannel\": " << event.midiChannel
                     << ", \"controllerMsb\": " << event.controllerMsb
                     << ", \"value\": " << event.value
                     << ", \"order\": " << event.order << " }"
                     << (eventIndex + 1 < pass.controller14BitEvents.size() ? "," : "") << "\n";
            }
            file << indent << "      ],\n";
            file << indent << "      \"rpnEvents\": [\n";
            for (std::size_t eventIndex = 0; eventIndex < pass.rpnEvents.size(); ++eventIndex)
            {
                const auto& event = pass.rpnEvents[eventIndex];
                file << indent << "        { \"tick\": " << event.tick
                     << ", \"midiChannel\": " << event.midiChannel
                     << ", \"parameter\": " << event.parameter
                     << ", \"value\": " << event.value
                     << ", \"order\": " << event.order << " }"
                     << (eventIndex + 1 < pass.rpnEvents.size() ? "," : "") << "\n";
            }
            file << indent << "      ],\n";
            file << indent << "      \"nrpnEvents\": [\n";
            for (std::size_t eventIndex = 0; eventIndex < pass.nrpnEvents.size(); ++eventIndex)
            {
                const auto& event = pass.nrpnEvents[eventIndex];
                file << indent << "        { \"tick\": " << event.tick
                     << ", \"midiChannel\": " << event.midiChannel
                     << ", \"parameter\": " << event.parameter
                     << ", \"value\": " << event.value
                     << ", \"order\": " << event.order << " }"
                     << (eventIndex + 1 < pass.nrpnEvents.size() ? "," : "") << "\n";
            }
            file << indent << "      ],\n";
            file << indent << "      \"takeNotes\": [\n";
            for (std::size_t noteIndex = 0; noteIndex < pass.notes.size(); ++noteIndex)
            {
                file << indent << "        ";
                writeNoteObject(file, pass.notes[noteIndex]);
                file << (noteIndex + 1 < pass.notes.size() ? "," : "") << "\n";
            }
            file << indent << "      ]\n";
            file << indent << "    }" << (passIndex + 1 < pendingReview.passes.size() ? "," : "") << "\n";
        }
        file << indent << "  ],\n";
        file << indent << "  \"segments\": [\n";
        for (std::size_t segmentIndex = 0; segmentIndex < pendingReview.segments.size(); ++segmentIndex)
        {
            const auto& segment = pendingReview.segments[segmentIndex];
            file << indent << "    { \"startTick\": " << segment.startTick
                 << ", \"endTick\": " << segment.endTick
                 << ", \"selectedPassNumber\": " << segment.selectedPassNumber << " }"
                 << (segmentIndex + 1 < pendingReview.segments.size() ? "," : "") << "\n";
        }
        file << indent << "  ]\n";
        file << indent << "}";
    }

    bool containsPersistentPass(const std::vector<mw::core::PersistentMidiTakePass>& passes, int passNumber) noexcept
    {
        return std::any_of(passes.begin(), passes.end(), [passNumber](const auto& pass)
        {
            return pass.passNumber == passNumber;
        });
    }

    std::optional<mw::core::PersistentMidiTakeReview> parsePersistentMidiTakeReviewObject(
        const std::string& objectText, int fallbackMidiChannel, int sourceFormatVersion)
    {
        if (objectText.empty() || !getBool(objectText, "active", false))
            return std::nullopt;

        mw::core::PersistentMidiTakeReview review;
        review.active = true;
        review.reviewSessionId = static_cast<mw::core::StableId>(std::max<long long>(0, getLongLong(objectText, "reviewSessionId", 0)));
        review.reviewName = getString(objectText, "reviewName");
        review.reviewTags = parseStringArray(extractArrayStrict(objectText, "tags"));
        review.reviewNotes = getStringStrict(objectText, "reviewNotes");
        review.regionInTick = getLongLong(objectText, "regionInTick", -1);
        review.regionOutTick = getLongLong(objectText, "regionOutTick", -1);
        review.selectedPassNumber = getInt(objectText, "selectedPassNumber", 0);
        review.reviewMode = persistentReviewModeFromString(getString(objectText, "reviewMode", "WholePass"));
        review.compGrid = persistentCompGridFromString(getString(objectText, "compGrid", "Beat"));
        review.boundaryNoteMode = persistentCompBoundaryNoteModeFromString(
            getString(objectText, "boundaryNoteMode", "Cut"));
        review.sustainPedalMode = persistentSustainPedalModeFromString(
            getString(objectText, "sustainPedalMode", "Ignore"));
        review.crossPassNoteMode = persistentCompCrossPassNoteModeFromString(
            getString(objectText, "crossPassNoteMode", "Off"));

        if (review.regionInTick < 0 || review.regionOutTick <= review.regionInTick)
            return std::nullopt;

        const auto passesArray = extractArray(objectText, "passes");
        const auto passObjects = splitTopLevelObjects(passesArray);
        int previousPassNumber = 0;
        std::size_t totalNotes = 0;
        std::size_t totalControllers = 0;
        for (const auto& passObject : passObjects)
        {
            mw::core::PersistentMidiTakePass pass;
            pass.passNumber = getInt(passObject, "passNumber", 0);
            if (pass.passNumber <= previousPassNumber)
                return std::nullopt;
            previousPassNumber = pass.passNumber;
            const auto sustainMaskRaw = getLongLong(passObject, "sustainDownMaskAtStart", 0);
            pass.sustainDownMaskAtStart = static_cast<std::uint16_t>(
                std::clamp<long long>(sustainMaskRaw, 0, 65535));

            const auto cc64Array = extractArray(passObject, "cc64Events");
            std::uint64_t fallbackOrder = 1;
            for (const auto& ccObject : splitTopLevelObjects(cc64Array))
            {
                mw::core::PersistentMidiCc64Event event;
                event.tick = getLongLong(ccObject, "tick", -1);
                event.midiChannel = getInt(ccObject, "midiChannel", fallbackMidiChannel);
                event.value = getInt(ccObject, "value", -1);
                const auto parsedOrder = getLongLong(ccObject, "order", static_cast<long long>(fallbackOrder));
                event.order = parsedOrder > 0 ? static_cast<std::uint64_t>(parsedOrder) : fallbackOrder;
                ++fallbackOrder;

                // Malformed controller metadata must not invalidate otherwise usable notes.
                // Ignore only the bad CC64 event and preserve the retained pass itself.
                if (event.tick < review.regionInTick || event.tick > review.regionOutTick
                    || event.midiChannel < 1 || event.midiChannel > 16
                    || event.value < 0 || event.value > 127)
                {
                    continue;
                }
                pass.cc64Events.push_back(event);
            }
            std::stable_sort(pass.cc64Events.begin(), pass.cc64Events.end(), [](const auto& lhs, const auto& rhs)
            {
                if (lhs.tick != rhs.tick)
                    return lhs.tick < rhs.tick;
                return lhs.order < rhs.order;
            });

            if (sourceFormatVersion >= 17)
            {
                const auto controllerArray = extractArray(passObject, "controllerEvents");
                std::uint64_t controllerFallbackOrder = 1;
                for (const auto& controllerObject : splitTopLevelObjects(controllerArray))
                {
                    mw::core::PersistentMidiControllerEvent event;
                    event.tick = getLongLong(controllerObject, "tick", -1);
                    event.midiChannel = getInt(controllerObject, "midiChannel", fallbackMidiChannel);
                    event.controller = getInt(controllerObject, "controller", -1);
                    event.value = getInt(controllerObject, "value", -1);
                    const auto parsedOrder = getLongLong(
                        controllerObject, "order", static_cast<long long>(controllerFallbackOrder));
                    event.order = parsedOrder > 0
                        ? static_cast<std::uint64_t>(parsedOrder)
                        : controllerFallbackOrder;
                    ++controllerFallbackOrder;
                    if (event.tick < review.regionInTick || event.tick >= review.regionOutTick
                        || event.midiChannel < 1 || event.midiChannel > 16
                        || event.controller < 0 || event.controller > 127
                        || event.value < 0 || event.value > 127)
                    {
                        continue;
                    }
                    pass.controllerEvents.push_back(event);
                }

                std::stable_sort(pass.controllerEvents.begin(), pass.controllerEvents.end(), [](const auto& lhs, const auto& rhs)
                {
                    if (lhs.tick != rhs.tick)
                        return lhs.tick < rhs.tick;
                    if (lhs.midiChannel != rhs.midiChannel)
                        return lhs.midiChannel < rhs.midiChannel;
                    if (lhs.controller != rhs.controller)
                        return lhs.controller < rhs.controller;
                    return lhs.order < rhs.order;
                });

                std::vector<mw::core::PersistentMidiControllerEvent> collapsedControllers;
                for (const auto& event : pass.controllerEvents)
                {
                    if (!collapsedControllers.empty())
                    {
                        auto& previous = collapsedControllers.back();
                        if (previous.tick == event.tick
                            && previous.midiChannel == event.midiChannel
                            && previous.controller == event.controller)
                        {
                            previous = event;
                            continue;
                        }
                    }
                    collapsedControllers.push_back(event);
                }
                pass.controllerEvents = std::move(collapsedControllers);
            }

            if (sourceFormatVersion >= 18)
            {
                const auto bendArray = extractArray(passObject, "pitchBendEvents");
                std::uint64_t bendFallbackOrder = 1;
                for (const auto& bendObject : splitTopLevelObjects(bendArray))
                {
                    mw::core::PersistentMidiPitchBendEvent event;
                    event.tick = getLongLong(bendObject, "tick", -1);
                    event.midiChannel = getInt(bendObject, "midiChannel", fallbackMidiChannel);
                    event.value = getInt(bendObject, "value", 99999);
                    const auto parsedOrder = getLongLong(bendObject, "order", static_cast<long long>(bendFallbackOrder));
                    event.order = parsedOrder > 0 ? static_cast<std::uint64_t>(parsedOrder) : bendFallbackOrder;
                    ++bendFallbackOrder;
                    if (event.tick < review.regionInTick || event.tick >= review.regionOutTick
                        || event.midiChannel < 1 || event.midiChannel > 16
                        || event.value < mw::core::midiPitchBendMinimum || event.value > mw::core::midiPitchBendMaximum)
                        continue;
                    pass.pitchBendEvents.push_back(event);
                }
                std::stable_sort(pass.pitchBendEvents.begin(), pass.pitchBendEvents.end(), [](const auto& a, const auto& b)
                {
                    if (a.tick != b.tick) return a.tick < b.tick;
                    if (a.midiChannel != b.midiChannel) return a.midiChannel < b.midiChannel;
                    return a.order < b.order;
                });
                std::vector<mw::core::PersistentMidiPitchBendEvent> collapsedBends;
                for (const auto& event : pass.pitchBendEvents)
                {
                    if (!collapsedBends.empty() && collapsedBends.back().tick == event.tick
                        && collapsedBends.back().midiChannel == event.midiChannel)
                    {
                        collapsedBends.back() = event;
                        continue;
                    }
                    collapsedBends.push_back(event);
                }
                pass.pitchBendEvents = std::move(collapsedBends);
            }

            if (sourceFormatVersion >= 19)
            {
                const auto programArray = extractArray(passObject, "programChangeEvents");
                std::uint64_t programFallbackOrder = 1;
                for (const auto& programObject : splitTopLevelObjects(programArray))
                {
                    mw::core::PersistentMidiProgramChangeEvent event;
                    event.tick = getLongLong(programObject, "tick", -1);
                    event.midiChannel = getInt(programObject, "midiChannel", fallbackMidiChannel);
                    event.program = getInt(programObject, "program", -1);
                    const auto parsedOrder = getLongLong(programObject, "order", static_cast<long long>(programFallbackOrder));
                    event.order = parsedOrder > 0 ? static_cast<std::uint64_t>(parsedOrder) : programFallbackOrder;
                    ++programFallbackOrder;
                    if (event.tick < review.regionInTick || event.tick >= review.regionOutTick
                        || event.midiChannel < 1 || event.midiChannel > 16
                        || event.program < 0 || event.program > 127)
                        continue;
                    pass.programChangeEvents.push_back(event);
                }
                std::stable_sort(pass.programChangeEvents.begin(), pass.programChangeEvents.end(), [](const auto& a, const auto& b)
                {
                    if (a.tick != b.tick) return a.tick < b.tick;
                    if (a.midiChannel != b.midiChannel) return a.midiChannel < b.midiChannel;
                    return a.order < b.order;
                });
                std::vector<mw::core::PersistentMidiProgramChangeEvent> collapsedPrograms;
                for (const auto& event : pass.programChangeEvents)
                {
                    if (!collapsedPrograms.empty() && collapsedPrograms.back().tick == event.tick
                        && collapsedPrograms.back().midiChannel == event.midiChannel)
                        collapsedPrograms.back() = event;
                    else
                        collapsedPrograms.push_back(event);
                }
                pass.programChangeEvents = std::move(collapsedPrograms);
            }

            if (sourceFormatVersion >= 20)
            {
                const auto pressureArray = extractArray(passObject, "channelPressureEvents");
                std::uint64_t pressureFallbackOrder = 1;
                for (const auto& pressureObject : splitTopLevelObjects(pressureArray))
                {
                    mw::core::PersistentMidiChannelPressureEvent event;
                    event.tick = getLongLong(pressureObject, "tick", -1);
                    event.midiChannel = getInt(pressureObject, "midiChannel", fallbackMidiChannel);
                    event.pressure = getInt(pressureObject, "pressure", -1);
                    const auto parsedOrder = getLongLong(pressureObject, "order", static_cast<long long>(pressureFallbackOrder));
                    event.order = parsedOrder > 0 ? static_cast<std::uint64_t>(parsedOrder) : pressureFallbackOrder;
                    ++pressureFallbackOrder;
                    if (event.tick < review.regionInTick || event.tick >= review.regionOutTick
                        || event.midiChannel < 1 || event.midiChannel > 16
                        || event.pressure < mw::core::midiChannelPressureMinimum || event.pressure > mw::core::midiChannelPressureMaximum)
                        continue;
                    pass.channelPressureEvents.push_back(event);
                }
                std::stable_sort(pass.channelPressureEvents.begin(), pass.channelPressureEvents.end(), [](const auto& a, const auto& b)
                {
                    if (a.tick != b.tick) return a.tick < b.tick;
                    if (a.midiChannel != b.midiChannel) return a.midiChannel < b.midiChannel;
                    return a.order < b.order;
                });
                std::vector<mw::core::PersistentMidiChannelPressureEvent> collapsedPressure;
                for (const auto& event : pass.channelPressureEvents)
                {
                    if (!collapsedPressure.empty() && collapsedPressure.back().tick == event.tick
                        && collapsedPressure.back().midiChannel == event.midiChannel)
                        collapsedPressure.back() = event;
                    else
                        collapsedPressure.push_back(event);
                }
                pass.channelPressureEvents = std::move(collapsedPressure);
            }

            if (sourceFormatVersion >= 21)
            {
                const auto polyArray = extractArray(passObject, "polyPressureEvents");
                std::uint64_t polyFallbackOrder = 1;
                for (const auto& polyObject : splitTopLevelObjects(polyArray))
                {
                    mw::core::PersistentMidiPolyPressureEvent event;
                    event.tick = getLongLong(polyObject, "tick", -1);
                    event.midiChannel = getInt(polyObject, "midiChannel", fallbackMidiChannel);
                    event.noteNumber = getInt(polyObject, "noteNumber", -1);
                    event.pressure = getInt(polyObject, "pressure", -1);
                    const auto parsedOrder = getLongLong(polyObject, "order", static_cast<long long>(polyFallbackOrder));
                    event.order = parsedOrder > 0 ? static_cast<std::uint64_t>(parsedOrder) : polyFallbackOrder;
                    ++polyFallbackOrder;
                    if (event.tick < review.regionInTick || event.tick >= review.regionOutTick
                        || event.midiChannel < 1 || event.midiChannel > 16
                        || event.noteNumber < 0 || event.noteNumber > 127
                        || event.pressure < mw::core::midiPolyPressureMinimum || event.pressure > mw::core::midiPolyPressureMaximum)
                        continue;
                    pass.polyPressureEvents.push_back(event);
                }
                std::stable_sort(pass.polyPressureEvents.begin(), pass.polyPressureEvents.end(), [](const auto& a, const auto& b)
                {
                    if (a.tick != b.tick) return a.tick < b.tick;
                    if (a.midiChannel != b.midiChannel) return a.midiChannel < b.midiChannel;
                    if (a.noteNumber != b.noteNumber) return a.noteNumber < b.noteNumber;
                    return a.order < b.order;
                });
                std::vector<mw::core::PersistentMidiPolyPressureEvent> collapsedPoly;
                for (const auto& event : pass.polyPressureEvents)
                {
                    if (!collapsedPoly.empty() && collapsedPoly.back().tick == event.tick
                        && collapsedPoly.back().midiChannel == event.midiChannel
                        && collapsedPoly.back().noteNumber == event.noteNumber)
                        collapsedPoly.back() = event;
                    else
                        collapsedPoly.push_back(event);
                }
                pass.polyPressureEvents = std::move(collapsedPoly);
            }

            if (sourceFormatVersion >= 22)
            {
                const auto array14 = extractArray(passObject, "controller14BitEvents");
                std::uint64_t fallback14 = 1;
                for (const auto& object : splitTopLevelObjects(array14))
                {
                    mw::core::PersistentMidi14BitControllerEvent event;
                    event.tick = getLongLong(object, "tick", -1);
                    event.midiChannel = getInt(object, "midiChannel", fallbackMidiChannel);
                    event.controllerMsb = getInt(object, "controllerMsb", -1);
                    event.value = getInt(object, "value", -1);
                    const auto parsed = getLongLong(object, "order", static_cast<long long>(fallback14));
                    event.order = parsed > 0 ? static_cast<std::uint64_t>(parsed) : fallback14; ++fallback14;
                    if (event.tick < review.regionInTick || event.tick >= review.regionOutTick
                        || event.midiChannel < 1 || event.midiChannel > 16
                        || event.controllerMsb < mw::core::midi14BitControllerMinimum || event.controllerMsb > mw::core::midi14BitControllerMaximum
                        || event.value < mw::core::midi14BitMinimum || event.value > mw::core::midi14BitMaximum) continue;
                    pass.controller14BitEvents.push_back(event);
                }
                std::stable_sort(pass.controller14BitEvents.begin(), pass.controller14BitEvents.end(), [](const auto& a, const auto& b)
                { if (a.tick != b.tick) return a.tick < b.tick; if (a.midiChannel != b.midiChannel) return a.midiChannel < b.midiChannel; if (a.controllerMsb != b.controllerMsb) return a.controllerMsb < b.controllerMsb; return a.order < b.order; });
                std::vector<mw::core::PersistentMidi14BitControllerEvent> collapsed14;
                for (const auto& event : pass.controller14BitEvents)
                {
                    if (!collapsed14.empty() && collapsed14.back().tick == event.tick && collapsed14.back().midiChannel == event.midiChannel && collapsed14.back().controllerMsb == event.controllerMsb) collapsed14.back() = event;
                    else collapsed14.push_back(event);
                }
                pass.controller14BitEvents = std::move(collapsed14);

                const auto rpnArray = extractArray(passObject, "rpnEvents");
                std::uint64_t fallbackRpn = 1;
                for (const auto& object : splitTopLevelObjects(rpnArray))
                {
                    mw::core::PersistentMidiRpnEvent event;
                    event.tick = getLongLong(object, "tick", -1); event.midiChannel = getInt(object, "midiChannel", fallbackMidiChannel);
                    event.parameter = getInt(object, "parameter", -1); event.value = getInt(object, "value", -1);
                    const auto parsed = getLongLong(object, "order", static_cast<long long>(fallbackRpn)); event.order = parsed > 0 ? static_cast<std::uint64_t>(parsed) : fallbackRpn; ++fallbackRpn;
                    if (event.tick < review.regionInTick || event.tick >= review.regionOutTick || event.midiChannel < 1 || event.midiChannel > 16
                        || event.parameter < mw::core::midiParameterMinimum || event.parameter >= mw::core::midiRpnNullParameter
                        || event.value < mw::core::midiParameterMinimum || event.value > mw::core::midiParameterMaximum) continue;
                    pass.rpnEvents.push_back(event);
                }
                std::stable_sort(pass.rpnEvents.begin(), pass.rpnEvents.end(), [](const auto& a, const auto& b)
                { if (a.tick != b.tick) return a.tick < b.tick; if (a.midiChannel != b.midiChannel) return a.midiChannel < b.midiChannel; if (a.parameter != b.parameter) return a.parameter < b.parameter; return a.order < b.order; });
                std::vector<mw::core::PersistentMidiRpnEvent> collapsedRpn;
                for (const auto& event : pass.rpnEvents) { if (!collapsedRpn.empty() && collapsedRpn.back().tick == event.tick && collapsedRpn.back().midiChannel == event.midiChannel && collapsedRpn.back().parameter == event.parameter) collapsedRpn.back() = event; else collapsedRpn.push_back(event); }
                pass.rpnEvents = std::move(collapsedRpn);

                const auto nrpnArray = extractArray(passObject, "nrpnEvents");
                std::uint64_t fallbackNrpn = 1;
                for (const auto& object : splitTopLevelObjects(nrpnArray))
                {
                    mw::core::PersistentMidiNrpnEvent event;
                    event.tick = getLongLong(object, "tick", -1); event.midiChannel = getInt(object, "midiChannel", fallbackMidiChannel);
                    event.parameter = getInt(object, "parameter", -1); event.value = getInt(object, "value", -1);
                    const auto parsed = getLongLong(object, "order", static_cast<long long>(fallbackNrpn)); event.order = parsed > 0 ? static_cast<std::uint64_t>(parsed) : fallbackNrpn; ++fallbackNrpn;
                    if (event.tick < review.regionInTick || event.tick >= review.regionOutTick || event.midiChannel < 1 || event.midiChannel > 16
                        || event.parameter < mw::core::midiParameterMinimum || event.parameter > mw::core::midiParameterMaximum
                        || event.value < mw::core::midiParameterMinimum || event.value > mw::core::midiParameterMaximum) continue;
                    pass.nrpnEvents.push_back(event);
                }
                std::stable_sort(pass.nrpnEvents.begin(), pass.nrpnEvents.end(), [](const auto& a, const auto& b)
                { if (a.tick != b.tick) return a.tick < b.tick; if (a.midiChannel != b.midiChannel) return a.midiChannel < b.midiChannel; if (a.parameter != b.parameter) return a.parameter < b.parameter; return a.order < b.order; });
                std::vector<mw::core::PersistentMidiNrpnEvent> collapsedNrpn;
                for (const auto& event : pass.nrpnEvents) { if (!collapsedNrpn.empty() && collapsedNrpn.back().tick == event.tick && collapsedNrpn.back().midiChannel == event.midiChannel && collapsedNrpn.back().parameter == event.parameter) collapsedNrpn.back() = event; else collapsedNrpn.push_back(event); }
                pass.nrpnEvents = std::move(collapsedNrpn);
            }

            const auto notesArray = extractArray(passObject, "takeNotes");
            for (const auto& noteObject : splitTopLevelObjects(notesArray))
            {
                mw::core::NoteEvent note(
                    getInt(noteObject, "pitch", 60),
                    getInt(noteObject, "velocity", 100),
                    getLongLong(noteObject, "startTick", 0),
                    getLongLong(noteObject, "durationTicks", 0),
                    getInt(noteObject, "midiChannel", fallbackMidiChannel),
                    mw::core::articulationFromString(getString(noteObject, "articulation", "Normal")));

                if (note.pitch < 0 || note.pitch > 127
                    || note.velocity < 0 || note.velocity > 127
                    || note.midiChannel < 1 || note.midiChannel > 16
                    || note.durationTicks <= 0
                    || note.startTick < review.regionInTick
                    || note.startTick >= review.regionOutTick)
                {
                    return std::nullopt;
                }

                if (note.startTick + note.durationTicks > review.regionOutTick)
                    return std::nullopt;

                pass.notes.push_back(note);
                ++totalNotes;
            }
            totalControllers += pass.controllerEvents.size() + pass.pitchBendEvents.size() + pass.programChangeEvents.size()
                + pass.channelPressureEvents.size() + pass.polyPressureEvents.size() + pass.controller14BitEvents.size()
                + pass.rpnEvents.size() + pass.nrpnEvents.size();
            review.passes.push_back(std::move(pass));
        }

        if (review.passes.empty() || (totalNotes == 0 && totalControllers == 0))
            return std::nullopt;
        if (!containsPersistentPass(review.passes, review.selectedPassNumber))
            review.selectedPassNumber = review.passes.front().passNumber;

        const auto segmentsArray = extractArray(objectText, "segments");
        for (const auto& segmentObject : splitTopLevelObjects(segmentsArray))
        {
            mw::core::PersistentMidiCompSegment segment;
            segment.startTick = getLongLong(segmentObject, "startTick", -1);
            segment.endTick = getLongLong(segmentObject, "endTick", -1);
            segment.selectedPassNumber = getInt(segmentObject, "selectedPassNumber", 0);
            if (segment.startTick < review.regionInTick
                || segment.endTick <= segment.startTick
                || segment.endTick > review.regionOutTick
                || !containsPersistentPass(review.passes, segment.selectedPassNumber))
            {
                return std::nullopt;
            }
            if (!review.segments.empty() && review.segments.back().endTick != segment.startTick)
                return std::nullopt;
            review.segments.push_back(segment);
        }

        if (!review.segments.empty())
        {
            if (review.segments.front().startTick != review.regionInTick
                || review.segments.back().endTick != review.regionOutTick)
            {
                return std::nullopt;
            }
        }
        else if (review.reviewMode == mw::core::PersistentMidiTakeReviewMode::Comp)
        {
            return std::nullopt;
        }

        return review;
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
        file << "  \"projectId\": " << static_cast<unsigned long long>(p.getStableId()) << ",\n";
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
        writeStringField(file, "projectDefaultCLAPPath", settings.clapPluginPath.string());
        writeStringField(file, "projectDefaultCLAPName", settings.clapPluginName);
        writeStringField(file, "projectDefaultCLAPVendor", settings.clapPluginVendor);
        writeStringField(file, "projectDefaultCLAPVersion", settings.clapPluginVersion);
        writeStringField(file, "projectDefaultCLAPCategory", settings.clapPluginCategory);
        writeStringField(file, "projectDefaultCLAPUid", settings.clapPluginUid);
        writeStringField(file, "projectDefaultCLAPCompatibility", settings.clapPluginCompatibilitySummary);
        writeStringField(file, "baseFileName", settings.baseFileName);
        writeStringField(file, "metadataTitle", settings.metadataTitle);
        writeStringField(file, "metadataArtist", settings.metadataArtist);
        writeStringField(file, "metadataAlbumArtist", settings.metadataAlbumArtist);
        writeStringField(file, "metadataAlbum", settings.metadataAlbum);
        writeStringField(file, "metadataGenre", settings.metadataGenre);
        writeStringField(file, "metadataTrackNumber", settings.metadataTrackNumber);
        writeStringField(file, "metadataYear", settings.metadataYear);
        file << "    \"albumArtEnabled\": " << (settings.albumArtEnabled ? "true" : "false") << ",\n";
        writeStringField(file, "albumArtPath", settings.albumArtPath.string());
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
            file << "      \"sourceTrimStartSamples\": " << mw::core::audioClipTrimStartSamples(clip) << ",\n";
            file << "      \"sourceTrimEndSamples\": " << mw::core::audioClipTrimEndSamples(clip) << ",\n";
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
            if (instrument.backendType != mw::core::SampleBackendType::VST3
                && instrument.backendType != mw::core::SampleBackendType::CLAP)
                serializedVst3 = {};
            file << "    {\n";
            file << "      \"trackId\": " << static_cast<unsigned long long>(t.getStableId()) << ",\n";
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
                file << "            \"backendType\": \"" << mw::core::effectSlotBackendTypeToString(effectSlot.backendType) << "\",\n";
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
                file << "        ";
                writeNoteObject(file, ns[n]);
                file << (n + 1 < ns.size() ? "," : "") << "\n";
            }

            const auto& pendingReview = t.getPendingMidiTakeReview();
            const auto& archivedReviews = t.getArchivedMidiTakeReviews();
            const bool writeControllerEvents = !t.isAudioClipTrack();
            const bool writePitchBendEvents = !t.isAudioClipTrack();
            const bool writeProgramChangeEvents = !t.isAudioClipTrack();
            const bool writeChannelPressureEvents = !t.isAudioClipTrack();
            const bool writePolyPressureEvents = !t.isAudioClipTrack();
            const bool write14BitControllerEvents = !t.isAudioClipTrack();
            const bool writeRpnEvents = !t.isAudioClipTrack();
            const bool writeNrpnEvents = !t.isAudioClipTrack();
            const bool writePendingReview = !t.isAudioClipTrack() && pendingReview.active;
            const bool writeArchivedReviews = !t.isAudioClipTrack() && !archivedReviews.empty();
            file << "      ]" << ((writeControllerEvents || writePitchBendEvents || writeProgramChangeEvents || writeChannelPressureEvents || writePolyPressureEvents || write14BitControllerEvents || writeRpnEvents || writeNrpnEvents || writePendingReview || writeArchivedReviews) ? "," : "") << "\n";
            if (writeControllerEvents)
            {
                auto controllerEvents = t.getMidiControllerEvents();
                std::sort(controllerEvents.begin(), controllerEvents.end(), mw::core::midiControllerEventOrder);
                file << "      \"controllerEvents\": [\n";
                for (std::size_t controllerIndex = 0; controllerIndex < controllerEvents.size(); ++controllerIndex)
                {
                    const auto& event = controllerEvents[controllerIndex];
                    file << "        {\"eventId\": " << static_cast<unsigned long long>(event.stableId)
                         << ", \"tick\": " << event.tick
                         << ", \"midiChannel\": " << event.midiChannel
                         << ", \"controller\": " << event.controller
                         << ", \"value\": " << event.value << "}"
                         << (controllerIndex + 1 < controllerEvents.size() ? "," : "") << "\n";
                }
                file << "      ]" << ((writePitchBendEvents || writeProgramChangeEvents || writeChannelPressureEvents || writePolyPressureEvents || write14BitControllerEvents || writeRpnEvents || writeNrpnEvents || writePendingReview || writeArchivedReviews) ? "," : "") << "\n";
            }
            if (writePitchBendEvents)
            {
                auto pitchBendEvents = t.getMidiPitchBendEvents();
                std::sort(pitchBendEvents.begin(), pitchBendEvents.end(), mw::core::midiPitchBendEventOrder);
                file << "      \"pitchBendEvents\": [\n";
                for (std::size_t bendIndex = 0; bendIndex < pitchBendEvents.size(); ++bendIndex)
                {
                    const auto& event = pitchBendEvents[bendIndex];
                    file << "        {\"eventId\": " << static_cast<unsigned long long>(event.stableId)
                         << ", \"tick\": " << event.tick
                         << ", \"midiChannel\": " << event.midiChannel
                         << ", \"value\": " << event.value << "}"
                         << (bendIndex + 1 < pitchBendEvents.size() ? "," : "") << "\n";
                }
                file << "      ]" << ((writeProgramChangeEvents || writeChannelPressureEvents || writePolyPressureEvents || write14BitControllerEvents || writeRpnEvents || writeNrpnEvents || writePendingReview || writeArchivedReviews) ? "," : "") << "\n";
            }
            if (writeProgramChangeEvents)
            {
                auto programChangeEvents = t.getMidiProgramChangeEvents();
                std::sort(programChangeEvents.begin(), programChangeEvents.end(), mw::core::midiProgramChangeEventOrder);
                file << "      \"programChangeEvents\": [\n";
                for (std::size_t programIndex = 0; programIndex < programChangeEvents.size(); ++programIndex)
                {
                    const auto& event = programChangeEvents[programIndex];
                    file << "        {\"eventId\": " << static_cast<unsigned long long>(event.stableId)
                         << ", \"tick\": " << event.tick
                         << ", \"midiChannel\": " << event.midiChannel
                         << ", \"program\": " << event.program << "}"
                         << (programIndex + 1 < programChangeEvents.size() ? "," : "") << "\n";
                }
                file << "      ]" << ((writeChannelPressureEvents || writePolyPressureEvents || write14BitControllerEvents || writeRpnEvents || writeNrpnEvents || writePendingReview || writeArchivedReviews) ? "," : "") << "\n";
            }
            if (writeChannelPressureEvents)
            {
                auto pressureEvents = t.getMidiChannelPressureEvents();
                std::sort(pressureEvents.begin(), pressureEvents.end(), mw::core::midiChannelPressureEventOrder);
                file << "      \"channelPressureEvents\": [\n";
                for (std::size_t pressureIndex = 0; pressureIndex < pressureEvents.size(); ++pressureIndex)
                {
                    const auto& event = pressureEvents[pressureIndex];
                    file << "        {\"eventId\": " << static_cast<unsigned long long>(event.stableId)
                         << ", \"tick\": " << event.tick
                         << ", \"midiChannel\": " << event.midiChannel
                         << ", \"pressure\": " << event.pressure << "}"
                         << (pressureIndex + 1 < pressureEvents.size() ? "," : "") << "\n";
                }
                file << "      ]" << ((writePolyPressureEvents || write14BitControllerEvents || writeRpnEvents || writeNrpnEvents || writePendingReview || writeArchivedReviews) ? "," : "") << "\n";
            }
            if (writePolyPressureEvents)
            {
                auto polyEvents = t.getMidiPolyPressureEvents();
                std::sort(polyEvents.begin(), polyEvents.end(), mw::core::midiPolyPressureEventOrder);
                file << "      \"polyPressureEvents\": [\n";
                for (std::size_t polyIndex = 0; polyIndex < polyEvents.size(); ++polyIndex)
                {
                    const auto& event = polyEvents[polyIndex];
                    file << "        {\"eventId\": " << static_cast<unsigned long long>(event.stableId)
                         << ", \"tick\": " << event.tick
                         << ", \"midiChannel\": " << event.midiChannel
                         << ", \"noteNumber\": " << event.noteNumber
                         << ", \"pressure\": " << event.pressure << "}"
                         << (polyIndex + 1 < polyEvents.size() ? "," : "") << "\n";
                }
                file << "      ]" << ((write14BitControllerEvents || writeRpnEvents || writeNrpnEvents || writePendingReview || writeArchivedReviews) ? "," : "") << "\n";
            }
            if (write14BitControllerEvents)
            {
                auto events = t.getMidi14BitControllerEvents();
                std::sort(events.begin(), events.end(), mw::core::midi14BitControllerEventOrder);
                file << "      \"controller14BitEvents\": [\n";
                for (std::size_t eventIndex = 0; eventIndex < events.size(); ++eventIndex)
                {
                    const auto& event = events[eventIndex];
                    file << "        {\"eventId\": " << static_cast<unsigned long long>(event.stableId)
                         << ", \"tick\": " << event.tick << ", \"midiChannel\": " << event.midiChannel
                         << ", \"controllerMsb\": " << event.controllerMsb << ", \"value\": " << event.value << "}"
                         << (eventIndex + 1 < events.size() ? "," : "") << "\n";
                }
                file << "      ]" << ((writeRpnEvents || writeNrpnEvents || writePendingReview || writeArchivedReviews) ? "," : "") << "\n";
            }
            if (writeRpnEvents)
            {
                auto events = t.getMidiRpnEvents(); std::sort(events.begin(), events.end(), mw::core::midiRpnEventOrder);
                file << "      \"rpnEvents\": [\n";
                for (std::size_t eventIndex = 0; eventIndex < events.size(); ++eventIndex)
                {
                    const auto& event = events[eventIndex];
                    file << "        {\"eventId\": " << static_cast<unsigned long long>(event.stableId)
                         << ", \"tick\": " << event.tick << ", \"midiChannel\": " << event.midiChannel
                         << ", \"parameter\": " << event.parameter << ", \"value\": " << event.value << "}"
                         << (eventIndex + 1 < events.size() ? "," : "") << "\n";
                }
                file << "      ]" << ((writeNrpnEvents || writePendingReview || writeArchivedReviews) ? "," : "") << "\n";
            }
            if (writeNrpnEvents)
            {
                auto events = t.getMidiNrpnEvents(); std::sort(events.begin(), events.end(), mw::core::midiNrpnEventOrder);
                file << "      \"nrpnEvents\": [\n";
                for (std::size_t eventIndex = 0; eventIndex < events.size(); ++eventIndex)
                {
                    const auto& event = events[eventIndex];
                    file << "        {\"eventId\": " << static_cast<unsigned long long>(event.stableId)
                         << ", \"tick\": " << event.tick << ", \"midiChannel\": " << event.midiChannel
                         << ", \"parameter\": " << event.parameter << ", \"value\": " << event.value << "}"
                         << (eventIndex + 1 < events.size() ? "," : "") << "\n";
                }
                file << "      ]" << ((writePendingReview || writeArchivedReviews) ? "," : "") << "\n";
            }
            if (writePendingReview)
            {
                file << "      \"pendingMidiTakeReview\": ";
                writePersistentMidiTakeReviewObject(file, pendingReview, "      ");
                file << (writeArchivedReviews ? "," : "") << "\n";
            }
            if (writeArchivedReviews)
            {
                file << "      \"archivedMidiTakeReviews\": [\n";
                for (std::size_t archiveIndex = 0; archiveIndex < archivedReviews.size(); ++archiveIndex)
                {
                    file << "        ";
                    writePersistentMidiTakeReviewObject(file, archivedReviews[archiveIndex], "        ");
                    file << (archiveIndex + 1 < archivedReviews.size() ? "," : "") << "\n";
                }
                file << "      ]\n";
            }
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

        const int sourceFormatVersion = getInt(text, "version", 9);
        mw::core::Project project(getString(text, "name", filePath.stem().string()));
        project.setStableId(static_cast<mw::core::StableId>(std::max<long long>(1, getLongLong(text, "projectId", static_cast<long long>(project.getStableId())))));
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
            settings.clapPluginPath = getString(settingsObject, "projectDefaultCLAPPath");
            settings.clapPluginName = getString(settingsObject, "projectDefaultCLAPName");
            settings.clapPluginVendor = getString(settingsObject, "projectDefaultCLAPVendor");
            settings.clapPluginVersion = getString(settingsObject, "projectDefaultCLAPVersion");
            settings.clapPluginCategory = getString(settingsObject, "projectDefaultCLAPCategory");
            settings.clapPluginUid = getString(settingsObject, "projectDefaultCLAPUid");
            settings.clapPluginCompatibilitySummary = getString(settingsObject, "projectDefaultCLAPCompatibility");
            settings.baseFileName = getString(settingsObject, "baseFileName", project.getName());
            settings.metadataTitle = getString(settingsObject, "metadataTitle");
            settings.metadataArtist = getString(settingsObject, "metadataArtist");
            settings.metadataAlbumArtist = getString(settingsObject, "metadataAlbumArtist");
            settings.metadataAlbum = getString(settingsObject, "metadataAlbum");
            settings.metadataGenre = getString(settingsObject, "metadataGenre");
            settings.metadataTrackNumber = getString(settingsObject, "metadataTrackNumber");
            settings.metadataYear = getString(settingsObject, "metadataYear");
            settings.albumArtEnabled = getBool(settingsObject, "albumArtEnabled", false);
            settings.albumArtPath = getString(settingsObject, "albumArtPath");
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
                clip.sourceTrimStartSamples = getLongLong(clipObject, "sourceTrimStartSamples", 0);
                clip.sourceTrimEndSamples = getLongLong(clipObject, "sourceTrimEndSamples", clip.durationSamples);
                mw::core::normalizeAudioClipTrim(clip);
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
            track.setStableId(static_cast<mw::core::StableId>(std::max<long long>(1, getLongLong(trackObject, "trackId", static_cast<long long>(track.getStableId())))));
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

                if (instrument.backendType != mw::core::SampleBackendType::VST3
                    && instrument.backendType != mw::core::SampleBackendType::CLAP)
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
                    slot.backendType = effectSlotBackendFromString(getString(effectSlotObject, "backendType", "VST3"));
                    const auto effectPluginObject = extractObject(effectSlotObject, "plugin");
                    if (!effectPluginObject.empty())
                        slot.plugin = parseVstPluginAssignmentObject(effectPluginObject);
                    if (slot.backendType == mw::core::EffectSlotBackendType::None && slot.plugin.hasPluginIdentity())
                        slot.backendType = mw::core::EffectSlotBackendType::VST3;
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

            if (sourceFormatVersion >= 16 && !track.isAudioClipTrack())
            {
                std::vector<mw::core::MidiControllerEvent> controllerEvents;
                const auto controllerArray = extractArray(trackObject, "controllerEvents");
                for (const auto& controllerObject : splitTopLevelObjects(controllerArray))
                {
                    const auto rawId = getLongLong(controllerObject, "eventId", 0);
                    const auto tick = getLongLong(controllerObject, "tick", -1);
                    const auto channel = getInt(controllerObject, "midiChannel", 0);
                    const auto controller = getInt(controllerObject, "controller", -1);
                    const auto value = getInt(controllerObject, "value", -1);
                    if (tick < 0 || channel < 1 || channel > 16 || controller < 0 || controller > 127 || value < 0 || value > 127)
                        continue;
                    const auto stableId = rawId > 0 ? static_cast<mw::core::StableId>(rawId) : 0;
                    auto duplicateTuple = std::find_if(controllerEvents.begin(), controllerEvents.end(), [&](const auto& event)
                    {
                        return event.tick == tick && event.midiChannel == channel && event.controller == controller;
                    });
                    if (duplicateTuple != controllerEvents.end())
                    {
                        duplicateTuple->value = value;
                        if (duplicateTuple->stableId == 0 && stableId != 0) duplicateTuple->stableId = stableId;
                    }
                    else
                    {
                        controllerEvents.emplace_back(stableId, tick, channel, controller, value);
                    }
                }
                track.setMidiControllerEvents(std::move(controllerEvents));
            }

            if (sourceFormatVersion >= 18 && !track.isAudioClipTrack())
            {
                std::vector<mw::core::MidiPitchBendEvent> bendEvents;
                const auto bendArray = extractArray(trackObject, "pitchBendEvents");
                for (const auto& bendObject : splitTopLevelObjects(bendArray))
                {
                    const auto rawId = getLongLong(bendObject, "eventId", 0);
                    const auto tick = getLongLong(bendObject, "tick", -1);
                    const auto channel = getInt(bendObject, "midiChannel", 0);
                    const auto value = getInt(bendObject, "value", 99999);
                    if (tick < 0 || channel < 1 || channel > 16
                        || value < mw::core::midiPitchBendMinimum || value > mw::core::midiPitchBendMaximum)
                        continue;
                    const auto stableId = rawId > 0 ? static_cast<mw::core::StableId>(rawId) : 0;
                    auto duplicateTuple = std::find_if(bendEvents.begin(), bendEvents.end(), [&](const auto& event)
                    { return event.tick == tick && event.midiChannel == channel; });
                    if (duplicateTuple != bendEvents.end())
                    {
                        duplicateTuple->value = value;
                        if (duplicateTuple->stableId == 0 && stableId != 0) duplicateTuple->stableId = stableId;
                    }
                    else
                        bendEvents.emplace_back(stableId, tick, channel, value);
                }
                track.setMidiPitchBendEvents(std::move(bendEvents));
            }

            if (sourceFormatVersion >= 19 && !track.isAudioClipTrack())
            {
                std::vector<mw::core::MidiProgramChangeEvent> programEvents;
                const auto programArray = extractArray(trackObject, "programChangeEvents");
                for (const auto& programObject : splitTopLevelObjects(programArray))
                {
                    const auto rawId = getLongLong(programObject, "eventId", 0);
                    const auto tick = getLongLong(programObject, "tick", -1);
                    const auto channel = getInt(programObject, "midiChannel", 0);
                    const auto program = getInt(programObject, "program", -1);
                    if (tick < 0 || channel < 1 || channel > 16 || program < 0 || program > 127)
                        continue;
                    const auto stableId = rawId > 0 ? static_cast<mw::core::StableId>(rawId) : 0;
                    auto duplicateTuple = std::find_if(programEvents.begin(), programEvents.end(), [&](const auto& event)
                    { return event.tick == tick && event.midiChannel == channel; });
                    if (duplicateTuple != programEvents.end())
                    {
                        duplicateTuple->program = program;
                        if (duplicateTuple->stableId == 0 && stableId != 0) duplicateTuple->stableId = stableId;
                    }
                    else
                        programEvents.emplace_back(stableId, tick, channel, program);
                }
                track.setMidiProgramChangeEvents(std::move(programEvents));
            }

            if (sourceFormatVersion >= 20 && !track.isAudioClipTrack())
            {
                std::vector<mw::core::MidiChannelPressureEvent> pressureEvents;
                const auto pressureArray = extractArray(trackObject, "channelPressureEvents");
                for (const auto& pressureObject : splitTopLevelObjects(pressureArray))
                {
                    const auto rawId = getLongLong(pressureObject, "eventId", 0);
                    const auto tick = getLongLong(pressureObject, "tick", -1);
                    const auto channel = getInt(pressureObject, "midiChannel", 0);
                    const auto pressureValue = getInt(pressureObject, "pressure", -1);
                    if (tick < 0 || channel < 1 || channel > 16
                        || pressureValue < mw::core::midiChannelPressureMinimum || pressureValue > mw::core::midiChannelPressureMaximum)
                        continue;
                    const auto stableId = rawId > 0 ? static_cast<mw::core::StableId>(rawId) : 0;
                    auto duplicateTuple = std::find_if(pressureEvents.begin(), pressureEvents.end(), [&](const auto& event)
                    { return event.tick == tick && event.midiChannel == channel; });
                    if (duplicateTuple != pressureEvents.end())
                    {
                        duplicateTuple->pressure = pressureValue;
                        if (duplicateTuple->stableId == 0 && stableId != 0) duplicateTuple->stableId = stableId;
                    }
                    else
                        pressureEvents.emplace_back(stableId, tick, channel, pressureValue);
                }
                track.setMidiChannelPressureEvents(std::move(pressureEvents));
            }

            if (sourceFormatVersion >= 21 && !track.isAudioClipTrack())
            {
                std::vector<mw::core::MidiPolyPressureEvent> polyEvents;
                const auto polyArray = extractArray(trackObject, "polyPressureEvents");
                for (const auto& polyObject : splitTopLevelObjects(polyArray))
                {
                    const auto rawId = getLongLong(polyObject, "eventId", 0);
                    const auto tick = getLongLong(polyObject, "tick", -1);
                    const auto channel = getInt(polyObject, "midiChannel", 0);
                    const auto note = getInt(polyObject, "noteNumber", -1);
                    const auto pressureValue = getInt(polyObject, "pressure", -1);
                    if (tick < 0 || channel < 1 || channel > 16 || note < 0 || note > 127
                        || pressureValue < mw::core::midiPolyPressureMinimum || pressureValue > mw::core::midiPolyPressureMaximum)
                        continue;
                    const auto stableId = rawId > 0 ? static_cast<mw::core::StableId>(rawId) : 0;
                    auto duplicateTuple = std::find_if(polyEvents.begin(), polyEvents.end(), [&](const auto& event)
                    { return event.tick == tick && event.midiChannel == channel && event.noteNumber == note; });
                    if (duplicateTuple != polyEvents.end())
                    {
                        duplicateTuple->pressure = pressureValue;
                        if (duplicateTuple->stableId == 0 && stableId != 0) duplicateTuple->stableId = stableId;
                    }
                    else
                        polyEvents.emplace_back(stableId, tick, channel, note, pressureValue);
                }
                track.setMidiPolyPressureEvents(std::move(polyEvents));
            }

            if (sourceFormatVersion >= 22 && !track.isAudioClipTrack())
            {
                std::vector<mw::core::Midi14BitControllerEvent> events14;
                for (const auto& object : splitTopLevelObjects(extractArray(trackObject, "controller14BitEvents")))
                {
                    const auto rawId=getLongLong(object,"eventId",0); const auto tick=getLongLong(object,"tick",-1);
                    const auto channel=getInt(object,"midiChannel",0); const auto controller=getInt(object,"controllerMsb",-1); const auto value=getInt(object,"value",-1);
                    if(tick<0||channel<1||channel>16||controller<mw::core::midi14BitControllerMinimum||controller>mw::core::midi14BitControllerMaximum||value<mw::core::midi14BitMinimum||value>mw::core::midi14BitMaximum) continue;
                    const auto id=rawId>0?static_cast<mw::core::StableId>(rawId):0; auto dup=std::find_if(events14.begin(),events14.end(),[&](const auto& e){return e.tick==tick&&e.midiChannel==channel&&e.controllerMsb==controller;});
                    if(dup!=events14.end()){dup->value=value;if(dup->stableId==0&&id!=0)dup->stableId=id;}else events14.emplace_back(id,tick,channel,controller,value);
                }
                track.setMidi14BitControllerEvents(std::move(events14));

                std::vector<mw::core::MidiRpnEvent> rpn;
                for (const auto& object : splitTopLevelObjects(extractArray(trackObject, "rpnEvents")))
                {
                    const auto rawId=getLongLong(object,"eventId",0);const auto tick=getLongLong(object,"tick",-1);const auto channel=getInt(object,"midiChannel",0);const auto parameter=getInt(object,"parameter",-1);const auto value=getInt(object,"value",-1);
                    if(tick<0||channel<1||channel>16||parameter<mw::core::midiParameterMinimum||parameter>=mw::core::midiRpnNullParameter||value<mw::core::midiParameterMinimum||value>mw::core::midiParameterMaximum)continue;
                    const auto id=rawId>0?static_cast<mw::core::StableId>(rawId):0;auto dup=std::find_if(rpn.begin(),rpn.end(),[&](const auto& e){return e.tick==tick&&e.midiChannel==channel&&e.parameter==parameter;});if(dup!=rpn.end()){dup->value=value;if(dup->stableId==0&&id!=0)dup->stableId=id;}else rpn.emplace_back(id,tick,channel,parameter,value);
                }
                track.setMidiRpnEvents(std::move(rpn));

                std::vector<mw::core::MidiNrpnEvent> nrpn;
                for (const auto& object : splitTopLevelObjects(extractArray(trackObject, "nrpnEvents")))
                {
                    const auto rawId=getLongLong(object,"eventId",0);const auto tick=getLongLong(object,"tick",-1);const auto channel=getInt(object,"midiChannel",0);const auto parameter=getInt(object,"parameter",-1);const auto value=getInt(object,"value",-1);
                    if(tick<0||channel<1||channel>16||parameter<mw::core::midiParameterMinimum||parameter>mw::core::midiParameterMaximum||value<mw::core::midiParameterMinimum||value>mw::core::midiParameterMaximum)continue;
                    const auto id=rawId>0?static_cast<mw::core::StableId>(rawId):0;auto dup=std::find_if(nrpn.begin(),nrpn.end(),[&](const auto& e){return e.tick==tick&&e.midiChannel==channel&&e.parameter==parameter;});if(dup!=nrpn.end()){dup->value=value;if(dup->stableId==0&&id!=0)dup->stableId=id;}else nrpn.emplace_back(id,tick,channel,parameter,value);
                }
                track.setMidiNrpnEvents(std::move(nrpn));
            }

            if (sourceFormatVersion >= 10 && !track.isAudioClipTrack())
            {
                const auto reviewObject = extractObject(trackObject, "pendingMidiTakeReview");
                if (const auto review = parsePersistentMidiTakeReviewObject(
                        reviewObject, track.getInstrument().midiChannel, sourceFormatVersion))
                {
                    track.setPendingMidiTakeReview(*review);
                }

                if (sourceFormatVersion >= 13)
                {
                    std::vector<mw::core::PersistentMidiTakeReview> archivedReviews;
                    const auto archivedArray = extractArray(trackObject, "archivedMidiTakeReviews");
                    for (const auto& archivedObject : splitTopLevelObjects(archivedArray))
                    {
                        if (auto archived = parsePersistentMidiTakeReviewObject(
                                archivedObject, track.getInstrument().midiChannel, sourceFormatVersion))
                        {
                            const bool duplicateId = archived->reviewSessionId != 0
                                && ((track.getPendingMidiTakeReview().active
                                     && track.getPendingMidiTakeReview().reviewSessionId == archived->reviewSessionId)
                                    || std::any_of(archivedReviews.begin(), archivedReviews.end(), [&](const auto& existing)
                                    {
                                        return existing.reviewSessionId == archived->reviewSessionId;
                                    }));
                            if (duplicateId)
                                continue;
                            if (archived->reviewSessionId == 0)
                                archived->reviewSessionId = mw::core::allocateStableId();
                            if (archived->reviewName.empty())
                                archived->reviewName = "Take Review " + std::to_string(archivedReviews.size() + 1U);
                            archivedReviews.push_back(std::move(*archived));
                        }
                    }
                    track.setArchivedMidiTakeReviews(std::move(archivedReviews));
                }
            }
        }

        project.ensureUniqueStableIds();
        return project;
    }
}
