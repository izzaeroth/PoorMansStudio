#pragma once

#include <juce_core/juce_core.h>

#include "app/VersionNumber.h"

namespace mw::app
{
    inline constexpr const char* appVersion = versionNumber;
    inline constexpr const char* appVersionLabel = versionLabel;

    inline juce::String applicationTitle()
    {
        return juce::String("Poor Man's Studio ") + appVersionLabel;
    }
}
