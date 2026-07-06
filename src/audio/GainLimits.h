#pragma once

#include <algorithm>
#include <cmath>

namespace mw::audio
{
    inline constexpr double kMainUiVolumeGainMaximum = 3.0;
    inline constexpr float kMainUiVolumeGainMaximumFloat = static_cast<float>(kMainUiVolumeGainMaximum);

    inline double sanitizeMainUiGain(double value)
    {
        return std::clamp(std::isfinite(value) ? value : 1.0, 0.0, kMainUiVolumeGainMaximum);
    }

    inline float sanitizeMainUiGain(float value)
    {
        return std::clamp(std::isfinite(value) ? value : 1.0f, 0.0f, kMainUiVolumeGainMaximumFloat);
    }
}
