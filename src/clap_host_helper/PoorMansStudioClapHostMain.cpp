#include "app/AppVersion.h"
#include "clap/ClapAbiProbe.h"
#include "clap/ClapPluginScanner.h"
#include "clap/ClapPluginTypes.h"

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace
{
    constexpr const char* helperName = "PoorMansStudioClapHost";

    std::string jsonEscape(const std::string& value)
    {
        std::string out;
        out.reserve(value.size() + 16);

        for (const auto ch : value)
        {
            switch (ch)
            {
                case '\\': out += "\\\\"; break;
                case '"':  out += "\\\""; break;
                case '\b': out += "\\b"; break;
                case '\f': out += "\\f"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (static_cast<unsigned char>(ch) < 0x20)
                        out += '?';
                    else
                        out += ch;
                    break;
            }
        }

        return out;
    }

    void printHelp()
    {
        std::cout
            << helperName << " " << mw::app::appVersionLabel << "\n"
            << "Out-of-process CLAP helper foundation for Poor Man's Studio.\n\n"
            << "Commands:\n"
            << "  --help                 Show this help text.\n"
            << "  --version              Print helper version.\n"
            << "  --ping                 Print pong and exit.\n"
            << "  --scan <plugin.clap>   Inspect one outer CLAP candidate in the helper process.\n"
            << "  --scan-json <plugin.clap>\n"
            << "                         Inspect one outer CLAP candidate and print JSON.\n\n"
            << "  --validate-instance <plugin.clap>\n"
            << "                         Create, init, destroy, and unload one CLAP instance.\n"
            << "  --validate-instance-json <plugin.clap>\n"
            << "                         Validate one CLAP instance and print JSON.\n"
            << "  --validate-activation <plugin.clap>\n"
            << "                         Create, init, activate, capability-probe, deactivate, destroy, and unload one CLAP instance.\n"
            << "  --validate-activation-json <plugin.clap>\n"
            << "                         Validate CLAP activation/capabilities and print JSON.\n"
            << "  --validate-process <plugin.clap>\n"
            << "                         Start CLAP processing, run one silent buffer block, stop, deactivate, destroy, and unload.\n"
            << "  --validate-process-json <plugin.clap>\n"
            << "                         Validate one silent CLAP process block and print JSON.\n\n"
            << "Notes:\n"
            << "  Scan commands remain scanner/metadata groundwork only. Each scan\n"
            << "  command loads the CLAP binary in the helper process, reads descriptor\n"
            << "  metadata through the CLAP ABI, and unloads it. Validate-instance\n"
            << "  commands go one step further by creating, initializing, destroying,\n"
            << "  and unloading\n"
            << "  a plugin instance with a minimal no-op host. Validate-activation\n"
            << "  commands also call activate/deactivate and safely query audio/note\n"
            << "  port extension availability. Validate-process commands go one step\n"
            << "  further by calling start_processing(), running one silent process()\n"
            << "  buffer, then stopping and cleaning up. This helper still does not assign\n"
            << "  Effect Slots, open plugin UIs, save CLAP state, or connect CLAP audio\n"
            << "  processing to the main app render/preview paths yet.\n";
    }

    void printDescriptorText(const mw::clap::ClapPluginDescriptor& plugin)
    {
        std::cout
            << "name=" << plugin.displayName() << "\n"
            << "vendor=" << plugin.vendor << "\n"
            << "version=" << plugin.version << "\n"
            << "category=" << plugin.category << "\n"
            << "uid=" << plugin.uid << "\n"
            << "kind=" << mw::clap::clapPluginKindToString(plugin.detectedKind) << "\n"
            << "status=" << mw::clap::clapPluginScanStatusToString(plugin.status) << "\n"
            << "statusMessage=" << plugin.statusMessage << "\n"
            << "classificationReason=" << plugin.classificationReason << "\n"
            << "pluginPath=" << plugin.pluginPath.string() << "\n"
            << "binaryPath=" << plugin.binaryPath.string() << "\n"
            << "description=" << plugin.description << "\n"
            << "pluginCount=" << plugin.clapPluginCount << "\n"
            << "clapVersion=" << plugin.clapVersionMajor << "." << plugin.clapVersionMinor << "." << plugin.clapVersionRevision << "\n"
            << "abiProbed=" << (plugin.abiProbed ? "true" : "false") << "\n"
            << "metadataOnly=" << (plugin.metadataOnly ? "true" : "false") << "\n"
            << "features=";

        for (std::size_t i = 0; i < plugin.features.size(); ++i)
        {
            if (i > 0)
                std::cout << ",";
            std::cout << plugin.features[i];
        }

        std::cout << "\n";
    }

    void printDescriptorJson(const mw::clap::ClapPluginDescriptor& plugin)
    {
        std::cout
            << "{\n"
            << "  \"helper\": \"" << helperName << "\",\n"
            << "  \"helperVersion\": \"" << jsonEscape(mw::app::appVersion) << "\",\n"
            << "  \"name\": \"" << jsonEscape(plugin.displayName()) << "\",\n"
            << "  \"vendor\": \"" << jsonEscape(plugin.vendor) << "\",\n"
            << "  \"version\": \"" << jsonEscape(plugin.version) << "\",\n"
            << "  \"category\": \"" << jsonEscape(plugin.category) << "\",\n"
            << "  \"uid\": \"" << jsonEscape(plugin.uid) << "\",\n"
            << "  \"kind\": \"" << jsonEscape(mw::clap::clapPluginKindToString(plugin.detectedKind)) << "\",\n"
            << "  \"status\": \"" << jsonEscape(mw::clap::clapPluginScanStatusToString(plugin.status)) << "\",\n"
            << "  \"statusMessage\": \"" << jsonEscape(plugin.statusMessage) << "\",\n"
            << "  \"classificationReason\": \"" << jsonEscape(plugin.classificationReason) << "\",\n"
            << "  \"pluginPath\": \"" << jsonEscape(plugin.pluginPath.string()) << "\",\n"
            << "  \"binaryPath\": \"" << jsonEscape(plugin.binaryPath.string()) << "\",\n"
            << "  \"description\": \"" << jsonEscape(plugin.description) << "\",\n"
            << "  \"pluginCount\": " << plugin.clapPluginCount << ",\n"
            << "  \"clapVersionMajor\": " << plugin.clapVersionMajor << ",\n"
            << "  \"clapVersionMinor\": " << plugin.clapVersionMinor << ",\n"
            << "  \"clapVersionRevision\": " << plugin.clapVersionRevision << ",\n"
            << "  \"abiProbed\": " << (plugin.abiProbed ? "true" : "false") << ",\n"
            << "  \"metadataOnly\": " << (plugin.metadataOnly ? "true" : "false") << ",\n"
            << "  \"features\": [";

        for (std::size_t i = 0; i < plugin.features.size(); ++i)
        {
            if (i > 0)
                std::cout << ", ";
            std::cout << "\"" << jsonEscape(plugin.features[i]) << "\"";
        }

        std::cout << "]\n"
            << "}\n";
    }

    void printValidationText(const mw::clap::ClapInstanceValidationResult& validation)
    {
        const auto& plugin = validation.descriptor;

        std::cout
            << "ok=" << (validation.ok() ? "true" : "false") << "\n"
            << "stage=" << validation.stage << "\n"
            << "message=" << validation.message << "\n"
            << "name=" << plugin.displayName() << "\n"
            << "vendor=" << plugin.vendor << "\n"
            << "version=" << plugin.version << "\n"
            << "uid=" << plugin.uid << "\n"
            << "kind=" << mw::clap::clapPluginKindToString(plugin.detectedKind) << "\n"
            << "pluginPath=" << plugin.pluginPath.string() << "\n"
            << "binaryPath=" << plugin.binaryPath.string() << "\n"
            << "pluginCount=" << validation.pluginCount << "\n"
            << "selectedIndex=" << validation.selectedIndex << "\n"
            << "loadedLibrary=" << (validation.loadedLibrary ? "true" : "false") << "\n"
            << "foundEntry=" << (validation.foundEntry ? "true" : "false") << "\n"
            << "entryInitialized=" << (validation.entryInitialized ? "true" : "false") << "\n"
            << "foundFactory=" << (validation.foundFactory ? "true" : "false") << "\n"
            << "foundDescriptor=" << (validation.foundDescriptor ? "true" : "false") << "\n"
            << "instanceCreated=" << (validation.instanceCreated ? "true" : "false") << "\n"
            << "pluginInitialized=" << (validation.pluginInitialized ? "true" : "false") << "\n"
            << "instanceDestroyed=" << (validation.instanceDestroyed ? "true" : "false") << "\n"
            << "entryDeinitialized=" << (validation.entryDeinitialized ? "true" : "false") << "\n"
            << "restartRequested=" << (validation.restartRequested ? "true" : "false") << "\n"
            << "processRequested=" << (validation.processRequested ? "true" : "false") << "\n"
            << "callbackRequested=" << (validation.callbackRequested ? "true" : "false") << "\n"
            << "features=";

        for (std::size_t i = 0; i < plugin.features.size(); ++i)
        {
            if (i > 0)
                std::cout << ",";
            std::cout << plugin.features[i];
        }

        std::cout << "\n";
    }

    void printValidationJson(const mw::clap::ClapInstanceValidationResult& validation)
    {
        const auto& plugin = validation.descriptor;

        std::cout
            << "{\n"
            << "  \"helper\": \"" << helperName << "\",\n"
            << "  \"helperVersion\": \"" << jsonEscape(mw::app::appVersion) << "\",\n"
            << "  \"ok\": " << (validation.ok() ? "true" : "false") << ",\n"
            << "  \"path\": \"" << jsonEscape(plugin.pluginPath.string()) << "\",\n"
            << "  \"binaryPath\": \"" << jsonEscape(plugin.binaryPath.string()) << "\",\n"
            << "  \"stage\": \"" << jsonEscape(validation.stage) << "\",\n"
            << "  \"message\": \"" << jsonEscape(validation.message) << "\",\n"
            << "  \"plugin_count\": " << validation.pluginCount << ",\n"
            << "  \"selected_index\": " << validation.selectedIndex << ",\n"
            << "  \"descriptor\": {\n"
            << "    \"id\": \"" << jsonEscape(plugin.uid) << "\",\n"
            << "    \"name\": \"" << jsonEscape(plugin.displayName()) << "\",\n"
            << "    \"vendor\": \"" << jsonEscape(plugin.vendor) << "\",\n"
            << "    \"version\": \"" << jsonEscape(plugin.version) << "\",\n"
            << "    \"kind\": \"" << jsonEscape(mw::clap::clapPluginKindToString(plugin.detectedKind)) << "\",\n"
            << "    \"status\": \"" << jsonEscape(mw::clap::clapPluginScanStatusToString(plugin.status)) << "\",\n"
            << "    \"statusMessage\": \"" << jsonEscape(plugin.statusMessage) << "\",\n"
            << "    \"features\": [";

        for (std::size_t i = 0; i < plugin.features.size(); ++i)
        {
            if (i > 0)
                std::cout << ", ";
            std::cout << "\"" << jsonEscape(plugin.features[i]) << "\"";
        }

        std::cout
            << "]\n"
            << "  },\n"
            << "  \"instance\": {\n"
            << "    \"loadedLibrary\": " << (validation.loadedLibrary ? "true" : "false") << ",\n"
            << "    \"foundEntry\": " << (validation.foundEntry ? "true" : "false") << ",\n"
            << "    \"entryInitialized\": " << (validation.entryInitialized ? "true" : "false") << ",\n"
            << "    \"foundFactory\": " << (validation.foundFactory ? "true" : "false") << ",\n"
            << "    \"foundDescriptor\": " << (validation.foundDescriptor ? "true" : "false") << ",\n"
            << "    \"created\": " << (validation.instanceCreated ? "true" : "false") << ",\n"
            << "    \"initialized\": " << (validation.pluginInitialized ? "true" : "false") << ",\n"
            << "    \"destroyed\": " << (validation.instanceDestroyed ? "true" : "false") << ",\n"
            << "    \"entryDeinitialized\": " << (validation.entryDeinitialized ? "true" : "false") << "\n"
            << "  },\n"
            << "  \"host_requests\": {\n"
            << "    \"restart_requested\": " << (validation.restartRequested ? "true" : "false") << ",\n"
            << "    \"process_requested\": " << (validation.processRequested ? "true" : "false") << ",\n"
            << "    \"callback_requested\": " << (validation.callbackRequested ? "true" : "false") << "\n"
            << "  },\n"
            << "  \"error\": \"" << jsonEscape(validation.ok() ? std::string() : validation.message) << "\"\n"
            << "}\n";
    }


    void printJsonStringArray(const std::vector<std::string>& values)
    {
        std::cout << "[";
        for (std::size_t i = 0; i < values.size(); ++i)
        {
            if (i > 0)
                std::cout << ", ";
            std::cout << "\"" << jsonEscape(values[i]) << "\"";
        }
        std::cout << "]";
    }

    void printActivationText(const mw::clap::ClapActivationValidationResult& validation)
    {
        const auto& plugin = validation.descriptor;

        std::cout
            << "ok=" << (validation.ok() ? "true" : "false") << "\n"
            << "stage=" << validation.stage << "\n"
            << "message=" << validation.message << "\n"
            << "name=" << plugin.displayName() << "\n"
            << "vendor=" << plugin.vendor << "\n"
            << "version=" << plugin.version << "\n"
            << "uid=" << plugin.uid << "\n"
            << "kind=" << mw::clap::clapPluginKindToString(plugin.detectedKind) << "\n"
            << "pluginPath=" << plugin.pluginPath.string() << "\n"
            << "binaryPath=" << plugin.binaryPath.string() << "\n"
            << "pluginCount=" << validation.pluginCount << "\n"
            << "selectedIndex=" << validation.selectedIndex << "\n"
            << "sampleRate=" << validation.sampleRate << "\n"
            << "minFrames=" << validation.minFrames << "\n"
            << "maxFrames=" << validation.maxFrames << "\n"
            << "loadedLibrary=" << (validation.loadedLibrary ? "true" : "false") << "\n"
            << "foundEntry=" << (validation.foundEntry ? "true" : "false") << "\n"
            << "entryInitialized=" << (validation.entryInitialized ? "true" : "false") << "\n"
            << "foundFactory=" << (validation.foundFactory ? "true" : "false") << "\n"
            << "foundDescriptor=" << (validation.foundDescriptor ? "true" : "false") << "\n"
            << "instanceCreated=" << (validation.instanceCreated ? "true" : "false") << "\n"
            << "pluginInitialized=" << (validation.pluginInitialized ? "true" : "false") << "\n"
            << "pluginActivated=" << (validation.pluginActivated ? "true" : "false") << "\n"
            << "pluginDeactivated=" << (validation.pluginDeactivated ? "true" : "false") << "\n"
            << "instanceDestroyed=" << (validation.instanceDestroyed ? "true" : "false") << "\n"
            << "entryDeinitialized=" << (validation.entryDeinitialized ? "true" : "false") << "\n"
            << "audioPortsExtensionAvailable=" << (validation.audioPortsExtensionAvailable ? "true" : "false") << "\n"
            << "audioInputPortCount=" << validation.audioInputPortCount << "\n"
            << "audioOutputPortCount=" << validation.audioOutputPortCount << "\n"
            << "audioPortsMessage=" << validation.audioPortsMessage << "\n"
            << "notePortsExtensionAvailable=" << (validation.notePortsExtensionAvailable ? "true" : "false") << "\n"
            << "noteInputPortCount=" << validation.noteInputPortCount << "\n"
            << "noteOutputPortCount=" << validation.noteOutputPortCount << "\n"
            << "notePortsMessage=" << validation.notePortsMessage << "\n"
            << "restartRequested=" << (validation.restartRequested ? "true" : "false") << "\n"
            << "processRequested=" << (validation.processRequested ? "true" : "false") << "\n"
            << "callbackRequested=" << (validation.callbackRequested ? "true" : "false") << "\n"
            << "audioInputPorts=";

        for (std::size_t i = 0; i < validation.audioInputPorts.size(); ++i)
        {
            if (i > 0)
                std::cout << ";";
            std::cout << validation.audioInputPorts[i];
        }

        std::cout << "\naudioOutputPorts=";
        for (std::size_t i = 0; i < validation.audioOutputPorts.size(); ++i)
        {
            if (i > 0)
                std::cout << ";";
            std::cout << validation.audioOutputPorts[i];
        }

        std::cout << "\nnoteInputPorts=";
        for (std::size_t i = 0; i < validation.noteInputPorts.size(); ++i)
        {
            if (i > 0)
                std::cout << ";";
            std::cout << validation.noteInputPorts[i];
        }

        std::cout << "\nnoteOutputPorts=";
        for (std::size_t i = 0; i < validation.noteOutputPorts.size(); ++i)
        {
            if (i > 0)
                std::cout << ";";
            std::cout << validation.noteOutputPorts[i];
        }

        std::cout << "\nfeatures=";
        for (std::size_t i = 0; i < plugin.features.size(); ++i)
        {
            if (i > 0)
                std::cout << ",";
            std::cout << plugin.features[i];
        }

        std::cout << "\n";
    }

    void printActivationJson(const mw::clap::ClapActivationValidationResult& validation)
    {
        const auto& plugin = validation.descriptor;

        std::cout
            << "{\n"
            << "  \"helper\": \"" << helperName << "\",\n"
            << "  \"helperVersion\": \"" << jsonEscape(mw::app::appVersion) << "\",\n"
            << "  \"ok\": " << (validation.ok() ? "true" : "false") << ",\n"
            << "  \"path\": \"" << jsonEscape(plugin.pluginPath.string()) << "\",\n"
            << "  \"binaryPath\": \"" << jsonEscape(plugin.binaryPath.string()) << "\",\n"
            << "  \"stage\": \"" << jsonEscape(validation.stage) << "\",\n"
            << "  \"message\": \"" << jsonEscape(validation.message) << "\",\n"
            << "  \"plugin_count\": " << validation.pluginCount << ",\n"
            << "  \"selected_index\": " << validation.selectedIndex << ",\n"
            << "  \"activation_settings\": {\n"
            << "    \"sample_rate\": " << validation.sampleRate << ",\n"
            << "    \"min_frames\": " << validation.minFrames << ",\n"
            << "    \"max_frames\": " << validation.maxFrames << "\n"
            << "  },\n"
            << "  \"descriptor\": {\n"
            << "    \"id\": \"" << jsonEscape(plugin.uid) << "\",\n"
            << "    \"name\": \"" << jsonEscape(plugin.displayName()) << "\",\n"
            << "    \"vendor\": \"" << jsonEscape(plugin.vendor) << "\",\n"
            << "    \"version\": \"" << jsonEscape(plugin.version) << "\",\n"
            << "    \"kind\": \"" << jsonEscape(mw::clap::clapPluginKindToString(plugin.detectedKind)) << "\",\n"
            << "    \"status\": \"" << jsonEscape(mw::clap::clapPluginScanStatusToString(plugin.status)) << "\",\n"
            << "    \"statusMessage\": \"" << jsonEscape(plugin.statusMessage) << "\",\n"
            << "    \"features\": [";

        for (std::size_t i = 0; i < plugin.features.size(); ++i)
        {
            if (i > 0)
                std::cout << ", ";
            std::cout << "\"" << jsonEscape(plugin.features[i]) << "\"";
        }

        std::cout
            << "]\n"
            << "  },\n"
            << "  \"instance\": {\n"
            << "    \"loadedLibrary\": " << (validation.loadedLibrary ? "true" : "false") << ",\n"
            << "    \"foundEntry\": " << (validation.foundEntry ? "true" : "false") << ",\n"
            << "    \"entryInitialized\": " << (validation.entryInitialized ? "true" : "false") << ",\n"
            << "    \"foundFactory\": " << (validation.foundFactory ? "true" : "false") << ",\n"
            << "    \"foundDescriptor\": " << (validation.foundDescriptor ? "true" : "false") << ",\n"
            << "    \"created\": " << (validation.instanceCreated ? "true" : "false") << ",\n"
            << "    \"initialized\": " << (validation.pluginInitialized ? "true" : "false") << ",\n"
            << "    \"activated\": " << (validation.pluginActivated ? "true" : "false") << ",\n"
            << "    \"deactivated\": " << (validation.pluginDeactivated ? "true" : "false") << ",\n"
            << "    \"destroyed\": " << (validation.instanceDestroyed ? "true" : "false") << ",\n"
            << "    \"entryDeinitialized\": " << (validation.entryDeinitialized ? "true" : "false") << "\n"
            << "  },\n"
            << "  \"capabilities\": {\n"
            << "    \"audio_ports\": {\n"
            << "      \"extension_available\": " << (validation.audioPortsExtensionAvailable ? "true" : "false") << ",\n"
            << "      \"input_count\": " << validation.audioInputPortCount << ",\n"
            << "      \"output_count\": " << validation.audioOutputPortCount << ",\n"
            << "      \"message\": \"" << jsonEscape(validation.audioPortsMessage) << "\",\n"
            << "      \"inputs\": ";
        printJsonStringArray(validation.audioInputPorts);
        std::cout << ",\n      \"outputs\": ";
        printJsonStringArray(validation.audioOutputPorts);
        std::cout
            << "\n    },\n"
            << "    \"note_ports\": {\n"
            << "      \"extension_available\": " << (validation.notePortsExtensionAvailable ? "true" : "false") << ",\n"
            << "      \"input_count\": " << validation.noteInputPortCount << ",\n"
            << "      \"output_count\": " << validation.noteOutputPortCount << ",\n"
            << "      \"message\": \"" << jsonEscape(validation.notePortsMessage) << "\",\n"
            << "      \"inputs\": ";
        printJsonStringArray(validation.noteInputPorts);
        std::cout << ",\n      \"outputs\": ";
        printJsonStringArray(validation.noteOutputPorts);
        std::cout
            << "\n    }\n"
            << "  },\n"
            << "  \"host_requests\": {\n"
            << "    \"restart_requested\": " << (validation.restartRequested ? "true" : "false") << ",\n"
            << "    \"process_requested\": " << (validation.processRequested ? "true" : "false") << ",\n"
            << "    \"callback_requested\": " << (validation.callbackRequested ? "true" : "false") << "\n"
            << "  },\n"
            << "  \"error\": \"" << jsonEscape(validation.ok() ? std::string() : validation.message) << "\"\n"
            << "}\n";
    }


    void printProcessText(const mw::clap::ClapProcessValidationResult& validation)
    {
        const auto& plugin = validation.descriptor;

        std::cout
            << "ok=" << (validation.ok() ? "true" : "false") << "\n"
            << "stage=" << validation.stage << "\n"
            << "message=" << validation.message << "\n"
            << "name=" << plugin.displayName() << "\n"
            << "vendor=" << plugin.vendor << "\n"
            << "version=" << plugin.version << "\n"
            << "uid=" << plugin.uid << "\n"
            << "kind=" << mw::clap::clapPluginKindToString(plugin.detectedKind) << "\n"
            << "pluginPath=" << plugin.pluginPath.string() << "\n"
            << "binaryPath=" << plugin.binaryPath.string() << "\n"
            << "pluginCount=" << validation.pluginCount << "\n"
            << "selectedIndex=" << validation.selectedIndex << "\n"
            << "sampleRate=" << validation.sampleRate << "\n"
            << "minFrames=" << validation.minFrames << "\n"
            << "maxFrames=" << validation.maxFrames << "\n"
            << "processFrames=" << validation.processFrames << "\n"
            << "loadedLibrary=" << (validation.loadedLibrary ? "true" : "false") << "\n"
            << "foundEntry=" << (validation.foundEntry ? "true" : "false") << "\n"
            << "entryInitialized=" << (validation.entryInitialized ? "true" : "false") << "\n"
            << "foundFactory=" << (validation.foundFactory ? "true" : "false") << "\n"
            << "foundDescriptor=" << (validation.foundDescriptor ? "true" : "false") << "\n"
            << "instanceCreated=" << (validation.instanceCreated ? "true" : "false") << "\n"
            << "pluginInitialized=" << (validation.pluginInitialized ? "true" : "false") << "\n"
            << "pluginActivated=" << (validation.pluginActivated ? "true" : "false") << "\n"
            << "pluginStartedProcessing=" << (validation.pluginStartedProcessing ? "true" : "false") << "\n"
            << "pluginProcessCalled=" << (validation.pluginProcessCalled ? "true" : "false") << "\n"
            << "pluginProcessReturnedOk=" << (validation.pluginProcessReturnedOk ? "true" : "false") << "\n"
            << "pluginStoppedProcessing=" << (validation.pluginStoppedProcessing ? "true" : "false") << "\n"
            << "pluginDeactivated=" << (validation.pluginDeactivated ? "true" : "false") << "\n"
            << "instanceDestroyed=" << (validation.instanceDestroyed ? "true" : "false") << "\n"
            << "entryDeinitialized=" << (validation.entryDeinitialized ? "true" : "false") << "\n"
            << "audioPortsExtensionAvailable=" << (validation.audioPortsExtensionAvailable ? "true" : "false") << "\n"
            << "audioInputPortCount=" << validation.audioInputPortCount << "\n"
            << "audioOutputPortCount=" << validation.audioOutputPortCount << "\n"
            << "audioPortsMessage=" << validation.audioPortsMessage << "\n"
            << "notePortsExtensionAvailable=" << (validation.notePortsExtensionAvailable ? "true" : "false") << "\n"
            << "noteInputPortCount=" << validation.noteInputPortCount << "\n"
            << "noteOutputPortCount=" << validation.noteOutputPortCount << "\n"
            << "notePortsMessage=" << validation.notePortsMessage << "\n"
            << "processInputBufferCount=" << validation.processInputBufferCount << "\n"
            << "processOutputBufferCount=" << validation.processOutputBufferCount << "\n"
            << "processInputChannelCount=" << validation.processInputChannelCount << "\n"
            << "processOutputChannelCount=" << validation.processOutputChannelCount << "\n"
            << "processStatus=" << validation.processStatus << "\n"
            << "processStatusText=" << validation.processStatusText << "\n"
            << "processMessage=" << validation.processMessage << "\n"
            << "outputEventCount=" << validation.outputEventCount << "\n"
            << "maxOutputAbs=" << validation.maxOutputAbs << "\n"
            << "restartRequested=" << (validation.restartRequested ? "true" : "false") << "\n"
            << "processRequested=" << (validation.processRequested ? "true" : "false") << "\n"
            << "callbackRequested=" << (validation.callbackRequested ? "true" : "false") << "\n"
            << "audioInputPorts=";

        for (std::size_t i = 0; i < validation.audioInputPorts.size(); ++i)
        {
            if (i > 0)
                std::cout << ";";
            std::cout << validation.audioInputPorts[i];
        }

        std::cout << "\naudioOutputPorts=";
        for (std::size_t i = 0; i < validation.audioOutputPorts.size(); ++i)
        {
            if (i > 0)
                std::cout << ";";
            std::cout << validation.audioOutputPorts[i];
        }

        std::cout << "\nnoteInputPorts=";
        for (std::size_t i = 0; i < validation.noteInputPorts.size(); ++i)
        {
            if (i > 0)
                std::cout << ";";
            std::cout << validation.noteInputPorts[i];
        }

        std::cout << "\nnoteOutputPorts=";
        for (std::size_t i = 0; i < validation.noteOutputPorts.size(); ++i)
        {
            if (i > 0)
                std::cout << ";";
            std::cout << validation.noteOutputPorts[i];
        }

        std::cout << "\nfeatures=";
        for (std::size_t i = 0; i < plugin.features.size(); ++i)
        {
            if (i > 0)
                std::cout << ",";
            std::cout << plugin.features[i];
        }

        std::cout << "\n";
    }

    void printProcessJson(const mw::clap::ClapProcessValidationResult& validation)
    {
        const auto& plugin = validation.descriptor;

        std::cout
            << "{\n"
            << "  \"helper\": \"" << helperName << "\",\n"
            << "  \"helperVersion\": \"" << jsonEscape(mw::app::appVersion) << "\",\n"
            << "  \"ok\": " << (validation.ok() ? "true" : "false") << ",\n"
            << "  \"path\": \"" << jsonEscape(plugin.pluginPath.string()) << "\",\n"
            << "  \"binaryPath\": \"" << jsonEscape(plugin.binaryPath.string()) << "\",\n"
            << "  \"stage\": \"" << jsonEscape(validation.stage) << "\",\n"
            << "  \"message\": \"" << jsonEscape(validation.message) << "\",\n"
            << "  \"plugin_count\": " << validation.pluginCount << ",\n"
            << "  \"selected_index\": " << validation.selectedIndex << ",\n"
            << "  \"activation_settings\": {\n"
            << "    \"sample_rate\": " << validation.sampleRate << ",\n"
            << "    \"min_frames\": " << validation.minFrames << ",\n"
            << "    \"max_frames\": " << validation.maxFrames << "\n"
            << "  },\n"
            << "  \"descriptor\": {\n"
            << "    \"id\": \"" << jsonEscape(plugin.uid) << "\",\n"
            << "    \"name\": \"" << jsonEscape(plugin.displayName()) << "\",\n"
            << "    \"vendor\": \"" << jsonEscape(plugin.vendor) << "\",\n"
            << "    \"version\": \"" << jsonEscape(plugin.version) << "\",\n"
            << "    \"kind\": \"" << jsonEscape(mw::clap::clapPluginKindToString(plugin.detectedKind)) << "\",\n"
            << "    \"status\": \"" << jsonEscape(mw::clap::clapPluginScanStatusToString(plugin.status)) << "\",\n"
            << "    \"statusMessage\": \"" << jsonEscape(plugin.statusMessage) << "\",\n"
            << "    \"features\": [";

        for (std::size_t i = 0; i < plugin.features.size(); ++i)
        {
            if (i > 0)
                std::cout << ", ";
            std::cout << "\"" << jsonEscape(plugin.features[i]) << "\"";
        }

        std::cout
            << "]\n"
            << "  },\n"
            << "  \"instance\": {\n"
            << "    \"loadedLibrary\": " << (validation.loadedLibrary ? "true" : "false") << ",\n"
            << "    \"foundEntry\": " << (validation.foundEntry ? "true" : "false") << ",\n"
            << "    \"entryInitialized\": " << (validation.entryInitialized ? "true" : "false") << ",\n"
            << "    \"foundFactory\": " << (validation.foundFactory ? "true" : "false") << ",\n"
            << "    \"foundDescriptor\": " << (validation.foundDescriptor ? "true" : "false") << ",\n"
            << "    \"created\": " << (validation.instanceCreated ? "true" : "false") << ",\n"
            << "    \"initialized\": " << (validation.pluginInitialized ? "true" : "false") << ",\n"
            << "    \"activated\": " << (validation.pluginActivated ? "true" : "false") << ",\n"
            << "    \"startedProcessing\": " << (validation.pluginStartedProcessing ? "true" : "false") << ",\n"
            << "    \"processCalled\": " << (validation.pluginProcessCalled ? "true" : "false") << ",\n"
            << "    \"processReturnedOk\": " << (validation.pluginProcessReturnedOk ? "true" : "false") << ",\n"
            << "    \"stoppedProcessing\": " << (validation.pluginStoppedProcessing ? "true" : "false") << ",\n"
            << "    \"deactivated\": " << (validation.pluginDeactivated ? "true" : "false") << ",\n"
            << "    \"destroyed\": " << (validation.instanceDestroyed ? "true" : "false") << ",\n"
            << "    \"entryDeinitialized\": " << (validation.entryDeinitialized ? "true" : "false") << "\n"
            << "  },\n"
            << "  \"capabilities\": {\n"
            << "    \"audio_ports\": {\n"
            << "      \"extension_available\": " << (validation.audioPortsExtensionAvailable ? "true" : "false") << ",\n"
            << "      \"input_count\": " << validation.audioInputPortCount << ",\n"
            << "      \"output_count\": " << validation.audioOutputPortCount << ",\n"
            << "      \"message\": \"" << jsonEscape(validation.audioPortsMessage) << "\",\n"
            << "      \"inputs\": ";
        printJsonStringArray(validation.audioInputPorts);
        std::cout << ",\n      \"outputs\": ";
        printJsonStringArray(validation.audioOutputPorts);
        std::cout
            << "\n    },\n"
            << "    \"note_ports\": {\n"
            << "      \"extension_available\": " << (validation.notePortsExtensionAvailable ? "true" : "false") << ",\n"
            << "      \"input_count\": " << validation.noteInputPortCount << ",\n"
            << "      \"output_count\": " << validation.noteOutputPortCount << ",\n"
            << "      \"message\": \"" << jsonEscape(validation.notePortsMessage) << "\",\n"
            << "      \"inputs\": ";
        printJsonStringArray(validation.noteInputPorts);
        std::cout << ",\n      \"outputs\": ";
        printJsonStringArray(validation.noteOutputPorts);
        std::cout
            << "\n    }\n"
            << "  },\n"
            << "  \"process\": {\n"
            << "    \"frames\": " << validation.processFrames << ",\n"
            << "    \"input_buffer_count\": " << validation.processInputBufferCount << ",\n"
            << "    \"output_buffer_count\": " << validation.processOutputBufferCount << ",\n"
            << "    \"input_channel_count\": " << validation.processInputChannelCount << ",\n"
            << "    \"output_channel_count\": " << validation.processOutputChannelCount << ",\n"
            << "    \"status\": " << validation.processStatus << ",\n"
            << "    \"status_text\": \"" << jsonEscape(validation.processStatusText) << "\",\n"
            << "    \"message\": \"" << jsonEscape(validation.processMessage) << "\",\n"
            << "    \"output_event_count\": " << validation.outputEventCount << ",\n"
            << "    \"max_output_abs\": " << validation.maxOutputAbs << "\n"
            << "  },\n"
            << "  \"host_requests\": {\n"
            << "    \"restart_requested\": " << (validation.restartRequested ? "true" : "false") << ",\n"
            << "    \"process_requested\": " << (validation.processRequested ? "true" : "false") << ",\n"
            << "    \"callback_requested\": " << (validation.callbackRequested ? "true" : "false") << "\n"
            << "  },\n"
            << "  \"error\": \"" << jsonEscape(validation.ok() ? std::string() : validation.message) << "\"\n"
            << "}\n";
    }

    int scanOne(const std::string& rawPath, bool json)
    {
        try
        {
            const auto pluginPath = std::filesystem::path(rawPath);

            if (!std::filesystem::exists(pluginPath))
            {
                if (json)
                {
                    std::cout
                        << "{\n"
                        << "  \"helper\": \"" << helperName << "\",\n"
                        << "  \"helperVersion\": \"" << jsonEscape(mw::app::appVersion) << "\",\n"
                        << "  \"status\": \"Missing\",\n"
                        << "  \"statusMessage\": \"CLAP plugin path was not found.\",\n"
                        << "  \"pluginPath\": \"" << jsonEscape(pluginPath.string()) << "\"\n"
                        << "}\n";
                }
                else
                {
                    std::cerr << "CLAP plugin path was not found: " << pluginPath.string() << "\n";
                }

                return 3;
            }

            if (!mw::clap::ClapPluginScanner::isOuterClapPlugin(pluginPath))
            {
                if (json)
                {
                    std::cout
                        << "{\n"
                        << "  \"helper\": \"" << helperName << "\",\n"
                        << "  \"helperVersion\": \"" << jsonEscape(mw::app::appVersion) << "\",\n"
                        << "  \"status\": \"Unsupported\",\n"
                        << "  \"statusMessage\": \"Path is not an outer .clap plugin file or bundle. Point to the outer path ending in .clap, not an inner binary.\",\n"
                        << "  \"pluginPath\": \"" << jsonEscape(pluginPath.string()) << "\"\n"
                        << "}\n";
                }
                else
                {
                    std::cerr << "Path is not an outer .clap plugin file or bundle: " << pluginPath.string() << "\n";
                    std::cerr << "Point to the outer path ending in .clap, not an inner binary.\n";
                }

                return 4;
            }

            auto probe = mw::clap::ClapAbiProbe::probePluginPath(pluginPath);
            auto descriptor = probe.descriptor;

            if (json)
                printDescriptorJson(descriptor);
            else
                printDescriptorText(descriptor);

            switch (descriptor.status)
            {
                case mw::clap::ClapPluginScanStatus::Candidate:
                    return 0;
                case mw::clap::ClapPluginScanStatus::Missing:
                    return 3;
                default:
                    return 5;
            }
        }
        catch (const std::exception& e)
        {
            if (json)
            {
                std::cout
                    << "{\n"
                    << "  \"helper\": \"" << helperName << "\",\n"
                    << "  \"helperVersion\": \"" << jsonEscape(mw::app::appVersion) << "\",\n"
                    << "  \"status\": \"Unsupported\",\n"
                    << "  \"statusMessage\": \"" << jsonEscape(e.what()) << "\"\n"
                    << "}\n";
            }
            else
            {
                std::cerr << "Scan failed: " << e.what() << "\n";
            }

            return 5;
        }
        catch (...)
        {
            if (json)
            {
                std::cout
                    << "{\n"
                    << "  \"helper\": \"" << helperName << "\",\n"
                    << "  \"helperVersion\": \"" << jsonEscape(mw::app::appVersion) << "\",\n"
                    << "  \"status\": \"Unsupported\",\n"
                    << "  \"statusMessage\": \"Unknown helper exception.\"\n"
                    << "}\n";
            }
            else
            {
                std::cerr << "Scan failed: unknown helper exception.\n";
            }

            return 5;
        }
    }

    int validateOne(const std::string& rawPath, bool json)
    {
        try
        {
            const auto pluginPath = std::filesystem::path(rawPath);
            auto validation = mw::clap::ClapAbiProbe::validatePluginInstance(pluginPath);

            if (json)
                printValidationJson(validation);
            else
                printValidationText(validation);

            if (validation.ok())
                return 0;

            return validation.descriptor.status == mw::clap::ClapPluginScanStatus::Missing ? 3 : 5;
        }
        catch (const std::exception& e)
        {
            if (json)
            {
                std::cout
                    << "{\n"
                    << "  \"helper\": \"" << helperName << "\",\n"
                    << "  \"helperVersion\": \"" << jsonEscape(mw::app::appVersion) << "\",\n"
                    << "  \"ok\": false,\n"
                    << "  \"stage\": \"exception\",\n"
                    << "  \"error\": \"" << jsonEscape(e.what()) << "\"\n"
                    << "}\n";
            }
            else
            {
                std::cerr << "Instance validation failed: " << e.what() << "\n";
            }

            return 5;
        }
        catch (...)
        {
            if (json)
            {
                std::cout
                    << "{\n"
                    << "  \"helper\": \"" << helperName << "\",\n"
                    << "  \"helperVersion\": \"" << jsonEscape(mw::app::appVersion) << "\",\n"
                    << "  \"ok\": false,\n"
                    << "  \"stage\": \"exception\",\n"
                    << "  \"error\": \"Unknown helper exception.\"\n"
                    << "}\n";
            }
            else
            {
                std::cerr << "Instance validation failed: unknown helper exception.\n";
            }

            return 5;
        }
    }


    int validateActivationOne(const std::string& rawPath, bool json)
    {
        try
        {
            const auto pluginPath = std::filesystem::path(rawPath);
            auto validation = mw::clap::ClapAbiProbe::validatePluginActivation(pluginPath);

            if (json)
                printActivationJson(validation);
            else
                printActivationText(validation);

            if (validation.ok())
                return 0;

            return validation.descriptor.status == mw::clap::ClapPluginScanStatus::Missing ? 3 : 5;
        }
        catch (const std::exception& e)
        {
            if (json)
            {
                std::cout
                    << "{\n"
                    << "  \"helper\": \"" << helperName << "\",\n"
                    << "  \"helperVersion\": \"" << jsonEscape(mw::app::appVersion) << "\",\n"
                    << "  \"ok\": false,\n"
                    << "  \"stage\": \"exception\",\n"
                    << "  \"error\": \"" << jsonEscape(e.what()) << "\"\n"
                    << "}\n";
            }
            else
            {
                std::cerr << "Activation validation failed: " << e.what() << "\n";
            }

            return 5;
        }
        catch (...)
        {
            if (json)
            {
                std::cout
                    << "{\n"
                    << "  \"helper\": \"" << helperName << "\",\n"
                    << "  \"helperVersion\": \"" << jsonEscape(mw::app::appVersion) << "\",\n"
                    << "  \"ok\": false,\n"
                    << "  \"stage\": \"exception\",\n"
                    << "  \"error\": \"Unknown helper exception.\"\n"
                    << "}\n";
            }
            else
            {
                std::cerr << "Activation validation failed: unknown helper exception.\n";
            }

            return 5;
        }
    }


    int validateProcessOne(const std::string& rawPath, bool json)
    {
        try
        {
            const auto pluginPath = std::filesystem::path(rawPath);
            auto validation = mw::clap::ClapAbiProbe::validatePluginSilentProcess(pluginPath);

            if (json)
                printProcessJson(validation);
            else
                printProcessText(validation);

            if (validation.ok())
                return 0;

            return validation.descriptor.status == mw::clap::ClapPluginScanStatus::Missing ? 3 : 5;
        }
        catch (const std::exception& e)
        {
            if (json)
            {
                std::cout
                    << "{\n"
                    << "  \"helper\": \"" << helperName << "\",\n"
                    << "  \"helperVersion\": \"" << jsonEscape(mw::app::appVersion) << "\",\n"
                    << "  \"ok\": false,\n"
                    << "  \"stage\": \"exception\",\n"
                    << "  \"error\": \"" << jsonEscape(e.what()) << "\"\n"
                    << "}\n";
            }
            else
            {
                std::cerr << "Silent process validation failed: " << e.what() << "\n";
            }

            return 5;
        }
        catch (...)
        {
            if (json)
            {
                std::cout
                    << "{\n"
                    << "  \"helper\": \"" << helperName << "\",\n"
                    << "  \"helperVersion\": \"" << jsonEscape(mw::app::appVersion) << "\",\n"
                    << "  \"ok\": false,\n"
                    << "  \"stage\": \"exception\",\n"
                    << "  \"error\": \"Unknown helper exception.\"\n"
                    << "}\n";
            }
            else
            {
                std::cerr << "Silent process validation failed: unknown helper exception.\n";
            }

            return 5;
        }
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
        std::cout << "PoorMansStudioClapHost " << mw::app::appVersionLabel << "\n";
        return 0;
    }

    if (args[0] == "--ping")
    {
        std::cout << "pong\n";
        return 0;
    }

    if (args[0] == "--scan" || args[0] == "--scan-json")
    {
        if (args.size() < 2)
        {
            std::cerr << args[0] << " requires a path to an outer .clap plugin file or bundle.\n";
            return 2;
        }

        return scanOne(args[1], args[0] == "--scan-json");
    }

    if (args[0] == "--validate-instance" || args[0] == "--validate-instance-json")
    {
        if (args.size() < 2)
        {
            std::cerr << args[0] << " requires a path to an outer .clap plugin file or bundle.\n";
            return 2;
        }

        return validateOne(args[1], args[0] == "--validate-instance-json");
    }

    if (args[0] == "--validate-activation" || args[0] == "--validate-activation-json")
    {
        if (args.size() < 2)
        {
            std::cerr << args[0] << " requires a path to an outer .clap plugin file or bundle.\n";
            return 2;
        }

        return validateActivationOne(args[1], args[0] == "--validate-activation-json");
    }


    if (args[0] == "--validate-process" || args[0] == "--validate-process-json")
    {
        if (args.size() < 2)
        {
            std::cerr << args[0] << " requires a path to an outer .clap plugin file or bundle.\n";
            return 2;
        }

        return validateProcessOne(args[1], args[0] == "--validate-process-json");
    }

    std::cerr << "Unknown command: " << args[0] << "\n\n";
    printHelp();
    return 2;
}
