#include "vst/VstPluginTypes.h"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace
{
    std::string lowerCopy(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return value;
    }

    bool containsAscii(const std::string& text, const std::string& needle)
    {
        return lowerCopy(text).find(lowerCopy(needle)) != std::string::npos;
    }
}

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

    bool GraphicsProfile::hasIntel() const
    {
        return std::any_of(adapters.begin(), adapters.end(), [](const auto& a) { return containsAscii(a.vendor + " " + a.name, "intel"); });
    }

    bool GraphicsProfile::hasNvidia() const
    {
        return std::any_of(adapters.begin(), adapters.end(), [](const auto& a) { return containsAscii(a.vendor + " " + a.name, "nvidia"); });
    }

    bool GraphicsProfile::hasAmd() const
    {
        return std::any_of(adapters.begin(), adapters.end(), [](const auto& a) { return containsAscii(a.vendor + " " + a.name, "amd") || containsAscii(a.vendor + " " + a.name, "radeon"); });
    }

    bool GraphicsProfile::hasHybridGpu() const
    {
        return adapters.size() > 1 && hasIntel() && (hasNvidia() || hasAmd());
    }

    std::string GraphicsProfile::summary() const
    {
        if (!detected)
            return "Graphics profile not detected";

        std::ostringstream out;
        out << adapters.size() << " GPU adapter(s)";
        if (monitorCount > 0)
            out << ", " << monitorCount << " monitor(s)";
        if (mixedDpi)
            out << ", mixed DPI";
        if (hasHybridGpu())
            out << ", hybrid graphics";
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
            out << "\n\nSaved graphics profile: " << graphicsProfile.summary() << ".";

            if (graphicsProfile.hasHybridGpu())
                out << "\nYour system appears to have integrated plus dedicated graphics. If this plugin has UI issues, try the High Performance GPU preference in Windows Graphics Settings and in Poor Man's Studio's VST compatibility settings.";

            if (graphicsProfile.mixedDpi || graphicsProfile.monitorCount > 1)
                out << "\nMultiple monitors or mixed DPI can affect some OpenGL/Vulkan plugin editors. Safe Plugin UI Mode opens plugin editors more conservatively.";
        }
        else
        {
            out << "\n\nA graphics profile has not been detected yet. VST3 Settings can detect it once and save it for more specific warnings.";
        }

        return out.str();
    }
}
