#pragma once

#include "core/Track.h"

#include <atomic>
#include <filesystem>
#include <string>

namespace mw::clap
{
    struct ClapInstrumentRenderRequest
    {
        mw::core::Track track;
        int tempoBpm = 120;
        int sampleRate = 48000;
        int channelCount = 2;
        int blockSize = 512;
        double tailSeconds = 4.0;
        std::filesystem::path wavOutputPath;
        std::atomic<bool>* cancelRequested = nullptr;
    };

    struct ClapInstrumentRenderResult
    {
        bool success = false;
        bool cancelled = false;
        std::string message;
        std::filesystem::path wavPath;
        int sampleRate = 0;
        int channelCount = 0;
        int processedBlocks = 0;
        int processStatus = -1;
        std::string processStatusText;
    };

    class ClapInstrumentHost
    {
    public:
        static ClapInstrumentRenderResult renderTrackToWav(const ClapInstrumentRenderRequest& request);
    };
}
