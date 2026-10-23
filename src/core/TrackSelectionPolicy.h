#pragma once

#include "core/StableId.h"
#include "core/Track.h"

#include <vector>

namespace mw::core
{
    struct TrackSelectionSnapshot
    {
        StableId stableId = 0;
        int index = -1;
    };

    TrackSelectionSnapshot captureTrackSelection(const std::vector<Track>& tracks, int selectedIndex) noexcept;
    int resolveTrackSelection(const std::vector<Track>& tracks, const TrackSelectionSnapshot& snapshot) noexcept;
}
