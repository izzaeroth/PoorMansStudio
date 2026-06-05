#include "gui/AudioClipEditorComponent.h"
#include "gui/HelperBubbles.h"

#include <juce_audio_utils/juce_audio_utils.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace mw::gui
{
namespace
{
    juce::String formatBytes(std::uintmax_t bytes)
    {
        const double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
        if (mb < 1024.0)
            return juce::String(mb, 1) + " MB";
        return juce::String(mb / 1024.0, 2) + " GB";
    }

    juce::String formatSecondsFromSamples(long long samples, double sampleRate)
    {
        if (sampleRate <= 0.0 || samples <= 0)
            return "0.00s";
        return juce::String(static_cast<double>(samples) / sampleRate, 2) + "s";
    }

    class AudioClipEditorComponentImpl final : public juce::Component,
                                         public WindowPendingCloseHandler
    {
    public:
        AudioClipEditorComponentImpl(
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
            std::function<void()> closeCallback)
            : projectSnapshot(project),
              selectedTrackIndex(selectedTrackIndex),
              clips(std::move(audioClipsForTrack)),
              projectFolder(std::move(projectFolder)),
              onApplyTrim(std::move(applyTrimCallback)),
              onResetTrim(std::move(resetTrimCallback)),
              onPreviewClip(std::move(previewClipCallback)),
              onStopPreview(std::move(stopPreviewCallback)),
              onPreviewArrangement(std::move(previewArrangementCallback)),
              onRenderArrangement(std::move(renderArrangementCallback)),
              onClose(std::move(closeCallback))
        {
            titleLabel.setText("AudioClip Editor", juce::dontSendNotification);
            titleLabel.setJustificationType(juce::Justification::centredLeft);
            titleLabel.setFont(juce::FontOptions(22.0f, juce::Font::bold));

            statusLabel.setText("Phase 4J: drag trim handles, Freeze the kept section, click the larger lane to arrange repeats/overlaps, Preview Arrangement, then render to a new AudioClip track. Source files stay untouched.", juce::dontSendNotification);
            statusLabel.setJustificationType(juce::Justification::centredLeft);
            statusLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
            statusLabel.setFont(juce::FontOptions(13.0f));
            statusLabel.setTooltip("Quick summary of the AudioClip Editor workflow: trim the source, freeze a kept section, place repeats in the lane, preview, then render to a new track.");

            detailsBox.setMultiLine(true);
            detailsBox.setReadOnly(true);
            detailsBox.setScrollbarsShown(true);
            detailsBox.setColour(juce::TextEditor::backgroundColourId, juce::Colours::black);
            detailsBox.setColour(juce::TextEditor::textColourId, juce::Colours::white);
            detailsBox.setColour(juce::TextEditor::outlineColourId, juce::Colours::grey);

            trimHelpLabel.setText("Interactive non-destructive source trim. Drag only the green Start or red End handle, or type seconds, then Apply Trim. Freeze locks the current kept range for arrangement.", juce::dontSendNotification);
            trimHelpLabel.setJustificationType(juce::Justification::centredLeft);
            trimHelpLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
            trimHelpLabel.setFont(juce::FontOptions(12.5f));
            trimHelpLabel.setTooltip("Source trimming is non-destructive. Drag only the green/red handles or type seconds, then Apply Trim to save the source trim metadata.");
            waveformView.setTooltip("Source waveform overview. Drag the green START handle or red END handle only; empty waveform clicks do not move trims.");

            trimStartLabel.setText("Trim Start", juce::dontSendNotification);
            trimStartLabel.setJustificationType(juce::Justification::centredLeft);
            trimStartLabel.setColour(juce::Label::textColourId, juce::Colours::lightgreen);
            trimStartLabel.setTooltip("Start point of the kept source range, measured in seconds from the beginning of the source media.");

            trimEndLabel.setText("Trim End", juce::dontSendNotification);
            trimEndLabel.setJustificationType(juce::Justification::centredLeft);
            trimEndLabel.setColour(juce::Label::textColourId, juce::Colours::red.withAlpha(0.88f));
            trimEndLabel.setTooltip("End point of the kept source range, measured in seconds from the beginning of the source media.");

            trimStartBox.setInputRestrictions(12, "0123456789.");
            trimEndBox.setInputRestrictions(12, "0123456789.");
            trimStartBox.setTooltip("Non-destructive source trim start in seconds from the beginning of the source media.");
            trimEndBox.setTooltip("Non-destructive source trim end in seconds from the beginning of the source media.");
            trimStartBox.onTextChange = [this] { updatePendingTrimFromTextBoxes(); };
            trimEndBox.onTextChange = [this] { updatePendingTrimFromTextBoxes(); };

            previewTrimButton.setButtonText("Preview Trim");
            previewTrimButton.setTooltip("Render/play the pending kept range without changing source files. Scratch files stay in workspace/temp.");
            previewTrimButton.onClick = [this] { previewPendingTrim(); };

            playFullSourceButton.setButtonText("Play Full Source");
            playFullSourceButton.setTooltip("Render/play the full original source range without changing trim metadata.");
            playFullSourceButton.onClick = [this] { previewFullSource(); };

            stopPreviewButton.setButtonText("Stop Preview");
            stopPreviewButton.setTooltip("Stop AudioClip Editor preview playback.");
            stopPreviewButton.onClick = [this]
            {
                if (onStopPreview)
                    onStopPreview();
            };

            applyTrimButton.setButtonText("Apply Trim");
            applyTrimButton.setTooltip("Save the pending trim start/end metadata for this AudioClip. The source media file is not modified.");
            applyTrimButton.onClick = [this] { static_cast<void>(applyTrimFromControls()); };

            resetTrimButton.setButtonText("Reset Trim");
            resetTrimButton.setTooltip("Reset the source trim metadata to the full source range.");
            resetTrimButton.onClick = [this] { resetTrimForCurrentClip(); };

            arrangementHelpLabel.setText("Local arrangement: Freeze the kept trim, click the lane to place/repeat it, Preview Arrangement, or Render To Track. Wider lane = finer placement.", juce::dontSendNotification);
            arrangementHelpLabel.setJustificationType(juce::Justification::centredLeft);
            arrangementHelpLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
            arrangementHelpLabel.setFont(juce::FontOptions(12.5f));
            arrangementHelpLabel.setTooltip("The local arrangement lane places repeated/overlapping copies of the frozen trim. It does not edit the original media file.");
            arrangementView.setTooltip("Arrangement lane. After Freeze Trim, click in the lane to place the frozen section; click again to repeat it. Drag placed clips to move them.");

            freezeClipButton.setButtonText("Freeze Trim");
            freezeClipButton.setTooltip("Lock the current pending trim range so lane clicks can place it into the local arrangement.");
            freezeClipButton.onClick = [this] { freezeCurrentTrimForArrangement(); };

            unfreezeClipButton.setButtonText("Unfreeze");
            unfreezeClipButton.setTooltip("Unlock the trim controls and clear the frozen drag source. Existing arranged clip blocks remain until deleted.");
            unfreezeClipButton.onClick = [this] { unfreezeArrangementTrim(); };

            clipSelectLabel.setText("Clip #", juce::dontSendNotification);
            clipSelectLabel.setJustificationType(juce::Justification::centredRight);
            clipSelectLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.86f));
            clipSelectLabel.setTooltip("Selected arranged clip number. Delete Clip affects this selected clip.");

            clipSelectCombo.setTooltip("Select the arrangement clip targeted by Delete Clip or lane dragging/moving.");
            clipSelectCombo.onChange = [this]
            {
                if (suppressClipSelectChange)
                    return;

                selectedArrangementClipNumber = clipSelectCombo.getSelectedId();
                refreshArrangementControls();
            };

            deleteClipButton.setButtonText("Delete Clip");
            deleteClipButton.setTooltip("Delete the currently selected arrangement clip number.");
            deleteClipButton.onClick = [this] { deleteSelectedArrangementClip(); };

            extendArrangementButton.setButtonText("Extend +10s");
            extendArrangementButton.setTooltip("Add more free-form timeline space to the local arrangement window. Final render will use actual clip end, not this window length.");
            extendArrangementButton.onClick = [this] { extendArrangementLength(10.0); };

            previewArrangementButton.setButtonText("Preview Arrangement");
            previewArrangementButton.setTooltip("Preview the current placed arrangement clips without creating a new track or changing source files.");
            previewArrangementButton.onClick = [this] { previewArrangement(); };

            renderArrangementButton.setButtonText("Render To Track");
            renderArrangementButton.setTooltip("Render the placed arrangement clips to project input audio and create a new imported-style AudioClip track.");
            renderArrangementButton.onClick = [this] { renderArrangementToNewTrack(); };

            arrangementScrollSlider.setSliderStyle(juce::Slider::LinearHorizontal);
            arrangementScrollSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
            arrangementScrollSlider.setTooltip("Free-form horizontal arrangement scroll. This is not paginated.");
            arrangementScrollSlider.onValueChange = [this]
            {
                if (suppressArrangementScrollChange)
                    return;

                arrangementViewStartSeconds = arrangementScrollSlider.getValue();
                refreshArrangementView();
            };

            arrangementStatusLabel.setJustificationType(juce::Justification::centredLeft);
            arrangementStatusLabel.setColour(juce::Label::textColourId, juce::Colours::yellow.withAlpha(0.92f));
            arrangementStatusLabel.setFont(juce::FontOptions(12.0f));
            arrangementStatusLabel.setTooltip("Shows placed clip count, audible arrangement length, visible window length, and frozen trim duration.");

            arrangementView.onFrozenClipDropped = [this](double requestedStartSeconds)
            {
                addFrozenClipToArrangement(requestedStartSeconds);
            };
            arrangementView.onSelectedClipChanged = [this](int clipNumber)
            {
                selectedArrangementClipNumber = clipNumber;
                refreshArrangementControls();
            };
            arrangementView.onClipMoved = [this](int clipNumber, double requestedStartSeconds)
            {
                moveArrangementClip(clipNumber, requestedStartSeconds);
            };

            trimStatusLabel.setJustificationType(juce::Justification::centredLeft);
            trimStatusLabel.setColour(juce::Label::textColourId, juce::Colours::yellow);
            trimStatusLabel.setFont(juce::FontOptions(12.5f));

            closeButton.setButtonText("Close");
            closeButton.setTooltip("Close the AudioClip Editor window.");
            closeButton.onClick = [this]
            {
                requestCloseWithPendingPrompt(onClose);
            };

            waveformView.onTrimRangeChanged = [this](long long startSamples, long long endSamples)
            {
                pendingTrimStartSamples = startSamples;
                pendingTrimEndSamples = endSamples;
                refreshTrimTextBoxesFromPending();
                updateTrimStatusLabel(false);
            };

            selectEditableClip();
            refreshTrimControls();
            refreshArrangementControls();
            detailsBox.setText(buildDetailsText(), juce::dontSendNotification);

            addAndMakeVisible(titleLabel);
            addAndMakeVisible(statusLabel);
            addAndMakeVisible(waveformView);
            addAndMakeVisible(trimHelpLabel);
            addAndMakeVisible(trimStartLabel);
            addAndMakeVisible(trimStartBox);
            addAndMakeVisible(trimEndLabel);
            addAndMakeVisible(trimEndBox);
            addAndMakeVisible(previewTrimButton);
            addAndMakeVisible(playFullSourceButton);
            addAndMakeVisible(stopPreviewButton);
            addAndMakeVisible(applyTrimButton);
            addAndMakeVisible(resetTrimButton);
            addAndMakeVisible(trimStatusLabel);
            addAndMakeVisible(arrangementHelpLabel);
            addAndMakeVisible(arrangementView);
            addAndMakeVisible(freezeClipButton);
            addAndMakeVisible(unfreezeClipButton);
            addAndMakeVisible(clipSelectLabel);
            addAndMakeVisible(clipSelectCombo);
            addAndMakeVisible(deleteClipButton);
            addAndMakeVisible(extendArrangementButton);
            addAndMakeVisible(previewArrangementButton);
            addAndMakeVisible(renderArrangementButton);
            addAndMakeVisible(arrangementScrollSlider);
            addAndMakeVisible(arrangementStatusLabel);
            addAndMakeVisible(detailsBox);
            addAndMakeVisible(closeButton);

            helperTooltipWindow = std::make_unique<mw::gui::FreshHoverTooltipWindow>(this, 2000);
            helperTooltipWindow->setLookAndFeel(&helperTooltipLookAndFeel);
        }

        ~AudioClipEditorComponentImpl() override
        {
            if (helperTooltipWindow != nullptr)
                helperTooltipWindow->setLookAndFeel(nullptr);

            helperTooltipWindow.reset();
        }

        void resized() override
        {
            auto area = getLocalBounds().reduced(16);
            auto top = area.removeFromTop(34);
            closeButton.setBounds(top.removeFromRight(96).reduced(4, 2));
            titleLabel.setBounds(top.reduced(4, 2));
            statusLabel.setBounds(area.removeFromTop(30).reduced(4, 2));
            area.removeFromTop(8);
            waveformView.setBounds(area.removeFromTop(142));
            area.removeFromTop(8);

            trimHelpLabel.setBounds(area.removeFromTop(24).reduced(4, 2));
            auto trimRow = area.removeFromTop(34);
            trimStartLabel.setBounds(trimRow.removeFromLeft(78).reduced(4, 2));
            trimStartBox.setBounds(trimRow.removeFromLeft(92).reduced(4, 4));
            trimEndLabel.setBounds(trimRow.removeFromLeft(68).reduced(4, 2));
            trimEndBox.setBounds(trimRow.removeFromLeft(92).reduced(4, 4));
            applyTrimButton.setBounds(trimRow.removeFromLeft(112).reduced(4, 4));
            resetTrimButton.setBounds(trimRow.removeFromLeft(112).reduced(4, 4));
            trimStatusLabel.setBounds(trimRow.reduced(4, 2));

            auto previewRow = area.removeFromTop(34);
            previewTrimButton.setBounds(previewRow.removeFromLeft(128).reduced(4, 4));
            playFullSourceButton.setBounds(previewRow.removeFromLeft(140).reduced(4, 4));
            stopPreviewButton.setBounds(previewRow.removeFromLeft(118).reduced(4, 4));
            area.removeFromTop(6);

            arrangementHelpLabel.setBounds(area.removeFromTop(24).reduced(4, 2));
            arrangementView.setBounds(area.removeFromTop(218));
            area.removeFromTop(4);

            auto arrangementRow = area.removeFromTop(34);
            freezeClipButton.setBounds(arrangementRow.removeFromLeft(116).reduced(4, 4));
            unfreezeClipButton.setBounds(arrangementRow.removeFromLeft(104).reduced(4, 4));
            clipSelectLabel.setBounds(arrangementRow.removeFromLeft(62).reduced(4, 2));
            clipSelectCombo.setBounds(arrangementRow.removeFromLeft(180).reduced(4, 4));
            deleteClipButton.setBounds(arrangementRow.removeFromLeft(104).reduced(4, 4));
            extendArrangementButton.setBounds(arrangementRow.removeFromLeft(112).reduced(4, 4));

            auto arrangementActionRow = area.removeFromTop(34);
            previewArrangementButton.setBounds(arrangementActionRow.removeFromLeft(170).reduced(4, 4));
            renderArrangementButton.setBounds(arrangementActionRow.removeFromLeft(146).reduced(4, 4));
            arrangementStatusLabel.setBounds(arrangementActionRow.reduced(4, 2));

            auto scrollRow = area.removeFromTop(26);
            arrangementScrollSlider.setBounds(scrollRow.reduced(4, 3));
            area.removeFromTop(6);

            detailsBox.setBounds(area);
        }

        bool requestCloseWithPendingPrompt(std::function<void()> closeAction) override
        {
            if (! hasPendingTrimChanges())
            {
                if (closeAction)
                    closeAction();
                return true;
            }

            auto* alert = new juce::AlertWindow(
                "AudioClip Editor Has Unapplied Trim",
                "The AudioClip Editor has pending source-trim handle/text changes that have not been applied. Apply or discard only affects this original AudioClip trim metadata; it does not delete or roll back any arrangement tracks already created with Render To Track. Apply the trim, discard the pending trim, or keep editing?",
                juce::AlertWindow::WarningIcon
            );

            alert->addButton("Apply Trim and Close", 1, juce::KeyPress(juce::KeyPress::returnKey));
            alert->addButton("Discard Pending and Close", 2);
            alert->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

            alert->enterModalState(
                true,
                juce::ModalCallbackFunction::create(
                    [this, alert, closeAction](int result)
                    {
                        std::unique_ptr<juce::AlertWindow> cleanup(alert);

                        if (result == 1)
                        {
                            if (! applyTrimFromControls())
                            {
                                trimStatusLabel.setText("Pending trim could not be applied; keeping editor open.", juce::dontSendNotification);
                                return;
                            }

                            if (closeAction)
                                closeAction();
                            return;
                        }

                        if (result == 2)
                        {
                            discardPendingTrimChanges();
                            if (closeAction)
                                closeAction();
                            return;
                        }

                        trimStatusLabel.setText("Pending trim changes kept. Apply Trim to commit them.", juce::dontSendNotification);
                    }
                ),
                true
            );

            return false;
        }

        bool hasPendingTrimChanges() const
        {
            const auto* clip = findLocalClip(editableClipId);
            if (clip == nullptr)
                return false;

            return pendingTrimStartSamples != mw::core::audioClipTrimStartSamples(*clip)
                || pendingTrimEndSamples != mw::core::audioClipTrimEndSamples(*clip);
        }

    private:
        class WaveformTrimView final : public juce::Component,
                                   public juce::SettableTooltipClient
        {
        public:
            std::function<void(long long, long long)> onTrimRangeChanged;

            void setClip(const mw::core::AudioClip* sourceClip, const std::optional<std::filesystem::path>& projectFolder)
            {
                hasClip = sourceClip != nullptr;
                peaks.clear();
                activeHandle = Handle::None;

                if (!hasClip)
                {
                    clip = {};
                    pendingStartSamples = 0;
                    pendingEndSamples = 0;
                    repaint();
                    return;
                }

                clip = *sourceClip;
                mw::core::normalizeAudioClipTrim(clip);
                pendingStartSamples = mw::core::audioClipTrimStartSamples(clip);
                pendingEndSamples = mw::core::audioClipTrimEndSamples(clip);
                loadWaveformPeaks(projectFolder);
                repaint();
            }

            void setPendingTrim(long long startSamples, long long endSamples)
            {
                if (!hasClip)
                    return;

                const auto clamped = clampRange(startSamples, endSamples);
                pendingStartSamples = clamped.first;
                pendingEndSamples = clamped.second;
                repaint();
            }

            void setTrimInteractionEnabled(bool enabled)
            {
                trimInteractionEnabled = enabled;
                if (!trimInteractionEnabled)
                    activeHandle = Handle::None;
                repaint();
            }

            void paint(juce::Graphics& g) override
            {
                auto bounds = getLocalBounds().toFloat().reduced(1.0f);
                g.setColour(juce::Colour(0xff171b21));
                g.fillRoundedRectangle(bounds, 9.0f);
                g.setColour(juce::Colour(0xff3b82b6));
                g.drawRoundedRectangle(bounds, 9.0f, 1.5f);

                g.setColour(juce::Colours::lightgrey);
                g.setFont(juce::FontOptions(15.0f, juce::Font::bold));
                g.drawFittedText("AudioClip waveform overview", getLocalBounds().reduced(12), juce::Justification::centredTop, 1);

                const auto wave = waveformBounds();
                g.setColour(juce::Colour(0xff0d1117));
                g.fillRoundedRectangle(wave, 6.0f);
                g.setColour(juce::Colour(0xff263544));
                g.drawRoundedRectangle(wave, 6.0f, 1.0f);

                if (!hasClip || clip.durationSamples <= 0)
                {
                    g.setColour(juce::Colours::lightgrey.withAlpha(0.75f));
                    g.setFont(juce::FontOptions(13.0f));
                    g.drawFittedText("No AudioClip source loaded.", wave.toNearestInt(), juce::Justification::centred, 1);
                    return;
                }

                drawWaveform(g, wave);

                const float startX = sampleToX(pendingStartSamples, wave);
                const float endX = sampleToX(pendingEndSamples, wave);
                const auto kept = juce::Rectangle<float>(startX, wave.getY(), std::max(1.0f, endX - startX), wave.getHeight());

                g.setColour(juce::Colours::black.withAlpha(0.55f));
                if (startX > wave.getX())
                    g.fillRect(juce::Rectangle<float>(wave.getX(), wave.getY(), startX - wave.getX(), wave.getHeight()));
                if (endX < wave.getRight())
                    g.fillRect(juce::Rectangle<float>(endX, wave.getY(), wave.getRight() - endX, wave.getHeight()));

                g.setColour(juce::Colour(0xffffc400).withAlpha(0.11f));
                g.fillRoundedRectangle(kept, 4.0f);
                g.setColour(juce::Colour(0xffffc400).withAlpha(0.65f));
                g.drawRoundedRectangle(kept, 4.0f, 1.0f);

                drawHandle(g, startX, wave, juce::Colours::limegreen, "START");
                drawHandle(g, endX, wave, juce::Colours::red.withAlpha(0.95f), "END");

                g.setColour(juce::Colours::lightgrey.withAlpha(0.9f));
                g.setFont(juce::FontOptions(12.0f));
                const auto fullSeconds = clip.sampleRate > 0.0 ? static_cast<double>(clip.durationSamples) / clip.sampleRate : 0.0;
                const auto keptSeconds = clip.sampleRate > 0.0 ? static_cast<double>(std::max<long long>(0, pendingEndSamples - pendingStartSamples)) / clip.sampleRate : 0.0;
                juce::String footer;
                footer << "Full source: " << juce::String(fullSeconds, 2) << "s   |   Pending kept range: " << juce::String(keptSeconds, 2) << "s";
                g.drawFittedText(footer, getLocalBounds().reduced(12), juce::Justification::centredBottom, 1);

                if (!trimInteractionEnabled)
                {
                    g.setColour(juce::Colours::black.withAlpha(0.50f));
                    const auto badge = juce::Rectangle<float>(wave.getRight() - 118.0f, wave.getY() + 8.0f, 104.0f, 24.0f);
                    g.fillRoundedRectangle(badge, 5.0f);
                    g.setColour(juce::Colours::yellow.withAlpha(0.92f));
                    g.setFont(juce::FontOptions(12.0f, juce::Font::bold));
                    g.drawFittedText("TRIM FROZEN", badge.toNearestInt(), juce::Justification::centred, 1);
                }
            }

            void mouseDown(const juce::MouseEvent& event) override
            {
                if (!trimInteractionEnabled || !hasClip || clip.durationSamples <= 0)
                    return;

                activeHandle = hitTestHandle(event);
                if (activeHandle == Handle::None)
                    return;

                mouseDrag(event);
            }

            void mouseMove(const juce::MouseEvent& event) override
            {
                if (!trimInteractionEnabled)
                {
                    setMouseCursor(juce::MouseCursor::NormalCursor);
                    return;
                }

                setMouseCursor(hitTestHandle(event) == Handle::None
                    ? juce::MouseCursor::NormalCursor
                    : juce::MouseCursor::DraggingHandCursor);
            }

            void mouseExit(const juce::MouseEvent&) override
            {
                setMouseCursor(juce::MouseCursor::NormalCursor);
            }

            void mouseDrag(const juce::MouseEvent& event) override
            {
                if (!trimInteractionEnabled || !hasClip || clip.durationSamples <= 0 || activeHandle == Handle::None)
                    return;

                const auto wave = waveformBounds();
                const auto sample = xToSample(static_cast<float>(event.x), wave);

                if (activeHandle == Handle::Start)
                    pendingStartSamples = std::clamp<long long>(sample, 0, std::max<long long>(0, pendingEndSamples - 1));
                else
                    pendingEndSamples = std::clamp<long long>(sample, std::min<long long>(clip.durationSamples, pendingStartSamples + 1), clip.durationSamples);

                const auto clamped = clampRange(pendingStartSamples, pendingEndSamples);
                pendingStartSamples = clamped.first;
                pendingEndSamples = clamped.second;

                if (onTrimRangeChanged)
                    onTrimRangeChanged(pendingStartSamples, pendingEndSamples);

                repaint();
            }

            void mouseUp(const juce::MouseEvent&) override
            {
                activeHandle = Handle::None;
            }

        private:
            enum class Handle { None, Start, End };

            Handle hitTestHandle(const juce::MouseEvent& event) const
            {
                if (!hasClip || clip.durationSamples <= 0)
                    return Handle::None;

                const auto wave = waveformBounds();
                const auto x = static_cast<float>(event.x);
                const auto y = static_cast<float>(event.y);
                const auto handleVerticalBounds = wave.expanded(0.0f, 24.0f);
                if (!handleVerticalBounds.contains(x, y))
                    return Handle::None;

                const auto startX = sampleToX(pendingStartSamples, wave);
                const auto endX = sampleToX(pendingEndSamples, wave);
                constexpr float handleHitTolerance = 12.0f;
                const auto startDistance = std::abs(x - startX);
                const auto endDistance = std::abs(x - endX);
                const bool startHit = startDistance <= handleHitTolerance;
                const bool endHit = endDistance <= handleHitTolerance;

                if (startHit && endHit)
                    return startDistance <= endDistance ? Handle::Start : Handle::End;

                if (startHit)
                    return Handle::Start;

                if (endHit)
                    return Handle::End;

                return Handle::None;
            }

            juce::Rectangle<float> waveformBounds() const
            {
                auto bounds = getLocalBounds().toFloat().reduced(20.0f, 34.0f);
                bounds.removeFromTop(10.0f);
                bounds.removeFromBottom(10.0f);
                return bounds;
            }

            std::pair<long long, long long> clampRange(long long startSamples, long long endSamples) const
            {
                if (clip.durationSamples <= 0)
                    return { 0, 0 };

                auto start = std::clamp<long long>(startSamples, 0, clip.durationSamples);
                auto end = std::clamp<long long>(endSamples, 0, clip.durationSamples);

                if (end <= start)
                    end = std::min<long long>(clip.durationSamples, start + 1);

                if (end <= start)
                    start = std::max<long long>(0, end - 1);

                return { start, end };
            }

            float sampleToX(long long sample, juce::Rectangle<float> wave) const
            {
                if (clip.durationSamples <= 0)
                    return wave.getX();

                const auto ratio = std::clamp(static_cast<double>(sample) / static_cast<double>(clip.durationSamples), 0.0, 1.0);
                return wave.getX() + static_cast<float>(ratio) * wave.getWidth();
            }

            long long xToSample(float x, juce::Rectangle<float> wave) const
            {
                if (clip.durationSamples <= 0 || wave.getWidth() <= 0.0f)
                    return 0;

                const auto ratio = std::clamp((x - wave.getX()) / wave.getWidth(), 0.0f, 1.0f);
                return static_cast<long long>(std::llround(static_cast<double>(ratio) * static_cast<double>(clip.durationSamples)));
            }

            std::filesystem::path resolvedClipPath(const std::optional<std::filesystem::path>& projectFolder) const
            {
                if (projectFolder.has_value() && !clip.projectRelativePath.empty())
                {
                    const auto absolute = (*projectFolder / clip.projectRelativePath).lexically_normal();
                    if (std::filesystem::exists(absolute))
                        return absolute;
                }

                if (!clip.originalSourcePath.empty() && std::filesystem::exists(clip.originalSourcePath))
                    return clip.originalSourcePath;

                if (!clip.projectRelativePath.empty() && std::filesystem::exists(clip.projectRelativePath))
                    return clip.projectRelativePath;

                return {};
            }

            void loadWaveformPeaks(const std::optional<std::filesystem::path>& projectFolder)
            {
                constexpr int bucketCount = 128;
                peaks.assign(bucketCount, 0.0f);

                const auto path = resolvedClipPath(projectFolder);
                if (path.empty())
                {
                    fillFallbackPeaks();
                    return;
                }

                juce::AudioFormatManager formatManager;
                formatManager.registerBasicFormats();
                std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(juce::File(path.string())));
                if (!reader || reader->lengthInSamples <= 0)
                {
                    fillFallbackPeaks();
                    return;
                }

                const auto sourceLength = static_cast<long long>(reader->lengthInSamples);
                const int blockSize = 2048;
                juce::AudioBuffer<float> buffer(static_cast<int>(std::max<unsigned int>(1, reader->numChannels)), blockSize);

                for (int bucket = 0; bucket < bucketCount; ++bucket)
                {
                    const auto bucketStart = static_cast<long long>((static_cast<double>(bucket) / bucketCount) * sourceLength);
                    const auto bucketEnd = static_cast<long long>((static_cast<double>(bucket + 1) / bucketCount) * sourceLength);
                    auto pos = bucketStart;
                    float peak = 0.0f;

                    while (pos < bucketEnd)
                    {
                        const int toRead = static_cast<int>(std::min<long long>(blockSize, bucketEnd - pos));
                        buffer.clear();
                        reader->read(&buffer, 0, toRead, pos, true, true);

                        const int channels = std::min(buffer.getNumChannels(), 2);
                        for (int ch = 0; ch < channels; ++ch)
                        {
                            const auto* data = buffer.getReadPointer(ch);
                            for (int i = 0; i < toRead; ++i)
                                peak = std::max(peak, std::abs(data[i]));
                        }

                        pos += toRead;
                    }

                    peaks[static_cast<std::size_t>(bucket)] = std::clamp(peak, 0.0f, 1.0f);
                }
            }

            void fillFallbackPeaks()
            {
                if (peaks.empty())
                    peaks.assign(128, 0.0f);

                for (std::size_t i = 0; i < peaks.size(); ++i)
                {
                    const auto phaseA = static_cast<float>(i) * 0.19f;
                    const auto phaseB = static_cast<float>(i) * 0.047f;
                    peaks[i] = std::clamp(0.18f + 0.54f * std::abs(std::sin(phaseA)) + 0.20f * std::abs(std::sin(phaseB)), 0.05f, 1.0f);
                }
            }

            void drawWaveform(juce::Graphics& g, juce::Rectangle<float> wave)
            {
                if (peaks.empty())
                    fillFallbackPeaks();

                const auto centreY = wave.getCentreY();
                const auto usableHeight = wave.getHeight() * 0.86f;
                const auto bucketWidth = wave.getWidth() / static_cast<float>(peaks.size());

                g.setColour(juce::Colour(0xff4db7ff).withAlpha(0.80f));
                for (std::size_t i = 0; i < peaks.size(); ++i)
                {
                    const auto peak = std::max(0.035f, peaks[i]);
                    const auto x = wave.getX() + static_cast<float>(i) * bucketWidth + bucketWidth * 0.48f;
                    const auto h = peak * usableHeight;
                    g.drawLine(x, centreY - h * 0.5f, x, centreY + h * 0.5f, std::max(1.0f, bucketWidth * 0.55f));
                }

                g.setColour(juce::Colour(0xff9ee7ff).withAlpha(0.25f));
                g.drawLine(wave.getX(), centreY, wave.getRight(), centreY, 1.0f);
            }

            void drawHandle(juce::Graphics& g, float x, juce::Rectangle<float> wave, juce::Colour colour, const juce::String& label)
            {
                g.setColour(juce::Colours::black.withAlpha(0.55f));
                g.drawLine(x + 1.2f, wave.getY() - 10.0f, x + 1.2f, wave.getBottom() + 10.0f, 5.0f);
                g.setColour(colour);
                g.drawLine(x, wave.getY() - 10.0f, x, wave.getBottom() + 10.0f, 3.0f);

                juce::Path triangle;
                triangle.startNewSubPath(x, wave.getY() - 12.0f);
                triangle.lineTo(x - 6.0f, wave.getY() - 22.0f);
                triangle.lineTo(x + 6.0f, wave.getY() - 22.0f);
                triangle.closeSubPath();
                g.fillPath(triangle);

                g.setFont(juce::FontOptions(10.0f, juce::Font::bold));
                g.drawFittedText(label, juce::Rectangle<int>(static_cast<int>(x - 28.0f), static_cast<int>(wave.getY() - 34.0f), 56, 12), juce::Justification::centred, 1);
            }

            mw::core::AudioClip clip;
            bool hasClip = false;
            long long pendingStartSamples = 0;
            long long pendingEndSamples = 0;
            std::vector<float> peaks;
            Handle activeHandle = Handle::None;
            bool trimInteractionEnabled = true;
        };


        struct ArrangementClipInstance
        {
            int number = 0;
            int sourceClipId = 0;
            long long sourceStartSamples = 0;
            long long sourceEndSamples = 0;
            double sourceSampleRate = 48000.0;
            double startSeconds = 0.0;
            double durationSeconds = 0.0;
            juce::Colour colour = juce::Colour(0xff87ceeb);
        };

        class ArrangementLaneView final : public juce::Component,
                                      public juce::SettableTooltipClient
        {
        public:
            std::function<void(double)> onFrozenClipDropped;
            std::function<void(int)> onSelectedClipChanged;
            std::function<void(int, double)> onClipMoved;

            void setFrozenSource(bool frozen, long long startSamples, long long endSamples, double sampleRate, juce::String sourceName)
            {
                hasFrozenSource = frozen;
                frozenStartSamples = startSamples;
                frozenEndSamples = endSamples;
                frozenSampleRate = sampleRate;
                frozenSourceName = std::move(sourceName);
                repaint();
            }

            void setArrangementState(std::vector<ArrangementClipInstance> newClips,
                                     int newSelectedClipNumber,
                                     double newArrangementLengthSeconds,
                                     double newViewStartSeconds,
                                     double newViewLengthSeconds)
            {
                clips = std::move(newClips);
                selectedClipNumber = newSelectedClipNumber;
                arrangementLengthSeconds = std::max(1.0, newArrangementLengthSeconds);
                viewStartSeconds = std::max(0.0, newViewStartSeconds);
                viewLengthSeconds = std::max(1.0, newViewLengthSeconds);
                repaint();
            }

            void paint(juce::Graphics& g) override
            {
                auto bounds = getLocalBounds().toFloat().reduced(1.0f);
                g.setColour(juce::Colour(0xff151922));
                g.fillRoundedRectangle(bounds, 8.0f);
                g.setColour(juce::Colour(0xff51606f));
                g.drawRoundedRectangle(bounds, 8.0f, 1.2f);

                const auto source = frozenSourceBounds();
                g.setColour(juce::Colour(0xff0e1319));
                g.fillRoundedRectangle(source, 6.0f);
                g.setColour(juce::Colour(0xff364656));
                g.drawRoundedRectangle(source, 6.0f, 1.0f);

                g.setFont(juce::FontOptions(12.0f, juce::Font::bold));
                if (hasFrozenSource && frozenEndSamples > frozenStartSamples && frozenSampleRate > 0.0)
                {
                    g.setColour(juce::Colours::orange.withAlpha(draggingFrozenSource ? 0.80f : 0.62f));
                    const auto block = source.reduced(8.0f, 7.0f);
                    g.fillRoundedRectangle(block, 5.0f);
                    g.setColour(juce::Colours::white.withAlpha(0.92f));
                    juce::String text;
                    text << "Frozen trim: " << frozenSourceName << "  " << juce::String(frozenDurationSeconds(), 2) << "s  | click lane to place/repeat";
                    g.drawFittedText(text, block.toNearestInt().reduced(8, 0), juce::Justification::centredLeft, 1);
                }
                else
                {
                    g.setColour(juce::Colours::lightgrey.withAlpha(0.78f));
                    g.drawFittedText("Freeze a trim range, then click in the arrangement lane below.", source.toNearestInt().reduced(8, 0), juce::Justification::centredLeft, 1);
                }

                const auto lane = laneBounds();
                g.setColour(juce::Colour(0xff090d12));
                g.fillRoundedRectangle(lane, 6.0f);
                g.setColour(juce::Colour(0xff2a3746));
                g.drawRoundedRectangle(lane, 6.0f, 1.0f);

                drawGrid(g, lane);

                for (const auto& clip : clips)
                    drawArrangementClip(g, lane, clip);

                if (draggingFrozenSource && hasFrozenSource)
                {
                    const auto duration = std::max(0.05, frozenDurationSeconds());
                    auto previewStart = snapStartSeconds(xToSeconds(static_cast<float>(lastMouseX), lane) - duration * 0.5, duration, 0);
                    const auto x = secondsToX(previewStart, lane);
                    const auto w = secondsToWidth(duration, lane);
                    auto preview = juce::Rectangle<float>(x, lane.getY() + 9.0f, std::max(6.0f, w), lane.getHeight() - 18.0f);
                    g.setColour(juce::Colours::orange.withAlpha(frozenDragCanDrop ? 0.30f : 0.16f));
                    g.fillRoundedRectangle(preview, 5.0f);
                    g.setColour(juce::Colours::orange.withAlpha(frozenDragCanDrop ? 0.95f : 0.55f));
                    g.drawRoundedRectangle(preview, 5.0f, 1.4f);
                }

                g.setFont(juce::FontOptions(11.5f));
                g.setColour(juce::Colours::lightgrey.withAlpha(0.86f));
                juce::String footer;
                footer << "View " << juce::String(viewStartSeconds, 2) << "s - " << juce::String(viewStartSeconds + viewLengthSeconds, 2)
                       << "s   |   Window " << juce::String(arrangementLengthSeconds, 2) << "s";
                g.drawFittedText(footer, getLocalBounds().reduced(10), juce::Justification::centredBottom, 1);
            }

            void mouseDown(const juce::MouseEvent& event) override
            {
                lastMouseX = event.x;
                draggingFrozenSource = false;
                frozenDragCanDrop = false;
                draggingClipNumber = 0;

                const auto point = event.position;
                const bool hasValidFrozenSource = hasFrozenSource && frozenEndSamples > frozenStartSamples;
                const int hitClip = hitTestClip(point);
                if (hitClip > 0)
                {
                    draggingClipNumber = hitClip;
                    if (onSelectedClipChanged)
                        onSelectedClipChanged(hitClip);

                    const auto* clip = findClip(hitClip);
                    dragOffsetSeconds = clip != nullptr ? xToSeconds(point.x, laneBounds()) - clip->startSeconds : 0.0;
                    setMouseCursor(juce::MouseCursor::DraggingHandCursor);
                    repaint();
                    return;
                }

                if (hasValidFrozenSource && laneBounds().contains(point) && onFrozenClipDropped)
                {
                    const auto duration = std::max(0.05, frozenDurationSeconds());
                    const auto requestedStart = xToSeconds(point.x, laneBounds()) - duration * 0.5;
                    onFrozenClipDropped(snapStartSeconds(requestedStart, duration, 0));
                    repaint();
                }
            }

            void mouseDrag(const juce::MouseEvent& event) override
            {
                lastMouseX = event.x;

                if (draggingFrozenSource)
                {
                    frozenDragCanDrop = canDropFrozenSourceAt(event.position);
                    repaint();
                    return;
                }

                if (draggingClipNumber > 0 && onClipMoved)
                {
                    const auto* clip = findClip(draggingClipNumber);
                    const double duration = clip != nullptr ? clip->durationSeconds : 0.0;
                    const double requestedStart = xToSeconds(event.position.x, laneBounds()) - dragOffsetSeconds;
                    onClipMoved(draggingClipNumber, snapStartSeconds(requestedStart, duration, draggingClipNumber));
                }

                repaint();
            }

            void mouseUp(const juce::MouseEvent& event) override
            {
                const bool wasDraggingFrozen = draggingFrozenSource;
                const bool shouldDropFrozen = wasDraggingFrozen && canDropFrozenSourceAt(event.position);
                draggingFrozenSource = false;
                frozenDragCanDrop = false;

                if (shouldDropFrozen && onFrozenClipDropped)
                {
                    const auto duration = std::max(0.05, frozenDurationSeconds());
                    const auto requestedStart = xToSeconds(event.position.x, laneBounds()) - duration * 0.5;
                    onFrozenClipDropped(snapStartSeconds(requestedStart, duration, 0));
                }

                draggingClipNumber = 0;
                dragOffsetSeconds = 0.0;
                setMouseCursor(juce::MouseCursor::NormalCursor);
                repaint();
            }

            void mouseMove(const juce::MouseEvent& event) override
            {
                const bool overClip = hitTestClip(event.position) > 0;
                setMouseCursor(overClip ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::NormalCursor);
            }

            void mouseExit(const juce::MouseEvent&) override
            {
                setMouseCursor(juce::MouseCursor::NormalCursor);
            }

        private:
            juce::Rectangle<float> frozenSourceBounds() const
            {
                auto area = getLocalBounds().toFloat().reduced(12.0f, 10.0f);
                return area.removeFromTop(40.0f);
            }

            juce::Rectangle<float> laneBounds() const
            {
                auto area = getLocalBounds().toFloat().reduced(12.0f, 10.0f);
                area.removeFromTop(48.0f);
                area.removeFromBottom(20.0f);
                return area;
            }

            double frozenDurationSeconds() const
            {
                if (frozenSampleRate <= 0.0 || frozenEndSamples <= frozenStartSamples)
                    return 0.0;
                return static_cast<double>(frozenEndSamples - frozenStartSamples) / frozenSampleRate;
            }

            bool canDropFrozenSourceAt(juce::Point<float> point) const
            {
                if (!hasFrozenSource || frozenEndSamples <= frozenStartSamples)
                    return false;

                const auto source = frozenSourceBounds();
                const auto lane = laneBounds();
                return point.y >= source.getBottom() - 2.0f
                    && point.y <= lane.getBottom() + 18.0f
                    && point.x >= lane.getX() - 24.0f
                    && point.x <= lane.getRight() + 24.0f;
            }

            float secondsToX(double seconds, juce::Rectangle<float> lane) const
            {
                const auto ratio = (seconds - viewStartSeconds) / std::max(0.001, viewLengthSeconds);
                return lane.getX() + static_cast<float>(ratio) * lane.getWidth();
            }

            float secondsToWidth(double seconds, juce::Rectangle<float> lane) const
            {
                return static_cast<float>(seconds / std::max(0.001, viewLengthSeconds)) * lane.getWidth();
            }

            double xToSeconds(float x, juce::Rectangle<float> lane) const
            {
                const auto ratio = std::clamp((x - lane.getX()) / std::max(1.0f, lane.getWidth()), 0.0f, 1.0f);
                return viewStartSeconds + static_cast<double>(ratio) * viewLengthSeconds;
            }

            double snapStartSeconds(double requestedStart, double durationSeconds, int movingClipNumber) const
            {
                double start = std::clamp(requestedStart, 0.0, std::max(0.0, arrangementLengthSeconds - std::max(0.0, durationSeconds)));
                constexpr double snapThresholdSeconds = 0.04;
                double bestStart = start;
                double bestDistance = snapThresholdSeconds;

                auto considerMarker = [&](double marker)
                {
                    const double distance = std::abs(start - marker);
                    if (distance < bestDistance)
                    {
                        bestDistance = distance;
                        bestStart = marker;
                    }
                };

                considerMarker(0.0);
                for (const auto& clip : clips)
                {
                    if (clip.number == movingClipNumber)
                        continue;

                    considerMarker(clip.startSeconds);
                    considerMarker(clip.startSeconds + clip.durationSeconds);
                }

                return std::clamp(bestStart, 0.0, std::max(0.0, arrangementLengthSeconds - std::max(0.0, durationSeconds)));
            }

            const ArrangementClipInstance* findClip(int clipNumber) const
            {
                for (const auto& clip : clips)
                    if (clip.number == clipNumber)
                        return &clip;
                return nullptr;
            }

            int hitTestClip(juce::Point<float> point) const
            {
                const auto lane = laneBounds();
                for (auto it = clips.rbegin(); it != clips.rend(); ++it)
                {
                    if (clipRect(lane, *it).contains(point))
                        return it->number;
                }
                return 0;
            }

            juce::Rectangle<float> clipRect(juce::Rectangle<float> lane, const ArrangementClipInstance& clip) const
            {
                const auto x = secondsToX(clip.startSeconds, lane);
                const auto w = secondsToWidth(clip.durationSeconds, lane);
                return juce::Rectangle<float>(x, lane.getY() + 10.0f, std::max(5.0f, w), lane.getHeight() - 20.0f);
            }

            void drawGrid(juce::Graphics& g, juce::Rectangle<float> lane)
            {
                g.setColour(juce::Colours::white.withAlpha(0.08f));
                const double firstSecond = std::floor(viewStartSeconds);
                for (double second = firstSecond; second <= viewStartSeconds + viewLengthSeconds + 0.01; second += 1.0)
                {
                    const auto x = secondsToX(second, lane);
                    g.drawLine(x, lane.getY(), x, lane.getBottom(), std::fmod(second, 5.0) == 0.0 ? 1.2f : 0.6f);
                }

                g.setColour(juce::Colours::white.withAlpha(0.16f));
                const auto zeroX = secondsToX(0.0, lane);
                if (zeroX >= lane.getX() && zeroX <= lane.getRight())
                    g.drawLine(zeroX, lane.getY(), zeroX, lane.getBottom(), 2.0f);
            }

            void drawArrangementClip(juce::Graphics& g, juce::Rectangle<float> lane, const ArrangementClipInstance& clip)
            {
                const auto rect = clipRect(lane, clip);
                if (rect.getRight() < lane.getX() || rect.getX() > lane.getRight())
                    return;

                const auto clipped = rect.getIntersection(lane);
                g.setColour(clip.colour.withAlpha(clip.number == selectedClipNumber ? 0.58f : 0.34f));
                g.fillRoundedRectangle(clipped, 5.0f);
                g.setColour(clip.number == selectedClipNumber ? juce::Colours::white.withAlpha(0.95f) : clip.colour.withAlpha(0.80f));
                g.drawRoundedRectangle(clipped, 5.0f, clip.number == selectedClipNumber ? 2.0f : 1.0f);

                g.setFont(juce::FontOptions(11.0f, juce::Font::bold));
                g.setColour(juce::Colours::white.withAlpha(0.92f));
                juce::String label;
                label << "Clip " << clip.number << "  " << juce::String(clip.startSeconds, 2) << "s";
                g.drawFittedText(label, clipped.toNearestInt().reduced(6, 0), juce::Justification::centredLeft, 1);
            }

            std::vector<ArrangementClipInstance> clips;
            bool hasFrozenSource = false;
            long long frozenStartSamples = 0;
            long long frozenEndSamples = 0;
            double frozenSampleRate = 48000.0;
            juce::String frozenSourceName;
            int selectedClipNumber = 0;
            double arrangementLengthSeconds = 12.0;
            double viewStartSeconds = 0.0;
            double viewLengthSeconds = 8.0;
            bool draggingFrozenSource = false;
            bool frozenDragCanDrop = false;
            int draggingClipNumber = 0;
            double dragOffsetSeconds = 0.0;
            int lastMouseX = 0;
        };

        static juce::String secondsTextForSamples(long long samples, double sampleRate)
        {
            if (sampleRate <= 0.0 || samples <= 0)
                return "0.00";

            return juce::String(static_cast<double>(samples) / sampleRate, 2);
        }

        static long long samplesFromSecondsText(const juce::TextEditor& editor, double sampleRate)
        {
            if (sampleRate <= 0.0)
                return 0;

            const auto seconds = std::max(0.0, editor.getText().trim().getDoubleValue());
            return static_cast<long long>(std::llround(seconds * sampleRate));
        }

        static juce::String formatSecondsFromSamplesLocal(long long samples, double sampleRate)
        {
            return formatSecondsFromSamples(samples, sampleRate);
        }

        void selectEditableClip()
        {
            editableClipId = 0;
            for (const auto& clip : clips)
            {
                if (clip.id > 0)
                {
                    editableClipId = clip.id;
                    return;
                }
            }
        }

        mw::core::AudioClip* findLocalClip(int clipId)
        {
            for (auto& clip : clips)
            {
                if (clip.id == clipId)
                    return &clip;
            }

            return nullptr;
        }

        const mw::core::AudioClip* findLocalClip(int clipId) const
        {
            for (const auto& clip : clips)
            {
                if (clip.id == clipId)
                    return &clip;
            }

            return nullptr;
        }

        void syncProjectSnapshotClip(const mw::core::AudioClip& source)
        {
            for (auto& clip : projectSnapshot.getAudioClips())
            {
                if (clip.id == source.id)
                {
                    clip = source;
                    return;
                }
            }
        }

        void refreshTrimControls()
        {
            const auto* clip = findLocalClip(editableClipId);
            const bool hasEditableClip = clip != nullptr && clip->durationSamples > 0 && clip->sampleRate > 0.0;

            trimStartBox.setEnabled(hasEditableClip && !trimFrozenForArrangement);
            trimEndBox.setEnabled(hasEditableClip && !trimFrozenForArrangement);
            previewTrimButton.setEnabled(hasEditableClip);
            playFullSourceButton.setEnabled(hasEditableClip);
            stopPreviewButton.setEnabled(hasEditableClip);
            applyTrimButton.setEnabled(hasEditableClip);
            resetTrimButton.setEnabled(hasEditableClip && !trimFrozenForArrangement);

            waveformView.setClip(clip, projectFolder);
            waveformView.setTrimInteractionEnabled(hasEditableClip && !trimFrozenForArrangement);

            if (!clip)
            {
                trimFrozenForArrangement = false;
                frozenTrimSourceName.clear();
                suppressTrimTextChange = true;
                trimStartBox.clear();
                trimEndBox.clear();
                suppressTrimTextChange = false;
                pendingTrimStartSamples = 0;
                pendingTrimEndSamples = 0;
                trimStatusLabel.setText("No AudioClip selected.", juce::dontSendNotification);
                return;
            }

            pendingTrimStartSamples = mw::core::audioClipTrimStartSamples(*clip);
            pendingTrimEndSamples = mw::core::audioClipTrimEndSamples(*clip);
            waveformView.setPendingTrim(pendingTrimStartSamples, pendingTrimEndSamples);
            refreshTrimTextBoxesFromPending();
            updateTrimStatusLabel(true);
            refreshArrangementControls();
        }

        void refreshTrimTextBoxesFromPending()
        {
            const auto* clip = findLocalClip(editableClipId);
            if (!clip)
                return;

            suppressTrimTextChange = true;
            trimStartBox.setText(secondsTextForSamples(pendingTrimStartSamples, clip->sampleRate), juce::dontSendNotification);
            trimEndBox.setText(secondsTextForSamples(pendingTrimEndSamples, clip->sampleRate), juce::dontSendNotification);
            suppressTrimTextChange = false;
        }

        void updatePendingTrimFromTextBoxes()
        {
            if (suppressTrimTextChange)
                return;

            auto* clip = findLocalClip(editableClipId);
            if (!clip)
                return;

            auto requestedStart = samplesFromSecondsText(trimStartBox, clip->sampleRate);
            auto requestedEnd = samplesFromSecondsText(trimEndBox, clip->sampleRate);
            requestedStart = std::clamp<long long>(requestedStart, 0, clip->durationSamples);
            requestedEnd = std::clamp<long long>(requestedEnd, 0, clip->durationSamples);

            if (requestedEnd <= requestedStart)
                requestedEnd = std::min<long long>(clip->durationSamples, requestedStart + 1);

            if (requestedEnd <= requestedStart)
                requestedStart = std::max<long long>(0, requestedEnd - 1);

            pendingTrimStartSamples = requestedStart;
            pendingTrimEndSamples = requestedEnd;
            waveformView.setPendingTrim(pendingTrimStartSamples, pendingTrimEndSamples);
            updateTrimStatusLabel(false);
        }

        void updateTrimStatusLabel(bool committed)
        {
            const auto* clip = findLocalClip(editableClipId);
            if (!clip)
                return;

            const auto keptSamples = std::max<long long>(0, pendingTrimEndSamples - pendingTrimStartSamples);
            const bool pendingDiffersFromCommitted = pendingTrimStartSamples != mw::core::audioClipTrimStartSamples(*clip)
                || pendingTrimEndSamples != mw::core::audioClipTrimEndSamples(*clip);

            juce::String text;
            text << (committed || !pendingDiffersFromCommitted ? "Kept: " : "Pending kept: ")
                 << formatSecondsFromSamplesLocal(keptSamples, clip->sampleRate);

            if (pendingDiffersFromCommitted)
                text << " (not applied yet)";
            else
                text << (mw::core::audioClipHasActiveTrim(*clip) ? " (trimmed)" : " (full source)");

            trimStatusLabel.setText(text, juce::dontSendNotification);
        }

        void previewPendingTrim()
        {
            auto* clip = findLocalClip(editableClipId);
            if (!clip || !onPreviewClip)
                return;

            updatePendingTrimFromTextBoxes();
            if (pendingTrimEndSamples <= pendingTrimStartSamples)
            {
                trimStatusLabel.setText("Preview range must be longer than zero.", juce::dontSendNotification);
                return;
            }

            onPreviewClip(clip->id, pendingTrimStartSamples, pendingTrimEndSamples, false);
            trimStatusLabel.setText("Previewing pending trim from workspace/temp.", juce::dontSendNotification);
        }

        void previewFullSource()
        {
            auto* clip = findLocalClip(editableClipId);
            if (!clip || !onPreviewClip)
                return;

            onPreviewClip(clip->id, 0, clip->durationSamples, true);
            trimStatusLabel.setText("Previewing full source from workspace/temp.", juce::dontSendNotification);
        }

        bool applyTrimFromControls()
        {
            auto* clip = findLocalClip(editableClipId);
            if (!clip || !onApplyTrim)
                return false;

            updatePendingTrimFromTextBoxes();
            const auto requestedStart = pendingTrimStartSamples;
            const auto requestedEnd = pendingTrimEndSamples;

            if (requestedEnd <= requestedStart)
            {
                trimStatusLabel.setText("Trim End must be after Trim Start.", juce::dontSendNotification);
                return false;
            }

            if (!onApplyTrim(clip->id, requestedStart, requestedEnd))
            {
                trimStatusLabel.setText("Trim was not applied.", juce::dontSendNotification);
                return false;
            }

            clip->sourceTrimStartSamples = requestedStart;
            clip->sourceTrimEndSamples = requestedEnd;
            mw::core::normalizeAudioClipTrim(*clip);
            pendingTrimStartSamples = mw::core::audioClipTrimStartSamples(*clip);
            pendingTrimEndSamples = mw::core::audioClipTrimEndSamples(*clip);
            syncProjectSnapshotClip(*clip);
            waveformView.setClip(clip, projectFolder);
            waveformView.setPendingTrim(pendingTrimStartSamples, pendingTrimEndSamples);
            refreshTrimTextBoxesFromPending();
            updateTrimStatusLabel(true);
            refreshArrangementControls();
            detailsBox.setText(buildDetailsText(), juce::dontSendNotification);
            return true;
        }

        void discardPendingTrimChanges()
        {
            auto* clip = findLocalClip(editableClipId);
            if (!clip)
                return;

            pendingTrimStartSamples = mw::core::audioClipTrimStartSamples(*clip);
            pendingTrimEndSamples = mw::core::audioClipTrimEndSamples(*clip);
            waveformView.setPendingTrim(pendingTrimStartSamples, pendingTrimEndSamples);
            refreshTrimTextBoxesFromPending();
            updateTrimStatusLabel(true);
            refreshArrangementControls();
            trimStatusLabel.setText("Pending trim changes discarded.", juce::dontSendNotification);
        }

        void resetTrimForCurrentClip()
        {
            auto* clip = findLocalClip(editableClipId);
            if (!clip || !onResetTrim)
                return;

            if (!onResetTrim(clip->id))
            {
                trimStatusLabel.setText("Trim reset was not applied.", juce::dontSendNotification);
                return;
            }

            clip->sourceTrimStartSamples = 0;
            clip->sourceTrimEndSamples = clip->durationSamples;
            mw::core::normalizeAudioClipTrim(*clip);
            pendingTrimStartSamples = mw::core::audioClipTrimStartSamples(*clip);
            pendingTrimEndSamples = mw::core::audioClipTrimEndSamples(*clip);
            syncProjectSnapshotClip(*clip);
            waveformView.setClip(clip, projectFolder);
            waveformView.setPendingTrim(pendingTrimStartSamples, pendingTrimEndSamples);
            refreshTrimTextBoxesFromPending();
            updateTrimStatusLabel(true);
            refreshArrangementControls();
            detailsBox.setText(buildDetailsText(), juce::dontSendNotification);
        }


        static juce::Colour arrangementColourForIndex(int index)
        {
            static const std::array<juce::Colour, 8> colours
            {
                juce::Colour(0xff00bfff),
                juce::Colour(0xffffa500),
                juce::Colour(0xff00fa9a),
                juce::Colour(0xffee82ee),
                juce::Colour(0xffffff00),
                juce::Colour(0xffff69b4),
                juce::Colour(0xff7fffd4),
                juce::Colour(0xffff7f50)
            };

            return colours[static_cast<std::size_t>(std::max(0, index) % static_cast<int>(colours.size()))];
        }

        const ArrangementClipInstance* findArrangementClip(int clipNumber) const
        {
            for (const auto& clip : arrangementClips)
                if (clip.number == clipNumber)
                    return &clip;
            return nullptr;
        }

        ArrangementClipInstance* findArrangementClip(int clipNumber)
        {
            for (auto& clip : arrangementClips)
                if (clip.number == clipNumber)
                    return &clip;
            return nullptr;
        }

        double currentFrozenDurationSeconds() const
        {
            if (!trimFrozenForArrangement || frozenTrimSampleRate <= 0.0 || frozenTrimEndSamples <= frozenTrimStartSamples)
                return 0.0;
            return static_cast<double>(frozenTrimEndSamples - frozenTrimStartSamples) / frozenTrimSampleRate;
        }

        double actualArrangementEndSeconds() const
        {
            double endSeconds = 0.0;
            for (const auto& clip : arrangementClips)
                endSeconds = std::max(endSeconds, clip.startSeconds + std::max(0.0, clip.durationSeconds));
            return endSeconds;
        }

        double snapArrangementStart(double requestedStartSeconds, double clipDurationSeconds, int movingClipNumber) const
        {
            double start = std::clamp(requestedStartSeconds, 0.0, std::max(0.0, arrangementLengthSeconds - std::max(0.0, clipDurationSeconds)));
            constexpr double snapThresholdSeconds = 0.04;
            double bestStart = start;
            double bestDistance = snapThresholdSeconds;

            auto considerMarker = [&](double marker)
            {
                const double distance = std::abs(start - marker);
                if (distance < bestDistance)
                {
                    bestDistance = distance;
                    bestStart = marker;
                }
            };

            considerMarker(0.0);
            for (const auto& clip : arrangementClips)
            {
                if (clip.number == movingClipNumber)
                    continue;
                considerMarker(clip.startSeconds);
                considerMarker(clip.startSeconds + clip.durationSeconds);
            }

            return std::clamp(bestStart, 0.0, std::max(0.0, arrangementLengthSeconds - std::max(0.0, clipDurationSeconds)));
        }

        void freezeCurrentTrimForArrangement()
        {
            auto* clip = findLocalClip(editableClipId);
            if (!clip || clip->sampleRate <= 0.0 || clip->durationSamples <= 0)
                return;

            updatePendingTrimFromTextBoxes();
            if (pendingTrimEndSamples <= pendingTrimStartSamples)
            {
                arrangementStatusLabel.setText("Cannot freeze an empty trim range.", juce::dontSendNotification);
                return;
            }

            trimFrozenForArrangement = true;
            frozenTrimStartSamples = pendingTrimStartSamples;
            frozenTrimEndSamples = pendingTrimEndSamples;
            frozenTrimSampleRate = clip->sampleRate;
            frozenTrimSourceName = clip->name.empty() ? juce::String("AudioClip") : juce::String(clip->name);
            trimStartBox.setEnabled(false);
            trimEndBox.setEnabled(false);
            resetTrimButton.setEnabled(false);
            waveformView.setTrimInteractionEnabled(false);
            updateTrimStatusLabel(false);
            refreshArrangementControls();
        }

        void unfreezeArrangementTrim()
        {
            trimFrozenForArrangement = false;
            frozenTrimStartSamples = 0;
            frozenTrimEndSamples = 0;
            frozenTrimSampleRate = 48000.0;
            frozenTrimSourceName.clear();
            const auto* clip = findLocalClip(editableClipId);
            const bool hasEditableClip = clip != nullptr && clip->durationSamples > 0 && clip->sampleRate > 0.0;
            trimStartBox.setEnabled(hasEditableClip);
            trimEndBox.setEnabled(hasEditableClip);
            resetTrimButton.setEnabled(hasEditableClip);
            waveformView.setTrimInteractionEnabled(hasEditableClip);
            updateTrimStatusLabel(false);
            refreshArrangementControls();
        }

        void addFrozenClipToArrangement(double requestedStartSeconds)
        {
            auto* sourceClip = findLocalClip(editableClipId);
            if (!sourceClip || !trimFrozenForArrangement)
            {
                arrangementStatusLabel.setText("Freeze a trim range before adding arrangement clips.", juce::dontSendNotification);
                return;
            }

            if (arrangementClips.size() >= maxArrangementClips)
            {
                arrangementStatusLabel.setText("Arrangement clip cap reached (64). Delete or render before adding more.", juce::dontSendNotification);
                return;
            }

            const double durationSeconds = currentFrozenDurationSeconds();
            if (durationSeconds <= 0.0)
            {
                arrangementStatusLabel.setText("Frozen trim range is empty.", juce::dontSendNotification);
                return;
            }

            ArrangementClipInstance instance;
            instance.number = nextArrangementClipNumber++;
            instance.sourceClipId = sourceClip->id;
            instance.sourceStartSamples = frozenTrimStartSamples;
            instance.sourceEndSamples = frozenTrimEndSamples;
            instance.sourceSampleRate = frozenTrimSampleRate;
            instance.durationSeconds = durationSeconds;
            instance.startSeconds = snapArrangementStart(requestedStartSeconds, durationSeconds, 0);
            instance.colour = arrangementColourForIndex(instance.number - 1);
            arrangementClips.push_back(instance);
            selectedArrangementClipNumber = instance.number;
            refreshArrangementControls();
        }

        void moveArrangementClip(int clipNumber, double requestedStartSeconds)
        {
            auto* clip = findArrangementClip(clipNumber);
            if (!clip)
                return;

            clip->startSeconds = snapArrangementStart(requestedStartSeconds, clip->durationSeconds, clipNumber);
            selectedArrangementClipNumber = clipNumber;
            refreshArrangementControls();
        }

        void deleteSelectedArrangementClip()
        {
            const auto oldSize = arrangementClips.size();
            arrangementClips.erase(
                std::remove_if(arrangementClips.begin(), arrangementClips.end(), [this](const ArrangementClipInstance& clip)
                {
                    return clip.number == selectedArrangementClipNumber;
                }),
                arrangementClips.end());

            if (arrangementClips.size() == oldSize)
            {
                arrangementStatusLabel.setText("Select an arrangement clip before deleting.", juce::dontSendNotification);
                return;
            }

            selectedArrangementClipNumber = arrangementClips.empty() ? 0 : arrangementClips.back().number;
            refreshArrangementControls();
        }

        void extendArrangementLength(double secondsToAdd)
        {
            arrangementLengthSeconds = std::min(600.0, arrangementLengthSeconds + std::max(1.0, secondsToAdd));
            refreshArrangementControls();
        }

        std::vector<AudioClipArrangementRenderClip> buildArrangementRenderClips() const
        {
            std::vector<AudioClipArrangementRenderClip> renderClips;
            renderClips.reserve(arrangementClips.size());
            for (const auto& clip : arrangementClips)
            {
                AudioClipArrangementRenderClip renderClip;
                renderClip.number = clip.number;
                renderClip.sourceClipId = clip.sourceClipId;
                renderClip.sourceStartSamples = clip.sourceStartSamples;
                renderClip.sourceEndSamples = clip.sourceEndSamples;
                renderClip.arrangementStartSeconds = clip.startSeconds;
                renderClips.push_back(renderClip);
            }

            std::sort(renderClips.begin(), renderClips.end(), [](const auto& a, const auto& b)
            {
                if (a.arrangementStartSeconds == b.arrangementStartSeconds)
                    return a.number < b.number;
                return a.arrangementStartSeconds < b.arrangementStartSeconds;
            });

            return renderClips;
        }

        void previewArrangement()
        {
            if (arrangementClips.empty())
            {
                arrangementStatusLabel.setText("Place at least one arrangement clip before previewing.", juce::dontSendNotification);
                return;
            }

            if (!onPreviewArrangement)
            {
                arrangementStatusLabel.setText("Preview Arrangement callback is unavailable.", juce::dontSendNotification);
                return;
            }

            arrangementStatusLabel.setText("Previewing current arrangement...", juce::dontSendNotification);
            if (!onPreviewArrangement(buildArrangementRenderClips()))
                arrangementStatusLabel.setText("Arrangement preview could not be started. See the main log.", juce::dontSendNotification);
        }

        void renderArrangementToNewTrack()
        {
            if (arrangementClips.empty())
            {
                arrangementStatusLabel.setText("Place at least one arrangement clip before rendering.", juce::dontSendNotification);
                return;
            }

            if (!onRenderArrangement)
            {
                arrangementStatusLabel.setText("Render-to-track callback is unavailable.", juce::dontSendNotification);
                return;
            }

            arrangementStatusLabel.setText("Rendering arrangement to a new AudioClip track...", juce::dontSendNotification);
            if (!onRenderArrangement(buildArrangementRenderClips()))
                arrangementStatusLabel.setText("Arrangement render could not be started. See the main log.", juce::dontSendNotification);
        }

        void updateArrangementScrollRange()
        {
            const double maxScroll = std::max(0.0, arrangementLengthSeconds - arrangementVisibleSeconds);
            arrangementViewStartSeconds = std::clamp(arrangementViewStartSeconds, 0.0, maxScroll);

            suppressArrangementScrollChange = true;
            if (maxScroll > 0.0)
            {
                arrangementScrollSlider.setRange(0.0, maxScroll, 0.001);
                arrangementScrollSlider.setValue(arrangementViewStartSeconds, juce::dontSendNotification);
                arrangementScrollSlider.setEnabled(true);
            }
            else
            {
                arrangementScrollSlider.setRange(0.0, 1.0, 0.001);
                arrangementScrollSlider.setValue(0.0, juce::dontSendNotification);
                arrangementScrollSlider.setEnabled(false);
            }
            suppressArrangementScrollChange = false;
        }

        void refreshArrangementView()
        {
            arrangementView.setFrozenSource(trimFrozenForArrangement, frozenTrimStartSamples, frozenTrimEndSamples, frozenTrimSampleRate, frozenTrimSourceName);
            arrangementView.setArrangementState(arrangementClips, selectedArrangementClipNumber, arrangementLengthSeconds, arrangementViewStartSeconds, arrangementVisibleSeconds);
        }

        void refreshArrangementControls()
        {
            const auto* clip = findLocalClip(editableClipId);
            const bool hasEditableClip = clip != nullptr && clip->durationSamples > 0 && clip->sampleRate > 0.0;

            freezeClipButton.setEnabled(hasEditableClip && !trimFrozenForArrangement);
            unfreezeClipButton.setEnabled(trimFrozenForArrangement);
            deleteClipButton.setEnabled(findArrangementClip(selectedArrangementClipNumber) != nullptr);
            extendArrangementButton.setEnabled(arrangementLengthSeconds < 600.0);
            previewArrangementButton.setEnabled(!arrangementClips.empty());
            renderArrangementButton.setEnabled(!arrangementClips.empty());

            suppressClipSelectChange = true;
            clipSelectCombo.clear(juce::dontSendNotification);
            if (arrangementClips.empty())
            {
                clipSelectCombo.addItem("No clips", 1);
                clipSelectCombo.setSelectedId(1, juce::dontSendNotification);
                clipSelectCombo.setEnabled(false);
                selectedArrangementClipNumber = 0;
            }
            else
            {
                clipSelectCombo.setEnabled(true);
                bool selectedStillExists = false;
                for (const auto& instance : arrangementClips)
                {
                    juce::String item;
                    item << "Clip " << instance.number << " @ " << juce::String(instance.startSeconds, 2) << "s";
                    clipSelectCombo.addItem(item, instance.number);
                    selectedStillExists = selectedStillExists || instance.number == selectedArrangementClipNumber;
                }

                if (!selectedStillExists)
                    selectedArrangementClipNumber = arrangementClips.front().number;

                clipSelectCombo.setSelectedId(selectedArrangementClipNumber, juce::dontSendNotification);
            }
            suppressClipSelectChange = false;

            updateArrangementScrollRange();
            refreshArrangementView();

            juce::String status;
            status << static_cast<int>(arrangementClips.size()) << "/" << static_cast<int>(maxArrangementClips) << " clips";
            status << " | audible end " << juce::String(actualArrangementEndSeconds(), 2) << "s";
            status << " | window " << juce::String(arrangementLengthSeconds, 2) << "s";
            if (trimFrozenForArrangement)
                status << " | frozen " << juce::String(currentFrozenDurationSeconds(), 2) << "s";
            arrangementStatusLabel.setText(status, juce::dontSendNotification);
        }

        juce::String buildDetailsText() const
        {
            juce::String text;
            text << "AudioClip Editor\n";
            text << "================\n\n";

            if (selectedTrackIndex >= 0 && selectedTrackIndex < static_cast<int>(projectSnapshot.getTracks().size()))
            {
                const auto& track = projectSnapshot.getTracks()[static_cast<std::size_t>(selectedTrackIndex)];
                text << "Track #" << (selectedTrackIndex + 1) << ": " << track.getName() << "\n";
                text << "Track Type: " << mw::core::trackTypeToString(track.getTrackType()).c_str() << "\n";
                text << "Muted: " << (track.getMuted() ? "yes" : "no")
                     << "    Solo: " << (track.getSolo() ? "yes" : "no")
                     << "    Volume: " << juce::String(track.getMixerSettings().volume, 2) << "\n\n";
            }

            if (clips.empty())
            {
                text << "No AudioClip media is attached to this selected track.\n";
                return text;
            }

            text << "Attached AudioClip media: " << static_cast<int>(clips.size()) << "\n";
            text << "Editing State: interactive non-destructive trim metadata. Preview/export honors the kept range; original media files are untouched.\n\n";

            for (std::size_t i = 0; i < clips.size(); ++i)
            {
                const auto& clip = clips[i];
                const auto startBeat = static_cast<double>(clip.startTick) / static_cast<double>(mw::core::Project::ticksPerQuarterNote);
                const auto fullDurationSeconds = clip.sampleRate > 0.0
                    ? static_cast<double>(std::max<long long>(0, clip.durationSamples)) / clip.sampleRate
                    : 0.0;
                const auto trimStartSamples = mw::core::audioClipTrimStartSamples(clip);
                const auto trimEndSamples = mw::core::audioClipTrimEndSamples(clip);
                const auto trimmedDurationSamples = mw::core::audioClipTrimmedDurationSamples(clip);
                const auto keptDurationSeconds = clip.sampleRate > 0.0
                    ? static_cast<double>(std::max<long long>(0, trimmedDurationSamples)) / clip.sampleRate
                    : 0.0;
                const auto approximateFullDurationBeats = fullDurationSeconds * static_cast<double>(std::max(1, projectSnapshot.getTempoBpm())) / 60.0;
                const auto approximateKeptDurationBeats = keptDurationSeconds * static_cast<double>(std::max(1, projectSnapshot.getTempoBpm())) / 60.0;
                const auto approximateEndBeat = startBeat + approximateKeptDurationBeats;

                long long sequenceStartTick = 0;
                juce::String sequenceName;
                for (const auto& sequence : projectSnapshot.getSequences())
                {
                    if (sequence.number == clip.sequenceNumber)
                    {
                        sequenceStartTick = sequence.startTick;
                        sequenceName = juce::String(sequence.name).trim();
                        break;
                    }
                }
                const auto sequenceStartBeat = static_cast<double>(sequenceStartTick) / static_cast<double>(mw::core::Project::ticksPerQuarterNote);
                const auto localStartBeat = startBeat - sequenceStartBeat;

                text << "AudioClip media #" << clip.id << "\n";
                text << "------------------\n";
                text << "Name: " << (clip.name.empty() ? juce::String("AudioClip") : juce::String(clip.name)) << "\n";
                text << "Track #: " << (clip.trackIndex + 1) << "\n";
                text << "Sequence #: " << clip.sequenceNumber;
                if (sequenceName.isNotEmpty())
                    text << " - " << sequenceName;
                text << "\n";
                text << "Source Type: " << mw::core::audioClipSourceTypeToString(clip.sourceType).c_str() << "\n";
                text << "Saved Format: " << mw::core::audioClipSavedFormatToString(clip.savedFormat).c_str() << "\n";
                text << "Project Start Beat: " << juce::String(startBeat, 2) << "\n";
                text << "Sequence-Local Start Beat: " << juce::String(localStartBeat, 2) << "\n";
                text << "Approx End Beat (trim-aware): " << juce::String(approximateEndBeat, 2) << "\n";
                text << "Full Source Duration: " << formatSecondsFromSamples(clip.durationSamples, clip.sampleRate)
                     << " / approx " << juce::String(approximateFullDurationBeats, 2) << " beats at project tempo\n";
                text << "Source Trim Start: " << formatSecondsFromSamples(trimStartSamples, clip.sampleRate)
                     << " / sample " << juce::String(trimStartSamples) << "\n";
                text << "Source Trim End: " << formatSecondsFromSamples(trimEndSamples, clip.sampleRate)
                     << " / sample " << juce::String(trimEndSamples) << "\n";
                text << "Kept Trim Duration: " << formatSecondsFromSamples(trimmedDurationSamples, clip.sampleRate)
                     << " / approx " << juce::String(approximateKeptDurationBeats, 2) << " beats at project tempo"
                     << " / samples " << juce::String(trimmedDurationSamples)
                     << (mw::core::audioClipHasActiveTrim(clip) ? " (trimmed)" : " (full source)") << "\n";
                text << "Samples: " << juce::String(clip.durationSamples) << "\n";
                text << "Sample Rate: " << juce::String(clip.sampleRate, 0) << " Hz\n";
                text << "Channels: " << clip.channelCount << "\n";
                text << "Bit Depth: " << clip.bitDepth << "\n";
                text << "Gain: " << juce::String(clip.gain, 2) << "\n";
                text << "Pan: " << juce::String(clip.pan, 2) << "\n";
                text << "Size: " << formatBytes(clip.sizeBytes) << "\n";
                text << "Missing Media: " << (clip.missingMedia ? "YES" : "no") << "\n";

                if (!clip.projectRelativePath.empty())
                {
                    text << "Project Media Path: " << clip.projectRelativePath.string() << "\n";
                    if (projectFolder.has_value())
                    {
                        const auto absolutePath = (*projectFolder / clip.projectRelativePath).lexically_normal();
                        text << "Resolved Media Path: " << absolutePath.string() << "\n";
                    }
                }

                if (!clip.originalSourcePath.empty())
                    text << "Original Source Path: " << clip.originalSourcePath.string() << "\n";

                if (!clip.notes.empty())
                    text << "Notes: " << clip.notes << "\n";

                if (i + 1 < clips.size())
                    text << "\n";
            }

            text << "\nPhase reminders:\n";
            text << "- AudioClip startTick still controls placement on the project/sequence timeline.\n";
            text << "- Source trim start/end only choose which source samples are kept.\n";
            text << "- Phase 4J uses direct handle dragging only for waveform trim changes before Apply.\n";
            text << "- Freeze locks the current pending trim range for local arrangement sketching.\n";
            text << "- Arrangement clips are transparent, editor-local instances capped at 64 clips in this phase.\n";
            text << "- Extend +10s changes the working window only; Preview/Render use actual audible clip end.\n";
            text << "- Repeated lane clicks intentionally place repeated copies of the frozen trim.\n";
            text << "- Preview/export honors the kept source trim range.\n";
            text << "- Original recorded/imported media files are never modified by this editor.\n";

            return text;
        }

        mw::core::Project projectSnapshot;
        int selectedTrackIndex = -1;
        std::vector<mw::core::AudioClip> clips;
        std::optional<std::filesystem::path> projectFolder;
        int editableClipId = 0;
        long long pendingTrimStartSamples = 0;
        long long pendingTrimEndSamples = 0;
        bool suppressTrimTextChange = false;

        static constexpr std::size_t maxArrangementClips = 64;
        bool trimFrozenForArrangement = false;
        long long frozenTrimStartSamples = 0;
        long long frozenTrimEndSamples = 0;
        double frozenTrimSampleRate = 48000.0;
        juce::String frozenTrimSourceName;
        std::vector<ArrangementClipInstance> arrangementClips;
        int selectedArrangementClipNumber = 0;
        int nextArrangementClipNumber = 1;
        double arrangementLengthSeconds = 12.0;
        double arrangementVisibleSeconds = 8.0;
        double arrangementViewStartSeconds = 0.0;
        bool suppressClipSelectChange = false;
        bool suppressArrangementScrollChange = false;

        juce::Label titleLabel;
        juce::Label statusLabel;
        WaveformTrimView waveformView;
        juce::Label trimHelpLabel;
        juce::Label trimStartLabel;
        juce::TextEditor trimStartBox;
        juce::Label trimEndLabel;
        juce::TextEditor trimEndBox;
        juce::TextButton previewTrimButton;
        juce::TextButton playFullSourceButton;
        juce::TextButton stopPreviewButton;
        juce::TextButton applyTrimButton;
        juce::TextButton resetTrimButton;
        juce::Label trimStatusLabel;
        juce::Label arrangementHelpLabel;
        ArrangementLaneView arrangementView;
        juce::TextButton freezeClipButton;
        juce::TextButton unfreezeClipButton;
        juce::Label clipSelectLabel;
        juce::ComboBox clipSelectCombo;
        juce::TextButton deleteClipButton;
        juce::TextButton extendArrangementButton;
        juce::TextButton previewArrangementButton;
        juce::TextButton renderArrangementButton;
        juce::Slider arrangementScrollSlider;
        juce::Label arrangementStatusLabel;
        juce::TextEditor detailsBox;
        juce::TextButton closeButton;

        mw::gui::HelperTooltipLookAndFeel helperTooltipLookAndFeel;
        std::unique_ptr<mw::gui::FreshHoverTooltipWindow> helperTooltipWindow;

        std::function<bool(int, long long, long long)> onApplyTrim;
        std::function<bool(int)> onResetTrim;
        std::function<void(int, long long, long long, bool)> onPreviewClip;
        std::function<void()> onStopPreview;
        std::function<bool(std::vector<AudioClipArrangementRenderClip>)> onPreviewArrangement;
        std::function<bool(std::vector<AudioClipArrangementRenderClip>)> onRenderArrangement;
        std::function<void()> onClose;
    };
}

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
    std::function<void()> closeCallback)
{
    return std::make_unique<AudioClipEditorComponentImpl>(
        project,
        selectedTrackIndex,
        std::move(audioClipsForTrack),
        std::move(projectFolder),
        std::move(applyTrimCallback),
        std::move(resetTrimCallback),
        std::move(previewClipCallback),
        std::move(stopPreviewCallback),
        std::move(previewArrangementCallback),
        std::move(renderArrangementCallback),
        std::move(closeCallback));
}
}
