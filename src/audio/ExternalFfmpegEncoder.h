#pragma once

#include <filesystem>
#include <string>

namespace mw::audio
{
    enum class EncodedAudioFormat
    {
        Wav,
        Flac,
        Mp3,
        Ogg,
        M4a
    };

    inline std::string encodedAudioFormatToExtension(EncodedAudioFormat format)
    {
        switch (format)
        {
            case EncodedAudioFormat::Flac: return ".flac";
            case EncodedAudioFormat::Mp3: return ".mp3";
            case EncodedAudioFormat::Ogg: return ".ogg";
            case EncodedAudioFormat::M4a: return ".m4a";
            case EncodedAudioFormat::Wav:
            default: return ".wav";
        }
    }

    struct FfmpegEncodeRequest
    {
        std::filesystem::path ffmpegExePath;
        std::filesystem::path inputWavPath;
        std::filesystem::path outputPath;
        EncodedAudioFormat format = EncodedAudioFormat::Wav;
        int mp3BitrateKbps = 192;
        int outputChannels = 2;
        bool overwriteExistingFile = true;
        int timeoutSeconds = 300;

        std::string metadataTitle;
        std::string metadataArtist;
        std::string metadataAlbum;
        std::string metadataTrackNumber;
        std::string metadataYear;
    };

    struct FfmpegEncodeResult
    {
        bool success = false;
        int exitCode = -1;
        std::string commandLine;
        std::string message;
    };

    struct FfmpegTrimRequest
    {
        std::filesystem::path ffmpegExePath;
        std::filesystem::path inputPath;
        std::filesystem::path outputWavPath;
        double trimStartSeconds = 0.0;
        double trimDurationSeconds = 0.0;
        int outputChannels = 2;
        int outputSampleRate = 0;
        bool mciCompatibleWav = false;
        int timeoutSeconds = 300;
    };

    struct FfmpegTrimResult
    {
        bool success = false;
        int exitCode = -1;
        std::string commandLine;
        std::string message;
    };

    class ExternalFfmpegEncoder
    {
    public:
        static FfmpegEncodeResult encodeFromWav(const FfmpegEncodeRequest& request);
        static std::string buildCommandLine(const FfmpegEncodeRequest& request);
        static bool validateRequest(const FfmpegEncodeRequest& request, std::string& errorMessage);

        static FfmpegTrimResult trimToWav(const FfmpegTrimRequest& request);
        static std::string buildTrimCommandLine(const FfmpegTrimRequest& request);
        static bool validateTrimRequest(const FfmpegTrimRequest& request, std::string& errorMessage);
    };
}
