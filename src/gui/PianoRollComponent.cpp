#include "gui/PianoRollComponent.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace mw::gui
{
    namespace
    {
        constexpr int defaultVisiblePitchRows = 25;

        int clampPitchWindowLow(int requestedLow, int visibleRows)
        {
            const auto rows = std::clamp(visibleRows, 1, 128);
            return std::clamp(requestedLow, 0, 128 - rows);
        }
    }

    static juce::String formatTimelineTime(double beat, int tempoBpm, bool showTenths)
    {
        const auto safeTempo = std::max(1, tempoBpm);
        const auto totalSeconds = std::max(0.0, beat * 60.0 / static_cast<double>(safeTempo));
        const int minutes = static_cast<int>(std::floor(totalSeconds / 60.0));
        const int seconds = static_cast<int>(std::floor(totalSeconds)) % 60;

        if (showTenths)
        {
            const int tenths = static_cast<int>(std::floor((totalSeconds - std::floor(totalSeconds)) * 10.0));
            return juce::String::formatted("%02d:%02d.%d", minutes, seconds, tenths);
        }

        return juce::String::formatted("%02d:%02d", minutes, seconds);
    }

    PianoRollComponent::PianoRollComponent()
    {
        setWantsKeyboardFocus(true);
        setOpaque(true);
        addAndMakeVisible(pitchScrollBar);
        pitchScrollBar.setColour(juce::ScrollBar::trackColourId, juce::Colour(0xff2d2d2d));
        pitchScrollBar.setColour(juce::ScrollBar::thumbColourId, juce::Colour(0xffb8b8b8));
        pitchScrollBar.setColour(juce::ScrollBar::backgroundColourId, juce::Colour(0xff202020));

        pitchScrollBar.addListener(this);
    }


    juce::Rectangle<float> PianoRollComponent::getGridBounds() const
    {
        return getLocalBounds().toFloat()
            .withTrimmedLeft(static_cast<float>(keyLabelWidth))
            .withTrimmedTop(static_cast<float>(rulerHeight))
            .withTrimmedRight(static_cast<float>(pitchScrollBarWidth + 4));
    }

    juce::String PianoRollComponent::pitchToName(int pitch)
    {
        pitch = std::clamp(pitch, 0, 127);
        return pitchToNoteName(pitch) + juce::String(pitchToOctave(pitch));
    }

    juce::String PianoRollComponent::pitchToNoteName(int pitch)
    {
        static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        pitch = std::clamp(pitch, 0, 127);
        return juce::String(names[pitch % 12]);
    }

    int PianoRollComponent::pitchToOctave(int pitch)
    {
        pitch = std::clamp(pitch, 0, 127);
        return (pitch / 12) - 1;
    }

    bool PianoRollComponent::isAccidentalPitch(int pitch)
    {
        const int semitone = std::clamp(pitch, 0, 127) % 12;
        return semitone == 1 || semitone == 3 || semitone == 6 || semitone == 8 || semitone == 10;
    }

    int PianoRollComponent::noteNameToSemitone(const juce::String& noteName)
    {
        auto text = noteName.trim().toUpperCase();

        if (text.startsWith("DB")) return 1;
        if (text.startsWith("EB")) return 3;
        if (text.startsWith("GB")) return 6;
        if (text.startsWith("AB")) return 8;
        if (text.startsWith("BB")) return 10;

        if (text.startsWith("C#")) return 1;
        if (text.startsWith("D#")) return 3;
        if (text.startsWith("F#")) return 6;
        if (text.startsWith("G#")) return 8;
        if (text.startsWith("A#")) return 10;

        if (text.startsWith("C")) return 0;
        if (text.startsWith("D")) return 2;
        if (text.startsWith("E")) return 4;
        if (text.startsWith("F")) return 5;
        if (text.startsWith("G")) return 7;
        if (text.startsWith("A")) return 9;
        if (text.startsWith("B")) return 11;

        return -1;
    }

    void PianoRollComponent::validatePitchWindow()
    {
        visiblePitchRows = std::clamp(visiblePitchRows, 1, defaultVisiblePitchRows);

        const auto maxStart = std::max(0.0, 128.0 - static_cast<double>(visiblePitchRows));
        pitchScrollStart = std::clamp(pitchScrollStart, 0.0, maxStart);

        lowPitch = std::clamp(static_cast<int>(std::floor(pitchScrollStart)), 0, 127);
        highPitch = std::clamp(static_cast<int>(std::ceil(pitchScrollStart + static_cast<double>(visiblePitchRows))) - 1, lowPitch, 127);
    }

    void PianoRollComponent::updatePitchScrollBar()
    {
        validatePitchWindow();
        const auto visibleCount = std::clamp(visiblePitchRows, 1, 128);
        pitchScrollBar.setRangeLimits(0.0, 128.0);
        pitchScrollBar.setCurrentRange(pitchScrollStart, static_cast<double>(visibleCount), juce::dontSendNotification);
        pitchScrollBar.setSingleStepSize(0.125);
    }

    void PianoRollComponent::setPitchScrollStart(double pitchStart)
    {
        visiblePitchRows = std::clamp(visiblePitchRows, 1, defaultVisiblePitchRows);

        const auto maxStart = std::max(0.0, 128.0 - static_cast<double>(visiblePitchRows));
        const auto clampedStart = std::clamp(pitchStart, 0.0, maxStart);

        pitchScrollStart = clampedStart;
        validatePitchWindow();
        updatePitchScrollBar();

        if (onPitchRangeChanged)
            onPitchRangeChanged();

        repaint();
    }

    void PianoRollComponent::setVisiblePitchStart(int pitchStart)
    {
        setPitchScrollStart(static_cast<double>(pitchStart));
    }

    juce::String PianoRollComponent::getVisiblePitchRangeText() const
    {
        return pitchToName(lowPitch) + " - " + pitchToName(highPitch);
    }

    bool PianoRollComponent::jumpToKey(const juce::String& keyText)
    {
        auto text = keyText.trim();

        if (text.isEmpty())
            return false;

        int targetPitch = -1;

        if (text.containsOnly("0123456789"))
        {
            targetPitch = text.getIntValue();
        }
        else
        {
            const auto semitone = noteNameToSemitone(text);

            if (semitone < 0)
                return false;

            juce::String octaveText;

            for (int i = 0; i < text.length(); ++i)
            {
                const auto ch = text[i];

                if (ch >= '0' && ch <= '9')
                    octaveText += juce::String::charToString(ch);
            }

            if (octaveText.isNotEmpty())
            {
                const int octave = octaveText.getIntValue();
                targetPitch = (octave + 1) * 12 + semitone;
            }
            else
            {
                // Default to the octave closest to the current visible range.
                const int currentOctave = std::max(0, ((lowPitch / 12) - 1));
                targetPitch = (currentOctave + 1) * 12 + semitone;
            }
        }

        if (targetPitch < 0 || targetPitch > 127)
            return false;

        validatePitchWindow();
        setVisiblePitchStart(targetPitch - (visiblePitchRows / 2));
        return true;
    }

    void PianoRollComponent::nudgeVisiblePitchRange(int semitones)
    {
        if (semitones == 0)
            return;

        setVisiblePitchStart(lowPitch + semitones);
    }

    void PianoRollComponent::resized()
    {
        auto area = getLocalBounds();
        area.removeFromTop(rulerHeight);
        pitchScrollBar.setBounds(area.removeFromRight(pitchScrollBarWidth).reduced(1, 2));
        updatePitchScrollBar();
    }

    void PianoRollComponent::mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& wheel)
    {
        if (std::abs(wheel.deltaY) <= 0.0f)
            return;

        setPitchScrollStart(pitchScrollStart + static_cast<double>(wheel.deltaY) * 2.5);
    }

    void PianoRollComponent::scrollBarMoved(juce::ScrollBar* scrollBarThatHasMoved, double newRangeStart)
    {
        if (scrollBarThatHasMoved != &pitchScrollBar)
            return;

        visiblePitchRows = std::clamp(visiblePitchRows, 1, defaultVisiblePitchRows);
        const auto maxStart = std::max(0.0, 128.0 - static_cast<double>(visiblePitchRows));
        pitchScrollStart = std::clamp(newRangeStart, 0.0, maxStart);

        validatePitchWindow();

        if (onPitchRangeChanged)
            onPitchRangeChanged();

        repaint();
    }


    void PianoRollComponent::setNotes(const std::vector<mw::core::NoteEvent>& notesToShow)
    {
        notes = notesToShow;
        undoStack.clear();
        redoStack.clear();

        if (selectedNoteIndex >= static_cast<int>(notes.size()))
            selectedNoteIndex = -1;

        selectedNoteIndices.erase(
            std::remove_if(
                selectedNoteIndices.begin(),
                selectedNoteIndices.end(),
                [this](int index)
                {
                    return index < 0 || index >= static_cast<int>(notes.size());
                }
            ),
            selectedNoteIndices.end()
        );

        if (selectedNoteIndices.empty() && selectedNoteIndex >= 0)
            selectedNoteIndices.push_back(selectedNoteIndex);

        repaint();
    }

    void PianoRollComponent::setNotesPreservingHistory(const std::vector<mw::core::NoteEvent>& notesToShow)
    {
        notes = notesToShow;

        if (selectedNoteIndex >= static_cast<int>(notes.size()))
            selectedNoteIndex = -1;

        selectedNoteIndices.erase(
            std::remove_if(
                selectedNoteIndices.begin(),
                selectedNoteIndices.end(),
                [this](int index)
                {
                    return index < 0 || index >= static_cast<int>(notes.size());
                }
            ),
            selectedNoteIndices.end()
        );

        if (selectedNoteIndices.empty() && selectedNoteIndex >= 0)
            selectedNoteIndices.push_back(selectedNoteIndex);

        repaint();
    }

    namespace
    {
        std::int64_t getSnapTickLength(int snapDivision)
        {
            const auto division = std::max(1, snapDivision);
            return std::max<std::int64_t>(1, mw::core::Project::ticksPerQuarterNote / division);
        }
    }

    void PianoRollComponent::setGrid(int visibleBeats, int lowestPitch, int highestPitch)
    {
        beatsVisible = std::max(1, visibleBeats);

        const auto requestedLow = std::clamp(lowestPitch, 0, 127);
        const auto requestedHigh = std::clamp(highestPitch, requestedLow, 127);
        const auto requestedRows = requestedHigh - requestedLow + 1;
        visiblePitchRows = std::clamp(requestedRows, 1, defaultVisiblePitchRows);

        auto windowLow = requestedLow;

        if (requestedRows > defaultVisiblePitchRows)
        {
            const auto requestedCentre = (requestedLow + requestedHigh) / 2;
            windowLow = requestedCentre - (visiblePitchRows / 2);
        }

        lowPitch = clampPitchWindowLow(windowLow, visiblePitchRows);
        pitchScrollStart = static_cast<double>(lowPitch);
        highPitch = std::clamp(lowPitch + visiblePitchRows - 1, lowPitch, 127);

        updatePitchScrollBar();

        if (onPitchRangeChanged)
            onPitchRangeChanged();

        repaint();
    }

    void PianoRollComponent::setDefaultNoteSettings(double lengthBeats, int velocity)
    {
        const auto minimumBeats = 1.0 / static_cast<double>(std::max(1, snapDivision));
        defaultNoteLengthBeats = std::max(minimumBeats, lengthBeats);
        defaultVelocity = std::clamp(velocity, 1, 127);
    }

    void PianoRollComponent::setSnapDivision(int snapDivisionValue)
    {
        if (snapDivisionValue != 1
            && snapDivisionValue != 2
            && snapDivisionValue != 4
            && snapDivisionValue != 8
            && snapDivisionValue != 16
            && snapDivisionValue != 32
            && snapDivisionValue != 64)
        {
            snapDivisionValue = 1;
        }

        snapDivision = snapDivisionValue;
        repaint();
    }

    void PianoRollComponent::setStartBeat(int startBeatValue)
    {
        startBeat = std::max(0, startBeatValue);
        repaint();
    }

    void PianoRollComponent::setTempoBpm(int tempoBpmValue)
    {
        tempoBpm = std::clamp(tempoBpmValue, 1, 400);
        repaint();
    }

    void PianoRollComponent::trimUndoHistory()
    {
        while (undoStack.size() > maxUndoSteps)
            undoStack.erase(undoStack.begin());
    }

    void PianoRollComponent::captureUndoState()
    {
        undoStack.push_back(notes);
        trimUndoHistory();
        redoStack.clear();
    }

    void PianoRollComponent::applyHistoryState(const std::vector<mw::core::NoteEvent>& state)
    {
        notes = state;
        clearSelection();

        if (onNotesChanged)
            onNotesChanged(notes);

        if (onSelectedNoteChanged)
            onSelectedNoteChanged(selectedNoteIndex);

        repaint();
    }

    void PianoRollComponent::undoLastNoteEdit()
    {
        if (undoStack.empty())
            return;

        redoStack.push_back(notes);
        const auto previous = undoStack.back();
        undoStack.pop_back();
        applyHistoryState(previous);
    }

    void PianoRollComponent::redoLastNoteEdit()
    {
        if (redoStack.empty())
            return;

        undoStack.push_back(notes);
        trimUndoHistory();

        const auto next = redoStack.back();
        redoStack.pop_back();
        applyHistoryState(next);
    }

    void PianoRollComponent::clearNoteSelection()
    {
        clearSelection();

        if (onSelectedNoteChanged)
            onSelectedNoteChanged(selectedNoteIndex);

        repaint();
    }

    void PianoRollComponent::deleteSelectedNote()
    {
        if (selectedNoteIndices.empty() && selectedNoteIndex >= 0)
            selectedNoteIndices.push_back(selectedNoteIndex);

        if (selectedNoteIndices.empty())
            return;

        captureUndoState();

        std::sort(selectedNoteIndices.begin(), selectedNoteIndices.end());
        selectedNoteIndices.erase(std::unique(selectedNoteIndices.begin(), selectedNoteIndices.end()), selectedNoteIndices.end());

        for (auto it = selectedNoteIndices.rbegin(); it != selectedNoteIndices.rend(); ++it)
        {
            if (*it >= 0 && *it < static_cast<int>(notes.size()))
                notes.erase(notes.begin() + *it);
        }

        selectedNoteIndices.clear();
        selectedNoteIndex = -1;

        if (onNotesChanged)
            onNotesChanged(notes);

        if (onSelectedNoteChanged)
            onSelectedNoteChanged(selectedNoteIndex);

        repaint();
    }

    void PianoRollComponent::copySelectedNote()
    {
        copiedNotes.clear();

        if (selectedNoteIndices.empty() && selectedNoteIndex >= 0)
            selectedNoteIndices.push_back(selectedNoteIndex);

        if (selectedNoteIndices.empty())
            return;

        auto indices = selectedNoteIndices;
        std::sort(indices.begin(), indices.end());
        indices.erase(std::unique(indices.begin(), indices.end()), indices.end());

        for (int index : indices)
        {
            if (index >= 0 && index < static_cast<int>(notes.size()))
                copiedNotes.push_back(notes[static_cast<std::size_t>(index)]);
        }

        hasCopiedNote = !copiedNotes.empty();
        copiedNote = copiedNotes.empty() ? mw::core::NoteEvent {} : copiedNotes.front();
    }

    void PianoRollComponent::pasteCopiedNote()
    {
        if (!hasCopiedNote)
            return;

        captureUndoState();

        if (copiedNotes.empty())
            copiedNotes.push_back(copiedNote);

        selectedNoteIndices.clear();

        for (auto note : copiedNotes)
        {
            note.startTick += mw::core::Project::ticksPerQuarterNote;
            notes.push_back(note);
            selectedNoteIndices.push_back(static_cast<int>(notes.size()) - 1);
        }

        selectedNoteIndex = selectedNoteIndices.empty() ? -1 : selectedNoteIndices.back();

        copiedNotes.clear();
        for (int index : selectedNoteIndices)
            copiedNotes.push_back(notes[static_cast<std::size_t>(index)]);

        copiedNote = copiedNotes.empty() ? mw::core::NoteEvent {} : copiedNotes.front();
        hasCopiedNote = !copiedNotes.empty();

        if (onNotesChanged)
            onNotesChanged(notes);

        if (onSelectedNoteChanged)
            onSelectedNoteChanged(selectedNoteIndex);

        repaint();
    }

    std::int64_t PianoRollComponent::xToSnapTick(float x) const
    {
        const auto grid = getGridBounds();
        const float beatWidth = grid.getWidth() / static_cast<float>(beatsVisible);

        if (beatWidth <= 0.0f)
            return static_cast<std::int64_t>(startBeat) * mw::core::Project::ticksPerQuarterNote;

        const double beatPosition =
            static_cast<double>(startBeat)
            + static_cast<double>(x - grid.getX()) / static_cast<double>(beatWidth);

        const double snappedBeat =
            std::round(beatPosition * static_cast<double>(snapDivision))
            / static_cast<double>(snapDivision);

        return std::max<std::int64_t>(
            0,
            static_cast<std::int64_t>(
                std::llround(snappedBeat * static_cast<double>(mw::core::Project::ticksPerQuarterNote))
            )
        );
    }

    std::int64_t PianoRollComponent::xToPaintSnapTick(float x) const
    {
        const auto grid = getGridBounds();
        const float beatWidth = grid.getWidth() / static_cast<float>(beatsVisible);

        if (beatWidth <= 0.0f)
            return static_cast<std::int64_t>(startBeat) * mw::core::Project::ticksPerQuarterNote;

        const double beatPosition =
            static_cast<double>(startBeat)
            + static_cast<double>(x - grid.getX()) / static_cast<double>(beatWidth);

        const double snappedBeat =
            std::floor(beatPosition * static_cast<double>(snapDivision))
            / static_cast<double>(snapDivision);

        return std::max<std::int64_t>(
            0,
            static_cast<std::int64_t>(
                std::llround(snappedBeat * static_cast<double>(mw::core::Project::ticksPerQuarterNote))
            )
        );
    }

    int PianoRollComponent::yToPitch(float y) const
    {
        const auto grid = getGridBounds();
        const int pitchCount = std::clamp(visiblePitchRows, 1, defaultVisiblePitchRows);
        const float rowHeight = grid.getHeight() / static_cast<float>(pitchCount);

        if (rowHeight <= 0.0f)
            return lowPitch;

        const auto rowIndex = std::clamp(
            static_cast<int>(std::floor((y - grid.getY()) / rowHeight)),
            0,
            pitchCount - 1
        );

        const auto topVisiblePitch = pitchScrollStart + static_cast<double>(visiblePitchRows);
        const auto pitch = static_cast<int>(std::floor(topVisiblePitch - 1.0 - static_cast<double>(rowIndex)));
        return std::clamp(pitch, 0, 127);
    }

    juce::Rectangle<float> PianoRollComponent::noteToRectangle(const mw::core::NoteEvent& note) const
    {
        const auto grid = getGridBounds();

        const int pitchCount = std::clamp(visiblePitchRows, 1, defaultVisiblePitchRows);
        const float rowHeight = grid.getHeight() / static_cast<float>(pitchCount);
        const float beatWidth = grid.getWidth() / static_cast<float>(beatsVisible);

        const float noteStartBeat = static_cast<float>(note.startTick) / static_cast<float>(mw::core::Project::ticksPerQuarterNote);
        const float localStartBeat = noteStartBeat - static_cast<float>(startBeat);
        const float durationBeats = std::max(
            0.25f,
            static_cast<float>(note.durationTicks) / static_cast<float>(mw::core::Project::ticksPerQuarterNote)
        );

        const auto topVisiblePitch = pitchScrollStart + static_cast<double>(visiblePitchRows);
        const auto pitchRow = topVisiblePitch - static_cast<double>(note.pitch + 1);

        return {
            grid.getX() + localStartBeat * beatWidth,
            grid.getY() + static_cast<float>(pitchRow) * rowHeight,
            durationBeats * beatWidth,
            std::max(2.0f, rowHeight)
        };
    }

    int PianoRollComponent::findNoteAt(juce::Point<float> point) const
    {
        for (int i = static_cast<int>(notes.size()) - 1; i >= 0; --i)
        {
            if (noteToRectangle(notes[static_cast<std::size_t>(i)]).contains(point))
                return i;
        }

        return -1;
    }


    bool PianoRollComponent::isNoteSelected(int index) const
    {
        return std::find(selectedNoteIndices.begin(), selectedNoteIndices.end(), index) != selectedNoteIndices.end();
    }

    void PianoRollComponent::clearSelection()
    {
        selectedNoteIndices.clear();
        selectedNoteIndex = -1;
    }

    void PianoRollComponent::setSingleSelection(int index)
    {
        selectedNoteIndices.clear();

        if (index >= 0 && index < static_cast<int>(notes.size()))
        {
            selectedNoteIndices.push_back(index);
            selectedNoteIndex = index;
        }
        else
        {
            selectedNoteIndex = -1;
        }
    }

    void PianoRollComponent::toggleNoteSelection(int index)
    {
        if (index < 0 || index >= static_cast<int>(notes.size()))
            return;

        const auto it = std::find(selectedNoteIndices.begin(), selectedNoteIndices.end(), index);

        if (it != selectedNoteIndices.end())
            selectedNoteIndices.erase(it);
        else
            selectedNoteIndices.push_back(index);

        selectedNoteIndex = selectedNoteIndices.empty() ? -1 : selectedNoteIndices.back();
    }

    juce::Rectangle<float> PianoRollComponent::getMarqueeRectangle() const
    {
        const auto x = std::min(marqueeStart.x, marqueeEnd.x);
        const auto y = std::min(marqueeStart.y, marqueeEnd.y);
        const auto right = std::max(marqueeStart.x, marqueeEnd.x);
        const auto bottom = std::max(marqueeStart.y, marqueeEnd.y);

        return { x, y, right - x, bottom - y };
    }

    void PianoRollComponent::selectNotesInRectangle(juce::Rectangle<float> rectangle)
    {
        selectedNoteIndices.clear();

        for (int i = 0; i < static_cast<int>(notes.size()); ++i)
        {
            if (rectangle.intersects(noteToRectangle(notes[static_cast<std::size_t>(i)])))
                selectedNoteIndices.push_back(i);
        }

        selectedNoteIndex = selectedNoteIndices.empty() ? -1 : selectedNoteIndices.back();
    }

    bool PianoRollComponent::isNearRightEdge(int noteIndex, juce::Point<float> point) const
    {
        if (noteIndex < 0 || noteIndex >= static_cast<int>(notes.size()))
            return false;

        const auto r = noteToRectangle(notes[static_cast<std::size_t>(noteIndex)]);
        return std::abs(point.x - r.getRight()) <= 8.0f && r.contains(point);
    }

    bool PianoRollComponent::hasNoteAt(int pitch, std::int64_t startTick) const
    {
        for (const auto& note : notes)
        {
            if (note.pitch == pitch && note.startTick == startTick)
                return true;
        }

        return false;
    }

    bool PianoRollComponent::addPaintNoteAt(juce::Point<float> point)
    {
        const auto grid = getGridBounds();

        if (!grid.contains(point))
            return false;

        const auto startTick = xToPaintSnapTick(point.x);
        const int pitch = yToPitch(point.y);

        if (startTick == lastPaintTick && pitch == lastPaintPitch)
            return false;

        lastPaintTick = startTick;
        lastPaintPitch = pitch;

        if (hasNoteAt(pitch, startTick))
            return false;

        if (!undoCapturedForGesture)
        {
            captureUndoState();
            undoCapturedForGesture = true;
        }

        const auto durationTicks = std::max<std::int64_t>(
            getSnapTickLength(snapDivision),
            static_cast<std::int64_t>(std::llround(defaultNoteLengthBeats * mw::core::Project::ticksPerQuarterNote))
        );

        notes.emplace_back(
            pitch,
            defaultVelocity,
            startTick,
            durationTicks,
            1,
            mw::core::Articulation::Normal
        );

        selectedNoteIndex = static_cast<int>(notes.size()) - 1;
        setSingleSelection(selectedNoteIndex);

        if (onNotesChanged)
            onNotesChanged(notes);

        if (onSelectedNoteChanged)
            onSelectedNoteChanged(selectedNoteIndex);

        repaint();
        return true;
    }


    void PianoRollComponent::startPreviewPlayhead(double tempoBpm, double durationBeats, double startBeatValue)
    {
        previewPlayheadTempoBpm = tempoBpm > 0.0 ? tempoBpm : 120.0;
        previewPlayheadDurationBeats = std::max(0.01, durationBeats);
        previewPlayheadStartBeat = std::max(0.0, startBeatValue);
        previewPlayheadBeat = previewPlayheadStartBeat;
        previewPlayheadStartMilliseconds = juce::Time::getMillisecondCounterHiRes();
        lastPreviewPlayheadPage = -1;
        previewPlayheadPaused = false;
        previewPlayheadActive = true;

        startTimerHz(30);
        repaint();
    }

    void PianoRollComponent::stopPreviewPlayhead()
    {
        previewPlayheadActive = false;
        previewPlayheadPaused = false;
        previewPlayheadBeat = previewPlayheadStartBeat;
        lastPreviewPlayheadPage = -1;
        stopTimer();
        repaint();
    }

    void PianoRollComponent::pausePreviewPlayhead()
    {
        if (!previewPlayheadActive)
            return;

        previewPlayheadPaused = true;
        stopTimer();
        repaint();
    }

    void PianoRollComponent::resumePreviewPlayhead()
    {
        if (!previewPlayheadActive)
            return;

        previewPlayheadPaused = false;

        const auto elapsedBeats = std::max(0.0, previewPlayheadBeat - previewPlayheadStartBeat);
        const auto elapsedSeconds = elapsedBeats / (previewPlayheadTempoBpm / 60.0);

        previewPlayheadStartMilliseconds =
            juce::Time::getMillisecondCounterHiRes()
            - (elapsedSeconds * 1000.0);

        startTimerHz(30);
        repaint();
    }

    void PianoRollComponent::timerCallback()
    {
        if (!previewPlayheadActive)
            return;

        const auto elapsedMilliseconds =
            juce::Time::getMillisecondCounterHiRes()
            - previewPlayheadStartMilliseconds;

        const auto elapsedSeconds = elapsedMilliseconds / 1000.0;
        const auto elapsedBeats = elapsedSeconds * (previewPlayheadTempoBpm / 60.0);

        previewPlayheadBeat = previewPlayheadStartBeat + elapsedBeats;

        if (beatsVisible > 0)
        {
            const int pageStartBeat =
                static_cast<int>(std::floor(previewPlayheadBeat / static_cast<double>(beatsVisible)))
                * beatsVisible;

            if (pageStartBeat != lastPreviewPlayheadPage)
            {
                lastPreviewPlayheadPage = pageStartBeat;

                if (onPreviewPlayheadPageChanged)
                    onPreviewPlayheadPageChanged(pageStartBeat);
            }
        }

        if (elapsedBeats >= previewPlayheadDurationBeats)
        {
            previewPlayheadActive = false;
            previewPlayheadPaused = false;
            stopTimer();
        }

        repaint();
    }

    void PianoRollComponent::paint(juce::Graphics& g)
    {
        validatePitchWindow();
        g.fillAll(juce::Colour(0xff151515));

        const auto bounds = getLocalBounds().toFloat();
        const auto grid = getGridBounds();

        g.setColour(juce::Colour(0xff222222));
        g.fillRect(grid);

        const int pitchCount = std::clamp(visiblePitchRows, 1, defaultVisiblePitchRows);
        const float rowHeight = grid.getHeight() / static_cast<float>(pitchCount);
        const float beatWidth = grid.getWidth() / static_cast<float>(beatsVisible);

        const int timeRulerHeight = rulerHeight / 2;
        const int beatRulerTop = timeRulerHeight;
        const bool showTenths = beatWidth >= 72.0f;
        const int markerStep = beatWidth >= 72.0f ? 1 : (beatWidth >= 38.0f ? 2 : (beatWidth >= 22.0f ? 4 : 8));

        g.setColour(juce::Colour(0xffaaaaaa));
        g.drawText("Time", 4, 1, keyLabelWidth - 8, timeRulerHeight - 2, juce::Justification::centredLeft);
        g.drawText("Beat", 4, beatRulerTop, keyLabelWidth - 8, rulerHeight - beatRulerTop - 2, juce::Justification::centredLeft);

        for (int beat = 0; beat <= beatsVisible; ++beat)
        {
            const float x = grid.getX() + static_cast<float>(beat) * beatWidth;
            g.setColour(beat % 4 == 0 ? juce::Colour(0xff5a5a5a) : juce::Colour(0xff383838));
            g.drawVerticalLine(static_cast<int>(x), grid.getY(), grid.getBottom());
            g.drawVerticalLine(static_cast<int>(x), 0.0f, static_cast<float>(rulerHeight));

            if (snapDivision > 1 && beat < beatsVisible)
            {
                for (int sub = 1; sub < snapDivision; ++sub)
                {
                    const float subX = x + (static_cast<float>(sub) / static_cast<float>(snapDivision)) * beatWidth;
                    g.setColour(juce::Colour(0xff2b2b2b));
                    g.drawVerticalLine(static_cast<int>(subX), grid.getY(), grid.getBottom());
                }
            }

            if (beat < beatsVisible && beat % markerStep == 0)
            {
                g.setColour(juce::Colour(0xff9fd0ff));
                g.drawText(
                    formatTimelineTime(static_cast<double>(startBeat + beat), tempoBpm, showTenths),
                    static_cast<int>(x + 2),
                    1,
                    static_cast<int>(beatWidth * markerStep - 4),
                    timeRulerHeight - 2,
                    juce::Justification::centredLeft
                );

                g.setColour(juce::Colour(0xffbbbbbb));
                g.drawText(
                    juce::String(startBeat + beat + 1),
                    static_cast<int>(x + 2),
                    beatRulerTop,
                    static_cast<int>(beatWidth * markerStep - 4),
                    rulerHeight - beatRulerTop - 2,
                    juce::Justification::centredLeft
                );
            }
        }

        for (int pitch = lowPitch; pitch <= highPitch; ++pitch)
        {
            const auto topVisiblePitch = pitchScrollStart + static_cast<double>(visiblePitchRows);
            const float y = grid.getY() + static_cast<float>(topVisiblePitch - static_cast<double>(pitch + 1)) * rowHeight;

            if (y >= grid.getBottom() || y + rowHeight <= grid.getY())
                continue;

            const bool isBlackKey = isAccidentalPitch(pitch);

            if (isBlackKey)
            {
                g.setColour(juce::Colour(0x14111111));
                g.fillRect(grid.getX(), y, grid.getWidth(), rowHeight);
            }

            g.setColour(pitch % 12 == 0 ? juce::Colour(0xff505050) : juce::Colour(0xff303030));
            g.drawHorizontalLine(static_cast<int>(y), grid.getX(), grid.getRight());

            const int rowY = static_cast<int>(y);
            const int rowH = std::max(1, static_cast<int>(std::ceil(rowHeight)));
            const int octaveColumnWidth = 18;
            const int accidentalColumnWidth = 14;
            const int noteColumnX = octaveColumnWidth + accidentalColumnWidth;
            const int noteColumnWidth = keyLabelWidth - noteColumnX - 6;

            g.setColour(juce::Colour(0xff777777));
            g.setFont(juce::FontOptions(juce::jlimit(7.0f, 10.0f, rowHeight - 2.0f)));
            g.drawText(
                juce::String(pitchToOctave(pitch)),
                2,
                rowY,
                octaveColumnWidth - 4,
                rowH,
                juce::Justification::centredRight
            );

            if (isBlackKey)
            {
                g.setColour(juce::Colour(0xff9fd0ff));
                g.setFont(juce::FontOptions(juce::jlimit(8.0f, 12.0f, rowHeight - 1.0f)));
                g.drawText(
                    juce::String(juce::CharPointer_UTF8("\xe2\x99\xaf")),
                    octaveColumnWidth,
                    rowY,
                    accidentalColumnWidth,
                    rowH,
                    juce::Justification::centred
                );
            }

            g.setColour(pitch % 12 == 0 ? juce::Colour(0xffeeeeee) : (isBlackKey ? juce::Colour(0xffd8d8d8) : juce::Colour(0xffcfcfcf)));
            g.setFont(juce::FontOptions(juce::jlimit(8.0f, 12.0f, rowHeight - 1.0f)));
            g.drawText(
                pitchToNoteName(pitch),
                noteColumnX,
                rowY,
                noteColumnWidth,
                rowH,
                juce::Justification::centredRight
            );
        }

        if (notes.empty())
        {
            g.setColour(juce::Colour(0xffaaaaaa));
            g.drawText(
                "Click anywhere in the grid to add a note",
                grid.toNearestInt().reduced(12),
                juce::Justification::centred
            );
        }

        for (int i = 0; i < static_cast<int>(notes.size()); ++i)
        {
            const auto& note = notes[static_cast<std::size_t>(i)];

            if (note.pitch < lowPitch || note.pitch > highPitch)
                continue;

            const float noteStartBeat = static_cast<float>(note.startTick) / static_cast<float>(mw::core::Project::ticksPerQuarterNote);
            const float noteEndBeat = noteStartBeat + static_cast<float>(note.durationTicks) / static_cast<float>(mw::core::Project::ticksPerQuarterNote);

            if (noteEndBeat <= static_cast<float>(startBeat) || noteStartBeat >= static_cast<float>(startBeat + beatsVisible))
                continue;

            auto r = noteToRectangle(note).reduced(1.5f);

            const bool isPlayingNote =
                previewPlayheadActive
                && previewPlayheadBeat >= static_cast<double>(noteStartBeat)
                && previewPlayheadBeat < static_cast<double>(noteEndBeat);

            if (isPlayingNote)
                g.setColour(juce::Colour(0xffffd23f));
            else if (previewPlayheadActive)
                g.setColour(isNoteSelected(i) ? juce::Colour(0xccffc857) : juce::Colour(0x884ea3ff));
            else
                g.setColour(isNoteSelected(i) ? juce::Colour(0xffffc857) : juce::Colour(0xff4ea3ff));

            g.fillRoundedRectangle(r, 3.0f);

            if (isPlayingNote)
            {
                g.setColour(juce::Colour(0xffffffff));
                g.drawRoundedRectangle(r.expanded(1.0f), 4.0f, 2.0f);
            }

            if (isNoteSelected(i))
            {
                g.setColour(juce::Colour(0xffffffff));
                g.drawRoundedRectangle(r, 3.0f, 1.5f);
            }

            // Draw a small resize handle on the right edge.
            g.setColour(juce::Colour(0xee101010));
            g.fillRect(r.getRight() - 5.0f, r.getY() + 2.0f, 3.0f, r.getHeight() - 4.0f);

            g.setColour(juce::Colour(0xff101010));
            g.drawRoundedRectangle(r, 3.0f, 1.0f);
        }

        if (previewPlayheadActive)
        {
            const auto relativeBeat = previewPlayheadBeat - static_cast<double>(startBeat);

            if (relativeBeat >= 0.0 && relativeBeat <= static_cast<double>(beatsVisible))
            {
                const float x =
                    grid.getX()
                    + static_cast<float>(relativeBeat)
                    * beatWidth;

                g.setColour(juce::Colour(0xffffe066));
                g.drawLine(x, grid.getY(), x, grid.getBottom(), 3.5f);

                g.setColour(juce::Colour(0xffffe066).withAlpha(0.35f));
                g.fillRect(x - 3.0f, grid.getY(), 6.0f, grid.getHeight());
            }
        }

        if (dragMode == DragMode::Marquee)
        {
            const auto marquee = getMarqueeRectangle();

            if (marquee.getWidth() > 2.0f && marquee.getHeight() > 2.0f)
            {
                g.setColour(juce::Colour(0x44ffe066));
                g.fillRect(marquee);
                g.setColour(juce::Colour(0xffffe066));
                g.drawRect(marquee, 1.5f);
            }
        }

        g.setColour(juce::Colour(0xff777777));
        g.drawRect(getLocalBounds());
    }

    void PianoRollComponent::mouseDown(const juce::MouseEvent& event)
    {
        const auto point = event.position;
        const auto grid = getGridBounds();

        if (!grid.contains(point))
            return;

        grabKeyboardFocus();
        undoCapturedForGesture = false;

        const int clickedNote = findNoteAt(point);

        if (event.mods.isRightButtonDown())
        {
            if (clickedNote >= 0)
            {
                if (!isNoteSelected(clickedNote))
                    setSingleSelection(clickedNote);

                deleteSelectedNote();
            }

            dragMode = DragMode::None;
            return;
        }

        if (event.mods.isShiftDown())
        {
            if (clickedNote >= 0)
            {
                toggleNoteSelection(clickedNote);

                if (onSelectedNoteChanged)
                    onSelectedNoteChanged(selectedNoteIndex);

                repaint();
                return;
            }

            marqueeStart = point;
            marqueeEnd = point;
            dragMode = DragMode::Marquee;
            selectNotesInRectangle(getMarqueeRectangle());

            if (onSelectedNoteChanged)
                onSelectedNoteChanged(selectedNoteIndex);

            repaint();
            return;
        }

        if (clickedNote >= 0)
        {
            if (!isNoteSelected(clickedNote))
                setSingleSelection(clickedNote);
            else
                selectedNoteIndex = clickedNote;

            const bool resizing = isNearRightEdge(clickedNote, point);

            if (resizing)
                setSingleSelection(clickedNote);

            dragMode = resizing ? DragMode::ResizeEnd : DragMode::Move;
            captureUndoState();
            undoCapturedForGesture = true;
            dragStartTick = xToSnapTick(point.x);
            dragStartBeat = static_cast<int>(dragStartTick / mw::core::Project::ticksPerQuarterNote);
            dragStartPitch = yToPitch(point.y);
            dragOriginalNote = notes[static_cast<std::size_t>(clickedNote)];

            dragOriginalSelectedNotes.clear();

            if (dragMode == DragMode::Move)
            {
                if (selectedNoteIndices.empty())
                    selectedNoteIndices.push_back(clickedNote);

                for (int index : selectedNoteIndices)
                {
                    if (index >= 0 && index < static_cast<int>(notes.size()))
                        dragOriginalSelectedNotes.push_back({ index, notes[static_cast<std::size_t>(index)] });
                }

                if (dragOriginalSelectedNotes.empty())
                    dragOriginalSelectedNotes.push_back({ clickedNote, notes[static_cast<std::size_t>(clickedNote)] });
            }

            if (onSelectedNoteChanged)
                onSelectedNoteChanged(selectedNoteIndex);

            repaint();
            return;
        }

        clearSelection();
        lastPaintTick = -1;
        lastPaintPitch = -1;
        dragMode = DragMode::Paint;
        addPaintNoteAt(point);
    }

    void PianoRollComponent::mouseDrag(const juce::MouseEvent& event)
    {
        if (dragMode == DragMode::Marquee)
        {
            marqueeEnd = event.position;
            selectNotesInRectangle(getMarqueeRectangle());

            if (onSelectedNoteChanged)
                onSelectedNoteChanged(selectedNoteIndex);

            repaint();
            return;
        }

        if (dragMode == DragMode::Paint)
        {
            addPaintNoteAt(event.position);
            return;
        }

        if (selectedNoteIndex < 0 || selectedNoteIndex >= static_cast<int>(notes.size()))
            return;

        if (dragMode == DragMode::None)
            return;

        const auto point = event.position;
        const auto snappedTick = xToSnapTick(point.x);
        const int pitch = yToPitch(point.y);

        if (dragMode == DragMode::Move)
        {
            if (dragOriginalSelectedNotes.empty() && selectedNoteIndex >= 0 && selectedNoteIndex < static_cast<int>(notes.size()))
                dragOriginalSelectedNotes.push_back({ selectedNoteIndex, notes[static_cast<std::size_t>(selectedNoteIndex)] });

            const auto tickDelta = snappedTick - dragStartTick;
            const int pitchDelta = pitch - dragStartPitch;

            for (const auto& original : dragOriginalSelectedNotes)
            {
                const int index = original.first;

                if (index < 0 || index >= static_cast<int>(notes.size()))
                    continue;

                auto& note = notes[static_cast<std::size_t>(index)];
                note.startTick = std::max<std::int64_t>(0, original.second.startTick + tickDelta);
                note.pitch = std::clamp(original.second.pitch + pitchDelta, 0, 127);
            }
        }
        else if (dragMode == DragMode::ResizeEnd)
        {
            auto& note = notes[static_cast<std::size_t>(selectedNoteIndex)];

            const auto minimumDurationTicks = getSnapTickLength(snapDivision);
            const auto newEndTick =
                std::max<std::int64_t>(
                    note.startTick + minimumDurationTicks,
                    snappedTick
                );

            note.durationTicks = std::max<std::int64_t>(minimumDurationTicks, newEndTick - note.startTick);
        }

        if (onNotesChanged)
            onNotesChanged(notes);

        repaint();
    }

    void PianoRollComponent::mouseUp(const juce::MouseEvent&)
    {
        if (dragMode != DragMode::None)
        {
            dragMode = DragMode::None;
            lastPaintTick = -1;
            lastPaintPitch = -1;
            undoCapturedForGesture = false;
            dragOriginalSelectedNotes.clear();

            if (onNotesChanged)
                onNotesChanged(notes);

            if (onSelectedNoteChanged)
                onSelectedNoteChanged(selectedNoteIndex);

            repaint();
        }
    }


    bool PianoRollComponent::keyPressed(const juce::KeyPress& key)
    {
        if (key == juce::KeyPress::escapeKey)
        {
            clearNoteSelection();
            return true;
        }

        if (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey)
        {
            deleteSelectedNote();
            return true;
        }

        const auto character = key.getTextCharacter();
        const auto modifiers = key.getModifiers();

        if ((modifiers.isCommandDown() || modifiers.isCtrlDown()) && (character == 'z' || character == 'Z'))
        {
            if (modifiers.isShiftDown())
                redoLastNoteEdit();
            else
                undoLastNoteEdit();

            return true;
        }

        if ((modifiers.isCommandDown() || modifiers.isCtrlDown()) && (character == 'y' || character == 'Y'))
        {
            redoLastNoteEdit();
            return true;
        }

        if ((modifiers.isCommandDown() || modifiers.isCtrlDown()) && (character == 'c' || character == 'C'))
        {
            copySelectedNote();
            return true;
        }

        if ((modifiers.isCommandDown() || modifiers.isCtrlDown()) && (character == 'v' || character == 'V'))
        {
            pasteCopiedNote();
            return true;
        }

        return false;
    }

}
