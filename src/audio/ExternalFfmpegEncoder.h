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
        Ogg
    };

    inline std::string encodedAudioFormatToExtension(EncodedAudioFormat format)
    {
        switch (format)
        {
            case EncodedAudioFormat::Flac: return ".flac";
            case EncodedAudioFormat::Mp3: return ".mp3";
            case EncodedAudioFormat::Ogg: return ".ogg";
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

    class ExternalFfmpegEncoder
    {
    public:
        static FfmpegEncodeResult encodeFromWav(const FfmpegEncodeRequest& request);
        static std::string buildCommandLine(const FfmpegEncodeRequest& request);
        static bool validateRequest(const FfmpegEncodeRequest& request, std::string& errorMessage);
    };
}
