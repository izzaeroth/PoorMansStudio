#include "clap/ClapLiveEffectSession.h"

#include "app/AppVersion.h"
#include "clap/ClapPluginScanner.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <limits>
#include <sstream>
#include <string>
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
    constexpr const char* clapStateExtensionId = "clap.state";
    constexpr const char* clapAudioPortsExtensionId = "clap.audio-ports";
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

    struct MemoryReadContext
    {
        const std::uint8_t* data = nullptr;
        std::size_t size = 0;
        std::size_t offset = 0;
    };

    struct LiveAudioStorage
    {
        std::vector<std::vector<float>> samples;
        std::vector<float*> channelPointers;
        clap_audio_buffer_t buffer {};

        void build(int channelCount, int frames)
        {
            const auto channels = std::max(1, std::min(channelCount, 32));
            const auto frameCount = std::max(1, frames);
            samples.assign(static_cast<std::size_t>(channels), {});
            channelPointers.assign(static_cast<std::size_t>(channels), nullptr);

            for (int channel = 0; channel < channels; ++channel)
            {
                samples[static_cast<std::size_t>(channel)].assign(static_cast<std::size_t>(frameCount), 0.0f);
                channelPointers[static_cast<std::size_t>(channel)] = samples[static_cast<std::size_t>(channel)].data();
            }

            buffer.data32 = channelPointers.empty() ? nullptr : channelPointers.data();
            buffer.data64 = nullptr;
            buffer.channel_count = static_cast<std::uint32_t>(channels);
            buffer.latency = 0;
            buffer.constant_mask = 0;
        }
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

    bool outputEventsTryPush(const clap_output_events_t*, const clap_event_header_t*)
    {
        return true;
    }

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

    bool restoreStateBase64(const clap_plugin_t* plugin, const std::string& stateBase64, std::string& error)
    {
        if (stateBase64.empty())
            return true;
        if (plugin == nullptr || plugin->get_extension == nullptr)
        {
            error = "CLAP live effect does not expose get_extension() for state restore.";
            return false;
        }

        const auto* state = static_cast<const clap_plugin_state_t*>(plugin->get_extension(plugin, clapStateExtensionId));
        if (state == nullptr || state->load == nullptr)
        {
            error = "CLAP live effect does not expose clap.state load().";
            return false;
        }

        juce::MemoryBlock block;
        if (!block.fromBase64Encoding(juce::String(stateBase64)) || block.getSize() == 0)
        {
            error = "Saved CLAP live effect state is not valid base64 data.";
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
            error = "CLAP live effect state load() returned false.";
            return false;
        }
        return true;
    }

    int firstAudioPortChannelCount(const clap_plugin_t* plugin, bool isInput, int fallbackChannels, bool& extensionAvailable)
    {
        const auto safeFallback = std::max(1, std::min(fallbackChannels, 32));
        if (plugin == nullptr || plugin->get_extension == nullptr)
            return safeFallback;

        const auto* ports = static_cast<const clap_plugin_audio_ports_t*>(plugin->get_extension(plugin, clapAudioPortsExtensionId));
        extensionAvailable = ports != nullptr;
        if (ports == nullptr || ports->count == nullptr || ports->get == nullptr)
            return safeFallback;

        const auto count = ports->count(plugin, isInput);
        if (count == 0)
            return isInput ? 0 : safeFallback;

        clap_audio_port_info_t info {};
        if (!ports->get(plugin, 0, isInput, &info) || info.channel_count == 0)
            return safeFallback;

        return std::max(1, std::min(static_cast<int>(info.channel_count), 32));
    }

    void copyInterleavedToStorage(const std::vector<float>& interleaved,
                                  int inputChannels,
                                  LiveAudioStorage& storage,
                                  int frames)
    {
        const auto storageChannels = static_cast<int>(storage.samples.size());
        const auto sourceChannels = std::max(1, inputChannels);
        for (int frame = 0; frame < frames; ++frame)
        {
            for (int channel = 0; channel < storageChannels; ++channel)
            {
                float sample = 0.0f;
                if (channel < sourceChannels)
                {
                    const auto idx = static_cast<std::size_t>(frame * sourceChannels + channel);
                    if (idx < interleaved.size())
                        sample = interleaved[idx];
                }
                storage.samples[static_cast<std::size_t>(channel)][static_cast<std::size_t>(frame)] = sample;
            }
        }
    }

    void primeOutputsFromInputs(const LiveAudioStorage& inputStorage,
                                LiveAudioStorage& outputStorage,
                                int frames)
    {
        const auto channels = std::min(inputStorage.samples.size(), outputStorage.samples.size());
        for (std::size_t channel = 0; channel < channels; ++channel)
            for (int frame = 0; frame < frames; ++frame)
                outputStorage.samples[channel][static_cast<std::size_t>(frame)] = inputStorage.samples[channel][static_cast<std::size_t>(frame)];
    }

    std::vector<float> storageToInterleaved(const LiveAudioStorage& storage, int frames)
    {
        const auto channels = static_cast<int>(storage.samples.size());
        std::vector<float> interleaved(static_cast<std::size_t>(std::max(0, frames) * std::max(0, channels)), 0.0f);
        for (int frame = 0; frame < frames; ++frame)
            for (int channel = 0; channel < channels; ++channel)
                interleaved[static_cast<std::size_t>(frame * channels + channel)] = storage.samples[static_cast<std::size_t>(channel)][static_cast<std::size_t>(frame)];
        return interleaved;
    }
}

namespace mw::clap
{
    struct ClapLiveEffectSession::Impl
    {
        ~Impl()
        {
            close();
        }

        bool open(const ClapLiveEffectSessionConfig& configIn, std::string& error)
        {
            close();
            config = configIn;
            infoState = {};
            infoState.pluginPath = config.pluginPath;
            infoState.pluginName = config.pluginName;
            infoState.pluginUid = config.pluginUid;
            infoState.sampleRate = std::max(8000, std::min(config.sampleRate, 384000));
            infoState.blockSize = std::max(16, std::min(config.blockSize, 8192));
            const auto fallbackChannels = std::max(1, std::min(config.channelCount, 32));

            if (config.pluginPath.empty())
                return fail(error, "CLAP live effect has no plugin path.");
            if (!ClapPluginScanner::isOuterClapPlugin(config.pluginPath))
                return fail(error, "CLAP live effect path is not an outer .clap plugin file or bundle: " + config.pluginPath.string());

            const auto inspected = ClapPluginScanner::inspectPluginPath(config.pluginPath);
            if (inspected.binaryPath.empty())
                return fail(error, "Could not find a loadable CLAP binary inside live effect plugin: " + config.pluginPath.string());

            library = std::make_unique<DynamicLibrary>(inspected.binaryPath);
            if (!library->isLoaded())
            {
                auto message = std::string("Could not load CLAP live effect binary");
                const auto libraryError = library->lastErrorText();
                if (!libraryError.empty())
                    message += ": " + libraryError;
                return fail(error, std::move(message));
            }

            entry = static_cast<const clap_plugin_entry_t*>(library->symbol("clap_entry"));
            if (entry == nullptr)
                return fail(error, "Loaded CLAP live effect binary but did not find exported CLAP entry symbol 'clap_entry'.");
            if (entry->init == nullptr || entry->deinit == nullptr || entry->get_factory == nullptr)
                return fail(error, "CLAP live effect entry is incomplete.");
            if (!entry->init(config.pluginPath.string().c_str()))
                return fail(error, "CLAP live effect entry init() returned false.");
            entryInitialized = true;

            const auto* factoryVoid = entry->get_factory(clapPluginFactoryId);
            const auto* factory = static_cast<const clap_plugin_factory_t*>(factoryVoid);
            if (factory == nullptr || factory->get_plugin_count == nullptr || factory->get_plugin_descriptor == nullptr || factory->create_plugin == nullptr)
                return fail(error, "CLAP live effect factory was not available or did not expose create_plugin().");

            const auto count = factory->get_plugin_count(factory);
            if (count == 0)
                return fail(error, "CLAP live effect factory reported zero plugins.");

            const clap_plugin_descriptor_t* selectedDesc = nullptr;
            for (std::uint32_t i = 0; i < count; ++i)
            {
                const auto* desc = factory->get_plugin_descriptor(factory, i);
                if (desc == nullptr)
                    continue;

                const auto descId = safeString(desc->id);
                const auto descName = safeString(desc->name);
                if ((!config.pluginUid.empty() && descId == config.pluginUid)
                    || (!config.pluginName.empty() && descName == config.pluginName))
                {
                    selectedDesc = desc;
                    break;
                }

                if (selectedDesc == nullptr)
                    selectedDesc = desc;
            }

            if (selectedDesc == nullptr)
                return fail(error, "CLAP live effect factory did not return a usable descriptor.");
            if (selectedDesc->id == nullptr || std::string(selectedDesc->id).empty())
                return fail(error, "CLAP live effect descriptor did not provide a plugin id for create_plugin().");

            hostState = {};
            host = {};
            host.clap_version = { 1, 1, 0 };
            host.host_data = &hostState;
            host.name = "Poor Man's Studio CLAP Live Effect Host";
            host.vendor = "Poor Man's Studio";
            host.url = "";
            host.version = mw::app::appVersion;
            host.get_extension = hostGetExtension;
            host.request_restart = hostRequestRestart;
            host.request_process = hostRequestProcess;
            host.request_callback = hostRequestCallback;

            const auto* pluginVoid = factory->create_plugin(factory, &host, selectedDesc->id);
            plugin = static_cast<const clap_plugin_t*>(pluginVoid);
            if (plugin == nullptr)
                return fail(error, "CLAP live effect factory returned a null effect instance.");
            if (plugin->destroy == nullptr)
                return fail(error, "CLAP live effect instance did not expose destroy().");
            if (plugin->init == nullptr)
                return fail(error, "CLAP live effect instance did not expose init().");
            if (!plugin->init(plugin))
                return fail(error, "CLAP live effect init() returned false.");
            pluginInitialized = true;

            if (!config.stateBase64.empty())
            {
                std::string stateError;
                if (!restoreStateBase64(plugin, config.stateBase64, stateError))
                    return fail(error, "CLAP live effect saved state restore failed: " + stateError);
                infoState.stateRestored = true;
            }

            bool audioPortsAvailable = false;
            inputChannelCount = firstAudioPortChannelCount(plugin, true, fallbackChannels, audioPortsAvailable);
            outputChannelCount = firstAudioPortChannelCount(plugin, false, fallbackChannels, audioPortsAvailable);
            infoState.audioPortsAvailable = audioPortsAvailable;
            infoState.inputChannelCount = inputChannelCount;
            infoState.outputChannelCount = outputChannelCount;

            if (outputChannelCount <= 0)
                return fail(error, "CLAP live effect did not expose a usable audio output port.");

            if (plugin->activate == nullptr || plugin->deactivate == nullptr)
                return fail(error, "CLAP live effect instance did not expose activate()/deactivate().");
            if (plugin->start_processing == nullptr || plugin->stop_processing == nullptr || plugin->process == nullptr)
                return fail(error, "CLAP live effect instance did not expose start/stop/process callbacks needed for live playback.");
            if (!plugin->activate(plugin, static_cast<double>(infoState.sampleRate), 1, static_cast<std::uint32_t>(infoState.blockSize)))
                return fail(error, "CLAP live effect activate() returned false.");
            activated = true;

            if (!plugin->start_processing(plugin))
                return fail(error, "CLAP live effect start_processing() returned false.");
            startedProcessing = true;
            infoState.startedProcessing = true;
            infoState.open = true;

            if (inputChannelCount > 0)
                realtimeInputStorage.build(inputChannelCount, infoState.blockSize);
            realtimeOutputStorage.build(std::max(1, outputChannelCount), infoState.blockSize);
            realtimeStorageReady = true;

            std::ostringstream message;
            message << "CLAP live effect instance opened and started for "
                    << (config.pluginName.empty() ? safeString(selectedDesc->name) : config.pluginName)
                    << " at " << infoState.sampleRate << " Hz, block " << infoState.blockSize
                    << ", input channels " << inputChannelCount
                    << ", output channels " << outputChannelCount
                    << ", state restored: " << (infoState.stateRestored ? "yes" : "no")
                    << ", audio-ports: " << (infoState.audioPortsAvailable ? "yes" : "no")
                    << ", host process request: " << (hostState.processRequested ? "yes" : "no")
                    << ".";
            infoState.message = message.str();
            return true;
        }

        ClapLiveEffectRealtimeProcessResult processPlanarBlock(const float* const* inputChannelData,
                                                                     int suppliedInputChannelCount,
                                                                     float* const* outputChannelData,
                                                                     int suppliedOutputChannelCount,
                                                                     int frameCount) noexcept
        {
            ClapLiveEffectRealtimeProcessResult result;
            if (!isOpen() || plugin == nullptr || plugin->process == nullptr || !realtimeStorageReady)
                return result;
            if (frameCount <= 0 || frameCount > infoState.blockSize || suppliedOutputChannelCount <= 0 || outputChannelData == nullptr)
                return result;

            const auto frames = frameCount;
            const auto pluginInputChannels = std::max(0, inputChannelCount);
            const auto pluginOutputChannels = std::max(1, outputChannelCount);

            if (pluginInputChannels > 0)
            {
                if (static_cast<int>(realtimeInputStorage.samples.size()) < pluginInputChannels)
                    return result;

                for (int channel = 0; channel < pluginInputChannels; ++channel)
                {
                    auto& destination = realtimeInputStorage.samples[static_cast<std::size_t>(channel)];
                    const float* source = nullptr;
                    if (inputChannelData != nullptr && suppliedInputChannelCount > 0)
                        source = inputChannelData[std::min(channel, suppliedInputChannelCount - 1)];

                    if (source != nullptr)
                        std::copy_n(source, frames, destination.begin());
                    else
                        std::fill_n(destination.begin(), frames, 0.0f);
                }
            }

            if (static_cast<int>(realtimeOutputStorage.samples.size()) < pluginOutputChannels)
                return result;

            for (int channel = 0; channel < pluginOutputChannels; ++channel)
                std::fill_n(realtimeOutputStorage.samples[static_cast<std::size_t>(channel)].begin(), frames, 0.0f);

            if (pluginInputChannels > 0)
                primeOutputsFromInputs(realtimeInputStorage, realtimeOutputStorage, frames);

            clap_input_events_t inputEvents {};
            inputEvents.ctx = nullptr;
            inputEvents.size = emptyInputEventsSize;
            inputEvents.get = emptyInputEventsGet;

            clap_output_events_t outputEvents {};
            outputEvents.ctx = nullptr;
            outputEvents.try_push = outputEventsTryPush;

            clap_process_t process {};
            process.frames_count = static_cast<std::uint32_t>(frames);
            process.audio_inputs = pluginInputChannels > 0 ? &realtimeInputStorage.buffer : nullptr;
            process.audio_inputs_count = pluginInputChannels > 0 ? 1u : 0u;
            process.audio_outputs = &realtimeOutputStorage.buffer;
            process.audio_outputs_count = 1;
            process.in_events = &inputEvents;
            process.out_events = &outputEvents;

            int status = clapProcessError;
            try
            {
                status = plugin->process(plugin, &process);
            }
            catch (...)
            {
                status = clapProcessError;
            }

            result.processStatus = status;
            result.outputChannelCount = pluginOutputChannels;
            result.success = status != clapProcessError;
            ++infoState.processedBlocks;
            infoState.lastProcessStatus = status;

            if (!result.success)
                return result;

            for (int channel = 0; channel < suppliedOutputChannelCount; ++channel)
            {
                auto* destination = outputChannelData[channel];
                if (destination == nullptr)
                    continue;

                const auto sourceChannel = std::min(channel, pluginOutputChannels - 1);
                const auto& source = realtimeOutputStorage.samples[static_cast<std::size_t>(sourceChannel)];
                for (int frame = 0; frame < frames; ++frame)
                {
                    const auto sample = source[static_cast<std::size_t>(frame)];
                    destination[frame] = std::isfinite(sample) ? sample : 0.0f;
                }
            }

            return result;
        }

        ClapLiveEffectProcessResult processBlock(const ClapLiveEffectProcessRequest& request)
        {
            ClapLiveEffectProcessResult result;
            if (!isOpen() || plugin == nullptr || plugin->process == nullptr)
            {
                result.message = "CLAP live effect process failed: no open live effect instance.";
                return result;
            }

            const int frames = std::max(1, std::min(request.frameCount > 0 ? request.frameCount : infoState.blockSize, std::max(1, infoState.blockSize)));
            const int inputChannels = std::max(1, std::min(request.inputChannelCount > 0 ? request.inputChannelCount : infoState.inputChannelCount, 32));
            const int storageInputChannels = std::max(0, infoState.inputChannelCount);

            LiveAudioStorage inputStorage;
            if (storageInputChannels > 0)
            {
                inputStorage.build(storageInputChannels, frames);
                copyInterleavedToStorage(request.interleavedInputAudio, inputChannels, inputStorage, frames);
            }

            LiveAudioStorage outputStorage;
            outputStorage.build(std::max(1, outputChannelCount), frames);
            if (storageInputChannels > 0)
                primeOutputsFromInputs(inputStorage, outputStorage, frames);

            clap_input_events_t inputEvents {};
            inputEvents.ctx = nullptr;
            inputEvents.size = emptyInputEventsSize;
            inputEvents.get = emptyInputEventsGet;

            clap_output_events_t outputEvents {};
            outputEvents.ctx = nullptr;
            outputEvents.try_push = outputEventsTryPush;

            clap_process_t process {};
            process.frames_count = static_cast<std::uint32_t>(frames);
            process.audio_inputs = storageInputChannels > 0 ? &inputStorage.buffer : nullptr;
            process.audio_inputs_count = storageInputChannels > 0 ? 1u : 0u;
            process.audio_outputs = &outputStorage.buffer;
            process.audio_outputs_count = 1;
            process.in_events = &inputEvents;
            process.out_events = &outputEvents;

            const auto status = plugin->process(plugin, &process);
            result.processStatus = status;
            result.processStatusText = processStatusToString(status);
            result.success = status != clapProcessError;
            result.inputChannelCount = inputChannels;
            result.outputChannelCount = outputChannelCount;
            result.outputFrameCount = frames;
            result.interleavedAudio = storageToInterleaved(outputStorage, frames);

            ++infoState.processedBlocks;
            infoState.lastProcessStatus = status;
            infoState.lastProcessStatusText = result.processStatusText;

            std::ostringstream message;
            message << "CLAP live effect block " << infoState.processedBlocks
                    << ": frames " << frames
                    << ", input channels " << inputChannels
                    << ", output channels " << outputChannelCount
                    << ", process status " << result.processStatusText
                    << ".";
            result.message = message.str();
            infoState.message = result.message;
            return result;
        }

        void close()
        {
            if (plugin != nullptr && startedProcessing && plugin->stop_processing != nullptr)
            {
                plugin->stop_processing(plugin);
                startedProcessing = false;
            }
            if (plugin != nullptr && activated && plugin->deactivate != nullptr)
            {
                plugin->deactivate(plugin);
                activated = false;
            }
            if (plugin != nullptr && plugin->destroy != nullptr)
            {
                plugin->destroy(plugin);
                plugin = nullptr;
            }
            if (entryInitialized && entry != nullptr && entry->deinit != nullptr)
            {
                entry->deinit();
                entryInitialized = false;
                entry = nullptr;
            }
            library.reset();
            realtimeInputStorage = {};
            realtimeOutputStorage = {};
            realtimeStorageReady = false;
            pluginInitialized = false;
            infoState.open = false;
            infoState.startedProcessing = false;
            if (!infoState.message.empty())
                infoState.message += " Closed.";
        }

        bool isOpen() const
        {
            return infoState.open && plugin != nullptr && startedProcessing;
        }

        ClapLiveEffectSessionInfo info() const
        {
            return infoState;
        }

        bool fail(std::string& error, std::string message)
        {
            close();
            error = std::move(message);
            infoState.message = error;
            return false;
        }

        ClapLiveEffectSessionConfig config;
        ClapLiveEffectSessionInfo infoState;
        std::unique_ptr<DynamicLibrary> library;
        const clap_plugin_entry_t* entry = nullptr;
        const clap_plugin_t* plugin = nullptr;
        clap_host_t host {};
        ClapHostRequestState hostState;
        int inputChannelCount = 2;
        int outputChannelCount = 2;
        LiveAudioStorage realtimeInputStorage;
        LiveAudioStorage realtimeOutputStorage;
        bool realtimeStorageReady = false;
        bool entryInitialized = false;
        bool pluginInitialized = false;
        bool activated = false;
        bool startedProcessing = false;
    };

    ClapLiveEffectSession::ClapLiveEffectSession()
        : impl(std::make_unique<Impl>())
    {
    }

    ClapLiveEffectSession::~ClapLiveEffectSession() = default;

    bool ClapLiveEffectSession::open(const ClapLiveEffectSessionConfig& config, std::string& errorMessage)
    {
        return impl->open(config, errorMessage);
    }

    void ClapLiveEffectSession::close()
    {
        impl->close();
    }

    bool ClapLiveEffectSession::isOpen() const
    {
        return impl->isOpen();
    }

    ClapLiveEffectSessionInfo ClapLiveEffectSession::info() const
    {
        return impl->info();
    }

    ClapLiveEffectProcessResult ClapLiveEffectSession::processBlock(const ClapLiveEffectProcessRequest& request)
    {
        return impl->processBlock(request);
    }

    ClapLiveEffectRealtimeProcessResult ClapLiveEffectSession::processPlanarBlock(const float* const* inputChannelData,
                                                                                  int inputChannelCount,
                                                                                  float* const* outputChannelData,
                                                                                  int outputChannelCount,
                                                                                  int frameCount) noexcept
    {
        return impl->processPlanarBlock(inputChannelData, inputChannelCount, outputChannelData, outputChannelCount, frameCount);
    }
}
