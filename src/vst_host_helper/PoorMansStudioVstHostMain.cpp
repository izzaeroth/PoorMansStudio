#include "app/AppVersion.h"
#include "vst/VstHostProbe.h"
#include "vst/VstPluginScanner.h"
#include "vst/VstPluginTypes.h"

#include <juce_core/juce_core.h>

#include <charconv>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace
{
    constexpr const char* helperName = "PoorMansStudioVstHost";

    std::string jsonEscape(const std::string& value)
    {
        std::string out;
        out.reserve(value.size() + 16);
        for (const auto ch : value)
        {
            switch (ch)
            {
                case '\\': out += "\\\\"; break;
                case '"': out += "\\\""; break;
                case '\b': out += "\\b"; break;
                case '\f': out += "\\f"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default: out += static_cast<unsigned char>(ch) < 0x20 ? '?' : ch; break;
            }
        }
        return out;
    }

    const char* modeName(mw::vst::VstValidationMode mode)
    {
        switch (mode)
        {
            case mw::vst::VstValidationMode::Activation: return "activation";
            case mw::vst::VstValidationMode::Process: return "process";
            case mw::vst::VstValidationMode::State: return "state";
            default: return "instance";
        }
    }

    void printHelp()
    {
        std::cout
            << helperName << " " << mw::app::appVersionLabel << "\n"
            << "Out-of-process JUCE VST3 validation helper for Poor Man's Studio.\n\n"
            << "Commands:\n"
            << "  --help                              Show this help text.\n"
            << "  --version                           Print helper version.\n"
            << "  --ping                              Print pong and exit.\n"
            << "  --scan <plugin.vst3>                Inspect one outer VST3 bundle.\n"
            << "  --scan-json <plugin.vst3>           Inspect one outer VST3 bundle and print JSON.\n"
            << "  --validate-instance <plugin.vst3>   Create and destroy a JUCE VST3 instance.\n"
            << "  --validate-instance-json <plugin.vst3>\n"
            << "  --validate-activation <plugin.vst3> Prepare, inspect, release, and destroy it.\n"
            << "  --validate-activation-json <plugin.vst3>\n"
            << "  --validate-process <plugin.vst3>    Process one silent block and verify finite output.\n"
            << "  --validate-process-json <plugin.vst3>\n"
            << "  --validate-state <plugin.vst3>      Save, recreate, restore, process, and recapture state.\n"
            << "  --validate-state-json <plugin.vst3>\n\n"
            << "Options for validation commands:\n"
            << "  --plugin-index <n>                  Select a zero-based descriptor index (default 0).\n\n"
            << "The helper uses JUCE's VST3 hosting path, matching the main application. It does not\n"
            << "open plugin editors, render project audio, or own live playback.\n";
    }

    void printStringArray(const std::vector<std::string>& values)
    {
        std::cout << "[";
        for (std::size_t i = 0; i < values.size(); ++i)
        {
            if (i > 0) std::cout << ", ";
            std::cout << "\"" << jsonEscape(values[i]) << "\"";
        }
        std::cout << "]";
    }

    void printDescriptorText(const mw::vst::VstPluginDescriptor& plugin)
    {
        std::cout
            << "name=" << plugin.displayName() << "\n"
            << "vendor=" << plugin.vendor << "\n"
            << "version=" << plugin.version << "\n"
            << "category=" << plugin.category << "\n"
            << "kind=" << mw::vst::vstPluginKindToString(plugin.kind) << "\n"
            << "status=" << mw::vst::vstPluginScanStatusToString(plugin.status) << "\n"
            << "statusMessage=" << plugin.statusMessage << "\n"
            << "classificationReason=" << plugin.classificationReason << "\n"
            << "bundlePath=" << plugin.bundlePath.string() << "\n"
            << "binaryPath=" << plugin.binaryPath.string() << "\n"
            << "reportedIdentifier=" << plugin.reportedIdentifier << "\n"
            << "reportedFormat=" << plugin.reportedFormat << "\n"
            << "reportedAudioInputs=" << plugin.reportedAudioInputs << "\n"
            << "reportedAudioOutputs=" << plugin.reportedAudioOutputs << "\n"
            << "compatibility=" << plugin.compatibility.summary() << "\n";
    }

    void printDescriptorJson(const mw::vst::VstPluginDescriptor& plugin)
    {
        std::cout
            << "{\n"
            << "  \"helper\": \"" << helperName << "\",\n"
            << "  \"helperVersion\": \"" << jsonEscape(mw::app::appVersion) << "\",\n"
            << "  \"name\": \"" << jsonEscape(plugin.displayName()) << "\",\n"
            << "  \"vendor\": \"" << jsonEscape(plugin.vendor) << "\",\n"
            << "  \"version\": \"" << jsonEscape(plugin.version) << "\",\n"
            << "  \"category\": \"" << jsonEscape(plugin.category) << "\",\n"
            << "  \"kind\": \"" << jsonEscape(mw::vst::vstPluginKindToString(plugin.kind)) << "\",\n"
            << "  \"status\": \"" << jsonEscape(mw::vst::vstPluginScanStatusToString(plugin.status)) << "\",\n"
            << "  \"statusMessage\": \"" << jsonEscape(plugin.statusMessage) << "\",\n"
            << "  \"classificationReason\": \"" << jsonEscape(plugin.classificationReason) << "\",\n"
            << "  \"bundlePath\": \"" << jsonEscape(plugin.bundlePath.string()) << "\",\n"
            << "  \"binaryPath\": \"" << jsonEscape(plugin.binaryPath.string()) << "\",\n"
            << "  \"reportedIdentifier\": \"" << jsonEscape(plugin.reportedIdentifier) << "\",\n"
            << "  \"reportedFormat\": \"" << jsonEscape(plugin.reportedFormat) << "\",\n"
            << "  \"reportedAudioInputs\": " << plugin.reportedAudioInputs << ",\n"
            << "  \"reportedAudioOutputs\": " << plugin.reportedAudioOutputs << ",\n"
            << "  \"compatibility\": \"" << jsonEscape(plugin.compatibility.summary()) << "\"\n"
            << "}\n";
    }

    void printValidationText(const mw::vst::VstHostValidationResult& v)
    {
        std::cout
            << "ok=" << (v.ok() ? "true" : "false") << "\n"
            << "validation=" << modeName(v.mode) << "\n"
            << "stage=" << v.stage << "\n"
            << "message=" << v.message << "\n"
            << "name=" << v.descriptor.displayName() << "\n"
            << "vendor=" << v.descriptor.vendor << "\n"
            << "version=" << v.descriptor.version << "\n"
            << "bundlePath=" << v.descriptor.bundlePath.string() << "\n"
            << "pluginCount=" << v.pluginCount << "\n"
            << "selectedIndex=" << v.selectedIndex << "\n"
            << "descriptionsEnumerated=" << (v.descriptionsEnumerated ? "true" : "false") << "\n"
            << "descriptionSelected=" << (v.descriptionSelected ? "true" : "false") << "\n"
            << "instanceCreated=" << (v.instanceCreated ? "true" : "false") << "\n"
            << "instanceCreateCount=" << v.instanceCreateCount << "\n"
            << "prepared=" << (v.prepared ? "true" : "false") << "\n"
            << "prepareCount=" << v.prepareCount << "\n"
            << "processCalled=" << (v.processCalled ? "true" : "false") << "\n"
            << "processCompleted=" << (v.processCompleted ? "true" : "false") << "\n"
            << "finiteOutput=" << (v.finiteOutput ? "true" : "false") << "\n"
            << "nonFiniteSampleCount=" << v.nonFiniteSampleCount << "\n"
            << "maxOutputAbs=" << v.maxOutputAbs << "\n"
            << "stateCaptured=" << (v.stateCaptured ? "true" : "false") << "\n"
            << "stateRestored=" << (v.stateRestored ? "true" : "false") << "\n"
            << "stateRecaptured=" << (v.stateRecaptured ? "true" : "false") << "\n"
            << "stateByteEquivalent=" << (v.stateByteEquivalent ? "true" : "false") << "\n"
            << "firstStateBytes=" << v.firstStateBytes << "\n"
            << "secondStateBytes=" << v.secondStateBytes << "\n"
            << "firstStateHash=" << v.firstStateHash << "\n"
            << "secondStateHash=" << v.secondStateHash << "\n"
            << "resourcesReleased=" << (v.resourcesReleased ? "true" : "false") << "\n"
            << "resourceReleaseCount=" << v.resourceReleaseCount << "\n"
            << "instanceDestroyed=" << (v.instanceDestroyed ? "true" : "false") << "\n"
            << "instanceDestroyCount=" << v.instanceDestroyCount << "\n"
            << "requestedInputChannels=" << v.requestedInputChannels << "\n"
            << "requestedOutputChannels=" << v.requestedOutputChannels << "\n"
            << "layoutConfigured=" << (v.layoutConfigured ? "true" : "false") << "\n"
            << "requestedLayoutMatched=" << (v.requestedLayoutMatched ? "true" : "false") << "\n"
            << "inputBusCount=" << v.inputBusCount << "\n"
            << "outputBusCount=" << v.outputBusCount << "\n"
            << "inputChannels=" << v.inputChannels << "\n"
            << "outputChannels=" << v.outputChannels << "\n"
            << "acceptsMidi=" << (v.acceptsMidi ? "true" : "false") << "\n"
            << "producesMidi=" << (v.producesMidi ? "true" : "false") << "\n"
            << "midiEffect=" << (v.midiEffect ? "true" : "false") << "\n"
            << "editorAvailable=" << (v.editorAvailable ? "true" : "false") << "\n"
            << "parameterCount=" << v.parameterCount << "\n"
            << "latencySamples=" << v.latencySamples << "\n"
            << "tailSeconds=" << v.tailSeconds << "\n";
    }

    void printValidationJson(const mw::vst::VstHostValidationResult& v)
    {
        std::cout
            << "{\n"
            << "  \"helper\": \"" << helperName << "\",\n"
            << "  \"helperVersion\": \"" << jsonEscape(mw::app::appVersion) << "\",\n"
            << "  \"validation\": \"" << modeName(v.mode) << "\",\n"
            << "  \"ok\": " << (v.ok() ? "true" : "false") << ",\n"
            << "  \"stage\": \"" << jsonEscape(v.stage) << "\",\n"
            << "  \"message\": \"" << jsonEscape(v.message) << "\",\n"
            << "  \"path\": \"" << jsonEscape(v.descriptor.bundlePath.string()) << "\",\n"
            << "  \"plugin_count\": " << v.pluginCount << ",\n"
            << "  \"selected_index\": " << v.selectedIndex << ",\n"
            << "  \"descriptor\": {\n"
            << "    \"name\": \"" << jsonEscape(v.descriptor.displayName()) << "\",\n"
            << "    \"vendor\": \"" << jsonEscape(v.descriptor.vendor) << "\",\n"
            << "    \"version\": \"" << jsonEscape(v.descriptor.version) << "\",\n"
            << "    \"identifier\": \"" << jsonEscape(v.descriptor.reportedIdentifier) << "\",\n"
            << "    \"instrument\": " << (v.descriptor.juceReportedInstrument ? "true" : "false") << "\n"
            << "  },\n"
            << "  \"lifecycle\": {\n"
            << "    \"descriptions_enumerated\": " << (v.descriptionsEnumerated ? "true" : "false") << ",\n"
            << "    \"description_selected\": " << (v.descriptionSelected ? "true" : "false") << ",\n"
            << "    \"instance_created\": " << (v.instanceCreated ? "true" : "false") << ",\n"
            << "    \"instance_create_count\": " << v.instanceCreateCount << ",\n"
            << "    \"prepared\": " << (v.prepared ? "true" : "false") << ",\n"
            << "    \"prepare_count\": " << v.prepareCount << ",\n"
            << "    \"processing_enabled\": " << (v.processingEnabled ? "true" : "false") << ",\n"
            << "    \"resources_released\": " << (v.resourcesReleased ? "true" : "false") << ",\n"
            << "    \"resource_release_count\": " << v.resourceReleaseCount << ",\n"
            << "    \"instance_destroyed\": " << (v.instanceDestroyed ? "true" : "false") << ",\n"
            << "    \"instance_destroy_count\": " << v.instanceDestroyCount << "\n"
            << "  },\n"
            << "  \"capabilities\": {\n"
            << "    \"audio_input_buses\": ";
        printStringArray(v.inputBuses);
        std::cout << ",\n    \"audio_output_buses\": ";
        printStringArray(v.outputBuses);
        std::cout
            << ",\n    \"requested_input_channels\": " << v.requestedInputChannels << ",\n"
            << "    \"requested_output_channels\": " << v.requestedOutputChannels << ",\n"
            << "    \"layout_configured\": " << (v.layoutConfigured ? "true" : "false") << ",\n"
            << "    \"requested_layout_matched\": " << (v.requestedLayoutMatched ? "true" : "false") << ",\n"
            << "    \"input_channels\": " << v.inputChannels << ",\n"
            << "    \"output_channels\": " << v.outputChannels << ",\n"
            << "    \"accepts_midi\": " << (v.acceptsMidi ? "true" : "false") << ",\n"
            << "    \"produces_midi\": " << (v.producesMidi ? "true" : "false") << ",\n"
            << "    \"midi_effect\": " << (v.midiEffect ? "true" : "false") << ",\n"
            << "    \"editor_available\": " << (v.editorAvailable ? "true" : "false") << ",\n"
            << "    \"parameter_count\": " << v.parameterCount << ",\n"
            << "    \"latency_samples\": " << v.latencySamples << ",\n"
            << "    \"tail_seconds\": " << v.tailSeconds << "\n"
            << "  },\n"
            << "  \"process\": {\n"
            << "    \"frames\": " << v.processFrames << ",\n"
            << "    \"called\": " << (v.processCalled ? "true" : "false") << ",\n"
            << "    \"completed\": " << (v.processCompleted ? "true" : "false") << ",\n"
            << "    \"finite_output\": " << (v.finiteOutput ? "true" : "false") << ",\n"
            << "    \"non_finite_sample_count\": " << v.nonFiniteSampleCount << ",\n"
            << "    \"max_output_abs\": " << v.maxOutputAbs << "\n"
            << "  },\n"
            << "  \"state\": {\n"
            << "    \"captured\": " << (v.stateCaptured ? "true" : "false") << ",\n"
            << "    \"restored\": " << (v.stateRestored ? "true" : "false") << ",\n"
            << "    \"recaptured\": " << (v.stateRecaptured ? "true" : "false") << ",\n"
            << "    \"byte_equivalent\": " << (v.stateByteEquivalent ? "true" : "false") << ",\n"
            << "    \"first_bytes\": " << v.firstStateBytes << ",\n"
            << "    \"second_bytes\": " << v.secondStateBytes << ",\n"
            << "    \"first_hash\": \"" << jsonEscape(v.firstStateHash) << "\",\n"
            << "    \"second_hash\": \"" << jsonEscape(v.secondStateHash) << "\"\n"
            << "  },\n"
            << "  \"error\": \"" << jsonEscape(v.ok() ? std::string() : v.message) << "\"\n"
            << "}\n";
    }

    bool parsePluginIndex(const std::vector<std::string>& args, int& pluginIndex)
    {
        pluginIndex = 0;
        for (std::size_t i = 2; i < args.size(); ++i)
        {
            if (args[i] != "--plugin-index" || i + 1 >= args.size())
                return false;

            const auto& value = args[++i];
            int parsed = 0;
            const auto conversion = std::from_chars(value.data(), value.data() + value.size(), parsed);
            if (conversion.ec != std::errc() || conversion.ptr != value.data() + value.size() || parsed < 0)
                return false;
            pluginIndex = parsed;
        }
        return true;
    }

    int scanOne(const std::string& rawPath, bool json)
    {
        const auto path = std::filesystem::path(rawPath);
        auto descriptor = mw::vst::VstPluginScanner::inspectBundle(path);
        if (json) printDescriptorJson(descriptor); else printDescriptorText(descriptor);
        if (descriptor.status == mw::vst::VstPluginScanStatus::Ok || descriptor.status == mw::vst::VstPluginScanStatus::Warning) return 0;
        return descriptor.status == mw::vst::VstPluginScanStatus::Missing ? 3 : 5;
    }

    int validateOne(const std::string& rawPath, mw::vst::VstValidationMode mode, bool json, int pluginIndex)
    {
        auto result = mw::vst::VstHostProbe::validate(std::filesystem::path(rawPath), mode, pluginIndex);
        if (json) printValidationJson(result); else printValidationText(result);
        if (result.ok()) return 0;
        return result.descriptor.status == mw::vst::VstPluginScanStatus::Missing ? 3 : 5;
    }
}

int main(int argc, char* argv[])
{
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i)
        args.emplace_back(argv[i] == nullptr ? "" : argv[i]);

    if (args.empty() || args[0] == "--help" || args[0] == "-h" || args[0] == "/?")
    {
        printHelp();
        return 0;
    }
    if (args[0] == "--version")
    {
        std::cout << helperName << " " << mw::app::appVersionLabel << "\n";
        return 0;
    }
    if (args[0] == "--ping")
    {
        std::cout << "pong\n";
        return 0;
    }

    if (args[0] == "--scan" || args[0] == "--scan-json")
    {
        if (args.size() != 2)
        {
            std::cerr << args[0] << " requires one path to an outer .vst3 bundle.\n";
            return 2;
        }
        return scanOne(args[1], args[0] == "--scan-json");
    }

    mw::vst::VstValidationMode mode;
    bool json = false;
    if (args[0] == "--validate-instance" || args[0] == "--validate-instance-json")
        mode = mw::vst::VstValidationMode::Instance;
    else if (args[0] == "--validate-activation" || args[0] == "--validate-activation-json")
        mode = mw::vst::VstValidationMode::Activation;
    else if (args[0] == "--validate-process" || args[0] == "--validate-process-json")
        mode = mw::vst::VstValidationMode::Process;
    else if (args[0] == "--validate-state" || args[0] == "--validate-state-json")
        mode = mw::vst::VstValidationMode::State;
    else
    {
        std::cerr << "Unknown command: " << args[0] << "\n\n";
        printHelp();
        return 2;
    }

    json = args[0].ends_with("-json");
    if (args.size() < 2)
    {
        std::cerr << args[0] << " requires a path to an outer .vst3 bundle.\n";
        return 2;
    }

    int pluginIndex = 0;
    if (!parsePluginIndex(args, pluginIndex))
    {
        std::cerr << "Invalid validation options. Use --plugin-index followed by a non-negative integer.\n";
        return 2;
    }

    return validateOne(args[1], mode, json, pluginIndex);
}
