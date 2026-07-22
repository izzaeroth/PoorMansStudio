#include "clap/ClapAbiProbe.h"

#include "clap/ClapPluginScanner.h"

#include <algorithm>
#include <cstdint>
#include <cctype>
#include <cmath>
#include <exception>
#include <initializer_list>
#include <limits>
#include <string>
#include <system_error>
#include <cstring>
#include <iomanip>
#include <sstream>
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
    constexpr const char* clapNotePortsExtensionId = "clap.note-ports";
    constexpr const char* clapLatencyExtensionId = "clap.latency";
    constexpr const char* clapTailExtensionId = "clap.tail";
    constexpr const char* clapParamsExtensionId = "clap.params";
    constexpr const char* clapStateExtensionId = "clap.state";
    constexpr const char* clapGuiExtensionId = "clap.gui";
    constexpr const char* clapThreadCheckExtensionId = "clap.thread-check";
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
    struct clap_host_t;

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

    struct clap_note_port_info_t
    {
        clap_id id;
        std::uint32_t supported_dialects;
        std::uint32_t preferred_dialect;
        char name[clapNameSize];
    };

    struct clap_plugin_note_ports_t
    {
        std::uint32_t (*count)(const clap_plugin_t* plugin, bool is_input);
        bool (*get)(const clap_plugin_t* plugin, std::uint32_t index, bool is_input, clap_note_port_info_t* info);
    };

    struct clap_plugin_latency_t
    {
        std::uint32_t (*get)(const clap_plugin_t* plugin);
    };

    struct clap_plugin_tail_t
    {
        std::uint32_t (*get)(const clap_plugin_t* plugin);
    };

    struct clap_plugin_params_t
    {
        std::uint32_t (*count)(const clap_plugin_t* plugin);
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

    struct clap_host_thread_check_t
    {
        bool (*is_main_thread)(const clap_host_t* host);
        bool (*is_audio_thread)(const clap_host_t* host);
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

    struct ClapOutputEventState
    {
        int pushedEvents = 0;
    };

    struct SilentAudioBufferStorage
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
                buffers[port].constant_mask = channels >= 64 ? std::numeric_limits<std::uint64_t>::max() : ((channels == 0) ? 0 : ((std::uint64_t { 1 } << channels) - 1));
            }
        }

        std::pair<double, int> analyze() const
        {
            double maximum = 0.0;
            int nonFinite = 0;
            for (const auto& port : samples)
            {
                for (const auto& channel : port)
                {
                    for (const auto sample : channel)
                    {
                        if (!std::isfinite(sample))
                        {
                            ++nonFinite;
                            continue;
                        }
                        maximum = std::max(maximum, static_cast<double>(std::abs(sample)));
                    }
                }
            }
            return { maximum, nonFinite };
        }
    };

    struct ClapHostRequestState
    {
        bool restartRequested = false;
        bool processRequested = false;
        bool callbackRequested = false;
        bool audioThreadPhase = false;
        bool threadCheckRequested = false;
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

    std::string safeString(const char* value)
    {
        return value == nullptr ? std::string() : std::string(value);
    }

    std::string safeFixedString(const char* value, std::size_t maxLength)
    {
        if (value == nullptr || maxLength == 0)
            return {};

        std::size_t length = 0;
        while (length < maxLength && value[length] != '\0')
            ++length;

        return std::string(value, length);
    }

    std::string lowerCopy(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return value;
    }

    bool equalsAny(const std::string& value, std::initializer_list<const char*> options)
    {
        const auto lower = lowerCopy(value);
        for (const auto* option : options)
            if (lower == option)
                return true;
        return false;
    }

#if defined(_WIN32)
    std::string wideToUtf8(const std::wstring& value)
    {
        if (value.empty())
            return {};

        const auto requiredBytes = ::WideCharToMultiByte(CP_UTF8,
                                                         0,
                                                         value.c_str(),
                                                         static_cast<int>(value.size()),
                                                         nullptr,
                                                         0,
                                                         nullptr,
                                                         nullptr);
        if (requiredBytes <= 0)
            return {};

        std::string result(static_cast<size_t>(requiredBytes), '\0');
        const auto convertedBytes = ::WideCharToMultiByte(CP_UTF8,
                                                          0,
                                                          value.c_str(),
                                                          static_cast<int>(value.size()),
                                                          result.data(),
                                                          requiredBytes,
                                                          nullptr,
                                                          nullptr);
        if (convertedBytes <= 0)
            return {};

        result.resize(static_cast<size_t>(convertedBytes));
        return result;
    }
#endif

    bool containsAnyFeature(const std::vector<std::string>& features, std::initializer_list<const char*> options)
    {
        for (const auto& feature : features)
            if (equalsAny(feature, options))
                return true;
        return false;
    }

    mw::clap::ClapPluginKind kindFromFeatures(const std::vector<std::string>& features)
    {
        if (containsAnyFeature(features, { "instrument", "synthesizer", "sampler" }))
            return mw::clap::ClapPluginKind::Instrument;

        if (containsAnyFeature(features, { "note-effect", "note_detector" }))
            return mw::clap::ClapPluginKind::MidiTool;

        if (containsAnyFeature(features, { "audio-effect", "analyzer", "delay", "reverb", "filter", "phaser", "equalizer", "compressor", "limiter", "distortion", "chorus" }))
            return mw::clap::ClapPluginKind::Effect;

        return mw::clap::ClapPluginKind::Unknown;
    }

    std::vector<std::string> readFeatures(const clap_plugin_descriptor_t& desc)
    {
        std::vector<std::string> result;
        if (desc.features == nullptr)
            return result;

        for (const char* const* feature = desc.features; *feature != nullptr; ++feature)
        {
            const auto value = safeString(*feature);
            if (!value.empty())
                result.push_back(value);
        }

        return result;
    }

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
            return wideToUtf8(message);
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

    std::string versionText(const clap_version_t& version)
    {
        return std::to_string(version.major) + "." + std::to_string(version.minor) + "." + std::to_string(version.revision);
    }

    void populateDescriptorFromClapDescriptor(mw::clap::ClapPluginDescriptor& descriptor,
                                              const clap_plugin_descriptor_t& desc,
                                              std::uint32_t pluginCount)
    {
        descriptor.uid = safeString(desc.id);
        descriptor.name = safeString(desc.name);
        descriptor.vendor = safeString(desc.vendor);
        descriptor.version = safeString(desc.version);
        descriptor.description = safeString(desc.description);
        descriptor.category = "CLAP " + versionText(desc.clap_version);
        descriptor.features = readFeatures(desc);
        descriptor.detectedKind = kindFromFeatures(descriptor.features);
        descriptor.kind = descriptor.detectedKind;
        descriptor.clapPluginCount = static_cast<int>(pluginCount);
        descriptor.metadataOnly = false;
        descriptor.abiProbed = true;
        descriptor.status = mw::clap::ClapPluginScanStatus::Candidate;
        descriptor.classificationReason = descriptor.detectedKind == mw::clap::ClapPluginKind::Unknown
            ? "CLAP descriptor did not expose a recognized instrument/effect feature; use the manager to catalog it if needed."
            : "CLAP descriptor features classified this plugin as " + mw::clap::clapPluginKindToString(descriptor.detectedKind) + ".";
    }


    std::string audioPortSummary(const clap_audio_port_info_t& info, std::uint32_t index, bool isInput)
    {
        auto name = safeFixedString(info.name, clapNameSize);
        if (name.empty())
            name = std::string(isInput ? "input" : "output") + " " + std::to_string(index);

        std::string result = name + " (channels=" + std::to_string(info.channel_count);
        const auto type = safeString(info.port_type);
        if (!type.empty())
            result += ", type=" + type;
        result += ", id=" + std::to_string(info.id) + ")";
        return result;
    }

    std::string notePortSummary(const clap_note_port_info_t& info, std::uint32_t index, bool isInput)
    {
        auto name = safeFixedString(info.name, clapNameSize);
        if (name.empty())
            name = std::string(isInput ? "input" : "output") + " " + std::to_string(index);

        return name + " (supportedDialects=" + std::to_string(info.supported_dialects)
            + ", preferredDialect=" + std::to_string(info.preferred_dialect)
            + ", id=" + std::to_string(info.id) + ")";
    }

    template <typename Result>
    void queryAudioPorts(const clap_plugin_t* plugin, Result& result)
    {
        if (plugin == nullptr || plugin->get_extension == nullptr)
        {
            result.audioPortsMessage = "Plugin does not expose get_extension(), so audio ports could not be queried.";
            return;
        }

        const auto* extension = static_cast<const clap_plugin_audio_ports_t*>(plugin->get_extension(plugin, clapAudioPortsExtensionId));
        if (extension == nullptr)
        {
            result.audioPortsMessage = "CLAP audio-ports extension is not exposed.";
            return;
        }

        result.audioPortsExtensionAvailable = true;
        if (extension->count == nullptr || extension->get == nullptr)
        {
            result.audioPortsMessage = "CLAP audio-ports extension is incomplete.";
            return;
        }

        const auto inputCount = extension->count(plugin, true);
        const auto outputCount = extension->count(plugin, false);
        result.audioInputPortCount = static_cast<int>(inputCount);
        result.audioOutputPortCount = static_cast<int>(outputCount);

        for (std::uint32_t i = 0; i < inputCount; ++i)
        {
            clap_audio_port_info_t info {};
            if (extension->get(plugin, i, true, &info))
                result.audioInputPorts.push_back(audioPortSummary(info, i, true));
        }

        for (std::uint32_t i = 0; i < outputCount; ++i)
        {
            clap_audio_port_info_t info {};
            if (extension->get(plugin, i, false, &info))
                result.audioOutputPorts.push_back(audioPortSummary(info, i, false));
        }

        result.audioPortsMessage = "CLAP audio-ports extension was queried.";
    }

    template <typename Result>
    void queryNotePorts(const clap_plugin_t* plugin, Result& result)
    {
        if (plugin == nullptr || plugin->get_extension == nullptr)
        {
            result.notePortsMessage = "Plugin does not expose get_extension(), so note ports could not be queried.";
            return;
        }

        const auto* extension = static_cast<const clap_plugin_note_ports_t*>(plugin->get_extension(plugin, clapNotePortsExtensionId));
        if (extension == nullptr)
        {
            result.notePortsMessage = "CLAP note-ports extension is not exposed.";
            return;
        }

        result.notePortsExtensionAvailable = true;
        if (extension->count == nullptr || extension->get == nullptr)
        {
            result.notePortsMessage = "CLAP note-ports extension is incomplete.";
            return;
        }

        const auto inputCount = extension->count(plugin, true);
        const auto outputCount = extension->count(plugin, false);
        result.noteInputPortCount = static_cast<int>(inputCount);
        result.noteOutputPortCount = static_cast<int>(outputCount);

        for (std::uint32_t i = 0; i < inputCount; ++i)
        {
            clap_note_port_info_t info {};
            if (extension->get(plugin, i, true, &info))
                result.noteInputPorts.push_back(notePortSummary(info, i, true));
        }

        for (std::uint32_t i = 0; i < outputCount; ++i)
        {
            clap_note_port_info_t info {};
            if (extension->get(plugin, i, false, &info))
                result.noteOutputPorts.push_back(notePortSummary(info, i, false));
        }

        result.notePortsMessage = "CLAP note-ports extension was queried.";
    }

    template <typename Result>
    void queryCapabilities(const clap_plugin_t* plugin, Result& result, bool queryActiveLatency)
    {
        if (plugin == nullptr || plugin->get_extension == nullptr)
            return;

        if (const auto* latency = static_cast<const clap_plugin_latency_t*>(plugin->get_extension(plugin, clapLatencyExtensionId)))
        {
            result.latencyExtensionAvailable = true;
            if (queryActiveLatency && latency->get != nullptr)
            {
                result.latencySamples = static_cast<std::int64_t>(latency->get(plugin));
                result.latencyValueQueried = true;
            }
        }

        if (const auto* tail = static_cast<const clap_plugin_tail_t*>(plugin->get_extension(plugin, clapTailExtensionId)))
        {
            result.tailExtensionAvailable = true;
            if (tail->get != nullptr)
            {
                const auto tailSamples = tail->get(plugin);
                result.tailSamples = static_cast<std::int64_t>(tailSamples);
                result.tailValueQueried = true;
                result.tailInfinite = tailSamples >= static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max());
            }
        }

        if (const auto* params = static_cast<const clap_plugin_params_t*>(plugin->get_extension(plugin, clapParamsExtensionId)))
        {
            result.paramsExtensionAvailable = true;
            if (params->count != nullptr)
            {
                result.parameterCount = static_cast<int>(params->count(plugin));
                result.parameterCountQueried = true;
            }
        }

        result.stateExtensionAvailable = plugin->get_extension(plugin, clapStateExtensionId) != nullptr;
        result.guiExtensionAvailable = plugin->get_extension(plugin, clapGuiExtensionId) != nullptr;
    }

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

    struct StateWriteStorage
    {
        std::vector<std::uint8_t> bytes;
    };

    std::int64_t stateWrite(const clap_ostream_t* stream, const void* buffer, std::uint64_t size)
    {
        if (stream == nullptr || stream->ctx == nullptr || (buffer == nullptr && size > 0))
            return -1;
        if (size == 0)
            return 0;
        if (size > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())
            || size > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            return -1;

        auto* storage = static_cast<StateWriteStorage*>(stream->ctx);
        const auto* begin = static_cast<const std::uint8_t*>(buffer);
        try
        {
            storage->bytes.insert(storage->bytes.end(), begin, begin + static_cast<std::size_t>(size));
        }
        catch (...)
        {
            // Never allow an allocation exception to cross the CLAP C callback boundary.
            return -1;
        }
        return static_cast<std::int64_t>(size);
    }

    struct StateReadStorage
    {
        const std::vector<std::uint8_t>* bytes = nullptr;
        std::size_t offset = 0;
    };

    std::int64_t stateRead(const clap_istream_t* stream, void* buffer, std::uint64_t size)
    {
        if (stream == nullptr || stream->ctx == nullptr || (buffer == nullptr && size > 0))
            return -1;
        auto* storage = static_cast<StateReadStorage*>(stream->ctx);
        if (storage->bytes == nullptr)
            return -1;
        const auto remaining = storage->bytes->size() - std::min(storage->offset, storage->bytes->size());
        const auto requested = size > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())
            ? std::numeric_limits<std::size_t>::max()
            : static_cast<std::size_t>(size);
        const auto count = std::min(remaining, requested);
        if (count > 0)
            std::memcpy(buffer, storage->bytes->data() + storage->offset, count);
        storage->offset += count;
        return static_cast<std::int64_t>(count);
    }

    bool hostIsMainThread(const clap_host_t* host)
    {
        if (host == nullptr)
            return false;
        if (auto* state = static_cast<ClapHostRequestState*>(host->host_data))
        {
            state->threadCheckRequested = true;
            return !state->audioThreadPhase;
        }
        return false;
    }

    bool hostIsAudioThread(const clap_host_t* host)
    {
        if (host == nullptr)
            return false;
        if (auto* state = static_cast<ClapHostRequestState*>(host->host_data))
        {
            state->threadCheckRequested = true;
            return state->audioThreadPhase;
        }
        return false;
    }

    const clap_host_thread_check_t hostThreadCheckExtension { hostIsMainThread, hostIsAudioThread };

    const void* hostGetExtension(const clap_host_t*, const char* extensionId)
    {
        if (extensionId != nullptr && std::strcmp(extensionId, clapThreadCheckExtensionId) == 0)
            return &hostThreadCheckExtension;
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

    std::vector<std::uint32_t> collectAudioPortChannelCounts(const clap_plugin_t* plugin, bool isInput)
    {
        std::vector<std::uint32_t> channels;
        if (plugin == nullptr || plugin->get_extension == nullptr)
            return channels;

        const auto* extension = static_cast<const clap_plugin_audio_ports_t*>(plugin->get_extension(plugin, clapAudioPortsExtensionId));
        if (extension == nullptr || extension->count == nullptr || extension->get == nullptr)
            return channels;

        constexpr std::uint32_t maxPortsForSmokeTest = 16;
        constexpr std::uint32_t maxChannelsPerPortForSmokeTest = 32;
        const auto count = std::min(extension->count(plugin, isInput), maxPortsForSmokeTest);
        channels.reserve(count);

        for (std::uint32_t i = 0; i < count; ++i)
        {
            clap_audio_port_info_t info {};
            if (extension->get(plugin, i, isInput, &info))
                channels.push_back(std::min(info.channel_count, maxChannelsPerPortForSmokeTest));
            else
                channels.push_back(0);
        }

        return channels;
    }

    int totalChannels(const std::vector<std::uint32_t>& ports)
    {
        int total = 0;
        for (const auto count : ports)
            total += static_cast<int>(count);
        return total;
    }
}

namespace mw::clap
{
    ClapAbiProbeResult ClapAbiProbe::probePluginPath(const std::filesystem::path& outerPluginPath,
                                                       int pluginIndex)
    {
        ClapAbiProbeResult result;
        result.attempted = true;
        result.selectedIndex = pluginIndex < 0 ? 0 : pluginIndex;
        result.descriptor = ClapPluginScanner::inspectPluginPath(outerPluginPath);
        result.descriptor.metadataOnly = true;
        result.descriptor.abiProbed = false;

        try
        {
            std::error_code ec;
            if (!std::filesystem::exists(outerPluginPath, ec))
            {
                result.message = "CLAP plugin path was not found.";
                result.descriptor.status = ClapPluginScanStatus::Missing;
                result.descriptor.statusMessage = result.message;
                return result;
            }

            if (!ClapPluginScanner::isOuterClapPlugin(outerPluginPath))
            {
                result.message = "Path is not an outer .clap plugin file or bundle.";
                result.descriptor.status = ClapPluginScanStatus::Unsupported;
                result.descriptor.statusMessage = result.message;
                return result;
            }

            if (result.descriptor.binaryPath.empty())
            {
                result.message = "Could not find a loadable CLAP binary inside the plugin path.";
                result.descriptor.status = ClapPluginScanStatus::Unsupported;
                result.descriptor.statusMessage = result.message;
                return result;
            }

            DynamicLibrary library(result.descriptor.binaryPath);
            if (!library.isLoaded())
            {
                result.message = "Could not load CLAP binary";
                const auto error = library.lastErrorText();
                if (!error.empty())
                    result.message += ": " + error;
                result.descriptor.status = ClapPluginScanStatus::Unsupported;
                result.descriptor.statusMessage = result.message;
                return result;
            }
            result.loadedLibrary = true;

            auto* entry = static_cast<const clap_plugin_entry_t*>(library.symbol("clap_entry"));
            if (entry == nullptr)
            {
                result.message = "Loaded binary but did not find exported CLAP entry symbol 'clap_entry'.";
                result.descriptor.status = ClapPluginScanStatus::Unsupported;
                result.descriptor.statusMessage = result.message;
                return result;
            }
            result.foundEntry = true;
            result.descriptor.clapVersionMajor = entry->clap_version.major;
            result.descriptor.clapVersionMinor = entry->clap_version.minor;
            result.descriptor.clapVersionRevision = entry->clap_version.revision;

            if (entry->init == nullptr || entry->deinit == nullptr || entry->get_factory == nullptr)
            {
                result.message = "CLAP entry is incomplete.";
                result.descriptor.status = ClapPluginScanStatus::Unsupported;
                result.descriptor.statusMessage = result.message;
                return result;
            }

            if (!entry->init(outerPluginPath.string().c_str()))
            {
                result.message = "CLAP entry init() returned false.";
                result.descriptor.status = ClapPluginScanStatus::Unsupported;
                result.descriptor.statusMessage = result.message;
                return result;
            }
            result.initialized = true;

            struct EntryDeinitGuard
            {
                const clap_plugin_entry_t* entry = nullptr;
                ~EntryDeinitGuard()
                {
                    if (entry != nullptr && entry->deinit != nullptr)
                        entry->deinit();
                }
            } guard { entry };

            const auto* factoryVoid = entry->get_factory(clapPluginFactoryId);
            const auto* factory = static_cast<const clap_plugin_factory_t*>(factoryVoid);
            if (factory == nullptr || factory->get_plugin_count == nullptr || factory->get_plugin_descriptor == nullptr)
            {
                result.message = "CLAP plugin factory was not available.";
                result.descriptor.status = ClapPluginScanStatus::Unsupported;
                result.descriptor.statusMessage = result.message;
                return result;
            }
            result.foundFactory = true;

            const auto count = factory->get_plugin_count(factory);
            result.pluginCount = static_cast<int>(count);
            result.descriptor.clapPluginCount = result.pluginCount;
            if (count == 0)
            {
                result.message = "CLAP plugin factory reported zero plugins.";
                result.descriptor.status = ClapPluginScanStatus::Unsupported;
                result.descriptor.statusMessage = result.message;
                return result;
            }

            if (result.selectedIndex >= static_cast<int>(count))
            {
                result.message = "Requested CLAP plugin index is outside the bundle's descriptor range.";
                result.descriptor.status = ClapPluginScanStatus::Unsupported;
                result.descriptor.statusMessage = result.message;
                return result;
            }

            const auto* desc = factory->get_plugin_descriptor(factory, static_cast<std::uint32_t>(result.selectedIndex));
            if (desc == nullptr)
            {
                result.message = "CLAP plugin factory returned a null descriptor for the selected plugin index.";
                result.descriptor.status = ClapPluginScanStatus::Unsupported;
                result.descriptor.statusMessage = result.message;
                return result;
            }
            result.foundDescriptor = true;

            populateDescriptorFromClapDescriptor(result.descriptor, *desc, count);
            result.descriptor.statusMessage = count > 1
                ? "CLAP ABI probe succeeded. This bundle exposes " + std::to_string(count) + " plugin descriptors; selected descriptor " + std::to_string(result.selectedIndex + 1) + "."
                : "CLAP ABI probe succeeded. Descriptor metadata was read without creating a plugin instance.";
            result.message = result.descriptor.statusMessage;
            return result;
        }
        catch (const std::exception& e)
        {
            result.message = e.what();
            result.descriptor.status = ClapPluginScanStatus::Unsupported;
            result.descriptor.statusMessage = result.message;
            return result;
        }
        catch (...)
        {
            result.message = "Unknown CLAP ABI probe exception.";
            result.descriptor.status = ClapPluginScanStatus::Unsupported;
            result.descriptor.statusMessage = result.message;
            return result;
        }
    }

    ClapInstanceValidationResult ClapAbiProbe::validatePluginInstance(const std::filesystem::path& outerPluginPath,
                                                                      int pluginIndex)
    {
        ClapInstanceValidationResult result;
        result.attempted = true;
        result.selectedIndex = pluginIndex < 0 ? 0 : pluginIndex;
        result.descriptor = ClapPluginScanner::inspectPluginPath(outerPluginPath);
        result.descriptor.metadataOnly = true;
        result.descriptor.abiProbed = false;

        const clap_plugin_entry_t* initializedEntry = nullptr;
        const clap_plugin_t* createdPlugin = nullptr;
        ClapHostRequestState hostState;

        auto cleanup = [&result, &initializedEntry, &createdPlugin, &hostState]()
        {
            if (createdPlugin != nullptr && createdPlugin->destroy != nullptr && !result.instanceDestroyed)
            {
                createdPlugin->destroy(createdPlugin);
                createdPlugin = nullptr;
                result.instanceDestroyed = true;
            }

            if (initializedEntry != nullptr && initializedEntry->deinit != nullptr && !result.entryDeinitialized)
            {
                initializedEntry->deinit();
                initializedEntry = nullptr;
                result.entryDeinitialized = true;
            }

            result.restartRequested = hostState.restartRequested;
            result.processRequested = hostState.processRequested;
            result.callbackRequested = hostState.callbackRequested;
            result.hostThreadCheckRequested = hostState.threadCheckRequested;
        };

        auto fail = [&result, &cleanup](std::string stage, std::string message) -> ClapInstanceValidationResult&
        {
            cleanup();
            result.stage = std::move(stage);
            result.message = std::move(message);
            result.descriptor.status = ClapPluginScanStatus::Unsupported;
            result.descriptor.statusMessage = result.message;
            return result;
        };

        try
        {
            std::error_code ec;
            if (!std::filesystem::exists(outerPluginPath, ec))
            {
                result.descriptor.status = ClapPluginScanStatus::Missing;
                result.stage = "path";
                result.message = "CLAP plugin path was not found.";
                result.descriptor.statusMessage = result.message;
                return result;
            }

            if (!ClapPluginScanner::isOuterClapPlugin(outerPluginPath))
                return fail("path", "Path is not an outer .clap plugin file or bundle.");

            if (result.descriptor.binaryPath.empty())
                return fail("binary", "Could not find a loadable CLAP binary inside the plugin path.");

            DynamicLibrary library(result.descriptor.binaryPath);
            if (!library.isLoaded())
            {
                std::string message = "Could not load CLAP binary";
                const auto error = library.lastErrorText();
                if (!error.empty())
                    message += ": " + error;
                return fail("load_library", std::move(message));
            }
            result.loadedLibrary = true;

            auto* entry = static_cast<const clap_plugin_entry_t*>(library.symbol("clap_entry"));
            if (entry == nullptr)
                return fail("entry", "Loaded binary but did not find exported CLAP entry symbol 'clap_entry'.");
            result.foundEntry = true;
            result.descriptor.clapVersionMajor = entry->clap_version.major;
            result.descriptor.clapVersionMinor = entry->clap_version.minor;
            result.descriptor.clapVersionRevision = entry->clap_version.revision;

            if (entry->init == nullptr || entry->deinit == nullptr || entry->get_factory == nullptr)
                return fail("entry", "CLAP entry is incomplete.");

            if (!entry->init(outerPluginPath.string().c_str()))
                return fail("entry_init", "CLAP entry init() returned false.");
            result.entryInitialized = true;
            initializedEntry = entry;

            const auto* factoryVoid = entry->get_factory(clapPluginFactoryId);
            const auto* factory = static_cast<const clap_plugin_factory_t*>(factoryVoid);
            if (factory == nullptr || factory->get_plugin_count == nullptr || factory->get_plugin_descriptor == nullptr || factory->create_plugin == nullptr)
                return fail("factory", "CLAP plugin factory was not available or did not expose create_plugin().");
            result.foundFactory = true;

            const auto count = factory->get_plugin_count(factory);
            result.pluginCount = static_cast<int>(count);
            result.descriptor.clapPluginCount = result.pluginCount;
            if (count == 0)
                return fail("factory", "CLAP plugin factory reported zero plugins.");

            const auto selectedIndex = static_cast<std::uint32_t>(result.selectedIndex);
            if (selectedIndex >= count)
                return fail("descriptor", "Requested CLAP plugin index is outside the plugin factory descriptor range.");

            const auto* desc = factory->get_plugin_descriptor(factory, selectedIndex);
            if (desc == nullptr)
                return fail("descriptor", "CLAP plugin factory returned a null descriptor for the selected plugin index.");
            result.foundDescriptor = true;

            populateDescriptorFromClapDescriptor(result.descriptor, *desc, count);
            result.descriptor.statusMessage = "CLAP instance validation is running in the helper process.";

            if (desc->id == nullptr || std::string(desc->id).empty())
                return fail("descriptor", "CLAP descriptor did not provide a plugin id for create_plugin().");

            clap_host_t host {};
            host.clap_version = { 1, 1, 0 };
            host.host_data = &hostState;
            host.name = "Poor Man's Studio CLAP Host Helper";
            host.vendor = "Poor Man's Studio";
            host.url = "";
            host.version = "0.66.6";
            host.get_extension = hostGetExtension;
            host.request_restart = hostRequestRestart;
            host.request_process = hostRequestProcess;
            host.request_callback = hostRequestCallback;

            const auto* pluginVoid = factory->create_plugin(factory, &host, desc->id);
            const auto* plugin = static_cast<const clap_plugin_t*>(pluginVoid);
            if (plugin == nullptr)
                return fail("create_plugin", "CLAP plugin factory returned a null plugin instance.");
            result.instanceCreated = true;
            createdPlugin = plugin;

            if (plugin->destroy == nullptr)
                return fail("plugin_destroy", "CLAP plugin instance did not expose destroy().");

            if (plugin->init == nullptr)
                return fail("plugin_init", "CLAP plugin instance did not expose init().");

            if (!plugin->init(plugin))
                return fail("plugin_init", "CLAP plugin init() returned false.");
            result.pluginInitialized = true;
            queryCapabilities(plugin, result, false);

            result.restartRequested = hostState.restartRequested;
            result.processRequested = hostState.processRequested;
            result.callbackRequested = hostState.callbackRequested;
            result.hostThreadCheckRequested = hostState.threadCheckRequested;

            result.stage = "ok";
            result.message = count > 1
                ? "CLAP instance validation succeeded. The selected plugin instance was created, initialized, destroyed, and the CLAP entry was deinitialized. This bundle exposes " + std::to_string(count) + " plugin descriptors."
                : "CLAP instance validation succeeded. The plugin instance was created, initialized, destroyed, and the CLAP entry was deinitialized.";
            result.descriptor.status = ClapPluginScanStatus::Candidate;
            result.descriptor.statusMessage = result.message;
            cleanup();
            return result;
        }
        catch (const std::exception& e)
        {
            cleanup();
            result.message = e.what();
            result.stage = result.stage.empty() ? "exception" : result.stage;
            result.descriptor.status = ClapPluginScanStatus::Unsupported;
            result.descriptor.statusMessage = result.message;
            return result;
        }
        catch (...)
        {
            cleanup();
            result.message = "Unknown CLAP instance validation exception.";
            result.stage = result.stage.empty() ? "exception" : result.stage;
            result.descriptor.status = ClapPluginScanStatus::Unsupported;
            result.descriptor.statusMessage = result.message;
            return result;
        }
    }

    ClapActivationValidationResult ClapAbiProbe::validatePluginActivation(const std::filesystem::path& outerPluginPath,
                                                                          int pluginIndex,
                                                                          double sampleRate,
                                                                          int minFrames,
                                                                          int maxFrames)
    {
        ClapActivationValidationResult result;
        result.attempted = true;
        result.selectedIndex = pluginIndex < 0 ? 0 : pluginIndex;
        result.sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
        result.minFrames = minFrames > 0 ? minFrames : 1;
        result.maxFrames = maxFrames >= result.minFrames ? maxFrames : result.minFrames;
        result.descriptor = ClapPluginScanner::inspectPluginPath(outerPluginPath);
        result.descriptor.metadataOnly = true;
        result.descriptor.abiProbed = false;

        const clap_plugin_entry_t* initializedEntry = nullptr;
        const clap_plugin_t* createdPlugin = nullptr;
        ClapHostRequestState hostState;

        auto cleanup = [&result, &initializedEntry, &createdPlugin, &hostState]()
        {
            if (createdPlugin != nullptr && result.pluginActivated && createdPlugin->deactivate != nullptr && !result.pluginDeactivated)
            {
                createdPlugin->deactivate(createdPlugin);
                result.pluginDeactivated = true;
            }

            if (createdPlugin != nullptr && createdPlugin->destroy != nullptr && !result.instanceDestroyed)
            {
                createdPlugin->destroy(createdPlugin);
                createdPlugin = nullptr;
                result.instanceDestroyed = true;
            }

            if (initializedEntry != nullptr && initializedEntry->deinit != nullptr && !result.entryDeinitialized)
            {
                initializedEntry->deinit();
                initializedEntry = nullptr;
                result.entryDeinitialized = true;
            }

            result.restartRequested = hostState.restartRequested;
            result.processRequested = hostState.processRequested;
            result.callbackRequested = hostState.callbackRequested;
            result.hostThreadCheckRequested = hostState.threadCheckRequested;
        };

        auto fail = [&result, &cleanup](std::string stage, std::string message) -> ClapActivationValidationResult&
        {
            cleanup();
            result.stage = std::move(stage);
            result.message = std::move(message);
            result.descriptor.status = ClapPluginScanStatus::Unsupported;
            result.descriptor.statusMessage = result.message;
            return result;
        };

        try
        {
            std::error_code ec;
            if (!std::filesystem::exists(outerPluginPath, ec))
            {
                result.descriptor.status = ClapPluginScanStatus::Missing;
                result.stage = "path";
                result.message = "CLAP plugin path was not found.";
                result.descriptor.statusMessage = result.message;
                return result;
            }

            if (!ClapPluginScanner::isOuterClapPlugin(outerPluginPath))
                return fail("path", "Path is not an outer .clap plugin file or bundle.");

            if (result.descriptor.binaryPath.empty())
                return fail("binary", "Could not find a loadable CLAP binary inside the plugin path.");

            DynamicLibrary library(result.descriptor.binaryPath);
            if (!library.isLoaded())
            {
                std::string message = "Could not load CLAP binary";
                const auto error = library.lastErrorText();
                if (!error.empty())
                    message += ": " + error;
                return fail("load_library", std::move(message));
            }
            result.loadedLibrary = true;

            auto* entry = static_cast<const clap_plugin_entry_t*>(library.symbol("clap_entry"));
            if (entry == nullptr)
                return fail("entry", "Loaded binary but did not find exported CLAP entry symbol 'clap_entry'.");
            result.foundEntry = true;
            result.descriptor.clapVersionMajor = entry->clap_version.major;
            result.descriptor.clapVersionMinor = entry->clap_version.minor;
            result.descriptor.clapVersionRevision = entry->clap_version.revision;

            if (entry->init == nullptr || entry->deinit == nullptr || entry->get_factory == nullptr)
                return fail("entry", "CLAP entry is incomplete.");

            if (!entry->init(outerPluginPath.string().c_str()))
                return fail("entry_init", "CLAP entry init() returned false.");
            result.entryInitialized = true;
            initializedEntry = entry;

            const auto* factoryVoid = entry->get_factory(clapPluginFactoryId);
            const auto* factory = static_cast<const clap_plugin_factory_t*>(factoryVoid);
            if (factory == nullptr || factory->get_plugin_count == nullptr || factory->get_plugin_descriptor == nullptr || factory->create_plugin == nullptr)
                return fail("factory", "CLAP plugin factory was not available or did not expose create_plugin().");
            result.foundFactory = true;

            const auto count = factory->get_plugin_count(factory);
            result.pluginCount = static_cast<int>(count);
            result.descriptor.clapPluginCount = result.pluginCount;
            if (count == 0)
                return fail("factory", "CLAP plugin factory reported zero plugins.");

            const auto selectedIndex = static_cast<std::uint32_t>(result.selectedIndex);
            if (selectedIndex >= count)
                return fail("descriptor", "Requested CLAP plugin index is outside the plugin factory descriptor range.");

            const auto* desc = factory->get_plugin_descriptor(factory, selectedIndex);
            if (desc == nullptr)
                return fail("descriptor", "CLAP plugin factory returned a null descriptor for the selected plugin index.");
            result.foundDescriptor = true;

            populateDescriptorFromClapDescriptor(result.descriptor, *desc, count);
            result.descriptor.statusMessage = "CLAP activation validation is running in the helper process.";

            if (desc->id == nullptr || std::string(desc->id).empty())
                return fail("descriptor", "CLAP descriptor did not provide a plugin id for create_plugin().");

            clap_host_t host {};
            host.clap_version = { 1, 1, 0 };
            host.host_data = &hostState;
            host.name = "Poor Man's Studio CLAP Host Helper";
            host.vendor = "Poor Man's Studio";
            host.url = "";
            host.version = "0.66.6";
            host.get_extension = hostGetExtension;
            host.request_restart = hostRequestRestart;
            host.request_process = hostRequestProcess;
            host.request_callback = hostRequestCallback;

            const auto* pluginVoid = factory->create_plugin(factory, &host, desc->id);
            const auto* plugin = static_cast<const clap_plugin_t*>(pluginVoid);
            if (plugin == nullptr)
                return fail("create_plugin", "CLAP plugin factory returned a null plugin instance.");
            result.instanceCreated = true;
            createdPlugin = plugin;

            if (plugin->destroy == nullptr)
                return fail("plugin_destroy", "CLAP plugin instance did not expose destroy().");

            if (plugin->init == nullptr)
                return fail("plugin_init", "CLAP plugin instance did not expose init().");

            if (!plugin->init(plugin))
                return fail("plugin_init", "CLAP plugin init() returned false.");
            result.pluginInitialized = true;
            queryCapabilities(plugin, result, false);

            if (plugin->activate == nullptr)
                return fail("plugin_activate", "CLAP plugin instance did not expose activate().");

            if (plugin->deactivate == nullptr)
                return fail("plugin_deactivate", "CLAP plugin instance did not expose deactivate().");

            if (!plugin->activate(plugin, result.sampleRate, static_cast<std::uint32_t>(result.minFrames), static_cast<std::uint32_t>(result.maxFrames)))
                return fail("plugin_activate", "CLAP plugin activate() returned false.");
            result.pluginActivated = true;
            queryCapabilities(plugin, result, true);

            queryAudioPorts(plugin, result);
            queryNotePorts(plugin, result);

            plugin->deactivate(plugin);
            result.pluginDeactivated = true;

            result.restartRequested = hostState.restartRequested;
            result.processRequested = hostState.processRequested;
            result.callbackRequested = hostState.callbackRequested;
            result.hostThreadCheckRequested = hostState.threadCheckRequested;

            result.stage = "ok";
            result.message = count > 1
                ? "CLAP activation validation succeeded. The selected plugin instance was created, initialized, activated, capability-probed, deactivated, destroyed, and the CLAP entry was deinitialized. This bundle exposes " + std::to_string(count) + " plugin descriptors."
                : "CLAP activation validation succeeded. The plugin instance was created, initialized, activated, capability-probed, deactivated, destroyed, and the CLAP entry was deinitialized.";
            result.descriptor.status = ClapPluginScanStatus::Candidate;
            result.descriptor.statusMessage = result.message;
            cleanup();
            return result;
        }
        catch (const std::exception& e)
        {
            cleanup();
            result.message = e.what();
            result.stage = result.stage.empty() ? "exception" : result.stage;
            result.descriptor.status = ClapPluginScanStatus::Unsupported;
            result.descriptor.statusMessage = result.message;
            return result;
        }
        catch (...)
        {
            cleanup();
            result.message = "Unknown CLAP activation validation exception.";
            result.stage = result.stage.empty() ? "exception" : result.stage;
            result.descriptor.status = ClapPluginScanStatus::Unsupported;
            result.descriptor.statusMessage = result.message;
            return result;
        }
    }

    ClapProcessValidationResult ClapAbiProbe::validatePluginSilentProcess(const std::filesystem::path& outerPluginPath,
                                                                          int pluginIndex,
                                                                          double sampleRate,
                                                                          int minFrames,
                                                                          int maxFrames,
                                                                          int processFrames)
    {
        ClapProcessValidationResult result;
        result.attempted = true;
        result.selectedIndex = pluginIndex < 0 ? 0 : pluginIndex;
        result.sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
        result.minFrames = minFrames > 0 ? minFrames : 1;
        result.maxFrames = maxFrames >= result.minFrames ? maxFrames : result.minFrames;
        result.processFrames = processFrames > 0 ? processFrames : 64;
        if (result.processFrames < result.minFrames)
            result.processFrames = result.minFrames;
        if (result.processFrames > result.maxFrames)
            result.processFrames = result.maxFrames;
        result.descriptor = ClapPluginScanner::inspectPluginPath(outerPluginPath);
        result.descriptor.metadataOnly = true;
        result.descriptor.abiProbed = false;

        const clap_plugin_entry_t* initializedEntry = nullptr;
        const clap_plugin_t* createdPlugin = nullptr;
        ClapHostRequestState hostState;

        auto cleanup = [&result, &initializedEntry, &createdPlugin, &hostState]()
        {
            if (createdPlugin != nullptr && result.pluginStartedProcessing && createdPlugin->stop_processing != nullptr && !result.pluginStoppedProcessing)
            {
                hostState.audioThreadPhase = true;
                createdPlugin->stop_processing(createdPlugin);
                hostState.audioThreadPhase = false;
                result.pluginStoppedProcessing = true;
            }

            if (createdPlugin != nullptr && result.pluginActivated && createdPlugin->deactivate != nullptr && !result.pluginDeactivated)
            {
                createdPlugin->deactivate(createdPlugin);
                result.pluginDeactivated = true;
            }

            if (createdPlugin != nullptr && createdPlugin->destroy != nullptr && !result.instanceDestroyed)
            {
                createdPlugin->destroy(createdPlugin);
                createdPlugin = nullptr;
                result.instanceDestroyed = true;
            }

            if (initializedEntry != nullptr && initializedEntry->deinit != nullptr && !result.entryDeinitialized)
            {
                initializedEntry->deinit();
                initializedEntry = nullptr;
                result.entryDeinitialized = true;
            }

            result.restartRequested = hostState.restartRequested;
            result.processRequested = hostState.processRequested;
            result.callbackRequested = hostState.callbackRequested;
            result.hostThreadCheckRequested = hostState.threadCheckRequested;
        };

        auto fail = [&result, &cleanup](std::string stage, std::string message) -> ClapProcessValidationResult&
        {
            cleanup();
            result.stage = std::move(stage);
            result.message = std::move(message);
            result.descriptor.status = ClapPluginScanStatus::Unsupported;
            result.descriptor.statusMessage = result.message;
            return result;
        };

        try
        {
            std::error_code ec;
            if (!std::filesystem::exists(outerPluginPath, ec))
            {
                result.descriptor.status = ClapPluginScanStatus::Missing;
                result.stage = "path";
                result.message = "CLAP plugin path was not found.";
                result.descriptor.statusMessage = result.message;
                return result;
            }

            if (!ClapPluginScanner::isOuterClapPlugin(outerPluginPath))
                return fail("path", "Path is not an outer .clap plugin file or bundle.");

            if (result.descriptor.binaryPath.empty())
                return fail("binary", "Could not find a loadable CLAP binary inside the plugin path.");

            DynamicLibrary library(result.descriptor.binaryPath);
            if (!library.isLoaded())
            {
                std::string message = "Could not load CLAP binary";
                const auto error = library.lastErrorText();
                if (!error.empty())
                    message += ": " + error;
                return fail("load_library", std::move(message));
            }
            result.loadedLibrary = true;

            auto* entry = static_cast<const clap_plugin_entry_t*>(library.symbol("clap_entry"));
            if (entry == nullptr)
                return fail("entry", "Loaded binary but did not find exported CLAP entry symbol 'clap_entry'.");
            result.foundEntry = true;
            result.descriptor.clapVersionMajor = entry->clap_version.major;
            result.descriptor.clapVersionMinor = entry->clap_version.minor;
            result.descriptor.clapVersionRevision = entry->clap_version.revision;

            if (entry->init == nullptr || entry->deinit == nullptr || entry->get_factory == nullptr)
                return fail("entry", "CLAP entry is incomplete.");

            if (!entry->init(outerPluginPath.string().c_str()))
                return fail("entry_init", "CLAP entry init() returned false.");
            result.entryInitialized = true;
            initializedEntry = entry;

            const auto* factoryVoid = entry->get_factory(clapPluginFactoryId);
            const auto* factory = static_cast<const clap_plugin_factory_t*>(factoryVoid);
            if (factory == nullptr || factory->get_plugin_count == nullptr || factory->get_plugin_descriptor == nullptr || factory->create_plugin == nullptr)
                return fail("factory", "CLAP plugin factory was not available or did not expose create_plugin().");
            result.foundFactory = true;

            const auto count = factory->get_plugin_count(factory);
            result.pluginCount = static_cast<int>(count);
            result.descriptor.clapPluginCount = result.pluginCount;
            if (count == 0)
                return fail("factory", "CLAP plugin factory reported zero plugins.");

            const auto selectedIndex = static_cast<std::uint32_t>(result.selectedIndex);
            if (selectedIndex >= count)
                return fail("descriptor", "Requested CLAP plugin index is outside the plugin factory descriptor range.");

            const auto* desc = factory->get_plugin_descriptor(factory, selectedIndex);
            if (desc == nullptr)
                return fail("descriptor", "CLAP plugin factory returned a null descriptor for the selected plugin index.");
            result.foundDescriptor = true;

            populateDescriptorFromClapDescriptor(result.descriptor, *desc, count);
            result.descriptor.statusMessage = "CLAP silent process smoke test is running in the helper process.";

            if (desc->id == nullptr || std::string(desc->id).empty())
                return fail("descriptor", "CLAP descriptor did not provide a plugin id for create_plugin().");

            clap_host_t host {};
            host.clap_version = { 1, 1, 0 };
            host.host_data = &hostState;
            host.name = "Poor Man's Studio CLAP Host Helper";
            host.vendor = "Poor Man's Studio";
            host.url = "";
            host.version = "0.66.6";
            host.get_extension = hostGetExtension;
            host.request_restart = hostRequestRestart;
            host.request_process = hostRequestProcess;
            host.request_callback = hostRequestCallback;

            const auto* pluginVoid = factory->create_plugin(factory, &host, desc->id);
            const auto* plugin = static_cast<const clap_plugin_t*>(pluginVoid);
            if (plugin == nullptr)
                return fail("create_plugin", "CLAP plugin factory returned a null plugin instance.");
            result.instanceCreated = true;
            createdPlugin = plugin;

            if (plugin->destroy == nullptr)
                return fail("plugin_destroy", "CLAP plugin instance did not expose destroy().");

            if (plugin->init == nullptr)
                return fail("plugin_init", "CLAP plugin instance did not expose init().");

            if (!plugin->init(plugin))
                return fail("plugin_init", "CLAP plugin init() returned false.");
            result.pluginInitialized = true;
            queryCapabilities(plugin, result, false);

            if (plugin->activate == nullptr)
                return fail("plugin_activate", "CLAP plugin instance did not expose activate().");

            if (plugin->deactivate == nullptr)
                return fail("plugin_deactivate", "CLAP plugin instance did not expose deactivate().");

            if (!plugin->activate(plugin, result.sampleRate, static_cast<std::uint32_t>(result.minFrames), static_cast<std::uint32_t>(result.maxFrames)))
                return fail("plugin_activate", "CLAP plugin activate() returned false.");
            result.pluginActivated = true;
            queryCapabilities(plugin, result, true);

            queryAudioPorts(plugin, result);
            queryNotePorts(plugin, result);

            const auto inputChannelCounts = collectAudioPortChannelCounts(plugin, true);
            const auto outputChannelCounts = collectAudioPortChannelCounts(plugin, false);
            result.processInputBufferCount = static_cast<int>(inputChannelCounts.size());
            result.processOutputBufferCount = static_cast<int>(outputChannelCounts.size());
            result.processInputChannelCount = totalChannels(inputChannelCounts);
            result.processOutputChannelCount = totalChannels(outputChannelCounts);

            if (plugin->start_processing == nullptr)
                return fail("plugin_start_processing", "CLAP plugin instance did not expose start_processing().");

            if (plugin->stop_processing == nullptr)
                return fail("plugin_stop_processing", "CLAP plugin instance did not expose stop_processing().");

            if (plugin->process == nullptr)
                return fail("plugin_process", "CLAP plugin instance did not expose process().");

            SilentAudioBufferStorage inputBuffers;
            SilentAudioBufferStorage outputBuffers;
            inputBuffers.build(inputChannelCounts, static_cast<std::uint32_t>(result.processFrames));
            outputBuffers.build(outputChannelCounts, static_cast<std::uint32_t>(result.processFrames));

            clap_input_events_t inputEvents {};
            inputEvents.ctx = nullptr;
            inputEvents.size = emptyInputEventsSize;
            inputEvents.get = emptyInputEventsGet;

            ClapOutputEventState outputEventState;
            clap_output_events_t outputEvents {};
            outputEvents.ctx = &outputEventState;
            outputEvents.try_push = outputEventsTryPush;

            clap_process_t process {};
            process.steady_time = -1;
            process.frames_count = static_cast<std::uint32_t>(result.processFrames);
            process.transport = nullptr;
            process.audio_inputs = inputBuffers.buffers.empty() ? nullptr : inputBuffers.buffers.data();
            process.audio_outputs = outputBuffers.buffers.empty() ? nullptr : outputBuffers.buffers.data();
            process.audio_inputs_count = static_cast<std::uint32_t>(inputBuffers.buffers.size());
            process.audio_outputs_count = static_cast<std::uint32_t>(outputBuffers.buffers.size());
            process.in_events = &inputEvents;
            process.out_events = &outputEvents;

            hostState.audioThreadPhase = true;
            if (!plugin->start_processing(plugin))
            {
                hostState.audioThreadPhase = false;
                return fail("plugin_start_processing", "CLAP plugin start_processing() returned false.");
            }
            result.pluginStartedProcessing = true;

            const auto status = plugin->process(plugin, &process);
            result.pluginProcessCalled = true;
            result.processStatus = status;
            result.processStatusText = processStatusToString(status);
            result.pluginProcessReturnedOk = isProcessStatusOk(status);
            result.outputEventCount = outputEventState.pushedEvents;
            const auto [maximumOutput, nonFiniteSamples] = outputBuffers.analyze();
            result.maxOutputAbs = maximumOutput;
            result.nonFiniteSampleCount = nonFiniteSamples;
            result.finiteOutput = nonFiniteSamples == 0;

            if (!result.pluginProcessReturnedOk)
            {
                result.processMessage = "CLAP process() returned " + result.processStatusText + ".";
                return fail("plugin_process", result.processMessage);
            }

            if (!result.finiteOutput)
            {
                result.processMessage = "CLAP process() produced non-finite output samples.";
                return fail("plugin_process_output", result.processMessage);
            }

            plugin->stop_processing(plugin);
            result.pluginStoppedProcessing = true;
            hostState.audioThreadPhase = false;

            plugin->deactivate(plugin);
            result.pluginDeactivated = true;

            result.restartRequested = hostState.restartRequested;
            result.processRequested = hostState.processRequested;
            result.callbackRequested = hostState.callbackRequested;
            result.hostThreadCheckRequested = hostState.threadCheckRequested;
            result.processMessage = "CLAP process() accepted one silent buffer block and returned " + result.processStatusText + ".";

            result.stage = "ok";
            result.message = count > 1
                ? "CLAP silent process smoke test succeeded. The selected plugin instance was created, initialized, activated, started, processed one silent block, stopped, deactivated, destroyed, and the CLAP entry was deinitialized. This bundle exposes " + std::to_string(count) + " plugin descriptors."
                : "CLAP silent process smoke test succeeded. The plugin instance was created, initialized, activated, started, processed one silent block, stopped, deactivated, destroyed, and the CLAP entry was deinitialized.";
            result.descriptor.status = ClapPluginScanStatus::Candidate;
            result.descriptor.statusMessage = result.message;
            cleanup();
            return result;
        }
        catch (const std::exception& e)
        {
            cleanup();
            result.message = e.what();
            result.stage = result.stage.empty() ? "exception" : result.stage;
            result.descriptor.status = ClapPluginScanStatus::Unsupported;
            result.descriptor.statusMessage = result.message;
            return result;
        }
        catch (...)
        {
            cleanup();
            result.message = "Unknown CLAP silent process smoke test exception.";
            result.stage = result.stage.empty() ? "exception" : result.stage;
            result.descriptor.status = ClapPluginScanStatus::Unsupported;
            result.descriptor.statusMessage = result.message;
            return result;
        }
    }


    ClapStateValidationResult ClapAbiProbe::validatePluginStateRoundTrip(const std::filesystem::path& outerPluginPath,
                                                                          int pluginIndex,
                                                                          double sampleRate,
                                                                          int minFrames,
                                                                          int maxFrames,
                                                                          int processFrames)
    {
        ClapStateValidationResult result;
        result.attempted = true;
        result.selectedIndex = pluginIndex < 0 ? 0 : pluginIndex;
        result.sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
        result.minFrames = std::max(1, minFrames);
        result.maxFrames = std::max(result.minFrames, maxFrames);
        result.processFrames = std::clamp(processFrames, result.minFrames, result.maxFrames);
        result.descriptor = ClapPluginScanner::inspectPluginPath(outerPluginPath);
        result.descriptor.metadataOnly = true;
        result.descriptor.abiProbed = false;

        const clap_plugin_entry_t* initializedEntry = nullptr;
        const clap_plugin_t* firstPlugin = nullptr;
        const clap_plugin_t* secondPlugin = nullptr;
        ClapHostRequestState hostState;

        auto cleanup = [&]()
        {
            if (secondPlugin != nullptr)
            {
                if (result.secondPluginStartedProcessing && !result.secondPluginStoppedProcessing && secondPlugin->stop_processing != nullptr)
                {
                    hostState.audioThreadPhase = true;
                    secondPlugin->stop_processing(secondPlugin);
                    hostState.audioThreadPhase = false;
                    result.secondPluginStoppedProcessing = true;
                }
                if (result.secondPluginActivated && !result.secondPluginDeactivated && secondPlugin->deactivate != nullptr)
                {
                    secondPlugin->deactivate(secondPlugin);
                    result.secondPluginDeactivated = true;
                }
                if (secondPlugin->destroy != nullptr && !result.secondInstanceDestroyed)
                {
                    secondPlugin->destroy(secondPlugin);
                    result.secondInstanceDestroyed = true;
                }
                secondPlugin = nullptr;
            }

            if (firstPlugin != nullptr)
            {
                if (firstPlugin->destroy != nullptr && !result.firstInstanceDestroyed)
                {
                    firstPlugin->destroy(firstPlugin);
                    result.firstInstanceDestroyed = true;
                }
                firstPlugin = nullptr;
            }

            if (initializedEntry != nullptr && initializedEntry->deinit != nullptr && !result.entryDeinitialized)
            {
                initializedEntry->deinit();
                initializedEntry = nullptr;
                result.entryDeinitialized = true;
            }

            result.restartRequested = hostState.restartRequested;
            result.processRequested = hostState.processRequested;
            result.callbackRequested = hostState.callbackRequested;
            result.hostThreadCheckRequested = hostState.threadCheckRequested;
        };

        auto fail = [&](std::string stage, std::string message) -> ClapStateValidationResult&
        {
            cleanup();
            result.stage = std::move(stage);
            result.message = std::move(message);
            if (result.descriptor.status != ClapPluginScanStatus::Missing)
                result.descriptor.status = ClapPluginScanStatus::Unsupported;
            result.descriptor.statusMessage = result.message;
            return result;
        };

        try
        {
            std::error_code ec;
            if (!std::filesystem::exists(outerPluginPath, ec))
            {
                result.descriptor.status = ClapPluginScanStatus::Missing;
                return fail("path", "CLAP plugin path was not found.");
            }
            if (!ClapPluginScanner::isOuterClapPlugin(outerPluginPath))
                return fail("path", "Path is not an outer .clap plugin file or bundle.");
            if (result.descriptor.binaryPath.empty())
                return fail("binary", "Could not find a loadable CLAP binary inside the plugin path.");

            DynamicLibrary library(result.descriptor.binaryPath);
            if (!library.isLoaded())
            {
                auto message = std::string("Could not load CLAP binary");
                const auto error = library.lastErrorText();
                if (!error.empty()) message += ": " + error;
                return fail("load_library", message);
            }
            result.loadedLibrary = true;

            auto* entry = static_cast<const clap_plugin_entry_t*>(library.symbol("clap_entry"));
            if (entry == nullptr)
                return fail("entry", "Loaded binary but did not find exported CLAP entry symbol 'clap_entry'.");
            result.foundEntry = true;
            result.descriptor.clapVersionMajor = entry->clap_version.major;
            result.descriptor.clapVersionMinor = entry->clap_version.minor;
            result.descriptor.clapVersionRevision = entry->clap_version.revision;
            if (entry->init == nullptr || entry->deinit == nullptr || entry->get_factory == nullptr)
                return fail("entry", "CLAP entry is incomplete.");
            if (!entry->init(outerPluginPath.string().c_str()))
                return fail("entry_init", "CLAP entry init() returned false.");
            result.entryInitialized = true;
            initializedEntry = entry;

            const auto* factory = static_cast<const clap_plugin_factory_t*>(entry->get_factory(clapPluginFactoryId));
            if (factory == nullptr || factory->get_plugin_count == nullptr || factory->get_plugin_descriptor == nullptr || factory->create_plugin == nullptr)
                return fail("factory", "CLAP plugin factory was not available or was incomplete.");
            result.foundFactory = true;

            const auto count = factory->get_plugin_count(factory);
            result.pluginCount = static_cast<int>(count);
            result.descriptor.clapPluginCount = result.pluginCount;
            if (count == 0)
                return fail("descriptor", "CLAP plugin factory reported zero plugins.");
            if (result.selectedIndex >= static_cast<int>(count))
                return fail("descriptor", "Requested CLAP plugin index is outside the bundle's descriptor range.");

            const auto* desc = factory->get_plugin_descriptor(factory, static_cast<std::uint32_t>(result.selectedIndex));
            if (desc == nullptr || desc->id == nullptr || std::string(desc->id).empty())
                return fail("descriptor", "CLAP factory did not provide a usable descriptor for the selected plugin index.");
            result.foundDescriptor = true;
            populateDescriptorFromClapDescriptor(result.descriptor, *desc, count);

            clap_host_t host {};
            host.clap_version = { 1, 1, 0 };
            host.host_data = &hostState;
            host.name = "Poor Man's Studio CLAP Host Helper";
            host.vendor = "Poor Man's Studio";
            host.url = "";
            host.version = "0.66.6";
            host.get_extension = hostGetExtension;
            host.request_restart = hostRequestRestart;
            host.request_process = hostRequestProcess;
            host.request_callback = hostRequestCallback;

            firstPlugin = static_cast<const clap_plugin_t*>(factory->create_plugin(factory, &host, desc->id));
            if (firstPlugin == nullptr)
                return fail("first_create", "CLAP factory returned a null first plugin instance.");
            result.firstInstanceCreated = true;
            if (firstPlugin->destroy == nullptr || firstPlugin->init == nullptr)
                return fail("first_instance", "First CLAP plugin instance did not expose init() and destroy().");
            if (!firstPlugin->init(firstPlugin))
                return fail("first_init", "First CLAP plugin init() returned false.");
            result.firstPluginInitialized = true;
            queryCapabilities(firstPlugin, result, false);

            const auto* firstState = firstPlugin->get_extension == nullptr
                ? nullptr
                : static_cast<const clap_plugin_state_t*>(firstPlugin->get_extension(firstPlugin, clapStateExtensionId));
            if (firstState == nullptr || firstState->save == nullptr || firstState->load == nullptr)
                return fail("state_extension", "Selected CLAP plugin does not expose a complete clap.state extension.");
            result.stateExtensionAvailable = true;

            StateWriteStorage firstStorage;
            clap_ostream_t firstStream { &firstStorage, stateWrite };
            if (!firstState->save(firstPlugin, &firstStream))
                return fail("state_save", "CLAP state save() returned false for the first instance.");
            result.stateSaved = true;
            result.firstStateBytes = firstStorage.bytes.size();
            result.firstStateHash = fnv1a64(firstStorage.bytes.data(), firstStorage.bytes.size());

            firstPlugin->destroy(firstPlugin);
            firstPlugin = nullptr;
            result.firstInstanceDestroyed = true;

            secondPlugin = static_cast<const clap_plugin_t*>(factory->create_plugin(factory, &host, desc->id));
            if (secondPlugin == nullptr)
                return fail("second_create", "CLAP factory returned a null second plugin instance.");
            result.secondInstanceCreated = true;
            if (secondPlugin->destroy == nullptr || secondPlugin->init == nullptr)
                return fail("second_instance", "Second CLAP plugin instance did not expose init() and destroy().");
            if (!secondPlugin->init(secondPlugin))
                return fail("second_init", "Second CLAP plugin init() returned false.");
            result.secondPluginInitialized = true;

            const auto* secondState = secondPlugin->get_extension == nullptr
                ? nullptr
                : static_cast<const clap_plugin_state_t*>(secondPlugin->get_extension(secondPlugin, clapStateExtensionId));
            if (secondState == nullptr || secondState->save == nullptr || secondState->load == nullptr)
                return fail("state_extension", "Recreated CLAP instance does not expose a complete clap.state extension.");

            StateReadStorage readStorage { &firstStorage.bytes, 0 };
            clap_istream_t readStream { &readStorage, stateRead };
            if (!secondState->load(secondPlugin, &readStream))
                return fail("state_load", "CLAP state load() returned false for the recreated instance.");
            result.stateLoaded = true;

            if (secondPlugin->activate == nullptr || secondPlugin->deactivate == nullptr
                || secondPlugin->start_processing == nullptr || secondPlugin->stop_processing == nullptr
                || secondPlugin->process == nullptr)
                return fail("second_lifecycle", "Recreated CLAP instance does not expose the complete activation/process lifecycle.");

            if (!secondPlugin->activate(secondPlugin, result.sampleRate,
                                        static_cast<std::uint32_t>(result.minFrames),
                                        static_cast<std::uint32_t>(result.maxFrames)))
                return fail("second_activate", "Recreated CLAP instance activate() returned false after state restore.");
            result.secondPluginActivated = true;
            queryCapabilities(secondPlugin, result, true);

            const auto inputChannelCounts = collectAudioPortChannelCounts(secondPlugin, true);
            const auto outputChannelCounts = collectAudioPortChannelCounts(secondPlugin, false);
            SilentAudioBufferStorage inputBuffers;
            SilentAudioBufferStorage outputBuffers;
            inputBuffers.build(inputChannelCounts, static_cast<std::uint32_t>(result.processFrames));
            outputBuffers.build(outputChannelCounts, static_cast<std::uint32_t>(result.processFrames));

            clap_input_events_t inputEvents {};
            inputEvents.size = emptyInputEventsSize;
            inputEvents.get = emptyInputEventsGet;
            ClapOutputEventState outputEventState;
            clap_output_events_t outputEvents { &outputEventState, outputEventsTryPush };
            clap_process_t process {};
            process.steady_time = -1;
            process.frames_count = static_cast<std::uint32_t>(result.processFrames);
            process.audio_inputs = inputBuffers.buffers.empty() ? nullptr : inputBuffers.buffers.data();
            process.audio_outputs = outputBuffers.buffers.empty() ? nullptr : outputBuffers.buffers.data();
            process.audio_inputs_count = static_cast<std::uint32_t>(inputBuffers.buffers.size());
            process.audio_outputs_count = static_cast<std::uint32_t>(outputBuffers.buffers.size());
            process.in_events = &inputEvents;
            process.out_events = &outputEvents;

            hostState.audioThreadPhase = true;
            if (!secondPlugin->start_processing(secondPlugin))
            {
                hostState.audioThreadPhase = false;
                return fail("second_start_processing", "Recreated CLAP instance start_processing() returned false.");
            }
            result.secondPluginStartedProcessing = true;
            const auto processStatus = secondPlugin->process(secondPlugin, &process);
            result.secondPluginProcessCalled = true;
            result.secondPluginProcessReturnedOk = isProcessStatusOk(processStatus);
            const auto [maximumOutput, nonFiniteSamples] = outputBuffers.analyze();
            result.maxOutputAbs = maximumOutput;
            result.nonFiniteSampleCount = nonFiniteSamples;
            result.finiteOutput = nonFiniteSamples == 0;
            if (!result.secondPluginProcessReturnedOk)
                return fail("second_process", "Recreated CLAP instance process() returned " + processStatusToString(processStatus) + ".");
            if (!result.finiteOutput)
                return fail("second_process_output", "Recreated CLAP instance produced non-finite output after state restore.");

            secondPlugin->stop_processing(secondPlugin);
            result.secondPluginStoppedProcessing = true;
            hostState.audioThreadPhase = false;
            secondPlugin->deactivate(secondPlugin);
            result.secondPluginDeactivated = true;

            StateWriteStorage secondStorage;
            clap_ostream_t secondStream { &secondStorage, stateWrite };
            if (!secondState->save(secondPlugin, &secondStream))
                return fail("state_resave", "CLAP state save() returned false for the recreated instance.");
            result.stateResaved = true;
            result.secondStateBytes = secondStorage.bytes.size();
            result.secondStateHash = fnv1a64(secondStorage.bytes.data(), secondStorage.bytes.size());
            result.stateByteEquivalent = firstStorage.bytes == secondStorage.bytes;

            secondPlugin->destroy(secondPlugin);
            secondPlugin = nullptr;
            result.secondInstanceDestroyed = true;
            initializedEntry->deinit();
            initializedEntry = nullptr;
            result.entryDeinitialized = true;
            result.restartRequested = hostState.restartRequested;
            result.processRequested = hostState.processRequested;
            result.callbackRequested = hostState.callbackRequested;
            result.hostThreadCheckRequested = hostState.threadCheckRequested;

            result.stage = "ok";
            result.message = "CLAP state round-trip succeeded: save, recreate, restore, silent process, second save, and cleanup completed.";
            result.descriptor.status = ClapPluginScanStatus::Candidate;
            result.descriptor.statusMessage = result.message;
            return result;
        }
        catch (const std::exception& e)
        {
            return fail("exception", e.what());
        }
        catch (...)
        {
            return fail("exception", "Unknown CLAP state round-trip validation exception.");
        }
    }

}
