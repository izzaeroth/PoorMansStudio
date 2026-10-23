#pragma once

#include <juce_gui_extra/juce_gui_extra.h>

#include <cstdint>
#include <functional>
#include <vector>

#include "core/Project.h"
#include "midi/MidiParameterLane.h"

namespace mw::gui
{
    class MidiParameterLaneComponent final : public juce::Component,
                                             public juce::SettableTooltipClient,
                                             private juce::ListBoxModel,
                                             private juce::Timer
    {
    public:
        struct Callbacks
        {
            std::function<mw::core::Project*()> project;
            std::function<mw::core::StableId()> selectedTrackId;
            std::function<std::int64_t()> previewTick;
            std::function<void(mw::core::StableId)> markProjectDirty;
            std::function<void(const juce::String&)> log;
        };

        explicit MidiParameterLaneComponent(Callbacks callbacks);
        ~MidiParameterLaneComponent() override;

        void resized() override;
        void paint(juce::Graphics& g) override;
        void refreshNow();

    private:
        struct VisibleEvent
        {
            mw::core::StableId stableId = 0;
            std::int64_t tick = 0;
            int parameter = 0;
            int value = 0;
        };

        int getNumRows() override;
        void paintListBoxItem(int rowNumber, juce::Graphics& g, int width, int height, bool rowIsSelected) override;
        void selectedRowsChanged(int lastRowSelected) override;
        void timerCallback() override;

        mw::core::Project* currentProject() const;
        mw::core::Track* findTrack(mw::core::StableId trackId) const;
        mw::core::StableId currentTrackId() const;
        mw::midi::MidiParameterMode selectedMode() const;
        mw::midi::MidiParameterSnapGrid selectedSnapGrid() const;
        int selectedChannel() const;
        int selectedParameter() const;
        int selectedValue() const;
        std::int64_t currentPreviewTick() const;
        std::int64_t snappedPreviewTick() const;
        void refreshLane(bool preserveSelection = true);
        void refreshEnabledState();
        void selectEventById(mw::core::StableId eventId);
        void performAdd();
        void performMove();
        void performSetParameter();
        void performSetValue();
        void performDelete();
        void performUndo();
        void performRedo();
        void commitChangedEdit(bool changed, const juce::String& message);
        void log(const juce::String& message) const;

        Callbacks callbacks;
        mw::midi::MidiParameterEditHistory editHistory;
        std::vector<VisibleEvent> visibleEvents;
        mw::core::StableId selectedEventId = 0;
        mw::core::StableId lastProjectId = 0;
        mw::core::Project* lastProjectPointer = nullptr;

        juce::Label titleLabel;
        juce::Label trackLabel;
        juce::Label statusLabel;
        juce::Label modeLabel;
        juce::Label channelLabel;
        juce::Label parameterLabel;
        juce::Label valueLabel;
        juce::Label snapLabel;
        juce::Label tickHeaderLabel;
        juce::Label parameterHeaderLabel;
        juce::Label valueHeaderLabel;
        juce::ComboBox modeCombo;
        juce::ComboBox channelCombo;
        juce::ComboBox snapCombo;
        juce::Slider parameterSlider;
        juce::Slider valueSlider;
        juce::ListBox eventList;
        juce::TextButton addButton { "Add at Preview" };
        juce::TextButton moveButton { "Move to Preview" };
        juce::TextButton setParameterButton { "Set Parameter" };
        juce::TextButton setValueButton { "Set Value" };
        juce::TextButton deleteButton { "Delete" };
        juce::TextButton undoButton { "Undo Parameter" };
        juce::TextButton redoButton { "Redo Parameter" };
    };
}
