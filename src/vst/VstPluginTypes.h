#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace mw::vst
{
    enum class VstPluginKind
    {
        Unknown,
        Instrument,
        Effect,
        MidiTool
    };

    inline std::string vstPluginKindToString(VstPluginKind kind)
    {
        switch (kind)
        {
            case VstPluginKind::Instrument: return "Instrument";
            case VstPluginKind::Effect: return "Effect";
            case VstPluginKind::MidiTool: return "MIDI Tool";
            default: return "Unknown";
        }
    }

    enum class VstPluginScanStatus
    {
        Unknown,
        Ok,
        Warning,
        Failed,
        Missing,
        Blacklisted
    };

    enum class VstPluginUserOverride
    {
        None,
        TreatAsInstrument,
        TreatAsUnsupported
    };

    inline std::string vstPluginUserOverrideToString(VstPluginUserOverride overrideValue)
    {
        switch (overrideValue)
        {
            case VstPluginUserOverride::TreatAsInstrument: return "Treat as Instrument";
            case VstPluginUserOverride::TreatAsUnsupported: return "Treat as Unsupported";
            default: return "None";
        }
    }

    inline std::string vstPluginScanStatusToString(VstPluginScanStatus status)
    {
        switch (status)
        {
            case VstPluginScanStatus::Ok: return "OK";
            case VstPluginScanStatus::Warning: return "Warning";
            case VstPluginScanStatus::Failed: return "Failed";
            case VstPluginScanStatus::Missing: return "Missing";
            case VstPluginScanStatus::Blacklisted: return "Blacklisted";
            default: return "Unknown";
        }
    }

    struct VstCompatibilityFlags
    {
        bool usesOpenGL = false;
        bool usesDirect3D = false;
        bool usesDirect2D = false;
        bool usesVulkan = false;
        bool usesOpenCL = false;
        bool usesWebView = false;
        bool usesCef = false;
        bool usesNvidiaSpecific = false;
        bool usesAmdSpecific = false;
        bool hasNativeWindowsUi = false;

        bool hasAnyGpuOrUiRisk() const
        {
            return usesOpenGL || usesDirect3D || usesDirect2D || usesVulkan || usesOpenCL
                || usesWebView || usesCef || usesNvidiaSpecific || usesAmdSpecific;
        }

        std::vector<std::string> labels() const;
        std::string summary() const;
    };

    struct VstPluginDescriptor
    {
        std::string name;
        std::string vendor;
        std::string version;
        std::string category;
        std::string uid;
        VstPluginKind detectedKind = VstPluginKind::Unknown;
        VstPluginKind kind = VstPluginKind::Unknown;
        VstPluginUserOverride userOverride = VstPluginUserOverride::None;
        VstPluginScanStatus status = VstPluginScanStatus::Unknown;
        std::filesystem::path bundlePath;
        std::filesystem::path binaryPath;
        VstCompatibilityFlags compatibility;
        std::string statusMessage;
        std::string classificationReason;
        std::string reportedCategory;
        std::string reportedClassInfo;
        std::string reportedIdentifier;
        std::string reportedFormat;
        std::string reportedDescriptiveName;
        bool juceDescriptionAvailable = false;
        bool juceReportedInstrument = false;
        int reportedAudioInputs = -1;
        int reportedAudioOutputs = -1;
        bool failedByHost = false;
        std::string failureMessage;

        bool isUsableInstrument() const
        {
            return kind == VstPluginKind::Instrument
                && !failedByHost
                && status != VstPluginScanStatus::Failed
                && status != VstPluginScanStatus::Missing
                && status != VstPluginScanStatus::Blacklisted;
        }

        std::string displayName() const
        {
            return name.empty() ? bundlePath.stem().string() : name;
        }
    };

    struct GraphicsAdapterInfo
    {
        std::string name;
        std::string vendor;
        std::string id;
        std::string type;
        unsigned long long videoMemoryMb = 0;
    };

    struct GraphicsProfile
    {
        bool detected = false;
        std::string source;
        std::string lastDetectedLocal;
        std::vector<GraphicsAdapterInfo> adapters;
        int monitorCount = 0;
        bool mixedDpi = false;
        std::string preferredPluginGpuId = "auto";

        bool hasMultipleGpus() const { return adapters.size() > 1; }
        std::string summary() const;
    };

    std::string makeCompatibilityWarning(
        const VstPluginDescriptor& plugin,
        const GraphicsProfile& graphicsProfile,
        bool conservativeMode
    );
}
