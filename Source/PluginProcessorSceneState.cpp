/*
  ==============================================================================

    PluginProcessorSceneState.cpp
    Extracted scene state and runtime slot bookkeeping for mlrVST.

  ==============================================================================
*/

#include "PluginProcessor.h"
#include "SceneAutomationRules.h"
#include <cmath>

namespace
{
struct ScopedSceneAutosaveSuppression
{
    explicit ScopedSceneAutosaveSuppression(MlrVSTAudioProcessor& p) : processor(p)
    {
        processor.beginSceneAutosaveSuppression();
    }

    ~ScopedSceneAutosaveSuppression()
    {
        processor.endSceneAutosaveSuppression();
    }

    MlrVSTAudioProcessor& processor;
};

uint32_t sceneSlotColourArgb(int sceneSlot) noexcept
{
    switch (juce::jlimit(0, MlrVSTAudioProcessor::SceneSlots - 1, sceneSlot))
    {
        case 0:  return 0xff79c8ff;
        case 1:  return 0xff95dd6a;
        case 2:  return 0xffffc467;
        case 3:  return 0xffff9376;
        case 4:  return 0xffc79bff;
        case 5:  return 0xff64dfc8;
        case 6:  return 0xffff8fbe;
        case 7:  return 0xffd6d66e;
        default: return 0xff78b7ff;
    }
}

void appendSceneDebugLog(const juce::String& message)
{
    const auto logFile = juce::File::getSpecialLocation(juce::File::tempDirectory)
        .getChildFile("mlrvst_scene_debug.log");
    juce::FileOutputStream out(logFile);
    if (!out.openedOk())
        return;

    out.setPosition(logFile.existsAsFile() ? logFile.getSize() : 0);
    const auto timestamp = juce::Time::getCurrentTime().formatted("%Y-%m-%d %H:%M:%S.%3Q");
    out.writeText(timestamp + " " + message + "\n", false, false, "\n");
    out.flush();
}
}

void MlrVSTAudioProcessor::focusSceneSlot(int sceneSlot)
{
    const int safeSceneSlot = juce::jlimit(0, SceneSlots - 1, sceneSlot);
    focusedSceneSlot = safeSceneSlot;

    const int mainPresetIndex = getActiveMainPresetIndexForScenes();
    ScopedSceneAutosaveSuppression suppressSceneAutosave(*this);

    if (const auto* storedSceneState = getStoredSceneSlotState(mainPresetIndex, safeSceneSlot))
    {
        if (storedSceneState->preparedSwitchPayloadTemplate != nullptr
            && storedSceneState->preparedSwitchPayloadTemplate->sceneTimingState.valid)
        {
            applyPreparedSceneTimingState(
                safeSceneSlot,
                storedSceneState->preparedSwitchPayloadTemplate->sceneTimingState);
            return;
        }
    }

    setSceneRepeatCount(safeSceneSlot, 1);
    setSceneLengthMode(safeSceneSlot, SceneLengthMode::ManualBars);
    setSceneLengthCount(safeSceneSlot, 4);
    setSceneAnchorStrip(safeSceneSlot, 0);
}

int MlrVSTAudioProcessor::getFocusedSceneSlot() const
{
    return juce::jlimit(0, SceneSlots - 1, focusedSceneSlot);
}

MlrVSTAudioProcessor::SceneInfo MlrVSTAudioProcessor::getSceneInfo(int sceneSlot,
                                                                   int mainPresetIndex) const
{
    const int safeSceneSlot = juce::jlimit(0, SceneSlots - 1, sceneSlot);
    const int resolvedMainPresetIndex = juce::jlimit(0,
                                                     MaxPresetSlots - 1,
                                                     mainPresetIndex >= 0 ? mainPresetIndex
                                                                          : getActiveMainPresetIndexForScenes());

    SceneInfo info;
    info.mainPresetIndex = resolvedMainPresetIndex;
    info.sceneSlot = safeSceneSlot;
    info.colourArgb = sceneSlotColourArgb(safeSceneSlot);
    juce::String storedName;
    if (const auto* storedSceneState = getStoredSceneSlotState(resolvedMainPresetIndex, safeSceneSlot))
        storedName = storedSceneState->name.trim();
    info.name = storedName.isNotEmpty()
        ? storedName
        : ("Scene " + juce::String(safeSceneSlot + 1));
    return info;
}

MlrVSTAudioProcessor::SceneClipSlotState MlrVSTAudioProcessor::getSceneClipSlotState(int sceneSlot,
                                                                                      int mainPresetIndex) const
{
    const int safeSceneSlot = juce::jlimit(0, SceneSlots - 1, sceneSlot);
    const int resolvedMainPresetIndex = juce::jlimit(0,
                                                     MaxPresetSlots - 1,
                                                     mainPresetIndex >= 0 ? mainPresetIndex
                                                                          : getActiveMainPresetIndexForScenes());
    const auto& runtimeState = sceneClipSlotRuntimeStates[static_cast<size_t>(safeSceneSlot)];

    SceneClipSlotState clipSlot;
    clipSlot.mainPresetIndex = resolvedMainPresetIndex;
    clipSlot.sceneSlot = safeSceneSlot;
    clipSlot.hasStoredContent = hasStoredSceneSlotState(resolvedMainPresetIndex, safeSceneSlot);
    clipSlot.hasMotionState = sceneSlotHasMotionState(safeSceneSlot);
    clipSlot.hasPerformanceClip = hasScenePerformanceClip(safeSceneSlot);
    clipSlot.hasLiveStripControlState = runtimeState.hasLiveStripControls
        && runtimeState.mainPresetIndex == resolvedMainPresetIndex;
    clipSlot.liveStripControlDirty = clipSlot.hasLiveStripControlState && runtimeState.liveStripControlsDirty;
    clipSlot.repeatCount = getSceneRepeatCount(safeSceneSlot);
    clipSlot.lengthMode = getSceneLengthMode(safeSceneSlot);
    clipSlot.lengthCount = getSceneLengthCount(safeSceneSlot);
    clipSlot.anchorStrip = getSceneAnchorStrip(safeSceneSlot);
    clipSlot.chainStepIndex = getSceneSequenceStepIndex(safeSceneSlot);
    clipSlot.inChain = clipSlot.chainStepIndex >= 0;
    return clipSlot;
}

MlrVSTAudioProcessor::SceneSlotDefinition MlrVSTAudioProcessor::getSceneSlotDefinition(int sceneSlot,
                                                                                        int mainPresetIndex) const
{
    const auto scene = getSceneInfo(sceneSlot, mainPresetIndex);
    const auto clipSlot = getSceneClipSlotState(sceneSlot, mainPresetIndex);

    SceneSlotDefinition definition;
    definition.mainPresetIndex = scene.mainPresetIndex;
    definition.sceneSlot = scene.sceneSlot;
    definition.name = scene.name;
    definition.colourArgb = scene.colourArgb;
    definition.hasStoredContent = clipSlot.hasStoredContent;
    definition.repeatCount = clipSlot.repeatCount;
    definition.lengthMode = clipSlot.lengthMode;
    definition.lengthCount = clipSlot.lengthCount;
    definition.anchorStrip = clipSlot.anchorStrip;
    definition.inChain = clipSlot.inChain;
    definition.chainStepIndex = clipSlot.chainStepIndex;
    return definition;
}

MlrVSTAudioProcessor::SceneWatcherState MlrVSTAudioProcessor::getSceneWatcherState() const
{
    SceneWatcherState watcher;
    watcher.focusedSceneSlot = getFocusedSceneSlot();
    watcher.activeMainPresetIndex = juce::jlimit(0, MaxPresetSlots - 1, activeSceneMainPresetIndex);
    watcher.activeSceneSlot = juce::jlimit(0, SceneSlots - 1, activeSceneSlot);
    watcher.chainActive = isSceneChainPlaybackActive();
    watcher.chainStepIndex = watcher.chainActive ? getSceneChainPlaybackStepIndex() : -1;
    watcher.playbackOwner = watcher.chainActive ? ScenePlaybackOwner::Chain : scenePlaybackOwner;
    watcher.manualPlaybackOwned = watcher.playbackOwner == ScenePlaybackOwner::Manual;
    watcher.chainPlaybackOwned = watcher.playbackOwner == ScenePlaybackOwner::Chain;

    SceneSwitchEvent pendingSwitchEvent;
    if (peekPendingSceneApplyState(pendingSwitchEvent))
    {
        watcher.hasQueuedScene = true;
        watcher.queuedMainPresetIndex = juce::jlimit(0, MaxPresetSlots - 1, pendingSwitchEvent.mainPresetIndex);
        watcher.queuedSceneSlot = juce::jlimit(0, SceneSlots - 1, pendingSwitchEvent.sceneSlot);
    }
    else if (pendingSceneRecall.active)
    {
        watcher.hasQueuedScene = true;
        watcher.queuedMainPresetIndex = juce::jlimit(0, MaxPresetSlots - 1, pendingSceneRecall.mainPresetIndex);
        watcher.queuedSceneSlot = juce::jlimit(0, SceneSlots - 1, pendingSceneRecall.sceneSlot);
    }

    const int resolvedMainPresetIndex = getActiveMainPresetIndexForScenes();
    for (int slot = 0; slot < SceneSlots; ++slot)
    {
        auto& slotState = watcher.slots[static_cast<size_t>(slot)];
        slotState.scene = getSceneInfo(slot, resolvedMainPresetIndex);
        slotState.clipSlot = getSceneClipSlotState(slot, resolvedMainPresetIndex);
        slotState.focused = (slot == watcher.focusedSceneSlot);
        slotState.active = (slot == watcher.activeSceneSlot
                            && slotState.scene.mainPresetIndex == watcher.activeMainPresetIndex);
        slotState.queued = watcher.hasQueuedScene
            && slot == watcher.queuedSceneSlot
            && slotState.scene.mainPresetIndex == watcher.queuedMainPresetIndex;
        slotState.chainCurrent = watcher.chainActive
            && slotState.clipSlot.chainStepIndex >= 0
            && slotState.clipSlot.chainStepIndex == watcher.chainStepIndex;
    }

    return watcher;
}

void MlrVSTAudioProcessor::startManualScenePlayback(int sceneSlot,
                                                    bool useTriggerQuantization,
                                                    bool focusSceneSlotFirst)
{
    const int safeSceneSlot = juce::jlimit(0, SceneSlots - 1, sceneSlot);
    if (focusSceneSlotFirst)
        focusSceneSlot(safeSceneSlot);

    const bool exitingSceneChain = sceneSequenceActive;
    if (exitingSceneChain)
    {
        appendSceneDebugLog("launch_scene stop_chain slot=" + juce::String(safeSceneSlot));
        stopSceneChainPlayback();
    }

    const int mainPresetIndex = getActiveMainPresetIndexForScenes();
    const bool targetsCurrentLiveScene = !sceneSequenceActive
        && activeSceneMainPresetIndex == mainPresetIndex
        && activeSceneSlot == safeSceneSlot;

    appendSceneDebugLog("launch_scene slot=" + juce::String(safeSceneSlot)
        + " mainPreset=" + juce::String(mainPresetIndex)
        + " activeScene=" + juce::String(activeSceneSlot)
        + " activeMain=" + juce::String(activeSceneMainPresetIndex)
        + " triggerQuantized=" + juce::String(useTriggerQuantization ? 1 : 0)
        + " targetsCurrent=" + juce::String(targetsCurrentLiveScene ? 1 : 0)
        + " needsCapture=" + juce::String(activeSceneNeedsCaptureBeforeManualRecall ? 1 : 0)
        + " exists=" + juce::String(sceneSlotExistsForMainPreset(mainPresetIndex, safeSceneSlot) ? 1 : 0));

    if (!sceneSequenceActive
        && activeSceneNeedsCaptureBeforeManualRecall
        && activeSceneRecallDegraded.load(std::memory_order_acquire) == 0
        && activeSceneMainPresetIndex == mainPresetIndex)
    {
        const int captureSceneSlot = juce::jlimit(0,
                                                  SceneSlots - 1,
                                                  targetsCurrentLiveScene ? safeSceneSlot : activeSceneSlot);
        const bool captured = refreshStoredSceneSlotSnapshot(mainPresetIndex, captureSceneSlot);
        appendSceneDebugLog("launch_scene capture_current slot=" + juce::String(captureSceneSlot)
            + " success=" + juce::String(captured ? 1 : 0));
        updateMonomeLEDs();
        if (!captured)
            return;
    }

    clearPendingSceneApplyState();
    pendingScenePreloadDirty.store(0, std::memory_order_release);
    pendingScenePreloadMainPreset.store(-1, std::memory_order_release);
    pendingScenePreloadSceneSlot.store(-1, std::memory_order_release);
    pendingScenePreloadSequenceDriven.store(0, std::memory_order_release);
    pendingScenePreloadSequenceStep.store(-1, std::memory_order_release);
    pendingScenePreloadTargetPpq.store(-1.0, std::memory_order_release);
    pendingScenePreloadTargetTempo.store(120.0, std::memory_order_release);
    pendingScenePreloadTargetSample.store(-1, std::memory_order_release);
    pendingScenePreloadTransitionType.store(static_cast<int>(SceneChainTransitionType::None),
                                            std::memory_order_release);
    requestAbortActiveSceneTransition();

    switchScenePlaybackOwner(ScenePlaybackOwner::Manual, false);
    requestSceneRecallQuantized(mainPresetIndex, safeSceneSlot, false, -1, useTriggerQuantization);
    appendSceneDebugLog("launch_scene queued_recall slot=" + juce::String(safeSceneSlot)
        + " mainPreset=" + juce::String(mainPresetIndex));
    updateMonomeLEDs();
}

void MlrVSTAudioProcessor::launchSceneSlotFromSurface(int sceneSlot)
{
    startManualScenePlayback(sceneSlot, true, true);
}

void MlrVSTAudioProcessor::launchSceneSlotFromMonome(int sceneSlot)
{
    startManualScenePlayback(sceneSlot, true, true);
}

void MlrVSTAudioProcessor::syncActiveSceneClipSlotRuntimeStateFromEngine(bool markDirty)
{
    if (!isSceneModeEnabled() || audioEngine == nullptr)
        return;

    const int safeSceneSlot = juce::jlimit(0, SceneSlots - 1, activeSceneSlot);
    auto& runtimeState = sceneClipSlotRuntimeStates[static_cast<size_t>(safeSceneSlot)];
    runtimeState.mainPresetIndex = getActiveMainPresetIndexForScenes();
    runtimeState.hasLiveStripControls = true;
    runtimeState.liveStripControlsDirty = runtimeState.liveStripControlsDirty || markDirty;

    for (int stripIndex = 0; stripIndex < MaxStrips; ++stripIndex)
    {
        auto& stripState = runtimeState.stripControls[static_cast<size_t>(stripIndex)];
        stripState.controlValues.fill(0.0f);
        stripState.controlValueValid.fill(false);

        if (auto* strip = audioEngine->getStrip(stripIndex))
        {
            stripState.playMode = strip->getPlayMode();

            for (int targetValue = static_cast<int>(ScenePerformanceControlTarget::Speed);
                 targetValue <= static_cast<int>(ScenePerformanceControlTarget::GrainShape);
                 ++targetValue)
            {
                const auto target = static_cast<ScenePerformanceControlTarget>(targetValue);
                if (!SceneAutomationRules::targetUsesStripRuntimeState(target))
                    continue;

                float value = 0.0f;
                if (!getSceneControlCurrentValue(stripIndex, target, value) || !std::isfinite(value))
                    continue;

                const auto targetIndex = static_cast<size_t>(SceneAutomationRules::controlTargetIndex(target));
                stripState.controlValues[targetIndex] = value;
                stripState.controlValueValid[targetIndex] = true;
            }
        }
    }
}

void MlrVSTAudioProcessor::applySceneClipSlotRuntimeState(int mainPresetIndex, int sceneSlot)
{
    if (audioEngine == nullptr)
        return;

    const int safeSceneSlot = juce::jlimit(0, SceneSlots - 1, sceneSlot);
    const int safeMainPresetIndex = juce::jlimit(0, MaxPresetSlots - 1, mainPresetIndex);
    const auto& runtimeState = sceneClipSlotRuntimeStates[static_cast<size_t>(safeSceneSlot)];
    const bool hasStoredSceneState = hasStoredSceneSlotState(safeMainPresetIndex, safeSceneSlot);
    if (!runtimeState.hasLiveStripControls
        || !runtimeState.liveStripControlsDirty
        || runtimeState.mainPresetIndex != safeMainPresetIndex)
    {
        return;
    }

    for (int stripIndex = 0; stripIndex < MaxStrips; ++stripIndex)
    {
        const auto& stripState = runtimeState.stripControls[static_cast<size_t>(stripIndex)];
        if (auto* strip = audioEngine->getStrip(stripIndex))
            strip->setPlayMode(stripState.playMode);

        for (int targetValue = static_cast<int>(ScenePerformanceControlTarget::Speed);
             targetValue <= static_cast<int>(ScenePerformanceControlTarget::GrainShape);
             ++targetValue)
        {
            const auto target = static_cast<ScenePerformanceControlTarget>(targetValue);
            if (!SceneAutomationRules::targetUsesStripRuntimeState(target))
                continue;

            const auto targetIndex = static_cast<size_t>(SceneAutomationRules::controlTargetIndex(target));
            if (!stripState.controlValueValid[targetIndex])
                continue;

            // Stored scene payloads are the canonical source of strip speed on recall.
            // The runtime slot cache is useful for empty scenes and unsaved live state,
            // but speed is much more likely to be dirtied by transient scene/stutter
            // handoffs. Reapplying that cached speed over a stored scene can produce
            // the brief "double speed then snap back" glitch on scene changes.
            if (hasStoredSceneState && target == ScenePerformanceControlTarget::Speed)
                continue;

            ScenePerformanceEvent event;
            event.type = ScenePerformanceEventType::ControlPoint;
            event.stripIndex = stripIndex;
            event.controlTarget = target;
            event.controlMode = static_cast<int>(SceneAutomationRules::controlModeForTarget(target));
            event.controlRow = SceneAutomationRules::controlRowForTarget(target);
            event.value = stripState.controlValues[targetIndex];
            event.column = SceneAutomationRules::columnFromNormalizedValue(
                SceneAutomationRules::normalizeValue(target, event.value));
            applyScenePerformanceEvent(event, StripControlWriteMode::CacheOnly);
        }

        reapplyStripStateForCurrentPlayMode(stripIndex);
    }
}

void MlrVSTAudioProcessor::clearSceneClipSlotRuntimeState(int mainPresetIndex, int sceneSlot)
{
    const int safeSceneSlot = juce::jlimit(0, SceneSlots - 1, sceneSlot);
    const int safeMainPresetIndex = juce::jlimit(0, MaxPresetSlots - 1, mainPresetIndex);
    auto& runtimeState = sceneClipSlotRuntimeStates[static_cast<size_t>(safeSceneSlot)];
    if (runtimeState.mainPresetIndex != safeMainPresetIndex)
        return;

    runtimeState = {};
    runtimeState.mainPresetIndex = safeMainPresetIndex;
}

void MlrVSTAudioProcessor::clearAllSceneClipSlotRuntimeStates()
{
    for (int slot = 0; slot < SceneSlots; ++slot)
        sceneClipSlotRuntimeStates[static_cast<size_t>(slot)] = {};
}

void MlrVSTAudioProcessor::copySceneClipSlotRuntimeState(int mainPresetIndex,
                                                         int sourceSceneSlot,
                                                         int destSceneSlot)
{
    const int safeMainPresetIndex = juce::jlimit(0, MaxPresetSlots - 1, mainPresetIndex);
    const int safeSourceSlot = juce::jlimit(0, SceneSlots - 1, sourceSceneSlot);
    const int safeDestSlot = juce::jlimit(0, SceneSlots - 1, destSceneSlot);

    auto& destState = sceneClipSlotRuntimeStates[static_cast<size_t>(safeDestSlot)];
    const auto& sourceState = sceneClipSlotRuntimeStates[static_cast<size_t>(safeSourceSlot)];
    destState = {};

    if (sourceState.hasLiveStripControls && sourceState.mainPresetIndex == safeMainPresetIndex)
    {
        destState = sourceState;
        destState.mainPresetIndex = safeMainPresetIndex;
    }
}

void MlrVSTAudioProcessor::refreshSceneAutomationTargetMask(int sceneSlot)
{
    const int safeSceneSlot = juce::jlimit(0, SceneSlots - 1, sceneSlot);
    std::array<uint64_t, MaxStrips + 1> masks{};
    const auto events = scenePerformanceRecorder.getEventsSnapshot(safeSceneSlot);

    for (const auto& event : events)
    {
        if (event.type != ScenePerformanceEventType::ControlPoint)
            continue;

        masks[static_cast<size_t>(SceneAutomationRules::automationMaskIndex(event.stripIndex, event.controlTarget))]
            |= SceneAutomationRules::automationTargetBit(event.controlTarget);
    }

    auto& sceneMasks = sceneClipAutomationTargetMasks[static_cast<size_t>(safeSceneSlot)];
    for (size_t i = 0; i < sceneMasks.size(); ++i)
        sceneMasks[i].store(masks[i], std::memory_order_release);

    if (safeSceneSlot == juce::jlimit(0, SceneSlots - 1, activeSceneSlot))
        pruneActiveSceneAutomationOverrides();
}

void MlrVSTAudioProcessor::refreshAllSceneAutomationTargetMasks()
{
    for (int sceneSlot = 0; sceneSlot < SceneSlots; ++sceneSlot)
        refreshSceneAutomationTargetMask(sceneSlot);
}

bool MlrVSTAudioProcessor::sceneClipHasAutomationTarget(int sceneSlot,
                                                        int stripIndex,
                                                        ScenePerformanceControlTarget target) const
{
    const int safeSceneSlot = juce::jlimit(0, SceneSlots - 1, sceneSlot);
    const auto& sceneMasks = sceneClipAutomationTargetMasks[static_cast<size_t>(safeSceneSlot)];
    const uint64_t mask = sceneMasks[static_cast<size_t>(SceneAutomationRules::automationMaskIndex(stripIndex, target))]
        .load(std::memory_order_acquire);
    return (mask & SceneAutomationRules::automationTargetBit(target)) != 0;
}

bool MlrVSTAudioProcessor::isActiveSceneAutomationOverridden(int stripIndex,
                                                             ScenePerformanceControlTarget target) const
{
    const auto& mask = activeSceneAutomationOverrideMasks[static_cast<size_t>(
        SceneAutomationRules::automationMaskIndex(stripIndex, target))];
    return (mask.load(std::memory_order_acquire) & SceneAutomationRules::automationTargetBit(target)) != 0;
}

bool MlrVSTAudioProcessor::hasAnyActiveSceneAutomationOverrides() const
{
    for (const auto& mask : activeSceneAutomationOverrideMasks)
    {
        if (mask.load(std::memory_order_acquire) != 0)
            return true;
    }

    return false;
}

void MlrVSTAudioProcessor::pruneActiveSceneAutomationOverrides()
{
    const int safeSceneSlot = juce::jlimit(0, SceneSlots - 1, activeSceneSlot);
    const auto& sceneMasks = sceneClipAutomationTargetMasks[static_cast<size_t>(safeSceneSlot)];
    for (size_t i = 0; i < activeSceneAutomationOverrideMasks.size(); ++i)
    {
        const uint64_t recordedMask = sceneMasks[i].load(std::memory_order_acquire);
        const uint64_t activeMask = activeSceneAutomationOverrideMasks[i].load(std::memory_order_acquire);
        activeSceneAutomationOverrideMasks[i].store(activeMask & recordedMask, std::memory_order_release);
    }
}

void MlrVSTAudioProcessor::beginSceneManualControlHandlingSuppression()
{
    suppressSceneManualControlHandlingDepth.fetch_add(1, std::memory_order_acq_rel);
}

void MlrVSTAudioProcessor::endSceneManualControlHandlingSuppression()
{
    suppressSceneManualControlHandlingDepth.fetch_sub(1, std::memory_order_acq_rel);
}

bool MlrVSTAudioProcessor::isSceneManualControlHandlingSuppressed() const
{
    return suppressSceneManualControlHandlingDepth.load(std::memory_order_acquire) > 0;
}

void MlrVSTAudioProcessor::requestAbortActiveSceneTransition()
{
    if (auto* payload = preparedSceneSwitchPayloadPublished.exchange(nullptr, std::memory_order_acq_rel))
        retirePreparedSceneSwitchPayload(payload);

    preparedSceneSwitchPayloadSwitchSerial.store(0, std::memory_order_release);
    preparedSceneSwitchPayloadMainPreset.store(-1, std::memory_order_release);
    preparedSceneSwitchPayloadSceneSlot.store(-1, std::memory_order_release);
    preparedSceneSwitchPayloadSequenceDriven.store(0, std::memory_order_release);
    preparedSceneSwitchPayloadSequenceStep.store(-1, std::memory_order_release);
    reclaimRetiredPreparedSceneSwitchPayloads();
}
