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
}

namespace mw::vst
{
    bool VstInstrumentHost::trackHasVstPlugin(const mw::core::Track& track)
    {
        return track.getInstrument().backendType == mw::core::SampleBackendType::VST3
            && !track.getInstrument().vst3.bundlePath.empty();
    }

    VstLoadResult VstInstrumentHost::loadInstrumentForTrack(const mw::core::Track& track, double sampleRate, int blockSize)
    {
        VstLoadResult result;

        if (!trackHasVstPlugin(track))
        {
            result.message = "Track does not have a VST3 plugin assignment.";
            return result;
        }

        const auto pluginPath = track.getInstrument().vst3.bundlePath;
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

            if (track.getInstrument().vst3.uid.empty()
                || description->createIdentifierString().toStdString() == track.getInstrument().vst3.uid
                || description->name.toStdString() == track.getInstrument().vst3.name)
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

            instance->setPlayConfigDetails(0, 2, sampleRate, blockSize);
            instance->prepareToPlay(sampleRate, blockSize);

            if (!track.getInstrument().vst3.stateBase64.empty())
            {
                juce::MemoryBlock stateData;
                if (stateData.fromBase64Encoding(track.getInstrument().vst3.stateBase64))
                {
                    try
                    {
                        instance->suspendProcessing(true);
                        instance->setStateInformation(stateData.getData(), static_cast<int>(stateData.getSize()));
                        instance->suspendProcessing(false);
                        instance->reset();
                    }
                    catch (...)
                    {
                        instance->suspendProcessing(false);
                        throw;
                    }
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
}
