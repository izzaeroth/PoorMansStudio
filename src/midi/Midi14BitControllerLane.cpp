#include "midi/Midi14BitControllerLane.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>

namespace mw::midi
{
    namespace
    {
        constexpr std::size_t kMaximumUndoDepth = 128;
        bool validChannel(int value) noexcept { return value >= 1 && value <= 16; }
        bool validController(int value) noexcept
        {
            return value >= mw::core::midi14BitControllerMinimum && value <= mw::core::midi14BitControllerMaximum;
        }
        bool validValue(int value) noexcept { return value >= mw::core::midi14BitMinimum && value <= mw::core::midi14BitMaximum; }
    }

    bool isValidMidi14BitControllerEvent(const mw::core::Midi14BitControllerEvent& event) noexcept
    {
        return event.tick >= 0 && validChannel(event.midiChannel) && validController(event.controllerMsb) && validValue(event.value);
    }

    void sortMidi14BitControllerEvents(std::vector<mw::core::Midi14BitControllerEvent>& events)
    {
        std::stable_sort(events.begin(), events.end(), mw::core::midi14BitControllerEventOrder);
    }

    std::vector<mw::core::Midi14BitControllerEvent> midi14BitControllerLaneEvents(
        const mw::core::Track& track, int midiChannel, int controllerMsb)
    {
        std::vector<mw::core::Midi14BitControllerEvent> result;
        if (track.isAudioClipTrack() || !validChannel(midiChannel) || !validController(controllerMsb)) return result;
        for (const auto& event : track.getMidi14BitControllerEvents())
            if (isValidMidi14BitControllerEvent(event) && event.midiChannel == midiChannel && event.controllerMsb == controllerMsb)
                result.push_back(event);
        sortMidi14BitControllerEvents(result);
        return result;
    }

    std::vector<mw::core::Midi14BitControllerEvent> midi14BitControllerStateAtOrBeforeTick(
        const mw::core::Track& track, std::int64_t startTick)
    {
        std::map<std::pair<int, int>, mw::core::Midi14BitControllerEvent> latest;
        if (track.isAudioClipTrack()) return {};
        for (const auto& event : track.getMidi14BitControllerEvents())
        {
            if (!isValidMidi14BitControllerEvent(event) || event.tick > startTick) continue;
            const auto key = std::make_pair(event.midiChannel, event.controllerMsb);
            auto it = latest.find(key);
            if (it == latest.end() || it->second.tick < event.tick
                || (it->second.tick == event.tick && it->second.stableId < event.stableId))
                latest[key] = event;
        }
        std::vector<mw::core::Midi14BitControllerEvent> result;
        for (const auto& [key, event] : latest) { (void) key; result.push_back(event); }
        sortMidi14BitControllerEvents(result);
        return result;
    }

    std::size_t commitRecordedMidi14BitControllerEvents(
        mw::core::Project& project, mw::core::StableId trackId,
        const std::vector<mw::core::PersistentMidi14BitControllerEvent>& recordedEvents)
    {
        auto trackIt = std::find_if(project.getTracks().begin(), project.getTracks().end(),
                                    [&](const auto& track) { return track.getStableId() == trackId; });
        if (trackIt == project.getTracks().end() || trackIt->isAudioClipTrack()) return 0;
        auto events = trackIt->getMidi14BitControllerEvents();
        std::size_t changed = 0;
        for (const auto& recorded : recordedEvents)
        {
            mw::core::Midi14BitControllerEvent event(recorded.tick, recorded.midiChannel, recorded.controllerMsb, recorded.value);
            if (!isValidMidi14BitControllerEvent(event)) continue;
            auto existing = std::find_if(events.begin(), events.end(), [&](const auto& current)
            {
                return current.tick == event.tick && current.midiChannel == event.midiChannel
                    && current.controllerMsb == event.controllerMsb;
            });
            if (existing != events.end())
            {
                if (existing->value != event.value) { existing->value = event.value; ++changed; }
            }
            else { events.push_back(event); ++changed; }
        }
        if (changed) trackIt->setMidi14BitControllerEvents(std::move(events));
        return changed;
    }

    bool midi14BitControllerEventVectorsEqual(
        const std::vector<mw::core::Midi14BitControllerEvent>& left,
        const std::vector<mw::core::Midi14BitControllerEvent>& right) noexcept
    {
        if (left.size() != right.size()) return false;
        for (std::size_t i = 0; i < left.size(); ++i)
        {
            const auto& a = left[i]; const auto& b = right[i];
            if (a.stableId != b.stableId || a.tick != b.tick || a.midiChannel != b.midiChannel
                || a.controllerMsb != b.controllerMsb || a.value != b.value) return false;
        }
        return true;
    }

    std::int64_t midi14BitSnapGridTicks(int ticksPerQuarterNote, Midi14BitSnapGrid grid) noexcept
    {
        const auto ppq = std::max(1, ticksPerQuarterNote);
        switch (grid)
        {
            case Midi14BitSnapGrid::Quarter: return ppq;
            case Midi14BitSnapGrid::Eighth: return std::max<std::int64_t>(1, ppq / 2);
            case Midi14BitSnapGrid::Sixteenth: return std::max<std::int64_t>(1, ppq / 4);
            case Midi14BitSnapGrid::ThirtySecond: return std::max<std::int64_t>(1, ppq / 8);
            case Midi14BitSnapGrid::SixtyFourth: return std::max<std::int64_t>(1, ppq / 16);
            case Midi14BitSnapGrid::Off: default: return 1;
        }
    }

    std::int64_t snapMidi14BitTick(std::int64_t rawTick, int ticksPerQuarterNote, Midi14BitSnapGrid grid) noexcept
    {
        const auto safe = std::max<std::int64_t>(0, rawTick);
        if (grid == Midi14BitSnapGrid::Off) return safe;
        const auto step = midi14BitSnapGridTicks(ticksPerQuarterNote, grid);
        const auto q = safe / step; const auto r = safe % step;
        return r * 2 >= step ? (q + 1) * step : q * step;
    }

    double midi14BitValueToY(int value, double top, double height) noexcept
    {
        const auto safeHeight = std::max(1.0, height);
        return top + (1.0 - static_cast<double>(std::clamp(value, 0, 16383)) / 16383.0) * safeHeight;
    }

    int midi14BitYToValue(double y, double top, double height) noexcept
    {
        const auto safeHeight = std::max(1.0, height);
        return std::clamp(static_cast<int>(std::llround((1.0 - ((y - top) / safeHeight)) * 16383.0)), 0, 16383);
    }

    std::int64_t midi14BitControllerSnapGridTicks(int ticksPerQuarterNote, Midi14BitControllerSnapGrid grid) noexcept
    {
        return midi14BitSnapGridTicks(ticksPerQuarterNote, grid);
    }

    std::int64_t snapMidi14BitControllerTick(std::int64_t rawTick, int ticksPerQuarterNote, Midi14BitControllerSnapGrid grid) noexcept
    {
        return snapMidi14BitTick(rawTick, ticksPerQuarterNote, grid);
    }

    double midi14BitControllerTickToX(std::int64_t tick, std::int64_t viewStartTick, double ticksPerPixel) noexcept
    {
        return static_cast<double>(tick - std::max<std::int64_t>(0, viewStartTick)) / std::max(0.000001, ticksPerPixel);
    }

    std::int64_t midi14BitControllerXToTick(double x, std::int64_t viewStartTick, double ticksPerPixel) noexcept
    {
        return std::max<std::int64_t>(0, viewStartTick
            + static_cast<std::int64_t>(std::llround(std::max(0.0, x) * std::max(0.000001, ticksPerPixel))));
    }

    double midi14BitControllerValueToY(int value, double top, double height) noexcept
    {
        return midi14BitValueToY(value, top, height);
    }

    int midi14BitControllerYToValue(double y, double top, double height) noexcept
    {
        return midi14BitYToValue(y, top, height);
    }

    mw::core::StableId hitTestMidi14BitControllerPoint(
        const std::vector<mw::core::Midi14BitControllerEvent>& events, std::int64_t viewStartTick,
        double ticksPerPixel, double top, double height, double x, double y, double radiusPixels) noexcept
    {
        const auto radius = std::max(1.0, radiusPixels);
        const auto radiusSquared = radius * radius;
        double bestDistance = radiusSquared + 1.0;
        mw::core::StableId bestId = 0;
        for (const auto& event : events)
        {
            if (!isValidMidi14BitControllerEvent(event)) continue;
            const auto dx = midi14BitControllerTickToX(event.tick, viewStartTick, ticksPerPixel) - x;
            const auto dy = midi14BitControllerValueToY(event.value, top, height) - y;
            const auto distance = dx * dx + dy * dy;
            if (distance <= radiusSquared
                && (distance < bestDistance || (distance == bestDistance && (bestId == 0 || event.stableId < bestId))))
            {
                bestDistance = distance;
                bestId = event.stableId;
            }
        }
        return bestId;
    }

    std::vector<Midi14BitControllerStairStep> midi14BitControllerStaircaseSegments(
        const mw::core::Track& track, int midiChannel, int controllerMsb,
        std::int64_t viewStartTick, std::int64_t viewEndTick)
    {
        std::vector<Midi14BitControllerStairStep> result;
        if (track.isAudioClipTrack() || !validChannel(midiChannel) || !validController(controllerMsb)
            || viewEndTick <= viewStartTick) return result;
        const auto events = midi14BitControllerLaneEvents(track, midiChannel, controllerMsb);
        const mw::core::Midi14BitControllerEvent* active = nullptr;
        for (const auto& event : events) { if (event.tick < viewStartTick) active = &event; else break; }
        if (active != nullptr)
        {
            const auto next = std::find_if(events.begin(), events.end(), [&](const auto& event) { return event.tick > viewStartTick; });
            const auto endTick = next != events.end() ? std::min(viewEndTick, next->tick) : viewEndTick;
            if (endTick > viewStartTick) result.push_back({viewStartTick, endTick, active->value, active->stableId});
        }
        for (std::size_t i = 0; i < events.size(); ++i)
        {
            const auto& event = events[i];
            if (event.tick < viewStartTick || event.tick >= viewEndTick) continue;
            const auto nextTick = i + 1 < events.size() ? events[i + 1].tick : viewEndTick;
            const auto endTick = std::min(viewEndTick, nextTick);
            if (endTick > event.tick) result.push_back({event.tick, endTick, event.value, event.stableId});
        }
        return result;
    }

    mw::core::Track* Midi14BitControllerEditHistory::findTrack(mw::core::Project& project, mw::core::StableId trackId)
    {
        auto it = std::find_if(project.getTracks().begin(), project.getTracks().end(), [&](auto& track) { return track.getStableId() == trackId; });
        return it == project.getTracks().end() || it->isAudioClipTrack() ? nullptr : &*it;
    }

    void Midi14BitControllerEditHistory::push(Transaction transaction)
    {
        lastAffectedTrackId_ = transaction.trackId;
        undoStack_.push_back(std::move(transaction));
        if (undoStack_.size() > kMaximumUndoDepth) undoStack_.erase(undoStack_.begin());
        redoStack_.clear();
    }

    Midi14BitEditResult Midi14BitControllerEditHistory::addAtTick(
        mw::core::Project& project, mw::core::StableId trackId, std::int64_t tick,
        int midiChannel, int controllerMsb, int value)
    {
        Midi14BitEditResult result;
        mw::core::Midi14BitControllerEvent candidate(tick, midiChannel, controllerMsb, value);
        if (!isValidMidi14BitControllerEvent(candidate)) return result;
        auto* track = findTrack(project, trackId); if (!track) return result;
        Transaction tx{trackId, track->getMidi14BitControllerEvents(), {}};
        auto events = tx.before;
        auto existing = std::find_if(events.begin(), events.end(), [&](const auto& event)
        { return event.tick == tick && event.midiChannel == midiChannel && event.controllerMsb == controllerMsb; });
        if (existing != events.end())
        {
            result.eventId = existing->stableId; result.replacedExisting = true;
            if (existing->value == value) return result;
            existing->value = value;
        }
        else { events.push_back(candidate); result.eventId = candidate.stableId; }
        sortMidi14BitControllerEvents(events); tx.after = events; track->setMidi14BitControllerEvents(events); push(std::move(tx)); result.changed = true; return result;
    }

    bool Midi14BitControllerEditHistory::moveAndSetValue(mw::core::Project& project, mw::core::StableId trackId,
                                                          mw::core::StableId eventId, std::int64_t tick, int value)
    {
        if (tick < 0 || !validValue(value)) return false;
        auto* track = findTrack(project, trackId); if (!track) return false;
        Transaction tx{trackId, track->getMidi14BitControllerEvents(), {}};
        auto events = tx.before;
        auto it = std::find_if(events.begin(), events.end(), [&](const auto& event) { return event.stableId == eventId; });
        if (it == events.end() || (it->tick == tick && it->value == value)) return false;
        const int channel = it->midiChannel, controller = it->controllerMsb;
        events.erase(std::remove_if(events.begin(), events.end(), [&](const auto& event)
        { return event.stableId != eventId && event.tick == tick && event.midiChannel == channel && event.controllerMsb == controller; }), events.end());
        it = std::find_if(events.begin(), events.end(), [&](const auto& event) { return event.stableId == eventId; });
        it->tick = tick; it->value = value; sortMidi14BitControllerEvents(events); tx.after = events; track->setMidi14BitControllerEvents(events); push(std::move(tx)); return true;
    }

    bool Midi14BitControllerEditHistory::moveToTick(mw::core::Project& project, mw::core::StableId trackId,
                                                     mw::core::StableId eventId, std::int64_t tick)
    {
        auto* track = findTrack(project, trackId); if (!track) return false;
        auto it = std::find_if(track->getMidi14BitControllerEvents().begin(), track->getMidi14BitControllerEvents().end(), [&](const auto& event) { return event.stableId == eventId; });
        return it != track->getMidi14BitControllerEvents().end() && moveAndSetValue(project, trackId, eventId, tick, it->value);
    }

    bool Midi14BitControllerEditHistory::setValue(mw::core::Project& project, mw::core::StableId trackId,
                                                   mw::core::StableId eventId, int value)
    {
        auto* track = findTrack(project, trackId); if (!track) return false;
        auto it = std::find_if(track->getMidi14BitControllerEvents().begin(), track->getMidi14BitControllerEvents().end(), [&](const auto& event) { return event.stableId == eventId; });
        return it != track->getMidi14BitControllerEvents().end() && moveAndSetValue(project, trackId, eventId, it->tick, value);
    }

    bool Midi14BitControllerEditHistory::deleteEvent(mw::core::Project& project, mw::core::StableId trackId,
                                                      mw::core::StableId eventId)
    {
        auto* track = findTrack(project, trackId); if (!track) return false;
        Transaction tx{trackId, track->getMidi14BitControllerEvents(), {}}; auto events = tx.before;
        const auto oldSize = events.size();
        events.erase(std::remove_if(events.begin(), events.end(), [&](const auto& event) { return event.stableId == eventId; }), events.end());
        if (events.size() == oldSize) return false;
        tx.after = events; track->setMidi14BitControllerEvents(events); push(std::move(tx)); return true;
    }

    bool Midi14BitControllerEditHistory::restore(mw::core::Project& project, const Transaction& tx, bool after)
    {
        auto* track = findTrack(project, tx.trackId); if (!track) return false;
        track->setMidi14BitControllerEvents(after ? tx.after : tx.before); return true;
    }
    bool Midi14BitControllerEditHistory::undo(mw::core::Project& project)
    {
        if (undoStack_.empty()) return false;
        auto tx = undoStack_.back();
        undoStack_.pop_back();
        if (!restore(project, tx, false))
        {
            undoStack_.push_back(std::move(tx));
            return false;
        }
        lastAffectedTrackId_ = tx.trackId;
        redoStack_.push_back(std::move(tx));
        return true;
    }
    bool Midi14BitControllerEditHistory::redo(mw::core::Project& project)
    {
        if (redoStack_.empty()) return false;
        auto tx = redoStack_.back();
        redoStack_.pop_back();
        if (!restore(project, tx, true))
        {
            redoStack_.push_back(std::move(tx));
            return false;
        }
        lastAffectedTrackId_ = tx.trackId;
        undoStack_.push_back(std::move(tx));
        return true;
    }
    void Midi14BitControllerEditHistory::clear() noexcept { undoStack_.clear(); redoStack_.clear(); lastAffectedTrackId_ = 0; }
}
