/*
  ==============================================================================

    PluginProcessorSceneAutomation.cpp
    Extracted live scene automation transition and override handling for mlrVST.

  ==============================================================================
*/

#include "PluginProcessor.h"
#include "SceneAutomationRules.h"
#include <cmath>

namespace
{
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

bool sceneControlSupportsTransitionSmoothing(ScenePerformanceControlTarget target) noexcept
{
    switch (target)
    {
        case ScenePerformanceControlTarget::Volume:
        case ScenePerformanceControlTarget::Pan:
        case ScenePerformanceControlTarget::Pitch:
        case ScenePerformanceControlTarget::FilterFrequency:
        case ScenePerformanceControlTarget::FilterResonance:
            return true;
        case ScenePerformanceControlTarget::None:
        case ScenePerformanceControlTarget::Speed:
        case ScenePerformanceControlTarget::Swing:
        case ScenePerformanceControlTarget::GrainSize:
        case ScenePerformanceControlTarget::GrainDensity:
        case ScenePerformanceControlTarget::GrainPitch:
        case ScenePerformanceControlTarget::GrainPitchJitter:
        case ScenePerformanceControlTarget::GrainSpread:
        case ScenePerformanceControlTarget::GrainJitter:
        case ScenePerformanceControlTarget::GrainPositionJitter:
        case ScenePerformanceControlTarget::GrainRandomDepth:
        case ScenePerformanceControlTarget::GrainArp:
        case ScenePerformanceControlTarget::GrainCloud:
        case ScenePerformanceControlTarget::GrainEmitter:
        case ScenePerformanceControlTarget::GrainEnvelope:
        case ScenePerformanceControlTarget::GrainShape:
        case ScenePerformanceControlTarget::FilterEnabled:
        case ScenePerformanceControlTarget::FilterMorph:
        case ScenePerformanceControlTarget::SliceLength:
        case ScenePerformanceControlTarget::Scratch:
        case ScenePerformanceControlTarget::DelayMix:
        case ScenePerformanceControlTarget::DelayTime:
        case ScenePerformanceControlTarget::DelayFeedback:
        case ScenePerformanceControlTarget::DelayLowCut:
        case ScenePerformanceControlTarget::DelayHighCut:
        case ScenePerformanceControlTarget::DelayMode:
        case ScenePerformanceControlTarget::DelaySyncEnabled:
        case ScenePerformanceControlTarget::Retrigger:
        case ScenePerformanceControlTarget::Rearrange:
        default:
            return false;
    }
}

double wrapBeatIntoSceneClip(double currentBeat, double sceneStartBeat, double lengthBeats) noexcept
{
    const double safeLength = (std::isfinite(lengthBeats) && lengthBeats > 1.0e-6)
        ? lengthBeats
        : 0.0;
    if (!std::isfinite(currentBeat) || !std::isfinite(sceneStartBeat) || safeLength <= 0.0)
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
} // namespace

double MlrVSTAudioProcessor::getSceneAutomationTransitionSeconds() const
{
    return 0.05;
}

double MlrVSTAudioProcessor::getSceneAutomationTransitionSeconds(ScenePerformanceControlTarget target) const
{
    switch (target)
    {
        case ScenePerformanceControlTarget::Volume:
        case ScenePerformanceControlTarget::Pan:
            return 0.08;
        case ScenePerformanceControlTarget::FilterFrequency:
        case ScenePerformanceControlTarget::FilterResonance:
            return 0.12;
        case ScenePerformanceControlTarget::None:
        case ScenePerformanceControlTarget::Speed:
        case ScenePerformanceControlTarget::Pitch:
        case ScenePerformanceControlTarget::Swing:
        case ScenePerformanceControlTarget::GrainSize:
        case ScenePerformanceControlTarget::GrainDensity:
        case ScenePerformanceControlTarget::GrainPitch:
        case ScenePerformanceControlTarget::GrainPitchJitter:
        case ScenePerformanceControlTarget::GrainSpread:
        case ScenePerformanceControlTarget::GrainJitter:
        case ScenePerformanceControlTarget::GrainPositionJitter:
        case ScenePerformanceControlTarget::GrainRandomDepth:
        case ScenePerformanceControlTarget::GrainArp:
        case ScenePerformanceControlTarget::GrainCloud:
        case ScenePerformanceControlTarget::GrainEmitter:
        case ScenePerformanceControlTarget::GrainEnvelope:
        case ScenePerformanceControlTarget::GrainShape:
        case ScenePerformanceControlTarget::FilterEnabled:
        case ScenePerformanceControlTarget::FilterMorph:
        case ScenePerformanceControlTarget::SliceLength:
        case ScenePerformanceControlTarget::Scratch:
        case ScenePerformanceControlTarget::DelayMix:
        case ScenePerformanceControlTarget::DelayTime:
        case ScenePerformanceControlTarget::DelayFeedback:
        case ScenePerformanceControlTarget::DelayLowCut:
        case ScenePerformanceControlTarget::DelayHighCut:
        case ScenePerformanceControlTarget::DelayMode:
        case ScenePerformanceControlTarget::DelaySyncEnabled:
        case ScenePerformanceControlTarget::Retrigger:
        case ScenePerformanceControlTarget::Rearrange:
        default:
            return getSceneAutomationTransitionSeconds();
    }
}

bool MlrVSTAudioProcessor::shouldSmoothLiveSceneControlTarget(ScenePerformanceControlTarget target) const
{
    switch (target)
    {
        case ScenePerformanceControlTarget::Volume:
        case ScenePerformanceControlTarget::Pan:
        case ScenePerformanceControlTarget::FilterFrequency:
        case ScenePerformanceControlTarget::FilterResonance:
            return true;
        case ScenePerformanceControlTarget::None:
        case ScenePerformanceControlTarget::Speed:
        case ScenePerformanceControlTarget::Pitch:
        case ScenePerformanceControlTarget::Swing:
        case ScenePerformanceControlTarget::GrainSize:
        case ScenePerformanceControlTarget::GrainDensity:
        case ScenePerformanceControlTarget::GrainPitch:
        case ScenePerformanceControlTarget::GrainPitchJitter:
        case ScenePerformanceControlTarget::GrainSpread:
        case ScenePerformanceControlTarget::GrainJitter:
        case ScenePerformanceControlTarget::GrainPositionJitter:
        case ScenePerformanceControlTarget::GrainRandomDepth:
        case ScenePerformanceControlTarget::GrainArp:
        case ScenePerformanceControlTarget::GrainCloud:
        case ScenePerformanceControlTarget::GrainEmitter:
        case ScenePerformanceControlTarget::GrainEnvelope:
        case ScenePerformanceControlTarget::GrainShape:
        case ScenePerformanceControlTarget::FilterEnabled:
        case ScenePerformanceControlTarget::FilterMorph:
        case ScenePerformanceControlTarget::SliceLength:
        case ScenePerformanceControlTarget::Scratch:
        case ScenePerformanceControlTarget::DelayMix:
        case ScenePerformanceControlTarget::DelayTime:
        case ScenePerformanceControlTarget::DelayFeedback:
        case ScenePerformanceControlTarget::DelayLowCut:
        case ScenePerformanceControlTarget::DelayHighCut:
        case ScenePerformanceControlTarget::DelayMode:
        case ScenePerformanceControlTarget::DelaySyncEnabled:
        case ScenePerformanceControlTarget::Retrigger:
        case ScenePerformanceControlTarget::Rearrange:
        default:
            return false;
    }
}

void MlrVSTAudioProcessor::seedSceneControlTransition(int stripIndex,
                                                      ScenePerformanceControlTarget target,
                                                      float fromValue,
                                                      float toValue,
                                                      double rampSeconds)
{
    if (!sceneControlSupportsTransitionSmoothing(target)
        || audioEngine == nullptr
        || stripIndex < 0
        || stripIndex >= MaxStrips)
    {
        return;
    }

    if (std::abs(fromValue - toValue) <= 1.0e-4f)
        return;

    auto* strip = audioEngine->getStrip(stripIndex);
    if (strip == nullptr)
        return;

    switch (target)
    {
        case ScenePerformanceControlTarget::None:
        case ScenePerformanceControlTarget::Speed:
        case ScenePerformanceControlTarget::Swing:
        case ScenePerformanceControlTarget::FilterEnabled:
        case ScenePerformanceControlTarget::SliceLength:
        case ScenePerformanceControlTarget::Scratch:
        case ScenePerformanceControlTarget::DelayMode:
        case ScenePerformanceControlTarget::DelaySyncEnabled:
        case ScenePerformanceControlTarget::Retrigger:
        case ScenePerformanceControlTarget::Rearrange:
            break;
        case ScenePerformanceControlTarget::Volume:
            strip->seedVolumeTransition(fromValue, toValue, rampSeconds);
            if (auto* stepSampler = strip->getStepSampler())
                stepSampler->setVolume(juce::jlimit(0.0f, 1.0f, toValue));
            break;
        case ScenePerformanceControlTarget::Pan:
            strip->seedPanTransition(fromValue, toValue, rampSeconds);
            if (auto* stepSampler = strip->getStepSampler())
                stepSampler->setPan(juce::jlimit(-1.0f, 1.0f, toValue));
            break;
        case ScenePerformanceControlTarget::Pitch:
            if (strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Grain)
                strip->seedGrainPitchTransition(fromValue, toValue, rampSeconds);
            else
                strip->seedPitchShiftTransition(fromValue, toValue, rampSeconds);
            break;
        case ScenePerformanceControlTarget::GrainSize:
            strip->seedGrainSizeTransition(fromValue, toValue, rampSeconds);
            break;
        case ScenePerformanceControlTarget::GrainDensity:
            strip->seedGrainDensityTransition(fromValue, toValue, rampSeconds);
            break;
        case ScenePerformanceControlTarget::GrainPitch:
            strip->seedGrainPitchTransition(fromValue, toValue, rampSeconds);
            break;
        case ScenePerformanceControlTarget::GrainPitchJitter:
            strip->seedGrainPitchJitterTransition(fromValue, toValue, rampSeconds);
            break;
        case ScenePerformanceControlTarget::GrainSpread:
            strip->seedGrainSpreadTransition(fromValue, toValue, rampSeconds);
            break;
        case ScenePerformanceControlTarget::GrainJitter:
            strip->seedGrainJitterTransition(fromValue, toValue, rampSeconds);
            break;
        case ScenePerformanceControlTarget::GrainPositionJitter:
            strip->seedGrainPositionJitterTransition(fromValue, toValue, rampSeconds);
            break;
        case ScenePerformanceControlTarget::GrainRandomDepth:
            strip->seedGrainRandomDepthTransition(fromValue, toValue, rampSeconds);
            break;
        case ScenePerformanceControlTarget::GrainArp:
            strip->seedGrainArpDepthTransition(fromValue, toValue, rampSeconds);
            break;
        case ScenePerformanceControlTarget::GrainCloud:
            strip->seedGrainCloudDepthTransition(fromValue, toValue, rampSeconds);
            break;
        case ScenePerformanceControlTarget::GrainEmitter:
            strip->seedGrainEmitterDepthTransition(fromValue, toValue, rampSeconds);
            break;
        case ScenePerformanceControlTarget::GrainEnvelope:
            strip->seedGrainEnvelopeTransition(fromValue, toValue, rampSeconds);
            break;
        case ScenePerformanceControlTarget::GrainShape:
            strip->seedGrainShapeTransition(fromValue, toValue, rampSeconds);
            break;
        case ScenePerformanceControlTarget::FilterFrequency:
            strip->seedFilterFrequencyTransition(fromValue, toValue, rampSeconds);
            if (auto* stepSampler = strip->getStepSampler())
            {
                stepSampler->setFilterEnabled(true);
                stepSampler->setFilterFrequency(juce::jlimit(20.0f, 20000.0f, toValue));
            }
            break;
        case ScenePerformanceControlTarget::FilterResonance:
            strip->seedFilterResonanceTransition(fromValue, toValue, rampSeconds);
            if (auto* stepSampler = strip->getStepSampler())
            {
                stepSampler->setFilterEnabled(true);
                stepSampler->setFilterResonance(juce::jlimit(0.1f, 10.0f, toValue));
            }
            break;
        case ScenePerformanceControlTarget::FilterMorph:
            strip->seedFilterMorphTransition(fromValue, toValue, rampSeconds);
            if (auto* stepSampler = strip->getStepSampler())
            {
                stepSampler->setFilterEnabled(true);
                auto stepType = ::FilterType::LowPass;
                if (toValue >= 0.75f)
                    stepType = ::FilterType::HighPass;
                else if (toValue >= 0.25f)
                    stepType = ::FilterType::BandPass;
                stepSampler->setFilterType(stepType);
            }
            break;
        case ScenePerformanceControlTarget::DelayMix:
            strip->seedDelayMixTransition(fromValue, toValue, rampSeconds);
            break;
        case ScenePerformanceControlTarget::DelayTime:
            strip->seedDelayTimeTransition(fromValue, toValue, rampSeconds);
            break;
        case ScenePerformanceControlTarget::DelayFeedback:
            strip->seedDelayFeedbackTransition(fromValue, toValue, rampSeconds);
            break;
        case ScenePerformanceControlTarget::DelayLowCut:
            strip->seedDelayLowCutTransition(fromValue, toValue, rampSeconds);
            break;
        case ScenePerformanceControlTarget::DelayHighCut:
            strip->seedDelayHighCutTransition(fromValue, toValue, rampSeconds);
            break;
    }
}

void MlrVSTAudioProcessor::clearActiveSceneAutomationOverrides(bool restoreWrittenValues)
{
    if (restoreWrittenValues && isSceneModeEnabled() && audioEngine != nullptr && hasAnyActiveSceneAutomationOverrides())
    {
        const int sceneSlot = juce::jlimit(0, SceneSlots - 1, activeSceneSlot);
        const double lengthBeats = juce::jmax(1.0, getScenePerformanceClipLengthBeats(sceneSlot));
        const double currentBeat = audioEngine->getTimelineBeat();
        const double clipBeat = (activeSceneStartPpqValid && std::isfinite(activeSceneStartPpq))
            ? wrapBeatIntoSceneClip(currentBeat, activeSceneStartPpq, lengthBeats)
            : 0.0;
        const auto events = getScenePerformanceEventsSnapshot(sceneSlot);

        beginSceneAutosaveSuppression();
        beginSceneManualControlHandlingSuppression();
        {
            for (int maskIndex = 0; maskIndex < static_cast<int>(activeSceneAutomationOverrideMasks.size()); ++maskIndex)
            {
                const uint64_t overrideMask = activeSceneAutomationOverrideMasks[static_cast<size_t>(maskIndex)]
                    .load(std::memory_order_acquire);
                if (overrideMask == 0)
                    continue;

                for (int targetValue = static_cast<int>(ScenePerformanceControlTarget::Speed);
                     targetValue <= static_cast<int>(ScenePerformanceControlTarget::GrainShape);
                     ++targetValue)
                {
                    const auto target = static_cast<ScenePerformanceControlTarget>(targetValue);
                    if ((overrideMask & sceneAutomationTargetBit(target)) == 0)
                        continue;

                    const int targetStripIndex = (maskIndex == MaxStrips) ? -1 : maskIndex;
                    ScenePerformanceEvent chosenEvent;
                    if (findHeldSceneAutomationEventAtClipBeat(events,
                                                               targetStripIndex,
                                                               target,
                                                               clipBeat,
                                                               chosenEvent))
                    {
                        float liveValue = 0.0f;
                        const bool isSteppedControlPoint = SceneAutomationRules::eventUsesSteppedSegments(chosenEvent);
                        const bool shouldSeedTransition = sceneControlSupportsTransitionSmoothing(target)
                            && !isSteppedControlPoint
                            && getSceneControlCurrentValue(targetStripIndex, target, liveValue);
                        applyScenePerformanceEvent(chosenEvent, StripControlWriteMode::NotifyHost);
                        if (shouldSeedTransition)
                        {
                            seedSceneControlTransition(targetStripIndex,
                                                       target,
                                                       liveValue,
                                                       chosenEvent.value,
                                                       getSceneAutomationTransitionSeconds(target));
                        }
                    }
                }
            }
        }
        endSceneManualControlHandlingSuppression();
        endSceneAutosaveSuppression();

        auto& runtimeState = sceneClipSlotRuntimeStates[static_cast<size_t>(sceneSlot)];
        runtimeState.liveStripControlsDirty = false;
        syncActiveSceneClipSlotRuntimeStateFromEngine(false);
        updateMonomeLEDs();
    }

    for (auto& mask : activeSceneAutomationOverrideMasks)
        mask.store(0, std::memory_order_release);
}

void MlrVSTAudioProcessor::clearActiveSceneAutomationOverrideForRecordedTarget(int sceneSlot,
                                                                               int stripIndex,
                                                                               ScenePerformanceControlTarget target)
{
    if (target == ScenePerformanceControlTarget::None)
        return;

    const int safeRecordedSceneSlot = juce::jlimit(0, SceneSlots - 1, sceneSlot);
    const int safeActiveSceneSlot = juce::jlimit(0, SceneSlots - 1, activeSceneSlot);
    if (safeRecordedSceneSlot != safeActiveSceneSlot)
        return;

    auto& overrideMask = activeSceneAutomationOverrideMasks[static_cast<size_t>(
        sceneAutomationMaskIndex(stripIndex, target))];
    overrideMask.fetch_and(~sceneAutomationTargetBit(target), std::memory_order_acq_rel);
}

void MlrVSTAudioProcessor::reenableActiveSceneAutomation()
{
    clearActiveSceneAutomationOverrides(true);
}
