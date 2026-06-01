#include "vst/VstSnapshotStore.h"

#include "app/AppPaths.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <system_error>

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

    std::string lowerCopy(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return value;
    }

    std::uint64_t fnv1a64(const std::string& text)
    {
        std::uint64_t hash = 1469598103934665603ull;
        for (unsigned char c : text)
        {
            hash ^= static_cast<std::uint64_t>(c);
            hash *= 1099511628211ull;
        }
        return hash;
    }

    std::string hexHash(const std::string& text)
    {
        std::ostringstream out;
        out << std::hex << std::setw(16) << std::setfill('0') << fnv1a64(text);
        return out.str();
    }

    std::filesystem::path snapshotFolder()
    {
        return mw::app::AppPaths::vst3Folder() / "snapshots";
    }


    std::string snapshotFileName(const mw::core::VstPluginAssignment& plugin, mw::vst::VstSnapshotRole role, int slot)
    {
        const auto identity = mw::vst::VstSnapshotStore::makeIdentityKey(plugin, role);
        return mw::vst::VstSnapshotStore::roleToString(role)
            + "_" + hexHash(identity)
            + "_slot" + std::to_string(slot)
            + ".snapshot.txt";
    }

    std::filesystem::path snapshotPathInFolder(const std::filesystem::path& folder,
                                               const mw::core::VstPluginAssignment& plugin,
                                               mw::vst::VstSnapshotRole role,
                                               int slot)
    {
        return folder / snapshotFileName(plugin, role, slot);
    }

    std::filesystem::path snapshotPath(const mw::core::VstPluginAssignment& plugin, mw::vst::VstSnapshotRole role, int slot)
    {
        return snapshotPathInFolder(snapshotFolder(), plugin, role, slot);
    }


    std::string nowLocalString()
    {
        const auto now = std::chrono::system_clock::now();
        const auto tt = std::chrono::system_clock::to_time_t(now);
        std::tm tmValue{};

#if defined(_WIN32)
        localtime_s(&tmValue, &tt);
#else
        localtime_r(&tt, &tmValue);
#endif

        std::ostringstream out;
        out << std::put_time(&tmValue, "%Y-%m-%d %H:%M:%S");
        return out.str();
    }

    std::map<std::string, std::string> readKeyValueFile(const std::filesystem::path& path)
    {
        std::map<std::string, std::string> values;
        std::ifstream input(path);
        std::string line;

        while (std::getline(input, line))
        {
            const auto equals = line.find('=');
            if (equals == std::string::npos)
                continue;

            const auto key = trim(line.substr(0, equals));
            if (!key.empty())
                values[key] = line.substr(equals + 1);
        }

        return values;
    }

    std::string getValue(const std::map<std::string, std::string>& values, const std::string& key)
    {
        const auto it = values.find(key);
        return it == values.end() ? std::string{} : it->second;
    }

    bool validSlot(int slot)
    {
        return slot >= mw::vst::VstSnapshotStore::kFirstSlot && slot <= mw::vst::VstSnapshotStore::kLastSlot;
    }

    void setError(std::string* errorMessage, const std::string& message)
    {
        if (errorMessage != nullptr)
            *errorMessage = message;
    }
}

namespace mw::vst
{
    std::string VstSnapshotStore::roleToString(VstSnapshotRole role)
    {
        return role == VstSnapshotRole::Effect ? "effect" : "instrument";
    }

    std::string VstSnapshotStore::makeIdentityKey(const mw::core::VstPluginAssignment& plugin, VstSnapshotRole role)
    {
        std::ostringstream key;
        key << roleToString(role) << "|";
        key << lowerCopy(plugin.uid) << "|";
        key << lowerCopy(plugin.bundlePath.string()) << "|";
        key << lowerCopy(plugin.name) << "|";
        key << lowerCopy(plugin.vendor) << "|";
        key << lowerCopy(plugin.category) << "|";
        key << lowerCopy(plugin.version);
        return key.str();
    }

    bool VstSnapshotStore::saveSnapshot(const mw::core::VstPluginAssignment& plugin,
                                        VstSnapshotRole role,
                                        int slot,
                                        const std::string& stateBase64,
                                        std::string* errorMessage)
    {
        if (!validSlot(slot))
        {
            setError(errorMessage, "Snapshot slot must be 1-5.");
            return false;
        }

        if (!plugin.hasPluginIdentity())
        {
            setError(errorMessage, "No plugin identity is available for this snapshot.");
            return false;
        }

        if (stateBase64.empty())
        {
            setError(errorMessage, "The plugin did not return snapshot state data.");
            return false;
        }

        std::error_code ignored;
        std::filesystem::create_directories(snapshotFolder(), ignored);

        const auto path = snapshotPath(plugin, role, slot);
        const auto tempPath = std::filesystem::path(path.string() + ".tmp");
        {
            std::ofstream output(tempPath, std::ios::trunc);
            if (!output)
            {
                setError(errorMessage, "Could not write the snapshot file.");
                return false;
            }

            output << "format=PoorMansStudioVstSnapshot.v1\n";
            output << "role=" << roleToString(role) << "\n";
            output << "slot=" << slot << "\n";
            output << "identityKey=" << makeIdentityKey(plugin, role) << "\n";
            output << "pluginName=" << plugin.name << "\n";
            output << "pluginVendor=" << plugin.vendor << "\n";
            output << "pluginUid=" << plugin.uid << "\n";
            output << "pluginBundlePath=" << plugin.bundlePath.string() << "\n";
            output << "savedAtLocal=" << nowLocalString() << "\n";
            output << "stateBase64=" << stateBase64 << "\n";

            if (!output.good())
            {
                setError(errorMessage, "Could not finish writing the snapshot file.");
                return false;
            }
        }

        std::error_code copyError;
        std::filesystem::copy_file(tempPath, path, std::filesystem::copy_options::overwrite_existing, copyError);
        std::filesystem::remove(tempPath, ignored);
        if (copyError)
        {
            setError(errorMessage, "Could not replace the previous snapshot file.");
            return false;
        }

        return true;
    }

    std::optional<VstSnapshotRecord> VstSnapshotStore::loadSnapshot(const mw::core::VstPluginAssignment& plugin,
                                                                    VstSnapshotRole role,
                                                                    int slot,
                                                                    std::string* errorMessage)
    {
        if (!validSlot(slot))
        {
            setError(errorMessage, "Snapshot slot must be 1-5.");
            return std::nullopt;
        }

        if (!plugin.hasPluginIdentity())
        {
            setError(errorMessage, "No plugin identity is available for this snapshot.");
            return std::nullopt;
        }

        const auto path = snapshotPath(plugin, role, slot);
        if (!std::filesystem::exists(path))
        {
            setError(errorMessage, "No snapshot has been saved in this slot for this plugin.");
            return std::nullopt;
        }

        const auto values = readKeyValueFile(path);
        if (getValue(values, "format") != "PoorMansStudioVstSnapshot.v1")
        {
            setError(errorMessage, "Snapshot file format was not recognized.");
            return std::nullopt;
        }

        if (getValue(values, "role") != roleToString(role))
        {
            setError(errorMessage, "Snapshot role does not match this editor.");
            return std::nullopt;
        }

        if (getValue(values, "identityKey") != makeIdentityKey(plugin, role))
        {
            setError(errorMessage, "Snapshot belongs to a different plugin.");
            return std::nullopt;
        }

        VstSnapshotRecord record;
        record.role = role;
        record.slot = slot;
        record.identityKey = getValue(values, "identityKey");
        record.pluginName = getValue(values, "pluginName");
        record.pluginVendor = getValue(values, "pluginVendor");
        record.pluginUid = getValue(values, "pluginUid");
        record.pluginBundlePath = getValue(values, "pluginBundlePath");
        record.savedAtLocal = getValue(values, "savedAtLocal");
        record.stateBase64 = getValue(values, "stateBase64");

        if (record.stateBase64.empty())
        {
            setError(errorMessage, "Snapshot did not contain plugin state data.");
            return std::nullopt;
        }

        return record;
    }

    bool VstSnapshotStore::snapshotExists(const mw::core::VstPluginAssignment& plugin,
                                          VstSnapshotRole role,
                                          int slot)
    {
        if (!validSlot(slot) || !plugin.hasPluginIdentity())
            return false;

        return std::filesystem::exists(snapshotPath(plugin, role, slot));
    }

    bool VstSnapshotStore::clearSnapshot(const mw::core::VstPluginAssignment& plugin,
                                         VstSnapshotRole role,
                                         int slot,
                                         std::string* errorMessage)
    {
        if (!validSlot(slot))
        {
            setError(errorMessage, "Snapshot slot must be 1-5.");
            return false;
        }

        if (!plugin.hasPluginIdentity())
        {
            setError(errorMessage, "No plugin identity is available for this snapshot.");
            return false;
        }

        std::error_code currentError;
        std::filesystem::remove(snapshotPath(plugin, role, slot), currentError);

        if (currentError)
        {
            setError(errorMessage, "Could not clear the current snapshot slot.");
            return false;
        }

        return true;
    }

    bool VstSnapshotStore::clearAllSnapshots(const mw::core::VstPluginAssignment& plugin,
                                             VstSnapshotRole role,
                                             std::string* errorMessage)
    {
        if (!plugin.hasPluginIdentity())
        {
            setError(errorMessage, "No plugin identity is available for these snapshots.");
            return false;
        }

        for (int slot = kFirstSlot; slot <= kLastSlot; ++slot)
        {
            if (!clearSnapshot(plugin, role, slot, errorMessage))
                return false;
        }

        return true;
    }
}
