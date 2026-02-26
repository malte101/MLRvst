/*
  ==============================================================================

    StepSequencerDisplay.h
    Visual display + editor for step sequencer mode

  ==============================================================================
*/

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <set>
#include <vector>

class StepSequencerDisplay : public juce::Component, public juce::Timer
{
public:
    enum class EditTool
    {
        Draw = 0,
        Divide,
        Volume,
        RampUp,
        RampDown,
        Probability,
        Select
    };

    StepSequencerDisplay()
    {
        stepSubdivisions.fill(1);
        stepVelocityStart.fill(1.0f);
        stepVelocityEnd.fill(1.0f);
        stepProbability.fill(1.0f);
        setWantsKeyboardFocus(true);
    }

    ~StepSequencerDisplay() override
    {
        stopTimer();
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour(0xff1f1f1f));

        const auto toolbar = getToolbarBounds();
        drawToolbar(g, toolbar);

        const auto grid = getGridBounds();
        drawGrid(g, grid);

        if (dragMode == DragMode::Lasso && !lassoRect.isEmpty())
        {
            g.setColour(juce::Colours::white.withAlpha(0.16f));
            g.fillRect(lassoRect);
            g.setColour(juce::Colours::white.withAlpha(0.92f));
            g.drawRect(lassoRect, 1.0f);
        }
    }

    void setStepPattern(const std::array<bool, 64>& pattern, int steps)
    {
        stepPattern = pattern;
        totalSteps = juce::jlimit(1, 64, steps);
        pruneSelectionToVisibleSteps();
        repaint();
    }

    void setStepSubdivisions(const std::array<int, 64>& subdivisions)
    {
        stepSubdivisions = subdivisions;
        for (auto& value : stepSubdivisions)
            value = juce::jlimit(1, kMaxStepSubdivisions, value);
        repaint();
    }

    void setStepSubdivisionVelocityRange(const std::array<float, 64>& startVelocity,
                                         const std::array<float, 64>& endVelocity)
    {
        stepVelocityStart = startVelocity;
        stepVelocityEnd = endVelocity;
        for (size_t i = 0; i < stepVelocityStart.size(); ++i)
        {
            stepVelocityStart[i] = juce::jlimit(0.0f, 1.0f, stepVelocityStart[i]);
            stepVelocityEnd[i] = juce::jlimit(0.0f, 1.0f, stepVelocityEnd[i]);
        }
        repaint();
    }

    void setStepProbability(const std::array<float, 64>& probability)
    {
        stepProbability = probability;
        for (auto& value : stepProbability)
            value = juce::jlimit(0.0f, 1.0f, value);
        repaint();
    }

    void setCurrentStep(int step)
    {
        const int clampedStep = juce::jlimit(0, juce::jmax(0, totalSteps - 1), step);
        if (currentStep != clampedStep)
        {
            currentStep = clampedStep;
            repaint();
        }
    }

    void setPlaying(bool playing)
    {
        if (isPlaying != playing)
        {
            isPlaying = playing;
            updateTimerState();
            repaint();
        }
    }

    void setStripColor(juce::Colour color)
    {
        stripColor = color;
        repaint();
    }

    void setPlaybackPosition(float position)
    {
        playbackPosition = position;
        repaint();
    }

    EditTool getActiveTool() const
    {
        return activeTool;
    }

    void setActiveTool(EditTool tool)
    {
        if (tool == EditTool::Draw)
            tool = EditTool::Volume;

        if (tool == activeTool)
            return;

        activeTool = tool;
        dragMode = DragMode::None;
        dragTool = activeTool;
        dragTargets.clear();
        dragAnchorStep = -1;
        dragAnchorRect = {};
        lastDrawStep = -1;
        lassoRect = {};
        selectClickCandidateStep = -1;
        selectLassoActivated = false;
        drawShiftToggleCandidateStep = -1;
        drawShiftToggleDragged = false;
        updateTimerState();
        repaint();
    }

    void timerCallback() override
    {
        repaint();
    }

    bool keyStateChanged(bool) override
    {
        handleSelectShortcutState(
            isSelectToggleShortcutDown(juce::ModifierKeys::getCurrentModifiersRealtime()));
        return false;
    }

    void modifierKeysChanged(const juce::ModifierKeys& modifiers) override
    {
        handleSelectShortcutState(isSelectToggleShortcutDown(modifiers));
    }

    void mouseDown(const juce::MouseEvent& event) override
    {
        if (!hasKeyboardFocus(true))
            grabKeyboardFocus();

        const bool shortcutDown = isSelectToggleShortcutDown(event.mods)
            || isSelectToggleShortcutDown(juce::ModifierKeys::getCurrentModifiersRealtime());
        handleSelectShortcutState(shortcutDown);
        if (shortcutDown && !event.mods.isRightButtonDown())
            return;

        if (handleToolbarMouseDown(event.position))
            return;

        const int stepIndex = getStepIndexFromPosition(event.position);
        if (stepIndex < 0 || stepIndex >= totalSteps)
            return;

        focusedStep = stepIndex;
        selectClickCandidateStep = -1;
        selectLassoActivated = false;
        drawShiftToggleCandidateStep = -1;
        drawShiftToggleDragged = false;

        const bool commandDown = event.mods.isCommandDown();
        const bool controlDown = event.mods.isCtrlDown();
        const bool shiftDown = event.mods.isShiftDown();
        const bool optionDown = event.mods.isAltDown();
        const bool editModifierDown = (commandDown || controlDown || optionDown);
        const bool stepIsInSelection = (selectedSteps.count(stepIndex) != 0);
        const bool inferredSelectCtrlGesture = (activeTool == EditTool::Select)
            && stepIsInSelection
            && event.mods.isRightButtonDown()
            && !commandDown
            && !optionDown
            && !shiftDown;
        const bool drawModifierGesture = isDrawLikeTool(activeTool)
            && (editModifierDown || shiftDown);
        const bool selectionModifierGesture = (activeTool == EditTool::Select)
            && stepIsInSelection
            && (editModifierDown || inferredSelectCtrlGesture);
        const bool modifierGesture = drawModifierGesture || selectionModifierGesture;

        if (event.mods.isRightButtonDown() && !modifierGesture)
        {
            showContextMenuForStep(stepIndex);
            return;
        }

        if (activeTool == EditTool::Select)
        {
            if (selectionModifierGesture)
            {
                EditTool modifierTool = EditTool::Volume;
                if (commandDown)
                    modifierTool = EditTool::Divide;
                else if (controlDown || inferredSelectCtrlGesture)
                    modifierTool = EditTool::RampUp;
                else if (optionDown)
                    modifierTool = EditTool::RampDown;

                beginEdit(stepIndex, event.y, modifierTool);
                applyContinuousTool(event.y);
                return;
            }

            selectClickCandidateStep = stepIndex;
            selectLassoActivated = false;
            beginLasso(event.position, true);
            return;
        }

        if (isDrawLikeTool(activeTool) && commandDown)
        {
            beginEdit(stepIndex, event.y, EditTool::Divide);
            applyContinuousTool(event.y);
            return;
        }

        if (isDrawLikeTool(activeTool) && controlDown)
        {
            if (!stepPattern[static_cast<size_t>(stepIndex)])
                setStepEnabled(stepIndex, true, true);
            beginEdit(stepIndex, event.y, EditTool::RampUp);
            applyContinuousTool(event.y);
            return;
        }

        if (isDrawLikeTool(activeTool) && optionDown)
        {
            if (!stepPattern[static_cast<size_t>(stepIndex)])
                setStepEnabled(stepIndex, true, true);
            beginEdit(stepIndex, event.y, EditTool::RampDown);
            applyContinuousTool(event.y);
            return;
        }

        if (isDrawLikeTool(activeTool) && shiftDown && !commandDown && !controlDown && !optionDown)
        {
            drawShiftToggleCandidateStep = stepIndex;
            drawShiftToggleDragged = false;
            drawShiftToggleStart = event.position;

            if (stepPattern[static_cast<size_t>(stepIndex)])
            {
                beginEdit(stepIndex, event.y, EditTool::Volume);
                applyContinuousTool(event.y);
            }
            return;
        }

        if (isDrawLikeTool(activeTool))
        {
            applyDrawVolumeAtStep(stepIndex, event.y, true);
            return;
        }

        beginEdit(stepIndex, event.y, activeTool);
        applyContinuousTool(event.y);
    }

    void mouseDrag(const juce::MouseEvent& event) override
    {
        if (dragMode == DragMode::Lasso)
        {
            if (activeTool == EditTool::Select
                && selectClickCandidateStep >= 0
                && !selectLassoActivated)
            {
                const float dragDistance = event.position.getDistanceFrom(lassoStart);
                if (dragDistance < kSelectDragThresholdPixels)
                    return;
                selectLassoActivated = true;
            }

            updateLasso(event.position);
            repaint();
            return;
        }

        if (dragMode != DragMode::Edit)
            return;

        if (drawShiftToggleCandidateStep >= 0 && dragTool == EditTool::Volume)
        {
            if (!drawShiftToggleDragged)
            {
                const float dragDistance = event.position.getDistanceFrom(drawShiftToggleStart);
                if (dragDistance < kSelectDragThresholdPixels)
                    return;
                drawShiftToggleDragged = true;
            }
        }

        if (isDrawLikeTool(activeTool)
            && dragTool == EditTool::Volume
            && drawShiftToggleCandidateStep < 0)
        {
            const int stepIndex = getStepIndexFromPosition(event.position);
            if (stepIndex >= 0 && stepIndex < totalSteps && stepIndex != lastDrawStep)
            {
                applyDrawVolumeAtStep(stepIndex, event.y, true);
                return;
            }
        }

        applyContinuousTool(event.y);
    }

    void mouseUp(const juce::MouseEvent&) override
    {
        if (dragMode == DragMode::Lasso
            && activeTool == EditTool::Select
            && selectClickCandidateStep >= 0
            && !selectLassoActivated)
        {
            toggleSelection(selectClickCandidateStep);
            repaint();
        }

        if (drawShiftToggleCandidateStep >= 0 && !drawShiftToggleDragged)
        {
            const int step = drawShiftToggleCandidateStep;
            if (step >= 0 && step < totalSteps)
                setStepEnabled(step, !stepPattern[static_cast<size_t>(step)], true);
            repaint();
        }

        dragMode = DragMode::None;
        dragTool = activeTool;
        dragTargets.clear();
        dragAnchorStep = -1;
        dragAnchorRect = {};
        lastDrawStep = -1;
        lassoRect = {};
        selectClickCandidateStep = -1;
        selectLassoActivated = false;
        drawShiftToggleCandidateStep = -1;
        drawShiftToggleDragged = false;
        updateTimerState();
    }

    std::function<void(int)> onStepClicked;
    std::function<void(int, bool)> onStepSet;
    std::function<void(int, int)> onStepSubdivisionSet;
    std::function<void(int, float, float)> onStepVelocityRangeSet;
    std::function<void(int, float)> onStepProbabilitySet;

private:
    static constexpr int kMaxStepSubdivisions = 16;
    static constexpr int kAutoRampSubdivision = 2;
    static constexpr float kToolbarHeight = 24.0f;
    static constexpr float kSelectDragThresholdPixels = 3.0f;

    enum class DragMode
    {
        None = 0,
        Edit,
        Lasso
    };

    struct ToolbarLayout
    {
        std::array<juce::Rectangle<float>, 6> toolButtons;
    };

    std::array<bool, 64> stepPattern = {};
    std::array<int, 64> stepSubdivisions = {};
    std::array<float, 64> stepVelocityStart = {};
    std::array<float, 64> stepVelocityEnd = {};
    std::array<float, 64> stepProbability = {};

    int totalSteps = 16;
    int currentStep = 0;
    bool isPlaying = false;
    float playbackPosition = -1.0f;
    juce::Colour stripColor = juce::Colour(0xff6f93c8);

    EditTool activeTool = EditTool::Volume;
    DragMode dragMode = DragMode::None;
    EditTool dragTool = EditTool::Volume;
    int dragStartY = 0;
    int dragAnchorStep = -1;
    juce::Rectangle<float> dragAnchorRect;
    std::vector<int> dragTargets;
    std::array<int, 64> dragStartSubdivisions = {};
    std::array<float, 64> dragStartVelocityStart = {};
    std::array<float, 64> dragStartVelocityEnd = {};

    int lastDrawStep = -1;
    int focusedStep = -1;

    std::set<int> selectedSteps;
    std::set<int> lassoBaseSelection;
    juce::Point<float> lassoStart;
    juce::Rectangle<float> lassoRect;
    bool lassoAdditive = true;
    int selectClickCandidateStep = -1;
    bool selectLassoActivated = false;

    int drawShiftToggleCandidateStep = -1;
    bool drawShiftToggleDragged = false;
    juce::Point<float> drawShiftToggleStart;
    bool selectShortcutLatched = false;

    juce::Rectangle<float> getContentBounds() const
    {
        return getLocalBounds().reduced(1).toFloat();
    }

    juce::Rectangle<float> getToolbarBounds() const
    {
        auto content = getContentBounds();
        return content.removeFromTop(kToolbarHeight);
    }

    juce::Rectangle<float> getGridBounds() const
    {
        auto content = getContentBounds();
        content.removeFromTop(kToolbarHeight + 1.0f);
        return content;
    }

    ToolbarLayout getToolbarLayout() const
    {
        ToolbarLayout layout{};
        const auto bar = getToolbarBounds().reduced(3.0f, 2.0f);

        constexpr float gap = 3.0f;
        const int toolCount = static_cast<int>(layout.toolButtons.size());
        const float available = juce::jmax(0.0f, bar.getWidth() - ((static_cast<float>(toolCount - 1) * gap)));
        const float toolW = juce::jmax(34.0f, available / static_cast<float>(toolCount));

        float x = bar.getX();
        for (int i = 0; i < toolCount; ++i)
        {
            layout.toolButtons[static_cast<size_t>(i)] = juce::Rectangle<float>(x, bar.getY(), toolW, bar.getHeight());
            x += toolW + gap;
        }

        return layout;
    }

    std::array<const char*, 6> getToolLabels() const
    {
        return {"Vol", "Divide", "Ramp+", "Ramp-", "Prob", "Select"};
    }

    EditTool toolFromIndex(int index) const
    {
        switch (juce::jlimit(0, 5, index))
        {
            case 0: return EditTool::Volume;
            case 1: return EditTool::Divide;
            case 2: return EditTool::RampUp;
            case 3: return EditTool::RampDown;
            case 4: return EditTool::Probability;
            case 5: return EditTool::Select;
            default: break;
        }
        return EditTool::Volume;
    }

    int indexFromTool(EditTool tool) const
    {
        switch (tool)
        {
            case EditTool::Divide: return 1;
            case EditTool::Volume:
            case EditTool::Draw:
                return 0;
            case EditTool::RampUp: return 2;
            case EditTool::RampDown: return 3;
            case EditTool::Probability: return 4;
            case EditTool::Select: return 5;
            default: break;
        }
        return 1;
    }

    void drawToolbar(juce::Graphics& g, juce::Rectangle<float> toolbar)
    {
        g.setColour(juce::Colour(0xff202226));
        g.fillRect(toolbar);

        const auto layout = getToolbarLayout();
        const auto labels = getToolLabels();

        for (size_t i = 0; i < layout.toolButtons.size(); ++i)
        {
            const auto tool = toolFromIndex(static_cast<int>(i));
            const bool active = (tool == activeTool);
            const auto rect = layout.toolButtons[i];

            g.setColour(active ? juce::Colour(0xff4c698d) : juce::Colour(0xff31353a));
            g.fillRoundedRectangle(rect, 3.0f);
            g.setColour(active ? juce::Colour(0xff98c6ff) : juce::Colour(0xff4b5158));
            g.drawRoundedRectangle(rect, 3.0f, 1.0f);
            g.setColour(active ? juce::Colour(0xfff2f6ff) : juce::Colour(0xffc1c7cf));
            g.setFont(11.0f);
            g.drawText(labels[i], rect.toNearestInt(), juce::Justification::centred, false);
        }
    }

    juce::Rectangle<float> getStepRect(int stepIndex) const
    {
        if (stepIndex < 0 || stepIndex >= totalSteps)
            return {};

        const auto grid = getGridBounds();
        const int stepsPerRow = 16;
        const int numRows = juce::jmax(1, (totalSteps + 15) / 16);
        const float stepWidth = grid.getWidth() / static_cast<float>(stepsPerRow);
        const float stepHeight = grid.getHeight() / static_cast<float>(numRows);

        const int row = stepIndex / stepsPerRow;
        const int col = stepIndex % stepsPerRow;

        return juce::Rectangle<float>(
            grid.getX() + (col * stepWidth),
            grid.getY() + (row * stepHeight),
            stepWidth - 2.0f,
            stepHeight - 2.0f);
    }

    int getStepIndexFromPosition(juce::Point<float> position) const
    {
        const auto grid = getGridBounds();
        if (!grid.contains(position))
            return -1;

        const int stepsPerRow = 16;
        const int numRows = juce::jmax(1, (totalSteps + 15) / 16);
        const float stepWidth = grid.getWidth() / static_cast<float>(stepsPerRow);
        const float stepHeight = grid.getHeight() / static_cast<float>(numRows);

        const float px = juce::jlimit(grid.getX(), grid.getRight() - 0.001f, position.x) - grid.getX();
        const float py = juce::jlimit(grid.getY(), grid.getBottom() - 0.001f, position.y) - grid.getY();

        const int col = juce::jlimit(0, stepsPerRow - 1, static_cast<int>(px / stepWidth));
        const int row = juce::jlimit(0, numRows - 1, static_cast<int>(py / stepHeight));

        const int index = row * stepsPerRow + col;
        if (index < 0 || index >= totalSteps)
            return -1;
        return index;
    }

    void drawGrid(juce::Graphics& g, juce::Rectangle<float> grid)
    {
        if (grid.isEmpty())
            return;

        const int numRows = juce::jmax(1, (totalSteps + 15) / 16);
        const int stepsPerRow = 16;
        const float stepWidth = grid.getWidth() / static_cast<float>(stepsPerRow);
        const float stepHeight = grid.getHeight() / static_cast<float>(numRows);
        const bool hasPlayStep = isPlaying && totalSteps > 0;
        const int playStep = hasPlayStep ? juce::jlimit(0, totalSteps - 1, currentStep) : -1;

        g.setColour(juce::Colour(0xff24272c));
        g.fillRect(grid);

        for (int i = 0; i < totalSteps; ++i)
        {
            const int row = i / stepsPerRow;
            const int col = i % stepsPerRow;
            juce::Rectangle<float> stepRect(
                grid.getX() + (col * stepWidth),
                grid.getY() + (row * stepHeight),
                stepWidth - 2.0f,
                stepHeight - 2.0f);
            const bool isCurrentPlayStep = (i == playStep);

            const float probability = juce::jlimit(0.0f, 1.0f, stepProbability[static_cast<size_t>(i)]);
            const bool isEnabled = stepPattern[static_cast<size_t>(i)];
            const bool selectedInSelectTool = (activeTool == EditTool::Select && selectedSteps.count(i) != 0);
            const float stepInnerTop = stepRect.getY() + 1.0f;
            const float probabilityTrackHeight = isEnabled
                ? juce::jmax(2.0f, juce::jmin(6.0f, stepRect.getHeight() * 0.14f))
                : 0.0f;
            const float velocityAreaTop = stepInnerTop + probabilityTrackHeight + (isEnabled ? 2.0f : 0.0f);

            if (!isEnabled)
            {
                g.setColour(juce::Colour(0xff141414).withAlpha(0.55f));
                g.drawRect(stepRect, 1.0f);

                g.setColour(juce::Colour(0xffa8a8a8).withAlpha(0.34f));
                g.setFont(stepHeight < 18.0f ? 8.0f : 10.0f);
                g.drawText(juce::String(i + 1), stepRect.toNearestInt(), juce::Justification::centred, false);

                if (selectedInSelectTool)
                {
                    g.setColour(juce::Colours::white.withAlpha(0.96f));
                    g.drawRect(stepRect.reduced(1.0f), 2.0f);
                }

                if (isCurrentPlayStep)
                {
                    g.setColour(juce::Colour(0xffffb347).withAlpha(0.96f));
                    g.drawRect(stepRect.reduced(0.5f), 2.0f);
                    g.fillRect(stepRect.withHeight(2.0f).reduced(1.0f, 0.0f));
                }
                continue;
            }

            juce::Colour stepColor;
            if (isCurrentPlayStep)
                stepColor = juce::Colour(0xfff29a36).withAlpha(0.82f);
            else
                stepColor = stripColor.withMultipliedSaturation(0.8f).withMultipliedBrightness(0.9f)
                                .withAlpha(0.78f);

            g.setColour(stepColor);
            g.fillRect(stepRect);

            g.setColour(juce::Colour(0xff141414));
            g.drawRect(stepRect, 1.0f);

            auto track = juce::Rectangle<float>(stepRect.getX() + 1.0f,
                                                stepInnerTop,
                                                juce::jmax(2.0f, stepRect.getWidth() - 2.0f),
                                                probabilityTrackHeight);
            const auto probabilityTint = stripColor.withMultipliedSaturation(1.25f).withMultipliedBrightness(1.15f);
            const auto probabilityTrack = stripColor.darker(1.9f).withAlpha(0.90f);
            const auto probabilityMissing = probabilityTint.darker(1.9f).withAlpha(0.82f);

            g.setColour(probabilityTrack);
            g.fillRect(track);

            if (probability > 0.0f)
            {
                auto fill = track.withWidth(track.getWidth() * probability);
                g.setColour(probabilityTint.withAlpha(0.95f));
                g.fillRect(fill);
            }

            if (probability < 0.995f)
            {
                const float startX = track.getX() + (track.getWidth() * probability);
                g.setColour(probabilityMissing);
                for (float x = startX; x < track.getRight(); x += 4.0f)
                {
                    const float x2 = juce::jmin(x + 3.0f, track.getRight());
                    g.drawLine(x, track.getY(), x2, track.getBottom(), 1.0f);
                }
            }

            g.setColour(probabilityTint.darker(1.2f).withAlpha(0.82f));
            g.drawRect(track, 1.0f);

            const int subdivision = juce::jlimit(1, kMaxStepSubdivisions, stepSubdivisions[static_cast<size_t>(i)]);
            const float velocityStart = juce::jlimit(0.0f, 1.0f, stepVelocityStart[static_cast<size_t>(i)]);
            const float velocityEnd = juce::jlimit(0.0f, 1.0f, stepVelocityEnd[static_cast<size_t>(i)]);

            const bool velocityToolActive = (activeTool == EditTool::Volume
                                             || activeTool == EditTool::RampUp
                                             || activeTool == EditTool::RampDown
                                             || (dragMode == DragMode::Edit
                                                 && (dragTool == EditTool::Volume
                                                     || dragTool == EditTool::RampUp
                                                     || dragTool == EditTool::RampDown)));
            const bool showVelocityBars = subdivision >= 1
                                          || velocityToolActive
                                          || velocityStart < 0.999f
                                          || velocityEnd < 0.999f;
            if (showVelocityBars)
            {
                const bool suppressVelocityFillOverlay = (dragMode == DragMode::Edit
                                                          && dragTool == EditTool::Volume);
                const int barCount = juce::jmax(1, subdivision);
                const float barAreaBottom = stepRect.getBottom() - 1.0f;
                const float barAreaTop = juce::jmin(barAreaBottom - 1.0f, velocityAreaTop);
                const float barAreaHeight = juce::jmax(1.0f, barAreaBottom - barAreaTop);
                const float slotWidth = stepRect.getWidth() / static_cast<float>(barCount);
                const float barWidth = juce::jmax(1.0f, slotWidth - 2.0f);
                const auto rampBright = stripColor
                    .withMultipliedSaturation(1.06f)
                    .withMultipliedBrightness(1.28f)
                    .interpolatedWith(juce::Colour(0xfff4f8ff), 0.12f);
                const auto rampDark = stripColor
                    .interpolatedWith(juce::Colour(0xff11161d), 0.35f)
                    .withMultipliedBrightness(0.74f);
                const float rampAlpha = 0.90f;
                const auto profileLine = juce::Colour(0xfff7fbff).withAlpha(0.72f);
                const auto clearedTopArea = juce::Colour(0xff1f2125).withAlpha(0.96f);
                const auto velocityAreaRect = juce::Rectangle<float>(stepRect.getX() + 1.0f,
                                                                     barAreaTop,
                                                                     juce::jmax(1.0f, stepRect.getWidth() - 2.0f),
                                                                     juce::jmax(1.0f, barAreaBottom - barAreaTop));

                g.setColour(clearedTopArea);
                g.fillRect(velocityAreaRect);

                juce::Graphics::ScopedSaveState scopedClip(g);
                g.reduceClipRegion(velocityAreaRect.getSmallestIntegerContainer());

                for (int s = 0; s < barCount; ++s)
                {
                    const float t = (barCount <= 1)
                        ? 1.0f
                        : (static_cast<float>(s) / static_cast<float>(barCount - 1));
                    const float velocity = juce::jlimit(0.0f, 1.0f, velocityStart + ((velocityEnd - velocityStart) * t));
                    const float shadeT = juce::jmap(velocity, 0.92f, 0.08f);
                    const float h = juce::jmax(1.0f, velocity * barAreaHeight);
                    const float x = stepRect.getX() + (slotWidth * static_cast<float>(s)) + 1.0f;
                    const float y = juce::jlimit(barAreaTop, barAreaBottom - 1.0f, barAreaBottom - h);

                    if (!suppressVelocityFillOverlay)
                    {
                        g.setColour(rampBright.interpolatedWith(rampDark, shadeT).withAlpha(rampAlpha));
                        g.fillRect(x, y, barWidth, barAreaBottom - y);
                    }

                    if (y > (barAreaTop + 0.6f))
                    {
                        g.setColour(profileLine);
                        g.drawLine(x, y, x + barWidth, y, 1.1f);
                    }
                }
            }

            if (selectedInSelectTool)
            {
                g.setColour(juce::Colours::white.withAlpha(0.96f));
                g.drawRect(stepRect.reduced(1.0f), 2.0f);
            }

            g.setColour(juce::Colour(0xffa8a8a8));
            g.setFont(stepHeight < 18.0f ? 8.0f : 10.0f);
            g.drawText(juce::String(i + 1), stepRect.toNearestInt(), juce::Justification::centred, false);

            if (subdivision > 1)
            {
                auto badge = stepRect.toNearestInt().reduced(2);
                g.setColour(juce::Colour(0xfff4f4f4).withAlpha(0.9f));
                g.setFont(stepHeight < 18.0f ? 7.0f : 8.0f);
                g.drawText("x" + juce::String(subdivision), badge, juce::Justification::bottomRight, false);
            }

            if (probability < 0.995f)
            {
                auto p = juce::jlimit(0, 100, static_cast<int>(std::round(probability * 100.0f)));
                auto pRect = stepRect.toNearestInt().reduced(2);
                g.setColour(stripColor.withMultipliedSaturation(1.20f).withMultipliedBrightness(1.20f).withAlpha(0.92f));
                g.setFont(stepHeight < 18.0f ? 7.0f : 8.0f);
                g.drawText(juce::String(p) + "%", pRect, juce::Justification::topRight, false);
            }

            if (isCurrentPlayStep)
            {
                g.setColour(juce::Colour(0xffffe7be).withAlpha(0.97f));
                g.drawRect(stepRect.reduced(0.5f), 2.0f);
                g.setColour(juce::Colour(0xffffb347).withAlpha(0.92f));
                g.fillRect(stepRect.withHeight(2.0f).reduced(1.0f, 0.0f));
            }
        }

        g.setColour(juce::Colour(0xff4f4f4f));
        for (int col = 4; col < stepsPerRow; col += 4)
        {
            const float x = grid.getX() + (col * stepWidth);
            g.drawLine(x, grid.getY(), x, grid.getBottom(), 1.5f);
        }

        if (numRows > 1)
        {
            g.setColour(juce::Colour(0xff1a1a1a));
            for (int row = 1; row < numRows; ++row)
            {
                const float y = grid.getY() + (row * stepHeight);
                g.drawLine(grid.getX(), y, grid.getRight(), y, 1.0f);
            }
        }

        if (playbackPosition >= 0.0f && playbackPosition <= 1.0f)
        {
            const float playheadX = grid.getX() + (playbackPosition * grid.getWidth());
            g.setColour(juce::Colour(0xffffb347).withAlpha(0.9f));
            g.drawLine(playheadX, grid.getY(), playheadX, grid.getY() + stepHeight, 2.0f);

            juce::Path triangle;
            triangle.addTriangle(playheadX - 5, grid.getY(), playheadX + 5, grid.getY(), playheadX, grid.getY() + 8.0f);
            g.setColour(juce::Colour(0xffffb347));
            g.fillPath(triangle);
        }
    }

    void toggleSelectToolShortcut()
    {
        if (activeTool == EditTool::Select)
        {
            activeTool = EditTool::Volume;
        }
        else
        {
            activeTool = EditTool::Select;
        }

        dragMode = DragMode::None;
        dragTool = activeTool;
        dragTargets.clear();
        dragAnchorStep = -1;
        dragAnchorRect = {};
        lastDrawStep = -1;
        lassoRect = {};
        selectClickCandidateStep = -1;
        selectLassoActivated = false;
        drawShiftToggleCandidateStep = -1;
        drawShiftToggleDragged = false;
        updateTimerState();
        repaint();
    }

    bool isSelectToggleShortcutDown(const juce::ModifierKeys& mods) const
    {
        return mods.isCommandDown() && mods.isShiftDown();
    }

    void handleSelectShortcutState(bool shortcutDown)
    {
        if (shortcutDown)
        {
            if (!selectShortcutLatched)
            {
                toggleSelectToolShortcut();
                selectShortcutLatched = true;
            }
            return;
        }

        selectShortcutLatched = false;
    }

    bool handleToolbarMouseDown(juce::Point<float> position)
    {
        if (!getToolbarBounds().contains(position))
            return false;

        const auto layout = getToolbarLayout();
        for (size_t i = 0; i < layout.toolButtons.size(); ++i)
        {
            if (!layout.toolButtons[i].contains(position))
                continue;

            const auto nextTool = toolFromIndex(static_cast<int>(i));
            if (nextTool != activeTool)
            {
                activeTool = nextTool;

                dragMode = DragMode::None;
                dragTool = activeTool;
                dragTargets.clear();
                dragAnchorStep = -1;
                dragAnchorRect = {};
                lastDrawStep = -1;
                lassoRect = {};
                selectClickCandidateStep = -1;
                selectLassoActivated = false;
                drawShiftToggleCandidateStep = -1;
                drawShiftToggleDragged = false;
                updateTimerState();
            }

            repaint();
            return true;
        }

        return true;
    }

    bool isDrawLikeTool(EditTool tool) const
    {
        return tool == EditTool::Draw || tool == EditTool::Volume;
    }

    void pruneSelectionToVisibleSteps()
    {
        for (auto it = selectedSteps.begin(); it != selectedSteps.end();)
        {
            if (*it < 0 || *it >= totalSteps)
                it = selectedSteps.erase(it);
            else
                ++it;
        }
        if (focusedStep >= totalSteps)
            focusedStep = totalSteps - 1;
    }

    void toggleSelection(int step)
    {
        if (selectedSteps.count(step) != 0)
            selectedSteps.erase(step);
        else
            selectedSteps.insert(step);
    }

    std::vector<int> toVector(const std::set<int>& values) const
    {
        return std::vector<int>(values.begin(), values.end());
    }

    std::vector<int> getEditTargetsForAnchor(int anchorStep) const
    {
        if (activeTool == EditTool::Select
            && !selectedSteps.empty()
            && selectedSteps.count(anchorStep) != 0)
            return toVector(selectedSteps);
        return { anchorStep };
    }

    void applyDrawVolumeAtStep(int stepIndex, int mouseY, bool beginDrag)
    {
        if (stepIndex < 0 || stepIndex >= totalSteps)
            return;

        const size_t stepIdx = static_cast<size_t>(stepIndex);
        const bool wasEnabled = stepPattern[stepIdx];
        if (!wasEnabled)
            setStepEnabled(stepIndex, true, true);

        const int subdivision = juce::jlimit(1, kMaxStepSubdivisions, stepSubdivisions[stepIdx]);
        const float startVelocity = juce::jlimit(0.0f, 1.0f, stepVelocityStart[stepIdx]);
        const float endVelocity = juce::jlimit(0.0f, 1.0f, stepVelocityEnd[stepIdx]);
        const bool hasDividerRampShape = wasEnabled
            && (subdivision > 1 || std::abs(endVelocity - startVelocity) > 0.001f);

        if (!hasDividerRampShape)
        {
            const float clickValue = getValueFromStepRectY(getStepRect(stepIndex), true, mouseY, true);
            setVelocityRange(stepIndex, clickValue, clickValue, true);
        }

        focusedStep = stepIndex;
        lastDrawStep = stepIndex;
        if (beginDrag)
            beginEdit(stepIndex, mouseY, EditTool::Volume);
    }

    void beginEdit(int anchorStep, int mouseY, EditTool toolForDrag)
    {
        dragMode = DragMode::Edit;
        dragTool = toolForDrag;
        dragStartY = mouseY;
        dragTargets = getEditTargetsForAnchor(anchorStep);
        dragAnchorStep = anchorStep;
        dragAnchorRect = getStepRect(anchorStep);
        if (dragAnchorRect.isEmpty() && !dragTargets.empty())
            dragAnchorRect = getStepRect(dragTargets.front());

        dragStartSubdivisions = stepSubdivisions;
        dragStartVelocityStart = stepVelocityStart;
        dragStartVelocityEnd = stepVelocityEnd;
        updateTimerState();
    }

    void beginLasso(juce::Point<float> startPos, bool additive)
    {
        dragMode = DragMode::Lasso;
        lassoAdditive = additive;
        lassoBaseSelection = selectedSteps;
        lassoStart = startPos;
        lassoRect = juce::Rectangle<float>(startPos.x, startPos.y, 0.0f, 0.0f);
        if (!lassoAdditive)
            selectedSteps.clear();
        updateTimerState();
    }

    void updateLasso(juce::Point<float> currentPos)
    {
        const float x0 = juce::jmin(lassoStart.x, currentPos.x);
        const float y0 = juce::jmin(lassoStart.y, currentPos.y);
        const float x1 = juce::jmax(lassoStart.x, currentPos.x);
        const float y1 = juce::jmax(lassoStart.y, currentPos.y);
        lassoRect = juce::Rectangle<float>(x0, y0, x1 - x0, y1 - y0);

        std::set<int> inside;
        for (int i = 0; i < totalSteps; ++i)
        {
            if (lassoRect.intersects(getStepRect(i)))
                inside.insert(i);
        }

        if (lassoAdditive)
        {
            selectedSteps = lassoBaseSelection;
            selectedSteps.insert(inside.begin(), inside.end());
        }
        else
        {
            selectedSteps = std::move(inside);
        }
    }

    float getValueFromStepRectY(const juce::Rectangle<float>& rect,
                                bool enabled,
                                int mouseY,
                                bool clampToUnit) const
    {
        if (rect.isEmpty() || rect.getHeight() <= 1.0f)
            return 1.0f;

        const float stepInnerTop = rect.getY() + 1.0f;
        const float probabilityTrackHeight = enabled
            ? juce::jmax(2.0f, juce::jmin(6.0f, rect.getHeight() * 0.14f))
            : 0.0f;
        const float velocityAreaTop = stepInnerTop + probabilityTrackHeight + (enabled ? 2.0f : 0.0f);
        const float areaTop = juce::jmin(rect.getBottom() - 2.0f, velocityAreaTop);
        const float areaBottom = rect.getBottom() - 1.0f;
        const float areaHeight = juce::jmax(1.0f, areaBottom - areaTop);
        const float value = 1.0f - ((static_cast<float>(mouseY) - areaTop) / areaHeight);

        if (clampToUnit)
            return juce::jlimit(0.0f, 1.0f, value);
        return value;
    }

    float getNormalizedDragValueFromY(int mouseY) const
    {
        const bool enabled = (dragAnchorStep >= 0 && dragAnchorStep < totalSteps)
            ? stepPattern[static_cast<size_t>(dragAnchorStep)]
            : true;
        return getValueFromStepRectY(dragAnchorRect, enabled, mouseY, true);
    }

    float getUnclampedDragValueFromY(int mouseY) const
    {
        const bool enabled = (dragAnchorStep >= 0 && dragAnchorStep < totalSteps)
            ? stepPattern[static_cast<size_t>(dragAnchorStep)]
            : true;
        return getValueFromStepRectY(dragAnchorRect, enabled, mouseY, false);
    }

    void applyContinuousTool(int mouseY)
    {
        const int deltaY = mouseY - dragStartY;
        const float valueFromY = getNormalizedDragValueFromY(mouseY);
        const float depth = valueFromY;
        const float dragHeight = juce::jmax(8.0f, dragAnchorRect.getHeight());
        const float dragShiftRaw = static_cast<float>(-deltaY) / dragHeight;
        const bool rampTool = (dragTool == EditTool::RampUp || dragTool == EditTool::RampDown);
        float rampOverflowShift = 0.0f;
        if (rampTool)
        {
            const float unclampedValueFromY = getUnclampedDragValueFromY(mouseY);
            const float overflowAbove = juce::jmax(0.0f, unclampedValueFromY - 1.0f);
            const float overflowBelow = juce::jmax(0.0f, -unclampedValueFromY);
            rampOverflowShift = overflowBelow - overflowAbove;
        }

        std::vector<int> orderedTargets = dragTargets;
        if (rampTool && orderedTargets.size() > 1)
            std::sort(orderedTargets.begin(), orderedTargets.end());

        const int targetCount = static_cast<int>(orderedTargets.size());
        const float volumeShift = dragShiftRaw;

        float rampMultiShift = rampOverflowShift;
        if (rampTool && targetCount > 1)
        {
            float baseLow = 1.0f;
            float baseHigh = 0.0f;
            bool hasBase = false;

            for (int targetIndex = 0; targetIndex < targetCount; ++targetIndex)
            {
                const int step = orderedTargets[static_cast<size_t>(targetIndex)];
                if (step < 0 || step >= totalSteps)
                    continue;

                const size_t idx = static_cast<size_t>(step);
                const float baseStart = juce::jlimit(0.0f, 1.0f, dragStartVelocityStart[idx]);
                const float baseEnd = juce::jlimit(0.0f, 1.0f, dragStartVelocityEnd[idx]);
                const float baseMax = juce::jmax(baseStart, baseEnd);
                const float t0 = static_cast<float>(targetIndex) / static_cast<float>(targetCount);
                const float t1 = static_cast<float>(targetIndex + 1) / static_cast<float>(targetCount);

                float segmentStart = 0.0f;
                float segmentEnd = 0.0f;
                if (dragTool == EditTool::RampUp)
                {
                    segmentStart = ((1.0f - depth) + (depth * t0)) * baseMax;
                    segmentEnd = ((1.0f - depth) + (depth * t1)) * baseMax;
                }
                else
                {
                    segmentStart = (1.0f - (depth * t0)) * baseMax;
                    segmentEnd = (1.0f - (depth * t1)) * baseMax;
                }

                baseLow = juce::jmin(baseLow, juce::jmin(segmentStart, segmentEnd));
                baseHigh = juce::jmax(baseHigh, juce::jmax(segmentStart, segmentEnd));
                hasBase = true;
            }

            if (hasBase)
            {
                const float minShift = -baseLow;
                const float maxShift = 1.0f - baseHigh;
                rampMultiShift = juce::jlimit(minShift, maxShift, rampOverflowShift);
            }
        }

        for (int targetIndex = 0; targetIndex < targetCount; ++targetIndex)
        {
            const int step = orderedTargets[static_cast<size_t>(targetIndex)];
            if (step < 0 || step >= totalSteps)
                continue;

            switch (dragTool)
            {
                case EditTool::Divide:
                {
                    const int base = dragStartSubdivisions[static_cast<size_t>(step)];
                    const int next = juce::jlimit(1, kMaxStepSubdivisions, base + ((-deltaY) / 14));
                    setSubdivision(step, next, true);
                    break;
                }

                case EditTool::Volume:
                {
                    const size_t idx = static_cast<size_t>(step);
                    const float baseStart = juce::jlimit(0.0f, 1.0f, dragStartVelocityStart[idx]);
                    const float baseEnd = juce::jlimit(0.0f, 1.0f, dragStartVelocityEnd[idx]);
                    const float newStart = juce::jlimit(0.0f, 1.0f, baseStart + volumeShift);
                    const float newEnd = juce::jlimit(0.0f, 1.0f, baseEnd + volumeShift);

                    setVelocityRange(step, newStart, newEnd, true);
                    break;
                }

                case EditTool::RampUp:
                {
                    const size_t idx = static_cast<size_t>(step);
                    const float baseStart = juce::jlimit(0.0f, 1.0f, dragStartVelocityStart[idx]);
                    const float baseEnd = juce::jlimit(0.0f, 1.0f, dragStartVelocityEnd[idx]);
                    const float baseMax = juce::jmax(baseStart, baseEnd);
                    const float rampShift = (targetCount > 1) ? rampMultiShift : rampOverflowShift;
                    if (dragStartSubdivisions[idx] <= 1)
                    {
                        const int autoSubdivision = juce::jlimit(
                            kAutoRampSubdivision,
                            kMaxStepSubdivisions,
                            kAutoRampSubdivision + (std::abs(deltaY) / 14));
                        setSubdivision(step, autoSubdivision, true);
                    }

                    if (targetCount > 1)
                    {
                        const float t0 = static_cast<float>(targetIndex) / static_cast<float>(targetCount);
                        const float t1 = static_cast<float>(targetIndex + 1) / static_cast<float>(targetCount);
                        const float start = juce::jlimit(0.0f, 1.0f,
                                                         ((((1.0f - depth) + (depth * t0)) * baseMax)
                                                          + rampShift));
                        const float end = juce::jlimit(0.0f, 1.0f,
                                                       ((((1.0f - depth) + (depth * t1)) * baseMax)
                                                        + rampShift));
                        setVelocityRange(step, start, end, true);
                    }
                    else
                    {
                        const float start = juce::jlimit(0.0f, 1.0f, (((1.0f - depth) * baseMax) + rampShift));
                        const float end = juce::jlimit(0.0f, 1.0f, baseMax + rampShift);
                        setVelocityRange(step, start, end, true);
                    }
                    break;
                }

                case EditTool::RampDown:
                {
                    const size_t idx = static_cast<size_t>(step);
                    const float baseStart = juce::jlimit(0.0f, 1.0f, dragStartVelocityStart[idx]);
                    const float baseEnd = juce::jlimit(0.0f, 1.0f, dragStartVelocityEnd[idx]);
                    const float baseMax = juce::jmax(baseStart, baseEnd);
                    const float rampShift = (targetCount > 1) ? rampMultiShift : rampOverflowShift;
                    if (dragStartSubdivisions[idx] <= 1)
                    {
                        const int autoSubdivision = juce::jlimit(
                            kAutoRampSubdivision,
                            kMaxStepSubdivisions,
                            kAutoRampSubdivision + (std::abs(deltaY) / 14));
                        setSubdivision(step, autoSubdivision, true);
                    }

                    if (targetCount > 1)
                    {
                        const float t0 = static_cast<float>(targetIndex) / static_cast<float>(targetCount);
                        const float t1 = static_cast<float>(targetIndex + 1) / static_cast<float>(targetCount);
                        const float start = juce::jlimit(0.0f, 1.0f,
                                                         (((1.0f - (depth * t0)) * baseMax)
                                                          + rampShift));
                        const float end = juce::jlimit(0.0f, 1.0f,
                                                       (((1.0f - (depth * t1)) * baseMax)
                                                        + rampShift));
                        setVelocityRange(step, start, end, true);
                    }
                    else
                    {
                        const float start = juce::jlimit(0.0f, 1.0f, baseMax + rampShift);
                        const float end = juce::jlimit(0.0f, 1.0f, (((1.0f - depth) * baseMax) + rampShift));
                        setVelocityRange(step, start, end, true);
                    }
                    break;
                }

                case EditTool::Probability:
                {
                    setProbability(step, valueFromY, true);
                    break;
                }

                case EditTool::Draw:
                case EditTool::Select:
                default:
                    break;
            }
        }

        repaint();
    }

    void updateTimerState()
    {
        const bool shouldRun = isPlaying || dragMode != DragMode::None;
        if (shouldRun)
        {
            if (!isTimerRunning())
                startTimer(50);
        }
        else if (isTimerRunning())
        {
            stopTimer();
        }
    }

    void resetStepToDefaults(int step, bool notify)
    {
        if (step < 0 || step >= totalSteps)
            return;

        const size_t idx = static_cast<size_t>(step);
        const bool wasEnabled = stepPattern[idx];
        stepPattern[idx] = false;
        stepSubdivisions[idx] = 1;
        stepVelocityStart[idx] = 1.0f;
        stepVelocityEnd[idx] = 1.0f;
        stepProbability[idx] = 1.0f;

        if (!notify)
            return;

        if (onStepSet)
            onStepSet(step, false);
        else if (onStepClicked && wasEnabled)
            onStepClicked(step);

        if (onStepSubdivisionSet)
            onStepSubdivisionSet(step, 1);
        if (onStepVelocityRangeSet)
            onStepVelocityRangeSet(step, 1.0f, 1.0f);
        if (onStepProbabilitySet)
            onStepProbabilitySet(step, 1.0f);
    }

    void setStepEnabled(int step, bool enabled, bool notify)
    {
        if (step < 0 || step >= totalSteps)
            return;

        const size_t idx = static_cast<size_t>(step);
        if (stepPattern[idx] == enabled)
            return;

        stepPattern[idx] = enabled;

        if (notify)
        {
            if (onStepSet)
                onStepSet(step, enabled);
            else if (onStepClicked)
                onStepClicked(step);
        }
    }

    void setSubdivision(int step, int subdivision, bool notify)
    {
        if (step < 0 || step >= totalSteps)
            return;

        const int clamped = juce::jlimit(1, kMaxStepSubdivisions, subdivision);
        size_t idx = static_cast<size_t>(step);
        if (stepSubdivisions[idx] == clamped)
            return;

        stepSubdivisions[idx] = clamped;
        if (notify && onStepSubdivisionSet)
            onStepSubdivisionSet(step, clamped);
    }

    void setVelocityRange(int step, float startVelocity, float endVelocity, bool notify)
    {
        if (step < 0 || step >= totalSteps)
            return;

        const float s = juce::jlimit(0.0f, 1.0f, startVelocity);
        const float e = juce::jlimit(0.0f, 1.0f, endVelocity);
        size_t idx = static_cast<size_t>(step);
        if (std::abs(stepVelocityStart[idx] - s) < 0.001f
            && std::abs(stepVelocityEnd[idx] - e) < 0.001f)
            return;

        stepVelocityStart[idx] = s;
        stepVelocityEnd[idx] = e;
        if (notify && onStepVelocityRangeSet)
            onStepVelocityRangeSet(step, s, e);
    }

    void setProbability(int step, float probability, bool notify)
    {
        if (step < 0 || step >= totalSteps)
            return;

        const float p = juce::jlimit(0.0f, 1.0f, probability);
        const size_t idx = static_cast<size_t>(step);
        if (std::abs(stepProbability[idx] - p) < 0.001f)
            return;

        stepProbability[idx] = p;
        if (notify && onStepProbabilitySet)
            onStepProbabilitySet(step, p);
    }

    void resetOneStep(int step)
    {
        if (step < 0 || step >= totalSteps)
            return;

        resetStepToDefaults(step, true);
        repaint();
    }

    void resetManySteps(const std::vector<int>& steps)
    {
        for (const int step : steps)
            resetStepToDefaults(step, true);
        selectedSteps.clear();
        repaint();
    }

    void showContextMenuForStep(int stepIndex)
    {
        juce::PopupMenu menu;
        menu.addItem(1, stepPattern[static_cast<size_t>(stepIndex)] ? "Disable Step" : "Enable Step");
        menu.addItem(2, "Divide x2");
        menu.addItem(3, "Divide x4");
        menu.addSeparator();
        menu.addItem(4, "Reset Step");
        menu.addItem(5, "Reset Selected", !selectedSteps.empty());
        menu.addSeparator();
        menu.addItem(10, "Probability 100%");
        menu.addItem(11, "Probability 75%");
        menu.addItem(12, "Probability 50%");
        menu.addItem(13, "Probability 25%");

        const int result = menu.show();
        switch (result)
        {
            case 1:
                setStepEnabled(stepIndex, !stepPattern[static_cast<size_t>(stepIndex)], true);
                break;
            case 2:
                setSubdivision(stepIndex, 2, true);
                break;
            case 3:
                setSubdivision(stepIndex, 4, true);
                break;
            case 4:
                resetOneStep(stepIndex);
                break;
            case 5:
                if (!selectedSteps.empty())
                    resetManySteps(toVector(selectedSteps));
                break;
            case 10:
                setProbability(stepIndex, 1.0f, true);
                break;
            case 11:
                setProbability(stepIndex, 0.75f, true);
                break;
            case 12:
                setProbability(stepIndex, 0.5f, true);
                break;
            case 13:
                setProbability(stepIndex, 0.25f, true);
                break;
            default:
                break;
        }

        repaint();
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StepSequencerDisplay)
};
