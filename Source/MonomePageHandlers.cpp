#include "PluginProcessor.h"
#include "MonomeFileBrowserActions.h"
#include "MonomeGroupAssignActions.h"
#include "MonomeMixActions.h"
#include <cmath>
#include <functional>

namespace
{
constexpr int kMonomeModPrevColumn = 14;
constexpr int kMonomeModNextColumn = 15;

float quantizeMonomeRearrangeValue(float value01)
{
    return juce::jlimit(0.0f, 1.0f,
                        std::round(juce::jlimit(0.0f, 1.0f, value01)
                                   * static_cast<float>(ModernAudioEngine::MaxColumns - 1))
                            / static_cast<float>(juce::jmax(1, ModernAudioEngine::MaxColumns - 1)));
}
}

bool MlrVSTAudioProcessor::handleMonomeControlPageStripPress(const MonomeLayoutState& layout,
                                                             EnhancedAudioStrip& strip,
                                                             int stripIndex,
                                                             int row,
                                                             int column,
                                                             bool scenePageActive)
{
    if (!audioEngine || !controlModeActive || currentControlMode == ControlMode::Normal)
        return false;

    const int CONTROL_ROW = layout.controlRow;
    const int visibleStripCount = layout.visibleStripCount;
    const int modulationRowsDenom = layout.modulationRowsDenom;

    const auto sceneStepMotionPageNavigationActive = [&]()
    {
        const bool sceneStepMotionMonomePageActive = isControlModeActive()
            && getCurrentControlMode() == ControlMode::Modulation;
        return isSceneModeEnabled()
            && getSceneModPageMode() == SceneModPageMode::StepMotion
            && (isSceneStepMotionEditorOpen() || sceneStepMotionMonomePageActive);
    };

    const auto modulationRowToUnitForColumn = [&](int sourceRow, int sourceColumn)
    {
        const bool modulationPageActive = isControlModeActive()
            && getCurrentControlMode() == ControlMode::Modulation;
        const bool reservedTopCell = modulationPageActive
            ? (sourceColumn == kMonomeModPrevColumn || sourceColumn == kMonomeModNextColumn)
            : (sceneStepMotionPageNavigationActive()
                ? (sourceColumn == kMonomeModPrevColumn || sourceColumn == kMonomeModNextColumn)
                : (sourceColumn == kMonomeModNextColumn));
        const int denom = reservedTopCell
            ? juce::jmax(1, visibleStripCount - 1)
            : modulationRowsDenom;
        return juce::jlimit(0.0f, 1.0f, static_cast<float>((CONTROL_ROW - 1) - sourceRow)
            / static_cast<float>(denom));
    };

    const auto sceneMainAutomationRowToUnit = [GROUP_ROW = layout.groupRow, CONTROL_ROW](int sourceRow)
    {
        const int topWritableRow = GROUP_ROW + 1;
        const int bottomWritableRow = juce::jmax(topWritableRow, CONTROL_ROW - 1);
        const int clampedRow = juce::jlimit(topWritableRow, bottomWritableRow, sourceRow);
        const int denom = juce::jmax(1, bottomWritableRow - topWritableRow);
        return juce::jlimit(0.0f,
                            1.0f,
                            static_cast<float>(bottomWritableRow - clampedRow)
                                / static_cast<float>(denom));
    };

    const auto syncMonomeModEditPageToPlayback = [&](int targetStrip)
    {
        if (sceneStepMotionPageNavigationActive() || isSceneMainAutomationMonomePageActive())
            return false;

        if (targetStrip < 0 || targetStrip >= MaxStrips)
            return false;

        auto* target = audioEngine->getStrip(targetStrip);
        if (target == nullptr || !target->isPlaying())
            return false;

        const int playbackPage = audioEngine->getModCurrentPage(targetStrip);
        if (audioEngine->getModEditPage(targetStrip) != playbackPage)
            audioEngine->setModEditPage(targetStrip, playbackPage);
        return true;
    };

    const auto handleSimpleMixControlPress = [&]()
    {
        MonomeMixActions::handleButtonPress(*this, strip, stripIndex, column, static_cast<int>(currentControlMode));
        recordMonomeControlPatternEvent(currentControlMode, stripIndex, -1, column);
    };

    const auto applySceneAwareMonomeControl =
        [this, stripIndex, column](ScenePerformanceControlTarget target,
                                   ControlMode controlMode,
                                   int controlRow,
                                   float value,
                                   const std::function<void(StripControlWriteMode)>& applyWrite)
    {
        applyLiveSceneControlTouch(stripIndex,
                                   target,
                                   controlMode,
                                   controlRow,
                                   value,
                                   column,
                                   applyWrite);
    };

    switch (currentControlMode)
    {
        case ControlMode::Speed:
        case ControlMode::Pitch:
        case ControlMode::Swing:
        case ControlMode::Gate:
            handleSimpleMixControlPress();
            return true;

        case ControlMode::Pan:
        {
            float pan = (column - 8) / 8.0f;
            pan = juce::jlimit(-1.0f, 1.0f, pan);
            applySceneAwareMonomeControl(ScenePerformanceControlTarget::Pan,
                                         ControlMode::Pan,
                                         0,
                                         pan,
                                         [this, stripIndex, pan](StripControlWriteMode writeMode)
                                         {
                                             setStripPanControlValue(stripIndex, pan, writeMode);
                                         });
            return true;
        }

        case ControlMode::Volume:
        {
            const float volume = juce::jlimit(0.0f, 1.0f, column / 15.0f);
            applySceneAwareMonomeControl(ScenePerformanceControlTarget::Volume,
                                         ControlMode::Volume,
                                         0,
                                         volume,
                                         [this, stripIndex, volume](StripControlWriteMode writeMode)
                                         {
                                             setStripVolumeControlValue(stripIndex, volume, writeMode);
                                         });
            return true;
        }

        case ControlMode::GrainSize:
        {
            const int targetStripIndex = layout.clampVisibleStrip(getLastMonomePressedStripRow());
            if (auto* targetStrip = audioEngine->getStrip(targetStripIndex))
            {
                MonomeMixActions::handleGrainPageButtonPress(*this,
                                                             *targetStrip,
                                                             targetStripIndex,
                                                             stripIndex,
                                                             column);
                recordMonomeControlPatternEvent(currentControlMode, targetStripIndex, stripIndex, column);
            }
            return true;
        }

        case ControlMode::Filter:
            if (filterSubPage == FilterSubPage::Frequency)
            {
                const float t = juce::jlimit(0.0f, 1.0f, column / 15.0f);
                const float frequency = 20.0f * std::pow(1000.0f, t);
                applySceneAwareMonomeControl(ScenePerformanceControlTarget::FilterFrequency,
                                             ControlMode::Filter,
                                             0,
                                             frequency,
                                             [this, stripIndex, frequency](StripControlWriteMode writeMode)
                                             {
                                                 setStripFilterFrequencyControlValue(stripIndex,
                                                                                    frequency,
                                                                                    writeMode,
                                                                                    true);
                                             });
            }
            else if (filterSubPage == FilterSubPage::Resonance)
            {
                const float resonance = 0.1f + (column / 15.0f) * 9.9f;
                applySceneAwareMonomeControl(ScenePerformanceControlTarget::FilterResonance,
                                             ControlMode::Filter,
                                             1,
                                             resonance,
                                             [this, stripIndex, resonance](StripControlWriteMode writeMode)
                                             {
                                                 setStripFilterResonanceControlValue(stripIndex,
                                                                                    resonance,
                                                                                    writeMode);
                                             });
            }
            else if (filterSubPage == FilterSubPage::Type && column <= 2)
            {
                const float morph = (column == 0) ? 0.0f : (column == 1 ? 0.5f : 1.0f);
                applySceneAwareMonomeControl(ScenePerformanceControlTarget::FilterMorph,
                                             ControlMode::Filter,
                                             2,
                                             morph,
                                             [this, stripIndex, morph](StripControlWriteMode writeMode)
                                             {
                                                 setStripFilterMorphControlValue(stripIndex,
                                                                                morph,
                                                                                writeMode);
                                             });
            }
            return true;

        case ControlMode::Delay:
        {
            const int targetStripIndex = layout.clampVisibleStrip(getLastMonomePressedStripRow());
            if (auto* targetStrip = audioEngine->getStrip(targetStripIndex))
            {
                MonomeMixActions::handleDelayPageButtonPress(*this,
                                                             *targetStrip,
                                                             targetStripIndex,
                                                             stripIndex,
                                                             column);
                recordMonomeControlPatternEvent(currentControlMode, targetStripIndex, stripIndex, column);
            }
            return true;
        }

        case ControlMode::FileBrowser:
            MonomeFileBrowserActions::handleButtonPress(*this, strip, stripIndex, column);
            return true;

        case ControlMode::GroupAssign:
            if (scenePageActive)
            {
                lastMonomePressedStripRow.store(stripIndex, std::memory_order_release);
                setArcSelectedStripRow(stripIndex);
                requestSceneEditorStripFocus(stripIndex, true);
                updateMonomeLEDs();
            }
            else if (MonomeGroupAssignActions::handleButtonPress(*audioEngine, stripIndex, column))
            {
                updateMonomeLEDs();
            }
            return true;

        case ControlMode::Modulation:
        {
            const int targetStrip = layout.clampVisibleStrip(getLastMonomePressedStripRow());
            syncMonomeModEditPageToPlayback(targetStrip);
            const bool sceneModPageWritesSceneMotion = isSceneModeEnabled()
                && getSceneModPageMode() == SceneModPageMode::StepMotion;
            if (isSceneMainAutomationMonomePageActive())
            {
                const float normalizedY = sceneMainAutomationRowToUnit(row);
                const double currentBeat = audioEngine->getTimelineBeat();
                writeSceneMainAutomationColumnFromMonome(targetStrip, column, normalizedY, currentBeat);
            }
            else
            {
                const float normalizedY = modulationRowToUnitForColumn(row, column);
                const bool bipolar = audioEngine->isModBipolar(targetStrip);
                const bool rearrangeTarget = (audioEngine->getModTarget(targetStrip) == ModernAudioEngine::ModTarget::Rearrange);
                float value = normalizedY;
                if (bipolar)
                {
                    const float signedValue = (normalizedY * 2.0f) - 1.0f;
                    value = juce::jlimit(0.0f, 1.0f, (signedValue * 0.5f) + 0.5f);
                }
                if (rearrangeTarget)
                    value = quantizeMonomeRearrangeValue(value);
                audioEngine->setModStepValue(targetStrip, column, value);
                if (sceneModPageWritesSceneMotion)
                    syncFocusedSceneMotionState();
            }
            updateMonomeLEDs();
            return true;
        }

        case ControlMode::Normal:
        case ControlMode::Preset:
        case ControlMode::StepEdit:
        default:
            return false;
    }
}
