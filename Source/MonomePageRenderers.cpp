#include "PluginProcessor.h"
#include "MonomeFileBrowserActions.h"
#include "MonomeFilterActions.h"
#include "MonomeGroupAssignActions.h"
#include "MonomeMixActions.h"
#include <cmath>
#include <vector>

namespace
{
constexpr int kMonomeModPrevColumn = 14;
constexpr int kMonomeModNextColumn = 15;
}

bool MlrVSTAudioProcessor::renderMonomeControlPageStripRow(const MonomeLayoutState& layout,
                                                           EnhancedAudioStrip& strip,
                                                           int stripIndex,
                                                           int row,
                                                           int newLedState[MaxGridWidth][MaxGridHeight],
                                                           bool scenePageActive,
                                                           bool fastBlinkOn,
                                                           bool slowBlinkOn,
                                                           double beatNow)
{
    if (!audioEngine || !controlModeActive || currentControlMode == ControlMode::Normal)
        return false;

    const bool simpleMixRenderMode =
        currentControlMode == ControlMode::Speed
        || currentControlMode == ControlMode::Pitch
        || currentControlMode == ControlMode::Pan
        || currentControlMode == ControlMode::Volume
        || currentControlMode == ControlMode::Swing
        || currentControlMode == ControlMode::Gate;

    if (simpleMixRenderMode)
    {
        MonomeMixActions::renderRow(strip, *this, row, newLedState, static_cast<int>(currentControlMode));
        return true;
    }

    if (currentControlMode == ControlMode::GrainSize)
    {
        const int targetStripIndex = layout.clampVisibleStrip(getLastMonomePressedStripRow());
        if (auto* targetStrip = audioEngine->getStrip(targetStripIndex))
            MonomeMixActions::renderGrainPageRow(*targetStrip, stripIndex, row, newLedState);
        return true;
    }

    if (currentControlMode == ControlMode::Filter)
    {
        MonomeFilterActions::renderRow(strip, row, newLedState, static_cast<int>(filterSubPage));
        return true;
    }

    if (currentControlMode == ControlMode::Delay)
    {
        const int targetStripIndex = layout.clampVisibleStrip(getLastMonomePressedStripRow());
        if (auto* targetStrip = audioEngine->getStrip(targetStripIndex))
            MonomeMixActions::renderDelayPageRow(*targetStrip, stripIndex, row, newLedState);
        return true;
    }

    if (currentControlMode == ControlMode::FileBrowser)
    {
        MonomeFileBrowserActions::renderRow(*this, *audioEngine, strip, stripIndex, row, newLedState);
        return true;
    }

    if (currentControlMode == ControlMode::GroupAssign)
    {
        if (scenePageActive)
        {
            const bool isSelectedStrip = stripIndex == layout.clampVisibleStrip(getLastMonomePressedStripRow());

            for (int x = 0; x < 16; ++x)
                newLedState[x][row] = 1;

            if (isSelectedStrip)
            {
                for (int x = 4; x <= 11; ++x)
                    newLedState[x][row] = 15;

                newLedState[0][row] = 8;
                newLedState[15][row] = 8;
            }
        }
        else
        {
            MonomeGroupAssignActions::renderRow(strip, row, newLedState);
        }
        return true;
    }

    if (currentControlMode != ControlMode::Modulation)
        return false;

    const int modulationMaxRow = juce::jmax(1, layout.visibleStripCount);
    const int sceneAutomationTopRow = layout.groupRow + 1;
    const int sceneAutomationBottomRow = juce::jmax(sceneAutomationTopRow, modulationMaxRow);
    const int sceneAutomationUsableRows = juce::jmax(1, sceneAutomationBottomRow - sceneAutomationTopRow);
    const int selectedStrip = layout.clampVisibleStrip(getLastMonomePressedStripRow());

    const auto sceneAutomationValueToRow =
        [sceneAutomationTopRow, sceneAutomationBottomRow, sceneAutomationUsableRows](float v)
    {
        return juce::jlimit(sceneAutomationTopRow,
                            sceneAutomationBottomRow,
                            sceneAutomationTopRow + static_cast<int>(std::round((1.0f - juce::jlimit(0.0f, 1.0f, v))
                                                                               * static_cast<float>(sceneAutomationUsableRows))));
    };

    const auto sceneAutomationRowValue01 =
        [sceneAutomationTopRow, sceneAutomationBottomRow, sceneAutomationUsableRows](int sourceRow)
    {
        return juce::jlimit(0.0f,
                            1.0f,
                            static_cast<float>(sceneAutomationBottomRow - juce::jlimit(sceneAutomationTopRow,
                                                                                       sceneAutomationBottomRow,
                                                                                       sourceRow))
                                / static_cast<float>(sceneAutomationUsableRows));
    };

    if (isSceneMainAutomationMonomePageActive())
    {
        const int sceneSlot = getFocusedSceneSlot();
        const auto target = getActiveSceneMainAutomationTargetForMonome(selectedStrip);
        const bool targetBipolar = isSceneAutomationTargetBipolarForMonome(target);
        const double lengthBeats = juce::jmax(1.0, getResolvedSceneLengthBeats(sceneSlot));
        const auto events = getScenePerformanceEventsSnapshot(sceneSlot);
        int activeStep = -1;
        const int displayPage = getSceneMainAutomationDisplayPageForMonome(sceneSlot, beatNow);
        getSceneMainAutomationPlaybackStepForMonome(sceneSlot, displayPage, beatNow, activeStep);

        const int modulationBaseRow = targetBipolar ? sceneAutomationValueToRow(0.5f)
                                                    : sceneAutomationBottomRow;
        const auto stepLevelForRow = [&](int sourceRow, int pointRow, int baseRow)
        {
            const int minRow = juce::jmin(baseRow, pointRow);
            const int maxRow = juce::jmax(baseRow, pointRow);
            if (sourceRow < minRow || sourceRow > maxRow)
                return 0;

            const float barRange = static_cast<float>(std::abs(pointRow - baseRow));
            const float fromBase = static_cast<float>(std::abs(sourceRow - baseRow));
            const float t = (barRange > 0.0f) ? (fromBase / barRange) : 1.0f;
            const float shapedT = std::pow(juce::jlimit(0.0f, 1.0f, t), 0.72f);
            const float rowValue01 = sceneAutomationRowValue01(sourceRow);
            const float minLevel = targetBipolar ? 3.0f : 2.0f;
            const float maxLevel = 9.0f + (4.0f * rowValue01);
            int level = static_cast<int>(std::round(minLevel + ((maxLevel - minLevel) * shapedT)));
            if (sourceRow == pointRow)
                level += 2;
            return juce::jlimit(1, 15, level);
        };

        for (int x = 0; x < 16; ++x)
        {
            newLedState[x][row] = 0;
            float normalizedValue = 0.0f;
            if (!getSceneMainAutomationColumnValueForMonome(events,
                                                            sceneSlot,
                                                            selectedStrip,
                                                            target,
                                                            x,
                                                            beatNow,
                                                            lengthBeats,
                                                            normalizedValue))
            {
                continue;
            }

            const int pointRow = sceneAutomationValueToRow(normalizedValue);
            newLedState[x][row] = stepLevelForRow(row, pointRow, modulationBaseRow);

            if (activeStep == x)
                newLedState[x][row] = juce::jmax(newLedState[x][row], 15);
        }
        return true;
    }

    const auto seq = audioEngine->getModSequencerState(selectedStrip);
    const int activeGlobalStep = audioEngine->getModCurrentGlobalStep(selectedStrip);
    const int playbackPage = juce::jlimit(
        0,
        ModernAudioEngine::MaxModBars - 1,
        activeGlobalStep / ModernAudioEngine::ModSteps);
    const int activeStep = (playbackPage == seq.editPage)
        ? (activeGlobalStep % ModernAudioEngine::ModSteps)
        : -1;
    const bool stripPlaying = audioEngine->getStrip(selectedStrip)
        && audioEngine->getStrip(selectedStrip)->isPlaying();

    const auto baseRowForColumn = [&](int column)
    {
        if (column == kMonomeModPrevColumn || column == kMonomeModNextColumn)
            return seq.bipolar ? (1 + ((modulationMaxRow - 1) / 2)) : modulationMaxRow;
        return seq.bipolar ? (modulationMaxRow / 2) : modulationMaxRow;
    };

    const auto valueToRow = [&](float v, int column)
    {
        v = juce::jlimit(0.0f, 1.0f, v);
        const bool reservedTopCell = (column == kMonomeModPrevColumn || column == kMonomeModNextColumn);
        const int topRow = reservedTopCell ? 1 : 0;
        const int usableRows = reservedTopCell ? juce::jmax(1, modulationMaxRow - 1) : modulationMaxRow;
        if (seq.bipolar)
        {
            const float signedV = (v * 2.0f) - 1.0f;
            const float n = (signedV + 1.0f) * 0.5f;
            return juce::jlimit(topRow,
                                modulationMaxRow,
                                topRow + static_cast<int>(std::round((1.0f - n) * usableRows)));
        }
        return juce::jlimit(topRow,
                            modulationMaxRow,
                            topRow + static_cast<int>(std::round((1.0f - v) * usableRows)));
    };

    const auto curveLevelForRow = [&](int sourceRow, bool isPoint)
    {
        const float rowValue01 = juce::jlimit(0.0f, 1.0f, 1.0f - (static_cast<float>(sourceRow) / static_cast<float>(modulationMaxRow)));
        const int base = juce::jlimit(1, 15, static_cast<int>(std::round(juce::jmap(rowValue01, 2.0f, 12.0f))));
        return juce::jlimit(1, 15, isPoint ? (base + 2) : base);
    };

    const auto stepLevelForRow = [&](int sourceRow, int pointRow, int baseRow)
    {
        const int minRow = juce::jmin(baseRow, pointRow);
        const int maxRow = juce::jmax(baseRow, pointRow);
        if (sourceRow < minRow || sourceRow > maxRow)
            return 0;

        const float barRange = static_cast<float>(std::abs(pointRow - baseRow));
        const float fromBase = static_cast<float>(std::abs(sourceRow - baseRow));
        const float t = (barRange > 0.0f) ? (fromBase / barRange) : 1.0f;
        const float shapedT = std::pow(juce::jlimit(0.0f, 1.0f, t), 0.72f);

        const float rowValue01 = juce::jlimit(0.0f, 1.0f, 1.0f - (static_cast<float>(sourceRow) / static_cast<float>(modulationMaxRow)));
        const float minLevel = seq.bipolar ? 3.0f : 2.0f;
        const float maxLevel = 9.0f + (4.0f * rowValue01);
        int level = static_cast<int>(std::round(minLevel + ((maxLevel - minLevel) * shapedT)));
        if (sourceRow == pointRow)
            level += 2;
        return juce::jlimit(1, 15, level);
    };

    for (int x = 0; x < 16; ++x)
    {
        newLedState[x][row] = 0;
        const float v = seq.steps[static_cast<size_t>(x)];
        const int pointRow = valueToRow(v, x);
        const int modulationBaseRow = baseRowForColumn(x);

        if (seq.curveMode)
        {
            int level = 0;
            if (row == pointRow)
                level = juce::jmax(level, curveLevelForRow(row, true));
            if (x < 15)
            {
                const int nextRow = valueToRow(seq.steps[static_cast<size_t>(x + 1)], x + 1);
                const int minRow = juce::jmin(pointRow, nextRow);
                const int maxRow = juce::jmax(pointRow, nextRow);
                if (row >= minRow && row <= maxRow)
                    level = juce::jmax(level, curveLevelForRow(row, false));
            }
            newLedState[x][row] = level;
        }
        else
        {
            newLedState[x][row] = stepLevelForRow(row, pointRow, modulationBaseRow);
        }

        if (stripPlaying && x == activeStep)
            newLedState[x][row] = juce::jmax(newLedState[x][row], 15);
    }

    (void) fastBlinkOn;
    (void) slowBlinkOn;
    return true;
}

void MlrVSTAudioProcessor::renderMonomePlaybackStripRow(EnhancedAudioStrip& strip,
                                                        int stripIndex,
                                                        int row,
                                                        int newLedState[MaxGridWidth][MaxGridHeight],
                                                        bool isGroupMuted,
                                                        bool fastBlinkOn,
                                                        bool slowBlinkOn)
{
    const auto playMode = strip.getPlayMode();

    if (playMode == EnhancedAudioStrip::PlayMode::Step)
    {
        const auto visiblePattern = strip.getVisibleStepPattern();
        const int visibleCurrentStep = strip.getVisibleCurrentStep();
        const bool stepTransportActive = strip.isPlaying();
        for (int x = 0; x < 16; ++x)
        {
            const bool isCurrentStep = (x == visibleCurrentStep);
            const bool isActiveStep = visiblePattern[static_cast<size_t>(x)];
            int level = isActiveStep ? 15 : 0;

            if (isCurrentStep && stepTransportActive)
                level = fastBlinkOn ? 15 : 0;

            newLedState[x][row] = level;
        }
        return;
    }

    if (playMode == EnhancedAudioStrip::PlayMode::Sample)
    {
        for (int x = 0; x < 16; ++x)
            newLedState[x][row] = 0;

        if (auto* sampleEngine = getSampleModeEngine(stripIndex, false))
        {
            const auto snapshot = sampleEngine->getStateSnapshot();
            const bool legacyLoopFeedback = snapshot.useLegacyLoopEngine
                || strip.isSampleModeLegacyLoopEngineEnabled();
            const bool loopTriggerMode = snapshot.triggerMode == SampleTriggerMode::Loop;
            const int heldSlot = getSampleModeHeldVisibleSliceSlot(stripIndex);
            int playbackSlot = -1;
            if (!legacyLoopFeedback && snapshot.playbackProgress >= 0.0f)
            {
                for (int x = 0; x < 16; ++x)
                {
                    const auto& slice = snapshot.visibleSlices[static_cast<size_t>(x)];
                    if (slice.id < 0)
                        continue;

                    const bool isLastSlice = (x == 15);
                    if (snapshot.playbackProgress >= slice.normalizedStart
                        && (snapshot.playbackProgress < slice.normalizedEnd
                            || (isLastSlice && snapshot.playbackProgress <= slice.normalizedEnd)))
                    {
                        playbackSlot = x;
                        break;
                    }
                }
            }

            for (int x = 0; x < 16; ++x)
            {
                const auto& slice = snapshot.visibleSlices[static_cast<size_t>(x)];
                if (slice.id >= 0)
                    newLedState[x][row] = legacyLoopFeedback
                        ? (strip.isPlaying() ? 4 : 2)
                        : (snapshot.isPlaying ? (loopTriggerMode ? 8 : 6)
                                              : (loopTriggerMode ? 5 : 3));
            }

            if (snapshot.pendingVisibleSliceSlot >= 0 && snapshot.pendingVisibleSliceSlot < 16)
                newLedState[snapshot.pendingVisibleSliceSlot][row] = juce::jmax(newLedState[snapshot.pendingVisibleSliceSlot][row],
                                                                                loopTriggerMode ? 12 : 10);
            if (!legacyLoopFeedback && heldSlot >= 0 && heldSlot < 16)
                newLedState[heldSlot][row] = juce::jmax(newLedState[heldSlot][row], loopTriggerMode ? 11 : 9);

            if (legacyLoopFeedback)
            {
                const int currentCol = strip.getCurrentColumn();
                if (!isGroupMuted
                    && strip.isPlaying()
                    && currentCol >= 0
                    && currentCol < 16)
                {
                    newLedState[currentCol][row] = 15;
                }
            }
            else if (!isGroupMuted
                && playbackSlot >= 0
                && playbackSlot < 16)
            {
                newLedState[playbackSlot][row] = 15;
            }
            else if (!isGroupMuted
                && snapshot.activeVisibleSliceSlot >= 0
                && snapshot.activeVisibleSliceSlot < 16)
            {
                newLedState[snapshot.activeVisibleSliceSlot][row] = 15;
            }
        }
        return;
    }

    if (playMode == EnhancedAudioStrip::PlayMode::Grain)
    {
        const int anchor = strip.getGrainAnchorColumn();
        const int secondary = strip.getGrainSecondaryColumn();
        const int sizeControl = strip.getGrainSizeControlColumn();
        const int heldCount = strip.getGrainHeldCount();
        const int currentCol = strip.getCurrentColumn();
        const auto preview = strip.getGrainPreviewPositions();
        const bool showScratchTrail = strip.isPlaying()
            || (heldCount > 0)
            || (strip.isScratchActive())
            || (strip.getDisplaySpeed() > 0.01f);

        const auto setLevelMax = [&](int x, int level)
        {
            if (x < 0 || x >= 16)
                return;
            newLedState[x][row] = juce::jmax(newLedState[x][row], level);
        };

        if (heldCount <= 0 && !showScratchTrail)
        {
            for (int x = 0; x < 16; ++x)
                newLedState[x][row] = 0;
            if (!isGroupMuted && strip.isPlaying() && currentCol >= 0 && currentCol < 16)
                newLedState[currentCol][row] = 15;
        }
        else
        {
            for (int x = 0; x < 16; ++x)
                newLedState[x][row] = 0;

            for (const float p : preview)
            {
                if (!std::isfinite(p) || p < 0.0f || p > 1.0f)
                    continue;

                const int px = juce::jlimit(0, 15, static_cast<int>(std::round(p * 15.0f)));
                const int dotLevel = (heldCount > 0) ? 11 : 8;
                setLevelMax(px, dotLevel);
            }

            if (!isGroupMuted && strip.isPlaying() && currentCol >= 0 && currentCol < 16)
                setLevelMax(currentCol, 7);
            if (secondary >= 0 && secondary < 16)
                setLevelMax(secondary, 13);
            if (sizeControl >= 0 && sizeControl < 16)
                setLevelMax(sizeControl, fastBlinkOn ? 15 : 3);
            if (anchor >= 0 && anchor < 16)
                setLevelMax(anchor, slowBlinkOn ? 15 : 10);
        }
        return;
    }

    if (!isGroupMuted && strip.isPlaying())
    {
        const int loopStart = juce::jlimit(0, 15, strip.getLoopStart());
        const int loopEnd = juce::jlimit(loopStart + 1, 16, strip.getLoopEnd());
        for (int x = loopStart; x < loopEnd && x < 16; ++x)
            newLedState[x][row] = 2;

        const int currentCol = strip.getCurrentColumn();
        if (currentCol >= 0 && currentCol < 16)
            newLedState[currentCol][row] = 15;
    }
}
