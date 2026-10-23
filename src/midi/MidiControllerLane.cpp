#include "midi/MidiControllerLane.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <sstream>
#include <utility>

namespace mw::midi
{
    namespace
    {
        constexpr std::size_t kMaximumControllerUndoDepth = 128;
        bool validChannel(int channel) noexcept { return channel >= 1 && channel <= 16; }
        bool validByte(int value) noexcept { return value >= 0 && value <= 127; }
        auto findEventById(std::vector<mw::core::MidiControllerEvent>& events, mw::core::StableId eventId)
        {
            return std::find_if(events.begin(), events.end(), [&](const auto& event) { return event.stableId == eventId; });
        }
    }

    std::string midiControllerDisplayName(int controllerNumber)
    {
        static constexpr std::array<const char*, 128> names {{
            "Bank Select MSB", "Modulation", "Breath", "", "Foot Controller", "Portamento Time", "Data Entry MSB", "Volume",
            "Balance", "", "Pan", "Expression", "Effect Control 1", "Effect Control 2", "", "",
            "General Purpose 1", "General Purpose 2", "General Purpose 3", "General Purpose 4", "", "", "", "",
            "", "", "", "", "", "", "", "",
            "Bank Select LSB", "Modulation LSB", "Breath LSB", "", "Foot Controller LSB", "Portamento Time LSB", "Data Entry LSB", "Volume LSB",
            "Balance LSB", "", "Pan LSB", "Expression LSB", "Effect Control 1 LSB", "Effect Control 2 LSB", "", "",
            "General Purpose 1 LSB", "General Purpose 2 LSB", "General Purpose 3 LSB", "General Purpose 4 LSB", "", "", "", "",
            "", "", "", "", "", "", "", "",
            "Sustain", "Portamento", "Sostenuto", "Soft Pedal", "Legato Footswitch", "Hold 2", "Sound Variation", "Resonance",
            "Release Time", "Attack Time", "Brightness", "Decay Time", "Vibrato Rate", "Vibrato Depth", "Vibrato Delay", "Sound Controller 10",
            "General Purpose 5", "General Purpose 6", "General Purpose 7", "General Purpose 8", "Portamento Control", "", "", "",
            "High Resolution Velocity Prefix", "", "", "Effects 1 Depth", "Effects 2 Depth", "Effects 3 Depth", "Effects 4 Depth", "Effects 5 Depth",
            "Data Increment", "Data Decrement", "NRPN LSB", "NRPN MSB", "RPN LSB", "RPN MSB", "", "",
            "", "", "", "", "", "", "", "",
            "", "", "", "", "", "", "", "",
            "All Sound Off", "Reset All Controllers", "Local Control", "All Notes Off", "Omni Off", "Omni On", "Mono Mode", "Poly Mode"
        }};
        const int safe = std::clamp(controllerNumber, 0, 127);
        std::ostringstream text;
        text << "CC " << safe;
        const auto* name = names[static_cast<std::size_t>(safe)];
        if (name != nullptr && *name != '\0') text << " - " << name;
        return text.str();
    }

    bool isValidMidiControllerEvent(const mw::core::MidiControllerEvent& event) noexcept
    {
        return event.stableId != 0 && event.tick >= 0 && validChannel(event.midiChannel) && validByte(event.controller) && validByte(event.value);
    }

    void sortMidiControllerEvents(std::vector<mw::core::MidiControllerEvent>& events)
    {
        std::sort(events.begin(), events.end(), mw::core::midiControllerEventOrder);
    }

    std::vector<mw::core::MidiControllerEvent> controllerLaneEvents(const mw::core::Track& track, int midiChannel, int controllerNumber)
    {
        std::vector<mw::core::MidiControllerEvent> result;
        if (track.isAudioClipTrack() || !validChannel(midiChannel) || !validByte(controllerNumber)) return result;
        for (const auto& event : track.getMidiControllerEvents())
            if (event.midiChannel == midiChannel && event.controller == controllerNumber) result.push_back(event);
        sortMidiControllerEvents(result);
        return result;
    }

    std::vector<mw::core::MidiControllerEvent> controllerStateAtOrBeforeTick(const mw::core::Track& track, std::int64_t startTick)
    {
        std::map<std::pair<int, int>, mw::core::MidiControllerEvent> latest;
        if (track.isAudioClipTrack() || startTick < 0) return {};
        for (const auto& event : track.getMidiControllerEvents())
        {
            if (!isValidMidiControllerEvent(event) || event.tick > startTick) continue;
            const auto key = std::make_pair(event.midiChannel, event.controller);
            const auto it = latest.find(key);
            if (it == latest.end() || event.tick > it->second.tick || (event.tick == it->second.tick && event.stableId > it->second.stableId))
                latest[key] = event;
        }
        std::vector<mw::core::MidiControllerEvent> result;
        result.reserve(latest.size());
        for (const auto& [key, event] : latest) { (void) key; result.push_back(event); }
        std::sort(result.begin(), result.end(), [](const auto& a, const auto& b)
        {
            if (a.midiChannel != b.midiChannel) return a.midiChannel < b.midiChannel;
            if (a.controller != b.controller) return a.controller < b.controller;
            return a.stableId < b.stableId;
        });
        return result;
    }

    std::size_t commitRecordedMidiControllerEvents(
        mw::core::Project& project,
        mw::core::StableId trackId,
        const std::vector<mw::core::PersistentMidiControllerEvent>& recordedEvents)
    {
        auto trackIt = std::find_if(project.getTracks().begin(), project.getTracks().end(), [&](const auto& track)
        {
            return track.getStableId() == trackId;
        });
        if (trackIt == project.getTracks().end() || trackIt->isAudioClipTrack())
            return 0;

        auto events = trackIt->getMidiControllerEvents();
        std::vector<mw::core::PersistentMidiControllerEvent> normalized;
        normalized.reserve(recordedEvents.size());
        for (const auto& event : recordedEvents)
        {
            if (event.tick < 0 || event.midiChannel < 1 || event.midiChannel > 16
                || event.controller < 0 || event.controller > 127
                || event.value < 0 || event.value > 127)
                continue;
            normalized.push_back(event);
        }
        std::stable_sort(normalized.begin(), normalized.end(), [](const auto& a, const auto& b)
        {
            if (a.tick != b.tick) return a.tick < b.tick;
            if (a.midiChannel != b.midiChannel) return a.midiChannel < b.midiChannel;
            if (a.controller != b.controller) return a.controller < b.controller;
            return a.order < b.order;
        });

        std::vector<mw::core::PersistentMidiControllerEvent> collapsed;
        for (const auto& event : normalized)
        {
            if (!collapsed.empty())
            {
                auto& previous = collapsed.back();
                if (previous.tick == event.tick && previous.midiChannel == event.midiChannel && previous.controller == event.controller)
                {
                    previous = event;
                    continue;
                }
            }
            collapsed.push_back(event);
        }

        std::size_t changed = 0;
        for (const auto& event : collapsed)
        {
            auto existing = std::find_if(events.begin(), events.end(), [&](const auto& current)
            {
                return current.tick == event.tick
                    && current.midiChannel == event.midiChannel
                    && current.controller == event.controller;
            });
            if (existing != events.end())
            {
                if (existing->value != event.value)
                {
                    existing->value = event.value;
                    ++changed;
                }
            }
            else
            {
                events.emplace_back(event.tick, event.midiChannel, event.controller, event.value);
                ++changed;
            }
        }
        if (changed > 0)
            trackIt->setMidiControllerEvents(std::move(events));
        return changed;
    }

    bool midiControllerEventVectorsEqual(
        const std::vector<mw::core::MidiControllerEvent>& left,
        const std::vector<mw::core::MidiControllerEvent>& right) noexcept
    {
        if (left.size() != right.size())
            return false;
        for (std::size_t i = 0; i < left.size(); ++i)
        {
            const auto& a = left[i];
            const auto& b = right[i];
            if (a.stableId != b.stableId || a.tick != b.tick || a.midiChannel != b.midiChannel
                || a.controller != b.controller || a.value != b.value)
                return false;
        }
        return true;
    }

    std::int64_t midiControllerSnapGridTicks(int ticksPerQuarterNote, MidiControllerSnapGrid grid) noexcept
    {
        const auto ppq = std::max(1, ticksPerQuarterNote);
        switch (grid)
        {
            case MidiControllerSnapGrid::Quarter: return ppq;
            case MidiControllerSnapGrid::Eighth: return std::max<std::int64_t>(1, ppq / 2);
            case MidiControllerSnapGrid::Sixteenth: return std::max<std::int64_t>(1, ppq / 4);
            case MidiControllerSnapGrid::ThirtySecond: return std::max<std::int64_t>(1, ppq / 8);
            case MidiControllerSnapGrid::SixtyFourth: return std::max<std::int64_t>(1, ppq / 16);
            case MidiControllerSnapGrid::Off:
            default: return 1;
        }
    }

    std::int64_t snapMidiControllerTick(std::int64_t rawTick, int ticksPerQuarterNote, MidiControllerSnapGrid grid) noexcept
    {
        const auto safeTick = std::max<std::int64_t>(0, rawTick);
        if (grid == MidiControllerSnapGrid::Off) return safeTick;
        const auto gridTicks = midiControllerSnapGridTicks(ticksPerQuarterNote, grid);
        const auto quotient = safeTick / gridTicks;
        const auto remainder = safeTick % gridTicks;
        return (remainder * 2 >= gridTicks) ? (quotient + 1) * gridTicks : quotient * gridTicks;
    }

    double midiControllerTickToX(std::int64_t tick, std::int64_t viewStartTick, double ticksPerPixel) noexcept
    {
        const auto scale = std::max(0.000001, ticksPerPixel);
        return static_cast<double>(tick - viewStartTick) / scale;
    }

    std::int64_t midiControllerXToTick(double x, std::int64_t viewStartTick, double ticksPerPixel) noexcept
    {
        const auto scale = std::max(0.000001, ticksPerPixel);
        const auto raw = static_cast<double>(viewStartTick) + std::max(0.0, x) * scale;
        if (raw >= static_cast<double>(std::numeric_limits<std::int64_t>::max()))
            return std::numeric_limits<std::int64_t>::max();
        return std::max<std::int64_t>(0, static_cast<std::int64_t>(std::llround(raw)));
    }

    double midiControllerValueToY(int value, double top, double height) noexcept
    {
        const auto safeHeight = std::max(1.0, height);
        const auto safeValue = std::clamp(value, 0, 127);
        return top + (1.0 - static_cast<double>(safeValue) / 127.0) * safeHeight;
    }

    int midiControllerYToValue(double y, double top, double height) noexcept
    {
        const auto safeHeight = std::max(1.0, height);
        const auto ratio = 1.0 - ((y - top) / safeHeight);
        return std::clamp(static_cast<int>(std::llround(ratio * 127.0)), 0, 127);
    }

    mw::core::StableId hitTestMidiControllerPoint(const std::vector<mw::core::MidiControllerEvent>& events,
                                                   std::int64_t viewStartTick, double ticksPerPixel, double top, double height,
                                                   double x, double y, double radiusPixels) noexcept
    {
        const auto radius = std::max(1.0, radiusPixels);
        const auto radiusSquared = radius * radius;
        double bestDistance = radiusSquared + 1.0;
        mw::core::StableId bestId = 0;
        for (const auto& event : events)
        {
            if (!isValidMidiControllerEvent(event)) continue;
            const auto eventX = midiControllerTickToX(event.tick, viewStartTick, ticksPerPixel);
            const auto eventY = midiControllerValueToY(event.value, top, height);
            const auto dx = eventX - x;
            const auto dy = eventY - y;
            const auto distance = dx * dx + dy * dy;
            if (distance <= radiusSquared && (distance < bestDistance || (distance == bestDistance && event.stableId < bestId)))
            {
                bestDistance = distance;
                bestId = event.stableId;
            }
        }
        return bestId;
    }

    std::vector<MidiControllerStairStep> controllerStaircaseSegments(const mw::core::Track& track, int midiChannel, int controllerNumber,
                                                                     std::int64_t viewStartTick, std::int64_t viewEndTick)
    {
        std::vector<MidiControllerStairStep> result;
        if (track.isAudioClipTrack() || !validChannel(midiChannel) || !validByte(controllerNumber) || viewEndTick <= viewStartTick)
            return result;

        const auto events = controllerLaneEvents(track, midiChannel, controllerNumber);
        const mw::core::MidiControllerEvent* active = nullptr;
        for (const auto& event : events)
        {
            if (event.tick <= viewStartTick) active = &event;
            else break;
        }

        std::int64_t cursor = viewStartTick;
        if (active != nullptr)
        {
            auto next = std::find_if(events.begin(), events.end(), [&](const auto& event) { return event.tick > viewStartTick; });
            const auto end = next != events.end() ? std::min(viewEndTick, next->tick) : viewEndTick;
            if (end > cursor) result.push_back({ cursor, end, active->value, active->stableId });
            cursor = end;
        }

        for (std::size_t i = 0; i < events.size(); ++i)
        {
            const auto& event = events[i];
            if (event.tick < viewStartTick || event.tick >= viewEndTick) continue;
            const auto nextTick = (i + 1 < events.size()) ? events[i + 1].tick : viewEndTick;
            const auto end = std::min(viewEndTick, nextTick);
            if (end > event.tick) result.push_back({ event.tick, end, event.value, event.stableId });
        }
        return result;
    }

    bool midiControllerLaneUiActionMayDirty(MidiControllerLaneUiAction action) noexcept
    {
        switch (action)
        {
            case MidiControllerLaneUiAction::AddAtPreview:
            case MidiControllerLaneUiAction::MoveToPreview:
            case MidiControllerLaneUiAction::SetValue:
            case MidiControllerLaneUiAction::DeleteEvent:
            case MidiControllerLaneUiAction::UndoCc:
            case MidiControllerLaneUiAction::RedoCc:
            case MidiControllerLaneUiAction::GraphDragCommit:
                return true;
            case MidiControllerLaneUiAction::OpenWindow:
            case MidiControllerLaneUiAction::CloseWindow:
            case MidiControllerLaneUiAction::SelectTrack:
            case MidiControllerLaneUiAction::SelectController:
            case MidiControllerLaneUiAction::SelectChannel:
            case MidiControllerLaneUiAction::SelectEvent:
            case MidiControllerLaneUiAction::Scroll:
            case MidiControllerLaneUiAction::HoverHelp:
            case MidiControllerLaneUiAction::SelectGraphPoint:
            case MidiControllerLaneUiAction::PanGraph:
            case MidiControllerLaneUiAction::ZoomGraph:
            case MidiControllerLaneUiAction::SelectSnap:
            case MidiControllerLaneUiAction::CancelGraphDrag:
            default:
                return false;
        }
    }

    mw::core::StableId reconcileMidiControllerLaneSelection(const mw::core::Track& track, int midiChannel, int controllerNumber, mw::core::StableId selectedEventId) noexcept
    {
        if (track.isAudioClipTrack() || selectedEventId == 0 || !validChannel(midiChannel) || !validByte(controllerNumber))
            return 0;
        const auto& events = track.getMidiControllerEvents();
        const auto found = std::find_if(events.begin(), events.end(), [&](const auto& event)
        {
            return event.stableId == selectedEventId && event.midiChannel == midiChannel && event.controller == controllerNumber;
        });
        return found != events.end() ? selectedEventId : 0;
    }

    mw::core::Track* MidiControllerEditHistory::findEditableMidiTrack(mw::core::Project& project, mw::core::StableId trackId)
    {
        for (auto& track : project.getTracks()) if (track.getStableId() == trackId) return track.isAudioClipTrack() ? nullptr : &track;
        return nullptr;
    }

    void MidiControllerEditHistory::pushTransaction(Transaction transaction)
    {
        lastAffectedTrackId_ = transaction.trackId;
        undoStack_.push_back(std::move(transaction));
        if (undoStack_.size() > kMaximumControllerUndoDepth) undoStack_.erase(undoStack_.begin());
        redoStack_.clear();
    }

    MidiControllerEditResult MidiControllerEditHistory::addAtTick(mw::core::Project& project, mw::core::StableId trackId, std::int64_t tick, int midiChannel, int controllerNumber, int value)
    {
        MidiControllerEditResult result;
        if (tick < 0 || !validChannel(midiChannel) || !validByte(controllerNumber) || !validByte(value)) return result;
        auto* track = findEditableMidiTrack(project, trackId); if (!track) return result;
        auto before = track->getMidiControllerEvents(); auto after = before;
        auto existing = std::find_if(after.begin(), after.end(), [&](const auto& e){ return e.tick == tick && e.midiChannel == midiChannel && e.controller == controllerNumber; });
        if (existing != after.end())
        {
            result.eventId = existing->stableId; result.replacedExisting = true;
            if (existing->value == value) return result;
            existing->value = value;
        }
        else
        {
            mw::core::MidiControllerEvent event(tick, midiChannel, controllerNumber, value);
            result.eventId = event.stableId; after.push_back(event);
        }
        sortMidiControllerEvents(after); track->setMidiControllerEvents(after);
        pushTransaction({ trackId, result.eventId, std::move(before), std::move(after) });
        result.changed = true; return result;
    }

    bool MidiControllerEditHistory::moveToTick(mw::core::Project& project, mw::core::StableId trackId, mw::core::StableId eventId, std::int64_t tick)
    {
        if (tick < 0 || eventId == 0) return false;
        auto* track = findEditableMidiTrack(project, trackId);
        if (!track) return false;
        auto before = track->getMidiControllerEvents(); auto after = before; auto it = findEventById(after, eventId); if (it == after.end() || it->tick == tick) return false;
        const int channel = it->midiChannel, controller = it->controller;
        after.erase(std::remove_if(after.begin(), after.end(), [&](const auto& e){ return e.stableId != eventId && e.tick == tick && e.midiChannel == channel && e.controller == controller; }), after.end());
        it = findEventById(after, eventId); if (it == after.end()) return false; it->tick = tick; sortMidiControllerEvents(after); track->setMidiControllerEvents(after);
        pushTransaction({ trackId, eventId, std::move(before), std::move(after) }); return true;
    }

    bool MidiControllerEditHistory::moveAndSetValue(mw::core::Project& project, mw::core::StableId trackId, mw::core::StableId eventId, std::int64_t tick, int value)
    {
        if (tick < 0 || eventId == 0 || !validByte(value)) return false;
        auto* track = findEditableMidiTrack(project, trackId);
        if (!track) return false;
        auto before = track->getMidiControllerEvents();
        auto after = before;
        auto it = findEventById(after, eventId);
        if (it == after.end() || (it->tick == tick && it->value == value)) return false;
        const int channel = it->midiChannel;
        const int controller = it->controller;
        after.erase(std::remove_if(after.begin(), after.end(), [&](const auto& event)
        {
            return event.stableId != eventId && event.tick == tick && event.midiChannel == channel && event.controller == controller;
        }), after.end());
        it = findEventById(after, eventId);
        if (it == after.end()) return false;
        it->tick = tick;
        it->value = value;
        sortMidiControllerEvents(after);
        track->setMidiControllerEvents(after);
        pushTransaction({ trackId, eventId, std::move(before), std::move(after) });
        return true;
    }

    bool MidiControllerEditHistory::setValue(mw::core::Project& project, mw::core::StableId trackId, mw::core::StableId eventId, int value)
    {
        if (eventId == 0 || !validByte(value)) return false;
        auto* track = findEditableMidiTrack(project, trackId);
        if (!track) return false;
        auto before = track->getMidiControllerEvents(); auto after = before; auto it = findEventById(after, eventId); if (it == after.end() || it->value == value) return false;
        it->value = value; track->setMidiControllerEvents(after); pushTransaction({ trackId, eventId, std::move(before), std::move(after) }); return true;
    }

    bool MidiControllerEditHistory::deleteEvent(mw::core::Project& project, mw::core::StableId trackId, mw::core::StableId eventId)
    {
        if (eventId == 0) return false;
        auto* track = findEditableMidiTrack(project, trackId);
        if (!track) return false;
        auto before = track->getMidiControllerEvents(); auto after = before; const auto oldSize = after.size();
        after.erase(std::remove_if(after.begin(), after.end(), [&](const auto& e){ return e.stableId == eventId; }), after.end()); if (after.size() == oldSize) return false;
        track->setMidiControllerEvents(after); pushTransaction({ trackId, eventId, std::move(before), std::move(after) }); return true;
    }

    bool MidiControllerEditHistory::restoreTransaction(mw::core::Project& project, const Transaction& transaction, bool useAfter)
    {
        auto* track = findEditableMidiTrack(project, transaction.trackId); if (!track) return false;
        track->setMidiControllerEvents(useAfter ? transaction.after : transaction.before); return true;
    }

    bool MidiControllerEditHistory::undo(mw::core::Project& project)
    {
        if (undoStack_.empty()) return false;
        auto transaction = undoStack_.back();
        if (!restoreTransaction(project, transaction, false)) return false;
        lastAffectedTrackId_ = transaction.trackId; undoStack_.pop_back(); redoStack_.push_back(std::move(transaction)); return true;
    }

    bool MidiControllerEditHistory::redo(mw::core::Project& project)
    {
        if (redoStack_.empty()) return false;
        auto transaction = redoStack_.back();
        if (!restoreTransaction(project, transaction, true)) return false;
        lastAffectedTrackId_ = transaction.trackId; redoStack_.pop_back(); undoStack_.push_back(std::move(transaction)); return true;
    }

    void MidiControllerEditHistory::clear() noexcept { undoStack_.clear(); redoStack_.clear(); lastAffectedTrackId_ = 0; }
}
