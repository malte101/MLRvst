/*
  ==============================================================================

    PluginProcessorSceneClipData.cpp
    Extracted scene clip data access and clipboard handling for mlrVST.

  ==============================================================================
*/

#include "PluginProcessor.h"
#include "PlayheadSpeedQuantizer.h"
#include "SceneAutomationRules.h"
#include "SceneScheduler.h"
#include <cmath>
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

double wrapBeatIntoSceneClip(double currentBeat, double sceneStartBeat, double lengthBeats) noexcept
{
    return SceneAutomationRules::wrapBeatIntoClip(currentBeat, sceneStartBeat, lengthBeats);
}

float normalizeSceneControlValue(const ScenePerformanceEvent& event)
{
    return SceneAutomationRules::normalizeValue(event);
}
} // namespace

bool MlrVSTAudioProcessor::isScenePerformanceRecording() const
{
    return scenePerformanceRecorder.isRecording();
}

bool MlrVSTAudioProcessor::isScenePerformanceOverdubbing() const
{
    return scenePerformanceRecorder.isOverdubbing();
}

int MlrVSTAudioProcessor::getScenePerformanceRecordingSceneSlot() const
{
    return scenePerformanceRecorder.getRecordingSceneSlot();
}

double MlrVSTAudioProcessor::getScenePerformanceRecordingStartBeat() const
{
    return scenePerformanceRecorder.getRecordingStartBeat();
}

double MlrVSTAudioProcessor::getScenePerformanceRecordingEndBeat() const
{
    return scenePerformanceRecorder.getRecordingEndBeat();
}

double MlrVSTAudioProcessor::getScenePerformanceClipLengthBeats(int sceneSlot) const
{
    return getResolvedSceneLengthBeats(sceneSlot);
}

bool MlrVSTAudioProcessor::hasScenePerformanceClip(int sceneSlot) const
{
    return scenePerformanceRecorder.hasEvents(sceneSlot);
}

int MlrVSTAudioProcessor::getScenePerformanceEventCount(int sceneSlot) const
{
    return scenePerformanceRecorder.getEventCount(sceneSlot);
}

double MlrVSTAudioProcessor::getScenePerformancePlaybackProgress(int sceneSlot, double currentBeat) const
{
    if (!activeSceneStartPpqValid || !std::isfinite(activeSceneStartPpq))
        return -1.0;

    return scenePerformanceRecorder.getPlaybackProgressForBeat(sceneSlot, activeSceneStartPpq, currentBeat);
}

double MlrVSTAudioProcessor::getScenePerformancePlaybackBeat(int sceneSlot, double currentBeat) const
{
    if (!activeSceneStartPpqValid || !std::isfinite(activeSceneStartPpq) || !std::isfinite(currentBeat))
        return -1.0;

    const int safeSceneSlot = juce::jlimit(0, SceneSlots - 1, sceneSlot);
    const double lengthBeats = juce::jmax(1.0, getScenePerformanceClipLengthBeats(safeSceneSlot));
    return wrapBeatIntoSceneClip(currentBeat, activeSceneStartPpq, lengthBeats);
}

double MlrVSTAudioProcessor::getScenePerformanceRecordingProgress(double currentBeat) const
{
    return scenePerformanceRecorder.getRecordingProgressForBeat(currentBeat);
}

std::vector<ScenePerformanceEvent> MlrVSTAudioProcessor::getScenePerformanceEventsSnapshot(int sceneSlot) const
{
    return scenePerformanceRecorder.getEventsSnapshot(sceneSlot);
}

const MlrVSTAudioProcessor::PreparedSceneStripState* MlrVSTAudioProcessor::getStoredSceneStripStateForSlot(
    int sceneSlot,
    int stripIndex,
    PreparedSceneStripState& fallbackStripState) const
{
    const int safeSceneSlot = juce::jlimit(0, SceneSlots - 1, sceneSlot);
    const int safeStripIndex = juce::jlimit(0, MaxStrips - 1, stripIndex);

    int mainPresetIndex = getActiveMainPresetIndexForScenes();
    if (safeSceneSlot == juce::jlimit(0, SceneSlots - 1, activeSceneSlot))
        mainPresetIndex = juce::jlimit(0, MaxPresetSlots - 1, activeSceneMainPresetIndex);

    if (const auto* storedSceneState = getStoredSceneSlotState(mainPresetIndex, safeSceneSlot))
    {
        if (storedSceneState->preparedSwitchPayloadTemplate != nullptr)
        {
            const auto& candidate =
                storedSceneState->preparedSwitchPayloadTemplate->stripStates[static_cast<size_t>(safeStripIndex)];
            if (candidate.present)
                return &candidate;
        }
    }

    return &fallbackStripState;
}

bool MlrVSTAudioProcessor::stripUsesGrainSceneLanesForSceneSlot(int sceneSlot, int stripIndex) const
{
    PreparedSceneStripState fallbackStripState;
    const auto* storedStripState = getStoredSceneStripStateForSlot(sceneSlot, stripIndex, fallbackStripState);
    if (storedStripState != nullptr && storedStripState->present)
        return storedStripState->playMode == EnhancedAudioStrip::PlayMode::Grain;

    return stripUsesGrainSceneLanes(stripIndex);
}

bool MlrVSTAudioProcessor::getStoredSceneControlValue(int sceneSlot,
                                                      int stripIndex,
                                                      ScenePerformanceControlTarget target,
                                                      float& valueOut) const
{
    if (target != ScenePerformanceControlTarget::Retrigger
        && (stripIndex < 0 || stripIndex >= MaxStrips))
    {
        return false;
    }

    const int safeSceneSlot = juce::jlimit(0, SceneSlots - 1, sceneSlot);
    const int safeStripIndex = juce::jlimit(0, MaxStrips - 1, stripIndex);
    PreparedSceneStripState fallbackStripState;
    const PreparedSceneStripState* storedStripState =
        getStoredSceneStripStateForSlot(safeSceneSlot, safeStripIndex, fallbackStripState);

    auto ownedControls = storedStripState->parameterState.ownedControls;
    if (!storedStripState->present && audioEngine != nullptr)
    {
        if (auto* strip = audioEngine->getStrip(safeStripIndex))
            ownedControls.usesGrainPlaybackSpeed = (strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Grain);
    }

    switch (target)
    {
        case ScenePerformanceControlTarget::Retrigger:
            valueOut = 0.0f;
            break;
        case ScenePerformanceControlTarget::Speed:
            valueOut = ownedControls.usesGrainPlaybackSpeed
                ? PlayheadSpeedQuantizer::grainControlValueFromPlaybackSpeed(ownedControls.playbackSpeed)
                : ownedControls.playheadSpeedRatio;
            break;
        case ScenePerformanceControlTarget::Pitch:
            valueOut = storedStripState->parameterState.pitchSemitones;
            break;
        case ScenePerformanceControlTarget::Pan:
            valueOut = ownedControls.pan;
            break;
        case ScenePerformanceControlTarget::Volume:
            valueOut = ownedControls.volume;
            break;
        case ScenePerformanceControlTarget::Swing:
            valueOut = storedStripState->swingAmount;
            break;
        case ScenePerformanceControlTarget::GrainSize:
            valueOut = storedStripState->grainSizeMs;
            break;
        case ScenePerformanceControlTarget::GrainDensity:
            valueOut = storedStripState->grainDensity;
            break;
        case ScenePerformanceControlTarget::GrainPitch:
            valueOut = storedStripState->grainPitch;
            break;
        case ScenePerformanceControlTarget::GrainPitchJitter:
            valueOut = storedStripState->grainPitchJitter;
            break;
        case ScenePerformanceControlTarget::GrainSpread:
            valueOut = storedStripState->grainSpread;
            break;
        case ScenePerformanceControlTarget::GrainJitter:
            valueOut = storedStripState->grainJitter;
            break;
        case ScenePerformanceControlTarget::GrainPositionJitter:
            valueOut = storedStripState->grainPositionJitter;
            break;
        case ScenePerformanceControlTarget::GrainRandomDepth:
            valueOut = storedStripState->grainRandomDepth;
            break;
        case ScenePerformanceControlTarget::GrainArp:
            valueOut = storedStripState->grainArpDepth;
            break;
        case ScenePerformanceControlTarget::GrainCloud:
            valueOut = storedStripState->grainCloudDepth;
            break;
        case ScenePerformanceControlTarget::GrainEmitter:
            valueOut = storedStripState->grainEmitterDepth;
            break;
        case ScenePerformanceControlTarget::GrainEnvelope:
            valueOut = storedStripState->grainEnvelope;
            break;
        case ScenePerformanceControlTarget::GrainShape:
            valueOut = storedStripState->grainShape;
            break;
        case ScenePerformanceControlTarget::FilterFrequency:
            valueOut = ownedControls.filterFrequency;
            break;
        case ScenePerformanceControlTarget::FilterResonance:
            valueOut = ownedControls.filterResonance;
            break;
        case ScenePerformanceControlTarget::FilterEnabled:
            valueOut = ownedControls.filterEnabled ? 1.0f : 0.0f;
            break;
        case ScenePerformanceControlTarget::FilterMorph:
            valueOut = ownedControls.filterMorph;
            break;
        case ScenePerformanceControlTarget::SliceLength:
            valueOut = storedStripState->parameterState.sliceLength;
            break;
        case ScenePerformanceControlTarget::Scratch:
            valueOut = storedStripState->scratchAmount;
            break;
        case ScenePerformanceControlTarget::DelayMix:
            valueOut = ownedControls.delayMix;
            break;
        case ScenePerformanceControlTarget::DelayTime:
            valueOut = ownedControls.delayTime;
            break;
        case ScenePerformanceControlTarget::DelayFeedback:
            valueOut = ownedControls.delayFeedback;
            break;
        case ScenePerformanceControlTarget::DelayLowCut:
            valueOut = ownedControls.delayLowCutHz;
            break;
        case ScenePerformanceControlTarget::DelayHighCut:
            valueOut = ownedControls.delayHighCutHz;
            break;
        case ScenePerformanceControlTarget::DelayMode:
            valueOut = static_cast<float>(static_cast<int>(ownedControls.delayMode));
            break;
        case ScenePerformanceControlTarget::DelaySyncEnabled:
            valueOut = ownedControls.delaySyncEnabled ? 1.0f : 0.0f;
            break;
        case ScenePerformanceControlTarget::Rearrange:
            valueOut = 0.0f;
            break;
        case ScenePerformanceControlTarget::None:
        default:
            return false;
    }

    return std::isfinite(valueOut);
}

bool MlrVSTAudioProcessor::getSceneControlBaseNormalizedValue(int stripIndex,
                                                              ScenePerformanceControlTarget target,
                                                              float& normalizedOut) const
{
    if (target != ScenePerformanceControlTarget::Retrigger
        && (stripIndex < 0 || stripIndex >= MaxStrips))
    {
        return false;
    }

    const int safeStripIndex = juce::jlimit(0, MaxStrips - 1, stripIndex);
    auto* strip = (audioEngine != nullptr && stripIndex >= 0) ? audioEngine->getStrip(safeStripIndex) : nullptr;

    float value = 0.0f;
    switch (target)
    {
        case ScenePerformanceControlTarget::Retrigger:
            value = getGlobalSceneStutterAmount();
            break;
        case ScenePerformanceControlTarget::Speed:
            value = stripSpeedParams[static_cast<size_t>(safeStripIndex)] != nullptr
                ? stripSpeedParams[static_cast<size_t>(safeStripIndex)]->load(std::memory_order_acquire)
                : (strip != nullptr ? strip->getPlaybackSpeed() : 1.0f);
            break;
        case ScenePerformanceControlTarget::Pitch:
            value = stripPitchParams[static_cast<size_t>(safeStripIndex)] != nullptr
                ? stripPitchParams[static_cast<size_t>(safeStripIndex)]->load(std::memory_order_acquire)
                : (strip != nullptr ? getPitchSemitonesForDisplay(*strip) : 0.0f);
            break;
        case ScenePerformanceControlTarget::Pan:
            value = stripPanParams[static_cast<size_t>(safeStripIndex)] != nullptr
                ? stripPanParams[static_cast<size_t>(safeStripIndex)]->load(std::memory_order_acquire)
                : (strip != nullptr ? strip->getPan() : 0.0f);
            break;
        case ScenePerformanceControlTarget::Volume:
            value = stripVolumeParams[static_cast<size_t>(safeStripIndex)] != nullptr
                ? stripVolumeParams[static_cast<size_t>(safeStripIndex)]->load(std::memory_order_acquire)
                : (strip != nullptr ? strip->getVolume() : 1.0f);
            break;
        case ScenePerformanceControlTarget::Swing:
            value = strip != nullptr ? strip->getSwingAmount() : 0.0f;
            break;
        case ScenePerformanceControlTarget::GrainSize:
            value = strip != nullptr ? strip->getGrainSizeMs() : 5.0f;
            break;
        case ScenePerformanceControlTarget::GrainDensity:
            value = strip != nullptr ? strip->getGrainDensity() : 0.05f;
            break;
        case ScenePerformanceControlTarget::GrainPitch:
            value = strip != nullptr ? strip->getGrainPitch() : 0.0f;
            break;
        case ScenePerformanceControlTarget::GrainPitchJitter:
            value = strip != nullptr ? strip->getGrainPitchJitter() : 0.0f;
            break;
        case ScenePerformanceControlTarget::GrainSpread:
            value = strip != nullptr ? strip->getGrainSpread() : 0.0f;
            break;
        case ScenePerformanceControlTarget::GrainJitter:
            value = strip != nullptr ? strip->getGrainJitter() : 0.0f;
            break;
        case ScenePerformanceControlTarget::GrainPositionJitter:
            value = strip != nullptr ? strip->getGrainPositionJitter() : 0.0f;
            break;
        case ScenePerformanceControlTarget::GrainRandomDepth:
            value = strip != nullptr ? strip->getGrainRandomDepth() : 0.0f;
            break;
        case ScenePerformanceControlTarget::GrainArp:
            value = strip != nullptr ? strip->getGrainArpDepth() : 0.0f;
            break;
        case ScenePerformanceControlTarget::GrainCloud:
            value = strip != nullptr ? strip->getGrainCloudDepth() : 0.0f;
            break;
        case ScenePerformanceControlTarget::GrainEmitter:
            value = strip != nullptr ? strip->getGrainEmitterDepth() : 0.0f;
            break;
        case ScenePerformanceControlTarget::GrainEnvelope:
            value = strip != nullptr ? strip->getGrainEnvelope() : 0.0f;
            break;
        case ScenePerformanceControlTarget::GrainShape:
            value = strip != nullptr ? strip->getGrainShape() : 0.0f;
            break;
        case ScenePerformanceControlTarget::FilterFrequency:
            value = stripFilterFrequencyParams[static_cast<size_t>(safeStripIndex)] != nullptr
                ? stripFilterFrequencyParams[static_cast<size_t>(safeStripIndex)]->load(std::memory_order_acquire)
                : (strip != nullptr ? strip->getFilterFrequency() : 20000.0f);
            break;
        case ScenePerformanceControlTarget::FilterResonance:
            value = stripFilterResonanceParams[static_cast<size_t>(safeStripIndex)] != nullptr
                ? stripFilterResonanceParams[static_cast<size_t>(safeStripIndex)]->load(std::memory_order_acquire)
                : (strip != nullptr ? strip->getFilterResonance() : 0.707f);
            break;
        case ScenePerformanceControlTarget::FilterEnabled:
            value = stripFilterEnabledParams[static_cast<size_t>(safeStripIndex)] != nullptr
                ? stripFilterEnabledParams[static_cast<size_t>(safeStripIndex)]->load(std::memory_order_acquire)
                : (strip != nullptr && strip->isFilterEnabled() ? 1.0f : 0.0f);
            break;
        case ScenePerformanceControlTarget::FilterMorph:
            value = stripFilterMorphParams[static_cast<size_t>(safeStripIndex)] != nullptr
                ? stripFilterMorphParams[static_cast<size_t>(safeStripIndex)]->load(std::memory_order_acquire)
                : (strip != nullptr ? strip->getFilterMorph() : 0.0f);
            break;
        case ScenePerformanceControlTarget::SliceLength:
            value = stripSliceLengthParams[static_cast<size_t>(safeStripIndex)] != nullptr
                ? stripSliceLengthParams[static_cast<size_t>(safeStripIndex)]->load(std::memory_order_acquire)
                : (strip != nullptr ? strip->getLoopSliceLength() : 1.0f);
            break;
        case ScenePerformanceControlTarget::Scratch:
            value = strip != nullptr ? strip->getScratchAmount() : 0.0f;
            break;
        case ScenePerformanceControlTarget::DelayMix:
            value = stripDelayMixParams[static_cast<size_t>(safeStripIndex)] != nullptr
                ? stripDelayMixParams[static_cast<size_t>(safeStripIndex)]->load(std::memory_order_acquire)
                : (strip != nullptr ? strip->getDelayMix() : 0.0f);
            break;
        case ScenePerformanceControlTarget::DelayTime:
            value = stripDelayTimeParams[static_cast<size_t>(safeStripIndex)] != nullptr
                ? stripDelayTimeParams[static_cast<size_t>(safeStripIndex)]->load(std::memory_order_acquire)
                : (strip != nullptr ? strip->getDelayTimeBeats() : 0.25f);
            break;
        case ScenePerformanceControlTarget::DelayFeedback:
            value = stripDelayFeedbackParams[static_cast<size_t>(safeStripIndex)] != nullptr
                ? stripDelayFeedbackParams[static_cast<size_t>(safeStripIndex)]->load(std::memory_order_acquire)
                : (strip != nullptr ? strip->getDelayFeedback() : 0.0f);
            break;
        case ScenePerformanceControlTarget::DelayLowCut:
            value = stripDelayLowCutParams[static_cast<size_t>(safeStripIndex)] != nullptr
                ? stripDelayLowCutParams[static_cast<size_t>(safeStripIndex)]->load(std::memory_order_acquire)
                : (strip != nullptr ? strip->getDelayLowCutHz() : 20.0f);
            break;
        case ScenePerformanceControlTarget::DelayHighCut:
            value = stripDelayHighCutParams[static_cast<size_t>(safeStripIndex)] != nullptr
                ? stripDelayHighCutParams[static_cast<size_t>(safeStripIndex)]->load(std::memory_order_acquire)
                : (strip != nullptr ? strip->getDelayHighCutHz() : 20000.0f);
            break;
        case ScenePerformanceControlTarget::DelayMode:
            value = stripDelayModeParams[static_cast<size_t>(safeStripIndex)] != nullptr
                ? stripDelayModeParams[static_cast<size_t>(safeStripIndex)]->load(std::memory_order_acquire)
                : (strip != nullptr ? static_cast<float>(static_cast<int>(strip->getDelayMode())) : 0.0f);
            break;
        case ScenePerformanceControlTarget::DelaySyncEnabled:
            value = stripDelaySyncParams[static_cast<size_t>(safeStripIndex)] != nullptr
                ? stripDelaySyncParams[static_cast<size_t>(safeStripIndex)]->load(std::memory_order_acquire)
                : (strip != nullptr && strip->isDelaySyncEnabled() ? 1.0f : 0.0f);
            break;
        case ScenePerformanceControlTarget::Rearrange:
            value = strip != nullptr && strip->isTraversalRearrangeActive()
                ? strip->getTraversalRearrangeValue()
                : 0.0f;
            break;
        case ScenePerformanceControlTarget::None:
        default:
            return false;
    }

    ScenePerformanceEvent probe;
    probe.type = ScenePerformanceEventType::ControlPoint;
    probe.stripIndex = (target == ScenePerformanceControlTarget::Retrigger) ? -1 : safeStripIndex;
    probe.controlTarget = target;
    probe.value = value;
    normalizedOut = normalizeSceneControlValue(probe);
    return std::isfinite(normalizedOut);
}

bool MlrVSTAudioProcessor::getStoredSceneControlNormalizedValue(int sceneSlot,
                                                                int stripIndex,
                                                                ScenePerformanceControlTarget target,
                                                                float& normalizedOut) const
{
    float value = 0.0f;
    if (!getStoredSceneControlValue(sceneSlot, stripIndex, target, value))
        return false;

    ScenePerformanceEvent probe;
    probe.type = ScenePerformanceEventType::ControlPoint;
    probe.stripIndex = (target == ScenePerformanceControlTarget::Retrigger)
        ? -1
        : juce::jlimit(0, MaxStrips - 1, stripIndex);
    probe.controlTarget = target;
    probe.value = value;
    normalizedOut = normalizeSceneControlValue(probe);
    return std::isfinite(normalizedOut);
}

bool MlrVSTAudioProcessor::getSceneControlCurrentValue(int stripIndex,
                                                       ScenePerformanceControlTarget target,
                                                       float& valueOut) const
{
    if (target != ScenePerformanceControlTarget::Retrigger
        && (stripIndex < 0 || stripIndex >= MaxStrips))
    {
        return false;
    }

    if (target == ScenePerformanceControlTarget::Retrigger)
    {
        valueOut = getGlobalSceneStutterAmount();
        return std::isfinite(valueOut);
    }

    if (audioEngine == nullptr)
        return false;

    const int safeStripIndex = juce::jlimit(0, MaxStrips - 1, stripIndex);
    auto* strip = audioEngine->getStrip(safeStripIndex);
    if (strip == nullptr)
        return false;

    switch (target)
    {
        case ScenePerformanceControlTarget::Speed:
            valueOut = strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Grain
                ? strip->getPlaybackSpeed()
                : strip->getPlayheadSpeedRatio();
            break;
        case ScenePerformanceControlTarget::Pitch:
            valueOut = strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Grain
                ? strip->getGrainPitch()
                : getPitchSemitonesForDisplay(*strip);
            break;
        case ScenePerformanceControlTarget::Pan:
            valueOut = strip->getPan();
            break;
        case ScenePerformanceControlTarget::Volume:
            valueOut = strip->getVolume();
            break;
        case ScenePerformanceControlTarget::Swing:
            valueOut = strip->getSwingAmount();
            break;
        case ScenePerformanceControlTarget::GrainSize:
            valueOut = strip->getGrainSizeMs();
            break;
        case ScenePerformanceControlTarget::GrainDensity:
            valueOut = strip->getGrainDensity();
            break;
        case ScenePerformanceControlTarget::GrainPitch:
            valueOut = strip->getGrainPitch();
            break;
        case ScenePerformanceControlTarget::GrainPitchJitter:
            valueOut = strip->getGrainPitchJitter();
            break;
        case ScenePerformanceControlTarget::GrainSpread:
            valueOut = strip->getGrainSpread();
            break;
        case ScenePerformanceControlTarget::GrainJitter:
            valueOut = strip->getGrainJitter();
            break;
        case ScenePerformanceControlTarget::GrainPositionJitter:
            valueOut = strip->getGrainPositionJitter();
            break;
        case ScenePerformanceControlTarget::GrainRandomDepth:
            valueOut = strip->getGrainRandomDepth();
            break;
        case ScenePerformanceControlTarget::GrainArp:
            valueOut = strip->getGrainArpDepth();
            break;
        case ScenePerformanceControlTarget::GrainCloud:
            valueOut = strip->getGrainCloudDepth();
            break;
        case ScenePerformanceControlTarget::GrainEmitter:
            valueOut = strip->getGrainEmitterDepth();
            break;
        case ScenePerformanceControlTarget::GrainEnvelope:
            valueOut = strip->getGrainEnvelope();
            break;
        case ScenePerformanceControlTarget::GrainShape:
            valueOut = strip->getGrainShape();
            break;
        case ScenePerformanceControlTarget::FilterFrequency:
            valueOut = strip->getDisplayedFilterFrequency();
            break;
        case ScenePerformanceControlTarget::FilterResonance:
            valueOut = strip->getDisplayedFilterResonance();
            break;
        case ScenePerformanceControlTarget::FilterEnabled:
            valueOut = strip->isFilterEnabled() ? 1.0f : 0.0f;
            break;
        case ScenePerformanceControlTarget::FilterMorph:
            valueOut = strip->getFilterMorph();
            break;
        case ScenePerformanceControlTarget::SliceLength:
            valueOut = strip->getLoopSliceLength();
            break;
        case ScenePerformanceControlTarget::Scratch:
            valueOut = strip->getScratchAmount();
            break;
        case ScenePerformanceControlTarget::DelayMix:
            valueOut = strip->getDisplayedDelayMix();
            break;
        case ScenePerformanceControlTarget::DelayTime:
            valueOut = strip->getDisplayedDelayTimeBeats();
            break;
        case ScenePerformanceControlTarget::DelayFeedback:
            valueOut = strip->getDisplayedDelayFeedback();
            break;
        case ScenePerformanceControlTarget::DelayLowCut:
            valueOut = strip->getDisplayedDelayLowCutHz();
            break;
        case ScenePerformanceControlTarget::DelayHighCut:
            valueOut = strip->getDisplayedDelayHighCutHz();
            break;
        case ScenePerformanceControlTarget::DelayMode:
            valueOut = static_cast<float>(static_cast<int>(strip->getDelayMode()));
            break;
        case ScenePerformanceControlTarget::DelaySyncEnabled:
            valueOut = strip->isDelaySyncEnabled() ? 1.0f : 0.0f;
            break;
        case ScenePerformanceControlTarget::Rearrange:
            valueOut = strip->isTraversalRearrangeActive()
                ? strip->getTraversalRearrangeValue()
                : 0.0f;
            break;
        case ScenePerformanceControlTarget::Retrigger:
        case ScenePerformanceControlTarget::None:
        default:
            return false;
    }

    return std::isfinite(valueOut);
}

bool MlrVSTAudioProcessor::copySceneSlotToClipboard(int sceneSlot)
{
    const int safeSceneSlot = juce::jlimit(0, SceneSlots - 1, sceneSlot);
    const int mainPresetIndex = getActiveMainPresetIndexForScenes();

    if (safeSceneSlot == activeSceneSlot)
    {
        if (!refreshStoredSceneSlotSnapshot(mainPresetIndex, safeSceneSlot))
            return false;
    }

    if (!sceneSlotExistsForMainPreset(mainPresetIndex, safeSceneSlot))
        return false;

    sceneSlotClipboardSourceSlot = safeSceneSlot;
    sceneSlotClipboardMainPresetIndex = mainPresetIndex;
    return true;
}

bool MlrVSTAudioProcessor::pasteSceneSlotFromClipboard(int sceneSlot)
{
    if (sceneSlotClipboardSourceSlot < 0)
        return false;

    const int safeSceneSlot = juce::jlimit(0, SceneSlots - 1, sceneSlot);
    const int mainPresetIndex = getActiveMainPresetIndexForScenes();
    if (sceneSlotClipboardMainPresetIndex != mainPresetIndex)
        return false;

    const bool pastingIntoActiveScene = isSceneModeEnabled()
        && audioEngine != nullptr
        && activeSceneMainPresetIndex == juce::jlimit(0, MaxPresetSlots - 1, mainPresetIndex)
        && activeSceneSlot == safeSceneSlot;

    const bool pasted = copySceneForMainPreset(mainPresetIndex,
                                               sceneSlotClipboardSourceSlot,
                                               safeSceneSlot);
    if (!pasted)
        return false;

    if (pastingIntoActiveScene)
    {
        double hostPpqSnapshot = std::numeric_limits<double>::quiet_NaN();
        double hostTempoSnapshot = 120.0;
        if (!getHostSyncSnapshot(hostPpqSnapshot, hostTempoSnapshot) && audioEngine != nullptr)
        {
            const double fallbackPpq = audioEngine->getTimelineBeat();
            const double fallbackTempo = juce::jmax(1.0, audioEngine->getCurrentTempo());
            if (std::isfinite(fallbackPpq) && std::isfinite(fallbackTempo) && fallbackTempo > 0.0)
            {
                hostPpqSnapshot = fallbackPpq;
                hostTempoSnapshot = fallbackTempo;
            }
        }

        performSceneLoad(mainPresetIndex,
                         safeSceneSlot,
                         hostPpqSnapshot,
                         hostTempoSnapshot,
                         audioEngine != nullptr ? audioEngine->getGlobalSampleCount() : -1,
                         false);
    }

    updateMonomeLEDs();
    return true;
}

bool MlrVSTAudioProcessor::replaceSceneSlotWithScene(int sourceSceneSlot, int destSceneSlot)
{
    const int safeSource = juce::jlimit(0, SceneSlots - 1, sourceSceneSlot);
    const int safeDest = juce::jlimit(0, SceneSlots - 1, destSceneSlot);
    if (safeSource == safeDest)
        return false;

    const int mainPresetIndex = getActiveMainPresetIndexForScenes();
    const bool replacingActiveScene = isSceneModeEnabled()
        && audioEngine != nullptr
        && activeSceneMainPresetIndex == juce::jlimit(0, MaxPresetSlots - 1, mainPresetIndex)
        && activeSceneSlot == safeDest;

    if (!copySceneForMainPreset(mainPresetIndex, safeSource, safeDest))
        return false;

    setSceneRepeatCount(safeDest, getSceneRepeatCount(safeSource));
    setSceneManualBars(safeDest, getSceneManualBars(safeSource));

    if (replacingActiveScene)
    {
        double hostPpqSnapshot = std::numeric_limits<double>::quiet_NaN();
        double hostTempoSnapshot = 120.0;
        if (!getHostSyncSnapshot(hostPpqSnapshot, hostTempoSnapshot) && audioEngine != nullptr)
        {
            const double fallbackPpq = audioEngine->getTimelineBeat();
            const double fallbackTempo = juce::jmax(1.0, audioEngine->getCurrentTempo());
            if (std::isfinite(fallbackPpq) && std::isfinite(fallbackTempo) && fallbackTempo > 0.0)
            {
                hostPpqSnapshot = fallbackPpq;
                hostTempoSnapshot = fallbackTempo;
            }
        }

        performSceneLoad(mainPresetIndex,
                         safeDest,
                         hostPpqSnapshot,
                         hostTempoSnapshot,
                         audioEngine != nullptr ? audioEngine->getGlobalSampleCount() : -1,
                         false);
    }

    updateMonomeLEDs();
    return true;
}

bool MlrVSTAudioProcessor::hasSceneSlotClipboard() const
{
    return sceneSlotClipboardSourceSlot >= 0;
}

int MlrVSTAudioProcessor::getSceneSlotClipboardSourceSlot() const
{
    return sceneSlotClipboardSourceSlot;
}

bool MlrVSTAudioProcessor::clearSceneSlot(int sceneSlot)
{
    const int safeSceneSlot = juce::jlimit(0, SceneSlots - 1, sceneSlot);
    const int mainPresetIndex = getActiveMainPresetIndexForScenes();
    const bool clearingActiveScene = isSceneModeEnabled()
        && audioEngine != nullptr
        && activeSceneMainPresetIndex == juce::jlimit(0, MaxPresetSlots - 1, mainPresetIndex)
        && activeSceneSlot == safeSceneSlot;
    ScopedSceneAutosaveSuppression suppressSceneAutosave(*this);

    setSceneRepeatCount(safeSceneSlot, 1);
    setSceneLengthMode(safeSceneSlot, SceneLengthMode::ManualBars);
    setSceneLengthCount(safeSceneSlot, 4);
    setSceneAnchorStrip(safeSceneSlot, 0);

    for (int stripIndex = 0; stripIndex < MaxStrips; ++stripIndex)
        clearSceneMotionStripState(safeSceneSlot, stripIndex);
    clearScenePerformanceClip(safeSceneSlot);

    const auto preservedSceneTimingState = createSceneChainStateXml(-1);
    const auto preservedScenePerformanceState = createScenePerformanceStateData(-1);

    deleteSceneForMainPreset(mainPresetIndex, safeSceneSlot);

    if (safeSceneSlot == activeSceneSlot)
    {
        clearPendingActiveSceneAutosave();
        activeSceneNeedsCaptureBeforeManualRecall = false;
    }

    if (!clearingActiveScene)
        return true;

    struct ScopedSuspendProcessing
    {
        explicit ScopedSuspendProcessing(MlrVSTAudioProcessor& p) : processor(p) { processor.suspendProcessing(true); }
        ~ScopedSuspendProcessing() { processor.suspendProcessing(false); }
        MlrVSTAudioProcessor& processor;
    } scopedSuspend(*this);

    const int preservedFocusedSceneSlot = getFocusedSceneSlot();
    resetRuntimePresetStateToDefaults();
    activeSceneMainPresetIndex = juce::jlimit(0, MaxPresetSlots - 1, mainPresetIndex);
    activeSceneSlot = safeSceneSlot;
    activeSceneNeedsCaptureBeforeManualRecall = false;

    if (preservedSceneTimingState != nullptr)
        applySceneChainStateXml(preservedSceneTimingState.get(), -1);

    if (preservedScenePerformanceState.getSize() > 0)
        applyScenePerformanceStateData(preservedScenePerformanceState, -1);
    else
        applyScenePerformanceStateData({}, -1);

    clearPendingActiveSceneAutosave();
    focusSceneSlot(preservedFocusedSceneSlot);
    updateMonomeLEDs();
    return true;
}

bool MlrVSTAudioProcessor::copyScenePerformanceClipToClipboard(int sceneSlot)
{
    const int safeSceneSlot = juce::jlimit(0, SceneSlots - 1, sceneSlot);
    if (isSceneModeEnabled() && audioEngine != nullptr && safeSceneSlot == activeSceneSlot)
        syncSceneMotionStateFromEngine(safeSceneSlot);
    syncScenePerformanceClipLengthToResolvedLength(safeSceneSlot);
    scenePerformanceClipboardData = createScenePerformanceStateData(safeSceneSlot);
    sceneCopySourceSlot = safeSceneSlot;
    sceneCopyMainPresetIndex = getActiveMainPresetIndexForScenes();
    return scenePerformanceClipboardData.getSize() > 0;
}

bool MlrVSTAudioProcessor::pasteScenePerformanceClipFromClipboard(int sceneSlot)
{
    if (scenePerformanceClipboardData.getSize() == 0)
        return false;

    const int safeSceneSlot = juce::jlimit(0, SceneSlots - 1, sceneSlot);
    if (!applyScenePerformanceStateData(scenePerformanceClipboardData, safeSceneSlot))
        return false;

    if (safeSceneSlot == activeSceneSlot)
    {
        lastScenePerformanceProcessBeat = std::numeric_limits<double>::quiet_NaN();
        lastScenePerformanceProcessSceneSlot = -1;
        lastScenePerformanceProcessSceneStartBeat = std::numeric_limits<double>::quiet_NaN();
        if (isSceneModeEnabled()
            && audioEngine != nullptr
            && activeSceneStartPpqValid
            && std::isfinite(activeSceneStartPpq))
        {
            applySceneHeldAutomationStateAtBeat(safeSceneSlot,
                                                audioEngine->getTimelineBeat(),
                                                activeSceneStartPpq);
        }
        queueActiveSceneAutosave();
    }

    updateMonomeLEDs();
    return true;
}

bool MlrVSTAudioProcessor::duplicateScenePerformanceClip(int sourceSceneSlot, int destSceneSlot)
{
    const int safeSourceSlot = juce::jlimit(0, SceneSlots - 1, sourceSceneSlot);
    const int safeDestSlot = juce::jlimit(0, SceneSlots - 1, destSceneSlot);
    if (safeSourceSlot == safeDestSlot)
        return false;

    if (loadedPresetIndex >= 0)
    {
        const int mainPresetIndex = getActiveMainPresetIndexForScenes();
        activeSceneMainPresetIndex = mainPresetIndex;
        if (isSceneModeEnabled() && safeSourceSlot == activeSceneSlot)
            juce::ignoreUnused(refreshStoredSceneSlotSnapshot(mainPresetIndex, safeSourceSlot));
        return SceneScheduler::copySceneForMainPreset(*this, mainPresetIndex, safeSourceSlot, safeDestSlot);
    }

    if (isSceneModeEnabled() && audioEngine != nullptr && safeSourceSlot == activeSceneSlot)
        syncSceneMotionStateFromEngine(safeSourceSlot);
    syncScenePerformanceClipLengthToResolvedLength(safeSourceSlot);

    const auto stateData = createScenePerformanceStateData(safeSourceSlot);
    if (stateData.getSize() == 0)
        return false;

    if (!applyScenePerformanceStateData(stateData, safeDestSlot))
        return false;

    if (safeDestSlot == activeSceneSlot)
    {
        lastScenePerformanceProcessBeat = std::numeric_limits<double>::quiet_NaN();
        lastScenePerformanceProcessSceneSlot = -1;
        lastScenePerformanceProcessSceneStartBeat = std::numeric_limits<double>::quiet_NaN();
        queueActiveSceneAutosave();
    }

    updateMonomeLEDs();
    return true;
}

bool MlrVSTAudioProcessor::hasScenePerformanceClipboard() const
{
    return scenePerformanceClipboardData.getSize() > 0;
}

int MlrVSTAudioProcessor::getScenePerformanceClipboardSourceSlot() const
{
    return sceneCopySourceSlot;
}
