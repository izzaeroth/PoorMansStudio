#include "gui/AudioClipEditorComponent.h"

#include <juce_audio_utils/juce_audio_utils.h>

#include <algorithm>
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
            std::function<void()> closeCallback)
            : projectSnapshot(project),
              selectedTrackIndex(selectedTrackIndex),
              clips(std::move(audioClipsForTrack)),
              projectFolder(std::move(projectFolder)),
              onApplyTrim(std::move(applyTrimCallback)),
              onResetTrim(std::move(resetTrimCallback)),
              onPreviewClip(std::move(previewClipCallback)),
              onStopPreview(std::move(stopPreviewCallback)),
              onClose(std::move(closeCallback))
        {
            titleLabel.setText("AudioClip Editor", juce::dontSendNotification);
            titleLabel.setJustificationType(juce::Justification::centredLeft);
            titleLabel.setFont(juce::FontOptions(22.0f, juce::Font::bold));

            statusLabel.setText("Phase 4A: drag green/red trim handles, preview the pending range, then Apply Trim. Source files stay untouched.", juce::dontSendNotification);
            statusLabel.setJustificationType(juce::Justification::centredLeft);
            statusLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
            statusLabel.setFont(juce::FontOptions(13.0f));

            detailsBox.setMultiLine(true);
            detailsBox.setReadOnly(true);
            detailsBox.setScrollbarsShown(true);
            detailsBox.setColour(juce::TextEditor::backgroundColourId, juce::Colours::black);
            detailsBox.setColour(juce::TextEditor::textColourId, juce::Colours::white);
            detailsBox.setColour(juce::TextEditor::outlineColourId, juce::Colours::grey);

            trimHelpLabel.setText("Interactive non-destructive source trim. Drag green Start and red End, or type seconds, then Apply Trim.", juce::dontSendNotification);
            trimHelpLabel.setJustificationType(juce::Justification::centredLeft);
            trimHelpLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
            trimHelpLabel.setFont(juce::FontOptions(12.5f));

            trimStartLabel.setText("Trim Start", juce::dontSendNotification);
            trimStartLabel.setJustificationType(juce::Justification::centredLeft);
            trimStartLabel.setColour(juce::Label::textColourId, juce::Colours::lightgreen);

            trimEndLabel.setText("Trim End", juce::dontSendNotification);
            trimEndLabel.setJustificationType(juce::Justification::centredLeft);
            trimEndLabel.setColour(juce::Label::textColourId, juce::Colours::red.withAlpha(0.88f));

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
            addAndMakeVisible(detailsBox);
            addAndMakeVisible(closeButton);
        }

        void resized() override
        {
            auto area = getLocalBounds().reduced(16);
            auto top = area.removeFromTop(34);
            closeButton.setBounds(top.removeFromRight(96).reduced(4, 2));
            titleLabel.setBounds(top.reduced(4, 2));
            statusLabel.setBounds(area.removeFromTop(30).reduced(4, 2));
            area.removeFromTop(8);
            waveformView.setBounds(area.removeFromTop(180));
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
                "The AudioClip Editor has pending trim handle/text changes that have not been applied. Apply them, discard them, or keep editing?",
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
        class WaveformTrimView final : public juce::Component
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
            }

            void mouseDown(const juce::MouseEvent& event) override
            {
                if (!hasClip || clip.durationSamples <= 0)
                    return;

                const auto wave = waveformBounds();
                const auto x = static_cast<float>(event.x);
                const auto startX = sampleToX(pendingStartSamples, wave);
                const auto endX = sampleToX(pendingEndSamples, wave);

                if (std::abs(x - startX) <= std::abs(x - endX))
                    activeHandle = Handle::Start;
                else
                    activeHandle = Handle::End;

                mouseDrag(event);
            }

            void mouseDrag(const juce::MouseEvent& event) override
            {
                if (!hasClip || clip.durationSamples <= 0 || activeHandle == Handle::None)
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

            trimStartBox.setEnabled(hasEditableClip);
            trimEndBox.setEnabled(hasEditableClip);
            previewTrimButton.setEnabled(hasEditableClip);
            playFullSourceButton.setEnabled(hasEditableClip);
            stopPreviewButton.setEnabled(hasEditableClip);
            applyTrimButton.setEnabled(hasEditableClip);
            resetTrimButton.setEnabled(hasEditableClip);

            waveformView.setClip(clip, projectFolder);

            if (!clip)
            {
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
            detailsBox.setText(buildDetailsText(), juce::dontSendNotification);
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
            text << "- Phase 4A lets handles preview pending trim before Apply.\n";
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
        juce::TextEditor detailsBox;
        juce::TextButton closeButton;
        std::function<bool(int, long long, long long)> onApplyTrim;
        std::function<bool(int)> onResetTrim;
        std::function<void(int, long long, long long, bool)> onPreviewClip;
        std::function<void()> onStopPreview;
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
        std::move(closeCallback));
}
}
