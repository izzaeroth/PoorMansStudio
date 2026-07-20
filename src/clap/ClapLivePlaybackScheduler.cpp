#include "clap/ClapLivePlaybackScheduler.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace mw::clap
{
    namespace
    {
        constexpr int kDefaultTempoBpm = 120;
        constexpr int kDefaultSampleRate = 48000;
        constexpr int kDefaultBlockSize = 512;
        constexpr std::int64_t kDefaultTicksPerQuarterNote = 960;
        constexpr double kMaximumTailSeconds = 30.0;

        bool noteOffBeforeNoteOnAtSameFrame(ClapLiveNoteEventType left, ClapLiveNoteEventType right)
        {
            if (left == right)
                return false;

            return left == ClapLiveNoteEventType::NoteOff;
        }
    }

    int ClapLivePlaybackScheduler::clampMidiValue(int value)
    {
        return std::clamp(value, 0, 127);
    }

    int ClapLivePlaybackScheduler::clampMidiChannel(int value)
    {
        return std::clamp(value <= 0 ? 1 : value, 1, 16);
    }

    std::int64_t ClapLivePlaybackScheduler::tickOffsetToSample(std::int64_t tickOffset, const ClapLivePlaybackSchedulerConfig& config)
    {
        if (tickOffset <= 0)
            return 0;

        const auto tempo = std::max(1, config.tempoBpm);
        const auto sampleRate = std::max(1, config.sampleRate);
        const auto ticksPerQuarter = std::max<std::int64_t>(1, config.ticksPerQuarterNote);
        const double secondsPerTick = 60.0 / (static_cast<double>(tempo) * static_cast<double>(ticksPerQuarter));
        const double sample = static_cast<double>(tickOffset) * secondsPerTick * static_cast<double>(sampleRate);

        if (sample >= static_cast<double>(std::numeric_limits<std::int64_t>::max()))
            return std::numeric_limits<std::int64_t>::max();

        return std::max<std::int64_t>(0, static_cast<std::int64_t>(std::llround(sample)));
    }

    void ClapLivePlaybackScheduler::prepare(std::vector<ClapLivePlaybackNote> notes, ClapLivePlaybackSchedulerConfig config)
    {
        config.tempoBpm = std::max(1, config.tempoBpm > 0 ? config.tempoBpm : kDefaultTempoBpm);
        config.sampleRate = std::max(1, config.sampleRate > 0 ? config.sampleRate : kDefaultSampleRate);
        config.blockSize = std::max(1, config.blockSize > 0 ? config.blockSize : kDefaultBlockSize);
        config.ticksPerQuarterNote = std::max<std::int64_t>(1, config.ticksPerQuarterNote > 0 ? config.ticksPerQuarterNote : kDefaultTicksPerQuarterNote);
        config.startSample = std::max<std::int64_t>(0, config.startSample);
        config.tailSeconds = std::clamp(config.tailSeconds, 0.0, kMaximumTailSeconds);
        config_ = config;

        scheduledEvents_.clear();
        scheduledEvents_.reserve(notes.size() * 2);
        nextEventIndex_ = 0;
        currentSample_ = config_.startSample;
        totalSamples_ = config_.startSample;
        maxEventsPerBlock_ = 0;

        std::int64_t lastNoteEndSample = config_.startSample;
        for (const auto& note : notes)
        {
            const auto durationTicks = std::max<std::int64_t>(1, note.durationTicks);
            const auto noteStartTick = note.startTick;
            const auto noteEndTick = noteStartTick + durationTicks;

            if (noteEndTick < config_.startTick)
                continue;

            const auto relativeStartTick = std::max<std::int64_t>(0, noteStartTick - config_.startTick);
            const auto relativeEndTick = std::max<std::int64_t>(0, noteEndTick - config_.startTick);
            const auto noteStartSample = config_.startSample + tickOffsetToSample(relativeStartTick, config_);
            const auto noteEndSample = std::max<std::int64_t>(noteStartSample + 1, config_.startSample + tickOffsetToSample(relativeEndTick, config_));

            ClapLiveNoteEvent noteOn;
            noteOn.type = ClapLiveNoteEventType::NoteOn;
            noteOn.frameOffset = 0;
            noteOn.key = clampMidiValue(note.pitch);
            noteOn.velocity = clampMidiValue(note.velocity);
            noteOn.midiChannel = clampMidiChannel(note.midiChannel);
            scheduledEvents_.push_back({ noteStartSample, noteOn });

            ClapLiveNoteEvent noteOff;
            noteOff.type = ClapLiveNoteEventType::NoteOff;
            noteOff.frameOffset = 0;
            noteOff.key = noteOn.key;
            noteOff.velocity = 0;
            noteOff.midiChannel = noteOn.midiChannel;
            scheduledEvents_.push_back({ noteEndSample, noteOff });

            lastNoteEndSample = std::max(lastNoteEndSample, noteEndSample);
        }

        std::sort(scheduledEvents_.begin(), scheduledEvents_.end(), [](const ScheduledEvent& left, const ScheduledEvent& right)
        {
            if (left.sample != right.sample)
                return left.sample < right.sample;

            if (left.event.type != right.event.type)
                return noteOffBeforeNoteOnAtSameFrame(left.event.type, right.event.type);

            if (left.event.key != right.event.key)
                return left.event.key < right.event.key;

            return left.event.midiChannel < right.event.midiChannel;
        });

        if (!scheduledEvents_.empty())
        {
            const auto tailSamples = static_cast<std::int64_t>(std::ceil(config_.tailSeconds * static_cast<double>(config_.sampleRate)));
            totalSamples_ = std::max<std::int64_t>(config_.startSample + config_.blockSize, lastNoteEndSample + std::max<std::int64_t>(0, tailSamples));

            std::int64_t activeBlockIndex = -1;
            int eventsInActiveBlock = 0;
            for (const auto& scheduled : scheduledEvents_)
            {
                if (scheduled.sample < config_.startSample)
                    continue;

                const auto blockIndex = (scheduled.sample - config_.startSample) / std::max<std::int64_t>(1, config_.blockSize);
                if (blockIndex != activeBlockIndex)
                {
                    maxEventsPerBlock_ = std::max(maxEventsPerBlock_, eventsInActiveBlock);
                    activeBlockIndex = blockIndex;
                    eventsInActiveBlock = 0;
                }

                ++eventsInActiveBlock;
            }
            maxEventsPerBlock_ = std::max(maxEventsPerBlock_, eventsInActiveBlock);
        }
    }

    bool ClapLivePlaybackScheduler::isFinished() const
    {
        return currentSample_ >= totalSamples_;
    }

    std::int64_t ClapLivePlaybackScheduler::currentSample() const
    {
        return currentSample_;
    }

    std::int64_t ClapLivePlaybackScheduler::totalSamples() const
    {
        return totalSamples_;
    }

    int ClapLivePlaybackScheduler::blockSize() const
    {
        return config_.blockSize;
    }

    int ClapLivePlaybackScheduler::totalScheduledEvents() const
    {
        return static_cast<int>(scheduledEvents_.size());
    }

    int ClapLivePlaybackScheduler::maxEventsPerBlock() const
    {
        return maxEventsPerBlock_;
    }

    void ClapLivePlaybackScheduler::nextBlockInto(ClapLiveProcessRequest& request)
    {
        request.frameCount = 0;
        request.noteEvents.clear();

        if (isFinished())
            return;

        const auto remainingSamples = std::max<std::int64_t>(0, totalSamples_ - currentSample_);
        const auto frameCount = static_cast<int>(std::min<std::int64_t>(config_.blockSize, remainingSamples));
        if (frameCount <= 0)
        {
            currentSample_ = totalSamples_;
            return;
        }

        request.frameCount = frameCount;
        const auto blockStartSample = currentSample_;
        const auto blockEndSample = blockStartSample + frameCount;

        while (nextEventIndex_ < scheduledEvents_.size())
        {
            const auto& scheduled = scheduledEvents_[nextEventIndex_];
            if (scheduled.sample < blockStartSample)
            {
                ++nextEventIndex_;
                continue;
            }

            if (scheduled.sample >= blockEndSample)
                break;

            auto event = scheduled.event;
            event.frameOffset = static_cast<std::uint32_t>(scheduled.sample - blockStartSample);
            request.noteEvents.push_back(event);
            ++nextEventIndex_;
        }

        currentSample_ = blockEndSample;
    }

}
