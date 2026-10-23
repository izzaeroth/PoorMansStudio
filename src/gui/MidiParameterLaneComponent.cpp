#include "gui/MidiParameterLaneComponent.h"

#include <algorithm>
#include <cmath>

namespace mw::gui
{
    MidiParameterLaneComponent::MidiParameterLaneComponent(Callbacks callbacksIn)
        : callbacks(std::move(callbacksIn)), eventList("MIDI Parameter Events", this)
    {
        setOpaque(true);
        setTooltip(mw::midi::midiParameterLaneTooltip);

        titleLabel.setText("MIDI Parameter Lane (RPN / NRPN)", juce::dontSendNotification);
        titleLabel.setFont(juce::FontOptions(18.0f, juce::Font::bold));
        titleLabel.setColour(juce::Label::textColourId, juce::Colour(0xffe8edf5));
        titleLabel.setTooltip(mw::midi::midiParameterLaneTooltip);

        trackLabel.setColour(juce::Label::textColourId, juce::Colour(0xffc9d2df));
        statusLabel.setColour(juce::Label::textColourId, juce::Colour(0xff9eabbc));
        trackLabel.setJustificationType(juce::Justification::centredLeft);
        statusLabel.setJustificationType(juce::Justification::centredLeft);

        modeLabel.setText("Mode", juce::dontSendNotification);
        channelLabel.setText("Channel", juce::dontSendNotification);
        parameterLabel.setText("Parameter", juce::dontSendNotification);
        valueLabel.setText("Value", juce::dontSendNotification);
        snapLabel.setText("Snap", juce::dontSendNotification);
        tickHeaderLabel.setText("Tick", juce::dontSendNotification);
        parameterHeaderLabel.setText("Parameter", juce::dontSendNotification);
        valueHeaderLabel.setText("Value", juce::dontSendNotification);
        for (auto* label : { &modeLabel, &channelLabel, &parameterLabel, &valueLabel, &snapLabel,
                             &tickHeaderLabel, &parameterHeaderLabel, &valueHeaderLabel })
            label->setColour(juce::Label::textColourId, juce::Colour(0xffd5dce6));

        modeCombo.addItem("RPN", 1);
        modeCombo.addItem("NRPN", 2);
        modeCombo.setSelectedId(1, juce::dontSendNotification);
        modeCombo.setTooltip(mw::midi::midiParameterModeTooltip);
        modeCombo.onChange = [this]
        {
            selectedEventId = 0;
            refreshLane(false);
        };

        for (int channel = 1; channel <= 16; ++channel)
            channelCombo.addItem(juce::String(channel), channel);
        channelCombo.setSelectedId(1, juce::dontSendNotification);
        channelCombo.setTooltip(mw::midi::midiParameterChannelTooltip);
        channelCombo.onChange = [this]
        {
            selectedEventId = 0;
            refreshLane(false);
        };

        snapCombo.addItem("Off", 1);
        snapCombo.addItem("1/4", 2);
        snapCombo.addItem("1/8", 3);
        snapCombo.addItem("1/16", 4);
        snapCombo.addItem("1/32", 5);
        snapCombo.addItem("1/64", 6);
        snapCombo.setSelectedId(1, juce::dontSendNotification);
        snapCombo.setTooltip("Snap - Applies to Add at Preview and Move to Preview. Off keeps the exact transport tick.");
        snapCombo.onChange = [this] { refreshLane(true); };

        parameterSlider.setSliderStyle(juce::Slider::IncDecButtons);
        parameterSlider.setTextBoxStyle(juce::Slider::TextBoxLeft, false, 90, 22);
        parameterSlider.setRange(0.0, 16383.0, 1.0);
        parameterSlider.setValue(0.0, juce::dontSendNotification);
        parameterSlider.setTooltip(mw::midi::midiParameterParameterTooltip);

        valueSlider.setSliderStyle(juce::Slider::IncDecButtons);
        valueSlider.setTextBoxStyle(juce::Slider::TextBoxLeft, false, 90, 22);
        valueSlider.setRange(0.0, 16383.0, 1.0);
        valueSlider.setValue(0.0, juce::dontSendNotification);
        valueSlider.setTooltip(mw::midi::midiParameterValueTooltip);

        eventList.setRowHeight(26);
        eventList.setMultipleSelectionEnabled(false);
        eventList.setColour(juce::ListBox::backgroundColourId, juce::Colour(0xff171b22));
        eventList.setColour(juce::ListBox::outlineColourId, juce::Colour(0xff47515f));
        eventList.setOutlineThickness(1);
        eventList.setTooltip("Parameter event list - Selects the exact stable RPN/NRPN event for the displayed mode and channel.");

        addButton.setTooltip("Add at Preview - Adds/replaces one semantic parameter transaction at the current preview tick after Snap.");
        moveButton.setTooltip("Move to Preview - Moves the selected semantic parameter event to the preview tick after Snap.");
        setParameterButton.setTooltip("Set Parameter - Changes the selected event's 14-bit parameter number atomically.");
        setValueButton.setTooltip("Set Value - Changes the selected event's 14-bit Data Entry value atomically.");
        deleteButton.setTooltip("Delete - Deletes the selected semantic parameter event.");
        undoButton.setTooltip("Undo Parameter - Undoes one RPN/NRPN Parameter Lane edit transaction.");
        redoButton.setTooltip("Redo Parameter - Redoes one RPN/NRPN Parameter Lane edit transaction.");

        addButton.onClick = [this] { performAdd(); };
        moveButton.onClick = [this] { performMove(); };
        setParameterButton.onClick = [this] { performSetParameter(); };
        setValueButton.onClick = [this] { performSetValue(); };
        deleteButton.onClick = [this] { performDelete(); };
        undoButton.onClick = [this] { performUndo(); };
        redoButton.onClick = [this] { performRedo(); };

        juce::Component* components[] = { &titleLabel, &trackLabel, &statusLabel,
                                          &modeLabel, &modeCombo, &channelLabel, &channelCombo,
                                          &parameterLabel, &parameterSlider, &valueLabel, &valueSlider,
                                          &snapLabel, &snapCombo, &tickHeaderLabel, &parameterHeaderLabel, &valueHeaderLabel,
                                          &eventList, &addButton, &moveButton, &setParameterButton, &setValueButton,
                                          &deleteButton, &undoButton, &redoButton };
        for (auto* component : components)
            addAndMakeVisible(component);

        startTimerHz(4);
        refreshNow();
    }

    MidiParameterLaneComponent::~MidiParameterLaneComponent()
    {
        stopTimer();
    }

    void MidiParameterLaneComponent::paint(juce::Graphics& g)
    {
        g.fillAll(juce::Colour(0xff10141a));
    }

    void MidiParameterLaneComponent::resized()
    {
        auto bounds = getLocalBounds().reduced(12);
        titleLabel.setBounds(bounds.removeFromTop(30));
        trackLabel.setBounds(bounds.removeFromTop(24));
        statusLabel.setBounds(bounds.removeFromTop(24));
        bounds.removeFromTop(6);

        auto selectors = bounds.removeFromTop(34);
        modeLabel.setBounds(selectors.removeFromLeft(48));
        modeCombo.setBounds(selectors.removeFromLeft(92).reduced(3, 2));
        channelLabel.setBounds(selectors.removeFromLeft(62));
        channelCombo.setBounds(selectors.removeFromLeft(66).reduced(3, 2));
        parameterLabel.setBounds(selectors.removeFromLeft(72));
        parameterSlider.setBounds(selectors.removeFromLeft(150).reduced(3, 2));
        valueLabel.setBounds(selectors.removeFromLeft(46));
        valueSlider.setBounds(selectors.removeFromLeft(150).reduced(3, 2));
        snapLabel.setBounds(selectors.removeFromLeft(42));
        snapCombo.setBounds(selectors.removeFromLeft(84).reduced(3, 2));

        bounds.removeFromTop(8);
        auto headers = bounds.removeFromTop(24);
        tickHeaderLabel.setBounds(headers.removeFromLeft(200));
        parameterHeaderLabel.setBounds(headers.removeFromLeft(150));
        valueHeaderLabel.setBounds(headers.removeFromLeft(150));
        eventList.setBounds(bounds.removeFromTop(std::max(120, bounds.getHeight() - 88)));
        bounds.removeFromTop(8);

        auto editRow = bounds.removeFromTop(36);
        addButton.setBounds(editRow.removeFromLeft(130).reduced(3, 2));
        moveButton.setBounds(editRow.removeFromLeft(140).reduced(3, 2));
        setParameterButton.setBounds(editRow.removeFromLeft(130).reduced(3, 2));
        setValueButton.setBounds(editRow.removeFromLeft(110).reduced(3, 2));
        deleteButton.setBounds(editRow.removeFromLeft(80).reduced(3, 2));
        undoButton.setBounds(editRow.removeFromLeft(125).reduced(3, 2));
        redoButton.setBounds(editRow.removeFromLeft(125).reduced(3, 2));
    }

    int MidiParameterLaneComponent::getNumRows()
    {
        return static_cast<int>(visibleEvents.size());
    }

    void MidiParameterLaneComponent::paintListBoxItem(int rowNumber, juce::Graphics& g, int width, int height, bool rowIsSelected)
    {
        if (rowNumber < 0 || rowNumber >= static_cast<int>(visibleEvents.size())) return;
        if (rowIsSelected)
        {
            g.setColour(juce::Colour(0xff315d87));
            g.fillRect(0, 0, width, height);
        }
        else if ((rowNumber & 1) != 0)
        {
            g.setColour(juce::Colour(0xff1d222a));
            g.fillRect(0, 0, width, height);
        }
        const auto& event = visibleEvents[static_cast<std::size_t>(rowNumber)];
        auto row = juce::Rectangle<int>(0, 0, width, height).reduced(8, 0);
        g.setColour(juce::Colour(0xffe1e7ef));
        g.setFont(juce::FontOptions(13.0f));
        g.drawText(juce::String(static_cast<juce::int64>(event.tick)), row.removeFromLeft(200), juce::Justification::centredLeft, true);
        g.drawText(juce::String(event.parameter), row.removeFromLeft(150), juce::Justification::centredLeft, true);
        g.drawText(juce::String(event.value), row.removeFromLeft(150), juce::Justification::centredLeft, true);
    }

    void MidiParameterLaneComponent::selectedRowsChanged(int lastRowSelected)
    {
        if (lastRowSelected >= 0 && lastRowSelected < static_cast<int>(visibleEvents.size()))
        {
            const auto& event = visibleEvents[static_cast<std::size_t>(lastRowSelected)];
            selectedEventId = event.stableId;
            parameterSlider.setValue(event.parameter, juce::dontSendNotification);
            valueSlider.setValue(event.value, juce::dontSendNotification);
        }
        else selectedEventId = 0;
        refreshEnabledState();
    }

    void MidiParameterLaneComponent::timerCallback()
    {
        refreshNow();
    }

    mw::core::Project* MidiParameterLaneComponent::currentProject() const
    {
        return callbacks.project ? callbacks.project() : nullptr;
    }

    mw::core::Track* MidiParameterLaneComponent::findTrack(mw::core::StableId trackId) const
    {
        auto* project = currentProject();
        if (project == nullptr || trackId == 0) return nullptr;
        for (auto& track : project->getTracks())
            if (track.getStableId() == trackId) return &track;
        return nullptr;
    }

    mw::core::StableId MidiParameterLaneComponent::currentTrackId() const
    {
        return callbacks.selectedTrackId ? callbacks.selectedTrackId() : 0;
    }

    mw::midi::MidiParameterMode MidiParameterLaneComponent::selectedMode() const
    {
        return modeCombo.getSelectedId() == 2 ? mw::midi::MidiParameterMode::Nrpn : mw::midi::MidiParameterMode::Rpn;
    }

    mw::midi::MidiParameterSnapGrid MidiParameterLaneComponent::selectedSnapGrid() const
    {
        switch (snapCombo.getSelectedId())
        {
            case 2: return mw::midi::MidiParameterSnapGrid::Quarter;
            case 3: return mw::midi::MidiParameterSnapGrid::Eighth;
            case 4: return mw::midi::MidiParameterSnapGrid::Sixteenth;
            case 5: return mw::midi::MidiParameterSnapGrid::ThirtySecond;
            case 6: return mw::midi::MidiParameterSnapGrid::SixtyFourth;
            case 1:
            default: return mw::midi::MidiParameterSnapGrid::Off;
        }
    }

    int MidiParameterLaneComponent::selectedChannel() const
    {
        return std::clamp(channelCombo.getSelectedId(), 1, 16);
    }

    int MidiParameterLaneComponent::selectedParameter() const
    {
        return std::clamp(static_cast<int>(std::llround(parameterSlider.getValue())), 0, 16383);
    }

    int MidiParameterLaneComponent::selectedValue() const
    {
        return std::clamp(static_cast<int>(std::llround(valueSlider.getValue())), 0, 16383);
    }

    std::int64_t MidiParameterLaneComponent::currentPreviewTick() const
    {
        return callbacks.previewTick ? std::max<std::int64_t>(0, callbacks.previewTick()) : 0;
    }

    std::int64_t MidiParameterLaneComponent::snappedPreviewTick() const
    {
        return mw::midi::snapMidiParameterTick(currentPreviewTick(), mw::core::Project::ticksPerQuarterNote, selectedSnapGrid());
    }

    void MidiParameterLaneComponent::refreshNow()
    {
        auto* project = currentProject();
        const auto projectId = project != nullptr ? project->getStableId() : 0;
        if (project != lastProjectPointer || projectId != lastProjectId)
        {
            editHistory.clear();
            selectedEventId = 0;
            lastProjectPointer = project;
            lastProjectId = projectId;
        }
        refreshLane(true);
    }

    void MidiParameterLaneComponent::refreshLane(bool preserveSelection)
    {
        if (!preserveSelection) selectedEventId = 0;
        visibleEvents.clear();
        auto* track = findTrack(currentTrackId());
        if (track == nullptr)
        {
            trackLabel.setText("Track: No selected track", juce::dontSendNotification);
            statusLabel.setText("Select a MIDI track to edit RPN/NRPN events.", juce::dontSendNotification);
            selectedEventId = 0;
        }
        else if (track->isAudioClipTrack())
        {
            trackLabel.setText("Track: " + juce::String(track->getName()), juce::dontSendNotification);
            statusLabel.setText("MIDI Parameter Lane is unavailable for AudioClip tracks.", juce::dontSendNotification);
            selectedEventId = 0;
        }
        else
        {
            trackLabel.setText("Track: " + juce::String(track->getName()), juce::dontSendNotification);
            const bool rpn = selectedMode() == mw::midi::MidiParameterMode::Rpn;
            statusLabel.setText((rpn ? "RPN" : "NRPN") + juce::String(" | preview tick ")
                + juce::String(static_cast<juce::int64>(currentPreviewTick())) + " -> snapped "
                + juce::String(static_cast<juce::int64>(snappedPreviewTick()))
                + (rpn ? " | RPN 16383 is reserved Null." : " | Parameter/value range 0..16383."), juce::dontSendNotification);
            if (rpn)
            {
                for (const auto& event : mw::midi::midiRpnLaneEvents(*track, selectedChannel()))
                    visibleEvents.push_back({event.stableId, event.tick, event.parameter, event.value});
            }
            else
            {
                for (const auto& event : mw::midi::midiNrpnLaneEvents(*track, selectedChannel()))
                    visibleEvents.push_back({event.stableId, event.tick, event.parameter, event.value});
            }
        }

        int selectedRow = -1;
        for (int row = 0; row < static_cast<int>(visibleEvents.size()); ++row)
            if (visibleEvents[static_cast<std::size_t>(row)].stableId == selectedEventId) { selectedRow = row; break; }
        if (selectedRow >= 0) eventList.selectRow(selectedRow, false, true);
        else { selectedEventId = 0; eventList.deselectAllRows(); }
        eventList.updateContent();
        eventList.repaint();
        refreshEnabledState();
    }

    void MidiParameterLaneComponent::refreshEnabledState()
    {
        auto* track = findTrack(currentTrackId());
        const bool editable = track != nullptr && !track->isAudioClipTrack();
        const bool selected = editable && selectedEventId != 0;
        modeCombo.setEnabled(editable);
        channelCombo.setEnabled(editable);
        parameterSlider.setEnabled(editable);
        valueSlider.setEnabled(editable);
        snapCombo.setEnabled(editable);
        addButton.setEnabled(editable);
        moveButton.setEnabled(selected);
        setParameterButton.setEnabled(selected);
        setValueButton.setEnabled(selected);
        deleteButton.setEnabled(selected);
        undoButton.setEnabled(editable && editHistory.canUndo());
        redoButton.setEnabled(editable && editHistory.canRedo());
    }

    void MidiParameterLaneComponent::selectEventById(mw::core::StableId eventId)
    {
        selectedEventId = eventId;
        refreshLane(true);
    }

    void MidiParameterLaneComponent::performAdd()
    {
        auto* project = currentProject(); if (project == nullptr) return;
        const auto result = editHistory.addAtTick(*project, currentTrackId(), selectedMode(), snappedPreviewTick(),
                                                   selectedChannel(), selectedParameter(), selectedValue());
        if (!result.changed && selectedMode() == mw::midi::MidiParameterMode::Rpn && selectedParameter() == mw::core::midiRpnNullParameter)
            log("RPN 16383 (127/127) is the MIDI Null Function and cannot be stored as an authored RPN event.");
        if (result.eventId != 0) selectedEventId = result.eventId;
        commitChangedEdit(result.changed, "Parameter event added/updated at tick " + juce::String(static_cast<juce::int64>(snappedPreviewTick())) + ".");
    }

    void MidiParameterLaneComponent::performMove()
    {
        auto* project = currentProject(); if (project == nullptr || selectedEventId == 0) return;
        commitChangedEdit(editHistory.moveToTick(*project, currentTrackId(), selectedMode(), selectedEventId, snappedPreviewTick()),
                          "Parameter event moved to tick " + juce::String(static_cast<juce::int64>(snappedPreviewTick())) + ".");
    }

    void MidiParameterLaneComponent::performSetParameter()
    {
        auto* project = currentProject(); if (project == nullptr || selectedEventId == 0) return;
        const bool changed = editHistory.setParameter(*project, currentTrackId(), selectedMode(), selectedEventId, selectedParameter());
        if (!changed && selectedMode() == mw::midi::MidiParameterMode::Rpn && selectedParameter() == mw::core::midiRpnNullParameter)
            log("RPN 16383 (127/127) is reserved as the MIDI Null Function.");
        commitChangedEdit(changed, "Parameter number changed to " + juce::String(selectedParameter()) + ".");
    }

    void MidiParameterLaneComponent::performSetValue()
    {
        auto* project = currentProject(); if (project == nullptr || selectedEventId == 0) return;
        commitChangedEdit(editHistory.setValue(*project, currentTrackId(), selectedMode(), selectedEventId, selectedValue()),
                          "Parameter value changed to " + juce::String(selectedValue()) + ".");
    }

    void MidiParameterLaneComponent::performDelete()
    {
        auto* project = currentProject(); if (project == nullptr || selectedEventId == 0) return;
        const bool changed = editHistory.deleteEvent(*project, currentTrackId(), selectedMode(), selectedEventId);
        if (changed) selectedEventId = 0;
        commitChangedEdit(changed, "Parameter event deleted.");
    }

    void MidiParameterLaneComponent::performUndo()
    {
        auto* project = currentProject(); if (project == nullptr) return;
        commitChangedEdit(editHistory.undo(*project), "Parameter edit undone.");
    }

    void MidiParameterLaneComponent::performRedo()
    {
        auto* project = currentProject(); if (project == nullptr) return;
        commitChangedEdit(editHistory.redo(*project), "Parameter edit redone.");
    }

    void MidiParameterLaneComponent::commitChangedEdit(bool changed, const juce::String& message)
    {
        if (!changed) { refreshLane(true); return; }
        if (callbacks.markProjectDirty) callbacks.markProjectDirty(currentTrackId());
        log(message);
        refreshLane(true);
    }

    void MidiParameterLaneComponent::log(const juce::String& message) const
    {
        if (callbacks.log) callbacks.log(message);
    }
}
