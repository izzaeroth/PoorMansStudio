#pragma once

#include <juce_gui_extra/juce_gui_extra.h>

#include <cstdint>
#include <functional>
#include <vector>
#include <utility>

#include "core/NoteEvent.h"
#include "core/Project.h"

namespace mw::gui
{
    class PianoRollComponent final : public juce::Component,
                                     private juce::ScrollBar::Listener,
                                     private juce::Timer
    {
    public:
        PianoRollComponent();

        void paint(juce::Graphics& g) override;
        void resized() override;
        void mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) override;
        void scrollBarMoved(juce::ScrollBar* scrollBarThatHasMoved, double newRangeStart) override;
        void timerCallback() override;
        void mouseDown(const juce::MouseEvent& event) override;
        void mouseDrag(const juce::MouseEvent& event) override;
        void mouseUp(const juce::MouseEvent& event) override;
        bool keyPressed(const juce::KeyPress& key) override;

        void setNotes(const std::vector<mw::core::NoteEvent>& notesToShow);
        void setNotesPreservingHistory(const std::vector<mw::core::NoteEvent>& notesToShow);
        const std::vector<mw::core::NoteEvent>& getNotes() const { return notes; }

        void setGrid(int visibleBeats, int lowestPitch, int highestPitch);
        void setDefaultNoteSettings(double lengthBeats, int velocity);
        void setSnapDivision(int snapDivisionValue);
        void setStartBeat(int startBeatValue);
        void setTempoBpm(int tempoBpmValue);
        int getStartBeat() const { return startBeat; }
        int getBeatsVisible() const { return beatsVisible; }
        juce::String getVisiblePitchRangeText() const;
        int getLowPitch() const { return lowPitch; }
        int getHighPitch() const { return highPitch; }
        bool jumpToKey(const juce::String& keyText);
        void nudgeVisiblePitchRange(int semitones);
        void startPreviewPlayhead(double tempoBpm, double durationBeats, double startBeatValue = 0.0);
        void stopPreviewPlayhead();
        void pausePreviewPlayhead();
        void resumePreviewPlayhead();
        bool isPreviewPlayheadActive() const { return previewPlayheadActive; }
        bool isPreviewPlayheadPaused() const { return previewPlayheadPaused; }
        double getPreviewPlayheadBeat() const { return previewPlayheadBeat; }

        void setSelectedNoteIndex(int index);
        int getSelectedNoteIndex() const { return selectedNoteIndex; }
        void clearNoteSelection();

        void deleteSelectedNote();
        void copySelectedNote();
        void pasteCopiedNote();
        void undoLastNoteEdit();
        void redoLastNoteEdit();
        bool canUndoNoteEdit() const { return !undoStack.empty(); }
        bool canRedoNoteEdit() const { return !redoStack.empty(); }

        std::function<void(const std::vector<mw::core::NoteEvent>&)> onNotesChanged;
        std::function<void(int)> onSelectedNoteChanged;
        std::function<void()> onPitchRangeChanged;
        std::function<void(int)> onPreviewPlayheadPageChanged;

    private:
        juce::Rectangle<float> getGridBounds() const;
        void updatePitchScrollBar();
        void setPitchScrollStart(double pitchStart);
        void setVisiblePitchStart(int pitchStart);
        void validatePitchWindow();
        static juce::String pitchToName(int pitch);
        static juce::String pitchToNoteName(int pitch);
        static int pitchToOctave(int pitch);
        static bool isAccidentalPitch(int pitch);
        static int noteNameToSemitone(const juce::String& noteName);
        int xToBeat(float x) const;
        std::int64_t xToSnapTick(float x) const;
        std::int64_t xToPaintSnapTick(float x) const;
        int yToPitch(float y) const;
        juce::Rectangle<float> noteToRectangle(const mw::core::NoteEvent& note) const;
        int findNoteAt(juce::Point<float> point) const;
        bool isNoteSelected(int index) const;
        void clearSelection();
        void setSingleSelection(int index);
        void toggleNoteSelection(int index);
        void selectNotesInRectangle(juce::Rectangle<float> rectangle);
        juce::Rectangle<float> getMarqueeRectangle() const;
        bool isNearRightEdge(int noteIndex, juce::Point<float> point) const;
        bool hasNoteAt(int pitch, std::int64_t startTick) const;
        bool addPaintNoteAt(juce::Point<float> point);
        void captureUndoState();
        void trimUndoHistory();
        void applyHistoryState(const std::vector<mw::core::NoteEvent>& state);

        enum class DragMode
        {
            None,
            Move,
            ResizeEnd,
            Paint,
            Marquee
        };

        std::vector<mw::core::NoteEvent> notes;
        static constexpr std::size_t maxUndoSteps = 10;
        std::vector<std::vector<mw::core::NoteEvent>> undoStack;
        std::vector<std::vector<mw::core::NoteEvent>> redoStack;

        int selectedNoteIndex = -1;
        std::vector<int> selectedNoteIndices;
        DragMode dragMode = DragMode::None;
        int dragStartBeat = 0;
        std::int64_t dragStartTick = 0;
        int dragStartPitch = 60;
        mw::core::NoteEvent dragOriginalNote {};
        std::vector<std::pair<int, mw::core::NoteEvent>> dragOriginalSelectedNotes;
        std::int64_t lastPaintTick = -1;
        int lastPaintPitch = -1;
        bool undoCapturedForGesture = false;
        juce::Point<float> marqueeStart;
        juce::Point<float> marqueeEnd;
        bool hasCopiedNote = false;
        mw::core::NoteEvent copiedNote {};
        std::vector<mw::core::NoteEvent> copiedNotes;

        int beatsVisible = 16;
        int startBeat = 0;
        int lowPitch = 48;
        int highPitch = 72;
        double pitchScrollStart = 48.0;
        int visiblePitchRows = 25;
        int keyLabelWidth = 68;
        int rulerHeight = 36;
        int snapDivision = 1;
        int tempoBpm = 120;
        static constexpr int pitchScrollBarWidth = 16;
        juce::ScrollBar pitchScrollBar { true };
        double defaultNoteLengthBeats = 1.0;
        int defaultVelocity = 100;
        bool previewPlayheadActive = false;
        bool previewPlayheadPaused = false;
        double previewPlayheadBeat = 0.0;
        double previewPlayheadStartBeat = 0.0;
        double previewPlayheadDurationBeats = 0.0;
        double previewPlayheadTempoBpm = 120.0;
        double previewPlayheadStartMilliseconds = 0.0;
        int lastPreviewPlayheadPage = -1;
    };
}
