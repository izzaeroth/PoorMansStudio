#include "audio/ExternalFfmpegEncoder.h"

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

    std::string quoteArg(const std::string& value)
    {
        std::string escaped;

        for (char c : value)
        {
            if (c == '"')
                escaped += "\\\"";
            else
                escaped += c;
        }

        return "\"" + escaped + "\"";
    }

    void appendMetadata(std::ostringstream& command, const std::string& key, const std::string& value)
    {
        if (!value.empty())
            command << " -metadata " << key << "=" << quoteArg(value);
    }

#ifdef _WIN32
    std::wstring quotePathW(const std::filesystem::path& path)
    {
        return L"\"" + path.wstring() + L"\"";
    }

    std::wstring toWide(const std::string& value)
    {
        return std::filesystem::path(value).wstring();
    }

    std::wstring quoteArgW(const std::string& value)
    {
        std::wstring wide = toWide(value);
        std::wstring escaped;

        for (wchar_t c : wide)
        {
            if (c == L'"')
                escaped += L"\\\"";
            else
                escaped += c;
        }

        return L"\"" + escaped + L"\"";
    }

    void appendMetadataW(std::wostringstream& command, const std::string& key, const std::string& value)
    {
        if (!value.empty())
            command << L" -metadata " << toWide(key) << L"=" << quoteArgW(value);
    }

    std::wstring buildWindowsCommandLine(const mw::audio::FfmpegEncodeRequest& request)
    {
        std::wostringstream command;

        command
            << quotePathW(request.ffmpegExePath)
            << L" -y"
            << L" -i "
            << quotePathW(request.inputWavPath);

        appendMetadataW(command, "title", request.metadataTitle);
        appendMetadataW(command, "artist", request.metadataArtist);
        appendMetadataW(command, "album", request.metadataAlbum);
        appendMetadataW(command, "track", request.metadataTrackNumber);
        appendMetadataW(command, "date", request.metadataYear);

        if (request.outputChannels == 1 || request.outputChannels == 2)
            command << L" -ac " << request.outputChannels;

        switch (request.format)
        {
            case mw::audio::EncodedAudioFormat::Flac:
                command << L" -c:a flac";
                break;

            case mw::audio::EncodedAudioFormat::Mp3:
                command << L" -c:a libmp3lame -b:a " << request.mp3BitrateKbps << L"k";
                break;

            case mw::audio::EncodedAudioFormat::Ogg:
                command << L" -c:a libvorbis -b:a " << request.mp3BitrateKbps << L"k";
                break;

            case mw::audio::EncodedAudioFormat::Wav:
            default:
                break;
        }

        command << L" " << quotePathW(request.outputPath);
        return command.str();
    }

    int runProcessWithTimeout(const mw::audio::FfmpegEncodeRequest& request, std::string& message)
    {
        auto commandLine = buildWindowsCommandLine(request);

        STARTUPINFOW startupInfo {};
        startupInfo.cb = sizeof(startupInfo);

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
            const auto error = GetLastError();
            message = "Failed to start FFmpeg process. Windows error code: " + std::to_string(error);
            return -1;
        }

        const DWORD timeoutMs =
            request.timeoutSeconds <= 0
                ? 300000
                : static_cast<DWORD>(request.timeoutSeconds) * 1000u;

        const DWORD waitResult = WaitForSingleObject(processInfo.hProcess, timeoutMs);

        DWORD exitCode = 1;

        if (waitResult == WAIT_TIMEOUT)
        {
            TerminateProcess(processInfo.hProcess, 1);
            message = "FFmpeg timed out and was terminated.";
            exitCode = 1;
        }
        else if (waitResult == WAIT_OBJECT_0)
        {
            GetExitCodeProcess(processInfo.hProcess, &exitCode);
        }
        else
        {
            message = "Failed while waiting for FFmpeg process.";
            exitCode = 1;
        }

        CloseHandle(processInfo.hThread);
        CloseHandle(processInfo.hProcess);

        return static_cast<int>(exitCode);
    }
#endif
}

namespace mw::audio
{
    bool ExternalFfmpegEncoder::validateRequest(const FfmpegEncodeRequest& request, std::string& errorMessage)
    {
        const bool wavPassthrough = request.format == EncodedAudioFormat::Wav
            && (request.outputChannels != 1 && request.outputChannels != 2 || request.outputChannels == 2)
            && request.inputWavPath == request.outputPath;

        if (wavPassthrough)
            return true;

        if (request.ffmpegExePath.empty())
        {
            errorMessage = "FFmpeg executable path is empty.";
            return false;
        }

        if (!std::filesystem::exists(request.ffmpegExePath))
        {
            errorMessage = "FFmpeg executable was not found: " + request.ffmpegExePath.string();
            return false;
        }

        if (request.inputWavPath.empty() || !std::filesystem::exists(request.inputWavPath))
        {
            errorMessage = "Input WAV file was not found: " + request.inputWavPath.string();
            return false;
        }

        if (request.outputPath.empty())
        {
            errorMessage = "Encoded output path is empty.";
            return false;
        }

        return true;
    }

    std::string ExternalFfmpegEncoder::buildCommandLine(const FfmpegEncodeRequest& request)
    {
        std::ostringstream command;

        command
            << quotePath(request.ffmpegExePath)
            << " -y"
            << " -i "
            << quotePath(request.inputWavPath);

        appendMetadata(command, "title", request.metadataTitle);
        appendMetadata(command, "artist", request.metadataArtist);
        appendMetadata(command, "album", request.metadataAlbum);
        appendMetadata(command, "track", request.metadataTrackNumber);
        appendMetadata(command, "date", request.metadataYear);

        if (request.outputChannels == 1 || request.outputChannels == 2)
            command << " -ac " << request.outputChannels;

        switch (request.format)
        {
            case EncodedAudioFormat::Flac:
                command << " -c:a flac";
                break;

            case EncodedAudioFormat::Mp3:
                command << " -c:a libmp3lame -b:a " << request.mp3BitrateKbps << "k";
                break;

            case EncodedAudioFormat::Ogg:
                command << " -c:a libvorbis -b:a " << request.mp3BitrateKbps << "k";
                break;

            case EncodedAudioFormat::Wav:
            default:
                break;
        }

        command << " " << quotePath(request.outputPath);
        return command.str();
    }

    FfmpegEncodeResult ExternalFfmpegEncoder::encodeFromWav(const FfmpegEncodeRequest& request)
    {
        FfmpegEncodeResult result;
        result.commandLine = buildCommandLine(request);

        const bool wavPassthrough = request.format == EncodedAudioFormat::Wav
            && (request.outputChannels != 1 && request.outputChannels != 2 || request.outputChannels == 2)
            && request.inputWavPath == request.outputPath;

        if (wavPassthrough)
        {
            result.success = true;
            result.exitCode = 0;
            result.message = "WAV selected; no FFmpeg conversion needed.";
            return result;
        }

        std::string errorMessage;
        if (!validateRequest(request, errorMessage))
        {
            result.success = false;
            result.message = errorMessage;
            return result;
        }

        if (std::filesystem::exists(request.outputPath) && !request.overwriteExistingFile)
        {
            result.success = false;
            result.message = "Encoded output already exists and overwrite is disabled.";
            return result;
        }

#ifdef _WIN32
        std::string processMessage;
        const int exitCode = runProcessWithTimeout(request, processMessage);
        result.exitCode = exitCode;
        result.success = (exitCode == 0);

        if (result.success)
            result.message = "FFmpeg conversion completed successfully.";
        else if (!processMessage.empty())
            result.message = processMessage;
        else
            result.message = "FFmpeg conversion failed with exit code: " + std::to_string(exitCode);

        return result;
#else
        const int exitCode = std::system(result.commandLine.c_str());
        result.exitCode = exitCode;
        result.success = (exitCode == 0);
        result.message = result.success ? "FFmpeg conversion completed successfully." : "FFmpeg conversion failed.";
        return result;
#endif
    }
}
