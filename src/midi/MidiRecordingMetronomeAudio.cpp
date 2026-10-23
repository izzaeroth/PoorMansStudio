#include "midi/MidiRecordingMetronomeAudio.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <limits>

namespace mw::midi
{
    bool createMidiRecordingMetronomeBackingWav(
        const std::filesystem::path& sourceWavPath,
        const std::filesystem::path& outputWavPath,
        const MidiRecordingMetronomeConfig& config,
        double activeStartSeconds,
        double activeEndSeconds,
        std::string& errorMessage)
    {
        errorMessage.clear();
        if (!std::isfinite(activeStartSeconds))
            activeStartSeconds = 0.0;
        if (!std::isfinite(activeEndSeconds))
            activeEndSeconds = std::numeric_limits<double>::infinity();
        activeStartSeconds = std::max(0.0, activeStartSeconds);
        activeEndSeconds = std::max(activeStartSeconds, activeEndSeconds);
        if (!config.enabled)
        {
            errorMessage = "MIDI recording metronome is not enabled.";
            return false;
        }
        if (sourceWavPath.empty() || !std::filesystem::exists(sourceWavPath))
        {
            errorMessage = "Rendered backing WAV does not exist: " + sourceWavPath.string();
            return false;
        }
        if (outputWavPath.empty() || outputWavPath == sourceWavPath)
        {
            errorMessage = "Metronome backing output path is invalid.";
            return false;
        }

        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader(
            formats.createReaderFor(juce::File(sourceWavPath.string())));
        if (!reader || reader->sampleRate <= 0.0 || reader->numChannels == 0)
        {
            errorMessage = "Rendered backing WAV could not be opened for metronome mixing.";
            return false;
        }

        std::error_code ignored;
        std::filesystem::create_directories(outputWavPath.parent_path(), ignored);
        std::filesystem::remove(outputWavPath, ignored);

        juce::File outputFile(outputWavPath.string());
        std::unique_ptr<juce::FileOutputStream> stream(outputFile.createOutputStream());
        if (!stream || !stream->openedOk())
        {
            errorMessage = "Could not create temporary metronome backing WAV: " + outputWavPath.string();
            return false;
        }

        juce::WavAudioFormat wavFormat;
        std::unique_ptr<juce::AudioFormatWriter> writer(
            wavFormat.createWriterFor(
                stream.get(),
                reader->sampleRate,
                reader->numChannels,
                24,
                {},
                0));
        if (!writer)
        {
            errorMessage = "Could not create temporary metronome WAV writer.";
            return false;
        }
        stream.release();

        constexpr int blockSize = 4096;
        juce::AudioBuffer<float> buffer(static_cast<int>(reader->numChannels), blockSize);
        juce::int64 position = 0;
        while (position < reader->lengthInSamples)
        {
            const int frames = static_cast<int>(std::min<juce::int64>(blockSize, reader->lengthInSamples - position));
            buffer.clear();
            if (!reader->read(&buffer, 0, frames, position, true, true))
            {
                errorMessage = "Could not read rendered backing WAV while adding the MIDI recording metronome.";
                return false;
            }

            for (int frame = 0; frame < frames; ++frame)
            {
                const auto absoluteSample = position + frame;
                const double projectSeconds = static_cast<double>(absoluteSample) / reader->sampleRate;
                if (projectSeconds < activeStartSeconds || projectSeconds >= activeEndSeconds)
                    continue;
                const float click = midiRecordingMetronomeSampleAtProjectTime(config, projectSeconds);
                if (click == 0.0f)
                    continue;

                for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
                {
                    const auto mixed = buffer.getSample(channel, frame) + click;
                    buffer.setSample(channel, frame, std::clamp(mixed, -1.0f, 1.0f));
                }
            }

            if (!writer->writeFromAudioSampleBuffer(buffer, 0, frames))
            {
                errorMessage = "Could not write temporary metronome backing WAV.";
                return false;
            }
            position += frames;
        }

        writer.reset();
        return true;
    }
}
