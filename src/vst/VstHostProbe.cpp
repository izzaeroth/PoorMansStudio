#include "vst/VstHostProbe.h"

#include "vst/VstPluginScanner.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <utility>

namespace
{
    std::string fnv1a64(const void* data, std::size_t size)
    {
        constexpr std::uint64_t offsetBasis = 14695981039346656037ull;
        constexpr std::uint64_t prime = 1099511628211ull;

        auto hash = offsetBasis;
        const auto* bytes = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < size; ++i)
        {
            hash ^= static_cast<std::uint64_t>(bytes[i]);
            hash *= prime;
        }

        std::ostringstream stream;
        stream << std::hex << std::setfill('0') << std::setw(16) << hash;
        return stream.str();
    }

    std::string busSummary(juce::AudioProcessor& instance, bool isInput, int index)
    {
        auto name = std::string(isInput ? "input " : "output ") + std::to_string(index);
        if (const auto* bus = instance.getBus(isInput, index))
        {
            const auto busName = bus->getName().toStdString();
            if (!busName.empty())
                name = busName;
        }

        return name + " (channels=" + std::to_string(instance.getChannelCountOfBus(isInput, index)) + ")";
    }

    void populateInstanceInspection(juce::AudioPluginInstance& instance, mw::vst::VstHostValidationResult& result)
    {
        result.inputChannels = instance.getTotalNumInputChannels();
        result.outputChannels = instance.getTotalNumOutputChannels();
        result.inputBusCount = instance.getBusCount(true);
        result.outputBusCount = instance.getBusCount(false);
        result.acceptsMidi = instance.acceptsMidi();
        result.producesMidi = instance.producesMidi();
        result.midiEffect = instance.isMidiEffect();
        result.editorAvailable = instance.hasEditor();
        result.parameterCount = instance.getParameters().size();
        result.latencySamples = instance.getLatencySamples();
        result.tailSeconds = instance.getTailLengthSeconds();

        result.inputBuses.clear();
        result.outputBuses.clear();
        for (int i = 0; i < result.inputBusCount; ++i)
            result.inputBuses.push_back(busSummary(instance, true, i));
        for (int i = 0; i < result.outputBusCount; ++i)
            result.outputBuses.push_back(busSummary(instance, false, i));
    }

    void populateDescriptor(const juce::PluginDescription& description, mw::vst::VstPluginDescriptor& descriptor)
    {
        descriptor.name = description.name.toStdString();
        descriptor.vendor = description.manufacturerName.toStdString();
        descriptor.version = description.version.toStdString();
        descriptor.category = description.category.toStdString();
        descriptor.reportedCategory = descriptor.category;
        descriptor.reportedIdentifier = description.createIdentifierString().toStdString();
        descriptor.reportedFormat = description.pluginFormatName.toStdString();
        descriptor.reportedDescriptiveName = description.descriptiveName.toStdString();
        descriptor.juceDescriptionAvailable = true;
        descriptor.juceReportedInstrument = description.isInstrument;
        descriptor.reportedAudioInputs = description.numInputChannels;
        descriptor.reportedAudioOutputs = description.numOutputChannels;
        descriptor.status = mw::vst::VstPluginScanStatus::Ok;
    }

    struct InstanceHandle
    {
        std::unique_ptr<juce::AudioPluginInstance> instance;
        bool prepared = false;

        void release(mw::vst::VstHostValidationResult& result)
        {
            if (instance != nullptr && prepared)
            {
                instance->suspendProcessing(true);
                instance->releaseResources();
                prepared = false;
                result.resourcesReleased = true;
                ++result.resourceReleaseCount;
            }

            if (instance != nullptr)
            {
                instance.reset();
                result.instanceDestroyed = true;
                ++result.instanceDestroyCount;
            }
        }
    };

    bool createInstance(juce::AudioPluginFormatManager& formatManager,
                        const juce::PluginDescription& description,
                        mw::vst::VstHostValidationResult& result,
                        InstanceHandle& handle,
                        std::string& errorMessage)
    {
        juce::String error;
        handle.instance = formatManager.createPluginInstance(description, result.sampleRate, result.blockSize, error);
        if (handle.instance == nullptr)
        {
            errorMessage = error.isEmpty() ? "JUCE returned a null VST3 instance." : error.toStdString();
            return false;
        }

        result.instanceCreated = true;
        ++result.instanceCreateCount;
        populateInstanceInspection(*handle.instance, result);
        return true;
    }

    void prepareInstance(const juce::PluginDescription& description,
                         mw::vst::VstHostValidationResult& result,
                         InstanceHandle& handle)
    {
        // Match the channel layouts Poor Man's Studio currently requests when it hosts
        // VST3 instruments and effects in-process.
        result.requestedInputChannels = description.isInstrument ? 0 : 2;
        result.requestedOutputChannels = 2;

        handle.instance->setPlayConfigDetails(result.requestedInputChannels,
                                              result.requestedOutputChannels,
                                              result.sampleRate,
                                              result.blockSize);
        result.layoutConfigured = true;
        handle.instance->prepareToPlay(result.sampleRate, result.blockSize);
        handle.prepared = true;
        result.prepared = true;
        ++result.prepareCount;
        handle.instance->suspendProcessing(false);
        result.processingEnabled = true;
        populateInstanceInspection(*handle.instance, result);
        result.requestedLayoutMatched = result.inputChannels == result.requestedInputChannels
            && result.outputChannels == result.requestedOutputChannels;
    }

    bool processSilentBlock(juce::AudioPluginInstance& instance, mw::vst::VstHostValidationResult& result)
    {
        const auto channelCount = std::max(1, std::max(instance.getTotalNumInputChannels(), instance.getTotalNumOutputChannels()));
        juce::AudioBuffer<float> audio(channelCount, result.processFrames);
        audio.clear();
        juce::MidiBuffer midi;

        result.processCalled = true;
        instance.processBlock(audio, midi);
        result.processCompleted = true;

        double maximum = 0.0;
        int nonFinite = 0;
        const auto outputChannels = std::max(0, instance.getTotalNumOutputChannels());
        for (int channel = 0; channel < outputChannels; ++channel)
        {
            const auto* samples = audio.getReadPointer(channel);
            for (int sample = 0; sample < result.processFrames; ++sample)
            {
                const auto value = samples[sample];
                if (!std::isfinite(value))
                {
                    ++nonFinite;
                    continue;
                }
                maximum = std::max(maximum, static_cast<double>(std::abs(value)));
            }
        }

        result.nonFiniteSampleCount = nonFinite;
        result.finiteOutput = nonFinite == 0;
        result.maxOutputAbs = maximum;
        return result.finiteOutput;
    }
}

namespace mw::vst
{
    bool VstHostValidationResult::ok() const
    {
        if (!attempted || !descriptionsEnumerated || !descriptionSelected || !instanceCreated || !instanceDestroyed)
            return false;

        if (mode == VstValidationMode::Instance)
            return instanceCreateCount == 1 && instanceDestroyCount == 1;

        if (!prepared || !resourcesReleased)
            return false;

        if (mode == VstValidationMode::Activation)
            return instanceCreateCount == 1 && prepareCount == 1
                && resourceReleaseCount == 1 && instanceDestroyCount == 1;

        if (mode == VstValidationMode::Process)
            return instanceCreateCount == 1 && prepareCount == 1
                && resourceReleaseCount == 1 && instanceDestroyCount == 1
                && processCalled && processCompleted && finiteOutput;

        return instanceCreateCount == 2 && prepareCount == 2
            && resourceReleaseCount == 2 && instanceDestroyCount == 2
            && stateCaptured && stateRestored && stateRecaptured
            && processCalled && processCompleted && finiteOutput;
    }

    VstHostValidationResult VstHostProbe::validate(const std::filesystem::path& outerBundlePath,
                                                    VstValidationMode mode,
                                                    int pluginIndex,
                                                    double sampleRate,
                                                    int blockSize,
                                                    int processFrames)
    {
        VstHostValidationResult result;
        result.mode = mode;
        result.attempted = true;
        result.selectedIndex = std::max(0, pluginIndex);
        result.sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
        result.blockSize = std::max(1, blockSize);
        result.processFrames = std::clamp(processFrames, 1, result.blockSize);
        result.descriptor = VstPluginScanner::inspectBundle(outerBundlePath);

        auto fail = [&result](std::string stage, std::string message)
        {
            result.stage = std::move(stage);
            result.message = std::move(message);
            result.descriptor.status = result.descriptor.status == VstPluginScanStatus::Missing
                ? VstPluginScanStatus::Missing
                : VstPluginScanStatus::Failed;
            result.descriptor.statusMessage = result.message;
            return result;
        };

        InstanceHandle first;
        InstanceHandle second;

        try
        {
            if (!std::filesystem::exists(outerBundlePath))
                return fail("path", "VST3 bundle was not found.");

            if (!VstPluginScanner::isOuterVst3Bundle(outerBundlePath))
                return fail("path", "Path is not an outer .vst3 bundle.");

            juce::AudioPluginFormatManager formatManager;
            formatManager.addFormat(new juce::VST3PluginFormat());

            juce::OwnedArray<juce::PluginDescription> descriptions;
            for (int i = 0; i < formatManager.getNumFormats(); ++i)
            {
                if (auto* format = formatManager.getFormat(i))
                    if (format->getName().containsIgnoreCase("VST3"))
                        format->findAllTypesForFile(descriptions, juce::String(outerBundlePath.string()));
            }

            result.descriptionsEnumerated = true;
            result.pluginCount = descriptions.size();
            if (descriptions.isEmpty())
                return fail("description_enumeration", "JUCE did not find a loadable VST3 description in the bundle.");

            if (result.selectedIndex >= descriptions.size())
                return fail("description_selection", "Requested VST3 plugin index is outside the bundle's descriptor range.");

            const auto* selected = descriptions[result.selectedIndex];
            if (selected == nullptr)
                return fail("description_selection", "JUCE returned a null VST3 plugin description.");

            result.descriptionSelected = true;
            populateDescriptor(*selected, result.descriptor);
            result.descriptor.statusMessage = "JUCE selected VST3 descriptor " + std::to_string(result.selectedIndex + 1)
                + " of " + std::to_string(result.pluginCount) + ".";

            std::string createError;
            if (!createInstance(formatManager, *selected, result, first, createError))
                return fail("instance_creation", "JUCE could not create the selected VST3 instance: " + createError);

            if (mode == VstValidationMode::Instance)
            {
                first.release(result);
                result.stage = "complete";
                result.message = "JUCE created, inspected, and destroyed the selected VST3 instance successfully.";
                return result;
            }

            prepareInstance(*selected, result, first);

            if (mode == VstValidationMode::Activation)
            {
                first.release(result);
                result.stage = "complete";
                result.message = "JUCE created, prepared, inspected, released, and destroyed the selected VST3 instance successfully.";
                return result;
            }

            if (mode == VstValidationMode::Process)
            {
                if (!processSilentBlock(*first.instance, result))
                {
                    first.release(result);
                    return fail("silent_process", "VST3 processBlock() produced non-finite output samples.");
                }

                first.release(result);
                result.stage = "complete";
                result.message = "JUCE processed one silent VST3 block and cleaned up successfully.";
                return result;
            }

            juce::MemoryBlock firstState;
            first.instance->getStateInformation(firstState);
            result.stateCaptured = true;
            result.firstStateBytes = firstState.getSize();
            result.firstStateHash = fnv1a64(firstState.getData(), firstState.getSize());
            if (firstState.getSize() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
            {
                first.release(result);
                return fail("state_capture", "VST3 state data is too large for JUCE setStateInformation().");
            }
            first.release(result);

            if (!createInstance(formatManager, *selected, result, second, createError))
                return fail("state_recreate", "JUCE could not recreate the selected VST3 instance: " + createError);
            prepareInstance(*selected, result, second);

            second.instance->suspendProcessing(true);
            second.instance->setStateInformation(firstState.getData(), static_cast<int>(firstState.getSize()));
            second.instance->suspendProcessing(false);
            second.instance->reset();
            result.stateRestored = true;

            if (!processSilentBlock(*second.instance, result))
            {
                second.release(result);
                return fail("state_process", "VST3 produced non-finite samples after state restore.");
            }

            juce::MemoryBlock secondState;
            second.instance->getStateInformation(secondState);
            result.stateRecaptured = true;
            result.secondStateBytes = secondState.getSize();
            result.secondStateHash = fnv1a64(secondState.getData(), secondState.getSize());
            result.stateByteEquivalent = firstState == secondState;
            second.release(result);

            result.stage = "complete";
            result.message = "JUCE completed VST3 state save, recreate, restore, silent process, and second state capture successfully.";
            return result;
        }
        catch (const std::exception& e)
        {
            first.release(result);
            second.release(result);
            return fail("exception", e.what());
        }
        catch (...)
        {
            first.release(result);
            second.release(result);
            return fail("exception", "Unknown JUCE VST3 validation exception.");
        }
    }
}
