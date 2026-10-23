#include "midi/MidiProgramChangeLane.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>

namespace mw::midi
{
    namespace
    {
        constexpr std::size_t kMaximumProgramChangeUndoDepth = 128;
        bool validChannel(int channel) noexcept { return channel >= 1 && channel <= 16; }
        bool validProgram(int program) noexcept { return program >= mw::core::midiProgramChangeMinimum && program <= mw::core::midiProgramChangeMaximum; }
        auto findEventById(std::vector<mw::core::MidiProgramChangeEvent>& events, mw::core::StableId eventId)
        {
            return std::find_if(events.begin(), events.end(), [&](const auto& event) { return event.stableId == eventId; });
        }
    }

    bool isValidMidiProgramChangeEvent(const mw::core::MidiProgramChangeEvent& event) noexcept
    {
        return event.stableId != 0 && event.tick >= 0 && validChannel(event.midiChannel) && validProgram(event.program);
    }

    void sortMidiProgramChangeEvents(std::vector<mw::core::MidiProgramChangeEvent>& events)
    {
        std::sort(events.begin(), events.end(), mw::core::midiProgramChangeEventOrder);
    }

    std::vector<mw::core::MidiProgramChangeEvent> programChangeLaneEvents(const mw::core::Track& track, int midiChannel)
    {
        std::vector<mw::core::MidiProgramChangeEvent> result;
        if (track.isAudioClipTrack() || !validChannel(midiChannel)) return result;
        for (const auto& event : track.getMidiProgramChangeEvents()) if (event.midiChannel == midiChannel) result.push_back(event);
        sortMidiProgramChangeEvents(result);
        return result;
    }

    std::vector<mw::core::MidiProgramChangeEvent> programChangeStateAtOrBeforeTick(const mw::core::Track& track, std::int64_t startTick)
    {
        std::map<int, mw::core::MidiProgramChangeEvent> latest;
        if (track.isAudioClipTrack() || startTick < 0) return {};
        for (const auto& event : track.getMidiProgramChangeEvents())
        {
            if (!isValidMidiProgramChangeEvent(event) || event.tick > startTick) continue;
            const auto it = latest.find(event.midiChannel);
            if (it == latest.end() || event.tick > it->second.tick || (event.tick == it->second.tick && event.stableId > it->second.stableId))
                latest[event.midiChannel] = event;
        }
        std::vector<mw::core::MidiProgramChangeEvent> result;
        result.reserve(latest.size());
        for (const auto& [channel, event] : latest) { (void) channel; result.push_back(event); }
        return result;
    }

    std::size_t commitRecordedMidiProgramChangeEvents(mw::core::Project& project, mw::core::StableId trackId,
                                                       const std::vector<mw::core::PersistentMidiProgramChangeEvent>& recordedEvents)
    {
        auto trackIt = std::find_if(project.getTracks().begin(), project.getTracks().end(), [&](const auto& track) { return track.getStableId() == trackId; });
        if (trackIt == project.getTracks().end() || trackIt->isAudioClipTrack()) return 0;
        auto events = trackIt->getMidiProgramChangeEvents();
        std::vector<mw::core::PersistentMidiProgramChangeEvent> normalized;
        for (const auto& event : recordedEvents)
            if (event.tick >= 0 && validChannel(event.midiChannel) && validProgram(event.program)) normalized.push_back(event);
        std::stable_sort(normalized.begin(), normalized.end(), [](const auto& a, const auto& b)
        {
            if (a.tick != b.tick) return a.tick < b.tick;
            if (a.midiChannel != b.midiChannel) return a.midiChannel < b.midiChannel;
            return a.order < b.order;
        });
        std::vector<mw::core::PersistentMidiProgramChangeEvent> collapsed;
        for (const auto& event : normalized)
        {
            if (!collapsed.empty() && collapsed.back().tick == event.tick && collapsed.back().midiChannel == event.midiChannel)
                collapsed.back() = event;
            else
                collapsed.push_back(event);
        }
        std::size_t changed = 0;
        for (const auto& event : collapsed)
        {
            auto existing = std::find_if(events.begin(), events.end(), [&](const auto& current)
            { return current.tick == event.tick && current.midiChannel == event.midiChannel; });
            if (existing != events.end())
            {
                if (existing->program != event.program) { existing->program = event.program; ++changed; }
            }
            else
            {
                events.emplace_back(event.tick, event.midiChannel, event.program); ++changed;
            }
        }
        if (changed > 0) trackIt->setMidiProgramChangeEvents(std::move(events));
        return changed;
    }

    bool midiProgramChangeEventVectorsEqual(const std::vector<mw::core::MidiProgramChangeEvent>& left,
                                             const std::vector<mw::core::MidiProgramChangeEvent>& right) noexcept
    {
        if (left.size() != right.size()) return false;
        for (std::size_t i = 0; i < left.size(); ++i)
        {
            const auto& a = left[i]; const auto& b = right[i];
            if (a.stableId != b.stableId || a.tick != b.tick || a.midiChannel != b.midiChannel || a.program != b.program) return false;
        }
        return true;
    }

    std::int64_t midiProgramChangeSnapGridTicks(int ticksPerQuarterNote, MidiProgramChangeSnapGrid grid) noexcept
    {
        const auto ppq = std::max(1, ticksPerQuarterNote);
        switch (grid)
        {
            case MidiProgramChangeSnapGrid::Quarter: return ppq;
            case MidiProgramChangeSnapGrid::Eighth: return std::max<std::int64_t>(1, ppq / 2);
            case MidiProgramChangeSnapGrid::Sixteenth: return std::max<std::int64_t>(1, ppq / 4);
            case MidiProgramChangeSnapGrid::ThirtySecond: return std::max<std::int64_t>(1, ppq / 8);
            case MidiProgramChangeSnapGrid::SixtyFourth: return std::max<std::int64_t>(1, ppq / 16);
            case MidiProgramChangeSnapGrid::Off: default: return 1;
        }
    }

    std::int64_t snapMidiProgramChangeTick(std::int64_t rawTick, int ticksPerQuarterNote, MidiProgramChangeSnapGrid grid) noexcept
    {
        const auto safeTick = std::max<std::int64_t>(0, rawTick);
        if (grid == MidiProgramChangeSnapGrid::Off) return safeTick;
        const auto gridTicks = midiProgramChangeSnapGridTicks(ticksPerQuarterNote, grid);
        const auto quotient = safeTick / gridTicks;
        const auto remainder = safeTick % gridTicks;
        return remainder * 2 >= gridTicks ? (quotient + 1) * gridTicks : quotient * gridTicks;
    }

    double midiProgramChangeTickToX(std::int64_t tick, std::int64_t viewStartTick, double ticksPerPixel) noexcept
    {
        return static_cast<double>(tick - std::max<std::int64_t>(0, viewStartTick)) / std::max(0.000001, ticksPerPixel);
    }

    std::int64_t midiProgramChangeXToTick(double x, std::int64_t viewStartTick, double ticksPerPixel) noexcept
    {
        return std::max<std::int64_t>(0, viewStartTick + static_cast<std::int64_t>(std::llround(std::max(0.0, x) * std::max(0.000001, ticksPerPixel))));
    }

    double midiProgramChangeProgramToY(int program, double top, double height) noexcept
    {
        const auto safeHeight = std::max(1.0, height);
        const auto safeProgram = std::clamp(program, mw::core::midiProgramChangeMinimum, mw::core::midiProgramChangeMaximum);
        return top + (1.0 - static_cast<double>(safeProgram) / 127.0) * safeHeight;
    }

    int midiProgramChangeYToProgram(double y, double top, double height) noexcept
    {
        const auto safeHeight = std::max(1.0, height);
        const auto ratio = std::clamp(1.0 - ((y - top) / safeHeight), 0.0, 1.0);
        return std::clamp(static_cast<int>(std::llround(ratio * 127.0)), mw::core::midiProgramChangeMinimum, mw::core::midiProgramChangeMaximum);
    }

    mw::core::StableId hitTestMidiProgramChangePoint(const std::vector<mw::core::MidiProgramChangeEvent>& events,
                                                      std::int64_t viewStartTick, double ticksPerPixel, double top, double height,
                                                      double x, double y, double radiusPixels) noexcept
    {
        const auto radius = std::max(1.0, radiusPixels), radiusSquared = radius * radius;
        double bestDistance = radiusSquared + 1.0;
        mw::core::StableId bestId = 0;
        for (const auto& event : events)
        {
            if (!isValidMidiProgramChangeEvent(event)) continue;
            const auto dx = midiProgramChangeTickToX(event.tick, viewStartTick, ticksPerPixel) - x;
            const auto dy = midiProgramChangeProgramToY(event.program, top, height) - y;
            const auto distance = dx * dx + dy * dy;
            if (distance <= radiusSquared && (distance < bestDistance || (distance == bestDistance && (bestId == 0 || event.stableId < bestId))))
            { bestDistance = distance; bestId = event.stableId; }
        }
        return bestId;
    }

    std::vector<MidiProgramChangeStairStep> programChangeStaircaseSegments(const mw::core::Track& track, int midiChannel,
                                                                           std::int64_t viewStartTick, std::int64_t viewEndTick)
    {
        std::vector<MidiProgramChangeStairStep> result;
        if (track.isAudioClipTrack() || !validChannel(midiChannel) || viewEndTick <= viewStartTick) return result;
        const auto events = programChangeLaneEvents(track, midiChannel);
        const mw::core::MidiProgramChangeEvent* active = nullptr;
        for (const auto& event : events) { if (event.tick <= viewStartTick) active = &event; else break; }
        if (active != nullptr)
        {
            auto next = std::find_if(events.begin(), events.end(), [&](const auto& e) { return e.tick > viewStartTick; });
            const auto endTick = next != events.end() ? std::min(viewEndTick, next->tick) : viewEndTick;
            if (endTick > viewStartTick) result.push_back({viewStartTick, endTick, active->program, active->stableId});
        }
        for (std::size_t i = 0; i < events.size(); ++i)
        {
            const auto& event = events[i]; if (event.tick < viewStartTick || event.tick >= viewEndTick) continue;
            const auto nextTick = i + 1 < events.size() ? events[i + 1].tick : viewEndTick;
            const auto endTick = std::min(viewEndTick, nextTick);
            if (endTick > event.tick) result.push_back({event.tick, endTick, event.program, event.stableId});
        }
        return result;
    }

    mw::core::Track* MidiProgramChangeEditHistory::findEditableMidiTrack(mw::core::Project& project, mw::core::StableId trackId)
    {
        for (auto& track : project.getTracks()) if (track.getStableId() == trackId) return track.isAudioClipTrack() ? nullptr : &track;
        return nullptr;
    }

    void MidiProgramChangeEditHistory::pushTransaction(Transaction transaction)
    {
        lastAffectedTrackId_ = transaction.trackId;
        undoStack_.push_back(std::move(transaction));
        if (undoStack_.size() > kMaximumProgramChangeUndoDepth)
            undoStack_.erase(undoStack_.begin());
        redoStack_.clear();
    }

    MidiProgramChangeEditResult MidiProgramChangeEditHistory::addAtTick(mw::core::Project& project, mw::core::StableId trackId, std::int64_t tick, int midiChannel, int program)
    {
        MidiProgramChangeEditResult result;
        if (tick < 0 || !validChannel(midiChannel) || !validProgram(program)) return result;
        auto* track = findEditableMidiTrack(project, trackId); if (!track) return result;
        auto before = track->getMidiProgramChangeEvents(); auto after = before;
        auto existing = std::find_if(after.begin(), after.end(), [&](const auto& e) { return e.tick == tick && e.midiChannel == midiChannel; });
        if (existing != after.end())
        {
            result.eventId = existing->stableId;
            result.replacedExisting = true;
            if (existing->program == program) return result;
            existing->program = program;
        }
        else
        {
            mw::core::MidiProgramChangeEvent event(tick, midiChannel, program);
            result.eventId = event.stableId;
            after.push_back(event);
        }
        sortMidiProgramChangeEvents(after);
        track->setMidiProgramChangeEvents(after);
        pushTransaction({trackId, result.eventId, std::move(before), std::move(after)});
        result.changed = true;
        return result;
    }

    bool MidiProgramChangeEditHistory::moveToTick(mw::core::Project& project, mw::core::StableId trackId, mw::core::StableId eventId, std::int64_t tick)
    {
        if (tick < 0 || eventId == 0) return false;
        auto* track = findEditableMidiTrack(project, trackId); if (!track) return false;
        auto before = track->getMidiProgramChangeEvents(); auto after = before;
        auto it = findEventById(after, eventId); if (it == after.end() || it->tick == tick) return false;
        const int channel = it->midiChannel;
        after.erase(std::remove_if(after.begin(), after.end(), [&](const auto& e) { return e.stableId != eventId && e.tick == tick && e.midiChannel == channel; }), after.end());
        it = findEventById(after, eventId); if (it == after.end()) return false;
        it->tick = tick;
        sortMidiProgramChangeEvents(after);
        track->setMidiProgramChangeEvents(after);
        pushTransaction({trackId, eventId, std::move(before), std::move(after)});
        return true;
    }

    bool MidiProgramChangeEditHistory::moveAndSetProgram(mw::core::Project& project, mw::core::StableId trackId, mw::core::StableId eventId, std::int64_t tick, int program)
    {
        if (tick < 0 || eventId == 0 || !validProgram(program)) return false;
        auto* track = findEditableMidiTrack(project, trackId); if (!track) return false;
        auto before = track->getMidiProgramChangeEvents(); auto after = before;
        auto it = findEventById(after, eventId); if (it == after.end() || (it->tick == tick && it->program == program)) return false;
        const int channel = it->midiChannel;
        after.erase(std::remove_if(after.begin(), after.end(), [&](const auto& e) { return e.stableId != eventId && e.tick == tick && e.midiChannel == channel; }), after.end());
        it = findEventById(after, eventId); if (it == after.end()) return false;
        it->tick = tick;
        it->program = program;
        sortMidiProgramChangeEvents(after);
        track->setMidiProgramChangeEvents(after);
        pushTransaction({trackId, eventId, std::move(before), std::move(after)});
        return true;
    }

    bool MidiProgramChangeEditHistory::setProgram(mw::core::Project& project, mw::core::StableId trackId, mw::core::StableId eventId, int program)
    {
        if (eventId == 0 || !validProgram(program)) return false;
        auto* track = findEditableMidiTrack(project, trackId); if (!track) return false;
        auto before = track->getMidiProgramChangeEvents(); auto after = before;
        auto it = findEventById(after, eventId); if (it == after.end() || it->program == program) return false;
        it->program = program;
        track->setMidiProgramChangeEvents(after);
        pushTransaction({trackId, eventId, std::move(before), std::move(after)});
        return true;
    }

    bool MidiProgramChangeEditHistory::deleteEvent(mw::core::Project& project, mw::core::StableId trackId, mw::core::StableId eventId)
    {
        if (eventId == 0) return false;
        auto* track = findEditableMidiTrack(project, trackId); if (!track) return false;
        auto before = track->getMidiProgramChangeEvents(); auto after = before;
        const auto oldSize = after.size();
        after.erase(std::remove_if(after.begin(), after.end(), [&](const auto& e) { return e.stableId == eventId; }), after.end());
        if (after.size() == oldSize) return false;
        track->setMidiProgramChangeEvents(after);
        pushTransaction({trackId, eventId, std::move(before), std::move(after)});
        return true;
    }

    bool MidiProgramChangeEditHistory::restoreTransaction(mw::core::Project& project, const Transaction& transaction, bool useAfter)
    {
        auto* track = findEditableMidiTrack(project, transaction.trackId); if (!track) return false;
        track->setMidiProgramChangeEvents(useAfter ? transaction.after : transaction.before);
        return true;
    }

    bool MidiProgramChangeEditHistory::undo(mw::core::Project& project)
    {
        if (undoStack_.empty()) return false;
        auto transaction = undoStack_.back();
        if (!restoreTransaction(project, transaction, false)) return false;
        lastAffectedTrackId_ = transaction.trackId;
        undoStack_.pop_back();
        redoStack_.push_back(std::move(transaction));
        return true;
    }

    bool MidiProgramChangeEditHistory::redo(mw::core::Project& project)
    {
        if (redoStack_.empty()) return false;
        auto transaction = redoStack_.back();
        if (!restoreTransaction(project, transaction, true)) return false;
        lastAffectedTrackId_ = transaction.trackId;
        redoStack_.pop_back();
        undoStack_.push_back(std::move(transaction));
        return true;
    }

    void MidiProgramChangeEditHistory::clear() noexcept
    {
        undoStack_.clear(); redoStack_.clear(); lastAffectedTrackId_ = 0;
    }
}
