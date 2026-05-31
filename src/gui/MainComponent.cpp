#include "gui/MainComponent.h"

#include "app/AppPaths.h"
#include "app/UserPreferences.h"
#include "app/TempCleaner.h"
#include "app/AppVersion.h"
#include "core/InstrumentNameMatcher.h"
#include "audio/ExternalFluidSynthRenderer.h"
#include "audio/ExternalSfizzRenderer.h"
#include "audio/ExternalFfmpegEncoder.h"
#include "audio/AudioClipImporter.h"
#include "audio/RenderPlan.h"
#include "audio/RenderJob.h"
#include "audio/RenderSettings.h"
#include "audio/SoundFontPresetReader.h"
#include "audio/SfzValidator.h"
#include "exporting/ExportSettings.h"
#include "import_export/MusicXmlImporter.h"
#include "import_export/MidiImporter.h"
#include "midi/MidiExporter.h"
#include "serialization/ProjectSerializer.h"
#include "vst/VstPluginScanner.h"
#include "vst/VstGraphicsProfile.h"
#include "vst/VstInstrumentHost.h"
#include "BinaryData.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <iomanip>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

#if JUCE_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>
#endif
#include <system_error>

namespace
{
    constexpr int kDefaultMaxOpenVstPluginWindows = 4;
    constexpr int kHardMaxOpenVstPluginWindows = 12;

    class VstEditorStateProvider
    {
    public:
        virtual ~VstEditorStateProvider() = default;
        virtual juce::String captureCurrentVstStateBase64() = 0;
        virtual bool restoreVstStateBase64(const juce::String& stateBase64) = 0;
        virtual void stopLiveAuditionForPreview() = 0;
    };

    int sanitizeMaxOpenVstPluginWindows(int value)
    {
        if (value <= 0)
            return kDefaultMaxOpenVstPluginWindows;

        return std::clamp(value, 1, kHardMaxOpenVstPluginWindows);
    }

    void maximiseDocumentWindowToWorkArea(juce::DocumentWindow& window)
    {
#if JUCE_WINDOWS
        if (auto* peer = window.getPeer())
        {
            if (auto nativeHandle = peer->getNativeHandle())
            {
                ShowWindow(static_cast<HWND>(nativeHandle), SW_MAXIMIZE);
                return;
            }
        }
#endif

        window.setBounds(juce::Desktop::getInstance().getDisplays().getMainDisplay().userArea);
    }


    juce::String formatBytes(std::uintmax_t bytes)
    {
        const double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
        if (mb < 1024.0)
            return juce::String(mb, 1) + " MB";
        return juce::String(mb / 1024.0, 2) + " GB";
    }

    juce::String formatSecondsFromSamples(long long samples, double sampleRate)
    {
        if (sampleRate <= 0.0 || samples <= 0)
            return "0.00s";
        return juce::String(static_cast<double>(samples) / sampleRate, 2) + "s";
    }

    juce::String safeClipDisplayName(const std::filesystem::path& path)
    {
        auto name = juce::String(path.stem().string()).trim();
        return name.isEmpty() ? juce::String("AudioClip") : name;
    }

    std::filesystem::path normalizedPathForCompare(std::filesystem::path path)
    {
        if (path.empty())
            return {};

        std::error_code ec;
        if (!path.is_absolute())
        {
            auto absolutePath = std::filesystem::absolute(path, ec);
            if (!ec)
                path = std::move(absolutePath);
        }

        auto weakPath = std::filesystem::weakly_canonical(path, ec);
        if (!ec)
            return weakPath.lexically_normal();

        return path.lexically_normal();
    }

    bool pathsReferToSameLocation(const std::filesystem::path& a, const std::filesystem::path& b)
    {
        if (a.empty() || b.empty())
            return false;

        return normalizedPathForCompare(a) == normalizedPathForCompare(b);
    }


    bool isLikelyVst3BundlePath(const std::filesystem::path& path)
    {
        if (path.empty())
            return false;

        return juce::String(path.extension().string()).equalsIgnoreCase(".vst3");
    }

    std::filesystem::path resolveVst3BundlePath(const mw::core::InstrumentAssignment& assignment)
    {
        if (!assignment.vst3.bundlePath.empty())
            return assignment.vst3.bundlePath;

        // Some older or pending assignments may still carry the bundle in
        // sampleLibraryPath, so resolve that path too.
        if (isLikelyVst3BundlePath(assignment.sampleLibraryPath))
            return assignment.sampleLibraryPath;

        return {};
    }

    bool hasResolvableVst3BundlePath(const mw::core::InstrumentAssignment& assignment)
    {
        return assignment.backendType == mw::core::SampleBackendType::VST3
            && !resolveVst3BundlePath(assignment).empty();
    }

    bool repairVst3BundlePathIfPossible(mw::core::InstrumentAssignment& assignment)
    {
        if (assignment.backendType != mw::core::SampleBackendType::VST3
            || !assignment.vst3.bundlePath.empty())
            return false;

        const auto resolved = resolveVst3BundlePath(assignment);
        if (resolved.empty())
            return false;

        assignment.vst3.bundlePath = resolved;
        if (assignment.sampleLibraryPath.empty())
            assignment.sampleLibraryPath = resolved;
        if (assignment.sampleLibraryDisplayName.empty())
            assignment.sampleLibraryDisplayName = resolved.filename().string();

        return true;
    }

    juce::String getVst3TrackLibraryLabel(const mw::core::InstrumentAssignment& assignment)
    {
        juce::String label("VST3");
        const auto vendor = juce::String(assignment.vst3.vendor).trim();
        if (vendor.isNotEmpty())
            label << ": " << vendor;
        return label.isNotEmpty() ? label : juce::String("VST3");
    }

    juce::String getTrackLibrarySummaryLabel(const mw::core::InstrumentAssignment& assignment)
    {
        if (assignment.backendType == mw::core::SampleBackendType::VST3)
            return getVst3TrackLibraryLabel(assignment);

        juce::String label;
        label << mw::core::sampleBackendTypeToString(assignment.backendType) << ": ";
        label << (assignment.sampleLibraryPath.empty()
            ? juce::String("No library selected")
            : juce::String(assignment.sampleLibraryPath.filename().string()));
        return label;
    }

    struct VstCatalogRecord
    {
        mw::vst::VstPluginUserOverride userOverride = mw::vst::VstPluginUserOverride::None;
        bool failed = false;
        std::string failureMessage;
    };

    std::filesystem::path vstPluginCatalogFilePath()
    {
        return mw::app::AppPaths::settingsFolder() / "vst_plugin_catalog.txt";
    }

    std::string sanitizeCatalogField(std::string value)
    {
        for (auto& c : value)
            if (c == '\t' || c == '\r' || c == '\n')
                c = ' ';
        return value;
    }

    mw::vst::VstPluginUserOverride parseVstUserOverride(const std::string& value)
    {
        if (value == "instrument")
            return mw::vst::VstPluginUserOverride::TreatAsInstrument;
        if (value == "unsupported")
            return mw::vst::VstPluginUserOverride::TreatAsUnsupported;
        return mw::vst::VstPluginUserOverride::None;
    }

    std::string formatVstUserOverride(mw::vst::VstPluginUserOverride value)
    {
        switch (value)
        {
            case mw::vst::VstPluginUserOverride::TreatAsInstrument: return "instrument";
            case mw::vst::VstPluginUserOverride::TreatAsUnsupported: return "unsupported";
            default: return "none";
        }
    }

    std::string vstPluginCatalogKey(const mw::vst::VstPluginDescriptor& plugin)
    {
        if (!plugin.uid.empty())
            return "uid:" + sanitizeCatalogField(plugin.uid);

        const auto normalized = normalizedPathForCompare(plugin.bundlePath);
        if (!normalized.empty())
            return "path:" + sanitizeCatalogField(normalized.string());

        return "name:" + sanitizeCatalogField(plugin.vendor + ":" + plugin.displayName());
    }

    std::map<std::string, VstCatalogRecord> loadVstPluginCatalogRecords()
    {
        std::map<std::string, VstCatalogRecord> records;
        std::ifstream file(vstPluginCatalogFilePath());
        if (!file)
            return records;

        std::string line;
        while (std::getline(file, line))
        {
            if (line.empty() || line[0] == '#')
                continue;

            std::vector<std::string> fields;
            std::stringstream stream(line);
            std::string field;
            while (std::getline(stream, field, '\t'))
                fields.push_back(field);

            if (fields.empty() || fields[0].empty())
                continue;

            VstCatalogRecord record;
            if (fields.size() > 1)
                record.userOverride = parseVstUserOverride(fields[1]);
            if (fields.size() > 2)
                record.failed = fields[2] == "1" || fields[2] == "true";
            if (fields.size() > 3)
                record.failureMessage = fields[3];

            records[fields[0]] = record;
        }

        return records;
    }

    bool saveVstPluginCatalogRecords(const std::map<std::string, VstCatalogRecord>& records)
    {
        std::error_code ignored;
        std::filesystem::create_directories(mw::app::AppPaths::settingsFolder(), ignored);

        std::ofstream file(vstPluginCatalogFilePath());
        if (!file)
            return false;

        file << "# Poor Man's Studio VST3 plugin catalog overrides\n";
        file << "# key<TAB>override<TAB>failed<TAB>message\n";

        for (const auto& [key, record] : records)
        {
            if (record.userOverride == mw::vst::VstPluginUserOverride::None && !record.failed)
                continue;

            file << sanitizeCatalogField(key) << '\t'
                 << formatVstUserOverride(record.userOverride) << '\t'
                 << (record.failed ? 1 : 0) << '\t'
                 << sanitizeCatalogField(record.failureMessage) << '\n';
        }

        return file.good();
    }

    void applyVstPluginCatalogRecords(std::vector<mw::vst::VstPluginDescriptor>& plugins)
    {
        const auto records = loadVstPluginCatalogRecords();
        for (auto& plugin : plugins)
        {
            const auto key = vstPluginCatalogKey(plugin);
            const auto it = records.find(key);
            if (it == records.end())
                continue;

            plugin.userOverride = it->second.userOverride;
            if (plugin.userOverride == mw::vst::VstPluginUserOverride::TreatAsInstrument)
            {
                plugin.kind = mw::vst::VstPluginKind::Instrument;
                plugin.classificationReason += " User override: Treat as Instrument.";
            }
            else if (plugin.userOverride == mw::vst::VstPluginUserOverride::TreatAsUnsupported)
            {
                // Preserve the detected role for Plugin Manager grouping/details, but keep
                // the plugin out of the Instrument dropdown.
                plugin.kind = plugin.detectedKind;
                plugin.classificationReason += " User override: Treat as Unsupported.";
            }

            if (it->second.failed)
            {
                plugin.failedByHost = true;
                plugin.failureMessage = it->second.failureMessage;
                plugin.status = mw::vst::VstPluginScanStatus::Failed;
                plugin.statusMessage = plugin.failureMessage.empty()
                    ? "Blocked because this plugin previously failed in the host."
                    : plugin.failureMessage;
            }
        }
    }

    bool updateVstPluginCatalogRecord(const mw::vst::VstPluginDescriptor& plugin,
                                      std::optional<mw::vst::VstPluginUserOverride> overrideValue,
                                      std::optional<bool> failed,
                                      const std::string& failureMessage)
    {
        auto records = loadVstPluginCatalogRecords();
        auto& record = records[vstPluginCatalogKey(plugin)];

        if (overrideValue)
            record.userOverride = *overrideValue;
        if (failed)
            record.failed = *failed;
        if (!failureMessage.empty() || failed.value_or(false))
            record.failureMessage = failureMessage;
        if (failed && !*failed)
            record.failureMessage.clear();

        return saveVstPluginCatalogRecords(records);
    }

    bool isSupportedVstInstrumentPlugin(const mw::vst::VstPluginDescriptor& plugin)
    {
        return plugin.isUsableInstrument()
            && plugin.userOverride != mw::vst::VstPluginUserOverride::TreatAsUnsupported;
    }

    bool isInstrumentLikeVstPlugin(const mw::vst::VstPluginDescriptor& plugin)
    {
        return plugin.detectedKind == mw::vst::VstPluginKind::Instrument
            || plugin.kind == mw::vst::VstPluginKind::Instrument
            || plugin.userOverride == mw::vst::VstPluginUserOverride::TreatAsInstrument
            || plugin.juceReportedInstrument;
    }

    bool isUnsupportedNonInstrumentVstPlugin(const mw::vst::VstPluginDescriptor& plugin)
    {
        return plugin.detectedKind == mw::vst::VstPluginKind::Effect
            || plugin.detectedKind == mw::vst::VstPluginKind::MidiTool
            || plugin.kind == mw::vst::VstPluginKind::Effect
            || plugin.kind == mw::vst::VstPluginKind::MidiTool;
    }

    std::vector<mw::vst::VstPluginDescriptor> supportedVstInstrumentPlugins(const std::vector<mw::vst::VstPluginDescriptor>& plugins)
    {
        std::vector<mw::vst::VstPluginDescriptor> result;
        for (const auto& plugin : plugins)
            if (isSupportedVstInstrumentPlugin(plugin))
                result.push_back(plugin);
        return result;
    }

    std::optional<mw::vst::VstPluginDescriptor> findVstPluginDescriptorForAssignment(
        const std::vector<mw::vst::VstPluginDescriptor>& plugins,
        const mw::core::InstrumentAssignment& assignment)
    {
        const auto bundlePath = resolveVst3BundlePath(assignment);
        for (const auto& plugin : plugins)
        {
            if ((!assignment.vst3.uid.empty() && plugin.uid == assignment.vst3.uid)
                || (!bundlePath.empty() && pathsReferToSameLocation(plugin.bundlePath, bundlePath)))
                return plugin;
        }
        return std::nullopt;
    }

    juce::String vstPluginFinalStatusText(const mw::vst::VstPluginDescriptor& plugin)
    {
        if (plugin.failedByHost)
            return isInstrumentLikeVstPlugin(plugin) ? "Instrument blocked after failure" : "Blocked / Failed";
        if (isSupportedVstInstrumentPlugin(plugin))
            return plugin.userOverride == mw::vst::VstPluginUserOverride::TreatAsInstrument
                ? "Available as instrument (user enabled)"
                : "Available as instrument";
        if (plugin.userOverride == mw::vst::VstPluginUserOverride::TreatAsUnsupported && isInstrumentLikeVstPlugin(plugin))
            return "Instrument manually marked unsupported";
        if (isUnsupportedNonInstrumentVstPlugin(plugin))
            return "Unsupported plugin - effects/MIDI tools are not track instruments yet";
        if (isInstrumentLikeVstPlugin(plugin))
            return "Unsupported instrument - not currently usable";
        return "Unknown - hidden until user enables it";
    }

    juce::String vstPluginGroupName(const mw::vst::VstPluginDescriptor& plugin)
    {
        if (isSupportedVstInstrumentPlugin(plugin))
            return "Supported Instruments";

        if (plugin.failedByHost || plugin.status == mw::vst::VstPluginScanStatus::Failed)
            return isInstrumentLikeVstPlugin(plugin) ? "Unsupported Instruments" : "Failed / Blocked Plugins";

        if (plugin.userOverride == mw::vst::VstPluginUserOverride::TreatAsUnsupported && isInstrumentLikeVstPlugin(plugin))
            return "Unsupported Instruments";

        if (isUnsupportedNonInstrumentVstPlugin(plugin))
            return "Unsupported Plugins";

        if (isInstrumentLikeVstPlugin(plugin))
            return "Unsupported Instruments";

        return "Unknown Plugins";
    }

    class VstSettingsContent final : public juce::Component
    {
    public:
        VstSettingsContent(const mw::vst::GraphicsProfile& profile,
                           bool compatibilityWarningsEnabled,
                           bool safePluginUiMode,
                           int warningStyleId,
                           int maxOpenPluginWindowsIn)
            : warningsEnabled(compatibilityWarningsEnabled),
              safeMode(safePluginUiMode),
              warningStyle(warningStyleId),
              maxOpenPluginWindows(sanitizeMaxOpenVstPluginWindows(maxOpenPluginWindowsIn))
        {
            title.setText("VST3 Settings / Compatibility", juce::dontSendNotification);
            title.setFont(juce::FontOptions(18.0f, juce::Font::bold));
            title.setJustificationType(juce::Justification::centredLeft);
            addAndMakeVisible(title);

            preferredGpuLabel.setText("VST Plugin Graphics Adapter:", juce::dontSendNotification);
            preferredGpuLabel.setJustificationType(juce::Justification::centredLeft);
            addAndMakeVisible(preferredGpuLabel);

            preferredGpuCombo.onChange = [this]
            {
                const int selectedId = preferredGpuCombo.getSelectedId();
                if (selectedId <= 0 || selectedId > static_cast<int>(gpuIds.size()))
                    return;

                if (onPreferredGpuChanged)
                    onPreferredGpuChanged(gpuIds[static_cast<std::size_t>(selectedId - 1)]);
            };
            addAndMakeVisible(preferredGpuCombo);

            maxOpenPluginWindowsLabel.setText("Max Open VST Windows:", juce::dontSendNotification);
            maxOpenPluginWindowsLabel.setJustificationType(juce::Justification::centredLeft);
            addAndMakeVisible(maxOpenPluginWindowsLabel);

            maxOpenPluginWindowsCombo.addItem("1", 1);
            maxOpenPluginWindowsCombo.addItem("2", 2);
            maxOpenPluginWindowsCombo.addItem("4 - Recommended", 4);
            maxOpenPluginWindowsCombo.addItem("8", 8);
            maxOpenPluginWindowsCombo.addItem("12 - Hard Cap", 12);
            maxOpenPluginWindowsCombo.setSelectedId(maxOpenPluginWindows, juce::dontSendNotification);
            maxOpenPluginWindowsCombo.onChange = [this]
            {
                const int selectedId = maxOpenPluginWindowsCombo.getSelectedId();
                if (selectedId <= 0)
                    return;

                maxOpenPluginWindows = sanitizeMaxOpenVstPluginWindows(selectedId);
                maxOpenPluginWindowsCombo.setSelectedId(maxOpenPluginWindows, juce::dontSendNotification);
                rebuildDetailsText();

                if (onMaxOpenPluginWindowsChanged)
                    onMaxOpenPluginWindowsChanged(maxOpenPluginWindows);
            };
            addAndMakeVisible(maxOpenPluginWindowsCombo);

            safeModeToggle.setButtonText("Safe Plugin UI Mode");
            safeModeToggle.setToggleState(safeMode, juce::dontSendNotification);
            safeModeToggle.setTooltip("Safe Plugin UI Mode opens VST plugin editors in a more cautious way to reduce crashes or unstable behavior. Plugin windows may open a little slower or with reduced UI behavior. Recommended if a plugin editor is having display issues.");
            safeModeToggle.onClick = [this]
            {
                safeMode = safeModeToggle.getToggleState();
                rebuildDetailsText();
                if (onSafePluginUiModeChanged)
                    onSafePluginUiModeChanged(safeMode);
            };
            addAndMakeVisible(safeModeToggle);

            refreshButton.setButtonText("Refresh Adapter List");
            refreshButton.onClick = [this]
            {
                if (onRefreshRequested)
                    onRefreshRequested();
            };
            addAndMakeVisible(refreshButton);

            details.setMultiLine(true);
            details.setReadOnly(true);
            details.setScrollbarsShown(true);
            details.setFont(juce::FontOptions(16.0f));
            addAndMakeVisible(details);

            help.setText("System Default / Auto lets Windows and the plugin decide. Adapter choices are a preference for VST plugin editor windows, not a guarantee.", juce::dontSendNotification);
            help.setJustificationType(juce::Justification::centredLeft);
            help.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
            addAndMakeVisible(help);

            helperTooltipWindow = std::make_unique<mw::gui::FreshHoverTooltipWindow>(this, 2000);
            helperTooltipWindow->setLookAndFeel(&helperTooltipLookAndFeel);

            setGraphicsProfile(profile);
        }

        ~VstSettingsContent() override
        {
            if (helperTooltipWindow != nullptr)
                helperTooltipWindow->setLookAndFeel(nullptr);
            helperTooltipWindow.reset();
        }

        void setGraphicsProfile(const mw::vst::GraphicsProfile& newProfile)
        {
            profile = newProfile;
            rebuildGpuCombo();
            rebuildDetailsText();
        }

        void resized() override
        {
            auto area = getLocalBounds().reduced(14);
            title.setBounds(area.removeFromTop(28));
            area.removeFromTop(8);

            auto gpuRow = area.removeFromTop(32);
            preferredGpuLabel.setBounds(gpuRow.removeFromLeft(170));
            refreshButton.setBounds(gpuRow.removeFromRight(190).reduced(4, 2));
            preferredGpuCombo.setBounds(gpuRow.reduced(4, 2));
            area.removeFromTop(6);

            auto maxWindowRow = area.removeFromTop(32);
            maxOpenPluginWindowsLabel.setBounds(maxWindowRow.removeFromLeft(170));
            maxOpenPluginWindowsCombo.setBounds(maxWindowRow.removeFromLeft(210).reduced(4, 2));
            safeModeToggle.setBounds(maxWindowRow.removeFromLeft(240).reduced(4, 2));
            area.removeFromTop(8);

            help.setBounds(area.removeFromTop(44));
            area.removeFromTop(8);
            details.setBounds(area);
        }

        std::function<void(const std::string&)> onPreferredGpuChanged;
        std::function<void(int)> onMaxOpenPluginWindowsChanged;
        std::function<void(bool)> onSafePluginUiModeChanged;
        std::function<void()> onRefreshRequested;

    private:
        juce::String adapterDisplayName(const mw::vst::GraphicsAdapterInfo& adapter) const
        {
            juce::String prefix = "Hardware";
            if (adapter.type == "Software")
                prefix = "Software";
            else if (adapter.type == "Hardware")
                prefix = "Hardware";
            else if (!adapter.type.empty())
                prefix = adapter.type;

            juce::String text;
            text << prefix << ": " << juce::String(adapter.name.empty() ? "Unknown graphics adapter" : adapter.name);
            return text;
        }

        juce::String selectedAdapterDisplayName() const
        {
            if (profile.preferredPluginGpuId.empty() || profile.preferredPluginGpuId == "auto")
                return "System Default / Auto";

            for (const auto& adapter : profile.adapters)
            {
                if (!adapter.id.empty() && adapter.id == profile.preferredPluginGpuId)
                    return adapterDisplayName(adapter);
            }

            return juce::String(profile.preferredPluginGpuId);
        }

        void rebuildGpuCombo()
        {
            preferredGpuCombo.clear(juce::dontSendNotification);
            gpuIds.clear();

            gpuIds.push_back("auto");
            preferredGpuCombo.addItem("System Default / Auto", 1);

            int selectedId = 1;
            for (int i = 0; i < static_cast<int>(profile.adapters.size()); ++i)
            {
                const auto& adapter = profile.adapters[static_cast<std::size_t>(i)];
                gpuIds.push_back(adapter.id.empty() ? std::string("auto") : adapter.id);
                const int itemId = i + 2;
                preferredGpuCombo.addItem(adapterDisplayName(adapter), itemId);

                if (!profile.preferredPluginGpuId.empty()
                    && profile.preferredPluginGpuId != "auto"
                    && profile.preferredPluginGpuId == adapter.id)
                {
                    selectedId = itemId;
                }
            }

            preferredGpuCombo.setSelectedId(selectedId, juce::dontSendNotification);
            preferredGpuCombo.setEnabled(!gpuIds.empty());
        }

        void rebuildDetailsText()
        {
            juce::String text;
            text << "Compatibility warnings: " << (warningsEnabled ? "On" : "Off") << "\n";
            text << "Safe Plugin UI Mode: " << (safeMode ? "On" : "Off") << "\n";
            text << "Warning style: " << (warningStyle == 2 ? "Conservative" : (warningStyle == 3 ? "Minimal" : "Auto")) << "\n";
            text << "Maximum open VST plugin windows: " << maxOpenPluginWindows << " (hard cap " << kHardMaxOpenVstPluginWindows << ")\n\n";
            text << "Graphics adapter list: " << (profile.detected ? "Detected" : "Not detected") << "\n";
            text << "Source: " << profile.source << "\n";
            text << "Last detected: " << profile.lastDetectedLocal << "\n";
            text << "Selected VST plugin graphics adapter: " << selectedAdapterDisplayName() << "\n";
            text << "Summary: " << profile.summary() << "\n\n";
            text << "Detected adapters:\n";

            if (profile.adapters.empty())
            {
                text << "  No graphics adapter list detected yet. Use Refresh Adapter List.\n";
            }
            else
            {
                for (const auto& adapter : profile.adapters)
                {
                    text << "  - " << adapterDisplayName(adapter) << "\n";
                    if (!adapter.id.empty() && !profile.preferredPluginGpuId.empty() && profile.preferredPluginGpuId != "auto" && adapter.id == profile.preferredPluginGpuId)
                        text << "    Selected: yes\n";
                    if (!adapter.vendor.empty())
                        text << "    Vendor: " << juce::String(adapter.vendor) << "\n";
                    text << "    DXGI video memory: " << juce::String(static_cast<long long>(adapter.videoMemoryMb)) << " MB\n";
                    if (!adapter.id.empty())
                        text << "    ID: " << adapter.id << "\n";
                }
            }

            text << "\nUse Help > VST Plugin Compatibility Warnings to toggle warning popups.\n";
            text << "Compatibility warnings are non-blocking. Adapter selection is a preference for VST plugin editor windows; some plugins, drivers, or Windows settings may still choose a different rendering path.\n";
            details.setText(text, juce::dontSendNotification);
        }

        mw::vst::GraphicsProfile profile;
        bool warningsEnabled = true;
        bool safeMode = false;
        int warningStyle = 1;
        int maxOpenPluginWindows = kDefaultMaxOpenVstPluginWindows;
        std::vector<std::string> gpuIds;
        juce::Label title;
        juce::Label preferredGpuLabel;
        juce::ComboBox preferredGpuCombo;
        juce::Label maxOpenPluginWindowsLabel;
        juce::ComboBox maxOpenPluginWindowsCombo;
        juce::ToggleButton safeModeToggle;
        juce::TextButton refreshButton;
        juce::Label help;
        juce::TextEditor details;
        mw::gui::HelperTooltipLookAndFeel helperTooltipLookAndFeel;
        std::unique_ptr<juce::TooltipWindow> helperTooltipWindow;
    };

    std::int64_t audioClipEndTickForTempo(const mw::core::AudioClip& clip, double tempoBpm)
    {
        if (clip.durationSamples <= 0 || clip.sampleRate <= 0.0)
            return clip.startTick;

        const auto safeTempo = tempoBpm > 0.0 ? tempoBpm : 120.0;
        const auto durationSeconds = static_cast<double>(clip.durationSamples) / clip.sampleRate;
        const auto durationBeats = durationSeconds * safeTempo / 60.0;
        const auto durationTicks = static_cast<std::int64_t>(std::llround(durationBeats * mw::core::Project::ticksPerQuarterNote));
        return std::max<std::int64_t>(clip.startTick, clip.startTick + std::max<std::int64_t>(1, durationTicks));
    }

    bool isAllowedSnapDivision(int value)
    {
        return value == 1 || value == 2 || value == 4 || value == 8 || value == 16 || value == 32 || value == 64;
    }

    int parseSnapDivisionText(const juce::String& text)
    {
        const auto trimmed = text.trim();

        if (trimmed.isEmpty())
            return 1;

        const auto raw = trimmed.toStdString();
        const auto slash = raw.find('/');

        try
        {
            if (slash != std::string::npos)
            {
                const auto numerator = std::stod(raw.substr(0, slash));
                const auto denominator = std::stod(raw.substr(slash + 1));

                if (numerator > 0.0 && denominator > 0.0)
                {
                    const int division = static_cast<int>(std::llround(denominator / numerator));
                    return isAllowedSnapDivision(division) ? division : 1;
                }
            }

            const auto value = std::stod(raw);

            if (value > 0.0 && value < 1.0)
            {
                const int division = static_cast<int>(std::llround(1.0 / value));
                return isAllowedSnapDivision(division) ? division : 1;
            }

            const int division = static_cast<int>(std::llround(value));
            return isAllowedSnapDivision(division) ? division : 1;
        }
        catch (...)
        {
            return 1;
        }
    }

    double parseBeatLengthText(const juce::String& text, double fallback)
    {
        const auto trimmed = text.trim();

        if (trimmed.isEmpty())
            return fallback;

        const auto raw = trimmed.toStdString();
        const auto slash = raw.find('/');

        try
        {
            if (slash != std::string::npos)
            {
                const auto numerator = std::stod(raw.substr(0, slash));
                const auto denominator = std::stod(raw.substr(slash + 1));

                if (numerator > 0.0 && denominator > 0.0)
                    return numerator / denominator;
            }

            const auto value = std::stod(raw);
            return value > 0.0 ? value : fallback;
        }
        catch (...)
        {
            return fallback;
        }
    }

    juce::String beatDivisionText(int division)
    {
        return division <= 1 ? juce::String("1") : juce::String("1/") + juce::String(division);
    }

    juce::String beatLengthDisplayText(double beats)
    {
        for (const int division : { 1, 2, 4, 8, 16, 32, 64 })
        {
            const double candidate = 1.0 / static_cast<double>(division);
            if (std::abs(beats - candidate) < 0.0001)
                return beatDivisionText(division);
        }

        return juce::String(beats, 3);
    }

    juce::String timelineTimeTextForBeat(double beat, int tempoBpm, bool showTenths = false)
    {
        const auto safeTempo = std::max(1, tempoBpm);
        const auto totalSeconds = std::max(0.0, beat * 60.0 / static_cast<double>(safeTempo));
        const int minutes = static_cast<int>(std::floor(totalSeconds / 60.0));
        const int seconds = static_cast<int>(std::floor(totalSeconds)) % 60;

        if (showTenths)
        {
            const int tenths = static_cast<int>(std::floor((totalSeconds - std::floor(totalSeconds)) * 10.0));
            return juce::String::formatted("%02d:%02d.%d", minutes, seconds, tenths);
        }

        return juce::String::formatted("%02d:%02d", minutes, seconds);
    }

    struct GmInstrument
    {
        const char* name;
        const char* normalizedName;
        int program;
    };

    const std::vector<GmInstrument>& gmInstruments()
    {
        static const std::vector<GmInstrument> instruments =
        {
            {"Acoustic Grand Piano", "piano", 0},
            {"Bright Acoustic Piano", "piano", 1},
            {"Electric Piano", "electric piano", 4},
            {"Harpsichord", "harpsichord", 6},
            {"Acoustic Guitar", "guitar", 24},
            {"Electric Guitar", "electric guitar", 27},
            {"Acoustic Bass", "bass", 32},
            {"Violin", "violin", 40},
            {"Viola", "viola", 41},
            {"Cello", "cello", 42},
            {"Contrabass", "contrabass", 43},
            {"String Ensemble", "strings", 48},
            {"Choir Aahs", "choir", 52},
            {"Trumpet", "trumpet", 56},
            {"Trombone", "trombone", 57},
            {"French Horn", "french horn", 60},
            {"Brass Section", "brass", 61},
            {"Soprano Sax", "saxophone", 64},
            {"Clarinet", "clarinet", 71},
            {"Flute", "flute", 73},
            {"Oboe", "oboe", 68},
            {"Bassoon", "bassoon", 70},
            {"Pad", "pad", 88},
            {"Percussion / Drum Kit", "percussion", 0}
        };

        return instruments;
    }

    int findInstrumentIndexByProgram(int program)
    {
        const auto& instruments = gmInstruments();

        for (int i = 0; i < static_cast<int>(instruments.size()); ++i)
        {
            if (instruments[static_cast<std::size_t>(i)].program == program)
                return i;
        }

        return 0;
    }

    std::string lowerAsciiCopy(std::string value)
    {
        for (auto& c : value)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return value;
    }

    bool textContainsAny(const std::string& text, std::initializer_list<const char*> needles)
    {
        const auto lowered = lowerAsciiCopy(text);
        for (const auto* needle : needles)
        {
            if (lowered.find(needle) != std::string::npos)
                return true;
        }
        return false;
    }

    std::string assignmentSearchText(const mw::core::InstrumentAssignment& assignment)
    {
        return lowerAsciiCopy(
            assignment.displayName + " "
            + assignment.originalImportedName + " "
            + assignment.normalizedName + " "
            + assignment.presetName
        );
    }

    bool assignmentLooksLikePercussion(const mw::core::InstrumentAssignment& assignment)
    {
        return assignment.midiChannel == 10
            || assignment.midiBank == 128
            || textContainsAny(assignmentSearchText(assignment), { "drum", "percussion", "kit" });
    }

    int gmProgramFamily(int program)
    {
        if (program >= 0 && program <= 7) return 1;      // Piano
        if (program >= 8 && program <= 15) return 2;     // Chromatic percussion
        if (program >= 16 && program <= 23) return 3;    // Organ
        if (program >= 24 && program <= 31) return 4;    // Guitar
        if (program >= 32 && program <= 39) return 5;    // Bass
        if (program >= 40 && program <= 47) return 6;    // Strings
        if (program >= 48 && program <= 55) return 7;    // Ensemble
        if (program >= 56 && program <= 63) return 8;    // Brass
        if (program >= 64 && program <= 79) return 9;    // Reed/pipe
        if (program >= 80 && program <= 103) return 10;  // Synth lead/pad/effects
        if (program >= 104 && program <= 111) return 11; // Ethnic
        if (program >= 112 && program <= 119) return 12; // Percussive
        return 13;                                       // Sound effects / fallback
    }

    int findInstrumentIndexForAssignment(const mw::core::InstrumentAssignment& assignment)
    {
        const auto& instruments = gmInstruments();

        if (assignmentLooksLikePercussion(assignment))
        {
            for (int i = static_cast<int>(instruments.size()) - 1; i >= 0; --i)
            {
                const auto& instrument = instruments[static_cast<std::size_t>(i)];
                if (std::string(instrument.normalizedName) == "percussion")
                    return i;
            }
        }

        return findInstrumentIndexByProgram(assignment.midiProgram);
    }

    std::vector<std::string> splitSearchTokens(const std::string& text)
    {
        std::vector<std::string> tokens;
        std::string token;

        for (const char c : lowerAsciiCopy(text))
        {
            if (std::isalnum(static_cast<unsigned char>(c)))
            {
                token.push_back(c);
            }
            else if (token.size() >= 3)
            {
                tokens.push_back(token);
                token.clear();
            }
            else
            {
                token.clear();
            }
        }

        if (token.size() >= 3)
            tokens.push_back(token);

        tokens.erase(
            std::remove_if(tokens.begin(), tokens.end(), [](const std::string& value)
            {
                return value == "midi" || value == "track" || value == "program" || value == "bank" || value == "safe";
            }),
            tokens.end()
        );

        std::sort(tokens.begin(), tokens.end());
        tokens.erase(std::unique(tokens.begin(), tokens.end()), tokens.end());
        return tokens;
    }

    std::optional<mw::audio::SoundFontPreset> findBestSoundFontPresetForAssignment(
        const std::vector<mw::audio::SoundFontPreset>& presets,
        const mw::core::InstrumentAssignment& assignment
    )
    {
        if (presets.empty())
            return std::nullopt;

        const auto searchText = assignmentSearchText(assignment);
        const auto tokens = splitSearchTokens(searchText);
        const bool wantsPercussion = assignmentLooksLikePercussion(assignment);
        const int requestedProgram = std::clamp(assignment.midiProgram, 0, 127);
        const int requestedBank = std::max(0, assignment.midiBank);
        const int requestedFamily = gmProgramFamily(requestedProgram);

        int bestScore = std::numeric_limits<int>::min();
        const mw::audio::SoundFontPreset* bestPreset = &presets.front();

        for (const auto& preset : presets)
        {
            int score = 0;
            const auto presetName = lowerAsciiCopy(preset.name);
            const bool presetLooksPercussion = textContainsAny(presetName, { "drum", "percussion", "kit" }) || preset.bank == 128;

            if (preset.bank == requestedBank && preset.program == requestedProgram)
                score += 12000;
            else
            {
                if (preset.program == requestedProgram)
                    score += 6500;

                if (preset.bank == requestedBank)
                    score += 1800;
                else
                    score -= std::min(1200, std::abs(preset.bank - requestedBank) * 6);
            }

            const int programDistance = std::abs(preset.program - requestedProgram);
            score += std::max(0, 900 - programDistance * 28);

            if (gmProgramFamily(preset.program) == requestedFamily)
                score += 650;

            if (wantsPercussion)
            {
                if (presetLooksPercussion)
                    score += 4500;
                if (preset.bank == 128)
                    score += 3200;
                if (preset.bank == 0 && preset.program == 0 && !presetLooksPercussion)
                    score -= 5000;
            }
            else if (preset.bank == 128 || presetLooksPercussion)
            {
                score -= 3000;
            }

            for (const auto& token : tokens)
            {
                if (presetName.find(token) != std::string::npos)
                    score += 850;
            }

            if (presetName == lowerAsciiCopy(assignment.displayName)
                || presetName == lowerAsciiCopy(assignment.presetName)
                || presetName == lowerAsciiCopy(assignment.originalImportedName))
            {
                score += 2500;
            }

            if (score > bestScore)
            {
                bestScore = score;
                bestPreset = &preset;
            }
        }

        return *bestPreset;
    }

    void applySoundFontPresetToAssignment(
        mw::core::InstrumentAssignment& assignment,
        const mw::audio::SoundFontPreset& preset,
        const std::filesystem::path& libraryPath
    )
    {
        assignment.displayName = preset.name;
        assignment.normalizedName = mw::core::InstrumentNameMatcher::normalizeInstrumentName(preset.name);
        assignment.presetName = preset.name;
        assignment.midiBank = preset.bank;
        assignment.midiProgram = preset.program;
        assignment.backendType = mw::core::SampleBackendType::SF2;
        assignment.sampleLibraryPath = libraryPath;
        assignment.sampleLibraryDisplayName = libraryPath.empty() ? std::string() : libraryPath.filename().string();
        assignment.wasAutoMatched = true;
        assignment.matchConfidence = 0.90f;
    }


    void drawHeadphonesIcon(juce::Graphics& g, juce::Rectangle<float> area)
            {
                area = area.reduced(3.0f);
    
                g.setColour(juce::Colour(0xff6ec6ff));
                g.drawEllipse(area, 2.0f);
    
                auto arc = area.reduced(area.getWidth() * 0.18f, area.getHeight() * 0.22f);
                g.setColour(juce::Colours::white.withAlpha(0.92f));
                juce::Path headphoneArc;
                headphoneArc.addArc(
                    arc.getX(),
                    arc.getY(),
                    arc.getWidth(),
                    arc.getHeight(),
                    juce::MathConstants<float>::pi * 1.08f,
                    juce::MathConstants<float>::pi * 1.92f,
                    true
                );
                g.strokePath(headphoneArc, juce::PathStrokeType(3.0f));
    
                auto leftCup = juce::Rectangle<float>(area.getX() + area.getWidth() * 0.24f, area.getY() + area.getHeight() * 0.48f, area.getWidth() * 0.16f, area.getHeight() * 0.26f);
                auto rightCup = juce::Rectangle<float>(area.getRight() - area.getWidth() * 0.40f, area.getY() + area.getHeight() * 0.48f, area.getWidth() * 0.16f, area.getHeight() * 0.26f);
    
                g.setColour(juce::Colour(0xff6ec6ff));
                g.fillRoundedRectangle(leftCup, 3.0f);
                g.fillRoundedRectangle(rightCup, 3.0f);
    
                const float stemX = area.getCentreX() + area.getWidth() * 0.05f;
                const float stemTop = area.getY() + area.getHeight() * 0.36f;
                const float stemBottom = area.getY() + area.getHeight() * 0.62f;
    
                g.setColour(juce::Colour(0xffffe066));
                g.drawLine(stemX, stemTop, stemX, stemBottom, 2.0f);
                g.fillEllipse(stemX - area.getWidth() * 0.13f, stemBottom - 2.0f, area.getWidth() * 0.14f, area.getHeight() * 0.11f);
            }
    
            void drawPianoKeysIcon(juce::Graphics& g, juce::Rectangle<float> area)
        {
            area = area.reduced(4.0f);

            g.setColour(juce::Colour(0xff202124));
            g.fillRoundedRectangle(area, 8.0f);

            auto keys = area.reduced(5.0f, 10.0f);

            g.setColour(juce::Colour(0xff6ec6ff));
            g.drawRoundedRectangle(area, 8.0f, 2.5f);

            g.setColour(juce::Colour(0xfff7f7f7));
            g.fillRoundedRectangle(keys, 4.0f);

            g.setColour(juce::Colour(0xff202124));
            g.drawRoundedRectangle(keys, 4.0f, 1.4f);

            const float whiteW = keys.getWidth() / 7.0f;

            for (int i = 1; i < 7; ++i)
            {
                const float x = keys.getX() + whiteW * static_cast<float>(i);
                g.drawLine(x, keys.getY(), x, keys.getBottom(), 1.4f);
            }

            const int blackKeyPositions[] = { 0, 1, 3, 4, 5 };

            g.setColour(juce::Colour(0xff111111));

            for (int i : blackKeyPositions)
            {
                const float x = keys.getX() + whiteW * (static_cast<float>(i) + 0.66f);
                g.fillRoundedRectangle(
                    x,
                    keys.getY(),
                    whiteW * 0.58f,
                    keys.getHeight() * 0.62f,
                    2.0f
                );
            }

            // Tiny blue accent line makes this read as the Piano Roll icon
            // instead of a generic keyboard.
            g.setColour(juce::Colour(0xff6ec6ff));
            g.fillRoundedRectangle(keys.getX() + 5.0f, keys.getBottom() - 5.0f, keys.getWidth() - 10.0f, 2.0f, 1.0f);
        }
    
            void drawTrackManagerIcon(juce::Graphics& g, juce::Rectangle<float> area)
            {
                area = area.reduced(6.0f, 6.0f);

                // Keep the Track Manager window icon legible on light Windows themes:
                // dark outer/inner strokes read better than the previous pale grey lines.
                g.setColour(juce::Colour(0xff8e959f));
                g.fillRoundedRectangle(area, 5.0f);
                g.setColour(juce::Colour(0xff111111));
                g.drawRoundedRectangle(area, 5.0f, 1.7f);
    
                const float rowH = area.getHeight() / 3.0f;
    
                for (int i = 0; i < 3; ++i)
                {
                    auto row = juce::Rectangle<float>(area.getX() + 5.0f, area.getY() + rowH * static_cast<float>(i) + 4.0f, area.getWidth() - 10.0f, rowH - 7.0f);
                    g.setColour(i == 0 ? juce::Colour(0xff6ec6ff) : (i == 1 ? juce::Colour(0xff79e27d) : juce::Colour(0xffffb55a)));
                    g.fillRoundedRectangle(row.removeFromLeft(row.getHeight()), 2.0f);
                    g.setColour(juce::Colour(0xff111111));
                    g.drawLine(row.getX() + 5.0f, row.getCentreY(), row.getRight(), row.getCentreY(), 2.2f);
                }
            }
    
            void drawEditInfoIcon(juce::Graphics& g, juce::Rectangle<float> area)
            {
                area = area.reduced(5.0f);
    
                juce::Path tag;
                const float notch = area.getWidth() * 0.18f;
                tag.startNewSubPath(area.getX(), area.getY());
                tag.lineTo(area.getRight() - notch, area.getY());
                tag.lineTo(area.getRight(), area.getY() + notch);
                tag.lineTo(area.getRight(), area.getBottom());
                tag.lineTo(area.getX(), area.getBottom());
                tag.closeSubPath();
    
                g.setColour(juce::Colour(0xffffe066));
                g.fillPath(tag);
                g.setColour(juce::Colour(0xffa8871d));
                g.strokePath(tag, juce::PathStrokeType(1.5f));
    
                g.setColour(juce::Colour(0xff202124).withAlpha(0.35f));
                g.drawLine(area.getX() + 7.0f, area.getY() + area.getHeight() * 0.68f, area.getRight() - 8.0f, area.getY() + area.getHeight() * 0.68f, 1.5f);
    
                // Pencil body
                const float x1 = area.getX() + area.getWidth() * 0.28f;
                const float y1 = area.getY() + area.getHeight() * 0.70f;
                const float x2 = area.getX() + area.getWidth() * 0.72f;
                const float y2 = area.getY() + area.getHeight() * 0.30f;
    
                g.setColour(juce::Colour(0xfff4f4f4));
                g.drawLine(x1, y1, x2, y2, 5.0f);
                g.setColour(juce::Colour(0xff6ec6ff));
                g.drawLine(x1 + 2.0f, y1 - 2.0f, x2 + 2.0f, y2 - 2.0f, 2.0f);
    
                // Pencil tip
                juce::Path tip;
                tip.startNewSubPath(x2, y2);
                tip.lineTo(x2 + 7.0f, y2 - 4.0f);
                tip.lineTo(x2 + 3.0f, y2 + 7.0f);
                tip.closeSubPath();
    
                g.setColour(juce::Colour(0xff202124));
                g.fillPath(tip);
            }



            void drawVstPluginIcon(juce::Graphics& g, juce::Rectangle<float> area)
            {
                area = area.reduced(4.0f);
                g.setColour(juce::Colour(0xff101419));
                g.fillRoundedRectangle(area, 8.0f);
                g.setColour(juce::Colour(0xff35b7ff));
                g.drawRoundedRectangle(area, 8.0f, 2.4f);

                const juce::Colour colours[]
                {
                    juce::Colour(0xff35b7ff),
                    juce::Colour(0xff22c55e),
                    juce::Colour(0xffffc74d),
                    juce::Colour(0xfffb7185),
                    juce::Colour(0xff8b5cf6)
                };

                const float baseY = area.getY() + area.getHeight() * 0.38f;
                const float startX = area.getX() + 12.0f;
                const float heights[] { 8.0f, 15.0f, 11.0f, 22.0f, 14.0f, 25.0f, 16.0f, 21.0f, 10.0f };

                for (int i = 0; i < 9; ++i)
                {
                    const auto colour = colours[i % 5];
                    const float x = startX + static_cast<float>(i) * 4.7f;
                    const float h = heights[i];
                    g.setColour(colour.withAlpha(0.26f));
                    g.drawLine(x, baseY - h * 0.5f, x, baseY + h * 0.5f, 4.0f);
                    g.setColour(colour);
                    g.drawLine(x, baseY - h * 0.5f, x, baseY + h * 0.5f, 1.8f);
                }

                // Refined angled audio jack: cable, barrel, sleeve, gold tip.
                auto plug = area.reduced(9.0f);
                const auto start = juce::Point<float>(plug.getX() + 6.0f, plug.getBottom() - 5.0f);
                const auto end = juce::Point<float>(plug.getRight() - 3.0f, plug.getY() + 17.0f);
                juce::Line<float> cable(start, end);
                g.setColour(juce::Colour(0xff1f2937));
                g.drawLine(cable, 6.0f);
                g.setColour(juce::Colour(0xff9ca3af));
                g.drawLine(cable, 2.0f);

                const float angle = std::atan2(end.y - start.y, end.x - start.x);
                const float cs = std::cos(angle);
                const float sn = std::sin(angle);
                auto along = [=](float d, float off)
                {
                    return juce::Point<float>(start.x + cs * d - sn * off, start.y + sn * d + cs * off);
                };

                auto makeQuad = [&](float a, float b, float halfW)
                {
                    juce::Path path;
                    auto p1 = along(a, -halfW);
                    auto p2 = along(b, -halfW);
                    auto p3 = along(b, halfW);
                    auto p4 = along(a, halfW);
                    path.startNewSubPath(p1);
                    path.lineTo(p2);
                    path.lineTo(p3);
                    path.lineTo(p4);
                    path.closeSubPath();
                    return path;
                };

                g.setColour(juce::Colour(0xff303843));
                auto barrel = makeQuad(21.0f, 43.0f, 7.0f);
                g.fillPath(barrel);
                g.setColour(juce::Colour(0xfff8fafc));
                g.strokePath(barrel, juce::PathStrokeType(1.3f));

                g.setColour(juce::Colour(0xff0ea5e9));
                auto band = makeQuad(39.0f, 45.0f, 7.2f);
                g.fillPath(band);

                g.setColour(juce::Colour(0xffb8c1cc));
                auto sleeve = makeQuad(45.0f, 55.0f, 5.4f);
                g.fillPath(sleeve);
                g.setColour(juce::Colour(0xfff8fafc));
                g.strokePath(sleeve, juce::PathStrokeType(1.0f));

                g.setColour(juce::Colour(0xffffc74d));
                auto tip = makeQuad(55.0f, 68.0f, 4.0f);
                g.fillPath(tip);
                g.setColour(juce::Colour(0xfffff4c2));
                g.strokePath(tip, juce::PathStrokeType(1.0f));

                g.setColour(juce::Colour(0xff111827));
                for (float d : { 59.0f, 64.0f })
                    g.drawLine(juce::Line<float>(along(d, -4.0f), along(d, 4.0f)), 1.0f);
            }


        enum class PoorMansStudioWindowIcon
        {
            PianoRoll,
            TrackManager,
            EditInfo,
            PreviewPlayer,
            ColourWheel,
            VSTPlugin,
            Caution
        };

        void drawCautionIcon(juce::Graphics& g, juce::Rectangle<float> area)
        {
            auto tri = area.reduced(4.0f);
            const auto centre = tri.getCentre();

            juce::Path triangle;
            triangle.startNewSubPath(centre.x, tri.getY());
            triangle.lineTo(tri.getRight(), tri.getBottom());
            triangle.lineTo(tri.getX(), tri.getBottom());
            triangle.closeSubPath();

            g.setColour(juce::Colour(0xffffd84d));
            g.fillPath(triangle);
            g.setColour(juce::Colour(0xff1f2933));
            g.strokePath(triangle, juce::PathStrokeType(4.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

            const float stemW = area.getWidth() * 0.10f;
            const float stemH = area.getHeight() * 0.34f;
            g.setColour(juce::Colour(0xff111827));
            g.fillRoundedRectangle(centre.x - stemW * 0.5f, area.getY() + area.getHeight() * 0.34f, stemW, stemH, stemW * 0.45f);
            g.fillEllipse(centre.x - stemW * 0.75f, area.getY() + area.getHeight() * 0.74f, stemW * 1.5f, stemW * 1.5f);
        }

        juce::Image makePoorMansStudioWindowIcon(PoorMansStudioWindowIcon iconType)
        {
            juce::Image image(juce::Image::ARGB, 64, 64, true);
            juce::Graphics g(image);

            g.fillAll(juce::Colours::transparentBlack);

            const auto area = juce::Rectangle<float>(0.0f, 0.0f, 64.0f, 64.0f);

            switch (iconType)
            {
                case PoorMansStudioWindowIcon::PianoRoll:
                    drawPianoKeysIcon(g, area);
                    break;


                case PoorMansStudioWindowIcon::TrackManager:
                    drawTrackManagerIcon(g, area);
                    break;

                case PoorMansStudioWindowIcon::EditInfo:
                    drawEditInfoIcon(g, area);
                    break;


                case PoorMansStudioWindowIcon::PreviewPlayer:
                {
                    auto iconArea = area.reduced(6.0f);

                    g.setColour(juce::Colour(0xff202124));
                    g.fillRoundedRectangle(iconArea, 10.0f);

                    g.setColour(juce::Colour(0xff6ec6ff));
                    g.drawRoundedRectangle(iconArea, 10.0f, 2.5f);

                    juce::Path play;
                    play.startNewSubPath(iconArea.getX() + iconArea.getWidth() * 0.38f, iconArea.getY() + iconArea.getHeight() * 0.27f);
                    play.lineTo(iconArea.getX() + iconArea.getWidth() * 0.38f, iconArea.getY() + iconArea.getHeight() * 0.73f);
                    play.lineTo(iconArea.getX() + iconArea.getWidth() * 0.72f, iconArea.getCentreY());
                    play.closeSubPath();

                    g.setColour(juce::Colour(0xffffe066));
                    g.fillPath(play);
                    break;
                }

                case PoorMansStudioWindowIcon::VSTPlugin:
                    drawVstPluginIcon(g, area);
                    break;

                case PoorMansStudioWindowIcon::Caution:
                    drawCautionIcon(g, area);
                    break;

                case PoorMansStudioWindowIcon::ColourWheel:
                {
                    auto wheelArea = area.reduced(7.0f);
                    const auto centre = wheelArea.getCentre();
                    const float dotRadius = 6.0f;
                    const float ringRadius = wheelArea.getWidth() * 0.32f;

                    for (int i = 0; i < 12; ++i)
                    {
                        const float hue = static_cast<float>(i) / 12.0f;
                        const float angle = juce::MathConstants<float>::twoPi * hue;
                        const float x = centre.x + std::cos(angle) * ringRadius - dotRadius;
                        const float y = centre.y + std::sin(angle) * ringRadius - dotRadius;

                        g.setColour(juce::Colour::fromHSV(hue, 0.78f, 0.95f, 1.0f));
                        g.fillEllipse(x, y, dotRadius * 2.0f, dotRadius * 2.0f);
                    }

                    g.setColour(juce::Colour(0xffd1d5db));
                    g.fillEllipse(centre.x - 8.0f, centre.y - 8.0f, 16.0f, 16.0f);
                    g.setColour(juce::Colour(0xff202124));
                    g.drawEllipse(wheelArea.reduced(4.0f), 2.0f);
                    g.drawEllipse(centre.x - 8.0f, centre.y - 8.0f, 16.0f, 16.0f, 1.5f);
                    break;
                }
            }

            return image;
        }

        class PoorMansStudioTitleBarButton final : public juce::Button,
                                                  private juce::Timer
        {
        public:
            enum class Kind
            {
                Minimise,
                Maximise,
                Close
            };

            PoorMansStudioTitleBarButton(juce::String symbolIn, juce::Colour colourIn, Kind kindIn)
                : juce::Button(symbolIn), symbol(std::move(symbolIn)), colour(colourIn), kind(kindIn)
            {
                setTriggeredOnMouseDown(false);
                setWantsKeyboardFocus(false);
                setMouseCursor(juce::MouseCursor::NormalCursor);

                if (kind == Kind::Maximise)
                    startTimerHz(4);
            }

            void paintButton(juce::Graphics& g, bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override
            {
                auto area = getLocalBounds().toFloat();
                if (shouldDrawButtonAsHighlighted || shouldDrawButtonAsDown)
                {
                    g.setColour(colour.withAlpha(shouldDrawButtonAsDown ? 0.22f : 0.12f));
                    g.fillRoundedRectangle(area.reduced(4.0f, 5.0f), 5.0f);
                }

                g.setColour(colour);

                if (kind == Kind::Maximise && shouldDrawRestoreSymbol())
                {
                    const auto glyph = area.withSizeKeepingCentre(10.0f, 10.0f);
                    g.fillRoundedRectangle(glyph, 1.8f);
                    return;
                }

                const auto cx = area.getCentreX();
                const auto cy = area.getCentreY();
                const float thickness = (kind == Kind::Close ? 2.8f : 2.6f);

                if (kind == Kind::Minimise)
                {
                    g.fillRoundedRectangle(cx - 5.8f, cy + 2.6f, 11.6f, thickness, thickness * 0.5f);
                    return;
                }

                if (kind == Kind::Maximise)
                {
                    g.fillRoundedRectangle(cx - 5.6f, cy - 1.3f, 11.2f, thickness, thickness * 0.5f);
                    g.fillRoundedRectangle(cx - (thickness * 0.5f), cy - 5.6f, thickness, 11.2f, thickness * 0.5f);
                    return;
                }

                juce::Path cross;
                cross.startNewSubPath(cx - 5.2f, cy - 5.2f);
                cross.lineTo(cx + 5.2f, cy + 5.2f);
                cross.startNewSubPath(cx + 5.2f, cy - 5.2f);
                cross.lineTo(cx - 5.2f, cy + 5.2f);
                g.strokePath(cross, juce::PathStrokeType(thickness, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            }

        private:
            void timerCallback() override
            {
                repaint();
            }

            bool shouldDrawRestoreSymbol() const
            {
                if (auto* window = dynamic_cast<juce::DocumentWindow*>(getTopLevelComponent()))
                {
#if JUCE_WINDOWS
                    if (auto* peer = window->getPeer())
                    {
                        if (auto nativeHandle = peer->getNativeHandle())
                            return IsZoomed(static_cast<HWND>(nativeHandle)) != FALSE;
                    }
#endif
                    return window->isFullScreen();
                }

                return false;
            }

            juce::String symbol;
            juce::Colour colour;
            Kind kind;
        };

        class PoorMansStudioWindowLookAndFeel final : public juce::LookAndFeel_V4
        {
        public:
            juce::Button* createDocumentWindowButton(int buttonType) override
            {
                if (buttonType == juce::DocumentWindow::minimiseButton)
                    return new PoorMansStudioTitleBarButton("-", juce::Colour(0xff2d8eff), PoorMansStudioTitleBarButton::Kind::Minimise);

                if (buttonType == juce::DocumentWindow::maximiseButton)
                    return new PoorMansStudioTitleBarButton("+", juce::Colour(0xff2fbe58), PoorMansStudioTitleBarButton::Kind::Maximise);

                if (buttonType == juce::DocumentWindow::closeButton)
                    return new PoorMansStudioTitleBarButton("x", juce::Colour(0xffe13c46), PoorMansStudioTitleBarButton::Kind::Close);

                return juce::LookAndFeel_V4::createDocumentWindowButton(buttonType);
            }

            void drawDocumentWindowTitleBar(juce::DocumentWindow& window,
                                            juce::Graphics& g,
                                            int w,
                                            int h,
                                            int titleSpaceX,
                                            int titleSpaceW,
                                            const juce::Image* icon,
                                            bool /*drawTitleTextOnLeft*/) override
            {
                g.fillAll(juce::Colour(0xffd1d5db));
                g.setColour(juce::Colour(0xffaeb5bf));
                g.drawRect(0, 0, w, h, 1);
                g.setColour(juce::Colour(0xff9fa7b2));
                g.drawLine(0.0f, static_cast<float>(h - 1), static_cast<float>(w), static_cast<float>(h - 1), 1.0f);

                int titleX = 12;
                if (icon != nullptr && icon->isValid())
                {
                    // Tool-window icons now use the same larger visual footprint as the
                    // main window icon. 26 px keeps a clean 3 px top/bottom margin inside
                    // the custom 32 px title bar while letting each icon read clearly.
                    const int iconSize = juce::jmin(26, juce::jmax(20, h - 6));
                    const int iconY = (h - iconSize) / 2;
                    g.setImageResamplingQuality(juce::Graphics::highResamplingQuality);
                    g.drawImageWithin(*icon, titleX, iconY, iconSize, iconSize, juce::RectanglePlacement::centred);
                    titleX += iconSize + 12;
                }

                const int rightLimit = juce::jmax(titleX, titleSpaceX + titleSpaceW);
                auto titleBounds = juce::Rectangle<int>(titleX, 0, juce::jmax(0, rightLimit - titleX), h).reduced(0, 1);
                g.setColour(juce::Colour(0xff202124));
                g.setFont(juce::Font(13.5f, juce::Font::plain));
                g.drawText(window.getName(), titleBounds, juce::Justification::centredLeft, true);
            }
        };

        PoorMansStudioWindowLookAndFeel& getPoorMansStudioWindowLookAndFeel()
        {
            static PoorMansStudioWindowLookAndFeel lookAndFeel;
            return lookAndFeel;
        }

        void applyPoorMansStudioCustomTitleBar(juce::DocumentWindow& window)
        {
            window.setLookAndFeel(&getPoorMansStudioWindowLookAndFeel());
            window.setUsingNativeTitleBar(false);
            window.setTitleBarHeight(32);
        }

        void applyPoorMansStudioWindowIcon(juce::DocumentWindow& window, PoorMansStudioWindowIcon iconType)
        {
            auto icon = makePoorMansStudioWindowIcon(iconType);
            window.setIcon(icon);
            if (auto* peer = window.getPeer())
                peer->setIcon(icon);
        }

        void runAfterMouseButtonsReleased(std::function<void()> callback, int attempts = 0)
        {
            if (juce::ModifierKeys::getCurrentModifiersRealtime().isAnyMouseButtonDown() && attempts < 200)
            {
                juce::Timer::callAfterDelay(20, [callback = std::move(callback), attempts]() mutable
                {
                    runAfterMouseButtonsReleased(std::move(callback), attempts + 1);
                });
                return;
            }

            juce::Timer::callAfterDelay(1, [callback = std::move(callback)]() mutable
            {
                if (callback)
                    callback();
            });
        }

    
    
    class PianoRollDocumentWindow final : public juce::DocumentWindow
            {
            public:
                PianoRollDocumentWindow(const juce::String& name, std::function<void()> onCloseCallback)
                    : juce::DocumentWindow(
                        name,
                        juce::Desktop::getInstance().getDefaultLookAndFeel()
                            .findColour(juce::ResizableWindow::backgroundColourId),
                        juce::DocumentWindow::allButtons
                      ),
                      onClose(std::move(onCloseCallback))
                {
                    applyPoorMansStudioCustomTitleBar(*this);
                }

                ~PianoRollDocumentWindow() override
                {
                    setLookAndFeel(nullptr);
                }
    
                void closeButtonPressed() override
                {
                    if (closeRequestPending)
                        return;

                    closeRequestPending = true;
                    juce::Component::SafePointer<PianoRollDocumentWindow> safeThis(this);

                    // Dirty-close prompts must not be shown while the title-bar X mouse
                    // press is still down. If the alert appears under the still-held cursor,
                    // the next mouse movement can drag the alert itself. Wait for mouse-up
                    // rather than relying on a fixed delay.
                    runAfterMouseButtonsReleased([safeThis]
                    {
                        if (safeThis == nullptr)
                            return;

                        safeThis->closeRequestPending = false;

                        if (safeThis->onClose)
                            safeThis->onClose();
                    });
                }
    
            private:
                bool closeRequestPending = false;
                std::function<void()> onClose;
            };
    
            
            
            

            class ProjectInfoWindowContent final : public juce::Component
            {
            public:
                ProjectInfoWindowContent(
                    juce::TextEditor& titleEditor,
                    juce::TextEditor& artistEditor,
                    juce::TextEditor& albumEditor,
                    juce::TextEditor& trackNumberEditor,
                    juce::TextEditor& yearEditor,
                    std::function<void()> applyCallback,
                    std::function<void()> closeCallback
                )
                    : titleBox(titleEditor),
                      artistBox(artistEditor),
                      albumBox(albumEditor),
                      trackNumberBox(trackNumberEditor),
                      yearBox(yearEditor),
                      onApply(std::move(applyCallback)),
                      onClose(std::move(closeCallback))
                {
                    titleLabel.setText("Title", juce::dontSendNotification);
                    artistLabel.setText("Artist", juce::dontSendNotification);
                    albumLabel.setText("Album", juce::dontSendNotification);
                    trackNumberLabel.setText("Track #", juce::dontSendNotification);
                    yearLabel.setText("Year", juce::dontSendNotification);
    
                    helpLabel.setText(
                        "Blank fields are allowed. Non-empty tags are embedded for MP3/FLAC/OGG during render.",
                        juce::dontSendNotification
                    );
    
                    applyButton.setButtonText("Apply Info");
                    closeButton.setButtonText("Cancel");
    
                    for (auto* label : { &helpLabel, &titleLabel, &artistLabel, &albumLabel, &trackNumberLabel, &yearLabel })
                    {
                        label->setJustificationType(juce::Justification::centredLeft);
                        addAndMakeVisible(*label);
                    }
    
                    addAndMakeVisible(titleBox);
                    addAndMakeVisible(artistBox);
                    addAndMakeVisible(albumBox);
                    addAndMakeVisible(trackNumberBox);
                    addAndMakeVisible(yearBox);
                    addAndMakeVisible(applyButton);
                    addAndMakeVisible(closeButton);
            
                    applyButton.onClick = [this]
                    {
                        if (onApply)
                            onApply();
                    };
    
                    closeButton.onClick = [this]
                    {
                        if (onClose)
                            onClose();
                    };
                }
    
    void resized() override
                {
                    auto area = getLocalBounds().reduced(12);
    
                    auto top = area.removeFromTop(34);
                    closeButton.setBounds(top.removeFromRight(90).reduced(4, 2));
                    applyButton.setBounds(top.removeFromRight(120).reduced(4, 2));
                    helpLabel.setBounds(top.reduced(4, 2));
    
                    area.removeFromTop(8);
    
                    auto layoutRow = [&area](juce::Label& label, juce::TextEditor& editor)
                    {
                        auto row = area.removeFromTop(34);
                        label.setBounds(row.removeFromLeft(100).reduced(4, 2));
                        editor.setBounds(row.reduced(4, 2));
                        area.removeFromTop(4);
                    };
    
                    layoutRow(titleLabel, titleBox);
                    layoutRow(artistLabel, artistBox);
                    layoutRow(albumLabel, albumBox);
                    layoutRow(trackNumberLabel, trackNumberBox);
                    layoutRow(yearLabel, yearBox);
                }
    
            private:
                juce::TextEditor& titleBox;
                juce::TextEditor& artistBox;
                juce::TextEditor& albumBox;
                juce::TextEditor& trackNumberBox;
                juce::TextEditor& yearBox;
    
                juce::Label helpLabel;
                juce::Label titleLabel;
                juce::Label artistLabel;
                juce::Label albumLabel;
                juce::Label trackNumberLabel;
                juce::Label yearLabel;
                juce::TextButton applyButton;
                juce::TextButton closeButton;
    
                std::function<void()> onApply;
                std::function<void()> onClose;
            };
    
    class SequenceMapView final : public juce::Component
            {
            public:
                SequenceMapView(
                    std::function<void(juce::Graphics&, juce::Rectangle<int>)> paintCallback,
                    std::function<void(const juce::MouseEvent&, bool)> clickCallback,
                    std::function<void(const juce::MouseWheelDetails&)> wheelCallback
                )
                    : onPaint(std::move(paintCallback)),
                      onClick(std::move(clickCallback)),
                      onWheel(std::move(wheelCallback)) {}

                void paint(juce::Graphics& g) override
                {
                    if (onPaint)
                        onPaint(g, getLocalBounds());
                }

                void mouseDown(const juce::MouseEvent& event) override
                {
                    if (onClick)
                        onClick(event, false);
                }

                void mouseDoubleClick(const juce::MouseEvent& event) override
                {
                    if (onClick)
                        onClick(event, true);
                }

                void mouseDrag(const juce::MouseEvent& event) override
                {
                    if (onClick)
                        onClick(event, false);
                }

                void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& wheel) override
                {
                    if (onWheel)
                        onWheel(wheel);
                }

            private:
                std::function<void(juce::Graphics&, juce::Rectangle<int>)> onPaint;
                std::function<void(const juce::MouseEvent&, bool)> onClick;
                std::function<void(const juce::MouseWheelDetails&)> onWheel;
            };


    static const juce::Colour poorMansSequencePalette[32] =
    {
        juce::Colour(0xff5aa9ff), juce::Colour(0xff7dd3c7),
        juce::Colour(0xffffb86b), juce::Colour(0xffb89cff),
        juce::Colour(0xffff8fa3), juce::Colour(0xff9bd77e),
        juce::Colour(0xffe6c65c), juce::Colour(0xff72c6ef),
        juce::Colour(0xffc7a6ff), juce::Colour(0xfff28c62),
        juce::Colour(0xff8ed1a6), juce::Colour(0xfff2a7d0),
        juce::Colour(0xffa5c8ff), juce::Colour(0xffd8b06a),
        juce::Colour(0xff89e0e0), juce::Colour(0xffff9a75),
        juce::Colour(0xffb7d66d), juce::Colour(0xff8fa7ff),
        juce::Colour(0xffe3a1ff), juce::Colour(0xff70d1b8),
        juce::Colour(0xffffd166), juce::Colour(0xffff7d7d),
        juce::Colour(0xff97e6a3), juce::Colour(0xff9fd0ff),
        juce::Colour(0xffd2c07f), juce::Colour(0xffcf9dff),
        juce::Colour(0xffefaa7a), juce::Colour(0xff7fcf95),
        juce::Colour(0xfff0d0ff), juce::Colour(0xff69b7d6),
        juce::Colour(0xffc5e37a), juce::Colour(0xffffa6b5)
    };

    static juce::String sequenceColourToHex(juce::Colour colour)
    {
        return juce::String::formatted(
            "#%02X%02X%02X",
            static_cast<int>(colour.getRed()),
            static_cast<int>(colour.getGreen()),
            static_cast<int>(colour.getBlue())
        );
    }

    static juce::Colour sequenceColourFromHex(juce::String text, juce::Colour fallback)
    {
        text = text.trim().removeCharacters("# ");

        if (text.length() != 6)
            return fallback;

        const auto value = text.getHexValue32();
        return juce::Colour(
            static_cast<juce::uint8>((value >> 16) & 0xff),
            static_cast<juce::uint8>((value >> 8) & 0xff),
            static_cast<juce::uint8>(value & 0xff)
        );
    }

    static juce::Colour readableSequenceColour(juce::Colour colour)
    {
        // Sequence colours are used on a black console background, so reserve
        // black/near-black and gently lift any low-contrast custom colour.
        if (colour.getBrightness() < 0.08f)
            return poorMansSequencePalette[0];

        for (int i = 0; i < 6 && colour.getBrightness() < 0.42f; ++i)
            colour = colour.brighter(0.18f);

        return colour.getBrightness() < 0.35f ? poorMansSequencePalette[0] : colour;
    }

    static juce::Colour defaultSequenceColourFor(int index, const juce::String&)
    {
        // Use a fixed high-contrast walk through the palette instead of name-based
        // hashing.  This keeps adjacent/new sequences from landing on similar shades.
        static const int order[32] =
        {
            0, 2, 3, 5, 11, 14, 21, 17,
            6, 9, 12, 19, 24, 27, 30, 4,
            7, 10, 13, 16, 18, 20, 22, 25,
            28, 31, 1, 8, 15, 23, 26, 29
        };

        const auto paletteIndex = order[static_cast<std::size_t>(std::abs(index) % 32)];
        auto colour = poorMansSequencePalette[paletteIndex];

        if (colour.getBrightness() < 0.45f)
            colour = colour.brighter(0.25f);

        return readableSequenceColour(colour);
    }

    class ColourPreviewComponent final : public juce::Component
    {
    public:
        void setPreviewColour(juce::Colour newColour)
        {
            colour = newColour;
            repaint();
        }

        void paint(juce::Graphics& g) override
        {
            auto area = getLocalBounds().reduced(3);
            g.setColour(juce::Colour(0xff202328));
            g.fillRoundedRectangle(area.toFloat(), 5.0f);
            g.setColour(colour);
            g.fillRoundedRectangle(area.reduced(4).toFloat(), 4.0f);
            g.setColour(juce::Colours::grey);
            g.drawRoundedRectangle(area.toFloat(), 5.0f, 1.0f);
        }

    private:
        juce::Colour colour { poorMansSequencePalette[0] };
    };

    class SequenceColourEditorContent final : public juce::Component,
                                             private juce::ChangeListener
    {
    public:
        SequenceColourEditorContent(
            int sequenceNumber,
            const juce::String& sequenceName,
            juce::Colour initialColour,
            std::function<void(juce::Colour)> applyCallback,
            std::function<void()> closeCallback
        )
            : selector(juce::ColourSelector::showColourAtTop
                     | juce::ColourSelector::showSliders
                     | juce::ColourSelector::showColourspace),
              onApply(std::move(applyCallback)),
              onClose(std::move(closeCallback))
        {
            titleLabel.setText("Sequence #" + juce::String(sequenceNumber) + " Color", juce::dontSendNotification);
            titleLabel.setJustificationType(juce::Justification::centredLeft);

            nameLabel.setText(sequenceName, juce::dontSendNotification);
            nameLabel.setJustificationType(juce::Justification::centredLeft);

            paletteLabel.setText("Palette:", juce::dontSendNotification);
            preview.setPreviewColour(readableSequenceColour(initialColour));

            for (int i = 0; i < 32; ++i)
                paletteCombo.addItem("Color " + juce::String(i + 1), i + 1);

            paletteCombo.setSelectedId(1, juce::dontSendNotification);

            selector.setCurrentColour(readableSequenceColour(initialColour));
            selector.addChangeListener(this);

            applyButton.setButtonText("Apply");
            closeButton.setButtonText("Cancel");

            addAndMakeVisible(titleLabel);
            addAndMakeVisible(nameLabel);
            addAndMakeVisible(paletteLabel);
            addAndMakeVisible(paletteCombo);
            addAndMakeVisible(selector);
            addAndMakeVisible(preview);
            addAndMakeVisible(applyButton);
            addAndMakeVisible(closeButton);

            paletteCombo.onChange = [this]
            {
                const auto id = paletteCombo.getSelectedId();

                if (id >= 1 && id <= 32)
                {
                    const auto colour = readableSequenceColour(poorMansSequencePalette[id - 1]);
                    selector.setCurrentColour(colour);
                    preview.setPreviewColour(colour);
                }
            };

            applyButton.onClick = [this]
            {
                if (onApply)
                    onApply(readableSequenceColour(selector.getCurrentColour()));
            };

            closeButton.onClick = [this]
            {
                if (onClose)
                    onClose();
            };

        }

        ~SequenceColourEditorContent() override
        {
            selector.removeChangeListener(this);
        }

        void resized() override
        {
            auto area = getLocalBounds().reduced(12);
            titleLabel.setBounds(area.removeFromTop(26));
            nameLabel.setBounds(area.removeFromTop(24));
            area.removeFromTop(10);

            auto paletteRow = area.removeFromTop(32);
            paletteLabel.setBounds(paletteRow.removeFromLeft(70));
            preview.setBounds(paletteRow.removeFromRight(70).reduced(4, 2));
            paletteCombo.setBounds(paletteRow.reduced(4, 2));

            area.removeFromTop(8);
            selector.setBounds(area.removeFromTop(std::max(160, area.getHeight() - 48)).reduced(2));

            area.removeFromTop(12);
            auto buttonRow = area.removeFromTop(34);
            closeButton.setBounds(buttonRow.removeFromRight(90).reduced(4, 2));
            applyButton.setBounds(buttonRow.removeFromRight(120).reduced(4, 2));
        }

    private:
        void changeListenerCallback(juce::ChangeBroadcaster*) override
        {
            preview.setPreviewColour(readableSequenceColour(selector.getCurrentColour()));
        }

        juce::Label titleLabel;
        juce::Label nameLabel;
        juce::Label paletteLabel;
        juce::ComboBox paletteCombo;
        juce::ColourSelector selector;
        ColourPreviewComponent preview;
        juce::TextButton applyButton;
        juce::TextButton closeButton;
        std::function<void(juce::Colour)> onApply;
        std::function<void()> onClose;
    };


    struct SequencePickerChoice
    {
        int number = 0;
        juce::String name;
        juce::String notes;
        bool locked = false;
        int trackCount = 0;
        juce::String trackSummary;
    };

    class SequencePickerDocumentWindow final : public juce::DocumentWindow
    {
    public:
        SequencePickerDocumentWindow(const juce::String& windowName, std::function<void()> closeCallback)
            : juce::DocumentWindow(windowName, juce::Colour(0xff202020), 0),
              onClose(std::move(closeCallback))
        {
            // Keep this picker styled like the About dialog: no OS/native title bar
            // and no custom Poor Man's Studio title controls. The content remains
            // scrollable via the viewport inside SequencePickerContent.
            setUsingNativeTitleBar(false);
            setTitleBarHeight(0);
            setResizable(true, true);
        }

        ~SequencePickerDocumentWindow() override
        {
            setLookAndFeel(nullptr);
        }

        void closeButtonPressed() override
        {
            if (onClose)
                onClose();
        }

    private:
        std::function<void()> onClose;
    };

    class SequencePickerContent final : public juce::Component
    {
    public:
        SequencePickerContent(
            juce::String pickerTitle,
            juce::String pickerInstructions,
            std::vector<SequencePickerChoice> sequenceChoices,
            int initialSequenceNumber,
            std::function<void(int)> applyExistingCallback,
            std::function<std::optional<std::pair<int, juce::String>>()> createNewCallback,
            std::function<void()> cancelCallback
        )
            : choices(std::move(sequenceChoices)),
              rows(choices, selectedSequenceNumber),
              onApplyExisting(std::move(applyExistingCallback)),
              onCreateNew(std::move(createNewCallback)),
              onCancel(std::move(cancelCallback))
        {
            selectedSequenceNumber = initialSequenceNumber;

            if (selectedSequenceNumber <= 0 && !choices.empty())
                selectedSequenceNumber = choices.front().number;

            titleLabel.setText(std::move(pickerTitle), juce::dontSendNotification);
            titleLabel.setJustificationType(juce::Justification::centredLeft);
            titleLabel.setFont(juce::FontOptions(18.0f, juce::Font::bold));
            titleLabel.setColour(juce::Label::textColourId, juce::Colours::white);

            instructionLabel.setText(std::move(pickerInstructions), juce::dontSendNotification);
            instructionLabel.setJustificationType(juce::Justification::centredLeft);
            instructionLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);

            statusLabel.setText({}, juce::dontSendNotification);
            statusLabel.setJustificationType(juce::Justification::centredLeft);
            statusLabel.setColour(juce::Label::textColourId, juce::Colours::yellow);

            okButton.setButtonText("OK");
            createNewButton.setButtonText("Create Blank Seq");
            cancelButton.setButtonText("Cancel");

            viewport.setViewedComponent(&rows, false);
            viewport.setScrollBarsShown(true, true);
            viewport.setScrollBarThickness(16);
            rows.onSelectionChanged = [this](int sequenceNumber)
            {
                selectedSequenceNumber = sequenceNumber;
                statusLabel.setText("Selected Seq #" + juce::String(sequenceNumber) + ". Press OK to apply.", juce::dontSendNotification);
            };

            okButton.onClick = [this]
            {
                if (selectedSequenceNumber <= 0)
                {
                    statusLabel.setText("Pick a sequence from the list first.", juce::dontSendNotification);
                    return;
                }

                if (onApplyExisting)
                    onApplyExisting(selectedSequenceNumber);
            };

            createNewButton.onClick = [this]
            {
                if (!onCreateNew)
                    return;

                auto created = onCreateNew();

                if (!created.has_value())
                    return;

                SequencePickerChoice choice;
                choice.number = created->first;
                choice.name = created->second;
                choice.locked = false;
                choice.trackCount = 0;
                choice.trackSummary = "(empty)";

                upsertChoice(std::move(choice));
                selectedSequenceNumber = created->first;
                rows.setSelectedSequenceNumber(selectedSequenceNumber);
                rows.refreshContentSize();
                viewport.setViewedComponent(&rows, false);
                viewport.setViewPosition(0, std::max(0, rows.getHeight() - viewport.getViewHeight()));
                statusLabel.setText("Created blank Seq #" + juce::String(selectedSequenceNumber) + ". Press OK to apply it.", juce::dontSendNotification);
            };

            cancelButton.onClick = [this]
            {
                if (onCancel)
                    onCancel();
            };

            addAndMakeVisible(titleLabel);
            addAndMakeVisible(instructionLabel);
            addAndMakeVisible(viewport);
            addAndMakeVisible(statusLabel);
            addAndMakeVisible(okButton);
            addAndMakeVisible(createNewButton);
            addAndMakeVisible(cancelButton);

            rows.setSelectedSequenceNumber(selectedSequenceNumber);
        }

        void replaceChoices(std::vector<SequencePickerChoice> nextChoices, int preferredSelection)
        {
            choices = std::move(nextChoices);

            selectedSequenceNumber = preferredSelection;
            if (selectedSequenceNumber <= 0 && !choices.empty())
                selectedSequenceNumber = choices.front().number;

            rows.setSelectedSequenceNumber(selectedSequenceNumber);
            rows.refreshContentSize();
            viewport.setViewedComponent(&rows, false);
            repaint();
        }

        void paint(juce::Graphics& g) override
        {
            g.fillAll(juce::Colour(0xff171717));
            g.setColour(juce::Colour(0xff4a4a4a));
            g.drawRect(getLocalBounds(), 1);
        }

        void resized() override
        {
            auto area = getLocalBounds().reduced(14);
            titleLabel.setBounds(area.removeFromTop(28));
            instructionLabel.setBounds(area.removeFromTop(42));
            area.removeFromTop(8);

            viewport.setBounds(area.removeFromTop(std::max(150, area.getHeight() - 76)));
            area.removeFromTop(8);
            statusLabel.setBounds(area.removeFromTop(24));
            area.removeFromTop(6);

            auto buttons = area.removeFromTop(34);
            cancelButton.setBounds(buttons.removeFromRight(90).reduced(4, 2));
            createNewButton.setBounds(buttons.removeFromRight(150).reduced(4, 2));
            okButton.setBounds(buttons.removeFromRight(90).reduced(4, 2));
        }

    private:
        void upsertChoice(SequencePickerChoice choice)
        {
            for (auto& existing : choices)
            {
                if (existing.number == choice.number)
                {
                    existing = std::move(choice);
                    return;
                }
            }

            choices.push_back(std::move(choice));
            std::sort(choices.begin(), choices.end(), [](const auto& a, const auto& b) { return a.number < b.number; });
        }

        class Rows final : public juce::Component
        {
        public:
            Rows(const std::vector<SequencePickerChoice>& sequenceChoices, int& selectedNumberRef)
                : choicesRef(sequenceChoices), selectedNumber(selectedNumberRef)
            {
                refreshContentSize();
            }

            void refreshContentSize()
            {
                setSize(calculateContentWidth(), std::max(1, static_cast<int>(choicesRef.size())) * rowHeight + 8);
                repaint();
            }

            void setSelectedSequenceNumber(int sequenceNumber)
            {
                selectedNumber = sequenceNumber;
                repaint();
            }

            void mouseDown(const juce::MouseEvent& event) override
            {
                const int rowIndex = std::clamp(static_cast<int>(event.position.y) / rowHeight, 0, std::max(0, static_cast<int>(choicesRef.size()) - 1));

                if (rowIndex < 0 || rowIndex >= static_cast<int>(choicesRef.size()))
                    return;

                selectedNumber = choicesRef[static_cast<std::size_t>(rowIndex)].number;

                if (onSelectionChanged)
                    onSelectionChanged(selectedNumber);

                repaint();
            }

            void paint(juce::Graphics& g) override
            {
                g.fillAll(juce::Colour(0xff0f0f0f));
                g.setFont(juce::FontOptions(14.0f));

                if (choicesRef.empty())
                {
                    g.setColour(juce::Colours::lightgrey);
                    g.drawText("No sequences yet. Use Create Blank Seq to add one.", getLocalBounds().reduced(10), juce::Justification::centredLeft, false);
                    return;
                }

                for (int i = 0; i < static_cast<int>(choicesRef.size()); ++i)
                {
                    const auto& choice = choicesRef[static_cast<std::size_t>(i)];
                    auto row = juce::Rectangle<int>(0, i * rowHeight, getWidth(), rowHeight).reduced(2, 1);
                    const bool selected = choice.number == selectedNumber;

                    g.setColour(selected ? juce::Colour(0xff365a88) : (i % 2 == 0 ? juce::Colour(0xff1b1b1b) : juce::Colour(0xff242424)));
                    g.fillRect(row);

                    g.setColour(selected ? juce::Colours::white : juce::Colours::lightgrey);
                    g.drawText(formatChoice(choice), row.reduced(10, 0), juce::Justification::centredLeft, false);
                }
            }

            std::function<void(int)> onSelectionChanged;

        private:
            static juce::String formatChoice(const SequencePickerChoice& choice)
            {
                juce::String text;
                text << "Seq #" << choice.number;

                if (choice.locked)
                    text << " [LOCKED]";

                const auto tracksText = choice.trackSummary.isNotEmpty()
                    ? choice.trackSummary
                    : (choice.trackCount <= 0 ? juce::String("(empty)") : juce::String(choice.trackCount) + " track" + (choice.trackCount == 1 ? juce::String() : juce::String("s")));

                text << " - " << (choice.name.isEmpty() ? juce::String("Unnamed Sequence") : choice.name)
                     << "   | Tracks: " << tracksText;

                if (!choice.notes.trim().isEmpty())
                    text << "   | Thoughts: " << choice.notes.trim();

                return text;
            }

            int calculateContentWidth() const
            {
                int width = 940;

                for (const auto& choice : choicesRef)
                    width = std::max(width, 260 + formatChoice(choice).length() * 8);

                return std::min(1800, width);
            }

            enum { rowHeight = 30 };
            const std::vector<SequencePickerChoice>& choicesRef;
            int& selectedNumber;
        };

        std::vector<SequencePickerChoice> choices;
        int selectedSequenceNumber = 0;
        juce::Label titleLabel;
        juce::Label instructionLabel;
        juce::Viewport viewport;
        Rows rows;
        juce::Label statusLabel;
        juce::TextButton okButton;
        juce::TextButton createNewButton;
        juce::TextButton cancelButton;
        std::function<void(int)> onApplyExisting;
        std::function<std::optional<std::pair<int, juce::String>>()> onCreateNew;
        std::function<void()> onCancel;
    };


    class RenderSettingsContent final : public juce::Component
    {
    public:
        RenderSettingsContent(int initialMask, int initialWorkerCount, std::function<void(int, int)> applyCallback, std::function<void()> cancelCallback)
            : originalMask(std::clamp(initialMask, 0, 3)),
              originalWorkerId(normalizeWorkerComboId(initialWorkerCount)),
              onApply(std::move(applyCallback)),
              onCancel(std::move(cancelCallback))
        {
            titleLabel.setText("Render Settings", juce::dontSendNotification);
            titleLabel.setJustificationType(juce::Justification::centredLeft);
            titleLabel.setFont(juce::FontOptions(18.0f, juce::Font::bold));
            titleLabel.setColour(juce::Label::textColourId, juce::Colours::white);

            instructionLabel.setText(
                "Choose render-retention and parallel-stem behavior. Preview/temp renders are still cleaned up.",
                juce::dontSendNotification);
            instructionLabel.setJustificationType(juce::Justification::centredLeft);
            instructionLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);

            wavToggle.setButtonText("Keep WAV audio stems");
            midiToggle.setButtonText("Keep MIDI stem files");
            wavToggle.setToggleState((originalMask & 1) != 0, juce::dontSendNotification);
            midiToggle.setToggleState((originalMask & 2) != 0, juce::dontSendNotification);

            wavToggle.setTooltip("When enabled, successful user renders keep rendered WAV stems under the stems/audio folder.");
            midiToggle.setTooltip("When enabled, successful user renders keep exported MIDI stem files under the stems/midi folder.");

            parallelStemsLabel.setText("Parallel Stems:", juce::dontSendNotification);
            parallelStemsLabel.setJustificationType(juce::Justification::centredLeft);
            parallelStemsLabel.setColour(juce::Label::textColourId, juce::Colours::white);

            parallelStemsCombo.addItem("Auto (safe)", 100);
            parallelStemsCombo.addItem("1", 1);
            parallelStemsCombo.addItem("2", 2);
            parallelStemsCombo.addItem("4", 4);
            parallelStemsCombo.addItem("6", 6);
            parallelStemsCombo.addItem("8", 8);
            parallelStemsCombo.addItem("12", 12);
            parallelStemsCombo.addItem("16", 16);
            parallelStemsCombo.setSelectedId(originalWorkerId, juce::dontSendNotification);
            parallelStemsCombo.setTooltip("Choose how many independent MIDI stems may render at the same time. Auto (safe) uses hardware-thread tiers and remains limited by eligible stem count.");

            parallelHelpLabel.setText(
                "Parallel Stems are simultaneous independent stem jobs, not direct CPU-core assignment. Final FFmpeg mix/encode remains serial.",
                juce::dontSendNotification);
            parallelHelpLabel.setJustificationType(juce::Justification::centredLeft);
            parallelHelpLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);

            noteLabel.setText(
                "Stem retention is stored as keepStemFiles=0, 1, 2, or 1,2. Parallel Stems is stored as lastRenderWorkerCount=0 for Auto or the selected count.",
                juce::dontSendNotification);
            noteLabel.setJustificationType(juce::Justification::centredLeft);
            noteLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);

            statusLabel.setJustificationType(juce::Justification::centredLeft);
            statusLabel.setColour(juce::Label::textColourId, juce::Colours::yellow);
            updateStatusLabel();

            wavToggle.onClick = [this] { updateStatusLabel(); };
            midiToggle.onClick = [this] { updateStatusLabel(); };
            parallelStemsCombo.onChange = [this] { updateStatusLabel(); };

            okButton.setButtonText("OK");
            cancelButton.setButtonText("Cancel");

            okButton.onClick = [this]
            {
                if (onApply)
                    onApply(getMask(), getWorkerPreferenceValue());
            };

            cancelButton.onClick = [this]
            {
                if (onCancel)
                    onCancel();
            };

            addAndMakeVisible(titleLabel);
            addAndMakeVisible(instructionLabel);
            addAndMakeVisible(wavToggle);
            addAndMakeVisible(midiToggle);
            addAndMakeVisible(parallelStemsLabel);
            addAndMakeVisible(parallelStemsCombo);
            addAndMakeVisible(parallelHelpLabel);
            addAndMakeVisible(noteLabel);
            addAndMakeVisible(statusLabel);
            addAndMakeVisible(okButton);
            addAndMakeVisible(cancelButton);
        }

        void paint(juce::Graphics& g) override
        {
            g.fillAll(juce::Colour(0xff171717));
            g.setColour(juce::Colour(0xff4a4a4a));
            g.drawRect(getLocalBounds(), 1);
        }

        void resized() override
        {
            auto area = getLocalBounds().reduced(14);
            titleLabel.setBounds(area.removeFromTop(28));
            instructionLabel.setBounds(area.removeFromTop(38));
            area.removeFromTop(8);

            wavToggle.setBounds(area.removeFromTop(30));
            midiToggle.setBounds(area.removeFromTop(30));
            area.removeFromTop(8);

            auto parallelRow = area.removeFromTop(32);
            parallelStemsLabel.setBounds(parallelRow.removeFromLeft(130));
            parallelStemsCombo.setBounds(parallelRow.removeFromLeft(175).reduced(4, 2));
            area.removeFromTop(6);
            parallelHelpLabel.setBounds(area.removeFromTop(36));
            area.removeFromTop(8);

            noteLabel.setBounds(area.removeFromTop(46));
            statusLabel.setBounds(area.removeFromTop(32));
            area.removeFromTop(6);

            auto buttons = area.removeFromTop(34);
            cancelButton.setBounds(buttons.removeFromRight(90).reduced(4, 2));
            okButton.setBounds(buttons.removeFromRight(90).reduced(4, 2));
        }

    private:
        int getMask() const
        {
            int mask = 0;
            if (wavToggle.getToggleState())
                mask |= 1;
            if (midiToggle.getToggleState())
                mask |= 2;
            return mask;
        }

        int getWorkerPreferenceValue() const
        {
            const int selectedId = normalizeWorkerComboId(parallelStemsCombo.getSelectedId() == 100 ? 0 : parallelStemsCombo.getSelectedId());
            return selectedId == 100 ? 0 : selectedId;
        }

        static int normalizeWorkerComboId(int workerCount)
        {
            switch (workerCount)
            {
                case 1:
                case 2:
                case 4:
                case 6:
                case 8:
                case 12:
                case 16:
                    return workerCount;
                case 0:
                default:
                    return 100;
            }
        }

        static juce::String workerToText(int workerCount)
        {
            const int comboId = normalizeWorkerComboId(workerCount);
            if (comboId == 100)
                return "Auto (safe)";

            return juce::String(comboId) + " parallel stem" + (comboId == 1 ? juce::String() : juce::String("s"));
        }

        static juce::String maskShortText(int mask)
        {
            switch (std::clamp(mask, 0, 3))
            {
                case 0: return "None";
                case 1: return "WAV only";
                case 2: return "MIDI only";
                case 3:
                default: return "WAV + MIDI";
            }
        }

        static juce::String maskToText(int mask)
        {
            switch (std::clamp(mask, 0, 3))
            {
                case 0: return "None - remove MIDI and WAV stems after successful user renders.";
                case 1: return "WAV only - keep audio stems, remove MIDI stems.";
                case 2: return "MIDI only - keep MIDI stems, remove WAV stems.";
                case 3:
                default: return "WAV + MIDI - keep both stem folders.";
            }
        }

        void updateStatusLabel()
        {
            statusLabel.setText(
                juce::String("Current choice: Stems ") + maskShortText(getMask()) + " | Parallel " + workerToText(getWorkerPreferenceValue()),
                juce::dontSendNotification);
        }

        int originalMask = 3;
        int originalWorkerId = 100;
        juce::Label titleLabel;
        juce::Label instructionLabel;
        juce::ToggleButton wavToggle;
        juce::ToggleButton midiToggle;
        juce::Label parallelStemsLabel;
        juce::ComboBox parallelStemsCombo;
        juce::Label parallelHelpLabel;
        juce::Label noteLabel;
        juce::Label statusLabel;
        juce::TextButton okButton;
        juce::TextButton cancelButton;
        std::function<void(int, int)> onApply;
        std::function<void()> onCancel;
    };

    class SequenceThoughtsEditorContent final : public juce::Component
    {
    public:
        SequenceThoughtsEditorContent(
            juce::String editorTitle,
            juce::String editorInstructions,
            juce::String initialThoughtsText,
            std::function<void(juce::String)> applyCallback,
            std::function<void()> closeCallback
        )
            : initialThoughts(std::move(initialThoughtsText)),
              onApply(std::move(applyCallback)),
              onClose(std::move(closeCallback))
        {
            titleLabel.setText(std::move(editorTitle), juce::dontSendNotification);
            titleLabel.setJustificationType(juce::Justification::centredLeft);
            titleLabel.setFont(juce::FontOptions(18.0f, juce::Font::bold));
            titleLabel.setColour(juce::Label::textColourId, juce::Colours::white);

            instructionLabel.setText(std::move(editorInstructions), juce::dontSendNotification);
            instructionLabel.setJustificationType(juce::Justification::centredLeft);
            instructionLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);

            dirtyLabel.setText("No unsaved thoughts changes.", juce::dontSendNotification);
            dirtyLabel.setJustificationType(juce::Justification::centredLeft);
            dirtyLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);

            thoughtsEditor.setMultiLine(true);
            thoughtsEditor.setReturnKeyStartsNewLine(true);
            thoughtsEditor.setScrollbarsShown(true);
            thoughtsEditor.setTextToShowWhenEmpty("Add thoughts about what this sequence is doing...", juce::Colours::grey);
            thoughtsEditor.setText(initialThoughts, juce::dontSendNotification);
            thoughtsEditor.onTextChange = [this] { updateDirtyLabel(); };

            okButton.setButtonText("OK");
            cancelButton.setButtonText("Cancel");

            okButton.onClick = [this]
            {
                if (onApply)
                    onApply(thoughtsEditor.getText());
            };

            cancelButton.onClick = [this]
            {
                requestClose();
            };

            addAndMakeVisible(titleLabel);
            addAndMakeVisible(instructionLabel);
            addAndMakeVisible(thoughtsEditor);
            addAndMakeVisible(dirtyLabel);
            addAndMakeVisible(okButton);
            addAndMakeVisible(cancelButton);
        }

        void paint(juce::Graphics& g) override
        {
            g.fillAll(juce::Colour(0xff171717));
            g.setColour(juce::Colour(0xff4a4a4a));
            g.drawRect(getLocalBounds(), 1);
        }

        void resized() override
        {
            auto area = getLocalBounds().reduced(14);
            titleLabel.setBounds(area.removeFromTop(28));
            instructionLabel.setBounds(area.removeFromTop(42));
            area.removeFromTop(8);

            thoughtsEditor.setBounds(area.removeFromTop(std::max(150, area.getHeight() - 76)));
            area.removeFromTop(8);
            dirtyLabel.setBounds(area.removeFromTop(24));
            area.removeFromTop(6);

            auto buttons = area.removeFromTop(34);
            cancelButton.setBounds(buttons.removeFromRight(90).reduced(4, 2));
            okButton.setBounds(buttons.removeFromRight(90).reduced(4, 2));
        }

        void requestClose()
        {
            if (! isDirty())
            {
                if (onClose)
                    onClose();
                return;
            }

            auto* alert = new juce::AlertWindow(
                "Discard Thoughts Changes?",
                "The Thoughts text has unsaved changes. Discard those changes or keep editing?",
                juce::AlertWindow::WarningIcon
            );

            alert->addButton("Discard Changes", 1);
            alert->addButton("Keep Editing", 0, juce::KeyPress(juce::KeyPress::escapeKey));

            alert->enterModalState(
                true,
                juce::ModalCallbackFunction::create(
                    [this, alert](int result)
                    {
                        std::unique_ptr<juce::AlertWindow> cleanup(alert);

                        if (result == 1)
                        {
                            if (onClose)
                                onClose();
                            return;
                        }

                        dirtyLabel.setText("Unsaved thoughts changes kept.", juce::dontSendNotification);
                        thoughtsEditor.grabKeyboardFocus();
                    }
                ),
                true
            );
        }

    private:
        bool isDirty() const
        {
            return thoughtsEditor.getText() != initialThoughts;
        }

        void updateDirtyLabel()
        {
            dirtyLabel.setText(isDirty() ? "Unsaved thoughts changes." : "No unsaved thoughts changes.", juce::dontSendNotification);
            dirtyLabel.setColour(juce::Label::textColourId, isDirty() ? juce::Colours::yellow : juce::Colours::lightgrey);
        }

        juce::String initialThoughts;
        juce::Label titleLabel;
        juce::Label instructionLabel;
        juce::TextEditor thoughtsEditor;
        juce::Label dirtyLabel;
        juce::TextButton okButton;
        juce::TextButton cancelButton;
        std::function<void(juce::String)> onApply;
        std::function<void()> onClose;
    };

    class TrackManagerWindowContent final : public juce::Component, private juce::ScrollBar::Listener
            {
            public:
                TrackManagerWindowContent(
                    mw::gui::SequenceConsoleComponent& managerText,
                    juce::TextEditor& selectEditor,
                    juce::TextEditor& startBeatEditor,
                    juce::TextEditor& sectionEditor,
                    juce::TextEditor& sectionStartEditor,
                    juce::TextEditor& projectTempoEditor,
                    juce::TextEditor& projectTimeSignatureEditor,
                    juce::TextEditor& projectLoopCountEditor,
                    juce::TextEditor& mapStartBeatEditor,
                    juce::TextEditor& mapBeatWindowEditor,
                    std::function<void()> applyProjectTimingCallback,
                    std::function<void()> refreshCallback,
                    std::function<void()> selectCallback,
                    std::function<void()> selectSequenceCallback,
                    std::function<void()> addCallback,
                    std::function<void()> duplicateCallback,
                    std::function<void()> removeCallback,
                    std::function<void()> removeSequenceCallback,
                    std::function<void()> undoCallback,
                    std::function<void()> applyStartBeatCallback,
                    std::function<void()> duplicateAtBeatCallback,
                    std::function<void()> openPianoRollCallback,
                    std::function<void()> previewTrackCallback,
                    std::function<void()> previewSequenceCallback,
                    std::function<void()> previewProjectCallback,
                    std::function<void()> startFromFileCallback,
                    std::function<void()> importSequenceCallback,
                    std::function<void()> applySectionStartCallback,
                    std::function<void(double)> nudgeSequenceCallback,
                    std::function<void()> addBlankToSectionCallback,
                    std::function<void()> linkTrackToSectionCallback,
                    std::function<void()> unlinkTrackFromSectionCallback,
                    std::function<void()> moveTrackToSequenceCallback,
                    std::function<void()> changeSequenceColourCallback,
                    std::function<void()> renameSequenceCallback,
                    std::function<void()> editSequenceNotesCallback,
                    std::function<void()> toggleSequenceLockCallback,
                    std::function<bool()> isSequenceLockedCallback,
                    std::function<void(const juce::MouseEvent&, bool)> sequenceMapClickCallback,
                    std::function<void()> mapUpCallback,
                    std::function<void()> mapDownCallback,
                    std::function<void()> applyMapWindowCallback,
                    std::function<void(int)> mapHorizontalScrollCallback,
                    std::function<void(int)> mapVerticalScrollCallback,
                    std::function<int()> getMapMaxBeatCallback,
                    std::function<int()> getMapSequenceCountCallback,
                    std::function<void(int)> filterChangedCallback,
                    std::function<void()> toggleNameModeCallback,
                    std::function<bool()> getShortNameModeCallback,
                    std::function<void(juce::Graphics&, juce::Rectangle<int>)> sequenceMapPaintCallback,
                    std::function<void()> applyChangesCallback,
                    std::function<void()> closeCallback
                )
                    : trackText(managerText),
                      selectBox(selectEditor),
                      startBeatBox(startBeatEditor),
                      sectionBox(sectionEditor),
                      sectionStartBox(sectionStartEditor),
                      projectTempoBox(projectTempoEditor),
                      projectTimeSignatureBox(projectTimeSignatureEditor),
                      projectLoopCountBox(projectLoopCountEditor),
                      mapStartBeatBox(mapStartBeatEditor),
                      mapBeatWindowBox(mapBeatWindowEditor),
                      onApplyProjectTiming(std::move(applyProjectTimingCallback)),
                      onRefresh(std::move(refreshCallback)),
                      onSelect(std::move(selectCallback)),
                      onSelectSequence(std::move(selectSequenceCallback)),
                      onAdd(std::move(addCallback)),
                      onDuplicate(std::move(duplicateCallback)),
                      onRemove(std::move(removeCallback)),
                      onRemoveSequence(std::move(removeSequenceCallback)),
                      onUndo(std::move(undoCallback)),
                      onApplyStartBeat(std::move(applyStartBeatCallback)),
                      onDuplicateAtBeat(std::move(duplicateAtBeatCallback)),
                      onOpenPianoRoll(std::move(openPianoRollCallback)),
                      onPreviewTrack(std::move(previewTrackCallback)),
                      onPreviewSequence(std::move(previewSequenceCallback)),
                      onPreviewProject(std::move(previewProjectCallback)),
                      onStartFromFile(std::move(startFromFileCallback)),
                      onImportSequence(std::move(importSequenceCallback)),
                      onApplySectionStart(std::move(applySectionStartCallback)),
                      onNudgeSequence(std::move(nudgeSequenceCallback)),
                      onAddBlankToSection(std::move(addBlankToSectionCallback)),
                      onLinkTrackToSection(std::move(linkTrackToSectionCallback)),
                      onUnlinkTrackFromSection(std::move(unlinkTrackFromSectionCallback)),
                      onMoveTrackToSequence(std::move(moveTrackToSequenceCallback)),
                      onChangeSequenceColour(std::move(changeSequenceColourCallback)),
                      onRenameSequence(std::move(renameSequenceCallback)),
                      onEditSequenceNotes(std::move(editSequenceNotesCallback)),
                      onToggleSequenceLock(std::move(toggleSequenceLockCallback)),
                      isSequenceLocked(std::move(isSequenceLockedCallback)),
                      onMapUp(std::move(mapUpCallback)),
                      onMapDown(std::move(mapDownCallback)),
                      onApplyMapWindow(std::move(applyMapWindowCallback)),
                      onMapHorizontalScroll(std::move(mapHorizontalScrollCallback)),
                      onMapVerticalScroll(std::move(mapVerticalScrollCallback)),
                      getMapMaxBeat(std::move(getMapMaxBeatCallback)),
                      getMapSequenceCount(std::move(getMapSequenceCountCallback)),
                      onFilterChanged(std::move(filterChangedCallback)),
                      onToggleNameMode(std::move(toggleNameModeCallback)),
                      getShortNameMode(std::move(getShortNameModeCallback)),
                      onApplyChanges(std::move(applyChangesCallback)),
                      sequenceMap(
                          std::move(sequenceMapPaintCallback),
                          std::move(sequenceMapClickCallback),
                          [this](const juce::MouseWheelDetails& wheel) { handleMapMouseWheel(wheel); }
                      ),
                      onClose(std::move(closeCallback))
                {
                    helpLabel.setText({}, juce::dontSendNotification);
                    helpLabel.setJustificationType(juce::Justification::centredLeft);
    
                    selectLabel.setText("Track #:", juce::dontSendNotification);
                    selectLabel.setJustificationType(juce::Justification::centredLeft);
                    selectLabel.setFont(juce::FontOptions(15.0f, juce::Font::bold));
    
                    startBeatLabel.setText("Track Start", juce::dontSendNotification);
                    startBeatLabel.setJustificationType(juce::Justification::centredLeft);

                    sectionLabel.setText("Seq #", juce::dontSendNotification);
                    sectionLabel.setJustificationType(juce::Justification::centredLeft);

                    sectionStartLabel.setText("Sequence Start", juce::dontSendNotification);
                    sectionStartLabel.setJustificationType(juce::Justification::centredLeft);

                    projectTempoLabel.setText("Project Tempo", juce::dontSendNotification);
                    projectTempoLabel.setJustificationType(juce::Justification::centredLeft);
                    projectTimeSignatureLabel.setText("Time Sig", juce::dontSendNotification);
                    projectTimeSignatureLabel.setJustificationType(juce::Justification::centredLeft);
                    projectLoopCountLabel.setText("Loop Count", juce::dontSendNotification);
                    projectLoopCountLabel.setJustificationType(juce::Justification::centredLeft);
                    mapBeatWindowLabel.setText("Beats Visible", juce::dontSendNotification);
                    mapBeatWindowLabel.setJustificationType(juce::Justification::centredLeft);
                    mapBeatWindowCombo.addItem("16", 16);
                    mapBeatWindowCombo.addItem("32", 32);
                    mapBeatWindowCombo.addItem("64", 64);
                    mapBeatWindowCombo.addItem("128", 128);
                    mapBeatWindowCombo.addItem("256", 256);
                    mapBeatWindowCombo.addItem("Full", 999);
                    syncMapBeatWindowComboFromText();
    
                    filterLabel.setText("Track Filter", juce::dontSendNotification);
                    filterLabel.setJustificationType(juce::Justification::centredLeft);
                    filterCombo.addItem("All Tracks", 1);
                    filterCombo.addItem("Current Seq", 2);
                    filterCombo.addItem("Muted", 3);
                    filterCombo.addItem("Soloed", 4);
                    filterCombo.addItem("SF2", 5);
                    filterCombo.addItem("SFZ", 6);
                    filterCombo.addItem("Project Backend", 7);
                    filterCombo.setSelectedId(1, juce::dontSendNotification);

                    refreshButton.setButtonText("Refresh");
                    selectButton.setButtonText("Select Track");
                    selectSequenceButton.setButtonText("Select Seq");
                    addButton.setButtonText("Add Blank Track");
                    duplicateButton.setButtonText("Duplicate");
                    duplicateAtBeatButton.setButtonText("Duplicate Beat");
                    applyStartBeatButton.setButtonText("Apply Start Beat");
                    removeButton.setButtonText("Remove Track");
                    removeSequenceButton.setButtonText("Remove Seq");
                    undoButton.setButtonText("Undo");
                    openPianoRollButton.setButtonText("Open In Piano Roll");
                    previewTrackButton.setButtonText("Preview Track");
                    previewSequenceButton.setButtonText("Preview Seq");
                    previewProjectButton.setButtonText("Preview Project");
                    startFromFileButton.setButtonText("Start From File");
                    importSequenceButton.setButtonText("Add Sequence");
                    applySectionStartButton.setButtonText("Apply Sequence Start");
                    applyProjectTimingButton.setButtonText("Apply Project Timing");
                    nudgeSeqMinus4Button.setButtonText("-4");
                    nudgeSeqMinus1Button.setButtonText("-1");
                    nudgeSeqPlus1Button.setButtonText("+1");
                    nudgeSeqPlus4Button.setButtonText("+4");
                    addBlankToSectionButton.setButtonText("Create Blank Seq");
                    moveTrackToSequenceButton.setButtonText("Change Track Seq");
                    changeSequenceColourButton.setButtonText("Chg Color");
                    renameSequenceButton.setButtonText("Rename Seq");
                    sequenceNotesButton.setButtonText("Edit Thoughts");
                    updateLockButton();
                    mapUpButton.setButtonText("Map Up");
                    mapDownButton.setButtonText("Map Down");
                    mapGoButton.setButtonText("Go");
                    updateNameModeButton();
                    applyChangesButton.setButtonText("Apply Changes");
                    helperTooltipWindow = std::make_unique<mw::gui::FreshHoverTooltipWindow>(this, 2000);
                    helperTooltipWindow->setLookAndFeel(&helperTooltipLookAndFeel);

                    auto labelAndControl = [](auto& label, auto& control, const juce::String& text)
                    {
                        label.setTooltip(text);
                        control.setTooltip(text);
                    };

                    labelAndControl(selectLabel, selectBox, "Track number to select or edit in Track Manager.");
                    labelAndControl(startBeatLabel, startBeatBox, "Target beat used by Apply Start Beat or Duplicate Beat.");
                    labelAndControl(sectionLabel, sectionBox, "Sequence row to select or edit. Sequence display names such as Sequence 3 are saved with the project and reused when gaps open.");
                    labelAndControl(sectionStartLabel, sectionStartBox, "Start beat for the selected sequence.");
                    labelAndControl(projectTempoLabel, projectTempoBox, "Project tempo in beats per minute.");
                    labelAndControl(projectTimeSignatureLabel, projectTimeSignatureBox, "Project time signature, such as 4/4.");
                    labelAndControl(projectLoopCountLabel, projectLoopCountBox, "Loop count used for project playback/rendering.");
                    labelAndControl(mapBeatWindowLabel, mapBeatWindowCombo, "Choose the Sequence Map zoom level. Full shows the entire project; smaller values keep scrolling available.");
                    filterLabel.setTooltip("Choose which tracks the Track Manager list should show.");
                    filterCombo.setTooltip("Filter tracks by sequence, mute/solo state, or sample backend.");
                    refreshButton.setTooltip("Refresh the Track Manager text and map view.");
                    selectButton.setTooltip("Select the track number typed in Track #.");
                    selectSequenceButton.setTooltip("Select the sequence row typed in Seq #. Sequence names and IDs stay saved with the project.");
                    addButton.setTooltip("Add a new blank track to the active sequence.");
                    duplicateButton.setTooltip("Duplicate the selected track.");
                    duplicateAtBeatButton.setTooltip("Duplicate the selected track and place the copy at Track Start.");
                    applyStartBeatButton.setTooltip("Move the selected track so its first note starts at Track Start.");
                    removeButton.setTooltip("Remove the selected track from the project.");
                    removeSequenceButton.setTooltip("Remove the selected sequence and its tracks. If any track from that sequence is open in Piano Roll, close Piano Roll first.");
                    undoButton.setTooltip("Undo the most recent Track Manager edit.");
                    openPianoRollButton.setTooltip("Open the selected track in the editable Piano Roll. Empty tracks are linked to a sequence automatically.");
                    previewTrackButton.setTooltip("Render and play a temporary preview of the selected track.");
                    previewSequenceButton.setTooltip("Render and play a temporary preview of the selected sequence.");
                    previewProjectButton.setTooltip("Render and play a temporary preview of the full project from Track Manager using current applied track settings.");
                    startFromFileButton.setTooltip("Import a file as the start of a new project.");
                    importSequenceButton.setTooltip("Add another imported file as a new sequence.");
                    applySectionStartButton.setTooltip("Apply the typed Sequence Start beat to the selected sequence.");
                    applyProjectTimingButton.setTooltip("Apply project tempo, time signature, and loop count.");
                    nudgeSeqMinus4Button.setTooltip("Move the selected sequence 4 beats earlier.");
                    nudgeSeqMinus1Button.setTooltip("Move the selected sequence 1 beat earlier.");
                    nudgeSeqPlus1Button.setTooltip("Move the selected sequence 1 beat later.");
                    nudgeSeqPlus4Button.setTooltip("Move the selected sequence 4 beats later.");
                    addBlankToSectionButton.setTooltip("Add a blank manual sequence.");
                    linkTrackToSectionButton.setTooltip("Link the selected track to the selected sequence.");
                    unlinkTrackFromSectionButton.setTooltip("Unlink the selected track from the selected sequence.");
                    moveTrackToSequenceButton.setTooltip("Move the selected track into the selected sequence.");
                    changeSequenceColourButton.setTooltip("Change the selected sequence colour. Black and near-black colours are avoided so console sequence rails stay visible.");
                    renameSequenceButton.setTooltip("Rename the selected sequence.");
                    sequenceNotesButton.setTooltip("Edit Thoughts for the selected sequence. OK applies, while Cancel/window close warns before discarding unsaved text.");
                    sequenceLockButton.setTooltip("Lock or unlock the selected sequence against accidental edits.");
                    mapUpButton.setTooltip("Scroll the Sequence Map up.");
                    mapDownButton.setTooltip("Scroll the Sequence Map down.");
                    mapGoButton.setTooltip("Apply the selected Beats Visible zoom choice to the Sequence Map.");
                    nameModeButton.setTooltip("Switch Track Manager between full names and shortened names.");
                    applyChangesButton.setTooltip("Apply Track Manager edits to the open project. Save Project still writes them to disk.");
    
                    addAndMakeVisible(selectLabel);
                    addAndMakeVisible(selectBox);
                    addAndMakeVisible(startBeatLabel);
                    addAndMakeVisible(startBeatBox);
                    addAndMakeVisible(sectionLabel);
                    addAndMakeVisible(sectionBox);
                    addAndMakeVisible(sectionStartLabel);
                    addAndMakeVisible(sectionStartBox);
                    addAndMakeVisible(projectTempoLabel);
                    addAndMakeVisible(projectTempoBox);
                    addAndMakeVisible(projectTimeSignatureLabel);
                    addAndMakeVisible(projectTimeSignatureBox);
                    addAndMakeVisible(projectLoopCountLabel);
                    addAndMakeVisible(projectLoopCountBox);
                    addAndMakeVisible(applyProjectTimingButton);
                    addAndMakeVisible(mapBeatWindowLabel);
                    addAndMakeVisible(mapBeatWindowCombo);
                    addAndMakeVisible(mapGoButton);
                    addAndMakeVisible(filterLabel);
                    addAndMakeVisible(filterCombo);
                    addAndMakeVisible(refreshButton);
                    addAndMakeVisible(selectButton);
                    addAndMakeVisible(selectSequenceButton);
                    addAndMakeVisible(addButton);
                    addAndMakeVisible(duplicateButton);
                    addAndMakeVisible(duplicateAtBeatButton);
                    addAndMakeVisible(applyStartBeatButton);
                    addAndMakeVisible(removeButton);
                    addAndMakeVisible(removeSequenceButton);
                    addAndMakeVisible(undoButton);
                    addAndMakeVisible(openPianoRollButton);
                    addAndMakeVisible(previewTrackButton);
                    addAndMakeVisible(previewSequenceButton);
                    addAndMakeVisible(previewProjectButton);
                    addAndMakeVisible(startFromFileButton);
                    addAndMakeVisible(importSequenceButton);
                    addAndMakeVisible(applySectionStartButton);
                    addAndMakeVisible(nudgeSeqMinus4Button);
                    addAndMakeVisible(nudgeSeqMinus1Button);
                    addAndMakeVisible(nudgeSeqPlus1Button);
                    addAndMakeVisible(nudgeSeqPlus4Button);
                    addAndMakeVisible(addBlankToSectionButton);
                    addAndMakeVisible(moveTrackToSequenceButton);
                    addAndMakeVisible(changeSequenceColourButton);
                    addAndMakeVisible(renameSequenceButton);
                    addAndMakeVisible(sequenceNotesButton);
                    addAndMakeVisible(sequenceLockButton);
                    addAndMakeVisible(mapUpButton);
                    addAndMakeVisible(mapDownButton);
                    addAndMakeVisible(nameModeButton);
                    addAndMakeVisible(applyChangesButton);
                    addAndMakeVisible(sequenceMap);
                    addAndMakeVisible(mapHorizontalScrollBar);
                    addAndMakeVisible(mapVerticalScrollBar);
                    addAndMakeVisible(trackText);
    
                    refreshButton.onClick = [this] { if (onRefresh) onRefresh(); };
                    selectButton.onClick = [this] { if (onSelect) onSelect(); };
                    selectSequenceButton.onClick = [this] { if (onSelectSequence) onSelectSequence(); };
                    addButton.onClick = [this] { if (onAdd) onAdd(); };
                    duplicateButton.onClick = [this] { if (onDuplicate) onDuplicate(); };
                    duplicateAtBeatButton.onClick = [this] { if (onDuplicateAtBeat) onDuplicateAtBeat(); };
                    applyStartBeatButton.onClick = [this] { if (onApplyStartBeat) onApplyStartBeat(); };
                    removeButton.onClick = [this] { if (onRemove) onRemove(); };
                    removeSequenceButton.onClick = [this] { if (onRemoveSequence) onRemoveSequence(); };
                    undoButton.onClick = [this] { if (onUndo) onUndo(); };
                    openPianoRollButton.onClick = [this] { if (onOpenPianoRoll) onOpenPianoRoll(); };
                    previewTrackButton.onClick = [this] { if (onPreviewTrack) onPreviewTrack(); };
                    previewSequenceButton.onClick = [this] { if (onPreviewSequence) onPreviewSequence(); };
                    previewProjectButton.onClick = [this] { if (onPreviewProject) onPreviewProject(); };
                    startFromFileButton.onClick = [this] { if (onStartFromFile) onStartFromFile(); };
                    importSequenceButton.onClick = [this] { if (onImportSequence) onImportSequence(); };
                    applySectionStartButton.onClick = [this] { if (onApplySectionStart) onApplySectionStart(); };
                    applyProjectTimingButton.onClick = [this] { if (onApplyProjectTiming) onApplyProjectTiming(); };
                    nudgeSeqMinus4Button.onClick = [this] { if (onNudgeSequence) onNudgeSequence(-4.0); };
                    nudgeSeqMinus1Button.onClick = [this] { if (onNudgeSequence) onNudgeSequence(-1.0); };
                    nudgeSeqPlus1Button.onClick = [this] { if (onNudgeSequence) onNudgeSequence(1.0); };
                    nudgeSeqPlus4Button.onClick = [this] { if (onNudgeSequence) onNudgeSequence(4.0); };
                    addBlankToSectionButton.onClick = [this] { if (onAddBlankToSection) onAddBlankToSection(); };
                    moveTrackToSequenceButton.onClick = [this] { if (onMoveTrackToSequence) onMoveTrackToSequence(); };
                    changeSequenceColourButton.onClick = [this] { if (onChangeSequenceColour) onChangeSequenceColour(); };
                    renameSequenceButton.onClick = [this] { if (onRenameSequence) onRenameSequence(); };
                    sequenceNotesButton.onClick = [this] { if (onEditSequenceNotes) onEditSequenceNotes(); };
                    sequenceLockButton.onClick = [this] { if (onToggleSequenceLock) onToggleSequenceLock(); updateLockButton(); };
                    mapUpButton.onClick = [this] { if (onMapUp) onMapUp(); };
                    mapDownButton.onClick = [this] { if (onMapDown) onMapDown(); };
                    mapGoButton.onClick = [this] { commitSelectedMapBeatWindowChoice(); if (onApplyMapWindow) onApplyMapWindow(); syncMapScrollBars(); };
                    mapHorizontalScrollBar.addListener(this);
                    mapVerticalScrollBar.addListener(this);
                    mapHorizontalScrollBar.setAutoHide(false);
                    mapVerticalScrollBar.setAutoHide(false);
                    mapHorizontalScrollBar.setVisible(true);
                    mapVerticalScrollBar.setVisible(true);
                    nameModeButton.onClick = [this] { if (onToggleNameMode) onToggleNameMode(); updateNameModeButton(); };
                    applyChangesButton.onClick = [this] { if (onApplyChanges) onApplyChanges(); };
                    filterCombo.onChange = [this] { if (onFilterChanged) onFilterChanged(filterCombo.getSelectedId()); };
                }

                void syncMapBeatWindowComboFromText()
                {
                    const auto text = mapBeatWindowBox.getText().trim();
                    const int value = text.getIntValue();

                    if (text.equalsIgnoreCase("Full") || value <= 0)
                    {
                        mapBeatWindowCombo.setSelectedId(999, juce::dontSendNotification);
                        return;
                    }

                    if (value == 16 || value == 32 || value == 64 || value == 128 || value == 256)
                        mapBeatWindowCombo.setSelectedId(value, juce::dontSendNotification);
                    else
                        mapBeatWindowCombo.setSelectedId(999, juce::dontSendNotification);
                }

                void commitSelectedMapBeatWindowChoice()
                {
                    const int selected = mapBeatWindowCombo.getSelectedId();

                    if (selected == 16 || selected == 32 || selected == 64 || selected == 128 || selected == 256)
                        mapBeatWindowBox.setText(juce::String(selected), juce::dontSendNotification);
                    else
                        mapBeatWindowBox.setText("Full", juce::dontSendNotification);
                }

                int getCurrentMapBeatWindow() const
                {
                    const int maxBeat = std::max(4, getMapMaxBeat ? getMapMaxBeat() : 4);
                    const auto text = mapBeatWindowBox.getText().trim();
                    const int requested = text.getIntValue();

                    if (text.equalsIgnoreCase("Full") || requested <= 0)
                        return maxBeat;

                    return std::max(4, requested);
                }
    
                    void refreshSequenceMap()
                {
                    updateLockButton();
                    updateNameModeButton();
                    syncMapBeatWindowComboFromText();
                    syncMapScrollBars();
                    sequenceMap.repaint();
                    repaint();
                }

                void updateLockButton()
                {
                    const bool locked = isSequenceLocked ? isSequenceLocked() : false;
                    sequenceLockButton.setButtonText(locked ? "[LOCKED]" : "[UNLOCKED]");
                    sequenceLockButton.setColour(
                        juce::TextButton::buttonColourId,
                        locked ? juce::Colour(0xff7a2f2f) : juce::Colour(0xff2f6f43)
                    );
                    sequenceLockButton.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
                }

                void updateNameModeButton()
                {
                    const bool shortNames = getShortNameMode ? getShortNameMode() : false;
                    nameModeButton.setButtonText(shortNames ? "Short Names" : "Full Names");
                }

                void setPreviewButtonsEnabled(bool shouldEnable)
                {
                    previewTrackButton.setEnabled(shouldEnable);
                    previewSequenceButton.setEnabled(shouldEnable);
                    previewProjectButton.setEnabled(shouldEnable);
                }

                void syncMapScrollBars()
                {
                    const int beatsVisible = getCurrentMapBeatWindow();
                    const int maxBeat = std::max(beatsVisible, getMapMaxBeat ? getMapMaxBeat() : beatsVisible);
                    const int mapStart = std::clamp(std::max(0, mapStartBeatBox.getText().getIntValue()), 0, std::max(0, maxBeat - beatsVisible));
                    const int sequenceCount = std::max(0, getMapSequenceCount ? getMapSequenceCount() : 0);
                    const int rowsVisible = std::max(1, (sequenceMap.getHeight() - 58) / 18);
                    const int maxFirstSeq = std::max(0, sequenceCount - rowsVisible);

                    mapStartBeatBox.setText(juce::String(mapStart), juce::dontSendNotification);

                    suppressMapScrollCallbacks = true;
                    mapHorizontalScrollBar.setRangeLimits(0.0, static_cast<double>(std::max(beatsVisible, maxBeat)));
                    mapHorizontalScrollBar.setCurrentRange(static_cast<double>(mapStart), static_cast<double>(beatsVisible));
                    mapVerticalScrollBar.setRangeLimits(0.0, static_cast<double>(std::max(rowsVisible, sequenceCount)));
                    mapVerticalScrollBar.setCurrentRange(static_cast<double>(std::clamp(mapVerticalScrollBar.getCurrentRangeStart(), 0.0, static_cast<double>(maxFirstSeq))), static_cast<double>(rowsVisible));
                    suppressMapScrollCallbacks = false;
                }

                void handleMapMouseWheel(const juce::MouseWheelDetails& wheel)
                {
                    const bool horizontal = std::abs(wheel.deltaX) > std::abs(wheel.deltaY);

                    if (horizontal)
                    {
                        const int beatsVisible = getCurrentMapBeatWindow();
                        const int maxBeat = std::max(beatsVisible, getMapMaxBeat ? getMapMaxBeat() : beatsVisible);

                        if (beatsVisible >= maxBeat)
                            return;

                        const int step = std::max(1, beatsVisible / 4);
                        const int direction = (wheel.deltaX < 0.0f || wheel.deltaY < 0.0f) ? 1 : -1;
                        const int nextStart = std::clamp(mapStartBeatBox.getText().getIntValue() + direction * step, 0, std::max(0, maxBeat - beatsVisible));
                        mapStartBeatBox.setText(juce::String(nextStart), juce::dontSendNotification);
                        suppressMapScrollCallbacks = true;
                        mapHorizontalScrollBar.setCurrentRange(static_cast<double>(nextStart), mapHorizontalScrollBar.getCurrentRangeSize());
                        suppressMapScrollCallbacks = false;
                        if (onMapHorizontalScroll)
                            onMapHorizontalScroll(nextStart);
                    }
                    else
                    {
                        const int direction = wheel.deltaY < 0.0f ? 1 : -1;
                        const int nextFirst = static_cast<int>(std::round(mapVerticalScrollBar.getCurrentRangeStart())) + direction;
                        suppressMapScrollCallbacks = true;
                        mapVerticalScrollBar.setCurrentRange(static_cast<double>(std::max(0, nextFirst)), mapVerticalScrollBar.getCurrentRangeSize());
                        suppressMapScrollCallbacks = false;
                        if (onMapVerticalScroll)
                            onMapVerticalScroll(nextFirst);
                    }
                }

                void scrollBarMoved(juce::ScrollBar* scrollBarThatHasMoved, double newRangeStart) override
                {
                    if (suppressMapScrollCallbacks)
                        return;

                    if (scrollBarThatHasMoved == &mapHorizontalScrollBar)
                    {
                        const auto start = static_cast<int>(std::round(newRangeStart));
                        mapStartBeatBox.setText(juce::String(start), juce::dontSendNotification);
                        if (onMapHorizontalScroll)
                            onMapHorizontalScroll(start);
                        return;
                    }

                    if (scrollBarThatHasMoved == &mapVerticalScrollBar)
                    {
                        const auto firstSeq = static_cast<int>(std::round(newRangeStart));
                        if (onMapVerticalScroll)
                            onMapVerticalScroll(firstSeq);
                    }
                }

    void resized() override
                {
                    auto area = getLocalBounds().reduced(10);
    
                    auto top = area.removeFromTop(34);
                    applyChangesButton.setBounds(top.removeFromRight(125).reduced(4, 2));
                    undoButton.setBounds(top.removeFromRight(80).reduced(4, 2));
                    openPianoRollButton.setBounds(top.removeFromRight(130).reduced(4, 2));
                    removeSequenceButton.setBounds(top.removeFromRight(105).reduced(4, 2));
                    removeButton.setBounds(top.removeFromRight(115).reduced(4, 2));
                    duplicateAtBeatButton.setBounds(top.removeFromRight(120).reduced(4, 2));
                    applyStartBeatButton.setBounds(top.removeFromRight(125).reduced(4, 2));
                    startBeatBox.setBounds(top.removeFromRight(65).reduced(4, 2));
                    startBeatLabel.setBounds(top.removeFromRight(105).reduced(4, 2));
                    duplicateButton.setBounds(top.removeFromRight(90).reduced(4, 2));
                    addButton.setBounds(top.removeFromRight(135).reduced(4, 2));
                    refreshButton.setBounds(top.removeFromRight(80).reduced(4, 2));
                    selectButton.setBounds(top.removeFromRight(100).reduced(4, 2));
                    selectBox.setBounds(top.removeFromRight(50).reduced(4, 2));
                    selectLabel.setBounds(top.removeFromRight(82).reduced(4, 2));

                    auto previewRow = area.removeFromTop(34);
                    previewProjectButton.setBounds(previewRow.removeFromRight(125).reduced(4, 2));
                    previewSequenceButton.setBounds(previewRow.removeFromRight(105).reduced(4, 2));
                    previewTrackButton.setBounds(previewRow.removeFromRight(115).reduced(4, 2));
    
                    auto importRow = area.removeFromTop(34);
                    nameModeButton.setBounds(importRow.removeFromRight(105).reduced(4, 2));
                    mapDownButton.setBounds(importRow.removeFromRight(95).reduced(4, 2));
                    mapUpButton.setBounds(importRow.removeFromRight(85).reduced(4, 2));
                    sequenceLockButton.setBounds(importRow.removeFromRight(105).reduced(4, 2));
                    sequenceNotesButton.setBounds(importRow.removeFromRight(95).reduced(4, 2));
                    renameSequenceButton.setBounds(importRow.removeFromRight(105).reduced(4, 2));
                    importSequenceButton.setBounds(importRow.removeFromRight(120).reduced(4, 2));
                    startFromFileButton.setBounds(importRow.removeFromRight(125).reduced(4, 2));

                    auto projectTimingRow = area.removeFromTop(34);
                    applyProjectTimingButton.setBounds(projectTimingRow.removeFromRight(165).reduced(4, 2));
                    projectLoopCountBox.setBounds(projectTimingRow.removeFromRight(55).reduced(4, 2));
                    projectLoopCountLabel.setBounds(projectTimingRow.removeFromRight(90).reduced(4, 2));
                    projectTimeSignatureBox.setBounds(projectTimingRow.removeFromRight(65).reduced(4, 2));
                    projectTimeSignatureLabel.setBounds(projectTimingRow.removeFromRight(70).reduced(4, 2));
                    projectTempoBox.setBounds(projectTimingRow.removeFromRight(75).reduced(4, 2));
                    projectTempoLabel.setBounds(projectTimingRow.removeFromRight(115).reduced(4, 2));

                    auto sectionRow = area.removeFromTop(34);
                    changeSequenceColourButton.setBounds(sectionRow.removeFromRight(88).reduced(4, 2));
                    moveTrackToSequenceButton.setBounds(sectionRow.removeFromRight(135).reduced(4, 2));
                    addBlankToSectionButton.setBounds(sectionRow.removeFromRight(145).reduced(4, 2));
                    nudgeSeqPlus4Button.setBounds(sectionRow.removeFromRight(42).reduced(3, 2));
                    nudgeSeqPlus1Button.setBounds(sectionRow.removeFromRight(42).reduced(3, 2));
                    nudgeSeqMinus1Button.setBounds(sectionRow.removeFromRight(42).reduced(3, 2));
                    nudgeSeqMinus4Button.setBounds(sectionRow.removeFromRight(42).reduced(3, 2));
                    applySectionStartButton.setBounds(sectionRow.removeFromRight(155).reduced(4, 2));
                    sectionStartBox.setBounds(sectionRow.removeFromRight(70).reduced(4, 2));
                    sectionStartLabel.setBounds(sectionRow.removeFromRight(115).reduced(4, 2));
                    selectSequenceButton.setBounds(sectionRow.removeFromRight(95).reduced(4, 2));
                    sectionBox.setBounds(sectionRow.removeFromRight(50).reduced(4, 2));
                    sectionLabel.setBounds(sectionRow.removeFromRight(48).reduced(4, 2));

                    auto filterRow = area.removeFromTop(34);
                    filterLabel.setBounds(filterRow.removeFromLeft(100).reduced(4, 2));
                    filterCombo.setBounds(filterRow.removeFromLeft(210).reduced(4, 2));

                    auto mapNavRow = area.removeFromTop(34);
                    mapBeatWindowLabel.setBounds(mapNavRow.removeFromLeft(115).reduced(4, 2));
                    mapBeatWindowCombo.setBounds(mapNavRow.removeFromLeft(90).reduced(4, 2));
                    mapGoButton.setBounds(mapNavRow.removeFromLeft(55).reduced(4, 2));

                    area.removeFromTop(4);
                    auto mapAndScroll = area.removeFromTop(std::max(260, area.getHeight() * 3 / 5));
                    auto bottomScroll = mapAndScroll.removeFromBottom(18);
                    auto rightScroll = mapAndScroll.removeFromRight(18);
                    sequenceMap.setBounds(mapAndScroll);
                    mapVerticalScrollBar.setBounds(rightScroll);
                    mapHorizontalScrollBar.setBounds(bottomScroll);
                    mapVerticalScrollBar.toFront(false);
                    mapHorizontalScrollBar.toFront(false);
                    area.removeFromTop(6);
                    trackText.setBounds(area);
                }
    
            private:
                mw::gui::SequenceConsoleComponent& trackText;
                juce::TextEditor& selectBox;
                juce::TextEditor& startBeatBox;
                juce::TextEditor& sectionBox;
                juce::TextEditor& sectionStartBox;
                juce::TextEditor& projectTempoBox;
                juce::TextEditor& projectTimeSignatureBox;
                juce::TextEditor& projectLoopCountBox;
                juce::TextEditor& mapStartBeatBox;
                juce::TextEditor& mapBeatWindowBox;
    
                juce::Label helpLabel;
                juce::Label selectLabel;
                juce::Label startBeatLabel;
                juce::Label sectionLabel;
                juce::Label sectionStartLabel;
                juce::Label projectTempoLabel;
                juce::Label projectTimeSignatureLabel;
                juce::Label projectLoopCountLabel;
                juce::Label mapStartBeatLabel;
                juce::Label mapBeatWindowLabel;
                juce::ComboBox mapBeatWindowCombo;
                juce::Label filterLabel;
                juce::ComboBox filterCombo;
                juce::TextButton refreshButton;
                juce::TextButton selectButton;
                juce::TextButton selectSequenceButton;
                juce::TextButton addButton;
                juce::TextButton duplicateButton;
                juce::TextButton duplicateAtBeatButton;
                juce::TextButton applyStartBeatButton;
                juce::TextButton removeButton;
                juce::TextButton removeSequenceButton;
                juce::TextButton undoButton;
                juce::TextButton closeButton;
                juce::TextButton applyChangesButton;
                juce::TextButton openPianoRollButton;
                juce::TextButton previewTrackButton;
                juce::TextButton previewSequenceButton;
                juce::TextButton previewProjectButton;
                juce::TextButton startFromFileButton;
                juce::TextButton importSequenceButton;
                juce::TextButton applySectionStartButton;
                juce::TextButton applyProjectTimingButton;
                juce::TextButton nudgeSeqMinus4Button;
                juce::TextButton nudgeSeqMinus1Button;
                juce::TextButton nudgeSeqPlus1Button;
                juce::TextButton nudgeSeqPlus4Button;
                juce::TextButton addBlankToSectionButton;
                juce::TextButton linkTrackToSectionButton;
                juce::TextButton unlinkTrackFromSectionButton;
                juce::TextButton moveTrackToSequenceButton;
                juce::TextButton changeSequenceColourButton;
                juce::TextButton renameSequenceButton;
                juce::TextButton sequenceNotesButton;
                juce::TextButton sequenceLockButton;
                juce::TextButton mapUpButton;
                juce::TextButton mapDownButton;
                juce::TextButton mapGoButton;
                juce::ScrollBar mapHorizontalScrollBar { false };
                juce::ScrollBar mapVerticalScrollBar { true };
                bool suppressMapScrollCallbacks = false;
                juce::TextButton nameModeButton;
                SequenceMapView sequenceMap;
                mw::gui::HelperTooltipLookAndFeel helperTooltipLookAndFeel;
                std::unique_ptr<juce::TooltipWindow> helperTooltipWindow;
    
                std::function<void()> onRefresh;
                std::function<void()> onSelect;
                std::function<void()> onSelectSequence;
                std::function<void()> onAdd;
                std::function<void()> onDuplicate;
                std::function<void()> onRemove;
                std::function<void()> onRemoveSequence;
                std::function<void()> onUndo;
                std::function<void()> onApplyStartBeat;
                std::function<void()> onDuplicateAtBeat;
                std::function<void()> onOpenPianoRoll;
                std::function<void()> onPreviewTrack;
                std::function<void()> onPreviewSequence;
                std::function<void()> onPreviewProject;
                std::function<void()> onStartFromFile;
                std::function<void()> onImportSequence;
                std::function<void()> onApplySectionStart;
                std::function<void()> onApplyProjectTiming;
                std::function<void(double)> onNudgeSequence;
                std::function<void()> onAddBlankToSection;
                std::function<void()> onLinkTrackToSection;
                std::function<void()> onUnlinkTrackFromSection;
                std::function<void()> onMoveTrackToSequence;
                std::function<void()> onChangeSequenceColour;
                std::function<void()> onRenameSequence;
                std::function<void()> onEditSequenceNotes;
                std::function<void()> onToggleSequenceLock;
                std::function<bool()> isSequenceLocked;
                std::function<void()> onMapUp;
                std::function<void()> onMapDown;
                std::function<void()> onApplyMapWindow;
                std::function<void(int)> onMapHorizontalScroll;
                std::function<void(int)> onMapVerticalScroll;
                std::function<int()> getMapMaxBeat;
                std::function<int()> getMapSequenceCount;
                std::function<void(int)> onFilterChanged;
                std::function<void()> onToggleNameMode;
                std::function<bool()> getShortNameMode;
                std::function<void()> onApplyChanges;
                std::function<void()> onClose;
            };
    
            class RawNotesWindowContent final : public juce::Component
            {
            public:
                RawNotesWindowContent(
                    juce::TextEditor& editor,
                    std::function<void()> applyCallback
                )
                    : noteEditor(editor),
                      onApply(std::move(applyCallback))
                {
                    helpLabel.setText(
                        "Advanced raw note CSV editor: pitch,startBeat,durationBeats,velocity",
                        juce::dontSendNotification
                    );
                    helpLabel.setJustificationType(juce::Justification::centredLeft);
    
                    applyButton.setButtonText("Apply Raw Notes");
    
                    addAndMakeVisible(noteEditor);
                    addAndMakeVisible(applyButton);
    
                    applyButton.onClick = [this]
                    {
                        if (onApply)
                            onApply();
                    };
    
                }
    
                void resized() override
                {
                    auto area = getLocalBounds().reduced(10);
    
                    auto top = area.removeFromTop(34);
                    applyButton.setBounds(top.removeFromRight(140).reduced(4, 2));
                    helpLabel.setBounds(top.reduced(4, 2));
    
                    area.removeFromTop(8);
                    noteEditor.setBounds(area);
                }
    
            private:
                juce::TextEditor& noteEditor;
                juce::Label helpLabel;
                juce::TextButton applyButton;
                std::function<void()> onApply;
            };
    
    
            
    class MiniNoteMapComponent final : public juce::Component,
                                               private juce::Timer
            {
            public:
                explicit MiniNoteMapComponent(mw::gui::PianoRollComponent& rollComponent)
                    : roll(rollComponent)
                {
                    setInterceptsMouseClicks(false, false);
                    startTimerHz(12);
                }

                void paint(juce::Graphics& g) override
                {
                    const auto area = getLocalBounds().toFloat();

                    g.setColour(juce::Colour(0xff151515));
                    g.fillRoundedRectangle(area, 5.0f);

                    const auto inner = area.reduced(6.0f, 5.0f);

                    if (inner.getWidth() <= 4.0f || inner.getHeight() <= 4.0f)
                        return;

                    g.setColour(juce::Colour(0xff242424));
                    g.fillRoundedRectangle(inner, 4.0f);
                    g.setColour(juce::Colour(0xff3a3a3a));
                    g.drawRoundedRectangle(inner, 4.0f, 1.0f);

                    const auto& notes = roll.getNotes();

                    int maxBeat = roll.getStartBeat() + roll.getBeatsVisible();
                    int lowestPitch = roll.getLowPitch();
                    int highestPitch = roll.getHighPitch();

                    for (const auto& note : notes)
                    {
                        const int endBeat = static_cast<int>(
                            std::ceil(
                                static_cast<double>(note.startTick + note.durationTicks)
                                / static_cast<double>(mw::core::Project::ticksPerQuarterNote)
                            )
                        );

                        maxBeat = std::max(maxBeat, endBeat);
                        lowestPitch = std::min(lowestPitch, note.pitch);
                        highestPitch = std::max(highestPitch, note.pitch);
                    }

                    maxBeat = std::max(maxBeat, 1);
                    lowestPitch = std::clamp(lowestPitch - 2, 0, 127);
                    highestPitch = std::clamp(highestPitch + 2, lowestPitch + 1, 127);

                    const int pitchSpan = std::max(1, highestPitch - lowestPitch);
                    const float pitchLaneHeight = inner.getHeight() / static_cast<float>(pitchSpan + 1);
                    const float noteBlockHeight = std::clamp(pitchLaneHeight * 0.85f, 2.0f, 4.5f);

                    const auto beatToX = [inner, maxBeat](double beat)
                    {
                        return inner.getX()
                            + static_cast<float>(std::clamp(beat / static_cast<double>(maxBeat), 0.0, 1.0))
                            * inner.getWidth();
                    };

                    const auto pitchToY = [inner, lowestPitch, highestPitch](int pitch)
                    {
                        const double normalised =
                            static_cast<double>(std::clamp(pitch, lowestPitch, highestPitch) - lowestPitch)
                            / static_cast<double>(std::max(1, highestPitch - lowestPitch));

                        return inner.getBottom()
                            - static_cast<float>(normalised) * inner.getHeight();
                    };

                    const int markerStep = maxBeat <= 16 ? 2 : (maxBeat <= 64 ? 4 : (maxBeat <= 128 ? 8 : 16));

                    for (int beat = 0; beat <= maxBeat; beat += markerStep)
                    {
                        const float x = beatToX(static_cast<double>(beat));
                        const bool major = (beat % (markerStep * 4)) == 0;
                        g.setColour(major ? juce::Colour(0xff363636) : juce::Colour(0xff2c2c2c));
                        g.drawVerticalLine(static_cast<int>(x), inner.getY(), inner.getBottom());
                    }

                    const int pitchLaneStep = (highestPitch - lowestPitch) <= 24 ? 6 : 12;
                    const int firstLane = ((lowestPitch + pitchLaneStep - 1) / pitchLaneStep) * pitchLaneStep;

                    for (int pitch = firstLane; pitch <= highestPitch; pitch += pitchLaneStep)
                    {
                        const float y = pitchToY(pitch);
                        g.setColour(juce::Colour(0xff2b2b2b));
                        g.drawHorizontalLine(static_cast<int>(y), inner.getX(), inner.getRight());
                    }

                    // Draw compressed note blocks before the viewport overlay so the map stays detailed.
                    for (const auto& note : notes)
                    {
                        const double startBeat =
                            static_cast<double>(note.startTick)
                            / static_cast<double>(mw::core::Project::ticksPerQuarterNote);

                        const double durationBeats =
                            std::max(
                                0.08,
                                static_cast<double>(note.durationTicks)
                                / static_cast<double>(mw::core::Project::ticksPerQuarterNote)
                            );

                        const float x = beatToX(startBeat);
                        const float right = beatToX(startBeat + durationBeats);
                        const float y = pitchToY(note.pitch);
                        const bool isPlayingNote =
                            roll.isPreviewPlayheadActive()
                            && roll.getPreviewPlayheadBeat() >= startBeat
                            && roll.getPreviewPlayheadBeat() < startBeat + durationBeats;

                        juce::Rectangle<float> r(
                            x,
                            y - (noteBlockHeight * 0.5f),
                            std::max(2.5f, right - x),
                            noteBlockHeight
                        );

                        if (isPlayingNote)
                            g.setColour(juce::Colour(0xffffd23f));
                        else if (roll.isPreviewPlayheadActive())
                            g.setColour(juce::Colour(0x887fd6ff));
                        else
                            g.setColour(juce::Colour(0xdd7fd6ff));

                        g.fillRoundedRectangle(r, 1.8f);

                        if (isPlayingNote)
                        {
                            g.setColour(juce::Colour(0xffffffff));
                            g.drawRoundedRectangle(r.expanded(0.75f), 2.0f, 1.2f);
                        }
                        else
                        {
                            g.setColour(juce::Colour(0x66101010));
                            g.drawRoundedRectangle(r, 1.8f, 0.75f);
                        }
                    }

                    // Current visible beat window highlight.
                    const float visibleX = beatToX(static_cast<double>(roll.getStartBeat()));
                    const float visibleRight = beatToX(static_cast<double>(roll.getStartBeat() + roll.getBeatsVisible()));

                    juce::Rectangle<float> visibleWindow(
                        visibleX,
                        inner.getY(),
                        std::max(3.0f, visibleRight - visibleX),
                        inner.getHeight()
                    );

                    g.setColour(juce::Colour(0x24ffe066));
                    g.fillRect(visibleWindow);
                    g.setColour(juce::Colour(0xffffe066));
                    g.drawRect(visibleWindow, 2.0f);

                    if (roll.isPreviewPlayheadActive())
                    {
                        const float x = beatToX(roll.getPreviewPlayheadBeat());

                        g.setColour(juce::Colour(0x55ffe066));
                        g.fillRect(x - 3.0f, inner.getY(), 6.0f, inner.getHeight());

                        g.setColour(juce::Colour(0xffffe066));
                        g.drawLine(x, inner.getY(), x, inner.getBottom(), 3.0f);
                    }
                }

            private:

                void timerCallback() override
                {
                    repaint();
                }

                mw::gui::PianoRollComponent& roll;
            };


class PianoRollWindowContent final : public juce::Component
            {
            public:
                                static juce::Colour headerTrackColourForIndex(int trackIndex)
                {
                    static const juce::Colour colours[] =
                    {
                        juce::Colour(0xff6ec6ff),
                        juce::Colour(0xff79e27d),
                        juce::Colour(0xffffb55a),
                        juce::Colour(0xffc58cff),
                        juce::Colour(0xffff7f9f),
                        juce::Colour(0xfff6e66d),
                        juce::Colour(0xff77e0d8),
                        juce::Colour(0xffd0d0d0)
                    };

                    return colours[std::max(0, trackIndex) % 8];
                }

PianoRollWindowContent(
                    mw::gui::PianoRollComponent& rollComponent,
                    juce::TextEditor& bpmEditor,
                    juce::TextEditor& sigEditor,
                    juce::TextEditor& beatWindowEditor,
                    juce::TextEditor& noteLengthEditor,
                    juce::TextEditor& velocityEditor,
                    juce::TextEditor& snapEditor,
                    juce::TextEditor& trackLibraryEditor,
                    juce::ComboBox& instrumentChooser,
                    juce::TextButton& changeLibraryButton,
                    juce::Label& pageIndicatorLabel,
                    juce::TextEditor& pageEditor,
                    juce::Label& keyRangeLabel,
                    juce::Label& keyJumpLabel,
                    juce::TextEditor& keyJumpEditor,
                    juce::TextButton& keyJumpButton,
                    juce::TextButton& notesDownButton,
                    juce::TextButton& notesUpButton,
                    juce::TextButton& beat4Button,
                    juce::TextButton& beat8Button,
                    juce::TextButton& beat16Button,
                    juce::TextButton& beat32Button,
                    juce::TextButton& beat64Button,
                    juce::TextButton& previousButton,
                    juce::TextButton& nextButton,
                    juce::TextButton& jumpPageButton,
                    juce::TextButton& copyButton,
                    juce::TextButton& pasteButton,
                    juce::TextButton& undoButton,
                    juce::TextButton& redoButton,
                    juce::TextButton& clearSelectionButton,
                    int trackIndexForHeader,
                    const juce::String& trackNameForHeader,
                    const juce::String& trackInstrumentForHeader,
                    std::function<void()> applySettingsCallback,
                    std::function<void()> applyCallback,
                    std::function<void()> previewCallback,
                    std::function<void()> changeLibraryCallback
                )
                    : roll(rollComponent),
                      bpmBox(bpmEditor),
                      timeSigBox(sigEditor),
                      beatWindowBox(beatWindowEditor),
                      noteLengthBox(noteLengthEditor),
                      velocityBox(velocityEditor),
                      snapBox(snapEditor),
                      trackLibraryBox(trackLibraryEditor),
                      instrumentCombo(instrumentChooser),
                      changeLibrary(changeLibraryButton),
                      miniMap(rollComponent),
                      pageLabel(pageIndicatorLabel),
                      pageBox(pageEditor),
                      keyRange(keyRangeLabel),
                      keyJump(keyJumpLabel),
                      keyJumpBox(keyJumpEditor),
                      keyJumpGo(keyJumpButton),
                      notesDown(notesDownButton),
                      notesUp(notesUpButton),
                      beat4(beat4Button),
                      beat8(beat8Button),
                      beat16(beat16Button),
                      beat32(beat32Button),
                      beat64(beat64Button),
                      previous(previousButton),
                      next(nextButton),
                      jumpPage(jumpPageButton),
                      copyNote(copyButton),
                      pasteNote(pasteButton),
                      undoNote(undoButton),
                      redoNote(redoButton),
                      clearSelection(clearSelectionButton),
                      headerTrackIndex(trackIndexForHeader),
                      headerTrackName(trackNameForHeader),
                      headerInstrumentText(trackInstrumentForHeader),
                      onApplySettings(std::move(applySettingsCallback)),
                      onApply(std::move(applyCallback)),
                      onPreview(std::move(previewCallback)),
                      onChangeLibrary(std::move(changeLibraryCallback))
                {
                    trackColourSwatch.setColour(juce::Label::backgroundColourId, headerTrackColourForIndex(headerTrackIndex));
                    trackColourSwatch.setColour(juce::Label::outlineColourId, juce::Colours::black.withAlpha(0.35f));
                    trackNameLabel.setText(headerTrackName, juce::dontSendNotification);
                    trackNameLabel.setJustificationType(juce::Justification::centredLeft);
                    trackNameLabel.setColour(juce::Label::textColourId, juce::Colours::white);
                    headerInstrumentLabel.setText(headerInstrumentText, juce::dontSendNotification);
                    headerInstrumentLabel.setJustificationType(juce::Justification::centredLeft);
                    headerInstrumentLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.82f));

                    bpmLabel.setText("Tempo BPM", juce::dontSendNotification);
                    sigLabel.setText("Time Sig", juce::dontSendNotification);
                    beatWindowLabel.setText("Beats Visible", juce::dontSendNotification);
                    noteLengthLabel.setText("Note Length", juce::dontSendNotification);
                    velocityLabel.setText("Velocity", juce::dontSendNotification);
                    snapLabel.setText("Snap Grid", juce::dontSendNotification);
                    libraryLabel.setText("Library", juce::dontSendNotification);
                    instrumentLabel.setText("Instrument", juce::dontSendNotification);
                    pageInputLabel.setText("Window", juce::dontSendNotification);
    
                    trackLibraryBox.setReadOnly(true);
                    beatWindowBox.setReadOnly(true);
                    beatWindowBox.setJustification(juce::Justification::centred);
    
                    applySettingsButton.setButtonText("Apply Track Settings");
                    applyButton.setButtonText("Apply Changes");
                    previewButton.setButtonText("▶ Player");
                    helperTooltipWindow = std::make_unique<mw::gui::FreshHoverTooltipWindow>(this, 2000);
                    helperTooltipWindow->setLookAndFeel(&helperTooltipLookAndFeel);

                    auto labelAndControl = [](auto& label, auto& control, const juce::String& text)
                    {
                        label.setTooltip(text);
                        control.setTooltip(text);
                    };

                    trackNameLabel.setTooltip("Track opened in this Piano Roll window.");
                    headerInstrumentLabel.setTooltip("Current or pending instrument for this Piano Roll track.");
                    labelAndControl(bpmLabel, bpmBox, "Tempo used for Piano Roll preview playback.");
                    labelAndControl(sigLabel, timeSigBox, "Time signature displayed for Piano Roll timing.");
                    labelAndControl(beatWindowLabel, beatWindowBox, "Current Piano Roll beat window size. Use the nearby beat buttons to change it.");
                    labelAndControl(noteLengthLabel, noteLengthBox, "Default length for newly added Piano Roll notes.");
                    labelAndControl(velocityLabel, velocityBox, "Default velocity/loudness for newly added Piano Roll notes.");
                    labelAndControl(snapLabel, snapBox, "Grid snap value for note placement and editing.");
                    labelAndControl(libraryLabel, trackLibraryBox, "Shows the sample library assigned to this Piano Roll track, including the active SF2/SFZ backend.");
                    labelAndControl(instrumentLabel, instrumentCombo, "Choose the instrument or preset for this track. Click Apply Track Settings to confirm.");
                    changeLibrary.setTooltip("Choose a different SF2/SF3 or SFZ library for this Piano Roll track.");
                    labelAndControl(pageInputLabel, pageBox, "Window number to jump to in the Piano Roll.");
                    labelAndControl(keyJump, keyJumpBox, "Pitch name to jump to, such as C4, A5, or B2.");
                    notesDown.setTooltip("Shift the visible Piano Roll note range down one octave.");
                    notesUp.setTooltip("Shift the visible Piano Roll note range up one octave.");
                    keyJumpGo.setTooltip("Jump the visible note range to a typed note such as B3 or C4.");
                    beat4.setTooltip("Show 4 beats in the Piano Roll window.");
                    beat8.setTooltip("Show 8 beats in the Piano Roll window.");
                    beat16.setTooltip("Show 16 beats in the Piano Roll window.");
                    beat32.setTooltip("Show 32 beats in the Piano Roll window.");
                    beat64.setTooltip("Show 64 beats in the Piano Roll window.");
                    previous.setTooltip("Move to the previous Piano Roll beat window.");
                    next.setTooltip("Move to the next Piano Roll beat window.");
                    jumpPage.setTooltip("Go to the typed Piano Roll window number.");
                    copyNote.setTooltip("Copy the selected note.");
                    pasteNote.setTooltip("Paste the copied note.");
                    undoNote.setTooltip("Undo the most recent Piano Roll edit.");
                    redoNote.setTooltip("Redo the most recently undone Piano Roll edit.");
                    clearSelection.setTooltip("Clear the current note selection.");
                    applySettingsButton.setTooltip("Apply Piano Roll timing controls plus pending track library and instrument changes. This does not reload or discard unsaved note edits.");
                    applyButton.setTooltip("Apply Piano Roll note edits to the open project. Save Project still writes them to disk.");
                    previewButton.setTooltip("Open the Preview Player for this Piano Roll using the current notes and track settings.");
    
                    helpLabel.setText(
                        "Shift-click or Shift-drag selects multiple notes.",
                        juce::dontSendNotification
                    );
                    helpLabel.setJustificationType(juce::Justification::centredLeft);
    
                    addAndMakeVisible(trackColourSwatch);
                    addAndMakeVisible(trackNameLabel);
                    addAndMakeVisible(headerInstrumentLabel);
                    addAndMakeVisible(bpmLabel);
                    addAndMakeVisible(bpmBox);
                    addAndMakeVisible(sigLabel);
                    addAndMakeVisible(timeSigBox);
                    addAndMakeVisible(beatWindowLabel);
                    addAndMakeVisible(beatWindowBox);
                    addAndMakeVisible(beat4);
                    addAndMakeVisible(beat8);
                    addAndMakeVisible(beat16);
                    addAndMakeVisible(beat32);
                    addAndMakeVisible(beat64);
                    addAndMakeVisible(noteLengthLabel);
                    addAndMakeVisible(noteLengthBox);
                    addAndMakeVisible(velocityLabel);
                    addAndMakeVisible(velocityBox);
                    addAndMakeVisible(snapLabel);
                    addAndMakeVisible(snapBox);
                    addAndMakeVisible(libraryLabel);
                    addAndMakeVisible(trackLibraryBox);
                    addAndMakeVisible(changeLibrary);
                    addAndMakeVisible(instrumentLabel);
                    addAndMakeVisible(instrumentCombo);
                    addAndMakeVisible(previous);
                    addAndMakeVisible(next);
                    addAndMakeVisible(pageLabel);
                    addAndMakeVisible(pageInputLabel);
                    addAndMakeVisible(pageBox);
                    addAndMakeVisible(jumpPage);
                    addAndMakeVisible(keyRange);
                    addAndMakeVisible(keyJump);
                    addAndMakeVisible(keyJumpBox);
                    addAndMakeVisible(keyJumpGo);
                    addAndMakeVisible(notesDown);
                    addAndMakeVisible(notesUp);
                    addAndMakeVisible(copyNote);
                    addAndMakeVisible(pasteNote);
                    addAndMakeVisible(undoNote);
                    addAndMakeVisible(redoNote);
                    addAndMakeVisible(clearSelection);
                    addAndMakeVisible(applySettingsButton);
                    addAndMakeVisible(applyButton);
                    addAndMakeVisible(previewButton);
                    addAndMakeVisible(miniMap);
                    addAndMakeVisible(roll);
    
                    changeLibrary.onClick = [this]
                    {
                        if (onChangeLibrary)
                            onChangeLibrary();
                    };

                    applySettingsButton.onClick = [this]
                    {
                        if (onApplySettings)
                            onApplySettings();
                    };
    
                    applyButton.onClick = [this]
                    {
                        if (onApplySettings)
                            onApplySettings();
    
                        if (onApply)
                            onApply();
                    };
    
                    previewButton.onClick = [this]
                    {
                        if (onPreview)
                            onPreview();
                    };
    
                }

                void setHeaderTrackInfo(int trackIndexForHeader, const juce::String& trackNameForHeader)
                {
                    headerTrackIndex = trackIndexForHeader;
                    headerTrackName = trackNameForHeader;
                    trackColourSwatch.setColour(juce::Label::backgroundColourId, headerTrackColourForIndex(headerTrackIndex));
                    trackNameLabel.setText(headerTrackName, juce::dontSendNotification);
                    repaint();
                }

                void setHeaderInstrumentInfo(const juce::String& trackInstrumentForHeader)
                {
                    headerInstrumentText = trackInstrumentForHeader;
                    headerInstrumentLabel.setText(headerInstrumentText, juce::dontSendNotification);
                    repaint();
                }
    
    void resized() override
                {
                    auto area = getLocalBounds().reduced(10);
    
                    auto toolbar = area.removeFromTop(34);
                    trackColourSwatch.setBounds(toolbar.removeFromLeft(26).reduced(4, 7));
                    trackNameLabel.setBounds(toolbar.removeFromLeft(265).reduced(4, 2));
                    previewButton.setBounds(toolbar.removeFromRight(95).reduced(4, 2));
                    applyButton.setBounds(toolbar.removeFromRight(125).reduced(4, 2));
                    applySettingsButton.setBounds(toolbar.removeFromRight(160).reduced(4, 2));
                    headerInstrumentLabel.setBounds(toolbar.reduced(4, 2));

                    auto libraryRow = area.removeFromTop(34);
                    libraryLabel.setBounds(libraryRow.removeFromLeft(70));

                    // Keep the backend hidden in the Piano Roll because the library text
                    // already shows SF2/SFZ. Leave the underlying combo/state available
                    // for existing track-setting logic, but do not place it in the UI.
                    const int changeLibraryWidth = 135;
                    const int instrumentLabelWidth = 82;
                    const int desiredInstrumentWidth = 410;
                    const int minimumInstrumentWidth = 240;
                    int libraryBoxWidth = 280;
                    int instrumentWidth = desiredInstrumentWidth;

                    const int availableWidth = libraryRow.getWidth();
                    const int desiredTotalWidth = libraryBoxWidth + changeLibraryWidth + instrumentLabelWidth + instrumentWidth;
                    if (desiredTotalWidth > availableWidth)
                    {
                        const int overflow = desiredTotalWidth - availableWidth;
                        instrumentWidth = juce::jmax(minimumInstrumentWidth, desiredInstrumentWidth - overflow);

                        const int compactTotalWidth = libraryBoxWidth + changeLibraryWidth + instrumentLabelWidth + instrumentWidth;
                        if (compactTotalWidth > availableWidth)
                            libraryBoxWidth = juce::jmax(180, availableWidth - changeLibraryWidth - instrumentLabelWidth - instrumentWidth);
                    }

                    trackLibraryBox.setBounds(libraryRow.removeFromLeft(libraryBoxWidth).reduced(4, 2));
                    changeLibrary.setBounds(libraryRow.removeFromLeft(changeLibraryWidth).reduced(4, 2));
                    instrumentLabel.setBounds(libraryRow.removeFromLeft(instrumentLabelWidth));
                    instrumentCombo.setBounds(libraryRow.removeFromLeft(instrumentWidth).reduced(4, 2));

                    auto settingsRow = area.removeFromTop(34);
                    bpmLabel.setBounds(settingsRow.removeFromLeft(82));
                    bpmBox.setBounds(settingsRow.removeFromLeft(60).reduced(4, 2));
                    sigLabel.setBounds(settingsRow.removeFromLeft(72));
                    timeSigBox.setBounds(settingsRow.removeFromLeft(60).reduced(4, 2));
                    beatWindowLabel.setBounds(settingsRow.removeFromLeft(105));
                    beatWindowBox.setBounds(settingsRow.removeFromLeft(52).reduced(4, 2));
                    beat4.setBounds(settingsRow.removeFromLeft(32).reduced(2, 2));
                    beat8.setBounds(settingsRow.removeFromLeft(32).reduced(2, 2));
                    beat16.setBounds(settingsRow.removeFromLeft(38).reduced(2, 2));
                    beat32.setBounds(settingsRow.removeFromLeft(38).reduced(2, 2));
                    beat64.setBounds(settingsRow.removeFromLeft(38).reduced(2, 2));
                    noteLengthLabel.setBounds(settingsRow.removeFromLeft(92));
                    noteLengthBox.setBounds(settingsRow.removeFromLeft(70).reduced(4, 2));
                    velocityLabel.setBounds(settingsRow.removeFromLeft(72));
                    velocityBox.setBounds(settingsRow.removeFromLeft(60).reduced(4, 2));
                    snapLabel.setBounds(settingsRow.removeFromLeft(76));
                    snapBox.setBounds(settingsRow.removeFromLeft(70).reduced(4, 2));

                    area.removeFromTop(4);
                    auto navRow = area.removeFromTop(34);
                    previous.setBounds(navRow.removeFromLeft(105).reduced(4, 2));
                    next.setBounds(navRow.removeFromLeft(105).reduced(4, 2));
                    pageLabel.setBounds(navRow.removeFromLeft(260).reduced(4, 2));
                    pageInputLabel.setBounds(navRow.removeFromLeft(65));
                    pageBox.setBounds(navRow.removeFromLeft(55).reduced(4, 2));
                    jumpPage.setBounds(navRow.removeFromLeft(60).reduced(4, 2));
                    keyRange.setBounds(navRow.removeFromLeft(150).reduced(4, 2));
                    keyJump.setBounds(navRow.removeFromLeft(65).reduced(4, 2));
                    keyJumpBox.setBounds(navRow.removeFromLeft(60).reduced(4, 2));
                    keyJumpGo.setBounds(navRow.removeFromLeft(65).reduced(4, 2));
                    notesDown.setBounds(navRow.removeFromLeft(105).reduced(4, 2));
                    notesUp.setBounds(navRow.removeFromLeft(90).reduced(4, 2));
                    copyNote.setBounds(navRow.removeFromLeft(65).reduced(4, 2));
                    pasteNote.setBounds(navRow.removeFromLeft(70).reduced(4, 2));
                    undoNote.setBounds(navRow.removeFromLeft(65).reduced(4, 2));
                    redoNote.setBounds(navRow.removeFromLeft(65).reduced(4, 2));
                    clearSelection.setBounds(navRow.removeFromLeft(90).reduced(4, 2));
                    helpLabel.setBounds(navRow.reduced(4, 2));
    
                    area.removeFromTop(6);
    
                    auto miniMapArea = area.removeFromTop(76);
                    miniMap.setBounds(miniMapArea.reduced(2, 2));
    
                    area.removeFromTop(6);
                    roll.setBounds(area);
                }
    
            private:
                mw::gui::PianoRollComponent& roll;
                juce::TextEditor& bpmBox;
                juce::TextEditor& timeSigBox;
                juce::TextEditor& beatWindowBox;
                juce::TextEditor& noteLengthBox;
                juce::TextEditor& velocityBox;
                juce::TextEditor& snapBox;
                juce::TextEditor& trackLibraryBox;
                juce::ComboBox& instrumentCombo;
                juce::TextButton& changeLibrary;
                MiniNoteMapComponent miniMap;
                juce::Label& pageLabel;
                juce::TextEditor& pageBox;
                juce::Label& keyRange;
                juce::Label& keyJump;
                juce::TextEditor& keyJumpBox;
                juce::TextButton& keyJumpGo;
                juce::TextButton& notesDown;
                juce::TextButton& notesUp;
    
                juce::TextButton& beat4;
                juce::TextButton& beat8;
                juce::TextButton& beat16;
                juce::TextButton& beat32;
                juce::TextButton& beat64;
                juce::TextButton& previous;
                juce::TextButton& next;
                juce::TextButton& jumpPage;
                juce::TextButton& copyNote;
                juce::TextButton& pasteNote;
    
                                juce::TextButton& undoNote;
                juce::TextButton& redoNote;

                                juce::TextButton& clearSelection;
                
                int headerTrackIndex = -1;
                juce::String headerTrackName;
                juce::String headerInstrumentText;
                juce::Label trackColourSwatch;
                juce::Label trackNameLabel;
                juce::Label headerInstrumentLabel;
juce::Label helpLabel;
                juce::Label bpmLabel;
                juce::Label sigLabel;
                juce::Label beatWindowLabel;
                juce::Label noteLengthLabel;
                juce::Label velocityLabel;
                juce::Label snapLabel;
                juce::Label libraryLabel;
                juce::Label instrumentLabel;
                juce::Label pageInputLabel;
    
                juce::TextButton applySettingsButton;
                juce::TextButton applyButton;
                juce::TextButton previewButton;
    
                std::function<void()> onApplySettings;
                std::function<void()> onApply;
                std::function<void()> onPreview;
                std::function<void()> onChangeLibrary;
                mw::gui::HelperTooltipLookAndFeel helperTooltipLookAndFeel;
                std::unique_ptr<juce::TooltipWindow> helperTooltipWindow;
            };
    
    }


    double trackEndBeatForPreview(const mw::core::Track& track);
    double tracksEndBeatForPreview(const std::vector<mw::core::Track>& tracks);
    double audioClipsEndBeatForPreview(const std::vector<mw::core::AudioClip>& clips, double tempoBpm);
    juce::String formatPreviewClockTime(double seconds);
    std::optional<double> parsePreviewClockTime(const juce::String& text);

    class PianoRollPreviewPlayerContent final : public juce::Component,
                                                private juce::Timer
    {
    public:
        PianoRollPreviewPlayerContent(
            mw::gui::PianoRollComponent& rollComponent,
            std::function<void()> previewCallback,
            std::function<void()> playPauseCallback,
            std::function<void()> stopCallback,
            std::function<void()> closeCallback,
            std::function<std::vector<mw::core::NoteEvent>()> previewNotesCallback,
            std::function<bool()> hasPreviewCallback,
            std::function<bool()> isRenderingCallback,
            std::function<double()> currentSecondsCallback,
            std::function<double()> totalSecondsCallback,
            std::function<double()> totalBeatsCallback,
            std::function<bool(double)> seekSecondsCallback
        )
            : roll(rollComponent),
              onPreview(std::move(previewCallback)),
              onPlayPause(std::move(playPauseCallback)),
              onStop(std::move(stopCallback)),
              onClose(std::move(closeCallback)),
              getPreviewNotes(std::move(previewNotesCallback)),
              hasPreview(std::move(hasPreviewCallback)),
              isRendering(std::move(isRenderingCallback)),
              getCurrentSeconds(std::move(currentSecondsCallback)),
              getTotalSeconds(std::move(totalSecondsCallback)),
              getTotalBeats(std::move(totalBeatsCallback)),
              seekSeconds(std::move(seekSecondsCallback))
        {
            title.setText("Piano Roll Preview Player", juce::dontSendNotification);
            title.setJustificationType(juce::Justification::centredLeft);
            title.setColour(juce::Label::textColourId, juce::Colours::white);

            previewButton.setButtonText("Preview");
            playPauseButton.setButtonText("▶ Play");
            stopButton.setButtonText("Stop");
            playPauseButton.setEnabled(false);
            stopButton.setEnabled(false);

            help.setText("Preview renders fresh and auto-plays. Drag the position bar or enter a time, then Apply Time to jump.", juce::dontSendNotification);
            help.setJustificationType(juce::Justification::centredLeft);
            help.setColour(juce::Label::textColourId, juce::Colours::lightgrey);

            statusLabel.setText("Status: Ready", juce::dontSendNotification);
            statusLabel.setJustificationType(juce::Justification::centredLeft);
            statusLabel.setColour(juce::Label::textColourId, juce::Colours::lightgreen);

            beatLabel.setText("Beat: --", juce::dontSendNotification);
            beatLabel.setJustificationType(juce::Justification::centredLeft);
            beatLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);

            timeLabel.setText("Time:", juce::dontSendNotification);
            timeLabel.setJustificationType(juce::Justification::centredLeft);
            timeLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);

            timeEditor.setText("00:00.000", juce::dontSendNotification);
            timeEditor.setSelectAllWhenFocused(true);
            timeEditor.setInputRestrictions(16, "0123456789:.");
            timeEditor.setTooltip("Enter seconds, mm:ss.mmm, or h:mm:ss.mmm, then click Apply Time.");
            timeEditor.onReturnKey = [this] { applyTimeFromField(); };
            timeEditor.onEscapeKey = [this]
            {
                timeEditor.setText(formatPreviewClockTime(getSafeCurrentSeconds()), juce::dontSendNotification);
                timeEditor.giveAwayKeyboardFocus();
            };

            applyTimeButton.setButtonText("Apply Time");
            applyTimeButton.setTooltip("Validate the entered time and move the preview play position.");
            applyTimeButton.onClick = [this] { applyTimeFromField(); };

            totalTimeLabel.setText("/ 00:00.000", juce::dontSendNotification);
            totalTimeLabel.setJustificationType(juce::Justification::centredLeft);
            totalTimeLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);

            positionSlider.setSliderStyle(juce::Slider::LinearHorizontal);
            positionSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
            positionSlider.setRange(0.0, 1.0, 0.001);
            positionSlider.setValue(0.0, juce::dontSendNotification);
            positionSlider.setTooltip("Drag or click to jump to a preview position.");
            positionSlider.onDragStart = [this]
            {
                sliderIsDragging = true;
            };
            positionSlider.onValueChange = [this]
            {
                if (sliderIsDragging)
                    timeEditor.setText(formatPreviewClockTime(positionSlider.getValue()), juce::dontSendNotification);
            };
            positionSlider.onDragEnd = [this]
            {
                sliderIsDragging = false;
                applyTimeSeconds(positionSlider.getValue());
            };

            addAndMakeVisible(title);
            addAndMakeVisible(help);
            addAndMakeVisible(previewButton);
            addAndMakeVisible(playPauseButton);
            addAndMakeVisible(stopButton);
            addAndMakeVisible(statusLabel);
            addAndMakeVisible(beatLabel);
            addAndMakeVisible(timeLabel);
            addAndMakeVisible(timeEditor);
            addAndMakeVisible(applyTimeButton);
            addAndMakeVisible(totalTimeLabel);
            addAndMakeVisible(positionSlider);

            previewButton.onClick = [this]
            {
                if (isRendering && isRendering())
                    return;

                if (onPreview)
                    onPreview();
            };

            playPauseButton.onClick = [this]
            {
                if (onPlayPause)
                    onPlayPause();
            };

            stopButton.onClick = [this]
            {
                if (onStop)
                    onStop();
            };


            startTimerHz(30);
        }

        ~PianoRollPreviewPlayerContent() override
        {
            stopTimer();
        }

        void resized() override
        {
            auto area = getLocalBounds().reduced(10);

            auto toolbar = area.removeFromTop(34);
            stopButton.setBounds(toolbar.removeFromRight(70).reduced(4, 2));
            playPauseButton.setBounds(toolbar.removeFromRight(95).reduced(4, 2));
            previewButton.setBounds(toolbar.removeFromRight(95).reduced(4, 2));
            title.setBounds(toolbar.reduced(4, 2));

            help.setBounds(area.removeFromTop(24).reduced(4, 2));

            auto timeRow = area.removeFromTop(34);
            timeLabel.setBounds(timeRow.removeFromLeft(52).reduced(4, 4));
            timeEditor.setBounds(timeRow.removeFromLeft(122).reduced(4, 4));
            applyTimeButton.setBounds(timeRow.removeFromLeft(118).reduced(4, 3));
            totalTimeLabel.setBounds(timeRow.reduced(4, 4));

            positionSlider.setBounds(area.removeFromTop(30).reduced(4, 4));

            auto statusRow = area.removeFromTop(26);
            statusLabel.setBounds(statusRow.removeFromLeft(260).reduced(4, 2));
            beatLabel.setBounds(statusRow.reduced(4, 2));

            visualizerBounds = area.reduced(4, 8).toFloat();
        }

        void paint(juce::Graphics& g) override
        {
            g.fillAll(juce::Colour(0xff151515));

            auto area = visualizerBounds;

            if (area.getWidth() <= 8.0f || area.getHeight() <= 8.0f)
                return;

            g.setColour(juce::Colour(0xff202124));
            g.fillRoundedRectangle(area, 8.0f);
            g.setColour(juce::Colour(0xff6a6a6a));
            g.drawRoundedRectangle(area, 8.0f, 1.5f);

            auto inner = area.reduced(10.0f, 10.0f);

            const auto previewTotalBeats = getSafeTotalBeats();
            const bool hasRenderedPreviewScope = previewTotalBeats > 0.0;

            auto notes = getPreviewNotes ? getPreviewNotes() : std::vector<mw::core::NoteEvent>{};
            if (notes.empty() && !hasRenderedPreviewScope)
                notes = roll.getNotes();

            int maxBeat = hasRenderedPreviewScope
                ? static_cast<int>(std::ceil(previewTotalBeats))
                : roll.getStartBeat() + roll.getBeatsVisible();

            if (!hasRenderedPreviewScope)
            {
                for (const auto& note : notes)
                {
                    const int endBeat = static_cast<int>(
                        std::ceil(
                            static_cast<double>(note.startTick + note.durationTicks)
                            / static_cast<double>(mw::core::Project::ticksPerQuarterNote)
                        )
                    );

                    maxBeat = std::max(maxBeat, endBeat);
                }
            }

            // The preview player can render a full track, sequence, or project.
            // When a rendered preview exists, make that preview duration the map
            // scope instead of letting the currently visible Piano Roll window
            // keep the note map stretched too wide or too narrow.
            maxBeat = std::max(maxBeat, 1);

            auto beatToX = [inner, maxBeat](double beat)
            {
                return inner.getX()
                    + static_cast<float>(std::clamp(beat / static_cast<double>(maxBeat), 0.0, 1.0))
                    * inner.getWidth();
            };

            auto pitchToY = [inner](int pitch)
            {
                const double normalised = static_cast<double>(std::clamp(pitch, 0, 127)) / 127.0;
                return inner.getBottom() - static_cast<float>(normalised) * inner.getHeight();
            };

            const int markerStep = maxBeat <= 32 ? 4 : (maxBeat <= 128 ? 16 : 32);

            for (int beat = 0; beat <= maxBeat; beat += markerStep)
            {
                const float x = beatToX(static_cast<double>(beat));
                g.setColour(juce::Colour(0xff2d2d2d));
                g.drawVerticalLine(static_cast<int>(x), inner.getY(), inner.getBottom());
            }

            const auto previewTotalBeatsForBar = getSafeTotalBeats();
            if (previewTotalBeatsForBar > 0.0)
            {
                const float right = beatToX(previewTotalBeatsForBar);
                auto previewBar = juce::Rectangle<float>(
                    inner.getX(),
                    inner.getBottom() - 12.0f,
                    std::max(3.0f, right - inner.getX()),
                    7.0f
                );

                g.setColour(juce::Colour(0xff9fd0ff).withAlpha(0.32f));
                g.fillRoundedRectangle(previewBar, 2.0f);
                g.setColour(juce::Colour(0xff9fd0ff).withAlpha(0.72f));
                g.drawRoundedRectangle(previewBar, 2.0f, 1.0f);
            }

            for (const auto& note : notes)
            {
                const double startBeat =
                    static_cast<double>(note.startTick)
                    / static_cast<double>(mw::core::Project::ticksPerQuarterNote);

                const double durationBeats =
                    std::max(
                        0.1,
                        static_cast<double>(note.durationTicks)
                        / static_cast<double>(mw::core::Project::ticksPerQuarterNote)
                    );

                const float x = beatToX(startBeat);
                const float right = beatToX(startBeat + durationBeats);
                const float y = pitchToY(note.pitch);

                juce::Rectangle<float> r(
                    x,
                    y - 3.0f,
                    std::max(3.0f, right - x),
                    6.0f
                );

                const bool isPlayingNote =
                    roll.isPreviewPlayheadActive()
                    && roll.getPreviewPlayheadBeat() >= startBeat
                    && roll.getPreviewPlayheadBeat() < startBeat + durationBeats;

                if (isPlayingNote)
                    g.setColour(juce::Colour(0xffffd23f));
                else if (roll.isPreviewPlayheadActive())
                    g.setColour(juce::Colour(0x667fd6ff));
                else
                    g.setColour(juce::Colour(0xff7fd6ff));

                g.fillRoundedRectangle(r, isPlayingNote ? 3.0f : 2.0f);

                if (isPlayingNote)
                {
                    g.setColour(juce::Colour(0xffffffff));
                    g.drawRoundedRectangle(r.expanded(1.0f), 3.0f, 1.5f);
                }
            }

            if (roll.isPreviewPlayheadActive())
            {
                const float x = beatToX(roll.getPreviewPlayheadBeat());

                g.setColour(juce::Colour(0xffffe066).withAlpha(0.25f));
                g.fillRect(x - 5.0f, inner.getY(), 10.0f, inner.getHeight());

                g.setColour(juce::Colour(0xffffe066));
                g.drawLine(x, inner.getY(), x, inner.getBottom(), 4.0f);
            }

            g.setColour(juce::Colour(0xffbdbdbd));
            auto mapTitle = juce::String("Preview Note Map");
            if (hasRenderedPreviewScope)
                mapTitle << "  |  " << juce::String(maxBeat) << " beats";
            g.drawText(mapTitle, inner.toNearestInt().reduced(6), juce::Justification::topLeft, false);
        }

    private:
        double getSafeTotalBeats() const
        {
            if (getTotalBeats)
                return std::max(0.0, getTotalBeats());

            return 0.0;
        }

        double getSafeTotalSeconds() const
        {
            return getTotalSeconds ? std::max(0.0, getTotalSeconds()) : 0.0;
        }

        double getSafeCurrentSeconds() const
        {
            const auto total = getSafeTotalSeconds();
            const auto current = getCurrentSeconds ? std::max(0.0, getCurrentSeconds()) : 0.0;

            if (total > 0.0)
                return std::clamp(current, 0.0, total);

            return current;
        }

        void applyTimeFromField()
        {
            const auto parsed = parsePreviewClockTime(timeEditor.getText());

            if (!parsed)
            {
                showTimeValidationError("Invalid time. Use seconds, mm:ss.mmm, or h:mm:ss.mmm.");
                return;
            }

            applyTimeSeconds(*parsed);
        }

        void applyTimeSeconds(double seconds)
        {
            const bool previewAvailable = hasPreview && hasPreview();
            const double total = getSafeTotalSeconds();

            if (!previewAvailable || total <= 0.0)
            {
                showTimeValidationError("No preview is loaded yet.");
                return;
            }

            if (seconds < 0.0 || seconds > total)
            {
                showTimeValidationError("Time is outside the preview length.");
                return;
            }

            const bool ok = seekSeconds && seekSeconds(seconds);

            if (!ok)
            {
                showTimeValidationError("Could not move preview to that time.");
                return;
            }

            timeEditor.setText(formatPreviewClockTime(seconds), juce::dontSendNotification);
            positionSlider.setValue(seconds, juce::dontSendNotification);
            statusLabel.setText("Status: Jumped to " + formatPreviewClockTime(seconds), juce::dontSendNotification);
            statusLabel.setColour(juce::Label::textColourId, juce::Colours::lightgreen);
            timeEditor.giveAwayKeyboardFocus();
        }

        void showTimeValidationError(const juce::String& message)
        {
            statusLabel.setText("Status: " + message, juce::dontSendNotification);
            statusLabel.setColour(juce::Label::textColourId, juce::Colours::red);
        }

        void timerCallback() override
        {
            const bool renderingNow = isRendering && isRendering();
            const bool previewAvailable = hasPreview && hasPreview();
            const double totalSeconds = getSafeTotalSeconds();
            const double currentSeconds = getSafeCurrentSeconds();

            previewButton.setEnabled(!renderingNow);
            playPauseButton.setEnabled(!renderingNow && (previewAvailable || roll.isPreviewPlayheadActive()));
            stopButton.setEnabled(roll.isPreviewPlayheadActive());
            applyTimeButton.setEnabled(!renderingNow && previewAvailable && totalSeconds > 0.0);
            timeEditor.setEnabled(!renderingNow && previewAvailable && totalSeconds > 0.0);
            positionSlider.setEnabled(!renderingNow && previewAvailable && totalSeconds > 0.0);
            positionSlider.setRange(0.0, std::max(0.001, totalSeconds), 0.001);
            totalTimeLabel.setText("/ " + formatPreviewClockTime(totalSeconds), juce::dontSendNotification);

            if (!timeEditor.hasKeyboardFocus(true) && !sliderIsDragging)
                timeEditor.setText(formatPreviewClockTime(currentSeconds), juce::dontSendNotification);

            if (!sliderIsDragging)
                positionSlider.setValue(currentSeconds, juce::dontSendNotification);

            if (renderingNow)
            {
                statusLabel.setText("Status: Rendering", juce::dontSendNotification);
                statusLabel.setColour(juce::Label::textColourId, juce::Colours::orange);
                playPauseButton.setButtonText("▶ Play");
                beatLabel.setText("Beat: --", juce::dontSendNotification);
            }
            else if (roll.isPreviewPlayheadActive())
            {
                if (roll.isPreviewPlayheadPaused())
                {
                    statusLabel.setText("Status: Paused", juce::dontSendNotification);
                    statusLabel.setColour(juce::Label::textColourId, juce::Colours::orange);
                    playPauseButton.setButtonText("▶ Resume");
                }
                else
                {
                    statusLabel.setText("Status: Playing", juce::dontSendNotification);
                    statusLabel.setColour(juce::Label::textColourId, juce::Colours::lightgreen);
                    playPauseButton.setButtonText("⏸ Pause");
                }

                beatLabel.setText("Beat: " + juce::String(roll.getPreviewPlayheadBeat() + 1.0, 2), juce::dontSendNotification);
            }
            else if (previewAvailable)
            {
                statusLabel.setText("Status: Stopped", juce::dontSendNotification);
                statusLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
                playPauseButton.setButtonText("▶ Play");
                beatLabel.setText("Beat: --", juce::dontSendNotification);
            }
            else
            {
                statusLabel.setText("Status: Ready", juce::dontSendNotification);
                statusLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
                playPauseButton.setButtonText("▶ Play");
                beatLabel.setText("Beat: --", juce::dontSendNotification);
            }

            repaint();
        }

        mw::gui::PianoRollComponent& roll;
        juce::Label title;
        juce::Label help;
        juce::TextButton previewButton;
        juce::TextButton playPauseButton;
        juce::TextButton stopButton;
        juce::TextButton closeButton;
        juce::Label statusLabel;
        juce::Label beatLabel;
        juce::Label timeLabel;
        juce::TextEditor timeEditor;
        juce::TextButton applyTimeButton;
        juce::Label totalTimeLabel;
        juce::Slider positionSlider;
        juce::Rectangle<float> visualizerBounds;
        bool sliderIsDragging = false;
        std::function<void()> onPreview;
        std::function<void()> onPlayPause;
        std::function<void()> onStop;
        std::function<void()> onClose;
        std::function<std::vector<mw::core::NoteEvent>()> getPreviewNotes;
        std::function<bool()> hasPreview;
        std::function<bool()> isRendering;
        std::function<double()> getCurrentSeconds;
        std::function<double()> getTotalSeconds;
        std::function<double()> getTotalBeats;
        std::function<bool(double)> seekSeconds;
    };




    struct ThemePreset
    {
        int id = 1;
        juce::String name = "Default";
        std::filesystem::path path;
        juce::Colour background = juce::Colour(0xff202124);
        juce::Colour panel = juce::Colour(0xff2c2f33);
        juce::Colour control = juce::Colour(0xff303236);
        juce::Colour text = juce::Colours::lightgrey;
        juce::Colour accent = juce::Colour(0xffffd23f);
    };

    juce::String colourToHex(juce::Colour colour)
    {
        return juce::String::toHexString(static_cast<int>(colour.getARGB())).paddedLeft('0', 8);
    }

    juce::Colour colourFromHex(const juce::String& value, juce::Colour fallback)
    {
        auto text = value.trim();

        if (text.startsWithChar('#'))
            text = text.substring(1);

        if (text.startsWithIgnoreCase("0x"))
            text = text.substring(2);

        if (text.length() == 6)
            text = "ff" + text;

        if (text.length() != 8)
            return fallback;

        juce::uint32 parsed = 0;

        for (int i = 0; i < text.length(); ++i)
        {
            const auto c = text[i];
            int nibble = -1;

            if (c >= '0' && c <= '9') nibble = c - '0';
            else if (c >= 'a' && c <= 'f') nibble = 10 + (c - 'a');
            else if (c >= 'A' && c <= 'F') nibble = 10 + (c - 'A');

            if (nibble < 0)
                return fallback;

            parsed = (parsed << 4) | static_cast<juce::uint32>(nibble);
        }

        return juce::Colour(parsed);
    }

    ThemePreset makeBuiltInTheme(int id)
    {
        switch (id)
        {
            case 2:
                return { 2, "Studio Dark", {}, juce::Colour(0xff121416), juce::Colour(0xff1d2024), juce::Colour(0xff2a2e34), juce::Colour(0xffe0e0e0), juce::Colour(0xff6ec6ff) };

            case 3:
                return { 3, "Warm Graphite", {}, juce::Colour(0xff191715), juce::Colour(0xff28231f), juce::Colour(0xff332d27), juce::Colour(0xffeadfce), juce::Colour(0xffffb55a) };

            case 4:
                return { 4, "Midnight Blue", {}, juce::Colour(0xff101722), juce::Colour(0xff172234), juce::Colour(0xff22304a), juce::Colour(0xffd8e6ff), juce::Colour(0xff7fd6ff) };

            case 5:
                return { 5, "Forest Slate", {}, juce::Colour(0xff101713), juce::Colour(0xff1a241d), juce::Colour(0xff25302a), juce::Colour(0xffdce8dc), juce::Colour(0xff79d98b) };

            case 6:
                return { 6, "Plum Night", {}, juce::Colour(0xff17111d), juce::Colour(0xff241a2d), juce::Colour(0xff30233b), juce::Colour(0xffeadfff), juce::Colour(0xffc792ff) };

            case 7:
                return { 7, "High Contrast", {}, juce::Colour(0xff080808), juce::Colour(0xff151515), juce::Colour(0xff242424), juce::Colour(0xffffffff), juce::Colour(0xffffd23f) };

            case 1:
            default:
                return {};
        }
    }

    void writeThemeFileIfMissing(const ThemePreset& preset)
    {
        juce::String fileName;
        fileName << juce::String(preset.id).paddedLeft('0', 3) << "-" << preset.name.toLowerCase() << ".xml";
        fileName = fileName.replaceCharacter(' ', '_');

        const auto filePath = mw::app::AppPaths::themesFolder() / fileName.toStdString();

        if (std::filesystem::exists(filePath))
            return;

        juce::XmlElement xml("PoorMansStudioTheme");
        xml.setAttribute("name", preset.name);
        xml.setAttribute("version", 1);
        xml.setAttribute("background", colourToHex(preset.background));
        xml.setAttribute("panel", colourToHex(preset.panel));
        xml.setAttribute("control", colourToHex(preset.control));
        xml.setAttribute("text", colourToHex(preset.text));
        xml.setAttribute("accent", colourToHex(preset.accent));
        xml.writeTo(juce::File(filePath.string()));
    }

    void ensureDefaultThemeFiles()
    {
        std::error_code ignored;
        std::filesystem::create_directories(mw::app::AppPaths::themesFolder(), ignored);

        for (int id = 1; id <= 7; ++id)
            writeThemeFileIfMissing(makeBuiltInTheme(id));
    }

    ThemePreset readThemePresetFromXml(const std::filesystem::path& path, int id)
    {
        auto preset = makeBuiltInTheme(id);
        preset.id = id;
        preset.path = path;

        juce::XmlDocument doc(juce::File(path.string()));
        std::unique_ptr<juce::XmlElement> xml(doc.getDocumentElement());

        if (xml == nullptr || !xml->hasTagName("PoorMansStudioTheme"))
            return preset;

        preset.name = xml->getStringAttribute("name", juce::File(path.string()).getFileNameWithoutExtension());
        preset.background = colourFromHex(xml->getStringAttribute("background"), preset.background);
        preset.panel = colourFromHex(xml->getStringAttribute("panel"), preset.panel);
        preset.control = colourFromHex(xml->getStringAttribute("control"), preset.control);
        preset.text = colourFromHex(xml->getStringAttribute("text"), preset.text);
        preset.accent = colourFromHex(xml->getStringAttribute("accent"), preset.accent);

        return preset;
    }

    juce::String themeDisplayName(const juce::String& name)
    {
        constexpr int maxThemeDisplayNameLength = 32;
        const auto trimmed = name.trim();

        if (trimmed.length() <= maxThemeDisplayNameLength)
            return trimmed;

        return trimmed.substring(0, maxThemeDisplayNameLength - 3) + "...";
    }

    ThemePreset getThemePresetFromFileList(const std::vector<std::filesystem::path>& files, int presetId)
    {
        if (presetId <= 0 || presetId > static_cast<int>(files.size()))
            presetId = 1;

        if (presetId > 0 && presetId <= static_cast<int>(files.size()))
            return readThemePresetFromXml(files[static_cast<std::size_t>(presetId - 1)], presetId);

        return makeBuiltInTheme(1);
    }


    double trackEndBeatForPreview(const mw::core::Track& track)
    {
        std::int64_t endTick = 0;

        for (const auto& note : track.getNotes())
            endTick = std::max(endTick, note.startTick + note.durationTicks);

        return std::max(
            1.0,
            static_cast<double>(endTick) / static_cast<double>(mw::core::Project::ticksPerQuarterNote)
        );
    }

    double tracksEndBeatForPreview(const std::vector<mw::core::Track>& tracks)
    {
        double endBeat = 1.0;

        for (const auto& track : tracks)
            endBeat = std::max(endBeat, trackEndBeatForPreview(track));

        return std::max(1.0, endBeat);
    }

    struct SequenceTrimResult
    {
        bool applied = false;
        int totalNotes = 0;
        int keptNotes = 0;
    };

    SequenceTrimResult trimTracksToTickRangeAndRebase(
        std::vector<mw::core::Track>& tracks,
        std::int64_t rangeStartTick,
        std::int64_t rangeEndTick)
    {
        SequenceTrimResult result;

        if (rangeEndTick <= rangeStartTick)
            return result;

        for (const auto& track : tracks)
        {
            for (const auto& note : track.getNotes())
            {
                ++result.totalNotes;
                const auto noteStart = note.startTick;
                const auto noteEnd = note.startTick + note.durationTicks;

                if (noteEnd > rangeStartTick && noteStart < rangeEndTick)
                    ++result.keptNotes;
            }
        }

        // Keep older/incomplete projects safe: if the saved sequence bounds do
        // not overlap these notes, leave the render snapshot unchanged instead
        // of producing a silent preview.
        if (result.totalNotes <= 0 || result.keptNotes <= 0)
            return result;

        for (auto& track : tracks)
        {
            std::vector<mw::core::NoteEvent> clippedNotes;
            clippedNotes.reserve(track.getNotes().size());

            for (const auto& note : track.getNotes())
            {
                const auto noteStart = note.startTick;
                const auto noteEnd = note.startTick + note.durationTicks;

                if (noteEnd <= rangeStartTick || noteStart >= rangeEndTick)
                    continue;

                const auto clippedStart = std::max(noteStart, rangeStartTick);
                const auto clippedEnd = std::min(noteEnd, rangeEndTick);

                if (clippedEnd <= clippedStart)
                    continue;

                auto clipped = note;
                clipped.startTick = clippedStart - rangeStartTick;
                clipped.durationTicks = std::max<std::int64_t>(1, clippedEnd - clippedStart);
                clippedNotes.push_back(clipped);
            }

            track.getNotes() = std::move(clippedNotes);
        }

        result.applied = true;
        return result;
    }

    std::int64_t rebaseSingleTrackPreviewToFirstContent(
        mw::core::Track& track,
        std::vector<mw::core::AudioClip>& clips)
    {
        bool foundContent = false;
        std::int64_t earliestTick = 0;

        auto considerStartTick = [&](std::int64_t tick)
        {
            tick = std::max<std::int64_t>(0, tick);

            if (!foundContent || tick < earliestTick)
            {
                earliestTick = tick;
                foundContent = true;
            }
        };

        for (const auto& note : track.getNotes())
            considerStartTick(note.startTick);

        for (const auto& clip : clips)
            considerStartTick(clip.startTick);

        if (!foundContent || earliestTick <= 0)
            return 0;

        for (auto& note : track.getNotes())
            note.startTick = std::max<std::int64_t>(0, note.startTick - earliestTick);

        for (auto& clip : clips)
            clip.startTick = std::max<std::int64_t>(0, clip.startTick - earliestTick);

        return earliestTick;
    }

    double audioClipsEndBeatForPreview(const std::vector<mw::core::AudioClip>& clips, double tempoBpm)
    {
        double endBeat = 1.0;
        const auto safeTempo = tempoBpm > 0.0 ? tempoBpm : 120.0;

        for (const auto& clip : clips)
        {
            const auto clipEndTick = audioClipEndTickForTempo(clip, safeTempo);
            endBeat = std::max(
                endBeat,
                static_cast<double>(clipEndTick) / static_cast<double>(mw::core::Project::ticksPerQuarterNote)
            );
        }

        return std::max(1.0, endBeat);
    }

    juce::String formatPreviewClockTime(double seconds)
    {
        seconds = std::max(0.0, seconds);

        const auto wholeMilliseconds = static_cast<long long>(std::llround(seconds * 1000.0));
        const auto totalSeconds = wholeMilliseconds / 1000;
        const auto milliseconds = wholeMilliseconds % 1000;
        const auto hours = totalSeconds / 3600;
        const auto minutes = (totalSeconds / 60) % 60;
        const auto secs = totalSeconds % 60;

        if (hours > 0)
        {
            return juce::String(hours)
                + ":"
                + juce::String(static_cast<int>(minutes)).paddedLeft('0', 2)
                + ":"
                + juce::String(static_cast<int>(secs)).paddedLeft('0', 2)
                + "."
                + juce::String(static_cast<int>(milliseconds)).paddedLeft('0', 3);
        }

        return juce::String(static_cast<int>(minutes)).paddedLeft('0', 2)
            + ":"
            + juce::String(static_cast<int>(secs)).paddedLeft('0', 2)
            + "."
            + juce::String(static_cast<int>(milliseconds)).paddedLeft('0', 3);
    }

    std::optional<double> parsePreviewClockTime(const juce::String& text)
    {
        const auto trimmed = text.trim();

        if (trimmed.isEmpty())
            return std::nullopt;

        const auto raw = trimmed.toStdString();

        for (const auto ch : raw)
        {
            if (!std::isdigit(static_cast<unsigned char>(ch)) && ch != ':' && ch != '.')
                return std::nullopt;
        }

        juce::StringArray parts;
        parts.addTokens(trimmed, ":", "");
        parts.removeEmptyStrings();

        if (parts.size() < 1 || parts.size() > 3)
            return std::nullopt;

        auto parseWhole = [](const juce::String& value) -> std::optional<int>
        {
            if (value.isEmpty())
                return std::nullopt;

            const auto rawValue = value.toStdString();

            for (const auto ch : rawValue)
            {
                if (!std::isdigit(static_cast<unsigned char>(ch)))
                    return std::nullopt;
            }

            return value.getIntValue();
        };

        double seconds = 0.0;

        if (parts.size() == 1)
        {
            seconds = parts[0].getDoubleValue();
        }
        else if (parts.size() == 2)
        {
            const auto minutes = parseWhole(parts[0]);
            const auto secs = parts[1].getDoubleValue();

            if (!minutes || secs < 0.0 || secs >= 60.0)
                return std::nullopt;

            seconds = static_cast<double>(*minutes * 60) + secs;
        }
        else
        {
            const auto hours = parseWhole(parts[0]);
            const auto minutes = parseWhole(parts[1]);
            const auto secs = parts[2].getDoubleValue();

            if (!hours || !minutes || *minutes < 0 || *minutes >= 60 || secs < 0.0 || secs >= 60.0)
                return std::nullopt;

            seconds = static_cast<double>(*hours * 3600 + *minutes * 60) + secs;
        }

        if (!std::isfinite(seconds) || seconds < 0.0)
            return std::nullopt;

        return seconds;
    }

namespace mw::gui
{

    namespace
    {
        class VstProjectDefaultDragonWarningContent final : public juce::Component
        {
        public:
            explicit VstProjectDefaultDragonWarningContent(std::function<void()> onOkIn)
                : onOk(std::move(onOkIn))
            {
                dragonImage = juce::ImageFileFormat::loadFrom(
                    BinaryData::vst_warning_dragon_png,
                    BinaryData::vst_warning_dragon_pngSize);

                addAndMakeVisible(okButton);
                okButton.setButtonText("OK");
                okButton.setWantsKeyboardFocus(true);
                okButton.onClick = [this]
                {
                    if (onOk)
                        onOk();

                    if (auto* window = findParentComponentOfClass<juce::DocumentWindow>())
                        window->exitModalState(0);
                };

                setSize(850, 585);
            }

            void paint(juce::Graphics& g) override
            {
                const auto bounds = getLocalBounds().toFloat();

                juce::ColourGradient parchment(
                    juce::Colour(0xfffbecd0), bounds.getTopLeft(),
                    juce::Colour(0xffe3c28b), bounds.getBottomRight(), false);
                g.setGradientFill(parchment);
                g.fillRoundedRectangle(bounds.reduced(2.0f), 16.0f);

                g.setColour(juce::Colour(0xff2c1711));
                g.drawRoundedRectangle(bounds.reduced(2.0f), 16.0f, 2.0f);
                g.setColour(juce::Colour(0xff9f1d1d));
                g.drawRoundedRectangle(bounds.reduced(7.0f), 12.0f, 2.0f);

                for (int i = 0; i < 70; ++i)
                {
                    const auto x = static_cast<float>((i * 97) % juce::jmax(1, getWidth()));
                    const auto y = static_cast<float>((i * 53) % juce::jmax(1, getHeight()));
                    const auto r = 0.7f + static_cast<float>(i % 5) * 0.18f;
                    g.setColour(juce::Colour(0xff8a5a2b).withAlpha(0.09f));
                    g.fillEllipse(x, y, r, r);
                }

                auto titleArea = getLocalBounds().withHeight(78).reduced(28, 10);
                g.setFont(juce::Font(30.0f, juce::Font::bold));
                g.setColour(juce::Colour(0xfff6cf7c));
                g.drawFittedText("DANGER!!! DRAGONS AHEAD!!!", titleArea.translated(2, 2), juce::Justification::centred, 1);
                g.setColour(juce::Colour(0xff990f0f));
                g.drawFittedText("DANGER!!! DRAGONS AHEAD!!!", titleArea, juce::Justification::centred, 1);

                auto imageFrame = juce::Rectangle<float>(34.0f, 92.0f, static_cast<float>(getWidth() - 68), 310.0f);
                g.setColour(juce::Colour(0xff3a1c12).withAlpha(0.28f));
                g.fillRoundedRectangle(imageFrame.translated(0.0f, 4.0f), 12.0f);
                g.setColour(juce::Colour(0xfff7d9a5));
                g.fillRoundedRectangle(imageFrame, 12.0f);
                g.setColour(juce::Colour(0xff6f1d1b));
                g.drawRoundedRectangle(imageFrame, 12.0f, 2.0f);

                auto imageArea = imageFrame.reduced(8.0f).toNearestInt();
                if (dragonImage.isValid())
                {
                    g.setImageResamplingQuality(juce::Graphics::highResamplingQuality);
                    g.drawImageWithin(dragonImage,
                                      imageArea.getX(), imageArea.getY(), imageArea.getWidth(), imageArea.getHeight(),
                                      juce::RectanglePlacement::fillDestination | juce::RectanglePlacement::centred);
                }

                auto textArea = juce::Rectangle<int>(54, 414, getWidth() - 108, 88);
                g.setColour(juce::Colour(0xff17120f));
                g.setFont(juce::Font(18.0f, juce::Font::bold));
                g.drawFittedText("The VST plugin feature is still experimental and could crash the program.\nClick OK to acknowledge and continue.",
                                 textArea,
                                 juce::Justification::centred,
                                 3);
            }

            void resized() override
            {
                okButton.setBounds((getWidth() - 128) / 2, getHeight() - 62, 128, 36);
            }

        private:
            std::function<void()> onOk;
            juce::TextButton okButton;
            juce::Image dragonImage;
        };

        class VstProjectDefaultDragonWarningWindow final : public juce::DocumentWindow
        {
        public:
            VstProjectDefaultDragonWarningWindow(juce::Component* centreAround, std::function<void()> onOk)
                : juce::DocumentWindow({}, juce::Colours::transparentBlack, 0)
            {
                setUsingNativeTitleBar(false);
                setTitleBarButtonsRequired(0, false);
                setTitleBarHeight(0);
                setResizable(false, false);
                setOpaque(false);
                setContentOwned(new VstProjectDefaultDragonWarningContent(std::move(onOk)), true);

                const auto width = getContentComponent() != nullptr ? getContentComponent()->getWidth() : 850;
                const auto height = getContentComponent() != nullptr ? getContentComponent()->getHeight() : 585;

                if (centreAround != nullptr)
                {
                    const auto centre = centreAround->getScreenBounds().getCentre();
                    setBounds(centre.x - width / 2, centre.y - height / 2, width, height);
                }
                else
                {
                    centreWithSize(width, height);
                }

                addToDesktop(juce::ComponentPeer::windowIsTemporary
                             | juce::ComponentPeer::windowHasDropShadow
                             | juce::ComponentPeer::windowAppearsOnTaskbar);
                setAlwaysOnTop(true);
                setVisible(true);
                enterModalState(true, nullptr, true);
            }

            void closeButtonPressed() override
            {
                exitModalState(0);
            }
        };

        bool noteEventsEqual(const mw::core::NoteEvent& a, const mw::core::NoteEvent& b)
        {
            return a.pitch == b.pitch
                && a.velocity == b.velocity
                && a.midiChannel == b.midiChannel
                && a.startTick == b.startTick
                && a.durationTicks == b.durationTicks
                && a.articulation == b.articulation;
        }

        bool noteListsEqual(const std::vector<mw::core::NoteEvent>& a, const std::vector<mw::core::NoteEvent>& b)
        {
            if (a.size() != b.size())
                return false;

            for (std::size_t i = 0; i < a.size(); ++i)
            {
                if (!noteEventsEqual(a[i], b[i]))
                    return false;
            }

            return true;
        }

        bool vstAssignmentsReferToSamePlugin(const mw::core::InstrumentAssignment& a, const mw::core::InstrumentAssignment& b)
        {
            if (a.backendType != mw::core::SampleBackendType::VST3
                || b.backendType != mw::core::SampleBackendType::VST3)
                return false;

            if (!a.vst3.uid.empty() && !b.vst3.uid.empty())
                return a.vst3.uid == b.vst3.uid;

            const auto aPath = resolveVst3BundlePath(a);
            const auto bPath = resolveVst3BundlePath(b);
            return !aPath.empty() && !bPath.empty() && pathsReferToSameLocation(aPath, bPath);
        }

        void stripVstRuntimeState(mw::core::InstrumentAssignment& assignment)
        {
            assignment.vst3.stateBase64.clear();
        }

        mw::core::InstrumentAssignment makeUndoSafeInstrumentAssignment(mw::core::InstrumentAssignment assignment)
        {
            if (assignment.backendType == mw::core::SampleBackendType::VST3)
                stripVstRuntimeState(assignment);
            else
                assignment.vst3 = {};

            return assignment;
        }

        mw::core::InstrumentAssignment prepareInstrumentAssignmentForUndoRestore(const mw::core::InstrumentAssignment& current,
                                                                                 mw::core::InstrumentAssignment restored)
        {
            if (restored.backendType == mw::core::SampleBackendType::VST3)
            {
                stripVstRuntimeState(restored);

                // The undo/redo stacks are deliberately metadata-only for VST3. If
                // the user is undoing/redoing around the same live plugin identity,
                // keep the current applied state instead of reviving an old blob.
                if (vstAssignmentsReferToSamePlugin(current, restored))
                    restored.vst3.stateBase64 = current.vst3.stateBase64;
            }
            else
            {
                restored.vst3 = {};
            }

            return restored;
        }

        void sanitizeProjectForUndoStack(mw::core::Project& project)
        {
            for (auto& track : project.getTracks())
            {
                auto assignment = track.getInstrument();
                assignment = makeUndoSafeInstrumentAssignment(std::move(assignment));
                track.setInstrumentAssignment(std::move(assignment));
            }
        }

        void preserveCurrentVstAppliedStates(mw::core::Project& restoredProject, const mw::core::Project& currentProject)
        {
            auto& restoredTracks = restoredProject.getTracks();
            const auto& currentTracks = currentProject.getTracks();
            const auto count = std::min(restoredTracks.size(), currentTracks.size());

            for (std::size_t i = 0; i < count; ++i)
            {
                auto restoredAssignment = restoredTracks[i].getInstrument();
                const auto& currentAssignment = currentTracks[i].getInstrument();

                if (restoredAssignment.backendType == mw::core::SampleBackendType::VST3)
                {
                    stripVstRuntimeState(restoredAssignment);
                    if (vstAssignmentsReferToSamePlugin(currentAssignment, restoredAssignment))
                        restoredAssignment.vst3.stateBase64 = currentAssignment.vst3.stateBase64;
                    restoredTracks[i].setInstrumentAssignment(std::move(restoredAssignment));
                }
                else
                {
                    restoredAssignment.vst3 = {};
                    restoredTracks[i].setInstrumentAssignment(std::move(restoredAssignment));
                }
            }
        }

        bool projectContainsVst3Track(const mw::core::Project& project)
        {
            for (const auto& track : project.getTracks())
            {
                if (track.getInstrument().backendType == mw::core::SampleBackendType::VST3)
                    return true;
            }

            return false;
        }

        bool instrumentAssignmentsEqual(const mw::core::InstrumentAssignment& a, const mw::core::InstrumentAssignment& b)
        {
            return a.originalImportedName == b.originalImportedName
                && a.normalizedName == b.normalizedName
                && a.displayName == b.displayName
                && a.backendType == b.backendType
                && a.sampleLibraryPath == b.sampleLibraryPath
                && a.sampleLibraryDisplayName == b.sampleLibraryDisplayName
                && a.midiChannel == b.midiChannel
                && a.midiBank == b.midiBank
                && a.midiProgram == b.midiProgram
                && a.presetName == b.presetName
                && a.articulationMap == b.articulationMap
                && a.wasAutoMatched == b.wasAutoMatched
                && a.vst3.bundlePath == b.vst3.bundlePath
                && a.vst3.name == b.vst3.name
                && a.vst3.vendor == b.vst3.vendor
                && a.vst3.uid == b.vst3.uid
                && a.vst3.bypassed == b.vst3.bypassed
                && std::abs(a.matchConfidence - b.matchConfidence) < 0.0001f;
        }

        bool isGenericInstrumentDisplayName(const juce::String& name)
        {
            const auto cleaned = name.trim().toLowerCase();
            return cleaned.isEmpty()
                || cleaned == "default"
                || cleaned == "default instrument"
                || cleaned == "midi instrument"
                || cleaned == "general midi"
                || cleaned == "gm instrument"
                || cleaned.startsWith("midi track");
        }

        juce::String stripBankProgramPrefix(const juce::String& comboText)
        {
            const auto marker = comboText.indexOf(" - ");
            if (marker >= 0)
                return comboText.substring(marker + 3).trim();

            return comboText.trim();
        }

        juce::String getPianoRollHeaderInstrumentText(const mw::core::InstrumentAssignment& assignment,
                                                       bool isPending,
                                                       const juce::String& resolvedInstrumentName = {})
        {
            juce::String instrumentName = resolvedInstrumentName.trim();

            if (instrumentName.isEmpty())
                instrumentName = assignment.displayName;

            if (isGenericInstrumentDisplayName(instrumentName))
                instrumentName = assignment.presetName;

            if (isGenericInstrumentDisplayName(instrumentName) && !assignment.sampleLibraryDisplayName.empty())
                instrumentName = assignment.sampleLibraryDisplayName;

            if (isGenericInstrumentDisplayName(instrumentName) && !assignment.sampleLibraryPath.empty())
                instrumentName = juce::String(assignment.sampleLibraryPath.stem().string());

            if (isGenericInstrumentDisplayName(instrumentName))
                instrumentName = "Default Instrument";

            juce::String label;
            label << (isPending ? "Pending Instrument: " : "Instrument: ");

            if (assignment.backendType != mw::core::SampleBackendType::None)
                label << mw::core::sampleBackendTypeToString(assignment.backendType) << " - ";

            label << instrumentName;
            return label;
        }

        juce::Image createMicrophoneWindowIconImage()
        {
            juce::Image image(juce::Image::ARGB, 32, 32, true);
            juce::Graphics g(image);
            g.fillAll(juce::Colours::transparentBlack);

            auto bounds = juce::Rectangle<float>(3.0f, 2.0f, 26.0f, 28.0f);
            const auto centre = bounds.getCentreX();
            const auto bodyWidth = bounds.getWidth() * 0.34f;
            const auto bodyHeight = bounds.getHeight() * 0.52f;
            const auto body = juce::Rectangle<float>(centre - bodyWidth * 0.5f,
                                                     bounds.getY() + 2.0f,
                                                     bodyWidth,
                                                     bodyHeight);

            g.setColour(juce::Colour(0xff202124));
            g.fillRoundedRectangle(body, bodyWidth * 0.45f);
            g.setColour(juce::Colour(0xff6ec6ff));
            g.drawRoundedRectangle(body, bodyWidth * 0.45f, 1.2f);

            g.setColour(juce::Colours::white.withAlpha(0.42f));
            for (float offset : { -0.18f, 0.0f, 0.18f })
            {
                const float grilleX = centre + bodyWidth * offset;
                g.drawLine(grilleX, body.getY() + 4.0f, grilleX, body.getBottom() - 4.0f, 0.7f);
            }

            g.setColour(juce::Colour(0xff202124));

            const auto arc = juce::Rectangle<float>(centre - bodyWidth * 0.85f,
                                                    body.getCentreY() - bodyHeight * 0.18f,
                                                    bodyWidth * 1.7f,
                                                    bodyHeight * 0.86f);
            juce::Path arcPath;
            arcPath.addCentredArc(arc.getCentreX(),
                                   arc.getCentreY(),
                                   arc.getWidth() * 0.5f,
                                   arc.getHeight() * 0.5f,
                                   0.0f,
                                   juce::MathConstants<float>::pi * 0.08f,
                                   juce::MathConstants<float>::pi * 0.92f,
                                   true);
            g.strokePath(arcPath, juce::PathStrokeType(1.8f));

            const auto stemTop = body.getBottom() + 2.0f;
            const auto stemBottom = bounds.getBottom() - 4.0f;
            g.drawLine(centre, stemTop, centre, stemBottom, 1.8f);
            g.drawLine(centre - bodyWidth * 0.55f, stemBottom, centre + bodyWidth * 0.55f, stemBottom, 1.8f);
            return image;
        }


        class AudioRecorderWindowContent final : public juce::Component,
                                                 private juce::Timer
        {
        private:
            class MicrophoneIcon final : public juce::Component
            {
            public:
                void paint(juce::Graphics& g) override
                {
                    const auto bounds = getLocalBounds().toFloat().reduced(3.0f);
                    const auto centre = bounds.getCentreX();
                    const auto bodyWidth = bounds.getWidth() * 0.38f;
                    const auto bodyHeight = bounds.getHeight() * 0.56f;
                    const auto body = juce::Rectangle<float>(centre - bodyWidth * 0.5f,
                                                             bounds.getY() + 2.0f,
                                                             bodyWidth,
                                                             bodyHeight);

                    g.setColour(juce::Colours::white.withAlpha(0.92f));
                    g.fillRoundedRectangle(body, bodyWidth * 0.45f);
                    g.setColour(juce::Colours::white.withAlpha(0.72f));
                    g.drawRoundedRectangle(body, bodyWidth * 0.45f, 1.4f);

                    const auto arc = juce::Rectangle<float>(centre - bodyWidth * 0.85f,
                                                            body.getCentreY() - bodyHeight * 0.18f,
                                                            bodyWidth * 1.7f,
                                                            bodyHeight * 0.86f);
                    juce::Path arcPath;
                    arcPath.addCentredArc(arc.getCentreX(),
                                           arc.getCentreY(),
                                           arc.getWidth() * 0.5f,
                                           arc.getHeight() * 0.5f,
                                           0.0f,
                                           juce::MathConstants<float>::pi * 0.08f,
                                           juce::MathConstants<float>::pi * 0.92f,
                                           true);
                    g.strokePath(arcPath, juce::PathStrokeType(2.0f));

                    const auto stemTop = body.getBottom() + 2.0f;
                    const auto stemBottom = bounds.getBottom() - 6.0f;
                    g.drawLine(centre, stemTop, centre, stemBottom, 2.0f);
                    g.drawLine(centre - bodyWidth * 0.55f, stemBottom, centre + bodyWidth * 0.55f, stemBottom, 2.0f);
                }
            };

            class CountdownOverlay final : public juce::Component
            {
            public:
                void setCountdownText(const juce::String& newText)
                {
                    text = newText;
                    repaint();
                }

                void paint(juce::Graphics& g) override
                {
                    g.setColour(juce::Colours::black.withAlpha(0.68f));
                    g.fillAll();
                    g.setColour(juce::Colours::white);
                    g.setFont(juce::FontOptions(56.0f, juce::Font::bold));
                    g.drawFittedText(text, getLocalBounds().reduced(12), juce::Justification::centred, 1);
                }

            private:
                juce::String text;
            };

        public:
            AudioRecorderWindowContent(std::function<void()> startCallback,
                                       std::function<void()> testRecordCallback,
                                       std::function<void(double)> micGainChangedCallback,
                                       std::function<void()> pauseCallback,
                                       std::function<void()> stopCallback,
                                       std::function<void()> keepCallback,
                                       std::function<void()> redoCallback,
                                       std::function<void()> discardCallback,
                                       std::function<void()> closeCallback,
                                       std::function<void(const juce::String&)> inputDeviceChangedCallback,
                                       std::function<void()> refreshDevicesCallback)
                : onStart(std::move(startCallback)),
                  onTestRecord(std::move(testRecordCallback)),
                  onMicGainChanged(std::move(micGainChangedCallback)),
                  onPauseResume(std::move(pauseCallback)),
                  onStop(std::move(stopCallback)),
                  onKeep(std::move(keepCallback)),
                  onRedo(std::move(redoCallback)),
                  onDiscard(std::move(discardCallback)),
                  onClose(std::move(closeCallback)),
                  onInputDeviceChanged(std::move(inputDeviceChangedCallback)),
                  onRefreshDevices(std::move(refreshDevicesCallback))
            {
                titleLabel.setText("AudioClip Recorder", juce::dontSendNotification);
                titleLabel.setFont(juce::FontOptions(18.0f, juce::Font::bold));
                titleLabel.setJustificationType(juce::Justification::centredLeft);
                statusLabel.setText("Ready. Record will create a new AudioClip track in the active sequence.", juce::dontSendNotification);
                statusLabel.setJustificationType(juce::Justification::topLeft);
                statusLabel.setColour(juce::Label::textColourId, juce::Colours::white);

                durationLabel.setText("Time: 0.00s", juce::dontSendNotification);
                durationLabel.setFont(juce::FontOptions(18.0f, juce::Font::bold));
                durationLabel.setJustificationType(juce::Justification::centredRight);
                durationLabel.setColour(juce::Label::textColourId, juce::Colours::lightgreen);

                delayLabel.setText("Delay", juce::dontSendNotification);
                delayLabel.setJustificationType(juce::Justification::centredRight);
                delayLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.88f));

                inputDeviceLabel.setText("Input", juce::dontSendNotification);
                inputDeviceLabel.setJustificationType(juce::Justification::centredRight);
                inputDeviceLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.88f));

                micGainLabel.setText("Mic Gain", juce::dontSendNotification);
                micGainLabel.setJustificationType(juce::Justification::centredRight);
                micGainLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.88f));
                micGainValueLabel.setText("0.0 dB", juce::dontSendNotification);
                micGainValueLabel.setJustificationType(juce::Justification::centred);
                micGainValueLabel.setColour(juce::Label::textColourId, juce::Colours::lightgreen);
                statusLabel.setTooltip({});
                durationLabel.setTooltip({});
                delayLabel.setTooltip("Delay used only by Record With Delay. Regular Record uses the short 0.25 second safety delay.");
                inputDeviceLabel.setTooltip("Audio input source for the recorder. The dropdown selection applies immediately.");
                micGainLabel.setTooltip("Software recording gain applied inside Poor Man's Studio before the take or test is written. Use small changes and avoid clipping.");
                micGainValueLabel.setTooltip("Current microphone recording gain in decibels. 0.0 dB means no extra software boost or cut.");
                refreshDevicesButton.setButtonText("Refresh Devices");


                startButton.setButtonText("Record");
                delayedStartButton.setButtonText("Record With Delay");
                recordTestButton.setButtonText("Record Test");
                micGainDownButton.setButtonText("-");
                micGainUpButton.setButtonText("+");
                micGainResetButton.setButtonText("0 dB");
                pauseButton.setButtonText("Pause");
                stopButton.setButtonText("Stop");
                keepButton.setButtonText("Save / Apply");
                redoButton.setButtonText("Redo From Top");
                discardButton.setButtonText("Discard Take");

                delayCombo.addItem("3 sec", 3);
                delayCombo.addItem("5 sec", 5);
                delayCombo.addItem("10 sec", 10);
                delayCombo.addItem("Custom", 99);
                delayCombo.setSelectedId(3, juce::dontSendNotification);
                delayCombo.setTooltip("Choose the countdown used by Record With Delay. Custom accepts 1-99 seconds.");
                delayCombo.onChange = [this]
                {
                    updateCustomDelayVisibility();
                    resized();
                };

                customDelayBox.setText("3", juce::dontSendNotification);
                customDelayBox.setInputRestrictions(2, "0123456789");
                customDelayBox.setJustification(juce::Justification::centred);
                customDelayBox.setTooltip("Custom delay in seconds. Maximum 99 seconds.");
                customDelayBox.onTextChange = [this]
                {
                    const auto text = customDelayBox.getText();
                    if (text.isNotEmpty() && text.getIntValue() > 99)
                        customDelayBox.setText("99", juce::dontSendNotification);
                };

                inputDeviceCombo.setTooltip("Choose the system audio input device used for recording. Selecting an item immediately confirms and applies that input choice.");
                inputDeviceCombo.onChange = [this]
                {
                    if (suppressInputDeviceChange)
                        return;

                    if (onInputDeviceChanged)
                        onInputDeviceChanged(inputDeviceCombo.getSelectedId() == 1 ? juce::String() : inputDeviceCombo.getText());
                };

                helperTooltipWindow = std::make_unique<mw::gui::FreshHoverTooltipWindow>(this, 2000);
                helperTooltipWindow->setLookAndFeel(&helperTooltipLookAndFeel);

                refreshDevicesButton.setTooltip("Rescan the system for audio input devices without changing the rest of the recorder setup.");
                refreshDevicesButton.onClick = [this]
                {
                    if (onRefreshDevices)
                        onRefreshDevices();
                };

                startButton.setTooltip("Start recording after a short 0.25 second safety delay. If the take is paused, this resumes the same take and appends audio instead of starting over.");
                delayedStartButton.setTooltip("Start or resume recording after the selected countdown delay. If the take is paused, it appends to the same take instead of overwriting it.");
                recordTestButton.setTooltip("Run a quick mic check: 3 second countdown, 5 second recording, automatic playback, then automatic temp cleanup. It does not create an AudioClip track.");
                micGainDownButton.setTooltip("Reduce the recorder's software mic gain by 3 dB.");
                micGainUpButton.setTooltip("Increase the recorder's software mic gain by 3 dB. Too much boost can add noise or clip.");
                micGainResetButton.setTooltip("Reset software mic gain to 0.0 dB.");
                pauseButton.setTooltip("Pause the active take. Paused status stays visible; either Record button can resume the same take without adding silence.");
                stopButton.setTooltip("Stop the active take so it can be saved, redone, or discarded.");
                keepButton.setTooltip("Save/apply the stopped take into the project audio folder and add it as an AudioClip track.");
                redoButton.setTooltip("Discard the current temp take and restart from the original top/start position.");
                discardButton.setTooltip("Delete the current temporary take without adding it to the project.");

                addAndMakeVisible(micIcon);
                addAndMakeVisible(titleLabel);
                addAndMakeVisible(statusLabel);
                addAndMakeVisible(durationLabel);
                addAndMakeVisible(startButton);
                addAndMakeVisible(delayedStartButton);
                addAndMakeVisible(delayLabel);
                addAndMakeVisible(delayCombo);
                addAndMakeVisible(customDelayBox);
                addAndMakeVisible(recordTestButton);
                addAndMakeVisible(micGainLabel);
                addAndMakeVisible(micGainDownButton);
                addAndMakeVisible(micGainValueLabel);
                addAndMakeVisible(micGainUpButton);
                addAndMakeVisible(micGainResetButton);
                addAndMakeVisible(inputDeviceLabel);
                addAndMakeVisible(inputDeviceCombo);
                addAndMakeVisible(refreshDevicesButton);
                addAndMakeVisible(pauseButton);
                addAndMakeVisible(stopButton);
                addAndMakeVisible(keepButton);
                addAndMakeVisible(redoButton);
                addAndMakeVisible(discardButton);
                addAndMakeVisible(countdownOverlay);
                countdownOverlay.setVisible(false);

                startButton.onClick = [this] { beginStartCountdown(0.25, false); };
                delayedStartButton.onClick = [this] { beginStartCountdown(getSelectedDelaySeconds(), false); };
                recordTestButton.onClick = [this] { beginStartCountdown(3.0, true); };
                micGainDownButton.onClick = [this] { setMicGainDb(currentMicGainDb - 3.0, true); };
                micGainUpButton.onClick = [this] { setMicGainDb(currentMicGainDb + 3.0, true); };
                micGainResetButton.onClick = [this] { setMicGainDb(0.0, true); };
                pauseButton.onClick = [this] { if (onPauseResume) onPauseResume(); };
                stopButton.onClick = [this]
                {
                    cancelPendingCountdown();
                    if (onStop) onStop();
                };
                keepButton.onClick = [this] { if (onKeep) onKeep(); };
                redoButton.onClick = [this]
                {
                    cancelPendingCountdown();
                    if (onRedo) onRedo();
                };
                discardButton.onClick = [this]
                {
                    cancelPendingCountdown();
                    if (onDiscard) onDiscard();
                };

                updateCustomDelayVisibility();
                startTimerHz(4);
            }

            ~AudioRecorderWindowContent() override
            {
                stopTimer();
                if (helperTooltipWindow != nullptr)
                    helperTooltipWindow->setLookAndFeel(nullptr);
                helperTooltipWindow.reset();
            }

            void setStatusText(const juce::String& text)
            {
                statusLabel.setText(text, juce::dontSendNotification);
            }

            void setDurationText(const juce::String& text)
            {
                durationLabel.setText("Time: " + text, juce::dontSendNotification);
            }

            void setMicGainDb(double gainDb, bool notify = false)
            {
                currentMicGainDb = juce::jlimit(-24.0, 24.0, gainDb);
                micGainValueLabel.setText(juce::String(currentMicGainDb, 1) + " dB", juce::dontSendNotification);
                if (notify && onMicGainChanged)
                    onMicGainChanged(currentMicGainDb);
            }

            void setTestModeActive(bool active)
            {
                startButton.setEnabled(!active);
                delayedStartButton.setEnabled(!active);
                recordTestButton.setEnabled(!active);
                pauseButton.setEnabled(!active);
                stopButton.setEnabled(!active);
                keepButton.setEnabled(!active);
                redoButton.setEnabled(!active);
                discardButton.setEnabled(!active);
                delayCombo.setEnabled(!active);
                customDelayBox.setEnabled(!active);
                inputDeviceCombo.setEnabled(!active);
                refreshDevicesButton.setEnabled(!active);
                micGainDownButton.setEnabled(!active);
                micGainUpButton.setEnabled(!active);
                micGainResetButton.setEnabled(!active);
            }

            void setInputDevices(const juce::StringArray& deviceNames, const juce::String& selectedDevice)
            {
                suppressInputDeviceChange = true;
                inputDeviceCombo.clear(juce::dontSendNotification);
                inputDeviceCombo.addItem("System Default", 1);

                int selectedId = 1;
                int id = 10;
                for (const auto& name : deviceNames)
                {
                    inputDeviceCombo.addItem(name, id);
                    if (selectedDevice.isNotEmpty() && name == selectedDevice)
                        selectedId = id;
                    ++id;
                }

                if (deviceNames.isEmpty())
                {
                    inputDeviceCombo.addItem("No input devices detected", 2);
                    inputDeviceCombo.setSelectedId(2, juce::dontSendNotification);
                }
                else
                {
                    inputDeviceCombo.setSelectedId(selectedId, juce::dontSendNotification);
                }

                suppressInputDeviceChange = false;
            }

            void resized() override
            {
                auto area = getLocalBounds().reduced(12);
                auto top = area.removeFromTop(38);
                durationLabel.setBounds(top.removeFromRight(150).reduced(4, 2));
                micIcon.setBounds(top.removeFromLeft(34).reduced(3));
                titleLabel.setBounds(top.reduced(4, 2));
                area.removeFromTop(8);
                statusLabel.setBounds(area.removeFromTop(96).reduced(4, 2));
                area.removeFromTop(8);

                auto inputRow = area.removeFromTop(34);
                inputDeviceLabel.setBounds(inputRow.removeFromLeft(62).reduced(4, 2));
                inputDeviceCombo.setBounds(inputRow.removeFromLeft(560).reduced(4, 2));
                refreshDevicesButton.setBounds(inputRow.removeFromLeft(150).reduced(4, 2));
                area.removeFromTop(4);

                auto gainRow = area.removeFromTop(34);
                micGainLabel.setBounds(gainRow.removeFromLeft(74).reduced(4, 2));
                micGainDownButton.setBounds(gainRow.removeFromLeft(46).reduced(4, 2));
                micGainValueLabel.setBounds(gainRow.removeFromLeft(88).reduced(4, 2));
                micGainUpButton.setBounds(gainRow.removeFromLeft(46).reduced(4, 2));
                micGainResetButton.setBounds(gainRow.removeFromLeft(76).reduced(4, 2));
                area.removeFromTop(4);

                auto recordRow = area.removeFromTop(34);
                startButton.setBounds(recordRow.removeFromLeft(92).reduced(4, 2));
                delayedStartButton.setBounds(recordRow.removeFromLeft(155).reduced(4, 2));
                recordTestButton.setBounds(recordRow.removeFromLeft(118).reduced(4, 2));
                delayLabel.setBounds(recordRow.removeFromLeft(54).reduced(4, 2));
                delayCombo.setBounds(recordRow.removeFromLeft(94).reduced(4, 2));
                if (customDelayBox.isVisible())
                    customDelayBox.setBounds(recordRow.removeFromLeft(56).reduced(4, 2));
                else
                    customDelayBox.setBounds({});

                area.removeFromTop(4);
                auto takeRow = area.removeFromTop(34);
                pauseButton.setBounds(takeRow.removeFromLeft(95).reduced(4, 2));
                stopButton.setBounds(takeRow.removeFromLeft(90).reduced(4, 2));
                keepButton.setBounds(takeRow.removeFromLeft(120).reduced(4, 2));
                redoButton.setBounds(takeRow.removeFromLeft(135).reduced(4, 2));
                discardButton.setBounds(takeRow.removeFromLeft(135).reduced(4, 2));

                countdownOverlay.setBounds(getLocalBounds());
                countdownOverlay.toFront(false);
            }

        private:
            void timerCallback() override
            {
                updateCountdownIfNeeded();

                if (onRefresh)
                    onRefresh();
            }

            void beginStartCountdown(double seconds, bool isTest)
            {
                if (countdownActive || (isTest ? !onTestRecord : !onStart))
                    return;

                countdownActive = true;
                countdownIsTest = isTest;
                countdownStartTriggered = false;
                countdownStartedMs = juce::Time::getMillisecondCounterHiRes();
                countdownDurationMs = juce::jmax(250.0, seconds * 1000.0);
                countdownHideAtMs = 0.0;
                countdownOverlay.setCountdownText(seconds <= 0.5 ? "Ready..." : juce::String(static_cast<int>(std::ceil(seconds))));
                countdownOverlay.setVisible(true);
                countdownOverlay.toFront(false);
                startTimerHz(20);
            }

            void cancelPendingCountdown()
            {
                if (!countdownActive || countdownStartTriggered)
                    return;

                countdownActive = false;
                countdownStartTriggered = false;
                countdownOverlay.setVisible(false);
                startTimerHz(4);
            }

            void updateCountdownIfNeeded()
            {
                if (!countdownActive)
                    return;

                const auto nowMs = juce::Time::getMillisecondCounterHiRes();

                if (!countdownStartTriggered)
                {
                    const auto elapsedMs = nowMs - countdownStartedMs;
                    const auto remainingMs = countdownDurationMs - elapsedMs;

                    if (remainingMs <= 0.0)
                    {
                        countdownStartTriggered = true;
                        countdownHideAtMs = nowMs + 250.0;
                        countdownOverlay.setCountdownText("GO!");
                        if (countdownIsTest)
                        {
                            if (onTestRecord)
                                onTestRecord();
                        }
                        else if (onStart)
                        {
                            onStart();
                        }
                        return;
                    }

                    const auto remainingSeconds = static_cast<int>(std::ceil(remainingMs / 1000.0));
                    countdownOverlay.setCountdownText(countdownDurationMs <= 500.0 ? "Ready..." : juce::String(juce::jmax(1, remainingSeconds)));
                    return;
                }

                if (nowMs >= countdownHideAtMs)
                {
                    countdownActive = false;
                    countdownStartTriggered = false;
                    countdownOverlay.setVisible(false);
                    startTimerHz(4);
                }
            }

            double getSelectedDelaySeconds() const
            {
                const auto selectedId = delayCombo.getSelectedId();
                if (selectedId == 3 || selectedId == 5 || selectedId == 10)
                    return static_cast<double>(selectedId);

                const auto custom = customDelayBox.getText().getIntValue();
                return static_cast<double>(juce::jlimit(1, 99, custom <= 0 ? 3 : custom));
            }

            void updateCustomDelayVisibility()
            {
                customDelayBox.setVisible(delayCombo.getSelectedId() == 99);
            }

        public:
            std::function<void()> onRefresh;

        private:
            MicrophoneIcon micIcon;
            juce::Label titleLabel;
            juce::Label statusLabel;
            juce::Label durationLabel;
            juce::TextButton startButton;
            juce::TextButton delayedStartButton;
            juce::TextButton recordTestButton;
            juce::Label delayLabel;
            juce::ComboBox delayCombo;
            juce::TextEditor customDelayBox;
            juce::Label micGainLabel;
            juce::TextButton micGainDownButton;
            juce::Label micGainValueLabel;
            juce::TextButton micGainUpButton;
            juce::TextButton micGainResetButton;
            juce::Label inputDeviceLabel;
            juce::ComboBox inputDeviceCombo;
            juce::TextButton refreshDevicesButton;
            juce::TextButton pauseButton;
            juce::TextButton stopButton;
            juce::TextButton keepButton;
            juce::TextButton redoButton;
            juce::TextButton discardButton;
            juce::TextButton closeButtonRef;
            CountdownOverlay countdownOverlay;
            double currentMicGainDb = 0.0;
            bool countdownActive = false;
            bool countdownIsTest = false;
            bool countdownStartTriggered = false;
            double countdownStartedMs = 0.0;
            double countdownDurationMs = 250.0;
            double countdownHideAtMs = 0.0;
            bool suppressInputDeviceChange = false;
            mw::gui::HelperTooltipLookAndFeel helperTooltipLookAndFeel;
            std::unique_ptr<juce::TooltipWindow> helperTooltipWindow;
            std::function<void()> onStart;
            std::function<void()> onTestRecord;
            std::function<void(double)> onMicGainChanged;
            std::function<void()> onPauseResume;
            std::function<void()> onStop;
            std::function<void()> onKeep;
            std::function<void()> onRedo;
            std::function<void()> onDiscard;
            std::function<void()> onClose;
            std::function<void(const juce::String&)> onInputDeviceChanged;
            std::function<void()> onRefreshDevices;
        };


        enum MenuCommandIds
        {
            menuNewProject = 1001,
            menuOpenProject,
            menuSaveProject,
            menuProjectInfo,
            menuSaveSettings,

            menuCleanTemp = 1101,
            menuChooseExportFolder,

            menuChooseInput = 1201,
            menuImportSequence,
            menuImportAudio,
            menuRecordAudio,

            menuRenderCurrent = 1301,
            menuRenderTrack,
            menuRenderSequence,
            menuRenderMidi,
            menuRenderSettings,
            menuPreviewTrack,
            menuPreviewSequence,
            menuCancelRender,

            menuTrackManager = 1401,
            menuPianoRoll,
            menuPreviewPlayer,
            menuToggleHelperBubbles,

            menuVstScan = 1551,
            menuVstPluginManager,
            menuVstSettings,
            menuVstOpenSelectedTrackUi,
            menuVstCloseAllWindows,
            menuVstRefreshGraphics,
            menuToggleVstCompatibilityWarnings,

            menuWindowMain = 1601,
            menuWindowTrackManager,
            menuWindowPreviewPlayer,
            menuWindowRawNotes,
            menuWindowAudioRecorder,
            menuWindowProjectInfo,
            menuWindowSequencePicker,
            menuWindowSequenceThoughts,
            menuWindowSequenceColor,
            menuWindowRenderSettings,
            menuWindowPianoRollBase = 1700,

            menuUserGuide = 1501,
            menuSetupGuide,
            menuAbout
        };
    }

    juce::StringArray MainComponent::getMenuBarNames()
    {
        return { "File", "Project", "Import", "Render", "View", "Window", "VST Plugins", "Help" };
    }

    juce::PopupMenu MainComponent::getMenuForIndex(int topLevelMenuIndex, const juce::String&)
    {
        juce::PopupMenu menu;

        switch (topLevelMenuIndex)
        {
            case 0:
                menu.addItem(menuNewProject, "New Blank Project");
                menu.addItem(menuOpenProject, "Open .mwproj Project...");
                menu.addItem(menuSaveProject, "Save .mwproj Project...");
                menu.addSeparator();
                menu.addItem(menuProjectInfo, "Edit Project Info / Metadata...");
                menu.addItem(menuSaveSettings, "Save Settings");
                break;

            case 1:
                menu.addItem(menuCleanTemp, "Clean Temp / Preview Files");
                menu.addItem(menuChooseExportFolder, "Choose Render Export Folder...");
                break;

            case 2:
                menu.addItem(-1, "Start = replace/begin project; Add = keep project and add to arrangement", false);
                menu.addSeparator();
                menu.addItem(menuChooseInput, "Start Project From MIDI / MusicXML...");
                menu.addItem(menuImportSequence, "Add File as New/Existing Sequence...");
                menu.addSeparator();
                menu.addItem(menuImportAudio, "Import Audio...");
                menu.addItem(menuRecordAudio, "Record AudioClip...");
                break;

            case 3:
                updateRenderTargetLabel();
                menu.addItem(-2, renderTargetLabel.getText(), false);
                menu.addSeparator();
                menu.addItem(menuRenderCurrent, "Render Full Project Audio");
                menu.addItem(menuRenderMidi, "Render Full Project MIDI");
                menu.addItem(menuRenderSettings, "Render Settings...");
                menu.addSeparator();
                menu.addItem(menuRenderTrack, "Render Selected Track Shown in Render Target");
                menu.addItem(menuRenderSequence, "Render Active Sequence Shown in Render Target");
                menu.addSeparator();
                menu.addItem(menuPreviewTrack, "Preview Selected Track in Player");
                menu.addItem(menuPreviewSequence, "Preview Active Sequence in Player");
                menu.addSeparator();
                menu.addItem(menuCancelRender, "Cancel Render", renderingInProgress);
                break;

            case 4:
                menu.addItem(menuTrackManager, "Open Track / Sequence Manager");
                menu.addItem(menuPianoRoll, "Open Selected Track Piano Roll");
                break;

            case 5:
                menu.addItem(menuWindowMain, "Main UI", true);

                if (trackManagerWindow != nullptr)
                    menu.addItem(menuWindowTrackManager, "Track / Sequence Manager");

                if (rawNotesWindow != nullptr)
                    menu.addItem(menuWindowRawNotes, "Raw Notes - Advanced");

                if (pianoRollPreviewPlayerWindow != nullptr)
                    menu.addItem(menuWindowPreviewPlayer, "Piano Roll Preview Player");

                if (audioRecorderWindow != nullptr)
                    menu.addItem(menuWindowAudioRecorder, "Audio Recorder");

                if (projectInfoWindow != nullptr)
                    menu.addItem(menuWindowProjectInfo, "Project Info");

                if (sequencePickerWindow != nullptr)
                    menu.addItem(menuWindowSequencePicker, "Change Active Sequence");

                if (sequenceThoughtsWindow != nullptr)
                    menu.addItem(menuWindowSequenceThoughts, "Sequence Thoughts");

                if (sequenceColorWindow != nullptr)
                    menu.addItem(menuWindowSequenceColor, "Sequence Color");

                if (renderSettingsWindow != nullptr)
                    menu.addItem(menuWindowRenderSettings, "Render Settings");

                if (!pianoRollEditorWindows.empty())
                {
                    menu.addSeparator();

                    juce::PopupMenu pianoRollMenu;
                    int openPianoRollCount = 0;

                    for (const auto& entry : pianoRollEditorWindows)
                    {
                        if (entry.second != nullptr && entry.second->window != nullptr)
                        {
                            ++openPianoRollCount;
                            pianoRollMenu.addItem(
                                menuWindowPianoRollBase + entry.first,
                                "Piano Roll - " + getTrackDisplayName(entry.first) + ((entry.second->dirty || entry.second->hasPendingInstrumentAssignment) ? " *" : "")
                            );
                        }
                    }

                    if (openPianoRollCount == 1)
                    {
                        for (const auto& entry : pianoRollEditorWindows)
                        {
                            if (entry.second != nullptr && entry.second->window != nullptr)
                            {
                                menu.addItem(
                                    menuWindowPianoRollBase + entry.first,
                                    "Piano Roll - " + getTrackDisplayName(entry.first) + ((entry.second->dirty || entry.second->hasPendingInstrumentAssignment) ? " *" : "")
                                );
                                break;
                            }
                        }
                    }
                    else if (openPianoRollCount > 1)
                    {
                        menu.addSubMenu("Piano Rolls", pianoRollMenu, true);
                    }
                }

                if (trackManagerWindow == nullptr
                    && rawNotesWindow == nullptr
                    && pianoRollPreviewPlayerWindow == nullptr
                    && audioRecorderWindow == nullptr
                    && projectInfoWindow == nullptr
                    && sequencePickerWindow == nullptr
                    && sequenceThoughtsWindow == nullptr
                    && sequenceColorWindow == nullptr
                    && renderSettingsWindow == nullptr
                    && pianoRollEditorWindows.empty())
                {
                    menu.addSeparator();
                    menu.addItem(-10, "No tool windows are open", false);
                }
                break;

            case 6:
                menu.addItem(menuVstScan, "Scan VST3 Plugins");
                menu.addItem(menuVstPluginManager, "VST3 Plugin Manager...");
                menu.addItem(menuVstSettings, "VST3 Settings...");
                menu.addItem(menuVstRefreshGraphics, "Refresh Adapter List");
                menu.addSeparator();
                menu.addItem(menuVstOpenSelectedTrackUi, "Open Selected Track VST Plugin", selectedTrackHasAppliedVstPlugin());
                menu.addItem(menuVstCloseAllWindows, "Close All VST Plugin Windows");
                break;

            case 7:
                menu.addItem(menuUserGuide, "Open User Guide PDF");
                menu.addItem(menuSetupGuide, "Show Setup / Build Guide File");
                menu.addSeparator();
                menu.addItem(menuToggleHelperBubbles, "Helper Bubbles", true, helperBubblesEnabled);
                menu.addItem(menuToggleVstCompatibilityWarnings, "VST Plugin Compatibility Warnings", true, vstCompatibilityWarningsEnabled);
                menu.addSeparator();
                menu.addItem(menuAbout, "About Poor Man's Studio");
                break;

            default:
                break;
        }

        return menu;
    }

    void MainComponent::focusOpenWindowByMenuId(int menuItemID)
    {
        auto focusWindow = [](std::unique_ptr<juce::DocumentWindow>& window)
        {
            if (window != nullptr)
                window->toFront(true);
        };

        if (menuItemID == menuWindowMain)
        {
            if (auto* top = getTopLevelComponent())
                top->toFront(true);
            return;
        }

        if (menuItemID == menuWindowTrackManager) { focusWindow(trackManagerWindow); return; }
        if (menuItemID == menuWindowPreviewPlayer) { focusWindow(pianoRollPreviewPlayerWindow); return; }
        if (menuItemID == menuWindowRawNotes) { focusWindow(rawNotesWindow); return; }
        if (menuItemID == menuWindowAudioRecorder) { focusWindow(audioRecorderWindow); return; }
        if (menuItemID == menuWindowProjectInfo) { focusWindow(projectInfoWindow); return; }
        if (menuItemID == menuWindowSequencePicker) { focusWindow(sequencePickerWindow); return; }
        if (menuItemID == menuWindowSequenceThoughts) { focusWindow(sequenceThoughtsWindow); return; }
        if (menuItemID == menuWindowSequenceColor) { focusWindow(sequenceColorWindow); return; }
        if (menuItemID == menuWindowRenderSettings) { focusWindow(renderSettingsWindow); return; }

        if (menuItemID >= menuWindowPianoRollBase)
        {
            const auto trackIndex = menuItemID - menuWindowPianoRollBase;
            if (auto* state = findPianoRollEditorWindow(trackIndex))
            {
                if (state->window != nullptr)
                    state->window->toFront(true);
            }
        }
    }

    void MainComponent::menuItemSelected(int menuItemID, int)
    {
        if (menuItemID == menuWindowMain
            || menuItemID == menuWindowTrackManager
            || menuItemID == menuWindowPreviewPlayer
            || menuItemID == menuWindowRawNotes
            || menuItemID == menuWindowAudioRecorder
            || menuItemID == menuWindowProjectInfo
            || menuItemID == menuWindowSequencePicker
            || menuItemID == menuWindowSequenceThoughts
            || menuItemID == menuWindowSequenceColor
            || menuItemID == menuWindowRenderSettings
            || menuItemID >= menuWindowPianoRollBase)
        {
            focusOpenWindowByMenuId(menuItemID);
            return;
        }

        switch (menuItemID)
        {
            case menuNewProject: startNewProject(); break;
            case menuOpenProject: openProjectFile(); break;
            case menuSaveProject: saveCurrentProjectFile(); break;
            case menuProjectInfo: openProjectInfoWindow(); break;
            case menuSaveSettings: saveUserSettingsNow(); break;

            case menuCleanTemp: cleanTempFolder(); break;
            case menuChooseExportFolder: chooseExportFolder(); break;

            case menuChooseInput: chooseMusicXml(); break;
            case menuImportSequence: importFilesAsSequence(); break;
            case menuImportAudio: importAudioFile(); break;
            case menuRecordAudio: openAudioRecorderWindow(); break;

            case menuRenderCurrent: renderCurrentProjectOnBackgroundThread(); break;
            case menuRenderMidi:
                renderMidiCurrentProject();
                break;

            case menuRenderSettings:
                showRenderSettingsWindow();
                break;

            case menuRenderTrack:
                updateRenderTargetLabel();
                renderSelectedTrackOnBackgroundThread();
                break;

            case menuRenderSequence:
                updateRenderTargetLabel();
                if (activeImportSectionIndex < 0)
                    openTrackManagerWindow();
                renderSelectedSequenceOnBackgroundThread();
                break;

            case menuPreviewTrack:
                updateRenderTargetLabel();
                previewSelectedTrackOnBackgroundThread();
                break;

            case menuPreviewSequence:
                updateRenderTargetLabel();
                if (activeImportSectionIndex < 0)
                    openTrackManagerWindow();
                previewSelectedSequenceOnBackgroundThread();
                break;
            case menuCancelRender:
                cancelRenderRequested = true;
                logMessage("Cancel requested. Stopping active parallel stem render processes where supported...");
                break;

            case menuVstScan: scanVstPlugins(true); break;
            case menuVstPluginManager: openVstPluginManagerWindow(); break;
            case menuVstSettings: openVstSettingsWindow(); break;
            case menuVstRefreshGraphics: refreshVstGraphicsProfile(false); break;
            case menuVstOpenSelectedTrackUi: openSelectedTrackVstPluginUi(); break;
            case menuVstCloseAllWindows: closeAllVstPluginWindows(); break;
            case menuToggleVstCompatibilityWarnings: setVstCompatibilityWarningsEnabled(!vstCompatibilityWarningsEnabled); break;

            case menuTrackManager: openTrackManagerWindow(); break;
            case menuPianoRoll: openPianoRollWindow(); break;
            case menuToggleHelperBubbles: setHelperBubblesEnabled(!helperBubblesEnabled); break;

            case menuUserGuide:
            {
                const auto docsFolder = juce::File((mw::app::AppPaths::workspaceFolder() / "docs").string());
                const auto pdfFile = docsFolder.getChildFile("PoorMansStudio_User_Guide.pdf");
                const auto textFile = docsFolder.getChildFile("USER_GUIDE.txt");

                if (pdfFile.existsAsFile())
                {
                    if (!pdfFile.startAsProcess())
                    {
                        pdfFile.revealToUser();
                        logMessage("Could not open user guide PDF directly; revealing file instead: " + pdfFile.getFullPathName());
                    }
                }
                else if (textFile.existsAsFile())
                {
                    logMessage("User guide PDF not found; opening text guide instead: " + textFile.getFullPathName());
                    textFile.revealToUser();
                }
                else
                {
                    logMessage("User guide not found in docs folder: " + docsFolder.getFullPathName());
                }

                break;
            }

            case menuSetupGuide:
            {
                const auto file = juce::File((mw::app::AppPaths::workspaceFolder() / "docs" / "SETUP_AND_BUILD_GUIDE.txt").string());
                if (file.existsAsFile())
                    file.revealToUser();
                else
                    logMessage("Setup guide not found: " + file.getFullPathName());
                break;
            }

            case menuAbout:
                juce::AlertWindow::showMessageBoxAsync(
                    juce::AlertWindow::InfoIcon,
                    "About Poor Man's Studio",
                    mw::app::applicationTitle() + juce::newLine + juce::newLine
                        + "A lightweight MusicXML/MIDI arranging and rendering workstation."
                );
                break;

            default:
                break;
        }
    }

    MainComponent::MainComponent()
    {
        const auto startupPreferences = mw::app::UserPreferencesStore::load();
        helperBubblesEnabled = startupPreferences.helperBubblesEnabled;
        vstCompatibilityWarningsEnabled = startupPreferences.vstCompatibilityWarningsEnabled;
        vstSafePluginUiMode = startupPreferences.vstSafePluginUiMode;
        vstWarningStyleId = startupPreferences.vstWarningStyleId;
        vstMaxOpenPluginWindows = sanitizeMaxOpenVstPluginWindows(startupPreferences.vstMaxOpenPluginWindows);
        vstExperimentalWarningAcknowledged = startupPreferences.vstExperimentalWarningAcknowledged;
        vstGraphicsProfile.detected = startupPreferences.vstGraphicsProfileDetected;
        vstGraphicsProfile.source = startupPreferences.vstGraphicsProfileSource;
        vstGraphicsProfile.lastDetectedLocal = startupPreferences.vstGraphicsProfileLastDetected;
        vstGraphicsProfile.preferredPluginGpuId = startupPreferences.vstPreferredPluginGpuId;
        mw::gui::setHelperBubblesGloballyEnabled(helperBubblesEnabled);

        const auto startupPreferredVstGpuId = vstGraphicsProfile.preferredPluginGpuId.empty()
            ? std::string("auto")
            : vstGraphicsProfile.preferredPluginGpuId;
        const bool shouldValidateSavedVstGpuOnStartup = startupPreferredVstGpuId != "auto"
            && vstGraphicsProfile.adapters.empty();

        if (!vstGraphicsProfile.detected || shouldValidateSavedVstGpuOnStartup)
            refreshVstGraphicsProfile(true);

        titleLabel.setText(mw::app::applicationTitle(), juce::dontSendNotification);
        titleLabel.setJustificationType(juce::Justification::centredLeft);
        titleLabel.setFont(juce::FontOptions(24.0f, juce::Font::bold));

        renderStatusLabel.setText("Ready", juce::dontSendNotification);
        renderStatusLabel.setJustificationType(juce::Justification::centredRight);
        renderStatusLabel.setColour(juce::Label::textColourId, juce::Colours::lightgreen);
        renderStatusLabel.setFont(juce::FontOptions(18.0f, juce::Font::bold));

        renderTargetLabel.setText("Render Target: Track -- | Seq --", juce::dontSendNotification);
        renderTargetLabel.setJustificationType(juce::Justification::centredLeft);
        renderTargetLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
        renderTargetLabel.setFont(juce::FontOptions(14.0f));

        auto setupLabel = [](juce::Label& label, const juce::String& text)
        {
            label.setText(text, juce::dontSendNotification);
            label.setJustificationType(juce::Justification::centredLeft);
        };

        setupLabel(musicXmlLabel, "MusicXML:");
        setupLabel(exportFolderLabel, "Export Folder:");
        setupLabel(projectDefaultsLabel, "Project Defaults");
        projectDefaultsLabel.setFont(juce::FontOptions(16.0f, juce::Font::bold));
        setupLabel(soundFontLabel, "SoundFont (.sf2/.sf3):");
        setupLabel(fluidSynthLabel, "FluidSynth:");
        setupLabel(ffmpegLabel, "FFmpeg:");
        setupLabel(backendLabel, "Backend:");
        setupLabel(sfzLabel, "SFZ:");
        setupLabel(sfizzLabel, "sfizz-render:");
        setupLabel(sfzKeySwitchLabel, "SFZ Key:");
        setupLabel(sfzCc1Label, "CC1:");
        setupLabel(sfzCc11Label, "CC11:");
        setupLabel(baseNameLabel, "Base Filename:");
        setupLabel(outputFormatLabel, "Output Format:");
        setupLabel(audioClipFormatLabel, "AudioClip Save:");
        setupLabel(audioClipQualityLabel, "Clip Quality:");
        setupLabel(sampleRateLabel, "Sample Rate:");
        setupLabel(bitrateLabel, "Bitrate:");
        setupLabel(channelsLabel, "Channels:");
        setupLabel(renderWorkersLabel, "Parallel Stems:");
        renderOutputSummaryLabel.setText("Render Output: WAV | 48 kHz | ~2304 kbps | Stereo", juce::dontSendNotification);
        renderOutputSummaryLabel.setJustificationType(juce::Justification::centredLeft);
        renderOutputSummaryLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
        setupLabel(themeLabel, "Theme:");
        setupLabel(trackLabel, "Track:");
        setupLabel(sequenceSelectorLabel, "Active Seq:");
        setupLabel(sequenceThoughtsLabel, "Thoughts:");
        setupLabel(trackSoundLibraryLabel, "Track Sound Library:");
        setupLabel(instrumentLabel, "Instrument:");
        setupLabel(trackVolumeLabel, "Track Vol:");
        setupLabel(masterVolumeLabel, "Master Vol:");
        setupLabel(tempoLabel, "Tempo BPM:");
        setupLabel(timeSignatureLabel, "Time Sig:");
        setupLabel(loopCountLabel, "Loop Count:");
        setupLabel(noteEditorLabel, "Editors:");
        setupLabel(pianoRollBpmLabel, "Tempo BPM:");
        setupLabel(pianoRollTimeSigLabel, "Time Sig:");
        setupLabel(pianoRollBeatWindowLabel, "Beats Visible:");
        setupLabel(pianoRollStartBeatLabel, "Start Beat:");
        setupLabel(pianoRollNoteLengthLabel, "Note Length:");
        setupLabel(pianoRollVelocityLabel, "Velocity:");
        setupLabel(pianoRollSnapLabel, "Snap Grid:");
        setupLabel(pianoRollPageInputLabel, "Window:");
        pianoRollPageLabel.setText("Window 1 / 1 | Beats 0-16", juce::dontSendNotification);
        pianoRollPageLabel.setJustificationType(juce::Justification::centredLeft);
        pianoRollPageLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
        pianoRollHelpLabel.setText("Piano Roll: click grid to add notes, click notes to select, use Piano Roll tools for note editing.", juce::dontSendNotification);
        pianoRollHelpLabel.setJustificationType(juce::Justification::centredLeft);
        pianoRollHelpLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);

        auto setupPathBox = [](juce::TextEditor& box)
        {
            box.setMultiLine(false);
            box.setReadOnly(false);
            box.setScrollbarsShown(true);
        };

        setupPathBox(musicXmlPathBox);
        setupPathBox(exportFolderBox);
        setupPathBox(soundFontPathBox);
        setupPathBox(fluidSynthPathBox);
        setupPathBox(ffmpegPathBox);
        setupPathBox(sfzPathBox);
        setupPathBox(sfizzPathBox);
        setupPathBox(sfzKeySwitchBox);
        setupPathBox(sfzCc1Box);
        setupPathBox(sfzCc11Box);
        setupPathBox(trackSoundLibraryBox);
        setupPathBox(trackManagerMapStartBeatBox);
        setupPathBox(trackManagerMapBeatWindowBox);
        trackSoundLibraryBox.setReadOnly(true);

        pianoRollBpmBox.setText("120", juce::dontSendNotification);
        pianoRollTimeSigBox.setText("4/4", juce::dontSendNotification);
        pianoRollBeatWindowBox.setText("16", juce::dontSendNotification);
        pianoRollStartBeatBox.setText("0", juce::dontSendNotification);
        pianoRollNoteLengthBox.setText("1", juce::dontSendNotification);
        pianoRollVelocityBox.setText("100", juce::dontSendNotification);
        pianoRollSnapBox.setText("1", juce::dontSendNotification);
        trackManagerMapStartBeatBox.setText("0", juce::dontSendNotification);
        trackManagerMapBeatWindowBox.setText("Full", juce::dontSendNotification);

        sfzKeySwitchBox.setText("24");
        sfzCc1Box.setText("100");
        sfzCc11Box.setText("127");
        setupPathBox(baseNameBox);
        baseNameBox.onTextChange = [this]
        {
            if (currentProject)
                setProjectDirty();
        };

        soundFontCombo.setTextWhenNothingSelected("No SoundFont selected");
        soundFontCombo.setTextWhenNoChoicesAvailable("No .sf2/.sf3 files found");
        soundFontCombo.onChange = [this]
        {
            const auto path = getSelectedSoundFontPath();

            if (!path.empty())
            {
                soundFontPathBox.setText(path.string());
                logMessage("Selected SoundFont: " + path.string());
                refreshPresetListFromSelectedSoundFont();

                if (currentProject)
                    setProjectDirty();
            }
        };

        outputFormatCombo.addItem("WAV", 1);
        outputFormatCombo.addItem("FLAC", 2);
        outputFormatCombo.addItem("MP3", 3);
        outputFormatCombo.addItem("OGG", 4);
        outputFormatCombo.setSelectedId(1);

        audioClipFormatCombo.addItem("WAV lossless", 1);
        audioClipFormatCombo.addItem("FLAC lossless", 2);
        audioClipFormatCombo.addItem("MP3 high quality", 3);
        audioClipFormatCombo.addItem("OGG high quality", 4);
        audioClipFormatCombo.setSelectedId(1);

        audioClipQualityCombo.addItem("High / lossless default", 320);
        audioClipQualityCombo.addItem("256 kbps", 256);
        audioClipQualityCombo.addItem("192 kbps", 192);
        audioClipQualityCombo.addItem("128 kbps", 128);
        audioClipQualityCombo.setSelectedId(320);

        backendCombo.addItem("SF2 / FluidSynth", 1);
        backendCombo.addItem("SFZ / sfizz-render", 2);
        backendCombo.addItem("VST3 Plugin", 3);
        backendCombo.setSelectedId(1);

        trackBackendCombo.addItem("Use Project Backend", 1);
        trackBackendCombo.addItem("SF2", 2);
        trackBackendCombo.addItem("SFZ", 3);
        trackBackendCombo.addItem("VST3 Plugin", 4);
        trackBackendCombo.setSelectedId(1);
        sequenceSelectorBox.setMultiLine(false);
        sequenceSelectorBox.setReadOnly(true);
        sequenceSelectorBox.setScrollbarsShown(false);
        sequenceSelectorBox.setText("No Sequence", juce::dontSendNotification);
        sequenceThoughtsBox.setMultiLine(true);
        sequenceThoughtsBox.setReturnKeyStartsNewLine(true);
        sequenceThoughtsBox.setReadOnly(true);
        sequenceThoughtsBox.setScrollbarsShown(true);
        sequenceThoughtsBox.setTextToShowWhenEmpty("Thoughts for the active sequence...", juce::Colours::grey);
        sequenceThoughtsBox.onTextChange = [this]
        {
            if (!suppressSequenceThoughtsChange)
                syncSequenceThoughtsFromEditor();
        };
        changeActiveSequenceButton.setButtonText("Chg Seq");
        changeActiveSequenceButton.onClick = [this] { showChangeActiveSequenceDialog(); };

        trackBackendCombo.onChange = [this]
        {
            populateInstrumentCombo();
            refreshTrackSoundLibraryDisplay();
            updateOpenVstPluginButtonState();
        };
        changeTrackLibraryButton.onClick = [this] { chooseTrackSoundLibrary(); };
        backendCombo.onChange = [this]
        {
            populateInstrumentCombo();
            refreshTrackSoundLibraryDisplay();
            updateOpenVstPluginButtonState();
        };
        applyBackendButton.onClick = [this] { applyProjectBackendSelection(); };
        instrumentCombo.onChange = [this]
        {
            if (currentProject && trackCombo.getSelectedId() > 0)
                setProjectDirty();

            updateOpenVstPluginButtonState();
        };

        sampleRateCombo.addItem("44100 Hz", 44100);
        sampleRateCombo.addItem("48000 Hz", 48000);
        sampleRateCombo.addItem("96000 Hz", 96000);
        sampleRateCombo.setSelectedId(48000);

        bitrateCombo.addItem("128 kbps", 128);
        bitrateCombo.addItem("192 kbps", 192);
        bitrateCombo.addItem("256 kbps", 256);
        bitrateCombo.addItem("320 kbps", 320);
        bitrateCombo.setSelectedId(192);

        channelsCombo.addItem("Mono", 1);
        channelsCombo.addItem("Stereo", 2);
        channelsCombo.setSelectedId(2);

        renderWorkersCombo.addItem("Auto (safe)", 100);
        renderWorkersCombo.addItem("1", 1);
        renderWorkersCombo.addItem("2", 2);
        renderWorkersCombo.addItem("4", 4);
        renderWorkersCombo.addItem("6", 6);
        renderWorkersCombo.addItem("8", 8);
        renderWorkersCombo.addItem("12", 12);
        renderWorkersCombo.addItem("16", 16);
        renderWorkersCombo.setSelectedId(100);

        outputFormatCombo.onChange = [this]
        {
            updateRenderOutputSummary();
            if (currentProject)
                setProjectDirty();
        };
        audioClipFormatCombo.onChange = [this]
        {
            if (currentProject)
                setProjectDirty();
        };
        audioClipQualityCombo.onChange = [this]
        {
            if (currentProject)
                setProjectDirty();
        };
        sampleRateCombo.onChange = [this]
        {
            updateRenderOutputSummary();
            if (currentProject)
                setProjectDirty();
        };
        bitrateCombo.onChange = [this]
        {
            updateRenderOutputSummary();
            if (currentProject)
                setProjectDirty();
        };
        channelsCombo.onChange = [this]
        {
            updateRenderOutputSummary();
            if (currentProject)
                setProjectDirty();
        };
        renderWorkersCombo.onChange = [this]
        {
            updateRenderOutputSummary();
            if (currentProject)
                setProjectDirty();
        };

        loadThemePresets();
        themeCombo.onChange = [this]
        {
            const int selectedId = themeCombo.getSelectedId() > 0 ? themeCombo.getSelectedId() : 1;
            applyThemePreset(selectedId);
            saveThemePreference();
        };



        auto setupSlider = [](juce::Slider& slider)
        {
            slider.setSliderStyle(juce::Slider::LinearHorizontal);
            slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 70, 22);
            slider.setRange(0.0, 1.5, 0.01);
            slider.setValue(1.0);
        };

        setupSlider(trackVolumeSlider);
        setupSlider(masterVolumeSlider);

        trackVolumeSlider.onValueChange = [this]
        {
            updateVolumeLabels();

            if (currentProject && trackCombo.getSelectedId() > 0)
                setProjectDirty();
        };
        masterVolumeSlider.onValueChange = [this]
        {
            updateVolumeLabels();

            if (currentProject)
                setProjectDirty();
        };

        muteToggle.onClick = [this]
        {
            if (currentProject && trackCombo.getSelectedId() > 0)
                setProjectDirty();
        };
        soloToggle.onClick = [this]
        {
            if (currentProject && trackCombo.getSelectedId() > 0)
                setProjectDirty();
        };

        populateInstrumentCombo();

        trackCombo.setTextWhenNothingSelected("Import a score first");
        trackCombo.onChange = [this]
        {
            const auto requestedIndex = getSelectedTrackIndex();

            if (!suppressTrackComboSwitchPrompt
                && pianoRollWindow != nullptr
                && pianoRollOpenTrackIndex >= 0
                && requestedIndex >= 0
                && requestedIndex != pianoRollOpenTrackIndex)
            {
                const auto previousIndex = pianoRollOpenTrackIndex;

                suppressTrackComboSwitchPrompt = true;
                trackCombo.setSelectedId(previousIndex + 1, juce::dontSendNotification);
                suppressTrackComboSwitchPrompt = false;

                auto switchToRequestedTrack = [this, requestedIndex]
                {
                    suppressTrackComboSwitchPrompt = true;
                    trackCombo.setSelectedId(requestedIndex + 1, juce::dontSendNotification);
                    suppressTrackComboSwitchPrompt = false;

                    pianoRollOpenTrackIndex = requestedIndex;
                    syncTrackInspectorFromSelection();
                    refreshNoteEditor();
                    syncTrackManagerSelectionFromCurrentTrack(true, requestedIndex >= 0 && !suppressTrackManagerConsoleFollow);
                    syncPianoRollFromSelectedTrack();
                    fitPianoRollToSelectedTrack();
                    clearPianoRollEditorDirty();

                    if (pianoRollWindow != nullptr)
                    {
                        updatePianoRollWindowDirtyIndicator();
                        pianoRollWindow->toFront(true);
                    }

                    logMessage("Switched open Piano Roll to selected track.");
                };

                if (!pianoRollEditorDirty)
                {
                    switchToRequestedTrack();
                    return;
                }

                auto* alert = new juce::AlertWindow(
                    "Switch Piano Roll Track?",
                    "The current Piano Roll has unapplied note edits. Apply them, discard them, or cancel the switch?",
                    juce::AlertWindow::QuestionIcon
                );

                alert->addButton("Apply Changes", 1, juce::KeyPress(juce::KeyPress::returnKey));
                alert->addButton("Discard and Continue", 2);
                alert->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

                alert->enterModalState(
                    true,
                    juce::ModalCallbackFunction::create(
                        [this, alert, switchToRequestedTrack](int result) mutable
                        {
                            std::unique_ptr<juce::AlertWindow> cleanup(alert);

                            if (result == 1)
                            {
                                applyPianoRollEditorChanges();
                            }
                            else if (result == 2)
                            {
                                discardPianoRollEditorChanges();
                            }
                            else
                            {
                                logMessage("Piano Roll track switch cancelled.");
                                return;
                            }

                            switchToRequestedTrack();
                        }
                    )
                );

                return;
            }

            syncTrackInspectorFromSelection();
            syncTrackManagerSelectionFromCurrentTrack(true, requestedIndex >= 0 && !suppressTrackManagerConsoleFollow);
        };

        trackSummaryBox.setMultiLine(true);
        trackSummaryBox.setReadOnly(true);
        trackSummaryBox.setScrollbarsShown(true);

        trackManagerBox.setMultiLine(true);
        trackManagerBox.setReadOnly(true);
        trackManagerBox.setScrollbarsShown(true);
        trackManagerBox.setText("Track Manager will appear here.\n");

        trackManagerSelectBox.setText("1");
        trackManagerSelectBox.setInputRestrictions(4, "0123456789");
        trackManagerStartBeatBox.setText("0");
        trackManagerStartBeatBox.setInputRestrictions(8, "0123456789");
        trackSummaryBox.setText("Import a MusicXML file to see tracks.");

        logBox.setMultiLine(true);
        logBox.setReadOnly(true);
        logBox.setScrollbarsShown(true);
        logBox.setColour(juce::TextEditor::backgroundColourId, juce::Colours::black);
        logBox.setColour(juce::TextEditor::textColourId, juce::Colours::white);
        logBox.setColour(juce::TextEditor::outlineColourId, juce::Colours::grey);
        logBox.setColour(juce::TextEditor::focusedOutlineColourId, juce::Colours::yellow);
        trackManagerBox.setColour(juce::TextEditor::backgroundColourId, juce::Colours::black);
        trackManagerBox.setColour(juce::TextEditor::textColourId, juce::Colours::white);
        trackManagerBox.setColour(juce::TextEditor::outlineColourId, juce::Colours::grey);
        trackManagerBox.setTooltip({});
        logBox.setTooltip({});
        logBox.setText("Ready.\n");

        chooseMusicXmlButton.onClick = [this] { chooseMusicXml(); };
        importAudioButton.onClick = [this] { importAudioFile(); };
        recordAudioButton.onClick = [this] { openAudioRecorderWindow(); };
        newProjectButton.onClick = [this] { startNewProject(); };
        openProjectButton.onClick = [this] { openProjectFile(); };
        saveProjectButton.onClick = [this] { saveCurrentProjectFile(); };
        cleanTempButton.onClick = [this] { cleanTempFolder(); };
        saveSettingsButton.onClick = [this] { saveUserSettingsNow(); };
        openVstPluginButton.onClick = [this] { openSelectedTrackVstPluginUi(); };
        editInfoButton.onClick = [this] { openProjectInfoWindow(); };
        exportFolderButton.onClick = [this] { chooseExportFolder(); };
        refreshSoundFontsButton.onClick = [this]
        {
            refreshSoundFontList();
            refreshPresetListFromSelectedSoundFont();
            populateInstrumentCombo();
        };
        browseSoundFontButton.onClick = [this] { chooseSoundFont(); };
        sfzButton.onClick = [this] { chooseSfzFile(); };
        refreshSfzButton.onClick = [this]
        {
            refreshSfzList();
            populateInstrumentCombo();
        };
        sfzCombo.onChange = [this]
        {
            const auto selected = getSelectedSfzPath();

            if (!selected.empty())
            {
                sfzPathBox.setText(selected.string(), juce::dontSendNotification);
                populateInstrumentCombo();
                logMessage("Selected detected SFZ: " + selected.string());

                if (currentProject)
                    setProjectDirty();
            }
        };
        addTrackButton.onClick = [this] { addManualTrack(); };
        duplicateTrackButton.onClick = [this] { duplicateSelectedTrack(); };
        removeTrackButton.onClick = [this] { removeSelectedTrack(); };
        renameTrackButton.onClick = [this] { renameSelectedTrack(); };
        trackManagerButton.onClick = [this] { openTrackManagerWindow(); };
        applyTimingButton.onClick = [this] { applyProjectTimingSettings(); };
        applyNotesButton.onClick = [this] { applyNoteEditorToTrack(); };
        showRawNotesButton.onClick = [this] { openRawNotesWindow(); };
        addNoteButton.onClick = [this] { addNoteToSelectedTrack(); };
        removeNoteButton.onClick = [this] { removeLastNoteFromSelectedTrack(); };
        deletePianoRollNoteButton.onClick = [this] { pianoRoll.deleteSelectedNote(); };
        syncPianoRollButton.onClick = [this] { applyNoteEditorToTrack(); refreshNoteEditor(); };
        openPianoRollButton.onClick = [this] { openPianoRollWindow(); };

        setBeatWindow4Button.onClick = [this] { pianoRollBeatWindowBox.setText("4"); applyPianoRollSettings(); };
        setBeatWindow8Button.onClick = [this] { pianoRollBeatWindowBox.setText("8"); applyPianoRollSettings(); };
        setBeatWindow16Button.onClick = [this] { pianoRollBeatWindowBox.setText("16"); applyPianoRollSettings(); };
        setBeatWindow32Button.onClick = [this] { pianoRollBeatWindowBox.setText("32"); applyPianoRollSettings(); };
        setBeatWindow64Button.onClick = [this] { pianoRollBeatWindowBox.setText("64"); applyPianoRollSettings(); };
        previousPianoRollWindowButton.onClick = [this] { previousPianoRollWindow(); };
        nextPianoRollWindowButton.onClick = [this] { nextPianoRollWindow(); };
        jumpPianoRollPageButton.onClick = [this] { jumpToPianoRollWindow(); };
        jumpPianoRollKeyButton.onClick = [this] { jumpToPianoRollKey(); };
        pianoRollKeyJumpBox.onReturnKey = [this] { jumpToPianoRollKey(); };
        pianoRollNotesDownButton.onClick = [this]
        {
            pianoRoll.nudgeVisiblePitchRange(-12);
            updatePianoRollKeyRangeLabel();
            logMessage("Piano Roll notes down: " + pianoRoll.getVisiblePitchRangeText());
        };
        pianoRollNotesUpButton.onClick = [this]
        {
            pianoRoll.nudgeVisiblePitchRange(12);
            updatePianoRollKeyRangeLabel();
            logMessage("Piano Roll notes up: " + pianoRoll.getVisiblePitchRangeText());
        };
        copyPianoRollNoteButton.onClick = [this] { pianoRoll.copySelectedNote(); logMessage("Copied selected piano roll note."); };
        pastePianoRollNoteButton.onClick = [this] { pianoRoll.pasteCopiedNote(); logMessage("Pasted copied piano roll note."); };
        undoPianoRollButton.onClick = [this] { pianoRoll.undoLastNoteEdit(); logMessage("Undo piano roll note edit."); };
        redoPianoRollButton.onClick = [this] { pianoRoll.redoLastNoteEdit(); logMessage("Redo piano roll note edit."); };
        clearPianoRollSelectionButton.onClick = [this] { pianoRoll.clearNoteSelection(); logMessage("Cleared piano roll selection."); };
        openPianoRollPreviewPlayerButton.onClick = [this] { openPianoRollPreviewPlayerWindow(); };
        playProjectPreviewButton.onClick = [this] { playProjectPreview(); };
        stopProjectPreviewButton.onClick = [this] { stopProjectPreview(); };

        pianoRoll.onNotesChanged = [this](const std::vector<mw::core::NoteEvent>& notes)
        {
            if (!suppressPianoRollEditorDirty)
                markPianoRollEditorDirty();

            const auto selectedIndex = getSelectedTrackIndex();
            const auto targetIndex = pianoRollOpenTrackIndex >= 0 ? pianoRollOpenTrackIndex : selectedIndex;

            if (targetIndex == selectedIndex)
                populateNoteEditorFromNotes(notes);
        };

        pianoRoll.onSelectedNoteChanged = [this](int)
        {
            const auto selectedIndex = getSelectedTrackIndex();
            const auto targetIndex = pianoRollOpenTrackIndex >= 0 ? pianoRollOpenTrackIndex : selectedIndex;

            if (targetIndex == selectedIndex)
                populateNoteEditorFromNotes(pianoRoll.getNotes());
            else
                refreshNoteEditor();
        };

        pianoRoll.onPitchRangeChanged = [this]
        {
            updatePianoRollKeyRangeLabel();
        };

        pianoRoll.onPreviewPlayheadPageChanged = [this](int pageStartBeat)
        {
            const auto safeStartBeat = std::max(0, pageStartBeat);
            pianoRollStartBeatBox.setText(juce::String(safeStartBeat), juce::dontSendNotification);
            pianoRoll.setStartBeat(safeStartBeat);
            updatePianoRollPageIndicator();
            pianoRoll.repaint();
        };
        sfzTestButton.onClick = [this] { createSfzTestMidiAndRender(); };
        applyTrackButton.onClick = [this] { applyTrackInspector(); };
        renderButton.onClick = [this] { renderCurrentProjectOnBackgroundThread(); };
        renderSettingsButton.onClick = [this] { showRenderSettingsWindow(); };
        renderSelectedTrackButton.onClick = [this] { renderSelectedTrackOnBackgroundThread(); };
        renderSelectedSequenceButton.onClick = [this] { renderSelectedSequenceOnBackgroundThread(); };
        renderMidiButton.onClick = [this] { renderMidiCurrentProject(); };
        cancelRenderButton.onClick = [this]
        {
            if (!renderingInProgress)
                return;

            cancelRenderRequested = true;
            logMessage("Cancel requested. Stopping active parallel stem render processes where supported...");
        };
        cancelRenderButton.setEnabled(false);

        addAndMakeVisible(menuBar);
        addAndMakeVisible(titleLabel);
        addAndMakeVisible(renderStatusLabel);
        addAndMakeVisible(renderTargetLabel);

        addAndMakeVisible(musicXmlLabel);
        addAndMakeVisible(exportFolderLabel);
        addAndMakeVisible(projectDefaultsLabel);
        addAndMakeVisible(soundFontLabel);
        addAndMakeVisible(backendLabel);
        addAndMakeVisible(sfzLabel);
        addAndMakeVisible(sfzKeySwitchLabel);
        addAndMakeVisible(sfzCc1Label);
        addAndMakeVisible(sfzCc11Label);
        addAndMakeVisible(baseNameLabel);
        addAndMakeVisible(outputFormatLabel);
        addAndMakeVisible(audioClipFormatLabel);
        addAndMakeVisible(audioClipQualityLabel);
        addAndMakeVisible(sampleRateLabel);
        addAndMakeVisible(bitrateLabel);
        addAndMakeVisible(channelsLabel);
        addAndMakeVisible(renderOutputSummaryLabel);
        addAndMakeVisible(themeLabel);
        addAndMakeVisible(trackLabel);
        addAndMakeVisible(sequenceSelectorLabel);
        addAndMakeVisible(sequenceSelectorBox);
        addAndMakeVisible(changeActiveSequenceButton);
        addAndMakeVisible(sequenceThoughtsLabel);
        addAndMakeVisible(sequenceThoughtsBox);
        addAndMakeVisible(trackSoundLibraryLabel);
        addAndMakeVisible(instrumentLabel);
        addAndMakeVisible(trackVolumeLabel);
        addAndMakeVisible(masterVolumeLabel);
        addAndMakeVisible(noteEditorLabel);
        addAndMakeVisible(pianoRollHelpLabel);
        addAndMakeVisible(pianoRollPageLabel);
        addAndMakeVisible(pianoRollPageInputLabel);
        addAndMakeVisible(pianoRollKeyRangeLabel);
        addAndMakeVisible(pianoRollKeyJumpLabel);

        addAndMakeVisible(musicXmlPathBox);
        addAndMakeVisible(exportFolderBox);
        addAndMakeVisible(sfzCombo);
        addAndMakeVisible(sfzKeySwitchBox);
        addAndMakeVisible(sfzCc1Box);
        addAndMakeVisible(sfzCc11Box);
        addAndMakeVisible(baseNameBox);
        addAndMakeVisible(pianoRollPageBox);
        addAndMakeVisible(pianoRollKeyJumpBox);
        addAndMakeVisible(trackSoundLibraryBox);

        addAndMakeVisible(soundFontCombo);
        addAndMakeVisible(backendCombo);
        addAndMakeVisible(outputFormatCombo);
        addAndMakeVisible(audioClipFormatCombo);
        addAndMakeVisible(audioClipQualityCombo);
        addAndMakeVisible(sampleRateCombo);
        addAndMakeVisible(bitrateCombo);
        addAndMakeVisible(channelsCombo);
        addAndMakeVisible(themeCombo);
        addAndMakeVisible(trackCombo);
        addAndMakeVisible(addTrackButton);
        addAndMakeVisible(duplicateTrackButton);
        addAndMakeVisible(removeTrackButton);
        addAndMakeVisible(renameTrackButton);
        addAndMakeVisible(trackManagerButton);
        addAndMakeVisible(instrumentCombo);

        addAndMakeVisible(muteToggle);
        addAndMakeVisible(soloToggle);

        addAndMakeVisible(trackVolumeSlider);
        addAndMakeVisible(masterVolumeSlider);

        addAndMakeVisible(chooseMusicXmlButton);
        addAndMakeVisible(importAudioButton);
        addAndMakeVisible(recordAudioButton);
        addAndMakeVisible(newProjectButton);
        addAndMakeVisible(openProjectButton);
        addAndMakeVisible(saveProjectButton);
        addAndMakeVisible(cleanTempButton);
        addAndMakeVisible(saveSettingsButton);
        addAndMakeVisible(editInfoButton);
        addAndMakeVisible(exportFolderButton);
        addAndMakeVisible(browseSoundFontButton);
        addAndMakeVisible(refreshSoundFontsButton);
        addAndMakeVisible(sfzButton);
        addAndMakeVisible(refreshSfzButton);
        addAndMakeVisible(sfzTestButton);
        addAndMakeVisible(applyBackendButton);
        addAndMakeVisible(applyTrackButton);
        addAndMakeVisible(changeTrackLibraryButton);
        addAndMakeVisible(openVstPluginButton);
        addAndMakeVisible(renderButton);
        addAndMakeVisible(renderSelectedTrackButton);
        addAndMakeVisible(renderSelectedSequenceButton);
        addAndMakeVisible(renderMidiButton);
        addAndMakeVisible(cancelRenderButton);
        addAndMakeVisible(renderSettingsButton);

        addAndMakeVisible(trackSummaryBox);
        addAndMakeVisible(logBox);
        addAndMakeVisible(openPianoRollButton);
        addAndMakeVisible(previousPianoRollWindowButton);
        addAndMakeVisible(nextPianoRollWindowButton);
        addAndMakeVisible(jumpPianoRollPageButton);
        addAndMakeVisible(jumpPianoRollKeyButton);
        addAndMakeVisible(pianoRollBeatWindowCombo);

        setDefaultPaths();
        updateVolumeLabels();
        updateOpenVstPluginButtonState();
        configureHelperBubbles();
        helperTooltipWindow = std::make_unique<mw::gui::FreshHoverTooltipWindow>(this, 2000);
        helperTooltipWindow->setLookAndFeel(&helperTooltipLookAndFeel);
        setSize(1250, 820);

    }

    MainComponent::~MainComponent()
    {
        if (helperTooltipWindow != nullptr)
            helperTooltipWindow->setLookAndFeel(nullptr);

        helperTooltipWindow.reset();

        menuBar.setModel(nullptr);

        if (rawNotesWindow != nullptr)
        {
            rawNotesWindow->setContentOwned(nullptr, false);
            rawNotesWindow.reset();
        }

        rawNotesContent.reset();

        if (pianoRollWindow != nullptr)
        {
            pianoRollWindow->setContentOwned(nullptr, false);
            pianoRollWindow.reset();
        }

        pianoRollContent.reset();


        cancelRenderRequested = true;
        renderingInProgress = false;

        if (renderThread.joinable())
            renderThread.join();

        cleanupPianoRollPreviewFiles();
    }


    void MainComponent::configureHelperBubbles()
    {
        auto labelAndControl = [](auto& label, auto& control, const juce::String& text)
        {
            label.setTooltip(text);
            control.setTooltip(text);
        };

        chooseMusicXmlButton.setTooltip("Import a MusicXML or MIDI file and start a new project from it.");
        importAudioButton.setTooltip("Import WAV, MP3, FLAC, or OGG audio as a new AudioClip track in the active sequence.");
        recordAudioButton.setTooltip("Open the AudioClip recorder. Kept takes become new AudioClip tracks in the active sequence.");
        newProjectButton.setTooltip("Create a fresh empty project. You will be asked before discarding unsaved work.");
        openProjectButton.setTooltip("Open a saved Poor Man's Studio .mwproj project file.");
        saveProjectButton.setTooltip("Save the current project as a .mwproj file.");
        cleanTempButton.setTooltip("Clean old temporary render and preview files created by the app.");
        saveSettingsButton.setTooltip("Save your current paths, render choices, and app preferences.");
        editInfoButton.setTooltip("Edit project metadata such as title, artist, album, track number, and year.");
        exportFolderButton.setTooltip("Choose the folder where rendered audio and MIDI files will be written.");
        browseSoundFontButton.setTooltip("Choose a SoundFont file or folder for SF2 playback and rendering.");
        refreshSoundFontsButton.setTooltip("Rescan the selected SoundFont location for available presets.");
        sfzButton.setTooltip("Choose an SFZ file or folder for SFZ playback and rendering.");
        refreshSfzButton.setTooltip("Rescan the selected SFZ location for available instruments.");
        sfzTestButton.setTooltip("Render a short test note through the selected SFZ setup.");
        applyBackendButton.setTooltip("Apply the selected project playback backend defaults to the project.");
        applyTrackButton.setTooltip("Apply the selected track instrument, mute/solo, and volume without changing unsaved Piano Roll notes.");
        changeTrackLibraryButton.setTooltip("Change the sample library used by the selected track.");
        openVstPluginButton.setTooltip("Open the selected track's applied VST3 plugin window. Choose a VST3 plugin and click Apply Track Settings first.");
        trackSfzButton.setTooltip("Choose an SFZ instrument specifically for the selected track.");
        addTrackButton.setTooltip("Add a new blank track to the active sequence.");
        duplicateTrackButton.setTooltip("Duplicate the currently selected track.");
        removeTrackButton.setTooltip("Remove the currently selected track from the project.");
        renameTrackButton.setTooltip("Rename the selected track.");
        trackManagerButton.setTooltip("Open Track Manager for sequence layout, track organization, and timeline viewing.");
        applyTimingButton.setTooltip("Apply project tempo, time signature, and loop count from the timing fields.");
        applyNotesButton.setTooltip("Apply the raw note editor contents to the selected track.");
        showRawNotesButton.setTooltip("Open the advanced raw note CSV editor for the selected track.");
        addNoteButton.setTooltip("Add one note from the note editor settings to the selected track.");
        removeNoteButton.setTooltip("Remove the last note from the selected track.");
        deletePianoRollNoteButton.setTooltip("Delete the currently selected Piano Roll note.");
        syncPianoRollButton.setTooltip("Sync note-editor changes into the Piano Roll view.");
        openPianoRollButton.setTooltip("Open the editable Piano Roll for the selected track. Empty tracks are linked to a sequence automatically.");
        previousPianoRollWindowButton.setTooltip("Move the Piano Roll to the previous beat window.");
        nextPianoRollWindowButton.setTooltip("Move the Piano Roll to the next beat window.");
        jumpPianoRollPageButton.setTooltip("Jump the Piano Roll to the window number typed beside it.");
        jumpPianoRollKeyButton.setTooltip("Jump the visible Piano Roll note range to the typed pitch, such as C4 or A5.");
        pianoRollNotesDownButton.setTooltip("Shift the visible Piano Roll note range down one octave.");
        pianoRollNotesUpButton.setTooltip("Shift the visible Piano Roll note range up one octave.");
        copyPianoRollNoteButton.setTooltip("Copy the selected Piano Roll note.");
        pastePianoRollNoteButton.setTooltip("Paste the copied Piano Roll note.");
        undoPianoRollButton.setTooltip("Undo the most recent Piano Roll note edit.");
        redoPianoRollButton.setTooltip("Redo the most recently undone Piano Roll note edit.");
        clearPianoRollSelectionButton.setTooltip("Clear the current Piano Roll note selection.");
        openPianoRollPreviewPlayerButton.setTooltip("Open the Piano Roll Preview Player window.");
        playProjectPreviewButton.setTooltip("Render and play a temporary preview of the current project.");
        stopProjectPreviewButton.setTooltip("Stop the current project preview playback.");
        renderButton.setTooltip("Render the full project to the selected output format.");
        renderSettingsButton.setTooltip("Open render settings for stem-file retention and render-output cleanup behavior.");
        renderSelectedTrackButton.setTooltip("Render only the selected track.");
        renderSelectedSequenceButton.setTooltip("Render only the selected sequence.");
        renderMidiButton.setTooltip("Export the project as MIDI.");
        cancelRenderButton.setTooltip("Request cancellation of the current render job.");
        previewButton.setTooltip("Preview the latest rendered WAV file.");

        musicXmlLabel.setTooltip("Shows the file used to start the project.");
        musicXmlPathBox.setTooltip("Path to the imported source file.");
        labelAndControl(exportFolderLabel, exportFolderBox, "Folder where renders and exports will be saved.");
        projectDefaultsLabel.setTooltip("Default playback/rendering library settings for the whole project.");
        labelAndControl(soundFontLabel, soundFontCombo, "Choose the SoundFont preset list used for SF2 instruments.");
        soundFontPathBox.setTooltip("Current SoundFont file or folder path.");
        labelAndControl(backendLabel, backendCombo, "Choose the project default backend before importing or adding tracks. VST3 exposes scanned instrument plugins as track instruments.");
        labelAndControl(sfzLabel, sfzCombo, "Choose the SFZ instrument used by the project defaults.");
        sfzPathBox.setTooltip("Current SFZ file or folder path.");
        labelAndControl(sfzKeySwitchLabel, sfzKeySwitchBox, "Optional SFZ key switch note value for expression changes.");
        labelAndControl(sfzCc1Label, sfzCc1Box, "Optional SFZ CC1 modulation value.");
        labelAndControl(sfzCc11Label, sfzCc11Box, "Optional SFZ CC11 expression value.");
        labelAndControl(baseNameLabel, baseNameBox, "Base filename used for rendered output files.");
        labelAndControl(outputFormatLabel, outputFormatCombo, "Choose WAV, MP3, FLAC, or another available render format.");
        labelAndControl(audioClipFormatLabel, audioClipFormatCombo, "Choose the saved media format for imported/recorded AudioClips. WAV and FLAC are lossless; MP3 and OGG use the Clip Quality setting.");
        labelAndControl(audioClipQualityLabel, audioClipQualityCombo, "Choose high-quality bitrate for compressed AudioClip files. Lossless formats ignore bitrate.");
        labelAndControl(sampleRateLabel, sampleRateCombo, "Choose the sample rate for audio renders.");
        labelAndControl(bitrateLabel, bitrateCombo, "Choose compressed-audio bitrate when the selected format uses one.");
        labelAndControl(channelsLabel, channelsCombo, "Choose mono or stereo output.");
        renderWorkersCombo.setTooltip("Parallel Stems moved to Render Settings.");
        labelAndControl(themeLabel, themeCombo, "Choose the app colour theme.");
        labelAndControl(sequenceSelectorLabel, sequenceSelectorBox, "Shows the active sequence. Click Chg Seq to switch or create a sequence.");
        labelAndControl(sequenceThoughtsLabel, sequenceThoughtsBox, "Read-only view of the active sequence Thoughts. Edit Thoughts from Track Manager to save them into the .mwproj sequence metadata.");
        changeActiveSequenceButton.setTooltip("Open a sequence-number popup with OK, Create Blank Seq, and Cancel.");
        labelAndControl(trackLabel, trackCombo, "Choose the track to edit, render, preview, or open in Piano Roll.");
        labelAndControl(trackSoundLibraryLabel, trackSoundLibraryBox, "Shows the sample library currently assigned to the selected track.");
        labelAndControl(instrumentLabel, instrumentCombo, "Choose the instrument or preset for the selected track, then click Apply Track Settings.");
        labelAndControl(trackVolumeLabel, trackVolumeSlider, "Set the volume for the selected track.");
        labelAndControl(masterVolumeLabel, masterVolumeSlider, "Set the master output volume for the project.");
        muteToggle.setTooltip("Mute the selected track when previewing or rendering.");
        soloToggle.setTooltip("Solo the selected track when previewing or rendering.");
        labelAndControl(tempoLabel, tempoBox, "Project tempo in beats per minute.");
        labelAndControl(timeSignatureLabel, timeSignatureBox, "Project time signature, such as 4/4 or 3/4.");
        labelAndControl(loopCountLabel, loopCountBox, "Number of times to loop the project for rendering.");
        labelAndControl(pianoRollPageInputLabel, pianoRollPageBox, "Window number to jump to in the Piano Roll.");
        labelAndControl(pianoRollKeyJumpLabel, pianoRollKeyJumpBox, "Pitch name to center in the Piano Roll, such as C4, A5, or B2.");
        pianoRollBeatWindowCombo.setTooltip("Choose how many beats the Piano Roll shows at once.");
        noteEditorLabel.setTooltip("Advanced note-entry fields for the selected track.");
        noteEditorBox.setTooltip("Raw note data for the selected track.");
        trackSummaryBox.setTooltip({});
        trackManagerBox.setTooltip({});
        logBox.setTooltip({});
    }


    void MainComponent::setHelperBubblesEnabled(bool enabled)
    {
        helperBubblesEnabled = enabled;
        mw::gui::setHelperBubblesGloballyEnabled(enabled);

        if (!mw::app::UserPreferencesStore::saveBoolValue("helperBubblesEnabled", enabled))
            logMessage("ERROR: Failed to save helper bubble preference.");

        logMessage(enabled ? "Helper bubbles enabled." : "Helper bubbles disabled.");
        menuBar.repaint();
    }


    void MainComponent::setVstCompatibilityWarningsEnabled(bool enabled)
    {
        vstCompatibilityWarningsEnabled = enabled;

        if (!mw::app::UserPreferencesStore::saveBoolValue("vstCompatibilityWarningsEnabled", enabled))
            logMessage("ERROR: Failed to save VST compatibility warning preference.");

        logMessage(enabled ? "VST compatibility warnings enabled." : "VST compatibility warnings disabled.");
        menuBar.repaint();
    }



    void MainComponent::showVstExperimentalWarningIfNeeded()
    {
        if (vstExperimentalWarningAcknowledged)
            return;

        auto preferences = mw::app::UserPreferencesStore::load();
        if (preferences.vstExperimentalWarningAcknowledged)
        {
            vstExperimentalWarningAcknowledged = true;
            return;
        }

        auto* warningWindow = new VstProjectDefaultDragonWarningWindow(this, [this]
        {
            vstExperimentalWarningAcknowledged = true;

            if (!mw::app::UserPreferencesStore::saveValue("vstExperimentalWarningAcknowledged", "1"))
                logMessage("ERROR: Failed to save VST experimental warning acknowledgement.");
            else
                logMessage("VST experimental warning acknowledged.");
        });

        applyPoorMansStudioWindowIcon(*warningWindow, PoorMansStudioWindowIcon::Caution);
    }



    bool MainComponent::selectedTrackHasAppliedVstPlugin() const
    {
        if (!currentProject)
            return false;

        const auto index = getSelectedTrackIndex();
        if (index < 0 || index >= static_cast<int>(currentProject->getTracks().size()))
            return false;

        const auto& track = currentProject->getTracks()[static_cast<std::size_t>(index)];
        if (track.isAudioClipTrack())
            return false;

        const auto& assignment = track.getInstrument();
        if (hasResolvableVst3BundlePath(assignment))
        {
            if (const auto descriptor = findVstPluginDescriptorForAssignment(detectedVstPlugins, assignment))
                return isSupportedVstInstrumentPlugin(*descriptor);
            return true;
        }

        const int selectedTrackBackendChoice = trackBackendCombo.getSelectedId() > 0 ? trackBackendCombo.getSelectedId() : 1;
        const bool trackUiWantsVst = assignment.backendType == mw::core::SampleBackendType::VST3
            || selectedTrackBackendChoice == 4
            || (selectedTrackBackendChoice == 1 && appliedProjectBackendId == 3);

        if (!trackUiWantsVst)
            return false;

        // Keep the button/menu in sync with the same visible VST3 state used by
        // the Instrument dropdown. If a VST3 plugin is selected in the dropdown
        // or saved as the project default, Open VST Plugin can resolve/apply it.
        const int selectedInstrumentId = instrumentCombo.getSelectedId();
        if (selectedInstrumentId > 0)
        {
            int instrumentPluginId = 1;
            for (const auto& plugin : detectedVstPlugins)
            {
                if (!isSupportedVstInstrumentPlugin(plugin))
                    continue;

                if (instrumentPluginId == selectedInstrumentId && !plugin.bundlePath.empty())
                    return true;

                ++instrumentPluginId;
            }
        }

        return !currentProject->getUserSettings().vst3PluginPath.empty();
    }

    void MainComponent::updateOpenVstPluginButtonState()
    {
        const bool canOpen = selectedTrackHasAppliedVstPlugin();
        openVstPluginButton.setEnabled(canOpen);
        openVstPluginButton.setTooltip(canOpen
            ? juce::String("Open the selected track's VST3 plugin window.")
            : juce::String("Choose a VST3 backend/plugin for the selected track before opening the plugin window."));
        menuBar.repaint();
    }



    void MainComponent::refreshVstGraphicsProfile(bool firstLaunchAutoDetect)
    {
        const auto previousPreferredGpu = vstGraphicsProfile.preferredPluginGpuId.empty()
            ? std::string("auto")
            : vstGraphicsProfile.preferredPluginGpuId;

        vstGraphicsProfile = mw::vst::VstGraphicsProfileDetector::detect(firstLaunchAutoDetect);

        bool previousSelectionStillValid = previousPreferredGpu == "auto";
        for (const auto& adapter : vstGraphicsProfile.adapters)
        {
            if (!adapter.id.empty() && adapter.id == previousPreferredGpu)
            {
                previousSelectionStillValid = true;
                break;
            }
        }

        vstGraphicsProfile.preferredPluginGpuId = previousSelectionStillValid ? previousPreferredGpu : std::string("auto");

        mw::app::UserPreferencesStore::saveValues({
            { "vstGraphicsProfileDetected", vstGraphicsProfile.detected ? "1" : "0" },
            { "vstGraphicsProfileSource", vstGraphicsProfile.source },
            { "vstGraphicsProfileLastDetected", vstGraphicsProfile.lastDetectedLocal },
            { "vstPreferredPluginGpuId", vstGraphicsProfile.preferredPluginGpuId },
            { "vstGraphicsProfileSummary", vstGraphicsProfile.summary() }
        });

        if (!firstLaunchAutoDetect)
            logMessage("VST graphics adapter list refreshed: " + juce::String(vstGraphicsProfile.summary()));
    }

    void MainComponent::scanVstPlugins(bool showSummary)
    {
        mw::vst::VstScanOptions options;
        options.includeWorkspaceFolder = true;
        options.includeSystemFolders = true;
        detectedVstPlugins = mw::vst::VstPluginScanner::scan(options);
        applyVstPluginCatalogRecords(detectedVstPlugins);

        int instruments = 0;
        int warnings = 0;
        for (const auto& plugin : detectedVstPlugins)
        {
            if (plugin.isUsableInstrument())
                ++instruments;
            if (plugin.status == mw::vst::VstPluginScanStatus::Warning)
                ++warnings;
        }

        if (showSummary)
        {
            logMessage("VST3 scan complete. Found " + juce::String(static_cast<int>(detectedVstPlugins.size()))
                + " plugin bundle(s), " + juce::String(instruments) + " instrument candidate(s), "
                + juce::String(warnings) + " compatibility warning(s).");
            populateInstrumentCombo();
        }
    }

    void MainComponent::openVstPluginManagerWindow()
    {
        if (detectedVstPlugins.empty())
            scanVstPlugins(false);

        if (vstPluginManagerWindow != nullptr)
        {
            vstPluginManagerWindow->toFront(true);
            return;
        }

        class VstPluginManagerContent final : public juce::Component
        {
        public:
            explicit VstPluginManagerContent(std::vector<mw::vst::VstPluginDescriptor> pluginsIn)
                : plugins(std::move(pluginsIn)),
                  unavailableModel(*this, false),
                  availableModel(*this, true)
            {
                title.setText("VST3 Plugin Manager", juce::dontSendNotification);
                title.setFont(juce::FontOptions(18.0f, juce::Font::bold));
                title.setJustificationType(juce::Justification::centredLeft);
                addAndMakeVisible(title);

                filterLabel.setText("Filter:", juce::dontSendNotification);
                filterLabel.setJustificationType(juce::Justification::centredLeft);
                filterLabel.setFont(juce::FontOptions(16.0f));
                addAndMakeVisible(filterLabel);

                filterCombo.onChange = [this] { handleFilterChangeRequest(); };
                addAndMakeVisible(filterCombo);

                searchLabel.setText("Search:", juce::dontSendNotification);
                searchLabel.setJustificationType(juce::Justification::centredLeft);
                searchLabel.setFont(juce::FontOptions(16.0f));
                addAndMakeVisible(searchLabel);

                searchBox.setTextToShowWhenEmpty("name, vendor, path, category...", juce::Colours::grey);
                searchBox.setFont(juce::FontOptions(16.0f));
                searchBox.onTextChange = [this] { rebuildLists(); rebuildDetails(); };
                addAndMakeVisible(searchBox);

                unavailableLabel.setText("Not available as instruments", juce::dontSendNotification);
                unavailableLabel.setFont(juce::FontOptions(16.5f, juce::Font::bold));
                unavailableLabel.setJustificationType(juce::Justification::centredLeft);
                addAndMakeVisible(unavailableLabel);

                availableLabel.setText("Available in Instrument dropdown", juce::dontSendNotification);
                availableLabel.setFont(juce::FontOptions(16.5f, juce::Font::bold));
                availableLabel.setJustificationType(juce::Justification::centredLeft);
                addAndMakeVisible(availableLabel);

                unavailableList.setModel(&unavailableModel);
                unavailableList.setMultipleSelectionEnabled(true);
                unavailableList.setRowHeight(pluginRowHeight());
                unavailableList.setColour(juce::ListBox::backgroundColourId, juce::Colours::black.withAlpha(0.20f));
                unavailableList.setTooltip("Plugins hidden from the Instrument dropdown. Select one or more, then use the right arrow to enable them as instruments. Use the horizontal scrollbar for long names, paths, and status text.");
                if (auto* viewport = unavailableList.getViewport())
                    viewport->setScrollBarsShown(true, true);
                addAndMakeVisible(unavailableList);

                availableList.setModel(&availableModel);
                availableList.setMultipleSelectionEnabled(true);
                availableList.setRowHeight(pluginRowHeight());
                availableList.setColour(juce::ListBox::backgroundColourId, juce::Colours::black.withAlpha(0.20f));
                availableList.setTooltip("Plugins currently allowed in the Instrument dropdown. Select one or more, then use the left arrow to remove them from instrument choices. Use the horizontal scrollbar for long names, paths, and status text.");
                if (auto* viewport = availableList.getViewport())
                    viewport->setScrollBarsShown(true, true);
                addAndMakeVisible(availableList);

                moveRightButton.setButtonText("→");
                moveRightButton.setTooltip("Move selected plugins into the Instrument dropdown. Effects/unknowns show a warning but can still be enabled by the user.");
                moveRightButton.onClick = [this] { moveSelectedToAvailable(); };
                addAndMakeVisible(moveRightButton);

                moveLeftButton.setButtonText("←");
                moveLeftButton.setTooltip("Remove selected plugins from the Instrument dropdown.");
                moveLeftButton.onClick = [this] { moveSelectedToUnavailable(); };
                addAndMakeVisible(moveLeftButton);

                applyButton.setButtonText("Apply Changes");
                applyButton.setTooltip("Save pending Plugin Manager changes and refresh the Instrument dropdown.");
                applyButton.onClick = [this] { applyPendingChanges(); };
                addAndMakeVisible(applyButton);

                revertButton.setButtonText("Revert Changes");
                revertButton.setTooltip("Discard pending Plugin Manager changes since the last Apply.");
                revertButton.onClick = [this] { discardPendingChanges(); };
                addAndMakeVisible(revertButton);

                resetOverrideButton.setButtonText("Reset Selected");
                resetOverrideButton.setTooltip("Reset selected plugin overrides back to scanner detection. Pending until Apply Changes.");
                resetOverrideButton.onClick = [this] { resetSelectedOverrides(); };
                addAndMakeVisible(resetOverrideButton);

                clearFailedButton.setButtonText("Clear Failed Block");
                clearFailedButton.setTooltip("Allow a plugin previously blocked by the host to be tried again.");
                clearFailedButton.onClick = [this] { clearSelectedFailedBlocks(); };
                addAndMakeVisible(clearFailedButton);

                rescanButton.setButtonText("Rescan");
                rescanButton.setTooltip("Rescan VST3 plugins. Prompts if there are unapplied Plugin Manager changes.");
                rescanButton.onClick = [this] { requestRescanWithPendingPrompt(); };
                addAndMakeVisible(rescanButton);

                copyInfoButton.setButtonText("Copy Plugin Info");
                copyInfoButton.onClick = [this]
                {
                    juce::SystemClipboard::copyTextToClipboard(details.getText());
                };
                addAndMakeVisible(copyInfoButton);

                details.setMultiLine(true);
                details.setReadOnly(true);
                details.setScrollbarsShown(true);
                details.setCaretVisible(false);
                details.setFont(juce::FontOptions(16.0f));
                details.setTooltip("Details for the highlighted plugin. Select multiple plugins for a bulk-selection summary.");
                addAndMakeVisible(details);

                help.setText("Move plugins between the two panes, then click Apply Changes. Unsupported plugins include effects and MIDI tools. Unknown or misdetected plugins can be user-enabled, but use caution.", juce::dontSendNotification);
                help.setJustificationType(juce::Justification::centredLeft);
                help.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
                help.setFont(juce::FontOptions(15.0f));
                addAndMakeVisible(help);

                rebuildFilterCombo();
                rebuildLists();
                rebuildDetails();
                updatePendingButtons();
            }

            ~VstPluginManagerContent() override
            {
                unavailableList.setModel(nullptr);
                availableList.setModel(nullptr);
            }

            void setPlugins(std::vector<mw::vst::VstPluginDescriptor> pluginsIn)
            {
                plugins = std::move(pluginsIn);
                pendingOverrides.clear();
                rebuildFilterCombo();
                rebuildLists();
                rebuildDetails();
                updatePendingButtons();
            }

            void resized() override
            {
                auto area = getLocalBounds().reduced(14);
                title.setBounds(area.removeFromTop(28));
                area.removeFromTop(8);

                auto filterRow = area.removeFromTop(32);
                filterLabel.setBounds(filterRow.removeFromLeft(55));
                filterCombo.setBounds(filterRow.removeFromLeft(270).reduced(4, 2));
                filterRow.removeFromLeft(16);
                searchLabel.setBounds(filterRow.removeFromLeft(60));
                searchBox.setBounds(filterRow.reduced(4, 2));
                area.removeFromTop(8);

                auto paneArea = area.removeFromTop(320);
                auto leftColumn = paneArea.removeFromLeft((paneArea.getWidth() - 70) / 2);
                auto middleColumn = paneArea.removeFromLeft(70);
                auto rightColumn = paneArea;

                unavailableLabel.setBounds(leftColumn.removeFromTop(24));
                availableLabel.setBounds(rightColumn.removeFromTop(24));
                unavailableList.setBounds(leftColumn);
                availableList.setBounds(rightColumn);
                refreshPluginListContentWidths();

                auto arrowArea = middleColumn.reduced(10, 90);
                moveRightButton.setBounds(arrowArea.removeFromTop(42).reduced(2));
                arrowArea.removeFromTop(10);
                moveLeftButton.setBounds(arrowArea.removeFromTop(42).reduced(2));

                area.removeFromTop(8);
                auto buttonRow = area.removeFromTop(34);
                applyButton.setBounds(buttonRow.removeFromLeft(130).reduced(3, 2));
                revertButton.setBounds(buttonRow.removeFromLeft(130).reduced(3, 2));
                resetOverrideButton.setBounds(buttonRow.removeFromLeft(120).reduced(3, 2));
                clearFailedButton.setBounds(buttonRow.removeFromLeft(140).reduced(3, 2));
                rescanButton.setBounds(buttonRow.removeFromLeft(90).reduced(3, 2));
                copyInfoButton.setBounds(buttonRow.removeFromLeft(135).reduced(3, 2));
                area.removeFromTop(8);

                help.setBounds(area.removeFromTop(42));
                area.removeFromTop(8);
                details.setBounds(area);
            }

            bool requestCloseWithPendingPrompt(std::function<void()> closeAction)
            {
                if (!hasPendingChanges())
                {
                    if (closeAction)
                        closeAction();
                    return true;
                }

                juce::Component::SafePointer<VstPluginManagerContent> safeThis(this);
                juce::AlertWindow::showYesNoCancelBox(juce::AlertWindow::WarningIcon,
                                                       "Unapplied Plugin Manager Changes",
                                                       "You have unapplied Plugin Manager changes. Save changes before closing?",
                                                       "Save",
                                                       "Discard",
                                                       "Cancel",
                                                       nullptr,
                                                       juce::ModalCallbackFunction::create([safeThis, closeAction](int result)
                                                       {
                                                           if (safeThis == nullptr)
                                                               return;

                                                           if (result == 1)
                                                           {
                                                               if (safeThis->applyPendingChanges() && closeAction)
                                                                   closeAction();
                                                           }
                                                           else if (result == 2)
                                                           {
                                                               safeThis->discardPendingChanges();
                                                               if (closeAction)
                                                                   closeAction();
                                                           }
                                                       }));
                return false;
            }

            std::function<bool(const std::vector<std::pair<int, mw::vst::VstPluginUserOverride>>&)> onApplyRequested;
            std::function<void(const std::vector<int>&)> onClearFailedRequested;
            std::function<void()> onRescanRequested;

        private:
            juce::Font pluginListFont() const
            {
                return juce::Font(juce::FontOptions(16.5f));
            }

            int pluginRowHeight() const
            {
                return 38;
            }

            juce::String pluginListRowText(const mw::vst::VstPluginDescriptor& plugin, int pluginIndex) const
            {
                juce::String line = plugin.displayName();
                if (!plugin.vendor.empty())
                    line << "  |  " << plugin.vendor;
                line << "  |  detected: " << mw::vst::vstPluginKindToString(plugin.detectedKind);
                line << "  |  final: " << mw::vst::vstPluginKindToString(plugin.kind);
                line << "  |  " << vstPluginFinalStatusText(plugin);
                if (isPending(pluginIndex))
                    line << "  |  pending";
                if (plugin.status == mw::vst::VstPluginScanStatus::Warning || plugin.compatibility.hasAnyGpuOrUiRisk())
                    line << "  |  warning";
                if (!plugin.category.empty())
                    line << "  |  category: " << plugin.category;
                return line;
            }

            int estimatedPluginListRowWidth(const juce::String& rowText) const
            {
                // JUCE 8 no longer exposes Font::getStringWidth().  The list only
                // needs a conservative virtual content width so the horizontal
                // scrollbar can reveal long plugin names/vendors/status text.
                const auto fontHeight = pluginListFont().getHeight();
                const auto estimatedAverageCharacterWidth = std::max(8.0f, fontHeight * 0.62f);
                return static_cast<int>(std::ceil(static_cast<float>(rowText.length()) * estimatedAverageCharacterWidth)) + 56;
            }

            int pluginListContentWidth(bool availablePane) const
            {
                const auto& indices = availablePane ? availableIndices : unavailableIndices;
                int widest = 1120;
                for (const auto pluginIndex : indices)
                {
                    const auto rowText = pluginListRowText(effectivePlugin(pluginIndex), pluginIndex);
                    widest = std::max(widest, estimatedPluginListRowWidth(rowText));
                }

                return juce::jlimit(1120, 6000, widest);
            }

            void refreshPluginListContentWidths()
            {
                unavailableList.setMinimumContentWidth(pluginListContentWidth(false));
                availableList.setMinimumContentWidth(pluginListContentWidth(true));
            }

            class PluginListModel final : public juce::ListBoxModel
            {
            public:
                PluginListModel(VstPluginManagerContent& ownerIn, bool availablePaneIn)
                    : owner(ownerIn), availablePane(availablePaneIn) {}

                int getNumRows() override
                {
                    return static_cast<int>((availablePane ? owner.availableIndices : owner.unavailableIndices).size());
                }

                void paintListBoxItem(int rowNumber, juce::Graphics& g, int width, int height, bool rowIsSelected) override
                {
                    const auto& indices = availablePane ? owner.availableIndices : owner.unavailableIndices;
                    if (rowNumber < 0 || rowNumber >= static_cast<int>(indices.size()))
                        return;

                    const auto pluginIndex = indices[static_cast<std::size_t>(rowNumber)];
                    const auto plugin = owner.effectivePlugin(pluginIndex);

                    if (rowIsSelected)
                        g.fillAll(juce::Colour(0xff315f9f));
                    else if (rowNumber % 2 == 0)
                        g.fillAll(juce::Colours::white.withAlpha(0.035f));

                    auto bounds = juce::Rectangle<int>(0, 0, width, height).reduced(8, 2);
                    const auto line = owner.pluginListRowText(plugin, pluginIndex);

                    g.setColour(rowIsSelected ? juce::Colours::white : juce::Colours::whitesmoke);
                    g.setFont(owner.pluginListFont());
                    g.drawText(line, bounds, juce::Justification::centredLeft, false);
                }

                juce::String getTooltipForRow(int rowNumber) override
                {
                    const auto& indices = availablePane ? owner.availableIndices : owner.unavailableIndices;
                    if (rowNumber < 0 || rowNumber >= static_cast<int>(indices.size()))
                        return {};
                    return owner.pluginDetailsText(owner.effectivePlugin(indices[static_cast<std::size_t>(rowNumber)]));
                }

                void selectedRowsChanged(int lastRowSelected) override
                {
                    owner.handleListSelectionChanged(availablePane, lastRowSelected);
                }

            private:
                VstPluginManagerContent& owner;
                bool availablePane = false;
            };

            bool hasPendingChanges() const
            {
                return !pendingOverrides.empty();
            }

            bool isPending(int pluginIndex) const
            {
                return pendingOverrides.find(pluginIndex) != pendingOverrides.end();
            }

            mw::vst::VstPluginDescriptor effectivePlugin(int pluginIndex) const
            {
                auto plugin = plugins[static_cast<std::size_t>(pluginIndex)];
                const auto it = pendingOverrides.find(pluginIndex);
                if (it == pendingOverrides.end())
                    return plugin;

                plugin.userOverride = it->second;
                if (it->second == mw::vst::VstPluginUserOverride::TreatAsInstrument)
                {
                    plugin.kind = mw::vst::VstPluginKind::Instrument;
                    plugin.classificationReason += " Pending override: Treat as Instrument.";
                }
                else if (it->second == mw::vst::VstPluginUserOverride::TreatAsUnsupported)
                {
                    plugin.kind = plugin.detectedKind;
                    plugin.classificationReason += " Pending override: Treat as Unsupported.";
                }
                else
                {
                    plugin.kind = plugin.detectedKind;
                    plugin.classificationReason += " Pending override: Reset to detected.";
                }
                return plugin;
            }

            void rebuildFilterCombo()
            {
                const auto previousText = filterCombo.getText();
                suppressFilterChange = true;
                filterCombo.clear(juce::dontSendNotification);
                filterNames.clear();
                filterNames.push_back("All Plugins");
                filterNames.push_back("Supported Instruments");
                filterNames.push_back("Unsupported Plugins");
                filterNames.push_back("Unsupported Instruments");
                filterNames.push_back("Unknown Plugins");
                filterNames.push_back("Failed / Blocked Plugins");
                filterNames.push_back("User-Enabled Instruments");

                int selectedId = activeFilterId > 0 ? activeFilterId : 1;
                for (int i = 0; i < static_cast<int>(filterNames.size()); ++i)
                {
                    filterCombo.addItem(filterNames[static_cast<std::size_t>(i)], i + 1);
                    if (previousText.isNotEmpty() && filterNames[static_cast<std::size_t>(i)] == previousText)
                        selectedId = i + 1;
                }
                selectedId = juce::jlimit(1, static_cast<int>(filterNames.size()), selectedId);
                activeFilterId = selectedId;
                filterCombo.setSelectedId(activeFilterId, juce::dontSendNotification);
                suppressFilterChange = false;
            }

            juce::String currentFilterName() const
            {
                if (activeFilterId <= 0 || activeFilterId > static_cast<int>(filterNames.size()))
                    return "All Plugins";
                return filterNames[static_cast<std::size_t>(activeFilterId - 1)];
            }

            bool pluginMatchesSearch(const mw::vst::VstPluginDescriptor& plugin) const
            {
                const auto query = searchBox.getText().trim().toLowerCase();
                if (query.isEmpty())
                    return true;

                juce::String haystack;
                haystack << plugin.displayName() << " " << plugin.vendor << " " << plugin.version << " "
                         << plugin.reportedCategory << " " << plugin.reportedClassInfo << " "
                         << plugin.reportedDescriptiveName << " " << plugin.reportedIdentifier << " "
                         << plugin.uid << " " << plugin.bundlePath.string();
                return haystack.toLowerCase().contains(query);
            }

            bool pluginMatchesFilter(const mw::vst::VstPluginDescriptor& plugin) const
            {
                const auto filter = currentFilterName();
                if (filter == "All Plugins")
                    return true;
                if (filter == "User-Enabled Instruments")
                    return plugin.userOverride == mw::vst::VstPluginUserOverride::TreatAsInstrument;
                return vstPluginGroupName(plugin) == filter;
            }

            void rebuildLists()
            {
                unavailableIndices.clear();
                availableIndices.clear();

                for (int i = 0; i < static_cast<int>(plugins.size()); ++i)
                {
                    const auto plugin = effectivePlugin(i);
                    if (!pluginMatchesSearch(plugin) || !pluginMatchesFilter(plugin))
                        continue;

                    if (isSupportedVstInstrumentPlugin(plugin))
                        availableIndices.push_back(i);
                    else
                        unavailableIndices.push_back(i);
                }

                refreshPluginListContentWidths();
                unavailableList.updateContent();
                availableList.updateContent();
                unavailableList.repaint();
                availableList.repaint();
                updatePendingButtons();
            }

            void handleListSelectionChanged(bool availablePane, int lastRowSelected)
            {
                if (suppressSelectionChange)
                    return;

                if (lastRowSelected >= 0)
                {
                    lastSelectionWasAvailable = availablePane;
                    suppressSelectionChange = true;
                    if (availablePane)
                        unavailableList.deselectAllRows();
                    else
                        availableList.deselectAllRows();
                    suppressSelectionChange = false;
                }

                rebuildDetails();
            }

            std::vector<int> selectedPluginIndices(bool availablePane) const
            {
                const auto& indices = availablePane ? availableIndices : unavailableIndices;
                const auto& list = availablePane ? availableList : unavailableList;
                std::vector<int> selected;
                for (int row = 0; row < static_cast<int>(indices.size()); ++row)
                    if (list.isRowSelected(row))
                        selected.push_back(indices[static_cast<std::size_t>(row)]);
                return selected;
            }

            std::vector<int> currentSelectedPluginIndices() const
            {
                auto selected = selectedPluginIndices(lastSelectionWasAvailable);
                if (!selected.empty())
                    return selected;

                selected = selectedPluginIndices(!lastSelectionWasAvailable);
                return selected;
            }

            void applyOverrideToSelection(const std::vector<int>& selected, mw::vst::VstPluginUserOverride overrideValue)
            {
                for (const auto pluginIndex : selected)
                    pendingOverrides[pluginIndex] = overrideValue;

                rebuildLists();
                rebuildDetails();
                updatePendingButtons();
            }

            void moveSelectedToAvailable()
            {
                const auto selected = selectedPluginIndices(false);
                if (selected.empty())
                    return;

                int riskyCount = 0;
                int failedCount = 0;
                for (const auto pluginIndex : selected)
                {
                    const auto plugin = effectivePlugin(pluginIndex);
                    if (plugin.failedByHost || plugin.status == mw::vst::VstPluginScanStatus::Failed)
                        ++failedCount;
                    if (plugin.detectedKind != mw::vst::VstPluginKind::Instrument
                        || plugin.compatibility.hasAnyGpuOrUiRisk())
                        ++riskyCount;
                }

                if (failedCount > 0)
                {
                    juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                                           "Plugin Is Blocked",
                                                           "One or more selected plugins are blocked after a previous failure. Clear the failed block before enabling them as instruments.");
                    return;
                }

                if (riskyCount > 0)
                {
                    juce::String message;
                    message << "One or more selected plugins were not safely detected as normal instruments, or have compatibility warnings.\n\n";
                    message << "Effects, MIDI tools, panners, EQs, compressors, reverbs, and delays usually do not produce sound from Piano Roll notes and may behave incorrectly or crash when used as instruments.\n\n";
                    message << "Move the selected plugin(s) into the Instrument dropdown anyway?";

                    juce::Component::SafePointer<VstPluginManagerContent> safeThis(this);
                    juce::AlertWindow::showOkCancelBox(juce::AlertWindow::WarningIcon,
                                                        "Enable Unsupported VST3 Plugin",
                                                        message,
                                                        "Move Anyway",
                                                        "Cancel",
                                                        nullptr,
                                                        juce::ModalCallbackFunction::create([safeThis, selected](int result)
                                                        {
                                                            if (safeThis != nullptr && result != 0)
                                                                safeThis->applyOverrideToSelection(selected, mw::vst::VstPluginUserOverride::TreatAsInstrument);
                                                        }));
                    return;
                }

                applyOverrideToSelection(selected, mw::vst::VstPluginUserOverride::TreatAsInstrument);
            }

            void moveSelectedToUnavailable()
            {
                const auto selected = selectedPluginIndices(true);
                if (selected.empty())
                    return;

                applyOverrideToSelection(selected, mw::vst::VstPluginUserOverride::TreatAsUnsupported);
            }

            void resetSelectedOverrides()
            {
                const auto selected = currentSelectedPluginIndices();
                if (selected.empty())
                    return;

                applyOverrideToSelection(selected, mw::vst::VstPluginUserOverride::None);
            }

            void clearSelectedFailedBlocks()
            {
                const auto selected = currentSelectedPluginIndices();
                if (selected.empty() || !onClearFailedRequested)
                    return;

                onClearFailedRequested(selected);
            }

            bool applyPendingChanges()
            {
                if (pendingOverrides.empty())
                    return true;

                std::vector<std::pair<int, mw::vst::VstPluginUserOverride>> changes;
                for (const auto& [pluginIndex, overrideValue] : pendingOverrides)
                    changes.push_back({ pluginIndex, overrideValue });

                if (!onApplyRequested || !onApplyRequested(changes))
                    return false;

                pendingOverrides.clear();
                rebuildLists();
                rebuildDetails();
                updatePendingButtons();
                return true;
            }

            void discardPendingChanges()
            {
                pendingOverrides.clear();
                rebuildLists();
                rebuildDetails();
                updatePendingButtons();
            }

            void requestRescanWithPendingPrompt()
            {
                if (!hasPendingChanges())
                {
                    if (onRescanRequested)
                        onRescanRequested();
                    return;
                }

                juce::Component::SafePointer<VstPluginManagerContent> safeThis(this);
                juce::AlertWindow::showYesNoCancelBox(juce::AlertWindow::WarningIcon,
                                                       "Unapplied Plugin Manager Changes",
                                                       "You have unapplied Plugin Manager changes. Save changes before rescanning?",
                                                       "Save",
                                                       "Discard",
                                                       "Cancel",
                                                       nullptr,
                                                       juce::ModalCallbackFunction::create([safeThis](int result)
                                                       {
                                                           if (safeThis == nullptr)
                                                               return;

                                                           if (result == 1)
                                                           {
                                                               if (safeThis->applyPendingChanges() && safeThis->onRescanRequested)
                                                                   safeThis->onRescanRequested();
                                                           }
                                                           else if (result == 2)
                                                           {
                                                               safeThis->discardPendingChanges();
                                                               if (safeThis->onRescanRequested)
                                                                   safeThis->onRescanRequested();
                                                           }
                                                       }));
            }

            void handleFilterChangeRequest()
            {
                if (suppressFilterChange)
                    return;

                const int requestedFilterId = filterCombo.getSelectedId();
                if (requestedFilterId == activeFilterId)
                    return;

                if (!hasPendingChanges())
                {
                    activeFilterId = requestedFilterId;
                    rebuildLists();
                    rebuildDetails();
                    return;
                }

                suppressFilterChange = true;
                filterCombo.setSelectedId(activeFilterId, juce::dontSendNotification);
                suppressFilterChange = false;

                juce::Component::SafePointer<VstPluginManagerContent> safeThis(this);
                juce::AlertWindow::showYesNoCancelBox(juce::AlertWindow::WarningIcon,
                                                       "Unapplied Plugin Manager Changes",
                                                       "You have unapplied Plugin Manager changes. Save changes before changing filters?",
                                                       "Save",
                                                       "Discard",
                                                       "Cancel",
                                                       nullptr,
                                                       juce::ModalCallbackFunction::create([safeThis, requestedFilterId](int result)
                                                       {
                                                           if (safeThis == nullptr)
                                                               return;

                                                           if (result == 1)
                                                           {
                                                               if (!safeThis->applyPendingChanges())
                                                                   return;
                                                           }
                                                           else if (result == 2)
                                                           {
                                                               safeThis->discardPendingChanges();
                                                           }
                                                           else
                                                           {
                                                               return;
                                                           }

                                                           safeThis->suppressFilterChange = true;
                                                           safeThis->activeFilterId = requestedFilterId;
                                                           safeThis->filterCombo.setSelectedId(requestedFilterId, juce::dontSendNotification);
                                                           safeThis->suppressFilterChange = false;
                                                           safeThis->rebuildLists();
                                                           safeThis->rebuildDetails();
                                                       }));
            }

            void updatePendingButtons()
            {
                const bool pending = hasPendingChanges();
                applyButton.setEnabled(pending);
                revertButton.setEnabled(pending);
                moveRightButton.setEnabled(!selectedPluginIndices(false).empty());
                moveLeftButton.setEnabled(!selectedPluginIndices(true).empty());
                resetOverrideButton.setEnabled(!currentSelectedPluginIndices().empty());
                clearFailedButton.setEnabled(!currentSelectedPluginIndices().empty());
            }

            juce::String pluginDetailsText(const mw::vst::VstPluginDescriptor& plugin) const
            {
                juce::String text;
                text << "Name: " << plugin.displayName() << "\n";
                text << "Vendor: " << (plugin.vendor.empty() ? "Unknown" : plugin.vendor) << "\n";
                text << "Version: " << (plugin.version.empty() ? "Unknown" : plugin.version) << "\n";
                text << "Format: VST3\n";
                text << "Filter group: " << vstPluginGroupName(plugin) << "\n\n";

                text << "Detected kind: " << mw::vst::vstPluginKindToString(plugin.detectedKind) << "\n";
                text << "User override: " << mw::vst::vstPluginUserOverrideToString(plugin.userOverride) << "\n";
                text << "Final kind: " << mw::vst::vstPluginKindToString(plugin.kind) << "\n";
                text << "Instrument dropdown: " << (isSupportedVstInstrumentPlugin(plugin) ? "Shown" : "Hidden") << "\n";
                text << "Final status: " << vstPluginFinalStatusText(plugin) << "\n";
                text << "Scan status: " << mw::vst::vstPluginScanStatusToString(plugin.status) << "\n";
                if (!plugin.statusMessage.empty())
                    text << "Status message: " << plugin.statusMessage << "\n";
                if (plugin.failedByHost)
                    text << "Failure: " << (plugin.failureMessage.empty() ? "Blocked after a previous host failure." : plugin.failureMessage) << "\n";

                text << "\nReported metadata:\n";
                text << "  Category: " << (plugin.reportedCategory.empty() ? "Unknown/empty" : plugin.reportedCategory) << "\n";
                text << "  Class info: " << (plugin.reportedClassInfo.empty() ? "Unknown/empty" : plugin.reportedClassInfo) << "\n";
                text << "  Descriptive name: " << (plugin.reportedDescriptiveName.empty() ? "Unknown/empty" : plugin.reportedDescriptiveName) << "\n";
                text << "  Format: " << (plugin.reportedFormat.empty() ? "VST3" : plugin.reportedFormat) << "\n";
                text << "  Identifier: " << (plugin.reportedIdentifier.empty() ? "Unknown/empty" : plugin.reportedIdentifier) << "\n";
                text << "  UID: " << (plugin.uid.empty() ? "Unknown/empty" : plugin.uid) << "\n";
                text << "  JUCE description available: " << (plugin.juceDescriptionAvailable ? "Yes" : "No") << "\n";
                text << "  JUCE isInstrument: " << (plugin.juceDescriptionAvailable ? (plugin.juceReportedInstrument ? "true" : "false") : "Unknown") << "\n";
                text << "  Audio inputs: " << (plugin.reportedAudioInputs >= 0 ? juce::String(plugin.reportedAudioInputs) : juce::String("Unknown")) << "\n";
                text << "  Audio outputs: " << (plugin.reportedAudioOutputs >= 0 ? juce::String(plugin.reportedAudioOutputs) : juce::String("Unknown")) << "\n";
                text << "  Has custom editor: checked when opening the plugin. No-editor plugins use fallback parameter sliders.\n";

                text << "\nClassification reason:\n";
                text << "  " << (plugin.classificationReason.empty() ? "No classification reason recorded." : plugin.classificationReason) << "\n";

                text << "\nCompatibility:\n";
                text << "  UI/GPU indicators: " << plugin.compatibility.summary() << "\n";
                if (plugin.compatibility.hasAnyGpuOrUiRisk())
                    text << "  Warning: this plugin may use graphics/UI paths that can freeze or crash in-process hosts. Safe Plugin UI Mode is recommended if it misbehaves.\n";

                text << "\nPaths:\n";
                text << "  Bundle: " << plugin.bundlePath.string() << "\n";
                text << "  Binary: " << (plugin.binaryPath.empty() ? "Unknown" : plugin.binaryPath.string()) << "\n";

                text << "\nUser decision guide:\n";
                text << "  Instruments/synths/samplers/drum plugins should report isInstrument=true or clearly accept MIDI notes and output audio.\n";
                text << "  Unsupported Plugins contains effects, MIDI tools, utility/context plugins, panners, EQs, compressors, reverbs, and delays.\n";
                text << "  Unknown plugins are hidden by default. Move them right only if you know they are note-producing instruments.\n";
                text << "  If Piano Roll Preview renders silence, check that this plugin is a real instrument and not an effect/utility.\n";
                return text;
            }

            void rebuildDetails()
            {
                const auto selected = currentSelectedPluginIndices();
                if (selected.empty())
                {
                    details.setText(plugins.empty()
                        ? "No VST3 plugins found. Place .vst3 bundles in workspace\\vst3 or install them in the system VST3 folder, then scan again."
                        : "Select a plugin to view details.", juce::dontSendNotification);
                    updatePendingButtons();
                    return;
                }

                if (selected.size() > 1)
                {
                    juce::String text;
                    text << "Multiple plugins selected\n";
                    text << "Selected count: " << static_cast<int>(selected.size()) << "\n\n";
                    text << "Available bulk actions:\n";
                    text << "  → Move selected plugins into the Instrument dropdown. Warnings are shown for unsupported or risky plugins.\n";
                    text << "  ← Remove selected plugins from the Instrument dropdown.\n";
                    text << "  Reset Selected returns selected plugins to scanner detection.\n";
                    text << "  Clear Failed Block allows blocked plugins to be tried again.\n";
                    details.setText(text, juce::dontSendNotification);
                    updatePendingButtons();
                    return;
                }

                const auto pluginIndex = selected.front();
                details.setText(pluginDetailsText(effectivePlugin(pluginIndex)), juce::dontSendNotification);
                updatePendingButtons();
            }

            std::vector<mw::vst::VstPluginDescriptor> plugins;
            std::vector<juce::String> filterNames;
            std::vector<int> unavailableIndices;
            std::vector<int> availableIndices;
            std::map<int, mw::vst::VstPluginUserOverride> pendingOverrides;
            int activeFilterId = 1;
            bool suppressFilterChange = false;
            bool suppressSelectionChange = false;
            bool lastSelectionWasAvailable = false;

            PluginListModel unavailableModel;
            PluginListModel availableModel;
            juce::Label title;
            juce::Label filterLabel;
            juce::ComboBox filterCombo;
            juce::Label searchLabel;
            juce::TextEditor searchBox;
            juce::Label unavailableLabel;
            juce::Label availableLabel;
            juce::ListBox unavailableList;
            juce::ListBox availableList;
            juce::TextButton moveRightButton;
            juce::TextButton moveLeftButton;
            juce::TextButton applyButton;
            juce::TextButton revertButton;
            juce::TextButton resetOverrideButton;
            juce::TextButton clearFailedButton;
            juce::TextButton rescanButton;
            juce::TextButton copyInfoButton;
            juce::Label help;
            juce::TextEditor details;
        };

        auto* content = new VstPluginManagerContent(detectedVstPlugins);
        juce::Component::SafePointer<VstPluginManagerContent> safeContent(content);

        content->onApplyRequested = [this, safeContent](const std::vector<std::pair<int, mw::vst::VstPluginUserOverride>>& changes) -> bool
        {
            if (changes.empty())
                return true;

            bool ok = true;
            int applied = 0;
            for (const auto& [pluginIndex, overrideValue] : changes)
            {
                if (pluginIndex < 0 || pluginIndex >= static_cast<int>(detectedVstPlugins.size()))
                {
                    ok = false;
                    continue;
                }

                const auto plugin = detectedVstPlugins[static_cast<std::size_t>(pluginIndex)];
                if (!updateVstPluginCatalogRecord(plugin, overrideValue, std::nullopt, {}))
                {
                    ok = false;
                    logMessage("ERROR: Failed to save VST3 plugin override for " + juce::String(plugin.displayName()) + ".");
                    continue;
                }

                ++applied;
                logMessage(juce::String("VST3 Plugin Manager: ") + juce::String(plugin.displayName())
                    + " override set to " + juce::String(mw::vst::vstPluginUserOverrideToString(overrideValue)) + ".");
            }

            detectedVstPlugins = mw::vst::VstPluginScanner::scan({});
            applyVstPluginCatalogRecords(detectedVstPlugins);
            if (safeContent != nullptr)
                safeContent->setPlugins(detectedVstPlugins);
            populateInstrumentCombo();
            updateOpenVstPluginButtonState();

            if (applied > 0)
                logMessage("VST3 Plugin Manager: applied " + juce::String(applied) + " pending change(s).");
            return ok;
        };

        content->onClearFailedRequested = [this, safeContent](const std::vector<int>& pluginIndices)
        {
            if (pluginIndices.empty())
                return;

            int cleared = 0;
            for (const auto pluginIndex : pluginIndices)
            {
                if (pluginIndex < 0 || pluginIndex >= static_cast<int>(detectedVstPlugins.size()))
                    continue;

                const auto plugin = detectedVstPlugins[static_cast<std::size_t>(pluginIndex)];
                if (!updateVstPluginCatalogRecord(plugin, std::nullopt, false, {}))
                {
                    logMessage("ERROR: Failed to clear VST3 plugin failed state for " + juce::String(plugin.displayName()) + ".");
                    continue;
                }

                ++cleared;
                logMessage(juce::String("VST3 Plugin Manager: cleared failed block for ") + juce::String(plugin.displayName()) + ".");
            }

            detectedVstPlugins = mw::vst::VstPluginScanner::scan({});
            applyVstPluginCatalogRecords(detectedVstPlugins);
            if (safeContent != nullptr)
                safeContent->setPlugins(detectedVstPlugins);
            populateInstrumentCombo();
            updateOpenVstPluginButtonState();

            if (cleared > 0)
                logMessage("VST3 Plugin Manager: cleared " + juce::String(cleared) + " failed block(s).");
        };

        content->onRescanRequested = [this, safeContent]
        {
            scanVstPlugins(true);
            if (safeContent != nullptr)
                safeContent->setPlugins(detectedVstPlugins);
        };

        auto window = std::make_unique<PianoRollDocumentWindow>("VST3 Plugin Manager", [this, safeContent]
        {
            if (safeContent != nullptr)
            {
                safeContent->requestCloseWithPendingPrompt([this] { vstPluginManagerWindow = nullptr; });
                return;
            }

            vstPluginManagerWindow = nullptr;
        });
        window->setResizable(true, true);
        window->setContentOwned(content, true);
        window->centreWithSize(1360, 860);
        window->setVisible(true);
        applyPoorMansStudioWindowIcon(*window, PoorMansStudioWindowIcon::VSTPlugin);
        vstPluginManagerWindow = std::move(window);
    }

    void MainComponent::openVstSettingsWindow()
    {
        if (vstSettingsWindow != nullptr)
        {
            vstSettingsWindow->toFront(true);
            return;
        }

        const auto savedPreferredVstGpuId = vstGraphicsProfile.preferredPluginGpuId.empty()
            ? std::string("auto")
            : vstGraphicsProfile.preferredPluginGpuId;
        if (savedPreferredVstGpuId != "auto" && vstGraphicsProfile.adapters.empty())
            refreshVstGraphicsProfile(true);

        auto* content = new VstSettingsContent(vstGraphicsProfile, vstCompatibilityWarningsEnabled, vstSafePluginUiMode, vstWarningStyleId, vstMaxOpenPluginWindows);
        content->onPreferredGpuChanged = [this, content](const std::string& preferredGpuId)
        {
            vstGraphicsProfile.preferredPluginGpuId = preferredGpuId.empty() ? std::string("auto") : preferredGpuId;

            if (!mw::app::UserPreferencesStore::saveValues({
                    { "vstPreferredPluginGpuId", vstGraphicsProfile.preferredPluginGpuId },
                    { "vstGraphicsProfileDetected", vstGraphicsProfile.detected ? "1" : "0" },
                    { "vstGraphicsProfileSource", vstGraphicsProfile.source },
                    { "vstGraphicsProfileLastDetected", vstGraphicsProfile.lastDetectedLocal },
                    { "vstGraphicsProfileSummary", vstGraphicsProfile.summary() }
                }))
                logMessage("ERROR: Failed to save VST plugin graphics adapter preference.");
            else
                logMessage("VST plugin graphics adapter set to: " + juce::String(vstGraphicsProfile.preferredPluginGpuId));

            if (content != nullptr)
                content->setGraphicsProfile(vstGraphicsProfile);
        };

        content->onMaxOpenPluginWindowsChanged = [this](int maxOpenPluginWindows)
        {
            vstMaxOpenPluginWindows = sanitizeMaxOpenVstPluginWindows(maxOpenPluginWindows);

            if (!mw::app::UserPreferencesStore::saveIntValue("vstMaxOpenPluginWindows", vstMaxOpenPluginWindows))
                logMessage("ERROR: Failed to save maximum VST plugin window preference.");
            else
                logMessage("Maximum open VST plugin windows set to: " + juce::String(vstMaxOpenPluginWindows));
        };

        content->onSafePluginUiModeChanged = [this](bool enabled)
        {
            vstSafePluginUiMode = enabled;

            if (!mw::app::UserPreferencesStore::saveBoolValue("vstSafePluginUiMode", enabled))
                logMessage("ERROR: Failed to save Safe Plugin UI Mode preference.");
            else
                logMessage(enabled ? "Safe Plugin UI Mode enabled." : "Safe Plugin UI Mode disabled.");
        };

        content->onRefreshRequested = [this, content]
        {
            refreshVstGraphicsProfile(false);
            if (content != nullptr)
                content->setGraphicsProfile(vstGraphicsProfile);
        };

        auto window = std::make_unique<PianoRollDocumentWindow>("VST3 Settings", [this] { vstSettingsWindow = nullptr; });
        window->setResizable(true, true);
        window->setContentOwned(content, true);
        window->centreWithSize(760, 520);
        window->setVisible(true);
        applyPoorMansStudioWindowIcon(*window, PoorMansStudioWindowIcon::VSTPlugin);
        vstSettingsWindow = std::move(window);
    }

    void MainComponent::assignSelectedTrackVstPlugin(const mw::vst::VstPluginDescriptor& descriptor)
    {
        if (!currentProject)
            return;

        const auto index = getSelectedTrackIndex();
        if (index < 0 || index >= static_cast<int>(currentProject->getTracks().size()))
            return;

        auto& track = currentProject->getTracks()[static_cast<std::size_t>(index)];
        if (track.isAudioClipTrack())
            return;

        closeVstPluginWindowForTrack(index, "Closed open VST plugin window before replacing the track plugin assignment.");

        showVstExperimentalWarningIfNeeded();

        auto assignment = track.getInstrument();
        applyVstPluginDescriptorToAssignment(assignment, descriptor);

        track.setInstrumentAssignment(assignment);
        trackBackendCombo.setSelectedId(4, juce::dontSendNotification);
        refreshTrackSoundLibraryDisplay();
        updateOpenVstPluginButtonState();
        populateInstrumentCombo();
        updateTrackSummary(*currentProject);
        refreshTrackManagerText();
        refreshOpenPianoRollInstrumentControls();
        setProjectDirty();
        logMessage("Assigned VST3 plugin to " + getTrackDisplayName(index) + ": " + descriptor.displayName());

        const auto warning = mw::vst::makeCompatibilityWarning(descriptor, vstGraphicsProfile, vstWarningStyleId == 2);
        if (vstCompatibilityWarningsEnabled && warning.size() > 0)
            juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon, "VST3 Compatibility Warning", warning);
    }

    void MainComponent::openSelectedTrackVstPluginUi()
    {
        if (!currentProject)
            return;

        const auto index = getSelectedTrackIndex();
        if (index < 0 || index >= static_cast<int>(currentProject->getTracks().size()))
        {
            logMessage("VST UI: no valid selected track.");
            return;
        }

        auto& track = currentProject->getTracks()[static_cast<std::size_t>(index)];
        auto assignment = track.getInstrument();
        if (repairVst3BundlePathIfPossible(assignment))
            track.setInstrumentAssignment(assignment);

        auto bundlePath = resolveVst3BundlePath(track.getInstrument());
        if (track.getInstrument().backendType != mw::core::SampleBackendType::VST3
            || bundlePath.empty())
        {
            if (detectedVstPlugins.empty())
                scanVstPlugins(false);

            std::vector<mw::vst::VstPluginDescriptor> instrumentPlugins;
            for (const auto& plugin : detectedVstPlugins)
            {
                if (isSupportedVstInstrumentPlugin(plugin))
                    instrumentPlugins.push_back(plugin);
            }

            std::optional<mw::vst::VstPluginDescriptor> descriptor;
            const int selectedInstrumentId = instrumentCombo.getSelectedId();
            if (selectedInstrumentId > 0 && selectedInstrumentId <= static_cast<int>(instrumentPlugins.size()))
                descriptor = instrumentPlugins[static_cast<std::size_t>(selectedInstrumentId - 1)];

            if (!descriptor && currentProject)
            {
                const auto& settings = currentProject->getUserSettings();
                for (const auto& plugin : instrumentPlugins)
                {
                    if ((!settings.vst3PluginUid.empty() && plugin.uid == settings.vst3PluginUid)
                        || (!settings.vst3PluginPath.empty() && pathsReferToSameLocation(plugin.bundlePath, settings.vst3PluginPath)))
                    {
                        descriptor = plugin;
                        break;
                    }
                }

                if (!descriptor && !settings.vst3PluginPath.empty())
                {
                    mw::vst::VstPluginDescriptor savedDescriptor;
                    savedDescriptor.bundlePath = settings.vst3PluginPath;
                    savedDescriptor.name = settings.vst3PluginName.empty()
                        ? settings.vst3PluginPath.stem().string()
                        : settings.vst3PluginName;
                    savedDescriptor.vendor = settings.vst3PluginVendor;
                    savedDescriptor.version = settings.vst3PluginVersion;
                    savedDescriptor.category = settings.vst3PluginCategory;
                    savedDescriptor.uid = settings.vst3PluginUid;
                    savedDescriptor.kind = mw::vst::VstPluginKind::Instrument;
                    savedDescriptor.status = mw::vst::VstPluginScanStatus::Unknown;
                    descriptor = savedDescriptor;
                }
            }

            if (descriptor)
            {
                assignment = track.getInstrument();
                applyVstPluginDescriptorToAssignment(assignment, *descriptor);
                track.setInstrumentAssignment(assignment);
                trackBackendCombo.setSelectedId(4, juce::dontSendNotification);
                bundlePath = descriptor->bundlePath;
                refreshTrackSoundLibraryDisplay();
                populateInstrumentCombo();
                updateTrackSummary(*currentProject);
                refreshTrackManagerText();
                refreshOpenPianoRollInstrumentControls();
                setProjectDirty();
                logMessage("Open VST Plugin: applied selected VST3 plugin to " + getTrackDisplayName(index) + ": " + descriptor->displayName());
            }
        }

        if (track.getInstrument().backendType != mw::core::SampleBackendType::VST3
            || bundlePath.empty())
        {
            logMessage("Open VST Plugin: selected track does not have a resolvable VST3 plugin. Choose a VST3 plugin, then click Open VST Plugin or Apply Track Settings.");
            updateOpenVstPluginButtonState();
            return;
        }

        if (detectedVstPlugins.empty())
            scanVstPlugins(false);

        auto resolvedPluginDescriptor = findVstPluginDescriptorForAssignment(detectedVstPlugins, track.getInstrument());
        if (!resolvedPluginDescriptor && !bundlePath.empty() && std::filesystem::exists(bundlePath))
        {
            auto inspected = mw::vst::VstPluginScanner::inspectBundle(bundlePath);
            std::vector<mw::vst::VstPluginDescriptor> inspectedPlugins { inspected };
            applyVstPluginCatalogRecords(inspectedPlugins);
            if (!inspectedPlugins.empty())
                resolvedPluginDescriptor = inspectedPlugins.front();
        }

        if (resolvedPluginDescriptor && !isSupportedVstInstrumentPlugin(*resolvedPluginDescriptor))
        {
            const auto message = juce::String("This VST3 plugin is not currently available as a track instrument.\n\n")
                + "Detected kind: " + juce::String(mw::vst::vstPluginKindToString(resolvedPluginDescriptor->detectedKind)) + "\n"
                + "Final status: " + vstPluginFinalStatusText(*resolvedPluginDescriptor) + "\n\n"
                + "Use VST3 Plugin Manager to review details or manually Treat as Instrument if you know it accepts MIDI notes and produces audio.";
            logMessage(juce::String("Open VST Plugin blocked: ") + juce::String(resolvedPluginDescriptor->displayName()) + " is not a supported instrument.");
            juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon, "Open VST Plugin", message);
            updateOpenVstPluginButtonState();
            return;
        }

        if (!std::filesystem::exists(bundlePath))
        {
            logMessage("Open VST Plugin: plugin bundle was not found: " + bundlePath.string());
            return;
        }

        showVstExperimentalWarningIfNeeded();

        if (auto found = vstPluginEditorWindows.find(index); found != vstPluginEditorWindows.end() && found->second != nullptr)
        {
            found->second->toFront(true);
            return;
        }

        const int effectiveWindowLimit = sanitizeMaxOpenVstPluginWindows(vstMaxOpenPluginWindows);
        if (static_cast<int>(vstPluginEditorWindows.size()) >= effectiveWindowLimit)
        {
            const auto message = juce::String("Maximum open VST plugin windows reached (")
                + juce::String(effectiveWindowLimit)
                + "). Close another VST plugin window first, or use VST Plugins > Close All VST Plugin Windows.";
            logMessage(message);
            juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon, "Open VST Plugin", message);
            return;
        }

        auto load = mw::vst::VstInstrumentHost::loadInstrumentForTrack(track, 48000.0, 512);
        if (!load.success || load.instance == nullptr)
        {
            if (resolvedPluginDescriptor)
            {
                updateVstPluginCatalogRecord(*resolvedPluginDescriptor, std::nullopt, true, load.message);
                scanVstPlugins(false);
                populateInstrumentCombo();
                updateOpenVstPluginButtonState();
                logMessage(juce::String("VST3 plugin marked failed after load/open failure: ") + juce::String(resolvedPluginDescriptor->displayName()) + ".");
            }

            juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon, "Open VST3 UI", load.message);
            return;
        }

        class VstEditorHolder final : public juce::Component,
                                      public VstEditorStateProvider
        {
        public:
            class VstFallbackParameterEditor final : public juce::Component,
                                                     private juce::Timer
            {
            public:
                explicit VstFallbackParameterEditor(juce::AudioProcessor& processorIn)
                    : processor(processorIn)
                {
                    heading.setText("This VST3 plugin did not provide a custom editor. Basic parameter editing is shown below.", juce::dontSendNotification);
                    heading.setJustificationType(juce::Justification::centredLeft);
                    heading.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
                    addAndMakeVisible(heading);

                    auto params = processor.getParameters();
                    for (auto* parameter : params)
                    {
                        if (parameter == nullptr)
                            continue;

                        auto row = std::make_unique<ParameterRow>();
                        row->parameter = parameter;
                        row->name.setText(parameter->getName(80), juce::dontSendNotification);
                        row->name.setJustificationType(juce::Justification::centredLeft);
                        row->name.setColour(juce::Label::textColourId, juce::Colours::white);
                        row->value.setText(parameterValueText(*parameter), juce::dontSendNotification);
                        row->value.setJustificationType(juce::Justification::centredRight);
                        row->value.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
                        row->slider.setRange(0.0, 1.0, 0.0001);
                        row->slider.setSliderStyle(juce::Slider::LinearHorizontal);
                        row->slider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
                        row->slider.setValue(parameter->getValue(), juce::dontSendNotification);

                        auto* rowPtr = row.get();
                        row->slider.onValueChange = [rowPtr]
                        {
                            if (rowPtr == nullptr || rowPtr->parameter == nullptr || rowPtr->updatingFromPlugin)
                                return;

                            rowPtr->parameter->setValueNotifyingHost(static_cast<float>(rowPtr->slider.getValue()));
                            rowPtr->value.setText(parameterValueText(*rowPtr->parameter), juce::dontSendNotification);
                        };

                        addAndMakeVisible(row->name);
                        addAndMakeVisible(row->slider);
                        addAndMakeVisible(row->value);
                        rows.push_back(std::move(row));
                    }

                    if (rows.empty())
                    {
                        emptyLabel.setText("This plugin does not expose editable parameters to the host.", juce::dontSendNotification);
                        emptyLabel.setJustificationType(juce::Justification::centred);
                        emptyLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
                        addAndMakeVisible(emptyLabel);
                    }

                    startTimerHz(8);
                }

                void resized() override
                {
                    auto area = getLocalBounds().reduced(12);
                    heading.setBounds(area.removeFromTop(42));
                    area.removeFromTop(8);

                    if (rows.empty())
                    {
                        emptyLabel.setBounds(area);
                        return;
                    }

                    for (auto& row : rows)
                    {
                        auto rowArea = area.removeFromTop(42);
                        row->name.setBounds(rowArea.removeFromLeft(190));
                        row->value.setBounds(rowArea.removeFromRight(150));
                        row->slider.setBounds(rowArea.reduced(8, 4));
                        area.removeFromTop(4);
                    }
                }

            private:
                struct ParameterRow
                {
                    juce::AudioProcessorParameter* parameter = nullptr;
                    juce::Label name;
                    juce::Slider slider;
                    juce::Label value;
                    bool updatingFromPlugin = false;
                };

                static juce::String parameterValueText(juce::AudioProcessorParameter& parameter)
                {
                    auto text = parameter.getCurrentValueAsText().trim();
                    if (text.isNotEmpty())
                        return text;

                    return juce::String(parameter.getValue(), 3);
                }

                void timerCallback() override
                {
                    for (auto& row : rows)
                    {
                        if (row->parameter == nullptr)
                            continue;

                        row->updatingFromPlugin = true;
                        row->slider.setValue(row->parameter->getValue(), juce::dontSendNotification);
                        row->value.setText(parameterValueText(*row->parameter), juce::dontSendNotification);
                        row->updatingFromPlugin = false;
                    }
                }

                juce::AudioProcessor& processor;
                juce::Label heading;
                juce::Label emptyLabel;
                std::vector<std::unique_ptr<ParameterRow>> rows;
            };

            class LiveVstAuditionCallback final : public juce::AudioIODeviceCallback
            {
            public:
                explicit LiveVstAuditionCallback(juce::AudioPluginInstance& pluginIn)
                    : plugin(pluginIn)
                {
                }

                void audioDeviceAboutToStart(juce::AudioIODevice* device) override
                {
                    const juce::ScopedLock lock(processorLock);
                    currentSampleRate = device != nullptr && device->getCurrentSampleRate() > 0.0
                        ? device->getCurrentSampleRate()
                        : 48000.0;
                    currentBlockSize = device != nullptr && device->getCurrentBufferSizeSamples() > 0
                        ? device->getCurrentBufferSizeSamples()
                        : 512;

                    try
                    {
                        plugin.setPlayConfigDetails(0, 2, currentSampleRate, currentBlockSize);
                        plugin.prepareToPlay(currentSampleRate, currentBlockSize);
                        scratch.setSize(2, currentBlockSize, false, false, true);
                        failed = false;
                    }
                    catch (...)
                    {
                        failed = true;
                    }
                }

                void audioDeviceStopped() override
                {
                }

                void audioDeviceIOCallback(const float* const* inputChannelData,
                                           int numInputChannels,
                                           float* const* outputChannelData,
                                           int numOutputChannels,
                                           int numSamples)
                {
                    processAudio(inputChannelData, numInputChannels, outputChannelData, numOutputChannels, numSamples);
                }

                void audioDeviceIOCallbackWithContext(const float* const* inputChannelData,
                                                      int numInputChannels,
                                                      float* const* outputChannelData,
                                                      int numOutputChannels,
                                                      int numSamples,
                                                      const juce::AudioIODeviceCallbackContext&) override
                {
                    processAudio(inputChannelData, numInputChannels, outputChannelData, numOutputChannels, numSamples);
                }

                bool hasFailed() const noexcept { return failed; }

            private:
                void processAudio(const float* const*,
                                  int,
                                  float* const* outputChannelData,
                                  int numOutputChannels,
                                  int numSamples)
                {
                    for (int channel = 0; channel < numOutputChannels; ++channel)
                        if (outputChannelData[channel] != nullptr)
                            juce::FloatVectorOperations::clear(outputChannelData[channel], numSamples);

                    if (numOutputChannels <= 0 || numSamples <= 0 || failed)
                        return;

                    constexpr int pluginOutputChannels = 2;
                    if (scratch.getNumChannels() < pluginOutputChannels || scratch.getNumSamples() < numSamples)
                        scratch.setSize(pluginOutputChannels, numSamples, false, false, true);

                    scratch.clear();
                    emptyMidi.clear();

                    try
                    {
                        const juce::ScopedLock lock(processorLock);
                        plugin.processBlock(scratch, emptyMidi);
                    }
                    catch (...)
                    {
                        failed = true;
                        return;
                    }

                    const int channelsToCopy = juce::jmin(numOutputChannels, scratch.getNumChannels());
                    for (int channel = 0; channel < channelsToCopy; ++channel)
                    {
                        if (outputChannelData[channel] != nullptr)
                            juce::FloatVectorOperations::copy(outputChannelData[channel], scratch.getReadPointer(channel), numSamples);
                    }
                }

                juce::AudioPluginInstance& plugin;
                juce::CriticalSection processorLock;
                juce::AudioBuffer<float> scratch;
                juce::MidiBuffer emptyMidi;
                double currentSampleRate = 48000.0;
                int currentBlockSize = 512;
                std::atomic<bool> failed { false };
            };

            class FloatingControlsContent final : public juce::Component
            {
            public:
                FloatingControlsContent(std::function<void()> onApplyIn,
                                        juce::String initialStatus,
                                        juce::Component* dragTargetIn)
                    : onApply(std::move(onApplyIn)),
                      dragTarget(dragTargetIn)
                {
                    // Keep the plugin editor controls intentionally simple: Apply and go.
                    // Native plugin state restore can be fragile in-process, so the older
                    // Revert/Redo buttons are no longer exposed from this palette.

                    applyButton.setButtonText("Apply Changes");
                    applyButton.setTooltip("Capture the current plugin UI state and apply it to this track for Piano Roll Preview and render.");
                    applyButton.onClick = [this]
                    {
                        if (onApply)
                            onApply();
                    };
                    addAndMakeVisible(applyButton);

                    juce::ignoreUnused(initialStatus);

                    // The palette is now a compact single-purpose Apply-only handle.
                    // Status still goes to the Apply button tooltip so the floating
                    // window can stay out of the plugin editor's way.
                    setSize(196, 48);

                    // Listen to mouse events from child buttons too, so a click-and-hold
                    // anywhere on the palette can drag the floating controls window.
                    addMouseListener(this, true);
                }

                void setStatusText(const juce::String& text)
                {
                    applyButton.setTooltip(text);
                }

                void paint(juce::Graphics& g) override
                {
                    g.fillAll(juce::Colours::transparentBlack);
                    auto bounds = getLocalBounds().toFloat().reduced(1.0f);
                    g.setColour(juce::Colour(0xff1f2328));
                    g.fillRoundedRectangle(bounds, 8.0f);
                    g.setColour(juce::Colour(0xff5b646f));
                    g.drawRoundedRectangle(bounds, 8.0f, 1.0f);
                }

                void mouseDown(const juce::MouseEvent& event) override
                {
                    if (dragTarget != nullptr)
                    {
                        auto targetEvent = event.getEventRelativeTo(dragTarget);
                        dragger.startDraggingComponent(dragTarget, targetEvent);
                    }
                }

                void mouseDrag(const juce::MouseEvent& event) override
                {
                    if (dragTarget != nullptr)
                    {
                        auto targetEvent = event.getEventRelativeTo(dragTarget);
                        dragger.dragComponent(dragTarget, targetEvent, nullptr);
                    }
                }

                void resized() override
                {
                    auto area = getLocalBounds().reduced(8, 8);
                    applyButton.setBounds(area.withSizeKeepingCentre(160, 30));
                }

            private:
                std::function<void()> onApply;
                juce::Component* dragTarget = nullptr;
                juce::ComponentDragger dragger;
                juce::TextButton applyButton;
            };

            class FloatingControlsWindow final : public juce::DocumentWindow
            {
            public:
                FloatingControlsWindow()
                    : juce::DocumentWindow("", juce::Colours::transparentBlack, 0)
                {
                    setUsingNativeTitleBar(false);
                    setTitleBarHeight(0);
                    setAlwaysOnTop(true);
                    setResizable(false, false);
                    setDropShadowEnabled(true);
                }

                ~FloatingControlsWindow() override
                {
                    setLookAndFeel(nullptr);
                }

                void closeButtonPressed() override
                {
                    setVisible(false);
                }
            };

            VstEditorHolder(std::unique_ptr<juce::AudioPluginInstance> pluginIn,
                            bool enableLiveAuditionIn,
                            std::function<bool(const juce::String&)> onApplyStateIn,
                            std::function<void(const juce::String&)> onCloseStateIn)
                : plugin(std::move(pluginIn)),
                  onApplyState(std::move(onApplyStateIn)),
                  onCloseState(std::move(onCloseStateIn)),
                  enableLiveAudition(enableLiveAuditionIn)
            {
                if (plugin != nullptr && plugin->hasEditor())
                    editor.reset(plugin->createEditorIfNeeded());

                if (editor == nullptr)
                {
                    editor = std::make_unique<VstFallbackParameterEditor>(*plugin);
                    editorCanResize = true;
                    const auto parameterCount = plugin != nullptr ? plugin->getParameters().size() : 0;
                    nativeEditorSize = juce::Rectangle<int>(640, juce::jlimit(320, 680, 78 + parameterCount * 46));
                }
                else if (auto* processorEditor = dynamic_cast<juce::AudioProcessorEditor*>(editor.get()))
                {
                    editorCanResize = processorEditor->isResizable();
                    nativeEditorSize = juce::Rectangle<int>(juce::jmax(420, processorEditor->getWidth()), juce::jmax(300, processorEditor->getHeight()));
                }
                else
                {
                    editorCanResize = false;
                    nativeEditorSize = juce::Rectangle<int>(juce::jmax(420, editor->getWidth()), juce::jmax(300, editor->getHeight()));
                }

                addAndMakeVisible(*editor);

                // The editor instance is intentionally edit-only. Piano Roll Preview
                // renders notes in a separate preview instance. Use Apply Changes
                // to make current UI edits the saved track state used by preview/render.
                juce::ignoreUnused(enableLiveAudition);
                setSize(nativeEditorSize.getWidth(), nativeEditorSize.getHeight());

                floatingControlsWindow = std::make_unique<FloatingControlsWindow>();
                auto* controls = new FloatingControlsContent(
                    [this] { applyCurrentChanges(); },
                    "Edit the plugin here, then click Apply Changes before Piano Roll Preview/render.",
                    floatingControlsWindow.get());
                floatingControlsContent = controls;
                floatingControlsWindow->setContentOwned(controls, true);
                floatingControlsWindow->setSize(controls->getWidth(), controls->getHeight());
            }

            ~VstEditorHolder() override
            {
                if (floatingControlsWindow != nullptr)
                    floatingControlsWindow->setVisible(false);
                floatingControlsContent = nullptr;
                floatingControlsWindow.reset();

                if (liveAuditionStarted)
                    audioDeviceManager.removeAudioCallback(liveAuditionCallback.get());

                liveAuditionCallback.reset();
                editor = nullptr;
                if (plugin != nullptr)
                {
                    try
                    {
                        // Apply-and-go only: closing the editor must not ask the
                        // native plugin to serialize state. Some plugins are fragile
                        // during close, and Apply Changes is the only intentional
                        // state-capture point.
                        plugin->suspendProcessing(true);
                        plugin->releaseResources();
                    }
                    catch (...)
                    {
                    }

                    if (onCloseState)
                        onCloseState(juce::String());
                }
            }

            juce::String captureCurrentVstStateBase64() override
            {
                stopLiveAuditionForPreview();

                if (plugin == nullptr)
                    return {};

                try
                {
                    // Repeated native plugin state capture is one of the riskiest
                    // in-process VST operations. Make each Apply Changes action a
                    // quiet commit point, but do not call reset() on the editor-owned
                    // instance before capture. Some plugins treat reset as more than
                    // an audio-tail reset while their UI is open, which can make later
                    // Apply attempts look stale or destabilize the editor instance.
                    plugin->suspendProcessing(true);

                    juce::MemoryBlock state;
                    plugin->getStateInformation(state);

                    plugin->suspendProcessing(false);
                    return state.toBase64Encoding();
                }
                catch (...)
                {
                    resumePluginAfterStateCapture();
                    return {};
                }
            }

            bool restoreVstStateBase64(const juce::String& stateBase64) override
            {
                stopLiveAuditionForPreview();

                if (plugin == nullptr || stateBase64.isEmpty())
                    return false;

                try
                {
                    juce::MemoryBlock state;
                    if (!state.fromBase64Encoding(stateBase64))
                        return false;

                    plugin->suspendProcessing(true);
                    plugin->setStateInformation(state.getData(), static_cast<int>(state.getSize()));
                    plugin->suspendProcessing(false);
                    if (editor != nullptr)
                        editor->repaint();
                    return true;
                }
                catch (...)
                {
                    if (plugin != nullptr)
                        plugin->suspendProcessing(false);
                    return false;
                }
            }

            void stopLiveAuditionForPreview() override
            {
                if (!liveAuditionStarted)
                    return;

                audioDeviceManager.removeAudioCallback(liveAuditionCallback.get());
                liveAuditionStarted = false;
                liveAuditionCallback.reset();
                setFloatingControlsStatus("Piano Roll Preview uses the last applied plugin state. Click Apply Changes after editing controls.");
            }

            void quiescePluginBeforeStateCapture()
            {
                stopLiveAuditionForPreview();

                if (plugin == nullptr)
                    return;

                try
                {
                    plugin->suspendProcessing(true);
                }
                catch (...)
                {
                }
            }

            void resumePluginAfterStateCapture()
            {
                if (plugin == nullptr)
                    return;

                try
                {
                    plugin->suspendProcessing(false);
                }
                catch (...)
                {
                    try
                    {
                        plugin->suspendProcessing(false);
                    }
                    catch (...)
                    {
                    }
                }
            }

            bool canResizeEditor() const noexcept { return editorCanResize; }

            void showFloatingControlsPalette()
            {
                if (floatingControlsWindow == nullptr)
                    return;

                const int paletteWidth = juce::jmax(196, floatingControlsWindow->getWidth());
                const int paletteHeight = juce::jmax(48, floatingControlsWindow->getHeight());
                auto targetBounds = getTopLevelComponent() != nullptr
                    ? getTopLevelComponent()->getScreenBounds()
                    : getScreenBounds();

                if (targetBounds.isEmpty())
                    targetBounds = juce::Rectangle<int>(80, 80, 600, 400);

                auto userArea = juce::Rectangle<int>(0, 0, 1920, 1080);
                if (const auto* display = juce::Desktop::getInstance().getDisplays().getDisplayForRect(targetBounds))
                    userArea = display->userArea;

                int x = targetBounds.getRight() + 12;
                int y = targetBounds.getY() + 24;

                if (x + paletteWidth > userArea.getRight())
                    x = targetBounds.getX() + 24;
                if (y + paletteHeight > userArea.getBottom())
                    y = userArea.getBottom() - paletteHeight - 24;

                x = juce::jlimit(userArea.getX(), juce::jmax(userArea.getX(), userArea.getRight() - paletteWidth), x);
                y = juce::jlimit(userArea.getY(), juce::jmax(userArea.getY(), userArea.getBottom() - paletteHeight), y);

                floatingControlsWindow->setBounds(x, y, paletteWidth, paletteHeight);
                floatingControlsWindow->setVisible(true);
                applyPoorMansStudioWindowIcon(*floatingControlsWindow, PoorMansStudioWindowIcon::VSTPlugin);
                floatingControlsWindow->toFront(false);
            }

            void resized() override
            {
                if (editor == nullptr)
                    return;

                if (editorCanResize)
                    editor->setBounds(getLocalBounds());
                else
                    editor->setBounds(nativeEditorSize.withPosition(0, 0));
            }

        private:
            void applyCurrentChanges()
            {
                if (applyInProgress)
                {
                    setFloatingControlsStatus("Apply already in progress...");
                    return;
                }

                applyInProgress = true;
                setFloatingControlsStatus("Stopping preview/audio before applying plugin changes...");
                quiescePluginBeforeStateCapture();

                juce::Component::SafePointer<VstEditorHolder> safeThis(this);
                juce::Timer::callAfterDelay(300, [safeThis]
                {
                    if (safeThis != nullptr)
                        safeThis->applyCurrentChangesNow();
                });
            }

            void applyCurrentChangesNow()
            {
                setFloatingControlsStatus("Capturing plugin state...");
                const auto currentState = captureCurrentVstStateBase64();
                if (currentState.isEmpty())
                {
                    resumePluginAfterStateCapture();
                    applyInProgress = false;
                    setFloatingControlsStatus("Apply failed: plugin did not return state data.");
                    return;
                }

                const bool applied = onApplyState && onApplyState(currentState);
                resumePluginAfterStateCapture();
                applyInProgress = false;

                if (applied)
                    setFloatingControlsStatus("Applied changes. Preview/render will use the new state.");
                else
                    setFloatingControlsStatus("Apply failed: track assignment could not be updated.");
            }


            void setFloatingControlsStatus(const juce::String& text)
            {
                if (floatingControlsContent != nullptr)
                    floatingControlsContent->setStatusText(text);
            }

            void startLiveAudition()
            {
                if (plugin == nullptr)
                {
                    setFloatingControlsStatus("Plugin UI audio unavailable: plugin did not load.");
                    return;
                }

                liveAuditionCallback = std::make_unique<LiveVstAuditionCallback>(*plugin);
                const auto error = audioDeviceManager.initialise(0, 2, nullptr, true);
                if (error.isNotEmpty())
                {
                    setFloatingControlsStatus("Plugin UI audio unavailable: " + error);
                    liveAuditionCallback.reset();
                    return;
                }

                audioDeviceManager.addAudioCallback(liveAuditionCallback.get());
                liveAuditionStarted = true;
                setFloatingControlsStatus("Plugin editor is open. Use Apply Changes before Piano Roll Preview/render.");
            }

            std::unique_ptr<juce::AudioPluginInstance> plugin;
            std::unique_ptr<juce::Component> editor;
            std::function<bool(const juce::String&)> onApplyState;
            std::function<void(const juce::String&)> onCloseState;
            juce::AudioDeviceManager audioDeviceManager;
            std::unique_ptr<LiveVstAuditionCallback> liveAuditionCallback;
            std::unique_ptr<FloatingControlsWindow> floatingControlsWindow;
            FloatingControlsContent* floatingControlsContent = nullptr;
            bool liveAuditionStarted = false;
            bool applyInProgress = false;
            bool enableLiveAudition = true;
            bool editorCanResize = false;
            juce::Rectangle<int> nativeEditorSize { 420, 300 };
        };

        const bool pluginLooksRisky = resolvedPluginDescriptor
            && (resolvedPluginDescriptor->compatibility.hasAnyGpuOrUiRisk()
                || resolvedPluginDescriptor->status == mw::vst::VstPluginScanStatus::Warning);
        const bool enableLiveAudition = !vstSafePluginUiMode && !pluginLooksRisky;


        auto* holder = new VstEditorHolder(
            std::move(load.instance),
            enableLiveAudition,
            [this, index](const juce::String& stateBase64) -> bool
            {
                if (!currentProject)
                    return false;

                if (index < 0 || index >= static_cast<int>(currentProject->getTracks().size()))
                    return false;

                if (stateBase64.isEmpty())
                    return false;

                auto& editedTrack = currentProject->getTracks()[static_cast<std::size_t>(index)];
                auto assignment = editedTrack.getInstrument();
                if (assignment.backendType != mw::core::SampleBackendType::VST3)
                    return false;

                // Apply-and-go only. Store the latest applied plugin state as the
                // single source of truth for preview, render, and project save.
                assignment.vst3.stateBase64 = stateBase64.toStdString();

                editedTrack.setInstrumentAssignment(assignment);

                // Treat every Apply Changes click as a fresh commit point. Even if a
                // plugin serializes the same bytes for a parameter change, clear any
                // rendered preview audio and refresh displays so the next preview/render
                // rebuilds from the editor's latest committed state rather than reusing
                // the first successful Apply result.
                cleanupPianoRollPreviewFiles();
                refreshTrackManagerText();
                recordExternalTrackStateUpdate(index);

                setProjectDirty();

                logMessage("Applied plugin UI changes for " + getTrackDisplayName(index) + ". Cleared old preview audio so the next preview/render rebuilds from the latest applied state.");
                return true;
            },
            [this, index](const juce::String&)
            {
                logMessage("Closed plugin editor for " + getTrackDisplayName(index) + ". Use Apply Changes before closing to keep plugin UI edits.");
            });
        const auto title = juce::String("Track ") + juce::String(index + 1) + " - " + juce::String(track.getInstrument().displayName);
        auto window = std::make_unique<PianoRollDocumentWindow>(title, [this, index] { vstPluginEditorWindows.erase(index); });
        const bool pluginEditorCanResize = holder->canResizeEditor();
        window->setResizable(pluginEditorCanResize, pluginEditorCanResize);
        window->setContentOwned(holder, true);
        window->centreWithSize(juce::jmax(460, holder->getWidth() + 30), juce::jmax(340, holder->getHeight() + 50));
        window->setVisible(true);
        holder->showFloatingControlsPalette();
        applyPoorMansStudioWindowIcon(*window, PoorMansStudioWindowIcon::VSTPlugin);
        vstPluginEditorWindows[index] = std::move(window);
        updateOpenVstPluginButtonState();
    }

    juce::String MainComponent::captureOpenVstPluginStateForTrack(int trackIndex, bool updateTrackAssignment, bool logCapture)
    {
        if (trackIndex < 0)
            return {};

        const auto found = vstPluginEditorWindows.find(trackIndex);
        if (found == vstPluginEditorWindows.end() || found->second == nullptr)
            return {};

        auto* content = found->second->getContentComponent();
        auto* stateProvider = dynamic_cast<VstEditorStateProvider*>(content);
        if (stateProvider == nullptr)
            return {};

        stateProvider->stopLiveAuditionForPreview();
        const auto capturedState = stateProvider->captureCurrentVstStateBase64();
        if (capturedState.isEmpty())
        {
            if (logCapture)
                logMessage("VST3 UI state capture skipped: plugin did not return state data.");
            return {};
        }

        if (updateTrackAssignment && currentProject
            && trackIndex >= 0
            && trackIndex < static_cast<int>(currentProject->getTracks().size()))
        {
            auto& track = currentProject->getTracks()[static_cast<std::size_t>(trackIndex)];
            auto assignment = track.getInstrument();
            if (assignment.backendType == mw::core::SampleBackendType::VST3
                && assignment.vst3.stateBase64 != capturedState.toStdString())
            {
                assignment.vst3.stateBase64 = capturedState.toStdString();
                track.setInstrumentAssignment(assignment);
                setProjectDirty();
                refreshTrackManagerText();
                if (logCapture)
                    logMessage("Captured current VST3 plugin UI state for " + getTrackDisplayName(trackIndex) + ".");
            }
        }

        return capturedState;
    }

    bool MainComponent::closeVstPluginWindowForTrack(int trackIndex, const juce::String& reason)
    {
        if (trackIndex < 0)
            return false;

        const auto found = vstPluginEditorWindows.find(trackIndex);
        if (found == vstPluginEditorWindows.end() || found->second == nullptr)
            return false;

        // Hide the editor immediately before destroying the holder/window. This
        // makes Apply Track Settings visibly close stale plugin UI windows even
        // when the close is triggered from another floating/piano-roll window.
        found->second->setVisible(false);
        vstPluginEditorWindows.erase(found);
        updateOpenVstPluginButtonState();

        if (reason.isNotEmpty())
            logMessage(reason);
        else
            logMessage("Closed open VST plugin window for " + getTrackDisplayName(trackIndex) + ".");

        return true;
    }

    void MainComponent::closeAllVstPluginWindows()
    {
        vstPluginEditorWindows.clear();
        updateOpenVstPluginButtonState();
        logMessage("Closed all VST plugin windows.");
    }


    void MainComponent::setDefaultPaths()
    {
        const auto preferences = mw::app::UserPreferencesStore::load();

        exportFolderBox.setText(preferences.lastExportFolder.string());

        if (!preferences.lastSoundFontPath.empty() && std::filesystem::exists(preferences.lastSoundFontPath))
            soundFontPathBox.setText(preferences.lastSoundFontPath.string());
        else
            soundFontPathBox.setText(mw::app::AppPaths::soundFontsFolder().string());

        if (!preferences.lastSfzPath.empty() && std::filesystem::exists(preferences.lastSfzPath))
            sfzPathBox.setText(preferences.lastSfzPath.string());
        else
            sfzPathBox.setText(mw::app::AppPaths::sfzFolder().string());

        refreshSoundFontList();
        refreshSfzList();
        refreshPresetListFromSelectedSoundFont();

        fluidSynthPathBox.setText(mw::app::AppPaths::defaultFluidSynthExePath().string());
        ffmpegPathBox.setText(mw::app::AppPaths::defaultFfmpegExePath().string());
        sfizzPathBox.setText(mw::app::AppPaths::defaultSfizzRenderExePath().string());
        mw::app::TempCleaner::cleanOldMxlExtractsOnStartup();

        baseNameBox.setText(juce::String(), juce::dontSendNotification);

        appliedProjectBackendId = preferences.lastBackendId > 0 ? preferences.lastBackendId : 1;
        backendCombo.setSelectedId(appliedProjectBackendId, juce::dontSendNotification);
        outputFormatCombo.setSelectedId(preferences.lastOutputFormatId > 0 ? preferences.lastOutputFormatId : 1);
        audioClipFormatCombo.setSelectedId(preferences.lastAudioClipFormatId > 0 ? preferences.lastAudioClipFormatId : 1);
        audioClipQualityCombo.setSelectedId(preferences.lastAudioClipQualityKbps > 0 ? preferences.lastAudioClipQualityKbps : 320);
        sampleRateCombo.setSelectedId(preferences.lastSampleRate > 0 ? preferences.lastSampleRate : 48000);
        bitrateCombo.setSelectedId(preferences.lastBitrateKbps > 0 ? preferences.lastBitrateKbps : 192);
        channelsCombo.setSelectedId(preferences.lastChannelCount > 0 ? preferences.lastChannelCount : 2);
        renderWorkersCombo.setSelectedId(preferences.lastRenderWorkerCount > 0 ? preferences.lastRenderWorkerCount : 100);
        updateRenderOutputSummary();
        populateInstrumentCombo();

        const int maxThemeId = std::max(1, static_cast<int>(themePresetFiles.size()));
        const int themeId = std::clamp(preferences.themePresetId, 1, maxThemeId);
        themeCombo.setSelectedId(themeId, juce::dontSendNotification);
        applyThemePreset(themeId);
    }

    void MainComponent::paint(juce::Graphics& g)
    {
        const auto theme = getThemePresetFromFileList(themePresetFiles, currentThemePresetId);

        g.fillAll(theme.background);
        g.setColour(theme.panel);
        g.fillRoundedRectangle(getLocalBounds().toFloat().reduced(12.0f), 12.0f);
    }

    void MainComponent::resized()
    {
        auto area = getLocalBounds().reduced(20);

        menuBar.setBounds(area.removeFromTop(24));
        area.removeFromTop(6);

        auto titleRow = area.removeFromTop(36);
        renderStatusLabel.setBounds(titleRow.removeFromRight(260));
        titleLabel.setBounds(titleRow);
        area.removeFromTop(8);

        const int rowHeight = 32;
        const int labelWidth = 115;
        const int buttonWidth = 145;

        auto topButtons = area.removeFromTop(rowHeight);
        newProjectButton.setBounds(topButtons.removeFromLeft(120).reduced(4, 2));
        openProjectButton.setBounds(topButtons.removeFromLeft(145).reduced(4, 2));
        saveProjectButton.setBounds(topButtons.removeFromLeft(130).reduced(4, 2));
        cleanTempButton.setBounds(topButtons.removeFromLeft(120).reduced(4, 2));
        saveSettingsButton.setBounds(topButtons.removeFromLeft(125).reduced(4, 2));
        renderButton.setBounds(topButtons.removeFromLeft(125).reduced(4, 2));
        renderSelectedTrackButton.setBounds(topButtons.removeFromLeft(105).reduced(4, 2));
        renderSelectedSequenceButton.setBounds(topButtons.removeFromLeft(95).reduced(4, 2));
        renderMidiButton.setBounds(topButtons.removeFromLeft(110).reduced(4, 2));
        cancelRenderButton.setBounds(topButtons.removeFromLeft(125).reduced(4, 2));
        renderSettingsButton.setBounds(topButtons.removeFromLeft(125).reduced(4, 2));
        themeLabel.setBounds(topButtons.removeFromLeft(65));
        themeCombo.setBounds(topButtons.removeFromLeft(230).reduced(4, 2));
        area.removeFromTop(4);
        renderTargetLabel.setBounds(area.removeFromTop(24).reduced(4, 2));
        area.removeFromTop(4);

        auto topArea = area.removeFromTop(290);
        auto leftTop = topArea.removeFromLeft(topArea.getWidth() / 2);
        leftTop.removeFromRight(8);
        auto rightTop = topArea;
        rightTop.removeFromLeft(8);

        auto layoutTextButtonRow = [&](juce::Rectangle<int>& panel, juce::Label& label, juce::TextEditor& editor, juce::TextButton& button)
        {
            auto row = panel.removeFromTop(rowHeight);
            label.setBounds(row.removeFromLeft(labelWidth));
            button.setBounds(row.removeFromRight(buttonWidth).reduced(4, 2));
            editor.setBounds(row.reduced(4, 2));
            panel.removeFromTop(6);
        };

        layoutTextButtonRow(leftTop, musicXmlLabel, musicXmlPathBox, chooseMusicXmlButton);
        layoutTextButtonRow(leftTop, exportFolderLabel, exportFolderBox, exportFolderButton);

        auto baseRow = leftTop.removeFromTop(rowHeight);
        baseNameLabel.setBounds(baseRow.removeFromLeft(labelWidth));
        editInfoButton.setBounds(baseRow.removeFromRight(95).reduced(4, 2));
        baseNameBox.setBounds(baseRow.reduced(4, 2));
        leftTop.removeFromTop(6);

        auto formatRow = leftTop.removeFromTop(rowHeight);
        outputFormatLabel.setBounds(formatRow.removeFromLeft(labelWidth));
        outputFormatCombo.setBounds(formatRow.removeFromLeft(190).reduced(4, 2));
        sampleRateLabel.setBounds(formatRow.removeFromLeft(100));
        sampleRateCombo.setBounds(formatRow.removeFromLeft(150).reduced(4, 2));
        leftTop.removeFromTop(6);

        auto renderQualityRow = leftTop.removeFromTop(rowHeight);
        bitrateLabel.setBounds(renderQualityRow.removeFromLeft(labelWidth));
        bitrateCombo.setBounds(renderQualityRow.removeFromLeft(120).reduced(4, 2));
        channelsLabel.setBounds(renderQualityRow.removeFromLeft(75));
        channelsCombo.setBounds(renderQualityRow.removeFromLeft(110).reduced(4, 2));
        renderOutputSummaryLabel.setBounds(renderQualityRow.reduced(4, 2));
        leftTop.removeFromTop(6);

        auto audioClipFormatRow = leftTop.removeFromTop(rowHeight);
        audioClipFormatLabel.setBounds(audioClipFormatRow.removeFromLeft(labelWidth));
        audioClipFormatCombo.setBounds(audioClipFormatRow.removeFromLeft(190).reduced(4, 2));
        audioClipQualityLabel.setBounds(audioClipFormatRow.removeFromLeft(100));
        audioClipQualityCombo.setBounds(audioClipFormatRow.removeFromLeft(190).reduced(4, 2));
        leftTop.removeFromTop(6);


        auto noteHeader = area.removeFromTop(rowHeight);
        noteEditorLabel.setBounds(noteHeader.removeFromLeft(labelWidth));
        openPianoRollButton.setBounds(noteHeader.removeFromLeft(140).reduced(4, 2));
        trackManagerButton.setBounds(noteHeader.removeFromLeft(130).reduced(4, 2));
        importAudioButton.setBounds(noteHeader.removeFromLeft(120).reduced(4, 2));
        recordAudioButton.setBounds(noteHeader.removeFromLeft(125).reduced(4, 2));
        area.removeFromTop(8);

        auto projectDefaultsHeaderRow = rightTop.removeFromTop(26);
        projectDefaultsLabel.setBounds(projectDefaultsHeaderRow.reduced(4, 2));
        rightTop.removeFromTop(4);

        auto sfRow = rightTop.removeFromTop(rowHeight);
        soundFontLabel.setBounds(sfRow.removeFromLeft(labelWidth));
        browseSoundFontButton.setBounds(sfRow.removeFromRight(36).reduced(4, 2));
        refreshSoundFontsButton.setBounds(sfRow.removeFromRight(82).reduced(4, 2));
        soundFontCombo.setBounds(sfRow.reduced(4, 2));
        rightTop.removeFromTop(6);

        auto sfzRow = rightTop.removeFromTop(rowHeight);
        sfzLabel.setBounds(sfzRow.removeFromLeft(labelWidth));
        sfzButton.setBounds(sfzRow.removeFromRight(36).reduced(4, 2));
        refreshSfzButton.setBounds(sfzRow.removeFromRight(82).reduced(4, 2));
        sfzCombo.setBounds(sfzRow.reduced(4, 2));
        rightTop.removeFromTop(6);

        auto sfzControlRow = rightTop.removeFromTop(rowHeight);
        sfzKeySwitchLabel.setBounds(sfzControlRow.removeFromLeft(labelWidth));
        sfzKeySwitchBox.setBounds(sfzControlRow.removeFromLeft(70).reduced(4, 2));
        sfzCc1Label.setBounds(sfzControlRow.removeFromLeft(45));
        sfzCc1Box.setBounds(sfzControlRow.removeFromLeft(65).reduced(4, 2));
        sfzCc11Label.setBounds(sfzControlRow.removeFromLeft(55));
        sfzCc11Box.setBounds(sfzControlRow.removeFromLeft(65).reduced(4, 2));
        sfzTestButton.setBounds(sfzControlRow.removeFromLeft(140).reduced(4, 2));
        rightTop.removeFromTop(6);

        auto backendRow = rightTop.removeFromTop(rowHeight);
        backendLabel.setBounds(backendRow.removeFromLeft(labelWidth));
        applyBackendButton.setBounds(backendRow.removeFromRight(170).reduced(4, 2));
        backendCombo.setBounds(backendRow.removeFromLeft(180).reduced(4, 2));
        rightTop.removeFromTop(6);

        auto trackRow = area.removeFromTop(rowHeight);
        trackLabel.setBounds(trackRow.removeFromLeft(labelWidth));
        constexpr int mainTrackSelectorWidth = 274;      // 210 + roughly 8 more visible characters
        constexpr int mainSequenceSelectorWidth = 294;   // 230 + roughly 8 more visible characters
        trackCombo.setBounds(trackRow.removeFromLeft(mainTrackSelectorWidth).reduced(4, 2));
        sequenceSelectorLabel.setBounds(trackRow.removeFromLeft(75).reduced(4, 2));
        sequenceSelectorBox.setBounds(trackRow.removeFromLeft(mainSequenceSelectorWidth).reduced(4, 2));
        changeActiveSequenceButton.setBounds(trackRow.removeFromLeft(76).reduced(4, 2));
        renameTrackButton.setBounds(trackRow.removeFromLeft(75).reduced(4, 2));
        addTrackButton.setBounds(trackRow.removeFromLeft(75).reduced(4, 2));
        duplicateTrackButton.setBounds(trackRow.removeFromLeft(75).reduced(4, 2));
        removeTrackButton.setBounds(trackRow.removeFromLeft(75).reduced(4, 2));
        muteToggle.setBounds(trackRow.removeFromLeft(60).reduced(4, 2));
        soloToggle.setBounds(trackRow.removeFromLeft(60).reduced(4, 2));
        applyTrackButton.setBounds(trackRow.removeFromLeft(150).reduced(4, 2));
        area.removeFromTop(6);

        auto thoughtsRow = area.removeFromTop(54);
        sequenceThoughtsLabel.setBounds(thoughtsRow.removeFromLeft(labelWidth));
        sequenceThoughtsBox.setBounds(thoughtsRow.reduced(4, 2));
        area.removeFromTop(6);

        auto trackLibraryRow = area.removeFromTop(rowHeight);
        trackSoundLibraryLabel.setBounds(trackLibraryRow.removeFromLeft(145));
        changeTrackLibraryButton.setBounds(trackLibraryRow.removeFromRight(140).reduced(4, 2));
        trackSoundLibraryBox.setBounds(trackLibraryRow.reduced(4, 2));
        area.removeFromTop(6);

        auto trackInstrumentRow = area.removeFromTop(rowHeight);
        instrumentLabel.setBounds(trackInstrumentRow.removeFromLeft(labelWidth));
        openVstPluginButton.setBounds(trackInstrumentRow.removeFromRight(155).reduced(4, 2));
        instrumentCombo.setBounds(trackInstrumentRow.reduced(4, 2));
        area.removeFromTop(6);

        auto volumeRow = area.removeFromTop(rowHeight);
        trackVolumeLabel.setBounds(volumeRow.removeFromLeft(labelWidth));
        trackVolumeSlider.setBounds(volumeRow.removeFromLeft(320).reduced(4, 2));
        masterVolumeLabel.setBounds(volumeRow.removeFromLeft(120));
        masterVolumeSlider.setBounds(volumeRow.removeFromLeft(320).reduced(4, 2));
        area.removeFromTop(12);

        auto bottom = area;
        auto leftBottom = bottom.removeFromLeft(bottom.getWidth() / 2);
        leftBottom.removeFromRight(8);
        auto rightBottom = bottom;
        rightBottom.removeFromLeft(8);

        pianoRollHelpLabel.setText("Open Piano Roll edits notes. Project timing is managed in Track Manager; note length, velocity, snap grid, and beat-window controls live in the Piano Roll window.", juce::dontSendNotification);
        pianoRollHelpLabel.setBounds(leftBottom.removeFromTop(24));
        leftBottom.removeFromTop(6);

        trackSummaryBox.setBounds(leftBottom);
        logBox.setBounds(rightBottom);
    }

    void MainComponent::createSfzTestMidiAndRender()
    {
        const auto sfzPath = std::filesystem::path(sfzPathBox.getText().toStdString());
        const auto sfizzPath = std::filesystem::path(sfizzPathBox.getText().toStdString());

        if (sfzPath.empty() || !std::filesystem::exists(sfzPath))
        {
            logMessage("ERROR: Choose a valid SFZ before test rendering.");
            return;
        }

        mw::core::Project testProject("sfz_test");
        auto& track = testProject.addTrack("SFZ Test Track");

        int keySwitch = 24;
        try { keySwitch = std::stoi(sfzKeySwitchBox.getText().toStdString()); } catch (...) {}

        if (keySwitch >= 0 && keySwitch <= 127)
        {
            track.addNote(mw::core::NoteEvent(keySwitch, 1, 0, 10, 1, mw::core::Articulation::Normal));
        }

        track.addNote(mw::core::NoteEvent(60, 100, 120, 960, 1, mw::core::Articulation::Normal));
        track.addNote(mw::core::NoteEvent(64, 100, 1080, 960, 1, mw::core::Articulation::Normal));
        track.addNote(mw::core::NoteEvent(67, 100, 2040, 960, 1, mw::core::Articulation::Normal));

        mw::exporting::ExportSettings settings;
        settings.outputFolder = std::filesystem::path(exportFolderBox.getText().toStdString());
        settings.baseFileName = "sfz_test";

        mw::exporting::ExportPathBuilder::ensureOutputFolderExists(settings);

        const auto midiPath = mw::exporting::ExportPathBuilder::buildMidiPath(settings);
        const auto wavPath = mw::exporting::ExportPathBuilder::buildAudioPath(settings, mw::exporting::AudioFormat::Wav);

        if (!mw::midi::MidiExporter::exportToFile(testProject, midiPath))
        {
            logMessage("ERROR: Failed to create SFZ test MIDI.");
            return;
        }

        const auto validation = mw::audio::SfzValidator::validateSampleReferences(sfzPath);
        logMessage(validation.message);

        if (!validation.ok)
        {
            logMessage("ERROR: SFZ test render cancelled because required samples are missing.");
            return;
        }

        mw::audio::SfizzRenderRequest request;
        request.sfizzRenderExePath = sfizzPath;
        request.sfzPath = sfzPath;
        request.midiInputPath = midiPath;
        request.wavOutputPath = wavPath;

        const auto result = mw::audio::ExternalSfizzRenderer::renderMidiToWav(request);

        logMessage("SFZ test command: " + result.commandLine);
        logMessage(result.message);

        if (result.success)
            logMessage("SFZ test WAV: " + wavPath.string());
        else
            logMessage("ERROR: SFZ test render failed.");
    }

    void MainComponent::captureProjectUserSettings()
    {
        if (!currentProject)
            return;

        auto& settings = currentProject->getUserSettings();

        settings.sourceInputPath = std::filesystem::path(musicXmlPathBox.getText().toStdString());
        settings.exportFolder = std::filesystem::path(exportFolderBox.getText().toStdString());
        settings.soundFontPath =
            getSelectedSoundFontPath().empty()
                ? std::filesystem::path(soundFontPathBox.getText().toStdString())
                : getSelectedSoundFontPath();
        settings.fluidSynthPath = mw::app::AppPaths::defaultFluidSynthExePath();
        settings.ffmpegPath = mw::app::AppPaths::defaultFfmpegExePath();
        settings.sfzPath = std::filesystem::path(sfzPathBox.getText().toStdString());
        settings.sfizzRenderPath = mw::app::AppPaths::defaultSfizzRenderExePath();

        settings.baseFileName = baseNameBox.getText().isEmpty()
            ? currentProject->getName()
            : baseNameBox.getText().toStdString();

        settings.metadataTitle = metadataTitleBox.getText().trim().toStdString();
        settings.metadataArtist = metadataArtistBox.getText().trim().toStdString();
        settings.metadataAlbum = metadataAlbumBox.getText().trim().toStdString();
        settings.metadataTrackNumber = metadataTrackNumberBox.getText().trim().toStdString();
        settings.metadataYear = metadataYearBox.getText().trim().toStdString();

        settings.backendId = appliedProjectBackendId > 0 ? appliedProjectBackendId : 1;
        settings.outputFormatId = outputFormatCombo.getSelectedId() > 0 ? outputFormatCombo.getSelectedId() : 1;
        settings.audioClipFormatId = audioClipFormatCombo.getSelectedId() > 0 ? audioClipFormatCombo.getSelectedId() : 1;
        settings.audioClipQualityKbps = audioClipQualityCombo.getSelectedId() > 0 ? audioClipQualityCombo.getSelectedId() : 320;
        settings.sampleRate = sampleRateCombo.getSelectedId() > 0 ? sampleRateCombo.getSelectedId() : 48000;
        settings.bitrateKbps = bitrateCombo.getSelectedId() > 0 ? bitrateCombo.getSelectedId() : 192;
        settings.channelCount = channelsCombo.getSelectedId() > 0 ? channelsCombo.getSelectedId() : 2;
        settings.renderWorkerCount = renderWorkersCombo.getSelectedId() == 100 ? 0 : std::max(1, renderWorkersCombo.getSelectedId());

        try { settings.sfzKeySwitch = std::stoi(sfzKeySwitchBox.getText().toStdString()); } catch (...) { settings.sfzKeySwitch = 24; }
        try { settings.sfzCc1 = std::stoi(sfzCc1Box.getText().toStdString()); } catch (...) { settings.sfzCc1 = 100; }
        try { settings.sfzCc11 = std::stoi(sfzCc11Box.getText().toStdString()); } catch (...) { settings.sfzCc11 = 127; }

        settings.masterVolume = static_cast<float>(masterVolumeSlider.getValue());
    }

    void MainComponent::applyProjectUserSettingsToGui()
    {
        if (!currentProject)
            return;

        const auto& settings = currentProject->getUserSettings();

        if (!settings.sourceInputPath.empty())
            musicXmlPathBox.setText(settings.sourceInputPath.string());

        if (!settings.exportFolder.empty())
            exportFolderBox.setText(settings.exportFolder.string());

        if (!settings.soundFontPath.empty())
            soundFontPathBox.setText(settings.soundFontPath.string());

        fluidSynthPathBox.setText(mw::app::AppPaths::defaultFluidSynthExePath().string(), juce::dontSendNotification);
        ffmpegPathBox.setText(mw::app::AppPaths::defaultFfmpegExePath().string(), juce::dontSendNotification);

        if (!settings.sfzPath.empty())
            sfzPathBox.setText(settings.sfzPath.string());

        sfizzPathBox.setText(mw::app::AppPaths::defaultSfizzRenderExePath().string(), juce::dontSendNotification);

        appliedProjectBackendId = settings.backendId > 0 ? settings.backendId : 1;
        backendCombo.setSelectedId(appliedProjectBackendId, juce::dontSendNotification);

        populateInstrumentCombo();

        if (!settings.baseFileName.empty())
            baseNameBox.setText(settings.baseFileName);

        metadataTitleBox.setText(settings.metadataTitle, juce::dontSendNotification);
        metadataArtistBox.setText(settings.metadataArtist, juce::dontSendNotification);
        metadataAlbumBox.setText(settings.metadataAlbum, juce::dontSendNotification);
        metadataTrackNumberBox.setText(settings.metadataTrackNumber, juce::dontSendNotification);
        metadataYearBox.setText(settings.metadataYear, juce::dontSendNotification);

        outputFormatCombo.setSelectedId(settings.outputFormatId > 0 ? settings.outputFormatId : 1, juce::dontSendNotification);
        audioClipFormatCombo.setSelectedId(settings.audioClipFormatId > 0 ? settings.audioClipFormatId : 1, juce::dontSendNotification);
        audioClipQualityCombo.setSelectedId(settings.audioClipQualityKbps > 0 ? settings.audioClipQualityKbps : 320, juce::dontSendNotification);
        sampleRateCombo.setSelectedId(settings.sampleRate > 0 ? settings.sampleRate : 48000, juce::dontSendNotification);
        bitrateCombo.setSelectedId(settings.bitrateKbps > 0 ? settings.bitrateKbps : 192, juce::dontSendNotification);
        channelsCombo.setSelectedId(settings.channelCount > 0 ? settings.channelCount : 2, juce::dontSendNotification);
        updateRenderOutputSummary();

        sfzKeySwitchBox.setText(juce::String(settings.sfzKeySwitch));
        sfzCc1Box.setText(juce::String(settings.sfzCc1));
        sfzCc11Box.setText(juce::String(settings.sfzCc11));
        masterVolumeSlider.setValue(settings.masterVolume, juce::dontSendNotification);

        refreshSoundFontList();
        refreshPresetListFromSelectedSoundFont();
        updateVolumeLabels();
    }

    void MainComponent::resetProjectWorkspace()
    {
        suppressDirtyTracking = true;

        finishClosingPianoRollWindow();

        currentProject.reset();
        currentProjectFilePath.reset();
        currentProjectFolderPath.reset();
        trackManagerUndoStack.clear();
        importSections.clear();
        activeImportSectionIndex = -1;
        sequenceColourOverrides.clear();

        trackCombo.clear(juce::dontSendNotification);
        instrumentCombo.clear(juce::dontSendNotification);
        trackBackendCombo.setSelectedId(1, juce::dontSendNotification);
        trackSoundLibraryBox.clear();
        trackManagerSelectBox.clear();
        trackManagerStartBeatBox.clear();
        trackManagerSectionBox.clear();
        trackManagerSectionStartBeatBox.clear();
        noteEditorBox.clear();
        trackSummaryBox.clear();
        trackManagerBox.setText("No project loaded.\n", juce::dontSendNotification);

        pianoRoll.setNotes({});
        pianoRoll.stopPreviewPlayhead();

        musicXmlPathBox.clear();
        baseNameBox.setText(juce::String(), juce::dontSendNotification);
        renderStatusLabel.setText("Ready", juce::dontSendNotification);

        finishProgrammaticProjectLoad();

        logMessage("Started a new blank project workspace. Choose Input or Import Sequence to begin.");
    }

    void MainComponent::startNewProject()
    {
        if (pianoRollEditorDirty || trackManagerEditorDirty)
        {
            auto* editorAlert = new juce::AlertWindow(
                "Editor Windows Have Unapplied Changes",
                "Apply open Piano Roll/Track Manager edits before starting a new project, discard those editor edits, or cancel?",
                juce::AlertWindow::QuestionIcon
            );

            editorAlert->addButton("Apply Changes", 1, juce::KeyPress(juce::KeyPress::returnKey));
            editorAlert->addButton("Discard Editor Changes", 2);
            editorAlert->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

            editorAlert->enterModalState(
                true,
                juce::ModalCallbackFunction::create(
                    [this, editorAlert](int result)
                    {
                        std::unique_ptr<juce::AlertWindow> alertOwner(editorAlert);

                        if (result == 1)
                        {
                            if (trackManagerEditorDirty)
                                applyTrackManagerEditorChanges();

                            if (pianoRollEditorDirty)
                                applyPianoRollEditorChanges();

                            startNewProject();
                        }
                        else if (result == 2)
                        {
                            if (trackManagerEditorDirty)
                                discardTrackManagerEditorChanges();

                            if (pianoRollEditorDirty)
                                discardPianoRollEditorChanges();

                            startNewProject();
                        }
                        else
                        {
                            logMessage("New Project cancelled because an editor has unapplied changes.");
                        }
                    }
                ),
                true
            );

            return;
        }

        if (!currentProject || !projectDirty)
        {
            resetProjectWorkspace();
            return;
        }

        auto* alert = new juce::AlertWindow(
            "Start New Project",
            "Save the current project before starting a new blank workspace?",
            juce::AlertWindow::WarningIcon
        );

        alert->addButton("Save and Start New", 1);
        alert->addButton("Discard and Start New", 2);
        alert->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

        alert->enterModalState(
            true,
            juce::ModalCallbackFunction::create(
                [this, alert](int result)
                {
                    std::unique_ptr<juce::AlertWindow> alertOwner(alert);

                    if (result == 0)
                    {
                        logMessage("New Project cancelled.");
                        return;
                    }

                    if (result == 2)
                    {
                        resetProjectWorkspace();
                        return;
                    }

                    applyTrackInspector();
                    captureProjectUserSettings();

                    std::filesystem::create_directories(mw::app::AppPaths::projectsFolder());

                    const auto defaultName =
                        baseNameBox.getText().isEmpty()
                            ? juce::String(currentProject ? currentProject->getName() : "project")
                            : baseNameBox.getText();

                    const auto defaultSaveFile = currentProjectFilePath
                        ? juce::File(currentProjectFilePath->string())
                        : juce::File(mw::app::AppPaths::projectFileForName(defaultName.toStdString()).string());

                    activeFileChooser = std::make_unique<juce::FileChooser>(
                        "Save Poor Man's Studio Project",
                        defaultSaveFile,
                        "*.mwproj"
                    );

                    activeFileChooser->launchAsync(
                        juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles,
                        [this](const juce::FileChooser& chooser)
                        {
                            auto file = chooser.getResult();

                            if (file == juce::File{})
                            {
                                logMessage("New Project cancelled during save.");
                                return;
                            }

                            saveProjectToFileWithOverwriteWarning(
                                file,
                                [this]
                                {
                                    resetProjectWorkspace();
                                }
                            );
                        }
                    );
                }
            ),
            true
        );
    }

    void MainComponent::openProjectFile()
    {
        if (projectDirty || pianoRollEditorDirty || trackManagerEditorDirty)
        {
            confirmDiscardUnsavedChanges(
                "Open another project",
                [this]
                {
                    clearProjectDirty();
                    openProjectFile();
                }
            );
            return;
        }


        activeFileChooser = std::make_unique<juce::FileChooser>(
            "Open Poor Man's Studio Project",
            juce::File(mw::app::AppPaths::projectsFolder().string()),
            "*.mwproj"
        );

        activeFileChooser->launchAsync(
            juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
            [this](const juce::FileChooser& chooser)
            {
                const auto file = chooser.getResult();
                if (file == juce::File{}) return;

                auto loaded = mw::serialization::ProjectSerializer::loadFromFile(
                    std::filesystem::path(file.getFullPathName().toStdString())
                );

                if (!loaded)
                {
                    logMessage("ERROR: Failed to open project.");
                    return;
                }

                if (!enforceProjectTrackLimit(*loaded, "Project file"))
                    return;

                suppressDirtyTracking = true;
                finishClosingPianoRollWindow();

                currentProject = *loaded;
                syncProjectIdentityToFile(file);
                const auto loadedProjectFolder = std::filesystem::path(file.getParentDirectory().getFullPathName().toStdString());
                for (auto& clip : currentProject->getAudioClips())
                {
                    const auto absoluteMediaPath = clip.projectRelativePath.is_absolute()
                        ? clip.projectRelativePath
                        : loadedProjectFolder / clip.projectRelativePath;
                    clip.missingMedia = !std::filesystem::exists(absoluteMediaPath);
                    if (clip.missingMedia)
                        logMessage(juce::String("WARNING: Missing AudioClip media: ") + absoluteMediaPath.string());
                }
                trackManagerUndoStack.clear();
                restoreSequencesFromProjectMetadata();

                applyProjectUserSettingsToGui();
                syncProjectIdentityToFile(file);
                if (exportFolderShouldFollowProjectFolder(loadedProjectFolder))
                    exportFolderBox.setText((loadedProjectFolder / "renders").string(), juce::dontSendNotification);
                captureProjectUserSettings();
                seedMissingTrackSoundLibrariesFromProjectDefaults(*currentProject);

                refreshTrackSelector();
                syncTrackInspectorFromSelection();
                tempoBox.setText(juce::String(currentProject->getTempoBpm()), juce::dontSendNotification);
                juce::String ts;
                ts << currentProject->getTimeSignature().numerator << "/" << currentProject->getTimeSignature().denominator;
                timeSignatureBox.setText(ts, juce::dontSendNotification);
                refreshNoteEditor();
                fitPianoRollToSelectedTrack();
                updateTrackSummary(*currentProject);
                refreshTrackManagerText();

                logMessage("Opened project: " + file.getFullPathName());
                logMessage("Restored source/input/backend/settings from project file.");

                finishProgrammaticProjectLoad();
            }
        );
    }

    bool MainComponent::writeProjectToFile(const juce::File& file)
    {
        if (!currentProject)
        {
            logMessage("ERROR: No project loaded to save.");
            return false;
        }

        std::error_code projectFolderError;
        const auto projectFilePath = std::filesystem::path(file.getFullPathName().toStdString());
        const auto projectFolderPath = std::filesystem::path(file.getParentDirectory().getFullPathName().toStdString());
        std::filesystem::create_directories(projectFolderPath / "input" / "midi", projectFolderError);
        std::filesystem::create_directories(projectFolderPath / "input" / "mxl", projectFolderError);
        std::filesystem::create_directories(projectFolderPath / "input" / "audio" / "imported", projectFolderError);
        std::filesystem::create_directories(projectFolderPath / "input" / "audio" / "recorded", projectFolderError);
        std::filesystem::create_directories(projectFolderPath / "input" / "audio" / "temp", projectFolderError);
        std::filesystem::create_directories(projectFolderPath / "output", projectFolderError);
        std::filesystem::create_directories(projectFolderPath / "renders", projectFolderError);

        if (projectFolderError)
        {
            logMessage("ERROR: Failed to create project folder structure: " + juce::String(projectFolderError.message()));
            return false;
        }

        const bool shouldFollowProjectFolder = exportFolderShouldFollowProjectFolder(projectFolderPath);

        currentProject->setName(file.getFileNameWithoutExtension().toStdString());
        baseNameBox.setText(file.getFileNameWithoutExtension(), juce::dontSendNotification);
        currentProjectFilePath = projectFilePath;
        currentProjectFolderPath = projectFolderPath;

        if (shouldFollowProjectFolder)
            exportFolderBox.setText((projectFolderPath / "renders").string(), juce::dontSendNotification);

        captureProjectUserSettings();
        syncSequencesToProjectMetadata();

        if (mw::serialization::ProjectSerializer::saveToFile(
                *currentProject,
                projectFilePath
            ))
        {
            clearProjectDirty();
            logMessage("Saved project: " + file.getFullPathName());
            logMessage("Project identity synced: folder, .mwproj filename, and project name are all \"" + file.getFileNameWithoutExtension() + "\".");
            return true;
        }

        logMessage("ERROR: Failed to save project.");
        return false;
    }

    void MainComponent::saveProjectToFileWithOverwriteWarning(juce::File file, std::function<void()> afterSuccessfulSave)
    {
        if (file.getFileExtension().isEmpty())
            file = file.withFileExtension(".mwproj");

        const auto projectName = file.getFileNameWithoutExtension();
        if (file.getParentDirectory().getFileName() != projectName)
        {
            const auto projectFolder = file.getParentDirectory().getChildFile(projectName);
            file = projectFolder.getChildFile(projectName + ".mwproj");
        }

        auto performSave = [this, file, afterSuccessfulSave]
        {
            if (writeProjectToFile(file) && afterSuccessfulSave)
                afterSuccessfulSave();
        };

        const bool fileExists = file.existsAsFile();
        const bool folderExists = file.getParentDirectory().exists();

        if (!fileExists && !folderExists)
        {
            performSave();
            return;
        }

        auto* alert = new juce::AlertWindow(
            "Overwrite Project?",
            "A project folder or .mwproj named \"" + file.getFileNameWithoutExtension() + "\" already exists in workspace/projects.\n\nOverwrite/use that project folder?",
            juce::AlertWindow::WarningIcon
        );

        alert->addButton("Overwrite", 1);
        alert->addButton("Choose Different Name", 2);
        alert->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

        alert->enterModalState(
            true,
            juce::ModalCallbackFunction::create(
                [this, alert, file, afterSuccessfulSave](int result)
                {
                    std::unique_ptr<juce::AlertWindow> cleanup(alert);

                    if (result == 0)
                    {
                        logMessage("Save cancelled; existing project was not overwritten.");
                        return;
                    }

                    if (result == 2)
                    {
                        saveCurrentProjectFile(afterSuccessfulSave);
                        return;
                    }

                    if (writeProjectToFile(file) && afterSuccessfulSave)
                        afterSuccessfulSave();
                }
            ),
            true
        );
    }

    void MainComponent::saveCurrentProjectFile(std::function<void()> afterSuccessfulSave)
    {
        if (!currentProject)
        {
            logMessage("ERROR: No project loaded to save.");
            return;
        }

        applyTrackInspector();
        captureProjectUserSettings();

        std::filesystem::create_directories(mw::app::AppPaths::projectsFolder());

        const auto defaultName =
            baseNameBox.getText().isEmpty()
                ? juce::String(currentProject->getName())
                : baseNameBox.getText();

        const auto defaultSaveFile = currentProjectFilePath
            ? juce::File(currentProjectFilePath->string())
            : juce::File(mw::app::AppPaths::projectFileForName(defaultName.toStdString()).string());

        activeFileChooser = std::make_unique<juce::FileChooser>(
            "Save Poor Man's Studio Project",
            defaultSaveFile,
            "*.mwproj"
        );

        activeFileChooser->launchAsync(
            juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles,
            [this, afterSuccessfulSave](const juce::FileChooser& chooser)
            {
                auto file = chooser.getResult();
                if (file == juce::File{}) return;

                saveProjectToFileWithOverwriteWarning(file, afterSuccessfulSave);
            }
        );
    }

    void MainComponent::setProjectDirty(bool shouldBeDirty)
    {
        if (shouldBeDirty && suppressDirtyTracking)
            return;

        if (projectDirty == shouldBeDirty)
            return;

        projectDirty = shouldBeDirty;

        titleLabel.setText(
            mw::app::applicationTitle() + (projectDirty ? " *" : ""),
            juce::dontSendNotification
        );
    }

    void MainComponent::clearProjectDirty()
    {
        setProjectDirty(false);
    }

    void MainComponent::finishProgrammaticProjectLoad()
    {
        clearProjectDirty();

        juce::MessageManager::callAsync(
            [this]
            {
                suppressDirtyTracking = false;
                clearProjectDirty();
            }
        );
    }


    void MainComponent::cleanupAppTempOnExit()
    {
        stopProjectPreview();
        cleanupPianoRollPreviewFiles();
        const auto result = mw::app::TempCleaner::cleanAppOwnedTempFiles();
        logMessage(result.message);
    }

    void MainComponent::requestCloseWithSaveAsPrompt(std::function<void()> exitCallback)
    {
        auto exitAfterCleanup = [this, exitCallback]
        {
            cleanupAppTempOnExit();
            if (exitCallback)
                exitCallback();
        };

        if (pianoRollEditorDirty || trackManagerEditorDirty)
        {
            auto* editorAlert = new juce::AlertWindow(
                "Editor Windows Have Unapplied Changes",
                "Apply open Piano Roll/Track Manager edits to the project, discard those editor edits, or cancel and keep working?",
                juce::AlertWindow::QuestionIcon
            );

            editorAlert->addButton("Apply Changes", 1, juce::KeyPress(juce::KeyPress::returnKey));
            editorAlert->addButton("Discard Editor Changes", 2);
            editorAlert->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

            editorAlert->enterModalState(
                true,
                juce::ModalCallbackFunction::create(
                    [this, editorAlert, exitCallback](int result)
                    {
                        std::unique_ptr<juce::AlertWindow> cleanup(editorAlert);

                        if (result == 1)
                        {
                            if (trackManagerEditorDirty)
                                applyTrackManagerEditorChanges();

                            if (pianoRollEditorDirty)
                                applyPianoRollEditorChanges();

                            requestCloseWithSaveAsPrompt(exitCallback);
                        }
                        else if (result == 2)
                        {
                            if (trackManagerEditorDirty)
                                discardTrackManagerEditorChanges();

                            if (pianoRollEditorDirty)
                                discardPianoRollEditorChanges();

                            requestCloseWithSaveAsPrompt(exitCallback);
                        }
                        else
                        {
                            logMessage("Exit cancelled because an editor has unapplied changes.");
                        }
                    }
                )
            );

            return;
        }

        if (!projectDirty)
        {
            exitAfterCleanup();
            return;
        }

        auto* alert = new juce::AlertWindow(
            "Unsaved Changes",
            "The current project has unsaved changes. What would you like to do before exiting?",
            juce::AlertWindow::WarningIcon
        );

        alert->addButton("Save As", 1, juce::KeyPress(juce::KeyPress::returnKey));
        alert->addButton("Discard and Exit", 2);
        alert->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

        alert->enterModalState(
            true,
            juce::ModalCallbackFunction::create(
                [this, alert, exitAfterCleanup](int result)
                {
                    std::unique_ptr<juce::AlertWindow> cleanup(alert);

                    if (result == 0)
                    {
                        logMessage("Exit cancelled because project has unsaved changes.");
                        return;
                    }

                    if (result == 2)
                    {
                        logMessage("Discarding unsaved changes and exiting.");
                        exitAfterCleanup();
                        return;
                    }

                    applyTrackInspector();
                    captureProjectUserSettings();
                    saveCurrentProjectFile(exitAfterCleanup);
                }
            ),
            true
        );
    }

    bool MainComponent::confirmDiscardUnsavedChanges(const juce::String& actionName, std::function<void()> proceedCallback)
    {
        if (pianoRollEditorDirty || trackManagerEditorDirty)
        {
            auto* editorAlert = new juce::AlertWindow(
                "Editor Windows Have Unapplied Changes",
                "Apply open Piano Roll/Track Manager edits before continuing, discard those editor edits, or cancel?",
                juce::AlertWindow::QuestionIcon
            );

            editorAlert->addButton("Apply Changes", 1, juce::KeyPress(juce::KeyPress::returnKey));
            editorAlert->addButton("Discard Editor Changes", 2);
            editorAlert->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

            editorAlert->enterModalState(
                true,
                juce::ModalCallbackFunction::create(
                    [this, editorAlert, actionName, proceedCallback](int result)
                    {
                        std::unique_ptr<juce::AlertWindow> cleanup(editorAlert);

                        if (result == 1)
                        {
                            if (trackManagerEditorDirty)
                                applyTrackManagerEditorChanges();

                            if (pianoRollEditorDirty)
                                applyPianoRollEditorChanges();

                            confirmDiscardUnsavedChanges(actionName, proceedCallback);
                        }
                        else if (result == 2)
                        {
                            if (trackManagerEditorDirty)
                                discardTrackManagerEditorChanges();

                            if (pianoRollEditorDirty)
                                discardPianoRollEditorChanges();

                            confirmDiscardUnsavedChanges(actionName, proceedCallback);
                        }
                        else
                        {
                            logMessage("Action cancelled because an editor has unapplied changes.");
                        }
                    }
                ),
                true
            );

            return false;
        }

        if (!projectDirty)
        {
            if (proceedCallback)
                proceedCallback();

            return true;
        }

        auto* alert = new juce::AlertWindow(
            "Unsaved Changes",
            "The current project has unsaved changes. " + actionName + "?",
            juce::AlertWindow::WarningIcon
        );

        alert->addButton("Save First", 1);
        alert->addButton("Discard", 2);
        alert->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

        alert->enterModalState(
            true,
            juce::ModalCallbackFunction::create(
                [this, alert, proceedCallback](int result)
                {
                    std::unique_ptr<juce::AlertWindow> cleanup(alert);

                    if (result == 0)
                    {
                        logMessage("Action cancelled because project has unsaved changes.");
                        return;
                    }

                    if (result == 1)
                    {
                        saveCurrentProjectFile();
                        logMessage("Save requested. Run the action again after the project is saved.");
                        return;
                    }

                    if (proceedCallback)
                        proceedCallback();
                }
            ),
            true
        );

        return false;
    }

    void MainComponent::cleanTempFolder()
    {
        const auto result = mw::app::TempCleaner::cleanTempFolder();
        logMessage(result.message);
    }


    void MainComponent::captureTrackManagerUndoState(const juce::String& actionLabel)
    {
        if (!currentProject)
            return;

        TrackManagerUndoState state;
        state.project = *currentProject;
        sanitizeProjectForUndoStack(state.project);
        state.importSections = importSections;
        state.sequenceColourOverrides = sequenceColourOverrides;
        state.activeImportSectionIndex = activeImportSectionIndex;
        state.sequenceMapFirstIndex = sequenceMapFirstIndex;
        state.sequenceMapStartBeat = sequenceMapStartBeat;
        state.sequenceMapBeatWindow = sequenceMapBeatWindow;
        state.label = actionLabel;

        trackManagerUndoStack.push_back(std::move(state));

        constexpr std::size_t maxUndoSteps = 10;
        if (trackManagerUndoStack.size() > maxUndoSteps)
            trackManagerUndoStack.erase(trackManagerUndoStack.begin());
    }

    void MainComponent::undoTrackManagerEdit()
    {
        if (trackManagerUndoStack.empty())
        {
            logMessage("Track Manager Undo: nothing to undo.");
            return;
        }

        auto state = std::move(trackManagerUndoStack.back());
        trackManagerUndoStack.pop_back();

        preserveCurrentVstAppliedStates(state.project, *currentProject);
        currentProject = state.project;
        importSections = std::move(state.importSections);
        sequenceColourOverrides = std::move(state.sequenceColourOverrides);
        activeImportSectionIndex = state.activeImportSectionIndex;
        sequenceMapFirstIndex = state.sequenceMapFirstIndex;
        sequenceMapStartBeat = state.sequenceMapStartBeat;
        sequenceMapBeatWindow = state.sequenceMapBeatWindow;
        trackManagerMapStartBeatBox.setText(juce::String(sequenceMapStartBeat), juce::dontSendNotification);
        trackManagerMapBeatWindowBox.setText(sequenceMapBeatWindow <= 0 ? juce::String("Full") : juce::String(sequenceMapBeatWindow), juce::dontSendNotification);

        refreshTrackSelector();

        if (currentProject && !currentProject->getTracks().empty())
        {
            const auto selectedId = std::clamp(trackCombo.getSelectedId(), 1, static_cast<int>(currentProject->getTracks().size()));
            trackCombo.setSelectedId(selectedId, juce::sendNotification);
        }

        syncTrackInspectorFromSelection();
        refreshNoteEditor();
        syncPianoRollFromSelectedTrack();
        updateTrackSummary(*currentProject);
        refreshTrackManagerText();
        refreshMainSequenceSelector();
        refreshActiveSequenceThoughtsEditor();
        updateRenderTargetLabel();
        if (trackManagerContent != nullptr)
        {
            if (auto* content = dynamic_cast<TrackManagerWindowContent*>(trackManagerContent.get()))
                content->refreshSequenceMap();
            else
                trackManagerContent->repaint();
        }

        setProjectDirty();
        logMessage("Track Manager Undo restored: " + state.label);
    }

    void MainComponent::startTrackManagerEditSession()
    {
        trackManagerSessionProjectSnapshot = currentProject;
        if (trackManagerSessionProjectSnapshot)
            sanitizeProjectForUndoStack(*trackManagerSessionProjectSnapshot);
        trackManagerSessionImportSectionsSnapshot = importSections;
        trackManagerSessionColourOverridesSnapshot = sequenceColourOverrides;
        trackManagerSessionActiveImportSectionIndexSnapshot = activeImportSectionIndex;
        trackManagerSessionSequenceMapFirstIndexSnapshot = sequenceMapFirstIndex;
        trackManagerSessionSequenceMapStartBeatSnapshot = sequenceMapStartBeat;
        trackManagerSessionSequenceMapBeatWindowSnapshot = sequenceMapBeatWindow;
        trackManagerSessionProjectWasDirty = projectDirty;
        trackManagerExternalTrackStateUpdates.clear();
        trackManagerExternalProjectDirtyWhileEditing = false;
        clearTrackManagerEditorDirty();
    }

    void MainComponent::refreshTrackManagerSessionSnapshotIfClean()
    {
        if (trackManagerWindow == nullptr || trackManagerEditorDirty)
            return;

        trackManagerSessionProjectSnapshot = currentProject;
        if (trackManagerSessionProjectSnapshot)
            sanitizeProjectForUndoStack(*trackManagerSessionProjectSnapshot);
        trackManagerSessionImportSectionsSnapshot = importSections;
        trackManagerSessionColourOverridesSnapshot = sequenceColourOverrides;
        trackManagerSessionActiveImportSectionIndexSnapshot = activeImportSectionIndex;
        trackManagerSessionSequenceMapFirstIndexSnapshot = sequenceMapFirstIndex;
        trackManagerSessionSequenceMapStartBeatSnapshot = sequenceMapStartBeat;
        trackManagerSessionSequenceMapBeatWindowSnapshot = sequenceMapBeatWindow;
        trackManagerSessionProjectWasDirty = projectDirty;
        trackManagerExternalTrackStateUpdates.clear();
        trackManagerExternalProjectDirtyWhileEditing = false;
    }

    void MainComponent::markTrackManagerEditorDirty(const juce::String& actionLabel)
    {
        if (trackManagerWindow == nullptr)
            return;

        if (!trackManagerEditorDirty)
            trackManagerDirtyReason = actionLabel;
        else if (!actionLabel.isEmpty())
            trackManagerDirtyReason = actionLabel;

        trackManagerEditorDirty = true;
        updateTrackManagerWindowDirtyIndicator();
    }

    void MainComponent::clearTrackManagerEditorDirty()
    {
        trackManagerEditorDirty = false;
        trackManagerDirtyReason.clear();
        updateTrackManagerWindowDirtyIndicator();
    }

    void MainComponent::updateTrackManagerWindowDirtyIndicator()
    {
        if (trackManagerWindow != nullptr)
            trackManagerWindow->setName(trackManagerEditorDirty ? "Track Manager *" : "Track Manager");
    }

    void MainComponent::recordExternalTrackStateUpdate(int trackIndex, bool marksProjectDirty)
    {
        if (trackManagerWindow == nullptr || trackIndex < 0)
            return;

        if (!trackManagerEditorDirty)
        {
            refreshTrackManagerSessionSnapshotIfClean();
            return;
        }

        if (std::find(trackManagerExternalTrackStateUpdates.begin(), trackManagerExternalTrackStateUpdates.end(), trackIndex) == trackManagerExternalTrackStateUpdates.end())
            trackManagerExternalTrackStateUpdates.push_back(trackIndex);

        if (marksProjectDirty)
            trackManagerExternalProjectDirtyWhileEditing = true;
    }

    void MainComponent::preserveExternalTrackStateUpdates(mw::core::Project& restoredProject) const
    {
        if (!currentProject)
            return;

        auto& restoredTracks = restoredProject.getTracks();
        const auto& currentTracks = currentProject->getTracks();

        for (int index : trackManagerExternalTrackStateUpdates)
        {
            if (index < 0
                || index >= static_cast<int>(restoredTracks.size())
                || index >= static_cast<int>(currentTracks.size()))
            {
                continue;
            }

            auto& dst = restoredTracks[static_cast<std::size_t>(index)];
            const auto& src = currentTracks[static_cast<std::size_t>(index)];

            dst.getNotes() = src.getNotes();
            dst.setInstrumentAssignment(src.getInstrument());
            dst.getMixerSettings() = src.getMixerSettings();
            dst.setMuted(src.getMuted());
            dst.setSolo(src.getSolo());
        }
    }

    void MainComponent::applyTrackManagerEditorChanges()
    {
        if (trackManagerWindow == nullptr)
        {
            clearTrackManagerEditorDirty();
            return;
        }

        if (!trackManagerEditorDirty)
        {
            refreshTrackManagerSessionSnapshotIfClean();
            logMessage("No unapplied Track Manager changes to apply.");
            return;
        }

        syncSequencesToProjectMetadata();
        setProjectDirty();
        trackManagerExternalTrackStateUpdates.clear();
        trackManagerExternalProjectDirtyWhileEditing = false;
        clearTrackManagerEditorDirty();
        refreshTrackManagerSessionSnapshotIfClean();
        refreshTrackManagerText();
        refreshMainSequenceSelector();
        refreshActiveSequenceThoughtsEditor();
        updateRenderTargetLabel();
        logMessage("Applied Track Manager changes to the open project. Use Save Project to write them to disk.");
    }

    void MainComponent::discardTrackManagerEditorChanges()
    {
        if (!trackManagerSessionProjectSnapshot)
        {
            clearTrackManagerEditorDirty();
            return;
        }

        auto restoredProject = *trackManagerSessionProjectSnapshot;
        preserveExternalTrackStateUpdates(restoredProject);
        if (currentProject)
            preserveCurrentVstAppliedStates(restoredProject, *currentProject);

        currentProject = std::move(restoredProject);
        importSections = trackManagerSessionImportSectionsSnapshot;
        sequenceColourOverrides = trackManagerSessionColourOverridesSnapshot;
        activeImportSectionIndex = trackManagerSessionActiveImportSectionIndexSnapshot;
        sequenceMapFirstIndex = trackManagerSessionSequenceMapFirstIndexSnapshot;
        sequenceMapStartBeat = trackManagerSessionSequenceMapStartBeatSnapshot;
        sequenceMapBeatWindow = trackManagerSessionSequenceMapBeatWindowSnapshot;
        trackManagerMapStartBeatBox.setText(juce::String(sequenceMapStartBeat), juce::dontSendNotification);
        trackManagerMapBeatWindowBox.setText(sequenceMapBeatWindow <= 0 ? juce::String("Full") : juce::String(sequenceMapBeatWindow), juce::dontSendNotification);

        refreshTrackSelector();
        if (currentProject && !currentProject->getTracks().empty())
        {
            const auto selectedId = std::clamp(trackCombo.getSelectedId(), 1, static_cast<int>(currentProject->getTracks().size()));
            trackCombo.setSelectedId(selectedId, juce::sendNotification);
        }

        syncTrackInspectorFromSelection();
        refreshNoteEditor();
        syncPianoRollFromSelectedTrack();
        if (currentProject)
            updateTrackSummary(*currentProject);
        refreshTrackManagerText();
        refreshMainSequenceSelector();
        refreshActiveSequenceThoughtsEditor();
        updateRenderTargetLabel();
        const bool shouldRemainDirty = trackManagerSessionProjectWasDirty || trackManagerExternalProjectDirtyWhileEditing;
        projectDirty = !shouldRemainDirty;
        setProjectDirty(shouldRemainDirty);
        trackManagerExternalTrackStateUpdates.clear();
        trackManagerExternalProjectDirtyWhileEditing = false;
        clearTrackManagerEditorDirty();
        refreshTrackManagerSessionSnapshotIfClean();

        if (trackManagerContent != nullptr)
        {
            if (auto* content = dynamic_cast<TrackManagerWindowContent*>(trackManagerContent.get()))
                content->refreshSequenceMap();
            else
                trackManagerContent->repaint();
        }

        logMessage("Discarded unapplied Track Manager changes.");
    }

    void MainComponent::finishClosingTrackManagerWindow()
    {
        if (trackManagerWindow != nullptr)
        {
            trackManagerWindow->setVisible(false);
            trackManagerWindow->setContentOwned(nullptr, false);

            juce::MessageManager::callAsync(
                [this]
                {
                    trackManagerWindow.reset();
                    trackManagerContent.reset();
                    clearTrackManagerEditorDirty();
                    trackManagerSessionProjectSnapshot.reset();
                    trackManagerSessionImportSectionsSnapshot.clear();
                    trackManagerSessionColourOverridesSnapshot.clear();
                    trackManagerExternalTrackStateUpdates.clear();
                    trackManagerExternalProjectDirtyWhileEditing = false;
                }
            );
        }
    }

    void MainComponent::closeTrackManagerWindowWithDirtyCheck()
    {
        if (!trackManagerEditorDirty)
        {
            finishClosingTrackManagerWindow();
            return;
        }

        auto* alert = new juce::AlertWindow(
            "Track Manager Has Unapplied Changes",
            "Apply your Track Manager changes to the open project, discard them and close, or cancel and keep editing?",
            juce::AlertWindow::QuestionIcon
        );

        alert->addButton("Apply Changes", 1, juce::KeyPress(juce::KeyPress::returnKey));
        alert->addButton("Discard and Exit", 2);
        alert->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

        alert->enterModalState(
            true,
            juce::ModalCallbackFunction::create(
                [this, alert](int result)
                {
                    std::unique_ptr<juce::AlertWindow> cleanup(alert);

                    if (result == 1)
                    {
                        applyTrackManagerEditorChanges();
                        finishClosingTrackManagerWindow();
                    }
                    else if (result == 2)
                    {
                        discardTrackManagerEditorChanges();
                        finishClosingTrackManagerWindow();
                    }
                    else
                    {
                        logMessage("Track Manager close cancelled.");
                    }
                }
            )
        );
    }

    void MainComponent::addManualTrack()
    {
        if (!currentProject)
        {
            currentProject = mw::core::Project("Manual Project");
            baseNameBox.setText("manual_project");
            captureProjectUserSettings();

            // If the user picked a project-default VST3 backend/plugin before any
            // tracks existed, persist that visible selection into the new project
            // before the first manual track or sequence workflow seeds from it.
            // Otherwise the backend could stay VST3 while the instrument selection
            // appeared to jump back to the first scanned plugin.
            if (appliedProjectBackendId == 3)
                captureProjectDefaultVstPluginSelection();
        }

        if (!canAddAnotherTrack("Add Blank Track"))
            return;

        const bool needsNewSequence = importSections.empty()
            || activeImportSectionIndex < 0
            || activeImportSectionIndex >= static_cast<int>(importSections.size())
            || importSections[static_cast<std::size_t>(activeImportSectionIndex)].locked;

        if (needsNewSequence && static_cast<int>(importSections.size()) >= mw::core::Project::maxSequenceCount)
        {
            logMessage("Sequence limit reached: " + juce::String(mw::core::Project::maxSequenceCount) + ". Add Blank Track could not create a new sequence.");
            return;
        }

        captureTrackManagerUndoState("Add Blank Track");

        const int trackNumber = static_cast<int>(currentProject->getTracks().size()) + 1;
        auto& track = currentProject->addTrack("Blank Track " + std::to_string(trackNumber));

        seedTrackSoundLibraryFromProjectDefaults(track);

        const auto newTrackId = static_cast<int>(currentProject->getTracks().size());

        if (needsNewSequence)
        {
            ImportSectionInfo section;
            section.id = allocateNextSequenceId();
            section.name = makeDefaultSequenceName(std::filesystem::path{}, "Manual");
            section.sourcePath = std::filesystem::path{};
            section.startTick = 0;
            section.endTick = mw::core::Project::ticksPerQuarterNote * 4;
            section.trackNumbers = { newTrackId };
            section.isLayer = false;
            section.createdBy = "manual";

            importSections.push_back(std::move(section));
            activeImportSectionIndex = static_cast<int>(importSections.size()) - 1;
        }
        else
        {
            auto& section = importSections[static_cast<std::size_t>(activeImportSectionIndex)];

            if (std::find(section.trackNumbers.begin(), section.trackNumbers.end(), newTrackId) == section.trackNumbers.end())
                section.trackNumbers.push_back(newTrackId);

            std::sort(section.trackNumbers.begin(), section.trackNumbers.end());
        }

        refreshTrackSelector();
        trackCombo.setSelectedId(newTrackId, juce::sendNotification);
        trackManagerSelectBox.setText(juce::String(newTrackId), juce::dontSendNotification);
        trackManagerSectionBox.setText(juce::String(activeImportSectionIndex + 1), juce::dontSendNotification);

        syncTrackInspectorFromSelection();
        refreshNoteEditor();
        syncPianoRollFromSelectedTrack();
        updateTrackSummary(*currentProject);
        refreshTrackManagerText();
        refreshMainSequenceSelector();
        refreshActiveSequenceThoughtsEditor();
        updateRenderTargetLabel();
        setProjectDirty();

        juce::String message;
        message << "Added blank track #"
                << newTrackId
                << " to sequence #"
                << (activeImportSectionIndex + 1)
                << ": "
                << track.getName()
                << ". Assign an instrument, then edit it in Piano Roll.";

        logMessage(message);
    }

    void MainComponent::duplicateSelectedTrack()
    {
        if (!currentProject)
            return;

        const auto index = getSelectedTrackIndex();

        if (index < 0 || index >= static_cast<int>(currentProject->getTracks().size()))
            return;

        if (!canAddAnotherTrack("Duplicate Track"))
            return;

        const auto sourceSequenceIndex = getSequenceIndexForTrack(index + 1);

        if ((sourceSequenceIndex < 0 || sourceSequenceIndex >= static_cast<int>(importSections.size()))
            && static_cast<int>(importSections.size()) >= mw::core::Project::maxSequenceCount)
        {
            logMessage("Sequence limit reached: " + juce::String(mw::core::Project::maxSequenceCount) + ". Duplicate Track could not create a new sequence.");
            return;
        }

        captureTrackManagerUndoState("Duplicate Track");

        auto copy = currentProject->getTracks()[static_cast<std::size_t>(index)];
        copy.setName(copy.getName() + " Copy");

        currentProject->getTracks().push_back(copy);

        const auto duplicatedTrackNumber = static_cast<int>(currentProject->getTracks().size());

        if (copy.isAudioClipTrack())
        {
            auto existingClips = currentProject->getAudioClips();
            for (auto clip : existingClips)
            {
                if (clip.trackIndex == index)
                {
                    clip.id = currentProject->allocateNextAudioClipId();
                    clip.trackIndex = duplicatedTrackNumber - 1;
                    clip.name += " Copy";
                    currentProject->getAudioClips().push_back(std::move(clip));
                }
            }
        }

        if (sourceSequenceIndex >= 0 && sourceSequenceIndex < static_cast<int>(importSections.size()))
        {
            auto& section = importSections[static_cast<std::size_t>(sourceSequenceIndex)];
            section.trackNumbers.push_back(duplicatedTrackNumber);
            std::sort(section.trackNumbers.begin(), section.trackNumbers.end());
            section.trackNumbers.erase(std::unique(section.trackNumbers.begin(), section.trackNumbers.end()), section.trackNumbers.end());
            activeImportSectionIndex = sourceSequenceIndex;
        }
        else
        {
            ImportSectionInfo section;
            section.id = allocateNextSequenceId();
            section.name = makeDefaultSequenceName(std::filesystem::path{}, "Manual");
            section.startTick = 0;
            section.endTick = mw::core::Project::ticksPerQuarterNote * 4;
            section.trackNumbers = { duplicatedTrackNumber };
            section.createdBy = "manual";
            importSections.push_back(std::move(section));
            activeImportSectionIndex = static_cast<int>(importSections.size()) - 1;
        }

        refreshTrackSelector();
        trackCombo.setSelectedId(duplicatedTrackNumber, juce::sendNotification);
        syncTrackInspectorFromSelection();
        updateTrackSummary(*currentProject);
        refreshTrackManagerText();
        refreshMainSequenceSelector();
        refreshActiveSequenceThoughtsEditor();
        updateRenderTargetLabel();

        setProjectDirty();
        logMessage("Duplicated track: " + copy.getName());
    }

    void MainComponent::removeSelectedTrack()
    {
        if (!currentProject)
            return;

        const auto index = getSelectedTrackIndex();

        if (index < 0 || index >= static_cast<int>(currentProject->getTracks().size()))
            return;

        if (findPianoRollEditorWindow(index) != nullptr)
        {
            juce::AlertWindow::showMessageBoxAsync(
                juce::AlertWindow::WarningIcon,
                "Close Piano Roll First",
                "Track " + juce::String(index + 1) + " is open in Piano Roll. Close that Piano Roll window before removing the track."
            );
            logMessage("Remove Track blocked: track #" + juce::String(index + 1) + " is open in Piano Roll.");
            return;
        }

        captureTrackManagerUndoState("Remove Track");

        const auto removedTrackNumber = index + 1;
        const auto removedName = currentProject->getTracks()[static_cast<std::size_t>(index)].getName();
        const bool removedAudioClipTrack = currentProject->getTracks()[static_cast<std::size_t>(index)].isAudioClipTrack();
        currentProject->getTracks().erase(currentProject->getTracks().begin() + index);

        auto& audioClips = currentProject->getAudioClips();
        audioClips.erase(
            std::remove_if(
                audioClips.begin(),
                audioClips.end(),
                [index](const mw::core::AudioClip& clip) { return clip.trackIndex == index; }
            ),
            audioClips.end()
        );
        for (auto& clip : audioClips)
        {
            if (clip.trackIndex > index)
                --clip.trackIndex;
        }

        if (!pianoRollEditorWindows.empty())
        {
            std::map<int, std::unique_ptr<PianoRollEditorWindowState>> rebuiltPianoRollWindows;

            for (auto& entry : pianoRollEditorWindows)
            {
                if (entry.second == nullptr)
                    continue;

                if (entry.second->trackIndex > index)
                    --entry.second->trackIndex;

                rebuiltPianoRollWindows[entry.second->trackIndex] = std::move(entry.second);
            }

            pianoRollEditorWindows = std::move(rebuiltPianoRollWindows);
            updateOpenPianoRollTrackNames();
        }

        std::vector<ImportSectionInfo> rebuiltSections;
        std::map<int, juce::Colour> rebuiltColours;

        for (int oldIndex = 0; oldIndex < static_cast<int>(importSections.size()); ++oldIndex)
        {
            auto section = importSections[static_cast<std::size_t>(oldIndex)];
            std::vector<int> adjustedTrackNumbers;

            for (auto trackNumber : section.trackNumbers)
            {
                if (trackNumber == removedTrackNumber)
                    continue;

                if (trackNumber > removedTrackNumber)
                    --trackNumber;

                if (trackNumber > 0)
                    adjustedTrackNumbers.push_back(trackNumber);
            }

            std::sort(adjustedTrackNumbers.begin(), adjustedTrackNumbers.end());
            adjustedTrackNumbers.erase(std::unique(adjustedTrackNumbers.begin(), adjustedTrackNumbers.end()), adjustedTrackNumbers.end());

            if (adjustedTrackNumbers.empty())
                continue;

            section.trackNumbers = std::move(adjustedTrackNumbers);
            const auto newIndex = static_cast<int>(rebuiltSections.size());

            if (auto colourIt = sequenceColourOverrides.find(oldIndex); colourIt != sequenceColourOverrides.end())
                rebuiltColours[newIndex] = colourIt->second;

            rebuiltSections.push_back(std::move(section));
        }

        importSections = std::move(rebuiltSections);
        sequenceColourOverrides = std::move(rebuiltColours);
        activeImportSectionIndex = importSections.empty()
            ? -1
            : std::clamp(activeImportSectionIndex, 0, static_cast<int>(importSections.size()) - 1);

        refreshTrackSelector();

        if (!currentProject->getTracks().empty())
            trackCombo.setSelectedId(std::min(index + 1, static_cast<int>(currentProject->getTracks().size())), juce::sendNotification);

        syncTrackInspectorFromSelection();
        updateTrackSummary(*currentProject);
        refreshTrackManagerText();
        refreshMainSequenceSelector();
        refreshActiveSequenceThoughtsEditor();
        updateRenderTargetLabel();

        setProjectDirty();
        logMessage("Removed track: " + removedName + (removedAudioClipTrack ? " (AudioClip metadata removed; media file left in project folder)." : ""));
    }

    void MainComponent::renameSelectedTrack()
    {
        if (!currentProject)
        {
            logMessage("ERROR: No project loaded.");
            return;
        }

        const auto index = getSelectedTrackIndex();

        if (index < 0 || index >= static_cast<int>(currentProject->getTracks().size()))
        {
            logMessage("ERROR: Select a track to rename.");
            return;
        }

        auto& track = currentProject->getTracks()[static_cast<std::size_t>(index)];

        auto* alert = new juce::AlertWindow(
            "Rename Track",
            "Enter a new name for the selected track.",
            juce::AlertWindow::InfoIcon
        );

        alert->addTextEditor("trackName", track.getName(), "Track Name:");
        alert->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
        alert->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

        alert->enterModalState(
            true,
            juce::ModalCallbackFunction::create(
                [this, alert, index](int result)
                {
                    std::unique_ptr<juce::AlertWindow> cleanup(alert);

                    if (result != 1 || !currentProject)
                        return;

                    if (index < 0 || index >= static_cast<int>(currentProject->getTracks().size()))
                        return;

                    auto newName = alert->getTextEditorContents("trackName").trim();

                    if (newName.isEmpty())
                    {
                        logMessage("Track rename cancelled: name was empty.");
                        return;
                    }

                    const auto applyRename = [this, index, newName]
                    {
                        if (!currentProject)
                            return;

                        if (index < 0 || index >= static_cast<int>(currentProject->getTracks().size()))
                            return;

                        auto& track = currentProject->getTracks()[static_cast<std::size_t>(index)];
                        track.setName(newName.toStdString());

                        suppressTrackComboSwitchPrompt = true;
                        refreshTrackSelector();
                        trackCombo.setSelectedId(index + 1, juce::dontSendNotification);
                        suppressTrackComboSwitchPrompt = false;

                        syncTrackInspectorFromSelection();


                        updateOpenPianoRollTrackNames();

                        updateTrackSummary(*currentProject);
                        setProjectDirty();
                        logMessage("Renamed track to: " + newName);
                    };

                    bool duplicateName = false;

                    const auto& tracks = currentProject->getTracks();

                    for (int i = 0; i < static_cast<int>(tracks.size()); ++i)
                    {
                        if (i == index)
                            continue;

                        if (juce::String(tracks[static_cast<std::size_t>(i)].getName()).equalsIgnoreCase(newName))
                        {
                            duplicateName = true;
                            break;
                        }
                    }

                    if (!duplicateName)
                    {
                        applyRename();
                        return;
                    }

                    auto* duplicateAlert = new juce::AlertWindow(
                        "Duplicate Track Name",
                        "Another track already uses this name. Use it anyway?",
                        juce::AlertWindow::WarningIcon
                    );

                    duplicateAlert->addButton("Use Anyway", 1, juce::KeyPress(juce::KeyPress::returnKey));
                    duplicateAlert->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

                    duplicateAlert->enterModalState(
                        true,
                        juce::ModalCallbackFunction::create(
                            [duplicateAlert, applyRename](int duplicateResult)
                            {
                                std::unique_ptr<juce::AlertWindow> duplicateCleanup(duplicateAlert);

                                if (duplicateResult == 1)
                                    applyRename();
                            }
                        )
                    );
                }
            )
        );
    }


    void MainComponent::chooseMusicXml()
    {
        if (projectDirty || pianoRollEditorDirty || trackManagerEditorDirty)
        {
            confirmDiscardUnsavedChanges(
                "start from file and replace the current project",
                [this]
                {
                    chooseMusicXmlAfterUnsavedCheck();
                }
            );
            return;
        }

        chooseMusicXmlAfterUnsavedCheck();
    }

    void MainComponent::chooseMusicXmlAfterUnsavedCheck()
    {
        activeFileChooser = std::make_unique<juce::FileChooser>(
            "Choose a MusicXML file",
            juce::File(mw::app::UserPreferencesStore::load().lastInputFolder.string()),
            "*.musicxml;*.xml;*.mxl;*.mid;*.midi"
        );

        activeFileChooser->launchAsync(
            juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
            [this](const juce::FileChooser& chooser)
            {
                const auto file = chooser.getResult();
                if (file == juce::File{}) return;

                musicXmlPathBox.setText(file.getFullPathName());

                // A new input file should produce outputs based on the new score name.
                // This prevents stale exports such as rendering a new .mxl file using
                // the previous file's base name.
                baseNameBox.setText(file.getFileNameWithoutExtension());
                metadataTitleBox.setText(file.getFileNameWithoutExtension(), juce::dontSendNotification);
                metadataArtistBox.clear();
                metadataAlbumBox.clear();
                metadataTrackNumberBox.clear();
                metadataYearBox.clear();

                currentProject.reset();
                currentProjectFilePath.reset();
                currentProjectFolderPath.reset();
                importSections.clear();
                activeImportSectionIndex = -1;
                sequenceColourOverrides.clear();
                trackCombo.clear(juce::dontSendNotification);
                trackSummaryBox.setText("Importing new score...");

                logMessage("Selected MusicXML: " + file.getFullPathName());
                logMessage("Base filename updated to: " + file.getFileNameWithoutExtension());

                clearProjectDirty();

                mw::app::UserPreferencesStore::saveValue("lastInputFolder", file.getParentDirectory().getFullPathName().toStdString());

                importMusicXmlOnly();
            }
        );
    }

    void MainComponent::chooseExportFolder()
    {
        activeFileChooser = std::make_unique<juce::FileChooser>(
            "Choose export folder",
            juce::File(exportFolderBox.getText()),
            "*"
        );

        activeFileChooser->launchAsync(
            juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
            [this](const juce::FileChooser& chooser)
            {
                const auto folder = chooser.getResult();
                if (folder == juce::File{}) return;

                exportFolderBox.setText(folder.getFullPathName());
                logMessage("Selected export folder: " + folder.getFullPathName());

                mw::app::UserPreferencesStore::saveValue("lastExportFolder", folder.getFullPathName().toStdString());
            }
        );
    }

    void MainComponent::chooseSoundFont()
    {
        activeFileChooser = std::make_unique<juce::FileChooser>(
            "Choose a SoundFont file",
            juce::File(mw::app::AppPaths::soundFontsFolder().string()),
            "*.sf2;*.sf3"
        );

        activeFileChooser->launchAsync(
            juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
            [this](const juce::FileChooser& chooser)
            {
                const auto file = chooser.getResult();
                if (file == juce::File{}) return;

                soundFontPathBox.setText(file.getFullPathName());
                refreshSoundFontList();
                refreshPresetListFromSelectedSoundFont();
                logMessage("Selected SoundFont: " + file.getFullPathName());

                mw::app::UserPreferencesStore::saveValue("lastSoundFontPath", file.getFullPathName().toStdString());
            }
        );
    }


void MainComponent::chooseSfzFile()
    {
        activeFileChooser = std::make_unique<juce::FileChooser>(
            "Choose SFZ instrument",
            juce::File(mw::app::AppPaths::sfzFolder().string()),
            "*.sfz"
        );

        activeFileChooser->launchAsync(
            juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
            [this](const juce::FileChooser& chooser)
            {
                const auto file = chooser.getResult();
                if (file == juce::File{}) return;

                sfzPathBox.setText(file.getFullPathName());
                refreshSfzList();

                // Selecting a new SFZ should immediately refresh the visible instrument
                // option and update the currently selected track when in SFZ mode.
                populateInstrumentCombo();

                refreshTrackSoundLibraryDisplay();
                syncTrackInspectorFromSelection();
                logMessage("Selected SFZ: " + file.getFullPathName());
            }
        );
    }

void MainComponent::importMusicXmlOnly()
    {
        const std::filesystem::path musicXmlPath = musicXmlPathBox.getText().toStdString();

        if (musicXmlPath.empty())
        {
            logMessage("ERROR: Choose a MusicXML file first.");
            return;
        }

        auto extension = musicXmlPath.extension().string();

        for (auto& c : extension)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        std::optional<mw::core::Project> project;

        if (extension == ".mid" || extension == ".midi")
        {
            project = mw::import_export::MidiImporter::importFromFile(musicXmlPath);

            if (!project)
            {
                logMessage("ERROR: Failed to import MIDI.");
                return;
            }
        }
        else
        {
            project = mw::import_export::MusicXmlImporter::importFromFile(musicXmlPath);

            if (!project)
            {
                logMessage("ERROR: Failed to import MusicXML/MXL.");
                return;
            }
        }

        if (!enforceProjectTrackLimit(*project, "Imported file"))
            return;

        currentProject = *project;
        currentProjectFilePath.reset();
        currentProjectFolderPath.reset();
        if (!baseNameBox.getText().trim().isEmpty())
            currentProject->setName(baseNameBox.getText().trim().toStdString());
        captureProjectUserSettings();
        seedMissingTrackSoundLibrariesFromProjectDefaults(*currentProject);

        importSections.clear();
        activeImportSectionIndex = -1;

        std::vector<int> importedTrackNumbers;
        for (int i = 0; i < static_cast<int>(currentProject->getTracks().size()); ++i)
            importedTrackNumbers.push_back(i + 1);

        recordImportSection(
            musicXmlPath.filename().string(),
            musicXmlPath,
            0,
            getProjectEndTick(),
            importedTrackNumbers,
            false,
            "input"
        );

        if (baseNameBox.getText().isEmpty() || baseNameBox.getText() == "rendered_score")
            baseNameBox.setText(musicXmlPath.stem().string());

        if (currentProject && !baseNameBox.getText().trim().isEmpty())
        {
            currentProject->setName(baseNameBox.getText().trim().toStdString());
            captureProjectUserSettings();
        }

        refreshTrackSelector();
        syncTrackInspectorFromSelection();
        tempoBox.setText(juce::String(currentProject->getTempoBpm()), juce::dontSendNotification);
        pianoRollBpmBox.setText(juce::String(currentProject->getTempoBpm()), juce::dontSendNotification);
        juce::String ts;
        ts << currentProject->getTimeSignature().numerator << "/" << currentProject->getTimeSignature().denominator;
        timeSignatureBox.setText(ts, juce::dontSendNotification);
        pianoRollTimeSigBox.setText(ts, juce::dontSendNotification);
        refreshNoteEditor();
        fitPianoRollToSelectedTrack();
        updateTrackSummary(*currentProject);

        logMessage("Imported project: " + currentProject->getName());
    }


    std::filesystem::path MainComponent::getCurrentProjectFolder() const
    {
        if (currentProjectFolderPath && !currentProjectFolderPath->empty())
            return *currentProjectFolderPath;

        const juce::String baseName = baseNameBox.getText().trim().isEmpty()
            ? juce::String(currentProject ? currentProject->getName() : "Untitled Project")
            : baseNameBox.getText().trim();

        return mw::app::AppPaths::projectFolderForName(baseName.toStdString());
    }

    bool MainComponent::exportFolderShouldFollowProjectFolder(const std::filesystem::path& nextProjectFolder) const
    {
        const auto currentExportFolder = std::filesystem::path(exportFolderBox.getText().toStdString());

        if (currentExportFolder.empty())
            return true;

        if (pathsReferToSameLocation(currentExportFolder, mw::app::AppPaths::exportsFolder()))
            return true;

        if (pathsReferToSameLocation(currentExportFolder, nextProjectFolder / "renders")
            || pathsReferToSameLocation(currentExportFolder, nextProjectFolder / "output"))
            return true;

        if (currentProjectFolderPath)
        {
            if (pathsReferToSameLocation(currentExportFolder, *currentProjectFolderPath / "renders")
                || pathsReferToSameLocation(currentExportFolder, *currentProjectFolderPath / "output"))
            {
                return true;
            }
        }

        return false;
    }

    void MainComponent::syncProjectIdentityToFile(const juce::File& file)
    {
        const auto projectName = file.getFileNameWithoutExtension().trim();
        const auto projectFilePath = std::filesystem::path(file.getFullPathName().toStdString());
        const auto projectFolderPath = std::filesystem::path(file.getParentDirectory().getFullPathName().toStdString());

        currentProjectFilePath = projectFilePath;
        currentProjectFolderPath = projectFolderPath;

        if (currentProject && !projectName.isEmpty())
            currentProject->setName(projectName.toStdString());

        if (!projectName.isEmpty())
            baseNameBox.setText(projectName, juce::dontSendNotification);
    }

    bool MainComponent::ensureProjectFolderReadyForAudio()
    {
        if (!currentProject)
        {
            currentProject = mw::core::Project("Untitled Project");
            baseNameBox.setText("Untitled Project", juce::dontSendNotification);
            logMessage("Created a blank project for AudioClip work.");
        }

        const auto projectFolder = getCurrentProjectFolder();
        std::error_code ec;
        std::filesystem::create_directories(projectFolder / "input" / "midi", ec);
        std::filesystem::create_directories(projectFolder / "input" / "mxl", ec);
        std::filesystem::create_directories(projectFolder / "input" / "audio" / "imported", ec);
        std::filesystem::create_directories(projectFolder / "input" / "audio" / "recorded", ec);
        std::filesystem::create_directories(projectFolder / "input" / "audio" / "temp", ec);
        std::filesystem::create_directories(projectFolder / "output", ec);
        std::filesystem::create_directories(projectFolder / "renders", ec);

        if (ec)
        {
            logMessage("ERROR: Could not create project media folders: " + juce::String(ec.message()));
            return false;
        }

        return true;
    }

    mw::core::AudioClipSavedFormat MainComponent::getSelectedAudioClipFormat() const
    {
        return mw::core::audioClipSavedFormatFromId(audioClipFormatCombo.getSelectedId() > 0 ? audioClipFormatCombo.getSelectedId() : 1);
    }

    int MainComponent::getSelectedAudioClipQualityKbps() const
    {
        const int selected = audioClipQualityCombo.getSelectedId();
        return selected > 0 ? selected : 320;
    }

    int MainComponent::createAudioClipTrackForNewClip(const juce::String& baseName)
    {
        if (!currentProject)
            return -1;

        auto& tracks = currentProject->getTracks();

        if (!canAddAnotherTrack("Add AudioClip Track"))
            return -1;

        const int nextNumber = static_cast<int>(tracks.size()) + 1;
        juce::String trackName = baseName.trim();
        if (trackName.isEmpty())
            trackName = "AudioClip";
        trackName << " " << nextNumber;

        auto& track = currentProject->addTrack(trackName.toStdString());
        track.setTrackType(mw::core::TrackType::AudioClip);
        track.setInstrumentAssignment(mw::core::makeCustomAudioInstrumentAssignment());

        const int index = static_cast<int>(tracks.size()) - 1;
        refreshTrackSelector();
        trackCombo.setSelectedId(index + 1, juce::sendNotification);
        logMessage(juce::String("Created AudioClip track #") + juce::String(index + 1) + " using Custom Audio.");
        setProjectDirty();
        return index;
    }

    void MainComponent::addAudioClipToProject(mw::core::AudioClip clip)
    {
        if (!currentProject)
            return;

        if (clip.id <= 0)
            clip.id = currentProject->allocateNextAudioClipId();

        if (clip.trackIndex >= 0 && clip.trackIndex < static_cast<int>(currentProject->getTracks().size()))
        {
            auto& track = currentProject->getTracks()[static_cast<std::size_t>(clip.trackIndex)];
            track.setTrackType(mw::core::TrackType::AudioClip);
            track.setInstrumentAssignment(mw::core::makeCustomAudioInstrumentAssignment());
            if (!clip.name.empty())
                track.setName(clip.name);
        }

        currentProject->getAudioClips().push_back(std::move(clip));
        syncSequencesToProjectMetadata();
        refreshTrackSelector();
        updateTrackSummary(*currentProject);
        refreshTrackManagerText(false);
        refreshAudioRecorderWindowStatus();
        setProjectDirty();
    }

    juce::String MainComponent::getAudioClipSummaryForTrack(int trackIndex) const
    {
        if (!currentProject)
            return {};

        juce::String summary;
        int count = 0;

        for (const auto& clip : currentProject->getAudioClips())
        {
            if (clip.trackIndex != trackIndex)
                continue;

            ++count;
            summary << "  AudioClip #" << clip.id << ": " << clip.name
                    << " | Seq #" << clip.sequenceNumber
                    << " | " << mw::core::audioClipSavedFormatToString(clip.savedFormat).c_str()
                    << " | " << formatSecondsFromSamples(clip.durationSamples, clip.sampleRate)
                    << " | " << formatBytes(clip.sizeBytes)
                    << (clip.missingMedia ? " | MISSING MEDIA" : "")
                    << "\n";
        }

        if (count == 0)
            return "  AudioClips: none\n";

        return "  AudioClips: " + juce::String(count) + "\n" + summary;
    }

    juce::String MainComponent::getAudioClipSummaryForSequence(int sequenceNumber) const
    {
        if (!currentProject || sequenceNumber <= 0)
            return {};

        juce::String summary;
        int count = 0;

        for (const auto& clip : currentProject->getAudioClips())
        {
            if (clip.sequenceNumber != sequenceNumber)
                continue;

            ++count;
            summary << "  AudioClip #" << clip.id << ": " << clip.name
                    << " | Track #" << (clip.trackIndex + 1)
                    << " | " << mw::core::audioClipSourceTypeToString(clip.sourceType).c_str()
                    << " | " << formatSecondsFromSamples(clip.durationSamples, clip.sampleRate)
                    << " | " << formatBytes(clip.sizeBytes)
                    << (clip.missingMedia ? " | MISSING MEDIA" : "")
                    << "\n";
        }

        if (count == 0)
            return "  AudioClips: none\n";

        return "  AudioClips: " + juce::String(count) + "\n" + summary;
    }

    bool MainComponent::sequenceHasAudioClips(int sequenceNumber) const
    {
        if (!currentProject || sequenceNumber <= 0)
            return false;

        for (const auto& clip : currentProject->getAudioClips())
        {
            if (clip.sequenceNumber == sequenceNumber)
                return true;
        }

        return false;
    }

    void MainComponent::importAudioFile()
    {
        if (!ensureProjectFolderReadyForAudio())
            return;

        activeFileChooser = std::make_unique<juce::FileChooser>(
            "Import Audio",
            juce::File(mw::app::AppPaths::inputFolder().string()),
            "*.wav;*.mp3;*.flac;*.ogg"
        );

        activeFileChooser->launchAsync(
            juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
            [this](const juce::FileChooser& chooser)
            {
                const auto file = chooser.getResult();
                if (file == juce::File{})
                    return;

                const auto sourcePath = std::filesystem::path(file.getFullPathName().toStdString());
                const int trackIndex = createAudioClipTrackForNewClip(juce::String(sourcePath.stem().string()));
                if (trackIndex < 0)
                    return;

                if (activeImportSectionIndex < 0 || activeImportSectionIndex >= static_cast<int>(importSections.size()))
                {
                    recordImportSection(
                        "AudioClip",
                        sourcePath,
                        0,
                        mw::core::Project::ticksPerQuarterNote * 4,
                        { trackIndex + 1 },
                        false,
                        "audio-import"
                    );
                }
                else
                {
                    auto& section = importSections[static_cast<std::size_t>(activeImportSectionIndex)];
                    if (std::find(section.trackNumbers.begin(), section.trackNumbers.end(), trackIndex + 1) == section.trackNumbers.end())
                    {
                        section.trackNumbers.push_back(trackIndex + 1);
                        std::sort(section.trackNumbers.begin(), section.trackNumbers.end());
                    }
                }

                const auto projectFolder = getCurrentProjectFolder();
                mw::audio::AudioClipImportRequest request;
                request.sourcePath = sourcePath;
                request.projectFolder = projectFolder;
                request.ffmpegExePath = std::filesystem::path(ffmpegPathBox.getText().toStdString());
                request.savedFormat = getSelectedAudioClipFormat();
                request.qualityKbps = getSelectedAudioClipQualityKbps();
                request.channelCount = channelsCombo.getSelectedId() == 1 ? 1 : 2;
                request.imported = true;
                request.preferredName = sourcePath.stem().string();

                const auto result = mw::audio::AudioClipImporter::importToProject(request);
                if (!result.success)
                {
                    logMessage(juce::String("ERROR: ") + result.message);
                    return;
                }

                if (importSections.empty())
                {
                    logMessage("ERROR: AudioClip import could not find an active sequence.");
                    return;
                }

                const int sequenceNumber = std::clamp(activeImportSectionIndex + 1, 1, static_cast<int>(importSections.size()));
                const auto& section = importSections[static_cast<std::size_t>(sequenceNumber - 1)];

                mw::core::AudioClip clip;
                clip.name = safeClipDisplayName(result.absolutePath).toStdString();
                clip.trackIndex = trackIndex;
                clip.sequenceNumber = sequenceNumber;
                clip.sourceType = mw::core::AudioClipSourceType::Imported;
                clip.savedFormat = getSelectedAudioClipFormat();
                clip.projectRelativePath = result.relativePath;
                clip.originalSourcePath = sourcePath;
                clip.startTick = section.startTick;
                clip.sampleRate = static_cast<double>(sampleRateCombo.getSelectedId() > 0 ? sampleRateCombo.getSelectedId() : 48000);
                clip.channelCount = channelsCombo.getSelectedId() == 1 ? 1 : 2;
                clip.bitDepth = 24;
                clip.sizeBytes = result.sizeBytes;

                addAudioClipToProject(std::move(clip));
                logMessage(juce::String("Imported AudioClip to track #") + juce::String(trackIndex + 1) + " / seq #" + juce::String(sequenceNumber) + ": " + result.relativePath.string());
            }
        );
    }

    void MainComponent::openAudioRecorderWindow()
    {
        if (!ensureProjectFolderReadyForAudio())
            return;

        if (audioRecorderWindow != nullptr)
        {
            audioRecorderWindow->toFront(true);
            return;
        }

        audioClipRecorder = std::make_unique<mw::audio::AudioClipRecorder>();
        audioRecordingTakeStopped = false;
        audioRecordingTakeDirty = false;

        auto closeWindow = [this] { closeAudioRecorderWindowWithPrompt(); };
        auto* window = new PianoRollDocumentWindow("AudioClip Recorder", closeWindow);
        audioRecorderWindow.reset(window);

        auto content = std::make_unique<AudioRecorderWindowContent>(
            [this] { startAudioRecordingTake(); },
            [this] { startAudioRecorderTest(); },
            [this](double gainDb) { setAudioRecorderMicGainDb(gainDb); },
            [this] { pauseOrResumeAudioRecordingTake(); },
            [this] { stopAudioRecordingTake(); },
            [this] { keepAudioRecordingTake(); },
            [this] { redoAudioRecordingTake(); },
            [this] { discardAudioRecordingTake(); },
            [this] { closeAudioRecorderWindowWithPrompt(); },
            [this](const juce::String& deviceName) { selectAudioRecorderInputDevice(deviceName); },
            [this] { refreshAudioRecorderInputDevices(); }
        );

        content->onRefresh = [this] { refreshAudioRecorderWindowStatus(); };
        audioRecorderContent = std::move(content);
        window->setContentNonOwned(audioRecorderContent.get(), false);
        window->centreWithSize(920, 370);
        window->setIcon(createMicrophoneWindowIconImage());
        window->setVisible(true);
        if (auto* peer = window->getPeer())
            peer->setIcon(createMicrophoneWindowIconImage());
        refreshAudioRecorderInputDevices();
        setAudioRecorderMicGainDb(audioRecorderMicGainDb);
        refreshAudioRecorderWindowStatus();
        logMessage("Opened AudioClip Recorder. Choose an input device, then Record, Record With Delay, or Record Test.");
    }

    void MainComponent::refreshAudioRecorderInputDevices()
    {
        if (!audioClipRecorder || !audioRecorderContent)
            return;

        auto* content = dynamic_cast<AudioRecorderWindowContent*>(audioRecorderContent.get());
        if (content == nullptr)
            return;

        const auto devices = audioClipRecorder->getAvailableInputDeviceNames();
        content->setInputDevices(devices, audioClipRecorder->getPreferredInputDeviceName());
        if (devices.isEmpty())
            logMessage("AudioClip Recorder: no input devices detected. Connect/enable a microphone, then click Refresh Devices.");
        else
            logMessage("AudioClip Recorder: detected " + juce::String(devices.size()) + " input device(s). Choose one from the Input dropdown; the selection is applied immediately.");
    }

    void MainComponent::selectAudioRecorderInputDevice(const juce::String& deviceName)
    {
        if (!audioClipRecorder)
            return;

        if (audioClipRecorder->isRecording())
        {
            logMessage(audioRecorderTestActive
                ? "AudioClip Recorder: wait for Record Test to finish before changing input devices."
                : "AudioClip Recorder: stop the current take before changing input devices.");
            refreshAudioRecorderInputDevices();
            return;
        }

        audioClipRecorder->setPreferredInputDeviceName(deviceName);
        logMessage(deviceName.isEmpty()
            ? juce::String("AudioClip Recorder input set to System Default.")
            : juce::String("AudioClip Recorder input set to: ") + deviceName);
        refreshAudioRecorderWindowStatus();
    }

    void MainComponent::setAudioRecorderMicGainDb(double gainDb)
    {
        audioRecorderMicGainDb = juce::jlimit(-24.0, 24.0, gainDb);
        if (audioClipRecorder)
            audioClipRecorder->setInputGainDb(audioRecorderMicGainDb);

        if (auto* content = dynamic_cast<AudioRecorderWindowContent*>(audioRecorderContent.get()))
            content->setMicGainDb(audioRecorderMicGainDb);

        logMessage("AudioClip Recorder mic gain set to " + juce::String(audioRecorderMicGainDb, 1) + " dB.");
        refreshAudioRecorderWindowStatus();
    }

    void MainComponent::startAudioRecorderTest()
    {
        if (audioRecorderTestActive || audioRecorderTestPlaybackActive)
        {
            logMessage("AudioClip Recorder: Record Test is already running. Wait for automatic playback and cleanup to finish.");
            return;
        }

        if (audioRecordingTakeDirty || (audioClipRecorder && audioClipRecorder->isRecording()))
        {
            logMessage("AudioClip Recorder: save/apply, redo, or discard the current take before running Record Test.");
            refreshAudioRecorderWindowStatus();
            return;
        }

        if (!ensureProjectFolderReadyForAudio())
            return;

        const auto projectFolder = getCurrentProjectFolder();
        const auto tempFolder = mw::audio::AudioClipImporter::tempAudioFolderFor(projectFolder);
        std::string spaceMessage;
        const auto minFreeBytes = static_cast<std::uintmax_t>(128u) * 1024u * 1024u;
        if (!mw::audio::AudioClipImporter::hasEnoughFreeSpace(tempFolder, minFreeBytes, spaceMessage))
        {
            logMessage(juce::String("ERROR: ") + spaceMessage);
            return;
        }

        std::error_code ec;
        std::filesystem::create_directories(tempFolder, ec);
        if (!audioRecorderTestTempWavPath.empty())
            std::filesystem::remove(audioRecorderTestTempWavPath, ec);
        audioRecorderTestTempWavPath = mw::audio::AudioClipImporter::makeUniqueMediaPath(tempFolder, "recording_test", mw::core::AudioClipSavedFormat::Wav);

        if (!audioClipRecorder)
            audioClipRecorder = std::make_unique<mw::audio::AudioClipRecorder>();
        audioClipRecorder->setInputGainDb(audioRecorderMicGainDb);

        const auto startResult = audioClipRecorder->startRecording(audioRecorderTestTempWavPath, channelsCombo.getSelectedId() == 1 ? 1 : 2, 24);
        if (!startResult.success)
        {
            logMessage(juce::String("ERROR: AudioClip Record Test did not start: ") + startResult.message);
            std::filesystem::remove(audioRecorderTestTempWavPath, ec);
            audioRecorderTestTempWavPath.clear();
            audioRecorderTestActive = false;
            refreshAudioRecorderWindowStatus();
            return;
        }

        audioRecorderTestActive = true;
        audioRecorderTestPlaybackActive = false;
        logMessage("AudioClip Record Test: recording 5 second test sample.");
        refreshAudioRecorderWindowStatus();

        juce::Component::SafePointer<MainComponent> safeThis(this);
        juce::Timer::callAfterDelay(5000, [safeThis]() mutable
        {
            if (safeThis != nullptr)
                safeThis->finishAudioRecorderTestRecording();
        });
    }

    void MainComponent::finishAudioRecorderTestRecording()
    {
        if (!audioRecorderTestActive)
            return;

        if (audioClipRecorder && audioClipRecorder->isRecording())
            audioClipRecorder->stop();

        audioRecorderTestActive = false;
        audioRecorderTestPlaybackActive = false;
        logMessage("AudioClip Record Test: recording complete. Playing test back once, then deleting the temp file.");
        refreshAudioRecorderWindowStatus();
        playAudioRecorderTestAndCleanup(audioRecorderTestTempWavPath);
    }

    void MainComponent::playAudioRecorderTestAndCleanup(const std::filesystem::path& testPath)
    {
        if (audioRecorderTestReaderSource)
        {
            audioRecorderTestTransport.stop();
            audioRecorderTestSourcePlayer.setSource(nullptr);
            audioRecorderTestPlaybackDeviceManager.removeAudioCallback(&audioRecorderTestSourcePlayer);
            audioRecorderTestTransport.setSource(nullptr);
            audioRecorderTestReaderSource.reset();
            audioRecorderTestPlaybackDeviceManager.closeAudioDevice();
            audioRecorderTestPlaybackActive = false;
        }

        if (testPath.empty())
        {
            audioRecorderTestPlaybackActive = false;
            refreshAudioRecorderWindowStatus();
            return;
        }

        if (audioRecorderTestFormatManager.getNumKnownFormats() == 0)
            audioRecorderTestFormatManager.registerBasicFormats();
        juce::File file(testPath.string());
        std::unique_ptr<juce::AudioFormatReader> reader(audioRecorderTestFormatManager.createReaderFor(file));
        if (!reader)
        {
            logMessage("AudioClip Record Test: could not open the test WAV for playback; deleting temp file.");
            stopAudioRecorderTestPlaybackAndCleanup(testPath);
            return;
        }

        const double readerSampleRate = reader->sampleRate > 0.0 ? reader->sampleRate : 48000.0;
        const double durationSeconds = reader->lengthInSamples > 0 ? static_cast<double>(reader->lengthInSamples) / readerSampleRate : 5.0;
        audioRecorderTestReaderSource = std::make_unique<juce::AudioFormatReaderSource>(reader.release(), true);
        audioRecorderTestTransport.setSource(audioRecorderTestReaderSource.get(), 0, nullptr, readerSampleRate);

        const auto initError = audioRecorderTestPlaybackDeviceManager.initialise(0, 2, nullptr, true);
        if (initError.isNotEmpty())
        {
            logMessage("AudioClip Record Test: playback device did not open; deleting temp file. " + initError);
            stopAudioRecorderTestPlaybackAndCleanup(testPath);
            return;
        }

        audioRecorderTestSourcePlayer.setSource(&audioRecorderTestTransport);
        audioRecorderTestPlaybackDeviceManager.addAudioCallback(&audioRecorderTestSourcePlayer);
        audioRecorderTestTransport.setPosition(0.0);
        audioRecorderTestPlaybackActive = true;
        audioRecorderTestTransport.start();
        refreshAudioRecorderWindowStatus();

        const int cleanupDelayMs = juce::jlimit(1000, 30000, static_cast<int>(std::ceil(durationSeconds * 1000.0)) + 700);
        juce::Component::SafePointer<MainComponent> safeThis(this);
        juce::Timer::callAfterDelay(cleanupDelayMs, [safeThis, testPath]() mutable
        {
            if (safeThis != nullptr)
                safeThis->stopAudioRecorderTestPlaybackAndCleanup(testPath);
        });
    }

    void MainComponent::stopAudioRecorderTestPlaybackAndCleanup(const std::filesystem::path& testPath)
    {
        if (audioRecorderTestPlaybackActive || audioRecorderTestReaderSource)
        {
            audioRecorderTestTransport.stop();
            audioRecorderTestSourcePlayer.setSource(nullptr);
            audioRecorderTestPlaybackDeviceManager.removeAudioCallback(&audioRecorderTestSourcePlayer);
            audioRecorderTestTransport.setSource(nullptr);
            audioRecorderTestReaderSource.reset();
            audioRecorderTestPlaybackDeviceManager.closeAudioDevice();
        }

        std::error_code ec;
        const auto pathToRemove = testPath.empty() ? audioRecorderTestTempWavPath : testPath;
        if (!pathToRemove.empty())
            std::filesystem::remove(pathToRemove, ec);

        if (pathToRemove == audioRecorderTestTempWavPath || !testPath.empty())
            audioRecorderTestTempWavPath.clear();

        if (audioRecorderTestPlaybackActive)
            logMessage("AudioClip Record Test: playback finished and temp file deleted.");

        audioRecorderTestPlaybackActive = false;
        refreshAudioRecorderWindowStatus();
    }

    void MainComponent::cancelAudioRecorderTestAndCleanup()
    {
        if (audioRecorderTestActive && audioClipRecorder && audioClipRecorder->isRecording())
            audioClipRecorder->stop();

        audioRecorderTestActive = false;
        stopAudioRecorderTestPlaybackAndCleanup(audioRecorderTestTempWavPath);
    }

    void MainComponent::refreshAudioRecorderWindowStatus()
    {
        if (!audioRecorderContent)
            return;

        auto* content = dynamic_cast<AudioRecorderWindowContent*>(audioRecorderContent.get());
        if (content == nullptr)
            return;

        juce::String status;
        status << "Target: ";

        if (activeRecordingTrackIndex >= 0)
            status << "Track #" << (activeRecordingTrackIndex + 1) << " | Seq #" << activeRecordingSequenceNumber;
        else
            status << "new AudioClip track / active sequence";

        status << "\nFormat: " << mw::core::audioClipSavedFormatToString(getSelectedAudioClipFormat()).c_str()
               << " | Quality: " << getSelectedAudioClipQualityKbps() << " kbps for compressed formats";

        status << "\nMic Gain: " << juce::String(audioRecorderMicGainDb, 1) << " dB";

        if (audioClipRecorder)
        {
            status << "\nDevice: " << audioClipRecorder->getCurrentDeviceSummary();
            if (audioRecorderTestActive && audioClipRecorder->isRecording())
                status << "\nStatus: Record Test capturing 5 seconds. Playback and cleanup are automatic."
                       << " | Captured: " << formatSecondsFromSamples(audioClipRecorder->getSamplesWritten(), audioClipRecorder->getSampleRate());
            else if (audioRecorderTestPlaybackActive)
                status << "\nStatus: Record Test playback. Temp file will be deleted automatically.";
            else if (audioClipRecorder->isRecording())
                status << "\nStatus: " << (audioClipRecorder->isPaused() ? "Paused" : "Recording")
                       << " | Captured: " << formatSecondsFromSamples(audioClipRecorder->getSamplesWritten(), audioClipRecorder->getSampleRate());
            else if (audioRecordingTakeStopped && audioRecordingTakeDirty)
                status << "\nStatus: Take stopped. Use Save / Apply, Redo From Top, or Discard Take.";
            else
                status << "\nStatus: Ready.";
        }

        content->setMicGainDb(audioRecorderMicGainDb);
        content->setTestModeActive(audioRecorderTestActive || audioRecorderTestPlaybackActive);
        content->setStatusText(status);
        content->setDurationText(audioClipRecorder ? formatSecondsFromSamples(audioClipRecorder->getSamplesWritten(), audioClipRecorder->getSampleRate()) : juce::String("0.00s"));
    }

    void MainComponent::startAudioRecordingTake()
    {
        if (audioRecorderTestActive || audioRecorderTestPlaybackActive)
        {
            logMessage("AudioClip Recorder: wait for Record Test playback and cleanup to finish before starting a take.");
            refreshAudioRecorderWindowStatus();
            return;
        }

        if (audioClipRecorder && audioClipRecorder->isRecording())
        {
            if (audioClipRecorder->isPaused())
            {
                audioClipRecorder->resume();
                logMessage("AudioClip recording resumed; appending to the same take.");
                refreshAudioRecorderWindowStatus();
                return;
            }

            logMessage("AudioClip Recorder: recording is already active. Use Pause or Stop before starting a new take.");
            refreshAudioRecorderWindowStatus();
            return;
        }

        if (!ensureProjectFolderReadyForAudio())
            return;

        int trackIndex = activeRecordingTrackIndex;
        if (!audioRecordingTakeDirty || trackIndex < 0)
            trackIndex = createAudioClipTrackForNewClip("Recorded AudioClip");
        if (trackIndex < 0)
            return;

        if (activeImportSectionIndex < 0 || activeImportSectionIndex >= static_cast<int>(importSections.size()))
        {
            recordImportSection(
                "Recorded Audio",
                std::filesystem::path{},
                0,
                mw::core::Project::ticksPerQuarterNote * 4,
                { trackIndex + 1 },
                false,
                "audio-record"
            );
        }
        else
        {
            auto& section = importSections[static_cast<std::size_t>(activeImportSectionIndex)];
            if (std::find(section.trackNumbers.begin(), section.trackNumbers.end(), trackIndex + 1) == section.trackNumbers.end())
                section.trackNumbers.push_back(trackIndex + 1);
            std::sort(section.trackNumbers.begin(), section.trackNumbers.end());
        }

        const auto projectFolder = getCurrentProjectFolder();
        const auto tempFolder = mw::audio::AudioClipImporter::tempAudioFolderFor(projectFolder);
        std::string spaceMessage;
        const auto minFreeBytes = static_cast<std::uintmax_t>(512u) * 1024u * 1024u;
        if (!mw::audio::AudioClipImporter::hasEnoughFreeSpace(tempFolder, minFreeBytes, spaceMessage))
        {
            logMessage(juce::String("ERROR: ") + spaceMessage);
            return;
        }

        if (audioClipRecorder && audioClipRecorder->isRecording())
            audioClipRecorder->stop();

        std::error_code ec;
        std::filesystem::remove(activeRecordingTempWavPath, ec);
        std::filesystem::create_directories(tempFolder, ec);
        activeRecordingTempWavPath = mw::audio::AudioClipImporter::makeUniqueMediaPath(tempFolder, "recording_take", mw::core::AudioClipSavedFormat::Wav);
        activeRecordingSourceWavPath.clear();
        if (importSections.empty())
        {
            logMessage("ERROR: AudioClip Recorder could not find an active sequence.");
            audioRecordingTakeDirty = false;
            return;
        }

        activeRecordingTrackIndex = trackIndex;
        activeRecordingSequenceNumber = std::clamp(activeImportSectionIndex + 1, 1, static_cast<int>(importSections.size()));
        activeRecordingStartTick = importSections[static_cast<std::size_t>(activeRecordingSequenceNumber - 1)].startTick;
        audioRecordingTakeStopped = false;
        audioRecordingTakeDirty = true;

        if (!audioClipRecorder)
            audioClipRecorder = std::make_unique<mw::audio::AudioClipRecorder>();

        const auto startResult = audioClipRecorder->startRecording(activeRecordingTempWavPath, channelsCombo.getSelectedId() == 1 ? 1 : 2, 24);
        if (!startResult.success)
        {
            logMessage(juce::String("ERROR: AudioClip recording did not start: ") + startResult.message);
            audioRecordingTakeDirty = false;
            std::filesystem::remove(activeRecordingTempWavPath, ec);
            activeRecordingTempWavPath.clear();
            return;
        }

        activeRecordingSampleRate = startResult.sampleRate;
        activeRecordingChannelCount = startResult.channelCount;
        logMessage("Recording AudioClip take to temp file. Pause skips time; no silent gap is inserted.");
        refreshAudioRecorderWindowStatus();
    }

    void MainComponent::pauseOrResumeAudioRecordingTake()
    {
        if (audioRecorderTestActive || audioRecorderTestPlaybackActive)
        {
            logMessage("AudioClip Recorder: Record Test is automatic and has no pause/resume controls.");
            return;
        }

        if (!audioClipRecorder || !audioClipRecorder->isRecording())
        {
            logMessage("AudioClip Recorder: no active recording to pause/resume.");
            return;
        }

        if (audioClipRecorder->isPaused())
        {
            audioClipRecorder->resume();
            logMessage("AudioClip recording resumed.");
        }
        else
        {
            audioClipRecorder->pause();
            logMessage("AudioClip recording paused. Paused time will not create silence.");
        }

        refreshAudioRecorderWindowStatus();
    }

    void MainComponent::stopAudioRecordingTake()
    {
        if (audioRecorderTestActive || audioRecorderTestPlaybackActive)
        {
            logMessage("AudioClip Recorder: Record Test stops, plays back, and deletes automatically.");
            return;
        }

        if (!audioClipRecorder || !audioClipRecorder->isRecording())
        {
            logMessage("AudioClip Recorder: no active recording to stop.");
            return;
        }

        audioClipRecorder->stop();
        activeRecordingSourceWavPath = activeRecordingTempWavPath;
        audioRecordingTakeStopped = true;
        audioRecordingTakeDirty = true;
        logMessage("AudioClip recording stopped. Choose Save / Apply, Redo From Top, or Discard Take.");
        refreshAudioRecorderWindowStatus();
    }

    void MainComponent::keepAudioRecordingTake()
    {
        if (audioRecorderTestActive || audioRecorderTestPlaybackActive)
        {
            logMessage("AudioClip Recorder: Record Test is temporary and cannot be saved/applied.");
            return;
        }

        if (!currentProject || !audioRecordingTakeDirty)
        {
            logMessage("AudioClip Recorder: no take to save/apply.");
            return;
        }

        if (audioClipRecorder && audioClipRecorder->isRecording())
            stopAudioRecordingTake();

        if (activeRecordingSourceWavPath.empty())
        {
            logMessage("AudioClip Recorder: no stopped take to save/apply.");
            return;
        }

        mw::audio::AudioClipImportRequest request;
        request.sourcePath = activeRecordingSourceWavPath;
        request.projectFolder = getCurrentProjectFolder();
        request.ffmpegExePath = std::filesystem::path(ffmpegPathBox.getText().toStdString());
        request.savedFormat = getSelectedAudioClipFormat();
        request.qualityKbps = getSelectedAudioClipQualityKbps();
        request.channelCount = activeRecordingChannelCount;
        request.imported = false;
        request.preferredName = "recorded_take";

        const auto result = mw::audio::AudioClipImporter::importToProject(request);
        if (!result.success)
        {
            logMessage(juce::String("ERROR: ") + result.message);
            return;
        }

        mw::core::AudioClip clip;
        clip.name = safeClipDisplayName(result.absolutePath).toStdString();
        clip.trackIndex = activeRecordingTrackIndex;
        clip.sequenceNumber = activeRecordingSequenceNumber;
        clip.sourceType = mw::core::AudioClipSourceType::Recorded;
        clip.savedFormat = getSelectedAudioClipFormat();
        clip.projectRelativePath = result.relativePath;
        clip.originalSourcePath = activeRecordingSourceWavPath;
        clip.startTick = activeRecordingStartTick;
        clip.durationSamples = audioClipRecorder ? audioClipRecorder->getSamplesWritten() : 0;
        clip.sampleRate = activeRecordingSampleRate;
        clip.channelCount = activeRecordingChannelCount;
        clip.bitDepth = 24;
        clip.sizeBytes = result.sizeBytes;

        addAudioClipToProject(std::move(clip));

        std::error_code ec;
        std::filesystem::remove(activeRecordingSourceWavPath, ec);
        activeRecordingTempWavPath.clear();
        activeRecordingSourceWavPath.clear();
        audioRecordingTakeStopped = false;
        audioRecordingTakeDirty = false;
        logMessage(juce::String("Saved/applied recorded AudioClip: ") + result.relativePath.string());
        refreshAudioRecorderWindowStatus();
    }

    void MainComponent::redoAudioRecordingTake()
    {
        if (audioRecorderTestActive || audioRecorderTestPlaybackActive)
        {
            logMessage("AudioClip Recorder: Record Test cleans itself up automatically; no redo is needed.");
            return;
        }

        if (audioClipRecorder && audioClipRecorder->isRecording())
            audioClipRecorder->stop();

        std::error_code ec;
        if (!activeRecordingTempWavPath.empty())
            std::filesystem::remove(activeRecordingTempWavPath, ec);
        if (!activeRecordingSourceWavPath.empty())
            std::filesystem::remove(activeRecordingSourceWavPath, ec);

        audioRecordingTakeStopped = false;
        audioRecordingTakeDirty = false;
        logMessage("Redo From Top: discarded temp take and restarting from the original sequence/start position.");
        startAudioRecordingTake();
    }

    void MainComponent::discardAudioRecordingTake()
    {
        if (audioRecorderTestActive || audioRecorderTestPlaybackActive)
        {
            logMessage("AudioClip Recorder: Record Test will delete itself automatically.");
            return;
        }

        if (audioClipRecorder && audioClipRecorder->isRecording())
            audioClipRecorder->stop();

        std::error_code ec;
        if (!activeRecordingTempWavPath.empty())
            std::filesystem::remove(activeRecordingTempWavPath, ec);
        if (!activeRecordingSourceWavPath.empty())
            std::filesystem::remove(activeRecordingSourceWavPath, ec);

        activeRecordingTempWavPath.clear();
        activeRecordingSourceWavPath.clear();
        audioRecordingTakeStopped = false;
        audioRecordingTakeDirty = false;
        logMessage("Discarded AudioClip temp take and cleaned up its temp file.");
        refreshAudioRecorderWindowStatus();
    }

    void MainComponent::closeAudioRecorderWindowWithPrompt()
    {
        const bool hasDirtyTake = audioRecordingTakeDirty || (audioClipRecorder && audioClipRecorder->isRecording() && !audioRecorderTestActive);
        if (hasDirtyTake)
        {
            auto* alert = new juce::AlertWindow(
                "Unsaved AudioClip Take",
                "There is an active or unsaved AudioClip take. Save it, discard it, or cancel closing?",
                juce::AlertWindow::WarningIcon
            );

            alert->addButton("Save Take and Close", 2);
            alert->addButton("Discard and Close", 1);
            alert->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
            alert->enterModalState(
                true,
                juce::ModalCallbackFunction::create(
                    [this, alert](int result)
                    {
                        std::unique_ptr<juce::AlertWindow> cleanup(alert);
                        if (result == 0)
                            return;

                        if (result == 2)
                        {
                            if (audioClipRecorder && audioClipRecorder->isRecording())
                                stopAudioRecordingTake();

                            const bool wasDirty = audioRecordingTakeDirty;
                            keepAudioRecordingTake();
                            if (wasDirty && audioRecordingTakeDirty)
                                return; // save failed; keep the recorder open so the user can recover/discard
                        }
                        else if (result == 1)
                        {
                            discardAudioRecordingTake();
                        }

                        closeAudioRecorderWindowNow();
                    }
                )
            );
            return;
        }

        closeAudioRecorderWindowNow();
    }

    void MainComponent::closeAudioRecorderWindowNow()
    {
        juce::MessageManager::callAsync(
            [this]
            {
                cancelAudioRecorderTestAndCleanup();

                if (audioClipRecorder && audioClipRecorder->isRecording())
                    audioClipRecorder->stop();

                if (audioRecorderWindow != nullptr)
                {
                    audioRecorderWindow->setVisible(false);
                    audioRecorderWindow.reset();
                    audioRecorderContent.reset();
                }

                audioClipRecorder.reset();
                logMessage("Closed AudioClip Recorder.");
            }
        );
    }

    void MainComponent::revealCurrentProjectFolder()
    {
        juce::File(getCurrentProjectFolder().string()).revealToUser();
    }


    void MainComponent::updateRenderOutputSummary()
    {
        const int formatId = outputFormatCombo.getSelectedId() > 0 ? outputFormatCombo.getSelectedId() : 1;
        const int sampleRate = sampleRateCombo.getSelectedId() > 0 ? sampleRateCombo.getSelectedId() : 48000;
        const int bitrate = bitrateCombo.getSelectedId() > 0 ? bitrateCombo.getSelectedId() : 192;
        const int channels = channelsCombo.getSelectedId() > 0 ? channelsCombo.getSelectedId() : 2;
        const auto channelText = channels == 1 ? juce::String("Mono") : juce::String("Stereo");
        const auto workersText = renderWorkersCombo.getSelectedId() == 100 || renderWorkersCombo.getSelectedId() <= 0
            ? juce::String("Auto parallel stems")
            : juce::String(renderWorkersCombo.getSelectedId()) + " parallel stem" + (renderWorkersCombo.getSelectedId() == 1 ? "" : "s");
        const double sampleRateKhz = static_cast<double>(sampleRate) / 1000.0;

        juce::String formatText = "WAV";
        juce::String bitrateText;

        switch (formatId)
        {
            case 2:
                formatText = "FLAC";
                bitrateText = "lossless variable bitrate";
                break;
            case 3:
                formatText = "MP3";
                bitrateText = juce::String(bitrate) + " kbps";
                break;
            case 4:
                formatText = "OGG";
                bitrateText = juce::String(bitrate) + " kbps";
                break;
            case 1:
            default:
            {
                formatText = "WAV";
                const int bitDepth = 24;
                const double estimatedKbps = (static_cast<double>(sampleRate) * bitDepth * channels) / 1000.0;
                bitrateText = "~" + juce::String(estimatedKbps, 0) + " kbps";
                break;
            }
        }

        renderOutputSummaryLabel.setText(
            "Render Output: " + formatText
                + " | " + juce::String(sampleRateKhz, sampleRate % 1000 == 0 ? 0 : 1) + " kHz"
                + " | " + bitrateText
                + " | " + channelText
                + " | " + workersText,
            juce::dontSendNotification
        );
    }

    void MainComponent::renderMidiCurrentProject()
    {
        if (!currentProject)
            importMusicXmlOnly();

        if (!currentProject)
            return;

        applyTrackInspector();
        captureProjectUserSettings();

        auto job = createRenderJobSnapshot();

        mw::exporting::ExportSettings exportSettings;
        exportSettings.outputFolder = job.exportFolder;
        exportSettings.baseFileName = job.baseFileName;

        if (!mw::exporting::ExportPathBuilder::ensureOutputFolderExists(exportSettings))
        {
            logMessage("Render MIDI: failed to access export folder.");
            return;
        }

        const auto midiPath = mw::exporting::ExportPathBuilder::buildMidiPath(exportSettings);

        if (mw::midi::MidiExporter::exportToFile(job.project, midiPath))
        {
            logMessage("Rendered MIDI: " + midiPath.string());
            renderStatusLabel.setText("MIDI rendered.", juce::dontSendNotification);
            renderStatusLabel.setColour(juce::Label::textColourId, juce::Colours::lightgreen);
        }
        else
        {
            logMessage("ERROR: Render MIDI failed: " + midiPath.string());
            renderStatusLabel.setText("MIDI render failed.", juce::dontSendNotification);
            renderStatusLabel.setColour(juce::Label::textColourId, juce::Colours::red);
        }
    }

    void MainComponent::setRenderingState(bool isRendering)
    {
        renderingInProgress = isRendering;
        renderButton.setEnabled(!isRendering);
        renderSettingsButton.setEnabled(!isRendering);
        renderSelectedTrackButton.setEnabled(!isRendering);
        renderSelectedSequenceButton.setEnabled(!isRendering);
        renderMidiButton.setEnabled(!isRendering);
        chooseMusicXmlButton.setEnabled(!isRendering);
        openProjectButton.setEnabled(!isRendering);
        saveProjectButton.setEnabled(!isRendering);
        cancelRenderButton.setEnabled(isRendering);

        if (auto* content = dynamic_cast<TrackManagerWindowContent*>(trackManagerContent.get()))
            content->setPreviewButtonsEnabled(!isRendering);

        if (isRendering)
        {
            renderButton.setButtonText("Rendering...");
            renderSelectedTrackButton.setButtonText("Rendering...");
            renderSelectedSequenceButton.setButtonText("Rendering...");
            renderMidiButton.setButtonText("Rendering...");
            renderStatusLabel.setText("Rendering ...", juce::dontSendNotification);
            renderStatusLabel.setColour(juce::Label::textColourId, juce::Colours::orange);
        }
        else
        {
            renderButton.setButtonText("Render Project");
            renderSelectedTrackButton.setButtonText("Render Track");
            renderSelectedSequenceButton.setButtonText("Render Seq");
            renderMidiButton.setButtonText("Render MIDI");
            renderStatusLabel.setText("Ready", juce::dontSendNotification);
            renderStatusLabel.setColour(juce::Label::textColourId, juce::Colours::lightgreen);
        }

        repaint();
    }

    void MainComponent::renderCurrentProjectOnBackgroundThread()
    {
        if (renderingInProgress)
        {
            logMessage("Render already in progress.");
            return;
        }

        if (!currentProject)
            importMusicXmlOnly();

        if (!currentProject)
            return;

        applyTrackInspector();

        if (appliedProjectBackendId == 2 && !mw::app::UserPreferencesStore::load().suppressSfzRenderWarning)
        {
            showSfzRenderWarning();
            return;
        }

        continueRenderAfterSfzWarning();
    }

    void MainComponent::startRenderJobOnBackgroundThread(mw::audio::RenderJob job, const juce::String& label, bool playWhenDone, double previewDurationBeats)
    {
        if (renderingInProgress)
        {
            logMessage("Render already in progress.");
            return;
        }

        if (playWhenDone)
            job.keepStemFilesMask = 0;

        if (projectContainsVst3Track(job.project))
            showVstExperimentalWarningIfNeeded();

        cancelRenderRequested = false;
        setRenderingState(true);
        logMessage("RenderJob started in background: " + label);

        if (renderThread.joinable())
            renderThread.join();

        renderThread = std::thread(
            [this, job, label, playWhenDone, previewDurationBeats]
            {
                mw::audio::RenderJobCallbacks callbacks;

                callbacks.log = [this](const std::string& message)
                {
                    logMessage(message);
                };

                callbacks.status = [this](const std::string& message)
                {
                    juce::MessageManager::callAsync(
                        [this, message]
                        {
                            renderStatusLabel.setText(message, juce::dontSendNotification);
                        }
                    );
                };

                const auto result = mw::audio::RenderJobRunner::run(job, cancelRenderRequested, callbacks);

                juce::MessageManager::callAsync(
                    [this, result, label, playWhenDone, previewDurationBeats]
                    {
                        if (result.cancelled)
                        {
                            logMessage("Render cancelled: " + label);
                        }
                        else if (result.success)
                        {
                            logMessage("Render completed successfully (" + label + "): " + result.finalAudioPath.string());

                            if (playWhenDone)
                            {
                                if (result.finalAudioPath.extension() == ".wav")
                                {
                                    generatedPreviewFiles.clear();

                                    if (!result.projectPath.empty())
                                        generatedPreviewFiles.push_back(result.projectPath);

                                    if (!result.midiPath.empty())
                                        generatedPreviewFiles.push_back(result.midiPath);

                                    if (!result.wavPath.empty())
                                        generatedPreviewFiles.push_back(result.wavPath);

                                    if (!result.finalAudioPath.empty() && result.finalAudioPath != result.wavPath)
                                        generatedPreviewFiles.push_back(result.finalAudioPath);

                                    lastPianoRollPreviewWavPath = result.finalAudioPath;
                                    lastPianoRollPreviewDurationBeats = std::max(1.0, previewDurationBeats > 0.0 ? previewDurationBeats : static_cast<double>(getSelectedTrackEndBeat()));
                                    lastPianoRollPreviewTempoBpm = pianoRollBpmBox.getText().getDoubleValue() > 0.0
                                        ? pianoRollBpmBox.getText().getDoubleValue()
                                        : 120.0;
                                    pianoRollPreviewPaused = false;
                                    pendingPianoRollPreviewStartSeconds = 0.0;

                                    openPianoRollPreviewPlayerWindow();
                                    playPianoRollPreviewFile(lastPianoRollPreviewWavPath);

                                    logMessage("Opened Preview Player for: " + result.finalAudioPath.string());
                                }
                                else
                                {
                                    logMessage("Preview player currently supports WAV files. Created: " + result.finalAudioPath.string());
                                }
                            }
                        }
                        else
                        {
                            logMessage("Render failed (" + label + "): " + result.message);
                        }

                        setRenderingState(false);
                    }
                );
            }
        );
    }

    void MainComponent::closeRenderSettingsWindow()
    {
        juce::MessageManager::callAsync(
            [this]
            {
                renderSettingsWindow.reset();
                renderSettingsContent.reset();
            }
        );
    }

    void MainComponent::showRenderSettingsWindow()
    {
        if (renderSettingsWindow != nullptr)
        {
            renderSettingsWindow->toFront(true);
            return;
        }

        auto closeCallback = [this]
        {
            closeRenderSettingsWindow();
        };

        auto* window = new SequencePickerDocumentWindow("Render Settings", closeCallback);
        const auto preferences = mw::app::UserPreferencesStore::load();
        const int currentWorkerId = renderWorkersCombo.getSelectedId();
        const int currentWorkerCount = currentWorkerId == 100 || currentWorkerId <= 0 ? 0 : currentWorkerId;

        auto* content = new RenderSettingsContent(
            preferences.keepStemFilesMask,
            currentWorkerCount,
            [this](int selectedMask, int workerCount)
            {
                auto preferencesToSave = mw::app::UserPreferencesStore::load();
                preferencesToSave.keepStemFilesMask = std::clamp(selectedMask, 0, 3);
                preferencesToSave.lastRenderWorkerCount = workerCount == 0 ? 0 : std::clamp(workerCount, 1, 16);

                renderWorkersCombo.setSelectedId(preferencesToSave.lastRenderWorkerCount > 0 ? preferencesToSave.lastRenderWorkerCount : 100, juce::dontSendNotification);
                updateRenderOutputSummary();

                if (mw::app::UserPreferencesStore::saveValues({
                        { "keepStemFiles", preferencesToSave.keepStemFilesMask == 0 ? std::string("0") : (preferencesToSave.keepStemFilesMask == 1 ? std::string("1") : (preferencesToSave.keepStemFilesMask == 2 ? std::string("2") : std::string("1,2"))) },
                        { "lastRenderWorkerCount", std::to_string(preferencesToSave.lastRenderWorkerCount) }
                    }))
                {
                    juce::String message;
                    message << "Render Settings saved: keep stem files = ";
                    switch (preferencesToSave.keepStemFilesMask)
                    {
                        case 0: message << "0 (none)"; break;
                        case 1: message << "1 (WAV only)"; break;
                        case 2: message << "2 (MIDI only)"; break;
                        case 3:
                        default: message << "1,2 (WAV + MIDI)"; break;
                    }

                    message << "; parallel stems = ";
                    if (preferencesToSave.lastRenderWorkerCount <= 0)
                        message << "Auto (safe).";
                    else
                        message << preferencesToSave.lastRenderWorkerCount << ".";

                    logMessage(message);
                }
                else
                {
                    logMessage("ERROR: Failed to save render settings.");
                }

                closeRenderSettingsWindow();
            },
            [this]
            {
                logMessage("Render Settings cancelled.");
                closeRenderSettingsWindow();
            }
        );

        renderSettingsContent.reset(content);
        window->setContentNonOwned(renderSettingsContent.get(), true);
        window->centreWithSize(760, 360);
        window->setVisible(true);
        window->toFront(true);
        renderSettingsWindow.reset(window);
    }

    void MainComponent::renderSelectedTrackOnBackgroundThread()
    {
        if (renderingInProgress)
        {
            logMessage("Render already in progress.");
            return;
        }

        if (!currentProject)
        {
            logMessage("Render Track: no project loaded.");
            return;
        }

        applyTrackInspector();
        updateRenderTargetLabel();

        const auto index = getSelectedTrackIndex();

        if (index < 0 || index >= static_cast<int>(currentProject->getTracks().size()))
        {
            logMessage("Render Track: no valid track selected.");
            return;
        }

        auto job = createRenderJobSnapshot();
        const auto& sourceTrack = currentProject->getTracks()[static_cast<std::size_t>(index)];

        job.project.setName(currentProject->getName() + " - Track " + std::to_string(index + 1));
        job.project.getTracks().clear();
        job.project.getTracks().push_back(sourceTrack);
        std::vector<mw::core::AudioClip> previewTrackClips;
        for (const auto& clip : currentProject->getAudioClips())
        {
            if (clip.trackIndex == index)
            {
                auto clipCopy = clip;
                clipCopy.trackIndex = 0;
                previewTrackClips.push_back(std::move(clipCopy));
            }
        }
        job.project.setAudioClips(std::move(previewTrackClips));
        job.project.setSequences({});

        job.baseFileName += "_track_" + std::to_string(index + 1);

        if (job.backend == mw::audio::RenderBackend::SFZ
            && sourceTrack.getInstrument().backendType == mw::core::SampleBackendType::SFZ
            && !sourceTrack.getInstrument().sampleLibraryPath.empty())
        {
            job.sfzPath = sourceTrack.getInstrument().sampleLibraryPath;
        }

        juce::String label;
        label << "selected track #"
              << (index + 1)
              << " - "
              << sourceTrack.getName();

        startRenderJobOnBackgroundThread(std::move(job), label);
    }

    void MainComponent::renderSelectedSequenceOnBackgroundThread()
    {
        if (renderingInProgress)
        {
            logMessage("Render already in progress.");
            return;
        }

        if (!currentProject)
        {
            logMessage("Render Sequence: no project loaded.");
            return;
        }

        rebuildSectionsFromTracksIfNeeded();
        updateRenderTargetLabel();

        auto sequenceNumber = trackManagerSectionBox.getText().getIntValue();

        if (sequenceNumber <= 0)
            sequenceNumber = activeImportSectionIndex + 1;

        if (sequenceNumber <= 0 || sequenceNumber > static_cast<int>(importSections.size()))
        {
            logMessage("Render Sequence: no valid sequence selected.");
            return;
        }

        applyTrackInspector();

        const auto& sequence = importSections[static_cast<std::size_t>(sequenceNumber - 1)];
        auto job = createRenderJobSnapshot();

        std::vector<mw::core::Track> selectedTracks;
        std::vector<std::pair<int, int>> trackIndexMap;

        for (const auto trackNumber : sequence.trackNumbers)
        {
            const auto trackIndex = trackNumber - 1;

            if (trackIndex >= 0 && trackIndex < static_cast<int>(currentProject->getTracks().size()))
            {
                trackIndexMap.emplace_back(trackIndex, static_cast<int>(selectedTracks.size()));
                selectedTracks.push_back(currentProject->getTracks()[static_cast<std::size_t>(trackIndex)]);
            }
        }

        if (selectedTracks.empty())
        {
            logMessage("Render Sequence: selected sequence has no valid tracks.");
            return;
        }

        const auto trimResult = trimTracksToTickRangeAndRebase(selectedTracks, sequence.startTick, sequence.endTick);
        if (trimResult.applied)
        {
            logMessage("Render Sequence: trimmed/rebased notes to the selected sequence range ("
                + juce::String(trimResult.keptNotes)
                + " of "
                + juce::String(trimResult.totalNotes)
                + " notes kept).");
        }

        job.project.setName(currentProject->getName() + " - Sequence " + std::to_string(sequenceNumber));
        job.project.getTracks() = std::move(selectedTracks);
        std::vector<mw::core::AudioClip> sequenceAudioClips;
        for (const auto& clip : currentProject->getAudioClips())
        {
            if (clip.sequenceNumber != sequenceNumber)
                continue;

            for (const auto& mapped : trackIndexMap)
            {
                if (clip.trackIndex == mapped.first)
                {
                    auto clipCopy = clip;
                    clipCopy.trackIndex = mapped.second;
                    clipCopy.sequenceNumber = 1;
                    clipCopy.startTick = std::max<std::int64_t>(0, clip.startTick - sequence.startTick);
                    sequenceAudioClips.push_back(std::move(clipCopy));
                    break;
                }
            }
        }
        job.project.setAudioClips(std::move(sequenceAudioClips));

        mw::core::ProjectSequenceMetadata metadata;
        metadata.id = sequence.id > 0 ? sequence.id : sequenceNumber;
        metadata.number = 1;
        metadata.type = "sequence";
        metadata.name = sequence.name.toStdString();
        metadata.sourceFile = sequence.sourcePath;
        metadata.createdBy = sequence.createdBy.toStdString();
        metadata.notes = sequence.notes.toStdString();
        metadata.locked = sequence.locked;
        metadata.startTick = sequence.startTick;
        metadata.endTick = sequence.endTick;

        for (int i = 0; i < static_cast<int>(job.project.getTracks().size()); ++i)
            metadata.tracks.push_back(i + 1);

        metadata.color = sequenceColourToHex(getSequenceColourForIndex(sequenceNumber - 1)).toStdString();
        job.project.setSequences({ metadata });

        job.baseFileName += "_seq_" + std::to_string(sequenceNumber);

        juce::String label;
        label << "selected sequence #"
              << sequenceNumber
              << " - "
              << sequence.name;

        startRenderJobOnBackgroundThread(std::move(job), label);
    }

    void MainComponent::previewSelectedTrackOnBackgroundThread()
    {
        if (renderingInProgress)
        {
            logMessage("Preview Track: render/preview already in progress.");
            return;
        }

        if (!currentProject)
        {
            logMessage("Preview Track: no project loaded.");
            return;
        }

        const auto index = getSelectedTrackIndex();

        if (index < 0 || index >= static_cast<int>(currentProject->getTracks().size()))
        {
            logMessage("Preview Track: no valid track selected.");
            return;
        }

        applyTrackInspector();
        updateRenderTargetLabel();
        cleanupPianoRollPreviewFiles();
        lastPianoRollPreviewScope = 1;

        auto job = createRenderJobSnapshot();
        const auto& sourceTrack = currentProject->getTracks()[static_cast<std::size_t>(index)];

        job.project.setName(currentProject->getName() + " - Track Preview " + std::to_string(index + 1));
        job.project.getTracks().clear();
        job.project.getTracks().push_back(sourceTrack);
        std::vector<mw::core::AudioClip> previewTrackClips;
        for (const auto& clip : currentProject->getAudioClips())
        {
            if (clip.trackIndex == index)
            {
                auto clipCopy = clip;
                clipCopy.trackIndex = 0;
                previewTrackClips.push_back(std::move(clipCopy));
            }
        }
        job.project.setAudioClips(std::move(previewTrackClips));
        job.project.setSequences({});

        if (!job.project.getTracks().empty())
        {
            const auto rebasedLeadingTicks = rebaseSingleTrackPreviewToFirstContent(
                job.project.getTracks().front(),
                job.project.getAudioClips());

            if (rebasedLeadingTicks > 0)
            {
                const auto rebasedLeadingBeats = static_cast<double>(rebasedLeadingTicks)
                    / static_cast<double>(mw::core::Project::ticksPerQuarterNote);
                logMessage("Preview Track: removed " + juce::String(rebasedLeadingBeats, 2)
                    + " beat(s) of leading timeline silence for single-track playback.");
            }
        }

        setPianoRollPreviewNoteMapFromTracks(job.project.getTracks());
        std::filesystem::create_directories(mw::app::AppPaths::previewFolder());
        job.exportFolder = mw::app::AppPaths::previewFolder();
        job.baseFileName = "preview_track_" + std::to_string(index + 1);
        job.outputFormat = mw::audio::RenderOutputFormat::Wav;

        if (job.backend == mw::audio::RenderBackend::SFZ
            && sourceTrack.getInstrument().backendType == mw::core::SampleBackendType::SFZ
            && !sourceTrack.getInstrument().sampleLibraryPath.empty())
        {
            job.sfzPath = sourceTrack.getInstrument().sampleLibraryPath;
        }

        juce::String label;
        label << "preview track #"
              << (index + 1)
              << " - "
              << sourceTrack.getName();

        const auto previewDurationBeats = std::max(
            job.project.getTracks().empty() ? 1.0 : trackEndBeatForPreview(job.project.getTracks().front()),
            audioClipsEndBeatForPreview(job.project.getAudioClips(), static_cast<double>(currentProject->getTempoBpm())));
        startRenderJobOnBackgroundThread(std::move(job), label, true, previewDurationBeats);
    }

    void MainComponent::previewSelectedSequenceOnBackgroundThread()
    {
        if (renderingInProgress)
        {
            logMessage("Preview Sequence: render/preview already in progress.");
            return;
        }

        if (!currentProject)
        {
            logMessage("Preview Sequence: no project loaded.");
            return;
        }

        rebuildSectionsFromTracksIfNeeded();

        auto sequenceNumber = trackManagerSectionBox.getText().getIntValue();

        if (sequenceNumber <= 0)
            sequenceNumber = activeImportSectionIndex + 1;

        if (sequenceNumber <= 0 || sequenceNumber > static_cast<int>(importSections.size()))
        {
            logMessage("Preview Sequence: no valid sequence selected.");
            return;
        }

        applyTrackInspector();
        updateRenderTargetLabel();
        cleanupPianoRollPreviewFiles();
        lastPianoRollPreviewScope = 2;

        const auto& sequence = importSections[static_cast<std::size_t>(sequenceNumber - 1)];
        auto job = createRenderJobSnapshot();

        std::vector<mw::core::Track> selectedTracks;
        std::vector<std::pair<int, int>> trackIndexMap;

        for (const auto trackNumber : sequence.trackNumbers)
        {
            const auto trackIndex = trackNumber - 1;

            if (trackIndex >= 0 && trackIndex < static_cast<int>(currentProject->getTracks().size()))
            {
                trackIndexMap.emplace_back(trackIndex, static_cast<int>(selectedTracks.size()));
                selectedTracks.push_back(currentProject->getTracks()[static_cast<std::size_t>(trackIndex)]);
            }
        }

        if (selectedTracks.empty())
        {
            logMessage("Preview Sequence: selected sequence has no valid tracks.");
            return;
        }

        const auto trimResult = trimTracksToTickRangeAndRebase(selectedTracks, sequence.startTick, sequence.endTick);
        if (trimResult.applied)
        {
            logMessage("Preview Sequence: trimmed/rebased notes to the selected sequence range ("
                + juce::String(trimResult.keptNotes)
                + " of "
                + juce::String(trimResult.totalNotes)
                + " notes kept).");
        }

        job.project.setName(currentProject->getName() + " - Sequence Preview " + std::to_string(sequenceNumber));
        job.project.getTracks() = std::move(selectedTracks);
        std::vector<mw::core::AudioClip> sequenceAudioClips;
        for (const auto& clip : currentProject->getAudioClips())
        {
            if (clip.sequenceNumber != sequenceNumber)
                continue;

            for (const auto& mapped : trackIndexMap)
            {
                if (clip.trackIndex == mapped.first)
                {
                    auto clipCopy = clip;
                    clipCopy.trackIndex = mapped.second;
                    clipCopy.sequenceNumber = 1;
                    clipCopy.startTick = std::max<std::int64_t>(0, clip.startTick - sequence.startTick);
                    sequenceAudioClips.push_back(std::move(clipCopy));
                    break;
                }
            }
        }
        job.project.setAudioClips(std::move(sequenceAudioClips));
        job.project.setSequences({});
        setPianoRollPreviewNoteMapFromTracks(job.project.getTracks());
        std::filesystem::create_directories(mw::app::AppPaths::previewFolder());
        job.exportFolder = mw::app::AppPaths::previewFolder();
        job.baseFileName = "preview_seq_" + std::to_string(sequenceNumber);
        job.outputFormat = mw::audio::RenderOutputFormat::Wav;

        juce::String label;
        label << "preview sequence #"
              << sequenceNumber
              << " - "
              << sequence.name;

        const auto previewDurationBeats = std::max(
            tracksEndBeatForPreview(job.project.getTracks()),
            audioClipsEndBeatForPreview(job.project.getAudioClips(), static_cast<double>(currentProject->getTempoBpm())));
        startRenderJobOnBackgroundThread(std::move(job), label, true, previewDurationBeats);
    }

    void MainComponent::previewCurrentProjectOnBackgroundThread()
    {
        if (renderingInProgress)
        {
            logMessage("Preview Project: render/preview already in progress.");
            return;
        }

        if (!currentProject)
        {
            logMessage("Preview Project: no project loaded.");
            return;
        }

        if (currentProject->getTracks().empty())
        {
            logMessage("Preview Project: project has no tracks to preview.");
            return;
        }

        applyTrackInspector();
        updateRenderTargetLabel();
        cleanupPianoRollPreviewFiles();
        lastPianoRollPreviewScope = 3;

        auto job = createRenderJobSnapshot();
        setPianoRollPreviewNoteMapFromTracks(job.project.getTracks());
        std::filesystem::create_directories(mw::app::AppPaths::previewFolder());
        job.exportFolder = mw::app::AppPaths::previewFolder();
        job.baseFileName = "preview_project";
        job.outputFormat = mw::audio::RenderOutputFormat::Wav;

        const auto previewDurationBeats = std::max(
            tracksEndBeatForPreview(job.project.getTracks()),
            audioClipsEndBeatForPreview(job.project.getAudioClips(), static_cast<double>(currentProject->getTempoBpm())));
        startRenderJobOnBackgroundThread(std::move(job), "preview project", true, previewDurationBeats);
    }

    void MainComponent::showSfzRenderWarning()
    {
        auto* alert = new juce::AlertWindow(
            "SFZ render may take a moment",
            "SFZ libraries can load many individual sample files, so the window may look frozen while rendering. "
            "This is expected in the current stable build. The render should continue until the output file is created.",
            juce::AlertWindow::WarningIcon
        );

        alert->addButton("Continue Render", 1, juce::KeyPress(juce::KeyPress::returnKey));
        alert->addButton("Continue and Don't Show Again", 2);
        alert->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

        alert->enterModalState(
            true,
            juce::ModalCallbackFunction::create(
                [this, alert](int result)
                {
                    std::unique_ptr<juce::AlertWindow> cleanup(alert);

                    if (result == 2)
                    {
                        mw::app::UserPreferencesStore::saveBoolValue("suppressSfzRenderWarning", true);

                        continueRenderAfterSfzWarning();
                    }
                    else if (result == 1)
                    {
                        continueRenderAfterSfzWarning();
                    }
                    else
                    {
                        logMessage("SFZ render cancelled before starting.");
                    }
                }
            )
        );
    }

    void MainComponent::continueRenderAfterSfzWarning()
    {
        if (!currentProject)
            return;

        auto job = createRenderJobSnapshot();
        startRenderJobOnBackgroundThread(std::move(job), "full project");
    }

mw::audio::RenderJob MainComponent::createRenderJobSnapshot() const
    {
        mw::audio::RenderJob job;

        if (currentProject)
            job.project = *currentProject;

        job.sourceInputPath = std::filesystem::path(musicXmlPathBox.getText().toStdString());
        job.exportFolder = std::filesystem::path(exportFolderBox.getText().toStdString());
        job.projectFolder = getCurrentProjectFolder();
        job.baseFileName = baseNameBox.getText().isEmpty()
            ? job.project.getName()
            : baseNameBox.getText().toStdString();

        job.metadataTitle = metadataTitleBox.getText().trim().toStdString();
        job.metadataArtist = metadataArtistBox.getText().trim().toStdString();
        job.metadataAlbum = metadataAlbumBox.getText().trim().toStdString();
        job.metadataTrackNumber = metadataTrackNumberBox.getText().trim().toStdString();
        job.metadataYear = metadataYearBox.getText().trim().toStdString();

        if (!job.metadataTitle.empty())
            job.baseFileName = job.metadataTitle;

        // RenderJob uses SF2/SFZ as the fallback backend for unassigned tracks.
        // VST3-backed tracks render from their per-track plugin assignment.
        job.backend = appliedProjectBackendId == 2
            ? mw::audio::RenderBackend::SFZ
            : mw::audio::RenderBackend::SF2;

        switch (outputFormatCombo.getSelectedId())
        {
            case 2: job.outputFormat = mw::audio::RenderOutputFormat::Flac; break;
            case 3: job.outputFormat = mw::audio::RenderOutputFormat::Mp3; break;
            case 4: job.outputFormat = mw::audio::RenderOutputFormat::Ogg; break;
            case 1:
            default: job.outputFormat = mw::audio::RenderOutputFormat::Wav; break;
        }

        job.soundFontPath =
            getSelectedSoundFontPath().empty()
                ? std::filesystem::path(soundFontPathBox.getText().toStdString())
                : getSelectedSoundFontPath();

        job.fluidSynthPath = std::filesystem::path(fluidSynthPathBox.getText().toStdString());
        job.ffmpegPath = std::filesystem::path(ffmpegPathBox.getText().toStdString());
        job.sfzPath = getSelectedSfzPath().empty() ? std::filesystem::path(sfzPathBox.getText().toStdString()) : getSelectedSfzPath();
        job.sfizzRenderPath = std::filesystem::path(sfizzPathBox.getText().toStdString());

        const auto selectedTrackIndex = getSelectedTrackIndex();
        if (job.backend == mw::audio::RenderBackend::SFZ
            && currentProject
            && selectedTrackIndex >= 0
            && selectedTrackIndex < static_cast<int>(currentProject->getTracks().size()))
        {
            const auto& selectedTrack = currentProject->getTracks()[static_cast<std::size_t>(selectedTrackIndex)];

            if (selectedTrack.getInstrument().backendType == mw::core::SampleBackendType::SFZ
                && !selectedTrack.getInstrument().sampleLibraryPath.empty())
            {
                job.sfzPath = selectedTrack.getInstrument().sampleLibraryPath;
            }
        }

        job.sampleRate = sampleRateCombo.getSelectedId() > 0 ? sampleRateCombo.getSelectedId() : 48000;
        job.bitDepth = 24;
        job.bitrateKbps = bitrateCombo.getSelectedId() > 0 ? bitrateCombo.getSelectedId() : 192;
        job.channelCount = channelsCombo.getSelectedId() > 0 ? channelsCombo.getSelectedId() : 2;
        job.renderWorkerCount = renderWorkersCombo.getSelectedId() == 100 ? 0 : std::max(1, renderWorkersCombo.getSelectedId());
        job.keepStemFilesMask = std::clamp(mw::app::UserPreferencesStore::load().keepStemFilesMask, 0, 3);

        try { job.sfzKeySwitch = std::stoi(sfzKeySwitchBox.getText().toStdString()); } catch (...) { job.sfzKeySwitch = 24; }
        try { job.sfzCc1 = std::stoi(sfzCc1Box.getText().toStdString()); } catch (...) { job.sfzCc1 = 100; }
        try { job.sfzCc11 = std::stoi(sfzCc11Box.getText().toStdString()); } catch (...) { job.sfzCc11 = 127; }

        job.masterVolume = static_cast<float>(masterVolumeSlider.getValue());

        return job;
    }

    void MainComponent::setPianoRollPreviewNoteMapFromTracks(const std::vector<mw::core::Track>& tracks)
    {
        lastPianoRollPreviewNotes.clear();

        for (const auto& track : tracks)
        {
            const auto& notes = track.getNotes();
            lastPianoRollPreviewNotes.insert(lastPianoRollPreviewNotes.end(), notes.begin(), notes.end());
        }
    }

void MainComponent::refreshSoundFontList()
    {
        detectedSoundFonts.clear();
        soundFontCombo.clear(juce::dontSendNotification);

        const auto folder = mw::app::AppPaths::soundFontsFolder();

        if (std::filesystem::exists(folder))
        {
            for (const auto& entry : std::filesystem::directory_iterator(folder))
            {
                if (!entry.is_regular_file())
                    continue;

                auto ext = entry.path().extension().string();

                for (auto& c : ext)
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

                if (ext == ".sf2" || ext == ".sf3")
                    detectedSoundFonts.push_back(entry.path());
            }
        }

        std::sort(
            detectedSoundFonts.begin(),
            detectedSoundFonts.end(),
            [](const auto& a, const auto& b)
            {
                return a.filename().string() < b.filename().string();
            }
        );

        for (int i = 0; i < static_cast<int>(detectedSoundFonts.size()); ++i)
            soundFontCombo.addItem(detectedSoundFonts[static_cast<std::size_t>(i)].filename().string(), i + 1);

        if (!detectedSoundFonts.empty())
        {
            int selectedIndex = 0;
            const auto currentPath = std::filesystem::path(soundFontPathBox.getText().toStdString());

            for (int i = 0; i < static_cast<int>(detectedSoundFonts.size()); ++i)
            {
                if (detectedSoundFonts[static_cast<std::size_t>(i)] == currentPath)
                {
                    selectedIndex = i;
                    break;
                }
            }

            soundFontCombo.setSelectedId(selectedIndex + 1, juce::sendNotification);
            soundFontPathBox.setText(detectedSoundFonts[static_cast<std::size_t>(selectedIndex)].string(), juce::dontSendNotification);
            logMessage("SoundFont library auto-scanned. Found " + juce::String(static_cast<int>(detectedSoundFonts.size())) + " .sf2/.sf3 file(s).");
        }
        else
        {
            soundFontPathBox.setText(folder.string());
            logMessage("SoundFont library auto-scan found no .sf2/.sf3 files in: " + folder.string());
        }
    }

    std::filesystem::path MainComponent::getSelectedSoundFontPath() const
    {
        const int selectedId = soundFontCombo.getSelectedId();

        if (selectedId > 0)
        {
            const auto index = static_cast<std::size_t>(selectedId - 1);

            if (index < detectedSoundFonts.size())
                return detectedSoundFonts[index];
        }

        return {};
    }

    mw::core::SampleBackendType MainComponent::getProjectDefaultBackendType() const
    {
        if (appliedProjectBackendId == 3)
            return mw::core::SampleBackendType::VST3;
        return appliedProjectBackendId == 2 ? mw::core::SampleBackendType::SFZ : mw::core::SampleBackendType::SF2;
    }

    std::filesystem::path MainComponent::getProjectDefaultLibraryPath(mw::core::SampleBackendType backendType) const
    {
        if (backendType == mw::core::SampleBackendType::SFZ)
        {
            const auto selectedSfz = getSelectedSfzPath();
            return selectedSfz.empty() ? std::filesystem::path(sfzPathBox.getText().toStdString()) : selectedSfz;
        }

        if (backendType == mw::core::SampleBackendType::SF2)
        {
            const auto selectedSoundFont = getSelectedSoundFontPath();
            return selectedSoundFont.empty() ? std::filesystem::path(soundFontPathBox.getText().toStdString()) : selectedSoundFont;
        }

        return {};
    }


    std::optional<mw::vst::VstPluginDescriptor> MainComponent::getProjectDefaultVstPluginDescriptor()
    {
        if (detectedVstPlugins.empty())
            scanVstPlugins(false);

        std::vector<mw::vst::VstPluginDescriptor> instrumentPlugins;
        for (const auto& plugin : detectedVstPlugins)
        {
            if (isSupportedVstInstrumentPlugin(plugin))
                instrumentPlugins.push_back(plugin);
        }

        if (instrumentPlugins.empty())
            return std::nullopt;

        std::filesystem::path savedPath;
        std::string savedUid;
        if (currentProject)
        {
            const auto& settings = currentProject->getUserSettings();
            savedPath = settings.vst3PluginPath;
            savedUid = settings.vst3PluginUid;
        }

        if (!savedUid.empty() || !savedPath.empty())
        {
            for (const auto& plugin : instrumentPlugins)
            {
                if ((!savedUid.empty() && plugin.uid == savedUid)
                    || (!savedPath.empty() && plugin.bundlePath == savedPath))
                    return plugin;
            }
        }

        const int selectedId = instrumentCombo.getSelectedId();
        if (selectedId > 0 && selectedId <= static_cast<int>(instrumentPlugins.size()))
            return instrumentPlugins[static_cast<std::size_t>(selectedId - 1)];

        return instrumentPlugins.front();
    }

    void MainComponent::applyVstPluginDescriptorToAssignment(mw::core::InstrumentAssignment& assignment, const mw::vst::VstPluginDescriptor& descriptor) const
    {
        assignment.backendType = mw::core::SampleBackendType::VST3;
        assignment.vst3 = {};
        assignment.sampleLibraryPath = descriptor.bundlePath;
        assignment.sampleLibraryDisplayName = descriptor.bundlePath.filename().string();
        assignment.displayName = descriptor.displayName();
        assignment.normalizedName = descriptor.displayName();
        assignment.presetName = descriptor.displayName();
        assignment.vst3.bundlePath = descriptor.bundlePath;
        assignment.vst3.name = descriptor.displayName();
        assignment.vst3.vendor = descriptor.vendor;
        assignment.vst3.version = descriptor.version;
        assignment.vst3.category = descriptor.category;
        assignment.vst3.uid = descriptor.uid;
        assignment.vst3.compatibilitySummary = descriptor.compatibility.summary();
    }

    void MainComponent::captureProjectDefaultVstPluginSelection()
    {
        if (!currentProject)
            return;

        if (detectedVstPlugins.empty())
            scanVstPlugins(false);

        std::optional<mw::vst::VstPluginDescriptor> descriptor;
        std::vector<mw::vst::VstPluginDescriptor> instrumentPlugins;
        for (const auto& plugin : detectedVstPlugins)
        {
            if (isSupportedVstInstrumentPlugin(plugin))
                instrumentPlugins.push_back(plugin);
        }

        const int selectedId = instrumentCombo.getSelectedId();
        if (selectedId > 0 && selectedId <= static_cast<int>(instrumentPlugins.size()))
            descriptor = instrumentPlugins[static_cast<std::size_t>(selectedId - 1)];
        else
            descriptor = getProjectDefaultVstPluginDescriptor();

        auto& settings = currentProject->getUserSettings();

        if (!descriptor)
        {
            settings.vst3PluginPath.clear();
            settings.vst3PluginName.clear();
            settings.vst3PluginVendor.clear();
            settings.vst3PluginVersion.clear();
            settings.vst3PluginCategory.clear();
            settings.vst3PluginUid.clear();
            settings.vst3PluginCompatibilitySummary.clear();
            return;
        }

        settings.vst3PluginPath = descriptor->bundlePath;
        settings.vst3PluginName = descriptor->displayName();
        settings.vst3PluginVendor = descriptor->vendor;
        settings.vst3PluginVersion = descriptor->version;
        settings.vst3PluginCategory = descriptor->category;
        settings.vst3PluginUid = descriptor->uid;
        settings.vst3PluginCompatibilitySummary = descriptor->compatibility.summary();
    }

    void MainComponent::seedTrackSoundLibraryFromProjectDefaults(mw::core::Track& track)
    {
        auto assignment = track.getInstrument();
        const auto backendType = getProjectDefaultBackendType();
        const auto libraryPath = getProjectDefaultLibraryPath(backendType);

        const auto displayName = juce::String(assignment.displayName).trim();
        const auto importedName = juce::String(assignment.originalImportedName).trim();
        const bool needsDefaultInstrumentName =
            isGenericInstrumentDisplayName(displayName)
            || displayName.toLowerCase().startsWith("blank track")
            || displayName.toLowerCase().startsWith("untitled track")
            || importedName.toLowerCase().startsWith("blank track")
            || importedName.toLowerCase().startsWith("untitled track");

        assignment.backendType = backendType;
        assignment.sampleLibraryPath = libraryPath;
        assignment.sampleLibraryDisplayName = libraryPath.empty() ? std::string() : libraryPath.filename().string();
        if (backendType != mw::core::SampleBackendType::VST3)
            assignment.vst3 = {};

        if (backendType == mw::core::SampleBackendType::VST3)
        {
            if (auto descriptor = getProjectDefaultVstPluginDescriptor())
            {
                applyVstPluginDescriptorToAssignment(assignment, *descriptor);
            }
            else
            {
                assignment.backendType = mw::core::SampleBackendType::VST3;
                assignment.sampleLibraryPath.clear();
                assignment.sampleLibraryDisplayName.clear();
                assignment.displayName = "No VST3 plugin selected";
                assignment.normalizedName = "vst3 missing";
                assignment.presetName = "No VST3 plugin selected";
                assignment.vst3 = {};
            }
        }
        else if (backendType == mw::core::SampleBackendType::SFZ)
        {
            const auto sfzName = libraryPath.empty() ? std::string("SFZ Instrument") : libraryPath.stem().string();
            assignment.displayName = sfzName;
            assignment.normalizedName = sfzName;
            assignment.presetName = sfzName;
        }
        else if (backendType == mw::core::SampleBackendType::SF2)
        {
            const auto presets = mw::audio::SoundFontPresetReader::readPresets(libraryPath);
            if (const auto bestPreset = findBestSoundFontPresetForAssignment(presets, assignment))
            {
                applySoundFontPresetToAssignment(assignment, *bestPreset, libraryPath);
            }
            else if (needsDefaultInstrumentName)
            {
                assignment.displayName = "Acoustic Grand Piano";
                assignment.normalizedName = "piano";
                assignment.presetName = "Acoustic Grand Piano";
                assignment.midiBank = 0;
                assignment.midiProgram = 0;
            }
        }

        track.setInstrumentAssignment(assignment);
    }

    void MainComponent::seedMissingTrackSoundLibrariesFromProjectDefaults(mw::core::Project& project)
    {
        for (auto& track : project.getTracks())
        {
            const auto& assignment = track.getInstrument();
            if (assignment.backendType == mw::core::SampleBackendType::None
                || (assignment.backendType == mw::core::SampleBackendType::VST3 && !hasResolvableVst3BundlePath(assignment))
                || (assignment.backendType != mw::core::SampleBackendType::VST3 && assignment.sampleLibraryPath.empty()))
                seedTrackSoundLibraryFromProjectDefaults(track);
        }
    }

    void MainComponent::refreshTrackSoundLibraryDisplay()
    {
        if (!currentProject)
        {
            trackSoundLibraryBox.clear();
            updateOpenVstPluginButtonState();
            return;
        }

        const auto index = getSelectedTrackIndex();
        if (index < 0 || index >= static_cast<int>(currentProject->getTracks().size()))
        {
            trackSoundLibraryBox.clear();
            updateOpenVstPluginButtonState();
            return;
        }

        auto& track = currentProject->getTracks()[static_cast<std::size_t>(index)];
        if (track.isAudioClipTrack())
        {
            track.setInstrumentAssignment(mw::core::makeCustomAudioInstrumentAssignment());
            trackSoundLibraryBox.setText("Custom Audio: AudioClip media", juce::dontSendNotification);
            updateOpenVstPluginButtonState();
            return;
        }

        auto assignment = track.getInstrument();
        if (repairVst3BundlePathIfPossible(assignment))
            track.setInstrumentAssignment(assignment);

        if (track.getInstrument().backendType == mw::core::SampleBackendType::None
            || (track.getInstrument().backendType == mw::core::SampleBackendType::VST3 && !hasResolvableVst3BundlePath(track.getInstrument()))
            || (track.getInstrument().backendType != mw::core::SampleBackendType::VST3 && track.getInstrument().sampleLibraryPath.empty()))
            seedTrackSoundLibraryFromProjectDefaults(track);

        const auto& appliedAssignment = track.getInstrument();
        auto label = getTrackLibrarySummaryLabel(appliedAssignment).trim();
        if (label.isEmpty())
            label = appliedAssignment.backendType == mw::core::SampleBackendType::VST3
                ? juce::String("VST3")
                : juce::String("No library selected");

        trackSoundLibraryBox.setText(label, juce::dontSendNotification);
        updateOpenVstPluginButtonState();
    }

    void MainComponent::applyProjectBackendSelection()
    {
        appliedProjectBackendId = backendCombo.getSelectedId() > 0 ? backendCombo.getSelectedId() : 1;

        std::optional<mw::vst::VstPluginDescriptor> selectedProjectDefaultVstPlugin;
        if (appliedProjectBackendId == 3)
        {
            if (detectedVstPlugins.empty())
                scanVstPlugins(false);

            selectedProjectDefaultVstPlugin = getProjectDefaultVstPluginDescriptor();
        }

        if (currentProject)
        {
            captureProjectUserSettings();

            if (appliedProjectBackendId == 3)
            {
                captureProjectDefaultVstPluginSelection();
                const auto& settings = currentProject->getUserSettings();
                if (settings.vst3PluginPath.empty())
                    logMessage("Applied VST3 as project backend, but no default VST3 instrument is selected. Use VST Plugins > Scan VST3 Plugins, then choose a plugin in the instrument dropdown and apply again.");
                else
                    logMessage("Applied VST3 project backend default: " + settings.vst3PluginName);
            }

            setProjectDirty();
        }

        juce::ignoreUnused(selectedProjectDefaultVstPlugin);
        if (appliedProjectBackendId == 3)
            showVstExperimentalWarningIfNeeded();

        populateInstrumentCombo();
        refreshTrackSoundLibraryDisplay();
        refreshOpenPianoRollInstrumentControls();
        logMessage("Applied project backend default. Future imported/added tracks will seed from this backend. Existing track assignments were left unchanged.");
    }

    void MainComponent::assignSelectedTrackSoundLibrary(const std::filesystem::path& libraryPath, mw::core::SampleBackendType backendType)
    {
        if (!currentProject)
        {
            logMessage("Change Library: no project loaded.");
            return;
        }

        const auto index = getSelectedTrackIndex();
        if (index < 0 || index >= static_cast<int>(currentProject->getTracks().size()))
        {
            logMessage("Change Library: no valid track selected.");
            return;
        }

        auto& track = currentProject->getTracks()[static_cast<std::size_t>(index)];
        if (track.isAudioClipTrack())
        {
            track.setInstrumentAssignment(mw::core::makeCustomAudioInstrumentAssignment());
            refreshTrackSoundLibraryDisplay();
            logMessage("Change Library is MIDI-only. AudioClip tracks use the built-in Custom Audio instrument.");
            return;
        }

        const auto instrumentBeforeApply = track.getInstrument();
        auto assignment = instrumentBeforeApply;
        assignment.backendType = backendType;
        assignment.sampleLibraryPath = libraryPath;
        assignment.sampleLibraryDisplayName = libraryPath.empty() ? std::string() : libraryPath.filename().string();
        if (backendType != mw::core::SampleBackendType::VST3)
            assignment.vst3 = {};

        if (backendType == mw::core::SampleBackendType::SFZ)
        {
            const auto sfzName = libraryPath.empty() ? std::string("SFZ Instrument") : libraryPath.stem().string();
            assignment.displayName = sfzName;
            assignment.normalizedName = sfzName;
            assignment.presetName = sfzName;
        }
        else if (backendType == mw::core::SampleBackendType::SF2)
        {
            const auto presets = mw::audio::SoundFontPresetReader::readPresets(libraryPath);
            if (const auto bestPreset = findBestSoundFontPresetForAssignment(presets, assignment))
            {
                applySoundFontPresetToAssignment(assignment, *bestPreset, libraryPath);
            }
            else if (assignment.displayName.empty() || assignment.displayName == "Default Instrument")
            {
                assignment.displayName = "Acoustic Grand Piano";
                assignment.normalizedName = "piano";
                assignment.presetName = "Acoustic Grand Piano";
                assignment.midiBank = 0;
                assignment.midiProgram = 0;
            }
        }

        track.setInstrumentAssignment(assignment);
        if (!instrumentAssignmentsEqual(instrumentBeforeApply, assignment)
            && (instrumentBeforeApply.backendType == mw::core::SampleBackendType::VST3
                || assignment.backendType == mw::core::SampleBackendType::VST3))
        {
            closeVstPluginWindowForTrack(index,
                "Closed open VST plugin window after changing the selected track library.");
        }
        trackBackendCombo.setSelectedId(backendType == mw::core::SampleBackendType::SFZ ? 3 : 2, juce::dontSendNotification);
        populateInstrumentCombo();
        syncTrackInspectorFromSelection();
        updateTrackSummary(*currentProject);
        refreshTrackManagerText();
        refreshOpenPianoRollInstrumentControls();
        setProjectDirty();
        logMessage("Track sound library assigned: " + libraryPath.string());
    }

    void MainComponent::chooseTrackSoundLibrary()
    {
        if (!currentProject)
        {
            logMessage("Change Library: no project loaded.");
            return;
        }

        const auto index = getSelectedTrackIndex();
        if (index < 0 || index >= static_cast<int>(currentProject->getTracks().size()))
        {
            logMessage("Change Library: no valid track selected.");
            return;
        }

        juce::PopupMenu menu;
        menu.addItem(1, "Use selected SoundFont/SF3 project default");
        menu.addItem(2, "Use selected SFZ project default");
        menu.addSeparator();
        menu.addItem(3, "Choose SF2/SF3 file...");
        menu.addItem(4, "Choose SFZ file...");
        menu.addItem(5, "Choose scanned VST3 plugin...");
        menu.showMenuAsync(
            juce::PopupMenu::Options().withTargetComponent(&changeTrackLibraryButton),
            [this](int result)
            {
                if (result == 1)
                    assignSelectedTrackSoundLibrary(getProjectDefaultLibraryPath(mw::core::SampleBackendType::SF2), mw::core::SampleBackendType::SF2);
                else if (result == 2)
                    assignSelectedTrackSoundLibrary(getProjectDefaultLibraryPath(mw::core::SampleBackendType::SFZ), mw::core::SampleBackendType::SFZ);
                else if (result == 3)
                {
                    activeFileChooser = std::make_unique<juce::FileChooser>("Choose SoundFont / SF3 for selected track", juce::File(mw::app::AppPaths::soundFontsFolder().string()), "*.sf2;*.sf3");
                    activeFileChooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this](const juce::FileChooser& chooser)
                    {
                        const auto file = chooser.getResult();
                        if (file == juce::File{}) return;
                        assignSelectedTrackSoundLibrary(std::filesystem::path(file.getFullPathName().toStdString()), mw::core::SampleBackendType::SF2);
                    });
                }
                else if (result == 4)
                {
                    activeFileChooser = std::make_unique<juce::FileChooser>("Choose SFZ for selected track", juce::File(mw::app::AppPaths::sfzFolder().string()), "*.sfz");
                    activeFileChooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this](const juce::FileChooser& chooser)
                    {
                        const auto file = chooser.getResult();
                        if (file == juce::File{}) return;
                        assignSelectedTrackSoundLibrary(std::filesystem::path(file.getFullPathName().toStdString()), mw::core::SampleBackendType::SFZ);
                    });
                }
                else if (result == 5)
                {
                    if (detectedVstPlugins.empty())
                        scanVstPlugins(false);

                    juce::PopupMenu vstMenu;
                    int id = 1;
                    std::vector<mw::vst::VstPluginDescriptor> candidates;
                    for (const auto& plugin : detectedVstPlugins)
                    {
                        if (!isSupportedVstInstrumentPlugin(plugin))
                            continue;
                        candidates.push_back(plugin);
                        vstMenu.addItem(id++, juce::String(plugin.displayName()) + (plugin.status == mw::vst::VstPluginScanStatus::Warning ? "  ⚠" : ""));
                    }

                    if (candidates.empty())
                    {
                        logMessage("No VST3 instrument candidates found. Use VST Plugins > Scan VST3 Plugins after placing .vst3 bundles in workspace\\vst3.");
                        return;
                    }

                    vstMenu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&changeTrackLibraryButton), [this, candidates](int vstResult)
                    {
                        if (vstResult <= 0 || vstResult > static_cast<int>(candidates.size()))
                            return;
                        assignSelectedTrackVstPlugin(candidates[static_cast<std::size_t>(vstResult - 1)]);
                    });
                }
            }
        );
    }

    void MainComponent::refreshPianoRollTrackSoundLibraryDisplay(PianoRollEditorWindowState& state)
    {
        if (!currentProject)
        {
            state.trackSoundLibraryBox.clear();
            if (auto* content = dynamic_cast<PianoRollWindowContent*>(state.content.get()))
                content->setHeaderInstrumentInfo({});
            return;
        }

        if (state.trackIndex < 0 || state.trackIndex >= static_cast<int>(currentProject->getTracks().size()))
        {
            state.trackSoundLibraryBox.clear();
            if (auto* content = dynamic_cast<PianoRollWindowContent*>(state.content.get()))
                content->setHeaderInstrumentInfo({});
            return;
        }

        auto& track = currentProject->getTracks()[static_cast<std::size_t>(state.trackIndex)];

        if (track.isAudioClipTrack())
        {
            state.hasPendingInstrumentAssignment = false;
            track.setInstrumentAssignment(mw::core::makeCustomAudioInstrumentAssignment());
            state.trackSoundLibraryBox.setText("Custom Audio: AudioClip media", juce::dontSendNotification);
            if (auto* content = dynamic_cast<PianoRollWindowContent*>(state.content.get()))
                content->setHeaderInstrumentInfo(getPianoRollHeaderInstrumentTextForState(state));
            updatePianoRollWindowDirtyIndicator(state);
            refreshAggregatePianoRollDirtyFlag();
            return;
        }

        auto appliedAssignment = track.getInstrument();
        if (!state.hasPendingInstrumentAssignment && repairVst3BundlePathIfPossible(appliedAssignment))
            track.setInstrumentAssignment(appliedAssignment);

        if (!state.hasPendingInstrumentAssignment
            && (track.getInstrument().backendType == mw::core::SampleBackendType::None
                || (track.getInstrument().backendType == mw::core::SampleBackendType::VST3 && !hasResolvableVst3BundlePath(track.getInstrument()))
                || (track.getInstrument().backendType != mw::core::SampleBackendType::VST3 && track.getInstrument().sampleLibraryPath.empty())))
            seedTrackSoundLibraryFromProjectDefaults(track);

        const auto& assignment = state.hasPendingInstrumentAssignment
            ? state.pendingInstrumentAssignment
            : track.getInstrument();

        juce::String label;
        if (state.hasPendingInstrumentAssignment)
            label << "Pending ";
        label << getTrackLibrarySummaryLabel(assignment);
        label = label.trim();
        if (label.isEmpty())
            label = assignment.backendType == mw::core::SampleBackendType::VST3 ? juce::String("VST3") : juce::String("No library selected");
        state.trackSoundLibraryBox.setText(label, juce::dontSendNotification);

        if (auto* content = dynamic_cast<PianoRollWindowContent*>(state.content.get()))
            content->setHeaderInstrumentInfo(getPianoRollHeaderInstrumentTextForState(state));
    }

    juce::String MainComponent::resolvePianoRollInstrumentName(const PianoRollEditorWindowState& state, const mw::core::InstrumentAssignment& assignment) const
    {
        auto clean = [](const juce::String& text)
        {
            return text.trim();
        };

        auto candidate = clean(assignment.displayName);
        if (!isGenericInstrumentDisplayName(candidate))
            return candidate;

        candidate = clean(assignment.presetName);
        if (!isGenericInstrumentDisplayName(candidate))
            return candidate;

        for (const auto& preset : state.instrumentPresets)
        {
            if (preset.bank == assignment.midiBank && preset.program == assignment.midiProgram)
            {
                candidate = clean(preset.name);
                if (!isGenericInstrumentDisplayName(candidate))
                    return candidate;
            }
        }

        candidate = stripBankProgramPrefix(state.instrumentCombo.getText());
        if (!isGenericInstrumentDisplayName(candidate) && candidate != "Choose instrument")
            return candidate;

        const auto& instruments = gmInstruments();
        for (const auto& instrument : instruments)
        {
            if (instrument.program == assignment.midiProgram)
            {
                candidate = clean(instrument.name);
                if (!candidate.isEmpty())
                    return candidate;
            }
        }

        if (!assignment.sampleLibraryDisplayName.empty())
        {
            candidate = clean(assignment.sampleLibraryDisplayName);
            if (!isGenericInstrumentDisplayName(candidate))
                return candidate;
        }

        if (!assignment.sampleLibraryPath.empty())
        {
            candidate = clean(juce::String(assignment.sampleLibraryPath.stem().string()));
            if (!isGenericInstrumentDisplayName(candidate))
                return candidate;
        }

        return "Default Instrument";
    }

    juce::String MainComponent::getPianoRollHeaderInstrumentTextForState(const PianoRollEditorWindowState& state) const
    {
        if (!currentProject || state.trackIndex < 0 || state.trackIndex >= static_cast<int>(currentProject->getTracks().size()))
            return {};

        const auto& track = currentProject->getTracks()[static_cast<std::size_t>(state.trackIndex)];
        const auto& assignment = state.hasPendingInstrumentAssignment ? state.pendingInstrumentAssignment : track.getInstrument();
        return getPianoRollHeaderInstrumentText(assignment, state.hasPendingInstrumentAssignment, resolvePianoRollInstrumentName(state, assignment));
    }

    void MainComponent::populatePianoRollInstrumentCombo(PianoRollEditorWindowState& state)
    {
        state.suppressInstrumentChange = true;
        state.instrumentPresets.clear();
        state.instrumentCombo.clear(juce::dontSendNotification);
        state.trackBackendCombo.clear(juce::dontSendNotification);
        state.trackBackendCombo.addItem("Use Project Backend", 1);
        state.trackBackendCombo.addItem("SF2", 2);
        state.trackBackendCombo.addItem("SFZ", 3);
        state.trackBackendCombo.addItem("VST3 Plugin", 4);
        state.trackBackendCombo.setTextWhenNothingSelected("Choose backend");
        state.instrumentCombo.setTextWhenNothingSelected("Choose instrument");

        if (!currentProject || state.trackIndex < 0 || state.trackIndex >= static_cast<int>(currentProject->getTracks().size()))
        {
            state.suppressInstrumentChange = false;
            return;
        }

        auto& track = currentProject->getTracks()[static_cast<std::size_t>(state.trackIndex)];

        if (track.isAudioClipTrack())
        {
            state.hasPendingInstrumentAssignment = false;
            track.setInstrumentAssignment(mw::core::makeCustomAudioInstrumentAssignment());
            state.trackBackendCombo.setSelectedId(1, juce::dontSendNotification);
            state.instrumentCombo.addItem("Custom Audio (AudioClip)", 1);
            state.instrumentCombo.setSelectedId(1, juce::dontSendNotification);
            refreshPianoRollTrackSoundLibraryDisplay(state);
            state.suppressInstrumentChange = false;
            return;
        }

        if (!state.hasPendingInstrumentAssignment
            && (track.getInstrument().backendType == mw::core::SampleBackendType::None
                || (track.getInstrument().backendType != mw::core::SampleBackendType::VST3 && track.getInstrument().sampleLibraryPath.empty())))
            seedTrackSoundLibraryFromProjectDefaults(track);

        const auto& assignment = state.hasPendingInstrumentAssignment
            ? state.pendingInstrumentAssignment
            : track.getInstrument();

        const auto backendType = assignment.backendType == mw::core::SampleBackendType::VST3
            ? mw::core::SampleBackendType::VST3
            : (assignment.backendType == mw::core::SampleBackendType::SFZ
                ? mw::core::SampleBackendType::SFZ
                : mw::core::SampleBackendType::SF2);

        state.trackBackendCombo.setSelectedId(
            backendType == mw::core::SampleBackendType::VST3 ? 4 : (backendType == mw::core::SampleBackendType::SFZ ? 3 : 2),
            juce::dontSendNotification);

        if (backendType == mw::core::SampleBackendType::VST3)
        {
            if (detectedVstPlugins.empty())
                scanVstPlugins(false);

            int selectedId = 1;
            int itemId = 1;
            for (const auto& plugin : detectedVstPlugins)
            {
                if (!isSupportedVstInstrumentPlugin(plugin))
                    continue;

                juce::String label = plugin.displayName();
                if (!plugin.vendor.empty())
                    label << " - " << plugin.vendor;
                if (plugin.status == mw::vst::VstPluginScanStatus::Warning)
                    label << "  ⚠";

                state.instrumentCombo.addItem(label, itemId);
                const auto selectedBundlePath = resolveVst3BundlePath(assignment);
                if (!selectedBundlePath.empty() && plugin.bundlePath == selectedBundlePath)
                    selectedId = itemId;
                ++itemId;
            }

            if (state.instrumentCombo.getNumItems() == 0)
                state.instrumentCombo.addItem("No VST3 instruments found", 1);

            state.instrumentCombo.setSelectedId(selectedId, juce::dontSendNotification);
            refreshPianoRollTrackSoundLibraryDisplay(state);
            state.suppressInstrumentChange = false;
            return;
        }

        if (backendType == mw::core::SampleBackendType::SFZ)
        {
            const auto label = assignment.sampleLibraryPath.empty()
                ? juce::String("Selected SFZ Instrument")
                : juce::String(assignment.sampleLibraryPath.filename().string());
            state.instrumentCombo.addItem(label, 1);
            state.instrumentCombo.setSelectedId(1, juce::dontSendNotification);
            refreshPianoRollTrackSoundLibraryDisplay(state);
            state.suppressInstrumentChange = false;
            return;
        }

        if (!assignment.sampleLibraryPath.empty() && std::filesystem::exists(assignment.sampleLibraryPath))
            state.instrumentPresets = mw::audio::SoundFontPresetReader::readPresets(assignment.sampleLibraryPath);

        if (!state.instrumentPresets.empty())
        {
            int selectedId = 1;

            for (int i = 0; i < static_cast<int>(state.instrumentPresets.size()); ++i)
            {
                const auto& preset = state.instrumentPresets[static_cast<std::size_t>(i)];
                juce::String label;
                label << "Bank " << preset.bank << " / Program " << preset.program << " - " << preset.name;
                state.instrumentCombo.addItem(label, i + 1);

                if (preset.bank == assignment.midiBank && preset.program == assignment.midiProgram)
                    selectedId = i + 1;
            }

            state.instrumentCombo.setSelectedId(selectedId, juce::dontSendNotification);
            refreshPianoRollTrackSoundLibraryDisplay(state);
            state.suppressInstrumentChange = false;
            return;
        }

        const auto& instruments = gmInstruments();
        int selectedId = 1;

        for (int i = 0; i < static_cast<int>(instruments.size()); ++i)
        {
            juce::String label;
            label << instruments[static_cast<std::size_t>(i)].name
                  << " (GM " << instruments[static_cast<std::size_t>(i)].program << ")";
            state.instrumentCombo.addItem(label, i + 1);

            if (instruments[static_cast<std::size_t>(i)].program == assignment.midiProgram)
                selectedId = i + 1;
        }

        state.instrumentCombo.setSelectedId(selectedId, juce::dontSendNotification);
        refreshPianoRollTrackSoundLibraryDisplay(state);
        state.suppressInstrumentChange = false;
    }

    void MainComponent::capturePianoRollInstrumentUndoState(PianoRollEditorWindowState& state, const juce::String&)
    {
        if (!currentProject || state.trackIndex < 0 || state.trackIndex >= static_cast<int>(currentProject->getTracks().size()))
            return;

        const auto& track = currentProject->getTracks()[static_cast<std::size_t>(state.trackIndex)];
        const auto current = makeUndoSafeInstrumentAssignment(track.getInstrument());

        if (!state.instrumentUndoStack.empty() && instrumentAssignmentsEqual(state.instrumentUndoStack.back(), current))
            return;

        state.instrumentUndoStack.push_back(current);

        constexpr std::size_t maxUndoSteps = 10;
        if (state.instrumentUndoStack.size() > maxUndoSteps)
            state.instrumentUndoStack.erase(state.instrumentUndoStack.begin());

        state.instrumentRedoStack.clear();
    }

    void MainComponent::assignPianoRollTrackSoundLibrary(PianoRollEditorWindowState& state, const std::filesystem::path& libraryPath, mw::core::SampleBackendType backendType)
    {
        if (!currentProject || state.trackIndex < 0 || state.trackIndex >= static_cast<int>(currentProject->getTracks().size()))
        {
            logMessage("Piano Roll Change Library: no valid track selected.");
            return;
        }

        auto& track = currentProject->getTracks()[static_cast<std::size_t>(state.trackIndex)];
        if (track.isAudioClipTrack())
        {
            state.hasPendingInstrumentAssignment = false;
            track.setInstrumentAssignment(mw::core::makeCustomAudioInstrumentAssignment());
            populatePianoRollInstrumentCombo(state);
            logMessage("Piano Roll Change Library is MIDI-only. AudioClip tracks use Custom Audio.");
            return;
        }

        auto assignment = state.hasPendingInstrumentAssignment ? state.pendingInstrumentAssignment : track.getInstrument();
        assignment.backendType = backendType;
        assignment.sampleLibraryPath = libraryPath;
        assignment.sampleLibraryDisplayName = libraryPath.empty() ? std::string() : libraryPath.filename().string();
        if (backendType != mw::core::SampleBackendType::VST3)
            assignment.vst3 = {};

        if (backendType == mw::core::SampleBackendType::SFZ)
        {
            const auto sfzName = libraryPath.empty() ? std::string("SFZ Instrument") : libraryPath.stem().string();
            assignment.displayName = sfzName;
            assignment.normalizedName = sfzName;
            assignment.presetName = sfzName;
        }
        else
        {
            const auto presets = mw::audio::SoundFontPresetReader::readPresets(libraryPath);
            if (!presets.empty())
            {
                const auto& preset = presets.front();
                assignment.displayName = preset.name;
                assignment.normalizedName = preset.name;
                assignment.presetName = preset.name;
                assignment.midiBank = preset.bank;
                assignment.midiProgram = preset.program;
            }
            else if (assignment.displayName.empty() || assignment.displayName == "Default Instrument")
            {
                assignment.displayName = "Acoustic Grand Piano";
                assignment.normalizedName = "piano";
                assignment.presetName = "Acoustic Grand Piano";
                assignment.midiBank = 0;
                assignment.midiProgram = 0;
            }
        }

        if (instrumentAssignmentsEqual(track.getInstrument(), assignment))
        {
            state.hasPendingInstrumentAssignment = false;
            populatePianoRollInstrumentCombo(state);
            refreshPianoRollTrackSoundLibraryDisplay(state);
            updatePianoRollWindowDirtyIndicator(state);
            refreshAggregatePianoRollDirtyFlag();
            logMessage("Piano Roll library already matches " + getTrackDisplayName(state.trackIndex) + ".");
            return;
        }

        state.pendingInstrumentAssignment = assignment;
        state.hasPendingInstrumentAssignment = true;
        populatePianoRollInstrumentCombo(state);
        refreshPianoRollTrackSoundLibraryDisplay(state);
        updatePianoRollWindowDirtyIndicator(state);
        refreshAggregatePianoRollDirtyFlag();
        logMessage("Piano Roll library selected for " + getTrackDisplayName(state.trackIndex) + ". Click Apply Track Settings to confirm.");
    }

    void MainComponent::applyPianoRollInstrumentSelection(PianoRollEditorWindowState& state, bool)
    {
        if (state.suppressInstrumentChange)
            return;

        if (!currentProject || state.trackIndex < 0 || state.trackIndex >= static_cast<int>(currentProject->getTracks().size()))
            return;

        auto& track = currentProject->getTracks()[static_cast<std::size_t>(state.trackIndex)];
        if (track.isAudioClipTrack())
        {
            state.hasPendingInstrumentAssignment = false;
            track.setInstrumentAssignment(mw::core::makeCustomAudioInstrumentAssignment());
            populatePianoRollInstrumentCombo(state);
            return;
        }

        if (!state.hasPendingInstrumentAssignment
            && (track.getInstrument().backendType == mw::core::SampleBackendType::None
                || (track.getInstrument().backendType != mw::core::SampleBackendType::VST3 && track.getInstrument().sampleLibraryPath.empty())))
            seedTrackSoundLibraryFromProjectDefaults(track);

        auto assignment = state.hasPendingInstrumentAssignment ? state.pendingInstrumentAssignment : track.getInstrument();

        const auto backendId = state.trackBackendCombo.getSelectedId();
        const auto backendType = backendId == 4
            ? mw::core::SampleBackendType::VST3
            : (backendId == 3
                ? mw::core::SampleBackendType::SFZ
                : (backendId == 2 ? mw::core::SampleBackendType::SF2 : getProjectDefaultBackendType()));

        if (backendType != assignment.backendType)
        {
            if (backendType == mw::core::SampleBackendType::VST3)
            {
                if (detectedVstPlugins.empty())
                    scanVstPlugins(false);

                std::vector<mw::vst::VstPluginDescriptor> candidates;
                for (const auto& plugin : detectedVstPlugins)
                    if (isSupportedVstInstrumentPlugin(plugin))
                        candidates.push_back(plugin);

                if (!candidates.empty())
                {
                    const auto& plugin = candidates.front();
                    assignment.backendType = mw::core::SampleBackendType::VST3;
                    assignment.vst3 = {};
                    assignment.sampleLibraryPath = plugin.bundlePath;
                    assignment.sampleLibraryDisplayName = plugin.bundlePath.filename().string();
                    assignment.displayName = plugin.displayName();
                    assignment.normalizedName = plugin.displayName();
                    assignment.presetName = plugin.displayName();
                    assignment.vst3.bundlePath = plugin.bundlePath;
                    assignment.vst3.name = plugin.displayName();
                    assignment.vst3.vendor = plugin.vendor;
                    assignment.vst3.version = plugin.version;
                    assignment.vst3.category = plugin.category;
                    assignment.vst3.uid = plugin.uid;
                    assignment.vst3.compatibilitySummary = plugin.compatibility.summary();
                }
            }
            else
            {
                const auto libraryPath = getProjectDefaultLibraryPath(backendType);
                assignPianoRollTrackSoundLibrary(state, libraryPath, backendType);
                return;
            }
        }

        const int instrumentIndex = state.instrumentCombo.getSelectedId() - 1;

        if (backendType == mw::core::SampleBackendType::VST3)
        {
            if (detectedVstPlugins.empty())
                scanVstPlugins(false);

            std::vector<mw::vst::VstPluginDescriptor> candidates;
            for (const auto& plugin : detectedVstPlugins)
                if (isSupportedVstInstrumentPlugin(plugin))
                    candidates.push_back(plugin);

            if (instrumentIndex >= 0 && instrumentIndex < static_cast<int>(candidates.size()))
            {
                const auto& plugin = candidates[static_cast<std::size_t>(instrumentIndex)];
                assignment.backendType = mw::core::SampleBackendType::VST3;
                assignment.vst3 = {};
                assignment.sampleLibraryPath = plugin.bundlePath;
                assignment.sampleLibraryDisplayName = plugin.bundlePath.filename().string();
                assignment.displayName = plugin.displayName();
                assignment.normalizedName = plugin.displayName();
                assignment.presetName = plugin.displayName();
                assignment.vst3.bundlePath = plugin.bundlePath;
                assignment.vst3.name = plugin.displayName();
                assignment.vst3.vendor = plugin.vendor;
                assignment.vst3.version = plugin.version;
                assignment.vst3.category = plugin.category;
                assignment.vst3.uid = plugin.uid;
                assignment.vst3.compatibilitySummary = plugin.compatibility.summary();
            }
        }
        else if (backendType == mw::core::SampleBackendType::SFZ)
        {
            const auto sfzPath = !assignment.sampleLibraryPath.empty()
                ? assignment.sampleLibraryPath
                : getProjectDefaultLibraryPath(mw::core::SampleBackendType::SFZ);
            const auto sfzName = sfzPath.empty() ? std::string("SFZ Instrument") : sfzPath.stem().string();
            assignment.backendType = mw::core::SampleBackendType::SFZ;
            assignment.sampleLibraryPath = sfzPath;
            assignment.sampleLibraryDisplayName = sfzPath.empty() ? std::string() : sfzPath.filename().string();
            assignment.vst3 = {};
            assignment.displayName = sfzName;
            assignment.normalizedName = sfzName;
            assignment.presetName = sfzName;
        }
        else if (!state.instrumentPresets.empty())
        {
            if (instrumentIndex >= 0 && instrumentIndex < static_cast<int>(state.instrumentPresets.size()))
            {
                const auto& selected = state.instrumentPresets[static_cast<std::size_t>(instrumentIndex)];
                assignment.displayName = selected.name;
                assignment.normalizedName = selected.name;
                assignment.presetName = selected.name;
                assignment.midiBank = selected.bank;
                assignment.midiProgram = selected.program;
                assignment.backendType = mw::core::SampleBackendType::SF2;
                assignment.vst3 = {};
                if (assignment.sampleLibraryPath.empty())
                    assignment.sampleLibraryPath = getProjectDefaultLibraryPath(mw::core::SampleBackendType::SF2);
                assignment.sampleLibraryDisplayName = assignment.sampleLibraryPath.empty() ? std::string() : assignment.sampleLibraryPath.filename().string();
            }
        }
        else if (instrumentIndex >= 0 && instrumentIndex < static_cast<int>(gmInstruments().size()))
        {
            const auto& selected = gmInstruments()[static_cast<std::size_t>(instrumentIndex)];
            assignment.displayName = selected.name;
            assignment.normalizedName = selected.normalizedName;
            assignment.presetName = selected.name;
            assignment.midiProgram = selected.program;
            assignment.backendType = mw::core::SampleBackendType::SF2;
            assignment.vst3 = {};
            if (assignment.sampleLibraryPath.empty())
                assignment.sampleLibraryPath = getProjectDefaultLibraryPath(mw::core::SampleBackendType::SF2);
            assignment.sampleLibraryDisplayName = assignment.sampleLibraryPath.empty() ? std::string() : assignment.sampleLibraryPath.filename().string();
        }

        const auto committed = track.getInstrument();
        if (instrumentAssignmentsEqual(committed, assignment))
        {
            state.hasPendingInstrumentAssignment = false;
            populatePianoRollInstrumentCombo(state);
            refreshPianoRollTrackSoundLibraryDisplay(state);
            updatePianoRollWindowDirtyIndicator(state);
            refreshAggregatePianoRollDirtyFlag();
            return;
        }

        if (assignment.backendType == mw::core::SampleBackendType::VST3)
            showVstExperimentalWarningIfNeeded();

        state.pendingInstrumentAssignment = assignment;
        state.hasPendingInstrumentAssignment = true;
        refreshPianoRollTrackSoundLibraryDisplay(state);
        updatePianoRollWindowDirtyIndicator(state);
        refreshAggregatePianoRollDirtyFlag();
        logMessage("Piano Roll instrument selected for " + getTrackDisplayName(state.trackIndex) + ". Click Apply Track Settings to confirm.");
    }

    bool MainComponent::applyPendingPianoRollTrackSettings(PianoRollEditorWindowState& state)
    {
        if (!currentProject || state.trackIndex < 0 || state.trackIndex >= static_cast<int>(currentProject->getTracks().size()))
            return false;

        if (!state.hasPendingInstrumentAssignment)
        {
            // The user may click Apply Track Settings after changing controls that
            // already resolved back to the current assignment, or after opening a
            // VST editor from another window. In either case, close any open VST
            // editor for this track so a stale plugin UI is not left attached to
            // an assignment the user just confirmed.
            closeVstPluginWindowForTrack(state.trackIndex,
                "Closed open VST plugin window after applying Piano Roll track settings.");
            return false;
        }

        auto& track = currentProject->getTracks()[static_cast<std::size_t>(state.trackIndex)];
        if (track.isAudioClipTrack())
        {
            state.hasPendingInstrumentAssignment = false;
            track.setInstrumentAssignment(mw::core::makeCustomAudioInstrumentAssignment());
            populatePianoRollInstrumentCombo(state);
            refreshAggregatePianoRollDirtyFlag();
            updatePianoRollWindowDirtyIndicator(state);
            return false;
        }

        const auto before = track.getInstrument();
        const auto assignment = state.pendingInstrumentAssignment;
        state.hasPendingInstrumentAssignment = false;

        // Close before committing/comparing so an editor for the old plugin cannot
        // remain open while the track settings are being confirmed.
        closeVstPluginWindowForTrack(state.trackIndex,
            "Closed open VST plugin window before applying changed Piano Roll track settings.");

        if (instrumentAssignmentsEqual(before, assignment))
        {
            populatePianoRollInstrumentCombo(state);
            refreshPianoRollTrackSoundLibraryDisplay(state);
            refreshAggregatePianoRollDirtyFlag();
            updatePianoRollWindowDirtyIndicator(state);
            logMessage("Piano Roll track settings were already current for " + getTrackDisplayName(state.trackIndex) + ".");
            return false;
        }

        if (assignment.backendType == mw::core::SampleBackendType::VST3)
            showVstExperimentalWarningIfNeeded();

        capturePianoRollInstrumentUndoState(state, "Apply Piano Roll Track Settings");
        track.setInstrumentAssignment(assignment);
        populatePianoRollInstrumentCombo(state);
        refreshPianoRollTrackSoundLibraryDisplay(state);
        refreshTrackSoundLibraryDisplay();
        if (state.trackIndex == getSelectedTrackIndex())
            syncTrackInspectorFromSelection();
        updateTrackSummary(*currentProject);
        refreshTrackManagerText();
        recordExternalTrackStateUpdate(state.trackIndex, true);
        setProjectDirty();
        refreshAggregatePianoRollDirtyFlag();
        updatePianoRollWindowDirtyIndicator(state);
        logMessage("Applied Piano Roll track settings for " + getTrackDisplayName(state.trackIndex) + ": " + juce::String(assignment.displayName));
        return true;
    }

    void MainComponent::discardPendingPianoRollTrackSettings(PianoRollEditorWindowState& state)
    {
        if (!state.hasPendingInstrumentAssignment)
            return;

        state.hasPendingInstrumentAssignment = false;
        populatePianoRollInstrumentCombo(state);
        refreshPianoRollTrackSoundLibraryDisplay(state);
        refreshAggregatePianoRollDirtyFlag();
        updatePianoRollWindowDirtyIndicator(state);
        logMessage("Discarded pending Piano Roll track settings for " + getTrackDisplayName(state.trackIndex) + ".");
    }

    void MainComponent::choosePianoRollTrackSoundLibrary(PianoRollEditorWindowState& state)
    {
        if (!currentProject || state.trackIndex < 0 || state.trackIndex >= static_cast<int>(currentProject->getTracks().size()))
        {
            logMessage("Piano Roll Change Library: no valid track selected.");
            return;
        }

        const auto trackIndex = state.trackIndex;
        juce::PopupMenu menu;
        menu.addItem(1, "Use selected SoundFont/SF3 project default");
        menu.addItem(2, "Use selected SFZ project default");
        menu.addSeparator();
        menu.addItem(3, "Choose SF2/SF3 file...");
        menu.addItem(4, "Choose SFZ file...");
        menu.addItem(5, "Choose scanned VST3 plugin...");
        menu.showMenuAsync(
            juce::PopupMenu::Options().withTargetComponent(&state.changeLibraryButton),
            [this, trackIndex](int result)
            {
                auto* liveState = findPianoRollEditorWindow(trackIndex);
                if (liveState == nullptr)
                    return;

                if (result == 1)
                    assignPianoRollTrackSoundLibrary(*liveState, getProjectDefaultLibraryPath(mw::core::SampleBackendType::SF2), mw::core::SampleBackendType::SF2);
                else if (result == 2)
                    assignPianoRollTrackSoundLibrary(*liveState, getProjectDefaultLibraryPath(mw::core::SampleBackendType::SFZ), mw::core::SampleBackendType::SFZ);
                else if (result == 3)
                {
                    activeFileChooser = std::make_unique<juce::FileChooser>("Choose SoundFont / SF3 for Piano Roll track", juce::File(mw::app::AppPaths::soundFontsFolder().string()), "*.sf2;*.sf3");
                    activeFileChooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this, trackIndex](const juce::FileChooser& chooser)
                    {
                        const auto file = chooser.getResult();
                        if (file == juce::File{}) return;
                        if (auto* stateForFile = findPianoRollEditorWindow(trackIndex))
                            assignPianoRollTrackSoundLibrary(*stateForFile, std::filesystem::path(file.getFullPathName().toStdString()), mw::core::SampleBackendType::SF2);
                    });
                }
                else if (result == 4)
                {
                    activeFileChooser = std::make_unique<juce::FileChooser>("Choose SFZ for Piano Roll track", juce::File(mw::app::AppPaths::sfzFolder().string()), "*.sfz");
                    activeFileChooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this, trackIndex](const juce::FileChooser& chooser)
                    {
                        const auto file = chooser.getResult();
                        if (file == juce::File{}) return;
                        if (auto* stateForFile = findPianoRollEditorWindow(trackIndex))
                            assignPianoRollTrackSoundLibrary(*stateForFile, std::filesystem::path(file.getFullPathName().toStdString()), mw::core::SampleBackendType::SFZ);
                    });
                }
                else if (result == 5)
                {
                    if (detectedVstPlugins.empty())
                        scanVstPlugins(false);

                    juce::PopupMenu vstMenu;
                    std::vector<mw::vst::VstPluginDescriptor> candidates;
                    int id = 1;
                    for (const auto& plugin : detectedVstPlugins)
                    {
                        if (!isSupportedVstInstrumentPlugin(plugin))
                            continue;
                        candidates.push_back(plugin);
                        vstMenu.addItem(id++, juce::String(plugin.displayName()) + (plugin.status == mw::vst::VstPluginScanStatus::Warning ? "  ⚠" : ""));
                    }

                    if (candidates.empty())
                    {
                        logMessage("No VST3 instrument candidates found for Piano Roll.");
                        return;
                    }

                    vstMenu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&liveState->changeLibraryButton), [this, trackIndex, candidates](int vstResult)
                    {
                        if (vstResult <= 0 || vstResult > static_cast<int>(candidates.size()))
                            return;

                        auto* stateForVst = findPianoRollEditorWindow(trackIndex);
                        if (stateForVst == nullptr || !currentProject)
                            return;

                        auto& trackForVst = currentProject->getTracks()[static_cast<std::size_t>(trackIndex)];
                        auto assignment = stateForVst->hasPendingInstrumentAssignment ? stateForVst->pendingInstrumentAssignment : trackForVst.getInstrument();
                        const auto& plugin = candidates[static_cast<std::size_t>(vstResult - 1)];
                        assignment.backendType = mw::core::SampleBackendType::VST3;
                        assignment.vst3 = {};
                        assignment.sampleLibraryPath = plugin.bundlePath;
                        assignment.sampleLibraryDisplayName = plugin.bundlePath.filename().string();
                        assignment.displayName = plugin.displayName();
                        assignment.normalizedName = plugin.displayName();
                        assignment.presetName = plugin.displayName();
                        assignment.vst3.bundlePath = plugin.bundlePath;
                        assignment.vst3.name = plugin.displayName();
                        assignment.vst3.vendor = plugin.vendor;
                        assignment.vst3.version = plugin.version;
                        assignment.vst3.category = plugin.category;
                        assignment.vst3.uid = plugin.uid;
                        assignment.vst3.compatibilitySummary = plugin.compatibility.summary();

                        showVstExperimentalWarningIfNeeded();

                        stateForVst->pendingInstrumentAssignment = assignment;
                        stateForVst->hasPendingInstrumentAssignment = true;
                        populatePianoRollInstrumentCombo(*stateForVst);
                        refreshPianoRollTrackSoundLibraryDisplay(*stateForVst);
                        updatePianoRollWindowDirtyIndicator(*stateForVst);
                        refreshAggregatePianoRollDirtyFlag();
                        logMessage("Piano Roll VST3 plugin selected for " + getTrackDisplayName(trackIndex) + ". Click Apply Track Settings to confirm.");
                    });
                }
            }
        );
    }

    bool MainComponent::undoPianoRollEditorAction(PianoRollEditorWindowState& state)
    {
        if (!currentProject || state.trackIndex < 0 || state.trackIndex >= static_cast<int>(currentProject->getTracks().size()))
            return false;

        auto& track = currentProject->getTracks()[static_cast<std::size_t>(state.trackIndex)];
        if (state.hasPendingInstrumentAssignment)
        {
            discardPendingPianoRollTrackSettings(state);
            logMessage("Undo discarded pending Piano Roll track settings for " + getTrackDisplayName(state.trackIndex) + ".");
            return true;
        }

        if (!state.instrumentUndoStack.empty())
        {
            state.instrumentRedoStack.push_back(makeUndoSafeInstrumentAssignment(track.getInstrument()));
            auto previous = state.instrumentUndoStack.back();
            state.instrumentUndoStack.pop_back();
            previous = prepareInstrumentAssignmentForUndoRestore(track.getInstrument(), std::move(previous));
            track.setInstrumentAssignment(previous);
            populatePianoRollInstrumentCombo(state);
            refreshPianoRollTrackSoundLibraryDisplay(state);
            refreshTrackSoundLibraryDisplay();
            refreshAggregatePianoRollDirtyFlag();
            updatePianoRollWindowDirtyIndicator(state);
            if (state.trackIndex == getSelectedTrackIndex())
                syncTrackInspectorFromSelection();
            updateTrackSummary(*currentProject);
            refreshTrackManagerText();
            recordExternalTrackStateUpdate(state.trackIndex, true);
            setProjectDirty();
            logMessage("Undo Piano Roll instrument/library change for " + getTrackDisplayName(state.trackIndex) + ".");
            return true;
        }

        if (state.roll.canUndoNoteEdit())
        {
            state.roll.undoLastNoteEdit();
            logMessage("Undo piano roll note edit.");
            return true;
        }

        logMessage("Piano Roll Undo: nothing to undo.");
        return false;
    }

    bool MainComponent::redoPianoRollEditorAction(PianoRollEditorWindowState& state)
    {
        if (!currentProject || state.trackIndex < 0 || state.trackIndex >= static_cast<int>(currentProject->getTracks().size()))
            return false;

        auto& track = currentProject->getTracks()[static_cast<std::size_t>(state.trackIndex)];
        if (state.hasPendingInstrumentAssignment)
        {
            discardPendingPianoRollTrackSettings(state);
            logMessage("Redo discarded pending Piano Roll track settings before redo for " + getTrackDisplayName(state.trackIndex) + ".");
            return true;
        }

        if (!state.instrumentRedoStack.empty())
        {
            state.instrumentUndoStack.push_back(makeUndoSafeInstrumentAssignment(track.getInstrument()));
            auto next = state.instrumentRedoStack.back();
            state.instrumentRedoStack.pop_back();
            next = prepareInstrumentAssignmentForUndoRestore(track.getInstrument(), std::move(next));
            track.setInstrumentAssignment(next);
            populatePianoRollInstrumentCombo(state);
            refreshPianoRollTrackSoundLibraryDisplay(state);
            refreshTrackSoundLibraryDisplay();
            refreshAggregatePianoRollDirtyFlag();
            updatePianoRollWindowDirtyIndicator(state);
            if (state.trackIndex == getSelectedTrackIndex())
                syncTrackInspectorFromSelection();
            updateTrackSummary(*currentProject);
            refreshTrackManagerText();
            recordExternalTrackStateUpdate(state.trackIndex, true);
            setProjectDirty();
            logMessage("Redo Piano Roll instrument/library change for " + getTrackDisplayName(state.trackIndex) + ".");
            return true;
        }

        if (state.roll.canRedoNoteEdit())
        {
            state.roll.redoLastNoteEdit();
            logMessage("Redo piano roll note edit.");
            return true;
        }

        logMessage("Piano Roll Redo: nothing to redo.");
        return false;
    }

    void MainComponent::refreshOpenPianoRollInstrumentControls()
    {
        for (auto& entry : pianoRollEditorWindows)
        {
            if (entry.second != nullptr)
            {
                populatePianoRollInstrumentCombo(*entry.second);
                refreshPianoRollTrackSoundLibraryDisplay(*entry.second);
            }
        }
    }

    void MainComponent::refreshSfzList()
    {
        detectedSfzFiles.clear();
        sfzCombo.clear(juce::dontSendNotification);

        const auto folder = mw::app::AppPaths::sfzFolder();

        const auto isSfzFile = [](const std::filesystem::path& path)
        {
            auto ext = path.extension().string();

            for (auto& c : ext)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

            return ext == ".sfz";
        };

        const auto sidecarPackFolderFor = [](const std::filesystem::path& sfzPath)
        {
            if (sfzPath.empty())
                return std::filesystem::path();

            const auto candidate = sfzPath.parent_path() / sfzPath.stem();

            std::error_code ec;
            if (std::filesystem::exists(candidate, ec) && std::filesystem::is_directory(candidate, ec))
                return candidate;

            return std::filesystem::path();
        };

        std::set<std::filesystem::path> addedPaths;
        std::set<std::filesystem::path> sidecarFolders;

        const auto addSfzPath = [&](const std::filesystem::path& path)
        {
            const auto normalized = std::filesystem::absolute(path).lexically_normal();
            if (addedPaths.insert(normalized).second)
                detectedSfzFiles.push_back(path);
        };

        if (std::filesystem::exists(folder))
        {
            // First add the public, top-level SFZ options. When a top-level
            // Name.sfz has a sibling Name/ folder, that folder is treated as
            // the pack root for instrument folders/includes and is not expanded into a
            // long list of internal SFZ options.
            for (const auto& entry : std::filesystem::directory_iterator(folder))
            {
                if (!entry.is_regular_file() || !isSfzFile(entry.path()))
                    continue;

                addSfzPath(entry.path());

                const auto sidecarFolder = sidecarPackFolderFor(entry.path());
                if (!sidecarFolder.empty())
                    sidecarFolders.insert(std::filesystem::absolute(sidecarFolder).lexically_normal());
            }

            for (std::filesystem::recursive_directory_iterator it(folder), end; it != end; ++it)
            {
                if (it->is_directory())
                {
                    const auto normalizedDir = std::filesystem::absolute(it->path()).lexically_normal();
                    if (sidecarFolders.count(normalizedDir) > 0)
                    {
                        it.disable_recursion_pending();
                        continue;
                    }
                }

                if (it->is_regular_file() && isSfzFile(it->path()))
                    addSfzPath(it->path());
            }
        }

        const auto typedCurrentPath = std::filesystem::path(sfzPathBox.getText().toStdString());
        if (!typedCurrentPath.empty() && std::filesystem::exists(typedCurrentPath) && isSfzFile(typedCurrentPath))
            addSfzPath(typedCurrentPath);

        std::sort(
            detectedSfzFiles.begin(),
            detectedSfzFiles.end(),
            [folder](const auto& a, const auto& b)
            {
                std::error_code aError;
                std::error_code bError;
                auto aRelative = std::filesystem::relative(a, folder, aError);
                auto bRelative = std::filesystem::relative(b, folder, bError);

                if (aError)
                    aRelative = a;

                if (bError)
                    bRelative = b;

                return aRelative.string() < bRelative.string();
            }
        );

        for (int i = 0; i < static_cast<int>(detectedSfzFiles.size()); ++i)
        {
            const auto& path = detectedSfzFiles[static_cast<std::size_t>(i)];
            juce::String label(path.filename().string());

            const auto sidecarFolder = sidecarPackFolderFor(path);
            if (!sidecarFolder.empty())
                label << "  [pack: " << sidecarFolder.filename().string() << "]";
            else if (detectedSfzFiles.size() > 1 && path.parent_path() != folder)
                label = juce::String(path.parent_path().filename().string()) + " / " + label;

            sfzCombo.addItem(label, i + 1);
        }

        if (!detectedSfzFiles.empty())
        {
            int selectedIndex = 0;
            const auto currentPath = std::filesystem::path(sfzPathBox.getText().toStdString());
            const auto normalizedCurrentPath = currentPath.empty() ? std::filesystem::path() : std::filesystem::absolute(currentPath).lexically_normal();

            for (int i = 0; i < static_cast<int>(detectedSfzFiles.size()); ++i)
            {
                const auto normalizedDetectedPath = std::filesystem::absolute(detectedSfzFiles[static_cast<std::size_t>(i)]).lexically_normal();

                if (normalizedDetectedPath == normalizedCurrentPath)
                {
                    selectedIndex = i;
                    break;
                }
            }

            sfzCombo.setSelectedId(selectedIndex + 1, juce::sendNotification);
            sfzPathBox.setText(detectedSfzFiles[static_cast<std::size_t>(selectedIndex)].string(), juce::dontSendNotification);
            logMessage("SFZ library auto-scanned. Found " + juce::String(static_cast<int>(detectedSfzFiles.size())) + " public .sfz option(s).");

            const auto selectedSidecar = sidecarPackFolderFor(detectedSfzFiles[static_cast<std::size_t>(selectedIndex)]);
            if (!selectedSidecar.empty())
                logMessage("Selected SFZ pack root: " + selectedSidecar.string());
        }
        else
        {
            sfzPathBox.setText(folder.string(), juce::dontSendNotification);
            logMessage("SFZ library auto-scan found no .sfz files in: " + folder.string());
        }
    }

    std::filesystem::path MainComponent::getSelectedSfzPath() const
    {
        const int selectedId = sfzCombo.getSelectedId();

        if (selectedId > 0)
        {
            const auto index = static_cast<std::size_t>(selectedId - 1);

            if (index < detectedSfzFiles.size())
                return detectedSfzFiles[index];
        }

        return {};
    }


    void MainComponent::refreshPresetListFromSelectedSoundFont()
    {
        detectedPresets.clear();

        const auto soundFontPath =
            getSelectedSoundFontPath().empty()
                ? std::filesystem::path(soundFontPathBox.getText().toStdString())
                : getSelectedSoundFontPath();

        if (!soundFontPath.empty() && std::filesystem::exists(soundFontPath))
        {
            detectedPresets = mw::audio::SoundFontPresetReader::readPresets(soundFontPath);

            if (!detectedPresets.empty())
            {
                logMessage(
                    "Loaded "
                    + juce::String(static_cast<int>(detectedPresets.size()))
                    + " presets from SoundFont."
                );
            }
            else
            {
                logMessage("No presets found in selected SoundFont; using General MIDI fallback list.");
            }
        }

        populateInstrumentCombo();
        syncTrackInspectorFromSelection();
    }

    void MainComponent::populateInstrumentCombo()
    {
        instrumentCombo.clear(juce::dontSendNotification);

        std::filesystem::path selectedTrackLibraryPath;
        std::filesystem::path selectedTrackVstBundlePath;
        std::filesystem::path projectDefaultVstBundlePath;
        auto selectedTrackBackend = mw::core::SampleBackendType::None;
        bool selectedTrackIsAudioClip = false;

        if (currentProject)
        {
            projectDefaultVstBundlePath = currentProject->getUserSettings().vst3PluginPath;
            const auto index = getSelectedTrackIndex();
            if (index >= 0 && index < static_cast<int>(currentProject->getTracks().size()))
            {
                const auto& selectedTrack = currentProject->getTracks()[static_cast<std::size_t>(index)];
                const auto& assignment = selectedTrack.getInstrument();
                selectedTrackIsAudioClip = selectedTrack.isAudioClipTrack();
                selectedTrackBackend = assignment.backendType;
                selectedTrackLibraryPath = assignment.sampleLibraryPath;
                selectedTrackVstBundlePath = resolveVst3BundlePath(assignment);
            }
        }

        if (selectedTrackIsAudioClip)
        {
            instrumentCombo.addItem("Custom Audio (AudioClip)", 1);
            instrumentCombo.setSelectedId(1, juce::dontSendNotification);
            return;
        }

        const int visibleProjectBackendId = backendCombo.getSelectedId() > 0 ? backendCombo.getSelectedId() : appliedProjectBackendId;
        const int selectedTrackBackendChoice = trackBackendCombo.getSelectedId() > 0 ? trackBackendCombo.getSelectedId() : 1;

        const bool trackWantsVst =
            selectedTrackBackend == mw::core::SampleBackendType::VST3
            || selectedTrackBackendChoice == 4
            || (selectedTrackBackendChoice == 1 && visibleProjectBackendId == 3)
            || (selectedTrackBackend == mw::core::SampleBackendType::None && visibleProjectBackendId == 3);

        if (trackWantsVst)
        {
            if (detectedVstPlugins.empty())
                scanVstPlugins(false);

            int itemId = 1;
            int selectedId = 1;
            for (const auto& plugin : detectedVstPlugins)
            {
                if (!isSupportedVstInstrumentPlugin(plugin))
                    continue;

                juce::String label = plugin.displayName();
                if (!plugin.vendor.empty())
                    label << " - " << plugin.vendor;
                if (plugin.status == mw::vst::VstPluginScanStatus::Warning)
                    label << "  ⚠";

                instrumentCombo.addItem(label, itemId);
                if ((!selectedTrackVstBundlePath.empty() && plugin.bundlePath == selectedTrackVstBundlePath)
                    || (selectedTrackVstBundlePath.empty() && !projectDefaultVstBundlePath.empty() && plugin.bundlePath == projectDefaultVstBundlePath))
                    selectedId = itemId;
                ++itemId;
            }

            if (instrumentCombo.getNumItems() == 0)
                instrumentCombo.addItem("No VST3 instruments found - use VST Plugins > Scan", 1);

            instrumentCombo.setSelectedId(selectedId, juce::dontSendNotification);
            return;
        }

        const bool trackWantsSfz =
            selectedTrackBackend == mw::core::SampleBackendType::SFZ
            || (selectedTrackBackend == mw::core::SampleBackendType::None
                && (selectedTrackBackendChoice == 3
                    || (selectedTrackBackendChoice == 1 && visibleProjectBackendId == 2)));

        if (trackWantsSfz)
        {
            const auto selectedSfzPath = getSelectedSfzPath();
            std::filesystem::path sfzPath =
                !selectedTrackLibraryPath.empty()
                    ? selectedTrackLibraryPath
                    : (!selectedSfzPath.empty()
                        ? selectedSfzPath
                        : std::filesystem::path(sfzPathBox.getText().toStdString()));

            const auto label = sfzPath.empty()
                ? juce::String("Selected SFZ Instrument")
                : juce::String(sfzPath.filename().string());

            instrumentCombo.addItem(label, 1);
            instrumentCombo.setSelectedId(1, juce::dontSendNotification);
            return;
        }

        if (selectedTrackBackend == mw::core::SampleBackendType::SF2
            && !selectedTrackLibraryPath.empty()
            && std::filesystem::exists(selectedTrackLibraryPath))
        {
            const auto trackPresets = mw::audio::SoundFontPresetReader::readPresets(selectedTrackLibraryPath);
            if (!trackPresets.empty())
                detectedPresets = trackPresets;
        }

        if (!detectedPresets.empty())
        {
            for (int i = 0; i < static_cast<int>(detectedPresets.size()); ++i)
            {
                const auto& preset = detectedPresets[static_cast<std::size_t>(i)];

                juce::String label;
                label << "Bank "
                      << preset.bank
                      << " / Program "
                      << preset.program
                      << " - "
                      << preset.name;

                instrumentCombo.addItem(label, i + 1);
            }

            instrumentCombo.setSelectedId(1, juce::dontSendNotification);
            return;
        }

        const auto& instruments = gmInstruments();

        for (int i = 0; i < static_cast<int>(instruments.size()); ++i)
        {
            juce::String label;
            label << instruments[static_cast<std::size_t>(i)].name
                  << " (GM "
                  << instruments[static_cast<std::size_t>(i)].program
                  << ")";

            instrumentCombo.addItem(label, i + 1);
        }

        instrumentCombo.setSelectedId(1, juce::dontSendNotification);
    }

        bool MainComponent::enforceProjectTrackLimit(mw::core::Project& project, const juce::String& sourceLabel)
    {
        const auto trackCount = static_cast<int>(project.getTracks().size());

        if (trackCount <= mw::core::Project::maxTrackCount)
            return true;

        juce::String message;
        message << sourceLabel
                << " contains "
                << trackCount
                << " tracks.\n\n"
                << "Poor Man's Studio supports up to "
                << mw::core::Project::maxTrackCount
                << " tracks.\n\n"
                << "Import/open the first "
                << mw::core::Project::maxTrackCount
                << " tracks and skip the rest?";

        logMessage(message);
        logMessage("Track limit applied automatically: keeping first " + juce::String(mw::core::Project::maxTrackCount) + " tracks.");

        project.getTracks().erase(
            project.getTracks().begin() + mw::core::Project::maxTrackCount,
            project.getTracks().end()
        );

        juce::String log;
        log << "Track limit applied to "
            << sourceLabel
            << ": kept first "
            << mw::core::Project::maxTrackCount
            << " tracks and skipped "
            << (trackCount - mw::core::Project::maxTrackCount)
            << ".";

        logMessage(log);
        return true;
    }

    bool MainComponent::canAddAnotherTrack(const juce::String& actionLabel)
    {
        if (!currentProject)
            return true;

        const auto trackCount = static_cast<int>(currentProject->getTracks().size());

        if (trackCount < mw::core::Project::maxTrackCount)
            return true;

        juce::String message;
        message << "Maximum track count reached: "
                << mw::core::Project::maxTrackCount
                << ". "
                << actionLabel
                << " was not applied.";

        logMessage(message);
        return false;
    }

void MainComponent::refreshTrackSelector()
    {
        trackCombo.clear(juce::dontSendNotification);

        if (!currentProject)
        {
            refreshMainSequenceSelector();
            updateRenderTargetLabel();
            return;
        }

        const auto& tracks = currentProject->getTracks();

        for (int i = 0; i < static_cast<int>(tracks.size()); ++i)
        {
            juce::String label;
            label << "#" << (i + 1) << " - " << tracks[static_cast<std::size_t>(i)].getName()
                  << (tracks[static_cast<std::size_t>(i)].isAudioClipTrack() ? " [AudioClip]" : " [MIDI]");
            trackCombo.addItem(label, i + 1);
        }

        if (!tracks.empty())
            trackCombo.setSelectedId(1, juce::dontSendNotification);

        refreshMainSequenceSelector();
        refreshActiveSequenceThoughtsEditor();
        updateRenderTargetLabel();
    }

    int MainComponent::getSelectedTrackIndex() const
    {
        const int selectedId = trackCombo.getSelectedId();

        if (selectedId <= 0)
            return -1;

        return selectedId - 1;
    }

    juce::String MainComponent::getSelectedTrackDisplayName() const
    {
        if (!currentProject)
            return "No Track";

        const auto index = getSelectedTrackIndex();

        if (index < 0 || index >= static_cast<int>(currentProject->getTracks().size()))
            return "No Track";

        juce::String label;
        label << "#" << (index + 1) << " - " << currentProject->getTracks()[static_cast<std::size_t>(index)].getName()
              << (currentProject->getTracks()[static_cast<std::size_t>(index)].isAudioClipTrack() ? " [AudioClip]" : " [MIDI]");
        return label;
    }

    juce::String MainComponent::getTrackDisplayName(int trackIndex) const
    {
        if (!currentProject)
            return "No Track";

        if (trackIndex < 0 || trackIndex >= static_cast<int>(currentProject->getTracks().size()))
            return "No Track";

        const auto& track = currentProject->getTracks()[static_cast<std::size_t>(trackIndex)];

        juce::String label;
        label << "#" << (trackIndex + 1) << " - " << track.getName()
              << (track.isAudioClipTrack() ? " [AudioClip]" : " [MIDI]");
        return label;
    }

    void MainComponent::syncTrackInspectorFromSelection()
    {
        if (!currentProject)
            return;

        const auto index = getSelectedTrackIndex();

        if (index < 0 || index >= static_cast<int>(currentProject->getTracks().size()))
            return;

        auto& track = currentProject->getTracks()[static_cast<std::size_t>(index)];

        if (track.isAudioClipTrack())
        {
            track.setInstrumentAssignment(mw::core::makeCustomAudioInstrumentAssignment());
            populateInstrumentCombo();
            instrumentCombo.setSelectedId(1, juce::dontSendNotification);
            trackBackendCombo.setSelectedId(1, juce::dontSendNotification);
            muteToggle.setToggleState(track.getMuted(), juce::dontSendNotification);
            soloToggle.setToggleState(track.getSolo(), juce::dontSendNotification);
            trackVolumeSlider.setValue(track.getMixerSettings().volume, juce::dontSendNotification);
            updateVolumeLabels();
            refreshTrackSoundLibraryDisplay();
            refreshNoteEditor();
            updateTrackSummary(*currentProject);
            return;
        }

        auto assignment = track.getInstrument();
        if (repairVst3BundlePathIfPossible(assignment))
            track.setInstrumentAssignment(assignment);

        if (track.getInstrument().backendType == mw::core::SampleBackendType::None
            || (track.getInstrument().backendType == mw::core::SampleBackendType::VST3 && !hasResolvableVst3BundlePath(track.getInstrument()))
            || (track.getInstrument().backendType != mw::core::SampleBackendType::VST3 && track.getInstrument().sampleLibraryPath.empty()))
            seedTrackSoundLibraryFromProjectDefaults(track);

        const auto& trackAssignment = track.getInstrument();
        const int backendComboId = trackAssignment.backendType == mw::core::SampleBackendType::VST3
            ? 4
            : (trackAssignment.backendType == mw::core::SampleBackendType::SFZ ? 3 : 2);
        trackBackendCombo.setSelectedId(backendComboId, juce::dontSendNotification);

        if (trackAssignment.backendType == mw::core::SampleBackendType::VST3)
        {
            // populateInstrumentCombo() already selects the VST plugin matching the
            // selected track's bundle path. Do not force item #1 here; that made a
            // newly added blank track appear to reset the selected plugin even while
            // the backend/library still showed VST3 correctly.
            populateInstrumentCombo();
        }
        else if (trackAssignment.backendType == mw::core::SampleBackendType::SFZ)
        {
            instrumentCombo.setSelectedId(1, juce::dontSendNotification);
        }
        else if (!detectedPresets.empty())
        {
            int presetIndex = 0;

            for (int i = 0; i < static_cast<int>(detectedPresets.size()); ++i)
            {
                const auto& preset = detectedPresets[static_cast<std::size_t>(i)];

                if (preset.bank == track.getInstrument().midiBank && preset.program == track.getInstrument().midiProgram)
                {
                    presetIndex = i;
                    break;
                }
            }

            instrumentCombo.setSelectedId(presetIndex + 1, juce::dontSendNotification);
        }
        else
        {
            const int gmIndex = findInstrumentIndexForAssignment(track.getInstrument());
            instrumentCombo.setSelectedId(gmIndex + 1, juce::dontSendNotification);
        }

        muteToggle.setToggleState(track.getMuted(), juce::dontSendNotification);
        soloToggle.setToggleState(track.getSolo(), juce::dontSendNotification);
        trackVolumeSlider.setValue(track.getMixerSettings().volume, juce::dontSendNotification);
        updateVolumeLabels();
        refreshTrackSoundLibraryDisplay();
        refreshNoteEditor();
        updateTrackSummary(*currentProject);
    }

    void MainComponent::applyTrackInspector()
    {
        if (!currentProject)
            return;

        const auto index = getSelectedTrackIndex();

        if (index < 0 || index >= static_cast<int>(currentProject->getTracks().size()))
            return;

        auto& track = currentProject->getTracks()[static_cast<std::size_t>(index)];

        if (track.isAudioClipTrack())
        {
            track.setInstrumentAssignment(mw::core::makeCustomAudioInstrumentAssignment());
            track.setMuted(muteToggle.getToggleState());
            track.setSolo(soloToggle.getToggleState());
            track.getMixerSettings().volume = static_cast<float>(trackVolumeSlider.getValue());
            refreshTrackSoundLibraryDisplay();
            updateTrackSummary(*currentProject);
            refreshTrackManagerText();
            refreshOpenPianoRollInstrumentControls();
            setProjectDirty();
            logMessage("Applied AudioClip track settings. Custom Audio is fixed for AudioClip tracks.");
            return;
        }

        // Applying track settings may replace or invalidate the plugin instance
        // used by an open VST editor. Close it up front instead of waiting until
        // after assignment comparison, because the combo boxes can point at a
        // new plugin even while the old editor is still attached to the track.
        closeVstPluginWindowForTrack(index,
            "Closed open VST plugin window before applying track settings.");

        auto assignment = track.getInstrument();
        if (repairVst3BundlePathIfPossible(assignment))
            track.setInstrumentAssignment(assignment);

        if (track.getInstrument().backendType == mw::core::SampleBackendType::None
            || (track.getInstrument().backendType == mw::core::SampleBackendType::VST3 && !hasResolvableVst3BundlePath(track.getInstrument()))
            || (track.getInstrument().backendType != mw::core::SampleBackendType::VST3 && track.getInstrument().sampleLibraryPath.empty()))
            seedTrackSoundLibraryFromProjectDefaults(track);

        const auto instrumentBeforeApply = track.getInstrument();

        const auto instrumentIndex = instrumentCombo.getSelectedId() - 1;
        const int requestedTrackBackendId = trackBackendCombo.getSelectedId() > 0
            ? trackBackendCombo.getSelectedId()
            : (track.getInstrument().backendType == mw::core::SampleBackendType::VST3
                ? 4
                : (track.getInstrument().backendType == mw::core::SampleBackendType::SFZ ? 3 : 2));
        const bool trackWantsVst = requestedTrackBackendId == 4
            || (requestedTrackBackendId == 1 && appliedProjectBackendId == 3);

        if (trackWantsVst)
        {
            if (detectedVstPlugins.empty())
                scanVstPlugins(false);

            std::vector<mw::vst::VstPluginDescriptor> instrumentPlugins;
            for (const auto& plugin : detectedVstPlugins)
            {
                if (isSupportedVstInstrumentPlugin(plugin))
                    instrumentPlugins.push_back(plugin);
            }

            if (instrumentIndex >= 0 && instrumentIndex < static_cast<int>(instrumentPlugins.size()))
                assignSelectedTrackVstPlugin(instrumentPlugins[static_cast<std::size_t>(instrumentIndex)]);
            else
                logMessage("Apply Track Settings: no VST3 instrument selected.");

            return;
        }

        const bool trackWantsSfz = requestedTrackBackendId == 3
            || (requestedTrackBackendId == 1 && appliedProjectBackendId == 2);

        if (trackWantsSfz)
        {
            auto assignment = track.getInstrument();
            const auto sfzPath = !track.getInstrument().sampleLibraryPath.empty()
                ? track.getInstrument().sampleLibraryPath
                : getProjectDefaultLibraryPath(mw::core::SampleBackendType::SFZ);

            const auto sfzName = sfzPath.empty()
                ? std::string("SFZ Instrument")
                : sfzPath.stem().string();

            assignment.displayName = sfzName;
            assignment.normalizedName = sfzName;
            assignment.presetName = sfzName;
            assignment.backendType = mw::core::SampleBackendType::SFZ;
            assignment.sampleLibraryPath = sfzPath;
            assignment.sampleLibraryDisplayName = sfzPath.empty() ? std::string() : sfzPath.filename().string();
            assignment.vst3 = {};

            track.setInstrumentAssignment(assignment);
        }
        else if (!detectedPresets.empty())
        {
            if (instrumentIndex >= 0 && instrumentIndex < static_cast<int>(detectedPresets.size()))
            {
                const auto& selected = detectedPresets[static_cast<std::size_t>(instrumentIndex)];
                auto assignment = track.getInstrument();

                assignment.displayName = selected.name;
                assignment.normalizedName = selected.name;
                assignment.presetName = selected.name;
                assignment.midiBank = selected.bank;
                assignment.midiProgram = selected.program;
                assignment.backendType = mw::core::SampleBackendType::SF2;
                assignment.sampleLibraryPath =
                    (!track.getInstrument().sampleLibraryPath.empty() && track.getInstrument().backendType == mw::core::SampleBackendType::SF2)
                        ? track.getInstrument().sampleLibraryPath
                        : getProjectDefaultLibraryPath(mw::core::SampleBackendType::SF2);
                assignment.sampleLibraryDisplayName = assignment.sampleLibraryPath.empty() ? std::string() : assignment.sampleLibraryPath.filename().string();
                assignment.vst3 = {};

                track.setInstrumentAssignment(assignment);
            }
        }
        else if (instrumentIndex >= 0 && instrumentIndex < static_cast<int>(gmInstruments().size()))
        {
            const auto& selected = gmInstruments()[static_cast<std::size_t>(instrumentIndex)];
            auto assignment = track.getInstrument();

            assignment.displayName = selected.name;
            assignment.normalizedName = selected.normalizedName;
            assignment.presetName = selected.name;
            assignment.midiProgram = selected.program;
            assignment.backendType = mw::core::SampleBackendType::SF2;
            assignment.sampleLibraryPath =
                (!track.getInstrument().sampleLibraryPath.empty() && track.getInstrument().backendType == mw::core::SampleBackendType::SF2)
                    ? track.getInstrument().sampleLibraryPath
                    : getProjectDefaultLibraryPath(mw::core::SampleBackendType::SF2);
            assignment.sampleLibraryDisplayName = assignment.sampleLibraryPath.empty() ? std::string() : assignment.sampleLibraryPath.filename().string();
            assignment.vst3 = {};

            track.setInstrumentAssignment(assignment);
        }

        track.setMuted(muteToggle.getToggleState());
        track.setSolo(soloToggle.getToggleState());
        track.getMixerSettings().volume = static_cast<float>(trackVolumeSlider.getValue());

        const auto instrumentAfterApply = track.getInstrument();
        if (!instrumentAssignmentsEqual(instrumentBeforeApply, instrumentAfterApply)
            && (instrumentBeforeApply.backendType == mw::core::SampleBackendType::VST3
                || instrumentAfterApply.backendType == mw::core::SampleBackendType::VST3))
        {
            closeVstPluginWindowForTrack(index,
                "Closed open VST plugin window after applying changed track settings.");
        }

        refreshTrackSoundLibraryDisplay();
        updateTrackSummary(*currentProject);
        refreshTrackManagerText();

        if (trackManagerContent != nullptr)
        {
            if (auto* content = dynamic_cast<TrackManagerWindowContent*>(trackManagerContent.get()))
                content->refreshSequenceMap();
        }

        recordExternalTrackStateUpdate(index);
        refreshOpenPianoRollInstrumentControls();
        setProjectDirty();
        logMessage("Applied track settings: " + track.getName());
    }

    

    int MainComponent::getSelectedTrackEndBeat() const
    {
        return getTrackEndBeat(getSelectedTrackIndex());
    }

    int MainComponent::getTrackEndBeat(int trackIndex) const
    {
        if (!currentProject)
            return 0;

        if (trackIndex < 0 || trackIndex >= static_cast<int>(currentProject->getTracks().size()))
            return 0;

        const auto& track = currentProject->getTracks()[static_cast<std::size_t>(trackIndex)];

        std::int64_t endTick = 0;

        for (const auto& note : track.getNotes())
            endTick = std::max(endTick, note.startTick + note.durationTicks);

        return static_cast<int>(
            std::ceil(
                static_cast<double>(endTick)
                / static_cast<double>(mw::core::Project::ticksPerQuarterNote)
            )
        );
    }

    int MainComponent::getCurrentPianoRollBeatWindow() const
    {
        int beatWindow = 16;

        try { beatWindow = std::stoi(pianoRollBeatWindowBox.getText().toStdString()); } catch (...) {}

        if (beatWindow != 4 && beatWindow != 8 && beatWindow != 16 && beatWindow != 32 && beatWindow != 64)
            beatWindow = 16;

        return beatWindow;
    }

    int MainComponent::getCurrentPianoRollStartBeat() const
    {
        int startBeat = 0;

        try { startBeat = std::stoi(pianoRollStartBeatBox.getText().toStdString()); } catch (...) {}

        return std::max(0, startBeat);
    }

    int MainComponent::getCurrentPianoRollTotalPages() const
    {
        const auto beatWindow = getCurrentPianoRollBeatWindow();
        const auto totalBeats = std::max(beatWindow, getSelectedTrackEndBeat());

        return std::max(1, static_cast<int>(std::ceil(static_cast<double>(totalBeats) / static_cast<double>(beatWindow))));
    }

    void MainComponent::updatePianoRollPageIndicator()
    {
        const auto beatWindow = getCurrentPianoRollBeatWindow();
        const auto startBeat = getCurrentPianoRollStartBeat();
        const auto totalPages = getCurrentPianoRollTotalPages();
        const auto currentPage = std::clamp((startBeat / beatWindow) + 1, 1, totalPages);
        const auto endBeat = startBeat + beatWindow;

        juce::String label;
        label << "Window " << currentPage << " / " << totalPages << " | Beats " << startBeat << "-" << endBeat;

        pianoRollPageLabel.setText(label, juce::dontSendNotification);
        pianoRollPageBox.setText(juce::String(currentPage), juce::dontSendNotification);
    }

    void MainComponent::previousPianoRollWindow()
    {
        const auto beatWindow = getCurrentPianoRollBeatWindow();
        const auto startBeat = std::max(0, getCurrentPianoRollStartBeat() - beatWindow);

        pianoRollStartBeatBox.setText(juce::String(startBeat), juce::dontSendNotification);
        applyPianoRollSettings();
    }

    void MainComponent::nextPianoRollWindow()
    {
        const auto beatWindow = getCurrentPianoRollBeatWindow();
        const auto startBeat = getCurrentPianoRollStartBeat() + beatWindow;

        pianoRollStartBeatBox.setText(juce::String(startBeat), juce::dontSendNotification);
        applyPianoRollSettings();
    }

    void MainComponent::jumpToPianoRollWindow()
    {
        auto windowNumber = 1;

        try { windowNumber = std::stoi(pianoRollPageBox.getText().toStdString()); } catch (...) {}

        const auto totalWindows = getCurrentPianoRollTotalPages();
        windowNumber = std::clamp(windowNumber, 1, totalWindows);

        const auto beatWindow = getCurrentPianoRollBeatWindow();
        const auto startBeat = (windowNumber - 1) * beatWindow;

        pianoRollStartBeatBox.setText(juce::String(startBeat), juce::dontSendNotification);
        applyPianoRollSettings();
    }

    void MainComponent::updatePianoRollKeyRangeLabel()
    {
        pianoRollKeyRangeLabel.setText("Visible Notes: " + pianoRoll.getVisiblePitchRangeText(), juce::dontSendNotification);
    }

    void MainComponent::jumpToPianoRollKey()
    {
        if (pianoRoll.jumpToKey(pianoRollKeyJumpBox.getText()))
        {
            updatePianoRollKeyRangeLabel();
            logMessage("Piano Roll key jump: " + pianoRoll.getVisiblePitchRangeText());
        }
        else
        {
            logMessage("Piano Roll key jump ignored. Use values like C, D, F#, Bb, C4, A0, or 60.");
        }
    }


    void MainComponent::applyPianoRollSettings()
    {
        if (currentProject)
        {
            try
            {
                const int bpm = std::stoi(pianoRollBpmBox.getText().toStdString());
                if (bpm > 0 && bpm <= 400)
                {
                    currentProject->setTempoBpm(bpm);
                    tempoBox.setText(juce::String(bpm), juce::dontSendNotification);
                }
            }
            catch (...)
            {
                logMessage("Invalid piano roll BPM.");
            }

            const auto ts = pianoRollTimeSigBox.getText().toStdString();
            const auto slash = ts.find('/');

            if (slash != std::string::npos)
            {
                try
                {
                    const int numerator = std::stoi(ts.substr(0, slash));
                    const int denominator = std::stoi(ts.substr(slash + 1));

                    if (numerator > 0 && denominator > 0)
                    {
                        currentProject->setTimeSignature({ numerator, denominator });
                        timeSignatureBox.setText(pianoRollTimeSigBox.getText(), juce::dontSendNotification);
                    }
                }
                catch (...)
                {
                    logMessage("Invalid piano roll time signature.");
                }
            }
        }

        int beatWindow = 16;

        try
        {
            beatWindow = std::stoi(pianoRollBeatWindowBox.getText().toStdString());
        }
        catch (...)
        {
            beatWindow = 16;
        }

        if (beatWindow != 4 && beatWindow != 8 && beatWindow != 16 && beatWindow != 32 && beatWindow != 64)
        {
            logMessage("Beat Window must be 4, 8, 16, 32, or 64. Using 16.");
            beatWindow = 16;
        }

        pianoRollBeatWindowBox.setText(juce::String(beatWindow), juce::dontSendNotification);
        updateBeatWindowButtonHighlights(beatWindow);

        int velocity = 100;

        try { velocity = std::stoi(pianoRollVelocityBox.getText().toStdString()); } catch (...) {}

        const auto startBeat = getCurrentPianoRollStartBeat();

        auto snapDivision = parseSnapDivisionText(pianoRollSnapBox.getText());

        if (!isAllowedSnapDivision(snapDivision))
            snapDivision = 1;

        const auto snapLengthBeats = 1.0 / static_cast<double>(snapDivision);
        const auto noteLengthText = pianoRollNoteLengthBox.getText().trim();
        auto noteLength = parseBeatLengthText(noteLengthText, snapLengthBeats);

        if (snapDivision > 1 && (noteLengthText.isEmpty() || noteLengthText == "1" || noteLengthText == "1.0"))
            noteLength = snapLengthBeats;

        noteLength = std::max(1.0 / 64.0, noteLength);
        pianoRollSnapBox.setText(beatDivisionText(snapDivision), juce::dontSendNotification);
        pianoRollNoteLengthBox.setText(beatLengthDisplayText(noteLength), juce::dontSendNotification);

        if (currentProject)
            pianoRoll.setTempoBpm(currentProject->getTempoBpm());

        pianoRoll.setGrid(beatWindow, pianoRoll.getLowPitch(), pianoRoll.getHighPitch());
        updatePianoRollKeyRangeLabel();
        pianoRoll.setStartBeat(startBeat);
        pianoRoll.setSnapDivision(snapDivision);
        pianoRoll.setDefaultNoteSettings(noteLength, velocity);
        updatePianoRollPageIndicator();
        pianoRoll.repaint();

        if (currentProject)
            updateTrackSummary(*currentProject);

        logMessage("Applied piano roll settings. Beat Window: " + juce::String(beatWindow) + ", Snap: " + beatDivisionText(snapDivision) + ", Note Len: " + beatLengthDisplayText(noteLength));
    }

    void MainComponent::updateBeatWindowButtonHighlights(int beatWindow)
    {
        auto resetButton = [](juce::TextButton& button)
        {
            button.setColour(juce::TextButton::buttonColourId, juce::Colours::darkgrey);
            button.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
        };

        auto selectButton = [](juce::TextButton& button)
        {
            button.setColour(juce::TextButton::buttonColourId, juce::Colours::yellow);
            button.setColour(juce::TextButton::textColourOffId, juce::Colours::black);
        };

        resetButton(setBeatWindow4Button);
        resetButton(setBeatWindow8Button);
        resetButton(setBeatWindow16Button);
        resetButton(setBeatWindow32Button);
        resetButton(setBeatWindow64Button);

        if (beatWindow == 4) selectButton(setBeatWindow4Button);
        else if (beatWindow == 8) selectButton(setBeatWindow8Button);
        else if (beatWindow == 16) selectButton(setBeatWindow16Button);
        else if (beatWindow == 32) selectButton(setBeatWindow32Button);
        else if (beatWindow == 64) selectButton(setBeatWindow64Button);
    }

    void MainComponent::fitPianoRollToSelectedTrack()
    {
        if (!currentProject)
            return;

        const auto index = getSelectedTrackIndex();

        if (index < 0 || index >= static_cast<int>(currentProject->getTracks().size()))
            return;

        const auto& track = currentProject->getTracks()[static_cast<std::size_t>(index)];

        if (track.getNotes().empty())
            return;

        std::int64_t endTick = 0;

        for (const auto& note : track.getNotes())
            endTick = std::max(endTick, note.startTick + note.durationTicks);

        const auto endBeat =
            static_cast<int>(
                std::ceil(
                    static_cast<double>(endTick)
                    / static_cast<double>(mw::core::Project::ticksPerQuarterNote)
                )
            );

        int beatWindow = 16;

        if (endBeat <= 4) beatWindow = 4;
        else if (endBeat <= 8) beatWindow = 8;
        else if (endBeat <= 16) beatWindow = 16;
        else if (endBeat <= 32) beatWindow = 32;
        else beatWindow = 64;

        pianoRollBeatWindowBox.setText(juce::String(beatWindow), juce::dontSendNotification);
        updateBeatWindowButtonHighlights(beatWindow);
        pianoRollStartBeatBox.setText("0", juce::dontSendNotification);
        if (currentProject)
            pianoRoll.setTempoBpm(currentProject->getTempoBpm());
        pianoRoll.setGrid(beatWindow, pianoRoll.getLowPitch(), pianoRoll.getHighPitch());
        updatePianoRollKeyRangeLabel();
        pianoRoll.setStartBeat(0);
        updatePianoRollPageIndicator();

        logMessage("Piano Roll fitted to selected track. Beat Window: " + juce::String(beatWindow));
    }

    MainComponent::PianoRollEditorWindowState* MainComponent::findPianoRollEditorWindow(int trackIndex) const
    {
        const auto it = pianoRollEditorWindows.find(trackIndex);
        if (it == pianoRollEditorWindows.end() || it->second == nullptr)
            return nullptr;
        return it->second.get();
    }

    void MainComponent::refreshAggregatePianoRollDirtyFlag()
    {
        bool anyDirty = false;

        for (const auto& entry : pianoRollEditorWindows)
        {
            if (entry.second != nullptr && (entry.second->dirty || entry.second->hasPendingInstrumentAssignment))
            {
                anyDirty = true;
                break;
            }
        }

        pianoRollEditorDirty = anyDirty;
    }

    int MainComponent::getPianoRollBeatWindow(const PianoRollEditorWindowState& state) const
    {
        int beatWindow = 16;
        try { beatWindow = std::stoi(state.beatWindowBox.getText().toStdString()); } catch (...) {}

        if (beatWindow != 4 && beatWindow != 8 && beatWindow != 16 && beatWindow != 32 && beatWindow != 64)
            beatWindow = 16;

        return beatWindow;
    }

    int MainComponent::getPianoRollStartBeat(const PianoRollEditorWindowState& state) const
    {
        int startBeat = 0;
        try { startBeat = std::stoi(state.startBeatBox.getText().toStdString()); } catch (...) {}
        return std::max(0, startBeat);
    }

    int MainComponent::getPianoRollTotalPages(const PianoRollEditorWindowState& state) const
    {
        const auto beatWindow = getPianoRollBeatWindow(state);
        std::int64_t endTick = 0;

        for (const auto& note : state.roll.getNotes())
            endTick = std::max(endTick, note.startTick + note.durationTicks);

        const auto localEndBeat = static_cast<int>(
            std::ceil(
                static_cast<double>(endTick)
                / static_cast<double>(mw::core::Project::ticksPerQuarterNote)
            )
        );

        const auto totalBeats = std::max(beatWindow, std::max(localEndBeat, getTrackEndBeat(state.trackIndex)));
        return std::max(1, static_cast<int>(std::ceil(static_cast<double>(totalBeats) / static_cast<double>(beatWindow))));
    }

    void MainComponent::updatePianoRollPageIndicator(PianoRollEditorWindowState& state)
    {
        const auto beatWindow = getPianoRollBeatWindow(state);
        const auto startBeat = getPianoRollStartBeat(state);
        const auto totalPages = getPianoRollTotalPages(state);
        const auto currentPage = std::clamp((startBeat / beatWindow) + 1, 1, totalPages);
        const auto endBeat = startBeat + beatWindow;

        juce::String label;
        label << "Window " << currentPage << " / " << totalPages << " | Beats " << startBeat << "-" << endBeat;

        state.pageLabel.setText(label, juce::dontSendNotification);
        state.pageBox.setText(juce::String(currentPage), juce::dontSendNotification);
    }

    void MainComponent::updatePianoRollKeyRangeLabel(PianoRollEditorWindowState& state)
    {
        state.keyRangeLabel.setText("Visible Notes: " + state.roll.getVisiblePitchRangeText(), juce::dontSendNotification);
    }

    void MainComponent::updateBeatWindowButtonHighlights(PianoRollEditorWindowState& state, int beatWindow)
    {
        auto resetButton = [](juce::TextButton& button)
        {
            button.setColour(juce::TextButton::buttonColourId, juce::Colours::darkgrey);
            button.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
        };

        auto selectButton = [](juce::TextButton& button)
        {
            button.setColour(juce::TextButton::buttonColourId, juce::Colours::yellow);
            button.setColour(juce::TextButton::textColourOffId, juce::Colours::black);
        };

        resetButton(state.beat4Button);
        resetButton(state.beat8Button);
        resetButton(state.beat16Button);
        resetButton(state.beat32Button);
        resetButton(state.beat64Button);

        if (beatWindow == 4) selectButton(state.beat4Button);
        else if (beatWindow == 8) selectButton(state.beat8Button);
        else if (beatWindow == 16) selectButton(state.beat16Button);
        else if (beatWindow == 32) selectButton(state.beat32Button);
        else if (beatWindow == 64) selectButton(state.beat64Button);
    }

    void MainComponent::applyPianoRollSettings(PianoRollEditorWindowState& state, bool commitTrackSettings)
    {
        if (currentProject)
        {
            try
            {
                const int bpm = std::stoi(state.bpmBox.getText().toStdString());
                if (bpm > 0 && bpm <= 400)
                {
                    currentProject->setTempoBpm(bpm);
                    tempoBox.setText(juce::String(bpm), juce::dontSendNotification);
                    pianoRollBpmBox.setText(juce::String(bpm), juce::dontSendNotification);
                }
            }
            catch (...)
            {
                logMessage("Invalid piano roll BPM.");
            }

            const auto ts = state.timeSigBox.getText().toStdString();
            const auto slash = ts.find('/');

            if (slash != std::string::npos)
            {
                try
                {
                    const int numerator = std::stoi(ts.substr(0, slash));
                    const int denominator = std::stoi(ts.substr(slash + 1));

                    if (numerator > 0 && denominator > 0)
                    {
                        currentProject->setTimeSignature({ numerator, denominator });
                        timeSignatureBox.setText(state.timeSigBox.getText(), juce::dontSendNotification);
                        pianoRollTimeSigBox.setText(state.timeSigBox.getText(), juce::dontSendNotification);
                    }
                }
                catch (...)
                {
                    logMessage("Invalid piano roll time signature.");
                }
            }
        }

        int beatWindow = 16;
        try { beatWindow = std::stoi(state.beatWindowBox.getText().toStdString()); } catch (...) { beatWindow = 16; }

        if (beatWindow != 4 && beatWindow != 8 && beatWindow != 16 && beatWindow != 32 && beatWindow != 64)
        {
            logMessage("Beat Window must be 4, 8, 16, 32, or 64. Using 16.");
            beatWindow = 16;
        }

        state.beatWindowBox.setText(juce::String(beatWindow), juce::dontSendNotification);
        updateBeatWindowButtonHighlights(state, beatWindow);

        int velocity = 100;
        try { velocity = std::stoi(state.velocityBox.getText().toStdString()); } catch (...) {}

        const auto startBeat = getPianoRollStartBeat(state);

        auto snapDivision = parseSnapDivisionText(state.snapBox.getText());
        if (!isAllowedSnapDivision(snapDivision))
            snapDivision = 1;

        const auto snapLengthBeats = 1.0 / static_cast<double>(snapDivision);
        const auto noteLengthText = state.noteLengthBox.getText().trim();
        auto noteLength = parseBeatLengthText(noteLengthText, snapLengthBeats);

        if (snapDivision > 1 && (noteLengthText.isEmpty() || noteLengthText == "1" || noteLengthText == "1.0"))
            noteLength = snapLengthBeats;

        noteLength = std::max(1.0 / 64.0, noteLength);
        state.snapBox.setText(beatDivisionText(snapDivision), juce::dontSendNotification);
        state.noteLengthBox.setText(beatLengthDisplayText(noteLength), juce::dontSendNotification);

        if (currentProject)
            state.roll.setTempoBpm(currentProject->getTempoBpm());

        state.roll.setGrid(beatWindow, state.roll.getLowPitch(), state.roll.getHighPitch());
        updatePianoRollKeyRangeLabel(state);
        state.roll.setStartBeat(startBeat);
        state.roll.setSnapDivision(snapDivision);
        state.roll.setDefaultNoteSettings(noteLength, velocity);
        updatePianoRollPageIndicator(state);
        state.roll.repaint();

        if (commitTrackSettings)
            applyPendingPianoRollTrackSettings(state);

        if (currentProject)
            updateTrackSummary(*currentProject);
    }

    void MainComponent::fitPianoRollToTrack(PianoRollEditorWindowState& state)
    {
        if (!currentProject)
            return;

        if (state.trackIndex < 0 || state.trackIndex >= static_cast<int>(currentProject->getTracks().size()))
            return;

        const auto& track = currentProject->getTracks()[static_cast<std::size_t>(state.trackIndex)];

        if (track.getNotes().empty())
        {
            updatePianoRollPageIndicator(state);
            updatePianoRollKeyRangeLabel(state);
            return;
        }

        const auto endBeat = getTrackEndBeat(state.trackIndex);
        int beatWindow = 16;

        if (endBeat <= 4) beatWindow = 4;
        else if (endBeat <= 8) beatWindow = 8;
        else if (endBeat <= 16) beatWindow = 16;
        else if (endBeat <= 32) beatWindow = 32;
        else beatWindow = 64;

        state.beatWindowBox.setText(juce::String(beatWindow), juce::dontSendNotification);
        updateBeatWindowButtonHighlights(state, beatWindow);
        state.startBeatBox.setText("0", juce::dontSendNotification);

        if (currentProject)
            state.roll.setTempoBpm(currentProject->getTempoBpm());

        state.roll.setGrid(beatWindow, state.roll.getLowPitch(), state.roll.getHighPitch());
        updatePianoRollKeyRangeLabel(state);
        state.roll.setStartBeat(0);
        updatePianoRollPageIndicator(state);
    }

    void MainComponent::markPianoRollEditorDirty(PianoRollEditorWindowState& state)
    {
        state.dirty = true;
        refreshAggregatePianoRollDirtyFlag();
        updatePianoRollWindowDirtyIndicator(state);
    }

    void MainComponent::clearPianoRollEditorDirty(PianoRollEditorWindowState& state)
    {
        state.dirty = false;
        refreshAggregatePianoRollDirtyFlag();
        updatePianoRollWindowDirtyIndicator(state);
    }

    void MainComponent::updatePianoRollWindowDirtyIndicator(PianoRollEditorWindowState& state)
    {
        if (state.window == nullptr)
            return;

        auto name = "Piano Roll - " + getTrackDisplayName(state.trackIndex);
        if (state.dirty || state.hasPendingInstrumentAssignment)
            name += " *";

        state.window->setName(name);
    }

    bool MainComponent::applyPianoRollEditorChanges(PianoRollEditorWindowState& state)
    {
        if (!currentProject)
            return false;

        const auto index = state.trackIndex;

        if (index < 0 || index >= static_cast<int>(currentProject->getTracks().size()))
            return false;

        applyPianoRollSettings(state);

        auto& track = currentProject->getTracks()[static_cast<std::size_t>(index)];
        const auto changed = !noteListsEqual(track.getNotes(), state.roll.getNotes());
        track.getNotes() = state.roll.getNotes();

        if (index == getSelectedTrackIndex())
            refreshNoteEditor();

        updateTrackSummary(*currentProject);
        refreshTrackManagerText();
        recordExternalTrackStateUpdate(index, changed);

        if (changed)
            setProjectDirty();

        clearPianoRollEditorDirty(state);
        logMessage("Applied Piano Roll changes for " + getTrackDisplayName(index) + ". Use Save Project to write them to disk.");
        return true;
    }

    void MainComponent::discardPianoRollEditorChanges(PianoRollEditorWindowState& state)
    {
        if (!currentProject)
        {
            clearPianoRollEditorDirty(state);
            return;
        }

        const auto index = state.trackIndex;

        if (index >= 0 && index < static_cast<int>(currentProject->getTracks().size()))
        {
            state.suppressDirty = true;
            state.roll.setNotes(currentProject->getTracks()[static_cast<std::size_t>(index)].getNotes());
            state.suppressDirty = false;

            if (index == getSelectedTrackIndex())
                refreshNoteEditor();
        }

        discardPendingPianoRollTrackSettings(state);
        clearPianoRollEditorDirty(state);
        logMessage("Discarded unapplied Piano Roll changes for " + getTrackDisplayName(index) + ".");
    }

    void MainComponent::finishClosingPianoRollWindow(int trackIndex)
    {
        if (auto* state = findPianoRollEditorWindow(trackIndex))
        {
            if (state->window != nullptr)
            {
                state->window->setVisible(false);
                state->window->setContentOwned(nullptr, false);
            }
        }

        juce::MessageManager::callAsync(
            [this, trackIndex]
            {
                pianoRollEditorWindows.erase(trackIndex);
                refreshAggregatePianoRollDirtyFlag();
            }
        );

        logMessage("Closed Piano Roll window for " + getTrackDisplayName(trackIndex) + ".");
    }

    void MainComponent::closePianoRollWindowWithDirtyCheck(int trackIndex)
    {
        auto* state = findPianoRollEditorWindow(trackIndex);

        if (state == nullptr)
            return;

        if (!state->dirty && !state->hasPendingInstrumentAssignment)
        {
            finishClosingPianoRollWindow(trackIndex);
            return;
        }

        auto* alert = new juce::AlertWindow(
            "Piano Roll Has Unapplied Changes",
            "Apply note edits and pending track settings for " + getTrackDisplayName(trackIndex) + ", discard them and close, or cancel and keep editing?",
            juce::AlertWindow::QuestionIcon
        );

        alert->addButton("Apply Changes", 1, juce::KeyPress(juce::KeyPress::returnKey));
        alert->addButton("Discard and Exit", 2);
        alert->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

        alert->enterModalState(
            true,
            juce::ModalCallbackFunction::create(
                [this, alert, trackIndex](int result)
                {
                    std::unique_ptr<juce::AlertWindow> cleanup(alert);
                    auto* state = findPianoRollEditorWindow(trackIndex);

                    if (state == nullptr)
                        return;

                    if (result == 1)
                    {
                        applyPianoRollEditorChanges(*state);
                        finishClosingPianoRollWindow(trackIndex);
                    }
                    else if (result == 2)
                    {
                        discardPianoRollEditorChanges(*state);
                        finishClosingPianoRollWindow(trackIndex);
                    }
                    else
                    {
                        logMessage("Piano Roll close cancelled.");
                    }
                }
            )
        );
    }

    int MainComponent::getFirstOpenPianoRollTrackNumberForSequence(int sequenceIndex) const
    {
        if (sequenceIndex < 0 || sequenceIndex >= static_cast<int>(importSections.size()))
            return 0;

        const auto& sequence = importSections[static_cast<std::size_t>(sequenceIndex)];

        for (const auto& entry : pianoRollEditorWindows)
        {
            if (entry.second == nullptr)
                continue;

            const auto trackNumber = entry.second->trackIndex + 1;
            if (std::find(sequence.trackNumbers.begin(), sequence.trackNumbers.end(), trackNumber) != sequence.trackNumbers.end())
                return trackNumber;
        }

        return 0;
    }

    void MainComponent::updateOpenPianoRollTrackNames()
    {
        for (const auto& entry : pianoRollEditorWindows)
        {
            if (entry.second != nullptr)
            {
                updatePianoRollWindowDirtyIndicator(*entry.second);

                if (auto* content = dynamic_cast<PianoRollWindowContent*>(entry.second->content.get()))
                    content->setHeaderTrackInfo(entry.second->trackIndex, getTrackDisplayName(entry.second->trackIndex));

                populatePianoRollInstrumentCombo(*entry.second);
                refreshPianoRollTrackSoundLibraryDisplay(*entry.second);
            }
        }
    }

    void MainComponent::refreshOpenPianoRollAfterTrackSequenceMove(int trackIndex)
    {
        auto* state = findPianoRollEditorWindow(trackIndex);
        if (state == nullptr)
            return;

        // Moving a track between sequences must not reload or replace the Piano Roll note buffer.
        // Only refresh non-destructive labels/dirty indicators so unapplied notes and pending
        // track settings remain exactly as the editor window held them before the move.
        updatePianoRollWindowDirtyIndicator(*state);

        if (auto* content = dynamic_cast<PianoRollWindowContent*>(state->content.get()))
        {
            content->setHeaderTrackInfo(state->trackIndex, getTrackDisplayName(state->trackIndex));
            content->setHeaderInstrumentInfo(getPianoRollHeaderInstrumentTextForState(*state));
        }

        refreshAggregatePianoRollDirtyFlag();
    }

    void MainComponent::syncPianoRollFromSelectedTrack()
    {
        if (pianoRollEditorDirty && pianoRollWindow != nullptr)
        {
            logMessage("Piano Roll has unapplied edits; external refresh left the local roll unchanged.");
            return;
        }

        if (!currentProject)
        {
            suppressPianoRollEditorDirty = true;
            pianoRoll.setNotes({});
            suppressPianoRollEditorDirty = false;
            clearPianoRollEditorDirty();
            return;
        }

        const auto index = getSelectedTrackIndex();

        if (index < 0 || index >= static_cast<int>(currentProject->getTracks().size()))
        {
            suppressPianoRollEditorDirty = true;
            pianoRoll.setNotes({});
            suppressPianoRollEditorDirty = false;
            clearPianoRollEditorDirty();
            return;
        }

        const auto& track = currentProject->getTracks()[static_cast<std::size_t>(index)];
        pianoRoll.setTempoBpm(currentProject->getTempoBpm());
        suppressPianoRollEditorDirty = true;
        pianoRoll.setNotes(track.getNotes());
        suppressPianoRollEditorDirty = false;
        clearPianoRollEditorDirty();

        if (!track.getNotes().empty())
        {
            juce::String message;
            message << "Loaded selected track into Piano Roll: "
                    << track.getName()
                    << " ("
                    << static_cast<int>(track.getNotes().size())
                    << " note(s))";
            logMessage(message);
        }
    }

    void MainComponent::markPianoRollEditorDirty()
    {
        if (pianoRollWindow == nullptr && pianoRollOpenTrackIndex < 0)
            return;

        pianoRollEditorDirty = true;
        updatePianoRollWindowDirtyIndicator();
    }

    void MainComponent::clearPianoRollEditorDirty()
    {
        if (!pianoRollEditorWindows.empty())
        {
            refreshAggregatePianoRollDirtyFlag();
            return;
        }

        pianoRollEditorDirty = false;
        updatePianoRollWindowDirtyIndicator();
    }

    void MainComponent::updatePianoRollWindowDirtyIndicator()
    {
        if (pianoRollWindow == nullptr)
            return;

        juce::String displayName = getSelectedTrackDisplayName();

        if (currentProject
            && pianoRollOpenTrackIndex >= 0
            && pianoRollOpenTrackIndex < static_cast<int>(currentProject->getTracks().size()))
        {
            displayName = currentProject->getTracks()[static_cast<std::size_t>(pianoRollOpenTrackIndex)].getName();
        }

        auto name = "Piano Roll - " + displayName;
        if (pianoRollEditorDirty)
            name += " *";

        pianoRollWindow->setName(name);
    }

    bool MainComponent::applyPianoRollEditorChanges()
    {
        if (!pianoRollEditorWindows.empty())
        {
            bool appliedAny = false;

            for (auto& entry : pianoRollEditorWindows)
            {
                if (entry.second != nullptr && entry.second->dirty)
                    appliedAny = applyPianoRollEditorChanges(*entry.second) || appliedAny;
            }

            refreshAggregatePianoRollDirtyFlag();
            return appliedAny;
        }

        if (!currentProject)
            return false;

        const auto selectedIndex = getSelectedTrackIndex();
        const auto index = pianoRollOpenTrackIndex >= 0 ? pianoRollOpenTrackIndex : selectedIndex;

        if (index < 0 || index >= static_cast<int>(currentProject->getTracks().size()))
            return false;

        auto& track = currentProject->getTracks()[static_cast<std::size_t>(index)];
        const auto changed = !noteListsEqual(track.getNotes(), pianoRoll.getNotes());
        track.getNotes() = pianoRoll.getNotes();

        if (index == selectedIndex)
            refreshNoteEditor();

        updateTrackSummary(*currentProject);
        refreshTrackManagerText();
        recordExternalTrackStateUpdate(index, changed);

        if (changed)
            setProjectDirty();

        pianoRollEditorDirty = false;
        updatePianoRollWindowDirtyIndicator();
        logMessage("Applied Piano Roll changes to the open project. Use Save Project to write them to disk.");
        return true;
    }

    void MainComponent::discardPianoRollEditorChanges()
    {
        if (!pianoRollEditorWindows.empty())
        {
            for (auto& entry : pianoRollEditorWindows)
            {
                if (entry.second != nullptr && entry.second->dirty)
                    discardPianoRollEditorChanges(*entry.second);
            }

            refreshAggregatePianoRollDirtyFlag();
            return;
        }

        if (!currentProject)
        {
            pianoRollEditorDirty = false;
            updatePianoRollWindowDirtyIndicator();
            return;
        }

        const auto selectedIndex = getSelectedTrackIndex();
        const auto index = pianoRollOpenTrackIndex >= 0 ? pianoRollOpenTrackIndex : selectedIndex;

        if (index >= 0 && index < static_cast<int>(currentProject->getTracks().size()))
        {
            suppressPianoRollEditorDirty = true;
            pianoRoll.setNotes(currentProject->getTracks()[static_cast<std::size_t>(index)].getNotes());
            suppressPianoRollEditorDirty = false;

            if (index == selectedIndex)
                refreshNoteEditor();
        }

        pianoRollEditorDirty = false;
        updatePianoRollWindowDirtyIndicator();
        logMessage("Discarded unapplied Piano Roll changes.");
    }

    void MainComponent::finishClosingPianoRollWindow()
    {
        cleanupPianoRollPreviewFiles();

        if (!pianoRollEditorWindows.empty())
        {
            std::vector<int> trackIndices;
            for (const auto& entry : pianoRollEditorWindows)
                trackIndices.push_back(entry.first);

            for (const auto trackIndex : trackIndices)
                finishClosingPianoRollWindow(trackIndex);

            return;
        }

        if (pianoRollWindow != nullptr)
        {
            pianoRollWindow->setVisible(false);
            pianoRollWindow->setContentOwned(nullptr, false);

            juce::MessageManager::callAsync(
                [this]
                {
                    pianoRollWindow.reset();
                    pianoRollContent.reset();
                    pianoRollOpenTrackIndex = -1;
                    pianoRollEditorDirty = false;
                    updatePianoRollWindowDirtyIndicator();
                }
            );
        }

        logMessage("Closed Piano Roll window.");
    }

    void MainComponent::closePianoRollWindowWithDirtyCheck()
    {
        if (!pianoRollEditorWindows.empty())
        {
            if (!pianoRollEditorDirty)
            {
                finishClosingPianoRollWindow();
                return;
            }

            auto* alert = new juce::AlertWindow(
                "Piano Roll Windows Have Unapplied Changes",
                "Apply all open Piano Roll edits to the project, discard them and close all Piano Rolls, or cancel and keep editing?",
                juce::AlertWindow::QuestionIcon
            );

            alert->addButton("Apply Changes", 1, juce::KeyPress(juce::KeyPress::returnKey));
            alert->addButton("Discard and Exit", 2);
            alert->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

            alert->enterModalState(
                true,
                juce::ModalCallbackFunction::create(
                    [this, alert](int result)
                    {
                        std::unique_ptr<juce::AlertWindow> cleanup(alert);

                        if (result == 1)
                        {
                            applyPianoRollEditorChanges();
                            finishClosingPianoRollWindow();
                        }
                        else if (result == 2)
                        {
                            discardPianoRollEditorChanges();
                            finishClosingPianoRollWindow();
                        }
                        else
                        {
                            logMessage("Piano Roll close cancelled.");
                        }
                    }
                )
            );

            return;
        }

        if (!pianoRollEditorDirty)
        {
            finishClosingPianoRollWindow();
            return;
        }

        auto* alert = new juce::AlertWindow(
            "Piano Roll Has Unapplied Changes",
            "Apply your Piano Roll note edits to the open project, discard them and close, or cancel and keep editing?",
            juce::AlertWindow::QuestionIcon
        );

        alert->addButton("Apply Changes", 1, juce::KeyPress(juce::KeyPress::returnKey));
        alert->addButton("Discard and Exit", 2);
        alert->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

        alert->enterModalState(
            true,
            juce::ModalCallbackFunction::create(
                [this, alert](int result)
                {
                    std::unique_ptr<juce::AlertWindow> cleanup(alert);

                    if (result == 1)
                    {
                        applyPianoRollEditorChanges();
                        finishClosingPianoRollWindow();
                    }
                    else if (result == 2)
                    {
                        discardPianoRollEditorChanges();
                        finishClosingPianoRollWindow();
                    }
                    else
                    {
                        logMessage("Piano Roll close cancelled.");
                    }
                }
            )
        );
    }


    void MainComponent::rebuildSectionsFromTracksIfNeeded()
    {
        // Sequences/layers are only created by import/load/new-project actions.
        // Do not auto-create one sequence per track, because that makes imported track numbers hard to read.
    }

    int MainComponent::sequenceDefaultNumberFromName(const juce::String& name) const
    {
        auto trimmed = name.trim();

        if (trimmed.startsWithIgnoreCase("Sequence "))
            trimmed = trimmed.substring(9).trimStart();
        else if (trimmed.startsWithIgnoreCase("Seq "))
            trimmed = trimmed.substring(4).trimStart();
        else
            return 0;

        int value = 0;
        bool sawDigit = false;

        for (int i = 0; i < trimmed.length(); ++i)
        {
            const auto ch = trimmed[i];

            if (ch >= '0' && ch <= '9')
            {
                sawDigit = true;
                value = value * 10 + (ch - '0');
            }
            else
            {
                break;
            }
        }

        return sawDigit ? value : 0;
    }

    int MainComponent::allocateNextSequenceId() const
    {
        std::set<int> usedIds;

        for (const auto& section : importSections)
        {
            if (section.id > 0)
                usedIds.insert(section.id);
        }

        for (int id = 1; id <= mw::core::Project::maxSequenceCount; ++id)
        {
            if (usedIds.find(id) == usedIds.end())
                return id;
        }

        return std::max(1, static_cast<int>(importSections.size()) + 1);
    }

    int MainComponent::allocateNextDefaultSequenceNumber() const
    {
        std::set<int> usedNumbers;

        for (const auto& section : importSections)
        {
            const auto parsed = sequenceDefaultNumberFromName(section.name);

            if (parsed > 0)
                usedNumbers.insert(parsed);
        }

        for (int number = 1; number <= mw::core::Project::maxSequenceCount; ++number)
        {
            if (usedNumbers.find(number) == usedNumbers.end())
                return number;
        }

        return std::max(1, static_cast<int>(importSections.size()) + 1);
    }

    juce::String MainComponent::makeDefaultSequenceName(const std::filesystem::path& sourcePath, const juce::String& fallbackName) const
    {
        const auto number = allocateNextDefaultSequenceNumber();
        juce::String name;
        name << "Sequence " << number;

        const auto suffix = sourcePath.empty()
            ? fallbackName.trim()
            : juce::String(sourcePath.stem().string()).trim();

        if (suffix.isNotEmpty() && !suffix.equalsIgnoreCase("manual") && !suffix.startsWithIgnoreCase("sequence ") && !suffix.startsWithIgnoreCase("seq "))
            name << " - " << suffix;

        return name;
    }

    bool MainComponent::isPianoRollOpenForSequence(int sequenceIndex) const
    {
        return getFirstOpenPianoRollTrackNumberForSequence(sequenceIndex) > 0;
    }

    void MainComponent::focusFirstSequenceAfterSequenceRemoval()
    {
        sequenceMapFirstIndex = 0;
        sequenceMapStartBeat = 0;
        trackManagerMapStartBeatBox.setText("0", juce::dontSendNotification);

        if (importSections.empty())
        {
            activeImportSectionIndex = -1;
            trackManagerSectionBox.setText("1", juce::dontSendNotification);
            return;
        }

        activeImportSectionIndex = 0;
        const auto& first = importSections.front();
        trackManagerSectionBox.setText("1", juce::dontSendNotification);
        trackManagerSectionStartBeatBox.setText(juce::String(static_cast<double>(first.startTick) / mw::core::Project::ticksPerQuarterNote, 2), juce::dontSendNotification);

        if (!first.trackNumbers.empty())
        {
            const auto firstTrack = first.trackNumbers.front();
            trackManagerSelectBox.setText(juce::String(firstTrack), juce::dontSendNotification);

            if (currentProject && firstTrack >= 1 && firstTrack <= static_cast<int>(currentProject->getTracks().size()))
                trackCombo.setSelectedId(firstTrack, juce::sendNotification);
        }
    }

    void MainComponent::recordImportSection(const juce::String& name, const std::filesystem::path& sourcePath, std::int64_t startTick, std::int64_t endTick, const std::vector<int>& trackNumbers, bool isLayer, const juce::String& createdBy)
    {
        if (static_cast<int>(importSections.size()) >= mw::core::Project::maxSequenceCount)
        {
            logMessage("Sequence limit reached: " + juce::String(mw::core::Project::maxSequenceCount) + ". New sequence was not added.");
            return;
        }

        ImportSectionInfo section;
        section.id = allocateNextSequenceId();
        section.name = makeDefaultSequenceName(sourcePath, name.isEmpty() ? juce::String("Manual") : name);
        section.sourcePath = sourcePath;
        section.startTick = startTick;
        section.endTick = std::max(startTick, endTick);
        section.trackNumbers = trackNumbers;
        std::sort(section.trackNumbers.begin(), section.trackNumbers.end());
        section.trackNumbers.erase(std::unique(section.trackNumbers.begin(), section.trackNumbers.end()), section.trackNumbers.end());
        section.isLayer = isLayer;
        section.createdBy = createdBy;

        importSections.push_back(std::move(section));
        activeImportSectionIndex = static_cast<int>(importSections.size()) - 1;
        sequenceMapFirstIndex = std::max(0, static_cast<int>(importSections.size()) - 8);

        trackManagerSectionBox.setText(juce::String(activeImportSectionIndex + 1), juce::dontSendNotification);
        trackManagerSectionStartBeatBox.setText(juce::String(static_cast<double>(startTick) / mw::core::Project::ticksPerQuarterNote, 2), juce::dontSendNotification);
        setProjectDirty();
    }

    juce::Colour MainComponent::getSequenceColourForIndex(int sequenceIndex) const
    {
        if (sequenceIndex >= 0)
        {
            const auto overrideIt = sequenceColourOverrides.find(sequenceIndex);

            if (overrideIt != sequenceColourOverrides.end())
                return overrideIt->second;

            if (sequenceIndex < static_cast<int>(importSections.size()))
                return defaultSequenceColourFor(sequenceIndex, importSections[static_cast<std::size_t>(sequenceIndex)].name);
        }

        return poorMansSequencePalette[0];
    }

    void MainComponent::syncSequencesToProjectMetadata()
    {
        if (!currentProject)
            return;

        std::vector<mw::core::ProjectSequenceMetadata> metadata;

        for (int i = 0; i < static_cast<int>(importSections.size()); ++i)
        {
            const auto& section = importSections[static_cast<std::size_t>(i)];

            mw::core::ProjectSequenceMetadata sequence;
            sequence.id = section.id > 0 ? section.id : (i + 1);
            sequence.number = i + 1;
            sequence.type = "sequence";
            sequence.name = section.name.toStdString();
            sequence.sourceFile = section.sourcePath;
            sequence.createdBy = section.createdBy.toStdString();
            sequence.notes = section.notes.toStdString();
            sequence.locked = section.locked;
            sequence.startTick = section.startTick;
            sequence.endTick = section.endTick;
            sequence.tracks = section.trackNumbers;
            std::sort(sequence.tracks.begin(), sequence.tracks.end());
            sequence.tracks.erase(std::unique(sequence.tracks.begin(), sequence.tracks.end()), sequence.tracks.end());
            sequence.color = sequenceColourToHex(getSequenceColourForIndex(i)).toStdString();

            metadata.push_back(std::move(sequence));
        }

        currentProject->setSequences(std::move(metadata));
    }

    void MainComponent::normalizeEmptySequencesAfterMembershipChange()
    {
        if (!currentProject)
            return;

        const auto trackCount = static_cast<int>(currentProject->getTracks().size());

        for (int i = 0; i < static_cast<int>(importSections.size()); ++i)
        {
            auto& section = importSections[static_cast<std::size_t>(i)];

            section.trackNumbers.erase(
                std::remove_if(
                    section.trackNumbers.begin(),
                    section.trackNumbers.end(),
                    [trackCount](int trackNumber)
                    {
                        return trackNumber <= 0 || trackNumber > trackCount;
                    }
                ),
                section.trackNumbers.end()
            );

            std::sort(section.trackNumbers.begin(), section.trackNumbers.end());
            section.trackNumbers.erase(std::unique(section.trackNumbers.begin(), section.trackNumbers.end()), section.trackNumbers.end());

            const bool hasMidiTracks = !section.trackNumbers.empty();
            const bool hasAudioClips = sequenceHasAudioClips(i + 1);

            if (!hasMidiTracks && !hasAudioClips)
            {
                // Keep the sequence row itself, but make the timeline/map clearly empty.
                // This prevents a moved-away source sequence from still drawing a colored block.
                section.endTick = section.startTick;
            }
            else if (section.endTick < section.startTick)
            {
                section.endTick = section.startTick;
            }
        }
    }

    void MainComponent::restoreSequencesFromProjectMetadata()
    {
        importSections.clear();
        sequenceColourOverrides.clear();
        activeImportSectionIndex = -1;
        sequenceMapFirstIndex = 0;

        if (!currentProject)
            return;

        const auto& metadata = currentProject->getSequences();

        for (const auto& sequence : metadata)
        {
            ImportSectionInfo section;
            section.id = sequence.id > 0 ? sequence.id : sequence.number;
            section.name = sequence.name.empty() ? ("Sequence " + juce::String(sequence.number > 0 ? sequence.number : static_cast<int>(importSections.size()) + 1)) : sequence.name;
            section.sourcePath = sequence.sourceFile;
            section.startTick = sequence.startTick;
            section.endTick = sequence.endTick;
            section.trackNumbers = sequence.tracks;
            std::sort(section.trackNumbers.begin(), section.trackNumbers.end());
            section.trackNumbers.erase(std::unique(section.trackNumbers.begin(), section.trackNumbers.end()), section.trackNumbers.end());
            section.isLayer = false;
            section.createdBy = sequence.createdBy;
            section.notes = sequence.notes;
            section.locked = sequence.locked;

            importSections.push_back(std::move(section));

            if (!sequence.color.empty())
                sequenceColourOverrides[static_cast<int>(importSections.size()) - 1] = readableSequenceColour(sequenceColourFromHex(sequence.color, defaultSequenceColourFor(static_cast<int>(importSections.size()) - 1, importSections.back().name)));
        }

        if (!importSections.empty())
            activeImportSectionIndex = 0;
    }

    int MainComponent::getSequenceIndexForTrack(int trackNumber) const
    {
        if (trackNumber <= 0)
            return -1;

        for (int i = 0; i < static_cast<int>(importSections.size()); ++i)
        {
            const auto& section = importSections[static_cast<std::size_t>(i)];

            if (std::find(section.trackNumbers.begin(), section.trackNumbers.end(), trackNumber) != section.trackNumbers.end())
                return i;
        }

        return -1;
    }

    bool MainComponent::ensureSelectedTrackHasSequenceForPianoRoll()
    {
        if (!currentProject)
        {
            logMessage("Piano Roll: no project loaded.");
            return false;
        }

        const auto index = getSelectedTrackIndex();

        if (index < 0 || index >= static_cast<int>(currentProject->getTracks().size()))
        {
            logMessage("Piano Roll: no valid track selected.");
            return false;
        }

        if (currentProject->getTracks()[static_cast<std::size_t>(index)].isAudioClipTrack())
        {
            logMessage("Piano Roll is MIDI-only. AudioClip tracks are managed like normal tracks in Track Manager.");
            return false;
        }

        const int trackNumber = index + 1;
        const auto existingSequenceIndex = getSequenceIndexForTrack(trackNumber);

        if (existingSequenceIndex >= 0)
        {
            activeImportSectionIndex = existingSequenceIndex;
            trackManagerSectionBox.setText(juce::String(existingSequenceIndex + 1), juce::dontSendNotification);
            trackManagerSelectBox.setText(juce::String(trackNumber), juce::dontSendNotification);
            return true;
        }

        const auto endBeat = std::max(4, getSelectedTrackEndBeat());
        const auto endTick = static_cast<std::int64_t>(endBeat) * mw::core::Project::ticksPerQuarterNote;
        bool createdNewSequence = false;

        if (importSections.empty()
            || activeImportSectionIndex < 0
            || activeImportSectionIndex >= static_cast<int>(importSections.size())
            || importSections[static_cast<std::size_t>(activeImportSectionIndex)].locked)
        {
            if (static_cast<int>(importSections.size()) >= mw::core::Project::maxSequenceCount)
            {
                logMessage("Piano Roll: sequence limit reached (" + juce::String(mw::core::Project::maxSequenceCount) + "). Close or remove another sequence before creating a new one.");
                return false;
            }

            ImportSectionInfo section;
            section.id = allocateNextSequenceId();
            section.name = makeDefaultSequenceName(std::filesystem::path{}, "Manual");
            section.sourcePath = std::filesystem::path{};
            section.startTick = 0;
            section.endTick = endTick;
            section.trackNumbers = { trackNumber };
            section.isLayer = false;
            section.createdBy = "manual";

            importSections.push_back(std::move(section));
            activeImportSectionIndex = static_cast<int>(importSections.size()) - 1;
            createdNewSequence = true;
        }
        else
        {
            auto& section = importSections[static_cast<std::size_t>(activeImportSectionIndex)];

            if (std::find(section.trackNumbers.begin(), section.trackNumbers.end(), trackNumber) == section.trackNumbers.end())
                section.trackNumbers.push_back(trackNumber);

            std::sort(section.trackNumbers.begin(), section.trackNumbers.end());
            section.trackNumbers.erase(std::unique(section.trackNumbers.begin(), section.trackNumbers.end()), section.trackNumbers.end());
            section.endTick = std::max(section.endTick, endTick);
        }

        trackManagerSelectBox.setText(juce::String(trackNumber), juce::dontSendNotification);
        trackManagerSectionBox.setText(juce::String(activeImportSectionIndex + 1), juce::dontSendNotification);
        trackManagerSectionStartBeatBox.setText("0", juce::dontSendNotification);
        syncSequencesToProjectMetadata();
        refreshTrackManagerText();
        refreshMainSequenceSelector();
        refreshActiveSequenceThoughtsEditor();
        updateRenderTargetLabel();

        if (trackManagerContent != nullptr)
        {
            if (auto* content = dynamic_cast<TrackManagerWindowContent*>(trackManagerContent.get()))
                content->refreshSequenceMap();
        }

        setProjectDirty();

        juce::String message;
        message << "Piano Roll linked track #"
                << trackNumber
                << " to "
                << (createdNewSequence ? "new" : "active")
                << " sequence #"
                << (activeImportSectionIndex + 1)
                << ".";
        logMessage(message);

        return true;
    }

    void MainComponent::updateActiveSequenceFromSelectedTrack()
    {
        rebuildSectionsFromTracksIfNeeded();
        normalizeEmptySequencesAfterMembershipChange();

        auto trackNumber = trackCombo.getSelectedId();

        if (trackNumber <= 0)
            trackNumber = trackManagerSelectBox.getText().getIntValue();

        if (currentProject)
        {
            const auto maxTrack = static_cast<int>(currentProject->getTracks().size());
            if (trackNumber > 0 && trackNumber <= maxTrack)
                trackManagerSelectBox.setText(juce::String(trackNumber), juce::dontSendNotification);
        }

        const auto sequenceIndex = getSequenceIndexForTrack(trackNumber);

        if (sequenceIndex >= 0)
            activeImportSectionIndex = sequenceIndex;

        if (activeImportSectionIndex >= 0 && activeImportSectionIndex < static_cast<int>(importSections.size()))
        {
            const auto& section = importSections[static_cast<std::size_t>(activeImportSectionIndex)];
            trackManagerSectionBox.setText(juce::String(activeImportSectionIndex + 1), juce::dontSendNotification);
            trackManagerSectionStartBeatBox.setText(juce::String(static_cast<double>(section.startTick) / mw::core::Project::ticksPerQuarterNote, 2), juce::dontSendNotification);
        }
    }

    void MainComponent::openSequenceColorEditorFromManager()
    {
        if (!currentProject)
        {
            logMessage("Sequence Color: no project loaded.");
            return;
        }

        updateActiveSequenceFromSelectedTrack();

        auto sequenceNumber = trackManagerSectionBox.getText().getIntValue();

        if (sequenceNumber <= 0)
            sequenceNumber = activeImportSectionIndex + 1;

        if (sequenceNumber <= 0 || sequenceNumber > static_cast<int>(importSections.size()))
        {
            logMessage("Sequence Color: no active sequence.");
            return;
        }

        const auto sequenceIndex = sequenceNumber - 1;
        const auto& section = importSections[static_cast<std::size_t>(sequenceIndex)];

        auto initialColour = defaultSequenceColourFor(sequenceIndex, section.name);
        const auto overrideIt = sequenceColourOverrides.find(sequenceIndex);

        if (overrideIt != sequenceColourOverrides.end())
            initialColour = overrideIt->second;

        auto closeWindow = [this]
        {
            if (sequenceColorWindow != nullptr)
            {
                sequenceColorWindow->setVisible(false);
                sequenceColorWindow->setContentOwned(nullptr, false);

                juce::MessageManager::callAsync(
                    [this]
                    {
                        sequenceColorWindow.reset();
                        sequenceColorContent.reset();
                    }
                );
            }
        };

        auto applyColour = [this, closeWindow](juce::Colour selectedColour)
        {
            if (applySequenceColorFromEditor(selectedColour))
                closeWindow();
        };

        sequenceColorContent = std::make_unique<SequenceColourEditorContent>(
            sequenceNumber,
            section.name,
            initialColour,
            applyColour,
            closeWindow
        );

        auto* window = new PianoRollDocumentWindow("Sequence Color", closeWindow);
        applyPoorMansStudioCustomTitleBar(*window);
        applyPoorMansStudioWindowIcon(*window, PoorMansStudioWindowIcon::ColourWheel);
        window->setResizable(false, false);
        window->setContentNonOwned(sequenceColorContent.get(), true);
        window->centreWithSize(460, 420);
        window->setVisible(true);

        sequenceColorWindow.reset(window);

        logMessage("Opened Sequence Color editor.");
    }

    bool MainComponent::applySequenceColorFromEditor(juce::Colour selectedColour)
    {
        rebuildSectionsFromTracksIfNeeded();

        auto sequenceNumber = trackManagerSectionBox.getText().getIntValue();

        if (sequenceNumber <= 0)
            sequenceNumber = activeImportSectionIndex + 1;

        if (sequenceNumber <= 0 || sequenceNumber > static_cast<int>(importSections.size()))
        {
            logMessage("Sequence Color: invalid sequence number.");
            return false;
        }

        auto colour = readableSequenceColour(selectedColour);
        const int sequenceIndex = sequenceNumber - 1;

        auto commitColour = [this, sequenceIndex, sequenceNumber, colour]
        {
            sequenceColourOverrides[sequenceIndex] = colour;
            activeImportSectionIndex = sequenceIndex;

            if (currentProject)
            {
                syncSequencesToProjectMetadata();
                updateTrackSummary(*currentProject);
            }

            refreshTrackManagerText();
            refreshMainSequenceSelector();
            setProjectDirty();

            logMessage(
                "Sequence #" + juce::String(sequenceNumber)
                + " color changed to "
                + sequenceColourToHex(colour)
                + "."
            );
        };

        const auto requestedHex = sequenceColourToHex(colour);
        for (int i = 0; i < static_cast<int>(importSections.size()); ++i)
        {
            if (i == sequenceIndex)
                continue;

            if (sequenceColourToHex(getSequenceColourForIndex(i)).equalsIgnoreCase(requestedHex))
            {
                auto* alert = new juce::AlertWindow(
                    "Sequence Color Already Used",
                    "Another sequence already uses " + requestedHex + ". Continue with this color or choose a different color?",
                    juce::AlertWindow::QuestionIcon
                );

                alert->addButton("Use Color", 1, juce::KeyPress(juce::KeyPress::returnKey));
                alert->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
                alert->enterModalState(
                    true,
                    juce::ModalCallbackFunction::create(
                        [alert, commitColour](int result) mutable
                        {
                            std::unique_ptr<juce::AlertWindow> cleanup(alert);
                            if (result == 1)
                                commitColour();
                        }
                    )
                );
                return true;
            }
        }

        commitColour();
        return true;
    }

    void MainComponent::renameSequenceFromManager()
    {
        rebuildSectionsFromTracksIfNeeded();

        auto sequenceNumber = trackManagerSectionBox.getText().getIntValue();

        if (sequenceNumber <= 0)
            sequenceNumber = activeImportSectionIndex + 1;

        if (sequenceNumber <= 0 || sequenceNumber > static_cast<int>(importSections.size()))
        {
            logMessage("Sequence Rename: invalid sequence number.");
            return;
        }

        auto& section = importSections[static_cast<std::size_t>(sequenceNumber - 1)];

        auto* alert = new juce::AlertWindow(
            "Rename Sequence",
            "Enter a new name for sequence #" + juce::String(sequenceNumber) + ".",
            juce::AlertWindow::InfoIcon
        );

        alert->addTextEditor("sequenceName", section.name, "Sequence Name:");
        alert->addButton("Apply", 1, juce::KeyPress(juce::KeyPress::returnKey));
        alert->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

        alert->enterModalState(
            true,
            juce::ModalCallbackFunction::create(
                [this, alert, sequenceNumber](int result)
                {
                    std::unique_ptr<juce::AlertWindow> cleanup(alert);

                    if (result != 1)
                        return;

                    if (sequenceNumber <= 0 || sequenceNumber > static_cast<int>(importSections.size()))
                        return;

                    auto newName = alert->getTextEditorContents("sequenceName").trim();

                    if (newName.isEmpty())
                    {
                        logMessage("Sequence Rename cancelled: name was empty.");
                        return;
                    }

                    importSections[static_cast<std::size_t>(sequenceNumber - 1)].name = newName;
                    activeImportSectionIndex = sequenceNumber - 1;
                    refreshTrackManagerText();
                    setProjectDirty();

                    logMessage("Renamed sequence #" + juce::String(sequenceNumber) + " to: " + newName);
                }
            ),
            true
        );
    }

    void MainComponent::editSequenceNotesFromManager()
    {
        rebuildSectionsFromTracksIfNeeded();

        auto sequenceNumber = trackManagerSectionBox.getText().getIntValue();

        if (sequenceNumber <= 0)
            sequenceNumber = activeImportSectionIndex + 1;

        if (sequenceNumber <= 0 || sequenceNumber > static_cast<int>(importSections.size()))
        {
            logMessage("Edit Thoughts: invalid sequence number.");
            return;
        }

        auto& section = importSections[static_cast<std::size_t>(sequenceNumber - 1)];
        const auto initialThoughts = section.notes;
        const auto sequenceName = section.name.isEmpty() ? juce::String("Unnamed Sequence") : section.name;

        sequenceThoughtsWindow.reset();

        auto closeWindow = [this]
        {
            juce::MessageManager::callAsync(
                [this]
                {
                    sequenceThoughtsWindow.reset();
                }
            );
        };

        auto* content = new SequenceThoughtsEditorContent(
            "Edit Thoughts",
            "Seq #" + juce::String(sequenceNumber) + " - " + sequenceName + ". These thoughts save into the sequence notes metadata when the project is saved.",
            initialThoughts,
            [this, closeWindow, sequenceNumber](juce::String nextThoughts)
            {
                if (sequenceNumber <= 0 || sequenceNumber > static_cast<int>(importSections.size()))
                {
                    closeWindow();
                    return;
                }

                auto& targetSection = importSections[static_cast<std::size_t>(sequenceNumber - 1)];

                if (targetSection.notes != nextThoughts)
                {
                    captureTrackManagerUndoState("Edit Sequence Thoughts");
                    targetSection.notes = nextThoughts;
                    activeImportSectionIndex = sequenceNumber - 1;
                    syncSequencesToProjectMetadata();
                    refreshTrackManagerText(false);
                    refreshMainSequenceSelector();
                    refreshActiveSequenceThoughtsEditor();
                    markTrackManagerEditorDirty("Edit Sequence Thoughts");

                    if (trackManagerContent != nullptr)
                    {
                        if (auto* managerContent = dynamic_cast<TrackManagerWindowContent*>(trackManagerContent.get()))
                            managerContent->refreshSequenceMap();
                        else
                            trackManagerContent->repaint();
                    }

                    logMessage("Updated Thoughts for sequence #" + juce::String(sequenceNumber) + ".");
                }
                else
                {
                    logMessage("Edit Thoughts: no changes for sequence #" + juce::String(sequenceNumber) + ".");
                }

                closeWindow();
            },
            closeWindow
        );

        auto* window = new PianoRollDocumentWindow(
            "Edit Thoughts",
            [content]
            {
                content->requestClose();
            }
        );

        applyPoorMansStudioWindowIcon(*window, PoorMansStudioWindowIcon::EditInfo);

        window->setContentOwned(content, true);
        window->centreWithSize(620, 430);
        window->setVisible(true);
        window->toFront(true);
        sequenceThoughtsWindow.reset(window);
    }

    void MainComponent::toggleSequenceLockFromManager()
    {
        rebuildSectionsFromTracksIfNeeded();

        auto sequenceNumber = trackManagerSectionBox.getText().getIntValue();

        if (sequenceNumber <= 0)
            sequenceNumber = activeImportSectionIndex + 1;

        if (sequenceNumber <= 0 || sequenceNumber > static_cast<int>(importSections.size()))
        {
            logMessage("Sequence Lock: invalid sequence number.");
            return;
        }

        auto& section = importSections[static_cast<std::size_t>(sequenceNumber - 1)];
        section.locked = !section.locked;
        activeImportSectionIndex = sequenceNumber - 1;

        refreshTrackManagerText();
        setProjectDirty();

        logMessage(
            "Sequence #" + juce::String(sequenceNumber)
            + (section.locked ? " locked." : " unlocked.")
        );
    }

    void MainComponent::moveTrackToSequenceFromManager()
    {
        if (!currentProject)
        {
            logMessage("Change Track Seq: no project loaded.");
            return;
        }

        rebuildSectionsFromTracksIfNeeded();
        normalizeEmptySequencesAfterMembershipChange();
        syncSequencesToProjectMetadata();

        auto trackNumber = trackManagerSelectBox.getText().getIntValue();
        if (trackNumber <= 0)
            trackNumber = trackCombo.getSelectedId();

        const auto maxTrack = static_cast<int>(currentProject->getTracks().size());
        if (trackNumber <= 0 || trackNumber > maxTrack)
        {
            logMessage("Change Track Seq: invalid track number.");
            return;
        }

        const int currentSequenceNumber = std::max(1, getSequenceIndexForTrack(trackNumber) + 1);
        trackManagerSelectBox.setText(juce::String(trackNumber), juce::dontSendNotification);
        if (currentSequenceNumber > 0 && currentSequenceNumber <= static_cast<int>(importSections.size()))
            trackManagerSectionBox.setText(juce::String(currentSequenceNumber), juce::dontSendNotification);

        showSequenceListPickerWindow(
            "Change Track Seq",
            "Pick an existing sequence from the scrollable list for track #" + juce::String(trackNumber) + ". OK commits that track to the selected sequence. Create Blank Seq adds an empty sequence to this picker; press OK to move the track there.",
            currentSequenceNumber,
            [this, trackNumber](int targetSequenceNumber)
            {
                if (applyTrackSequenceChangeFromManager(targetSequenceNumber, false, trackNumber))
                    markTrackManagerEditorDirty("Change Track Seq");
            },
            [this]
            {
                return createBlankSequenceOnly("Change Track Seq Create Blank Seq", false);
            }
        );
    }

    bool MainComponent::applyTrackSequenceChangeFromManager(int targetSequenceNumber, bool createIfNeeded, int trackNumberOverride)
    {
        if (!currentProject)
            return false;

        rebuildSectionsFromTracksIfNeeded();

        auto trackNumber = trackNumberOverride > 0 ? trackNumberOverride : trackManagerSelectBox.getText().getIntValue();
        if (trackNumber <= 0)
            trackNumber = trackCombo.getSelectedId();

        const auto maxTrack = static_cast<int>(currentProject->getTracks().size());
        if (trackNumber <= 0 || trackNumber > maxTrack)
        {
            logMessage("Change Track Seq: invalid track number.");
            return false;
        }

        if (targetSequenceNumber <= 0)
        {
            logMessage("Change Track Seq: target sequence cannot be blank or zero.");
            return false;
        }

        auto* movedPianoRollStateBeforeMove = findPianoRollEditorWindow(trackNumber - 1);
        const bool movingTrackWithOpenPianoRoll = movedPianoRollStateBeforeMove != nullptr;
        const bool movingTrackWithUnappliedPianoRollState = movingTrackWithOpenPianoRoll
            && (movedPianoRollStateBeforeMove->dirty || movedPianoRollStateBeforeMove->hasPendingInstrumentAssignment);

        bool undoCaptured = false;
        auto captureUndoOnce = [this, &undoCaptured]
        {
            if (!undoCaptured)
            {
                captureTrackManagerUndoState("Change Track Seq");
                undoCaptured = true;
            }
        };

        if (targetSequenceNumber > static_cast<int>(importSections.size()))
        {
            if (!createIfNeeded)
            {
                logMessage("Change Track Seq: target sequence does not exist.");
                return false;
            }

            if (static_cast<int>(importSections.size()) >= mw::core::Project::maxSequenceCount)
            {
                logMessage("Sequence limit reached: " + juce::String(mw::core::Project::maxSequenceCount) + ". Change Track Seq could not create a new sequence.");
                return false;
            }

            const auto& track = currentProject->getTracks()[static_cast<std::size_t>(trackNumber - 1)];
            std::int64_t startTick = 0;
            std::int64_t endTick = 0;

            if (!track.getNotes().empty())
            {
                startTick = track.getNotes().front().startTick;
                endTick = track.getNotes().front().startTick + track.getNotes().front().durationTicks;

                for (const auto& note : track.getNotes())
                {
                    startTick = std::min(startTick, note.startTick);
                    endTick = std::max(endTick, note.startTick + note.durationTicks);
                }
            }

            captureUndoOnce();

            ImportSectionInfo newSequence;
            newSequence.id = allocateNextSequenceId();
            newSequence.name = makeDefaultSequenceName(std::filesystem::path{}, "Track " + juce::String(trackNumber) + " - " + track.getName());
            newSequence.startTick = startTick;
            newSequence.endTick = std::max(startTick, endTick);
            newSequence.isLayer = false;

            importSections.push_back(std::move(newSequence));
            targetSequenceNumber = static_cast<int>(importSections.size());
        }

        if (targetSequenceNumber <= 0 || targetSequenceNumber > static_cast<int>(importSections.size()))
        {
            logMessage("Change Track Seq: invalid target sequence.");
            return false;
        }

        if (importSections[static_cast<std::size_t>(targetSequenceNumber - 1)].locked)
        {
            logMessage("Change Track Seq: target sequence is locked. Unlock it before moving tracks into it.");
            return false;
        }

        captureUndoOnce();

        const int sourceSequenceIndex = getSequenceIndexForTrack(trackNumber);
        const int sourceSequenceNumber = sourceSequenceIndex >= 0 ? sourceSequenceIndex + 1 : 0;

        for (auto& section : importSections)
        {
            section.trackNumbers.erase(
                std::remove(section.trackNumbers.begin(), section.trackNumbers.end(), trackNumber),
                section.trackNumbers.end()
            );
        }

        auto& target = importSections[static_cast<std::size_t>(targetSequenceNumber - 1)];

        if (std::find(target.trackNumbers.begin(), target.trackNumbers.end(), trackNumber) == target.trackNumbers.end())
            target.trackNumbers.push_back(trackNumber);

        std::sort(target.trackNumbers.begin(), target.trackNumbers.end());
        target.trackNumbers.erase(std::unique(target.trackNumbers.begin(), target.trackNumbers.end()), target.trackNumbers.end());

        // Commit the membership change immediately to both the live sequence list and
        // project metadata.  The main Track dropdown and the console rebuild from this
        // same state, so selecting away and back must retain the new sequence.
        normalizeEmptySequencesAfterMembershipChange();
        syncSequencesToProjectMetadata();

        activeImportSectionIndex = targetSequenceNumber - 1;
        trackManagerSelectBox.setText(juce::String(trackNumber), juce::dontSendNotification);
        trackManagerSectionBox.setText(juce::String(targetSequenceNumber), juce::dontSendNotification);

        if (activeImportSectionIndex >= 0 && activeImportSectionIndex < static_cast<int>(importSections.size()))
        {
            const auto& activeSection = importSections[static_cast<std::size_t>(activeImportSectionIndex)];
            trackManagerSectionStartBeatBox.setText(
                juce::String(static_cast<double>(activeSection.startTick) / mw::core::Project::ticksPerQuarterNote, 2),
                juce::dontSendNotification
            );
        }

        if (currentProject)
            updateTrackSummary(*currentProject);

        refreshOpenPianoRollAfterTrackSequenceMove(trackNumber - 1);
        refreshSequenceMembershipDisplays(sourceSequenceNumber, targetSequenceNumber, sourceSequenceNumber > 0 && sourceSequenceNumber != targetSequenceNumber);

        juce::String message;
        message << "Change Track Seq moved track #"
                << trackNumber
                << " to sequence #"
                << targetSequenceNumber
                << ".";

        if (sourceSequenceNumber > 0 && sourceSequenceNumber != targetSequenceNumber)
        {
            message << " Source sequence #"
                    << sourceSequenceNumber
                    << " now has tracks: "
                    << formatSequenceTrackSummary(sourceSequenceNumber - 1)
                    << ".";
        }

        message << " Target sequence #"
                << targetSequenceNumber
                << " now has tracks: "
                << formatSequenceTrackSummary(targetSequenceNumber - 1)
                << ".";

        if (movingTrackWithUnappliedPianoRollState)
            message << " Open Piano Roll edits and pending track settings were preserved.";
        else if (movingTrackWithOpenPianoRoll)
            message << " Open Piano Roll window was kept open.";

        setProjectDirty();
        logMessage(message);
        return true;
    }

    void MainComponent::removeSelectedSequenceFromManager()
    {
        if (!currentProject)
        {
            logMessage("Remove Seq: no project loaded.");
            return;
        }

        rebuildSectionsFromTracksIfNeeded();

        auto sequenceNumber = trackManagerSectionBox.getText().getIntValue();

        if (sequenceNumber <= 0)
            sequenceNumber = activeImportSectionIndex + 1;

        if (sequenceNumber <= 0 || sequenceNumber > static_cast<int>(importSections.size()))
        {
            logMessage("Remove Seq: no valid sequence selected.");
            return;
        }

        const auto sequenceIndex = sequenceNumber - 1;
        const auto sequenceName = importSections[static_cast<std::size_t>(sequenceIndex)].name;

        if (isPianoRollOpenForSequence(sequenceIndex))
        {
            const auto openTrackNumber = getFirstOpenPianoRollTrackNumberForSequence(sequenceIndex);
            juce::AlertWindow::showMessageBoxAsync(
                juce::AlertWindow::WarningIcon,
                "Close Piano Roll First",
                "Sequence " + juce::String(sequenceNumber) + " has track " + juce::String(openTrackNumber)
                    + " open in Piano Roll. Close that Piano Roll window before removing this sequence."
            );
            logMessage("Remove Seq blocked: sequence #" + juce::String(sequenceNumber) + " has track #" + juce::String(openTrackNumber) + " open in Piano Roll.");
            return;
        }

        auto* alert = new juce::AlertWindow(
            "Remove Sequence",
            "Remove sequence #" + juce::String(sequenceNumber) + " and all tracks in it?\n\n"
            "If a track from this sequence is open in Piano Roll, this action is blocked until that window is closed. "
            "Use Track Manager Undo if you need to restore it.",
            juce::AlertWindow::WarningIcon
        );

        alert->addButton("Remove Seq + Tracks", 1);
        alert->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

        alert->enterModalState(
            true,
            juce::ModalCallbackFunction::create(
                [this, alert, sequenceNumber, sequenceIndex, sequenceName](int result)
                {
                    std::unique_ptr<juce::AlertWindow> cleanup(alert);

                    if (result != 1)
                    {
                        logMessage("Remove Seq cancelled.");
                        return;
                    }

                    if (!currentProject || sequenceIndex < 0 || sequenceIndex >= static_cast<int>(importSections.size()))
                        return;

                    if (isPianoRollOpenForSequence(sequenceIndex))
                    {
                        juce::AlertWindow::showMessageBoxAsync(
                            juce::AlertWindow::WarningIcon,
                            "Close Piano Roll First",
                            "A track from this sequence is open in Piano Roll. Close Piano Roll before removing the sequence."
                        );
                        logMessage("Remove Seq blocked at confirm: sequence #" + juce::String(sequenceNumber) + " is open in Piano Roll.");
                        return;
                    }

                    captureTrackManagerUndoState("Remove Seq #" + juce::String(sequenceNumber));

                    std::vector<int> deletedTrackNumbers = importSections[static_cast<std::size_t>(sequenceIndex)].trackNumbers;
                    std::sort(deletedTrackNumbers.begin(), deletedTrackNumbers.end());
                    deletedTrackNumbers.erase(std::unique(deletedTrackNumbers.begin(), deletedTrackNumbers.end()), deletedTrackNumbers.end());

                    auto isDeletedTrack = [&deletedTrackNumbers](int trackNumber)
                    {
                        return std::binary_search(deletedTrackNumbers.begin(), deletedTrackNumbers.end(), trackNumber);
                    };

                    auto shiftTrackNumber = [&deletedTrackNumbers](int trackNumber)
                    {
                        const auto removedBefore = static_cast<int>(std::count_if(
                            deletedTrackNumbers.begin(),
                            deletedTrackNumbers.end(),
                            [trackNumber](int deletedTrack) { return deletedTrack < trackNumber; }
                        ));

                        return trackNumber - removedBefore;
                    };

                    std::vector<ImportSectionInfo> rebuiltSections;
                    std::map<int, juce::Colour> rebuiltColours;

                    for (int oldIndex = 0; oldIndex < static_cast<int>(importSections.size()); ++oldIndex)
                    {
                        if (oldIndex == sequenceIndex)
                            continue;

                        auto section = importSections[static_cast<std::size_t>(oldIndex)];
                        std::vector<int> adjustedTrackNumbers;

                        for (const auto trackNumber : section.trackNumbers)
                        {
                            if (isDeletedTrack(trackNumber))
                                continue;

                            const auto adjustedTrackNumber = shiftTrackNumber(trackNumber);

                            if (adjustedTrackNumber > 0)
                                adjustedTrackNumbers.push_back(adjustedTrackNumber);
                        }

                        std::sort(adjustedTrackNumbers.begin(), adjustedTrackNumbers.end());
                        adjustedTrackNumbers.erase(std::unique(adjustedTrackNumbers.begin(), adjustedTrackNumbers.end()), adjustedTrackNumbers.end());

                        if (adjustedTrackNumbers.empty())
                            continue;

                        section.trackNumbers = std::move(adjustedTrackNumbers);
                        const auto newIndex = static_cast<int>(rebuiltSections.size());

                        if (auto colourIt = sequenceColourOverrides.find(oldIndex); colourIt != sequenceColourOverrides.end())
                            rebuiltColours[newIndex] = colourIt->second;

                        rebuiltSections.push_back(std::move(section));
                    }

                    auto& tracks = currentProject->getTracks();

                    for (auto it = deletedTrackNumbers.rbegin(); it != deletedTrackNumbers.rend(); ++it)
                    {
                        const auto trackIndex = *it - 1;

                        if (trackIndex >= 0 && trackIndex < static_cast<int>(tracks.size()))
                            tracks.erase(tracks.begin() + trackIndex);
                    }

                    if (!pianoRollEditorWindows.empty())
                    {
                        std::map<int, std::unique_ptr<PianoRollEditorWindowState>> rebuiltPianoRollWindows;

                        for (auto& entry : pianoRollEditorWindows)
                        {
                            if (entry.second == nullptr)
                                continue;

                            const auto openTrackNumber = entry.second->trackIndex + 1;

                            if (isDeletedTrack(openTrackNumber))
                                continue;

                            const auto adjustedIndex = std::max(0, shiftTrackNumber(openTrackNumber) - 1);
                            entry.second->trackIndex = adjustedIndex;
                            rebuiltPianoRollWindows[adjustedIndex] = std::move(entry.second);
                        }

                        pianoRollEditorWindows = std::move(rebuiltPianoRollWindows);
                        updateOpenPianoRollTrackNames();
                    }

                    if (pianoRollOpenTrackIndex >= 0)
                    {
                        const auto openTrackNumber = pianoRollOpenTrackIndex + 1;

                        if (isDeletedTrack(openTrackNumber))
                            pianoRollOpenTrackIndex = -1;
                        else
                            pianoRollOpenTrackIndex = std::max(0, shiftTrackNumber(openTrackNumber) - 1);
                    }

                    importSections = std::move(rebuiltSections);
                    sequenceColourOverrides = std::move(rebuiltColours);
                    focusFirstSequenceAfterSequenceRemoval();

                    refreshTrackSelector();
                    focusFirstSequenceAfterSequenceRemoval();
                    syncTrackInspectorFromSelection();
                    refreshNoteEditor();

                    if (pianoRollWindow == nullptr)
                        syncPianoRollFromSelectedTrack();
                    else
                        updatePianoRollWindowDirtyIndicator();

                    updateTrackSummary(*currentProject);
                    refreshTrackManagerText();
                    refreshMainSequenceSelector();
                    updateRenderTargetLabel();

                    if (trackManagerContent != nullptr)
                    {
                        if (auto* content = dynamic_cast<TrackManagerWindowContent*>(trackManagerContent.get()))
                            content->refreshSequenceMap();
                        else
                            trackManagerContent->repaint();
                    }

                    setProjectDirty();
                    markTrackManagerEditorDirty("Remove Sequence");
                    logMessage("Removed sequence #" + juce::String(sequenceNumber) + " and refocused Sequence 1: " + sequenceName);
                }
            ),
            true
        );
    }

    void MainComponent::linkTrackToSectionFromManager()
    {
        if (!currentProject)
        {
            logMessage("Sequence Editor: no project loaded.");
            return;
        }

        rebuildSectionsFromTracksIfNeeded();

        auto sectionNumber = trackManagerSectionBox.getText().getIntValue();

        if (sectionNumber <= 0)
        {
            updateActiveSequenceFromSelectedTrack();
            sectionNumber = activeImportSectionIndex + 1;
        }

        auto trackNumber = trackManagerSelectBox.getText().getIntValue();

        if (trackNumber <= 0)
            trackNumber = trackCombo.getSelectedId();

        const auto maxTrack = static_cast<int>(currentProject->getTracks().size());

        if (sectionNumber <= 0 || sectionNumber > static_cast<int>(importSections.size()))
        {
            logMessage("Sequence Editor: no active sequence for Link Track.");
            return;
        }

        if (importSections[static_cast<std::size_t>(sectionNumber - 1)].locked)
        {
            logMessage("Sequence Editor: sequence is locked. Unlock it before linking tracks.");
            return;
        }

        if (trackNumber <= 0 || trackNumber > maxTrack)
        {
            logMessage("Sequence Editor: invalid track number for Link Track.");
            return;
        }

        auto& section = importSections[static_cast<std::size_t>(sectionNumber - 1)];

        if (std::find(section.trackNumbers.begin(), section.trackNumbers.end(), trackNumber) == section.trackNumbers.end())
        {
            captureTrackManagerUndoState("Link Track");
            section.trackNumbers.push_back(trackNumber);
        }

        activeImportSectionIndex = sectionNumber - 1;
        refreshTrackManagerText();

        juce::String message;
        message << "Sequence Editor linked track #"
                << trackNumber
                << " to sequence #"
                << sectionNumber
                << ".";

        setProjectDirty();
        logMessage(message);
    }

    void MainComponent::unlinkTrackFromSectionFromManager()
    {
        if (!currentProject)
        {
            logMessage("Sequence Editor: no project loaded.");
            return;
        }

        rebuildSectionsFromTracksIfNeeded();

        auto sectionNumber = trackManagerSectionBox.getText().getIntValue();

        if (sectionNumber <= 0)
        {
            updateActiveSequenceFromSelectedTrack();
            sectionNumber = activeImportSectionIndex + 1;
        }

        auto trackNumber = trackManagerSelectBox.getText().getIntValue();

        if (trackNumber <= 0)
            trackNumber = trackCombo.getSelectedId();

        if (sectionNumber <= 0 || sectionNumber > static_cast<int>(importSections.size()))
        {
            logMessage("Sequence Editor: no active sequence for Unlink Track.");
            return;
        }

        if (importSections[static_cast<std::size_t>(sectionNumber - 1)].locked)
        {
            logMessage("Sequence Editor: sequence is locked. Unlock it before unlinking tracks.");
            return;
        }

        const auto maxTrack = currentProject ? static_cast<int>(currentProject->getTracks().size()) : 0;

        if (trackNumber <= 0 || trackNumber > maxTrack)
        {
            logMessage("Sequence Editor: invalid track number for Unlink Track.");
            return;
        }

        auto& section = importSections[static_cast<std::size_t>(sectionNumber - 1)];
        if (std::find(section.trackNumbers.begin(), section.trackNumbers.end(), trackNumber) == section.trackNumbers.end())
        {
            refreshTrackManagerText();

            juce::String message;
            message << "Sequence Editor did not find track #"
                    << trackNumber
                    << " in sequence #"
                    << sectionNumber
                    << ".";

            logMessage(message);
            return;
        }

        captureTrackManagerUndoState("Unlink Track");

        section.trackNumbers.erase(
            std::remove(section.trackNumbers.begin(), section.trackNumbers.end(), trackNumber),
            section.trackNumbers.end()
        );

        const bool removedEmptyOriginalSequence = section.trackNumbers.empty();

        if (removedEmptyOriginalSequence)
        {
            const auto removedIndex = sectionNumber - 1;
            importSections.erase(importSections.begin() + removedIndex);

            std::map<int, juce::Colour> rebuiltColours;
            for (const auto& [oldIndex, colour] : sequenceColourOverrides)
            {
                if (oldIndex < removedIndex)
                    rebuiltColours[oldIndex] = colour;
                else if (oldIndex > removedIndex)
                    rebuiltColours[oldIndex - 1] = colour;
            }
            sequenceColourOverrides = std::move(rebuiltColours);

            if (activeImportSectionIndex >= removedIndex)
                activeImportSectionIndex = std::max(-1, activeImportSectionIndex - 1);
        }

        const auto& track = currentProject->getTracks()[static_cast<std::size_t>(trackNumber - 1)];

        std::int64_t startTick = 0;
        std::int64_t endTick = 0;

        if (!track.getNotes().empty())
        {
            startTick = track.getNotes().front().startTick;
            endTick = track.getNotes().front().startTick + track.getNotes().front().durationTicks;

            for (const auto& note : track.getNotes())
            {
                startTick = std::min(startTick, note.startTick);
                endTick = std::max(endTick, note.startTick + note.durationTicks);
            }
        }

        if (static_cast<int>(importSections.size()) >= mw::core::Project::maxSequenceCount)
        {
            logMessage("Sequence limit reached: " + juce::String(mw::core::Project::maxSequenceCount) + ". Unlink Track could not create a new sequence.");
            return;
        }

        ImportSectionInfo newSequence;
        newSequence.id = allocateNextSequenceId();
        newSequence.name = makeDefaultSequenceName(std::filesystem::path{}, "Track " + juce::String(trackNumber) + " - " + track.getName());
        newSequence.startTick = startTick;
        newSequence.endTick = std::max(startTick, endTick);
        newSequence.trackNumbers.push_back(trackNumber);
        newSequence.isLayer = false;

        importSections.push_back(std::move(newSequence));
        activeImportSectionIndex = static_cast<int>(importSections.size()) - 1;
        trackManagerSectionBox.setText(juce::String(activeImportSectionIndex + 1), juce::dontSendNotification);
        trackManagerSectionStartBeatBox.setText(juce::String(static_cast<double>(startTick) / mw::core::Project::ticksPerQuarterNote, 2), juce::dontSendNotification);

        refreshTrackManagerText();

        juce::String message;
        message << "Sequence Editor unlinked track #"
                << trackNumber
                << " from sequence #"
                << sectionNumber
                << " and created sequence #"
                << (activeImportSectionIndex + 1)
                << (removedEmptyOriginalSequence ? ". Empty original sequence removed." : ".");

        setProjectDirty();
        logMessage(message);
    }

    void MainComponent::nudgeSequenceStartFromManager(double beatDelta)
    {
        rebuildSectionsFromTracksIfNeeded();

        auto sequenceNumber = trackManagerSectionBox.getText().getIntValue();

        if (sequenceNumber <= 0)
            sequenceNumber = activeImportSectionIndex + 1;

        if (sequenceNumber <= 0 || sequenceNumber > static_cast<int>(importSections.size()))
        {
            logMessage("Sequence Nudge: no valid sequence selected.");
            return;
        }

        auto& section = importSections[static_cast<std::size_t>(sequenceNumber - 1)];

        if (section.locked)
        {
            logMessage("Sequence Nudge: sequence is locked. Unlock it before nudging its start beat.");
            juce::AlertWindow::showMessageBoxAsync(
                juce::AlertWindow::WarningIcon,
                "Sequence Locked",
                "This sequence is locked. Unlock it before changing its start beat."
            );
            return;
        }

        const auto oldStartBeat = static_cast<double>(section.startTick) / mw::core::Project::ticksPerQuarterNote;
        const auto newStartBeat = std::max(0.0, oldStartBeat + beatDelta);

        trackManagerSectionBox.setText(juce::String(activeImportSectionIndex + 1), juce::dontSendNotification);
        trackManagerSectionStartBeatBox.setText(juce::String(newStartBeat, 2), juce::dontSendNotification);

        applySectionStartBeatFromManager();

        juce::String message;
        message << "Nudged sequence #"
                << sequenceNumber
                << " by "
                << (beatDelta >= 0.0 ? "+" : "")
                << beatDelta
                << " beat(s).";

        logMessage(message);
    }

    void MainComponent::applySectionStartBeatFromManager()
    {
        if (!currentProject)
        {
            logMessage("Sequence Editor: no project loaded.");
            return;
        }

        rebuildSectionsFromTracksIfNeeded();

        auto sectionNumber = trackManagerSectionBox.getText().getIntValue();

        if (sectionNumber <= 0)
        {
            updateActiveSequenceFromSelectedTrack();
            sectionNumber = activeImportSectionIndex + 1;
        }

        if (sectionNumber <= 0 || sectionNumber > static_cast<int>(importSections.size()))
        {
            logMessage("Sequence Editor: no active sequence for Apply Sequence Start.");
            return;
        }

        if (importSections[static_cast<std::size_t>(sectionNumber - 1)].locked)
        {
            logMessage("Sequence Editor: sequence is locked. Unlock it before changing its start beat.");
            return;
        }

        const auto newStartBeat = std::max(0.0, trackManagerSectionStartBeatBox.getText().getDoubleValue());
        const auto newStartTick = static_cast<std::int64_t>(std::llround(newStartBeat * mw::core::Project::ticksPerQuarterNote));

        auto& section = importSections[static_cast<std::size_t>(sectionNumber - 1)];
        const auto delta = newStartTick - section.startTick;

        if (delta == 0)
        {
            logMessage("Sequence Editor: section already starts at requested beat.");
            return;
        }

        captureTrackManagerUndoState("Apply Sequence Start");

        auto& tracks = currentProject->getTracks();

        for (const auto trackNumber : section.trackNumbers)
        {
            const auto trackIndex = trackNumber - 1;

            if (trackIndex < 0 || trackIndex >= static_cast<int>(tracks.size()))
                continue;

            for (auto& note : tracks[static_cast<std::size_t>(trackIndex)].getNotes())
            {
                if (note.startTick >= section.startTick && note.startTick < section.endTick)
                    note.startTick = std::max<std::int64_t>(0, note.startTick + delta);
            }
        }

        section.startTick = std::max<std::int64_t>(0, section.startTick + delta);
        section.endTick = std::max(section.startTick, section.endTick + delta);
        activeImportSectionIndex = sectionNumber - 1;
        trackManagerSectionStartBeatBox.setText(juce::String(newStartBeat, 2), juce::dontSendNotification);

        if (sequenceMapBeatWindow > 0)
        {
            std::int64_t maxTick = getProjectEndTick();
            for (const auto& visibleSection : importSections)
                maxTick = std::max(maxTick, visibleSection.endTick);

            const int fullProjectBeats = std::max(4, static_cast<int>(std::ceil(static_cast<double>(std::max<std::int64_t>(maxTick, mw::core::Project::ticksPerQuarterNote)) / mw::core::Project::ticksPerQuarterNote)));
            sequenceMapStartBeat = std::clamp(static_cast<int>(std::floor(newStartBeat)), 0, std::max(0, fullProjectBeats - sequenceMapBeatWindow));
            trackManagerMapStartBeatBox.setText(juce::String(sequenceMapStartBeat), juce::dontSendNotification);
        }

        refreshNoteEditor();
        syncPianoRollFromSelectedTrack();
        fitPianoRollToSelectedTrack();
        updateTrackSummary(*currentProject);
        refreshTrackManagerText();

        if (trackManagerContent != nullptr)
        {
            if (auto* content = dynamic_cast<TrackManagerWindowContent*>(trackManagerContent.get()))
                content->refreshSequenceMap();
            else
                trackManagerContent->repaint();
        }

        juce::String message;
        message << "Sequence Editor shifted sequence #"
                << sectionNumber
                << " to beat "
                << juce::String(newStartBeat, 2)
                << ".";

        setProjectDirty();
        logMessage(message);
    }

    std::optional<std::pair<int, juce::String>> MainComponent::createBlankSequenceOnly(const juce::String& actionLabel, bool focusNewSequence)
    {
        if (!currentProject)
        {
            currentProject = mw::core::Project("Manual Project");
            baseNameBox.setText("manual_project");
            captureProjectUserSettings();

            // If the user picked a project-default VST3 backend/plugin before any
            // tracks existed, persist that visible selection into the new project
            // before the first manual track or sequence workflow seeds from it.
            // Otherwise the backend could stay VST3 while the instrument selection
            // appeared to jump back to the first scanned plugin.
            if (appliedProjectBackendId == 3)
                captureProjectDefaultVstPluginSelection();
        }

        if (static_cast<int>(importSections.size()) >= mw::core::Project::maxSequenceCount)
        {
            logMessage("Sequence limit reached: " + juce::String(mw::core::Project::maxSequenceCount) + ". Create Blank Seq was not applied.");
            return std::nullopt;
        }

        const int previousActiveSequenceIndex = activeImportSectionIndex;

        captureTrackManagerUndoState(actionLabel.isEmpty() ? juce::String("Create Blank Seq") : actionLabel);

        ImportSectionInfo section;
        section.id = allocateNextSequenceId();
        section.name = makeDefaultSequenceName(std::filesystem::path{}, "Manual");
        section.sourcePath = std::filesystem::path{};
        section.startTick = 0;
        section.endTick = 0;
        section.trackNumbers.clear();
        section.isLayer = false;
        section.createdBy = "manual";

        importSections.push_back(std::move(section));
        const int newSequenceNumber = static_cast<int>(importSections.size());
        const auto newSequenceName = importSections.back().name;

        sequenceMapFirstIndex = std::max(0, static_cast<int>(importSections.size()) - 8);

        if (focusNewSequence)
        {
            activeImportSectionIndex = newSequenceNumber - 1;
            trackManagerSectionBox.setText(juce::String(newSequenceNumber), juce::dontSendNotification);
            trackManagerSectionStartBeatBox.setText("0.00", juce::dontSendNotification);
        }
        else
        {
            activeImportSectionIndex = previousActiveSequenceIndex;
        }

        normalizeEmptySequencesAfterMembershipChange();
        syncSequencesToProjectMetadata();

        if (currentProject)
            updateTrackSummary(*currentProject);

        refreshTrackManagerText(focusNewSequence);
        refreshMainSequenceSelector();
        refreshActiveSequenceThoughtsEditor();
        updateRenderTargetLabel();

        if (trackManagerContent != nullptr)
        {
            if (auto* content = dynamic_cast<TrackManagerWindowContent*>(trackManagerContent.get()))
                content->refreshSequenceMap();
            else
                trackManagerContent->repaint();
        }

        setProjectDirty();

        logMessage(
            "Created blank sequence #" + juce::String(newSequenceNumber)
            + " - " + newSequenceName
            + " with tracks: (empty)."
        );

        return std::make_pair(newSequenceNumber, newSequenceName);
    }

    bool MainComponent::trackPassesManagerFilter(int trackIndex) const
    {
        if (!currentProject)
            return false;

        if (trackIndex < 0 || trackIndex >= static_cast<int>(currentProject->getTracks().size()))
            return false;

        const auto& track = currentProject->getTracks()[static_cast<std::size_t>(trackIndex)];

        switch (trackManagerFilterId)
        {
            case 2: // Current sequence
            {
                if (activeImportSectionIndex < 0 || activeImportSectionIndex >= static_cast<int>(importSections.size()))
                    return false;

                const auto trackNumber = trackIndex + 1;
                const auto& sequence = importSections[static_cast<std::size_t>(activeImportSectionIndex)];

                return std::find(sequence.trackNumbers.begin(), sequence.trackNumbers.end(), trackNumber) != sequence.trackNumbers.end();
            }
            case 3:
                return track.getMuted();
            case 4:
                return track.getSolo();
            case 5:
                return track.getInstrument().backendType == mw::core::SampleBackendType::SF2;
            case 6:
                return track.getInstrument().backendType == mw::core::SampleBackendType::SFZ;
            case 7:
                return track.getInstrument().backendType == mw::core::SampleBackendType::None;
            default:
                return true;
        }
    }

    juce::String MainComponent::formatSequenceTrackSummary(int sequenceIndex) const
    {
        if (sequenceIndex < 0 || sequenceIndex >= static_cast<int>(importSections.size()))
            return "(empty)";

        const auto& section = importSections[static_cast<std::size_t>(sequenceIndex)];
        juce::String tracksText;

        for (int t = 0; t < static_cast<int>(section.trackNumbers.size()); ++t)
        {
            const auto trackNumber = section.trackNumbers[static_cast<std::size_t>(t)];

            if (trackNumber <= 0)
                continue;

            if (tracksText.isNotEmpty())
                tracksText << ", ";

            tracksText << "#" << trackNumber;
        }

        return tracksText.isEmpty() ? juce::String("(empty)") : tracksText;
    }

    void MainComponent::syncTrackManagerSelectionFromCurrentTrack(bool refreshConsole, bool jumpToTrack)
    {
        if (!currentProject)
            return;

        normalizeEmptySequencesAfterMembershipChange();
        syncSequencesToProjectMetadata();

        const int selectedTrackIndex = getSelectedTrackIndex();
        int selectedTrackNumber = 0;

        if (selectedTrackIndex >= 0 && selectedTrackIndex < static_cast<int>(currentProject->getTracks().size()))
        {
            selectedTrackNumber = selectedTrackIndex + 1;
            trackManagerSelectBox.setText(juce::String(selectedTrackNumber), juce::dontSendNotification);

            const int sequenceIndex = getSequenceIndexForTrack(selectedTrackNumber);
            if (sequenceIndex >= 0 && sequenceIndex < static_cast<int>(importSections.size()))
            {
                activeImportSectionIndex = sequenceIndex;
                const auto& section = importSections[static_cast<std::size_t>(activeImportSectionIndex)];
                trackManagerSectionBox.setText(juce::String(activeImportSectionIndex + 1), juce::dontSendNotification);
                trackManagerSectionStartBeatBox.setText(
                    juce::String(static_cast<double>(section.startTick) / mw::core::Project::ticksPerQuarterNote, 2),
                    juce::dontSendNotification
                );
            }
        }

        refreshMainSequenceSelector();
        refreshActiveSequenceThoughtsEditor();
        updateRenderTargetLabel();
        updateTrackSummary(*currentProject);

        if (refreshConsole)
        {
            // Rebuild, reset the rendered console entries, and then scroll to the
            // selected track.  Preserving scroll here made the console look stale
            // after track dropdown changes even when the text had been rebuilt.
            refreshTrackManagerText(true);

            if (jumpToTrack && selectedTrackNumber > 0)
                trackManagerBox.scrollToFirstTrackNumber(selectedTrackNumber);
        }

        if (trackManagerContent != nullptr)
        {
            if (auto* content = dynamic_cast<TrackManagerWindowContent*>(trackManagerContent.get()))
                content->refreshSequenceMap();
            else
                trackManagerContent->repaint();
        }
    }

    void MainComponent::refreshSequenceMembershipDisplays(int sourceSequenceNumber, int targetSequenceNumber, bool scrollToSourceSequence)
    {
        normalizeEmptySequencesAfterMembershipChange();
        syncSequencesToProjectMetadata();

        if (currentProject)
            updateTrackSummary(*currentProject);

        refreshMainSequenceSelector();
        refreshActiveSequenceThoughtsEditor();
        updateRenderTargetLabel();

        // Force the shared Track Manager console component to rebuild from the
        // canonical importSections membership list instead of preserving a stale
        // rendered line list or a stale scrollbar page.  The same component is
        // shown in the main UI and inside the Track Manager window, so this keeps
        // both views in sync.
        refreshTrackManagerText(true);

        if (scrollToSourceSequence && sourceSequenceNumber > 0)
            trackManagerBox.scrollToFirstSequenceNumber(sourceSequenceNumber);
        else if (targetSequenceNumber > 0)
            trackManagerBox.scrollToFirstSequenceNumber(targetSequenceNumber);

        if (trackManagerContent != nullptr)
        {
            if (auto* content = dynamic_cast<TrackManagerWindowContent*>(trackManagerContent.get()))
                content->refreshSequenceMap();
            else
                trackManagerContent->repaint();
        }

        if (sequencePickerWindow != nullptr)
        {
            std::vector<SequencePickerChoice> choices;
            choices.reserve(importSections.size());

            for (int i = 0; i < static_cast<int>(importSections.size()); ++i)
            {
                const auto& section = importSections[static_cast<std::size_t>(i)];
                SequencePickerChoice choice;
                choice.number = i + 1;
                choice.name = section.name;
                choice.notes = section.notes;
                choice.locked = section.locked;
                choice.trackCount = static_cast<int>(section.trackNumbers.size());
                choice.trackSummary = formatSequenceTrackSummary(i);
                choices.push_back(std::move(choice));
            }

            const int preferredSelection = targetSequenceNumber > 0 ? targetSequenceNumber : sourceSequenceNumber;

            if (auto* pickerContent = dynamic_cast<SequencePickerContent*>(sequencePickerWindow->getContentComponent()))
                pickerContent->replaceChoices(std::move(choices), preferredSelection);
            else
                sequencePickerWindow->repaint();
        }
    }

    void MainComponent::refreshTrackManagerText(bool jumpToActiveSequence)
    {
        juce::String text;

        auto pad = [](juce::String value, int width)
        {
            value = value.replaceCharacter('\n', ' ').replaceCharacter('\r', ' ');

            if (value.length() > width)
                value = value.substring(0, std::max(0, width - 3)) + "...";

            while (value.length() < width)
                value << " ";

            return value;
        };

        auto right = [](juce::String value, int width)
        {
            value = value.replaceCharacter('\n', ' ').replaceCharacter('\r', ' ');

            if (value.length() > width)
                value = value.substring(0, std::max(0, width - 3)) + "...";

            while (value.length() < width)
                value = " " + value;

            return value;
        };

        auto repeat = [](const juce::String& value, int count)
        {
            juce::String result;

            for (int i = 0; i < count; ++i)
                result << value;

            return result;
        };

        auto divider = [&repeat](int width)
        {
            juce::String line;
            line << repeat("=", width) << "\n";
            return line;
        };

        auto subDivider = [&repeat](int width)
        {
            juce::String line;
            line << repeat("-", width) << "\n";
            return line;
        };

        if (!currentProject)
        {
            text << "No project loaded.\n";
            trackManagerBox.setSequenceColours({});
            trackManagerBox.setTextPreservingScroll(text, juce::dontSendNotification);

            if (trackManagerContent != nullptr)
                trackManagerContent->repaint();

            return;
        }

        normalizeEmptySequencesAfterMembershipChange();
        syncSequencesToProjectMetadata();

        const auto& tracks = currentProject->getTracks();

        constexpr int pageWidth = 112;
        constexpr int labelWidth = 15;

        text << divider(pageWidth);
        text << "Poor Man's Studio - Track Manager\n";
        int visibleTrackCount = 0;

        for (int i = 0; i < static_cast<int>(tracks.size()); ++i)
        {
            if (trackPassesManagerFilter(i))
                ++visibleTrackCount;
        }

        juce::String filterName = "All";
        if (trackManagerFilterId == 2) filterName = "Current Seq";
        else if (trackManagerFilterId == 3) filterName = "Muted";
        else if (trackManagerFilterId == 4) filterName = "Soloed";
        else if (trackManagerFilterId == 5) filterName = "SF2";
        else if (trackManagerFilterId == 6) filterName = "SFZ";
        else if (trackManagerFilterId == 7) filterName = "Project Backend";

        text << "Project : " << currentProject->getName() << "\n";
        text << "Tracks  : " << visibleTrackCount << " shown / " << static_cast<int>(tracks.size()) << " total\n";
        text << "Filter  : " << filterName << "\n";
        text << divider(pageWidth);
        text << "\n";

        const int selectedTrackIndex = getSelectedTrackIndex();
        if (selectedTrackIndex >= 0 && selectedTrackIndex < static_cast<int>(tracks.size()))
        {
            const auto& selectedTrack = tracks[static_cast<std::size_t>(selectedTrackIndex)];
            const int selectedSeq = getSequenceIndexForTrack(selectedTrackIndex + 1) + 1;
            text << "SELECTED TRACK CONTEXT\n";
            text << subDivider(pageWidth);
            text << "Track #" << (selectedTrackIndex + 1)
                 << "  Seq #" << selectedSeq
                 << "  " << selectedTrack.getName()
                 << "  Type: " << mw::core::trackTypeToString(selectedTrack.getTrackType()).c_str()
                 << "  Notes: " << static_cast<int>(selectedTrack.getNotes().size())
                 << "\n";
            if (selectedTrack.isAudioClipTrack())
                text << getAudioClipSummaryForTrack(selectedTrackIndex);
            text << "Track details live in the main Track Inspector. This console is sequence-focused.\n";
            text << subDivider(pageWidth);
            text << "\n";
        }

        rebuildSectionsFromTracksIfNeeded();

        if (activeImportSectionIndex < 0 || activeImportSectionIndex >= static_cast<int>(importSections.size()))
            updateActiveSequenceFromSelectedTrack();

        if (activeImportSectionIndex >= 0 && activeImportSectionIndex < static_cast<int>(importSections.size()))
        {
            const auto& active = importSections[static_cast<std::size_t>(activeImportSectionIndex)];
            trackManagerSectionBox.setText(juce::String(activeImportSectionIndex + 1), juce::dontSendNotification);
            trackManagerSectionStartBeatBox.setText(juce::String(static_cast<double>(active.startTick) / mw::core::Project::ticksPerQuarterNote, 2), juce::dontSendNotification);

            const auto startBeat = static_cast<double>(active.startTick) / mw::core::Project::ticksPerQuarterNote;
            const auto endBeat = static_cast<double>(active.endTick) / mw::core::Project::ticksPerQuarterNote;
            const auto lengthBeat = std::max(0.0, endBeat - startBeat);

            text << "ACTIVE SEQUENCE CONTEXT\n";
            text << subDivider(pageWidth);
            text << "Seq # : " << (activeImportSectionIndex + 1) << "\n";
            text << "Seq ID: " << active.id << "\n";
            text << "State : " << (active.locked ? "LOCKED" : "UNLOCKED") << "\n";
            text << "Name  : " << active.name << "\n";
            text << "Start : " << juce::String(startBeat, 2) << " beat    End: " << juce::String(endBeat, 2) << " beat    Length: " << juce::String(lengthBeat, 2) << " beats\n";
            if (!active.notes.trim().isEmpty())
                text << "Thoughts: " << active.notes.trim() << "\n";
            text << getAudioClipSummaryForSequence(activeImportSectionIndex + 1);
            text << subDivider(pageWidth);
            text << "\n";
        }

        text << "SEQUENCES\n";
        text << subDivider(pageWidth);

        if (importSections.empty())
        {
            text << "(no sequences yet)\n";
        }
        else
        {
            for (int i = 0; i < static_cast<int>(importSections.size()); ++i)
            {
                const auto& section = importSections[static_cast<std::size_t>(i)];

                const auto tracksText = formatSequenceTrackSummary(i);

                const auto startBeat = static_cast<double>(section.startTick) / mw::core::Project::ticksPerQuarterNote;
                const auto endBeat = static_cast<double>(section.endTick) / mw::core::Project::ticksPerQuarterNote;
                const auto lengthBeat = std::max(0.0, endBeat - startBeat);

                text << "Seq #" << right(juce::String(i + 1), 3)
                     << "  "
                     << (section.locked ? "[LOCKED] " : "[UNLOCKED] ")
                     << section.name
                     << "  (ID " << section.id << ")"
                     << "\n";

                text << "      "
                     << pad("Start", labelWidth) << ": " << right(juce::String(startBeat, 2), 8)
                     << "    "
                     << pad("End", labelWidth) << ": " << right(juce::String(endBeat, 2), 8)
                     << "    "
                     << pad("Length", labelWidth) << ": " << right(juce::String(lengthBeat, 2), 8)
                     << "\n";

                text << "      "
                     << pad("Tracks", labelWidth) << ": "
                     << tracksText
                     << "\n";

                if (!section.notes.isEmpty())
                    text << "      " << pad("Notes", labelWidth) << ": " << section.notes << "\n";
                text << getAudioClipSummaryForSequence(i + 1);

                text << subDivider(pageWidth);
            }
        }


        text << "\n";
        text << "Tips\n";
        text << "----\n";
        text << "- Enter a track number in Track # and click Select Track to jump to it.\n";
        text << "- Use Track Start + Apply Start Beat to shift one selected track.\n";
        text << "- Use Seq # + Sequence Start + Apply Sequence Start to shift a sequence.\n";
        text << "- Use Create Blank Seq to create a new empty manual sequence; use Add Blank Track when you need a new track.\n";
        text << "- Use Change Track Seq to move a track to an existing sequence, or use the popup's Create Blank Seq button to add an empty target sequence.\n";
        text << "- Remove Track deletes the selected track; Remove Seq deletes the selected sequence and its tracks after confirmation. If a Piano Roll is open for that sequence, close it first.\n";
        text << "- Preview Project plays a temporary full-project preview from Track Manager.\n- Undo restores the previous Track Manager arrangement state.\n";
        text << "- The visual Sequence Map above the table shows colored blocks for sequence placement; sequence colors avoid black/near-black so console rails stay visible.\n";
        text << "- Use the Track Filter dropdown to narrow the console view without changing the project.\n";
        text << "- Use the main Track Inspector to change backend/instrument details.\n- Default sequence names reuse gaps without renumbering existing sequences.\n";

        {
            std::vector<juce::Colour> colours;
            colours.reserve(importSections.size());
            for (int i = 0; i < static_cast<int>(importSections.size()); ++i)
                colours.push_back(getSequenceColourForIndex(i));
            trackManagerBox.setSequenceColours(colours);
        }

        if (jumpToActiveSequence)
            trackManagerBox.setText(text, juce::dontSendNotification);
        else
            trackManagerBox.setTextPreservingScroll(text, juce::dontSendNotification);

        if (jumpToActiveSequence && activeImportSectionIndex >= 0)
            trackManagerBox.scrollToFirstSequenceNumber(activeImportSectionIndex + 1);

        if (trackManagerContent != nullptr)
        {
            if (auto* content = dynamic_cast<TrackManagerWindowContent*>(trackManagerContent.get()))
                content->refreshSequenceMap();
            else
                trackManagerContent->repaint();
        }
    }

void MainComponent::selectTrackFromManagerPage()
    {
        if (!currentProject)
            return;

        auto trackNumber = trackManagerSelectBox.getText().getIntValue();

        if (trackNumber <= 0)
            trackNumber = 1;

        const auto maxTrack = static_cast<int>(currentProject->getTracks().size());
        trackNumber = std::clamp(trackNumber, 1, std::max(1, maxTrack));

        suppressTrackManagerConsoleFollow = true;
        trackCombo.setSelectedId(trackNumber, juce::sendNotification);
        suppressTrackManagerConsoleFollow = false;
        updateActiveSequenceFromSelectedTrack();
        updateRenderTargetLabel();
        if (currentProject)
            updateTrackSummary(*currentProject);
        refreshTrackManagerText(false);
        trackManagerBox.scrollToFirstTrackNumber(trackNumber);
        if (trackManagerContent != nullptr)
            trackManagerContent->repaint();

        juce::String message;
        message << "Track Manager selected track #" << trackNumber;
        logMessage(message);
    }

    void MainComponent::selectSequenceFromMap(int sequenceIndex, bool focusFirstTrack)
    {
        rebuildSectionsFromTracksIfNeeded();

        if (sequenceIndex < 0 || sequenceIndex >= static_cast<int>(importSections.size()))
            return;

        activeImportSectionIndex = sequenceIndex;

        const auto& sequence = importSections[static_cast<std::size_t>(sequenceIndex)];
        trackManagerSectionBox.setText(juce::String(sequenceIndex + 1), juce::dontSendNotification);
        trackManagerSectionStartBeatBox.setText(juce::String(static_cast<double>(sequence.startTick) / mw::core::Project::ticksPerQuarterNote, 2), juce::dontSendNotification);

        if (focusFirstTrack && !sequence.trackNumbers.empty())
        {
            const auto firstTrack = sequence.trackNumbers.front();
            trackManagerSelectBox.setText(juce::String(firstTrack), juce::dontSendNotification);

            if (currentProject && firstTrack >= 1 && firstTrack <= static_cast<int>(currentProject->getTracks().size()))
            {
                trackCombo.setSelectedId(firstTrack, juce::sendNotification);
                syncTrackInspectorFromSelection();
                refreshNoteEditor();
                syncPianoRollFromSelectedTrack();
                fitPianoRollToSelectedTrack();
            }
        }

        refreshMainSequenceSelector();
        updateRenderTargetLabel();

        if (currentProject)
            updateTrackSummary(*currentProject);

        refreshTrackManagerText();

        if (trackManagerContent != nullptr)
        {
            if (auto* content = dynamic_cast<TrackManagerWindowContent*>(trackManagerContent.get()))
                content->refreshSequenceMap();
            else
                trackManagerContent->repaint();
        }

        juce::String message;
        message << "Sequence Map selected sequence #"
                << (sequenceIndex + 1);

        if (focusFirstTrack && !sequence.trackNumbers.empty())
            message << " and focused track #" << sequence.trackNumbers.front();

        message << ".";

        logMessage(message);
    }

    void MainComponent::selectSequenceFromManagerPage()
    {
        rebuildSectionsFromTracksIfNeeded();

        auto sequenceNumber = trackManagerSectionBox.getText().getIntValue();

        if (sequenceNumber <= 0)
            sequenceNumber = activeImportSectionIndex + 1;

        if (sequenceNumber <= 0 || sequenceNumber > static_cast<int>(importSections.size()))
        {
            logMessage("Track Manager: invalid sequence number.");
            return;
        }

        activeImportSectionIndex = sequenceNumber - 1;

        const auto& sequence = importSections[static_cast<std::size_t>(activeImportSectionIndex)];
        trackManagerSectionBox.setText(juce::String(activeImportSectionIndex + 1), juce::dontSendNotification);
        trackManagerSectionStartBeatBox.setText(juce::String(static_cast<double>(sequence.startTick) / mw::core::Project::ticksPerQuarterNote, 2), juce::dontSendNotification);

        int focusedTrack = 0;
        if (!sequence.trackNumbers.empty())
        {
            focusedTrack = sequence.trackNumbers.front();
            trackManagerSelectBox.setText(juce::String(focusedTrack), juce::dontSendNotification);

            if (currentProject && focusedTrack >= 1 && focusedTrack <= static_cast<int>(currentProject->getTracks().size()))
            {
                trackCombo.setSelectedId(focusedTrack, juce::sendNotification);
                syncTrackInspectorFromSelection();
                refreshNoteEditor();
                syncPianoRollFromSelectedTrack();
                fitPianoRollToSelectedTrack();
            }
        }

        const int visibleCapacityHint = 8;
        sequenceMapFirstIndex = std::clamp(
            activeImportSectionIndex - visibleCapacityHint / 2,
            0,
            std::max(0, static_cast<int>(importSections.size()) - visibleCapacityHint)
        );

        if (sequenceMapBeatWindow > 0)
        {
            const auto selectedStartBeat = static_cast<int>(std::floor(static_cast<double>(sequence.startTick) / mw::core::Project::ticksPerQuarterNote));
            const int maxBeat = std::max(
                sequenceMapBeatWindow,
                static_cast<int>(std::ceil(static_cast<double>(std::max<std::int64_t>(getProjectEndTick(), mw::core::Project::ticksPerQuarterNote)) / mw::core::Project::ticksPerQuarterNote))
            );
            sequenceMapStartBeat = std::clamp(selectedStartBeat, 0, std::max(0, maxBeat - sequenceMapBeatWindow));
            trackManagerMapStartBeatBox.setText(juce::String(sequenceMapStartBeat), juce::dontSendNotification);
        }

        refreshMainSequenceSelector();
        updateRenderTargetLabel();

        if (currentProject)
            updateTrackSummary(*currentProject);

        refreshTrackManagerText();

        if (trackManagerContent != nullptr)
        {
            if (auto* content = dynamic_cast<TrackManagerWindowContent*>(trackManagerContent.get()))
                content->refreshSequenceMap();
            else
                trackManagerContent->repaint();
        }

        juce::String message;
        message << "Track Manager selected sequence #"
                << sequenceNumber
                << " - "
                << sequence.name;

        if (focusedTrack > 0)
            message << " and focused track #" << focusedTrack;

        message << ".";

        logMessage(message);
    }

    void MainComponent::openSelectedTrackInPianoRollFromManager()
    {
        selectTrackFromManagerPage();
        openPianoRollWindow();
    }


    void MainComponent::applyTrackStartBeatFromManager()
    {
        if (!currentProject)
        {
            logMessage("Track Manager: no project loaded.");
            return;
        }

        auto trackNumber = trackManagerSelectBox.getText().getIntValue();

        if (trackNumber <= 0)
            trackNumber = trackCombo.getSelectedId();

        const auto maxTrack = static_cast<int>(currentProject->getTracks().size());

        if (trackNumber <= 0 || trackNumber > maxTrack)
        {
            logMessage("Track Manager: invalid track number for Apply Start Beat.");
            return;
        }

        auto targetBeat = trackManagerStartBeatBox.getText().getIntValue();
        targetBeat = std::max(0, targetBeat);

        auto& track = currentProject->getTracks()[static_cast<std::size_t>(trackNumber - 1)];
        auto& notes = track.getNotes();

        if (notes.empty())
        {
            logMessage("Track Manager: selected track has no notes to shift.");
            return;
        }

        std::int64_t earliestTick = notes.front().startTick;

        for (const auto& note : notes)
            earliestTick = std::min(earliestTick, note.startTick);

        const auto targetTick =
            static_cast<std::int64_t>(targetBeat) * mw::core::Project::ticksPerQuarterNote;

        const auto delta = targetTick - earliestTick;

        for (auto& note : notes)
            note.startTick = std::max<std::int64_t>(0, note.startTick + delta);

        trackCombo.setSelectedId(trackNumber, juce::sendNotification);
        refreshNoteEditor();
        syncPianoRollFromSelectedTrack();
        fitPianoRollToSelectedTrack();
        updateTrackSummary(*currentProject);
        refreshTrackManagerText();

        juce::String message;
        message << "Track Manager shifted track #"
                << trackNumber
                << " ("
                << track.getName()
                << ") to start at beat "
                << targetBeat
                << ".";

        setProjectDirty();
        logMessage(message);
    }

    void MainComponent::duplicateTrackToStartBeatFromManager()
    {
        if (!currentProject)
        {
            logMessage("Track Manager: no project loaded.");
            return;
        }

        auto sourceTrackNumber = trackManagerSelectBox.getText().getIntValue();

        if (sourceTrackNumber <= 0)
            sourceTrackNumber = trackCombo.getSelectedId();

        const auto maxTrack = static_cast<int>(currentProject->getTracks().size());

        if (sourceTrackNumber <= 0 || sourceTrackNumber > maxTrack)
        {
            logMessage("Track Manager: invalid track number for Duplicate Beat.");
            return;
        }

        auto targetBeat = trackManagerStartBeatBox.getText().getIntValue();
        targetBeat = std::max(0, targetBeat);

        const auto sourceIndex = static_cast<std::size_t>(sourceTrackNumber - 1);
        auto duplicated = currentProject->getTracks()[sourceIndex];
        duplicated.setName(duplicated.getName() + " @ beat " + std::to_string(targetBeat));

        auto& notes = duplicated.getNotes();

        if (!notes.empty())
        {
            std::int64_t earliestTick = notes.front().startTick;

            for (const auto& note : notes)
                earliestTick = std::min(earliestTick, note.startTick);

            const auto targetTick =
                static_cast<std::int64_t>(targetBeat) * mw::core::Project::ticksPerQuarterNote;

            const auto delta = targetTick - earliestTick;

            for (auto& note : notes)
                note.startTick = std::max<std::int64_t>(0, note.startTick + delta);
        }

        if (!canAddAnotherTrack("Duplicate Track To Beat"))
            return;

        currentProject->getTracks().push_back(std::move(duplicated));
        refreshTrackSelector();

        const auto newTrackNumber = static_cast<int>(currentProject->getTracks().size());
        trackCombo.setSelectedId(newTrackNumber, juce::sendNotification);
        trackManagerSelectBox.setText(juce::String(newTrackNumber), juce::dontSendNotification);

        refreshNoteEditor();
        syncPianoRollFromSelectedTrack();
        fitPianoRollToSelectedTrack();
        updateTrackSummary(*currentProject);
        refreshTrackManagerText();

        juce::String message;
        message << "Track Manager duplicated track #"
                << sourceTrackNumber
                << " to new track #"
                << newTrackNumber
                << " at beat "
                << targetBeat
                << ".";

        logMessage(message);
    }


        void MainComponent::applyProjectInfoToGuiAndProject()
    {
        const auto title = metadataTitleBox.getText().trim();

        if (!title.isEmpty())
            baseNameBox.setText(title, juce::dontSendNotification);

        if (currentProject)
        {
            auto& settings = currentProject->getUserSettings();
            settings.metadataTitle = metadataTitleBox.getText().trim().toStdString();
            settings.metadataArtist = metadataArtistBox.getText().trim().toStdString();
            settings.metadataAlbum = metadataAlbumBox.getText().trim().toStdString();
            settings.metadataTrackNumber = metadataTrackNumberBox.getText().trim().toStdString();
            settings.metadataYear = metadataYearBox.getText().trim().toStdString();

            if (!title.isEmpty())
            {
                currentProject->setName(title.toStdString());
                settings.baseFileName = title.toStdString();
            }
            else
            {
                settings.baseFileName = baseNameBox.getText().toStdString();
            }

            updateTrackSummary(*currentProject);
            refreshTrackManagerText();
            setProjectDirty();
        }

        logMessage("Applied project info. Blank metadata fields will be skipped during tagging.");
    }

    void MainComponent::openProjectInfoWindow()
    {
        if (currentProject)
        {
            const auto& settings = currentProject->getUserSettings();

            if (metadataTitleBox.getText().isEmpty())
                metadataTitleBox.setText(settings.metadataTitle, juce::dontSendNotification);

            if (metadataArtistBox.getText().isEmpty())
                metadataArtistBox.setText(settings.metadataArtist, juce::dontSendNotification);

            if (metadataAlbumBox.getText().isEmpty())
                metadataAlbumBox.setText(settings.metadataAlbum, juce::dontSendNotification);

            if (metadataTrackNumberBox.getText().isEmpty())
                metadataTrackNumberBox.setText(settings.metadataTrackNumber, juce::dontSendNotification);

            if (metadataYearBox.getText().isEmpty())
                metadataYearBox.setText(settings.metadataYear, juce::dontSendNotification);
        }

        if (metadataTitleBox.getText().isEmpty() && !baseNameBox.getText().isEmpty())
            metadataTitleBox.setText(baseNameBox.getText(), juce::dontSendNotification);

        if (projectInfoWindow != nullptr)
        {
            projectInfoWindow->toFront(true);
            return;
        }

        auto closeWindow = [this]
        {
            if (projectInfoWindow != nullptr)
            {
                projectInfoWindow->setVisible(false);
                projectInfoWindow->setContentOwned(nullptr, false);

                juce::MessageManager::callAsync(
                    [this]
                    {
                        projectInfoWindow.reset();
                        projectInfoContent.reset();
                    }
                );
            }
        };

        auto applyInfo = [this]
        {
            applyProjectInfoToGuiAndProject();
        };

        projectInfoContent = std::make_unique<ProjectInfoWindowContent>(
            metadataTitleBox,
            metadataArtistBox,
            metadataAlbumBox,
            metadataTrackNumberBox,
            metadataYearBox,
            applyInfo,
            closeWindow
        );

        auto* window = new PianoRollDocumentWindow("Edit Info", closeWindow);
        applyPoorMansStudioCustomTitleBar(*window);
        window->setResizable(true, true);
        window->setResizeLimits(480, 300, 1200, 800);
        window->setContentNonOwned(projectInfoContent.get(), true);
        window->centreWithSize(620, 360);
        window->setVisible(true);
        applyPoorMansStudioWindowIcon(*window, PoorMansStudioWindowIcon::EditInfo);

        projectInfoWindow.reset(window);

        logMessage("Opened Edit Info window.");
    }

std::optional<mw::core::Project> MainComponent::importProjectFromPath(const std::filesystem::path& path)
    {
        auto extension = path.extension().string();

        for (auto& c : extension)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        std::optional<mw::core::Project> imported;

        if (extension == ".mid" || extension == ".midi")
            imported = mw::import_export::MidiImporter::importFromFile(path);
        else if (extension == ".musicxml" || extension == ".xml" || extension == ".mxl")
            imported = mw::import_export::MusicXmlImporter::importFromFile(path);
        else
        {
            logMessage("ERROR: Unsupported import file type: " + path.string());
            return std::nullopt;
        }

        if (!imported)
        {
            logMessage("ERROR: Failed to import: " + path.string());
            return std::nullopt;
        }

        if (!enforceProjectTrackLimit(*imported, juce::String(path.filename().string())))
            return std::nullopt;

        return imported;
    }

    std::int64_t MainComponent::getProjectEndTick() const
    {
        if (!currentProject)
            return 0;

        std::int64_t endTick = 0;

        for (const auto& track : currentProject->getTracks())
        {
            for (const auto& note : track.getNotes())
                endTick = std::max(endTick, note.startTick + note.durationTicks);
        }

        const auto tempo = static_cast<double>(currentProject->getTempoBpm());
        for (const auto& clip : currentProject->getAudioClips())
            endTick = std::max(endTick, audioClipEndTickForTempo(clip, tempo));

        return endTick;
    }

void MainComponent::refreshAfterMultiFileImport()
    {
        if (!currentProject)
            return;

        if (baseNameBox.getText().isEmpty() || baseNameBox.getText() == "rendered_score")
            baseNameBox.setText(currentProject->getName());

        const int postImportSequenceIndex = activeImportSectionIndex;
        int postImportFirstTrack = 0;

        if (postImportSequenceIndex >= 0 && postImportSequenceIndex < static_cast<int>(importSections.size()))
        {
            auto& sequence = importSections[static_cast<std::size_t>(postImportSequenceIndex)];
            std::sort(sequence.trackNumbers.begin(), sequence.trackNumbers.end());
            sequence.trackNumbers.erase(std::unique(sequence.trackNumbers.begin(), sequence.trackNumbers.end()), sequence.trackNumbers.end());

            if (!sequence.trackNumbers.empty())
                postImportFirstTrack = sequence.trackNumbers.front();

            trackManagerSectionBox.setText(juce::String(postImportSequenceIndex + 1), juce::dontSendNotification);
            trackManagerSectionStartBeatBox.setText(
                juce::String(static_cast<double>(sequence.startTick) / mw::core::Project::ticksPerQuarterNote, 2),
                juce::dontSendNotification
            );
        }

        refreshTrackSelector();

        if (postImportFirstTrack > 0 && postImportFirstTrack <= static_cast<int>(currentProject->getTracks().size()))
        {
            trackManagerSelectBox.setText(juce::String(postImportFirstTrack), juce::dontSendNotification);
            trackCombo.setSelectedId(postImportFirstTrack, juce::sendNotification);
        }

        syncTrackInspectorFromSelection();

        tempoBox.setText(juce::String(currentProject->getTempoBpm()), juce::dontSendNotification);
        pianoRollBpmBox.setText(juce::String(currentProject->getTempoBpm()), juce::dontSendNotification);

        juce::String ts;
        ts << currentProject->getTimeSignature().numerator << "/" << currentProject->getTimeSignature().denominator;
        timeSignatureBox.setText(ts, juce::dontSendNotification);
        pianoRollTimeSigBox.setText(ts, juce::dontSendNotification);

        refreshNoteEditor();
        fitPianoRollToSelectedTrack();
        updateTrackSummary(*currentProject);
        refreshTrackManagerText();
        syncPianoRollFromSelectedTrack();

        if (trackManagerContent != nullptr)
        {
            if (auto* content = dynamic_cast<TrackManagerWindowContent*>(trackManagerContent.get()))
                content->refreshSequenceMap();
            else
                trackManagerContent->repaint();
        }
    }

    void MainComponent::appendImportedProjectAsNewTracksInSequence(mw::core::Project importedProject, const std::filesystem::path& sourcePath, int targetSequenceIndex, std::int64_t startTick)
    {
        if (!currentProject)
        {
            currentProject = std::move(importedProject);
            captureProjectUserSettings();
            seedMissingTrackSoundLibrariesFromProjectDefaults(*currentProject);

            std::vector<int> trackNumbers;
            for (int i = 0; i < static_cast<int>(currentProject->getTracks().size()); ++i)
                trackNumbers.push_back(i + 1);

            recordImportSection(sourcePath.filename().string(), sourcePath, startTick, getProjectEndTick(), trackNumbers, false, "importSequence");
            return;
        }

        const bool addToExistingSequence = targetSequenceIndex >= 0 && targetSequenceIndex < static_cast<int>(importSections.size());

        if (!addToExistingSequence && static_cast<int>(importSections.size()) >= mw::core::Project::maxSequenceCount)
        {
            logMessage("Import Sequence: sequence limit reached (" + juce::String(mw::core::Project::maxSequenceCount) + "). Remove a sequence before importing a new one.");
            return;
        }

        if (addToExistingSequence && importSections[static_cast<std::size_t>(targetSequenceIndex)].locked)
        {
            logMessage("Import Sequence: target sequence is locked. Unlock it before adding imported tracks.");
            juce::AlertWindow::showMessageBoxAsync(
                juce::AlertWindow::WarningIcon,
                "Sequence Locked",
                "This sequence is locked. Unlock it before importing tracks into it."
            );
            return;
        }

        auto& destinationTracks = currentProject->getTracks();
        std::vector<int> addedTrackNumbers;
        std::int64_t sectionEndTick = startTick;

        for (auto importedTrack : importedProject.getTracks())
        {
            if (!canAddAnotherTrack("Import Sequence"))
            {
                logMessage("WARNING: Track limit reached while importing sequence file: " + sourcePath.filename().string());
                break;
            }

            for (auto& note : importedTrack.getNotes())
            {
                note.startTick += startTick;
                sectionEndTick = std::max(sectionEndTick, note.startTick + note.durationTicks);
            }

            if (shortenImportedTrackNames)
                importedTrack.setName(importedTrack.getName());
            else
                importedTrack.setName(sourcePath.stem().string() + " - " + importedTrack.getName());

            if (importedTrack.getInstrument().backendType == mw::core::SampleBackendType::None || importedTrack.getInstrument().sampleLibraryPath.empty())
                seedTrackSoundLibraryFromProjectDefaults(importedTrack);

            destinationTracks.push_back(std::move(importedTrack));
            addedTrackNumbers.push_back(static_cast<int>(destinationTracks.size()));
        }

        if (addToExistingSequence)
        {
            auto& target = importSections[static_cast<std::size_t>(targetSequenceIndex)];

            for (const auto trackNumber : addedTrackNumbers)
            {
                if (std::find(target.trackNumbers.begin(), target.trackNumbers.end(), trackNumber) == target.trackNumbers.end())
                    target.trackNumbers.push_back(trackNumber);
            }

            std::sort(target.trackNumbers.begin(), target.trackNumbers.end());
            target.startTick = std::min(target.startTick, startTick);
            target.endTick = std::max(target.endTick, sectionEndTick);
            activeImportSectionIndex = targetSequenceIndex;

            juce::String message;
            message << "Imported "
                    << sourcePath.filename().string()
                    << " into sequence #"
                    << (targetSequenceIndex + 1)
                    << ".";

            setProjectDirty();
            logMessage(message);
            return;
        }

        recordImportSection(sourcePath.filename().string(), sourcePath, startTick, sectionEndTick, addedTrackNumbers, false, "importSequence");
    }

    void MainComponent::importFilesAsSequence()
    {
        rebuildSectionsFromTracksIfNeeded();
        normalizeEmptySequencesAfterMembershipChange();
        syncSequencesToProjectMetadata();

        int selectedSequenceNumber = trackManagerSectionBox.getText().getIntValue();
        const int sequenceCount = static_cast<int>(importSections.size());

        // Add Sequence should honor the sequence currently focused in Track Manager.
        // Do not resync from the selected track here, because that can snap the
        // picker prompt back to the track's sequence (often Seq #1) even after the
        // user has selected a different sequence row/map block.
        if (selectedSequenceNumber < 1 || selectedSequenceNumber > sequenceCount)
        {
            if (activeImportSectionIndex >= 0 && activeImportSectionIndex < sequenceCount)
                selectedSequenceNumber = activeImportSectionIndex + 1;
            else if (sequenceCount > 0)
                selectedSequenceNumber = 1;
        }

        if (selectedSequenceNumber >= 1 && selectedSequenceNumber <= sequenceCount)
        {
            activeImportSectionIndex = selectedSequenceNumber - 1;
            const auto& selectedSequence = importSections[static_cast<std::size_t>(activeImportSectionIndex)];
            trackManagerSectionBox.setText(juce::String(selectedSequenceNumber), juce::dontSendNotification);
            trackManagerSectionStartBeatBox.setText(
                juce::String(static_cast<double>(selectedSequence.startTick) / mw::core::Project::ticksPerQuarterNote, 2),
                juce::dontSendNotification
            );
        }

        juce::String selectedSequenceName = "(none)";

        if (selectedSequenceNumber >= 1 && selectedSequenceNumber <= static_cast<int>(importSections.size()))
            selectedSequenceName = importSections[static_cast<std::size_t>(selectedSequenceNumber - 1)].name;

        auto launchChooser = [this](bool addToSelectedSequence, int targetSequenceNumber)
        {
            if (addToSelectedSequence
                && targetSequenceNumber >= 1
                && targetSequenceNumber <= static_cast<int>(importSections.size())
                && importSections[static_cast<std::size_t>(targetSequenceNumber - 1)].locked)
            {
                logMessage("Import Sequence: selected sequence is locked. Unlock it before importing into it.");
                juce::AlertWindow::showMessageBoxAsync(
                    juce::AlertWindow::WarningIcon,
                    "Sequence Locked",
                    "This sequence is locked. Unlock it before importing tracks into it."
                );
                return;
            }

            activeFileChooser = std::make_unique<juce::FileChooser>(
                "Import files as sequence",
                juce::File(mw::app::AppPaths::inputFolder().string()),
                "*.musicxml;*.xml;*.mxl;*.mid;*.midi"
            );

            activeFileChooser->launchAsync(
                juce::FileBrowserComponent::openMode
                    | juce::FileBrowserComponent::canSelectFiles
                    | juce::FileBrowserComponent::canSelectMultipleItems,
                [this, addToSelectedSequence, targetSequenceNumber](const juce::FileChooser& chooser)
                {
                    const auto files = chooser.getResults();

                    if (files.isEmpty())
                        return;

                    const auto requestedStartBeat = std::max(0.0, trackManagerSectionStartBeatBox.getText().getDoubleValue());
                    const auto requestedStartTick = static_cast<std::int64_t>(std::llround(requestedStartBeat * mw::core::Project::ticksPerQuarterNote));

                    for (const auto& file : files)
                    {
                        auto imported = importProjectFromPath(std::filesystem::path(file.getFullPathName().toStdString()));

                        if (!imported)
                            continue;

                        if (addToSelectedSequence && targetSequenceNumber >= 1 && targetSequenceNumber <= static_cast<int>(importSections.size()))
                        {
                            auto startTick = importSections[static_cast<std::size_t>(targetSequenceNumber - 1)].startTick;

                            if (requestedStartBeat > 0.0)
                                startTick = requestedStartTick;

                            appendImportedProjectAsNewTracksInSequence(
                                std::move(*imported),
                                std::filesystem::path(file.getFullPathName().toStdString()),
                                targetSequenceNumber - 1,
                                startTick
                            );
                        }
                        else
                        {
                            const auto startTick = requestedStartBeat > 0.0
                                ? requestedStartTick
                                : getProjectEndTick();

                            appendImportedProjectAsNewTracksInSequence(
                                std::move(*imported),
                                std::filesystem::path(file.getFullPathName().toStdString()),
                                -1,
                                startTick
                            );
                        }
                    }

                    refreshAfterMultiFileImport();
                    logMessage("Import Sequence complete.");
                }
            );
        };

        if (currentProject && !importSections.empty())
        {
            juce::String message;
            message << "Add imported file(s) to currently selected sequence #"
                    << selectedSequenceNumber
                    << " - "
                    << selectedSequenceName
                    << ", or create a new sequence?";

            auto* alert = new juce::AlertWindow(
                "Import Sequence",
                message,
                juce::AlertWindow::QuestionIcon
            );

            alert->addButton("Add to Seq #" + juce::String(selectedSequenceNumber), 1);
            alert->addButton("Create New Seq", 2);
            alert->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

            alert->enterModalState(
                true,
                juce::ModalCallbackFunction::create(
                    [this, alert, launchChooser, selectedSequenceNumber](int result)
                    {
                        std::unique_ptr<juce::AlertWindow> cleanup(alert);

                        if (result == 0)
                        {
                            logMessage("Import Sequence cancelled.");
                            return;
                        }

                        launchChooser(result == 1, selectedSequenceNumber);
                    }
                ),
                true
            );

            return;
        }

        launchChooser(false, -1);
    }


void MainComponent::openTrackManagerWindow()
    {
        refreshTrackManagerText();

        if (trackManagerWindow != nullptr)
        {
            trackManagerWindow->toFront(true);
            return;
        }

        auto closeWindow = [this]
        {
            closeTrackManagerWindowWithDirtyCheck();
        };

        auto refresh = [this]
        {
            refreshTrackManagerSessionSnapshotIfClean();
            refreshTrackManagerText();
            if (trackManagerContent != nullptr)
            {
                if (auto* content = dynamic_cast<TrackManagerWindowContent*>(trackManagerContent.get()))
                    content->refreshSequenceMap();
                else
                    trackManagerContent->repaint();
            }
        };

        auto select = [this]
        {
            selectTrackFromManagerPage();
        };

        auto selectSequence = [this]
        {
            selectSequenceFromManagerPage();
        };

        auto add = [this]
        {
            addManualTrack();
            refreshTrackManagerText();
            markTrackManagerEditorDirty("Add Blank Track");
        };

        auto duplicate = [this]
        {
            duplicateSelectedTrack();
            refreshTrackManagerText();
            markTrackManagerEditorDirty("Duplicate Track");
        };

        auto remove = [this]
        {
            removeSelectedTrack();
            refreshTrackManagerText();
            markTrackManagerEditorDirty("Remove Track");
        };

        auto removeSequence = [this]
        {
            removeSelectedSequenceFromManager();
        };

        auto undoTrackManager = [this]
        {
            undoTrackManagerEdit();
            refreshTrackManagerText();
            markTrackManagerEditorDirty("Undo Track Manager Edit");

            if (trackManagerContent != nullptr)
            {
                if (auto* content = dynamic_cast<TrackManagerWindowContent*>(trackManagerContent.get()))
                    content->refreshSequenceMap();
                else
                    trackManagerContent->repaint();
            }
        };

        auto applyStartBeat = [this]
        {
            applyTrackStartBeatFromManager();
            markTrackManagerEditorDirty("Apply Start Beat");
        };

        auto duplicateAtBeat = [this]
        {
            duplicateTrackToStartBeatFromManager();
            markTrackManagerEditorDirty("Duplicate Beat");
        };

        auto openInPianoRoll = [this]
        {
            openSelectedTrackInPianoRollFromManager();
        };

        auto previewTrackFromManager = [this]
        {
            previewSelectedTrackOnBackgroundThread();
        };

        auto previewSequenceFromManager = [this]
        {
            previewSelectedSequenceOnBackgroundThread();
        };

        auto previewProjectFromManager = [this]
        {
            previewCurrentProjectOnBackgroundThread();
        };

        auto startFromFileFromManager = [this]
        {
            chooseMusicXml();
        };

        auto importSequence = [this]
        {
            importFilesAsSequence();
            markTrackManagerEditorDirty("Add Sequence");
        };

        auto applySectionStart = [this]
        {
            applySectionStartBeatFromManager();
            markTrackManagerEditorDirty("Apply Sequence Start");
        };

        auto nudgeSequence = [this](double beatDelta)
        {
            nudgeSequenceStartFromManager(beatDelta);
            markTrackManagerEditorDirty("Nudge Sequence");
        };

        auto addBlankToSection = [this]
        {
            if (createBlankSequenceOnly("Create Blank Seq", true).has_value())
                markTrackManagerEditorDirty("Create Blank Seq");
        };

        auto linkTrackToSection = [this]
        {
            linkTrackToSectionFromManager();
            markTrackManagerEditorDirty("Link Track");
        };

        auto unlinkTrackFromSection = [this]
        {
            unlinkTrackFromSectionFromManager();
            markTrackManagerEditorDirty("Unlink Track");
        };

        auto moveTrackToSequence = [this]
        {
            moveTrackToSequenceFromManager();
            markTrackManagerEditorDirty("Change Track Seq");
        };

        auto changeSequenceColour = [this]
        {
            openSequenceColorEditorFromManager();
            markTrackManagerEditorDirty("Change Sequence Colour");
        };

        auto renameSequence = [this]
        {
            renameSequenceFromManager();
            markTrackManagerEditorDirty("Rename Sequence");
        };

        auto editSequenceNotes = [this]
        {
            editSequenceNotesFromManager();
        };

        auto toggleSequenceLock = [this]
        {
            toggleSequenceLockFromManager();
            markTrackManagerEditorDirty("Toggle Sequence Lock");
        };

        auto isSequenceLocked = [this]() -> bool
        {
            auto sequenceNumber = trackManagerSectionBox.getText().getIntValue();

            if (sequenceNumber <= 0)
                sequenceNumber = activeImportSectionIndex + 1;

            return sequenceNumber > 0
                && sequenceNumber <= static_cast<int>(importSections.size())
                && importSections[static_cast<std::size_t>(sequenceNumber - 1)].locked;
        };

        auto filterChanged = [this](int filterId)
        {
            trackManagerFilterId = filterId > 0 ? filterId : 1;
            refreshTrackManagerText();

            if (trackManagerContent != nullptr)
                trackManagerContent->repaint();
        };

        auto toggleNameMode = [this]
        {
            shortenImportedTrackNames = !shortenImportedTrackNames;
            logMessage(shortenImportedTrackNames
                ? "Import Sequence name mode: Short Names."
                : "Import Sequence name mode: Full Names.");
        };

        auto getShortNameMode = [this]() -> bool
        {
            return shortenImportedTrackNames;
        };

        if (trackManagerMapStartBeatBox.getText().isEmpty())
            trackManagerMapStartBeatBox.setText(juce::String(sequenceMapStartBeat), juce::dontSendNotification);
        if (trackManagerMapBeatWindowBox.getText().isEmpty())
            trackManagerMapBeatWindowBox.setText(sequenceMapBeatWindow <= 0 ? juce::String("Full") : juce::String(sequenceMapBeatWindow), juce::dontSendNotification);

        auto refreshMapView = [this]
        {
            if (trackManagerContent != nullptr)
            {
                if (auto* content = dynamic_cast<TrackManagerWindowContent*>(trackManagerContent.get()))
                    content->refreshSequenceMap();
                else
                    trackManagerContent->repaint();
            }
        };

        auto getSequenceMapMaxTick = [this]() -> std::int64_t
        {
            std::int64_t maxTick = getProjectEndTick();

            for (const auto& section : importSections)
                maxTick = std::max(maxTick, section.endTick);

            if (currentProject.has_value())
            {
                const auto tempo = static_cast<double>(currentProject->getTempoBpm());
                for (const auto& clip : currentProject->getAudioClips())
                    maxTick = std::max(maxTick, audioClipEndTickForTempo(clip, tempo));
            }

            return std::max<std::int64_t>(maxTick, mw::core::Project::ticksPerQuarterNote);
        };

        auto applyMapWindow = [this, getSequenceMapMaxTick]
        {
            const auto requestedText = trackManagerMapBeatWindowBox.getText().trim();
            int beatWindow = 0;

            if (! requestedText.equalsIgnoreCase("Full"))
            {
                try { beatWindow = std::stoi(requestedText.toStdString()); } catch (...) { beatWindow = 0; }

                if (beatWindow != 16 && beatWindow != 32 && beatWindow != 64 && beatWindow != 128 && beatWindow != 256)
                    beatWindow = 0;
            }

            const auto maxTick = getSequenceMapMaxTick();
            const int fullProjectBeats = std::max(4, static_cast<int>(std::ceil(static_cast<double>(maxTick) / mw::core::Project::ticksPerQuarterNote)));

            sequenceMapBeatWindow = beatWindow;

            if (sequenceMapBeatWindow <= 0)
            {
                sequenceMapStartBeat = 0;
                trackManagerMapStartBeatBox.setText("0", juce::dontSendNotification);
                trackManagerMapBeatWindowBox.setText("Full", juce::dontSendNotification);
                logMessage("Sequence Map Beats Visible: Full project (" + juce::String(fullProjectBeats) + " beats).");
            }
            else
            {
                sequenceMapStartBeat = std::clamp(sequenceMapStartBeat, 0, std::max(0, fullProjectBeats - sequenceMapBeatWindow));
                trackManagerMapStartBeatBox.setText(juce::String(sequenceMapStartBeat), juce::dontSendNotification);
                trackManagerMapBeatWindowBox.setText(juce::String(sequenceMapBeatWindow), juce::dontSendNotification);
                logMessage("Sequence Map Beats Visible: " + juce::String(sequenceMapBeatWindow) + ". Use the horizontal scrollbar or mouse wheel to move through the project.");
            }

            if (trackManagerContent != nullptr)
            {
                if (auto* content = dynamic_cast<TrackManagerWindowContent*>(trackManagerContent.get()))
                    content->refreshSequenceMap();
                else
                    trackManagerContent->repaint();
            }
        };

        auto mapUp = [this]
        {
            sequenceMapFirstIndex = std::max(0, sequenceMapFirstIndex - 1);
            if (trackManagerContent != nullptr)
            {
                if (auto* content = dynamic_cast<TrackManagerWindowContent*>(trackManagerContent.get()))
                    content->refreshSequenceMap();
                else
                    trackManagerContent->repaint();
            }
        };

        auto mapDown = [this]
        {
            const auto maxOffset = std::max(0, static_cast<int>(importSections.size()) - 1);
            sequenceMapFirstIndex = std::min(maxOffset, sequenceMapFirstIndex + 1);
            if (trackManagerContent != nullptr)
            {
                if (auto* content = dynamic_cast<TrackManagerWindowContent*>(trackManagerContent.get()))
                    content->refreshSequenceMap();
                else
                    trackManagerContent->repaint();
            }
        };

        auto mapHorizontalScroll = [this, refreshMapView, getSequenceMapMaxTick](int startBeat)
        {
            const auto maxTick = getSequenceMapMaxTick();
            const int fullProjectBeats = std::max(4, static_cast<int>(std::ceil(static_cast<double>(maxTick) / mw::core::Project::ticksPerQuarterNote)));
            const int beatWindow = sequenceMapBeatWindow <= 0 ? fullProjectBeats : std::max(4, sequenceMapBeatWindow);
            const int maxBeat = std::max(beatWindow, fullProjectBeats);
            sequenceMapStartBeat = sequenceMapBeatWindow <= 0 ? 0 : std::clamp(startBeat, 0, std::max(0, maxBeat - beatWindow));
            trackManagerMapStartBeatBox.setText(juce::String(sequenceMapStartBeat), juce::dontSendNotification);
            refreshMapView();
        };

        auto mapVerticalScroll = [this, refreshMapView](int firstSequence)
        {
            const auto maxOffset = std::max(0, static_cast<int>(importSections.size()) - 1);
            sequenceMapFirstIndex = std::clamp(firstSequence, 0, maxOffset);
            refreshMapView();
        };

        auto getMapMaxBeat = [getSequenceMapMaxTick]() -> int
        {
            const auto maxTick = getSequenceMapMaxTick();
            return std::max(4, static_cast<int>(std::ceil(static_cast<double>(maxTick) / mw::core::Project::ticksPerQuarterNote)));
        };

        auto getMapSequenceCount = [this]() -> int
        {
            return static_cast<int>(importSections.size());
        };

        auto sequenceMapClick = [this, getSequenceMapMaxTick](const juce::MouseEvent& event, bool doubleClick)
        {
            if (!currentProject || importSections.empty())
                return;

            auto bounds = event.eventComponent != nullptr
                ? event.eventComponent->getLocalBounds()
                : juce::Rectangle<int>();

            auto area = bounds.reduced(10);

            const int rowHeight = 18;
            const int labelWidth = 76;
            const int miniMapHeight = 14;
            const int miniMapGap = 4;
            const int timelineRulerHeight = 38;
            const int headerHeight = miniMapHeight + miniMapGap + timelineRulerHeight;
            const int visibleCapacity = std::max(1, (area.getHeight() - headerHeight - 4) / rowHeight);
            const int totalSequences = static_cast<int>(importSections.size());
            const int maxVisibleSequences = std::max(1, std::min(visibleCapacity, totalSequences));
            const int maxOffset = std::max(0, totalSequences - visibleCapacity);
            sequenceMapFirstIndex = std::clamp(sequenceMapFirstIndex, 0, maxOffset);
            const int firstVisibleSequence = sequenceMapFirstIndex;
            const int lastVisibleSequence = std::min(totalSequences, firstVisibleSequence + maxVisibleSequences);
            const int mapHeight = maxVisibleSequences * rowHeight + headerHeight + 4;

            auto mapArea = area.removeFromTop(mapHeight);
            auto header = mapArea.removeFromTop(headerHeight);
            auto overviewRow = header.removeFromTop(miniMapHeight);
            auto overviewTimeline = overviewRow.withTrimmedLeft(labelWidth);

            const auto x = static_cast<int>(event.position.x);
            const auto y = static_cast<int>(event.position.y);

            if (overviewTimeline.contains(x, y))
            {
                const auto maxTick = getSequenceMapMaxTick();
                const int fullProjectBeats = std::max(4, static_cast<int>(std::ceil(static_cast<double>(maxTick) / mw::core::Project::ticksPerQuarterNote)));
                const int beatWindow = sequenceMapBeatWindow <= 0 ? fullProjectBeats : std::max(4, sequenceMapBeatWindow);
                const int maxStart = std::max(0, fullProjectBeats - beatWindow);

                if (maxStart > 0)
                {
                    const double ratio = static_cast<double>(x - overviewTimeline.getX()) / std::max(1, overviewTimeline.getWidth());
                    sequenceMapStartBeat = std::clamp(static_cast<int>(std::round(ratio * maxStart)), 0, maxStart);
                    trackManagerMapStartBeatBox.setText(juce::String(sequenceMapStartBeat), juce::dontSendNotification);
                    if (trackManagerContent != nullptr)
                    {
                        if (auto* content = dynamic_cast<TrackManagerWindowContent*>(trackManagerContent.get()))
                            content->refreshSequenceMap();
                        else
                            trackManagerContent->repaint();
                    }
                }

                return;
            }

            if (!mapArea.contains(mapArea.getX(), y))
                return;

            const int rowOffset = y - mapArea.getY();

            if (rowOffset < 0)
                return;

            const int visibleRow = rowOffset / rowHeight;
            const int sequenceIndex = firstVisibleSequence + visibleRow;

            if (sequenceIndex < firstVisibleSequence || sequenceIndex >= lastVisibleSequence)
                return;

            selectSequenceFromMap(sequenceIndex, true);
        };

        auto paintSequenceMap = [this, getSequenceMapMaxTick](juce::Graphics& g, juce::Rectangle<int> bounds)
        {
            g.fillAll(juce::Colour(0xff101216));

            auto area = bounds.reduced(10);

            g.setColour(juce::Colours::lightgrey);
            g.setFont(14.0f);
            if (!currentProject || importSections.empty())
            {
                g.setColour(juce::Colours::grey);
                g.drawText("Import a file or sequence to see the visual map.", area, juce::Justification::centred);
                return;
            }

            const auto maxTick = getSequenceMapMaxTick();

            auto sequenceColour = [this](int index, const juce::String& name)
            {
                const auto overrideIt = sequenceColourOverrides.find(index);

                if (overrideIt != sequenceColourOverrides.end())
                    return overrideIt->second;

                return defaultSequenceColourFor(index, name);
            };

            auto sequenceEndTickForMap = [this](int sequenceIndex, std::int64_t fallbackEndTick)
            {
                if (!currentProject)
                    return fallbackEndTick;

                const auto sequenceNumber = sequenceIndex + 1;
                auto endTick = fallbackEndTick;
                const auto tempo = static_cast<double>(currentProject->getTempoBpm());

                for (const auto& clip : currentProject->getAudioClips())
                {
                    if (clip.sequenceNumber == sequenceNumber)
                        endTick = std::max(endTick, audioClipEndTickForTempo(clip, tempo));
                }

                return endTick;
            };

            auto sequenceHasTimelineContent = [this](int sequenceIndex)
            {
                if (sequenceIndex < 0 || sequenceIndex >= static_cast<int>(importSections.size()))
                    return false;

                const auto& section = importSections[static_cast<std::size_t>(sequenceIndex)];
                return !section.trackNumbers.empty() || sequenceHasAudioClips(sequenceIndex + 1);
            };

            const int rowHeight = 18;
            const int labelWidth = 76;
            const int miniMapHeight = 14;
            const int miniMapGap = 4;
            const int timelineRulerHeight = 38;
            const int headerHeight = miniMapHeight + miniMapGap + timelineRulerHeight;
            const int visibleCapacity = std::max(1, (area.getHeight() - headerHeight - 4) / rowHeight);
            const int totalSequences = static_cast<int>(importSections.size());
            const int maxVisibleSequences = std::max(1, std::min(visibleCapacity, totalSequences));
            const int maxOffset = std::max(0, totalSequences - visibleCapacity);
            sequenceMapFirstIndex = std::clamp(sequenceMapFirstIndex, 0, maxOffset);
            const int firstVisibleSequence = sequenceMapFirstIndex;
            const int lastVisibleSequence = std::min(totalSequences, firstVisibleSequence + maxVisibleSequences);
            const int mapHeight = maxVisibleSequences * rowHeight + headerHeight + 4;

            auto mapArea = area.removeFromTop(mapHeight);
            auto header = mapArea.removeFromTop(headerHeight);
            auto overviewRow = header.removeFromTop(miniMapHeight);
            header.removeFromTop(miniMapGap);
            auto timeline = mapArea.withTrimmedLeft(labelWidth);

            g.setColour(juce::Colour(0xff202328));
            g.fillRect(timeline);
            g.setColour(juce::Colours::darkgrey);
            g.drawRect(timeline);

            const auto maxBeat = static_cast<double>(maxTick) / mw::core::Project::ticksPerQuarterNote;
            const auto effectiveWindowBeats = sequenceMapBeatWindow <= 0 ? std::max(4.0, maxBeat) : static_cast<double>(std::max(4, sequenceMapBeatWindow));
            const auto visibleStartBeat = sequenceMapBeatWindow <= 0 ? 0.0 : static_cast<double>(std::max(0, sequenceMapStartBeat));
            const auto visibleBeatWindow = effectiveWindowBeats;
            const auto visibleEndBeat = std::min(std::max(maxBeat, visibleStartBeat + visibleBeatWindow), visibleStartBeat + visibleBeatWindow);
            const auto visibleStartTick = static_cast<std::int64_t>(std::llround(visibleStartBeat * mw::core::Project::ticksPerQuarterNote));
            const auto visibleEndTick = static_cast<std::int64_t>(std::llround(visibleEndBeat * mw::core::Project::ticksPerQuarterNote));
            const auto visibleTickLength = std::max<std::int64_t>(mw::core::Project::ticksPerQuarterNote, visibleEndTick - visibleStartTick);

            auto overviewLabel = overviewRow.removeFromLeft(labelWidth);
            auto overviewTimeline = overviewRow.reduced(0, 1);
            g.setFont(juce::FontOptions(13.0f, juce::Font::bold));
            g.setColour(juce::Colour(0xff9fd0ff));
            g.drawText("View", overviewLabel, juce::Justification::centredLeft);
            g.setColour(juce::Colour(0xff1f2329));
            g.fillRect(overviewTimeline);
            g.setColour(juce::Colours::darkgrey);
            g.drawRect(overviewTimeline);

            // Keep the overview bar intentionally plain: it should not add per-sequence
            // markers as sequences are imported.  Only the focused sequence is shown,
            // scaled to its actual start/end span against the full project length.
            if (activeImportSectionIndex >= 0 && activeImportSectionIndex < totalSequences)
            {
                const auto& activeSection = importSections[static_cast<std::size_t>(activeImportSectionIndex)];

                if (sequenceHasTimelineContent(activeImportSectionIndex))
                {
                    const double activeStartBeat = static_cast<double>(activeSection.startTick) / mw::core::Project::ticksPerQuarterNote;
                    const double activeEndBeat = static_cast<double>(sequenceEndTickForMap(activeImportSectionIndex, activeSection.endTick)) / mw::core::Project::ticksPerQuarterNote;
                    const auto activeX1 = overviewTimeline.getX() + static_cast<int>((activeStartBeat / std::max(1.0, maxBeat)) * overviewTimeline.getWidth());
                    const auto activeX2 = overviewTimeline.getX() + static_cast<int>((activeEndBeat / std::max(1.0, maxBeat)) * overviewTimeline.getWidth());
                    const auto clippedX1 = juce::jlimit(overviewTimeline.getX(), overviewTimeline.getRight(), activeX1);
                    const auto clippedX2 = juce::jlimit(overviewTimeline.getX(), overviewTimeline.getRight(), activeX2);
                    auto activeSpan = juce::Rectangle<int>(
                        clippedX1,
                        overviewTimeline.getY() + 2,
                        std::max(4, clippedX2 - clippedX1),
                        std::max(2, overviewTimeline.getHeight() - 4)
                    ).getIntersection(overviewTimeline);

                    g.setColour(sequenceColour(activeImportSectionIndex, activeSection.name).withAlpha(0.64f));
                    g.fillRect(activeSpan);
                    g.setColour(juce::Colours::yellow.withAlpha(0.92f));
                    g.drawRect(activeSpan, 1);
                }
                else
                {
                    g.setColour(juce::Colours::grey);
                    g.drawText("empty", overviewTimeline, juce::Justification::centredLeft);
                }
            }

            auto headerLabels = header.removeFromLeft(labelWidth);
            auto timeHeader = header.removeFromTop(timelineRulerHeight / 2);
            auto beatHeader = header;

            g.setFont(14.0f);
            g.setColour(juce::Colour(0xff9fd0ff));
            g.drawText("Time", headerLabels.removeFromTop(timelineRulerHeight / 2), juce::Justification::centredLeft);
            g.setColour(juce::Colours::lightgrey);
            g.drawText("Beat", headerLabels, juce::Justification::centredLeft);

            auto chooseBeatStep = [](double beatLength)
            {
                if (beatLength <= 16.0) return 2;
                if (beatLength <= 32.0) return 4;
                if (beatLength <= 64.0) return 8;
                if (beatLength <= 128.0) return 16;
                if (beatLength <= 256.0) return 32;
                if (beatLength <= 512.0) return 64;
                return 128;
            };

            const int beatStep = chooseBeatStep(visibleBeatWindow);
            const int firstMarkerBeat = (static_cast<int>(std::floor(visibleStartBeat)) / beatStep) * beatStep;
            const int lastMarkerBeat = static_cast<int>(std::ceil(visibleEndBeat / beatStep)) * beatStep;

            for (int beat = firstMarkerBeat; beat <= lastMarkerBeat; beat += beatStep)
            {
                if (beat < visibleStartBeat || beat > visibleEndBeat)
                    continue;

                const auto ratio = (static_cast<double>(beat) - visibleStartBeat) / std::max(1.0, visibleBeatWindow);
                const auto x = timeline.getX() + static_cast<int>(ratio * timeline.getWidth());

                g.setColour(beat == 0 ? juce::Colour(0xff5a626e) : juce::Colour(0xff424852));
                g.drawVerticalLine(x, static_cast<float>(timeline.getY()), static_cast<float>(timeline.getBottom()));

                const auto markerWidth = juce::jlimit(72, 112, timeline.getWidth() / std::max(1, (lastMarkerBeat - firstMarkerBeat) / std::max(1, beatStep)));
                const auto labelX = juce::jlimit(timeline.getX(), std::max(timeline.getX(), timeline.getRight() - markerWidth), x - markerWidth / 2);

                g.setColour(juce::Colour(0xff9fd0ff));
                auto timeLabelArea = juce::Rectangle<int>(labelX, timeHeader.getY(), markerWidth, timeHeader.getHeight());
                g.drawText(
                    timelineTimeTextForBeat(static_cast<double>(beat), currentProject ? currentProject->getTempoBpm() : 120),
                    timeLabelArea,
                    juce::Justification::centred
                );

                g.setColour(juce::Colours::grey);
                auto beatLabelArea = juce::Rectangle<int>(labelX, beatHeader.getY(), markerWidth, beatHeader.getHeight());
                g.drawText(juce::String(beat), beatLabelArea, juce::Justification::centred);
            }

            for (int i = firstVisibleSequence; i < lastVisibleSequence; ++i)
            {
                const auto& section = importSections[static_cast<std::size_t>(i)];
                auto row = mapArea.removeFromTop(rowHeight);
                const bool selected = i == activeImportSectionIndex;

                if (selected)
                {
                    g.setColour(juce::Colour(0xff303846));
                    g.fillRect(row);
                    g.setColour(juce::Colours::white.withAlpha(0.75f));
                    g.drawRect(row.reduced(1), 1);
                }

                auto label = row.removeFromLeft(labelWidth);
                g.setColour(sequenceColour(i, section.name));
                g.fillRect(label.removeFromLeft(10).reduced(1, 4));

                g.setColour(selected ? juce::Colours::white : juce::Colours::lightgrey);
                g.drawText((selected ? "> Seq " : "Seq ") + juce::String(i + 1), label, juce::Justification::centredLeft);

                auto lane = row.reduced(2, 4);
                g.setColour(juce::Colour(0xff181b20));
                g.fillRect(lane);

                const auto clippedStartTick = std::max(section.startTick, visibleStartTick);
                const auto clippedEndTick = std::min(sequenceEndTickForMap(i, section.endTick), visibleEndTick);

                if (!sequenceHasTimelineContent(i))
                {
                    g.setColour(juce::Colours::grey);
                    g.setFont(12.0f);
                    g.drawText("empty", lane.reduced(5, 0), juce::Justification::centredLeft);
                }
                else if (clippedEndTick > clippedStartTick)
                {
                    const auto x1 = lane.getX() + static_cast<int>(((clippedStartTick - visibleStartTick) * lane.getWidth()) / visibleTickLength);
                    const auto x2 = lane.getX() + static_cast<int>(((clippedEndTick - visibleStartTick) * lane.getWidth()) / visibleTickLength);
                    auto block = juce::Rectangle<int>(
                        x1,
                        lane.getY(),
                        std::max(4, x2 - x1),
                        lane.getHeight()
                    );

                    g.setColour(sequenceColour(i, section.name).withAlpha(0.92f));
                    g.fillRect(block);

                    g.setColour(juce::Colours::black.withAlpha(0.35f));
                    g.drawRect(block);
                }
            }

        };

        trackManagerContent = std::make_unique<TrackManagerWindowContent>(
            trackManagerBox,
            trackManagerSelectBox,
            trackManagerStartBeatBox,
            trackManagerSectionBox,
            trackManagerSectionStartBeatBox,
            tempoBox,
            timeSignatureBox,
            loopCountBox,
            trackManagerMapStartBeatBox,
            trackManagerMapBeatWindowBox,
            [this] { applyProjectTimingSettings(); markTrackManagerEditorDirty("Apply Project Timing"); },
            refresh,
            select,
            selectSequence,
            add,
            duplicate,
            remove,
            removeSequence,
            undoTrackManager,
            applyStartBeat,
            duplicateAtBeat,
            openInPianoRoll,
            previewTrackFromManager,
            previewSequenceFromManager,
            previewProjectFromManager,
            startFromFileFromManager,
            importSequence,
            applySectionStart,
            nudgeSequence,
            addBlankToSection,
            linkTrackToSection,
            unlinkTrackFromSection,
            moveTrackToSequence,
            changeSequenceColour,
            renameSequence,
            editSequenceNotes,
            toggleSequenceLock,
            isSequenceLocked,
            sequenceMapClick,
            mapUp,
            mapDown,
            applyMapWindow,
            mapHorizontalScroll,
            mapVerticalScroll,
            getMapMaxBeat,
            getMapSequenceCount,
            filterChanged,
            toggleNameMode,
            getShortNameMode,
            paintSequenceMap,
            [this] { applyTrackManagerEditorChanges(); },
            closeWindow
        );

        auto* window = new PianoRollDocumentWindow("Track Manager", closeWindow);
        applyPoorMansStudioCustomTitleBar(*window);
        window->setResizable(true, true);
        window->setResizeLimits(720, 420, 2200, 1600);
        window->setContentNonOwned(trackManagerContent.get(), true);
        window->centreWithSize(1120, 680);
        window->setVisible(true);
        applyPoorMansStudioWindowIcon(*window, PoorMansStudioWindowIcon::TrackManager);
        maximiseDocumentWindowToWorkArea(*window);

        trackManagerWindow.reset(window);
        startTrackManagerEditSession();

        logMessage("Opened Track Manager window.");
    }

    void MainComponent::openRawNotesWindow()
    {
        refreshNoteEditor();

        if (rawNotesWindow != nullptr)
        {
            rawNotesWindow->toFront(true);
            return;
        }

        auto closeWindow = [this]
        {
            if (rawNotesWindow != nullptr)
            {
                rawNotesWindow->setVisible(false);
                rawNotesWindow->setContentOwned(nullptr, false);

                juce::MessageManager::callAsync(
                    [this]
                    {
                        rawNotesWindow.reset();
                        rawNotesContent.reset();
                    }
                );
            }
        };

        auto applyRawNotes = [this]
        {
            applyNoteEditorToTrack();
            syncPianoRollFromSelectedTrack();
            logMessage("Applied raw notes to selected track.");
        };

        rawNotesContent = std::make_unique<RawNotesWindowContent>(
            noteEditorBox,
            applyRawNotes
        );

        auto* window = new PianoRollDocumentWindow("Raw Notes - Advanced", closeWindow);
        applyPoorMansStudioCustomTitleBar(*window);
        window->setResizable(true, true);
        window->setResizeLimits(520, 340, 1600, 1200);
        window->setContentNonOwned(rawNotesContent.get(), true);
        window->centreWithSize(760, 520);
        window->setVisible(true);
        applyPoorMansStudioWindowIcon(*window, PoorMansStudioWindowIcon::EditInfo);

        rawNotesWindow.reset(window);

        logMessage("Opened Raw Notes window.");
    }

    void MainComponent::openPianoRollPreviewPlayerWindow()
    {
        if (pianoRollPreviewPlayerWindow != nullptr)
        {
            pianoRollPreviewPlayerWindow->toFront(true);
            return;
        }

        auto closeWindow = [this]
        {
            cleanupPianoRollPreviewFiles();

            if (pianoRollPreviewPlayerWindow != nullptr)
            {
                pianoRollPreviewPlayerWindow->setVisible(false);
                pianoRollPreviewPlayerWindow->setContentOwned(nullptr, false);

                juce::MessageManager::callAsync(
                    [this]
                    {
                        pianoRollPreviewPlayerWindow.reset();
                        pianoRollPreviewPlayerContent.reset();
                    }
                );
            }
        };

        auto preview = [this]
        {
            switch (lastPianoRollPreviewScope)
            {
                case 1: previewSelectedTrackOnBackgroundThread(); break;
                case 2: previewSelectedSequenceOnBackgroundThread(); break;
                case 3: previewCurrentProjectOnBackgroundThread(); break;
                case 0:
                default: renderPianoRollPreview(); break;
            }
        };

        auto playPause = [this]
        {
            if (pianoRoll.isPreviewPlayheadActive() && !pianoRoll.isPreviewPlayheadPaused())
                pausePianoRollPreview();
            else
                playPianoRollPreview();
        };

        auto stop = [this]
        {
            stopPianoRollPreview();
        };

        auto previewNotes = [this]
        {
            return lastPianoRollPreviewNotes;
        };

        auto hasPreview = [this]
        {
            return !lastPianoRollPreviewWavPath.empty()
                && std::filesystem::exists(lastPianoRollPreviewWavPath);
        };

        auto isRenderingPreview = [this]
        {
            return renderingInProgress.load();
        };

        auto currentPreviewSeconds = [this]
        {
            return getPianoRollPreviewCurrentSeconds();
        };

        auto totalPreviewSeconds = [this]
        {
            return getPianoRollPreviewTotalSeconds();
        };

        auto seekPreviewSeconds = [this](double seconds)
        {
            return seekPianoRollPreviewToSeconds(seconds);
        };

        auto totalPreviewBeats = [this]() -> double
        {
            return std::max(0.0, lastPianoRollPreviewDurationBeats);
        };

        pianoRollPreviewPlayerContent =
            std::make_unique<PianoRollPreviewPlayerContent>(
                pianoRoll,
                preview,
                playPause,
                stop,
                closeWindow,
                previewNotes,
                hasPreview,
                isRenderingPreview,
                currentPreviewSeconds,
                totalPreviewSeconds,
                totalPreviewBeats,
                seekPreviewSeconds
            );

        auto* window = new PianoRollDocumentWindow("Piano Roll Preview Player", closeWindow);
        applyPoorMansStudioCustomTitleBar(*window);
        window->setResizable(true, true);
        window->setResizeLimits(520, 260, 1600, 900);
        window->setContentNonOwned(pianoRollPreviewPlayerContent.get(), true);
        window->centreWithSize(760, 420);
        window->setVisible(true);
        applyPoorMansStudioWindowIcon(*window, PoorMansStudioWindowIcon::PreviewPlayer);

        pianoRollPreviewPlayerWindow.reset(window);
    }

void MainComponent::openPianoRollWindow()
    {
        if (!ensureSelectedTrackHasSequenceForPianoRoll())
            return;

        const auto trackIndex = getSelectedTrackIndex();

        if (!currentProject || trackIndex < 0 || trackIndex >= static_cast<int>(currentProject->getTracks().size()))
        {
            logMessage("ERROR: Select a valid track before opening Piano Roll.");
            return;
        }

        if (rawNotesWindow != nullptr)
        {
            rawNotesWindow->setContentOwned(nullptr, false);
            rawNotesWindow.reset();
        }

        rawNotesContent.reset();

        if (auto* existing = findPianoRollEditorWindow(trackIndex))
        {
            updatePianoRollWindowDirtyIndicator(*existing);
            if (existing->window != nullptr)
                existing->window->toFront(true);
            return;
        }

        auto state = std::make_unique<PianoRollEditorWindowState>();
        state->trackIndex = trackIndex;

        const auto& track = currentProject->getTracks()[static_cast<std::size_t>(trackIndex)];
        const auto timeSignature = currentProject->getTimeSignature();
        state->bpmBox.setText(juce::String(currentProject->getTempoBpm()), juce::dontSendNotification);
        state->timeSigBox.setText(juce::String(timeSignature.numerator) + "/" + juce::String(timeSignature.denominator), juce::dontSendNotification);
        state->beatWindowBox.setText("16", juce::dontSendNotification);
        state->startBeatBox.setText("0", juce::dontSendNotification);
        state->noteLengthBox.setText("1", juce::dontSendNotification);
        state->velocityBox.setText("100", juce::dontSendNotification);
        state->snapBox.setText("1", juce::dontSendNotification);
        state->pageLabel.setText("Window 1 / 1 | Beats 0-16", juce::dontSendNotification);
        state->pageLabel.setJustificationType(juce::Justification::centredLeft);
        state->pageLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
        state->keyRangeLabel.setJustificationType(juce::Justification::centredLeft);
        state->keyRangeLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
        state->keyJumpLabel.setText("Key", juce::dontSendNotification);
        state->keyJumpLabel.setJustificationType(juce::Justification::centredLeft);
        state->keyJumpLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
        state->trackSoundLibraryBox.setReadOnly(true);
        state->trackSoundLibraryBox.setScrollbarsShown(false);
        state->trackSoundLibraryBox.setJustification(juce::Justification::centredLeft);
        state->instrumentCombo.setTextWhenNothingSelected("Choose instrument");
        state->trackBackendCombo.setTextWhenNothingSelected("Choose backend");

        state->suppressDirty = true;
        state->roll.setTempoBpm(currentProject->getTempoBpm());
        state->roll.setNotes(track.getNotes());
        state->suppressDirty = false;

        auto stateRaw = state.get();
        pianoRollEditorWindows[trackIndex] = std::move(state);

        auto getLivePianoRollState = [this, stateRaw]() -> PianoRollEditorWindowState*
        {
            if (stateRaw == nullptr)
                return nullptr;

            return findPianoRollEditorWindow(stateRaw->trackIndex);
        };

        auto applySettingsForTrack = [this, getLivePianoRollState]
        {
            if (auto* s = getLivePianoRollState())
                applyPianoRollSettings(*s);
        };

        auto applyRollForTrack = [this, getLivePianoRollState]
        {
            if (auto* s = getLivePianoRollState())
                applyPianoRollEditorChanges(*s);
        };

        auto previewRollForTrack = [this, getLivePianoRollState]
        {
            if (auto* s = getLivePianoRollState())
            {
                applyPianoRollSettings(*s, false);

                pianoRollOpenTrackIndex = s->trackIndex;
                pianoRollBpmBox.setText(s->bpmBox.getText(), juce::dontSendNotification);
                pianoRollTimeSigBox.setText(s->timeSigBox.getText(), juce::dontSendNotification);
                pianoRollBeatWindowBox.setText(s->beatWindowBox.getText(), juce::dontSendNotification);
                pianoRollStartBeatBox.setText(s->startBeatBox.getText(), juce::dontSendNotification);
                pianoRollNoteLengthBox.setText(s->noteLengthBox.getText(), juce::dontSendNotification);
                pianoRollVelocityBox.setText(s->velocityBox.getText(), juce::dontSendNotification);
                pianoRollSnapBox.setText(s->snapBox.getText(), juce::dontSendNotification);

                suppressPianoRollEditorDirty = true;
                pianoRoll.setTempoBpm(currentProject ? currentProject->getTempoBpm() : 120);
                pianoRoll.setNotes(s->roll.getNotes());
                pianoRoll.setGrid(getPianoRollBeatWindow(*s), s->roll.getLowPitch(), s->roll.getHighPitch());
                pianoRoll.setStartBeat(getPianoRollStartBeat(*s));
                suppressPianoRollEditorDirty = false;

                openPianoRollPreviewPlayerWindow();
            }
        };

        auto changeLibraryForTrack = [this, getLivePianoRollState]
        {
            if (auto* s = getLivePianoRollState())
                choosePianoRollTrackSoundLibrary(*s);
        };

        auto closeWindow = [this, getLivePianoRollState]
        {
            if (auto* s = getLivePianoRollState())
                closePianoRollWindowWithDirtyCheck(s->trackIndex);
        };

        stateRaw->trackBackendCombo.onChange = [this, getLivePianoRollState]
        {
            if (auto* s = getLivePianoRollState())
            {
                if (s->suppressInstrumentChange)
                    return;

                const auto backendId = s->trackBackendCombo.getSelectedId();
                const auto backendType = backendId == 4
                    ? mw::core::SampleBackendType::VST3
                    : (backendId == 3
                        ? mw::core::SampleBackendType::SFZ
                        : (backendId == 2 ? mw::core::SampleBackendType::SF2 : getProjectDefaultBackendType()));
                if (backendType == mw::core::SampleBackendType::VST3)
                {
                    populatePianoRollInstrumentCombo(*s);
                    applyPianoRollInstrumentSelection(*s, true);
                }
                else
                {
                    assignPianoRollTrackSoundLibrary(*s, getProjectDefaultLibraryPath(backendType), backendType);
                }
            }
        };

        stateRaw->instrumentCombo.onChange = [this, getLivePianoRollState]
        {
            if (auto* s = getLivePianoRollState())
                applyPianoRollInstrumentSelection(*s, true);
        };

        populatePianoRollInstrumentCombo(*stateRaw);
        refreshPianoRollTrackSoundLibraryDisplay(*stateRaw);

        stateRaw->beat4Button.onClick = [this, getLivePianoRollState] { if (auto* s = getLivePianoRollState()) { s->beatWindowBox.setText("4", juce::dontSendNotification); applyPianoRollSettings(*s, false); } };
        stateRaw->beat8Button.onClick = [this, getLivePianoRollState] { if (auto* s = getLivePianoRollState()) { s->beatWindowBox.setText("8", juce::dontSendNotification); applyPianoRollSettings(*s, false); } };
        stateRaw->beat16Button.onClick = [this, getLivePianoRollState] { if (auto* s = getLivePianoRollState()) { s->beatWindowBox.setText("16", juce::dontSendNotification); applyPianoRollSettings(*s, false); } };
        stateRaw->beat32Button.onClick = [this, getLivePianoRollState] { if (auto* s = getLivePianoRollState()) { s->beatWindowBox.setText("32", juce::dontSendNotification); applyPianoRollSettings(*s, false); } };
        stateRaw->beat64Button.onClick = [this, getLivePianoRollState] { if (auto* s = getLivePianoRollState()) { s->beatWindowBox.setText("64", juce::dontSendNotification); applyPianoRollSettings(*s, false); } };

        stateRaw->previousButton.onClick = [this, getLivePianoRollState]
        {
            if (auto* s = getLivePianoRollState())
            {
                const auto beatWindow = getPianoRollBeatWindow(*s);
                const auto startBeat = std::max(0, getPianoRollStartBeat(*s) - beatWindow);
                s->startBeatBox.setText(juce::String(startBeat), juce::dontSendNotification);
                applyPianoRollSettings(*s, false);
            }
        };

        stateRaw->nextButton.onClick = [this, getLivePianoRollState]
        {
            if (auto* s = getLivePianoRollState())
            {
                const auto beatWindow = getPianoRollBeatWindow(*s);
                const auto startBeat = getPianoRollStartBeat(*s) + beatWindow;
                s->startBeatBox.setText(juce::String(startBeat), juce::dontSendNotification);
                applyPianoRollSettings(*s, false);
            }
        };

        stateRaw->jumpPageButton.onClick = [this, getLivePianoRollState]
        {
            if (auto* s = getLivePianoRollState())
            {
                int windowNumber = 1;
                try { windowNumber = std::stoi(s->pageBox.getText().toStdString()); } catch (...) {}
                const auto totalWindows = getPianoRollTotalPages(*s);
                windowNumber = std::clamp(windowNumber, 1, totalWindows);
                const auto beatWindow = getPianoRollBeatWindow(*s);
                s->startBeatBox.setText(juce::String((windowNumber - 1) * beatWindow), juce::dontSendNotification);
                applyPianoRollSettings(*s, false);
            }
        };

        auto jumpToKey = [this, getLivePianoRollState]
        {
            if (auto* s = getLivePianoRollState())
            {
                if (s->roll.jumpToKey(s->keyJumpBox.getText()))
                {
                    updatePianoRollKeyRangeLabel(*s);
                    logMessage("Piano Roll key jump: " + s->roll.getVisiblePitchRangeText());
                }
                else
                {
                    logMessage("Piano Roll key jump ignored. Use values like C, D, F#, Bb, C4, A0, or 60.");
                }
            }
        };

        stateRaw->keyJumpButton.onClick = jumpToKey;
        stateRaw->keyJumpBox.onReturnKey = jumpToKey;
        stateRaw->notesDownButton.onClick = [this, getLivePianoRollState] { if (auto* s = getLivePianoRollState()) { s->roll.nudgeVisiblePitchRange(-12); updatePianoRollKeyRangeLabel(*s); logMessage("Piano Roll notes down: " + s->roll.getVisiblePitchRangeText()); } };
        stateRaw->notesUpButton.onClick = [this, getLivePianoRollState] { if (auto* s = getLivePianoRollState()) { s->roll.nudgeVisiblePitchRange(12); updatePianoRollKeyRangeLabel(*s); logMessage("Piano Roll notes up: " + s->roll.getVisiblePitchRangeText()); } };
        stateRaw->copyButton.onClick = [this, getLivePianoRollState] { if (auto* s = getLivePianoRollState()) { s->roll.copySelectedNote(); logMessage("Copied selected piano roll note."); } };
        stateRaw->pasteButton.onClick = [this, getLivePianoRollState] { if (auto* s = getLivePianoRollState()) { s->roll.pasteCopiedNote(); logMessage("Pasted copied piano roll note."); } };
        stateRaw->undoButton.onClick = [this, getLivePianoRollState] { if (auto* s = getLivePianoRollState()) undoPianoRollEditorAction(*s); };
        stateRaw->redoButton.onClick = [this, getLivePianoRollState] { if (auto* s = getLivePianoRollState()) redoPianoRollEditorAction(*s); };
        stateRaw->clearSelectionButton.onClick = [this, getLivePianoRollState] { if (auto* s = getLivePianoRollState()) { s->roll.clearNoteSelection(); logMessage("Cleared piano roll selection."); } };

        stateRaw->roll.onNotesChanged = [this, getLivePianoRollState](const std::vector<mw::core::NoteEvent>& notes)
        {
            if (auto* s = getLivePianoRollState())
            {
                if (!s->suppressDirty)
                    markPianoRollEditorDirty(*s);

                if (s->trackIndex == getSelectedTrackIndex())
                    populateNoteEditorFromNotes(notes);
            }
        };

        stateRaw->roll.onSelectedNoteChanged = [this, getLivePianoRollState](int)
        {
            if (auto* s = getLivePianoRollState())
            {
                if (s->trackIndex == getSelectedTrackIndex())
                    populateNoteEditorFromNotes(s->roll.getNotes());
                else
                    refreshNoteEditor();
            }
        };

        stateRaw->roll.onPitchRangeChanged = [this, getLivePianoRollState]
        {
            if (auto* s = getLivePianoRollState())
                updatePianoRollKeyRangeLabel(*s);
        };

        stateRaw->roll.onPreviewPlayheadPageChanged = [this, getLivePianoRollState](int pageStartBeat)
        {
            if (auto* s = getLivePianoRollState())
            {
                const auto safeStartBeat = std::max(0, pageStartBeat);
                s->startBeatBox.setText(juce::String(safeStartBeat), juce::dontSendNotification);
                s->roll.setStartBeat(safeStartBeat);
                updatePianoRollPageIndicator(*s);
                s->roll.repaint();
            }
        };

        applyPianoRollSettings(*stateRaw, false);
        fitPianoRollToTrack(*stateRaw);

        stateRaw->content = std::make_unique<PianoRollWindowContent>(
            stateRaw->roll,
            stateRaw->bpmBox,
            stateRaw->timeSigBox,
            stateRaw->beatWindowBox,
            stateRaw->noteLengthBox,
            stateRaw->velocityBox,
            stateRaw->snapBox,
            stateRaw->trackSoundLibraryBox,
            stateRaw->instrumentCombo,
            stateRaw->changeLibraryButton,
            stateRaw->pageLabel,
            stateRaw->pageBox,
            stateRaw->keyRangeLabel,
            stateRaw->keyJumpLabel,
            stateRaw->keyJumpBox,
            stateRaw->keyJumpButton,
            stateRaw->notesDownButton,
            stateRaw->notesUpButton,
            stateRaw->beat4Button,
            stateRaw->beat8Button,
            stateRaw->beat16Button,
            stateRaw->beat32Button,
            stateRaw->beat64Button,
            stateRaw->previousButton,
            stateRaw->nextButton,
            stateRaw->jumpPageButton,
            stateRaw->copyButton,
            stateRaw->pasteButton,
            stateRaw->undoButton,
            stateRaw->redoButton,
            stateRaw->clearSelectionButton,
            trackIndex,
            getTrackDisplayName(trackIndex),
            getPianoRollHeaderInstrumentTextForState(*stateRaw),
            applySettingsForTrack,
            applyRollForTrack,
            previewRollForTrack,
            changeLibraryForTrack
        );

        auto* window = new PianoRollDocumentWindow("Piano Roll - " + getTrackDisplayName(trackIndex), closeWindow);
        applyPoorMansStudioCustomTitleBar(*window);
        window->setResizable(true, true);
        window->setResizeLimits(900, 540, 2600, 1800);
        window->setContentNonOwned(stateRaw->content.get(), true);
        window->centreWithSize(1320, 820);
        window->setVisible(true);
        applyPoorMansStudioWindowIcon(*window, PoorMansStudioWindowIcon::PianoRoll);
        maximiseDocumentWindowToWorkArea(*window);

        stateRaw->window.reset(window);
        updatePianoRollWindowDirtyIndicator(*stateRaw);
        refreshAggregatePianoRollDirtyFlag();

        logMessage("Opened Piano Roll window for " + getTrackDisplayName(trackIndex) + ".");
    }

        void MainComponent::playPianoRollPreview()
    {
        if (pianoRollPreviewPaused)
        {
#if JUCE_WINDOWS
            const auto resumeResult = mciSendStringW(L"resume PoorMansStudioPianoRollPreview", nullptr, 0, nullptr);

            if (resumeResult == 0)
            {
                pianoRollPreviewPaused = false;
                pianoRoll.resumePreviewPlayhead();
                logMessage("Resumed Piano Roll preview playback.");
                return;
            }

            logMessage("Resume failed. Restarting preview playback from the selected preview time.");
            pianoRollPreviewPaused = false;
#endif
        }

        playPianoRollPreviewFile(lastPianoRollPreviewWavPath);
    }

    void MainComponent::playPianoRollPreviewFile(const std::filesystem::path& previewPath)
    {
#if JUCE_WINDOWS
        if (previewPath.empty() || !std::filesystem::exists(previewPath))
        {
            logMessage("No Piano Roll preview WAV available. Use Preview first.");
            return;
        }

        const auto totalSeconds = getPianoRollPreviewTotalSeconds();
        const auto startSeconds = totalSeconds > 0.0
            ? std::clamp(pendingPianoRollPreviewStartSeconds, 0.0, std::max(0.0, totalSeconds - 0.001))
            : 0.0;
        const auto startMilliseconds = static_cast<long long>(std::llround(startSeconds * 1000.0));

        mciSendStringW(L"stop PoorMansStudioPianoRollPreview", nullptr, 0, nullptr);
        mciSendStringW(L"close PoorMansStudioPianoRollPreview", nullptr, 0, nullptr);

        const auto nativePath = previewPath.wstring();
        const auto openCommand =
            std::wstring(L"open \"")
            + nativePath
            + L"\" type waveaudio alias PoorMansStudioPianoRollPreview";

        const auto openResult = mciSendStringW(openCommand.c_str(), nullptr, 0, nullptr);

        if (openResult != 0)
        {
            logMessage("ERROR: Windows could not open the Piano Roll preview WAV: " + previewPath.string());
            return;
        }

        mciSendStringW(L"set PoorMansStudioPianoRollPreview time format milliseconds", nullptr, 0, nullptr);

        const auto playCommand =
            std::wstring(L"play PoorMansStudioPianoRollPreview from ")
            + std::to_wstring(startMilliseconds);

        const auto playResult = mciSendStringW(playCommand.c_str(), nullptr, 0, nullptr);

        if (playResult == 0)
        {
            pianoRollPreviewPaused = false;

            const auto startBeat = startSeconds * (lastPianoRollPreviewTempoBpm / 60.0);
            const auto remainingBeats = std::max(0.01, lastPianoRollPreviewDurationBeats - startBeat);
            pianoRoll.startPreviewPlayhead(lastPianoRollPreviewTempoBpm, remainingBeats, startBeat);

            logMessage(
                "Playing Piano Roll preview from "
                + formatPreviewClockTime(startSeconds)
                + ": "
                + previewPath.string()
            );
        }
        else
        {
            mciSendStringW(L"close PoorMansStudioPianoRollPreview", nullptr, 0, nullptr);
            logMessage("ERROR: Windows could not play the Piano Roll preview WAV: " + previewPath.string());
        }
#else
        logMessage("Embedded preview playback is currently only available on Windows.");
#endif
    }

    bool MainComponent::seekPianoRollPreviewToSeconds(double seconds)
    {
        if (lastPianoRollPreviewWavPath.empty() || !std::filesystem::exists(lastPianoRollPreviewWavPath))
        {
            logMessage("No Piano Roll preview WAV available. Use Preview first.");
            return false;
        }

        const auto totalSeconds = getPianoRollPreviewTotalSeconds();

        if (totalSeconds <= 0.0 || seconds < 0.0 || seconds > totalSeconds)
            return false;

        const auto targetSeconds = std::clamp(seconds, 0.0, totalSeconds);
        pendingPianoRollPreviewStartSeconds = targetSeconds;

        const auto targetBeat = targetSeconds * (lastPianoRollPreviewTempoBpm / 60.0);
        const auto remainingBeats = std::max(0.01, lastPianoRollPreviewDurationBeats - targetBeat);
        const auto targetMilliseconds = static_cast<long long>(std::llround(targetSeconds * 1000.0));

#if JUCE_WINDOWS
        if (pianoRoll.isPreviewPlayheadActive())
        {
            if (pianoRollPreviewPaused)
            {
                const auto seekCommand =
                    std::wstring(L"seek PoorMansStudioPianoRollPreview to ")
                    + std::to_wstring(targetMilliseconds);

                const auto seekResult = mciSendStringW(seekCommand.c_str(), nullptr, 0, nullptr);

                if (seekResult != 0)
                    logMessage("Preview seek command failed while paused; visual playhead was still moved.");

                pianoRoll.startPreviewPlayhead(lastPianoRollPreviewTempoBpm, remainingBeats, targetBeat);
                pianoRoll.pausePreviewPlayhead();
                pianoRollPreviewPaused = true;
            }
            else
            {
                const auto playCommand =
                    std::wstring(L"play PoorMansStudioPianoRollPreview from ")
                    + std::to_wstring(targetMilliseconds);

                const auto playResult = mciSendStringW(playCommand.c_str(), nullptr, 0, nullptr);

                if (playResult != 0)
                {
                    logMessage("Preview seek command failed; reopening preview from the requested time.");
                    playPianoRollPreviewFile(lastPianoRollPreviewWavPath);
                    return true;
                }

                pianoRollPreviewPaused = false;
                pianoRoll.startPreviewPlayhead(lastPianoRollPreviewTempoBpm, remainingBeats, targetBeat);
            }
        }
#else
        if (pianoRoll.isPreviewPlayheadActive())
            pianoRoll.startPreviewPlayhead(lastPianoRollPreviewTempoBpm, remainingBeats, targetBeat);
#endif

        logMessage("Preview position set to " + formatPreviewClockTime(targetSeconds) + ".");
        return true;
    }

    double MainComponent::getPianoRollPreviewCurrentSeconds() const
    {
        if (lastPianoRollPreviewTempoBpm <= 0.0)
            return std::max(0.0, pendingPianoRollPreviewStartSeconds);

        if (pianoRoll.isPreviewPlayheadActive())
            return std::max(0.0, pianoRoll.getPreviewPlayheadBeat() * 60.0 / lastPianoRollPreviewTempoBpm);

        return std::max(0.0, pendingPianoRollPreviewStartSeconds);
    }

    double MainComponent::getPianoRollPreviewTotalSeconds() const
    {
        if (lastPianoRollPreviewTempoBpm <= 0.0 || lastPianoRollPreviewDurationBeats <= 0.0)
            return 0.0;

        return std::max(0.0, lastPianoRollPreviewDurationBeats * 60.0 / lastPianoRollPreviewTempoBpm);
    }

    void MainComponent::pausePianoRollPreview()
    {
#if JUCE_WINDOWS
        const auto pauseResult = mciSendStringW(L"pause PoorMansStudioPianoRollPreview", nullptr, 0, nullptr);

        if (pauseResult == 0)
        {
            pianoRollPreviewPaused = true;
            pianoRoll.pausePreviewPlayhead();
            logMessage("Paused Piano Roll preview playback.");
        }
        else
        {
            logMessage("No active Piano Roll preview playback to pause.");
        }
#else
        pianoRoll.pausePreviewPlayhead();
        logMessage("Embedded preview playback is currently only available on Windows.");
#endif
    }

    void MainComponent::stopPianoRollPreview()
    {
#if JUCE_WINDOWS
        mciSendStringW(L"stop PoorMansStudioPianoRollPreview", nullptr, 0, nullptr);
        mciSendStringW(L"close PoorMansStudioPianoRollPreview", nullptr, 0, nullptr);
        pianoRollPreviewPaused = false;
        pendingPianoRollPreviewStartSeconds = 0.0;
        pianoRoll.stopPreviewPlayhead();
        logMessage("Stopped Piano Roll preview playback.");
#else
        pianoRollPreviewPaused = false;
        pendingPianoRollPreviewStartSeconds = 0.0;
        pianoRoll.stopPreviewPlayhead();
        logMessage("Embedded preview playback is currently only available on Windows.");
#endif
    }

    std::filesystem::path MainComponent::findLatestProjectPreviewFile() const
    {
        const auto exportFolderPath =
            std::filesystem::path(exportFolderBox.getText().toStdString()).empty()
                ? mw::app::AppPaths::exportsFolder()
                : std::filesystem::path(exportFolderBox.getText().toStdString());

        if (!std::filesystem::exists(exportFolderPath))
            return {};

        const std::string preferredBaseName =
            baseNameBox.getText().isEmpty()
                ? std::string()
                : baseNameBox.getText().toStdString();

        std::filesystem::path newestPath;
        std::filesystem::file_time_type newestTime {};

        std::error_code ignored;

        for (const auto& entry : std::filesystem::directory_iterator(exportFolderPath, ignored))
        {
            if (ignored || !entry.is_regular_file())
                continue;

            auto ext = entry.path().extension().string();

            for (auto& c : ext)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

            if (ext != ".wav" && ext != ".mp3" && ext != ".flac" && ext != ".ogg")
                continue;

            if (!preferredBaseName.empty()
                && entry.path().stem().string().find(preferredBaseName) == std::string::npos)
            {
                continue;
            }

            const auto writeTime = std::filesystem::last_write_time(entry.path(), ignored);

            if (ignored)
                continue;

            if (newestPath.empty() || writeTime > newestTime)
            {
                newestPath = entry.path();
                newestTime = writeTime;
            }
        }

        return newestPath;
    }

    void MainComponent::playProjectPreview()
    {
#if JUCE_WINDOWS
        const auto pathToPlay = findLatestProjectPreviewFile();

        if (pathToPlay.empty() || !std::filesystem::exists(pathToPlay))
        {
            logMessage("No project preview/export audio found. Use the main Render Project action first.");
            return;
        }

        auto ext = pathToPlay.extension().string();

        for (auto& c : ext)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        if (ext != ".wav")
        {
            logMessage("Embedded project preview playback currently supports WAV files. Latest file found: " + pathToPlay.string());
            return;
        }

        const auto path = juce::String(pathToPlay.wstring().c_str());

        const auto ok = PlaySoundW(
            path.toWideCharPointer(),
            nullptr,
            SND_FILENAME | SND_ASYNC
        );

        if (ok)
            logMessage("Playing project preview: " + pathToPlay.string());
        else
            logMessage("ERROR: Windows could not play project preview WAV: " + pathToPlay.string());
#else
        logMessage("Embedded project preview playback is currently only available on Windows.");
#endif
    }

    void MainComponent::stopProjectPreview()
    {
#if JUCE_WINDOWS
        PlaySoundW(nullptr, nullptr, 0);
        logMessage("Stopped project preview playback.");
#else
        logMessage("Embedded project preview playback is currently only available on Windows.");
#endif
    }

    void MainComponent::cleanupGeneratedPreviewFiles()
    {
        std::error_code ignored;

        for (const auto& path : generatedPreviewFiles)
        {
            if (!path.empty())
                std::filesystem::remove_all(path, ignored);
        }

        generatedPreviewFiles.clear();

        const auto previewFolder = mw::app::AppPaths::previewFolder();
        if (std::filesystem::exists(previewFolder))
        {
            for (const auto& entry : std::filesystem::directory_iterator(previewFolder, ignored))
                std::filesystem::remove_all(entry.path(), ignored);
        }
    }

    void MainComponent::cleanupPianoRollPreviewFiles()
    {
        stopPianoRollPreview();
        cleanupGeneratedPreviewFiles();

        std::error_code ignored;

        if (!lastPianoRollPreviewMidiPath.empty())
            std::filesystem::remove(lastPianoRollPreviewMidiPath, ignored);

        if (!lastPianoRollPreviewWavPath.empty())
            std::filesystem::remove(lastPianoRollPreviewWavPath, ignored);

        lastPianoRollPreviewMidiPath.clear();
        lastPianoRollPreviewWavPath.clear();
        lastPianoRollPreviewNotes.clear();
        lastPianoRollPreviewDurationBeats = 0.0;
        pendingPianoRollPreviewStartSeconds = 0.0;
    }

void MainComponent::renderPianoRollPreview()
    {
        applyPianoRollSettings();
        lastPianoRollPreviewScope = 0;

        if (!currentProject)
        {
            currentProject = mw::core::Project("Piano Roll Preview Project");
            currentProject->addTrack("Piano Roll Track");
            baseNameBox.setText("piano_roll_preview_project");
            refreshTrackSelector();
            trackCombo.setSelectedId(1, juce::sendNotification);
            logMessage("Created temporary project for piano roll preview.");
        }

        const auto selectedIndex = getSelectedTrackIndex();
        const auto index = pianoRollOpenTrackIndex >= 0 ? pianoRollOpenTrackIndex : selectedIndex;

        if (index < 0 || index >= static_cast<int>(currentProject->getTracks().size()))
        {
            logMessage("ERROR: Select a track before rendering piano roll preview.");
            return;
        }

        if (renderingInProgress)
        {
            logMessage("Render already in progress. Preview skipped.");
            return;
        }

        auto selectedTrack = currentProject->getTracks()[static_cast<std::size_t>(index)];

        if (index == pianoRollOpenTrackIndex)
            selectedTrack.getNotes() = pianoRoll.getNotes();

        if (selectedTrack.getNotes().empty())
        {
            logMessage("ERROR: Selected track has no notes to preview. Open the Piano Roll and click the grid to add notes first.");
            return;
        }

        auto sanitizePreviewStem = [](const std::string& value)
        {
            std::string out;

            for (char c : value)
            {
                if ((c >= 'a' && c <= 'z')
                    || (c >= 'A' && c <= 'Z')
                    || (c >= '0' && c <= '9')
                    || c == '-'
                    || c == '_')
                {
                    out.push_back(c);
                }
                else
                {
                    out.push_back('_');
                }
            }

            return out.empty() ? std::string("track") : out;
        };

        const bool trackExplicitSfz =
            selectedTrack.getInstrument().backendType == mw::core::SampleBackendType::SFZ;

        const bool trackExplicitSf2 =
            selectedTrack.getInstrument().backendType == mw::core::SampleBackendType::SF2;

        const bool trackExplicitVst =
            selectedTrack.getInstrument().backendType == mw::core::SampleBackendType::VST3;

        const bool backendIsVst =
            trackExplicitVst
            || trackBackendCombo.getSelectedId() == 4
            || (!trackExplicitSf2 && !trackExplicitSfz && trackBackendCombo.getSelectedId() == 1 && appliedProjectBackendId == 3);

        const bool backendIsSfz =
            !backendIsVst
            && (trackExplicitSfz
                || (!trackExplicitSf2
                    && (trackBackendCombo.getSelectedId() == 3
                        || (trackBackendCombo.getSelectedId() == 1 && appliedProjectBackendId == 2))));

        auto soundFontPath =
            (!backendIsSfz && !selectedTrack.getInstrument().sampleLibraryPath.empty())
                ? selectedTrack.getInstrument().sampleLibraryPath
                : (getSelectedSoundFontPath().empty()
                    ? std::filesystem::path(soundFontPathBox.getText().toStdString())
                    : getSelectedSoundFontPath());

        auto sfzPath =
            (backendIsSfz && !selectedTrack.getInstrument().sampleLibraryPath.empty())
                ? selectedTrack.getInstrument().sampleLibraryPath
                : (getSelectedSfzPath().empty()
                    ? std::filesystem::path(sfzPathBox.getText().toStdString())
                    : getSelectedSfzPath());

        const auto fluidSynthPath = std::filesystem::path(fluidSynthPathBox.getText().toStdString());
        const auto sfizzPath = std::filesystem::path(sfizzPathBox.getText().toStdString());

        if (backendIsVst)
        {
            showVstExperimentalWarningIfNeeded();

            auto repairedAssignment = selectedTrack.getInstrument();
            if (repairVst3BundlePathIfPossible(repairedAssignment))
                selectedTrack.setInstrumentAssignment(repairedAssignment);

            const auto vstBundlePath = resolveVst3BundlePath(selectedTrack.getInstrument());
            if (vstBundlePath.empty() || !std::filesystem::exists(vstBundlePath))
            {
                logMessage("ERROR: Cannot preview VST3. Plugin bundle was not found: " + vstBundlePath.string());
                return;
            }

            if (vstPluginEditorWindows.find(index) != vstPluginEditorWindows.end())
            {
                logMessage("VST3 Piano Roll preview will use the last applied plugin state. Click Apply Changes in the plugin window after editing controls.");
            }
        }
        else if (backendIsSfz)
        {
            if (sfizzPath.empty() || !std::filesystem::exists(sfizzPath))
            {
                logMessage("ERROR: Cannot preview SFZ. sfizz-render was not found: " + sfizzPath.string());
                return;
            }

            if (sfzPath.empty() || !std::filesystem::exists(sfzPath))
            {
                logMessage("ERROR: Cannot preview SFZ. SFZ file was not found: " + sfzPath.string());
                return;
            }
        }
        else
        {
            if (fluidSynthPath.empty() || !std::filesystem::exists(fluidSynthPath))
            {
                logMessage("ERROR: Cannot preview SF2. FluidSynth was not found: " + fluidSynthPath.string());
                return;
            }

            if (soundFontPath.empty() || !std::filesystem::exists(soundFontPath))
            {
                logMessage("ERROR: Cannot preview SF2. SoundFont was not found: " + soundFontPath.string());
                return;
            }
        }

        auto previewProject = mw::core::Project("Piano Roll Preview");
        previewProject.setTempoBpm(currentProject->getTempoBpm());
        previewProject.setTimeSignature(currentProject->getTimeSignature());

        // Force a safe channel for preview. Some imported tracks or manually added
        // tracks can have unusual channel data, which makes preview harder to diagnose.
        auto assignment = selectedTrack.getInstrument();
        if (assignment.midiChannel <= 0 || assignment.midiChannel > 16)
            assignment.midiChannel = 1;
        selectedTrack.setInstrumentAssignment(assignment);

        previewProject.getTracks().push_back(selectedTrack);

        const auto previewBaseName =
            "piano_roll_preview_" + sanitizePreviewStem(selectedTrack.getName());

        const auto previewFolder = mw::app::AppPaths::previewFolder();
        std::filesystem::create_directories(previewFolder);

        const auto previewMidiPath = previewFolder / (previewBaseName + ".mid");
        const auto previewWavPath = previewFolder / (previewBaseName + ".wav");

        cleanupPianoRollPreviewFiles();

        {
            std::error_code ignored;
            std::filesystem::remove(previewMidiPath, ignored);
            std::filesystem::remove(previewWavPath, ignored);
        }

        lastPianoRollPreviewMidiPath = previewMidiPath;
        lastPianoRollPreviewWavPath = previewWavPath;
        setPianoRollPreviewNoteMapFromTracks(previewProject.getTracks());

        std::int64_t previewEndTick = 0;

        for (const auto& note : selectedTrack.getNotes())
            previewEndTick = std::max(previewEndTick, note.startTick + note.durationTicks);

        lastPianoRollPreviewDurationBeats =
            std::max(
                0.01,
                static_cast<double>(previewEndTick)
                / static_cast<double>(mw::core::Project::ticksPerQuarterNote)
            );

        lastPianoRollPreviewTempoBpm = previewProject.getTempoBpm();
        pendingPianoRollPreviewStartSeconds = 0.0;

        logMessage("Preview backend: " + juce::String(backendIsVst ? "VST3 plugin" : (backendIsSfz ? "SFZ / sfizz-render" : "SF2 / FluidSynth")));
        logMessage("Preview track: " + selectedTrack.getName());
        logMessage("Preview note count: " + juce::String(static_cast<int>(selectedTrack.getNotes().size())));
        logMessage("Preview temporary MIDI: " + previewMidiPath.string());
        logMessage("Preview temporary WAV: " + previewWavPath.string());

        if (!mw::midi::MidiExporter::exportToFile(previewProject, previewMidiPath))
        {
            logMessage("ERROR: Failed to export preview MIDI.");
            return;
        }

        if (!std::filesystem::exists(previewMidiPath))
        {
            logMessage("ERROR: Preview MIDI export reported success, but file was not found.");
            return;
        }

        logMessage("Preview MIDI exported successfully.");

        const int previewTempoBpm = previewProject.getTempoBpm();
        const int previewSampleRate = sampleRateCombo.getText().getIntValue() > 0 ? sampleRateCombo.getText().getIntValue() : 48000;
        const int previewChannelCount = channelsCombo.getSelectedId() > 0 ? channelsCombo.getSelectedId() : 2;

        cancelRenderRequested = false;
        setRenderingState(true);
        logMessage("Rendering piano roll preview audio...");

        if (renderThread.joinable())
            renderThread.join();

        renderThread = std::thread(
            [this, previewMidiPath, previewWavPath, backendIsVst, backendIsSfz, soundFontPath, sfzPath, fluidSynthPath, sfizzPath, selectedTrack, previewTempoBpm, previewSampleRate, previewChannelCount]
            {
                bool success = false;
                std::string finalMessage;

                if (backendIsVst)
                {
                    mw::vst::VstRenderRequest request;
                    request.track = selectedTrack;
                    request.tempoBpm = previewTempoBpm;
                    request.sampleRate = previewSampleRate;
                    request.channelCount = previewChannelCount;
                    request.blockSize = 512;
                    request.wavOutputPath = previewWavPath;
                    request.cancelRequested = &cancelRenderRequested;

                    const auto result = mw::vst::VstInstrumentHost::renderTrackToWav(request);
                    logMessage(result.message);
                    success = result.success && std::filesystem::exists(previewWavPath);
                    finalMessage = success
                        ? "Piano roll VST3 preview rendered temporary WAV: " + previewWavPath.string()
                        : "ERROR: Piano roll VST3 preview render failed or WAV was not created.";
                }
                else if (backendIsSfz)
                {
                    const auto validation = mw::audio::SfzValidator::validateSampleReferences(sfzPath);
                    logMessage(validation.message);

                    if (!validation.ok)
                    {
                        finalMessage = "ERROR: Preview SFZ samples are missing.";
                    }
                    else
                    {
                        mw::audio::SfizzRenderRequest request;
                        request.sfizzRenderExePath = sfizzPath;
                        request.sfzPath = sfzPath;
                        request.midiInputPath = previewMidiPath;
                        request.wavOutputPath = previewWavPath;

                        const auto result = mw::audio::ExternalSfizzRenderer::renderMidiToWav(request);
                        logMessage("Preview sfizz command: " + result.commandLine);
                        logMessage(result.message);

                        success = result.success && std::filesystem::exists(previewWavPath);
                        finalMessage = success
                            ? "Piano roll preview rendered temporary WAV: " + previewWavPath.string()
                            : "ERROR: Piano roll SFZ preview render failed or WAV was not created.";
                    }
                }
                else
                {
                    mw::audio::FluidSynthRenderRequest request;
                    request.fluidSynthExePath = fluidSynthPath;
                    request.soundFontPath = soundFontPath;
                    request.midiInputPath = previewMidiPath;
                    request.wavOutputPath = previewWavPath;

                    const auto result = mw::audio::ExternalFluidSynthRenderer::renderMidiToWav(request);
                    logMessage("Preview FluidSynth command: " + result.commandLine);
                    logMessage(result.message);

                    success = result.success && std::filesystem::exists(previewWavPath);
                    finalMessage = success
                        ? "Piano roll preview rendered temporary WAV: " + previewWavPath.string()
                        : "ERROR: Piano roll SF2 preview render failed or WAV was not created.";
                }

                juce::MessageManager::callAsync(
                    [this, success, finalMessage, previewWavPath]
                    {
                        logMessage(finalMessage);

                        if (success)
                        {
                            logMessage("Preview files are temporary. Closing Piano Roll or Clean Temp removes them.");
                            if (std::filesystem::exists(previewWavPath))
                            {
                                lastPianoRollPreviewWavPath = previewWavPath;
                                setRenderingState(false);
                                logMessage("Preview rendered. Playing inside the Preview Player.");
                                playPianoRollPreviewFile(previewWavPath);
                                return;
                            }
                            else
                            {
                                logMessage("Preview reported success, but WAV was not found: " + previewWavPath.string());
                            }
                        }

                        setRenderingState(false);
                    }
                );
            }
        );
    }

    void MainComponent::populateNoteEditorFromNotes(const std::vector<mw::core::NoteEvent>& notes)
    {
        juce::String text;
        text << "pitch,startBeat,durationBeats,velocity\n";

        for (const auto& note : notes)
        {
            const double startBeat = static_cast<double>(note.startTick) / static_cast<double>(mw::core::Project::ticksPerQuarterNote);
            const double durationBeats = static_cast<double>(note.durationTicks) / static_cast<double>(mw::core::Project::ticksPerQuarterNote);

            text << note.pitch
                 << ","
                 << juce::String(startBeat, 3)
                 << ","
                 << juce::String(durationBeats, 3)
                 << ","
                 << note.velocity
                 << "\n";
        }

        noteEditorBox.setText(text, juce::dontSendNotification);
    }

    void MainComponent::refreshNoteEditor()
    {
        if (!currentProject)
        {
            noteEditorBox.setText("pitch,startBeat,durationBeats,velocity\n");
            return;
        }

        const auto index = getSelectedTrackIndex();

        if (index < 0 || index >= static_cast<int>(currentProject->getTracks().size()))
            return;

        const auto& track = currentProject->getTracks()[static_cast<std::size_t>(index)];
        auto* openState = findPianoRollEditorWindow(index);
        const bool keepLocalPianoRollEdits = openState != nullptr && openState->dirty;

        if (keepLocalPianoRollEdits)
            populateNoteEditorFromNotes(openState->roll.getNotes());
        else
            populateNoteEditorFromNotes(track.getNotes());

        if (currentProject)
            pianoRoll.setTempoBpm(currentProject->getTempoBpm());

        if (!keepLocalPianoRollEdits)
        {
            suppressPianoRollEditorDirty = true;
            pianoRoll.setNotesPreservingHistory(track.getNotes());
            suppressPianoRollEditorDirty = false;
        }
    }

    void MainComponent::applyProjectTimingSettings()
    {
        if (!currentProject)
            return;

        try
        {
            const int tempo = std::stoi(tempoBox.getText().toStdString());
            if (tempo > 0 && tempo <= 400)
                currentProject->setTempoBpm(tempo);
        }
        catch (...)
        {
            logMessage("Invalid tempo. Use a number like 120.");
        }

        const auto ts = timeSignatureBox.getText().toStdString();
        const auto slash = ts.find('/');

        if (slash != std::string::npos)
        {
            try
            {
                const int numerator = std::stoi(ts.substr(0, slash));
                const int denominator = std::stoi(ts.substr(slash + 1));

                if (numerator > 0 && denominator > 0)
                    currentProject->setTimeSignature({ numerator, denominator });
            }
            catch (...)
            {
                logMessage("Invalid time signature. Use a value like 4/4.");
            }
        }

        pianoRoll.setTempoBpm(currentProject->getTempoBpm());
        pianoRoll.repaint();
        updateTrackSummary(*currentProject);
        logMessage("Applied project timing.");
    }

    void MainComponent::applyNoteEditorToTrack()
    {
        if (!currentProject)
            return;

        const auto index = getSelectedTrackIndex();

        if (index < 0 || index >= static_cast<int>(currentProject->getTracks().size()))
            return;

        auto& track = currentProject->getTracks()[static_cast<std::size_t>(index)];
        track.getNotes().clear();

        const auto lines = juce::StringArray::fromLines(noteEditorBox.getText());

        int lineNumber = 0;
        for (const auto& line : lines)
        {
            ++lineNumber;
            const auto trimmed = line.trim();

            if (trimmed.isEmpty() || trimmed.startsWithIgnoreCase("pitch"))
                continue;

            auto parts = juce::StringArray::fromTokens(trimmed, ",", "");
            parts.trim();
            parts.removeEmptyStrings();

            if (parts.size() < 4)
            {
                logMessage("Skipped note row " + juce::String(lineNumber) + ": expected pitch,startBeat,durationBeats,velocity");
                continue;
            }

            try
            {
                const int pitch = std::clamp(std::stoi(parts[0].toStdString()), 0, 127);
                const double startBeat = std::stod(parts[1].toStdString());
                const double durationBeats = std::max(0.001, std::stod(parts[2].toStdString()));
                const int velocity = std::clamp(std::stoi(parts[3].toStdString()), 1, 127);

                const auto startTick = static_cast<std::int64_t>(startBeat * mw::core::Project::ticksPerQuarterNote);
                const auto durationTicks = static_cast<std::int64_t>(durationBeats * mw::core::Project::ticksPerQuarterNote);

                track.addNote(
                    mw::core::NoteEvent(
                        pitch,
                        velocity,
                        startTick,
                        durationTicks,
                        track.getInstrument().midiChannel,
                        mw::core::Articulation::Normal
                    )
                );
            }
            catch (...)
            {
                logMessage("Skipped invalid note row " + juce::String(lineNumber));
            }
        }

        int loopCount = 1;
        try { loopCount = std::max(1, std::stoi(loopCountBox.getText().toStdString())); } catch (...) {}

        if (loopCount > 1 && !track.getNotes().empty())
        {
            auto originalNotes = track.getNotes();

            std::int64_t loopLengthTicks = 0;
            for (const auto& note : originalNotes)
                loopLengthTicks = std::max(loopLengthTicks, note.startTick + note.durationTicks);

            for (int loop = 1; loop < loopCount; ++loop)
            {
                for (auto note : originalNotes)
                {
                    note.startTick += loopLengthTicks * loop;
                    track.addNote(note);
                }
            }

            logMessage("Applied note loop count: " + juce::String(loopCount));
        }

        updateTrackSummary(*currentProject);
        refreshNoteEditor();
        setProjectDirty();
        logMessage("Applied note editor to track: " + track.getName());
    }

    void MainComponent::addNoteToSelectedTrack()
    {
        if (!currentProject)
            return;

        const auto index = getSelectedTrackIndex();

        if (index < 0 || index >= static_cast<int>(currentProject->getTracks().size()))
            return;

        auto& track = currentProject->getTracks()[static_cast<std::size_t>(index)];

        std::int64_t nextStart = 0;
        for (const auto& note : track.getNotes())
            nextStart = std::max(nextStart, note.startTick + note.durationTicks);

        track.addNote(
            mw::core::NoteEvent(
                60,
                100,
                nextStart,
                mw::core::Project::ticksPerQuarterNote,
                track.getInstrument().midiChannel,
                mw::core::Articulation::Normal
            )
        );

        refreshNoteEditor();
        updateTrackSummary(*currentProject);
        setProjectDirty();
        logMessage("Added note to track: " + track.getName());
    }

    void MainComponent::removeLastNoteFromSelectedTrack()
    {
        if (!currentProject)
            return;

        const auto index = getSelectedTrackIndex();

        if (index < 0 || index >= static_cast<int>(currentProject->getTracks().size()))
            return;

        auto& notes = currentProject->getTracks()[static_cast<std::size_t>(index)].getNotes();

        if (!notes.empty())
            notes.pop_back();

        refreshNoteEditor();
        updateTrackSummary(*currentProject);
        logMessage("Removed last note from selected track.");
    }

    void MainComponent::updateVolumeLabels()
    {
        juce::String trackText;
        trackText << "Track Vol: " << juce::String(trackVolumeSlider.getValue(), 2);
        trackVolumeLabel.setText(trackText, juce::dontSendNotification);

        juce::String masterText;
        masterText << "Master Vol: " << juce::String(masterVolumeSlider.getValue(), 2);
        masterVolumeLabel.setText(masterText, juce::dontSendNotification);
    }

    void MainComponent::loadThemePresets()
    {
        ensureDefaultThemeFiles();

        themePresetFiles.clear();
        themeCombo.clear(juce::dontSendNotification);

        const auto folder = mw::app::AppPaths::themesFolder();

        if (std::filesystem::exists(folder))
        {
            for (const auto& entry : std::filesystem::directory_iterator(folder))
            {
                if (!entry.is_regular_file())
                    continue;

                auto ext = entry.path().extension().string();

                for (auto& c : ext)
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

                if (ext == ".xml")
                    themePresetFiles.push_back(entry.path());
            }
        }

        std::sort(themePresetFiles.begin(), themePresetFiles.end());

        if (themePresetFiles.empty())
        {
            const auto fallback = mw::app::AppPaths::themesFolder() / "01_Default.xml";
            themePresetFiles.push_back(fallback);
        }

        for (int i = 0; i < static_cast<int>(themePresetFiles.size()); ++i)
        {
            const auto preset = readThemePresetFromXml(themePresetFiles[static_cast<std::size_t>(i)], i + 1);
            themeCombo.addItem(themeDisplayName(preset.name), i + 1);
        }

        if (themeCombo.getNumItems() == 0)
            themeCombo.addItem("Default", 1);

        themeCombo.setSelectedId(1, juce::dontSendNotification);
    }

    void MainComponent::applyThemePreset(int presetId)
    {
        currentThemePresetId = std::clamp(presetId, 1, std::max(1, static_cast<int>(themePresetFiles.size())));
        const auto theme = getThemePresetFromFileList(themePresetFiles, currentThemePresetId);

        auto applyLabel = [theme](juce::Label& label)
        {
            label.setColour(juce::Label::textColourId, theme.text);
        };

        auto applyTextEditor = [theme](juce::TextEditor& editor)
        {
            editor.setColour(juce::TextEditor::backgroundColourId, theme.control);
            editor.setColour(juce::TextEditor::textColourId, theme.text);
            editor.setColour(juce::TextEditor::outlineColourId, theme.panel.brighter(0.35f));
            editor.setColour(juce::TextEditor::focusedOutlineColourId, theme.accent);
            editor.setColour(juce::TextEditor::highlightColourId, theme.accent.withAlpha(0.35f));
        };

        auto applyButton = [theme](juce::TextButton& button)
        {
            button.setColour(juce::TextButton::buttonColourId, theme.control);
            button.setColour(juce::TextButton::buttonOnColourId, theme.accent.darker(0.35f));
            button.setColour(juce::TextButton::textColourOffId, theme.text);
            button.setColour(juce::TextButton::textColourOnId, juce::Colours::white);
        };

        auto applyCombo = [theme](juce::ComboBox& combo)
        {
            combo.setColour(juce::ComboBox::backgroundColourId, theme.control);
            combo.setColour(juce::ComboBox::textColourId, theme.text);
            combo.setColour(juce::ComboBox::outlineColourId, theme.panel.brighter(0.35f));
            combo.setColour(juce::ComboBox::arrowColourId, theme.accent);
        };

        auto applySlider = [theme](juce::Slider& slider)
        {
            slider.setColour(juce::Slider::backgroundColourId, theme.control.darker(0.25f));
            slider.setColour(juce::Slider::trackColourId, theme.accent);
            slider.setColour(juce::Slider::thumbColourId, theme.text);
            slider.setColour(juce::Slider::textBoxTextColourId, theme.text);
            slider.setColour(juce::Slider::textBoxBackgroundColourId, theme.control);
            slider.setColour(juce::Slider::textBoxOutlineColourId, theme.panel.brighter(0.35f));
        };

        for (auto* label : {
            &musicXmlLabel, &exportFolderLabel, &projectDefaultsLabel, &soundFontLabel, &fluidSynthLabel, &ffmpegLabel,
            &backendLabel, &sfzLabel, &sfizzLabel, &sfzKeySwitchLabel, &sfzCc1Label, &sfzCc11Label,
            &baseNameLabel, &outputFormatLabel, &sampleRateLabel, &bitrateLabel, &channelsLabel, &renderWorkersLabel, &renderOutputSummaryLabel, &themeLabel, &trackLabel, &sequenceSelectorLabel, &sequenceThoughtsLabel,
            &trackSoundLibraryLabel, &instrumentLabel, &trackVolumeLabel, &masterVolumeLabel,
            &tempoLabel, &timeSignatureLabel, &loopCountLabel, &noteEditorLabel, &pianoRollBpmLabel,
            &pianoRollTimeSigLabel, &pianoRollBeatWindowLabel, &pianoRollStartBeatLabel,
            &pianoRollNoteLengthLabel, &pianoRollVelocityLabel, &pianoRollSnapLabel,
            &pianoRollPageInputLabel, &pianoRollKeyRangeLabel, &pianoRollKeyJumpLabel
        })
            applyLabel(*label);

        titleLabel.setColour(juce::Label::textColourId, theme.text);
        pianoRollHelpLabel.setColour(juce::Label::textColourId, theme.text.withAlpha(0.85f));
        pianoRollPageLabel.setColour(juce::Label::textColourId, theme.text.withAlpha(0.85f));
        renderStatusLabel.setColour(juce::Label::textColourId, juce::Colours::lightgreen);

        for (auto* editor : {
            &musicXmlPathBox, &exportFolderBox, &soundFontPathBox, &fluidSynthPathBox, &ffmpegPathBox,
            &sfzPathBox, &sfizzPathBox, &sfzKeySwitchBox, &sfzCc1Box, &sfzCc11Box, &baseNameBox,
            &tempoBox, &timeSignatureBox, &loopCountBox, &pianoRollBpmBox, &pianoRollTimeSigBox,
            &pianoRollBeatWindowBox, &pianoRollStartBeatBox, &pianoRollNoteLengthBox,
            &pianoRollVelocityBox, &pianoRollPageBox, &pianoRollKeyJumpBox, &trackSoundLibraryBox,
            &trackSummaryBox, &sequenceSelectorBox, &sequenceThoughtsBox, &trackManagerSelectBox, &trackManagerStartBeatBox,
            &trackManagerMapStartBeatBox, &trackManagerMapBeatWindowBox,
            &noteEditorBox, &pianoRollSnapBox
        })
            applyTextEditor(*editor);

        // The status console is intentionally theme-proof: black background,
        // fixed readable text, and sequence colour rails instead of coloured text.
        logBox.setColour(juce::TextEditor::backgroundColourId, juce::Colours::black);
        logBox.setColour(juce::TextEditor::textColourId, juce::Colours::white);
        logBox.setColour(juce::TextEditor::outlineColourId, juce::Colours::grey);
        logBox.setColour(juce::TextEditor::focusedOutlineColourId, juce::Colours::yellow);
        trackManagerBox.setColour(juce::TextEditor::backgroundColourId, juce::Colours::black);
        trackManagerBox.setColour(juce::TextEditor::textColourId, juce::Colours::white);
        trackManagerBox.setColour(juce::TextEditor::outlineColourId, juce::Colours::grey);

        for (auto* combo : {
            &soundFontCombo, &backendCombo, &sfzCombo, &outputFormatCombo, &audioClipFormatCombo, &audioClipQualityCombo, &sampleRateCombo, &bitrateCombo, &channelsCombo, &renderWorkersCombo,
            &themeCombo, &trackCombo, &instrumentCombo, &trackBackendCombo, &pianoRollBeatWindowCombo
        })
            applyCombo(*combo);

        for (auto* button : {
            &chooseMusicXmlButton, &importAudioButton, &recordAudioButton, &newProjectButton, &openProjectButton, &saveProjectButton, &cleanTempButton,
            &saveSettingsButton,
            &changeActiveSequenceButton, &editInfoButton, &exportFolderButton, &browseSoundFontButton,
            &refreshSoundFontsButton, &sfzButton, &refreshSfzButton,
            &sfzTestButton, &applyBackendButton, &applyTrackButton, &changeTrackLibraryButton, &trackSfzButton, &addTrackButton,
            &duplicateTrackButton, &removeTrackButton, &renameTrackButton, &trackManagerButton,
            &applyTimingButton, &applyNotesButton, &showRawNotesButton,
            &addNoteButton, &removeNoteButton, &deletePianoRollNoteButton, &syncPianoRollButton,
            &openPianoRollButton, &previousPianoRollWindowButton,
            &nextPianoRollWindowButton, &jumpPianoRollPageButton, &jumpPianoRollKeyButton,
            &copyPianoRollNoteButton, &pastePianoRollNoteButton, &undoPianoRollButton,
            &redoPianoRollButton, &clearPianoRollSelectionButton, &openPianoRollPreviewPlayerButton,
            &playProjectPreviewButton, &stopProjectPreviewButton, &renderButton, &renderSelectedTrackButton, &renderSelectedSequenceButton, &renderMidiButton, &cancelRenderButton, &renderSettingsButton,
            &previewButton, &setBeatWindow4Button, &setBeatWindow8Button, &setBeatWindow16Button,
            &setBeatWindow32Button, &setBeatWindow64Button
        })
            applyButton(*button);

        applySlider(trackVolumeSlider);
        applySlider(masterVolumeSlider);

        muteToggle.setColour(juce::ToggleButton::textColourId, theme.text);
        muteToggle.setColour(juce::ToggleButton::tickColourId, theme.accent);
        soloToggle.setColour(juce::ToggleButton::textColourId, theme.text);
        soloToggle.setColour(juce::ToggleButton::tickColourId, theme.accent);

        repaint();
    }

    void MainComponent::saveThemePreference()
    {
        mw::app::UserPreferencesStore::saveIntValue("themePresetId", currentThemePresetId);
        logMessage("Theme preset saved: " + getThemePresetFromFileList(themePresetFiles, currentThemePresetId).name);
    }

    void MainComponent::saveUserSettingsNow()
    {
        auto preferences = mw::app::UserPreferencesStore::load();

        preferences.lastExportFolder = std::filesystem::path(exportFolderBox.getText().toStdString());
        preferences.lastSoundFontPath =
            getSelectedSoundFontPath().empty()
                ? std::filesystem::path(soundFontPathBox.getText().toStdString())
                : getSelectedSoundFontPath();
        preferences.lastSfzPath =
            getSelectedSfzPath().empty()
                ? std::filesystem::path(sfzPathBox.getText().toStdString())
                : getSelectedSfzPath();
        preferences.lastBackendId = appliedProjectBackendId > 0 ? appliedProjectBackendId : 1;
        preferences.lastOutputFormatId = outputFormatCombo.getSelectedId();
        preferences.lastAudioClipFormatId = audioClipFormatCombo.getSelectedId();
        preferences.lastAudioClipQualityKbps = audioClipQualityCombo.getSelectedId();
        preferences.lastSampleRate = sampleRateCombo.getSelectedId();
        preferences.lastBitrateKbps = bitrateCombo.getSelectedId();
        preferences.lastChannelCount = channelsCombo.getSelectedId();
        preferences.lastRenderWorkerCount = renderWorkersCombo.getSelectedId() == 100 ? 0 : renderWorkersCombo.getSelectedId();
        preferences.themePresetId = currentThemePresetId;
        preferences.helperBubblesEnabled = helperBubblesEnabled;
        preferences.vstCompatibilityWarningsEnabled = vstCompatibilityWarningsEnabled;
        preferences.vstSafePluginUiMode = vstSafePluginUiMode;
        preferences.vstWarningStyleId = vstWarningStyleId;
        preferences.vstGraphicsProfileDetected = vstGraphicsProfile.detected;
        preferences.vstGraphicsProfileSource = vstGraphicsProfile.source;
        preferences.vstGraphicsProfileLastDetected = vstGraphicsProfile.lastDetectedLocal;
        preferences.vstPreferredPluginGpuId = vstGraphicsProfile.preferredPluginGpuId.empty() ? std::string("auto") : vstGraphicsProfile.preferredPluginGpuId;
        preferences.vstMaxOpenPluginWindows = sanitizeMaxOpenVstPluginWindows(vstMaxOpenPluginWindows);
        preferences.vstGraphicsProfileSummary = vstGraphicsProfile.summary();
        preferences.vstExperimentalWarningAcknowledged = vstExperimentalWarningAcknowledged;

        if (mw::app::UserPreferencesStore::save(preferences))
            logMessage("Settings saved.");
        else
            logMessage("ERROR: Failed to save settings.");
    }

    juce::Colour MainComponent::getConsoleColourForMessage(const juce::String& message) const
    {
        auto findSequenceNumber = [&message](const juce::String& marker) -> int
        {
            const auto lower = message.toLowerCase();
            const auto pos = lower.indexOf(marker.toLowerCase());

            if (pos < 0)
                return 0;

            auto tail = message.substring(pos + marker.length()).trimStart();

            if (tail.startsWithChar('#'))
                tail = tail.substring(1).trimStart();

            return tail.getIntValue();
        };

        int sequenceNumber = findSequenceNumber("sequence ");

        if (sequenceNumber <= 0)
            sequenceNumber = findSequenceNumber("seq ");

        if (sequenceNumber <= 0)
            sequenceNumber = findSequenceNumber("sequence #");

        if (sequenceNumber <= 0)
            sequenceNumber = findSequenceNumber("seq #");

        if (sequenceNumber > 0 && sequenceNumber <= static_cast<int>(importSections.size()))
            return getSequenceColourForIndex(sequenceNumber - 1);

        return juce::Colours::white;
    }

    void MainComponent::logMessage(const juce::String& message)
    {
        auto appendMessage = [this](const juce::String& rawMessage)
        {
            const auto colour = getConsoleColourForMessage(rawMessage);
            const bool hasSequenceColour = (colour != juce::Colours::white);
            logBox.appendMessage(rawMessage, colour, hasSequenceColour);
        };

        if (juce::MessageManager::getInstance()->isThisTheMessageThread())
        {
            appendMessage(message);
            return;
        }

        juce::MessageManager::callAsync(
            [this, message]
            {
                const auto colour = getConsoleColourForMessage(message);
                const bool hasSequenceColour = (colour != juce::Colours::white);
                logBox.appendMessage(message, colour, hasSequenceColour);
            }
        );
    }

    void MainComponent::refreshMainSequenceSelector()
    {
        if (!currentProject)
        {
            sequenceSelectorBox.setText("No Sequence", juce::dontSendNotification);
            refreshActiveSequenceThoughtsEditor();
            return;
        }

        rebuildSectionsFromTracksIfNeeded();

        if (importSections.empty())
        {
            sequenceSelectorBox.setText("No Sequence", juce::dontSendNotification);
            refreshActiveSequenceThoughtsEditor();
            return;
        }

        // Preserve an explicitly selected Track Manager / sequence-map target.
        // The selected track may legitimately belong to another sequence, and
        // preview/render scope should follow the active sequence instead of
        // snapping back to the selected track's group.
        if (activeImportSectionIndex < 0 || activeImportSectionIndex >= static_cast<int>(importSections.size()))
            updateActiveSequenceFromSelectedTrack();

        if (activeImportSectionIndex >= 0 && activeImportSectionIndex < static_cast<int>(importSections.size()))
        {
            const auto& section = importSections[static_cast<std::size_t>(activeImportSectionIndex)];
            juce::String label;
            label << "Seq #" << (activeImportSectionIndex + 1)
                  << (section.locked ? " [LOCKED] - " : " - ")
                  << section.name;
            sequenceSelectorBox.setText(label, juce::dontSendNotification);
        }
        else
        {
            sequenceSelectorBox.setText("No Sequence", juce::dontSendNotification);
        }

        refreshActiveSequenceThoughtsEditor();
    }

    void MainComponent::refreshActiveSequenceThoughtsEditor()
    {
        juce::ScopedValueSetter<bool> guard(suppressSequenceThoughtsChange, true);

        if (!currentProject || activeImportSectionIndex < 0 || activeImportSectionIndex >= static_cast<int>(importSections.size()))
        {
            sequenceThoughtsBox.setReadOnly(true);
            sequenceThoughtsBox.setText({}, juce::dontSendNotification);
            return;
        }

        sequenceThoughtsBox.setReadOnly(true);
        sequenceThoughtsBox.setText(importSections[static_cast<std::size_t>(activeImportSectionIndex)].notes, juce::dontSendNotification);
    }

    void MainComponent::syncSequenceThoughtsFromEditor()
    {
        if (!currentProject || activeImportSectionIndex < 0 || activeImportSectionIndex >= static_cast<int>(importSections.size()))
            return;

        auto& section = importSections[static_cast<std::size_t>(activeImportSectionIndex)];
        const auto nextNotes = sequenceThoughtsBox.getText();

        if (section.notes == nextNotes)
            return;

        section.notes = nextNotes;
        syncSequencesToProjectMetadata();
        setProjectDirty();
    }

    void MainComponent::closeSequenceListPickerWindow()
    {
        juce::MessageManager::callAsync(
            [this]
            {
                sequencePickerWindow.reset();
            }
        );
    }

    void MainComponent::showSequenceListPickerWindow(
        const juce::String& title,
        const juce::String& instructions,
        int initialSequenceNumber,
        std::function<void(int)> applyExistingCallback,
        std::function<std::optional<std::pair<int, juce::String>>()> createNewCallback
    )
    {
        rebuildSectionsFromTracksIfNeeded();
        normalizeEmptySequencesAfterMembershipChange();
        syncSequencesToProjectMetadata();
        refreshTrackManagerText(false);

        std::vector<SequencePickerChoice> choices;
        choices.reserve(importSections.size());

        for (int i = 0; i < static_cast<int>(importSections.size()); ++i)
        {
            const auto& section = importSections[static_cast<std::size_t>(i)];
            SequencePickerChoice choice;
            choice.number = i + 1;
            choice.name = section.name;
            choice.notes = section.notes;
            choice.locked = section.locked;
            choice.trackCount = static_cast<int>(section.trackNumbers.size());
            choice.trackSummary = formatSequenceTrackSummary(i);

            choices.push_back(std::move(choice));
        }

        sequencePickerWindow.reset();

        auto closeCallback = [this]
        {
            closeSequenceListPickerWindow();
        };

        auto* window = new SequencePickerDocumentWindow(title, closeCallback);
        auto* content = new SequencePickerContent(
            title,
            instructions,
            std::move(choices),
            initialSequenceNumber,
            [this, applyExistingCallback = std::move(applyExistingCallback)](int sequenceNumber) mutable
            {
                if (applyExistingCallback)
                    applyExistingCallback(sequenceNumber);

                closeSequenceListPickerWindow();
            },
            [createNewCallback = std::move(createNewCallback)]() mutable -> std::optional<std::pair<int, juce::String>>
            {
                if (createNewCallback)
                    return createNewCallback();

                return std::nullopt;
            },
            [this]
            {
                logMessage("Sequence picker cancelled.");
                closeSequenceListPickerWindow();
            }
        );

        window->setContentOwned(content, true);
        window->centreWithSize(900, 500);
        window->setVisible(true);
        window->toFront(true);
        sequencePickerWindow.reset(window);
    }

    void MainComponent::showChangeActiveSequenceDialog()
    {
        if (!currentProject)
        {
            logMessage("Change Track Seq: no project loaded.");
            return;
        }

        rebuildSectionsFromTracksIfNeeded();
        normalizeEmptySequencesAfterMembershipChange();
        syncSequencesToProjectMetadata();

        const int selectedTrackIndex = getSelectedTrackIndex();
        const int selectedTrackNumber = selectedTrackIndex >= 0 ? selectedTrackIndex + 1 : 0;
        const bool hasSelectedTrack = selectedTrackNumber > 0
            && selectedTrackNumber <= static_cast<int>(currentProject->getTracks().size());

        int initialSequenceNumber = activeImportSectionIndex >= 0 ? activeImportSectionIndex + 1 : 1;

        if (hasSelectedTrack)
        {
            const int selectedTrackSequence = getSequenceIndexForTrack(selectedTrackNumber) + 1;
            if (selectedTrackSequence > 0)
                initialSequenceNumber = selectedTrackSequence;

            trackManagerSelectBox.setText(juce::String(selectedTrackNumber), juce::dontSendNotification);
            if (initialSequenceNumber > 0 && initialSequenceNumber <= static_cast<int>(importSections.size()))
                trackManagerSectionBox.setText(juce::String(initialSequenceNumber), juce::dontSendNotification);
        }

        showSequenceListPickerWindow(
            hasSelectedTrack ? "Change Track Seq" : "Change Active Seq",
            hasSelectedTrack
                ? ("Pick an existing sequence for track #" + juce::String(selectedTrackNumber) + ". OK commits this track to the selected sequence. Create Blank Seq adds an empty sequence to this picker; press OK to move the track there.")
                : juce::String("Pick an existing sequence from the scrollable list. OK applies the selected row. Create Blank Seq adds an empty sequence to this picker; press OK to switch to it."),
            initialSequenceNumber,
            [this, hasSelectedTrack, selectedTrackNumber](int targetSequenceNumber)
            {
                if (hasSelectedTrack)
                {
                    if (applyTrackSequenceChangeFromManager(targetSequenceNumber, false, selectedTrackNumber))
                        markTrackManagerEditorDirty("Change Track Seq");
                    return;
                }

                applyActiveSequenceNumberFromMain(targetSequenceNumber, false);
            },
            [this]
            {
                return createBlankSequenceOnly("Change Track Seq Create Blank Seq", false);
            }
        );
    }

    bool MainComponent::applyActiveSequenceNumberFromMain(int targetSequenceNumber, bool createIfNeeded)
    {
        if (!currentProject)
            return false;

        rebuildSectionsFromTracksIfNeeded();
        const int currentSequenceCount = static_cast<int>(importSections.size());

        if (targetSequenceNumber <= 0)
        {
            logMessage("Change Active Seq: target sequence cannot be blank or zero.");
            return false;
        }

        if (targetSequenceNumber > currentSequenceCount)
        {
            if (!createIfNeeded)
            {
                logMessage("Change Active Seq: Seq #" + juce::String(targetSequenceNumber) + " does not exist. Use Create Blank Seq to make a new empty sequence.");
                return false;
            }

            if (static_cast<int>(importSections.size()) >= mw::core::Project::maxSequenceCount)
            {
                logMessage("Sequence limit reached: " + juce::String(mw::core::Project::maxSequenceCount) + ". Change Active Seq could not create a new sequence.");
                return false;
            }

            auto created = createBlankSequenceOnly("Change Active Seq Create Blank Seq", true);
            return created.has_value();
        }

        activeImportSectionIndex = targetSequenceNumber - 1;
        const auto& sequence = importSections[static_cast<std::size_t>(activeImportSectionIndex)];
        trackManagerSectionBox.setText(juce::String(targetSequenceNumber), juce::dontSendNotification);
        trackManagerSectionStartBeatBox.setText(
            juce::String(static_cast<double>(sequence.startTick) / mw::core::Project::ticksPerQuarterNote, 2),
            juce::dontSendNotification
        );

        updateRenderTargetLabel();
        refreshTrackManagerText(true);
        refreshMainSequenceSelector();
        refreshActiveSequenceThoughtsEditor();

        if (trackManagerContent != nullptr)
        {
            if (auto* content = dynamic_cast<TrackManagerWindowContent*>(trackManagerContent.get()))
                content->refreshSequenceMap();
            else
                trackManagerContent->repaint();
        }

        trackManagerBox.scrollToFirstSequenceNumber(targetSequenceNumber);
        logMessage("Changed active sequence from main UI: Seq #" + juce::String(targetSequenceNumber) + " - " + sequence.name);
        return true;
    }

    void MainComponent::updateRenderTargetLabel()
    {
        juce::String trackText = "Track --";
        juce::String sequenceText = "Seq --";

        if (currentProject)
        {
            const auto trackIndex = getSelectedTrackIndex();

            if (trackIndex >= 0 && trackIndex < static_cast<int>(currentProject->getTracks().size()))
            {
                const auto& track = currentProject->getTracks()[static_cast<std::size_t>(trackIndex)];
                trackText = "Track #" + juce::String(trackIndex + 1) + " - " + track.getName();
            }

            if (activeImportSectionIndex >= 0 && activeImportSectionIndex < static_cast<int>(importSections.size()))
            {
                const auto& sequence = importSections[static_cast<std::size_t>(activeImportSectionIndex)];
                sequenceText = "Seq #" + juce::String(activeImportSectionIndex + 1) + " - " + sequence.name;
            }
            else
            {
                updateActiveSequenceFromSelectedTrack();

                if (activeImportSectionIndex >= 0 && activeImportSectionIndex < static_cast<int>(importSections.size()))
                {
                    const auto& sequence = importSections[static_cast<std::size_t>(activeImportSectionIndex)];
                    sequenceText = "Seq #" + juce::String(activeImportSectionIndex + 1) + " - " + sequence.name;
                }
            }
        }

        if (activeImportSectionIndex >= 0 && activeImportSectionIndex < static_cast<int>(importSections.size()))
            refreshMainSequenceSelector();

        renderTargetLabel.setText("Render Target: " + trackText + " | " + sequenceText, juce::dontSendNotification);
    }

void MainComponent::updateTrackSummary(const mw::core::Project& project)
    {
        juce::String summary;

        summary << "Project: " << project.getName() << "\n";
        summary << "Tempo: " << project.getTempoBpm() << " BPM\n";
        summary << "Time Signature: "
                << project.getTimeSignature().numerator
                << "/"
                << project.getTimeSignature().denominator
                << "\n\n";

        rebuildSectionsFromTracksIfNeeded();

        const int selectedTrackIndex = getSelectedTrackIndex();
        int selectedSummaryOffset = summary.length();

        if (selectedTrackIndex >= 0 && selectedTrackIndex < static_cast<int>(project.getTracks().size()))
        {
            const auto& track = project.getTracks()[static_cast<std::size_t>(selectedTrackIndex)];
            const int selectedSeq = getSequenceIndexForTrack(selectedTrackIndex + 1) + 1;

            summary << "ACTIVE TRACK\n";
            summary << "------------\n";
            summary << "Track #" << (selectedTrackIndex + 1) << ": " << track.getName() << "\n";
            summary << "Track Type: " << mw::core::trackTypeToString(track.getTrackType()).c_str() << "\n";
            if (selectedSeq > 0)
                summary << "Seq #  : " << selectedSeq << "\n";
            summary << "Instrument: " << track.getInstrument().displayName << "\n";
            summary << "Sound Library: " << getTrackLibrarySummaryLabel(track.getInstrument()) << "\n";
            summary << "Muted: " << (track.getMuted() ? "yes" : "no") << "    Solo: " << (track.getSolo() ? "yes" : "no") << "    Volume: " << juce::String(track.getMixerSettings().volume, 2) << "\n";
            summary << "Notes: " << static_cast<int>(track.getNotes().size()) << "\n";
            if (track.isAudioClipTrack())
                summary << getAudioClipSummaryForTrack(selectedTrackIndex);
            summary << "\n";
        }

        if (activeImportSectionIndex < 0 || activeImportSectionIndex >= static_cast<int>(importSections.size()))
            updateActiveSequenceFromSelectedTrack();

        if (activeImportSectionIndex >= 0 && activeImportSectionIndex < static_cast<int>(importSections.size()))
        {
            const auto& sequence = importSections[static_cast<std::size_t>(activeImportSectionIndex)];
            summary << "ACTIVE SEQUENCE\n";
            summary << "---------------\n";
            summary << "Seq # : " << (activeImportSectionIndex + 1) << "\n";
            summary << "Name  : " << sequence.name << "\n";
            summary << "State : " << (sequence.locked ? "LOCKED" : "UNLOCKED") << "\n";
            summary << "Tracks: " << formatSequenceTrackSummary(activeImportSectionIndex) << "\n";
            if (!sequence.notes.trim().isEmpty())
                summary << "Thoughts: " << sequence.notes.trim() << "\n";
            summary << getAudioClipSummaryForSequence(activeImportSectionIndex + 1);
            summary << "\n";
        }

        summary << "SEQUENCE INFO\n";
        summary << "-------------\n";
        for (int sequenceIndex = 0; sequenceIndex < static_cast<int>(importSections.size()); ++sequenceIndex)
        {
            const auto& sequence = importSections[static_cast<std::size_t>(sequenceIndex)];
            summary << "Seq #" << (sequenceIndex + 1) << ": "
                    << (sequence.name.isEmpty() ? juce::String("Unnamed Sequence") : sequence.name)
                    << " | Tracks: " << formatSequenceTrackSummary(sequenceIndex)
                    << (sequence.locked ? " | LOCKED" : "")
                    << "\n";
        }
        summary << "\n";

        summary << "TRACK INFO\n";
        summary << "----------\n";

        for (int trackIndex = 0; trackIndex < static_cast<int>(project.getTracks().size()); ++trackIndex)
        {
            const auto& track = project.getTracks()[static_cast<std::size_t>(trackIndex)];
            summary << (trackIndex == selectedTrackIndex ? "> Selected Track: " : "Track: ") << track.getName() << "\n";
            summary << "Track Type: " << mw::core::trackTypeToString(track.getTrackType()).c_str() << "\n";
            summary << "Instrument: " << track.getInstrument().displayName << "\n";
            summary << "Sound Library: " << getTrackLibrarySummaryLabel(track.getInstrument()) << "\n";
            summary << "Normalized: " << track.getInstrument().normalizedName << "\n";
            summary << "GM Program: " << track.getInstrument().midiProgram << "\n";
            summary << "MIDI Channel: " << track.getInstrument().midiChannel << "\n";
            summary << "Muted: " << (track.getMuted() ? "yes" : "no") << "\n";
            summary << "Solo: " << (track.getSolo() ? "yes" : "no") << "\n";
            summary << "Volume: " << juce::String(track.getMixerSettings().volume, 2) << "\n";
            summary << "Notes: " << static_cast<int>(track.getNotes().size()) << "\n";
            if (track.isAudioClipTrack())
                summary << getAudioClipSummaryForTrack(trackIndex);
            summary << "\n";
        }

        trackSummaryBox.setText(summary);
        trackSummaryBox.setCaretPosition(std::max(0, selectedSummaryOffset));
    }
}
