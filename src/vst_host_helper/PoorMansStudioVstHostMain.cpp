#include "app/AppVersion.h"
#include "vst/VstPluginScanner.h"
#include "vst/VstPluginTypes.h"

#include <juce_core/juce_core.h>

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
            << "Out-of-process VST3 helper foundation for Poor Man's Studio.\n\n"
            << "Commands:\n"
            << "  --help                 Show this help text.\n"
            << "  --version              Print helper version.\n"
            << "  --ping                 Print pong and exit.\n"
            << "  --scan <plugin.vst3>   Inspect one outer VST3 bundle in the helper process.\n"
            << "  --scan-json <plugin.vst3>\n"
            << "                         Inspect one outer VST3 bundle and print JSON.\n\n"
            << "Notes:\n"
            << "  This helper is Phase 3 groundwork. It is not yet the main app's live\n"
            << "  VST bridge, audio renderer, or plugin editor owner.\n";
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
                        << "  \"statusMessage\": \"VST3 bundle was not found.\",\n"
                        << "  \"bundlePath\": \"" << jsonEscape(pluginPath.string()) << "\"\n"
                        << "}\n";
                }
                else
                {
                    std::cerr << "VST3 bundle was not found: " << pluginPath.string() << "\n";
                }

                return 3;
            }

            if (!mw::vst::VstPluginScanner::isOuterVst3Bundle(pluginPath))
            {
                if (json)
                {
                    std::cout
                        << "{\n"
                        << "  \"helper\": \"" << helperName << "\",\n"
                        << "  \"helperVersion\": \"" << jsonEscape(mw::app::appVersion) << "\",\n"
                        << "  \"status\": \"Failed\",\n"
                        << "  \"statusMessage\": \"Path is not an outer .vst3 bundle. Point to the plugin folder ending in .vst3, not an inner binary.\",\n"
                        << "  \"bundlePath\": \"" << jsonEscape(pluginPath.string()) << "\"\n"
                        << "}\n";
                }
                else
                {
                    std::cerr << "Path is not an outer .vst3 bundle: " << pluginPath.string() << "\n";
                    std::cerr << "Point to the plugin folder ending in .vst3, not an inner binary.\n";
                }

                return 4;
            }

            auto descriptor = mw::vst::VstPluginScanner::inspectBundle(pluginPath);

            if (json)
                printDescriptorJson(descriptor);
            else
                printDescriptorText(descriptor);

            switch (descriptor.status)
            {
                case mw::vst::VstPluginScanStatus::Ok:
                case mw::vst::VstPluginScanStatus::Warning:
                    return 0;
                case mw::vst::VstPluginScanStatus::Missing:
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
                    << "  \"status\": \"Failed\",\n"
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
                    << "  \"status\": \"Failed\",\n"
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
        std::cout << "PoorMansStudioVstHost " << mw::app::appVersionLabel << "\n";
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
            std::cerr << args[0] << " requires a path to an outer .vst3 bundle.\n";
            return 2;
        }

        return scanOne(args[1], args[0] == "--scan-json");
    }

    std::cerr << "Unknown command: " << args[0] << "\n\n";
    printHelp();
    return 2;
}
