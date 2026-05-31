#include "vst/VstPluginTypes.h"

#include <sstream>

namespace mw::vst
{
    std::vector<std::string> VstCompatibilityFlags::labels() const
    {
        std::vector<std::string> result;
        if (usesOpenGL) result.push_back("OpenGL");
        if (usesDirect3D) result.push_back("Direct3D/DXGI");
        if (usesDirect2D) result.push_back("Direct2D/DWrite");
        if (usesVulkan) result.push_back("Vulkan");
        if (usesOpenCL) result.push_back("OpenCL");
        if (usesWebView) result.push_back("WebView2");
        if (usesCef) result.push_back("CEF/Chromium");
        if (usesNvidiaSpecific) result.push_back("NVIDIA-specific runtime");
        if (usesAmdSpecific) result.push_back("AMD-specific runtime");
        if (hasNativeWindowsUi) result.push_back("Windows UI");
        return result;
    }

    std::string VstCompatibilityFlags::summary() const
    {
        const auto values = labels();
        if (values.empty())
            return "No obvious GPU/UI indicators";

        std::string output;
        for (std::size_t i = 0; i < values.size(); ++i)
        {
            if (i > 0)
                output += ", ";
            output += values[i];
        }
        return output;
    }

    std::string GraphicsProfile::summary() const
    {
        if (!detected)
            return "Graphics profile not detected";

        std::ostringstream out;
        out << adapters.size() << " graphics adapter(s)";
        if (monitorCount > 0)
            out << ", " << monitorCount << " monitor(s)";
        if (mixedDpi)
            out << ", mixed DPI";
        return out.str();
    }

    std::string makeCompatibilityWarning(const VstPluginDescriptor& plugin, const GraphicsProfile& graphicsProfile, bool conservativeMode)
    {
        if (!plugin.compatibility.hasAnyGpuOrUiRisk() && !conservativeMode)
            return {};

        std::ostringstream out;
        out << "This VST3 plugin appears to use: " << plugin.compatibility.summary() << ".\n\n";
        out << "This is a warning only; Poor Man's Studio will not block the plugin. ";
        out << "If the editor opens blank, freezes, or crashes, try Safe Plugin UI Mode, update graphics drivers, or open the editor on the primary monitor.";

        if (graphicsProfile.detected)
        {
            out << "\n\nSaved graphics adapter list: " << graphicsProfile.summary() << ".";

            if (graphicsProfile.mixedDpi || graphicsProfile.monitorCount > 1)
                out << "\nMultiple monitors or mixed DPI can affect some OpenGL/Vulkan plugin editors. Safe Plugin UI Mode opens plugin editors more conservatively.";
        }
        else
        {
            out << "\n\nA graphics adapter list has not been detected yet. VST3 Settings can refresh it and save it for more specific warnings.";
        }

        return out.str();
    }
}
