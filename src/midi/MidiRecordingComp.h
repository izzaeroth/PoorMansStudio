#pragma once

#include "core/StableId.h"
#include "midi/MidiRecordingTakeReview.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mw::midi
{
    enum class MidiRecordingCompGrid
    {
        Beat = 1,
        Bar = 2,
        Custom = 3
    };

    enum class MidiRecordingCompBoundaryNoteMode
    {
        Cut = 1,
        PreserveSamePass = 2
    };

    enum class MidiRecordingCompCrossPassNoteMode
    {
        Off = 1,
        BridgeExact = 2
    };

    inline constexpr const char midiRecordingCompBoundaryNotesTooltip[] =
        "Controls notes that cross comp-segment boundaries. Cut ends them at every segment boundary. Preserve Same Pass lets a real note continue through adjacent segments assigned to the same retained pass; pass changes and Loop Out still stop it, and no new Note On is invented.";
    inline constexpr const char midiRecordingCompCutTooltip[] =
        "Cut ends selected notes at each comp-segment boundary. This preserves the original v0.67.21-v0.67.23 comp behavior.";
    inline constexpr const char midiRecordingCompPreserveSamePassTooltip[] =
        "Preserve Same Pass lets a real note continue through adjacent comp segments assigned to the same retained pass. It still stops at the first pass change or Loop Out unless the opt-in Cross-Pass Notes rule bridges an exact matching boundary note.";
    inline constexpr const char midiRecordingCompCrossPassNotesTooltip[] =
        "Controls conservative note reconstruction at a comp boundary where the selected retained pass changes. Off preserves the existing no-cross-pass-fusion rule. Bridge Exact is opt-in and works only with Boundary Notes = Preserve Same Pass: it bridges only one unambiguous same-pitch/same-channel note that reaches the boundary to one matching real Note On exactly at that boundary in the next pass.";
    inline constexpr const char midiRecordingCompCrossPassOffTooltip[] =
        "Off preserves the existing comp contract: a pass-change boundary always stops the earlier note and the next pass keeps its own real Note On.";
    inline constexpr const char midiRecordingCompCrossPassBridgeExactTooltip[] =
        "Bridge Exact is a conservative opt-in reconstruction rule. With Preserve Same Pass, one unambiguous same-pitch/same-channel note reaching a pass-change boundary can continue through one matching real Note On exactly at that boundary. The earlier note keeps its velocity and articulation; the matched boundary Note On is suppressed. Gaps, near-matches, ambiguity, different pitch/channel, and Cut mode are never bridged.";

    struct MidiRecordingCompSegment
    {
        std::int64_t startTick = 0;
        std::int64_t endTick = 0;
        int selectedPassNumber = 0;
    };

    struct MidiRecordingCompBuildResult
    {
        std::vector<mw::core::NoteEvent> notes;
        int clippedNoteCount = 0;
        int crossPassBridgedNoteCount = 0;
    };

    class MidiRecordingLoopCompState final
    {
    public:
        std::uint64_t begin(mw::core::StableId projectId,
                            mw::core::StableId trackId,
                            std::uint64_t reviewGeneration,
                            std::int64_t regionInTick,
                            std::int64_t regionOutTick,
                            int ticksPerQuarterNote,
                            int timeSignatureNumerator,
                            int timeSignatureDenominator,
                            MidiRecordingCompGrid grid,
                            int defaultPassNumber,
                            const std::vector<MidiRecordingLoopTakePass>& passes);
        void cancel() noexcept;
        bool restore(mw::core::StableId projectId,
                     mw::core::StableId trackId,
                     std::uint64_t reviewGeneration,
                     std::int64_t regionInTick,
                     std::int64_t regionOutTick,
                     int ticksPerQuarterNote,
                     int timeSignatureNumerator,
                     int timeSignatureDenominator,
                     MidiRecordingCompGrid grid,
                     std::vector<MidiRecordingCompSegment> segments,
                     const std::vector<MidiRecordingLoopTakePass>& passes);

        bool setGrid(std::uint64_t generation,
                     MidiRecordingCompGrid grid,
                     int defaultPassNumber,
                     const std::vector<MidiRecordingLoopTakePass>& passes);
        bool assignPass(std::uint64_t generation,
                        std::size_t segmentIndex,
                        int passNumber,
                        const std::vector<MidiRecordingLoopTakePass>& passes) noexcept;
        bool setBoundaryNoteMode(std::uint64_t generation,
                                 MidiRecordingCompBoundaryNoteMode mode) noexcept;
        bool setSustainPedalMode(std::uint64_t generation,
                                 MidiRecordingSustainPedalMode mode) noexcept;
        bool setCrossPassNoteMode(std::uint64_t generation,
                                  MidiRecordingCompCrossPassNoteMode mode) noexcept;
        bool addCustomBoundary(std::uint64_t generation, std::int64_t tick) noexcept;
        bool removeCustomBoundary(std::uint64_t generation, std::int64_t tick) noexcept;

        bool isActive() const noexcept;
        bool matches(std::uint64_t generation) const noexcept;
        bool matchesProjectTrack(mw::core::StableId projectId,
                                 mw::core::StableId trackId) const noexcept;
        std::uint64_t generation() const noexcept;
        mw::core::StableId projectId() const noexcept;
        mw::core::StableId trackId() const noexcept;
        MidiRecordingCompGrid grid() const noexcept;
        MidiRecordingCompBoundaryNoteMode boundaryNoteMode() const noexcept;
        MidiRecordingSustainPedalMode sustainPedalMode() const noexcept;
        MidiRecordingCompCrossPassNoteMode crossPassNoteMode() const noexcept;
        std::int64_t regionInTick() const noexcept;
        std::int64_t regionOutTick() const noexcept;
        const std::vector<MidiRecordingCompSegment>& segments() const noexcept;
        std::vector<std::int64_t> customBoundaries() const;

        MidiRecordingCompBuildResult buildNotes(
            std::uint64_t generation,
            const std::vector<MidiRecordingLoopTakePass>& passes) const;
        std::vector<MidiRecordingControllerEvent> buildControllerEvents(
            std::uint64_t generation,
            const std::vector<MidiRecordingLoopTakePass>& passes) const;
        std::vector<MidiRecordingPitchBendEvent> buildPitchBendEvents(
            std::uint64_t generation,
            const std::vector<MidiRecordingLoopTakePass>& passes) const;
        std::vector<MidiRecordingProgramChangeEvent> buildProgramChangeEvents(
            std::uint64_t generation,
            const std::vector<MidiRecordingLoopTakePass>& passes) const;
        std::vector<MidiRecordingChannelPressureEvent> buildChannelPressureEvents(
            std::uint64_t generation,
            const std::vector<MidiRecordingLoopTakePass>& passes) const;
        std::vector<MidiRecordingPolyPressureEvent> buildPolyPressureEvents(
            std::uint64_t generation,
            const std::vector<MidiRecordingLoopTakePass>& passes) const;
        std::vector<MidiRecording14BitControllerEvent> build14BitControllerEvents(
            std::uint64_t generation, const std::vector<MidiRecordingLoopTakePass>& passes) const;
        std::vector<MidiRecordingRpnEvent> buildRpnEvents(
            std::uint64_t generation, const std::vector<MidiRecordingLoopTakePass>& passes) const;
        std::vector<MidiRecordingNrpnEvent> buildNrpnEvents(
            std::uint64_t generation, const std::vector<MidiRecordingLoopTakePass>& passes) const;

    private:
        bool rebuildSegments(MidiRecordingCompGrid grid,
                             int defaultPassNumber,
                             const std::vector<MidiRecordingLoopTakePass>& passes);
        static bool containsPass(const std::vector<MidiRecordingLoopTakePass>& passes,
                                 int passNumber) noexcept;

        std::uint64_t generation_ = 0;
        mw::core::StableId projectId_ = 0;
        mw::core::StableId trackId_ = 0;
        std::int64_t regionInTick_ = 0;
        std::int64_t regionOutTick_ = 0;
        int ticksPerQuarterNote_ = 960;
        int numerator_ = 4;
        int denominator_ = 4;
        MidiRecordingCompGrid grid_ = MidiRecordingCompGrid::Beat;
        MidiRecordingCompBoundaryNoteMode boundaryNoteMode_ = MidiRecordingCompBoundaryNoteMode::Cut;
        MidiRecordingSustainPedalMode sustainPedalMode_ = MidiRecordingSustainPedalMode::Ignore;
        MidiRecordingCompCrossPassNoteMode crossPassNoteMode_ = MidiRecordingCompCrossPassNoteMode::Off;
        bool active_ = false;
        std::vector<MidiRecordingCompSegment> segments_;
    };
}
