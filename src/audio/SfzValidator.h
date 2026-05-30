#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace mw::audio
{
    struct SfzValidationResult
    {
        bool ok = false;
        int sampleReferences = 0;
        int missingSamples = 0;
        std::vector<std::filesystem::path> missingSamplePaths;
        std::string message;
    };

    class SfzValidator
    {
    public:
        static SfzValidationResult validateSampleReferences(const std::filesystem::path& sfzPath);
    };
}
