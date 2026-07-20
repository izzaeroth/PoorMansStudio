#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace mw::clap
{
    struct ClapLiveInstrumentSessionConfig
    {
        std::filesystem::path pluginPath;
        std::string pluginUid;
        std::string pluginName;
        std::string stateBase64;
        int sampleRate = 48000;
        int channelCount = 2;
        int blockSize = 512;
    };

    struct ClapLiveInstrumentSessionInfo
    {
        bool open = false;
        std::filesystem::path pluginPath;
        std::string pluginName;
        std::string pluginUid;
        int sampleRate = 0;
        int channelCount = 0;
        int blockSize = 0;
        int latencySamples = 0;
        bool stateRestored = false;
        bool audioPortsAvailable = false;
        bool startedProcessing = false;
        std::string noteDialectSummary;
        int notePortIndex = 0;
        int processedBlocks = 0;
        int lastProcessStatus = -1;
        std::string lastProcessStatusText;
        std::string message;
    };

    enum class ClapLiveNoteEventType
    {
        NoteOn,
        NoteOff
    };

    struct ClapLiveNoteEvent
    {
        ClapLiveNoteEventType type = ClapLiveNoteEventType::NoteOn;
        std::uint32_t frameOffset = 0;
        int key = 60;
        int velocity = 96;
        int midiChannel = 1;
    };

    struct ClapLiveProcessRequest
    {
        int frameCount = 0;
        std::vector<ClapLiveNoteEvent> noteEvents;
    };

    struct ClapLiveProcessResult
    {
        bool success = false;
        int processStatus = -1;
        std::string processStatusText;
        int eventsSubmitted = 0;
        int outputChannelCount = 0;
        int outputFrameCount = 0;
        std::vector<float> interleavedAudio;
        std::string message;
    };

    class ClapLiveInstrumentSession final
    {
    public:
        ClapLiveInstrumentSession();
        ~ClapLiveInstrumentSession();

        ClapLiveInstrumentSession(const ClapLiveInstrumentSession&) = delete;
        ClapLiveInstrumentSession& operator=(const ClapLiveInstrumentSession&) = delete;

        bool open(const ClapLiveInstrumentSessionConfig& config, std::string& errorMessage);
        void close();
        bool isOpen() const;
        bool prepareForPlayback(int sampleRate, int blockSize, int channelCount, std::string& errorMessage);
        ClapLiveInstrumentSessionInfo info() const;
        ClapLiveProcessResult processBlock(const ClapLiveProcessRequest& request);

    private:
        struct Impl;
        std::unique_ptr<Impl> impl;
    };
}
