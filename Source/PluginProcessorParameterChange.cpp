/*
  ==============================================================================

    PluginProcessorParameterChange.cpp
    Extracted parameter change routing and scene autosave helpers for mlrVST.

  ==============================================================================
*/

#include "PluginProcessor.h"
#include "PlayheadSpeedQuantizer.h"
#include <cmath>

namespace
{
constexpr uint32_t kSceneAutosaveDebounceMs = 180;

constexpr std::array<const char*, 22> kPersistentGlobalControlParameterIds {
    "masterVolume",
    "limiterThreshold",
    "limiterEnabled",
    "quantize",
    "innerLoopLength",
    "quality",
    "pitchSmoothing",
    "inputMonitor",
    "crossfadeLength",
    "triggerFadeIn",
    "outputRouting",
    "pitchControlMode",
    "flipTempoMatchMode",
    "stretchBackend",
    "continuousTraversal",
    "sceneMode",
    "sceneRecallMode",
    "soundTouchEnabled",
    "transientOnsetMethod",
    "transientSensitivity",
    "transientSnap",
    "transientSpacing"
};

float normalizeSceneControlValue(const ScenePerformanceEvent& event)
{
    switch (event.controlTarget)
    {
        case ScenePerformanceControlTarget::Speed:
        {
            const float safeValue = juce::jlimit(0.125f, 8.0f, event.value);
            return juce::jlimit(0.0f, 1.0f, (std::log2(safeValue) + 3.0f) / 6.0f);
        }
        case ScenePerformanceControlTarget::Pitch:
            return juce::jlimit(0.0f, 1.0f, (event.value + 24.0f) / 48.0f);
        case ScenePerformanceControlTarget::GrainPitch:
            return juce::jlimit(0.0f, 1.0f, (event.value + 48.0f) / 96.0f);
        case ScenePerformanceControlTarget::Pan:
            return juce::jlimit(0.0f, 1.0f, (event.value + 1.0f) * 0.5f);
        case ScenePerformanceControlTarget::Volume:
        case ScenePerformanceControlTarget::Swing:
        case ScenePerformanceControlTarget::GrainSpread:
        case ScenePerformanceControlTarget::GrainJitter:
        case ScenePerformanceControlTarget::GrainPositionJitter:
        case ScenePerformanceControlTarget::GrainRandomDepth:
        case ScenePerformanceControlTarget::GrainArp:
        case ScenePerformanceControlTarget::GrainCloud:
        case ScenePerformanceControlTarget::GrainEmitter:
        case ScenePerformanceControlTarget::GrainEnvelope:
        case ScenePerformanceControlTarget::DelayMix:
        case ScenePerformanceControlTarget::FilterMorph:
        case ScenePerformanceControlTarget::FilterEnabled:
        case ScenePerformanceControlTarget::Retrigger:
        case ScenePerformanceControlTarget::Rearrange:
        case ScenePerformanceControlTarget::DelaySyncEnabled:
            return juce::jlimit(0.0f, 1.0f, event.value);
        case ScenePerformanceControlTarget::SliceLength:
            return juce::jlimit(0.0f, 1.0f, (event.value - 0.02f) / 0.98f);
        case ScenePerformanceControlTarget::Scratch:
            return juce::jlimit(0.0f, 1.0f, event.value / 100.0f);
        case ScenePerformanceControlTarget::GrainSize:
            return juce::jlimit(0.0f, 1.0f, (event.value - 5.0f) / (2400.0f - 5.0f));
        case ScenePerformanceControlTarget::GrainDensity:
            return juce::jlimit(0.0f, 1.0f, (event.value - 0.05f) / (0.9f - 0.05f));
        case ScenePerformanceControlTarget::GrainPitchJitter:
            return juce::jlimit(0.0f, 1.0f, event.value / 48.0f);
        case ScenePerformanceControlTarget::GrainShape:
            return juce::jlimit(0.0f, 1.0f, (event.value + 1.0f) * 0.5f);
        case ScenePerformanceControlTarget::FilterFrequency:
        {
            const float safeValue = juce::jlimit(20.0f, 20000.0f, event.value);
            return juce::jlimit(0.0f, 1.0f, std::log(safeValue / 20.0f) / std::log(1000.0f));
        }
        case ScenePerformanceControlTarget::FilterResonance:
            return juce::jlimit(0.0f, 1.0f, (event.value - 0.1f) / 9.9f);
        case ScenePerformanceControlTarget::DelayTime:
            return juce::jlimit(0.0f, 1.0f, (event.value - 0.25f) / (4.0f - 0.25f));
        case ScenePerformanceControlTarget::DelayFeedback:
            return juce::jlimit(0.0f, 1.0f, event.value / 0.97f);
        case ScenePerformanceControlTarget::DelayLowCut:
        {
            const juce::NormalisableRange<float> range(20.0f, 12000.0f, 1.0f, 0.25f);
            return juce::jlimit(0.0f, 1.0f, range.convertTo0to1(event.value));
        }
        case ScenePerformanceControlTarget::DelayHighCut:
        {
            const juce::NormalisableRange<float> range(200.0f, 20000.0f, 1.0f, 0.3f);
            return juce::jlimit(0.0f, 1.0f, range.convertTo0to1(event.value));
        }
        case ScenePerformanceControlTarget::DelayMode:
            return juce::jlimit(0.0f, 1.0f, event.value / 2.0f);
        case ScenePerformanceControlTarget::None:
        default:
            return 0.5f;
    }
}

bool parseStripIndexForPrefix(const juce::String& parameterID,
                              const char* prefix,
                              int& stripIndexOut) noexcept
{
    const juce::String prefixString(prefix);
    if (!parameterID.startsWith(prefixString))
        return false;

    const int stripIndex = parameterID.substring(prefixString.length()).getIntValue();
    if (stripIndex < 0 || stripIndex >= MlrVSTAudioProcessor::MaxStrips)
        return false;

    stripIndexOut = stripIndex;
    return true;
}

bool buildSceneEventFromParameterChange(const juce::String& parameterID,
                                        float newValue,
                                        ScenePerformanceEvent& outEvent)
{
    int stripIndex = -1;
    outEvent = {};
    outEvent.type = ScenePerformanceEventType::ControlPoint;

    auto assignStripEvent = [&](const char* prefix,
                                ScenePerformanceControlTarget target,
                                MlrVSTAudioProcessor::ControlMode controlMode,
                                int controlRow,
                                float value) -> bool
    {
        if (!parseStripIndexForPrefix(parameterID, prefix, stripIndex))
            return false;

        outEvent.stripIndex = stripIndex;
        outEvent.controlTarget = target;
        outEvent.controlMode = static_cast<int>(controlMode);
        outEvent.controlRow = controlRow;
        outEvent.value = value;
        outEvent.column = juce::jlimit(0,
                                       15,
                                       static_cast<int>(std::round(normalizeSceneControlValue(outEvent) * 15.0f)));
        return true;
    };

    return assignStripEvent("stripVolume",
                            ScenePerformanceControlTarget::Volume,
                            MlrVSTAudioProcessor::ControlMode::Volume,
                            0,
                            juce::jlimit(0.0f, 1.0f, newValue))
        || assignStripEvent("stripPan",
                            ScenePerformanceControlTarget::Pan,
                            MlrVSTAudioProcessor::ControlMode::Pan,
                            0,
                            juce::jlimit(-1.0f, 1.0f, newValue))
        || assignStripEvent("stripSpeed",
                            ScenePerformanceControlTarget::Speed,
                            MlrVSTAudioProcessor::ControlMode::Speed,
                            0,
                            PlayheadSpeedQuantizer::quantizeRatio(juce::jlimit(0.125f, 8.0f, newValue)))
        || assignStripEvent("stripPitch",
                            ScenePerformanceControlTarget::Pitch,
                            MlrVSTAudioProcessor::ControlMode::Pitch,
                            0,
                            newValue)
        || assignStripEvent("stripSliceLength",
                            ScenePerformanceControlTarget::SliceLength,
                            MlrVSTAudioProcessor::ControlMode::Normal,
                            0,
                            juce::jlimit(0.02f, 1.0f, newValue))
        || assignStripEvent("stripFilterFrequency",
                            ScenePerformanceControlTarget::FilterFrequency,
                            MlrVSTAudioProcessor::ControlMode::Filter,
                            0,
                            juce::jlimit(20.0f, 20000.0f, newValue))
        || assignStripEvent("stripFilterResonance",
                            ScenePerformanceControlTarget::FilterResonance,
                            MlrVSTAudioProcessor::ControlMode::Filter,
                            1,
                            juce::jmax(0.1f, newValue))
        || assignStripEvent("stripFilterMorph",
                            ScenePerformanceControlTarget::FilterMorph,
                            MlrVSTAudioProcessor::ControlMode::Filter,
                            2,
                            juce::jlimit(0.0f, 1.0f, newValue))
        || assignStripEvent("stripDelayMix",
                            ScenePerformanceControlTarget::DelayMix,
                            MlrVSTAudioProcessor::ControlMode::Delay,
                            0,
                            juce::jlimit(0.0f, 1.0f, newValue))
        || assignStripEvent("stripDelayTime",
                            ScenePerformanceControlTarget::DelayTime,
                            MlrVSTAudioProcessor::ControlMode::Delay,
                            1,
                            juce::jlimit(0.25f, 4.0f, newValue))
        || assignStripEvent("stripDelayFeedback",
                            ScenePerformanceControlTarget::DelayFeedback,
                            MlrVSTAudioProcessor::ControlMode::Delay,
                            2,
                            juce::jlimit(0.0f, 0.97f, newValue));
}

bool isSceneAutosaveParameterId(const juce::String& parameterID)
{
    return parameterID.startsWith("stripVolume")
        || parameterID.startsWith("stripTrimDb")
        || parameterID.startsWith("stripPan")
        || parameterID.startsWith("stripSpeed")
        || parameterID.startsWith("stripPitch")
        || parameterID.startsWith("stripSliceLength")
        || parameterID.startsWith("stripPitchControlMode")
        || parameterID.startsWith("stripTempoMatchMode")
        || parameterID.startsWith("stripFilterEnabled")
        || parameterID.startsWith("stripFilterFrequency")
        || parameterID.startsWith("stripFilterResonance")
        || parameterID.startsWith("stripFilterMorph")
        || parameterID.startsWith("stripFilterAlgorithm")
        || parameterID.startsWith("stripDuckEnabled")
        || parameterID.startsWith("stripDuckSource")
        || parameterID.startsWith("stripDuckThreshold")
        || parameterID.startsWith("stripDuckRatio")
        || parameterID.startsWith("stripDuckAttack")
        || parameterID.startsWith("stripDuckRelease")
        || parameterID.startsWith("stripDuckGainComp")
        || parameterID.startsWith("stripDuckFollowMaster")
        || parameterID.startsWith("stripDelayMix")
        || parameterID.startsWith("stripDelayTime")
        || parameterID.startsWith("stripDelaySync")
        || parameterID.startsWith("stripDelayFeedback")
        || parameterID.startsWith("stripDelayLowCut")
        || parameterID.startsWith("stripDelayHighCut")
        || parameterID.startsWith("stripDelayMode");
}

bool isPersistentGlobalControlParameterId(const juce::String& parameterID)
{
    for (const auto* id : kPersistentGlobalControlParameterIds)
    {
        if (parameterID == id)
            return true;
    }
    return false;
}
} // namespace

void MlrVSTAudioProcessor::parameterChanged(const juce::String& parameterID, float newValue)
{
    juce::ignoreUnused(newValue);

    if (parameterID == "innerLoopLength")
    {
        const int newSelection = juce::jlimit(0, 4, static_cast<int>(std::round(newValue)));
        const int previousSelection = lastAppliedInnerLoopLengthSelection.exchange(newSelection,
                                                                                   std::memory_order_acq_rel);
        innerLoopLengthSelection.store(newSelection, std::memory_order_release);

        if (previousSelection != newSelection)
            rescaleActiveInnerLoopsForGlobalFactor(previousSelection, newSelection);
    }

    if (parameterID == "limiterThreshold")
    {
        if (auto* param = parameters.getParameter("limiterThreshold"))
        {
            const float currentValue = param->convertFrom0to1(param->getValue());
            if (std::abs(currentValue) > 1.0e-4f)
            {
                suppressPersistentGlobalControlsSave.store(1, std::memory_order_release);
                param->setValueNotifyingHost(param->convertTo0to1(0.0f));
                suppressPersistentGlobalControlsSave.store(0, std::memory_order_release);
            }
        }
    }

    const bool sceneManualControlSuppressed = isSceneManualControlHandlingSuppressed();
    ManualSceneControlHandling manualHandling = ManualSceneControlHandling::Ignored;
    if (!sceneManualControlSuppressed)
    {
        ScenePerformanceEvent sceneControlEvent;
        if (buildSceneEventFromParameterChange(parameterID, newValue, sceneControlEvent))
        {
            manualHandling = processManualSceneControlChange(sceneControlEvent.stripIndex,
                                                             sceneControlEvent.controlTarget,
                                                             static_cast<ControlMode>(sceneControlEvent.controlMode),
                                                             sceneControlEvent.controlRow,
                                                             sceneControlEvent.value,
                                                             sceneControlEvent.column);
        }
    }

    if (!isSceneAutosaveSuppressed()
        && !sceneManualControlSuppressed
        && (parameterID.startsWith("stripVolume")
            || parameterID.startsWith("stripPan")
            || parameterID.startsWith("stripSpeed"))
        && manualHandling != ManualSceneControlHandling::Recorded
        && manualHandling != ManualSceneControlHandling::OverrodeAutomation)
    {
        syncActiveSceneClipSlotRuntimeStateFromEngine(true);
    }

    if (!sceneManualControlSuppressed
        && isSceneAutosaveParameterId(parameterID)
        && manualHandling != ManualSceneControlHandling::Recorded
        && manualHandling != ManualSceneControlHandling::OverrodeAutomation)
    {
        queueActiveSceneAutosave();
    }

    if (parameterID.startsWith("stripSpeed")
        && controlModeActive
        && currentControlMode == ControlMode::Speed)
    {
        requestMonomeLedRefreshAsync();
    }

    if (!isPersistentGlobalControlParameterId(parameterID))
        return;
    persistentGlobalUserTouched.store(1, std::memory_order_release);
    queuePersistentGlobalControlsSave();
}

void MlrVSTAudioProcessor::markPersistentGlobalUserChange()
{
    persistentGlobalUserTouched.store(1, std::memory_order_release);
    persistentGlobalControlsReady.store(1, std::memory_order_release);
    queuePersistentGlobalControlsSave();
}

void MlrVSTAudioProcessor::queueActiveSceneAutosave()
{
    queueActiveSceneAutosave(0);
}

void MlrVSTAudioProcessor::queueActiveSceneAutosave(uint32_t additionalDelayMs)
{
    if (isSceneAutosaveSuppressed())
        return;

    const int mainPresetIndex = getActiveMainPresetIndexForScenes();
    const int safeSceneSlot = juce::jlimit(0, SceneSlots - 1, activeSceneSlot);
    const uint32_t nowMs = juce::Time::getMillisecondCounter();
    const uint32_t clampedDelayMs = juce::jmin<uint32_t>(additionalDelayMs, 8000u);
    const uint32_t readyMs = nowMs + kSceneAutosaveDebounceMs + clampedDelayMs;

    pendingActiveSceneAutosaveMainPreset.store(mainPresetIndex, std::memory_order_release);
    pendingActiveSceneAutosaveSlot.store(safeSceneSlot, std::memory_order_release);
    pendingActiveSceneAutosaveQueuedMs.store(readyMs, std::memory_order_release);
    pendingActiveSceneAutosaveDirty.store(1, std::memory_order_release);

    if (activeSceneMainPresetIndex == mainPresetIndex && activeSceneSlot == safeSceneSlot)
        activeSceneNeedsCaptureBeforeManualRecall = true;
}
