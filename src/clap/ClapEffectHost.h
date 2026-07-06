#pragma once

#include "core/InstrumentAssignment.h"

#include <atomic>
#include <filesystem>
#include <string>

namespace mw::clap
{
    struct ClapEffectProcessRequest
    {
        mw::core::VstPluginAssignment plugin;
        std::filesystem::path inputWavPath;
        std::filesystem::path outputWavPath;
        int blockSize = 512;
        double tailSeconds = 2.0;
        std::atomic<bool>* cancelRequested = nullptr;
    };

    struct ClapEffectProcessResult
    {
        bool success = false;
        bool cancelled = false;
        bool effectApplied = false;
        std::string message;
        std::filesystem::path wavPath;
        int sampleRate = 0;
        int channelCount = 0;
        int processedBlocks = 0;
        int processStatus = -1;
        std::string processStatusText;
    };

    class ClapEffectHost
    {
    public:
        static ClapEffectProcessResult processWavWithPlugin(const ClapEffectProcessRequest& request);
    };
}
