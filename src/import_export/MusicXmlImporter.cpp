#include "import_export/MusicXmlImporter.h"

#include "app/AppPaths.h"

#include "core/InstrumentNameMatcher.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    std::string readFile(const std::filesystem::path& p)
    {
        std::ifstream f(p, std::ios::binary);
        if (!f) return {};

        std::ostringstream b;
        b << f.rdbuf();
        return b.str();
    }

    std::string trim(std::string value)
    {
        const auto first = value.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return {};

        const auto last = value.find_last_not_of(" \t\r\n");
        return value.substr(first, last - first + 1);
    }

    std::string between(const std::string& text, const std::string& start, const std::string& end, std::size_t pos = 0)
    {
        auto s = text.find(start, pos);
        if (s == std::string::npos) return {};

        s += start.size();
        auto e = text.find(end, s);
        if (e == std::string::npos) return {};

        return trim(text.substr(s, e - s));
    }

    int intBetween(const std::string& text, const std::string& start, const std::string& end, int fallback = 0)
    {
        const auto value = between(text, start, end);
        if (value.empty()) return fallback;

        try { return std::stoi(value); }
        catch (...) { return fallback; }
    }

    std::string attributeValue(const std::string& tag, const std::string& attributeName)
    {
        const auto pattern = attributeName + "=\"";
        auto pos = tag.find(pattern);
        if (pos == std::string::npos) return {};

        pos += pattern.size();
        auto end = tag.find('"', pos);
        if (end == std::string::npos) return {};

        return tag.substr(pos, end - pos);
    }

    int stepToSemitone(char step)
    {
        switch (step)
        {
            case 'C': return 0;
            case 'D': return 2;
            case 'E': return 4;
            case 'F': return 5;
            case 'G': return 7;
            case 'A': return 9;
            case 'B': return 11;
            default: return 0;
        }
    }

    int midiPitchFromNoteBlock(const std::string& noteBlock, int transposeChromatic)
    {
        const auto step = between(noteBlock, "<step>", "</step>");
        const int alter = intBetween(noteBlock, "<alter>", "</alter>", 0);
        const int octave = intBetween(noteBlock, "<octave>", "</octave>", 4);

        if (step.empty()) return 60;

        return (octave + 1) * 12 + stepToSemitone(step[0]) + alter + transposeChromatic;
    }

    std::string parsePartNameFromScorePartBlock(const std::string& block, const std::string& fallback)
    {
        auto name = between(block, "<part-name>", "</part-name>");
        if (!name.empty()) return name;

        name = between(block, "<instrument-name>", "</instrument-name>");
        if (!name.empty()) return name;

        return fallback;
    }


    struct PartInfo
    {
        std::string name;
        int midiChannel = -1;
        int midiProgram = -1;
    };

    std::map<std::string, std::string> parsePartNames(const std::string& xml)
    {
        std::map<std::string, std::string> partNames;
        std::size_t pos = 0;

        // Use "<score-part " so we do not match "<score-partwise>".
        while ((pos = xml.find("<score-part ", pos)) != std::string::npos)
        {
            const auto tagEnd = xml.find('>', pos);
            if (tagEnd == std::string::npos) break;

            const auto close = xml.find("</score-part>", tagEnd);
            if (close == std::string::npos) break;

            const auto tag = xml.substr(pos, tagEnd - pos + 1);
            const auto id = attributeValue(tag, "id");
            const auto block = xml.substr(tagEnd + 1, close - tagEnd - 1);

            if (!id.empty())
                partNames[id] = parsePartNameFromScorePartBlock(block, id);

            pos = close + std::string("</score-part>").size();
        }

        return partNames;
    }


    std::map<std::string, PartInfo> parsePartInfos(const std::string& xml)
    {
        std::map<std::string, PartInfo> partInfos;
        std::size_t pos = 0;

        while ((pos = xml.find("<score-part ", pos)) != std::string::npos)
        {
            const auto tagEnd = xml.find('>', pos);
            if (tagEnd == std::string::npos) break;

            const auto close = xml.find("</score-part>", tagEnd);
            if (close == std::string::npos) break;

            const auto tag = xml.substr(pos, tagEnd - pos + 1);
            const auto id = attributeValue(tag, "id");
            const auto block = xml.substr(tagEnd + 1, close - tagEnd - 1);

            if (!id.empty())
            {
                PartInfo info;
                info.name = parsePartNameFromScorePartBlock(block, id);

                const int midiChannel = intBetween(block, "<midi-channel>", "</midi-channel>", -1);
                const int midiProgram = intBetween(block, "<midi-program>", "</midi-program>", -1);

                if (midiChannel > 0)
                    info.midiChannel = midiChannel;

                if (midiProgram > 0)
                    info.midiProgram = midiProgram - 1; // MusicXML is usually 1-based; MIDI program is 0-based.

                partInfos[id] = info;
            }

            pos = close + std::string("</score-part>").size();
        }

        return partInfos;
    }

    int ticksFromDurationDivisions(int durationDivisions, int divisions)
    {
        if (divisions <= 0) divisions = 1;

        return static_cast<int>(
            (static_cast<double>(durationDivisions) / static_cast<double>(divisions)) *
            static_cast<double>(mw::core::Project::ticksPerQuarterNote)
        );
    }

    mw::core::Articulation parseArticulation(const std::string& noteBlock)
    {
        if (noteBlock.find("<staccato") != std::string::npos) return mw::core::Articulation::Staccato;
        if (noteBlock.find("<accent") != std::string::npos) return mw::core::Articulation::Accent;
        if (noteBlock.find("<slur") != std::string::npos) return mw::core::Articulation::Legato;
        return mw::core::Articulation::Normal;
    }

    int parseVelocity(const std::string& noteBlock, int currentVelocity)
    {
        if (noteBlock.find("<ppp") != std::string::npos) return 30;
        if (noteBlock.find("<pp") != std::string::npos) return 40;
        if (noteBlock.find("<p/>") != std::string::npos || noteBlock.find("<p ") != std::string::npos) return 55;
        if (noteBlock.find("<mp") != std::string::npos) return 70;
        if (noteBlock.find("<mf") != std::string::npos) return 85;
        if (noteBlock.find("<f/>") != std::string::npos || noteBlock.find("<f ") != std::string::npos) return 100;
        if (noteBlock.find("<ff") != std::string::npos) return 115;
        if (noteBlock.find("<fff") != std::string::npos) return 122;

        return currentVelocity;
    }

    int parseDirectionVelocity(const std::string& directionBlock, int currentVelocity)
    {
        return parseVelocity(directionBlock, currentVelocity);
    }

    int parseTransposeChromatic(const std::string& measureBlock, int currentTranspose)
    {
        const auto transposeBlock = between(measureBlock, "<transpose>", "</transpose>");
        if (transposeBlock.empty())
            return currentTranspose;

        return intBetween(transposeBlock, "<chromatic>", "</chromatic>", currentTranspose);
    }
}


    std::string lowerExtension(const std::filesystem::path& path)
    {
        auto ext = path.extension().string();

        for (auto& c : ext)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        return ext;
    }

    std::filesystem::path tempMxlExtractFolder(const std::filesystem::path& mxlPath)
    {
        auto safeName = mxlPath.stem().string();

        for (auto& c : safeName)
        {
            if (!std::isalnum(static_cast<unsigned char>(c)))
                c = '_';
        }

        return mw::app::AppPaths::tempFolder() / ("mxl_extract_" + safeName);
    }

    std::string quotePathForPowerShell(const std::filesystem::path& path)
    {
        auto text = path.string();
        std::string escaped;

        for (char c : text)
        {
            if (c == '\'')
                escaped += "''";
            else
                escaped += c;
        }

        return "'" + escaped + "'";
    }

    bool extractMxlWithPowerShell(const std::filesystem::path& mxlPath, const std::filesystem::path& outputFolder)
    {
        std::error_code ignored;
        std::filesystem::remove_all(outputFolder, ignored);
        std::filesystem::create_directories(outputFolder, ignored);

        // .mxl is a ZIP container. PowerShell Expand-Archive is built into modern Windows.
        // We copy it to a temporary .zip name because Expand-Archive is extension-sensitive.
        const auto tempZip = outputFolder.parent_path() / (outputFolder.filename().string() + ".zip");
        std::filesystem::copy_file(mxlPath, tempZip, std::filesystem::copy_options::overwrite_existing, ignored);

        if (!std::filesystem::exists(tempZip))
            return false;

        std::ostringstream command;
        command
            << "powershell -NoProfile -ExecutionPolicy Bypass -Command "
            << "\"Expand-Archive -LiteralPath "
            << quotePathForPowerShell(tempZip)
            << " -DestinationPath "
            << quotePathForPowerShell(outputFolder)
            << " -Force\"";

        const int exitCode = std::system(command.str().c_str());

        std::filesystem::remove(tempZip, ignored);

        return exitCode == 0;
    }

    std::filesystem::path findRootMusicXmlInExtractedMxl(const std::filesystem::path& folder)
    {
        const auto containerPath = folder / "META-INF" / "container.xml";

        if (std::filesystem::exists(containerPath))
        {
            const auto containerXml = readFile(containerPath);
            const auto fullPath = between(containerXml, "full-path=\"", "\"");

            if (!fullPath.empty())
            {
                const auto candidate = folder / std::filesystem::path(fullPath);

                if (std::filesystem::exists(candidate))
                    return candidate;
            }
        }

        std::vector<std::filesystem::path> candidates;

        for (const auto& entry : std::filesystem::recursive_directory_iterator(folder))
        {
            if (!entry.is_regular_file())
                continue;

            const auto ext = lowerExtension(entry.path());

            if (ext == ".musicxml" || ext == ".xml")
                candidates.push_back(entry.path());
        }

        if (!candidates.empty())
            return candidates.front();

        return {};
    }

namespace mw::import_export
{
    std::optional<mw::core::Project> MusicXmlImporter::importFromFile(const std::filesystem::path& filePath)
    {
        auto actualInputPath = filePath;
        const auto ext = lowerExtension(filePath);

        if (ext == ".mxl")
        {
            const auto extractFolder = tempMxlExtractFolder(filePath);

            if (!extractMxlWithPowerShell(filePath, extractFolder))
                return std::nullopt;

            actualInputPath = findRootMusicXmlInExtractedMxl(extractFolder);

            if (actualInputPath.empty())
                return std::nullopt;
        }

        const auto xml = readFile(actualInputPath);
        if (xml.empty()) return std::nullopt;

        mw::core::Project project(filePath.stem().string());
        const auto partInfos = parsePartInfos(xml);

        std::size_t pos = 0;
        int trackIndex = 0;

        while ((pos = xml.find("<part ", pos)) != std::string::npos)
        {
            const auto tagEnd = xml.find('>', pos);
            if (tagEnd == std::string::npos) break;

            const auto close = xml.find("</part>", tagEnd);
            if (close == std::string::npos) break;

            const auto tag = xml.substr(pos, tagEnd - pos + 1);
            const auto id = attributeValue(tag, "id");

            const auto infoIt = partInfos.find(id);
            const auto trackName = infoIt != partInfos.end() ? infoIt->second.name : (id.empty() ? "Imported Part" : id);

            auto& track = project.addTrack(trackName);

            auto assignment = mw::core::InstrumentNameMatcher::createDefaultAssignment(trackName, trackIndex);

            if (infoIt != partInfos.end())
            {
                if (infoIt->second.midiChannel > 0)
                    assignment.midiChannel = infoIt->second.midiChannel;

                if (infoIt->second.midiProgram >= 0)
                    assignment.midiProgram = infoIt->second.midiProgram;
            }

            track.setInstrumentAssignment(assignment);

            const auto partBlock = xml.substr(tagEnd + 1, close - tagEnd - 1);

            int divisions = 1;
            int currentVelocity = 100;
            int transposeChromatic = 0;
            std::int64_t currentTick = 0;
            std::int64_t previousNoteStart = 0;

            std::size_t measurePos = 0;
            while ((measurePos = partBlock.find("<measure", measurePos)) != std::string::npos)
            {
                const auto measureEnd = partBlock.find("</measure>", measurePos);
                if (measureEnd == std::string::npos) break;

                const auto measureBlock = partBlock.substr(measurePos, measureEnd - measurePos);

                const int newDivisions = intBetween(measureBlock, "<divisions>", "</divisions>", -1);
                if (newDivisions > 0) divisions = newDivisions;

                const int beats = intBetween(measureBlock, "<beats>", "</beats>", -1);
                const int beatType = intBetween(measureBlock, "<beat-type>", "</beat-type>", -1);
                if (beats > 0 && beatType > 0)
                    project.setTimeSignature({beats, beatType});

                const int tempo = intBetween(measureBlock, "<sound tempo=\"", "\"", -1);
                if (tempo > 0)
                    project.setTempoBpm(tempo);

                transposeChromatic = parseTransposeChromatic(measureBlock, transposeChromatic);

                std::size_t cursor = 0;
                while (cursor < measureBlock.size())
                {
                    const auto nextNote = measureBlock.find("<note", cursor);
                    const auto nextBackup = measureBlock.find("<backup>", cursor);
                    const auto nextForward = measureBlock.find("<forward>", cursor);
                    const auto nextDirection = measureBlock.find("<direction", cursor);

                    auto next = std::min({
                        nextNote == std::string::npos ? measureBlock.size() : nextNote,
                        nextBackup == std::string::npos ? measureBlock.size() : nextBackup,
                        nextForward == std::string::npos ? measureBlock.size() : nextForward,
                        nextDirection == std::string::npos ? measureBlock.size() : nextDirection
                    });

                    if (next == measureBlock.size())
                        break;

                    if (next == nextDirection)
                    {
                        const auto end = measureBlock.find("</direction>", nextDirection);
                        if (end == std::string::npos) break;

                        const auto block = measureBlock.substr(nextDirection, end - nextDirection);
                        currentVelocity = parseDirectionVelocity(block, currentVelocity);

                        cursor = end + std::string("</direction>").size();
                        continue;
                    }

                    if (next == nextBackup)
                    {
                        const auto end = measureBlock.find("</backup>", nextBackup);
                        if (end == std::string::npos) break;

                        const auto block = measureBlock.substr(nextBackup, end - nextBackup);
                        const int durationDivisions = intBetween(block, "<duration>", "</duration>", 0);
                        currentTick -= ticksFromDurationDivisions(durationDivisions, divisions);
                        if (currentTick < 0) currentTick = 0;

                        cursor = end + std::string("</backup>").size();
                        continue;
                    }

                    if (next == nextForward)
                    {
                        const auto end = measureBlock.find("</forward>", nextForward);
                        if (end == std::string::npos) break;

                        const auto block = measureBlock.substr(nextForward, end - nextForward);
                        const int durationDivisions = intBetween(block, "<duration>", "</duration>", 0);
                        currentTick += ticksFromDurationDivisions(durationDivisions, divisions);

                        cursor = end + std::string("</forward>").size();
                        continue;
                    }

                    if (next == nextNote)
                    {
                        const auto noteEnd = measureBlock.find("</note>", nextNote);
                        if (noteEnd == std::string::npos) break;

                        const auto noteBlock = measureBlock.substr(nextNote, noteEnd - nextNote);
                        const bool isRest = noteBlock.find("<rest") != std::string::npos;
                        const bool isChord = noteBlock.find("<chord") != std::string::npos;

                        const int durationDivisions = intBetween(noteBlock, "<duration>", "</duration>", divisions);
                        const auto durationTicks = static_cast<std::int64_t>(
                            ticksFromDurationDivisions(durationDivisions, divisions)
                        );

                        const auto startTick = isChord ? previousNoteStart : currentTick;
                        const auto velocity = parseVelocity(noteBlock, currentVelocity);

                        if (!isRest)
                        {
                            track.addNote(
                                mw::core::NoteEvent(
                                    midiPitchFromNoteBlock(noteBlock, transposeChromatic),
                                    velocity,
                                    startTick,
                                    durationTicks,
                                    track.getInstrument().midiChannel,
                                    parseArticulation(noteBlock)
                                )
                            );
                        }

                        if (!isChord)
                        {
                            previousNoteStart = currentTick;
                            currentTick += durationTicks;
                        }

                        cursor = noteEnd + std::string("</note>").size();
                        continue;
                    }
                }

                measurePos = measureEnd + std::string("</measure>").size();
            }

            ++trackIndex;
            pos = close + std::string("</part>").size();
        }

        if (project.getTracks().empty())
            return std::nullopt;

        return project;
    }
}
