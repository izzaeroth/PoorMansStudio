#pragma once

#include <juce_gui_extra/juce_gui_extra.h>
#include <juce_audio_utils/juce_audio_utils.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <optional>
#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>
#include <utility>
#include <vector>
#include <map>

#include "core/Project.h"
#include "audio/SoundFontPresetReader.h"
#include "audio/RenderJob.h"
#include "audio/AudioClipRecorder.h"
#include "vst/VstPluginTypes.h"
#include "gui/PianoRollComponent.h"

namespace mw::gui
{

    inline std::atomic_bool helperBubblesGloballyEnabled { true };

    inline void setHelperBubblesGloballyEnabled(bool enabled)
    {
        helperBubblesGloballyEnabled.store(enabled);
    }

    inline bool areHelperBubblesGloballyEnabled()
    {
        return helperBubblesGloballyEnabled.load();
    }

    class FreshHoverTooltipWindow final : public juce::TooltipWindow
    {
    public:
        FreshHoverTooltipWindow(juce::Component* parentComponent, int newHoverDelayMs)
            : juce::TooltipWindow(parentComponent, 50),
              hoverDelayMs(newHoverDelayMs)
        {
        }

        juce::String getTipFor(juce::Component& component) override
        {
            if (!areHelperBubblesGloballyEnabled())
            {
                lastTipComponent = nullptr;
                lastTipText = {};
                hoverStartedMs = juce::Time::getMillisecondCounter();
                return {};
            }

            const auto tip = juce::TooltipWindow::getTipFor(component);
            const auto now = juce::Time::getMillisecondCounter();

            if (tip.isEmpty())
            {
                lastTipComponent = nullptr;
                lastTipText = {};
                hoverStartedMs = now;
                return {};
            }

            if (lastTipComponent != &component || lastTipText != tip)
            {
                lastTipComponent = &component;
                lastTipText = tip;
                hoverStartedMs = now;
                return {};
            }

            if (static_cast<int>(now - hoverStartedMs) < hoverDelayMs)
                return {};

            return tip;
        }

    private:
        int hoverDelayMs = 2000;
        juce::Component* lastTipComponent = nullptr;
        juce::String lastTipText;
        uint32_t hoverStartedMs = 0;
    };

    class HelperTooltipLookAndFeel final : public juce::LookAndFeel_V4
    {
    public:
        void drawTooltip(juce::Graphics& g, const juce::String& text, int width, int height) override
        {
            g.fillAll(juce::Colours::transparentBlack);

            auto borderArea = juce::Rectangle<float>(0.5f, 0.5f,
                                                     static_cast<float>(std::max(1, width)) - 1.0f,
                                                     static_cast<float>(std::max(1, height)) - 1.0f);
            auto fillArea = borderArea.reduced(1.5f);

            g.setColour(juce::Colours::black.withAlpha(0.98f));
            g.fillRoundedRectangle(borderArea, 6.0f);
            g.setColour(juce::Colour(0xffffe066));
            g.fillRoundedRectangle(fillArea, 5.0f);
            g.setColour(juce::Colours::black.withAlpha(0.95f));
            g.drawRoundedRectangle(fillArea, 5.0f, 1.0f);
            g.setColour(juce::Colours::black);
            g.setFont(juce::Font(14.0f, juce::Font::plain));
            g.drawFittedText(text, juce::Rectangle<int>(width, height).reduced(11, 8), juce::Justification::centredLeft, 4);
        }

        juce::Rectangle<int> getTooltipBounds(const juce::String& text, juce::Point<int> screenPos, juce::Rectangle<int> parentArea) override
        {
            const auto maxWidth = std::min(420, std::max(220, parentArea.getWidth() - 24));
            const int estimatedTextWidth = 18 + static_cast<int>(text.length()) * 8;
            const int textWidth = juce::jlimit(180, maxWidth, estimatedTextWidth);
            const int lines = std::max(1, 1 + static_cast<int>(text.length()) / 54);
            const auto height = juce::jlimit(38, 104, 22 + lines * 18);

            auto bounds = juce::Rectangle<int>(textWidth, height).withPosition(screenPos.x + 14, screenPos.y + 20);

            if (bounds.getRight() > parentArea.getRight())
                bounds.setX(parentArea.getRight() - bounds.getWidth() - 6);

            if (bounds.getBottom() > parentArea.getBottom())
                bounds.setY(screenPos.y - bounds.getHeight() - 12);

            if (bounds.getX() < parentArea.getX())
                bounds.setX(parentArea.getX() + 6);

            if (bounds.getY() < parentArea.getY())
                bounds.setY(parentArea.getY() + 6);

            return bounds;
        }
    };

    class SequenceConsoleComponent final : public juce::Component,
                                           public juce::SettableTooltipClient,
                                           private juce::ScrollBar::Listener
    {
    public:
        SequenceConsoleComponent()
            : verticalScrollBar(true)
        {
            setWantsKeyboardFocus(true);
            addAndMakeVisible(verticalScrollBar);
            verticalScrollBar.addListener(this);
            verticalScrollBar.setAutoHide(false);
            verticalScrollBar.setVisible(true);
        }

        ~SequenceConsoleComponent() override
        {
            verticalScrollBar.removeListener(this);
        }

        void resized() override
        {
            auto area = getLocalBounds();
            verticalScrollBar.setBounds(area.removeFromRight(scrollBarWidth));
            updateScrollRange(false);
        }

        void setMultiLine(bool) {}
        void setReadOnly(bool) {}
        void setScrollbarsShown(bool shouldShow)
        {
            showScrollBar = shouldShow;
            verticalScrollBar.setVisible(shouldShow);
            resized();
        }

        void setColour(int colourId, juce::Colour colour)
        {
            if (colourId == juce::TextEditor::backgroundColourId)
                backgroundColour = colour;
            else if (colourId == juce::TextEditor::textColourId)
                textColour = colour;
            else if (colourId == juce::TextEditor::outlineColourId || colourId == juce::TextEditor::focusedOutlineColourId)
                outlineColour = colour;

            repaint();
        }

        void setText(const juce::String& text, juce::NotificationType = juce::sendNotification)
        {
            setTextInternal(text, true);
        }

        void setTextPreservingScroll(const juce::String& text, juce::NotificationType = juce::sendNotification)
        {
            setTextInternal(text, false);
        }

        void clear()
        {
            entries.clear();
            updateScrollRange(true);
            repaint();
        }

        void moveCaretToEnd()
        {
            scrollToBottom();
        }

        void setTooltip(const juce::String&)
        {
            // Console panes intentionally do not show helper bubbles.
            juce::SettableTooltipClient::setTooltip({});
        }

        void insertTextAtCaret(const juce::String& text)
        {
            appendMessage(text.trimCharactersAtEnd("\r\n"), juce::Colours::transparentBlack, false);
        }

        void appendMessage(const juce::String& text, juce::Colour sequenceColour, bool hasSequenceColour)
        {
            juce::StringArray lines;
            lines.addLines(text);

            if (lines.isEmpty())
                lines.add(text);

            for (const auto& line : lines)
            {
                if (line.isEmpty())
                    continue;

                entries.push_back({ line, sequenceColour, hasSequenceColour });
            }

            constexpr std::size_t maxEntries = 1200;
            if (entries.size() > maxEntries)
                entries.erase(entries.begin(), entries.begin() + static_cast<std::ptrdiff_t>(entries.size() - maxEntries));

            updateScrollRange(true);
            repaint();
        }

        void setSequenceColours(const std::vector<juce::Colour>& colours)
        {
            sequenceColours = colours;
        }

        void scrollToFirstMatchingText(const juce::String& needle)
        {
            if (needle.isEmpty())
                return;

            const auto lowerNeedle = needle.toLowerCase();

            for (int i = 0; i < static_cast<int>(entries.size()); ++i)
            {
                if (entries[static_cast<std::size_t>(i)].message.toLowerCase().contains(lowerNeedle))
                {
                    scrollToEntry(i);
                    return;
                }
            }
        }

        void scrollToFirstSequenceNumber(int sequenceNumber)
        {
            if (sequenceNumber <= 0)
                return;

            for (int i = 0; i < static_cast<int>(entries.size()); ++i)
            {
                if (findSequenceNumberInText(entries[static_cast<std::size_t>(i)].message) == sequenceNumber)
                {
                    scrollToEntry(i);
                    return;
                }
            }
        }

        void scrollToFirstTrackNumber(int trackNumber)
        {
            if (trackNumber <= 0)
                return;

            const juce::String selectedNeedle = ">" + juce::String(trackNumber);
            const juce::String trackNeedle = "track #" + juce::String(trackNumber);

            for (int i = 0; i < static_cast<int>(entries.size()); ++i)
            {
                const auto lower = entries[static_cast<std::size_t>(i)].message.toLowerCase();
                if (lower.contains(trackNeedle) || lower.startsWith(selectedNeedle))
                {
                    scrollToEntry(i);
                    return;
                }
            }
        }

        int getScrollStart() const
        {
            return firstVisiblePixel;
        }

        void setScrollStart(int pixel)
        {
            const int visibleHeight = std::max(1, getHeight());
            const int maxStart = std::max(0, getContentHeight() - visibleHeight);
            const int next = std::clamp(pixel, 0, maxStart);
            verticalScrollBar.setCurrentRangeStart(static_cast<double>(next));
            firstVisiblePixel = next;
            repaint();
        }

        juce::String getText() const
        {
            juce::String result;
            for (const auto& entry : entries)
                result << entry.message << "\n";
            return result;
        }

        void mouseDown(const juce::MouseEvent& event) override
        {
            grabKeyboardFocus();

            if (event.mods.isPopupMenu())
            {
                juce::PopupMenu menu;
                menu.addItem(1, "Copy Console Text", !getText().isEmpty());

                menu.showMenuAsync(
                    juce::PopupMenu::Options().withTargetComponent(this),
                    [this](int result)
                    {
                        if (result == 1)
                            juce::SystemClipboard::copyTextToClipboard(getText());
                    }
                );
            }
        }

        void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& wheel) override
        {
            const int visibleHeight = std::max(1, getHeight());
            const int maxStart = std::max(0, getContentHeight() - visibleHeight);
            if (maxStart <= 0)
                return;

            const float wheelAmount = std::abs(wheel.deltaY) >= std::abs(wheel.deltaX) ? wheel.deltaY : wheel.deltaX;
            const int direction = wheelAmount < 0.0f ? 1 : -1;
            const int step = lineHeight * 3;
            setScrollStart(firstVisiblePixel + direction * step);
        }

        bool keyPressed(const juce::KeyPress& key) override
        {
            const auto mods = key.getModifiers();
            const auto ch = key.getTextCharacter();

            if ((ch == 'c' || ch == 'C') && (mods.isCommandDown() || mods.isCtrlDown()))
            {
                juce::SystemClipboard::copyTextToClipboard(getText());
                return true;
            }

            if ((ch == 'a' || ch == 'A') && (mods.isCommandDown() || mods.isCtrlDown()))
                return true;

            return false;
        }

        void paint(juce::Graphics& g) override
        {
            g.fillAll(backgroundColour);

            auto area = getLocalBounds();
            if (showScrollBar)
                area.removeFromRight(scrollBarWidth);

            const auto clip = g.getClipBounds().getIntersection(area);
            const int firstLine = std::max(0, firstVisiblePixel / lineHeight);
            const int lastLine = std::min(static_cast<int>(entries.size()), 2 + (firstVisiblePixel + clip.getBottom()) / lineHeight);

            g.setFont(juce::Font(13.0f, juce::Font::plain));

            for (int i = firstLine; i < lastLine; ++i)
            {
                const auto& entry = entries[static_cast<std::size_t>(i)];
                auto row = juce::Rectangle<int>(area.getX(), i * lineHeight - firstVisiblePixel, area.getWidth(), lineHeight).reduced(6, 2);

                if (! row.intersects(clip.expanded(0, lineHeight)))
                    continue;

                if (entry.hasSequenceColour)
                {
                    const auto railColour = entry.sequenceColour.withAlpha(0.96f);
                    g.setColour(railColour);
                    g.fillRoundedRectangle(row.removeFromLeft(6).toFloat(), 2.0f);
                    row.removeFromLeft(7);

                    auto rightRail = row.removeFromRight(6);
                    g.fillRoundedRectangle(rightRail.toFloat(), 2.0f);
                    row.removeFromRight(7);
                }
                else
                {
                    row.removeFromLeft(13);
                    row.removeFromRight(13);
                }

                g.setColour(entry.message.startsWithChar('>') ? juce::Colours::white : textColour);
                g.drawFittedText(entry.message, row, juce::Justification::centredLeft, 1);
            }

            g.setColour(outlineColour);
            g.drawRect(getLocalBounds(), 1);
        }

    private:
        struct Entry
        {
            juce::String message;
            juce::Colour sequenceColour;
            bool hasSequenceColour = false;
        };

        void setTextInternal(const juce::String& text, bool scrollToEnd)
        {
            const int previousScroll = firstVisiblePixel;
            entries.clear();

            juce::StringArray lines;
            lines.addLines(text);

            for (const auto& line : lines)
            {
                if (line.isEmpty())
                    entries.push_back({ juce::String(), juce::Colours::transparentBlack, false });
                else
                    appendParsedLine(line);
            }

            updateScrollRange(scrollToEnd);
            if (!scrollToEnd)
                setScrollStart(previousScroll);
            repaint();
        }

        void scrollToEntry(int entryIndex)
        {
            const int y = std::max(0, entryIndex * lineHeight - lineHeight);
            setScrollStart(y);
        }

        static int findSequenceNumberInText(const juce::String& message)
        {
            auto findAfter = [&message](const juce::String& marker) -> int
            {
                const auto lower = message.toLowerCase();
                const auto pos = lower.indexOf(marker.toLowerCase());

                if (pos < 0)
                    return 0;

                auto tail = message.substring(pos + marker.length()).trimStart();

                if (tail.startsWithChar('#'))
                    tail = tail.substring(1).trimStart();

                return tail.getIntValue();
            };

            int number = findAfter("sequence ");
            if (number <= 0) number = findAfter("seq ");
            if (number <= 0) number = findAfter("sequence #");
            if (number <= 0) number = findAfter("seq #");
            return number;
        }

        void appendParsedLine(const juce::String& line)
        {
            const int sequenceNumber = findSequenceNumberInText(line);
            const bool hasSequenceColour = sequenceNumber > 0 && sequenceNumber <= static_cast<int>(sequenceColours.size());
            const auto colour = hasSequenceColour ? sequenceColours[static_cast<std::size_t>(sequenceNumber - 1)] : juce::Colours::transparentBlack;
            entries.push_back({ line, colour, hasSequenceColour });
        }

        int getContentHeight() const
        {
            return std::max(getHeight(), 8 + static_cast<int>(entries.size()) * lineHeight);
        }

        void updateScrollRange(bool scrollToEnd)
        {
            const int visibleHeight = std::max(1, getHeight());
            const int contentHeight = getContentHeight();

            verticalScrollBar.setRangeLimits(0.0, static_cast<double>(contentHeight));
            verticalScrollBar.setCurrentRange(
                scrollToEnd ? static_cast<double>(std::max(0, contentHeight - visibleHeight)) : static_cast<double>(std::clamp(firstVisiblePixel, 0, std::max(0, contentHeight - visibleHeight))),
                static_cast<double>(visibleHeight)
            );

            firstVisiblePixel = static_cast<int>(std::round(verticalScrollBar.getCurrentRangeStart()));
        }

        void scrollToBottom()
        {
            updateScrollRange(true);
            repaint();
        }

        void scrollBarMoved(juce::ScrollBar* scrollBarThatHasMoved, double newRangeStart) override
        {
            if (scrollBarThatHasMoved == &verticalScrollBar)
            {
                firstVisiblePixel = std::max(0, static_cast<int>(std::round(newRangeStart)));
                repaint();
            }
        }

        static constexpr int lineHeight = 20;
        static constexpr int scrollBarWidth = 16;
        std::vector<Entry> entries;
        std::vector<juce::Colour> sequenceColours;
        juce::ScrollBar verticalScrollBar;
        bool showScrollBar = true;
        int firstVisiblePixel = 0;
        juce::Colour backgroundColour { juce::Colours::black };
        juce::Colour textColour { juce::Colours::white };
        juce::Colour outlineColour { juce::Colours::grey };
    };
    struct VstHostHelperStatusSnapshot
    {
        bool checked = false;
        bool executableFound = false;
        bool versionOk = false;
        bool pingOk = false;
        bool scanCommandsOk = false;
        juce::String executablePath;
        juce::String versionOutput;
        juce::String pingOutput;
        juce::String helpOutput;
        juce::String errorText;
        juce::String checkedAtLocal;

        bool allOk() const noexcept
        {
            return checked && executableFound && versionOk && pingOk && scanCommandsOk;
        }
    };

    class MainComponent final : public juce::Component,
                                public juce::MenuBarModel
    {
    public:
        MainComponent();
        ~MainComponent() override;

        void paint(juce::Graphics& g) override;
        void resized() override;

        juce::StringArray getMenuBarNames() override;
        juce::PopupMenu getMenuForIndex(int topLevelMenuIndex, const juce::String& menuName) override;
        void menuItemSelected(int menuItemID, int topLevelMenuIndex) override;
        void requestCloseWithSaveAsPrompt(std::function<void()> exitCallback);

    private:
        void chooseMusicXml();
        void chooseMusicXmlAfterUnsavedCheck();
        void openProjectFile();
        void saveCurrentProjectFile(std::function<void()> afterSuccessfulSave = {});
        bool writeProjectToFile(const juce::File& file);
        void saveProjectToFileWithOverwriteWarning(juce::File file, std::function<void()> afterSuccessfulSave = {});
        void startNewProject();
        void resetProjectWorkspace();
        void setProjectDirty(bool shouldBeDirty = true);
        void clearProjectDirty();
        void finishProgrammaticProjectLoad();
        bool confirmDiscardUnsavedChanges(const juce::String& actionName, std::function<void()> proceedCallback);
        void cleanTempFolder();
        void chooseSfzFile();
        void createSfzTestMidiAndRender();
        void chooseExportFolder();
        void chooseSoundFont();
        void openProjectInfoWindow();
        void applyProjectInfoToGuiAndProject();

        void importMusicXmlOnly();
        void importAudioFile();
        void openAudioRecorderWindow();
        void closeAudioRecorderWindowWithPrompt();
        void closeAudioRecorderWindowNow();
        void startAudioRecordingTake();
        void startAudioRecorderTest();
        void finishAudioRecorderTestRecording();
        void playAudioRecorderTestAndCleanup(const std::filesystem::path& testPath);
        void stopAudioRecorderTestPlaybackAndCleanup(const std::filesystem::path& testPath = {});
        void cancelAudioRecorderTestAndCleanup();
        void setAudioRecorderMicGainDb(double gainDb);
        void setAudioRecorderTrackLiveEffectEnabled(bool enabled);
        mw::audio::AudioClipRecorderLiveEffectOptions buildAudioRecorderLiveEffectOptions(int targetTrackIndex, int sourceTrackIndex) const;
        int getAudioRecorderTargetTrackIndex() const;
        bool trackHasUsableLiveEffectForAudioRecorder(int trackIndex) const;
        void pauseOrResumeAudioRecordingTake();
        void stopAudioRecordingTake();
        void keepAudioRecordingTake();
        void redoAudioRecordingTake();
        void discardAudioRecordingTake();
        void refreshAudioRecorderInputDevices();
        void selectAudioRecorderInputDevice(const juce::String& deviceName);
        void revealCurrentProjectFolder();
        bool ensureProjectFolderReadyForAudio();
        std::filesystem::path getCurrentProjectFolder() const;
        std::filesystem::path getAudioRecordingSessionFolder();
        void clearAudioRecordingSessionStaging(bool removeFiles);
        void discardCurrentAudioClipStagingSession(const juce::String& reason);
        bool commitAppliedStagedAudioClipsToProjectFolder(const std::filesystem::path& projectFolder);
        bool exportFolderShouldResetToWorkspaceExports(const std::filesystem::path& nextProjectFolder) const;
        void syncProjectIdentityToFile(const juce::File& file);
        mw::core::AudioClipSavedFormat getSelectedAudioClipFormat() const;
        int getSelectedAudioClipQualityKbps() const;
        int createAudioClipTrackForNewClip(const juce::String& baseName);
        void addAudioClipToProject(mw::core::AudioClip clip);
        juce::String getAudioClipSummaryForTrack(int trackIndex) const;
        juce::String getAudioClipSummaryForSequence(int sequenceNumber) const;
        bool sequenceHasAudioClips(int sequenceNumber) const;
        void refreshAudioRecorderWindowStatus();
        mw::audio::RenderJob createRenderJobSnapshot() const;
        void renderCurrentProjectOnBackgroundThread();
        void renderSelectedTrackOnBackgroundThread();
        void renderSelectedSequenceOnBackgroundThread();
        void renderMidiCurrentProject();
        void showRenderSettingsWindow();
        void closeRenderSettingsWindow();
        void previewSelectedTrackOnBackgroundThread();
        void previewSelectedSequenceOnBackgroundThread();
        void previewCurrentProjectOnBackgroundThread();
        void setPianoRollPreviewNoteMapFromTracks(const std::vector<mw::core::Track>& tracks);
        void startRenderJobOnBackgroundThread(mw::audio::RenderJob job, const juce::String& label, bool playWhenDone = false, double previewDurationBeats = 0.0);
        void continueRenderAfterSfzWarning();
        void showSfzRenderWarning();
        void setRenderingState(bool isRendering);
        void updateRenderOutputSummary();
        void cleanupAppTempOnExit();

        void refreshSoundFontList();
        void refreshSfzList();
        std::filesystem::path getSelectedSfzPath() const;
        std::filesystem::path getSelectedSoundFontPath() const;
        mw::core::SampleBackendType getProjectDefaultBackendType() const;
        std::filesystem::path getProjectDefaultLibraryPath(mw::core::SampleBackendType backendType) const;
        std::optional<mw::vst::VstPluginDescriptor> getProjectDefaultVstPluginDescriptor();
        void applyVstPluginDescriptorToAssignment(mw::core::InstrumentAssignment& assignment, const mw::vst::VstPluginDescriptor& descriptor) const;
        void captureProjectDefaultVstPluginSelection();
        void seedTrackSoundLibraryFromProjectDefaults(mw::core::Track& track);
        void seedMissingTrackSoundLibrariesFromProjectDefaults(mw::core::Project& project);
        void refreshTrackSoundLibraryDisplay();
        void applyProjectBackendSelection();
        void chooseTrackSoundLibrary();
        void assignSelectedTrackSoundLibrary(const std::filesystem::path& libraryPath, mw::core::SampleBackendType backendType);
        void showScannedVstInstrumentChooserDialog(const juce::String& title,
                                                  const juce::String& message,
                                                  std::function<void(const mw::vst::VstPluginDescriptor&)> onChoose);
        void scanVstPlugins(bool showSummary);
        void openVstPluginManagerWindow();
        void openVstSettingsWindow();
        void refreshVstGraphicsProfile(bool firstLaunchAutoDetect);
        juce::String runVstHostHelperCommandForStatus(const juce::File& helperFile, const juce::String& argument, int timeoutMs, int& exitCode);
        void refreshVstHostHelperStatusCache();
        void showVstHostHelperStatusWindow();
        void assignSelectedTrackVstPlugin(const mw::vst::VstPluginDescriptor& descriptor);
        void applyVstPluginDescriptorToEffectSlot(mw::core::VstPluginAssignment& slot, const mw::vst::VstPluginDescriptor& descriptor) const;
        void populateVstEffectCombo();
        void syncVstEffectControlsFromSelection();
        void applySelectedTrackVstEffectSlots();
        void applySelectedTrackVstEffectSlot() { applySelectedTrackVstEffectSlots(); }
        void setVstEffectStatusText(int trackIndex, const juce::String& baseStatus);
        void recordVstEffectRenderStatusForTrack(int trackIndex, const juce::String& statusText);
        void openSelectedTrackVstPluginUi();
        void openSelectedTrackVstEffectUi(int effectSlotIndex = 0);
        void renderSelectedTrackVstEffectTestSample(int effectSlotIndex = 0);
        void renderVstEffectTestSampleForTrack(int trackIndex, int effectSlotIndex);
        juce::String captureOpenVstPluginStateForTrack(int trackIndex, bool updateTrackAssignment, bool logCapture);
        juce::String captureOpenVstEffectStateForTrack(int trackIndex, int effectSlotIndex, bool updateTrackAssignment, bool logCapture);
        int captureOpenVstPluginStatesForPreview(const juce::String& contextLabel);
        int captureOpenVstPluginStatesForProjectSave();
        bool closeVstPluginWindowForTrack(int trackIndex, const juce::String& reason = {});
        void closeAllVstPluginWindows();
        void closeAllOpenWindows();
        void finishClosingAllOpenWindows();

        void populateInstrumentCombo();
        void refreshPresetListFromSelectedSoundFont();
        bool enforceProjectTrackLimit(mw::core::Project& project, const juce::String& sourceLabel);
        bool canAddAnotherTrack(const juce::String& actionLabel);
        void refreshTrackSelector();
        void openTrackManagerWindow();
        bool trackPassesManagerFilter(int trackIndex) const;
        void refreshTrackManagerText(bool jumpToActiveSequence = true);
        void syncTrackManagerSelectionFromCurrentTrack(bool refreshConsole, bool jumpToTrack);
        void refreshSequenceMembershipDisplays(int sourceSequenceNumber = 0, int targetSequenceNumber = 0, bool scrollToSourceSequence = false);
        juce::String formatSequenceTrackSummary(int sequenceIndex) const;
        void selectTrackFromManagerPage();
        void selectSequenceFromManagerPage();
        void selectSequenceFromMap(int sequenceIndex, bool focusFirstTrack);
        void openSelectedTrackInPianoRollFromManager();
        void importFilesAsSequence();
        std::optional<mw::core::Project> importProjectFromPath(const std::filesystem::path& path);
        std::int64_t getProjectEndTick() const;
        void refreshAfterMultiFileImport();
        void rebuildSectionsFromTracksIfNeeded();
        void recordImportSection(const juce::String& name, const std::filesystem::path& sourcePath, std::int64_t startTick, std::int64_t endTick, const std::vector<int>& trackNumbers, bool isLayer, const juce::String& createdBy = "manual");
        int allocateNextSequenceId() const;
        int allocateNextDefaultSequenceNumber() const;
        int sequenceDefaultNumberFromName(const juce::String& name) const;
        juce::String makeDefaultSequenceName(const std::filesystem::path& sourcePath, const juce::String& fallbackName) const;
        bool isPianoRollOpenForSequence(int sequenceIndex) const;
        void focusFirstSequenceAfterSequenceRemoval();
        void syncSequencesToProjectMetadata();
        void normalizeEmptySequencesAfterMembershipChange();
        void restoreSequencesFromProjectMetadata();
        void appendImportedProjectAsNewTracksInSequence(mw::core::Project importedProject, const std::filesystem::path& sourcePath, int targetSequenceIndex, std::int64_t startTick);
        juce::Colour getSequenceColourForIndex(int sequenceIndex) const;
        void applySectionStartBeatFromManager();
        void nudgeSequenceStartFromManager(double beatDelta);
        std::optional<std::pair<int, juce::String>> createBlankSequenceOnly(const juce::String& actionLabel, bool focusNewSequence);
        void removeSelectedSequenceFromManager();
        void captureTrackManagerUndoState(const juce::String& actionLabel);
        void undoTrackManagerEdit();
        void linkTrackToSectionFromManager();
        void unlinkTrackFromSectionFromManager();
        void moveTrackToSequenceFromManager();
        bool applyTrackSequenceChangeFromManager(int targetSequenceNumber, bool createIfNeeded, int trackNumberOverride = 0);
        void openSequenceColorEditorFromManager();
        bool applySequenceColorFromEditor(juce::Colour selectedColour);
        void renameSequenceFromManager();
        void editSequenceNotesFromManager();
        void toggleSequenceLockFromManager();
        int getSequenceIndexForTrack(int trackNumber) const;
        void updateActiveSequenceFromSelectedTrack();
        void applyTrackStartBeatFromManager();
        void duplicateTrackToStartBeatFromManager();
        int getSelectedTrackIndex() const;
        void syncTrackInspectorFromSelection();
        void applyTrackInspector();
        void addManualTrack();
        void duplicateSelectedTrack();
        void removeSelectedTrack();
        void renameSelectedTrack();
        void refreshNoteEditor();
        void applyProjectTimingSettings();
        void applyNoteEditorToTrack();
        void addNoteToSelectedTrack();
        void removeLastNoteFromSelectedTrack();
        void openRawNotesWindow();
        void openPianoRollWindow();
        juce::String getSelectedTrackDisplayName() const;
        juce::String getTrackDisplayName(int trackIndex) const;
        void applyPianoRollSettings();
        void updateBeatWindowButtonHighlights(int beatWindow);
        void fitPianoRollToSelectedTrack();
        int getSelectedTrackEndBeat() const;
        int getTrackEndBeat(int trackIndex) const;
        int getCurrentPianoRollBeatWindow() const;
        int getCurrentPianoRollStartBeat() const;
        int getCurrentPianoRollTotalPages() const;
        void updatePianoRollPageIndicator();
        void previousPianoRollWindow();
        void nextPianoRollWindow();
        void jumpToPianoRollWindow();
        void jumpToPianoRollKey();
        void updatePianoRollKeyRangeLabel();
        void renderPianoRollPreview();
        void openPianoRollPreviewPlayerWindow();
        void playPianoRollPreview();
        void playPianoRollPreviewFile(const std::filesystem::path& previewPath);
        bool seekPianoRollPreviewToSeconds(double seconds);
        double getPianoRollPreviewCurrentSeconds() const;
        double getPianoRollPreviewTotalSeconds() const;
        void pausePianoRollPreview();
        void stopPianoRollPreview();
        void playProjectPreview();
        void stopProjectPreview();
        std::filesystem::path findLatestProjectPreviewFile() const;
        void cleanupPianoRollPreviewFiles();
        void cleanupGeneratedPreviewFiles();
        void syncPianoRollFromSelectedTrack();
        void updateVolumeLabels();
        void captureProjectUserSettings();
        void applyProjectUserSettingsToGui();
        void applyThemePreset(int presetId);
        void saveThemePreference();
        void loadThemePresets();

        juce::Colour getConsoleColourForMessage(const juce::String& message) const;
        void logMessage(const juce::String& message);
        void updateTrackSummary(const mw::core::Project& project);
        void updateRenderTargetLabel();
        void refreshMainSequenceSelector();
        void refreshActiveSequenceThoughtsEditor();
        void syncSequenceThoughtsFromEditor();
        void showSequenceListPickerWindow(const juce::String& title, const juce::String& instructions, int initialSequenceNumber, std::function<void(int)> applyExistingCallback, std::function<std::optional<std::pair<int, juce::String>>()> createNewCallback);
        void closeSequenceListPickerWindow();
        void showChangeActiveSequenceDialog();
        bool applyActiveSequenceNumberFromMain(int targetSequenceNumber, bool createIfNeeded);
        void setDefaultPaths();
        void saveUserSettingsNow();
        void configureHelperBubbles();
        void setHelperBubblesEnabled(bool enabled);
        void setVstCompatibilityWarningsEnabled(bool enabled);
        void showVstExperimentalWarningIfNeeded();
        bool selectedTrackHasAppliedVstPlugin() const;
        bool selectedTrackHasOpenableVstEffect(int effectSlotIndex) const;
        void updateOpenVstPluginButtonState();
        bool areHelperBubblesEnabled() const { return helperBubblesEnabled; }
        bool ensureSelectedTrackHasSequenceForPianoRoll();

        void startTrackManagerEditSession();
        void refreshTrackManagerSessionSnapshotIfClean();
        void markTrackManagerEditorDirty(const juce::String& actionLabel = {});
        void clearTrackManagerEditorDirty();
        void updateTrackManagerWindowDirtyIndicator();
        void applyTrackManagerEditorChanges();
        void discardTrackManagerEditorChanges();
        void closeTrackManagerWindowWithDirtyCheck();
        void finishClosingTrackManagerWindow();
        void recordExternalTrackStateUpdate(int trackIndex, bool marksProjectDirty = true);
        void preserveExternalTrackStateUpdates(mw::core::Project& restoredProject) const;

        void populateNoteEditorFromNotes(const std::vector<mw::core::NoteEvent>& notes);
        struct PianoRollEditorWindowState;
        void markPianoRollEditorDirty();
        void markPianoRollEditorDirty(PianoRollEditorWindowState& state);
        void clearPianoRollEditorDirty();
        void clearPianoRollEditorDirty(PianoRollEditorWindowState& state);
        void refreshAggregatePianoRollDirtyFlag();
        void updatePianoRollWindowDirtyIndicator();
        void updatePianoRollWindowDirtyIndicator(PianoRollEditorWindowState& state);
        bool applyPianoRollEditorChanges();
        bool applyPianoRollEditorChanges(PianoRollEditorWindowState& state);
        void discardPianoRollEditorChanges();
        void discardPianoRollEditorChanges(PianoRollEditorWindowState& state);
        void closePianoRollWindowWithDirtyCheck();
        void closePianoRollWindowWithDirtyCheck(int trackIndex);
        void finishClosingPianoRollWindow();
        void finishClosingPianoRollWindow(int trackIndex);
        PianoRollEditorWindowState* findPianoRollEditorWindow(int trackIndex) const;
        void applyPianoRollSettings(PianoRollEditorWindowState& state, bool commitTrackSettings = true);
        void fitPianoRollToTrack(PianoRollEditorWindowState& state);
        void refreshPianoRollTrackSoundLibraryDisplay(PianoRollEditorWindowState& state);
        juce::String resolvePianoRollInstrumentName(const PianoRollEditorWindowState& state, const mw::core::InstrumentAssignment& assignment) const;
        juce::String getPianoRollHeaderInstrumentTextForState(const PianoRollEditorWindowState& state) const;
        void populatePianoRollInstrumentCombo(PianoRollEditorWindowState& state);
        void choosePianoRollTrackSoundLibrary(PianoRollEditorWindowState& state);
        void assignPianoRollTrackSoundLibrary(PianoRollEditorWindowState& state, const std::filesystem::path& libraryPath, mw::core::SampleBackendType backendType);
        void applyPianoRollInstrumentSelection(PianoRollEditorWindowState& state, bool captureUndo = true);
        bool applyPendingPianoRollTrackSettings(PianoRollEditorWindowState& state);
        void discardPendingPianoRollTrackSettings(PianoRollEditorWindowState& state);
        void capturePianoRollInstrumentUndoState(PianoRollEditorWindowState& state, const juce::String& actionLabel);
        bool undoPianoRollEditorAction(PianoRollEditorWindowState& state);
        bool redoPianoRollEditorAction(PianoRollEditorWindowState& state);
        void refreshOpenPianoRollInstrumentControls();
        int getPianoRollBeatWindow(const PianoRollEditorWindowState& state) const;
        int getPianoRollStartBeat(const PianoRollEditorWindowState& state) const;
        int getPianoRollTotalPages(const PianoRollEditorWindowState& state) const;
        void updatePianoRollPageIndicator(PianoRollEditorWindowState& state);
        void updatePianoRollKeyRangeLabel(PianoRollEditorWindowState& state);
        void updateBeatWindowButtonHighlights(PianoRollEditorWindowState& state, int beatWindow);
        void focusOpenWindowByMenuId(int menuItemID);
        int getFirstOpenPianoRollTrackNumberForSequence(int sequenceIndex) const;
        void updateOpenPianoRollTrackNames();
        void refreshOpenPianoRollAfterTrackSequenceMove(int trackIndex);

        juce::TextButton chooseMusicXmlButton {"Start From File"};
        juce::TextButton importAudioButton {"Import Audio"};
        juce::TextButton recordAudioButton {"Record Audio"};
        juce::TextButton newProjectButton {"New Project"};
        juce::TextButton openProjectButton {"Open .mwproj"};
        juce::TextButton saveProjectButton {"Save Project"};
        juce::TextButton cleanTempButton {"Clean Temp"};
        juce::TextButton saveSettingsButton {"Save Settings"};
        juce::TextButton editInfoButton {"Edit Info"};
        juce::TextButton exportFolderButton {"Choose Export Folder"};
        juce::TextButton browseSoundFontButton {"C"};
        juce::TextButton refreshSoundFontsButton {"Refresh"};
                juce::TextButton sfzButton {"C"};
        juce::TextButton refreshSfzButton {"Refresh"};
        juce::TextButton sfzTestButton {"Test SFZ"};
        juce::TextButton applyBackendButton {"Apply Project Defaults"};
        juce::TextButton applyTrackButton {"Apply Track Settings"};
        juce::TextButton changeTrackLibraryButton {"Change Library"};
        juce::TextButton openVstPluginButton {"Open VST Instrument"};
        juce::TextButton openVstEffectButton {"Open Slot 1"};
        juce::TextButton openVstEffect2Button {"Open Slot 2"};
        juce::TextButton trackSfzButton {"Track SFZ"};
        juce::TextButton addTrackButton {"Add Blank"};
        juce::TextButton duplicateTrackButton {"Duplicate"};
        juce::TextButton removeTrackButton {"Remove"};
        juce::TextButton renameTrackButton {"Rename"};
        juce::TextButton trackManagerButton {"Track Manager"};
        juce::TextButton applyTimingButton {"Apply Timing"};
        juce::TextButton applyNotesButton {"Update Notes"};
        juce::TextButton showRawNotesButton {"Show Raw Notes"};
        juce::TextButton addNoteButton {"Add Note"};
        juce::TextButton removeNoteButton {"Remove Last Note"};
        juce::TextButton deletePianoRollNoteButton {"Delete Selected"};
        juce::TextButton syncPianoRollButton {"Sync Roll"};
        juce::TextButton openPianoRollButton {"Open Piano Roll"};
        juce::TextButton setBeatWindow4Button {"4"};
        juce::TextButton setBeatWindow8Button {"8"};
        juce::TextButton setBeatWindow16Button {"16"};
        juce::TextButton setBeatWindow32Button {"32"};
        juce::TextButton setBeatWindow64Button {"64"};
        juce::TextButton previousPianoRollWindowButton {"Prev Window"};
        juce::TextButton nextPianoRollWindowButton {"Next Window"};
        juce::TextButton jumpPianoRollPageButton {"Go"};
        juce::TextButton jumpPianoRollKeyButton {"Go Key"};
        juce::TextButton pianoRollNotesDownButton {"Notes Down"};
        juce::TextButton pianoRollNotesUpButton {"Notes Up"};
        juce::TextButton copyPianoRollNoteButton {"Copy"};
        juce::TextButton pastePianoRollNoteButton {"Paste"};
        juce::TextButton undoPianoRollButton {"Undo"};
        juce::TextButton redoPianoRollButton {"Redo"};
        juce::TextButton clearPianoRollSelectionButton {"Clear Sel"};
        juce::TextButton openPianoRollPreviewPlayerButton {"Player"};
        juce::TextButton playProjectPreviewButton {"Play Preview"};
        juce::TextButton stopProjectPreviewButton {"Stop"};
        juce::TextButton renderButton {"Render Project"};
        juce::TextButton renderSettingsButton {"Render Settings"};
        juce::TextButton renderSelectedTrackButton {"Render Track"};
        juce::TextButton renderSelectedSequenceButton {"Render Seq"};
        juce::TextButton renderMidiButton {"Render MIDI"};
        juce::TextButton cancelRenderButton {"Cancel Render"};
        juce::TextButton previewButton {"Preview WAV"};

        juce::MenuBarComponent menuBar { this };

        juce::Label titleLabel;
        juce::Label renderStatusLabel;
        juce::Label renderTargetLabel;
        juce::Label sequenceSelectorLabel;
        juce::TextEditor sequenceSelectorBox;
        juce::TextButton changeActiveSequenceButton { "Chg Seq" };
        juce::Label sequenceThoughtsLabel;
        juce::TextEditor sequenceThoughtsBox;
        juce::Label themeLabel;
        juce::Label musicXmlLabel;
        juce::Label exportFolderLabel;
        juce::Label projectDefaultsLabel;
        juce::Label soundFontLabel;
        juce::Label fluidSynthLabel;
        juce::Label ffmpegLabel;
        juce::Label backendLabel;
        juce::Label sfzLabel;
        juce::Label sfizzLabel;
        juce::Label sfzKeySwitchLabel;
        juce::Label sfzCc1Label;
        juce::Label sfzCc11Label;
        juce::Label baseNameLabel;
        juce::Label outputFormatLabel;
        juce::Label audioClipFormatLabel;
        juce::Label audioClipQualityLabel;
        juce::Label sampleRateLabel;
        juce::Label bitrateLabel;
        juce::Label channelsLabel;
        juce::Label renderWorkersLabel;
        juce::Label renderOutputSummaryLabel;
        juce::Label trackLabel;
        juce::Label trackSoundLibraryLabel;
        juce::Label instrumentLabel;
        juce::Label vstEffectLabel;
        juce::Label vstEffect2Label;
        juce::Label vstEffectStatusLabel;
        juce::Label trackVolumeLabel;
        juce::Label masterVolumeLabel;
        juce::Label tempoLabel;
        juce::Label timeSignatureLabel;
        juce::Label loopCountLabel;
        juce::Label noteEditorLabel;
        juce::Label pianoRollHelpLabel;
        juce::Label pianoRollBpmLabel;
        juce::Label pianoRollTimeSigLabel;
        juce::Label pianoRollBeatWindowLabel;
        juce::Label pianoRollStartBeatLabel;
        juce::Label pianoRollNoteLengthLabel;
        juce::Label pianoRollVelocityLabel;
        juce::Label pianoRollSnapLabel;
        juce::Label pianoRollPageLabel;
        juce::Label pianoRollPageInputLabel;
        juce::Label pianoRollKeyRangeLabel;
        juce::Label pianoRollKeyJumpLabel;

        juce::TextEditor musicXmlPathBox;
        juce::TextEditor exportFolderBox;
        juce::TextEditor soundFontPathBox;
        juce::TextEditor fluidSynthPathBox;
        juce::TextEditor ffmpegPathBox;
        juce::TextEditor sfzPathBox;
        juce::TextEditor sfizzPathBox;
        juce::TextEditor sfzKeySwitchBox;
        juce::TextEditor sfzCc1Box;
        juce::TextEditor sfzCc11Box;
        juce::TextEditor baseNameBox;
        juce::TextEditor metadataTitleBox;
        juce::TextEditor metadataArtistBox;
        juce::TextEditor metadataAlbumBox;
        juce::TextEditor metadataTrackNumberBox;
        juce::TextEditor metadataYearBox;
        juce::TextEditor tempoBox;
        juce::TextEditor timeSignatureBox;
        juce::TextEditor loopCountBox;
        juce::TextEditor pianoRollBpmBox;
        juce::TextEditor pianoRollTimeSigBox;
        juce::TextEditor pianoRollBeatWindowBox;
        juce::TextEditor pianoRollStartBeatBox;
        juce::TextEditor pianoRollNoteLengthBox;
        juce::TextEditor pianoRollVelocityBox;
        juce::TextEditor pianoRollPageBox;
        juce::TextEditor pianoRollKeyJumpBox;
        juce::TextEditor trackSoundLibraryBox;

        juce::ComboBox soundFontCombo;
        juce::ComboBox backendCombo;
        juce::ComboBox sfzCombo;
        juce::ComboBox outputFormatCombo;
        juce::ComboBox audioClipFormatCombo;
        juce::ComboBox audioClipQualityCombo;
        juce::ComboBox sampleRateCombo;
        juce::ComboBox bitrateCombo;
        juce::ComboBox channelsCombo;
        juce::ComboBox renderWorkersCombo;
        juce::ComboBox themeCombo;
        juce::ComboBox trackCombo;
        juce::ComboBox instrumentCombo;
        juce::ComboBox vstEffectCombo;
        juce::ComboBox vstEffect2Combo;
        juce::ComboBox trackBackendCombo;
        juce::TextEditor pianoRollSnapBox;
        juce::ComboBox pianoRollBeatWindowCombo;

        juce::ToggleButton muteToggle {"Mute"};
        juce::ToggleButton soloToggle {"Solo"};
        juce::ToggleButton enableVstEffectsToggle {"Enable"};
        juce::ToggleButton bypassVstEffectToggle {"Bypass"};
        juce::ToggleButton enableVstEffect2Toggle {"Enable"};
        juce::ToggleButton bypassVstEffect2Toggle {"Bypass"};

        juce::Slider trackVolumeSlider;
        juce::Slider masterVolumeSlider;

        juce::TextEditor trackSummaryBox;
        SequenceConsoleComponent trackManagerBox;
        juce::TextEditor trackManagerSelectBox;
        juce::TextEditor trackManagerStartBeatBox;
        juce::TextEditor trackManagerSectionBox;
        juce::TextEditor trackManagerSectionStartBeatBox;
        juce::TextEditor trackManagerMapStartBeatBox;
        juce::TextEditor trackManagerMapBeatWindowBox;
        SequenceConsoleComponent logBox;
        juce::TextEditor noteEditorBox;

        std::map<int, juce::String> lastVstEffectRenderStatusByTrack;

        struct PianoRollEditorWindowState
        {
            int trackIndex = -1;
            bool dirty = false;
            bool suppressDirty = false;

            PianoRollComponent roll;
            juce::TextEditor bpmBox;
            juce::TextEditor timeSigBox;
            juce::TextEditor beatWindowBox;
            juce::TextEditor startBeatBox;
            juce::TextEditor noteLengthBox;
            juce::TextEditor velocityBox;
            juce::TextEditor snapBox;
            juce::TextEditor pageBox;
            juce::TextEditor keyJumpBox;
            juce::TextEditor trackSoundLibraryBox;
            juce::ComboBox trackBackendCombo;
            juce::ComboBox instrumentCombo;

            juce::Label pageLabel;
            juce::Label keyRangeLabel;
            juce::Label keyJumpLabel;

            juce::TextButton keyJumpButton {"Go Key"};
            juce::TextButton notesDownButton {"Notes Down"};
            juce::TextButton notesUpButton {"Notes Up"};
            juce::TextButton beat4Button {"4"};
            juce::TextButton beat8Button {"8"};
            juce::TextButton beat16Button {"16"};
            juce::TextButton beat32Button {"32"};
            juce::TextButton beat64Button {"64"};
            juce::TextButton previousButton {"Prev Window"};
            juce::TextButton nextButton {"Next Window"};
            juce::TextButton jumpPageButton {"Go"};
            juce::TextButton copyButton {"Copy"};
            juce::TextButton pasteButton {"Paste"};
            juce::TextButton undoButton {"Undo"};
            juce::TextButton redoButton {"Redo"};
            juce::TextButton clearSelectionButton {"Clear Sel"};
            juce::TextButton changeLibraryButton {"Change Library"};

            std::vector<mw::audio::SoundFontPreset> instrumentPresets;
            mw::core::InstrumentAssignment pendingInstrumentAssignment;
            bool hasPendingInstrumentAssignment = false;
            std::vector<mw::core::InstrumentAssignment> instrumentUndoStack;
            std::vector<mw::core::InstrumentAssignment> instrumentRedoStack;
            bool suppressInstrumentChange = false;

            std::unique_ptr<juce::Component> content;
            std::unique_ptr<juce::DocumentWindow> window;
        };

        PianoRollComponent pianoRoll;
        std::map<int, std::unique_ptr<PianoRollEditorWindowState>> pianoRollEditorWindows;
        std::unique_ptr<juce::DocumentWindow> pianoRollWindow;
        std::unique_ptr<juce::DocumentWindow> pianoRollPreviewPlayerWindow;
        std::unique_ptr<juce::DocumentWindow> rawNotesWindow;
        std::unique_ptr<juce::DocumentWindow> trackManagerWindow;
        std::unique_ptr<juce::DocumentWindow> sequencePickerWindow;
        std::unique_ptr<juce::DocumentWindow> sequenceThoughtsWindow;
        std::unique_ptr<juce::DocumentWindow> renderSettingsWindow;
        std::unique_ptr<juce::DocumentWindow> audioRecorderWindow;
        std::unique_ptr<juce::DocumentWindow> vstPluginManagerWindow;
        std::unique_ptr<juce::DocumentWindow> vstSettingsWindow;
        std::unique_ptr<juce::DocumentWindow> vstHostHelperStatusWindow;
        std::map<int, std::unique_ptr<juce::DocumentWindow>> vstPluginEditorWindows;
        std::map<int, std::unique_ptr<juce::DocumentWindow>> vstEffectEditorWindows;
        std::map<int, int> windowMenuVstInstrumentCommandToTrack;
        std::map<int, int> windowMenuVstEffectCommandToKey;
        std::unique_ptr<juce::DocumentWindow> projectInfoWindow;
        std::unique_ptr<juce::Component> rawNotesContent;
        std::unique_ptr<juce::Component> trackManagerContent;
        std::unique_ptr<juce::Component> audioRecorderContent;
        std::unique_ptr<juce::Component> renderSettingsContent;
        std::unique_ptr<juce::Component> projectInfoContent;
        int pianoRollOpenTrackIndex = -1;
        bool pianoRollEditorDirty = false;
        bool suppressPianoRollEditorDirty = false;
        bool suppressTrackComboSwitchPrompt = false;
        bool suppressTrackManagerConsoleFollow = false;
        std::filesystem::path lastPianoRollPreviewMidiPath;
        std::filesystem::path lastPianoRollPreviewWavPath;
        std::vector<mw::core::NoteEvent> lastPianoRollPreviewNotes;
        std::vector<std::filesystem::path> generatedPreviewFiles;
        std::unique_ptr<mw::audio::AudioClipRecorder> audioClipRecorder;
        juce::AudioFormatManager audioRecorderTestFormatManager;
        juce::AudioDeviceManager audioRecorderTestPlaybackDeviceManager;
        juce::AudioSourcePlayer audioRecorderTestSourcePlayer;
        juce::AudioTransportSource audioRecorderTestTransport;
        std::unique_ptr<juce::AudioFormatReaderSource> audioRecorderTestReaderSource;
        std::filesystem::path audioRecorderTestTempWavPath;
        bool audioRecorderTestActive = false;
        bool audioRecorderTestPlaybackActive = false;
        double audioRecorderMicGainDb = 0.0;
        bool audioRecorderTrackLiveEffectEnabled = false;
        std::optional<std::filesystem::path> audioRecordingSessionFolderPath;
        std::filesystem::path activeRecordingTempWavPath;
        std::filesystem::path activeRecordingSourceWavPath;
        std::optional<std::filesystem::path> activeRecordingProjectFolder;
        bool audioRecordingTakeStopped = false;
        bool audioRecordingTakeDirty = false;
        double activeRecordingSampleRate = 48000.0;
        int activeRecordingChannelCount = 1;
        long long activeRecordingStartTick = 0;
        int activeRecordingSequenceNumber = 1;
        int activeRecordingTrackIndex = -1;
        double lastPianoRollPreviewDurationBeats = 0.0;
        double lastPianoRollPreviewTempoBpm = 120.0;
        bool pianoRollPreviewPaused = false;
        int lastPianoRollPreviewScope = 0; // 0 Piano Roll, 1 selected track, 2 active sequence, 3 project
        double pendingPianoRollPreviewStartSeconds = 0.0;
        std::unique_ptr<juce::Component> pianoRollContent;
        std::unique_ptr<juce::Component> pianoRollPreviewPlayerContent;
        int currentThemePresetId = 1;
        std::vector<std::filesystem::path> themePresetFiles;

        struct ImportSectionInfo
        {
            int id = 0;
            juce::String name;
            std::filesystem::path sourcePath;
            std::int64_t startTick = 0;
            std::int64_t endTick = 0;
            std::vector<int> trackNumbers;
            bool isLayer = false;
            juce::String createdBy = "manual";
            juce::String notes;
            bool locked = false;
        };

        std::vector<ImportSectionInfo> importSections;
        int activeImportSectionIndex = -1;
        int sequenceMapFirstIndex = 0;
        int sequenceMapStartBeat = 0;
        int sequenceMapBeatWindow = 0;
        int trackManagerFilterId = 1;
        bool shortenImportedTrackNames = false;
        bool suppressSequenceThoughtsChange = false;
        std::map<int, juce::Colour> sequenceColourOverrides;

        struct TrackManagerUndoState
        {
            mw::core::Project project;
            std::vector<ImportSectionInfo> importSections;
            std::map<int, juce::Colour> sequenceColourOverrides;
            int activeImportSectionIndex = -1;
            int sequenceMapFirstIndex = 0;
            int sequenceMapStartBeat = 0;
            int sequenceMapBeatWindow = 0;
            juce::String label;
        };

        std::vector<TrackManagerUndoState> trackManagerUndoStack;

        bool trackManagerEditorDirty = false;
        juce::String trackManagerDirtyReason;
        bool trackManagerSessionProjectWasDirty = false;
        std::optional<mw::core::Project> trackManagerSessionProjectSnapshot;
        std::vector<ImportSectionInfo> trackManagerSessionImportSectionsSnapshot;
        std::map<int, juce::Colour> trackManagerSessionColourOverridesSnapshot;
        int trackManagerSessionActiveImportSectionIndexSnapshot = -1;
        int trackManagerSessionSequenceMapFirstIndexSnapshot = 0;
        int trackManagerSessionSequenceMapStartBeatSnapshot = 0;
        int trackManagerSessionSequenceMapBeatWindowSnapshot = 0;
        std::vector<int> trackManagerExternalTrackStateUpdates;
        bool trackManagerExternalProjectDirtyWhileEditing = false;

        std::unique_ptr<juce::DocumentWindow> sequenceColorWindow;
        std::unique_ptr<juce::Component> sequenceColorContent;
        bool helperBubblesEnabled = true;
        bool vstCompatibilityWarningsEnabled = true;
        bool vstSafePluginUiMode = false;
        int vstWarningStyleId = 1;
        int vstMaxOpenPluginWindows = 4;
        bool vstExperimentalWarningAcknowledged = false;
        mw::vst::GraphicsProfile vstGraphicsProfile;
        VstHostHelperStatusSnapshot vstHostHelperStatus;
        HelperTooltipLookAndFeel helperTooltipLookAndFeel;
        std::unique_ptr<juce::TooltipWindow> helperTooltipWindow;

        std::vector<std::filesystem::path> detectedSoundFonts;
        std::vector<std::filesystem::path> detectedSfzFiles;
        std::vector<mw::vst::VstPluginDescriptor> detectedVstPlugins;
        std::vector<mw::audio::SoundFontPreset> detectedPresets;
        std::optional<mw::core::Project> currentProject;
        std::optional<std::filesystem::path> currentProjectFilePath;
        std::optional<std::filesystem::path> currentProjectFolderPath;
        bool projectDirty = false;
        bool suppressDirtyTracking = false;
        int appliedProjectBackendId = 1;

        std::atomic<bool> renderingInProgress { false };
        std::atomic<bool> cancelRenderRequested { false };
        std::thread renderThread;

        std::unique_ptr<juce::FileChooser> activeFileChooser;
    };
}
