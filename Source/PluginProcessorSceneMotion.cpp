/*
  ==============================================================================

    PluginProcessorSceneMotion.cpp
    Extracted scene editor and motion state handling for mlrVST.

  ==============================================================================
*/

#include "PluginProcessor.h"
#include "AudioEngineModulation.h"
#include "PluginEditor.h"
#include "PlayheadSpeedQuantizer.h"
#include "SceneScheduler.h"
#include <limits>

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

bool sceneModTargetsMatch(ModernAudioEngine::ModTarget lhs, ModernAudioEngine::ModTarget rhs) noexcept
{
    return lhs == rhs
        || (lhs == ModernAudioEngine::ModTarget::Pitch && rhs == ModernAudioEngine::ModTarget::GrainPitch)
        || (lhs == ModernAudioEngine::ModTarget::GrainPitch && rhs == ModernAudioEngine::ModTarget::Pitch);
}

int sceneAutomationMaskIndex(int stripIndex, ScenePerformanceControlTarget target) noexcept
{
    if (target == ScenePerformanceControlTarget::Retrigger && stripIndex < 0)
        return MlrVSTAudioProcessor::MaxStrips;

    return juce::jlimit(0, MlrVSTAudioProcessor::MaxStrips - 1, stripIndex);
}

uint64_t sceneAutomationTargetBit(ScenePerformanceControlTarget target) noexcept
{
    if (target == ScenePerformanceControlTarget::None)
        return 0;

    return uint64_t{ 1 } << juce::jlimit(0, 63, static_cast<int>(target));
}

std::vector<ModernAudioEngine::ModTarget> buildVisibleModTargetsForSceneLaneMode(bool grainStrip)
{
    std::vector<ModernAudioEngine::ModTarget> targets{
        ModernAudioEngine::ModTarget::Volume,
        ModernAudioEngine::ModTarget::Pan,
        grainStrip ? ModernAudioEngine::ModTarget::GrainPitch
                   : ModernAudioEngine::ModTarget::Pitch,
        ModernAudioEngine::ModTarget::Cutoff,
        ModernAudioEngine::ModTarget::Resonance,
        ModernAudioEngine::ModTarget::FilterMorph,
        ModernAudioEngine::ModTarget::Speed,
        ModernAudioEngine::ModTarget::Retrigger,
        ModernAudioEngine::ModTarget::SliceLength,
        ModernAudioEngine::ModTarget::Scratch,
        ModernAudioEngine::ModTarget::DelayMix,
        ModernAudioEngine::ModTarget::DelayTime,
        ModernAudioEngine::ModTarget::DelayFeedback
    };

    if (grainStrip)
    {
        targets.push_back(ModernAudioEngine::ModTarget::GrainSize);
        targets.push_back(ModernAudioEngine::ModTarget::GrainDensity);
        targets.push_back(ModernAudioEngine::ModTarget::GrainPitchJitter);
        targets.push_back(ModernAudioEngine::ModTarget::GrainSpread);
        targets.push_back(ModernAudioEngine::ModTarget::GrainJitter);
        targets.push_back(ModernAudioEngine::ModTarget::GrainPositionJitter);
        targets.push_back(ModernAudioEngine::ModTarget::GrainRandom);
        targets.push_back(ModernAudioEngine::ModTarget::GrainArp);
        targets.push_back(ModernAudioEngine::ModTarget::GrainCloud);
        targets.push_back(ModernAudioEngine::ModTarget::GrainEmitter);
        targets.push_back(ModernAudioEngine::ModTarget::GrainEnvelope);
        targets.push_back(ModernAudioEngine::ModTarget::GrainShape);
    }

    return targets;
}

ScenePerformanceRecorder::MotionStripState makeDefaultSceneMotionStripState()
{
    ScenePerformanceRecorder::MotionStripState stripState;
    stripState.activeSlot = 0;

    for (int slot = 0; slot < ModernAudioEngine::NumModSequencers; ++slot)
    {
        auto& lane = stripState.lanes[static_cast<size_t>(slot)];
        const auto target = ModernAudioEngine::defaultModTargetForSlot(slot);
        const float defaultValue = ModernAudioEngine::defaultModStepValueForTarget(target);

        lane = {};
        lane.target = target;
        lane.bipolar = ModernAudioEngine::modTargetSupportsBipolar(target)
            && AudioEngineModulationInternal::modTargetAutoDefaultBipolar(target);
        lane.curveMode = false;
        lane.depth = 1.0f;
        lane.rate = 1.0f;
        lane.transportMode = ModernAudioEngine::ModTransportMode::Sync;
        lane.offset = 0;
        lane.lengthBars = 1;
        lane.editPage = 0;
        lane.smoothingMs = 0.0f;
        lane.curveBend = 0.0f;
        lane.curveShape = ModernAudioEngine::ModCurveShape::Linear;
        lane.pitchScaleQuantize = false;
        lane.pitchScale = ModernAudioEngine::PitchScale::Chromatic;
        lane.steps.fill(defaultValue);
        lane.stepSubdivisions.fill(1);
        lane.stepEndValues.fill(defaultValue);
        lane.stepCurveShapes.fill(ModernAudioEngine::ModCurveShape::Linear);
    }

    return stripState;
}
} // namespace

bool MlrVSTAudioProcessor::getSceneEditorStripAutomationExpanded(int stripIndex) const
{
    const int safeStripIndex = juce::jlimit(0, MaxStrips - 1, stripIndex);
    return sceneEditorStripAutomationExpanded[static_cast<size_t>(safeStripIndex)];
}

bool MlrVSTAudioProcessor::getSceneEditorStripHeightExpanded(int stripIndex) const
{
    const int safeStripIndex = juce::jlimit(0, MaxStrips - 1, stripIndex);
    return sceneEditorStripHeightExpanded[static_cast<size_t>(safeStripIndex)];
}

void MlrVSTAudioProcessor::setSceneEditorGridEnabled(bool enabled)
{
    if (sceneEditorGridEnabledState == enabled)
        return;

    sceneEditorGridEnabledState = enabled;
    markPersistentGlobalUserChange();
}

void MlrVSTAudioProcessor::setSceneEditorGridDivision(int division)
{
    const int safeDivision = juce::jlimit(1, 64, division);
    if (sceneEditorGridDivisionState == safeDivision)
        return;

    sceneEditorGridDivisionState = safeDivision;
    markPersistentGlobalUserChange();
}

void MlrVSTAudioProcessor::setSceneEditorDrawModeEnabled(bool enabled)
{
    if (sceneEditorDrawModeEnabledState == enabled)
        return;

    sceneEditorDrawModeEnabledState = enabled;
    markPersistentGlobalUserChange();
}

void MlrVSTAudioProcessor::setSceneEditorLaneOverlaysEnabled(bool enabled)
{
    if (sceneEditorLaneOverlaysEnabledState == enabled)
        return;

    sceneEditorLaneOverlaysEnabledState = enabled;
    markPersistentGlobalUserChange();
}

void MlrVSTAudioProcessor::setSceneEditorZoomFactor(int factor)
{
    static constexpr std::array<int, 7> kZoomChoices{ 1, 2, 3, 4, 5, 6, 8 };
    int safeFactor = kZoomChoices.front();
    int bestDelta = std::abs(factor - safeFactor);
    for (const int candidate : kZoomChoices)
    {
        const int delta = std::abs(factor - candidate);
        if (delta < bestDelta)
        {
            bestDelta = delta;
            safeFactor = candidate;
        }
    }
    if (sceneEditorZoomFactorState == safeFactor)
        return;

    sceneEditorZoomFactorState = safeFactor;
    markPersistentGlobalUserChange();
}

void MlrVSTAudioProcessor::setSceneEditorFollowPlayheadEnabled(bool enabled)
{
    if (sceneEditorFollowPlayheadState == enabled)
        return;

    sceneEditorFollowPlayheadState = enabled;
    markPersistentGlobalUserChange();
}

void MlrVSTAudioProcessor::setSceneModPageMode(SceneModPageMode mode)
{
    if (sceneModPageModeState == mode)
        return;

    sceneModPageModeState = mode;
    if (mode == SceneModPageMode::MainModulation)
    {
        for (int stripIndex = 0; stripIndex < MaxStrips; ++stripIndex)
        {
            const auto targets = getSceneVisibleModTargetsForStrip(stripIndex);
            ModernAudioEngine::ModTarget resolvedTarget = ModernAudioEngine::ModTarget::None;
            const auto liveTarget = audioEngine != nullptr
                ? sanitizeModPerformanceTarget(audioEngine->getModTarget(stripIndex))
                : ModernAudioEngine::ModTarget::None;

            for (const auto candidate : targets)
            {
                if (sceneModTargetsMatch(candidate, liveTarget))
                {
                    resolvedTarget = sanitizeModPerformanceTarget(candidate);
                    break;
                }
            }

            if (resolvedTarget == ModernAudioEngine::ModTarget::None && !targets.empty())
                resolvedTarget = sanitizeModPerformanceTarget(targets.front());

            sceneMainAutomationDisplayTargets[static_cast<size_t>(stripIndex)] = resolvedTarget;
        }
    }
    markPersistentGlobalUserChange();
}

void MlrVSTAudioProcessor::requestSceneControlRefreshAsync()
{
    juce::MessageManager::callAsync([this]()
    {
        if (auto* editor = dynamic_cast<MlrVSTAudioProcessorEditor*>(getActiveEditor()))
            editor->refreshSceneControlNow();
    });
}

void MlrVSTAudioProcessor::syncActiveSceneMotionState()
{
    if (!isSceneModeEnabled())
        return;

    syncSceneMotionStateFromEngine(getActiveSceneSlot());
    queueActiveSceneAutosave();
}

void MlrVSTAudioProcessor::syncFocusedSceneMotionState()
{
    if (!isSceneModeEnabled())
        return;

    const int syncedSceneSlot = getFocusedSceneSlot();
    syncSceneMotionStateFromEngine(syncedSceneSlot);
    if (syncedSceneSlot == getActiveSceneSlot())
        queueActiveSceneAutosave();
}

void MlrVSTAudioProcessor::setSceneEditorStripAutomationExpanded(int stripIndex, bool expanded)
{
    const int safeStripIndex = juce::jlimit(0, MaxStrips - 1, stripIndex);
    auto& state = sceneEditorStripAutomationExpanded[static_cast<size_t>(safeStripIndex)];
    if (state == expanded)
        return;

    state = expanded;
    markPersistentGlobalUserChange();
}

void MlrVSTAudioProcessor::setSceneEditorStripHeightExpanded(int stripIndex, bool expanded)
{
    const int safeStripIndex = juce::jlimit(0, MaxStrips - 1, stripIndex);
    auto& state = sceneEditorStripHeightExpanded[static_cast<size_t>(safeStripIndex)];
    if (state == expanded)
        return;

    state = expanded;
    markPersistentGlobalUserChange();
}

void MlrVSTAudioProcessor::setSceneEditorStripAutomationExpandedAll(bool expanded)
{
    bool anyChanged = false;
    for (auto& state : sceneEditorStripAutomationExpanded)
    {
        if (state == expanded)
            continue;
        state = expanded;
        anyChanged = true;
    }

    if (anyChanged)
        markPersistentGlobalUserChange();
}

void MlrVSTAudioProcessor::requestSceneEditorStripFocus(int stripIndex, bool collapseOthers)
{
    const int safeStripIndex = juce::jlimit(0, MaxStrips - 1, stripIndex);
    pendingSceneEditorFocusCollapseOthers.store(collapseOthers ? 1 : 0, std::memory_order_release);
    pendingSceneEditorFocusStrip.store(safeStripIndex, std::memory_order_release);
    requestSceneControlRefreshAsync();
}

bool MlrVSTAudioProcessor::consumePendingSceneEditorStripFocus(int& stripIndex, bool& collapseOthers)
{
    const int pendingStripIndex = pendingSceneEditorFocusStrip.exchange(-1, std::memory_order_acq_rel);
    if (pendingStripIndex < 0 || pendingStripIndex >= MaxStrips)
        return false;

    stripIndex = pendingStripIndex;
    collapseOthers = pendingSceneEditorFocusCollapseOthers.exchange(0, std::memory_order_acq_rel) != 0;
    return true;
}

bool MlrVSTAudioProcessor::replaceScenePerformanceClipEvents(int sceneSlot,
                                                             const std::vector<ScenePerformanceEvent>& events)
{
    if (scenePerformanceRecorder.isRecording())
        return false;

    const int safeSceneSlot = juce::jlimit(0, SceneSlots - 1, sceneSlot);
    const double resolvedLengthBeats = getResolvedSceneLengthBeats(safeSceneSlot);
    const bool replaced = scenePerformanceRecorder.replaceClipEvents(safeSceneSlot, events, resolvedLengthBeats);
    if (!replaced)
        return false;

    refreshSceneAutomationTargetMask(safeSceneSlot);

    if (safeSceneSlot == activeSceneSlot)
    {
        lastScenePerformanceProcessBeat = std::numeric_limits<double>::quiet_NaN();
        lastScenePerformanceProcessSceneSlot = -1;
        lastScenePerformanceProcessSceneStartBeat = std::numeric_limits<double>::quiet_NaN();
        queueActiveSceneAutosave();
    }

    updateMonomeLEDs();
    return true;
}

void MlrVSTAudioProcessor::syncScenePerformanceClipLengthToResolvedLength(int sceneSlot)
{
    const int safeSceneSlot = juce::jlimit(0, SceneSlots - 1, sceneSlot);
    scenePerformanceRecorder.setClipLengthBeats(safeSceneSlot, getResolvedSceneLengthBeats(safeSceneSlot));

    if (safeSceneSlot == activeSceneSlot)
    {
        lastScenePerformanceProcessBeat = std::numeric_limits<double>::quiet_NaN();
        lastScenePerformanceProcessSceneSlot = -1;
        lastScenePerformanceProcessSceneStartBeat = std::numeric_limits<double>::quiet_NaN();
    }
}

void MlrVSTAudioProcessor::syncAllScenePerformanceClipLengthsToResolvedLengths()
{
    for (int sceneSlot = 0; sceneSlot < SceneSlots; ++sceneSlot)
        syncScenePerformanceClipLengthToResolvedLength(sceneSlot);
}

std::unique_ptr<juce::XmlElement> MlrVSTAudioProcessor::createSceneChainStateXml(int sceneSlotOverride) const
{
    return SceneScheduler::createSceneChainStateXml(*this, sceneSlotOverride);
}

void MlrVSTAudioProcessor::applySceneChainStateXml(const juce::XmlElement* xml, int sceneSlotOverride)
{
    ScopedSceneAutosaveSuppression suppressSceneAutosave(*this);
    SceneScheduler::applySceneChainStateXml(*this, xml, sceneSlotOverride);
    if (sceneSlotOverride < 0)
        reloadAllSceneChainTransitionEndSamples();
}

juce::MemoryBlock MlrVSTAudioProcessor::createScenePerformanceStateData(int sceneSlotOverride) const
{
    std::array<double, SceneSlots> canonicalLengths{};
    for (int sceneSlot = 0; sceneSlot < SceneSlots; ++sceneSlot)
        canonicalLengths[static_cast<size_t>(sceneSlot)] = getResolvedSceneLengthBeats(sceneSlot);

    return scenePerformanceRecorder.createData(sceneSlotOverride, &canonicalLengths);
}

bool MlrVSTAudioProcessor::applyScenePerformanceStateData(const juce::MemoryBlock& data, int sceneSlotOverride)
{
    const bool applied = scenePerformanceRecorder.applyData(data, sceneSlotOverride);
    if (!applied)
        return false;

    if (sceneSlotOverride >= 0)
    {
        syncScenePerformanceClipLengthToResolvedLength(sceneSlotOverride);
        refreshSceneAutomationTargetMask(sceneSlotOverride);
    }
    else
    {
        syncAllScenePerformanceClipLengthsToResolvedLengths();
        refreshAllSceneAutomationTargetMasks();
    }

    if (isSceneModeEnabled())
    {
        const int activeScene = juce::jlimit(0, SceneSlots - 1, activeSceneSlot);
        const int targetScene = sceneSlotOverride >= 0
            ? juce::jlimit(0, SceneSlots - 1, sceneSlotOverride)
            : activeScene;
        if (targetScene == activeScene && sceneSlotHasMotionState(targetScene))
            applySceneMotionStateToEngine(targetScene);
    }

    return true;
}

bool MlrVSTAudioProcessor::sceneSlotHasMotionState(int sceneSlot) const
{
    return scenePerformanceRecorder.hasMotionState(sceneSlot);
}

void MlrVSTAudioProcessor::syncSceneMotionStateFromEngine(int sceneSlot)
{
    if (audioEngine == nullptr)
        return;

    const int safeSceneSlot = juce::jlimit(0, SceneSlots - 1, sceneSlot);
    for (int stripIndex = 0; stripIndex < MaxStrips; ++stripIndex)
    {
        ScenePerformanceRecorder::MotionStripState stripState;
        stripState.activeSlot = audioEngine->getModSequencerSlot(stripIndex);

        for (int slot = 0; slot < ModernAudioEngine::NumModSequencers; ++slot)
        {
            auto& lane = stripState.lanes[static_cast<size_t>(slot)];
            lane.target = audioEngine->getModTargetForSlot(stripIndex, slot);
            lane.bipolar = audioEngine->isModBipolarForSlot(stripIndex, slot);
            lane.curveMode = audioEngine->isModCurveModeForSlot(stripIndex, slot);
            lane.depth = audioEngine->getModDepthForSlot(stripIndex, slot);
            lane.rate = audioEngine->getModRateForSlot(stripIndex, slot);
            lane.transportMode = audioEngine->getModTransportModeForSlot(stripIndex, slot);
            lane.offset = audioEngine->getModOffsetForSlot(stripIndex, slot);
            lane.lengthBars = audioEngine->getModLengthBarsForSlot(stripIndex, slot);
            lane.editPage = audioEngine->getModEditPageForSlot(stripIndex, slot);
            lane.smoothingMs = audioEngine->getModSmoothingMsForSlot(stripIndex, slot);
            lane.curveBend = audioEngine->getModCurveBendForSlot(stripIndex, slot);
            lane.curveShape = audioEngine->getModCurveShapeForSlot(stripIndex, slot);
            lane.pitchScaleQuantize = audioEngine->isModPitchScaleQuantizeForSlot(stripIndex, slot);
            lane.pitchScale = audioEngine->getModPitchScaleForSlot(stripIndex, slot);

            for (int stepIndex = 0; stepIndex < ModernAudioEngine::ModTotalSteps; ++stepIndex)
            {
                const auto index = static_cast<size_t>(stepIndex);
                lane.steps[index] = audioEngine->getModStepValueAbsoluteForSlot(stripIndex, slot, stepIndex);
                lane.stepSubdivisions[index] = audioEngine->getModStepSubdivisionAbsoluteForSlot(stripIndex, slot, stepIndex);
                lane.stepEndValues[index] = audioEngine->getModStepEndValueAbsoluteForSlot(stripIndex, slot, stepIndex);
                lane.stepCurveShapes[index] = audioEngine->getModStepCurveShapeAbsoluteForSlot(stripIndex, slot, stepIndex);
            }
        }

        scenePerformanceRecorder.setMotionStripState(safeSceneSlot, stripIndex, stripState);
    }
}

void MlrVSTAudioProcessor::ensureSceneMotionStateInitialized(int sceneSlot)
{
    const int safeSceneSlot = juce::jlimit(0, SceneSlots - 1, sceneSlot);
    if (scenePerformanceRecorder.hasMotionState(safeSceneSlot))
        return;

    const auto defaultStripState = makeDefaultSceneMotionStripState();
    for (int stripIndex = 0; stripIndex < MaxStrips; ++stripIndex)
        scenePerformanceRecorder.setMotionStripState(safeSceneSlot, stripIndex, defaultStripState);
}

void MlrVSTAudioProcessor::applySceneMotionStateToEngine(int sceneSlot)
{
    if (audioEngine == nullptr)
        return;

    const int safeSceneSlot = juce::jlimit(0, SceneSlots - 1, sceneSlot);
    if (!scenePerformanceRecorder.hasMotionState(safeSceneSlot))
        return;

    for (int stripIndex = 0; stripIndex < MaxStrips; ++stripIndex)
    {
        const auto stripState = scenePerformanceRecorder.getMotionStripState(safeSceneSlot, stripIndex);

        for (int slot = 0; slot < ModernAudioEngine::NumModSequencers; ++slot)
        {
            const auto& lane = stripState.lanes[static_cast<size_t>(slot)];
            audioEngine->setModSequencerSlot(stripIndex, slot);
            audioEngine->setModTarget(stripIndex, lane.target);
            audioEngine->setModBipolar(stripIndex,
                                       lane.bipolar,
                                       ModernAudioEngine::ModBipolarToggleMode::Reinterpret);
            audioEngine->setModCurveMode(stripIndex, lane.curveMode);
            audioEngine->setModDepth(stripIndex, lane.depth);
            audioEngine->setModRate(stripIndex, lane.rate);
            audioEngine->setModTransportMode(stripIndex, lane.transportMode);
            audioEngine->setModOffset(stripIndex, lane.offset);
            audioEngine->setModLengthBars(stripIndex, lane.lengthBars);
            audioEngine->setModEditPage(stripIndex, lane.editPage);
            audioEngine->setModSmoothingMs(stripIndex, lane.smoothingMs);
            audioEngine->setModCurveBend(stripIndex, lane.curveBend);
            audioEngine->setModCurveShape(stripIndex, lane.curveShape);
            audioEngine->setModPitchScaleQuantize(stripIndex, lane.pitchScaleQuantize);
            audioEngine->setModPitchScale(stripIndex, lane.pitchScale);

            for (int stepIndex = 0; stepIndex < ModernAudioEngine::ModTotalSteps; ++stepIndex)
            {
                const auto index = static_cast<size_t>(stepIndex);
                audioEngine->setModStepValueAbsolute(stripIndex, stepIndex, lane.steps[index]);
                audioEngine->setModStepShapeAbsolute(stripIndex,
                                                    stepIndex,
                                                    lane.stepSubdivisions[index],
                                                    lane.stepEndValues[index]);
                audioEngine->setModStepCurveShapeAbsolute(stripIndex,
                                                          stepIndex,
                                                          lane.stepCurveShapes[index]);
            }
        }

        audioEngine->setModSequencerSlot(stripIndex, stripState.activeSlot);
    }
}

void MlrVSTAudioProcessor::applySceneMotionStateOrDefaultsToEngine(int sceneSlot)
{
    if (audioEngine == nullptr)
        return;

    const int safeSceneSlot = juce::jlimit(0, SceneSlots - 1, sceneSlot);
    ensureSceneMotionStateInitialized(safeSceneSlot);
    applySceneMotionStateToEngine(safeSceneSlot);
}

void MlrVSTAudioProcessor::clearSceneMotionStripState(int sceneSlot, int stripIndex)
{
    const int safeSceneSlot = juce::jlimit(0, SceneSlots - 1, sceneSlot);
    const int safeStripIndex = juce::jlimit(0, MaxStrips - 1, stripIndex);
    scenePerformanceRecorder.setMotionStripState(safeSceneSlot,
                                                 safeStripIndex,
                                                 makeDefaultSceneMotionStripState());

    if (audioEngine != nullptr
        && isSceneModeEnabled()
        && safeSceneSlot == juce::jlimit(0, SceneSlots - 1, activeSceneSlot))
    {
        for (int slot = 0; slot < ModernAudioEngine::NumModSequencers; ++slot)
            audioEngine->resetModSequencerSlotToDefaults(safeStripIndex, slot);
        audioEngine->setModSequencerSlot(safeStripIndex, 0);
    }

    if (safeSceneSlot == juce::jlimit(0, SceneSlots - 1, activeSceneSlot))
        queueActiveSceneAutosave();
}

void MlrVSTAudioProcessor::restoreSceneStripControlTargetsToStoredState(
    int sceneSlot,
    int stripIndex,
    const std::vector<ScenePerformanceControlTarget>& targets)
{
    restoreSceneStripControlTargetsToDefaultState(sceneSlot, stripIndex, targets);
}

void MlrVSTAudioProcessor::restoreSceneStripControlTargetsToDefaultState(
    int sceneSlot,
    int stripIndex,
    const std::vector<ScenePerformanceControlTarget>& targets)
{
    if (targets.empty()
        || !isSceneModeEnabled()
        || audioEngine == nullptr)
    {
        return;
    }

    const int safeSceneSlot = juce::jlimit(0, SceneSlots - 1, sceneSlot);
    const int safeStripIndex = juce::jlimit(0, MaxStrips - 1, stripIndex);
    if (safeSceneSlot != juce::jlimit(0, SceneSlots - 1, activeSceneSlot))
        return;

    auto* strip = audioEngine->getStrip(safeStripIndex);
    if (strip == nullptr)
        return;

    const ResolvedOwnedStripControlState defaultOwnedControls;
    const bool usesGrainPlaybackSpeed = (strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Grain);

    bool restoreVolume = false;
    bool restorePan = false;
    bool restoreSpeed = false;
    bool restorePitch = false;
    bool restoreSwing = false;
    bool restoreFilter = false;
    bool restoreSliceLength = false;
    bool restoreScratch = false;
    bool restoreDelay = false;
    bool restoreGrainSize = false;
    bool restoreGrainDensity = false;
    bool restoreGrainPitch = false;
    bool restoreGrainPitchJitter = false;
    bool restoreGrainSpread = false;
    bool restoreGrainJitter = false;
    bool restoreGrainPositionJitter = false;
    bool restoreGrainRandomDepth = false;
    bool restoreGrainArp = false;
    bool restoreGrainCloud = false;
    bool restoreGrainEmitter = false;
    bool restoreGrainEnvelope = false;
    bool restoreGrainShape = false;
    bool clearRearrange = false;

    for (const auto target : targets)
    {
        if (target != ScenePerformanceControlTarget::None
            && target != ScenePerformanceControlTarget::Retrigger)
        {
            auto& overrideMask = activeSceneAutomationOverrideMasks[static_cast<size_t>(
                sceneAutomationMaskIndex(safeStripIndex, target))];
            overrideMask.fetch_and(~sceneAutomationTargetBit(target), std::memory_order_acq_rel);
        }

        switch (target)
        {
            case ScenePerformanceControlTarget::Volume:
                restoreVolume = true;
                break;
            case ScenePerformanceControlTarget::Pan:
                restorePan = true;
                break;
            case ScenePerformanceControlTarget::Speed:
                restoreSpeed = true;
                break;
            case ScenePerformanceControlTarget::Pitch:
                restorePitch = true;
                break;
            case ScenePerformanceControlTarget::Swing:
                restoreSwing = true;
                break;
            case ScenePerformanceControlTarget::FilterFrequency:
            case ScenePerformanceControlTarget::FilterResonance:
            case ScenePerformanceControlTarget::FilterEnabled:
            case ScenePerformanceControlTarget::FilterMorph:
                restoreFilter = true;
                break;
            case ScenePerformanceControlTarget::SliceLength:
                restoreSliceLength = true;
                break;
            case ScenePerformanceControlTarget::Scratch:
                restoreScratch = true;
                break;
            case ScenePerformanceControlTarget::DelayMix:
            case ScenePerformanceControlTarget::DelayTime:
            case ScenePerformanceControlTarget::DelayFeedback:
            case ScenePerformanceControlTarget::DelayLowCut:
            case ScenePerformanceControlTarget::DelayHighCut:
            case ScenePerformanceControlTarget::DelayMode:
            case ScenePerformanceControlTarget::DelaySyncEnabled:
                restoreDelay = true;
                break;
            case ScenePerformanceControlTarget::GrainSize:
                restoreGrainSize = true;
                break;
            case ScenePerformanceControlTarget::GrainDensity:
                restoreGrainDensity = true;
                break;
            case ScenePerformanceControlTarget::GrainPitch:
                restoreGrainPitch = true;
                break;
            case ScenePerformanceControlTarget::GrainPitchJitter:
                restoreGrainPitchJitter = true;
                break;
            case ScenePerformanceControlTarget::GrainSpread:
                restoreGrainSpread = true;
                break;
            case ScenePerformanceControlTarget::GrainJitter:
                restoreGrainJitter = true;
                break;
            case ScenePerformanceControlTarget::GrainPositionJitter:
                restoreGrainPositionJitter = true;
                break;
            case ScenePerformanceControlTarget::GrainRandomDepth:
                restoreGrainRandomDepth = true;
                break;
            case ScenePerformanceControlTarget::GrainArp:
                restoreGrainArp = true;
                break;
            case ScenePerformanceControlTarget::GrainCloud:
                restoreGrainCloud = true;
                break;
            case ScenePerformanceControlTarget::GrainEmitter:
                restoreGrainEmitter = true;
                break;
            case ScenePerformanceControlTarget::GrainEnvelope:
                restoreGrainEnvelope = true;
                break;
            case ScenePerformanceControlTarget::GrainShape:
                restoreGrainShape = true;
                break;
            case ScenePerformanceControlTarget::Rearrange:
                clearRearrange = true;
                break;
            case ScenePerformanceControlTarget::Retrigger:
            case ScenePerformanceControlTarget::None:
            default:
                break;
        }
    }

    if (restoreVolume)
        setStripVolumeControlValue(safeStripIndex, defaultOwnedControls.volume, StripControlWriteMode::CacheOnly);
    if (restorePan)
        setStripPanControlValue(safeStripIndex, defaultOwnedControls.pan, StripControlWriteMode::CacheOnly);
    if (restoreSpeed)
    {
        const float defaultSpeedControl = usesGrainPlaybackSpeed
            ? PlayheadSpeedQuantizer::grainControlValueFromPlaybackSpeed(defaultOwnedControls.playbackSpeed)
            : defaultOwnedControls.playheadSpeedRatio;
        setStripSpeedControlValue(safeStripIndex, defaultSpeedControl, StripControlWriteMode::CacheOnly);
    }

    if (restorePitch)
    {
        writeStripFloatParameter("stripPitch" + juce::String(safeStripIndex),
                                 0.0f,
                                 stripPitchParams[static_cast<size_t>(safeStripIndex)],
                                 StripControlWriteMode::CacheOnly);
        applyPitchControlToStrip(safeStripIndex, *strip, 0.0f);
    }

    if (restoreFilter)
    {
        writeStripBoolParameter("stripFilterEnabled" + juce::String(safeStripIndex),
                                defaultOwnedControls.filterEnabled,
                                stripFilterEnabledParams[static_cast<size_t>(safeStripIndex)],
                                StripControlWriteMode::CacheOnly);
        writeStripFloatParameter("stripFilterFrequency" + juce::String(safeStripIndex),
                                 defaultOwnedControls.filterFrequency,
                                 stripFilterFrequencyParams[static_cast<size_t>(safeStripIndex)],
                                 StripControlWriteMode::CacheOnly);
        writeStripFloatParameter("stripFilterResonance" + juce::String(safeStripIndex),
                                 defaultOwnedControls.filterResonance,
                                 stripFilterResonanceParams[static_cast<size_t>(safeStripIndex)],
                                 StripControlWriteMode::CacheOnly);
        writeStripFloatParameter("stripFilterMorph" + juce::String(safeStripIndex),
                                 defaultOwnedControls.filterMorph,
                                 stripFilterMorphParams[static_cast<size_t>(safeStripIndex)],
                                 StripControlWriteMode::CacheOnly);
        writeStripFloatParameter("stripFilterAlgorithm" + juce::String(safeStripIndex),
                                 static_cast<float>(static_cast<int>(defaultOwnedControls.filterAlgorithm)),
                                 stripFilterAlgorithmParams[static_cast<size_t>(safeStripIndex)],
                                 StripControlWriteMode::CacheOnly);
        applyResolvedStripFilterState(*strip, defaultOwnedControls);
    }

    if (restoreDelay)
    {
        writeStripFloatParameter("stripDelayMix" + juce::String(safeStripIndex),
                                 defaultOwnedControls.delayMix,
                                 stripDelayMixParams[static_cast<size_t>(safeStripIndex)],
                                 StripControlWriteMode::CacheOnly);
        writeStripFloatParameter("stripDelayTime" + juce::String(safeStripIndex),
                                 defaultOwnedControls.delayTime,
                                 stripDelayTimeParams[static_cast<size_t>(safeStripIndex)],
                                 StripControlWriteMode::CacheOnly);
        writeStripBoolParameter("stripDelaySync" + juce::String(safeStripIndex),
                                defaultOwnedControls.delaySyncEnabled,
                                stripDelaySyncParams[static_cast<size_t>(safeStripIndex)],
                                StripControlWriteMode::CacheOnly);
        writeStripFloatParameter("stripDelayFeedback" + juce::String(safeStripIndex),
                                 defaultOwnedControls.delayFeedback,
                                 stripDelayFeedbackParams[static_cast<size_t>(safeStripIndex)],
                                 StripControlWriteMode::CacheOnly);
        writeStripFloatParameter("stripDelayLowCut" + juce::String(safeStripIndex),
                                 defaultOwnedControls.delayLowCutHz,
                                 stripDelayLowCutParams[static_cast<size_t>(safeStripIndex)],
                                 StripControlWriteMode::CacheOnly);
        writeStripFloatParameter("stripDelayHighCut" + juce::String(safeStripIndex),
                                 defaultOwnedControls.delayHighCutHz,
                                 stripDelayHighCutParams[static_cast<size_t>(safeStripIndex)],
                                 StripControlWriteMode::CacheOnly);
        writeStripFloatParameter("stripDelayMode" + juce::String(safeStripIndex),
                                 static_cast<float>(static_cast<int>(defaultOwnedControls.delayMode)),
                                 stripDelayModeParams[static_cast<size_t>(safeStripIndex)],
                                 StripControlWriteMode::CacheOnly);
        applyResolvedStripDelayState(*strip, defaultOwnedControls);
    }

    if (restoreSwing)
        strip->setSwingAmount(0.0f);
    if (restoreSliceLength)
    {
        writeStripFloatParameter("stripSliceLength" + juce::String(safeStripIndex),
                                 1.0f,
                                 stripSliceLengthParams[static_cast<size_t>(safeStripIndex)],
                                 StripControlWriteMode::CacheOnly);
        strip->setLoopSliceLength(1.0f);
    }
    if (restoreScratch)
        strip->setScratchAmount(0.0f);
    if (restoreGrainSize)
        strip->setGrainSizeMs(1240.0f);
    if (restoreGrainDensity)
        strip->setGrainDensity(0.05f);
    if (restoreGrainPitch)
        strip->setGrainPitch(0.0f);
    if (restoreGrainPitchJitter)
        strip->setGrainPitchJitter(0.0f);
    if (restoreGrainSpread)
        strip->setGrainSpread(0.0f);
    if (restoreGrainJitter)
        strip->setGrainJitter(0.0f);
    if (restoreGrainPositionJitter)
        strip->setGrainPositionJitter(0.0f);
    if (restoreGrainRandomDepth)
        strip->setGrainRandomDepth(0.0f);
    if (restoreGrainArp)
        strip->setGrainArpDepth(0.0f);
    if (restoreGrainCloud)
        strip->setGrainCloudDepth(0.0f);
    if (restoreGrainEmitter)
        strip->setGrainEmitterDepth(0.0f);
    if (restoreGrainEnvelope)
        strip->setGrainEnvelope(0.0f);
    if (restoreGrainShape)
        strip->setGrainShape(0.0f);
    if (clearRearrange)
        strip->clearTraversalRearrange();

    syncActiveSceneClipSlotRuntimeStateFromEngine(false);
    updateMonomeLEDs();
}

void MlrVSTAudioProcessor::copySceneMotionStripState(int sceneSlot, int sourceStripIndex, int destStripIndex)
{
    const int safeSceneSlot = juce::jlimit(0, SceneSlots - 1, sceneSlot);
    const int safeSourceStripIndex = juce::jlimit(0, MaxStrips - 1, sourceStripIndex);
    const int safeDestStripIndex = juce::jlimit(0, MaxStrips - 1, destStripIndex);
    if (safeSourceStripIndex == safeDestStripIndex)
        return;

    scenePerformanceRecorder.setMotionStripState(
        safeSceneSlot,
        safeDestStripIndex,
        scenePerformanceRecorder.getMotionStripState(safeSceneSlot, safeSourceStripIndex));

    if (audioEngine != nullptr
        && isSceneModeEnabled()
        && safeSceneSlot == juce::jlimit(0, SceneSlots - 1, activeSceneSlot))
    {
        applySceneMotionStateToEngine(safeSceneSlot);
        queueActiveSceneAutosave();
    }
}

bool MlrVSTAudioProcessor::stripUsesGrainSceneLanes(int stripIndex) const
{
    if (audioEngine == nullptr)
        return false;

    auto* strip = audioEngine->getStrip(juce::jlimit(0, MaxStrips - 1, stripIndex));
    return strip != nullptr && strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Grain;
}

std::vector<ModernAudioEngine::ModTarget> MlrVSTAudioProcessor::getVisibleModTargetsForStrip(int stripIndex) const
{
    return buildVisibleModTargetsForSceneLaneMode(stripUsesGrainSceneLanes(stripIndex));
}

std::vector<ModernAudioEngine::ModTarget> MlrVSTAudioProcessor::getSceneVisibleModTargetsForStrip(int stripIndex) const
{
    return buildVisibleModTargetsForSceneLaneMode(
        stripUsesGrainSceneLanesForSceneSlot(getFocusedSceneSlot(), stripIndex));
}

ModernAudioEngine::ModTarget MlrVSTAudioProcessor::getSceneMainAutomationDisplayTargetForStrip(int stripIndex) const
{
    if (audioEngine == nullptr)
        return ModernAudioEngine::ModTarget::None;

    const int safeStripIndex = juce::jlimit(0, MaxStrips - 1, stripIndex);
    const auto targets = getSceneVisibleModTargetsForStrip(safeStripIndex);
    if (targets.empty())
        return ModernAudioEngine::ModTarget::None;

    const auto storedTarget = sanitizeModPerformanceTarget(
        sceneMainAutomationDisplayTargets[static_cast<size_t>(safeStripIndex)]);
    for (const auto candidate : targets)
    {
        if (sceneModTargetsMatch(candidate, storedTarget))
            return sanitizeModPerformanceTarget(candidate);
    }

    const auto liveTarget = sanitizeModPerformanceTarget(audioEngine->getModTarget(safeStripIndex));
    for (const auto candidate : targets)
    {
        if (sceneModTargetsMatch(candidate, liveTarget))
            return sanitizeModPerformanceTarget(candidate);
    }

    return sanitizeModPerformanceTarget(targets.front());
}

void MlrVSTAudioProcessor::setSceneMainAutomationDisplayTargetForStrip(int stripIndex,
                                                                       ModernAudioEngine::ModTarget target)
{
    const int safeStripIndex = juce::jlimit(0, MaxStrips - 1, stripIndex);
    const auto targets = getSceneVisibleModTargetsForStrip(safeStripIndex);
    const auto safeTarget = sanitizeModPerformanceTarget(target);

    ModernAudioEngine::ModTarget resolvedTarget = ModernAudioEngine::ModTarget::None;
    for (const auto candidate : targets)
    {
        if (sceneModTargetsMatch(candidate, safeTarget))
        {
            resolvedTarget = sanitizeModPerformanceTarget(candidate);
            break;
        }
    }

    if (resolvedTarget == ModernAudioEngine::ModTarget::None && !targets.empty())
        resolvedTarget = sanitizeModPerformanceTarget(targets.front());

    sceneMainAutomationDisplayTargets[static_cast<size_t>(safeStripIndex)] = resolvedTarget;
}

ModernAudioEngine::ModTarget MlrVSTAudioProcessor::getSceneMotionTargetForSlot(int stripIndex, int laneSlot) const
{
    if (audioEngine == nullptr)
        return ModernAudioEngine::ModTarget::None;

    const int safeStripIndex = juce::jlimit(0, MaxStrips - 1, stripIndex);
    const int safeSlot = juce::jlimit(0, ModernAudioEngine::NumModSequencers - 1, laneSlot);
    return audioEngine->getModTargetForSlot(safeStripIndex, safeSlot);
}

void MlrVSTAudioProcessor::setSceneMotionTargetForSlot(int stripIndex,
                                                       int laneSlot,
                                                       ModernAudioEngine::ModTarget target)
{
    if (audioEngine == nullptr)
        return;

    const int safeStripIndex = juce::jlimit(0, MaxStrips - 1, stripIndex);
    const int safeSlot = juce::jlimit(0, ModernAudioEngine::NumModSequencers - 1, laneSlot);
    const auto safeTarget = sanitizeModPerformanceTarget(target);
    const auto currentTarget = sanitizeModPerformanceTarget(audioEngine->getModTargetForSlot(safeStripIndex, safeSlot));

    if (sceneModTargetsMatch(currentTarget, safeTarget))
    {
        audioEngine->setModSequencerSlot(safeStripIndex, safeSlot);
        if (isSceneModeEnabled())
            syncFocusedSceneMotionState();
        return;
    }

    int swapSlot = -1;
    if (safeTarget != ModernAudioEngine::ModTarget::None)
    {
        for (int slot = 0; slot < ModernAudioEngine::NumModSequencers; ++slot)
        {
            if (slot == safeSlot)
                continue;
            if (!sceneModTargetsMatch(audioEngine->getModTargetForSlot(safeStripIndex, slot), safeTarget))
                continue;
            swapSlot = slot;
            break;
        }
    }

    if (swapSlot >= 0)
    {
        audioEngine->setModSequencerSlot(safeStripIndex, swapSlot);
        audioEngine->setModTarget(safeStripIndex, currentTarget);
    }

    audioEngine->setModSequencerSlot(safeStripIndex, safeSlot);
    audioEngine->setModTarget(safeStripIndex, safeTarget);

    if (isSceneModeEnabled())
        syncFocusedSceneMotionState();
}

void MlrVSTAudioProcessor::stepVisibleModLaneTarget(int stripIndex, int direction)
{
    if (audioEngine == nullptr)
        return;

    const int safeDirection = (direction > 0) ? 1 : ((direction < 0) ? -1 : 0);
    if (safeDirection == 0)
        return;

    const int safeStripIndex = juce::jlimit(0, MaxStrips - 1, stripIndex);
    const bool sceneMainModActive = isSceneModeEnabled() && getSceneModPageMode() == SceneModPageMode::MainModulation;
    const auto targets = sceneMainModActive
        ? getSceneVisibleModTargetsForStrip(safeStripIndex)
        : getVisibleModTargetsForStrip(safeStripIndex);
    if (targets.empty())
        return;

    const int activeSlot = juce::jlimit(0,
                                        ModernAudioEngine::NumModSequencers - 1,
                                        audioEngine->getModSequencerSlot(safeStripIndex));
    const auto activeTarget = sceneMainModActive
        ? getSceneMainAutomationDisplayTargetForStrip(safeStripIndex)
        : sanitizeModPerformanceTarget(audioEngine->getModTargetForSlot(safeStripIndex, activeSlot));

    int currentIndex = -1;
    for (int targetIndex = 0; targetIndex < static_cast<int>(targets.size()); ++targetIndex)
    {
        if (sceneModTargetsMatch(targets[static_cast<size_t>(targetIndex)], activeTarget))
        {
            currentIndex = targetIndex;
            break;
        }
    }

    const int targetCount = static_cast<int>(targets.size());
    const int nextIndex = currentIndex >= 0
        ? (currentIndex + safeDirection + targetCount) % targetCount
        : (safeDirection > 0 ? 0 : targetCount - 1);
    const auto nextTarget = sanitizeModPerformanceTarget(targets[static_cast<size_t>(nextIndex)]);

    if (sceneMainModActive)
    {
        setSceneMainAutomationDisplayTargetForStrip(safeStripIndex, nextTarget);
        requestSceneControlRefreshAsync();
        return;
    }

    int nextSlot = -1;
    for (int slot = 0; slot < ModernAudioEngine::NumModSequencers; ++slot)
    {
        if (sceneModTargetsMatch(audioEngine->getModTargetForSlot(safeStripIndex, slot), nextTarget))
        {
            nextSlot = slot;
            break;
        }
    }

    if (nextSlot >= 0)
    {
        audioEngine->setModSequencerSlot(safeStripIndex, nextSlot);
    }
    else
    {
        int emptySlot = -1;
        for (int slot = 0; slot < ModernAudioEngine::NumModSequencers; ++slot)
        {
            if (sanitizeModPerformanceTarget(audioEngine->getModTargetForSlot(safeStripIndex, slot))
                != ModernAudioEngine::ModTarget::None)
            {
                continue;
            }

            emptySlot = slot;
            break;
        }

        audioEngine->setModSequencerSlot(safeStripIndex, emptySlot >= 0 ? emptySlot : activeSlot);
        audioEngine->setModTarget(safeStripIndex, nextTarget);
    }

    requestSceneControlRefreshAsync();
}

void MlrVSTAudioProcessor::stepSceneModLaneTarget(int stripIndex, int direction)
{
    stepVisibleModLaneTarget(stripIndex, direction);

    if (isSceneModeEnabled())
        syncFocusedSceneMotionState();
}

void MlrVSTAudioProcessor::cycleSceneModLaneTarget(int stripIndex)
{
    stepSceneModLaneTarget(stripIndex, 1);
}

void MlrVSTAudioProcessor::setSceneStepMotionEditorOpen(bool isOpen)
{
    sceneStepMotionEditorOpenState.store(isOpen ? 1 : 0, std::memory_order_release);
}
