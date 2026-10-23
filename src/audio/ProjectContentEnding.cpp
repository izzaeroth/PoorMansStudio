#include "audio/ProjectContentEnding.h"

#include "audio/GainLimits.h"
#include "core/Project.h"

#include <algorithm>
#include <cmath>

namespace mw::audio
{
    double contentEndingTicksToSeconds(std::int64_t ticks, int tempoBpm) noexcept
    {
        const auto safeTempo = tempoBpm > 0 ? tempoBpm : 120;
        const auto safeTicks = std::max<std::int64_t>(0, ticks);
        const auto beats = static_cast<double>(safeTicks) / static_cast<double>(mw::core::Project::ticksPerQuarterNote);
        return beats * 60.0 / static_cast<double>(safeTempo);
    }

    double calculateProjectContentEndingSeconds(const ProjectContentEndingInput& input) noexcept
    {
        const bool anySolo = std::any_of(
            input.tracks.begin(),
            input.tracks.end(),
            [](const auto& track) { return track.solo; });

        double contentEndSeconds = 0.0;
        for (const auto& track : input.tracks)
        {
            if (track.audioClipTrack || track.muted)
                continue;
            if (anySolo && !track.solo)
                continue;

            const auto effectiveGain = sanitizeMainUiGain(track.volume * input.masterVolume);
            if (effectiveGain <= 0.000001)
                continue;

            contentEndSeconds = std::max(
                contentEndSeconds,
                contentEndingTicksToSeconds(track.contentEndTick, input.tempoBpm));
        }

        for (const auto& clip : input.clips)
        {
            const ContentEndingTrack* track = nullptr;
            if (clip.trackIndex >= 0 && clip.trackIndex < static_cast<int>(input.tracks.size()))
                track = &input.tracks[static_cast<std::size_t>(clip.trackIndex)];

            if (track == nullptr)
            {
                if (anySolo)
                    continue;
            }
            else
            {
                if (track->muted)
                    continue;
                if (anySolo && !track->solo)
                    continue;
            }

            const double trackVolume = track != nullptr ? track->volume : 1.0;
            const double clipGain = std::isfinite(clip.gain) ? clip.gain : 1.0;
            const double masterVolume = std::isfinite(input.masterVolume) ? input.masterVolume : 1.0;
            if (sanitizeMainUiGain(trackVolume * clipGain * masterVolume) <= 0.000001)
                continue;

            const auto clipStartSeconds = contentEndingTicksToSeconds(clip.startTick, input.tempoBpm);
            const auto keptDurationSeconds = std::isfinite(clip.keptDurationSeconds)
                ? std::max(0.0, clip.keptDurationSeconds)
                : 0.0;
            contentEndSeconds = std::max(contentEndSeconds, clipStartSeconds + keptDurationSeconds);
        }

        return contentEndSeconds;
    }
}
