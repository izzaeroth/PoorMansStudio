#pragma once

#include <filesystem>
#include <string>
#include <atomic>

namespace mw::audio
{
    struct FluidSynthRenderRequest
    {
        std::filesystem::path fluidSynthExePath;
        std::filesystem::path soundFontPath;
        std::filesystem::path midiInputPath;
        std::filesystem::path wavOutputPath;

        int sampleRate = 48000;
        bool overwriteExistingFile = true;
        int timeoutSeconds = 300;
        std::atomic<bool>* cancelRequested = nullptr;
    };

    struct FluidSynthRenderResult
    {
        bool success = false;
        int exitCode = -1;
        std::string commandLine;
        std::string message;
    };

    class ExternalFluidSynthRenderer
    {
    public:
        static FluidSynthRenderResult renderMidiToWav(const FluidSynthRenderRequest& request);

        static std::string buildCommandLine(const FluidSynthRenderRequest& request);
        static bool validateRequest(const FluidSynthRenderRequest& request, std::string& errorMessage);
    };
}
