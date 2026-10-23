#include "midi/MidiParameterLane.h"

#include <algorithm>
#include <map>
#include <utility>

namespace mw::midi
{
    namespace
    {
        constexpr std::size_t kMaximumUndoDepth = 128;
        bool validChannel(int channel) noexcept { return channel >= 1 && channel <= 16; }
        bool valid14(int value) noexcept { return value >= 0 && value <= 16383; }
        bool validRpnParameter(int parameter) noexcept { return valid14(parameter) && parameter != mw::core::midiRpnNullParameter; }

        template <typename T, typename Valid>
        std::vector<T> stateAt(const std::vector<T>& source, std::int64_t tick, Valid valid)
        {
            std::map<std::pair<int, int>, T> latest;
            for (const auto& event : source)
            {
                if (!valid(event) || event.tick > tick) continue;
                const auto key = std::make_pair(event.midiChannel, event.parameter);
                auto it = latest.find(key);
                if (it == latest.end() || it->second.tick < event.tick
                    || (it->second.tick == event.tick && it->second.stableId < event.stableId)) latest[key] = event;
            }
            std::vector<T> result; for (const auto& [key, event] : latest) { (void)key; result.push_back(event); }
            return result;
        }
    }

    bool isValidMidiRpnEvent(const mw::core::MidiRpnEvent& event) noexcept
    { return event.tick >= 0 && validChannel(event.midiChannel) && validRpnParameter(event.parameter) && valid14(event.value); }
    bool isValidMidiNrpnEvent(const mw::core::MidiNrpnEvent& event) noexcept
    { return event.tick >= 0 && validChannel(event.midiChannel) && valid14(event.parameter) && valid14(event.value); }

    std::vector<mw::core::MidiRpnEvent> midiRpnLaneEvents(const mw::core::Track& track, int midiChannel)
    {
        std::vector<mw::core::MidiRpnEvent> out; if (track.isAudioClipTrack() || !validChannel(midiChannel)) return out;
        for (const auto& e : track.getMidiRpnEvents()) if (isValidMidiRpnEvent(e) && e.midiChannel == midiChannel) out.push_back(e);
        std::stable_sort(out.begin(), out.end(), mw::core::midiRpnEventOrder); return out;
    }
    std::vector<mw::core::MidiNrpnEvent> midiNrpnLaneEvents(const mw::core::Track& track, int midiChannel)
    {
        std::vector<mw::core::MidiNrpnEvent> out; if (track.isAudioClipTrack() || !validChannel(midiChannel)) return out;
        for (const auto& e : track.getMidiNrpnEvents()) if (isValidMidiNrpnEvent(e) && e.midiChannel == midiChannel) out.push_back(e);
        std::stable_sort(out.begin(), out.end(), mw::core::midiNrpnEventOrder); return out;
    }
    std::vector<mw::core::MidiRpnEvent> midiRpnStateAtOrBeforeTick(const mw::core::Track& track, std::int64_t tick)
    { return track.isAudioClipTrack() ? std::vector<mw::core::MidiRpnEvent>{} : stateAt(track.getMidiRpnEvents(), tick, isValidMidiRpnEvent); }
    std::vector<mw::core::MidiNrpnEvent> midiNrpnStateAtOrBeforeTick(const mw::core::Track& track, std::int64_t tick)
    { return track.isAudioClipTrack() ? std::vector<mw::core::MidiNrpnEvent>{} : stateAt(track.getMidiNrpnEvents(), tick, isValidMidiNrpnEvent); }

    bool midiRpnEventVectorsEqual(const std::vector<mw::core::MidiRpnEvent>& a,
                                  const std::vector<mw::core::MidiRpnEvent>& b) noexcept
    {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i)
            if (a[i].stableId != b[i].stableId || a[i].tick != b[i].tick || a[i].midiChannel != b[i].midiChannel
                || a[i].parameter != b[i].parameter || a[i].value != b[i].value) return false;
        return true;
    }

    bool midiNrpnEventVectorsEqual(const std::vector<mw::core::MidiNrpnEvent>& a,
                                   const std::vector<mw::core::MidiNrpnEvent>& b) noexcept
    {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i)
            if (a[i].stableId != b[i].stableId || a[i].tick != b[i].tick || a[i].midiChannel != b[i].midiChannel
                || a[i].parameter != b[i].parameter || a[i].value != b[i].value) return false;
        return true;
    }

    std::int64_t midiParameterSnapGridTicks(int ticksPerQuarterNote, MidiParameterSnapGrid grid) noexcept
    {
        const auto ppq = std::max(1, ticksPerQuarterNote);
        switch (grid)
        {
            case MidiParameterSnapGrid::Quarter: return ppq;
            case MidiParameterSnapGrid::Eighth: return std::max<std::int64_t>(1, ppq / 2);
            case MidiParameterSnapGrid::Sixteenth: return std::max<std::int64_t>(1, ppq / 4);
            case MidiParameterSnapGrid::ThirtySecond: return std::max<std::int64_t>(1, ppq / 8);
            case MidiParameterSnapGrid::SixtyFourth: return std::max<std::int64_t>(1, ppq / 16);
            case MidiParameterSnapGrid::Off: default: return 1;
        }
    }

    std::int64_t snapMidiParameterTick(std::int64_t rawTick, int ticksPerQuarterNote, MidiParameterSnapGrid grid) noexcept
    {
        const auto safe = std::max<std::int64_t>(0, rawTick);
        if (grid == MidiParameterSnapGrid::Off) return safe;
        const auto step = midiParameterSnapGridTicks(ticksPerQuarterNote, grid);
        const auto q = safe / step;
        const auto r = safe % step;
        return r * 2 >= step ? (q + 1) * step : q * step;
    }

    std::size_t commitRecordedMidiRpnEvents(mw::core::Project& project, mw::core::StableId trackId,
                                            const std::vector<mw::core::PersistentMidiRpnEvent>& recorded)
    {
        auto it = std::find_if(project.getTracks().begin(), project.getTracks().end(), [&](const auto& t) { return t.getStableId() == trackId; });
        if (it == project.getTracks().end() || it->isAudioClipTrack()) return 0;
        auto events = it->getMidiRpnEvents();
        std::size_t changed = 0;
        for (const auto& r : recorded)
        {
            mw::core::MidiRpnEvent e(r.tick, r.midiChannel, r.parameter, r.value); if (!isValidMidiRpnEvent(e)) continue;
            auto x = std::find_if(events.begin(), events.end(), [&](const auto& a) { return a.tick == e.tick && a.midiChannel == e.midiChannel && a.parameter == e.parameter; });
            if (x == events.end()) { events.push_back(e); ++changed; } else if (x->value != e.value) { x->value = e.value; ++changed; }
        }
        if (changed) it->setMidiRpnEvents(std::move(events));
        return changed;
    }
    std::size_t commitRecordedMidiNrpnEvents(mw::core::Project& project, mw::core::StableId trackId,
                                             const std::vector<mw::core::PersistentMidiNrpnEvent>& recorded)
    {
        auto it = std::find_if(project.getTracks().begin(), project.getTracks().end(), [&](const auto& t) { return t.getStableId() == trackId; });
        if (it == project.getTracks().end() || it->isAudioClipTrack()) return 0;
        auto events = it->getMidiNrpnEvents();
        std::size_t changed = 0;
        for (const auto& r : recorded)
        {
            mw::core::MidiNrpnEvent e(r.tick, r.midiChannel, r.parameter, r.value); if (!isValidMidiNrpnEvent(e)) continue;
            auto x = std::find_if(events.begin(), events.end(), [&](const auto& a) { return a.tick == e.tick && a.midiChannel == e.midiChannel && a.parameter == e.parameter; });
            if (x == events.end()) { events.push_back(e); ++changed; } else if (x->value != e.value) { x->value = e.value; ++changed; }
        }
        if (changed) it->setMidiNrpnEvents(std::move(events));
        return changed;
    }

    mw::core::Track* MidiParameterEditHistory::findTrack(mw::core::Project& project, mw::core::StableId trackId)
    {
        auto it = std::find_if(project.getTracks().begin(), project.getTracks().end(), [&](auto& t) { return t.getStableId() == trackId; });
        return it == project.getTracks().end() || it->isAudioClipTrack() ? nullptr : &*it;
    }
    void MidiParameterEditHistory::push(Transaction tx)
    { undoStack_.push_back(std::move(tx)); if (undoStack_.size() > kMaximumUndoDepth) undoStack_.erase(undoStack_.begin()); redoStack_.clear(); }
    bool MidiParameterEditHistory::restore(mw::core::Project& project, const Transaction& tx, bool after)
    {
        auto* t = findTrack(project, tx.trackId);
        if (!t) return false;
        if (tx.mode == MidiParameterMode::Rpn)
            t->setMidiRpnEvents(after ? tx.rpnAfter : tx.rpnBefore);
        else
            t->setMidiNrpnEvents(after ? tx.nrpnAfter : tx.nrpnBefore);
        return true;
    }

    MidiParameterEditResult MidiParameterEditHistory::addAtTick(mw::core::Project& project, mw::core::StableId trackId, MidiParameterMode mode,
                                                                 std::int64_t tick, int channel, int parameter, int value)
    {
        MidiParameterEditResult result; auto* t = findTrack(project, trackId); if (!t) return result;
        if (mode == MidiParameterMode::Rpn)
        {
            mw::core::MidiRpnEvent c(tick, channel, parameter, value); if (!isValidMidiRpnEvent(c)) return result;
            Transaction tx; tx.trackId=trackId; tx.mode=mode; tx.rpnBefore=t->getMidiRpnEvents(); auto events=tx.rpnBefore;
            auto it=std::find_if(events.begin(),events.end(),[&](const auto& e){return e.tick==tick&&e.midiChannel==channel&&e.parameter==parameter;});
            if(it!=events.end()){result.eventId=it->stableId;result.replacedExisting=true;if(it->value==value)return result;it->value=value;}else{events.push_back(c);result.eventId=c.stableId;}
            std::stable_sort(events.begin(),events.end(),mw::core::midiRpnEventOrder);tx.rpnAfter=events;t->setMidiRpnEvents(events);push(std::move(tx));result.changed=true;return result;
        }
        mw::core::MidiNrpnEvent c(tick, channel, parameter, value); if (!isValidMidiNrpnEvent(c)) return result;
        Transaction tx; tx.trackId=trackId; tx.mode=mode; tx.nrpnBefore=t->getMidiNrpnEvents(); auto events=tx.nrpnBefore;
        auto it=std::find_if(events.begin(),events.end(),[&](const auto& e){return e.tick==tick&&e.midiChannel==channel&&e.parameter==parameter;});
        if(it!=events.end()){result.eventId=it->stableId;result.replacedExisting=true;if(it->value==value)return result;it->value=value;}else{events.push_back(c);result.eventId=c.stableId;}
        std::stable_sort(events.begin(),events.end(),mw::core::midiNrpnEventOrder);tx.nrpnAfter=events;t->setMidiNrpnEvents(events);push(std::move(tx));result.changed=true;return result;
    }

    bool MidiParameterEditHistory::moveToTick(mw::core::Project& project, mw::core::StableId trackId, MidiParameterMode mode, mw::core::StableId eventId, std::int64_t tick)
    {
        if (tick < 0) return false;
        auto* t = findTrack(project, trackId);
        if (!t) return false;
        Transaction tx;
        tx.trackId = trackId;
        tx.mode = mode;
        if(mode==MidiParameterMode::Rpn){tx.rpnBefore=t->getMidiRpnEvents();auto e=tx.rpnBefore;auto it=std::find_if(e.begin(),e.end(),[&](const auto& x){return x.stableId==eventId;});if(it==e.end()||it->tick==tick)return false;int c=it->midiChannel,p=it->parameter;e.erase(std::remove_if(e.begin(),e.end(),[&](const auto& x){return x.stableId!=eventId&&x.tick==tick&&x.midiChannel==c&&x.parameter==p;}),e.end());it=std::find_if(e.begin(),e.end(),[&](const auto& x){return x.stableId==eventId;});it->tick=tick;std::stable_sort(e.begin(),e.end(),mw::core::midiRpnEventOrder);tx.rpnAfter=e;t->setMidiRpnEvents(e);push(std::move(tx));return true;}
        tx.nrpnBefore=t->getMidiNrpnEvents();auto e=tx.nrpnBefore;auto it=std::find_if(e.begin(),e.end(),[&](const auto& x){return x.stableId==eventId;});if(it==e.end()||it->tick==tick)return false;int c=it->midiChannel,p=it->parameter;e.erase(std::remove_if(e.begin(),e.end(),[&](const auto& x){return x.stableId!=eventId&&x.tick==tick&&x.midiChannel==c&&x.parameter==p;}),e.end());it=std::find_if(e.begin(),e.end(),[&](const auto& x){return x.stableId==eventId;});it->tick=tick;std::stable_sort(e.begin(),e.end(),mw::core::midiNrpnEventOrder);tx.nrpnAfter=e;t->setMidiNrpnEvents(e);push(std::move(tx));return true;
    }

    bool MidiParameterEditHistory::setParameter(mw::core::Project& project, mw::core::StableId trackId, MidiParameterMode mode, mw::core::StableId eventId, int parameter)
    {
        auto* t=findTrack(project,trackId);if(!t)return false;if((mode==MidiParameterMode::Rpn&&!validRpnParameter(parameter))||(mode==MidiParameterMode::Nrpn&&!valid14(parameter)))return false;Transaction tx;tx.trackId=trackId;tx.mode=mode;
        if(mode==MidiParameterMode::Rpn){tx.rpnBefore=t->getMidiRpnEvents();auto e=tx.rpnBefore;auto it=std::find_if(e.begin(),e.end(),[&](const auto& x){return x.stableId==eventId;});if(it==e.end()||it->parameter==parameter)return false;int c=it->midiChannel;auto tick=it->tick;e.erase(std::remove_if(e.begin(),e.end(),[&](const auto& x){return x.stableId!=eventId&&x.tick==tick&&x.midiChannel==c&&x.parameter==parameter;}),e.end());it=std::find_if(e.begin(),e.end(),[&](const auto& x){return x.stableId==eventId;});it->parameter=parameter;std::stable_sort(e.begin(),e.end(),mw::core::midiRpnEventOrder);tx.rpnAfter=e;t->setMidiRpnEvents(e);push(std::move(tx));return true;}
        tx.nrpnBefore=t->getMidiNrpnEvents();auto e=tx.nrpnBefore;auto it=std::find_if(e.begin(),e.end(),[&](const auto& x){return x.stableId==eventId;});if(it==e.end()||it->parameter==parameter)return false;int c=it->midiChannel;auto tick=it->tick;e.erase(std::remove_if(e.begin(),e.end(),[&](const auto& x){return x.stableId!=eventId&&x.tick==tick&&x.midiChannel==c&&x.parameter==parameter;}),e.end());it=std::find_if(e.begin(),e.end(),[&](const auto& x){return x.stableId==eventId;});it->parameter=parameter;std::stable_sort(e.begin(),e.end(),mw::core::midiNrpnEventOrder);tx.nrpnAfter=e;t->setMidiNrpnEvents(e);push(std::move(tx));return true;
    }

    bool MidiParameterEditHistory::setValue(mw::core::Project& project, mw::core::StableId trackId, MidiParameterMode mode, mw::core::StableId eventId, int value)
    {
        if (!valid14(value)) return false;
        auto* t = findTrack(project, trackId);
        if (!t) return false;
        Transaction tx;
        tx.trackId = trackId;
        tx.mode = mode;
        if(mode==MidiParameterMode::Rpn){tx.rpnBefore=t->getMidiRpnEvents();auto e=tx.rpnBefore;auto it=std::find_if(e.begin(),e.end(),[&](const auto& x){return x.stableId==eventId;});if(it==e.end()||it->value==value)return false;it->value=value;tx.rpnAfter=e;t->setMidiRpnEvents(e);push(std::move(tx));return true;}
        tx.nrpnBefore=t->getMidiNrpnEvents();auto e=tx.nrpnBefore;auto it=std::find_if(e.begin(),e.end(),[&](const auto& x){return x.stableId==eventId;});if(it==e.end()||it->value==value)return false;it->value=value;tx.nrpnAfter=e;t->setMidiNrpnEvents(e);push(std::move(tx));return true;
    }

    bool MidiParameterEditHistory::deleteEvent(mw::core::Project& project, mw::core::StableId trackId, MidiParameterMode mode, mw::core::StableId eventId)
    {
        auto* t=findTrack(project,trackId);if(!t)return false;Transaction tx;tx.trackId=trackId;tx.mode=mode;
        if(mode==MidiParameterMode::Rpn){tx.rpnBefore=t->getMidiRpnEvents();auto e=tx.rpnBefore;auto n=e.size();e.erase(std::remove_if(e.begin(),e.end(),[&](const auto& x){return x.stableId==eventId;}),e.end());if(e.size()==n)return false;tx.rpnAfter=e;t->setMidiRpnEvents(e);push(std::move(tx));return true;}
        tx.nrpnBefore=t->getMidiNrpnEvents();auto e=tx.nrpnBefore;auto n=e.size();e.erase(std::remove_if(e.begin(),e.end(),[&](const auto& x){return x.stableId==eventId;}),e.end());if(e.size()==n)return false;tx.nrpnAfter=e;t->setMidiNrpnEvents(e);push(std::move(tx));return true;
    }

    bool MidiParameterEditHistory::undo(mw::core::Project& project){if(undoStack_.empty())return false;auto tx=undoStack_.back();undoStack_.pop_back();if(!restore(project,tx,false)){undoStack_.push_back(std::move(tx));return false;}redoStack_.push_back(std::move(tx));return true;}
    bool MidiParameterEditHistory::redo(mw::core::Project& project){if(redoStack_.empty())return false;auto tx=redoStack_.back();redoStack_.pop_back();if(!restore(project,tx,true)){redoStack_.push_back(std::move(tx));return false;}undoStack_.push_back(std::move(tx));return true;}
    void MidiParameterEditHistory::clear() noexcept { undoStack_.clear(); redoStack_.clear(); }
}
