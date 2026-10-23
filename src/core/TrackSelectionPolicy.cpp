#include "core/TrackSelectionPolicy.h"

#include <algorithm>

namespace mw::core
{
    TrackSelectionSnapshot captureTrackSelection(const std::vector<Track>& tracks, int selectedIndex) noexcept
    {
        TrackSelectionSnapshot snapshot;
        snapshot.index = selectedIndex;

        if (selectedIndex >= 0 && selectedIndex < static_cast<int>(tracks.size()))
            snapshot.stableId = tracks[static_cast<std::size_t>(selectedIndex)].getStableId();

        return snapshot;
    }

    int resolveTrackSelection(const std::vector<Track>& tracks, const TrackSelectionSnapshot& snapshot) noexcept
    {
        if (tracks.empty())
            return -1;

        if (snapshot.stableId != 0)
        {
            const auto match = std::find_if(
                tracks.begin(),
                tracks.end(),
                [&snapshot](const Track& track)
                {
                    return track.getStableId() == snapshot.stableId;
                }
            );

            if (match != tracks.end())
                return static_cast<int>(std::distance(tracks.begin(), match));
        }

        if (snapshot.index >= 0)
            return std::clamp(snapshot.index, 0, static_cast<int>(tracks.size()) - 1);

        return 0;
    }
}
