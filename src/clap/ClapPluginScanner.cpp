#include "clap/ClapPluginScanner.h"

#include "app/AppPaths.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <initializer_list>
#include <set>
#include <string>
#include <system_error>

namespace
{
    std::string lowerCopy(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return value;
    }

    bool hasClapExtension(const std::filesystem::path& path)
    {
        return lowerCopy(path.extension().string()) == ".clap";
    }

    bool pathHasClapParent(const std::filesystem::path& path)
    {
        auto parent = path.parent_path();
        while (!parent.empty())
        {
            if (hasClapExtension(parent))
                return true;
            const auto next = parent.parent_path();
            if (next == parent)
                break;
            parent = next;
        }
        return false;
    }

    bool containsAny(const std::string& lowerText, std::initializer_list<const char*> needles)
    {
        for (const auto* needle : needles)
            if (lowerText.find(needle) != std::string::npos)
                return true;
        return false;
    }

    struct InferredClapKind
    {
        mw::clap::ClapPluginKind kind = mw::clap::ClapPluginKind::Unknown;
        std::string reason;
    };

    InferredClapKind inferKindFromPathText(const std::filesystem::path& path)
    {
        const auto text = lowerCopy(path.stem().string() + " " + path.parent_path().filename().string());

        if (containsAny(text, { "synth", "sampler", "piano", "organ", "drum", "rompler", "instrument" }))
            return { mw::clap::ClapPluginKind::Instrument, "Metadata-only scan inferred an instrument role from the CLAP filename/folder text." };

        if (containsAny(text, { "midi", "note", "chord", "arp", "arpeggiator" }))
            return { mw::clap::ClapPluginKind::MidiTool, "Metadata-only scan inferred a MIDI-tool role from the CLAP filename/folder text." };

        if (containsAny(text, { "delay", "reverb", "compressor", "limiter", "eq", "equalizer", "filter", "chorus", "flanger", "phaser", "distortion", "saturator", "gate", "deesser", "de-esser", "effect", "fx" }))
            return { mw::clap::ClapPluginKind::Effect, "Metadata-only scan inferred an effect role from the CLAP filename/folder text." };

        return { mw::clap::ClapPluginKind::Unknown, "Metadata-only scan found a CLAP plugin. Role validation requires CLAP helper/ABI inspection." };
    }

    std::filesystem::path findLikelyBinary(const std::filesystem::path& pluginPath)
    {
        if (std::filesystem::is_regular_file(pluginPath))
            return pluginPath;

        std::error_code ec;
        if (!std::filesystem::is_directory(pluginPath, ec))
            return {};

#if defined(_WIN32)
        const auto expected = pluginPath / "Contents" / "x86_64-win" / pluginPath.filename();
        if (std::filesystem::exists(expected, ec))
            return expected;
#endif

        for (const auto& entry : std::filesystem::recursive_directory_iterator(pluginPath, ec))
        {
            if (ec)
                break;

            if (!entry.is_regular_file(ec))
                continue;

            if (hasClapExtension(entry.path()))
                return entry.path();
        }

        return {};
    }

    std::vector<std::filesystem::path> uniqueExistingFolders(std::vector<std::filesystem::path> folders)
    {
        std::vector<std::filesystem::path> result;
        std::set<std::string> seen;
        std::error_code ec;

        for (auto folder : folders)
        {
            if (folder.empty() || !std::filesystem::exists(folder, ec))
                continue;

            auto canonical = std::filesystem::weakly_canonical(folder, ec);
            if (ec)
                canonical = folder.lexically_normal();

            const auto key = lowerCopy(canonical.string());
            if (seen.insert(key).second)
                result.push_back(canonical);
        }

        return result;
    }

    std::vector<std::filesystem::path> discoverOuterClapPlugins(const std::filesystem::path& root)
    {
        std::vector<std::filesystem::path> results;
        std::error_code ec;

        if (root.empty() || !std::filesystem::exists(root, ec))
            return results;

        if (hasClapExtension(root) && !pathHasClapParent(root))
        {
            results.push_back(root);
            return results;
        }

        if (!std::filesystem::is_directory(root, ec))
            return results;

        const auto options = std::filesystem::directory_options::skip_permission_denied;
        std::filesystem::recursive_directory_iterator it(root, options, ec);
        std::filesystem::recursive_directory_iterator end;

        while (!ec && it != end)
        {
            const auto path = it->path();

            if (hasClapExtension(path))
            {
                if (!pathHasClapParent(path))
                    results.push_back(path);

                if (it->is_directory(ec))
                    it.disable_recursion_pending();
            }

            it.increment(ec);
            if (ec)
                ec.clear();
        }

        return results;
    }
}

namespace mw::clap
{
    std::vector<std::filesystem::path> ClapPluginScanner::defaultScanFolders(bool includeWorkspaceFolder, bool includeSystemFolders)
    {
        std::vector<std::filesystem::path> folders;

        if (includeWorkspaceFolder)
            folders.push_back(mw::app::AppPaths::clapFolder());

#if defined(_WIN32)
        if (includeSystemFolders)
        {
            folders.emplace_back("C:\\Program Files\\Common Files\\CLAP");
            folders.emplace_back("C:\\Program Files (x86)\\Common Files\\CLAP");
        }
#else
        if (includeSystemFolders)
        {
            folders.emplace_back("/usr/lib/clap");
            folders.emplace_back("/usr/local/lib/clap");
            if (const auto* home = std::getenv("HOME"))
                folders.emplace_back(std::filesystem::path(home) / ".clap");
        }
#endif

        return uniqueExistingFolders(std::move(folders));
    }

    bool ClapPluginScanner::isOuterClapPlugin(const std::filesystem::path& path)
    {
        return hasClapExtension(path) && !pathHasClapParent(path);
    }

    ClapPluginDescriptor ClapPluginScanner::inspectPluginPath(const std::filesystem::path& pluginPath)
    {
        ClapPluginDescriptor descriptor;
        descriptor.pluginPath = pluginPath;
        descriptor.name = pluginPath.stem().string();
        descriptor.status = ClapPluginScanStatus::Candidate;
        descriptor.statusMessage = "Found CLAP plugin by path. If the CLAP helper is available, descriptor metadata will be probed out of process.";
        descriptor.metadataOnly = true;

        std::error_code ec;
        if (!std::filesystem::exists(pluginPath, ec))
        {
            descriptor.status = ClapPluginScanStatus::Missing;
            descriptor.statusMessage = "CLAP plugin path was not found.";
            return descriptor;
        }

        descriptor.binaryPath = findLikelyBinary(pluginPath);
        const auto inferred = inferKindFromPathText(pluginPath);
        descriptor.detectedKind = inferred.kind;
        descriptor.kind = inferred.kind;
        descriptor.classificationReason = inferred.reason;

        return descriptor;
    }

    std::vector<ClapPluginDescriptor> ClapPluginScanner::scanFolders(const std::vector<std::filesystem::path>& folders)
    {
        std::vector<ClapPluginDescriptor> plugins;
        std::set<std::string> seen;
        std::error_code ec;

        for (const auto& folder : folders)
        {
            if (folder.empty() || !std::filesystem::exists(folder, ec))
                continue;

            for (const auto& path : discoverOuterClapPlugins(folder))
            {
                auto canonical = std::filesystem::weakly_canonical(path, ec);
                if (ec)
                {
                    canonical = path.lexically_normal();
                    ec.clear();
                }

                const auto key = lowerCopy(canonical.string());
                if (!seen.insert(key).second)
                    continue;

                plugins.push_back(inspectPluginPath(canonical));
            }
        }

        std::sort(plugins.begin(), plugins.end(), [](const auto& a, const auto& b)
        {
            return lowerCopy(a.displayName()) < lowerCopy(b.displayName());
        });

        return plugins;
    }

    std::vector<ClapPluginDescriptor> ClapPluginScanner::scan(const ClapScanOptions& options)
    {
        auto folders = defaultScanFolders(options.includeWorkspaceFolder, options.includeSystemFolders);
        folders.insert(folders.end(), options.scanFolders.begin(), options.scanFolders.end());
        return scanFolders(uniqueExistingFolders(std::move(folders)));
    }
}
