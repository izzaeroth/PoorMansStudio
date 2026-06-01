#pragma once

#include "core/InstrumentAssignment.h"

#include <optional>
#include <string>

namespace mw::vst
{
    enum class VstSnapshotRole
    {
        Instrument,
        Effect
    };

    struct VstSnapshotRecord
    {
        VstSnapshotRole role = VstSnapshotRole::Instrument;
        int slot = 1;
        std::string identityKey;
        std::string pluginName;
        std::string pluginVendor;
        std::string pluginUid;
        std::string pluginBundlePath;
        std::string savedAtLocal;
        std::string stateBase64;
    };

    class VstSnapshotStore
    {
    public:
        static constexpr int kFirstSlot = 1;
        static constexpr int kLastSlot = 5;

        static std::string roleToString(VstSnapshotRole role);
        static std::string makeIdentityKey(const mw::core::VstPluginAssignment& plugin, VstSnapshotRole role);

        static bool saveSnapshot(const mw::core::VstPluginAssignment& plugin,
                                 VstSnapshotRole role,
                                 int slot,
                                 const std::string& stateBase64,
                                 std::string* errorMessage = nullptr);

        static std::optional<VstSnapshotRecord> loadSnapshot(const mw::core::VstPluginAssignment& plugin,
                                                             VstSnapshotRole role,
                                                             int slot,
                                                             std::string* errorMessage = nullptr);

        static bool snapshotExists(const mw::core::VstPluginAssignment& plugin,
                                   VstSnapshotRole role,
                                   int slot);

        static bool clearSnapshot(const mw::core::VstPluginAssignment& plugin,
                                  VstSnapshotRole role,
                                  int slot,
                                  std::string* errorMessage = nullptr);

        static bool clearAllSnapshots(const mw::core::VstPluginAssignment& plugin,
                                      VstSnapshotRole role,
                                      std::string* errorMessage = nullptr);
    };
}
