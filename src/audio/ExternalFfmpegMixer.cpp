#include "audio/ExternalFfmpegMixer.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <string>
#include <iomanip>
#include <cmath>

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

    double inputStartOffsetSecondsAt(const mw::audio::FfmpegMixRequest& request, std::size_t index)
    {
        if (index >= request.inputStartOffsetsSeconds.size())
            return 0.0;

        const auto seconds = request.inputStartOffsetsSeconds[index];
        return std::isfinite(seconds) ? std::max(0.0, seconds) : 0.0;
    }

    bool hasAnyInputOffset(const mw::audio::FfmpegMixRequest& request)
    {
        for (std::size_t i = 0; i < request.inputWavPaths.size(); ++i)
        {
            if (inputStartOffsetSecondsAt(request, i) > 0.0005)
                return true;
        }

        return false;
    }

    double inputGainAt(const mw::audio::FfmpegMixRequest& request, std::size_t index)
    {
        if (index >= request.inputGains.size())
            return 1.0;

        const auto gain = request.inputGains[index];
        return std::isfinite(gain) ? std::clamp(gain, 0.0, 8.0) : 1.0;
    }

    bool hasAnyInputGain(const mw::audio::FfmpegMixRequest& request)
    {
        for (std::size_t i = 0; i < request.inputWavPaths.size(); ++i)
        {
            if (std::abs(inputGainAt(request, i) - 1.0) > 0.0005)
                return true;
        }

        return false;
    }

    std::string formatGain(double gain)
    {
        std::ostringstream text;
        text << std::fixed << std::setprecision(6) << std::clamp(gain, 0.0, 8.0);
        return text.str();
    }

    long long inputStartOffsetMillisecondsAt(const mw::audio::FfmpegMixRequest& request, std::size_t index)
    {
        return static_cast<long long>(std::llround(inputStartOffsetSecondsAt(request, index) * 1000.0));
    }

    std::string buildFilterComplex(const mw::audio::FfmpegMixRequest& request)
    {
        const auto inputCount = request.inputWavPaths.size();

        if (!hasAnyInputOffset(request) && !hasAnyInputGain(request))
            return "amix=inputs=" + std::to_string(inputCount) + ":duration=longest:normalize=0";

        std::ostringstream filter;
        for (std::size_t i = 0; i < inputCount; ++i)
        {
            const auto delayMs = inputStartOffsetMillisecondsAt(request, i);
            const auto gain = inputGainAt(request, i);

            filter << "[" << i << ":a]";

            bool wroteFilter = false;
            if (std::abs(gain - 1.0) > 0.0005)
            {
                filter << "volume=" << formatGain(gain);
                wroteFilter = true;
            }

            if (delayMs > 0)
            {
                if (wroteFilter)
                    filter << ",";
                filter << "adelay=" << delayMs << ":all=1";
                wroteFilter = true;
            }

            if (!wroteFilter)
                filter << "anull";

            filter << "[a" << i << "];";
        }

        for (std::size_t i = 0; i < inputCount; ++i)
            filter << "[a" << i << "]";

        filter << "amix=inputs=" << inputCount << ":duration=longest:normalize=0";
        return filter.str();
    }

#ifdef _WIN32
    std::wstring quotePathW(const std::filesystem::path& path)
    {
        return L"\"" + path.wstring() + L"\"";
    }

    std::wstring buildWindowsCommandLine(const mw::audio::FfmpegMixRequest& request)
    {
        std::wostringstream command;
        command << quotePathW(request.ffmpegExePath) << L" -y ";

        for (const auto& input : request.inputWavPaths)
            command << L"-i " << quotePathW(input) << L" ";

        const auto filter = buildFilterComplex(request);
        command
            << L"-filter_complex \""
            << std::wstring(filter.begin(), filter.end())
            << L"\" "
            << L"-c:a pcm_s16le "
            << quotePathW(request.outputWavPath);

        return command.str();
    }

    int runProcessWithTimeout(const mw::audio::FfmpegMixRequest& request, std::string& message)
    {
        auto commandLine = buildWindowsCommandLine(request);

        STARTUPINFOW startupInfo {};
        startupInfo.cb = sizeof(startupInfo);
        startupInfo.dwFlags = STARTF_USESHOWWINDOW;
        startupInfo.wShowWindow = SW_HIDE;

        PROCESS_INFORMATION processInfo {};
        std::wstring mutableCommandLine = commandLine;

        const BOOL created = CreateProcessW(
            request.ffmpegExePath.wstring().c_str(),
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
            message = "Failed to start FFmpeg mixer. Windows error code: " + std::to_string(GetLastError());
            return -1;
        }

        const DWORD timeoutMs = request.timeoutSeconds <= 0 ? 600000 : static_cast<DWORD>(request.timeoutSeconds) * 1000u;
        const DWORD waitResult = WaitForSingleObject(processInfo.hProcess, timeoutMs);

        DWORD exitCode = 1;
        if (waitResult == WAIT_TIMEOUT)
        {
            TerminateProcess(processInfo.hProcess, 1);
            message = "FFmpeg mix timed out and was terminated.";
        }
        else if (waitResult == WAIT_OBJECT_0)
        {
            GetExitCodeProcess(processInfo.hProcess, &exitCode);
        }
        else
        {
            message = "Failed while waiting for FFmpeg mixer.";
        }

        CloseHandle(processInfo.hThread);
        CloseHandle(processInfo.hProcess);
        return static_cast<int>(exitCode);
    }
#endif
}

namespace mw::audio
{
    bool ExternalFfmpegMixer::validateRequest(const FfmpegMixRequest& request, std::string& errorMessage)
    {
        if (request.ffmpegExePath.empty() || !std::filesystem::exists(request.ffmpegExePath))
        {
            errorMessage = "FFmpeg executable was not found: " + request.ffmpegExePath.string();
            return false;
        }

        if (request.inputWavPaths.empty())
        {
            errorMessage = "No WAV files were provided for mixing.";
            return false;
        }

        for (const auto& input : request.inputWavPaths)
        {
            if (input.empty() || !std::filesystem::exists(input))
            {
                errorMessage = "Mix input WAV file was not found: " + input.string();
                return false;
            }
        }

        if (request.outputWavPath.empty())
        {
            errorMessage = "Mix output WAV path is empty.";
            return false;
        }

        return true;
    }

    std::string ExternalFfmpegMixer::buildCommandLine(const FfmpegMixRequest& request)
    {
        std::ostringstream command;
        command << quotePath(request.ffmpegExePath) << " -y ";

        for (const auto& input : request.inputWavPaths)
            command << "-i " << quotePath(input) << " ";

        command
            << "-filter_complex \""
            << buildFilterComplex(request)
            << "\" "
            << "-c:a pcm_s16le "
            << quotePath(request.outputWavPath);

        return command.str();
    }

    FfmpegMixResult ExternalFfmpegMixer::mixWavFiles(const FfmpegMixRequest& request)
    {
        FfmpegMixResult result;
        result.commandLine = buildCommandLine(request);

        std::string errorMessage;
        if (!validateRequest(request, errorMessage))
        {
            result.message = errorMessage;
            return result;
        }

#ifdef _WIN32
        std::string processMessage;
        result.exitCode = runProcessWithTimeout(request, processMessage);
        result.success = result.exitCode == 0;
        result.message = result.success
            ? "FFmpeg stem mix completed successfully."
            : (!processMessage.empty() ? processMessage : "FFmpeg stem mix failed.");
#else
        result.exitCode = std::system(result.commandLine.c_str());
        result.success = result.exitCode == 0;
        result.message = result.success ? "FFmpeg stem mix completed successfully." : "FFmpeg stem mix failed.";
#endif

        return result;
    }
}
