#pragma once

#include <cstdint>

namespace mw::audio
{
    enum class TailPolicyMode
    {
        Preview,
        Render
    };

    struct TailTrackingSettings
    {
        float silenceThreshold = 1.0e-4f;
        double silenceHoldSeconds = 0.30;
        double tailPadSeconds = 0.05;
        double maximumTailSeconds = 4.0;
        const char* policyName = "preview";
        const char* thresholdDescription = "-80 dBFS";
    };

    struct TailScanPlan
    {
        std::int64_t sourceSamples = 0;
        std::int64_t scanEndSamples = 0;
        std::int64_t silenceHoldSamples = 1;
        std::int64_t tailPadSamples = 0;
    };

    struct TailScanDecision
    {
        bool endedInConfirmedSilence = false;
        std::int64_t keepSamples = 0;
        std::int64_t trimmedSamples = 0;
    };

    TailTrackingSettings tailTrackingSettings(TailPolicyMode mode) noexcept;

    TailScanPlan makeTailScanPlan(std::int64_t totalSamples,
                                  std::int64_t sourceSamples,
                                  int sampleRate,
                                  double maximumTailSeconds,
                                  TailPolicyMode mode) noexcept;

    TailScanDecision finalizeTailScan(const TailScanPlan& plan,
                                      std::int64_t totalSamples,
                                      std::int64_t lastAudibleSample,
                                      std::int64_t consecutiveSilentSamples) noexcept;
}
