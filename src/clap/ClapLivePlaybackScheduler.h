#pragma once

#include "clap/ClapLiveInstrumentSession.h"
#include "core/StableId.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mw::clap
{
    struct ClapLivePlaybackNote
    {
        int pitch = 60;
        int velocity = 100;
        int midiChannel = 1;
        std::int64_t startTick = 0;
        std::int64_t durationTicks = 960;
    };

    struct ClapLivePlaybackControllerEvent
    {
        mw::core::StableId stableId = 0;
        std::int64_t tick = 0;
        int midiChannel = 1;
        int controller = 0;
        int value = 0;
        int sequenceOrder = 0;
    };

    struct ClapLivePlayback14BitControllerEvent
    {
        mw::core::StableId stableId = 0;
        std::int64_t tick = 0;
        int midiChannel = 1;
        int controllerMsb = 1;
        int value = 0;
    };

    struct ClapLivePlaybackRpnEvent
    {
        mw::core::StableId stableId = 0;
        std::int64_t tick = 0;
        int midiChannel = 1;
        int parameter = 0;
        int value = 0;
    };

    struct ClapLivePlaybackNrpnEvent
    {
        mw::core::StableId stableId = 0;
        std::int64_t tick = 0;
        int midiChannel = 1;
        int parameter = 0;
        int value = 0;
    };

    struct ClapLivePlaybackPitchBendEvent
    {
        mw::core::StableId stableId = 0;
        std::int64_t tick = 0;
        int midiChannel = 1;
        int value = 0; // -8192..+8191
    };

    struct ClapLivePlaybackProgramChangeEvent
    {
        mw::core::StableId stableId = 0;
        std::int64_t tick = 0;
        int midiChannel = 1;
        int program = 0;
    };

    struct ClapLivePlaybackChannelPressureEvent
    {
        mw::core::StableId stableId = 0;
        std::int64_t tick = 0;
        int midiChannel = 1;
        int pressure = 0;
    };

    struct ClapLivePlaybackPolyPressureEvent
    {
        mw::core::StableId stableId = 0;
        std::int64_t tick = 0;
        int midiChannel = 1;
        int noteNumber = 60;
        int pressure = 0;
    };

    struct ClapLivePlaybackSchedulerConfig
    {
        int tempoBpm = 120;
        int sampleRate = 48000;
        int blockSize = 512;
        std::int64_t ticksPerQuarterNote = 960;
        std::int64_t startTick = 0;
        std::int64_t startSample = 0;
        double tailSeconds = 1.0;
    };

    class ClapLivePlaybackScheduler final
    {
    public:
        ClapLivePlaybackScheduler() = default;
        void prepare(std::vector<ClapLivePlaybackNote> notes, ClapLivePlaybackSchedulerConfig config);
        void prepare(std::vector<ClapLivePlaybackNote> notes, std::vector<ClapLivePlaybackControllerEvent> controllerEvents, ClapLivePlaybackSchedulerConfig config);
        void prepare(std::vector<ClapLivePlaybackNote> notes, std::vector<ClapLivePlaybackControllerEvent> controllerEvents,
                     std::vector<ClapLivePlaybackPitchBendEvent> pitchBendEvents, ClapLivePlaybackSchedulerConfig config);
        void prepare(std::vector<ClapLivePlaybackNote> notes, std::vector<ClapLivePlaybackControllerEvent> controllerEvents,
                     std::vector<ClapLivePlaybackPitchBendEvent> pitchBendEvents,
                     std::vector<ClapLivePlaybackProgramChangeEvent> programChangeEvents, ClapLivePlaybackSchedulerConfig config);
        void prepare(std::vector<ClapLivePlaybackNote> notes, std::vector<ClapLivePlaybackControllerEvent> controllerEvents,
                     std::vector<ClapLivePlaybackPitchBendEvent> pitchBendEvents,
                     std::vector<ClapLivePlaybackProgramChangeEvent> programChangeEvents,
                     std::vector<ClapLivePlaybackChannelPressureEvent> channelPressureEvents, ClapLivePlaybackSchedulerConfig config);
        void prepare(std::vector<ClapLivePlaybackNote> notes, std::vector<ClapLivePlaybackControllerEvent> controllerEvents,
                     std::vector<ClapLivePlaybackPitchBendEvent> pitchBendEvents,
                     std::vector<ClapLivePlaybackProgramChangeEvent> programChangeEvents,
                     std::vector<ClapLivePlaybackChannelPressureEvent> channelPressureEvents,
                     std::vector<ClapLivePlaybackPolyPressureEvent> polyPressureEvents, ClapLivePlaybackSchedulerConfig config);
        void prepare(std::vector<ClapLivePlaybackNote> notes, std::vector<ClapLivePlaybackControllerEvent> controllerEvents,
                     std::vector<ClapLivePlaybackPitchBendEvent> pitchBendEvents,
                     std::vector<ClapLivePlaybackProgramChangeEvent> programChangeEvents,
                     std::vector<ClapLivePlaybackChannelPressureEvent> channelPressureEvents,
                     std::vector<ClapLivePlaybackPolyPressureEvent> polyPressureEvents,
                     std::vector<ClapLivePlayback14BitControllerEvent> controller14BitEvents,
                     std::vector<ClapLivePlaybackRpnEvent> rpnEvents,
                     std::vector<ClapLivePlaybackNrpnEvent> nrpnEvents, ClapLivePlaybackSchedulerConfig config);
        bool isFinished() const;
        std::int64_t currentSample() const;
        std::int64_t totalSamples() const;
        int blockSize() const;
        int totalScheduledEvents() const;
        int maxEventsPerBlock() const;
        void nextBlockInto(ClapLiveProcessRequest& request);

    private:
        enum class ScheduledEventType { Controller, ProgramChange, PitchBend, ChannelPressure, NoteOff, NoteOn, PolyPressure };
        struct ScheduledEvent
        {
            std::int64_t sample = 0;
            ScheduledEventType type = ScheduledEventType::Controller;
            ClapLiveNoteEvent note;
            ClapLiveControllerEvent controller;
            ClapLivePitchBendEvent pitchBend;
            ClapLiveProgramChangeEvent programChange;
            ClapLiveChannelPressureEvent channelPressure;
            ClapLivePolyPressureEvent polyPressure;
            mw::core::StableId stableId = 0;
            int sequenceOrder = 0;
        };
        static int clampMidiValue(int value);
        static int clampMidiChannel(int value);
        static std::int64_t tickOffsetToSample(std::int64_t tickOffset, const ClapLivePlaybackSchedulerConfig& config);
        ClapLivePlaybackSchedulerConfig config_;
        std::vector<ScheduledEvent> scheduledEvents_;
        std::size_t nextEventIndex_ = 0;
        std::int64_t currentSample_ = 0;
        std::int64_t totalSamples_ = 0;
        int maxEventsPerBlock_ = 0;
    };
}
