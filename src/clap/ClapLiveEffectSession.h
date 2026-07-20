#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace mw::clap
{
    struct ClapLiveEffectSessionConfig
    {
        std::filesystem::path pluginPath;
        std::string pluginUid;
        std::string pluginName;
        std::string stateBase64;
        int sampleRate = 48000;
        int channelCount = 2;
        int blockSize = 512;
    };

    struct ClapLiveEffectSessionInfo
    {
        bool open = false;
        std::filesystem::path pluginPath;
        std::string pluginName;
        std::string pluginUid;
        int sampleRate = 0;
        int inputChannelCount = 0;
        int outputChannelCount = 0;
        int blockSize = 0;
        int latencySamples = 0;
        bool stateRestored = false;
        bool audioPortsAvailable = false;
        bool startedProcessing = false;
        int processedBlocks = 0;
        int lastProcessStatus = -1;
        std::string lastProcessStatusText;
        std::string message;
    };

    struct ClapLiveEffectProcessRequest
    {
        int frameCount = 0;
        int inputChannelCount = 0;
        std::vector<float> interleavedInputAudio;
    };


    struct ClapLiveEffectRealtimeProcessResult
    {
        bool success = false;
        int processStatus = -1;
        int outputChannelCount = 0;
    };

    struct ClapLiveEffectProcessResult
    {
        bool success = false;
        int processStatus = -1;
        std::string processStatusText;
        int inputChannelCount = 0;
        int outputChannelCount = 0;
        int outputFrameCount = 0;
        std::vector<float> interleavedAudio;
        std::string message;
    };

    class ClapLiveEffectSession final
    {
    public:
        ClapLiveEffectSession();
        ~ClapLiveEffectSession();

        ClapLiveEffectSession(const ClapLiveEffectSession&) = delete;
        ClapLiveEffectSession& operator=(const ClapLiveEffectSession&) = delete;

        bool open(const ClapLiveEffectSessionConfig& config, std::string& errorMessage);
        void close();
        bool isOpen() const;
        bool prepareForPlayback(int sampleRate, int blockSize, int channelCount, std::string& errorMessage);
        ClapLiveEffectSessionInfo info() const;
        ClapLiveEffectProcessResult processBlock(const ClapLiveEffectProcessRequest& request);
        ClapLiveEffectRealtimeProcessResult processPlanarBlock(const float* const* inputChannelData,
                                                               int inputChannelCount,
                                                               float* const* outputChannelData,
                                                               int outputChannelCount,
                                                               int frameCount) noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl;
    };
}
