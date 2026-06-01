#pragma once
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace mw::core
{
    enum class SampleBackendType
    {
        None,
        SF2,
        SFZ,
        WAV,
        VST3
    };

    inline std::string sampleBackendTypeToString(SampleBackendType type)
    {
        switch (type)
        {
            case SampleBackendType::SF2: return "SF2";
            case SampleBackendType::SFZ: return "SFZ";
            case SampleBackendType::WAV: return "WAV";
            case SampleBackendType::VST3: return "VST3";
            default: return "None";
        }
    }

    struct VstPluginAssignment
    {
        std::filesystem::path bundlePath;
        std::string name;
        std::string vendor;
        std::string version;
        std::string category;
        std::string uid;
        std::string stateBase64;
        bool bypassed = false;
        bool compatibilityWarningSeen = false;
        std::string compatibilitySummary;

        bool hasPluginIdentity() const
        {
            return !bundlePath.empty() || !uid.empty() || !name.empty();
        }
    };

    inline constexpr std::size_t maxVstEffectSlots = 2;

    struct VstEffectSlotAssignment
    {
        bool enabled = false;
        VstPluginAssignment plugin;
    };

    struct VstEffectsAssignment
    {
        // Legacy compatibility: older projects saved one track-level enabled
        // flag. New projects use per-slot enabled flags; keep this mirror true
        // when any slot is enabled so older code/projects remain readable.
        bool enabled = false;
        std::vector<VstEffectSlotAssignment> slots;

        VstEffectSlotAssignment& ensureSlot(std::size_t slotIndex)
        {
            if (slotIndex >= maxVstEffectSlots)
                slotIndex = maxVstEffectSlots - 1;

            while (slots.size() <= slotIndex)
                slots.emplace_back();

            return slots[slotIndex];
        }

        VstEffectSlotAssignment& ensureFirstSlot()
        {
            return ensureSlot(0);
        }

        VstEffectSlotAssignment* slot(std::size_t slotIndex)
        {
            return slotIndex < slots.size() ? &slots[slotIndex] : nullptr;
        }

        const VstEffectSlotAssignment* slot(std::size_t slotIndex) const
        {
            return slotIndex < slots.size() ? &slots[slotIndex] : nullptr;
        }

        VstEffectSlotAssignment* firstSlot()
        {
            return slot(0);
        }

        const VstEffectSlotAssignment* firstSlot() const
        {
            return slot(0);
        }

        bool slotEnabled(std::size_t slotIndex) const
        {
            const auto* s = slot(slotIndex);
            if (s == nullptr)
                return false;

            // Old project files did not have per-slot enabled flags. Treat the
            // legacy top-level flag as slot 1 enabled when no explicit slot
            // state has been saved yet.
            return s->enabled || (slotIndex == 0 && enabled && s->plugin.hasPluginIdentity());
        }

        void updateLegacyEnabledMirror()
        {
            enabled = false;
            for (const auto& s : slots)
            {
                if (s.enabled && s.plugin.hasPluginIdentity())
                {
                    enabled = true;
                    break;
                }
            }
        }
    };

    struct InstrumentAssignment
    {
        std::string originalImportedName;
        std::string normalizedName;
        std::string displayName = "Default Instrument";

        SampleBackendType backendType = SampleBackendType::None;
        std::filesystem::path sampleLibraryPath;
        std::string sampleLibraryDisplayName;

        int midiChannel = 1;
        int midiBank = 0;
        int midiProgram = 0;

        std::string presetName = "Default";
        std::string articulationMap = "Default";

        bool wasAutoMatched = false;
        float matchConfidence = 0.0f;

        // VST3 is track-instance state, not a global plugin selection. Multiple
        // tracks may point at the same bundle path but keep independent stateBase64.
        VstPluginAssignment vst3;
    };

    inline InstrumentAssignment makeCustomAudioInstrumentAssignment()
    {
        InstrumentAssignment assignment;
        assignment.originalImportedName = "Custom Audio";
        assignment.normalizedName = "custom audio";
        assignment.displayName = "Custom Audio";
        assignment.backendType = SampleBackendType::WAV;
        assignment.sampleLibraryDisplayName = "AudioClip media";
        assignment.presetName = "Custom Audio";
        assignment.articulationMap = "AudioClip";
        assignment.wasAutoMatched = false;
        assignment.matchConfidence = 1.0f;
        return assignment;
    }
}
