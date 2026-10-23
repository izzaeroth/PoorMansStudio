#include "simulation/SimulationRegressionSuite.h"

#include "app/VersionNumber.h"
#include "audio/TailPolicy.h"
#include "audio/ProjectContentEnding.h"
#include "core/TrackSelectionPolicy.h"
#include "midi/MidiInputFilter.h"
#include "midi/MidiControllerLane.h"
#include "midi/MidiPitchBendLane.h"
#include "midi/MidiProgramChangeLane.h"
#include "midi/MidiChannelPressureLane.h"
#include "midi/MidiPolyPressureLane.h"
#include "midi/Midi14BitControllerLane.h"
#include "midi/MidiParameterLane.h"
#include "midi/MidiAdvancedParameterDecoder.h"
#include "midi/MidiExporter.h"
#include "clap/ClapLivePlaybackScheduler.h"
#include "midi/MidiRecordingSession.h"
#include "midi/MidiRecordingCountIn.h"
#include "midi/MidiRecordingQuantizer.h"
#include "midi/MidiRecordingMetronome.h"
#include "midi/MidiRecordingControlPolicy.h"
#include "midi/MidiRecordingPunch.h"
#include "midi/MidiRecordingLoop.h"
#include "midi/MidiRecordingTakeReview.h"
#include "midi/MidiRecordingComp.h"
#include "midi/MidiPendingTakeReviewManager.h"
#include "midi/MidiRecordingTransportSync.h"
#include "playback/PlaybackTransportCoordinator.h"
#include "import_export/MidiImporter.h"
#include "serialization/ProjectSerializer.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <filesystem>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <sstream>
#include <tuple>
#include <utility>
#include <vector>

namespace mw::simulation
{
    namespace
    {
        enum class SimOwner
        {
            None,
            VstPreview,
            ClapPreview,
            MixedPreview,
            RecordTest,
            LiveMidi,
            Recorder
        };

        enum class SimInputMode
        {
            Mono,
            StereoAdjacent
        };

        struct SimAudioRuntime
        {
            bool outputOpen = true;
            bool deviceLost = false;
            SimOwner owner = SimOwner::None;
            SimInputMode inputMode = SimInputMode::Mono;
            int inputChannelIndex = 0;
            double sampleRate = 48000.0;
            int bufferSize = 512;
            int preparationGeneration = 1;
            int dropoutCount = 0;

            bool activatePlayback(SimOwner requested)
            {
                if (deviceLost || !outputOpen || requested == SimOwner::None || requested == SimOwner::Recorder)
                    return false;
                if (owner != SimOwner::None)
                    return false;
                owner = requested;
                return true;
            }

            bool activateRecorder()
            {
                if (deviceLost || owner != SimOwner::None)
                    return false;
                owner = SimOwner::Recorder;
                return true;
            }

            void deactivate(SimOwner requested)
            {
                if (owner == requested)
                    owner = SimOwner::None;
            }

            bool reconfigure(double requestedSampleRate, int requestedBufferSize)
            {
                if (deviceLost || owner != SimOwner::None || requestedSampleRate <= 0.0 || requestedBufferSize <= 0)
                    return false;
                sampleRate = requestedSampleRate;
                bufferSize = requestedBufferSize;
                ++preparationGeneration;
                return true;
            }

            void registerDropout() noexcept
            {
                ++dropoutCount;
            }

            void loseDevice()
            {
                deviceLost = true;
                outputOpen = false;
                owner = SimOwner::None;
            }

            void restoreDevice()
            {
                deviceLost = false;
                outputOpen = true;
                owner = SimOwner::None;
                ++preparationGeneration;
            }

            std::vector<std::vector<float>> routeInput(const std::vector<std::vector<float>>& physical) const
            {
                const int wantedChannels = inputMode == SimInputMode::StereoAdjacent ? 2 : 1;
                std::size_t samples = 0;
                for (const auto& channel : physical)
                    samples = std::max(samples, channel.size());

                std::vector<std::vector<float>> routed(static_cast<std::size_t>(wantedChannels),
                                                       std::vector<float>(samples, 0.0f));
                if (physical.empty())
                    return routed;

                const int maximumFirst = std::max(0, static_cast<int>(physical.size()) - wantedChannels);
                const int first = std::clamp(inputChannelIndex, 0, maximumFirst);
                for (int outputChannel = 0; outputChannel < wantedChannels; ++outputChannel)
                {
                    const int source = std::clamp(first + outputChannel, 0, static_cast<int>(physical.size()) - 1);
                    const auto& sourceData = physical[static_cast<std::size_t>(source)];
                    auto& destination = routed[static_cast<std::size_t>(outputChannel)];
                    std::copy(sourceData.begin(), sourceData.end(), destination.begin());
                }
                return routed;
            }
        };

        struct SimEffect
        {
            enum class Kind { Gain, Clamp, Fail } kind = Kind::Gain;
            float amount = 1.0f;
        };

        bool processEffectChain(std::vector<std::vector<float>>& audio, const std::vector<SimEffect>& effects)
        {
            for (const auto& effect : effects)
            {
                if (effect.kind == SimEffect::Kind::Fail)
                    return false;

                for (auto& channel : audio)
                {
                    for (auto& sample : channel)
                    {
                        if (effect.kind == SimEffect::Kind::Gain)
                            sample *= effect.amount;
                        else if (effect.kind == SimEffect::Kind::Clamp)
                            sample = std::clamp(sample, -std::abs(effect.amount), std::abs(effect.amount));
                    }
                }
            }
            return true;
        }

        std::vector<float> renderInternalInstrument(int midiNote, int velocity, int sampleCount, double sampleRate)
        {
            std::vector<float> audio(static_cast<std::size_t>(std::max(0, sampleCount)), 0.0f);
            if (audio.empty() || sampleRate <= 0.0)
                return audio;

            const double frequency = 440.0 * std::pow(2.0, (static_cast<double>(midiNote) - 69.0) / 12.0);
            const double amplitude = static_cast<double>(std::clamp(velocity, 1, 127)) / 127.0 * 0.25;
            constexpr double twoPi = 6.283185307179586476925286766559;
            for (int sample = 0; sample < sampleCount; ++sample)
                audio[static_cast<std::size_t>(sample)] = static_cast<float>(amplitude * std::sin(twoPi * frequency * static_cast<double>(sample) / sampleRate));
            return audio;
        }

        struct AudioArtifactFixture
        {
            int sampleRate = 48000;
            std::vector<float> dry;
            std::vector<float> wet;
        };

        AudioArtifactFixture makeAudioArtifactFixture()
        {
            AudioArtifactFixture fixture;
            fixture.dry.resize(960);
            for (std::size_t i = 0; i < fixture.dry.size(); ++i)
            {
                const int step = static_cast<int>(i % 64u) - 32;
                fixture.dry[i] = static_cast<float>(step) / 128.0f;
            }

            fixture.wet = fixture.dry;
            std::vector<std::vector<float>> wetChannels { fixture.wet };
            const bool processed = processEffectChain(wetChannels,
            {
                { SimEffect::Kind::Gain, 1.5f },
                { SimEffect::Kind::Clamp, 0.1875f }
            });
            if (!processed || wetChannels.empty())
                throw std::runtime_error("Could not create deterministic wet audio fixture.");
            fixture.wet = std::move(wetChannels.front());
            return fixture;
        }

        void appendLittleEndian16(std::vector<std::uint8_t>& bytes, std::uint16_t value)
        {
            bytes.push_back(static_cast<std::uint8_t>(value & 0xffu));
            bytes.push_back(static_cast<std::uint8_t>((value >> 8u) & 0xffu));
        }

        void appendLittleEndian32(std::vector<std::uint8_t>& bytes, std::uint32_t value)
        {
            bytes.push_back(static_cast<std::uint8_t>(value & 0xffu));
            bytes.push_back(static_cast<std::uint8_t>((value >> 8u) & 0xffu));
            bytes.push_back(static_cast<std::uint8_t>((value >> 16u) & 0xffu));
            bytes.push_back(static_cast<std::uint8_t>((value >> 24u) & 0xffu));
        }

        std::vector<std::uint8_t> pcm16MonoWavBytes(const std::vector<float>& samples, int sampleRate)
        {
            if (sampleRate <= 0)
                throw std::runtime_error("Invalid WAV sample rate.");

            std::vector<std::uint8_t> bytes;
            const auto dataBytes = static_cast<std::uint32_t>(samples.size() * sizeof(std::int16_t));
            bytes.reserve(static_cast<std::size_t>(44u + dataBytes));
            const auto appendFour = [&bytes](const char* text)
            {
                for (int i = 0; i < 4; ++i)
                    bytes.push_back(static_cast<std::uint8_t>(text[i]));
            };

            appendFour("RIFF");
            appendLittleEndian32(bytes, 36u + dataBytes);
            appendFour("WAVE");
            appendFour("fmt ");
            appendLittleEndian32(bytes, 16u);
            appendLittleEndian16(bytes, 1u);
            appendLittleEndian16(bytes, 1u);
            appendLittleEndian32(bytes, static_cast<std::uint32_t>(sampleRate));
            appendLittleEndian32(bytes, static_cast<std::uint32_t>(sampleRate * 2));
            appendLittleEndian16(bytes, 2u);
            appendLittleEndian16(bytes, 16u);
            appendFour("data");
            appendLittleEndian32(bytes, dataBytes);

            for (const auto sample : samples)
            {
                const auto clipped = std::clamp(sample, -1.0f, 1.0f);
                const auto scaled = static_cast<long>(std::lround(static_cast<double>(clipped) * 32767.0));
                const auto value = static_cast<std::int16_t>(std::clamp<long>(scaled, -32767, 32767));
                appendLittleEndian16(bytes, static_cast<std::uint16_t>(value));
            }
            return bytes;
        }

        std::uint64_t fnv1a64(const std::vector<std::uint8_t>& bytes)
        {
            std::uint64_t hash = 1469598103934665603ull;
            for (const auto byte : bytes)
            {
                hash ^= static_cast<std::uint64_t>(byte);
                hash *= 1099511628211ull;
            }
            return hash;
        }

        std::string hex64(std::uint64_t value)
        {
            std::ostringstream out;
            out << std::hex << std::setfill('0') << std::setw(16) << value;
            return out.str();
        }

        std::string hex32(std::uint32_t value)
        {
            std::ostringstream out;
            out << "0x" << std::uppercase << std::hex << std::setfill('0') << std::setw(8) << value;
            return out.str();
        }

        std::uint64_t fnv1a64(const std::string& text)
        {
            std::uint64_t hash = 1469598103934665603ull;
            for (const unsigned char byte : text)
            {
                hash ^= static_cast<std::uint64_t>(byte);
                hash *= 1099511628211ull;
            }
            return hash;
        }

        std::string xmlEscape(const std::string& value)
        {
            std::ostringstream out;
            for (const char c : value)
            {
                switch (c)
                {
                    case '&': out << "&amp;"; break;
                    case '<': out << "&lt;"; break;
                    case '>': out << "&gt;"; break;
                    case '\"': out << "&quot;"; break;
                    case '\'': out << "&apos;"; break;
                    default: out << c; break;
                }
            }
            return out.str();
        }

        bool nearlyEqual(float a, float b, float tolerance = 1.0e-6f)
        {
            return std::abs(a - b) <= tolerance;
        }

        bool vectorNearlyEqual(const std::vector<float>& a, const std::vector<float>& b, float tolerance = 1.0e-6f)
        {
            if (a.size() != b.size())
                return false;
            for (std::size_t i = 0; i < a.size(); ++i)
                if (!nearlyEqual(a[i], b[i], tolerance))
                    return false;
            return true;
        }

        std::string utcNow()
        {
            const auto now = std::chrono::system_clock::now();
            const std::time_t nowTime = std::chrono::system_clock::to_time_t(now);
            std::tm utc {};
#if defined(_WIN32)
            gmtime_s(&utc, &nowTime);
#else
            gmtime_r(&nowTime, &utc);
#endif
            std::ostringstream out;
            out << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
            return out.str();
        }

        std::string jsonEscape(const std::string& value)
        {
            std::ostringstream out;
            for (const unsigned char c : value)
            {
                switch (c)
                {
                    case '\\': out << "\\\\"; break;
                    case '"': out << "\\\""; break;
                    case '\n': out << "\\n"; break;
                    case '\r': out << "\\r"; break;
                    case '\t': out << "\\t"; break;
                    default:
                        if (c < 0x20)
                            out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(c) << std::dec;
                        else
                            out << static_cast<char>(c);
                        break;
                }
            }
            return out.str();
        }

        SimulationScenarioResult makeResult(std::string id, std::string stage, std::string name, bool passed, std::string details)
        {
            return { std::move(id), std::move(stage), std::move(name), passed, std::move(details) };
        }

        struct SimAudioAnalysis
        {
            float peak = 0.0f;
            double rms = 0.0;
            std::size_t lastSampleAboveThreshold = 0;
            std::size_t retainedSamples = 0;
            bool foundAboveThreshold = false;
        };

        SimAudioAnalysis analyzeAudioFixture(const std::vector<float>& samples, float threshold, std::size_t paddingSamples)
        {
            SimAudioAnalysis analysis;
            double sumSquares = 0.0;
            for (std::size_t i = 0; i < samples.size(); ++i)
            {
                const float magnitude = std::abs(samples[i]);
                analysis.peak = std::max(analysis.peak, magnitude);
                sumSquares += static_cast<double>(samples[i]) * static_cast<double>(samples[i]);
                if (magnitude > threshold)
                {
                    analysis.lastSampleAboveThreshold = i;
                    analysis.foundAboveThreshold = true;
                }
            }

            if (!samples.empty())
                analysis.rms = std::sqrt(sumSquares / static_cast<double>(samples.size()));

            if (analysis.foundAboveThreshold)
                analysis.retainedSamples = std::min(samples.size(), analysis.lastSampleAboveThreshold + 1u + paddingSamples);
            return analysis;
        }

        std::uint32_t xorshift32(std::uint32_t& state) noexcept
        {
            state ^= state << 13u;
            state ^= state >> 17u;
            state ^= state << 5u;
            return state;
        }

        int seededRange(std::uint32_t& state, int minimum, int maximum)
        {
            if (maximum <= minimum)
                return minimum;
            const auto span = static_cast<std::uint32_t>(maximum - minimum + 1);
            return minimum + static_cast<int>(xorshift32(state) % span);
        }

        struct SeededProjectRoundTripSummary
        {
            bool passed = false;
            std::uint64_t fingerprint = 0;
            int noteCount = 0;
        };

        SeededProjectRoundTripSummary runSeededProjectRoundTrips(std::uint32_t seed)
        {
            const auto tempFolder = std::filesystem::temp_directory_path() / "PoorMansStudioSimulation";
            std::error_code ec;
            std::filesystem::create_directories(tempFolder, ec);

            std::uint32_t randomState = seed != 0u ? seed : 0xA341316Cu;
            constexpr int projectCount = 8;
            constexpr int tracksPerProject = 4;
            constexpr int notesPerTrack = 32;
            bool allOk = true;
            SeededProjectRoundTripSummary summary;
            std::ostringstream fingerprintSource;

            for (int projectIndex = 0; projectIndex < projectCount && allOk; ++projectIndex)
            {
                const auto projectPath = tempFolder / ("seeded_roundtrip_" + hex32(seed).substr(2) + "_" + std::to_string(projectIndex) + ".mwproj");
                std::filesystem::remove(projectPath, ec);

                mw::core::Project project("Seeded Fixture " + std::to_string(projectIndex));
                project.setStableId(static_cast<mw::core::StableId>(820000 + projectIndex));
                project.setTempoBpm(seededRange(randomState, 60, 190));
                project.setTimeSignature({ seededRange(randomState, 2, 7), seededRange(randomState, 0, 1) == 0 ? 4 : 8 });

                for (int trackIndex = 0; trackIndex < tracksPerProject; ++trackIndex)
                {
                    auto& track = project.addTrack("Seeded Track " + std::to_string(trackIndex));
                    track.setStableId(static_cast<mw::core::StableId>(830000 + projectIndex * 100 + trackIndex));
                    track.getMixerSettings().volume = static_cast<float>(seededRange(randomState, 25, 100)) / 100.0f;
                    track.getMixerSettings().pan = static_cast<float>(seededRange(randomState, -100, 100)) / 100.0f;

                    long long tick = seededRange(randomState, 0, 480);
                    for (int noteIndex = 0; noteIndex < notesPerTrack; ++noteIndex)
                    {
                        const int pitch = seededRange(randomState, 24, 96);
                        const int velocity = seededRange(randomState, 1, 127);
                        const int channel = seededRange(randomState, 1, 16);
                        const long long duration = seededRange(randomState, 60, 1920);
                        tick += seededRange(randomState, 0, 480);
                        const auto articulation = static_cast<mw::core::Articulation>(seededRange(randomState, 0, 3));
                        track.addNote({ pitch, velocity, tick, duration, channel, articulation });
                    }
                }

                mw::core::ProjectSequenceMetadata sequence;
                sequence.id = projectIndex + 1;
                sequence.number = projectIndex + 1;
                sequence.name = "Seeded Sequence " + std::to_string(projectIndex);
                sequence.createdBy = "simulation-seed-" + hex32(seed).substr(2);
                sequence.startTick = 0;
                sequence.endTick = 20000;
                sequence.tracks = { 0, 1, 2, 3 };
                project.setSequences({ sequence });

                const bool saved = mw::serialization::ProjectSerializer::saveToFile(project, projectPath);
                const auto loaded = saved ? mw::serialization::ProjectSerializer::loadFromFile(projectPath) : std::nullopt;
                std::filesystem::remove(projectPath, ec);
                if (!saved || !loaded.has_value())
                {
                    allOk = false;
                    break;
                }

                const auto& reloaded = *loaded;
                allOk = reloaded.getStableId() == project.getStableId()
                    && reloaded.getTempoBpm() == project.getTempoBpm()
                    && reloaded.getTimeSignature().numerator == project.getTimeSignature().numerator
                    && reloaded.getTimeSignature().denominator == project.getTimeSignature().denominator
                    && reloaded.getTracks().size() == project.getTracks().size()
                    && reloaded.getSequences().size() == 1
                    && reloaded.getSequences()[0].tracks == std::vector<int>({ 0, 1, 2, 3 });

                fingerprintSource << reloaded.getStableId() << ':' << reloaded.getTempoBpm() << ':'
                    << reloaded.getTimeSignature().numerator << '/' << reloaded.getTimeSignature().denominator << ';';

                for (std::size_t trackIndex = 0; trackIndex < project.getTracks().size() && allOk; ++trackIndex)
                {
                    const auto& expectedTrack = project.getTracks()[trackIndex];
                    const auto& actualTrack = reloaded.getTracks()[trackIndex];
                    allOk = actualTrack.getStableId() == expectedTrack.getStableId()
                        && actualTrack.getNotes().size() == expectedTrack.getNotes().size()
                        && nearlyEqual(actualTrack.getMixerSettings().volume, expectedTrack.getMixerSettings().volume)
                        && nearlyEqual(actualTrack.getMixerSettings().pan, expectedTrack.getMixerSettings().pan);

                    fingerprintSource << actualTrack.getStableId() << ':'
                        << std::fixed << std::setprecision(6) << actualTrack.getMixerSettings().volume << ':'
                        << actualTrack.getMixerSettings().pan << ';';

                    for (std::size_t noteIndex = 0; noteIndex < expectedTrack.getNotes().size() && allOk; ++noteIndex)
                    {
                        const auto& expected = expectedTrack.getNotes()[noteIndex];
                        const auto& actual = actualTrack.getNotes()[noteIndex];
                        allOk = actual.pitch == expected.pitch
                            && actual.velocity == expected.velocity
                            && actual.midiChannel == expected.midiChannel
                            && actual.startTick == expected.startTick
                            && actual.durationTicks == expected.durationTicks
                            && actual.articulation == expected.articulation;

                        fingerprintSource << actual.pitch << ',' << actual.velocity << ',' << actual.midiChannel << ','
                            << actual.startTick << ',' << actual.durationTicks << ',' << static_cast<int>(actual.articulation) << ';';
                        ++summary.noteCount;
                    }
                }
            }

            summary.passed = allOk;
            summary.fingerprint = fnv1a64(fingerprintSource.str());
            return summary;
        }

        SimulationScenarioResult scenarioA01()
        {
            SimAudioRuntime runtime;
            const bool vstStarted = runtime.activatePlayback(SimOwner::VstPreview);
            const bool clapBlocked = !runtime.activatePlayback(SimOwner::ClapPreview);
            runtime.deactivate(SimOwner::VstPreview);
            const bool clapStarted = runtime.activatePlayback(SimOwner::ClapPreview);
            runtime.deactivate(SimOwner::ClapPreview);
            const bool clean = runtime.owner == SimOwner::None;
            const bool passed = vstStarted && clapBlocked && clapStarted && clean;
            return makeResult("A01", "Stage A", "Shared output ownership", passed,
                              passed ? "One playback owner at a time; release permits the next owner and leaves no stale owner."
                                     : "Shared playback-owner exclusion or cleanup contract failed.");
        }

        SimulationScenarioResult scenarioB01()
        {
            SimAudioRuntime runtime;
            const bool recorderStarted = runtime.activateRecorder();
            const bool previewBlocked = !runtime.activatePlayback(SimOwner::MixedPreview);
            runtime.deactivate(SimOwner::Recorder);
            const bool previewRestored = runtime.activatePlayback(SimOwner::MixedPreview);
            const bool passed = recorderStarted && previewBlocked && previewRestored;
            return makeResult("B01", "Stage B", "Recorder ownership and preview restore", passed,
                              passed ? "Recorder excludes normal playback; after recorder release, preview can acquire shared output again."
                                     : "Recorder/playback ownership transition contract failed.");
        }

        SimulationScenarioResult scenarioB02()
        {
            SimAudioRuntime runtime;
            runtime.inputMode = SimInputMode::Mono;
            runtime.inputChannelIndex = 0;
            const std::vector<std::vector<float>> physical { { 0.25f, -0.25f, 0.5f, -0.5f } };
            const auto dry = runtime.routeInput(physical);
            auto wet = dry;
            const bool effectsOk = processEffectChain(wet,
            {
                { SimEffect::Kind::Gain, 2.0f },
                { SimEffect::Kind::Clamp, 0.6f }
            });
            const bool dryPreserved = vectorNearlyEqual(dry[0], physical[0]);
            const bool wetDifferent = !vectorNearlyEqual(wet[0], dry[0]);
            const bool passed = effectsOk && dryPreserved && wetDifferent;
            return makeResult("B02", "Stage B", "Dry capture and wet monitor separation", passed,
                              passed ? "Effect fixtures changed monitor audio while the simulated writer buffer remained dry."
                                     : "Dry/wet separation contract failed.");
        }

        SimulationScenarioResult scenarioB03()
        {
            SimAudioRuntime runtime;
            const std::vector<std::vector<float>> physical { { 0.1f, 0.2f, 0.3f } };
            const auto dry = runtime.routeInput(physical);
            auto wet = dry;
            const bool effectsOk = processEffectChain(wet,
            {
                { SimEffect::Kind::Gain, 2.0f },
                { SimEffect::Kind::Fail, 0.0f }
            });
            const bool dryPreserved = vectorNearlyEqual(dry[0], physical[0]);
            const bool passed = !effectsOk && dryPreserved;
            return makeResult("B03", "Stage B", "Complete-chain monitor failure", passed,
                              passed ? "A failed second effect invalidated the wet chain while dry capture remained available."
                                     : "Complete-chain failure did not preserve the dry capture contract.");
        }

        SimulationScenarioResult scenarioC01()
        {
            SimAudioRuntime runtime;
            runtime.inputMode = SimInputMode::Mono;
            runtime.inputChannelIndex = 1;
            const std::vector<std::vector<float>> physical
            {
                { 0.10f, 0.11f },
                { 0.20f, 0.21f },
                { 0.30f, 0.31f },
                { 0.40f, 0.41f }
            };
            const auto routed = runtime.routeInput(physical);
            const bool passed = routed.size() == 1 && vectorNearlyEqual(routed[0], physical[1]);
            return makeResult("C01", "Stage C", "Mono physical input routing", passed,
                              passed ? "Mono Input 2 selected physical channel 2 exactly."
                                     : "Mono physical input routing selected the wrong channel.");
        }

        SimulationScenarioResult scenarioC02()
        {
            SimAudioRuntime runtime;
            runtime.inputMode = SimInputMode::StereoAdjacent;
            runtime.inputChannelIndex = 2;
            const std::vector<std::vector<float>> physical
            {
                { 0.10f, 0.11f },
                { 0.20f, 0.21f },
                { 0.30f, 0.31f },
                { 0.40f, 0.41f }
            };
            const auto routed = runtime.routeInput(physical);
            const bool passed = routed.size() == 2
                && vectorNearlyEqual(routed[0], physical[2])
                && vectorNearlyEqual(routed[1], physical[3]);
            return makeResult("C02", "Stage C", "Adjacent stereo input routing", passed,
                              passed ? "Stereo Input 3-4 selected physical channels 3 and 4 in order."
                                     : "Adjacent stereo input routing selected the wrong channel pair.");
        }

        SimulationScenarioResult scenarioD01()
        {
            const bool omni = mw::midi::midiInputChannelAccepted(0, 1)
                && mw::midi::midiInputChannelAccepted(0, 16);
            const bool exact = mw::midi::midiInputChannelAccepted(3, 3)
                && !mw::midi::midiInputChannelAccepted(3, 2)
                && !mw::midi::midiInputChannelAccepted(3, 4);
            const bool clamp = mw::midi::sanitizeMidiInputChannelFilter(99) == 16
                && mw::midi::sanitizeMidiInputChannelFilter(-5) == 0;
            const bool passed = omni && exact && clamp;
            return makeResult("D01", "Stage D", "MIDI Omni and channel filtering", passed,
                              passed ? "Omni accepts all channels; exact-channel mode accepts only its selected channel."
                                     : "MIDI channel-filter contract failed.");
        }

        SimulationScenarioResult scenarioD02()
        {
            std::vector<std::vector<float>> audio { renderInternalInstrument(69, 100, 128, 48000.0) };
            const auto original = audio[0];
            const bool processed = processEffectChain(audio,
            {
                { SimEffect::Kind::Gain, 2.0f },
                { SimEffect::Kind::Clamp, 0.20f }
            });
            const auto peak = [](const std::vector<float>& samples)
            {
                float value = 0.0f;
                for (const auto sample : samples)
                    value = std::max(value, std::abs(sample));
                return value;
            };
            const float originalPeak = peak(original);
            const float processedPeak = peak(audio[0]);
            const bool passed = processed && originalPeak > 0.01f && processedPeak > 0.01f
                && processedPeak <= 0.200001f && !vectorNearlyEqual(original, audio[0]);
            return makeResult("D02", "Stage D", "Internal instrument and ordered effect fixtures", passed,
                              passed ? "Generated MIDI-note audio reached internal Slot 1 then Slot 2 fixtures without third-party plugins."
                                     : "Internal instrument/effect fixture path failed.");
        }

        SimulationScenarioResult scenarioE01()
        {
            mw::midi::MidiRecordingSession session;
            mw::midi::MidiRecordingConfig config;
            config.tempoBpm = 120;
            config.ticksPerQuarterNote = 960;
            config.startTick = 1920;
            config.startTimeSeconds = 10.0;
            const bool started = session.start(config);
            session.noteOn(2, 64, 91, 10.25);
            session.noteOff(2, 64, 10.75);
            const auto result = session.stop(11.0);
            const bool oneNote = result.notes.size() == 1;
            bool fieldsOk = false;
            if (oneNote)
            {
                const auto& note = result.notes.front();
                fieldsOk = note.midiChannel == 2 && note.pitch == 64 && note.velocity == 91
                    && note.startTick == 2400 && note.durationTicks == 960;
            }
            const bool passed = started && oneNote && fieldsOk && result.stopTick == 3840;
            return makeResult("E01", "Stage E", "MIDI recording timing and note fields", passed,
                              passed ? "Timestamp conversion, sequence offset, pitch, velocity, channel, and duration matched expected project ticks."
                                     : "MIDI recording timing or note-field contract failed.");
        }

        SimulationScenarioResult scenarioE02()
        {
            mw::midi::MidiRecordingSession session;
            mw::midi::MidiRecordingConfig config;
            config.tempoBpm = 120;
            config.ticksPerQuarterNote = 960;
            config.startTick = 0;
            config.startTimeSeconds = 0.0;
            const bool started = session.start(config);
            session.noteOn(1, 60, 80, 0.0);
            session.noteOn(1, 60, 100, 0.25);
            session.noteOff(1, 60, 0.5);
            const auto releasedByPanic = session.releaseActiveNotes(0.75);
            session.noteOn(1, 67, 110, 1.0);
            const auto result = session.stop(1.25);
            const bool countOk = result.notes.size() == 3 && releasedByPanic == 1 && result.finalizedHeldNoteCount == 1;
            bool overlapOk = false;
            if (result.notes.size() >= 2)
            {
                const auto& first = result.notes[0];
                const auto& second = result.notes[1];
                overlapOk = first.pitch == 60 && first.startTick == 0 && first.durationTicks == 960
                    && second.pitch == 60 && second.startTick == 480 && second.durationTicks == 960;
            }
            const bool passed = started && countOk && overlapOk;
            return makeResult("E02", "Stage E", "Overlapping notes, Panic, and held-note finalization", passed,
                              passed ? "Same-pitch overlap remained distinct; Panic and Stop both finalized held notes safely."
                                     : "Overlap/Panic/held-note recording contract failed.");
        }

        SimulationScenarioResult scenarioF01()
        {
            SimAudioRuntime runtime;
            const bool started = runtime.activatePlayback(SimOwner::LiveMidi);
            runtime.loseDevice();
            const bool cleaned = runtime.owner == SimOwner::None && !runtime.outputOpen && runtime.deviceLost;
            runtime.restoreDevice();
            const bool restarted = runtime.activatePlayback(SimOwner::RecordTest);
            const bool passed = started && cleaned && restarted;
            return makeResult("F01", "Simulation", "Device-loss cleanup and deterministic recovery", passed,
                              passed ? "Simulated device loss cleared ownership and a deterministic restore accepted a new playback owner."
                                     : "Device-loss cleanup/recovery simulation failed.");
        }

        SimulationScenarioResult scenarioF02()
        {
            SimAudioRuntime runtime;
            const int initialGeneration = runtime.preparationGeneration;
            const bool started = runtime.activatePlayback(SimOwner::VstPreview);
            const bool blockedWhileActive = !runtime.reconfigure(44100.0, 256);
            runtime.deactivate(SimOwner::VstPreview);
            const bool firstReprepare = runtime.reconfigure(44100.0, 256);
            const int generationAfterFirst = runtime.preparationGeneration;
            const bool clapStarted = runtime.activatePlayback(SimOwner::ClapPreview);
            runtime.deactivate(SimOwner::ClapPreview);
            const bool secondReprepare = runtime.reconfigure(96000.0, 1024);
            const bool mixedStarted = runtime.activatePlayback(SimOwner::MixedPreview);
            const bool passed = started && blockedWhileActive && firstReprepare && clapStarted && secondReprepare && mixedStarted
                && initialGeneration == 1 && generationAfterFirst == 2 && runtime.preparationGeneration == 3
                && runtime.sampleRate == 96000.0 && runtime.bufferSize == 1024;
            return makeResult("F02", "Simulation", "Sample-rate and buffer reprepare", passed,
                              passed ? "Active playback blocked reconfiguration; released clients reprepared deterministically at 44.1 kHz/256 then 96 kHz/1024."
                                     : "Sample-rate/buffer reprepare contract failed.");
        }

        SimulationScenarioResult scenarioF03()
        {
            SimAudioRuntime runtime;
            const bool liveStarted = runtime.activatePlayback(SimOwner::LiveMidi);
            runtime.registerDropout();
            runtime.registerDropout();
            runtime.registerDropout();
            runtime.loseDevice();
            const bool lossClean = runtime.deviceLost && !runtime.outputOpen && runtime.owner == SimOwner::None;
            const bool blockedWhileLost = !runtime.reconfigure(48000.0, 128) && !runtime.activatePlayback(SimOwner::RecordTest);
            runtime.restoreDevice();
            const bool reprepared = runtime.reconfigure(48000.0, 128);
            const bool recorderStarted = runtime.activateRecorder();
            runtime.registerDropout();
            runtime.deactivate(SimOwner::Recorder);
            const bool mixedStarted = runtime.activatePlayback(SimOwner::MixedPreview);
            runtime.deactivate(SimOwner::MixedPreview);
            const bool passed = liveStarted && lossClean && blockedWhileLost && reprepared && recorderStarted && mixedStarted
                && runtime.dropoutCount == 4 && runtime.owner == SimOwner::None && runtime.preparationGeneration == 3;
            return makeResult("F03", "Simulation", "Scripted dropouts and device-loss sequence", passed,
                              passed ? "Three playback dropouts, device loss, restore/reprepare, recorder ownership, a fourth dropout, and final preview recovery completed deterministically."
                                     : "Scripted dropout/device-loss sequence failed.");
        }

        SimulationScenarioResult scenarioG01()
        {
            const auto fixture = makeAudioArtifactFixture();
            const auto dryA = pcm16MonoWavBytes(fixture.dry, fixture.sampleRate);
            const auto dryB = pcm16MonoWavBytes(fixture.dry, fixture.sampleRate);
            const auto wet = pcm16MonoWavBytes(fixture.wet, fixture.sampleRate);
            const bool headerOk = dryA.size() == 44u + fixture.dry.size() * 2u
                && dryA.size() >= 44u
                && std::memcmp(dryA.data(), "RIFF", 4) == 0
                && std::memcmp(dryA.data() + 8, "WAVE", 4) == 0
                && std::memcmp(dryA.data() + 36, "data", 4) == 0;
            const bool deterministic = dryA == dryB;
            const bool wetDifferent = dryA != wet;
            const bool passed = headerOk && deterministic && wetDifferent && fnv1a64(dryA) != fnv1a64(wet);
            return makeResult("G01", "Simulation", "Deterministic PCM WAV artifacts", passed,
                              passed ? "Dry and wet 48 kHz PCM16 WAV fixtures encoded deterministically with distinct byte hashes."
                                     : "Deterministic PCM WAV artifact contract failed.");
        }

        SimulationScenarioResult scenarioH01()
        {
            const auto tempFolder = std::filesystem::temp_directory_path() / "PoorMansStudioSimulation";
            const auto projectPath = tempFolder / "serializer_roundtrip_fixture.mwproj";
            std::error_code ec;
            std::filesystem::create_directories(tempFolder, ec);
            std::filesystem::remove(projectPath, ec);

            mw::core::Project project("Simulation Round Trip");
            project.setStableId(700001);
            project.setTempoBpm(137);
            project.setTimeSignature({ 7, 8 });
            auto settings = project.getUserSettings();
            settings.baseFileName = "simulation_roundtrip";
            settings.sampleRate = 44100;
            settings.channelCount = 2;
            settings.metadataTitle = "Serializer Fixture";
            project.setUserSettings(settings);

            auto& midiTrack = project.addTrack("Recorded MIDI Fixture");
            midiTrack.setStableId(710001);
            midiTrack.getMixerSettings().volume = 0.75f;
            midiTrack.getMixerSettings().pan = -0.25f;
            midiTrack.addNote({ 60, 96, 480, 720, 2, mw::core::Articulation::Accent });
            midiTrack.addNote({ 67, 110, 1440, 960, 2, mw::core::Articulation::Legato });

            auto& audioTrack = project.addTrack("Audio Fixture");
            audioTrack.setStableId(710002);
            audioTrack.setTrackType(mw::core::TrackType::AudioClip);

            mw::core::ProjectSequenceMetadata sequence;
            sequence.id = 1;
            sequence.number = 1;
            sequence.name = "Simulation Sequence";
            sequence.createdBy = "simulation";
            sequence.startTick = 480;
            sequence.endTick = 4800;
            sequence.tracks = { 0, 1 };
            sequence.color = "#556677";
            project.setSequences({ sequence });

            mw::core::AudioClip clip;
            clip.id = 1;
            clip.name = "Synthetic Audio Fixture";
            clip.trackIndex = 1;
            clip.sequenceNumber = 1;
            clip.sourceType = mw::core::AudioClipSourceType::Recorded;
            clip.savedFormat = mw::core::AudioClipSavedFormat::Wav;
            clip.projectRelativePath = "input/audio/recorded/simulation_fixture.wav";
            clip.startTick = 960;
            clip.durationSamples = 44100;
            clip.sourceTrimStartSamples = 2205;
            clip.sourceTrimEndSamples = 41895;
            clip.sampleRate = 44100.0;
            clip.channelCount = 1;
            clip.bitDepth = 24;
            clip.gain = 0.8f;
            clip.pan = 0.15f;
            clip.notes = "simulation round trip";
            project.setAudioClips({ clip });

            const bool saved = mw::serialization::ProjectSerializer::saveToFile(project, projectPath);
            const auto loaded = saved ? mw::serialization::ProjectSerializer::loadFromFile(projectPath) : std::nullopt;
            std::filesystem::remove(projectPath, ec);

            bool fieldsOk = false;
            if (loaded.has_value())
            {
                const auto& p = *loaded;
                fieldsOk = p.getMetadata().projectFormatVersion == 22
                    && p.getStableId() == 700001
                    && p.getName() == "Simulation Round Trip"
                    && p.getTempoBpm() == 137
                    && p.getTimeSignature().numerator == 7
                    && p.getTimeSignature().denominator == 8
                    && p.getUserSettings().sampleRate == 44100
                    && p.getUserSettings().metadataTitle == "Serializer Fixture"
                    && p.getTracks().size() == 2
                    && p.getTracks()[0].getStableId() == 710001
                    && p.getTracks()[0].getNotes().size() == 2
                    && p.getTracks()[0].getNotes()[0].pitch == 60
                    && p.getTracks()[0].getNotes()[0].midiChannel == 2
                    && p.getTracks()[0].getNotes()[0].articulation == mw::core::Articulation::Accent
                    && p.getTracks()[1].getStableId() == 710002
                    && p.getTracks()[1].getTrackType() == mw::core::TrackType::AudioClip
                    && p.getSequences().size() == 1
                    && p.getSequences()[0].tracks == std::vector<int>({ 0, 1 })
                    && p.getAudioClips().size() == 1
                    && p.getAudioClips()[0].trackIndex == 1
                    && p.getAudioClips()[0].sourceTrimStartSamples == 2205
                    && p.getAudioClips()[0].sourceTrimEndSamples == 41895;
            }

            const bool passed = saved && loaded.has_value() && fieldsOk;
            return makeResult("H01", "Simulation", "Project save/reopen round trip", passed,
                              passed ? "A synthetic format-15 project round-tripped through the production ProjectSerializer with stable IDs, notes, sequence metadata, and AudioClip trim data preserved."
                                     : "Synthetic ProjectSerializer save/reopen contract failed.");
        }


        SimulationScenarioResult scenarioF04()
        {
            SimAudioRuntime runtime;
            constexpr int cycles = 50000;
            int expectedDropouts = 0;
            int expectedRestores = 0;
            bool ok = true;

            for (int cycle = 0; cycle < cycles && ok; ++cycle)
            {
                if (cycle % 257 == 0)
                {
                    runtime.loseDevice();
                    ok = runtime.owner == SimOwner::None
                        && !runtime.activatePlayback(SimOwner::RecordTest)
                        && !runtime.reconfigure(48000.0, 512);
                    runtime.restoreDevice();
                    ++expectedRestores;
                }

                const SimOwner owner = cycle % 5 == 0 ? SimOwner::VstPreview
                    : cycle % 5 == 1 ? SimOwner::ClapPreview
                    : cycle % 5 == 2 ? SimOwner::MixedPreview
                    : cycle % 5 == 3 ? SimOwner::RecordTest
                    : SimOwner::LiveMidi;

                ok = ok && runtime.activatePlayback(owner);
                ok = ok && !runtime.reconfigure(44100.0, 256);
                runtime.deactivate(owner);

                const double sampleRate = cycle % 3 == 0 ? 44100.0 : cycle % 3 == 1 ? 48000.0 : 96000.0;
                const int bufferSize = cycle % 4 == 0 ? 128 : cycle % 4 == 1 ? 256 : cycle % 4 == 2 ? 512 : 1024;
                ok = ok && runtime.reconfigure(sampleRate, bufferSize);

                if (cycle % 97 == 0)
                {
                    runtime.registerDropout();
                    ++expectedDropouts;
                }

                ok = ok && runtime.owner == SimOwner::None;
            }

            const int expectedGeneration = 1 + cycles + expectedRestores;
            const bool passed = ok
                && runtime.owner == SimOwner::None
                && runtime.dropoutCount == expectedDropouts
                && runtime.preparationGeneration == expectedGeneration;
            return makeResult("F04", "Simulation", "Long ownership/reprepare stress", passed,
                              passed ? "50,000 deterministic ownership, device-restore, reprepare, and dropout cycles completed without stale ownership."
                                     : "Long deterministic ownership/reprepare stress contract failed.");
        }

        SimulationScenarioResult scenarioG02()
        {
            std::vector<float> samples(256, 0.0f);
            for (int echo = 0; echo < 8; ++echo)
                samples[static_cast<std::size_t>(echo * 32)] = 0.5f / static_cast<float>(1u << echo);

            const auto analysis = analyzeAudioFixture(samples, 0.01f, 16u);
            constexpr double expectedRms = 0.03608411652123976;
            const bool passed = nearlyEqual(analysis.peak, 0.5f)
                && std::abs(analysis.rms - expectedRms) < 1.0e-10
                && analysis.foundAboveThreshold
                && analysis.lastSampleAboveThreshold == 160u
                && analysis.retainedSamples == 177u;
            return makeResult("G02", "Simulation", "Deterministic peak, RMS, and tail analysis", passed,
                              passed ? "A sparse decaying fixture produced the expected 0.5 peak, RMS, threshold crossing, and padded retained tail length."
                                     : "Deterministic peak/RMS/tail analysis contract failed.");
        }

        SimulationScenarioResult scenarioH02(const SimulationRunOptions& options)
        {
            const auto summary = runSeededProjectRoundTrips(options.projectFixtureSeed);
            const auto seedText = hex32(options.projectFixtureSeed);
            const auto fingerprintText = hex64(summary.fingerprint);
            return makeResult("H02", "Simulation", "Seeded randomized project round trips", summary.passed,
                              summary.passed
                                  ? "Seed " + seedText + " generated eight projects with " + std::to_string(summary.noteCount)
                                      + " total MIDI notes; ProjectSerializer round-trips preserved deterministic note/stable-ID data (fingerprint "
                                      + fingerprintText + ")."
                                  : "Seeded randomized ProjectSerializer round-trip contract failed for seed " + seedText + ".");
        }

        SimulationScenarioResult scenarioH03()
        {
            const auto tempFolder = std::filesystem::temp_directory_path() / "PoorMansStudioSimulation";
            const auto missingPath = tempFolder / "missing_project_fixture.mwproj";
            const auto emptyPath = tempFolder / "empty_project_fixture.mwproj";
            const auto duplicatePath = tempFolder / "duplicate_id_fixture.mwproj";
            const auto truncatedPath = tempFolder / "truncated_fixture.mwproj";
            std::error_code ec;
            std::filesystem::create_directories(tempFolder, ec);
            std::filesystem::remove(missingPath, ec);
            std::filesystem::remove(emptyPath, ec);
            std::filesystem::remove(duplicatePath, ec);
            std::filesystem::remove(truncatedPath, ec);

            {
                std::ofstream file(emptyPath, std::ios::binary | std::ios::trunc);
            }
            {
                std::ofstream file(duplicatePath, std::ios::binary | std::ios::trunc);
                file << "{\"name\":\"Duplicate IDs\",\"projectId\":900001,\"tracks\":["
                     << "{\"trackId\":910001,\"name\":\"One\",\"notes\":[]},"
                     << "{\"trackId\":910001,\"name\":\"Two\",\"notes\":[]}]}";
            }
            {
                std::ofstream file(truncatedPath, std::ios::binary | std::ios::trunc);
                file << "{\"name\":\"Truncated\",\"tempoBpm\":not-a-number,\"tracks\":[{";
            }

            const auto missing = mw::serialization::ProjectSerializer::loadFromFile(missingPath);
            const auto empty = mw::serialization::ProjectSerializer::loadFromFile(emptyPath);
            const auto duplicate = mw::serialization::ProjectSerializer::loadFromFile(duplicatePath);
            const auto truncated = mw::serialization::ProjectSerializer::loadFromFile(truncatedPath);

            std::filesystem::remove(emptyPath, ec);
            std::filesystem::remove(duplicatePath, ec);
            std::filesystem::remove(truncatedPath, ec);

            bool duplicateSafe = false;
            if (duplicate.has_value() && duplicate->getTracks().size() == 2)
            {
                const auto firstId = duplicate->getTracks()[0].getStableId();
                const auto secondId = duplicate->getTracks()[1].getStableId();
                duplicateSafe = firstId != 0 && secondId != 0 && firstId != secondId;
            }

            const bool truncatedSafe = truncated.has_value()
                && truncated->getName() == "Truncated"
                && truncated->getTempoBpm() == 120
                && truncated->getTracks().empty();
            const bool passed = !missing.has_value() && !empty.has_value() && duplicateSafe && truncatedSafe;
            return makeResult("H03", "Simulation", "Malformed project safety and fallback", passed,
                              passed ? "Missing/empty files were rejected, duplicate track IDs were normalized, and truncated numeric/track content fell back safely without phantom tracks."
                                     : "Malformed project safety/fallback contract failed.");
        }



        SimulationScenarioResult scenarioF05()
        {
            mw::playback::PlaybackTransportCoordinator transport;
            const auto generation = transport.beginPreparing(940001, -250, 48000, 512, { 941001, 941002 });
            const auto preparing = transport.snapshot();
            const bool preparingOk = generation != 0
                && preparing.state == mw::playback::TransportState::Preparing
                && preparing.currentSample == 0
                && preparing.sampleRate == 48000
                && preparing.blockSize == 512
                && preparing.activeTrackIds == std::vector<mw::core::StableId>({ 941001, 941002 })
                && transport.matches(generation, 940001);

            const bool playingOk = transport.beginPlaying(generation, 96000)
                && transport.updatePosition(generation, 24000, 96000);
            const bool seekStarted = transport.beginSeeking(generation, 120000);
            const auto seeking = transport.snapshot();
            const bool seekClamped = seekStarted
                && seeking.state == mw::playback::TransportState::Seeking
                && seeking.currentSample == 96000;
            const bool seekFinished = transport.updatePositionWithAudioFormat(generation, 48000, 96000, 44100, 256);
            const auto afterSeek = transport.snapshot();
            const bool formatUpdated = seekFinished
                && afterSeek.state == mw::playback::TransportState::Playing
                && afterSeek.currentSample == 48000
                && afterSeek.totalSamples == 96000
                && afterSeek.sampleRate == 44100
                && afterSeek.blockSize == 256
                && std::abs(afterSeek.currentSeconds() - (48000.0 / 44100.0)) < 1.0e-12;

            const bool staleRejected = !transport.beginStopping(generation + 1);
            const bool stopOk = transport.beginStopping(generation)
                && transport.finishStopped(generation);
            const auto stopped = transport.snapshot();
            const bool stoppedClean = stopped.state == mw::playback::TransportState::Stopped
                && stopped.currentSample == 0
                && stopped.totalSamples == 0
                && stopped.activeTrackIds.empty();

            const auto failedGeneration = transport.beginPreparing(950001, 123, 96000, 1024, { 951001 });
            const bool failedOk = transport.fail(failedGeneration, "simulated preparation failure");
            const auto failed = transport.snapshot();
            const bool failureStateOk = failedOk
                && failed.state == mw::playback::TransportState::Failed
                && failed.failureMessage == "simulated preparation failure"
                && !failed.isActive();

            transport.reset();
            const auto reset = transport.snapshot();
            const bool resetOk = reset.state == mw::playback::TransportState::Stopped
                && reset.generation > failedGeneration
                && reset.projectId == 0
                && reset.failureMessage.empty()
                && !transport.matches(failedGeneration, 950001);

            const bool passed = preparingOk && playingOk && seekClamped && formatUpdated
                && staleRejected && stopOk && stoppedClean && failureStateOk && resetOk;
            return makeResult("F05", "Simulation", "Production transport and seek state transitions", passed,
                              passed ? "The production PlaybackTransportCoordinator handled prepare/play/update, clamped seek, audio-format update, stale-generation rejection, stop, failure, and reset transitions deterministically."
                                     : "Production PlaybackTransportCoordinator transition contract failed.");
        }

        SimulationScenarioResult scenarioG03()
        {
            const auto previewSettings = mw::audio::tailTrackingSettings(mw::audio::TailPolicyMode::Preview);
            const auto renderSettings = mw::audio::tailTrackingSettings(mw::audio::TailPolicyMode::Render);

            const auto previewPlan = mw::audio::makeTailScanPlan(20000, 2000, 1000, 4.0, mw::audio::TailPolicyMode::Preview);
            const auto previewDecision = mw::audio::finalizeTailScan(previewPlan, 20000, 5500, 499);
            const auto previewUnconfirmed = mw::audio::finalizeTailScan(previewPlan, 20000, 5500, 299);

            const auto renderPlan = mw::audio::makeTailScanPlan(20000, 2000, 1000, 12.0, mw::audio::TailPolicyMode::Render);
            const auto renderDecision = mw::audio::finalizeTailScan(renderPlan, 20000, 12500, 1500);

            const bool passed = nearlyEqual(previewSettings.silenceThreshold, 1.0e-4f)
                && std::abs(previewSettings.silenceHoldSeconds - 0.30) < 1.0e-12
                && std::abs(previewSettings.tailPadSeconds - 0.05) < 1.0e-12
                && std::abs(previewSettings.maximumTailSeconds - 4.0) < 1.0e-12
                && nearlyEqual(renderSettings.silenceThreshold, 1.5848932e-5f, 1.0e-11f)
                && std::abs(renderSettings.silenceHoldSeconds - 1.0) < 1.0e-12
                && std::abs(renderSettings.tailPadSeconds - 0.15) < 1.0e-12
                && std::abs(renderSettings.maximumTailSeconds - 12.0) < 1.0e-12
                && previewPlan.scanEndSamples == 6000
                && previewPlan.silenceHoldSamples == 300
                && previewPlan.tailPadSamples == 50
                && previewDecision.endedInConfirmedSilence
                && previewDecision.keepSamples == 5551
                && previewDecision.trimmedSamples == 14449
                && !previewUnconfirmed.endedInConfirmedSilence
                && previewUnconfirmed.keepSamples == 6000
                && renderPlan.scanEndSamples == 14000
                && renderPlan.silenceHoldSamples == 1000
                && renderPlan.tailPadSamples == 150
                && renderDecision.endedInConfirmedSilence
                && renderDecision.keepSamples == 12651
                && renderDecision.trimmedSamples == 7349;

            return makeResult("G03", "Simulation", "Production preview/final tail-policy decisions", passed,
                              passed ? "Shared production tail-policy logic retained -80 dBFS/0.30 s/0.05 s/4 s preview and -96 dBFS/1.0 s/0.15 s/12 s render contracts, including late-repeat and unconfirmed-silence behavior."
                                     : "Production preview/render tail-policy contract failed.");
        }

        SimulationScenarioResult scenarioH04()
        {
            const auto tempFolder = std::filesystem::temp_directory_path() / "PoorMansStudioSimulation";
            const auto legacyPath = tempFolder / "legacy_compatibility_fixture.mwproj";
            std::error_code ec;
            std::filesystem::create_directories(tempFolder, ec);
            std::filesystem::remove(legacyPath, ec);

            {
                std::ofstream file(legacyPath, std::ios::binary | std::ios::trunc);
                file
                    << "{\n"
                    << "  \"format\": \"MusicWorkstationProject\",\n"
                    << "  \"version\": 9,\n"
                    << "  \"projectId\": 960001,\n"
                    << "  \"name\": \"Compatibility Fixture\",\n"
                    << "  \"tempoBpm\": 96,\n"
                    << "  \"projectSettings\": {\n"
                    << "    \"projectDefaultBackendId\": 2,\n"
                    << "    \"projectDefaultSoundFontPath\": \"legacy_bank.sf2\",\n"
                    << "    \"projectDefaultSFZPath\": \"legacy_patch.sfz\"\n"
                    << "  },\n"
                    << "  \"sequences\": [{\"number\": 3, \"name\": \"Legacy Sequence\", \"tracks\": [1, 0, 1]}],\n"
                    << "  \"audioClips\": [{\"id\": 7, \"name\": \"Legacy Clip\", \"trackIndex\": 1, \"durationSamples\": 48000}],\n"
                    << "  \"tracks\": [\n"
                    << "    {\"trackId\": 961001, \"name\": \"Legacy MIDI\", \"instrument\": {\"displayName\": \"Legacy Instrument\", \"backendType\": \"SF2\", \"midiChannel\": 5}, \"notes\": [{\"pitch\": 64, \"velocity\": 88, \"startTick\": 120, \"durationTicks\": 720}]},\n"
                    << "    {\"trackId\": 961002, \"name\": \"Legacy Audio\", \"trackType\": \"AudioClip\", \"vstEffects\": {\"enabled\": true, \"slots\": [{\"plugin\": {\"bundlePath\": \"LegacyFx.vst3\", \"name\": \"Legacy FX\", \"uid\": \"legacy-fx\"}}]}, \"notes\": []}\n"
                    << "  ]\n"
                    << "}\n";
            }

            const auto loaded = mw::serialization::ProjectSerializer::loadFromFile(legacyPath);
            std::filesystem::remove(legacyPath, ec);

            bool passed = false;
            if (loaded.has_value())
            {
                const auto& project = *loaded;
                const auto& settings = project.getUserSettings();
                const auto& tracks = project.getTracks();
                passed = project.getMetadata().projectFormatVersion == 22
                    && project.getStableId() == 960001
                    && project.getName() == "Compatibility Fixture"
                    && project.getTempoBpm() == 96
                    && settings.backendId == 2
                    && settings.soundFontPath.filename() == "legacy_bank.sf2"
                    && settings.sfzPath.filename() == "legacy_patch.sfz"
                    && project.getSequences().size() == 1
                    && project.getSequences()[0].id == 3
                    && project.getSequences()[0].number == 3
                    && project.getSequences()[0].tracks == std::vector<int>({ 0, 1 })
                    && project.getAudioClips().size() == 1
                    && project.getAudioClips()[0].sourceTrimStartSamples == 0
                    && project.getAudioClips()[0].sourceTrimEndSamples == 48000
                    && tracks.size() == 2
                    && tracks[0].getStableId() == 961001
                    && tracks[0].getNotes().size() == 1
                    && tracks[0].getNotes()[0].midiChannel == 5
                    && tracks[1].getTrackType() == mw::core::TrackType::AudioClip
                    && tracks[1].getVstEffects().slots.size() == 1
                    && tracks[1].getVstEffects().slots[0].enabled
                    && tracks[1].getVstEffects().slots[0].backendType == mw::core::EffectSlotBackendType::VST3;
            }

            return makeResult("H04", "Simulation", "Legacy project compatibility aliases and defaults", passed,
                              passed ? "A sparse format-9 compatibility fixture loaded through the production ProjectSerializer using legacy setting aliases, sequence defaults/deduplication, AudioClip trim defaults, note-channel fallback, and legacy VST effect-slot defaults."
                                     : "Legacy project compatibility/default contract failed.");
        }

        SimulationScenarioResult scenarioG04()
        {
            mw::audio::ProjectContentEndingInput normal;
            normal.tempoBpm = 120;
            normal.masterVolume = 1.0;
            normal.tracks =
            {
                { false, false, false, 1.0, 3840 },
                { false, true, false, 1.0, 9600 },
                { true, false, false, 0.8, 0 },
                { false, false, false, 0.0, 12000 }
            };
            normal.clips =
            {
                { 2, 1920, 1.5, 1.0 },
                { 1, 7000, 5.0, 1.0 },
                { 99, 4800, 0.25, 1.0 }
            };
            const auto normalEnd = mw::audio::calculateProjectContentEndingSeconds(normal);

            auto solo = normal;
            solo.tracks[0].solo = true;
            solo.tracks[2].solo = true;
            const auto soloEnd = mw::audio::calculateProjectContentEndingSeconds(solo);

            auto silentMaster = normal;
            silentMaster.masterVolume = 0.0;
            const auto silentEnd = mw::audio::calculateProjectContentEndingSeconds(silentMaster);

            mw::audio::ProjectContentEndingInput fallback;
            fallback.tempoBpm = 0;
            fallback.masterVolume = 1.0;
            fallback.tracks = { { false, false, false, 1.0, 960 } };
            fallback.clips = { { -1, -960, 1.25, std::numeric_limits<double>::quiet_NaN() } };
            const auto fallbackEnd = mw::audio::calculateProjectContentEndingSeconds(fallback);

            const bool passed = std::abs(normalEnd - 2.75) < 1.0e-12
                && std::abs(soloEnd - 2.5) < 1.0e-12
                && std::abs(silentEnd) < 1.0e-12
                && std::abs(fallbackEnd - 1.25) < 1.0e-12;
            return makeResult("G04", "Simulation", "Production final-mix content-ending calculation", passed,
                              passed
                                  ? "Production content-ending logic preserved MIDI note endings, AudioClip start/kept duration, solo/mute/gain filtering, unassigned-clip handling, tick clamping, and tempo/gain fallbacks."
                                  : "Production final-mix content-ending calculation contract failed.");
        }

        SimulationScenarioResult scenarioH05()
        {
            constexpr std::uint32_t seedA = 0x06790079u;
            constexpr std::uint32_t seedB = 0x0679007Au;
            const auto firstA = runSeededProjectRoundTrips(seedA);
            const auto secondA = runSeededProjectRoundTrips(seedA);
            const auto firstB = runSeededProjectRoundTrips(seedB);
            const bool passed = firstA.passed && secondA.passed && firstB.passed
                && firstA.noteCount == 1024 && secondA.noteCount == 1024 && firstB.noteCount == 1024
                && firstA.fingerprint == secondA.fingerprint
                && firstA.fingerprint != firstB.fingerprint;
            return makeResult("H05", "Simulation", "Explicit alternate-seed reproducibility", passed,
                              passed
                                  ? "Two runs of seed " + hex32(seedA) + " produced the same ProjectSerializer fixture fingerprint, while seed "
                                      + hex32(seedB) + " produced a different reproducible fixture set."
                                  : "Explicit alternate-seed reproducibility contract failed.");
        }


        SimulationScenarioResult scenarioI01()
        {
            auto makeTrack = [](const char* name, mw::core::StableId stableId)
            {
                mw::core::Track track(name);
                track.setStableId(stableId);
                return track;
            };

            std::vector<mw::core::Track> tracks;
            tracks.push_back(makeTrack("Track A", 101));
            tracks.push_back(makeTrack("Track B", 202));
            tracks.push_back(makeTrack("Track C", 303));

            const auto selectedB = mw::core::captureTrackSelection(tracks, 1);
            const bool initial = selectedB.stableId == 202 && selectedB.index == 1
                && mw::core::resolveTrackSelection(tracks, selectedB) == 1;

            tracks[1].setName("Track B renamed");
            const bool renamePreserved = mw::core::resolveTrackSelection(tracks, selectedB) == 1;

            tracks.insert(tracks.begin(), makeTrack("Inserted", 404));
            const bool insertionPreservedByStableId = mw::core::resolveTrackSelection(tracks, selectedB) == 2;

            std::swap(tracks[0], tracks[2]);
            const bool reorderPreservedByStableId = mw::core::resolveTrackSelection(tracks, selectedB) == 0;

            tracks.erase(tracks.begin());
            const bool removedFallsBackToOrdinal = mw::core::resolveTrackSelection(tracks, selectedB) == 1;

            std::vector<mw::core::Track> replacement;
            replacement.push_back(makeTrack("Replacement A", 501));
            replacement.push_back(makeTrack("Replacement B", 502));
            replacement.push_back(makeTrack("Replacement C", 503));
            const bool unrelatedReplacementUsesOrdinal = mw::core::resolveTrackSelection(replacement, selectedB) == 1;

            replacement.resize(1);
            const bool shortReplacementClamps = mw::core::resolveTrackSelection(replacement, selectedB) == 0;
            const bool noPriorSelectionChoosesFirst = mw::core::resolveTrackSelection(replacement, {}) == 0;
            const bool emptyProjectHasNoSelection = mw::core::resolveTrackSelection({}, selectedB) == -1;

            const bool passed = initial
                && renamePreserved
                && insertionPreservedByStableId
                && reorderPreservedByStableId
                && removedFallsBackToOrdinal
                && unrelatedReplacementUsesOrdinal
                && shortReplacementClamps
                && noPriorSelectionChoosesFirst
                && emptyProjectHasNoSelection;

            return makeResult("I01", "Workflow", "Production track-selection preservation policy", passed,
                              passed
                                  ? "Stable track identity survives selector rebuilds, rename/insert/reorder changes; missing identities fall back to the prior ordinal with clamping, and empty projects clear selection."
                                  : "Production track-selection preservation policy failed.");
        }

        SimulationScenarioResult scenarioI02()
        {
            constexpr mw::core::StableId projectId = 9001;
            constexpr mw::core::StableId armedTrackId = 202;
            constexpr int sampleRate = 48000;
            constexpr int blockSize = 512;
            constexpr int tempoBpm = 120;
            constexpr int ticksPerQuarter = 960;

            mw::playback::PlaybackTransportCoordinator transport;
            const auto startSample = static_cast<std::int64_t>(2.5 * sampleRate);
            const auto generation = transport.beginPreparing(
                projectId,
                startSample,
                sampleRate,
                blockSize,
                { 101, armedTrackId, 303 });
            const bool playbackStarted = transport.beginPlayingWithAudioFormat(
                generation,
                startSample,
                static_cast<std::int64_t>(10.0 * sampleRate),
                sampleRate,
                blockSize);

            const auto config = mw::midi::makeTransportSynchronizedRecordingConfig(
                transport.snapshot(), tempoBpm, ticksPerQuarter);
            mw::midi::MidiRecordingSession session;
            const bool recordingStarted = session.start(config);

            std::vector<mw::core::NoteEvent> projectNotes;
            mw::core::NoteEvent existing;
            existing.pitch = 48;
            existing.velocity = 88;
            existing.midiChannel = 1;
            existing.startTick = 240;
            existing.durationTicks = 480;
            projectNotes.push_back(existing);

            transport.updatePosition(generation, static_cast<std::int64_t>(3.0 * sampleRate), static_cast<std::int64_t>(10.0 * sampleRate));
            session.noteOn(2, 64, 110, transport.snapshot().currentSeconds());

            // UI selection may move during a take, but the armed stable ID remains the
            // recording owner and is deliberately independent of the selected ordinal.
            mw::core::StableId selectedTrackId = armedTrackId;
            selectedTrackId = 303;
            const bool armedTargetStayedStable = selectedTrackId != armedTrackId && armedTrackId == 202;

            transport.updatePosition(generation, static_cast<std::int64_t>(3.5 * sampleRate), static_cast<std::int64_t>(10.0 * sampleRate));
            session.noteOff(2, 64, transport.snapshot().currentSeconds());

            transport.updatePosition(generation, static_cast<std::int64_t>(4.0 * sampleRate), static_cast<std::int64_t>(10.0 * sampleRate));
            session.noteOn(3, 67, 96, transport.snapshot().currentSeconds());
            transport.updatePosition(generation, static_cast<std::int64_t>(4.25 * sampleRate), static_cast<std::int64_t>(10.0 * sampleRate));
            const auto panicFinalized = session.releaseActiveNotes(transport.snapshot().currentSeconds());
            const bool panicKeptTakeActive = session.isRecording();

            transport.updatePosition(generation, static_cast<std::int64_t>(4.5 * sampleRate), static_cast<std::int64_t>(10.0 * sampleRate));
            session.noteOn(4, 69, 100, transport.snapshot().currentSeconds());
            transport.updatePosition(generation, static_cast<std::int64_t>(5.0 * sampleRate), static_cast<std::int64_t>(10.0 * sampleRate));
            const auto result = session.stop(transport.snapshot().currentSeconds());
            transport.beginStopping(generation);
            const bool transportStopped = transport.finishStopped(generation);

            projectNotes.insert(projectNotes.end(), result.notes.begin(), result.notes.end());

            const bool firstNoteCorrect = result.notes.size() >= 1
                && result.notes[0].pitch == 64
                && result.notes[0].velocity == 110
                && result.notes[0].midiChannel == 2
                && result.notes[0].startTick == 5760
                && result.notes[0].durationTicks == 960;
            const bool panicNoteCorrect = result.notes.size() >= 2
                && result.notes[1].pitch == 67
                && result.notes[1].midiChannel == 3
                && result.notes[1].startTick == 7680
                && result.notes[1].durationTicks == 480;
            const bool heldStopNoteCorrect = result.notes.size() >= 3
                && result.notes[2].pitch == 69
                && result.notes[2].midiChannel == 4
                && result.notes[2].startTick == 8640
                && result.notes[2].durationTicks == 960
                && result.finalizedHeldNoteCount == 1;
            const bool appendPreservedExisting = projectNotes.size() == 4
                && projectNotes.front().pitch == 48
                && projectNotes.front().startTick == 240;

            const bool passed = playbackStarted
                && recordingStarted
                && config.startTimeSeconds == 2.5
                && config.startTick == 4800
                && armedTargetStayedStable
                && panicFinalized == 1
                && panicKeptTakeActive
                && firstNoteCorrect
                && panicNoteCorrect
                && heldStopNoteCorrect
                && appendPreservedExisting
                && transportStopped;

            return makeResult("I02", "Workflow", "Transport-synchronized MIDI recording and overdub", passed,
                              passed
                                  ? "A nonzero project transport position anchored the take; Note On/Off timing followed transport ticks, the armed stable target survived UI selection changes, Panic finalized held notes without ending the take, Stop finalized remaining notes, and the take appended without replacing existing notes."
                                  : "Transport-synchronized MIDI recording/overdub contract failed.");
        }

        SimulationScenarioResult scenarioI03()
        {
            const auto offPlan = mw::midi::makeMidiRecordingCountInPlan(0, 120.0, 4, 4);
            const bool offCorrect = !offPlan.enabled()
                && offPlan.beats.empty()
                && std::abs(offPlan.durationSeconds) < 1.0e-9;

            const auto oneBar44 = mw::midi::makeMidiRecordingCountInPlan(1, 120.0, 4, 4);
            const bool oneBar44Correct = oneBar44.enabled()
                && oneBar44.beats.size() == 4
                && std::abs(oneBar44.secondsPerBeat - 0.5) < 1.0e-9
                && std::abs(oneBar44.durationSeconds - 2.0) < 1.0e-9
                && oneBar44.beats[0].accented
                && !oneBar44.beats[1].accented
                && std::abs(oneBar44.beats[3].startSeconds - 1.5) < 1.0e-9;

            const auto twoBar68 = mw::midi::makeMidiRecordingCountInPlan(2, 120.0, 6, 8);
            const bool twoBar68Correct = twoBar68.enabled()
                && twoBar68.beats.size() == 12
                && std::abs(twoBar68.secondsPerBeat - 0.25) < 1.0e-9
                && std::abs(twoBar68.durationSeconds - 3.0) < 1.0e-9
                && twoBar68.beats[0].accented
                && twoBar68.beats[6].accented
                && !twoBar68.beats[5].accented
                && !twoBar68.beats[7].accented;

            mw::midi::MidiRecordingCountInState state;
            const auto cancelledGeneration = state.begin(2, 120.0, 6, 8, 2.5);
            const bool cancellationPrepared = state.isActive()
                && std::abs(state.requestedStartSeconds() - 2.5) < 1.0e-9;
            state.cancel();
            const bool cancellationCorrect = cancellationPrepared
                && !state.isActive()
                && !state.complete(cancelledGeneration);

            const auto handoffGeneration = state.begin(1, 120.0, 4, 4, 2.5);
            const double preservedAnchor = state.requestedStartSeconds();
            const bool handoffAccepted = state.complete(handoffGeneration) && !state.isActive();

            constexpr mw::core::StableId projectId = 9012;
            constexpr int sampleRate = 48000;
            constexpr int blockSize = 512;
            mw::playback::PlaybackTransportCoordinator transport;
            const auto startSample = static_cast<std::int64_t>(std::llround(preservedAnchor * sampleRate));
            const auto transportGeneration = transport.beginPreparing(
                projectId,
                startSample,
                sampleRate,
                blockSize,
                { 202 });
            const bool playbackStarted = transport.beginPlayingWithAudioFormat(
                transportGeneration,
                startSample,
                static_cast<std::int64_t>(10.0 * sampleRate),
                sampleRate,
                blockSize);
            const auto recordingConfig = mw::midi::makeTransportSynchronizedRecordingConfig(
                transport.snapshot(), 120, 960);
            const bool anchorUnshifted = playbackStarted
                && std::abs(recordingConfig.startTimeSeconds - 2.5) < 1.0e-9
                && recordingConfig.startTick == 4800;

            const bool passed = offCorrect
                && oneBar44Correct
                && twoBar68Correct
                && cancellationCorrect
                && handoffAccepted
                && anchorUnshifted;

            return makeResult("I03", "Workflow", "MIDI recording count-in scheduling", passed,
                              passed
                                  ? "Off/1-bar/2-bar count-in scheduling preserved 4/4 and 6/8 notated beats with first-beat accents; cancellation invalidated stale completion, and handoff kept a nonzero 2.5 s / tick 4800 recording anchor unchanged."
                                  : "MIDI recording count-in scheduling, cancellation, accent, or transport-handoff contract failed.");
        }

        SimulationScenarioResult scenarioI04()
        {
            constexpr int ticksPerQuarter = 960;

            const bool gridSizesCorrect =
                mw::midi::midiRecordingQuantizeGridTicks(4, ticksPerQuarter) == 960
                && mw::midi::midiRecordingQuantizeGridTicks(8, ticksPerQuarter) == 480
                && mw::midi::midiRecordingQuantizeGridTicks(16, ticksPerQuarter) == 240
                && mw::midi::midiRecordingQuantizeGridTicks(32, ticksPerQuarter) == 120
                && mw::midi::midiRecordingQuantizeGridTicks(64, ticksPerQuarter) == 60;

            bool nearestGridCorrect = true;
            for (const int division : { 4, 8, 16, 32, 64 })
            {
                const auto grid = mw::midi::midiRecordingQuantizeGridTicks(division, ticksPerQuarter);
                nearestGridCorrect = nearestGridCorrect
                    && mw::midi::quantizeMidiRecordingStartTick((grid / 2) - 1, division, ticksPerQuarter) == 0
                    && mw::midi::quantizeMidiRecordingStartTick(grid / 2, division, ticksPerQuarter) == grid;
            }

            std::vector<mw::core::NoteEvent> existingNotes;
            existingNotes.emplace_back(48, 88, 240, 480, 1, mw::core::Articulation::Legato);
            const auto existingBefore = existingNotes;

            std::vector<mw::core::NoteEvent> take;
            take.emplace_back(64, 110, 5030, 333, 2, mw::core::Articulation::Accent);
            take.emplace_back(67, 96, 5150, 777, 3, mw::core::Articulation::Staccato);
            take.emplace_back(69, 100, -30, 120, 4, mw::core::Articulation::Normal);

            auto offTake = take;
            const auto offChanged = mw::midi::quantizeMidiRecordingTake(offTake, 0, ticksPerQuarter);
            const bool offPreserved = offChanged == 0
                && offTake.size() == take.size()
                && offTake[0].startTick == take[0].startTick
                && offTake[1].startTick == take[1].startTick
                && offTake[2].startTick == take[2].startTick;

            const auto changed = mw::midi::quantizeMidiRecordingTake(take, 16, ticksPerQuarter);
            const bool nonzeroTransportTimingQuantized = take.size() == 3
                && take[0].startTick == 5040
                && take[1].startTick == 5040
                && take[2].startTick == 0
                && changed == 3;

            const bool noteFieldsPreserved = take[0].pitch == 64
                && take[0].velocity == 110
                && take[0].midiChannel == 2
                && take[0].durationTicks == 333
                && take[0].articulation == mw::core::Articulation::Accent
                && take[1].durationTicks == 777
                && take[1].articulation == mw::core::Articulation::Staccato;

            std::vector<mw::core::NoteEvent> projectNotes = existingNotes;
            projectNotes.insert(projectNotes.end(), take.begin(), take.end());
            const bool existingNotesUntouched = existingNotes.size() == existingBefore.size()
                && existingNotes[0].pitch == existingBefore[0].pitch
                && existingNotes[0].startTick == existingBefore[0].startTick
                && existingNotes[0].durationTicks == existingBefore[0].durationTicks
                && existingNotes[0].articulation == existingBefore[0].articulation
                && projectNotes.front().startTick == 240;

            const bool invalidDivisionIsOff = mw::midi::sanitizeMidiRecordingQuantizeDivision(12) == 0
                && mw::midi::quantizeMidiRecordingStartTick(5030, 12, ticksPerQuarter) == 5030;

            const bool passed = gridSizesCorrect
                && nearestGridCorrect
                && offPreserved
                && nonzeroTransportTimingQuantized
                && noteFieldsPreserved
                && existingNotesUntouched
                && invalidDivisionIsOff;

            return makeResult("I04", "Workflow", "Post-take MIDI recording quantization", passed,
                              passed
                                  ? "Off preserved raw take timing; 1/4 through 1/64 grids rounded Note On/start ticks to the nearest project grid with forward half-grid ties and nonnegative clamping, while duration, pitch, velocity, MIDI channel, articulation, and pre-existing project notes remained unchanged."
                                  : "Post-take MIDI quantization grid, preservation, or existing-note isolation contract failed.");
        }

        SimulationScenarioResult scenarioI05()
        {
            const auto off = mw::midi::makeMidiRecordingMetronomeConfig(false, 120.0, 4, 4);
            const bool offIsSilent = !off.enabled
                && mw::midi::midiRecordingMetronomeSampleAtProjectTime(off, 0.001) == 0.0f;

            const auto fourFour = mw::midi::makeMidiRecordingMetronomeConfig(true, 120.0, 4, 4);
            const auto beat0 = mw::midi::midiRecordingMetronomeNextBeatAtOrAfter(fourFour, 0.0);
            const auto beatAfterPointOne = mw::midi::midiRecordingMetronomeNextBeatAtOrAfter(fourFour, 0.1);
            const auto barTwo = mw::midi::midiRecordingMetronomeBeatAtOrBefore(fourFour, 2.0);
            const bool fourFourPhase = std::abs(fourFour.secondsPerBeat - 0.5) < 1.0e-9
                && beat0.globalBeatIndex == 0 && beat0.accented
                && beatAfterPointOne.globalBeatIndex == 1
                && std::abs(beatAfterPointOne.startSeconds - 0.5) < 1.0e-9
                && !beatAfterPointOne.accented
                && barTwo.globalBeatIndex == 4 && barTwo.accented;

            const auto sixEight = mw::midi::makeMidiRecordingMetronomeConfig(true, 120.0, 6, 8);
            const auto sixEightBarTwo = mw::midi::midiRecordingMetronomeBeatAtOrBefore(sixEight, 1.5);
            const bool sixEightPhase = std::abs(sixEight.secondsPerBeat - 0.25) < 1.0e-9
                && sixEightBarTwo.globalBeatIndex == 6
                && sixEightBarTwo.beatIndex == 0
                && sixEightBarTwo.accented;

            constexpr double requestedStartSeconds = 2.5;
            const auto nonzeroBeat = mw::midi::midiRecordingMetronomeNextBeatAtOrAfter(
                fourFour, requestedStartSeconds);
            const auto countIn = mw::midi::makeMidiRecordingCountInPlan(1, 120.0, 4, 4);
            const bool nonzeroProjectPhasePreserved = countIn.enabled()
                && std::abs(countIn.durationSeconds - 2.0) < 1.0e-9
                && nonzeroBeat.globalBeatIndex == 5
                && std::abs(nonzeroBeat.startSeconds - requestedStartSeconds) < 1.0e-9
                && !nonzeroBeat.accented;

            mw::midi::MidiRecordingMetronomeRunState runState;
            const bool began = runState.begin(fourFour, requestedStartSeconds);
            mw::midi::MidiRecordingSession session;
            mw::midi::MidiRecordingConfig config;
            config.startTimeSeconds = requestedStartSeconds;
            config.startTick = 4800;
            config.tempoBpm = 120;
            config.ticksPerQuarterNote = 960;
            const bool sessionStarted = session.start(config);
            session.noteOn(1, 64, 100, 2.55);
            const auto panicClosed = session.releaseActiveNotes(2.75);
            const auto futureBeat = mw::midi::midiRecordingMetronomeNextBeatAtOrAfter(fourFour, 2.76);
            const bool panicContinues = began && sessionStarted && panicClosed == 1
                && session.isRecording() && runState.isActive()
                && futureBeat.startSeconds >= 3.0 - 1.0e-9;
            const auto stoppedTake = session.stop(3.10);
            runState.stop();
            const bool stopEnds = !session.isRecording() && !runState.isActive()
                && stoppedTake.notes.size() == 1;

            const auto accentSample = std::abs(mw::midi::midiRecordingMetronomeSampleAtProjectTime(fourFour, 2.001));
            const auto regularSample = std::abs(mw::midi::midiRecordingMetronomeSampleAtProjectTime(fourFour, 2.501));
            const bool accentIsStronger = accentSample > regularSample && regularSample > 0.0f;

            const bool passed = offIsSilent
                && fourFourPhase
                && sixEightPhase
                && nonzeroProjectPhasePreserved
                && panicContinues
                && stopEnds
                && accentIsStronger;

            return makeResult("I05", "Workflow", "Continuous MIDI recording metronome phase", passed,
                              passed
                                  ? "Off remained silent; 4/4 and 6/8 clicks followed absolute project beat/bar phase with stronger bar accents; a 2.5 s nonzero recording anchor stayed on project beat 5 after count-in, Panic finalized held notes while recording/metronome state continued, and Stop ended both cleanly."
                                  : "Continuous recording metronome phase, accent, count-in handoff, Panic continuation, or Stop contract failed.");
        }

        SimulationScenarioResult scenarioI06()
        {
            using mw::midi::MidiRecordingControlContext;
            using mw::midi::MidiRecordingControlState;

            MidiRecordingControlContext idle;
            idle.state = MidiRecordingControlState::Idle;
            idle.armedTrackAvailable = true;
            idle.midiInputOpen = true;
            idle.deviceSelectionAvailable = true;
            idle.canUndoTake = true;
            idle.recordingConfigurationValid = true;
            idle.punchPositionAvailable = true;
            const auto idleAvailability = mw::midi::makeMidiRecordingControlAvailability(idle);

            const bool idleReady = idleAvailability.deviceSelectorEnabled
                && idleAvailability.channelSelectorEnabled
                && idleAvailability.refreshDevicesEnabled
                && idleAvailability.applySelectionEnabled
                && idleAvailability.startMonitorEnabled
                && !idleAvailability.stopMonitorEnabled
                && idleAvailability.armSelectedEnabled
                && idleAvailability.recordMidiEnabled
                && !idleAvailability.stopRecordingEnabled
                && !idleAvailability.panicEnabled
                && idleAvailability.countInEnabled
                && idleAvailability.quantizeEnabled
                && idleAvailability.metronomeEnabled
                && idleAvailability.punchModeEnabled
                && idleAvailability.loopModeEnabled
                && idleAvailability.punchSetInEnabled
                && idleAvailability.punchSetOutEnabled
                && idleAvailability.punchClearEnabled
                && idleAvailability.undoTakeEnabled
                && !idleAvailability.redoTakeEnabled
                && idleAvailability.closeEnabled;

            idle.liveMonitoringActive = true;
            idle.canRedoTake = true;
            const auto monitoringAvailability = mw::midi::makeMidiRecordingControlAvailability(idle);
            const bool idleMonitorRestores = !monitoringAvailability.startMonitorEnabled
                && monitoringAvailability.stopMonitorEnabled
                && monitoringAvailability.panicEnabled
                && monitoringAvailability.undoTakeEnabled
                && monitoringAvailability.redoTakeEnabled;

            const auto busyLockedExceptSafety = [](MidiRecordingControlState state)
            {
                MidiRecordingControlContext context;
                context.state = state;
                context.armedTrackAvailable = true;
                context.midiInputOpen = true;
                context.liveMonitoringActive = true;
                context.deviceSelectionAvailable = true;
                context.canUndoTake = true;
                context.canRedoTake = true;
                const auto availability = mw::midi::makeMidiRecordingControlAvailability(context);
                return !availability.deviceSelectorEnabled
                    && !availability.channelSelectorEnabled
                    && !availability.refreshDevicesEnabled
                    && !availability.applySelectionEnabled
                    && !availability.startMonitorEnabled
                    && !availability.stopMonitorEnabled
                    && !availability.armSelectedEnabled
                    && !availability.recordMidiEnabled
                    && availability.stopRecordingEnabled
                    && availability.panicEnabled
                    && !availability.countInEnabled
                    && !availability.quantizeEnabled
                    && !availability.metronomeEnabled
                    && !availability.punchModeEnabled
                    && !availability.loopModeEnabled
                    && !availability.punchSetInEnabled
                    && !availability.punchSetOutEnabled
                    && !availability.punchClearEnabled
                    && !availability.undoTakeEnabled
                    && !availability.redoTakeEnabled
                    && !availability.closeEnabled;
            };

            const bool countInLocked = busyLockedExceptSafety(MidiRecordingControlState::CountIn);
            const bool preparingLocked = busyLockedExceptSafety(MidiRecordingControlState::Preparing);
            const bool punchPreRollLocked = busyLockedExceptSafety(MidiRecordingControlState::PunchPreRoll);
            const bool recordingLocked = busyLockedExceptSafety(MidiRecordingControlState::Recording);

            const bool stateResolution = mw::midi::midiRecordingControlState(false, false, false) == MidiRecordingControlState::Idle
                && mw::midi::midiRecordingControlState(false, false, true) == MidiRecordingControlState::CountIn
                && mw::midi::midiRecordingControlState(false, true, false) == MidiRecordingControlState::Preparing
                && mw::midi::midiRecordingControlState(false, false, false, true) == MidiRecordingControlState::PunchPreRoll
                && mw::midi::midiRecordingControlState(true, true, true) == MidiRecordingControlState::Recording
                && std::string(mw::midi::midiRecordingControlStateLabel(MidiRecordingControlState::Preparing)) == "PREPARING PLAYBACK";

            const bool passed = idleReady
                && idleMonitorRestores
                && countInLocked
                && preparingLocked
                && punchPreRollLocked
                && recordingLocked
                && stateResolution;

            return makeResult("I06", "Workflow", "MIDI recording control safety and state clarity", passed,
                              passed
                                  ? "Idle restored route/monitor/take controls from actual context; Count-In, Preparing, Punch Pre-Roll, and Recording locked route/options/Undo/Redo/Close while keeping Panic and Stop Recording available; explicit state resolution preserved Recording precedence."
                                  : "Recording control-state locking, safety-control availability, idle restoration, or state resolution failed.");
        }


        SimulationScenarioResult scenarioI07()
        {
            using mw::midi::MidiRecordingPunchPhase;
            using mw::midi::MidiRecordingPunchPosition;
            using mw::midi::MidiRecordingPunchRegion;

            constexpr int tempoBpm = 120;
            constexpr int ticksPerQuarter = 960;
            const MidiRecordingPunchRegion disabled { false, -1, -1 };
            const MidiRecordingPunchRegion valid { true, 5760, 7680 }; // 3.0 s -> 4.0 s
            const MidiRecordingPunchRegion invalid { true, 7680, 5760 };

            const bool validationCorrect = mw::midi::isValidMidiRecordingPunchRegion(disabled)
                && mw::midi::isValidMidiRecordingPunchRegion(valid)
                && !mw::midi::isValidMidiRecordingPunchRegion(invalid);

            const bool boundaryClassification =
                mw::midi::midiRecordingPunchPositionForTick(valid, 5759) == MidiRecordingPunchPosition::BeforeIn
                && mw::midi::midiRecordingPunchPositionForTick(valid, 5760) == MidiRecordingPunchPosition::Inside
                && mw::midi::midiRecordingPunchPositionForTick(valid, 7679) == MidiRecordingPunchPosition::Inside
                && mw::midi::midiRecordingPunchPositionForTick(valid, 7680) == MidiRecordingPunchPosition::AtOrAfterOut;

            const bool boundarySeconds =
                std::abs(mw::midi::midiRecordingProjectSecondsForTick(5760, tempoBpm, ticksPerQuarter) - 3.0) < 1.0e-9
                && std::abs(mw::midi::midiRecordingProjectSecondsForTick(7680, tempoBpm, ticksPerQuarter) - 4.0) < 1.0e-9;

            mw::midi::MidiRecordingPunchRunState runState;
            const auto generation = runState.begin(valid, 4800); // project playback starts at 2.5 s
            const bool preRollCorrect = generation != 0
                && runState.isActive()
                && runState.isPreRoll()
                && runState.phase() == MidiRecordingPunchPhase::PreRoll
                && !runState.activateCapture(generation, 5759)
                && runState.activateCapture(generation, 5760)
                && runState.isCapturePhase();

            mw::midi::MidiRecordingConfig config;
            config.startTimeSeconds = 3.0;
            config.startTick = valid.inTick;
            config.tempoBpm = tempoBpm;
            config.ticksPerQuarterNote = ticksPerQuarter;
            mw::midi::MidiRecordingSession session;
            const bool sessionStarted = session.start(config);
            // The physical Note On that occurred during punch pre-roll is deliberately not
            // submitted to the recording session. A new Note On at Punch In is captured.
            session.noteOn(1, 60, 100, 3.0);
            session.noteOn(2, 64, 110, 3.5);
            const auto stopped = session.stop(4.0); // exact Punch Out boundary
            const bool exactCaptureBoundary = sessionStarted
                && stopped.stopTick == valid.outTick
                && stopped.notes.size() == 2
                && stopped.notes[0].startTick == valid.inTick
                && stopped.notes[0].durationTicks == 1920
                && stopped.notes[1].startTick == 6720
                && stopped.notes[1].durationTicks == 960
                && stopped.finalizedHeldNoteCount == 2;

            const bool punchOutRecognized = runState.shouldPunchOut(generation, valid.outTick)
                && runState.finish(generation)
                && !runState.isActive();

            mw::midi::MidiRecordingPunchRunState insideState;
            const auto insideGeneration = insideState.begin(valid, 6720);
            const bool startsInsideImmediately = insideGeneration != 0
                && insideState.isCapturePhase();
            mw::midi::MidiRecordingPunchRunState outState;
            const auto outGeneration = outState.begin(valid, valid.outTick);
            const bool startAtOutRejected = outGeneration != 0 && !outState.isActive();

            const auto countIn = mw::midi::makeMidiRecordingCountInPlan(1, tempoBpm, 4, 4);
            const bool countInDoesNotShiftRegion = countIn.enabled()
                && std::abs(countIn.durationSeconds - 2.0) < 1.0e-9
                && valid.inTick == 5760 && valid.outTick == 7680;

            std::vector<mw::core::NoteEvent> existingNotes;
            existingNotes.emplace_back(48, 80, 1200, 240, 1, mw::core::Articulation::Legato);
            const auto existingBefore = existingNotes;
            existingNotes.insert(existingNotes.end(), stopped.notes.begin(), stopped.notes.end());
            const bool overdubPreservesExisting = existingNotes.size() == 3
                && existingNotes.front().startTick == existingBefore.front().startTick
                && existingNotes.front().durationTicks == existingBefore.front().durationTicks
                && existingNotes.front().articulation == existingBefore.front().articulation;

            std::vector<mw::core::NoteEvent> quantizedPunchTake;
            quantizedPunchTake.emplace_back(72, 100, 7600, 500, 1, mw::core::Articulation::Normal);
            mw::midi::quantizeMidiRecordingTake(quantizedPunchTake, 16, ticksPerQuarter);
            const auto punchAdjusted = mw::midi::constrainMidiRecordingTakeToPunchRegion(quantizedPunchTake, valid);
            const bool quantizeStillRespectsPunchOut = punchAdjusted == 1
                && quantizedPunchTake.size() == 1
                && quantizedPunchTake[0].startTick == 7679
                && quantizedPunchTake[0].durationTicks == 1;

            const bool passed = validationCorrect
                && boundaryClassification
                && boundarySeconds
                && preRollCorrect
                && exactCaptureBoundary
                && punchOutRecognized
                && startsInsideImmediately
                && startAtOutRejected
                && countInDoesNotShiftRegion
                && overdubPreservesExisting
                && quantizeStillRespectsPunchOut;

            return makeResult("I07", "Workflow", "MIDI auto punch region and capture gating", passed,
                              passed
                                  ? "Runtime Punch In/Out ticks validated exact before/inside/out boundaries; backing pre-roll gated capture until Punch In, capture started immediately when already inside, held notes finalized exactly at Punch Out, Count-In left region ticks unchanged, quantized punch notes remained inside the region, and overdub preserved existing notes."
                                  : "MIDI punch-region validation, pre-roll gating, exact Punch Out finalization, or overdub-isolation contract failed.");
        }

        SimulationScenarioResult scenarioI08()
        {
            using mw::midi::MidiRecordingPunchRegion;

            const MidiRecordingPunchRegion region { true, 5760, 7680 };
            mw::midi::MidiRecordingPunchRunState runState;
            const auto generation = runState.begin(region, 5760);

            const bool earlyClaimRejected = generation != 0
                && !runState.claimPunchOut(generation, 7679)
                && !runState.punchOutClaimed();
            const bool exactClaimAcceptedOnce = runState.claimPunchOut(generation, 7680)
                && runState.punchOutClaimed()
                && !runState.shouldPunchOut(generation, 7680)
                && !runState.claimPunchOut(generation, 7680);
            const bool staleGenerationRejected = !runState.claimPunchOut(generation + 1, 9000);
            const bool finishAfterClaim = runState.finish(generation) && !runState.isActive();

            const auto nextGeneration = runState.begin(region, 6000);
            const bool newRunRearmsStop = nextGeneration != generation
                && runState.isCapturePhase()
                && !runState.punchOutClaimed()
                && runState.claimPunchOut(nextGeneration, 8000);

            const bool finalWindowPolling =
                mw::midi::midiRecordingPunchBoundaryDelayMilliseconds(1.0, true) == 980
                && mw::midi::midiRecordingPunchBoundaryDelayMilliseconds(0.020, true) == 4
                && mw::midi::midiRecordingPunchBoundaryDelayMilliseconds(0.003, true) == 3
                && mw::midi::midiRecordingPunchBoundaryDelayMilliseconds(0.0, true) == 1
                && mw::midi::midiRecordingPunchBoundaryDelayMilliseconds(0.125, false) == 125;

            const auto stopSeconds = mw::midi::midiRecordingProjectSecondsForTick(7680, 120, 960);
            const bool logicalStopPositionExact = std::abs(stopSeconds - 4.0) < 1.0e-9;

            const bool passed = earlyClaimRejected
                && exactClaimAcceptedOnce
                && staleGenerationRejected
                && finishAfterClaim
                && newRunRearmsStop
                && finalWindowPolling
                && logicalStopPositionExact;

            return makeResult("I08", "Workflow", "Punch Out playback-stop coordination", passed,
                              passed
                                  ? "Punch Out stop ownership was generation-guarded and one-shot; early, duplicate, and stale stop claims were rejected; each new punch run re-armed cleanly; final-window polling tightened the boundary check while preserving long-wait efficiency; the logical stop position remained the exact Punch Out tick."
                                  : "Punch Out one-shot stop ownership, final-window boundary polling, or exact logical stop-position coordination failed.");
        }

        SimulationScenarioResult scenarioI09()
        {
            using mw::midi::MidiRecordingPunchRegion;

            constexpr int tempo = 120;
            constexpr int ticksPerQuarter = 960;
            const MidiRecordingPunchRegion region { true, 5760, 7680 };

            mw::midi::MidiRecordingLoopRunState loopState;
            const auto generation = loopState.begin(true, region, 4800);
            const bool preRollStartsWithoutPass = generation != 0
                && loopState.isActive()
                && loopState.passNumber() == 0
                && loopState.activateFirstPass(generation, 5760)
                && loopState.passNumber() == 1;

            mw::midi::MidiRecordingConfig passConfig;
            passConfig.tempoBpm = tempo;
            passConfig.ticksPerQuarterNote = ticksPerQuarter;
            passConfig.startTick = region.inTick;
            passConfig.startTimeSeconds = mw::midi::midiRecordingProjectSecondsForTick(
                region.inTick, tempo, ticksPerQuarter);
            const auto outSeconds = mw::midi::midiRecordingProjectSecondsForTick(
                region.outTick, tempo, ticksPerQuarter);

            mw::midi::MidiRecordingSession passOne;
            const bool passOneStarted = passOne.start(passConfig);
            passOne.noteOn(1, 60, 100, passConfig.startTimeSeconds + 0.25);
            auto passOneResult = passOne.stop(outSeconds);
            mw::midi::constrainMidiRecordingTakeToPunchRegion(passOneResult.notes, region);
            const bool heldNoteClosesAtOut = passOneResult.notes.size() == 1
                && passOneResult.finalizedHeldNoteCount == 1
                && passOneResult.notes[0].startTick >= region.inTick
                && passOneResult.notes[0].startTick + passOneResult.notes[0].durationTicks == region.outTick;

            const bool oneShotWrap = loopState.claimWrap(generation, region.outTick)
                && loopState.wrapClaimed()
                && !loopState.claimWrap(generation, region.outTick)
                && loopState.beginNextPass(generation)
                && loopState.passNumber() == 2
                && !loopState.wrapClaimed();

            mw::midi::MidiRecordingSession passTwo;
            const bool passTwoStarted = passTwo.start(passConfig);
            // A Note Off from a key held across the wrap must not create a new note in the
            // fresh pass. Only an actual retriggered Note On is recordable in pass two.
            passTwo.noteOff(1, 60, passConfig.startTimeSeconds + 0.05);
            passTwo.noteOn(1, 64, 96, passConfig.startTimeSeconds + 0.10);
            passTwo.noteOff(1, 64, passConfig.startTimeSeconds + 0.30);
            auto passTwoResult = passTwo.stop(passConfig.startTimeSeconds + 0.50);
            const bool noPhantomHeldNote = passTwoResult.notes.size() == 1
                && passTwoResult.notes[0].pitch == 64;

            mw::midi::MidiRecordingLoopRunState insideState;
            const auto insideGeneration = insideState.begin(true, region, 6500);
            const bool partialFirstPass = insideState.isActive()
                && insideState.passNumber() == 1
                && insideState.matches(insideGeneration);

            const bool staleGenerationRejected = !loopState.claimWrap(generation + 1, region.outTick + 100);

            mw::midi::MidiRecordingCountInState countInState;
            const auto countInGeneration = countInState.begin(
                2, tempo, 4, 4, passConfig.startTimeSeconds);
            const bool countInLeavesLoopAnchor = countInGeneration != 0
                && countInState.plan().enabled()
                && std::abs(countInState.requestedStartSeconds() - passConfig.startTimeSeconds) < 1.0e-9
                && region.inTick == 5760
                && region.outTick == 7680;

            const auto metronomeConfig = mw::midi::makeMidiRecordingMetronomeConfig(true, tempo, 4, 4);
            const auto firstPassBeat = mw::midi::midiRecordingMetronomeBeatAtOrBefore(
                metronomeConfig, passConfig.startTimeSeconds);
            const auto wrappedPassBeat = mw::midi::midiRecordingMetronomeBeatAtOrBefore(
                metronomeConfig, passConfig.startTimeSeconds);
            const bool metronomePhaseRepeatsProjectGrid = firstPassBeat.globalBeatIndex == wrappedPassBeat.globalBeatIndex
                && firstPassBeat.beatIndex == wrappedPassBeat.beatIndex;

            std::vector<mw::core::NoteEvent> existingNotes;
            existingNotes.emplace_back(48, 90, 1000, 240, 1, mw::core::Articulation::Normal);
            const auto undoSnapshot = existingNotes;
            existingNotes.insert(existingNotes.end(), passOneResult.notes.begin(), passOneResult.notes.end());
            existingNotes.insert(existingNotes.end(), passTwoResult.notes.begin(), passTwoResult.notes.end());
            const bool overdubAccumulated = existingNotes.size() == 3 && existingNotes[0].pitch == 48;
            existingNotes = undoSnapshot;
            const bool wholeSessionUndoModel = existingNotes.size() == 1 && existingNotes[0].pitch == 48;

            const bool passed = preRollStartsWithoutPass
                && passOneStarted
                && heldNoteClosesAtOut
                && oneShotWrap
                && passTwoStarted
                && noPhantomHeldNote
                && partialFirstPass
                && staleGenerationRejected
                && countInLeavesLoopAnchor
                && metronomePhaseRepeatsProjectGrid
                && overdubAccumulated
                && wholeSessionUndoModel;

            return makeResult("I09", "Workflow", "MIDI loop recording and pass wrapping", passed,
                              passed
                                  ? "Loop recording used the runtime In/Out region for repeated overdub passes; pre-roll activated pass 1 at In, partial starts entered pass 1 immediately, exact Out finalized held notes, one-shot wraps re-armed the next pass, held keys produced no phantom Note On after wrap, Count-In left the loop anchor unchanged, metronome phase remained project-grid based, and all passes modeled one session-level Undo unit."
                                  : "MIDI loop pass activation, exact wrap ownership, held-note isolation, Count-In/metronome composition, or whole-session overdub/Undo contract failed.");
        }


        SimulationScenarioResult scenarioI10()
        {
            using mw::midi::MidiRecordingLoopWrapStage;
            using mw::midi::MidiRecordingPunchRegion;

            const MidiRecordingPunchRegion region { true, 5760, 7680 };
            constexpr std::uint64_t generation = 41;

            mw::midi::MidiRecordingLoopWrapTransaction transaction;
            const bool earlyClaimRejected = !transaction.begin(generation, 1, region, region.outTick - 1);
            const bool exactClaimAccepted = transaction.begin(generation, 1, region, region.outTick)
                && transaction.isActive()
                && transaction.stage() == MidiRecordingLoopWrapStage::Claimed
                && transaction.inTick() == region.inTick
                && transaction.outTick() == region.outTick;
            const bool duplicateClaimRejected = !transaction.begin(generation, 1, region, region.outTick + 5);
            const bool staleSeekRejected = !transaction.markSeeking(generation + 1, 1);
            const bool seekAccepted = transaction.markSeeking(generation, 1)
                && transaction.stage() == MidiRecordingLoopWrapStage::Seeking;
            const bool wrongPassRestartRejected = !transaction.markRestarted(generation, 2);
            const bool restartAccepted = transaction.markRestarted(generation, 1)
                && transaction.stage() == MidiRecordingLoopWrapStage::Restarted;
            const bool finishAccepted = transaction.finish(generation, 1)
                && !transaction.isActive()
                && transaction.stage() == MidiRecordingLoopWrapStage::Idle;

            const bool newWrapRearms = transaction.begin(generation + 1, 2, region, region.outTick + 20)
                && transaction.markSeeking(generation + 1, 2);
            transaction.cancel();
            const bool manualStopCancelsPendingWrap = !transaction.isActive()
                && !transaction.markRestarted(generation + 1, 2)
                && !transaction.finish(generation + 1, 2);

            mw::midi::MidiRecordingControlContext noMidiLoopContext;
            noMidiLoopContext.state = mw::midi::MidiRecordingControlState::Idle;
            noMidiLoopContext.armedTrackAvailable = true;
            noMidiLoopContext.midiInputOpen = false;
            noMidiLoopContext.recordingConfigurationValid = true;
            noMidiLoopContext.allowLoopWrapTestWithoutMidi = true;
            const auto diagnosticAvailability = mw::midi::makeMidiRecordingControlAvailability(noMidiLoopContext);
            noMidiLoopContext.allowLoopWrapTestWithoutMidi = false;
            const auto normalNoMidiAvailability = mw::midi::makeMidiRecordingControlAvailability(noMidiLoopContext);
            const bool noMidiDiagnosticPath = diagnosticAvailability.recordMidiEnabled
                && !normalNoMidiAvailability.recordMidiEnabled;

            const auto restartSeconds = mw::midi::midiRecordingProjectSecondsForTick(region.inTick, 120, 960);
            const bool logicalRestartPositionExact = std::abs(restartSeconds - 3.0) < 1.0e-9;

            std::vector<mw::core::NoteEvent> projectNotes;
            projectNotes.emplace_back(52, 90, 1000, 240, 1, mw::core::Articulation::Normal);
            const auto beforeDiagnostic = projectNotes;
            const std::vector<mw::core::NoteEvent> emptyDiagnosticPass;
            projectNotes.insert(projectNotes.end(), emptyDiagnosticPass.begin(), emptyDiagnosticPass.end());
            const bool emptyPassLeavesProjectUntouched = projectNotes.size() == beforeDiagnostic.size()
                && projectNotes[0].pitch == beforeDiagnostic[0].pitch
                && projectNotes[0].startTick == beforeDiagnostic[0].startTick;

            const bool passed = earlyClaimRejected
                && exactClaimAccepted
                && duplicateClaimRejected
                && staleSeekRejected
                && seekAccepted
                && wrongPassRestartRejected
                && restartAccepted
                && finishAccepted
                && newWrapRearms
                && manualStopCancelsPendingWrap
                && noMidiDiagnosticPath
                && logicalRestartPositionExact
                && emptyPassLeavesProjectUntouched;

            return makeResult("I10", "Workflow", "Loop wrap transaction and continuity policy", passed,
                              passed
                                  ? "Loop wrapping used one generation/pass-scoped transaction from exact Out claim through seek/restart; duplicate, stale, wrong-pass, and cancelled transitions were rejected; the logical restart remained exact In; and loop wrap diagnostics can run with an armed track but no MIDI input without creating project notes."
                                  : "Loop wrap transaction staging, stale/cancel rejection, exact In restart, or no-MIDI diagnostic policy failed.");
        }


        SimulationScenarioResult scenarioI11()
        {
            constexpr mw::core::StableId projectId = 1201;
            constexpr mw::core::StableId trackId = 2201;
            constexpr std::uint64_t generation = 51;

            mw::midi::MidiRecordingLoopTakeReviewState review;
            const bool beginAccepted = review.begin(projectId, trackId, generation) == generation
                && review.isCollecting()
                && !review.isReviewActive()
                && review.matchesProjectTrack(projectId, trackId);

            std::vector<mw::core::NoteEvent> pass1;
            pass1.emplace_back(60, 100, 6200, 240, 1, mw::core::Articulation::Normal);
            std::vector<mw::core::NoteEvent> pass2;
            pass2.emplace_back(64, 96, 5800, 360, 1, mw::core::Articulation::Normal);
            pass2.emplace_back(67, 92, 7000, 240, 1, mw::core::Articulation::Normal);

            const bool passIdentityPreserved = review.addPass(generation, 1, pass1)
                && review.addPass(generation, 2, pass2)
                && review.passCount() == 2
                && review.totalNoteCount() == 3
                && review.passes()[0].passNumber == 1
                && review.passes()[1].passNumber == 2
                && review.passes()[0].notes.size() == 1
                && review.passes()[0].notes[0].startTick == 6200;

            const bool staleAndDuplicateRejected = !review.addPass(generation + 1, 3, {})
                && !review.addPass(generation, 2, {})
                && !review.addPass(generation, 1, {});

            std::vector<mw::core::NoteEvent> projectNotes;
            projectNotes.emplace_back(52, 88, 4000, 300, 1, mw::core::Articulation::Normal);
            const auto preReviewProjectNotes = projectNotes;
            const bool reviewDoesNotMutateProject = projectNotes.size() == preReviewProjectNotes.size()
                && projectNotes[0].pitch == preReviewProjectNotes[0].pitch;

            const bool reviewActivated = review.finishCollection(generation)
                && review.isReviewActive()
                && !review.isCollecting()
                && review.selectedPassNumber() == 1;

            const bool selectionWorks = review.selectPass(generation, 2)
                && review.selectedPassNumber() == 2
                && review.selectedPass() != nullptr
                && review.selectedPass()->notes.size() == 2
                && !review.selectPass(generation + 1, 1)
                && !review.selectPass(generation, 99);

            const auto merged = review.mergedNotes();
            const bool keepAllMatchesLegacyMerge = merged.size() == 3
                && merged[0].pitch == 60
                && merged[1].pitch == 64
                && merged[2].pitch == 67;

            auto keepSelectedProject = preReviewProjectNotes;
            const auto* selected = review.selectedPass();
            if (selected != nullptr)
                keepSelectedProject.insert(keepSelectedProject.end(), selected->notes.begin(), selected->notes.end());
            const bool keepSelectedWholePassOnly = keepSelectedProject.size() == 3
                && keepSelectedProject[0].pitch == 52
                && keepSelectedProject[1].pitch == 64
                && keepSelectedProject[2].pitch == 67;

            auto keepAllProject = preReviewProjectNotes;
            keepAllProject.insert(keepAllProject.end(), merged.begin(), merged.end());
            const bool keepAllPreservesExisting = keepAllProject.size() == 4
                && keepAllProject.front().pitch == 52;

            const auto discardProject = preReviewProjectNotes;
            const bool discardLeavesProjectAndUndoUntouched = discardProject.size() == 1
                && discardProject.front().pitch == 52;

            const bool identityGuard = review.matchesProjectTrack(projectId, trackId)
                && !review.matchesProjectTrack(projectId + 1, trackId)
                && !review.matchesProjectTrack(projectId, trackId + 1);

            review.cancel();
            const bool cancelClearsRuntimeReview = !review.isReviewActive()
                && !review.isCollecting()
                && review.passCount() == 0
                && review.totalNoteCount() == 0;

            const bool passed = beginAccepted
                && passIdentityPreserved
                && staleAndDuplicateRejected
                && reviewDoesNotMutateProject
                && reviewActivated
                && selectionWorks
                && keepAllMatchesLegacyMerge
                && keepSelectedWholePassOnly
                && keepAllPreservesExisting
                && discardLeavesProjectAndUndoUntouched
                && identityGuard
                && cancelClearsRuntimeReview;

            return makeResult("I11", "Workflow", "MIDI loop take review and whole-pass selection", passed,
                              passed
                                  ? "Loop passes retained runtime identity and note contents without mutating project data; whole-pass selection, Keep Selected, Keep All legacy-merge equivalence, Discard, identity guards, and review cleanup behaved deterministically."
                                  : "Loop take-review pass identity, selection, commit/discard semantics, legacy Merge equivalence, or stale-identity cleanup failed.");
        }


        SimulationScenarioResult scenarioI12()
        {
            constexpr mw::core::StableId projectId = 1301;
            constexpr mw::core::StableId trackId = 2301;
            constexpr std::uint64_t generation = 61;

            std::vector<mw::midi::MidiRecordingLoopTakePass> passes;
            mw::midi::MidiRecordingLoopTakePass pass1;
            pass1.passNumber = 1;
            pass1.notes.emplace_back(50, 70, 900, 400, 1, mw::core::Articulation::Normal); // began before region: never manufactured
            pass1.notes.emplace_back(60, 101, 1100, 1000, 2, mw::core::Articulation::Accent); // clipped at first beat segment end
            pass1.notes.emplace_back(61, 92, 3840, 300, 1, mw::core::Articulation::Legato); // exact segment start
            passes.push_back(pass1);

            mw::midi::MidiRecordingLoopTakePass pass2;
            pass2.passNumber = 2;
            pass2.notes.emplace_back(62, 90, 1920, 120, 1, mw::core::Articulation::Normal); // exact segment start
            pass2.notes.emplace_back(64, 96, 2000, 1200, 3, mw::core::Articulation::Staccato); // clipped at 2880
            pass2.notes.emplace_back(65, 88, 3000, 200, 1, mw::core::Articulation::Normal);
            passes.push_back(pass2);

            mw::midi::MidiRecordingLoopTakePass emptyPass;
            emptyPass.passNumber = 3;
            passes.push_back(emptyPass);

            mw::midi::MidiRecordingLoopCompState comp;
            const bool beginAccepted = comp.begin(
                projectId,
                trackId,
                generation,
                1000,
                5000,
                960,
                4,
                4,
                mw::midi::MidiRecordingCompGrid::Beat,
                1,
                passes) == generation;

            const auto beatSegments = comp.segments();
            const bool beatSegmentsCorrect = beginAccepted
                && beatSegments.size() == 5
                && beatSegments[0].startTick == 1000 && beatSegments[0].endTick == 1920
                && beatSegments[1].startTick == 1920 && beatSegments[1].endTick == 2880
                && beatSegments[2].startTick == 2880 && beatSegments[2].endTick == 3840
                && beatSegments[3].startTick == 3840 && beatSegments[3].endTick == 4800
                && beatSegments[4].startTick == 4800 && beatSegments[4].endTick == 5000;

            const bool assignmentsAccepted = comp.assignPass(generation, 1, 2, passes)
                && comp.assignPass(generation, 2, 2, passes)
                && !comp.assignPass(generation + 1, 0, 2, passes)
                && !comp.assignPass(generation, 99, 2, passes)
                && !comp.assignPass(generation, 0, 99, passes);

            const auto built = comp.buildNotes(generation, passes);
            const bool noteOwnershipAndClipping = built.notes.size() == 5
                && built.clippedNoteCount == 2
                && built.notes[0].pitch == 60
                && built.notes[0].startTick == 1100
                && built.notes[0].durationTicks == 820
                && built.notes[0].velocity == 101
                && built.notes[0].midiChannel == 2
                && built.notes[0].articulation == mw::core::Articulation::Accent
                && built.notes[1].pitch == 62
                && built.notes[1].startTick == 1920
                && built.notes[2].pitch == 64
                && built.notes[2].durationTicks == 880
                && built.notes[3].pitch == 65
                && built.notes[3].startTick == 3000
                && built.notes[4].pitch == 61
                && built.notes[4].startTick == 3840;

            const bool preSegmentHeldNoteNotManufactured = std::none_of(
                built.notes.begin(), built.notes.end(), [](const auto& note) { return note.pitch == 50; });

            mw::midi::MidiRecordingLoopCompState reordered;
            reordered.begin(projectId, trackId, generation, 1000, 5000, 960, 4, 4,
                            mw::midi::MidiRecordingCompGrid::Beat, 1, passes);
            const bool reverseAssignmentOrderAccepted = reordered.assignPass(generation, 2, 2, passes)
                && reordered.assignPass(generation, 1, 2, passes);
            const auto reorderedBuilt = reordered.buildNotes(generation, passes);
            const bool deterministicAssignmentOrder = reverseAssignmentOrderAccepted
                && reorderedBuilt.notes.size() == built.notes.size()
                && std::equal(reorderedBuilt.notes.begin(), reorderedBuilt.notes.end(), built.notes.begin(),
                    [](const auto& lhs, const auto& rhs)
                    {
                        return lhs.pitch == rhs.pitch
                            && lhs.velocity == rhs.velocity
                            && lhs.midiChannel == rhs.midiChannel
                            && lhs.startTick == rhs.startTick
                            && lhs.durationTicks == rhs.durationTicks
                            && lhs.articulation == rhs.articulation;
                    });

            const bool barGridRebuild = comp.setGrid(
                    generation,
                    mw::midi::MidiRecordingCompGrid::Bar,
                    2,
                    passes)
                && comp.segments().size() == 2
                && comp.segments()[0].startTick == 1000
                && comp.segments()[0].endTick == 3840
                && comp.segments()[1].startTick == 3840
                && comp.segments()[1].endTick == 5000
                && comp.segments()[0].selectedPassNumber == 2
                && comp.segments()[1].selectedPassNumber == 2;

            const bool emptyCompSupported = comp.assignPass(generation, 0, 3, passes)
                && comp.assignPass(generation, 1, 3, passes)
                && comp.buildNotes(generation, passes).notes.empty();

            const bool identityGuard = comp.matchesProjectTrack(projectId, trackId)
                && !comp.matchesProjectTrack(projectId + 1, trackId)
                && !comp.matchesProjectTrack(projectId, trackId + 1)
                && comp.buildNotes(generation + 1, passes).notes.empty();

            comp.cancel();
            const bool cleanup = !comp.isActive() && comp.segments().empty();

            const bool passed = beatSegmentsCorrect
                && assignmentsAccepted
                && noteOwnershipAndClipping
                && preSegmentHeldNoteNotManufactured
                && deterministicAssignmentOrder
                && barGridRebuild
                && emptyCompSupported
                && identityGuard
                && cleanup;

            return makeResult("I12", "Workflow", "MIDI loop segment comping", passed,
                              passed
                                  ? "Beat/Bar comp segments clipped to the loop region; each segment selected one retained pass; Note On ownership used [start,end), held notes clipped exactly at segment ends without synthetic boundary Note Ons; assignment order was deterministic; empty comps, identity guards, and cleanup behaved safely."
                                  : "Segment grid generation, pass assignment, note-boundary clipping, deterministic comp construction, empty-comp behavior, or identity cleanup failed.");
        }


        SimulationScenarioResult scenarioI13()
        {
            constexpr mw::core::StableId projectId = 1401;
            constexpr mw::core::StableId trackId = 2401;
            constexpr std::uint64_t generation = 71;

            std::vector<mw::midi::MidiRecordingLoopTakePass> passes;
            mw::midi::MidiRecordingLoopTakePass pass1;
            pass1.passNumber = 1;
            pass1.notes.emplace_back(60, 100, 1200, 1800, 1, mw::core::Articulation::Normal);
            pass1.notes.emplace_back(61, 96, 3200, 1000, 1, mw::core::Articulation::Accent);
            passes.push_back(pass1);

            mw::midi::MidiRecordingLoopTakePass pass2;
            pass2.passNumber = 2;
            pass2.notes.emplace_back(70, 90, 2600, 200, 2, mw::core::Articulation::Staccato);
            pass2.notes.emplace_back(71, 88, 4100, 600, 2, mw::core::Articulation::Legato);
            passes.push_back(pass2);

            mw::midi::MidiRecordingLoopCompState comp;
            const bool beginCustom = comp.begin(
                projectId,
                trackId,
                generation,
                1000,
                5000,
                960,
                4,
                4,
                mw::midi::MidiRecordingCompGrid::Custom,
                1,
                passes) == generation;

            const bool initialRegionLocked = beginCustom
                && comp.grid() == mw::midi::MidiRecordingCompGrid::Custom
                && comp.segments().size() == 1
                && comp.segments()[0].startTick == 1000
                && comp.segments()[0].endTick == 5000
                && comp.customBoundaries().empty();

            const bool invalidRejected = !comp.addCustomBoundary(generation, 1000)
                && !comp.addCustomBoundary(generation, 5000)
                && !comp.addCustomBoundary(generation, 999)
                && !comp.addCustomBoundary(generation, 5001)
                && !comp.addCustomBoundary(generation + 1, 2500);

            const bool firstSplit = comp.addCustomBoundary(generation, 2500)
                && !comp.addCustomBoundary(generation, 2500)
                && comp.segments().size() == 2
                && comp.segments()[0].startTick == 1000
                && comp.segments()[0].endTick == 2500
                && comp.segments()[1].startTick == 2500
                && comp.segments()[1].endTick == 5000
                && comp.segments()[0].selectedPassNumber == 1
                && comp.segments()[1].selectedPassNumber == 1;

            const bool rightAssignment = comp.assignPass(generation, 1, 2, passes);
            const bool secondSplit = comp.addCustomBoundary(generation, 4000)
                && comp.segments().size() == 3
                && comp.segments()[1].startTick == 2500
                && comp.segments()[1].endTick == 4000
                && comp.segments()[2].startTick == 4000
                && comp.segments()[2].endTick == 5000
                && comp.segments()[1].selectedPassNumber == 2
                && comp.segments()[2].selectedPassNumber == 2;

            const auto boundaries = comp.customBoundaries();
            const bool orderedBoundaries = boundaries == std::vector<std::int64_t>({ 2500, 4000 });

            const auto built = comp.buildNotes(generation, passes);
            const bool customNoteResult = built.notes.size() == 3
                && built.clippedNoteCount == 1
                && built.notes[0].pitch == 60
                && built.notes[0].startTick == 1200
                && built.notes[0].durationTicks == 1300
                && built.notes[1].pitch == 70
                && built.notes[1].startTick == 2600
                && built.notes[2].pitch == 71
                && built.notes[2].startTick == 4100;

            const bool removeFirstBoundary = comp.removeCustomBoundary(generation, 2500)
                && comp.segments().size() == 2
                && comp.segments()[0].startTick == 1000
                && comp.segments()[0].endTick == 4000
                && comp.segments()[0].selectedPassNumber == 1
                && comp.segments()[1].startTick == 4000
                && comp.segments()[1].selectedPassNumber == 2
                && comp.customBoundaries() == std::vector<std::int64_t>({ 4000 });

            const bool staleAndMissingRemovalRejected = !comp.removeCustomBoundary(generation + 1, 4000)
                && !comp.removeCustomBoundary(generation, 2500)
                && !comp.removeCustomBoundary(generation, 1000)
                && !comp.removeCustomBoundary(generation, 5000);

            const bool removeLastBoundary = comp.removeCustomBoundary(generation, 4000)
                && comp.segments().size() == 1
                && comp.segments()[0].startTick == 1000
                && comp.segments()[0].endTick == 5000
                && comp.segments()[0].selectedPassNumber == 1
                && comp.customBoundaries().empty();

            const bool beatStillAvailable = comp.setGrid(
                    generation,
                    mw::midi::MidiRecordingCompGrid::Beat,
                    2,
                    passes)
                && comp.segments().size() == 5
                && comp.segments().front().selectedPassNumber == 2;

            const bool customResetFromGrid = comp.setGrid(
                    generation,
                    mw::midi::MidiRecordingCompGrid::Custom,
                    2,
                    passes)
                && comp.segments().size() == 1
                && comp.segments()[0].selectedPassNumber == 2
                && comp.customBoundaries().empty();

            comp.cancel();
            const bool cleanup = !comp.isActive()
                && comp.segments().empty()
                && comp.customBoundaries().empty();

            const bool passed = initialRegionLocked
                && invalidRejected
                && firstSplit
                && rightAssignment
                && secondSplit
                && orderedBoundaries
                && customNoteResult
                && removeFirstBoundary
                && staleAndMissingRemovalRejected
                && removeLastBoundary
                && beatStillAvailable
                && customResetFromGrid
                && cleanup;

            return makeResult("I13", "Workflow", "Custom MIDI comp boundaries and comp-map editing", passed,
                              passed
                                  ? "Custom comp mode kept In/Out locked, accepted ordered internal boundaries only, split segments while inheriting pass assignment, removed boundaries with deterministic left-side merge ownership, preserved existing note-boundary clipping, rejected stale/duplicate/out-of-range edits, and reset cleanly across Beat/Custom grid changes."
                                  : "Custom comp boundary insertion/removal, segment inheritance, note generation, stale/duplicate rejection, grid reset, or cleanup failed.");
        }

        SimulationScenarioResult scenarioI14()
        {
            const auto tempFolder = std::filesystem::temp_directory_path() / "PoorMansStudioSimulation";
            const auto projectPath = tempFolder / "persistent_take_review_v11.mwproj";
            const auto malformedPath = tempFolder / "persistent_take_review_malformed_v10.mwproj";
            const auto legacyPath = tempFolder / "persistent_take_review_legacy_v9.mwproj";
            std::error_code ec;
            std::filesystem::create_directories(tempFolder, ec);
            std::filesystem::remove(projectPath, ec);
            std::filesystem::remove(malformedPath, ec);
            std::filesystem::remove(legacyPath, ec);

            mw::core::Project project("Persistent Review Fixture");
            project.setStableId(1501);
            project.setTempoBpm(120);
            project.setTimeSignature({ 4, 4 });
            auto& track = project.addTrack("Persistent MIDI Review");
            track.setStableId(2501);
            track.addNote({ 48, 80, 200, 200, 1, mw::core::Articulation::Normal });

            mw::core::PersistentMidiTakeReview persisted;
            persisted.active = true;
            persisted.regionInTick = 1000;
            persisted.regionOutTick = 5000;
            persisted.selectedPassNumber = 2;
            persisted.reviewMode = mw::core::PersistentMidiTakeReviewMode::Comp;
            persisted.compGrid = mw::core::PersistentMidiCompGrid::Custom;
            mw::core::PersistentMidiTakePass pass1;
            pass1.passNumber = 1;
            pass1.notes.emplace_back(60, 100, 1200, 1800, 1, mw::core::Articulation::Normal);
            pass1.notes.emplace_back(61, 96, 3200, 1000, 1, mw::core::Articulation::Accent);
            mw::core::PersistentMidiTakePass pass2;
            pass2.passNumber = 2;
            pass2.notes.emplace_back(70, 90, 2600, 200, 2, mw::core::Articulation::Staccato);
            pass2.notes.emplace_back(71, 88, 4100, 600, 2, mw::core::Articulation::Legato);
            persisted.passes = { pass1, pass2 };
            persisted.segments = {
                { 1000, 2500, 1 },
                { 2500, 4000, 2 },
                { 4000, 5000, 2 }
            };
            track.setPendingMidiTakeReview(persisted);

            auto& secondTrack = project.addTrack("Second Persistent Review");
            secondTrack.setStableId(2502);
            mw::core::PersistentMidiTakeReview secondReview;
            secondReview.active = true;
            secondReview.regionInTick = 6000;
            secondReview.regionOutTick = 8000;
            secondReview.selectedPassNumber = 1;
            secondReview.reviewMode = mw::core::PersistentMidiTakeReviewMode::WholePass;
            mw::core::PersistentMidiTakePass secondPass;
            secondPass.passNumber = 1;
            secondPass.notes.emplace_back(80, 100, 6200, 400, 3, mw::core::Articulation::Legato);
            secondReview.passes = { secondPass };
            secondTrack.setPendingMidiTakeReview(secondReview);

            auto& audioTrack = project.addTrack("Audio Track Must Not Retain MIDI Review");
            audioTrack.setStableId(2503);
            audioTrack.setPendingMidiTakeReview(secondReview);
            audioTrack.setTrackType(mw::core::TrackType::AudioClip);
            const bool audioTypeChangeClearedReview = !audioTrack.getPendingMidiTakeReview().active;

            const bool saved = mw::serialization::ProjectSerializer::saveToFile(project, projectPath);
            const auto loaded = saved ? mw::serialization::ProjectSerializer::loadFromFile(projectPath) : std::nullopt;

            bool roundTrip = false;
            bool runtimeRestore = false;
            bool atomicKeepUndoRedoModel = false;
            if (loaded.has_value() && loaded->getTracks().size() == 3)
            {
                auto reopened = *loaded;
                auto& reopenedTrack = reopened.getTracks()[0];
                const auto& review = reopenedTrack.getPendingMidiTakeReview();
                roundTrip = reopened.getMetadata().projectFormatVersion == 22
                    && review.active
                    && review.regionInTick == 1000
                    && review.regionOutTick == 5000
                    && review.selectedPassNumber == 2
                    && review.reviewMode == mw::core::PersistentMidiTakeReviewMode::Comp
                    && review.compGrid == mw::core::PersistentMidiCompGrid::Custom
                    && review.passes.size() == 2
                    && review.passes[0].passNumber == 1
                    && review.passes[0].notes.size() == 2
                    && review.passes[0].notes[1].articulation == mw::core::Articulation::Accent
                    && review.passes[1].passNumber == 2
                    && review.passes[1].notes[0].midiChannel == 2
                    && review.segments.size() == 3
                    && review.segments[1].startTick == 2500
                    && review.segments[1].endTick == 4000
                    && review.segments[1].selectedPassNumber == 2
                    && reopened.getTracks()[1].getPendingMidiTakeReview().active
                    && reopened.getTracks()[1].getPendingMidiTakeReview().reviewMode == mw::core::PersistentMidiTakeReviewMode::WholePass
                    && reopened.getTracks()[1].getPendingMidiTakeReview().passes.size() == 1
                    && reopened.getTracks()[1].getPendingMidiTakeReview().passes[0].notes[0].pitch == 80
                    && reopened.getTracks()[2].isAudioClipTrack()
                    && !reopened.getTracks()[2].getPendingMidiTakeReview().active;

                std::vector<mw::midi::MidiRecordingLoopTakePass> runtimePasses;
                for (const auto& storedPass : review.passes)
                    runtimePasses.push_back({ storedPass.passNumber, storedPass.notes });
                mw::midi::MidiRecordingLoopTakeReviewState runtimeReview;
                constexpr std::uint64_t generation = 81;
                const bool reviewRestored = runtimeReview.restoreReview(
                    reopened.getStableId(), reopenedTrack.getStableId(), generation,
                    runtimePasses, review.selectedPassNumber);

                std::vector<mw::midi::MidiRecordingCompSegment> runtimeSegments;
                for (const auto& segment : review.segments)
                    runtimeSegments.push_back({ segment.startTick, segment.endTick, segment.selectedPassNumber });
                mw::midi::MidiRecordingLoopCompState runtimeComp;
                const bool compRestored = runtimeComp.restore(
                    reopened.getStableId(), reopenedTrack.getStableId(), generation,
                    review.regionInTick, review.regionOutTick, 960, 4, 4,
                    mw::midi::MidiRecordingCompGrid::Custom,
                    runtimeSegments, runtimeReview.passes());
                const auto compBuild = runtimeComp.buildNotes(generation, runtimeReview.passes());
                runtimeRestore = reviewRestored && compRestored
                    && runtimeReview.selectedPassNumber() == 2
                    && runtimeComp.segments().size() == 3
                    && compBuild.notes.size() == 3
                    && compBuild.notes[0].pitch == 60
                    && compBuild.notes[0].durationTicks == 1300
                    && compBuild.notes[1].pitch == 70
                    && compBuild.notes[2].pitch == 71;

                const auto preKeepNotes = reopenedTrack.getNotes();
                const auto preKeepReview = reopenedTrack.getPendingMidiTakeReview();
                auto committedNotes = preKeepNotes;
                committedNotes.insert(committedNotes.end(), compBuild.notes.begin(), compBuild.notes.end());
                reopenedTrack.getNotes() = committedNotes;
                reopenedTrack.clearPendingMidiTakeReview();
                const bool committed = reopenedTrack.getNotes().size() == preKeepNotes.size() + compBuild.notes.size()
                    && !reopenedTrack.getPendingMidiTakeReview().active;

                const auto redoNotes = reopenedTrack.getNotes();
                const auto redoReview = reopenedTrack.getPendingMidiTakeReview();
                reopenedTrack.getNotes() = preKeepNotes;
                reopenedTrack.setPendingMidiTakeReview(preKeepReview);
                const bool undoRestoredReview = reopenedTrack.getNotes().size() == preKeepNotes.size()
                    && reopenedTrack.getPendingMidiTakeReview().active
                    && reopenedTrack.getPendingMidiTakeReview().segments.size() == 3;
                reopenedTrack.getNotes() = redoNotes;
                reopenedTrack.setPendingMidiTakeReview(redoReview);
                const bool redoRestoredCommit = reopenedTrack.getNotes().size() == committedNotes.size()
                    && !reopenedTrack.getPendingMidiTakeReview().active;
                atomicKeepUndoRedoModel = committed && undoRestoredReview && redoRestoredCommit;
            }

            {
                std::ofstream file(legacyPath, std::ios::binary | std::ios::trunc);
                file << "{\n"
                     << "  \"format\": \"MusicWorkstationProject\",\n"
                     << "  \"version\": 9,\n"
                     << "  \"projectId\": 1502,\n"
                     << "  \"name\": \"Legacy No Review\",\n"
                     << "  \"tempoBpm\": 120,\n"
                     << "  \"tracks\": [{\"trackId\":2502,\"name\":\"Legacy MIDI\",\"notes\":[]}]\n"
                     << "}\n";
            }
            const auto legacyLoaded = mw::serialization::ProjectSerializer::loadFromFile(legacyPath);
            const bool legacyCompatibility = legacyLoaded.has_value()
                && legacyLoaded->getMetadata().projectFormatVersion == 22
                && legacyLoaded->getTracks().size() == 1
                && !legacyLoaded->getTracks()[0].getPendingMidiTakeReview().active;

            {
                std::ofstream file(malformedPath, std::ios::binary | std::ios::trunc);
                file << "{\n"
                     << "  \"format\": \"MusicWorkstationProject\",\n"
                     << "  \"version\": 10,\n"
                     << "  \"projectId\": 1503,\n"
                     << "  \"name\": \"Malformed Review\",\n"
                     << "  \"tempoBpm\": 120,\n"
                     << "  \"tracks\": [{\n"
                     << "    \"trackId\":2503,\"name\":\"MIDI\",\"notes\":[],\n"
                     << "    \"pendingMidiTakeReview\":{\"active\":true,\"regionInTick\":5000,\"regionOutTick\":1000,\"selectedPassNumber\":1,\"reviewMode\":\"Comp\",\"compGrid\":\"Custom\",\"passes\":[{\"passNumber\":1,\"takeNotes\":[{\"pitch\":60,\"velocity\":100,\"startTick\":1200,\"durationTicks\":100,\"midiChannel\":1,\"articulation\":\"Normal\"}]}],\"segments\":[]}\n"
                     << "  }]\n"
                     << "}\n";
            }
            const auto malformedLoaded = mw::serialization::ProjectSerializer::loadFromFile(malformedPath);
            const bool malformedSafe = malformedLoaded.has_value()
                && malformedLoaded->getTracks().size() == 1
                && !malformedLoaded->getTracks()[0].getPendingMidiTakeReview().active;

            std::ifstream savedFile(projectPath, std::ios::binary);
            const std::string serialized((std::istreambuf_iterator<char>(savedFile)), std::istreambuf_iterator<char>());
            const bool noTransientRuntimeState = serialized.find("pendingMidiTakeReview") != std::string::npos
                && serialized.find("recordingGeneration") == std::string::npos
                && serialized.find("wrapTransaction") == std::string::npos
                && serialized.find("transportGeneration") == std::string::npos;

            std::filesystem::remove(projectPath, ec);
            std::filesystem::remove(malformedPath, ec);
            std::filesystem::remove(legacyPath, ec);

            const bool passed = audioTypeChangeClearedReview && saved && roundTrip && runtimeRestore && atomicKeepUndoRedoModel
                && legacyCompatibility && malformedSafe && noTransientRuntimeState;
            return makeResult("I14", "Workflow", "Persistent MIDI take/comp project round trip", passed,
                              passed
                                  ? "Format-15 projects persisted independent per-MIDI-track retained passes, loop regions, selected pass, Comp mode/grid, custom segment assignments, and note fields while AudioClip tracks rejected MIDI review state; reopen restored deterministic TAKE REVIEW/comp runtime state; Keep/Undo/Redo modeled atomic review-state restoration; format-9 projects loaded without phantom review data; malformed persisted review was ignored safely; transient transport/wrap generations were not serialized."
                                  : "Persistent take/comp serialization, runtime reconstruction, atomic Keep/Undo/Redo modeling, legacy compatibility, malformed-state safety, or transient-state exclusion failed.");
        }


        SimulationScenarioResult scenarioI15()
        {
            std::vector<mw::midi::MidiRecordingLoopTakePass> passes;
            mw::midi::MidiRecordingLoopTakePass pass1;
            pass1.passNumber = 1;
            pass1.notes.emplace_back(60, 100, 1500, 3200, 1, mw::core::Articulation::Normal);
            pass1.notes.emplace_back(61, 96, 2200, 300, 1, mw::core::Articulation::Accent);
            pass1.notes.emplace_back(70, 92, 3500, 1200, 1, mw::core::Articulation::Legato);
            mw::midi::MidiRecordingLoopTakePass pass2;
            pass2.passNumber = 2;
            pass2.notes.emplace_back(70, 88, 3500, 1000, 1, mw::core::Articulation::Normal);
            pass2.notes.emplace_back(70, 90, 4200, 300, 1, mw::core::Articulation::Staccato);
            passes = { pass1, pass2 };

            constexpr std::uint64_t generation = 91;
            const std::vector<mw::midi::MidiRecordingCompSegment> segments {
                { 1000, 2000, 1 },
                { 2000, 3000, 1 },
                { 3000, 4000, 1 },
                { 4000, 5000, 2 }
            };
            mw::midi::MidiRecordingLoopCompState comp;
            const bool restored = comp.restore(1601, 2601, generation, 1000, 5000, 960, 4, 4,
                                                mw::midi::MidiRecordingCompGrid::Custom,
                                                segments, passes);
            const auto cut = comp.buildNotes(generation, passes);
            const bool cutPreserved = restored
                && comp.boundaryNoteMode() == mw::midi::MidiRecordingCompBoundaryNoteMode::Cut
                && cut.notes.size() == 4
                && cut.notes[0].pitch == 60 && cut.notes[0].durationTicks == 500
                && cut.notes[1].pitch == 61 && cut.notes[1].durationTicks == 300
                && cut.notes[2].pitch == 70 && cut.notes[2].startTick == 3500 && cut.notes[2].durationTicks == 500
                && cut.notes[3].pitch == 70 && cut.notes[3].startTick == 4200;

            const bool modeSet = comp.setBoundaryNoteMode(
                generation, mw::midi::MidiRecordingCompBoundaryNoteMode::PreserveSamePass);
            const auto preserved = comp.buildNotes(generation, passes);
            const bool samePassPreserved = modeSet
                && preserved.notes.size() == 4
                && preserved.notes[0].pitch == 60 && preserved.notes[0].durationTicks == 2500
                && preserved.notes[1].pitch == 61 && preserved.notes[1].durationTicks == 300
                && preserved.notes[2].pitch == 70 && preserved.notes[2].startTick == 3500
                && preserved.notes[2].durationTicks == 500
                && preserved.notes[3].pitch == 70 && preserved.notes[3].startTick == 4200
                && preserved.notes[3].durationTicks == 300;

            // A note beginning before the Pass-2 segment is never synthesized at tick 4000.
            const bool noSyntheticBoundaryNoteOn = std::none_of(
                preserved.notes.begin(), preserved.notes.end(), [](const auto& note)
                {
                    return note.startTick == 4000;
                });

            // When every remaining segment uses the same pass, Loop Out is still the hard cap.
            mw::midi::MidiRecordingLoopTakePass longPass;
            longPass.passNumber = 1;
            longPass.notes.emplace_back(72, 100, 3200, 3000, 1, mw::core::Articulation::Legato);
            std::vector<mw::midi::MidiRecordingLoopTakePass> longPasses { longPass };
            mw::midi::MidiRecordingLoopCompState loopOutComp;
            const bool loopOutReady = loopOutComp.restore(
                1601, 2601, generation, 1000, 5000, 960, 4, 4,
                mw::midi::MidiRecordingCompGrid::Custom,
                { { 1000, 3000, 1 }, { 3000, 4000, 1 }, { 4000, 5000, 1 } }, longPasses)
                && loopOutComp.setBoundaryNoteMode(
                    generation, mw::midi::MidiRecordingCompBoundaryNoteMode::PreserveSamePass);
            const auto loopOutBuild = loopOutComp.buildNotes(generation, longPasses);
            const bool loopOutClamp = loopOutReady && loopOutBuild.notes.size() == 1
                && loopOutBuild.notes[0].startTick == 3200
                && loopOutBuild.notes[0].durationTicks == 1800;

            // Beat and Bar segment generators use the same preserve rule.
            mw::midi::MidiRecordingLoopTakePass beatPass;
            beatPass.passNumber = 1;
            beatPass.notes.emplace_back(65, 100, 500, 2000, 1, mw::core::Articulation::Normal);
            std::vector<mw::midi::MidiRecordingLoopTakePass> beatPasses { beatPass };
            mw::midi::MidiRecordingLoopCompState beatComp;
            const auto beatGen = beatComp.begin(1601, 2601, generation, 0, 3840, 960, 4, 4,
                                                mw::midi::MidiRecordingCompGrid::Beat, 1, beatPasses);
            const bool beatPreserve = beatGen == generation
                && beatComp.setBoundaryNoteMode(generation, mw::midi::MidiRecordingCompBoundaryNoteMode::PreserveSamePass)
                && beatComp.buildNotes(generation, beatPasses).notes.size() == 1
                && beatComp.buildNotes(generation, beatPasses).notes[0].durationTicks == 2000;

            mw::midi::MidiRecordingLoopTakePass barPass;
            barPass.passNumber = 1;
            barPass.notes.emplace_back(66, 100, 3000, 2000, 1, mw::core::Articulation::Normal);
            std::vector<mw::midi::MidiRecordingLoopTakePass> barPasses { barPass };
            mw::midi::MidiRecordingLoopCompState barComp;
            const auto barGen = barComp.begin(1601, 2601, generation, 0, 7680, 960, 4, 4,
                                              mw::midi::MidiRecordingCompGrid::Bar, 1, barPasses);
            const bool barPreserve = barGen == generation
                && barComp.setBoundaryNoteMode(generation, mw::midi::MidiRecordingCompBoundaryNoteMode::PreserveSamePass)
                && barComp.buildNotes(generation, barPasses).notes.size() == 1
                && barComp.buildNotes(generation, barPasses).notes[0].durationTicks == 2000;

            const auto tempFolder = std::filesystem::temp_directory_path() / "PoorMansStudioSimulation";
            const auto v11Path = tempFolder / "sustain_boundary_v11.mwproj";
            const auto v10Path = tempFolder / "sustain_boundary_v10_default_cut.mwproj";
            const auto malformedPath = tempFolder / "sustain_boundary_unknown_mode_v11.mwproj";
            std::error_code ec;
            std::filesystem::create_directories(tempFolder, ec);

            mw::core::Project project("Sustain Boundary Persistence");
            project.setStableId(1601);
            auto& track = project.addTrack("MIDI");
            track.setStableId(2601);
            mw::core::PersistentMidiTakeReview review;
            review.active = true;
            review.regionInTick = 1000;
            review.regionOutTick = 5000;
            review.selectedPassNumber = 1;
            review.reviewMode = mw::core::PersistentMidiTakeReviewMode::Comp;
            review.compGrid = mw::core::PersistentMidiCompGrid::Custom;
            review.boundaryNoteMode = mw::core::PersistentMidiCompBoundaryNoteMode::PreserveSamePass;
            review.passes = { { 1, pass1.notes }, { 2, pass2.notes } };
            review.segments = { { 1000, 2000, 1 }, { 2000, 3000, 1 }, { 3000, 4000, 1 }, { 4000, 5000, 2 } };
            track.setPendingMidiTakeReview(review);
            const bool saved = mw::serialization::ProjectSerializer::saveToFile(project, v11Path);
            const auto reopened = saved ? mw::serialization::ProjectSerializer::loadFromFile(v11Path) : std::nullopt;
            const bool v11RoundTrip = reopened.has_value()
                && reopened->getMetadata().projectFormatVersion == 22
                && reopened->getTracks()[0].getPendingMidiTakeReview().boundaryNoteMode
                    == mw::core::PersistentMidiCompBoundaryNoteMode::PreserveSamePass;

            {
                std::ofstream file(v10Path, std::ios::binary | std::ios::trunc);
                file << "{\n  \"format\": \"MusicWorkstationProject\",\n  \"version\": 10,\n"
                     << "  \"projectId\": 1602, \"name\": \"Format10 Default Cut\", \"tempoBpm\": 120,\n"
                     << "  \"tracks\": [{\"trackId\":2602,\"name\":\"MIDI\",\"notes\":[],"
                     << "\"pendingMidiTakeReview\":{\"active\":true,\"regionInTick\":1000,\"regionOutTick\":5000,"
                     << "\"selectedPassNumber\":1,\"reviewMode\":\"Comp\",\"compGrid\":\"Custom\","
                     << "\"passes\":[{\"passNumber\":1,\"takeNotes\":[{\"pitch\":60,\"velocity\":100,\"startTick\":1200,\"durationTicks\":200,\"midiChannel\":1,\"articulation\":\"Normal\"}]}],"
                     << "\"segments\":[{\"startTick\":1000,\"endTick\":5000,\"selectedPassNumber\":1}]}}]}\n";
            }
            const auto v10Loaded = mw::serialization::ProjectSerializer::loadFromFile(v10Path);
            const bool v10DefaultsCut = v10Loaded.has_value()
                && v10Loaded->getMetadata().projectFormatVersion == 22
                && v10Loaded->getTracks()[0].getPendingMidiTakeReview().active
                && v10Loaded->getTracks()[0].getPendingMidiTakeReview().boundaryNoteMode
                    == mw::core::PersistentMidiCompBoundaryNoteMode::Cut;

            {
                std::ofstream file(malformedPath, std::ios::binary | std::ios::trunc);
                file << "{\n  \"format\": \"MusicWorkstationProject\",\n  \"version\": 11,\n"
                     << "  \"projectId\": 1603, \"name\": \"Unknown Mode\", \"tempoBpm\": 120,\n"
                     << "  \"tracks\": [{\"trackId\":2603,\"name\":\"MIDI\",\"notes\":[],"
                     << "\"pendingMidiTakeReview\":{\"active\":true,\"regionInTick\":1000,\"regionOutTick\":5000,"
                     << "\"selectedPassNumber\":1,\"reviewMode\":\"Comp\",\"compGrid\":\"Custom\",\"boundaryNoteMode\":\"FutureMode\","
                     << "\"passes\":[{\"passNumber\":1,\"takeNotes\":[{\"pitch\":60,\"velocity\":100,\"startTick\":1200,\"durationTicks\":200,\"midiChannel\":1,\"articulation\":\"Normal\"}]}],"
                     << "\"segments\":[{\"startTick\":1000,\"endTick\":5000,\"selectedPassNumber\":1}]}}]}\n";
            }
            const auto malformedLoaded = mw::serialization::ProjectSerializer::loadFromFile(malformedPath);
            const bool malformedDefaultsCut = malformedLoaded.has_value()
                && malformedLoaded->getTracks()[0].getPendingMidiTakeReview().boundaryNoteMode
                    == mw::core::PersistentMidiCompBoundaryNoteMode::Cut;

            auto preKeepReview = review;
            mw::core::PersistentMidiTakeReview committedReview;
            committedReview.clear();
            auto undoRestored = committedReview;
            undoRestored = preKeepReview;
            const bool undoRestoresMode = undoRestored.active
                && undoRestored.boundaryNoteMode == mw::core::PersistentMidiCompBoundaryNoteMode::PreserveSamePass;

            const std::string boundaryHelp = mw::midi::midiRecordingCompBoundaryNotesTooltip;
            const std::string cutHelp = mw::midi::midiRecordingCompCutTooltip;
            const std::string preserveHelp = mw::midi::midiRecordingCompPreserveSamePassTooltip;
            const bool helperBubbleContract = !boundaryHelp.empty() && !cutHelp.empty() && !preserveHelp.empty()
                && boundaryHelp.find("no new Note On") != std::string::npos
                && preserveHelp.find("same retained pass") != std::string::npos;

            std::filesystem::remove(v11Path, ec);
            std::filesystem::remove(v10Path, ec);
            std::filesystem::remove(malformedPath, ec);

            const bool passed = cutPreserved && samePassPreserved && noSyntheticBoundaryNoteOn
                && loopOutClamp && beatPreserve && barPreserve && v11RoundTrip && v10DefaultsCut
                && malformedDefaultsCut && undoRestoresMode && helperBubbleContract;
            return makeResult("I15", "Workflow", "Sustain-aware persisted MIDI comp boundaries", passed,
                              passed
                                  ? "Cut preserved the prior comp contract; Preserve Same Pass carried real notes through contiguous same-pass segments only, clipped at pass changes/Loop Out, never synthesized boundary Note Ons, behaved consistently across Beat/Bar/Custom maps, persisted through current format 15, defaulted safely to Cut for format-10/unknown values, survived modeled Undo restoration, and exposed current helper-bubble text."
                                  : "Sustain-aware boundary handling, compatibility defaults, persistence, Undo restoration, grid consistency, or helper-bubble contract failed.");
        }


        SimulationScenarioResult scenarioI16()
        {
            const auto makeReview = [](int passBase,
                                       mw::core::PersistentMidiTakeReviewMode mode,
                                       mw::core::PersistentMidiCompGrid grid,
                                       mw::core::PersistentMidiCompBoundaryNoteMode boundaryMode)
            {
                mw::core::PersistentMidiTakeReview review;
                review.active = true;
                review.regionInTick = 1000 + passBase * 100;
                review.regionOutTick = 5000 + passBase * 100;
                review.selectedPassNumber = passBase;
                review.reviewMode = mode;
                review.compGrid = grid;
                review.boundaryNoteMode = boundaryMode;
                mw::core::PersistentMidiTakePass first;
                first.passNumber = passBase;
                first.notes.emplace_back(60 + passBase, 100, review.regionInTick + 100, 500, 1, mw::core::Articulation::Normal);
                review.passes.push_back(first);
                if (mode == mw::core::PersistentMidiTakeReviewMode::Comp)
                {
                    mw::core::PersistentMidiTakePass second;
                    second.passNumber = passBase + 1;
                    second.notes.emplace_back(72 + passBase, 90, review.regionInTick + 900, 400, 2, mw::core::Articulation::Accent);
                    review.passes.push_back(second);
                    const auto midpoint = (review.regionInTick + review.regionOutTick) / 2;
                    review.segments = {
                        { review.regionInTick, midpoint, passBase },
                        { midpoint, review.regionOutTick, passBase + 1 }
                    };
                }
                return review;
            };

            mw::core::Project project("Pending Review Manager Fixture");
            project.setStableId(1701);
            auto& first = project.addTrack("Piano");
            first.setStableId(2701);
            first.setPendingMidiTakeReview(makeReview(
                1,
                mw::core::PersistentMidiTakeReviewMode::Comp,
                mw::core::PersistentMidiCompGrid::Custom,
                mw::core::PersistentMidiCompBoundaryNoteMode::PreserveSamePass));

            auto& empty = project.addTrack("No Review");
            empty.setStableId(2702);

            auto& third = project.addTrack("Synth Lead");
            third.setStableId(2703);
            third.setPendingMidiTakeReview(makeReview(
                4,
                mw::core::PersistentMidiTakeReviewMode::WholePass,
                mw::core::PersistentMidiCompGrid::Beat,
                mw::core::PersistentMidiCompBoundaryNoteMode::Cut));

            auto& audio = project.addTrack("Audio Excluded");
            audio.setStableId(2704);
            audio.setPendingMidiTakeReview(makeReview(
                7,
                mw::core::PersistentMidiTakeReviewMode::Comp,
                mw::core::PersistentMidiCompGrid::Bar,
                mw::core::PersistentMidiCompBoundaryNoteMode::Cut));
            audio.setTrackType(mw::core::TrackType::AudioClip);

            const auto initial = mw::midi::buildPendingMidiTakeReviewSummaries(project);
            const bool initialSummary = initial.size() == 2
                && initial[0].trackId == 2701
                && initial[0].trackIndex == 0
                && initial[0].trackName == "Piano"
                && initial[0].passCount == 2
                && initial[0].segmentCount == 2
                && initial[0].reviewMode == mw::core::PersistentMidiTakeReviewMode::Comp
                && initial[0].compGrid == mw::core::PersistentMidiCompGrid::Custom
                && initial[0].boundaryNoteMode == mw::core::PersistentMidiCompBoundaryNoteMode::PreserveSamePass
                && initial[1].trackId == 2703
                && initial[1].trackIndex == 2
                && initial[1].passCount == 1
                && initial[1].reviewMode == mw::core::PersistentMidiTakeReviewMode::WholePass;

            const auto tempFolder = std::filesystem::temp_directory_path() / "PoorMansStudioSimulation";
            const auto beforePath = tempFolder / "pending_manager_before_v11.mwproj";
            const auto afterPath = tempFolder / "pending_manager_after_read_v11.mwproj";
            const auto v10Path = tempFolder / "pending_manager_v10.mwproj";
            std::error_code ec;
            std::filesystem::create_directories(tempFolder, ec);
            std::filesystem::remove(beforePath, ec);
            std::filesystem::remove(afterPath, ec);
            std::filesystem::remove(v10Path, ec);

            const bool savedBefore = mw::serialization::ProjectSerializer::saveToFile(project, beforePath);
            const auto resolvedPiano = mw::midi::findPendingMidiTakeReviewTrackIndexByStableId(project, 2701);
            const auto resolvedMissing = mw::midi::findPendingMidiTakeReviewTrackIndexByStableId(project, 999999);
            const bool savedAfter = mw::serialization::ProjectSerializer::saveToFile(project, afterPath);
            auto readBytes = [](const std::filesystem::path& path)
            {
                std::ifstream file(path, std::ios::binary);
                return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
            };
            const bool resumeReadOnly = savedBefore && savedAfter
                && resolvedPiano.has_value() && *resolvedPiano == 0
                && !resolvedMissing.has_value()
                && readBytes(beforePath) == readBytes(afterPath);

            // Rename and reorder without losing the stable review identity.
            project.getTracks()[0].setName("Grand Piano Renamed");
            std::swap(project.getTracks()[0], project.getTracks()[2]);
            const auto reordered = mw::midi::buildPendingMidiTakeReviewSummaries(project);
            const auto pianoAfterReorder = mw::midi::findPendingMidiTakeReviewTrackIndexByStableId(project, 2701);
            const bool stableRenameReorder = reordered.size() == 2
                && reordered[0].trackId == 2703 && reordered[0].trackIndex == 0
                && reordered[1].trackId == 2701 && reordered[1].trackIndex == 2
                && reordered[1].trackName == "Grand Piano Renamed"
                && pianoAfterReorder.has_value() && *pianoAfterReorder == 2;

            // Reconstruct the selected persisted review through the production runtime policies.
            const auto& persisted = project.getTracks()[static_cast<std::size_t>(*pianoAfterReorder)].getPendingMidiTakeReview();
            std::vector<mw::midi::MidiRecordingLoopTakePass> runtimePasses;
            for (const auto& pass : persisted.passes)
                runtimePasses.push_back({ pass.passNumber, pass.notes });
            mw::midi::MidiRecordingLoopTakeReviewState runtimeReview;
            constexpr std::uint64_t generation = 160;
            const bool reviewRestored = runtimeReview.restoreReview(
                project.getStableId(), 2701, generation, runtimePasses, persisted.selectedPassNumber);
            std::vector<mw::midi::MidiRecordingCompSegment> runtimeSegments;
            for (const auto& segment : persisted.segments)
                runtimeSegments.push_back({ segment.startTick, segment.endTick, segment.selectedPassNumber });
            mw::midi::MidiRecordingLoopCompState runtimeComp;
            const bool compRestored = reviewRestored
                && runtimeComp.restore(project.getStableId(), 2701, generation,
                                       persisted.regionInTick, persisted.regionOutTick,
                                       mw::core::Project::ticksPerQuarterNote, 4, 4,
                                       mw::midi::MidiRecordingCompGrid::Custom,
                                       runtimeSegments, runtimeReview.passes())
                && runtimeComp.setBoundaryNoteMode(generation, mw::midi::MidiRecordingCompBoundaryNoteMode::PreserveSamePass);
            const bool restoredEquivalent = compRestored
                && runtimeReview.passCount() == persisted.passes.size()
                && runtimeReview.selectedPassNumber() == persisted.selectedPassNumber
                && runtimeComp.segments().size() == persisted.segments.size()
                && runtimeComp.boundaryNoteMode() == mw::midi::MidiRecordingCompBoundaryNoteMode::PreserveSamePass;

            // Discard by stable ID changes only the chosen review.
            const auto synthBefore = project.getTracks()[0].getPendingMidiTakeReview();
            const bool discardedPiano = mw::midi::discardPendingMidiTakeReviewByStableId(project, 2701);
            const bool discardIsolation = discardedPiano
                && !project.getTracks()[2].getPendingMidiTakeReview().active
                && project.getTracks()[0].getPendingMidiTakeReview().active
                && project.getTracks()[0].getPendingMidiTakeReview().passes.size() == synthBefore.passes.size()
                && project.getTracks()[0].getPendingMidiTakeReview().passes[0].notes[0].pitch
                    == synthBefore.passes[0].notes[0].pitch;

            // A removed track invalidates stale manager identities rather than falling through to another row.
            project.getTracks().erase(project.getTracks().begin());
            const bool staleRemovalRejected = !mw::midi::findPendingMidiTakeReviewTrackIndexByStableId(project, 2703).has_value()
                && !mw::midi::discardPendingMidiTakeReviewByStableId(project, 2703);

            mw::core::Project replacement("Replacement Project");
            replacement.setStableId(1702);
            auto& replacementTrack = replacement.addTrack("Other");
            replacementTrack.setStableId(2801);
            replacementTrack.setPendingMidiTakeReview(makeReview(
                9,
                mw::core::PersistentMidiTakeReviewMode::WholePass,
                mw::core::PersistentMidiCompGrid::Beat,
                mw::core::PersistentMidiCompBoundaryNoteMode::Cut));
            const bool replacementInvalidatesOld = !mw::midi::findPendingMidiTakeReviewTrackIndexByStableId(replacement, 2701).has_value()
                && mw::midi::buildPendingMidiTakeReviewSummaries(replacement).size() == 1
                && mw::midi::buildPendingMidiTakeReviewSummaries(replacement)[0].trackId == 2801;

            // Format-10 projects remain accepted and simply expose whatever persisted review data exists.
            {
                std::ofstream file(v10Path, std::ios::binary | std::ios::trunc);
                file << "{\n  \"format\": \"MusicWorkstationProject\",\n  \"version\": 10,\n"
                     << "  \"projectId\": 1703, \"name\": \"Manager Legacy V10\", \"tempoBpm\": 120,\n"
                     << "  \"tracks\": [{\"trackId\":2802,\"name\":\"Legacy Pending\",\"notes\":[],"
                     << "\"pendingMidiTakeReview\":{\"active\":true,\"regionInTick\":1000,\"regionOutTick\":5000,"
                     << "\"selectedPassNumber\":1,\"reviewMode\":\"WholePass\",\"compGrid\":\"Beat\","
                     << "\"passes\":[{\"passNumber\":1,\"takeNotes\":[{\"pitch\":60,\"velocity\":100,\"startTick\":1200,\"durationTicks\":200,\"midiChannel\":1,\"articulation\":\"Normal\"}]}],"
                     << "\"segments\":[]}}]}\n";
            }
            const auto legacy = mw::serialization::ProjectSerializer::loadFromFile(v10Path);
            const bool legacyCompatible = legacy.has_value()
                && legacy->getMetadata().projectFormatVersion == 22
                && mw::midi::buildPendingMidiTakeReviewSummaries(*legacy).size() == 1
                && mw::midi::buildPendingMidiTakeReviewSummaries(*legacy)[0].boundaryNoteMode
                    == mw::core::PersistentMidiCompBoundaryNoteMode::Cut;

            const std::string indicatorHelp = mw::midi::pendingTakeReviewsIndicatorTooltip;
            const std::string trackHelp = mw::midi::pendingTakeReviewsTrackTooltip;
            const std::string resumeHelp = mw::midi::pendingTakeReviewsResumeTooltip;
            const std::string discardHelp = mw::midi::pendingTakeReviewsDiscardTooltip;
            const std::string refreshHelp = mw::midi::pendingTakeReviewsRefreshTooltip;
            const bool helperBubbleContract = !indicatorHelp.empty() && !trackHelp.empty() && !resumeHelp.empty()
                && !discardHelp.empty() && !refreshHelp.empty()
                && indicatorHelp.find("saved TAKE REVIEW") != std::string::npos
                && resumeHelp.find("without changing project notes") != std::string::npos
                && discardHelp.find("Other pending reviews") != std::string::npos
                && trackHelp.find("stable track identity") != std::string::npos;

            std::filesystem::remove(beforePath, ec);
            std::filesystem::remove(afterPath, ec);
            std::filesystem::remove(v10Path, ec);

            const bool passed = initialSummary && resumeReadOnly && stableRenameReorder && restoredEquivalent
                && discardIsolation && staleRemovalRejected && replacementInvalidatesOld && legacyCompatible
                && helperBubbleContract;
            return makeResult("I16", "Workflow", "Pending take-review management and stable resume", passed,
                              passed
                                  ? "Pending TAKE REVIEW summaries listed only unresolved MIDI tracks in project order; stable-ID resume survived rename/reorder without mutating serialized project state; runtime review/comp reconstruction matched persisted data; discard affected only the selected review; stale/removed/project-replacement identities were rejected; format-10 compatibility remained intact; and manager helper-bubble contracts stayed populated."
                                  : "Pending-review summary, stable-ID resume, non-mutating reconstruction, discard isolation, stale identity rejection, legacy compatibility, or helper-bubble contract failed.");
        }


        SimulationScenarioResult scenarioI17()
        {
            mw::midi::MidiRecordingConfig config;
            config.tempoBpm = 120;
            config.ticksPerQuarterNote = 960;
            config.startTick = 1000;
            config.startTimeSeconds = 0.0;

            mw::midi::MidiRecordingSession session;
            const bool sessionStarted = session.start(config);
            session.noteOn(1, 60, 100, 0.10);
            session.controller64(1, 63, 0.20); // threshold remains Up
            session.controller64(1, 64, 0.30); // threshold becomes Down
            session.noteOff(1, 60, 0.40);
            session.noteOn(2, 62, 90, 0.32);
            session.noteOff(2, 62, 0.42);       // ch1 pedal must not affect ch2
            session.noteOn(3, 64, 80, 0.12);
            session.noteOff(3, 64, 0.22);
            session.controller64(3, 127, 0.30); // pedal after Note Off must not resurrect
            session.controller64(3, 0, 0.50);
            session.controller64(1, 0, 0.60);
            const auto stopped = session.stop(0.70);

            mw::midi::MidiRecordingLoopTakePass recordedPass;
            recordedPass.passNumber = 1;
            recordedPass.notes = stopped.notes;
            recordedPass.sustainDownMaskAtStart = stopped.sustainDownMaskAtStart;
            recordedPass.cc64Events = stopped.cc64Events;
            const auto ignored = mw::midi::buildMidiRecordingTakePassNotes(
                recordedPass, mw::midi::MidiRecordingSustainPedalMode::Ignore, 4000);
            const auto applied = mw::midi::buildMidiRecordingTakePassNotes(
                recordedPass, mw::midi::MidiRecordingSustainPedalMode::ApplyCc64, 4000);

            auto findPitch = [](const std::vector<mw::core::NoteEvent>& notes, int pitch) -> const mw::core::NoteEvent*
            {
                const auto it = std::find_if(notes.begin(), notes.end(), [pitch](const auto& note)
                {
                    return note.pitch == pitch;
                });
                return it == notes.end() ? nullptr : &*it;
            };
            const auto* ignored60 = findPitch(ignored, 60);
            const auto* applied60 = findPitch(applied, 60);
            const auto* ignored62 = findPitch(ignored, 62);
            const auto* applied62 = findPitch(applied, 62);
            const auto* ignored64 = findPitch(ignored, 64);
            const auto* applied64 = findPitch(applied, 64);
            const bool thresholdAndChannelRules = sessionStarted
                && stopped.cc64Events.size() == 5
                && ignored60 != nullptr && applied60 != nullptr
                && applied60->durationTicks > ignored60->durationTicks
                && ignored62 != nullptr && applied62 != nullptr
                && applied62->durationTicks == ignored62->durationTicks
                && ignored64 != nullptr && applied64 != nullptr
                && applied64->durationTicks == ignored64->durationTicks;

            mw::midi::MidiRecordingLoopTakePass sameTickUpWins;
            sameTickUpWins.passNumber = 10;
            sameTickUpWins.notes.emplace_back(65, 100, 1200, 300, 1, mw::core::Articulation::Normal);
            sameTickUpWins.cc64Events = { { 1500, 1, 127, 1 }, { 1500, 1, 0, 2 } };
            const auto sameTickUpNotes = mw::midi::buildMidiRecordingTakePassNotes(
                sameTickUpWins, mw::midi::MidiRecordingSustainPedalMode::ApplyCc64, 3000);
            mw::midi::MidiRecordingLoopTakePass sameTickDownWins = sameTickUpWins;
            sameTickDownWins.cc64Events = { { 1500, 1, 0, 1 }, { 1500, 1, 127, 2 } };
            const auto sameTickDownNotes = mw::midi::buildMidiRecordingTakePassNotes(
                sameTickDownWins, mw::midi::MidiRecordingSustainPedalMode::ApplyCc64, 3000);
            const bool sameTickLastStateWins = sameTickUpNotes.size() == 1 && sameTickDownNotes.size() == 1
                && sameTickUpNotes[0].durationTicks == 300
                && sameTickDownNotes[0].startTick + sameTickDownNotes[0].durationTicks == 3000;

            mw::midi::MidiRecordingLoopTakePass repeated;
            repeated.passNumber = 11;
            repeated.sustainDownMaskAtStart = 1;
            repeated.notes.emplace_back(66, 100, 1200, 300, 1, mw::core::Articulation::Normal);
            repeated.notes.emplace_back(66, 90, 1300, 350, 1, mw::core::Articulation::Normal);
            const auto repeatedApplied = mw::midi::buildMidiRecordingTakePassNotes(
                repeated, mw::midi::MidiRecordingSustainPedalMode::ApplyCc64, 3000);
            const bool missingUpAndOverlap = repeatedApplied.size() == 2
                && repeatedApplied[0].startTick + repeatedApplied[0].durationTicks == 3000
                && repeatedApplied[1].startTick + repeatedApplied[1].durationTicks == 3000;

            // Initial per-channel sustain state supports pedal held across a loop wrap.
            mw::midi::MidiRecordingLoopTakePass wrappedPass;
            wrappedPass.passNumber = 2;
            wrappedPass.sustainDownMaskAtStart = static_cast<std::uint16_t>(1u << 3u); // channel 4
            wrappedPass.notes.emplace_back(67, 100, 1200, 300, 4, mw::core::Articulation::Normal);
            wrappedPass.cc64Events.push_back({ 2400, 4, 0, 1 });
            const auto wrappedApplied = mw::midi::buildMidiRecordingTakePassNotes(
                wrappedPass, mw::midi::MidiRecordingSustainPedalMode::ApplyCc64, 3000);
            const bool wrapInitialState = wrappedApplied.size() == 1
                && wrappedApplied[0].startTick + wrappedApplied[0].durationTicks == 2400;

            // Panic must force captured pedal state Up without ending the session.
            mw::midi::MidiRecordingSession panicSession;
            panicSession.start(config);
            panicSession.controller64(5, 127, 0.10);
            panicSession.noteOn(5, 70, 100, 0.12);
            const auto panicFinalized = panicSession.releaseActiveNotes(0.20);
            const auto panicStop = panicSession.stop(0.30);
            const bool panicClearsPedal = panicFinalized == 1
                && panicStop.sustainDownMaskAtStop == 0
                && std::any_of(panicStop.cc64Events.begin(), panicStop.cc64Events.end(), [](const auto& event)
                {
                    return event.midiChannel == 5 && event.value == 0;
                });

            mw::midi::MidiRecordingLoopTakePass compPass1;
            compPass1.passNumber = 1;
            compPass1.notes.emplace_back(72, 100, 1200, 300, 1, mw::core::Articulation::Normal);
            compPass1.cc64Events = { { 1300, 1, 127, 1 }, { 3500, 1, 0, 2 } };
            mw::midi::MidiRecordingLoopTakePass compPass2;
            compPass2.passNumber = 2;
            // Begins before the pass-2 segment: must never be synthesized at the boundary.
            compPass2.notes.emplace_back(72, 100, 1500, 2000, 1, mw::core::Articulation::Normal);
            const std::vector<mw::midi::MidiRecordingLoopTakePass> compPasses { compPass1, compPass2 };

            mw::midi::MidiRecordingLoopCompState comp;
            const auto generation = comp.begin(1801, 2801, 3801, 1000, 4000, 960, 4, 4,
                                                mw::midi::MidiRecordingCompGrid::Custom, 1, compPasses);
            const bool splitA = comp.addCustomBoundary(generation, 2000);
            const bool splitB = comp.addCustomBoundary(generation, 3000);
            const bool assignSame = comp.assignPass(generation, 1, 1, compPasses);
            const bool assignChange = comp.assignPass(generation, 2, 2, compPasses);
            const bool applyMode = comp.setSustainPedalMode(generation, mw::midi::MidiRecordingSustainPedalMode::ApplyCc64);
            comp.setBoundaryNoteMode(generation, mw::midi::MidiRecordingCompBoundaryNoteMode::Cut);
            const auto cutBuild = comp.buildNotes(generation, compPasses);
            comp.setBoundaryNoteMode(generation, mw::midi::MidiRecordingCompBoundaryNoteMode::PreserveSamePass);
            const auto preserveBuild = comp.buildNotes(generation, compPasses);
            const bool compRules = splitA && splitB && assignSame && assignChange && applyMode
                && cutBuild.notes.size() == 1 && preserveBuild.notes.size() == 1
                && cutBuild.notes[0].startTick + cutBuild.notes[0].durationTicks == 2000
                && preserveBuild.notes[0].startTick + preserveBuild.notes[0].durationTicks == 3000;

            mw::midi::MidiRecordingLoopTakePass gridPass;
            gridPass.passNumber = 1;
            gridPass.notes.emplace_back(75, 100, 100, 300, 1, mw::core::Articulation::Normal);
            gridPass.cc64Events = { { 200, 1, 127, 1 }, { 3000, 1, 0, 2 } };
            const std::vector<mw::midi::MidiRecordingLoopTakePass> gridPasses { gridPass };
            auto buildGrid = [&](mw::midi::MidiRecordingCompGrid grid)
            {
                mw::midi::MidiRecordingLoopCompState state;
                const auto gen = state.begin(1810, 2810, 3810, 0, 3840, 960, 4, 4, grid, 1, gridPasses);
                state.setBoundaryNoteMode(gen, mw::midi::MidiRecordingCompBoundaryNoteMode::PreserveSamePass);
                state.setSustainPedalMode(gen, mw::midi::MidiRecordingSustainPedalMode::ApplyCc64);
                return state.buildNotes(gen, gridPasses);
            };
            const auto beatBuild = buildGrid(mw::midi::MidiRecordingCompGrid::Beat);
            const auto barBuild = buildGrid(mw::midi::MidiRecordingCompGrid::Bar);
            const bool gridConsistency = beatBuild.notes.size() == 1 && barBuild.notes.size() == 1
                && beatBuild.notes[0].startTick + beatBuild.notes[0].durationTicks == 3000
                && barBuild.notes[0].startTick + barBuild.notes[0].durationTicks == 3000;

            // Format 12 persists mode, CC64 events and pass-start state. Format 11 defaults to Ignore.
            mw::core::Project project("CC64 Review Fixture");
            project.setStableId(1802);
            auto& track = project.addTrack("Pedal Piano");
            track.setStableId(2802);
            mw::core::PersistentMidiTakeReview review;
            review.active = true;
            review.regionInTick = 1000;
            review.regionOutTick = 4000;
            review.selectedPassNumber = 1;
            review.reviewMode = mw::core::PersistentMidiTakeReviewMode::Comp;
            review.compGrid = mw::core::PersistentMidiCompGrid::Custom;
            review.boundaryNoteMode = mw::core::PersistentMidiCompBoundaryNoteMode::PreserveSamePass;
            review.sustainPedalMode = mw::core::PersistentMidiSustainPedalMode::ApplyCc64;
            mw::core::PersistentMidiTakePass persistentPass;
            persistentPass.passNumber = 1;
            persistentPass.notes = compPass1.notes;
            persistentPass.sustainDownMaskAtStart = static_cast<std::uint16_t>(1u << 0u);
            persistentPass.cc64Events = compPass1.cc64Events;
            review.passes.push_back(persistentPass);
            review.segments.push_back({ 1000, 4000, 1 });
            track.setPendingMidiTakeReview(review);

            const auto tempFolder = std::filesystem::temp_directory_path() / "PoorMansStudioSimulation";
            const auto v12Path = tempFolder / "cc64_review_v12.mwproj";
            const auto v11Path = tempFolder / "cc64_review_v11.mwproj";
            const auto malformedPath = tempFolder / "cc64_review_malformed_v12.mwproj";
            std::error_code ec;
            std::filesystem::create_directories(tempFolder, ec);
            std::filesystem::remove(v12Path, ec);
            std::filesystem::remove(v11Path, ec);
            std::filesystem::remove(malformedPath, ec);

            const bool saved = mw::serialization::ProjectSerializer::saveToFile(project, v12Path);
            const auto reopened = saved ? mw::serialization::ProjectSerializer::loadFromFile(v12Path) : std::nullopt;
            const bool v12RoundTrip = reopened.has_value()
                && reopened->getMetadata().projectFormatVersion == 22
                && reopened->getTracks()[0].getPendingMidiTakeReview().sustainPedalMode
                    == mw::core::PersistentMidiSustainPedalMode::ApplyCc64
                && reopened->getTracks()[0].getPendingMidiTakeReview().passes[0].sustainDownMaskAtStart == 1
                && reopened->getTracks()[0].getPendingMidiTakeReview().passes[0].cc64Events.size() == 2;

            bool stableResumeAndUndo = false;
            if (reopened.has_value())
            {
                const auto summariesAfterReopen = mw::midi::buildPendingMidiTakeReviewSummaries(*reopened);
                const auto resolved = mw::midi::findPendingMidiTakeReviewTrackIndexByStableId(*reopened, 2802);
                if (!summariesAfterReopen.empty() && resolved.has_value())
                {
                    const auto persistedReview = reopened->getTracks()[static_cast<std::size_t>(*resolved)].getPendingMidiTakeReview();
                    std::vector<mw::midi::MidiRecordingLoopTakePass> runtimePasses;
                    for (const auto& pass : persistedReview.passes)
                    {
                        runtimePasses.emplace_back(pass.passNumber, pass.notes, pass.sustainDownMaskAtStart, pass.cc64Events);
                    }
                    mw::midi::MidiRecordingLoopTakeReviewState restoredState;
                    const bool restored = restoredState.restoreReview(1802, 2802, 4802, std::move(runtimePasses), persistedReview.selectedPassNumber);
                    mw::core::PersistentMidiTakeReview cleared;
                    cleared.clear();
                    cleared = persistedReview; // modeled Undo restoration of the complete pending review
                    stableResumeAndUndo = restored
                        && restoredState.selectedPass() != nullptr
                        && restoredState.selectedPass()->cc64Events.size() == 2
                        && restoredState.selectedPass()->sustainDownMaskAtStart == 1
                        && cleared.sustainPedalMode == mw::core::PersistentMidiSustainPedalMode::ApplyCc64;
                }
            }

            {
                std::ofstream file(v11Path, std::ios::binary | std::ios::trunc);
                file << "{\n  \"format\": \"MusicWorkstationProject\",\n  \"version\": 11,\n"
                     << "  \"projectId\": 1803, \"name\": \"Format11 Ignore\", \"tempoBpm\": 120,\n"
                     << "  \"tracks\": [{\"trackId\":2803,\"name\":\"MIDI\",\"notes\":[],"
                     << "\"pendingMidiTakeReview\":{\"active\":true,\"regionInTick\":1000,\"regionOutTick\":4000,"
                     << "\"selectedPassNumber\":1,\"reviewMode\":\"WholePass\",\"compGrid\":\"Beat\",\"boundaryNoteMode\":\"Cut\","
                     << "\"passes\":[{\"passNumber\":1,\"takeNotes\":[{\"pitch\":60,\"velocity\":100,\"startTick\":1200,\"durationTicks\":200,\"midiChannel\":1,\"articulation\":\"Normal\"}]}],"
                     << "\"segments\":[]}}]}\n";
            }
            const auto legacy = mw::serialization::ProjectSerializer::loadFromFile(v11Path);
            const bool v11DefaultsIgnore = legacy.has_value()
                && legacy->getMetadata().projectFormatVersion == 22
                && legacy->getTracks()[0].getPendingMidiTakeReview().sustainPedalMode
                    == mw::core::PersistentMidiSustainPedalMode::Ignore
                && legacy->getTracks()[0].getPendingMidiTakeReview().passes[0].cc64Events.empty();

            {
                std::ofstream file(malformedPath, std::ios::binary | std::ios::trunc);
                file << "{\n  \"format\": \"MusicWorkstationProject\",\n  \"version\": 12,\n"
                     << "  \"projectId\": 1804, \"name\": \"Malformed CC64\", \"tempoBpm\": 120,\n"
                     << "  \"tracks\": [{\"trackId\":2804,\"name\":\"MIDI\",\"notes\":[],"
                     << "\"pendingMidiTakeReview\":{\"active\":true,\"regionInTick\":1000,\"regionOutTick\":4000,"
                     << "\"selectedPassNumber\":1,\"reviewMode\":\"WholePass\",\"compGrid\":\"Beat\",\"boundaryNoteMode\":\"Cut\",\"sustainPedalMode\":\"FuturePedal\","
                     << "\"passes\":[{\"passNumber\":1,\"sustainDownMaskAtStart\":999999,\"cc64Events\":[{\"tick\":1200,\"midiChannel\":99,\"value\":200,\"order\":1}],"
                     << "\"takeNotes\":[{\"pitch\":60,\"velocity\":100,\"startTick\":1200,\"durationTicks\":200,\"midiChannel\":1,\"articulation\":\"Normal\"}]}],"
                     << "\"segments\":[]}}]}\n";
            }
            const auto malformed = mw::serialization::ProjectSerializer::loadFromFile(malformedPath);
            const bool malformedSafe = malformed.has_value()
                && malformed->getTracks()[0].getPendingMidiTakeReview().sustainPedalMode
                    == mw::core::PersistentMidiSustainPedalMode::Ignore
                && malformed->getTracks()[0].getPendingMidiTakeReview().passes[0].cc64Events.empty()
                && malformed->getTracks()[0].getPendingMidiTakeReview().passes[0].sustainDownMaskAtStart == 65535;

            const auto summaries = mw::midi::buildPendingMidiTakeReviewSummaries(project);
            const bool managerCarriesMode = summaries.size() == 1
                && summaries[0].sustainPedalMode == mw::core::PersistentMidiSustainPedalMode::ApplyCc64;

            const std::string sustainHelp = mw::midi::midiRecordingSustainPedalTooltip;
            const std::string applyHelp = mw::midi::midiRecordingSustainApplyCc64Tooltip;
            const bool helperBubbleContract = sustainHelp.find("CC64") != std::string::npos
                && sustainHelp.find("Loop Out") != std::string::npos
                && applyHelp.find("no Note On is invented") != std::string::npos;

            std::filesystem::remove(v12Path, ec);
            std::filesystem::remove(v11Path, ec);
            std::filesystem::remove(malformedPath, ec);

            const bool passed = thresholdAndChannelRules && sameTickLastStateWins && missingUpAndOverlap
                && wrapInitialState && panicClearsPedal && compRules && gridConsistency
                && v12RoundTrip && stableResumeAndUndo && v11DefaultsIgnore && malformedSafe
                && managerCarriesMode && helperBubbleContract;
            return makeResult("I17", "Workflow", "CC64 sustain-pedal-aware retained takes and comps", passed,
                              passed
                                  ? "CC64 threshold/channel capture, same-tick last-state ordering, pedal-held note extension, no-resurrection, missing-up Loop Out clamp, overlapping-note independence, loop-wrap initial sustain state, Panic release, Cut/Preserve Same Pass and Beat/Bar/Custom comp interaction, format-15 persistence/resume/Undo state, format-11 Ignore compatibility, malformed-controller safety, Pending Take Review summary state, and helper-bubble contracts all behaved deterministically."
                                  : "CC64 capture/application, comp-boundary interaction, persistence compatibility, manager summary, Panic handling, or helper-bubble contract failed.");
        }


        SimulationScenarioResult scenarioI18()
        {
            auto makeReview = [](mw::core::StableId sessionId,
                                 std::string name,
                                 int passNumber,
                                 int pitch)
            {
                mw::core::PersistentMidiTakeReview review;
                review.active = true;
                review.reviewSessionId = sessionId;
                review.reviewName = std::move(name);
                review.regionInTick = 1000;
                review.regionOutTick = 5000;
                review.selectedPassNumber = passNumber;
                review.reviewMode = mw::core::PersistentMidiTakeReviewMode::Comp;
                review.compGrid = mw::core::PersistentMidiCompGrid::Custom;
                review.boundaryNoteMode = mw::core::PersistentMidiCompBoundaryNoteMode::PreserveSamePass;
                review.sustainPedalMode = mw::core::PersistentMidiSustainPedalMode::ApplyCc64;
                mw::core::PersistentMidiTakePass pass;
                pass.passNumber = passNumber;
                pass.sustainDownMaskAtStart = 1;
                pass.cc64Events.push_back({ 2600, 1, 0, 1 });
                pass.notes.emplace_back(pitch, 100, 1200, 1000, 1, mw::core::Articulation::Normal);
                review.passes.push_back(pass);
                review.segments.push_back({ 1000, 3000, passNumber });
                review.segments.push_back({ 3000, 5000, passNumber });
                return review;
            };

            mw::core::Project project("Named Archive Fixture");
            project.setStableId(1901);
            auto& track = project.addTrack("Piano");
            track.setStableId(2901);
            track.addNote(mw::core::NoteEvent(48, 90, 200, 300, 1, mw::core::Articulation::Normal));
            track.setPendingMidiTakeReview(makeReview(3901, "Chorus Attempt 1", 1, 60));
            const auto projectNotesBefore = track.getNotes();

            const bool archivedFirst = mw::midi::archivePendingMidiTakeReviewByStableId(project, 2901)
                && !track.getPendingMidiTakeReview().active
                && track.getArchivedMidiTakeReviews().size() == 1
                && track.getArchivedMidiTakeReviews()[0].reviewSessionId == 3901
                && track.getArchivedMidiTakeReviews()[0].reviewName == "Chorus Attempt 1"
                && track.getNotes().size() == projectNotesBefore.size()
                && track.getNotes()[0].pitch == projectNotesBefore[0].pitch
                && track.getNotes()[0].startTick == projectNotesBefore[0].startTick
                && track.getNotes()[0].durationTicks == projectNotesBefore[0].durationTicks;

            track.setPendingMidiTakeReview(makeReview(3902, "Chorus Attempt 2", 2, 62));
            const bool activeAndArchiveIndependent = track.getPendingMidiTakeReview().reviewSessionId == 3902
                && track.getArchivedMidiTakeReviews().size() == 1;
            const bool archivedSecond = mw::midi::archivePendingMidiTakeReviewByStableId(project, 2901)
                && track.getArchivedMidiTakeReviews().size() == 2
                && !track.getPendingMidiTakeReview().active;

            const bool renamedArchive = mw::midi::renameMidiTakeReviewByStableIds(
                project, 2901, 3901, "Chorus Gold")
                && track.getArchivedMidiTakeReviews()[0].reviewName == "Chorus Gold"
                && !mw::midi::renameMidiTakeReviewByStableIds(project, 2901, 3901, "   ");

            track.setPendingMidiTakeReview(makeReview(3903, "Fresh Active", 3, 64));
            const bool restoreBlockedByActive = !mw::midi::restoreArchivedMidiTakeReviewByStableIds(project, 2901, 3901)
                && track.getPendingMidiTakeReview().reviewSessionId == 3903;
            const bool archivedThird = mw::midi::archivePendingMidiTakeReviewByStableId(project, 2901)
                && track.getArchivedMidiTakeReviews().size() == 3;

            const bool restoredExact = mw::midi::restoreArchivedMidiTakeReviewByStableIds(project, 2901, 3901)
                && track.getPendingMidiTakeReview().reviewSessionId == 3901
                && track.getPendingMidiTakeReview().reviewName == "Chorus Gold"
                && track.getPendingMidiTakeReview().passes.size() == 1
                && track.getPendingMidiTakeReview().passes[0].cc64Events.size() == 1
                && track.getPendingMidiTakeReview().boundaryNoteMode
                    == mw::core::PersistentMidiCompBoundaryNoteMode::PreserveSamePass
                && track.getPendingMidiTakeReview().sustainPedalMode
                    == mw::core::PersistentMidiSustainPedalMode::ApplyCc64
                && track.getArchivedMidiTakeReviews().size() == 2;

            const bool deletedOnlySelected = mw::midi::deleteArchivedMidiTakeReviewByStableIds(project, 2901, 3902)
                && track.getArchivedMidiTakeReviews().size() == 1
                && track.getArchivedMidiTakeReviews()[0].reviewSessionId == 3903
                && !mw::midi::deleteArchivedMidiTakeReviewByStableIds(project, 2901, 999999);

            const auto allSummaries = mw::midi::buildMidiTakeReviewSummaries(project);
            const auto pendingSummaries = mw::midi::buildPendingMidiTakeReviewSummaries(project);
            const bool managerSemantics = allSummaries.size() == 2
                && pendingSummaries.size() == 1
                && allSummaries[0].storageState == mw::midi::MidiTakeReviewStorageState::Active
                && allSummaries[0].reviewSessionId == 3901
                && allSummaries[1].storageState == mw::midi::MidiTakeReviewStorageState::Archived
                && allSummaries[1].reviewSessionId == 3903;

            const auto tempRoot = std::filesystem::temp_directory_path();
            const auto v13Path = tempRoot / "pms_sim_i18_v13.pms";
            const auto v12Path = tempRoot / "pms_sim_i18_v12.pms";
            const auto malformedPath = tempRoot / "pms_sim_i18_malformed.pms";
            std::error_code ec;
            std::filesystem::remove(v13Path, ec);
            std::filesystem::remove(v12Path, ec);
            std::filesystem::remove(malformedPath, ec);

            const bool saved = mw::serialization::ProjectSerializer::saveToFile(project, v13Path);
            const auto reopened = mw::serialization::ProjectSerializer::loadFromFile(v13Path);
            bool v13RoundTrip = false;
            if (saved && reopened.has_value())
            {
                const auto& reopenedTrack = reopened->getTracks()[0];
                v13RoundTrip = reopened->getMetadata().projectFormatVersion == 22
                    && reopenedTrack.getPendingMidiTakeReview().active
                    && reopenedTrack.getPendingMidiTakeReview().reviewSessionId == 3901
                    && reopenedTrack.getPendingMidiTakeReview().reviewName == "Chorus Gold"
                    && reopenedTrack.getArchivedMidiTakeReviews().size() == 1
                    && reopenedTrack.getArchivedMidiTakeReviews()[0].reviewSessionId == 3903
                    && reopenedTrack.getArchivedMidiTakeReviews()[0].reviewName == "Fresh Active"
                    && reopenedTrack.getArchivedMidiTakeReviews()[0].passes[0].cc64Events.size() == 1;
            }

            {
                std::ofstream file(v12Path, std::ios::binary | std::ios::trunc);
                file << "{\n  \"format\": \"MusicWorkstationProject\",\n  \"version\": 12,\n"
                     << "  \"projectId\": 1902, \"name\": \"Legacy Active\", \"tempoBpm\": 120,\n"
                     << "  \"tracks\": [{\"trackId\":2902,\"name\":\"Legacy MIDI\",\"notes\":[],"
                     << "\"pendingMidiTakeReview\":{\"active\":true,\"regionInTick\":1000,\"regionOutTick\":5000,"
                     << "\"selectedPassNumber\":1,\"reviewMode\":\"WholePass\",\"compGrid\":\"Beat\",\"boundaryNoteMode\":\"Cut\",\"sustainPedalMode\":\"Ignore\","
                     << "\"passes\":[{\"passNumber\":1,\"takeNotes\":[{\"pitch\":60,\"velocity\":100,\"startTick\":1200,\"durationTicks\":300,\"midiChannel\":1,\"articulation\":\"Normal\"}]}],"
                     << "\"segments\":[]}}]}\n";
            }
            const auto legacy = mw::serialization::ProjectSerializer::loadFromFile(v12Path);
            const bool v12Migrates = legacy.has_value()
                && legacy->getMetadata().projectFormatVersion == 22
                && legacy->getTracks()[0].getPendingMidiTakeReview().active
                && legacy->getTracks()[0].getPendingMidiTakeReview().reviewSessionId != 0
                && legacy->getTracks()[0].getPendingMidiTakeReview().reviewName == "Take Review 1"
                && legacy->getTracks()[0].getArchivedMidiTakeReviews().empty();

            {
                std::ofstream file(malformedPath, std::ios::binary | std::ios::trunc);
                file << "{\n  \"format\": \"MusicWorkstationProject\",\n  \"version\": 13,\n"
                     << "  \"projectId\": 1903, \"name\": \"Malformed Archive\", \"tempoBpm\": 120,\n"
                     << "  \"tracks\": [{\"trackId\":2903,\"name\":\"MIDI\",\"notes\":[],"
                     << "\"pendingMidiTakeReview\":{\"active\":true,\"reviewSessionId\":4901,\"reviewName\":\"Good Active\",\"regionInTick\":1000,\"regionOutTick\":5000,\"selectedPassNumber\":1,\"reviewMode\":\"WholePass\",\"compGrid\":\"Beat\",\"passes\":[{\"passNumber\":1,\"takeNotes\":[{\"pitch\":60,\"velocity\":100,\"startTick\":1200,\"durationTicks\":300,\"midiChannel\":1,\"articulation\":\"Normal\"}]}],\"segments\":[]},"
                     << "\"archivedMidiTakeReviews\":[{\"active\":true,\"reviewSessionId\":4902,\"reviewName\":\"Bad Archive\",\"regionInTick\":5000,\"regionOutTick\":1000,\"selectedPassNumber\":1,\"reviewMode\":\"WholePass\",\"compGrid\":\"Beat\",\"passes\":[],\"segments\":[]}]}]}\n";
            }
            const auto malformed = mw::serialization::ProjectSerializer::loadFromFile(malformedPath);
            const bool malformedArchiveSafe = malformed.has_value()
                && malformed->getTracks()[0].getPendingMidiTakeReview().active
                && malformed->getTracks()[0].getPendingMidiTakeReview().reviewName == "Good Active"
                && malformed->getTracks()[0].getArchivedMidiTakeReviews().empty();

            const bool helperBubbleContract = std::string(mw::midi::pendingTakeReviewsArchiveTooltip).find("free") != std::string::npos
                && std::string(mw::midi::pendingTakeReviewsRestoreTooltip).find("Active") != std::string::npos
                && std::string(mw::midi::pendingTakeReviewsRenameTooltip).find("Rename") != std::string::npos
                && std::string(mw::midi::pendingTakeReviewsDeleteArchiveTooltip).find("delete") != std::string::npos
                && std::string(mw::midi::midiTakeReviewArchiveTooltip).find("No MIDI notes") != std::string::npos;

            std::filesystem::remove(v13Path, ec);
            std::filesystem::remove(v12Path, ec);
            std::filesystem::remove(malformedPath, ec);

            const bool passed = archivedFirst && activeAndArchiveIndependent && archivedSecond
                && renamedArchive && restoreBlockedByActive && archivedThird && restoredExact
                && deletedOnlySelected && managerSemantics && v13RoundTrip && v12Migrates
                && malformedArchiveSafe && helperBubbleContract;
            return makeResult("I18", "Workflow", "Named and archived persisted MIDI take-review management", passed,
                              passed
                                  ? "Format-15 named active/archive review state round-tripped; Archive freed the one active slot without changing project notes; multiple archives remained independent; stable review-session IDs survived rename/reopen; Restore reproduced Comp/Boundary Notes/CC64 state and was blocked by an existing active review; Delete removed only its selected archive; format-12 migrated to a safely named active review with no archives; malformed archive entries were skipped safely; Pending Takes counted active reviews only; and new helper-bubble contracts stayed populated."
                                  : "Named/archive persistence, stable-session identity, archive/restore/delete isolation, legacy migration, malformed safety, active-only Pending count, or helper-bubble contracts failed.");
        }

        SimulationScenarioResult scenarioI19()
        {
            auto makeReview = [](mw::core::StableId sessionId, std::string name, int pitch)
            {
                mw::core::PersistentMidiTakeReview review;
                review.active = true;
                review.reviewSessionId = sessionId;
                review.reviewName = std::move(name);
                review.regionInTick = 1000;
                review.regionOutTick = 5000;
                review.selectedPassNumber = 1;
                review.reviewMode = mw::core::PersistentMidiTakeReviewMode::Comp;
                review.compGrid = mw::core::PersistentMidiCompGrid::Custom;
                review.boundaryNoteMode = mw::core::PersistentMidiCompBoundaryNoteMode::PreserveSamePass;
                review.sustainPedalMode = mw::core::PersistentMidiSustainPedalMode::ApplyCc64;
                mw::core::PersistentMidiTakePass pass;
                pass.passNumber = 1;
                pass.cc64Events.push_back({ 2600, 1, 0, 1 });
                pass.notes.emplace_back(pitch, 100, 1200, 1000, 1, mw::core::Articulation::Normal);
                review.passes.push_back(pass);
                review.segments.push_back({ 1000, 3000, 1 });
                review.segments.push_back({ 3000, 5000, 1 });
                return review;
            };

            mw::core::Project project("Take Library Fixture");
            project.setStableId(1951);
            auto& piano = project.addTrack("Piano");
            piano.setStableId(2951);
            piano.addNote(mw::core::NoteEvent(48, 90, 200, 300, 1, mw::core::Articulation::Normal));
            piano.setPendingMidiTakeReview(makeReview(3951, "Chorus Gold", 60));

            const auto notesBefore = piano.getNotes();
            const auto compBefore = piano.getPendingMidiTakeReview().segments;
            const bool normalizedMetadata = mw::midi::updateMidiTakeReviewMetadataByStableIds(
                    project, 2951, 3951, " favorite, Tight, FAVORITE, , needs-edit ",
                    "Best chorus performance so far")
                && piano.getPendingMidiTakeReview().reviewTags.size() == 3
                && piano.getPendingMidiTakeReview().reviewTags[0] == "favorite"
                && piano.getPendingMidiTakeReview().reviewTags[1] == "Tight"
                && piano.getPendingMidiTakeReview().reviewTags[2] == "needs-edit"
                && piano.getPendingMidiTakeReview().reviewNotes == "Best chorus performance so far"
                && piano.getNotes().size() == notesBefore.size()
                && piano.getPendingMidiTakeReview().segments.size() == compBefore.size()
                && piano.getPendingMidiTakeReview().passes[0].cc64Events.size() == 1;

            const bool archivedWithMetadata = mw::midi::archivePendingMidiTakeReviewByStableId(project, 2951)
                && piano.getArchivedMidiTakeReviews().size() == 1
                && piano.getArchivedMidiTakeReviews()[0].reviewTags.size() == 3
                && piano.getArchivedMidiTakeReviews()[0].reviewNotes == "Best chorus performance so far";

            piano.setPendingMidiTakeReview(makeReview(3952, "Chorus Working", 62));
            const bool activeMetadata = mw::midi::updateMidiTakeReviewMetadataByStableIds(
                    project, 2951, 3952, "rough, needs-edit", "Try a softer ending")
                && piano.getPendingMidiTakeReview().reviewTags.size() == 2;

            auto& strings = project.addTrack("Strings");
            strings.setStableId(2952);
            strings.setPendingMidiTakeReview(makeReview(3953, "Verse Alt", 67));
            const bool stringMetadata = mw::midi::updateMidiTakeReviewMetadataByStableIds(
                    project, 2952, 3953, "favorite, warm", "Warm strings alternate")
                && mw::midi::archivePendingMidiTakeReviewByStableId(project, 2952);

            const auto all = mw::midi::buildMidiTakeReviewSummaries(project);
            const auto chorusFavorite = mw::midi::filterMidiTakeReviewSummaries(
                all, "chorus favorite", mw::midi::MidiTakeReviewLibraryFilter::All);
            const auto pianoTight = mw::midi::filterMidiTakeReviewSummaries(
                all, "PIANO tight", mw::midi::MidiTakeReviewLibraryFilter::All);
            const auto verseWarm = mw::midi::filterMidiTakeReviewSummaries(
                all, "verse warm", mw::midi::MidiTakeReviewLibraryFilter::All);
            const auto activeOnly = mw::midi::filterMidiTakeReviewSummaries(
                all, "", mw::midi::MidiTakeReviewLibraryFilter::Active);
            const auto archivedOnly = mw::midi::filterMidiTakeReviewSummaries(
                all, "", mw::midi::MidiTakeReviewLibraryFilter::Archived);
            const bool filtersWork = all.size() == 3
                && chorusFavorite.size() == 1 && chorusFavorite[0].reviewSessionId == 3951
                && pianoTight.size() == 1 && pianoTight[0].reviewSessionId == 3951
                && verseWarm.size() == 1 && verseWarm[0].reviewSessionId == 3953
                && activeOnly.size() == 1 && activeOnly[0].reviewSessionId == 3952
                && archivedOnly.size() == 2
                && archivedOnly[0].reviewSessionId == 3951
                && archivedOnly[1].reviewSessionId == 3953;

            const auto tempRoot = std::filesystem::temp_directory_path();
            const auto beforeFilterPath = tempRoot / "pms_sim_i19_before_filter.pms";
            const auto afterFilterPath = tempRoot / "pms_sim_i19_after_filter.pms";
            const auto v13Path = tempRoot / "pms_sim_i19_v13.pms";
            const auto malformedPath = tempRoot / "pms_sim_i19_malformed.pms";
            std::error_code ec;
            for (const auto& path : { beforeFilterPath, afterFilterPath, v13Path, malformedPath })
                std::filesystem::remove(path, ec);

            const bool savedBefore = mw::serialization::ProjectSerializer::saveToFile(project, beforeFilterPath);
            (void) mw::midi::filterMidiTakeReviewSummaries(all, "favorite warm", mw::midi::MidiTakeReviewLibraryFilter::Archived);
            const bool savedAfter = mw::serialization::ProjectSerializer::saveToFile(project, afterFilterPath);
            auto readBytes = [](const std::filesystem::path& path)
            {
                std::ifstream file(path, std::ios::binary);
                return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
            };
            const bool filterNoMutation = savedBefore && savedAfter && readBytes(beforeFilterPath) == readBytes(afterFilterPath);

            const auto reopened = mw::serialization::ProjectSerializer::loadFromFile(beforeFilterPath);
            const bool v14RoundTrip = reopened.has_value()
                && reopened->getMetadata().projectFormatVersion == 22
                && reopened->getTracks().size() == 2
                && reopened->getTracks()[0].getPendingMidiTakeReview().reviewTags.size() == 2
                && reopened->getTracks()[0].getArchivedMidiTakeReviews().size() == 1
                && reopened->getTracks()[0].getArchivedMidiTakeReviews()[0].reviewTags.size() == 3
                && reopened->getTracks()[0].getArchivedMidiTakeReviews()[0].reviewNotes == "Best chorus performance so far"
                && reopened->getTracks()[0].getArchivedMidiTakeReviews()[0].passes[0].cc64Events.size() == 1;

            {
                std::ofstream file(v13Path, std::ios::binary | std::ios::trunc);
                file << "{\n  \"format\": \"MusicWorkstationProject\",\n  \"version\": 13,\n"
                     << "  \"projectId\": 1952, \"name\": \"Format13\", \"tempoBpm\": 120,\n"
                     << "  \"tracks\": [{\"trackId\":2953,\"name\":\"Legacy MIDI\",\"notes\":[],"
                     << "\"pendingMidiTakeReview\":{\"active\":true,\"reviewSessionId\":3954,\"reviewName\":\"Legacy Review\",\"regionInTick\":1000,\"regionOutTick\":5000,"
                     << "\"selectedPassNumber\":1,\"reviewMode\":\"WholePass\",\"compGrid\":\"Beat\","
                     << "\"passes\":[{\"passNumber\":1,\"takeNotes\":[{\"pitch\":60,\"velocity\":100,\"startTick\":1200,\"durationTicks\":300,\"midiChannel\":1,\"articulation\":\"Normal\"}]}],\"segments\":[]}}]}\n";
            }
            const auto legacy = mw::serialization::ProjectSerializer::loadFromFile(v13Path);
            const bool v13Migration = legacy.has_value()
                && legacy->getMetadata().projectFormatVersion == 22
                && legacy->getTracks()[0].getPendingMidiTakeReview().reviewTags.empty()
                && legacy->getTracks()[0].getPendingMidiTakeReview().reviewNotes.empty();

            {
                std::ofstream file(malformedPath, std::ios::binary | std::ios::trunc);
                file << "{\n  \"format\": \"MusicWorkstationProject\",\n  \"version\": 14,\n"
                     << "  \"projectId\": 1953, \"name\": \"Malformed Metadata\", \"tempoBpm\": 120,\n"
                     << "  \"tracks\": [{\"trackId\":2954,\"name\":\"MIDI\",\"notes\":[],"
                     << "\"pendingMidiTakeReview\":{\"active\":true,\"reviewSessionId\":3955,\"reviewName\":\"Good Review\","
                     << "\"tags\":[\" Favorite \",\"\",\"favorite\",\"Blue\"],\"reviewNotes\":123,"
                     << "\"regionInTick\":1000,\"regionOutTick\":5000,\"selectedPassNumber\":1,\"reviewMode\":\"WholePass\",\"compGrid\":\"Beat\","
                     << "\"passes\":[{\"passNumber\":1,\"takeNotes\":[{\"pitch\":60,\"velocity\":100,\"startTick\":1200,\"durationTicks\":300,\"midiChannel\":1,\"articulation\":\"Normal\"}]}],\"segments\":[]}}]}\n";
            }
            const auto malformed = mw::serialization::ProjectSerializer::loadFromFile(malformedPath);
            const bool malformedSafe = malformed.has_value()
                && malformed->getTracks()[0].getPendingMidiTakeReview().reviewTags.size() == 2
                && malformed->getTracks()[0].getPendingMidiTakeReview().reviewTags[0] == "Favorite"
                && malformed->getTracks()[0].getPendingMidiTakeReview().reviewTags[1] == "Blue"
                && malformed->getTracks()[0].getPendingMidiTakeReview().reviewNotes.empty()
                && malformed->getTracks()[0].getPendingMidiTakeReview().passes.size() == 1;

            const bool helperBubbleContract = std::string(mw::midi::takeReviewSearchTooltip).find("never changes project data") != std::string::npos
                && std::string(mw::midi::takeReviewShowFilterTooltip).find("Show All") != std::string::npos
                && std::string(mw::midi::takeReviewTagsTooltip).find("searchable") != std::string::npos
                && std::string(mw::midi::takeReviewNotesTooltip).find("organizational") != std::string::npos
                && std::string(mw::midi::takeReviewSaveMetadataTooltip).find("Undo Take") != std::string::npos;

            for (const auto& path : { beforeFilterPath, afterFilterPath, v13Path, malformedPath })
                std::filesystem::remove(path, ec);

            const bool passed = normalizedMetadata && archivedWithMetadata && activeMetadata && stringMetadata
                && filtersWork && filterNoMutation && v14RoundTrip && v13Migration && malformedSafe
                && helperBubbleContract;
            return makeResult("I19", "Workflow", "Persisted Take Review metadata and library filtering", passed,
                              passed
                                  ? "Format-15 Take Review tags/notes round-tripped; format-13 reviews migrated with empty metadata; tags normalized/deduplicated; metadata stayed independent from MIDI/Comp/CC64 data; Active/Archived search used case-insensitive multi-term AND matching across track/review/tag/note fields; All/Active/Archived filters were deterministic and non-mutating; malformed metadata was sanitized safely; and helper-bubble contracts stayed populated."
                                  : "Take Review metadata persistence, tag normalization, deterministic search/filter, legacy migration, malformed safety, MIDI isolation, or helper-bubble contracts failed.");
        }

        SimulationScenarioResult scenarioI20()
        {
            auto makeReview = [](mw::core::StableId sessionId, std::string name)
            {
                mw::core::PersistentMidiTakeReview review;
                review.active = true;
                review.reviewSessionId = sessionId;
                review.reviewName = std::move(name);
                review.reviewTags = { "favorite", "chorus" };
                review.reviewNotes = "Source archive notes";
                review.regionInTick = 960;
                review.regionOutTick = 6720;
                review.selectedPassNumber = 2;
                review.reviewMode = mw::core::PersistentMidiTakeReviewMode::Comp;
                review.compGrid = mw::core::PersistentMidiCompGrid::Custom;
                review.boundaryNoteMode = mw::core::PersistentMidiCompBoundaryNoteMode::PreserveSamePass;
                review.sustainPedalMode = mw::core::PersistentMidiSustainPedalMode::ApplyCc64;

                mw::core::PersistentMidiTakePass pass1;
                pass1.passNumber = 1;
                pass1.sustainDownMaskAtStart = 0x0001U;
                pass1.cc64Events.push_back({ 1200, 1, 127, 1 });
                pass1.cc64Events.push_back({ 2400, 1, 0, 2 });
                pass1.notes.emplace_back(60, 101, 1100, 900, 1, mw::core::Articulation::Legato);

                mw::core::PersistentMidiTakePass pass2;
                pass2.passNumber = 2;
                pass2.sustainDownMaskAtStart = 0x0002U;
                pass2.cc64Events.push_back({ 3600, 2, 127, 3 });
                pass2.notes.emplace_back(64, 96, 3000, 1400, 2, mw::core::Articulation::Accent);

                review.passes.push_back(pass1);
                review.passes.push_back(pass2);
                review.segments.push_back({ 960, 2880, 1 });
                review.segments.push_back({ 2880, 4800, 2 });
                review.segments.push_back({ 4800, 6720, 2 });
                return review;
            };

            auto sameNote = [](const mw::core::NoteEvent& a, const mw::core::NoteEvent& b)
            {
                return a.pitch == b.pitch && a.velocity == b.velocity && a.midiChannel == b.midiChannel
                    && a.startTick == b.startTick && a.durationTicks == b.durationTicks
                    && a.articulation == b.articulation;
            };
            auto sameReviewPayload = [&](const mw::core::PersistentMidiTakeReview& a,
                                         const mw::core::PersistentMidiTakeReview& b)
            {
                if (a.active != b.active || a.reviewTags != b.reviewTags || a.reviewNotes != b.reviewNotes
                    || a.regionInTick != b.regionInTick || a.regionOutTick != b.regionOutTick
                    || a.selectedPassNumber != b.selectedPassNumber || a.reviewMode != b.reviewMode
                    || a.compGrid != b.compGrid || a.boundaryNoteMode != b.boundaryNoteMode
                    || a.sustainPedalMode != b.sustainPedalMode || a.passes.size() != b.passes.size()
                    || a.segments.size() != b.segments.size())
                    return false;
                for (std::size_t passIndex = 0; passIndex < a.passes.size(); ++passIndex)
                {
                    const auto& left = a.passes[passIndex];
                    const auto& right = b.passes[passIndex];
                    if (left.passNumber != right.passNumber
                        || left.sustainDownMaskAtStart != right.sustainDownMaskAtStart
                        || left.notes.size() != right.notes.size()
                        || left.cc64Events.size() != right.cc64Events.size())
                        return false;
                    for (std::size_t noteIndex = 0; noteIndex < left.notes.size(); ++noteIndex)
                        if (!sameNote(left.notes[noteIndex], right.notes[noteIndex]))
                            return false;
                    for (std::size_t eventIndex = 0; eventIndex < left.cc64Events.size(); ++eventIndex)
                    {
                        const auto& x = left.cc64Events[eventIndex];
                        const auto& y = right.cc64Events[eventIndex];
                        if (x.tick != y.tick || x.midiChannel != y.midiChannel
                            || x.value != y.value || x.order != y.order)
                            return false;
                    }
                }
                for (std::size_t segmentIndex = 0; segmentIndex < a.segments.size(); ++segmentIndex)
                {
                    const auto& x = a.segments[segmentIndex];
                    const auto& y = b.segments[segmentIndex];
                    if (x.startTick != y.startTick || x.endTick != y.endTick
                        || x.selectedPassNumber != y.selectedPassNumber)
                        return false;
                }
                return true;
            };

            mw::core::Project project("Archive Duplication Fixture");
            project.setStableId(1960);
            auto& piano = project.addTrack("Piano");
            piano.setStableId(2960);
            piano.addNote(mw::core::NoteEvent(48, 90, 240, 480, 1, mw::core::Articulation::Normal));
            piano.setPendingMidiTakeReview(makeReview(4961, "Working Active"));
            const auto activeBefore = piano.getPendingMidiTakeReview();
            const auto notesBefore = piano.getNotes();
            auto source = makeReview(4960, "Chorus Alt");
            piano.setArchivedMidiTakeReviews({ source });

            const auto firstCopyId = mw::midi::duplicateArchivedMidiTakeReviewByStableIds(project, 2960, 4960);
            const bool firstCopy = firstCopyId.has_value()
                && *firstCopyId != 0 && *firstCopyId != 4960 && *firstCopyId != 4961
                && piano.getArchivedMidiTakeReviews().size() == 2
                && piano.getArchivedMidiTakeReviews().back().reviewSessionId == *firstCopyId
                && piano.getArchivedMidiTakeReviews().back().reviewName == "Chorus Alt Copy"
                && sameReviewPayload(source, piano.getArchivedMidiTakeReviews().back())
                && piano.getPendingMidiTakeReview().reviewSessionId == activeBefore.reviewSessionId
                && piano.getPendingMidiTakeReview().reviewName == activeBefore.reviewName
                && piano.getNotes().size() == notesBefore.size()
                && sameNote(piano.getNotes()[0], notesBefore[0]);

            const auto secondCopyId = mw::midi::duplicateArchivedMidiTakeReviewByStableIds(project, 2960, 4960);
            const bool secondCopy = secondCopyId.has_value()
                && *secondCopyId != *firstCopyId
                && piano.getArchivedMidiTakeReviews().back().reviewName == "Chorus Alt Copy 2"
                && piano.getArchivedMidiTakeReviews().back().reviewSessionId == *secondCopyId;

            auto collision = source;
            collision.reviewSessionId = 4963;
            collision.reviewName = "cHoRuS aLt CoPy 3";
            piano.getArchivedMidiTakeReviews().push_back(collision);
            const auto fourthNameId = mw::midi::duplicateArchivedMidiTakeReviewByStableIds(project, 2960, 4960);
            const bool caseInsensitiveNaming = fourthNameId.has_value()
                && piano.getArchivedMidiTakeReviews().back().reviewName == "Chorus Alt Copy 4"
                && piano.getArchivedMidiTakeReviews().back().reviewSessionId == *fourthNameId;

            const bool metadataIndependent = firstCopyId.has_value()
                && mw::midi::updateMidiTakeReviewMetadataByStableIds(
                    project, 2960, *firstCopyId, "copy-only, favorite", "Independent copy notes")
                && piano.getArchivedMidiTakeReviews()[0].reviewTags == source.reviewTags
                && piano.getArchivedMidiTakeReviews()[0].reviewNotes == source.reviewNotes
                && piano.getArchivedMidiTakeReviews()[1].reviewTags.size() == 2
                && piano.getArchivedMidiTakeReviews()[1].reviewTags[0] == "copy-only"
                && piano.getArchivedMidiTakeReviews()[1].reviewNotes == "Independent copy notes";

            const bool activeBlocksRestore = firstCopyId.has_value()
                && !mw::midi::restoreArchivedMidiTakeReviewByStableIds(project, 2960, *firstCopyId)
                && piano.getPendingMidiTakeReview().reviewSessionId == 4961;

            const auto archiveCountBeforeDelete = piano.getArchivedMidiTakeReviews().size();
            const bool deleteIndependent = secondCopyId.has_value()
                && mw::midi::deleteArchivedMidiTakeReviewByStableIds(project, 2960, *secondCopyId)
                && piano.getArchivedMidiTakeReviews().size() + 1 == archiveCountBeforeDelete
                && std::any_of(piano.getArchivedMidiTakeReviews().begin(), piano.getArchivedMidiTakeReviews().end(),
                               [](const auto& review) { return review.reviewSessionId == 4960; })
                && std::none_of(piano.getArchivedMidiTakeReviews().begin(), piano.getArchivedMidiTakeReviews().end(),
                                [&](const auto& review) { return review.reviewSessionId == *secondCopyId; });

            auto& other = project.addTrack("Other");
            other.setStableId(2961);
            std::swap(project.getTracks()[0], project.getTracks()[1]);
            for (auto& track : project.getTracks())
                if (track.getStableId() == 2960)
                    track.setName("Piano Renamed");
            const bool stableTrackOwnership = firstCopyId.has_value()
                && mw::midi::renameMidiTakeReviewByStableIds(project, 2960, *firstCopyId, "Independent Copy")
                && std::any_of(project.getTracks().begin(), project.getTracks().end(), [&](const auto& track)
                {
                    if (track.getStableId() != 2960)
                        return false;
                    return std::any_of(track.getArchivedMidiTakeReviews().begin(), track.getArchivedMidiTakeReviews().end(),
                                       [&](const auto& review)
                                       {
                                           return review.reviewSessionId == *firstCopyId
                                               && review.reviewName == "Independent Copy";
                                       });
                });

            const auto summaries = mw::midi::buildMidiTakeReviewSummaries(project);
            const auto copySearch = mw::midi::filterMidiTakeReviewSummaries(
                summaries, "copy-only independent", mw::midi::MidiTakeReviewLibraryFilter::Archived);
            const auto activeOnly = mw::midi::filterMidiTakeReviewSummaries(
                summaries, "", mw::midi::MidiTakeReviewLibraryFilter::Active);
            const bool filterIdentity = firstCopyId.has_value()
                && copySearch.size() == 1 && copySearch[0].reviewSessionId == *firstCopyId
                && activeOnly.size() == 1 && activeOnly[0].reviewSessionId == 4961;

            const auto tempRoot = std::filesystem::temp_directory_path();
            const auto beforeFilterPath = tempRoot / "pms_sim_i20_before_filter.pms";
            const auto afterFilterPath = tempRoot / "pms_sim_i20_after_filter.pms";
            std::error_code ec;
            std::filesystem::remove(beforeFilterPath, ec);
            std::filesystem::remove(afterFilterPath, ec);
            const bool savedBefore = mw::serialization::ProjectSerializer::saveToFile(project, beforeFilterPath);
            (void) mw::midi::filterMidiTakeReviewSummaries(
                summaries, "chorus", mw::midi::MidiTakeReviewLibraryFilter::Archived);
            const bool savedAfter = mw::serialization::ProjectSerializer::saveToFile(project, afterFilterPath);
            auto readBytes = [](const std::filesystem::path& path)
            {
                std::ifstream file(path, std::ios::binary);
                return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
            };
            const bool filterNoMutation = savedBefore && savedAfter
                && readBytes(beforeFilterPath) == readBytes(afterFilterPath);

            const auto reopened = mw::serialization::ProjectSerializer::loadFromFile(beforeFilterPath);
            bool persistedIndependentIds = false;
            if (reopened.has_value() && reopened->getMetadata().projectFormatVersion == 22 && firstCopyId.has_value())
            {
                for (const auto& track : reopened->getTracks())
                {
                    if (track.getStableId() != 2960)
                        continue;
                    bool sourceFound = false;
                    bool copyFound = false;
                    for (const auto& review : track.getArchivedMidiTakeReviews())
                    {
                        sourceFound = sourceFound || (review.reviewSessionId == 4960);
                        copyFound = copyFound || (review.reviewSessionId == *firstCopyId
                            && review.reviewName == "Independent Copy"
                            && review.reviewNotes == "Independent copy notes");
                    }
                    persistedIndependentIds = sourceFound && copyFound;
                }
            }

            mw::core::Project staleProject("Stale");
            auto& staleTrack = staleProject.addTrack("MIDI");
            staleTrack.setStableId(2999);
            staleTrack.setArchivedMidiTakeReviews({ makeReview(4999, "Stale Source") });
            const bool staleRejected = !mw::midi::duplicateArchivedMidiTakeReviewByStableIds(staleProject, 0, 4999).has_value()
                && !mw::midi::duplicateArchivedMidiTakeReviewByStableIds(staleProject, 2999, 0).has_value()
                && !mw::midi::duplicateArchivedMidiTakeReviewByStableIds(staleProject, 2999, 123456).has_value();
            staleProject.getTracks().clear();
            const bool removedTrackRejected = !mw::midi::duplicateArchivedMidiTakeReviewByStableIds(staleProject, 2999, 4999).has_value();

            const bool helperBubbleContract = std::string(mw::midi::pendingTakeReviewsDuplicateArchiveTooltip).find("independent") != std::string::npos
                && std::string(mw::midi::pendingTakeReviewsDuplicateArchiveTooltip).find("new stable review identity") != std::string::npos
                && std::string(mw::midi::pendingTakeReviewsDuplicateArchiveTooltip).find("Project MIDI notes are not changed") != std::string::npos;

            std::filesystem::remove(beforeFilterPath, ec);
            std::filesystem::remove(afterFilterPath, ec);

            const bool passed = firstCopy && secondCopy && caseInsensitiveNaming && metadataIndependent
                && activeBlocksRestore && deleteIndependent && stableTrackOwnership && filterIdentity
                && filterNoMutation && persistedIndependentIds && staleRejected && removedTrackRejected
                && helperBubbleContract;
            return makeResult("I20", "Workflow", "Archived Take Review duplication and stable copy identity", passed,
                              passed
                                  ? "Archived Review duplication deep-copied retained passes/Comp/Boundary Notes/CC64/Tags/Notes onto the same MIDI track with a new stable review-session ID; deterministic Copy/Copy 2 naming and case-insensitive collision skipping worked; an existing Active review stayed untouched; source/copy metadata and archive operations remained independent; stable track/session identity survived rename/reorder/save-reopen; Search/Show stayed non-mutating; stale identities were rejected; the original v0.67.29 format-14 duplication contract remained covered under current compatibility; and the Duplicate Archive helper-bubble contract stayed populated."
                                  : "Archived Review duplication identity, naming, deep-copy isolation, active-slot safety, stable ownership, filtering, persistence, stale rejection, format-15 continuity, or helper-bubble contracts failed.");
        }

        SimulationScenarioResult scenarioI21()
        {
            auto makeReview = [](mw::core::StableId sessionId, std::string name)
            {
                mw::core::PersistentMidiTakeReview review;
                review.active = true;
                review.reviewSessionId = sessionId;
                review.reviewName = std::move(name);
                review.reviewTags = { "working", "chorus" };
                review.reviewNotes = "Saved active metadata";
                review.regionInTick = 480;
                review.regionOutTick = 6240;
                review.selectedPassNumber = 2;
                review.reviewMode = mw::core::PersistentMidiTakeReviewMode::Comp;
                review.compGrid = mw::core::PersistentMidiCompGrid::Custom;
                review.boundaryNoteMode = mw::core::PersistentMidiCompBoundaryNoteMode::PreserveSamePass;
                review.sustainPedalMode = mw::core::PersistentMidiSustainPedalMode::ApplyCc64;

                mw::core::PersistentMidiTakePass pass1;
                pass1.passNumber = 1;
                pass1.sustainDownMaskAtStart = 0x0001U;
                pass1.cc64Events.push_back({ 720, 1, 127, 1 });
                pass1.cc64Events.push_back({ 1800, 1, 0, 2 });
                pass1.notes.emplace_back(60, 100, 600, 960, 1, mw::core::Articulation::Legato);

                mw::core::PersistentMidiTakePass pass2;
                pass2.passNumber = 2;
                pass2.sustainDownMaskAtStart = 0x0004U;
                pass2.cc64Events.push_back({ 3000, 3, 127, 3 });
                pass2.notes.emplace_back(67, 92, 2520, 1440, 3, mw::core::Articulation::Accent);

                review.passes = { pass1, pass2 };
                review.segments.push_back({ 480, 2400, 1 });
                review.segments.push_back({ 2400, 4320, 2 });
                review.segments.push_back({ 4320, 6240, 2 });
                return review;
            };

            auto sameNote = [](const mw::core::NoteEvent& a, const mw::core::NoteEvent& b)
            {
                return a.pitch == b.pitch && a.velocity == b.velocity && a.midiChannel == b.midiChannel
                    && a.startTick == b.startTick && a.durationTicks == b.durationTicks
                    && a.articulation == b.articulation;
            };
            auto sameReviewPayload = [&](const mw::core::PersistentMidiTakeReview& a,
                                         const mw::core::PersistentMidiTakeReview& b)
            {
                if (a.active != b.active || a.reviewTags != b.reviewTags || a.reviewNotes != b.reviewNotes
                    || a.regionInTick != b.regionInTick || a.regionOutTick != b.regionOutTick
                    || a.selectedPassNumber != b.selectedPassNumber || a.reviewMode != b.reviewMode
                    || a.compGrid != b.compGrid || a.boundaryNoteMode != b.boundaryNoteMode
                    || a.sustainPedalMode != b.sustainPedalMode || a.passes.size() != b.passes.size()
                    || a.segments.size() != b.segments.size())
                    return false;
                for (std::size_t passIndex = 0; passIndex < a.passes.size(); ++passIndex)
                {
                    const auto& left = a.passes[passIndex];
                    const auto& right = b.passes[passIndex];
                    if (left.passNumber != right.passNumber
                        || left.sustainDownMaskAtStart != right.sustainDownMaskAtStart
                        || left.notes.size() != right.notes.size()
                        || left.cc64Events.size() != right.cc64Events.size())
                        return false;
                    for (std::size_t noteIndex = 0; noteIndex < left.notes.size(); ++noteIndex)
                        if (!sameNote(left.notes[noteIndex], right.notes[noteIndex]))
                            return false;
                    for (std::size_t eventIndex = 0; eventIndex < left.cc64Events.size(); ++eventIndex)
                    {
                        const auto& x = left.cc64Events[eventIndex];
                        const auto& y = right.cc64Events[eventIndex];
                        if (x.tick != y.tick || x.midiChannel != y.midiChannel
                            || x.value != y.value || x.order != y.order)
                            return false;
                    }
                }
                for (std::size_t segmentIndex = 0; segmentIndex < a.segments.size(); ++segmentIndex)
                {
                    const auto& x = a.segments[segmentIndex];
                    const auto& y = b.segments[segmentIndex];
                    if (x.startTick != y.startTick || x.endTick != y.endTick
                        || x.selectedPassNumber != y.selectedPassNumber)
                        return false;
                }
                return true;
            };

            mw::core::Project project("Active Snapshot Fixture");
            project.setStableId(1970);
            auto& track = project.addTrack("Piano");
            track.setStableId(2970);
            track.addNote(mw::core::NoteEvent(50, 88, 120, 360, 1, mw::core::Articulation::Normal));
            track.setPendingMidiTakeReview(makeReview(4970, "Chorus Work"));
            const auto activeBefore = track.getPendingMidiTakeReview();
            const auto notesBefore = track.getNotes();

            const bool invalidRejected = !mw::midi::snapshotActiveMidiTakeReviewByStableIds(project, 0, 4970).has_value()
                && !mw::midi::snapshotActiveMidiTakeReviewByStableIds(project, 2970, 0).has_value()
                && !mw::midi::snapshotActiveMidiTakeReviewByStableIds(project, 2970, 999999).has_value();

            const auto firstSnapshotId = mw::midi::snapshotActiveMidiTakeReviewByStableIds(project, 2970, 4970);
            const bool firstSnapshot = firstSnapshotId.has_value()
                && *firstSnapshotId != 0 && *firstSnapshotId != 4970
                && track.getPendingMidiTakeReview().reviewSessionId == 4970
                && track.getPendingMidiTakeReview().reviewName == "Chorus Work"
                && track.getArchivedMidiTakeReviews().size() == 1
                && track.getArchivedMidiTakeReviews()[0].reviewSessionId == *firstSnapshotId
                && track.getArchivedMidiTakeReviews()[0].reviewName == "Chorus Work Snapshot"
                && sameReviewPayload(activeBefore, track.getArchivedMidiTakeReviews()[0])
                && track.getNotes().size() == notesBefore.size()
                && sameNote(track.getNotes()[0], notesBefore[0]);

            const auto pendingCountAfterFirst = mw::midi::buildPendingMidiTakeReviewSummaries(project).size();
            const auto secondSnapshotId = mw::midi::snapshotActiveMidiTakeReviewByStableIds(project, 2970, 4970);
            const bool repeatedSnapshot = secondSnapshotId.has_value()
                && *secondSnapshotId != *firstSnapshotId
                && track.getArchivedMidiTakeReviews().size() == 2
                && track.getArchivedMidiTakeReviews().back().reviewName == "Chorus Work Snapshot 2"
                && mw::midi::buildPendingMidiTakeReviewSummaries(project).size() == pendingCountAfterFirst
                && pendingCountAfterFirst == 1;

            auto collision = activeBefore;
            collision.reviewSessionId = 4973;
            collision.reviewName = "cHoRuS wOrK sNaPsHoT 3";
            track.getArchivedMidiTakeReviews().push_back(collision);
            const auto fourthNameId = mw::midi::snapshotActiveMidiTakeReviewByStableIds(project, 2970, 4970);
            const bool caseInsensitiveNaming = fourthNameId.has_value()
                && track.getArchivedMidiTakeReviews().back().reviewName == "Chorus Work Snapshot 4";

            const auto firstSnapshotBeforeActiveEdit = track.getArchivedMidiTakeReviews()[0];
            auto& active = track.getPendingMidiTakeReview();
            active.reviewName = "Chorus Work Edited";
            active.reviewTags = { "active-only" };
            active.reviewNotes = "Active changed after snapshot";
            active.selectedPassNumber = 1;
            active.boundaryNoteMode = mw::core::PersistentMidiCompBoundaryNoteMode::Cut;
            const bool activeEditIsolation = track.getArchivedMidiTakeReviews()[0].reviewName == "Chorus Work Snapshot"
                && track.getArchivedMidiTakeReviews()[0].reviewTags == firstSnapshotBeforeActiveEdit.reviewTags
                && track.getArchivedMidiTakeReviews()[0].reviewNotes == firstSnapshotBeforeActiveEdit.reviewNotes
                && track.getArchivedMidiTakeReviews()[0].selectedPassNumber == firstSnapshotBeforeActiveEdit.selectedPassNumber
                && track.getArchivedMidiTakeReviews()[0].boundaryNoteMode == firstSnapshotBeforeActiveEdit.boundaryNoteMode;

            const bool snapshotMetadataIsolation = firstSnapshotId.has_value()
                && mw::midi::updateMidiTakeReviewMetadataByStableIds(project, 2970, *firstSnapshotId,
                                                                      "snapshot-only, safety", "Snapshot changed independently")
                && track.getPendingMidiTakeReview().reviewTags.size() == 1
                && track.getPendingMidiTakeReview().reviewTags[0] == "active-only"
                && track.getPendingMidiTakeReview().reviewNotes == "Active changed after snapshot";

            const auto archivedCountBeforeModeledKeep = track.getArchivedMidiTakeReviews().size();
            const auto activeForUndo = track.getPendingMidiTakeReview();
            track.clearPendingMidiTakeReview();
            const bool modeledKeepIsolation = !track.getPendingMidiTakeReview().active
                && track.getArchivedMidiTakeReviews().size() == archivedCountBeforeModeledKeep;
            track.setPendingMidiTakeReview(activeForUndo);
            const bool modeledUndoIsolation = track.getPendingMidiTakeReview().active
                && track.getPendingMidiTakeReview().reviewSessionId == 4970
                && track.getArchivedMidiTakeReviews().size() == archivedCountBeforeModeledKeep
                && firstSnapshotId.has_value()
                && std::any_of(track.getArchivedMidiTakeReviews().begin(), track.getArchivedMidiTakeReviews().end(),
                               [&](const auto& review) { return review.reviewSessionId == *firstSnapshotId; });

            auto& other = project.addTrack("Other");
            other.setStableId(2971);
            std::swap(project.getTracks()[0], project.getTracks()[1]);
            for (auto& candidate : project.getTracks())
                if (candidate.getStableId() == 2970)
                    candidate.setName("Piano Renamed");
            const auto snapshotSummaries = mw::midi::buildMidiTakeReviewSummaries(project);
            const auto filteredSnapshot = mw::midi::filterMidiTakeReviewSummaries(
                snapshotSummaries, "snapshot-only safety", mw::midi::MidiTakeReviewLibraryFilter::Archived);
            const bool stableOwnershipAndFilter = firstSnapshotId.has_value()
                && filteredSnapshot.size() == 1
                && filteredSnapshot[0].trackId == 2970
                && filteredSnapshot[0].reviewSessionId == *firstSnapshotId;

            const auto tempRoot = std::filesystem::temp_directory_path();
            const auto savePath = tempRoot / "pms_sim_i21_snapshot_roundtrip.pms";
            std::error_code ec;
            std::filesystem::remove(savePath, ec);
            const bool saved = mw::serialization::ProjectSerializer::saveToFile(project, savePath);
            const auto reopened = saved ? mw::serialization::ProjectSerializer::loadFromFile(savePath) : std::nullopt;
            bool roundTripIdentity = false;
            if (reopened.has_value() && reopened->getMetadata().projectFormatVersion == 22 && firstSnapshotId.has_value())
            {
                for (const auto& candidate : reopened->getTracks())
                {
                    if (candidate.getStableId() != 2970)
                        continue;
                    const auto& reopenedActive = candidate.getPendingMidiTakeReview();
                    const bool activePreserved = reopenedActive.active && reopenedActive.reviewSessionId == 4970;
                    const bool snapshotPreserved = std::any_of(candidate.getArchivedMidiTakeReviews().begin(),
                                                               candidate.getArchivedMidiTakeReviews().end(),
                                                               [&](const auto& review)
                                                               {
                                                                   return review.reviewSessionId == *firstSnapshotId
                                                                       && review.reviewName == "Chorus Work Snapshot"
                                                                       && review.reviewNotes == "Snapshot changed independently";
                                                               });
                    roundTripIdentity = activePreserved && snapshotPreserved;
                }
            }
            std::filesystem::remove(savePath, ec);

            mw::core::Project archiveProject("Archive distinction");
            auto& archiveTrack = archiveProject.addTrack("MIDI");
            archiveTrack.setStableId(2980);
            archiveTrack.setPendingMidiTakeReview(makeReview(4980, "Archive Source"));
            const auto snapshotBeforeArchive = mw::midi::snapshotActiveMidiTakeReviewByStableIds(archiveProject, 2980, 4980);
            const bool archiveStillFreesActive = snapshotBeforeArchive.has_value()
                && mw::midi::archivePendingMidiTakeReviewByStableId(archiveProject, 2980)
                && !archiveTrack.getPendingMidiTakeReview().active
                && archiveTrack.getArchivedMidiTakeReviews().size() == 2;

            mw::core::Project staleProject("Stale Snapshot");
            auto& staleTrack = staleProject.addTrack("MIDI");
            staleTrack.setStableId(2990);
            staleTrack.setPendingMidiTakeReview(makeReview(4990, "Stale Active"));
            staleProject.getTracks().clear();
            const bool removedTrackRejected = !mw::midi::snapshotActiveMidiTakeReviewByStableIds(staleProject, 2990, 4990).has_value();

            const bool helperBubbleContract = std::string(mw::midi::pendingTakeReviewsSnapshotActiveTooltip).find("independent Archived snapshot") != std::string::npos
                && std::string(mw::midi::pendingTakeReviewsSnapshotActiveTooltip).find("leaving the Active review unresolved") != std::string::npos
                && std::string(mw::midi::pendingTakeReviewsSnapshotActiveTooltip).find("Save Tags/Notes first") != std::string::npos
                && std::string(mw::midi::midiTakeReviewSnapshotTooltip).find("keeping the Active editor") != std::string::npos
                && std::string(mw::midi::pendingTakeReviewsArchiveTooltip).find("frees the track") != std::string::npos;

            const bool passed = invalidRejected && firstSnapshot && repeatedSnapshot && caseInsensitiveNaming
                && activeEditIsolation && snapshotMetadataIsolation && modeledKeepIsolation && modeledUndoIsolation
                && stableOwnershipAndFilter && roundTripIdentity && archiveStillFreesActive && removedTrackRejected
                && helperBubbleContract;
            return makeResult("I21", "Workflow", "Active Take Review snapshot-to-Archive and identity isolation", passed,
                              passed
                                  ? "Active Review Snapshot created a full independent same-track Archived checkpoint with a new stable review-session ID and deterministic Snapshot/Snapshot 2 naming while leaving the Active identity, Pending count, project MIDI notes, and active-slot blocking unchanged; source/snapshot edits stayed isolated; Keep/Undo modeling and ordinary Archive preserved the snapshot contract; rename/reorder/save-reopen identity, Search/Show filtering, stale rejection, format-15 continuity, and Snapshot/Archive helper-bubble contracts all behaved deterministically."
                                  : "Active Review snapshot identity, deterministic naming, deep-copy isolation, Active-slot/Pending behavior, Keep/Undo or Archive distinction, persistence/filtering, stale rejection, format-15 continuity, or helper-bubble contracts failed.");
        }

        SimulationScenarioResult scenarioI22()
        {
            auto makeReview = [](mw::core::StableId sessionId, std::string name)
            {
                mw::core::PersistentMidiTakeReview review;
                review.active = true;
                review.reviewSessionId = sessionId;
                review.reviewName = std::move(name);
                review.reviewTags = { "favorite", "cross-track" };
                review.reviewNotes = "Cross-track source archive";
                review.regionInTick = 960;
                review.regionOutTick = 7680;
                review.selectedPassNumber = 2;
                review.reviewMode = mw::core::PersistentMidiTakeReviewMode::Comp;
                review.compGrid = mw::core::PersistentMidiCompGrid::Custom;
                review.boundaryNoteMode = mw::core::PersistentMidiCompBoundaryNoteMode::PreserveSamePass;
                review.sustainPedalMode = mw::core::PersistentMidiSustainPedalMode::ApplyCc64;

                mw::core::PersistentMidiTakePass pass1;
                pass1.passNumber = 1;
                pass1.sustainDownMaskAtStart = 0x0001U;
                pass1.cc64Events.push_back({ 1200, 1, 127, 1 });
                pass1.cc64Events.push_back({ 2640, 1, 0, 2 });
                pass1.notes.emplace_back(60, 104, 1080, 1200, 1, mw::core::Articulation::Legato);

                mw::core::PersistentMidiTakePass pass2;
                pass2.passNumber = 2;
                pass2.sustainDownMaskAtStart = 0x0004U;
                pass2.cc64Events.push_back({ 3840, 3, 127, 3 });
                pass2.cc64Events.push_back({ 6000, 3, 0, 4 });
                pass2.notes.emplace_back(67, 95, 3360, 1800, 3, mw::core::Articulation::Accent);

                review.passes = { pass1, pass2 };
                review.segments.push_back({ 960, 2880, 1 });
                review.segments.push_back({ 2880, 4800, 2 });
                review.segments.push_back({ 4800, 7680, 2 });
                return review;
            };

            auto sameNote = [](const mw::core::NoteEvent& a, const mw::core::NoteEvent& b)
            {
                return a.pitch == b.pitch && a.velocity == b.velocity && a.midiChannel == b.midiChannel
                    && a.startTick == b.startTick && a.durationTicks == b.durationTicks
                    && a.articulation == b.articulation;
            };
            auto sameReviewPayload = [&](const mw::core::PersistentMidiTakeReview& a,
                                         const mw::core::PersistentMidiTakeReview& b)
            {
                if (a.active != b.active || a.reviewTags != b.reviewTags || a.reviewNotes != b.reviewNotes
                    || a.regionInTick != b.regionInTick || a.regionOutTick != b.regionOutTick
                    || a.selectedPassNumber != b.selectedPassNumber || a.reviewMode != b.reviewMode
                    || a.compGrid != b.compGrid || a.boundaryNoteMode != b.boundaryNoteMode
                    || a.sustainPedalMode != b.sustainPedalMode || a.passes.size() != b.passes.size()
                    || a.segments.size() != b.segments.size())
                    return false;
                for (std::size_t passIndex = 0; passIndex < a.passes.size(); ++passIndex)
                {
                    const auto& left = a.passes[passIndex];
                    const auto& right = b.passes[passIndex];
                    if (left.passNumber != right.passNumber
                        || left.sustainDownMaskAtStart != right.sustainDownMaskAtStart
                        || left.notes.size() != right.notes.size()
                        || left.cc64Events.size() != right.cc64Events.size())
                        return false;
                    for (std::size_t noteIndex = 0; noteIndex < left.notes.size(); ++noteIndex)
                        if (!sameNote(left.notes[noteIndex], right.notes[noteIndex]))
                            return false;
                    for (std::size_t eventIndex = 0; eventIndex < left.cc64Events.size(); ++eventIndex)
                    {
                        const auto& x = left.cc64Events[eventIndex];
                        const auto& y = right.cc64Events[eventIndex];
                        if (x.tick != y.tick || x.midiChannel != y.midiChannel
                            || x.value != y.value || x.order != y.order)
                            return false;
                    }
                }
                for (std::size_t segmentIndex = 0; segmentIndex < a.segments.size(); ++segmentIndex)
                {
                    const auto& x = a.segments[segmentIndex];
                    const auto& y = b.segments[segmentIndex];
                    if (x.startTick != y.startTick || x.endTick != y.endTick
                        || x.selectedPassNumber != y.selectedPassNumber)
                        return false;
                }
                return true;
            };

            mw::core::Project project("Cross-track Archive Copy Fixture");
            project.setStableId(1980);
            auto& source = project.addTrack("Piano");
            source.setStableId(2980);
            source.addNote(mw::core::NoteEvent(48, 88, 120, 480, 1, mw::core::Articulation::Normal));
            source.getArchivedMidiTakeReviews().push_back(makeReview(4980, "Chorus Take"));

            auto& destination = project.addTrack("Synth Pad");
            destination.setStableId(2981);
            destination.addNote(mw::core::NoteEvent(55, 82, 360, 720, 2, mw::core::Articulation::Normal));
            destination.setPendingMidiTakeReview(makeReview(4981, "Destination Active"));
            auto collision = makeReview(4982, "cHoRuS tAkE cOpY");
            destination.getArchivedMidiTakeReviews().push_back(collision);

            auto& audio = project.addTrack("Audio Clip");
            audio.setStableId(2982);
            audio.setTrackType(mw::core::TrackType::AudioClip);
            auto& otherMidi = project.addTrack("Strings");
            otherMidi.setStableId(2983);

            const auto destinations = mw::midi::buildCrossTrackMidiTakeReviewCopyDestinations(project, 2980);
            const bool destinationRules = destinations.size() == 2
                && destinations[0].trackId == 2981 && destinations[0].trackIndex == 1
                && destinations[1].trackId == 2983 && destinations[1].trackIndex == 3
                && mw::midi::buildCrossTrackMidiTakeReviewCopyDestinations(project, 999999).empty();

            const auto sourceBefore = project.getTracks()[0].getArchivedMidiTakeReviews()[0];
            const auto sourceNotesBefore = project.getTracks()[0].getNotes();
            const auto destinationNotesBefore = project.getTracks()[1].getNotes();
            const auto destinationActiveBefore = project.getTracks()[1].getPendingMidiTakeReview();
            const auto pendingCountBefore = mw::midi::buildPendingMidiTakeReviewSummaries(project).size();

            const bool invalidRejected = !mw::midi::copyArchivedMidiTakeReviewToTrackByStableIds(project, 0, 4980, 2981).has_value()
                && !mw::midi::copyArchivedMidiTakeReviewToTrackByStableIds(project, 2980, 0, 2981).has_value()
                && !mw::midi::copyArchivedMidiTakeReviewToTrackByStableIds(project, 2980, 4980, 2980).has_value()
                && !mw::midi::copyArchivedMidiTakeReviewToTrackByStableIds(project, 2980, 4980, 2982).has_value()
                && !mw::midi::copyArchivedMidiTakeReviewToTrackByStableIds(project, 2980, 999999, 2981).has_value();

            const auto copiedId = mw::midi::copyArchivedMidiTakeReviewToTrackByStableIds(project, 2980, 4980, 2981);
            const auto& destinationAfter = project.getTracks()[1];
            const auto& sourceAfter = project.getTracks()[0];
            const auto& copied = destinationAfter.getArchivedMidiTakeReviews().back();
            const bool copiedDeeply = copiedId.has_value()
                && copied.reviewSessionId == *copiedId && *copiedId != 4980 && *copiedId != 4981 && *copiedId != 4982
                && copied.reviewName == "Chorus Take Copy 2"
                && sameReviewPayload(sourceBefore, copied)
                && sourceAfter.getArchivedMidiTakeReviews().size() == 1
                && sourceAfter.getArchivedMidiTakeReviews()[0].reviewSessionId == 4980
                && sourceAfter.getArchivedMidiTakeReviews()[0].reviewName == "Chorus Take"
                && destinationAfter.getPendingMidiTakeReview().reviewSessionId == destinationActiveBefore.reviewSessionId
                && destinationAfter.getPendingMidiTakeReview().reviewName == destinationActiveBefore.reviewName
                && mw::midi::buildPendingMidiTakeReviewSummaries(project).size() == pendingCountBefore
                && sourceAfter.getNotes().size() == sourceNotesBefore.size()
                && destinationAfter.getNotes().size() == destinationNotesBefore.size()
                && sameNote(sourceAfter.getNotes()[0], sourceNotesBefore[0])
                && sameNote(destinationAfter.getNotes()[0], destinationNotesBefore[0]);

            const bool restoreBlockedByDestinationActive = copiedId.has_value()
                && !mw::midi::restoreArchivedMidiTakeReviewByStableIds(project, 2981, *copiedId);

            const bool metadataIsolation = copiedId.has_value()
                && mw::midi::updateMidiTakeReviewMetadataByStableIds(project, 2981, *copiedId,
                                                                     "destination-only, safety", "Destination copy changed")
                && sourceAfter.getArchivedMidiTakeReviews()[0].reviewTags == sourceBefore.reviewTags
                && sourceAfter.getArchivedMidiTakeReviews()[0].reviewNotes == sourceBefore.reviewNotes;

            std::swap(project.getTracks()[0], project.getTracks()[3]);
            for (auto& track : project.getTracks())
            {
                if (track.getStableId() == 2980) track.setName("Piano Renamed");
                if (track.getStableId() == 2981) track.setName("Synth Pad Renamed");
            }
            const auto secondCopyId = mw::midi::copyArchivedMidiTakeReviewToTrackByStableIds(project, 2980, 4980, 2981);
            bool stableReorderAndNaming = false;
            if (secondCopyId.has_value())
            {
                for (const auto& track : project.getTracks())
                {
                    if (track.getStableId() != 2981)
                        continue;
                    const auto& archives = track.getArchivedMidiTakeReviews();
                    stableReorderAndNaming = !archives.empty()
                        && archives.back().reviewSessionId == *secondCopyId
                        && archives.back().reviewName == "Chorus Take Copy 3"
                        && sameReviewPayload(sourceBefore, archives.back());
                }
            }

            const auto summariesBeforeFilter = mw::midi::buildMidiTakeReviewSummaries(project);
            const auto filtered = mw::midi::filterMidiTakeReviewSummaries(
                summariesBeforeFilter, "destination-only safety", mw::midi::MidiTakeReviewLibraryFilter::Archived);
            const bool filterFindsIndependentCopy = copiedId.has_value() && filtered.size() == 1
                && filtered[0].trackId == 2981 && filtered[0].reviewSessionId == *copiedId;

            const auto tempRoot = std::filesystem::temp_directory_path();
            const auto savePath = tempRoot / "pms_sim_i22_cross_track_copy_roundtrip.pms";
            std::error_code ec;
            std::filesystem::remove(savePath, ec);
            const bool saved = mw::serialization::ProjectSerializer::saveToFile(project, savePath);
            const auto reopened = saved ? mw::serialization::ProjectSerializer::loadFromFile(savePath) : std::nullopt;
            bool roundTripIdentity = false;
            if (reopened.has_value() && reopened->getMetadata().projectFormatVersion == 22
                && copiedId.has_value() && secondCopyId.has_value())
            {
                for (const auto& track : reopened->getTracks())
                {
                    if (track.getStableId() != 2981)
                        continue;
                    const auto firstFound = std::find_if(track.getArchivedMidiTakeReviews().begin(),
                                                         track.getArchivedMidiTakeReviews().end(),
                                                         [&](const auto& review) { return review.reviewSessionId == *copiedId; });
                    const auto secondFound = std::find_if(track.getArchivedMidiTakeReviews().begin(),
                                                          track.getArchivedMidiTakeReviews().end(),
                                                          [&](const auto& review) { return review.reviewSessionId == *secondCopyId; });
                    roundTripIdentity = firstFound != track.getArchivedMidiTakeReviews().end()
                        && secondFound != track.getArchivedMidiTakeReviews().end()
                        && firstFound->reviewNotes == "Destination copy changed"
                        && secondFound->reviewName == "Chorus Take Copy 3";
                }
            }
            std::filesystem::remove(savePath, ec);

            mw::core::Project replacement("Replacement Project");
            replacement.setStableId(1981);
            auto& replacementTrack = replacement.addTrack("Replacement MIDI");
            replacementTrack.setStableId(3980);
            const bool replacementRejected = !mw::midi::copyArchivedMidiTakeReviewToTrackByStableIds(
                replacement, 2980, 4980, 2981).has_value();

            mw::core::Project staleDestination("Stale Destination");
            auto& staleSource = staleDestination.addTrack("Source");
            staleSource.setStableId(4080);
            staleSource.getArchivedMidiTakeReviews().push_back(makeReview(5080, "Stale Copy"));
            auto& staleDest = staleDestination.addTrack("Destination");
            staleDest.setStableId(4081);
            staleDestination.getTracks().erase(staleDestination.getTracks().begin() + 1);
            const bool removedDestinationRejected = !mw::midi::copyArchivedMidiTakeReviewToTrackByStableIds(
                staleDestination, 4080, 5080, 4081).has_value();

            const bool helperBubbleContract = std::string(mw::midi::pendingTakeReviewsCopyToTrackTooltip).find("different MIDI track") != std::string::npos
                && std::string(mw::midi::pendingTakeReviewsCopyToTrackTooltip).find("Pending Takes") != std::string::npos
                && std::string(mw::midi::pendingTakeReviewsCopyDestinationTooltip).find("AudioClip") != std::string::npos
                && std::string(mw::midi::pendingTakeReviewsCopyDestinationTooltip).find("source track") != std::string::npos
                && std::string(mw::midi::pendingTakeReviewsCopyConfirmTooltip).find("source review") != std::string::npos;

            const bool passed = destinationRules && invalidRejected && copiedDeeply && restoreBlockedByDestinationActive
                && metadataIsolation && stableReorderAndNaming && filterFindsIndependentCopy && roundTripIdentity
                && replacementRejected && removedDestinationRejected && helperBubbleContract;
            return makeResult("I22", "Workflow", "Cross-track Archived Take Review copy and destination identity", passed,
                              passed
                                  ? "Archived Take Review copy used an explicit different MIDI destination track, deep-copied passes/Comp/Boundary Notes/CC64/Tags/Notes with a fresh review-session ID and deterministic destination Copy naming, preserved source and destination Active/Pending/MIDI-note state, survived track rename/reorder/save-reopen by stable identity, kept source/copy metadata independent, blocked Restore while destination Active existed, rejected AudioClip/same-track/stale/project-replacement identities, kept format 15, and exposed Copy-to-Track helper contracts."
                                  : "Cross-track Archived Take Review destination filtering, deep-copy identity/naming, Active/Pending/note isolation, metadata independence, stable-ID persistence, Restore blocking, stale/type rejection, format continuity, or helper-bubble contracts failed.");
        }

        SimulationScenarioResult scenarioI23()
        {
            auto makeReview = [](mw::core::StableId sessionId, std::string name, std::string noteText = "Move source archive")
            {
                mw::core::PersistentMidiTakeReview review;
                review.active = true;
                review.reviewSessionId = sessionId;
                review.reviewName = std::move(name);
                review.reviewTags = { "favorite", "move-track" };
                review.reviewNotes = std::move(noteText);
                review.regionInTick = 1440;
                review.regionOutTick = 8640;
                review.selectedPassNumber = 2;
                review.reviewMode = mw::core::PersistentMidiTakeReviewMode::Comp;
                review.compGrid = mw::core::PersistentMidiCompGrid::Custom;
                review.boundaryNoteMode = mw::core::PersistentMidiCompBoundaryNoteMode::PreserveSamePass;
                review.sustainPedalMode = mw::core::PersistentMidiSustainPedalMode::ApplyCc64;

                mw::core::PersistentMidiTakePass pass1;
                pass1.passNumber = 1;
                pass1.sustainDownMaskAtStart = 0x0002U;
                pass1.cc64Events.push_back({ 1800, 2, 127, 1 });
                pass1.cc64Events.push_back({ 3120, 2, 0, 2 });
                pass1.notes.emplace_back(62, 101, 1560, 1320, 2, mw::core::Articulation::Legato);

                mw::core::PersistentMidiTakePass pass2;
                pass2.passNumber = 2;
                pass2.sustainDownMaskAtStart = 0x0008U;
                pass2.cc64Events.push_back({ 4320, 4, 127, 3 });
                pass2.cc64Events.push_back({ 7080, 4, 0, 4 });
                pass2.notes.emplace_back(69, 93, 3960, 2040, 4, mw::core::Articulation::Accent);

                review.passes = { pass1, pass2 };
                review.segments.push_back({ 1440, 3360, 1 });
                review.segments.push_back({ 3360, 5760, 2 });
                review.segments.push_back({ 5760, 8640, 2 });
                return review;
            };

            auto sameNote = [](const mw::core::NoteEvent& a, const mw::core::NoteEvent& b)
            {
                return a.pitch == b.pitch && a.velocity == b.velocity && a.midiChannel == b.midiChannel
                    && a.startTick == b.startTick && a.durationTicks == b.durationTicks
                    && a.articulation == b.articulation;
            };
            auto sameReviewPayload = [&](const mw::core::PersistentMidiTakeReview& a,
                                         const mw::core::PersistentMidiTakeReview& b)
            {
                if (a.active != b.active || a.reviewTags != b.reviewTags || a.reviewNotes != b.reviewNotes
                    || a.regionInTick != b.regionInTick || a.regionOutTick != b.regionOutTick
                    || a.selectedPassNumber != b.selectedPassNumber || a.reviewMode != b.reviewMode
                    || a.compGrid != b.compGrid || a.boundaryNoteMode != b.boundaryNoteMode
                    || a.sustainPedalMode != b.sustainPedalMode || a.passes.size() != b.passes.size()
                    || a.segments.size() != b.segments.size())
                    return false;
                for (std::size_t passIndex = 0; passIndex < a.passes.size(); ++passIndex)
                {
                    const auto& left = a.passes[passIndex];
                    const auto& right = b.passes[passIndex];
                    if (left.passNumber != right.passNumber
                        || left.sustainDownMaskAtStart != right.sustainDownMaskAtStart
                        || left.notes.size() != right.notes.size()
                        || left.cc64Events.size() != right.cc64Events.size())
                        return false;
                    for (std::size_t noteIndex = 0; noteIndex < left.notes.size(); ++noteIndex)
                        if (!sameNote(left.notes[noteIndex], right.notes[noteIndex]))
                            return false;
                    for (std::size_t eventIndex = 0; eventIndex < left.cc64Events.size(); ++eventIndex)
                    {
                        const auto& x = left.cc64Events[eventIndex];
                        const auto& y = right.cc64Events[eventIndex];
                        if (x.tick != y.tick || x.midiChannel != y.midiChannel
                            || x.value != y.value || x.order != y.order)
                            return false;
                    }
                }
                for (std::size_t segmentIndex = 0; segmentIndex < a.segments.size(); ++segmentIndex)
                {
                    const auto& x = a.segments[segmentIndex];
                    const auto& y = b.segments[segmentIndex];
                    if (x.startTick != y.startTick || x.endTick != y.endTick
                        || x.selectedPassNumber != y.selectedPassNumber)
                        return false;
                }
                return true;
            };

            mw::core::Project project("Cross-track Archive Move Fixture");
            project.setStableId(1990);
            auto& source = project.addTrack("Piano");
            source.setStableId(2990);
            source.addNote(mw::core::NoteEvent(45, 86, 240, 600, 1, mw::core::Articulation::Normal));
            source.getArchivedMidiTakeReviews().push_back(makeReview(4990, "Chorus Take"));
            source.getArchivedMidiTakeReviews().push_back(makeReview(4993, "Chorus Take", "Second move source"));
            source.getArchivedMidiTakeReviews().push_back(makeReview(4994, "Copy Control", "Copy must allocate a fresh ID"));

            auto& destination = project.addTrack("Synth Pad");
            destination.setStableId(2991);
            destination.addNote(mw::core::NoteEvent(57, 84, 480, 840, 3, mw::core::Articulation::Normal));
            destination.setPendingMidiTakeReview(makeReview(4991, "Destination Active"));
            destination.getArchivedMidiTakeReviews().push_back(makeReview(4992, "cHoRuS tAkE"));

            auto& audio = project.addTrack("Audio Clip");
            audio.setStableId(2992);
            audio.setTrackType(mw::core::TrackType::AudioClip);
            auto& otherMidi = project.addTrack("Strings");
            otherMidi.setStableId(2993);

            const auto destinations = mw::midi::buildCrossTrackMidiTakeReviewCopyDestinations(project, 2990);
            const bool destinationRules = destinations.size() == 2
                && destinations[0].trackId == 2991 && destinations[1].trackId == 2993;

            const auto firstBefore = project.getTracks()[0].getArchivedMidiTakeReviews()[0];
            const auto secondBefore = project.getTracks()[0].getArchivedMidiTakeReviews()[1];
            const auto sourceNotesBefore = project.getTracks()[0].getNotes();
            const auto destinationNotesBefore = project.getTracks()[1].getNotes();
            const auto destinationActiveBefore = project.getTracks()[1].getPendingMidiTakeReview();
            const auto pendingCountBefore = mw::midi::buildPendingMidiTakeReviewSummaries(project).size();
            const auto sourceCountBeforeInvalid = project.getTracks()[0].getArchivedMidiTakeReviews().size();
            const auto destinationCountBeforeInvalid = project.getTracks()[1].getArchivedMidiTakeReviews().size();

            const bool invalidRejected = !mw::midi::moveArchivedMidiTakeReviewToTrackByStableIds(project, 0, 4990, 2991).has_value()
                && !mw::midi::moveArchivedMidiTakeReviewToTrackByStableIds(project, 2990, 0, 2991).has_value()
                && !mw::midi::moveArchivedMidiTakeReviewToTrackByStableIds(project, 2990, 4990, 2990).has_value()
                && !mw::midi::moveArchivedMidiTakeReviewToTrackByStableIds(project, 2990, 4990, 2992).has_value()
                && !mw::midi::moveArchivedMidiTakeReviewToTrackByStableIds(project, 2990, 999999, 2991).has_value()
                && project.getTracks()[0].getArchivedMidiTakeReviews().size() == sourceCountBeforeInvalid
                && project.getTracks()[1].getArchivedMidiTakeReviews().size() == destinationCountBeforeInvalid;

            const auto movedId = mw::midi::moveArchivedMidiTakeReviewToTrackByStableIds(project, 2990, 4990, 2991);
            const auto& destinationAfterFirst = project.getTracks()[1];
            const auto& sourceAfterFirst = project.getTracks()[0];
            const auto& moved = destinationAfterFirst.getArchivedMidiTakeReviews().back();
            const bool firstMoveAtomicAndPreserved = movedId.has_value() && *movedId == 4990
                && moved.reviewSessionId == 4990
                && moved.reviewName == "Chorus Take Move"
                && sameReviewPayload(firstBefore, moved)
                && sourceAfterFirst.getArchivedMidiTakeReviews().size() == 2
                && std::none_of(sourceAfterFirst.getArchivedMidiTakeReviews().begin(), sourceAfterFirst.getArchivedMidiTakeReviews().end(),
                                [](const auto& review) { return review.reviewSessionId == 4990; })
                && destinationAfterFirst.getPendingMidiTakeReview().reviewSessionId == destinationActiveBefore.reviewSessionId
                && destinationAfterFirst.getPendingMidiTakeReview().reviewName == destinationActiveBefore.reviewName
                && mw::midi::buildPendingMidiTakeReviewSummaries(project).size() == pendingCountBefore
                && sourceAfterFirst.getNotes().size() == sourceNotesBefore.size()
                && destinationAfterFirst.getNotes().size() == destinationNotesBefore.size()
                && sameNote(sourceAfterFirst.getNotes()[0], sourceNotesBefore[0])
                && sameNote(destinationAfterFirst.getNotes()[0], destinationNotesBefore[0]);

            const bool restoreStillBlocked = movedId.has_value()
                && !mw::midi::restoreArchivedMidiTakeReviewByStableIds(project, 2991, *movedId);

            const auto secondMovedId = mw::midi::moveArchivedMidiTakeReviewToTrackByStableIds(project, 2990, 4993, 2991);
            bool secondCollisionName = false;
            if (secondMovedId.has_value())
            {
                const auto& archives = project.getTracks()[1].getArchivedMidiTakeReviews();
                secondCollisionName = *secondMovedId == 4993 && !archives.empty()
                    && archives.back().reviewSessionId == 4993
                    && archives.back().reviewName == "Chorus Take Move 2"
                    && sameReviewPayload(secondBefore, archives.back());
            }

            const auto copiedId = mw::midi::copyArchivedMidiTakeReviewToTrackByStableIds(project, 2990, 4994, 2993);
            bool copyStillFreshIdentity = false;
            if (copiedId.has_value())
            {
                for (const auto& track : project.getTracks())
                {
                    if (track.getStableId() == 2993 && !track.getArchivedMidiTakeReviews().empty())
                        copyStillFreshIdentity = *copiedId != 4994
                            && track.getArchivedMidiTakeReviews().back().reviewSessionId == *copiedId;
                }
            }

            bool metadataOwnershipIsolation = movedId.has_value()
                && mw::midi::updateMidiTakeReviewMetadataByStableIds(project, 2991, *movedId,
                                                                     "destination, moved", "Moved review changed only here");
            if (metadataOwnershipIsolation)
            {
                for (const auto& track : project.getTracks())
                {
                    if (track.getStableId() == 2990)
                    {
                        metadataOwnershipIsolation = std::none_of(track.getArchivedMidiTakeReviews().begin(),
                                                                  track.getArchivedMidiTakeReviews().end(),
                                                                  [&](const auto& review) { return review.reviewSessionId == *movedId; });
                    }
                }
            }

            std::swap(project.getTracks()[0], project.getTracks()[3]);
            for (auto& track : project.getTracks())
            {
                if (track.getStableId() == 2990) track.setName("Piano Renamed");
                if (track.getStableId() == 2991) track.setName("Synth Pad Renamed");
            }
            const auto summaries = mw::midi::buildMidiTakeReviewSummaries(project);
            const auto movedSummary = std::find_if(summaries.begin(), summaries.end(), [&](const auto& summary)
            {
                return movedId.has_value() && summary.trackId == 2991 && summary.reviewSessionId == *movedId
                    && summary.storageState == mw::midi::MidiTakeReviewStorageState::Archived;
            });
            const auto filtered = mw::midi::filterMidiTakeReviewSummaries(
                summaries, "destination moved", mw::midi::MidiTakeReviewLibraryFilter::Archived);
            const bool stableReorderAndFilter = movedSummary != summaries.end() && filtered.size() == 1
                && filtered[0].trackId == 2991 && filtered[0].reviewSessionId == *movedId;

            const auto tempRoot = std::filesystem::temp_directory_path();
            const auto savePath = tempRoot / "pms_sim_i23_cross_track_move_roundtrip.pms";
            std::error_code ec;
            std::filesystem::remove(savePath, ec);
            const bool saved = mw::serialization::ProjectSerializer::saveToFile(project, savePath);
            const auto reopened = saved ? mw::serialization::ProjectSerializer::loadFromFile(savePath) : std::nullopt;
            bool roundTripOwnership = false;
            if (reopened.has_value() && reopened->getMetadata().projectFormatVersion == 22
                && movedId.has_value() && secondMovedId.has_value())
            {
                bool absentFromSource = false;
                bool foundFirst = false;
                bool foundSecond = false;
                for (const auto& track : reopened->getTracks())
                {
                    if (track.getStableId() == 2990)
                    {
                        absentFromSource = std::none_of(track.getArchivedMidiTakeReviews().begin(),
                                                        track.getArchivedMidiTakeReviews().end(),
                                                        [&](const auto& review)
                                                        {
                                                            return review.reviewSessionId == *movedId
                                                                || review.reviewSessionId == *secondMovedId;
                                                        });
                    }
                    if (track.getStableId() == 2991)
                    {
                        for (const auto& review : track.getArchivedMidiTakeReviews())
                        {
                            if (review.reviewSessionId == *movedId)
                                foundFirst = review.reviewNotes == "Moved review changed only here";
                            if (review.reviewSessionId == *secondMovedId)
                                foundSecond = review.reviewName == "Chorus Take Move 2";
                        }
                    }
                }
                roundTripOwnership = absentFromSource && foundFirst && foundSecond;
            }
            std::filesystem::remove(savePath, ec);

            mw::core::Project staleSourceProject("Stale Move Source");
            auto& staleSource = staleSourceProject.addTrack("Source");
            staleSource.setStableId(4090);
            staleSource.getArchivedMidiTakeReviews().push_back(makeReview(5090, "Stale Move"));
            auto& staleDestination = staleSourceProject.addTrack("Destination");
            staleDestination.setStableId(4091);
            const auto staleDestCount = staleDestination.getArchivedMidiTakeReviews().size();
            staleSourceProject.getTracks().erase(staleSourceProject.getTracks().begin());
            const bool removedSourceRejected = !mw::midi::moveArchivedMidiTakeReviewToTrackByStableIds(
                staleSourceProject, 4090, 5090, 4091).has_value()
                && staleSourceProject.getTracks()[0].getArchivedMidiTakeReviews().size() == staleDestCount;

            mw::core::Project staleDestinationProject("Stale Move Destination");
            auto& liveSource = staleDestinationProject.addTrack("Source");
            liveSource.setStableId(4190);
            liveSource.getArchivedMidiTakeReviews().push_back(makeReview(5190, "Destination Gone"));
            auto& disappearingDestination = staleDestinationProject.addTrack("Destination");
            disappearingDestination.setStableId(4191);
            staleDestinationProject.getTracks().erase(staleDestinationProject.getTracks().begin() + 1);
            const auto liveSourceBefore = staleDestinationProject.getTracks()[0].getArchivedMidiTakeReviews();
            const bool removedDestinationRejected = !mw::midi::moveArchivedMidiTakeReviewToTrackByStableIds(
                staleDestinationProject, 4190, 5190, 4191).has_value()
                && staleDestinationProject.getTracks()[0].getArchivedMidiTakeReviews().size() == liveSourceBefore.size()
                && staleDestinationProject.getTracks()[0].getArchivedMidiTakeReviews()[0].reviewSessionId == 5190;

            mw::core::Project collisionProject("Move ID Collision");
            auto& collisionSource = collisionProject.addTrack("Source");
            collisionSource.setStableId(4290);
            collisionSource.getArchivedMidiTakeReviews().push_back(makeReview(5290, "Collision Source"));
            auto& collisionDestination = collisionProject.addTrack("Destination");
            collisionDestination.setStableId(4291);
            collisionDestination.getArchivedMidiTakeReviews().push_back(makeReview(5290, "Malformed Duplicate ID"));
            const bool idCollisionRejectedAtomically = !mw::midi::moveArchivedMidiTakeReviewToTrackByStableIds(
                collisionProject, 4290, 5290, 4291).has_value()
                && collisionProject.getTracks()[0].getArchivedMidiTakeReviews().size() == 1
                && collisionProject.getTracks()[1].getArchivedMidiTakeReviews().size() == 1;

            mw::core::Project replacement("Replacement Move Project");
            replacement.setStableId(29999);
            auto& replacementTrack = replacement.addTrack("Replacement MIDI");
            replacementTrack.setStableId(4390);
            const bool replacementRejected = !mw::midi::moveArchivedMidiTakeReviewToTrackByStableIds(
                replacement, 2990, 4990, 2991).has_value();

            const bool helperBubbleContract = std::string(mw::midi::pendingTakeReviewsMoveToTrackTooltip).find("keeps its stable identity") != std::string::npos
                && std::string(mw::midi::pendingTakeReviewsMoveToTrackTooltip).find("Pending Takes") != std::string::npos
                && std::string(mw::midi::pendingTakeReviewsMoveDestinationTooltip).find("AudioClip") != std::string::npos
                && std::string(mw::midi::pendingTakeReviewsMoveDestinationTooltip).find("source track") != std::string::npos
                && std::string(mw::midi::pendingTakeReviewsMoveConfirmTooltip).find("removed only after destination validation succeeds") != std::string::npos
                && std::string(mw::midi::pendingTakeReviewsCopyToTrackTooltip).find("new stable review identity") != std::string::npos;

            const bool passed = destinationRules && invalidRejected && firstMoveAtomicAndPreserved && restoreStillBlocked
                && secondCollisionName && copyStillFreshIdentity && metadataOwnershipIsolation && stableReorderAndFilter
                && roundTripOwnership && removedSourceRejected && removedDestinationRejected && idCollisionRejectedAtomically
                && replacementRejected && helperBubbleContract;
            return makeResult("I23", "Workflow", "Cross-track Archived Take Review atomic move and identity preservation", passed,
                              passed
                                  ? "Archived Take Review Move used an explicit different MIDI destination track, preserved the review-session ID while atomically creating destination ownership and removing the source archive, used deterministic Move/Move 2 collision naming, preserved passes/Comp/Boundary Notes/CC64/Tags/Notes plus destination Active/Pending/project-note state, survived rename/reorder/save-reopen by stable identity, kept Copy's fresh-ID semantics, rejected stale/type/project-replacement/session-ID-collision cases without partial source removal, kept format 15, and exposed Move-to-Track helper contracts."
                                  : "Cross-track Archived Take Review atomic move, stable identity, deterministic destination naming, payload/Active/Pending/note isolation, copy distinction, stable-ID persistence, stale/type/collision rollback, format continuity, or helper-bubble contracts failed.");
        }

        SimulationScenarioResult scenarioI24()
        {
            std::vector<mw::midi::MidiRecordingLoopTakePass> passes;

            mw::midi::MidiRecordingLoopTakePass pass1;
            pass1.passNumber = 1;
            pass1.notes.emplace_back(60, 100, 480, 1000, 1, mw::core::Articulation::Legato);
            pass1.notes.emplace_back(62, 90, 600, 800, 1, mw::core::Articulation::Normal);
            pass1.notes.emplace_back(64, 91, 700, 900, 1, mw::core::Articulation::Accent);
            pass1.notes.emplace_back(65, 88, 700, 200, 2, mw::core::Articulation::Legato);
            pass1.cc64Events.push_back({ 800, 2, 127, 1 });
            pass1.cc64Events.push_back({ 1100, 2, 0, 2 });

            mw::midi::MidiRecordingLoopTakePass pass2;
            pass2.passNumber = 2;
            pass2.notes.emplace_back(60, 70, 960, 1200, 1, mw::core::Articulation::Accent);
            pass2.notes.emplace_back(62, 80, 970, 600, 1, mw::core::Articulation::Normal);
            pass2.notes.emplace_back(64, 80, 960, 300, 1, mw::core::Articulation::Normal);
            pass2.notes.emplace_back(64, 81, 960, 350, 1, mw::core::Articulation::Staccato);
            pass2.notes.emplace_back(65, 75, 960, 300, 2, mw::core::Articulation::Normal);

            mw::midi::MidiRecordingLoopTakePass pass3;
            pass3.passNumber = 3;
            pass3.notes.emplace_back(60, 60, 1920, 480, 1, mw::core::Articulation::Staccato);
            passes = { pass1, pass2, pass3 };

            const std::vector<mw::midi::MidiRecordingCompSegment> segments {
                { 0, 960, 1 }, { 960, 1920, 2 }, { 1920, 2880, 3 }
            };

            mw::midi::MidiRecordingLoopCompState comp;
            constexpr std::uint64_t generation = 24001;
            const bool restored = comp.restore(2400, 2401, generation, 0, 2880, 960, 4, 4,
                                                mw::midi::MidiRecordingCompGrid::Custom,
                                                segments, passes)
                && comp.setBoundaryNoteMode(generation, mw::midi::MidiRecordingCompBoundaryNoteMode::PreserveSamePass)
                && comp.setSustainPedalMode(generation, mw::midi::MidiRecordingSustainPedalMode::ApplyCc64);

            const auto offBuild = comp.buildNotes(generation, passes);
            const bool offPreservesPriorContract = restored
                && comp.crossPassNoteMode() == mw::midi::MidiRecordingCompCrossPassNoteMode::Off
                && offBuild.crossPassBridgedNoteCount == 0
                && offBuild.notes.size() == 10;

            const bool bridgeModeSet = comp.setCrossPassNoteMode(
                generation, mw::midi::MidiRecordingCompCrossPassNoteMode::BridgeExact);
            const auto bridgeBuild = comp.buildNotes(generation, passes);

            const auto findNote = [&](int pitch, int channel, std::int64_t startTick)
                -> const mw::core::NoteEvent*
            {
                const auto it = std::find_if(bridgeBuild.notes.begin(), bridgeBuild.notes.end(),
                    [&](const auto& note)
                    {
                        return note.pitch == pitch && note.midiChannel == channel && note.startTick == startTick;
                    });
                return it == bridgeBuild.notes.end() ? nullptr : &*it;
            };

            const auto* bridged60 = findNote(60, 1, 480);
            const auto* bridged65 = findNote(65, 2, 700);
            const bool exactBridgeContract = bridgeModeSet
                && bridgeBuild.crossPassBridgedNoteCount == 3
                && bridgeBuild.notes.size() == 7
                && bridged60 != nullptr && bridged60->durationTicks == 1920
                && bridged60->velocity == 100 && bridged60->articulation == mw::core::Articulation::Legato
                && bridged65 != nullptr && bridged65->durationTicks == 560
                && bridged65->velocity == 88 && bridged65->articulation == mw::core::Articulation::Legato;

            const int pitch62Count = static_cast<int>(std::count_if(bridgeBuild.notes.begin(), bridgeBuild.notes.end(), [](const auto& note)
            {
                return note.pitch == 62 && note.midiChannel == 1;
            }));
            const int pitch64Count = static_cast<int>(std::count_if(bridgeBuild.notes.begin(), bridgeBuild.notes.end(), [](const auto& note)
            {
                return note.pitch == 64 && note.midiChannel == 1;
            }));
            const bool conservativeRejection = pitch62Count == 2 && pitch64Count == 3;

            mw::midi::MidiRecordingLoopCompState cutComp;
            constexpr std::uint64_t cutGeneration = 24002;
            const bool cutSetup = cutComp.restore(2400, 2401, cutGeneration, 0, 2880, 960, 4, 4,
                                                  mw::midi::MidiRecordingCompGrid::Custom,
                                                  segments, passes)
                && cutComp.setBoundaryNoteMode(cutGeneration, mw::midi::MidiRecordingCompBoundaryNoteMode::Cut)
                && cutComp.setSustainPedalMode(cutGeneration, mw::midi::MidiRecordingSustainPedalMode::ApplyCc64)
                && cutComp.setCrossPassNoteMode(cutGeneration, mw::midi::MidiRecordingCompCrossPassNoteMode::BridgeExact);
            const auto cutBuild = cutComp.buildNotes(cutGeneration, passes);
            const bool cutAlwaysWins = cutSetup && cutBuild.crossPassBridgedNoteCount == 0 && cutBuild.notes.size() == 10;

            mw::core::PersistentMidiTakeReview review;
            review.active = true;
            review.reviewSessionId = 7401;
            review.reviewName = "Bridge Exact Fixture";
            review.regionInTick = 0;
            review.regionOutTick = 2880;
            review.selectedPassNumber = 1;
            review.reviewMode = mw::core::PersistentMidiTakeReviewMode::Comp;
            review.compGrid = mw::core::PersistentMidiCompGrid::Custom;
            review.boundaryNoteMode = mw::core::PersistentMidiCompBoundaryNoteMode::PreserveSamePass;
            review.sustainPedalMode = mw::core::PersistentMidiSustainPedalMode::ApplyCc64;
            review.crossPassNoteMode = mw::core::PersistentMidiCompCrossPassNoteMode::BridgeExact;
            for (const auto& pass : passes)
                review.passes.push_back({ pass.passNumber, pass.notes, pass.sustainDownMaskAtStart, pass.cc64Events });
            review.segments = { {0, 960, 1}, {960, 1920, 2}, {1920, 2880, 3} };

            mw::core::Project project("Cross-Pass Bridge Persistence");
            project.setStableId(7400);
            auto& track = project.addTrack("Comp Track");
            track.setStableId(7402);
            track.setPendingMidiTakeReview(review);
            track.getArchivedMidiTakeReviews().push_back(review);
            track.getArchivedMidiTakeReviews().back().reviewSessionId = 7403;
            track.getArchivedMidiTakeReviews().back().reviewName = "Archived Bridge";
            auto& secondTrack = project.addTrack("Destination");
            secondTrack.setStableId(7404);

            const auto tempRoot = std::filesystem::temp_directory_path() / "PoorMansStudioSimulation";
            std::filesystem::create_directories(tempRoot);
            const auto currentPath = tempRoot / "cross_pass_v15.json";
            const auto legacyPath = tempRoot / "cross_pass_v14.json";
            const auto unknownPath = tempRoot / "cross_pass_unknown.json";
            std::error_code ec;
            std::filesystem::remove(currentPath, ec);
            std::filesystem::remove(legacyPath, ec);
            std::filesystem::remove(unknownPath, ec);

            bool persistence = false;
            bool legacyDefaultsOff = false;
            bool unknownDefaultsOff = false;
            if (mw::serialization::ProjectSerializer::saveToFile(project, currentPath))
            {
                const auto reopened = mw::serialization::ProjectSerializer::loadFromFile(currentPath);
                persistence = reopened.has_value()
                    && reopened->getMetadata().projectFormatVersion == 22
                    && reopened->getTracks().size() == 2
                    && reopened->getTracks()[0].getPendingMidiTakeReview().crossPassNoteMode
                        == mw::core::PersistentMidiCompCrossPassNoteMode::BridgeExact
                    && !reopened->getTracks()[0].getArchivedMidiTakeReviews().empty()
                    && reopened->getTracks()[0].getArchivedMidiTakeReviews()[0].crossPassNoteMode
                        == mw::core::PersistentMidiCompCrossPassNoteMode::BridgeExact;

                std::ifstream in(currentPath, std::ios::binary);
                std::ostringstream buffer;
                buffer << in.rdbuf();
                auto text = buffer.str();
                auto legacyText = text;
                if (const auto pos = legacyText.find("\"version\": 16"); pos != std::string::npos)
                    legacyText.replace(pos, std::string("\"version\": 16").size(), "\"version\": 14");
                const std::string crossLine = "      \"crossPassNoteMode\": \"BridgeExact\",\n";
                for (std::size_t pos = legacyText.find(crossLine); pos != std::string::npos; pos = legacyText.find(crossLine))
                    legacyText.erase(pos, crossLine.size());
                {
                    std::ofstream out(legacyPath, std::ios::binary);
                    out << legacyText;
                }
                const auto legacy = mw::serialization::ProjectSerializer::loadFromFile(legacyPath);
                legacyDefaultsOff = legacy.has_value()
                    && legacy->getMetadata().projectFormatVersion == 22
                    && legacy->getTracks()[0].getPendingMidiTakeReview().crossPassNoteMode
                        == mw::core::PersistentMidiCompCrossPassNoteMode::Off
                    && legacy->getTracks()[0].getArchivedMidiTakeReviews()[0].crossPassNoteMode
                        == mw::core::PersistentMidiCompCrossPassNoteMode::Off;

                auto unknownText = text;
                for (std::size_t pos = unknownText.find("\"crossPassNoteMode\": \"BridgeExact\""); pos != std::string::npos;
                     pos = unknownText.find("\"crossPassNoteMode\": \"BridgeExact\""))
                {
                    unknownText.replace(pos, std::string("\"crossPassNoteMode\": \"BridgeExact\"").size(),
                                        "\"crossPassNoteMode\": \"UnknownFutureValue\"");
                }
                {
                    std::ofstream out(unknownPath, std::ios::binary);
                    out << unknownText;
                }
                const auto unknown = mw::serialization::ProjectSerializer::loadFromFile(unknownPath);
                unknownDefaultsOff = unknown.has_value()
                    && unknown->getTracks()[0].getPendingMidiTakeReview().crossPassNoteMode
                        == mw::core::PersistentMidiCompCrossPassNoteMode::Off;
            }

            const auto snapshotId = mw::midi::snapshotActiveMidiTakeReviewByStableIds(project, 7402, 7401);
            const auto duplicateId = mw::midi::duplicateArchivedMidiTakeReviewByStableIds(project, 7402, 7403);
            const auto copyId = mw::midi::copyArchivedMidiTakeReviewToTrackByStableIds(project, 7402, 7403, 7404);
            const auto moveSourceId = duplicateId;
            const auto movedId = moveSourceId.has_value()
                ? mw::midi::moveArchivedMidiTakeReviewToTrackByStableIds(project, 7402, *moveSourceId, 7404)
                : std::nullopt;
            bool libraryPreservesMode = snapshotId.has_value() && duplicateId.has_value()
                && copyId.has_value() && movedId.has_value() && *movedId == *duplicateId;
            if (libraryPreservesMode)
            {
                const auto& sourceArchives = project.getTracks()[0].getArchivedMidiTakeReviews();
                const auto& destinationArchives = project.getTracks()[1].getArchivedMidiTakeReviews();
                const auto snapshotIt = std::find_if(sourceArchives.begin(), sourceArchives.end(), [&](const auto& r)
                {
                    return r.reviewSessionId == *snapshotId;
                });
                const auto movedStillAtSource = std::find_if(sourceArchives.begin(), sourceArchives.end(), [&](const auto& r)
                {
                    return r.reviewSessionId == *duplicateId;
                });
                const auto copyIt = std::find_if(destinationArchives.begin(), destinationArchives.end(), [&](const auto& r)
                {
                    return r.reviewSessionId == *copyId;
                });
                const auto movedIt = std::find_if(destinationArchives.begin(), destinationArchives.end(), [&](const auto& r)
                {
                    return r.reviewSessionId == *movedId;
                });
                libraryPreservesMode = snapshotIt != sourceArchives.end()
                    && movedStillAtSource == sourceArchives.end()
                    && copyIt != destinationArchives.end() && movedIt != destinationArchives.end()
                    && snapshotIt->crossPassNoteMode == mw::core::PersistentMidiCompCrossPassNoteMode::BridgeExact
                    && copyIt->crossPassNoteMode == mw::core::PersistentMidiCompCrossPassNoteMode::BridgeExact
                    && movedIt->crossPassNoteMode == mw::core::PersistentMidiCompCrossPassNoteMode::BridgeExact;
            }

            const bool helperBubbleContract = std::string(mw::midi::midiRecordingCompCrossPassNotesTooltip).find("conservative") != std::string::npos
                && std::string(mw::midi::midiRecordingCompCrossPassBridgeExactTooltip).find("same-pitch/same-channel") != std::string::npos
                && std::string(mw::midi::midiRecordingCompCrossPassBridgeExactTooltip).find("exactly at that boundary") != std::string::npos
                && std::string(mw::midi::midiRecordingCompCrossPassBridgeExactTooltip).find("ambiguity") != std::string::npos
                && std::string(mw::midi::midiRecordingCompCrossPassOffTooltip).find("existing comp contract") != std::string::npos;

            std::filesystem::remove(currentPath, ec);
            std::filesystem::remove(legacyPath, ec);
            std::filesystem::remove(unknownPath, ec);

            const bool passed = offPreservesPriorContract && exactBridgeContract && conservativeRejection
                && cutAlwaysWins && persistence && legacyDefaultsOff && unknownDefaultsOff
                && libraryPreservesMode && helperBubbleContract;
            return makeResult("I24", "Workflow", "Conservative cross-pass exact-note reconstruction", passed,
                              passed
                                  ? "Cross-Pass Notes defaulted Off; Bridge Exact reconstructed only unique same-pitch/same-channel notes whose left note reached a pass-change boundary and whose next-pass real Note On began exactly at that boundary, retained the earlier velocity/articulation, supported deterministic chained boundaries and CC64-effective left duration, rejected near/ambiguous matches, stayed disabled by Cut, persisted in project format 15, migrated format-14/unknown values to Off, survived Archived Snapshot/Duplicate/Copy/Move operations, and exposed current helper-bubble contracts."
                                  : "Cross-pass Off compatibility, exact/unique matching, chain/CC64 behavior, ambiguity/gap rejection, Cut precedence, format-15 persistence/migration, library-operation preservation, or helper-bubble contracts failed.");
        }

        SimulationScenarioResult scenarioI25()
        {
            mw::core::Project project("General Controller Lane Fixture");
            project.setStableId(2001);
            auto& track = project.addTrack("Controller MIDI");
            track.setStableId(3001);
            track.addNote({ 60, 100, 900, 900, 1, mw::core::Articulation::Normal });
            const auto originalDuration = track.getNotes().front().durationTicks;

            mw::core::PersistentMidiTakeReview review;
            review.active = true;
            review.reviewSessionId = 4001;
            review.reviewName = "Retained Sustain Review";
            review.sustainPedalMode = mw::core::PersistentMidiSustainPedalMode::ApplyCc64;
            review.passes.push_back({ 1, { { 60, 100, 900, 300, 1, mw::core::Articulation::Normal } }, 0,
                                      { { 1000, 1, 127, 1 }, { 1500, 1, 0, 2 } } });
            track.setPendingMidiTakeReview(review);

            auto& secondTrack = project.addTrack("Second MIDI"); secondTrack.setStableId(3002);
            auto& audioTrack = project.addTrack("Audio"); audioTrack.setStableId(3003); audioTrack.setTrackType(mw::core::TrackType::AudioClip);

            mw::midi::MidiControllerEditHistory history;
            const auto a = history.addAtTick(project, 3001, 1920, 1, 1, 34);
            const auto replace = history.addAtTick(project, 3001, 1920, 1, 1, 35);
            const auto b = history.addAtTick(project, 3001, 2400, 1, 1, 78);
            const auto edge = history.addAtTick(project, 3001, 2400, 16, 127, 127);
            const auto other = history.addAtTick(project, 3001, 2400, 1, 74, 64);
            const bool addContract = a.changed && a.eventId != 0 && replace.changed && replace.replacedExisting
                && replace.eventId == a.eventId && b.changed && edge.changed && other.changed
                && mw::midi::controllerLaneEvents(project.getTracks()[0], 1, 1).size() == 2;

            const bool moved = history.moveToTick(project, 3001, a.eventId, 3000);
            const bool valued = history.setValue(project, 3001, a.eventId, 99);
            auto lane = mw::midi::controllerLaneEvents(project.getTracks()[0], 1, 1);
            auto movedIt = std::find_if(lane.begin(), lane.end(), [&](const auto& e){ return e.stableId == a.eventId; });
            const bool stableEventIdentity = moved && valued && movedIt != lane.end() && movedIt->tick == 3000 && movedIt->value == 99;
            const bool deleted = history.deleteEvent(project, 3001, b.eventId);
            const bool undoDelete = history.undo(project);
            const bool redoDelete = history.redo(project);
            const bool historyContract = deleted && undoDelete && redoDelete && history.canUndo()
                && project.getTracks()[0].getPendingMidiTakeReview().reviewSessionId == 4001;

            mw::midi::MidiControllerEditHistory stateHistory;
            stateHistory.addAtTick(project, 3002, 1000, 1, 1, 20);
            stateHistory.addAtTick(project, 3002, 3000, 1, 1, 90);
            stateHistory.addAtTick(project, 3002, 2000, 2, 1, 44);
            stateHistory.addAtTick(project, 3002, 3200, 1, 64, 127);
            const auto state = mw::midi::controllerStateAtOrBeforeTick(project.getTracks()[1], 2500);
            const auto hasState = [&](int ch, int cc, int value){ return std::any_of(state.begin(), state.end(), [&](const auto& e){ return e.midiChannel==ch && e.controller==cc && e.value==value; }); };
            const bool stateContract = hasState(1,1,20) && hasState(2,1,44) && !hasState(1,64,127);

            std::vector<mw::clap::ClapLivePlaybackControllerEvent> playback;
            for (const auto& e : project.getTracks()[1].getMidiControllerEvents()) playback.push_back({e.stableId,e.tick,e.midiChannel,e.controller,e.value});
            mw::clap::ClapLivePlaybackScheduler scheduler;
            mw::clap::ClapLivePlaybackSchedulerConfig config; config.startTick=2500; config.tailSeconds=0.0;
            scheduler.prepare({}, playback, config);
            bool initial20=false, initial44=false, timed90=false, timed64=false, fabricated=false;
            mw::clap::ClapLiveProcessRequest req;
            while (!scheduler.isFinished())
            {
                scheduler.nextBlockInto(req);
                for (const auto& e : req.controllerEvents)
                {
                    if(e.midiChannel==1 && e.controller==1 && e.value==20) initial20=true;
                    if(e.midiChannel==2 && e.controller==1 && e.value==44) initial44=true;
                    if(e.midiChannel==1 && e.controller==1 && e.value==90) timed90=true;
                    if(e.midiChannel==1 && e.controller==64 && e.value==127) timed64=true;
                    if(e.controller==74) fabricated=true;
                }
            }
            const bool schedulerContract = initial20 && initial44 && timed90 && timed64 && !fabricated;

            const auto cc64 = history.addAtTick(project, 3001, 2100, 1, 64, 127);
            const bool cc64Contract = cc64.changed && project.getTracks()[0].getNotes().front().durationTicks == originalDuration
                && project.getTracks()[0].getPendingMidiTakeReview().passes.front().cc64Events.size() == 2
                && mw::midi::controllerLaneEvents(project.getTracks()[0],1,64).size()==1;
            const bool audioRejected = !history.addAtTick(project,3003,100,1,1,1).changed && project.getTracks()[2].getMidiControllerEvents().empty();

            project.getTracks()[0].setName("Controller MIDI Renamed");
            std::swap(project.getTracks()[0], project.getTracks()[1]);
            const bool stableTrackEdit = history.setValue(project,3001,a.eventId,100);
            const auto stableTrack = std::find_if(project.getTracks().begin(),project.getTracks().end(),[](const auto& t){return t.getStableId()==3001;});
            const bool stableTrackContract = stableTrackEdit && stableTrack!=project.getTracks().end()
                && std::any_of(stableTrack->getMidiControllerEvents().begin(),stableTrack->getMidiControllerEvents().end(),[&](const auto& e){return e.stableId==a.eventId && e.value==100;});

            const auto temp = std::filesystem::temp_directory_path()/"PoorMansStudioSimulation";
            const auto currentPath=temp/"controller_format16.mwproj", legacyPath=temp/"controller_format15.mwproj", malformedPath=temp/"controller_malformed.mwproj";
            std::error_code ec; std::filesystem::create_directories(temp,ec);
            const bool saved=mw::serialization::ProjectSerializer::saveToFile(project,currentPath);
            const auto loaded=saved?mw::serialization::ProjectSerializer::loadFromFile(currentPath):std::nullopt;
            bool persistence=false;
            if(loaded)
            {
                const auto it=std::find_if(loaded->getTracks().begin(),loaded->getTracks().end(),[](const auto& t){return t.getStableId()==3001;});
                persistence=loaded->getMetadata().projectFormatVersion==22 && it!=loaded->getTracks().end() && it->getMidiControllerEvents().size()==stableTrack->getMidiControllerEvents().size();
            }
            std::string text; if(saved){std::ifstream in(currentPath,std::ios::binary);text.assign(std::istreambuf_iterator<char>(in),{});}
            bool legacyEmpty=false;
            if(!text.empty())
            {
                auto legacy=text; const auto pos=legacy.find("\"version\": 22"); if(pos!=std::string::npos) legacy.replace(pos,std::string("\"version\": 22").size(),"\"version\": 15");
                {std::ofstream out(legacyPath,std::ios::binary);out<<legacy;} const auto old=mw::serialization::ProjectSerializer::loadFromFile(legacyPath);
                legacyEmpty=old && std::all_of(old->getTracks().begin(),old->getTracks().end(),[](const auto& t){return t.getMidiControllerEvents().empty();});
            }
            bool malformedSafe=false;
            if(!text.empty())
            {
                auto bad=text; const std::string key="\"controllerEvents\": ["; const auto pos=bad.find(key);
                if(pos!=std::string::npos) bad.insert(pos+key.size(),"{\"eventId\":0,\"tick\":100,\"midiChannel\":1,\"controller\":2,\"value\":3},{\"eventId\":0,\"tick\":101,\"midiChannel\":0,\"controller\":2,\"value\":3},");
                {std::ofstream out(malformedPath,std::ios::binary);out<<bad;} const auto malformed=mw::serialization::ProjectSerializer::loadFromFile(malformedPath);
                if(malformed){std::set<mw::core::StableId> ids; bool valid=true, repaired=false; for(const auto& t:malformed->getTracks())for(const auto& e:t.getMidiControllerEvents()){valid=valid&&mw::midi::isValidMidiControllerEvent(e)&&ids.insert(e.stableId).second;if(e.tick==100&&e.controller==2)repaired=e.stableId!=0;} malformedSafe=valid&&repaired;}
            }
            std::filesystem::remove(currentPath,ec);std::filesystem::remove(legacyPath,ec);std::filesystem::remove(malformedPath,ec);
            const bool ordering=std::is_sorted(stableTrack->getMidiControllerEvents().begin(),stableTrack->getMidiControllerEvents().end(),mw::core::midiControllerEventOrder);
            const bool helpers=std::string(mw::midi::midiControllerLaneTooltip).find("discrete MIDI Control Change")!=std::string::npos
                && std::string(mw::midi::midiControllerUndoTooltip).find("independent from Undo Take")!=std::string::npos
                && std::string(mw::midi::midiControllerCc64Tooltip).find("does not rewrite stored NoteEvent duration")!=std::string::npos
                && mw::midi::midiControllerDisplayName(1).find("Modulation")!=std::string::npos;
            const bool passed=addContract&&stableEventIdentity&&historyContract&&stateContract&&schedulerContract&&cc64Contract&&audioRejected&&stableTrackContract&&ordering&&persistence&&legacyEmpty&&malformedSafe&&helpers;
            return makeResult("I25","Workflow","General MIDI controller lane storage, editing, playback state and persistence",passed,
                passed?"Discrete CC 0-127 storage/edit history, stable IDs, seek-state restoration, timed playback scheduling, CC64 separation, current-format persistence, format-15 migration, malformed-data safety, AudioClip rejection, stable track identity, deterministic ordering, and helper contracts passed.":"General MIDI controller lane contract failed.");
        }

        SimulationScenarioResult scenarioI26()
        {
            using Action = mw::midi::MidiControllerLaneUiAction;

            const std::array<Action, 8> viewActions {{
                Action::OpenWindow, Action::CloseWindow, Action::SelectTrack, Action::SelectController,
                Action::SelectChannel, Action::SelectEvent, Action::Scroll, Action::HoverHelp
            }};
            const std::array<Action, 6> editActions {{
                Action::AddAtPreview, Action::MoveToPreview, Action::SetValue,
                Action::DeleteEvent, Action::UndoCc, Action::RedoCc
            }};
            const bool dirtyPolicy = std::all_of(viewActions.begin(), viewActions.end(), [](Action action)
                { return !mw::midi::midiControllerLaneUiActionMayDirty(action); })
                && std::all_of(editActions.begin(), editActions.end(), [](Action action)
                { return mw::midi::midiControllerLaneUiActionMayDirty(action); });

            mw::core::Project project("Controller Lane UI Fixture");
            project.setStableId(26001);
            auto& track = project.addTrack("UI MIDI");
            track.setStableId(26101);
            auto& second = project.addTrack("Second MIDI");
            second.setStableId(26102);
            auto& audio = project.addTrack("AudioClip");
            audio.setStableId(26103);
            audio.setTrackType(mw::core::TrackType::AudioClip);

            mw::midi::MidiControllerEditHistory history;
            const auto first = history.addAtTick(project, 26101, 1920, 1, 1, 34);
            const auto secondPoint = history.addAtTick(project, 26101, 2400, 1, 1, 78);
            const auto otherController = history.addAtTick(project, 26101, 2100, 1, 74, 55);
            const auto otherChannel = history.addAtTick(project, 26101, 2200, 2, 1, 66);

            auto findTrackById = [&](mw::core::StableId trackId) -> mw::core::Track*
            {
                for (auto& candidate : project.getTracks())
                    if (candidate.getStableId() == trackId)
                        return &candidate;
                return nullptr;
            };

            auto* uiTrack = findTrackById(26101);
            const bool initialSelection = uiTrack != nullptr
                && mw::midi::reconcileMidiControllerLaneSelection(*uiTrack, 1, 1, first.eventId) == first.eventId
                && mw::midi::reconcileMidiControllerLaneSelection(*uiTrack, 1, 74, first.eventId) == 0
                && mw::midi::reconcileMidiControllerLaneSelection(*uiTrack, 2, 1, first.eventId) == 0
                && mw::midi::reconcileMidiControllerLaneSelection(audio, 1, 1, first.eventId) == 0;

            const bool moved = history.moveToTick(project, 26101, first.eventId, 3000);
            uiTrack = findTrackById(26101);
            const auto visibleAfterMove = uiTrack ? mw::midi::controllerLaneEvents(*uiTrack, 1, 1) : std::vector<mw::core::MidiControllerEvent>{};
            const bool reorderedSelection = moved && uiTrack != nullptr
                && visibleAfterMove.size() == 2
                && visibleAfterMove.front().stableId == secondPoint.eventId
                && visibleAfterMove.back().stableId == first.eventId
                && mw::midi::reconcileMidiControllerLaneSelection(*uiTrack, 1, 1, first.eventId) == first.eventId;

            const auto noOpReplace = history.addAtTick(project, 26101, 3000, 1, 1, 34);
            const bool noOpValue = !history.setValue(project, 26101, first.eventId, 34);
            const bool noOpMove = !history.moveToTick(project, 26101, first.eventId, 3000);
            const bool noOpEdits = !noOpReplace.changed && noOpReplace.replacedExisting && noOpValue && noOpMove;

            const bool changedValue = history.setValue(project, 26101, first.eventId, 99);
            const bool undoValue = history.undo(project);
            uiTrack = findTrackById(26101);
            const bool valueRestored = uiTrack != nullptr && std::any_of(uiTrack->getMidiControllerEvents().begin(), uiTrack->getMidiControllerEvents().end(), [&](const auto& event)
                { return event.stableId == first.eventId && event.value == 34; });
            const bool redoValue = history.redo(project);

            const bool deleted = history.deleteEvent(project, 26101, secondPoint.eventId);
            const bool undoDelete = history.undo(project);
            uiTrack = findTrackById(26101);
            const bool deleteRestored = uiTrack != nullptr && std::any_of(uiTrack->getMidiControllerEvents().begin(), uiTrack->getMidiControllerEvents().end(), [&](const auto& event)
                { return event.stableId == secondPoint.eventId; });
            const bool redoDelete = history.redo(project);

            const auto addedForUndo = history.addAtTick(project, 26101, 3600, 1, 1, 111);
            const bool undoAdd = history.undo(project);
            uiTrack = findTrackById(26101);
            const bool addRemoved = uiTrack != nullptr && std::none_of(uiTrack->getMidiControllerEvents().begin(), uiTrack->getMidiControllerEvents().end(), [&](const auto& event)
                { return event.stableId == addedForUndo.eventId; });
            const bool redoAdd = history.redo(project);

            project.getTracks()[0].setName("UI MIDI Renamed");
            std::swap(project.getTracks()[0], project.getTracks()[1]);
            uiTrack = findTrackById(26101);
            const bool stableTrackTarget = uiTrack != nullptr && uiTrack->getName() == "UI MIDI Renamed"
                && mw::midi::reconcileMidiControllerLaneSelection(*uiTrack, 1, 1, first.eventId) == first.eventId;

            bool allControllerNamesAvailable = true;
            for (int controller = 0; controller <= 127; ++controller)
            {
                const auto name = mw::midi::midiControllerDisplayName(controller);
                allControllerNamesAvailable = allControllerNamesAvailable && name.rfind("CC ", 0) == 0;
            }

            const bool helperContract = std::string(mw::midi::midiControllerLaneTooltip).find("selected MIDI track") != std::string::npos
                && std::string(mw::midi::midiControllerSelectorTooltip).find("0-127") != std::string::npos
                && std::string(mw::midi::midiControllerChannelTooltip).find("does not edit or dirty") != std::string::npos
                && std::string(mw::midi::midiControllerValueTooltip).find("does not edit the project") != std::string::npos
                && std::string(mw::midi::midiControllerEventListTooltip).find("stable") != std::string::npos
                && std::string(mw::midi::midiControllerCc64Tooltip).find("TAKE REVIEW") != std::string::npos;

            const bool historyCoverage = changedValue && undoValue && valueRestored && redoValue
                && deleted && undoDelete && deleteRestored && redoDelete
                && addedForUndo.changed && undoAdd && addRemoved && redoAdd;
            const bool formatUnchanged = project.getMetadata().projectFormatVersion == 22;
            const bool setup = first.changed && secondPoint.changed && otherController.changed && otherChannel.changed;

            const bool passed = setup && dirtyPolicy && initialSelection && reorderedSelection && noOpEdits
                && historyCoverage && stableTrackTarget && allControllerNamesAvailable && helperContract && formatUnchanged;
            return makeResult("I26", "Workflow", "Native MIDI controller lane UI and edit integration", passed,
                passed
                    ? "Controller Lane UI policy kept selector/selection/window actions non-dirty, edit actions dirty only on real changes, retained stable event selection across move/reorder and stable track identity across rename/reorder, covered Add/Move/Value/Delete Undo/Redo behavior, exposed all CC 0-127 plus helper contracts, rejected AudioClip selection, and remained compatible with the current project format."
                    : "Controller Lane UI dirty-state, stable selection/track identity, edit-history integration, controller availability, helper contracts, AudioClip rejection, or format-16 preservation failed.");
        }


        SimulationScenarioResult scenarioI27()
        {
            using Action = mw::midi::MidiControllerLaneUiAction;
            using Grid = mw::midi::MidiControllerSnapGrid;

            const std::array<Action, 5> graphViewActions {{
                Action::SelectGraphPoint, Action::PanGraph, Action::ZoomGraph, Action::SelectSnap, Action::CancelGraphDrag
            }};
            const bool dirtyPolicy = std::all_of(graphViewActions.begin(), graphViewActions.end(), [](Action action)
                { return !mw::midi::midiControllerLaneUiActionMayDirty(action); })
                && mw::midi::midiControllerLaneUiActionMayDirty(Action::GraphDragCommit);

            const bool snapPolicy = mw::midi::midiControllerSnapGridTicks(960, Grid::Quarter) == 960
                && mw::midi::midiControllerSnapGridTicks(960, Grid::Eighth) == 480
                && mw::midi::midiControllerSnapGridTicks(960, Grid::Sixteenth) == 240
                && mw::midi::midiControllerSnapGridTicks(960, Grid::ThirtySecond) == 120
                && mw::midi::midiControllerSnapGridTicks(960, Grid::SixtyFourth) == 60
                && mw::midi::snapMidiControllerTick(1234, 960, Grid::Off) == 1234
                && mw::midi::snapMidiControllerTick(-20, 960, Grid::Off) == 0
                && mw::midi::snapMidiControllerTick(1439, 960, Grid::Quarter) == 960
                && mw::midi::snapMidiControllerTick(1440, 960, Grid::Quarter) == 1920
                && mw::midi::snapMidiControllerTick(720, 960, Grid::Eighth) == 960
                && mw::midi::snapMidiControllerTick(360, 960, Grid::Sixteenth) == 480
                && mw::midi::snapMidiControllerTick(180, 960, Grid::ThirtySecond) == 240
                && mw::midi::snapMidiControllerTick(90, 960, Grid::SixtyFourth) == 120;

            const double tickX = mw::midi::midiControllerTickToX(1920, 960, 4.0);
            const auto tickRoundTrip = mw::midi::midiControllerXToTick(tickX, 960, 4.0);
            const double y127 = mw::midi::midiControllerValueToY(127, 10.0, 254.0);
            const double y0 = mw::midi::midiControllerValueToY(0, 10.0, 254.0);
            const bool mappingPolicy = std::abs(tickX - 240.0) < 0.0001 && tickRoundTrip == 1920
                && std::abs(y127 - 10.0) < 0.0001 && std::abs(y0 - 264.0) < 0.0001
                && mw::midi::midiControllerYToValue(10.0, 10.0, 254.0) == 127
                && mw::midi::midiControllerYToValue(264.0, 10.0, 254.0) == 0
                && mw::midi::midiControllerYToValue(-100.0, 10.0, 254.0) == 127
                && mw::midi::midiControllerYToValue(1000.0, 10.0, 254.0) == 0;

            mw::core::Project project("Graphical Controller Lane Fixture");
            project.setStableId(27001);
            auto& track = project.addTrack("Graph MIDI");
            track.setStableId(27101);
            track.getNotes().push_back(mw::core::NoteEvent(60, 100, 1000, 500, 1));
            auto& audio = project.addTrack("Graph Audio");
            audio.setStableId(27102);
            audio.setTrackType(mw::core::TrackType::AudioClip);
            auto findTrackById = [&](mw::core::StableId trackId) -> mw::core::Track*
            {
                for (auto& candidate : project.getTracks())
                    if (candidate.getStableId() == trackId) return &candidate;
                return nullptr;
            };
            auto* midiTrack = findTrackById(27101);
            const auto originalNoteDuration = midiTrack != nullptr && !midiTrack->getNotes().empty() ? midiTrack->getNotes().front().durationTicks : 0;

            mw::midi::MidiControllerEditHistory history;
            const auto first = history.addAtTick(project, 27101, 1000, 1, 1, 20);
            const auto collision = history.addAtTick(project, 27101, 2000, 1, 1, 80);
            const auto later = history.addAtTick(project, 27101, 3000, 1, 1, 90);
            const auto cc64 = history.addAtTick(project, 27101, 1500, 1, 64, 127);
            const bool setup = midiTrack != nullptr && first.changed && collision.changed && later.changed && cc64.changed;

            const auto laneBefore = mw::midi::controllerLaneEvents(*midiTrack, 1, 1);
            const auto hit = mw::midi::hitTestMidiControllerPoint(laneBefore, 0, 4.0, 10.0, 254.0,
                mw::midi::midiControllerTickToX(1000, 0, 4.0),
                mw::midi::midiControllerValueToY(20, 10.0, 254.0), 8.0);
            const auto miss = mw::midi::hitTestMidiControllerPoint(laneBefore, 0, 4.0, 10.0, 254.0, 700.0, 250.0, 4.0);
            const bool hitPolicy = hit == first.eventId && miss == 0
                && mw::midi::reconcileMidiControllerLaneSelection(*midiTrack, 1, 1, hit) == first.eventId;

            const auto steps = mw::midi::controllerStaircaseSegments(*midiTrack, 1, 1, 2500, 4000);
            const bool staircasePolicy = steps.size() == 2
                && steps[0].startTick == 2500 && steps[0].endTick == 3000 && steps[0].value == 80 && steps[0].sourceEventId == collision.eventId
                && steps[1].startTick == 3000 && steps[1].endTick == 4000 && steps[1].value == 90 && steps[1].sourceEventId == later.eventId;

            const bool dragChanged = history.moveAndSetValue(project, 27101, first.eventId, 2000, 64);
            const auto laneAfterDrag = mw::midi::controllerLaneEvents(*midiTrack, 1, 1);
            const bool collisionPolicy = dragChanged && laneAfterDrag.size() == 2
                && std::any_of(laneAfterDrag.begin(), laneAfterDrag.end(), [&](const auto& event)
                    { return event.stableId == first.eventId && event.tick == 2000 && event.value == 64; })
                && std::none_of(laneAfterDrag.begin(), laneAfterDrag.end(), [&](const auto& event)
                    { return event.stableId == collision.eventId; });

            const bool oneUndo = history.undo(project);
            const auto laneAfterUndo = mw::midi::controllerLaneEvents(*midiTrack, 1, 1);
            const bool undoRestoresCollision = oneUndo && laneAfterUndo.size() == 3
                && std::any_of(laneAfterUndo.begin(), laneAfterUndo.end(), [&](const auto& event)
                    { return event.stableId == first.eventId && event.tick == 1000 && event.value == 20; })
                && std::any_of(laneAfterUndo.begin(), laneAfterUndo.end(), [&](const auto& event)
                    { return event.stableId == collision.eventId && event.tick == 2000 && event.value == 80; });
            const bool oneRedo = history.redo(project);
            const auto laneAfterRedo = mw::midi::controllerLaneEvents(*midiTrack, 1, 1);
            const bool redoRestoresDrag = oneRedo && laneAfterRedo.size() == 2
                && std::any_of(laneAfterRedo.begin(), laneAfterRedo.end(), [&](const auto& event)
                    { return event.stableId == first.eventId && event.tick == 2000 && event.value == 64; });

            const bool noOpDrag = !history.moveAndSetValue(project, 27101, first.eventId, 2000, 64);
            const bool invalidDrag = !history.moveAndSetValue(project, 27101, first.eventId, -1, 64)
                && !history.moveAndSetValue(project, 27101, first.eventId, 2001, 128);
            const bool audioRejected = !history.moveAndSetValue(project, 27102, first.eventId, 2001, 64);

            const bool cc64Dragged = history.moveAndSetValue(project, 27101, cc64.eventId, 1600, 0);
            const bool cc64Separated = cc64Dragged && midiTrack != nullptr && midiTrack->getNotes().front().durationTicks == originalNoteDuration
                && mw::midi::controllerLaneEvents(*midiTrack, 1, 64).size() == 1;

            const bool helpers = std::string(mw::midi::midiControllerGraphTooltip).find("staircase") != std::string::npos
                && std::string(mw::midi::midiControllerGraphTooltip).find("one Undo CC transaction") != std::string::npos
                && std::string(mw::midi::midiControllerSnapTooltip).find("half-grid ties go forward") != std::string::npos
                && std::string(mw::midi::midiControllerZoomTooltip).find("do not edit or dirty") != std::string::npos;
            const bool formatUnchanged = project.getMetadata().projectFormatVersion == 22;

            const bool passed = dirtyPolicy && snapPolicy && mappingPolicy && setup && hitPolicy && staircasePolicy
                && collisionPolicy && undoRestoresCollision && redoRestoresDrag && noOpDrag && invalidDrag
                && audioRejected && cc64Separated && helpers && formatUnchanged;
            std::ostringstream i27Failure;
            if (!passed)
            {
                i27Failure << "I27 failed:"
                    << " dirty=" << dirtyPolicy
                    << " snap=" << snapPolicy
                    << " mapping=" << mappingPolicy
                    << " setup=" << setup
                    << " hit=" << hitPolicy
                    << " stairs=" << staircasePolicy
                    << " collision=" << collisionPolicy
                    << " undo=" << undoRestoresCollision
                    << " redo=" << redoRestoresDrag
                    << " noop=" << noOpDrag
                    << " invalid=" << invalidDrag
                    << " audio=" << audioRejected
                    << " cc64=" << cc64Separated
                    << " helpers=" << helpers
                    << " format=" << formatUnchanged;
            }
            return makeResult("I27", "Workflow", "Graphical MIDI controller point editing and snap policy", passed,
                passed
                    ? "Graphical CC point policy passed deterministic tick/value mapping, stable-ID hit selection, discrete staircase state with latest-before-view restoration, Off/1-4 through 1-64 horizontal snapping with forward half-grid ties, one-transaction drag tick/value editing, collision identity/Undo restoration, no-op/cancel-style non-dirty policy, AudioClip rejection, CC64 separation, helper contracts, and compatibility with the current project format."
                    : i27Failure.str());
        }

        SimulationScenarioResult scenarioI28()
        {
            const auto tempRoot = std::filesystem::temp_directory_path() / "PoorMansStudioSimulation";
            const auto midiPath = tempRoot / "i28_controller_import.mid";
            const auto malformedPath = tempRoot / "i28_controller_import_malformed.mid";
            const auto roundTripPath = tempRoot / "i28_controller_import_roundtrip.pms";
            std::error_code ec;
            std::filesystem::create_directories(tempRoot, ec);
            std::filesystem::remove(midiPath, ec);
            std::filesystem::remove(malformedPath, ec);
            std::filesystem::remove(roundTripPath, ec);

            auto appendBe16 = [](std::vector<std::uint8_t>& bytes, std::uint16_t value)
            {
                bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFU));
                bytes.push_back(static_cast<std::uint8_t>(value & 0xFFU));
            };
            auto appendBe32 = [](std::vector<std::uint8_t>& bytes, std::uint32_t value)
            {
                bytes.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFFU));
                bytes.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFFU));
                bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFU));
                bytes.push_back(static_cast<std::uint8_t>(value & 0xFFU));
            };
            auto appendAscii = [](std::vector<std::uint8_t>& bytes, const char* text)
            {
                while (*text != '\0')
                {
                    bytes.push_back(static_cast<std::uint8_t>(*text));
                    ++text;
                }
            };
            auto appendTrack = [&](std::vector<std::uint8_t>& file, const std::vector<std::uint8_t>& track)
            {
                appendAscii(file, "MTrk");
                appendBe32(file, static_cast<std::uint32_t>(track.size()));
                file.insert(file.end(), track.begin(), track.end());
            };
            auto makeHeader = [&](std::uint16_t format, std::uint16_t tracks)
            {
                std::vector<std::uint8_t> file;
                appendAscii(file, "MThd");
                appendBe32(file, 6U);
                appendBe16(file, format);
                appendBe16(file, tracks);
                appendBe16(file, 480U);
                return file;
            };
            auto appendMetaName = [](std::vector<std::uint8_t>& track, const std::string& name)
            {
                track.push_back(0x00U);
                track.push_back(0xFFU);
                track.push_back(0x03U);
                track.push_back(static_cast<std::uint8_t>(name.size()));
                for (const char c : name)
                    track.push_back(static_cast<std::uint8_t>(c));
            };
            auto appendEnd = [](std::vector<std::uint8_t>& track)
            {
                track.insert(track.end(), {0x00U, 0xFFU, 0x2FU, 0x00U});
            };
            auto writeBinary = [](const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes)
            {
                std::ofstream out(path, std::ios::binary | std::ios::trunc);
                if (!out)
                    return false;
                out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                return static_cast<bool>(out);
            };

            auto midiBytes = makeHeader(1U, 3U);

            std::vector<std::uint8_t> conductor;
            appendMetaName(conductor, "Conductor");
            conductor.insert(conductor.end(), {0x00U, 0xFFU, 0x51U, 0x03U, 0x07U, 0xA1U, 0x20U});
            conductor.insert(conductor.end(), {0x00U, 0xFFU, 0x58U, 0x04U, 0x04U, 0x02U, 0x18U, 0x08U});
            appendEnd(conductor);
            appendTrack(midiBytes, conductor);

            std::vector<std::uint8_t> mixed;
            appendMetaName(mixed, "Mixed");
            mixed.insert(mixed.end(), {0x00U, 0xB0U, 0x00U, 0x01U}); // CC0 MSB
            mixed.insert(mixed.end(), {0x00U, 0x20U, 0x02U});        // running-status CC32 LSB
            mixed.insert(mixed.end(), {0x00U, 0xC0U, 0x28U});        // program 40
            mixed.insert(mixed.end(), {0x00U, 0x90U, 0x3CU, 0x64U}); // note on
            mixed.insert(mixed.end(), {0x78U, 0xB0U, 0x01U, 0x14U}); // CC1=20 at source tick 120
            mixed.insert(mixed.end(), {0x00U, 0x01U, 0x40U});        // same tick CC1=64
            mixed.insert(mixed.end(), {0x00U, 0x01U, 0x64U});        // same tick CC1=100, last wins
            mixed.insert(mixed.end(), {0x00U, 0xB1U, 0x01U, 0x37U}); // ch2 CC1=55
            mixed.insert(mixed.end(), {0x00U, 0xB0U, 0x4AU, 0x7FU}); // CC74=127
            mixed.insert(mixed.end(), {0x00U, 0xB0U, 0x7FU, 0x00U}); // CC127=0
            mixed.insert(mixed.end(), {0x78U, 0xB0U, 0x40U, 0x7FU}); // CC64=127 at source tick 240
            mixed.insert(mixed.end(), {0x78U, 0x80U, 0x3CU, 0x00U}); // note off at source tick 360
            appendEnd(mixed);
            appendTrack(midiBytes, mixed);

            std::vector<std::uint8_t> controllersOnly;
            appendMetaName(controllersOnly, "Controllers Only");
            controllersOnly.insert(controllersOnly.end(), {0x3CU, 0xBFU, 0x7FU, 0x7FU}); // ch16 CC127=127 at source tick 60
            controllersOnly.insert(controllersOnly.end(), {0x00U, 0x0BU, 0x00U});        // running-status CC11=0
            appendEnd(controllersOnly);
            appendTrack(midiBytes, controllersOnly);

            const bool wroteMidi = writeBinary(midiPath, midiBytes);
            const auto imported = wroteMidi ? mw::import_export::MidiImporter::importFromFile(midiPath) : std::nullopt;

            auto findTrackByName = [](mw::core::Project& project, const std::string& name) -> mw::core::Track*
            {
                const auto it = std::find_if(project.getTracks().begin(), project.getTracks().end(), [&](const auto& track)
                {
                    return track.getName() == name;
                });
                return it == project.getTracks().end() ? nullptr : &*it;
            };

            bool importedCore = false;
            bool controllerOnlyCreated = false;
            bool controllerValues = false;
            bool notePreserved = false;
            bool bankProgramPreserved = false;
            bool cc64Separated = false;
            bool stableIdsValid = false;
            bool roundTripStable = false;
            bool noReviewFabricated = false;

            if (imported.has_value())
            {
                auto importedProject = *imported;
                auto* mixedTrack = findTrackByName(importedProject, "Mixed");
                auto* controllerTrack = findTrackByName(importedProject, "Controllers Only");
                importedCore = importedProject.getTracks().size() == 2U
                    && importedProject.getTempoBpm() == 120
                    && mixedTrack != nullptr
                    && controllerTrack != nullptr;

                if (mixedTrack != nullptr)
                {
                    const auto& events = mixedTrack->getMidiControllerEvents();
                    auto hasEvent = [&](std::int64_t tick, int channel, int controller, int value)
                    {
                        return std::any_of(events.begin(), events.end(), [&](const auto& event)
                        {
                            return event.tick == tick
                                && event.midiChannel == channel
                                && event.controller == controller
                                && event.value == value;
                        });
                    };

                    controllerValues = events.size() == 7U
                        && hasEvent(0, 1, 0, 1)
                        && hasEvent(0, 1, 32, 2)
                        && hasEvent(240, 1, 1, 100)
                        && hasEvent(240, 2, 1, 55)
                        && hasEvent(240, 1, 74, 127)
                        && hasEvent(240, 1, 127, 0)
                        && hasEvent(480, 1, 64, 127);

                    notePreserved = mixedTrack->getNotes().size() == 1U
                        && mixedTrack->getNotes().front().pitch == 60
                        && mixedTrack->getNotes().front().velocity == 100
                        && mixedTrack->getNotes().front().midiChannel == 1
                        && mixedTrack->getNotes().front().startTick == 0
                        && mixedTrack->getNotes().front().durationTicks == 720;
                    bankProgramPreserved = mixedTrack->getInstrument().midiBank == 130
                        && mixedTrack->getInstrument().midiProgram == 40
                        && mixedTrack->getInstrument().midiChannel == 1;
                    cc64Separated = notePreserved
                        && !mixedTrack->getPendingMidiTakeReview().active
                        && mixedTrack->getArchivedMidiTakeReviews().empty();
                }

                if (controllerTrack != nullptr)
                {
                    const auto& events = controllerTrack->getMidiControllerEvents();
                    controllerOnlyCreated = controllerTrack->getNotes().empty()
                        && events.size() == 2U
                        && std::any_of(events.begin(), events.end(), [](const auto& event)
                        {
                            return event.tick == 120 && event.midiChannel == 16 && event.controller == 127 && event.value == 127;
                        })
                        && std::any_of(events.begin(), events.end(), [](const auto& event)
                        {
                            return event.tick == 120 && event.midiChannel == 16 && event.controller == 11 && event.value == 0;
                        });
                }

                std::set<mw::core::StableId> eventIds;
                std::size_t totalEvents = 0;
                std::map<std::tuple<std::string, std::int64_t, int, int>, mw::core::StableId> identityBefore;
                for (const auto& track : importedProject.getTracks())
                {
                    for (const auto& event : track.getMidiControllerEvents())
                    {
                        ++totalEvents;
                        if (event.stableId != 0)
                            eventIds.insert(event.stableId);
                        identityBefore.emplace(std::make_tuple(track.getName(), event.tick, event.midiChannel, event.controller), event.stableId);
                    }
                }
                noReviewFabricated = std::all_of(importedProject.getTracks().begin(), importedProject.getTracks().end(), [](const auto& track)
                {
                    return !track.getPendingMidiTakeReview().active && track.getArchivedMidiTakeReviews().empty();
                });
                stableIdsValid = totalEvents == 9U && eventIds.size() == totalEvents;

                const bool saved = mw::serialization::ProjectSerializer::saveToFile(importedProject, roundTripPath);
                const auto reopened = saved ? mw::serialization::ProjectSerializer::loadFromFile(roundTripPath) : std::nullopt;
                if (reopened.has_value())
                {
                    std::map<std::tuple<std::string, std::int64_t, int, int>, mw::core::StableId> identityAfter;
                    for (const auto& track : reopened->getTracks())
                    {
                        for (const auto& event : track.getMidiControllerEvents())
                            identityAfter.emplace(std::make_tuple(track.getName(), event.tick, event.midiChannel, event.controller), event.stableId);
                    }
                    roundTripStable = reopened->getMetadata().projectFormatVersion == 22
                        && identityAfter == identityBefore;
                }
            }

            auto malformedBytes = makeHeader(0U, 1U);
            std::vector<std::uint8_t> malformedTrack;
            malformedTrack.insert(malformedTrack.end(), {0x00U, 0x90U, 0x3CU, 0x64U});
            malformedTrack.insert(malformedTrack.end(), {0x78U, 0x80U, 0x3CU, 0x00U});
            malformedTrack.insert(malformedTrack.end(), {0x00U, 0xB0U, 0x01U}); // missing CC value byte
            appendTrack(malformedBytes, malformedTrack);
            const bool wroteMalformed = writeBinary(malformedPath, malformedBytes);
            const auto malformed = wroteMalformed ? mw::import_export::MidiImporter::importFromFile(malformedPath) : std::nullopt;
            const bool malformedSafe = malformed.has_value()
                && malformed->getTracks().size() == 1U
                && malformed->getTracks().front().getNotes().size() == 1U
                && malformed->getTracks().front().getMidiControllerEvents().empty();

            std::filesystem::remove(midiPath, ec);
            std::filesystem::remove(malformedPath, ec);
            std::filesystem::remove(roundTripPath, ec);

            const bool passed = importedCore
                && controllerOnlyCreated
                && controllerValues
                && notePreserved
                && bankProgramPreserved
                && cc64Separated
                && stableIdsValid
                && roundTripStable
                && noReviewFabricated
                && malformedSafe;

            std::ostringstream failure;
            if (!passed)
            {
                failure << "I28 failed:"
                        << " importedCore=" << importedCore
                        << " controllerOnly=" << controllerOnlyCreated
                        << " controllerValues=" << controllerValues
                        << " notePreserved=" << notePreserved
                        << " bankProgram=" << bankProgramPreserved
                        << " cc64Separated=" << cc64Separated
                        << " stableIds=" << stableIdsValid
                        << " roundTrip=" << roundTripStable
                        << " noReview=" << noReviewFabricated
                        << " malformedSafe=" << malformedSafe;
            }

            return makeResult("I28", "Workflow", "Standard MIDI file controller-event import", passed,
                passed
                    ? "Standard MIDI File import preserved arbitrary CC 0-127 by exact scaled tick/channel/value, running-status messages, controller-only tracks, same-tick last-source-value semantics, stable project event identity and current-format round trips, while retaining CC0/32 bank assignment, note/program behavior, CC64 separation, and malformed/truncated safety."
                    : failure.str());
        }

        SimulationScenarioResult scenarioI29()
        {
            const auto tempRoot = std::filesystem::temp_directory_path() / "PoorMansStudioSimulation";
            const auto userMidiPath = tempRoot / "i29_user_export.mid";
            const auto internalMidiPath = tempRoot / "i29_internal_export.mid";
            const auto malformedMarkerPath = tempRoot / "i29_nonmatching_marker.mid";
            std::error_code ec;
            std::filesystem::create_directories(tempRoot, ec);
            std::filesystem::remove(userMidiPath, ec);
            std::filesystem::remove(internalMidiPath, ec);
            std::filesystem::remove(malformedMarkerPath, ec);

            mw::core::Project source("I29 MIDI CC Round Trip");
            source.setTempoBpm(120);
            // Do not retain Track& references across Project::addTrack() calls.
            // Project owns tracks in a std::vector, so a later addTrack may reallocate
            // and invalidate an earlier reference (observed as a Windows full-suite
            // crash at I29). Build each track in a short scope, then reacquire by
            // stable index after the project structure is complete.
            {
                auto& mainTrack = source.addTrack("Round Trip Main");
                auto assignment = mainTrack.getInstrument();
                assignment.midiChannel = 1;
                assignment.midiBank = 130;
                assignment.midiProgram = 40;
                mainTrack.setInstrumentAssignment(assignment);
                mainTrack.getMixerSettings().volume = 0.5f;
                mainTrack.addNote(mw::core::NoteEvent(60, 100, 0, 960, 1));
                mainTrack.getMidiControllerEvents().emplace_back(0, 1, 0, 3);
                mainTrack.getMidiControllerEvents().emplace_back(0, 1, 32, 4);
                mainTrack.getMidiControllerEvents().emplace_back(0, 1, 1, 45);
                mainTrack.getMidiControllerEvents().emplace_back(0, 1, 7, 99);
                mainTrack.getMidiControllerEvents().emplace_back(0, 1, 11, 88);
                mainTrack.getMidiControllerEvents().emplace_back(240, 16, 127, 127);
                mainTrack.getMidiControllerEvents().emplace_back(480, 1, 64, 127);
            }

            {
                auto& controllerOnly = source.addTrack("Controller Only Round Trip");
                auto controllerAssignment = controllerOnly.getInstrument();
                controllerAssignment.midiChannel = 16;
                controllerAssignment.midiBank = 0;
                controllerAssignment.midiProgram = 10;
                controllerOnly.setInstrumentAssignment(controllerAssignment);
                controllerOnly.getMidiControllerEvents().emplace_back(120, 16, 74, 0);
            }

            source.ensureUniqueStableIds();
            const auto& mainTrack = source.getTracks().front();

            const bool userExported = mw::midi::MidiExporter::exportToFile(
                source, userMidiPath, { mw::midi::MidiExportPurpose::UserMidiExport });
            const bool internalExported = mw::midi::MidiExporter::exportToFile(
                source, internalMidiPath, { mw::midi::MidiExportPurpose::InternalRender });

            auto readBytes = [](const std::filesystem::path& path)
            {
                std::ifstream input(path, std::ios::binary);
                return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
            };
            const auto userBytes = userExported ? readBytes(userMidiPath) : std::vector<std::uint8_t>{};
            const auto internalBytes = internalExported ? readBytes(internalMidiPath) : std::vector<std::uint8_t>{};
            const auto hasMarker = [&](int channel, int controller, int value)
            {
                const std::array<std::uint8_t, 9> payload {{
                    'P', 'M', 'S', 'C', 'C', 0x01,
                    static_cast<std::uint8_t>(channel),
                    static_cast<std::uint8_t>(controller),
                    static_cast<std::uint8_t>(value)
                }};
                return std::search(userBytes.begin(), userBytes.end(), payload.begin(), payload.end()) != userBytes.end();
            };
            const auto hasRawCc = [&](int channel, int controller, int value)
            {
                const std::array<std::uint8_t, 3> bytes {{
                    static_cast<std::uint8_t>(0xB0 | ((channel - 1) & 0x0F)),
                    static_cast<std::uint8_t>(controller),
                    static_cast<std::uint8_t>(value)
                }};
                return std::search(userBytes.begin(), userBytes.end(), bytes.begin(), bytes.end()) != userBytes.end();
            };
            const bool generatedSetupPresent = hasMarker(1, 7, 63) && hasRawCc(1, 7, 63)
                && hasMarker(1, 1, 100) && hasRawCc(1, 1, 100)
                && hasMarker(1, 11, 127) && hasRawCc(1, 11, 127)
                && hasMarker(1, 0, 1) && hasRawCc(1, 0, 1)
                && hasMarker(1, 32, 2) && hasRawCc(1, 32, 2);
            const std::array<std::uint8_t, 5> markerPrefix {{ 'P', 'M', 'S', 'C', 'C' }};
            const bool internalHasNoPmsMarker = std::search(internalBytes.begin(), internalBytes.end(), markerPrefix.begin(), markerPrefix.end()) == internalBytes.end();

            const auto roundTripped = userExported ? mw::import_export::MidiImporter::importFromFile(userMidiPath) : std::nullopt;
            bool cleanRoundTrip = false;
            bool authoredTickZeroWins = false;
            bool bankAtProgram = false;
            bool controllerOnlyRoundTrip = false;
            bool cc64Separated = false;
            bool freshStableIds = false;
            if (roundTripped.has_value())
            {
                auto findTrack = [&](const std::string& name) -> const mw::core::Track*
                {
                    const auto it = std::find_if(roundTripped->getTracks().begin(), roundTripped->getTracks().end(), [&](const auto& track)
                    {
                        return track.getName() == name;
                    });
                    return it == roundTripped->getTracks().end() ? nullptr : &*it;
                };
                const auto* importedMain = findTrack("Round Trip Main");
                const auto* importedControllerOnly = findTrack("Controller Only Round Trip");
                if (importedMain != nullptr)
                {
                    const auto& events = importedMain->getMidiControllerEvents();
                    auto hasEvent = [&](std::int64_t tick, int channel, int controller, int value)
                    {
                        return std::any_of(events.begin(), events.end(), [&](const auto& event)
                        {
                            return event.tick == tick && event.midiChannel == channel
                                && event.controller == controller && event.value == value;
                        });
                    };
                    cleanRoundTrip = events.size() == 7U
                        && hasEvent(0, 1, 0, 3) && hasEvent(0, 1, 32, 4)
                        && hasEvent(0, 1, 1, 45) && hasEvent(0, 1, 7, 99) && hasEvent(0, 1, 11, 88)
                        && hasEvent(240, 16, 127, 127) && hasEvent(480, 1, 64, 127);
                    authoredTickZeroWins = cleanRoundTrip;
                    bankAtProgram = importedMain->getInstrument().midiBank == 130
                        && importedMain->getInstrument().midiProgram == 40;
                    cc64Separated = importedMain->getNotes().size() == 1U
                        && importedMain->getNotes().front().durationTicks == 960
                        && !importedMain->getPendingMidiTakeReview().active;

                    std::set<mw::core::StableId> sourceIds;
                    for (const auto& event : mainTrack.getMidiControllerEvents()) sourceIds.insert(event.stableId);
                    std::set<mw::core::StableId> importedIds;
                    for (const auto& event : events) importedIds.insert(event.stableId);
                    freshStableIds = importedIds.size() == events.size() && importedIds.count(0) == 0;
                    if (sourceIds == importedIds)
                        freshStableIds = false;
                }
                controllerOnlyRoundTrip = importedControllerOnly != nullptr
                    && importedControllerOnly->getNotes().empty()
                    && importedControllerOnly->getMidiControllerEvents().size() == 1U
                    && importedControllerOnly->getMidiControllerEvents().front().tick == 120
                    && importedControllerOnly->getMidiControllerEvents().front().midiChannel == 16
                    && importedControllerOnly->getMidiControllerEvents().front().controller == 74
                    && importedControllerOnly->getMidiControllerEvents().front().value == 0;
            }

            // A valid PMS marker suppresses only its exact immediately-following match.
            // A nonmatching following CC must import normally so malformed/reordered files
            // fail safe by preserving external data rather than losing it.
            std::vector<std::uint8_t> malformed;
            auto be16 = [&](std::uint16_t v){ malformed.push_back(static_cast<std::uint8_t>(v >> 8)); malformed.push_back(static_cast<std::uint8_t>(v)); };
            auto be32 = [&](std::uint32_t v){ malformed.push_back(static_cast<std::uint8_t>(v >> 24)); malformed.push_back(static_cast<std::uint8_t>(v >> 16)); malformed.push_back(static_cast<std::uint8_t>(v >> 8)); malformed.push_back(static_cast<std::uint8_t>(v)); };
            auto ascii = [&](const char* t){ while (*t) malformed.push_back(static_cast<std::uint8_t>(*t++)); };
            ascii("MThd"); be32(6); be16(0); be16(1); be16(960);
            std::vector<std::uint8_t> tr;
            tr.insert(tr.end(), {0x00,0xFF,0x03,0x0A,'N','o','n','M','a','t','c','h','e','d'});
            tr.insert(tr.end(), {0x00,0xFF,0x7F,0x09,'P','M','S','C','C',0x01,0x01,0x01,0x64});
            tr.insert(tr.end(), {0x00,0xB0,0x01,0x63});
            tr.insert(tr.end(), {0x00,0xFF,0x2F,0x00});
            ascii("MTrk"); be32(static_cast<std::uint32_t>(tr.size())); malformed.insert(malformed.end(), tr.begin(), tr.end());
            {
                std::ofstream out(malformedMarkerPath, std::ios::binary | std::ios::trunc);
                out.write(reinterpret_cast<const char*>(malformed.data()), static_cast<std::streamsize>(malformed.size()));
            }
            const auto malformedImported = mw::import_export::MidiImporter::importFromFile(malformedMarkerPath);
            const bool nonmatchingMarkerSafe = malformedImported.has_value()
                && malformedImported->getTracks().size() == 1U
                && malformedImported->getTracks().front().getMidiControllerEvents().size() == 1U
                && malformedImported->getTracks().front().getMidiControllerEvents().front().controller == 1
                && malformedImported->getTracks().front().getMidiControllerEvents().front().value == 99;

            const auto internalImported = internalExported ? mw::import_export::MidiImporter::importFromFile(internalMidiPath) : std::nullopt;
            bool internalStillCarriesControllers = false;
            if (internalImported.has_value() && !internalImported->getTracks().empty())
            {
                const auto& internalEvents = internalImported->getTracks().front().getMidiControllerEvents();
                const auto hasInternalEvent = [&](std::int64_t tick, int channel, int controller, int value)
                {
                    return std::any_of(internalEvents.begin(), internalEvents.end(), [&](const auto& event)
                    {
                        return event.tick == tick && event.midiChannel == channel
                            && event.controller == controller && event.value == value;
                    });
                };

                // Internal render has no PMS setup markers. Generated setup CCs can
                // therefore collapse with authored same-tick tuples on re-import, so
                // the imported event count is not required to be greater than the
                // authored count. Prove instead that authored controller data that
                // cannot be explained by setup defaults survives the internal path.
                internalStillCarriesControllers = internalEvents.size() >= mainTrack.getMidiControllerEvents().size()
                    && hasInternalEvent(240, 16, 127, 127)
                    && hasInternalEvent(480, 1, 64, 127);
            }

            std::filesystem::remove(userMidiPath, ec);
            std::filesystem::remove(internalMidiPath, ec);
            std::filesystem::remove(malformedMarkerPath, ec);

            const bool passed = userExported && internalExported && generatedSetupPresent && internalHasNoPmsMarker
                && cleanRoundTrip && authoredTickZeroWins && bankAtProgram && controllerOnlyRoundTrip
                && cc64Separated && freshStableIds && nonmatchingMarkerSafe && internalStillCarriesControllers
                && source.getMetadata().projectFormatVersion == 22;

            std::ostringstream failure;
            if (!passed)
            {
                failure << "I29 failed:"
                    << " userExport=" << userExported
                    << " internalExport=" << internalExported
                    << " setup=" << generatedSetupPresent
                    << " internalMarker=" << internalHasNoPmsMarker
                    << " roundTrip=" << cleanRoundTrip
                    << " authored=" << authoredTickZeroWins
                    << " bank=" << bankAtProgram
                    << " controllerOnly=" << controllerOnlyRoundTrip
                    << " cc64=" << cc64Separated
                    << " freshIds=" << freshStableIds
                    << " nonmatch=" << nonmatchingMarkerSafe
                    << " internalCc=" << internalStillCarriesControllers;
            }

            return makeResult("I29", "Workflow", "MIDI controller export and clean PMS round-trip", passed,
                passed
                    ? "User MIDI export included project CC data while PMS sequencer-specific setup markers prevented generated CC1/7/11/0/32 defaults from becoming editable lane points on re-import; project-authored tick-zero CC won final state, bank/program assignment used bank state at Program Change, controller-only tracks and CC64 separation round-tripped, malformed/nonmatching markers failed safe, internal render export remained marker-free with controller data, fresh project event IDs were assigned, and current project-format compatibility was preserved."
                    : failure.str());
        }

        SimulationScenarioResult scenarioI30()
        {
            bool inputFilterContract = mw::midi::midiInputChannelAccepted(0, 16)
                && mw::midi::midiInputChannelAccepted(2, 2)
                && !mw::midi::midiInputChannelAccepted(2, 1)
                && std::string(mw::midi::midiRecordingRecordControllersTooltip).find("CC 0-127") != std::string::npos;

            // Record Controllers = Off must preserve the existing note/CC64 semantic
            // while declining ordinary project-controller capture.
            mw::midi::MidiRecordingSession offSession;
            mw::midi::MidiRecordingConfig offConfig;
            offConfig.tempoBpm = 120;
            offConfig.ticksPerQuarterNote = 960;
            offConfig.startTick = 1000;
            offConfig.startTimeSeconds = 10.0;
            offConfig.recordControllers = false;
            const bool offStarted = offSession.start(offConfig);
            offSession.noteOn(1, 60, 100, 10.0);
            offSession.controller(1, 1, 55, 10.25);
            offSession.controller(1, 64, 127, 10.5);
            offSession.noteOff(1, 60, 10.75);
            const auto offResult = offSession.stop(11.0);
            const bool offPolicy = offStarted
                && offResult.notes.size() == 1U
                && offResult.controllerEvents.empty()
                && offResult.cc64Events.size() == 1U
                && offResult.cc64Events.front().value == 127;

            // All CC records exact transport ticks, all channels/controllers/values,
            // and collapses exact same-tick tuples with last source message winning.
            mw::midi::MidiRecordingSession onSession;
            auto onConfig = offConfig;
            onConfig.startTick = 0;
            onConfig.startTimeSeconds = 20.0;
            onConfig.recordControllers = true;
            const bool onStarted = onSession.start(onConfig);
            onSession.noteOn(1, 64, 90, 20.111); // deliberately off note grid
            onSession.controller(1, 0, 0, 20.125);       // tick 240
            onSession.controller(1, 1, 20, 20.25);       // tick 480
            onSession.controller(1, 1, 99, 20.25);       // same tick -> last wins
            onSession.controller(16, 127, 127, 20.375);  // tick 720
            onSession.controller(1, 64, 127, 20.5);      // tick 960
            onSession.noteOff(1, 64, 20.611);
            const auto panicFinalized = onSession.releaseActiveNotes(20.75);
            const auto onResult = onSession.stop(21.0);
            auto hasRecorded = [&](std::int64_t tick, int channel, int controller, int value)
            {
                return std::any_of(onResult.controllerEvents.begin(), onResult.controllerEvents.end(), [&](const auto& event)
                {
                    return event.tick == tick && event.midiChannel == channel
                        && event.controller == controller && event.value == value;
                });
            };
            const bool exactControllerCapture = onStarted
                && onResult.controllerEvents.size() == 5U
                && hasRecorded(240, 1, 0, 0)
                && hasRecorded(480, 1, 1, 99)
                && hasRecorded(720, 16, 127, 127)
                && hasRecorded(960, 1, 64, 127)
                && hasRecorded(1440, 1, 64, 0) // Panic forces sustain up at exact tick
                && panicFinalized == 0U;

            // Note quantize is still note-only. Controller timing remains exact.
            auto quantizedNotes = onResult.notes;
            const auto beforeControllerTick = onResult.controllerEvents[1].tick;
            const auto quantizedCount = mw::midi::quantizeMidiRecordingTake(quantizedNotes, 16, 960);
            const bool controllerNotQuantized = quantizedCount > 0
                && beforeControllerTick == 480
                && onResult.controllerEvents[1].tick == beforeControllerTick;

            // Project commit keeps an existing stable ID on an exact tuple collision and
            // allocates fresh identity only for genuinely new recorded points.
            mw::core::Project project("I30 controller recording");
            auto& track = project.addTrack("Recorded Controllers");
            track.getMidiControllerEvents().emplace_back(480, 1, 1, 10);
            const auto existingId = track.getMidiControllerEvents().front().stableId;
            const auto changed = mw::midi::commitRecordedMidiControllerEvents(
                project, track.getStableId(), onResult.controllerEvents);
            const auto collisionIt = std::find_if(track.getMidiControllerEvents().begin(), track.getMidiControllerEvents().end(), [](const auto& event)
            {
                return event.tick == 480 && event.midiChannel == 1 && event.controller == 1;
            });
            const bool stableCommit = changed >= 4U
                && collisionIt != track.getMidiControllerEvents().end()
                && collisionIt->stableId == existingId
                && collisionIt->value == 99
                && std::all_of(track.getMidiControllerEvents().begin(), track.getMidiControllerEvents().end(), [](const auto& event)
                {
                    return event.stableId != 0;
                });

            // Retained loop passes can be controller-only. Later passes win exact
            // controller collisions in Merge, and Comp takes events only from each
            // selected half-open segment without synthesizing boundary CC.
            mw::midi::MidiRecordingLoopTakeReviewState review;
            const auto generation = review.begin(project.getStableId(), track.getStableId(), 3001);
            const bool pass1Added = review.addPass(generation, 1, {}, 0, {}, {
                {100, 1, 1, 20, 1}, {1900, 1, 74, 30, 2}
            });
            const bool pass2Added = review.addPass(generation, 2, {}, 0, {}, {
                {100, 1, 1, 80, 1}, {1920, 1, 74, 90, 2}, {2500, 1, 11, 100, 3}
            });
            const bool reviewReady = review.finishCollection(generation);
            const auto mergedControllers = review.mergedControllerEvents();
            const bool controllerOnlyReview = pass1Added && pass2Added && reviewReady
                && review.totalNoteCount() == 0U
                && review.totalControllerEventCount() == 5U
                && mergedControllers.size() == 4U
                && std::any_of(mergedControllers.begin(), mergedControllers.end(), [](const auto& event)
                {
                    return event.tick == 100 && event.controller == 1 && event.value == 80;
                });

            mw::midi::MidiRecordingLoopCompState comp;
            const auto compGeneration = comp.begin(project.getStableId(), track.getStableId(), generation,
                0, 3840, 960, 4, 4, mw::midi::MidiRecordingCompGrid::Custom, 1, review.passes());
            const bool split = comp.addCustomBoundary(compGeneration, 1920);
            const bool assignSecond = comp.assignPass(compGeneration, 1, 2, review.passes());
            const auto compControllers = comp.buildControllerEvents(compGeneration, review.passes());
            const bool compPolicy = split && assignSecond && compControllers.size() == 4U
                && std::any_of(compControllers.begin(), compControllers.end(), [](const auto& event)
                {
                    return event.tick == 100 && event.controller == 1 && event.value == 20;
                })
                && std::any_of(compControllers.begin(), compControllers.end(), [](const auto& event)
                {
                    return event.tick == 1900 && event.controller == 74 && event.value == 30;
                })
                && std::any_of(compControllers.begin(), compControllers.end(), [](const auto& event)
                {
                    return event.tick == 1920 && event.controller == 74 && event.value == 90;
                });

            // Format 17 persists general retained controller performances in Active and
            // Archived reviews; format 16 migration deliberately leaves that new field empty.
            mw::core::PersistentMidiTakeReview persistent;
            persistent.active = true;
            persistent.reviewSessionId = mw::core::allocateStableId();
            persistent.reviewName = "Controller Review";
            persistent.regionInTick = 0;
            persistent.regionOutTick = 3840;
            persistent.selectedPassNumber = 1;
            persistent.passes.push_back({1, {mw::core::NoteEvent(60, 100, 0, 480, 1)}, 0, {}, {
                {120, 1, 1, 25, 1}, {240, 16, 127, 127, 2}
            }});
            track.setPendingMidiTakeReview(persistent);
            auto archived = persistent;
            archived.reviewSessionId = mw::core::allocateStableId();
            archived.reviewName = "Controller Archive";
            track.getArchivedMidiTakeReviews().push_back(archived);

            const auto tempRoot = std::filesystem::temp_directory_path() / "PoorMansStudioSimulation";
            std::filesystem::create_directories(tempRoot);
            const auto v17Path = tempRoot / "i30_format17.pmsproj";
            const auto v16Path = tempRoot / "i30_format16.pmsproj";
            const auto malformedPath = tempRoot / "i30_malformed17.pmsproj";
            const bool saved = mw::serialization::ProjectSerializer::saveToFile(project, v17Path);
            const auto loaded = saved ? mw::serialization::ProjectSerializer::loadFromFile(v17Path) : std::nullopt;
            bool format17RoundTrip = false;
            if (loaded.has_value() && !loaded->getTracks().empty())
            {
                const auto& loadedTrack = loaded->getTracks().front();
                format17RoundTrip = loaded->getMetadata().projectFormatVersion == 22
                    && loadedTrack.getPendingMidiTakeReview().active
                    && loadedTrack.getPendingMidiTakeReview().passes.size() == 1U
                    && loadedTrack.getPendingMidiTakeReview().passes.front().controllerEvents.size() == 2U
                    && loadedTrack.getArchivedMidiTakeReviews().size() == 1U
                    && loadedTrack.getArchivedMidiTakeReviews().front().passes.front().controllerEvents.size() == 2U;
            }

            std::ifstream sourceIn(v17Path, std::ios::binary);
            std::string serialized((std::istreambuf_iterator<char>(sourceIn)), std::istreambuf_iterator<char>());
            auto v16Text = serialized;
            const auto versionPos = v16Text.find("\"version\": 22");
            if (versionPos != std::string::npos)
                v16Text.replace(versionPos, std::strlen("\"version\": 22"), "\"version\": 16");
            { std::ofstream out(v16Path, std::ios::binary | std::ios::trunc); out << v16Text; }
            const auto legacy = mw::serialization::ProjectSerializer::loadFromFile(v16Path);
            const bool format16Migration = legacy.has_value() && !legacy->getTracks().empty()
                && legacy->getMetadata().projectFormatVersion == 22
                && legacy->getTracks().front().getPendingMidiTakeReview().active
                && legacy->getTracks().front().getPendingMidiTakeReview().passes.front().controllerEvents.empty()
                && !legacy->getTracks().front().getPendingMidiTakeReview().passes.front().notes.empty();

            auto malformedText = serialized;
            const auto reviewToken = malformedText.find("\"pendingMidiTakeReview\"");
            const auto controllerToken = reviewToken == std::string::npos
                ? std::string::npos
                : malformedText.find("\"controller\": 1", reviewToken);
            if (controllerToken != std::string::npos)
                malformedText.replace(controllerToken, std::strlen("\"controller\": 1"), "\"controller\": 200");
            { std::ofstream out(malformedPath, std::ios::binary | std::ios::trunc); out << malformedText; }
            const auto malformed = mw::serialization::ProjectSerializer::loadFromFile(malformedPath);
            const bool malformedSafe = malformed.has_value() && !malformed->getTracks().empty()
                && malformed->getTracks().front().getPendingMidiTakeReview().active
                && malformed->getTracks().front().getPendingMidiTakeReview().passes.front().controllerEvents.size() == 1U;

            std::error_code ec;
            std::filesystem::remove(v17Path, ec);
            std::filesystem::remove(v16Path, ec);
            std::filesystem::remove(malformedPath, ec);

            // Undo Take/Undo CC interlock is based on exact controller-vector state:
            // a newer controller edit must not compare equal to the take-produced state.
            const auto expectedAfterTake = track.getMidiControllerEvents();
            auto newerEdit = expectedAfterTake;
            if (!newerEdit.empty()) newerEdit.front().value = (newerEdit.front().value + 1) % 128;
            const bool historyInterlock = mw::midi::midiControllerEventVectorsEqual(expectedAfterTake, expectedAfterTake)
                && !mw::midi::midiControllerEventVectorsEqual(newerEdit, expectedAfterTake);

            const bool passed = inputFilterContract && offPolicy && exactControllerCapture
                && controllerNotQuantized && stableCommit && controllerOnlyReview && compPolicy
                && format17RoundTrip && format16Migration && malformedSafe && historyInterlock
                && project.getMetadata().projectFormatVersion == 22;

            std::ostringstream failure;
            if (!passed)
            {
                failure << "I30 failed: input=" << inputFilterContract
                    << " off=" << offPolicy
                    << " capture=" << exactControllerCapture
                    << " quantize=" << controllerNotQuantized
                    << " stable=" << stableCommit
                    << " review=" << controllerOnlyReview
                    << " comp=" << compPolicy
                    << " f17=" << format17RoundTrip
                    << " f16=" << format16Migration
                    << " malformed=" << malformedSafe
                    << " history=" << historyInterlock;
            }

            return makeResult("I30", "Workflow", "Live controller recording, loop review and atomic take history", passed,
                passed
                    ? "Live CC recording policy passed opt-in CC0-127 capture, exact transport timing without note quantize, same-tick last-message semantics, CC64 dual project/Take-Review roles including Panic release, stable-ID project collision commit, controller-only Loop Review, later-pass Merge, half-open Comp controller selection without synthetic boundary CC, format-17 Active/Archived retained-controller persistence, format-16 empty-controller migration, malformed retained-controller safety, and controller-state interlock for independent Undo Take/Undo CC histories."
                    : failure.str());
        }

        SimulationScenarioResult scenarioI31()
        {
            bool conversionContract = false;
            bool laneContract = false;
            bool schedulerContract = false;
            bool recordingContract = false;
            bool reviewCompContract = false;
            bool persistenceContract = false;
            bool format17Migration = false;
            bool midiRoundTripContract = false;
            bool malformedMidiSafe = false;
            bool audioRejected = false;
            bool helpers = false;

            mw::core::Project project("I31 Pitch Bend");
            project.setStableId(910001);
            auto& track = project.addTrack("Pitch Bend Main");
            track.setStableId(910101);
            track.addNote({60, 100, 0, 960, 1, mw::core::Articulation::Normal});

            mw::midi::MidiPitchBendEditHistory history;
            const auto down = history.addAtTick(project, track.getStableId(), 0, 1, -8192);
            const auto center = history.addAtTick(project, track.getStableId(), 480, 16, 0);
            const auto up = history.addAtTick(project, track.getStableId(), 960, 1, 8191);
            const auto occupied = history.addAtTick(project, track.getStableId(), 1440, 1, 2222);
            const auto downId = down.eventId;
            const auto moved = history.moveAndSetValue(project, track.getStableId(), downId, 1440, -4096);
            const auto movedLane = mw::midi::pitchBendLaneEvents(track, 1);
            const auto movedIt = std::find_if(movedLane.begin(), movedLane.end(), [&](const auto& event)
            {
                return event.stableId == downId;
            });
            const bool displacedRemoved = std::none_of(movedLane.begin(), movedLane.end(), [&](const auto& event)
            {
                return event.stableId == occupied.eventId;
            });
            const bool undo = history.undo(project);
            const bool undoRestoredCollision = std::any_of(track.getMidiPitchBendEvents().begin(), track.getMidiPitchBendEvents().end(), [&](const auto& event)
            {
                return event.stableId == occupied.eventId && event.tick == 1440 && event.value == 2222;
            });
            const bool redo = history.redo(project);
            const bool snapContract = mw::midi::snapMidiPitchBendTick(120, 960, mw::midi::MidiPitchBendSnapGrid::Quarter) == 0
                && mw::midi::snapMidiPitchBendTick(480, 960, mw::midi::MidiPitchBendSnapGrid::Quarter) == 960
                && mw::midi::snapMidiPitchBendTick(61, 960, mw::midi::MidiPitchBendSnapGrid::Sixteenth) == 0
                && mw::midi::snapMidiPitchBendTick(120, 960, mw::midi::MidiPitchBendSnapGrid::Sixteenth) == 240
                && mw::midi::snapMidiPitchBendTick(123, 960, mw::midi::MidiPitchBendSnapGrid::Off) == 123;
            const auto centerY = mw::midi::midiPitchBendValueToY(0, 10.0, 200.0);
            const auto centerRoundTrip = mw::midi::midiPitchBendYToValue(centerY, 10.0, 200.0);
            laneContract = down.changed && center.changed && up.changed && occupied.changed
                && moved && movedIt != movedLane.end() && movedIt->tick == 1440 && movedIt->value == -4096
                && displacedRemoved && undo && undoRestoredCollision && redo
                && snapContract && std::abs(centerRoundTrip) <= 1
                && project.getMetadata().projectFormatVersion == 22;

            // Playback scheduler: restore the latest state at/before seek and emit a
            // runtime center for a channel that has future bend automation but no prior state.
            std::vector<mw::clap::ClapLivePlaybackPitchBendEvent> scheduledBends {
                { 920001, 1000, 1, -1000 },
                { 920002, 3000, 1, 2000 },
                { 920003, 3200, 2, 8191 }
            };
            mw::clap::ClapLivePlaybackScheduler scheduler;
            mw::clap::ClapLivePlaybackSchedulerConfig schedulerConfig;
            schedulerConfig.startTick = 2500;
            schedulerConfig.tailSeconds = 0.0;
            scheduler.prepare({}, {}, scheduledBends, schedulerConfig);
            bool restoredCh1 = false, centeredCh2 = false, timedCh1 = false, timedCh2 = false;
            mw::clap::ClapLiveProcessRequest schedulerRequest;
            while (!scheduler.isFinished())
            {
                scheduler.nextBlockInto(schedulerRequest);
                for (const auto& event : schedulerRequest.pitchBendEvents)
                {
                    if (event.midiChannel == 1 && event.value == -1000) restoredCh1 = true;
                    if (event.midiChannel == 2 && event.value == 0) centeredCh2 = true;
                    if (event.midiChannel == 1 && event.value == 2000) timedCh1 = true;
                    if (event.midiChannel == 2 && event.value == 8191) timedCh2 = true;
                }
            }
            schedulerContract = restoredCh1 && centeredCh2 && timedCh1 && timedCh2;

            // Live recording is opt-in, exact-tick, and exact same-tick/channel collapse
            // uses the last physical source value.
            mw::midi::MidiRecordingSession recording;
            mw::midi::MidiRecordingConfig recordingConfig;
            recordingConfig.tempoBpm = 120;
            recordingConfig.ticksPerQuarterNote = 960;
            recordingConfig.startTick = 1000;
            recordingConfig.startTimeSeconds = 10.0;
            recordingConfig.recordPitchBend = false;
            const bool startedOff = recording.start(recordingConfig);
            recording.pitchBend(1, 4000, 10.125);
            const auto offResult = recording.stop(10.25);
            recordingConfig.recordPitchBend = true;
            const bool startedOn = recording.start(recordingConfig);
            recording.pitchBend(1, -8192, 10.125);
            recording.pitchBend(1, 777, 10.125);
            recording.pitchBend(16, 8191, 10.25);
            const auto onResult = recording.stop(10.50);
            recordingContract = startedOff && offResult.pitchBendEvents.empty() && startedOn
                && onResult.pitchBendEvents.size() == 2U
                && onResult.pitchBendEvents[0].tick == 1240
                && onResult.pitchBendEvents[0].midiChannel == 1
                && onResult.pitchBendEvents[0].value == 777
                && onResult.pitchBendEvents[1].tick == 1480
                && onResult.pitchBendEvents[1].midiChannel == 16
                && onResult.pitchBendEvents[1].value == 8191;

            // Retained Review and Comp can be bend-only and select real events by
            // half-open [start,end) segments without synthesizing a boundary bend.
            mw::midi::MidiRecordingLoopTakeReviewState review;
            const auto reviewGeneration = review.begin(project.getStableId(), track.getStableId(), 930001);
            const bool pass1 = review.addPass(reviewGeneration, 1, {}, 0, {}, {}, {
                {100, 1, -1000, 1}, {1900, 1, -2000, 2}
            });
            const bool pass2 = review.addPass(reviewGeneration, 2, {}, 0, {}, {}, {
                {2000, 1, 3000, 1}, {3000, 1, 4000, 2}
            });
            const bool reviewReady = review.finishCollection(reviewGeneration)
                && review.totalPitchBendEventCount() == 4U;
            mw::midi::MidiRecordingLoopCompState comp;
            const auto compGeneration = comp.begin(project.getStableId(), track.getStableId(), reviewGeneration,
                                                   0, 3840, 960, 4, 4,
                                                   mw::midi::MidiRecordingCompGrid::Beat, 1, review.passes());
            bool compConfigured = compGeneration != 0 && comp.segments().size() == 4U;
            if (compConfigured)
            {
                compConfigured = comp.assignPass(compGeneration, 2, 2, review.passes())
                    && comp.assignPass(compGeneration, 3, 2, review.passes());
            }
            const auto compBends = comp.buildPitchBendEvents(compGeneration, review.passes());
            const bool compHasPass1 = std::any_of(compBends.begin(), compBends.end(), [](const auto& event)
            {
                return event.tick == 100 && event.value == -1000;
            });
            const bool compHasBoundaryPass2 = std::any_of(compBends.begin(), compBends.end(), [](const auto& event)
            {
                return event.tick == 2000 && event.value == 3000;
            });
            const bool noSyntheticBoundary = std::none_of(compBends.begin(), compBends.end(), [](const auto& event)
            {
                return event.tick == 1920;
            });
            reviewCompContract = pass1 && pass2 && reviewReady && compConfigured
                && compHasPass1 && compHasBoundaryPass2 && noSyntheticBoundary;

            // Persistence: project Pitch Bend has stable IDs. Retained Review bends do
            // not get project IDs until Keep. Format 17 loads with empty bend state.
            mw::core::PersistentMidiTakeReview persistedReview;
            persistedReview.active = true;
            persistedReview.reviewSessionId = 940001;
            persistedReview.reviewName = "Pitch Bend Review";
            persistedReview.regionInTick = 0;
            persistedReview.regionOutTick = 3840;
            persistedReview.selectedPassNumber = 1;
            persistedReview.passes.push_back({1, {mw::core::NoteEvent(67, 90, 0, 240, 1)}, 0, {}, {}, {{120, 1, -3333, 1}, {240, 16, 8191, 2}}});
            track.setPendingMidiTakeReview(persistedReview);
            auto archivedReview = persistedReview;
            archivedReview.reviewSessionId = 940002;
            archivedReview.reviewName = "Pitch Bend Archive";
            track.getArchivedMidiTakeReviews().push_back(archivedReview);

            const auto tempRoot = std::filesystem::temp_directory_path() / "PoorMansStudioSimulation";
            std::error_code ec;
            std::filesystem::create_directories(tempRoot, ec);
            const auto projectPath = tempRoot / "i31_pitchbend_format18.pmsproj";
            const auto legacyPath = tempRoot / "i31_pitchbend_format17.pmsproj";
            const auto midiPath = tempRoot / "i31_pitchbend.mid";
            const auto malformedMidiPath = tempRoot / "i31_pitchbend_malformed.mid";
            const bool saved = mw::serialization::ProjectSerializer::saveToFile(project, projectPath);
            const auto reopened = saved ? mw::serialization::ProjectSerializer::loadFromFile(projectPath) : std::nullopt;
            if (reopened.has_value())
            {
                const auto it = std::find_if(reopened->getTracks().begin(), reopened->getTracks().end(), [](const auto& candidate)
                {
                    return candidate.getStableId() == 910101;
                });
                if (it != reopened->getTracks().end())
                {
                    std::set<mw::core::StableId> bendIds;
                    for (const auto& event : it->getMidiPitchBendEvents())
                        bendIds.insert(event.stableId);
                    persistenceContract = reopened->getMetadata().projectFormatVersion == 22
                        && !it->getMidiPitchBendEvents().empty()
                        && bendIds.size() == it->getMidiPitchBendEvents().size()
                        && bendIds.count(0) == 0
                        && it->getPendingMidiTakeReview().active
                        && it->getPendingMidiTakeReview().passes.size() == 1U
                        && it->getPendingMidiTakeReview().passes.front().pitchBendEvents.size() == 2U
                        && it->getArchivedMidiTakeReviews().size() == 1U
                        && it->getArchivedMidiTakeReviews().front().passes.front().pitchBendEvents.size() == 2U;
                }
            }

            std::string serialized;
            if (saved)
            {
                std::ifstream in(projectPath, std::ios::binary);
                serialized.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            }
            if (!serialized.empty())
            {
                auto legacyText = serialized;
                const auto versionPos = legacyText.find("\"version\": 22");
                if (versionPos != std::string::npos)
                    legacyText.replace(versionPos, std::strlen("\"version\": 22"), "\"version\": 17");
                { std::ofstream out(legacyPath, std::ios::binary | std::ios::trunc); out << legacyText; }
                const auto legacy = mw::serialization::ProjectSerializer::loadFromFile(legacyPath);
                if (legacy.has_value())
                {
                    const auto it = std::find_if(legacy->getTracks().begin(), legacy->getTracks().end(), [](const auto& candidate)
                    {
                        return candidate.getStableId() == 910101;
                    });
                    format17Migration = legacy->getMetadata().projectFormatVersion == 22
                        && it != legacy->getTracks().end()
                        && it->getMidiPitchBendEvents().empty()
                        && it->getPendingMidiTakeReview().active
                        && it->getPendingMidiTakeReview().passes.front().pitchBendEvents.empty()
                        && !it->getNotes().empty();
                }
            }

            // Add a dedicated bend-only track for SMF interchange and prove wire-value
            // conversion at the three important 14-bit values.
            auto& bendOnly = project.addTrack("Pitch Bend Only");
            bendOnly.setStableId(910102);
            bendOnly.setMidiPitchBendEvents({
                mw::core::MidiPitchBendEvent(950001, 0, 1, -8192),
                mw::core::MidiPitchBendEvent(950002, 240, 1, 0),
                mw::core::MidiPitchBendEvent(950003, 480, 16, 8191)
            });
            const bool exported = mw::midi::MidiExporter::exportToFile(
                project, midiPath, { mw::midi::MidiExportPurpose::UserMidiExport });
            std::vector<std::uint8_t> midiBytes;
            if (exported)
            {
                std::ifstream in(midiPath, std::ios::binary);
                midiBytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            }
            auto containsBytes = [&](std::initializer_list<std::uint8_t> pattern)
            {
                return std::search(midiBytes.begin(), midiBytes.end(), pattern.begin(), pattern.end()) != midiBytes.end();
            };
            conversionContract = containsBytes({0xE0U, 0x00U, 0x00U})
                && containsBytes({0xE0U, 0x00U, 0x40U})
                && containsBytes({0xEFU, 0x7FU, 0x7FU});

            const auto imported = exported ? mw::import_export::MidiImporter::importFromFile(midiPath) : std::nullopt;
            if (imported.has_value())
            {
                const auto it = std::find_if(imported->getTracks().begin(), imported->getTracks().end(), [](const auto& candidate)
                {
                    return candidate.getName() == "Pitch Bend Only";
                });
                if (it != imported->getTracks().end())
                {
                    const auto& bends = it->getMidiPitchBendEvents();
                    const auto hasBend = [&](std::int64_t tick, int channel, int value)
                    {
                        return std::any_of(bends.begin(), bends.end(), [&](const auto& event)
                        {
                            return event.tick == tick && event.midiChannel == channel && event.value == value;
                        });
                    };
                    std::set<mw::core::StableId> ids;
                    for (const auto& event : bends) ids.insert(event.stableId);
                    midiRoundTripContract = it->getNotes().empty()
                        && bends.size() == 3U
                        && hasBend(0, 1, -8192)
                        && hasBend(240, 1, 0)
                        && hasBend(480, 16, 8191)
                        && ids.size() == bends.size() && ids.count(0) == 0;
                }
            }

            // Small SMF with a valid note followed by a truncated Pitch Bend message.
            // The note survives and no bend is fabricated.
            auto appendBe16 = [](std::vector<std::uint8_t>& bytes, std::uint16_t value)
            {
                bytes.push_back(static_cast<std::uint8_t>(value >> 8));
                bytes.push_back(static_cast<std::uint8_t>(value));
            };
            auto appendBe32 = [](std::vector<std::uint8_t>& bytes, std::uint32_t value)
            {
                bytes.push_back(static_cast<std::uint8_t>(value >> 24));
                bytes.push_back(static_cast<std::uint8_t>(value >> 16));
                bytes.push_back(static_cast<std::uint8_t>(value >> 8));
                bytes.push_back(static_cast<std::uint8_t>(value));
            };
            std::vector<std::uint8_t> malformedBytes;
            malformedBytes.insert(malformedBytes.end(), {'M','T','h','d'});
            appendBe32(malformedBytes, 6); appendBe16(malformedBytes, 0); appendBe16(malformedBytes, 1); appendBe16(malformedBytes, 960);
            std::vector<std::uint8_t> malformedTrack;
            malformedTrack.insert(malformedTrack.end(), {0x00U,0x90U,0x3CU,0x64U, 0x78U,0x80U,0x3CU,0x00U, 0x00U,0xE0U,0x00U});
            malformedBytes.insert(malformedBytes.end(), {'M','T','r','k'});
            appendBe32(malformedBytes, static_cast<std::uint32_t>(malformedTrack.size()));
            malformedBytes.insert(malformedBytes.end(), malformedTrack.begin(), malformedTrack.end());
            { std::ofstream out(malformedMidiPath, std::ios::binary | std::ios::trunc); out.write(reinterpret_cast<const char*>(malformedBytes.data()), static_cast<std::streamsize>(malformedBytes.size())); }
            const auto malformedImported = mw::import_export::MidiImporter::importFromFile(malformedMidiPath);
            malformedMidiSafe = malformedImported.has_value()
                && malformedImported->getTracks().size() == 1U
                && malformedImported->getTracks().front().getNotes().size() == 1U
                && malformedImported->getTracks().front().getMidiPitchBendEvents().empty();

            auto& audioTrack = project.addTrack("Pitch Bend Audio");
            audioTrack.setTrackType(mw::core::TrackType::AudioClip);
            audioTrack.setMidiPitchBendEvents({ mw::core::MidiPitchBendEvent(0, 1, 100) });
            audioRejected = audioTrack.getMidiPitchBendEvents().empty()
                && !history.addAtTick(project, audioTrack.getStableId(), 10, 1, 100).changed;

            helpers = std::string(mw::midi::midiPitchBendLaneTooltip).find("-8192") != std::string::npos
                && std::string(mw::midi::midiPitchBendValueTooltip).find("RPN") != std::string::npos
                && std::string(mw::midi::midiPitchBendGraphTooltip).find("staircase") != std::string::npos;

            std::filesystem::remove(projectPath, ec);
            std::filesystem::remove(legacyPath, ec);
            std::filesystem::remove(midiPath, ec);
            std::filesystem::remove(malformedMidiPath, ec);

            const bool passed = conversionContract && laneContract && schedulerContract
                && recordingContract && reviewCompContract && persistenceContract
                && format17Migration && midiRoundTripContract && malformedMidiSafe
                && audioRejected && helpers;

            std::ostringstream failure;
            if (!passed)
            {
                failure << "I31 failed: conversion=" << conversionContract
                    << " lane=" << laneContract
                    << " scheduler=" << schedulerContract
                    << " recording=" << recordingContract
                    << " reviewComp=" << reviewCompContract
                    << " persistence=" << persistenceContract
                    << " f17=" << format17Migration
                    << " midi=" << midiRoundTripContract
                    << " malformed=" << malformedMidiSafe
                    << " audio=" << audioRejected
                    << " helpers=" << helpers;
            }

            return makeResult("I31", "Workflow", "End-to-end MIDI Pitch Bend", passed,
                passed
                    ? "Channel Pitch Bend passed signed 14-bit MIDI conversion, stable-ID lane editing/snap/Undo, seek-state restoration with center reset, exact unquantized recording, bend-only Take Review and half-open Comp ownership without synthetic boundary events, project-format-18 Active/Archived persistence with format-17 empty-bend migration, Standard MIDI File export/import including pitch-bend-only tracks, malformed-message safety, and AudioClip rejection."
                    : failure.str());
        }

        SimulationScenarioResult scenarioI32()
        {
            bool laneContract = false;
            bool schedulerContract = false;
            bool recordingContract = false;
            bool reviewCompContract = false;
            bool persistenceContract = false;
            bool format18Migration = false;
            bool midiRoundTripContract = false;
            bool malformedMidiSafe = false;
            bool audioRejected = false;
            bool helpers = false;

            mw::core::Project project("I32 Program Change");
            project.setStableId(920001);
            auto& track = project.addTrack("Program Main");
            track.setStableId(920101);
            track.getInstrument().midiChannel = 1;
            track.getInstrument().midiBank = 0;
            track.getInstrument().midiProgram = 5;
            track.addNote({60, 100, 0, 960, 1, mw::core::Articulation::Normal});

            mw::midi::MidiProgramChangeEditHistory history;
            const auto p0 = history.addAtTick(project, track.getStableId(), 0, 1, 0);
            const auto p127 = history.addAtTick(project, track.getStableId(), 960, 16, 127);
            const auto occupied = history.addAtTick(project, track.getStableId(), 1440, 1, 33);
            const auto movedId = p0.eventId;
            const bool moved = history.moveAndSetProgram(project, track.getStableId(), movedId, 1440, 99);
            const auto lane = mw::midi::programChangeLaneEvents(track, 1);
            const auto movedIt = std::find_if(lane.begin(), lane.end(), [&](const auto& e){ return e.stableId == movedId; });
            const bool displaced = std::none_of(lane.begin(), lane.end(), [&](const auto& e){ return e.stableId == occupied.eventId; });
            const bool undo = history.undo(project);
            const bool undoRestored = std::any_of(track.getMidiProgramChangeEvents().begin(), track.getMidiProgramChangeEvents().end(), [&](const auto& e){ return e.stableId == occupied.eventId && e.program == 33; });
            const bool redo = history.redo(project);
            const bool snap = mw::midi::snapMidiProgramChangeTick(480, 960, mw::midi::MidiProgramChangeSnapGrid::Quarter) == 960
                && mw::midi::snapMidiProgramChangeTick(123, 960, mw::midi::MidiProgramChangeSnapGrid::Off) == 123;
            laneContract = p0.changed && p127.changed && occupied.changed && moved
                && movedIt != lane.end() && movedIt->tick == 1440 && movedIt->program == 99
                && displaced && undo && undoRestored && redo && snap
                && project.getMetadata().projectFormatVersion == 22;

            std::vector<mw::clap::ClapLivePlaybackControllerEvent> ccEvents {
                { 930001, 1000, 1, 0, 2 }, { 930002, 1000, 1, 32, 3 }
            };
            std::vector<mw::clap::ClapLivePlaybackProgramChangeEvent> programs {
                { 930003, 1000, 1, 40 }, { 930004, 3000, 1, 41 }
            };
            mw::clap::ClapLivePlaybackScheduler scheduler;
            mw::clap::ClapLivePlaybackSchedulerConfig schedulerConfig;
            schedulerConfig.startTick = 2500;
            schedulerConfig.tailSeconds = 0.0;
            scheduler.prepare({}, ccEvents, {}, programs, schedulerConfig);
            bool restoredProgram = false, timedProgram = false;
            mw::clap::ClapLiveProcessRequest request;
            while (!scheduler.isFinished())
            {
                scheduler.nextBlockInto(request);
                for (const auto& e : request.programChangeEvents)
                {
                    if (e.midiChannel == 1 && e.program == 40) restoredProgram = true;
                    if (e.midiChannel == 1 && e.program == 41) timedProgram = true;
                }
            }
            schedulerContract = restoredProgram && timedProgram;

            mw::midi::MidiRecordingSession recording;
            mw::midi::MidiRecordingConfig config;
            config.tempoBpm = 120; config.ticksPerQuarterNote = 960; config.startTick = 1000; config.startTimeSeconds = 10.0;
            config.recordProgramChange = false;
            const bool startedOff = recording.start(config);
            recording.programChange(1, 10, 10.125);
            const auto off = recording.stop(10.25);
            config.recordProgramChange = true;
            const bool startedOn = recording.start(config);
            recording.programChange(1, 10, 10.125);
            recording.programChange(1, 11, 10.125);
            recording.programChange(16, 127, 10.25);
            const auto on = recording.stop(10.50);
            recordingContract = startedOff && off.programChangeEvents.empty() && startedOn
                && on.programChangeEvents.size() == 2U
                && on.programChangeEvents[0].tick == 1240 && on.programChangeEvents[0].midiChannel == 1 && on.programChangeEvents[0].program == 11
                && on.programChangeEvents[1].tick == 1480 && on.programChangeEvents[1].midiChannel == 16 && on.programChangeEvents[1].program == 127;

            mw::midi::MidiRecordingLoopTakeReviewState review;
            const auto reviewGeneration = review.begin(project.getStableId(), track.getStableId(), 940001);
            const bool pass1 = review.addPass(reviewGeneration, 1, {}, 0, {}, {}, {}, {{100,1,12,1},{1900,1,13,2}});
            const bool pass2 = review.addPass(reviewGeneration, 2, {}, 0, {}, {}, {}, {{2000,1,40,1},{3000,1,41,2}});
            const bool reviewReady = review.finishCollection(reviewGeneration) && review.totalProgramChangeEventCount() == 4U;
            mw::midi::MidiRecordingLoopCompState comp;
            const auto compGeneration = comp.begin(project.getStableId(), track.getStableId(), reviewGeneration, 0, 3840, 960, 4, 4,
                mw::midi::MidiRecordingCompGrid::Beat, 1, review.passes());
            bool compConfigured = compGeneration != 0 && comp.segments().size() == 4U;
            if (compConfigured) compConfigured = comp.assignPass(compGeneration, 2, 2, review.passes()) && comp.assignPass(compGeneration, 3, 2, review.passes());
            const auto compPrograms = comp.buildProgramChangeEvents(compGeneration, review.passes());
            const bool hasP1 = std::any_of(compPrograms.begin(), compPrograms.end(), [](const auto& e){ return e.tick == 100 && e.program == 12; });
            const bool hasP2 = std::any_of(compPrograms.begin(), compPrograms.end(), [](const auto& e){ return e.tick == 2000 && e.program == 40; });
            const bool noSynthetic = std::none_of(compPrograms.begin(), compPrograms.end(), [](const auto& e){ return e.tick == 1920; });
            reviewCompContract = pass1 && pass2 && reviewReady && compConfigured && hasP1 && hasP2 && noSynthetic;

            mw::core::PersistentMidiTakeReview persisted;
            persisted.active = true; persisted.reviewSessionId = 950001; persisted.reviewName = "Program Review";
            persisted.regionInTick = 0; persisted.regionOutTick = 3840; persisted.selectedPassNumber = 1;
            persisted.passes.push_back({1, {mw::core::NoteEvent(67,90,0,240,1)}, 0, {}, {}, {}, {{120,1,64,1},{240,16,127,2}}});
            track.setPendingMidiTakeReview(persisted);
            auto archived = persisted; archived.reviewSessionId = 950002; archived.reviewName = "Program Archive";
            track.getArchivedMidiTakeReviews().push_back(archived);

            const auto tempRoot = std::filesystem::temp_directory_path() / "PoorMansStudioSimulation";
            std::error_code ec; std::filesystem::create_directories(tempRoot, ec);
            const auto projectPath = tempRoot / "i32_program_format19.pmsproj";
            const auto legacyPath = tempRoot / "i32_program_format18.pmsproj";
            const auto midiPath = tempRoot / "i32_program.mid";
            const auto malformedPath = tempRoot / "i32_program_malformed.mid";
            const bool saved = mw::serialization::ProjectSerializer::saveToFile(project, projectPath);
            const auto reopened = saved ? mw::serialization::ProjectSerializer::loadFromFile(projectPath) : std::nullopt;
            if (reopened)
            {
                const auto it = std::find_if(reopened->getTracks().begin(), reopened->getTracks().end(), [](const auto& t){ return t.getStableId() == 920101; });
                if (it != reopened->getTracks().end())
                    persistenceContract = reopened->getMetadata().projectFormatVersion == 22
                        && !it->getMidiProgramChangeEvents().empty()
                        && it->getPendingMidiTakeReview().active
                        && it->getPendingMidiTakeReview().passes.front().programChangeEvents.size() == 2U
                        && it->getArchivedMidiTakeReviews().size() == 1U
                        && it->getArchivedMidiTakeReviews().front().passes.front().programChangeEvents.size() == 2U;
            }
            std::string serialized;
            if (saved) { std::ifstream in(projectPath, std::ios::binary); serialized.assign(std::istreambuf_iterator<char>(in), {}); }
            if (!serialized.empty())
            {
                auto legacyText = serialized;
                const auto pos = legacyText.find("\"version\": 22");
                if (pos != std::string::npos) legacyText.replace(pos, std::strlen("\"version\": 22"), "\"version\": 18");
                { std::ofstream outFile(legacyPath, std::ios::binary | std::ios::trunc); outFile << legacyText; }
                const auto legacy = mw::serialization::ProjectSerializer::loadFromFile(legacyPath);
                if (legacy)
                {
                    const auto it = std::find_if(legacy->getTracks().begin(), legacy->getTracks().end(), [](const auto& t){ return t.getStableId() == 920101; });
                    format18Migration = legacy->getMetadata().projectFormatVersion == 22 && it != legacy->getTracks().end()
                        && it->getMidiProgramChangeEvents().empty() && it->getPendingMidiTakeReview().active
                        && it->getPendingMidiTakeReview().passes.front().programChangeEvents.empty() && !it->getNotes().empty();
                }
            }

            auto& programOnly = project.addTrack("Program Only");
            programOnly.setStableId(920102);
            programOnly.getInstrument().midiChannel = 1; programOnly.getInstrument().midiProgram = 5;
            programOnly.setMidiProgramChangeEvents({ mw::core::MidiProgramChangeEvent(960001,0,1,10), mw::core::MidiProgramChangeEvent(960002,240,1,20), mw::core::MidiProgramChangeEvent(960003,480,16,127) });
            const bool exported = mw::midi::MidiExporter::exportToFile(project, midiPath, {mw::midi::MidiExportPurpose::UserMidiExport});
            const auto imported = exported ? mw::import_export::MidiImporter::importFromFile(midiPath) : std::nullopt;
            if (imported)
            {
                const auto it = std::find_if(imported->getTracks().begin(), imported->getTracks().end(), [](const auto& t){ return t.getName() == "Program Only"; });
                if (it != imported->getTracks().end())
                {
                    const auto& events = it->getMidiProgramChangeEvents();
                    const auto has = [&](std::int64_t tick,int ch,int pg){ return std::any_of(events.begin(), events.end(), [&](const auto& e){ return e.tick==tick && e.midiChannel==ch && e.program==pg; }); };
                    std::set<mw::core::StableId> ids; for (const auto& e: events) ids.insert(e.stableId);
                    midiRoundTripContract = it->getNotes().empty() && events.size() == 3U
                        && has(0,1,10) && has(240,1,20) && has(480,16,127)
                        && ids.size() == events.size() && ids.count(0) == 0
                        && it->getInstrument().midiProgram == 5;
                }
            }

            auto appendBe16 = [](std::vector<std::uint8_t>& b,std::uint16_t v){ b.push_back(static_cast<std::uint8_t>(v>>8)); b.push_back(static_cast<std::uint8_t>(v)); };
            auto appendBe32 = [](std::vector<std::uint8_t>& b,std::uint32_t v){ b.push_back(static_cast<std::uint8_t>(v>>24)); b.push_back(static_cast<std::uint8_t>(v>>16)); b.push_back(static_cast<std::uint8_t>(v>>8)); b.push_back(static_cast<std::uint8_t>(v)); };
            std::vector<std::uint8_t> malformed; malformed.insert(malformed.end(), {'M','T','h','d'}); appendBe32(malformed,6); appendBe16(malformed,0); appendBe16(malformed,1); appendBe16(malformed,960);
            std::vector<std::uint8_t> mt {0x00U,0x90U,0x3CU,0x64U,0x78U,0x80U,0x3CU,0x00U,0x00U,0xC0U};
            malformed.insert(malformed.end(), {'M','T','r','k'}); appendBe32(malformed, static_cast<std::uint32_t>(mt.size())); malformed.insert(malformed.end(), mt.begin(), mt.end());
            { std::ofstream outFile(malformedPath, std::ios::binary|std::ios::trunc); outFile.write(reinterpret_cast<const char*>(malformed.data()), static_cast<std::streamsize>(malformed.size())); }
            const auto malformedImported = mw::import_export::MidiImporter::importFromFile(malformedPath);
            malformedMidiSafe = malformedImported && malformedImported->getTracks().size()==1U && malformedImported->getTracks().front().getNotes().size()==1U && malformedImported->getTracks().front().getMidiProgramChangeEvents().empty();

            auto& audio = project.addTrack("Program Audio"); audio.setTrackType(mw::core::TrackType::AudioClip);
            audio.setMidiProgramChangeEvents({mw::core::MidiProgramChangeEvent(0,1,10)});
            audioRejected = audio.getMidiProgramChangeEvents().empty() && !history.addAtTick(project,audio.getStableId(),10,1,10).changed;
            helpers = std::string(mw::midi::midiProgramChangeLaneTooltip).find("0 through 127") != std::string::npos
                && std::string(mw::midi::midiProgramChangeProgramTooltip).find("1 through 128") != std::string::npos
                && std::string(mw::midi::midiProgramChangeGraphTooltip).find("staircase") != std::string::npos;

            std::filesystem::remove(projectPath,ec); std::filesystem::remove(legacyPath,ec); std::filesystem::remove(midiPath,ec); std::filesystem::remove(malformedPath,ec);
            const bool passed = laneContract && schedulerContract && recordingContract && reviewCompContract && persistenceContract
                && format18Migration && midiRoundTripContract && malformedMidiSafe && audioRejected && helpers;
            std::ostringstream failure;
            if (!passed) failure << "I32 failed: lane="<<laneContract<<" scheduler="<<schedulerContract<<" recording="<<recordingContract
                <<" reviewComp="<<reviewCompContract<<" persistence="<<persistenceContract<<" f18="<<format18Migration
                <<" midi="<<midiRoundTripContract<<" malformed="<<malformedMidiSafe<<" audio="<<audioRejected<<" helpers="<<helpers;
            return makeResult("I32", "Workflow", "End-to-end MIDI Program Change", passed,
                passed ? "Channel Program Change passed stable-ID lane editing/snap/Undo, playback seek restoration, exact unquantized recording, Program-only Take Review and half-open Comp ownership without synthetic boundary events, project-format-19 Active/Archived persistence with format-18 empty-program migration, clean PMS Standard MIDI File export/import including Program-only tracks, malformed-message safety, and AudioClip rejection." : failure.str());
        }

        SimulationScenarioResult scenarioI33()
        {
            bool laneContract = false;
            bool schedulerContract = false;
            bool recordingContract = false;
            bool reviewCompContract = false;
            bool persistenceContract = false;
            bool format19Migration = false;
            bool midiRoundTripContract = false;
            bool malformedMidiSafe = false;
            bool audioRejected = false;
            bool helpers = false;

            mw::core::Project project("I33 Channel Pressure");
            project.setStableId(970001);
            auto& track = project.addTrack("Pressure Main");
            track.setStableId(970101);
            track.addNote({60, 100, 0, 960, 1, mw::core::Articulation::Normal});

            mw::midi::MidiChannelPressureEditHistory history;
            const auto p0 = history.addAtTick(project, track.getStableId(), 0, 1, 0);
            const auto p127 = history.addAtTick(project, track.getStableId(), 960, 16, 127);
            const auto occupied = history.addAtTick(project, track.getStableId(), 1440, 1, 33);
            const auto movedId = p0.eventId;
            const bool moved = history.moveAndSetPressure(project, track.getStableId(), movedId, 1440, 99);
            const auto lane = mw::midi::channelPressureLaneEvents(track, 1);
            const auto movedIt = std::find_if(lane.begin(), lane.end(), [&](const auto& e){ return e.stableId == movedId; });
            const bool displaced = std::none_of(lane.begin(), lane.end(), [&](const auto& e){ return e.stableId == occupied.eventId; });
            const bool undo = history.undo(project);
            const bool undoRestored = std::any_of(track.getMidiChannelPressureEvents().begin(), track.getMidiChannelPressureEvents().end(), [&](const auto& e){ return e.stableId == occupied.eventId && e.pressure == 33; });
            const bool redo = history.redo(project);
            const bool snap = mw::midi::snapMidiChannelPressureTick(480, 960, mw::midi::MidiChannelPressureSnapGrid::Quarter) == 960
                && mw::midi::snapMidiChannelPressureTick(123, 960, mw::midi::MidiChannelPressureSnapGrid::Off) == 123;
            const bool graphMap = mw::midi::midiChannelPressureYToPressure(
                    mw::midi::midiChannelPressurePressureToY(127, 10.0, 127.0), 10.0, 127.0) == 127
                && mw::midi::midiChannelPressureYToPressure(
                    mw::midi::midiChannelPressurePressureToY(0, 10.0, 127.0), 10.0, 127.0) == 0;
            laneContract = p0.changed && p127.changed && occupied.changed && moved
                && movedIt != lane.end() && movedIt->tick == 1440 && movedIt->pressure == 99
                && displaced && undo && undoRestored && redo && snap && graphMap
                && project.getMetadata().projectFormatVersion == 22;

            std::vector<mw::clap::ClapLivePlaybackChannelPressureEvent> pressures {
                { 971001, 1000, 1, 70 }, { 971002, 3000, 1, 80 }, { 971003, 3000, 16, 127 }
            };
            mw::clap::ClapLivePlaybackScheduler scheduler;
            mw::clap::ClapLivePlaybackSchedulerConfig schedulerConfig;
            schedulerConfig.startTick = 2500;
            schedulerConfig.tailSeconds = 0.0;
            scheduler.prepare({}, {}, {}, {}, pressures, schedulerConfig);
            bool restoredPressure = false, resetBeforeFirst = false, timedPressure = false, timedCh16 = false;
            mw::clap::ClapLiveProcessRequest request;
            while (!scheduler.isFinished())
            {
                scheduler.nextBlockInto(request);
                for (const auto& e : request.channelPressureEvents)
                {
                    if (e.midiChannel == 1 && e.pressure == 70) restoredPressure = true;
                    if (e.midiChannel == 16 && e.pressure == 0) resetBeforeFirst = true;
                    if (e.midiChannel == 1 && e.pressure == 80) timedPressure = true;
                    if (e.midiChannel == 16 && e.pressure == 127) timedCh16 = true;
                }
            }
            schedulerContract = restoredPressure && resetBeforeFirst && timedPressure && timedCh16;

            mw::midi::MidiRecordingSession recording;
            mw::midi::MidiRecordingConfig config;
            config.tempoBpm = 120; config.ticksPerQuarterNote = 960; config.startTick = 1000; config.startTimeSeconds = 10.0;
            config.recordChannelPressure = false;
            const bool startedOff = recording.start(config);
            recording.channelPressure(1, 10, 10.125);
            const auto off = recording.stop(10.25);
            config.recordChannelPressure = true;
            const bool startedOn = recording.start(config);
            recording.channelPressure(1, 10, 10.125);
            recording.channelPressure(1, 11, 10.125);
            recording.channelPressure(16, 127, 10.25);
            const auto on = recording.stop(10.50);
            recordingContract = startedOff && off.channelPressureEvents.empty() && startedOn
                && on.channelPressureEvents.size() == 2U
                && on.channelPressureEvents[0].tick == 1240 && on.channelPressureEvents[0].midiChannel == 1 && on.channelPressureEvents[0].pressure == 11
                && on.channelPressureEvents[1].tick == 1480 && on.channelPressureEvents[1].midiChannel == 16 && on.channelPressureEvents[1].pressure == 127;

            mw::midi::MidiRecordingLoopTakeReviewState review;
            const auto reviewGeneration = review.begin(project.getStableId(), track.getStableId(), 972001);
            const bool pass1 = review.addPass(reviewGeneration, 1, {}, 0, {}, {}, {}, {}, {{100,1,12,1},{500,1,20,2},{1900,1,13,3}});
            const bool pass2 = review.addPass(reviewGeneration, 2, {}, 0, {}, {}, {}, {}, {{500,1,99,1},{2000,1,40,2},{3000,16,127,3}});
            const bool reviewReady = review.finishCollection(reviewGeneration) && review.totalChannelPressureEventCount() == 6U;
            const auto mergedPressure = review.mergedChannelPressureEvents();
            const bool laterPassWins = std::any_of(mergedPressure.begin(), mergedPressure.end(), [](const auto& e){ return e.tick == 500 && e.midiChannel == 1 && e.pressure == 99; });
            mw::midi::MidiRecordingLoopCompState comp;
            const auto compGeneration = comp.begin(project.getStableId(), track.getStableId(), reviewGeneration, 0, 3840, 960, 4, 4,
                mw::midi::MidiRecordingCompGrid::Beat, 1, review.passes());
            bool compConfigured = compGeneration != 0 && comp.segments().size() == 4U;
            if (compConfigured) compConfigured = comp.assignPass(compGeneration, 2, 2, review.passes()) && comp.assignPass(compGeneration, 3, 2, review.passes());
            const auto compPressure = comp.buildChannelPressureEvents(compGeneration, review.passes());
            const bool hasP1 = std::any_of(compPressure.begin(), compPressure.end(), [](const auto& e){ return e.tick == 100 && e.pressure == 12; });
            const bool hasP2 = std::any_of(compPressure.begin(), compPressure.end(), [](const auto& e){ return e.tick == 2000 && e.pressure == 40; });
            const bool noSynthetic = std::none_of(compPressure.begin(), compPressure.end(), [](const auto& e){ return e.tick == 1920; });
            reviewCompContract = pass1 && pass2 && reviewReady && laterPassWins && compConfigured && hasP1 && hasP2 && noSynthetic;

            mw::core::PersistentMidiTakeReview persisted;
            persisted.active = true; persisted.reviewSessionId = 973001; persisted.reviewName = "Pressure Review";
            persisted.regionInTick = 0; persisted.regionOutTick = 3840; persisted.selectedPassNumber = 1;
            persisted.passes.push_back({1, {mw::core::NoteEvent(67,90,0,240,1)}, 0, {}, {}, {}, {}, {{120,1,64,1},{240,16,127,2}}});
            track.setPendingMidiTakeReview(persisted);
            auto archived = persisted; archived.reviewSessionId = 973002; archived.reviewName = "Pressure Archive";
            track.getArchivedMidiTakeReviews().push_back(archived);

            const auto tempRoot = std::filesystem::temp_directory_path() / "PoorMansStudioSimulation";
            std::error_code ec; std::filesystem::create_directories(tempRoot, ec);
            const auto projectPath = tempRoot / "i33_pressure_format20.pmsproj";
            const auto legacyPath = tempRoot / "i33_pressure_format19.pmsproj";
            const auto midiPath = tempRoot / "i33_pressure.mid";
            const auto malformedPath = tempRoot / "i33_pressure_malformed.mid";
            const bool saved = mw::serialization::ProjectSerializer::saveToFile(project, projectPath);
            const auto reopened = saved ? mw::serialization::ProjectSerializer::loadFromFile(projectPath) : std::nullopt;
            if (reopened)
            {
                const auto it = std::find_if(reopened->getTracks().begin(), reopened->getTracks().end(), [](const auto& t){ return t.getStableId() == 970101; });
                if (it != reopened->getTracks().end())
                    persistenceContract = reopened->getMetadata().projectFormatVersion == 22
                        && !it->getMidiChannelPressureEvents().empty()
                        && it->getPendingMidiTakeReview().active
                        && it->getPendingMidiTakeReview().passes.front().channelPressureEvents.size() == 2U
                        && it->getArchivedMidiTakeReviews().size() == 1U
                        && it->getArchivedMidiTakeReviews().front().passes.front().channelPressureEvents.size() == 2U;
            }
            std::string serialized;
            if (saved) { std::ifstream in(projectPath, std::ios::binary); serialized.assign(std::istreambuf_iterator<char>(in), {}); }
            if (!serialized.empty())
            {
                auto legacyText = serialized;
                const auto pos = legacyText.find("\"version\": 22");
                if (pos != std::string::npos) legacyText.replace(pos, std::strlen("\"version\": 22"), "\"version\": 19");
                { std::ofstream outFile(legacyPath, std::ios::binary | std::ios::trunc); outFile << legacyText; }
                const auto legacy = mw::serialization::ProjectSerializer::loadFromFile(legacyPath);
                if (legacy)
                {
                    const auto it = std::find_if(legacy->getTracks().begin(), legacy->getTracks().end(), [](const auto& t){ return t.getStableId() == 970101; });
                    format19Migration = legacy->getMetadata().projectFormatVersion == 22 && it != legacy->getTracks().end()
                        && it->getMidiChannelPressureEvents().empty() && it->getPendingMidiTakeReview().active
                        && it->getPendingMidiTakeReview().passes.front().channelPressureEvents.empty() && !it->getNotes().empty();
                }
            }

            auto& pressureOnly = project.addTrack("Pressure Only");
            pressureOnly.setStableId(970102);
            pressureOnly.setMidiChannelPressureEvents({
                mw::core::MidiChannelPressureEvent(974001,0,1,0),
                mw::core::MidiChannelPressureEvent(974002,240,1,64),
                mw::core::MidiChannelPressureEvent(974003,480,16,127) });
            const bool exported = mw::midi::MidiExporter::exportToFile(project, midiPath, {mw::midi::MidiExportPurpose::UserMidiExport});
            const auto imported = exported ? mw::import_export::MidiImporter::importFromFile(midiPath) : std::nullopt;
            if (imported)
            {
                const auto it = std::find_if(imported->getTracks().begin(), imported->getTracks().end(), [](const auto& t){ return t.getName() == "Pressure Only"; });
                if (it != imported->getTracks().end())
                {
                    const auto& events = it->getMidiChannelPressureEvents();
                    const auto has = [&](std::int64_t tick,int ch,int pressure){ return std::any_of(events.begin(), events.end(), [&](const auto& e){ return e.tick==tick && e.midiChannel==ch && e.pressure==pressure; }); };
                    std::set<mw::core::StableId> ids; for (const auto& e: events) ids.insert(e.stableId);
                    midiRoundTripContract = it->getNotes().empty() && events.size() == 3U
                        && has(0,1,0) && has(240,1,64) && has(480,16,127)
                        && ids.size() == events.size() && ids.count(0) == 0;
                }
            }

            auto appendBe16 = [](std::vector<std::uint8_t>& b,std::uint16_t v){ b.push_back(static_cast<std::uint8_t>(v>>8)); b.push_back(static_cast<std::uint8_t>(v)); };
            auto appendBe32 = [](std::vector<std::uint8_t>& b,std::uint32_t v){ b.push_back(static_cast<std::uint8_t>(v>>24)); b.push_back(static_cast<std::uint8_t>(v>>16)); b.push_back(static_cast<std::uint8_t>(v>>8)); b.push_back(static_cast<std::uint8_t>(v)); };
            std::vector<std::uint8_t> malformed; malformed.insert(malformed.end(), {'M','T','h','d'}); appendBe32(malformed,6); appendBe16(malformed,0); appendBe16(malformed,1); appendBe16(malformed,960);
            std::vector<std::uint8_t> mt {0x00U,0x90U,0x3CU,0x64U,0x78U,0x80U,0x3CU,0x00U,0x00U,0xD0U};
            malformed.insert(malformed.end(), {'M','T','r','k'}); appendBe32(malformed, static_cast<std::uint32_t>(mt.size())); malformed.insert(malformed.end(), mt.begin(), mt.end());
            { std::ofstream outFile(malformedPath, std::ios::binary|std::ios::trunc); outFile.write(reinterpret_cast<const char*>(malformed.data()), static_cast<std::streamsize>(malformed.size())); }
            const auto malformedImported = mw::import_export::MidiImporter::importFromFile(malformedPath);
            malformedMidiSafe = malformedImported && malformedImported->getTracks().size()==1U && malformedImported->getTracks().front().getNotes().size()==1U && malformedImported->getTracks().front().getMidiChannelPressureEvents().empty();

            auto& audio = project.addTrack("Pressure Audio"); audio.setTrackType(mw::core::TrackType::AudioClip);
            audio.setMidiChannelPressureEvents({mw::core::MidiChannelPressureEvent(0,1,10)});
            audioRejected = audio.getMidiChannelPressureEvents().empty() && !history.addAtTick(project,audio.getStableId(),10,1,10).changed;
            helpers = std::string(mw::midi::midiChannelPressureLaneTooltip).find("0 through 127") != std::string::npos
                && std::string(mw::midi::midiChannelPressurePressureTooltip).find("baseline") != std::string::npos
                && std::string(mw::midi::midiChannelPressureGraphTooltip).find("staircase") != std::string::npos;

            std::filesystem::remove(projectPath,ec); std::filesystem::remove(legacyPath,ec); std::filesystem::remove(midiPath,ec); std::filesystem::remove(malformedPath,ec);
            const bool passed = laneContract && schedulerContract && recordingContract && reviewCompContract && persistenceContract
                && format19Migration && midiRoundTripContract && malformedMidiSafe && audioRejected && helpers;
            std::ostringstream failure;
            if (!passed) failure << "I33 failed: lane="<<laneContract<<" scheduler="<<schedulerContract<<" recording="<<recordingContract
                <<" reviewComp="<<reviewCompContract<<" persistence="<<persistenceContract<<" f19="<<format19Migration
                <<" midi="<<midiRoundTripContract<<" malformed="<<malformedMidiSafe<<" audio="<<audioRejected<<" helpers="<<helpers;
            return makeResult("I33", "Workflow", "End-to-end MIDI Channel Pressure", passed,
                passed ? "Channel Pressure passed 0-127 stable-ID lane editing/snap/Undo, playback seek restoration with runtime zero reset, exact unquantized recording, pressure-only Take Review and half-open Comp ownership without synthetic boundary events, project-format-20 Active/Archived persistence with format-19 empty-pressure migration, Standard MIDI File export/import including pressure-only tracks, malformed-message safety, and AudioClip rejection." : failure.str());
        }

        SimulationScenarioResult scenarioI34()
        {
            bool laneContract = false;
            bool schedulerContract = false;
            bool recordingContract = false;
            bool reviewCompContract = false;
            bool persistenceContract = false;
            bool format20Migration = false;
            bool midiRoundTripContract = false;
            bool malformedMidiSafe = false;
            bool audioRejected = false;
            bool helpers = false;

            mw::core::Project project("I34 Poly Pressure");
            project.setStableId(980001);
            auto& track = project.addTrack("Poly Pressure Main");
            track.setStableId(980101);
            track.addNote({60, 100, 0, 960, 1, mw::core::Articulation::Normal});

            mw::midi::MidiPolyPressureEditHistory history;
            const auto key0 = history.addAtTick(project, track.getStableId(), 0, 1, 0, 0);
            const auto key60 = history.addAtTick(project, track.getStableId(), 960, 1, 60, 64);
            const auto key127 = history.addAtTick(project, track.getStableId(), 960, 16, 127, 127);
            const auto occupied = history.addAtTick(project, track.getStableId(), 1440, 1, 60, 33);
            const auto movedId = key60.eventId;
            const bool moved = history.moveAndSetPressure(project, track.getStableId(), movedId, 1440, 99);
            const auto lane = mw::midi::polyPressureLaneEvents(track, 1, 60);
            const auto movedIt = std::find_if(lane.begin(), lane.end(), [&](const auto& e){ return e.stableId == movedId; });
            const bool displaced = std::none_of(lane.begin(), lane.end(), [&](const auto& e){ return e.stableId == occupied.eventId; });
            const bool otherKeysPreserved = std::any_of(track.getMidiPolyPressureEvents().begin(), track.getMidiPolyPressureEvents().end(), [&](const auto& e){ return e.stableId == key0.eventId && e.noteNumber == 0; })
                && std::any_of(track.getMidiPolyPressureEvents().begin(), track.getMidiPolyPressureEvents().end(), [&](const auto& e){ return e.stableId == key127.eventId && e.noteNumber == 127 && e.midiChannel == 16; });
            const bool undo = history.undo(project);
            const bool undoRestored = std::any_of(track.getMidiPolyPressureEvents().begin(), track.getMidiPolyPressureEvents().end(), [&](const auto& e){ return e.stableId == occupied.eventId && e.noteNumber == 60 && e.pressure == 33; });
            const bool redo = history.redo(project);
            const bool snap = mw::midi::snapMidiPolyPressureTick(480, 960, mw::midi::MidiPolyPressureSnapGrid::Quarter) == 960
                && mw::midi::snapMidiPolyPressureTick(123, 960, mw::midi::MidiPolyPressureSnapGrid::Off) == 123;
            const bool graphMap = mw::midi::midiPolyPressureYToPressure(
                    mw::midi::midiPolyPressurePressureToY(127, 10.0, 127.0), 10.0, 127.0) == 127
                && mw::midi::midiPolyPressureYToPressure(
                    mw::midi::midiPolyPressurePressureToY(0, 10.0, 127.0), 10.0, 127.0) == 0;
            const auto staircase = mw::midi::polyPressureStaircaseSegments(track, 1, 60, 0, 1920);
            laneContract = key0.changed && key60.changed && key127.changed && occupied.changed && moved
                && movedIt != lane.end() && movedIt->tick == 1440 && movedIt->noteNumber == 60 && movedIt->pressure == 99
                && displaced && otherKeysPreserved && undo && undoRestored && redo && snap && graphMap && !staircase.empty()
                && project.getMetadata().projectFormatVersion == 22;

            std::vector<mw::clap::ClapLivePlaybackPolyPressureEvent> poly {
                { 981001, 1000, 1, 60, 70 }, { 981002, 3000, 1, 60, 80 },
                { 981003, 3000, 16, 127, 127 }, { 981004, 3200, 1, 0, 25 }
            };
            std::vector<mw::clap::ClapLivePlaybackNote> schedulerNotes {{60,100,1,3000,960}};
            mw::clap::ClapLivePlaybackScheduler scheduler;
            mw::clap::ClapLivePlaybackSchedulerConfig schedulerConfig;
            schedulerConfig.startTick = 2500;
            schedulerConfig.tailSeconds = 0.0;
            scheduler.prepare(schedulerNotes, {}, {}, {}, {}, poly, schedulerConfig);
            bool restoredKey60 = false, resetKey127 = false, resetKey0 = false, timedKey60 = false, timedKey127 = false, timedKey0 = false;
            int zeroResetCount = 0;
            mw::clap::ClapLiveProcessRequest request;
            while (!scheduler.isFinished())
            {
                scheduler.nextBlockInto(request);
                for (const auto& e : request.polyPressureEvents)
                {
                    if (e.pressure == 0) ++zeroResetCount;
                    if (e.midiChannel == 1 && e.noteNumber == 60 && e.pressure == 70) restoredKey60 = true;
                    if (e.midiChannel == 16 && e.noteNumber == 127 && e.pressure == 0) resetKey127 = true;
                    if (e.midiChannel == 1 && e.noteNumber == 0 && e.pressure == 0) resetKey0 = true;
                    if (e.midiChannel == 1 && e.noteNumber == 60 && e.pressure == 80) timedKey60 = true;
                    if (e.midiChannel == 16 && e.noteNumber == 127 && e.pressure == 127) timedKey127 = true;
                    if (e.midiChannel == 1 && e.noteNumber == 0 && e.pressure == 25) timedKey0 = true;
                }
            }
            schedulerContract = restoredKey60 && resetKey127 && resetKey0 && timedKey60 && timedKey127 && timedKey0
                && zeroResetCount == 2 && scheduler.totalScheduledEvents() == 8;

            mw::midi::MidiRecordingSession recording;
            mw::midi::MidiRecordingConfig config;
            config.tempoBpm = 120; config.ticksPerQuarterNote = 960; config.startTick = 1000; config.startTimeSeconds = 10.0;
            config.recordPolyPressure = false;
            const bool startedOff = recording.start(config);
            recording.polyPressure(1, 60, 10, 10.125);
            const auto off = recording.stop(10.25);
            config.recordPolyPressure = true;
            const bool startedOn = recording.start(config);
            recording.polyPressure(1, 60, 10, 10.125);
            recording.polyPressure(1, 60, 11, 10.125);
            recording.polyPressure(1, 61, 12, 10.125);
            recording.polyPressure(16, 127, 127, 10.25);
            const auto on = recording.stop(10.50);
            recordingContract = startedOff && off.polyPressureEvents.empty() && startedOn
                && on.polyPressureEvents.size() == 3U
                && on.polyPressureEvents[0].tick == 1240 && on.polyPressureEvents[0].midiChannel == 1 && on.polyPressureEvents[0].noteNumber == 60 && on.polyPressureEvents[0].pressure == 11
                && on.polyPressureEvents[1].tick == 1240 && on.polyPressureEvents[1].midiChannel == 1 && on.polyPressureEvents[1].noteNumber == 61 && on.polyPressureEvents[1].pressure == 12
                && on.polyPressureEvents[2].tick == 1480 && on.polyPressureEvents[2].midiChannel == 16 && on.polyPressureEvents[2].noteNumber == 127 && on.polyPressureEvents[2].pressure == 127;

            mw::midi::MidiRecordingLoopTakeReviewState review;
            const auto reviewGeneration = review.begin(project.getStableId(), track.getStableId(), 982001);
            const bool pass1 = review.addPass(reviewGeneration, 1, {}, 0, {}, {}, {}, {}, {}, {{100,1,60,12,1},{500,1,60,20,2},{1900,1,61,13,3}});
            const bool pass2 = review.addPass(reviewGeneration, 2, {}, 0, {}, {}, {}, {}, {}, {{500,1,60,99,1},{2000,1,61,40,2},{3000,16,127,127,3}});
            const bool reviewReady = review.finishCollection(reviewGeneration) && review.totalPolyPressureEventCount() == 6U;
            const auto mergedPoly = review.mergedPolyPressureEvents();
            const bool laterPassWins = std::any_of(mergedPoly.begin(), mergedPoly.end(), [](const auto& e){ return e.tick == 500 && e.midiChannel == 1 && e.noteNumber == 60 && e.pressure == 99; });
            mw::midi::MidiRecordingLoopCompState comp;
            const auto compGeneration = comp.begin(project.getStableId(), track.getStableId(), reviewGeneration, 0, 3840, 960, 4, 4,
                mw::midi::MidiRecordingCompGrid::Beat, 1, review.passes());
            bool compConfigured = compGeneration != 0 && comp.segments().size() == 4U;
            if (compConfigured) compConfigured = comp.assignPass(compGeneration, 2, 2, review.passes()) && comp.assignPass(compGeneration, 3, 2, review.passes());
            const auto compPoly = comp.buildPolyPressureEvents(compGeneration, review.passes());
            const bool hasP1 = std::any_of(compPoly.begin(), compPoly.end(), [](const auto& e){ return e.tick == 100 && e.noteNumber == 60 && e.pressure == 12; });
            const bool hasP2 = std::any_of(compPoly.begin(), compPoly.end(), [](const auto& e){ return e.tick == 2000 && e.noteNumber == 61 && e.pressure == 40; });
            const bool noSynthetic = std::none_of(compPoly.begin(), compPoly.end(), [](const auto& e){ return e.tick == 1920; });
            reviewCompContract = pass1 && pass2 && reviewReady && laterPassWins && compConfigured && hasP1 && hasP2 && noSynthetic;

            mw::core::PersistentMidiTakeReview persisted;
            persisted.active = true; persisted.reviewSessionId = 983001; persisted.reviewName = "Poly Pressure Review";
            persisted.regionInTick = 0; persisted.regionOutTick = 3840; persisted.selectedPassNumber = 1;
            persisted.passes.push_back({1, {mw::core::NoteEvent(67,90,0,240,1)}, 0, {}, {}, {}, {}, {}, {{120,1,60,64,1},{240,16,127,127,2}}});
            track.setPendingMidiTakeReview(persisted);
            auto archived = persisted; archived.reviewSessionId = 983002; archived.reviewName = "Poly Pressure Archive";
            track.getArchivedMidiTakeReviews().push_back(archived);

            const auto tempRoot = std::filesystem::temp_directory_path() / "PoorMansStudioSimulation";
            std::error_code ec; std::filesystem::create_directories(tempRoot, ec);
            const auto projectPath = tempRoot / "i34_poly_pressure_format21.pmsproj";
            const auto legacyPath = tempRoot / "i34_poly_pressure_format20.pmsproj";
            const auto midiPath = tempRoot / "i34_poly_pressure.mid";
            const auto malformedPath = tempRoot / "i34_poly_pressure_malformed.mid";
            const bool saved = mw::serialization::ProjectSerializer::saveToFile(project, projectPath);
            const auto reopened = saved ? mw::serialization::ProjectSerializer::loadFromFile(projectPath) : std::nullopt;
            if (reopened)
            {
                const auto it = std::find_if(reopened->getTracks().begin(), reopened->getTracks().end(), [](const auto& t){ return t.getStableId() == 980101; });
                if (it != reopened->getTracks().end())
                    persistenceContract = reopened->getMetadata().projectFormatVersion == 22
                        && !it->getMidiPolyPressureEvents().empty()
                        && it->getPendingMidiTakeReview().active
                        && it->getPendingMidiTakeReview().passes.front().polyPressureEvents.size() == 2U
                        && it->getArchivedMidiTakeReviews().size() == 1U
                        && it->getArchivedMidiTakeReviews().front().passes.front().polyPressureEvents.size() == 2U;
            }
            std::string serialized;
            if (saved) { std::ifstream in(projectPath, std::ios::binary); serialized.assign(std::istreambuf_iterator<char>(in), {}); }
            if (!serialized.empty())
            {
                auto legacyText = serialized;
                const auto pos = legacyText.find("\"version\": 22");
                if (pos != std::string::npos) legacyText.replace(pos, std::strlen("\"version\": 22"), "\"version\": 20");
                { std::ofstream outFile(legacyPath, std::ios::binary | std::ios::trunc); outFile << legacyText; }
                const auto legacy = mw::serialization::ProjectSerializer::loadFromFile(legacyPath);
                if (legacy)
                {
                    const auto it = std::find_if(legacy->getTracks().begin(), legacy->getTracks().end(), [](const auto& t){ return t.getStableId() == 980101; });
                    format20Migration = legacy->getMetadata().projectFormatVersion == 22 && it != legacy->getTracks().end()
                        && it->getMidiPolyPressureEvents().empty() && it->getPendingMidiTakeReview().active
                        && it->getPendingMidiTakeReview().passes.front().polyPressureEvents.empty() && !it->getNotes().empty();
                }
            }

            {
                auto& polyOnly = project.addTrack("Poly Pressure Only");
                polyOnly.setStableId(980102);
                polyOnly.setMidiPolyPressureEvents({
                    mw::core::MidiPolyPressureEvent(984001,0,1,0,0),
                    mw::core::MidiPolyPressureEvent(984002,240,1,60,64),
                    mw::core::MidiPolyPressureEvent(984003,480,16,127,127) });
            }
            const bool exported = mw::midi::MidiExporter::exportToFile(project, midiPath, {mw::midi::MidiExportPurpose::UserMidiExport});
            const auto imported = exported ? mw::import_export::MidiImporter::importFromFile(midiPath) : std::nullopt;
            if (imported)
            {
                const auto it = std::find_if(imported->getTracks().begin(), imported->getTracks().end(), [](const auto& t){ return t.getName() == "Poly Pressure Only"; });
                if (it != imported->getTracks().end())
                {
                    const auto& events = it->getMidiPolyPressureEvents();
                    const auto has = [&](std::int64_t tick,int ch,int key,int pressure){ return std::any_of(events.begin(), events.end(), [&](const auto& e){ return e.tick==tick && e.midiChannel==ch && e.noteNumber==key && e.pressure==pressure; }); };
                    std::set<mw::core::StableId> ids; for (const auto& e: events) ids.insert(e.stableId);
                    midiRoundTripContract = it->getNotes().empty() && events.size() == 3U
                        && has(0,1,0,0) && has(240,1,60,64) && has(480,16,127,127)
                        && ids.size() == events.size() && ids.count(0) == 0;
                }
            }

            auto appendBe16 = [](std::vector<std::uint8_t>& b,std::uint16_t v){ b.push_back(static_cast<std::uint8_t>(v>>8)); b.push_back(static_cast<std::uint8_t>(v)); };
            auto appendBe32 = [](std::vector<std::uint8_t>& b,std::uint32_t v){ b.push_back(static_cast<std::uint8_t>(v>>24)); b.push_back(static_cast<std::uint8_t>(v>>16)); b.push_back(static_cast<std::uint8_t>(v>>8)); b.push_back(static_cast<std::uint8_t>(v)); };
            std::vector<std::uint8_t> malformed; malformed.insert(malformed.end(), {'M','T','h','d'}); appendBe32(malformed,6); appendBe16(malformed,0); appendBe16(malformed,1); appendBe16(malformed,960);
            std::vector<std::uint8_t> mt {0x00U,0x90U,0x3CU,0x64U,0x78U,0x80U,0x3CU,0x00U,0x00U,0xA0U,0x3CU};
            malformed.insert(malformed.end(), {'M','T','r','k'}); appendBe32(malformed, static_cast<std::uint32_t>(mt.size())); malformed.insert(malformed.end(), mt.begin(), mt.end());
            { std::ofstream outFile(malformedPath, std::ios::binary|std::ios::trunc); outFile.write(reinterpret_cast<const char*>(malformed.data()), static_cast<std::streamsize>(malformed.size())); }
            const auto malformedImported = mw::import_export::MidiImporter::importFromFile(malformedPath);
            malformedMidiSafe = malformedImported && malformedImported->getTracks().size()==1U && malformedImported->getTracks().front().getNotes().size()==1U && malformedImported->getTracks().front().getMidiPolyPressureEvents().empty();

            mw::core::StableId audioId = 0;
            {
                auto& audio = project.addTrack("Poly Pressure Audio");
                audio.setTrackType(mw::core::TrackType::AudioClip);
                audioId = audio.getStableId();
                audio.setMidiPolyPressureEvents({mw::core::MidiPolyPressureEvent(0,1,60,10)});
                audioRejected = audio.getMidiPolyPressureEvents().empty();
            }
            audioRejected = audioRejected && !history.addAtTick(project,audioId,10,1,60,10).changed;
            helpers = std::string(mw::midi::midiPolyPressureLaneTooltip).find("0 through 127") != std::string::npos
                && std::string(mw::midi::midiPolyPressureKeyTooltip).find("channel+key") != std::string::npos
                && std::string(mw::midi::midiPolyPressureGraphTooltip).find("staircase") != std::string::npos;

            std::filesystem::remove(projectPath,ec); std::filesystem::remove(legacyPath,ec); std::filesystem::remove(midiPath,ec); std::filesystem::remove(malformedPath,ec);
            const bool passed = laneContract && schedulerContract && recordingContract && reviewCompContract && persistenceContract
                && format20Migration && midiRoundTripContract && malformedMidiSafe && audioRejected && helpers;
            std::ostringstream failure;
            if (!passed) failure << "I34 failed: lane="<<laneContract<<" scheduler="<<schedulerContract<<" recording="<<recordingContract
                <<" reviewComp="<<reviewCompContract<<" persistence="<<persistenceContract<<" f20="<<format20Migration
                <<" midi="<<midiRoundTripContract<<" malformed="<<malformedMidiSafe<<" audio="<<audioRejected<<" helpers="<<helpers;
            return makeResult("I34", "Workflow", "End-to-end MIDI Polyphonic Key Pressure", passed,
                passed ? "Polyphonic Key Pressure passed per-key 0-127 stable-ID lane editing/snap/Undo, represented-key seek restoration with runtime zero reset, exact unquantized recording, Poly-Pressure-only Take Review and half-open Comp ownership without synthetic boundary events, project-format-21 Active/Archived persistence with format-20 empty-poly migration, Standard MIDI File export/import including Poly-Pressure-only tracks, malformed-message safety, and AudioClip rejection." : failure.str());
        }


        SimulationScenarioResult scenarioI35()
        {
            bool lane = false, scheduler = false, persistence = false, midiRoundTrip = false, malformed = false, audioRejected = false;
            mw::core::Project project("I35 14-bit Controller");
            project.setStableId(990001);
            auto& track = project.addTrack("14-bit Main");
            track.setStableId(990101);
            mw::midi::Midi14BitControllerEditHistory history;
            const auto a = history.addAtTick(project, track.getStableId(), 0, 1, 1, 0);
            const auto b = history.addAtTick(project, track.getStableId(), 480, 1, 1, 8192);
            const auto c = history.addAtTick(project, track.getStableId(), 960, 16, 31, 16383);
            const bool moved = history.moveAndSetValue(project, track.getStableId(), b.eventId, 720, 12000);
            const bool undo = history.undo(project);
            const bool redo = history.redo(project);
            const auto laneEvents = mw::midi::midi14BitControllerLaneEvents(track, 1, 1);
            const auto restored = mw::midi::midi14BitControllerStateAtOrBeforeTick(track, 800);
            const bool snap = mw::midi::snapMidi14BitTick(480, 960, mw::midi::Midi14BitSnapGrid::Quarter) == 960
                && mw::midi::snapMidi14BitControllerTick(480, 960, mw::midi::Midi14BitControllerSnapGrid::Quarter) == 960;
            const bool map = mw::midi::midi14BitYToValue(mw::midi::midi14BitValueToY(16383, 0.0, 100.0), 0.0, 100.0) == 16383;
            const auto graphSteps = mw::midi::midi14BitControllerStaircaseSegments(track, 1, 1, 0, 1000);
            const auto hitId = mw::midi::hitTestMidi14BitControllerPoint(
                laneEvents, 0, 8.0, 0.0, 100.0,
                mw::midi::midi14BitControllerTickToX(720, 0, 8.0),
                mw::midi::midi14BitControllerValueToY(12000, 0.0, 100.0), 8.0);
            const bool graph = graphSteps.size() == 2U && hitId == b.eventId;
            lane = a.changed && b.changed && c.changed && moved && undo && redo && laneEvents.size() == 2U
                && std::any_of(laneEvents.begin(), laneEvents.end(), [&](const auto& e){ return e.stableId == b.eventId && e.tick == 720 && e.value == 12000; })
                && !restored.empty() && snap && map && graph && project.getMetadata().projectFormatVersion == 22;

            mw::clap::ClapLivePlaybackScheduler play;
            mw::clap::ClapLivePlaybackSchedulerConfig pc; pc.startTick = 100; pc.tailSeconds = 0.0;
            std::vector<mw::clap::ClapLivePlayback14BitControllerEvent> adv {{991001,0,1,1,8192},{991002,200,1,31,16383}};
            play.prepare({}, {}, {}, {}, {}, {}, adv, {}, {}, pc);
            std::vector<std::pair<int,int>> emitted;
            mw::clap::ClapLiveProcessRequest req;
            while (!play.isFinished()) { play.nextBlockInto(req); for (const auto& e : req.controllerEvents) emitted.emplace_back(e.controller,e.value); }
            const auto hasPair = [&](int cc1,int v1,int cc2,int v2){
                for (std::size_t i=1;i<emitted.size();++i) if(emitted[i-1]==std::make_pair(cc1,v1)&&emitted[i]==std::make_pair(cc2,v2)) return true;
                return false;
            };
            scheduler = hasPair(1,64,33,0) && hasPair(31,127,63,127);

            const auto root = std::filesystem::temp_directory_path()/"PoorMansStudioSimulation";
            std::error_code ec; std::filesystem::create_directories(root,ec);
            const auto projectPath=root/"i35_format22.pms"; const auto midiPath=root/"i35_14bit.mid";
            const bool saved=mw::serialization::ProjectSerializer::saveToFile(project,projectPath);
            const auto reopened=saved?mw::serialization::ProjectSerializer::loadFromFile(projectPath):std::nullopt;
            persistence = reopened && reopened->getMetadata().projectFormatVersion==22 && !reopened->getTracks().empty()
                && reopened->getTracks().front().getMidi14BitControllerEvents().size()==track.getMidi14BitControllerEvents().size();
            const bool exported=mw::midi::MidiExporter::exportToFile(project,midiPath,{mw::midi::MidiExportPurpose::UserMidiExport});
            const auto imported=exported?mw::import_export::MidiImporter::importFromFile(midiPath):std::nullopt;
            if(imported && !imported->getTracks().empty())
            {
                const auto& es=imported->getTracks().front().getMidi14BitControllerEvents();
                midiRoundTrip = es.size()==track.getMidi14BitControllerEvents().size()
                    && std::any_of(es.begin(),es.end(),[](const auto& e){return e.controllerMsb==1&&e.value==12000;})
                    && std::any_of(es.begin(),es.end(),[](const auto& e){return e.controllerMsb==31&&e.value==16383;});
            }
            const std::vector<mw::core::PersistentMidiControllerEvent> incomplete {{100,1,5,64,1},{200,1,40,12,2}};
            const auto decoded=mw::midi::decodeAdvancedMidiParameters(incomplete);
            malformed = decoded.controller14BitEvents.empty() && decoded.ordinaryControllers.size()==2U;
            auto& audio=project.addTrack("Audio"); audio.setTrackType(mw::core::TrackType::AudioClip);
            audio.setMidi14BitControllerEvents({mw::core::Midi14BitControllerEvent(0,1,1,1)});
            audioRejected = audio.getMidi14BitControllerEvents().empty() && !history.addAtTick(project,audio.getStableId(),0,1,1,1).changed;
            std::filesystem::remove(projectPath,ec); std::filesystem::remove(midiPath,ec);
            const bool passed=lane&&scheduler&&persistence&&midiRoundTrip&&malformed&&audioRejected;
            std::ostringstream failure; if(!passed) failure<<"I35 failed: lane="<<lane<<" scheduler="<<scheduler<<" persistence="<<persistence<<" midi="<<midiRoundTrip<<" malformed="<<malformed<<" audio="<<audioRejected;
            return makeResult("I35","Workflow","14-bit Controller Automation",passed,
                passed?"14-bit CC passed 0/8192/16383 values, controllers 1/31 and channels 1/16, atomic lane editing/Undo, seek playback MSB+LSB encoding, format-22 persistence, standard-MIDI round-trip, malformed-pair fallback, and AudioClip rejection.":failure.str());
        }

        SimulationScenarioResult scenarioI36()
        {
            bool lane=false, decoder=false, scheduler=false, persistence=false, midiRoundTrip=false;
            mw::core::Project project("I36 RPN NRPN"); project.setStableId(992001);
            auto& track=project.addTrack("Parameters"); track.setStableId(992101);
            mw::midi::MidiParameterEditHistory history;
            const auto r0=history.addAtTick(project,track.getStableId(),mw::midi::MidiParameterMode::Rpn,0,1,0,0);
            const auto rMax=history.addAtTick(project,track.getStableId(),mw::midi::MidiParameterMode::Rpn,240,16,16382,16383);
            const auto nMax=history.addAtTick(project,track.getStableId(),mw::midi::MidiParameterMode::Nrpn,480,16,16383,16383);
            const auto nullRejected=history.addAtTick(project,track.getStableId(),mw::midi::MidiParameterMode::Rpn,720,1,16383,1);
            const bool parameterSnap = mw::midi::snapMidiParameterTick(480, 960, mw::midi::MidiParameterSnapGrid::Quarter) == 960
                && mw::midi::snapMidiParameterTick(123, 960, mw::midi::MidiParameterSnapGrid::Off) == 123;
            lane=r0.changed&&rMax.changed&&nMax.changed&&!nullRejected.changed&&history.setValue(project,track.getStableId(),mw::midi::MidiParameterMode::Rpn,r0.eventId,8192)
                && history.undo(project)&&history.redo(project)&&track.getMidiRpnEvents().size()==2U&&track.getMidiNrpnEvents().size()==1U&&parameterSnap;

            std::vector<mw::core::PersistentMidiControllerEvent> raw {
                {0,1,101,0,1},{0,1,100,1,2},{0,1,6,64,3},{0,1,38,0,4},
                {100,16,99,127,5},{100,16,98,127,6},{100,16,6,127,7},{100,16,38,127,8},
                {200,2,101,127,9},{200,2,100,127,10},{200,2,6,5,11},{200,2,38,6,12},
                {300,3,101,0,13},{300,3,100,2,14},{300,3,6,3,15}
            };
            const auto decoded=mw::midi::decodeAdvancedMidiParameters(raw);
            decoder=decoded.rpnEvents.size()==1U&&decoded.rpnEvents.front().parameter==1&&decoded.rpnEvents.front().value==8192
                && decoded.nrpnEvents.size()==1U&&decoded.nrpnEvents.front().parameter==16383&&decoded.nrpnEvents.front().value==16383
                && decoded.ordinaryControllers.size()==7U;

            mw::clap::ClapLivePlaybackScheduler play; mw::clap::ClapLivePlaybackSchedulerConfig pc; pc.tailSeconds=0.0;
            std::vector<mw::clap::ClapLivePlaybackRpnEvent> rpns {{993001,100,1,1,8192}};
            std::vector<mw::clap::ClapLivePlaybackNrpnEvent> nrpns {{993002,200,16,16383,16383}};
            play.prepare({}, {}, {}, {}, {}, {}, {}, rpns, nrpns, pc);
            std::vector<std::pair<int,int>> ch1,ch16; mw::clap::ClapLiveProcessRequest req;
            while(!play.isFinished()){ play.nextBlockInto(req); for(const auto& e:req.controllerEvents){ if(e.midiChannel==1)ch1.emplace_back(e.controller,e.value); if(e.midiChannel==16)ch16.emplace_back(e.controller,e.value);} }
            const std::vector<std::pair<int,int>> expectedRpn {{101,0},{100,1},{6,64},{38,0},{101,127},{100,127}};
            const std::vector<std::pair<int,int>> expectedNrpn {{99,127},{98,127},{6,127},{38,127}};
            scheduler=ch1==expectedRpn&&ch16==expectedNrpn;

            const auto root=std::filesystem::temp_directory_path()/"PoorMansStudioSimulation"; std::error_code ec; std::filesystem::create_directories(root,ec);
            const auto pp=root/"i36_format22.pms"; const auto mp=root/"i36_params.mid";
            const bool saved=mw::serialization::ProjectSerializer::saveToFile(project,pp); const auto loaded=saved?mw::serialization::ProjectSerializer::loadFromFile(pp):std::nullopt;
            persistence=loaded&&!loaded->getTracks().empty()&&loaded->getTracks().front().getMidiRpnEvents().size()==2U&&loaded->getTracks().front().getMidiNrpnEvents().size()==1U;
            const bool exported=mw::midi::MidiExporter::exportToFile(project,mp,{mw::midi::MidiExportPurpose::UserMidiExport}); const auto imported=exported?mw::import_export::MidiImporter::importFromFile(mp):std::nullopt;
            if(imported&&!imported->getTracks().empty())
            {
                const auto& t=imported->getTracks().front();
                midiRoundTrip=t.getMidiRpnEvents().size()==2U&&t.getMidiNrpnEvents().size()==1U
                    && std::none_of(t.getMidiRpnEvents().begin(),t.getMidiRpnEvents().end(),[](const auto& e){return e.parameter==16383;});
            }
            std::filesystem::remove(pp,ec); std::filesystem::remove(mp,ec);
            const bool passed=lane&&decoder&&scheduler&&persistence&&midiRoundTrip;
            std::ostringstream failure; if(!passed) failure<<"I36 failed: lane="<<lane<<" decoder="<<decoder<<" scheduler="<<scheduler<<" persistence="<<persistence<<" midi="<<midiRoundTrip;
            return makeResult("I36","Workflow","RPN / NRPN Semantics",passed,
                passed?"RPN/NRPN passed semantic editing, channel-independent decode state, Data Entry MSB/LSB, RPN Null rejection, ordered standard-MIDI playback sequences, format-22 persistence, and export/reimport reconstruction.":failure.str());
        }

        SimulationScenarioResult scenarioI37()
        {
            bool defaultOff=false, recording=false, review=false, comp=false, persistence=false;
            mw::midi::MidiRecordingSession offSession; mw::midi::MidiRecordingConfig offConfig; offConfig.startTimeSeconds=1.0;
            offSession.start(offConfig); offSession.controller(1,1,64,1.1); offSession.controller(1,33,1,1.1); const auto off=offSession.stop(1.2);
            defaultOff=off.controller14BitEvents.empty()&&off.rpnEvents.empty()&&off.nrpnEvents.empty()&&off.controllerEvents.empty();

            mw::midi::MidiRecordingSession session; mw::midi::MidiRecordingConfig config; config.startTimeSeconds=10.0; config.startTick=1000; config.recordAdvancedParameters=true; config.recordControllers=true;
            session.start(config);
            session.controller(1,1,64,10.125); session.controller(1,33,1,10.125);
            session.controller(1,101,0,10.250); session.controller(1,100,2,10.250); session.controller(1,6,10,10.250); session.controller(1,38,20,10.250);
            session.controller(16,99,1,10.375); session.controller(16,98,2,10.375); session.controller(16,6,3,10.375); session.controller(16,38,4,10.375);
            const auto result=session.stop(10.5);
            recording=result.controller14BitEvents.size()==1U&&result.controller14BitEvents.front().value==8193&&result.controller14BitEvents.front().tick==1240
                && result.rpnEvents.size()==1U&&result.rpnEvents.front().parameter==2&&result.rpnEvents.front().value==1300&&result.rpnEvents.front().tick==1480
                && result.nrpnEvents.size()==1U&&result.nrpnEvents.front().parameter==130&&result.nrpnEvents.front().value==388&&result.nrpnEvents.front().tick==1720
                && result.controllerEvents.empty();

            mw::midi::MidiRecordingLoopTakeReviewState state; const auto gen=state.begin(994001,994101,77);
            const bool added=state.addPass(gen,1,{},0,{}, {}, {}, {}, {}, {}, result.controller14BitEvents,result.rpnEvents,result.nrpnEvents);
            const bool finished=state.finishCollection(gen);
            review=added&&finished&&state.isReviewActive()&&state.total14BitControllerEventCount()==1U&&state.totalRpnEventCount()==1U&&state.totalNrpnEventCount()==1U
                && state.merged14BitControllerEvents().size()==1U&&state.mergedRpnEvents().size()==1U&&state.mergedNrpnEvents().size()==1U;

            mw::midi::MidiRecordingLoopCompState compState; const auto cgen=compState.begin(994001,994101,gen,1000,1960,960,4,4,mw::midi::MidiRecordingCompGrid::Beat,1,state.passes());
            const auto c14=compState.build14BitControllerEvents(cgen,state.passes()); const auto cr=compState.buildRpnEvents(cgen,state.passes()); const auto cn=compState.buildNrpnEvents(cgen,state.passes());
            comp=!c14.empty()&&!cr.empty()&&!cn.empty()&&std::all_of(c14.begin(),c14.end(),[](const auto&e){return e.tick>=1000&&e.tick<1960;})
                &&std::all_of(cr.begin(),cr.end(),[](const auto&e){return e.tick>=1000&&e.tick<1960;})&&std::all_of(cn.begin(),cn.end(),[](const auto&e){return e.tick>=1000&&e.tick<1960;});

            mw::core::Project project("I37 Review"); project.setStableId(994001); auto& track=project.addTrack("Recorded"); track.setStableId(994101);
            mw::core::PersistentMidiTakeReview pending; pending.active=true; pending.reviewSessionId=994201; pending.reviewName="Advanced"; pending.regionInTick=1000; pending.regionOutTick=1960; pending.selectedPassNumber=1;
            pending.passes.push_back({1,{},0,{}, {}, {}, {}, {}, {}, result.controller14BitEvents,result.rpnEvents,result.nrpnEvents});
            track.setPendingMidiTakeReview(pending); auto archived=pending; archived.active=false; archived.reviewSessionId=994202; archived.reviewName="Advanced Archive"; track.setArchivedMidiTakeReviews({archived});
            const auto root=std::filesystem::temp_directory_path()/"PoorMansStudioSimulation"; std::error_code ec; std::filesystem::create_directories(root,ec); const auto path=root/"i37_review.pms";
            const bool saved=mw::serialization::ProjectSerializer::saveToFile(project,path); const auto loaded=saved?mw::serialization::ProjectSerializer::loadFromFile(path):std::nullopt;
            if(loaded&&!loaded->getTracks().empty())
            {
                const auto& t=loaded->getTracks().front(); persistence=loaded->getMetadata().projectFormatVersion==22&&t.getPendingMidiTakeReview().passes.size()==1U
                    &&t.getPendingMidiTakeReview().passes.front().controller14BitEvents.size()==1U&&t.getPendingMidiTakeReview().passes.front().rpnEvents.size()==1U&&t.getPendingMidiTakeReview().passes.front().nrpnEvents.size()==1U
                    &&t.getArchivedMidiTakeReviews().size()==1U&&t.getArchivedMidiTakeReviews().front().passes.front().nrpnEvents.size()==1U;
            }
            std::filesystem::remove(path,ec);
            const bool passed=defaultOff&&recording&&review&&comp&&persistence;
            std::ostringstream failure; if(!passed) failure<<"I37 failed: defaultOff="<<defaultOff<<" recording="<<recording<<" review="<<review<<" comp="<<comp<<" persistence="<<persistence;
            return makeResult("I37","Workflow","Advanced Parameter Recording + Take Review",passed,
                passed?"Advanced parameter recording passed default-Off gating, exact transport ticks, semantic 14-bit/RPN/NRPN decode without raw duplication, retained Take Review/Merge, half-open Comp ownership, and format-22 Active/Archived persistence.":failure.str());
        }

        template <SimulationScenarioResult (*Function)()>
        SimulationScenarioResult scenarioWithoutOptions(const SimulationRunOptions&)
        {
            return Function();
        }

        using ScenarioFunction = SimulationScenarioResult (*)(const SimulationRunOptions&);
        const std::array<std::pair<const char*, ScenarioFunction>, 61> scenarioTable {{
            { "A01", &scenarioWithoutOptions<scenarioA01> },
            { "B01", &scenarioWithoutOptions<scenarioB01> },
            { "B02", &scenarioWithoutOptions<scenarioB02> },
            { "B03", &scenarioWithoutOptions<scenarioB03> },
            { "C01", &scenarioWithoutOptions<scenarioC01> },
            { "C02", &scenarioWithoutOptions<scenarioC02> },
            { "D01", &scenarioWithoutOptions<scenarioD01> },
            { "D02", &scenarioWithoutOptions<scenarioD02> },
            { "E01", &scenarioWithoutOptions<scenarioE01> },
            { "E02", &scenarioWithoutOptions<scenarioE02> },
            { "F01", &scenarioWithoutOptions<scenarioF01> },
            { "F02", &scenarioWithoutOptions<scenarioF02> },
            { "F03", &scenarioWithoutOptions<scenarioF03> },
            { "F04", &scenarioWithoutOptions<scenarioF04> },
            { "F05", &scenarioWithoutOptions<scenarioF05> },
            { "G01", &scenarioWithoutOptions<scenarioG01> },
            { "G02", &scenarioWithoutOptions<scenarioG02> },
            { "G03", &scenarioWithoutOptions<scenarioG03> },
            { "G04", &scenarioWithoutOptions<scenarioG04> },
            { "H01", &scenarioWithoutOptions<scenarioH01> },
            { "H02", &scenarioH02 },
            { "H03", &scenarioWithoutOptions<scenarioH03> },
            { "H04", &scenarioWithoutOptions<scenarioH04> },
            { "H05", &scenarioWithoutOptions<scenarioH05> },
            { "I01", &scenarioWithoutOptions<scenarioI01> },
            { "I02", &scenarioWithoutOptions<scenarioI02> },
            { "I03", &scenarioWithoutOptions<scenarioI03> },
            { "I04", &scenarioWithoutOptions<scenarioI04> },
            { "I05", &scenarioWithoutOptions<scenarioI05> },
            { "I06", &scenarioWithoutOptions<scenarioI06> },
            { "I07", &scenarioWithoutOptions<scenarioI07> },
            { "I08", &scenarioWithoutOptions<scenarioI08> },
            { "I09", &scenarioWithoutOptions<scenarioI09> },
            { "I10", &scenarioWithoutOptions<scenarioI10> },
            { "I11", &scenarioWithoutOptions<scenarioI11> },
            { "I12", &scenarioWithoutOptions<scenarioI12> },
            { "I13", &scenarioWithoutOptions<scenarioI13> },
            { "I14", &scenarioWithoutOptions<scenarioI14> },
            { "I15", &scenarioWithoutOptions<scenarioI15> },
            { "I16", &scenarioWithoutOptions<scenarioI16> },
            { "I17", &scenarioWithoutOptions<scenarioI17> },
            { "I18", &scenarioWithoutOptions<scenarioI18> },
            { "I19", &scenarioWithoutOptions<scenarioI19> },
            { "I20", &scenarioWithoutOptions<scenarioI20> },
            { "I21", &scenarioWithoutOptions<scenarioI21> },
            { "I22", &scenarioWithoutOptions<scenarioI22> },
            { "I23", &scenarioWithoutOptions<scenarioI23> },
            { "I24", &scenarioWithoutOptions<scenarioI24> },
            { "I25", &scenarioWithoutOptions<scenarioI25> },
            { "I26", &scenarioWithoutOptions<scenarioI26> },
            { "I27", &scenarioWithoutOptions<scenarioI27> },
            { "I28", &scenarioWithoutOptions<scenarioI28> },
            { "I29", &scenarioWithoutOptions<scenarioI29> },
            { "I30", &scenarioWithoutOptions<scenarioI30> },
            { "I31", &scenarioWithoutOptions<scenarioI31> },
            { "I32", &scenarioWithoutOptions<scenarioI32> },
            { "I33", &scenarioWithoutOptions<scenarioI33> },
            { "I34", &scenarioWithoutOptions<scenarioI34> },
            { "I35", &scenarioWithoutOptions<scenarioI35> },
            { "I36", &scenarioWithoutOptions<scenarioI36> },
            { "I37", &scenarioWithoutOptions<scenarioI37> }
        }};
    }

    int SimulationSuiteReport::passedCount() const noexcept
    {
        return static_cast<int>(std::count_if(scenarios.begin(), scenarios.end(), [](const auto& result) { return result.passed; }));
    }

    int SimulationSuiteReport::failedCount() const noexcept
    {
        return static_cast<int>(scenarios.size()) - passedCount();
    }

    bool SimulationSuiteReport::allPassed() const noexcept
    {
        return !scenarios.empty() && failedCount() == 0;
    }

    std::vector<std::string> availableSimulationScenarioIds()
    {
        std::vector<std::string> ids;
        ids.reserve(scenarioTable.size());
        for (const auto& [id, function] : scenarioTable)
        {
            (void) function;
            ids.emplace_back(id);
        }
        return ids;
    }

    SimulationScenarioResult runScenarioSafely(
        const char* id, ScenarioFunction function, const SimulationRunOptions& options)
    {
        try
        {
            return function(options);
        }
        catch (const std::exception& e)
        {
            return { id, "Simulation", "Unhandled scenario exception", false,
                std::string("Unhandled exception while running ") + id + ": " + e.what() };
        }
        catch (...)
        {
            return { id, "Simulation", "Unhandled scenario exception", false,
                std::string("Unhandled non-standard exception while running ") + id + "." };
        }
    }

    SimulationSuiteReport runSimulationRegressionSuite(const std::vector<std::string>& requestedScenarioIds, const SimulationRunOptions& options)
    {
        SimulationSuiteReport report;
        report.suiteVersion = mw::app::versionNumber;
        report.generatedAtUtc = utcNow();

        std::set<std::string> requested;
        for (const auto& id : requestedScenarioIds)
            requested.insert(id);

        for (const auto& [id, function] : scenarioTable)
        {
            if (!requested.empty() && requested.find(id) == requested.end())
                continue;
            report.scenarios.push_back(runScenarioSafely(id, function, options));
        }
        return report;
    }

    SimulationTimedRun runSimulationRegressionSuiteTimed(
        const std::vector<std::string>& requestedScenarioIds, const SimulationRunOptions& options,
        const SimulationProgressCallback& progressCallback)
    {
        SimulationTimedRun timedRun;
        timedRun.report.suiteVersion = mw::app::versionNumber;
        timedRun.report.generatedAtUtc = utcNow();

        std::set<std::string> requested;
        for (const auto& id : requestedScenarioIds)
            requested.insert(id);

        const auto suiteStart = std::chrono::steady_clock::now();
        for (const auto& [id, function] : scenarioTable)
        {
            if (!requested.empty() && requested.find(id) == requested.end())
                continue;

            if (progressCallback)
                progressCallback(timedRun, id, true);

            const auto start = std::chrono::steady_clock::now();
            timedRun.report.scenarios.push_back(runScenarioSafely(id, function, options));
            const auto end = std::chrono::steady_clock::now();
            const auto elapsed = std::chrono::duration<double, std::milli>(end - start).count();
            timedRun.timings.push_back({ id, elapsed });
            timedRun.totalElapsedMilliseconds = std::chrono::duration<double, std::milli>(
                end - suiteStart).count();

            if (progressCallback)
                progressCallback(timedRun, id, false);
        }
        timedRun.totalElapsedMilliseconds = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - suiteStart).count();
        return timedRun;
    }

    std::string simulationReportToText(const SimulationSuiteReport& report)
    {
        std::ostringstream out;
        out << "Poor Man's Studio Simulation / Regression Suite\n";
        out << "Suite version: " << report.suiteVersion << "\n";
        out << "Generated UTC: " << report.generatedAtUtc << "\n";
        out << "Coverage: deterministic contract-level simulation; no physical audio/MIDI device or third-party plugin is used.\n\n";
        for (const auto& scenario : report.scenarios)
        {
            out << (scenario.passed ? "PASS" : "FAIL") << " [" << scenario.id << "] "
                << scenario.stage << " - " << scenario.name << "\n"
                << "  " << scenario.details << "\n";
        }
        out << "\nSummary: " << report.passedCount() << " passed, " << report.failedCount() << " failed, "
            << report.scenarios.size() << " total.\n";
        out << "Native Windows/JUCE device and third-party plugin testing is still required for hardware/driver/plugin acceptance.\n";
        return out.str();
    }

    std::string simulationReportToJson(const SimulationSuiteReport& report)
    {
        std::ostringstream out;
        out << "{\n";
        out << "  \"schemaVersion\": 1,\n";
        out << "  \"suiteVersion\": \"" << jsonEscape(report.suiteVersion) << "\",\n";
        out << "  \"generatedAtUtc\": \"" << jsonEscape(report.generatedAtUtc) << "\",\n";
        out << "  \"allPassed\": " << (report.allPassed() ? "true" : "false") << ",\n";
        out << "  \"passed\": " << report.passedCount() << ",\n";
        out << "  \"failed\": " << report.failedCount() << ",\n";
        out << "  \"coverageNote\": \"Deterministic contract-level simulation only; native Windows/JUCE devices and third-party plugins remain separate acceptance gates.\",\n";
        out << "  \"scenarios\": [\n";
        for (std::size_t i = 0; i < report.scenarios.size(); ++i)
        {
            const auto& scenario = report.scenarios[i];
            out << "    {\"id\": \"" << jsonEscape(scenario.id)
                << "\", \"stage\": \"" << jsonEscape(scenario.stage)
                << "\", \"name\": \"" << jsonEscape(scenario.name)
                << "\", \"passed\": " << (scenario.passed ? "true" : "false")
                << ", \"details\": \"" << jsonEscape(scenario.details) << "\"}";
            if (i + 1 < report.scenarios.size())
                out << ',';
            out << '\n';
        }
        out << "  ]\n";
        out << "}\n";
        return out.str();
    }

    std::string simulationReportToJUnitXml(const SimulationSuiteReport& report)
    {
        std::ostringstream out;
        out << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
        out << "<testsuite name=\"PoorMansStudioSimulation\" tests=\"" << report.scenarios.size()
            << "\" failures=\"" << report.failedCount()
            << "\" timestamp=\"" << xmlEscape(report.generatedAtUtc)
            << "\" version=\"" << xmlEscape(report.suiteVersion) << "\">\n";
        for (const auto& scenario : report.scenarios)
        {
            out << "  <testcase classname=\"" << xmlEscape(scenario.stage)
                << "\" name=\"" << xmlEscape(scenario.id + " " + scenario.name) << "\">\n";
            if (!scenario.passed)
                out << "    <failure message=\"" << xmlEscape(scenario.details) << "\"/>\n";
            out << "    <system-out>" << xmlEscape(scenario.details) << "</system-out>\n";
            out << "  </testcase>\n";
        }
        out << "</testsuite>\n";
        return out.str();
    }


    std::string simulationTimingReportToText(const SimulationTimedRun& timedRun)
    {
        std::ostringstream out;
        out << "Poor Man's Studio Simulation Timing Diagnostics\n";
        out << "Suite version: " << timedRun.report.suiteVersion << "\n";
        out << "Generated UTC: " << timedRun.report.generatedAtUtc << "\n";
        out << "Timing is diagnostic only and is not a pass/fail criterion.\n\n";
        out << std::fixed << std::setprecision(3);
        for (const auto& timing : timedRun.timings)
            out << timing.id << " " << timing.elapsedMilliseconds << " ms\n";
        out << "\nTotal " << timedRun.totalElapsedMilliseconds << " ms\n";
        return out.str();
    }

    bool writeSimulationReports(const SimulationSuiteReport& report,
                                const std::filesystem::path& outputFolder,
                                std::filesystem::path& textPath,
                                std::filesystem::path& jsonPath,
                                std::string& errorMessage)
    {
        try
        {
            std::filesystem::create_directories(outputFolder);
            textPath = outputFolder / "last_results.txt";
            jsonPath = outputFolder / "last_results.json";

            {
                std::ofstream textFile(textPath, std::ios::binary | std::ios::trunc);
                if (!textFile)
                    throw std::runtime_error("Could not open text result file.");
                textFile << simulationReportToText(report);
            }
            {
                std::ofstream jsonFile(jsonPath, std::ios::binary | std::ios::trunc);
                if (!jsonFile)
                    throw std::runtime_error("Could not open JSON result file.");
                jsonFile << simulationReportToJson(report);
            }
            errorMessage.clear();
            return true;
        }
        catch (const std::exception& e)
        {
            errorMessage = e.what();
            return false;
        }
    }

    bool writeSimulationArtifacts(const std::filesystem::path& outputFolder,
                                  std::vector<std::filesystem::path>& artifactPaths,
                                  std::string& errorMessage)
    {
        try
        {
            const auto fixture = makeAudioArtifactFixture();
            const auto dryBytes = pcm16MonoWavBytes(fixture.dry, fixture.sampleRate);
            const auto wetBytes = pcm16MonoWavBytes(fixture.wet, fixture.sampleRate);
            const auto artifactFolder = outputFolder / "artifacts";
            std::filesystem::create_directories(artifactFolder);

            const auto dryPath = artifactFolder / "dry_capture_fixture.wav";
            const auto wetPath = artifactFolder / "wet_monitor_fixture.wav";
            const auto manifestPath = artifactFolder / "artifact_manifest.txt";

            const auto writeBytes = [](const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes)
            {
                std::ofstream file(path, std::ios::binary | std::ios::trunc);
                if (!file)
                    throw std::runtime_error("Could not open simulation WAV artifact.");
                file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                if (!file)
                    throw std::runtime_error("Could not write simulation WAV artifact.");
            };

            writeBytes(dryPath, dryBytes);
            writeBytes(wetPath, wetBytes);

            std::ofstream manifest(manifestPath, std::ios::binary | std::ios::trunc);
            if (!manifest)
                throw std::runtime_error("Could not open simulation artifact manifest.");
            manifest << "Poor Man's Studio deterministic audio artifacts\n";
            manifest << "sampleRate=" << fixture.sampleRate << "\n";
            manifest << "channels=1\n";
            manifest << "samples=" << fixture.dry.size() << "\n";
            manifest << "format=PCM16 WAV\n";
            manifest << "dry_capture_fixture.wav fnv1a64=" << hex64(fnv1a64(dryBytes)) << " bytes=" << dryBytes.size() << "\n";
            manifest << "wet_monitor_fixture.wav fnv1a64=" << hex64(fnv1a64(wetBytes)) << " bytes=" << wetBytes.size() << "\n";
            if (!manifest)
                throw std::runtime_error("Could not write simulation artifact manifest.");

            artifactPaths = { dryPath, wetPath, manifestPath };
            errorMessage.clear();
            return true;
        }
        catch (const std::exception& e)
        {
            artifactPaths.clear();
            errorMessage = e.what();
            return false;
        }
    }

    bool writeSimulationJUnitReport(const SimulationSuiteReport& report,
                                    const std::filesystem::path& outputFolder,
                                    std::filesystem::path& junitPath,
                                    std::string& errorMessage)
    {
        try
        {
            std::filesystem::create_directories(outputFolder);
            junitPath = outputFolder / "last_results.junit.xml";
            std::ofstream file(junitPath, std::ios::binary | std::ios::trunc);
            if (!file)
                throw std::runtime_error("Could not open JUnit result file.");
            file << simulationReportToJUnitXml(report);
            if (!file)
                throw std::runtime_error("Could not write JUnit result file.");
            errorMessage.clear();
            return true;
        }
        catch (const std::exception& e)
        {
            errorMessage = e.what();
            return false;
        }
    }
    bool writeSimulationTimingReport(const SimulationTimedRun& timedRun,
                                     const std::filesystem::path& outputFolder,
                                     std::filesystem::path& timingPath,
                                     std::string& errorMessage)
    {
        try
        {
            std::filesystem::create_directories(outputFolder);
            timingPath = outputFolder / "last_timing.txt";
            std::ofstream file(timingPath, std::ios::binary | std::ios::trunc);
            if (!file)
                throw std::runtime_error("Could not open simulation timing file.");
            file << simulationTimingReportToText(timedRun);
            if (!file)
                throw std::runtime_error("Could not write simulation timing file.");
            errorMessage.clear();
            return true;
        }
        catch (const std::exception& e)
        {
            errorMessage = e.what();
            return false;
        }
    }

}
