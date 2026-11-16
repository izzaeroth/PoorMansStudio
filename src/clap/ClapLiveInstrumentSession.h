#pragma once

#include <cstddef>
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

    struct ClapLiveControllerEvent
    {
        std::uint32_t frameOffset = 0;
        int midiChannel = 1;
        int controller = 0;
        int value = 0;
    };

    struct ClapLivePitchBendEvent
    {
        std::uint32_t frameOffset = 0;
        int midiChannel = 1;
        int value = 0; // signed project representation: -8192..+8191
    };

    struct ClapLiveProgramChangeEvent
    {
        std::uint32_t frameOffset = 0;
        int midiChannel = 1;
        int program = 0;
    };

    struct ClapLiveChannelPressureEvent
    {
        std::uint32_t frameOffset = 0;
        int midiChannel = 1;
        int pressure = 0;
    };

    struct ClapLivePolyPressureEvent
    {
        std::uint32_t frameOffset = 0;
        int midiChannel = 1;
        int noteNumber = 60;
        int pressure = 0;
    };

    struct ClapLiveProcessRequest
    {
        int frameCount = 0;
        std::vector<ClapLiveNoteEvent> noteEvents;
        std::vector<ClapLiveControllerEvent> controllerEvents;
        std::vector<ClapLivePitchBendEvent> pitchBendEvents;
        std::vector<ClapLiveProgramChangeEvent> programChangeEvents;
        std::vector<ClapLiveChannelPressureEvent> channelPressureEvents;
        std::vector<ClapLivePolyPressureEvent> polyPressureEvents;
    };

    struct ClapLiveRealtimeProcessResult
    {
        bool success = false;
        int processStatus = -1;
        int eventsSubmitted = 0;
        int outputChannelCount = 0;
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
        // Caller must own the session's processing mutex. Scratch/event storage is
        // reserved on the control thread; processing never resizes it.
        void prepareRealtimeStorage(std::size_t maximumEvents);
        ClapLiveRealtimeProcessResult processPlanarBlock(const ClapLiveProcessRequest& request,
                                                        float* const* outputChannelData,
                                                        int outputChannelCount) noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl;
    };
}
