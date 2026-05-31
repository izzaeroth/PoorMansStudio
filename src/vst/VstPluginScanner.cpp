#include "vst/VstPluginScanner.h"

#include "app/AppPaths.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <set>
#include <sstream>
#include <string>
#include <system_error>

namespace
{
    std::string lowerCopy(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return value;
    }

    bool hasVst3Extension(const std::filesystem::path& path)
    {
        return lowerCopy(path.extension().string()) == ".vst3";
    }

    bool pathHasVst3Parent(const std::filesystem::path& path)
    {
        auto parent = path.parent_path();
        while (!parent.empty())
        {
            if (hasVst3Extension(parent))
                return true;
            const auto next = parent.parent_path();
            if (next == parent)
                break;
            parent = next;
        }
        return false;
    }

    std::string readSmallTextFile(const std::filesystem::path& path, std::size_t maxBytes = 512 * 1024)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file)
            return {};

        std::string data;
        data.resize(maxBytes);
        file.read(data.data(), static_cast<std::streamsize>(data.size()));
        data.resize(static_cast<std::size_t>(file.gcount()));
        return data;
    }

    std::string getJsonStringValue(const std::string& text, const std::string& key)
    {
        const auto pattern = "\"" + key + "\"";
        auto pos = text.find(pattern);
        if (pos == std::string::npos)
            return {};

        pos = text.find(':', pos + pattern.size());
        if (pos == std::string::npos)
            return {};

        pos = text.find('"', pos);
        if (pos == std::string::npos)
            return {};

        std::string value;
        bool escaping = false;
        for (std::size_t i = pos + 1; i < text.size(); ++i)
        {
            const char c = text[i];
            if (escaping)
            {
                value += c;
                escaping = false;
                continue;
            }
            if (c == '\\')
            {
                escaping = true;
                continue;
            }
            if (c == '"')
                break;
            value += c;
        }
        return value;
    }

    bool binaryContains(const std::string& data, const std::string& needle)
    {
        return lowerCopy(data).find(lowerCopy(needle)) != std::string::npos;
    }

    struct InferredKind
    {
        mw::vst::VstPluginKind kind = mw::vst::VstPluginKind::Unknown;
        std::string reason;
    };

    bool containsAny(const std::string& lowerText, std::initializer_list<const char*> needles)
    {
        for (const auto* needle : needles)
            if (lowerText.find(needle) != std::string::npos)
                return true;
        return false;
    }

    bool nameLooksExactlyLike(const std::string& lowerName, std::initializer_list<const char*> names)
    {
        for (const auto* name : names)
            if (lowerName == name)
                return true;
        return false;
    }

    InferredKind inferKind(const std::string& name,
                           const std::string& category,
                           const std::string& classInfo,
                           bool hasJuceDescription,
                           bool juceReportedInstrument,
                           int reportedAudioInputs,
                           int reportedAudioOutputs)
    {
        const auto lowerName = lowerCopy(name);
        const auto lowerCategory = lowerCopy(category);
        const auto lowerClass = lowerCopy(classInfo);
        const auto metadata = lowerCategory + " " + lowerClass;
        const auto visibleText = lowerName + " " + metadata;

        const bool strongInstrumentName = containsAny(lowerName, { "synth", "synthesizer", "sampler", "sample player", "piano", "organ", "drum", "rompler" });
        if (strongInstrumentName)
            return { mw::vst::VstPluginKind::Instrument, "Plugin name contains a strong instrument keyword such as Synth, Sampler, Piano, Organ, Drum, or Rompler." };

        if (containsAny(lowerName, { "channel-context", "channel context", "host-checker", "host checker", "program-change", "program change", "pitch-names", "pitch names" }))
            return { mw::vst::VstPluginKind::MidiTool, "Plugin name matches a known VST3 SDK utility/MIDI-context sample rather than a note-producing instrument." };

        if (nameLooksExactlyLike(lowerName, { "adelay", "again", "again-simple", "again-sample-accurate", "panner", "pan", "channel context", "channel-context" })
            || containsAny(lowerName, { "panner", " delay", "delay", "reverb", "compressor", "limiter", "equalizer", " eq", "filter", "chorus", "flanger", "phaser", "distortion", "saturator", "spatial" }))
        {
            return { mw::vst::VstPluginKind::Effect, "Plugin name matches an effect/processor keyword such as panner, delay, reverb, compressor, EQ, filter, modulation, or the Steinberg AGain/aDelay samples." };
        }

        if (containsAny(metadata, { "midi fx", "midi effect", "midi tool", "note fx", "note expression", "program change", "pitch names", "channel context", "controller" }))
            return { mw::vst::VstPluginKind::MidiTool, "Reported metadata suggests a MIDI utility/generator/context plugin rather than an audio-producing instrument." };

        if (containsAny(metadata, { "effect", "fx", "delay", "reverb", "panner", "pan", "eq", "equalizer", "compressor", "limiter", "gate", "expander", "filter", "chorus", "flanger", "phaser", "distortion", "saturator", "spatial", "utility" }))
            return { mw::vst::VstPluginKind::Effect, "Reported metadata contains an effect keyword such as Effect, Fx, Panner, EQ, Compressor, Reverb, Delay, Filter, Spatial, or Utility." };

        if (containsAny(visibleText, { "instrument", "generator" }))
            return { mw::vst::VstPluginKind::Instrument, "Plugin metadata contains Instrument/Generator and no stronger effect or MIDI-tool evidence was found." };

        if (hasJuceDescription && juceReportedInstrument)
            return { mw::vst::VstPluginKind::Instrument, "JUCE/VST3 plugin description reports isInstrument = true." };

        if (hasJuceDescription && reportedAudioInputs > 0 && reportedAudioOutputs > 0)
            return { mw::vst::VstPluginKind::Effect, "JUCE/VST3 bus layout reports audio input and audio output, which usually indicates an effect/processor rather than a MIDI note-producing instrument." };

        if (hasJuceDescription && reportedAudioOutputs <= 0)
            return { mw::vst::VstPluginKind::MidiTool, "JUCE/VST3 bus layout does not report audio output, so this is treated as a MIDI/utility plugin until manually overridden." };

        if (hasJuceDescription && reportedAudioInputs == 0 && reportedAudioOutputs > 0)
            return { mw::vst::VstPluginKind::Unknown, "Plugin has audio output but did not report itself as an instrument. It is hidden until the user confirms it is a note-producing instrument." };

        return { mw::vst::VstPluginKind::Unknown, "Plugin did not clearly report an instrument, effect, or MIDI-tool role. It is treated as unsupported until the user overrides it in Plugin Manager." };
    }

    void inspectWithJucePluginDescription(mw::vst::VstPluginDescriptor& descriptor)
    {
        try
        {
            juce::AudioPluginFormatManager formatManager;
            formatManager.addFormat(new juce::VST3PluginFormat());

            juce::OwnedArray<juce::PluginDescription> descriptions;
            for (int i = 0; i < formatManager.getNumFormats(); ++i)
            {
                auto* format = formatManager.getFormat(i);
                if (format == nullptr)
                    continue;

                if (format->getName().containsIgnoreCase("VST3"))
                    format->findAllTypesForFile(descriptions, juce::String(descriptor.bundlePath.string()));
            }

            if (descriptions.isEmpty())
                return;

            const juce::PluginDescription* selected = descriptions[0];
            for (auto* description : descriptions)
            {
                if (description != nullptr && description->isInstrument)
                {
                    selected = description;
                    break;
                }
            }

            if (selected == nullptr)
                return;

            descriptor.juceDescriptionAvailable = true;
            descriptor.juceReportedInstrument = selected->isInstrument;
            descriptor.reportedAudioInputs = selected->numInputChannels;
            descriptor.reportedAudioOutputs = selected->numOutputChannels;
            descriptor.reportedIdentifier = selected->createIdentifierString().toStdString();
            descriptor.reportedFormat = selected->pluginFormatName.toStdString();
            descriptor.reportedDescriptiveName = selected->descriptiveName.toStdString();

            if (!selected->name.isEmpty())
                descriptor.name = selected->name.toStdString();
            if (!selected->manufacturerName.isEmpty())
                descriptor.vendor = selected->manufacturerName.toStdString();
            if (!selected->version.isEmpty())
                descriptor.version = selected->version.toStdString();
            if (!selected->category.isEmpty())
                descriptor.category = selected->category.toStdString();
            if (descriptor.uid.empty())
                descriptor.uid = descriptor.reportedIdentifier;
        }
        catch (...)
        {
            // Keep the lightweight moduleinfo/binary inspection result if JUCE metadata probing fails.
        }
    }

    std::filesystem::path findBinaryInsideBundle(const std::filesystem::path& bundle)
    {
        const auto expected = bundle / "Contents" / "x86_64-win" / bundle.filename();
        if (std::filesystem::exists(expected))
            return expected;

        std::error_code ec;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(bundle, ec))
        {
            if (ec)
                break;

            if (!entry.is_regular_file(ec))
                continue;

            if (hasVst3Extension(entry.path()))
                return entry.path();
        }

        return {};
    }

    mw::vst::VstCompatibilityFlags detectCompatibilityFlags(const std::filesystem::path& bundle, const std::filesystem::path& binary)
    {
        mw::vst::VstCompatibilityFlags flags;

        std::string data;
        if (!binary.empty())
            data = readSmallTextFile(binary, 8 * 1024 * 1024);

        auto markFromText = [&](const std::string& text)
        {
            flags.usesOpenGL = flags.usesOpenGL || binaryContains(text, "opengl32.dll") || binaryContains(text, "wgl");
            flags.usesDirect3D = flags.usesDirect3D || binaryContains(text, "d3d9.dll") || binaryContains(text, "d3d10.dll") || binaryContains(text, "d3d11.dll") || binaryContains(text, "d3d12.dll") || binaryContains(text, "dxgi.dll");
            flags.usesDirect2D = flags.usesDirect2D || binaryContains(text, "d2d1.dll") || binaryContains(text, "dwrite.dll");
            flags.usesVulkan = flags.usesVulkan || binaryContains(text, "vulkan-1.dll");
            flags.usesOpenCL = flags.usesOpenCL || binaryContains(text, "opencl.dll");
            flags.usesWebView = flags.usesWebView || binaryContains(text, "webview2loader.dll");
            flags.usesCef = flags.usesCef || binaryContains(text, "libcef.dll") || binaryContains(text, "chromium");
            flags.usesNvidiaSpecific = flags.usesNvidiaSpecific || binaryContains(text, "nvapi64.dll") || binaryContains(text, "nvcuda.dll");
            flags.usesAmdSpecific = flags.usesAmdSpecific || binaryContains(text, "amd_ags") || binaryContains(text, "amd_ags_x64.dll");
            flags.hasNativeWindowsUi = flags.hasNativeWindowsUi || binaryContains(text, "user32.dll") || binaryContains(text, "gdi32.dll") || binaryContains(text, "dwmapi.dll");
        };

        markFromText(data);

        std::error_code ec;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(bundle, ec))
        {
            if (ec)
                break;

            const auto name = lowerCopy(entry.path().filename().string());
            markFromText(name);
        }

        return flags;
    }

    std::vector<std::filesystem::path> uniqueExistingFolders(std::vector<std::filesystem::path> folders)
    {
        std::vector<std::filesystem::path> result;
        std::set<std::string> seen;
        std::error_code ec;

        for (auto folder : folders)
        {
            if (folder.empty() || !std::filesystem::exists(folder, ec))
                continue;

            auto canonical = std::filesystem::weakly_canonical(folder, ec);
            if (ec)
                canonical = folder.lexically_normal();

            const auto key = lowerCopy(canonical.string());
            if (seen.insert(key).second)
                result.push_back(canonical);
        }

        return result;
    }
}

namespace mw::vst
{
    std::vector<std::filesystem::path> VstPluginScanner::defaultScanFolders(bool includeWorkspaceFolder, bool includeSystemFolders)
    {
        std::vector<std::filesystem::path> folders;

        if (includeWorkspaceFolder)
            folders.push_back(mw::app::AppPaths::vst3Folder());

#if defined(_WIN32)
        if (includeSystemFolders)
        {
            folders.emplace_back("C:\\Program Files\\Common Files\\VST3");
            folders.emplace_back("C:\\Program Files (x86)\\Common Files\\VST3");
        }
#endif

        return uniqueExistingFolders(std::move(folders));
    }

    bool VstPluginScanner::isOuterVst3Bundle(const std::filesystem::path& path)
    {
        return hasVst3Extension(path) && !pathHasVst3Parent(path);
    }

    VstPluginDescriptor VstPluginScanner::inspectBundle(const std::filesystem::path& outerBundlePath)
    {
        VstPluginDescriptor descriptor;
        descriptor.bundlePath = outerBundlePath;
        descriptor.name = outerBundlePath.stem().string();
        descriptor.status = VstPluginScanStatus::Ok;

        if (!std::filesystem::exists(outerBundlePath))
        {
            descriptor.status = VstPluginScanStatus::Missing;
            descriptor.statusMessage = "VST3 bundle was not found.";
            return descriptor;
        }

        const auto moduleInfo = outerBundlePath / "Contents" / "Resources" / "moduleinfo.json";
        const auto moduleText = readSmallTextFile(moduleInfo);

        if (!moduleText.empty())
        {
            const auto jsonName = getJsonStringValue(moduleText, "Name").empty() ? getJsonStringValue(moduleText, "name") : getJsonStringValue(moduleText, "Name");
            const auto jsonVendor = getJsonStringValue(moduleText, "Vendor").empty() ? getJsonStringValue(moduleText, "vendor") : getJsonStringValue(moduleText, "Vendor");
            const auto jsonVersion = getJsonStringValue(moduleText, "Version").empty() ? getJsonStringValue(moduleText, "version") : getJsonStringValue(moduleText, "Version");
            const auto jsonCategory = getJsonStringValue(moduleText, "Category").empty() ? getJsonStringValue(moduleText, "category") : getJsonStringValue(moduleText, "Category");
            const auto jsonSubCategories = getJsonStringValue(moduleText, "Sub Categories").empty() ? getJsonStringValue(moduleText, "subCategories") : getJsonStringValue(moduleText, "Sub Categories");
            const auto jsonClass = getJsonStringValue(moduleText, "Class").empty() ? getJsonStringValue(moduleText, "class") : getJsonStringValue(moduleText, "Class");
            const auto jsonUid = getJsonStringValue(moduleText, "CID").empty() ? getJsonStringValue(moduleText, "cid") : getJsonStringValue(moduleText, "CID");

            if (!jsonName.empty()) descriptor.name = jsonName;
            if (!jsonVendor.empty()) descriptor.vendor = jsonVendor;
            if (!jsonVersion.empty()) descriptor.version = jsonVersion;
            if (!jsonCategory.empty()) descriptor.category = jsonCategory;
            if (!jsonSubCategories.empty())
            {
                if (!descriptor.category.empty()) descriptor.category += "; ";
                descriptor.category += jsonSubCategories;
            }
            if (!jsonUid.empty()) descriptor.uid = jsonUid;
            descriptor.reportedCategory = descriptor.category;
            descriptor.reportedClassInfo = jsonClass;
        }

        descriptor.binaryPath = findBinaryInsideBundle(outerBundlePath);
        inspectWithJucePluginDescription(descriptor);
        descriptor.reportedCategory = descriptor.category.empty() ? descriptor.reportedCategory : descriptor.category;

        const auto inferred = inferKind(descriptor.name,
                                        descriptor.category,
                                        descriptor.reportedClassInfo + " " + descriptor.reportedDescriptiveName,
                                        descriptor.juceDescriptionAvailable,
                                        descriptor.juceReportedInstrument,
                                        descriptor.reportedAudioInputs,
                                        descriptor.reportedAudioOutputs);
        descriptor.detectedKind = inferred.kind;
        descriptor.kind = inferred.kind;
        descriptor.classificationReason = inferred.reason;
        descriptor.compatibility = detectCompatibilityFlags(outerBundlePath, descriptor.binaryPath);

        if (descriptor.compatibility.hasAnyGpuOrUiRisk())
        {
            descriptor.status = VstPluginScanStatus::Warning;
            descriptor.statusMessage = "Scanned with compatibility warning: " + descriptor.compatibility.summary();
        }
        else
        {
            descriptor.statusMessage = "Scanned successfully.";
        }

        return descriptor;
    }

    std::vector<VstPluginDescriptor> VstPluginScanner::scanFolders(const std::vector<std::filesystem::path>& folders)
    {
        std::vector<VstPluginDescriptor> plugins;
        std::set<std::string> seen;
        std::error_code ec;

        for (const auto& folder : folders)
        {
            if (folder.empty() || !std::filesystem::exists(folder, ec))
                continue;

            for (const auto& entry : std::filesystem::directory_iterator(folder, ec))
            {
                if (ec)
                    break;

                const auto path = entry.path();
                if (!isOuterVst3Bundle(path))
                    continue;

                auto canonical = std::filesystem::weakly_canonical(path, ec);
                if (ec)
                    canonical = path.lexically_normal();

                const auto key = lowerCopy(canonical.string());
                if (!seen.insert(key).second)
                    continue;

                plugins.push_back(inspectBundle(canonical));
            }
        }

        std::sort(plugins.begin(), plugins.end(), [](const auto& a, const auto& b)
        {
            return lowerCopy(a.displayName()) < lowerCopy(b.displayName());
        });

        return plugins;
    }

    std::vector<VstPluginDescriptor> VstPluginScanner::scan(const VstScanOptions& options)
    {
        auto folders = defaultScanFolders(options.includeWorkspaceFolder, options.includeSystemFolders);
        folders.insert(folders.end(), options.scanFolders.begin(), options.scanFolders.end());
        return scanFolders(uniqueExistingFolders(std::move(folders)));
    }
}
