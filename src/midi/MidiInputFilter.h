#pragma once

#include <algorithm>

namespace mw::midi
{
    inline int sanitizeMidiInputChannelFilter(int channel) noexcept
    {
        return channel <= 0 ? 0 : std::clamp(channel, 1, 16);
    }

    inline bool midiInputChannelAccepted(int channelFilter, int messageChannel) noexcept
    {
        const int acceptedFilter = sanitizeMidiInputChannelFilter(channelFilter);
        const int acceptedMessageChannel = std::clamp(messageChannel, 1, 16);
        return acceptedFilter == 0 || acceptedMessageChannel == acceptedFilter;
    }
}
