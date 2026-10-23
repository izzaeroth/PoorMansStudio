#include "midi/MidiPendingTakeReviewManager.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <unordered_set>

namespace mw::midi
{
    namespace
    {
        PendingMidiTakeReviewSummary makeSummary(const mw::core::Track& track,
                                                 int trackIndex,
                                                 const mw::core::PersistentMidiTakeReview& review,
                                                 MidiTakeReviewStorageState storageState)
        {
            PendingMidiTakeReviewSummary summary;
            summary.trackId = track.getStableId();
            summary.reviewSessionId = review.reviewSessionId;
            summary.trackIndex = trackIndex;
            summary.trackName = track.getName();
            summary.reviewName = review.reviewName;
            summary.reviewTags = review.reviewTags;
            summary.reviewNotes = review.reviewNotes;
            summary.storageState = storageState;
            summary.passCount = static_cast<int>(review.passes.size());
            summary.segmentCount = static_cast<int>(review.segments.size());
            summary.regionInTick = review.regionInTick;
            summary.regionOutTick = review.regionOutTick;
            summary.reviewMode = review.reviewMode;
            summary.compGrid = review.compGrid;
            summary.boundaryNoteMode = review.boundaryNoteMode;
            summary.sustainPedalMode = review.sustainPedalMode;
            summary.crossPassNoteMode = review.crossPassNoteMode;
            return summary;
        }

        std::optional<int> findTrackIndex(const mw::core::Project& project, mw::core::StableId trackId) noexcept
        {
            if (trackId == 0)
                return std::nullopt;
            const auto& tracks = project.getTracks();
            for (int index = 0; index < static_cast<int>(tracks.size()); ++index)
            {
                if (tracks[static_cast<std::size_t>(index)].getStableId() == trackId
                    && !tracks[static_cast<std::size_t>(index)].isAudioClipTrack())
                    return index;
            }
            return std::nullopt;
        }

        bool hasVisibleText(const std::string& text)
        {
            return std::any_of(text.begin(), text.end(), [](unsigned char c) { return !std::isspace(c); });
        }

        std::string trimCopy(const std::string& text)
        {
            auto first = text.begin();
            while (first != text.end() && std::isspace(static_cast<unsigned char>(*first)))
                ++first;
            auto last = text.end();
            while (last != first && std::isspace(static_cast<unsigned char>(*(last - 1))))
                --last;
            return std::string(first, last);
        }

        std::string lowerCopy(std::string text)
        {
            std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c)
            {
                return static_cast<char>(std::tolower(c));
            });
            return text;
        }

        bool reviewNameExistsCaseInsensitive(const mw::core::Track& track, const std::string& name)
        {
            const auto wanted = lowerCopy(name);
            const auto& active = track.getPendingMidiTakeReview();
            if (active.active && lowerCopy(active.reviewName) == wanted)
                return true;
            return std::any_of(track.getArchivedMidiTakeReviews().begin(),
                               track.getArchivedMidiTakeReviews().end(),
                               [&](const auto& review) { return lowerCopy(review.reviewName) == wanted; });
        }

        bool reviewSessionIdExists(const mw::core::Track& track, mw::core::StableId sessionId) noexcept
        {
            if (sessionId == 0)
                return true;
            const auto& active = track.getPendingMidiTakeReview();
            if (active.active && active.reviewSessionId == sessionId)
                return true;
            return std::any_of(track.getArchivedMidiTakeReviews().begin(),
                               track.getArchivedMidiTakeReviews().end(),
                               [sessionId](const auto& review) { return review.reviewSessionId == sessionId; });
        }

        std::string derivedReviewName(const mw::core::Track& track,
                                      const std::string& sourceName,
                                      const std::string& suffix)
        {
            const auto base = trimCopy(sourceName).empty() ? std::string("Take Review") : trimCopy(sourceName);
            auto candidate = base + " " + suffix;
            if (!reviewNameExistsCaseInsensitive(track, candidate))
                return candidate;
            for (int number = 2; ; ++number)
            {
                candidate = base + " " + suffix + " " + std::to_string(number);
                if (!reviewNameExistsCaseInsensitive(track, candidate))
                    return candidate;
            }
        }

        std::string duplicateReviewName(const mw::core::Track& track, const std::string& sourceName)
        {
            return derivedReviewName(track, sourceName, "Copy");
        }

        std::string snapshotReviewName(const mw::core::Track& track, const std::string& sourceName)
        {
            return derivedReviewName(track, sourceName, "Snapshot");
        }

        std::string movedReviewName(const mw::core::Track& destinationTrack, const std::string& sourceName)
        {
            const auto trimmed = trimCopy(sourceName);
            if (!trimmed.empty() && !reviewNameExistsCaseInsensitive(destinationTrack, sourceName))
                return sourceName;
            return derivedReviewName(destinationTrack, sourceName, "Move");
        }

        mw::core::PersistentMidiTakeReview* findReview(mw::core::Project& project,
                                                       mw::core::StableId trackId,
                                                       mw::core::StableId reviewSessionId) noexcept
        {
            const auto index = findTrackIndex(project, trackId);
            if (!index.has_value() || reviewSessionId == 0)
                return nullptr;
            auto& track = project.getTracks()[static_cast<std::size_t>(*index)];
            auto& active = track.getPendingMidiTakeReview();
            if (active.active && active.reviewSessionId == reviewSessionId)
                return &active;
            for (auto& archived : track.getArchivedMidiTakeReviews())
            {
                if (archived.reviewSessionId == reviewSessionId)
                    return &archived;
            }
            return nullptr;
        }
    }

    std::vector<PendingMidiTakeReviewSummary> buildPendingMidiTakeReviewSummaries(const mw::core::Project& project)
    {
        std::vector<PendingMidiTakeReviewSummary> summaries;
        const auto& tracks = project.getTracks();
        summaries.reserve(tracks.size());
        for (int index = 0; index < static_cast<int>(tracks.size()); ++index)
        {
            const auto& track = tracks[static_cast<std::size_t>(index)];
            if (track.isAudioClipTrack())
                continue;
            const auto& review = track.getPendingMidiTakeReview();
            if (review.active && !review.passes.empty())
                summaries.push_back(makeSummary(track, index, review, MidiTakeReviewStorageState::Active));
        }
        return summaries;
    }

    std::vector<PendingMidiTakeReviewSummary> buildMidiTakeReviewSummaries(const mw::core::Project& project)
    {
        std::vector<PendingMidiTakeReviewSummary> summaries;
        const auto& tracks = project.getTracks();
        for (int index = 0; index < static_cast<int>(tracks.size()); ++index)
        {
            const auto& track = tracks[static_cast<std::size_t>(index)];
            if (track.isAudioClipTrack())
                continue;
            const auto& active = track.getPendingMidiTakeReview();
            if (active.active && !active.passes.empty())
                summaries.push_back(makeSummary(track, index, active, MidiTakeReviewStorageState::Active));
            for (const auto& archived : track.getArchivedMidiTakeReviews())
            {
                if (archived.active && !archived.passes.empty())
                    summaries.push_back(makeSummary(track, index, archived, MidiTakeReviewStorageState::Archived));
            }
        }
        return summaries;
    }

    std::vector<MidiTakeReviewCopyDestination> buildCrossTrackMidiTakeReviewCopyDestinations(
        const mw::core::Project& project,
        mw::core::StableId sourceTrackId)
    {
        std::vector<MidiTakeReviewCopyDestination> destinations;
        if (sourceTrackId == 0 || !findTrackIndex(project, sourceTrackId).has_value())
            return destinations;

        const auto& tracks = project.getTracks();
        destinations.reserve(tracks.size());
        for (int index = 0; index < static_cast<int>(tracks.size()); ++index)
        {
            const auto& track = tracks[static_cast<std::size_t>(index)];
            if (track.isAudioClipTrack() || track.getStableId() == sourceTrackId)
                continue;
            MidiTakeReviewCopyDestination destination;
            destination.trackId = track.getStableId();
            destination.trackIndex = index;
            destination.trackName = track.getName();
            destinations.push_back(std::move(destination));
        }
        return destinations;
    }

    std::vector<std::string> normalizeMidiTakeReviewTags(const std::string& commaSeparatedTags)
    {
        std::vector<std::string> normalized;
        std::unordered_set<std::string> seen;
        std::stringstream stream(commaSeparatedTags);
        std::string token;
        while (std::getline(stream, token, ','))
        {
            auto tag = trimCopy(token);
            if (tag.empty())
                continue;
            const auto key = lowerCopy(tag);
            if (seen.insert(key).second)
                normalized.push_back(std::move(tag));
        }
        return normalized;
    }

    std::vector<PendingMidiTakeReviewSummary> filterMidiTakeReviewSummaries(
        const std::vector<PendingMidiTakeReviewSummary>& summaries,
        const std::string& searchText,
        MidiTakeReviewLibraryFilter filter)
    {
        std::vector<std::string> terms;
        std::stringstream termStream(lowerCopy(searchText));
        std::string term;
        while (termStream >> term)
            terms.push_back(std::move(term));

        std::vector<PendingMidiTakeReviewSummary> filtered;
        filtered.reserve(summaries.size());
        for (const auto& summary : summaries)
        {
            if (filter == MidiTakeReviewLibraryFilter::Active
                && summary.storageState != MidiTakeReviewStorageState::Active)
                continue;
            if (filter == MidiTakeReviewLibraryFilter::Archived
                && summary.storageState != MidiTakeReviewStorageState::Archived)
                continue;

            std::string searchable = summary.trackName + "\n" + summary.reviewName + "\n" + summary.reviewNotes;
            for (const auto& tag : summary.reviewTags)
                searchable += "\n" + tag;
            searchable = lowerCopy(std::move(searchable));

            const bool allTermsMatch = std::all_of(terms.begin(), terms.end(), [&](const auto& wanted)
            {
                return searchable.find(wanted) != std::string::npos;
            });
            if (allTermsMatch)
                filtered.push_back(summary);
        }
        return filtered;
    }

    std::optional<int> findPendingMidiTakeReviewTrackIndexByStableId(const mw::core::Project& project,
                                                                     mw::core::StableId trackId) noexcept
    {
        const auto index = findTrackIndex(project, trackId);
        if (!index.has_value())
            return std::nullopt;
        const auto& review = project.getTracks()[static_cast<std::size_t>(*index)].getPendingMidiTakeReview();
        return review.active && !review.passes.empty() ? index : std::nullopt;
    }

    bool discardPendingMidiTakeReviewByStableId(mw::core::Project& project, mw::core::StableId trackId) noexcept
    {
        const auto index = findPendingMidiTakeReviewTrackIndexByStableId(project, trackId);
        if (!index.has_value())
            return false;
        project.getTracks()[static_cast<std::size_t>(*index)].clearPendingMidiTakeReview();
        return true;
    }

    bool archivePendingMidiTakeReviewByStableId(mw::core::Project& project, mw::core::StableId trackId) noexcept
    {
        const auto index = findPendingMidiTakeReviewTrackIndexByStableId(project, trackId);
        if (!index.has_value())
            return false;
        auto& track = project.getTracks()[static_cast<std::size_t>(*index)];
        auto snapshot = track.getPendingMidiTakeReview();
        if (snapshot.reviewSessionId == 0)
            snapshot.reviewSessionId = mw::core::allocateStableId();
        if (snapshot.reviewName.empty())
            snapshot.reviewName = "Take Review " + std::to_string(track.getArchivedMidiTakeReviews().size() + 1U);
        track.getArchivedMidiTakeReviews().push_back(std::move(snapshot));
        track.clearPendingMidiTakeReview();
        return true;
    }

    std::optional<mw::core::StableId> snapshotActiveMidiTakeReviewByStableIds(
        mw::core::Project& project,
        mw::core::StableId trackId,
        mw::core::StableId reviewSessionId)
    {
        const auto index = findTrackIndex(project, trackId);
        if (!index.has_value() || reviewSessionId == 0)
            return std::nullopt;

        auto& track = project.getTracks()[static_cast<std::size_t>(*index)];
        const auto& active = track.getPendingMidiTakeReview();
        if (!active.active || active.passes.empty() || active.reviewSessionId != reviewSessionId)
            return std::nullopt;

        auto snapshot = active;
        snapshot.reviewName = snapshotReviewName(track, active.reviewName);
        do
        {
            snapshot.reviewSessionId = mw::core::allocateStableId();
        }
        while (reviewSessionIdExists(track, snapshot.reviewSessionId));

        const auto snapshotId = snapshot.reviewSessionId;
        track.getArchivedMidiTakeReviews().push_back(std::move(snapshot));
        return snapshotId;
    }

    bool restoreArchivedMidiTakeReviewByStableIds(mw::core::Project& project,
                                                   mw::core::StableId trackId,
                                                   mw::core::StableId reviewSessionId) noexcept
    {
        const auto index = findTrackIndex(project, trackId);
        if (!index.has_value() || reviewSessionId == 0)
            return false;
        auto& track = project.getTracks()[static_cast<std::size_t>(*index)];
        if (track.getPendingMidiTakeReview().active)
            return false;
        auto& archives = track.getArchivedMidiTakeReviews();
        const auto it = std::find_if(archives.begin(), archives.end(), [reviewSessionId](const auto& review)
        {
            return review.reviewSessionId == reviewSessionId;
        });
        if (it == archives.end())
            return false;
        auto restored = std::move(*it);
        archives.erase(it);
        track.setPendingMidiTakeReview(std::move(restored));
        return true;
    }

    bool deleteArchivedMidiTakeReviewByStableIds(mw::core::Project& project,
                                                  mw::core::StableId trackId,
                                                  mw::core::StableId reviewSessionId) noexcept
    {
        const auto index = findTrackIndex(project, trackId);
        if (!index.has_value() || reviewSessionId == 0)
            return false;
        auto& archives = project.getTracks()[static_cast<std::size_t>(*index)].getArchivedMidiTakeReviews();
        const auto it = std::find_if(archives.begin(), archives.end(), [reviewSessionId](const auto& review)
        {
            return review.reviewSessionId == reviewSessionId;
        });
        if (it == archives.end())
            return false;
        archives.erase(it);
        return true;
    }

    std::optional<mw::core::StableId> duplicateArchivedMidiTakeReviewByStableIds(
        mw::core::Project& project,
        mw::core::StableId trackId,
        mw::core::StableId reviewSessionId)
    {
        const auto index = findTrackIndex(project, trackId);
        if (!index.has_value() || reviewSessionId == 0)
            return std::nullopt;

        auto& track = project.getTracks()[static_cast<std::size_t>(*index)];
        auto& archives = track.getArchivedMidiTakeReviews();
        const auto sourceIt = std::find_if(archives.begin(), archives.end(), [reviewSessionId](const auto& review)
        {
            return review.active && review.reviewSessionId == reviewSessionId;
        });
        if (sourceIt == archives.end())
            return std::nullopt;

        auto duplicate = *sourceIt;
        duplicate.reviewName = duplicateReviewName(track, sourceIt->reviewName);
        do
        {
            duplicate.reviewSessionId = mw::core::allocateStableId();
        }
        while (reviewSessionIdExists(track, duplicate.reviewSessionId));

        const auto duplicateId = duplicate.reviewSessionId;
        archives.push_back(std::move(duplicate));
        return duplicateId;
    }

    std::optional<mw::core::StableId> copyArchivedMidiTakeReviewToTrackByStableIds(
        mw::core::Project& project,
        mw::core::StableId sourceTrackId,
        mw::core::StableId reviewSessionId,
        mw::core::StableId destinationTrackId)
    {
        if (sourceTrackId == 0 || destinationTrackId == 0 || reviewSessionId == 0
            || sourceTrackId == destinationTrackId)
            return std::nullopt;

        const auto sourceIndex = findTrackIndex(project, sourceTrackId);
        const auto destinationIndex = findTrackIndex(project, destinationTrackId);
        if (!sourceIndex.has_value() || !destinationIndex.has_value())
            return std::nullopt;

        const auto& sourceTrack = project.getTracks()[static_cast<std::size_t>(*sourceIndex)];
        const auto& sourceArchives = sourceTrack.getArchivedMidiTakeReviews();
        const auto sourceIt = std::find_if(sourceArchives.begin(), sourceArchives.end(), [reviewSessionId](const auto& review)
        {
            return review.active && !review.passes.empty() && review.reviewSessionId == reviewSessionId;
        });
        if (sourceIt == sourceArchives.end())
            return std::nullopt;

        auto& destinationTrack = project.getTracks()[static_cast<std::size_t>(*destinationIndex)];
        auto copied = *sourceIt;
        copied.reviewName = duplicateReviewName(destinationTrack, sourceIt->reviewName);
        do
        {
            copied.reviewSessionId = mw::core::allocateStableId();
        }
        while (copied.reviewSessionId == reviewSessionId
               || reviewSessionIdExists(destinationTrack, copied.reviewSessionId));

        const auto copiedId = copied.reviewSessionId;
        destinationTrack.getArchivedMidiTakeReviews().push_back(std::move(copied));
        return copiedId;
    }

    std::optional<mw::core::StableId> moveArchivedMidiTakeReviewToTrackByStableIds(
        mw::core::Project& project,
        mw::core::StableId sourceTrackId,
        mw::core::StableId reviewSessionId,
        mw::core::StableId destinationTrackId)
    {
        if (sourceTrackId == 0 || destinationTrackId == 0 || reviewSessionId == 0
            || sourceTrackId == destinationTrackId)
            return std::nullopt;

        const auto sourceIndex = findTrackIndex(project, sourceTrackId);
        const auto destinationIndex = findTrackIndex(project, destinationTrackId);
        if (!sourceIndex.has_value() || !destinationIndex.has_value())
            return std::nullopt;

        auto& sourceTrack = project.getTracks()[static_cast<std::size_t>(*sourceIndex)];
        auto& destinationTrack = project.getTracks()[static_cast<std::size_t>(*destinationIndex)];
        if (reviewSessionIdExists(destinationTrack, reviewSessionId))
            return std::nullopt;

        auto& sourceArchives = sourceTrack.getArchivedMidiTakeReviews();
        const auto sourceIt = std::find_if(sourceArchives.begin(), sourceArchives.end(), [reviewSessionId](const auto& review)
        {
            return review.active && !review.passes.empty() && review.reviewSessionId == reviewSessionId;
        });
        if (sourceIt == sourceArchives.end())
            return std::nullopt;

        // Build the complete destination value before mutating either track. The destination push is the
        // only potentially allocating step; source removal occurs only after it succeeds, making the
        // project-data operation atomic from the caller's perspective.
        auto moved = *sourceIt;
        moved.reviewName = movedReviewName(destinationTrack, sourceIt->reviewName);
        destinationTrack.getArchivedMidiTakeReviews().push_back(std::move(moved));
        sourceArchives.erase(sourceIt);
        return reviewSessionId;
    }

    bool updateMidiTakeReviewMetadataByStableIds(mw::core::Project& project,
                                                  mw::core::StableId trackId,
                                                  mw::core::StableId reviewSessionId,
                                                  const std::string& commaSeparatedTags,
                                                  const std::string& notes)
    {
        auto* review = findReview(project, trackId, reviewSessionId);
        if (review == nullptr)
            return false;
        review->reviewTags = normalizeMidiTakeReviewTags(commaSeparatedTags);
        review->reviewNotes = notes;
        return true;
    }

    bool renameMidiTakeReviewByStableIds(mw::core::Project& project,
                                         mw::core::StableId trackId,
                                         mw::core::StableId reviewSessionId,
                                         const std::string& newName)
    {
        if (reviewSessionId == 0 || !hasVisibleText(newName))
            return false;
        const auto index = findTrackIndex(project, trackId);
        if (!index.has_value())
            return false;
        auto& track = project.getTracks()[static_cast<std::size_t>(*index)];
        auto& active = track.getPendingMidiTakeReview();
        if (active.active && active.reviewSessionId == reviewSessionId)
        {
            active.reviewName = newName;
            return true;
        }
        for (auto& archived : track.getArchivedMidiTakeReviews())
        {
            if (archived.reviewSessionId == reviewSessionId)
            {
                archived.reviewName = newName;
                return true;
            }
        }
        return false;
    }
}
