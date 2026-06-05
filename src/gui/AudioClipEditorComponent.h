#pragma once

#include <juce_gui_extra/juce_gui_extra.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "core/Project.h"

namespace mw::gui
{
    struct WindowPendingCloseHandler
    {
        virtual ~WindowPendingCloseHandler() = default;
        virtual bool requestCloseWithPendingPrompt(std::function<void()> closeAction) = 0;
    };

    std::unique_ptr<juce::Component> createAudioClipEditorComponent(
        const mw::core::Project& project,
        int selectedTrackIndex,
        std::vector<mw::core::AudioClip> audioClipsForTrack,
        std::optional<std::filesystem::path> projectFolder,
        std::function<bool(int, long long, long long)> applyTrimCallback,
        std::function<bool(int)> resetTrimCallback,
        std::function<void(int, long long, long long, bool)> previewClipCallback,
        std::function<void()> stopPreviewCallback,
        std::function<void()> closeCallback);
}
