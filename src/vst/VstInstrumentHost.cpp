#include "vst/VstInstrumentHost.h"

#include "core/Project.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <sstream>
#include <system_error>

namespace
{
    std::string errorText(const juce::String& text)
    {
        return text.toStdString();
    }

    std::int64_t projectEndTickForTrack(const mw::core::Track& track)
    {
        std::int64_t endTick = 0;
        for (const auto& note : track.getNotes())
            endTick = std::max<std::int64_t>(endTick, note.startTick + note.durationTicks);
        return endTick;
    }

    bool pathsReferToSameLocation(const std::filesystem::path& a, const std::filesystem::path& b)
    {
        if (a.empty() || b.empty())
            return false;

        std::error_code ec;
        if (std::filesystem::exists(a, ec) && std::filesystem::exists(b, ec))
            return std::filesystem::equivalent(a, b, ec);

        return std::filesystem::absolute(a).lexically_normal() == std::filesystem::absolute(b).lexically_normal();
    }

    std::filesystem::path temporaryEffectRenderPathFor(const std::filesystem::path& input)
    {
        auto temp = input;
        temp.replace_filename(input.stem().string() + ".vstfx.tmp.wav");
        return temp;
    }
}

namespace mw::vst
{
    bool VstInstrumentHost::trackHasVstPlugin(const mw::core::Track& track)
    {
        return track.getInstrument().backendType == mw::core::SampleBackendType::VST3
            && !track.getInstrument().vst3.bundlePath.empty();
    }

    bool VstInstrumentHost::trackHasEnabledVstEffect(const mw::core::Track& track)
    {
        const auto& effects = track.getVstEffects();
        for (std::size_t slotIndex = 0; slotIndex < mw::core::maxVstEffectSlots; ++slotIndex)
        {
            const auto* slot = effects.slot(slotIndex);
            if (slot != nullptr
                && effects.slotEnabled(slotIndex)
                && !slot->plugin.bypassed
                && slot->plugin.hasPluginIdentity()
                && !slot->plugin.bundlePath.empty())
                return true;
        }

        return false;
    }

    VstLoadResult VstInstrumentHost::loadPluginAssignment(const mw::core::VstPluginAssignment& pluginAssignment,
                                                           double sampleRate,
                                                           int blockSize,
                                                           int inputChannels,
                                                           int outputChannels)
    {
        VstLoadResult result;

        if (pluginAssignment.bundlePath.empty())
        {
            result.message = "VST3 plugin assignment does not have a bundle path.";
            return result;
        }

        const auto pluginPath = pluginAssignment.bundlePath;
        if (!std::filesystem::exists(pluginPath))
        {
            result.message = "VST3 bundle not found: " + pluginPath.string();
            return result;
        }

        juce::AudioPluginFormatManager formatManager;
        formatManager.addFormat(new juce::VST3PluginFormat());

        juce::OwnedArray<juce::PluginDescription> descriptions;
        try
        {
            for (int i = 0; i < formatManager.getNumFormats(); ++i)
            {
                auto* format = formatManager.getFormat(i);
                if (format == nullptr)
                    continue;

                if (format->getName().containsIgnoreCase("VST3"))
                    format->findAllTypesForFile(descriptions, juce::String(pluginPath.string()));
            }
        }
        catch (const std::exception& ex)
        {
            result.message = std::string("VST3 scan/load failed while reading plugin descriptions: ") + ex.what();
            return result;
        }
        catch (...)
        {
            result.message = "VST3 scan/load failed while reading plugin descriptions.";
            return result;
        }

        if (descriptions.isEmpty())
        {
            result.message = "No loadable VST3 plugin description found in: " + pluginPath.string();
            return result;
        }

        const juce::PluginDescription* selected = descriptions[0];
        for (auto* description : descriptions)
        {
            if (description == nullptr)
                continue;

            if (pluginAssignment.uid.empty()
                || description->createIdentifierString().toStdString() == pluginAssignment.uid
                || description->name.toStdString() == pluginAssignment.name)
            {
                selected = description;
                break;
            }
        }

        juce::String error;
        std::unique_ptr<juce::AudioPluginInstance> instance;
        try
        {
            instance = formatManager.createPluginInstance(*selected, sampleRate, blockSize, error);
            if (instance == nullptr)
            {
                result.message = "Failed to load VST3 plugin: " + errorText(error);
                return result;
            }

            const int safeInputChannels = std::max(0, inputChannels);
            const int safeOutputChannels = std::max(1, outputChannels);
            instance->setPlayConfigDetails(safeInputChannels, safeOutputChannels, sampleRate, blockSize);
            instance->prepareToPlay(sampleRate, blockSize);

            if (!pluginAssignment.stateBase64.empty())
            {
                juce::MemoryBlock stateData;
                if (stateData.fromBase64Encoding(pluginAssignment.stateBase64))
                {
                    try
                    {
                        // Track-owned VST3 state must be restored before an editor
                        // is created. Some plugin UIs only paint their current
                        // parameters correctly when the processor state is already
                        // loaded before createEditorIfNeeded() runs.
                        instance->suspendProcessing(true);
                        instance->setStateInformation(stateData.getData(), static_cast<int>(stateData.getSize()));
                        instance->suspendProcessing(false);
                        instance->reset();
                        result.savedStateApplied = true;
                        result.savedStateMessage = "Restored saved track VST3 state before plugin editor/render use.";
                    }
                    catch (...)
                    {
                        result.savedStateRestoreFailed = true;
                        result.savedStateMessage = "Failed while applying saved track VST3 state.";
                        instance->suspendProcessing(false);
                        throw;
                    }
                }
                else
                {
                    result.savedStateRestoreFailed = true;
                    result.savedStateMessage = "Saved track VST3 state was not valid Base64 and was ignored.";
                }
            }
        }
        catch (const std::exception& ex)
        {
            result.message = std::string("Failed to initialise VST3 plugin: ") + ex.what();
            return result;
        }
        catch (...)
        {
            result.message = "Failed to initialise VST3 plugin.";
            return result;
        }

        result.success = true;
        result.message = "Loaded VST3 plugin: " + selected->name.toStdString();
        result.description = *selected;
        result.instance = std::move(instance);
        return result;
    }

    VstLoadResult VstInstrumentHost::loadInstrumentForTrack(const mw::core::Track& track, double sampleRate, int blockSize)
    {
        VstLoadResult result;

        if (!trackHasVstPlugin(track))
        {
            result.message = "Track does not have a VST3 plugin assignment.";
            return result;
        }

        return loadPluginAssignment(track.getInstrument().vst3, sampleRate, blockSize, 0, 2);
    }

    juce::MidiBuffer VstInstrumentHost::buildMidiForBlock(const mw::core::Track& track, std::int64_t blockStartSample, int blockNumSamples, double samplesPerTick)
    {
        juce::MidiBuffer buffer;
        if (samplesPerTick <= 0.0)
            return buffer;

        const auto blockEndSample = blockStartSample + blockNumSamples;

        for (const auto& note : track.getNotes())
        {
            const auto channel = juce::jlimit(1, 16, note.midiChannel);
            const auto pitch = juce::jlimit(0, 127, note.pitch);
            const auto velocity = static_cast<juce::uint8>(juce::jlimit(1, 127, note.velocity));
            const auto noteOnSample = static_cast<std::int64_t>(std::llround(static_cast<double>(note.startTick) * samplesPerTick));
            const auto noteOffSample = static_cast<std::int64_t>(std::llround(static_cast<double>(note.startTick + note.durationTicks) * samplesPerTick));

            if (noteOnSample >= blockStartSample && noteOnSample < blockEndSample)
                buffer.addEvent(juce::MidiMessage::noteOn(channel, pitch, velocity), static_cast<int>(noteOnSample - blockStartSample));

            if (noteOffSample >= blockStartSample && noteOffSample < blockEndSample)
                buffer.addEvent(juce::MidiMessage::noteOff(channel, pitch), static_cast<int>(noteOffSample - blockStartSample));
        }

        return buffer;
    }

    VstRenderResult VstInstrumentHost::renderTrackToWav(const VstRenderRequest& request)
    {
        VstRenderResult result;
        result.wavPath = request.wavOutputPath;

        if (request.cancelRequested != nullptr && request.cancelRequested->load())
        {
            result.cancelled = true;
            result.message = "VST3 render cancelled before start.";
            return result;
        }

        auto load = loadInstrumentForTrack(request.track, request.sampleRate, request.blockSize);
        if (!load.success || load.instance == nullptr)
        {
            result.message = load.message;
            return result;
        }

        std::error_code ec;
        std::filesystem::create_directories(request.wavOutputPath.parent_path(), ec);

        juce::WavAudioFormat wavFormat;
        std::unique_ptr<juce::FileOutputStream> stream(juce::File(request.wavOutputPath.string()).createOutputStream());
        if (stream == nullptr || !stream->openedOk())
        {
            result.message = "Could not create VST3 render WAV file: " + request.wavOutputPath.string();
            return result;
        }

        std::unique_ptr<juce::AudioFormatWriter> writer(
            wavFormat.createWriterFor(stream.get(), request.sampleRate, static_cast<unsigned int>(request.channelCount), 24, {}, 0)
        );

        if (writer == nullptr)
        {
            result.message = "Could not create WAV writer for VST3 render.";
            return result;
        }
        stream.release();

        const double ticksPerSecond = static_cast<double>(mw::core::Project::ticksPerQuarterNote) * static_cast<double>(request.tempoBpm) / 60.0;
        const double samplesPerTick = static_cast<double>(request.sampleRate) / std::max(1.0, ticksPerSecond);
        const auto endTick = projectEndTickForTrack(request.track);
        const auto tailSamples = static_cast<std::int64_t>(request.sampleRate * 4); // safe first-pass release/tail allowance
        const auto totalSamples = static_cast<std::int64_t>(std::ceil(static_cast<double>(endTick) * samplesPerTick)) + tailSamples;

        juce::AudioBuffer<float> audio(request.channelCount, request.blockSize);
        std::int64_t sample = 0;

        while (sample < totalSamples)
        {
            if (request.cancelRequested != nullptr && request.cancelRequested->load())
            {
                load.instance->releaseResources();
                result.cancelled = true;
                result.message = "VST3 render cancelled.";
                return result;
            }

            const int blockSamples = static_cast<int>(std::min<std::int64_t>(request.blockSize, totalSamples - sample));
            audio.setSize(request.channelCount, blockSamples, false, false, true);
            audio.clear();

            auto midi = buildMidiForBlock(request.track, sample, blockSamples, samplesPerTick);
            try
            {
                load.instance->processBlock(audio, midi);
                writer->writeFromAudioSampleBuffer(audio, 0, blockSamples);
            }
            catch (const std::exception& ex)
            {
                load.instance->releaseResources();
                result.message = std::string("VST3 render failed while processing audio: ") + ex.what();
                return result;
            }
            catch (...)
            {
                load.instance->releaseResources();
                result.message = "VST3 render failed while processing audio.";
                return result;
            }
            sample += blockSamples;
        }

        juce::MidiBuffer panic;
        for (int ch = 1; ch <= 16; ++ch)
        {
            panic.addEvent(juce::MidiMessage::allNotesOff(ch), 0);
            panic.addEvent(juce::MidiMessage::allSoundOff(ch), 0);
        }
        audio.setSize(request.channelCount, request.blockSize, false, false, true);
        audio.clear();
        try
        {
            load.instance->processBlock(audio, panic);
        }
        catch (...)
        {
            // Best-effort all-notes-off only; do not turn a successful render into a crash.
        }
        load.instance->releaseResources();

        result.success = true;
        result.message = "Rendered VST3 track to WAV: " + request.wavOutputPath.string();
        return result;
    }

    namespace
    {
        bool copyAudioFileForUnprocessedEffect(const std::filesystem::path& input, const std::filesystem::path& output, std::string& error)
        {
            if (output.empty() || pathsReferToSameLocation(input, output))
                return true;

            std::error_code ec;
            std::filesystem::create_directories(output.parent_path(), ec);
            ec.clear();
            std::filesystem::copy_file(input, output, std::filesystem::copy_options::overwrite_existing, ec);
            if (ec)
            {
                error = ec.message();
                return false;
            }

            return std::filesystem::exists(output);
        }

        VstEffectProcessResult processWavWithTrackEffectSlot(const VstEffectProcessRequest& request, std::size_t slotIndex)
        {
            VstEffectProcessResult result;
            result.wavPath = request.outputWavPath.empty() ? request.inputWavPath : request.outputWavPath;

            if (request.cancelRequested != nullptr && request.cancelRequested->load())
            {
                result.cancelled = true;
                result.message = "VST3 effect processing cancelled before start.";
                return result;
            }

            const auto& effects = request.track.getVstEffects();
            const auto* slot = effects.slot(slotIndex);
            if (slot == nullptr || !slot->plugin.hasPluginIdentity())
            {
                std::string copyError;
                result.success = copyAudioFileForUnprocessedEffect(request.inputWavPath, result.wavPath, copyError);
                result.effectApplied = false;
                result.message = result.success
                    ? ("No assigned VST3 effect in slot " + std::to_string(slotIndex + 1) + "; WAV left unchanged.")
                    : ("No assigned VST3 effect in slot " + std::to_string(slotIndex + 1) + "; dry WAV copy failed: " + copyError);
                return result;
            }

            if (!effects.slotEnabled(slotIndex) || slot->plugin.bypassed)
            {
                std::string copyError;
                result.success = copyAudioFileForUnprocessedEffect(request.inputWavPath, result.wavPath, copyError);
                result.effectApplied = false;
                result.message = result.success
                    ? ("VST3 effect slot " + std::to_string(slotIndex + 1) + (slot->plugin.bypassed ? " bypassed" : " disabled") + "; WAV left unchanged.")
                    : ("VST3 effect slot " + std::to_string(slotIndex + 1) + " dry WAV copy failed: " + copyError);
                return result;
            }

            if (request.inputWavPath.empty() || !std::filesystem::exists(request.inputWavPath))
            {
                result.message = "VST3 effect input WAV not found: " + request.inputWavPath.string();
                return result;
            }

            juce::AudioFormatManager formatManager;
            formatManager.registerBasicFormats();

            std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(juce::File(request.inputWavPath.string())));
            if (reader == nullptr)
            {
                result.message = "Could not open WAV for VST3 effect processing: " + request.inputWavPath.string();
                return result;
            }

            const int channelCount = juce::jlimit(1, 8, static_cast<int>(reader->numChannels));
            const int sampleRate = static_cast<int>(std::max(1.0, reader->sampleRate));
            const int blockSize = juce::jlimit(64, 8192, request.blockSize);
            const auto totalInputSamples = static_cast<std::int64_t>(reader->lengthInSamples);
            const auto tailSamples = static_cast<std::int64_t>(std::max(0.0, request.tailSeconds) * static_cast<double>(sampleRate));

            auto load = VstInstrumentHost::loadPluginAssignment(slot->plugin, static_cast<double>(sampleRate), blockSize, channelCount, channelCount);
            if (!load.success || load.instance == nullptr)
            {
                result.message = "Failed to load VST3 effect slot " + std::to_string(slotIndex + 1) + " for offline processing: " + load.message;
                return result;
            }

            const auto effectName = slot->plugin.name.empty()
                ? slot->plugin.bundlePath.filename().string()
                : slot->plugin.name;
            const bool hasSavedEffectState = !slot->plugin.stateBase64.empty();

            const bool replacingInput = result.wavPath.empty() || pathsReferToSameLocation(request.inputWavPath, result.wavPath);
            const auto actualOutputPath = replacingInput ? temporaryEffectRenderPathFor(request.inputWavPath) : result.wavPath;
            result.wavPath = replacingInput ? request.inputWavPath : actualOutputPath;

            std::error_code ec;
            std::filesystem::create_directories(actualOutputPath.parent_path(), ec);
            ec.clear();
            if (std::filesystem::exists(actualOutputPath, ec))
                std::filesystem::remove(actualOutputPath, ec);

            juce::WavAudioFormat wavFormat;
            std::unique_ptr<juce::FileOutputStream> stream(juce::File(actualOutputPath.string()).createOutputStream());
            if (stream == nullptr || !stream->openedOk())
            {
                load.instance->releaseResources();
                result.message = "Could not create VST3 effect output WAV: " + actualOutputPath.string();
                return result;
            }

            std::unique_ptr<juce::AudioFormatWriter> writer(
                wavFormat.createWriterFor(stream.get(), static_cast<double>(sampleRate), static_cast<unsigned int>(channelCount), 24, {}, 0)
            );

            if (writer == nullptr)
            {
                load.instance->releaseResources();
                result.message = "Could not create WAV writer for VST3 effect output.";
                return result;
            }
            stream.release();

            juce::AudioBuffer<float> audio(channelCount, blockSize);
            juce::MidiBuffer emptyMidi;
            std::int64_t sample = 0;

            try
            {
                while (sample < totalInputSamples)
                {
                    if (request.cancelRequested != nullptr && request.cancelRequested->load())
                    {
                        load.instance->releaseResources();
                        result.cancelled = true;
                        result.message = "VST3 effect processing cancelled.";
                        return result;
                    }

                    const int blockSamples = static_cast<int>(std::min<std::int64_t>(blockSize, totalInputSamples - sample));
                    audio.setSize(channelCount, blockSamples, false, false, true);
                    audio.clear();
                    reader->read(&audio, 0, blockSamples, sample, true, true);
                    emptyMidi.clear();
                    load.instance->processBlock(audio, emptyMidi);
                    writer->writeFromAudioSampleBuffer(audio, 0, blockSamples);
                    sample += blockSamples;
                }

                std::int64_t tailWritten = 0;
                while (tailWritten < tailSamples)
                {
                    if (request.cancelRequested != nullptr && request.cancelRequested->load())
                    {
                        load.instance->releaseResources();
                        result.cancelled = true;
                        result.message = "VST3 effect processing cancelled during tail render.";
                        return result;
                    }

                    const int blockSamples = static_cast<int>(std::min<std::int64_t>(blockSize, tailSamples - tailWritten));
                    audio.setSize(channelCount, blockSamples, false, false, true);
                    audio.clear();
                    emptyMidi.clear();
                    load.instance->processBlock(audio, emptyMidi);
                    writer->writeFromAudioSampleBuffer(audio, 0, blockSamples);
                    tailWritten += blockSamples;
                }
            }
            catch (const std::exception& ex)
            {
                load.instance->releaseResources();
                result.message = std::string("VST3 effect processing failed while processing audio: ") + ex.what();
                return result;
            }
            catch (...)
            {
                load.instance->releaseResources();
                result.message = "VST3 effect processing failed while processing audio.";
                return result;
            }

            load.instance->releaseResources();
            writer.reset();
            reader.reset();

            if (replacingInput)
            {
                ec.clear();
                std::filesystem::remove(request.inputWavPath, ec);
                if (ec)
                {
                    result.message = "Rendered VST3 effect output, but could not remove original WAV: " + ec.message();
                    return result;
                }

                ec.clear();
                std::filesystem::rename(actualOutputPath, request.inputWavPath, ec);
                if (ec)
                {
                    result.message = "Rendered VST3 effect output, but could not replace original WAV: " + ec.message();
                    return result;
                }
            }

            result.success = true;
            result.effectApplied = true;
            result.message = "Processed WAV through track VST3 effect slot " + std::to_string(slotIndex + 1) + ". Plugin: " + effectName
                + "; saved state present: " + std::string(hasSavedEffectState ? "yes" : "no")
                + "; state restored: " + std::string(load.savedStateApplied ? "yes" : "no")
                + "; input: " + request.inputWavPath.string()
                + "; output: " + result.wavPath.string();
            return result;
        }
    }

    VstEffectProcessResult VstInstrumentHost::processWavWithTrackEffectChain(const VstEffectProcessRequest& request)
    {
        VstEffectProcessResult result;
        result.wavPath = request.outputWavPath.empty() ? request.inputWavPath : request.outputWavPath;

        if (request.cancelRequested != nullptr && request.cancelRequested->load())
        {
            result.cancelled = true;
            result.message = "VST3 effect chain processing cancelled before start.";
            return result;
        }

        const auto& effects = request.track.getVstEffects();
        std::vector<std::size_t> activeSlots;

        if (request.effectSlotIndex >= 0)
        {
            const auto requestedSlot = static_cast<std::size_t>(std::min<int>(request.effectSlotIndex, static_cast<int>(mw::core::maxVstEffectSlots) - 1));
            activeSlots.push_back(requestedSlot);
        }
        else
        {
            for (std::size_t slotIndex = 0; slotIndex < mw::core::maxVstEffectSlots; ++slotIndex)
            {
                const auto* slot = effects.slot(slotIndex);
                if (slot != nullptr
                    && effects.slotEnabled(slotIndex)
                    && !slot->plugin.bypassed
                    && slot->plugin.hasPluginIdentity()
                    && !slot->plugin.bundlePath.empty())
                    activeSlots.push_back(slotIndex);
            }
        }

        if (activeSlots.empty())
        {
            std::string copyError;
            result.success = copyAudioFileForUnprocessedEffect(request.inputWavPath, result.wavPath, copyError);
            result.effectApplied = false;
            result.message = result.success
                ? "No enabled VST3 effect slots on this track; WAV left unchanged."
                : "No enabled VST3 effect slots on this track; dry WAV copy failed: " + copyError;
            return result;
        }

        std::filesystem::path currentInputPath = request.inputWavPath;
        std::vector<std::filesystem::path> temporaryChainFiles;
        bool anyApplied = false;
        std::ostringstream chainLog;
        chainLog << "Processed WAV through track VST3 effect chain";

        for (std::size_t i = 0; i < activeSlots.size(); ++i)
        {
            const auto slotIndex = activeSlots[i];
            const bool lastSlot = (i + 1 == activeSlots.size());
            auto slotOutputPath = lastSlot
                ? (request.outputWavPath.empty() ? request.inputWavPath : request.outputWavPath)
                : currentInputPath.parent_path() / (currentInputPath.stem().string() + ".slot" + std::to_string(slotIndex + 1) + ".vstfx.tmp.wav");

            VstEffectProcessRequest slotRequest = request;
            slotRequest.inputWavPath = currentInputPath;
            slotRequest.outputWavPath = slotOutputPath;
            slotRequest.effectSlotIndex = static_cast<int>(slotIndex);

            auto slotResult = processWavWithTrackEffectSlot(slotRequest, slotIndex);
            if (slotResult.cancelled)
                return slotResult;
            if (!slotResult.success)
                return slotResult;

            anyApplied = anyApplied || slotResult.effectApplied;
            chainLog << " -> slot " << (slotIndex + 1);
            currentInputPath = slotResult.wavPath;

            if (!lastSlot)
                temporaryChainFiles.push_back(currentInputPath);
        }

        for (const auto& tempPath : temporaryChainFiles)
        {
            std::error_code ec;
            std::filesystem::remove(tempPath, ec);
        }

        result.success = true;
        result.effectApplied = anyApplied;
        result.wavPath = currentInputPath;
        chainLog << "; input: " << request.inputWavPath.string() << "; output: " << result.wavPath.string();
        result.message = chainLog.str();
        return result;
    }

    VstEffectProcessResult VstInstrumentHost::processWavWithFirstTrackEffect(const VstEffectProcessRequest& request)
    {
        VstEffectProcessRequest firstSlotRequest = request;
        firstSlotRequest.effectSlotIndex = 0;
        return processWavWithTrackEffectChain(firstSlotRequest);
    }

}
