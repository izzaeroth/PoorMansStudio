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
        // Optional per-input timeline offsets in seconds.  Empty means all inputs
        // start at 0.0, matching the original mixer behavior.
        std::vector<double> inputStartOffsetsSeconds;
        // Optional per-input linear gain values. Empty or missing entries mean 1.0.
        // Used by AudioClip render paths so track/master volume affects clips.
        std::vector<double> inputGains;
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
