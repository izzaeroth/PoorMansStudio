#include "clap/ClapInstrumentHost.h"

#include "clap/ClapPluginScanner.h"
#include "core/Project.h"

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
    constexpr const char* clapNotePortsExtensionId = "clap.note-ports";
    constexpr std::size_t clapNameSize = 256;

    constexpr std::uint16_t clapCoreEventSpaceId = 0;
    constexpr std::uint16_t clapEventNoteOn = 0;
    constexpr std::uint16_t clapEventNoteOff = 1;
    constexpr std::uint16_t clapEventMidi = 10;
    constexpr std::uint32_t clapNoteDialectClap = 1u << 0;
    constexpr std::uint32_t clapNoteDialectMidi = 1u << 1;
    constexpr std::uint32_t clapNoteDialectMidiMpe = 1u << 2;
    constexpr std::uint32_t clapNoteDialectMidi2 = 1u << 3;

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

    struct clap_note_port_info_t
    {
        clap_id id;
        std::uint32_t supported_dialects;
        std::uint32_t preferred_dialect;
        char name[clapNameSize];
    };

    struct clap_host_t;

    struct clap_plugin_note_ports_t
    {
        std::uint32_t (*count)(const clap_plugin_t* plugin, bool is_input);
        bool (*get)(const clap_plugin_t* plugin, std::uint32_t index, bool is_input, clap_note_port_info_t* info);
    };

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

    struct ClapHostRequestState
    {
        bool restartRequested = false;
        bool processRequested = false;
        bool callbackRequested = false;
        bool notePortsRescanRequested = false;
        std::uint32_t notePortsRescanFlags = 0;
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

    struct AudioOutputStorage
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
    };

    struct FlattenedChannelRef
    {
        std::size_t port = 0;
        std::size_t channel = 0;
    };

    enum class NoteEventEncoding
    {
        ClapNote,
        Midi
    };

    struct ScheduledNoteEvent
    {
        NoteEventEncoding encoding = NoteEventEncoding::ClapNote;
        clap_event_note_t clapNote {};
        clap_event_midi_t midi {};

        const clap_event_header_t* header() const
        {
            return encoding == NoteEventEncoding::Midi ? &midi.header : &clapNote.header;
        }
    };

    struct NoteEventBlock
    {
        std::vector<ScheduledNoteEvent> events;
    };

    enum class NoteInputDialect
    {
        ClapNote,
        Midi,
        MidiMpe
    };

    struct NoteInputDialectSelection
    {
        bool supported = true;
        NoteInputDialect dialect = NoteInputDialect::ClapNote;
        std::uint32_t portIndex = 0;
        std::string summary;
    };

    std::string safeString(const char* value)
    {
        return value == nullptr ? std::string() : std::string(value);
    }

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

    std::uint32_t inputEventsSize(const clap_input_events_t* list)
    {
        if (list == nullptr || list->ctx == nullptr)
            return 0;
        const auto* block = static_cast<const NoteEventBlock*>(list->ctx);
        return static_cast<std::uint32_t>(block->events.size());
    }

    const clap_event_header_t* inputEventsGet(const clap_input_events_t* list, std::uint32_t index)
    {
        if (list == nullptr || list->ctx == nullptr)
            return nullptr;
        const auto* block = static_cast<const NoteEventBlock*>(list->ctx);
        if (index >= block->events.size())
            return nullptr;
        return block->events[index].header();
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

    std::int64_t projectEndTickForTrack(const mw::core::Track& track)
    {
        std::int64_t endTick = 0;
        for (const auto& note : track.getNotes())
            endTick = std::max<std::int64_t>(endTick, note.startTick + note.durationTicks);
        return endTick;
    }

    std::vector<std::uint32_t> collectOutputChannelsFromExtension(const clap_plugin_t* plugin, const clap_plugin_audio_ports_t* extension)
    {
        std::vector<std::uint32_t> channels;
        if (plugin == nullptr || extension == nullptr || extension->count == nullptr || extension->get == nullptr)
            return channels;

        constexpr std::uint32_t maxPorts = 16;
        constexpr std::uint32_t maxChannelsPerPort = 32;
        const auto count = std::min(extension->count(plugin, false), maxPorts);
        channels.reserve(count);

        for (std::uint32_t i = 0; i < count; ++i)
        {
            clap_audio_port_info_t info {};
            if (extension->get(plugin, i, false, &info))
                channels.push_back(std::max<std::uint32_t>(1, std::min(info.channel_count, maxChannelsPerPort)));
        }

        return channels;
    }

    std::vector<std::uint32_t> chooseOutputPortLayout(const clap_plugin_t* plugin, int fallbackChannels, bool& audioPortsExtensionAvailable)
    {
        std::vector<std::uint32_t> outputChannels;
        audioPortsExtensionAvailable = false;

        if (plugin != nullptr && plugin->get_extension != nullptr)
        {
            const auto* extension = static_cast<const clap_plugin_audio_ports_t*>(plugin->get_extension(plugin, clapAudioPortsExtensionId));
            if (extension != nullptr && extension->count != nullptr && extension->get != nullptr)
            {
                audioPortsExtensionAvailable = true;
                outputChannels = collectOutputChannelsFromExtension(plugin, extension);
            }
        }

        if (outputChannels.empty())
            outputChannels = { static_cast<std::uint32_t>(juce::jlimit(1, 8, fallbackChannels)) };

        return outputChannels;
    }

    std::string noteDialectName(NoteInputDialect dialect)
    {
        switch (dialect)
        {
            case NoteInputDialect::ClapNote: return "CLAP note";
            case NoteInputDialect::Midi: return "MIDI 1";
            case NoteInputDialect::MidiMpe: return "MIDI/MPE";
            default: return "unknown";
        }
    }

    NoteInputDialectSelection chooseNoteInputDialect(const clap_plugin_t* plugin)
    {
        NoteInputDialectSelection selection;

        if (plugin == nullptr || plugin->get_extension == nullptr)
        {
            selection.summary = "note-ports extension unavailable; using CLAP note events optimistically";
            return selection;
        }

        const auto* extension = static_cast<const clap_plugin_note_ports_t*>(plugin->get_extension(plugin, clapNotePortsExtensionId));
        if (extension == nullptr || extension->count == nullptr || extension->get == nullptr)
        {
            selection.summary = "note-ports extension unavailable; using CLAP note events optimistically";
            return selection;
        }

        const auto count = std::min<std::uint32_t>(extension->count(plugin, true), 16);
        if (count == 0)
        {
            selection.supported = false;
            selection.summary = "note-ports extension reported no input ports";
            return selection;
        }

        NoteInputDialectSelection firstClap;
        firstClap.supported = false;
        NoteInputDialectSelection firstMidi;
        firstMidi.supported = false;
        NoteInputDialectSelection firstMidiMpe;
        firstMidiMpe.supported = false;
        bool sawMidi2Only = false;

        auto makeSelection = [](std::uint32_t port, NoteInputDialect dialect, const char* reason)
        {
            NoteInputDialectSelection result;
            result.supported = true;
            result.dialect = dialect;
            result.portIndex = port;
            result.summary = std::string("note-ports selected ") + noteDialectName(dialect) + " on input port " + std::to_string(port) + " (" + reason + ")";
            return result;
        };

        for (std::uint32_t i = 0; i < count; ++i)
        {
            clap_note_port_info_t info {};
            if (!extension->get(plugin, i, true, &info))
                continue;

            const auto supported = info.supported_dialects;
            const auto preferred = info.preferred_dialect;

            if ((preferred & clapNoteDialectClap) != 0 && (supported & clapNoteDialectClap) != 0)
                return makeSelection(i, NoteInputDialect::ClapNote, "plugin preferred CLAP notes");
            if ((preferred & clapNoteDialectMidi) != 0 && (supported & clapNoteDialectMidi) != 0)
                return makeSelection(i, NoteInputDialect::Midi, "plugin preferred MIDI 1");
            if ((preferred & clapNoteDialectMidiMpe) != 0 && (supported & clapNoteDialectMidiMpe) != 0)
                return makeSelection(i, NoteInputDialect::MidiMpe, "plugin preferred MIDI/MPE");

            if (!firstClap.supported && (supported & clapNoteDialectClap) != 0)
                firstClap = makeSelection(i, NoteInputDialect::ClapNote, "first supported CLAP-note port");
            if (!firstMidi.supported && (supported & clapNoteDialectMidi) != 0)
                firstMidi = makeSelection(i, NoteInputDialect::Midi, "first supported MIDI 1 port");
            if (!firstMidiMpe.supported && (supported & clapNoteDialectMidiMpe) != 0)
                firstMidiMpe = makeSelection(i, NoteInputDialect::MidiMpe, "first supported MIDI/MPE port");
            if ((supported & clapNoteDialectMidi2) != 0)
                sawMidi2Only = true;
        }

        if (firstClap.supported)
            return firstClap;
        if (firstMidi.supported)
            return firstMidi;
        if (firstMidiMpe.supported)
            return firstMidiMpe;

        selection.supported = false;
        selection.summary = sawMidi2Only
            ? "note-ports extension only advertised MIDI 2.0, which this renderer does not emit yet"
            : "note-ports extension did not advertise CLAP note, MIDI 1, or MIDI/MPE input support";
        return selection;
    }

    std::vector<FlattenedChannelRef> flattenChannels(const AudioOutputStorage& storage)
    {
        std::vector<FlattenedChannelRef> refs;
        for (std::size_t port = 0; port < storage.samples.size(); ++port)
            for (std::size_t channel = 0; channel < storage.samples[port].size(); ++channel)
                refs.push_back({ port, channel });
        return refs;
    }

    void copyClapOutputsToJuce(const AudioOutputStorage& outputs, juce::AudioBuffer<float>& dest, int frames)
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

    ScheduledNoteEvent makeClapNoteEvent(std::uint16_t type,
                                         std::uint32_t time,
                                         const mw::core::NoteEvent& note,
                                         std::uint32_t portIndex)
    {
        ScheduledNoteEvent scheduled;
        scheduled.encoding = NoteEventEncoding::ClapNote;
        auto& event = scheduled.clapNote;
        event.header.size = sizeof(clap_event_note_t);
        event.header.time = time;
        event.header.space_id = clapCoreEventSpaceId;
        event.header.type = type;
        event.header.flags = 0;
        event.note_id = -1;
        event.port_index = static_cast<std::int16_t>(std::min<std::uint32_t>(portIndex, 32767u));
        event.channel = static_cast<std::int16_t>(juce::jlimit(0, 15, note.midiChannel - 1));
        event.key = static_cast<std::int16_t>(juce::jlimit(0, 127, note.pitch));
        event.velocity = type == clapEventNoteOff ? 0.0 : static_cast<double>(juce::jlimit(1, 127, note.velocity)) / 127.0;
        return scheduled;
    }

    ScheduledNoteEvent makeMidiNoteEvent(bool noteOn,
                                         std::uint32_t time,
                                         const mw::core::NoteEvent& note,
                                         std::uint32_t portIndex)
    {
        ScheduledNoteEvent scheduled;
        scheduled.encoding = NoteEventEncoding::Midi;
        auto& event = scheduled.midi;
        event.header.size = sizeof(clap_event_midi_t);
        event.header.time = time;
        event.header.space_id = clapCoreEventSpaceId;
        event.header.type = clapEventMidi;
        event.header.flags = 0;
        event.port_index = static_cast<std::uint16_t>(std::min<std::uint32_t>(portIndex, 65535u));

        const auto channel = static_cast<std::uint8_t>(juce::jlimit(0, 15, note.midiChannel - 1));
        const auto key = static_cast<std::uint8_t>(juce::jlimit(0, 127, note.pitch));
        const auto velocity = static_cast<std::uint8_t>(noteOn ? juce::jlimit(1, 127, note.velocity) : 0);
        event.data[0] = static_cast<std::uint8_t>((noteOn ? 0x90 : 0x80) | channel);
        event.data[1] = key;
        event.data[2] = velocity;
        return scheduled;
    }

    void buildNoteEventsForBlock(const mw::core::Track& track,
                                 std::int64_t blockStartSample,
                                 int blockNumSamples,
                                 double samplesPerTick,
                                 const NoteInputDialectSelection& dialectSelection,
                                 NoteEventBlock& block)
    {
        block.events.clear();
        if (samplesPerTick <= 0.0 || blockNumSamples <= 0)
            return;

        const auto blockEndSample = blockStartSample + blockNumSamples;

        for (const auto& note : track.getNotes())
        {
            const auto noteOnSample = static_cast<std::int64_t>(std::llround(static_cast<double>(note.startTick) * samplesPerTick));
            const auto noteOffSample = static_cast<std::int64_t>(std::llround(static_cast<double>(note.startTick + note.durationTicks) * samplesPerTick));

            if (noteOnSample >= blockStartSample && noteOnSample < blockEndSample)
            {
                const auto offset = static_cast<std::uint32_t>(std::max<std::int64_t>(0, noteOnSample - blockStartSample));
                if (dialectSelection.dialect == NoteInputDialect::ClapNote)
                    block.events.push_back(makeClapNoteEvent(clapEventNoteOn, offset, note, dialectSelection.portIndex));
                else
                    block.events.push_back(makeMidiNoteEvent(true, offset, note, dialectSelection.portIndex));
            }

            if (noteOffSample >= blockStartSample && noteOffSample < blockEndSample)
            {
                const auto offset = static_cast<std::uint32_t>(std::max<std::int64_t>(0, noteOffSample - blockStartSample));
                if (dialectSelection.dialect == NoteInputDialect::ClapNote)
                    block.events.push_back(makeClapNoteEvent(clapEventNoteOff, offset, note, dialectSelection.portIndex));
                else
                    block.events.push_back(makeMidiNoteEvent(false, offset, note, dialectSelection.portIndex));
            }
        }

        std::sort(block.events.begin(), block.events.end(), [](const auto& a, const auto& b)
        {
            const auto* ah = a.header();
            const auto* bh = b.header();
            if (ah->time != bh->time)
                return ah->time < bh->time;
            return ah->type < bh->type;
        });
    }
}

namespace mw::clap
{
    ClapInstrumentRenderResult ClapInstrumentHost::renderTrackToWav(const ClapInstrumentRenderRequest& request)
    {
        ClapInstrumentRenderResult result;
        result.wavPath = request.wavOutputPath;
        result.sampleRate = request.sampleRate;
        result.channelCount = request.channelCount;

        if (request.cancelRequested != nullptr && request.cancelRequested->load())
        {
            result.cancelled = true;
            result.message = "CLAP instrument render cancelled before start.";
            return result;
        }

        const auto& assignment = request.track.getInstrument().vst3;
        if (assignment.bundlePath.empty())
        {
            result.message = "CLAP instrument assignment has no plugin path.";
            return result;
        }

        if (request.track.getNotes().empty())
        {
            result.message = "CLAP instrument render skipped because the track has no notes.";
            return result;
        }

        const int channelCount = juce::jlimit(1, 8, request.channelCount);
        const int sampleRate = std::max(1, request.sampleRate);
        const int blockSize = juce::jlimit(64, 8192, request.blockSize);
        result.sampleRate = sampleRate;
        result.channelCount = channelCount;

        auto inspected = ClapPluginScanner::inspectPluginPath(assignment.bundlePath);
        if (!ClapPluginScanner::isOuterClapPlugin(assignment.bundlePath))
        {
            result.message = "CLAP instrument path is not an outer .clap plugin file or bundle: " + assignment.bundlePath.string();
            return result;
        }
        if (inspected.binaryPath.empty())
        {
            result.message = "Could not find a loadable CLAP binary inside instrument plugin: " + assignment.bundlePath.string();
            return result;
        }

        DynamicLibrary library(inspected.binaryPath);
        if (!library.isLoaded())
        {
            result.message = "Could not load CLAP instrument binary";
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

        auto fail = [&](std::string message) -> ClapInstrumentRenderResult&
        {
            cleanup();
            result.message = std::move(message);
            return result;
        };

        try
        {
            auto* entry = static_cast<const clap_plugin_entry_t*>(library.symbol("clap_entry"));
            if (entry == nullptr)
                return fail("Loaded CLAP instrument binary but did not find exported CLAP entry symbol 'clap_entry'.");

            if (entry->init == nullptr || entry->deinit == nullptr || entry->get_factory == nullptr)
                return fail("CLAP instrument entry is incomplete.");

            if (!entry->init(assignment.bundlePath.string().c_str()))
                return fail("CLAP instrument entry init() returned false.");
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
                if ((!assignment.uid.empty() && descId == assignment.uid)
                    || (!assignment.name.empty() && descName == assignment.name))
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
            host.name = "Poor Man's Studio CLAP Instrument Host";
            host.vendor = "Poor Man's Studio";
            host.url = "";
            host.version = "0.66.0";
            host.get_extension = hostGetExtension;
            host.request_restart = hostRequestRestart;
            host.request_process = hostRequestProcess;
            host.request_callback = hostRequestCallback;

            const auto* pluginVoid = factory->create_plugin(factory, &host, selectedDesc->id);
            const auto* plugin = static_cast<const clap_plugin_t*>(pluginVoid);
            if (plugin == nullptr)
                return fail("CLAP plugin factory returned a null instrument instance.");
            createdPlugin = plugin;

            if (plugin->destroy == nullptr)
                return fail("CLAP instrument instance did not expose destroy().");
            if (plugin->init == nullptr)
                return fail("CLAP instrument instance did not expose init().");
            if (!plugin->init(plugin))
                return fail("CLAP instrument init() returned false.");

            if (!assignment.stateBase64.empty())
            {
                std::string stateError;
                if (!loadClapStateBase64(plugin, assignment.stateBase64, stateError))
                    return fail("CLAP instrument saved state restore failed before rendering: " + stateError);
            }

            const auto noteDialectSelection = chooseNoteInputDialect(plugin);
            if (!noteDialectSelection.supported)
                return fail("CLAP instrument does not advertise a supported note input dialect: " + noteDialectSelection.summary + ".");

            if (plugin->activate == nullptr || plugin->deactivate == nullptr)
                return fail("CLAP instrument instance did not expose activate()/deactivate().");
            if (plugin->start_processing == nullptr || plugin->stop_processing == nullptr || plugin->process == nullptr)
                return fail("CLAP instrument instance did not expose the required offline processing callbacks.");

            if (!plugin->activate(plugin, static_cast<double>(sampleRate), 1, static_cast<std::uint32_t>(blockSize)))
                return fail("CLAP instrument activate() returned false.");
            pluginActivated = true;

            bool audioPortsExtensionAvailable = false;
            const auto outputChannels = chooseOutputPortLayout(plugin, channelCount, audioPortsExtensionAvailable);

            if (!plugin->start_processing(plugin))
                return fail("CLAP instrument start_processing() returned false.");
            pluginStartedProcessing = true;

            std::error_code ec;
            std::filesystem::create_directories(request.wavOutputPath.parent_path(), ec);
            ec.clear();
            if (std::filesystem::exists(request.wavOutputPath, ec))
                std::filesystem::remove(request.wavOutputPath, ec);

            juce::WavAudioFormat wavFormat;
            std::unique_ptr<juce::FileOutputStream> stream(juce::File(request.wavOutputPath.string()).createOutputStream());
            if (stream == nullptr || !stream->openedOk())
                return fail("Could not create CLAP instrument output WAV: " + request.wavOutputPath.string());

            std::unique_ptr<juce::AudioFormatWriter> writer(
                wavFormat.createWriterFor(stream.get(), static_cast<double>(sampleRate), static_cast<unsigned int>(channelCount), 24, {}, 0)
            );

            if (writer == nullptr)
                return fail("Could not create WAV writer for CLAP instrument render.");
            stream.release();

            const double ticksPerSecond = static_cast<double>(mw::core::Project::ticksPerQuarterNote) * static_cast<double>(std::max(1, request.tempoBpm)) / 60.0;
            const double samplesPerTick = static_cast<double>(sampleRate) / std::max(1.0, ticksPerSecond);
            const auto endTick = projectEndTickForTrack(request.track);
            const auto tailSamples = static_cast<std::int64_t>(std::max(0.0, request.tailSeconds) * static_cast<double>(sampleRate));
            const auto totalSamples = static_cast<std::int64_t>(std::ceil(static_cast<double>(endTick) * samplesPerTick)) + tailSamples;

            juce::AudioBuffer<float> outputAudio(channelCount, blockSize);
            NoteEventBlock noteBlock;
            clap_input_events_t inputEvents {};
            inputEvents.ctx = &noteBlock;
            inputEvents.size = inputEventsSize;
            inputEvents.get = inputEventsGet;

            ClapOutputEventState outputEventState;
            clap_output_events_t outputEvents {};
            outputEvents.ctx = &outputEventState;
            outputEvents.try_push = outputEventsTryPush;

            std::int64_t sample = 0;
            while (sample < totalSamples)
            {
                if (request.cancelRequested != nullptr && request.cancelRequested->load())
                {
                    cleanup();
                    result.cancelled = true;
                    result.message = "CLAP instrument render cancelled.";
                    return result;
                }

                const int blockSamples = static_cast<int>(std::min<std::int64_t>(blockSize, totalSamples - sample));
                outputAudio.setSize(channelCount, blockSamples, false, false, true);
                outputAudio.clear();

                buildNoteEventsForBlock(request.track, sample, blockSamples, samplesPerTick, noteDialectSelection, noteBlock);

                AudioOutputStorage outputBuffers;
                outputBuffers.build(outputChannels, static_cast<std::uint32_t>(blockSamples));

                clap_process_t process {};
                process.steady_time = -1;
                process.frames_count = static_cast<std::uint32_t>(blockSamples);
                process.transport = nullptr;
                process.audio_inputs = nullptr;
                process.audio_outputs = outputBuffers.buffers.empty() ? nullptr : outputBuffers.buffers.data();
                process.audio_inputs_count = 0;
                process.audio_outputs_count = static_cast<std::uint32_t>(outputBuffers.buffers.size());
                process.in_events = &inputEvents;
                process.out_events = &outputEvents;

                const auto status = plugin->process(plugin, &process);
                result.processStatus = status;
                result.processStatusText = processStatusToString(status);
                if (!isProcessStatusOk(status))
                    return fail("CLAP instrument process() returned " + result.processStatusText + ".");

                copyClapOutputsToJuce(outputBuffers, outputAudio, blockSamples);
                writer->writeFromAudioSampleBuffer(outputAudio, 0, blockSamples);
                ++result.processedBlocks;
                sample += blockSamples;
            }

            writer.reset();
            cleanup();

            const auto pluginName = assignment.name.empty()
                ? assignment.bundlePath.filename().string()
                : assignment.name;

            std::ostringstream message;
            message << "Rendered CLAP instrument track to WAV. Plugin: " << pluginName
                    << "; selected descriptor index: " << selectedIndex
                    << "; audio ports: " << (audioPortsExtensionAvailable ? "extension" : "fallback")
                    << "; output ports: " << outputChannels.size()
                    << "; note dialect: " << noteDialectSelection.summary
                    << "; blocks: " << result.processedBlocks
                    << "; last process status: " << result.processStatusText
                    << "; host restart requested: " << (hostState.restartRequested ? "yes" : "no")
                    << "; host process requested: " << (hostState.processRequested ? "yes" : "no")
                    << "; host callback requested: " << (hostState.callbackRequested ? "yes" : "no")
                    << "; host note-port rescan requested: " << (hostState.notePortsRescanRequested ? "yes" : "no")
                    << "; output: " << result.wavPath.string();

            result.success = true;
            result.message = message.str();
            return result;
        }
        catch (const std::exception& ex)
        {
            cleanup();
            result.message = std::string("CLAP instrument rendering failed: ") + ex.what();
            return result;
        }
        catch (...)
        {
            cleanup();
            result.message = "CLAP instrument rendering failed with an unknown exception.";
            return result;
        }
    }
}
