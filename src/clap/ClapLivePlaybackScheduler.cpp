#include "clap/ClapLivePlaybackScheduler.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <utility>

namespace mw::clap
{
    namespace
    {
        constexpr int kDefaultTempoBpm = 120;
        constexpr int kDefaultSampleRate = 48000;
        constexpr int kDefaultBlockSize = 512;
        constexpr std::int64_t kDefaultTicksPerQuarterNote = 960;
        constexpr double kMaximumTailSeconds = 30.0;
    }
    int ClapLivePlaybackScheduler::clampMidiValue(int value) { return std::clamp(value, 0, 127); }
    int ClapLivePlaybackScheduler::clampMidiChannel(int value) { return std::clamp(value <= 0 ? 1 : value, 1, 16); }
    std::int64_t ClapLivePlaybackScheduler::tickOffsetToSample(std::int64_t tickOffset, const ClapLivePlaybackSchedulerConfig& config)
    {
        if (tickOffset <= 0) return 0;
        const auto tempo = std::max(1, config.tempoBpm);
        const auto sampleRate = std::max(1, config.sampleRate);
        const auto tpq = std::max<std::int64_t>(1, config.ticksPerQuarterNote);
        const double sample = static_cast<double>(tickOffset) * 60.0 / (static_cast<double>(tempo) * static_cast<double>(tpq)) * static_cast<double>(sampleRate);
        if (sample >= static_cast<double>(std::numeric_limits<std::int64_t>::max())) return std::numeric_limits<std::int64_t>::max();
        return std::max<std::int64_t>(0, static_cast<std::int64_t>(std::llround(sample)));
    }
    void ClapLivePlaybackScheduler::prepare(std::vector<ClapLivePlaybackNote> notes, ClapLivePlaybackSchedulerConfig config)
    {
        prepare(std::move(notes), {}, {}, {}, {}, {}, std::move(config));
    }
    void ClapLivePlaybackScheduler::prepare(std::vector<ClapLivePlaybackNote> notes, std::vector<ClapLivePlaybackControllerEvent> controllers, ClapLivePlaybackSchedulerConfig config)
    {
        prepare(std::move(notes), std::move(controllers), {}, {}, {}, {}, std::move(config));
    }
    void ClapLivePlaybackScheduler::prepare(std::vector<ClapLivePlaybackNote> notes, std::vector<ClapLivePlaybackControllerEvent> controllers,
                                            std::vector<ClapLivePlaybackPitchBendEvent> bends, ClapLivePlaybackSchedulerConfig config)
    {
        prepare(std::move(notes), std::move(controllers), std::move(bends), {}, {}, {}, std::move(config));
    }

    void ClapLivePlaybackScheduler::prepare(std::vector<ClapLivePlaybackNote> notes, std::vector<ClapLivePlaybackControllerEvent> controllers,
                                            std::vector<ClapLivePlaybackPitchBendEvent> bends,
                                            std::vector<ClapLivePlaybackProgramChangeEvent> programs, ClapLivePlaybackSchedulerConfig config)
    {
        prepare(std::move(notes), std::move(controllers), std::move(bends), std::move(programs), {}, {}, std::move(config));
    }

    void ClapLivePlaybackScheduler::prepare(std::vector<ClapLivePlaybackNote> notes, std::vector<ClapLivePlaybackControllerEvent> controllers,
                                            std::vector<ClapLivePlaybackPitchBendEvent> bends,
                                            std::vector<ClapLivePlaybackProgramChangeEvent> programs,
                                            std::vector<ClapLivePlaybackChannelPressureEvent> pressures, ClapLivePlaybackSchedulerConfig config)
    {
        prepare(std::move(notes), std::move(controllers), std::move(bends), std::move(programs), std::move(pressures), {}, std::move(config));
    }

    void ClapLivePlaybackScheduler::prepare(std::vector<ClapLivePlaybackNote> notes, std::vector<ClapLivePlaybackControllerEvent> controllers,
                                            std::vector<ClapLivePlaybackPitchBendEvent> bends,
                                            std::vector<ClapLivePlaybackProgramChangeEvent> programs,
                                            std::vector<ClapLivePlaybackChannelPressureEvent> pressures,
                                            std::vector<ClapLivePlaybackPolyPressureEvent> polyPressures, ClapLivePlaybackSchedulerConfig config)
    {
        prepare(std::move(notes), std::move(controllers), std::move(bends), std::move(programs), std::move(pressures),
                std::move(polyPressures), {}, {}, {}, std::move(config));
    }

    void ClapLivePlaybackScheduler::prepare(std::vector<ClapLivePlaybackNote> notes, std::vector<ClapLivePlaybackControllerEvent> controllers,
                                            std::vector<ClapLivePlaybackPitchBendEvent> bends,
                                            std::vector<ClapLivePlaybackProgramChangeEvent> programs,
                                            std::vector<ClapLivePlaybackChannelPressureEvent> pressures,
                                            std::vector<ClapLivePlaybackPolyPressureEvent> polyPressures,
                                            std::vector<ClapLivePlayback14BitControllerEvent> controller14BitEvents,
                                            std::vector<ClapLivePlaybackRpnEvent> rpnEvents,
                                            std::vector<ClapLivePlaybackNrpnEvent> nrpnEvents, ClapLivePlaybackSchedulerConfig config)
    {
        const auto addAdvancedController = [&controllers](mw::core::StableId stableId, std::int64_t tick, int channel, int controller, int value, int order)
        {
            controllers.push_back({ stableId, tick, channel, controller, value, order });
        };
        for (const auto& e : controller14BitEvents)
        {
            if (e.tick < 0 || e.midiChannel < 1 || e.midiChannel > 16 || e.controllerMsb < 1 || e.controllerMsb > 31 || e.value < 0 || e.value > 16383) continue;
            addAdvancedController(e.stableId, e.tick, e.midiChannel, e.controllerMsb, (e.value >> 7) & 0x7f, 20);
            addAdvancedController(e.stableId, e.tick, e.midiChannel, e.controllerMsb + 32, e.value & 0x7f, 21);
        }
        for (const auto& e : rpnEvents)
        {
            if (e.tick < 0 || e.midiChannel < 1 || e.midiChannel > 16 || e.parameter < 0 || e.parameter >= 16383 || e.value < 0 || e.value > 16383) continue;
            addAdvancedController(e.stableId, e.tick, e.midiChannel, 101, (e.parameter >> 7) & 0x7f, 30);
            addAdvancedController(e.stableId, e.tick, e.midiChannel, 100, e.parameter & 0x7f, 31);
            addAdvancedController(e.stableId, e.tick, e.midiChannel, 6, (e.value >> 7) & 0x7f, 32);
            addAdvancedController(e.stableId, e.tick, e.midiChannel, 38, e.value & 0x7f, 33);
            addAdvancedController(e.stableId, e.tick, e.midiChannel, 101, 127, 34);
            addAdvancedController(e.stableId, e.tick, e.midiChannel, 100, 127, 35);
        }
        for (const auto& e : nrpnEvents)
        {
            if (e.tick < 0 || e.midiChannel < 1 || e.midiChannel > 16 || e.parameter < 0 || e.parameter > 16383 || e.value < 0 || e.value > 16383) continue;
            addAdvancedController(e.stableId, e.tick, e.midiChannel, 99, (e.parameter >> 7) & 0x7f, 40);
            addAdvancedController(e.stableId, e.tick, e.midiChannel, 98, e.parameter & 0x7f, 41);
            addAdvancedController(e.stableId, e.tick, e.midiChannel, 6, (e.value >> 7) & 0x7f, 42);
            addAdvancedController(e.stableId, e.tick, e.midiChannel, 38, e.value & 0x7f, 43);
        }

        config.tempoBpm = std::max(1, config.tempoBpm > 0 ? config.tempoBpm : kDefaultTempoBpm);
        config.sampleRate = std::max(1, config.sampleRate > 0 ? config.sampleRate : kDefaultSampleRate);
        config.blockSize = std::max(1, config.blockSize > 0 ? config.blockSize : kDefaultBlockSize);
        config.ticksPerQuarterNote = std::max<std::int64_t>(1, config.ticksPerQuarterNote > 0 ? config.ticksPerQuarterNote : kDefaultTicksPerQuarterNote);
        config.startTick = std::max<std::int64_t>(0, config.startTick);
        config.startSample = std::max<std::int64_t>(0, config.startSample);
        config.tailSeconds = std::clamp(config.tailSeconds, 0.0, kMaximumTailSeconds);
        config_ = config;
        scheduledEvents_.clear(); scheduledEvents_.reserve(notes.size()*2 + controllers.size() + programs.size() + bends.size() + pressures.size() + polyPressures.size() + 32); nextEventIndex_=0; currentSample_=config_.startSample; totalSamples_=config_.startSample; maxEventsPerBlock_=0;
        std::int64_t lastEventSample = config_.startSample;
        std::map<std::pair<int,int>, ClapLivePlaybackControllerEvent> initial;
        for (const auto& e : controllers)
        {
            if (e.tick < 0 || e.midiChannel<1 || e.midiChannel>16 || e.controller<0 || e.controller>127 || e.value<0 || e.value>127) continue;
            if (e.tick <= config_.startTick)
            {
                const auto k=std::make_pair(e.midiChannel,e.controller); const auto it=initial.find(k);
                if (it==initial.end() || e.tick>it->second.tick || (e.tick==it->second.tick && e.stableId>it->second.stableId)) initial[k]=e;
            }
        }
        for (const auto& [k,e] : initial)
        {
            (void)k; ScheduledEvent s; s.sample=config_.startSample; s.type=ScheduledEventType::Controller; s.stableId=e.stableId;
            s.controller.midiChannel=clampMidiChannel(e.midiChannel); s.controller.controller=clampMidiValue(e.controller); s.controller.value=clampMidiValue(e.value); s.sequenceOrder=e.sequenceOrder; scheduledEvents_.push_back(s);
        }
        for (const auto& e : controllers)
        {
            if (e.tick<=config_.startTick || e.tick<0 || e.midiChannel<1 || e.midiChannel>16 || e.controller<0 || e.controller>127 || e.value<0 || e.value>127) continue;
            ScheduledEvent s; s.sample=config_.startSample+tickOffsetToSample(e.tick-config_.startTick,config_); s.type=ScheduledEventType::Controller; s.stableId=e.stableId;
            s.controller.midiChannel=clampMidiChannel(e.midiChannel); s.controller.controller=clampMidiValue(e.controller); s.controller.value=clampMidiValue(e.value); s.sequenceOrder=e.sequenceOrder; scheduledEvents_.push_back(s); lastEventSample=std::max(lastEventSample,s.sample);
        }
        // Program Change is channel-wide patch state. Restore the latest project
        // Program Change at/before the start tick. If there is no authored Program
        // Change yet, the track's normal setup assignment remains authoritative and
        // no synthetic Program Change event is manufactured here.
        std::map<int, ClapLivePlaybackProgramChangeEvent> initialPrograms;
        for (const auto& e : programs)
        {
            if (e.tick < 0 || e.midiChannel < 1 || e.midiChannel > 16 || e.program < 0 || e.program > 127) continue;
            if (e.tick <= config_.startTick)
            {
                const auto it = initialPrograms.find(e.midiChannel);
                if (it == initialPrograms.end() || e.tick > it->second.tick
                    || (e.tick == it->second.tick && e.stableId > it->second.stableId))
                    initialPrograms[e.midiChannel] = e;
            }
        }
        for (const auto& [channel, e] : initialPrograms)
        {
            (void) channel; ScheduledEvent s; s.sample = config_.startSample; s.type = ScheduledEventType::ProgramChange; s.stableId = e.stableId;
            s.programChange.midiChannel = clampMidiChannel(e.midiChannel); s.programChange.program = clampMidiValue(e.program); scheduledEvents_.push_back(s);
        }
        for (const auto& e : programs)
        {
            if (e.tick <= config_.startTick || e.tick < 0 || e.midiChannel < 1 || e.midiChannel > 16 || e.program < 0 || e.program > 127) continue;
            ScheduledEvent s; s.sample = config_.startSample + tickOffsetToSample(e.tick - config_.startTick, config_); s.type = ScheduledEventType::ProgramChange; s.stableId = e.stableId;
            s.programChange.midiChannel = clampMidiChannel(e.midiChannel); s.programChange.program = clampMidiValue(e.program); scheduledEvents_.push_back(s); lastEventSample = std::max(lastEventSample, s.sample);
        }

        // Pitch Bend has channel-wide state. Restore the latest bend at/before the
        // requested start tick. If the track has bend automation on a channel but no
        // earlier event, send center at the start so a reused instrument cannot retain
        // stale bend state from prior playback.
        std::map<int, ClapLivePlaybackPitchBendEvent> initialBends;
        std::map<int, bool> channelsWithBend;
        for (const auto& e : bends)
        {
            if (e.tick < 0 || e.midiChannel < 1 || e.midiChannel > 16 || e.value < -8192 || e.value > 8191) continue;
            channelsWithBend[e.midiChannel] = true;
            if (e.tick <= config_.startTick)
            {
                const auto it = initialBends.find(e.midiChannel);
                if (it == initialBends.end() || e.tick > it->second.tick
                    || (e.tick == it->second.tick && e.stableId > it->second.stableId))
                    initialBends[e.midiChannel] = e;
            }
        }
        for (const auto& [channel, present] : channelsWithBend)
        {
            (void) present;
            ScheduledEvent s; s.sample = config_.startSample; s.type = ScheduledEventType::PitchBend;
            const auto it = initialBends.find(channel);
            if (it != initialBends.end())
            {
                s.stableId = it->second.stableId;
                s.pitchBend.midiChannel = clampMidiChannel(it->second.midiChannel);
                s.pitchBend.value = std::clamp(it->second.value, -8192, 8191);
            }
            else
            {
                s.stableId = 0;
                s.pitchBend.midiChannel = clampMidiChannel(channel);
                s.pitchBend.value = 0;
            }
            scheduledEvents_.push_back(s);
        }
        for (const auto& e : bends)
        {
            if (e.tick <= config_.startTick || e.tick < 0 || e.midiChannel < 1 || e.midiChannel > 16 || e.value < -8192 || e.value > 8191) continue;
            ScheduledEvent s; s.sample = config_.startSample + tickOffsetToSample(e.tick - config_.startTick, config_);
            s.type = ScheduledEventType::PitchBend; s.stableId = e.stableId;
            s.pitchBend.midiChannel = clampMidiChannel(e.midiChannel); s.pitchBend.value = std::clamp(e.value, -8192, 8191);
            scheduledEvents_.push_back(s); lastEventSample = std::max(lastEventSample, s.sample);
        }

        // Channel Pressure is channel-wide expressive state. Restore the latest
        // pressure at/before the start tick. If the channel has later pressure
        // automation but no prior value, send runtime zero to clear stale synth state.
        std::map<int, ClapLivePlaybackChannelPressureEvent> initialPressures;
        std::map<int, bool> channelsWithPressure;
        for (const auto& e : pressures)
        {
            if (e.tick < 0 || e.midiChannel < 1 || e.midiChannel > 16 || e.pressure < 0 || e.pressure > 127) continue;
            channelsWithPressure[e.midiChannel] = true;
            if (e.tick <= config_.startTick)
            {
                const auto it = initialPressures.find(e.midiChannel);
                if (it == initialPressures.end() || e.tick > it->second.tick
                    || (e.tick == it->second.tick && e.stableId > it->second.stableId))
                    initialPressures[e.midiChannel] = e;
            }
        }
        for (const auto& [channel, present] : channelsWithPressure)
        {
            (void) present;
            ScheduledEvent s; s.sample = config_.startSample; s.type = ScheduledEventType::ChannelPressure;
            const auto it = initialPressures.find(channel);
            if (it != initialPressures.end())
            {
                s.stableId = it->second.stableId;
                s.channelPressure.midiChannel = clampMidiChannel(it->second.midiChannel);
                s.channelPressure.pressure = clampMidiValue(it->second.pressure);
            }
            else
            {
                s.stableId = 0;
                s.channelPressure.midiChannel = clampMidiChannel(channel);
                s.channelPressure.pressure = 0;
            }
            scheduledEvents_.push_back(s);
        }
        for (const auto& e : pressures)
        {
            if (e.tick <= config_.startTick || e.tick < 0 || e.midiChannel < 1 || e.midiChannel > 16 || e.pressure < 0 || e.pressure > 127) continue;
            ScheduledEvent s; s.sample = config_.startSample + tickOffsetToSample(e.tick - config_.startTick, config_);
            s.type = ScheduledEventType::ChannelPressure; s.stableId = e.stableId;
            s.channelPressure.midiChannel = clampMidiChannel(e.midiChannel); s.channelPressure.pressure = clampMidiValue(e.pressure);
            scheduledEvents_.push_back(s); lastEventSample = std::max(lastEventSample, s.sample);
        }

        // Polyphonic Key Pressure is per channel+key state. Restore only keys
        // represented by real project automation, and use runtime zero before the
        // first value to prevent stale per-key aftertouch without flooding 16x128 keys.
        std::map<std::pair<int,int>, ClapLivePlaybackPolyPressureEvent> initialPoly;
        std::map<std::pair<int,int>, bool> keysWithPoly;
        for (const auto& e : polyPressures)
        {
            if (e.tick < 0 || e.midiChannel < 1 || e.midiChannel > 16
                || e.noteNumber < 0 || e.noteNumber > 127 || e.pressure < 0 || e.pressure > 127) continue;
            const auto key = std::make_pair(e.midiChannel, e.noteNumber);
            keysWithPoly[key] = true;
            if (e.tick <= config_.startTick)
            {
                const auto it = initialPoly.find(key);
                if (it == initialPoly.end() || e.tick > it->second.tick
                    || (e.tick == it->second.tick && e.stableId > it->second.stableId))
                    initialPoly[key] = e;
            }
        }
        for (const auto& [key, present] : keysWithPoly)
        {
            (void) present;
            ScheduledEvent s; s.sample = config_.startSample; s.type = ScheduledEventType::PolyPressure;
            const auto it = initialPoly.find(key);
            s.polyPressure.midiChannel = clampMidiChannel(key.first);
            s.polyPressure.noteNumber = clampMidiValue(key.second);
            if (it != initialPoly.end())
            {
                s.stableId = it->second.stableId;
                s.polyPressure.pressure = clampMidiValue(it->second.pressure);
            }
            else
            {
                s.stableId = 0;
                s.polyPressure.pressure = 0;
            }
            scheduledEvents_.push_back(s);
        }
        for (const auto& e : polyPressures)
        {
            if (e.tick <= config_.startTick || e.tick < 0 || e.midiChannel < 1 || e.midiChannel > 16
                || e.noteNumber < 0 || e.noteNumber > 127 || e.pressure < 0 || e.pressure > 127) continue;
            ScheduledEvent s; s.sample = config_.startSample + tickOffsetToSample(e.tick - config_.startTick, config_);
            s.type = ScheduledEventType::PolyPressure; s.stableId = e.stableId;
            s.polyPressure.midiChannel = clampMidiChannel(e.midiChannel);
            s.polyPressure.noteNumber = clampMidiValue(e.noteNumber);
            s.polyPressure.pressure = clampMidiValue(e.pressure);
            scheduledEvents_.push_back(s); lastEventSample = std::max(lastEventSample, s.sample);
        }

        for (const auto& n : notes)
        {
            const auto dur=std::max<std::int64_t>(1,n.durationTicks); const auto endTick=n.startTick+dur; if (endTick<config_.startTick) continue;
            const auto startSample=config_.startSample+tickOffsetToSample(std::max<std::int64_t>(0,n.startTick-config_.startTick),config_);
            const auto endSample=std::max<std::int64_t>(startSample+1,config_.startSample+tickOffsetToSample(std::max<std::int64_t>(0,endTick-config_.startTick),config_));
            ScheduledEvent on; on.sample=startSample; on.type=ScheduledEventType::NoteOn; on.note={ClapLiveNoteEventType::NoteOn,0,clampMidiValue(n.pitch),clampMidiValue(n.velocity),clampMidiChannel(n.midiChannel)}; scheduledEvents_.push_back(on);
            ScheduledEvent off; off.sample=endSample; off.type=ScheduledEventType::NoteOff; off.note={ClapLiveNoteEventType::NoteOff,0,on.note.key,0,on.note.midiChannel}; scheduledEvents_.push_back(off); lastEventSample=std::max(lastEventSample,endSample);
        }
        std::sort(scheduledEvents_.begin(),scheduledEvents_.end(),[](const auto& a,const auto& b){
            if (a.sample != b.sample) return a.sample < b.sample;
            if (a.type != b.type) return static_cast<int>(a.type) < static_cast<int>(b.type);
            if(a.type==ScheduledEventType::Controller){if(a.sequenceOrder!=b.sequenceOrder)return a.sequenceOrder<b.sequenceOrder; if(a.controller.midiChannel!=b.controller.midiChannel)return a.controller.midiChannel<b.controller.midiChannel; if(a.controller.controller!=b.controller.controller)return a.controller.controller<b.controller.controller; return a.stableId<b.stableId;}
            if(a.type==ScheduledEventType::ProgramChange){if(a.programChange.midiChannel!=b.programChange.midiChannel)return a.programChange.midiChannel<b.programChange.midiChannel; return a.stableId<b.stableId;}
            if(a.type==ScheduledEventType::PitchBend){if(a.pitchBend.midiChannel!=b.pitchBend.midiChannel)return a.pitchBend.midiChannel<b.pitchBend.midiChannel; return a.stableId<b.stableId;}
            if(a.type==ScheduledEventType::ChannelPressure){if(a.channelPressure.midiChannel!=b.channelPressure.midiChannel)return a.channelPressure.midiChannel<b.channelPressure.midiChannel; return a.stableId<b.stableId;}
            if(a.type==ScheduledEventType::PolyPressure){if(a.polyPressure.midiChannel!=b.polyPressure.midiChannel)return a.polyPressure.midiChannel<b.polyPressure.midiChannel; if(a.polyPressure.noteNumber!=b.polyPressure.noteNumber)return a.polyPressure.noteNumber<b.polyPressure.noteNumber; return a.stableId<b.stableId;}
            if (a.note.key != b.note.key) return a.note.key < b.note.key;
            return a.note.midiChannel < b.note.midiChannel;
        });
        if(!scheduledEvents_.empty())
        {
            const auto tail=static_cast<std::int64_t>(std::ceil(config_.tailSeconds*config_.sampleRate));
            totalSamples_=std::max<std::int64_t>(config_.startSample+config_.blockSize,lastEventSample+std::max<std::int64_t>(1,tail));
            std::int64_t active=-1; int count=0; for(const auto& e:scheduledEvents_){const auto bi=(e.sample-config_.startSample)/std::max<std::int64_t>(1,config_.blockSize); if(bi!=active){maxEventsPerBlock_=std::max(maxEventsPerBlock_,count);active=bi;count=0;}++count;} maxEventsPerBlock_=std::max(maxEventsPerBlock_,count);
        }
    }
    bool ClapLivePlaybackScheduler::isFinished() const { return currentSample_>=totalSamples_; }
    std::int64_t ClapLivePlaybackScheduler::currentSample() const{return currentSample_;}
    std::int64_t ClapLivePlaybackScheduler::totalSamples() const{return totalSamples_;}
    int ClapLivePlaybackScheduler::blockSize() const{return config_.blockSize;}
    int ClapLivePlaybackScheduler::totalScheduledEvents() const{return static_cast<int>(scheduledEvents_.size());}
    int ClapLivePlaybackScheduler::maxEventsPerBlock() const{return maxEventsPerBlock_;}
    void ClapLivePlaybackScheduler::rewind() noexcept
    {
        nextEventIndex_ = 0;
        currentSample_ = config_.startSample;
    }
    void ClapLivePlaybackScheduler::nextFramesInto(ClapLiveProcessRequest& request, int maximumFrames)
    {
        request.frameCount=0; request.noteEvents.clear(); request.controllerEvents.clear(); request.programChangeEvents.clear(); request.pitchBendEvents.clear(); request.channelPressureEvents.clear(); request.polyPressureEvents.clear(); if(isFinished())return;
        const auto frameLimit = std::clamp(maximumFrames, 0, std::max(1, config_.blockSize));
        if(frameLimit<=0)return;
        const auto remaining=std::max<std::int64_t>(0,totalSamples_-currentSample_); const auto frames=static_cast<int>(std::min<std::int64_t>(frameLimit,remaining)); if(frames<=0){currentSample_=totalSamples_;return;}
        request.frameCount=frames; const auto start=currentSample_; const auto end=start+frames;
        while(nextEventIndex_<scheduledEvents_.size())
        {
            const auto& s=scheduledEvents_[nextEventIndex_]; if(s.sample<start){++nextEventIndex_;continue;} if(s.sample>=end)break; const auto offset=static_cast<std::uint32_t>(s.sample-start);
            if(s.type==ScheduledEventType::Controller){auto e=s.controller;e.frameOffset=offset;request.controllerEvents.push_back(e);}
            else if(s.type==ScheduledEventType::ProgramChange){auto e=s.programChange;e.frameOffset=offset;request.programChangeEvents.push_back(e);}
            else if(s.type==ScheduledEventType::PitchBend){auto e=s.pitchBend;e.frameOffset=offset;request.pitchBendEvents.push_back(e);}
            else if(s.type==ScheduledEventType::ChannelPressure){auto e=s.channelPressure;e.frameOffset=offset;request.channelPressureEvents.push_back(e);}
            else if(s.type==ScheduledEventType::PolyPressure){auto e=s.polyPressure;e.frameOffset=offset;request.polyPressureEvents.push_back(e);}
            else{auto e=s.note;e.frameOffset=offset;request.noteEvents.push_back(e);} ++nextEventIndex_;
        }
        currentSample_=end;
    }
    void ClapLivePlaybackScheduler::nextBlockInto(ClapLiveProcessRequest& request)
    {
        nextFramesInto(request, config_.blockSize);
    }
}
