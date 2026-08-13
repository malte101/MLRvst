#include "SceneAutomationRules.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace SceneAutomationRules
{
constexpr float kSceneContinuousControlThinValueThreshold = 0.015f;
constexpr float kSceneDiscreteControlThinValueThreshold = 1.0e-4f;

float normalizeValue(const ScenePerformanceEvent& event)
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

float normalizeValue(ScenePerformanceControlTarget target, float value)
{
    ScenePerformanceEvent event;
    event.type = ScenePerformanceEventType::ControlPoint;
    event.controlTarget = target;
    event.value = value;
    return normalizeValue(event);
}

float denormalizeValue(const ScenePerformanceEvent& event, float normalizedValue)
{
    const float t = juce::jlimit(0.0f, 1.0f, normalizedValue);

    switch (event.controlTarget)
    {
        case ScenePerformanceControlTarget::Speed:
            return juce::jlimit(0.125f, 8.0f, std::pow(2.0f, -3.0f + (t * 6.0f)));
        case ScenePerformanceControlTarget::Pitch:
            return -24.0f + (t * 48.0f);
        case ScenePerformanceControlTarget::GrainPitch:
            return -48.0f + (t * 96.0f);
        case ScenePerformanceControlTarget::Pan:
            return (t * 2.0f) - 1.0f;
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
            return t;
        case ScenePerformanceControlTarget::SliceLength:
            return 0.02f + (t * 0.98f);
        case ScenePerformanceControlTarget::Scratch:
            return 100.0f * t;
        case ScenePerformanceControlTarget::GrainSize:
            return 5.0f + (t * (2400.0f - 5.0f));
        case ScenePerformanceControlTarget::GrainDensity:
            return 0.05f + (t * (0.9f - 0.05f));
        case ScenePerformanceControlTarget::GrainPitchJitter:
            return 48.0f * t;
        case ScenePerformanceControlTarget::GrainShape:
            return (t * 2.0f) - 1.0f;
        case ScenePerformanceControlTarget::FilterFrequency:
            return 20.0f * std::pow(1000.0f, t);
        case ScenePerformanceControlTarget::FilterResonance:
            return 0.1f + (t * 9.9f);
        case ScenePerformanceControlTarget::DelayTime:
            return 0.25f + (t * (4.0f - 0.25f));
        case ScenePerformanceControlTarget::DelayFeedback:
            return 0.97f * t;
        case ScenePerformanceControlTarget::DelayLowCut:
        {
            const juce::NormalisableRange<float> range(20.0f, 12000.0f, 1.0f, 0.25f);
            return range.convertFrom0to1(t);
        }
        case ScenePerformanceControlTarget::DelayHighCut:
        {
            const juce::NormalisableRange<float> range(200.0f, 20000.0f, 1.0f, 0.3f);
            return range.convertFrom0to1(t);
        }
        case ScenePerformanceControlTarget::DelayMode:
            return static_cast<float>(juce::jlimit(0, 2, static_cast<int>(std::round(t * 2.0f))));
        case ScenePerformanceControlTarget::DelaySyncEnabled:
            return t >= 0.5f ? 1.0f : 0.0f;
        case ScenePerformanceControlTarget::None:
        default:
            return event.value;
    }
}

float defaultValue(ScenePerformanceControlTarget target) noexcept
{
    switch (target)
    {
        case ScenePerformanceControlTarget::Speed:               return 1.0f;
        case ScenePerformanceControlTarget::Pitch:               return 0.0f;
        case ScenePerformanceControlTarget::GrainPitch:          return 0.0f;
        case ScenePerformanceControlTarget::Pan:                 return 0.0f;
        case ScenePerformanceControlTarget::Volume:              return 1.0f;
        case ScenePerformanceControlTarget::Swing:               return 0.0f;
        case ScenePerformanceControlTarget::GrainSize:           return 1240.0f;
        case ScenePerformanceControlTarget::GrainDensity:        return 0.05f;
        case ScenePerformanceControlTarget::GrainPitchJitter:    return 0.0f;
        case ScenePerformanceControlTarget::GrainSpread:         return 0.0f;
        case ScenePerformanceControlTarget::GrainJitter:         return 0.0f;
        case ScenePerformanceControlTarget::GrainPositionJitter: return 0.0f;
        case ScenePerformanceControlTarget::GrainRandomDepth:    return 0.0f;
        case ScenePerformanceControlTarget::GrainArp:            return 0.0f;
        case ScenePerformanceControlTarget::GrainCloud:          return 0.0f;
        case ScenePerformanceControlTarget::GrainEmitter:        return 0.0f;
        case ScenePerformanceControlTarget::GrainEnvelope:       return 0.0f;
        case ScenePerformanceControlTarget::GrainShape:          return 0.0f;
        case ScenePerformanceControlTarget::FilterFrequency:     return 20000.0f;
        case ScenePerformanceControlTarget::FilterResonance:     return 0.707f;
        case ScenePerformanceControlTarget::FilterEnabled:       return 0.0f;
        case ScenePerformanceControlTarget::FilterMorph:         return 0.0f;
        case ScenePerformanceControlTarget::SliceLength:         return 1.0f;
        case ScenePerformanceControlTarget::Scratch:             return 0.0f;
        case ScenePerformanceControlTarget::DelayMix:            return 0.0f;
        case ScenePerformanceControlTarget::DelayTime:           return 0.25f;
        case ScenePerformanceControlTarget::DelayFeedback:       return 0.0f;
        case ScenePerformanceControlTarget::DelayLowCut:         return 20.0f;
        case ScenePerformanceControlTarget::DelayHighCut:        return 20000.0f;
        case ScenePerformanceControlTarget::DelayMode:           return 0.0f;
        case ScenePerformanceControlTarget::DelaySyncEnabled:    return 0.0f;
        case ScenePerformanceControlTarget::Retrigger:           return 0.0f;
        case ScenePerformanceControlTarget::Rearrange:           return 0.0f;
        case ScenePerformanceControlTarget::None:
        default:
            return 0.0f;
    }
}

float valueThinThreshold(ScenePerformanceControlTarget target) noexcept
{
    switch (target)
    {
        case ScenePerformanceControlTarget::Speed:
        case ScenePerformanceControlTarget::Pitch:
        case ScenePerformanceControlTarget::Pan:
        case ScenePerformanceControlTarget::Volume:
        case ScenePerformanceControlTarget::Swing:
        case ScenePerformanceControlTarget::GrainSize:
        case ScenePerformanceControlTarget::GrainDensity:
        case ScenePerformanceControlTarget::GrainPitch:
        case ScenePerformanceControlTarget::GrainJitter:
        case ScenePerformanceControlTarget::GrainRandomDepth:
        case ScenePerformanceControlTarget::GrainEnvelope:
        case ScenePerformanceControlTarget::FilterFrequency:
        case ScenePerformanceControlTarget::FilterResonance:
        case ScenePerformanceControlTarget::FilterMorph:
        case ScenePerformanceControlTarget::SliceLength:
        case ScenePerformanceControlTarget::Scratch:
        case ScenePerformanceControlTarget::DelayMix:
        case ScenePerformanceControlTarget::DelayTime:
        case ScenePerformanceControlTarget::DelayFeedback:
        case ScenePerformanceControlTarget::DelayLowCut:
        case ScenePerformanceControlTarget::DelayHighCut:
        case ScenePerformanceControlTarget::Retrigger:
        case ScenePerformanceControlTarget::Rearrange:
        case ScenePerformanceControlTarget::GrainPitchJitter:
        case ScenePerformanceControlTarget::GrainSpread:
        case ScenePerformanceControlTarget::GrainPositionJitter:
        case ScenePerformanceControlTarget::GrainArp:
        case ScenePerformanceControlTarget::GrainCloud:
        case ScenePerformanceControlTarget::GrainEmitter:
        case ScenePerformanceControlTarget::GrainShape:
            return kSceneContinuousControlThinValueThreshold;
        case ScenePerformanceControlTarget::FilterEnabled:
        case ScenePerformanceControlTarget::DelayMode:
        case ScenePerformanceControlTarget::DelaySyncEnabled:
            return kSceneDiscreteControlThinValueThreshold;
        case ScenePerformanceControlTarget::None:
            return 0.0f;
    }

    return kSceneContinuousControlThinValueThreshold;
}

MlrVSTAudioProcessor::ControlMode controlModeForTarget(ScenePerformanceControlTarget target) noexcept
{
    using ControlMode = MlrVSTAudioProcessor::ControlMode;

    switch (target)
    {
        case ScenePerformanceControlTarget::Speed:
            return ControlMode::Speed;
        case ScenePerformanceControlTarget::Pitch:
        case ScenePerformanceControlTarget::GrainPitch:
            return ControlMode::Pitch;
        case ScenePerformanceControlTarget::Pan:
            return ControlMode::Pan;
        case ScenePerformanceControlTarget::Volume:
            return ControlMode::Volume;
        case ScenePerformanceControlTarget::Swing:
            return ControlMode::Swing;
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
            return ControlMode::GrainSize;
        case ScenePerformanceControlTarget::FilterFrequency:
        case ScenePerformanceControlTarget::FilterResonance:
        case ScenePerformanceControlTarget::FilterEnabled:
        case ScenePerformanceControlTarget::FilterMorph:
            return ControlMode::Filter;
        case ScenePerformanceControlTarget::DelayMix:
        case ScenePerformanceControlTarget::DelayTime:
        case ScenePerformanceControlTarget::DelayFeedback:
        case ScenePerformanceControlTarget::DelayLowCut:
        case ScenePerformanceControlTarget::DelayHighCut:
        case ScenePerformanceControlTarget::DelayMode:
        case ScenePerformanceControlTarget::DelaySyncEnabled:
            return ControlMode::Delay;
        case ScenePerformanceControlTarget::SliceLength:
        case ScenePerformanceControlTarget::Scratch:
        case ScenePerformanceControlTarget::Retrigger:
        case ScenePerformanceControlTarget::Rearrange:
        case ScenePerformanceControlTarget::None:
        default:
            return ControlMode::Normal;
    }
}

int controlRowForTarget(ScenePerformanceControlTarget target) noexcept
{
    switch (target)
    {
        case ScenePerformanceControlTarget::FilterFrequency:  return 0;
        case ScenePerformanceControlTarget::FilterResonance:  return 1;
        case ScenePerformanceControlTarget::FilterMorph:      return 2;
        case ScenePerformanceControlTarget::FilterEnabled:    return 3;
        case ScenePerformanceControlTarget::DelayMix:         return 0;
        case ScenePerformanceControlTarget::DelayTime:        return 1;
        case ScenePerformanceControlTarget::DelayFeedback:    return 2;
        case ScenePerformanceControlTarget::DelayLowCut:      return 3;
        case ScenePerformanceControlTarget::DelayHighCut:     return 4;
        case ScenePerformanceControlTarget::DelayMode:
        case ScenePerformanceControlTarget::DelaySyncEnabled: return 5;
        case ScenePerformanceControlTarget::None:
        case ScenePerformanceControlTarget::Speed:
        case ScenePerformanceControlTarget::Pitch:
        case ScenePerformanceControlTarget::Pan:
        case ScenePerformanceControlTarget::Volume:
        case ScenePerformanceControlTarget::Swing:
        case ScenePerformanceControlTarget::GrainSize:
        case ScenePerformanceControlTarget::GrainDensity:
        case ScenePerformanceControlTarget::GrainPitch:
        case ScenePerformanceControlTarget::GrainJitter:
        case ScenePerformanceControlTarget::GrainRandomDepth:
        case ScenePerformanceControlTarget::GrainEnvelope:
        case ScenePerformanceControlTarget::SliceLength:
        case ScenePerformanceControlTarget::Scratch:
        case ScenePerformanceControlTarget::Retrigger:
        case ScenePerformanceControlTarget::Rearrange:
        case ScenePerformanceControlTarget::GrainPitchJitter:
        case ScenePerformanceControlTarget::GrainSpread:
        case ScenePerformanceControlTarget::GrainPositionJitter:
        case ScenePerformanceControlTarget::GrainArp:
        case ScenePerformanceControlTarget::GrainCloud:
        case ScenePerformanceControlTarget::GrainEmitter:
        case ScenePerformanceControlTarget::GrainShape:
        default:
            return 0;
    }
}

bool targetUsesGlobalStrip(ScenePerformanceControlTarget target) noexcept
{
    return target == ScenePerformanceControlTarget::Retrigger;
}

bool targetUsesSteppedSegments(ScenePerformanceControlTarget target) noexcept
{
    switch (target)
    {
        case ScenePerformanceControlTarget::FilterEnabled:
        case ScenePerformanceControlTarget::DelayMode:
        case ScenePerformanceControlTarget::DelaySyncEnabled:
            return true;
        case ScenePerformanceControlTarget::None:
        case ScenePerformanceControlTarget::Speed:
        case ScenePerformanceControlTarget::Pitch:
        case ScenePerformanceControlTarget::Pan:
        case ScenePerformanceControlTarget::Volume:
        case ScenePerformanceControlTarget::Swing:
        case ScenePerformanceControlTarget::GrainSize:
        case ScenePerformanceControlTarget::GrainDensity:
        case ScenePerformanceControlTarget::GrainPitch:
        case ScenePerformanceControlTarget::GrainJitter:
        case ScenePerformanceControlTarget::GrainRandomDepth:
        case ScenePerformanceControlTarget::GrainEnvelope:
        case ScenePerformanceControlTarget::FilterFrequency:
        case ScenePerformanceControlTarget::FilterResonance:
        case ScenePerformanceControlTarget::FilterMorph:
        case ScenePerformanceControlTarget::SliceLength:
        case ScenePerformanceControlTarget::Scratch:
        case ScenePerformanceControlTarget::DelayMix:
        case ScenePerformanceControlTarget::DelayTime:
        case ScenePerformanceControlTarget::DelayFeedback:
        case ScenePerformanceControlTarget::DelayLowCut:
        case ScenePerformanceControlTarget::DelayHighCut:
        case ScenePerformanceControlTarget::Retrigger:
        case ScenePerformanceControlTarget::Rearrange:
        case ScenePerformanceControlTarget::GrainPitchJitter:
        case ScenePerformanceControlTarget::GrainSpread:
        case ScenePerformanceControlTarget::GrainPositionJitter:
        case ScenePerformanceControlTarget::GrainArp:
        case ScenePerformanceControlTarget::GrainCloud:
        case ScenePerformanceControlTarget::GrainEmitter:
        case ScenePerformanceControlTarget::GrainShape:
        default:
            return false;
    }
}

bool eventUsesSteppedSegments(const ScenePerformanceEvent& event) noexcept
{
    return event.drawStepped || targetUsesSteppedSegments(event.controlTarget);
}

bool targetUsesStripRuntimeState(ScenePerformanceControlTarget target) noexcept
{
    return target != ScenePerformanceControlTarget::None
        && target != ScenePerformanceControlTarget::Retrigger
        && target != ScenePerformanceControlTarget::Rearrange;
}

int resolvedStripIndexForTarget(int stripIndex, ScenePerformanceControlTarget target) noexcept
{
    if (targetUsesGlobalStrip(target))
        return -1;

    return juce::jlimit(0, MlrVSTAudioProcessor::MaxStrips - 1, stripIndex);
}

int columnFromNormalizedValue(float normalizedValue) noexcept
{
    return juce::jlimit(0,
                        MlrVSTAudioProcessor::MaxColumns - 1,
                        static_cast<int>(std::round(juce::jlimit(0.0f, 1.0f, normalizedValue)
                                                    * static_cast<float>(MlrVSTAudioProcessor::MaxColumns - 1))));
}

int controlTargetIndex(ScenePerformanceControlTarget target) noexcept
{
    return juce::jlimit(0,
                        static_cast<int>(ScenePerformanceControlTarget::GrainShape),
                        static_cast<int>(target));
}

int automationMaskIndex(int stripIndex, ScenePerformanceControlTarget target) noexcept
{
    if (targetUsesGlobalStrip(target) && stripIndex < 0)
        return MlrVSTAudioProcessor::MaxStrips;

    return juce::jlimit(0, MlrVSTAudioProcessor::MaxStrips - 1, stripIndex);
}

uint64_t automationTargetBit(ScenePerformanceControlTarget target) noexcept
{
    if (target == ScenePerformanceControlTarget::None)
        return 0;

    return uint64_t{ 1 } << juce::jlimit(0, 63, static_cast<int>(target));
}

double wrapBeatIntoClip(double currentBeat, double sceneStartBeat, double lengthBeats) noexcept
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

bool findHeldEventAtClipBeat(const std::vector<ScenePerformanceEvent>& events,
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

ScenePerformanceEvent makeControlPointEvent(int stripIndex,
                                            ScenePerformanceControlTarget target,
                                            double timeBeats,
                                            float normalizedValue,
                                            bool drawStepped)
{
    ScenePerformanceEvent event;
    event.type = ScenePerformanceEventType::ControlPoint;
    event.stripIndex = resolvedStripIndexForTarget(stripIndex, target);
    event.timeBeats = juce::jmax(0.0, timeBeats);
    event.controlRow = controlRowForTarget(target);
    event.column = columnFromNormalizedValue(normalizedValue);
    event.controlTarget = target;
    event.controlMode = static_cast<int>(controlModeForTarget(target));
    event.value = denormalizeValue(event, normalizedValue);
    event.drawStepped = drawStepped;
    return event;
}

double snapBeatToGrid(double timeBeats, double lengthBeats, int gridDivision, bool enabled)
{
    const double maxBeat = juce::jmax(0.0, std::nextafter(juce::jmax(1.0, lengthBeats), 0.0));
    const double clampedBeat = juce::jlimit(0.0, maxBeat, timeBeats);
    if (!enabled)
        return clampedBeat;

    const int safeDivision = juce::jlimit(1, 64, gridDivision);
    const double gridStepBeats = 4.0 / static_cast<double>(safeDivision);
    if (!(gridStepBeats > 0.0) || !std::isfinite(gridStepBeats))
        return clampedBeat;

    return juce::jlimit(0.0, maxBeat, std::round(clampedBeat / gridStepBeats) * gridStepBeats);
}

double pointMatchEpsilon(int gridDivision, bool gridEnabled)
{
    const int safeDivision = juce::jlimit(1, 64, gridDivision);
    const double gridStepBeats = 4.0 / static_cast<double>(safeDivision);
    return gridEnabled ? juce::jmax(1.0e-4, gridStepBeats * 0.45) : 1.0e-3;
}

PointWriteResult writeControlPoint(std::vector<ScenePerformanceEvent>& events, const PointWriteOptions& options)
{
    PointWriteResult result;
    if (options.target == ScenePerformanceControlTarget::None)
        return result;

    const bool snapToGrid = options.gridEnabled || options.drawStepped;
    const double beat = snapBeatToGrid(options.timeBeats,
                                       options.lengthBeats,
                                       options.gridDivision,
                                       snapToGrid);
    const double matchEpsilon = pointMatchEpsilon(options.gridDivision, snapToGrid);
    const int safeStripIndex = resolvedStripIndexForTarget(options.stripIndex, options.target);
    const bool targetIsGlobal = targetUsesGlobalStrip(options.target);

    events.erase(std::remove_if(events.begin(),
                                events.end(),
                                [safeStripIndex,
                                 target = options.target,
                                 targetIsGlobal,
                                 beat,
                                 matchEpsilon](const ScenePerformanceEvent& event)
                                {
                                    return event.type == ScenePerformanceEventType::ControlPoint
                                        && event.controlTarget == target
                                        && (targetIsGlobal || event.stripIndex == safeStripIndex)
                                        && std::abs(event.timeBeats - beat) <= matchEpsilon;
                                }),
                 events.end());

    result.event = makeControlPointEvent(safeStripIndex,
                                         options.target,
                                         beat,
                                         options.normalizedValue,
                                         options.drawStepped);
    events.push_back(result.event);
    result.changed = true;
    return result;
}
} // namespace SceneAutomationRules
