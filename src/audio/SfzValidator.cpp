#include "audio/SfzValidator.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <system_error>

namespace
{
    std::string trim(std::string value)
    {
        const auto first = value.find_first_not_of(" \t\r\n\"");
        if (first == std::string::npos)
            return {};

        const auto last = value.find_last_not_of(" \t\r\n\"");
        return value.substr(first, last - first + 1);
    }

    std::string stripComment(std::string line)
    {
        const auto pos = line.find("//");
        if (pos != std::string::npos)
            line = line.substr(0, pos);

        return line;
    }

    std::string lower(std::string value)
    {
        for (auto& c : value)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        return value;
    }

    std::string extractValueAfterKey(const std::string& line, const std::string& key)
    {
        const auto lowerLine = lower(line);
        const auto lowerKey = lower(key);

        auto pos = lowerLine.find(lowerKey);
        if (pos == std::string::npos)
            return {};

        pos += key.size();

        while (pos < line.size() && std::isspace(static_cast<unsigned char>(line[pos])))
            ++pos;

        if (pos >= line.size())
            return {};

        if (line[pos] == '"')
        {
            const auto endQuote = line.find('"', pos + 1);
            if (endQuote != std::string::npos)
                return trim(line.substr(pos + 1, endQuote - pos - 1));
        }

        auto end = line.find_first_of(" \t\r\n", pos);
        if (end == std::string::npos)
            end = line.size();

        return trim(line.substr(pos, end - pos));
    }

    std::filesystem::path sidecarPackFolderFor(const std::filesystem::path& rootSfzPath)
    {
        if (rootSfzPath.empty())
            return {};

        const auto candidate = rootSfzPath.parent_path() / rootSfzPath.stem();

        std::error_code ec;
        if (std::filesystem::exists(candidate, ec) && std::filesystem::is_directory(candidate, ec))
            return candidate;

        return {};
    }

    std::filesystem::path resolveExistingPath(
        const std::filesystem::path& currentBaseFolder,
        const std::filesystem::path& rootSidecarFolder,
        const std::string& rawPath
    )
    {
        const auto requestedPath = std::filesystem::path(rawPath);

        std::error_code ec;
        if (requestedPath.is_absolute())
        {
            return std::filesystem::exists(requestedPath, ec) ? requestedPath.lexically_normal() : std::filesystem::path();
        }

        const auto baseCandidate = (currentBaseFolder / requestedPath).lexically_normal();
        if (std::filesystem::exists(baseCandidate, ec))
            return baseCandidate;

        if (!rootSidecarFolder.empty())
        {
            const auto sidecarCandidate = (rootSidecarFolder / requestedPath).lexically_normal();
            if (std::filesystem::exists(sidecarCandidate, ec))
                return sidecarCandidate;
        }

        return {};
    }

    std::filesystem::path bestDisplayPathForMissing(
        const std::filesystem::path& currentBaseFolder,
        const std::filesystem::path& rootSidecarFolder,
        const std::string& rawPath
    )
    {
        const auto requestedPath = std::filesystem::path(rawPath);

        if (requestedPath.is_absolute())
            return requestedPath.lexically_normal();

        if (!rootSidecarFolder.empty())
            return (rootSidecarFolder / requestedPath).lexically_normal();

        return (currentBaseFolder / requestedPath).lexically_normal();
    }

    void scanSfzFile(
        const std::filesystem::path& sfzPath,
        const std::filesystem::path& rootSidecarFolder,
        mw::audio::SfzValidationResult& result,
        std::set<std::filesystem::path>& visited
    )
    {
        auto normalizedPath = std::filesystem::absolute(sfzPath).lexically_normal();

        if (visited.count(normalizedPath) > 0)
            return;

        visited.insert(normalizedPath);

        std::ifstream file(sfzPath);

        if (!file)
        {
            ++result.missingSamples;
            result.missingSamplePaths.push_back(sfzPath);
            return;
        }

        const auto baseFolder = sfzPath.parent_path();

        std::string line;
        while (std::getline(file, line))
        {
            line = stripComment(line);

            const auto includePath = extractValueAfterKey(line, "#include");
            if (!includePath.empty())
            {
                auto fullIncludePath = resolveExistingPath(baseFolder, rootSidecarFolder, includePath);

                if (fullIncludePath.empty())
                    fullIncludePath = bestDisplayPathForMissing(baseFolder, rootSidecarFolder, includePath);

                scanSfzFile(fullIncludePath, rootSidecarFolder, result, visited);
            }

            const auto samplePath = extractValueAfterKey(line, "sample=");
            if (!samplePath.empty())
            {
                ++result.sampleReferences;

                // SFZ sample paths are normally relative to the current .sfz file.
                // Poor Man's Studio also supports a sidecar pack folder beside the
                // top-level .sfz, for example:
                //   workspace/sfz/MyKit.sfz
                //   workspace/sfz/MyKit/<instrument folders and included pack files>
                const auto fullSamplePath = resolveExistingPath(baseFolder, rootSidecarFolder, samplePath);

                if (fullSamplePath.empty())
                {
                    ++result.missingSamples;

                    if (result.missingSamplePaths.size() < 25)
                        result.missingSamplePaths.push_back(bestDisplayPathForMissing(baseFolder, rootSidecarFolder, samplePath));
                }
            }
        }
    }
}

namespace mw::audio
{
    SfzValidationResult SfzValidator::validateSampleReferences(const std::filesystem::path& sfzPath)
    {
        SfzValidationResult result;

        if (sfzPath.empty())
        {
            result.message = "SFZ path is empty.";
            return result;
        }

        if (!std::filesystem::exists(sfzPath))
        {
            result.message = "SFZ file does not exist: " + sfzPath.string();
            return result;
        }

        std::set<std::filesystem::path> visited;
        const auto rootSidecarFolder = sidecarPackFolderFor(sfzPath);
        scanSfzFile(sfzPath, rootSidecarFolder, result, visited);

        result.ok = result.missingSamples == 0;

        std::ostringstream message;
        message << "SFZ preflight: "
                << result.sampleReferences
                << " sample reference(s), "
                << result.missingSamples
                << " missing sample file(s).";

        if (!rootSidecarFolder.empty())
            message << "\nSidecar pack root checked: " << rootSidecarFolder.string();

        if (!result.ok)
        {
            message << "\nFirst missing sample paths:";

            for (const auto& missing : result.missingSamplePaths)
                message << "\n  " << missing.string();
        }

        result.message = message.str();
        return result;
    }
}
