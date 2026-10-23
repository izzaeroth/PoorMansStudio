#pragma once

#include "core/Project.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace mw::midi
{
    enum class MidiTakeReviewStorageState
    {
        Active,
        Archived
    };

    enum class MidiTakeReviewLibraryFilter
    {
        All,
        Active,
        Archived
    };

    struct PendingMidiTakeReviewSummary
    {
        mw::core::StableId trackId = 0;
        mw::core::StableId reviewSessionId = 0;
        int trackIndex = -1;
        std::string trackName;
        std::string reviewName;
        std::vector<std::string> reviewTags;
        std::string reviewNotes;
        MidiTakeReviewStorageState storageState = MidiTakeReviewStorageState::Active;
        int passCount = 0;
        int segmentCount = 0;
        std::int64_t regionInTick = 0;
        std::int64_t regionOutTick = 0;
        mw::core::PersistentMidiTakeReviewMode reviewMode = mw::core::PersistentMidiTakeReviewMode::WholePass;
        mw::core::PersistentMidiCompGrid compGrid = mw::core::PersistentMidiCompGrid::Beat;
        mw::core::PersistentMidiCompBoundaryNoteMode boundaryNoteMode = mw::core::PersistentMidiCompBoundaryNoteMode::Cut;
        mw::core::PersistentMidiSustainPedalMode sustainPedalMode = mw::core::PersistentMidiSustainPedalMode::Ignore;
        mw::core::PersistentMidiCompCrossPassNoteMode crossPassNoteMode = mw::core::PersistentMidiCompCrossPassNoteMode::Off;
    };

    struct MidiTakeReviewCopyDestination
    {
        mw::core::StableId trackId = 0;
        int trackIndex = -1;
        std::string trackName;
    };

    inline constexpr const char pendingTakeReviewsIndicatorTooltip[] =
        "Shows how many MIDI tracks contain active unresolved saved TAKE REVIEW material. Archived reviews are saved in the Take Review Manager but do not increase this count.";
    inline constexpr const char pendingTakeReviewsTrackTooltip[] =
        "Choose a named active or archived MIDI TAKE REVIEW. Ownership is resolved by stable track identity and stable review identity, not list position.";
    inline constexpr const char pendingTakeReviewsResumeTooltip[] =
        "Resume the selected Active review without changing project notes or dirtying the project by itself. Archived reviews must be restored first.";
    inline constexpr const char pendingTakeReviewsArchiveTooltip[] =
        "Move the selected Active review into named project archive storage. This frees the track for another Review recording and does not add MIDI notes. Use Snapshot Active instead when you want the Active editor to remain open.";
    inline constexpr const char pendingTakeReviewsSnapshotActiveTooltip[] =
        "Save an independent Archived snapshot of the selected Active review while leaving the Active review unresolved and intact. The snapshot gets a new stable review identity and deterministic Snapshot name; project MIDI notes are not changed. Save Tags/Notes first if you want unsaved metadata editor changes included.";
    inline constexpr const char pendingTakeReviewsRestoreTooltip[] =
        "Move the selected Archived review back into the track's one Active TAKE REVIEW slot. Restore is rejected while that track already has an Active review.";
    inline constexpr const char pendingTakeReviewsRenameTooltip[] =
        "Rename the selected Active or Archived review. Renaming changes only persisted review metadata and never changes MIDI notes or comp contents.";
    inline constexpr const char pendingTakeReviewsDiscardTooltip[] =
        "Permanently remove the selected Active review without adding its notes. Other pending reviews and archived reviews are not changed.";
    inline constexpr const char pendingTakeReviewsDeleteArchiveTooltip[] =
        "Permanently delete the selected Archived review snapshot. Project MIDI notes and other active/archived reviews are not changed.";
    inline constexpr const char pendingTakeReviewsDuplicateArchiveTooltip[] =
        "Create an independent Archived copy of the selected Take Review on the same MIDI track. The copy gets a new stable review identity and deterministic name while retaining passes, comp choices, CC64 data, Tags, and Notes. Project MIDI notes are not changed.";
    inline constexpr const char pendingTakeReviewsCopyToTrackTooltip[] =
        "Create an independent Archived copy of the selected Take Review on a different MIDI track in this project. The destination copy gets a new stable review identity and deterministic name; source review, Active reviews, Pending Takes, and project MIDI notes are unchanged.";
    inline constexpr const char pendingTakeReviewsCopyDestinationTooltip[] =
        "Choose a different MIDI track to receive the Archived copy. AudioClip tracks and the source track are excluded; an existing Active review on the destination track is left unchanged.";
    inline constexpr const char pendingTakeReviewsCopyConfirmTooltip[] =
        "Copy the selected Archived review to the chosen MIDI track as a new independent Archived review. The source review and project MIDI notes are not changed.";
    inline constexpr const char pendingTakeReviewsMoveToTrackTooltip[] =
        "Move the selected Archived Take Review to a different MIDI track in this project. The review keeps its stable identity and full saved payload but is removed from the source track; Active reviews, Pending Takes, and project MIDI notes are unchanged.";
    inline constexpr const char pendingTakeReviewsMoveDestinationTooltip[] =
        "Choose a different MIDI track to own this Archived review. AudioClip tracks and the current source track are excluded; an existing Active review on the destination track is left unchanged.";
    inline constexpr const char pendingTakeReviewsMoveConfirmTooltip[] =
        "Move the selected Archived review to the chosen MIDI track. The source archive is removed only after destination validation succeeds; the review keeps its stable identity and project MIDI notes are not changed.";
    inline constexpr const char pendingTakeReviewsRefreshTooltip[] =
        "Refresh the Take Review Manager from the current project. Renames, reorders, archives, restores, removals, and resolved reviews use stable identities.";
    inline constexpr const char takeReviewSearchTooltip[] =
        "Filter Take Reviews by track name, review name, tags, or notes. Search is case-insensitive, multiple words use AND matching, and filtering never changes project data.";
    inline constexpr const char takeReviewShowFilterTooltip[] =
        "Show All, Active, or Archived Take Reviews. Filtering changes only the manager view and never changes saved project data.";
    inline constexpr const char takeReviewTagsTooltip[] =
        "Add comma-separated searchable labels to the selected Take Review. Tags are normalized and saved with the project but never change MIDI, comp, Boundary Notes, Cross-Pass Notes, or CC64 behavior.";
    inline constexpr const char takeReviewNotesTooltip[] =
        "Save descriptive text with the selected Take Review. Notes are organizational project metadata only and do not alter the recorded performance.";
    inline constexpr const char takeReviewSaveMetadataTooltip[] =
        "Save Tags and Notes for the selected Take Review. This marks persisted project metadata changed but does not create an Undo Take step or modify MIDI notes.";
    inline constexpr const char midiTakeReviewArchiveTooltip[] =
        "Move the current unresolved TAKE REVIEW into Archive storage and free this track for another Review recording. No MIDI notes are committed. Use Snapshot to keep the Active review open.";
    inline constexpr const char midiTakeReviewSnapshotTooltip[] =
        "Save an independent Archived snapshot of the current Active TAKE REVIEW while keeping the Active editor unresolved and intact. The snapshot gets a new stable identity and deterministic name; no project MIDI notes are changed.";

    std::vector<PendingMidiTakeReviewSummary> buildPendingMidiTakeReviewSummaries(const mw::core::Project& project);
    std::vector<PendingMidiTakeReviewSummary> buildMidiTakeReviewSummaries(const mw::core::Project& project);
    std::vector<MidiTakeReviewCopyDestination> buildCrossTrackMidiTakeReviewCopyDestinations(
        const mw::core::Project& project,
        mw::core::StableId sourceTrackId);
    std::vector<PendingMidiTakeReviewSummary> filterMidiTakeReviewSummaries(
        const std::vector<PendingMidiTakeReviewSummary>& summaries,
        const std::string& searchText,
        MidiTakeReviewLibraryFilter filter);
    std::vector<std::string> normalizeMidiTakeReviewTags(const std::string& commaSeparatedTags);
    std::optional<int> findPendingMidiTakeReviewTrackIndexByStableId(const mw::core::Project& project,
                                                                     mw::core::StableId trackId) noexcept;
    bool discardPendingMidiTakeReviewByStableId(mw::core::Project& project,
                                                 mw::core::StableId trackId) noexcept;
    bool archivePendingMidiTakeReviewByStableId(mw::core::Project& project,
                                                 mw::core::StableId trackId) noexcept;
    std::optional<mw::core::StableId> snapshotActiveMidiTakeReviewByStableIds(
        mw::core::Project& project,
        mw::core::StableId trackId,
        mw::core::StableId reviewSessionId);
    bool restoreArchivedMidiTakeReviewByStableIds(mw::core::Project& project,
                                                   mw::core::StableId trackId,
                                                   mw::core::StableId reviewSessionId) noexcept;
    bool deleteArchivedMidiTakeReviewByStableIds(mw::core::Project& project,
                                                  mw::core::StableId trackId,
                                                  mw::core::StableId reviewSessionId) noexcept;
    std::optional<mw::core::StableId> duplicateArchivedMidiTakeReviewByStableIds(
        mw::core::Project& project,
        mw::core::StableId trackId,
        mw::core::StableId reviewSessionId);
    std::optional<mw::core::StableId> copyArchivedMidiTakeReviewToTrackByStableIds(
        mw::core::Project& project,
        mw::core::StableId sourceTrackId,
        mw::core::StableId reviewSessionId,
        mw::core::StableId destinationTrackId);
    std::optional<mw::core::StableId> moveArchivedMidiTakeReviewToTrackByStableIds(
        mw::core::Project& project,
        mw::core::StableId sourceTrackId,
        mw::core::StableId reviewSessionId,
        mw::core::StableId destinationTrackId);
    bool updateMidiTakeReviewMetadataByStableIds(mw::core::Project& project,
                                                  mw::core::StableId trackId,
                                                  mw::core::StableId reviewSessionId,
                                                  const std::string& commaSeparatedTags,
                                                  const std::string& notes);
    bool renameMidiTakeReviewByStableIds(mw::core::Project& project,
                                         mw::core::StableId trackId,
                                         mw::core::StableId reviewSessionId,
                                         const std::string& newName);
}
