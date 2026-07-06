#pragma once

#include "core/InstrumentAssignment.h"

#include <cstdint>
#include <memory>
#include <string>

namespace mw::clap
{
    class ClapEffectEditorInstance
    {
    public:
        ~ClapEffectEditorInstance();

        ClapEffectEditorInstance(ClapEffectEditorInstance&&) noexcept;
        ClapEffectEditorInstance& operator=(ClapEffectEditorInstance&&) noexcept;

        ClapEffectEditorInstance(const ClapEffectEditorInstance&) = delete;
        ClapEffectEditorInstance& operator=(const ClapEffectEditorInstance&) = delete;

        bool hasGui() const noexcept;
        bool isFloating() const noexcept;
        bool canResize() const noexcept;
        std::uint32_t width() const noexcept;
        std::uint32_t height() const noexcept;
        std::string statusMessage() const;

        bool attachToParent(void* nativeParentWindow, std::string* error = nullptr);
        bool show(std::string* error = nullptr);
        void hide() noexcept;
        bool resize(std::uint32_t width, std::uint32_t height, std::string* error = nullptr);

        std::string captureStateBase64(std::string* error = nullptr);
        bool restoreStateBase64(const std::string& stateBase64, std::string* error = nullptr);

    private:
        struct Impl;
        explicit ClapEffectEditorInstance(std::unique_ptr<Impl> implIn);
        std::unique_ptr<Impl> impl;

        friend class ClapEffectEditorHost;
    };

    struct ClapEffectEditorOpenResult
    {
        bool success = false;
        bool guiAvailable = false;
        bool guiCreated = false;
        std::string message;
        std::unique_ptr<ClapEffectEditorInstance> instance;
    };

    class ClapEffectEditorHost
    {
    public:
        static ClapEffectEditorOpenResult openEffectEditor(const mw::core::VstPluginAssignment& plugin,
                                                           double sampleRate = 48000.0,
                                                           int blockSize = 512);
    };
}
