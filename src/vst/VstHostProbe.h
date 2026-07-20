#pragma once

#include "vst/VstPluginTypes.h"

#include <filesystem>
#include <string>
#include <vector>

namespace mw::vst
{
    enum class VstValidationMode
    {
        Instance,
        Activation,
        Process,
        State
    };

    struct VstHostValidationResult
    {
        VstValidationMode mode = VstValidationMode::Instance;
        bool attempted = false;
        bool descriptionsEnumerated = false;
        bool descriptionSelected = false;
        bool instanceCreated = false;
        int instanceCreateCount = 0;
        bool prepared = false;
        int prepareCount = 0;
        bool processingEnabled = false;
        bool processCalled = false;
        bool processCompleted = false;
        bool finiteOutput = true;
        int nonFiniteSampleCount = 0;
        double maxOutputAbs = 0.0;
        bool stateCaptured = false;
        bool stateRestored = false;
        bool stateRecaptured = false;
        bool stateByteEquivalent = false;
        bool resourcesReleased = false;
        int resourceReleaseCount = 0;
        bool instanceDestroyed = false;
        int instanceDestroyCount = 0;
        int pluginCount = 0;
        int selectedIndex = 0;
        double sampleRate = 48000.0;
        int blockSize = 512;
        int processFrames = 64;
        int requestedInputChannels = 0;
        int requestedOutputChannels = 0;
        bool layoutConfigured = false;
        bool requestedLayoutMatched = false;
        int inputChannels = 0;
        int outputChannels = 0;
        int inputBusCount = 0;
        int outputBusCount = 0;
        bool acceptsMidi = false;
        bool producesMidi = false;
        bool midiEffect = false;
        bool editorAvailable = false;
        int parameterCount = 0;
        int latencySamples = 0;
        double tailSeconds = 0.0;
        std::size_t firstStateBytes = 0;
        std::size_t secondStateBytes = 0;
        std::string firstStateHash;
        std::string secondStateHash;
        std::string stage;
        std::string message;
        std::vector<std::string> inputBuses;
        std::vector<std::string> outputBuses;
        VstPluginDescriptor descriptor;

        bool ok() const;
    };

    class VstHostProbe
    {
    public:
        static VstHostValidationResult validate(const std::filesystem::path& outerBundlePath,
                                                VstValidationMode mode,
                                                int pluginIndex = 0,
                                                double sampleRate = 48000.0,
                                                int blockSize = 512,
                                                int processFrames = 64);
    };
}
