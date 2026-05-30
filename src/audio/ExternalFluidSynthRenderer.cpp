#include "audio/ExternalFluidSynthRenderer.h"

#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <string>

#ifdef _WIN32
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#endif

namespace
{
    std::string quotePath(const std::filesystem::path& path)
    {
        return "\"" + path.string() + "\"";
    }

#ifdef _WIN32
    std::wstring quotePathW(const std::filesystem::path& path)
    {
        return L"\"" + path.wstring() + L"\"";
    }

    std::wstring buildWindowsCommandLine(
        const mw::audio::FluidSynthRenderRequest& request
    )
    {
        std::wostringstream command;

        command
            << quotePathW(request.fluidSynthExePath)
            << L" -ni"
            << L" -F "
            << quotePathW(request.wavOutputPath)
            << L" -r "
            << request.sampleRate
            << L" "
            << quotePathW(request.soundFontPath)
            << L" "
            << quotePathW(request.midiInputPath);

        return command.str();
    }

    int runProcessWithTimeout(
        const mw::audio::FluidSynthRenderRequest& request,
        std::string& message
    )
    {
        auto commandLine = buildWindowsCommandLine(request);

        STARTUPINFOW startupInfo {};
        startupInfo.cb = sizeof(startupInfo);

        PROCESS_INFORMATION processInfo {};

        // CreateProcessW requires a mutable command buffer.
        std::wstring mutableCommandLine = commandLine;

        const BOOL created = CreateProcessW(
            request.fluidSynthExePath.wstring().c_str(),
            mutableCommandLine.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW,
            nullptr,
            nullptr,
            &startupInfo,
            &processInfo
        );

        if (!created)
        {
            const auto error = GetLastError();
            message = "Failed to start FluidSynth process. Windows error code: " + std::to_string(error);
            return -1;
        }

        const DWORD timeoutMs =
            request.timeoutSeconds <= 0
                ? 300000
                : static_cast<DWORD>(request.timeoutSeconds) * 1000u;

        constexpr DWORD pollMs = 100;
        DWORD waitedMs = 0;
        DWORD waitResult = WAIT_TIMEOUT;
        DWORD exitCode = 1;

        while (true)
        {
            if (request.cancelRequested != nullptr && request.cancelRequested->load())
            {
                TerminateProcess(processInfo.hProcess, 1);
                message = "FluidSynth render cancelled and process was terminated.";
                exitCode = 1;
                break;
            }

            waitResult = WaitForSingleObject(processInfo.hProcess, pollMs);

            if (waitResult == WAIT_OBJECT_0)
            {
                GetExitCodeProcess(processInfo.hProcess, &exitCode);
                break;
            }

            if (waitResult != WAIT_TIMEOUT)
            {
                message = "Failed while waiting for FluidSynth process.";
                exitCode = 1;
                break;
            }

            waitedMs += pollMs;
            if (waitedMs >= timeoutMs)
            {
                TerminateProcess(processInfo.hProcess, 1);
                message = "FluidSynth timed out and was terminated.";
                exitCode = 1;
                break;
            }
        }

        CloseHandle(processInfo.hThread);
        CloseHandle(processInfo.hProcess);

        return static_cast<int>(exitCode);
    }
#endif
}

namespace mw::audio
{
    bool ExternalFluidSynthRenderer::validateRequest(
        const FluidSynthRenderRequest& request,
        std::string& errorMessage
    )
    {
        if (request.fluidSynthExePath.empty())
        {
            errorMessage = "FluidSynth executable path is empty.";
            return false;
        }

        if (!std::filesystem::exists(request.fluidSynthExePath))
        {
            errorMessage = "FluidSynth executable was not found: " + request.fluidSynthExePath.string();
            return false;
        }

        if (request.soundFontPath.empty())
        {
            errorMessage = "SoundFont path is empty.";
            return false;
        }

        if (!std::filesystem::exists(request.soundFontPath))
        {
            errorMessage = "SoundFont file was not found: " + request.soundFontPath.string();
            return false;
        }

        if (request.midiInputPath.empty())
        {
            errorMessage = "MIDI input path is empty.";
            return false;
        }

        if (!std::filesystem::exists(request.midiInputPath))
        {
            errorMessage = "MIDI input file was not found: " + request.midiInputPath.string();
            return false;
        }

        if (request.wavOutputPath.empty())
        {
            errorMessage = "WAV output path is empty.";
            return false;
        }

        return true;
    }

    std::string ExternalFluidSynthRenderer::buildCommandLine(const FluidSynthRenderRequest& request)
    {
        std::ostringstream command;

        // FluidSynth 2.5.x requires options before SoundFont/MIDI paths.
        command
            << quotePath(request.fluidSynthExePath)
            << " -ni"
            << " -F "
            << quotePath(request.wavOutputPath)
            << " -r "
            << request.sampleRate
            << " "
            << quotePath(request.soundFontPath)
            << " "
            << quotePath(request.midiInputPath);

        return command.str();
    }

    FluidSynthRenderResult ExternalFluidSynthRenderer::renderMidiToWav(
        const FluidSynthRenderRequest& request
    )
    {
        FluidSynthRenderResult result;
        result.commandLine = buildCommandLine(request);

        std::string errorMessage;
        if (!validateRequest(request, errorMessage))
        {
            result.success = false;
            result.message = errorMessage;
            return result;
        }

        if (std::filesystem::exists(request.wavOutputPath) && !request.overwriteExistingFile)
        {
            result.success = false;
            result.message = "Output WAV already exists and overwrite is disabled.";
            return result;
        }

#ifdef _WIN32
        std::string processMessage;
        const int exitCode = runProcessWithTimeout(request, processMessage);
        result.exitCode = exitCode;
        result.success = (exitCode == 0);

        if (result.success)
            result.message = "FluidSynth render completed successfully.";
        else if (!processMessage.empty())
            result.message = processMessage;
        else
            result.message = "FluidSynth render failed with exit code: " + std::to_string(exitCode);

        return result;
#else
        const int exitCode = std::system(result.commandLine.c_str());

        result.exitCode = exitCode;
        result.success = (exitCode == 0);

        if (result.success)
            result.message = "FluidSynth render completed successfully.";
        else
            result.message = "FluidSynth render failed. Check executable, SoundFont, MIDI file, and command output.";

        return result;
#endif
    }
}
