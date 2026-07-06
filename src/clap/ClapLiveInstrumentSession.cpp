#include "clap/ClapLiveInstrumentSession.h"

#include "app/AppVersion.h"
#include "clap/ClapPluginScanner.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cstdint>
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
    constexpr const char* clapNotePortsExtensionId = "clap.note-ports";
    constexpr std::size_t clapNameSize = 256;

    constexpr std::uint32_t clapNoteDialectClap = 1u << 0;
    constexpr std::uint32_t clapNoteDialectMidi = 1u << 1;
    constexpr std::uint32_t clapNoteDialectMidiMpe = 1u << 2;

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

    struct clap_host_t;

    struct clap_host_note_ports_t
    {
        std::uint32_t (*supported_dialects)(const clap_host_t* host);
        void (*rescan)(const clap_host_t* host, std::uint32_t flags);
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

    constexpr std::uint16_t clapEventNoteOn = 0;
    constexpr std::uint16_t clapEventNoteOff = 1;
    constexpr std::uint16_t clapEventMidi = 10;
    constexpr std::uint16_t clapCoreEventSpaceId = 0;

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

    struct clap_event_note_t
    {
        clap_event_header_t header;
        std::int32_t note_id;
        std::int16_t port_index;
        std::int16_t channel;
        std::int16_t key;
        double velocity;
    };

    struct clap_event_midi_t
    {
        clap_event_header_t header;
        std::uint16_t port_index;
        std::uint8_t data[3];
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

    struct ClapHostRequestState
    {
        bool restartRequested = false;
        bool processRequested = false;
        bool callbackRequested = false;
        bool notePortsRescanRequested = false;
        std::uint32_t notePortsRescanFlags = 0;
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

    enum class LiveNoteDialect
    {
        ClapNote,
        Midi,
        MidiMpe
    };

    std::string noteDialectName(LiveNoteDialect dialect)
    {
        switch (dialect)
        {
            case LiveNoteDialect::ClapNote: return "CLAP note";
            case LiveNoteDialect::Midi: return "MIDI 1";
            case LiveNoteDialect::MidiMpe: return "MIDI/MPE";
            default: return "unknown";
        }
    }

    struct NoteDialectSelection
    {
        bool supported = true;
        LiveNoteDialect dialect = LiveNoteDialect::ClapNote;
        std::uint32_t portIndex = 0;
        std::string summary = "note-ports extension unavailable; using CLAP note events optimistically";
    };

    NoteDialectSelection chooseNoteInputDialect(const clap_plugin_t* plugin)
    {
        NoteDialectSelection selection;
        if (plugin == nullptr || plugin->get_extension == nullptr)
            return selection;

        const auto* extension = static_cast<const clap_plugin_note_ports_t*>(plugin->get_extension(plugin, clapNotePortsExtensionId));
        if (extension == nullptr || extension->count == nullptr || extension->get == nullptr)
            return selection;

        const auto count = std::min<std::uint32_t>(extension->count(plugin, true), 16);
        if (count == 0)
        {
            selection.supported = false;
            selection.summary = "note-ports extension reported no input ports";
            return selection;
        }

        auto makeSelection = [](std::uint32_t port, LiveNoteDialect dialect, const char* reason)
        {
            NoteDialectSelection result;
            result.supported = true;
            result.dialect = dialect;
            result.portIndex = port;
            result.summary = std::string("note-ports selected ") + noteDialectName(dialect) + " on input port " + std::to_string(port) + " (" + reason + ")";
            return result;
        };

        NoteDialectSelection firstClap; firstClap.supported = false;
        NoteDialectSelection firstMidi; firstMidi.supported = false;
        NoteDialectSelection firstMpe; firstMpe.supported = false;

        for (std::uint32_t i = 0; i < count; ++i)
        {
            clap_note_port_info_t info {};
            if (!extension->get(plugin, i, true, &info))
                continue;

            const auto supported = info.supported_dialects;
            const auto preferred = info.preferred_dialect;

            if ((preferred & clapNoteDialectClap) != 0 && (supported & clapNoteDialectClap) != 0)
                return makeSelection(i, LiveNoteDialect::ClapNote, "plugin preferred CLAP notes");
            if ((preferred & clapNoteDialectMidi) != 0 && (supported & clapNoteDialectMidi) != 0)
                return makeSelection(i, LiveNoteDialect::Midi, "plugin preferred MIDI 1");
            if ((preferred & clapNoteDialectMidiMpe) != 0 && (supported & clapNoteDialectMidiMpe) != 0)
                return makeSelection(i, LiveNoteDialect::MidiMpe, "plugin preferred MIDI/MPE");

            if (!firstClap.supported && (supported & clapNoteDialectClap) != 0)
                firstClap = makeSelection(i, LiveNoteDialect::ClapNote, "first supported CLAP-note port");
            if (!firstMidi.supported && (supported & clapNoteDialectMidi) != 0)
                firstMidi = makeSelection(i, LiveNoteDialect::Midi, "first supported MIDI 1 port");
            if (!firstMpe.supported && (supported & clapNoteDialectMidiMpe) != 0)
                firstMpe = makeSelection(i, LiveNoteDialect::MidiMpe, "first supported MIDI/MPE port");
        }

        if (firstClap.supported) return firstClap;
        if (firstMidi.supported) return firstMidi;
        if (firstMpe.supported) return firstMpe;

        selection.supported = false;
        selection.summary = "note-ports extension did not advertise CLAP note, MIDI 1, or MIDI/MPE input support";
        return selection;
    }

    enum class ScheduledEventEncoding
    {
        ClapNote,
        Midi
    };

    struct ScheduledLiveEvent
    {
        ScheduledEventEncoding encoding = ScheduledEventEncoding::ClapNote;
        clap_event_note_t note {};
        clap_event_midi_t midi {};

        const clap_event_header_t* header() const
        {
            return encoding == ScheduledEventEncoding::Midi ? &midi.header : &note.header;
        }
    };

    struct LiveEventBlock
    {
        std::vector<ScheduledLiveEvent> events;
    };

    std::uint32_t inputEventsSize(const clap_input_events_t* list)
    {
        const auto* block = list == nullptr ? nullptr : static_cast<const LiveEventBlock*>(list->ctx);
        return block == nullptr ? 0u : static_cast<std::uint32_t>(block->events.size());
    }

    const clap_event_header_t* inputEventsGet(const clap_input_events_t* list, std::uint32_t index)
    {
        const auto* block = list == nullptr ? nullptr : static_cast<const LiveEventBlock*>(list->ctx);
        if (block == nullptr || index >= block->events.size())
            return nullptr;
        return block->events[index].header();
    }

    struct ClapOutputEventState
    {
        int pushedEvents = 0;
    };

    bool outputEventsTryPush(const clap_output_events_t* list, const clap_event_header_t*)
    {
        if (list != nullptr)
            if (auto* state = static_cast<ClapOutputEventState*>(list->ctx))
                ++state->pushedEvents;
        return true;
    }

    ScheduledLiveEvent makeClapLiveNoteEvent(const mw::clap::ClapLiveNoteEvent& source, LiveNoteDialect dialect, std::uint32_t portIndex, int frameCount)
    {
        ScheduledLiveEvent event;
        const auto time = static_cast<std::uint32_t>(std::clamp<int>(static_cast<int>(source.frameOffset), 0, std::max(0, frameCount - 1)));
        const auto channel = static_cast<std::uint8_t>(std::clamp(source.midiChannel - 1, 0, 15));
        const auto key = static_cast<std::uint8_t>(std::clamp(source.key, 0, 127));
        const auto velocity = static_cast<std::uint8_t>(source.type == mw::clap::ClapLiveNoteEventType::NoteOff ? 0 : std::clamp(source.velocity, 1, 127));

        if (dialect == LiveNoteDialect::ClapNote)
        {
            event.encoding = ScheduledEventEncoding::ClapNote;
            auto& note = event.note;
            note.header.size = sizeof(clap_event_note_t);
            note.header.time = time;
            note.header.space_id = clapCoreEventSpaceId;
            note.header.type = source.type == mw::clap::ClapLiveNoteEventType::NoteOff ? clapEventNoteOff : clapEventNoteOn;
            note.header.flags = 0;
            note.note_id = -1;
            note.port_index = static_cast<std::int16_t>(std::min<std::uint32_t>(portIndex, 32767u));
            note.channel = static_cast<std::int16_t>(channel);
            note.key = static_cast<std::int16_t>(key);
            note.velocity = source.type == mw::clap::ClapLiveNoteEventType::NoteOff ? 0.0 : static_cast<double>(velocity) / 127.0;
            return event;
        }

        event.encoding = ScheduledEventEncoding::Midi;
        auto& midi = event.midi;
        midi.header.size = sizeof(clap_event_midi_t);
        midi.header.time = time;
        midi.header.space_id = clapCoreEventSpaceId;
        midi.header.type = clapEventMidi;
        midi.header.flags = 0;
        midi.port_index = static_cast<std::uint16_t>(std::min<std::uint32_t>(portIndex, 65535u));
        midi.data[0] = static_cast<std::uint8_t>((source.type == mw::clap::ClapLiveNoteEventType::NoteOff ? 0x80 : 0x90) | channel);
        midi.data[1] = key;
        midi.data[2] = velocity;
        return event;
    }

    struct LiveOutputStorage
    {
        std::vector<std::vector<float>> samples;
        std::vector<float*> channelPointers;
        clap_audio_buffer_t buffer {};

        void build(int channels, int frames)
        {
            const auto safeChannels = std::clamp(channels, 1, 32);
            const auto safeFrames = std::max(1, frames);
            samples.assign(static_cast<std::size_t>(safeChannels), std::vector<float>(static_cast<std::size_t>(safeFrames), 0.0f));
            channelPointers.assign(static_cast<std::size_t>(safeChannels), nullptr);
            for (int ch = 0; ch < safeChannels; ++ch)
                channelPointers[static_cast<std::size_t>(ch)] = samples[static_cast<std::size_t>(ch)].data();
            buffer.data32 = channelPointers.data();
            buffer.data64 = nullptr;
            buffer.channel_count = static_cast<std::uint32_t>(safeChannels);
            buffer.latency = 0;
            buffer.constant_mask = 0;
        }
    };

    std::uint32_t hostSupportedNoteDialects(const clap_host_t*)
    {
        return clapNoteDialectClap | clapNoteDialectMidi | clapNoteDialectMidiMpe;
    }

    void hostNotePortsRescan(const clap_host_t* host, std::uint32_t flags)
    {
        if (host != nullptr)
            if (auto* state = static_cast<ClapHostRequestState*>(host->host_data))
            {
                state->notePortsRescanRequested = true;
                state->notePortsRescanFlags |= flags;
            }
    }

    const clap_host_note_ports_t hostNotePortsExtension
    {
        hostSupportedNoteDialects,
        hostNotePortsRescan
    };

    const void* hostGetExtension(const clap_host_t*, const char* extensionId)
    {
        if (extensionId != nullptr && std::strcmp(extensionId, clapNotePortsExtensionId) == 0)
            return &hostNotePortsExtension;
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
        error.clear();
        if (plugin == nullptr || stateBase64.empty())
            return true;
        if (plugin->get_extension == nullptr)
        {
            error = "plugin does not expose get_extension() for clap.state";
            return false;
        }

        const auto* state = static_cast<const clap_plugin_state_t*>(plugin->get_extension(plugin, clapStateExtensionId));
        if (state == nullptr || state->load == nullptr)
        {
            error = "plugin does not expose clap.state load()";
            return false;
        }

        juce::MemoryBlock memory;
        if (!memory.fromBase64Encoding(juce::String(stateBase64)))
        {
            error = "saved state was not valid base64";
            return false;
        }

        MemoryReadContext context;
        context.data = static_cast<const std::uint8_t*>(memory.getData());
        context.size = memory.getSize();

        clap_istream_t stream {};
        stream.ctx = &context;
        stream.read = memoryRead;

        if (!state->load(plugin, &stream))
        {
            error = "clap.state load() returned false";
            return false;
        }
        return true;
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
}

namespace mw::clap
{
    struct ClapLiveInstrumentSession::Impl
    {
        ~Impl() { close(); }

        bool open(const ClapLiveInstrumentSessionConfig& configIn, std::string& error)
        {
            close();
            error.clear();
            config = configIn;
            infoState = {};
            infoState.pluginPath = config.pluginPath;
            infoState.pluginName = config.pluginName;
            infoState.pluginUid = config.pluginUid;
            infoState.sampleRate = std::max(1, config.sampleRate);
            infoState.channelCount = std::clamp(config.channelCount, 1, 8);
            infoState.blockSize = std::clamp(config.blockSize, 64, 8192);

            if (config.pluginPath.empty())
                return fail(error, "CLAP live instance has no plugin path.");
            if (!ClapPluginScanner::isOuterClapPlugin(config.pluginPath))
                return fail(error, "CLAP live instance path is not an outer .clap plugin file or bundle: " + config.pluginPath.string());
            if (!std::filesystem::exists(config.pluginPath))
                return fail(error, "CLAP live instance plugin was not found: " + config.pluginPath.string());

            auto inspected = ClapPluginScanner::inspectPluginPath(config.pluginPath);
            if (inspected.binaryPath.empty())
                return fail(error, "Could not find a loadable CLAP binary inside live instrument plugin: " + config.pluginPath.string());

            library = std::make_unique<DynamicLibrary>(inspected.binaryPath);
            if (!library->isLoaded())
            {
                auto message = std::string("Could not load CLAP live instrument binary");
                const auto loadError = library->lastErrorText();
                if (!loadError.empty())
                    message += ": " + loadError;
                return fail(error, message);
            }

            auto* entryCandidate = static_cast<const clap_plugin_entry_t*>(library->symbol("clap_entry"));
            if (entryCandidate == nullptr)
                return fail(error, "Loaded CLAP live instrument binary but did not find exported CLAP entry symbol 'clap_entry'.");
            if (entryCandidate->init == nullptr || entryCandidate->deinit == nullptr || entryCandidate->get_factory == nullptr)
                return fail(error, "CLAP live instrument entry is incomplete.");
            if (!entryCandidate->init(config.pluginPath.string().c_str()))
                return fail(error, "CLAP live instrument entry init() returned false.");
            entry = entryCandidate;
            entryInitialized = true;

            const auto* factoryVoid = entry->get_factory(clapPluginFactoryId);
            const auto* factory = static_cast<const clap_plugin_factory_t*>(factoryVoid);
            if (factory == nullptr || factory->get_plugin_count == nullptr || factory->get_plugin_descriptor == nullptr || factory->create_plugin == nullptr)
                return fail(error, "CLAP live plugin factory was not available or did not expose create_plugin().");

            const auto count = factory->get_plugin_count(factory);
            if (count == 0)
                return fail(error, "CLAP live plugin factory reported zero plugins.");

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
                return fail(error, "CLAP live plugin factory did not return a usable descriptor.");
            if (selectedDesc->id == nullptr || std::string(selectedDesc->id).empty())
                return fail(error, "CLAP live descriptor did not provide a plugin id for create_plugin().");

            hostState = {};
            host = {};
            host.clap_version = { 1, 1, 0 };
            host.host_data = &hostState;
            host.name = "Poor Man's Studio CLAP Live Instrument Host";
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
                return fail(error, "CLAP live plugin factory returned a null instrument instance.");
            if (plugin->destroy == nullptr)
                return fail(error, "CLAP live instrument instance did not expose destroy().");
            if (plugin->init == nullptr)
                return fail(error, "CLAP live instrument instance did not expose init().");
            if (!plugin->init(plugin))
                return fail(error, "CLAP live instrument init() returned false.");
            pluginInitialized = true;

            if (!config.stateBase64.empty())
            {
                std::string stateError;
                if (!restoreStateBase64(plugin, config.stateBase64, stateError))
                    return fail(error, "CLAP live instrument saved state restore failed: " + stateError);
                infoState.stateRestored = true;
            }

            outputChannelCount = infoState.channelCount;
            if (plugin->get_extension != nullptr)
            {
                const auto* audioPorts = static_cast<const clap_plugin_audio_ports_t*>(plugin->get_extension(plugin, clapAudioPortsExtensionId));
                infoState.audioPortsAvailable = audioPorts != nullptr;
                if (audioPorts != nullptr && audioPorts->count != nullptr && audioPorts->get != nullptr)
                {
                    const auto outputPortCount = audioPorts->count(plugin, false);
                    if (outputPortCount > 0)
                    {
                        clap_audio_port_info_t portInfo {};
                        if (audioPorts->get(plugin, 0, false, &portInfo) && portInfo.channel_count > 0)
                            outputChannelCount = static_cast<int>(std::clamp<std::uint32_t>(portInfo.channel_count, 1, 32));
                    }
                }
            }

            noteDialectSelection = chooseNoteInputDialect(plugin);
            if (!noteDialectSelection.supported)
                return fail(error, "CLAP live instrument does not advertise a supported note input dialect: " + noteDialectSelection.summary);
            infoState.noteDialectSummary = noteDialectSelection.summary;
            infoState.notePortIndex = static_cast<int>(noteDialectSelection.portIndex);

            if (plugin->activate == nullptr || plugin->deactivate == nullptr)
                return fail(error, "CLAP live instrument instance did not expose activate()/deactivate().");
            if (plugin->start_processing == nullptr || plugin->stop_processing == nullptr || plugin->process == nullptr)
                return fail(error, "CLAP live instrument instance did not expose start/stop/process callbacks needed for future live playback.");
            if (!plugin->activate(plugin, static_cast<double>(infoState.sampleRate), 1, static_cast<std::uint32_t>(infoState.blockSize)))
                return fail(error, "CLAP live instrument activate() returned false.");
            activated = true;

            if (!plugin->start_processing(plugin))
                return fail(error, "CLAP live instrument start_processing() returned false.");
            startedProcessing = true;
            infoState.startedProcessing = true;
            infoState.open = true;

            std::ostringstream message;
            message << "CLAP live instrument instance opened and started for "
                    << (config.pluginName.empty() ? safeString(selectedDesc->name) : config.pluginName)
                    << " at " << infoState.sampleRate << " Hz, block " << infoState.blockSize
                    << ", channels " << infoState.channelCount
                    << ", state restored: " << (infoState.stateRestored ? "yes" : "no")
                    << ", audio-ports: " << (infoState.audioPortsAvailable ? "yes" : "no")
                    << ", output channels: " << outputChannelCount
                    << ", note dialect: " << noteDialectSelection.summary
                    << ", host process request: " << (hostState.processRequested ? "yes" : "no")
                    << ".";
            infoState.message = message.str();
            return true;
        }

        ClapLiveProcessResult processBlock(const ClapLiveProcessRequest& request)
        {
            ClapLiveProcessResult result;
            if (!isOpen() || plugin == nullptr || plugin->process == nullptr)
            {
                result.message = "CLAP live process bridge failed: no open live instance.";
                return result;
            }

            const int frames = std::clamp(request.frameCount > 0 ? request.frameCount : infoState.blockSize, 1, std::max(1, infoState.blockSize));

            LiveEventBlock eventBlock;
            eventBlock.events.reserve(request.noteEvents.size());
            for (const auto& noteEvent : request.noteEvents)
                eventBlock.events.push_back(makeClapLiveNoteEvent(noteEvent, noteDialectSelection.dialect, noteDialectSelection.portIndex, frames));

            std::sort(eventBlock.events.begin(), eventBlock.events.end(), [](const auto& a, const auto& b)
            {
                const auto* ah = a.header();
                const auto* bh = b.header();
                if (ah->time != bh->time)
                    return ah->time < bh->time;
                return ah->type < bh->type;
            });

            clap_input_events_t inputEvents {};
            inputEvents.ctx = &eventBlock;
            inputEvents.size = inputEventsSize;
            inputEvents.get = inputEventsGet;

            ClapOutputEventState outputEventState;
            clap_output_events_t outputEvents {};
            outputEvents.ctx = &outputEventState;
            outputEvents.try_push = outputEventsTryPush;

            LiveOutputStorage outputStorage;
            outputStorage.build(outputChannelCount, frames);

            clap_process_t process {};
            process.frames_count = static_cast<std::uint32_t>(frames);
            process.audio_inputs = nullptr;
            process.audio_inputs_count = 0;
            process.audio_outputs = &outputStorage.buffer;
            process.audio_outputs_count = 1;
            process.in_events = &inputEvents;
            process.out_events = &outputEvents;

            const auto status = plugin->process(plugin, &process);
            result.processStatus = status;
            result.processStatusText = processStatusToString(status);
            result.success = status != clapProcessError;
            result.eventsSubmitted = static_cast<int>(eventBlock.events.size());
            result.outputChannelCount = outputChannelCount;
            result.outputFrameCount = frames;
            result.interleavedAudio.assign(static_cast<std::size_t>(frames * outputChannelCount), 0.0f);

            for (int sample = 0; sample < frames; ++sample)
            {
                for (int channel = 0; channel < outputChannelCount; ++channel)
                {
                    const auto ch = static_cast<std::size_t>(channel);
                    const auto smp = static_cast<std::size_t>(sample);
                    result.interleavedAudio[static_cast<std::size_t>(sample * outputChannelCount + channel)] = outputStorage.samples[ch][smp];
                }
            }

            ++infoState.processedBlocks;
            infoState.lastProcessStatus = status;
            infoState.lastProcessStatusText = result.processStatusText;

            std::ostringstream message;
            message << "CLAP live process bridge block " << infoState.processedBlocks
                    << ": frames " << frames
                    << ", submitted events " << result.eventsSubmitted
                    << ", output channels " << outputChannelCount
                    << ", process status " << result.processStatusText
                    << ", output events " << outputEventState.pushedEvents
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

        ClapLiveInstrumentSessionInfo info() const
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

        ClapLiveInstrumentSessionConfig config;
        ClapLiveInstrumentSessionInfo infoState;
        std::unique_ptr<DynamicLibrary> library;
        const clap_plugin_entry_t* entry = nullptr;
        const clap_plugin_t* plugin = nullptr;
        clap_host_t host {};
        ClapHostRequestState hostState;
        NoteDialectSelection noteDialectSelection;
        int outputChannelCount = 2;
        bool entryInitialized = false;
        bool pluginInitialized = false;
        bool activated = false;
        bool startedProcessing = false;
    };

    ClapLiveInstrumentSession::ClapLiveInstrumentSession()
        : impl(std::make_unique<Impl>())
    {
    }

    ClapLiveInstrumentSession::~ClapLiveInstrumentSession() = default;

    bool ClapLiveInstrumentSession::open(const ClapLiveInstrumentSessionConfig& config, std::string& errorMessage)
    {
        return impl->open(config, errorMessage);
    }

    void ClapLiveInstrumentSession::close()
    {
        impl->close();
    }

    bool ClapLiveInstrumentSession::isOpen() const
    {
        return impl->isOpen();
    }

    ClapLiveInstrumentSessionInfo ClapLiveInstrumentSession::info() const
    {
        return impl->info();
    }

    ClapLiveProcessResult ClapLiveInstrumentSession::processBlock(const ClapLiveProcessRequest& request)
    {
        return impl->processBlock(request);
    }
}
