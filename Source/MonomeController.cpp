#include "PluginProcessor.h"
#include "MonomeFileBrowserActions.h"
#include "MonomeFilterActions.h"
#include "MonomeGroupAssignActions.h"
#include "MonomeMixActions.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

namespace
{
constexpr int kMonomeModPrevColumn = 14;
constexpr int kMonomeModNextColumn = 15;
constexpr int kMonomeSceneRecorderColumn = 7;
constexpr int kMonomeSceneLaunchColumns = 7;
constexpr int kSceneLengthFirstColumn = 8;
constexpr int kSceneLengthLastColumn = 12;
constexpr float kScratchZeroEpsilon = 1.0e-6f;
constexpr uint32_t kSceneRecorderIdleBlinkIntervalMs = 480;
constexpr uint32_t kSceneRecorderActiveBlinkIntervalMs = 180;
constexpr std::array<int, 5> kSceneLengthButtonValues{ 1, 2, 4, 8, 16 };

double stutterDivisionBeatsFromButton(int x)
{
    static constexpr std::array<double, 7> kDivisionBeats{
        2.0,            // col 9  -> 1/2
        1.0,            // col 10 -> 1/4
        0.5,            // col 11 -> 1/8
        0.25,           // col 12 -> 1/16
        0.125,          // col 13 -> 1/32
        0.0625,         // col 14 -> 1/64
        0.03125         // col 15 -> 1/128
    };

    const int idx = juce::jlimit(0, 6, x - 9);
    return kDivisionBeats[static_cast<size_t>(idx)];
}

uint8_t stutterButtonBitForColumn(int x)
{
    if (x < 9 || x > 15)
        return 0;
    return static_cast<uint8_t>(1u << static_cast<unsigned int>(x - 9));
}

int stutterColumnFromMask(uint8_t mask)
{
    for (int bit = 6; bit >= 0; --bit)
    {
        if ((mask & static_cast<uint8_t>(1u << static_cast<unsigned int>(bit))) != 0)
            return 9 + bit;
    }
    return -1;
}

int sceneLengthCountFromButton(int x)
{
    if (x < kSceneLengthFirstColumn || x > kSceneLengthLastColumn)
        return 0;

    return kSceneLengthButtonValues[static_cast<size_t>(x - kSceneLengthFirstColumn)];
}

float quantizeMonomeRearrangeValue(float value01)
{
    return juce::jlimit(0.0f, 1.0f,
                        std::round(juce::jlimit(0.0f, 1.0f, value01)
                                   * static_cast<float>(ModernAudioEngine::MaxColumns - 1))
                            / static_cast<float>(juce::jmax(1, ModernAudioEngine::MaxColumns - 1)));
}

MlrVSTAudioProcessor::ControlMode sceneControlModeForTarget(ScenePerformanceControlTarget target)
{
    using ControlMode = MlrVSTAudioProcessor::ControlMode;

    switch (target)
    {
        case ScenePerformanceControlTarget::Speed:            return ControlMode::Speed;
        case ScenePerformanceControlTarget::Pitch:
        case ScenePerformanceControlTarget::GrainPitch:       return ControlMode::Pitch;
        case ScenePerformanceControlTarget::Pan:              return ControlMode::Pan;
        case ScenePerformanceControlTarget::Volume:           return ControlMode::Volume;
        case ScenePerformanceControlTarget::Swing:            return ControlMode::Swing;
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
        case ScenePerformanceControlTarget::GrainShape:       return ControlMode::GrainSize;
        case ScenePerformanceControlTarget::FilterFrequency:
        case ScenePerformanceControlTarget::FilterResonance:
        case ScenePerformanceControlTarget::FilterEnabled:
        case ScenePerformanceControlTarget::FilterMorph:      return ControlMode::Filter;
        case ScenePerformanceControlTarget::DelayMix:
        case ScenePerformanceControlTarget::DelayTime:
        case ScenePerformanceControlTarget::DelayFeedback:
        case ScenePerformanceControlTarget::DelayLowCut:
        case ScenePerformanceControlTarget::DelayHighCut:
        case ScenePerformanceControlTarget::DelayMode:
        case ScenePerformanceControlTarget::DelaySyncEnabled: return ControlMode::Delay;
        case ScenePerformanceControlTarget::SliceLength:
        case ScenePerformanceControlTarget::Scratch:
        case ScenePerformanceControlTarget::Retrigger:
        case ScenePerformanceControlTarget::Rearrange:
        case ScenePerformanceControlTarget::None:
        default:                                              return ControlMode::Normal;
    }
}

ScenePerformanceControlTarget sceneControlTargetForModTarget(bool grainMode,
                                                             ModernAudioEngine::ModTarget target)
{
    using ModTarget = ModernAudioEngine::ModTarget;

    switch (sanitizeModPerformanceTarget(target))
    {
        case ModTarget::Volume:              return ScenePerformanceControlTarget::Volume;
        case ModTarget::Pan:                 return ScenePerformanceControlTarget::Pan;
        case ModTarget::Pitch:               return grainMode ? ScenePerformanceControlTarget::GrainPitch
                                                              : ScenePerformanceControlTarget::Pitch;
        case ModTarget::GrainPitch:          return ScenePerformanceControlTarget::GrainPitch;
        case ModTarget::Cutoff:              return ScenePerformanceControlTarget::FilterFrequency;
        case ModTarget::Resonance:           return ScenePerformanceControlTarget::FilterResonance;
        case ModTarget::FilterMorph:         return ScenePerformanceControlTarget::FilterMorph;
        case ModTarget::Speed:               return ScenePerformanceControlTarget::Speed;
        case ModTarget::Retrigger:           return ScenePerformanceControlTarget::Retrigger;
        case ModTarget::SliceLength:         return ScenePerformanceControlTarget::SliceLength;
        case ModTarget::Scratch:             return ScenePerformanceControlTarget::Scratch;
        case ModTarget::DelayMix:            return ScenePerformanceControlTarget::DelayMix;
        case ModTarget::DelayTime:           return ScenePerformanceControlTarget::DelayTime;
        case ModTarget::DelayFeedback:       return ScenePerformanceControlTarget::DelayFeedback;
        case ModTarget::GrainSize:           return ScenePerformanceControlTarget::GrainSize;
        case ModTarget::GrainDensity:        return ScenePerformanceControlTarget::GrainDensity;
        case ModTarget::GrainPitchJitter:    return ScenePerformanceControlTarget::GrainPitchJitter;
        case ModTarget::GrainSpread:         return ScenePerformanceControlTarget::GrainSpread;
        case ModTarget::GrainJitter:         return ScenePerformanceControlTarget::GrainJitter;
        case ModTarget::GrainPositionJitter: return ScenePerformanceControlTarget::GrainPositionJitter;
        case ModTarget::GrainRandom:         return ScenePerformanceControlTarget::GrainRandomDepth;
        case ModTarget::GrainArp:            return ScenePerformanceControlTarget::GrainArp;
        case ModTarget::GrainCloud:          return ScenePerformanceControlTarget::GrainCloud;
        case ModTarget::GrainEmitter:        return ScenePerformanceControlTarget::GrainEmitter;
        case ModTarget::GrainEnvelope:       return ScenePerformanceControlTarget::GrainEnvelope;
        case ModTarget::GrainShape:          return ScenePerformanceControlTarget::GrainShape;
        case ModTarget::None:
        default:                             return ScenePerformanceControlTarget::None;
    }
}

float defaultNormalizedSceneAutomationValue(ScenePerformanceControlTarget target)
{
    switch (target)
    {
        case ScenePerformanceControlTarget::Volume:           return 1.0f;
        case ScenePerformanceControlTarget::Pan:              return 0.5f;
        case ScenePerformanceControlTarget::Pitch:            return 0.5f;
        case ScenePerformanceControlTarget::FilterFrequency:  return 1.0f;
        case ScenePerformanceControlTarget::FilterResonance:  return 0.0613131f;
        case ScenePerformanceControlTarget::FilterMorph:      return 0.0f;
        case ScenePerformanceControlTarget::Speed:            return 0.5f;
        case ScenePerformanceControlTarget::Retrigger:        return 0.0f;
        case ScenePerformanceControlTarget::SliceLength:      return 1.0f;
        case ScenePerformanceControlTarget::Scratch:          return 0.0f;
        case ScenePerformanceControlTarget::DelayMix:         return 0.0f;
        case ScenePerformanceControlTarget::DelayTime:        return 0.0f;
        case ScenePerformanceControlTarget::DelayFeedback:    return 0.0f;
        case ScenePerformanceControlTarget::GrainPitch:       return 0.5f;
        case ScenePerformanceControlTarget::GrainSize:        return 0.5156576f;
        case ScenePerformanceControlTarget::GrainDensity:     return 0.0f;
        case ScenePerformanceControlTarget::GrainPitchJitter: return 0.0f;
        case ScenePerformanceControlTarget::GrainSpread:      return 0.0f;
        case ScenePerformanceControlTarget::GrainJitter:      return 0.0f;
        case ScenePerformanceControlTarget::GrainPositionJitter: return 0.0f;
        case ScenePerformanceControlTarget::GrainRandomDepth: return 0.0f;
        case ScenePerformanceControlTarget::GrainArp:         return 0.0f;
        case ScenePerformanceControlTarget::GrainCloud:       return 0.0f;
        case ScenePerformanceControlTarget::GrainEmitter:     return 0.0f;
        case ScenePerformanceControlTarget::GrainEnvelope:    return 0.0f;
        case ScenePerformanceControlTarget::GrainShape:       return 0.5f;
        case ScenePerformanceControlTarget::None:
        default:                                              return 0.5f;
    }
}

float normalizeSceneAutomationValueForMonome(const ScenePerformanceEvent& event)
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

float denormalizeSceneAutomationValueForMonome(const ScenePerformanceEvent& event, float normalizedValue)
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

bool isSceneMainAutomationMonomeActive(const MlrVSTAudioProcessor& processor)
{
    return processor.isSceneModeEnabled()
        && processor.isControlModeActive()
        && processor.getCurrentControlMode() == MlrVSTAudioProcessor::ControlMode::Modulation
        && processor.getSceneModPageMode() == MlrVSTAudioProcessor::SceneModPageMode::MainModulation
        && processor.getAudioEngine() != nullptr;
}

bool sceneAutomationTargetIsBipolarForMonome(ScenePerformanceControlTarget target)
{
    switch (target)
    {
        case ScenePerformanceControlTarget::Pan:
        case ScenePerformanceControlTarget::Pitch:
        case ScenePerformanceControlTarget::GrainPitch:
        case ScenePerformanceControlTarget::Speed:
        case ScenePerformanceControlTarget::GrainShape:
            return true;
        case ScenePerformanceControlTarget::None:
        case ScenePerformanceControlTarget::Volume:
        case ScenePerformanceControlTarget::Swing:
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
        case ScenePerformanceControlTarget::FilterFrequency:
        case ScenePerformanceControlTarget::FilterResonance:
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

bool sceneAutomationTargetUsesGlobalStripForMonome(ScenePerformanceControlTarget target)
{
    return target == ScenePerformanceControlTarget::Retrigger;
}

ScenePerformanceControlTarget activeSceneMainAutomationTarget(const MlrVSTAudioProcessor& processor,
                                                             int stripIndex)
{
    const int safeStripIndex = juce::jlimit(0, MlrVSTAudioProcessor::MaxStrips - 1, stripIndex);
    const auto activeTarget = processor.getSceneMainAutomationDisplayTargetForStrip(safeStripIndex);
    const auto sceneTargets = processor.getSceneVisibleModTargetsForStrip(safeStripIndex);
    const bool grainMode = std::find(sceneTargets.begin(),
                                     sceneTargets.end(),
                                     ModernAudioEngine::ModTarget::GrainPitch) != sceneTargets.end();
    return sceneControlTargetForModTarget(grainMode, activeTarget);
}

int sceneMainAutomationDisplayPage(const MlrVSTAudioProcessor& processor,
                                   int sceneSlot,
                                   double currentBeat)
{
    const int safeSceneSlot = juce::jlimit(0, MlrVSTAudioProcessor::SceneSlots - 1, sceneSlot);
    const double lengthBeats = juce::jmax(1.0, processor.getResolvedSceneLengthBeats(safeSceneSlot));
    const int maxPage = juce::jmax(0, static_cast<int>(std::ceil(lengthBeats / 4.0)) - 1);

    if (!processor.isSceneModeEnabled() || safeSceneSlot != processor.getActiveSceneSlot())
        return 0;

    const double progressBeats = processor.getScenePerformancePlaybackBeat(safeSceneSlot, currentBeat);
    if (!std::isfinite(progressBeats) || progressBeats < 0.0)
        return 0;

    return juce::jlimit(0, maxPage, static_cast<int>(std::floor(progressBeats / 4.0)));
}

bool sceneMainAutomationBeatForColumn(const MlrVSTAudioProcessor& processor,
                                      int sceneSlot,
                                      int column,
                                      double currentBeat,
                                      double& beatOut,
                                      int* pageIndexOut = nullptr)
{
    const int safeSceneSlot = juce::jlimit(0, MlrVSTAudioProcessor::SceneSlots - 1, sceneSlot);
    const int safeColumn = juce::jlimit(0, ModernAudioEngine::MaxColumns - 1, column);
    const double lengthBeats = juce::jmax(1.0, processor.getResolvedSceneLengthBeats(safeSceneSlot));
    const double maxBeat = juce::jmax(0.0, std::nextafter(lengthBeats, 0.0));
    const int pageIndex = sceneMainAutomationDisplayPage(processor, safeSceneSlot, currentBeat);
    const double beat = (static_cast<double>(pageIndex) * 4.0) + (static_cast<double>(safeColumn) * 0.25);

    if (pageIndexOut != nullptr)
        *pageIndexOut = pageIndex;

    if (beat > (maxBeat + 1.0e-6))
        return false;

    beatOut = juce::jlimit(0.0, maxBeat, beat);
    return true;
}

bool sceneMainAutomationPlaybackStepForPage(const MlrVSTAudioProcessor& processor,
                                            int sceneSlot,
                                            int displayPage,
                                            double currentBeat,
                                            int& stepOut)
{
    const int safeSceneSlot = juce::jlimit(0, MlrVSTAudioProcessor::SceneSlots - 1, sceneSlot);
    if (!processor.isSceneModeEnabled() || safeSceneSlot != processor.getActiveSceneSlot())
        return false;

    const double progressBeats = processor.getScenePerformancePlaybackBeat(safeSceneSlot, currentBeat);
    if (!std::isfinite(progressBeats) || progressBeats < 0.0)
        return false;

    const int pageIndex = juce::jmax(0, static_cast<int>(std::floor(progressBeats / 4.0)));
    if (pageIndex != displayPage)
        return false;

    const double pageBeat = progressBeats - (static_cast<double>(pageIndex) * 4.0);
    stepOut = juce::jlimit(0,
                           ModernAudioEngine::MaxColumns - 1,
                           static_cast<int>(std::floor(pageBeat / 0.25)));
    return true;
}

float defaultSceneAutomationNormalizedValueForMonome(ScenePerformanceControlTarget target)
{
    return defaultNormalizedSceneAutomationValue(target);
}

float storedSceneAutomationNormalizedValueForMonome(const MlrVSTAudioProcessor& processor,
                                                    int sceneSlot,
                                                    int stripIndex,
                                                    ScenePerformanceControlTarget target)
{
    const int resolvedStripIndex = sceneAutomationTargetUsesGlobalStripForMonome(target)
        ? -1
        : juce::jlimit(0, MlrVSTAudioProcessor::MaxStrips - 1, stripIndex);

    float normalizedValue = defaultSceneAutomationNormalizedValueForMonome(target);
    if (processor.getStoredSceneControlNormalizedValue(sceneSlot,
                                                       resolvedStripIndex,
                                                       target,
                                                       normalizedValue)
        && std::isfinite(normalizedValue))
    {
        return juce::jlimit(0.0f, 1.0f, normalizedValue);
    }

    if (processor.getSceneControlBaseNormalizedValue(resolvedStripIndex, target, normalizedValue)
        && std::isfinite(normalizedValue))
    {
        return juce::jlimit(0.0f, 1.0f, normalizedValue);
    }

    return juce::jlimit(0.0f, 1.0f, normalizedValue);
}

float sceneMainAutomationColumnNormalizedValue(const MlrVSTAudioProcessor& processor,
                                               const std::vector<ScenePerformanceEvent>& events,
                                               int sceneSlot,
                                               int stripIndex,
                                               ScenePerformanceControlTarget target,
                                               double beat,
                                               double lengthBeats)
{
    const int safeStripIndex = sceneAutomationTargetUsesGlobalStripForMonome(target)
        ? -1
        : juce::jlimit(0, MlrVSTAudioProcessor::MaxStrips - 1, stripIndex);
    const double safeBeat = juce::jlimit(0.0, std::nextafter(juce::jmax(1.0, lengthBeats), 0.0), beat);
    const ScenePerformanceEvent* lastEvent = nullptr;
    const ScenePerformanceEvent* chosenEvent = nullptr;

    for (const auto& event : events)
    {
        if (event.type != ScenePerformanceEventType::ControlPoint
            || event.controlTarget != target)
        {
            continue;
        }

        if (!sceneAutomationTargetUsesGlobalStripForMonome(target))
        {
            if (event.stripIndex != safeStripIndex)
                continue;
        }
        else if (event.stripIndex != -1)
        {
            continue;
        }

        lastEvent = &event;
        if (event.timeBeats <= safeBeat + 1.0e-6)
            chosenEvent = &event;
    }

    if (chosenEvent == nullptr)
        chosenEvent = lastEvent;
    if (chosenEvent != nullptr)
    {
        const float normalized = normalizeSceneAutomationValueForMonome(*chosenEvent);
        if (std::isfinite(normalized))
            return juce::jlimit(0.0f, 1.0f, normalized);
    }

    return storedSceneAutomationNormalizedValueForMonome(processor,
                                                         sceneSlot,
                                                         safeStripIndex,
                                                         target);
}

bool writeSceneMainAutomationColumn(MlrVSTAudioProcessor& processor,
                                    int stripIndex,
                                    int column,
                                    float normalizedValue,
                                    double currentBeat)
{
    if (!isSceneMainAutomationMonomeActive(processor) || processor.isScenePerformanceRecording())
        return false;

    const int sceneSlot = processor.getFocusedSceneSlot();
    double beat = 0.0;
    if (!sceneMainAutomationBeatForColumn(processor, sceneSlot, column, currentBeat, beat))
        return false;

    const int safeStripIndex = juce::jlimit(0, MlrVSTAudioProcessor::MaxStrips - 1, stripIndex);
    const auto target = activeSceneMainAutomationTarget(processor, safeStripIndex);
    if (target == ScenePerformanceControlTarget::None)
        return false;

    const int eventStripIndex = sceneAutomationTargetUsesGlobalStripForMonome(target) ? -1 : safeStripIndex;
    const double matchEpsilon = 0.1125;
    const double nextBeat = beat + 0.25;
    const double lengthBeats = juce::jmax(1.0, processor.getResolvedSceneLengthBeats(sceneSlot));
    const double maxBeat = juce::jmax(0.0, std::nextafter(lengthBeats, 0.0));
    auto events = processor.getScenePerformanceEventsSnapshot(sceneSlot);
    events.erase(std::remove_if(events.begin(),
                                events.end(),
                                [target, eventStripIndex, beat, matchEpsilon](const ScenePerformanceEvent& event)
                                {
                                    return event.type == ScenePerformanceEventType::ControlPoint
                                        && event.controlTarget == target
                                        && event.stripIndex == eventStripIndex
                                        && std::abs(event.timeBeats - beat) <= matchEpsilon;
                                }),
                 events.end());

    ScenePerformanceEvent event;
    event.type = ScenePerformanceEventType::ControlPoint;
    event.stripIndex = eventStripIndex;
    event.timeBeats = beat;
    event.controlRow = 0;
    event.column = juce::jlimit(0,
                                ModernAudioEngine::MaxColumns - 1,
                                static_cast<int>(std::round(juce::jlimit(0.0f, 1.0f, normalizedValue)
                                                            * static_cast<float>(ModernAudioEngine::MaxColumns - 1))));
    event.controlTarget = target;
    event.controlMode = static_cast<int>(sceneControlModeForTarget(target));
    event.value = denormalizeSceneAutomationValueForMonome(event, normalizedValue);
    events.push_back(event);

    if (nextBeat <= maxBeat + 1.0e-6)
    {
        const bool hasNextStepAnchor = std::any_of(events.begin(),
                                                   events.end(),
                                                   [target, eventStripIndex, nextBeat, matchEpsilon](const ScenePerformanceEvent& existingEvent)
                                                   {
                                                       return existingEvent.type == ScenePerformanceEventType::ControlPoint
                                                           && existingEvent.controlTarget == target
                                                           && existingEvent.stripIndex == eventStripIndex
                                                           && std::abs(existingEvent.timeBeats - nextBeat) <= matchEpsilon;
                                                   });

        if (!hasNextStepAnchor)
        {
            ScenePerformanceEvent resetEvent = event;
            resetEvent.timeBeats = juce::jlimit(0.0, maxBeat, nextBeat);
            const float resetNormalizedValue = storedSceneAutomationNormalizedValueForMonome(processor,
                                                                                            sceneSlot,
                                                                                            safeStripIndex,
                                                                                            target);
            resetEvent.column = juce::jlimit(0,
                                             ModernAudioEngine::MaxColumns - 1,
                                             static_cast<int>(std::round(juce::jlimit(0.0f, 1.0f, resetNormalizedValue)
                                                                         * static_cast<float>(ModernAudioEngine::MaxColumns - 1))));
            resetEvent.value = denormalizeSceneAutomationValueForMonome(resetEvent, resetNormalizedValue);
            events.push_back(resetEvent);
        }
    }

    return processor.replaceScenePerformanceClipEvents(sceneSlot, events);
}
}

void MlrVSTAudioProcessor::resetStepEditVelocityGestures()
{
    stepEditVelocityGestureActive.fill(false);
    stepEditVelocityGestureStrip.fill(0);
    stepEditVelocityGestureStep.fill(0);
    stepEditVelocityGestureAnchorStart.fill(1.0f);
    stepEditVelocityGestureAnchorEnd.fill(1.0f);
    stepEditVelocityGestureAnchorValue.fill(1.0f);
    stepEditVelocityGestureLastActivityMs.fill(0);
}

void MlrVSTAudioProcessor::setMomentaryScratchHold(bool shouldEnable)
{
    if (!audioEngine)
        return;

    if (momentaryScratchHoldActive == shouldEnable)
        return;

    const double hostPpqNow = audioEngine->getTimelineBeat();
    momentaryScratchHoldActive = shouldEnable;

    for (int i = 0; i < MaxStrips; ++i)
    {
        auto* strip = audioEngine->getStrip(i);
        if (!strip)
            continue;

        const auto idx = static_cast<size_t>(i);
        if (shouldEnable)
        {
            strip->captureMomentaryPhaseReference(hostPpqNow);

            momentaryScratchSavedAmount[idx] = strip->getScratchAmount();
            momentaryScratchSavedDirection[idx] = strip->getDirectionMode();
            momentaryScratchWasStepMode[idx] = (strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Step);

            // Original momentary scratch profile.
            strip->setScratchAmount(15.0f);

            if (momentaryScratchWasStepMode[idx])
                strip->setDirectionMode(EnhancedAudioStrip::DirectionMode::Random);
        }
        else
        {
            const int64_t nowSample = audioEngine->getGlobalSampleCount();
            strip->setScratchAmount(momentaryScratchSavedAmount[idx]);

            if (momentaryScratchWasStepMode[idx])
                strip->setDirectionMode(momentaryScratchSavedDirection[idx]);

            if (strip->isScratchActive())
                strip->snapToTimeline(nowSample);

            strip->enforceMomentaryPhaseReference(hostPpqNow, nowSample);
        }
    }
}

void MlrVSTAudioProcessor::setMomentaryStutterHold(bool shouldEnable)
{
    if (!audioEngine)
        return;

    const bool startPending = (pendingStutterStartActive.load(std::memory_order_acquire) != 0);
    const bool playbackActive = (momentaryStutterPlaybackActive.load(std::memory_order_acquire) != 0);
    if (!shouldEnable && !momentaryStutterHoldActive && !startPending && !playbackActive)
        return;

    const auto resolveRecordedStutterColumn = [this]() -> int
    {
        if (momentaryStutterActiveDivisionButton >= 9 && momentaryStutterActiveDivisionButton <= 15)
            return momentaryStutterActiveDivisionButton;
        if (momentaryStutterRecordedDivisionButton >= 9 && momentaryStutterRecordedDivisionButton <= 15)
            return momentaryStutterRecordedDivisionButton;
        return stutterColumnFromMask(static_cast<uint8_t>(
            momentaryStutterButtonMask.load(std::memory_order_acquire) & 0x7f));
    };

    const int64_t nowSample = audioEngine->getGlobalSampleCount();
    auto readHostTiming = [this](double& outPpq, double& outTempo)
    {
        outPpq = audioEngine ? audioEngine->getTimelineBeat() : 0.0;
        outTempo = audioEngine ? juce::jmax(1.0, audioEngine->getCurrentTempo()) : 120.0;
        if (auto* hostPlayHead = getPlayHead())
        {
            if (auto position = hostPlayHead->getPosition())
            {
                if (position->getPpqPosition().hasValue())
                    outPpq = *position->getPpqPosition();
                if (position->getBpm().hasValue() && *position->getBpm() > 1.0)
                    outTempo = *position->getBpm();
            }
        }
    };

    if (shouldEnable && (momentaryStutterHoldActive || startPending || playbackActive))
    {
        momentaryStutterHoldActive = true;
        pendingStutterReleaseActive.store(0, std::memory_order_release);
        pendingStutterReleasePpq.store(-1.0, std::memory_order_release);
        pendingStutterReleaseSampleTarget.store(-1, std::memory_order_release);
        audioEngine->setMomentaryStutterReleasePpq(-1.0);
        const int startQuantizeDivision = juce::jmax(1, getQuantizeDivision());
        pendingStutterStartQuantizeDivision.store(startQuantizeDivision, std::memory_order_release);
        const double entryDivision = juce::jlimit(0.03125, 4.0, momentaryStutterDivisionBeats);
        const int currentColumn = resolveRecordedStutterColumn();
        pendingStutterStartDivisionBeats.store(entryDivision, std::memory_order_release);
        audioEngine->setMomentaryStutterDivision(entryDivision);
        if (currentColumn >= 9 && currentColumn <= 15)
            momentaryStutterRecordedDivisionButton = currentColumn;

        if (startPending && !playbackActive)
        {
            pendingStutterStartPpq.store(std::numeric_limits<double>::quiet_NaN(), std::memory_order_release);
            pendingStutterStartSampleTarget.store(-1, std::memory_order_release);
        }

        if (playbackActive)
        {
            recordMomentaryStutterSceneDivision(entryDivision,
                                                momentaryStutterRecordedDivisionButton,
                                                audioEngine->getTimelineBeat());
            audioEngine->setMomentaryStutterActive(true);
        }
        return;
    }

    if (shouldEnable)
    {
        momentaryStutterHoldActive = true;
        if (momentaryStutterButtonMask.load(std::memory_order_acquire) == 0)
        {
            const uint8_t fallbackBit = stutterButtonBitForColumn(momentaryStutterActiveDivisionButton);
            if (fallbackBit != 0)
                momentaryStutterButtonMask.store(fallbackBit, std::memory_order_release);
        }
        const int currentColumn = resolveRecordedStutterColumn();
        if (currentColumn >= 9 && currentColumn <= 15)
            momentaryStutterRecordedDivisionButton = currentColumn;

        momentaryStutterMacroCapturePending = true;
        momentaryStutterMacroBaselineCaptured = false;
        for (auto& saved : momentaryStutterSavedState)
            saved = MomentaryStutterSavedStripState{};

        pendingStutterReleaseActive.store(0, std::memory_order_release);
        pendingStutterReleasePpq.store(-1.0, std::memory_order_release);
        pendingStutterReleaseSampleTarget.store(-1, std::memory_order_release);

        double currentPpq = 0.0;
        double tempoNow = 120.0;
        readHostTiming(currentPpq, tempoNow);
        juce::ignoreUnused(tempoNow);
        if (!(std::isfinite(currentPpq) && currentPpq >= 0.0))
        {
            // Host PPQ can be briefly unavailable during transport transitions.
            // Fall back to immediate engine-timeline start instead of dropping stutter.
            const double fallbackPpq = audioEngine->getTimelineBeat();
            if (!std::isfinite(fallbackPpq))
            {
                momentaryStutterHoldActive = false;
                momentaryStutterRecordedDivisionButton = -1;
                pendingStutterStartActive.store(0, std::memory_order_release);
                pendingStutterStartPpq.store(-1.0, std::memory_order_release);
                pendingStutterStartSampleTarget.store(-1, std::memory_order_release);
                momentaryStutterPlaybackActive.store(0, std::memory_order_release);
                audioEngine->setMomentaryStutterActive(false);
                return;
            }

            const double entryDivision = juce::jlimit(0.03125, 4.0, momentaryStutterDivisionBeats);
            pendingStutterStartDivisionBeats.store(entryDivision, std::memory_order_release);
            pendingStutterStartActive.store(0, std::memory_order_release);
            pendingStutterStartPpq.store(-1.0, std::memory_order_release);
            pendingStutterStartSampleTarget.store(-1, std::memory_order_release);
            performMomentaryStutterStartNow(fallbackPpq, nowSample);
            return;
        }

        const double entryDivision = juce::jlimit(0.03125, 4.0, momentaryStutterDivisionBeats);
        const int startQuantizeDivision = juce::jmax(1, getQuantizeDivision());
        pendingStutterStartQuantizeDivision.store(startQuantizeDivision, std::memory_order_release);
        pendingStutterStartDivisionBeats.store(entryDivision, std::memory_order_release);
        pendingStutterStartPpq.store(std::numeric_limits<double>::quiet_NaN(), std::memory_order_release);
        pendingStutterStartSampleTarget.store(-1, std::memory_order_release);
        pendingStutterStartActive.store(1, std::memory_order_release);
        momentaryStutterPlaybackActive.store(0, std::memory_order_release);
        audioEngine->setMomentaryStutterActive(false);
        return;
    }

    // UI/key state ends immediately on key-up; audio release remains quantized.
    momentaryStutterHoldActive = false;
    momentaryStutterActiveDivisionButton = -1;
    momentaryStutterButtonMask.store(0, std::memory_order_release);

    if (startPending && !playbackActive)
    {
        momentaryStutterRecordedDivisionButton = -1;
        pendingStutterStartActive.store(0, std::memory_order_release);
        pendingStutterStartPpq.store(-1.0, std::memory_order_release);
        pendingStutterStartSampleTarget.store(-1, std::memory_order_release);
        momentaryStutterPlaybackActive.store(0, std::memory_order_release);
        momentaryStutterLastComboMask = 0;
        momentaryStutterTwoButtonStepBaseValid = false;
        momentaryStutterTwoButtonStepBase = 0;
        momentaryStutterMacroBaselineCaptured = false;
        momentaryStutterMacroCapturePending = false;
        audioEngine->setMomentaryStutterActive(false);
        audioEngine->setMomentaryStutterStartPpq(-1.0);
        audioEngine->setMomentaryStutterReleasePpq(-1.0);
        audioEngine->clearMomentaryStutterStrips();
        for (auto& armed : momentaryStutterStripArmed)
            armed = false;
        return;
    }

    restoreMomentaryStutterMacroBaseline();

    if (!playbackActive)
        return;

    // Quantized stutter release (PPQ-locked):
    // convert next PPQ grid boundary to an absolute sample target now.
    const int division = juce::jmax(1, getQuantizeDivision());
    const double quantBeats = 4.0 / static_cast<double>(division);

    double currentPpq = audioEngine->getTimelineBeat();
    double tempoNow = juce::jmax(1.0, audioEngine->getCurrentTempo());
    if (auto* hostPlayHead = getPlayHead())
    {
        if (auto position = hostPlayHead->getPosition())
        {
            if (position->getPpqPosition().hasValue())
                currentPpq = *position->getPpqPosition();
            if (position->getBpm().hasValue() && *position->getBpm() > 1.0)
                tempoNow = *position->getBpm();
        }
    }

    if (!std::isfinite(currentPpq) || !std::isfinite(tempoNow) || tempoNow <= 0.0 || currentSampleRate <= 0.0)
    {
        pendingStutterReleaseActive.store(0, std::memory_order_release);
        pendingStutterReleasePpq.store(-1.0, std::memory_order_release);
        pendingStutterReleaseSampleTarget.store(-1, std::memory_order_release);
        performMomentaryStutterReleaseNow(audioEngine->getTimelineBeat(), nowSample);
        return;
    }

    double releasePpq = std::ceil(currentPpq / quantBeats) * quantBeats;
    if (releasePpq <= (currentPpq + 1.0e-6))
        releasePpq += quantBeats;
    releasePpq = std::round(releasePpq / quantBeats) * quantBeats;

    const double samplesPerQuarter = (60.0 / tempoNow) * currentSampleRate;
    const int64_t currentAbsSample = static_cast<int64_t>(std::llround(currentPpq * samplesPerQuarter));
    const int64_t targetAbsSample = static_cast<int64_t>(std::llround(releasePpq * samplesPerQuarter));
    const int64_t deltaSamples = juce::jmax<int64_t>(1, targetAbsSample - currentAbsSample);
    const int64_t targetSample = nowSample + deltaSamples;

    pendingStutterReleaseQuantizeDivision.store(division, std::memory_order_release);
    pendingStutterReleasePpq.store(releasePpq, std::memory_order_release);
    pendingStutterReleaseSampleTarget.store(targetSample, std::memory_order_release);
    pendingStutterReleaseActive.store(1, std::memory_order_release);
    audioEngine->setMomentaryStutterReleasePpq(releasePpq);
}

void MlrVSTAudioProcessor::performMomentaryStutterStartNow(double hostPpqNow, int64_t nowSample)
{
    juce::ignoreUnused(nowSample);

    if (!audioEngine || !momentaryStutterHoldActive)
        return;

    double entryPpq = hostPpqNow;
    if (!std::isfinite(entryPpq))
        entryPpq = audioEngine->getTimelineBeat();
    if (!std::isfinite(entryPpq))
        return;

    const double entryDivision = juce::jlimit(
        0.03125, 4.0, pendingStutterStartDivisionBeats.load(std::memory_order_acquire));
    momentaryStutterMacroStartPpq = entryPpq;
    momentaryStutterLastComboMask = 0;
    momentaryStutterTwoButtonStepBaseValid = false;
    momentaryStutterTwoButtonStepBase = 0;

    audioEngine->setMomentaryStutterDivision(entryDivision);
    audioEngine->setMomentaryStutterRetriggerFadeMs(0.7f);
    audioEngine->setMomentaryStutterStartPpq(entryPpq);
    audioEngine->setMomentaryStutterReleasePpq(-1.0);
    audioEngine->clearMomentaryStutterStrips();
    for (int i = 0; i < MaxStrips; ++i)
    {
        auto* strip = audioEngine->getStrip(i);
        const auto idx = static_cast<size_t>(i);
        momentaryStutterStripArmed[idx] = false;
        const bool stepMode = (strip && strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Step);
        const bool hasStepAudio = stepMode && strip->getStepSampler() && strip->getStepSampler()->getHasAudio();
        const bool hasPlayableContent = (strip && (strip->hasAudio() || hasStepAudio || stepMode));
        if (!strip || !hasPlayableContent)
        {
            audioEngine->setMomentaryStutterStrip(i, 0, 0.0, false);
            continue;
        }

        if (stepMode && !strip->isPlaying())
            strip->startStepSequencer();
        if (!stepMode && !strip->isPlaying())
        {
            audioEngine->setMomentaryStutterStrip(i, 0, 0.0, false);
            continue;
        }

        strip->captureMomentaryPhaseReference(entryPpq);
        const int stutterColumn = juce::jlimit(0, 15, strip->getStutterEntryColumn());
        const double stutterOffsetRatio = strip->getStutterEntryOffsetRatio();
        audioEngine->setMomentaryStutterStrip(i, stutterColumn, stutterOffsetRatio, true);
        audioEngine->clearPendingQuantizedTriggersForStrip(i);
        momentaryStutterStripArmed[idx] = true;
    }
    audioEngine->setMomentaryStutterActive(true);
    momentaryStutterPlaybackActive.store(1, std::memory_order_release);
    pendingStutterStartActive.store(0, std::memory_order_release);
    pendingStutterStartPpq.store(-1.0, std::memory_order_release);
    pendingStutterStartSampleTarget.store(-1, std::memory_order_release);

    int recordColumn = momentaryStutterRecordedDivisionButton;
    if (recordColumn < 9 || recordColumn > 15)
        recordColumn = stutterColumnFromMask(static_cast<uint8_t>(
            momentaryStutterButtonMask.load(std::memory_order_acquire) & 0x7f));
    if (recordColumn >= 9 && recordColumn <= 15)
        momentaryStutterRecordedDivisionButton = recordColumn;

    recordMomentaryStutterSceneDivision(entryDivision,
                                        momentaryStutterRecordedDivisionButton,
                                        entryPpq);
}

void MlrVSTAudioProcessor::performMomentaryStutterReleaseNow(double hostPpqNow, int64_t nowSample)
{
    if (!audioEngine)
        return;

    const int recordColumn = (momentaryStutterRecordedDivisionButton >= 9
                              && momentaryStutterRecordedDivisionButton <= 15)
        ? momentaryStutterRecordedDivisionButton
        : momentaryStutterActiveDivisionButton;
    recordSceneGlobalStutterEvent(0.0f, recordColumn, hostPpqNow);

    pendingStutterStartActive.store(0, std::memory_order_release);
    pendingStutterStartPpq.store(-1.0, std::memory_order_release);
    pendingStutterStartSampleTarget.store(-1, std::memory_order_release);
    momentaryStutterPlaybackActive.store(0, std::memory_order_release);
    momentaryStutterLastComboMask = 0;
    momentaryStutterTwoButtonStepBaseValid = false;
    momentaryStutterTwoButtonStepBase = 0;
    restoreMomentaryStutterMacroBaseline();
    audioEngine->setMomentaryStutterActive(false);
    audioEngine->setMomentaryStutterRetriggerFadeMs(0.7f);
    audioEngine->setMomentaryStutterStartPpq(-1.0);
    audioEngine->setMomentaryStutterReleasePpq(-1.0);
    audioEngine->clearMomentaryStutterStrips();
    momentaryStutterButtonMask.store(0, std::memory_order_release);
    momentaryStutterRecordedDivisionButton = -1;
    for (int i = 0; i < MaxStrips; ++i)
    {
        auto* strip = audioEngine->getStrip(i);
        if (!strip)
            continue;
        strip->enforceMomentaryPhaseReference(hostPpqNow, nowSample);
        momentaryStutterStripArmed[static_cast<size_t>(i)] = false;
    }
}

void MlrVSTAudioProcessor::handleMonomeKeyPress(int x, int y, int state)
{
    if (!audioEngine) return;

    const auto layout = getMonomeLayoutState();
    const int gridWidth = layout.gridWidth;
    const int gridHeight = layout.gridHeight;
    if (x < 0 || y < 0 || x >= gridWidth || y >= gridHeight)
        return;

    const int GROUP_ROW = layout.groupRow;
    const int CONTROL_ROW = layout.controlRow;
    const int FIRST_STRIP_ROW = layout.firstStripRow;
    const int visibleStripCount = layout.visibleStripCount;
    const int stepEditBankSize = layout.stepEditBankSize;
    const int maxStepEditBank = layout.maxStepEditBank;
    stepEditStripBank = juce::jlimit(0, maxStepEditBank, stepEditStripBank);
    const int stripRowsDenom = layout.stripRowsDenom;
    const int modulationRowsDenom = layout.modulationRowsDenom;
    const auto clampVisibleStrip = [&layout](int index)
    {
        return layout.clampVisibleStrip(index);
    };
    const auto stripRowToUnit = [CONTROL_ROW, stripRowsDenom](int row)
    {
        return juce::jlimit(0.0f, 1.0f, static_cast<float>((CONTROL_ROW - 1) - row)
            / static_cast<float>(stripRowsDenom));
    };
    const auto modPageDirectionForColumn = [](int column)
    {
        if (column == kMonomeModPrevColumn)
            return -1;
        if (column == kMonomeModNextColumn)
            return 1;
        return 0;
    };
    const auto sceneStepMotionPageNavigationActive = [&]()
    {
        const bool sceneStepMotionMonomePageActive = isControlModeActive()
            && getCurrentControlMode() == ControlMode::Modulation;
        return isSceneModeEnabled()
            && getSceneModPageMode() == SceneModPageMode::StepMotion
            && (isSceneStepMotionEditorOpen() || sceneStepMotionMonomePageActive);
    };
    const auto sceneStepMotionLaneNavigationAvailable = [this](int stripIndex)
    {
        const int safeStripIndex = juce::jlimit(0, MaxStrips - 1, stripIndex);
        return getSceneVisibleModTargetsForStrip(safeStripIndex).size() > 1;
    };
    const auto sceneMainAutomationMonomeActive = [this]()
    {
        return isSceneMainAutomationMonomeActive(*this);
    };
    const auto sceneModRepeatButtonsVisible = [&]()
    {
        return isSceneModeEnabled()
            && controlModeActive
            && currentControlMode == ControlMode::Modulation
            && (layout.topRowMode == MonomeLayoutState::TopRowMode::SceneLaunch
                || layout.topRowMode == MonomeLayoutState::TopRowMode::Modulation);
    };
    const auto modulationRowToUnitForColumn = [this, CONTROL_ROW, visibleStripCount, modulationRowsDenom, &sceneStepMotionPageNavigationActive](int row, int column)
    {
        const bool modulationPageActive = isControlModeActive()
            && getCurrentControlMode() == ControlMode::Modulation;
        const bool reservedTopCell = modulationPageActive
            ? (column == kMonomeModPrevColumn || column == kMonomeModNextColumn)
            : (sceneStepMotionPageNavigationActive()
                ? (column == kMonomeModPrevColumn || column == kMonomeModNextColumn)
                : (column == kMonomeModNextColumn));
        const int denom = reservedTopCell
            ? juce::jmax(1, visibleStripCount - 1)
            : modulationRowsDenom;
        return juce::jlimit(0.0f, 1.0f, static_cast<float>((CONTROL_ROW - 1) - row)
            / static_cast<float>(denom));
    };
    const auto sceneMainAutomationRowToUnit = [GROUP_ROW, CONTROL_ROW](int row)
    {
        const int topWritableRow = GROUP_ROW + 1;
        const int bottomWritableRow = juce::jmax(topWritableRow, CONTROL_ROW - 1);
        const int clampedRow = juce::jlimit(topWritableRow, bottomWritableRow, row);
        const int denom = juce::jmax(1, bottomWritableRow - topWritableRow);
        return juce::jlimit(0.0f,
                            1.0f,
                            static_cast<float>(bottomWritableRow - clampedRow)
                                / static_cast<float>(denom));
    };
    const auto syncMonomeModEditPageToPlayback = [&](ModernAudioEngine& engine, int stripIndex)
    {
        if (sceneStepMotionPageNavigationActive() || sceneMainAutomationMonomeActive())
            return false;

        if (stripIndex < 0 || stripIndex >= MaxStrips)
            return false;

        auto* strip = engine.getStrip(stripIndex);
        if (strip == nullptr || !strip->isPlaying())
            return false;

        const int playbackPage = engine.getModCurrentPage(stripIndex);
        if (engine.getModEditPage(stripIndex) != playbackPage)
            engine.setModEditPage(stripIndex, playbackPage);
        return true;
    };
    const auto navigateSceneStepMotionLane = [&](int stripIndex, int direction)
    {
        if (!sceneStepMotionPageNavigationActive())
            return false;

        const int safeDirection = (direction > 0) ? 1 : ((direction < 0) ? -1 : 0);
        if (safeDirection == 0 || stripIndex < 0 || stripIndex >= MaxStrips)
            return false;

        if (!sceneStepMotionLaneNavigationAvailable(stripIndex))
            return false;

        stepSceneModLaneTarget(stripIndex, safeDirection);
        return true;
    };
    const auto regularModPageLaneNavigationVisible = [&]()
    {
        return controlModeActive
            && currentControlMode == ControlMode::Modulation
            && layout.topRowMode != MonomeLayoutState::TopRowMode::Modulation
            && !sceneStepMotionPageNavigationActive();
    };
    const auto navigatePerRowModLane = [&](ModernAudioEngine& engine, int stripIndex, int direction)
    {
        const int safeDirection = (direction > 0) ? 1 : ((direction < 0) ? -1 : 0);
        if (safeDirection == 0 || stripIndex < 0 || stripIndex >= MaxStrips)
            return false;

        syncMonomeModEditPageToPlayback(engine, stripIndex);
        stepVisibleModLaneTarget(stripIndex, safeDirection);
        return true;
    };
    const auto isPresetCell = [&layout](int gridX, int gridY)
    {
        return layout.isPresetCell(gridX, gridY);
    };
    const auto toPresetIndex = [&layout](int gridX, int gridY)
    {
        return layout.toPresetIndex(gridX, gridY);
    };
    const auto isSceneLaunchCell = [topRowMode = layout.topRowMode, GROUP_ROW, gridWidth](int gridX, int gridY)
    {
        return topRowMode == MonomeLayoutState::TopRowMode::SceneLaunch
            && gridY == GROUP_ROW
            && gridX >= 0
            && gridX < juce::jmin(gridWidth, kMonomeSceneLaunchColumns);
    };
    const auto isSceneRecorderCell = [topRowMode = layout.topRowMode, GROUP_ROW, gridWidth](int gridX, int gridY)
    {
        return topRowMode == MonomeLayoutState::TopRowMode::SceneLaunch
            && gridY == GROUP_ROW
            && kMonomeSceneRecorderColumn >= 0
            && kMonomeSceneRecorderColumn < gridWidth
            && gridX == kMonomeSceneRecorderColumn;
    };
    const bool presetModeActive = layout.presetModeActive;
    const bool stepEditModeActive = layout.stepEditModeActive;
    const bool sceneModeActive = layout.sceneModeActive;
    const auto topRowMode = layout.topRowMode;
    const auto topRowUsesScratchAndStutter = [&]()
    {
        return (!controlModeActive || currentControlMode == ControlMode::Normal)
            && (topRowMode == MonomeLayoutState::TopRowMode::Launch
                || (topRowMode == MonomeLayoutState::TopRowMode::SceneLaunch
                    && !sceneStepMotionPageNavigationActive()));
    };
    const auto isDisplayedDataRow = [&layout](int row)
    {
        return layout.isDisplayedDataRow(row);
    };
    
    static int loopSetFirstButton = -1;
    static int loopSetStrip = -1;
    
    if (state == 1) // Key down
    {
        // GROUP ROW (y=0): Groups 0-3 + Pattern Recorders 4-7
        if (y == GROUP_ROW)
        {
            const int sceneLaunchStepMotionLaneStep = topRowMode == MonomeLayoutState::TopRowMode::SceneLaunch
                && sceneStepMotionPageNavigationActive()
                ? modPageDirectionForColumn(x)
                : 0;

            if (sceneLaunchStepMotionLaneStep != 0)
            {
                const int targetStrip = clampVisibleStrip(getLastMonomePressedStripRow());
                navigateSceneStepMotionLane(targetStrip, sceneLaunchStepMotionLaneStep);
                updateMonomeLEDs();
                return;
            }

            const int regularModPageLaneStep = regularModPageLaneNavigationVisible()
                ? modPageDirectionForColumn(x)
                : 0;

            if (regularModPageLaneStep != 0)
            {
                auto* engine = getAudioEngine();
                if (!engine)
                    return;

                const int targetStrip = clampVisibleStrip(getLastMonomePressedStripRow());
                if (navigatePerRowModLane(*engine, targetStrip, regularModPageLaneStep))
                {
                    updateMonomeLEDs();
                    return;
                }
            }

            if (sceneModRepeatButtonsVisible())
            {
                const int sceneLengthCount = sceneLengthCountFromButton(x);
                if (sceneLengthCount > 0)
                {
                    const int sceneSlot = getFocusedSceneSlot();
                    setSceneLengthCount(sceneSlot, sceneLengthCount);
                    persistSceneTimingForSlot(sceneSlot);
                    updateMonomeLEDs();
                    return;
                }
            }

            if (isSceneRecorderCell(x, y))
            {
                monomeSceneRecorderHeld = true;
                monomeSceneRecorderHoldClearTriggered = false;
                monomeSceneRecorderPressStartMs = juce::Time::getMillisecondCounter();
                monomeSceneRecorderClearBurstUntilMs = 0;
                updateMonomeLEDs();
                return;
            }

            if (topRowMode == MonomeLayoutState::TopRowMode::SceneLaunch && isSceneLaunchCell(x, y))
            {
                const int sceneSlot = juce::jlimit(0, SceneSlots - 1, x);
                const auto slotIdx = static_cast<size_t>(sceneSlot);
                const uint32_t nowMs = juce::Time::getMillisecondCounter();
                const int mainPresetIndex = getActiveMainPresetIndexForScenes();
                const int previousSceneSlot = juce::jlimit(0, SceneSlots - 1, activeSceneSlot);
                activeSceneMainPresetIndex = mainPresetIndex;

                if (sceneCopySourceSlot >= 0 && sceneCopyMainPresetIndex != mainPresetIndex)
                {
                    sceneCopySourceSlot = -1;
                    sceneCopyMainPresetIndex = 0;
                }

                const bool anyHeldBefore =
                    std::any_of(scenePadHeld.begin(), scenePadHeld.end(),
                                [](bool v) { return v; });
                const bool qualifiesForSequence = anyHeldBefore;

                scenePadHeld[slotIdx] = true;
                scenePadHoldDeleteTriggered[slotIdx] = false;
                scenePadLaunchConsumed[slotIdx] = false;
                scenePadPressStartMs[slotIdx] = nowMs;

                const bool copyArmed = sceneCopySourceSlot >= 0 && sceneCopyMainPresetIndex == mainPresetIndex;
                if (copyArmed)
                {
                    if (sceneSlot == sceneCopySourceSlot)
                    {
                        sceneCopySourceSlot = -1;
                        sceneCopyMainPresetIndex = 0;
                    }
                    else
                    {
                        const bool copied = copySceneForMainPreset(mainPresetIndex, sceneCopySourceSlot, sceneSlot);
                        sceneCopySourceSlot = -1;
                        sceneCopyMainPresetIndex = 0;
                        scenePadHoldDeleteTriggered[slotIdx] = true;
                        if (copied)
                            scenePadActionBurstUntilMs[slotIdx] = nowMs + sceneActionBurstDurationMs;
                    }

                    scenePadLaunchConsumed[slotIdx] = true;
                    scenePadLastTapMs[slotIdx] = 0;
                    updateMonomeLEDs();
                    return;
                }

                if (!anyHeldBefore || !qualifiesForSequence)
                {
                    for (int i = 0; i < SceneSlots; ++i)
                    {
                        if (i == sceneSlot)
                            continue;
                        const auto idx = static_cast<size_t>(i);
                        scenePadHeld[idx] = false;
                        scenePadHoldDeleteTriggered[idx] = false;
                        scenePadLaunchConsumed[idx] = false;
                    }
                }
                else
                {
                    auto resolveSequenceAnchor = [&]() -> int
                    {
                        int anchor = -1;
                        uint32_t longestHeldMs = 0;
                        for (int i = 0; i < SceneSlots; ++i)
                        {
                            if (i == sceneSlot)
                                continue;
                            const auto idx = static_cast<size_t>(i);
                            if (!scenePadHeld[idx])
                                continue;

                            const uint32_t heldMs = nowMs - scenePadPressStartMs[idx];
                            if (anchor < 0 || heldMs > longestHeldMs)
                            {
                                anchor = i;
                                longestHeldMs = heldMs;
                            }
                        }

                        if (anchor >= 0)
                            return anchor;
                        const int firstChainSlot = getSceneChainStepSceneSlot(0);
                        if (firstChainSlot >= 0)
                            return firstChainSlot;
                        return sceneSlot;
                    };

	                    const int anchorSlot = resolveSequenceAnchor();
	                    const int step = (sceneSlot >= anchorSlot) ? 1 : -1;
	                    std::vector<int> desiredChainSlots;
	                    desiredChainSlots.reserve(static_cast<size_t>(SceneSlots));
	                    for (int slot = anchorSlot;; slot += step)
	                    {
	                        desiredChainSlots.push_back(juce::jlimit(0, SceneSlots - 1, slot));
	                        if (slot == sceneSlot)
	                            break;
	                    }

	                    bool preserveExistingChainDefinition = getSceneChainLength()
	                        == static_cast<int>(desiredChainSlots.size());
	                    if (preserveExistingChainDefinition)
	                    {
	                        for (int stepIndex = 0; stepIndex < static_cast<int>(desiredChainSlots.size()); ++stepIndex)
	                        {
	                            if (getSceneChainStepSceneSlot(stepIndex)
	                                != desiredChainSlots[static_cast<size_t>(stepIndex)])
	                            {
	                                preserveExistingChainDefinition = false;
	                                break;
	                            }
	                        }
	                    }

	                    if (!preserveExistingChainDefinition)
	                    {
	                        clearSceneChain();
	                        for (int stepIndex = 0; stepIndex < static_cast<int>(desiredChainSlots.size()); ++stepIndex)
	                            setSceneChainStep(stepIndex, desiredChainSlots[static_cast<size_t>(stepIndex)], 1);
	                    }
	
	                    for (int i = 0; i < SceneSlots; ++i)
	                    {
                        const auto idx = static_cast<size_t>(i);
                        if (scenePadHeld[idx])
                            scenePadLaunchConsumed[idx] = true;
                    }

                    if (getSceneChainLength() >= 2)
                    {
                        const int firstSceneSlot = getSceneChainStepSceneSlot(0);
                        if (firstSceneSlot == previousSceneSlot
                            && activeScenePlaybackHandle.active
                            && std::isfinite(activeScenePlaybackHandle.startPpq))
                        {
                            sceneSequenceActive = true;
                            sceneSequenceCurrentStepIndex = 0;
                            setActiveScenePlaybackHandle(mainPresetIndex,
                                                         previousSceneSlot,
                                                         true,
                                                         0,
                                                         activeScenePlaybackHandle.startPpq,
                                                         getResolvedSceneLengthBeats(previousSceneSlot));
                            armNextSceneInSequence(mainPresetIndex,
                                                   previousSceneSlot,
                                                   activeScenePlaybackHandle.startPpq);
                        }
                        else
                        {
                            startSceneChainPlayback(0);
                        }
                    }
                }

                updateMonomeLEDs();
                return;
            }

            if (topRowMode == MonomeLayoutState::TopRowMode::PresetGrid && isPresetCell(x, y))
            {
                const int presetIndex = toPresetIndex(x, y);
                auto nowMs = juce::Time::getMillisecondCounter();
                auto& held = presetPadHeld[static_cast<size_t>(presetIndex)];
                auto& holdSaved = presetPadHoldSaveTriggered[static_cast<size_t>(presetIndex)];
                auto& deletedTap = presetPadDeleteTriggered[static_cast<size_t>(presetIndex)];
                auto& pressStart = presetPadPressStartMs[static_cast<size_t>(presetIndex)];
                auto& lastTap = presetPadLastTapMs[static_cast<size_t>(presetIndex)];

                held = true;
                holdSaved = false;
                deletedTap = false;
                pressStart = nowMs;

                const uint32_t delta = nowMs - lastTap;
                if (delta <= presetDoubleTapMs)
                {
                    deletedTap = true;
                    deletePreset(presetIndex);
                    lastTap = 0;
                }

                updateMonomeLEDs();
                return;
            }

            if (topRowMode == MonomeLayoutState::TopRowMode::StepEdit)
            {
                const int bankStart = stepEditStripBank * stepEditBankSize;

                if (x >= 0 && x <= 7)
                {
                    const auto previousTool = stepEditTool;
                    switch (x)
                    {
                        case 0: stepEditTool = StepEditTool::Velocity; break;
                        case 1: stepEditTool = StepEditTool::Divide; break;
                        case 2: stepEditTool = StepEditTool::RampUp; break;
                        case 3: stepEditTool = StepEditTool::RampDown; break;
                        case 4: stepEditTool = StepEditTool::Probability; break;
                        case 5: stepEditTool = StepEditTool::Attack; break;
                        case 6: stepEditTool = StepEditTool::Decay; break;
                        case 7: stepEditTool = StepEditTool::Release; break; // Pitch tool (reusing Release slot)
                        default: break;
                    }
                    if (stepEditTool != previousTool)
                        resetStepEditVelocityGestures();

                    updateMonomeLEDs();
                    return;
                }

                if (x >= 8 && x <= 13)
                {
                    const int targetStrip = bankStart + (x - 8);
                    if (targetStrip >= 0 && targetStrip < visibleStripCount)
                    {
                        stepEditSelectedStrip = clampVisibleStrip(targetStrip);
                        lastMonomePressedStripRow.store(stepEditSelectedStrip, std::memory_order_release);
                        setArcSelectedStripRow(stepEditSelectedStrip);
                        resetStepEditVelocityGestures();
                    }
                    updateMonomeLEDs();
                    return;
                }

                if (x == 14 && stepEditStripBank > 0)
                {
                    --stepEditStripBank;
                    stepEditStripBank = juce::jlimit(0, maxStepEditBank, stepEditStripBank);
                    const int bankStartAfter = stepEditStripBank * stepEditBankSize;
                    if (stepEditSelectedStrip < bankStartAfter
                        || stepEditSelectedStrip >= (bankStartAfter + stepEditBankSize))
                    {
                        stepEditSelectedStrip = clampVisibleStrip(bankStartAfter);
                        lastMonomePressedStripRow.store(stepEditSelectedStrip, std::memory_order_release);
                        setArcSelectedStripRow(stepEditSelectedStrip);
                        resetStepEditVelocityGestures();
                    }
                    updateMonomeLEDs();
                    return;
                }

                if (x == 15 && stepEditStripBank < maxStepEditBank)
                {
                    ++stepEditStripBank;
                    stepEditStripBank = juce::jlimit(0, maxStepEditBank, stepEditStripBank);
                    const int bankStartAfter = stepEditStripBank * stepEditBankSize;
                    if (stepEditSelectedStrip < bankStartAfter
                        || stepEditSelectedStrip >= (bankStartAfter + stepEditBankSize))
                    {
                        stepEditSelectedStrip = clampVisibleStrip(bankStartAfter);
                        lastMonomePressedStripRow.store(stepEditSelectedStrip, std::memory_order_release);
                        setArcSelectedStripRow(stepEditSelectedStrip);
                        resetStepEditVelocityGestures();
                    }
                    updateMonomeLEDs();
                    return;
                }

                return;
            }

            if (topRowMode == MonomeLayoutState::TopRowMode::Gate)
            {
                switch (x)
                {
                    case 0: setGatePageMode(GatePageMode::Adaptive); break;
                    case 1: setGatePageMode(GatePageMode::Quarter); break;
                    case 2: setGatePageMode(GatePageMode::Sixth); break;
                    case 3: setGatePageMode(GatePageMode::Eighth); break;
                    case 4: setGatePageMode(GatePageMode::Sixteenth); break;
                    default: return;
                }

                updateMonomeLEDs();
                return;
            }

            // Row 0 col 8: original momentary scratch hold.
            if (x == 8 && topRowUsesScratchAndStutter())
            {
                setMomentaryScratchHold(true);
                updateMonomeLEDs();
                return;
            }

            // Row 0, cols 9-15: momentary stutter rates (timeline-synced):
            // 9=1/2 ... 15=1/128.
            if (x >= 9 && x <= 15 && topRowUsesScratchAndStutter())
            {
                const uint8_t bit = stutterButtonBitForColumn(x);
                if (bit != 0)
                    momentaryStutterButtonMask.fetch_or(bit, std::memory_order_acq_rel);
                momentaryStutterDivisionBeats = stutterDivisionBeatsFromButton(x);
                momentaryStutterActiveDivisionButton = x;
                updateMonomeLEDs();
                setMomentaryStutterHold(true);
                return;
            }

            // FILTER MODE: Buttons 0-3 select filter sub-pages
            if (topRowMode == MonomeLayoutState::TopRowMode::Filter)
            {
                if (x == 0)
                {
                    filterSubPage = FilterSubPage::Frequency;
                    updateMonomeLEDs();
                    return;
                }
                else if (x == 1)
                {
                    filterSubPage = FilterSubPage::Resonance;
                    updateMonomeLEDs();
                    return;
                }
                else if (x == 3)  // Skip button 2, use button 3 for Type
                {
                    filterSubPage = FilterSubPage::Type;
                    updateMonomeLEDs();
                    return;
                }
                return;
            }

            if (topRowMode == MonomeLayoutState::TopRowMode::Modulation)
            {
                const int targetStrip = clampVisibleStrip(getLastMonomePressedStripRow());
                auto* engine = getAudioEngine();
                if (!engine)
                    return;
                syncMonomeModEditPageToPlayback(*engine, targetStrip);
                const bool sceneModPageWritesSceneMotion = isSceneModeEnabled()
                    && getSceneModPageMode() == SceneModPageMode::StepMotion;
                const bool sceneMainModPageWritesAutomation = sceneMainAutomationMonomeActive();
                const int sceneModStepMotionLaneStep = sceneModPageWritesSceneMotion
                    && sceneStepMotionPageNavigationActive()
                    ? modPageDirectionForColumn(x)
                    : 0;

                if (sceneModStepMotionLaneStep != 0)
                {
                    navigateSceneStepMotionLane(targetStrip, sceneModStepMotionLaneStep);
                    updateMonomeLEDs();
                    return;
                }

                const int modulationLaneStep = modPageDirectionForColumn(x);
                if (modulationLaneStep != 0)
                {
                    if (sceneModPageWritesSceneMotion && sceneStepMotionPageNavigationActive())
                        navigateSceneStepMotionLane(targetStrip, modulationLaneStep);
                    else
                        navigatePerRowModLane(*engine, targetStrip, modulationLaneStep);
                    updateMonomeLEDs();
                    return;
                }

                if (sceneMainModPageWritesAutomation)
                {
                    // In scene main automation mode the top row is navigation/indicator only.
                    updateMonomeLEDs();
                    return;
                }
                else
                {
                    const float normalizedY = 1.0f; // y=0 is highest value
                    const bool bipolar = engine->isModBipolar(targetStrip);
                    const bool rearrangeTarget = (engine->getModTarget(targetStrip) == ModernAudioEngine::ModTarget::Rearrange);
                    float value = normalizedY;
                    if (bipolar)
                    {
                        const float signedValue = (normalizedY * 2.0f) - 1.0f;
                        value = juce::jlimit(0.0f, 1.0f, (signedValue * 0.5f) + 0.5f);
                    }
                    if (rearrangeTarget)
                        value = quantizeMonomeRearrangeValue(value);
                    engine->setModStepValue(targetStrip, x, value);
                    if (sceneModPageWritesSceneMotion)
                        syncFocusedSceneMotionState();
                }

                updateMonomeLEDs();
                return;
            }
            
            // NORMAL MODE: Columns 0-3 = Group mute/unmute
            if (topRowMode == MonomeLayoutState::TopRowMode::Launch && x < 4)
            {
                auto* group = audioEngine->getGroup(x);
                if (group)
                {
                    // Toggle mute state
                    bool wasMuted = group->isMuted();
                    group->setMuted(!wasMuted);
                    
                    // If we just muted, stop all strips in the group
                    if (!wasMuted)  // Was playing, now muted
                    {
                        auto strips = group->getStrips();
                        for (int stripIdx : strips)
                        {
                            if (auto* strip = audioEngine->getStrip(stripIdx))
                                strip->stop(false);
                        }
                    }
                    else // Was muted, now unmuted: resume group strips in PPQ sync
                    {
                        const double restartTimelineBeat = audioEngine->getTimelineBeat();
                        const double restartTempo = audioEngine->getCurrentTempo();
                        const int64_t restartGlobalSample = audioEngine->getGlobalSampleCount();
                        const auto& strips = group->getStrips();
                        for (int stripIdx : strips)
                        {
                            if (auto* strip = audioEngine->getStrip(stripIdx))
                            {
                                if (!strip->hasAudio())
                                    continue;

                                const int restartColumn = juce::jlimit(0, 15, strip->getCurrentColumn());
                                if (strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Step)
                                {
                                    // Step mode follows global clock directly.
                                    strip->startStepSequencer();
                                    continue;
                                }

                                strip->restorePresetPpqState(true,
                                                             strip->isPpqTimelineAnchored(),
                                                             strip->getPpqTimelineOffsetBeats(),
                                                             restartColumn,
                                                             restartTempo,
                                                             restartTimelineBeat,
                                                             restartGlobalSample);
                            }
                        }
                    }

                    if (isSceneModeEnabled())
                        queueActiveSceneAutosave();
                }
            }
            // Columns 4-7: Pattern recorders (manual stop with auto-quantized length)
            else if (topRowMode == MonomeLayoutState::TopRowMode::Launch && x >= 4 && x <= 7)
            {
                int patternIndex = x - 4;  // 0-3 for patterns 0-3
                if (handleMonomePatternButtonPress(patternIndex, juce::Time::getMillisecondCounter()))
                {
                    DBG("Monome pattern button " << patternIndex << " pressed");
                }
            }
            else if (topRowMode != MonomeLayoutState::TopRowMode::Launch)
            {
                return;
            }
        }
        // CONTROL ROW - Mode buttons
        else if (y == CONTROL_ROW)
        {
            if (layout.topRowEditSupported && x == layout.topRowEditToggleColumn)
            {
                monomeTopRowEditOverlayActive = !monomeTopRowEditOverlayActive;
                updateMonomeLEDs();
                return;
            }
            else if (stepEditModeActive && (x == 13 || x == 14))
            {
                const int selectedStripIndex = clampVisibleStrip(stepEditSelectedStrip);
                if (auto* strip = audioEngine->getStrip(selectedStripIndex))
                {
                    const float currentSemitones = getPitchSemitonesForDisplay(*strip);
                    const float delta = (x == 13) ? -1.0f : 1.0f;
                    const float nextSemitones = juce::jlimit(-24.0f, 24.0f, currentSemitones + delta);
                    applyUserPitchControlToStrip(selectedStripIndex, nextSemitones);
                }

                updateMonomeLEDs();
                return;
            }
            else if ((!controlModeActive || currentControlMode == ControlMode::Normal)
                     && isMonomeControlRowUtilityCell(layout, x)
                     && x >= 13 && x <= 15)
            {
                const int selectedStripIndex = clampVisibleStrip(getLastMonomePressedStripRow());
                auto* strip = audioEngine->getStrip(selectedStripIndex);
                auto* sampleEngine = getSampleModeEngine(selectedStripIndex, false);
                if (strip != nullptr
                    && sampleEngine != nullptr
                    && strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Sample)
                {
                    if (x == 13)
                        sampleEngine->setTriggerMode(SampleTriggerMode::OneShot);
                    else if (x == 14)
                        sampleEngine->setTriggerMode(SampleTriggerMode::Loop);
                    else
                        sampleEngine->setLegacyLoopEngineEnabled(!sampleEngine->isLegacyLoopEngineEnabled());

                    updateMonomeLEDs();
                    return;
                }

                if (x == 15)
                    return;  // Don't process as strip trigger!
            }
            else if (x >= 0 && x < NumControlRowPages)
            {
                const bool wasStepEditMode = (controlModeActive && currentControlMode == ControlMode::StepEdit);
                const auto selectedMode = getControlModeForControlButton(x);
                if (sceneModeActive && selectedMode == ControlMode::GroupAssign)
                {
                    currentControlMode = ControlMode::Normal;
                    controlModeActive = false;
                    updateMonomeLEDs();
                    return;
                }
                if (isControlPageMomentary())
                {
                    currentControlMode = selectedMode;
                    controlModeActive = true;
                }
                else
                {
                    if (controlModeActive && currentControlMode == selectedMode)
                    {
                        currentControlMode = ControlMode::Normal;
                        controlModeActive = false;
                    }
                    else
                    {
                        currentControlMode = selectedMode;
                        controlModeActive = true;
                    }
                }
                monomeTopRowEditOverlayActive = false;

                if (controlModeActive && currentControlMode == ControlMode::StepEdit)
                {
                    if (stepEditTool == StepEditTool::Gate)
                        stepEditTool = StepEditTool::Velocity;
                    stepEditSelectedStrip = clampVisibleStrip(getLastMonomePressedStripRow());
                    stepEditStripBank = juce::jlimit(0, maxStepEditBank, stepEditSelectedStrip / stepEditBankSize);
                    resetStepEditVelocityGestures();
                }
                else if (wasStepEditMode)
                {
                    resetStepEditVelocityGestures();
                }

                updateMonomeLEDs();  // Force immediate LED update
                return;  // Don't process as strip trigger!
            }
            else if (x == 15)
            {
                return;  // Don't process as strip trigger!
            }
        }
        // STRIP ROWS
        else if (isDisplayedDataRow(y))
        {
            if (presetModeActive && isPresetCell(x, y))
            {
                const int presetIndex = toPresetIndex(x, y);
                auto nowMs = juce::Time::getMillisecondCounter();
                auto& held = presetPadHeld[static_cast<size_t>(presetIndex)];
                auto& holdSaved = presetPadHoldSaveTriggered[static_cast<size_t>(presetIndex)];
                auto& deletedTap = presetPadDeleteTriggered[static_cast<size_t>(presetIndex)];
                auto& pressStart = presetPadPressStartMs[static_cast<size_t>(presetIndex)];
                auto& lastTap = presetPadLastTapMs[static_cast<size_t>(presetIndex)];

                held = true;
                holdSaved = false;
                deletedTap = false;
                pressStart = nowMs;

                const uint32_t delta = nowMs - lastTap;
                if (delta <= presetDoubleTapMs)
                {
                    deletedTap = true;
                    deletePreset(presetIndex);
                    lastTap = 0;
                }

                updateMonomeLEDs();
                return;
            }

            if (stepEditModeActive)
            {
                if (stepEditTool == StepEditTool::Gate)
                    stepEditTool = StepEditTool::Velocity;

                const int selectedStripIndex = clampVisibleStrip(stepEditSelectedStrip);
                auto* targetStrip = audioEngine->getStrip(selectedStripIndex);
                if (!targetStrip)
                {
                    updateMonomeLEDs();
                    return;
                }

                const float rowValue = stripRowToUnit(y);
                const float columnNorm = juce::jlimit(0.0f, 1.0f, static_cast<float>(x) / 15.0f);
                auto setStepEnabled = [targetStrip](int absoluteStep, bool shouldEnable)
                {
                    const int clampedStep = juce::jlimit(0, targetStrip->getStepTotalSteps() - 1, absoluteStep);
                    if (targetStrip->stepPattern[static_cast<size_t>(clampedStep)] != shouldEnable)
                        targetStrip->toggleStepAtIndex(clampedStep);
                };

                if (stepEditTool == StepEditTool::Attack)
                {
                    targetStrip->setStepEnvelopeAttackMs(columnNorm * 400.0f);
                    updateMonomeLEDs();
                    return;
                }

                if (stepEditTool == StepEditTool::Decay)
                {
                    targetStrip->setStepEnvelopeDecayMs(1.0f + (columnNorm * 3999.0f));
                    updateMonomeLEDs();
                    return;
                }

                if (stepEditTool == StepEditTool::Release)
                {
                    const float pitchSemitones = juce::jmap(columnNorm, -24.0f, 24.0f);
                    applyUserPitchControlToStrip(selectedStripIndex, pitchSemitones);

                    updateMonomeLEDs();
                    return;
                }

                const int totalSteps = targetStrip->getStepTotalSteps();
                const int absoluteStep = targetStrip->getVisibleStepOffset() + juce::jlimit(0, 15, x);
                if (absoluteStep < 0 || absoluteStep >= totalSteps)
                {
                    updateMonomeLEDs();
                    return;
                }

                const auto stepIdx = static_cast<size_t>(absoluteStep);
                const bool wasEnabled = targetStrip->stepPattern[stepIdx];

                switch (stepEditTool)
                {
                    case StepEditTool::Gate:
                    {
                        targetStrip->toggleStepAtIndex(absoluteStep);
                        break;
                    }

                    case StepEditTool::Velocity:
                    {
                        const int column = juce::jlimit(0, MaxColumns - 1, x);
                        const uint32_t nowMs = juce::Time::getMillisecondCounter();
                        const bool sameTarget = stepEditVelocityGestureActive[static_cast<size_t>(column)]
                            && stepEditVelocityGestureStrip[static_cast<size_t>(column)] == selectedStripIndex
                            && stepEditVelocityGestureStep[static_cast<size_t>(column)] == absoluteStep;
                        const uint32_t elapsedMs = nowMs
                            - stepEditVelocityGestureLastActivityMs[static_cast<size_t>(column)];
                        const bool continueGesture = sameTarget
                            && (elapsedMs <= stepEditVelocityGestureLatchMs);

                        if (!continueGesture)
                        {
                            const float anchorStart = juce::jlimit(
                                0.0f, 1.0f, targetStrip->getStepSubdivisionStartVelocityAtIndex(absoluteStep));
                            const float anchorEnd = juce::jlimit(
                                0.0f, 1.0f, targetStrip->getStepSubdivisionRepeatVelocityAtIndex(absoluteStep));
                            stepEditVelocityGestureActive[static_cast<size_t>(column)] = true;
                            stepEditVelocityGestureStrip[static_cast<size_t>(column)] = selectedStripIndex;
                            stepEditVelocityGestureStep[static_cast<size_t>(column)] = absoluteStep;
                            stepEditVelocityGestureAnchorStart[static_cast<size_t>(column)] = anchorStart;
                            stepEditVelocityGestureAnchorEnd[static_cast<size_t>(column)] = anchorEnd;
                            stepEditVelocityGestureAnchorValue[static_cast<size_t>(column)] = juce::jmax(anchorStart, anchorEnd);
                        }

                        stepEditVelocityGestureLastActivityMs[static_cast<size_t>(column)] = nowMs;

                        // Bottom row (y=6) in volume tool is an explicit step-off command.
                        const bool explicitOff = (y >= (CONTROL_ROW - 1));
                        if (explicitOff)
                        {
                            setStepEnabled(absoluteStep, false);
                            targetStrip->setStepSubdivisionVelocityRangeAtIndex(absoluteStep, 0.0f, 0.0f);
                            stepEditVelocityGestureActive[static_cast<size_t>(column)] = false;
                            break;
                        }

                        const float dragShift = rowValue
                            - stepEditVelocityGestureAnchorValue[static_cast<size_t>(column)];
                        const float start = juce::jlimit(
                            0.0f,
                            1.0f,
                            stepEditVelocityGestureAnchorStart[static_cast<size_t>(column)] + dragShift);
                        const float end = juce::jlimit(
                            0.0f,
                            1.0f,
                            stepEditVelocityGestureAnchorEnd[static_cast<size_t>(column)] + dragShift);

                        targetStrip->setStepSubdivisionVelocityRangeAtIndex(absoluteStep, start, end);
                        const bool shouldEnable = juce::jmax(start, end) > 0.001f;
                        setStepEnabled(absoluteStep, shouldEnable);
                        break;
                    }

                    case StepEditTool::Divide:
                    {
                        setStepEnabled(absoluteStep, true);
                        const int maxSubs = juce::jmax(2, EnhancedAudioStrip::MaxStepSubdivisions);
                        // Map row value directly to the visible subdivision scale.
                        // (Old mapping had a +2 bias that made low values feel offset.)
                        const int subdivisions = juce::jlimit(
                            2,
                            maxSubs,
                            1 + static_cast<int>(std::round(rowValue
                                * static_cast<float>(juce::jmax(1, maxSubs - 1)))));
                        targetStrip->setStepSubdivisionAtIndex(absoluteStep, subdivisions);
                        break;
                    }

                    case StepEditTool::RampUp:
                    {
                        setStepEnabled(absoluteStep, true);
                        if (rowValue <= 0.001f)
                            targetStrip->setStepSubdivisionAtIndex(absoluteStep, 2);
                        else if (targetStrip->getStepSubdivisionAtIndex(absoluteStep) <= 1)
                            targetStrip->setStepSubdivisionAtIndex(absoluteStep, 2);

                        const float baseStart = targetStrip->getStepSubdivisionStartVelocityAtIndex(absoluteStep);
                        const float baseEnd = targetStrip->getStepSubdivisionRepeatVelocityAtIndex(absoluteStep);
                        float baseMax = juce::jmax(baseStart, baseEnd);
                        if (baseMax < 0.001f)
                            baseMax = wasEnabled ? 1.0f : juce::jmax(0.25f, rowValue);

                        const float depth = rowValue;
                        const float start = juce::jlimit(0.0f, 1.0f, (1.0f - depth) * baseMax);
                        const float end = juce::jlimit(0.0f, 1.0f, baseMax);
                        targetStrip->setStepSubdivisionVelocityRangeAtIndex(absoluteStep, start, end);
                        break;
                    }

                    case StepEditTool::RampDown:
                    {
                        setStepEnabled(absoluteStep, true);
                        if (rowValue <= 0.001f)
                            targetStrip->setStepSubdivisionAtIndex(absoluteStep, 2);
                        else if (targetStrip->getStepSubdivisionAtIndex(absoluteStep) <= 1)
                            targetStrip->setStepSubdivisionAtIndex(absoluteStep, 2);

                        const float baseStart = targetStrip->getStepSubdivisionStartVelocityAtIndex(absoluteStep);
                        const float baseEnd = targetStrip->getStepSubdivisionRepeatVelocityAtIndex(absoluteStep);
                        float baseMax = juce::jmax(baseStart, baseEnd);
                        if (baseMax < 0.001f)
                            baseMax = wasEnabled ? 1.0f : juce::jmax(0.25f, rowValue);

                        const float depth = rowValue;
                        const float start = juce::jlimit(0.0f, 1.0f, baseMax);
                        const float end = juce::jlimit(0.0f, 1.0f, (1.0f - depth) * baseMax);
                        targetStrip->setStepSubdivisionVelocityRangeAtIndex(absoluteStep, start, end);
                        break;
                    }

                    case StepEditTool::Probability:
                    {
                        if (rowValue > 0.001f)
                            setStepEnabled(absoluteStep, true);
                        targetStrip->setStepProbabilityAtIndex(absoluteStep, rowValue);
                        break;
                    }

                    case StepEditTool::Attack:
                    case StepEditTool::Decay:
                    case StepEditTool::Release:
                    default:
                        break;
                }

                updateMonomeLEDs();
                return;
            }

            int stripIndex = y - FIRST_STRIP_ROW;
            if (stripIndex >= 0 && stripIndex < visibleStripCount && x < MaxColumns)
            {
                const bool usesSelectedTargetStrip =
                    controlModeActive
                    && (currentControlMode == ControlMode::GrainSize
                        || currentControlMode == ControlMode::Delay
                        || currentControlMode == ControlMode::Modulation);

                if (!usesSelectedTargetStrip)
                    lastMonomePressedStripRow.store(stripIndex, std::memory_order_release);
                if (!usesSelectedTargetStrip)
                    setArcSelectedStripRow(stripIndex);
                auto* strip = audioEngine->getStrip(stripIndex);
                if (!strip) 
                {
                    // Clear any stale loop setting state
                    loopSetFirstButton = -1;
                    loopSetStrip = -1;
                    return;
                }

                const bool normalPlaybackMode = (!controlModeActive || currentControlMode == ControlMode::Normal);
                const bool stopRowComboPressed =
                    normalPlaybackMode
                    && strip->getPlayMode() != EnhancedAudioStrip::PlayMode::Step
                    && strip->getPlayMode() != EnhancedAudioStrip::PlayMode::Sample
                    && ((x == 0 && strip->isButtonHeld(1))
                        || (x == 1 && strip->isButtonHeld(0)));
                if (stopRowComboPressed)
                {
                    stopStrip(stripIndex, false, strip->getHeldButton());
                    loopSetFirstButton = -1;
                    loopSetStrip = -1;
                    updateMonomeLEDs();
                    return;
                }
                
                // Loop length setting mode - ONLY if scratch is disabled and strip is not in Step mode.
                if (strip->getPlayMode() != EnhancedAudioStrip::PlayMode::Step
                    && strip->getPlayMode() != EnhancedAudioStrip::PlayMode::Sample
                    && loopSetFirstButton >= 0
                    && loopSetStrip == stripIndex
                    && strip->isButtonHeld(loopSetFirstButton)
                    && std::abs(strip->getScratchAmount()) <= kScratchZeroEpsilon)
                {
                    const int firstButton = juce::jlimit(0, MaxColumns - 1, loopSetFirstButton);
                    const int secondButton = juce::jlimit(0, MaxColumns - 1, x);
                    int start = juce::jmin(firstButton, secondButton);
                    int end = juce::jmax(firstButton, secondButton) + 1;
                    const int originalLength = juce::jmax(1, end - start);

                    // Detect reverse: first button > second button
                    const bool shouldReverse = (firstButton > secondButton);

                    // Global inner-loop size divisor:
                    // 1, 1/2, 1/4, 1/8, 1/16 where 1 keeps legacy behavior.
                    const float loopLengthFactor = juce::jlimit(0.0625f, 1.0f, getInnerLoopLengthFactor());
                    if (loopLengthFactor < 0.999f)
                    {
                        const int scaledLength = juce::jmax(1, static_cast<int>(
                            std::floor(static_cast<double>(originalLength) * static_cast<double>(loopLengthFactor))));

                        if (shouldReverse)
                        {
                            end = juce::jlimit(1, MaxColumns, firstButton + 1);
                            start = juce::jmax(0, end - scaledLength);
                        }
                        else
                        {
                            start = firstButton;
                            end = juce::jmin(MaxColumns, start + scaledLength);
                        }

                        start = juce::jlimit(0, MaxColumns - 1, start);
                        end = juce::jlimit(start + 1, MaxColumns, end);
                    }

                    const int appliedLength = juce::jmax(1, end - start);
                    const double desiredScaledLength = static_cast<double>(originalLength)
                        * static_cast<double>(loopLengthFactor);
                    float beatsPerLoopOverride = std::numeric_limits<float>::quiet_NaN();
                    if (strip->hasInnerLoopTempoOverride()
                        || std::abs(desiredScaledLength - static_cast<double>(appliedLength)) > 1.0e-6)
                    {
                        float baseBeatsPerLoop = strip->getInnerLoopBaseBeatsPerLoop();
                        if (!(baseBeatsPerLoop > 0.0f))
                            baseBeatsPerLoop = 4.0f;

                        beatsPerLoopOverride = juce::jmax(
                            0.25f,
                            static_cast<float>(
                                static_cast<double>(baseBeatsPerLoop)
                                * desiredScaledLength
                                / static_cast<double>(appliedLength)));
                    }

                    queueLoopChange(stripIndex,
                                    false,
                                    start,
                                    end,
                                    shouldReverse,
                                    -1,
                                    beatsPerLoopOverride);
                    
                    DBG("Inner loop set: " << start << "-" << end << 
                        (shouldReverse ? " (REVERSE)" : " (NORMAL)"));
                    
                    loopSetFirstButton = -1;
                    loopSetStrip = -1;
                }
                // Control modes - adjust parameters
                else if (controlModeActive && currentControlMode != ControlMode::Normal)
                {
                    auto handleSimpleMixControlPress = [&]()
                    {
                        MonomeMixActions::handleButtonPress(*this, *strip, stripIndex, x, static_cast<int>(currentControlMode));
                        recordMonomeControlPatternEvent(currentControlMode, stripIndex, -1, x);
                    };
                    auto applySceneAwareMonomeControl = [this, stripIndex, x]
                        (ScenePerformanceControlTarget target,
                         ControlMode controlMode,
                         int controlRow,
                         float value,
                         auto&& applyWrite)
                    {
                        applyLiveSceneControlTouch(stripIndex,
                                                   target,
                                                   controlMode,
                                                   controlRow,
                                                   value,
                                                   x,
                                                   std::forward<decltype(applyWrite)>(applyWrite));
                    };

                    switch (currentControlMode)
                    {
                        case ControlMode::Speed:
                        case ControlMode::Pitch:
                        case ControlMode::Swing:
                        case ControlMode::Gate:
                            handleSimpleMixControlPress();
                            break;

                        case ControlMode::Pan:
                        {
                            float pan = (x - 8) / 8.0f;
                            pan = juce::jlimit(-1.0f, 1.0f, pan);
                            applySceneAwareMonomeControl(ScenePerformanceControlTarget::Pan,
                                                         ControlMode::Pan,
                                                         0,
                                                         pan,
                                                         [this, stripIndex, pan](StripControlWriteMode writeMode)
                                                         {
                                                             setStripPanControlValue(stripIndex, pan, writeMode);
                                                         });
                            break;
                        }

                        case ControlMode::Volume:
                        {
                            const float volume = juce::jlimit(0.0f, 1.0f, x / 15.0f);
                            applySceneAwareMonomeControl(ScenePerformanceControlTarget::Volume,
                                                         ControlMode::Volume,
                                                         0,
                                                         volume,
                                                         [this, stripIndex, volume](StripControlWriteMode writeMode)
                                                         {
                                                             setStripVolumeControlValue(stripIndex, volume, writeMode);
                                                         });
                            break;
                        }

                        case ControlMode::GrainSize:
                        {
                            const int targetStripIndex = clampVisibleStrip(getLastMonomePressedStripRow());
                            if (auto* targetStrip = audioEngine->getStrip(targetStripIndex))
                            {
                                MonomeMixActions::handleGrainPageButtonPress(*this,
                                                                            *targetStrip,
                                                                            targetStripIndex,
                                                                            stripIndex,
                                                                            x);
                                recordMonomeControlPatternEvent(currentControlMode, targetStripIndex, stripIndex, x);
                            }
                            break;
                        }

                        case ControlMode::Filter:
                            if (filterSubPage == FilterSubPage::Frequency)
                            {
                                const float t = juce::jlimit(0.0f, 1.0f, x / 15.0f);
                                const float frequency = 20.0f * std::pow(1000.0f, t);
                                applySceneAwareMonomeControl(ScenePerformanceControlTarget::FilterFrequency,
                                                             ControlMode::Filter,
                                                             0,
                                                             frequency,
                                                             [this, stripIndex, frequency](StripControlWriteMode writeMode)
                                                             {
                                                                 setStripFilterFrequencyControlValue(stripIndex,
                                                                                                frequency,
                                                                                                writeMode);
                                                             });
                            }
                            else if (filterSubPage == FilterSubPage::Resonance)
                            {
                                const float resonance = 0.1f + (x / 15.0f) * 9.9f;
                                applySceneAwareMonomeControl(ScenePerformanceControlTarget::FilterResonance,
                                                             ControlMode::Filter,
                                                             1,
                                                             resonance,
                                                             [this, stripIndex, resonance](StripControlWriteMode writeMode)
                                                             {
                                                                 setStripFilterResonanceControlValue(stripIndex,
                                                                                                resonance,
                                                                                                writeMode);
                                                             });
                            }
                            else if (filterSubPage == FilterSubPage::Type && x <= 2)
                            {
                                const float morph = (x == 0) ? 0.0f : (x == 1 ? 0.5f : 1.0f);
                                applySceneAwareMonomeControl(ScenePerformanceControlTarget::FilterMorph,
                                                             ControlMode::Filter,
                                                             2,
                                                             morph,
                                                             [this, stripIndex, morph](StripControlWriteMode writeMode)
                                                             {
                                                                 setStripFilterMorphControlValue(stripIndex,
                                                                                            morph,
                                                                                            writeMode);
                                                             });
                            }
                            break;

                        case ControlMode::Delay:
                        {
                            const int targetStripIndex = clampVisibleStrip(getLastMonomePressedStripRow());
                            if (auto* targetStrip = audioEngine->getStrip(targetStripIndex))
                            {
                                MonomeMixActions::handleDelayPageButtonPress(*this, *targetStrip, targetStripIndex, stripIndex, x);
                                recordMonomeControlPatternEvent(currentControlMode, targetStripIndex, stripIndex, x);
                            }
                            break;
                        }

                        case ControlMode::FileBrowser:
                            MonomeFileBrowserActions::handleButtonPress(*this, *strip, stripIndex, x);
                            break;

                        case ControlMode::GroupAssign:
                            if (MonomeGroupAssignActions::handleButtonPress(*audioEngine, stripIndex, x))
                                updateMonomeLEDs();
                            break;

                        case ControlMode::Modulation:
                        {
                            const int targetStrip = clampVisibleStrip(getLastMonomePressedStripRow());
                            syncMonomeModEditPageToPlayback(*audioEngine, targetStrip);
                            const bool sceneModPageWritesSceneMotion = isSceneModeEnabled()
                                && getSceneModPageMode() == SceneModPageMode::StepMotion;
                            if (sceneMainAutomationMonomeActive())
                            {
                                const float normalizedY = sceneMainAutomationRowToUnit(y);
                                const double currentBeat = audioEngine->getTimelineBeat();
                                writeSceneMainAutomationColumn(*this, targetStrip, x, normalizedY, currentBeat);
                            }
                            else
                            {
                                const float normalizedY = modulationRowToUnitForColumn(y, x);
                                const bool bipolar = audioEngine->isModBipolar(targetStrip);
                                const bool rearrangeTarget = (audioEngine->getModTarget(targetStrip) == ModernAudioEngine::ModTarget::Rearrange);
                                float value = normalizedY;
                                if (bipolar)
                                {
                                    // In bipolar mode, center row maps to 0.5 and extremes map to 0/1.
                                    const float signedValue = (normalizedY * 2.0f) - 1.0f;
                                    value = juce::jlimit(0.0f, 1.0f, (signedValue * 0.5f) + 0.5f);
                                }
                                if (rearrangeTarget)
                                    value = quantizeMonomeRearrangeValue(value);
                                audioEngine->setModStepValue(targetStrip, x, value);
                                if (sceneModPageWritesSceneMotion)
                                    syncFocusedSceneMotionState();
                            }
                            updateMonomeLEDs();
                            break;
                        }

                        case ControlMode::Normal:
                        case ControlMode::Preset:
                        case ControlMode::StepEdit:
                        default:
                            break;
                    }
                }
                else
                {
                    // Normal playback trigger:
                    // - Loop/Grain/Gate: requires loaded strip audio
                    // - Step mode: allow direct step toggling on main page
                    const bool canTriggerFromMainPage = (strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Step)
                        || strip->hasAudio()
                        || (strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Sample && hasSampleModeAudio(stripIndex));
                    if (canTriggerFromMainPage)
                    {
                        // Always notify strip of press for scratch hold-state.
                        // Actual scratch motion still starts when trigger fires,
                        // so quantized scheduling remains sample-accurate.
                        if (strip->getPlayMode() != EnhancedAudioStrip::PlayMode::Sample)
                        {
                            int64_t globalSample = audioEngine->getGlobalSampleCount();
                            strip->onButtonPress(x, globalSample);
                        }
                        else
                        {
                            setSampleModeHeldVisibleSliceSlot(stripIndex, x);
                        }
                        
                        // Trigger the strip (quantized or immediate)
                        triggerStrip(stripIndex, x);
                        
                        // Set up for potential loop range setting (non-step modes only).
                        if (strip->getPlayMode() != EnhancedAudioStrip::PlayMode::Step
                            && strip->getPlayMode() != EnhancedAudioStrip::PlayMode::Sample)
                        {
                            loopSetFirstButton = x;
                            loopSetStrip = stripIndex;
                        }
                    }
                    // If no sample loaded, do nothing (just show visual feedback via LEDs)
                }
            }
        }
    }
    else if (state == 0) // Key up
    {
        if (isSceneRecorderCell(x, y))
        {
            const bool holdTriggered = monomeSceneRecorderHoldClearTriggered;
            monomeSceneRecorderHeld = false;
            monomeSceneRecorderHoldClearTriggered = false;
            monomeSceneRecorderPressStartMs = 0;

            if (!holdTriggered)
                handleMonomeSceneRecorderButtonPress(juce::Time::getMillisecondCounter());

            updateMonomeLEDs();
            return;
        }

        if (topRowMode == MonomeLayoutState::TopRowMode::SceneLaunch && isSceneLaunchCell(x, y))
        {
            const int sceneSlot = juce::jlimit(0, SceneSlots - 1, x);
            const auto slotIdx = static_cast<size_t>(sceneSlot);
            const bool shouldLaunchScene = scenePadHeld[slotIdx] && !scenePadLaunchConsumed[slotIdx];
            scenePadHeld[slotIdx] = false;
            scenePadHoldDeleteTriggered[slotIdx] = false;
            scenePadLaunchConsumed[slotIdx] = false;

            if (shouldLaunchScene)
                launchSceneSlotFromMonome(sceneSlot);

            updateMonomeLEDs();
            return;
        }

        if (presetModeActive && isPresetCell(x, y))
        {
            const int presetIndex = toPresetIndex(x, y);
            auto nowMs = juce::Time::getMillisecondCounter();
            auto& held = presetPadHeld[static_cast<size_t>(presetIndex)];
            auto& holdSaved = presetPadHoldSaveTriggered[static_cast<size_t>(presetIndex)];
            auto& deletedTap = presetPadDeleteTriggered[static_cast<size_t>(presetIndex)];
            auto& lastTap = presetPadLastTapMs[static_cast<size_t>(presetIndex)];

            if (held && !holdSaved && !deletedTap)
                loadPreset(presetIndex);

            held = false;
            holdSaved = false;
            deletedTap = false;
            lastTap = nowMs;

            updateMonomeLEDs();
            return;
        }

        if (isDisplayedDataRow(y))
        {
            int stripIndex = y - FIRST_STRIP_ROW;
            if (stripIndex >= 0 && stripIndex < visibleStripCount && x >= 3 && x < (3 + BrowserFavoriteSlots))
            {
                const int slot = x - 3;
                const bool browserModeActive = controlModeActive && currentControlMode == ControlMode::FileBrowser;
                const bool favoriteWasHeld = isBrowserFavoritePadHeld(stripIndex, slot);
                if (browserModeActive || favoriteWasHeld)
                {
                    if (auto* strip = audioEngine->getStrip(stripIndex))
                    {
                        MonomeFileBrowserActions::handleButtonRelease(*this, *strip, stripIndex, x);
                        updateMonomeLEDs();
                        return;
                    }
                }
            }
        }

        if (stepEditModeActive && y == GROUP_ROW)
        {
            updateMonomeLEDs();
            return;
        }

        if (y == GROUP_ROW && x == 8)
        {
            setMomentaryScratchHold(false);
            updateMonomeLEDs();
            return;
        }
        if (y == GROUP_ROW && x >= 9 && x <= 15)
        {
            const int releasedColumn = x;
            const int previousActiveColumn = momentaryStutterActiveDivisionButton;
            const uint8_t bit = stutterButtonBitForColumn(x);
            uint8_t currentMask = momentaryStutterButtonMask.load(std::memory_order_acquire);
            currentMask = static_cast<uint8_t>(currentMask & static_cast<uint8_t>(~bit));
            momentaryStutterButtonMask.store(currentMask, std::memory_order_release);

            if (currentMask == 0)
            {
                if (previousActiveColumn >= 9 && previousActiveColumn <= 15)
                    momentaryStutterRecordedDivisionButton = previousActiveColumn;
                else
                    momentaryStutterRecordedDivisionButton = releasedColumn;
                setMomentaryStutterHold(false);
            }
            else
            {
                const int activeColumn = stutterColumnFromMask(currentMask);
                if (activeColumn >= 9 && activeColumn <= 15)
                {
                    momentaryStutterActiveDivisionButton = activeColumn;
                    momentaryStutterDivisionBeats = stutterDivisionBeatsFromButton(activeColumn);
                    momentaryStutterRecordedDivisionButton = activeColumn;
                    audioEngine->setMomentaryStutterDivision(momentaryStutterDivisionBeats);
                    if (momentaryStutterPlaybackActive.load(std::memory_order_acquire) != 0)
                    {
                        recordMomentaryStutterSceneDivision(momentaryStutterDivisionBeats,
                                                            activeColumn,
                                                            audioEngine != nullptr ? audioEngine->getTimelineBeat() : 0.0);
                    }
                }
            }
            updateMonomeLEDs();
            return;
        }

        if (stepEditModeActive && isDisplayedDataRow(y))
        {
            updateMonomeLEDs();
            return;
        }

        // Notify strip of button release (for musical scratching)
        if (isDisplayedDataRow(y))
        {
            int stripIndex = y - FIRST_STRIP_ROW;
            if (stripIndex >= 0 && stripIndex < visibleStripCount && x < MaxColumns)
            {
                auto* strip = audioEngine->getStrip(stripIndex);
                if (strip)
                {
                    if (strip->getPlayMode() != EnhancedAudioStrip::PlayMode::Sample)
                    {
                        int64_t globalSample = audioEngine->getGlobalSampleCount();
                        strip->onButtonRelease(x, globalSample);
                    }
                    else
                    {
                        clearSampleModeHeldVisibleSliceSlot(stripIndex, x);
                    }
                }
            }
        }
        
        // Gate-playback release should only stop strips during normal launch use,
        // not while a control page is open and rows are editing parameters.
        if ((!controlModeActive || currentControlMode == ControlMode::Normal)
            && isDisplayedDataRow(y))
        {
            int stripIndex = y - FIRST_STRIP_ROW;
            if (stripIndex >= 0 && stripIndex < visibleStripCount && x < MaxColumns)
            {
                auto* strip = audioEngine->getStrip(stripIndex);
                if (strip && strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Gate)
                {
                    stopStrip(stripIndex, true, x);
                }
            }
        }
        
        // Release control mode in momentary behavior (control-page buttons)
        if (isControlPageMomentary()
            && y == CONTROL_ROW
            && (x >= 0 && x < NumControlRowPages)
            && !isMonomeControlRowUtilityCell(layout, x))
        {
            const bool wasStepEditMode = (controlModeActive && currentControlMode == ControlMode::StepEdit);
            currentControlMode = ControlMode::Normal;
            controlModeActive = false;
            monomeTopRowEditOverlayActive = false;
            if (wasStepEditMode)
                resetStepEditVelocityGestures();
            updateMonomeLEDs();  // Update LEDs when returning to normal
        }
        
        // Reset loop setting
        if (isDisplayedDataRow(y))
        {
            int stripIndex = y - FIRST_STRIP_ROW;
            if (stripIndex == loopSetStrip && x == loopSetFirstButton)
            {
                loopSetFirstButton = -1;
                loopSetStrip = -1;
            }
        }
    }
    
    updateMonomeLEDs();
}
void MlrVSTAudioProcessor::updateMonomeLEDs()
{
    if (monomeLedUpdateInProgress)
    {
        monomeLedUpdatePending = true;
        return;
    }

    monomeLedUpdateInProgress = true;
    const auto monomeLedUpdateGuard = juce::ScopeGuard{ [this]
    {
        monomeLedUpdateInProgress = false;

        if (monomeLedUpdatePending)
        {
            monomeLedUpdatePending = false;
            updateMonomeLEDs();
        }
    }};

    if (!monomeConnection.isConnected() || !audioEngine || !monomeConnection.supportsGrid())
        return;
    
    const auto layout = getMonomeLayoutState();
    const int gridWidth = layout.gridWidth;
    const int gridHeight = layout.gridHeight;
    const int GROUP_ROW = layout.groupRow;
    const int FIRST_STRIP_ROW = layout.firstStripRow;
    const int CONTROL_ROW = layout.controlRow;
    const int visibleStripCount = layout.visibleStripCount;
    const int stepEditBankSize = layout.stepEditBankSize;
    const int maxStepEditBank = layout.maxStepEditBank;
    stepEditStripBank = juce::jlimit(0, maxStepEditBank, stepEditStripBank);
    const int modulationMaxRow = juce::jmax(1, visibleStripCount);
    const int sceneAutomationTopRow = GROUP_ROW + 1;
    const int sceneAutomationBottomRow = juce::jmax(sceneAutomationTopRow, modulationMaxRow);
    const int sceneAutomationUsableRows = juce::jmax(1, sceneAutomationBottomRow - sceneAutomationTopRow);
    const bool sceneModeActive = layout.sceneModeActive;
    const auto topRowMode = layout.topRowMode;
    const auto clampVisibleStrip = [&layout](int index)
    {
        return layout.clampVisibleStrip(index);
    };
    const auto stepEditRowNorm = [CONTROL_ROW, visibleStripCount](int row)
    {
        const int denom = juce::jmax(1, visibleStripCount - 1);
        return juce::jlimit(0.0f, 1.0f,
            static_cast<float>((CONTROL_ROW - 1) - row) / static_cast<float>(denom));
    };
    const auto sceneAutomationValueToRow = [sceneAutomationTopRow, sceneAutomationBottomRow, sceneAutomationUsableRows](float v)
    {
        return juce::jlimit(sceneAutomationTopRow,
                            sceneAutomationBottomRow,
                            sceneAutomationTopRow + static_cast<int>(std::round((1.0f - juce::jlimit(0.0f, 1.0f, v))
                                                                               * static_cast<float>(sceneAutomationUsableRows))));
    };
    const auto sceneAutomationRowValue01 = [sceneAutomationTopRow, sceneAutomationBottomRow, sceneAutomationUsableRows](int row)
    {
        return juce::jlimit(0.0f,
                            1.0f,
                            static_cast<float>(sceneAutomationBottomRow - juce::jlimit(sceneAutomationTopRow,
                                                                                       sceneAutomationBottomRow,
                                                                                       row))
                                / static_cast<float>(sceneAutomationUsableRows));
    };
    const auto sceneStepMotionPageNavigationActive = [&]()
    {
        const bool sceneStepMotionMonomePageActive = isControlModeActive()
            && getCurrentControlMode() == ControlMode::Modulation;
        return isSceneModeEnabled()
            && getSceneModPageMode() == SceneModPageMode::StepMotion
            && (isSceneStepMotionEditorOpen() || sceneStepMotionMonomePageActive);
    };
    const auto topRowUsesScratchAndStutter = [&]()
    {
        return (!controlModeActive || currentControlMode == ControlMode::Normal)
            && (topRowMode == MonomeLayoutState::TopRowMode::Launch
                || (topRowMode == MonomeLayoutState::TopRowMode::SceneLaunch
                    && !sceneStepMotionPageNavigationActive()));
    };
    const auto sceneStepMotionLaneNavigationAvailable = [this](int stripIndex)
    {
        const int safeStripIndex = juce::jlimit(0, MaxStrips - 1, stripIndex);
        return getSceneVisibleModTargetsForStrip(safeStripIndex).size() > 1;
    };
    const auto sceneMainAutomationMonomeActive = [this]()
    {
        return isSceneMainAutomationMonomeActive(*this);
    };
    const auto sceneModRepeatButtonsVisible = [&]()
    {
        return sceneModeActive
            && controlModeActive
            && currentControlMode == ControlMode::Modulation
            && (topRowMode == MonomeLayoutState::TopRowMode::SceneLaunch
                || topRowMode == MonomeLayoutState::TopRowMode::Modulation);
    };
    const auto regularModLaneNavigationAvailable = [this](int stripIndex)
    {
        const int safeStripIndex = juce::jlimit(0, MaxStrips - 1, stripIndex);
        return getVisibleModTargetsForStrip(safeStripIndex).size() > 1;
    };
    const auto sceneStepMotionLaneNavigationLevel = [](bool canNavigate)
    {
        return canNavigate ? 10 : 3;
    };
    const auto regularModPageLaneNavigationVisible = [&]()
    {
        return controlModeActive
            && currentControlMode == ControlMode::Modulation
            && topRowMode != MonomeLayoutState::TopRowMode::Modulation
            && !sceneStepMotionPageNavigationActive();
    };
    const auto syncMonomeModEditPageToPlayback = [&](int stripIndex)
    {
        if (sceneStepMotionPageNavigationActive() || sceneMainAutomationMonomeActive())
            return false;

        if (stripIndex < 0 || stripIndex >= MaxStrips)
            return false;

        auto* strip = audioEngine->getStrip(stripIndex);
        if (strip == nullptr || !strip->isPlaying())
            return false;

        const int playbackPage = audioEngine->getModCurrentPage(stripIndex);
        if (audioEngine->getModEditPage(stripIndex) != playbackPage)
            audioEngine->setModEditPage(stripIndex, playbackPage);
        return true;
    };
    
    // Temporary LED state
    int newLedState[MaxGridWidth][MaxGridHeight] = {{0}};
    const double beatNow = audioEngine->getTimelineBeat();
    const bool hostTransportPlaying = lastHostTransportPlaying.load(std::memory_order_acquire) != 0;
    const bool fastBlinkOn = hostTransportPlaying && (std::fmod(beatNow * 2.0, 1.0) < 0.5);  // Twice per beat
    const bool slowBlinkOn = hostTransportPlaying && (std::fmod(beatNow, 1.0) < 0.5);        // Once per beat
    const double beatPhase = std::fmod(beatNow, 1.0);
    const bool metroPulseOn = hostTransportPlaying && (beatPhase < 0.22);                     // Short pulse at each beat
    const int beatIndexInBar = juce::jmax(0, static_cast<int>(std::floor(beatNow)) % 4);
    const bool metroDownbeat = (beatIndexInBar == 0);
    const int metroOffLevel = 1;
    const int metroBeatLevel = 10;
    const int metroDownbeatLevel = 15;
    const auto nowMs = juce::Time::getMillisecondCounter();
    const int sceneMainModStripIndex = sceneMainAutomationMonomeActive()
        ? clampVisibleStrip(getLastMonomePressedStripRow())
        : -1;
    const int sceneMainModSceneSlot = sceneMainAutomationMonomeActive()
        ? getFocusedSceneSlot()
        : -1;
    const auto sceneMainModTarget = sceneMainAutomationMonomeActive()
        ? activeSceneMainAutomationTarget(*this, sceneMainModStripIndex)
        : ScenePerformanceControlTarget::None;
    const bool sceneMainModTargetBipolar = sceneAutomationTargetIsBipolarForMonome(sceneMainModTarget);
    const double sceneMainModLengthBeats = sceneMainAutomationMonomeActive()
        ? juce::jmax(1.0, getResolvedSceneLengthBeats(sceneMainModSceneSlot))
        : 1.0;
    const auto sceneMainModEvents = sceneMainAutomationMonomeActive()
        ? getScenePerformanceEventsSnapshot(sceneMainModSceneSlot)
        : std::vector<ScenePerformanceEvent>{};
    const int sceneMainModDisplayPage = sceneMainAutomationMonomeActive()
        ? sceneMainAutomationDisplayPage(*this, sceneMainModSceneSlot, beatNow)
        : 0;
    int sceneMainModActiveStep = -1;
    if (sceneMainAutomationMonomeActive())
        sceneMainAutomationPlaybackStepForPage(*this, sceneMainModSceneSlot, sceneMainModDisplayPage, beatNow, sceneMainModActiveStep);
    const auto sceneMainModColumnValue = [&](int column, float& normalizedOut)
    {
        if (!sceneMainAutomationMonomeActive() || sceneMainModTarget == ScenePerformanceControlTarget::None)
            return false;

        double beat = 0.0;
        if (!sceneMainAutomationBeatForColumn(*this, sceneMainModSceneSlot, column, beatNow, beat))
            return false;

        normalizedOut = sceneMainAutomationColumnNormalizedValue(*this,
                                                                 sceneMainModEvents,
                                                                 sceneMainModSceneSlot,
                                                                 sceneMainModStripIndex,
                                                                 sceneMainModTarget,
                                                                 beat,
                                                                 sceneMainModLengthBeats);
        return true;
    };
    const int activeSceneMainPreset = getActiveMainPresetIndexForScenes();
    auto updateSceneSlotLeds = [&]()
    {
        const bool chainPlaying = isSceneChainPlaybackActive();
        const int chainPlaybackStep = getSceneChainPlaybackStepIndex();
        const int chainPlaybackScene = chainPlaybackStep >= 0
            ? getSceneChainStepSceneSlot(chainPlaybackStep)
            : -1;
        const int visibleSceneSlots = juce::jmin(SceneSlots, kMonomeSceneLaunchColumns);
        for (int sceneSlot = 0; sceneSlot < visibleSceneSlots; ++sceneSlot)
        {
            const auto slotIdx = static_cast<size_t>(sceneSlot);
            if (!scenePadHeld[slotIdx] || scenePadHoldDeleteTriggered[slotIdx])
                continue;

            const uint32_t elapsed = nowMs - scenePadPressStartMs[slotIdx];
            if (elapsed < sceneHoldDeleteMs)
                continue;

            const bool saved = saveSceneForMainPreset(activeSceneMainPreset, sceneSlot);
            if (!saved && !sceneSlotExistsForMainPreset(activeSceneMainPreset, sceneSlot))
                continue;

            sceneCopySourceSlot = sceneSlot;
            sceneCopyMainPresetIndex = activeSceneMainPreset;
            scenePadHoldDeleteTriggered[slotIdx] = true;
            scenePadLaunchConsumed[slotIdx] = true;
            scenePadLastTapMs[slotIdx] = 0;
            scenePadActionBurstUntilMs[slotIdx] = nowMs + sceneActionBurstDurationMs;
        }

        for (int sceneSlot = 0; sceneSlot < visibleSceneSlots; ++sceneSlot)
        {
            const auto slotIdx = static_cast<size_t>(sceneSlot);
            const bool exists = sceneSlotExistsForMainPreset(activeSceneMainPreset, sceneSlot);
            const bool held = scenePadHeld[slotIdx];
            const bool active = (sceneSlot == activeSceneSlot);
            const bool inSequence = chainPlaying && getSceneSequenceStepIndex(sceneSlot) >= 0;
            const bool sequenceCurrent = chainPlaying && chainPlaybackScene == sceneSlot;
            const bool queued = pendingSceneRecall.active
                && pendingSceneRecall.sceneSlot == sceneSlot;
            const bool burstActive = nowMs < scenePadActionBurstUntilMs[slotIdx];
            const bool copyArmed = sceneCopySourceSlot == sceneSlot
                && sceneCopyMainPresetIndex == activeSceneMainPreset;

            int level = exists ? 5 : 2;
            if (active)
                level = exists ? 11 : 3;
            if (inSequence)
                level = chainPlaying ? (fastBlinkOn ? 15 : 6) : juce::jmax(level, 5);
            if (sequenceCurrent)
                level = fastBlinkOn ? 15 : 9;
            if (queued)
                level = slowBlinkOn ? 15 : 7;
            if (held && !copyArmed)
                level = 15;
            if (copyArmed)
                level = fastBlinkOn ? 15 : 4;
            if (burstActive)
            {
                const bool burstOn = ((nowMs / sceneActionBurstIntervalMs) & 1u) == 0u;
                level = burstOn ? 15 : 0;
            }

            newLedState[sceneSlot][GROUP_ROW] = level;
        }

        if (kMonomeSceneRecorderColumn >= 0 && kMonomeSceneRecorderColumn < gridWidth)
        {
            if (monomeSceneRecorderHeld
                && !monomeSceneRecorderHoldClearTriggered
                && !isScenePerformanceRecording()
                && monomeSceneRecorderPressStartMs != 0)
            {
                const uint32_t elapsed = nowMs - monomeSceneRecorderPressStartMs;
                if (elapsed >= sceneHoldDeleteMs)
                {
                    monomeSceneRecorderHoldClearTriggered = true;
                    clearPendingMonomeSceneRecorderTap();
                    clearPendingSceneRecorderAction();
                    const int targetSceneSlot = getFocusedSceneSlot();
                    const int targetStrip = clampVisibleStrip(getLastMonomePressedStripRow());
                    if (clearSceneStripAutomationAndMotion(targetSceneSlot, targetStrip))
                        monomeSceneRecorderClearBurstUntilMs = nowMs + sceneActionBurstDurationMs;
                }
            }

            const bool recordingThisScene = isScenePerformanceRecording()
                && getScenePerformanceRecordingSceneSlot() == juce::jlimit(0, SceneSlots - 1, activeSceneSlot);
            const bool overdubbingThisScene = recordingThisScene && isScenePerformanceOverdubbing();
            const bool pendingTap = monomeSceneRecorderPendingAction != MonomeSceneRecorderTapAction::None;
            const bool pendingQuantized = pendingSceneRecorderAction.active
                || pendingSceneRecorderApplyAction.load(std::memory_order_acquire)
                    != static_cast<int>(SceneRecorderAction::None);
            const bool recorderHeld = monomeSceneRecorderHeld;
            const bool clearBurstActive = nowMs < monomeSceneRecorderClearBurstUntilMs;
            const bool idleBlinkOn = ((nowMs / kSceneRecorderIdleBlinkIntervalMs) & 1u) == 0u;
            const bool activeBlinkOn = ((nowMs / kSceneRecorderActiveBlinkIntervalMs) & 1u) == 0u;

            int recorderLevel = idleBlinkOn ? 10 : 0;
            if (clearBurstActive)
            {
                const bool burstOn = ((nowMs / sceneActionBurstIntervalMs) & 1u) == 0u;
                recorderLevel = burstOn ? 15 : 0;
            }
            else if (recordingThisScene || pendingTap || pendingQuantized)
                recorderLevel = activeBlinkOn ? 15 : (overdubbingThisScene ? 5 : 0);
            else if (recorderHeld)
                recorderLevel = 15;

            newLedState[kMonomeSceneRecorderColumn][GROUP_ROW] = recorderLevel;
        }

        if (sceneStepMotionPageNavigationActive())
        {
            const int targetStrip = clampVisibleStrip(getLastMonomePressedStripRow());
            const bool canNavigate = sceneStepMotionLaneNavigationAvailable(targetStrip);
            if (kMonomeModPrevColumn >= 0 && kMonomeModPrevColumn < gridWidth)
                newLedState[kMonomeModPrevColumn][GROUP_ROW] = sceneStepMotionLaneNavigationLevel(canNavigate);
            if (kMonomeModNextColumn >= 0 && kMonomeModNextColumn < gridWidth)
                newLedState[kMonomeModNextColumn][GROUP_ROW] = sceneStepMotionLaneNavigationLevel(canNavigate);
        }
    };

    if (controlModeActive && currentControlMode == ControlMode::FileBrowser)
    {
        for (int stripIndex = 0; stripIndex < MaxStrips; ++stripIndex)
        {
            const auto stripIdx = static_cast<size_t>(stripIndex);
            for (int slot = 0; slot < BrowserFavoriteSlots; ++slot)
            {
                const auto slotIdx = static_cast<size_t>(slot);
                if (!browserFavoritePadHeld[stripIdx][slotIdx] || browserFavoritePadHoldSaveTriggered[stripIdx][slotIdx])
                    continue;

                const uint32_t elapsed = nowMs - browserFavoritePadPressStartMs[stripIdx][slotIdx];
                if (elapsed < browserFavoriteHoldSaveMs)
                    continue;

                const bool saved = saveBrowserFavoriteDirectoryFromStrip(stripIndex, slot);
                browserFavoritePadHoldSaveTriggered[stripIdx][slotIdx] = true;
                if (saved)
                {
                    browserFavoriteSaveBurstUntilMs[slotIdx] = nowMs + browserFavoriteSaveBurstDurationMs;
                    browserFavoriteMissingBurstUntilMs[slotIdx] = 0;
                }
                else
                {
                    browserFavoriteMissingBurstUntilMs[slotIdx] = nowMs + browserFavoriteMissingBurstDurationMs;
                }
            }
        }
    }

    if (controlModeActive && currentControlMode == ControlMode::Preset)
    {
        for (int y = 0; y < PresetRows; ++y)
        {
            for (int x = 0; x < PresetColumns; ++x)
            {
                const int presetIndex = y * PresetColumns + x;
                const auto idx = static_cast<size_t>(presetIndex);

                if (presetPadHeld[idx] && !presetPadHoldSaveTriggered[idx])
                {
                    const uint32_t elapsed = nowMs - presetPadPressStartMs[idx];
                    if (elapsed >= presetHoldSaveMs)
                    {
                        savePreset(presetIndex);
                        presetPadHoldSaveTriggered[idx] = true;
                        presetPadSaveBurstUntilMs[idx] = nowMs + presetSaveBurstDurationMs;
                    }
                }

                const bool exists = presetExists(presetIndex);
                int level = exists ? 8 : 2;  // Existing lit, empty dim.
                const bool burstActive = nowMs < presetPadSaveBurstUntilMs[idx];
                if (burstActive)
                {
                    const bool burstOn = ((nowMs / presetSaveBurstIntervalMs) & 1u) == 0u;
                    level = burstOn ? 15 : 0;
                }
                else if (presetIndex == loadedPresetIndex && exists)
                {
                    level = slowBlinkOn ? 15 : 0;  // Loaded preset blinks.
                }
                newLedState[x][y] = level;
            }
        }

        if (sceneModeActive)
            updateSceneSlotLeds();

        // Keep control row visible while preset grid is active.
        for (int x = 0; x < NumControlRowPages && x < gridWidth; ++x)
            newLedState[x][CONTROL_ROW] = 5;
        const int activeButton = getControlButtonForMode(currentControlMode);
        if (activeButton >= 0 && activeButton < NumControlRowPages && activeButton < gridWidth)
            newLedState[activeButton][CONTROL_ROW] = 15;
        // Metronome pulse on control-row quantize button (row 7, col 15):
        // beat pulses dim, bar "1" pulses bright.
        if (metroPulseOn)
            newLedState[15][CONTROL_ROW] = metroDownbeat ? metroDownbeatLevel : metroBeatLevel;
        else
            newLedState[15][CONTROL_ROW] = metroOffLevel;

        for (int y = 0; y < gridHeight; ++y)
        {
            for (int x = 0; x < gridWidth; ++x)
            {
                if (newLedState[x][y] != ledCache[x][y])
                {
                    monomeConnection.setLEDLevel(x, y, newLedState[x][y]);
                    ledCache[x][y] = newLedState[x][y];
                }
            }
        }

        return;
    }

    // ROW 0 ownership is explicit: launch surface by default, edit overlay when toggled.
    if (topRowMode == MonomeLayoutState::TopRowMode::StepEdit)
    {
        for (int i = 0; i < 16; ++i)
            newLedState[i][GROUP_ROW] = 0;

        auto getStepToolColumn = [this]()
        {
            switch (stepEditTool)
            {
                case StepEditTool::Velocity: return 0;
                case StepEditTool::Divide: return 1;
                case StepEditTool::RampUp: return 2;
                case StepEditTool::RampDown: return 3;
                case StepEditTool::Probability: return 4;
                case StepEditTool::Attack: return 5;
                case StepEditTool::Decay: return 6;
                case StepEditTool::Release: return 7;
                case StepEditTool::Gate:
                default: return -1;
            }
        };

        const int toolColumn = getStepToolColumn();
        for (int col = 0; col <= 7; ++col)
            newLedState[col][GROUP_ROW] = (col == toolColumn) ? 15 : 4;

        stepEditSelectedStrip = clampVisibleStrip(stepEditSelectedStrip);
        const int selectedStripIndex = stepEditSelectedStrip;
        if (selectedStripIndex < (stepEditStripBank * stepEditBankSize)
            || selectedStripIndex >= ((stepEditStripBank + 1) * stepEditBankSize))
        {
            stepEditStripBank = juce::jlimit(0, maxStepEditBank, selectedStripIndex / stepEditBankSize);
        }
        const int bankStart = stepEditStripBank * stepEditBankSize;

        for (int col = 8; col <= 13; ++col)
        {
            const int stripIndex = bankStart + (col - 8);
            if (stripIndex >= visibleStripCount)
                continue;

            auto* strip = audioEngine->getStrip(stripIndex);
            const bool inStepMode = (strip && strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Step);
            int level = inStepMode ? 6 : 3;
            if (stripIndex == selectedStripIndex)
                level = inStepMode ? 15 : 10;
            newLedState[col][GROUP_ROW] = level;
        }

        const bool bankDownAvailable = (stepEditStripBank > 0);
        const bool bankUpAvailable = (stepEditStripBank < maxStepEditBank);
        newLedState[14][GROUP_ROW] = bankDownAvailable ? 9 : 2;
        newLedState[15][GROUP_ROW] = bankUpAvailable ? 9 : 2;
    }
    else if (topRowMode == MonomeLayoutState::TopRowMode::Gate)
    {
        for (int i = 0; i < 16; ++i)
            newLedState[i][GROUP_ROW] = 0;

        const int selectedColumn = static_cast<int>(getGatePageMode());
        for (int col = 0; col <= 4; ++col)
            newLedState[col][GROUP_ROW] = (col == selectedColumn) ? 15 : 4;
    }
    else if (topRowMode == MonomeLayoutState::TopRowMode::Filter)
    {
        // Filter sub-page indicators
        newLedState[0][GROUP_ROW] = (filterSubPage == FilterSubPage::Frequency) ? 15 : 5;  // Frequency
        newLedState[1][GROUP_ROW] = (filterSubPage == FilterSubPage::Resonance) ? 15 : 5;  // Resonance
        newLedState[2][GROUP_ROW] = 0;  // Unused (skip button 2)
        newLedState[3][GROUP_ROW] = (filterSubPage == FilterSubPage::Type) ? 15 : 5;       // Type
    }
    else if (topRowMode == MonomeLayoutState::TopRowMode::Modulation)
    {
        const int targetStrip = clampVisibleStrip(getLastMonomePressedStripRow());
        if (sceneMainAutomationMonomeActive())
        {
            const bool canNavigate = regularModLaneNavigationAvailable(targetStrip);
            for (int x = 0; x < 16; ++x)
            {
                if (x == kMonomeModPrevColumn || x == kMonomeModNextColumn)
                {
                    newLedState[x][GROUP_ROW] = sceneStepMotionLaneNavigationLevel(canNavigate);
                    continue;
                }

                newLedState[x][GROUP_ROW] = 0;
            }
        }
        else
        {
            syncMonomeModEditPageToPlayback(targetStrip);
            const auto seq = audioEngine->getModSequencerState(targetStrip);
            const int activeGlobalStep = audioEngine->getModCurrentGlobalStep(targetStrip);
            const int playbackPage = juce::jlimit(
                0,
                ModernAudioEngine::MaxModBars - 1,
                activeGlobalStep / ModernAudioEngine::ModSteps);
            const int activeStep = (playbackPage == seq.editPage)
                ? (activeGlobalStep % ModernAudioEngine::ModSteps)
                : -1;
            const bool stripPlaying = audioEngine->getStrip(targetStrip) && audioEngine->getStrip(targetStrip)->isPlaying();
            const int displayRow = 0; // Top row is highest value in modulation mode.
            const int modulationBaseRow = seq.bipolar ? (modulationMaxRow / 2) : modulationMaxRow;
            const bool sceneStepMotionPageNavigationVisible = sceneStepMotionPageNavigationActive()
                && isControlModeActive()
                && getCurrentControlMode() == ControlMode::Modulation;
            const bool sceneStepMotionLaneNavigationEnabled = sceneStepMotionPageNavigationVisible
                && sceneStepMotionLaneNavigationAvailable(targetStrip);

            auto valueToRow = [&](float v)
            {
                v = juce::jlimit(0.0f, 1.0f, v);
                if (seq.bipolar)
                {
                    const float signedV = (v * 2.0f) - 1.0f;
                    const float n = (signedV + 1.0f) * 0.5f;
                    return juce::jlimit(0, modulationMaxRow, static_cast<int>(std::round((1.0f - n) * modulationMaxRow)));
                }
                return juce::jlimit(0, modulationMaxRow, static_cast<int>(std::round((1.0f - v) * modulationMaxRow)));
            };
            auto curveLevelForRow = [&](int row, bool isPoint)
            {
                // Value-encoded intensity: high values (top rows) are more solid,
                // low values (bottom rows) are dimmer.
                const float rowValue01 = juce::jlimit(0.0f, 1.0f, 1.0f - (static_cast<float>(row) / static_cast<float>(modulationMaxRow)));
                const int base = juce::jlimit(1, 15, static_cast<int>(std::round(juce::jmap(rowValue01, 2.0f, 12.0f))));
                return juce::jlimit(1, 15, isPoint ? (base + 2) : base);
            };
            auto stepLevelForRow = [&](int row, int pointRow, int baseRow)
            {
                const int minRow = juce::jmin(baseRow, pointRow);
                const int maxRow = juce::jmax(baseRow, pointRow);
                if (row < minRow || row > maxRow)
                    return 0;

                const float barRange = static_cast<float>(std::abs(pointRow - baseRow));
                const float fromBase = static_cast<float>(std::abs(row - baseRow));
                const float t = (barRange > 0.0f) ? (fromBase / barRange) : 1.0f; // 0 at base, 1 at point
                const float shapedT = std::pow(juce::jlimit(0.0f, 1.0f, t), 0.72f);

                const float rowValue01 = juce::jlimit(0.0f, 1.0f, 1.0f - (static_cast<float>(row) / static_cast<float>(modulationMaxRow)));
                const float minLevel = seq.bipolar ? 3.0f : 2.0f;
                const float maxLevel = 9.0f + (4.0f * rowValue01); // 9..13, brighter for higher values
                int level = static_cast<int>(std::round(minLevel + ((maxLevel - minLevel) * shapedT)));
                if (row == pointRow)
                    level += 2;
                return juce::jlimit(1, 15, level);
            };

            for (int x = 0; x < 16; ++x)
            {
                if (x == kMonomeModPrevColumn || x == kMonomeModNextColumn)
                {
                    if (sceneStepMotionPageNavigationVisible)
                    {
                        newLedState[x][GROUP_ROW] = sceneStepMotionLaneNavigationLevel(sceneStepMotionLaneNavigationEnabled);
                    }
                    else
                    {
                        newLedState[x][GROUP_ROW] = slowBlinkOn ? 15 : 5;
                    }
                    continue;
                }

                newLedState[x][GROUP_ROW] = 0;
                const float v = seq.steps[static_cast<size_t>(x)];
                const int pointRow = valueToRow(v);

                if (seq.curveMode)
                {
                    int level = 0;
                    if (displayRow == pointRow)
                        level = juce::jmax(level, curveLevelForRow(displayRow, true));
                    if (x < 15)
                    {
                        const int nextRow = valueToRow(seq.steps[static_cast<size_t>(x + 1)]);
                        const int minRow = juce::jmin(pointRow, nextRow);
                        const int maxRow = juce::jmax(pointRow, nextRow);
                        if (displayRow >= minRow && displayRow <= maxRow)
                            level = juce::jmax(level, curveLevelForRow(displayRow, false));
                    }
                    newLedState[x][GROUP_ROW] = level;
                }
                else
                {
                    newLedState[x][GROUP_ROW] = stepLevelForRow(displayRow, pointRow, modulationBaseRow);
                }

                if (stripPlaying && x == activeStep)
                    newLedState[x][GROUP_ROW] = juce::jmax(newLedState[x][GROUP_ROW], 15);
            }
        }

    }
    else
    {
        if (sceneModeActive)
            updateSceneSlotLeds();
        else
        {
            // Normal mode: Groups 0-3 + Patterns 4-7
            for (int groupId = 0; groupId < 4; ++groupId)
            {
                auto* group = audioEngine->getGroup(groupId);
                if (!group)
                    continue;

                bool anyPlaying = false;
                bool isMuted = group->isMuted();
                bool hasStrips = !group->getStrips().empty();

                if (!isMuted && hasStrips)
                {
                    auto strips = group->getStrips();
                    for (int stripIdx : strips)
                    {
                        if (auto* strip = audioEngine->getStrip(stripIdx))
                        {
                            if (strip->isPlaying())
                            {
                                anyPlaying = true;
                                break;
                            }
                        }
                    }
                }

                if (anyPlaying)
                    newLedState[groupId][GROUP_ROW] = 15;
                else if (isMuted)
                    newLedState[groupId][GROUP_ROW] = 3;
                else if (hasStrips)
                    newLedState[groupId][GROUP_ROW] = 8;
                else
                    newLedState[groupId][GROUP_ROW] = 0;
            }
        }

        // Row 0, columns 4-7: Pattern recorder status on the launch surface.
        if (topRowMode == MonomeLayoutState::TopRowMode::Launch)
        {
            for (int i = 0; i < 4; ++i)
            {
                int col = i + 4;  // Columns 4-7
                auto* pattern = audioEngine->getPattern(i);
                if (pattern)
                {
                    if (pattern->isRecording())
                        newLedState[col][GROUP_ROW] = fastBlinkOn ? 15 : 0;  // Recording: Fast blink
                    else if (pattern->isPlaying())
                        newLedState[col][GROUP_ROW] = slowBlinkOn ? 12 : 0;  // Playing: Slow blink
                    else
                        newLedState[col][GROUP_ROW] = 3;   // Stopped/idle: Dim
                }
            }
        }
    }  // End else (normal mode)

    if (sceneModRepeatButtonsVisible())
    {
        const int selectedSceneSlot = getFocusedSceneSlot();
        const int currentLengthCount = getSceneLengthCount(selectedSceneSlot);
        for (int x = kSceneLengthFirstColumn; x <= kSceneLengthLastColumn && x < gridWidth; ++x)
        {
            const int lengthCount = sceneLengthCountFromButton(x);
            if (lengthCount <= 0)
                continue;

            newLedState[x][GROUP_ROW] = (currentLengthCount == lengthCount) ? 15 : 4;
        }
    }

    if (regularModPageLaneNavigationVisible())
    {
        const int targetStrip = clampVisibleStrip(getLastMonomePressedStripRow());
        const bool canNavigate = regularModLaneNavigationAvailable(targetStrip);
        if (kMonomeModPrevColumn >= 0 && kMonomeModPrevColumn < gridWidth)
            newLedState[kMonomeModPrevColumn][GROUP_ROW] = sceneStepMotionLaneNavigationLevel(canNavigate);
        if (kMonomeModNextColumn >= 0 && kMonomeModNextColumn < gridWidth)
            newLedState[kMonomeModNextColumn][GROUP_ROW] = sceneStepMotionLaneNavigationLevel(canNavigate);
    }

    // Row 0 col 8: momentary scratch indicator on the main launch surface.
    if (topRowUsesScratchAndStutter())
        newLedState[8][GROUP_ROW] = momentaryScratchHoldActive ? 15 : 4;

    // Row 0, cols 9-15: momentary stutter division selectors.
    // Visible on the main launch surface only.
    if (topRowUsesScratchAndStutter())
    {
        const uint8_t heldMask = momentaryStutterButtonMask.load(std::memory_order_acquire);
        for (int x = 9; x <= 15; ++x)
        {
            const uint8_t bit = stutterButtonBitForColumn(x);
            const bool held = (heldMask & bit) != 0;
            const bool active = momentaryStutterHoldActive && (momentaryStutterActiveDivisionButton == x);
            if (active)
                newLedState[x][GROUP_ROW] = fastBlinkOn ? 15 : 8;
            else if (held)
                newLedState[x][GROUP_ROW] = 9;
            else
                newLedState[x][GROUP_ROW] = 2;
        }
    }

    const bool stripRowSelectionHighlightActive = visibleStripCount > 0
        && (!controlModeActive
            || (currentControlMode != ControlMode::StepEdit
                && currentControlMode != ControlMode::Modulation
                && currentControlMode != ControlMode::GrainSize
                && currentControlMode != ControlMode::Preset));
    const int highlightedStripIndex = stripRowSelectionHighlightActive
        ? clampVisibleStrip(getLastMonomePressedStripRow())
        : -1;
    const auto applySelectedStripHighlight = [&](int stripIndex, int y)
    {
        if (!stripRowSelectionHighlightActive
            || stripIndex != highlightedStripIndex
            || y < FIRST_STRIP_ROW
            || y >= CONTROL_ROW)
        {
            return;
        }

        const int outerLevel = controlModeActive ? 6 : 5;
        const int innerLevel = controlModeActive ? 3 : 2;
        if (gridWidth > 0)
        {
            newLedState[0][y] = juce::jmax(newLedState[0][y], outerLevel);
            newLedState[gridWidth - 1][y] = juce::jmax(newLedState[gridWidth - 1][y], outerLevel);
        }

        if (gridWidth > 3)
        {
            newLedState[1][y] = juce::jmax(newLedState[1][y], innerLevel);
            newLedState[gridWidth - 2][y] = juce::jmax(newLedState[gridWidth - 2][y], innerLevel);
        }
    };
    
    // Strip rows (between group row and control row).
    for (int stripIndex = 0; stripIndex < visibleStripCount; ++stripIndex)
    {
        int y = FIRST_STRIP_ROW + stripIndex;
        auto* strip = audioEngine->getStrip(stripIndex);

        if (controlModeActive && currentControlMode == ControlMode::StepEdit)
        {
            const int selectedStripIndex = clampVisibleStrip(stepEditSelectedStrip);
            auto* selectedStrip = audioEngine->getStrip(selectedStripIndex);
            if (!selectedStrip)
            {
                for (int x = 0; x < 16; ++x)
                    newLedState[x][y] = 0;
                continue;
            }

            const int totalSteps = selectedStrip->getStepTotalSteps();
            const int visibleOffset = selectedStrip->getVisibleStepOffset();
            const int visibleCurrentStep = selectedStrip->getVisibleCurrentStep();
            const bool stripPlaying = selectedStrip->isPlaying()
                && selectedStrip->getPlayMode() == EnhancedAudioStrip::PlayMode::Step;
            const float rowNorm = stepEditRowNorm(y);

            if (stepEditTool == StepEditTool::Attack
                || stepEditTool == StepEditTool::Decay
                || stepEditTool == StepEditTool::Release)
            {
                float normalized = 0.0f;
                if (stepEditTool == StepEditTool::Attack)
                    normalized = juce::jlimit(0.0f, 1.0f, selectedStrip->getStepEnvelopeAttackMs() / 400.0f);
                else if (stepEditTool == StepEditTool::Decay)
                    normalized = juce::jlimit(0.0f, 1.0f, (selectedStrip->getStepEnvelopeDecayMs() - 1.0f) / 3999.0f);
                else
                {
                    const float pitchSemitones = getPitchSemitonesForDisplay(*selectedStrip);
                    normalized = juce::jlimit(0.0f, 1.0f, (pitchSemitones + 24.0f) / 48.0f);
                }

                const int activeCol = juce::jlimit(0, 15, static_cast<int>(std::round(normalized * 15.0f)));
                for (int x = 0; x < 16; ++x)
                {
                    int level = (x == activeCol) ? 15 : ((x < activeCol) ? 6 : 1);
                    if (stripPlaying && x == visibleCurrentStep)
                        level = juce::jmax(level, 9);
                    newLedState[x][y] = level;
                }
                continue;
            }

            for (int x = 0; x < 16; ++x)
            {
                const int absoluteStep = visibleOffset + x;
                if (absoluteStep < 0 || absoluteStep >= totalSteps)
                {
                    newLedState[x][y] = 0;
                    continue;
                }

                const auto idx = static_cast<size_t>(absoluteStep);
                const bool enabled = selectedStrip->stepPattern[idx];
                const int subdivision = selectedStrip->getStepSubdivisionAtIndex(absoluteStep);
                const float startVelocity = selectedStrip->getStepSubdivisionStartVelocityAtIndex(absoluteStep);
                const float endVelocity = selectedStrip->getStepSubdivisionRepeatVelocityAtIndex(absoluteStep);
                const float maxVelocity = juce::jmax(startVelocity, endVelocity);
                const float probability = selectedStrip->getStepProbabilityAtIndex(absoluteStep);

                int level = 0;
                if (stepEditTool == StepEditTool::Gate)
                {
                    level = (enabled && y == (CONTROL_ROW - 1)) ? 12 : 0;
                }
                else
                {
                    float value = 0.0f;
                    switch (stepEditTool)
                    {
                        case StepEditTool::Gate:
                        case StepEditTool::Attack:
                        case StepEditTool::Decay:
                        case StepEditTool::Release:
                            break;
                        case StepEditTool::Velocity:
                            value = enabled ? maxVelocity : 0.0f;
                            break;
                        case StepEditTool::Divide:
                            value = enabled
                                ? static_cast<float>(subdivision - 1)
                                    / static_cast<float>(juce::jmax(1, EnhancedAudioStrip::MaxStepSubdivisions - 1))
                                : 0.0f;
                            break;
                        case StepEditTool::RampUp:
                        {
                            const float base = juce::jmax(0.001f, maxVelocity);
                            value = enabled ? juce::jlimit(0.0f, 1.0f, 1.0f - (startVelocity / base)) : 0.0f;
                            break;
                        }
                        case StepEditTool::RampDown:
                        {
                            const float base = juce::jmax(0.001f, maxVelocity);
                            value = enabled ? juce::jlimit(0.0f, 1.0f, 1.0f - (endVelocity / base)) : 0.0f;
                            break;
                        }
                        case StepEditTool::Probability:
                            value = enabled ? probability : 0.0f;
                            break;
                    }

                    if (value + 0.0001f >= rowNorm)
                        level = enabled ? 11 : 7;
                    else
                        level = enabled ? 2 : 0;
                }

                if (stripPlaying && x == visibleCurrentStep)
                    level = juce::jmax(level, (y == (CONTROL_ROW - 1)) ? 15 : 6);
                newLedState[x][y] = level;
            }

            continue;
        }
        
        if (!strip)
        {
            applySelectedStripHighlight(stripIndex, y);
            continue;
        }
        
        // Skip empty strips ONLY in Normal mode (not in control modes)
        // In control modes, we always want to show the control LEDs even on empty strips
        bool hasContent = strip->hasAudio();
        if (strip->playMode == EnhancedAudioStrip::PlayMode::Step)
        {
            // In step mode, check if stepSampler has audio
            hasContent = strip->stepSampler.getHasAudio();
        }
        
        // Only skip empty strips when in Normal mode or FileBrowser mode
        if (!hasContent && currentControlMode == ControlMode::Normal)
        {
            applySelectedStripHighlight(stripIndex, y);
            continue;
        }
        
        // Check if group is muted
        bool isGroupMuted = false;
        int groupId = strip->getGroup();
        if (groupId >= 0 && groupId < 4)
        {
            auto* group = audioEngine->getGroup(groupId);
            if (group && group->isMuted())
                isGroupMuted = true;
        }
        
        // Different displays per mode - ONLY when control button is HELD
        const bool simpleMixRenderMode = controlModeActive && (
            currentControlMode == ControlMode::Speed
            || currentControlMode == ControlMode::Pitch
            || currentControlMode == ControlMode::Pan
            || currentControlMode == ControlMode::Volume
            || currentControlMode == ControlMode::Swing
            || currentControlMode == ControlMode::Gate);

        if (simpleMixRenderMode)
        {
            MonomeMixActions::renderRow(*strip, *this, y, newLedState, static_cast<int>(currentControlMode));
        }
        else if (controlModeActive && currentControlMode == ControlMode::GrainSize)
        {
            const int targetStripIndex = clampVisibleStrip(getLastMonomePressedStripRow());
            if (auto* targetStrip = audioEngine->getStrip(targetStripIndex))
                MonomeMixActions::renderGrainPageRow(*targetStrip, stripIndex, y, newLedState);
        }
        else if (controlModeActive && currentControlMode == ControlMode::Filter)
        {
            MonomeFilterActions::renderRow(*strip, y, newLedState, static_cast<int>(filterSubPage));
        }
        else if (controlModeActive && currentControlMode == ControlMode::Delay)
        {
            const int targetStripIndex = clampVisibleStrip(getLastMonomePressedStripRow());
            if (auto* targetStrip = audioEngine->getStrip(targetStripIndex))
                MonomeMixActions::renderDelayPageRow(*targetStrip, stripIndex, y, newLedState);
        }
        else if (controlModeActive && currentControlMode == ControlMode::FileBrowser)
        {
            MonomeFileBrowserActions::renderRow(*this, *audioEngine, *strip, stripIndex, y, newLedState);
        }
        else if (controlModeActive && currentControlMode == ControlMode::GroupAssign)
        {
            MonomeGroupAssignActions::renderRow(*strip, y, newLedState);
        }
        else if (controlModeActive && currentControlMode == ControlMode::Modulation)
        {
            const int selectedStrip = clampVisibleStrip(getLastMonomePressedStripRow());
            if (sceneMainAutomationMonomeActive())
            {
                const int displayRow = y; // Row 0 stays reserved for navigation/indication.
                const int modulationBaseRow = sceneMainModTargetBipolar ? sceneAutomationValueToRow(0.5f)
                                                                        : sceneAutomationBottomRow;
                auto stepLevelForRow = [&](int row, int pointRow, int baseRow)
                {
                    const int minRow = juce::jmin(baseRow, pointRow);
                    const int maxRow = juce::jmax(baseRow, pointRow);
                    if (row < minRow || row > maxRow)
                        return 0;

                    const float barRange = static_cast<float>(std::abs(pointRow - baseRow));
                    const float fromBase = static_cast<float>(std::abs(row - baseRow));
                    const float t = (barRange > 0.0f) ? (fromBase / barRange) : 1.0f;
                    const float shapedT = std::pow(juce::jlimit(0.0f, 1.0f, t), 0.72f);
                    const float rowValue01 = sceneAutomationRowValue01(row);
                    const float minLevel = sceneMainModTargetBipolar ? 3.0f : 2.0f;
                    const float maxLevel = 9.0f + (4.0f * rowValue01);
                    int level = static_cast<int>(std::round(minLevel + ((maxLevel - minLevel) * shapedT)));
                    if (row == pointRow)
                        level += 2;
                    return juce::jlimit(1, 15, level);
                };

                for (int x = 0; x < 16; ++x)
                {
                    newLedState[x][y] = 0;
                    float normalizedValue = 0.0f;
                    if (!sceneMainModColumnValue(x, normalizedValue))
                        continue;

                    const int pointRow = sceneAutomationValueToRow(normalizedValue);
                    newLedState[x][y] = stepLevelForRow(displayRow, pointRow, modulationBaseRow);

                    if (sceneMainModActiveStep == x)
                        newLedState[x][y] = juce::jmax(newLedState[x][y], 15);
                }
            }
            else
            {
                const auto seq = audioEngine->getModSequencerState(selectedStrip);
                const int activeGlobalStep = audioEngine->getModCurrentGlobalStep(selectedStrip);
                const int playbackPage = juce::jlimit(
                    0,
                    ModernAudioEngine::MaxModBars - 1,
                    activeGlobalStep / ModernAudioEngine::ModSteps);
                const int activeStep = (playbackPage == seq.editPage)
                    ? (activeGlobalStep % ModernAudioEngine::ModSteps)
                    : -1;
                const bool stripPlaying = audioEngine->getStrip(selectedStrip) && audioEngine->getStrip(selectedStrip)->isPlaying();
                const int displayRow = y; // Strip rows, with row 0 rendered in GROUP_ROW branch.
                auto baseRowForColumn = [&](int column)
                {
                    if (column == kMonomeModPrevColumn || column == kMonomeModNextColumn)
                        return seq.bipolar ? (1 + ((modulationMaxRow - 1) / 2)) : modulationMaxRow;
                    return seq.bipolar ? (modulationMaxRow / 2) : modulationMaxRow;
                };

                auto valueToRow = [&](float v, int column)
                {
                    v = juce::jlimit(0.0f, 1.0f, v);
                    const bool reservedTopCell = (column == kMonomeModPrevColumn || column == kMonomeModNextColumn);
                    const int topRow = reservedTopCell ? 1 : 0;
                    const int usableRows = reservedTopCell ? juce::jmax(1, modulationMaxRow - 1) : modulationMaxRow;
                    if (seq.bipolar)
                    {
                        const float signedV = (v * 2.0f) - 1.0f;
                        const float n = (signedV + 1.0f) * 0.5f;
                        return juce::jlimit(topRow,
                                            modulationMaxRow,
                                            topRow + static_cast<int>(std::round((1.0f - n) * usableRows)));
                    }
                    return juce::jlimit(topRow,
                                        modulationMaxRow,
                                        topRow + static_cast<int>(std::round((1.0f - v) * usableRows)));
                };
                auto curveLevelForRow = [&](int row, bool isPoint)
                {
                    // Value-encoded intensity: high values (top rows) are more solid,
                    // low values (bottom rows) are dimmer.
                    const float rowValue01 = juce::jlimit(0.0f, 1.0f, 1.0f - (static_cast<float>(row) / static_cast<float>(modulationMaxRow)));
                    const int base = juce::jlimit(1, 15, static_cast<int>(std::round(juce::jmap(rowValue01, 2.0f, 12.0f))));
                    return juce::jlimit(1, 15, isPoint ? (base + 2) : base);
                };
                auto stepLevelForRow = [&](int row, int pointRow, int baseRow)
                {
                    const int minRow = juce::jmin(baseRow, pointRow);
                    const int maxRow = juce::jmax(baseRow, pointRow);
                    if (row < minRow || row > maxRow)
                        return 0;

                    const float barRange = static_cast<float>(std::abs(pointRow - baseRow));
                    const float fromBase = static_cast<float>(std::abs(row - baseRow));
                    const float t = (barRange > 0.0f) ? (fromBase / barRange) : 1.0f; // 0 at base, 1 at point
                    const float shapedT = std::pow(juce::jlimit(0.0f, 1.0f, t), 0.72f);

                    const float rowValue01 = juce::jlimit(0.0f, 1.0f, 1.0f - (static_cast<float>(row) / static_cast<float>(modulationMaxRow)));
                    const float minLevel = seq.bipolar ? 3.0f : 2.0f;
                    const float maxLevel = 9.0f + (4.0f * rowValue01); // 9..13, brighter for higher values
                    int level = static_cast<int>(std::round(minLevel + ((maxLevel - minLevel) * shapedT)));
                    if (row == pointRow)
                        level += 2;
                    return juce::jlimit(1, 15, level);
                };

                for (int x = 0; x < 16; ++x)
                {
                    newLedState[x][y] = 0;
                    const float v = seq.steps[static_cast<size_t>(x)];
                    const int pointRow = valueToRow(v, x);
                    const int modulationBaseRow = baseRowForColumn(x);

                    if (seq.curveMode)
                    {
                        // Draw point + interpolated line to next point for readable curve graph.
                        int level = 0;
                        if (displayRow == pointRow)
                            level = juce::jmax(level, curveLevelForRow(displayRow, true));
                        if (x < 15)
                        {
                            const int nextRow = valueToRow(seq.steps[static_cast<size_t>(x + 1)], x + 1);
                            const int minRow = juce::jmin(pointRow, nextRow);
                            const int maxRow = juce::jmax(pointRow, nextRow);
                            if (displayRow >= minRow && displayRow <= maxRow)
                                level = juce::jmax(level, curveLevelForRow(displayRow, false));
                        }
                        newLedState[x][y] = level;
                    }
                    else
                    {
                        // Step-slider mode: vertical bar to value.
                        newLedState[x][y] = stepLevelForRow(displayRow, pointRow, modulationBaseRow);
                    }

                    if (stripPlaying && x == activeStep)
                        newLedState[x][y] = juce::jmax(newLedState[x][y], 15);
                }
            }
        }
        else // Normal - playhead or step sequencer
        {
            // Check if this strip is in step mode
            if (strip->playMode == EnhancedAudioStrip::PlayMode::Step)
            {
                // STEP SEQUENCER MODE - show step pattern
                const auto visiblePattern = strip->getVisibleStepPattern();
                const int visibleCurrentStep = strip->getVisibleCurrentStep();
                for (int x = 0; x < 16; ++x)
                {
                    bool isCurrentStep = (x == visibleCurrentStep);
                    bool isActiveStep = visiblePattern[static_cast<size_t>(x)];
                    
                    if (isCurrentStep && isActiveStep)
                    {
                        // Current step AND active - brightest
                        newLedState[x][y] = 15;
                    }
                    else if (isCurrentStep)
                    {
                        // Current step but inactive - medium
                        newLedState[x][y] = 6;
                    }
                    else if (isActiveStep)
                    {
                        // Active step (not current) - medium bright
                        newLedState[x][y] = 10;
                    }
                    else
                    {
                        // Inactive step - dim
                        newLedState[x][y] = 2;
                    }
                }
            }
            else if (strip->playMode == EnhancedAudioStrip::PlayMode::Sample)
            {
                for (int x = 0; x < 16; ++x)
                    newLedState[x][y] = 0;

                if (auto* sampleEngine = getSampleModeEngine(stripIndex, false))
                {
                    const auto snapshot = sampleEngine->getStateSnapshot();
                    const bool legacyLoopFeedback = snapshot.useLegacyLoopEngine
                        || strip->isSampleModeLegacyLoopEngineEnabled();
                    const bool loopTriggerMode = snapshot.triggerMode == SampleTriggerMode::Loop;
                    const int heldSlot = getSampleModeHeldVisibleSliceSlot(stripIndex);
                    int playbackSlot = -1;
                    if (!legacyLoopFeedback && snapshot.playbackProgress >= 0.0f)
                    {
                        for (int x = 0; x < 16; ++x)
                        {
                            const auto& slice = snapshot.visibleSlices[static_cast<size_t>(x)];
                            if (slice.id < 0)
                                continue;

                            const bool isLastSlice = (x == 15);
                            if (snapshot.playbackProgress >= slice.normalizedStart
                                && (snapshot.playbackProgress < slice.normalizedEnd
                                    || (isLastSlice && snapshot.playbackProgress <= slice.normalizedEnd)))
                            {
                                playbackSlot = x;
                                break;
                            }
                        }
                    }
                    for (int x = 0; x < 16; ++x)
                    {
                        const auto& slice = snapshot.visibleSlices[static_cast<size_t>(x)];
                        if (slice.id >= 0)
                            newLedState[x][y] = legacyLoopFeedback
                                ? (strip->isPlaying() ? 4 : 2)
                                : (snapshot.isPlaying ? (loopTriggerMode ? 8 : 6)
                                                      : (loopTriggerMode ? 5 : 3));
                    }

                    if (snapshot.pendingVisibleSliceSlot >= 0 && snapshot.pendingVisibleSliceSlot < 16)
                        newLedState[snapshot.pendingVisibleSliceSlot][y] = juce::jmax(newLedState[snapshot.pendingVisibleSliceSlot][y],
                                                                                      loopTriggerMode ? 12 : 10);
                    if (!legacyLoopFeedback && heldSlot >= 0 && heldSlot < 16)
                        newLedState[heldSlot][y] = juce::jmax(newLedState[heldSlot][y], loopTriggerMode ? 11 : 9);

                    if (legacyLoopFeedback)
                    {
                        const int currentCol = strip->getCurrentColumn();
                        if (!isGroupMuted
                            && strip->isPlaying()
                            && currentCol >= 0
                            && currentCol < 16)
                        {
                            newLedState[currentCol][y] = 15;
                        }
                    }
                    else if (!isGroupMuted
                        && playbackSlot >= 0
                        && playbackSlot < 16)
                    {
                        newLedState[playbackSlot][y] = 15;
                    }
                    else if (!isGroupMuted
                        && snapshot.activeVisibleSliceSlot >= 0
                        && snapshot.activeVisibleSliceSlot < 16)
                    {
                        newLedState[snapshot.activeVisibleSliceSlot][y] = 15;
                    }
                }
            }
            else if (strip->playMode == EnhancedAudioStrip::PlayMode::Grain)
            {
                const int anchor = strip->getGrainAnchorColumn();
                const int secondary = strip->getGrainSecondaryColumn();
                const int sizeControl = strip->getGrainSizeControlColumn();
                const int heldCount = strip->getGrainHeldCount();
                const int currentCol = strip->getCurrentColumn();
                const auto preview = strip->getGrainPreviewPositions();
                const bool showScratchTrail = strip->isPlaying()
                    || (heldCount > 0)
                    || (strip->isScratchActive())
                    || (strip->getDisplaySpeed() > 0.01f);

                auto setLevelMax = [&](int x, int level)
                {
                    if (x < 0 || x >= 16)
                        return;
                    newLedState[x][y] = juce::jmax(newLedState[x][y], level);
                };

                if (heldCount <= 0 && !showScratchTrail)
                {
                    for (int x = 0; x < 16; ++x)
                        newLedState[x][y] = 0;
                    if (!isGroupMuted && strip->isPlaying() && currentCol >= 0 && currentCol < 16)
                        newLedState[currentCol][y] = 15;
                }
                else
                {
                    for (int x = 0; x < 16; ++x)
                        newLedState[x][y] = 0;

                    // Visualize grain voice "dots" as moving LED trail on the strip row.
                    // This is active while buttons are held and while scratch movement is active.
                    for (const float p : preview)
                    {
                        if (!std::isfinite(p) || p < 0.0f || p > 1.0f)
                            continue;

                        const int px = juce::jlimit(0, 15, static_cast<int>(std::round(p * 15.0f)));
                        const int dotLevel = (heldCount > 0) ? 11 : 8;
                        setLevelMax(px, dotLevel);
                    }

                    if (!isGroupMuted && strip->isPlaying() && currentCol >= 0 && currentCol < 16)
                        setLevelMax(currentCol, 7);
                    if (secondary >= 0 && secondary < 16)
                        setLevelMax(secondary, 13);
                    if (sizeControl >= 0 && sizeControl < 16)
                        setLevelMax(sizeControl, fastBlinkOn ? 15 : 3);
                    if (anchor >= 0 && anchor < 16)
                        setLevelMax(anchor, slowBlinkOn ? 15 : 10);
                }
            }
            else if (!isGroupMuted && strip->isPlaying())
            {
                // NORMAL LOOP/GATE PLAYBACK - show a single playhead LED with the loop span dimly lit.
                const int loopStart = juce::jlimit(0, 15, strip->getLoopStart());
                const int loopEnd = juce::jlimit(loopStart + 1, 16, strip->getLoopEnd());
                for (int x = loopStart; x < loopEnd && x < 16; ++x)
                    newLedState[x][y] = 2;

                const int currentCol = strip->getCurrentColumn();
                if (currentCol >= 0 && currentCol < 16)
                    newLedState[currentCol][y] = 15;
            }
        }

        applySelectedStripHighlight(stripIndex, y);
    }
    
    // Control row (bottom row): page buttons and quantize indicator.
    for (int x = 0; x < NumControlRowPages && x < gridWidth; ++x)
        newLedState[x][CONTROL_ROW] = 5;

    if (controlModeActive)
    {
        const int activeButton = getControlButtonForMode(currentControlMode);
        if (activeButton >= 0 && activeButton < NumControlRowPages && activeButton < gridWidth)
            newLedState[activeButton][CONTROL_ROW] = 15;
    }

    if (layout.topRowEditSupported && layout.topRowEditToggleColumn >= 0 && layout.topRowEditToggleColumn < gridWidth)
    {
        const int level = layout.topRowEditActive
            ? (fastBlinkOn ? 15 : 11)
            : (slowBlinkOn ? 12 : 4);
        newLedState[layout.topRowEditToggleColumn][CONTROL_ROW] = level;
    }

    if (controlModeActive && currentControlMode == ControlMode::StepEdit)
    {
        const int selectedStripIndex = clampVisibleStrip(stepEditSelectedStrip);
        bool hasSelectedStrip = false;
        int pitchSemitones = 0;

        if (auto* selectedStrip = audioEngine->getStrip(selectedStripIndex))
        {
            hasSelectedStrip = true;
            pitchSemitones = static_cast<int>(std::round(selectedStrip->getPitchShift()));

            if (selectedStrip->getPlayMode() == EnhancedAudioStrip::PlayMode::Step)
            {
                pitchSemitones = static_cast<int>(std::round(getPitchSemitonesForDisplay(*selectedStrip)));
            }
        }

        const bool canDown = hasSelectedStrip && pitchSemitones > -24;
        const bool canUp = hasSelectedStrip && pitchSemitones < 24;

        int downLevel = canDown ? 8 : 2;
        int upLevel = canUp ? 8 : 2;
        if (pitchSemitones < 0)
            downLevel = canDown ? 13 : 3;
        else if (pitchSemitones > 0)
            upLevel = canUp ? 13 : 3;
        else if (hasSelectedStrip)
        {
            downLevel = canDown ? 9 : 2;
            upLevel = canUp ? 9 : 2;
        }

        newLedState[13][CONTROL_ROW] = downLevel;
        newLedState[14][CONTROL_ROW] = upLevel;
    }
    else if (!controlModeActive || currentControlMode == ControlMode::Normal)
    {
        const int selectedStripIndex = clampVisibleStrip(getLastMonomePressedStripRow());
        if (auto* selectedStrip = audioEngine->getStrip(selectedStripIndex))
        {
            if (selectedStrip->getPlayMode() == EnhancedAudioStrip::PlayMode::Sample)
            {
                if (auto* sampleEngine = getSampleModeEngine(selectedStripIndex, false))
                {
                    const auto triggerMode = sampleEngine->getTriggerMode();
                    newLedState[13][CONTROL_ROW] = (triggerMode == SampleTriggerMode::OneShot) ? 15 : 4;
                    newLedState[14][CONTROL_ROW] = (triggerMode == SampleTriggerMode::Loop) ? 15 : 4;
                    newLedState[15][CONTROL_ROW] = sampleEngine->isLegacyLoopEngineEnabled() ? 15 : 4;
                }
            }
        }
    }

    // Metronome pulse on control-row quantize button (row 7, col 15):
    // beat pulses dim, bar "1" pulses bright.
    if ((!controlModeActive || currentControlMode == ControlMode::Normal)
        && clampVisibleStrip(getLastMonomePressedStripRow()) >= 0)
    {
        const int selectedStripIndex = clampVisibleStrip(getLastMonomePressedStripRow());
        if (auto* selectedStrip = audioEngine->getStrip(selectedStripIndex))
        {
            if (selectedStrip->getPlayMode() != EnhancedAudioStrip::PlayMode::Sample)
            {
                if (metroPulseOn)
                    newLedState[15][CONTROL_ROW] = metroDownbeat ? metroDownbeatLevel : metroBeatLevel;
                else
                    newLedState[15][CONTROL_ROW] = metroOffLevel;
            }
        }
    }
    else if (!layout.topRowEditSupported)
    {
        if (metroPulseOn)
            newLedState[15][CONTROL_ROW] = metroDownbeat ? 15 : 7;
        else
            newLedState[15][CONTROL_ROW] = 5;
    }
    
    // Differential update
    for (int y = 0; y < gridHeight; ++y)
    {
        for (int x = 0; x < gridWidth; ++x)
        {
            if (newLedState[x][y] != ledCache[x][y])
            {
                monomeConnection.setLEDLevel(x, y, newLedState[x][y]);
                ledCache[x][y] = newLedState[x][y];
            }
        }
    }
}

//==============================================================================
