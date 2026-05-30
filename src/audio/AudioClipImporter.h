#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include "core/AudioClip.h"

namespace mw::audio
{
    struct AudioClipImportRequest
    {
        std::filesystem::path sourcePath;
        std::filesystem::path projectFolder;
        std::filesystem::path ffmpegExePath;
        mw::core::AudioClipSavedFormat savedFormat = mw::core::AudioClipSavedFormat::Wav;
        int qualityKbps = 320;
        int channelCount = 2;
        bool imported = true;
        std::string preferredName;
    };

    struct AudioClipImportResult
    {
        bool success = false;
        std::filesystem::path relativePath;
        std::filesystem::path absolutePath;
        std::uintmax_t sizeBytes = 0;
        std::string message;
    };

    class AudioClipImporter
    {
    public:
        static bool isSupportedImportPath(const std::filesystem::path& path);
        static std::uintmax_t estimateRequiredBytes(const std::filesystem::path& sourcePath, mw::core::AudioClipSavedFormat savedFormat);
        static bool hasEnoughFreeSpace(const std::filesystem::path& targetFolder, std::uintmax_t requiredBytes, std::string& message);
        static AudioClipImportResult importToProject(const AudioClipImportRequest& request);
        static std::filesystem::path audioFolderFor(const std::filesystem::path& projectFolder, bool imported);
        static std::filesystem::path tempAudioFolderFor(const std::filesystem::path& projectFolder);
        static std::filesystem::path makeUniqueMediaPath(const std::filesystem::path& folder, const std::string& baseName, mw::core::AudioClipSavedFormat format);
    };
}
