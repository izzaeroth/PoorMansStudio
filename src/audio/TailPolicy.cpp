#include "audio/TailPolicy.h"

#include <algorithm>
#include <cmath>

namespace mw::audio
{
    TailTrackingSettings tailTrackingSettings(TailPolicyMode mode) noexcept
    {
        if (mode == TailPolicyMode::Preview)
            return {};

        TailTrackingSettings settings;
        settings.silenceThreshold = 1.5848932e-5f; // Approximately -96 dBFS.
        settings.silenceHoldSeconds = 1.0;
        settings.tailPadSeconds = 0.15;
        settings.maximumTailSeconds = 12.0;
        settings.policyName = "render";
        settings.thresholdDescription = "-96 dBFS";
        return settings;
    }

    TailScanPlan makeTailScanPlan(std::int64_t totalSamples,
                                  std::int64_t sourceSamples,
                                  int sampleRate,
                                  double maximumTailSeconds,
                                  TailPolicyMode mode) noexcept
    {
        const auto settings = tailTrackingSettings(mode);
        const auto safeTotalSamples = std::max<std::int64_t>(0, totalSamples);
        const auto safeSourceSamples = std::clamp<std::int64_t>(sourceSamples, 0, safeTotalSamples);
        const auto safeSampleRate = std::max(1, sampleRate);
        const auto safeMaximumTailSeconds = std::max(0.0, maximumTailSeconds);
        const auto maximumTailSamples = static_cast<std::int64_t>(
            std::ceil(safeMaximumTailSeconds * static_cast<double>(safeSampleRate)));

        TailScanPlan plan;
        plan.sourceSamples = safeSourceSamples;
        plan.scanEndSamples = std::min<std::int64_t>(safeTotalSamples, safeSourceSamples + maximumTailSamples);
        plan.silenceHoldSamples = std::max<std::int64_t>(1, static_cast<std::int64_t>(
            std::ceil(settings.silenceHoldSeconds * static_cast<double>(safeSampleRate))));
        plan.tailPadSamples = std::max<std::int64_t>(0, static_cast<std::int64_t>(
            std::ceil(settings.tailPadSeconds * static_cast<double>(safeSampleRate))));
        return plan;
    }

    TailScanDecision finalizeTailScan(const TailScanPlan& plan,
                                      std::int64_t totalSamples,
                                      std::int64_t lastAudibleSample,
                                      std::int64_t consecutiveSilentSamples) noexcept
    {
        const auto safeTotalSamples = std::max<std::int64_t>(0, totalSamples);
        TailScanDecision decision;
        decision.endedInConfirmedSilence = consecutiveSilentSamples >= plan.silenceHoldSamples;

        const auto lastAudibleKeep = decision.endedInConfirmedSilence
            ? (lastAudibleSample >= 0 ? lastAudibleSample + 1 + plan.tailPadSamples : plan.sourceSamples)
            : plan.scanEndSamples;
        decision.keepSamples = std::clamp<std::int64_t>(
            std::max<std::int64_t>(plan.sourceSamples, lastAudibleKeep),
            0,
            plan.scanEndSamples);
        decision.trimmedSamples = std::max<std::int64_t>(0, safeTotalSamples - decision.keepSamples);
        return decision;
    }
}
