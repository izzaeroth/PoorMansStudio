#pragma once
#include <filesystem>
#include <string>
#include <atomic>

namespace mw::audio
{
    struct SfizzRenderRequest
    {
        std::filesystem::path sfizzRenderExePath;
        std::filesystem::path sfzPath;
        std::filesystem::path midiInputPath;
        std::filesystem::path wavOutputPath;
        int sampleRate = 48000;
        int timeoutSeconds = 300;
        std::atomic<bool>* cancelRequested = nullptr;
        bool overwriteExistingFile = true;
    };

    struct SfizzRenderResult
    {
        bool success = false;
        int exitCode = -1;
        std::string commandLine;
        std::string message;
    };

    class ExternalSfizzRenderer
    {
    public:
        static SfizzRenderResult renderMidiToWav(const SfizzRenderRequest& request);
        static std::string buildCommandLine(const SfizzRenderRequest& request);
        static bool validateRequest(const SfizzRenderRequest& request, std::string& errorMessage);
    };
}
