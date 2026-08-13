/*
  ==============================================================================

    PluginProcessorSceneSwitch.cpp
    Extracted scene switch runtime for mlrVST.

  ==============================================================================
*/

#include "PluginProcessor.h"
#include "PlayheadSpeedQuantizer.h"
#include "PresetStore.h"
#include "SceneScheduler.h"
#include <cmath>
#include <cstring>
#include <limits>

namespace
{
constexpr const char* kEmbeddedFlipSampleAttr = "embeddedSampleWavBase64";

uint64_t mixSceneLaunchHash(uint64_t hash, uint64_t value) noexcept
{
    hash ^= value + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
    return hash;
}

uint64_t hashStringForSceneLaunch(const juce::String& text) noexcept
{
    uint64_t hash = 1469598103934665603ULL;
    for (const auto c : text)
        hash = (hash ^ static_cast<uint64_t>(static_cast<uint32_t>(c))) * 1099511628211ULL;
    return hash;
}

uint64_t hashFloatForSceneLaunch(float value) noexcept
{
    uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value), "float bit width mismatch");
    std::memcpy(&bits, &value, sizeof(bits));
    return static_cast<uint64_t>(bits);
}

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
}

void MlrVSTAudioProcessor::requestScenePreload(int mainPresetIndex,
                                               int sceneSlot,
                                               bool sequenceDriven,
                                               int sequenceStepIndex,
                                               double targetPpq,
                                               double targetTempo,
                                               int64_t targetSample,
                                               SceneChainTransitionType transitionType,
                                               uint64_t switchSerial)
{
    pendingScenePreloadMainPreset.store(mainPresetIndex, std::memory_order_release);
    pendingScenePreloadSceneSlot.store(sceneSlot, std::memory_order_release);
    pendingScenePreloadSequenceDriven.store(sequenceDriven ? 1 : 0, std::memory_order_release);
    pendingScenePreloadSequenceStep.store(sequenceStepIndex, std::memory_order_release);
    pendingScenePreloadTargetPpq.store(targetPpq, std::memory_order_release);
    pendingScenePreloadTargetTempo.store(targetTempo, std::memory_order_release);
    pendingScenePreloadTargetSample.store(targetSample, std::memory_order_release);
    pendingScenePreloadTransitionType.store(static_cast<int>(transitionType), std::memory_order_release);
    pendingScenePreloadSwitchSerial.store(switchSerial, std::memory_order_release);
    pendingScenePreloadDirty.store(1, std::memory_order_release);
}

bool MlrVSTAudioProcessor::preparedSceneSwitchPayloadMatchesTarget(const PreparedSceneSwitchPayload& payload,
                                                                   int mainPresetIndex,
                                                                   int sceneSlot,
                                                                   bool sequenceDriven,
                                                                   int sequenceStepIndex) const noexcept
{
    return payload.mainPresetIndex == mainPresetIndex
        && payload.sceneSlot == sceneSlot
        && payload.sequenceDriven == sequenceDriven
        && payload.sequenceStepIndex == sequenceStepIndex;
}

bool MlrVSTAudioProcessor::preparedSceneSwitchPayloadMatchesSwitchEvent(const PreparedSceneSwitchPayload& payload,
                                                                        const SceneSwitchEvent& event) const noexcept
{
    const bool serialMatches = event.serial == 0
        || payload.switchSerial == 0
        || payload.switchSerial == event.serial;
    if (!serialMatches)
        return false;

    return preparedSceneSwitchPayloadMatchesTarget(payload,
                                                   event.mainPresetIndex,
                                                   event.sceneSlot,
                                                   event.sequenceDriven,
                                                   event.sequenceStepIndex);
}

void MlrVSTAudioProcessor::servicePendingScenePreloadRequest()
{
    reclaimRetiredPreparedSceneSwitchPayloads();

    if (pendingScenePreloadDirty.load(std::memory_order_acquire) == 0)
        return;

    const int mainPresetIndex = pendingScenePreloadMainPreset.load(std::memory_order_acquire);
    const int sceneSlot = pendingScenePreloadSceneSlot.load(std::memory_order_acquire);
    const bool sequenceDriven = pendingScenePreloadSequenceDriven.load(std::memory_order_acquire) != 0;
    const int sequenceStepIndex = pendingScenePreloadSequenceStep.load(std::memory_order_acquire);
    const double targetPpq = pendingScenePreloadTargetPpq.load(std::memory_order_acquire);
    const double targetTempo = pendingScenePreloadTargetTempo.load(std::memory_order_acquire);
    const int64_t targetSample = pendingScenePreloadTargetSample.load(std::memory_order_acquire);
    const uint64_t switchSerial = pendingScenePreloadSwitchSerial.load(std::memory_order_acquire);
    const auto transitionType = static_cast<SceneChainTransitionType>(juce::jlimit(
        0,
        static_cast<int>(SceneChainTransitionType::Return),
        pendingScenePreloadTransitionType.load(std::memory_order_acquire)));

    const int publishedMainPreset = preparedSceneSwitchPayloadMainPreset.load(std::memory_order_acquire);
    const int publishedSceneSlot = preparedSceneSwitchPayloadSceneSlot.load(std::memory_order_acquire);
    const bool publishedSequenceDriven = preparedSceneSwitchPayloadSequenceDriven.load(std::memory_order_acquire) != 0;
    const int publishedSequenceStep = preparedSceneSwitchPayloadSequenceStep.load(std::memory_order_acquire);
    const uint64_t publishedSwitchSerial = preparedSceneSwitchPayloadSwitchSerial.load(std::memory_order_acquire);
    if (preparedSceneSwitchPayloadPublished.load(std::memory_order_acquire) != nullptr
        && publishedMainPreset == mainPresetIndex
        && publishedSceneSlot == sceneSlot
        && publishedSequenceDriven == sequenceDriven
        && publishedSequenceStep == sequenceStepIndex
        && (switchSerial == 0 || publishedSwitchSerial == 0 || publishedSwitchSerial == switchSerial))
    {
        pendingScenePreloadDirty.store(0, std::memory_order_release);
        return;
    }

    if (mainPresetIndex < 0 || sceneSlot < 0 || currentSampleRate <= 0.0)
        return;

    juce::ignoreUnused(targetPpq, targetTempo, targetSample, transitionType);

    auto preparedPayload = std::make_unique<PreparedSceneSwitchPayload>();
    if (!buildPreparedSceneSwitchPayload(*preparedPayload,
                                         mainPresetIndex,
                                         sceneSlot,
                                         sequenceDriven,
                                         sequenceStepIndex))
    {
        pendingScenePreloadDirty.store(0, std::memory_order_release);
        return;
    }

    preparedPayload->switchSerial = switchSerial;

    preparedSceneSwitchPayloadSwitchSerial.store(0, std::memory_order_release);
    preparedSceneSwitchPayloadMainPreset.store(-1, std::memory_order_release);
    preparedSceneSwitchPayloadSceneSlot.store(-1, std::memory_order_release);
    preparedSceneSwitchPayloadSequenceDriven.store(0, std::memory_order_release);
    preparedSceneSwitchPayloadSequenceStep.store(-1, std::memory_order_release);

    auto* publishedPayload = preparedPayload.release();
    if (auto* replacedPayload = preparedSceneSwitchPayloadPublished.exchange(publishedPayload, std::memory_order_acq_rel))
        retirePreparedSceneSwitchPayload(replacedPayload);

    preparedSceneSwitchPayloadMainPreset.store(mainPresetIndex, std::memory_order_release);
    preparedSceneSwitchPayloadSceneSlot.store(sceneSlot, std::memory_order_release);
    preparedSceneSwitchPayloadSequenceDriven.store(sequenceDriven ? 1 : 0, std::memory_order_release);
    preparedSceneSwitchPayloadSequenceStep.store(sequenceStepIndex, std::memory_order_release);
    preparedSceneSwitchPayloadSwitchSerial.store(switchSerial, std::memory_order_release);
    pendingScenePreloadDirty.store(0, std::memory_order_release);
}

bool MlrVSTAudioProcessor::hasPreparedSceneSwitchPayloadForEvent(const SceneSwitchEvent& event) const
{
    if (preparedSceneSwitchPayloadPublished.load(std::memory_order_acquire) == nullptr)
        return false;

    const int mainPresetIndex = preparedSceneSwitchPayloadMainPreset.load(std::memory_order_acquire);
    const int sceneSlot = preparedSceneSwitchPayloadSceneSlot.load(std::memory_order_acquire);
    const bool sequenceDriven = preparedSceneSwitchPayloadSequenceDriven.load(std::memory_order_acquire) != 0;
    const int sequenceStepIndex = preparedSceneSwitchPayloadSequenceStep.load(std::memory_order_acquire);
    const uint64_t switchSerial = preparedSceneSwitchPayloadSwitchSerial.load(std::memory_order_acquire);

    if (mainPresetIndex != event.mainPresetIndex
        || sceneSlot != event.sceneSlot
        || sequenceDriven != event.sequenceDriven
        || sequenceStepIndex != event.sequenceStepIndex)
    {
        return false;
    }

    return event.serial == 0
        || switchSerial == 0
        || switchSerial == event.serial;
}

MlrVSTAudioProcessor::PreparedSceneSwitchPayload*
MlrVSTAudioProcessor::takePreparedSceneSwitchPayloadForEvent(const SceneSwitchEvent& event)
{
    if (!hasPreparedSceneSwitchPayloadForEvent(event))
        return nullptr;

    auto* payload = preparedSceneSwitchPayloadPublished.exchange(nullptr, std::memory_order_acq_rel);
    preparedSceneSwitchPayloadSwitchSerial.store(0, std::memory_order_release);
    preparedSceneSwitchPayloadMainPreset.store(-1, std::memory_order_release);
    preparedSceneSwitchPayloadSceneSlot.store(-1, std::memory_order_release);
    preparedSceneSwitchPayloadSequenceDriven.store(0, std::memory_order_release);
    preparedSceneSwitchPayloadSequenceStep.store(-1, std::memory_order_release);

    if (payload == nullptr)
        return nullptr;

    if (!preparedSceneSwitchPayloadMatchesSwitchEvent(*payload, event))
    {
        retirePreparedSceneSwitchPayload(payload);
        return nullptr;
    }

    return payload;
}

void MlrVSTAudioProcessor::retirePreparedSceneSwitchPayload(PreparedSceneSwitchPayload* payload) noexcept
{
    if (payload == nullptr)
        return;

    for (auto& slot : retiredPreparedSceneSwitchPayloads)
    {
        PreparedSceneSwitchPayload* empty = nullptr;
        if (slot.compare_exchange_strong(empty, payload, std::memory_order_acq_rel))
            return;
    }

    delete payload;
}

void MlrVSTAudioProcessor::reclaimRetiredPreparedSceneSwitchPayloads()
{
    for (auto& slot : retiredPreparedSceneSwitchPayloads)
    {
        if (auto* payload = slot.exchange(nullptr, std::memory_order_acq_rel))
            delete payload;
    }
}

void MlrVSTAudioProcessor::queuePendingSceneParameterState(const juce::ValueTree& state)
{
    auto pendingState = std::make_unique<juce::ValueTree>(state);
    std::unique_ptr<juce::ValueTree> replacedState(
        pendingSceneParameterStatePtr.exchange(pendingState.release(), std::memory_order_acq_rel));
    if (replacedState != nullptr)
    {
        for (auto& slot : retiredPendingSceneParameterStates)
        {
            juce::ValueTree* empty = nullptr;
            if (slot.compare_exchange_strong(empty, replacedState.get(), std::memory_order_acq_rel))
            {
                replacedState.release();
                break;
            }
        }
    }
}

void MlrVSTAudioProcessor::applyPendingSceneParameterState()
{
    for (auto& slot : retiredPendingSceneParameterStates)
    {
        std::unique_ptr<juce::ValueTree> retiredState(
            slot.exchange(nullptr, std::memory_order_acq_rel));
    }

    std::unique_ptr<juce::ValueTree> stateToApply(
        pendingSceneParameterStatePtr.exchange(nullptr, std::memory_order_acq_rel));
    if (stateToApply == nullptr)
        return;

    if (stateToApply->isValid())
    {
        auto stateCopy = *stateToApply;
        stripPersistentGlobalControlsFromState(stateCopy);
        beginSceneManualControlHandlingSuppression();
        parameters.replaceState(stateCopy);
        endSceneManualControlHandlingSuppression();
    }

    suppressOwnedStripParameterSync.store(0, std::memory_order_release);
}

void MlrVSTAudioProcessor::applyPendingSceneRawParameterResync()
{
    if (pendingSceneRawParameterResync.exchange(0, std::memory_order_acq_rel) == 0)
        return;

    // Rebuild every parameter from its raw atomic (the engine truth after
    // CacheOnly scene writes). Parameters that were untouched sync to their
    // own current value, so this is a no-op for them.
    suppressPersistentGlobalControlsSave.store(1, std::memory_order_release);
    beginSceneManualControlHandlingSuppression();
    for (auto* param : getParameters())
    {
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*>(param))
        {
            if (auto* raw = parameters.getRawParameterValue(ranged->paramID))
            {
                const float normalized = ranged->convertTo0to1(raw->load(std::memory_order_acquire));
                if (std::abs(ranged->getValue() - normalized) > 1.0e-6f)
                    ranged->setValueNotifyingHost(normalized);
            }
        }
    }
    endSceneManualControlHandlingSuppression();
    suppressPersistentGlobalControlsSave.store(0, std::memory_order_release);

    suppressOwnedStripParameterSync.store(0, std::memory_order_release);
}

void MlrVSTAudioProcessor::clearSceneStripLaunchHandles() noexcept
{
    for (auto& handle : sceneStripLaunchHandles)
        handle.reset();

    sceneStripLaunchRevisionCounter.store(1, std::memory_order_release);
}

MlrVSTAudioProcessor::SceneStripPlaybackHandle
MlrVSTAudioProcessor::captureSceneStripPlaybackHandle(int stripIndex)
{
    SceneStripPlaybackHandle handle;
    handle.reset();

    if (audioEngine == nullptr || stripIndex < 0 || stripIndex >= MaxStrips)
        return handle;

    auto* strip = audioEngine->getStrip(stripIndex);
    if (strip == nullptr)
        return handle;

    handle.valid = true;
    handle.present = true;
    handle.playing = strip->isPlaying();
    handle.sequenceDriven = activeScenePlaybackHandle.sequenceDriven;
    handle.mainPresetIndex = activeScenePlaybackHandle.mainPresetIndex;
    handle.sceneSlot = activeScenePlaybackHandle.sceneSlot;
    handle.stripIndex = stripIndex;
    handle.sequenceStepIndex = activeScenePlaybackHandle.sequenceStepIndex;
    handle.playMode = strip->getPlayMode();
    handle.loopStart = strip->getLoopStart();
    handle.loopEnd = strip->getLoopEnd();
    handle.recordingBars = strip->getRecordingBars();
    handle.beatsPerLoop = strip->getBeatsPerLoop();
    handle.playheadSpeedRatio = strip->getPlayheadSpeedRatio();
    handle.directionMode = strip->getDirectionMode();
    handle.reverse = strip->isReverse();
    handle.ppqTimelineAnchored = strip->isPpqTimelineAnchored();
    handle.ppqTimelineOffsetBeats = strip->getPpqTimelineOffsetBeats();
    handle.playbackColumn = strip->getCurrentColumn();
    handle.sampleFile = currentStripFiles[static_cast<size_t>(stripIndex)];
    handle.audioSourceSignature = !handle.sampleFile.getFullPathName().isEmpty()
        ? hashStringForSceneLaunch(handle.sampleFile.getFullPathName())
        : 0;
    handle.continuityBlendState = strip->captureContinuityBlendState();
    handle.revision = sceneStripLaunchRevisionCounter.fetch_add(1, std::memory_order_acq_rel);
    return handle;
}

void MlrVSTAudioProcessor::refreshSceneStripLaunchHandlesFromEngine()
{
    if (audioEngine == nullptr)
        return;

    for (int stripIndex = 0; stripIndex < MaxStrips; ++stripIndex)
    {
        auto snapshot = captureSceneStripPlaybackHandle(stripIndex);
        auto& launchHandle = sceneStripLaunchHandles[static_cast<size_t>(stripIndex)];
        if (!snapshot.valid)
        {
            if (!launchHandle.current.valid)
                launchHandle.reset();
            continue;
        }

        if (launchHandle.current.valid)
        {
            snapshot.present = launchHandle.current.present;
            snapshot.sequenceDriven = launchHandle.current.sequenceDriven;
            snapshot.mainPresetIndex = launchHandle.current.mainPresetIndex;
            snapshot.sceneSlot = launchHandle.current.sceneSlot;
            snapshot.sequenceStepIndex = launchHandle.current.sequenceStepIndex;
            snapshot.audioSourceSignature = launchHandle.current.audioSourceSignature != 0
                ? launchHandle.current.audioSourceSignature
                : snapshot.audioSourceSignature;

            if ((!snapshot.continuityBlendState.valid || !snapshot.playing)
                && launchHandle.current.continuityBlendState.valid)
            {
                snapshot.continuityBlendState = launchHandle.current.continuityBlendState;
            }
        }
        else if (activeScenePlaybackHandle.active)
        {
            snapshot.sequenceDriven = activeScenePlaybackHandle.sequenceDriven;
            snapshot.mainPresetIndex = activeScenePlaybackHandle.mainPresetIndex;
            snapshot.sceneSlot = activeScenePlaybackHandle.sceneSlot;
            snapshot.sequenceStepIndex = activeScenePlaybackHandle.sequenceStepIndex;
        }

        launchHandle.current = snapshot;
        launchHandle.active = snapshot.present || snapshot.playing || snapshot.continuityBlendState.valid;
    }
}

void MlrVSTAudioProcessor::clearActiveScenePlaybackHandle()
{
    activeScenePlaybackHandle = {};
    sceneChainAttachStartPpqValid = false;
    sceneChainAttachStartPpq = 0.0;
    activeSceneStartPpqValid = false;
    activeSceneStartPpq = 0.0;
    sceneSequenceStartPpqValid = false;
    sceneSequenceStartPpq = 0.0;
    clearSceneStripLaunchHandles();
    clearActiveSceneAutomationOverrides(false);
}

void MlrVSTAudioProcessor::setActiveScenePlaybackHandle(int mainPresetIndex,
                                                        int sceneSlot,
                                                        bool sequenceDriven,
                                                        int sequenceStepIndex,
                                                        double startPpq,
                                                        double resolvedLengthBeats)
{
    const bool sceneChanged = activeScenePlaybackHandle.mainPresetIndex != juce::jlimit(0, MaxPresetSlots - 1, mainPresetIndex)
        || activeScenePlaybackHandle.sceneSlot != juce::jlimit(0, SceneSlots - 1, sceneSlot)
        || !activeScenePlaybackHandle.active
        || !std::isfinite(activeScenePlaybackHandle.startPpq)
        || !std::isfinite(startPpq)
        || std::abs(activeScenePlaybackHandle.startPpq - startPpq) > 1.0e-9;

    activeScenePlaybackHandle.active = std::isfinite(startPpq);
    activeScenePlaybackHandle.sequenceDriven = sequenceDriven;
    activeScenePlaybackHandle.mainPresetIndex = juce::jlimit(0, MaxPresetSlots - 1, mainPresetIndex);
    activeScenePlaybackHandle.sceneSlot = juce::jlimit(0, SceneSlots - 1, sceneSlot);
    activeScenePlaybackHandle.sequenceStepIndex = sequenceDriven ? sequenceStepIndex : -1;
    activeScenePlaybackHandle.startPpq = activeScenePlaybackHandle.active ? startPpq : 0.0;
    activeScenePlaybackHandle.resolvedLengthBeats = juce::jlimit(0.25, 4096.0, resolvedLengthBeats);

    activeSceneStartPpqValid = activeScenePlaybackHandle.active;
    activeSceneStartPpq = activeScenePlaybackHandle.startPpq;
    sceneSequenceStartPpqValid = activeScenePlaybackHandle.active && sequenceDriven;
    sceneSequenceStartPpq = sceneSequenceStartPpqValid ? activeScenePlaybackHandle.startPpq : 0.0;

    if (sceneChanged)
        clearActiveSceneAutomationOverrides(false);
}

void MlrVSTAudioProcessor::switchScenePlaybackOwner(ScenePlaybackOwner owner,
                                                    bool chainActive,
                                                    int chainStepIndex)
{
    scenePlaybackOwner = owner;
    sceneSequenceActive = chainActive;
    sceneSequenceCurrentStepIndex = chainActive ? chainStepIndex : -1;

    if (!chainActive)
    {
        sceneSequenceStartPpqValid = false;
        sceneSequenceStartPpq = 0.0;
    }
}

void MlrVSTAudioProcessor::setSceneChainAttachStartPpq(double startPpq)
{
    sceneChainAttachStartPpqValid = std::isfinite(startPpq);
    sceneChainAttachStartPpq = sceneChainAttachStartPpqValid ? startPpq : 0.0;
}

void MlrVSTAudioProcessor::setScenePlaybackOwner(ScenePlaybackOwner owner)
{
    scenePlaybackOwner = owner;
}

MlrVSTAudioProcessor::SceneSwitchSplitStatus
MlrVSTAudioProcessor::buildSceneSwitchSplitStatus(const SceneSwitchEvent& event) const noexcept
{
    SceneSwitchSplitStatus status;

    const int blockNumSamples = juce::jmax(0, event.blockNumSamples);
    const int targetOffset = juce::jlimit(0, blockNumSamples, event.targetSampleOffsetInBlock);
    const bool outgoingPlaying = hasAnyLiveScenePlayback();
    const bool incomingPlaying = sceneSwitchHasIncomingPlayback(event);
    const double outgoingStartPpq = (activeScenePlaybackHandle.active
                                     && std::isfinite(activeScenePlaybackHandle.startPpq))
        ? activeScenePlaybackHandle.startPpq
        : std::numeric_limits<double>::quiet_NaN();
    const double incomingStartPpq = (event.legatoOwnerSwitch && std::isfinite(outgoingStartPpq))
        ? outgoingStartPpq
        : event.targetPpq;

    status.range1.playing = outgoingPlaying && targetOffset > 0;
    status.range1.startOffsetInBlock = 0;
    status.range1.numSamples = targetOffset;
    status.range1.playStartPpq = status.range1.playing
        ? outgoingStartPpq
        : std::numeric_limits<double>::quiet_NaN();

    status.range2.playing = incomingPlaying && targetOffset < blockNumSamples;
    status.range2.startOffsetInBlock = targetOffset;
    status.range2.numSamples = juce::jmax(0, blockNumSamples - targetOffset);
    status.range2.playStartPpq = status.range2.playing
        ? incomingStartPpq
        : std::numeric_limits<double>::quiet_NaN();

    status.isSplit = status.range1.numSamples > 0 && status.range2.numSamples > 0;
    return status;
}

int MlrVSTAudioProcessor::resolveSceneSwitchTargetOffsetForCurrentBlock(const SceneSwitchEvent& event,
                                                                        int64_t blockStartSample,
                                                                        int blockNumSamples) const noexcept
{
    const int safeBlockNumSamples = juce::jmax(0, blockNumSamples);
    if (safeBlockNumSamples <= 0)
        return 0;

    if (event.targetGlobalSample >= 0 && blockStartSample >= 0)
    {
        const auto rawOffset = event.targetGlobalSample - blockStartSample;
        if (rawOffset <= 0)
            return 0;
        if (rawOffset >= safeBlockNumSamples)
            return safeBlockNumSamples;
        return static_cast<int>(rawOffset);
    }

    if (event.blockNumSamples > 0 && event.blockStartSample == blockStartSample)
    {
        return juce::jlimit(0,
                            safeBlockNumSamples,
                            juce::jmax(0, event.targetSampleOffsetInBlock));
    }

    return juce::jlimit(0,
                        safeBlockNumSamples,
                        juce::jmax(0, event.targetSampleOffsetInBlock));
}

void MlrVSTAudioProcessor::clearPendingSceneApplyState()
{
    pendingSceneApplyPublishedSerial.store(0, std::memory_order_release);
    pendingSceneApplyConsumedSerial.store(0, std::memory_order_release);
    pendingSceneApplyMainPreset.store(-1, std::memory_order_release);
    pendingSceneApplySlot.store(-1, std::memory_order_release);
    pendingSceneApplySequenceDriven.store(0, std::memory_order_release);
    pendingSceneApplySequenceStep.store(-1, std::memory_order_release);
    pendingSceneApplyTargetPpq.store(-1.0, std::memory_order_release);
    pendingSceneApplyTargetTempo.store(120.0, std::memory_order_release);
    pendingSceneApplyTargetSample.store(-1, std::memory_order_release);
    pendingSceneApplyBlockStartSample.store(-1, std::memory_order_release);
    pendingSceneApplyBlockNumSamples.store(0, std::memory_order_release);
    pendingSceneApplyTargetSampleOffset.store(-1, std::memory_order_release);
    pendingSceneApplyOutgoingOwner.store(static_cast<int>(ScenePlaybackOwner::Manual), std::memory_order_release);
    pendingSceneApplyOwnerOnlySwitch.store(0, std::memory_order_release);
    pendingSceneApplyLegatoOwnerSwitch.store(0, std::memory_order_release);
}

uint64_t MlrVSTAudioProcessor::queuePendingSceneApplyState(const SceneSwitchEvent& event)
{
    auto normalizedEvent = event;
    normalizedEvent.mainPresetIndex = juce::jlimit(0, MaxPresetSlots - 1, normalizedEvent.mainPresetIndex);
    normalizedEvent.sceneSlot = juce::jlimit(0, SceneSlots - 1, normalizedEvent.sceneSlot);
    normalizedEvent.blockNumSamples = juce::jmax(0, normalizedEvent.blockNumSamples);
    if (normalizedEvent.blockNumSamples > 0)
    {
        normalizedEvent.targetSampleOffsetInBlock = juce::jlimit(0,
                                                                 normalizedEvent.blockNumSamples,
                                                                 normalizedEvent.targetSampleOffsetInBlock);
    }
    normalizedEvent.exactBlockBoundary = normalizedEvent.targetSampleOffsetInBlock == 0
        || (normalizedEvent.blockNumSamples > 0
            && normalizedEvent.targetSampleOffsetInBlock == normalizedEvent.blockNumSamples);
    normalizedEvent.splitSwitchInBlock = normalizedEvent.targetSampleOffsetInBlock > 0
        && normalizedEvent.blockNumSamples > 0
        && normalizedEvent.targetSampleOffsetInBlock < normalizedEvent.blockNumSamples;
    normalizedEvent.boundaryCaptureSampleOffsetInBlock = normalizedEvent.targetSampleOffsetInBlock > 0
        ? (normalizedEvent.targetSampleOffsetInBlock - 1)
        : -1;
    normalizedEvent.serial = pendingSceneApplyNextSerial.fetch_add(1, std::memory_order_acq_rel);
    normalizedEvent.splitStatus = buildSceneSwitchSplitStatus(normalizedEvent);

    const int safeMain = normalizedEvent.mainPresetIndex;
    const int safeSlot = normalizedEvent.sceneSlot;
    pendingSceneApplyMainPreset.store(safeMain, std::memory_order_release);
    pendingSceneApplySlot.store(safeSlot, std::memory_order_release);
    pendingSceneApplySequenceDriven.store(normalizedEvent.sequenceDriven ? 1 : 0, std::memory_order_release);
    pendingSceneApplySequenceStep.store(normalizedEvent.sequenceDriven ? normalizedEvent.sequenceStepIndex : -1,
                                        std::memory_order_release);
    pendingSceneApplyTargetPpq.store(normalizedEvent.targetPpq, std::memory_order_release);
    pendingSceneApplyTargetTempo.store(std::isfinite(normalizedEvent.targetTempo) ? normalizedEvent.targetTempo : 120.0,
                                       std::memory_order_release);
    pendingSceneApplyTargetSample.store(normalizedEvent.targetGlobalSample, std::memory_order_release);
    pendingSceneApplyBlockStartSample.store(normalizedEvent.blockStartSample, std::memory_order_release);
    pendingSceneApplyBlockNumSamples.store(normalizedEvent.blockNumSamples, std::memory_order_release);
    pendingSceneApplyTargetSampleOffset.store(normalizedEvent.targetSampleOffsetInBlock, std::memory_order_release);
    pendingSceneApplyOutgoingOwner.store(static_cast<int>(normalizedEvent.outgoingOwner), std::memory_order_release);
    pendingSceneApplyOwnerOnlySwitch.store(normalizedEvent.ownerOnlySwitch ? 1 : 0, std::memory_order_release);
    pendingSceneApplyLegatoOwnerSwitch.store(normalizedEvent.legatoOwnerSwitch ? 1 : 0, std::memory_order_release);
    pendingSceneApplyPublishedSerial.store(normalizedEvent.serial, std::memory_order_release);

    return normalizedEvent.serial;
}

bool MlrVSTAudioProcessor::peekPendingSceneApplyState(SceneSwitchEvent& event) const
{
    for (int attempt = 0; attempt < 8; ++attempt)
    {
        const auto publishedSerial = pendingSceneApplyPublishedSerial.load(std::memory_order_acquire);
        const auto consumedSerial = pendingSceneApplyConsumedSerial.load(std::memory_order_acquire);
        if (publishedSerial == 0 || publishedSerial == consumedSerial)
            return false;

        SceneSwitchEvent snapshot;
        snapshot = {};
        snapshot.active = true;
        snapshot.serial = publishedSerial;
        snapshot.mainPresetIndex = juce::jlimit(0,
                                                MaxPresetSlots - 1,
                                                pendingSceneApplyMainPreset.load(std::memory_order_acquire));
        snapshot.sceneSlot = juce::jlimit(0,
                                          SceneSlots - 1,
                                          pendingSceneApplySlot.load(std::memory_order_acquire));
        snapshot.sequenceDriven = pendingSceneApplySequenceDriven.load(std::memory_order_acquire) != 0;
        snapshot.owner = snapshot.sequenceDriven ? ScenePlaybackOwner::Chain : ScenePlaybackOwner::Manual;
        snapshot.outgoingOwner = static_cast<ScenePlaybackOwner>(juce::jlimit(
            0,
            static_cast<int>(ScenePlaybackOwner::Chain),
            pendingSceneApplyOutgoingOwner.load(std::memory_order_acquire)));
        snapshot.ownerOnlySwitch = pendingSceneApplyOwnerOnlySwitch.load(std::memory_order_acquire) != 0;
        snapshot.legatoOwnerSwitch = pendingSceneApplyLegatoOwnerSwitch.load(std::memory_order_acquire) != 0;
        snapshot.sequenceStepIndex = pendingSceneApplySequenceStep.load(std::memory_order_acquire);
        snapshot.targetPpq = pendingSceneApplyTargetPpq.load(std::memory_order_acquire);
        snapshot.targetTempo = pendingSceneApplyTargetTempo.load(std::memory_order_acquire);
        snapshot.targetGlobalSample = pendingSceneApplyTargetSample.load(std::memory_order_acquire);
        snapshot.blockStartSample = pendingSceneApplyBlockStartSample.load(std::memory_order_acquire);
        snapshot.blockNumSamples = pendingSceneApplyBlockNumSamples.load(std::memory_order_acquire);
        snapshot.targetSampleOffsetInBlock = pendingSceneApplyTargetSampleOffset.load(std::memory_order_acquire);
        snapshot.exactBlockBoundary = snapshot.targetSampleOffsetInBlock == 0
            || (snapshot.blockNumSamples > 0
                && snapshot.targetSampleOffsetInBlock == snapshot.blockNumSamples);
        snapshot.splitSwitchInBlock = snapshot.targetSampleOffsetInBlock > 0
            && snapshot.blockNumSamples > 0
            && snapshot.targetSampleOffsetInBlock < snapshot.blockNumSamples;
        snapshot.boundaryCaptureSampleOffsetInBlock = snapshot.targetSampleOffsetInBlock > 0
            ? (snapshot.targetSampleOffsetInBlock - 1)
            : -1;

        if (pendingSceneApplyPublishedSerial.load(std::memory_order_acquire) != publishedSerial)
            continue;

        snapshot.splitStatus = buildSceneSwitchSplitStatus(snapshot);
        event = snapshot;
        return true;
    }

    return false;
}

bool MlrVSTAudioProcessor::consumePendingSceneApplyState(SceneSwitchEvent& event)
{
    for (;;)
    {
        if (!peekPendingSceneApplyState(event))
            return false;

        auto consumedSerial = pendingSceneApplyConsumedSerial.load(std::memory_order_acquire);
        const auto publishedSerial = event.serial;
        if (!pendingSceneApplyConsumedSerial.compare_exchange_strong(consumedSerial,
                                                                     publishedSerial,
                                                                     std::memory_order_acq_rel))
            continue;

        return true;
    }
}

bool MlrVSTAudioProcessor::sceneSwitchHasIncomingPlayback(const SceneSwitchEvent& event) const noexcept
{
    if (event.legatoOwnerSwitch && hasAnyLiveScenePlayback())
        return true;

    const auto clipSlot = getSceneClipSlotState(event.sceneSlot, event.mainPresetIndex);
    if (clipSlot.hasStoredContent || clipSlot.hasPerformanceClip || clipSlot.hasLiveStripControlState)
        return true;

    return event.sceneSlot == activeSceneSlot
        && event.mainPresetIndex == activeSceneMainPresetIndex
        && hasAnyLiveScenePlayback();
}

bool MlrVSTAudioProcessor::loadPreparedSceneStripAudioToActiveEngine(const PreparedSceneSwitchPayload& payload,
                                                                     int stripIndex)
{
    if (audioEngine == nullptr || stripIndex < 0 || stripIndex >= MaxStrips)
        return false;

    const auto& prepared = payload.stripAudioPayloads[static_cast<size_t>(stripIndex)];
    if (!prepared.valid)
        return false;

    auto* strip = audioEngine->getStrip(stripIndex);
    if (strip == nullptr)
        return false;

    if (prepared.analysisSampleCount > 0)
    {
        strip->loadSampleWithAnalysisCache(prepared.audioBuffer,
                                           prepared.sourceSampleRate,
                                           prepared.transientSlices,
                                           prepared.rmsMap,
                                           prepared.zeroCrossMap,
                                           prepared.analysisSampleCount);
    }
    else
    {
        strip->loadSample(prepared.audioBuffer, prepared.sourceSampleRate);
    }

    return true;
}

bool MlrVSTAudioProcessor::applyPreparedSceneSwitchPayload(const PreparedSceneSwitchPayload& payload,
                                                           const SceneSwitchEvent& event,
                                                           bool& recallContinuityBroken)
{
    recallContinuityBroken = false;

    if (audioEngine == nullptr)
        return false;

    scenePerformanceRecorder.stopRecording();
    clearPendingMonomeSceneRecorderTap();
    clearPendingSceneRecorderAction();
    lastScenePerformanceProcessBeat = std::numeric_limits<double>::quiet_NaN();
    lastScenePerformanceProcessSceneSlot = -1;
    lastScenePerformanceProcessSceneStartBeat = std::numeric_limits<double>::quiet_NaN();

    if (isSceneModeEnabled())
        syncSceneMotionStateFromEngine(activeSceneSlot);

    struct PitchRecallBlendSnapshot
    {
        float pitch = 0.0f;
        float pitchControlMode = 0.0f;
    };

    enum class StripRecallTransitionKind
    {
        Continue,
        Restart,
        NewStart,
        Stop,
        Idle
    };

    struct StripRecallTransitionDecision
    {
        StripRecallTransitionKind kind = StripRecallTransitionKind::Idle;
        bool continuityPreserved = false;
        bool shouldBlendPitchPath = false;
        bool shouldApplyRecallOutputBlend = false;
        bool shouldApplyRecallEdgeFade = false;
        bool continuityBroken = false;
    };

    auto capturePitchRecallBlendSnapshot = [this](int stripIndex) -> PitchRecallBlendSnapshot
    {
        PitchRecallBlendSnapshot snapshot;
        if (stripIndex < 0 || stripIndex >= MaxStrips)
            return snapshot;

        if (auto* pitchParam = stripPitchParams[static_cast<size_t>(stripIndex)])
            snapshot.pitch = pitchParam->load(std::memory_order_acquire);
        if (auto* modeParam = stripPitchControlModeParams[static_cast<size_t>(stripIndex)])
            snapshot.pitchControlMode = modeParam->load(std::memory_order_acquire);
        return snapshot;
    };

    auto resolveBeatsPerLoopForContinuity = [](float manualBeats) -> double
    {
        return manualBeats >= 0.0f ? static_cast<double>(manualBeats) : 4.0;
    };

    auto nearlyEqual = [](double a, double b, double tolerance = 1.0e-4) -> bool
    {
        return std::abs(a - b) <= tolerance;
    };

    auto classifyStripRecallTransition =
        [&](const SceneStripPlaybackHandle& previousHandle,
            bool allowContinuityPreservation,
            bool restorePlaying,
            const SceneStripPlaybackHandle& incomingHandle,
            bool shouldBlendPitchPath) -> StripRecallTransitionDecision
    {
        StripRecallTransitionDecision decision;
        decision.shouldBlendPitchPath = shouldBlendPitchPath;
        const bool wasPreviouslyPlaying = previousHandle.valid && previousHandle.playing;

        if (!restorePlaying)
        {
            decision.kind = wasPreviouslyPlaying
                ? StripRecallTransitionKind::Stop
                : StripRecallTransitionKind::Idle;
            decision.continuityBroken = wasPreviouslyPlaying;
            return decision;
        }

        if (!wasPreviouslyPlaying)
        {
            decision.kind = StripRecallTransitionKind::NewStart;
            decision.shouldApplyRecallOutputBlend = !shouldBlendPitchPath;
            decision.continuityBroken = true;
            return decision;
        }

        const bool sameSourceSegment = previousHandle.audioSourceSignature != 0
            && previousHandle.audioSourceSignature == incomingHandle.audioSourceSignature;
        const bool sameLoopGeometry = previousHandle.playMode == incomingHandle.playMode
            && previousHandle.loopStart == incomingHandle.loopStart
            && previousHandle.loopEnd == incomingHandle.loopEnd
            && previousHandle.recordingBars == incomingHandle.recordingBars
            && nearlyEqual(resolveBeatsPerLoopForContinuity(previousHandle.beatsPerLoop),
                           resolveBeatsPerLoopForContinuity(incomingHandle.beatsPerLoop))
            && nearlyEqual(previousHandle.playheadSpeedRatio, incomingHandle.playheadSpeedRatio)
            && previousHandle.directionMode == incomingHandle.directionMode
            && previousHandle.reverse == incomingHandle.reverse;
        const bool samePlaybackAnchor = previousHandle.ppqTimelineAnchored == incomingHandle.ppqTimelineAnchored
            && (incomingHandle.ppqTimelineAnchored
                    ? nearlyEqual(previousHandle.ppqTimelineOffsetBeats, incomingHandle.ppqTimelineOffsetBeats)
                    : previousHandle.playbackColumn == incomingHandle.playbackColumn);

        decision.continuityPreserved = allowContinuityPreservation
            && !shouldBlendPitchPath
            && previousHandle.playing
            && sameSourceSegment
            && sameLoopGeometry
            && samePlaybackAnchor;

        if (decision.continuityPreserved)
        {
            decision.kind = StripRecallTransitionKind::Continue;
            return decision;
        }

        decision.kind = StripRecallTransitionKind::Restart;
        decision.shouldApplyRecallOutputBlend = !shouldBlendPitchPath;
        decision.continuityBroken = true;
        return decision;
    };

    const double recallPpq = std::isfinite(event.targetPpq)
        ? event.targetPpq
        : audioEngine->getTimelineBeat();
    const double recallTempo = (std::isfinite(event.targetTempo) && event.targetTempo > 0.0)
        ? event.targetTempo
        : audioEngine->getCurrentTempo();
    const bool canRecallStripPlayback = std::isfinite(recallTempo) && recallTempo > 0.0;
    const bool canRecallPatternPlayback = std::isfinite(recallPpq) && std::isfinite(recallTempo) && recallTempo > 0.0;
    const int64_t restoreGlobalSample = event.targetGlobalSample >= 0
        ? event.targetGlobalSample
        : audioEngine->getGlobalSampleCount();

    std::array<PitchRecallBlendSnapshot, MaxStrips> previousPitchBlendSnapshots{};
    std::array<SceneStripPlaybackHandle, MaxStrips> previousStripLaunchHandles{};
    refreshSceneStripLaunchHandlesFromEngine();
    for (int stripIndex = 0; stripIndex < MaxStrips; ++stripIndex)
    {
        previousPitchBlendSnapshots[static_cast<size_t>(stripIndex)] =
            capturePitchRecallBlendSnapshot(stripIndex);

        auto snapshot = captureSceneStripPlaybackHandle(stripIndex);
        const auto& launchHandle = sceneStripLaunchHandles[static_cast<size_t>(stripIndex)];
        if (launchHandle.current.valid)
        {
            snapshot.present = launchHandle.current.present;
            snapshot.sequenceDriven = launchHandle.current.sequenceDriven;
            snapshot.mainPresetIndex = launchHandle.current.mainPresetIndex;
            snapshot.sceneSlot = launchHandle.current.sceneSlot;
            snapshot.sequenceStepIndex = launchHandle.current.sequenceStepIndex;
            snapshot.audioSourceSignature = launchHandle.current.audioSourceSignature != 0
                ? launchHandle.current.audioSourceSignature
                : snapshot.audioSourceSignature;
            if ((!snapshot.continuityBlendState.valid || !snapshot.playing)
                && launchHandle.current.continuityBlendState.valid)
            {
                snapshot.continuityBlendState = launchHandle.current.continuityBlendState;
            }
        }

        previousStripLaunchHandles[static_cast<size_t>(stripIndex)] = snapshot;
    }

    const ResolvedOwnedStripControlState defaultOwnedControls;
    ScopedSceneAutosaveSuppression suppressSceneAutosave(*this);
    bool anyStoredStripContentUnavailable = false;
    for (int stripIndex = 0; stripIndex < MaxStrips; ++stripIndex)
    {
        auto* strip = audioEngine->getStrip(stripIndex);
        if (strip == nullptr)
            continue;

        auto& stripLaunchHandle = sceneStripLaunchHandles[static_cast<size_t>(stripIndex)];
        const auto& previousLaunchHandle = previousStripLaunchHandles[static_cast<size_t>(stripIndex)];
        const bool wasPreviouslyPlaying = previousLaunchHandle.valid && previousLaunchHandle.playing;
        const auto& stripState = payload.stripStates[static_cast<size_t>(stripIndex)];

        auto commitLaunchHandleState =
            [&](const SceneStripPlaybackHandle& incomingHandle,
                SceneStripLaunchTransitionKind transitionKind,
                const StripRecallTransitionDecision& decision,
                bool preservePreviousContinuity)
        {
            stripLaunchHandle.lastTransition = transitionKind;
            stripLaunchHandle.lastTransitionContinuityPreserved = decision.continuityPreserved;
            stripLaunchHandle.lastTransitionContinuityBroken = decision.continuityBroken;
            stripLaunchHandle.lastTransitionBlendPitchPath = decision.shouldBlendPitchPath;
            stripLaunchHandle.lastTransitionApplyOutputBlend = decision.shouldApplyRecallOutputBlend;
            stripLaunchHandle.lastTransitionApplyEdgeFade = decision.shouldApplyRecallEdgeFade;

            auto committedHandle = incomingHandle;
            committedHandle.revision = sceneStripLaunchRevisionCounter.fetch_add(1, std::memory_order_acq_rel);
            if (preservePreviousContinuity && previousLaunchHandle.continuityBlendState.valid)
                committedHandle.continuityBlendState = previousLaunchHandle.continuityBlendState;
            stripLaunchHandle.current = committedHandle;
            stripLaunchHandle.active = committedHandle.present || committedHandle.playing
                || committedHandle.continuityBlendState.valid;
        };

        auto stopStripForUnavailableScene = [&](bool fadeIfNeeded)
        {
            SceneStripPlaybackHandle unavailableHandle;
            unavailableHandle.reset();
            unavailableHandle.valid = true;
            unavailableHandle.sequenceDriven = event.sequenceDriven;
            unavailableHandle.mainPresetIndex = payload.mainPresetIndex;
            unavailableHandle.sceneSlot = payload.sceneSlot;
            unavailableHandle.stripIndex = stripIndex;
            unavailableHandle.sequenceStepIndex = event.sequenceStepIndex;
            unavailableHandle.present = false;
            unavailableHandle.playing = false;

            const bool shouldFadeStoppedOutput = fadeIfNeeded && wasPreviouslyPlaying;
            if (strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Sample)
                stopSampleModeStrip(stripIndex, !shouldFadeStoppedOutput);
            else if (shouldFadeStoppedOutput)
                strip->stopForSceneRecallWithOutputFade(&previousLaunchHandle.continuityBlendState);
            else
                strip->stop(true);

            audioEngine->clearPendingQuantizedTriggersForStrip(stripIndex);
            strip->clearSample();
            strip->setLoop(0, ModernAudioEngine::MaxColumns);
            strip->setPlayMode(EnhancedAudioStrip::PlayMode::Loop);
            strip->setDirectionMode(EnhancedAudioStrip::DirectionMode::Normal);
            strip->setReverse(false);
            strip->setRecordingBars(2);
            strip->setBeatsPerLoop(-1.0f);
            strip->setScratchAmount(0.0f);
            strip->setTransientSliceMode(false);
            strip->setLoopSliceLength(1.0f);
            strip->setSwingAmount(0.0f);
            strip->setGateAmount(0.0f);
            strip->setGateSpeed(4.0f);
            strip->setGateEnvelope(0.5f);
            strip->setGateShape(0.5f);
            strip->setStepPatternLengthSteps(16);
            strip->setStepPage(0);
            strip->currentStep = 0;
            strip->stepPattern.fill(false);
            strip->stepSubdivisions.fill(1);
            strip->stepSubdivisionStartVelocity.fill(1.0f);
            strip->stepSubdivisionRepeatVelocity.fill(1.0f);
            strip->stepProbability.fill(1.0f);
            strip->setStepEnvelopeAttackMs(0.0f);
            strip->setStepEnvelopeDecayMs(4000.0f);
            strip->setStepEnvelopeReleaseMs(110.0f);
            strip->setGrainSizeMs(1240.0f);
            strip->setGrainDensity(0.05f);
            strip->setGrainPitch(0.0f);
            strip->setGrainPitchJitter(0.0f);
            strip->setGrainSpread(0.0f);
            strip->setGrainJitter(0.0f);
            strip->setGrainPositionJitter(0.0f);
            strip->setGrainRandomDepth(0.0f);
            strip->setGrainArpDepth(0.0f);
            strip->setGrainCloudDepth(0.0f);
            strip->setGrainEmitterDepth(0.0f);
            strip->setGrainEnvelope(0.0f);
            strip->setGrainShape(0.0f);
            strip->setGrainArpMode(0);
            strip->setGrainTempoSyncEnabled(true);
            applyResolvedOwnedStripControlState(*strip, defaultOwnedControls);
            applyResolvedStripFilterState(*strip, defaultOwnedControls);
            applyResolvedStripDelayState(*strip, defaultOwnedControls);
            strip->setTempoMatchBackend(resolveLoopTempoMatchBackendForStrip(stripIndex));
            strip->setDuckEnabled(false);
            strip->setDuckSourceSelection(0);
            strip->setDuckThresholdDb(-24.0f);
            strip->setDuckRatio(4.0f);
            strip->setDuckAttackMs(10.0f);
            strip->setDuckReleaseMs(180.0f);
            strip->setDuckGainCompDb(0.0f);
            strip->setDuckFollowMaster(false);
            writeStripFloatParameter("stripVolume" + juce::String(stripIndex),
                                     defaultOwnedControls.volume,
                                     stripVolumeParams[static_cast<size_t>(stripIndex)],
                                     StripControlWriteMode::CacheOnly);
            writeStripFloatParameter("stripTrimDb" + juce::String(stripIndex),
                                     defaultOwnedControls.trimDb,
                                     stripTrimDbParams[static_cast<size_t>(stripIndex)],
                                     StripControlWriteMode::CacheOnly);
            writeStripFloatParameter("stripPan" + juce::String(stripIndex),
                                     defaultOwnedControls.pan,
                                     stripPanParams[static_cast<size_t>(stripIndex)],
                                     StripControlWriteMode::CacheOnly);
            writeStripFloatParameter("stripSpeed" + juce::String(stripIndex),
                                     defaultOwnedControls.playheadSpeedRatio,
                                     stripSpeedParams[static_cast<size_t>(stripIndex)],
                                     StripControlWriteMode::CacheOnly);
            writeStripBoolParameter("stripFilterEnabled" + juce::String(stripIndex),
                                    defaultOwnedControls.filterEnabled,
                                    stripFilterEnabledParams[static_cast<size_t>(stripIndex)],
                                    StripControlWriteMode::CacheOnly);
            writeStripFloatParameter("stripFilterFrequency" + juce::String(stripIndex),
                                     defaultOwnedControls.filterFrequency,
                                     stripFilterFrequencyParams[static_cast<size_t>(stripIndex)],
                                     StripControlWriteMode::CacheOnly);
            writeStripFloatParameter("stripFilterResonance" + juce::String(stripIndex),
                                     defaultOwnedControls.filterResonance,
                                     stripFilterResonanceParams[static_cast<size_t>(stripIndex)],
                                     StripControlWriteMode::CacheOnly);
            writeStripFloatParameter("stripFilterMorph" + juce::String(stripIndex),
                                     defaultOwnedControls.filterMorph,
                                     stripFilterMorphParams[static_cast<size_t>(stripIndex)],
                                     StripControlWriteMode::CacheOnly);
            writeStripFloatParameter("stripFilterAlgorithm" + juce::String(stripIndex),
                                     static_cast<float>(defaultOwnedControls.filterAlgorithm),
                                     stripFilterAlgorithmParams[static_cast<size_t>(stripIndex)],
                                     StripControlWriteMode::CacheOnly);
            writeStripFloatParameter("stripDelayMix" + juce::String(stripIndex),
                                     defaultOwnedControls.delayMix,
                                     stripDelayMixParams[static_cast<size_t>(stripIndex)],
                                     StripControlWriteMode::CacheOnly);
            writeStripFloatParameter("stripDelayTime" + juce::String(stripIndex),
                                     defaultOwnedControls.delayTime,
                                     stripDelayTimeParams[static_cast<size_t>(stripIndex)],
                                     StripControlWriteMode::CacheOnly);
            writeStripBoolParameter("stripDelaySync" + juce::String(stripIndex),
                                    defaultOwnedControls.delaySyncEnabled,
                                    stripDelaySyncParams[static_cast<size_t>(stripIndex)],
                                    StripControlWriteMode::CacheOnly);
            writeStripFloatParameter("stripDelayFeedback" + juce::String(stripIndex),
                                     defaultOwnedControls.delayFeedback,
                                     stripDelayFeedbackParams[static_cast<size_t>(stripIndex)],
                                     StripControlWriteMode::CacheOnly);
            writeStripFloatParameter("stripDelayLowCut" + juce::String(stripIndex),
                                     defaultOwnedControls.delayLowCutHz,
                                     stripDelayLowCutParams[static_cast<size_t>(stripIndex)],
                                     StripControlWriteMode::CacheOnly);
            writeStripFloatParameter("stripDelayHighCut" + juce::String(stripIndex),
                                     defaultOwnedControls.delayHighCutHz,
                                     stripDelayHighCutParams[static_cast<size_t>(stripIndex)],
                                     StripControlWriteMode::CacheOnly);
            writeStripFloatParameter("stripDelayMode" + juce::String(stripIndex),
                                     static_cast<float>(defaultOwnedControls.delayMode),
                                     stripDelayModeParams[static_cast<size_t>(stripIndex)],
                                     StripControlWriteMode::CacheOnly);
            writeStripFloatParameter("stripPitch" + juce::String(stripIndex),
                                     0.0f,
                                     stripPitchParams[static_cast<size_t>(stripIndex)],
                                     StripControlWriteMode::CacheOnly);
            writeStripFloatParameter("stripSliceLength" + juce::String(stripIndex),
                                     1.0f,
                                     stripSliceLengthParams[static_cast<size_t>(stripIndex)],
                                     StripControlWriteMode::CacheOnly);
            writeStripFloatParameter("stripPitchControlMode" + juce::String(stripIndex),
                                     0.0f,
                                     stripPitchControlModeParams[static_cast<size_t>(stripIndex)],
                                     StripControlWriteMode::CacheOnly);
            writeStripFloatParameter("stripTempoMatchMode" + juce::String(stripIndex),
                                     0.0f,
                                     stripTempoMatchModeParams[static_cast<size_t>(stripIndex)],
                                     StripControlWriteMode::CacheOnly);
            writeStripBoolParameter("stripDuckEnabled" + juce::String(stripIndex),
                                    false,
                                    stripDuckEnabledParams[static_cast<size_t>(stripIndex)],
                                    StripControlWriteMode::CacheOnly);
            writeStripFloatParameter("stripDuckSource" + juce::String(stripIndex),
                                     0.0f,
                                     stripDuckSourceParams[static_cast<size_t>(stripIndex)],
                                     StripControlWriteMode::CacheOnly);
            writeStripFloatParameter("stripDuckThreshold" + juce::String(stripIndex),
                                     -24.0f,
                                     stripDuckThresholdParams[static_cast<size_t>(stripIndex)],
                                     StripControlWriteMode::CacheOnly);
            writeStripFloatParameter("stripDuckRatio" + juce::String(stripIndex),
                                     4.0f,
                                     stripDuckRatioParams[static_cast<size_t>(stripIndex)],
                                     StripControlWriteMode::CacheOnly);
            writeStripFloatParameter("stripDuckAttack" + juce::String(stripIndex),
                                     10.0f,
                                     stripDuckAttackParams[static_cast<size_t>(stripIndex)],
                                     StripControlWriteMode::CacheOnly);
            writeStripFloatParameter("stripDuckRelease" + juce::String(stripIndex),
                                     180.0f,
                                     stripDuckReleaseParams[static_cast<size_t>(stripIndex)],
                                     StripControlWriteMode::CacheOnly);
            writeStripFloatParameter("stripDuckGainComp" + juce::String(stripIndex),
                                     0.0f,
                                     stripDuckGainCompParams[static_cast<size_t>(stripIndex)],
                                     StripControlWriteMode::CacheOnly);
            writeStripBoolParameter("stripDuckFollowMaster" + juce::String(stripIndex),
                                    false,
                                    stripDuckFollowMasterParams[static_cast<size_t>(stripIndex)],
                                    StripControlWriteMode::CacheOnly);
            applyPitchControlToStrip(stripIndex, *strip, 0.0f);
            audioEngine->assignStripToGroup(stripIndex, -1);
            for (int modSlot = 0; modSlot < ModernAudioEngine::NumModSequencers; ++modSlot)
                audioEngine->resetModSequencerSlotToDefaults(stripIndex, modSlot);
            audioEngine->setModSequencerSlot(stripIndex, 0);

            currentStripFiles[static_cast<size_t>(stripIndex)] = juce::File();
            sampleModeRenderedLastBlock[static_cast<size_t>(stripIndex)] = false;

            StripRecallTransitionDecision decision;
            if (shouldFadeStoppedOutput)
            {
                recallContinuityBroken = true;
                decision.kind = StripRecallTransitionKind::Stop;
                decision.continuityBroken = true;
                commitLaunchHandleState(unavailableHandle,
                                        SceneStripLaunchTransitionKind::Stop,
                                        decision,
                                        true);
            }
            else
            {
                decision.kind = StripRecallTransitionKind::Idle;
                commitLaunchHandleState(unavailableHandle,
                                        SceneStripLaunchTransitionKind::Idle,
                                        decision,
                                        false);
            }
        };

        if (!stripState.present)
        {
            stopStripForUnavailableScene(true);
            continue;
        }

        strip->setPlayMode(stripState.playMode);
        strip->setLoop(stripState.loopStart, stripState.loopEnd);
        strip->setDirectionMode(stripState.directionMode);
        strip->setReverse(stripState.reverse);
        strip->setRecordingBars(stripState.recordingBars);
        strip->setBeatsPerLoop(stripState.beatsPerLoop);
        strip->setScratchAmount(stripState.scratchAmount);
        strip->setTransientSliceMode(stripState.transientSliceMode);
        strip->setSwingAmount(stripState.swingAmount);
        strip->setGateAmount(stripState.gateAmount);
        strip->setGateSpeed(stripState.gateSpeed);
        strip->setGateEnvelope(stripState.gateEnvelope);
        strip->setGateShape(stripState.gateShape);
        strip->setStepPatternLengthSteps(stripState.stepPatternSteps);
        strip->setStepPage(stripState.stepViewPage);
        strip->currentStep = stripState.stepCurrent;
        strip->stepPattern = stripState.stepPattern;
        strip->stepSubdivisions = stripState.stepSubdivisions;
        strip->stepSubdivisionStartVelocity = stripState.stepSubdivisionStartVelocity;
        strip->stepSubdivisionRepeatVelocity = stripState.stepSubdivisionRepeatVelocity;
        strip->stepProbability = stripState.stepProbability;
        strip->setStepEnvelopeAttackMs(stripState.stepAttackMs);
        strip->setStepEnvelopeDecayMs(stripState.stepDecayMs);
        strip->setStepEnvelopeReleaseMs(stripState.stepReleaseMs);
        strip->setGrainSizeMs(stripState.grainSizeMs);
        strip->setGrainDensity(stripState.grainDensity);
        strip->setGrainPitch(stripState.grainPitch);
        strip->setGrainPitchJitter(stripState.grainPitchJitter);
        strip->setGrainSpread(stripState.grainSpread);
        strip->setGrainJitter(stripState.grainJitter);
        strip->setGrainPositionJitter(stripState.grainPositionJitter);
        strip->setGrainRandomDepth(stripState.grainRandomDepth);
        strip->setGrainArpDepth(stripState.grainArpDepth);
        strip->setGrainCloudDepth(stripState.grainCloudDepth);
        strip->setGrainEmitterDepth(stripState.grainEmitterDepth);
        strip->setGrainEnvelope(stripState.grainEnvelope);
        strip->setGrainShape(stripState.grainShape);
        strip->setGrainArpMode(stripState.grainArpMode);
        strip->setGrainTempoSyncEnabled(stripState.grainTempoSync);
        audioEngine->assignStripToGroup(stripIndex, stripState.groupId);
        setRecentSampleDirectory(stripIndex, SamplePathMode::Loop, stripState.recentLoopDirectory, false);
        setRecentSampleDirectory(stripIndex, SamplePathMode::Step, stripState.recentStepDirectory, false);
        setRecentSampleDirectory(stripIndex, SamplePathMode::Flip, stripState.recentFlipDirectory, false);
        if (stripState.playMode == EnhancedAudioStrip::PlayMode::Sample && stripState.hasFlipState)
        {
            auto flipStateXml = stripState.flipState.createXml("FlipState");
            if (flipStateXml != nullptr)
            {
                if (stripState.embeddedFlipSampleBase64.isNotEmpty())
                    flipStateXml->setAttribute(kEmbeddedFlipSampleAttr, stripState.embeddedFlipSampleBase64);
                applyFlipPresetStateXml(stripIndex, flipStateXml.get());
            }
        }
        if (stripState.loopPitchState.valid)
            applyPreparedSceneLoopPitchState(stripIndex, stripState.loopPitchState);

        const auto& paramState = stripState.parameterState;
        writeStripFloatParameter("stripVolume" + juce::String(stripIndex),
                                 paramState.ownedControls.volume,
                                 stripVolumeParams[static_cast<size_t>(stripIndex)],
                                 StripControlWriteMode::CacheOnly);
        writeStripFloatParameter("stripTrimDb" + juce::String(stripIndex),
                                 paramState.ownedControls.trimDb,
                                 stripTrimDbParams[static_cast<size_t>(stripIndex)],
                                 StripControlWriteMode::CacheOnly);
        writeStripFloatParameter("stripPan" + juce::String(stripIndex),
                                 paramState.ownedControls.pan,
                                 stripPanParams[static_cast<size_t>(stripIndex)],
                                 StripControlWriteMode::CacheOnly);
        const float storedSpeedControl = paramState.ownedControls.usesGrainPlaybackSpeed
            ? juce::jlimit(0.0f,
                           8.0f,
                           PlayheadSpeedQuantizer::grainControlValueFromPlaybackSpeed(
                               paramState.ownedControls.playbackSpeed))
            : juce::jlimit(0.0f, 8.0f, paramState.ownedControls.playheadSpeedRatio);
        writeStripFloatParameter("stripSpeed" + juce::String(stripIndex),
                                 storedSpeedControl,
                                 stripSpeedParams[static_cast<size_t>(stripIndex)],
                                 StripControlWriteMode::CacheOnly);
        writeStripBoolParameter("stripFilterEnabled" + juce::String(stripIndex),
                                paramState.ownedControls.filterEnabled,
                                stripFilterEnabledParams[static_cast<size_t>(stripIndex)],
                                StripControlWriteMode::CacheOnly);
        writeStripFloatParameter("stripFilterFrequency" + juce::String(stripIndex),
                                 paramState.ownedControls.filterFrequency,
                                 stripFilterFrequencyParams[static_cast<size_t>(stripIndex)],
                                 StripControlWriteMode::CacheOnly);
        writeStripFloatParameter("stripFilterResonance" + juce::String(stripIndex),
                                 paramState.ownedControls.filterResonance,
                                 stripFilterResonanceParams[static_cast<size_t>(stripIndex)],
                                 StripControlWriteMode::CacheOnly);
        writeStripFloatParameter("stripFilterMorph" + juce::String(stripIndex),
                                 paramState.ownedControls.filterMorph,
                                 stripFilterMorphParams[static_cast<size_t>(stripIndex)],
                                 StripControlWriteMode::CacheOnly);
        writeStripFloatParameter("stripFilterAlgorithm" + juce::String(stripIndex),
                                 static_cast<float>(paramState.ownedControls.filterAlgorithm),
                                 stripFilterAlgorithmParams[static_cast<size_t>(stripIndex)],
                                 StripControlWriteMode::CacheOnly);
        writeStripFloatParameter("stripDelayMix" + juce::String(stripIndex),
                                 paramState.ownedControls.delayMix,
                                 stripDelayMixParams[static_cast<size_t>(stripIndex)],
                                 StripControlWriteMode::CacheOnly);
        writeStripFloatParameter("stripDelayTime" + juce::String(stripIndex),
                                 paramState.ownedControls.delayTime,
                                 stripDelayTimeParams[static_cast<size_t>(stripIndex)],
                                 StripControlWriteMode::CacheOnly);
        writeStripBoolParameter("stripDelaySync" + juce::String(stripIndex),
                                paramState.ownedControls.delaySyncEnabled,
                                stripDelaySyncParams[static_cast<size_t>(stripIndex)],
                                StripControlWriteMode::CacheOnly);
        writeStripFloatParameter("stripDelayFeedback" + juce::String(stripIndex),
                                 paramState.ownedControls.delayFeedback,
                                 stripDelayFeedbackParams[static_cast<size_t>(stripIndex)],
                                 StripControlWriteMode::CacheOnly);
        writeStripFloatParameter("stripDelayLowCut" + juce::String(stripIndex),
                                 paramState.ownedControls.delayLowCutHz,
                                 stripDelayLowCutParams[static_cast<size_t>(stripIndex)],
                                 StripControlWriteMode::CacheOnly);
        writeStripFloatParameter("stripDelayHighCut" + juce::String(stripIndex),
                                 paramState.ownedControls.delayHighCutHz,
                                 stripDelayHighCutParams[static_cast<size_t>(stripIndex)],
                                 StripControlWriteMode::CacheOnly);
        writeStripFloatParameter("stripDelayMode" + juce::String(stripIndex),
                                 static_cast<float>(paramState.ownedControls.delayMode),
                                 stripDelayModeParams[static_cast<size_t>(stripIndex)],
                                 StripControlWriteMode::CacheOnly);
        writeStripFloatParameter("stripPitch" + juce::String(stripIndex),
                                 paramState.pitchSemitones,
                                 stripPitchParams[static_cast<size_t>(stripIndex)],
                                 StripControlWriteMode::CacheOnly);
        writeStripFloatParameter("stripSliceLength" + juce::String(stripIndex),
                                 paramState.sliceLength,
                                 stripSliceLengthParams[static_cast<size_t>(stripIndex)],
                                 StripControlWriteMode::CacheOnly);
        writeStripFloatParameter("stripPitchControlMode" + juce::String(stripIndex),
                                 static_cast<float>(paramState.pitchControlMode),
                                 stripPitchControlModeParams[static_cast<size_t>(stripIndex)],
                                 StripControlWriteMode::CacheOnly);
        writeStripFloatParameter("stripTempoMatchMode" + juce::String(stripIndex),
                                 static_cast<float>(paramState.tempoMatchMode),
                                 stripTempoMatchModeParams[static_cast<size_t>(stripIndex)],
                                 StripControlWriteMode::CacheOnly);
        writeStripBoolParameter("stripDuckEnabled" + juce::String(stripIndex),
                                paramState.duckEnabled,
                                stripDuckEnabledParams[static_cast<size_t>(stripIndex)],
                                StripControlWriteMode::CacheOnly);
        writeStripFloatParameter("stripDuckSource" + juce::String(stripIndex),
                                 static_cast<float>(paramState.duckSource),
                                 stripDuckSourceParams[static_cast<size_t>(stripIndex)],
                                 StripControlWriteMode::CacheOnly);
        writeStripFloatParameter("stripDuckThreshold" + juce::String(stripIndex),
                                 paramState.duckThresholdDb,
                                 stripDuckThresholdParams[static_cast<size_t>(stripIndex)],
                                 StripControlWriteMode::CacheOnly);
        writeStripFloatParameter("stripDuckRatio" + juce::String(stripIndex),
                                 paramState.duckRatio,
                                 stripDuckRatioParams[static_cast<size_t>(stripIndex)],
                                 StripControlWriteMode::CacheOnly);
        writeStripFloatParameter("stripDuckAttack" + juce::String(stripIndex),
                                 paramState.duckAttackMs,
                                 stripDuckAttackParams[static_cast<size_t>(stripIndex)],
                                 StripControlWriteMode::CacheOnly);
        writeStripFloatParameter("stripDuckRelease" + juce::String(stripIndex),
                                 paramState.duckReleaseMs,
                                 stripDuckReleaseParams[static_cast<size_t>(stripIndex)],
                                 StripControlWriteMode::CacheOnly);
        writeStripFloatParameter("stripDuckGainComp" + juce::String(stripIndex),
                                 paramState.duckGainCompDb,
                                 stripDuckGainCompParams[static_cast<size_t>(stripIndex)],
                                 StripControlWriteMode::CacheOnly);
        writeStripBoolParameter("stripDuckFollowMaster" + juce::String(stripIndex),
                                paramState.duckFollowMaster,
                                stripDuckFollowMasterParams[static_cast<size_t>(stripIndex)],
                                StripControlWriteMode::CacheOnly);

        applyResolvedOwnedStripControlState(*strip, paramState.ownedControls);
        applyResolvedStripFilterState(*strip, paramState.ownedControls);
        applyResolvedStripDelayState(*strip, paramState.ownedControls);
        strip->setLoopSliceLength(paramState.sliceLength);
        strip->setTempoMatchBackend(resolveLoopTempoMatchBackendForStrip(stripIndex));
        strip->setDuckEnabled(paramState.duckEnabled);
        strip->setDuckSourceSelection(paramState.duckSource);
        strip->setDuckThresholdDb(paramState.duckThresholdDb);
        strip->setDuckRatio(paramState.duckRatio);
        strip->setDuckAttackMs(paramState.duckAttackMs);
        strip->setDuckReleaseMs(paramState.duckReleaseMs);
        strip->setDuckGainCompDb(paramState.duckGainCompDb);
        strip->setDuckFollowMaster(paramState.duckFollowMaster);

        for (int modSlot = 0; modSlot < ModernAudioEngine::NumModSequencers; ++modSlot)
        {
            const auto& lane = stripState.modLanes[static_cast<size_t>(modSlot)];
            audioEngine->setModSequencerSlot(stripIndex, modSlot);
            audioEngine->resetModSequencerSlotToDefaults(stripIndex, modSlot);
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
                const auto stepArrayIndex = static_cast<size_t>(stepIndex);
                audioEngine->setModStepValueAbsolute(stripIndex, stepIndex, lane.steps[stepArrayIndex]);
                audioEngine->setModStepShapeAbsolute(stripIndex,
                                                     stepIndex,
                                                     juce::jlimit(1,
                                                                  ModernAudioEngine::ModMaxStepSubdivisions,
                                                                  lane.stepSubdivisions[stepArrayIndex]),
                                                     juce::jlimit(0.0f,
                                                                  1.0f,
                                                                  lane.stepEndValues[stepArrayIndex]));
                audioEngine->setModStepCurveShapeAbsolute(stripIndex,
                                                          stepIndex,
                                                          lane.stepCurveShapes[stepArrayIndex]);
            }
        }
        audioEngine->setModSequencerSlot(stripIndex, stripState.activeModSlot);

        const bool restorePlaying = canRecallStripPlayback && stripState.restorePlaying;
        const int effectiveRestoreMarkerColumn = stripState.loopStart;
        bool effectiveRestorePpqAnchored = false;
        double effectiveRestorePpqOffsetBeats = 0.0;
        if (restorePlaying)
        {
            const double beatsForLoop = (stripState.beatsPerLoop >= 0.0f)
                ? static_cast<double>(stripState.beatsPerLoop)
                : 4.0;
            if (beatsForLoop > 0.0 && std::isfinite(recallPpq) && recallTempo > 0.0)
            {
                double currentBeatInLoop = std::fmod(recallPpq, beatsForLoop);
                if (currentBeatInLoop < 0.0)
                    currentBeatInLoop += beatsForLoop;

                effectiveRestorePpqAnchored = true;
                effectiveRestorePpqOffsetBeats = std::fmod(-currentBeatInLoop, beatsForLoop);
                if (effectiveRestorePpqOffsetBeats < 0.0)
                    effectiveRestorePpqOffsetBeats += beatsForLoop;
            }
        }

        auto computeIncomingAudioSourceSignature = [&]() -> uint64_t
        {
            if (stripState.hasStoredSamplePath)
            {
                uint64_t hash = hashStringForSceneLaunch(stripState.storedSampleFile.getFullPathName());
                if (stripState.storedSampleFile.existsAsFile())
                {
                    hash = mixSceneLaunchHash(hash,
                                              static_cast<uint64_t>(stripState.storedSampleFile.getSize()));
                    hash = mixSceneLaunchHash(
                        hash,
                        static_cast<uint64_t>(stripState.storedSampleFile.getLastModificationTime().toMilliseconds()));
                }
                return hash;
            }

            const auto& audioPayload = payload.stripAudioPayloads[static_cast<size_t>(stripIndex)];
            if (!audioPayload.valid)
                return 0;

            uint64_t hash = 1469598103934665603ULL;
            hash = mixSceneLaunchHash(hash, static_cast<uint64_t>(audioPayload.audioBuffer.getNumChannels()));
            hash = mixSceneLaunchHash(hash, static_cast<uint64_t>(audioPayload.audioBuffer.getNumSamples()));
            hash = mixSceneLaunchHash(hash, static_cast<uint64_t>(audioPayload.analysisSampleCount));
            hash = mixSceneLaunchHash(hash, hashFloatForSceneLaunch(static_cast<float>(audioPayload.sourceSampleRate)));
            if (audioPayload.audioBuffer.getNumSamples() > 0 && audioPayload.audioBuffer.getNumChannels() > 0)
            {
                hash = mixSceneLaunchHash(hash,
                                          hashFloatForSceneLaunch(audioPayload.audioBuffer.getSample(0, 0)));
                hash = mixSceneLaunchHash(
                    hash,
                    hashFloatForSceneLaunch(audioPayload.audioBuffer.getSample(
                        0,
                        audioPayload.audioBuffer.getNumSamples() - 1)));
            }
            return hash;
        };

        SceneStripPlaybackHandle incomingLaunchHandle;
        incomingLaunchHandle.reset();
        incomingLaunchHandle.valid = true;
        incomingLaunchHandle.present = true;
        incomingLaunchHandle.playing = restorePlaying;
        incomingLaunchHandle.sequenceDriven = event.sequenceDriven;
        incomingLaunchHandle.mainPresetIndex = payload.mainPresetIndex;
        incomingLaunchHandle.sceneSlot = payload.sceneSlot;
        incomingLaunchHandle.stripIndex = stripIndex;
        incomingLaunchHandle.sequenceStepIndex = event.sequenceStepIndex;
        incomingLaunchHandle.playMode = stripState.playMode;
        incomingLaunchHandle.loopStart = stripState.loopStart;
        incomingLaunchHandle.loopEnd = stripState.loopEnd;
        incomingLaunchHandle.recordingBars = stripState.recordingBars;
        incomingLaunchHandle.beatsPerLoop = stripState.beatsPerLoop;
        incomingLaunchHandle.playheadSpeedRatio = paramState.ownedControls.playheadSpeedRatio;
        incomingLaunchHandle.directionMode = stripState.directionMode;
        incomingLaunchHandle.reverse = stripState.reverse;
        incomingLaunchHandle.ppqTimelineAnchored = effectiveRestorePpqAnchored;
        incomingLaunchHandle.ppqTimelineOffsetBeats = effectiveRestorePpqOffsetBeats;
        incomingLaunchHandle.playbackColumn = effectiveRestoreMarkerColumn;
        incomingLaunchHandle.sampleFile = stripState.hasStoredSamplePath ? stripState.storedSampleFile : juce::File();
        incomingLaunchHandle.audioSourceSignature = computeIncomingAudioSourceSignature();

        const auto& previousPitchBlendSnapshot = previousPitchBlendSnapshots[static_cast<size_t>(stripIndex)];
        const bool shouldBlendPitchPath = std::abs(previousPitchBlendSnapshot.pitch - paramState.pitchSemitones) > 1.0e-4f
            || std::abs(previousPitchBlendSnapshot.pitchControlMode
                        - static_cast<float>(paramState.pitchControlMode)) > 1.0e-4f;
        const auto transitionDecision = classifyStripRecallTransition(previousLaunchHandle,
                                                                      previousLaunchHandle.valid
                                                                          && previousLaunchHandle.present,
                                                                      restorePlaying,
                                                                      incomingLaunchHandle,
                                                                      shouldBlendPitchPath);

        const bool shouldReuseLoadedStrip = transitionDecision.kind == StripRecallTransitionKind::Continue;
        bool loadedStripAudio = shouldReuseLoadedStrip;
        if (!shouldReuseLoadedStrip)
            loadedStripAudio = loadPreparedSceneStripAudioToActiveEngine(payload, stripIndex);
        if (!loadedStripAudio && !shouldReuseLoadedStrip)
            strip->clearSample();

        const bool loadedFromStoredFile = stripState.hasStoredSamplePath
            && (shouldReuseLoadedStrip || loadedStripAudio);
        rememberLoadedSamplePathForStrip(stripIndex,
                                         loadedFromStoredFile ? stripState.storedSampleFile : juce::File(),
                                         false);

        recallContinuityBroken = recallContinuityBroken || transitionDecision.continuityBroken;

        if (!loadedStripAudio)
        {
            // The scene stored real content for this strip but it could not be
            // loaded (missing/unreadable sample). The wiped strip must not be
            // re-captured over the stored snapshot by any automatic save.
            anyStoredStripContentUnavailable = true;
            stopStripForUnavailableScene(true);
            continue;
        }

        if (transitionDecision.kind == StripRecallTransitionKind::Continue)
        {
            applyPitchControlToStrip(stripIndex, *strip, paramState.pitchSemitones);
            commitLaunchHandleState(incomingLaunchHandle,
                                    SceneStripLaunchTransitionKind::Continue,
                                    transitionDecision,
                                    true);
            continue;
        }

        if (transitionDecision.kind == StripRecallTransitionKind::Stop)
        {
            strip->setPlaybackMarkerColumn(effectiveRestoreMarkerColumn, restoreGlobalSample);
            strip->stopForSceneRecallWithOutputFade(&previousLaunchHandle.continuityBlendState);
            incomingLaunchHandle.playing = false;
            commitLaunchHandleState(incomingLaunchHandle,
                                    SceneStripLaunchTransitionKind::Stop,
                                    transitionDecision,
                                    true);
            continue;
        }

        if (transitionDecision.kind == StripRecallTransitionKind::Idle)
        {
            strip->setPlaybackMarkerColumn(effectiveRestoreMarkerColumn, restoreGlobalSample);
            strip->stop(true);
            incomingLaunchHandle.playing = false;
            commitLaunchHandleState(incomingLaunchHandle,
                                    SceneStripLaunchTransitionKind::Idle,
                                    transitionDecision,
                                    false);
            continue;
        }

        applyPitchControlToStrip(stripIndex, *strip, paramState.pitchSemitones);
        strip->restorePresetPpqState(restorePlaying,
                                     effectiveRestorePpqAnchored,
                                     effectiveRestorePpqOffsetBeats,
                                     effectiveRestoreMarkerColumn,
                                     recallTempo,
                                     recallPpq,
                                     restoreGlobalSample,
                                     transitionDecision.shouldBlendPitchPath,
                                     transitionDecision.shouldApplyRecallOutputBlend,
                                     transitionDecision.shouldApplyRecallEdgeFade,
                                     &previousLaunchHandle.continuityBlendState);
        commitLaunchHandleState(incomingLaunchHandle,
                                transitionDecision.kind == StripRecallTransitionKind::NewStart
                                    ? SceneStripLaunchTransitionKind::NewStart
                                    : SceneStripLaunchTransitionKind::Restart,
                                transitionDecision,
                                true);
    }

    for (int groupIndex = 0; groupIndex < ModernAudioEngine::MaxGroups; ++groupIndex)
    {
        if (auto* group = audioEngine->getGroup(groupIndex))
        {
            group->setVolume(payload.groupStates[static_cast<size_t>(groupIndex)].volume);
            group->setMuted(payload.groupStates[static_cast<size_t>(groupIndex)].muted);
        }
    }

    for (int patternIndex = 0; patternIndex < ModernAudioEngine::MaxPatterns; ++patternIndex)
    {
        audioEngine->clearPattern(patternIndex);
        const auto& patternState = payload.patternStates[static_cast<size_t>(patternIndex)];
        if (!patternState.present)
            continue;

        if (auto* pattern = audioEngine->getPattern(patternIndex))
        {
            pattern->setEventsSnapshot(patternState.events, patternState.lengthBeats);
            if (canRecallPatternPlayback
                && std::isfinite(audioEngine->getTimelineBeat())
                && patternState.playing
                && !patternState.events.empty())
            {
                pattern->startPlayback(audioEngine->getTimelineBeat());
            }
        }
    }

    applyPreparedSceneTimingState(payload.sceneSlot, payload.sceneTimingState);
    applyScenePerformanceStateData(payload.scenePerformanceStateData, payload.sceneSlot);

    setSceneChainAttachStartPpq(event.targetPpq);
    applySceneClipSlotRuntimeState(payload.mainPresetIndex, payload.sceneSlot);
    activeSceneRecallDegraded.store(anyStoredStripContentUnavailable ? 1 : 0,
                                    std::memory_order_release);
    activeSceneNeedsCaptureBeforeManualRecall =
        getSceneClipSlotState(payload.sceneSlot, payload.mainPresetIndex).liveStripControlDirty;
    resolveLoopPitchRecallStateImmediately();
    syncScenePerformanceClipLengthToResolvedLength(payload.sceneSlot);
    if (isSceneModeEnabled())
        applySceneMotionStateOrDefaultsToEngine(payload.sceneSlot);

    if (std::isfinite(event.targetPpq))
    {
        const int64_t recallSample = event.targetGlobalSample >= 0
            ? event.targetGlobalSample
            : (audioEngine != nullptr ? audioEngine->getGlobalSampleCount() : -1);

        for (int stripIndex = 0; stripIndex < MaxStrips; ++stripIndex)
        {
            auto* strip = audioEngine->getStrip(stripIndex);
            if (strip == nullptr || !strip->isPlaying() || !strip->isPpqTimelineAnchored())
                continue;

            if (strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Step
                || strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Sample)
                continue;

            if (!strip->hasAudio())
                continue;

            strip->realignToPpqAnchor(event.targetPpq, recallSample);
        }
    }

    if (payload.parameterState.isValid())
    {
        suppressOwnedStripParameterSync.store(1, std::memory_order_release);
        queuePendingSceneParameterState(payload.parameterState);
    }
    else
    {
        // Legacy/partial scene without a parameter snapshot: the CacheOnly
        // writes above changed the raw atomics but not the APVTS parameters.
        // Keep the per-block sync suppressed until the message thread has
        // pushed the raw values back into the parameters, otherwise the stale
        // parameter values would immediately overwrite the recalled scene.
        pendingSceneRawParameterResync.store(1, std::memory_order_release);
        suppressOwnedStripParameterSync.store(1, std::memory_order_release);
    }

    syncSceneModeFromParameters();
    if (isSceneModeEnabled())
        clearAllStripGroupsForSceneMode();

    return true;
}

bool MlrVSTAudioProcessor::renderPendingPreparedSceneSwitch(juce::AudioBuffer<float>& buffer,
                                                            juce::MidiBuffer& midiMessages,
                                                            const juce::AudioPlayHead::PositionInfo& posInfo,
                                                            int64_t blockStartSample)
{
    const bool separateStripRouting = outputRoutingParam != nullptr && *outputRoutingParam > 0.5f;
    if (buffer.getNumSamples() <= 0)
    {
        return false;
    }

    SceneSwitchEvent pendingSwitchEvent;
    if (!peekPendingSceneApplyState(pendingSwitchEvent))
        return false;

    if (!hasPreparedSceneSwitchPayloadForEvent(pendingSwitchEvent))
        return false;

    if (pendingSwitchEvent.targetGlobalSample >= 0
        && blockStartSample >= 0
        && pendingSwitchEvent.targetGlobalSample >= blockStartSample + buffer.getNumSamples())
    {
        return false;
    }

    SceneSwitchEvent switchEvent;
    if (!consumePendingSceneApplyState(switchEvent))
        return false;

    auto* preparedPayload = takePreparedSceneSwitchPayloadForEvent(switchEvent);
    if (preparedPayload == nullptr)
    {
        queuePendingSceneApplyState(switchEvent);
        return false;
    }

    const int numSamples = juce::jmax(0, buffer.getNumSamples());
    const int targetOffset = resolveSceneSwitchTargetOffsetForCurrentBlock(switchEvent,
                                                                           blockStartSample,
                                                                           numSamples);

    auto makeAdvancedPositionInfo = [&](int sampleOffset)
    {
        juce::AudioPlayHead::PositionInfo adjustedPos = posInfo;
        if (sampleOffset > 0
            && posInfo.getPpqPosition().hasValue()
            && posInfo.getBpm().hasValue()
            && *posInfo.getBpm() > 0.0
            && currentSampleRate > 0.0)
        {
            const double ppqAdvance = (static_cast<double>(sampleOffset) * *posInfo.getBpm())
                / (60.0 * currentSampleRate);
            adjustedPos.setPpqPosition(*posInfo.getPpqPosition() + ppqAdvance);
        }
        return adjustedPos;
    };

    auto copyRenderedRange = [](const juce::AudioBuffer<float>& source,
                                juce::AudioBuffer<float>& destination,
                                int sourceStart,
                                int destinationStart,
                                int samplesToCopy)
    {
        if (samplesToCopy <= 0)
            return;

        const int numChannels = juce::jmin(source.getNumChannels(), destination.getNumChannels());
        for (int channel = 0; channel < numChannels; ++channel)
            destination.copyFrom(channel, destinationStart, source, channel, sourceStart, samplesToCopy);
    };

    if (targetOffset > 0)
    {
        if (separateStripRouting)
        {
            auto& outgoingBuffer = preparedSceneSwitchOutgoingBuffer;
            if (outgoingBuffer.getNumChannels() < buffer.getNumChannels()
                || outgoingBuffer.getNumSamples() < numSamples)
            {
                outgoingBuffer.setSize(buffer.getNumChannels(), numSamples, false, false, true);
            }
            outgoingBuffer.clear();
            juce::MidiBuffer outgoingMidi(midiMessages);
            renderActiveSceneAudio(outgoingBuffer, outgoingMidi, posInfo, true, true);
            buffer.clear();
            copyRenderedRange(outgoingBuffer, buffer, 0, 0, targetOffset);
        }
        else
        {
            juce::MidiBuffer outgoingMidi(midiMessages);
            renderActiveSceneAudio(buffer, outgoingMidi, posInfo, true, true);
        }
    }
    else
    {
        buffer.clear();
    }

    bool recallContinuityBroken = false;
    if (!applyPreparedSceneSwitchPayload(*preparedPayload, switchEvent, recallContinuityBroken))
    {
        retirePreparedSceneSwitchPayload(preparedPayload);
        queuePendingSceneApplyState(switchEvent);
        return false;
    }
    retirePreparedSceneSwitchPayload(preparedPayload);

    clearSceneBoundaryTransitionState(true);

    const int chainLength = getSceneChainLength();
    const int preservedFocusedSceneSlot = getFocusedSceneSlot();
    const int focusSceneSlotAfterSwitch = switchEvent.sequenceDriven
        ? preservedFocusedSceneSlot
        : juce::jlimit(0, SceneSlots - 1, switchEvent.sceneSlot);
    int appliedSequenceStepIndex = -1;
    if (switchEvent.sequenceDriven && chainLength >= 2)
    {
        const int fallbackStepIndex = getSceneSequenceStepIndex(switchEvent.sceneSlot);
        appliedSequenceStepIndex = juce::jlimit(0,
                                                chainLength - 1,
                                                juce::jmax(0,
                                                           switchEvent.sequenceStepIndex >= 0
                                                               ? switchEvent.sequenceStepIndex
                                                               : fallbackStepIndex));
    }
    const int outgoingSequenceStepIndex = sceneSequenceActive ? sceneSequenceCurrentStepIndex : -1;

    activeSceneMainPresetIndex = switchEvent.mainPresetIndex;
    activeSceneSlot = switchEvent.sceneSlot;
    switchScenePlaybackOwner(switchEvent.sequenceDriven ? ScenePlaybackOwner::Chain
                                                        : ScenePlaybackOwner::Manual,
                             switchEvent.sequenceDriven && chainLength >= 2,
                             appliedSequenceStepIndex);
    const double appliedSceneStartPpq = std::isfinite(switchEvent.targetPpq)
        ? switchEvent.targetPpq
        : std::numeric_limits<double>::quiet_NaN();
    setActiveScenePlaybackHandle(switchEvent.mainPresetIndex,
                                 switchEvent.sceneSlot,
                                 switchEvent.sequenceDriven,
                                 switchEvent.sequenceDriven ? appliedSequenceStepIndex : -1,
                                 appliedSceneStartPpq,
                                 computeCurrentSceneSequenceLengthBeats());
    setSceneChainAttachStartPpq(appliedSceneStartPpq);
    focusSceneSlot(focusSceneSlotAfterSwitch);
    if (PresetStore::presetExists(switchEvent.mainPresetIndex))
        loadedPresetIndex = switchEvent.mainPresetIndex;
    if (switchEvent.sequenceDriven)
    {
        // Mirrors the stopped-transport apply path: a Return ("Back") step arms
        // an override consumed by armNextSceneInSequence so the chain bounces
        // back to it after the entered step ends.
        SceneScheduler::armReturnRouteForSequenceHandoff(*this,
                                                         outgoingSequenceStepIndex,
                                                         appliedSequenceStepIndex,
                                                         switchEvent.targetPpq);
        armNextSceneInSequence(switchEvent.mainPresetIndex, switchEvent.sceneSlot, appliedSceneStartPpq);
    }
    else
    {
        clearSceneChainReturnOverride();
        activeScenePlaybackHandle.sequenceDriven = false;
        activeScenePlaybackHandle.sequenceStepIndex = -1;
    }
    presetRefreshToken.fetch_add(1, std::memory_order_acq_rel);
    updateMonomeLEDs();

    juce::ignoreUnused(recallContinuityBroken);
    if (targetOffset < numSamples)
    {
        if (separateStripRouting)
        {
            auto& incomingBuffer = preparedSceneSwitchIncomingBuffer;
            if (incomingBuffer.getNumChannels() < buffer.getNumChannels()
                || incomingBuffer.getNumSamples() < numSamples)
            {
                incomingBuffer.setSize(buffer.getNumChannels(), numSamples, false, false, true);
            }
            incomingBuffer.clear();
            juce::MidiBuffer incomingMidi;
            renderActiveSceneAudio(incomingBuffer,
                                   incomingMidi,
                                   makeAdvancedPositionInfo(targetOffset),
                                   true,
                                   false);
            copyRenderedRange(incomingBuffer, buffer, 0, targetOffset, numSamples - targetOffset);
        }
        else
        {
            renderActiveSceneAudioRange(buffer, midiMessages, posInfo, targetOffset);
        }
    }

    return true;
}
