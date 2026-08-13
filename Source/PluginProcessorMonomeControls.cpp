/*
  ==============================================================================

    PluginProcessorMonomeControls.cpp
    Extracted monome layout and control-page handling for mlrVST.

  ==============================================================================
*/

#include "PluginProcessor.h"
#include "GlobalSettingsStore.h"
#include <cmath>

juce::String MlrVSTAudioProcessor::getControlModeName(ControlMode mode)
{
    switch (mode)
    {
        case ControlMode::Speed: return "Speed";
        case ControlMode::Pitch: return "Pitch";
        case ControlMode::Pan: return "Pan";
        case ControlMode::Volume: return "Volume";
        case ControlMode::GrainSize: return "Grain Size";
        case ControlMode::Filter: return "Filter";
        case ControlMode::Delay: return "Delay";
        case ControlMode::Swing: return "Swing";
        case ControlMode::Gate: return "Gate";
        case ControlMode::FileBrowser: return "Browser";
        case ControlMode::GroupAssign: return "Group";
        case ControlMode::Modulation: return "Modulation";
        case ControlMode::Preset: return "Preset";
        case ControlMode::StepEdit: return "Step Edit";
        case ControlMode::Normal:
        default: return "Normal";
    }
}

int MlrVSTAudioProcessor::getMonomeGridWidth() const
{
    if (!monomeConnection.supportsGrid())
        return MaxGridWidth;

    const auto device = monomeConnection.getCurrentGridDevice();
    const int reportedWidth = (device.sizeX > 0) ? device.sizeX : MaxGridWidth;
    return juce::jlimit(1, MaxGridWidth, reportedWidth);
}

int MlrVSTAudioProcessor::getMonomeGridHeight() const
{
    if (!monomeConnection.supportsGrid())
        return 8;

    const auto device = monomeConnection.getCurrentGridDevice();
    const int reportedHeight = (device.sizeY > 0) ? device.sizeY : 8;
    return juce::jlimit(2, MaxGridHeight, reportedHeight);
}

int MlrVSTAudioProcessor::getMonomeControlRow() const
{
    return juce::jmax(1, getMonomeGridHeight() - 1);
}

int MlrVSTAudioProcessor::getMonomeActiveStripCount() const
{
    const int stripRows = juce::jmax(0, getMonomeControlRow() - 1);
    return juce::jlimit(0, MaxStrips, stripRows);
}

MlrVSTAudioProcessor::MonomeLayoutState MlrVSTAudioProcessor::getMonomeLayoutState() const
{
    MonomeLayoutState layout;
    layout.gridWidth = getMonomeGridWidth();
    layout.gridHeight = getMonomeGridHeight();
    layout.controlRow = getMonomeControlRow();
    layout.visibleStripCount = juce::jmax(0, getMonomeActiveStripCount());
    layout.maxStepEditBank = juce::jmax(0, (layout.visibleStripCount - 1) / layout.stepEditBankSize);
    layout.maxVisibleStripIndex = juce::jmax(0, layout.visibleStripCount - 1);
    layout.stripRowsDenom = juce::jmax(1, layout.visibleStripCount - 1);
    layout.modulationRowsDenom = juce::jmax(1, layout.visibleStripCount);
    layout.lastDisplayedStripRow = layout.firstStripRow + juce::jmax(0, layout.visibleStripCount - 1);
    layout.lastPresetRow = juce::jmin(layout.controlRow - 1, PresetRows - 1);
    layout.presetModeActive = (controlModeActive && currentControlMode == ControlMode::Preset);
    layout.stepEditModeActive = (controlModeActive && currentControlMode == ControlMode::StepEdit);
    layout.sceneModeActive = isSceneModeEnabled();
    layout.patternRecorderVisibleOnControlPage =
        controlModeActive && monomeControlPageShowsPatternRecorder(currentControlMode);

    const bool scenePageActive = layout.sceneModeActive
        && controlModeActive
        && currentControlMode == ControlMode::GroupAssign;

    switch (currentControlMode)
    {
        case ControlMode::Normal:
        case ControlMode::Speed:
        case ControlMode::Pitch:
        case ControlMode::Pan:
        case ControlMode::Volume:
        case ControlMode::GrainSize:
        case ControlMode::Delay:
        case ControlMode::Swing:
        case ControlMode::FileBrowser:
        case ControlMode::Preset:
            layout.topRowEditSupported = false;
            break;
        case ControlMode::GroupAssign:
            layout.topRowEditSupported = scenePageActive;
            break;
        case ControlMode::StepEdit:
        case ControlMode::Gate:
        case ControlMode::Filter:
        case ControlMode::Modulation:
            layout.topRowEditSupported = controlModeActive;
            break;
    }

    layout.topRowEditActive = layout.topRowEditSupported && monomeTopRowEditOverlayActive;

    if (layout.presetModeActive)
    {
        layout.topRowMode = layout.sceneModeActive
            ? MonomeLayoutState::TopRowMode::SceneLaunch
            : MonomeLayoutState::TopRowMode::PresetGrid;
    }
    else if (scenePageActive)
    {
        layout.topRowMode = MonomeLayoutState::TopRowMode::Scene;
    }
    else if (layout.topRowEditActive)
    {
        switch (currentControlMode)
        {
            case ControlMode::Normal:
            case ControlMode::Speed:
            case ControlMode::Pitch:
            case ControlMode::Pan:
            case ControlMode::Volume:
            case ControlMode::GrainSize:
            case ControlMode::Delay:
            case ControlMode::Swing:
            case ControlMode::FileBrowser:
            case ControlMode::GroupAssign:
            case ControlMode::Preset:
                layout.topRowMode = layout.sceneModeActive
                    ? MonomeLayoutState::TopRowMode::SceneLaunch
                    : MonomeLayoutState::TopRowMode::Launch;
                break;
            case ControlMode::StepEdit:
                layout.topRowMode = MonomeLayoutState::TopRowMode::StepEdit;
                break;
            case ControlMode::Gate:
                layout.topRowMode = MonomeLayoutState::TopRowMode::Gate;
                break;
            case ControlMode::Filter:
                layout.topRowMode = MonomeLayoutState::TopRowMode::Filter;
                break;
            case ControlMode::Modulation:
                layout.topRowMode = MonomeLayoutState::TopRowMode::Modulation;
                break;
        }
    }
    else
    {
        layout.topRowMode = layout.sceneModeActive
            ? MonomeLayoutState::TopRowMode::SceneLaunch
            : MonomeLayoutState::TopRowMode::Launch;
    }

    layout.topRowSceneMode = layout.topRowMode == MonomeLayoutState::TopRowMode::SceneLaunch;
    layout.sceneActionStartColumn = layout.topRowSceneMode
        ? juce::jmin(layout.gridWidth, SceneSlots - 1)
        : 4;
    layout.sceneRecorderVisible = layout.topRowSceneMode
        && layout.sceneActionStartColumn < layout.gridWidth;
    return layout;
}

bool MlrVSTAudioProcessor::isMonomeControlRowUtilityCell(const MonomeLayoutState& layout, int x) const
{
    if (x < 0 || x >= layout.gridWidth)
        return false;

    if (layout.stepEditModeActive)
        return (x == 13 || x == 14) && x != getControlButtonForMode(ControlMode::StepEdit);

    if (layout.topRowEditSupported && x == layout.topRowEditToggleColumn)
        return true;

    if ((!controlModeActive || currentControlMode == ControlMode::Normal) && x >= 14 && x <= 15)
    {
        if (x == 15)
            return true;

        if (audioEngine == nullptr)
            return false;

        const int selectedStripIndex = layout.clampVisibleStrip(getLastMonomePressedStripRow());
        if (auto* strip = audioEngine->getStrip(selectedStripIndex))
            return strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Sample;
    }

    return false;
}

bool MlrVSTAudioProcessor::isMonomeTopRowEditSupported() const
{
    return getMonomeLayoutState().topRowEditSupported;
}

bool MlrVSTAudioProcessor::isMonomeTopRowEditActive() const
{
    return getMonomeLayoutState().topRowEditActive;
}

juce::String MlrVSTAudioProcessor::getMonomeTopRowModeName() const
{
    const auto layout = getMonomeLayoutState();
    switch (layout.topRowMode)
    {
        case MonomeLayoutState::TopRowMode::Launch:      return "Launch";
        case MonomeLayoutState::TopRowMode::SceneLaunch: return "Scene Launch";
        case MonomeLayoutState::TopRowMode::PresetGrid:  return "Preset Grid";
        case MonomeLayoutState::TopRowMode::Scene:       return "Scene Controls";
        case MonomeLayoutState::TopRowMode::StepEdit:    return "Edit: Step Edit";
        case MonomeLayoutState::TopRowMode::Gate:        return "Edit: Gate";
        case MonomeLayoutState::TopRowMode::Filter:      return "Edit: Filter";
        case MonomeLayoutState::TopRowMode::Modulation:  return "Edit: Modulation";
    }

    return "Launch";
}

juce::String MlrVSTAudioProcessor::getMonomeTopRowHintText() const
{
    const auto layout = getMonomeLayoutState();
    const juce::String togglePadText = "bottom-right pad (control-row 15)";
    const bool scenePageActive = layout.sceneModeActive
        && controlModeActive
        && currentControlMode == ControlMode::GroupAssign;

    if (scenePageActive)
    {
        if (layout.topRowEditActive)
            return "Scene page: body rows focus the strip, collapse the others, and expand it in the scene editor, and row 0 right side picks transition types. Press " + togglePadText + " for quick scene controls.";

        return "Scene page: body rows focus the strip, collapse the others, and expand it in the scene editor, and row 0 right side edits the focused scene. Press " + togglePadText + " for direct transition pads.";
    }

    if (layout.topRowEditSupported)
    {
        if (layout.topRowEditActive)
        {
            if (layout.sceneModeActive)
                return "Press " + togglePadText + " to return row 0 right side to the base scene view.";

            return "Press " + togglePadText + " to return the top row to Launch.";
        }

        if (layout.sceneModeActive)
            return "Press " + togglePadText + " to open this page on row 0 right of the scene pads.";

        return "Press " + togglePadText + " to open top-row Edit for this page.";
    }

    if (layout.topRowMode == MonomeLayoutState::TopRowMode::PresetGrid)
        return "Preset mode owns the top rows.";

    if (layout.topRowMode == MonomeLayoutState::TopRowMode::SceneLaunch)
        return "Scene mode keeps the scene pads on the left side of row 0.";

    return "Edit toggle appears on Step Edit, Gate, Filter, Mod, and Scene pages.";
}

MlrVSTAudioProcessor::ControlPageOrder MlrVSTAudioProcessor::getControlPageOrder() const
{
    const juce::ScopedLock lock(controlPageOrderLock);
    return controlPageOrder;
}

MlrVSTAudioProcessor::ControlMode MlrVSTAudioProcessor::getControlModeForControlButton(int buttonIndex) const
{
    const int clamped = juce::jlimit(0, NumControlRowPages - 1, buttonIndex);
    const juce::ScopedLock lock(controlPageOrderLock);
    return controlPageOrder[static_cast<size_t>(clamped)];
}

int MlrVSTAudioProcessor::getControlButtonForMode(ControlMode mode) const
{
    const juce::ScopedLock lock(controlPageOrderLock);
    for (int i = 0; i < NumControlRowPages; ++i)
    {
        if (controlPageOrder[static_cast<size_t>(i)] == mode)
            return i;
    }
    return -1;
}

void MlrVSTAudioProcessor::moveControlPage(int fromIndex, int toIndex)
{
    if (fromIndex == toIndex)
        return;

    fromIndex = juce::jlimit(0, NumControlRowPages - 1, fromIndex);
    toIndex = juce::jlimit(0, NumControlRowPages - 1, toIndex);
    if (fromIndex == toIndex)
        return;

    {
        const juce::ScopedLock lock(controlPageOrderLock);
        std::swap(controlPageOrder[static_cast<size_t>(fromIndex)],
                  controlPageOrder[static_cast<size_t>(toIndex)]);
    }

    GlobalSettingsStore::saveControlPages(*this);
}

void MlrVSTAudioProcessor::setControlPageMomentary(bool shouldBeMomentary)
{
    controlPageMomentary.store(shouldBeMomentary, std::memory_order_release);
    GlobalSettingsStore::saveControlPages(*this);
}

void MlrVSTAudioProcessor::setSwingDivisionSelection(int mode)
{
    const int maxDivision = static_cast<int>(EnhancedAudioStrip::SwingDivision::SixteenthTriplet);
    const int clamped = juce::jlimit(0, maxDivision, mode);
    swingDivisionSelection.store(clamped, std::memory_order_release);
    if (audioEngine)
        audioEngine->setGlobalSwingDivision(static_cast<EnhancedAudioStrip::SwingDivision>(clamped));
    GlobalSettingsStore::saveControlPages(*this);
}

void MlrVSTAudioProcessor::setInnerLoopLengthSelection(int choiceIndex)
{
    const int clamped = juce::jlimit(0, 4, choiceIndex);
    innerLoopLengthSelection.store(clamped, std::memory_order_release);

    if (auto* param = parameters.getParameter("innerLoopLength"))
        param->setValueNotifyingHost(param->convertTo0to1(static_cast<float>(clamped)));
    else
        lastAppliedInnerLoopLengthSelection.store(clamped, std::memory_order_release);
}

void MlrVSTAudioProcessor::syncTransientDetectionSettingsFromParameters(bool refreshSlicesNow)
{
    const int onsetMethod = transientOnsetMethodParam != nullptr
        ? juce::jlimit(0, 2, static_cast<int>(std::round(transientOnsetMethodParam->load(std::memory_order_acquire))))
        : 2;
    const int sensitivity = transientSensitivityParam != nullptr
        ? juce::jlimit(0, 4, static_cast<int>(std::round(transientSensitivityParam->load(std::memory_order_acquire))))
        : 2;
    const int snap = transientSnapParam != nullptr
        ? juce::jlimit(0, 4, static_cast<int>(std::round(transientSnapParam->load(std::memory_order_acquire))))
        : 3;
    const int spacing = transientSpacingParam != nullptr
        ? juce::jlimit(0, 4, static_cast<int>(std::round(transientSpacingParam->load(std::memory_order_acquire))))
        : 2;

    const bool changed = onsetMethod != lastAppliedTransientOnsetMethod
        || sensitivity != lastAppliedTransientSensitivity
        || snap != lastAppliedTransientSnap
        || spacing != lastAppliedTransientSpacing;

    if (!changed && !refreshSlicesNow)
        return;

    if (audioEngine != nullptr)
        audioEngine->setTransientDetectionConfig(onsetMethod, sensitivity, snap, spacing);
    lastAppliedTransientOnsetMethod = onsetMethod;
    lastAppliedTransientSensitivity = sensitivity;
    lastAppliedTransientSnap = snap;
    lastAppliedTransientSpacing = spacing;

    if (refreshSlicesNow)
    {
        if (audioEngine != nullptr)
            audioEngine->refreshTransientSliceMaps();

        for (auto& engine : sampleModeEngines)
        {
            if (engine != nullptr)
                engine->refreshTransientAnalysis();
        }
    }
}

void MlrVSTAudioProcessor::setControlModeFromGui(ControlMode mode, bool shouldBeActive)
{
    if (!shouldBeActive || mode == ControlMode::Normal)
    {
        currentControlMode = ControlMode::Normal;
        controlModeActive = false;
    }
    else
    {
        currentControlMode = mode;
        controlModeActive = true;
    }
    monomeTopRowEditOverlayActive = false;

    updateMonomeLEDs();
}

bool MlrVSTAudioProcessor::monomeControlPageShowsPatternRecorder(ControlMode mode) const
{
    switch (mode)
    {
        case ControlMode::Speed:
        case ControlMode::Pitch:
        case ControlMode::Pan:
        case ControlMode::Volume:
        case ControlMode::GrainSize:
        case ControlMode::Swing:
        case ControlMode::Delay:
        case ControlMode::Filter:
            return true;
        case ControlMode::Normal:
        case ControlMode::Gate:
        case ControlMode::FileBrowser:
        case ControlMode::GroupAssign:
        case ControlMode::Modulation:
        case ControlMode::Preset:
        case ControlMode::StepEdit:
        default:
            return false;
    }
}

bool MlrVSTAudioProcessor::handleMonomePatternButtonPress(int patternIndex, uint32_t nowMs)
{
    if (audioEngine == nullptr)
        return false;

    auto* pattern = audioEngine->getPattern(patternIndex);
    if (pattern == nullptr)
        return false;

    const auto idx = static_cast<size_t>(patternIndex);
    const auto pendingAction = monomePatternPadPendingAction[idx];
    const uint32_t pendingUntilMs = monomePatternPadPendingUntilMs[idx];
    const bool pendingActive = pendingAction != MonomePatternTapAction::None
        && pendingUntilMs != 0
        && static_cast<int32_t>(pendingUntilMs - nowMs) >= 0;

    if (pendingActive)
    {
        clearPendingMonomePatternTap(patternIndex);
        audioEngine->startPatternOverdub(patternIndex);
        return true;
    }

    clearPendingMonomePatternTap(patternIndex);

    if (pattern->isRecording())
    {
        if (pattern->isOverdubbing())
        {
            monomePatternPadPendingAction[idx] = MonomePatternTapAction::StopRecording;
            monomePatternPadPendingUntilMs[idx] = nowMs + monomePatternDoubleTapMs;
        }
        else
        {
            audioEngine->stopPatternRecording(patternIndex);
        }

        return true;
    }

    if (pattern->getEventCount() <= 0)
    {
        audioEngine->startPatternRecording(patternIndex);
        return true;
    }

    monomePatternPadPendingAction[idx] = pattern->isPlaying()
        ? MonomePatternTapAction::StopPlayback
        : MonomePatternTapAction::StartFreshRecording;
    monomePatternPadPendingUntilMs[idx] = nowMs + monomePatternDoubleTapMs;
    return true;
}

void MlrVSTAudioProcessor::processPendingMonomePatternTapActions(uint32_t nowMs)
{
    if (audioEngine == nullptr)
    {
        for (int patternIndex = 0; patternIndex < ModernAudioEngine::MaxPatterns; ++patternIndex)
            clearPendingMonomePatternTap(patternIndex);
        return;
    }

    for (int patternIndex = 0; patternIndex < ModernAudioEngine::MaxPatterns; ++patternIndex)
    {
        const auto idx = static_cast<size_t>(patternIndex);
        const auto action = monomePatternPadPendingAction[idx];
        const uint32_t pendingUntilMs = monomePatternPadPendingUntilMs[idx];
        const bool stillWaiting = action != MonomePatternTapAction::None
            && pendingUntilMs != 0
            && static_cast<int32_t>(pendingUntilMs - nowMs) > 0;
        if (stillWaiting)
            continue;

        clearPendingMonomePatternTap(patternIndex);

        switch (action)
        {
            case MonomePatternTapAction::StartFreshRecording:
                audioEngine->startPatternRecording(patternIndex);
                break;
            case MonomePatternTapAction::StopPlayback:
                audioEngine->stopPatternPlayback(patternIndex);
                break;
            case MonomePatternTapAction::StopRecording:
                audioEngine->stopPatternRecording(patternIndex);
                break;
            case MonomePatternTapAction::None:
            default:
                break;
        }
    }
}

void MlrVSTAudioProcessor::clearPendingMonomePatternTap(int patternIndex)
{
    if (patternIndex < 0 || patternIndex >= ModernAudioEngine::MaxPatterns)
        return;

    const auto idx = static_cast<size_t>(patternIndex);
    monomePatternPadPendingUntilMs[idx] = 0;
    monomePatternPadPendingAction[idx] = MonomePatternTapAction::None;
}

bool MlrVSTAudioProcessor::handleMonomeSceneRecorderButtonPress(uint32_t nowMs)
{
    if (!isSceneModeEnabled())
        return false;

    const auto pendingAction = monomeSceneRecorderPendingAction;
    const uint32_t pendingUntilMs = monomeSceneRecorderPendingUntilMs;
    const bool pendingActive = pendingAction != MonomeSceneRecorderTapAction::None
        && pendingUntilMs != 0
        && static_cast<int32_t>(pendingUntilMs - nowMs) >= 0;

    if (pendingActive)
    {
        clearPendingMonomeSceneRecorderTap();
        if (scenePerformanceRecorder.isRecording() && scenePerformanceRecorder.isOverdubbing())
            extendScenePerformanceRecording();
        else
            requestSceneRecorderActionQuantized(SceneRecorderAction::StartOverdub);
        return true;
    }

    clearPendingMonomeSceneRecorderTap();

    if (scenePerformanceRecorder.isRecording())
    {
        if (scenePerformanceRecorder.isOverdubbing())
        {
            monomeSceneRecorderPendingAction = MonomeSceneRecorderTapAction::StopRecording;
            monomeSceneRecorderPendingUntilMs = nowMs + monomePatternDoubleTapMs;
        }
        else
        {
            requestSceneRecorderActionQuantized(SceneRecorderAction::StopRecording);
        }

        return true;
    }

    const int sceneSlot = juce::jlimit(0, SceneSlots - 1, activeSceneSlot);
    if (!scenePerformanceRecorder.hasEvents(sceneSlot))
    {
        requestSceneRecorderActionQuantized(SceneRecorderAction::StartFreshRecording);
        return true;
    }

    monomeSceneRecorderPendingAction = MonomeSceneRecorderTapAction::StartFreshRecording;
    monomeSceneRecorderPendingUntilMs = nowMs + monomePatternDoubleTapMs;
    return true;
}

bool MlrVSTAudioProcessor::beginMonomeSceneGestureRecording()
{
    if (!isSceneModeEnabled() || audioEngine == nullptr || !monomeSceneRecorderHeld)
        return false;

    monomeSceneRecorderGestureRecordConsumed = true;
    clearPendingMonomeSceneRecorderTap();
    clearPendingSceneRecorderAction();

    if (scenePerformanceRecorder.isRecording())
        return true;

    const int sceneSlot = juce::jlimit(0, SceneSlots - 1, activeSceneSlot);
    const bool overdub = scenePerformanceRecorder.hasEvents(sceneSlot);
    const double currentBeat = audioEngine->getTimelineBeat();
    const double sceneStartBeat = activeSceneStartPpqValid && std::isfinite(activeSceneStartPpq)
        ? activeSceneStartPpq
        : currentBeat;

    startScenePerformanceRecordingAt(overdub, sceneSlot, currentBeat, sceneStartBeat, false);
    return scenePerformanceRecorder.isRecording();
}

void MlrVSTAudioProcessor::processPendingMonomeSceneRecorderTapActions(uint32_t nowMs)
{
    const auto action = monomeSceneRecorderPendingAction;
    const uint32_t pendingUntilMs = monomeSceneRecorderPendingUntilMs;
    const bool stillWaiting = action != MonomeSceneRecorderTapAction::None
        && pendingUntilMs != 0
        && static_cast<int32_t>(pendingUntilMs - nowMs) > 0;
    if (stillWaiting)
        return;

    clearPendingMonomeSceneRecorderTap();

    switch (action)
    {
        case MonomeSceneRecorderTapAction::StartFreshRecording:
            requestSceneRecorderActionQuantized(SceneRecorderAction::StartFreshRecording);
            break;
        case MonomeSceneRecorderTapAction::StopRecording:
            requestSceneRecorderActionQuantized(SceneRecorderAction::StopRecording);
            break;
        case MonomeSceneRecorderTapAction::None:
        default:
            break;
    }
}

void MlrVSTAudioProcessor::clearPendingMonomeSceneRecorderTap()
{
    monomeSceneRecorderPendingUntilMs = 0;
    monomeSceneRecorderPendingAction = MonomeSceneRecorderTapAction::None;
}

void MlrVSTAudioProcessor::requestSceneRecorderActionQuantized(SceneRecorderAction action)
{
    if (!isSceneModeEnabled() || audioEngine == nullptr || action == SceneRecorderAction::None)
        return;

    const int sceneSlot = juce::jlimit(0, SceneSlots - 1, activeSceneSlot);
    const bool hostTransportPlaying = isHostTransportPlaying();
    const bool immediateAction = !hostTransportPlaying || getSceneRecallMode() == SceneRecallMode::Manual;
    const double currentBeat = audioEngine->getTimelineBeat();
    const double sceneStartBeat = activeSceneStartPpqValid && std::isfinite(activeSceneStartPpq)
        ? activeSceneStartPpq
        : currentBeat;

    if (immediateAction)
    {
        clearPendingSceneRecorderAction();
        switch (action)
        {
            case SceneRecorderAction::StartFreshRecording:
                startScenePerformanceRecordingAt(false, sceneSlot, currentBeat, sceneStartBeat);
                break;
            case SceneRecorderAction::StartOverdub:
                startScenePerformanceRecordingAt(true, sceneSlot, currentBeat, sceneStartBeat);
                break;
            case SceneRecorderAction::StopRecording:
                stopScenePerformanceRecordingNow();
                break;
            case SceneRecorderAction::None:
            default:
                break;
        }
        return;
    }

    pendingSceneRecorderAction.active = true;
    pendingSceneRecorderAction.targetResolved = false;
    pendingSceneRecorderAction.patternEndPhaseSignatureValid = false;
    pendingSceneRecorderAction.action = action;
    pendingSceneRecorderAction.sceneSlot = sceneSlot;
    pendingSceneRecorderAction.targetPpq = 0.0;
    pendingSceneRecorderAction.intervalBeats = 4.0;
    pendingSceneRecorderAction.patternEndPhaseSignature = 0;
    refreshUtilityTimerCadence();
    updateMonomeLEDs();
}
