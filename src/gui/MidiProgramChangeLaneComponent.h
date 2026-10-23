#pragma once

#include <juce_gui_extra/juce_gui_extra.h>

#include <cstdint>
#include <functional>
#include <vector>

#include "core/Project.h"
#include "midi/MidiProgramChangeLane.h"

namespace mw::gui
{
    class MidiProgramChangeLaneComponent final : public juce::Component,
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

        explicit MidiProgramChangeLaneComponent(Callbacks callbacks);
        ~MidiProgramChangeLaneComponent() override;

        void resized() override;
        void paint(juce::Graphics& g) override;
        void mouseDown(const juce::MouseEvent& event) override;
        void mouseDrag(const juce::MouseEvent& event) override;
        void mouseUp(const juce::MouseEvent& event) override;
        void mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) override;
        bool keyPressed(const juce::KeyPress& key) override;
        void refreshNow();

    private:
        int getNumRows() override;
        void paintListBoxItem(int rowNumber, juce::Graphics& g, int width, int height, bool rowIsSelected) override;
        void selectedRowsChanged(int lastRowSelected) override;
        void timerCallback() override;

        mw::core::Project* currentProject() const;
        mw::core::Track* findTrack(mw::core::StableId trackId) const;
        mw::core::StableId currentTrackId() const;
        int selectedChannel() const;
        int selectedProgram() const;
        std::int64_t currentPreviewTick() const;
        mw::midi::MidiProgramChangeSnapGrid selectedSnapGrid() const;
        double graphTicksPerPixel() const;
        juce::Rectangle<int> graphPlotBounds() const;
        std::int64_t graphViewEndTick() const;
        void ensureGraphViewInitialized();
        void centreGraphOnPreview();
        void paintGraph(juce::Graphics& g);
        void selectEventById(mw::core::StableId eventId);
        const mw::core::MidiProgramChangeEvent* selectedVisibleEvent() const;
        void cancelGraphDrag();
        void refreshLane(bool preserveSelection = true);
        void refreshEnabledState();
        void performAdd();
        void performMove();
        void performSetProgram();
        void performDelete();
        void performUndo();
        void performRedo();
        void commitChangedEdit(bool changed, const juce::String& message);
        void log(const juce::String& message) const;

        Callbacks callbacks;
        mw::midi::MidiProgramChangeEditHistory editHistory;
        std::vector<mw::core::MidiProgramChangeEvent> visibleEvents;
        mw::core::StableId selectedEventId = 0;
        mw::core::StableId lastProjectId = 0;
        mw::core::StableId graphTrackId = 0;
        mw::core::Project* lastProjectPointer = nullptr;

        juce::Rectangle<int> graphBounds;
        std::int64_t graphViewStartTick = 0;
        bool graphViewInitialized = false;
        bool graphDragActive = false;
        mw::core::StableId graphDragEventId = 0;
        std::int64_t graphDragOriginalTick = 0;
        int graphDragOriginalProgram = 0;
        std::int64_t graphDragPreviewTick = 0;
        int graphDragPreviewProgram = 0;

        juce::Label titleLabel;
        juce::Label trackLabel;
        juce::Label statusLabel;
        juce::Label channelLabel;
        juce::Label programLabel;
        juce::Label snapLabel;
        juce::Label zoomLabel;
        juce::Label tickHeaderLabel;
        juce::Label programHeaderLabel;
        juce::ComboBox channelCombo;
        juce::ComboBox snapCombo;
        juce::Slider programSlider;
        juce::Slider zoomSlider;
        juce::ListBox eventList;
        juce::TextButton addButton { "Add at Preview" };
        juce::TextButton moveButton { "Move to Preview" };
        juce::TextButton setProgramButton { "Set Program" };
        juce::TextButton deleteButton { "Delete" };
        juce::TextButton undoButton { "Undo Program" };
        juce::TextButton redoButton { "Redo Program" };
    };
}
