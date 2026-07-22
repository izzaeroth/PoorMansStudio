#include "clap/ClapEffectEditorHost.h"

#include "clap/ClapPluginScanner.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <limits>
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
    constexpr const char* clapStateExtensionId = "clap.state";
    constexpr const char* clapGuiExtensionId = "clap.gui";
#if defined(_WIN32)
    constexpr const char* clapGuiApi = "win32";
#elif defined(__APPLE__)
    constexpr const char* clapGuiApi = "cocoa";
#else
    constexpr const char* clapGuiApi = "x11";
#endif

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

    struct clap_plugin_entry_t
    {
        clap_version_t clap_version;
        bool (*init)(const char* plugin_path);
        void (*deinit)();
        const void* (*get_factory)(const char* factory_id);
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

    struct clap_window_t
    {
        const char* api;
        union
        {
            void* cocoa;
            void* x11;
            void* win32;
            void* ptr;
        };
    };

    struct clap_gui_resize_hints_t
    {
        bool can_resize_horizontally;
        bool can_resize_vertically;
        bool preserve_aspect_ratio;
        std::uint32_t aspect_ratio_width;
        std::uint32_t aspect_ratio_height;
    };

    struct clap_plugin_gui_t
    {
        bool (*is_api_supported)(const clap_plugin_t* plugin, const char* api, bool is_floating);
        bool (*get_preferred_api)(const clap_plugin_t* plugin, const char** api, bool* is_floating);
        bool (*create)(const clap_plugin_t* plugin, const char* api, bool is_floating);
        void (*destroy)(const clap_plugin_t* plugin);
        bool (*set_scale)(const clap_plugin_t* plugin, double scale);
        bool (*get_size)(const clap_plugin_t* plugin, std::uint32_t* width, std::uint32_t* height);
        bool (*can_resize)(const clap_plugin_t* plugin);
        bool (*get_resize_hints)(const clap_plugin_t* plugin, clap_gui_resize_hints_t* hints);
        bool (*adjust_size)(const clap_plugin_t* plugin, std::uint32_t* width, std::uint32_t* height);
        bool (*set_size)(const clap_plugin_t* plugin, std::uint32_t width, std::uint32_t height);
        bool (*set_parent)(const clap_plugin_t* plugin, const clap_window_t* window);
        bool (*set_transient)(const clap_plugin_t* plugin, const clap_window_t* window);
        void (*suggest_title)(const clap_plugin_t* plugin, const char* title);
        bool (*show)(const clap_plugin_t* plugin);
        bool (*hide)(const clap_plugin_t* plugin);
    };

    struct HostRequestState
    {
        bool restartRequested = false;
        bool processRequested = false;
        bool callbackRequested = false;
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

        bool isLoaded() const noexcept { return handle != nullptr; }

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

    const void* hostGetExtension(const clap_host_t*, const char*)
    {
        return nullptr;
    }

    void hostRequestRestart(const clap_host_t* host)
    {
        if (host != nullptr)
            if (auto* state = static_cast<HostRequestState*>(host->host_data))
                state->restartRequested = true;
    }

    void hostRequestProcess(const clap_host_t* host)
    {
        if (host != nullptr)
            if (auto* state = static_cast<HostRequestState*>(host->host_data))
                state->processRequested = true;
    }

    void hostRequestCallback(const clap_host_t* host)
    {
        if (host != nullptr)
            if (auto* state = static_cast<HostRequestState*>(host->host_data))
                state->callbackRequested = true;
    }

    struct MemoryWriteContext
    {
        std::vector<std::uint8_t> bytes;
    };

    std::int64_t memoryWrite(const clap_ostream_t* stream, const void* buffer, std::uint64_t size)
    {
        if (stream == nullptr || stream->ctx == nullptr || (buffer == nullptr && size > 0))
            return -1;
        if (size > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            return -1;

        auto* context = static_cast<MemoryWriteContext*>(stream->ctx);
        const auto* first = static_cast<const std::uint8_t*>(buffer);
        context->bytes.insert(context->bytes.end(), first, first + static_cast<std::size_t>(size));
        return static_cast<std::int64_t>(size);
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

    std::string encodeBase64(const std::vector<std::uint8_t>& bytes)
    {
        if (bytes.empty())
            return {};
        juce::MemoryBlock block(bytes.data(), bytes.size());
        return block.toBase64Encoding().toStdString();
    }

    bool decodeBase64(const std::string& text, juce::MemoryBlock& block)
    {
        if (text.empty())
            return false;
        return block.fromBase64Encoding(juce::String(text));
    }

    bool savePluginState(const clap_plugin_t* plugin, std::string& stateBase64, std::string* error)
    {
        stateBase64.clear();
        if (plugin == nullptr || plugin->get_extension == nullptr)
        {
            if (error != nullptr)
                *error = "CLAP plugin does not expose get_extension().";
            return false;
        }

        const auto* state = static_cast<const clap_plugin_state_t*>(plugin->get_extension(plugin, clapStateExtensionId));
        if (state == nullptr || state->save == nullptr)
        {
            if (error != nullptr)
                *error = "CLAP plugin does not expose clap.state save().";
            return false;
        }

        MemoryWriteContext context;
        clap_ostream_t stream {};
        stream.ctx = &context;
        stream.write = memoryWrite;
        if (!state->save(plugin, &stream))
        {
            if (error != nullptr)
                *error = "CLAP state save() returned false.";
            return false;
        }

        if (context.bytes.empty())
        {
            if (error != nullptr)
                *error = "CLAP state save() returned an empty state blob.";
            return false;
        }

        stateBase64 = encodeBase64(context.bytes);
        return !stateBase64.empty();
    }

    bool loadPluginState(const clap_plugin_t* plugin, const std::string& stateBase64, std::string* error)
    {
        if (stateBase64.empty())
            return true;
        if (plugin == nullptr || plugin->get_extension == nullptr)
        {
            if (error != nullptr)
                *error = "CLAP plugin does not expose get_extension().";
            return false;
        }

        const auto* state = static_cast<const clap_plugin_state_t*>(plugin->get_extension(plugin, clapStateExtensionId));
        if (state == nullptr || state->load == nullptr)
        {
            if (error != nullptr)
                *error = "CLAP plugin does not expose clap.state load().";
            return false;
        }

        juce::MemoryBlock block;
        if (!decodeBase64(stateBase64, block) || block.getSize() == 0)
        {
            if (error != nullptr)
                *error = "Saved CLAP state is not valid base64 data.";
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
            if (error != nullptr)
                *error = "CLAP state load() returned false.";
            return false;
        }
        return true;
    }
}

namespace mw::clap
{
    struct ClapEffectEditorInstance::Impl
    {
        std::unique_ptr<DynamicLibrary> library;
        const clap_plugin_entry_t* initializedEntry = nullptr;
        const clap_plugin_t* plugin = nullptr;
        const clap_plugin_gui_t* gui = nullptr;
        HostRequestState hostState;
        clap_host_t host {};
        bool guiCreated = false;
        bool guiShown = false;
        bool floating = false;
        bool canResizeEditor = false;
        std::uint32_t editorWidth = 640;
        std::uint32_t editorHeight = 420;
        std::string message;

        ~Impl()
        {
            cleanup();
        }

        void cleanup() noexcept
        {
            try
            {
                if (plugin != nullptr && gui != nullptr && guiShown && gui->hide != nullptr)
                {
                    gui->hide(plugin);
                    guiShown = false;
                }

                if (plugin != nullptr && gui != nullptr && guiCreated && gui->destroy != nullptr)
                {
                    gui->destroy(plugin);
                    guiCreated = false;
                }

                if (plugin != nullptr && plugin->destroy != nullptr)
                {
                    plugin->destroy(plugin);
                    plugin = nullptr;
                }

                if (initializedEntry != nullptr && initializedEntry->deinit != nullptr)
                {
                    initializedEntry->deinit();
                    initializedEntry = nullptr;
                }

                library.reset();
            }
            catch (...)
            {
            }
        }
    };

    ClapEffectEditorInstance::ClapEffectEditorInstance(std::unique_ptr<Impl> implIn)
        : impl(std::move(implIn))
    {
    }

    ClapEffectEditorInstance::~ClapEffectEditorInstance() = default;
    ClapEffectEditorInstance::ClapEffectEditorInstance(ClapEffectEditorInstance&&) noexcept = default;
    ClapEffectEditorInstance& ClapEffectEditorInstance::operator=(ClapEffectEditorInstance&&) noexcept = default;

    bool ClapEffectEditorInstance::hasGui() const noexcept
    {
        return impl != nullptr && impl->gui != nullptr && impl->guiCreated;
    }

    bool ClapEffectEditorInstance::isFloating() const noexcept
    {
        return impl != nullptr && impl->floating;
    }

    bool ClapEffectEditorInstance::canResize() const noexcept
    {
        return impl != nullptr && impl->canResizeEditor;
    }

    std::uint32_t ClapEffectEditorInstance::width() const noexcept
    {
        return impl != nullptr ? impl->editorWidth : 640;
    }

    std::uint32_t ClapEffectEditorInstance::height() const noexcept
    {
        return impl != nullptr ? impl->editorHeight : 420;
    }

    std::string ClapEffectEditorInstance::statusMessage() const
    {
        return impl != nullptr ? impl->message : std::string{};
    }

    bool ClapEffectEditorInstance::attachToParent(void* nativeParentWindow, std::string* error)
    {
        if (impl == nullptr || impl->plugin == nullptr || impl->gui == nullptr || !impl->guiCreated)
            return true;

        if (impl->floating)
            return true;

        if (nativeParentWindow == nullptr)
        {
            if (error != nullptr)
                *error = "Native parent window handle was not available.";
            return false;
        }

        if (impl->gui->set_parent == nullptr)
        {
            if (error != nullptr)
                *error = "CLAP GUI does not expose set_parent().";
            return false;
        }

        clap_window_t parent {};
        parent.api = clapGuiApi;
#if defined(_WIN32)
        parent.win32 = nativeParentWindow;
#elif defined(__APPLE__)
        parent.cocoa = nativeParentWindow;
#else
        parent.x11 = nativeParentWindow;
#endif
        if (!impl->gui->set_parent(impl->plugin, &parent))
        {
            if (error != nullptr)
                *error = "CLAP GUI set_parent() returned false.";
            return false;
        }
        return true;
    }

    bool ClapEffectEditorInstance::show(std::string* error)
    {
        if (impl == nullptr || impl->plugin == nullptr || impl->gui == nullptr || !impl->guiCreated)
            return true;
        if (impl->gui->show == nullptr)
            return true;
        if (!impl->gui->show(impl->plugin))
        {
            if (error != nullptr)
                *error = "CLAP GUI show() returned false.";
            return false;
        }
        impl->guiShown = true;
        return true;
    }

    void ClapEffectEditorInstance::hide() noexcept
    {
        if (impl == nullptr || impl->plugin == nullptr || impl->gui == nullptr || !impl->guiShown || impl->gui->hide == nullptr)
            return;
        try
        {
            impl->gui->hide(impl->plugin);
            impl->guiShown = false;
        }
        catch (...)
        {
        }
    }

    bool ClapEffectEditorInstance::resize(std::uint32_t widthIn, std::uint32_t heightIn, std::string* error)
    {
        if (impl == nullptr || impl->plugin == nullptr || impl->gui == nullptr || !impl->guiCreated)
            return true;

        // Some CLAP plugins advertise fixed-size GUIs. Treat that flag as
        // authoritative and do not send set_size() during host-window resize;
        // several plugins become visually detached or unstable if a fixed-size
        // editor is forced to resize. The JUCE host window can still be resized
        // around the fixed native viewport, matching the VST3 editor behavior.
        if (!impl->canResizeEditor)
            return true;

        std::uint32_t adjustedWidth = std::max<std::uint32_t>(120, widthIn);
        std::uint32_t adjustedHeight = std::max<std::uint32_t>(90, heightIn);

        if (impl->gui->adjust_size != nullptr)
            impl->gui->adjust_size(impl->plugin, &adjustedWidth, &adjustedHeight);

        if (impl->gui->set_size != nullptr && !impl->gui->set_size(impl->plugin, adjustedWidth, adjustedHeight))
        {
            if (error != nullptr)
                *error = "CLAP GUI set_size() returned false.";
            return false;
        }

        impl->editorWidth = adjustedWidth;
        impl->editorHeight = adjustedHeight;
        return true;
    }

    std::string ClapEffectEditorInstance::captureStateBase64(std::string* error)
    {
        std::string state;
        if (impl == nullptr || impl->plugin == nullptr)
        {
            if (error != nullptr)
                *error = "CLAP editor instance is not loaded.";
            return {};
        }

        if (impl->plugin->on_main_thread != nullptr)
            impl->plugin->on_main_thread(impl->plugin);

        if (!savePluginState(impl->plugin, state, error))
            return {};
        return state;
    }

    bool ClapEffectEditorInstance::restoreStateBase64(const std::string& stateBase64, std::string* error)
    {
        if (impl == nullptr || impl->plugin == nullptr)
        {
            if (error != nullptr)
                *error = "CLAP editor instance is not loaded.";
            return false;
        }

        const bool ok = loadPluginState(impl->plugin, stateBase64, error);
        if (ok && impl->plugin->on_main_thread != nullptr)
            impl->plugin->on_main_thread(impl->plugin);
        return ok;
    }

    ClapEffectEditorOpenResult ClapEffectEditorHost::openEffectEditor(const mw::core::VstPluginAssignment& pluginAssignment,
                                                                       double,
                                                                       int)
    {
        ClapEffectEditorOpenResult result;
        if (pluginAssignment.bundlePath.empty())
        {
            result.message = "CLAP effect assignment has no plugin path.";
            return result;
        }

        if (!ClapPluginScanner::isOuterClapPlugin(pluginAssignment.bundlePath))
        {
            result.message = "CLAP effect path is not an outer .clap plugin file or bundle: " + pluginAssignment.bundlePath.string();
            return result;
        }

        auto inspected = ClapPluginScanner::inspectPluginPath(pluginAssignment.bundlePath);
        if (inspected.binaryPath.empty())
        {
            result.message = "Could not find a loadable CLAP binary inside effect plugin: " + pluginAssignment.bundlePath.string();
            return result;
        }

        auto impl = std::make_unique<ClapEffectEditorInstance::Impl>();
        impl->library = std::make_unique<DynamicLibrary>(inspected.binaryPath);
        if (!impl->library->isLoaded())
        {
            result.message = "Could not load CLAP effect binary";
            const auto error = impl->library->lastErrorText();
            if (!error.empty())
                result.message += ": " + error;
            return result;
        }

        try
        {
            auto* entry = static_cast<const clap_plugin_entry_t*>(impl->library->symbol("clap_entry"));
            if (entry == nullptr)
            {
                result.message = "Loaded CLAP effect binary but did not find exported CLAP entry symbol 'clap_entry'.";
                return result;
            }
            if (entry->init == nullptr || entry->deinit == nullptr || entry->get_factory == nullptr)
            {
                result.message = "CLAP effect entry is incomplete.";
                return result;
            }
            if (!entry->init(pluginAssignment.bundlePath.string().c_str()))
            {
                result.message = "CLAP effect entry init() returned false.";
                return result;
            }
            impl->initializedEntry = entry;

            const auto* factory = static_cast<const clap_plugin_factory_t*>(entry->get_factory(clapPluginFactoryId));
            if (factory == nullptr || factory->get_plugin_count == nullptr || factory->get_plugin_descriptor == nullptr || factory->create_plugin == nullptr)
            {
                result.message = "CLAP plugin factory was not available or did not expose create_plugin().";
                return result;
            }

            const auto count = factory->get_plugin_count(factory);
            if (count == 0)
            {
                result.message = "CLAP plugin factory reported zero plugins.";
                return result;
            }

            const clap_plugin_descriptor_t* selectedDesc = nullptr;
            for (std::uint32_t i = 0; i < count; ++i)
            {
                const auto* desc = factory->get_plugin_descriptor(factory, i);
                if (desc == nullptr)
                    continue;

                const auto descId = safeString(desc->id);
                const auto descName = safeString(desc->name);
                if ((!pluginAssignment.uid.empty() && descId == pluginAssignment.uid)
                    || (!pluginAssignment.name.empty() && descName == pluginAssignment.name))
                {
                    selectedDesc = desc;
                    break;
                }

                if (selectedDesc == nullptr)
                    selectedDesc = desc;
            }

            if (selectedDesc == nullptr || selectedDesc->id == nullptr || std::string(selectedDesc->id).empty())
            {
                result.message = "CLAP plugin factory did not return a usable descriptor id.";
                return result;
            }

            impl->host.clap_version = { 1, 1, 0 };
            impl->host.host_data = &impl->hostState;
            impl->host.name = "Poor Man's Studio CLAP Editor Host";
            impl->host.vendor = "Poor Man's Studio";
            impl->host.url = "";
            impl->host.version = "0.66.6";
            impl->host.get_extension = hostGetExtension;
            impl->host.request_restart = hostRequestRestart;
            impl->host.request_process = hostRequestProcess;
            impl->host.request_callback = hostRequestCallback;

            const auto* pluginVoid = factory->create_plugin(factory, &impl->host, selectedDesc->id);
            impl->plugin = static_cast<const clap_plugin_t*>(pluginVoid);
            if (impl->plugin == nullptr)
            {
                result.message = "CLAP plugin factory returned a null effect instance.";
                return result;
            }
            if (impl->plugin->destroy == nullptr || impl->plugin->init == nullptr)
            {
                result.message = "CLAP effect instance did not expose required init()/destroy() callbacks.";
                return result;
            }
            if (!impl->plugin->init(impl->plugin))
            {
                result.message = "CLAP effect init() returned false.";
                return result;
            }

            if (!pluginAssignment.stateBase64.empty())
            {
                std::string stateError;
                if (!loadPluginState(impl->plugin, pluginAssignment.stateBase64, &stateError))
                {
                    result.message = "CLAP effect loaded, but saved state could not be restored: " + stateError;
                    return result;
                }
            }

            if (impl->plugin->get_extension != nullptr)
                impl->gui = static_cast<const clap_plugin_gui_t*>(impl->plugin->get_extension(impl->plugin, clapGuiExtensionId));

            if (impl->gui != nullptr)
            {
                bool supportedEmbedded = true;
                if (impl->gui->is_api_supported != nullptr)
                    supportedEmbedded = impl->gui->is_api_supported(impl->plugin, clapGuiApi, false);

                bool createFloating = false;
                if (!supportedEmbedded)
                {
                    createFloating = impl->gui->is_api_supported == nullptr
                        ? true
                        : impl->gui->is_api_supported(impl->plugin, clapGuiApi, true);
                }

                if (supportedEmbedded || createFloating)
                {
                    if (impl->gui->create != nullptr && impl->gui->create(impl->plugin, clapGuiApi, createFloating))
                    {
                        impl->guiCreated = true;
                        impl->floating = createFloating;
                        result.guiCreated = true;

                        if (impl->gui->get_size != nullptr)
                        {
                            std::uint32_t width = 0;
                            std::uint32_t height = 0;
                            if (impl->gui->get_size(impl->plugin, &width, &height) && width > 0 && height > 0)
                            {
                                impl->editorWidth = std::clamp<std::uint32_t>(width, 240, 1800);
                                impl->editorHeight = std::clamp<std::uint32_t>(height, 160, 1200);
                            }
                        }

                        impl->canResizeEditor = impl->gui->can_resize != nullptr && impl->gui->can_resize(impl->plugin);
                        if (impl->gui->suggest_title != nullptr)
                        {
                            const auto title = pluginAssignment.name.empty() ? pluginAssignment.bundlePath.stem().string() : pluginAssignment.name;
                            impl->gui->suggest_title(impl->plugin, title.c_str());
                        }
                    }
                    else
                    {
                        impl->message = "CLAP GUI extension was found, but create() returned false.";
                    }
                }
                else
                {
                    impl->message = "CLAP GUI extension was found, but it does not support the host window API.";
                }
            }
            else
            {
                impl->message = "CLAP plugin does not expose the clap.gui extension.";
            }

            result.success = true;
            result.guiAvailable = impl->gui != nullptr;
            result.guiCreated = impl->guiCreated;
            result.message = impl->guiCreated
                ? std::string("CLAP editor instance opened.")
                : (impl->message.empty() ? std::string("CLAP editor instance opened without a GUI extension.") : impl->message);
            result.instance = std::unique_ptr<ClapEffectEditorInstance>(new ClapEffectEditorInstance(std::move(impl)));
            return result;
        }
        catch (const std::exception& ex)
        {
            result.message = std::string("CLAP editor open failed: ") + ex.what();
            return result;
        }
        catch (...)
        {
            result.message = "CLAP editor open failed with an unknown exception.";
            return result;
        }
    }
}
