/*
  ==============================================================================

    PluginProcessorSceneControl.cpp
    Extracted direct scene control resolution and live override handling for mlrVST.

  ==============================================================================
*/

#include "PluginProcessor.h"
#include "PlayheadSpeedQuantizer.h"
#include <algorithm>
#include <array>
#include <cmath>

namespace
{
constexpr int kSceneAutomationGlobalMaskIndex = MlrVSTAudioProcessor::MaxStrips;

int sceneAutomationMaskIndex(int stripIndex, ScenePerformanceControlTarget target) noexcept
{
    if (target == ScenePerformanceControlTarget::Retrigger && stripIndex < 0)
        return kSceneAutomationGlobalMaskIndex;

    return juce::jlimit(0, MlrVSTAudioProcessor::MaxStrips - 1, stripIndex);
}

uint64_t sceneAutomationTargetBit(ScenePerformanceControlTarget target) noexcept
{
    if (target == ScenePerformanceControlTarget::None)
        return 0;

    return uint64_t{ 1 } << juce::jlimit(0, 63, static_cast<int>(target));
}

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

constexpr std::array<ScenePerformanceControlTarget, 24> kSceneTransitionSeedTargets{{
    ScenePerformanceControlTarget::Volume,
    ScenePerformanceControlTarget::Pan,
    ScenePerformanceControlTarget::Pitch,
    ScenePerformanceControlTarget::GrainSize,
    ScenePerformanceControlTarget::GrainDensity,
    ScenePerformanceControlTarget::GrainPitch,
    ScenePerformanceControlTarget::GrainPitchJitter,
    ScenePerformanceControlTarget::GrainSpread,
    ScenePerformanceControlTarget::GrainJitter,
    ScenePerformanceControlTarget::GrainPositionJitter,
    ScenePerformanceControlTarget::GrainRandomDepth,
    ScenePerformanceControlTarget::GrainArp,
    ScenePerformanceControlTarget::GrainCloud,
    ScenePerformanceControlTarget::GrainEmitter,
    ScenePerformanceControlTarget::GrainEnvelope,
    ScenePerformanceControlTarget::GrainShape,
    ScenePerformanceControlTarget::FilterFrequency,
    ScenePerformanceControlTarget::FilterResonance,
    ScenePerformanceControlTarget::FilterMorph,
    ScenePerformanceControlTarget::DelayMix,
    ScenePerformanceControlTarget::DelayTime,
    ScenePerformanceControlTarget::DelayFeedback,
    ScenePerformanceControlTarget::DelayLowCut,
    ScenePerformanceControlTarget::DelayHighCut
}};

bool sceneControlSupportsTransitionSmoothing(ScenePerformanceControlTarget target) noexcept
{
    return std::find(kSceneTransitionSeedTargets.begin(),
                     kSceneTransitionSeedTargets.end(),
                     target) != kSceneTransitionSeedTargets.end();
}

bool sceneAutomationTargetHasEditorLane(ScenePerformanceControlTarget target) noexcept
{
    switch (target)
    {
        case ScenePerformanceControlTarget::Volume:
        case ScenePerformanceControlTarget::Pan:
        case ScenePerformanceControlTarget::Pitch:
        case ScenePerformanceControlTarget::FilterFrequency:
        case ScenePerformanceControlTarget::FilterResonance:
        case ScenePerformanceControlTarget::FilterMorph:
        case ScenePerformanceControlTarget::Speed:
        case ScenePerformanceControlTarget::Retrigger:
        case ScenePerformanceControlTarget::SliceLength:
        case ScenePerformanceControlTarget::Scratch:
        case ScenePerformanceControlTarget::DelayMix:
        case ScenePerformanceControlTarget::DelayTime:
        case ScenePerformanceControlTarget::DelayFeedback:
        case ScenePerformanceControlTarget::GrainPitch:
        case ScenePerformanceControlTarget::GrainSize:
        case ScenePerformanceControlTarget::GrainDensity:
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
            return true;
        case ScenePerformanceControlTarget::None:
        case ScenePerformanceControlTarget::Swing:
        case ScenePerformanceControlTarget::FilterEnabled:
        case ScenePerformanceControlTarget::DelayLowCut:
        case ScenePerformanceControlTarget::DelayHighCut:
        case ScenePerformanceControlTarget::DelayMode:
        case ScenePerformanceControlTarget::DelaySyncEnabled:
        case ScenePerformanceControlTarget::Rearrange:
        default:
            return false;
    }
}

float denormalizeMacroLinearValue(float normalized, float minValue, float maxValue)
{
    return juce::jmap(juce::jlimit(0.0f, 1.0f, normalized), 0.0f, 1.0f, minValue, maxValue);
}

float denormalizeMacroCutoffValue(float normalized)
{
    juce::NormalisableRange<float> range(20.0f, 20000.0f, 1.0f);
    range.setSkewForCentre(1000.0f);
    return juce::jlimit(20.0f, 20000.0f, range.convertFrom0to1(juce::jlimit(0.0f, 1.0f, normalized)));
}

float denormalizeMacroPitchValue(float normalized)
{
    return denormalizeMacroLinearValue(normalized, -24.0f, 24.0f);
}

bool buildDirectSceneControlProbeEvent(int stripIndex,
                                       ScenePerformanceControlTarget target,
                                       MlrVSTAudioProcessor::ControlMode controlMode,
                                       int controlRow,
                                       float value,
                                       int columnHint,
                                       ScenePerformanceEvent& outEvent)
{
    if (target == ScenePerformanceControlTarget::None)
        return false;

    outEvent = {};
    outEvent.type = ScenePerformanceEventType::ControlPoint;
    outEvent.stripIndex = (target == ScenePerformanceControlTarget::Retrigger && stripIndex < 0)
        ? -1
        : juce::jlimit(0, MlrVSTAudioProcessor::MaxStrips - 1, stripIndex);
    outEvent.controlTarget = target;
    outEvent.controlMode = static_cast<int>(controlMode);
    outEvent.controlRow = controlRow;
    outEvent.value = value;
    outEvent.column = columnHint >= 0
        ? juce::jlimit(0, MlrVSTAudioProcessor::MaxColumns - 1, columnHint)
        : juce::jlimit(0,
                       MlrVSTAudioProcessor::MaxColumns - 1,
                       static_cast<int>(std::round(normalizeSceneControlValue(outEvent) * 15.0f)));
    return true;
}
} // namespace

bool MlrVSTAudioProcessor::resolveScenePerformanceControlEvent(ControlMode mode,
                                                               int stripIndex,
                                                               int controlRow,
                                                               int column,
                                                               ScenePerformanceEvent& outEvent) const
{
    if (audioEngine == nullptr)
        return false;

    const int safeStripIndex = juce::jlimit(0, MaxStrips - 1, stripIndex);
    const int safeColumn = juce::jlimit(0, MaxColumns - 1, column);
    auto* strip = audioEngine->getStrip(safeStripIndex);
    if (strip == nullptr)
        return false;

    outEvent = {};
    outEvent.type = ScenePerformanceEventType::ControlPoint;
    outEvent.stripIndex = safeStripIndex;
    outEvent.column = safeColumn;
    outEvent.controlMode = static_cast<int>(mode);
    outEvent.controlRow = controlRow;

    switch (mode)
    {
        case ControlMode::Speed:
            outEvent.controlTarget = ScenePerformanceControlTarget::Speed;
            outEvent.value = PlayheadSpeedQuantizer::monomeSpeedControlValueFromColumn(
                safeColumn,
                strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Grain);
            return true;

        case ControlMode::Pitch:
        {
            static constexpr std::array<float, 16> kMusicalPitchSemitones{
                -24.0f, -21.0f, -18.0f, -15.0f, -12.0f, -9.0f, -6.0f, -3.0f,
                  0.0f,   3.0f,   6.0f,   9.0f,  12.0f, 15.0f, 18.0f, 24.0f
            };
            outEvent.controlTarget = ScenePerformanceControlTarget::Pitch;
            outEvent.value = kMusicalPitchSemitones[static_cast<size_t>(safeColumn)];
            return true;
        }

        case ControlMode::Pan:
            outEvent.controlTarget = ScenePerformanceControlTarget::Pan;
            outEvent.value = juce::jlimit(-1.0f, 1.0f, (safeColumn - 8) / 8.0f);
            return true;

        case ControlMode::Volume:
            outEvent.controlTarget = ScenePerformanceControlTarget::Volume;
            outEvent.value = juce::jlimit(0.0f, 1.0f, safeColumn / 15.0f);
            return true;

        case ControlMode::Swing:
            return false;

        case ControlMode::GrainSize:
        {
            const float unit = juce::jlimit(0.0f, 1.0f, safeColumn / 15.0f);
            switch (juce::jlimit(0, 5, controlRow))
            {
                case 0:
                    outEvent.controlTarget = ScenePerformanceControlTarget::GrainSize;
                    outEvent.value = 5.0f + (unit * (2400.0f - 5.0f));
                    return true;
                case 1:
                    outEvent.controlTarget = ScenePerformanceControlTarget::GrainDensity;
                    outEvent.value = 0.05f + (unit * (0.9f - 0.05f));
                    return true;
                case 2:
                    outEvent.controlTarget = ScenePerformanceControlTarget::Pitch;
                    outEvent.value = -24.0f + (unit * 48.0f);
                    return true;
                default:
                    return false;
            }
        }

        case ControlMode::Delay:
        {
            switch (juce::jlimit(0, 5, controlRow))
            {
                case 0:
                    outEvent.controlTarget = ScenePerformanceControlTarget::DelayMix;
                    outEvent.value = juce::jlimit(0.0f, 1.0f, safeColumn / 15.0f);
                    return true;
                case 1:
                {
                    const bool syncEnabled = strip->isDelaySyncEnabled();
                    const juce::NormalisableRange<float> syncRange(0.25f, 4.0f, 0.001f, 0.45f);
                    const juce::NormalisableRange<float> freeRange(0.25f, 4.0f, 0.001f, 0.4f);
                    outEvent.controlTarget = ScenePerformanceControlTarget::DelayTime;
                    outEvent.value = (syncEnabled ? syncRange : freeRange).convertFrom0to1(
                        juce::jlimit(0.0f, 1.0f, safeColumn / 15.0f));
                    return true;
                }
                case 2:
                    outEvent.controlTarget = ScenePerformanceControlTarget::DelayFeedback;
                    outEvent.value = 0.97f * juce::jlimit(0.0f, 1.0f, safeColumn / 15.0f);
                    return true;
                case 3:
                case 4:
                case 5:
                default:
                    return false;
            }
        }

        case ControlMode::Filter:
            if (controlRow == 0)
            {
                const float t = juce::jlimit(0.0f, 1.0f, safeColumn / 15.0f);
                outEvent.controlTarget = ScenePerformanceControlTarget::FilterFrequency;
                outEvent.value = 20.0f * std::pow(1000.0f, t);
                return true;
            }
            if (controlRow == 1)
            {
                outEvent.controlTarget = ScenePerformanceControlTarget::FilterResonance;
                outEvent.value = 0.1f + (juce::jlimit(0.0f, 15.0f, static_cast<float>(safeColumn)) / 15.0f) * 9.9f;
                return true;
            }
            if (controlRow == 2 && safeColumn <= 2)
            {
                outEvent.controlTarget = ScenePerformanceControlTarget::FilterMorph;
                outEvent.value = (safeColumn <= 0) ? 0.0f : (safeColumn == 1 ? 0.5f : 1.0f);
                return true;
            }
            return false;

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

bool MlrVSTAudioProcessor::resolveScenePerformanceMacroEvent(int stripIndex,
                                                             MacroTarget target,
                                                             float normalizedValue,
                                                             ScenePerformanceEvent& outEvent) const
{
    if (audioEngine == nullptr)
        return false;

    const int safeStripIndex = juce::jlimit(0, MaxStrips - 1, stripIndex);
    auto* strip = audioEngine->getStrip(safeStripIndex);
    if (strip == nullptr)
        return false;

    const auto safeTarget = sanitizeMacroPerformanceTarget(target);
    const float clamped = juce::jlimit(0.0f, 1.0f, normalizedValue);

    outEvent = {};
    outEvent.type = ScenePerformanceEventType::ControlPoint;
    outEvent.stripIndex = safeStripIndex;
    outEvent.column = juce::jlimit(0, MaxColumns - 1, static_cast<int>(std::round(clamped * 15.0f)));

    switch (safeTarget)
    {
        case MacroTarget::Speed:
            outEvent.controlMode = static_cast<int>(ControlMode::Speed);
            outEvent.controlTarget = ScenePerformanceControlTarget::Speed;
            outEvent.value = denormalizeMacroLinearValue(clamped, 0.125f, 8.0f);
            return true;

        case MacroTarget::Pitch:
            outEvent.controlMode = static_cast<int>(ControlMode::Pitch);
            outEvent.controlTarget = ScenePerformanceControlTarget::Pitch;
            outEvent.value = denormalizeMacroPitchValue(clamped);
            return true;

        case MacroTarget::Pan:
            outEvent.controlMode = static_cast<int>(ControlMode::Pan);
            outEvent.controlTarget = ScenePerformanceControlTarget::Pan;
            outEvent.value = denormalizeMacroLinearValue(clamped, -1.0f, 1.0f);
            return true;

        case MacroTarget::Volume:
            outEvent.controlMode = static_cast<int>(ControlMode::Volume);
            outEvent.controlTarget = ScenePerformanceControlTarget::Volume;
            outEvent.value = clamped;
            return true;

        case MacroTarget::Cutoff:
            outEvent.controlMode = static_cast<int>(ControlMode::Filter);
            outEvent.controlRow = 0;
            outEvent.controlTarget = ScenePerformanceControlTarget::FilterFrequency;
            outEvent.value = denormalizeMacroCutoffValue(clamped);
            return true;

        case MacroTarget::Resonance:
            outEvent.controlMode = static_cast<int>(ControlMode::Filter);
            outEvent.controlRow = 1;
            outEvent.controlTarget = ScenePerformanceControlTarget::FilterResonance;
            outEvent.value = denormalizeMacroLinearValue(clamped, 0.1f, 10.0f);
            return true;

        case MacroTarget::FilterMorph:
            outEvent.controlMode = static_cast<int>(ControlMode::Filter);
            outEvent.controlRow = 2;
            outEvent.controlTarget = ScenePerformanceControlTarget::FilterMorph;
            outEvent.value = clamped;
            return true;

        case MacroTarget::FilterEnable:
            return false;

        case MacroTarget::SliceLength:
            outEvent.controlMode = static_cast<int>(ControlMode::Normal);
            outEvent.controlRow = 0;
            outEvent.controlTarget = ScenePerformanceControlTarget::SliceLength;
            outEvent.value = juce::jlimit(0.02f, 1.0f, clamped);
            return true;

        case MacroTarget::Scratch:
            outEvent.controlMode = static_cast<int>(ControlMode::Normal);
            outEvent.controlRow = 1;
            outEvent.controlTarget = ScenePerformanceControlTarget::Scratch;
            outEvent.value = clamped * 100.0f;
            return true;

        case MacroTarget::DelayMix:
            outEvent.controlMode = static_cast<int>(ControlMode::Delay);
            outEvent.controlRow = 0;
            outEvent.controlTarget = ScenePerformanceControlTarget::DelayMix;
            outEvent.value = clamped;
            return true;

        case MacroTarget::DelayTime:
            outEvent.controlMode = static_cast<int>(ControlMode::Delay);
            outEvent.controlRow = 1;
            outEvent.controlTarget = ScenePerformanceControlTarget::DelayTime;
            outEvent.value = denormalizeMacroLinearValue(clamped, 0.25f, 4.0f);
            return true;

        case MacroTarget::DelayFeedback:
            outEvent.controlMode = static_cast<int>(ControlMode::Delay);
            outEvent.controlRow = 2;
            outEvent.controlTarget = ScenePerformanceControlTarget::DelayFeedback;
            outEvent.value = denormalizeMacroLinearValue(clamped, 0.0f, 0.97f);
            return true;

        case MacroTarget::DelayLowCut:
        case MacroTarget::DelayHighCut:
            return false;

        case MacroTarget::GrainSize:
            outEvent.controlMode = static_cast<int>(ControlMode::GrainSize);
            outEvent.controlRow = 0;
            outEvent.controlTarget = ScenePerformanceControlTarget::GrainSize;
            outEvent.value = denormalizeMacroLinearValue(clamped, 5.0f, 2400.0f);
            return true;

        case MacroTarget::GrainDensity:
            outEvent.controlMode = static_cast<int>(ControlMode::GrainSize);
            outEvent.controlRow = 1;
            outEvent.controlTarget = ScenePerformanceControlTarget::GrainDensity;
            outEvent.value = denormalizeMacroLinearValue(clamped, 0.05f, 0.9f);
            return true;

        case MacroTarget::GrainPitch:
            outEvent.controlMode = static_cast<int>(ControlMode::GrainSize);
            outEvent.controlRow = 2;
            outEvent.controlTarget = ScenePerformanceControlTarget::GrainPitch;
            outEvent.value = denormalizeMacroLinearValue(clamped, -48.0f, 48.0f);
            return true;

        case MacroTarget::GrainPitchJitter:
            outEvent.controlMode = static_cast<int>(ControlMode::GrainSize);
            outEvent.controlRow = 3;
            outEvent.controlTarget = ScenePerformanceControlTarget::GrainPitchJitter;
            outEvent.value = denormalizeMacroLinearValue(clamped, 0.0f, 48.0f);
            return true;

        case MacroTarget::GrainSpread:
            outEvent.controlMode = static_cast<int>(ControlMode::GrainSize);
            outEvent.controlRow = 3;
            outEvent.controlTarget = ScenePerformanceControlTarget::GrainSpread;
            outEvent.value = clamped;
            return true;

        case MacroTarget::GrainJitter:
            outEvent.controlMode = static_cast<int>(ControlMode::GrainSize);
            outEvent.controlRow = 4;
            outEvent.controlTarget = ScenePerformanceControlTarget::GrainJitter;
            outEvent.value = clamped;
            return true;

        case MacroTarget::GrainPositionJitter:
            outEvent.controlMode = static_cast<int>(ControlMode::GrainSize);
            outEvent.controlRow = 4;
            outEvent.controlTarget = ScenePerformanceControlTarget::GrainPositionJitter;
            outEvent.value = clamped;
            return true;

        case MacroTarget::GrainRandom:
            outEvent.controlMode = static_cast<int>(ControlMode::GrainSize);
            outEvent.controlRow = 4;
            outEvent.controlTarget = ScenePerformanceControlTarget::GrainRandomDepth;
            outEvent.value = clamped;
            return true;

        case MacroTarget::GrainArp:
            outEvent.controlMode = static_cast<int>(ControlMode::GrainSize);
            outEvent.controlRow = 4;
            outEvent.controlTarget = ScenePerformanceControlTarget::GrainArp;
            outEvent.value = clamped;
            return true;

        case MacroTarget::GrainCloud:
            outEvent.controlMode = static_cast<int>(ControlMode::GrainSize);
            outEvent.controlRow = 4;
            outEvent.controlTarget = ScenePerformanceControlTarget::GrainCloud;
            outEvent.value = clamped;
            return true;

        case MacroTarget::GrainEmitter:
            outEvent.controlMode = static_cast<int>(ControlMode::GrainSize);
            outEvent.controlRow = 4;
            outEvent.controlTarget = ScenePerformanceControlTarget::GrainEmitter;
            outEvent.value = clamped;
            return true;

        case MacroTarget::GrainEnvelope:
            outEvent.controlMode = static_cast<int>(ControlMode::GrainSize);
            outEvent.controlRow = 5;
            outEvent.controlTarget = ScenePerformanceControlTarget::GrainEnvelope;
            outEvent.value = clamped;
            return true;

        case MacroTarget::GrainShape:
            outEvent.controlMode = static_cast<int>(ControlMode::GrainSize);
            outEvent.controlRow = 5;
            outEvent.controlTarget = ScenePerformanceControlTarget::GrainShape;
            outEvent.value = denormalizeMacroLinearValue(clamped, -1.0f, 1.0f);
            return true;

        case MacroTarget::Retrigger:
            outEvent.controlMode = static_cast<int>(ControlMode::Normal);
            outEvent.controlRow = 2;
            outEvent.stripIndex = -1;
            outEvent.controlTarget = ScenePerformanceControlTarget::Retrigger;
            outEvent.value = clamped;
            return true;

        case MacroTarget::Rearrange:
            return false;

        case MacroTarget::None:
        default:
            return false;
    }
}

void MlrVSTAudioProcessor::recordMacroSceneEvent(int stripIndex, MacroTarget target, float normalizedValue)
{
    if (!isSceneModeEnabled() || !scenePerformanceRecorder.isRecording() || audioEngine == nullptr)
        return;

    if (!macroTargetWritesToSceneLane(target))
        return;

    ScenePerformanceEvent event;
    if (!resolveScenePerformanceMacroEvent(stripIndex, target, normalizedValue, event))
        return;

    const int sceneSlot = juce::jlimit(0, SceneSlots - 1, activeSceneSlot);
    scenePerformanceRecorder.recordControlEvent(sceneSlot,
                                                event.stripIndex,
                                                event.controlMode,
                                                event.controlRow,
                                                event.column,
                                                event.controlTarget,
                                                event.value,
                                                audioEngine->getTimelineBeat());
    clearActiveSceneAutomationOverrideForRecordedTarget(sceneSlot, event.stripIndex, event.controlTarget);
}

void MlrVSTAudioProcessor::recordMonomeControlSceneEvent(ControlMode mode,
                                                         int targetStripIndex,
                                                         int controlRow,
                                                         int column)
{
    if (!isSceneModeEnabled() || !scenePerformanceRecorder.isRecording())
        return;

    ScenePerformanceEvent event;
    if (!resolveScenePerformanceControlEvent(mode, targetStripIndex, controlRow, column, event))
        return;

    const int sceneSlot = juce::jlimit(0, SceneSlots - 1, activeSceneSlot);
    scenePerformanceRecorder.recordControlEvent(sceneSlot,
                                                event.stripIndex,
                                                event.controlMode,
                                                event.controlRow,
                                                event.column,
                                                event.controlTarget,
                                                event.value,
                                                audioEngine != nullptr ? audioEngine->getTimelineBeat() : 0.0);
    clearActiveSceneAutomationOverrideForRecordedTarget(sceneSlot, event.stripIndex, event.controlTarget);
}

MlrVSTAudioProcessor::ManualSceneControlHandling
MlrVSTAudioProcessor::handleLiveSceneControlTouch(int stripIndex,
                                                  ScenePerformanceControlTarget target,
                                                  ControlMode controlMode,
                                                  int controlRow,
                                                  float value,
                                                  int columnHint,
                                                  const std::function<void(StripControlWriteMode)>& applyLiveValue,
                                                  bool liveValueAlreadyApplied)
{
    ScenePerformanceEvent probeEvent;
    if (!buildDirectSceneControlProbeEvent(stripIndex,
                                           target,
                                           controlMode,
                                           controlRow,
                                           value,
                                           columnHint,
                                           probeEvent))
    {
        if (!liveValueAlreadyApplied && applyLiveValue)
            applyLiveValue(StripControlWriteMode::NotifyHost);
        return ManualSceneControlHandling::Ignored;
    }

    float liveValueBeforeTouch = 0.0f;
    const bool shouldSeedTransition = !liveValueAlreadyApplied
        && sceneControlSupportsTransitionSmoothing(target)
        && getSceneControlCurrentValue(probeEvent.stripIndex, target, liveValueBeforeTouch);

    auto applyLiveTouch = [this, &applyLiveValue](StripControlWriteMode writeMode)
    {
        if (!applyLiveValue)
            return;

        beginSceneManualControlHandlingSuppression();
        applyLiveValue(writeMode);
        endSceneManualControlHandlingSuppression();
    };

    auto seedTransitionIfNeeded = [this,
                                   target,
                                   &probeEvent,
                                   shouldSeedTransition,
                                   liveValueBeforeTouch]()
    {
        if (!shouldSeedTransition)
            return;

        seedSceneControlTransition(probeEvent.stripIndex,
                                   target,
                                   liveValueBeforeTouch,
                                   probeEvent.value,
                                   getSceneAutomationTransitionSeconds(target));
    };

    auto refreshStutterBaseline = [this, &probeEvent]()
    {
        refreshMomentaryStutterSavedStateFromCurrentStrip(probeEvent.stripIndex, probeEvent.controlTarget);
    };

    if (!sceneAutomationTargetHasEditorLane(probeEvent.controlTarget))
    {
        if (!liveValueAlreadyApplied)
        {
            applyLiveTouch(StripControlWriteMode::NotifyHost);
            seedTransitionIfNeeded();
        }

        refreshStutterBaseline();
        return ManualSceneControlHandling::Ignored;
    }

    if (!isSceneModeEnabled() || audioEngine == nullptr)
    {
        if (!liveValueAlreadyApplied)
        {
            applyLiveTouch(StripControlWriteMode::NotifyHost);
            seedTransitionIfNeeded();
        }
        else
        {
            refreshStutterBaseline();
        }

        if (!liveValueAlreadyApplied)
            refreshStutterBaseline();
        return ManualSceneControlHandling::Ignored;
    }

    const int sceneSlot = juce::jlimit(0, SceneSlots - 1, activeSceneSlot);
    if (scenePerformanceRecorder.isRecording())
    {
        if (!liveValueAlreadyApplied)
        {
            applyLiveTouch(StripControlWriteMode::NotifyHost);
            seedTransitionIfNeeded();
        }
        else
        {
            refreshStutterBaseline();
        }

        if (!liveValueAlreadyApplied)
            refreshStutterBaseline();

        scenePerformanceRecorder.recordControlEvent(sceneSlot,
                                                    probeEvent.stripIndex,
                                                    probeEvent.controlMode,
                                                    probeEvent.controlRow,
                                                    probeEvent.column,
                                                    probeEvent.controlTarget,
                                                    probeEvent.value,
                                                    audioEngine->getTimelineBeat());
        clearActiveSceneAutomationOverrideForRecordedTarget(sceneSlot,
                                                            probeEvent.stripIndex,
                                                            probeEvent.controlTarget);
        return ManualSceneControlHandling::Recorded;
    }

    if (sceneClipHasAutomationTarget(sceneSlot, probeEvent.stripIndex, target))
    {
        const auto maskIndex = static_cast<size_t>(sceneAutomationMaskIndex(probeEvent.stripIndex, target));
        activeSceneAutomationOverrideMasks[maskIndex].fetch_or(sceneAutomationTargetBit(target),
                                                               std::memory_order_acq_rel);

        if (liveValueAlreadyApplied)
        {
            // Reassert the touched live value after the override owner is active
            // so GUI touches, macros, and Monome writes all land the same way.
            beginSceneManualControlHandlingSuppression();
            applyScenePerformanceEvent(probeEvent, StripControlWriteMode::CacheOnly);
            endSceneManualControlHandlingSuppression();
            refreshStutterBaseline();
        }
        else
        {
            applyLiveTouch(StripControlWriteMode::NotifyHost);
            seedTransitionIfNeeded();
            refreshStutterBaseline();
        }

        return ManualSceneControlHandling::OverrodeAutomation;
    }

    if (!liveValueAlreadyApplied)
    {
        applyLiveTouch(StripControlWriteMode::NotifyHost);
        seedTransitionIfNeeded();
        refreshStutterBaseline();
    }
    else
    {
        refreshStutterBaseline();
    }

    if (target == ScenePerformanceControlTarget::Volume
        || target == ScenePerformanceControlTarget::Pan
        || target == ScenePerformanceControlTarget::Speed)
    {
        syncActiveSceneClipSlotRuntimeStateFromEngine(true);
    }

    queueActiveSceneAutosave();
    return ManualSceneControlHandling::PassedThrough;
}

MlrVSTAudioProcessor::ManualSceneControlHandling
MlrVSTAudioProcessor::processManualSceneControlChange(int stripIndex,
                                                      ScenePerformanceControlTarget target,
                                                      ControlMode controlMode,
                                                      int controlRow,
                                                      float value,
                                                      int columnHint)
{
    return handleLiveSceneControlTouch(stripIndex,
                                       target,
                                       controlMode,
                                       controlRow,
                                       value,
                                       columnHint,
                                       {},
                                       true);
}

void MlrVSTAudioProcessor::applyLiveSceneControlTouch(int stripIndex,
                                                      ScenePerformanceControlTarget target,
                                                      ControlMode controlMode,
                                                      int controlRow,
                                                      float value,
                                                      int columnHint,
                                                      const std::function<void(StripControlWriteMode)>& applyLiveValue,
                                                      bool liveValueAlreadyApplied)
{
    juce::ignoreUnused(handleLiveSceneControlTouch(stripIndex,
                                                   target,
                                                   controlMode,
                                                   controlRow,
                                                   value,
                                                   columnHint,
                                                   applyLiveValue,
                                                   liveValueAlreadyApplied));
}

void MlrVSTAudioProcessor::notifyDirectSceneControlChange(int stripIndex,
                                                          ScenePerformanceControlTarget target,
                                                          ControlMode controlMode,
                                                          int controlRow,
                                                          float value,
                                                          int columnHint)
{
    applyLiveSceneControlTouch(stripIndex,
                               target,
                               controlMode,
                               controlRow,
                               value,
                               columnHint,
                               {},
                               true);
}
