#include "core/InstrumentNameMatcher.h"
#include <algorithm>
#include <cctype>
#include <map>
#include <string>
#include <vector>

namespace
{
    bool contains(const std::string& text, const std::string& needle)
    {
        return text.find(needle) != std::string::npos;
    }

    std::string lowerCopy(std::string value)
    {
        for (auto& c : value)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return value;
    }

    std::string removeCommonNoiseWords(std::string value)
    {
        const std::vector<std::string> noise =
        {
            "solo ", "section ", "ensemble ", "part ", "staff ",
            "instrument ", "1", "2", "3", "4"
        };

        for (const auto& word : noise)
        {
            std::size_t pos = 0;
            while ((pos = value.find(word, pos)) != std::string::npos)
                value.erase(pos, word.size());
        }

        return value;
    }
}

namespace mw::core
{
    std::string InstrumentNameMatcher::normalizeInstrumentName(const std::string& name)
    {
        auto n = lowerCopy(name);
        n = removeCommonNoiseWords(n);

        const std::map<std::string, std::string> aliases =
        {
            {"vln", "violin"},
            {"vl", "violin"},
            {"vn", "violin"},
            {"violins", "violin"},
            {"vc", "cello"},
            {"violoncello", "cello"},
            {"cellos", "cello"},
            {"vla", "viola"},
            {"cb", "contrabass"},
            {"db", "contrabass"},
            {"double bass", "contrabass"},
            {"bass", "contrabass"},
            {"pno", "piano"},
            {"pf", "piano"},
            {"keyboard", "piano"},
            {"gtr", "guitar"},
            {"flt", "flute"},
            {"fl", "flute"},
            {"clar", "clarinet"},
            {"cl", "clarinet"},
            {"tpt", "trumpet"},
            {"trp", "trumpet"},
            {"hn", "french horn"},
            {"fr horn", "french horn"},
            {"perc", "percussion"},
            {"drums", "percussion"}
        };

        for (const auto& [alias, canonical] : aliases)
        {
            if (contains(n, alias))
                return canonical;
        }

        if (contains(n, "piano")) return "piano";
        if (contains(n, "violin")) return "violin";
        if (contains(n, "viola")) return "viola";
        if (contains(n, "cello")) return "cello";
        if (contains(n, "contrabass")) return "contrabass";
        if (contains(n, "flute")) return "flute";
        if (contains(n, "clarinet")) return "clarinet";
        if (contains(n, "trumpet")) return "trumpet";
        if (contains(n, "horn")) return "french horn";
        if (contains(n, "guitar")) return "guitar";
        if (contains(n, "percussion")) return "percussion";

        return n.empty() ? "unknown" : n;
    }

    int InstrumentNameMatcher::guessGeneralMidiProgram(const std::string& normalizedName)
    {
        if (normalizedName == "piano") return 0;
        if (normalizedName == "guitar") return 24;
        if (normalizedName == "violin") return 40;
        if (normalizedName == "viola") return 41;
        if (normalizedName == "cello") return 42;
        if (normalizedName == "contrabass") return 43;
        if (normalizedName == "trumpet") return 56;
        if (normalizedName == "french horn") return 60;
        if (normalizedName == "clarinet") return 71;
        if (normalizedName == "flute") return 73;
        if (normalizedName == "percussion") return 0;
        return 0;
    }

    InstrumentAssignment InstrumentNameMatcher::createDefaultAssignment(
        const std::string& importedName,
        int trackIndex
    )
    {
        InstrumentAssignment assignment;
        assignment.originalImportedName = importedName;
        assignment.normalizedName = normalizeInstrumentName(importedName);
        assignment.displayName = importedName;
        assignment.backendType = SampleBackendType::None;
        assignment.midiChannel = std::min(trackIndex + 1, 16);
        assignment.midiBank = 0;
        assignment.midiProgram = guessGeneralMidiProgram(assignment.normalizedName);
        assignment.presetName = assignment.normalizedName;
        assignment.wasAutoMatched = assignment.normalizedName != "unknown";
        assignment.matchConfidence = assignment.wasAutoMatched ? 0.75f : 0.0f;
        return assignment;
    }
}
