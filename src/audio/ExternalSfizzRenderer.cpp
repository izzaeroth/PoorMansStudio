#include "audio/ExternalSfizzRenderer.h"

#include <chrono>
#include <cstdlib>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

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

    std::filesystem::path buildLogPath(const std::filesystem::path& wavOutputPath)
    {
        auto logPath = wavOutputPath;
        logPath.replace_extension(".sfizz.log.txt");
        return logPath;
    }

    std::string readSmallTextFile(const std::filesystem::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file)
            return {};

        std::ostringstream buffer;
        buffer << file.rdbuf();

        auto text = buffer.str();

        constexpr std::size_t maxLength = 2500;
        if (text.size() > maxLength)
            text = text.substr(0, maxLength) + "\n... log truncated ...";

        return text;
    }

    std::string lower(std::string value)
    {
        for (auto& c : value)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        return value;
    }

    std::string trimPathToken(std::string value)
    {
        const auto first = value.find_first_not_of(" \t\r\n\"");
        if (first == std::string::npos)
            return {};

        const auto last = value.find_last_not_of(" \t\r\n\"");
        return value.substr(first, last - first + 1);
    }

    std::string stripComment(std::string line)
    {
        const auto pos = line.find("//");
        if (pos != std::string::npos)
            line = line.substr(0, pos);

        return line;
    }

    std::string extractValueAfterKey(const std::string& line, const std::string& key)
    {
        const auto lowerLine = lower(line);
        const auto lowerKey = lower(key);

        auto pos = lowerLine.find(lowerKey);
        if (pos == std::string::npos)
            return {};

        pos += key.size();

        while (pos < line.size() && std::isspace(static_cast<unsigned char>(line[pos])))
            ++pos;

        if (pos >= line.size())
            return {};

        if (line[pos] == '\"')
        {
            const auto endQuote = line.find('\"', pos + 1);
            if (endQuote != std::string::npos)
                return trimPathToken(line.substr(pos + 1, endQuote - pos - 1));
        }

        auto end = line.find_first_of(" \t\r\n", pos);
        if (end == std::string::npos)
            end = line.size();

        return trimPathToken(line.substr(pos, end - pos));
    }

    std::filesystem::path sidecarPackFolderFor(const std::filesystem::path& sfzPath)
    {
        if (sfzPath.empty())
            return {};

        const auto candidate = sfzPath.parent_path() / sfzPath.stem();

        std::error_code ec;
        if (std::filesystem::exists(candidate, ec) && std::filesystem::is_directory(candidate, ec))
            return candidate;

        return {};
    }

    bool shouldRenderFromSidecarFolder(
        const std::filesystem::path& sfzPath,
        const std::filesystem::path& sidecarFolder
    )
    {
        if (sfzPath.empty() || sidecarFolder.empty())
            return false;

        std::ifstream file(sfzPath);
        if (!file)
            return false;

        const auto baseFolder = sfzPath.parent_path();
        int baseHits = 0;
        int sidecarHits = 0;

        std::string line;
        while (std::getline(file, line))
        {
            line = stripComment(line);

            const std::string pathValues[]
            {
                extractValueAfterKey(line, "sample="),
                extractValueAfterKey(line, "#include")
            };

            for (const auto& pathValue : pathValues)
            {
                if (pathValue.empty())
                    continue;

                const auto requestedPath = std::filesystem::path(pathValue);
                if (requestedPath.is_absolute())
                {
                    std::error_code ec;
                    if (std::filesystem::exists(requestedPath, ec))
                        ++baseHits;
                    continue;
                }

                std::error_code ec;
                if (std::filesystem::exists((baseFolder / requestedPath).lexically_normal(), ec))
                    ++baseHits;
                else if (std::filesystem::exists((sidecarFolder / requestedPath).lexically_normal(), ec))
                    ++sidecarHits;
            }
        }

        return sidecarHits > 0 && baseHits == 0;
    }

    bool prepareSidecarEntrySfz(
        const std::filesystem::path& originalSfzPath,
        const std::filesystem::path& wavOutputPath,
        std::filesystem::path& renderSfzPath,
        std::filesystem::path& temporarySfzPath,
        std::string& message
    )
    {
        renderSfzPath = originalSfzPath;
        temporarySfzPath.clear();
        message.clear();

        const auto sidecarFolder = sidecarPackFolderFor(originalSfzPath);
        if (sidecarFolder.empty() || !shouldRenderFromSidecarFolder(originalSfzPath, sidecarFolder))
            return false;

        const auto uniqueTick = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto safeStem = wavOutputPath.empty() ? originalSfzPath.stem().string() : wavOutputPath.stem().string();
        temporarySfzPath = sidecarFolder / (".__pms_sfizz_entry_" + safeStem + "_" + std::to_string(uniqueTick) + ".sfz");

        std::error_code ec;
        std::filesystem::copy_file(originalSfzPath, temporarySfzPath, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec)
        {
            temporarySfzPath.clear();
            message = "SFZ sidecar pack root was detected, but the temporary render entry could not be created: " + ec.message();
            return false;
        }

        renderSfzPath = temporarySfzPath;
        message = "SFZ sidecar pack root detected: " + sidecarFolder.string();
        return true;
    }

    void removeTemporarySfz(const std::filesystem::path& temporarySfzPath)
    {
        if (temporarySfzPath.empty())
            return;

        std::error_code ignored;
        std::filesystem::remove(temporarySfzPath, ignored);
    }

#ifdef _WIN32
    std::wstring quotePathW(const std::filesystem::path& path)
    {
        return L"\"" + path.wstring() + L"\"";
    }

    std::wstring buildWindowsCommandLine(const mw::audio::SfizzRenderRequest& request)
    {
        std::wostringstream command;
        command
            << quotePathW(request.sfizzRenderExePath)
            << L" --wav " << quotePathW(request.wavOutputPath)
            << L" --sfz " << quotePathW(request.sfzPath)
            << L" --midi " << quotePathW(request.midiInputPath);

        return command.str();
    }

    int runProcessWithTimeout(
        const mw::audio::SfizzRenderRequest& request,
        const std::filesystem::path& logPath,
        std::string& message
    )
    {
        auto commandLine = buildWindowsCommandLine(request);

        SECURITY_ATTRIBUTES securityAttributes {};
        securityAttributes.nLength = sizeof(securityAttributes);
        securityAttributes.bInheritHandle = TRUE;
        securityAttributes.lpSecurityDescriptor = nullptr;

        HANDLE logHandle = CreateFileW(
            logPath.wstring().c_str(),
            GENERIC_WRITE,
            FILE_SHARE_READ,
            &securityAttributes,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        );

        if (logHandle == INVALID_HANDLE_VALUE)
            logHandle = nullptr;

        STARTUPINFOW startupInfo {};
        startupInfo.cb = sizeof(startupInfo);

        if (logHandle != nullptr)
        {
            startupInfo.dwFlags |= STARTF_USESTDHANDLES;
            startupInfo.hStdOutput = logHandle;
            startupInfo.hStdError = logHandle;
            startupInfo.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        }

        PROCESS_INFORMATION processInfo {};
        std::wstring mutableCommandLine = commandLine;

        const BOOL created = CreateProcessW(
            request.sfizzRenderExePath.wstring().c_str(),
            mutableCommandLine.data(),
            nullptr,
            nullptr,
            logHandle != nullptr ? TRUE : FALSE,
            CREATE_NO_WINDOW,
            nullptr,
            nullptr,
            &startupInfo,
            &processInfo
        );

        if (!created)
        {
            message = "Failed to start sfizz-render. Windows error code: " + std::to_string(GetLastError());

            if (logHandle != nullptr)
                CloseHandle(logHandle);

            return -1;
        }

        const DWORD timeoutMs = request.timeoutSeconds <= 0 ? 300000 : static_cast<DWORD>(request.timeoutSeconds) * 1000u;
        constexpr DWORD pollMs = 100;
        DWORD waitedMs = 0;
        DWORD exitCode = 1;

        while (true)
        {
            if (request.cancelRequested != nullptr && request.cancelRequested->load())
            {
                TerminateProcess(processInfo.hProcess, 1);
                message = "sfizz-render cancelled and process was terminated.";
                exitCode = 1;
                break;
            }

            const DWORD waitResult = WaitForSingleObject(processInfo.hProcess, pollMs);
            if (waitResult == WAIT_OBJECT_0)
            {
                GetExitCodeProcess(processInfo.hProcess, &exitCode);
                break;
            }

            if (waitResult != WAIT_TIMEOUT)
            {
                message = "Failed while waiting for sfizz-render.";
                exitCode = 1;
                break;
            }

            waitedMs += pollMs;
            if (waitedMs >= timeoutMs)
            {
                TerminateProcess(processInfo.hProcess, 1);
                message = "sfizz-render timed out and was terminated.";
                exitCode = 1;
                break;
            }
        }

        CloseHandle(processInfo.hThread);
        CloseHandle(processInfo.hProcess);

        if (logHandle != nullptr)
            CloseHandle(logHandle);

        return static_cast<int>(exitCode);
    }
#endif
}

namespace mw::audio
{
    bool ExternalSfizzRenderer::validateRequest(const SfizzRenderRequest& request, std::string& errorMessage)
    {
        if (request.sfizzRenderExePath.empty() || !std::filesystem::exists(request.sfizzRenderExePath))
        {
            errorMessage = "sfizz-render executable was not found: " + request.sfizzRenderExePath.string();
            return false;
        }

        if (request.sfzPath.empty() || !std::filesystem::exists(request.sfzPath))
        {
            errorMessage = "SFZ file was not found: " + request.sfzPath.string();
            return false;
        }

        if (request.midiInputPath.empty() || !std::filesystem::exists(request.midiInputPath))
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

    std::string ExternalSfizzRenderer::buildCommandLine(const SfizzRenderRequest& request)
    {
        std::ostringstream command;
        command
            << quotePath(request.sfizzRenderExePath)
            << " --wav " << quotePath(request.wavOutputPath)
            << " --sfz " << quotePath(request.sfzPath)
            << " --midi " << quotePath(request.midiInputPath);

        return command.str();
    }

    SfizzRenderResult ExternalSfizzRenderer::renderMidiToWav(const SfizzRenderRequest& request)
    {
        SfizzRenderResult result;

        const auto logPath = buildLogPath(request.wavOutputPath);

        std::string errorMessage;
        if (!validateRequest(request, errorMessage))
        {
            result.commandLine = buildCommandLine(request);
            result.message = errorMessage;
            return result;
        }

        auto renderRequest = request;
        std::filesystem::path temporarySfzPath;
        std::string sidecarMessage;
        prepareSidecarEntrySfz(request.sfzPath, request.wavOutputPath, renderRequest.sfzPath, temporarySfzPath, sidecarMessage);

        result.commandLine = buildCommandLine(renderRequest);

#ifdef _WIN32
        std::string processMessage;
        result.exitCode = runProcessWithTimeout(renderRequest, logPath, processMessage);
        result.success = result.exitCode == 0;

        const auto logText = readSmallTextFile(logPath);

        if (result.success)
        {
            result.message = "sfizz-render completed successfully. Log: " + logPath.string();
        }
        else
        {
            result.message =
                (!processMessage.empty() ? processMessage : "sfizz-render failed.")
                + " Log: "
                + logPath.string();

            if (!logText.empty())
                result.message += "\n" + logText;
        }

        if (!sidecarMessage.empty())
            result.message += "\n" + sidecarMessage;

        removeTemporarySfz(temporarySfzPath);
        return result;
#else
        const int exitCode = std::system(result.commandLine.c_str());
        result.exitCode = exitCode;
        result.success = exitCode == 0;
        result.message = result.success ? "sfizz-render completed successfully." : "sfizz-render failed.";

        if (!sidecarMessage.empty())
            result.message += "\n" + sidecarMessage;

        removeTemporarySfz(temporarySfzPath);
        return result;
#endif
    }
}
