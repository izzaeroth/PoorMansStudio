#pragma once

#include "clap/ClapLiveInstrumentSession.h"

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
        bool isFinished() const;
        std::int64_t currentSample() const;
        std::int64_t totalSamples() const;
        int blockSize() const;
        int totalScheduledEvents() const;
        int maxEventsPerBlock() const;

        void nextBlockInto(ClapLiveProcessRequest& request);

    private:
        struct ScheduledEvent
        {
            std::int64_t sample = 0;
            ClapLiveNoteEvent event;
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
