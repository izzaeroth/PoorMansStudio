#include "midi/MidiPolyPressureLane.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>

namespace mw::midi
{
    namespace
    {
        constexpr std::size_t kMaximumPolyPressureUndoDepth = 128;
        bool validChannel(int channel) noexcept { return channel >= 1 && channel <= 16; }
        bool validNote(int note) noexcept { return note >= 0 && note <= 127; }
        bool validPressure(int pressure) noexcept { return pressure >= mw::core::midiPolyPressureMinimum && pressure <= mw::core::midiPolyPressureMaximum; }
        auto findEventById(std::vector<mw::core::MidiPolyPressureEvent>& events, mw::core::StableId eventId)
        {
            return std::find_if(events.begin(), events.end(), [&](const auto& event) { return event.stableId == eventId; });
        }
    }

    bool isValidMidiPolyPressureEvent(const mw::core::MidiPolyPressureEvent& event) noexcept
    {
        return event.stableId != 0 && event.tick >= 0 && validChannel(event.midiChannel) && validNote(event.noteNumber) && validPressure(event.pressure);
    }

    void sortMidiPolyPressureEvents(std::vector<mw::core::MidiPolyPressureEvent>& events)
    {
        std::sort(events.begin(), events.end(), mw::core::midiPolyPressureEventOrder);
    }

    std::vector<mw::core::MidiPolyPressureEvent> polyPressureLaneEvents(const mw::core::Track& track, int midiChannel, int noteNumber)
    {
        std::vector<mw::core::MidiPolyPressureEvent> result;
        if (track.isAudioClipTrack() || !validChannel(midiChannel) || !validNote(noteNumber)) return result;
        for (const auto& event : track.getMidiPolyPressureEvents()) if (event.midiChannel == midiChannel && event.noteNumber == noteNumber) result.push_back(event);
        sortMidiPolyPressureEvents(result);
        return result;
    }

    std::vector<mw::core::MidiPolyPressureEvent> polyPressureStateAtOrBeforeTick(const mw::core::Track& track, std::int64_t startTick)
    {
        std::map<int, mw::core::MidiPolyPressureEvent> latest;
        if (track.isAudioClipTrack() || startTick < 0) return {};
        for (const auto& event : track.getMidiPolyPressureEvents())
        {
            if (!isValidMidiPolyPressureEvent(event) || event.tick > startTick) continue;
            const int key = (event.midiChannel - 1) * 128 + event.noteNumber;
            const auto it = latest.find(key);
            if (it == latest.end() || event.tick > it->second.tick || (event.tick == it->second.tick && event.stableId > it->second.stableId))
                latest[key] = event;
        }
        std::vector<mw::core::MidiPolyPressureEvent> result;
        result.reserve(latest.size());
        for (const auto& [key, event] : latest) { (void) key; result.push_back(event); }
        return result;
    }

    std::size_t commitRecordedMidiPolyPressureEvents(mw::core::Project& project, mw::core::StableId trackId,
                                                       const std::vector<mw::core::PersistentMidiPolyPressureEvent>& recordedEvents)
    {
        auto trackIt = std::find_if(project.getTracks().begin(), project.getTracks().end(), [&](const auto& track) { return track.getStableId() == trackId; });
        if (trackIt == project.getTracks().end() || trackIt->isAudioClipTrack()) return 0;
        auto events = trackIt->getMidiPolyPressureEvents();
        std::vector<mw::core::PersistentMidiPolyPressureEvent> normalized;
        for (const auto& event : recordedEvents)
            if (event.tick >= 0 && validChannel(event.midiChannel) && validNote(event.noteNumber) && validPressure(event.pressure)) normalized.push_back(event);
        std::stable_sort(normalized.begin(), normalized.end(), [](const auto& a, const auto& b)
        {
            if (a.tick != b.tick) return a.tick < b.tick;
            if (a.midiChannel != b.midiChannel) return a.midiChannel < b.midiChannel;
            if (a.noteNumber != b.noteNumber) return a.noteNumber < b.noteNumber;
            return a.order < b.order;
        });
        std::vector<mw::core::PersistentMidiPolyPressureEvent> collapsed;
        for (const auto& event : normalized)
        {
            if (!collapsed.empty() && collapsed.back().tick == event.tick && collapsed.back().midiChannel == event.midiChannel && collapsed.back().noteNumber == event.noteNumber)
                collapsed.back() = event;
            else
                collapsed.push_back(event);
        }
        std::size_t changed = 0;
        for (const auto& event : collapsed)
        {
            auto existing = std::find_if(events.begin(), events.end(), [&](const auto& current)
            { return current.tick == event.tick && current.midiChannel == event.midiChannel && current.noteNumber == event.noteNumber; });
            if (existing != events.end())
            {
                if (existing->pressure != event.pressure) { existing->pressure = event.pressure; ++changed; }
            }
            else
            {
                events.emplace_back(event.tick, event.midiChannel, event.noteNumber, event.pressure); ++changed;
            }
        }
        if (changed > 0) trackIt->setMidiPolyPressureEvents(std::move(events));
        return changed;
    }

    bool midiPolyPressureEventVectorsEqual(const std::vector<mw::core::MidiPolyPressureEvent>& left,
                                             const std::vector<mw::core::MidiPolyPressureEvent>& right) noexcept
    {
        if (left.size() != right.size()) return false;
        for (std::size_t i = 0; i < left.size(); ++i)
        {
            const auto& a = left[i]; const auto& b = right[i];
            if (a.stableId != b.stableId || a.tick != b.tick || a.midiChannel != b.midiChannel || a.noteNumber != b.noteNumber || a.pressure != b.pressure) return false;
        }
        return true;
    }

    std::int64_t midiPolyPressureSnapGridTicks(int ticksPerQuarterNote, MidiPolyPressureSnapGrid grid) noexcept
    {
        const auto ppq = std::max(1, ticksPerQuarterNote);
        switch (grid)
        {
            case MidiPolyPressureSnapGrid::Quarter: return ppq;
            case MidiPolyPressureSnapGrid::Eighth: return std::max<std::int64_t>(1, ppq / 2);
            case MidiPolyPressureSnapGrid::Sixteenth: return std::max<std::int64_t>(1, ppq / 4);
            case MidiPolyPressureSnapGrid::ThirtySecond: return std::max<std::int64_t>(1, ppq / 8);
            case MidiPolyPressureSnapGrid::SixtyFourth: return std::max<std::int64_t>(1, ppq / 16);
            case MidiPolyPressureSnapGrid::Off: default: return 1;
        }
    }

    std::int64_t snapMidiPolyPressureTick(std::int64_t rawTick, int ticksPerQuarterNote, MidiPolyPressureSnapGrid grid) noexcept
    {
        const auto safeTick = std::max<std::int64_t>(0, rawTick);
        if (grid == MidiPolyPressureSnapGrid::Off) return safeTick;
        const auto gridTicks = midiPolyPressureSnapGridTicks(ticksPerQuarterNote, grid);
        const auto quotient = safeTick / gridTicks;
        const auto remainder = safeTick % gridTicks;
        return remainder * 2 >= gridTicks ? (quotient + 1) * gridTicks : quotient * gridTicks;
    }

    double midiPolyPressureTickToX(std::int64_t tick, std::int64_t viewStartTick, double ticksPerPixel) noexcept
    {
        return static_cast<double>(tick - std::max<std::int64_t>(0, viewStartTick)) / std::max(0.000001, ticksPerPixel);
    }

    std::int64_t midiPolyPressureXToTick(double x, std::int64_t viewStartTick, double ticksPerPixel) noexcept
    {
        return std::max<std::int64_t>(0, viewStartTick + static_cast<std::int64_t>(std::llround(std::max(0.0, x) * std::max(0.000001, ticksPerPixel))));
    }

    double midiPolyPressurePressureToY(int pressure, double top, double height) noexcept
    {
        const auto safeHeight = std::max(1.0, height);
        const auto safePressure = std::clamp(pressure, mw::core::midiPolyPressureMinimum, mw::core::midiPolyPressureMaximum);
        return top + (1.0 - static_cast<double>(safePressure) / 127.0) * safeHeight;
    }

    int midiPolyPressureYToPressure(double y, double top, double height) noexcept
    {
        const auto safeHeight = std::max(1.0, height);
        const auto ratio = std::clamp(1.0 - ((y - top) / safeHeight), 0.0, 1.0);
        return std::clamp(static_cast<int>(std::llround(ratio * 127.0)), mw::core::midiPolyPressureMinimum, mw::core::midiPolyPressureMaximum);
    }

    mw::core::StableId hitTestMidiPolyPressurePoint(const std::vector<mw::core::MidiPolyPressureEvent>& events,
                                                      std::int64_t viewStartTick, double ticksPerPixel, double top, double height,
                                                      double x, double y, double radiusPixels) noexcept
    {
        const auto radius = std::max(1.0, radiusPixels), radiusSquared = radius * radius;
        double bestDistance = radiusSquared + 1.0;
        mw::core::StableId bestId = 0;
        for (const auto& event : events)
        {
            if (!isValidMidiPolyPressureEvent(event)) continue;
            const auto dx = midiPolyPressureTickToX(event.tick, viewStartTick, ticksPerPixel) - x;
            const auto dy = midiPolyPressurePressureToY(event.pressure, top, height) - y;
            const auto distance = dx * dx + dy * dy;
            if (distance <= radiusSquared && (distance < bestDistance || (distance == bestDistance && (bestId == 0 || event.stableId < bestId))))
            { bestDistance = distance; bestId = event.stableId; }
        }
        return bestId;
    }

    std::vector<MidiPolyPressureStairStep> polyPressureStaircaseSegments(const mw::core::Track& track, int midiChannel, int noteNumber,
                                                                           std::int64_t viewStartTick, std::int64_t viewEndTick)
    {
        std::vector<MidiPolyPressureStairStep> result;
        if (track.isAudioClipTrack() || !validChannel(midiChannel) || !validNote(noteNumber) || viewEndTick <= viewStartTick) return result;
        const auto events = polyPressureLaneEvents(track, midiChannel, noteNumber);
        const mw::core::MidiPolyPressureEvent* active = nullptr;
        for (const auto& event : events) { if (event.tick <= viewStartTick) active = &event; else break; }
        if (active != nullptr)
        {
            auto next = std::find_if(events.begin(), events.end(), [&](const auto& e) { return e.tick > viewStartTick; });
            const auto endTick = next != events.end() ? std::min(viewEndTick, next->tick) : viewEndTick;
            if (endTick > viewStartTick) result.push_back({viewStartTick, endTick, active->pressure, active->stableId});
        }
        for (std::size_t i = 0; i < events.size(); ++i)
        {
            const auto& event = events[i]; if (event.tick < viewStartTick || event.tick >= viewEndTick) continue;
            const auto nextTick = i + 1 < events.size() ? events[i + 1].tick : viewEndTick;
            const auto endTick = std::min(viewEndTick, nextTick);
            if (endTick > event.tick) result.push_back({event.tick, endTick, event.pressure, event.stableId});
        }
        return result;
    }

    mw::core::Track* MidiPolyPressureEditHistory::findEditableMidiTrack(mw::core::Project& project, mw::core::StableId trackId)
    {
        for (auto& track : project.getTracks()) if (track.getStableId() == trackId) return track.isAudioClipTrack() ? nullptr : &track;
        return nullptr;
    }

    void MidiPolyPressureEditHistory::pushTransaction(Transaction transaction)
    {
        lastAffectedTrackId_ = transaction.trackId;
        undoStack_.push_back(std::move(transaction));
        if (undoStack_.size() > kMaximumPolyPressureUndoDepth)
            undoStack_.erase(undoStack_.begin());
        redoStack_.clear();
    }

    MidiPolyPressureEditResult MidiPolyPressureEditHistory::addAtTick(mw::core::Project& project, mw::core::StableId trackId, std::int64_t tick, int midiChannel, int noteNumber, int pressure)
    {
        MidiPolyPressureEditResult result;
        if (tick < 0 || !validChannel(midiChannel) || !validNote(noteNumber) || !validPressure(pressure)) return result;
        auto* track = findEditableMidiTrack(project, trackId); if (!track) return result;
        auto before = track->getMidiPolyPressureEvents(); auto after = before;
        auto existing = std::find_if(after.begin(), after.end(), [&](const auto& e) { return e.tick == tick && e.midiChannel == midiChannel && e.noteNumber == noteNumber; });
        if (existing != after.end())
        {
            result.eventId = existing->stableId;
            result.replacedExisting = true;
            if (existing->pressure == pressure) return result;
            existing->pressure = pressure;
        }
        else
        {
            mw::core::MidiPolyPressureEvent event(tick, midiChannel, noteNumber, pressure);
            result.eventId = event.stableId;
            after.push_back(event);
        }
        sortMidiPolyPressureEvents(after);
        track->setMidiPolyPressureEvents(after);
        pushTransaction({trackId, result.eventId, std::move(before), std::move(after)});
        result.changed = true;
        return result;
    }

    bool MidiPolyPressureEditHistory::moveToTick(mw::core::Project& project, mw::core::StableId trackId, mw::core::StableId eventId, std::int64_t tick)
    {
        if (tick < 0 || eventId == 0) return false;
        auto* track = findEditableMidiTrack(project, trackId); if (!track) return false;
        auto before = track->getMidiPolyPressureEvents(); auto after = before;
        auto it = findEventById(after, eventId); if (it == after.end() || it->tick == tick) return false;
        const int channel = it->midiChannel;
        const int note = it->noteNumber;
        after.erase(std::remove_if(after.begin(), after.end(), [&](const auto& e) { return e.stableId != eventId && e.tick == tick && e.midiChannel == channel && e.noteNumber == note; }), after.end());
        it = findEventById(after, eventId); if (it == after.end()) return false;
        it->tick = tick;
        sortMidiPolyPressureEvents(after);
        track->setMidiPolyPressureEvents(after);
        pushTransaction({trackId, eventId, std::move(before), std::move(after)});
        return true;
    }

    bool MidiPolyPressureEditHistory::moveAndSetPressure(mw::core::Project& project, mw::core::StableId trackId, mw::core::StableId eventId, std::int64_t tick, int pressure)
    {
        if (tick < 0 || eventId == 0 || !validPressure(pressure)) return false;
        auto* track = findEditableMidiTrack(project, trackId); if (!track) return false;
        auto before = track->getMidiPolyPressureEvents(); auto after = before;
        auto it = findEventById(after, eventId); if (it == after.end() || (it->tick == tick && it->pressure == pressure)) return false;
        const int channel = it->midiChannel;
        const int note = it->noteNumber;
        after.erase(std::remove_if(after.begin(), after.end(), [&](const auto& e) { return e.stableId != eventId && e.tick == tick && e.midiChannel == channel && e.noteNumber == note; }), after.end());
        it = findEventById(after, eventId); if (it == after.end()) return false;
        it->tick = tick;
        it->pressure = pressure;
        sortMidiPolyPressureEvents(after);
        track->setMidiPolyPressureEvents(after);
        pushTransaction({trackId, eventId, std::move(before), std::move(after)});
        return true;
    }

    bool MidiPolyPressureEditHistory::setPressure(mw::core::Project& project, mw::core::StableId trackId, mw::core::StableId eventId, int pressure)
    {
        if (eventId == 0 || !validPressure(pressure)) return false;
        auto* track = findEditableMidiTrack(project, trackId); if (!track) return false;
        auto before = track->getMidiPolyPressureEvents(); auto after = before;
        auto it = findEventById(after, eventId); if (it == after.end() || it->pressure == pressure) return false;
        it->pressure = pressure;
        track->setMidiPolyPressureEvents(after);
        pushTransaction({trackId, eventId, std::move(before), std::move(after)});
        return true;
    }

    bool MidiPolyPressureEditHistory::deleteEvent(mw::core::Project& project, mw::core::StableId trackId, mw::core::StableId eventId)
    {
        if (eventId == 0) return false;
        auto* track = findEditableMidiTrack(project, trackId); if (!track) return false;
        auto before = track->getMidiPolyPressureEvents(); auto after = before;
        const auto oldSize = after.size();
        after.erase(std::remove_if(after.begin(), after.end(), [&](const auto& e) { return e.stableId == eventId; }), after.end());
        if (after.size() == oldSize) return false;
        track->setMidiPolyPressureEvents(after);
        pushTransaction({trackId, eventId, std::move(before), std::move(after)});
        return true;
    }

    bool MidiPolyPressureEditHistory::restoreTransaction(mw::core::Project& project, const Transaction& transaction, bool useAfter)
    {
        auto* track = findEditableMidiTrack(project, transaction.trackId); if (!track) return false;
        track->setMidiPolyPressureEvents(useAfter ? transaction.after : transaction.before);
        return true;
    }

    bool MidiPolyPressureEditHistory::undo(mw::core::Project& project)
    {
        if (undoStack_.empty()) return false;
        auto transaction = undoStack_.back();
        if (!restoreTransaction(project, transaction, false)) return false;
        lastAffectedTrackId_ = transaction.trackId;
        undoStack_.pop_back();
        redoStack_.push_back(std::move(transaction));
        return true;
    }

    bool MidiPolyPressureEditHistory::redo(mw::core::Project& project)
    {
        if (redoStack_.empty()) return false;
        auto transaction = redoStack_.back();
        if (!restoreTransaction(project, transaction, true)) return false;
        lastAffectedTrackId_ = transaction.trackId;
        redoStack_.pop_back();
        undoStack_.push_back(std::move(transaction));
        return true;
    }

    void MidiPolyPressureEditHistory::clear() noexcept
    {
        undoStack_.clear(); redoStack_.clear(); lastAffectedTrackId_ = 0;
    }
}
