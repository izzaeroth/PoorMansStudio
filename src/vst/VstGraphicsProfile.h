#pragma once

#include "vst/VstPluginTypes.h"

namespace mw::vst
{
    class VstGraphicsProfileDetector
    {
    public:
        static GraphicsProfile detect(bool firstLaunchAutoDetect);
    };
}
