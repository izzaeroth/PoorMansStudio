#pragma once

#include "clap/ClapPluginTypes.h"

#include <filesystem>
#include <string>
#include <vector>

namespace mw::clap
{
    struct ClapAbiProbeResult
    {
        bool attempted = false;
        bool loadedLibrary = false;
        bool foundEntry = false;
        bool initialized = false;
        bool foundFactory = false;
        bool foundDescriptor = false;
        int pluginCount = 0;
        std::string message;
        ClapPluginDescriptor descriptor;
    };

    struct ClapInstanceValidationResult
    {
        bool attempted = false;
        bool loadedLibrary = false;
        bool foundEntry = false;
        bool entryInitialized = false;
        bool foundFactory = false;
        bool foundDescriptor = false;
        bool instanceCreated = false;
        bool pluginInitialized = false;
        bool instanceDestroyed = false;
        bool entryDeinitialized = false;
        bool restartRequested = false;
        bool processRequested = false;
        bool callbackRequested = false;
        int pluginCount = 0;
        int selectedIndex = 0;
        std::string stage;
        std::string message;
        ClapPluginDescriptor descriptor;

        bool ok() const
        {
            return attempted
                && loadedLibrary
                && foundEntry
                && entryInitialized
                && foundFactory
                && foundDescriptor
                && instanceCreated
                && pluginInitialized
                && instanceDestroyed
                && entryDeinitialized;
        }
    };


    struct ClapActivationValidationResult
    {
        bool attempted = false;
        bool loadedLibrary = false;
        bool foundEntry = false;
        bool entryInitialized = false;
        bool foundFactory = false;
        bool foundDescriptor = false;
        bool instanceCreated = false;
        bool pluginInitialized = false;
        bool pluginActivated = false;
        bool pluginDeactivated = false;
        bool instanceDestroyed = false;
        bool entryDeinitialized = false;
        bool restartRequested = false;
        bool processRequested = false;
        bool callbackRequested = false;
        bool audioPortsExtensionAvailable = false;
        bool notePortsExtensionAvailable = false;
        int audioInputPortCount = -1;
        int audioOutputPortCount = -1;
        int noteInputPortCount = -1;
        int noteOutputPortCount = -1;
        int pluginCount = 0;
        int selectedIndex = 0;
        double sampleRate = 48000.0;
        int minFrames = 1;
        int maxFrames = 1024;
        std::string stage;
        std::string message;
        std::string audioPortsMessage;
        std::string notePortsMessage;
        std::vector<std::string> audioInputPorts;
        std::vector<std::string> audioOutputPorts;
        std::vector<std::string> noteInputPorts;
        std::vector<std::string> noteOutputPorts;
        ClapPluginDescriptor descriptor;

        bool ok() const
        {
            return attempted
                && loadedLibrary
                && foundEntry
                && entryInitialized
                && foundFactory
                && foundDescriptor
                && instanceCreated
                && pluginInitialized
                && pluginActivated
                && pluginDeactivated
                && instanceDestroyed
                && entryDeinitialized;
        }
    };



    struct ClapProcessValidationResult
    {
        bool attempted = false;
        bool loadedLibrary = false;
        bool foundEntry = false;
        bool entryInitialized = false;
        bool foundFactory = false;
        bool foundDescriptor = false;
        bool instanceCreated = false;
        bool pluginInitialized = false;
        bool pluginActivated = false;
        bool pluginStartedProcessing = false;
        bool pluginProcessCalled = false;
        bool pluginProcessReturnedOk = false;
        bool pluginStoppedProcessing = false;
        bool pluginDeactivated = false;
        bool instanceDestroyed = false;
        bool entryDeinitialized = false;
        bool restartRequested = false;
        bool processRequested = false;
        bool callbackRequested = false;
        bool audioPortsExtensionAvailable = false;
        bool notePortsExtensionAvailable = false;
        int audioInputPortCount = -1;
        int audioOutputPortCount = -1;
        int noteInputPortCount = -1;
        int noteOutputPortCount = -1;
        int processInputBufferCount = 0;
        int processOutputBufferCount = 0;
        int processInputChannelCount = 0;
        int processOutputChannelCount = 0;
        int processFrames = 64;
        int processStatus = -1;
        int outputEventCount = 0;
        double maxOutputAbs = 0.0;
        int pluginCount = 0;
        int selectedIndex = 0;
        double sampleRate = 48000.0;
        int minFrames = 1;
        int maxFrames = 1024;
        std::string stage;
        std::string message;
        std::string audioPortsMessage;
        std::string notePortsMessage;
        std::string processStatusText;
        std::string processMessage;
        std::vector<std::string> audioInputPorts;
        std::vector<std::string> audioOutputPorts;
        std::vector<std::string> noteInputPorts;
        std::vector<std::string> noteOutputPorts;
        ClapPluginDescriptor descriptor;

        bool ok() const
        {
            return attempted
                && loadedLibrary
                && foundEntry
                && entryInitialized
                && foundFactory
                && foundDescriptor
                && instanceCreated
                && pluginInitialized
                && pluginActivated
                && pluginStartedProcessing
                && pluginProcessCalled
                && pluginProcessReturnedOk
                && pluginStoppedProcessing
                && pluginDeactivated
                && instanceDestroyed
                && entryDeinitialized;
        }
    };

    class ClapAbiProbe
    {
    public:
        static ClapAbiProbeResult probePluginPath(const std::filesystem::path& outerPluginPath);
        static ClapInstanceValidationResult validatePluginInstance(const std::filesystem::path& outerPluginPath,
                                                                   int pluginIndex = 0);
        static ClapActivationValidationResult validatePluginActivation(const std::filesystem::path& outerPluginPath,
                                                                       int pluginIndex = 0,
                                                                       double sampleRate = 48000.0,
                                                                       int minFrames = 1,
                                                                       int maxFrames = 1024);
        static ClapProcessValidationResult validatePluginSilentProcess(const std::filesystem::path& outerPluginPath,
                                                                        int pluginIndex = 0,
                                                                        double sampleRate = 48000.0,
                                                                        int minFrames = 1,
                                                                        int maxFrames = 1024,
                                                                        int processFrames = 64);
    };
}
