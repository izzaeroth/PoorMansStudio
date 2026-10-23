#include "midi/MidiRecordingTakeReview.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace mw::midi
{
    namespace
    {
        std::int64_t safeNoteEnd(const mw::core::NoteEvent& note) noexcept
        {
            if (note.durationTicks <= 0)
                return note.startTick;
            if (note.startTick > std::numeric_limits<std::int64_t>::max() - note.durationTicks)
                return std::numeric_limits<std::int64_t>::max();
            return note.startTick + note.durationTicks;
        }

        bool sustainDownFromMask(std::uint16_t mask, int midiChannel) noexcept
        {
            if (midiChannel < 1 || midiChannel > 16)
                return false;
            return (mask & static_cast<std::uint16_t>(1u << static_cast<unsigned>(midiChannel - 1))) != 0;
        }

        bool cc64Less(const MidiRecordingCc64Event& lhs, const MidiRecordingCc64Event& rhs) noexcept
        {
            if (lhs.tick != rhs.tick)
                return lhs.tick < rhs.tick;
            return lhs.order < rhs.order;
        }

        bool controllerLess(const MidiRecordingControllerEvent& lhs, const MidiRecordingControllerEvent& rhs) noexcept
        {
            if (lhs.tick != rhs.tick) return lhs.tick < rhs.tick;
            if (lhs.midiChannel != rhs.midiChannel) return lhs.midiChannel < rhs.midiChannel;
            if (lhs.controller != rhs.controller) return lhs.controller < rhs.controller;
            return lhs.order < rhs.order;
        }

        bool validControllerEvent(const MidiRecordingControllerEvent& event) noexcept
        {
            return event.tick >= 0
                && event.midiChannel >= 1 && event.midiChannel <= 16
                && event.controller >= 0 && event.controller <= 127
                && event.value >= 0 && event.value <= 127;
        }

        bool pitchBendLess(const MidiRecordingPitchBendEvent& lhs, const MidiRecordingPitchBendEvent& rhs) noexcept
        {
            if (lhs.tick != rhs.tick) return lhs.tick < rhs.tick;
            if (lhs.midiChannel != rhs.midiChannel) return lhs.midiChannel < rhs.midiChannel;
            return lhs.order < rhs.order;
        }

        bool validPitchBendEvent(const MidiRecordingPitchBendEvent& event) noexcept
        {
            return event.tick >= 0
                && event.midiChannel >= 1 && event.midiChannel <= 16
                && event.value >= mw::core::midiPitchBendMinimum
                && event.value <= mw::core::midiPitchBendMaximum;
        }

        bool programChangeLess(const MidiRecordingProgramChangeEvent& lhs, const MidiRecordingProgramChangeEvent& rhs) noexcept
        {
            if (lhs.tick != rhs.tick) return lhs.tick < rhs.tick;
            if (lhs.midiChannel != rhs.midiChannel) return lhs.midiChannel < rhs.midiChannel;
            return lhs.order < rhs.order;
        }

        bool validProgramChangeEvent(const MidiRecordingProgramChangeEvent& event) noexcept
        {
            return event.tick >= 0
                && event.midiChannel >= 1 && event.midiChannel <= 16
                && event.program >= mw::core::midiProgramChangeMinimum
                && event.program <= mw::core::midiProgramChangeMaximum;
        }

        bool channelPressureLess(const MidiRecordingChannelPressureEvent& lhs, const MidiRecordingChannelPressureEvent& rhs) noexcept
        {
            if (lhs.tick != rhs.tick) return lhs.tick < rhs.tick;
            if (lhs.midiChannel != rhs.midiChannel) return lhs.midiChannel < rhs.midiChannel;
            return lhs.order < rhs.order;
        }

        bool validChannelPressureEvent(const MidiRecordingChannelPressureEvent& event) noexcept
        {
            return event.tick >= 0
                && event.midiChannel >= 1 && event.midiChannel <= 16
                && event.pressure >= mw::core::midiChannelPressureMinimum
                && event.pressure <= mw::core::midiChannelPressureMaximum;
        }

        bool polyPressureLess(const MidiRecordingPolyPressureEvent& lhs, const MidiRecordingPolyPressureEvent& rhs) noexcept
        {
            if (lhs.tick != rhs.tick) return lhs.tick < rhs.tick;
            if (lhs.midiChannel != rhs.midiChannel) return lhs.midiChannel < rhs.midiChannel;
            if (lhs.noteNumber != rhs.noteNumber) return lhs.noteNumber < rhs.noteNumber;
            return lhs.order < rhs.order;
        }

        bool validPolyPressureEvent(const MidiRecordingPolyPressureEvent& event) noexcept
        {
            return event.tick >= 0
                && event.midiChannel >= 1 && event.midiChannel <= 16
                && event.noteNumber >= 0 && event.noteNumber <= 127
                && event.pressure >= mw::core::midiPolyPressureMinimum
                && event.pressure <= mw::core::midiPolyPressureMaximum;
        }

        bool controller14Less(const MidiRecording14BitControllerEvent& lhs, const MidiRecording14BitControllerEvent& rhs) noexcept
        {
            if (lhs.tick != rhs.tick) return lhs.tick < rhs.tick;
            if (lhs.midiChannel != rhs.midiChannel) return lhs.midiChannel < rhs.midiChannel;
            if (lhs.controllerMsb != rhs.controllerMsb) return lhs.controllerMsb < rhs.controllerMsb;
            return lhs.order < rhs.order;
        }
        bool valid14BitControllerEvent(const MidiRecording14BitControllerEvent& event) noexcept
        {
            return event.tick >= 0 && event.midiChannel >= 1 && event.midiChannel <= 16
                && event.controllerMsb >= mw::core::midi14BitControllerMinimum
                && event.controllerMsb <= mw::core::midi14BitControllerMaximum
                && event.value >= mw::core::midi14BitMinimum && event.value <= mw::core::midi14BitMaximum;
        }
        bool rpnLess(const MidiRecordingRpnEvent& lhs, const MidiRecordingRpnEvent& rhs) noexcept
        {
            if (lhs.tick != rhs.tick) return lhs.tick < rhs.tick;
            if (lhs.midiChannel != rhs.midiChannel) return lhs.midiChannel < rhs.midiChannel;
            if (lhs.parameter != rhs.parameter) return lhs.parameter < rhs.parameter;
            return lhs.order < rhs.order;
        }
        bool nrpnLess(const MidiRecordingNrpnEvent& lhs, const MidiRecordingNrpnEvent& rhs) noexcept
        {
            if (lhs.tick != rhs.tick) return lhs.tick < rhs.tick;
            if (lhs.midiChannel != rhs.midiChannel) return lhs.midiChannel < rhs.midiChannel;
            if (lhs.parameter != rhs.parameter) return lhs.parameter < rhs.parameter;
            return lhs.order < rhs.order;
        }
        bool validRpnEvent(const MidiRecordingRpnEvent& event) noexcept
        {
            return event.tick >= 0 && event.midiChannel >= 1 && event.midiChannel <= 16
                && event.parameter >= mw::core::midiParameterMinimum && event.parameter < mw::core::midiRpnNullParameter
                && event.value >= mw::core::midiParameterMinimum && event.value <= mw::core::midiParameterMaximum;
        }
        bool validNrpnEvent(const MidiRecordingNrpnEvent& event) noexcept
        {
            return event.tick >= 0 && event.midiChannel >= 1 && event.midiChannel <= 16
                && event.parameter >= mw::core::midiParameterMinimum && event.parameter <= mw::core::midiParameterMaximum
                && event.value >= mw::core::midiParameterMinimum && event.value <= mw::core::midiParameterMaximum;
        }

        std::vector<MidiRecordingCc64Event> sortedValidCc64(const MidiRecordingLoopTakePass& pass)
        {
            std::vector<MidiRecordingCc64Event> events;
            events.reserve(pass.cc64Events.size());
            for (const auto& event : pass.cc64Events)
            {
                if (event.midiChannel < 1 || event.midiChannel > 16
                    || event.value < 0 || event.value > 127
                    || event.tick < 0)
                {
                    continue;
                }
                events.push_back(event);
            }
            std::stable_sort(events.begin(), events.end(), cc64Less);
            return events;
        }

        std::int64_t effectiveNoteEnd(const mw::core::NoteEvent& note,
                                      const MidiRecordingLoopTakePass& pass,
                                      const std::vector<MidiRecordingCc64Event>& events,
                                      MidiRecordingSustainPedalMode sustainMode,
                                      std::int64_t regionOutTick) noexcept
        {
            const auto keyOffTick = std::min(safeNoteEnd(note), regionOutTick);
            if (sustainMode != MidiRecordingSustainPedalMode::ApplyCc64
                || keyOffTick <= note.startTick
                || note.midiChannel < 1 || note.midiChannel > 16)
            {
                return keyOffTick;
            }

            bool pedalDown = sustainDownFromMask(pass.sustainDownMaskAtStart, note.midiChannel);
            for (const auto& event : events)
            {
                if (event.tick > keyOffTick)
                    break;
                if (event.midiChannel == note.midiChannel)
                    pedalDown = event.value >= 64;
            }

            // A CC64 down arriving after the real Note Off never resurrects a note.
            if (!pedalDown)
                return keyOffTick;

            for (const auto& event : events)
            {
                if (event.tick <= keyOffTick || event.midiChannel != note.midiChannel)
                    continue;
                if (event.value < 64)
                    return std::min(event.tick, regionOutTick);
            }
            return regionOutTick;
        }
    }

    std::vector<MidiRecordingControllerEvent> normalizeMidiRecordingControllerEvents(
        std::vector<MidiRecordingControllerEvent> events)
    {
        events.erase(std::remove_if(events.begin(), events.end(), [](const auto& event)
        {
            return !validControllerEvent(event);
        }), events.end());
        std::stable_sort(events.begin(), events.end(), controllerLess);

        std::vector<MidiRecordingControllerEvent> result;
        result.reserve(events.size());
        for (const auto& event : events)
        {
            if (!result.empty())
            {
                auto& previous = result.back();
                if (previous.tick == event.tick
                    && previous.midiChannel == event.midiChannel
                    && previous.controller == event.controller)
                {
                    previous = event;
                    continue;
                }
            }
            result.push_back(event);
        }
        return result;
    }

    std::vector<MidiRecordingControllerEvent> mergeMidiRecordingControllerPasses(
        const std::vector<MidiRecordingLoopTakePass>& passes)
    {
        std::vector<MidiRecordingControllerEvent> result;
        for (const auto& pass : passes)
        {
            const auto normalized = normalizeMidiRecordingControllerEvents(pass.controllerEvents);
            for (const auto& event : normalized)
            {
                auto collision = std::find_if(result.begin(), result.end(), [&](const auto& existing)
                {
                    return existing.tick == event.tick
                        && existing.midiChannel == event.midiChannel
                        && existing.controller == event.controller;
                });
                if (collision != result.end())
                    *collision = event;
                else
                    result.push_back(event);
            }
        }
        std::stable_sort(result.begin(), result.end(), controllerLess);
        return result;
    }

    std::vector<MidiRecordingPitchBendEvent> normalizeMidiRecordingPitchBendEvents(
        std::vector<MidiRecordingPitchBendEvent> events)
    {
        events.erase(std::remove_if(events.begin(), events.end(), [](const auto& event)
        {
            return !validPitchBendEvent(event);
        }), events.end());
        std::stable_sort(events.begin(), events.end(), pitchBendLess);

        std::vector<MidiRecordingPitchBendEvent> result;
        result.reserve(events.size());
        for (const auto& event : events)
        {
            if (!result.empty())
            {
                auto& previous = result.back();
                if (previous.tick == event.tick && previous.midiChannel == event.midiChannel)
                {
                    previous = event;
                    continue;
                }
            }
            result.push_back(event);
        }
        return result;
    }

    std::vector<MidiRecordingPitchBendEvent> mergeMidiRecordingPitchBendPasses(
        const std::vector<MidiRecordingLoopTakePass>& passes)
    {
        std::vector<MidiRecordingPitchBendEvent> result;
        for (const auto& pass : passes)
        {
            const auto normalized = normalizeMidiRecordingPitchBendEvents(pass.pitchBendEvents);
            for (const auto& event : normalized)
            {
                auto collision = std::find_if(result.begin(), result.end(), [&](const auto& existing)
                {
                    return existing.tick == event.tick && existing.midiChannel == event.midiChannel;
                });
                if (collision != result.end())
                    *collision = event;
                else
                    result.push_back(event);
            }
        }
        std::stable_sort(result.begin(), result.end(), pitchBendLess);
        return result;
    }

    std::vector<MidiRecordingProgramChangeEvent> normalizeMidiRecordingProgramChangeEvents(
        std::vector<MidiRecordingProgramChangeEvent> events)
    {
        events.erase(std::remove_if(events.begin(), events.end(), [](const auto& event)
        {
            return !validProgramChangeEvent(event);
        }), events.end());
        std::stable_sort(events.begin(), events.end(), programChangeLess);
        std::vector<MidiRecordingProgramChangeEvent> result;
        result.reserve(events.size());
        for (const auto& event : events)
        {
            if (!result.empty() && result.back().tick == event.tick && result.back().midiChannel == event.midiChannel)
                result.back() = event;
            else
                result.push_back(event);
        }
        return result;
    }

    std::vector<MidiRecordingProgramChangeEvent> mergeMidiRecordingProgramChangePasses(
        const std::vector<MidiRecordingLoopTakePass>& passes)
    {
        std::vector<MidiRecordingProgramChangeEvent> result;
        for (const auto& pass : passes)
        {
            const auto normalized = normalizeMidiRecordingProgramChangeEvents(pass.programChangeEvents);
            for (const auto& event : normalized)
            {
                auto collision = std::find_if(result.begin(), result.end(), [&](const auto& existing)
                {
                    return existing.tick == event.tick && existing.midiChannel == event.midiChannel;
                });
                if (collision != result.end()) *collision = event; else result.push_back(event);
            }
        }
        std::stable_sort(result.begin(), result.end(), programChangeLess);
        return result;
    }

    std::vector<MidiRecordingChannelPressureEvent> normalizeMidiRecordingChannelPressureEvents(
        std::vector<MidiRecordingChannelPressureEvent> events)
    {
        events.erase(std::remove_if(events.begin(), events.end(), [](const auto& event)
        {
            return !validChannelPressureEvent(event);
        }), events.end());
        std::stable_sort(events.begin(), events.end(), channelPressureLess);
        std::vector<MidiRecordingChannelPressureEvent> result;
        result.reserve(events.size());
        for (const auto& event : events)
        {
            if (!result.empty() && result.back().tick == event.tick && result.back().midiChannel == event.midiChannel)
                result.back() = event;
            else
                result.push_back(event);
        }
        return result;
    }

    std::vector<MidiRecordingChannelPressureEvent> mergeMidiRecordingChannelPressurePasses(
        const std::vector<MidiRecordingLoopTakePass>& passes)
    {
        std::vector<MidiRecordingChannelPressureEvent> result;
        for (const auto& pass : passes)
        {
            const auto normalized = normalizeMidiRecordingChannelPressureEvents(pass.channelPressureEvents);
            for (const auto& event : normalized)
            {
                auto collision = std::find_if(result.begin(), result.end(), [&](const auto& existing)
                {
                    return existing.tick == event.tick && existing.midiChannel == event.midiChannel;
                });
                if (collision != result.end()) *collision = event; else result.push_back(event);
            }
        }
        std::stable_sort(result.begin(), result.end(), channelPressureLess);
        return result;
    }

    std::vector<MidiRecordingPolyPressureEvent> normalizeMidiRecordingPolyPressureEvents(
        std::vector<MidiRecordingPolyPressureEvent> events)
    {
        events.erase(std::remove_if(events.begin(), events.end(), [](const auto& event)
        {
            return !validPolyPressureEvent(event);
        }), events.end());
        std::stable_sort(events.begin(), events.end(), polyPressureLess);
        std::vector<MidiRecordingPolyPressureEvent> result;
        result.reserve(events.size());
        for (const auto& event : events)
        {
            if (!result.empty() && result.back().tick == event.tick
                && result.back().midiChannel == event.midiChannel
                && result.back().noteNumber == event.noteNumber)
                result.back() = event;
            else
                result.push_back(event);
        }
        return result;
    }

    std::vector<MidiRecordingPolyPressureEvent> mergeMidiRecordingPolyPressurePasses(
        const std::vector<MidiRecordingLoopTakePass>& passes)
    {
        std::vector<MidiRecordingPolyPressureEvent> result;
        for (const auto& pass : passes)
        {
            const auto normalized = normalizeMidiRecordingPolyPressureEvents(pass.polyPressureEvents);
            for (const auto& event : normalized)
            {
                auto collision = std::find_if(result.begin(), result.end(), [&](const auto& existing)
                {
                    return existing.tick == event.tick && existing.midiChannel == event.midiChannel
                        && existing.noteNumber == event.noteNumber;
                });
                if (collision != result.end()) *collision = event; else result.push_back(event);
            }
        }
        std::stable_sort(result.begin(), result.end(), polyPressureLess);
        return result;
    }

    std::vector<MidiRecording14BitControllerEvent> normalizeMidiRecording14BitControllerEvents(
        std::vector<MidiRecording14BitControllerEvent> events)
    {
        events.erase(std::remove_if(events.begin(), events.end(), [](const auto& event){ return !valid14BitControllerEvent(event); }), events.end());
        std::stable_sort(events.begin(), events.end(), controller14Less);
        std::vector<MidiRecording14BitControllerEvent> result; result.reserve(events.size());
        for (const auto& event : events)
        {
            if (!result.empty() && result.back().tick == event.tick && result.back().midiChannel == event.midiChannel && result.back().controllerMsb == event.controllerMsb) result.back() = event;
            else result.push_back(event);
        }
        return result;
    }

    std::vector<MidiRecording14BitControllerEvent> mergeMidiRecording14BitControllerPasses(const std::vector<MidiRecordingLoopTakePass>& passes)
    {
        std::vector<MidiRecording14BitControllerEvent> result;
        for (const auto& pass : passes) for (const auto& event : normalizeMidiRecording14BitControllerEvents(pass.controller14BitEvents))
        {
            auto collision = std::find_if(result.begin(), result.end(), [&](const auto& e){ return e.tick==event.tick && e.midiChannel==event.midiChannel && e.controllerMsb==event.controllerMsb; });
            if (collision != result.end()) *collision = event; else result.push_back(event);
        }
        std::stable_sort(result.begin(), result.end(), controller14Less); return result;
    }

    std::vector<MidiRecordingRpnEvent> normalizeMidiRecordingRpnEvents(std::vector<MidiRecordingRpnEvent> events)
    {
        events.erase(std::remove_if(events.begin(), events.end(), [](const auto& event){ return !validRpnEvent(event); }), events.end());
        std::stable_sort(events.begin(), events.end(), rpnLess); std::vector<MidiRecordingRpnEvent> result; result.reserve(events.size());
        for (const auto& event : events) { if (!result.empty() && result.back().tick==event.tick && result.back().midiChannel==event.midiChannel && result.back().parameter==event.parameter) result.back()=event; else result.push_back(event); }
        return result;
    }
    std::vector<MidiRecordingRpnEvent> mergeMidiRecordingRpnPasses(const std::vector<MidiRecordingLoopTakePass>& passes)
    {
        std::vector<MidiRecordingRpnEvent> result; for (const auto& pass:passes) for(const auto& event:normalizeMidiRecordingRpnEvents(pass.rpnEvents))
        { auto collision=std::find_if(result.begin(),result.end(),[&](const auto& e){return e.tick==event.tick&&e.midiChannel==event.midiChannel&&e.parameter==event.parameter;}); if(collision!=result.end())*collision=event;else result.push_back(event); }
        std::stable_sort(result.begin(),result.end(),rpnLess); return result;
    }
    std::vector<MidiRecordingNrpnEvent> normalizeMidiRecordingNrpnEvents(std::vector<MidiRecordingNrpnEvent> events)
    {
        events.erase(std::remove_if(events.begin(), events.end(), [](const auto& event){ return !validNrpnEvent(event); }), events.end());
        std::stable_sort(events.begin(), events.end(), nrpnLess); std::vector<MidiRecordingNrpnEvent> result; result.reserve(events.size());
        for (const auto& event : events) { if (!result.empty() && result.back().tick==event.tick && result.back().midiChannel==event.midiChannel && result.back().parameter==event.parameter) result.back()=event; else result.push_back(event); }
        return result;
    }
    std::vector<MidiRecordingNrpnEvent> mergeMidiRecordingNrpnPasses(const std::vector<MidiRecordingLoopTakePass>& passes)
    {
        std::vector<MidiRecordingNrpnEvent> result; for (const auto& pass:passes) for(const auto& event:normalizeMidiRecordingNrpnEvents(pass.nrpnEvents))
        { auto collision=std::find_if(result.begin(),result.end(),[&](const auto& e){return e.tick==event.tick&&e.midiChannel==event.midiChannel&&e.parameter==event.parameter;}); if(collision!=result.end())*collision=event;else result.push_back(event); }
        std::stable_sort(result.begin(),result.end(),nrpnLess); return result;
    }

    std::vector<mw::core::NoteEvent> buildMidiRecordingTakePassNotes(
        const MidiRecordingLoopTakePass& pass,
        MidiRecordingSustainPedalMode sustainMode,
        std::int64_t regionOutTick)
    {
        std::vector<mw::core::NoteEvent> result;
        if (regionOutTick <= 0)
            return result;

        const auto events = sortedValidCc64(pass);
        result.reserve(pass.notes.size());
        for (const auto& source : pass.notes)
        {
            if (source.durationTicks <= 0 || source.startTick >= regionOutTick)
                continue;
            auto note = source;
            const auto endTick = effectiveNoteEnd(source, pass, events, sustainMode, regionOutTick);
            if (endTick <= note.startTick)
                continue;
            note.durationTicks = endTick - note.startTick;
            result.push_back(note);
        }
        return result;
    }

    std::uint64_t MidiRecordingLoopTakeReviewState::begin(mw::core::StableId projectId,
                                                            mw::core::StableId trackId,
                                                            std::uint64_t recordingGeneration) noexcept
    {
        cancel();
        if (projectId == 0 || trackId == 0 || recordingGeneration == 0)
            return 0;

        generation_ = recordingGeneration;
        projectId_ = projectId;
        trackId_ = trackId;
        collecting_ = true;
        return generation_;
    }

    void MidiRecordingLoopTakeReviewState::cancel() noexcept
    {
        generation_ = 0;
        projectId_ = 0;
        trackId_ = 0;
        selectedPassNumber_ = 0;
        collecting_ = false;
        reviewActive_ = false;
        passes_.clear();
    }

    bool MidiRecordingLoopTakeReviewState::addPass(std::uint64_t generation,
                                                     int passNumber,
                                                     std::vector<mw::core::NoteEvent> notes,
                                                     std::uint16_t sustainDownMaskAtStart,
                                                     std::vector<MidiRecordingCc64Event> cc64Events,
                                                     std::vector<MidiRecordingControllerEvent> controllerEvents,
                                                     std::vector<MidiRecordingPitchBendEvent> pitchBendEvents,
                                                     std::vector<MidiRecordingProgramChangeEvent> programChangeEvents,
                                                     std::vector<MidiRecordingChannelPressureEvent> channelPressureEvents,
                                                     std::vector<MidiRecordingPolyPressureEvent> polyPressureEvents,
                                                     std::vector<MidiRecording14BitControllerEvent> controller14BitEvents,
                                                     std::vector<MidiRecordingRpnEvent> rpnEvents,
                                                     std::vector<MidiRecordingNrpnEvent> nrpnEvents)
    {
        if (!matches(generation) || !collecting_ || reviewActive_ || passNumber <= 0)
            return false;

        if (!passes_.empty() && passNumber <= passes_.back().passNumber)
            return false;

        std::stable_sort(cc64Events.begin(), cc64Events.end(), cc64Less);
        controllerEvents = normalizeMidiRecordingControllerEvents(std::move(controllerEvents));
        pitchBendEvents = normalizeMidiRecordingPitchBendEvents(std::move(pitchBendEvents));
        programChangeEvents = normalizeMidiRecordingProgramChangeEvents(std::move(programChangeEvents));
        channelPressureEvents = normalizeMidiRecordingChannelPressureEvents(std::move(channelPressureEvents));
        polyPressureEvents = normalizeMidiRecordingPolyPressureEvents(std::move(polyPressureEvents));
        controller14BitEvents = normalizeMidiRecording14BitControllerEvents(std::move(controller14BitEvents));
        rpnEvents = normalizeMidiRecordingRpnEvents(std::move(rpnEvents));
        nrpnEvents = normalizeMidiRecordingNrpnEvents(std::move(nrpnEvents));
        passes_.push_back({ passNumber, std::move(notes), sustainDownMaskAtStart, std::move(cc64Events), std::move(controllerEvents), std::move(pitchBendEvents), std::move(programChangeEvents), std::move(channelPressureEvents), std::move(polyPressureEvents), std::move(controller14BitEvents), std::move(rpnEvents), std::move(nrpnEvents) });
        if (selectedPassNumber_ == 0)
            selectedPassNumber_ = passNumber;
        return true;
    }

    bool MidiRecordingLoopTakeReviewState::finishCollection(std::uint64_t generation) noexcept
    {
        if (!matches(generation) || !collecting_ || reviewActive_ || passes_.empty())
            return false;
        collecting_ = false;
        reviewActive_ = true;
        if (findPass(selectedPassNumber_) == nullptr)
            selectedPassNumber_ = passes_.front().passNumber;
        return true;
    }

    bool MidiRecordingLoopTakeReviewState::restoreReview(
        mw::core::StableId projectId,
        mw::core::StableId trackId,
        std::uint64_t generation,
        std::vector<MidiRecordingLoopTakePass> passes,
        int selectedPassNumber)
    {
        cancel();
        if (projectId == 0 || trackId == 0 || generation == 0 || passes.empty())
            return false;

        int previousPassNumber = 0;
        for (auto& pass : passes)
        {
            if (pass.passNumber <= previousPassNumber)
                return false;
            previousPassNumber = pass.passNumber;
            std::stable_sort(pass.cc64Events.begin(), pass.cc64Events.end(), cc64Less);
            pass.controllerEvents = normalizeMidiRecordingControllerEvents(std::move(pass.controllerEvents));
            pass.pitchBendEvents = normalizeMidiRecordingPitchBendEvents(std::move(pass.pitchBendEvents));
            pass.programChangeEvents = normalizeMidiRecordingProgramChangeEvents(std::move(pass.programChangeEvents));
            pass.channelPressureEvents = normalizeMidiRecordingChannelPressureEvents(std::move(pass.channelPressureEvents));
            pass.polyPressureEvents = normalizeMidiRecordingPolyPressureEvents(std::move(pass.polyPressureEvents));
            pass.controller14BitEvents = normalizeMidiRecording14BitControllerEvents(std::move(pass.controller14BitEvents));
            pass.rpnEvents = normalizeMidiRecordingRpnEvents(std::move(pass.rpnEvents));
            pass.nrpnEvents = normalizeMidiRecordingNrpnEvents(std::move(pass.nrpnEvents));
        }

        generation_ = generation;
        projectId_ = projectId;
        trackId_ = trackId;
        passes_ = std::move(passes);
        selectedPassNumber_ = selectedPassNumber;
        if (findPass(selectedPassNumber_) == nullptr)
            selectedPassNumber_ = passes_.front().passNumber;
        collecting_ = false;
        reviewActive_ = true;
        return true;
    }

    bool MidiRecordingLoopTakeReviewState::selectPass(std::uint64_t generation, int passNumber) noexcept
    {
        if (!matches(generation) || !reviewActive_ || findPass(passNumber) == nullptr)
            return false;
        selectedPassNumber_ = passNumber;
        return true;
    }

    bool MidiRecordingLoopTakeReviewState::isCollecting() const noexcept { return collecting_; }
    bool MidiRecordingLoopTakeReviewState::isReviewActive() const noexcept { return reviewActive_; }
    bool MidiRecordingLoopTakeReviewState::matches(std::uint64_t generation) const noexcept { return generation_ != 0 && generation == generation_; }

    bool MidiRecordingLoopTakeReviewState::matchesProjectTrack(mw::core::StableId projectId,
                                                                 mw::core::StableId trackId) const noexcept
    {
        return generation_ != 0 && projectId != 0 && trackId != 0
            && projectId == projectId_ && trackId == trackId_;
    }

    std::uint64_t MidiRecordingLoopTakeReviewState::generation() const noexcept { return generation_; }
    mw::core::StableId MidiRecordingLoopTakeReviewState::projectId() const noexcept { return projectId_; }
    mw::core::StableId MidiRecordingLoopTakeReviewState::trackId() const noexcept { return trackId_; }
    int MidiRecordingLoopTakeReviewState::selectedPassNumber() const noexcept { return selectedPassNumber_; }
    std::size_t MidiRecordingLoopTakeReviewState::passCount() const noexcept { return passes_.size(); }

    std::size_t MidiRecordingLoopTakeReviewState::totalNoteCount() const noexcept
    {
        std::size_t count = 0;
        for (const auto& pass : passes_)
            count += pass.notes.size();
        return count;
    }

    std::size_t MidiRecordingLoopTakeReviewState::totalControllerEventCount() const noexcept
    {
        std::size_t count = 0;
        for (const auto& pass : passes_)
            count += pass.controllerEvents.size();
        return count;
    }

    std::size_t MidiRecordingLoopTakeReviewState::totalPitchBendEventCount() const noexcept
    {
        std::size_t count = 0;
        for (const auto& pass : passes_)
            count += pass.pitchBendEvents.size();
        return count;
    }

    std::size_t MidiRecordingLoopTakeReviewState::totalProgramChangeEventCount() const noexcept
    {
        std::size_t count = 0;
        for (const auto& pass : passes_) count += pass.programChangeEvents.size();
        return count;
    }

    std::size_t MidiRecordingLoopTakeReviewState::totalChannelPressureEventCount() const noexcept
    {
        std::size_t count = 0;
        for (const auto& pass : passes_) count += pass.channelPressureEvents.size();
        return count;
    }

    std::size_t MidiRecordingLoopTakeReviewState::totalPolyPressureEventCount() const noexcept
    {
        std::size_t count = 0;
        for (const auto& pass : passes_) count += pass.polyPressureEvents.size();
        return count;
    }

    std::size_t MidiRecordingLoopTakeReviewState::total14BitControllerEventCount() const noexcept
    { std::size_t count=0; for(const auto& pass:passes_) count+=pass.controller14BitEvents.size(); return count; }
    std::size_t MidiRecordingLoopTakeReviewState::totalRpnEventCount() const noexcept
    { std::size_t count=0; for(const auto& pass:passes_) count+=pass.rpnEvents.size(); return count; }
    std::size_t MidiRecordingLoopTakeReviewState::totalNrpnEventCount() const noexcept
    { std::size_t count=0; for(const auto& pass:passes_) count+=pass.nrpnEvents.size(); return count; }

    const std::vector<MidiRecordingLoopTakePass>& MidiRecordingLoopTakeReviewState::passes() const noexcept { return passes_; }
    const MidiRecordingLoopTakePass* MidiRecordingLoopTakeReviewState::selectedPass() const noexcept { return findPass(selectedPassNumber_); }

    std::vector<mw::core::NoteEvent> MidiRecordingLoopTakeReviewState::mergedNotes() const
    {
        std::vector<mw::core::NoteEvent> result;
        result.reserve(totalNoteCount());
        for (const auto& pass : passes_)
            result.insert(result.end(), pass.notes.begin(), pass.notes.end());
        return result;
    }

    std::vector<MidiRecordingControllerEvent> MidiRecordingLoopTakeReviewState::mergedControllerEvents() const
    {
        return mergeMidiRecordingControllerPasses(passes_);
    }

    std::vector<MidiRecordingPitchBendEvent> MidiRecordingLoopTakeReviewState::mergedPitchBendEvents() const
    {
        return mergeMidiRecordingPitchBendPasses(passes_);
    }

    std::vector<MidiRecordingProgramChangeEvent> MidiRecordingLoopTakeReviewState::mergedProgramChangeEvents() const
    {
        return mergeMidiRecordingProgramChangePasses(passes_);
    }

    std::vector<MidiRecordingChannelPressureEvent> MidiRecordingLoopTakeReviewState::mergedChannelPressureEvents() const
    {
        return mergeMidiRecordingChannelPressurePasses(passes_);
    }

    std::vector<MidiRecordingPolyPressureEvent> MidiRecordingLoopTakeReviewState::mergedPolyPressureEvents() const
    {
        return mergeMidiRecordingPolyPressurePasses(passes_);
    }

    std::vector<MidiRecording14BitControllerEvent> MidiRecordingLoopTakeReviewState::merged14BitControllerEvents() const
    { return mergeMidiRecording14BitControllerPasses(passes_); }
    std::vector<MidiRecordingRpnEvent> MidiRecordingLoopTakeReviewState::mergedRpnEvents() const
    { return mergeMidiRecordingRpnPasses(passes_); }
    std::vector<MidiRecordingNrpnEvent> MidiRecordingLoopTakeReviewState::mergedNrpnEvents() const
    { return mergeMidiRecordingNrpnPasses(passes_); }

    std::vector<mw::core::NoteEvent> MidiRecordingLoopTakeReviewState::mergedNotes(
        MidiRecordingSustainPedalMode sustainMode,
        std::int64_t regionOutTick) const
    {
        std::vector<mw::core::NoteEvent> result;
        result.reserve(totalNoteCount());
        for (const auto& pass : passes_)
        {
            auto passNotes = buildMidiRecordingTakePassNotes(pass, sustainMode, regionOutTick);
            result.insert(result.end(), passNotes.begin(), passNotes.end());
        }
        return result;
    }

    const MidiRecordingLoopTakePass* MidiRecordingLoopTakeReviewState::findPass(int passNumber) const noexcept
    {
        const auto it = std::find_if(passes_.begin(), passes_.end(), [passNumber](const auto& pass)
        {
            return pass.passNumber == passNumber;
        });
        return it == passes_.end() ? nullptr : &*it;
    }
}
