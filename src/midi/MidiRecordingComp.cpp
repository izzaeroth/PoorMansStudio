#include "midi/MidiRecordingComp.h"

#include <algorithm>
#include <limits>

namespace mw::midi
{
    namespace
    {
        int sanitizeNumerator(int numerator) noexcept
        {
            return numerator >= 1 && numerator <= 32 ? numerator : 4;
        }

        int sanitizeDenominator(int denominator) noexcept
        {
            switch (denominator)
            {
                case 1:
                case 2:
                case 4:
                case 8:
                case 16:
                case 32:
                    return denominator;
                default:
                    return 4;
            }
        }

        int sanitizeTicksPerQuarter(int ticksPerQuarterNote) noexcept
        {
            return ticksPerQuarterNote > 0 ? ticksPerQuarterNote : 960;
        }

        std::int64_t safeNoteEnd(const mw::core::NoteEvent& note) noexcept
        {
            if (note.durationTicks <= 0)
                return note.startTick;
            if (note.startTick > std::numeric_limits<std::int64_t>::max() - note.durationTicks)
                return std::numeric_limits<std::int64_t>::max();
            return note.startTick + note.durationTicks;
        }
    }

    std::uint64_t MidiRecordingLoopCompState::begin(
        mw::core::StableId projectId,
        mw::core::StableId trackId,
        std::uint64_t reviewGeneration,
        std::int64_t regionInTick,
        std::int64_t regionOutTick,
        int ticksPerQuarterNote,
        int timeSignatureNumerator,
        int timeSignatureDenominator,
        MidiRecordingCompGrid grid,
        int defaultPassNumber,
        const std::vector<MidiRecordingLoopTakePass>& passes)
    {
        cancel();
        if (projectId == 0 || trackId == 0 || reviewGeneration == 0
            || regionInTick < 0 || regionOutTick <= regionInTick
            || !containsPass(passes, defaultPassNumber))
        {
            return 0;
        }

        generation_ = reviewGeneration;
        projectId_ = projectId;
        trackId_ = trackId;
        regionInTick_ = regionInTick;
        regionOutTick_ = regionOutTick;
        ticksPerQuarterNote_ = sanitizeTicksPerQuarter(ticksPerQuarterNote);
        numerator_ = sanitizeNumerator(timeSignatureNumerator);
        denominator_ = sanitizeDenominator(timeSignatureDenominator);

        if (!rebuildSegments(grid, defaultPassNumber, passes))
        {
            cancel();
            return 0;
        }

        active_ = true;
        return generation_;
    }

    bool MidiRecordingLoopCompState::restore(
        mw::core::StableId projectId,
        mw::core::StableId trackId,
        std::uint64_t reviewGeneration,
        std::int64_t regionInTick,
        std::int64_t regionOutTick,
        int ticksPerQuarterNote,
        int timeSignatureNumerator,
        int timeSignatureDenominator,
        MidiRecordingCompGrid grid,
        std::vector<MidiRecordingCompSegment> segments,
        const std::vector<MidiRecordingLoopTakePass>& passes)
    {
        cancel();
        if (projectId == 0 || trackId == 0 || reviewGeneration == 0
            || regionInTick < 0 || regionOutTick <= regionInTick || segments.empty())
        {
            return false;
        }

        for (std::size_t index = 0; index < segments.size(); ++index)
        {
            const auto& segment = segments[index];
            if (segment.startTick < regionInTick || segment.endTick <= segment.startTick
                || segment.endTick > regionOutTick
                || !containsPass(passes, segment.selectedPassNumber))
            {
                return false;
            }
            if (index == 0 && segment.startTick != regionInTick)
                return false;
            if (index > 0 && segments[index - 1].endTick != segment.startTick)
                return false;
        }
        if (segments.back().endTick != regionOutTick)
            return false;

        generation_ = reviewGeneration;
        projectId_ = projectId;
        trackId_ = trackId;
        regionInTick_ = regionInTick;
        regionOutTick_ = regionOutTick;
        ticksPerQuarterNote_ = sanitizeTicksPerQuarter(ticksPerQuarterNote);
        numerator_ = sanitizeNumerator(timeSignatureNumerator);
        denominator_ = sanitizeDenominator(timeSignatureDenominator);
        grid_ = grid;
        segments_ = std::move(segments);
        active_ = true;
        return true;
    }

    void MidiRecordingLoopCompState::cancel() noexcept
    {
        generation_ = 0;
        projectId_ = 0;
        trackId_ = 0;
        regionInTick_ = 0;
        regionOutTick_ = 0;
        ticksPerQuarterNote_ = 960;
        numerator_ = 4;
        denominator_ = 4;
        grid_ = MidiRecordingCompGrid::Beat;
        boundaryNoteMode_ = MidiRecordingCompBoundaryNoteMode::Cut;
        sustainPedalMode_ = MidiRecordingSustainPedalMode::Ignore;
        crossPassNoteMode_ = MidiRecordingCompCrossPassNoteMode::Off;
        active_ = false;
        segments_.clear();
    }

    bool MidiRecordingLoopCompState::setGrid(
        std::uint64_t generation,
        MidiRecordingCompGrid grid,
        int defaultPassNumber,
        const std::vector<MidiRecordingLoopTakePass>& passes)
    {
        if (!matches(generation) || !active_ || !containsPass(passes, defaultPassNumber))
            return false;
        return rebuildSegments(grid, defaultPassNumber, passes);
    }

    bool MidiRecordingLoopCompState::assignPass(
        std::uint64_t generation,
        std::size_t segmentIndex,
        int passNumber,
        const std::vector<MidiRecordingLoopTakePass>& passes) noexcept
    {
        if (!matches(generation) || !active_ || segmentIndex >= segments_.size()
            || !containsPass(passes, passNumber))
        {
            return false;
        }
        segments_[segmentIndex].selectedPassNumber = passNumber;
        return true;
    }


    bool MidiRecordingLoopCompState::setBoundaryNoteMode(
        std::uint64_t generation,
        MidiRecordingCompBoundaryNoteMode mode) noexcept
    {
        if (!matches(generation) || !active_)
            return false;
        if (mode != MidiRecordingCompBoundaryNoteMode::Cut
            && mode != MidiRecordingCompBoundaryNoteMode::PreserveSamePass)
        {
            return false;
        }
        boundaryNoteMode_ = mode;
        return true;
    }

    bool MidiRecordingLoopCompState::setSustainPedalMode(
        std::uint64_t generation,
        MidiRecordingSustainPedalMode mode) noexcept
    {
        if (!matches(generation) || !active_)
            return false;
        if (mode != MidiRecordingSustainPedalMode::Ignore
            && mode != MidiRecordingSustainPedalMode::ApplyCc64)
        {
            return false;
        }
        sustainPedalMode_ = mode;
        return true;
    }

    bool MidiRecordingLoopCompState::setCrossPassNoteMode(
        std::uint64_t generation,
        MidiRecordingCompCrossPassNoteMode mode) noexcept
    {
        if (!matches(generation) || !active_)
            return false;
        if (mode != MidiRecordingCompCrossPassNoteMode::Off
            && mode != MidiRecordingCompCrossPassNoteMode::BridgeExact)
        {
            return false;
        }
        crossPassNoteMode_ = mode;
        return true;
    }

    bool MidiRecordingLoopCompState::addCustomBoundary(
        std::uint64_t generation,
        std::int64_t tick) noexcept
    {
        if (!matches(generation) || grid_ != MidiRecordingCompGrid::Custom
            || tick <= regionInTick_ || tick >= regionOutTick_)
        {
            return false;
        }

        const auto containing = std::find_if(segments_.begin(), segments_.end(), [tick](const auto& segment)
        {
            return tick > segment.startTick && tick < segment.endTick;
        });
        if (containing == segments_.end())
            return false;

        const auto index = static_cast<std::size_t>(std::distance(segments_.begin(), containing));
        const auto original = *containing;
        segments_[index].endTick = tick;
        segments_.insert(segments_.begin() + static_cast<std::ptrdiff_t>(index + 1),
                         { tick, original.endTick, original.selectedPassNumber });
        return true;
    }

    bool MidiRecordingLoopCompState::removeCustomBoundary(
        std::uint64_t generation,
        std::int64_t tick) noexcept
    {
        if (!matches(generation) || grid_ != MidiRecordingCompGrid::Custom
            || tick <= regionInTick_ || tick >= regionOutTick_ || segments_.size() < 2)
        {
            return false;
        }

        const auto right = std::find_if(segments_.begin() + 1, segments_.end(), [tick](const auto& segment)
        {
            return segment.startTick == tick;
        });
        if (right == segments_.end())
            return false;

        const auto rightIndex = static_cast<std::size_t>(std::distance(segments_.begin(), right));
        auto& left = segments_[rightIndex - 1];
        if (left.endTick != tick)
            return false;

        // Merging keeps the pass assignment from the segment on the left. This is deterministic
        // and avoids silently changing already-reviewed material on the earlier side of the split.
        left.endTick = right->endTick;
        segments_.erase(segments_.begin() + static_cast<std::ptrdiff_t>(rightIndex));
        return true;
    }

    bool MidiRecordingLoopCompState::isActive() const noexcept
    {
        return active_;
    }

    bool MidiRecordingLoopCompState::matches(std::uint64_t generation) const noexcept
    {
        return active_ && generation_ != 0 && generation == generation_;
    }

    bool MidiRecordingLoopCompState::matchesProjectTrack(
        mw::core::StableId projectId,
        mw::core::StableId trackId) const noexcept
    {
        return active_ && projectId_ != 0 && trackId_ != 0
            && projectId == projectId_ && trackId == trackId_;
    }

    std::uint64_t MidiRecordingLoopCompState::generation() const noexcept
    {
        return generation_;
    }

    mw::core::StableId MidiRecordingLoopCompState::projectId() const noexcept
    {
        return projectId_;
    }

    mw::core::StableId MidiRecordingLoopCompState::trackId() const noexcept
    {
        return trackId_;
    }

    MidiRecordingCompGrid MidiRecordingLoopCompState::grid() const noexcept
    {
        return grid_;
    }


    MidiRecordingCompBoundaryNoteMode MidiRecordingLoopCompState::boundaryNoteMode() const noexcept
    {
        return boundaryNoteMode_;
    }

    MidiRecordingSustainPedalMode MidiRecordingLoopCompState::sustainPedalMode() const noexcept
    {
        return sustainPedalMode_;
    }

    MidiRecordingCompCrossPassNoteMode MidiRecordingLoopCompState::crossPassNoteMode() const noexcept
    {
        return crossPassNoteMode_;
    }

    std::int64_t MidiRecordingLoopCompState::regionInTick() const noexcept
    {
        return regionInTick_;
    }

    std::int64_t MidiRecordingLoopCompState::regionOutTick() const noexcept
    {
        return regionOutTick_;
    }

    const std::vector<MidiRecordingCompSegment>& MidiRecordingLoopCompState::segments() const noexcept
    {
        return segments_;
    }

    std::vector<std::int64_t> MidiRecordingLoopCompState::customBoundaries() const
    {
        std::vector<std::int64_t> boundaries;
        if (!active_ || grid_ != MidiRecordingCompGrid::Custom || segments_.size() < 2)
            return boundaries;
        boundaries.reserve(segments_.size() - 1);
        for (std::size_t index = 1; index < segments_.size(); ++index)
            boundaries.push_back(segments_[index].startTick);
        return boundaries;
    }

    MidiRecordingCompBuildResult MidiRecordingLoopCompState::buildNotes(
        std::uint64_t generation,
        const std::vector<MidiRecordingLoopTakePass>& passes) const
    {
        MidiRecordingCompBuildResult result;
        if (!matches(generation))
            return result;

        struct BuiltCompNote
        {
            mw::core::NoteEvent note;
            int activePassNumber = 0;
            std::size_t segmentIndex = 0;
            bool suppressed = false;
        };

        std::vector<BuiltCompNote> builtNotes;

        for (std::size_t segmentIndex = 0; segmentIndex < segments_.size(); ++segmentIndex)
        {
            const auto& segment = segments_[segmentIndex];
            const auto passIt = std::find_if(passes.begin(), passes.end(), [&segment](const auto& pass)
            {
                return pass.passNumber == segment.selectedPassNumber;
            });
            if (passIt == passes.end())
                return {};

            std::int64_t allowedEndTick = segment.endTick;
            if (boundaryNoteMode_ == MidiRecordingCompBoundaryNoteMode::PreserveSamePass)
            {
                for (std::size_t next = segmentIndex + 1; next < segments_.size(); ++next)
                {
                    if (segments_[next].selectedPassNumber != segment.selectedPassNumber)
                        break;
                    allowedEndTick = segments_[next].endTick;
                }
            }
            allowedEndTick = std::min(allowedEndTick, regionOutTick_);

            const auto effectivePassNotes = buildMidiRecordingTakePassNotes(
                *passIt, sustainPedalMode_, regionOutTick_);
            std::vector<BuiltCompNote> segmentNotes;
            for (const auto& note : effectivePassNotes)
            {
                // Comp ownership always follows the real Note On. A note that began before this
                // segment is never manufactured at a boundary. Preserve Same Pass relaxes only
                // the end clamp across a contiguous run assigned to this retained pass.
                if (note.startTick < segment.startTick || note.startTick >= segment.endTick
                    || note.durationTicks <= 0)
                {
                    continue;
                }

                auto clipped = note;
                const auto originalEnd = safeNoteEnd(note);
                const auto clippedEnd = std::min(originalEnd, allowedEndTick);
                if (clippedEnd <= clipped.startTick)
                    continue;
                clipped.durationTicks = clippedEnd - clipped.startTick;
                if (clippedEnd < originalEnd)
                    ++result.clippedNoteCount;
                segmentNotes.push_back({ clipped, segment.selectedPassNumber, segmentIndex, false });
            }

            std::stable_sort(segmentNotes.begin(), segmentNotes.end(), [](const auto& lhs, const auto& rhs)
            {
                if (lhs.note.startTick != rhs.note.startTick)
                    return lhs.note.startTick < rhs.note.startTick;
                if (lhs.note.midiChannel != rhs.note.midiChannel)
                    return lhs.note.midiChannel < rhs.note.midiChannel;
                if (lhs.note.pitch != rhs.note.pitch)
                    return lhs.note.pitch < rhs.note.pitch;
                if (lhs.note.velocity != rhs.note.velocity)
                    return lhs.note.velocity < rhs.note.velocity;
                return lhs.note.durationTicks < rhs.note.durationTicks;
            });

            builtNotes.insert(builtNotes.end(), segmentNotes.begin(), segmentNotes.end());
        }

        // Bridge Exact is intentionally conservative. It is disabled unless Preserve Same Pass
        // is selected. At a pass-change boundary it requires exactly one live left note and one
        // real right-pass Note On with the same pitch/channel exactly on the boundary. The earlier
        // note keeps its velocity/articulation and the matched right Note On is suppressed. No gap,
        // near-match, ambiguity, different channel/pitch, or synthetic boundary Note On is allowed.
        if (boundaryNoteMode_ == MidiRecordingCompBoundaryNoteMode::PreserveSamePass
            && crossPassNoteMode_ == MidiRecordingCompCrossPassNoteMode::BridgeExact)
        {
            for (std::size_t boundaryIndex = 0; boundaryIndex + 1 < segments_.size(); ++boundaryIndex)
            {
                const auto& leftSegment = segments_[boundaryIndex];
                const auto& rightSegment = segments_[boundaryIndex + 1];
                if (leftSegment.selectedPassNumber == rightSegment.selectedPassNumber)
                    continue;

                const auto boundaryTick = leftSegment.endTick;
                for (int channel = 1; channel <= 16; ++channel)
                {
                    for (int pitch = 0; pitch <= 127; ++pitch)
                    {
                        BuiltCompNote* leftCandidate = nullptr;
                        BuiltCompNote* rightCandidate = nullptr;
                        int leftCount = 0;
                        int rightCount = 0;

                        for (auto& built : builtNotes)
                        {
                            if (built.suppressed || built.note.midiChannel != channel || built.note.pitch != pitch)
                                continue;

                            const auto endTick = safeNoteEnd(built.note);
                            if (built.activePassNumber == leftSegment.selectedPassNumber
                                && built.note.startTick < boundaryTick
                                && endTick == boundaryTick)
                            {
                                ++leftCount;
                                leftCandidate = &built;
                            }

                            if (built.activePassNumber == rightSegment.selectedPassNumber
                                && built.segmentIndex == boundaryIndex + 1
                                && built.note.startTick == boundaryTick
                                && endTick > boundaryTick)
                            {
                                ++rightCount;
                                rightCandidate = &built;
                            }
                        }

                        if (leftCount != 1 || rightCount != 1
                            || leftCandidate == nullptr || rightCandidate == nullptr
                            || leftCandidate == rightCandidate)
                        {
                            continue;
                        }

                        const auto rightEnd = safeNoteEnd(rightCandidate->note);
                        if (rightEnd <= boundaryTick || rightEnd <= leftCandidate->note.startTick)
                            continue;

                        leftCandidate->note.durationTicks = rightEnd - leftCandidate->note.startTick;
                        leftCandidate->activePassNumber = rightSegment.selectedPassNumber;
                        leftCandidate->segmentIndex = rightCandidate->segmentIndex;
                        rightCandidate->suppressed = true;
                        ++result.crossPassBridgedNoteCount;
                    }
                }
            }
        }

        result.notes.reserve(builtNotes.size());
        for (const auto& built : builtNotes)
            if (!built.suppressed)
                result.notes.push_back(built.note);

        std::stable_sort(result.notes.begin(), result.notes.end(), [](const auto& lhs, const auto& rhs)
        {
            if (lhs.startTick != rhs.startTick)
                return lhs.startTick < rhs.startTick;
            if (lhs.midiChannel != rhs.midiChannel)
                return lhs.midiChannel < rhs.midiChannel;
            if (lhs.pitch != rhs.pitch)
                return lhs.pitch < rhs.pitch;
            if (lhs.velocity != rhs.velocity)
                return lhs.velocity < rhs.velocity;
            return lhs.durationTicks < rhs.durationTicks;
        });
        return result;
    }

    bool MidiRecordingLoopCompState::rebuildSegments(
        MidiRecordingCompGrid grid,
        int defaultPassNumber,
        const std::vector<MidiRecordingLoopTakePass>& passes)
    {
        if (regionOutTick_ <= regionInTick_ || !containsPass(passes, defaultPassNumber))
            return false;

        std::vector<MidiRecordingCompSegment> rebuilt;
        if (grid == MidiRecordingCompGrid::Custom)
        {
            rebuilt.push_back({ regionInTick_, regionOutTick_, defaultPassNumber });
        }
        else
        {
            const auto beatTicks = static_cast<std::int64_t>(ticksPerQuarterNote_) * 4
                / static_cast<std::int64_t>(denominator_);
            if (beatTicks <= 0)
                return false;
            const auto gridTicks = grid == MidiRecordingCompGrid::Bar
                ? beatTicks * static_cast<std::int64_t>(numerator_)
                : beatTicks;
            if (gridTicks <= 0)
                return false;

            auto cursor = regionInTick_;
            while (cursor < regionOutTick_)
            {
                const auto quotient = cursor / gridTicks;
                auto nextBoundary = (quotient + 1) * gridTicks;
                if (nextBoundary <= cursor)
                    nextBoundary = cursor + gridTicks;
                const auto endTick = std::min(regionOutTick_, nextBoundary);
                if (endTick <= cursor)
                    return false;
                rebuilt.push_back({ cursor, endTick, defaultPassNumber });
                cursor = endTick;
            }
        }

        if (rebuilt.empty())
            return false;
        grid_ = grid;
        segments_ = std::move(rebuilt);
        return true;
    }

    bool MidiRecordingLoopCompState::containsPass(
        const std::vector<MidiRecordingLoopTakePass>& passes,
        int passNumber) noexcept
    {
        if (passNumber <= 0)
            return false;
        return std::any_of(passes.begin(), passes.end(), [passNumber](const auto& pass)
        {
            return pass.passNumber == passNumber;
        });
    }

    std::vector<MidiRecordingControllerEvent> MidiRecordingLoopCompState::buildControllerEvents(
        std::uint64_t generation,
        const std::vector<MidiRecordingLoopTakePass>& passes) const
    {
        std::vector<MidiRecordingControllerEvent> result;
        if (!matches(generation) || !active_)
            return result;

        for (const auto& segment : segments_)
        {
            const auto passIt = std::find_if(passes.begin(), passes.end(), [&](const auto& pass)
            {
                return pass.passNumber == segment.selectedPassNumber;
            });
            if (passIt == passes.end())
                continue;

            const auto normalized = normalizeMidiRecordingControllerEvents(passIt->controllerEvents);
            for (const auto& event : normalized)
            {
                if (event.tick >= segment.startTick && event.tick < segment.endTick)
                    result.push_back(event);
            }
        }
        return normalizeMidiRecordingControllerEvents(std::move(result));
    }

    std::vector<MidiRecordingPitchBendEvent> MidiRecordingLoopCompState::buildPitchBendEvents(
        std::uint64_t generation,
        const std::vector<MidiRecordingLoopTakePass>& passes) const
    {
        std::vector<MidiRecordingPitchBendEvent> result;
        if (!matches(generation) || !active_)
            return result;

        for (const auto& segment : segments_)
        {
            const auto passIt = std::find_if(passes.begin(), passes.end(), [&](const auto& pass)
            {
                return pass.passNumber == segment.selectedPassNumber;
            });
            if (passIt == passes.end())
                continue;

            const auto normalized = normalizeMidiRecordingPitchBendEvents(passIt->pitchBendEvents);
            for (const auto& event : normalized)
            {
                if (event.tick >= segment.startTick && event.tick < segment.endTick)
                    result.push_back(event);
            }
        }
        return normalizeMidiRecordingPitchBendEvents(std::move(result));
    }

    std::vector<MidiRecordingProgramChangeEvent> MidiRecordingLoopCompState::buildProgramChangeEvents(
        std::uint64_t generation,
        const std::vector<MidiRecordingLoopTakePass>& passes) const
    {
        std::vector<MidiRecordingProgramChangeEvent> result;
        if (!matches(generation) || !active_)
            return result;

        for (const auto& segment : segments_)
        {
            const auto passIt = std::find_if(passes.begin(), passes.end(), [&](const auto& pass)
            {
                return pass.passNumber == segment.selectedPassNumber;
            });
            if (passIt == passes.end())
                continue;

            const auto normalized = normalizeMidiRecordingProgramChangeEvents(passIt->programChangeEvents);
            for (const auto& event : normalized)
            {
                if (event.tick >= segment.startTick && event.tick < segment.endTick)
                    result.push_back(event);
            }
        }
        return normalizeMidiRecordingProgramChangeEvents(std::move(result));
    }

    std::vector<MidiRecordingChannelPressureEvent> MidiRecordingLoopCompState::buildChannelPressureEvents(
        std::uint64_t generation,
        const std::vector<MidiRecordingLoopTakePass>& passes) const
    {
        std::vector<MidiRecordingChannelPressureEvent> result;
        if (!matches(generation) || !active_)
            return result;

        for (const auto& segment : segments_)
        {
            const auto passIt = std::find_if(passes.begin(), passes.end(), [&](const auto& pass)
            {
                return pass.passNumber == segment.selectedPassNumber;
            });
            if (passIt == passes.end())
                continue;

            const auto normalized = normalizeMidiRecordingChannelPressureEvents(passIt->channelPressureEvents);
            for (const auto& event : normalized)
            {
                if (event.tick >= segment.startTick && event.tick < segment.endTick)
                    result.push_back(event);
            }
        }
        return normalizeMidiRecordingChannelPressureEvents(std::move(result));
    }

    std::vector<MidiRecordingPolyPressureEvent> MidiRecordingLoopCompState::buildPolyPressureEvents(
        std::uint64_t generation,
        const std::vector<MidiRecordingLoopTakePass>& passes) const
    {
        std::vector<MidiRecordingPolyPressureEvent> result;
        if (!matches(generation) || !active_)
            return result;

        for (const auto& segment : segments_)
        {
            const auto passIt = std::find_if(passes.begin(), passes.end(), [&](const auto& pass)
            {
                return pass.passNumber == segment.selectedPassNumber;
            });
            if (passIt == passes.end())
                continue;

            const auto normalized = normalizeMidiRecordingPolyPressureEvents(passIt->polyPressureEvents);
            for (const auto& event : normalized)
            {
                if (event.tick >= segment.startTick && event.tick < segment.endTick)
                    result.push_back(event);
            }
        }
        return normalizeMidiRecordingPolyPressureEvents(std::move(result));
    }

    std::vector<MidiRecording14BitControllerEvent> MidiRecordingLoopCompState::build14BitControllerEvents(
        std::uint64_t generation, const std::vector<MidiRecordingLoopTakePass>& passes) const
    {
        std::vector<MidiRecording14BitControllerEvent> result; if (!matches(generation) || !active_) return result;
        for (const auto& segment : segments_)
        {
            const auto passIt=std::find_if(passes.begin(),passes.end(),[&](const auto& pass){return pass.passNumber==segment.selectedPassNumber;});
            if(passIt==passes.end())continue;
            for(const auto& event:normalizeMidiRecording14BitControllerEvents(passIt->controller14BitEvents))
                if(event.tick>=segment.startTick&&event.tick<segment.endTick)result.push_back(event);
        }
        return normalizeMidiRecording14BitControllerEvents(std::move(result));
    }

    std::vector<MidiRecordingRpnEvent> MidiRecordingLoopCompState::buildRpnEvents(
        std::uint64_t generation, const std::vector<MidiRecordingLoopTakePass>& passes) const
    {
        std::vector<MidiRecordingRpnEvent> result; if(!matches(generation)||!active_)return result;
        for(const auto& segment:segments_)
        {
            const auto passIt=std::find_if(passes.begin(),passes.end(),[&](const auto& pass){return pass.passNumber==segment.selectedPassNumber;}); if(passIt==passes.end())continue;
            for(const auto& event:normalizeMidiRecordingRpnEvents(passIt->rpnEvents))if(event.tick>=segment.startTick&&event.tick<segment.endTick)result.push_back(event);
        }
        return normalizeMidiRecordingRpnEvents(std::move(result));
    }

    std::vector<MidiRecordingNrpnEvent> MidiRecordingLoopCompState::buildNrpnEvents(
        std::uint64_t generation, const std::vector<MidiRecordingLoopTakePass>& passes) const
    {
        std::vector<MidiRecordingNrpnEvent> result; if(!matches(generation)||!active_)return result;
        for(const auto& segment:segments_)
        {
            const auto passIt=std::find_if(passes.begin(),passes.end(),[&](const auto& pass){return pass.passNumber==segment.selectedPassNumber;}); if(passIt==passes.end())continue;
            for(const auto& event:normalizeMidiRecordingNrpnEvents(passIt->nrpnEvents))if(event.tick>=segment.startTick&&event.tick<segment.endTick)result.push_back(event);
        }
        return normalizeMidiRecordingNrpnEvents(std::move(result));
    }

}
