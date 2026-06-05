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
    struct AudioClipArrangementRenderClip
    {
        int number = 0;
        int sourceClipId = 0;
        long long sourceStartSamples = 0;
        long long sourceEndSamples = 0;
        double arrangementStartSeconds = 0.0;
    };

    struct WindowPendingCloseHandler
    {
        virtual ~WindowPendingCloseHandler() = default;
        virtual bool requestCloseWithPendingPrompt(std::function<void()> closeAction) = 0;
    };

    enum class AudioClipEnhancementPreset
    {
        mildCleanup = 1,
        lowBitrateMp3Repair,
        warmAndSmooth,
        voiceCleanup,
        musicMasteringLite,
        gentleDeHarsh,
        loudnessNormalize
    };

    enum class AudioClipEnhancementAmount
    {
        low = 1,
        medium,
        high
    };

    enum class AudioClipEnhancementAction
    {
        previewEnhanced = 1,
        createEnhancedCopy
    };

    struct AudioClipEnhancementRequest
    {
        int clipId = 0;
        AudioClipEnhancementPreset preset = AudioClipEnhancementPreset::mildCleanup;
        AudioClipEnhancementAmount amount = AudioClipEnhancementAmount::medium;
        AudioClipEnhancementAction action = AudioClipEnhancementAction::previewEnhanced;
    };

    juce::String audioClipEnhancementPresetName(AudioClipEnhancementPreset preset);
    juce::String audioClipEnhancementAmountName(AudioClipEnhancementAmount amount);
    juce::String audioClipEnhancementActionName(AudioClipEnhancementAction action);

    std::unique_ptr<juce::Component> createAudioClipEditorComponent(
        const mw::core::Project& project,
        int selectedTrackIndex,
        std::vector<mw::core::AudioClip> audioClipsForTrack,
        std::optional<std::filesystem::path> projectFolder,
        std::function<bool(int, long long, long long)> applyTrimCallback,
        std::function<bool(int)> resetTrimCallback,
        std::function<void(int, long long, long long, bool)> previewClipCallback,
        std::function<void()> stopPreviewCallback,
        std::function<bool(std::vector<AudioClipArrangementRenderClip>)> previewArrangementCallback,
        std::function<bool(std::vector<AudioClipArrangementRenderClip>)> renderArrangementCallback,
        std::function<bool(AudioClipEnhancementRequest)> enhancementCallback,
        std::function<void()> closeCallback);
}
