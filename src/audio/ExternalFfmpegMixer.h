#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace mw::audio
{
    struct FfmpegMixRequest
    {
        std::filesystem::path ffmpegExePath;
        std::vector<std::filesystem::path> inputWavPaths;
        std::filesystem::path outputWavPath;
        int timeoutSeconds = 600;
    };

    struct FfmpegMixResult
    {
        bool success = false;
        int exitCode = -1;
        std::string commandLine;
        std::string message;
    };

    class ExternalFfmpegMixer
    {
    public:
        static FfmpegMixResult mixWavFiles(const FfmpegMixRequest& request);
        static std::string buildCommandLine(const FfmpegMixRequest& request);
        static bool validateRequest(const FfmpegMixRequest& request, std::string& errorMessage);
    };
}
