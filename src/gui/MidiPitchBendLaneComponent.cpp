#include "gui/MidiPitchBendLaneComponent.h"

#include <algorithm>
#include <cmath>

namespace mw::gui
{
    namespace
    {
        juce::String eventTickText(const mw::core::MidiPitchBendEvent& event)
        {
            return juce::String(static_cast<juce::int64>(event.tick));
        }

        constexpr int graphLeftMargin = 42;
        constexpr int graphRightMargin = 10;
        constexpr int graphTopMargin = 10;
        constexpr int graphBottomMargin = 24;
    }

    MidiPitchBendLaneComponent::MidiPitchBendLaneComponent(Callbacks callbacksIn)
        : callbacks(std::move(callbacksIn)), eventList("MIDI Pitch Bend Events", this)
    {
        setOpaque(true);
        setWantsKeyboardFocus(true);
        setTooltip(mw::midi::midiPitchBendGraphTooltip);

        titleLabel.setText("MIDI Pitch Bend Lane", juce::dontSendNotification);
        titleLabel.setFont(juce::FontOptions(18.0f, juce::Font::bold));
        titleLabel.setColour(juce::Label::textColourId, juce::Colour(0xffe8edf5));
        titleLabel.setTooltip(mw::midi::midiPitchBendLaneTooltip);

        trackLabel.setColour(juce::Label::textColourId, juce::Colour(0xffc9d2df));
        trackLabel.setJustificationType(juce::Justification::centredLeft);

        statusLabel.setColour(juce::Label::textColourId, juce::Colour(0xff9eabbc));
        statusLabel.setJustificationType(juce::Justification::centredLeft);

        channelLabel.setText("Channel", juce::dontSendNotification);
        valueLabel.setText("Value", juce::dontSendNotification);
        snapLabel.setText("Snap", juce::dontSendNotification);
        zoomLabel.setText("Zoom", juce::dontSendNotification);
        tickHeaderLabel.setText("Tick", juce::dontSendNotification);
        valueHeaderLabel.setText("Value", juce::dontSendNotification);

        for (auto* label : { &channelLabel, &valueLabel, &snapLabel, &zoomLabel, &tickHeaderLabel, &valueHeaderLabel })
            label->setColour(juce::Label::textColourId, juce::Colour(0xffd5dce6));

        for (int channel = 1; channel <= 16; ++channel)
            channelCombo.addItem(juce::String(channel), channel);
        channelCombo.setSelectedId(1, juce::dontSendNotification);
        channelCombo.setTooltip(mw::midi::midiPitchBendChannelTooltip);
        channelCombo.onChange = [this]
        {
            selectedEventId = 0;
            cancelGraphDrag();
            refreshLane(false);
        };

        snapCombo.addItem("Off", 1);
        snapCombo.addItem("1/4", 2);
        snapCombo.addItem("1/8", 3);
        snapCombo.addItem("1/16", 4);
        snapCombo.addItem("1/32", 5);
        snapCombo.addItem("1/64", 6);
        snapCombo.setSelectedId(1, juce::dontSendNotification);
        snapCombo.setTooltip("Snap - Horizontal edit snap only. Off keeps exact ticks; 1/4 through 1/64 use nearest-grid snapping with half-grid ties forward.");
        snapCombo.onChange = [this]
        {
            repaint(graphBounds);
        };

        valueSlider.setSliderStyle(juce::Slider::IncDecButtons);
        valueSlider.setTextBoxStyle(juce::Slider::TextBoxLeft, false, 72, 22);
        valueSlider.setRange(static_cast<double>(mw::core::midiPitchBendMinimum), static_cast<double>(mw::core::midiPitchBendMaximum), 1.0);
        valueSlider.setValue(0.0, juce::dontSendNotification);
        valueSlider.setTooltip(mw::midi::midiPitchBendValueTooltip);

        zoomSlider.setSliderStyle(juce::Slider::LinearHorizontal);
        zoomSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 58, 22);
        zoomSlider.setRange(0.25, 4.0, 0.25);
        zoomSlider.setValue(1.0, juce::dontSendNotification);
        zoomSlider.setTooltip("Zoom - Changes only the horizontal Pitch Bend Lane view scale; it does not edit project data.");
        zoomSlider.onValueChange = [this]
        {
            ensureGraphViewInitialized();
            repaint(graphBounds);
        };

        eventList.setRowHeight(26);
        eventList.setMultipleSelectionEnabled(false);
        eventList.setColour(juce::ListBox::backgroundColourId, juce::Colour(0xff171b22));
        eventList.setColour(juce::ListBox::outlineColourId, juce::Colour(0xff47515f));
        eventList.setOutlineThickness(1);
        eventList.setTooltip("Pitch Bend event list - Selects an exact stable project bend event on the displayed channel.");

        addButton.setTooltip("Add at Preview - Adds or replaces the displayed channel's Pitch Bend event at the current Preview tick.");
        moveButton.setTooltip("Move to Preview - Moves the selected Pitch Bend event to the current Preview tick. The moved stable ID survives an exact collision.");
        setValueButton.setTooltip("Set Value - Sets the selected Pitch Bend event to the signed Value control.");
        deleteButton.setTooltip("Delete - Deletes the selected Pitch Bend event.");
        undoButton.setTooltip("Undo Bend - Undoes one Pitch Bend Lane edit transaction.");
        redoButton.setTooltip("Redo Bend - Redoes one Pitch Bend Lane edit transaction.");

        addButton.onClick = [this] { performAdd(); };
        moveButton.onClick = [this] { performMove(); };
        setValueButton.onClick = [this] { performSetValue(); };
        deleteButton.onClick = [this] { performDelete(); };
        undoButton.onClick = [this] { performUndo(); };
        redoButton.onClick = [this] { performRedo(); };

        for (auto* component : std::initializer_list<juce::Component*> {
                 &titleLabel, &trackLabel, &statusLabel,
                 &channelLabel, &channelCombo, &valueLabel, &valueSlider,
                 &snapLabel, &snapCombo, &zoomLabel, &zoomSlider,
                 &tickHeaderLabel, &valueHeaderLabel, &eventList,
                 &addButton, &moveButton, &setValueButton, &deleteButton, &undoButton, &redoButton })
            addAndMakeVisible(component);

        refreshNow();
        startTimerHz(4);
    }

    MidiPitchBendLaneComponent::~MidiPitchBendLaneComponent()
    {
        stopTimer();
    }

    void MidiPitchBendLaneComponent::paint(juce::Graphics& g)
    {
        g.fillAll(juce::Colour(0xff20242b));
        g.setColour(juce::Colour(0xff343b46));
        g.drawRect(getLocalBounds(), 1);
        paintGraph(g);
    }

    void MidiPitchBendLaneComponent::resized()
    {
        auto area = getLocalBounds().reduced(14);
        titleLabel.setBounds(area.removeFromTop(28));
        trackLabel.setBounds(area.removeFromTop(24));
        statusLabel.setBounds(area.removeFromTop(24));
        area.removeFromTop(8);

        auto selectorRow = area.removeFromTop(34);
        channelLabel.setBounds(selectorRow.removeFromLeft(70));
        channelCombo.setBounds(selectorRow.removeFromLeft(96).reduced(3, 2));
        valueLabel.setBounds(selectorRow.removeFromLeft(58));
        valueSlider.setBounds(selectorRow.removeFromLeft(210).reduced(3, 2));
        area.removeFromTop(4);

        auto graphControls = area.removeFromTop(32);
        snapLabel.setBounds(graphControls.removeFromLeft(50));
        snapCombo.setBounds(graphControls.removeFromLeft(92).reduced(3, 2));
        zoomLabel.setBounds(graphControls.removeFromLeft(52));
        zoomSlider.setBounds(graphControls.removeFromLeft(210).reduced(3, 2));
        area.removeFromTop(5);

        const int graphHeight = std::clamp(area.getHeight() / 2, 180, 300);
        graphBounds = area.removeFromTop(graphHeight);
        area.removeFromTop(7);

        auto header = area.removeFromTop(24).reduced(6, 0);
        tickHeaderLabel.setBounds(header.removeFromLeft(190));
        valueHeaderLabel.setBounds(header.removeFromLeft(120));
        eventList.setBounds(area.removeFromTop(std::max(105, area.getHeight() - 94)));
        area.removeFromTop(8);

        auto editRow = area.removeFromTop(36);
        addButton.setBounds(editRow.removeFromLeft(145).reduced(3, 2));
        moveButton.setBounds(editRow.removeFromLeft(150).reduced(3, 2));
        setValueButton.setBounds(editRow.removeFromLeft(125).reduced(3, 2));
        deleteButton.setBounds(editRow.removeFromLeft(100).reduced(3, 2));
        area.removeFromTop(4);

        auto historyRow = area.removeFromTop(36);
        undoButton.setBounds(historyRow.removeFromLeft(120).reduced(3, 2));
        redoButton.setBounds(historyRow.removeFromLeft(120).reduced(3, 2));
    }

    juce::Rectangle<int> MidiPitchBendLaneComponent::graphPlotBounds() const
    {
        auto plot = graphBounds;
        plot.removeFromLeft(graphLeftMargin);
        plot.removeFromRight(graphRightMargin);
        plot.removeFromTop(graphTopMargin);
        plot.removeFromBottom(graphBottomMargin);
        return plot;
    }

    double MidiPitchBendLaneComponent::graphTicksPerPixel() const
    {
        return 8.0 / std::max(0.25, zoomSlider.getValue());
    }

    std::int64_t MidiPitchBendLaneComponent::graphViewEndTick() const
    {
        const auto plot = graphPlotBounds();
        return graphViewStartTick + static_cast<std::int64_t>(std::llround(static_cast<double>(std::max(1, plot.getWidth())) * graphTicksPerPixel()));
    }

    mw::midi::MidiPitchBendSnapGrid MidiPitchBendLaneComponent::selectedSnapGrid() const
    {
        switch (snapCombo.getSelectedId())
        {
            case 2: return mw::midi::MidiPitchBendSnapGrid::Quarter;
            case 3: return mw::midi::MidiPitchBendSnapGrid::Eighth;
            case 4: return mw::midi::MidiPitchBendSnapGrid::Sixteenth;
            case 5: return mw::midi::MidiPitchBendSnapGrid::ThirtySecond;
            case 6: return mw::midi::MidiPitchBendSnapGrid::SixtyFourth;
            case 1:
            default: return mw::midi::MidiPitchBendSnapGrid::Off;
        }
    }

    void MidiPitchBendLaneComponent::ensureGraphViewInitialized()
    {
        const auto trackId = currentTrackId();
        if (!graphViewInitialized || graphTrackId != trackId)
        {
            graphTrackId = trackId;
            graphViewInitialized = true;
            centreGraphOnPreview();
        }
    }

    void MidiPitchBendLaneComponent::centreGraphOnPreview()
    {
        const auto plot = graphPlotBounds();
        const auto visibleTicks = static_cast<std::int64_t>(std::llround(static_cast<double>(std::max(1, plot.getWidth())) * graphTicksPerPixel()));
        graphViewStartTick = std::max<std::int64_t>(0, currentPreviewTick() - visibleTicks / 2);
    }

    void MidiPitchBendLaneComponent::paintGraph(juce::Graphics& g)
    {
        if (graphBounds.isEmpty()) return;

        g.setColour(juce::Colour(0xff151a21));
        g.fillRect(graphBounds);
        g.setColour(juce::Colour(0xff4a5564));
        g.drawRect(graphBounds, 1);

        const auto plot = graphPlotBounds();
        if (plot.isEmpty()) return;

        g.setFont(juce::FontOptions(10.0f));
        for (const int value : { mw::core::midiPitchBendMaximum, 4096, 0, -4096, mw::core::midiPitchBendMinimum })
        {
            const auto y = static_cast<float>(mw::midi::midiPitchBendValueToY(value, plot.getY(), plot.getHeight()));
            g.setColour(value == 0 ? juce::Colour(0xff657284) : juce::Colour(0xff313946));
            g.drawHorizontalLine(static_cast<int>(std::lround(y)), static_cast<float>(plot.getX()), static_cast<float>(plot.getRight()));
            g.setColour(juce::Colour(0xff9eabbc));
            g.drawText(juce::String(value), graphBounds.getX() + 4, static_cast<int>(std::lround(y)) - 8, graphLeftMargin - 8, 16,
                       juce::Justification::centredRight, false);
        }

        ensureGraphViewInitialized();
        const auto viewEnd = graphViewEndTick();
        const auto ppq = static_cast<std::int64_t>(mw::core::Project::ticksPerQuarterNote);
        auto firstGrid = ((graphViewStartTick + ppq - 1) / ppq) * ppq;
        for (auto tick = firstGrid; tick <= viewEnd; tick += ppq)
        {
            const auto x = plot.getX() + static_cast<int>(std::lround(mw::midi::midiPitchBendTickToX(tick, graphViewStartTick, graphTicksPerPixel())));
            if (x < plot.getX() || x > plot.getRight()) continue;
            g.setColour(juce::Colour(0xff2a313c));
            g.drawVerticalLine(x, static_cast<float>(plot.getY()), static_cast<float>(plot.getBottom()));
            g.setColour(juce::Colour(0xff909baa));
            g.drawText(juce::String(static_cast<juce::int64>(tick)), x - 34, plot.getBottom() + 3, 68, 18,
                       juce::Justification::centred, false);
        }

        const auto previewTick = currentPreviewTick();
        if (previewTick >= graphViewStartTick && previewTick <= viewEnd)
        {
            const auto x = plot.getX() + static_cast<int>(std::lround(mw::midi::midiPitchBendTickToX(previewTick, graphViewStartTick, graphTicksPerPixel())));
            g.setColour(juce::Colour(0xffd7a642));
            g.drawVerticalLine(x, static_cast<float>(plot.getY()), static_cast<float>(plot.getBottom()));
        }

        auto* track = findTrack(currentTrackId());
        if (track == nullptr || track->isAudioClipTrack()) return;

        const auto steps = mw::midi::pitchBendStaircaseSegments(*track, selectedChannel(), graphViewStartTick, viewEnd);
        if (!steps.empty())
        {
            juce::Path path;
            bool started = false;
            float previousY = 0.0f;
            for (const auto& step : steps)
            {
                const auto x1 = static_cast<float>(plot.getX() + mw::midi::midiPitchBendTickToX(step.startTick, graphViewStartTick, graphTicksPerPixel()));
                const auto x2 = static_cast<float>(plot.getX() + mw::midi::midiPitchBendTickToX(step.endTick, graphViewStartTick, graphTicksPerPixel()));
                const auto y = static_cast<float>(mw::midi::midiPitchBendValueToY(step.value, plot.getY(), plot.getHeight()));
                if (!started)
                {
                    path.startNewSubPath(x1, y);
                    started = true;
                }
                else
                {
                    path.lineTo(x1, previousY);
                    path.lineTo(x1, y);
                }
                path.lineTo(x2, y);
                previousY = y;
            }
            g.setColour(juce::Colour(0xff6cb7df));
            g.strokePath(path, juce::PathStrokeType(1.5f));
        }

        for (const auto& event : visibleEvents)
        {
            if (event.tick < graphViewStartTick || event.tick > viewEnd) continue;
            if (graphDragActive && event.stableId == graphDragEventId) continue;
            const auto x = static_cast<float>(plot.getX() + mw::midi::midiPitchBendTickToX(event.tick, graphViewStartTick, graphTicksPerPixel()));
            const auto y = static_cast<float>(mw::midi::midiPitchBendValueToY(event.value, plot.getY(), plot.getHeight()));
            g.setColour(event.stableId == selectedEventId ? juce::Colour(0xffffca5f) : juce::Colour(0xff8bd1f1));
            g.fillEllipse(x - 5.0f, y - 5.0f, 10.0f, 10.0f);
            g.setColour(juce::Colour(0xff11161c));
            g.drawEllipse(x - 5.0f, y - 5.0f, 10.0f, 10.0f, 1.0f);
        }

        if (graphDragActive)
        {
            const auto oldX = static_cast<float>(plot.getX() + mw::midi::midiPitchBendTickToX(graphDragOriginalTick, graphViewStartTick, graphTicksPerPixel()));
            const auto oldY = static_cast<float>(mw::midi::midiPitchBendValueToY(graphDragOriginalValue, plot.getY(), plot.getHeight()));
            const auto newX = static_cast<float>(plot.getX() + mw::midi::midiPitchBendTickToX(graphDragPreviewTick, graphViewStartTick, graphTicksPerPixel()));
            const auto newY = static_cast<float>(mw::midi::midiPitchBendValueToY(graphDragPreviewValue, plot.getY(), plot.getHeight()));
            g.setColour(juce::Colour(0x88ffca5f));
            g.drawLine(oldX, oldY, newX, newY, 1.0f);
            g.setColour(juce::Colour(0xffffca5f));
            g.fillEllipse(newX - 6.0f, newY - 6.0f, 12.0f, 12.0f);
            g.drawText("tick " + juce::String(static_cast<juce::int64>(graphDragPreviewTick)) + "  value " + juce::String(graphDragPreviewValue),
                       plot.getX() + 8, plot.getY() + 8, std::max(100, plot.getWidth() - 16), 18,
                       juce::Justification::topRight, false);
        }
    }

    void MidiPitchBendLaneComponent::mouseDown(const juce::MouseEvent& event)
    {
        if (!graphBounds.contains(event.getPosition())) return;
        grabKeyboardFocus();
        ensureGraphViewInitialized();
        const auto plot = graphPlotBounds();
        if (!plot.contains(event.getPosition()))
        {
            selectEventById(0);
            return;
        }

        const auto hit = mw::midi::hitTestMidiPitchBendPoint(visibleEvents, graphViewStartTick, graphTicksPerPixel(),
                                                               plot.getY(), plot.getHeight(),
                                                               event.position.x - static_cast<float>(plot.getX()), event.position.y, 9.0);
        selectEventById(hit);
        if (hit == 0) return;

        const auto* selected = selectedVisibleEvent();
        if (selected == nullptr) return;
        graphDragActive = true;
        graphDragEventId = selected->stableId;
        graphDragOriginalTick = selected->tick;
        graphDragOriginalValue = selected->value;
        graphDragPreviewTick = selected->tick;
        graphDragPreviewValue = selected->value;
        repaint(graphBounds);
    }

    void MidiPitchBendLaneComponent::mouseDrag(const juce::MouseEvent& event)
    {
        if (!graphDragActive) return;
        const auto plot = graphPlotBounds();
        const auto x = std::clamp(event.position.x, static_cast<float>(plot.getX()), static_cast<float>(plot.getRight()));
        const auto y = std::clamp(event.position.y, static_cast<float>(plot.getY()), static_cast<float>(plot.getBottom()));
        const auto rawTick = mw::midi::midiPitchBendXToTick(x - static_cast<float>(plot.getX()), graphViewStartTick, graphTicksPerPixel());
        graphDragPreviewTick = mw::midi::snapMidiPitchBendTick(rawTick, mw::core::Project::ticksPerQuarterNote, selectedSnapGrid());
        graphDragPreviewValue = mw::midi::midiPitchBendYToValue(y, plot.getY(), plot.getHeight());
        valueSlider.setValue(graphDragPreviewValue, juce::dontSendNotification);
        repaint(graphBounds);
    }

    void MidiPitchBendLaneComponent::mouseUp(const juce::MouseEvent&)
    {
        if (!graphDragActive) return;
        const auto eventId = graphDragEventId;
        const auto tick = graphDragPreviewTick;
        const auto value = graphDragPreviewValue;
        graphDragActive = false;
        graphDragEventId = 0;

        auto* project = currentProject();
        if (project == nullptr)
        {
            repaint(graphBounds);
            return;
        }

        selectedEventId = eventId;
        const bool changed = editHistory.moveAndSetValue(*project, currentTrackId(), eventId, tick, value);
        commitChangedEdit(changed,
                          "Pitch Bend point dragged to tick " + juce::String(static_cast<juce::int64>(tick)) + " value " + juce::String(value) + ".");
    }

    void MidiPitchBendLaneComponent::mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel)
    {
        if (!graphBounds.contains(event.getPosition()))
        {
            juce::Component::mouseWheelMove(event, wheel);
            return;
        }
        ensureGraphViewInitialized();
        if (event.mods.isCtrlDown() || event.mods.isCommandDown())
        {
            const auto direction = wheel.deltaY >= 0.0f ? 0.25 : -0.25;
            zoomSlider.setValue(std::clamp(zoomSlider.getValue() + direction, 0.25, 4.0), juce::sendNotificationSync);
            return;
        }

        const auto visibleTicks = std::max<std::int64_t>(1, graphViewEndTick() - graphViewStartTick);
        const auto wheelAmount = std::abs(wheel.deltaX) > std::abs(wheel.deltaY) ? wheel.deltaX : wheel.deltaY;
        const auto shift = std::max<std::int64_t>(1, visibleTicks / 5);
        graphViewStartTick = std::max<std::int64_t>(0, graphViewStartTick - static_cast<std::int64_t>(std::llround(wheelAmount * 5.0f)) * shift);
        repaint(graphBounds);
    }

    bool MidiPitchBendLaneComponent::keyPressed(const juce::KeyPress& key)
    {
        if (graphDragActive && key.getKeyCode() == juce::KeyPress::escapeKey)
        {
            cancelGraphDrag();
            return true;
        }
        return false;
    }

    void MidiPitchBendLaneComponent::cancelGraphDrag()
    {
        if (!graphDragActive) return;
        graphDragActive = false;
        graphDragEventId = 0;
        graphDragPreviewTick = graphDragOriginalTick;
        graphDragPreviewValue = graphDragOriginalValue;
        valueSlider.setValue(graphDragOriginalValue, juce::dontSendNotification);
        repaint(graphBounds);
    }

    int MidiPitchBendLaneComponent::getNumRows()
    {
        return static_cast<int>(visibleEvents.size());
    }

    void MidiPitchBendLaneComponent::paintListBoxItem(int rowNumber, juce::Graphics& g, int width, int height, bool rowIsSelected)
    {
        if (rowNumber < 0 || rowNumber >= static_cast<int>(visibleEvents.size())) return;
        const auto& event = visibleEvents[static_cast<std::size_t>(rowNumber)];
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
        auto row = juce::Rectangle<int>(0, 0, width, height).reduced(8, 0);
        g.setColour(juce::Colour(0xffe1e7ef));
        g.setFont(juce::FontOptions(13.0f));
        g.drawText(eventTickText(event), row.removeFromLeft(190), juce::Justification::centredLeft, true);
        g.drawText(juce::String(event.value), row.removeFromLeft(120), juce::Justification::centredLeft, true);
    }

    void MidiPitchBendLaneComponent::selectedRowsChanged(int lastRowSelected)
    {
        if (lastRowSelected >= 0 && lastRowSelected < static_cast<int>(visibleEvents.size()))
        {
            const auto& event = visibleEvents[static_cast<std::size_t>(lastRowSelected)];
            selectedEventId = event.stableId;
            valueSlider.setValue(event.value, juce::dontSendNotification);
        }
        else
        {
            selectedEventId = 0;
        }
        refreshEnabledState();
        repaint(graphBounds);
    }

    void MidiPitchBendLaneComponent::selectEventById(mw::core::StableId eventId)
    {
        selectedEventId = eventId;
        int selectedRow = -1;
        for (int row = 0; row < static_cast<int>(visibleEvents.size()); ++row)
        {
            if (visibleEvents[static_cast<std::size_t>(row)].stableId == selectedEventId)
            {
                selectedRow = row;
                valueSlider.setValue(visibleEvents[static_cast<std::size_t>(row)].value, juce::dontSendNotification);
                break;
            }
        }
        if (selectedRow >= 0) eventList.selectRow(selectedRow, false, true);
        else eventList.deselectAllRows();
        refreshEnabledState();
        repaint(graphBounds);
    }

    const mw::core::MidiPitchBendEvent* MidiPitchBendLaneComponent::selectedVisibleEvent() const
    {
        const auto found = std::find_if(visibleEvents.begin(), visibleEvents.end(), [&](const auto& event)
        {
            return event.stableId == selectedEventId;
        });
        return found != visibleEvents.end() ? &(*found) : nullptr;
    }

    void MidiPitchBendLaneComponent::timerCallback()
    {
        refreshNow();
    }

    mw::core::Project* MidiPitchBendLaneComponent::currentProject() const
    {
        return callbacks.project ? callbacks.project() : nullptr;
    }

    mw::core::Track* MidiPitchBendLaneComponent::findTrack(mw::core::StableId trackId) const
    {
        auto* project = currentProject();
        if (project == nullptr || trackId == 0) return nullptr;
        for (auto& track : project->getTracks())
            if (track.getStableId() == trackId) return &track;
        return nullptr;
    }

    mw::core::StableId MidiPitchBendLaneComponent::currentTrackId() const
    {
        return callbacks.selectedTrackId ? callbacks.selectedTrackId() : 0;
    }

    int MidiPitchBendLaneComponent::selectedChannel() const
    {
        return std::clamp(channelCombo.getSelectedId(), 1, 16);
    }

    int MidiPitchBendLaneComponent::selectedValue() const
    {
        return std::clamp(static_cast<int>(std::lround(valueSlider.getValue())), mw::core::midiPitchBendMinimum, mw::core::midiPitchBendMaximum);
    }

    std::int64_t MidiPitchBendLaneComponent::currentPreviewTick() const
    {
        return callbacks.previewTick ? std::max<std::int64_t>(0, callbacks.previewTick()) : 0;
    }

    void MidiPitchBendLaneComponent::refreshNow()
    {
        auto* project = currentProject();
        const auto projectId = project != nullptr ? project->getStableId() : 0;
        if (project != lastProjectPointer || projectId != lastProjectId)
        {
            editHistory.clear();
            selectedEventId = 0;
            graphViewInitialized = false;
            cancelGraphDrag();
            lastProjectPointer = project;
            lastProjectId = projectId;
        }
        refreshLane(true);
    }

    void MidiPitchBendLaneComponent::refreshLane(bool preserveSelection)
    {
        if (!preserveSelection) selectedEventId = 0;

        const auto trackId = currentTrackId();
        auto* track = findTrack(trackId);
        visibleEvents.clear();
        if (graphTrackId != trackId)
        {
            graphViewInitialized = false;
            cancelGraphDrag();
        }

        if (track == nullptr)
        {
            trackLabel.setText("Track: No selected track", juce::dontSendNotification);
            statusLabel.setText("Select a MIDI track to edit Pitch Bend events.", juce::dontSendNotification);
            selectedEventId = 0;
        }
        else if (track->isAudioClipTrack())
        {
            trackLabel.setText("Track: " + juce::String(track->getName()), juce::dontSendNotification);
            statusLabel.setText("Pitch Bend Lane is unavailable for AudioClip tracks.", juce::dontSendNotification);
            selectedEventId = 0;
        }
        else
        {
            trackLabel.setText("Track: " + juce::String(track->getName()), juce::dontSendNotification);
            statusLabel.setText("Preview tick: " + juce::String(static_cast<juce::int64>(currentPreviewTick()))
                + "  |  Signed range -8192..+8191; 0 = center.", juce::dontSendNotification);
            statusLabel.setTooltip(mw::midi::midiPitchBendLaneTooltip);
            visibleEvents = mw::midi::pitchBendLaneEvents(*track, selectedChannel());
            if (selectedEventId != 0)
            {
                const auto selectedStillVisible = std::any_of(visibleEvents.begin(), visibleEvents.end(), [&](const auto& event)
                {
                    return event.stableId == selectedEventId;
                });
                if (!selectedStillVisible)
                    selectedEventId = 0;
            }
        }

        eventList.updateContent();
        int selectedRow = -1;
        for (int row = 0; row < static_cast<int>(visibleEvents.size()); ++row)
            if (visibleEvents[static_cast<std::size_t>(row)].stableId == selectedEventId) selectedRow = row;
        if (selectedRow >= 0) eventList.selectRow(selectedRow, false, true);
        else eventList.deselectAllRows();

        ensureGraphViewInitialized();
        refreshEnabledState();
        eventList.repaint();
        repaint(graphBounds);
    }

    void MidiPitchBendLaneComponent::refreshEnabledState()
    {
        auto* track = findTrack(currentTrackId());
        const bool editableTrack = track != nullptr && !track->isAudioClipTrack();
        const bool hasSelection = editableTrack && selectedEventId != 0;
        channelCombo.setEnabled(editableTrack);
        valueSlider.setEnabled(editableTrack);
        snapCombo.setEnabled(editableTrack);
        zoomSlider.setEnabled(editableTrack);
        eventList.setEnabled(editableTrack);
        addButton.setEnabled(editableTrack);
        moveButton.setEnabled(hasSelection);
        setValueButton.setEnabled(hasSelection);
        deleteButton.setEnabled(hasSelection);
        undoButton.setEnabled(editHistory.canUndo());
        redoButton.setEnabled(editHistory.canRedo());
    }

    void MidiPitchBendLaneComponent::performAdd()
    {
        auto* project = currentProject();
        if (project == nullptr) return;
        const auto result = editHistory.addAtTick(*project, currentTrackId(), currentPreviewTick(), selectedChannel(), selectedValue());
        if (result.changed) selectedEventId = result.eventId;
        commitChangedEdit(result.changed,
                          result.replacedExisting ? "Pitch Bend value replaced at Preview tick." : "Pitch Bend event added at Preview tick.");
    }

    void MidiPitchBendLaneComponent::performMove()
    {
        auto* project = currentProject();
        if (project == nullptr) return;
        const bool changed = editHistory.moveToTick(*project, currentTrackId(), selectedEventId, currentPreviewTick());
        commitChangedEdit(changed, "Pitch Bend event moved to Preview tick.");
    }

    void MidiPitchBendLaneComponent::performSetValue()
    {
        auto* project = currentProject();
        if (project == nullptr) return;
        const bool changed = editHistory.setValue(*project, currentTrackId(), selectedEventId, selectedValue());
        commitChangedEdit(changed, "Pitch Bend event value updated.");
    }

    void MidiPitchBendLaneComponent::performDelete()
    {
        auto* project = currentProject();
        if (project == nullptr) return;
        const bool changed = editHistory.deleteEvent(*project, currentTrackId(), selectedEventId);
        if (changed) selectedEventId = 0;
        commitChangedEdit(changed, "Pitch Bend event deleted.");
    }

    void MidiPitchBendLaneComponent::performUndo()
    {
        auto* project = currentProject();
        if (project == nullptr) return;
        cancelGraphDrag();
        commitChangedEdit(editHistory.undo(*project), "Undo Bend applied.");
    }

    void MidiPitchBendLaneComponent::performRedo()
    {
        auto* project = currentProject();
        if (project == nullptr) return;
        cancelGraphDrag();
        commitChangedEdit(editHistory.redo(*project), "Redo Bend applied.");
    }

    void MidiPitchBendLaneComponent::commitChangedEdit(bool changed, const juce::String& message)
    {
        if (changed)
        {
            if (callbacks.markProjectDirty)
                callbacks.markProjectDirty(editHistory.lastAffectedTrackId() != 0 ? editHistory.lastAffectedTrackId() : currentTrackId());
            log(message);
        }
        refreshLane(true);
    }

    void MidiPitchBendLaneComponent::log(const juce::String& message) const
    {
        if (callbacks.log) callbacks.log(message);
    }
}
