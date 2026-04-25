/*
  ==============================================================================

    PluginProcessorSceneMode.cpp
    Extracted scene mode orchestration and scene scheduler wrappers for mlrVST.

  ==============================================================================
*/

#include "PluginProcessor.h"
#include "SceneScheduler.h"
#include <cmath>

bool MlrVSTAudioProcessor::ensureActiveScenePlaybackHandleInitialized()
{
    if (!isSceneModeEnabled() || audioEngine == nullptr)
        return false;

    if (activeSceneStartPpqValid && std::isfinite(activeSceneStartPpq))
    {
        updateAudioEngineSceneModulationContext();
        return true;
    }

    double startPpq = audioEngine->getTimelineBeat();
    juce::AudioPlayHead::PositionInfo positionInfo;
    if (getCurrentHostPositionInfo(positionInfo)
        && positionInfo.getPpqPosition().hasValue()
        && std::isfinite(*positionInfo.getPpqPosition()))
    {
        startPpq = *positionInfo.getPpqPosition();
    }

    if (!std::isfinite(startPpq))
        return false;

    const int sceneSlot = juce::jlimit(0, SceneSlots - 1, activeSceneSlot);
    setActiveScenePlaybackHandle(getActiveMainPresetIndexForScenes(),
                                 sceneSlot,
                                 sceneSequenceActive,
                                 sceneSequenceCurrentStepIndex,
                                 startPpq,
                                 getResolvedSceneLengthBeats(sceneSlot));
    setSceneChainAttachStartPpq(startPpq);
    applySceneHeldAutomationStateAtBeat(sceneSlot, startPpq, startPpq);
    updateAudioEngineSceneModulationContext();
    return true;
}

void MlrVSTAudioProcessor::updateAudioEngineSceneModulationContext() const
{
    if (audioEngine == nullptr)
        return;

    if (!isSceneModeEnabled() || !activeSceneStartPpqValid || !std::isfinite(activeSceneStartPpq))
    {
        audioEngine->setSceneModulationContext(false, 0.0, 4.0);
        return;
    }

    const double sceneLengthBeats = juce::jmax(0.001, getResolvedSceneLengthBeats(getActiveSceneSlot()));
    audioEngine->setSceneModulationContext(true, activeSceneStartPpq, sceneLengthBeats);
}

double MlrVSTAudioProcessor::computeStripSceneSequenceLengthBeats(int stripIndex) const
{
    return SceneScheduler::computeStripSceneSequenceLengthBeats(*this, stripIndex);
}

double MlrVSTAudioProcessor::computeLongestStripSceneSequenceLengthBeats() const
{
    return SceneScheduler::computeLongestStripSceneSequenceLengthBeats(*this);
}

double MlrVSTAudioProcessor::computeLongestPatternSceneSequenceLengthBeats() const
{
    return SceneScheduler::computeLongestPatternSceneSequenceLengthBeats(*this);
}

double MlrVSTAudioProcessor::computeCurrentSceneSequenceLengthBeats() const
{
    return SceneScheduler::computeCurrentSceneSequenceLengthBeats(*this);
}

double MlrVSTAudioProcessor::computeNextScenePatternEndPpq(int sceneSlot,
                                                           double currentPpq,
                                                           double cycleBeats,
                                                           uint64_t* outPhaseSignature) const
{
    return SceneScheduler::computeNextScenePatternEndPpq(*this,
                                                         sceneSlot,
                                                         currentPpq,
                                                         cycleBeats,
                                                         outPhaseSignature);
}

void MlrVSTAudioProcessor::armNextSceneInSequence(int mainPresetIndex,
                                                  int currentSceneSlot,
                                                  double sceneStartPpq)
{
    SceneScheduler::armNextSceneInSequence(*this, mainPresetIndex, currentSceneSlot, sceneStartPpq);
}

void MlrVSTAudioProcessor::setSceneModeEnabled(bool enabled)
{
    SceneScheduler::setSceneModeEnabled(*this, enabled);
}

void MlrVSTAudioProcessor::captureSceneModeGroupSnapshot()
{
    SceneScheduler::captureSceneModeGroupSnapshot(*this);
}

void MlrVSTAudioProcessor::restoreSceneModeGroupSnapshot()
{
    SceneScheduler::restoreSceneModeGroupSnapshot(*this);
}

void MlrVSTAudioProcessor::clearAllStripGroupsForSceneMode()
{
    SceneScheduler::clearAllStripGroupsForSceneMode(*this);
}

void MlrVSTAudioProcessor::applySceneModeState(bool enabled)
{
    SceneScheduler::applySceneModeState(*this, enabled);
}

void MlrVSTAudioProcessor::syncSceneModeFromParameters()
{
    SceneScheduler::syncSceneModeFromParameters(*this);
}

bool MlrVSTAudioProcessor::saveSceneForMainPreset(int mainPresetIndex, int sceneSlot)
{
    return SceneScheduler::saveSceneForMainPreset(*this, mainPresetIndex, sceneSlot);
}

bool MlrVSTAudioProcessor::copySceneForMainPreset(int mainPresetIndex, int sourceSceneSlot, int destSceneSlot)
{
    return SceneScheduler::copySceneForMainPreset(*this, mainPresetIndex, sourceSceneSlot, destSceneSlot);
}

bool MlrVSTAudioProcessor::deleteSceneForMainPreset(int mainPresetIndex, int sceneSlot)
{
    return SceneScheduler::deleteSceneForMainPreset(*this, mainPresetIndex, sceneSlot);
}

bool MlrVSTAudioProcessor::captureSceneSlot(int sceneSlot)
{
    return SceneScheduler::captureSceneSlot(*this, sceneSlot);
}

bool MlrVSTAudioProcessor::insertSceneSlot(int sceneSlot, bool insertAfter)
{
    return SceneScheduler::insertSceneSlot(*this, sceneSlot, insertAfter);
}

void MlrVSTAudioProcessor::copyScenePerformanceClip(int sourceSceneSlot, int destSceneSlot)
{
    scenePerformanceRecorder.copyClip(sourceSceneSlot, destSceneSlot);
    syncScenePerformanceClipLengthToResolvedLength(destSceneSlot);
    refreshSceneAutomationTargetMask(destSceneSlot);
}

void MlrVSTAudioProcessor::requestSceneRecallQuantized(int mainPresetIndex,
                                                       int sceneSlot,
                                                       bool sequenceDriven,
                                                       int sequenceStepIndex,
                                                       bool useTriggerQuantization)
{
    SceneScheduler::requestSceneRecallQuantized(
        *this,
        mainPresetIndex,
        sceneSlot,
        sequenceDriven,
        sequenceStepIndex,
        useTriggerQuantization);
}

double MlrVSTAudioProcessor::getSceneRecallIntervalBeats() const
{
    return SceneScheduler::getSceneRecallIntervalBeats(*this);
}

void MlrVSTAudioProcessor::updateSceneQuantizedRecall(
    const juce::AudioPlayHead::PositionInfo& posInfo, int numSamples)
{
    SceneScheduler::updateSceneQuantizedRecall(*this, posInfo, numSamples);
}

void MlrVSTAudioProcessor::processPendingSceneApply()
{
    SceneScheduler::processPendingSceneApply(*this);
}

void MlrVSTAudioProcessor::performEmptySceneLoad()
{
    SceneScheduler::performEmptySceneLoad(*this);
}

void MlrVSTAudioProcessor::performSceneLoad(int mainPresetIndex,
                                            int sceneSlot,
                                            double hostPpqSnapshot,
                                            double hostTempoSnapshot,
                                            int64_t hostGlobalSampleSnapshot,
                                            bool preserveLoadedStripAudio,
                                            bool* recallContinuityBrokenOut)
{
    SceneScheduler::performSceneLoad(*this,
                                     mainPresetIndex,
                                     sceneSlot,
                                     hostPpqSnapshot,
                                     hostTempoSnapshot,
                                     hostGlobalSampleSnapshot,
                                     preserveLoadedStripAudio,
                                     recallContinuityBrokenOut);
}
