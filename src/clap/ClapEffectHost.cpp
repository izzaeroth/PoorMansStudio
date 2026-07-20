#include "clap/ClapEffectHost.h"

#include "clap/ClapPluginScanner.h"

#include <juce_audio_utils/juce_audio_utils.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
    #ifndef NOMINMAX
        #define NOMINMAX 1
    #endif
    #include <windows.h>
#else
    #include <dlfcn.h>
#endif

namespace
{
    constexpr const char* clapPluginFactoryId = "clap.plugin-factory";
    constexpr const char* clapAudioPortsExtensionId = "clap.audio-ports";
    constexpr const char* clapStateExtensionId = "clap.state";
    constexpr std::size_t clapNameSize = 256;

    struct clap_version_t
    {
        std::uint32_t major;
        std::uint32_t minor;
        std::uint32_t revision;
    };

    struct clap_plugin_descriptor_t
    {
        clap_version_t clap_version;
        const char* id;
        const char* name;
        const char* vendor;
        const char* url;
        const char* manual_url;
        const char* support_url;
        const char* version;
        const char* description;
        const char* const* features;
    };

    struct clap_plugin_factory_t
    {
        std::uint32_t (*get_plugin_count)(const clap_plugin_factory_t* factory);
        const clap_plugin_descriptor_t* (*get_plugin_descriptor)(const clap_plugin_factory_t* factory, std::uint32_t index);
        const void* (*create_plugin)(const clap_plugin_factory_t* factory, const void* host, const char* plugin_id);
    };

    struct clap_plugin_t
    {
        const clap_plugin_descriptor_t* desc;
        void* plugin_data;
        bool (*init)(const clap_plugin_t* plugin);
        void (*destroy)(const clap_plugin_t* plugin);
        bool (*activate)(const clap_plugin_t* plugin, double sample_rate, std::uint32_t min_frames_count, std::uint32_t max_frames_count);
        void (*deactivate)(const clap_plugin_t* plugin);
        bool (*start_processing)(const clap_plugin_t* plugin);
        void (*stop_processing)(const clap_plugin_t* plugin);
        void (*reset)(const clap_plugin_t* plugin);
        int (*process)(const clap_plugin_t* plugin, const void* process);
        const void* (*get_extension)(const clap_plugin_t* plugin, const char* extension_id);
        void (*on_main_thread)(const clap_plugin_t* plugin);
    };

    using clap_id = std::uint32_t;

    struct clap_audio_port_info_t
    {
        clap_id id;
        char name[clapNameSize];
        std::uint32_t flags;
        std::uint32_t channel_count;
        const char* port_type;
        clap_id in_place_pair;
    };

    struct clap_plugin_audio_ports_t
    {
        std::uint32_t (*count)(const clap_plugin_t* plugin, bool is_input);
        bool (*get)(const clap_plugin_t* plugin, std::uint32_t index, bool is_input, clap_audio_port_info_t* info);
    };

    struct clap_ostream_t
    {
        void* ctx;
        std::int64_t (*write)(const clap_ostream_t* stream, const void* buffer, std::uint64_t size);
    };

    struct clap_istream_t
    {
        void* ctx;
        std::int64_t (*read)(const clap_istream_t* stream, void* buffer, std::uint64_t size);
    };

    struct clap_plugin_state_t
    {
        bool (*save)(const clap_plugin_t* plugin, const clap_ostream_t* stream);
        bool (*load)(const clap_plugin_t* plugin, const clap_istream_t* stream);
    };

    using clap_process_status = std::int32_t;
    constexpr clap_process_status clapProcessError = 0;
    constexpr clap_process_status clapProcessContinue = 1;
    constexpr clap_process_status clapProcessContinueIfNotQuiet = 2;
    constexpr clap_process_status clapProcessTail = 3;
    constexpr clap_process_status clapProcessSleep = 4;

    struct clap_event_header_t
    {
        std::uint32_t size;
        std::uint32_t time;
        std::uint16_t space_id;
        std::uint16_t type;
        std::uint32_t flags;
    };

    struct clap_input_events_t
    {
        void* ctx;
        std::uint32_t (*size)(const clap_input_events_t* list);
        const clap_event_header_t* (*get)(const clap_input_events_t* list, std::uint32_t index);
    };

    struct clap_output_events_t
    {
        void* ctx;
        bool (*try_push)(const clap_output_events_t* list, const clap_event_header_t* event);
    };

    struct clap_audio_buffer_t
    {
        float** data32;
        double** data64;
        std::uint32_t channel_count;
        std::uint32_t latency;
        std::uint64_t constant_mask;
    };

    struct clap_process_t
    {
        std::int64_t steady_time;
        std::uint32_t frames_count;
        const void* transport;
        const clap_audio_buffer_t* audio_inputs;
        clap_audio_buffer_t* audio_outputs;
        std::uint32_t audio_inputs_count;
        std::uint32_t audio_outputs_count;
        const clap_input_events_t* in_events;
        const clap_output_events_t* out_events;
    };

    struct ClapHostRequestState
    {
        bool restartRequested = false;
        bool processRequested = false;
        bool callbackRequested = false;
    };

    struct ClapOutputEventState
    {
        int pushedEvents = 0;
    };

    struct clap_host_t
    {
        clap_version_t clap_version;
        void* host_data;
        const char* name;
        const char* vendor;
        const char* url;
        const char* version;
        const void* (*get_extension)(const clap_host_t* host, const char* extension_id);
        void (*request_restart)(const clap_host_t* host);
        void (*request_process)(const clap_host_t* host);
        void (*request_callback)(const clap_host_t* host);
    };

    struct clap_plugin_entry_t
    {
        clap_version_t clap_version;
        bool (*init)(const char* plugin_path);
        void (*deinit)();
        const void* (*get_factory)(const char* factory_id);
    };

    struct AudioPortLayout
    {
        std::vector<std::uint32_t> inputChannels;
        std::vector<std::uint32_t> outputChannels;
        bool extensionAvailable = false;
        std::string summary;
    };

    struct AudioBufferStorage
    {
        std::vector<std::vector<std::vector<float>>> samples;
        std::vector<std::vector<float*>> channelPointers;
        std::vector<clap_audio_buffer_t> buffers;

        void build(const std::vector<std::uint32_t>& channelCounts, std::uint32_t frames)
        {
            samples.clear();
            channelPointers.clear();
            buffers.clear();

            samples.resize(channelCounts.size());
            channelPointers.resize(channelCounts.size());
            buffers.resize(channelCounts.size());

            for (std::size_t port = 0; port < channelCounts.size(); ++port)
            {
                const auto channels = channelCounts[port];
                samples[port].resize(channels);
                channelPointers[port].resize(channels, nullptr);

                for (std::uint32_t channel = 0; channel < channels; ++channel)
                {
                    samples[port][channel].assign(frames, 0.0f);
                    channelPointers[port][channel] = samples[port][channel].data();
                }

                buffers[port].data32 = channelPointers[port].empty() ? nullptr : channelPointers[port].data();
                buffers[port].data64 = nullptr;
                buffers[port].channel_count = channels;
                buffers[port].latency = 0;
                buffers[port].constant_mask = 0;
            }
        }

        int totalChannels() const
        {
            int total = 0;
            for (const auto& port : samples)
                total += static_cast<int>(port.size());
            return total;
        }
    };

    struct FlattenedChannelRef
    {
        std::size_t port = 0;
        std::size_t channel = 0;
    };

    class DynamicLibrary final
    {
    public:
        explicit DynamicLibrary(const std::filesystem::path& path)
        {
#if defined(_WIN32)
            handle = ::LoadLibraryW(path.wstring().c_str());
#else
            handle = dlopen(path.string().c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
        }

        ~DynamicLibrary()
        {
#if defined(_WIN32)
            if (handle != nullptr)
                ::FreeLibrary(static_cast<HMODULE>(handle));
#else
            if (handle != nullptr)
                dlclose(handle);
#endif
        }

        DynamicLibrary(const DynamicLibrary&) = delete;
        DynamicLibrary& operator=(const DynamicLibrary&) = delete;

        bool isLoaded() const { return handle != nullptr; }

        void* symbol(const char* name) const
        {
            if (handle == nullptr)
                return nullptr;
#if defined(_WIN32)
            return reinterpret_cast<void*>(::GetProcAddress(static_cast<HMODULE>(handle), name));
#else
            return dlsym(handle, name);
#endif
        }

        std::string lastErrorText() const
        {
#if defined(_WIN32)
            const auto code = ::GetLastError();
            if (code == 0)
                return {};

            LPWSTR buffer = nullptr;
            const auto size = ::FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                               nullptr,
                                               code,
                                               MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                               reinterpret_cast<LPWSTR>(&buffer),
                                               0,
                                               nullptr);
            std::wstring message;
            if (size > 0 && buffer != nullptr)
                message.assign(buffer, size);
            if (buffer != nullptr)
                ::LocalFree(buffer);

            if (message.empty())
                return {};

            const auto requiredBytes = ::WideCharToMultiByte(CP_UTF8, 0, message.c_str(), static_cast<int>(message.size()), nullptr, 0, nullptr, nullptr);
            if (requiredBytes <= 0)
                return {};
            std::string result(static_cast<std::size_t>(requiredBytes), '\0');
            const auto convertedBytes = ::WideCharToMultiByte(CP_UTF8, 0, message.c_str(), static_cast<int>(message.size()), result.data(), requiredBytes, nullptr, nullptr);
            if (convertedBytes <= 0)
                return {};
            result.resize(static_cast<std::size_t>(convertedBytes));
            return result;
#else
            const char* error = dlerror();
            return error == nullptr ? std::string() : std::string(error);
#endif
        }

    private:
#if defined(_WIN32)
        HMODULE handle = nullptr;
#else
        void* handle = nullptr;
#endif
    };

    std::string safeString(const char* value)
    {
        return value == nullptr ? std::string() : std::string(value);
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
        temp.replace_filename(input.stem().string() + ".clapfx.tmp.wav");
        return temp;
    }

    std::filesystem::path temporaryEffectTailTrimPathFor(const std::filesystem::path& input)
    {
        auto temp = input;
        temp.replace_filename(input.stem().string() + ".clapfx.tail.trim.tmp.wav");
        return temp;
    }

    constexpr float kDynamicTailSilenceThreshold = 1.0e-5f;
    constexpr double kDynamicTailSilenceHoldSeconds = 0.75;
    constexpr double kDynamicTailPadSeconds = 0.10;

    int lastAudibleSampleInBlock(const juce::AudioBuffer<float>& buffer, int numSamples)
    {
        const auto channels = buffer.getNumChannels();
        for (int sample = numSamples - 1; sample >= 0; --sample)
        {
            for (int ch = 0; ch < channels; ++ch)
            {
                if (std::abs(buffer.getReadPointer(ch)[sample]) > kDynamicTailSilenceThreshold)
                    return sample;
            }
        }
        return -1;
    }

    bool rewriteWavTrimmedToSamples(const std::filesystem::path& wavPath,
                                    std::int64_t keepSamples,
                                    int channelCount,
                                    int sampleRate,
                                    std::string& error)
    {
        if (keepSamples < 0)
            keepSamples = 0;

        juce::AudioFormatManager formatManager;
        formatManager.registerBasicFormats();

        std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(juce::File(wavPath.string())));
        if (reader == nullptr)
        {
            error = "Could not reopen wet CLAP WAV for dynamic tail trim: " + wavPath.string();
            return false;
        }

        keepSamples = std::min<std::int64_t>(keepSamples, static_cast<std::int64_t>(reader->lengthInSamples));
        if (keepSamples >= static_cast<std::int64_t>(reader->lengthInSamples))
            return true;

        const auto trimPath = temporaryEffectTailTrimPathFor(wavPath);
        std::error_code ec;
        if (std::filesystem::exists(trimPath, ec))
            std::filesystem::remove(trimPath, ec);

        juce::WavAudioFormat wavFormat;
        std::unique_ptr<juce::FileOutputStream> stream(juce::File(trimPath.string()).createOutputStream());
        if (stream == nullptr || !stream->openedOk())
        {
            error = "Could not create CLAP dynamic tail trim temp WAV: " + trimPath.string();
            return false;
        }

        std::unique_ptr<juce::AudioFormatWriter> writer(
            wavFormat.createWriterFor(stream.get(), static_cast<double>(sampleRate), static_cast<unsigned int>(channelCount), 24, {}, 0)
        );
        if (writer == nullptr)
        {
            error = "Could not create CLAP dynamic tail trim WAV writer.";
            return false;
        }
        stream.release();

        constexpr int blockSize = 32768;
        juce::AudioBuffer<float> buffer(channelCount, blockSize);
        std::int64_t position = 0;
        while (position < keepSamples)
        {
            const int blockSamples = static_cast<int>(std::min<std::int64_t>(blockSize, keepSamples - position));
            buffer.setSize(channelCount, blockSamples, false, false, true);
            buffer.clear();
            reader->read(&buffer, 0, blockSamples, position, true, true);
            writer->writeFromAudioSampleBuffer(buffer, 0, blockSamples);
            position += blockSamples;
        }

        writer.reset();
        reader.reset();

        std::filesystem::remove(wavPath, ec);
        ec.clear();
        std::filesystem::rename(trimPath, wavPath, ec);
        if (ec)
        {
            error = "Could not replace CLAP wet WAV with dynamic tail trim result: " + ec.message();
            return false;
        }

        return true;
    }

    const void* hostGetExtension(const clap_host_t*, const char*)
    {
        return nullptr;
    }

    void hostRequestRestart(const clap_host_t* host)
    {
        if (host != nullptr)
            if (auto* state = static_cast<ClapHostRequestState*>(host->host_data))
                state->restartRequested = true;
    }

    void hostRequestProcess(const clap_host_t* host)
    {
        if (host != nullptr)
            if (auto* state = static_cast<ClapHostRequestState*>(host->host_data))
                state->processRequested = true;
    }

    void hostRequestCallback(const clap_host_t* host)
    {
        if (host != nullptr)
            if (auto* state = static_cast<ClapHostRequestState*>(host->host_data))
                state->callbackRequested = true;
    }

    std::uint32_t emptyInputEventsSize(const clap_input_events_t*)
    {
        return 0;
    }

    const clap_event_header_t* emptyInputEventsGet(const clap_input_events_t*, std::uint32_t)
    {
        return nullptr;
    }

    bool outputEventsTryPush(const clap_output_events_t* events, const clap_event_header_t*)
    {
        if (events != nullptr)
            if (auto* state = static_cast<ClapOutputEventState*>(events->ctx))
                ++state->pushedEvents;
        return true;
    }


    struct MemoryReadContext
    {
        const std::uint8_t* data = nullptr;
        std::size_t size = 0;
        std::size_t offset = 0;
    };

    std::int64_t memoryRead(const clap_istream_t* stream, void* buffer, std::uint64_t size)
    {
        if (stream == nullptr || stream->ctx == nullptr || (buffer == nullptr && size > 0))
            return -1;
        if (size > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            return -1;

        auto* context = static_cast<MemoryReadContext*>(stream->ctx);
        const auto available = context->offset <= context->size ? context->size - context->offset : 0;
        const auto amount = std::min<std::size_t>(available, static_cast<std::size_t>(size));
        if (amount > 0)
        {
            std::memcpy(buffer, context->data + context->offset, amount);
            context->offset += amount;
        }
        return static_cast<std::int64_t>(amount);
    }

    bool loadClapStateBase64(const clap_plugin_t* plugin, const std::string& stateBase64, std::string& error)
    {
        if (stateBase64.empty())
            return true;
        if (plugin == nullptr || plugin->get_extension == nullptr)
        {
            error = "CLAP plugin does not expose get_extension() for state restore.";
            return false;
        }

        const auto* state = static_cast<const clap_plugin_state_t*>(plugin->get_extension(plugin, clapStateExtensionId));
        if (state == nullptr || state->load == nullptr)
        {
            error = "CLAP plugin does not expose clap.state load().";
            return false;
        }

        juce::MemoryBlock block;
        if (!block.fromBase64Encoding(juce::String(stateBase64)) || block.getSize() == 0)
        {
            error = "Saved CLAP state is not valid base64 data.";
            return false;
        }

        MemoryReadContext context;
        context.data = static_cast<const std::uint8_t*>(block.getData());
        context.size = block.getSize();
        clap_istream_t stream {};
        stream.ctx = &context;
        stream.read = memoryRead;
        if (!state->load(plugin, &stream))
        {
            error = "CLAP state load() returned false.";
            return false;
        }
        return true;
    }

    std::string processStatusToString(int status)
    {
        switch (status)
        {
            case clapProcessError: return "error";
            case clapProcessContinue: return "continue";
            case clapProcessContinueIfNotQuiet: return "continue-if-not-quiet";
            case clapProcessTail: return "tail";
            case clapProcessSleep: return "sleep";
            default: return "unknown(" + std::to_string(status) + ")";
        }
    }

    bool isProcessStatusOk(int status)
    {
        return status == clapProcessContinue
            || status == clapProcessContinueIfNotQuiet
            || status == clapProcessTail
            || status == clapProcessSleep;
    }

    std::vector<std::uint32_t> collectPortChannelsFromExtension(const clap_plugin_t* plugin, const clap_plugin_audio_ports_t* extension, bool isInput)
    {
        std::vector<std::uint32_t> channels;
        if (plugin == nullptr || extension == nullptr || extension->count == nullptr || extension->get == nullptr)
            return channels;

        constexpr std::uint32_t maxPorts = 16;
        constexpr std::uint32_t maxChannelsPerPort = 32;
        const auto count = std::min(extension->count(plugin, isInput), maxPorts);
        channels.reserve(count);

        for (std::uint32_t i = 0; i < count; ++i)
        {
            clap_audio_port_info_t info {};
            if (extension->get(plugin, i, isInput, &info))
                channels.push_back(std::max<std::uint32_t>(1, std::min(info.channel_count, maxChannelsPerPort)));
        }

        return channels;
    }

    AudioPortLayout chooseAudioPortLayout(const clap_plugin_t* plugin, int fallbackChannels)
    {
        AudioPortLayout layout;
        const auto safeFallbackChannels = static_cast<std::uint32_t>(juce::jlimit(1, 8, fallbackChannels));

        if (plugin != nullptr && plugin->get_extension != nullptr)
        {
            const auto* extension = static_cast<const clap_plugin_audio_ports_t*>(plugin->get_extension(plugin, clapAudioPortsExtensionId));
            if (extension != nullptr && extension->count != nullptr && extension->get != nullptr)
            {
                layout.extensionAvailable = true;
                layout.inputChannels = collectPortChannelsFromExtension(plugin, extension, true);
                layout.outputChannels = collectPortChannelsFromExtension(plugin, extension, false);
            }
        }

        if (layout.inputChannels.empty())
            layout.inputChannels = { safeFallbackChannels };
        if (layout.outputChannels.empty())
            layout.outputChannels = { safeFallbackChannels };

        std::ostringstream text;
        text << (layout.extensionAvailable ? "audio-ports extension" : "fallback stereo-compatible ports")
             << "; inputs=" << layout.inputChannels.size()
             << "; outputs=" << layout.outputChannels.size();
        layout.summary = text.str();
        return layout;
    }

    std::vector<FlattenedChannelRef> flattenChannels(const AudioBufferStorage& storage)
    {
        std::vector<FlattenedChannelRef> refs;
        for (std::size_t port = 0; port < storage.samples.size(); ++port)
            for (std::size_t channel = 0; channel < storage.samples[port].size(); ++channel)
                refs.push_back({ port, channel });
        return refs;
    }

    void copyJuceToClapInputs(const juce::AudioBuffer<float>& source, AudioBufferStorage& inputs, int frames)
    {
        const auto refs = flattenChannels(inputs);
        const int sourceChannels = source.getNumChannels();
        if (sourceChannels <= 0 || frames <= 0)
            return;

        for (std::size_t i = 0; i < refs.size(); ++i)
        {
            const auto& ref = refs[i];
            auto& dest = inputs.samples[ref.port][ref.channel];
            const int sourceChannel = static_cast<int>(std::min<std::size_t>(i, static_cast<std::size_t>(sourceChannels - 1)));
            const auto* src = source.getReadPointer(sourceChannel);
            std::copy(src, src + frames, dest.begin());
        }
    }

    void primeOutputsFromInputs(const AudioBufferStorage& inputs, AudioBufferStorage& outputs, int frames)
    {
        const auto inputRefs = flattenChannels(inputs);
        const auto outputRefs = flattenChannels(outputs);
        if (inputRefs.empty() || outputRefs.empty() || frames <= 0)
            return;

        for (std::size_t i = 0; i < outputRefs.size(); ++i)
        {
            const auto& out = outputRefs[i];
            const auto& in = inputRefs[std::min(i, inputRefs.size() - 1)];
            const auto& src = inputs.samples[in.port][in.channel];
            auto& dest = outputs.samples[out.port][out.channel];
            std::copy(src.begin(), src.begin() + frames, dest.begin());
        }
    }

    void copyClapOutputsToJuce(const AudioBufferStorage& outputs, juce::AudioBuffer<float>& dest, int frames)
    {
        const auto refs = flattenChannels(outputs);
        const int destChannels = dest.getNumChannels();
        dest.clear();
        if (refs.empty() || destChannels <= 0 || frames <= 0)
            return;

        for (int channel = 0; channel < destChannels; ++channel)
        {
            const auto& ref = refs[std::min<std::size_t>(static_cast<std::size_t>(channel), refs.size() - 1)];
            const auto& src = outputs.samples[ref.port][ref.channel];
            auto* dst = dest.getWritePointer(channel);
            std::copy(src.begin(), src.begin() + frames, dst);
        }
    }

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
}

namespace mw::clap
{
    ClapEffectProcessResult ClapEffectHost::processWavWithPlugin(const ClapEffectProcessRequest& request)
    {
        ClapEffectProcessResult result;
        result.wavPath = request.outputWavPath.empty() ? request.inputWavPath : request.outputWavPath;

        if (request.cancelRequested != nullptr && request.cancelRequested->load())
        {
            result.cancelled = true;
            result.message = "CLAP effect processing cancelled before start.";
            return result;
        }

        if (request.plugin.bundlePath.empty())
        {
            std::string copyError;
            result.success = copyAudioFileForUnprocessedEffect(request.inputWavPath, result.wavPath, copyError);
            result.effectApplied = false;
            result.message = result.success
                ? "CLAP effect assignment has no plugin path; WAV left unchanged."
                : "CLAP effect assignment has no plugin path; dry WAV copy failed: " + copyError;
            return result;
        }

        if (request.inputWavPath.empty() || !std::filesystem::exists(request.inputWavPath))
        {
            result.message = "CLAP effect input WAV not found: " + request.inputWavPath.string();
            return result;
        }

        juce::AudioFormatManager formatManager;
        formatManager.registerBasicFormats();

        std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(juce::File(request.inputWavPath.string())));
        if (reader == nullptr)
        {
            result.message = "Could not open WAV for CLAP effect processing: " + request.inputWavPath.string();
            return result;
        }

        const int channelCount = juce::jlimit(1, 8, static_cast<int>(reader->numChannels));
        const int sampleRate = static_cast<int>(std::max(1.0, reader->sampleRate));
        const int blockSize = juce::jlimit(64, 8192, request.blockSize);
        const auto totalInputSamples = static_cast<std::int64_t>(reader->lengthInSamples);
        const auto tailSamples = static_cast<std::int64_t>(std::max(0.0, request.tailSeconds) * static_cast<double>(sampleRate));
        result.sampleRate = sampleRate;
        result.channelCount = channelCount;

        auto inspected = ClapPluginScanner::inspectPluginPath(request.plugin.bundlePath);
        if (!ClapPluginScanner::isOuterClapPlugin(request.plugin.bundlePath))
        {
            result.message = "CLAP effect path is not an outer .clap plugin file or bundle: " + request.plugin.bundlePath.string();
            return result;
        }
        if (inspected.binaryPath.empty())
        {
            result.message = "Could not find a loadable CLAP binary inside effect plugin: " + request.plugin.bundlePath.string();
            return result;
        }

        DynamicLibrary library(inspected.binaryPath);
        if (!library.isLoaded())
        {
            result.message = "Could not load CLAP effect binary";
            const auto error = library.lastErrorText();
            if (!error.empty())
                result.message += ": " + error;
            return result;
        }

        const clap_plugin_entry_t* initializedEntry = nullptr;
        const clap_plugin_t* createdPlugin = nullptr;
        bool pluginActivated = false;
        bool pluginStartedProcessing = false;
        ClapHostRequestState hostState;

        auto cleanup = [&]()
        {
            if (createdPlugin != nullptr && pluginStartedProcessing && createdPlugin->stop_processing != nullptr)
            {
                createdPlugin->stop_processing(createdPlugin);
                pluginStartedProcessing = false;
            }

            if (createdPlugin != nullptr && pluginActivated && createdPlugin->deactivate != nullptr)
            {
                createdPlugin->deactivate(createdPlugin);
                pluginActivated = false;
            }

            if (createdPlugin != nullptr && createdPlugin->destroy != nullptr)
            {
                createdPlugin->destroy(createdPlugin);
                createdPlugin = nullptr;
            }

            if (initializedEntry != nullptr && initializedEntry->deinit != nullptr)
            {
                initializedEntry->deinit();
                initializedEntry = nullptr;
            }
        };

        auto fail = [&](std::string message) -> ClapEffectProcessResult&
        {
            cleanup();
            result.message = std::move(message);
            return result;
        };

        try
        {
            auto* entry = static_cast<const clap_plugin_entry_t*>(library.symbol("clap_entry"));
            if (entry == nullptr)
                return fail("Loaded CLAP effect binary but did not find exported CLAP entry symbol 'clap_entry'.");

            if (entry->init == nullptr || entry->deinit == nullptr || entry->get_factory == nullptr)
                return fail("CLAP effect entry is incomplete.");

            if (!entry->init(request.plugin.bundlePath.string().c_str()))
                return fail("CLAP effect entry init() returned false.");
            initializedEntry = entry;

            const auto* factoryVoid = entry->get_factory(clapPluginFactoryId);
            const auto* factory = static_cast<const clap_plugin_factory_t*>(factoryVoid);
            if (factory == nullptr || factory->get_plugin_count == nullptr || factory->get_plugin_descriptor == nullptr || factory->create_plugin == nullptr)
                return fail("CLAP plugin factory was not available or did not expose create_plugin().");

            const auto count = factory->get_plugin_count(factory);
            if (count == 0)
                return fail("CLAP plugin factory reported zero plugins.");

            const clap_plugin_descriptor_t* selectedDesc = nullptr;
            std::uint32_t selectedIndex = 0;
            for (std::uint32_t i = 0; i < count; ++i)
            {
                const auto* desc = factory->get_plugin_descriptor(factory, i);
                if (desc == nullptr)
                    continue;

                const auto descId = safeString(desc->id);
                const auto descName = safeString(desc->name);
                if ((!request.plugin.uid.empty() && descId == request.plugin.uid)
                    || (!request.plugin.name.empty() && descName == request.plugin.name))
                {
                    selectedDesc = desc;
                    selectedIndex = i;
                    break;
                }

                if (selectedDesc == nullptr)
                {
                    selectedDesc = desc;
                    selectedIndex = i;
                }
            }

            if (selectedDesc == nullptr)
                return fail("CLAP plugin factory did not return a usable descriptor.");
            if (selectedDesc->id == nullptr || std::string(selectedDesc->id).empty())
                return fail("CLAP descriptor did not provide a plugin id for create_plugin().");

            clap_host_t host {};
            host.clap_version = { 1, 1, 0 };
            host.host_data = &hostState;
            host.name = "Poor Man's Studio CLAP Effect Host";
            host.vendor = "Poor Man's Studio";
            host.url = "";
            host.version = "0.66.3";
            host.get_extension = hostGetExtension;
            host.request_restart = hostRequestRestart;
            host.request_process = hostRequestProcess;
            host.request_callback = hostRequestCallback;

            const auto* pluginVoid = factory->create_plugin(factory, &host, selectedDesc->id);
            const auto* plugin = static_cast<const clap_plugin_t*>(pluginVoid);
            if (plugin == nullptr)
                return fail("CLAP plugin factory returned a null effect instance.");
            createdPlugin = plugin;

            if (plugin->destroy == nullptr)
                return fail("CLAP effect instance did not expose destroy().");
            if (plugin->init == nullptr)
                return fail("CLAP effect instance did not expose init().");
            if (!plugin->init(plugin))
                return fail("CLAP effect init() returned false.");

            if (!request.plugin.stateBase64.empty())
            {
                std::string stateError;
                if (!loadClapStateBase64(plugin, request.plugin.stateBase64, stateError))
                    return fail("CLAP effect saved state restore failed before processing: " + stateError);
            }

            if (plugin->activate == nullptr || plugin->deactivate == nullptr)
                return fail("CLAP effect instance did not expose activate()/deactivate().");
            if (plugin->start_processing == nullptr || plugin->stop_processing == nullptr || plugin->process == nullptr)
                return fail("CLAP effect instance did not expose the required offline processing callbacks.");

            if (!plugin->activate(plugin, static_cast<double>(sampleRate), 1, static_cast<std::uint32_t>(blockSize)))
                return fail("CLAP effect activate() returned false.");
            pluginActivated = true;

            const auto layout = chooseAudioPortLayout(plugin, channelCount);

            if (!plugin->start_processing(plugin))
                return fail("CLAP effect start_processing() returned false.");
            pluginStartedProcessing = true;

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
                return fail("Could not create CLAP effect output WAV: " + actualOutputPath.string());

            std::unique_ptr<juce::AudioFormatWriter> writer(
                wavFormat.createWriterFor(stream.get(), static_cast<double>(sampleRate), static_cast<unsigned int>(channelCount), 24, {}, 0)
            );

            if (writer == nullptr)
                return fail("Could not create WAV writer for CLAP effect output.");
            stream.release();

            juce::AudioBuffer<float> fileAudio(channelCount, blockSize);
            juce::AudioBuffer<float> outputAudio(channelCount, blockSize);

            clap_input_events_t inputEvents {};
            inputEvents.ctx = nullptr;
            inputEvents.size = emptyInputEventsSize;
            inputEvents.get = emptyInputEventsGet;

            ClapOutputEventState outputEventState;
            clap_output_events_t outputEvents {};
            outputEvents.ctx = &outputEventState;
            outputEvents.try_push = outputEventsTryPush;

            std::int64_t readerSamplePosition = 0;
            std::int64_t writtenSamples = 0;
            std::int64_t lastAudibleOutputSample = totalInputSamples > 0 ? totalInputSamples - 1 : 0;
            std::int64_t silentTailSamples = 0;
            const auto silenceHoldSamples = static_cast<std::int64_t>(kDynamicTailSilenceHoldSeconds * static_cast<double>(sampleRate));
            const auto trimPadSamples = static_cast<std::int64_t>(kDynamicTailPadSeconds * static_cast<double>(sampleRate));
            bool dynamicTailStoppedOnSilence = false;

            auto processOneBlock = [&](int blockSamples, bool tailBlock) -> bool
            {
                fileAudio.setSize(channelCount, blockSamples, false, false, true);
                fileAudio.clear();
                outputAudio.setSize(channelCount, blockSamples, false, false, true);
                outputAudio.clear();

                AudioBufferStorage inputBuffers;
                AudioBufferStorage outputBuffers;
                inputBuffers.build(layout.inputChannels, static_cast<std::uint32_t>(blockSamples));
                outputBuffers.build(layout.outputChannels, static_cast<std::uint32_t>(blockSamples));

                if (!tailBlock)
                {
                    reader->read(&fileAudio, 0, blockSamples, readerSamplePosition, true, true);
                    copyJuceToClapInputs(fileAudio, inputBuffers, blockSamples);
                    primeOutputsFromInputs(inputBuffers, outputBuffers, blockSamples);
                }

                clap_process_t process {};
                process.steady_time = -1;
                process.frames_count = static_cast<std::uint32_t>(blockSamples);
                process.transport = nullptr;
                process.audio_inputs = inputBuffers.buffers.empty() ? nullptr : inputBuffers.buffers.data();
                process.audio_outputs = outputBuffers.buffers.empty() ? nullptr : outputBuffers.buffers.data();
                process.audio_inputs_count = static_cast<std::uint32_t>(inputBuffers.buffers.size());
                process.audio_outputs_count = static_cast<std::uint32_t>(outputBuffers.buffers.size());
                process.in_events = &inputEvents;
                process.out_events = &outputEvents;

                const auto status = plugin->process(plugin, &process);
                result.processStatus = status;
                result.processStatusText = processStatusToString(status);
                if (!isProcessStatusOk(status))
                    return false;

                copyClapOutputsToJuce(outputBuffers, outputAudio, blockSamples);
                const auto audibleIndex = lastAudibleSampleInBlock(outputAudio, blockSamples);
                if (audibleIndex >= 0)
                {
                    lastAudibleOutputSample = writtenSamples + audibleIndex;
                    silentTailSamples = tailBlock ? (blockSamples - audibleIndex - 1) : 0;
                }
                else if (tailBlock)
                {
                    silentTailSamples += blockSamples;
                }
                writer->writeFromAudioSampleBuffer(outputAudio, 0, blockSamples);
                writtenSamples += blockSamples;
                ++result.processedBlocks;
                return true;
            };

            while (readerSamplePosition < totalInputSamples)
            {
                if (request.cancelRequested != nullptr && request.cancelRequested->load())
                {
                    cleanup();
                    result.cancelled = true;
                    result.message = "CLAP effect processing cancelled.";
                    return result;
                }

                const int blockSamples = static_cast<int>(std::min<std::int64_t>(blockSize, totalInputSamples - readerSamplePosition));
                if (!processOneBlock(blockSamples, false))
                    return fail("CLAP effect process() returned " + result.processStatusText + ".");
                readerSamplePosition += blockSamples;
            }

            std::int64_t tailWritten = 0;
            while (tailWritten < tailSamples)
            {
                if (request.cancelRequested != nullptr && request.cancelRequested->load())
                {
                    cleanup();
                    result.cancelled = true;
                    result.message = "CLAP effect processing cancelled during tail render.";
                    return result;
                }

                const int blockSamples = static_cast<int>(std::min<std::int64_t>(blockSize, tailSamples - tailWritten));
                if (!processOneBlock(blockSamples, true))
                    return fail("CLAP effect tail process() returned " + result.processStatusText + ".");
                tailWritten += blockSamples;

                if (silenceHoldSamples > 0 && silentTailSamples >= silenceHoldSamples)
                {
                    dynamicTailStoppedOnSilence = true;
                    break;
                }
            }

            writer.reset();
            reader.reset();
            cleanup();

            const auto minimumKeepSamples = std::max<std::int64_t>(0, totalInputSamples);
            const auto dynamicKeepSamples = std::min<std::int64_t>(writtenSamples, std::max<std::int64_t>(minimumKeepSamples, lastAudibleOutputSample + 1 + trimPadSamples));
            const auto trimmedSamples = std::max<std::int64_t>(0, writtenSamples - dynamicKeepSamples);
            if (trimmedSamples > 0)
            {
                std::string trimError;
                if (!rewriteWavTrimmedToSamples(actualOutputPath, dynamicKeepSamples, channelCount, sampleRate, trimError))
                    return fail(trimError);
            }

            if (replacingInput)
            {
                ec.clear();
                std::filesystem::remove(request.inputWavPath, ec);
                if (ec)
                {
                    result.message = "Rendered CLAP effect output, but could not remove original WAV: " + ec.message();
                    return result;
                }

                ec.clear();
                std::filesystem::rename(actualOutputPath, request.inputWavPath, ec);
                if (ec)
                {
                    result.message = "Rendered CLAP effect output, but could not replace original WAV: " + ec.message();
                    return result;
                }
            }

            const auto effectName = request.plugin.name.empty()
                ? request.plugin.bundlePath.filename().string()
                : request.plugin.name;

            result.success = true;
            result.effectApplied = true;
            result.message = "Processed WAV through CLAP effect. Plugin: " + effectName
                + "; selected descriptor index: " + std::to_string(selectedIndex)
                + "; port layout: " + layout.summary
                + "; blocks: " + std::to_string(result.processedBlocks)
                + "; last process status: " + result.processStatusText
                + "; max tail overscan seconds: " + std::to_string(std::max(0.0, request.tailSeconds))
                + "; dynamic tail stopped on silence: " + std::string(dynamicTailStoppedOnSilence ? "yes" : "no")
                + "; dynamic tail trimmed samples: " + std::to_string(trimmedSamples)
                + "; host restart requested: " + std::string(hostState.restartRequested ? "yes" : "no")
                + "; host process requested: " + std::string(hostState.processRequested ? "yes" : "no")
                + "; host callback requested: " + std::string(hostState.callbackRequested ? "yes" : "no")
                + "; input: " + request.inputWavPath.string()
                + "; output: " + result.wavPath.string();
            return result;
        }
        catch (const std::exception& ex)
        {
            cleanup();
            result.message = std::string("CLAP effect processing failed: ") + ex.what();
            return result;
        }
        catch (...)
        {
            cleanup();
            result.message = "CLAP effect processing failed with an unknown exception.";
            return result;
        }
    }
}
