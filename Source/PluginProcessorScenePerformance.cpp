/*
  ==============================================================================

    PluginProcessorScenePerformance.cpp
    Extracted scene performance recording and playback handling for mlrVST.

  ==============================================================================
*/

#include "PluginProcessor.h"
#include <cmath>
#include <limits>

namespace
{
constexpr int kSceneAutomationGlobalMaskIndex = MlrVSTAudioProcessor::MaxStrips;

double wrapBeatIntoSceneClip(double currentBeat, double sceneStartBeat, double lengthBeats) noexcept
{
    const double safeLength = juce::jmax(1.0, lengthBeats);
    if (!std::isfinite(currentBeat) || !std::isfinite(sceneStartBeat))
        return 0.0;

    const double relativeBeat = currentBeat - sceneStartBeat;
    const double wrapped = std::fmod(relativeBeat, safeLength);
    return wrapped >= 0.0 ? wrapped : wrapped + safeLength;
}

bool findHeldSceneAutomationEventAtClipBeat(const std::vector<ScenePerformanceEvent>& events,
                                            int stripIndex,
                                            ScenePerformanceControlTarget target,
                                            double clipBeat,
                                            ScenePerformanceEvent& outEvent)
{
    const ScenePerformanceEvent* lastEvent = nullptr;
    const ScenePerformanceEvent* chosenEvent = nullptr;
    for (const auto& event : events)
    {
        if (event.type != ScenePerformanceEventType::ControlPoint
            || event.controlTarget != target
            || event.stripIndex != stripIndex)
        {
            continue;
        }

        lastEvent = &event;
        if (event.timeBeats <= clipBeat + 1.0e-6)
            chosenEvent = &event;
    }

    if (chosenEvent == nullptr)
        chosenEvent = lastEvent;
    if (chosenEvent == nullptr)
        return false;

    outEvent = *chosenEvent;
    return true;
}

float sceneStutterAmountFromDivisionBeats(double divisionBeats)
{
    const double v = juce::jlimit(0.03125, 2.0, divisionBeats);
    if (v >= 1.5) return 0.0625f;       // 1/2
    if (v >= 0.75) return 0.1875f;      // 1/4
    if (v >= 0.375) return 0.3125f;     // 1/8
    if (v >= 0.1875) return 0.4375f;    // 1/16
    if (v >= 0.09375) return 0.625f;    // 1/32
    if (v >= 0.046875) return 0.875f;   // 1/64
    return 1.0f;                        // 1/128 folds into the top lane bucket
}
} // namespace

void MlrVSTAudioProcessor::updateSceneRecorderQuantizedAction(const juce::AudioPlayHead::PositionInfo& posInfo,
                                                              int numSamples)
{
    if (!pendingSceneRecorderAction.active || audioEngine == nullptr)
        return;

    if (pendingSceneRecorderApplyAction.load(std::memory_order_acquire) != static_cast<int>(SceneRecorderAction::None))
        return;

    const int sceneSlot = juce::jlimit(0, SceneSlots - 1, pendingSceneRecorderAction.sceneSlot);
    if (sceneSlot != juce::jlimit(0, SceneSlots - 1, activeSceneSlot))
    {
        clearPendingSceneRecorderAction();
        return;
    }

    if (!posInfo.getIsPlaying())
    {
        pendingSceneRecorderAction.targetResolved = false;
        pendingSceneRecorderAction.patternEndPhaseSignatureValid = false;
        pendingSceneRecorderAction.patternEndPhaseSignature = 0;
        return;
    }

    const auto ppqOpt = posInfo.getPpqPosition();
    const auto bpmOpt = posInfo.getBpm();
    if (!ppqOpt.hasValue() || !bpmOpt.hasValue()
        || !std::isfinite(*ppqOpt) || !std::isfinite(*bpmOpt)
        || *bpmOpt <= 0.0 || currentSampleRate <= 1.0)
    {
        return;
    }

    const double currentPpq = *ppqOpt;
    const auto sceneRecallMode = getSceneRecallMode();
    const bool patternEndEnabled = sceneRecallMode == SceneRecallMode::PatternEnd;
    const bool sceneEndEnabled = sceneRecallMode == SceneRecallMode::SceneEnd;
    uint64_t phaseAlignedPatternEndSignature = 0;
    const double currentSceneDurationBeats = juce::jlimit(0.25, 4096.0, computeCurrentSceneSequenceLengthBeats());
    const double phaseAlignedPatternEndPpq = patternEndEnabled
        ? computeNextScenePatternEndPpq(sceneSlot,
                                        currentPpq,
                                        currentSceneDurationBeats,
                                        &phaseAlignedPatternEndSignature)
        : std::numeric_limits<double>::quiet_NaN();
    const bool phaseAlignedTimingReady = patternEndEnabled && std::isfinite(phaseAlignedPatternEndPpq);
    const bool sceneEndTimingReady = sceneEndEnabled
        && activeSceneStartPpqValid
        && std::isfinite(activeSceneStartPpq);
    const bool useSceneDurationTiming = phaseAlignedTimingReady || sceneEndTimingReady;
    double intervalBeatsNow = getSceneRecallIntervalBeats();
    if (useSceneDurationTiming)
        intervalBeatsNow = currentSceneDurationBeats;

    if (pendingSceneRecorderAction.targetResolved
        && std::abs(intervalBeatsNow - pendingSceneRecorderAction.intervalBeats) > 1.0e-9)
    {
        pendingSceneRecorderAction.targetResolved = false;
        pendingSceneRecorderAction.targetPpq = 0.0;
    }

    if (patternEndEnabled)
    {
        if (phaseAlignedTimingReady)
        {
            if (!pendingSceneRecorderAction.patternEndPhaseSignatureValid
                || pendingSceneRecorderAction.patternEndPhaseSignature != phaseAlignedPatternEndSignature)
            {
                pendingSceneRecorderAction.targetResolved = false;
                pendingSceneRecorderAction.targetPpq = 0.0;
            }

            pendingSceneRecorderAction.patternEndPhaseSignatureValid = true;
            pendingSceneRecorderAction.patternEndPhaseSignature = phaseAlignedPatternEndSignature;
        }
        else if (pendingSceneRecorderAction.patternEndPhaseSignatureValid)
        {
            pendingSceneRecorderAction.patternEndPhaseSignatureValid = false;
            pendingSceneRecorderAction.patternEndPhaseSignature = 0;
            pendingSceneRecorderAction.targetResolved = false;
            pendingSceneRecorderAction.targetPpq = 0.0;
        }

        if (!phaseAlignedTimingReady)
        {
            pendingSceneRecorderAction.targetResolved = false;
            pendingSceneRecorderAction.targetPpq = 0.0;
            return;
        }
    }
    else if (pendingSceneRecorderAction.patternEndPhaseSignatureValid)
    {
        pendingSceneRecorderAction.patternEndPhaseSignatureValid = false;
        pendingSceneRecorderAction.patternEndPhaseSignature = 0;
    }

    if (!pendingSceneRecorderAction.targetResolved)
    {
        pendingSceneRecorderAction.intervalBeats = intervalBeatsNow;
        if (phaseAlignedTimingReady)
        {
            pendingSceneRecorderAction.targetPpq = phaseAlignedPatternEndPpq;
        }
        else if (useSceneDurationTiming)
        {
            double nextTarget = activeSceneStartPpq + intervalBeatsNow;
            if (nextTarget <= currentPpq + 1.0e-9)
            {
                const double elapsed = juce::jmax(0.0, currentPpq - activeSceneStartPpq);
                const double completedCycles = std::floor(elapsed / juce::jmax(1.0e-9, intervalBeatsNow));
                nextTarget = activeSceneStartPpq + ((completedCycles + 1.0) * intervalBeatsNow);
            }
            pendingSceneRecorderAction.targetPpq = nextTarget;
        }
        else
        {
            double nextBoundary = std::ceil(currentPpq / intervalBeatsNow) * intervalBeatsNow;
            if (nextBoundary <= currentPpq + 1.0e-9)
                nextBoundary += intervalBeatsNow;
            pendingSceneRecorderAction.targetPpq = std::round(nextBoundary / intervalBeatsNow) * intervalBeatsNow;
        }
        pendingSceneRecorderAction.targetResolved = true;
    }

    const double ppqPerSecond = *bpmOpt / 60.0;
    const double ppqPerSample = ppqPerSecond / currentSampleRate;
    const double blockEndPpq = currentPpq + (ppqPerSample * static_cast<double>(juce::jmax(1, numSamples)));
    if (blockEndPpq + 1.0e-9 < pendingSceneRecorderAction.targetPpq)
        return;

    const double targetPpq = pendingSceneRecorderAction.targetPpq;
    const double samplesToTarget = (targetPpq - currentPpq) / juce::jmax(1.0e-12, ppqPerSample);
    const int sampleOffset = juce::jlimit(0,
                                          juce::jmax(0, numSamples - 1),
                                          static_cast<int>(std::llround(samplesToTarget)));
    const int64_t targetGlobalSample = audioEngine->getGlobalSampleCount() + static_cast<int64_t>(sampleOffset);

    pendingSceneRecorderApplyAction.store(static_cast<int>(pendingSceneRecorderAction.action), std::memory_order_release);
    pendingSceneRecorderApplySceneSlot.store(sceneSlot, std::memory_order_release);
    pendingSceneRecorderApplyTargetPpq.store(targetPpq, std::memory_order_release);
    pendingSceneRecorderApplyTargetSample.store(targetGlobalSample, std::memory_order_release);
    pendingSceneRecorderAction.active = false;
    pendingSceneRecorderAction.targetResolved = false;
}

void MlrVSTAudioProcessor::processPendingSceneRecorderApply()
{
    const auto action = static_cast<SceneRecorderAction>(
        pendingSceneRecorderApplyAction.exchange(static_cast<int>(SceneRecorderAction::None), std::memory_order_acq_rel));
    if (action == SceneRecorderAction::None || audioEngine == nullptr || !isSceneModeEnabled())
        return;

    const int queuedSceneSlot = pendingSceneRecorderApplySceneSlot.exchange(-1, std::memory_order_acq_rel);
    const double queuedTargetPpq = pendingSceneRecorderApplyTargetPpq.exchange(-1.0, std::memory_order_acq_rel);
    const int64_t queuedTargetSample = pendingSceneRecorderApplyTargetSample.exchange(-1, std::memory_order_acq_rel);

    auto requeuePendingApply = [&](SceneRecorderAction requeueAction)
    {
        pendingSceneRecorderApplyAction.store(static_cast<int>(requeueAction), std::memory_order_release);
        pendingSceneRecorderApplySceneSlot.store(queuedSceneSlot, std::memory_order_release);
        pendingSceneRecorderApplyTargetPpq.store(queuedTargetPpq, std::memory_order_release);
        pendingSceneRecorderApplyTargetSample.store(queuedTargetSample, std::memory_order_release);
    };

    const int64_t currentGlobalSample = audioEngine->getGlobalSampleCount();
    if (queuedTargetSample >= 0
        && currentGlobalSample >= 0
        && isHostTransportPlaying()
        && currentGlobalSample + 1 < queuedTargetSample)
    {
        requeuePendingApply(action);
        return;
    }

    const int sceneSlot = juce::jlimit(0, SceneSlots - 1, queuedSceneSlot >= 0 ? queuedSceneSlot : activeSceneSlot);
    if (sceneSlot != juce::jlimit(0, SceneSlots - 1, activeSceneSlot))
        return;

    const double currentBeat = (std::isfinite(queuedTargetPpq) && queuedTargetPpq >= 0.0)
        ? queuedTargetPpq
        : audioEngine->getTimelineBeat();
    const double sceneStartBeat = activeSceneStartPpqValid && std::isfinite(activeSceneStartPpq)
        ? activeSceneStartPpq
        : currentBeat;

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
}

void MlrVSTAudioProcessor::clearPendingSceneRecorderAction()
{
    pendingSceneRecorderAction = {};
    pendingSceneRecorderApplyAction.store(static_cast<int>(SceneRecorderAction::None), std::memory_order_release);
    pendingSceneRecorderApplySceneSlot.store(-1, std::memory_order_release);
    pendingSceneRecorderApplyTargetPpq.store(-1.0, std::memory_order_release);
    pendingSceneRecorderApplyTargetSample.store(-1, std::memory_order_release);
}

void MlrVSTAudioProcessor::clearPendingSceneTriggerRecord(int stripIndex)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;

    pendingSceneTriggerRecords[static_cast<size_t>(stripIndex)] = {};
}

void MlrVSTAudioProcessor::rememberPendingSceneTriggerRecord(int stripIndex,
                                                             int column,
                                                             double eventBeat,
                                                             bool scratchGesture)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;

    auto& pending = pendingSceneTriggerRecords[static_cast<size_t>(stripIndex)];
    pending.active = std::isfinite(eventBeat);
    pending.scratchGesture = scratchGesture;
    pending.sceneSlot = juce::jlimit(0, SceneSlots - 1, activeSceneSlot);
    pending.column = juce::jlimit(0, MaxColumns - 1, column);
    pending.eventBeat = eventBeat;
}

void MlrVSTAudioProcessor::cancelPendingSceneTriggerRecord(int stripIndex)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips || audioEngine == nullptr)
        return;

    auto& pending = pendingSceneTriggerRecords[static_cast<size_t>(stripIndex)];
    if (pending.active
        && isSceneModeEnabled()
        && scenePerformanceRecorder.isRecording()
        && audioEngine->hasPendingTrigger(stripIndex))
    {
        scenePerformanceRecorder.cancelTriggerEvent(pending.sceneSlot,
                                                    stripIndex,
                                                    pending.column,
                                                    true,
                                                    pending.eventBeat);
    }

    pending = {};
}

void MlrVSTAudioProcessor::downgradePendingSceneScratchTriggerRecord(int stripIndex)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips || audioEngine == nullptr)
        return;

    auto& pending = pendingSceneTriggerRecords[static_cast<size_t>(stripIndex)];
    if (!(pending.active
          && pending.scratchGesture
          && isSceneModeEnabled()
          && scenePerformanceRecorder.isRecording()
          && audioEngine->hasPendingTrigger(stripIndex)))
    {
        return;
    }

    if (!scenePerformanceRecorder.cancelTriggerEvent(pending.sceneSlot,
                                                     stripIndex,
                                                     pending.column,
                                                     true,
                                                     pending.eventBeat))
    {
        return;
    }

    scenePerformanceRecorder.recordTriggerEvent(pending.sceneSlot,
                                                stripIndex,
                                                pending.column,
                                                true,
                                                pending.eventBeat,
                                                -1,
                                                -1,
                                                false);
    pending.scratchGesture = false;
}

void MlrVSTAudioProcessor::captureSceneTriggerRelease(int stripIndex, int columnHint)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips || audioEngine == nullptr)
        return;

    auto* strip = audioEngine->getStrip(stripIndex);
    if (strip == nullptr)
    {
        clearPendingSceneTriggerRecord(stripIndex);
        return;
    }

    cancelPendingSceneTriggerRecord(stripIndex);

    if (!isSceneModeEnabled() || !scenePerformanceRecorder.isRecording() || !strip->isPlaying())
        return;

    int releaseColumn = columnHint;
    if (releaseColumn < 0 || releaseColumn >= MaxColumns)
        releaseColumn = strip->getHeldButton();
    if (releaseColumn < 0 || releaseColumn >= MaxColumns)
        releaseColumn = 0;

    recordSceneTriggerEvent(stripIndex,
                            releaseColumn,
                            audioEngine->getTimelineBeat(),
                            -1,
                            -1,
                            false);
}

void MlrVSTAudioProcessor::recordSceneTriggerEvent(int stripIndex,
                                                   int column,
                                                   double eventBeat,
                                                   int sampleSliceId,
                                                   int64_t sampleStartSample,
                                                   bool noteOn,
                                                   bool scratchGesture)
{
    if (!isSceneModeEnabled() || !scenePerformanceRecorder.isRecording())
        return;

    const int sceneSlot = juce::jlimit(0, SceneSlots - 1, activeSceneSlot);
    scenePerformanceRecorder.recordTriggerEvent(sceneSlot,
                                                juce::jlimit(0, MaxStrips - 1, stripIndex),
                                                juce::jlimit(0, MaxColumns - 1, column),
                                                noteOn,
                                                eventBeat,
                                                sampleSliceId,
                                                sampleStartSample,
                                                scratchGesture);
}

void MlrVSTAudioProcessor::recordSceneGlobalStutterEvent(float normalizedValue, int columnHint, double eventBeat)
{
    if (!isSceneModeEnabled() || !scenePerformanceRecorder.isRecording())
        return;

    int sceneSlot = scenePerformanceRecorder.getRecordingSceneSlot();
    if (sceneSlot < 0 || sceneSlot >= SceneSlots)
        sceneSlot = juce::jlimit(0, SceneSlots - 1, activeSceneSlot);

    double safeBeat = eventBeat;
    if (!std::isfinite(safeBeat))
        safeBeat = (audioEngine != nullptr) ? audioEngine->getTimelineBeat() : 0.0;

    scenePerformanceRecorder.recordControlEvent(sceneSlot,
                                                -1,
                                                static_cast<int>(ControlMode::Normal),
                                                2,
                                                juce::jlimit(-1, MaxColumns - 1, columnHint),
                                                ScenePerformanceControlTarget::Retrigger,
                                                juce::jlimit(0.0f, 1.0f, normalizedValue),
                                                safeBeat);
    clearActiveSceneAutomationOverrideForRecordedTarget(sceneSlot,
                                                        -1,
                                                        ScenePerformanceControlTarget::Retrigger);
}

void MlrVSTAudioProcessor::recordMomentaryStutterSceneDivision(double divisionBeats,
                                                               int columnHint,
                                                               double eventBeat)
{
    recordSceneGlobalStutterEvent(sceneStutterAmountFromDivisionBeats(divisionBeats), columnHint, eventBeat);
}

void MlrVSTAudioProcessor::applyScenePerformanceEvent(const ScenePerformanceEvent& event,
                                                      StripControlWriteMode writeMode)
{
    if (audioEngine == nullptr)
        return;

    if (event.type == ScenePerformanceEventType::ControlPoint
        && event.controlTarget == ScenePerformanceControlTarget::Retrigger)
    {
        setGlobalSceneStutterAmount(event.value);
        return;
    }

    const int stripIndex = juce::jlimit(0, MaxStrips - 1, event.stripIndex);
    auto* strip = audioEngine->getStrip(stripIndex);
    if (strip == nullptr)
        return;

    if (event.type == ScenePerformanceEventType::Trigger)
    {
        if (event.isNoteOn)
        {
            audioEngine->triggerStripWithQuantization(stripIndex,
                                                      juce::jlimit(0, MaxColumns - 1, event.column),
                                                      false,
                                                      event.sampleSliceId,
                                                      event.sampleStartSample);
        }
        else if (strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Sample)
        {
            stopSampleModeStrip(stripIndex, false);
        }
        else if (event.scratchGesture)
        {
            strip->onButtonRelease(juce::jlimit(0, MaxColumns - 1, event.column),
                                   audioEngine->getGlobalSampleCount());
        }
        else
        {
            strip->stop(false);
        }
        return;
    }

    switch (event.controlTarget)
    {
        case ScenePerformanceControlTarget::Speed:
            setStripSpeedControlValue(stripIndex, event.value, writeMode);
            break;
        case ScenePerformanceControlTarget::Pitch:
            if (strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Grain)
            {
                strip->setGrainPitch(event.value);
            }
            else if (writeMode == StripControlWriteMode::NotifyHost)
            {
                applyUserPitchControlToStrip(stripIndex, event.value);
            }
            else
            {
                const float quantizedSemitones = quantizePitchSemitonesForStripControl(stripIndex, event.value);
                if (auto* rawParam = stripPitchParams[static_cast<size_t>(stripIndex)])
                    rawParam->store(quantizedSemitones, std::memory_order_release);

                const auto loopPitchRole = getLoopPitchRole(stripIndex);
                if (loopPitchRole == LoopPitchRole::Sync)
                    requestLoopPitchRoleStateUpdate(stripIndex);
                else
                    applyPitchControlToStrip(stripIndex, *strip, quantizedSemitones);

                if (loopPitchRole == LoopPitchRole::Master)
                    updateGlobalRootFromLoopPitchMaster(stripIndex, true);
            }
            break;
        case ScenePerformanceControlTarget::Pan:
            setStripPanControlValue(stripIndex, event.value, writeMode);
            break;
        case ScenePerformanceControlTarget::Volume:
            setStripVolumeControlValue(stripIndex, event.value, writeMode);
            break;
        case ScenePerformanceControlTarget::Swing:
            strip->setSwingAmount(juce::jlimit(0.0f, 1.0f, event.value));
            break;
        case ScenePerformanceControlTarget::GrainSize:
            strip->setGrainSizeMs(event.value);
            break;
        case ScenePerformanceControlTarget::GrainDensity:
            strip->setGrainDensity(event.value);
            break;
        case ScenePerformanceControlTarget::GrainPitch:
            strip->setGrainPitch(event.value);
            break;
        case ScenePerformanceControlTarget::GrainPitchJitter:
            strip->setGrainPitchJitter(juce::jlimit(0.0f, 48.0f, event.value));
            break;
        case ScenePerformanceControlTarget::GrainSpread:
            strip->setGrainSpread(juce::jlimit(0.0f, 1.0f, event.value));
            break;
        case ScenePerformanceControlTarget::GrainJitter:
            strip->setGrainJitter(juce::jlimit(0.0f, 1.0f, event.value));
            break;
        case ScenePerformanceControlTarget::GrainPositionJitter:
            strip->setGrainPositionJitter(juce::jlimit(0.0f, 1.0f, event.value));
            break;
        case ScenePerformanceControlTarget::GrainRandomDepth:
            strip->setGrainRandomDepth(juce::jlimit(0.0f, 1.0f, event.value));
            break;
        case ScenePerformanceControlTarget::GrainArp:
            strip->setGrainArpDepth(juce::jlimit(0.0f, 1.0f, event.value));
            break;
        case ScenePerformanceControlTarget::GrainCloud:
            strip->setGrainCloudDepth(juce::jlimit(0.0f, 1.0f, event.value));
            break;
        case ScenePerformanceControlTarget::GrainEmitter:
            strip->setGrainEmitterDepth(juce::jlimit(0.0f, 1.0f, event.value));
            break;
        case ScenePerformanceControlTarget::GrainEnvelope:
            strip->setGrainEnvelope(juce::jlimit(0.0f, 1.0f, event.value));
            break;
        case ScenePerformanceControlTarget::GrainShape:
            strip->setGrainShape(juce::jlimit(-1.0f, 1.0f, event.value));
            break;
        case ScenePerformanceControlTarget::FilterFrequency:
            setStripFilterFrequencyControlValue(stripIndex, event.value, writeMode);
            break;
        case ScenePerformanceControlTarget::FilterResonance:
            setStripFilterResonanceControlValue(stripIndex, event.value, writeMode);
            break;
        case ScenePerformanceControlTarget::FilterEnabled:
            setStripFilterEnabledControlValue(stripIndex,
                                              event.value >= 0.5f,
                                              writeMode);
            break;
        case ScenePerformanceControlTarget::FilterMorph:
            setStripFilterMorphControlValue(stripIndex, event.value, writeMode);
            break;
        case ScenePerformanceControlTarget::SliceLength:
            writeStripFloatParameter("stripSliceLength" + juce::String(stripIndex),
                                     juce::jlimit(0.02f, 1.0f, event.value),
                                     stripSliceLengthParams[static_cast<size_t>(stripIndex)],
                                     writeMode);
            strip->setLoopSliceLength(event.value);
            break;
        case ScenePerformanceControlTarget::Scratch:
            strip->setScratchAmount(juce::jlimit(0.0f, 100.0f, event.value));
            break;
        case ScenePerformanceControlTarget::DelayMix:
            setStripDelayMixControlValue(stripIndex, event.value, writeMode);
            break;
        case ScenePerformanceControlTarget::DelayTime:
            setStripDelayTimeControlValue(stripIndex, event.value, writeMode);
            break;
        case ScenePerformanceControlTarget::DelayFeedback:
            setStripDelayFeedbackControlValue(stripIndex, event.value, writeMode);
            break;
        case ScenePerformanceControlTarget::DelayLowCut:
            setStripDelayLowCutControlValue(stripIndex, event.value, writeMode);
            break;
        case ScenePerformanceControlTarget::DelayHighCut:
            setStripDelayHighCutControlValue(stripIndex, event.value, writeMode);
            break;
        case ScenePerformanceControlTarget::DelayMode:
            setStripDelayModeControlValue(stripIndex,
                                          static_cast<EnhancedAudioStrip::DelayMode>(
                                              juce::jlimit(0, 2, static_cast<int>(std::round(event.value)))),
                                          writeMode);
            break;
        case ScenePerformanceControlTarget::DelaySyncEnabled:
            setStripDelaySyncEnabledControlValue(stripIndex,
                                                 event.value >= 0.5f,
                                                 writeMode);
            break;
        case ScenePerformanceControlTarget::Retrigger:
            audioEngine->setMacroRetriggerAmount(stripIndex, juce::jlimit(0.0f, 1.0f, event.value));
            break;
        case ScenePerformanceControlTarget::Rearrange:
            break;
        case ScenePerformanceControlTarget::None:
        default:
            break;
    }
}

void MlrVSTAudioProcessor::playbackScenePerformanceEvent(const ScenePerformanceEvent& event)
{
    if (event.stripIndex >= 0 && !isStripScenePlaybackAvailable(event.stripIndex))
        return;

    float liveValue = 0.0f;
    const bool shouldSeedTransition = event.type == ScenePerformanceEventType::ControlPoint
        && shouldSmoothLiveSceneControlTarget(event.controlTarget)
        && getSceneControlCurrentValue(event.stripIndex, event.controlTarget, liveValue);

    applyScenePerformanceEvent(event, StripControlWriteMode::CacheOnly);

    if (shouldSeedTransition)
    {
        seedSceneControlTransition(event.stripIndex,
                                   event.controlTarget,
                                   liveValue,
                                   event.value,
                                   getSceneAutomationTransitionSeconds(event.controlTarget));
    }
}

void MlrVSTAudioProcessor::applySceneHeldAutomationStateAtBeat(int sceneSlot,
                                                               double currentBeat,
                                                               double sceneStartBeat)
{
    if (!isSceneModeEnabled()
        || audioEngine == nullptr
        || !std::isfinite(currentBeat)
        || !std::isfinite(sceneStartBeat))
    {
        return;
    }

    const int safeSceneSlot = juce::jlimit(0, SceneSlots - 1, sceneSlot);
    const double lengthBeats = juce::jmax(1.0, getScenePerformanceClipLengthBeats(safeSceneSlot));
    const double clipBeat = wrapBeatIntoSceneClip(currentBeat, sceneStartBeat, lengthBeats);
    const auto events = getScenePerformanceEventsSnapshot(safeSceneSlot);
    if (events.empty())
        return;

    beginSceneAutosaveSuppression();
    beginSceneManualControlHandlingSuppression();
    {
        for (int maskIndex = 0; maskIndex < static_cast<int>(activeSceneAutomationOverrideMasks.size()); ++maskIndex)
        {
            const int targetStripIndex = (maskIndex == kSceneAutomationGlobalMaskIndex) ? -1 : maskIndex;
            for (int targetValue = static_cast<int>(ScenePerformanceControlTarget::Speed);
                 targetValue <= static_cast<int>(ScenePerformanceControlTarget::GrainShape);
                 ++targetValue)
            {
                const auto target = static_cast<ScenePerformanceControlTarget>(targetValue);
                if (!sceneClipHasAutomationTarget(safeSceneSlot, targetStripIndex, target)
                    || isActiveSceneAutomationOverridden(targetStripIndex, target))
                {
                    continue;
                }

                ScenePerformanceEvent chosenEvent;
                if (findHeldSceneAutomationEventAtClipBeat(events,
                                                           targetStripIndex,
                                                           target,
                                                           clipBeat,
                                                           chosenEvent))
                {
                    playbackScenePerformanceEvent(chosenEvent);
                }
            }
        }
    }
    endSceneManualControlHandlingSuppression();
    endSceneAutosaveSuppression();
}

void MlrVSTAudioProcessor::processScenePerformancePlayback(const juce::AudioPlayHead::PositionInfo& posInfo,
                                                           int numSamples)
{
    if (!isSceneModeEnabled() || audioEngine == nullptr)
    {
        scenePlaybackBlockStutterPostRenderPending = false;
        lastScenePerformanceProcessBeat = std::numeric_limits<double>::quiet_NaN();
        lastScenePerformanceProcessSceneSlot = -1;
        lastScenePerformanceProcessSceneStartBeat = std::numeric_limits<double>::quiet_NaN();
        return;
    }

    const int sceneSlot = juce::jlimit(0, SceneSlots - 1, activeSceneSlot);
    if (!activeSceneStartPpqValid || !std::isfinite(activeSceneStartPpq))
    {
        if (!ensureActiveScenePlaybackHandleInitialized()
            || !activeSceneStartPpqValid
            || !std::isfinite(activeSceneStartPpq))
        {
            scenePlaybackBlockStutterPostRenderPending = false;
            lastScenePerformanceProcessBeat = std::numeric_limits<double>::quiet_NaN();
            lastScenePerformanceProcessSceneSlot = sceneSlot;
            lastScenePerformanceProcessSceneStartBeat = activeSceneStartPpq;
            return;
        }
    }

    if (!posInfo.getIsPlaying())
    {
        scenePlaybackBlockStutterPostRenderPending = false;
        scenePlaybackBlockStutterPostRenderAmount = 0.0f;
        setGlobalSceneStutterAmount(0.0f);
        lastScenePerformanceProcessBeat = std::numeric_limits<double>::quiet_NaN();
        lastScenePerformanceProcessSceneSlot = sceneSlot;
        lastScenePerformanceProcessSceneStartBeat = activeSceneStartPpq;
        return;
    }

    const double blockStartBeat = posInfo.getPpqPosition().hasValue()
        ? *posInfo.getPpqPosition()
        : audioEngine->getTimelineBeat();
    const int64_t blockStartSample = audioEngine->getGlobalSampleCount();
    const double bpm = posInfo.getBpm().hasValue()
        ? juce::jmax(1.0, *posInfo.getBpm())
        : juce::jmax(1.0, audioEngine->getCurrentTempo());
    const double beatDeltaPerSample = bpm / (60.0 * juce::jmax(1.0, currentSampleRate));
    const double samplesPerBeat = 1.0 / beatDeltaPerSample;
    const double blockEndBeat = blockStartBeat + (beatDeltaPerSample * static_cast<double>(juce::jmax(0, numSamples)));

    scenePerformanceRecorder.updateRecording(blockEndBeat);

    const bool sceneChanged = sceneSlot != lastScenePerformanceProcessSceneSlot
        || !std::isfinite(lastScenePerformanceProcessSceneStartBeat)
        || std::abs(lastScenePerformanceProcessSceneStartBeat - activeSceneStartPpq) > 1.0e-9;
    const bool sceneHasGlobalRetriggerLane =
        sceneClipHasAutomationTarget(sceneSlot, -1, ScenePerformanceControlTarget::Retrigger);

    if (!scenePerformanceRecorder.hasEvents(sceneSlot))
    {
        scenePlaybackBlockStutterPostRenderPending = false;
        if (sceneChanged)
        {
            scenePlaybackBlockStutterPostRenderAmount = 0.0f;
            setGlobalSceneStutterAmount(0.0f);
        }
        lastScenePerformanceProcessBeat = blockEndBeat;
        lastScenePerformanceProcessSceneSlot = sceneSlot;
        lastScenePerformanceProcessSceneStartBeat = activeSceneStartPpq;
        return;
    }

    double fromBeat = sceneChanged ? juce::jmax(blockStartBeat, activeSceneStartPpq) : lastScenePerformanceProcessBeat;
    if (!std::isfinite(fromBeat))
        fromBeat = blockStartBeat;

    if (fromBeat < blockStartBeat - 1.0 || fromBeat > blockEndBeat + 1.0)
        fromBeat = blockStartBeat;

    if (sceneChanged)
    {
        scenePlaybackBlockStutterPostRenderPending = false;
        scenePlaybackBlockStutterPostRenderAmount = 0.0f;
        if (!sceneHasGlobalRetriggerLane)
            setGlobalSceneStutterAmount(0.0f);
        applySceneHeldAutomationStateAtBeat(sceneSlot, blockStartBeat, activeSceneStartPpq);
    }

    const double clipLengthBeats = juce::jmax(1.0, scenePerformanceRecorder.getClipLengthBeats(sceneSlot));
    const float stutterAmountAtBlockStart = getGlobalSceneStutterAmount();
    bool sawRetriggerEventThisBlock = false;
    float retriggerRenderAmount = stutterAmountAtBlockStart;
    float retriggerFinalAmount = stutterAmountAtBlockStart;
    const double beatDelta = blockEndBeat - fromBeat;
    if (beatDelta > 0.0 && beatDelta <= clipLengthBeats)
    {
        scenePerformanceRecorder.processEventsForBeatWindow(sceneSlot,
                                                            activeSceneStartPpq,
                                                            fromBeat,
                                                            blockEndBeat,
                                                            [this,
                                                             blockStartBeat,
                                                             blockStartSample,
                                                             numSamples,
                                                             samplesPerBeat,
                                                             &sawRetriggerEventThisBlock,
                                                             &retriggerRenderAmount,
                                                             &retriggerFinalAmount]
                                                            (const ScenePerformanceEvent& event, double occurrenceBeat)
                                                            {
                                                                if (event.stripIndex >= 0
                                                                    && !isStripScenePlaybackAvailable(event.stripIndex))
                                                                {
                                                                    return;
                                                                }

                                                                if (event.type == ScenePerformanceEventType::ControlPoint
                                                                    && isActiveSceneAutomationOverridden(event.stripIndex,
                                                                                                         event.controlTarget))
                                                                {
                                                                    return;
                                                                }

                                                                if (event.type == ScenePerformanceEventType::ControlPoint
                                                                    && event.controlTarget
                                                                        == ScenePerformanceControlTarget::Retrigger)
                                                                {
                                                                    sawRetriggerEventThisBlock = true;
                                                                    const float clampedAmount = juce::jlimit(0.0f,
                                                                                                              1.0f,
                                                                                                              event.value);
                                                                    if (clampedAmount > 1.0e-4f)
                                                                        retriggerRenderAmount = clampedAmount;
                                                                    retriggerFinalAmount = clampedAmount;
                                                                    return;
                                                                }

                                                                if (event.type == ScenePerformanceEventType::Trigger
                                                                    && event.isNoteOn
                                                                    && std::isfinite(occurrenceBeat))
                                                                {
                                                                    const int maxOffset = juce::jmax(0, numSamples - 1);
                                                                    const int offsetSamples = juce::jlimit(
                                                                        0,
                                                                        maxOffset,
                                                                        static_cast<int>(std::llround(
                                                                            (occurrenceBeat - blockStartBeat) * samplesPerBeat)));
                                                                    if (event.scratchGesture)
                                                                    {
                                                                        if (auto* strip = audioEngine->getStrip(event.stripIndex))
                                                                        {
                                                                            if (strip->getPlayMode() != EnhancedAudioStrip::PlayMode::Sample)
                                                                            {
                                                                                strip->onButtonPress(juce::jlimit(0, MaxColumns - 1, event.column),
                                                                                                     blockStartSample + offsetSamples);
                                                                            }
                                                                        }
                                                                    }
                                                                    audioEngine->queueExactTrigger(event.stripIndex,
                                                                                                  juce::jlimit(0, MaxColumns - 1, event.column),
                                                                                                  occurrenceBeat,
                                                                                                  blockStartSample + offsetSamples,
                                                                                                  event.sampleSliceId,
                                                                                                  event.sampleStartSample);
                                                                    return;
                                                                }

                                                                playbackScenePerformanceEvent(event);
                                                            });
    }

    if (sawRetriggerEventThisBlock)
    {
        setGlobalSceneStutterAmount(retriggerRenderAmount);
        scenePlaybackBlockStutterPostRenderPending = true;
        scenePlaybackBlockStutterPostRenderAmount = retriggerFinalAmount;
    }
    else
    {
        scenePlaybackBlockStutterPostRenderPending = false;
    }

    lastScenePerformanceProcessBeat = blockEndBeat;
    lastScenePerformanceProcessSceneSlot = sceneSlot;
    lastScenePerformanceProcessSceneStartBeat = activeSceneStartPpq;
}
