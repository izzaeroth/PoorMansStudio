#pragma once

#include <juce_core/juce_core.h>

namespace mw::app
{
    inline constexpr const char* appVersion = "0.63.0";
    inline constexpr const char* appVersionLabel = "v0.63.0";

    inline juce::String applicationTitle()
    {
        return juce::String("Poor Man's Studio ") + appVersionLabel;
    }
}
