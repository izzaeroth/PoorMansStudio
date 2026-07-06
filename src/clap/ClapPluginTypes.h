#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace mw::clap
{
    enum class ClapPluginKind
    {
        Unknown,
        Instrument,
        Effect,
        MidiTool
    };

    inline std::string clapPluginKindToString(ClapPluginKind kind)
    {
        switch (kind)
        {
            case ClapPluginKind::Instrument: return "Instrument";
            case ClapPluginKind::Effect: return "Effect";
            case ClapPluginKind::MidiTool: return "MIDI Tool";
            default: return "Unknown";
        }
    }

    enum class ClapPluginScanStatus
    {
        Unknown,
        Candidate,
        Missing,
        Unsupported
    };

    inline std::string clapPluginScanStatusToString(ClapPluginScanStatus status)
    {
        switch (status)
        {
            case ClapPluginScanStatus::Candidate: return "Candidate";
            case ClapPluginScanStatus::Missing: return "Missing";
            case ClapPluginScanStatus::Unsupported: return "Unsupported";
            default: return "Unknown";
        }
    }

    enum class ClapPluginUserOverride
    {
        None,
        TreatAsInstrument,
        TreatAsEffect,
        TreatAsUnsupported
    };

    inline std::string clapPluginUserOverrideToString(ClapPluginUserOverride overrideValue)
    {
        switch (overrideValue)
        {
            case ClapPluginUserOverride::TreatAsInstrument: return "Treat as Instrument";
            case ClapPluginUserOverride::TreatAsEffect: return "Treat as Effect";
            case ClapPluginUserOverride::TreatAsUnsupported: return "Treat as Unsupported";
            default: return "None";
        }
    }

    struct ClapPluginDescriptor
    {
        std::string name;
        std::string vendor;
        std::string version;
        std::string category;
        std::string uid;
        std::string description;
        std::vector<std::string> features;
        int clapPluginCount = 0;
        unsigned int clapVersionMajor = 0;
        unsigned int clapVersionMinor = 0;
        unsigned int clapVersionRevision = 0;
        ClapPluginKind detectedKind = ClapPluginKind::Unknown;
        ClapPluginKind kind = ClapPluginKind::Unknown;
        ClapPluginUserOverride userOverride = ClapPluginUserOverride::None;
        ClapPluginScanStatus status = ClapPluginScanStatus::Unknown;
        std::filesystem::path pluginPath;
        std::filesystem::path binaryPath;
        std::string statusMessage;
        std::string classificationReason;
        bool metadataOnly = true;
        bool abiProbed = false;
        bool helperChecked = false;
        int helperExitCode = -1;
        std::string helperOutput;

        bool isUsableCandidate() const
        {
            return status != ClapPluginScanStatus::Missing
                && status != ClapPluginScanStatus::Unsupported;
        }

        std::string displayName() const
        {
            return name.empty() ? pluginPath.stem().string() : name;
        }
    };
}
