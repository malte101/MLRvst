/*
  ==============================================================================

    PluginEditor.cpp
    Modern Comprehensive UI Implementation

  ==============================================================================
*/

#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "PluginEditorPanelUtils.h"
#include "PlayheadSpeedQuantizer.h"
#include "mlrvst_build_info.h"
#include <cmath>
#include <limits>
#include <juce_audio_formats/juce_audio_formats.h>

using namespace PluginEditorPanelUtils;

namespace
{
const auto kBgTop = juce::Colour(0xff232629);
const auto kBgBottom = juce::Colour(0xff16181a);
const auto kPanelTop = juce::Colour(0xff36393d);
const auto kPanelBottom = juce::Colour(0xff272a2d);
const auto kPanelStroke = juce::Colour(0xff70757a);
const auto kPanelInnerStroke = juce::Colour(0xff242424);
const auto kAccent = juce::Colour(0xffffb347);
const auto kTextPrimary = juce::Colour(0xffefefef);
const auto kTextSecondary = juce::Colour(0xffc3c3c3);
const auto kTextMuted = juce::Colour(0xff969696);
const auto kSurfaceDark = juce::Colour(0xff1a1a1a);
constexpr int kPatternCardHeight = 96;
constexpr int kPatternCardGap = 6;

void drawPanel(juce::Graphics& g, juce::Rectangle<float> bounds, juce::Colour accent, float radius = 8.0f)
{
    g.setColour(juce::Colours::black.withAlpha(0.2f));
    g.fillRoundedRectangle(bounds.translated(0.0f, 1.5f), radius);

    juce::ColourGradient fill(kPanelTop, bounds.getX(), bounds.getY(),
                              kPanelBottom, bounds.getX(), bounds.getBottom(), false);
    g.setGradientFill(fill);
    g.fillRoundedRectangle(bounds, radius);

    juce::ColourGradient topSheen(juce::Colours::white.withAlpha(0.06f), bounds.getX(), bounds.getY(),
                                  juce::Colours::transparentWhite, bounds.getX(), bounds.getY() + (bounds.getHeight() * 0.33f), false);
    g.setGradientFill(topSheen);
    g.fillRoundedRectangle(bounds.reduced(1.0f), juce::jmax(2.0f, radius - 1.0f));

    g.setColour(kPanelStroke);
    g.drawRoundedRectangle(bounds.reduced(0.5f), radius, 1.0f);

    g.setColour(accent.withAlpha(0.22f));
    g.drawRoundedRectangle(bounds.reduced(1.5f), juce::jmax(2.0f, radius - 1.5f), 1.0f);

    g.setColour(kPanelInnerStroke);
    g.drawRoundedRectangle(bounds.reduced(2.0f), juce::jmax(2.0f, radius - 2.0f), 1.0f);
}

juce::Colour patternControlEventColour(int controlMode)
{
    using ControlMode = MlrVSTAudioProcessor::ControlMode;

    switch (static_cast<ControlMode>(controlMode))
    {
        case ControlMode::Speed:     return juce::Colour(0xff6bbcff);
        case ControlMode::Pitch:     return juce::Colour(0xffff8f6b);
        case ControlMode::Pan:       return juce::Colour(0xff6ce0c4);
        case ControlMode::Volume:    return juce::Colour(0xff88d96b);
        case ControlMode::GrainSize: return juce::Colour(0xffffc86b);
        case ControlMode::Swing:     return juce::Colour(0xffffdd74);
        case ControlMode::Gate:      return juce::Colour(0xffff7d9c);
        case ControlMode::Delay:     return juce::Colour(0xff74d6ff);
        case ControlMode::Filter:    return juce::Colour(0xffff9d5a);
        case ControlMode::Normal:
        case ControlMode::FileBrowser:
        case ControlMode::GroupAssign:
        case ControlMode::Modulation:
        case ControlMode::Preset:
        case ControlMode::StepEdit:
        default:                     return kAccent;
    }
}

int sceneControlLaneIndex(const ScenePerformanceEvent& event)
{
    using ControlMode = MlrVSTAudioProcessor::ControlMode;

    switch (static_cast<ControlMode>(event.controlMode))
    {
        case ControlMode::Speed:     return 0;
        case ControlMode::Pitch:     return 1;
        case ControlMode::Pan:       return 2;
        case ControlMode::Volume:    return 3;
        case ControlMode::GrainSize: return 4;
        case ControlMode::Swing:     return 5;
        case ControlMode::Delay:     return 6;
        case ControlMode::Filter:    return 7;
        case ControlMode::Normal:
        case ControlMode::Gate:
        case ControlMode::FileBrowser:
        case ControlMode::GroupAssign:
        case ControlMode::Modulation:
        case ControlMode::Preset:
        case ControlMode::StepEdit:
        default:                     return 0;
    }
}

float normalizeSceneControlValue(const ScenePerformanceEvent& event)
{
    switch (event.controlTarget)
    {
        case ScenePerformanceControlTarget::Speed:
            return juce::jlimit(0.0f, 1.0f, (event.value - 0.125f) / (8.0f - 0.125f));
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
        case ScenePerformanceControlTarget::FilterMorph:
            return juce::jlimit(0.0f, 1.0f, event.value);
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

float denormalizeSceneControlValue(const ScenePerformanceEvent& event, float normalizedValue)
{
    const float t = juce::jlimit(0.0f, 1.0f, normalizedValue);

    switch (event.controlTarget)
    {
        case ScenePerformanceControlTarget::Speed:
            return 0.125f + (t * (8.0f - 0.125f));
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

juce::String describeSceneEvent(const ScenePerformanceEvent& event)
{
    if (event.type == ScenePerformanceEventType::Trigger)
        return "Trig S" + juce::String(event.stripIndex + 1) + " @ " + juce::String(event.timeBeats, 2);

    using ControlMode = MlrVSTAudioProcessor::ControlMode;
    juce::String laneName;
    switch (static_cast<ControlMode>(event.controlMode))
    {
        case ControlMode::Speed: laneName = "Speed"; break;
        case ControlMode::Pitch: laneName = "Pitch"; break;
        case ControlMode::Pan: laneName = "Pan"; break;
        case ControlMode::Volume: laneName = "Volume"; break;
        case ControlMode::GrainSize: laneName = "Grain"; break;
        case ControlMode::Swing: laneName = "Swing"; break;
        case ControlMode::Delay: laneName = "Delay"; break;
        case ControlMode::Filter: laneName = "Filter"; break;
        case ControlMode::Normal:
        case ControlMode::Gate:
        case ControlMode::FileBrowser:
        case ControlMode::GroupAssign:
        case ControlMode::Modulation:
        case ControlMode::Preset:
        case ControlMode::StepEdit:
        default: laneName = "Ctrl"; break;
    }

    return laneName + " S" + juce::String(event.stripIndex + 1)
        + " @ " + juce::String(event.timeBeats, 2);
}

struct SceneTimelineLayout
{
    juce::Rectangle<float> bounds;
    juce::Rectangle<float> controlArea;
    juce::Rectangle<float> triggerArea;
    float controlLaneHeight = 0.0f;
    float triggerLaneHeight = 0.0f;
};

SceneTimelineLayout makeSceneTimelineLayout(juce::Rectangle<float> bounds)
{
    SceneTimelineLayout layout;
    layout.bounds = bounds;

    const float gap = 4.0f;
    const float triggerHeight = juce::jmax(54.0f, bounds.getHeight() * 0.42f);
    layout.controlArea = bounds.withTrimmedBottom(triggerHeight + gap);
    layout.triggerArea = bounds.withTrimmedTop(layout.controlArea.getHeight() + gap);
    layout.controlLaneHeight = layout.controlArea.getHeight() / 8.0f;
    layout.triggerLaneHeight = layout.triggerArea.getHeight()
        / static_cast<float>(juce::jmax(1, MlrVSTAudioProcessor::MaxStrips));
    return layout;
}

juce::Rectangle<float> sceneTriggerMarkerBounds(const SceneTimelineLayout& layout,
                                                const ScenePerformanceEvent& event,
                                                double lengthBeats)
{
    const float x = layout.bounds.getX()
        + (layout.bounds.getWidth() * static_cast<float>(event.timeBeats / juce::jmax(1.0, lengthBeats)));
    const int laneIndex = juce::jlimit(0, MlrVSTAudioProcessor::MaxStrips - 1, event.stripIndex);
    const float laneTop = layout.triggerArea.getY() + (layout.triggerLaneHeight * static_cast<float>(laneIndex));
    return juce::Rectangle<float>(x - 4.0f,
                                  laneTop + 1.0f,
                                  8.0f,
                                  juce::jmax(6.0f, layout.triggerLaneHeight - 2.0f));
}

juce::Rectangle<float> sceneControlMarkerBounds(const SceneTimelineLayout& layout,
                                                const ScenePerformanceEvent& event,
                                                double lengthBeats)
{
    const float x = layout.bounds.getX()
        + (layout.bounds.getWidth() * static_cast<float>(event.timeBeats / juce::jmax(1.0, lengthBeats)));
    const int laneIndex = juce::jlimit(0, 7, sceneControlLaneIndex(event));
    const float laneTop = layout.controlArea.getY() + (layout.controlLaneHeight * static_cast<float>(laneIndex));
    const float laneBottom = laneTop + layout.controlLaneHeight;
    const float valueY = laneBottom - (normalizeSceneControlValue(event) * juce::jmax(3.0f, layout.controlLaneHeight - 4.0f)) - 2.0f;
    return juce::Rectangle<float>(x - 5.0f, valueY - 5.0f, 10.0f, 10.0f);
}

int findBestMatchingSceneEventIndex(const std::vector<ScenePerformanceEvent>& events,
                                    const ScenePerformanceEvent& target)
{
    int bestIndex = -1;
    double bestScore = std::numeric_limits<double>::max();

    for (int i = 0; i < static_cast<int>(events.size()); ++i)
    {
        const auto& candidate = events[static_cast<size_t>(i)];
        if (candidate.type != target.type)
            continue;

        double score = std::abs(candidate.timeBeats - target.timeBeats) * 8.0;
        if (candidate.type == ScenePerformanceEventType::Trigger)
        {
            if (candidate.stripIndex != target.stripIndex)
                score += 4.0;
            if (candidate.column != target.column)
                score += 1.0;
            if (candidate.isNoteOn != target.isNoteOn)
                score += 2.0;
            if (candidate.sampleSliceId != target.sampleSliceId)
                score += 0.25;
        }
        else
        {
            if (candidate.controlMode != target.controlMode)
                score += 4.0;
            if (candidate.controlRow != target.controlRow)
                score += 1.0;
            if (candidate.controlTarget != target.controlTarget)
                score += 4.0;
            if (candidate.stripIndex != target.stripIndex)
                score += 2.0;
            score += std::abs(static_cast<double>(candidate.value - target.value));
        }

        if (score < bestScore)
        {
            bestScore = score;
            bestIndex = i;
        }
    }

    return bestIndex;
}

enum class StripHarmonyOverlayState
{
    None = 0,
    ScaleLocked,
    RootLocked
};

StripHarmonyOverlayState getStripHarmonyOverlayState(MlrVSTAudioProcessor& processor, int stripIndex)
{
    switch (processor.getLoopPitchRole(stripIndex))
    {
        case MlrVSTAudioProcessor::LoopPitchRole::Master:
            return StripHarmonyOverlayState::ScaleLocked;
        case MlrVSTAudioProcessor::LoopPitchRole::Sync:
            return StripHarmonyOverlayState::RootLocked;
        case MlrVSTAudioProcessor::LoopPitchRole::None:
        default:
            return StripHarmonyOverlayState::None;
    }
}

void paintStripHarmonyOverlay(juce::Graphics& g,
                              juce::Rectangle<float> bounds,
                              StripHarmonyOverlayState state,
                              juce::Colour stripColor)
{
    if (state == StripHarmonyOverlayState::None)
        return;

    const juce::Colour accent = state == StripHarmonyOverlayState::RootLocked
        ? juce::Colour(0xff75c7ca)
        : stripColor.interpolatedWith(kAccent, 0.48f).withMultipliedSaturation(0.72f);

    auto inner = bounds.reduced(3.0f);
    const float radius = 7.0f;

    juce::ColourGradient topGlow(accent.withAlpha(0.085f),
                                 inner.getX() + 12.0f, inner.getY() + 1.5f,
                                 juce::Colours::transparentBlack,
                                 inner.getX() + (inner.getWidth() * 0.48f), inner.getY() + 10.0f,
                                 false);
    g.setGradientFill(topGlow);
    g.fillRoundedRectangle(inner, radius);

    const float topW = juce::jlimit(34.0f, juce::jmax(34.0f, inner.getWidth() - 24.0f), inner.getWidth() * 0.2f);
    const float sideH = juce::jlimit(18.0f, juce::jmax(18.0f, inner.getHeight() - 28.0f), inner.getHeight() * 0.12f);
    auto topAccent = juce::Rectangle<float>(inner.getX() + 10.0f, inner.getY() + 2.0f, topW, 1.6f);
    auto sideAccent = juce::Rectangle<float>(inner.getX() + 2.0f, inner.getY() + 11.0f, 1.6f, sideH);

    g.setColour(accent.withAlpha(0.30f));
    g.fillRoundedRectangle(topAccent, 1.0f);
    g.fillRoundedRectangle(sideAccent, 1.0f);

    g.setColour(accent.withAlpha(0.11f));
    g.drawRoundedRectangle(inner, radius, 0.8f);
}

void enableAltClickReset(juce::Slider& slider, double defaultValue)
{
    // JUCE supports modifier-click reset when a double-click return value is set.
    slider.setDoubleClickReturnValue(true, defaultValue);
}

void styleUiButton(juce::Button& button, bool primary = false)
{
    button.setColour(juce::TextButton::buttonColourId,
                     primary ? kAccent.withAlpha(0.9f) : juce::Colour(0xff3b4146));
    button.setColour(juce::TextButton::buttonOnColourId,
                     primary ? kAccent.brighter(0.12f) : juce::Colour(0xff4a5258));
    button.setColour(juce::TextButton::textColourOffId,
                     primary ? juce::Colour(0xff141414) : kTextPrimary);
    button.setColour(juce::TextButton::textColourOnId,
                     primary ? juce::Colour(0xff101010) : juce::Colour(0xfff5f5f5));
}

void styleUiCombo(juce::ComboBox& combo)
{
    combo.setColour(juce::ComboBox::backgroundColourId, juce::Colour(0xff353b42));
    combo.setColour(juce::ComboBox::outlineColourId, juce::Colour(0xff6a7076));
    combo.setColour(juce::ComboBox::textColourId, kTextPrimary);
    combo.setColour(juce::ComboBox::arrowColourId, kAccent.brighter(0.08f));
    combo.setJustificationType(juce::Justification::centredLeft);
}

juce::String getGrainArpModeName(int mode)
{
    switch (juce::jlimit(0, 5, mode))
    {
        case 0: return "Octave";
        case 1: return "Power";
        case 2: return "Zigzag";
        case 3: return "Major";
        case 4: return "Minor";
        case 5: return "Penta";
        default: break;
    }
    return "Octave";
}

juce::String getPlayheadSpeedLabel(float ratio)
{
    return juce::String(PlayheadSpeedQuantizer::labelForRatio(ratio));
}

juce::String getGrainSpeedLabel(float speed)
{
    speed = PlayheadSpeedQuantizer::grainPlaybackSpeedFromControl(speed);
    if (std::abs(speed) <= 1.0e-4f)
        return "0";

    if (std::abs(speed - std::round(speed)) <= 1.0e-3f)
        return juce::String(static_cast<int>(std::round(speed)));

    return juce::String(speed, speed < 1.0f ? 2 : 1);
}

float getSpeedControlValueForStrip(const EnhancedAudioStrip& strip)
{
    if (strip.getPlayMode() == EnhancedAudioStrip::PlayMode::Grain)
        return PlayheadSpeedQuantizer::grainControlValueFromPlaybackSpeed(strip.getPlaybackSpeed());

    return PlayheadSpeedQuantizer::quantizeRatio(strip.getPlayheadSpeedRatio());
}

juce::String getCompactNoteName(int midiNote)
{
    static constexpr const char* names[] = { "C", "C#", "D", "D#", "E", "F",
                                             "F#", "G", "G#", "A", "A#", "B" };
    if (midiNote < 0)
        return "?";
    const int clamped = juce::jlimit(0, 127, midiNote);
    return juce::String(names[clamped % 12]) + juce::String((clamped / 12) - 1);
}

int getNearestMidiForPitchClass(int referenceMidi, int pitchClass)
{
    const int normalizedPitchClass = ((pitchClass % 12) + 12) % 12;
    const int clampedReference = juce::jlimit(0, 127, referenceMidi);
    int bestMidi = normalizedPitchClass;
    int bestDistance = std::numeric_limits<int>::max();

    for (int octave = -1; octave <= 10; ++octave)
    {
        const int midi = (octave * 12) + normalizedPitchClass;
        if (midi < 0 || midi > 127)
            continue;

        const int distance = std::abs(midi - clampedReference);
        if (distance < bestDistance)
        {
            bestDistance = distance;
            bestMidi = midi;
        }
    }

    return bestMidi;
}

int modTargetToComboId(ModernAudioEngine::ModTarget target)
{
    return performanceTargetToComboId(sanitizeModPerformanceTarget(target));
}

ModernAudioEngine::ModTarget comboIdToModTarget(int id)
{
    return sanitizeModPerformanceTarget(performanceTargetFromComboId(id));
}

bool modTargetAllowsBipolar(ModernAudioEngine::ModTarget target)
{
    return performanceTargetSupportsBipolar(sanitizeModPerformanceTarget(target));
}

juce::String makeRetriggerHintText(float rawStepValue01, float depth01)
{
    const float amount = juce::jlimit(0.0f, 1.0f, rawStepValue01 * depth01);
    return "Retrig now " + retriggerDivisionLabel(amount)
        + "  |  low = slower, high = faster";
}

float quantizeRearrangeStepValue(float value01)
{
    return juce::jlimit(0.0f, 1.0f,
                        std::round(juce::jlimit(0.0f, 1.0f, value01)
                                   * static_cast<float>(ModernAudioEngine::MaxColumns - 1))
                            / static_cast<float>(juce::jmax(1, ModernAudioEngine::MaxColumns - 1)));
}

float defaultRearrangeStepValueUi(int absoluteStep)
{
    return quantizeRearrangeStepValue(static_cast<float>(absoluteStep % ModernAudioEngine::ModSteps)
                                      / static_cast<float>(juce::jmax(1, ModernAudioEngine::ModSteps - 1)));
}

int rearrangeSliceDisplayIndex(float value01)
{
    return 1 + juce::jlimit(0,
                            ModernAudioEngine::MaxColumns - 1,
                            static_cast<int>(std::round(juce::jlimit(0.0f, 1.0f, value01)
                                                        * static_cast<float>(ModernAudioEngine::MaxColumns - 1))));
}

juce::String makeRearrangeHintText(float rawStepValue01)
{
    return "Src S" + juce::String(rearrangeSliceDisplayIndex(rawStepValue01))
        + "  |  ramp 1->16 = normal";
}

const std::array<float, 16>& modRateChoices()
{
    return PlayheadSpeedQuantizer::kSpeedRatios;
}

juce::String modRateLabelForValue(float rate)
{
    return juce::String(PlayheadSpeedQuantizer::labelForRatio(rate));
}

int modRateToComboId(float rate)
{
    return PlayheadSpeedQuantizer::nearestSpeedIndex(rate) + 1;
}

float comboIdToModRate(int comboId)
{
    const int index = juce::jlimit(0,
                                   static_cast<int>(modRateChoices().size()) - 1,
                                   comboId - 1);
    return modRateChoices()[static_cast<size_t>(index)];
}

juce::String modTargetDisplayName(ModernAudioEngine::ModTarget target)
{
    return performanceTargetDisplayName(sanitizeModPerformanceTarget(target));
}

enum class StepCellModifierGesture
{
    None = 0,
    Divide,
    RampUp,
    RampDown
};

StepCellModifierGesture getStepCellModifierGesture(const juce::ModifierKeys& mods)
{
    // Keep modifier priority aligned with StepSequencerDisplay cell editing.
    if (mods.isCommandDown())
        return StepCellModifierGesture::Divide;
    if (mods.isCtrlDown())
        return StepCellModifierGesture::RampUp;
    if (mods.isAltDown())
        return StepCellModifierGesture::RampDown;
    return StepCellModifierGesture::None;
}

float shapeCurvePhaseUi(float phase01, float bend, ModernAudioEngine::ModCurveShape shape);

float sampleModSubdivisionValueUi(float startValue,
                                  float endValue,
                                  int subdivisions,
                                  float phase01)
{
    const float start = juce::jlimit(0.0f, 1.0f, startValue);
    const float end = juce::jlimit(0.0f, 1.0f, endValue);
    const int subdiv = juce::jlimit(1, ModernAudioEngine::ModMaxStepSubdivisions, subdivisions);
    const float phase = juce::jlimit(0.0f, 0.999999f, phase01);

    if (subdiv <= 1)
        return start;

    const float subdivPos = phase * static_cast<float>(subdiv);
    const int subdivIndex = juce::jlimit(0, subdiv - 1, static_cast<int>(std::floor(subdivPos)));
    const float t = static_cast<float>(subdivIndex) / static_cast<float>(juce::jmax(1, subdiv - 1));
    return juce::jlimit(0.0f, 1.0f, start + ((end - start) * t));
}

void computeSingleModCellRamp(float sourceStart,
                              float sourceEnd,
                              int deltaY,
                              bool rampUpMode,
                              float& outStart,
                              float& outEnd)
{
    const float clampedStart = juce::jlimit(0.0f, 1.0f, sourceStart);
    const float clampedEnd = juce::jlimit(0.0f, 1.0f, sourceEnd);
    float peak = juce::jlimit(0.0f, 1.0f, juce::jmax(clampedStart, clampedEnd));
    if (peak < 0.001f)
        peak = 1.0f;

    const float depth = juce::jlimit(0.0f, 1.0f, 0.5f + (static_cast<float>(-deltaY) / 160.0f));
    const float low = juce::jlimit(0.0f, 1.0f, peak * (1.0f - depth));

    if (rampUpMode)
    {
        outStart = low;
        outEnd = peak;
    }
    else
    {
        outStart = peak;
        outEnd = low;
    }
}

juce::String pitchScaleDisplayName(ModernAudioEngine::PitchScale scale, bool compact)
{
    switch (scale)
    {
        case ModernAudioEngine::PitchScale::Major: return compact ? "Maj" : "Major";
        case ModernAudioEngine::PitchScale::Minor: return compact ? "Min" : "Minor";
        case ModernAudioEngine::PitchScale::Dorian: return compact ? "Dor" : "Dorian";
        case ModernAudioEngine::PitchScale::PentatonicMinor: return compact ? "Pent" : "Pentatonic";
        case ModernAudioEngine::PitchScale::Chromatic:
        default: return compact ? "Chr" : "Chromatic";
    }
}

juce::String loopPitchSyncTimingDisplayName(MlrVSTAudioProcessor::LoopPitchSyncTiming timing, bool compact)
{
    switch (timing)
    {
        case MlrVSTAudioProcessor::LoopPitchSyncTiming::NextTrigger: return compact ? "Trig" : "Next Trigger";
        case MlrVSTAudioProcessor::LoopPitchSyncTiming::NextLoop: return compact ? "Loop" : "Next Loop";
        case MlrVSTAudioProcessor::LoopPitchSyncTiming::NextBar: return compact ? "Bar" : "Next Bar";
        case MlrVSTAudioProcessor::LoopPitchSyncTiming::Immediate:
        default: return compact ? "Imm" : "Immediate";
    }
}

juce::String loopPitchRoleDisplayName(MlrVSTAudioProcessor::LoopPitchRole role, bool compact)
{
    switch (role)
    {
        case MlrVSTAudioProcessor::LoopPitchRole::Master: return compact ? "PM" : "Master";
        case MlrVSTAudioProcessor::LoopPitchRole::Sync: return compact ? "PS" : "Sync";
        case MlrVSTAudioProcessor::LoopPitchRole::None:
        default: return compact ? "Free" : "Free";
    }
}

juce::String playModeDisplayName(EnhancedAudioStrip::PlayMode mode, bool compact)
{
    switch (mode)
    {
        case EnhancedAudioStrip::PlayMode::OneShot: return compact ? "One" : "One-Shot";
        case EnhancedAudioStrip::PlayMode::Loop: return "Loop";
        case EnhancedAudioStrip::PlayMode::Gate: return "Gate";
        case EnhancedAudioStrip::PlayMode::Step: return "Step";
        case EnhancedAudioStrip::PlayMode::Grain: return compact ? "Grn" : "Grain";
        case EnhancedAudioStrip::PlayMode::Sample: return "Flip";
        default: return "Mode";
    }
}

juce::String directionModeDisplayName(EnhancedAudioStrip::DirectionMode mode, bool compact)
{
    switch (mode)
    {
        case EnhancedAudioStrip::DirectionMode::Normal: return compact ? "Norm" : "Normal";
        case EnhancedAudioStrip::DirectionMode::Reverse: return compact ? "Rev" : "Reverse";
        case EnhancedAudioStrip::DirectionMode::PingPong: return compact ? "Ping" : "Ping-Pong";
        case EnhancedAudioStrip::DirectionMode::Random: return compact ? "Rand" : "Random";
        case EnhancedAudioStrip::DirectionMode::RandomWalk: return compact ? "Walk" : "Random Walk";
        case EnhancedAudioStrip::DirectionMode::RandomSlice: return compact ? "Slice" : "Random Slice";
        default: return compact ? "Norm" : "Normal";
    }
}

juce::String confidencePercentText(float confidence)
{
    if (!(confidence > 0.0f))
        return {};
    return juce::String(static_cast<int>(std::round(juce::jlimit(0.0f, 1.0f, confidence) * 100.0f))) + "%";
}

int curveShapeToComboId(ModernAudioEngine::ModCurveShape shape)
{
    switch (shape)
    {
        case ModernAudioEngine::ModCurveShape::Linear: return 1;
        case ModernAudioEngine::ModCurveShape::ExponentialUp: return 2;
        case ModernAudioEngine::ModCurveShape::ExponentialDown: return 3;
        case ModernAudioEngine::ModCurveShape::Sine: return 4;
        case ModernAudioEngine::ModCurveShape::Square: return 5;
        default: return 1;
    }
}

ModernAudioEngine::ModCurveShape comboIdToCurveShape(int id)
{
    switch (id)
    {
        case 2: return ModernAudioEngine::ModCurveShape::ExponentialUp;
        case 3: return ModernAudioEngine::ModCurveShape::ExponentialDown;
        case 4: return ModernAudioEngine::ModCurveShape::Sine;
        case 5: return ModernAudioEngine::ModCurveShape::Square;
        case 1:
        default: return ModernAudioEngine::ModCurveShape::Linear;
    }
}

float shapeCurvePhaseUi(float phase01, float bend, ModernAudioEngine::ModCurveShape shape)
{
    const float t = juce::jlimit(0.0f, 1.0f, phase01);
    const float b = juce::jlimit(-1.0f, 1.0f, bend);
    const float amount = std::abs(b);

    switch (shape)
    {
        case ModernAudioEngine::ModCurveShape::Linear:
            return t;
        case ModernAudioEngine::ModCurveShape::ExponentialUp:
        {
            const float exp = 1.0f + (15.0f * amount);
            return std::pow(t, exp);
        }
        case ModernAudioEngine::ModCurveShape::ExponentialDown:
        {
            const float exp = 1.0f + (15.0f * amount);
            return 1.0f - std::pow(1.0f - t, exp);
        }
        case ModernAudioEngine::ModCurveShape::Sine:
        {
            const float phase = juce::jlimit(0.0f, 1.0f, t + (b * 0.45f));
            return 0.5f - (0.5f * std::cos(phase * juce::MathConstants<float>::pi));
        }
        case ModernAudioEngine::ModCurveShape::Square:
        {
            const float duty = juce::jlimit(0.02f, 0.98f, 0.5f + (b * 0.45f));
            return (t >= duty) ? 1.0f : 0.0f;
        }
        default:
            return t;
    }
}

float shapeSubdivisionBendPhaseUi(float phase01, float bend)
{
    const float t = juce::jlimit(0.0f, 1.0f, phase01);
    const float b = juce::jlimit(-1.0f, 1.0f, bend);
    const float amount = std::abs(b);
    const float exp = 1.0f + (18.0f * amount);
    return b >= 0.0f ? std::pow(t, exp) : (1.0f - std::pow(1.0f - t, exp));
}

}

//==============================================================================
// WaveformDisplay Implementation
//==============================================================================

WaveformDisplay::WaveformDisplay()
{
    setOpaque(false);
    setMouseCursor(juce::MouseCursor::NormalCursor);
}

float WaveformDisplay::normalizedPositionFromX(float x) const
{
    const float width = static_cast<float>(juce::jmax(1, getWidth()));
    const float clampedX = juce::jlimit(0.0f, width, x);
    const float localNorm = clampedX / width;
    return juce::jlimit(0.0f, 1.0f, viewStartNorm + (localNorm * viewSpanNorm));
}

float WaveformDisplay::xFromNormalizedPosition(float normalizedPosition, float width) const
{
    if (viewSpanNorm <= 1.0e-6f)
        return 0.0f;

    return ((normalizedPosition - viewStartNorm) / viewSpanNorm) * width;
}

void WaveformDisplay::resetView()
{
    viewStartNorm = 0.0f;
    viewSpanNorm = 1.0f;
    repaint();
}

void WaveformDisplay::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();
    
    // Safety check for invalid bounds
    if (bounds.getWidth() <= 0 || bounds.getHeight() <= 0 || 
        !std::isfinite(bounds.getWidth()) || !std::isfinite(bounds.getHeight()))
        return;
    
    // Background with depth so grain overlays read clearly.
    juce::ColourGradient bgGrad(kSurfaceDark.brighter(0.12f), bounds.getX(), bounds.getY(),
                                kSurfaceDark.darker(0.22f), bounds.getRight(), bounds.getBottom(), false);
    g.setGradientFill(bgGrad);
    g.fillRoundedRectangle(bounds, 4.0f);

    g.setColour(kPanelStroke.withAlpha(0.85f));
    g.drawRoundedRectangle(bounds, 4.0f, 1.0f);
    
    if (!hasAudio)
    {
        // Keep the gradient look, but tint it with the strip color so empty strips
        // feel connected to their lane identity.
        const auto tint = waveformColor.withAlpha(0.18f);
        juce::ColourGradient emptyGrad(
            kSurfaceDark.brighter(0.16f).interpolatedWith(tint.brighter(0.45f), 0.26f),
            bounds.getX(), bounds.getY(),
            kSurfaceDark.darker(0.24f).interpolatedWith(tint.darker(0.35f), 0.22f),
            bounds.getRight(), bounds.getBottom(),
            false);
        g.setGradientFill(emptyGrad);
        g.fillRoundedRectangle(bounds.reduced(0.5f), 4.0f);

        // "No Sample" text (like reference image)
        g.setColour(kTextMuted);
        g.setFont(juce::Font(juce::FontOptions(14.0f)));
        g.drawText("No Sample", bounds, juce::Justification::centred);
        return;
    }

    const juce::Colour grainAccent = waveformColor.interpolatedWith(kAccent, 0.35f)
                                       .withMultipliedSaturation(1.1f)
                                       .withMultipliedBrightness(1.08f);
    const float visibleStart = juce::jlimit(0.0f, 1.0f, viewStartNorm);
    const float visibleSpan = juce::jlimit(1.0f / 64.0f, 1.0f, viewSpanNorm);
    const float visibleEnd = juce::jlimit(visibleStart, 1.0f, visibleStart + visibleSpan);
    const auto normToX = [&](float normalized) -> float
    {
        return xFromNormalizedPosition(normalized, bounds.getWidth());
    };
    const auto isNormVisible = [&](float normalized) -> bool
    {
        return normalized >= (visibleStart - 1.0e-4f) && normalized <= (visibleEnd + 1.0e-4f);
    };
    
    // Draw waveform
    if (!thumbnail.empty())
    {
        juce::Path waveformPath;
        auto height = bounds.getHeight();
        auto centerY = height * 0.5f;

        const int thumbCount = static_cast<int>(thumbnail.size());
        const int firstIndex = juce::jlimit(0, juce::jmax(0, thumbCount - 1),
                                            static_cast<int>(std::floor(visibleStart * static_cast<float>(juce::jmax(1, thumbCount - 1)))));
        const int lastIndex = juce::jlimit(firstIndex, juce::jmax(0, thumbCount - 1),
                                           static_cast<int>(std::ceil(visibleEnd * static_cast<float>(juce::jmax(1, thumbCount - 1)))));

        waveformPath.startNewSubPath(normToX(static_cast<float>(firstIndex) / static_cast<float>(juce::jmax(1, thumbCount - 1))), centerY);

        for (int i = firstIndex; i <= lastIndex; ++i)
        {
            const float fullNorm = static_cast<float>(i) / static_cast<float>(juce::jmax(1, thumbCount - 1));
            auto x = normToX(fullNorm);
            const float displayValue = juce::jlimit(0.0f, 1.0f, thumbnail[static_cast<size_t>(i)] * visualGain);
            auto y = centerY - (displayValue * centerY * 0.9f);
            
            // Safety check for valid coordinates
            if (std::isfinite(x) && std::isfinite(y))
                waveformPath.lineTo(x, y);
        }
        
        // Mirror bottom half
        for (int i = lastIndex; i >= firstIndex; --i)
        {
            const float fullNorm = static_cast<float>(i) / static_cast<float>(juce::jmax(1, thumbCount - 1));
            auto x = normToX(fullNorm);
            const float displayValue = juce::jlimit(0.0f, 1.0f, thumbnail[static_cast<size_t>(i)] * visualGain);
            auto y = centerY + (displayValue * centerY * 0.9f);
            
            // Safety check for valid coordinates
            if (std::isfinite(x) && std::isfinite(y))
                waveformPath.lineTo(x, y);
        }
        
        waveformPath.closeSubPath();
        
        // Fill waveform with custom color
        g.setColour(waveformColor.withAlpha(0.5f));
        g.fillPath(waveformPath);
        
        // Outline
        g.setColour(waveformColor.brighter(0.2f));
        g.strokePath(waveformPath, juce::PathStrokeType(1.35f));
    }
    
    // Draw loop points with matching waveform color
    if (maxColumns > 0)
    {
        const float loopStartNorm = juce::jlimit(0.0f, 1.0f, loopStart / static_cast<float>(juce::jmax(1, maxColumns)));
        const float loopEndNorm = juce::jlimit(0.0f, 1.0f, loopEnd / static_cast<float>(juce::jmax(1, maxColumns)));
        auto loopStartX = normToX(loopStartNorm);
        auto loopEndX = normToX(loopEndNorm);
        auto rectWidth = loopEndX - loopStartX;
        auto rectHeight = bounds.getHeight();
        
        // Strict safety check - JUCE requires positive, finite dimensions
        if (std::isfinite(loopStartX) && std::isfinite(loopEndX) && 
            std::isfinite(rectWidth) && std::isfinite(rectHeight) &&
            rectWidth > 0.0f && rectHeight > 0.0f &&
            loopStartX >= 0.0f && loopStartX < bounds.getWidth())
        {
            // Fill with transparent waveform color
            g.setColour(waveformColor.withAlpha(0.25f));
            g.fillRect(loopStartX, 0.0f, rectWidth, rectHeight);
            
            // Draw loop markers with semi-transparent waveform color
            g.setColour(waveformColor.withAlpha(0.95f));
            g.drawLine(loopStartX, 0.0f, loopStartX, rectHeight, 2.0f);
            g.drawLine(loopEndX, 0.0f, loopEndX, rectHeight, 2.0f);
        }
    }
    
    // Draw playback position with matching waveform color (darker)
    if (std::isfinite(playbackPosition) && playbackPosition >= 0.0 && playbackPosition <= 1.0)
    {
        auto playX = normToX(static_cast<float>(playbackPosition));
        if (std::isfinite(playX))
        {
            if (grainWindowOverlayEnabled && grainWindowNorm > 0.0)
            {
                const auto winW = juce::jlimit(1.0f,
                                               bounds.getWidth(),
                                               static_cast<float>(grainWindowNorm * bounds.getWidth()));
                auto x0 = static_cast<float>(playX) - (winW * 0.5f);
                x0 = juce::jlimit(0.0f, bounds.getWidth() - winW, x0);
                auto windowRect = juce::Rectangle<float>(x0, 0.0f, winW, bounds.getHeight()).reduced(0.0f, 1.0f);
                juce::ColourGradient winGrad(grainAccent.withAlpha(0.08f), windowRect.getX(), windowRect.getY(),
                                             grainAccent.withAlpha(0.24f), windowRect.getCentreX(), windowRect.getCentreY(), true);
                g.setGradientFill(winGrad);
                g.fillRoundedRectangle(windowRect, 2.5f);
                g.setColour(grainAccent.withAlpha(0.42f));
                g.drawRoundedRectangle(windowRect, 2.5f, 1.0f);
            }

            g.setColour(grainAccent.withAlpha(0.2f));
            g.drawLine(static_cast<float>(playX), 0.0f, static_cast<float>(playX),
                       static_cast<float>(bounds.getHeight()), 7.0f);
            g.setColour(grainAccent.withAlpha(0.98f));
            g.drawLine(static_cast<float>(playX), 0.0f, static_cast<float>(playX),
                       static_cast<float>(bounds.getHeight()), 2.0f);
            g.fillEllipse(static_cast<float>(playX) - 2.6f, 1.0f, 5.2f, 5.2f);
        }
    }

    // Draw slice markers overlay for active mode only.
    if (waveformTotalSamples > 0)
    {
        const auto drawSliceSet = [&](const std::array<int, 16>& slices, juce::Colour colour, float thickness)
        {
            g.setColour(colour);
            for (int i = 0; i < 16; ++i)
            {
                const float norm = juce::jlimit(0.0f, 1.0f,
                                                static_cast<float>(slices[static_cast<size_t>(i)])
                                                / static_cast<float>(juce::jmax(1, waveformTotalSamples - 1)));
                if (!isNormVisible(norm))
                    continue;
                const float x = normToX(norm);
                if (std::isfinite(x))
                    g.drawLine(x, 0.0f, x, bounds.getHeight(), thickness);
            }
        };

        const auto markerColor = waveformColor.withAlpha(transientSlicesActive ? 0.95f : 0.7f);
        if (transientSlicesActive)
            drawSliceSet(transientSliceSamples, markerColor, 1.7f);
        else
            drawSliceSet(normalSliceSamples, markerColor, 1.2f);
    }

    // Draw column dividers
    g.setColour(juce::Colour(0xff4a4a4a).withAlpha(grainWindowOverlayEnabled ? 0.55f : 1.0f));
    for (int i = 1; i < maxColumns; ++i)
    {
        const float norm = i / static_cast<float>(juce::jmax(1, maxColumns));
        if (!isNormVisible(norm))
            continue;
        auto x = normToX(norm);
        if (std::isfinite(x))
            g.drawLine(x, 0, x, bounds.getHeight(), 0.5f);
    }

    if (grainWindowOverlayEnabled)
    {
        g.setColour(grainAccent.withAlpha(0.22f));
        int markerIdx = 0;
        const float markerHalfHeight = 6.0f;
        const float markerRadius = 3.2f;
        const float markerGlowRadius = 6.4f;
        const float edgePad = juce::jmax(markerHalfHeight, markerGlowRadius) + 1.0f;
        const float maxPitchTravel = juce::jmax(1.0f, (bounds.getHeight() * 0.5f) - edgePad);
        for (const float marker : grainMarkerPositions)
        {
            if (marker < 0.0f || marker > 1.0f || !std::isfinite(marker))
            {
                ++markerIdx;
                continue;
            }
            if (!isNormVisible(marker))
            {
                ++markerIdx;
                continue;
            }
            const float x = normToX(marker);
            float pitchNorm = juce::jlimit(-1.0f, 1.0f, grainHudPitchSemitones / 48.0f);
            if (markerIdx >= 0 && markerIdx < static_cast<int>(grainMarkerPitchNorms.size()))
            {
                const float markerPitchNorm = grainMarkerPitchNorms[static_cast<size_t>(markerIdx)];
                if (std::isfinite(markerPitchNorm))
                    pitchNorm = juce::jlimit(-1.0f, 1.0f, markerPitchNorm);
            }
            const float jitterNorm = juce::jlimit(0.0f, 1.0f, grainHudPitchJitterSemitones / 48.0f);
            const float phase = static_cast<float>(juce::Time::getMillisecondCounterHiRes() * 0.0025);
            const float yBase = (bounds.getHeight() * 0.5f) - (pitchNorm * maxPitchTravel);
            const float yJitter = std::sin((static_cast<float>(markerIdx) * 1.3f) + phase)
                * (grainHudArpDepth * 0.08f + jitterNorm * 0.12f) * bounds.getHeight();
            const float yCenter = juce::jlimit(edgePad, bounds.getHeight() - edgePad, yBase + yJitter);
            g.drawLine(x, yCenter - markerHalfHeight, x, yCenter + markerHalfHeight, 2.4f);
            g.setColour(grainAccent.withAlpha(0.84f));
            g.fillEllipse(x - markerRadius, yCenter - markerRadius, markerRadius * 2.0f, markerRadius * 2.0f);
            g.setColour(grainAccent.withAlpha(0.26f));
            g.fillEllipse(x - markerGlowRadius, yCenter - markerGlowRadius, markerGlowRadius * 2.0f, markerGlowRadius * 2.0f);
            g.setColour(grainAccent.withAlpha(0.22f));
            ++markerIdx;
        }
    }

    if (grainHudOverlayEnabled)
    {
        auto hud = bounds.reduced(6.0f);
        auto hudW = juce::jlimit(150.0f, bounds.getWidth() - 8.0f, bounds.getWidth() * 0.56f);
        auto hudH = juce::jlimit(22.0f, bounds.getHeight() - 8.0f, bounds.getHeight() * 0.45f);
        auto hudRect = juce::Rectangle<float>(hud.getRight() - hudW, hud.getY() + 2.0f, hudW, hudH);
        g.setColour(juce::Colour(0xff121212).withAlpha(0.72f));
        g.fillRoundedRectangle(hudRect, 3.0f);
        g.setColour(grainAccent.withAlpha(0.4f));
        g.drawRoundedRectangle(hudRect, 3.0f, 0.9f);

        auto textRect = hudRect.reduced(5.0f, 2.5f);
        g.setColour(kTextSecondary.withAlpha(0.95f));
        g.setFont(juce::Font(juce::FontOptions(8.4f, juce::Font::bold)));
        g.drawText(grainHudLineA, textRect.removeFromTop(8.8f), juce::Justification::left, false);
        g.setColour(kTextMuted.withAlpha(0.98f));
        g.setFont(juce::Font(juce::FontOptions(7.8f)));
        g.drawText(grainHudLineB, textRect.removeFromTop(8.5f), juce::Justification::left, false);

        auto bars = hudRect.removeFromBottom(5.0f).reduced(5.0f, 0.0f);
        auto drawHudBar = [&](float value, juce::Colour c)
        {
            const float clamped = juce::jlimit(0.0f, 1.0f, value);
            auto slot = bars.removeFromLeft((bars.getWidth() / 3.0f) - 1.0f);
            g.setColour(juce::Colours::black.withAlpha(0.3f));
            g.fillRoundedRectangle(slot, 1.4f);
            g.setColour(c.withAlpha(0.85f));
            g.fillRoundedRectangle(slot.withWidth(slot.getWidth() * clamped), 1.4f);
            bars.removeFromLeft(1.0f);
        };
        drawHudBar(grainHudDensity, waveformColor.withMultipliedBrightness(1.1f));
        drawHudBar(grainHudSpread, grainAccent.withMultipliedBrightness(1.05f));
        drawHudBar(grainHudEmitter, grainAccent.brighter(0.22f));
    }

    if (loopPitchOverlayEnabled)
    {
        juce::Font lineAFont(juce::FontOptions(9.4f, juce::Font::bold));
        juce::Font lineBFont(juce::FontOptions(7.8f));
        const auto measureTextWidth = [](const juce::Font& font, const juce::String& text) -> int
        {
            if (text.isEmpty())
                return 0;
            juce::GlyphArrangement glyphs;
            glyphs.addLineOfText(font, text, 0.0f, 0.0f);
            return juce::roundToInt(glyphs.getBoundingBox(0, glyphs.getNumGlyphs(), true).getWidth());
        };
        const int lineAWidth = measureTextWidth(lineAFont, loopPitchOverlayLineA);
        const int lineBWidth = measureTextWidth(lineBFont, loopPitchOverlayLineB);
        const int contentWidth = juce::jmax(lineAWidth, lineBWidth);
        const bool hasSecondLine = loopPitchOverlayLineB.isNotEmpty();
        const float badgeWidth = juce::jlimit(20.0f, bounds.getWidth() - 12.0f, static_cast<float>(contentWidth + 10));
        const float badgeHeight = hasSecondLine ? 20.0f : 12.0f;
        auto hudRect = juce::Rectangle<float>(bounds.getRight() - badgeWidth - 6.0f,
                                              bounds.getY() + 6.0f,
                                              badgeWidth,
                                              badgeHeight);
        g.setColour(juce::Colour(0xff101010).withAlpha(0.78f));
        g.fillRoundedRectangle(hudRect, 3.0f);
        g.setColour(waveformColor.withAlpha(0.54f));
        g.drawRoundedRectangle(hudRect, 3.0f, 0.9f);

        auto text = hudRect.reduced(4.0f, 2.0f);
        g.setColour(kTextSecondary.withAlpha(0.98f));
        g.setFont(lineAFont);
        if (hasSecondLine)
        {
            g.drawText(loopPitchOverlayLineA, text.removeFromTop(9), juce::Justification::centredRight, false);
            g.setColour(kTextMuted.withAlpha(0.98f));
            g.setFont(lineBFont);
            g.drawText(loopPitchOverlayLineB, text, juce::Justification::centredRight, false);
        }
        else
        {
            g.drawText(loopPitchOverlayLineA, text, juce::Justification::centredRight, false);
        }
    }
}

void WaveformDisplay::resized()
{
}

void WaveformDisplay::mouseDown(const juce::MouseEvent& e)
{
    if (!hasAudio || !loopInteractionEnabled)
        return;

    if (e.mods.isShiftDown())
    {
        if (onShiftSliceEdit != nullptr)
            onShiftSliceEdit(static_cast<double>(normalizedPositionFromX(static_cast<float>(e.position.x))));
        return;
    }

    if ((e.mods.isAltDown() || e.mods.isMiddleButtonDown()) && viewSpanNorm < 0.999f)
    {
        viewDragActive = true;
        viewDragStart = e.getPosition();
        viewDragStartNorm = viewStartNorm;
        setMouseCursor(juce::MouseCursor::DraggingHandCursor);
    }
}

void WaveformDisplay::mouseDrag(const juce::MouseEvent& e)
{
    if (!viewDragActive || !loopInteractionEnabled)
        return;

    const float width = static_cast<float>(juce::jmax(1, getWidth()));
    const float deltaNorm = static_cast<float>(e.getDistanceFromDragStartX()) / width * viewSpanNorm;
    viewStartNorm = juce::jlimit(0.0f, juce::jmax(0.0f, 1.0f - viewSpanNorm), viewDragStartNorm - deltaNorm);
    repaint();
}

void WaveformDisplay::mouseUp(const juce::MouseEvent& /*e*/)
{
    if (!viewDragActive)
        return;

    viewDragActive = false;
    setMouseCursor(juce::MouseCursor::NormalCursor);
}

void WaveformDisplay::mouseDoubleClick(const juce::MouseEvent& e)
{
    if (!loopInteractionEnabled)
        return;

    if (!e.mods.isShiftDown())
        resetView();
}

void WaveformDisplay::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (!hasAudio || !loopInteractionEnabled)
        return;

    constexpr float epsilon = 1.0e-5f;
    bool changed = false;
    float nextSpan = viewSpanNorm;
    float nextStart = viewStartNorm;

    if (std::abs(wheel.deltaY) > epsilon)
    {
        const float cursorNorm = normalizedPositionFromX(static_cast<float>(e.position.x));
        const float zoomFactor = (wheel.deltaY > 0.0f) ? 0.85f : 1.18f;
        nextSpan = juce::jlimit(1.0f / 64.0f, 1.0f, nextSpan * zoomFactor);
        const float cursorRatio = juce::jlimit(0.0f, 1.0f,
                                               (cursorNorm - nextStart) / juce::jmax(1.0e-6f, viewSpanNorm));
        nextStart = cursorNorm - (cursorRatio * nextSpan);
        nextStart = juce::jlimit(0.0f, juce::jmax(0.0f, 1.0f - nextSpan), nextStart);
        changed = true;
    }

    if (std::abs(wheel.deltaX) > epsilon && nextSpan < 0.999f)
    {
        const float panScale = juce::jmax(0.015f, nextSpan * 0.42f);
        nextStart = juce::jlimit(0.0f,
                                 juce::jmax(0.0f, 1.0f - nextSpan),
                                 nextStart - (wheel.deltaX * panScale));
        changed = true;
    }

    if (!changed)
        return;

    viewSpanNorm = nextSpan;
    viewStartNorm = nextStart;
    repaint();
}

void WaveformDisplay::setAudioBuffer(const juce::AudioBuffer<float>& buffer, double sampleRate)
{
    (void) sampleRate;
    hasAudio = buffer.getNumSamples() > 0;
    if (!hasAudio)
    {
        clear();
        return;
    }

    generateThumbnail(buffer);
    repaint();
}

void WaveformDisplay::generateThumbnail(const juce::AudioBuffer<float>& buffer)
{
    const int thumbnailSize = 2048;
    thumbnail.clear();
    thumbnail.resize(static_cast<size_t>(thumbnailSize), 0.0f);
    
    auto numSamples = buffer.getNumSamples();
    if (numSamples == 0) return;
    
    auto samplesPerPixel = juce::jmax(1, numSamples / thumbnailSize);
    
    for (int i = 0; i < thumbnailSize; ++i)
    {
        float maxVal = 0.0f;
        auto startSample = i * samplesPerPixel;
        auto endSample = juce::jmin((i + 1) * samplesPerPixel, numSamples);
        
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            auto* channelData = buffer.getReadPointer(ch);
            for (int s = startSample; s < endSample; ++s)
            {
                maxVal = juce::jmax(maxVal, std::abs(channelData[s]));
            }
        }
        
        thumbnail[static_cast<size_t>(i)] = maxVal;
    }
}

void WaveformDisplay::setPlaybackPosition(double normalizedPosition)
{
    // Validate input to prevent NaN/Inf
    if (std::isfinite(normalizedPosition))
        playbackPosition = juce::jlimit(0.0, 1.0, normalizedPosition);
    else
        playbackPosition = 0.0;
    
    repaint();
}

void WaveformDisplay::setGrainWindowOverlay(bool enabled, double windowNorm)
{
    grainWindowOverlayEnabled = enabled;
    grainWindowNorm = juce::jlimit(0.0, 1.0, std::isfinite(windowNorm) ? windowNorm : 0.0);
    repaint();
}

void WaveformDisplay::setGrainMarkerPositions(const std::array<float, 8>& positions,
                                              const std::array<float, 8>& pitchNorms)
{
    grainMarkerPositions = positions;
    grainMarkerPitchNorms = pitchNorms;
    repaint();
}

void WaveformDisplay::setGrainHudOverlay(bool enabled,
                                         const juce::String& lineA,
                                         const juce::String& lineB,
                                         float density,
                                         float spread,
                                         float emitter,
                                         float pitchSemitones,
                                         float arpDepth,
                                         float pitchJitterSemitones)
{
    grainHudOverlayEnabled = enabled;
    grainHudLineA = lineA;
    grainHudLineB = lineB;
    grainHudDensity = juce::jlimit(0.0f, 1.0f, density);
    grainHudSpread = juce::jlimit(0.0f, 1.0f, spread);
    grainHudEmitter = juce::jlimit(0.0f, 1.0f, emitter);
    grainHudPitchSemitones = juce::jlimit(-48.0f, 48.0f, pitchSemitones);
    grainHudArpDepth = juce::jlimit(0.0f, 1.0f, arpDepth);
    grainHudPitchJitterSemitones = juce::jlimit(0.0f, 48.0f, pitchJitterSemitones);
    repaint();
}

void WaveformDisplay::setLoopPoints(int startCol, int endCol, int cols)
{
    loopStart = startCol;
    loopEnd = endCol;
    maxColumns = cols;
    repaint();
}

void WaveformDisplay::setSliceMarkers(const std::array<int, 16>& normalSlices,
                                      const std::array<int, 16>& transientSlices,
                                      int totalSamples,
                                      bool transientModeActive)
{
    normalSliceSamples = normalSlices;
    transientSliceSamples = transientSlices;
    waveformTotalSamples = juce::jmax(0, totalSamples);
    transientSlicesActive = transientModeActive;
    repaint();
}

void WaveformDisplay::setLoopPitchOverlay(bool enabled, const juce::String& lineA, const juce::String& lineB)
{
    loopPitchOverlayEnabled = enabled && (lineA.isNotEmpty() || lineB.isNotEmpty());
    loopPitchOverlayLineA = lineA;
    loopPitchOverlayLineB = lineB;
    repaint();
}

void WaveformDisplay::setLoopInteractionEnabled(bool enabled)
{
    if (loopInteractionEnabled == enabled)
        return;

    loopInteractionEnabled = enabled;
    if (!loopInteractionEnabled)
    {
        viewDragActive = false;
        resetView();
    }
}

void WaveformDisplay::clear()
{
    hasAudio = false;
    thumbnail.clear();
    playbackPosition = 0.0;
    waveformTotalSamples = 0;
    normalSliceSamples.fill(0);
    transientSliceSamples.fill(0);
    grainWindowOverlayEnabled = false;
    grainWindowNorm = 0.0;
    grainMarkerPositions.fill(-1.0f);
    grainMarkerPitchNorms.fill(0.0f);
    grainHudOverlayEnabled = false;
    grainHudLineA.clear();
    grainHudLineB.clear();
    grainHudDensity = 0.0f;
    grainHudSpread = 0.0f;
    grainHudEmitter = 0.0f;
    grainHudPitchSemitones = 0.0f;
    grainHudArpDepth = 0.0f;
    grainHudPitchJitterSemitones = 0.0f;
    loopPitchOverlayEnabled = false;
    loopPitchOverlayLineA.clear();
    loopPitchOverlayLineB.clear();
    viewDragActive = false;
    viewStartNorm = 0.0f;
    viewSpanNorm = 1.0f;
    repaint();
}

void WaveformDisplay::setWaveformColor(juce::Colour color)
{
    waveformColor = color;
    repaint();
}

void WaveformDisplay::setVisualGainDb(float trimDb)
{
    const float newGain = juce::Decibels::decibelsToGain(juce::jlimit(-36.0f, 36.0f, trimDb));
    if (std::abs(visualGain - newGain) <= 1.0e-6f)
        return;
    visualGain = newGain;
    repaint();
}

//==============================================================================
// LevelMeter Implementation
//==============================================================================

LevelMeter::LevelMeter()
{
    setOpaque(false);
}

void LevelMeter::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();
    
    // Background
    g.setColour(kSurfaceDark);
    g.fillRoundedRectangle(bounds, 2.0f);
    
    // Border
    g.setColour(kPanelStroke);
    g.drawRoundedRectangle(bounds, 2.0f, 1.0f);
    
    // Level bar
    if (currentLevel > 0.0f)
    {
        float barHeight = bounds.getHeight() * currentLevel;
        auto barBounds = bounds.removeFromBottom(barHeight).reduced(2.0f);
        
        // Color based on level (green -> yellow -> red)
        juce::Colour barColor;
        if (currentLevel < 0.7f)
            barColor = juce::Colour(0xff6eb676);
        else if (currentLevel < 0.9f)
            barColor = juce::Colour(0xffd3b35c);
        else
            barColor = juce::Colour(0xffd46b62);
        
        g.setColour(barColor);
        g.fillRoundedRectangle(barBounds, 1.0f);
    }
    
    // Peak indicator (small line at peak level)
    if (peakLevel > 0.0f)
    {
        float peakY = bounds.getBottom() - (bounds.getHeight() * peakLevel);
        g.setColour(kTextPrimary);
        g.drawLine(bounds.getX() + 2, peakY, bounds.getRight() - 2, peakY, 1.0f);
    }
}

void LevelMeter::setLevel(float level)
{
    currentLevel = juce::jlimit(0.0f, 1.0f, level);
    
    // Update peak with decay
    if (currentLevel > peakLevel)
        peakLevel = currentLevel;
    else
        peakLevel *= 0.95f;  // Slow decay
    
    repaint();
}

void LevelMeter::setPeak(float peak)
{
    peakLevel = juce::jlimit(0.0f, 1.0f, peak);
    repaint();
}

//==============================================================================
// StripControl Implementation
//==============================================================================

//==============================================================================
// StripControl - Compact horizontal layout with LED overlay
//==============================================================================

StripControl::StripControl(int idx, MlrVSTAudioProcessor& p)
    : stripIndex(idx), processor(p), waveform()
{
    setupComponents();
    startTimer(30);
}

void StripControl::setupComponents()
{
    // Track palette uses muted tones closer to Ableton's default session colors.
    const juce::Colour trackColors[] = {
        juce::Colour(0xffd36f63),
        juce::Colour(0xffd18f4f),
        juce::Colour(0xffbda659),
        juce::Colour(0xff6faa6f),
        juce::Colour(0xff5ea5a8),
        juce::Colour(0xff6f93c8),
        juce::Colour(0xff9a82bc)
    };

    stripColor = trackColors[juce::jmax(0, stripIndex) % 7];
    
    // Setup colored knob look and feel
    knobLookAndFeel.setKnobColor(stripColor);
    
    // Strip label with colored text
    stripLabel.setText("S" + juce::String(stripIndex + 1), juce::dontSendNotification);
    stripLabel.setFont(juce::Font(juce::FontOptions(11.0f, juce::Font::bold)));
    stripLabel.setJustificationType(juce::Justification::centredLeft);
    stripLabel.setColour(juce::Label::textColourId, stripColor);
    addAndMakeVisible(stripLabel);

    stripSampleNameLabel.setFont(juce::Font(juce::FontOptions(8.4f)));
    stripSampleNameLabel.setJustificationType(juce::Justification::centredLeft);
    stripSampleNameLabel.setColour(juce::Label::textColourId, kTextMuted);
    stripSampleNameLabel.setInterceptsMouseClicks(false, false);
    stripSampleNameLabel.setTooltip("Click to load a sample.");
    addAndMakeVisible(stripSampleNameLabel);

    trimSlider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    trimSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    trimSlider.setRange(-36.0, 36.0, 0.1);
    trimSlider.setValue(0.0, juce::dontSendNotification);
    trimSlider.setNumDecimalPlacesToDisplay(1);
    trimSlider.setPopupDisplayEnabled(false, false, this);
    trimSlider.setDoubleClickReturnValue(true, 0.0);
    trimSlider.setScrollWheelEnabled(false);
    trimSlider.setVelocityBasedMode(false);
    trimSlider.setMouseDragSensitivity(210);
    trimSlider.setColour(juce::Slider::backgroundColourId, juce::Colour(0xff2a2f34));
    trimSlider.setColour(juce::Slider::trackColourId, stripColor.withAlpha(0.9f));
    trimSlider.setColour(juce::Slider::textBoxBackgroundColourId, juce::Colour(0xff24282c));
    trimSlider.setColour(juce::Slider::textBoxOutlineColourId, juce::Colour(0xff586068));
    trimSlider.setColour(juce::Slider::textBoxTextColourId, kTextPrimary);
    trimSlider.textFromValueFunction = [](double value)
    {
        const double clamped = juce::jlimit(-36.0, 36.0, value);
        return juce::String(clamped >= 0.0 ? "+" : "") + juce::String(clamped, 1);
    };
    trimSlider.valueFromTextFunction = [](const juce::String& text)
    {
        return juce::jlimit(-36.0, 36.0, text.getDoubleValue());
    };
    trimSlider.setTooltip("Pre-FX trim for this strip in dB. Drag up/down to adjust. Alt/double-click resets to 0.0 dB.");
    trimSlider.onValueChange = [this]()
    {
        waveform.setVisualGainDb(static_cast<float>(trimSlider.getValue()));
    };
    addAndMakeVisible(trimSlider);
    trimAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        processor.parameters, "stripTrimDb" + juce::String(stripIndex), trimSlider);
    waveform.setVisualGainDb(static_cast<float>(trimSlider.getValue()));
    
    // Waveform display with rainbow color
    waveform.setWaveformColor(stripColor.withMultipliedSaturation(1.35f).withMultipliedBrightness(1.25f));
    waveform.onShiftSliceEdit = [this](double normalizedPosition)
    {
        handleWaveformShiftSliceEdit(normalizedPosition);
    };
    addAndMakeVisible(waveform);

    sampleModeComponent.setEngine(nullptr);
    sampleModeComponent.setWaveformColour(stripColor);
    sampleModeComponent.onTriggerVisibleSlice = [this](int visibleSlot)
    {
        processor.triggerStrip(stripIndex, visibleSlot);
    };
    sampleModeComponent.onNavigateVisibleBank = [this](int delta)
    {
        if (auto* engine = processor.getSampleModeEngine(stripIndex, false))
            engine->stepVisibleBank(delta);
    };
    sampleModeComponent.onRequestLoad = [this]()
    {
        loadSample();
    };
    sampleModeComponent.onCopyToLoop = [this]()
    {
        juce::PopupMenu menu;
        for (int targetStrip = 0; targetStrip < MlrVSTAudioProcessor::MaxStrips; ++targetStrip)
        {
            const bool sameStrip = (targetStrip == stripIndex);
            menu.addItem(targetStrip + 1,
                         sameStrip
                             ? ("S" + juce::String(targetStrip + 1) + " (This)")
                             : ("S" + juce::String(targetStrip + 1)));
        }

        juce::Component::SafePointer<StripControl> safeThis(this);
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&sampleModeComponent),
                           [safeThis](int result)
                           {
                               if (safeThis == nullptr || result <= 0)
                                   return;

                               const int targetStrip = result - 1;
                               if (safeThis->processor.copyFlipCurrentSlicesToMode(safeThis->stripIndex,
                                                                                   targetStrip,
                                                                                   EnhancedAudioStrip::PlayMode::Loop)
                                   && targetStrip == safeThis->stripIndex)
                               {
                                   safeThis->playModeBox.setSelectedId(static_cast<int>(EnhancedAudioStrip::PlayMode::Loop) + 1,
                                                                       juce::sendNotification);
                               }
                           });
    };
    sampleModeComponent.onCopyToGrain = [this]()
    {
        if (processor.copyFlipCurrentSlicesToMode(stripIndex, EnhancedAudioStrip::PlayMode::Grain))
            playModeBox.setSelectedId(static_cast<int>(EnhancedAudioStrip::PlayMode::Grain) + 1, juce::sendNotification);
    };
    sampleModeComponent.onRequestLegacyLoopBarsMenu = [this]()
    {
        auto* sampleEngine = processor.getSampleModeEngine(stripIndex, false);
        if (sampleEngine == nullptr)
            return;

        juce::PopupMenu menu;
        const int currentSelection = sampleEngine->getLegacyLoopBarSelection();
        const struct Item { int id; const char* label; int selection; } items[] = {
            { 1, "Auto", 0 },
            { 2, "1/4 Bar", 25 },
            { 3, "1/2 Bar", 50 },
            { 4, "1 Bar", 100 },
            { 5, "2 Bars", 200 },
            { 6, "4 Bars", 400 },
            { 7, "8 Bars", 800 }
        };

        for (const auto& item : items)
            menu.addItem(item.id, item.label, true, currentSelection == item.selection);

        juce::Component::SafePointer<StripControl> safeThis(this);
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&sampleModeComponent),
                           [safeThis](int result)
                           {
                               if (safeThis == nullptr || result <= 0)
                                   return;

                               auto* engine = safeThis->processor.getSampleModeEngine(safeThis->stripIndex, false);
                               if (engine == nullptr)
                                   return;

                               switch (result)
                               {
                                   case 2: engine->setLegacyLoopBarSelection(25); break;
                                   case 3: engine->setLegacyLoopBarSelection(50); break;
                                   case 4: engine->setLegacyLoopBarSelection(100); break;
                                   case 5: engine->setLegacyLoopBarSelection(200); break;
                                   case 6: engine->setLegacyLoopBarSelection(400); break;
                                   case 7: engine->setLegacyLoopBarSelection(800); break;
                                   case 1:
                                   default:
                                       engine->setLegacyLoopBarSelection(0);
                                       break;
                               }
                           });
    };
    addChildComponent(sampleModeComponent);
    
    // Step sequencer display with matching rainbow color
    stepDisplay.setStripColor(stripColor);
    stepDisplay.onStepSet = [this](int stepIndex, bool enabled)
    {
        if (auto* engine = processor.getAudioEngine())
        {
            if (auto* strip = engine->getStrip(stripIndex))
            {
                const int totalSteps = strip->getStepTotalSteps();
                if (stepIndex >= 0 && stepIndex < totalSteps)
                    strip->setStepEnabledAtIndex(stepIndex, enabled, true);
            }
        }
    };
    stepDisplay.onStepSubdivisionSet = [this](int stepIndex, int subdivisions)
    {
        if (auto* engine = processor.getAudioEngine())
        {
            if (auto* strip = engine->getStrip(stripIndex))
                strip->setStepSubdivisionAtIndex(stepIndex, subdivisions);
        }
    };
    stepDisplay.onStepVelocityRangeSet = [this](int stepIndex, float startVelocity, float endVelocity)
    {
        if (auto* engine = processor.getAudioEngine())
        {
            if (auto* strip = engine->getStrip(stripIndex))
                strip->setStepSubdivisionVelocityRangeAtIndex(stepIndex, startVelocity, endVelocity);
        }
    };
    stepDisplay.onStepProbabilitySet = [this](int stepIndex, float probability)
    {
        if (auto* engine = processor.getAudioEngine())
        {
            if (auto* strip = engine->getStrip(stripIndex))
                strip->setStepProbabilityAtIndex(stepIndex, probability);
        }
    };
    addChildComponent(stepDisplay);  // Hidden initially
    
    // Load button - compact
    loadButton.setButtonText("Load");
    loadButton.onClick = [this]() { loadSample(); };
    loadButton.setTooltip("Replace the strip source. Preserve timing safely. Rerun pitch analysis only when the current pitch role requires it.");
    addAndMakeVisible(loadButton);

    pitchMasterButton.setButtonText("PM");
    pitchMasterButton.onClick = [this]()
    {
        const auto currentRole = processor.getLoopPitchRole(stripIndex);
        processor.setLoopPitchRole(stripIndex,
                                   currentRole == MlrVSTAudioProcessor::LoopPitchRole::Master
                                       ? MlrVSTAudioProcessor::LoopPitchRole::None
                                       : MlrVSTAudioProcessor::LoopPitchRole::Master);
    };
    pitchMasterButton.setTooltip("Toggle Pitch Master for this strip. When active, analysis sets the global root note.");
    styleUiButton(pitchMasterButton);
    addAndMakeVisible(pitchMasterButton);

    pitchSyncButton.setButtonText("PS");
    pitchSyncButton.onClick = [this]()
    {
        if (juce::ModifierKeys::getCurrentModifiersRealtime().isAltDown())
        {
            const auto currentTiming = processor.getLoopPitchSyncTiming(stripIndex);
            const int nextTiming = (static_cast<int>(currentTiming) + 1)
                % (static_cast<int>(MlrVSTAudioProcessor::LoopPitchSyncTiming::NextBar) + 1);
            processor.setLoopPitchSyncTiming(stripIndex,
                                             static_cast<MlrVSTAudioProcessor::LoopPitchSyncTiming>(nextTiming));
            return;
        }

        const auto currentRole = processor.getLoopPitchRole(stripIndex);
        processor.setLoopPitchRole(stripIndex,
                                   currentRole == MlrVSTAudioProcessor::LoopPitchRole::Sync
                                       ? MlrVSTAudioProcessor::LoopPitchRole::None
                                       : MlrVSTAudioProcessor::LoopPitchRole::Sync);
    };
    pitchSyncButton.setTooltip("Toggle Pitch Sync for this strip. Option-click cycles retune timing.");
    styleUiButton(pitchSyncButton);
    addAndMakeVisible(pitchSyncButton);

    pitchNoteBox.setJustificationType(juce::Justification::centredLeft);
    pitchNoteBox.setTextWhenNothingSelected("Note");
    for (int pitchClass = 0; pitchClass < 12; ++pitchClass)
        pitchNoteBox.addItem(getPitchClassName(pitchClass), pitchClass + 1);
    pitchNoteBox.onChange = [this]()
    {
        const int selectedPitchClass = pitchNoteBox.getSelectedId() - 1;
        if (selectedPitchClass >= 0)
        {
            const int currentAssignedMidi = processor.getLoopStripAssignedPitchMidi(stripIndex);
            const int referenceMidi = currentAssignedMidi >= 0
                ? currentAssignedMidi
                : processor.getGlobalRootNoteMidi();
            const int selectedMidi = getNearestMidiForPitchClass(referenceMidi, selectedPitchClass);
            processor.setLoopStripAssignedPitchMidi(stripIndex, selectedMidi);
            if (processor.getLoopPitchRole(stripIndex) == MlrVSTAudioProcessor::LoopPitchRole::Master)
                processor.updateGlobalRootFromLoopPitchMaster(stripIndex, true);
        }
    };
    pitchNoteBox.setTooltip("Detected or manual source note for this strip. PM sets the global root note. PS retunes from this note to the global root.");
    styleUiCombo(pitchNoteBox);
    addAndMakeVisible(pitchNoteBox);

    identityModeButton.onClick = [this]()
    {
        juce::PopupMenu menu;
        menu.addItem(1, "One-Shot", true, playModeBox.getSelectedId() == 1);
        menu.addItem(2, "Loop", true, playModeBox.getSelectedId() == 2);
        menu.addItem(3, "Gate", true, playModeBox.getSelectedId() == 3);
        menu.addItem(4, "Step", true, playModeBox.getSelectedId() == 4);
        menu.addItem(5, "Grain", true, playModeBox.getSelectedId() == 5);
        menu.addItem(6, "Flip", true, playModeBox.getSelectedId() == 6);

        juce::Component::SafePointer<StripControl> safeThis(this);
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&identityModeButton),
                           [safeThis](int result)
                           {
                               if (safeThis == nullptr || result <= 0)
                                   return;
                               safeThis->playModeBox.setSelectedId(result, juce::sendNotification);
                           });
    };
    identityModeButton.setTooltip("Playback mode for this strip.");
    styleUiButton(identityModeButton);
    addAndMakeVisible(identityModeButton);

    identityGroupButton.onClick = [this]()
    {
        juce::PopupMenu menu;
        const int currentGroupId = groupSelector.getSelectedId();
        menu.addItem(1, "None", true, currentGroupId == 1);
        menu.addItem(2, "G1", true, currentGroupId == 2);
        menu.addItem(3, "G2", true, currentGroupId == 3);
        menu.addItem(4, "G3", true, currentGroupId == 4);
        menu.addItem(5, "G4", true, currentGroupId == 5);

        juce::Component::SafePointer<StripControl> safeThis(this);
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&identityGroupButton),
                           [safeThis](int result)
                           {
                               if (safeThis == nullptr || result <= 0)
                                   return;
                               safeThis->groupSelector.setSelectedId(result, juce::sendNotification);
                           });
    };
    identityGroupButton.setTooltip("Mute group assignment for this strip.");
    styleUiButton(identityGroupButton);
    addAndMakeVisible(identityGroupButton);

    identityRoleButton.onClick = [this]()
    {
        juce::PopupMenu menu;
        const auto currentRole = processor.getLoopPitchRole(stripIndex);
        menu.addItem(1, "Free", true, currentRole == MlrVSTAudioProcessor::LoopPitchRole::None);
        menu.addItem(2, "PM", true, currentRole == MlrVSTAudioProcessor::LoopPitchRole::Master);
        menu.addItem(3, "PS", true, currentRole == MlrVSTAudioProcessor::LoopPitchRole::Sync);

        juce::Component::SafePointer<StripControl> safeThis(this);
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&identityRoleButton),
                           [safeThis](int result)
                           {
                               if (safeThis == nullptr || result <= 0)
                                   return;

                               MlrVSTAudioProcessor::LoopPitchRole role = MlrVSTAudioProcessor::LoopPitchRole::None;
                               if (result == 2)
                                   role = MlrVSTAudioProcessor::LoopPitchRole::Master;
                               else if (result == 3)
                                   role = MlrVSTAudioProcessor::LoopPitchRole::Sync;
                               safeThis->processor.setLoopPitchRole(safeThis->stripIndex, role);
                           });
    };
    identityRoleButton.setTooltip("Pitch role for this strip. PM sets the global tonal center. PS follows the global tonal center. Free ignores it.");
    styleUiButton(identityRoleButton);
    addAndMakeVisible(identityRoleButton);

    identityNoteButton.onClick = [this]()
    {
        juce::PopupMenu menu;
        const int assignedPitchMidi = processor.getLoopStripAssignedPitchMidi(stripIndex);
        const int assignedPitchClass = assignedPitchMidi >= 0 ? ((assignedPitchMidi % 12) + 12) % 12 : -1;
        for (int pitchClass = 0; pitchClass < 12; ++pitchClass)
            menu.addItem(100 + pitchClass, getPitchClassName(pitchClass), true, pitchClass == assignedPitchClass);

        juce::Component::SafePointer<StripControl> safeThis(this);
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&identityNoteButton),
                           [safeThis](int result)
                           {
                               if (safeThis == nullptr || result < 100 || result >= 112)
                                   return;

                               const int selectedPitchClass = result - 100;
                               const int currentAssignedMidi = safeThis->processor.getLoopStripAssignedPitchMidi(safeThis->stripIndex);
                               const int referenceMidi = currentAssignedMidi >= 0
                                   ? currentAssignedMidi
                                   : safeThis->processor.getGlobalRootNoteMidi();
                               const int selectedMidi = getNearestMidiForPitchClass(referenceMidi, selectedPitchClass);
                               safeThis->processor.setLoopStripAssignedPitchMidi(safeThis->stripIndex, selectedMidi);
                               if (safeThis->processor.getLoopPitchRole(safeThis->stripIndex) == MlrVSTAudioProcessor::LoopPitchRole::Master)
                                   safeThis->processor.updateGlobalRootFromLoopPitchMaster(safeThis->stripIndex, true);
                           });
    };
    identityNoteButton.setTooltip("Source note of this material. This is metadata for the strip source, not the global target root.");
    styleUiButton(identityNoteButton);
    addAndMakeVisible(identityNoteButton);

    identityTargetButton.onClick = [this]()
    {
        juce::PopupMenu menu;
        const int currentModeId = directionModeBox.getSelectedId();
        menu.addItem(1, "Normal", true, currentModeId == 1);
        menu.addItem(2, "Reverse", true, currentModeId == 2);
        menu.addItem(3, "Ping-Pong", true, currentModeId == 3);
        menu.addItem(4, "Random", true, currentModeId == 4);
        menu.addItem(5, "Random Walk", true, currentModeId == 5);
        menu.addItem(6, "Random Slice", true, currentModeId == 6);

        juce::Component::SafePointer<StripControl> safeThis(this);
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&identityTargetButton),
                           [safeThis](int result)
                           {
                               if (safeThis == nullptr || result <= 0)
                                   return;
                               safeThis->directionModeBox.setSelectedId(result, juce::sendNotification);
                           });
    };
    identityTargetButton.setTooltip("Playback direction behavior for this strip.");
    styleUiButton(identityTargetButton);
    addAndMakeVisible(identityTargetButton);

    identityTimingButton.onClick = [this]()
    {
        juce::PopupMenu menu;
        const auto currentTiming = processor.getLoopPitchSyncTiming(stripIndex);
        for (int timing = 0; timing <= static_cast<int>(MlrVSTAudioProcessor::LoopPitchSyncTiming::NextBar); ++timing)
        {
            const auto value = static_cast<MlrVSTAudioProcessor::LoopPitchSyncTiming>(timing);
            menu.addItem(100 + timing,
                         loopPitchSyncTimingDisplayName(value, false),
                         true,
                         value == currentTiming);
        }

        juce::Component::SafePointer<StripControl> safeThis(this);
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&identityTimingButton),
                           [safeThis](int result)
                           {
                               if (safeThis == nullptr || result < 100)
                                   return;

                               safeThis->processor.setLoopPitchSyncTiming(
                                   safeThis->stripIndex,
                                   static_cast<MlrVSTAudioProcessor::LoopPitchSyncTiming>(result - 100));
                           });
    };
    identityTimingButton.setTooltip("Pitch retune timing for PS strips.");
    styleUiButton(identityTimingButton);
    addAndMakeVisible(identityTimingButton);

    transientSliceButton.setButtonText("TIME");
    transientSliceButton.setClickingTogglesState(true);
    transientSliceButton.setTooltip("Toggle slice mapping: Time = 16 equal slices, Transient = onset-based slices");
    transientSliceButton.onClick = [this]()
    {
        if (auto* engine = processor.getAudioEngine())
        {
            if (auto* strip = engine->getStrip(stripIndex))
                strip->setTransientSliceMode(transientSliceButton.getToggleState());
        }
    };
    addAndMakeVisible(transientSliceButton);

    // Play mode selector - compact (removed Reverse and Ping-Pong)
    playModeBox.addItem("One-Shot", 1);
    playModeBox.addItem("Loop", 2);
    playModeBox.addItem("Gate", 3);
    playModeBox.addItem("Step", 4);  // Step sequencer mode
    playModeBox.addItem("Grain", 5);
    playModeBox.addItem("Flip", 6);
    playModeBox.setJustificationType(juce::Justification::centredLeft);
    playModeBox.setSelectedId(2);  // Default Loop
    playModeBox.setTooltip("Playback mode for this strip.");
    playModeBox.onChange = [this]()
    {
        if (!processor.getAudioEngine()) return;
        if (auto* strip = processor.getAudioEngine()->getStrip(stripIndex))
        {
            int modeId = playModeBox.getSelectedId() - 1;
            const auto playMode = static_cast<EnhancedAudioStrip::PlayMode>(modeId);
            strip->setPlayMode(playMode);
            
            // Switch between waveform and step display
            const bool isStepMode = (playMode == EnhancedAudioStrip::PlayMode::Step);
            const bool isSampleMode = (playMode == EnhancedAudioStrip::PlayMode::Sample);
            showingStepDisplay = isStepMode;
            showingSampleMode = isSampleMode;

            if (isSampleMode)
                sampleModeComponent.setEngine(processor.getSampleModeEngine(stripIndex, true));
            else
                sampleModeComponent.setEngine(processor.getSampleModeEngine(stripIndex, false));
            
            waveform.setVisible(!isStepMode && !isSampleMode);
            stepDisplay.setVisible(isStepMode);
            sampleModeComponent.setVisible(isSampleMode);
            patternLengthBox.setVisible(isStepMode);
            updateGrainOverlayVisibility();
            
            // Don't manually start - let process() auto-start when DAW plays
            // This respects the host transport state (paused or playing)
            
            resized();  // Re-layout components
            
            DBG("Strip " << stripIndex << " mode changed to " << modeId);
        }
    };
    addAndMakeVisible(playModeBox);
    
    // Direction mode selector - NEW!
    directionModeBox.addItem("Normal", 1);
    directionModeBox.addItem("Reverse", 2);
    directionModeBox.addItem("Ping-Pong", 3);
    directionModeBox.addItem("Random", 4);
    directionModeBox.addItem("Rnd Walk", 5);
    directionModeBox.addItem("Rnd Slice", 6);
    directionModeBox.setJustificationType(juce::Justification::centredLeft);
    directionModeBox.setSelectedId(1);  // Default Normal
    directionModeBox.setTooltip("Playback direction behavior.");
    directionModeBox.onChange = [this]()
    {
        if (!processor.getAudioEngine()) return;
        if (auto* strip = processor.getAudioEngine()->getStrip(stripIndex))
        {
            int modeId = directionModeBox.getSelectedId() - 1;
            strip->setDirectionMode(static_cast<EnhancedAudioStrip::DirectionMode>(modeId));
            
            DBG("Strip " << stripIndex << " direction changed to " << modeId);
        }
    };
    addAndMakeVisible(directionModeBox);
    addAndMakeVisible(playModeBox);
    
    // Group selector - compact
    groupSelector.addItem("None", 1);
    groupSelector.addItem("G1", 2);
    groupSelector.addItem("G2", 3);
    groupSelector.addItem("G3", 4);
    groupSelector.addItem("G4", 5);
    groupSelector.setJustificationType(juce::Justification::centredLeft);
    groupSelector.setSelectedId(1);
    groupSelector.setTooltip("Assign strip to mute group.");
    groupSelector.onChange = [this]()
    {
        if (!processor.getAudioEngine()) return;
        
        // Get group ID: None=1, G1=2, G2=3, G3=4, G4=5
        // Convert to: None=-1, G1=0, G2=1, G3=2, G4=3
        int groupId = groupSelector.getSelectedId() - 2;
        
        // assignStripToGroup handles everything (removal from old, add to new, setGroup)
        processor.getAudioEngine()->assignStripToGroup(stripIndex, groupId);
    };
    addAndMakeVisible(groupSelector);
    
    // Compact rotary controls with colored look
    volumeSlider.setLookAndFeel(&knobLookAndFeel);
    volumeSlider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    volumeSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    volumeSlider.setRange(0.0, 1.0, 0.01);
    volumeSlider.setValue(1.0);
    enableAltClickReset(volumeSlider, 1.0);
    volumeSlider.setPopupDisplayEnabled(true, false, this);
    addAndMakeVisible(volumeSlider);
    
    volumeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        processor.parameters, "stripVolume" + juce::String(stripIndex), volumeSlider);
    
    panSlider.setLookAndFeel(&knobLookAndFeel);
    panSlider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    panSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    panSlider.setRange(-1.0, 1.0, 0.01);
    panSlider.setValue(0.0);
    enableAltClickReset(panSlider, 0.0);
    panSlider.setPopupDisplayEnabled(true, false, this);
    addAndMakeVisible(panSlider);
    
    panAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        processor.parameters, "stripPan" + juce::String(stripIndex), panSlider);

    pitchSlider.setLookAndFeel(&knobLookAndFeel);
    pitchSlider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    pitchSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    pitchSlider.setRange(-24.0, 24.0, 0.01);
    pitchSlider.setValue(0.0);
    enableAltClickReset(pitchSlider, 0.0);
    pitchSlider.setPopupDisplayEnabled(true, false, this);
    pitchSlider.setTextValueSuffix(" st");
    pitchSlider.setTooltip("Pitch offset in semitones.");
    addAndMakeVisible(pitchSlider);

    pitchAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        processor.parameters, "stripPitch" + juce::String(stripIndex), pitchSlider);
    pitchSlider.onValueChange = [this]()
    {
        processor.applyUserPitchControlToStrip(stripIndex,
                                               static_cast<float>(pitchSlider.getValue()));
    };

    speedSlider.setLookAndFeel(&knobLookAndFeel);
    speedSlider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    speedSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    speedSlider.setRange(0.0, 8.0, 0.001);
    speedSlider.setValue(1.0);
    enableAltClickReset(speedSlider, 1.0);
    speedSlider.textFromValueFunction = [this](double value)
    {
        auto* strip = processor.getAudioEngine() != nullptr ? processor.getAudioEngine()->getStrip(stripIndex) : nullptr;
        if (strip != nullptr && strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Grain)
            return getGrainSpeedLabel(static_cast<float>(value));

        return getPlayheadSpeedLabel(PlayheadSpeedQuantizer::quantizeRatio(static_cast<float>(value)));
    };
    speedSlider.setPopupDisplayEnabled(true, false, this);
    speedSlider.setTooltip("Speed. In Grain mode this changes playback position speed from 0 to 2; otherwise it changes slice traversal speed.");
    speedSlider.onValueChange = [this]()
    {
        if (auto* strip = processor.getAudioEngine()->getStrip(stripIndex))
        {
            if (strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Grain)
            {
                processor.setStripSpeedControlValue(stripIndex,
                                                    static_cast<float>(speedSlider.getValue()),
                                                    MlrVSTAudioProcessor::StripControlWriteMode::CacheOnly);
            }
            else
            {
                const float quantizedRatio = PlayheadSpeedQuantizer::quantizeRatio(static_cast<float>(speedSlider.getValue()));
                if (std::abs(quantizedRatio - static_cast<float>(speedSlider.getValue())) > 1.0e-4f)
                {
                    speedSlider.setValue(quantizedRatio, juce::sendNotificationSync);
                    return;
                }
                processor.setStripSpeedControlValue(stripIndex,
                                                    quantizedRatio,
                                                    MlrVSTAudioProcessor::StripControlWriteMode::CacheOnly);
            }
        }
    };
    addAndMakeVisible(speedSlider);
    
    speedAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        processor.parameters, "stripSpeed" + juce::String(stripIndex), speedSlider);
    
    // Scratch amount
    scratchSlider.setLookAndFeel(&knobLookAndFeel);
    scratchSlider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    scratchSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    scratchSlider.setRange(0.0, 100.0, 1.0);
    scratchSlider.setValue(0.0);
    enableAltClickReset(scratchSlider, 0.0);
    scratchSlider.textFromValueFunction = [this](double value)
    {
        const double clamped = juce::jlimit(0.0, 100.0, value);
        if (clamped <= 0.0001)
            return juce::String("0.00 s");

        double seconds = 0.0;
        if (auto* strip = processor.getAudioEngine()->getStrip(stripIndex);
            strip != nullptr && strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Grain)
        {
            const double t = clamped / 100.0;
            seconds = juce::jlimit(0.015, 3.0, std::pow(t, 1.7) * 3.0);
        }
        else
        {
            double beats = 0.25;
            if (clamped <= 10.0)
            {
                const double t = clamped / 10.0;
                beats = 0.02 + (std::pow(t, 1.6) * 0.08);
            }
            else
            {
                const double t = (clamped - 10.0) / 90.0;
                beats = 0.10 + (std::pow(t, 1.8) * 7.90);
            }

            double tempo = 120.0;
            if (auto* engine = processor.getAudioEngine())
                tempo = juce::jmax(1.0, engine->getCurrentTempo());
            seconds = beats * (60.0 / tempo);
        }

        return juce::String(seconds, 2) + " s";
    };
    scratchSlider.setPopupDisplayEnabled(true, false, this);
    scratchSlider.setTooltip("Scratch amount. Higher values increase scratch gesture duration.");
    scratchSlider.onValueChange = [this]()
    {
        if (auto* strip = processor.getAudioEngine()->getStrip(stripIndex))
        {
            strip->setScratchAmount(static_cast<float>(scratchSlider.getValue()));
            processor.queueActiveSceneAutosave();
        }
    };
    addAndMakeVisible(scratchSlider);

    sliceLengthSlider.setLookAndFeel(&knobLookAndFeel);
    sliceLengthSlider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    sliceLengthSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    sliceLengthSlider.setRange(0.0, 1.0, 0.001);
    sliceLengthSlider.setValue(1.0);
    enableAltClickReset(sliceLengthSlider, 1.0);
    sliceLengthSlider.textFromValueFunction = [](double value)
    {
        const int percent = static_cast<int>(std::round(juce::jlimit(0.0, 1.0, value) * 100.0));
        return juce::String(percent) + "%";
    };
    sliceLengthSlider.setPopupDisplayEnabled(true, false, this);
    sliceLengthSlider.setTooltip("Loop segment length. 100% = full segment, lower values add click-free gaps.");
    addAndMakeVisible(sliceLengthSlider);
    sliceLengthAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        processor.parameters, "stripSliceLength" + juce::String(stripIndex), sliceLengthSlider);

    auto setupGrainKnob = [this](juce::Slider& slider, juce::Label& label, const char* text,
                                 double min, double max, double step)
    {
        slider.setLookAndFeel(&knobLookAndFeel);
        slider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        slider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        slider.setRange(min, max, step);
        enableAltClickReset(slider, juce::jlimit(min, max, 0.5 * (min + max)));
        slider.setPopupDisplayEnabled(true, false, this);
        addAndMakeVisible(slider);
        label.setText(text, juce::dontSendNotification);
        label.setFont(juce::Font(juce::FontOptions(9.2f, juce::Font::bold)));
        label.setJustificationType(juce::Justification::centred);
        label.setColour(juce::Label::textColourId, stripColor.brighter(0.35f));
        addAndMakeVisible(label);
    };

    setupGrainKnob(grainSizeSlider, grainSizeLabel, "SIZE", 5.0, 2400.0, 1.0);
    setupGrainKnob(grainDensitySlider, grainDensityLabel, "DENS", 0.05, 0.9, 0.01);
    setupGrainKnob(grainPitchSlider, grainPitchLabel, "PITCH", -48.0, 48.0, 0.1);
    grainPitchSlider.getProperties().set("bipolarBase", true);
    grainPitchLabel.setJustificationType(juce::Justification::centredLeft);
    setupGrainKnob(grainPitchJitterSlider, grainPitchJitterLabel, "PJIT", 0.0, 48.0, 0.1);
    setupGrainKnob(grainSpreadSlider, grainSpreadLabel, "SPRD", 0.0, 1.0, 0.01);
    setupGrainKnob(grainJitterSlider, grainJitterLabel, "SJTR", 0.0, 1.0, 0.01);
    setupGrainKnob(grainPositionJitterSlider, grainPositionJitterLabel, "POSJ", 0.0, 1.0, 0.01);
    setupGrainKnob(grainRandomSlider, grainRandomLabel, "RAND", 0.0, 1.0, 0.01);
    setupGrainKnob(grainArpSlider, grainArpLabel, "ARP", 0.0, 1.0, 0.01);
    setupGrainKnob(grainCloudSlider, grainCloudLabel, "CLOUD", 0.0, 1.0, 0.01);
    setupGrainKnob(grainEmitterSlider, grainEmitterLabel, "EMIT", 0.0, 1.0, 0.01);
    setupGrainKnob(grainEnvelopeSlider, grainEnvelopeLabel, "ENV", 0.0, 1.0, 0.01);
    setupGrainKnob(grainShapeSlider, grainShapeLabel, "SHAPE", -1.0, 1.0, 0.01);
    enableAltClickReset(grainSizeSlider, 1240.0);
    enableAltClickReset(grainDensitySlider, 0.05);
    enableAltClickReset(grainPitchSlider, 0.0);
    enableAltClickReset(grainPitchJitterSlider, 0.0);
    enableAltClickReset(grainSpreadSlider, 0.0);
    enableAltClickReset(grainJitterSlider, 0.0);
    enableAltClickReset(grainPositionJitterSlider, 0.0);
    enableAltClickReset(grainRandomSlider, 0.0);
    enableAltClickReset(grainArpSlider, 0.0);
    enableAltClickReset(grainCloudSlider, 0.0);
    enableAltClickReset(grainEmitterSlider, 0.0);
    enableAltClickReset(grainEnvelopeSlider, 0.0);
    enableAltClickReset(grainShapeSlider, 0.0);
    auto setupMini = [](juce::Slider& slider)
    {
        slider.setSliderStyle(juce::Slider::LinearHorizontal);
        slider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    };
    setupMini(grainSizeSlider);
    setupMini(grainDensitySlider);
    setupMini(grainPitchSlider);
    setupMini(grainPitchJitterSlider);
    setupMini(grainSpreadSlider);
    setupMini(grainJitterSlider);
    setupMini(grainPositionJitterSlider);
    setupMini(grainRandomSlider);
    setupMini(grainArpSlider);
    setupMini(grainCloudSlider);
    setupMini(grainEmitterSlider);
    setupMini(grainEnvelopeSlider);
    setupMini(grainShapeSlider);
    grainPitchSlider.textFromValueFunction = [this](double value)
    {
        const bool arpActive = grainArpSlider.getValue() > 0.001;
        const juce::String prefix = arpActive ? "Range " : "Pitch ";
        return prefix + juce::String(value, 1) + " st";
    };
    grainSizeSlider.textFromValueFunction = [this](double value)
    {
        static constexpr std::array<const char*, 13> sizeDivisionLabels {
            "1/64", "1/48", "1/32", "1/24", "1/16", "1/12", "1/8", "1/6", "1/4", "1/3", "1/2", "1", "2"
        };
        const bool syncEnabled = [this]()
        {
            if (auto* engine = processor.getAudioEngine())
                if (auto* strip = engine->getStrip(stripIndex))
                    return strip->isGrainTempoSyncEnabled();
            return grainSizeSyncToggle.getToggleState();
        }();

        if (!syncEnabled)
            return juce::String(value, 1) + " ms (FREE)";

        const double t = juce::jlimit(0.0, 1.0, (value - 5.0) / (2400.0 - 5.0));
        const int idx = juce::jlimit(0, static_cast<int>(sizeDivisionLabels.size()) - 1,
                                     static_cast<int>(std::round(t * static_cast<double>(sizeDivisionLabels.size() - 1))));
        return juce::String(sizeDivisionLabels[static_cast<size_t>(idx)]);
    };
    grainArpSlider.textFromValueFunction = [](double value)
    {
        if (value <= 0.001)
            return juce::String("Off");
        const int mode = juce::jlimit(0, 5, static_cast<int>(std::floor(juce::jlimit(0.0, 0.999999, value) * 6.0)));
        return getGrainArpModeName(mode);
    };
    grainJitterSlider.textFromValueFunction = [](double value)
    {
        const int percent = static_cast<int>(std::round(juce::jlimit(0.0, 1.0, value) * 100.0));
        return juce::String(percent) + "% size jitter";
    };
    grainPositionJitterSlider.textFromValueFunction = [this](double value)
    {
        const int percent = static_cast<int>(std::round(juce::jlimit(0.0, 1.0, value) * 100.0));
        bool syncEnabled = grainSizeSyncToggle.getToggleState();
        if (auto* engine = processor.getAudioEngine())
            if (auto* strip = engine->getStrip(stripIndex))
                syncEnabled = strip->isGrainTempoSyncEnabled();
        return juce::String(percent) + (syncEnabled ? "% pos jitter (sync)" : "% pos jitter");
    };
    grainRandomSlider.textFromValueFunction = [](double value)
    {
        const int percent = static_cast<int>(std::round(juce::jlimit(0.0, 1.0, value) * 100.0));
        return juce::String(percent) + "% macro rand";
    };
    grainEnvelopeSlider.textFromValueFunction = [](double value)
    {
        const int percent = static_cast<int>(std::round(juce::jlimit(0.0, 1.0, value) * 100.0));
        return juce::String(percent) + "% Fade";
    };
    grainShapeSlider.textFromValueFunction = [](double value)
    {
        const int percent = static_cast<int>(std::round(juce::jlimit(-1.0, 1.0, value) * 100.0));
        return juce::String(percent) + "% Shape";
    };
    grainPositionJitterSlider.setTooltip("POSJ: grain center position jitter. Quantized to sync grid when Grain SYNC is enabled.");
    grainRandomSlider.setTooltip("RAND: macro random depth (pitch, size, reverse, spray), independent of POSJ.");

    grainSizeSlider.onValueChange = [this]()
    {
        if (auto* strip = processor.getAudioEngine()->getStrip(stripIndex))
        {
            strip->setGrainSizeMs(static_cast<float>(grainSizeSlider.getValue()));
            processor.queueActiveSceneAutosave();
        }
    };
    grainDensitySlider.onValueChange = [this]()
    {
        if (auto* strip = processor.getAudioEngine()->getStrip(stripIndex))
        {
            strip->setGrainDensity(static_cast<float>(grainDensitySlider.getValue()));
            processor.queueActiveSceneAutosave();
        }
    };
    grainPitchSlider.onValueChange = [this]()
    {
        if (auto* strip = processor.getAudioEngine()->getStrip(stripIndex))
        {
            float value = static_cast<float>(grainPitchSlider.getValue());
            if (strip->getGrainArpDepth() > 0.001f)
            {
                value = std::abs(value);
                if (std::abs(static_cast<float>(grainPitchSlider.getValue()) - value) > 1.0e-4f)
                    grainPitchSlider.setValue(value, juce::dontSendNotification);
            }
            else
            {
                value = processor.quantizePitchSemitonesToGlobalScale(value);
                if (std::abs(static_cast<float>(grainPitchSlider.getValue()) - value) > 1.0e-4f)
                {
                    grainPitchSlider.setValue(value, juce::sendNotificationSync);
                    return;
                }
            }
            strip->setGrainPitch(value);
            processor.queueActiveSceneAutosave();
        }
    };
    grainPitchJitterSlider.onValueChange = [this]()
    {
        if (auto* strip = processor.getAudioEngine()->getStrip(stripIndex))
        {
            strip->setGrainPitchJitter(static_cast<float>(grainPitchJitterSlider.getValue()));
            processor.queueActiveSceneAutosave();
        }
    };
    grainSpreadSlider.onValueChange = [this]()
    {
        if (auto* strip = processor.getAudioEngine()->getStrip(stripIndex))
        {
            strip->setGrainSpread(static_cast<float>(grainSpreadSlider.getValue()));
            processor.queueActiveSceneAutosave();
        }
    };
    grainJitterSlider.onValueChange = [this]()
    {
        if (auto* strip = processor.getAudioEngine()->getStrip(stripIndex))
        {
            strip->setGrainJitter(static_cast<float>(grainJitterSlider.getValue()));
            processor.queueActiveSceneAutosave();
        }
    };
    grainPositionJitterSlider.onValueChange = [this]()
    {
        if (auto* strip = processor.getAudioEngine()->getStrip(stripIndex))
        {
            strip->setGrainPositionJitter(static_cast<float>(grainPositionJitterSlider.getValue()));
            processor.queueActiveSceneAutosave();
        }
    };
    grainRandomSlider.onValueChange = [this]()
    {
        if (auto* strip = processor.getAudioEngine()->getStrip(stripIndex))
        {
            strip->setGrainRandomDepth(static_cast<float>(grainRandomSlider.getValue()));
            processor.queueActiveSceneAutosave();
        }
    };
    grainArpSlider.onValueChange = [this]()
    {
        if (auto* strip = processor.getAudioEngine()->getStrip(stripIndex))
        {
            strip->setGrainArpDepth(static_cast<float>(grainArpSlider.getValue()));
            if (grainArpSlider.getValue() > 0.001)
            {
                const int mode = juce::jlimit(0, 5, static_cast<int>(std::floor(juce::jlimit(0.0, 0.999999, grainArpSlider.getValue()) * 6.0)));
                strip->setGrainArpMode(mode);
            }
            processor.queueActiveSceneAutosave();
        }
    };
    grainCloudSlider.onValueChange = [this]()
    {
        if (auto* strip = processor.getAudioEngine()->getStrip(stripIndex))
        {
            strip->setGrainCloudDepth(static_cast<float>(grainCloudSlider.getValue()));
            processor.queueActiveSceneAutosave();
        }
    };
    grainEmitterSlider.onValueChange = [this]()
    {
        if (auto* strip = processor.getAudioEngine()->getStrip(stripIndex))
        {
            strip->setGrainEmitterDepth(static_cast<float>(grainEmitterSlider.getValue()));
            processor.queueActiveSceneAutosave();
        }
    };
    grainEnvelopeSlider.onValueChange = [this]()
    {
        if (auto* strip = processor.getAudioEngine()->getStrip(stripIndex))
        {
            strip->setGrainEnvelope(static_cast<float>(grainEnvelopeSlider.getValue()));
            processor.queueActiveSceneAutosave();
        }
    };
    grainShapeSlider.onValueChange = [this]()
    {
        if (auto* strip = processor.getAudioEngine()->getStrip(stripIndex))
        {
            strip->setGrainShape(static_cast<float>(grainShapeSlider.getValue()));
            processor.queueActiveSceneAutosave();
        }
    };

    grainSizeSyncToggle.setButtonText("");
    grainSizeSyncToggle.setClickingTogglesState(true);
    grainSizeSyncToggle.setToggleState(true, juce::dontSendNotification);
    grainSizeSyncToggle.setColour(juce::ToggleButton::textColourId, stripColor.withAlpha(0.72f));
    grainSizeSyncToggle.setColour(juce::ToggleButton::tickColourId, stripColor.withAlpha(0.72f));
    grainSizeSyncToggle.setColour(juce::ToggleButton::tickDisabledColourId, stripColor.withAlpha(0.28f));
    grainSizeSyncToggle.setTooltip("Tempo-sync grain size.");
    grainSizeSyncToggle.onClick = [this]()
    {
        const bool enabled = grainSizeSyncToggle.getToggleState();
        grainSizeDivLabel.setText(enabled ? "SYNC" : "FREE", juce::dontSendNotification);
        grainSizeSyncToggle.setColour(juce::ToggleButton::textColourId, enabled ? stripColor.brighter(0.35f) : stripColor.withAlpha(0.72f));
        grainSizeSyncToggle.setColour(juce::ToggleButton::tickColourId, enabled ? stripColor.brighter(0.35f) : stripColor.withAlpha(0.72f));
        if (auto* strip = processor.getAudioEngine()->getStrip(stripIndex))
        {
            strip->setGrainTempoSyncEnabled(enabled);
            processor.queueActiveSceneAutosave();
        }
    };
    addAndMakeVisible(grainSizeSyncToggle);

    grainSizeDivLabel.setText("SYNC", juce::dontSendNotification);
    grainSizeDivLabel.setJustificationType(juce::Justification::centredRight);
    grainSizeDivLabel.setColour(juce::Label::textColourId, stripColor.withAlpha(0.78f));
    grainSizeDivLabel.setFont(juce::Font(juce::FontOptions(9.0f, juce::Font::bold)));
    addAndMakeVisible(grainSizeDivLabel);
    grainSizeLabel.setJustificationType(juce::Justification::centredLeft);
    grainDensityLabel.setJustificationType(juce::Justification::centredLeft);

    auto setupGrainTab = [this](juce::TextButton& button, const juce::String& text, GrainSubPage page)
    {
        button.setButtonText(text);
        button.setClickingTogglesState(false);
        button.setTooltip("Grain page: " + text);
        styleUiButton(button, false);
        button.onClick = [this, page]()
        {
            grainSubPage = page;
            updateGrainTabButtons();
            updateGrainOverlayVisibility();
            resized();
            repaint();
        };
        addAndMakeVisible(button);
    };
    setupGrainTab(grainTabPitchButton, "ENGINE", GrainSubPage::Pitch);
    setupGrainTab(grainTabSpaceButton, "SPACE", GrainSubPage::Space);
    setupGrainTab(grainTabShapeButton, "SHAPE", GrainSubPage::Shape);
    updateGrainTabButtons();

    patternLengthBox.addItem("16", 1);
    patternLengthBox.addItem("32", 2);
    patternLengthBox.addItem("48", 3);
    patternLengthBox.addItem("64", 4);
    patternLengthBox.setJustificationType(juce::Justification::centred);
    patternLengthBox.setTextWhenNothingSelected("");
    patternLengthBox.setTextWhenNoChoicesAvailable("");
    patternLengthBox.setColour(juce::ComboBox::textColourId, juce::Colours::transparentWhite);
    patternLengthBox.setSelectedId(1, juce::dontSendNotification);
    patternLengthBox.setTooltip("Step pattern length");
    patternLengthBox.onChange = [this]()
    {
        const int bars = juce::jmax(1, patternLengthBox.getSelectedId());
        const int steps = juce::jlimit(1, 64, bars * 16);
        if (auto* strip = processor.getAudioEngine()->getStrip(stripIndex))
            strip->setStepPatternBars(bars);
        stepLengthReadoutBox.setValue(steps, juce::dontSendNotification);
    };
    addAndMakeVisible(patternLengthBox);

    stepLengthReadoutBox.setRange(1, 64);
    stepLengthReadoutBox.setEditable(false, true, false);
    stepLengthReadoutBox.setJustificationType(juce::Justification::centred);
    stepLengthReadoutBox.setInterceptsMouseClicks(true, false);
    stepLengthReadoutBox.setColour(juce::Label::backgroundColourId, juce::Colour(0xff202427));
    stepLengthReadoutBox.setColour(juce::Label::textColourId, kTextPrimary);
    stepLengthReadoutBox.setColour(juce::Label::outlineColourId, juce::Colour(0xff5a5f64));
    stepLengthReadoutBox.setColour(juce::TextEditor::focusedOutlineColourId, stripColor.withAlpha(0.9f));
    stepLengthReadoutBox.setTooltip("Step pattern length (1..64). Drag or double-click to type.");
    stepLengthReadoutBox.setValue(16, juce::dontSendNotification);
    stepLengthReadoutBox.onValueChange = [this](int steps)
    {
        const int clampedSteps = juce::jlimit(1, 64, steps);
        if (auto* strip = processor.getAudioEngine()->getStrip(stripIndex))
            strip->setStepPatternLengthSteps(clampedSteps);

        if (clampedSteps % 16 == 0)
            patternLengthBox.setSelectedId(juce::jlimit(1, 4, clampedSteps / 16), juce::dontSendNotification);
        else
        {
            patternLengthBox.setSelectedId(0, juce::dontSendNotification);
            patternLengthBox.setText(juce::String(clampedSteps), juce::dontSendNotification);
        }
    };
    addAndMakeVisible(stepLengthReadoutBox);

    auto setupStepEnvelopeSlider = [this](juce::Slider& slider, juce::Label& label,
                                          const char* text, double min, double max, double def, double skewMid)
    {
        slider.setLookAndFeel(&knobLookAndFeel);
        slider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        slider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        slider.setRange(min, max, 0.1);
        slider.setSkewFactorFromMidPoint(skewMid);
        slider.setValue(def, juce::dontSendNotification);
        slider.setPopupDisplayEnabled(true, false, this);
        slider.setTextValueSuffix(" ms");
        slider.setColour(juce::Slider::trackColourId, stripColor.withAlpha(0.9f));
        slider.setColour(juce::Slider::thumbColourId, stripColor.brighter(0.35f));
        enableAltClickReset(slider, def);
        addAndMakeVisible(slider);

        label.setText(text, juce::dontSendNotification);
        label.setFont(juce::Font(juce::FontOptions(9.0f, juce::Font::bold)));
        label.setJustificationType(juce::Justification::centred);
        label.setColour(juce::Label::textColourId, stripColor.brighter(0.35f));
        addAndMakeVisible(label);
    };

    setupStepEnvelopeSlider(stepAttackSlider, stepAttackLabel, "A", 0.0, 400.0, 0.0, 12.0);
    setupStepEnvelopeSlider(stepDecaySlider, stepDecayLabel, "D", 1.0, 4000.0, 4000.0, 700.0);
    setupStepEnvelopeSlider(stepReleaseSlider, stepReleaseLabel, "R", 1.0, 4000.0, 110.0, 180.0);
    stepAttackSlider.setTooltip("Step envelope attack");
    stepDecaySlider.setTooltip("Step envelope decay");
    stepReleaseSlider.setTooltip("Step envelope release");
    stepAttackSlider.onValueChange = [this]()
    {
        if (auto* strip = processor.getAudioEngine()->getStrip(stripIndex))
            strip->setStepEnvelopeAttackMs(static_cast<float>(stepAttackSlider.getValue()));
    };
    stepDecaySlider.onValueChange = [this]()
    {
        if (auto* strip = processor.getAudioEngine()->getStrip(stripIndex))
            strip->setStepEnvelopeDecayMs(static_cast<float>(stepDecaySlider.getValue()));
    };
    stepReleaseSlider.onValueChange = [this]()
    {
        if (auto* strip = processor.getAudioEngine()->getStrip(stripIndex))
            strip->setStepEnvelopeReleaseMs(static_cast<float>(stepReleaseSlider.getValue()));
    };
    
    // Labels below knobs
    volumeLabel.setText("VOL", juce::dontSendNotification);
    volumeLabel.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));  // Increased from 9
    volumeLabel.setJustificationType(juce::Justification::centred);
    volumeLabel.setColour(juce::Label::textColourId, stripColor.brighter(0.3f));
    addAndMakeVisible(volumeLabel);
    
    panLabel.setText("PAN", juce::dontSendNotification);
    panLabel.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
    panLabel.setJustificationType(juce::Justification::centred);
    panLabel.setColour(juce::Label::textColourId, stripColor.brighter(0.3f));
    addAndMakeVisible(panLabel);

    pitchLabel.setText("PITCH", juce::dontSendNotification);
    pitchLabel.setFont(juce::Font(juce::FontOptions(9.0f, juce::Font::bold)));
    pitchLabel.setJustificationType(juce::Justification::centred);
    pitchLabel.setColour(juce::Label::textColourId, stripColor.brighter(0.3f));
    addAndMakeVisible(pitchLabel);

    speedLabel.setText("SPEED", juce::dontSendNotification);
    speedLabel.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
    speedLabel.setJustificationType(juce::Justification::centred);
    speedLabel.setColour(juce::Label::textColourId, stripColor.brighter(0.3f));
    addAndMakeVisible(speedLabel);
    
    scratchLabel.setText("SCR", juce::dontSendNotification);
    scratchLabel.setFont(juce::Font(juce::FontOptions(9.0f, juce::Font::bold)));
    scratchLabel.setJustificationType(juce::Justification::centred);
    scratchLabel.setColour(juce::Label::textColourId, stripColor.brighter(0.3f));
    addAndMakeVisible(scratchLabel);

    sliceLengthLabel.setText("SLICE", juce::dontSendNotification);
    sliceLengthLabel.setFont(juce::Font(juce::FontOptions(8.5f, juce::Font::bold)));
    sliceLengthLabel.setJustificationType(juce::Justification::centred);
    sliceLengthLabel.setColour(juce::Label::textColourId, stripColor.brighter(0.3f));
    addAndMakeVisible(sliceLengthLabel);

    // Label showing current beats setting
    tempoLabel.setText("AUTO", juce::dontSendNotification);
    tempoLabel.setFont(juce::Font(juce::FontOptions(9.0f)));
    tempoLabel.setJustificationType(juce::Justification::centred);
    tempoLabel.setColour(juce::Label::textColourId, stripColor);
    addAndMakeVisible(tempoLabel);
    tempoLabel.setTooltip("Beats per loop (auto or manual).");

    recordBarsLabel.setText("", juce::dontSendNotification);
    recordBarsLabel.setFont(juce::Font(juce::FontOptions(8.0f, juce::Font::bold)));
    recordBarsLabel.setJustificationType(juce::Justification::centredLeft);
    recordBarsLabel.setColour(juce::Label::textColourId, kTextMuted);
    addAndMakeVisible(recordBarsLabel);
    recordBarsLabel.setTooltip("Unified loop bars: used for live capture and loaded sample tempo mapping.");

    recordBarsBox.addItem("1/4", 25);
    recordBarsBox.addItem("1/2", 50);
    recordBarsBox.addItem("1", 100);
    recordBarsBox.addItem("2", 200);
    recordBarsBox.addItem("4", 400);
    recordBarsBox.addItem("8", 800);
    recordBarsBox.setJustificationType(juce::Justification::centredLeft);
    recordBarsBox.setSelectedId(200, juce::dontSendNotification);
    recordBarsBox.setTooltip("Loop bars per strip (capture + loaded sample mapping).");
    recordBarsBox.onChange = [this]()
    {
        processor.requestBarLengthChange(stripIndex, recordBarsBox.getSelectedId());
    };
    addAndMakeVisible(recordBarsBox);

    recordButton.setButtonText("REC");
    recordButton.setTooltip("Left-click: capture recent input audio. Right-click: clear recent input buffer.");
    recordButton.onClick = [this]()
    {
        processor.captureRecentAudioToStrip(stripIndex);
    };
    addAndMakeVisible(recordButton);
    recordButton.addMouseListener(this, false);

    tempoMatchOverrideBox.addItem("Global", 1);
    tempoMatchOverrideBox.addItem("Repitch", 2);
    tempoMatchOverrideBox.addItem("Stretch", 3);
    tempoMatchOverrideBox.setSelectedId(1, juce::dontSendNotification);
    tempoMatchOverrideBox.setJustificationType(juce::Justification::centred);
    tempoMatchOverrideBox.setTooltip("Per-strip loop tempo-match override. Global follows the Global tab, Repitch forces resample tempo match, Stretch follows the current Stretch selection.");
    addAndMakeVisible(tempoMatchOverrideBox);
    styleUiCombo(tempoMatchOverrideBox);
    tempoMatchOverrideAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        processor.parameters, "stripTempoMatchMode" + juce::String(stripIndex), tempoMatchOverrideBox);

    pitchControlOverrideBox.addItem("Global", 1);
    pitchControlOverrideBox.addItem("Pitch", 2);
    pitchControlOverrideBox.addItem("Touch", 3);
    pitchControlOverrideBox.addItem("Resamp", 4);
    pitchControlOverrideBox.addItem("Signal", 5);
    pitchControlOverrideBox.addItem("Bungee", 6);
    pitchControlOverrideBox.setSelectedId(1, juce::dontSendNotification);
    pitchControlOverrideBox.setJustificationType(juce::Justification::centred);
    pitchControlOverrideBox.setTooltip(
        "Per-strip pitch backend override. Global follows the Global Pitch dropdown; the other options force this strip to use that pitch method. "
        "Signalsmith is not currently available in Sample mode, so Sample strips use the local Pitch Shift path instead.");
    addAndMakeVisible(pitchControlOverrideBox);
    styleUiCombo(pitchControlOverrideBox);
    pitchControlOverrideAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        processor.parameters, "stripPitchControlMode" + juce::String(stripIndex), pitchControlOverrideBox);

    modTargetLabel.setText("TARGET", juce::dontSendNotification);
    modTargetLabel.setFont(juce::Font(juce::FontOptions(8.0f, juce::Font::bold)));
    modTargetLabel.setColour(juce::Label::textColourId, kTextMuted);
    addAndMakeVisible(modTargetLabel);

    for (auto target : kModPerformanceTargetOrder)
        modTargetBox.addItem(performanceTargetDisplayName(target, true), performanceTargetToComboId(target));
    modTargetBox.setSelectedId(1, juce::dontSendNotification);
    modTargetBox.onChange = [this]()
    {
        if (auto* engine = processor.getAudioEngine())
        {
            const auto target = comboIdToModTarget(modTargetBox.getSelectedId());
            engine->setModTarget(stripIndex, target);
            if (target == ModernAudioEngine::ModTarget::Rearrange)
                engine->setModEditPage(stripIndex, 0);
            modBipolarToggle.setToggleState(engine->isModBipolar(stripIndex), juce::dontSendNotification);
        }
        resized();
        repaint();
    };
    addAndMakeVisible(modTargetBox);

    modBipolarToggle.setButtonText("BIP");
    modBipolarToggle.setTooltip("Click: convert existing steps so neutral is preserved. Option-click: reinterpret stored values without remapping.");
    modBipolarToggle.onClick = [this]()
    {
        if (auto* engine = processor.getAudioEngine())
        {
            const auto mode = juce::ModifierKeys::getCurrentModifiersRealtime().isAltDown()
                ? ModernAudioEngine::ModBipolarToggleMode::Reinterpret
                : ModernAudioEngine::ModBipolarToggleMode::ConvertPreserveNeutral;
            engine->setModBipolar(stripIndex, modBipolarToggle.getToggleState(), mode);
        }
    };
    addAndMakeVisible(modBipolarToggle);

    modDepthLabel.setText("DEPTH", juce::dontSendNotification);
    modDepthLabel.setFont(juce::Font(juce::FontOptions(8.0f, juce::Font::bold)));
    modDepthLabel.setColour(juce::Label::textColourId, kTextMuted);
    addAndMakeVisible(modDepthLabel);

    modDepthSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    modDepthSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    modDepthSlider.setRange(0.0, 1.0, 0.01);
    modDepthSlider.onValueChange = [this]()
    {
        if (auto* engine = processor.getAudioEngine())
            engine->setModDepth(stripIndex, static_cast<float>(modDepthSlider.getValue()));
    };
    addAndMakeVisible(modDepthSlider);

    modRateLabel.setText("RATE", juce::dontSendNotification);
    modRateLabel.setFont(juce::Font(juce::FontOptions(8.0f, juce::Font::bold)));
    modRateLabel.setColour(juce::Label::textColourId, kTextMuted);
    addAndMakeVisible(modRateLabel);

    for (size_t idx = 0; idx < modRateChoices().size(); ++idx)
        modRateBox.addItem(modRateLabelForValue(modRateChoices()[idx]), static_cast<int>(idx) + 1);
    modRateBox.setSelectedId(modRateToComboId(1.0f), juce::dontSendNotification);
    modRateBox.onChange = [this]()
    {
        if (auto* engine = processor.getAudioEngine())
            engine->setModRate(stripIndex, comboIdToModRate(modRateBox.getSelectedId()));
    };
    addAndMakeVisible(modRateBox);

    modTransportLabel.setText("CLOCK", juce::dontSendNotification);
    modTransportLabel.setFont(juce::Font(juce::FontOptions(8.0f, juce::Font::bold)));
    modTransportLabel.setColour(juce::Label::textColourId, kTextMuted);
    addAndMakeVisible(modTransportLabel);

    modTransportBox.addItem("Scene Loop", static_cast<int>(ModernAudioEngine::ModTransportMode::Free) + 1);
    modTransportBox.addItem("Follow Scene", static_cast<int>(ModernAudioEngine::ModTransportMode::Scene) + 1);
    modTransportBox.addItem("Follow Strip", static_cast<int>(ModernAudioEngine::ModTransportMode::Sync) + 1);
    modTransportBox.setSelectedId(static_cast<int>(ModernAudioEngine::ModTransportMode::Free) + 1,
                                  juce::dontSendNotification);
    modTransportBox.onChange = [this]()
    {
        if (auto* engine = processor.getAudioEngine())
        {
            engine->setModTransportMode(stripIndex,
                static_cast<ModernAudioEngine::ModTransportMode>(juce::jlimit(
                    0,
                    static_cast<int>(ModernAudioEngine::ModTransportMode::Scene),
                    modTransportBox.getSelectedId() - 1)));
        }
    };
    addAndMakeVisible(modTransportBox);

    modOffsetLabel.setText("SMTH", juce::dontSendNotification);
    modOffsetLabel.setFont(juce::Font(juce::FontOptions(8.0f, juce::Font::bold)));
    modOffsetLabel.setColour(juce::Label::textColourId, kTextMuted);
    addAndMakeVisible(modOffsetLabel);

    modOffsetSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    modOffsetSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    modOffsetSlider.setRange(0.0, 250.0, 1.0);
    modOffsetSlider.setSkewFactorFromMidPoint(40.0);
    modOffsetSlider.onValueChange = [this]()
    {
        if (auto* engine = processor.getAudioEngine())
            engine->setModSmoothingMs(stripIndex, static_cast<float>(modOffsetSlider.getValue()));
    };
    addAndMakeVisible(modOffsetSlider);

    modCurveBendLabel.setText("BEND", juce::dontSendNotification);
    modCurveBendLabel.setFont(juce::Font(juce::FontOptions(8.0f, juce::Font::bold)));
    modCurveBendLabel.setColour(juce::Label::textColourId, kTextMuted);
    addAndMakeVisible(modCurveBendLabel);

    modCurveBendSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    modCurveBendSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    modCurveBendSlider.setRange(-1.0, 1.0, 0.01);
    modCurveBendSlider.setValue(0.0, juce::dontSendNotification);
    modCurveBendSlider.onValueChange = [this]()
    {
        if (auto* engine = processor.getAudioEngine())
            engine->setModCurveBend(stripIndex, static_cast<float>(modCurveBendSlider.getValue()));
    };
    addAndMakeVisible(modCurveBendSlider);

    modLengthLabel.setText("LEN", juce::dontSendNotification);
    modLengthLabel.setFont(juce::Font(juce::FontOptions(8.0f, juce::Font::bold)));
    modLengthLabel.setColour(juce::Label::textColourId, kTextMuted);
    addAndMakeVisible(modLengthLabel);

    for (int bars = 1; bars <= ModernAudioEngine::MaxModBars; ++bars)
        modLengthBox.addItem(juce::String(bars), bars);
    modLengthBox.setSelectedId(1, juce::dontSendNotification);
    modLengthBox.onChange = [this]()
    {
        if (auto* engine = processor.getAudioEngine())
            engine->setModLengthBars(stripIndex, modLengthBox.getSelectedId());
    };
    addAndMakeVisible(modLengthBox);

    for (int slot = 0; slot < ModernAudioEngine::NumModSequencers; ++slot)
    {
        auto& tab = modSequencerTabs[static_cast<size_t>(slot)];
        tab.setButtonText("S" + juce::String(slot + 1));
        tab.setTooltip("Switch to modulation sequencer " + juce::String(slot + 1)
            + ". Default target: "
            + modTargetDisplayName(ModernAudioEngine::defaultModTargetForSlot(slot)) + ".");
        tab.onClick = [this, slot]()
        {
            if (auto* engine = processor.getAudioEngine())
                engine->setModSequencerSlot(stripIndex, slot);
            updateModSequencerTabButtons();
            repaint();
        };
        addAndMakeVisible(tab);
    }
    updateModSequencerTabButtons();

    modPitchQuantToggle.setButtonText("P.Quant");
    modPitchQuantToggle.onClick = [this]()
    {
        if (auto* engine = processor.getAudioEngine())
            engine->setModPitchScaleQuantize(stripIndex, modPitchQuantToggle.getToggleState());
    };
    addAndMakeVisible(modPitchQuantToggle);

    modPitchScaleBox.addItem("None", 1);
    modPitchScaleBox.addItem("Maj", 2);
    modPitchScaleBox.addItem("Min", 3);
    modPitchScaleBox.addItem("Dor", 4);
    modPitchScaleBox.addItem("Pent", 5);
    modPitchScaleBox.setSelectedId(1, juce::dontSendNotification);
    modPitchScaleBox.onChange = [this]()
    {
        if (auto* engine = processor.getAudioEngine())
            engine->setModPitchScale(stripIndex, comboIdToPitchScale(modPitchScaleBox.getSelectedId()));
    };
    addAndMakeVisible(modPitchScaleBox);

    modTargetHintLabel.setColour(juce::Label::textColourId, kTextMuted.brighter(0.15f));
    modTargetHintLabel.setFont(juce::Font(juce::FontOptions(9.0f)));
    modTargetHintLabel.setJustificationType(juce::Justification::centredLeft);
    modTargetHintLabel.setVisible(false);
    addAndMakeVisible(modTargetHintLabel);

    modShapeLabel.setText("SHAPE", juce::dontSendNotification);
    modShapeLabel.setFont(juce::Font(juce::FontOptions(8.0f, juce::Font::bold)));
    modShapeLabel.setColour(juce::Label::textColourId, kTextMuted);
    addAndMakeVisible(modShapeLabel);

    modShapeBox.addItem("Curve", 1);
    modShapeBox.addItem("Steps", 2);
    modShapeBox.setSelectedId(1, juce::dontSendNotification);
    modShapeBox.onChange = [this]()
    {
        const bool curveMode = (modShapeBox.getSelectedId() == 1);
        modCurveBendSlider.setEnabled(curveMode);
        modCurveTypeBox.setEnabled(curveMode);
        if (auto* engine = processor.getAudioEngine())
            engine->setModCurveMode(stripIndex, curveMode);
        resized();
        repaint();
    };
    addAndMakeVisible(modShapeBox);

    modCurveTypeLabel.setText("CTYPE", juce::dontSendNotification);
    modCurveTypeLabel.setFont(juce::Font(juce::FontOptions(8.0f, juce::Font::bold)));
    modCurveTypeLabel.setColour(juce::Label::textColourId, kTextMuted);
    addAndMakeVisible(modCurveTypeLabel);

    modCurveTypeBox.addItem("Normal", 1);
    modCurveTypeBox.addItem("Exp+", 2);
    modCurveTypeBox.addItem("Exp-", 3);
    modCurveTypeBox.addItem("Sine", 4);
    modCurveTypeBox.addItem("Square", 5);
    modCurveTypeBox.setSelectedId(1, juce::dontSendNotification);
    modCurveTypeBox.setTooltip("Curve draw type in Curve mode: Normal, Exp+/-, Sine, Square.");
    modCurveTypeBox.onChange = [this]()
    {
        if (auto* engine = processor.getAudioEngine())
            engine->setModCurveShape(stripIndex, comboIdToCurveShape(modCurveTypeBox.getSelectedId()));
    };
    addAndMakeVisible(modCurveTypeBox);
    
    // Legacy readout removed from strip UI (kept as hidden component for compatibility).
    recordLengthLabel.setVisible(false);

    patternLengthBox.setVisible(false);
    stepLengthReadoutBox.setVisible(false);
    stepAttackSlider.setVisible(false);
    stepDecaySlider.setVisible(false);
    stepReleaseSlider.setVisible(false);
    stepAttackLabel.setVisible(false);
    stepDecayLabel.setVisible(false);
    stepReleaseLabel.setVisible(false);
    updateGrainOverlayVisibility();
}

void StripControl::updateGrainOverlayVisibility()
{
    auto* strip = processor.getAudioEngine() != nullptr ? processor.getAudioEngine()->getStrip(stripIndex) : nullptr;
    const auto playMode = strip != nullptr ? strip->getPlayMode() : EnhancedAudioStrip::PlayMode::Loop;
    const bool isStepMode = (playMode == EnhancedAudioStrip::PlayMode::Step);
    const bool isSampleMode = (playMode == EnhancedAudioStrip::PlayMode::Sample);
    const bool isGrainMode = (playMode == EnhancedAudioStrip::PlayMode::Grain);
    grainOverlayVisible = isGrainMode;
    showingStepDisplay = isStepMode;
    showingSampleMode = isSampleMode;
    const bool showPitchPage = isGrainMode && grainSubPage == GrainSubPage::Pitch;
    const bool showSpacePage = isGrainMode && grainSubPage == GrainSubPage::Space;
    const bool showShapePage = isGrainMode && grainSubPage == GrainSubPage::Shape;

    if (isSampleMode)
        sampleModeComponent.setEngine(processor.getSampleModeEngine(stripIndex, true));
    else
        sampleModeComponent.setEngine(processor.getSampleModeEngine(stripIndex, false));

    waveform.setLoopInteractionEnabled(playMode == EnhancedAudioStrip::PlayMode::Loop);

    waveform.setVisible(!isStepMode && !isSampleMode && !modulationLaneView);
    stepDisplay.setVisible(isStepMode && !modulationLaneView);
    sampleModeComponent.setVisible(isSampleMode && !modulationLaneView);

    speedSlider.setSliderStyle(isGrainMode
                                   ? juce::Slider::LinearHorizontal
                                   : juce::Slider::RotaryHorizontalVerticalDrag);
    speedSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    speedSlider.setRange(isGrainMode ? 0.0 : 0.125, 8.0, 0.001);

    volumeSlider.setVisible(!isGrainMode);
    panSlider.setVisible(!isGrainMode);
    volumeLabel.setVisible(!isGrainMode);
    panLabel.setVisible(!isGrainMode);

    const bool showLoopKnobs = !showingStepDisplay && !isGrainMode && !isSampleMode;
    const bool showSampleModeKnobs = isSampleMode;
    bool showSliceLength = false;
    if (showLoopKnobs && strip != nullptr)
        showSliceLength = (strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Loop);

    pitchSlider.setVisible(showLoopKnobs || showSampleModeKnobs);
    speedSlider.setVisible(showLoopKnobs || showSampleModeKnobs || showPitchPage);
    scratchSlider.setVisible(showLoopKnobs);
    sliceLengthSlider.setVisible(showSliceLength);
    pitchLabel.setVisible(showLoopKnobs || showSampleModeKnobs);
    speedLabel.setVisible(showLoopKnobs || showSampleModeKnobs || showPitchPage);
    scratchLabel.setVisible(showLoopKnobs);
    sliceLengthLabel.setVisible(showSliceLength);
    patternLengthBox.setVisible(isStepMode && !isGrainMode);
    stepLengthReadoutBox.setVisible(isStepMode && !isGrainMode);
    stepAttackSlider.setVisible(isStepMode && !isGrainMode);
    stepDecaySlider.setVisible(isStepMode && !isGrainMode);
    stepReleaseSlider.setVisible(isStepMode && !isGrainMode);
    stepAttackLabel.setVisible(isStepMode && !isGrainMode);
    stepDecayLabel.setVisible(isStepMode && !isGrainMode);
    stepReleaseLabel.setVisible(isStepMode && !isGrainMode);
    recordLengthLabel.setVisible(false);

    grainSizeSlider.setVisible(showPitchPage);
    grainDensitySlider.setVisible(showPitchPage);
    grainPitchSlider.setVisible(showPitchPage);
    grainPitchJitterSlider.setVisible(showPitchPage);
    grainSpreadSlider.setVisible(showSpacePage);
    grainJitterSlider.setVisible(showSpacePage);
    grainPositionJitterSlider.setVisible(showSpacePage);
    grainRandomSlider.setVisible(showPitchPage);
    grainArpSlider.setVisible(showPitchPage);
    grainCloudSlider.setVisible(showSpacePage);
    grainEmitterSlider.setVisible(showSpacePage);
    grainEnvelopeSlider.setVisible(showShapePage);
    grainShapeSlider.setVisible(showShapePage);
    grainTabPitchButton.setVisible(isGrainMode);
    grainTabSpaceButton.setVisible(isGrainMode);
    grainTabShapeButton.setVisible(isGrainMode);
    grainSizeSyncToggle.setVisible(showPitchPage);
    grainSizeDivLabel.setVisible(showPitchPage);
    grainSizeLabel.setVisible(showPitchPage);
    grainDensityLabel.setVisible(showPitchPage);
    grainPitchLabel.setVisible(showPitchPage);
    grainPitchJitterLabel.setVisible(showPitchPage);
    grainSpreadLabel.setVisible(showSpacePage);
    grainJitterLabel.setVisible(showSpacePage);
    grainPositionJitterLabel.setVisible(showSpacePage);
    grainRandomLabel.setVisible(showPitchPage);
    grainArpLabel.setVisible(showPitchPage);
    grainCloudLabel.setVisible(showSpacePage);
    grainEmitterLabel.setVisible(showSpacePage);
    grainEnvelopeLabel.setVisible(showShapePage);
    grainShapeLabel.setVisible(showShapePage);
    updateGrainTabButtons();
}

void StripControl::updateGrainTabButtons()
{
    auto tintTab = [](juce::TextButton& button, bool active)
    {
        button.setColour(juce::TextButton::buttonColourId,
                         active ? kAccent.withAlpha(0.95f) : juce::Colour(0xff3b4146));
        button.setColour(juce::TextButton::buttonOnColourId,
                         active ? kAccent.brighter(0.12f) : juce::Colour(0xff4a5258));
        button.setColour(juce::TextButton::textColourOffId,
                         active ? juce::Colour(0xff121212) : kTextPrimary);
        button.setColour(juce::TextButton::textColourOnId,
                         active ? juce::Colour(0xff101010) : juce::Colour(0xfff5f5f5));
    };
    tintTab(grainTabPitchButton, grainSubPage == GrainSubPage::Pitch);
    tintTab(grainTabSpaceButton, grainSubPage == GrainSubPage::Space);
    tintTab(grainTabShapeButton, grainSubPage == GrainSubPage::Shape);
}

void StripControl::updateModSequencerTabButtons()
{
    const int activeSlot = processor.getAudioEngine()
        ? processor.getAudioEngine()->getModSequencerSlot(stripIndex)
        : 0;

    for (int slot = 0; slot < ModernAudioEngine::NumModSequencers; ++slot)
    {
        auto& tab = modSequencerTabs[static_cast<size_t>(slot)];
        const bool active = (slot == activeSlot);
        tab.setColour(juce::TextButton::buttonColourId,
                      active ? kAccent.withAlpha(0.95f) : juce::Colour(0xff3b4146));
        tab.setColour(juce::TextButton::buttonOnColourId,
                      active ? kAccent.brighter(0.12f) : juce::Colour(0xff4a5258));
        tab.setColour(juce::TextButton::textColourOffId,
                      active ? juce::Colour(0xff121212) : kTextPrimary);
        tab.setColour(juce::TextButton::textColourOnId,
                      active ? juce::Colour(0xff101010) : juce::Colour(0xfff5f5f5));
    }
}

void StripControl::handleWaveformShiftSliceEdit(double normalizedPosition)
{
    auto* engine = processor.getAudioEngine();
    if (engine == nullptr)
        return;

    auto* strip = engine->getStrip(stripIndex);
    if (strip == nullptr || !strip->hasAudio() || !strip->isTransientSliceMode())
        return;

    const int totalSamples = juce::jmax(1, strip->getAudioBuffer()->getNumSamples());
    const int sampleIndex = juce::jlimit(0,
                                         totalSamples - 1,
                                         static_cast<int>(std::round(juce::jlimit(0.0, 1.0, normalizedPosition)
                                                                     * static_cast<double>(juce::jmax(1, totalSamples - 1)))));
    const auto slices = strip->getSliceStartSamples(true);
    int closestIndex = 1;
    int closestDistance = std::numeric_limits<int>::max();
    for (int i = 1; i < ModernAudioEngine::MaxColumns; ++i)
    {
        const int distance = std::abs(slices[static_cast<size_t>(i)] - sampleIndex);
        if (distance < closestDistance)
        {
            closestDistance = distance;
            closestIndex = i;
        }
    }

    strip->setTransientSliceMarkerSample(closestIndex, sampleIndex);
    waveform.setSliceMarkers(strip->getSliceStartSamples(false),
                             strip->getSliceStartSamples(true),
                             totalSamples,
                             true);
}


void StripControl::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();

    drawPanel(g, bounds, stripColor, 10.0f);
    paintStripHarmonyOverlay(g, bounds, getStripHarmonyOverlayState(processor, stripIndex), stripColor);

    if (modulationLaneView)
    {
        paintModulationLane(g);
    }
    else if (!showingSampleMode)
    {
        // Paint LED overlay on top of waveform area
        paintLEDOverlay(g);
    }

    paintLoopPitchAnalysisProgress(g);
}

void StripControl::setModulationLaneView(bool shouldShow)
{
    if (modulationLaneView == shouldShow)
        return;
    if (shouldShow)
    {
        preModulationShowingStepDisplay = showingStepDisplay;
        preModulationWaveformVisible = waveform.isVisible();
        preModulationStepVisible = stepDisplay.isVisible();
        preModulationSampleVisible = sampleModeComponent.isVisible();
    }
    modulationLaneView = shouldShow;
    if (!shouldShow)
    {
        showingStepDisplay = preModulationShowingStepDisplay;
        waveform.setVisible(preModulationWaveformVisible);
        stepDisplay.setVisible(preModulationStepVisible);
        sampleModeComponent.setVisible(preModulationSampleVisible);
        modulationLastDrawStep = -1;
        updateGrainOverlayVisibility();
        updateFromEngine();
    }
    resized();
    repaint();
}

juce::Rectangle<int> StripControl::getModulationLaneBounds() const
{
    return modulationLaneBounds;
}

void StripControl::paintModulationLane(juce::Graphics& g)
{
    auto* engine = processor.getAudioEngine();
    if (!engine)
        return;

    auto lane = getModulationLaneBounds();
    if (lane.isEmpty())
        return;

    const auto seq = engine->getModSequencerState(stripIndex);
    const int lengthBars = juce::jlimit(1, ModernAudioEngine::MaxModBars, engine->getModLengthBars(stripIndex));
    const int totalSteps = juce::jmax(ModernAudioEngine::ModSteps, lengthBars * ModernAudioEngine::ModSteps);
    const int activeStep = juce::jlimit(0, totalSteps - 1, engine->getModCurrentGlobalStep(stripIndex));
    if (totalSteps <= 0)
        return;

    g.setColour(juce::Colour(0xff1f1f1f));
    g.fillRoundedRectangle(lane.toFloat(), 6.0f);
    g.setColour(stripColor.withAlpha(0.35f));
    g.drawRoundedRectangle(lane.toFloat().reduced(0.5f), 6.0f, 1.0f);

    const int activeSlot = juce::jlimit(0, ModernAudioEngine::NumModSequencers - 1, engine->getModSequencerSlot(stripIndex));
    const juce::String laneInfo = "SEQ " + juce::String(activeSlot + 1)
        + "  PAGE " + juce::String(seq.editPage + 1) + "/" + juce::String(lengthBars);
    auto infoBadge = juce::Rectangle<float>(lane.getX() + 8.0f, lane.getY() + 4.0f, 118.0f, 14.0f);
    g.setColour(juce::Colour(0xff111111).withAlpha(0.72f));
    g.fillRoundedRectangle(infoBadge, 3.0f);
    g.setColour(stripColor.withAlpha(0.22f));
    g.drawRoundedRectangle(infoBadge, 3.0f, 1.0f);
    g.setColour(kTextPrimary.withAlpha(0.88f));
    g.setFont(8.5f);
    g.drawText(laneInfo, infoBadge.toNearestInt(), juce::Justification::centred, false);

    const auto drawLane = lane.reduced(12, 2);
    const float dotSize = (totalSteps > 32) ? 4.0f : 6.0f;
    const float dotPad = dotSize * 0.6f;
    const float left = static_cast<float>(drawLane.getX()) + dotPad;
    const float right = juce::jmax(left, static_cast<float>(drawLane.getRight() - 1) - dotPad);
    const float top = static_cast<float>(drawLane.getY()) + 2.0f;
    const float bottom = static_cast<float>(drawLane.getBottom()) - 2.0f;
    const float width = juce::jmax(1.0f, right - left);
    const float height = bottom - top;
    const float stepWidth = juce::jmax(0.25f, width / static_cast<float>(juce::jmax(1, totalSteps)));
    const float centerY = top + (height * 0.5f);

    if (seq.bipolar)
    {
        g.setColour(juce::Colour(0xff454545));
        g.drawLine(left, centerY, right, centerY, 1.0f);
    }

    auto valueToY = [&](float v) -> float
    {
        const float clamped = juce::jlimit(0.0f, 1.0f, v);
        const float n = seq.bipolar ? ((clamped * 2.0f) - 1.0f) : clamped;
        return seq.bipolar
            ? (centerY - (n * (height * 0.48f)))
            : (bottom - (n * height));
    };

    std::vector<float> startValues(static_cast<size_t>(totalSteps));
    std::vector<float> endValues(static_cast<size_t>(totalSteps));
    std::vector<int> subdivisions(static_cast<size_t>(totalSteps));
    std::vector<ModernAudioEngine::ModCurveShape> stepCurveShapes(static_cast<size_t>(totalSteps),
                                                                   ModernAudioEngine::ModCurveShape::Linear);
    for (int i = 0; i < totalSteps; ++i)
    {
        const float startValue = juce::jlimit(0.0f, 1.0f, engine->getModStepValueAbsolute(stripIndex, i));
        const int subdiv = juce::jlimit(
            1, ModernAudioEngine::ModMaxStepSubdivisions, engine->getModStepSubdivisionAbsolute(stripIndex, i));
        float endValue = juce::jlimit(0.0f, 1.0f, engine->getModStepEndValueAbsolute(stripIndex, i));
        const auto stepCurveShape = engine->getModStepCurveShapeAbsolute(stripIndex, i);
        if (subdiv <= 1)
            endValue = startValue;
        startValues[static_cast<size_t>(i)] = startValue;
        endValues[static_cast<size_t>(i)] = endValue;
        subdivisions[static_cast<size_t>(i)] = subdiv;
        stepCurveShapes[static_cast<size_t>(i)] = stepCurveShape;
    }

    const float activeStepX = left + (stepWidth * static_cast<float>(activeStep));
    g.setColour(kAccent.withAlpha(0.10f));
    g.fillRect(activeStepX, top, juce::jmax(1.0f, stepWidth), juce::jmax(1.0f, height));

    const float bend = juce::jlimit(-1.0f, 1.0f, seq.curveBend);
    std::vector<juce::Point<float>> stepMarkerPoints(static_cast<size_t>(totalSteps));
    for (int i = 0; i < totalSteps; ++i)
    {
        const float markerPhase = seq.curveMode ? shapeSubdivisionBendPhaseUi(0.5f, bend) : 0.5f;
        const float markerValue = (subdivisions[static_cast<size_t>(i)] > 1)
            ? sampleModSubdivisionValueUi(
                startValues[static_cast<size_t>(i)],
                endValues[static_cast<size_t>(i)],
                subdivisions[static_cast<size_t>(i)],
                markerPhase)
            : startValues[static_cast<size_t>(i)];
        const float x = left + (stepWidth * (static_cast<float>(i) + 0.5f));
        stepMarkerPoints[static_cast<size_t>(i)] = { x, valueToY(markerValue) };
    }

    if (seq.curveMode)
    {
        juce::Path rawPath;
        std::vector<float> sampledX;
        std::vector<float> sampledValues;
        sampledX.reserve(static_cast<size_t>(totalSteps * 10));
        sampledValues.reserve(static_cast<size_t>(totalSteps * 10));
        bool started = false;
        for (int i = 0; i < totalSteps; ++i)
        {
            const int subdiv = subdivisions[static_cast<size_t>(i)];
            const bool hasLocalRamp = (subdiv > 1);
            const int segmentCount = hasLocalRamp ? juce::jlimit(2, 64, subdiv * 4) : 8;
            const float startValue = startValues[static_cast<size_t>(i)];
            const float endValue = endValues[static_cast<size_t>(i)];
            const float nextStart = startValues[static_cast<size_t>((i + 1) % totalSteps)];

            for (int s = 0; s <= segmentCount; ++s)
            {
                if (i > 0 && s == 0)
                    continue;

                const float t = static_cast<float>(s) / static_cast<float>(segmentCount);
                const float shapedT = shapeCurvePhaseUi(
                    t,
                    bend,
                    stepCurveShapes[static_cast<size_t>(i)]);
                const float bendT = shapeSubdivisionBendPhaseUi(t, bend);
                const float value = hasLocalRamp
                    ? sampleModSubdivisionValueUi(startValue,
                                                  endValue,
                                                  subdiv,
                                                  bendT)
                    : juce::jlimit(0.0f, 1.0f,
                                   startValue + ((nextStart - startValue) * shapedT));
                const float x = juce::jlimit(left, right, left + (stepWidth * (static_cast<float>(i) + t)));
                const float y = valueToY(value);

                if (!started)
                {
                    rawPath.startNewSubPath(x, y);
                    started = true;
                }
                else
                {
                    rawPath.lineTo(x, y);
                }
                sampledX.push_back(x);
                sampledValues.push_back(value);
            }
        }

        const float smoothingMs = juce::jlimit(0.0f, 250.0f, seq.smoothingMs);
        const bool showSmoothedOverlay = (smoothingMs > 0.05f && sampledValues.size() > 2);

        g.setColour(stripColor.withAlpha(showSmoothedOverlay ? 0.58f : 0.9f));
        g.strokePath(rawPath, juce::PathStrokeType(showSmoothedOverlay ? 1.6f : 2.0f));

        if (showSmoothedOverlay)
        {
            // Approximate post-curve smoothing for visual feedback in curve mode.
            const float refStepMs = 125.0f;
            const float totalMs = refStepMs * static_cast<float>(totalSteps);
            const int sampleCount = static_cast<int>(sampledValues.size());
            const float dtMs = totalMs / static_cast<float>(juce::jmax(1, sampleCount - 1));
            const float alpha = 1.0f - std::exp(-dtMs / juce::jmax(1.0f, smoothingMs));

            float smoothed = sampledValues.front();
            juce::Path smoothPath;
            smoothPath.startNewSubPath(sampledX.front(), valueToY(smoothed));
            for (size_t idx = 1; idx < sampledValues.size(); ++idx)
            {
                smoothed += (sampledValues[idx] - smoothed) * juce::jlimit(0.0f, 1.0f, alpha);
                smoothPath.lineTo(sampledX[idx], valueToY(smoothed));
            }

            g.setColour(juce::Colour(0xff101010).withAlpha(0.68f));
            g.strokePath(smoothPath, juce::PathStrokeType(3.4f));
            g.setColour(kAccent.brighter(0.35f).withAlpha(0.92f));
            g.strokePath(smoothPath, juce::PathStrokeType(2.2f));

            auto badge = juce::Rectangle<float>(right - 76.0f, top + 1.0f, 74.0f, 13.0f);
            g.setColour(juce::Colour(0xff111111).withAlpha(0.74f));
            g.fillRoundedRectangle(badge, 3.0f);
            g.setColour(kAccent.withAlpha(0.26f));
            g.drawRoundedRectangle(badge, 3.0f, 1.0f);
            g.setColour(juce::Colour(0xfff8e7c2).withAlpha(0.92f));
            g.setFont(8.0f);
            g.drawText("Smth " + juce::String(static_cast<int>(std::round(smoothingMs))) + "ms",
                       badge.toNearestInt(), juce::Justification::centred, false);
        }
    }
    else
    {
        for (int i = 0; i < totalSteps; ++i)
        {
            const float stepX = left + (stepWidth * static_cast<float>(i));
            const int subdiv = subdivisions[static_cast<size_t>(i)];
            const float startValue = startValues[static_cast<size_t>(i)];
            const float endValue = endValues[static_cast<size_t>(i)];
            const float slotWidth = stepWidth / static_cast<float>(juce::jmax(1, subdiv));
            const float barWidth = juce::jmax(1.0f, slotWidth * 0.72f);

            for (int s = 0; s < subdiv; ++s)
            {
                const float t = (subdiv <= 1)
                    ? 1.0f
                    : (static_cast<float>(s + 1) / static_cast<float>(subdiv));
                const float value = (subdiv <= 1)
                    ? startValue
                    : juce::jlimit(0.0f, 1.0f, startValue + ((endValue - startValue) * t));

                float y0 = seq.bipolar ? centerY : bottom;
                const float y1 = valueToY(value);
                const float x = stepX + (slotWidth * (static_cast<float>(s) + 0.5f)) - (barWidth * 0.5f);
                const float yTop = juce::jmin(y0, y1);
                const float h = juce::jmax(1.0f, std::abs(y1 - y0));
                const float shade = (subdiv <= 1)
                    ? 0.55f
                    : juce::jmap(static_cast<float>(s) / static_cast<float>(juce::jmax(1, subdiv - 1)), 0.72f, 0.44f);
                g.setColour(stripColor.withAlpha(shade));
                g.fillRoundedRectangle(x, yTop, barWidth, h, 1.5f);
            }
        }
    }

    for (int i = 0; i < totalSteps; ++i)
    {
        const auto point = stepMarkerPoints[static_cast<size_t>(i)];
        const bool isActive = (i == activeStep);
        g.setColour(isActive ? kAccent : stripColor.withMultipliedBrightness(0.8f));
        g.fillEllipse(point.x - (dotSize * 0.5f), point.y - (dotSize * 0.5f), dotSize, dotSize);

        const int subdiv = subdivisions[static_cast<size_t>(i)];
        if (subdiv > 1 && stepWidth > 14.0f)
        {
            auto label = juce::Rectangle<float>(
                left + (stepWidth * static_cast<float>(i)),
                top + 1.0f,
                juce::jmax(8.0f, stepWidth),
                juce::jmax(8.0f, juce::jmin(12.0f, height * 0.25f)));
            g.setColour(juce::Colour(0xfff0f0f0).withAlpha(0.72f));
            g.setFont(juce::jmax(7.0f, juce::jmin(10.0f, stepWidth * 0.34f)));
            g.drawText("x" + juce::String(subdiv), label.toNearestInt(), juce::Justification::centred, false);
        }
    }
}

void StripControl::paintLoopPitchAnalysisProgress(juce::Graphics& g)
{
    if (!loopPitchAnalysisActive || loopPitchProgressBounds.isEmpty())
        return;

    auto bounds = loopPitchProgressBounds.toFloat().reduced(0.0f, 1.0f);
    if (bounds.getWidth() <= 6.0f || bounds.getHeight() <= 2.0f)
        return;

    g.setColour(juce::Colour(0xff121212).withAlpha(0.92f));
    g.fillRoundedRectangle(bounds, 3.0f);

    const auto fillWidth = juce::jmax(2.0f, bounds.getWidth() * juce::jlimit(0.0f, 1.0f, loopPitchAnalysisProgress));
    auto fillBounds = bounds.withWidth(fillWidth);
    g.setColour(stripColor.withMultipliedBrightness(1.1f).withAlpha(0.92f));
    g.fillRoundedRectangle(fillBounds, 3.0f);

    g.setColour(stripColor.withAlpha(0.35f));
    g.drawRoundedRectangle(bounds, 3.0f, 1.0f);

    g.setColour(kTextPrimary.withAlpha(0.9f));
    g.setFont(8.5f);
    const auto text = loopPitchAnalysisStatus.isNotEmpty()
        ? loopPitchAnalysisStatus
        : "Pitch";
    g.drawFittedText(text, loopPitchProgressBounds, juce::Justification::centred, 1);
}

void StripControl::applyModulationPoint(juce::Point<int> p)
{
    auto* engine = processor.getAudioEngine();
    if (!engine || stripIndex >= MlrVSTAudioProcessor::MaxStrips)
        return;

    auto lane = getModulationLaneBounds().reduced(12, 2);
    auto hitLane = lane.expanded(1, 0);
    if (!hitLane.contains(p))
        return;

    const int lengthBars = juce::jlimit(1, ModernAudioEngine::MaxModBars, engine->getModLengthBars(stripIndex));
    const int totalSteps = juce::jmax(ModernAudioEngine::ModSteps, lengthBars * ModernAudioEngine::ModSteps);
    if (modulationLastDrawStep >= totalSteps)
        modulationLastDrawStep = -1;
    const bool isRearrangeTarget = (engine->getModSequencerState(stripIndex).target == ModernAudioEngine::ModTarget::Rearrange);
    const float x = juce::jlimit(static_cast<float>(lane.getX()),
                                 static_cast<float>(lane.getRight() - 1),
                                 static_cast<float>(p.x));
    const float nx = juce::jlimit(0.0f, 1.0f, (x - static_cast<float>(lane.getX())) / juce::jmax(1.0f, static_cast<float>(lane.getWidth() - 1)));
    const float ny = juce::jlimit(0.0f, 1.0f, (static_cast<float>(p.y - lane.getY())) / juce::jmax(1.0f, static_cast<float>(lane.getHeight())));
    const int step = juce::jlimit(0, totalSteps - 1,
                                  static_cast<int>(std::round(nx * static_cast<float>(juce::jmax(1, totalSteps - 1)))));
    const float value = isRearrangeTarget
        ? quantizeRearrangeStepValue(1.0f - ny)
        : juce::jlimit(0.0f, 1.0f, 1.0f - ny);
    if (modulationLastDrawStep < 0)
    {
        engine->setModStepValueAbsolute(stripIndex, step, value);
        modulationLastDrawStep = step;
        modulationLastDrawValue = value;
        return;
    }

    const int from = juce::jmin(modulationLastDrawStep, step);
    const int to = juce::jmax(modulationLastDrawStep, step);
    for (int s = from; s <= to; ++s)
    {
        const float t = (to == from) ? 1.0f : (static_cast<float>(s - from) / static_cast<float>(to - from));
        const float v = isRearrangeTarget
            ? quantizeRearrangeStepValue(modulationLastDrawValue + ((value - modulationLastDrawValue) * t))
            : (modulationLastDrawValue + ((value - modulationLastDrawValue) * t));
        engine->setModStepValueAbsolute(stripIndex, s, v);
    }
    modulationLastDrawStep = step;
    modulationLastDrawValue = value;
}

int StripControl::getModulationStepFromPoint(juce::Point<int> p) const
{
    auto* engine = processor.getAudioEngine();
    if (!engine)
        return -1;
    auto lane = getModulationLaneBounds().reduced(12, 2);
    if (lane.isEmpty())
        return -1;
    if (!lane.expanded(1, 0).contains(p))
        return -1;

    const int lengthBars = juce::jlimit(1, ModernAudioEngine::MaxModBars, engine->getModLengthBars(stripIndex));
    const int totalSteps = juce::jmax(ModernAudioEngine::ModSteps, lengthBars * ModernAudioEngine::ModSteps);
    const float x = juce::jlimit(static_cast<float>(lane.getX()),
                                 static_cast<float>(lane.getRight() - 1),
                                 static_cast<float>(p.x));
    const float nx = juce::jlimit(0.0f, 1.0f,
                                  (x - static_cast<float>(lane.getX()))
                                  / juce::jmax(1.0f, static_cast<float>(lane.getWidth() - 1)));
    return juce::jlimit(0, totalSteps - 1,
                        static_cast<int>(std::round(nx * static_cast<float>(juce::jmax(1, totalSteps - 1)))));
}

void StripControl::applyModulationCellDuplicateFromDrag(int deltaY)
{
    auto* engine = processor.getAudioEngine();
    if (!engine || stripIndex >= MlrVSTAudioProcessor::MaxStrips || modTransformStep < 0)
        return;

    const int lengthBars = juce::jlimit(1, ModernAudioEngine::MaxModBars, engine->getModLengthBars(stripIndex));
    const int totalSteps = juce::jmax(ModernAudioEngine::ModSteps, lengthBars * ModernAudioEngine::ModSteps);
    if (modTransformStep >= totalSteps)
        return;

    const int nextSubdivision = juce::jlimit(
        1,
        ModernAudioEngine::ModMaxStepSubdivisions,
        modTransformStartSubdivision + ((-deltaY) / 14));
    const float endValue = (nextSubdivision > 1) ? modTransformStartEndValue : modTransformStartValue;
    engine->setModStepShapeAbsolute(stripIndex, modTransformStep, nextSubdivision, endValue);
}

void StripControl::applyModulationCellCurveFromDrag(int deltaY, bool rampUpMode)
{
    auto* engine = processor.getAudioEngine();
    if (!engine || stripIndex >= MlrVSTAudioProcessor::MaxStrips || modTransformStep < 0)
        return;

    const int lengthBars = juce::jlimit(1, ModernAudioEngine::MaxModBars, engine->getModLengthBars(stripIndex));
    const int totalSteps = juce::jmax(ModernAudioEngine::ModSteps, lengthBars * ModernAudioEngine::ModSteps);
    if (modTransformStep >= totalSteps)
        return;

    int subdivisions = modTransformStartSubdivision;
    if (subdivisions <= 1)
    {
        subdivisions = juce::jlimit(
            2,
            ModernAudioEngine::ModMaxStepSubdivisions,
            2 + (std::abs(deltaY) / 14));
    }

    float startValue = modTransformStartValue;
    float endValue = modTransformStartEndValue;
    computeSingleModCellRamp(modTransformStartValue, modTransformStartEndValue, deltaY, rampUpMode, startValue, endValue);
    engine->setModStepValueAbsolute(stripIndex, modTransformStep, startValue);
    engine->setModStepShapeAbsolute(stripIndex, modTransformStep, subdivisions, endValue);
}

void StripControl::mouseDown(const juce::MouseEvent& e)
{
    if (e.originalComponent == &recordButton || e.eventComponent == &recordButton)
    {
        if (e.mods.isRightButtonDown())
            processor.clearRecentAudioBuffer();
        return;
    }

    if (modulationLaneView)
    {
        auto* engine = processor.getAudioEngine();
        if (!engine || stripIndex >= MlrVSTAudioProcessor::MaxStrips)
            return;

        const auto state = engine->getModSequencerState(stripIndex);
        const bool neutralBipolar = modTargetAllowsBipolar(state.target) && state.bipolar;
        const float neutralValue = neutralBipolar ? 0.5f : 0.0f;
        const bool isRearrangeTarget = (state.target == ModernAudioEngine::ModTarget::Rearrange);
        const int lengthBars = juce::jlimit(1, ModernAudioEngine::MaxModBars, engine->getModLengthBars(stripIndex));
        const int totalSteps = juce::jmax(ModernAudioEngine::ModSteps, lengthBars * ModernAudioEngine::ModSteps);

        const auto mods = e.mods;
        const auto modifierGesture = getStepCellModifierGesture(mods);
        const int clickedStep = getModulationStepFromPoint(e.getPosition());
        if (clickedStep >= 0 && modifierGesture != StepCellModifierGesture::None)
        {
            modTransformStartY = e.y;
            modTransformStep = clickedStep;
            modTransformStartValue = juce::jlimit(0.0f, 1.0f, engine->getModStepValueAbsolute(stripIndex, clickedStep));
            modTransformStartSubdivision = juce::jlimit(
                1,
                ModernAudioEngine::ModMaxStepSubdivisions,
                engine->getModStepSubdivisionAbsolute(stripIndex, clickedStep));
            modTransformStartEndValue = juce::jlimit(
                0.0f, 1.0f, engine->getModStepEndValueAbsolute(stripIndex, clickedStep));
            switch (modifierGesture)
            {
                case StepCellModifierGesture::Divide:
                    modTransformMode = ModTransformMode::DuplicateCell;
                    break;
                case StepCellModifierGesture::RampUp:
                    modTransformMode = ModTransformMode::ShapeUpCell;
                    break;
                case StepCellModifierGesture::RampDown:
                    modTransformMode = ModTransformMode::ShapeDownCell;
                    break;
                case StepCellModifierGesture::None:
                default:
                    modTransformMode = ModTransformMode::None;
                    break;
            }

            if (modTransformMode == ModTransformMode::ShapeUpCell)
                applyModulationCellCurveFromDrag(0, true);
            else if (modTransformMode == ModTransformMode::ShapeDownCell)
                applyModulationCellCurveFromDrag(0, false);
            return;
        }

        if (mods.isRightButtonDown() && modifierGesture == StepCellModifierGesture::None)
        {
            for (int i = 0; i < totalSteps; ++i)
                engine->setModStepValueAbsolute(stripIndex, i,
                    isRearrangeTarget ? defaultRearrangeStepValueUi(i) : neutralValue);
            modulationLastDrawStep = -1;
            return;
        }

        modTransformMode = ModTransformMode::None;
        modTransformStep = -1;
        modulationLastDrawStep = -1;
        applyModulationPoint(e.getPosition());
    }
}

void StripControl::mouseDoubleClick(const juce::MouseEvent& e)
{
    if (!modulationLaneView)
        return;

    auto* engine = processor.getAudioEngine();
    if (!engine || stripIndex >= MlrVSTAudioProcessor::MaxStrips)
        return;

    const int step = getModulationStepFromPoint(e.getPosition());
    if (step < 0)
        return;

    const auto state = engine->getModSequencerState(stripIndex);
    const bool neutralBipolar = modTargetAllowsBipolar(state.target) && state.bipolar;
    const float neutralValue = neutralBipolar ? 0.5f : 0.0f;
    engine->setModStepValueAbsolute(stripIndex,
                                    step,
                                    state.target == ModernAudioEngine::ModTarget::Rearrange
                                        ? defaultRearrangeStepValueUi(step)
                                        : neutralValue);
    modulationLastDrawStep = -1;
}

void StripControl::mouseDrag(const juce::MouseEvent& e)
{
    if (modulationLaneView)
    {
        if (modTransformMode != ModTransformMode::None)
        {
            const int deltaY = e.y - modTransformStartY;
            if (modTransformMode == ModTransformMode::DuplicateCell)
                applyModulationCellDuplicateFromDrag(deltaY);
            else if (modTransformMode == ModTransformMode::ShapeUpCell)
                applyModulationCellCurveFromDrag(deltaY, true);
            else if (modTransformMode == ModTransformMode::ShapeDownCell)
                applyModulationCellCurveFromDrag(deltaY, false);
            return;
        }

        applyModulationPoint(e.getPosition());
    }
}

void StripControl::mouseUp(const juce::MouseEvent& e)
{
    modTransformMode = ModTransformMode::None;
    modTransformStep = -1;

    if (!e.mods.isPopupMenu()
        && e.getDistanceFromDragStart() < 4
        && stripSampleNameLabel.getBounds().contains(e.getPosition()))
    {
        loadSample();
    }
}

void StripControl::hideAllPrimaryControls()
{
    auto hide = [](juce::Component& c){ c.setVisible(false); };
    hide(loadButton); hide(pitchMasterButton); hide(pitchSyncButton); hide(pitchNoteBox); hide(transientSliceButton); hide(playModeBox); hide(directionModeBox); hide(groupSelector);
    hide(volumeSlider); hide(panSlider); hide(pitchSlider); hide(speedSlider); hide(scratchSlider); hide(sliceLengthSlider); hide(patternLengthBox); hide(stepLengthReadoutBox);
    hide(stepAttackSlider); hide(stepDecaySlider); hide(stepReleaseSlider);
    hide(tempoLabel); hide(recordBarsBox); hide(recordButton); hide(tempoMatchOverrideBox); hide(pitchControlOverrideBox); hide(recordBarsLabel);
    hide(volumeLabel); hide(panLabel); hide(pitchLabel); hide(speedLabel); hide(scratchLabel); hide(sliceLengthLabel);
    hide(stepAttackLabel); hide(stepDecayLabel); hide(stepReleaseLabel);
    hide(recordLengthLabel);
}

void StripControl::hideAllGrainControls()
{
    auto hide = [](juce::Component& c){ c.setVisible(false); };
    hide(grainSizeSlider); hide(grainDensitySlider); hide(grainPitchSlider); hide(grainPitchJitterSlider);
    hide(grainSpreadSlider); hide(grainJitterSlider); hide(grainPositionJitterSlider); hide(grainRandomSlider); hide(grainArpSlider);
    hide(grainCloudSlider); hide(grainEmitterSlider); hide(grainEnvelopeSlider); hide(grainShapeSlider);
    hide(grainTabPitchButton); hide(grainTabSpaceButton); hide(grainTabShapeButton);
    hide(grainSizeSyncToggle); hide(grainSizeDivLabel); hide(grainSizeLabel);
    hide(grainDensityLabel); hide(grainPitchLabel); hide(grainPitchJitterLabel); hide(grainSpreadLabel);
    hide(grainJitterLabel); hide(grainPositionJitterLabel); hide(grainRandomLabel); hide(grainArpLabel); hide(grainCloudLabel);
    hide(grainEmitterLabel); hide(grainEnvelopeLabel); hide(grainShapeLabel);
}

void StripControl::paintLEDOverlay(juce::Graphics& g)
{
    if (!processor.getAudioEngine()) return;
    
    auto* strip = processor.getAudioEngine()->getStrip(stripIndex);
    if (!strip || !strip->hasAudio()) return;
    
    // Get waveform bounds
    auto wfBounds = waveform.getBounds();
    if (wfBounds.isEmpty() || wfBounds.getWidth() <= 0 || wfBounds.getHeight() <= 0) 
        return;
    
    float colWidth = wfBounds.getWidth() / 16.0f;
    float ledHeight = 10.0f;
    
    // Safety check for valid dimensions
    if (!std::isfinite(colWidth) || colWidth <= 0.0f || ledHeight <= 0.0f)
        return;
    
    int currentCol = strip->getCurrentColumn();
    int loopStart = strip->getLoopStart();
    int loopEnd = strip->getLoopEnd();
    bool isPlaying = strip->isPlaying();
    
    // Draw LED blocks at top of waveform
    for (int x = 0; x < 16; ++x)
    {
        float xPos = wfBounds.getX() + x * colWidth;
        float rectWidth = colWidth - 2.0f;
        
        // Validate rectangle dimensions
        if (!std::isfinite(xPos) || !std::isfinite(rectWidth) || rectWidth <= 0.0f)
            continue;
        
        juce::Rectangle<float> ledRect(xPos + 1.0f, wfBounds.getY() + 1.0f, rectWidth, ledHeight);
        
        // Double-check the rectangle is valid
        if (ledRect.isEmpty() || !ledRect.isFinite())
            continue;
        
        // Determine LED brightness
        juce::Colour ledColor;
        
        if (isPlaying && x == currentCol)
        {
            ledColor = kAccent;
        }
        else if (x >= loopStart && x < loopEnd)
        {
            ledColor = juce::Colour(0xff4f4f4f);
        }
        else
        {
            ledColor = juce::Colour(0xff292929);
        }
        
        g.setColour(ledColor);
        g.fillRoundedRectangle(ledRect, 1.0f);
        
        // Subtle border
        g.setColour(juce::Colour(0xff171717));
        g.drawRoundedRectangle(ledRect, 1.0f, 0.5f);
    }
}

void StripControl::resized()
{
    auto bounds = getLocalBounds().reduced(2);
    
    // Safety check for minimum size
    if (bounds.getWidth() < 50 || bounds.getHeight() < 50)
        return;
    
    const int headerHeight = 19;
    auto labelArea = bounds.removeFromTop(headerHeight);
    auto headerRow = labelArea.reduced(0, 1);
    stripLabel.setBounds(headerRow.removeFromLeft(30));
    identityModeButton.setVisible(true);

    auto* strip = processor.getAudioEngine() != nullptr ? processor.getAudioEngine()->getStrip(stripIndex) : nullptr;
    const auto playMode = strip != nullptr ? strip->getPlayMode() : EnhancedAudioStrip::PlayMode::Loop;
    const bool isStepMode = (playMode == EnhancedAudioStrip::PlayMode::Step);
    const bool isSampleMode = (playMode == EnhancedAudioStrip::PlayMode::Sample);
    const bool isGrainMode = (playMode == EnhancedAudioStrip::PlayMode::Grain);
    showingStepDisplay = isStepMode;
    showingSampleMode = isSampleMode;
    grainOverlayVisible = isGrainMode;

    const bool showPitchIdentity = !isSampleMode && !isStepMode;
    const bool showDirectionIdentity = !isSampleMode;
    const bool showIdentityTiming = showPitchIdentity
        && processor.getLoopPitchRole(stripIndex) == MlrVSTAudioProcessor::LoopPitchRole::Sync;
    const bool sceneModeEnabled = processor.isSceneModeEnabled();

    identityTargetButton.setVisible(showDirectionIdentity);
    identityGroupButton.setVisible(!sceneModeEnabled);
    identityRoleButton.setVisible(showPitchIdentity);
    identityNoteButton.setVisible(showPitchIdentity);
    identityTimingButton.setVisible(showIdentityTiming);

    const int chipGap = 4;
    auto placeChip = [&](juce::Component& component, int width)
    {
        component.setBounds(headerRow.removeFromLeft(juce::jmin(width, headerRow.getWidth())));
        if (headerRow.getWidth() > chipGap)
            headerRow.removeFromLeft(chipGap);
    };

    placeChip(identityModeButton, 54);
    if (showDirectionIdentity)
        placeChip(identityTargetButton, 52);
    else
        identityTargetButton.setBounds({});
    if (!sceneModeEnabled)
        placeChip(identityGroupButton, 38);
    else
        identityGroupButton.setBounds({});
    if (showPitchIdentity)
    {
        placeChip(identityRoleButton, 44);
        placeChip(identityNoteButton, 38);
        if (showIdentityTiming)
            placeChip(identityTimingButton, 44);
        else
            identityTimingButton.setBounds({});
    }
    else
    {
        identityRoleButton.setBounds({});
        identityNoteButton.setBounds({});
        identityTargetButton.setBounds({});
        identityTimingButton.setBounds({});
    }

    const int trimWidth = juce::jlimit(48, 58, headerRow.getWidth() / 5);
    const bool showTrimSlider = headerRow.getWidth() > (trimWidth + 12);
    if (showTrimSlider)
    {
        auto trimArea = headerRow.removeFromLeft(trimWidth);
        trimSlider.setBounds(trimArea.reduced(0, 2));
        if (headerRow.getWidth() > chipGap)
            headerRow.removeFromLeft(chipGap);
    }
    else
    {
        trimSlider.setBounds({});
    }

    stripSampleNameLabel.setBounds(headerRow.reduced(0, 1));
    
    // Main area splits: waveform left, controls right
    auto controlsArea = bounds.removeFromRight(228);
    
    // Waveform OR step display gets all remaining space
    waveform.setBounds(bounds);
    sampleModeComponent.setBounds(bounds);
    stepDisplay.setBounds(bounds);  // Same position, visibility toggled
    modulationLaneBounds = bounds;  // Match waveform/step display exactly
    
    if (modulationLaneView)
    {
        const auto currentModTarget = comboIdToModTarget(modTargetBox.getSelectedId());
        const bool showPitchQuantControls = (currentModTarget == ModernAudioEngine::ModTarget::Pitch);
        const bool showRetriggerHint = (currentModTarget == ModernAudioEngine::ModTarget::Retrigger);
        const bool showRearrangeHint = (currentModTarget == ModernAudioEngine::ModTarget::Rearrange);
        const bool showTargetHint = showRetriggerHint || showRearrangeHint;
        const bool showCurveControls = (modShapeBox.getSelectedId() == 1);
        waveform.setVisible(false);
        stepDisplay.setVisible(false);
        sampleModeComponent.setVisible(false);
        hideAllPrimaryControls();
        hideAllGrainControls();

        modTargetLabel.setVisible(true);
        modTargetBox.setVisible(true);
        modBipolarToggle.setVisible(true);
        modDepthLabel.setVisible(true);
        modDepthSlider.setVisible(true);
        modRateLabel.setVisible(true);
        modRateBox.setVisible(true);
        modTransportLabel.setVisible(true);
        modTransportBox.setVisible(true);
        modOffsetLabel.setVisible(true);
        modOffsetSlider.setVisible(true);
        modCurveBendLabel.setVisible(showCurveControls);
        modCurveBendSlider.setVisible(showCurveControls);
        modLengthLabel.setVisible(true);
        modLengthBox.setVisible(true);
        for (auto& tab : modSequencerTabs)
            tab.setVisible(true);
        modPitchQuantToggle.setVisible(showPitchQuantControls);
        modPitchScaleBox.setVisible(showPitchQuantControls);
        modTargetHintLabel.setVisible(showTargetHint);
        modShapeLabel.setVisible(true);
        modShapeBox.setVisible(true);
        modCurveTypeLabel.setVisible(showCurveControls);
        modCurveTypeBox.setVisible(showCurveControls);

        controlsArea.reduce(4, 0);
        const int gap = 4;
        const int compactGap = 1;
        const int tabRowHeight = 14;
        const int compactRowHeight = 16;
        const int columnWidth = juce::jmax(80, (controlsArea.getWidth() - gap) / 2);
        auto splitRow = [columnWidth](juce::Rectangle<int> row, const int rowGap)
        {
            auto left = row.removeFromLeft(columnWidth);
            row.removeFromLeft(rowGap);
            return std::pair<juce::Rectangle<int>, juce::Rectangle<int>>(left, row);
        };

        auto tabRow = controlsArea.removeFromTop(tabRowHeight);
        const int tabGap = 2;
        const int tabWidth = juce::jmax(1, (tabRow.getWidth() - ((ModernAudioEngine::NumModSequencers - 1) * tabGap))
                                             / ModernAudioEngine::NumModSequencers);
        for (int slot = 0; slot < ModernAudioEngine::NumModSequencers; ++slot)
        {
            modSequencerTabs[static_cast<size_t>(slot)].setBounds(tabRow.removeFromLeft(tabWidth));
            if (slot < (ModernAudioEngine::NumModSequencers - 1))
                tabRow.removeFromLeft(tabGap);
        }
        controlsArea.removeFromTop(compactGap);

        auto row0 = controlsArea.removeFromTop(compactRowHeight);
        auto cols0 = splitRow(row0, gap);
        auto row0Left = cols0.first;
        modTargetLabel.setBounds(row0Left.removeFromLeft(36));
        modTargetBox.setBounds(row0Left);
        modLengthLabel.setBounds(cols0.second.removeFromLeft(22));
        modLengthBox.setBounds(cols0.second.removeFromLeft(60));

        controlsArea.removeFromTop(compactGap);
        auto row1 = controlsArea.removeFromTop(compactRowHeight);
        auto cols1 = splitRow(row1, gap);
        auto row1Left = cols1.first;
        modDepthLabel.setBounds(row1Left.removeFromLeft(36));
        modDepthSlider.setBounds(row1Left);
        modBipolarToggle.setBounds(cols1.second);

        controlsArea.removeFromTop(compactGap);
        auto row2 = controlsArea.removeFromTop(compactRowHeight);
        auto cols2 = splitRow(row2, gap);
        auto row2Left = cols2.first;
        modRateLabel.setBounds(row2Left.removeFromLeft(32));
        modRateBox.setBounds(row2Left);
        modTransportLabel.setBounds(cols2.second.removeFromLeft(36));
        modTransportBox.setBounds(cols2.second);

        controlsArea.removeFromTop(compactGap);
        auto row3 = controlsArea.removeFromTop(compactRowHeight);
        auto cols3 = splitRow(row3, gap);
        auto row3Left = cols3.first;
        modOffsetLabel.setBounds(row3Left.removeFromLeft(32));
        modOffsetSlider.setBounds(row3Left);
        modShapeLabel.setBounds(cols3.second.removeFromLeft(30));
        modShapeBox.setBounds(cols3.second);

        if (showCurveControls)
        {
            controlsArea.removeFromTop(compactGap);
            auto row4 = controlsArea.removeFromTop(compactRowHeight);
            auto cols4 = splitRow(row4, gap);
            modCurveBendLabel.setBounds(cols4.first.removeFromLeft(30));
            modCurveBendSlider.setBounds(cols4.first);
            modCurveTypeLabel.setBounds(cols4.second.removeFromLeft(30));
            modCurveTypeBox.setBounds(cols4.second);
        }
        else
        {
            modCurveBendLabel.setBounds({});
            modCurveBendSlider.setBounds({});
            modCurveTypeLabel.setBounds({});
            modCurveTypeBox.setBounds({});
        }

        if (showPitchQuantControls || showTargetHint)
        {
            controlsArea.removeFromTop(compactGap);
            auto row5 = controlsArea.removeFromTop(compactRowHeight);
            if (showPitchQuantControls)
            {
                auto cols5 = splitRow(row5, gap);
                modPitchQuantToggle.setBounds(cols5.first);
                modPitchScaleBox.setBounds(cols5.second);
                modTargetHintLabel.setBounds({});
            }
            else
            {
                modPitchQuantToggle.setBounds({});
                modPitchScaleBox.setBounds({});
                modTargetHintLabel.setBounds(row5);
            }
        }
        else
        {
            modPitchQuantToggle.setBounds({});
            modPitchScaleBox.setBounds({});
            modTargetHintLabel.setBounds({});
        }
        return;
    }

    loadButton.setVisible(true);
    pitchMasterButton.setVisible(false);
    pitchSyncButton.setVisible(false);
    transientSliceButton.setVisible(!isSampleMode);
    playModeBox.setVisible(false);
    directionModeBox.setVisible(false);
    groupSelector.setVisible(false);
    modTargetLabel.setVisible(false);
    modTargetBox.setVisible(false);
    modBipolarToggle.setVisible(false);
    modDepthLabel.setVisible(false);
    modDepthSlider.setVisible(false);
    modRateLabel.setVisible(false);
    modRateBox.setVisible(false);
    modTransportLabel.setVisible(false);
    modTransportBox.setVisible(false);
    modOffsetLabel.setVisible(false);
    modOffsetSlider.setVisible(false);
    modCurveBendLabel.setVisible(false);
    modCurveBendSlider.setVisible(false);
    modLengthLabel.setVisible(false);
    modLengthBox.setVisible(false);
    for (auto& tab : modSequencerTabs)
        tab.setVisible(false);
    modPitchQuantToggle.setVisible(false);
    modPitchScaleBox.setVisible(false);
    modTargetHintLabel.setVisible(false);
    modShapeLabel.setVisible(false);
    modShapeBox.setVisible(false);
    modCurveTypeLabel.setVisible(false);
    modCurveTypeBox.setVisible(false);
    
    // Controls column on right
    controlsArea.reduce(4, 0);
    
    patternLengthBox.setBounds({});
    stepLengthReadoutBox.setBounds({});

    const int rowGap = isGrainMode ? 0 : 1;
    loopPitchProgressBounds = {};

    // Top row: Load + slice mode
    const int topRowHeight = isGrainMode ? 14 : 18;
    auto topRow = controlsArea.removeFromTop(topRowHeight);
    if (isSampleMode)
    {
        loadButton.setBounds(topRow.reduced(0, 0));
        transientSliceButton.setBounds({});
    }
    else
    {
        auto loadArea = topRow.removeFromLeft(juce::jmax(42, (topRow.getWidth() * 3) / 5));
        loadButton.setBounds(loadArea.reduced(0, 0));
        topRow.removeFromLeft(2);
        transientSliceButton.setBounds(topRow);
    }
    pitchMasterButton.setBounds({});
    pitchSyncButton.setBounds({});
    pitchNoteBox.setBounds({});
    controlsArea.removeFromTop(rowGap);
    loopPitchProgressBounds = {};
    
    // Playback mode, direction, and group are controlled in the header line.
    playModeBox.setBounds({});
    directionModeBox.setBounds({});
    groupSelector.setBounds({});

    if (isGrainMode)
    {
        tempoLabel.setVisible(false);
        recordBarsBox.setVisible(false);
        recordButton.setVisible(false);
        tempoMatchOverrideBox.setVisible(false);
        pitchControlOverrideBox.setVisible(false);
        recordBarsLabel.setVisible(false);
        tempoLabel.setBounds({});
        recordBarsBox.setBounds({});
        recordButton.setBounds({});
        tempoMatchOverrideBox.setBounds({});
        pitchControlOverrideBox.setBounds({});
        recordBarsLabel.setBounds({});
        patternLengthBox.setVisible(false);
        stepLengthReadoutBox.setVisible(false);
        stepAttackSlider.setVisible(false);
        stepDecaySlider.setVisible(false);
        stepReleaseSlider.setVisible(false);
        stepAttackLabel.setVisible(false);
        stepDecayLabel.setVisible(false);
        stepReleaseLabel.setVisible(false);
        patternLengthBox.setBounds({});
        stepLengthReadoutBox.setBounds({});
        stepAttackSlider.setBounds({});
        stepDecaySlider.setBounds({});
        stepReleaseSlider.setBounds({});
        stepAttackLabel.setBounds({});
        stepDecayLabel.setBounds({});
        stepReleaseLabel.setBounds({});
        pitchNoteBox.setVisible(false);
        pitchNoteBox.setBounds({});
        recordLengthLabel.setVisible(false);
        recordLengthLabel.setBounds({});

        const int grainTabRowHeight = 13;
        const int grainTabGap = 3;
        const int preferredGrainTabWidth = 58;
        auto tabRow = controlsArea.removeFromTop(grainTabRowHeight);
        const int minClusterWidth = (3 * preferredGrainTabWidth) + (2 * grainTabGap);
        const int grainTabWidth = tabRow.getWidth() >= minClusterWidth
            ? preferredGrainTabWidth
            : juce::jmax(1, (tabRow.getWidth() - (2 * grainTabGap)) / 3);
        const int tabClusterWidth = (3 * grainTabWidth) + (2 * grainTabGap);
        if (tabRow.getWidth() > tabClusterWidth)
            tabRow.removeFromLeft((tabRow.getWidth() - tabClusterWidth) / 2);
        grainTabPitchButton.setBounds(tabRow.removeFromLeft(grainTabWidth));
        tabRow.removeFromLeft(grainTabGap);
        grainTabSpaceButton.setBounds(tabRow.removeFromLeft(grainTabWidth));
        tabRow.removeFromLeft(grainTabGap);
        grainTabShapeButton.setBounds(tabRow.removeFromLeft(grainTabWidth));

        const int grainSectionGap = 2;
        if (controlsArea.getHeight() > grainSectionGap)
            controlsArea.removeFromTop(grainSectionGap);

        const bool showEnginePage = grainSubPage == GrainSubPage::Pitch;
        if (showEnginePage)
        {
            const int engineRowHeight = 13;
            const int engineRowGap = 1;
            const int engineLabelW = 34;
            const int syncToggleW = 14;
            const int syncModeW = 30;

            auto sizeRow = controlsArea.removeFromTop(engineRowHeight);
            grainSizeLabel.setBounds(sizeRow.removeFromLeft(engineLabelW));
            if (sizeRow.getWidth() > 4)
                sizeRow.removeFromLeft(4);
            const int sliderW = juce::jmax(8, sizeRow.getWidth() - syncToggleW - syncModeW - 4);
            grainSizeSlider.setBounds(sizeRow.removeFromLeft(sliderW));
            if (sizeRow.getWidth() > 2)
                sizeRow.removeFromLeft(2);
            grainSizeSyncToggle.setBounds(sizeRow.removeFromLeft(syncToggleW));
            if (sizeRow.getWidth() > 2)
                sizeRow.removeFromLeft(2);
            grainSizeDivLabel.setBounds(sizeRow);

            if (controlsArea.getHeight() > engineRowGap)
                controlsArea.removeFromTop(engineRowGap);

            auto densityRow = controlsArea.removeFromTop(engineRowHeight);
            grainDensityLabel.setBounds(densityRow.removeFromLeft(engineLabelW));
            if (densityRow.getWidth() > 4)
                densityRow.removeFromLeft(4);
            grainDensitySlider.setBounds(densityRow);

            if (controlsArea.getHeight() > engineRowGap)
                controlsArea.removeFromTop(engineRowGap);

            auto speedRow = controlsArea.removeFromTop(engineRowHeight);
            speedLabel.setBounds(speedRow.removeFromLeft(engineLabelW));
            if (speedRow.getWidth() > 4)
                speedRow.removeFromLeft(4);
            speedSlider.setBounds(speedRow);

            if (controlsArea.getHeight() > engineRowGap)
                controlsArea.removeFromTop(engineRowGap);
        }
        else
        {
            grainSizeSlider.setBounds({});
            grainDensitySlider.setBounds({});
            grainSizeSyncToggle.setBounds({});
            grainSizeDivLabel.setBounds({});
            grainSizeLabel.setBounds({});
            grainDensityLabel.setBounds({});
            speedSlider.setBounds({});
            speedLabel.setBounds({});
        }

        const int rowGapMini = 1;
        const int totalMiniRows = (grainSubPage == GrainSubPage::Space) ? 3 : 2;
        const int minMiniRowH = 9;
        const int maxMiniRowH = 20;
        const int rowH = juce::jlimit(minMiniRowH,
                                      maxMiniRowH,
                                      (controlsArea.getHeight() - ((totalMiniRows - 1) * rowGapMini)) / totalMiniRows);
        const int miniLabelW = 34;
        auto layoutGrainMiniRow = [&](juce::Label& labelA, juce::Slider& sliderA,
                                      juce::Label& labelB, juce::Slider& sliderB)
        {
            if (controlsArea.getHeight() <= 0)
                return;
            auto row = controlsArea.removeFromTop(rowH);
            auto left = row.removeFromLeft(row.getWidth() / 2);
            labelA.setBounds(left.removeFromLeft(miniLabelW));
            sliderA.setBounds(left);
            row.removeFromLeft(2);
            labelB.setBounds(row.removeFromLeft(miniLabelW));
            sliderB.setBounds(row);
            if (controlsArea.getHeight() > rowGapMini)
                controlsArea.removeFromTop(rowGapMini);
        };
        auto layoutGrainSingleRow = [&](juce::Label& label, juce::Slider& slider)
        {
            if (controlsArea.getHeight() <= 0)
                return;
            auto row = controlsArea.removeFromTop(rowH);
            label.setBounds(row.removeFromLeft(miniLabelW));
            slider.setBounds(row);
            if (controlsArea.getHeight() > rowGapMini)
                controlsArea.removeFromTop(rowGapMini);
        };

        if (grainSubPage == GrainSubPage::Pitch)
        {
            layoutGrainMiniRow(grainPitchLabel, grainPitchSlider, grainPitchJitterLabel, grainPitchJitterSlider);
            layoutGrainMiniRow(grainArpLabel, grainArpSlider, grainRandomLabel, grainRandomSlider);
        }
        else if (grainSubPage == GrainSubPage::Space)
        {
            layoutGrainMiniRow(grainSpreadLabel, grainSpreadSlider, grainJitterLabel, grainJitterSlider);
            layoutGrainMiniRow(grainCloudLabel, grainCloudSlider, grainEmitterLabel, grainEmitterSlider);
            layoutGrainSingleRow(grainPositionJitterLabel, grainPositionJitterSlider);
        }
        else
        {
            layoutGrainMiniRow(grainEnvelopeLabel, grainEnvelopeSlider, grainShapeLabel, grainShapeSlider);
        }
        return;
    }
    
    // Check if we have enough height for compact transport + record controls.
    const int requiredTopControlsHeight = 22 + 2 + 20 + 2 + 30 + 10 + 10;
    bool showTempoControls = (!isGrainMode) && !isSampleMode && (controlsArea.getHeight() >= requiredTopControlsHeight);

    // Per-strip tempo-match override only appears on the standard Loop page so
    // Gate / One-shot / Step / Grain layouts stay unchanged.
    const bool isLoopMode = (playMode == EnhancedAudioStrip::PlayMode::Loop);

    // Update visibility
    const bool showRecordBars = (!isGrainMode) && !isSampleMode && controlsArea.getHeight() >= 18;
    const bool showTempoMatchOverride = showRecordBars && isLoopMode;
    const bool showPitchControlOverride = showRecordBars && !isStepMode;
    tempoLabel.setVisible(showTempoControls);
    recordBarsBox.setVisible(showRecordBars);
    recordButton.setVisible(showRecordBars);
    tempoMatchOverrideBox.setVisible(showTempoMatchOverride);
    pitchControlOverrideBox.setVisible(showPitchControlOverride);
    pitchNoteBox.setVisible(false);
    recordBarsLabel.setVisible(false);

    // Tempo controls row - only if we have space
    if (showTempoControls)
    {
        auto tempoRow = controlsArea.removeFromTop(22);
        tempoLabel.setBounds(tempoRow.removeFromLeft(44));
        controlsArea.removeFromTop(2);

        auto recBarsRow = controlsArea.removeFromTop(18);
        recordBarsBox.setBounds(recBarsRow.removeFromLeft(70));
        recBarsRow.removeFromLeft(8);
        recordButton.setBounds(recBarsRow.removeFromLeft(46));
        if (showTempoMatchOverride || showPitchControlOverride)
        {
            recBarsRow.removeFromLeft(6);
            const int comboGap = (showTempoMatchOverride && showPitchControlOverride) ? 4 : 0;
            const int available = juce::jmax(0, recBarsRow.getWidth());
            const int tempoWidth = showTempoMatchOverride
                ? juce::jmin(46, juce::jmax(30, (available - comboGap) / (showPitchControlOverride ? 2 : 1)))
                : 0;
            const int pitchWidth = showPitchControlOverride
                ? juce::jmin(52, juce::jmax(30, available - tempoWidth - comboGap))
                : 0;

            if (showTempoMatchOverride)
                tempoMatchOverrideBox.setBounds(recBarsRow.removeFromLeft(juce::jmin(tempoWidth, recBarsRow.getWidth())));
            else
                tempoMatchOverrideBox.setBounds({});

            if (showTempoMatchOverride && showPitchControlOverride && recBarsRow.getWidth() > 0)
                recBarsRow.removeFromLeft(juce::jmin(comboGap, recBarsRow.getWidth()));

            if (showPitchControlOverride)
                pitchControlOverrideBox.setBounds(recBarsRow.removeFromLeft(juce::jmin(pitchWidth, recBarsRow.getWidth())));
            else
                pitchControlOverrideBox.setBounds({});
        }
        else
        {
            tempoMatchOverrideBox.setBounds({});
            pitchControlOverrideBox.setBounds({});
        }
        pitchNoteBox.setBounds({});
        if (isStepMode)
        {
            recBarsRow.removeFromLeft(4);
            const int lenWidth = juce::jlimit(18, 26, recBarsRow.getWidth());
            patternLengthBox.setBounds(recBarsRow.removeFromLeft(lenWidth));
            if (recBarsRow.getWidth() > 0)
            {
                recBarsRow.removeFromLeft(3);
                const int readoutWidth = juce::jlimit(34, 62, recBarsRow.getWidth());
                stepLengthReadoutBox.setBounds(recBarsRow.removeFromLeft(readoutWidth));
            }
        }
        controlsArea.removeFromTop(2);
    }
    else if (showRecordBars)
    {
        auto recBarsRow = controlsArea.removeFromTop(16);
        recordBarsBox.setBounds(recBarsRow.removeFromLeft(66));
        recBarsRow.removeFromLeft(8);
        recordButton.setBounds(recBarsRow.removeFromLeft(42));
        if (showTempoMatchOverride || showPitchControlOverride)
        {
            recBarsRow.removeFromLeft(6);
            const int comboGap = (showTempoMatchOverride && showPitchControlOverride) ? 4 : 0;
            const int available = juce::jmax(0, recBarsRow.getWidth());
            const int tempoWidth = showTempoMatchOverride
                ? juce::jmin(44, juce::jmax(28, (available - comboGap) / (showPitchControlOverride ? 2 : 1)))
                : 0;
            const int pitchWidth = showPitchControlOverride
                ? juce::jmin(50, juce::jmax(28, available - tempoWidth - comboGap))
                : 0;

            if (showTempoMatchOverride)
                tempoMatchOverrideBox.setBounds(recBarsRow.removeFromLeft(juce::jmin(tempoWidth, recBarsRow.getWidth())));
            else
                tempoMatchOverrideBox.setBounds({});

            if (showTempoMatchOverride && showPitchControlOverride && recBarsRow.getWidth() > 0)
                recBarsRow.removeFromLeft(juce::jmin(comboGap, recBarsRow.getWidth()));

            if (showPitchControlOverride)
                pitchControlOverrideBox.setBounds(recBarsRow.removeFromLeft(juce::jmin(pitchWidth, recBarsRow.getWidth())));
            else
                pitchControlOverrideBox.setBounds({});
        }
        else
        {
            tempoMatchOverrideBox.setBounds({});
            pitchControlOverrideBox.setBounds({});
        }
        pitchNoteBox.setBounds({});
        if (isStepMode)
        {
            recBarsRow.removeFromLeft(4);
            const int lenWidth = juce::jlimit(18, 24, recBarsRow.getWidth());
            patternLengthBox.setBounds(recBarsRow.removeFromLeft(lenWidth));
            if (recBarsRow.getWidth() > 0)
            {
                recBarsRow.removeFromLeft(3);
                const int readoutWidth = juce::jlimit(32, 58, recBarsRow.getWidth());
                stepLengthReadoutBox.setBounds(recBarsRow.removeFromLeft(readoutWidth));
            }
        }
        controlsArea.removeFromTop(2);
    }
    else
    {
        tempoMatchOverrideBox.setBounds({});
        pitchControlOverrideBox.setBounds({});
        pitchNoteBox.setBounds({});
        if (isStepMode && controlsArea.getHeight() >= 14)
        {
            auto lenRow = controlsArea.removeFromTop(16);
            const int lenWidth = juce::jlimit(18, 24, lenRow.getWidth());
            patternLengthBox.setBounds(lenRow.removeFromLeft(lenWidth));
            if (lenRow.getWidth() > 0)
            {
                lenRow.removeFromLeft(3);
                const int readoutWidth = juce::jlimit(32, 58, lenRow.getWidth());
                stepLengthReadoutBox.setBounds(lenRow.removeFromLeft(readoutWidth));
            }
            controlsArea.removeFromTop(2);
        }
    }
    
    // Rotary knobs row.
    const int knobsRowHeight = 30;
    auto knobsRow = controlsArea.removeFromTop(knobsRowHeight);

    if (isStepMode)
    {
        const int stepGap = 2;
        const int stepKnobWidth = juce::jmax(8, (knobsRow.getWidth() - (4 * stepGap)) / 5);
        volumeSlider.setBounds(knobsRow.removeFromLeft(stepKnobWidth).reduced(1));
        knobsRow.removeFromLeft(stepGap);
        panSlider.setBounds(knobsRow.removeFromLeft(stepKnobWidth).reduced(1));
        knobsRow.removeFromLeft(stepGap);
        stepAttackSlider.setBounds(knobsRow.removeFromLeft(stepKnobWidth).reduced(1));
        knobsRow.removeFromLeft(stepGap);
        stepDecaySlider.setBounds(knobsRow.removeFromLeft(stepKnobWidth).reduced(1));
        knobsRow.removeFromLeft(stepGap);
        stepReleaseSlider.setBounds(knobsRow.reduced(1));
    }
    else
    {
        std::array<juce::Slider*, 6> visibleKnobs{};
        int visibleCount = 0;
        auto addVisibleKnob = [&](juce::Slider& knob)
        {
            if (knob.isVisible() && visibleCount < static_cast<int>(visibleKnobs.size()))
                visibleKnobs[static_cast<size_t>(visibleCount++)] = &knob;
        };
        addVisibleKnob(volumeSlider);
        addVisibleKnob(panSlider);
        addVisibleKnob(pitchSlider);
        addVisibleKnob(speedSlider);
        addVisibleKnob(scratchSlider);
        addVisibleKnob(sliceLengthSlider);

        const bool denseLayout = visibleCount >= 6;
        const int knobGap = denseLayout ? 1 : 2;
        const int totalGap = knobGap * juce::jmax(0, visibleCount - 1);
        const int knobWidth = juce::jmax(8, (knobsRow.getWidth() - totalGap) / juce::jmax(1, visibleCount));
        for (int i = 0; i < visibleCount; ++i)
        {
            if (auto* knob = visibleKnobs[static_cast<size_t>(i)])
                knob->setBounds(knobsRow.removeFromLeft(knobWidth).reduced(denseLayout ? 2 : 1));
            if (i < (visibleCount - 1))
                knobsRow.removeFromLeft(knobGap);
        }
    }

    const int labelsRowHeight = 9;
    auto labelsRow = controlsArea.removeFromTop(labelsRowHeight);
    if (isStepMode)
    {
        const int stepGap = 2;
        const int stepLabelWidth = juce::jmax(8, (labelsRow.getWidth() - (4 * stepGap)) / 5);
        volumeLabel.setBounds(labelsRow.removeFromLeft(stepLabelWidth));
        labelsRow.removeFromLeft(stepGap);
        panLabel.setBounds(labelsRow.removeFromLeft(stepLabelWidth));
        labelsRow.removeFromLeft(stepGap);
        stepAttackLabel.setBounds(labelsRow.removeFromLeft(stepLabelWidth));
        labelsRow.removeFromLeft(stepGap);
        stepDecayLabel.setBounds(labelsRow.removeFromLeft(stepLabelWidth));
        labelsRow.removeFromLeft(stepGap);
        stepReleaseLabel.setBounds(labelsRow);
    }
    else
    {
        std::array<juce::Label*, 6> visibleLabels{};
        int visibleCount = 0;
        auto addVisibleLabel = [&](juce::Label& label)
        {
            if (label.isVisible() && visibleCount < static_cast<int>(visibleLabels.size()))
                visibleLabels[static_cast<size_t>(visibleCount++)] = &label;
        };
        addVisibleLabel(volumeLabel);
        addVisibleLabel(panLabel);
        addVisibleLabel(pitchLabel);
        addVisibleLabel(speedLabel);
        addVisibleLabel(scratchLabel);
        addVisibleLabel(sliceLengthLabel);

        const int labelGap = 2;
        const int totalGap = labelGap * juce::jmax(0, visibleCount - 1);
        const int labelWidth = juce::jmax(8, (labelsRow.getWidth() - totalGap) / juce::jmax(1, visibleCount));
        for (int i = 0; i < visibleCount; ++i)
        {
            if (auto* label = visibleLabels[static_cast<size_t>(i)])
                label->setBounds(labelsRow.removeFromLeft(labelWidth));
            if (i < (visibleCount - 1))
                labelsRow.removeFromLeft(labelGap);
        }
    }
    if (controlsArea.getHeight() >= 10)
    {
        // Recording loop length label (small, at bottom)
        recordLengthLabel.setBounds(controlsArea.removeFromTop(10));
    }
}


void StripControl::loadSample()
{
    // Get current play mode to determine which path to use
    auto* strip = processor.getAudioEngine()->getStrip(stripIndex);
    bool isStepMode = (strip && strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Step);
    bool isFlipMode = (strip && strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Sample);
    auto mode = isStepMode ? MlrVSTAudioProcessor::SamplePathMode::Step
                           : (isFlipMode ? MlrVSTAudioProcessor::SamplePathMode::Flip
                                         : MlrVSTAudioProcessor::SamplePathMode::Loop);
    juce::File startingDirectory = processor.getCurrentBrowserDirectoryForStrip(stripIndex, mode);
    
    juce::FileChooser chooser("Load Sample", startingDirectory, "*.wav;*.aif;*.aiff;*.mp3;*.ogg;*.flac");
    
    if (chooser.browseForFileToOpen())
    {
        const auto selectedFile = chooser.getResult();
        const auto selectedDirectory = selectedFile.getParentDirectory();
        if (selectedDirectory != juce::File())
            processor.setCurrentBrowserDirectoryForStrip(stripIndex, mode, selectedDirectory);
        loadSampleFromFile(selectedFile);
    }
}

bool StripControl::isSupportedAudioFile(const juce::File& file)
{
    if (!file.existsAsFile())
        return false;

    return file.hasFileExtension(".wav;.aif;.aiff;.mp3;.ogg;.flac");
}

void StripControl::loadSampleFromFile(const juce::File& file)
{
    if (!isSupportedAudioFile(file))
        return;

    processor.loadSampleToStripPreservingPlaybackState(stripIndex, file);
}

bool StripControl::isInterestedInFileDrag(const juce::StringArray& files)
{
    for (const auto& path : files)
    {
        if (isSupportedAudioFile(juce::File(path)))
            return true;
    }
    return false;
}

void StripControl::filesDropped(const juce::StringArray& files, int /*x*/, int /*y*/)
{
    for (const auto& path : files)
    {
        juce::File file(path);
        if (isSupportedAudioFile(file))
        {
            loadSampleFromFile(file);
            break;
        }
    }
}

void StripControl::updateFromEngine()
{
    if (!processor.getAudioEngine()) return;
    
    auto* strip = processor.getAudioEngine()->getStrip(stripIndex);
    if (!strip) return;
    const auto modState = processor.getAudioEngine()->getModSequencerState(stripIndex);
    const auto modulates = [&](ModernAudioEngine::ModTarget t)
    {
        return modState.target == t;
    };

    if (modulationLaneView)
    {
        const auto mod = processor.getAudioEngine()->getModSequencerState(stripIndex);
        const bool showPitchQuantControls = (mod.target == ModernAudioEngine::ModTarget::Pitch);
        const bool showRetriggerHint = (mod.target == ModernAudioEngine::ModTarget::Retrigger);
        const bool showRearrangeHint = (mod.target == ModernAudioEngine::ModTarget::Rearrange);
        const bool showTargetHint = showRetriggerHint || showRearrangeHint;
        const bool showCurveControls = mod.curveMode;
        const bool targetUiChanged = (modPitchQuantToggle.isVisible() != showPitchQuantControls)
            || (modPitchScaleBox.isVisible() != showPitchQuantControls)
            || (modTargetHintLabel.isVisible() != showTargetHint)
            || (modCurveBendSlider.isVisible() != showCurveControls)
            || (modCurveTypeBox.isVisible() != showCurveControls);
        modTargetBox.setSelectedId(modTargetToComboId(mod.target), juce::dontSendNotification);
        modBipolarToggle.setToggleState(mod.bipolar, juce::dontSendNotification);
        modBipolarToggle.setEnabled(modTargetAllowsBipolar(mod.target));
        modDepthSlider.setValue(mod.depth, juce::dontSendNotification);
        modRateBox.setSelectedId(modRateToComboId(mod.rate), juce::dontSendNotification);
        modTransportBox.setSelectedId(mod.transportMode + 1, juce::dontSendNotification);
        modOffsetSlider.setValue(mod.smoothingMs, juce::dontSendNotification);
        modCurveBendSlider.setValue(mod.curveBend, juce::dontSendNotification);
        modLengthBox.setSelectedId(mod.lengthBars, juce::dontSendNotification);
        modPitchQuantToggle.setToggleState(mod.pitchScaleQuantize, juce::dontSendNotification);
        modPitchScaleBox.setSelectedId(pitchScaleToComboId(static_cast<ModernAudioEngine::PitchScale>(mod.pitchScale)), juce::dontSendNotification);
        modPitchScaleBox.setEnabled(showPitchQuantControls && mod.pitchScaleQuantize);
        modPitchQuantToggle.setVisible(showPitchQuantControls);
        modPitchScaleBox.setVisible(showPitchQuantControls);
        modTargetHintLabel.setVisible(showTargetHint);
        if (showRetriggerHint)
        {
            const int activeStep = juce::jlimit(
                0,
                ModernAudioEngine::ModTotalSteps - 1,
                processor.getAudioEngine()->getModCurrentGlobalStep(stripIndex));
            const float activeRaw = juce::jlimit(0.0f, 1.0f,
                processor.getAudioEngine()->getModStepValueAbsolute(stripIndex, activeStep));
            modTargetHintLabel.setText(makeRetriggerHintText(activeRaw, mod.depth), juce::dontSendNotification);
        }
        else if (showRearrangeHint)
        {
            const int activeStep = juce::jlimit(
                0,
                ModernAudioEngine::ModTotalSteps - 1,
                processor.getAudioEngine()->getModCurrentGlobalStep(stripIndex));
            const float activeRaw = juce::jlimit(0.0f, 1.0f,
                processor.getAudioEngine()->getModStepValueAbsolute(stripIndex, activeStep));
            modTargetHintLabel.setText(makeRearrangeHintText(activeRaw), juce::dontSendNotification);
        }
        modShapeBox.setSelectedId(mod.curveMode ? 1 : 2, juce::dontSendNotification);
        modCurveTypeBox.setSelectedId(curveShapeToComboId(static_cast<ModernAudioEngine::ModCurveShape>(mod.curveShape)),
                                      juce::dontSendNotification);
        modCurveBendSlider.setEnabled(mod.curveMode);
        modCurveTypeBox.setEnabled(mod.curveMode);
        if (targetUiChanged)
            resized();
        updateModSequencerTabButtons();
        repaint();
        return;
    }

    const auto playMode = strip->getPlayMode();
    const bool isStepMode = (playMode == EnhancedAudioStrip::PlayMode::Step);
    const bool isSampleMode = (playMode == EnhancedAudioStrip::PlayMode::Sample);
    const bool isGrainMode = (playMode == EnhancedAudioStrip::PlayMode::Grain);
    if (showingStepDisplay != isStepMode
        || showingSampleMode != isSampleMode
        || grainOverlayVisible != isGrainMode)
    {
        showingStepDisplay = isStepMode;
        showingSampleMode = isSampleMode;
        if (isSampleMode)
            sampleModeComponent.setEngine(processor.getSampleModeEngine(stripIndex, true));
        else
            sampleModeComponent.setEngine(processor.getSampleModeEngine(stripIndex, false));
        waveform.setVisible(!isStepMode && !isSampleMode);
        stepDisplay.setVisible(isStepMode);
        sampleModeComponent.setVisible(isSampleMode);
        patternLengthBox.setVisible(isStepMode);
        stepLengthReadoutBox.setVisible(isStepMode);
        updateGrainOverlayVisibility();
        resized();
    }
    
    // Update step display if in step mode
    if (showingStepDisplay)
    {
        stepDisplay.setStepPattern(strip->stepPattern, strip->getStepTotalSteps());
        stepDisplay.setStepSubdivisions(strip->stepSubdivisions);
        stepDisplay.setStepSubdivisionVelocityRange(strip->stepSubdivisionStartVelocity,
                                                    strip->stepSubdivisionRepeatVelocity);
        stepDisplay.setStepProbability(strip->stepProbability);
        stepDisplay.setCurrentStep(strip->currentStep);
        stepDisplay.setPlaying(strip->isPlaying());

        if (processor.isStepEditModeActive())
        {
            StepSequencerDisplay::EditTool mappedTool = StepSequencerDisplay::EditTool::Volume;
            switch (processor.getStepEditToolIndex())
            {
                case 1: mappedTool = StepSequencerDisplay::EditTool::Volume; break;
                case 2: mappedTool = StepSequencerDisplay::EditTool::Divide; break;
                case 3: mappedTool = StepSequencerDisplay::EditTool::RampUp; break;
                case 4: mappedTool = StepSequencerDisplay::EditTool::RampDown; break;
                case 5: mappedTool = StepSequencerDisplay::EditTool::Probability; break;
                case 0:
                case 6:
                case 7:
                case 8:
                default: mappedTool = StepSequencerDisplay::EditTool::Volume; break;
            }

            if (stepDisplay.getActiveTool() != mappedTool)
                stepDisplay.setActiveTool(mappedTool);
        }

        // No playback position indicator in step mode - just show steps
    }
    
    // Update waveform display (only if visible - i.e., not in step mode)
    if (!showingStepDisplay && !showingSampleMode && strip->hasAudio())
    {
        auto* buffer = strip->getAudioBuffer();
        if (buffer && buffer->getNumSamples() > 0)
        {
            waveform.setAudioBuffer(*buffer, strip->getSourceSampleRate());
            waveform.setLoopPoints(strip->getLoopStart(), strip->getLoopEnd(), 16);
            waveform.setSliceMarkers(strip->getSliceStartSamples(false),
                                     strip->getSliceStartSamples(true),
                                     buffer->getNumSamples(),
                                     strip->isTransientSliceMode());
            
            if (strip->isPlaying() || strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Grain)
            {
                double playbackPos = strip->getPlaybackPosition();
                double numSamples = static_cast<double>(buffer->getNumSamples());
                
                // Safety check to prevent division by zero or NaN
                if (numSamples > 0 && std::isfinite(playbackPos))
                {
                    double wrappedPos = std::fmod(playbackPos, numSamples);
                    if (wrappedPos < 0.0)
                        wrappedPos += numSamples;
                    double normalized = wrappedPos / numSamples;
                    waveform.setPlaybackPosition(normalized);
                }
            }

            double grainWindowNorm = 0.0;
            if (isGrainMode && buffer->getNumSamples() > 0 && strip->getSourceSampleRate() > 0.0)
            {
                double sizeMsForDisplay = static_cast<double>(strip->getGrainSizeMs());
                const double hostTempo = juce::jmax(1.0, processor.getAudioEngine()->getCurrentTempo());
                static constexpr std::array<double, 13> sizeDivisionsBeats {
                    1.0 / 64.0, 1.0 / 48.0, 1.0 / 32.0, 1.0 / 24.0, 1.0 / 16.0,
                    1.0 / 12.0, 1.0 / 8.0, 1.0 / 6.0, 1.0 / 4.0, 1.0 / 3.0,
                    1.0 / 2.0, 1.0, 2.0
                };
                const double t = juce::jlimit(0.0, 1.0, (sizeMsForDisplay - 5.0) / (2400.0 - 5.0));
                const int idx = juce::jlimit(0, static_cast<int>(sizeDivisionsBeats.size()) - 1,
                                             static_cast<int>(std::round(t * static_cast<double>(sizeDivisionsBeats.size() - 1))));
                if (strip->isGrainTempoSyncEnabled())
                    sizeMsForDisplay = sizeDivisionsBeats[static_cast<size_t>(idx)] * (60.0 / hostTempo) * 1000.0;
                const double sizeSamples = (sizeMsForDisplay * 0.001) * strip->getSourceSampleRate();
                grainWindowNorm = sizeSamples / static_cast<double>(buffer->getNumSamples());
            }
            waveform.setGrainWindowOverlay(isGrainMode, grainWindowNorm);
            waveform.setGrainMarkerPositions(strip->getGrainPreviewPositions(),
                                             strip->getGrainPreviewPitchNorms());
            waveform.setGrainHudOverlay(false, {}, {}, 0.0f, 0.0f, 0.0f,
                                        strip->getGrainPitch(), strip->getGrainArpDepth(), strip->getGrainPitchJitter());
        }
    }
    else if (!showingStepDisplay && !showingSampleMode)
    {
        if (waveform.hasLoadedAudio())
            waveform.clear();
    }
    
    // Update tempo label - only if visible
    if (tempoLabel.isVisible())
    {
        float beats = strip->getBeatsPerLoop();
        
        // Simple, safe validation
        if (beats >= 0.25f && beats <= 64.0f && std::isfinite(beats))
        {
            // Valid range - format it
            tempoLabel.setText(juce::String(beats, 1) + "b", juce::dontSendNotification);
        }
        else
        {
            // Invalid or auto - show AUTO
            tempoLabel.setText("AUTO", juce::dontSendNotification);
        }
    }
    
    if (!speedSlider.isMouseButtonDown() && !modulates(ModernAudioEngine::ModTarget::Speed))
        speedSlider.setValue(getSpeedControlValueForStrip(*strip), juce::dontSendNotification);
    if (!scratchSlider.isMouseButtonDown())
        scratchSlider.setValue(strip->getScratchAmount(), juce::dontSendNotification);
    if (!sliceLengthSlider.isMouseButtonDown())
        sliceLengthSlider.setValue(strip->getLoopSliceLength(), juce::dontSendNotification);
    const int stepLength = strip->getStepPatternLengthSteps();
    if ((stepLength % 16) == 0)
        patternLengthBox.setSelectedId(juce::jlimit(1, 4, stepLength / 16), juce::dontSendNotification);
    else
    {
        patternLengthBox.setSelectedId(0, juce::dontSendNotification);
        patternLengthBox.setText(juce::String(stepLength), juce::dontSendNotification);
    }
    if (!stepLengthReadoutBox.isInteracting())
        stepLengthReadoutBox.setValue(stepLength, juce::dontSendNotification);
    if (!stepAttackSlider.isMouseButtonDown())
        stepAttackSlider.setValue(strip->getStepEnvelopeAttackMs(), juce::dontSendNotification);
    if (!stepDecaySlider.isMouseButtonDown())
        stepDecaySlider.setValue(strip->getStepEnvelopeDecayMs(), juce::dontSendNotification);
    if (!stepReleaseSlider.isMouseButtonDown())
        stepReleaseSlider.setValue(strip->getStepEnvelopeReleaseMs(), juce::dontSendNotification);
    {
        const float recordingBarsBeats = static_cast<float>(juce::jlimit(1, 8, strip->getRecordingBars()) * 4);
        float beats = strip->getBeatsPerLoop();
        if (!(beats > 0.0f && std::isfinite(beats)))
            beats = recordingBarsBeats;
        else if (strip->isPlaying() && beats >= 4.0f && std::abs(beats - recordingBarsBeats) > 0.01f)
            beats = recordingBarsBeats;

        struct BeatChoice { float beats; int id; };
        static constexpr BeatChoice choices[] {
            { 1.0f, 25 }, { 2.0f, 50 }, { 4.0f, 100 },
            { 8.0f, 200 }, { 16.0f, 400 }, { 32.0f, 800 }
        };

        int selectedId = 100;
        float best = std::numeric_limits<float>::max();
        for (const auto& c : choices)
        {
            const float d = std::abs(beats - c.beats);
            if (d < best)
            {
                best = d;
                selectedId = c.id;
            }
        }
        recordBarsBox.setSelectedId(selectedId, juce::dontSendNotification);
        recordBarsBox.setEnabled(processor.canChangeBarLengthNow(stripIndex));
    }
    const bool recordArmed = !strip->hasAudio();
    const bool blinkOn = processor.getAudioEngine()->shouldBlinkRecordLED();
    recordButton.setButtonText(recordArmed ? "ARM" : "REC");
    recordButton.setColour(juce::TextButton::buttonColourId,
                           recordArmed
                               ? (blinkOn ? juce::Colour(0xffc95252) : juce::Colour(0xff743636))
                               : (blinkOn ? juce::Colour(0xffa64a4a) : juce::Colour(0xff444444)));
    recordButton.setColour(juce::TextButton::textColourOffId, juce::Colour(0xfff0f0f0));
    
    // Sync volume and pan from engine
    if (!modulates(ModernAudioEngine::ModTarget::Volume))
        volumeSlider.setValue(strip->getVolume(), juce::dontSendNotification);
    waveform.setVisualGainDb(static_cast<float>(trimSlider.getValue()));
    if (!modulates(ModernAudioEngine::ModTarget::Pan))
        panSlider.setValue(strip->getPan(), juce::dontSendNotification);
    // The pitch slider is APVTS-attached, so pushing strip state back into it here can
    // fight user drags and briefly snap the parameter back to an older value.
    // Sync play mode dropdown with strip state
    int modeId = static_cast<int>(strip->getPlayMode()) + 1;
    if (playModeBox.getSelectedId() != modeId)
        playModeBox.setSelectedId(modeId, juce::dontSendNotification);
    
    // Sync direction mode dropdown with strip state
    int dirModeId = static_cast<int>(strip->getDirectionMode()) + 1;
    if (directionModeBox.getSelectedId() != dirModeId)
        directionModeBox.setSelectedId(dirModeId, juce::dontSendNotification);

    const bool transientMode = strip->isTransientSliceMode();
    transientSliceButton.setToggleState(transientMode, juce::dontSendNotification);
    transientSliceButton.setButtonText(transientMode ? "TRANS" : "TIME");
    const bool stripLoadBusy = processor.isLoopStripLoadInFlight(stripIndex);
    const float stripLoadProgress = processor.getLoopStripLoadProgress(stripIndex);
    const auto stripLoadStatus = processor.getLoopStripLoadStatusText(stripIndex);
    const bool pitchAnalysisBusy = processor.isLoopStripPitchAnalysisInFlight(stripIndex);
    const float pitchAnalysisProgress = processor.getLoopStripPitchAnalysisProgress(stripIndex);
    const auto pitchAnalysisStatus = processor.getLoopStripPitchAnalysisStatusText(stripIndex);
    const bool stripTaskBusy = stripLoadBusy || pitchAnalysisBusy;
    const float stripTaskProgress = stripLoadBusy ? stripLoadProgress : pitchAnalysisProgress;
    const auto stripTaskStatus = stripLoadBusy ? stripLoadStatus : pitchAnalysisStatus;
    const int detectedPitchMidi = processor.getLoopStripDetectedPitchMidi(stripIndex);
    const float detectedPitchHz = processor.getLoopStripDetectedPitchHz(stripIndex);
    const float detectedPitchConfidence = processor.getLoopStripDetectedPitchConfidence(stripIndex);
    const int detectedScaleIndex = processor.getLoopStripDetectedScaleIndex(stripIndex);
    const float detectedScaleConfidence = processor.getLoopStripDetectedScaleConfidence(stripIndex);
    const int assignedPitchMidi = processor.getLoopStripAssignedPitchMidi(stripIndex);
    const bool assignedPitchManual = processor.isLoopStripAssignedPitchManual(stripIndex);
    const bool pitchUsedEssentia = processor.didLoopStripPitchUseEssentia(stripIndex);
    const float pitchSyncCorrectionSemitones = processor.getLoopStripPitchSyncCorrectionSemitones(stripIndex);
    const auto pitchRole = processor.getLoopPitchRole(stripIndex);
    const auto pitchSyncTiming = processor.getLoopPitchSyncTiming(stripIndex);
    const int rootNoteMidi = processor.getGlobalRootNoteMidi();
    const auto globalScale = processor.getGlobalPitchScale();
    const juce::String detectedScaleText = detectedScaleIndex >= 0
        ? pitchScaleDisplayName(static_cast<ModernAudioEngine::PitchScale>(detectedScaleIndex), false)
        : juce::String();
    const juce::String detectedPitchConfidenceText = confidencePercentText(detectedPitchConfidence);
    const juce::String detectedScaleConfidenceText = confidencePercentText(detectedScaleConfidence);
    const juce::String noteSourceText = assignedPitchManual ? "Manual" : "Detected";
    const juce::String backendText = pitchUsedEssentia ? "Essentia" : "Fallback";
    pitchMasterButton.setEnabled(!stripLoadBusy
                                 && (!pitchAnalysisBusy || pitchRole == MlrVSTAudioProcessor::LoopPitchRole::Master));
    pitchSyncButton.setEnabled(!stripLoadBusy
                               && (!pitchAnalysisBusy || pitchRole == MlrVSTAudioProcessor::LoopPitchRole::Sync));
    pitchMasterButton.setButtonText("PM");
    pitchSyncButton.setButtonText("PS");
    const bool progressChanged = loopPitchAnalysisActive != stripTaskBusy
        || std::abs(loopPitchAnalysisProgress - stripTaskProgress) > 0.0001f
        || loopPitchAnalysisStatus != stripTaskStatus;
    loopPitchAnalysisActive = stripTaskBusy;
    loopPitchAnalysisProgress = stripTaskProgress;
    loopPitchAnalysisStatus = stripTaskStatus;
    pitchMasterButton.setColour(juce::TextButton::buttonColourId,
                                pitchRole == MlrVSTAudioProcessor::LoopPitchRole::Master
                                    ? juce::Colour(0xffc78a3a)
                                    : juce::Colour(0xff444444));
    pitchMasterButton.setColour(juce::TextButton::textColourOffId,
                                pitchRole == MlrVSTAudioProcessor::LoopPitchRole::Master
                                    ? juce::Colours::black
                                    : juce::Colour(0xfff0f0f0));
    pitchSyncButton.setColour(juce::TextButton::buttonColourId,
                              pitchRole == MlrVSTAudioProcessor::LoopPitchRole::Sync
                                  ? juce::Colour(0xff4ca7a7)
                                  : juce::Colour(0xff444444));
    pitchSyncButton.setColour(juce::TextButton::textColourOffId,
                              pitchRole == MlrVSTAudioProcessor::LoopPitchRole::Sync
                                  ? juce::Colours::black
                                  : juce::Colour(0xfff0f0f0));
    identityModeButton.setButtonText(playModeDisplayName(playMode, true));
    identityModeButton.setTooltip("Playback mode for this strip.");
    identityTargetButton.setButtonText(directionModeDisplayName(strip->getDirectionMode(), true));
    identityTargetButton.setTooltip("Playback direction for this strip.");
    identityRoleButton.setButtonText(loopPitchRoleDisplayName(pitchRole, true));
    identityRoleButton.setTooltip("Pitch role for this strip. PM suggests or sets the global tonal center. PS follows that shared tonal center. Free stays independent.");
    identityRoleButton.setColour(juce::TextButton::buttonColourId,
                                 pitchRole == MlrVSTAudioProcessor::LoopPitchRole::Master
                                     ? juce::Colour(0xffc78a3a)
                                     : (pitchRole == MlrVSTAudioProcessor::LoopPitchRole::Sync
                                            ? juce::Colour(0xff4ca7a7)
                                            : juce::Colour(0xff3b4146)));
    identityRoleButton.setColour(juce::TextButton::textColourOffId,
                                 pitchRole == MlrVSTAudioProcessor::LoopPitchRole::None
                                     ? kTextPrimary
                                     : juce::Colours::black);
    identityNoteButton.setButtonText(assignedPitchMidi >= 0 ? getCompactNoteName(assignedPitchMidi) : "--");
    identityNoteButton.setTooltip("Source note of this material. Click to set detected or manual source metadata for the strip.");
    identityTimingButton.setButtonText(loopPitchSyncTimingDisplayName(pitchSyncTiming, true));
    identityTimingButton.setTooltip("Retune timing for PS strips. Global root: "
                                    + getCompactNoteName(rootNoteMidi)
                                    + " | scale: "
                                    + pitchScaleDisplayName(globalScale, false));
    pitchMasterButton.setTooltip("Pitch Master. Active strip analysis sets the global root note. Current root: "
                                 + getCompactNoteName(rootNoteMidi)
                                 + " | scale: " + pitchScaleDisplayName(globalScale, false)
                                 + (detectedPitchMidi >= 0 ? (" | detected: " + getCompactNoteName(detectedPitchMidi)
                                                              + (detectedPitchConfidenceText.isNotEmpty() ? (" " + detectedPitchConfidenceText) : juce::String()))
                                                         : juce::String())
                                 + (detectedScaleText.isNotEmpty() ? (" | scale sugg: " + detectedScaleText
                                                                      + (detectedScaleConfidenceText.isNotEmpty() ? (" " + detectedScaleConfidenceText) : juce::String()))
                                                                 : juce::String())
                                 + " | source: " + noteSourceText
                                 + " | backend: " + backendText);
    pitchSyncButton.setTooltip("Pitch Sync. Active strips retune from their note to the global root note ("
                               + getCompactNoteName(rootNoteMidi) + "). Timing: "
                               + loopPitchSyncTimingDisplayName(pitchSyncTiming, false)
                               + " | Option-click cycles timing.");
    pitchNoteBox.setEnabled(!stripTaskBusy);
    pitchNoteBox.setTooltip(
        (pitchRole == MlrVSTAudioProcessor::LoopPitchRole::Master
            ? "Pitch Master note. Manual changes also set the global root note."
            : "Source note for this strip. PS strips retune from this note to the global root.")
        + juce::String(" | source: ") + noteSourceText
        + (detectedPitchConfidenceText.isNotEmpty() ? (" | note conf: " + detectedPitchConfidenceText) : juce::String())
        + (detectedScaleText.isNotEmpty() ? (" | scale: " + detectedScaleText) : juce::String())
        + (detectedScaleConfidenceText.isNotEmpty() ? (" " + detectedScaleConfidenceText) : juce::String())
        + " | backend: " + backendText);
    const int pitchNoteId = assignedPitchMidi >= 0 ? ((assignedPitchMidi % 12) + 1) : 0;
    if (pitchNoteBox.getSelectedId() != pitchNoteId)
        pitchNoteBox.setSelectedId(pitchNoteId, juce::dontSendNotification);
    const auto sampleName = processor.getStripDisplaySampleName(stripIndex);
    const auto sampleLabelText = sampleName.isNotEmpty() ? sampleName : juce::String("No sample");
    stripSampleNameLabel.setText(sampleLabelText, juce::dontSendNotification);
    stripSampleNameLabel.setTooltip(sampleName.isNotEmpty()
                                        ? sampleName
                                        : juce::String("No sample. Click to load."));
    if (progressChanged)
        repaint(loopPitchProgressBounds);

    const bool showLoopPitchOverlay = !showingStepDisplay
        && !showingSampleMode
        && (playMode == EnhancedAudioStrip::PlayMode::OneShot
            || playMode == EnhancedAudioStrip::PlayMode::Loop
            || playMode == EnhancedAudioStrip::PlayMode::Gate);
    if (showLoopPitchOverlay)
    {
        juce::String lineA;
        juce::String lineB;
        if (stripTaskBusy)
        {
            lineA = stripTaskStatus.isNotEmpty()
                ? stripTaskStatus
                : "Loading";
            lineB = juce::String(juce::roundToInt(stripTaskProgress * 100.0f)) + "%";
        }
        else if (detectedPitchMidi >= 0)
        {
            lineA = "Key " + getCompactNoteName(detectedPitchMidi);
            if (detectedPitchHz > 0.0f && std::isfinite(detectedPitchHz))
                lineA << " " << juce::String(detectedPitchHz, detectedPitchHz >= 100.0f ? 1 : 2) << "Hz";
            if (detectedPitchConfidenceText.isNotEmpty())
                lineA << " " << detectedPitchConfidenceText;
            if (detectedScaleText.isNotEmpty())
            {
                lineB = "Scale "
                    + pitchScaleDisplayName(static_cast<ModernAudioEngine::PitchScale>(detectedScaleIndex), true);
                if (detectedScaleConfidenceText.isNotEmpty())
                    lineB << " " << detectedScaleConfidenceText;
            }
            if (pitchRole == MlrVSTAudioProcessor::LoopPitchRole::Sync)
            {
                if (lineB.isNotEmpty())
                    lineB << " • ";
                lineB << "PS "
                      << (pitchSyncCorrectionSemitones >= 0.0f ? "+" : "")
                      << juce::String(pitchSyncCorrectionSemitones, 1) << " st";
            }
            if (lineB.isNotEmpty())
                lineB << " • ";
            lineB << (assignedPitchManual ? "Man" : "Det");
        }
        else if (detectedScaleText.isNotEmpty())
        {
            lineA = "Scale " + detectedScaleText;
            if (detectedScaleConfidenceText.isNotEmpty())
                lineA << " " << detectedScaleConfidenceText;
            if (pitchRole == MlrVSTAudioProcessor::LoopPitchRole::Sync)
            {
                lineB << "PS "
                      << (pitchSyncCorrectionSemitones >= 0.0f ? "+" : "")
                      << juce::String(pitchSyncCorrectionSemitones, 1) << " st";
                lineB << " • ";
            }
            lineB << (assignedPitchManual ? "Man" : "Det");
        }
        waveform.setLoopPitchOverlay(lineA.isNotEmpty() || lineB.isNotEmpty(), lineA, lineB);
    }
    else
    {
        waveform.setLoopPitchOverlay(false, {}, {});
    }

    updateGrainOverlayVisibility();
    if (!modulates(ModernAudioEngine::ModTarget::GrainSize))
        grainSizeSlider.setValue(strip->getGrainSizeMs(), juce::dontSendNotification);
    if (!modulates(ModernAudioEngine::ModTarget::GrainDensity))
        grainDensitySlider.setValue(strip->getGrainDensity(), juce::dontSendNotification);
    if (!modulates(ModernAudioEngine::ModTarget::GrainPitch))
        grainPitchSlider.setValue(strip->getGrainPitch(), juce::dontSendNotification);
    if (!modulates(ModernAudioEngine::ModTarget::GrainPitchJitter))
        grainPitchJitterSlider.setValue(strip->getGrainPitchJitter(), juce::dontSendNotification);
    if (!modulates(ModernAudioEngine::ModTarget::GrainSpread))
        grainSpreadSlider.setValue(strip->getGrainSpread(), juce::dontSendNotification);
    if (!modulates(ModernAudioEngine::ModTarget::GrainJitter))
        grainJitterSlider.setValue(strip->getGrainJitter(), juce::dontSendNotification);
    if (!grainPositionJitterSlider.isMouseButtonDown() && !modulates(ModernAudioEngine::ModTarget::GrainPositionJitter))
        grainPositionJitterSlider.setValue(strip->getGrainPositionJitter(), juce::dontSendNotification);
    if (!modulates(ModernAudioEngine::ModTarget::GrainRandom))
        grainRandomSlider.setValue(strip->getGrainRandomDepth(), juce::dontSendNotification);
    if (!modulates(ModernAudioEngine::ModTarget::GrainArp))
        grainArpSlider.setValue(strip->getGrainArpDepth(), juce::dontSendNotification);
    if (!modulates(ModernAudioEngine::ModTarget::GrainCloud))
        grainCloudSlider.setValue(strip->getGrainCloudDepth(), juce::dontSendNotification);
    if (!modulates(ModernAudioEngine::ModTarget::GrainEmitter))
        grainEmitterSlider.setValue(strip->getGrainEmitterDepth(), juce::dontSendNotification);
    if (!modulates(ModernAudioEngine::ModTarget::GrainEnvelope))
        grainEnvelopeSlider.setValue(strip->getGrainEnvelope(), juce::dontSendNotification);
    if (!grainShapeSlider.isMouseButtonDown() && !modulates(ModernAudioEngine::ModTarget::GrainShape))
        grainShapeSlider.setValue(strip->getGrainShape(), juce::dontSendNotification);
    const bool grainSyncEnabled = strip->isGrainTempoSyncEnabled();
    grainSizeSyncToggle.setToggleState(grainSyncEnabled, juce::dontSendNotification);
    grainSizeDivLabel.setText(grainSyncEnabled ? "SYNC" : "FREE", juce::dontSendNotification);
    grainSizeSyncToggle.setColour(juce::ToggleButton::textColourId, grainSyncEnabled ? stripColor.brighter(0.35f) : stripColor.withAlpha(0.72f));
    grainSizeSyncToggle.setColour(juce::ToggleButton::tickColourId, grainSyncEnabled ? stripColor.brighter(0.35f) : stripColor.withAlpha(0.72f));
    {
        const bool arpActive = strip->getGrainArpDepth() > 0.001f;
        grainPitchLabel.setText(arpActive ? "RANGE" : "PITCH", juce::dontSendNotification);
        if (!grainPitchSlider.isMouseButtonDown())
        {
            if (arpActive)
            {
                grainPitchSlider.setRange(0.0, 48.0, 0.1);
                grainPitchSlider.setValue(std::abs(strip->getGrainPitch()), juce::dontSendNotification);
            }
            else
            {
                grainPitchSlider.setRange(-48.0, 48.0, 0.1);
                grainPitchSlider.setValue(strip->getGrainPitch(), juce::dontSendNotification);
            }
        }
    }

    // Sync group selector from engine
    int currentGroup = strip->getGroup();
    int selectedId = currentGroup + 2;  // Convert: -1→1, 0→2, 1→3, 2→4, 3→5
    if (groupSelector.getSelectedId() != selectedId)
    {
        groupSelector.setSelectedId(selectedId, juce::dontSendNotification);
    }
    identityGroupButton.setButtonText(currentGroup >= 0 ? ("G" + juce::String(currentGroup + 1)) : "None");
    identityGroupButton.setTooltip("Mute group assignment for this strip.");

    // Mod target pulse indication on actual control colours (not label text).
    auto tintSlider = [](juce::Slider& s, juce::Colour c, float pulseAmount)
    {
        const float pulse = juce::jlimit(0.0f, 1.0f, pulseAmount);
        const auto fill = c.interpolatedWith(kAccent.brighter(0.5f), 0.25f * pulse);
        s.setColour(juce::Slider::rotarySliderFillColourId, fill);
        s.setColour(juce::Slider::trackColourId, fill.withAlpha(0.78f + (0.2f * pulse)));
        s.setColour(juce::Slider::thumbColourId, fill.brighter(0.18f + (0.42f * pulse)));
        s.setColour(juce::Slider::rotarySliderOutlineColourId,
                    juce::Colour(0xff4a4a4a).interpolatedWith(fill.brighter(0.55f), 0.7f * pulse));
    };
    auto setModIndicator = [](juce::Slider& s, bool active, float depth, float signedPos, juce::Colour colour)
    {
        auto& props = s.getProperties();
        props.set("modActive", active);
        props.set("modDepth", juce::jlimit(0.0f, 1.0f, depth));
        props.set("modSigned", juce::jlimit(-1.0f, 1.0f, signedPos));
        props.set("modColour", static_cast<int>(colour.getARGB()));
    };
    auto pickVisibleModColour = [](const juce::Slider& s)
    {
        const auto base = s.findColour(juce::Slider::rotarySliderFillColourId);
        const float hue = base.getHue();
        const bool nearYellowHue = (hue > 0.10f && hue < 0.18f) && base.getSaturation() > 0.25f;
        const auto ref = juce::Colour(0xffffd24a);
        const float dr = base.getFloatRed() - ref.getFloatRed();
        const float dg = base.getFloatGreen() - ref.getFloatGreen();
        const float db = base.getFloatBlue() - ref.getFloatBlue();
        const float rgbDist = std::sqrt((dr * dr) + (dg * dg) + (db * db));
        const bool nearAccent = base.getPerceivedBrightness() > 0.45f
                             && rgbDist < 0.34f;
        if (nearYellowHue || nearAccent)
            return juce::Colour(0xff3bd5ff); // cyan contrast for yellow/orange controls
        return juce::Colour(0xffffd24a);     // default warm modulation color
    };
    const auto baseControl = stripColor.withAlpha(0.72f);
    tintSlider(volumeSlider, baseControl, 0.0f);
    tintSlider(panSlider, baseControl, 0.0f);
    tintSlider(pitchSlider, baseControl, 0.0f);
    tintSlider(speedSlider, baseControl, 0.0f);
    tintSlider(scratchSlider, baseControl, 0.0f);
    tintSlider(sliceLengthSlider, baseControl, 0.0f);
    tintSlider(grainSizeSlider, baseControl, 0.0f);
    tintSlider(grainDensitySlider, baseControl, 0.0f);
    tintSlider(grainPitchSlider, baseControl, 0.0f);
    tintSlider(grainPitchJitterSlider, baseControl, 0.0f);
    tintSlider(grainSpreadSlider, baseControl, 0.0f);
    tintSlider(grainJitterSlider, baseControl, 0.0f);
    tintSlider(grainPositionJitterSlider, baseControl, 0.0f);
    tintSlider(grainRandomSlider, baseControl, 0.0f);
    tintSlider(grainArpSlider, baseControl, 0.0f);
    tintSlider(grainCloudSlider, baseControl, 0.0f);
    tintSlider(grainEmitterSlider, baseControl, 0.0f);
    tintSlider(grainEnvelopeSlider, baseControl, 0.0f);
    tintSlider(grainShapeSlider, baseControl, 0.0f);
    setModIndicator(volumeSlider, false, 0.0f, 0.0f, kAccent);
    setModIndicator(panSlider, false, 0.0f, 0.0f, kAccent);
    setModIndicator(pitchSlider, false, 0.0f, 0.0f, kAccent);
    setModIndicator(speedSlider, false, 0.0f, 0.0f, kAccent);
    setModIndicator(scratchSlider, false, 0.0f, 0.0f, kAccent);
    setModIndicator(sliceLengthSlider, false, 0.0f, 0.0f, kAccent);
    setModIndicator(grainSizeSlider, false, 0.0f, 0.0f, kAccent);
    setModIndicator(grainDensitySlider, false, 0.0f, 0.0f, kAccent);
    setModIndicator(grainPitchSlider, false, 0.0f, 0.0f, kAccent);
    setModIndicator(grainPitchJitterSlider, false, 0.0f, 0.0f, kAccent);
    setModIndicator(grainSpreadSlider, false, 0.0f, 0.0f, kAccent);
    setModIndicator(grainJitterSlider, false, 0.0f, 0.0f, kAccent);
    setModIndicator(grainPositionJitterSlider, false, 0.0f, 0.0f, kAccent);
    setModIndicator(grainRandomSlider, false, 0.0f, 0.0f, kAccent);
    setModIndicator(grainArpSlider, false, 0.0f, 0.0f, kAccent);
    setModIndicator(grainCloudSlider, false, 0.0f, 0.0f, kAccent);
    setModIndicator(grainEmitterSlider, false, 0.0f, 0.0f, kAccent);
    setModIndicator(grainEnvelopeSlider, false, 0.0f, 0.0f, kAccent);
    setModIndicator(grainShapeSlider, false, 0.0f, 0.0f, kAccent);

    if (auto* engine = processor.getAudioEngine())
    {
        const auto mod = engine->getModSequencerState(stripIndex);
        if (mod.target != ModernAudioEngine::ModTarget::None)
        {
            const int lengthBars = juce::jlimit(1, ModernAudioEngine::MaxModBars, engine->getModLengthBars(stripIndex));
            const int totalSteps = juce::jmax(ModernAudioEngine::ModSteps, lengthBars * ModernAudioEngine::ModSteps);
            const int activeStep = juce::jlimit(0, totalSteps - 1, engine->getModCurrentGlobalStep(stripIndex));
            const float raw = juce::jlimit(0.0f, 1.0f, engine->getModStepValueAbsolute(stripIndex, activeStep));
            const bool bipolar = mod.bipolar && modTargetAllowsBipolar(mod.target);
            const float depth = juce::jlimit(0.0f, 1.0f, mod.depth);
            const float modNorm = juce::jlimit(0.0f, 1.0f, raw * depth);
            const float modBi = juce::jlimit(-1.0f, 1.0f, ((raw * 2.0f) - 1.0f) * depth);
            const float intensity = bipolar ? std::abs(modBi) : modNorm;
            const float signedPos = juce::jlimit(-1.0f, 1.0f, (raw * 2.0f) - 1.0f);

            const float stepPulse = ((activeStep & 1) == 0) ? 1.0f : 0.65f;
            const float pulseAmount = juce::jlimit(0.0f, 1.0f,
                                                   (0.35f + (0.65f * juce::jmax(0.2f, intensity))) * stepPulse);

            auto* targetSlider = [&]() -> juce::Slider*
            {
                switch (mod.target)
                {
                    case ModernAudioEngine::ModTarget::None: return nullptr;
                    case ModernAudioEngine::ModTarget::Volume: return &volumeSlider;
                    case ModernAudioEngine::ModTarget::Pan: return &panSlider;
                    case ModernAudioEngine::ModTarget::Pitch: return &pitchSlider;
                    case ModernAudioEngine::ModTarget::Speed: return &speedSlider;
                    case ModernAudioEngine::ModTarget::Cutoff: return nullptr;
                    case ModernAudioEngine::ModTarget::Resonance: return nullptr;
                    case ModernAudioEngine::ModTarget::GrainSize: return &grainSizeSlider;
                    case ModernAudioEngine::ModTarget::GrainDensity: return &grainDensitySlider;
                    case ModernAudioEngine::ModTarget::GrainPitch: return &grainPitchSlider;
                    case ModernAudioEngine::ModTarget::GrainPitchJitter: return &grainPitchJitterSlider;
                    case ModernAudioEngine::ModTarget::GrainSpread: return &grainSpreadSlider;
                    case ModernAudioEngine::ModTarget::GrainJitter: return &grainJitterSlider;
                    case ModernAudioEngine::ModTarget::GrainRandom: return &grainRandomSlider;
                    case ModernAudioEngine::ModTarget::GrainArp: return &grainArpSlider;
                    case ModernAudioEngine::ModTarget::GrainCloud: return &grainCloudSlider;
                    case ModernAudioEngine::ModTarget::GrainEmitter: return &grainEmitterSlider;
                    case ModernAudioEngine::ModTarget::GrainEnvelope: return &grainEnvelopeSlider;
                    case ModernAudioEngine::ModTarget::GrainPositionJitter: return &grainPositionJitterSlider;
                    case ModernAudioEngine::ModTarget::GrainShape: return &grainShapeSlider;
                    case ModernAudioEngine::ModTarget::FilterMorph: return nullptr;
                    case ModernAudioEngine::ModTarget::Retrigger: return nullptr;
                    case ModernAudioEngine::ModTarget::FilterEnable: return nullptr;
                    case ModernAudioEngine::ModTarget::SliceLength: return nullptr;
                    case ModernAudioEngine::ModTarget::Scratch: return nullptr;
                    case ModernAudioEngine::ModTarget::Rearrange: return nullptr;
                    case ModernAudioEngine::ModTarget::DelayMix: return nullptr;
                    case ModernAudioEngine::ModTarget::DelayTime: return nullptr;
                    case ModernAudioEngine::ModTarget::DelayFeedback: return nullptr;
                    case ModernAudioEngine::ModTarget::DelayLowCut: return nullptr;
                    case ModernAudioEngine::ModTarget::DelayHighCut: return nullptr;
                    default: return nullptr;
                }
            }();
            if (targetSlider != nullptr)
            {
                const auto targetColour = pickVisibleModColour(*targetSlider);
                const auto pulseColour = targetColour.withAlpha(0.82f + (0.18f * pulseAmount));
                tintSlider(*targetSlider, pulseColour, pulseAmount);
                setModIndicator(*targetSlider, true, depth, signedPos, targetColour);
            }
        }
    }
    
    repaint();  // For LED overlay
}

//==============================================================================
// FXStripControl Implementation
//==============================================================================

FXStripControl::FXStripControl(int idx, MlrVSTAudioProcessor& p)
    : stripIndex(idx), processor(p)
{
    // Get strip color
    stripColor = getStripColor(idx);
    
    // Strip label exists but not visible (used internally if needed)
    stripLabel.setText("Strip " + juce::String(idx + 1), juce::dontSendNotification);
    stripLabel.setFont(juce::Font(juce::FontOptions(11.0f, juce::Font::bold)));
    stripLabel.setColour(juce::Label::textColourId, stripColor);
    // DON'T add to view - no label shown
    
    // Filter Enable (button only, no text label)
    filterEnableButton.setButtonText("Filter");
    filterEnableButton.setClickingTogglesState(true);
    filterEnableButton.onClick = [this]() {
        processor.setStripFilterEnabledControlValue(stripIndex,
                                                    filterEnableButton.getToggleState());
    };
    addAndMakeVisible(filterEnableButton);
    
    // Filter Frequency
    filterFreqLabel.setText("Freq", juce::dontSendNotification);
    filterFreqLabel.setJustificationType(juce::Justification::centred);
    filterFreqLabel.setFont(juce::Font(juce::FontOptions(9.0f, juce::Font::bold)));
    filterFreqLabel.setColour(juce::Label::textColourId, stripColor);
    addAndMakeVisible(filterFreqLabel);
    
    filterFreqSlider.setSliderStyle(juce::Slider::Rotary);
    filterFreqSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 38, 12);
    filterFreqSlider.setRange(20.0, 20000.0, 1.0);
    filterFreqSlider.setSkewFactorFromMidPoint(1000.0);
    filterFreqSlider.setValue(20000.0);  // Default fully open (20kHz)
    enableAltClickReset(filterFreqSlider, 20000.0);
    filterFreqSlider.setTextValueSuffix(" Hz");
    filterFreqSlider.onValueChange = [this]() {
        processor.setStripFilterFrequencyControlValue(stripIndex,
                                                      static_cast<float>(filterFreqSlider.getValue()));
    };
    addAndMakeVisible(filterFreqSlider);
    
    // Filter Resonance
    filterResLabel.setText("Res", juce::dontSendNotification);
    filterResLabel.setJustificationType(juce::Justification::centred);
    filterResLabel.setFont(juce::Font(juce::FontOptions(9.0f, juce::Font::bold)));
    filterResLabel.setColour(juce::Label::textColourId, stripColor);
    addAndMakeVisible(filterResLabel);
    
    filterResSlider.setSliderStyle(juce::Slider::Rotary);
    filterResSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 35, 12);
    filterResSlider.setRange(0.1, 10.0, 0.01);
    filterResSlider.setValue(0.707);
    enableAltClickReset(filterResSlider, 0.707);
    filterResSlider.setTextValueSuffix(" Q");
    filterResSlider.onValueChange = [this]() {
        processor.setStripFilterResonanceControlValue(stripIndex,
                                                      static_cast<float>(filterResSlider.getValue()));
    };
    addAndMakeVisible(filterResSlider);
    
    // Filter Morph
    filterMorphLabel.setText("Morph", juce::dontSendNotification);
    filterMorphLabel.setJustificationType(juce::Justification::centred);
    filterMorphLabel.setFont(juce::Font(juce::FontOptions(9.0f, juce::Font::bold)));
    filterMorphLabel.setColour(juce::Label::textColourId, stripColor);
    addAndMakeVisible(filterMorphLabel);

    filterMorphSlider.setSliderStyle(juce::Slider::Rotary);
    filterMorphSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 38, 12);
    filterMorphSlider.setRange(0.0, 1.0, 0.001);
    filterMorphSlider.setValue(0.0);
    filterMorphSlider.setDoubleClickReturnValue(true, 0.0);
    filterMorphSlider.textFromValueFunction = [](double value)
    {
        const double v = juce::jlimit(0.0, 1.0, value);
        if (v < 0.25) return juce::String("LP");
        if (v < 0.75) return juce::String("BP");
        return juce::String("HP");
    };
    filterMorphSlider.valueFromTextFunction = [](const juce::String& text)
    {
        const auto t = text.trim().toUpperCase();
        if (t.contains("LP")) return 0.0;
        if (t.contains("BP")) return 0.5;
        if (t.contains("HP")) return 1.0;
        return 0.0;
    };
    filterMorphSlider.onValueChange = [this]()
    {
        processor.setStripFilterMorphControlValue(stripIndex,
                                                  static_cast<float>(filterMorphSlider.getValue()));
    };
    addAndMakeVisible(filterMorphSlider);

    // Filter Algorithm selector
    filterAlgoLabel.setText("Alg", juce::dontSendNotification);
    filterAlgoLabel.setJustificationType(juce::Justification::centred);
    filterAlgoLabel.setFont(juce::Font(juce::FontOptions(9.0f, juce::Font::bold)));
    filterAlgoLabel.setColour(juce::Label::textColourId, stripColor);
    addAndMakeVisible(filterAlgoLabel);

    filterAlgoBox.addItem("SVF12", 1);
    filterAlgoBox.addItem("SVF24", 2);
    filterAlgoBox.addItem("LAD12", 3);
    filterAlgoBox.addItem("LAD24", 4);
    filterAlgoBox.addItem("MOOG S", 5);
#if MLRVST_ENABLE_HUOVILAINEN
    filterAlgoBox.addItem("MOOG H", 6);
#else
    filterAlgoBox.addItem("MOOG H*", 6);
#endif
    filterAlgoBox.setSelectedId(1);
    styleUiCombo(filterAlgoBox);
    filterAlgoBox.setJustificationType(juce::Justification::centred);
#if MLRVST_ENABLE_HUOVILAINEN
    filterAlgoBox.setTooltip("Filter algorithm: SVF12, SVF24, Ladder12, Ladder24, Moog Stilson LP, Moog Huovilainen LP");
#else
    filterAlgoBox.setTooltip("Filter algorithm: SVF12, SVF24, Ladder12, Ladder24, Moog Stilson LP, Moog H* (Stilson fallback; Huovilainen disabled in this build)");
#endif
    filterAlgoBox.onChange = [this]()
    {
        const int id = filterAlgoBox.getSelectedId();
        auto algo = EnhancedAudioStrip::FilterAlgorithm::Tpt12;
        if (id == 2) algo = EnhancedAudioStrip::FilterAlgorithm::Tpt24;
        else if (id == 3) algo = EnhancedAudioStrip::FilterAlgorithm::Ladder12;
        else if (id == 4) algo = EnhancedAudioStrip::FilterAlgorithm::Ladder24;
        else if (id == 5) algo = EnhancedAudioStrip::FilterAlgorithm::MoogStilson;
        else if (id == 6) algo = EnhancedAudioStrip::FilterAlgorithm::MoogHuov;
        processor.setStripFilterAlgorithmControlValue(stripIndex, algo);
    };
    addAndMakeVisible(filterAlgoBox);

    duckEnableButton.setButtonText("Duck");
    duckEnableButton.setClickingTogglesState(true);
    duckEnableButton.setTooltip("Enable strip ducking");
    addAndMakeVisible(duckEnableButton);

    duckFollowMasterButton.setButtonText("Follow M");
    duckFollowMasterButton.setClickingTogglesState(true);
    duckFollowMasterButton.setTooltip("Use the global master duck trigger strip when available");
    addAndMakeVisible(duckFollowMasterButton);

    duckSourceLabel.setText("Src", juce::dontSendNotification);
    duckSourceLabel.setJustificationType(juce::Justification::centred);
    duckSourceLabel.setFont(juce::Font(juce::FontOptions(9.0f, juce::Font::bold)));
    duckSourceLabel.setColour(juce::Label::textColourId, stripColor);
    addAndMakeVisible(duckSourceLabel);

    duckSourceBox.addItem("Self", 1);
    duckSourceBox.addItem("Master", 2);
    for (int sourceStrip = 0; sourceStrip < MlrVSTAudioProcessor::MaxStrips; ++sourceStrip)
        duckSourceBox.addItem("S" + juce::String(sourceStrip + 1), sourceStrip + 3);
    styleUiCombo(duckSourceBox);
    duckSourceBox.setJustificationType(juce::Justification::centred);
    duckSourceBox.setSelectedId(1);
    addAndMakeVisible(duckSourceBox);

    auto setupDuckLabel = [this](juce::Label& label, const juce::String& text)
    {
        label.setText(text, juce::dontSendNotification);
        label.setJustificationType(juce::Justification::centredLeft);
        label.setFont(juce::Font(juce::FontOptions(9.0f, juce::Font::bold)));
        label.setColour(juce::Label::textColourId, stripColor);
        addAndMakeVisible(label);
    };

    auto setupDuckSlider = [](juce::Slider& slider)
    {
        slider.setSliderStyle(juce::Slider::LinearHorizontal);
        slider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    };

    setupDuckLabel(duckThresholdLabel, "Thresh");
    setupDuckSlider(duckThresholdSlider);
    duckThresholdSlider.setRange(-60.0, 0.0, 0.1);
    duckThresholdSlider.setValue(-24.0);
    enableAltClickReset(duckThresholdSlider, -24.0);
    duckThresholdSlider.setTooltip("Duck threshold in dB");
    addAndMakeVisible(duckThresholdSlider);

    setupDuckLabel(duckRatioLabel, "Ratio");
    setupDuckSlider(duckRatioSlider);
    duckRatioSlider.setRange(1.0, 20.0, 0.01);
    duckRatioSlider.setValue(4.0);
    enableAltClickReset(duckRatioSlider, 4.0);
    duckRatioSlider.setTooltip("Duck ratio");
    addAndMakeVisible(duckRatioSlider);

    setupDuckLabel(duckAttackLabel, "Atk");
    setupDuckSlider(duckAttackSlider);
    duckAttackSlider.setRange(0.1, 200.0, 0.1);
    duckAttackSlider.setValue(10.0);
    enableAltClickReset(duckAttackSlider, 10.0);
    duckAttackSlider.setTooltip("Duck attack in milliseconds");
    addAndMakeVisible(duckAttackSlider);

    setupDuckLabel(duckReleaseLabel, "Rel");
    setupDuckSlider(duckReleaseSlider);
    duckReleaseSlider.setRange(5.0, 1000.0, 0.1);
    duckReleaseSlider.setValue(180.0);
    enableAltClickReset(duckReleaseSlider, 180.0);
    duckReleaseSlider.setTooltip("Duck release in milliseconds");
    addAndMakeVisible(duckReleaseSlider);

    setupDuckLabel(duckGainCompLabel, "Gain");
    duckGainCompSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    duckGainCompSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    duckGainCompSlider.setRange(0.0, 24.0, 0.1);
    duckGainCompSlider.setValue(0.0);
    enableAltClickReset(duckGainCompSlider, 0.0);
    duckGainCompSlider.setTooltip("Compressor output gain compensation in dB");
    addAndMakeVisible(duckGainCompSlider);

    auto setupDelayLabel = [this](juce::Label& label, const juce::String& text)
    {
        label.setText(text, juce::dontSendNotification);
        label.setJustificationType(juce::Justification::centredLeft);
        label.setFont(juce::Font(juce::FontOptions(9.0f, juce::Font::bold)));
        label.setColour(juce::Label::textColourId, stripColor);
        addAndMakeVisible(label);
    };

    auto setupDelaySlider = [](juce::Slider& slider)
    {
        slider.setSliderStyle(juce::Slider::LinearHorizontal);
        slider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    };

    setupDelayLabel(delayModeLabel, "Mode");
    delayModeBox.addItem("Single", 1);
    delayModeBox.addItem("Dual", 2);
    delayModeBox.addItem("Ping-Pong", 3);
    delayModeBox.setSelectedId(1);
    styleUiCombo(delayModeBox);
    delayModeBox.setJustificationType(juce::Justification::centred);
    delayModeBox.setTooltip("Delay routing: Single = mono repeats, Dual = independent left/right repeats, Ping-Pong = alternating cross-feedback repeats.");
    addAndMakeVisible(delayModeBox);

    delaySyncButton.setButtonText("Sync");
    delaySyncButton.setClickingTogglesState(true);
    delaySyncButton.setTooltip("Sync delay time to tempo. Off = free seconds.");
    addAndMakeVisible(delaySyncButton);

    setupDelayLabel(delayMixLabel, "Mix");
    setupDelaySlider(delayMixSlider);
    delayMixSlider.setRange(0.0, 1.0, 0.001);
    delayMixSlider.setValue(0.0);
    enableAltClickReset(delayMixSlider, 0.0);
    delayMixSlider.setTooltip("Delay wet/dry mix.");
    addAndMakeVisible(delayMixSlider);

    setupDelayLabel(delayTimeLabel, "Time");
    setupDelaySlider(delayTimeSlider);
    delayTimeSlider.setRange(0.25, 4.0, 0.001);
    delayTimeSlider.setValue(1.0);
    enableAltClickReset(delayTimeSlider, 1.0);
    delayTimeSlider.setTooltip("Delay time. Sync on = beats, Sync off = seconds.");
    addAndMakeVisible(delayTimeSlider);

    setupDelayLabel(delayFeedbackLabel, "Fdbk");
    setupDelaySlider(delayFeedbackSlider);
    delayFeedbackSlider.setRange(0.0, 0.97, 0.001);
    delayFeedbackSlider.setValue(0.35);
    enableAltClickReset(delayFeedbackSlider, 0.35);
    delayFeedbackSlider.setTooltip("Delay feedback amount.");
    addAndMakeVisible(delayFeedbackSlider);

    setupDelayLabel(delayLowCutLabel, "Low");
    setupDelaySlider(delayLowCutSlider);
    delayLowCutSlider.setRange(20.0, 12000.0, 1.0);
    delayLowCutSlider.setSkewFactorFromMidPoint(400.0);
    delayLowCutSlider.setValue(20.0);
    enableAltClickReset(delayLowCutSlider, 20.0);
    delayLowCutSlider.setTooltip("Delay low cut in Hz.");
    addAndMakeVisible(delayLowCutSlider);

    setupDelayLabel(delayHighCutLabel, "High");
    setupDelaySlider(delayHighCutSlider);
    delayHighCutSlider.setRange(200.0, 20000.0, 1.0);
    delayHighCutSlider.setSkewFactorFromMidPoint(3000.0);
    delayHighCutSlider.setValue(12000.0);
    enableAltClickReset(delayHighCutSlider, 12000.0);
    delayHighCutSlider.setTooltip("Delay high cut in Hz.");
    addAndMakeVisible(delayHighCutSlider);

    duckEnableAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        processor.parameters, "stripDuckEnabled" + juce::String(stripIndex), duckEnableButton);
    duckFollowMasterAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        processor.parameters, "stripDuckFollowMaster" + juce::String(stripIndex), duckFollowMasterButton);
    duckSourceAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        processor.parameters, "stripDuckSource" + juce::String(stripIndex), duckSourceBox);
    duckThresholdAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        processor.parameters, "stripDuckThreshold" + juce::String(stripIndex), duckThresholdSlider);
    duckRatioAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        processor.parameters, "stripDuckRatio" + juce::String(stripIndex), duckRatioSlider);
    duckAttackAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        processor.parameters, "stripDuckAttack" + juce::String(stripIndex), duckAttackSlider);
    duckReleaseAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        processor.parameters, "stripDuckRelease" + juce::String(stripIndex), duckReleaseSlider);
    duckGainCompAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        processor.parameters, "stripDuckGainComp" + juce::String(stripIndex), duckGainCompSlider);
    delayModeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        processor.parameters, "stripDelayMode" + juce::String(stripIndex), delayModeBox);
    delaySyncAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        processor.parameters, "stripDelaySync" + juce::String(stripIndex), delaySyncButton);
    delayMixAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        processor.parameters, "stripDelayMix" + juce::String(stripIndex), delayMixSlider);
    delayTimeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        processor.parameters, "stripDelayTime" + juce::String(stripIndex), delayTimeSlider);
    delayFeedbackAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        processor.parameters, "stripDelayFeedback" + juce::String(stripIndex), delayFeedbackSlider);
    delayLowCutAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        processor.parameters, "stripDelayLowCut" + juce::String(stripIndex), delayLowCutSlider);
    delayHighCutAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        processor.parameters, "stripDelayHighCut" + juce::String(stripIndex), delayHighCutSlider);

    // Start timer for updating from engine
    startTimer(50);  // Update at 20Hz
}

void FXStripControl::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();

    drawPanel(g, bounds, stripColor, 10.0f);

    const auto inner = bounds.reduced(8.0f, 8.0f);
    const float filterWidth = inner.getWidth() * 0.32f;
    const float delayWidth = inner.getWidth() * 0.32f;
    const float dividerX1 = inner.getX() + filterWidth;
    const float dividerX2 = dividerX1 + delayWidth;

    g.setColour(kPanelStroke.withAlpha(0.7f));
    g.fillRect(dividerX1 - 1.0f, inner.getY() + 4.0f, 2.0f, inner.getHeight() - 8.0f);
    g.fillRect(dividerX2 - 1.0f, inner.getY() + 4.0f, 2.0f, inner.getHeight() - 8.0f);
}

void FXStripControl::resized()
{
    auto bounds = getLocalBounds();
    bounds.reduce(8, 8);

    const int totalWidth = bounds.getWidth();
    const int filterWidth = juce::jmax(210, static_cast<int>(std::round(totalWidth * 0.32f)));
    const int delayWidth = juce::jmax(224, static_cast<int>(std::round(totalWidth * 0.32f)));
    auto filterField = bounds.removeFromLeft(filterWidth).reduced(6, 0);
    auto delayField = bounds.removeFromLeft(delayWidth).reduced(6, 0);
    auto compField = bounds.reduced(6, 0);

    filterField.removeFromTop(4);
    delayField.removeFromTop(4);
    compField.removeFromTop(4);

    auto topRow = filterField.removeFromTop(22);
    filterEnableButton.setBounds(topRow.removeFromLeft(56));
    topRow.removeFromLeft(4);
    filterAlgoLabel.setBounds(topRow.removeFromLeft(24));
    topRow.removeFromLeft(3);
    filterAlgoBox.setBounds(topRow.removeFromLeft(92));
    filterField.removeFromTop(4);

    auto controlsRow = filterField.removeFromTop(64);

    int controlWidth = controlsRow.getWidth() / 3;
    auto freqCol = controlsRow.removeFromLeft(controlWidth).reduced(2, 0);
    filterFreqLabel.setBounds(freqCol.removeFromTop(12));
    filterFreqSlider.setBounds(freqCol);

    auto resCol = controlsRow.removeFromLeft(controlWidth).reduced(2, 0);
    filterResLabel.setBounds(resCol.removeFromTop(12));
    filterResSlider.setBounds(resCol);

    auto morphCol = controlsRow.reduced(2, 0);
    filterMorphLabel.setBounds(morphCol.removeFromTop(12));
    filterMorphSlider.setBounds(morphCol);

    auto delayTopRow = delayField.removeFromTop(22);
    delayModeLabel.setBounds(delayTopRow.removeFromLeft(34));
    delayTopRow.removeFromLeft(4);
    delayModeBox.setBounds(delayTopRow.removeFromLeft(96));
    delayTopRow.removeFromLeft(6);
    delaySyncButton.setBounds(delayTopRow.removeFromLeft(54));
    delayField.removeFromTop(4);

    const int delayRowCount = 5;
    const int delayRowGap = 2;
    const int delayRowHeight = juce::jmax(12,
                                          (delayField.getHeight() - ((delayRowCount - 1) * delayRowGap))
                                              / juce::jmax(1, delayRowCount));

    auto layoutDelayRow = [&delayField, delayRowHeight](juce::Label& label, juce::Slider& slider)
    {
        if (delayField.getHeight() <= 0)
        {
            label.setBounds({});
            slider.setBounds({});
            return;
        }

        const int rowHeight = juce::jmin(delayRowHeight, delayField.getHeight());
        auto row = delayField.removeFromTop(rowHeight);
        const int labelWidth = juce::jmin(38, juce::jmax(28, row.getWidth() / 7));
        label.setBounds(row.removeFromLeft(labelWidth));
        row.removeFromLeft(4);
        slider.setBounds(row);
        if (delayField.getHeight() > delayRowGap)
            delayField.removeFromTop(delayRowGap);
    };

    layoutDelayRow(delayMixLabel, delayMixSlider);
    layoutDelayRow(delayTimeLabel, delayTimeSlider);
    layoutDelayRow(delayFeedbackLabel, delayFeedbackSlider);
    layoutDelayRow(delayLowCutLabel, delayLowCutSlider);
    layoutDelayRow(delayHighCutLabel, delayHighCutSlider);

    auto duckTopRow = compField.removeFromTop(22);
    duckEnableButton.setBounds(duckTopRow.removeFromLeft(54));
    duckTopRow.removeFromLeft(4);
    duckFollowMasterButton.setBounds(duckTopRow.removeFromLeft(68));
    duckTopRow.removeFromLeft(6);
    duckSourceLabel.setBounds(duckTopRow.removeFromLeft(24));
    duckTopRow.removeFromLeft(4);
    duckSourceBox.setBounds(duckTopRow);
    compField.removeFromTop(4);

    const int duckRowCount = 5;
    const int duckRowGap = 2;
    const int duckRowHeight = juce::jmax(12,
                                         (compField.getHeight() - ((duckRowCount - 1) * duckRowGap))
                                             / juce::jmax(1, duckRowCount));

    auto layoutDuckRow = [&compField, duckRowHeight](juce::Label& label, juce::Slider& slider)
    {
        if (compField.getHeight() <= 0)
        {
            label.setBounds({});
            slider.setBounds({});
            return;
        }

        const int rowHeight = juce::jmin(duckRowHeight, compField.getHeight());
        auto row = compField.removeFromTop(rowHeight);
        const int labelWidth = juce::jmin(42, juce::jmax(30, row.getWidth() / 8));
        label.setBounds(row.removeFromLeft(labelWidth));
        row.removeFromLeft(4);
        slider.setBounds(row);
        if (compField.getHeight() > duckRowGap)
            compField.removeFromTop(duckRowGap);
    };

    layoutDuckRow(duckThresholdLabel, duckThresholdSlider);
    layoutDuckRow(duckRatioLabel, duckRatioSlider);
    layoutDuckRow(duckAttackLabel, duckAttackSlider);
    layoutDuckRow(duckReleaseLabel, duckReleaseSlider);
    layoutDuckRow(duckGainCompLabel, duckGainCompSlider);
}

void FXStripControl::updateFromEngine()
{
    if (!processor.getAudioEngine()) return;
    
    auto* strip = processor.getAudioEngine()->getStrip(stripIndex);
    if (!strip) return;

    const bool isStepMode = (strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Step);
    auto* stepSampler = isStepMode ? strip->getStepSampler() : nullptr;
    
    // Update from engine state
    const bool filterEnabled = (isStepMode && stepSampler) ? stepSampler->isFilterEnabled() : strip->isFilterEnabled();
    const float filterFreq = (isStepMode && stepSampler) ? stepSampler->getFilterFrequency() : strip->getFilterFrequency();
    const float filterRes = (isStepMode && stepSampler) ? stepSampler->getFilterResonance() : strip->getFilterResonance();
    float filterMorph = strip->getFilterMorph();
    if (isStepMode && stepSampler)
    {
        switch (stepSampler->getFilterType())
        {
            case FilterType::LowPass:  filterMorph = 0.0f; break;
            case FilterType::BandPass: filterMorph = 0.5f; break;
            case FilterType::HighPass: filterMorph = 1.0f; break;
            default: break;
        }
    }

    filterEnableButton.setToggleState(filterEnabled, juce::dontSendNotification);
    filterFreqSlider.setValue(filterFreq, juce::dontSendNotification);
    filterResSlider.setValue(filterRes, juce::dontSendNotification);
    filterMorphSlider.setValue(filterMorph, juce::dontSendNotification);

    const auto base = stripColor.withAlpha(0.72f);
    auto setBaseSliderTint = [](juce::Slider& s, juce::Colour c)
    {
        s.setColour(juce::Slider::rotarySliderFillColourId, c);
        s.setColour(juce::Slider::trackColourId, c.withAlpha(0.78f));
        s.setColour(juce::Slider::thumbColourId, c.brighter(0.18f));
        s.setColour(juce::Slider::rotarySliderOutlineColourId, c.darker(0.72f).withAlpha(0.82f));
        s.setColour(juce::Slider::backgroundColourId, c.darker(0.88f).withAlpha(0.92f));
    };
    auto pickVisibleModColour = [](juce::Colour baseColour)
    {
        const auto baseRgb = juce::Colour::fromRGB(baseColour.getRed(), baseColour.getGreen(), baseColour.getBlue());
        const auto accentRgb = juce::Colour::fromRGB(0xff, 0xd2, 0x4a);
        const auto dR = static_cast<float>(baseRgb.getFloatRed() - accentRgb.getFloatRed());
        const auto dG = static_cast<float>(baseRgb.getFloatGreen() - accentRgb.getFloatGreen());
        const auto dB = static_cast<float>(baseRgb.getFloatBlue() - accentRgb.getFloatBlue());
        const float rgbDist = std::sqrt((dR * dR) + (dG * dG) + (dB * dB));
        const bool nearYellowHue = baseColour.getHue() > 0.10f && baseColour.getHue() < 0.18f;
        const bool nearAccent = baseColour.getPerceivedBrightness() > 0.45f && rgbDist < 0.34f;
        return (nearYellowHue || nearAccent) ? juce::Colour(0xff3bd5ff) : juce::Colour(0xffffd24a);
    };

    setBaseSliderTint(filterFreqSlider, base);
    setBaseSliderTint(filterResSlider, base);
    setBaseSliderTint(filterMorphSlider, base);
    setBaseSliderTint(duckThresholdSlider, base);
    setBaseSliderTint(duckRatioSlider, base);
    setBaseSliderTint(duckAttackSlider, base);
    setBaseSliderTint(duckReleaseSlider, base);
    setBaseSliderTint(duckGainCompSlider, base);
    setBaseSliderTint(delayMixSlider, base);
    setBaseSliderTint(delayTimeSlider, base);
    setBaseSliderTint(delayFeedbackSlider, base);
    setBaseSliderTint(delayLowCutSlider, base);
    setBaseSliderTint(delayHighCutSlider, base);

    const auto algo = strip->getFilterAlgorithm();
    int algoId = 1;
    if (algo == EnhancedAudioStrip::FilterAlgorithm::Tpt24) algoId = 2;
    else if (algo == EnhancedAudioStrip::FilterAlgorithm::Ladder12) algoId = 3;
    else if (algo == EnhancedAudioStrip::FilterAlgorithm::Ladder24) algoId = 4;
    else if (algo == EnhancedAudioStrip::FilterAlgorithm::MoogStilson) algoId = 5;
    else if (algo == EnhancedAudioStrip::FilterAlgorithm::MoogHuov) algoId = 6;
    filterAlgoBox.setSelectedId(algoId, juce::dontSendNotification);

    if (auto* engine = processor.getAudioEngine())
    {
        const auto mod = engine->getModSequencerState(stripIndex);
        const bool active = (mod.target == ModernAudioEngine::ModTarget::Cutoff
                          || mod.target == ModernAudioEngine::ModTarget::Resonance
                          || mod.target == ModernAudioEngine::ModTarget::FilterMorph
                          || mod.target == ModernAudioEngine::ModTarget::DelayMix
                          || mod.target == ModernAudioEngine::ModTarget::DelayTime
                          || mod.target == ModernAudioEngine::ModTarget::DelayFeedback
                          || mod.target == ModernAudioEngine::ModTarget::DelayLowCut
                          || mod.target == ModernAudioEngine::ModTarget::DelayHighCut);
        if (active)
        {
            const float depth = juce::jlimit(0.0f, 1.0f, mod.depth);
            const int lengthBars = juce::jlimit(1, ModernAudioEngine::MaxModBars, engine->getModLengthBars(stripIndex));
            const int totalSteps = juce::jmax(ModernAudioEngine::ModSteps, lengthBars * ModernAudioEngine::ModSteps);
            const int step = juce::jlimit(0, totalSteps - 1, engine->getModCurrentGlobalStep(stripIndex));
            const float raw = juce::jlimit(0.0f, 1.0f, engine->getModStepValueAbsolute(stripIndex, step));
            const bool bipolar = mod.bipolar && modTargetAllowsBipolar(mod.target);
            const float modNorm = juce::jlimit(0.0f, 1.0f, raw * depth);
            const float modBi = juce::jlimit(-1.0f, 1.0f, ((raw * 2.0f) - 1.0f) * depth);
            const float intensity = bipolar ? std::abs(modBi) : modNorm;
            const float stepPulse = ((step & 1) == 0) ? 1.0f : 0.65f;
            const float pulse = juce::jlimit(0.0f, 1.0f,
                                             (0.35f + (0.65f * juce::jmax(0.2f, intensity))) * stepPulse);
            const auto modColour = pickVisibleModColour(base).withAlpha(0.82f + (0.18f * pulse));
            if (mod.target == ModernAudioEngine::ModTarget::Cutoff)
                setBaseSliderTint(filterFreqSlider, modColour);
            else if (mod.target == ModernAudioEngine::ModTarget::Resonance)
                setBaseSliderTint(filterResSlider, modColour);
            else if (mod.target == ModernAudioEngine::ModTarget::FilterMorph)
                setBaseSliderTint(filterMorphSlider, modColour);
            else if (mod.target == ModernAudioEngine::ModTarget::DelayMix)
                setBaseSliderTint(delayMixSlider, modColour);
            else if (mod.target == ModernAudioEngine::ModTarget::DelayTime)
                setBaseSliderTint(delayTimeSlider, modColour);
            else if (mod.target == ModernAudioEngine::ModTarget::DelayFeedback)
                setBaseSliderTint(delayFeedbackSlider, modColour);
            else if (mod.target == ModernAudioEngine::ModTarget::DelayLowCut)
                setBaseSliderTint(delayLowCutSlider, modColour);
            else if (mod.target == ModernAudioEngine::ModTarget::DelayHighCut)
                setBaseSliderTint(delayHighCutSlider, modColour);
        }
    }

    const bool duckEnabled = duckEnableButton.getToggleState();
    const bool followMaster = duckFollowMasterButton.getToggleState();
    duckFollowMasterButton.setEnabled(duckEnabled);
    duckSourceBox.setEnabled(duckEnabled && !followMaster);
    duckThresholdSlider.setEnabled(duckEnabled);
    duckRatioSlider.setEnabled(duckEnabled);
    duckAttackSlider.setEnabled(duckEnabled);
    duckReleaseSlider.setEnabled(duckEnabled);
    duckGainCompSlider.setEnabled(duckEnabled);
}

void FXStripControl::timerCallback()
{
    updateFromEngine();
}

void StripControl::timerCallback()
{
    if (!processor.getAudioEngine())
        return;
    
    updateFromEngine();
}

MonomeGridDisplay::MonomeGridDisplay(MlrVSTAudioProcessor& p)
    : processor(p)
{
    startTimer(50); // 20fps updates
}

void MonomeGridDisplay::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds();
    
    // Background
    g.setColour(kSurfaceDark);
    g.fillRoundedRectangle(bounds.toFloat(), 8.0f);
    
    // Title
    g.setColour(kTextPrimary);
    g.setFont(juce::Font(juce::FontOptions(16.0f, juce::Font::bold)));
    auto titleArea = bounds.removeFromTop(30);
    g.drawText("Monome Grid", titleArea, juce::Justification::centred);
    
    bounds.removeFromTop(4);
    
    // Draw grid
    for (int y = 0; y < gridHeight; ++y)
    {
        for (int x = 0; x < gridWidth; ++x)
        {
            auto buttonBounds = getButtonBounds(x, y);
            
            // Button background
            g.setColour(juce::Colour(0xff2a2a2a));
            g.fillRoundedRectangle(buttonBounds.toFloat(), 2.0f);
            
            // LED state
            int brightness = ledState[x][y];
            if (brightness > 0)
            {
                float alpha = brightness / 15.0f;
                g.setColour(kAccent.withAlpha(alpha));
                g.fillRoundedRectangle(buttonBounds.toFloat().reduced(2), 2.0f);
            }
            
            // Pressed state
            if (buttonPressed[x][y])
            {
                g.setColour(kTextPrimary.withAlpha(0.25f));
                g.fillRoundedRectangle(buttonBounds.toFloat(), 2.0f);
            }
            
            // Border
            g.setColour(kPanelStroke);
            g.drawRoundedRectangle(buttonBounds.toFloat(), 2.0f, 1.0f);
        }
    }
}

void MonomeGridDisplay::resized()
{
    repaint();
}

juce::Rectangle<int> MonomeGridDisplay::getButtonBounds(int x, int y) const
{
    auto bounds = getLocalBounds();
    bounds.removeFromTop(34); // Title area
    
    auto buttonSize = juce::jmin(
        bounds.getWidth() / gridWidth - 4,
        bounds.getHeight() / gridHeight - 4
    );
    
    auto gridStartX = (bounds.getWidth() - (buttonSize + 4) * gridWidth) / 2;
    auto gridStartY = bounds.getY() + (bounds.getHeight() - (buttonSize + 4) * gridHeight) / 2;
    
    return juce::Rectangle<int>(
        gridStartX + x * (buttonSize + 4),
        gridStartY + y * (buttonSize + 4),
        buttonSize,
        buttonSize
    );
}

void MonomeGridDisplay::mouseDown(const juce::MouseEvent& e)
{
    for (int y = 0; y < gridHeight; ++y)
    {
        for (int x = 0; x < gridWidth; ++x)
        {
            if (getButtonBounds(x, y).contains(e.getPosition()))
            {
                handleButtonPress(x, y, true);
                return;
            }
        }
    }
}

void MonomeGridDisplay::mouseUp(const juce::MouseEvent& e)
{
    (void) e;
    for (int y = 0; y < gridHeight; ++y)
    {
        for (int x = 0; x < gridWidth; ++x)
        {
            if (buttonPressed[x][y])
            {
                handleButtonPress(x, y, false);
            }
        }
    }
}

void MonomeGridDisplay::mouseDrag(const juce::MouseEvent& e)
{
    for (int y = 0; y < gridHeight; ++y)
    {
        for (int x = 0; x < gridWidth; ++x)
        {
            bool shouldBePressed = getButtonBounds(x, y).contains(e.getPosition());
            if (shouldBePressed != buttonPressed[x][y])
            {
                handleButtonPress(x, y, shouldBePressed);
            }
        }
    }
}

void MonomeGridDisplay::handleButtonPress(int x, int y, bool down)
{
    buttonPressed[x][y] = down;
    
    if (down)
    {
        DBG("Button pressed: x=" << x << ", y=" << y);
        
        // First row (y=0), columns 4-7: Pattern recorders
        if (y == 0 && x >= 4 && x <= 7)
        {
            DBG("  -> Pattern recorder button detected!");
            int patternIndex = x - 4;  // 0-3 for patterns 0-3
            
            auto* engine = processor.getAudioEngine();
            if (engine)
            {
                auto* pattern = engine->getPattern(patternIndex);
                if (pattern)
                {
                    // Cycle through states: off → recording → playing → off
                    if (pattern->isRecording())
                    {
                        // Recording → Playing: Stop recording and start playback
                        DBG("Pattern " << patternIndex << ": Stop recording, start playback. Events: " << pattern->getEventCount());
                        const double currentBeat = engine->getTimelineBeat();
                        pattern->stopRecording();
                        pattern->startPlayback(currentBeat);
                    }
                    else if (pattern->isPlaying())
                    {
                        // Playing → Off: Stop playback
                        DBG("Pattern " << patternIndex << ": Stop playback");
                        pattern->stopPlayback();
                    }
                    else
                    {
                        // Off → Recording: Start recording
                        DBG("Pattern " << patternIndex << ": Start recording");
                        if (engine)
                            pattern->startRecording(engine->getTimelineBeat());
                    }
                }
            }
        }
        // Rows 0-5: Strip triggering (row 0 = strip 0, row 1 = strip 1, etc.)
        else if (y >= 0 && y < processor.MaxStrips && x < processor.MaxColumns)
        {
            // Skip pattern recorder buttons on row 0, columns 4-7
            if (y == 0 && x >= 4 && x <= 7)
                return;  // Already handled above
            
            int stripIndex = y;  // Row 0 → strip 0, Row 1 → strip 1, etc.
            
            // Trigger the strip
            processor.triggerStrip(stripIndex, x);
        }
    }
    
    // Don't send LEDs to Monome from here - PluginProcessor handles all LED updates
    // This updateFromEngine() is only for updating the GUI visualization
    
    repaint();
}

void MonomeGridDisplay::timerCallback()
{
    if (!processor.getAudioEngine())
        return;
    
    updateFromEngine();
}

void MonomeGridDisplay::updateFromEngine()
{
    // Update LED states from strips
    // Row 0 = Pattern recorder (columns 4-7)
    // Row 1 = Strip 0
    // Row 2 = Strip 1, etc.
    for (int stripIndex = 0; stripIndex < processor.MaxStrips; ++stripIndex)
    {
        int monomeRow = stripIndex + 1;  // Strip 0 → row 1, Strip 1 → row 2, etc.
        
        if (monomeRow >= gridHeight)
            break;  // Don't exceed grid height
        
        auto* strip = processor.getAudioEngine()->getStrip(stripIndex);
        if (strip)
        {
            // Check if this strip is in Step mode AND control mode is not active
            // When control mode is active (level, pan, sample select, etc.), hide step display
            bool controlModeActive = (processor.getCurrentControlMode() != MlrVSTAudioProcessor::ControlMode::Normal);
            
            if (strip->playMode == EnhancedAudioStrip::PlayMode::Step && !controlModeActive)
            {
                DBG("Strip " << stripIndex << " in Step mode - updating row " << monomeRow);
                
                // Show step pattern on Monome
                const auto visiblePattern = strip->getVisibleStepPattern();
                const int visibleCurrentStep = strip->getVisibleCurrentStep();
                for (int x = 0; x < gridWidth && x < 16; ++x)
                {
                    bool isCurrentStep = (x == visibleCurrentStep);
                    bool isActiveStep = visiblePattern[static_cast<size_t>(x)];
                    
                    int brightness = 0;
                    if (isCurrentStep && isActiveStep)
                    {
                        // Current step AND active - brightest
                        brightness = 15;
                    }
                    else if (isCurrentStep)
                    {
                        // Current step but inactive - medium
                        brightness = 6;
                    }
                    else if (isActiveStep)
                    {
                        // Active step (not current) - medium bright
                        brightness = 10;
                    }
                    else
                    {
                        // Inactive step - dim
                        brightness = 2;
                    }
                    
                    ledState[x][monomeRow] = brightness;
                }
                
                // Debug first few LEDs
                DBG("Step LEDs [0-3]: " << ledState[0][monomeRow] << " " 
                    << ledState[1][monomeRow] << " " 
                    << ledState[2][monomeRow] << " " 
                    << ledState[3][monomeRow]);
            }
            else if (strip->playMode == EnhancedAudioStrip::PlayMode::Sample && !controlModeActive)
            {
                if (auto* sampleEngine = processor.getSampleModeEngine(stripIndex, false))
                {
                    const auto snapshot = sampleEngine->getStateSnapshot();
                    const bool legacyLoopFeedback = snapshot.useLegacyLoopEngine
                        || strip->isSampleModeLegacyLoopEngineEnabled();
                    const bool loopTriggerMode = snapshot.triggerMode == SampleTriggerMode::Loop;
                    const int heldSlot = processor.getSampleModeHeldVisibleSliceSlot(stripIndex);
                    int playbackSlot = -1;
                    if (!legacyLoopFeedback && snapshot.playbackProgress >= 0.0f)
                    {
                        for (int x = 0; x < gridWidth && x < 16; ++x)
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
                    for (int x = 0; x < gridWidth && x < 16; ++x)
                    {
                        int brightness = 0;
                        if (snapshot.visibleSlices[static_cast<size_t>(x)].id >= 0)
                            brightness = legacyLoopFeedback
                                ? (strip->isPlaying() ? 4 : 2)
                                : (snapshot.isPlaying ? (loopTriggerMode ? 8 : 6)
                                                      : (loopTriggerMode ? 5 : 3));
                        if (snapshot.pendingVisibleSliceSlot == x)
                            brightness = juce::jmax(brightness, loopTriggerMode ? 12 : 10);
                        if (!legacyLoopFeedback && heldSlot == x)
                            brightness = juce::jmax(brightness, loopTriggerMode ? 11 : 9);
                        if ((legacyLoopFeedback && snapshot.activeVisibleSliceSlot == x) || playbackSlot == x)
                            brightness = 15;
                        else if (!legacyLoopFeedback && snapshot.activeVisibleSliceSlot == x)
                            brightness = 15;
                        ledState[x][monomeRow] = brightness;
                    }
                }
                else
                {
                    for (int x = 0; x < gridWidth && x < 16; ++x)
                        ledState[x][monomeRow] = 0;
                }
            }
            else if (strip->playMode != EnhancedAudioStrip::PlayMode::Step && !controlModeActive)
            {
                // Normal playback mode (Loop/OneShot) - show LED states from strip
                // When control mode is active, PluginProcessor handles ALL LED display
                auto ledStates = strip->getLEDStates();
                for (int x = 0; x < gridWidth && x < processor.MaxColumns; ++x)
                {
                    ledState[x][monomeRow] = ledStates[static_cast<size_t>(x)] ? 12 : 0; // Variable brightness
                }
            }
            // If control mode is active, don't touch LEDs - PluginProcessor handles it
        }
    }
    
    // Row 0, columns 4-7: Pattern recorder status (only if strip 0 NOT in step mode)
    if (gridHeight > 0)
    {
        auto* engine = processor.getAudioEngine();
        if (engine)
        {
            auto* strip0 = engine->getStrip(0);
            bool strip0IsStep = (strip0 && strip0->playMode == EnhancedAudioStrip::PlayMode::Step);
            
            // Only show pattern recorder if strip 0 is not in step mode
            if (!strip0IsStep)
            {
                for (int x = 4; x <= 7 && x < gridWidth; ++x)
                {
                    int patternIndex = x - 4;
                    auto* pattern = engine->getPattern(patternIndex);
                    if (pattern)
                    {
                        if (pattern->isRecording())
                        {
                            // Recording: Bright red (full brightness)
                            ledState[x][0] = 15;
                        }
                        else if (pattern->isPlaying())
                        {
                            // Playing: Medium green
                            ledState[x][0] = 10;
                        }
                        else if (pattern->hasEvents())
                        {
                            // Has recorded pattern: Dim (ready to play)
                            ledState[x][0] = 4;
                        }
                        else
                        {
                            // Empty: Off
                            ledState[x][0] = 0;
                        }
                    }
                }
            }
        }
    }
    
    // Hardware LED writes are centralized in MlrVSTAudioProcessor::updateMonomeLEDs().
    // The editor grid is visualization-only.
    repaint();
}


//==============================================================================
// MonomeControlPanel Implementation
//==============================================================================

MonomeControlPanel::MonomeControlPanel(MlrVSTAudioProcessor& p)
    : processor(p)
{
    // Title - compact
    titleLabel.setText("MONOME DEVICE", juce::dontSendNotification);
    titleLabel.setFont(juce::Font(juce::FontOptions(12.0f, juce::Font::bold)));  // Smaller
    titleLabel.setColour(juce::Label::textColourId, kTextMuted);
    addAndMakeVisible(titleLabel);

    refreshButton.setButtonText("Refresh");
    refreshButton.onClick = [this]()
    {
        processor.getMonomeConnection().refreshDeviceList();
    };
    addAndMakeVisible(refreshButton);

    gridLabel.setText("Grid", juce::dontSendNotification);
    gridLabel.setFont(juce::Font(juce::FontOptions(11.0f)));
    gridLabel.setColour(juce::Label::textColourId, kTextPrimary);
    addAndMakeVisible(gridLabel);

    gridDeviceSelector.setTextWhenNoChoicesAvailable("No grids found");
    gridDeviceSelector.setTextWhenNothingSelected("Select grid...");
    addAndMakeVisible(gridDeviceSelector);

    gridConnectButton.setButtonText("Use");
    gridConnectButton.onClick = [this]() { connectToGridDevice(); };
    addAndMakeVisible(gridConnectButton);

    gridStatusLabel.setText("Grid: not connected", juce::dontSendNotification);
    gridStatusLabel.setFont(juce::Font(juce::FontOptions(11.0f)));
    gridStatusLabel.setColour(juce::Label::textColourId, kAccent);
    addAndMakeVisible(gridStatusLabel);

    arcLabel.setText("Arc", juce::dontSendNotification);
    arcLabel.setFont(juce::Font(juce::FontOptions(11.0f)));
    arcLabel.setColour(juce::Label::textColourId, kTextPrimary);
    addAndMakeVisible(arcLabel);

    arcDeviceSelector.setTextWhenNoChoicesAvailable("No arcs found");
    arcDeviceSelector.setTextWhenNothingSelected("Select arc...");
    addAndMakeVisible(arcDeviceSelector);

    arcConnectButton.setButtonText("Use");
    arcConnectButton.onClick = [this]() { connectToArcDevice(); };
    addAndMakeVisible(arcConnectButton);

    arcStatusLabel.setText("Arc: not connected", juce::dontSendNotification);
    arcStatusLabel.setFont(juce::Font(juce::FontOptions(11.0f)));
    arcStatusLabel.setColour(juce::Label::textColourId, kAccent);
    addAndMakeVisible(arcStatusLabel);

    topRowModeLabel.setText("Top Row: Launch", juce::dontSendNotification);
    topRowModeLabel.setFont(juce::Font(juce::FontOptions(10.8f, juce::Font::bold)));
    topRowModeLabel.setColour(juce::Label::textColourId, kTextPrimary);
    addAndMakeVisible(topRowModeLabel);

    topRowHintLabel.setFont(juce::Font(juce::FontOptions(10.0f)));
    topRowHintLabel.setColour(juce::Label::textColourId, kTextMuted);
    topRowHintLabel.setJustificationType(juce::Justification::topLeft);
    topRowHintLabel.setMinimumHorizontalScale(0.82f);
    addAndMakeVisible(topRowHintLabel);

    rotationLabel.setText("Rotation", juce::dontSendNotification);
    rotationLabel.setFont(juce::Font(juce::FontOptions(11.0f)));
    rotationLabel.setColour(juce::Label::textColourId, kTextPrimary);
    addAndMakeVisible(rotationLabel);

    rotationSelector.addItem("0 deg", 1);
    rotationSelector.addItem("90 deg", 2);
    rotationSelector.addItem("180 deg", 3);
    rotationSelector.addItem("270 deg", 4);
    rotationSelector.setSelectedId(1);
    rotationSelector.onChange = [this]()
    {
        int rotation = (rotationSelector.getSelectedId() - 1) * 90;
        processor.getMonomeConnection().setRotation(rotation);
    };
    addAndMakeVisible(rotationSelector);
    
    auto safeThis = juce::Component::SafePointer<MonomeControlPanel>(this);
    processor.getMonomeConnection().onDeviceListUpdated = [safeThis](const std::vector<MonomeConnection::DeviceInfo>&)
    {
        juce::MessageManager::callAsync([safeThis]()
        {
            if (safeThis != nullptr)
                safeThis->updateDeviceList();
        });
    };

    updateDeviceList();
    updateStatus();
    processor.getMonomeConnection().refreshDeviceList();
    startTimer(1000); // Update status every second
}

MonomeControlPanel::~MonomeControlPanel()
{
    processor.getMonomeConnection().onDeviceListUpdated = nullptr;
}

void MonomeControlPanel::paint(juce::Graphics& g)
{
    drawPanel(g, getLocalBounds().toFloat(), kAccent, 8.0f);
}

void MonomeControlPanel::resized()
{
    auto bounds = getLocalBounds().reduced(8);
    const auto comboWidthForRow = [](int availableWidth)
    {
        return juce::jlimit(96, 168, availableWidth / 2);
    };

    auto titleRow = bounds.removeFromTop(20);
    refreshButton.setBounds(titleRow.removeFromRight(72));
    titleLabel.setBounds(titleRow);

    bounds.removeFromTop(6);

    auto gridRow = bounds.removeFromTop(22);
    gridLabel.setBounds(gridRow.removeFromLeft(34));
    gridRow.removeFromLeft(4);
    gridConnectButton.setBounds(gridRow.removeFromRight(52));
    gridRow.removeFromRight(4);
    gridDeviceSelector.setBounds(gridRow.removeFromLeft(comboWidthForRow(gridRow.getWidth())));

    bounds.removeFromTop(4);
    auto gridStatusRow = bounds.removeFromTop(18);
    gridStatusLabel.setBounds(gridStatusRow);

    bounds.removeFromTop(6);

    auto arcRow = bounds.removeFromTop(22);
    arcLabel.setBounds(arcRow.removeFromLeft(34));
    arcRow.removeFromLeft(4);
    arcConnectButton.setBounds(arcRow.removeFromRight(52));
    arcRow.removeFromRight(4);
    arcDeviceSelector.setBounds(arcRow.removeFromLeft(comboWidthForRow(arcRow.getWidth())));

    bounds.removeFromTop(4);
    auto arcStatusRow = bounds.removeFromTop(18);
    arcStatusLabel.setBounds(arcStatusRow);

    bounds.removeFromTop(6);

    auto rotationRow = bounds.removeFromTop(22);
    rotationLabel.setBounds(rotationRow.removeFromLeft(70));
    rotationRow.removeFromLeft(4);
    rotationSelector.setBounds(rotationRow.removeFromLeft(110));

    bounds.removeFromTop(6);
    auto topRowModeRow = bounds.removeFromTop(18);
    topRowModeLabel.setBounds(topRowModeRow);

    bounds.removeFromTop(2);
    auto topRowHintBounds = bounds.removeFromTop(32);
    topRowHintLabel.setBounds(topRowHintBounds);
}

void MonomeControlPanel::timerCallback()
{
    if (!processor.getAudioEngine())
        return;
    
    updateStatus();
}

void MonomeControlPanel::updateDeviceList()
{
    gridDeviceIndices.clear();
    arcDeviceIndices.clear();
    gridDeviceSelector.clear();
    arcDeviceSelector.clear();

    auto devices = processor.getMonomeConnection().getDiscoveredDevices();
    const auto currentGrid = processor.getMonomeConnection().getCurrentGridDevice();
    const auto currentArc = processor.getMonomeConnection().getCurrentArcDevice();
    int selectedGridId = 0;
    int selectedArcId = 0;

    for (size_t i = 0; i < devices.size(); ++i)
    {
        auto& device = devices[i];
        juce::String itemText = device.id + " (" + device.type + ") - " +
                                juce::String(device.sizeX) + "x" + juce::String(device.sizeY);

        if (device.type.containsIgnoreCase("arc"))
        {
            arcDeviceIndices.push_back(static_cast<int>(i));
            const int itemId = static_cast<int>(arcDeviceIndices.size());
            arcDeviceSelector.addItem(itemText, itemId);
            if (currentArc.id.isNotEmpty() && currentArc.id == device.id)
                selectedArcId = itemId;
        }
        else
        {
            gridDeviceIndices.push_back(static_cast<int>(i));
            const int itemId = static_cast<int>(gridDeviceIndices.size());
            gridDeviceSelector.addItem(itemText, itemId);
            if (currentGrid.id.isNotEmpty() && currentGrid.id == device.id)
                selectedGridId = itemId;
        }
    }

    if (gridDeviceSelector.getNumItems() > 0)
        gridDeviceSelector.setSelectedId(selectedGridId > 0 ? selectedGridId : 1, juce::dontSendNotification);
    else
        gridDeviceSelector.setSelectedId(0, juce::dontSendNotification);

    if (arcDeviceSelector.getNumItems() > 0)
        arcDeviceSelector.setSelectedId(selectedArcId > 0 ? selectedArcId : 1, juce::dontSendNotification);
    else
        arcDeviceSelector.setSelectedId(0, juce::dontSendNotification);
}

void MonomeControlPanel::connectToGridDevice()
{
    const int selectedId = gridDeviceSelector.getSelectedId();
    if (selectedId <= 0 || selectedId > static_cast<int>(gridDeviceIndices.size()))
        return;

    processor.getMonomeConnection().selectGridDevice(gridDeviceIndices[static_cast<size_t>(selectedId - 1)]);
}

void MonomeControlPanel::connectToArcDevice()
{
    const int selectedId = arcDeviceSelector.getSelectedId();
    if (selectedId <= 0 || selectedId > static_cast<int>(arcDeviceIndices.size()))
        return;

    processor.getMonomeConnection().selectArcDevice(arcDeviceIndices[static_cast<size_t>(selectedId - 1)]);
}

void MonomeControlPanel::updateStatus()
{
    auto& monome = processor.getMonomeConnection();
    const auto gridStatus = monome.getGridConnectionStatus();
    const auto arcStatus = monome.getArcConnectionStatus();
    gridStatusLabel.setText(gridStatus, juce::dontSendNotification);
    arcStatusLabel.setText(arcStatus, juce::dontSendNotification);

    gridStatusLabel.setColour(juce::Label::textColourId,
                              monome.supportsGrid() ? juce::Colour(0xff76be7e) : kAccent);
    arcStatusLabel.setColour(juce::Label::textColourId,
                             monome.supportsArc() ? juce::Colour(0xff76be7e) : kAccent);

    const auto topRowModeText = "Top Row: " + processor.getMonomeTopRowModeName();
    topRowModeLabel.setText(topRowModeText, juce::dontSendNotification);
    topRowModeLabel.setColour(juce::Label::textColourId,
                              processor.isMonomeTopRowEditActive() ? juce::Colour(0xff76be7e)
                                                                   : (processor.isMonomeTopRowEditSupported()
                                                                       ? kAccent
                                                                       : kTextPrimary));

    const auto topRowHintText = processor.getMonomeTopRowHintText();
    topRowHintLabel.setText(topRowHintText, juce::dontSendNotification);
    topRowHintLabel.setTooltip(topRowHintText);
}


//==============================================================================
// PatternControlPanel Implementation
//==============================================================================

void PatternRecordButton::clicked(const juce::ModifierKeys&)
{
    const juce::uint32 nowMs = juce::Time::getMillisecondCounter();

    if (pendingSingleTrigger && (nowMs - lastClickMs) <= static_cast<juce::uint32>(kDoubleClickWindowMs))
    {
        pendingSingleTrigger = false;
        stopTimer();
        if (onDoubleTrigger)
            onDoubleTrigger();
        return;
    }

    pendingSingleTrigger = true;
    lastClickMs = nowMs;
    startTimer(kDoubleClickWindowMs);

    if (triggerSingleImmediately && onSingleTrigger)
        onSingleTrigger();
}

void PatternRecordButton::timerCallback()
{
    stopTimer();

    if (!pendingSingleTrigger)
        return;

    pendingSingleTrigger = false;
    if (triggerSingleImmediately)
        return;

    if (onSingleTrigger)
        onSingleTrigger();
}

PatternControlPanel::PatternControlPanel(MlrVSTAudioProcessor& p)
    : processor(p)
{
    setWantsKeyboardFocus(true);

    titleLabel.setText("Pattern Recorder", juce::dontSendNotification);
    titleLabel.setFont(juce::Font(juce::FontOptions(16.0f, juce::Font::bold)));
    titleLabel.setColour(juce::Label::textColourId, kTextPrimary);
    titleLabel.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(titleLabel);

    instructionsLabel.setVisible(false);

    timingLabel.setText("Beat: --", juce::dontSendNotification);
    timingLabel.setFont(juce::Font(juce::FontOptions(10.0f)));
    timingLabel.setColour(juce::Label::textColourId, kTextSecondary);
    timingLabel.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(timingLabel);

    quantizeLabel.setText("Quantize: --", juce::dontSendNotification);
    quantizeLabel.setFont(juce::Font(juce::FontOptions(10.0f)));
    quantizeLabel.setColour(juce::Label::textColourId, kTextSecondary);
    quantizeLabel.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(quantizeLabel);
    
    // Pattern controls
    for (int i = 0; i < 4; ++i)
    {
        auto& pattern = patterns[i];
        const juce::String groupTag = "G" + juce::String(i + 1);

        pattern.nameLabel.setText("PATTERN " + juce::String(i + 1) + "  " + groupTag, juce::dontSendNotification);
        pattern.nameLabel.setFont(juce::Font(juce::FontOptions(12.0f, juce::Font::bold)));
        pattern.nameLabel.setColour(juce::Label::textColourId, getStripColor(i));
        pattern.nameLabel.setJustificationType(juce::Justification::centredLeft);
        addAndMakeVisible(pattern.nameLabel);
        
        pattern.recordButton.setButtonText("Rec");
        pattern.recordButton.setClickingTogglesState(false);
        pattern.recordButton.setTooltip("Single-click records a new loop. Double-click overdubs into the existing loop.");
        pattern.recordButton.onSingleTrigger = [this, i]()
        {
            auto* engine = processor.getAudioEngine();
            auto* patternState = engine != nullptr ? engine->getPattern(i) : nullptr;
            if (engine == nullptr || patternState == nullptr)
                return;

            if (patternState->isRecording())
                engine->stopPatternRecording(i);
            else
                engine->startPatternRecording(i);
        };
        pattern.recordButton.onDoubleTrigger = [this, i]()
        {
            if (auto* engine = processor.getAudioEngine())
                engine->startPatternOverdub(i);
        };
        addAndMakeVisible(pattern.recordButton);
        styleUiButton(pattern.recordButton, true);
        
        pattern.playButton.setButtonText("Play");
        pattern.playButton.setToggleable(true);
        pattern.playButton.setTooltip("Play/loop recorded trigger presses for " + groupTag + ".");
        pattern.playButton.onClick = [this, i]()
        {
            if (patterns[i].playButton.getToggleState())
                processor.getAudioEngine()->startPatternPlayback(i);
            else
                processor.getAudioEngine()->stopPatternPlayback(i);
        };
        addAndMakeVisible(pattern.playButton);
        styleUiButton(pattern.playButton);
        
        pattern.stopButton.setButtonText("Stop");
        pattern.stopButton.setTooltip("Stop pattern playback.");
        pattern.stopButton.onClick = [this, i]()
        {
            processor.getAudioEngine()->stopPatternPlayback(i);
            patterns[i].playButton.setToggleState(false, juce::dontSendNotification);
        };
        addAndMakeVisible(pattern.stopButton);
        styleUiButton(pattern.stopButton);
        
        pattern.clearButton.setButtonText("Clear");
        pattern.clearButton.setTooltip("Erase all events recorded for " + groupTag + ".");
        pattern.clearButton.onClick = [this, i]()
        {
            processor.getAudioEngine()->clearPattern(i);
        };
        addAndMakeVisible(pattern.clearButton);
        styleUiButton(pattern.clearButton);
        
        pattern.statusLabel.setText("EMPTY", juce::dontSendNotification);
        pattern.statusLabel.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
        pattern.statusLabel.setJustificationType(juce::Justification::centredRight);
        addAndMakeVisible(pattern.statusLabel);

        pattern.detailLabel.setText("Records " + groupTag + " trigger presses from the main page", juce::dontSendNotification);
        pattern.detailLabel.setFont(juce::Font(juce::FontOptions(10.0f)));
        pattern.detailLabel.setColour(juce::Label::textColourId, kTextSecondary);
        pattern.detailLabel.setJustificationType(juce::Justification::centredLeft);
        addAndMakeVisible(pattern.detailLabel);
    }

    sceneControls.nameLabel.setText("ACTIVE SCENE", juce::dontSendNotification);
    sceneControls.nameLabel.setFont(juce::Font(juce::FontOptions(12.0f, juce::Font::bold)));
    sceneControls.nameLabel.setColour(juce::Label::textColourId, kAccent);
    sceneControls.nameLabel.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(sceneControls.nameLabel);

    sceneControls.recordButton.setButtonText("Rec");
    sceneControls.recordButton.setClickingTogglesState(false);
    sceneControls.recordButton.setTooltip("Single-click records the current scene cycle. Double-click overdubs another pass.");
    sceneControls.recordButton.onSingleTrigger = [this]()
    {
        if (processor.isScenePerformanceRecording())
            processor.stopScenePerformanceRecording();
        else
            processor.startScenePerformanceRecording(false);
    };
    sceneControls.recordButton.onDoubleTrigger = [this]()
    {
        if (processor.isScenePerformanceRecording() && processor.isScenePerformanceOverdubbing())
            processor.extendScenePerformanceRecording();
        else
            processor.startScenePerformanceRecording(true);
    };
    addAndMakeVisible(sceneControls.recordButton);
    styleUiButton(sceneControls.recordButton, true);

    sceneControls.clearButton.setButtonText("Clear All");
    sceneControls.clearButton.setTooltip("Erase the active scene's recorded monome performance.");
    sceneControls.clearButton.onClick = [this]()
    {
        processor.clearScenePerformanceClip(processor.getActiveSceneSlot());
        sceneControls.selectedEventIndex = -1;
    };
    addAndMakeVisible(sceneControls.clearButton);
    styleUiButton(sceneControls.clearButton);

    sceneControls.deleteButton.setButtonText("Delete");
    sceneControls.deleteButton.setTooltip("Delete the selected scene event.");
    sceneControls.deleteButton.onClick = [this]()
    {
        deleteSelectedSceneEvent();
    };
    addAndMakeVisible(sceneControls.deleteButton);
    styleUiButton(sceneControls.deleteButton);

    sceneControls.trimBeforeButton.setButtonText("Trim <");
    sceneControls.trimBeforeButton.setTooltip("Keep the selected event and everything after it.");
    sceneControls.trimBeforeButton.onClick = [this]()
    {
        trimSceneEventsToSelection(true);
    };
    addAndMakeVisible(sceneControls.trimBeforeButton);
    styleUiButton(sceneControls.trimBeforeButton);

    sceneControls.trimAfterButton.setButtonText("Trim >");
    sceneControls.trimAfterButton.setTooltip("Keep the selected event and everything before it.");
    sceneControls.trimAfterButton.onClick = [this]()
    {
        trimSceneEventsToSelection(false);
    };
    addAndMakeVisible(sceneControls.trimAfterButton);
    styleUiButton(sceneControls.trimAfterButton);

    sceneControls.clearTriggersButton.setButtonText("Clear Trig");
    sceneControls.clearTriggersButton.setTooltip("Remove all trigger events from the scene clip.");
    sceneControls.clearTriggersButton.onClick = [this]()
    {
        clearSceneEventsByType(ScenePerformanceEventType::Trigger);
    };
    addAndMakeVisible(sceneControls.clearTriggersButton);
    styleUiButton(sceneControls.clearTriggersButton);

    sceneControls.clearControlsButton.setButtonText("Clear Ctrl");
    sceneControls.clearControlsButton.setTooltip("Remove all recorded control points from the scene clip.");
    sceneControls.clearControlsButton.onClick = [this]()
    {
        clearSceneEventsByType(ScenePerformanceEventType::ControlPoint);
    };
    addAndMakeVisible(sceneControls.clearControlsButton);
    styleUiButton(sceneControls.clearControlsButton);

    sceneControls.statusLabel.setText("EMPTY", juce::dontSendNotification);
    sceneControls.statusLabel.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
    sceneControls.statusLabel.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(sceneControls.statusLabel);

    sceneControls.detailLabel.setText("Records active scene monome triggers and control moves", juce::dontSendNotification);
    sceneControls.detailLabel.setFont(juce::Font(juce::FontOptions(10.0f)));
    sceneControls.detailLabel.setColour(juce::Label::textColourId, kTextSecondary);
    sceneControls.detailLabel.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(sceneControls.detailLabel);

    sceneControls.selectionLabel.setText("Click or drag a scene event to edit it", juce::dontSendNotification);
    sceneControls.selectionLabel.setFont(juce::Font(juce::FontOptions(9.0f)));
    sceneControls.selectionLabel.setColour(juce::Label::textColourId, kTextMuted);
    sceneControls.selectionLabel.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(sceneControls.selectionLabel);
    
    startTimer(100);
}

void PatternControlPanel::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();
    
    juce::ColourGradient bg(juce::Colour(0xff2e2e2e), 0.0f, 0.0f,
                            juce::Colour(0xff242424), 0.0f, bounds.getBottom(), false);
    g.setGradientFill(bg);
    g.fillAll();

    if (isSceneRecorderView())
    {
        auto card = getSceneCardBounds().toFloat();
        g.setColour(juce::Colour(0xff2b2b2b));
        g.fillRoundedRectangle(card, 8.0f);

        g.setColour(kPanelStroke);
        g.drawRoundedRectangle(card.reduced(0.5f), 8.0f, 1.0f);

        g.setColour(kAccent.withAlpha(0.85f));
        g.fillRoundedRectangle(card.removeFromLeft(3.0f), 2.0f);

        auto timelineArea = card.reduced(10.0f, 8.0f);
        timelineArea.removeFromTop(18.0f);
        timelineArea.removeFromTop(4.0f);
        timelineArea.removeFromTop(24.0f);
        timelineArea.removeFromTop(4.0f);
        timelineArea.removeFromTop(24.0f);
        timelineArea.removeFromTop(4.0f);
        timelineArea.removeFromTop(14.0f);
        timelineArea.removeFromTop(4.0f);
        timelineArea.removeFromTop(14.0f);
        timelineArea.removeFromTop(4.0f);
        paintSceneTimeline(g, timelineArea);
        return;
    }

    for (int i = 0; i < 4; ++i)
    {
        auto card = getPatternCardBounds(i).toFloat();

        g.setColour(juce::Colour(0xff2b2b2b));
        g.fillRoundedRectangle(card, 8.0f);

        g.setColour(kPanelStroke);
        g.drawRoundedRectangle(card.reduced(0.5f), 8.0f, 1.0f);

        g.setColour(getStripColor(i).withAlpha(0.85f));
        g.fillRoundedRectangle(card.removeFromLeft(3.0f), 2.0f);

        auto timelineArea = card.reduced(10.0f, 8.0f);
        timelineArea.removeFromTop(18.0f);
        timelineArea.removeFromTop(4.0f);
        timelineArea.removeFromTop(24.0f);
        timelineArea.removeFromTop(4.0f);
        timelineArea.removeFromTop(14.0f);
        timelineArea.removeFromTop(4.0f);
        paintPatternTimeline(g, i, timelineArea);
    }
}

void PatternControlPanel::resized()
{
    auto bounds = getLocalBounds().reduced(10);

    auto topRow = bounds.removeFromTop(24);
    titleLabel.setBounds(topRow.removeFromLeft(bounds.getWidth() / 2));
    timingLabel.setBounds(topRow.removeFromRight(130));
    quantizeLabel.setBounds(topRow.removeFromRight(140));

    const bool sceneView = isSceneRecorderView();
    for (auto& pattern : patterns)
    {
        pattern.nameLabel.setVisible(!sceneView);
        pattern.recordButton.setVisible(!sceneView);
        pattern.playButton.setVisible(!sceneView);
        pattern.stopButton.setVisible(!sceneView);
        pattern.clearButton.setVisible(!sceneView);
        pattern.statusLabel.setVisible(!sceneView);
        pattern.detailLabel.setVisible(!sceneView);
    }

    sceneControls.nameLabel.setVisible(sceneView);
    sceneControls.recordButton.setVisible(sceneView);
    sceneControls.clearButton.setVisible(sceneView);
    sceneControls.deleteButton.setVisible(sceneView);
    sceneControls.trimBeforeButton.setVisible(sceneView);
    sceneControls.trimAfterButton.setVisible(sceneView);
    sceneControls.clearTriggersButton.setVisible(sceneView);
    sceneControls.clearControlsButton.setVisible(sceneView);
    sceneControls.statusLabel.setVisible(sceneView);
    sceneControls.detailLabel.setVisible(sceneView);
    sceneControls.selectionLabel.setVisible(sceneView);

    if (sceneView)
    {
        auto sceneBounds = getSceneCardBounds().reduced(10, 8);
        auto header = sceneBounds.removeFromTop(18);
        sceneControls.nameLabel.setBounds(header.removeFromLeft(180));
        sceneControls.statusLabel.setBounds(header.removeFromRight(120));

        sceneBounds.removeFromTop(4);
        auto controls = sceneBounds.removeFromTop(24);
        sceneControls.recordButton.setBounds(controls.removeFromLeft(72));
        controls.removeFromLeft(4);
        sceneControls.clearButton.setBounds(controls.removeFromLeft(76));
        controls.removeFromLeft(4);
        sceneControls.deleteButton.setBounds(controls.removeFromLeft(72));
        controls.removeFromLeft(4);
        sceneControls.trimBeforeButton.setBounds(controls.removeFromLeft(72));
        controls.removeFromLeft(4);
        sceneControls.trimAfterButton.setBounds(controls.removeFromLeft(72));

        sceneBounds.removeFromTop(4);
        auto editRow = sceneBounds.removeFromTop(24);
        sceneControls.clearTriggersButton.setBounds(editRow.removeFromLeft(84));
        editRow.removeFromLeft(4);
        sceneControls.clearControlsButton.setBounds(editRow.removeFromLeft(84));

        sceneBounds.removeFromTop(4);
        sceneControls.detailLabel.setBounds(sceneBounds.removeFromTop(14));
        sceneBounds.removeFromTop(4);
        sceneControls.selectionLabel.setBounds(sceneBounds.removeFromTop(14));
        return;
    }

    for (int i = 0; i < 4; ++i)
    {
        auto patternBounds = getPatternCardBounds(i).reduced(10, 8);
        auto& pattern = patterns[i];

        auto header = patternBounds.removeFromTop(18);
        pattern.nameLabel.setBounds(header.removeFromLeft(130));
        pattern.statusLabel.setBounds(header.removeFromRight(120));

        patternBounds.removeFromTop(4);

        auto controls = patternBounds.removeFromTop(24);
        pattern.recordButton.setBounds(controls.removeFromLeft(64));
        controls.removeFromLeft(4);
        pattern.playButton.setBounds(controls.removeFromLeft(64));
        controls.removeFromLeft(4);
        pattern.stopButton.setBounds(controls.removeFromLeft(64));
        controls.removeFromLeft(4);
        pattern.clearButton.setBounds(controls.removeFromLeft(64));

        patternBounds.removeFromTop(4);
        pattern.detailLabel.setBounds(patternBounds.removeFromTop(14));
    }
}

void PatternControlPanel::timerCallback()
{
    if (!processor.getAudioEngine())
        return;
    
    updatePatternStates();
    resized();
    repaint();
}

bool PatternControlPanel::isSceneRecorderView() const
{
    return processor.isSceneModeEnabled();
}

juce::Rectangle<int> PatternControlPanel::getPatternCardBounds(int patternIndex) const
{
    auto bounds = getLocalBounds().reduced(10);
    bounds.removeFromTop(32);

    for (int i = 0; i < patternIndex; ++i)
    {
        bounds.removeFromTop(kPatternCardHeight);
        bounds.removeFromTop(kPatternCardGap);
    }

    return bounds.removeFromTop(kPatternCardHeight);
}

juce::Rectangle<int> PatternControlPanel::getSceneCardBounds() const
{
    auto bounds = getLocalBounds().reduced(10);
    bounds.removeFromTop(32);
    return bounds;
}

juce::Rectangle<float> PatternControlPanel::getSceneTimelineBounds() const
{
    auto timelineArea = getSceneCardBounds().toFloat().reduced(10.0f, 8.0f);
    timelineArea.removeFromTop(18.0f);
    timelineArea.removeFromTop(4.0f);
    timelineArea.removeFromTop(24.0f);
    timelineArea.removeFromTop(4.0f);
    timelineArea.removeFromTop(24.0f);
    timelineArea.removeFromTop(4.0f);
    timelineArea.removeFromTop(14.0f);
    timelineArea.removeFromTop(4.0f);
    timelineArea.removeFromTop(14.0f);
    timelineArea.removeFromTop(4.0f);
    return timelineArea;
}

void PatternControlPanel::paintPatternTimeline(juce::Graphics& g,
                                               int patternIndex,
                                               juce::Rectangle<float> bounds) const
{
    if (patternIndex < 0 || patternIndex >= 4 || bounds.getWidth() <= 0.0f || bounds.getHeight() <= 0.0f)
        return;

    auto* engine = processor.getAudioEngine();
    if (engine == nullptr)
        return;

    auto* pattern = engine->getPattern(patternIndex);
    if (pattern == nullptr)
        return;

    const auto& patternData = patterns[patternIndex];
    const int lengthBeats = juce::jmax(1, pattern->getLengthInBeats());

    g.setColour(kSurfaceDark.brighter(0.06f));
    g.fillRoundedRectangle(bounds, 5.0f);

    g.setColour(juce::Colours::white.withAlpha(0.06f));
    g.drawRoundedRectangle(bounds.reduced(0.5f), 5.0f, 1.0f);

    const float midY = bounds.getCentreY();
    g.setColour(juce::Colours::white.withAlpha(0.08f));
    g.drawHorizontalLine(static_cast<int>(midY), bounds.getX(), bounds.getRight());

    for (int beatIndex = 0; beatIndex <= lengthBeats; ++beatIndex)
    {
        const float x = bounds.getX() + (bounds.getWidth() * static_cast<float>(beatIndex) / static_cast<float>(lengthBeats));
        const float alpha = (beatIndex == 0 || beatIndex == lengthBeats) ? 0.20f : 0.10f;
        g.setColour(juce::Colours::white.withAlpha(alpha));
        g.drawVerticalLine(static_cast<int>(std::round(x)), bounds.getY(), bounds.getBottom());
    }

    if (patternData.events.empty())
    {
        g.setColour(kTextMuted.withAlpha(0.7f));
        g.setFont(juce::Font(juce::FontOptions(9.0f)));
        g.drawText("No recorded events", bounds.toNearestInt(), juce::Justification::centred);
    }
    else
    {
        for (const auto& event : patternData.events)
        {
            const float x = bounds.getX() + (bounds.getWidth() * static_cast<float>(event.time)
                                             / static_cast<float>(lengthBeats));

            if (event.type == PatternRecorder::EventType::ControlChange)
            {
                g.setColour(patternControlEventColour(event.controlMode).withAlpha(0.95f));
                g.fillRoundedRectangle(juce::Rectangle<float>(x - 1.5f,
                                                              bounds.getY() + 2.0f,
                                                              3.0f,
                                                              juce::jmax(4.0f, (midY - bounds.getY()) - 4.0f)),
                                       1.5f);
            }
            else
            {
                g.setColour(getStripColor(patternIndex).withAlpha(event.isNoteOn ? 0.95f : 0.55f));
                g.fillRoundedRectangle(juce::Rectangle<float>(x - 1.5f,
                                                              midY + 1.0f,
                                                              3.0f,
                                                              juce::jmax(4.0f, bounds.getBottom() - midY - 3.0f)),
                                       1.5f);
            }
        }
    }

    if (patternData.transportProgress >= 0.0f)
    {
        const float headX = bounds.getX() + (bounds.getWidth() * patternData.transportProgress);
        g.setColour((patternData.transportRecording ? juce::Colour(0xffd46b62) : juce::Colour(0xff76be7e)).withAlpha(0.95f));
        g.drawLine(headX, bounds.getY(), headX, bounds.getBottom(), 1.5f);
    }
}

void PatternControlPanel::paintSceneTimeline(juce::Graphics& g, juce::Rectangle<float> bounds) const
{
    if (bounds.getWidth() <= 0.0f || bounds.getHeight() <= 0.0f)
        return;

    auto* engine = processor.getAudioEngine();
    if (engine == nullptr)
        return;

    const int sceneSlot = processor.getActiveSceneSlot();
    const double lengthBeats = juce::jmax(1.0, processor.getScenePerformanceClipLengthBeats(sceneSlot));
    const auto layout = makeSceneTimelineLayout(bounds);
    const int controlLaneCount = 8;
    const int triggerLaneCount = MlrVSTAudioProcessor::MaxStrips;

    g.setColour(kSurfaceDark.brighter(0.06f));
    g.fillRoundedRectangle(layout.bounds, 5.0f);

    g.setColour(juce::Colours::white.withAlpha(0.06f));
    g.drawRoundedRectangle(layout.bounds.reduced(0.5f), 5.0f, 1.0f);

    for (int beatIndex = 0; beatIndex <= static_cast<int>(lengthBeats); ++beatIndex)
    {
        const float x = layout.bounds.getX()
            + (layout.bounds.getWidth() * static_cast<float>(beatIndex) / static_cast<float>(lengthBeats));
        const float alpha = (beatIndex == 0 || beatIndex == static_cast<int>(lengthBeats)) ? 0.20f : 0.10f;
        g.setColour(juce::Colours::white.withAlpha(alpha));
        g.drawVerticalLine(static_cast<int>(std::round(x)), layout.bounds.getY(), layout.bounds.getBottom());
    }

    g.setColour(juce::Colours::white.withAlpha(0.08f));
    for (int lane = 0; lane <= controlLaneCount; ++lane)
    {
        const float y = layout.controlArea.getY() + (layout.controlLaneHeight * static_cast<float>(lane));
        g.drawHorizontalLine(static_cast<int>(std::round(y)), layout.controlArea.getX(), layout.controlArea.getRight());
    }
    for (int lane = 0; lane <= triggerLaneCount; ++lane)
    {
        const float y = layout.triggerArea.getY() + (layout.triggerLaneHeight * static_cast<float>(lane));
        g.drawHorizontalLine(static_cast<int>(std::round(y)), layout.triggerArea.getX(), layout.triggerArea.getRight());
    }

    if (sceneControls.events.empty())
    {
        g.setColour(kTextMuted.withAlpha(0.7f));
        g.setFont(juce::Font(juce::FontOptions(9.0f)));
        g.drawText("No recorded scene events", layout.bounds.toNearestInt(), juce::Justification::centred);
    }
    else
    {
        for (int eventIndex = 0; eventIndex < static_cast<int>(sceneControls.events.size()); ++eventIndex)
        {
            const auto& event = sceneControls.events[static_cast<size_t>(eventIndex)];
            const bool isSelected = eventIndex == sceneControls.selectedEventIndex
                && sceneControls.selectedSceneSlot == sceneSlot;

            if (event.type == ScenePerformanceEventType::ControlPoint)
            {
                const auto marker = sceneControlMarkerBounds(layout, event, lengthBeats);
                g.setColour(patternControlEventColour(event.controlMode).withAlpha(0.95f));
                g.fillEllipse(marker.reduced(2.8f));
                if (isSelected)
                {
                    g.setColour(juce::Colours::white.withAlpha(0.9f));
                    g.drawEllipse(marker.reduced(1.6f), 1.2f);
                }
            }
            else
            {
                const auto marker = sceneTriggerMarkerBounds(layout, event, lengthBeats);
                const int laneIndex = juce::jlimit(0, triggerLaneCount - 1, event.stripIndex);
                g.setColour(getStripColor(laneIndex).withAlpha(event.isNoteOn ? 0.95f : 0.55f));
                g.fillRoundedRectangle(marker.reduced(2.4f, 1.0f), 1.5f);
                if (isSelected)
                {
                    g.setColour(juce::Colours::white.withAlpha(0.9f));
                    g.drawRoundedRectangle(marker.reduced(1.5f, 0.5f), 2.0f, 1.1f);
                }
            }
        }
    }

    g.setColour(kTextMuted.withAlpha(0.75f));
    g.setFont(juce::Font(juce::FontOptions(8.0f)));
    auto controlLabelArea = layout.controlArea;
    auto triggerLabelArea = layout.triggerArea;
    controlLabelArea.removeFromTop(10.0f);
    triggerLabelArea.removeFromTop(10.0f);
    g.drawText("CTRL", controlLabelArea.toNearestInt(), juce::Justification::topLeft);
    g.drawText("TRIG", triggerLabelArea.toNearestInt(), juce::Justification::topLeft);

    if (sceneControls.transportProgress >= 0.0f)
    {
        const float headX = layout.bounds.getX() + (layout.bounds.getWidth() * sceneControls.transportProgress);
        g.setColour((sceneControls.transportRecording ? juce::Colour(0xffd46b62) : juce::Colour(0xff76be7e)).withAlpha(0.95f));
        g.drawLine(headX, layout.bounds.getY(), headX, layout.bounds.getBottom(), 1.5f);
    }
}

void PatternControlPanel::updateSceneState(double beat)
{
    const int sceneSlot = processor.getActiveSceneSlot();
    const double lengthBeats = processor.getScenePerformanceClipLengthBeats(sceneSlot);
    const bool editingEnabled = !processor.isScenePerformanceRecording();

    if (sceneControls.selectedSceneSlot != sceneSlot)
    {
        sceneControls.selectedSceneSlot = sceneSlot;
        sceneControls.selectedEventIndex = -1;
        sceneControls.dragActive = false;
        sceneControls.dragEventIndex = -1;
        sceneControls.dragBaseEvents.clear();
    }

    sceneControls.events = processor.getScenePerformanceEventsSnapshot(sceneSlot);
    sceneControls.triggerEventCount = 0;
    sceneControls.controlEventCount = 0;
    sceneControls.transportProgress = -1.0f;
    sceneControls.transportRecording = false;

    if (sceneControls.selectedEventIndex >= static_cast<int>(sceneControls.events.size()))
        sceneControls.selectedEventIndex = -1;

    for (const auto& event : sceneControls.events)
    {
        if (event.type == ScenePerformanceEventType::ControlPoint)
            ++sceneControls.controlEventCount;
        else
            ++sceneControls.triggerEventCount;
    }

    sceneControls.nameLabel.setText("SCENE " + juce::String(sceneSlot + 1), juce::dontSendNotification);
    sceneControls.recordButton.setToggleState(processor.isScenePerformanceRecording(), juce::dontSendNotification);
    sceneControls.deleteButton.setEnabled(editingEnabled && sceneControls.selectedEventIndex >= 0);
    sceneControls.trimBeforeButton.setEnabled(editingEnabled && sceneControls.selectedEventIndex >= 0);
    sceneControls.trimAfterButton.setEnabled(editingEnabled && sceneControls.selectedEventIndex >= 0);
    sceneControls.clearTriggersButton.setEnabled(editingEnabled && sceneControls.triggerEventCount > 0);
    sceneControls.clearControlsButton.setEnabled(editingEnabled && sceneControls.controlEventCount > 0);
    sceneControls.clearButton.setEnabled(editingEnabled && !sceneControls.events.empty());

    const juce::String eventSummary = (sceneControls.triggerEventCount > 0 || sceneControls.controlEventCount > 0)
        ? (juce::String(sceneControls.triggerEventCount) + " trig • "
           + juce::String(sceneControls.controlEventCount) + " ctrl")
        : juce::String("No events recorded");
    const juce::String selectionText = (sceneControls.selectedEventIndex >= 0
                                        && sceneControls.selectedEventIndex < static_cast<int>(sceneControls.events.size()))
        ? ("Selected: " + describeSceneEvent(sceneControls.events[static_cast<size_t>(sceneControls.selectedEventIndex)]))
        : juce::String(editingEnabled
                           ? "Click, drag, or press Delete on a scene event to edit it"
                           : "Editing is disabled while the scene recorder is running");
    sceneControls.selectionLabel.setText(selectionText, juce::dontSendNotification);

    if (processor.isScenePerformanceRecording()
        && processor.getScenePerformanceRecordingSceneSlot() == sceneSlot)
    {
        const double startBeat = processor.getScenePerformanceRecordingStartBeat();
        const double endBeat = processor.getScenePerformanceRecordingEndBeat();
        const double beatsLeft = juce::jmax(0.0, endBeat - beat);
        if (endBeat > startBeat)
            sceneControls.transportProgress = static_cast<float>(juce::jlimit(0.0, 1.0, (beat - startBeat) / (endBeat - startBeat)));
        sceneControls.transportRecording = true;
        sceneControls.statusLabel.setText(processor.isScenePerformanceOverdubbing() ? "OVERDUB" : "RECORDING",
                                          juce::dontSendNotification);
        sceneControls.statusLabel.setColour(juce::Label::textColourId, juce::Colour(0xffd46b62));
        const juce::String prefix = processor.isScenePerformanceOverdubbing()
            ? juce::String("Layers onto current scene clip")
            : eventSummary;
        sceneControls.detailLabel.setText(prefix + " • Ends in " + juce::String(beatsLeft, 2) + " beats",
                                          juce::dontSendNotification);
    }
    else if (processor.hasScenePerformanceClip(sceneSlot))
    {
        sceneControls.transportProgress = static_cast<float>(processor.getScenePerformancePlaybackProgress(sceneSlot, beat));
        sceneControls.statusLabel.setText("LIVE", juce::dontSendNotification);
        sceneControls.statusLabel.setColour(juce::Label::textColourId, juce::Colour(0xff76be7e));
        sceneControls.detailLabel.setText("Len " + juce::String(lengthBeats, 2) + " beats • " + eventSummary,
                                          juce::dontSendNotification);
    }
    else
    {
        sceneControls.statusLabel.setText("EMPTY", juce::dontSendNotification);
        sceneControls.statusLabel.setColour(juce::Label::textColourId, kTextMuted);
        sceneControls.detailLabel.setText("Len " + juce::String(lengthBeats, 2) + " beats • No events recorded",
                                          juce::dontSendNotification);
    }
}

void PatternControlPanel::updatePatternStates()
{
    auto* engine = processor.getAudioEngine();
    if (engine == nullptr)
        return;

    const double beat = engine->getCurrentBeat();
    timingLabel.setText("Beat: " + juce::String(beat, 2), juce::dontSendNotification);
    titleLabel.setText(isSceneRecorderView() ? "Scene Recorder" : "Pattern Recorder", juce::dontSendNotification);

    if (auto* quantizeParam = processor.parameters.getRawParameterValue("quantize"))
    {
        static const char* values[] = {"1", "1/2", "1/2T", "1/4", "1/4T", "1/8", "1/8T", "1/16", "1/16T", "1/32"};
        int idx = juce::jlimit(0, 9, static_cast<int>(*quantizeParam));
        quantizeLabel.setText("Quantize: " + juce::String(values[idx]), juce::dontSendNotification);
    }

    if (isSceneRecorderView())
    {
        updateSceneState(beat);
        return;
    }

    for (int i = 0; i < 4; ++i)
    {
        auto* pattern = engine->getPattern(i);
        if (pattern)
        {
            patterns[i].events = pattern->getEventsSnapshot();
            patterns[i].triggerEventCount = 0;
            patterns[i].controlEventCount = 0;
            patterns[i].transportProgress = -1.0f;
            patterns[i].transportRecording = false;

            for (const auto& event : patterns[i].events)
            {
                if (event.type == PatternRecorder::EventType::ControlChange)
                    ++patterns[i].controlEventCount;
                else
                    ++patterns[i].triggerEventCount;
            }

            patterns[i].recordButton.setToggleState(pattern->isRecording(), juce::dontSendNotification);
            patterns[i].playButton.setToggleState(pattern->isPlaying(), juce::dontSendNotification);

            const int eventCount = pattern->getEventCount();
            const int lengthBeats = pattern->getLengthInBeats();
            const double startBeat = pattern->getRecordingStartBeat();
            const juce::String eventSummary = (eventCount > 0)
                ? (juce::String(patterns[i].triggerEventCount) + " trig • "
                   + juce::String(patterns[i].controlEventCount) + " ctrl")
                : juce::String("No events recorded");

            if (pattern->isRecording())
            {
                const double beatsLeft = juce::jmax(0.0, (startBeat + static_cast<double>(lengthBeats)) - beat);
                patterns[i].transportRecording = true;
                if (lengthBeats > 0)
                    patterns[i].transportProgress = static_cast<float>(juce::jlimit(0.0, 1.0, (beat - startBeat) / static_cast<double>(lengthBeats)));
                patterns[i].statusLabel.setText(pattern->isOverdubbing() ? "OVERDUB" : "RECORDING", juce::dontSendNotification);
                patterns[i].statusLabel.setColour(juce::Label::textColourId, juce::Colour(0xffd46b62));
                const juce::String prefix = pattern->isOverdubbing()
                    ? juce::String("Adds to current loop")
                    : eventSummary;
                patterns[i].detailLabel.setText(prefix + " • Ends in " + juce::String(beatsLeft, 2) + " beats",
                                                juce::dontSendNotification);
            }
            else if (pattern->isPlaying())
            {
                patterns[i].transportProgress = static_cast<float>(pattern->getPlaybackProgressForBeat(beat));
                patterns[i].statusLabel.setText("PLAYING", juce::dontSendNotification);
                patterns[i].statusLabel.setColour(juce::Label::textColourId, juce::Colour(0xff76be7e));
                patterns[i].detailLabel.setText("Len " + juce::String(lengthBeats) + " beats • " + eventSummary,
                                                juce::dontSendNotification);
            }
            else
            {
                if (eventCount > 0)
                {
                    patterns[i].statusLabel.setText("READY", juce::dontSendNotification);
                    patterns[i].statusLabel.setColour(juce::Label::textColourId, kAccent.withMultipliedBrightness(1.1f));
                    patterns[i].detailLabel.setText("Len " + juce::String(lengthBeats) + " beats • " + eventSummary,
                                                    juce::dontSendNotification);
                }
                else
                {
                    patterns[i].statusLabel.setText("EMPTY", juce::dontSendNotification);
                    patterns[i].statusLabel.setColour(juce::Label::textColourId, kTextMuted);
                    patterns[i].detailLabel.setText("Len " + juce::String(lengthBeats) + " beats • No events recorded",
                                                    juce::dontSendNotification);
                }
            }
        }
    }
}

bool PatternControlPanel::applyEditedSceneEvents(std::vector<ScenePerformanceEvent> events,
                                                 int preferredSelectedIndex,
                                                 const ScenePerformanceEvent* preferredSelectedEvent)
{
    if (!isSceneRecorderView() || processor.isScenePerformanceRecording())
        return false;

    const int sceneSlot = processor.getActiveSceneSlot();
    if (!processor.replaceScenePerformanceClipEvents(sceneSlot, events))
        return false;

    sceneControls.events = processor.getScenePerformanceEventsSnapshot(sceneSlot);
    sceneControls.selectedSceneSlot = sceneSlot;
    if (sceneControls.events.empty())
    {
        sceneControls.selectedEventIndex = -1;
    }
    else if (preferredSelectedEvent != nullptr)
    {
        sceneControls.selectedEventIndex = findBestMatchingSceneEventIndex(sceneControls.events, *preferredSelectedEvent);
    }
    else if (preferredSelectedIndex >= 0 && preferredSelectedIndex < static_cast<int>(sceneControls.events.size()))
    {
        sceneControls.selectedEventIndex = preferredSelectedIndex;
    }
    else
    {
        sceneControls.selectedEventIndex = juce::jlimit(-1,
                                                        static_cast<int>(sceneControls.events.size()) - 1,
                                                        sceneControls.selectedEventIndex);
    }

    repaint();
    return true;
}

bool PatternControlPanel::deleteSelectedSceneEvent()
{
    if (!isSceneRecorderView()
        || sceneControls.selectedEventIndex < 0
        || sceneControls.selectedEventIndex >= static_cast<int>(sceneControls.events.size()))
    {
        return false;
    }

    auto events = sceneControls.events;
    events.erase(events.begin() + sceneControls.selectedEventIndex);
    const int nextIndex = juce::jmin(sceneControls.selectedEventIndex, static_cast<int>(events.size()) - 1);
    return applyEditedSceneEvents(std::move(events), nextIndex, nullptr);
}

bool PatternControlPanel::trimSceneEventsToSelection(bool keepAfterSelection)
{
    if (!isSceneRecorderView()
        || sceneControls.selectedEventIndex < 0
        || sceneControls.selectedEventIndex >= static_cast<int>(sceneControls.events.size()))
    {
        return false;
    }

    const auto selectedEvent = sceneControls.events[static_cast<size_t>(sceneControls.selectedEventIndex)];
    auto events = sceneControls.events;
    const double pivotBeat = selectedEvent.timeBeats;
    const double epsilon = 1.0e-6;

    events.erase(std::remove_if(events.begin(),
                                events.end(),
                                [keepAfterSelection, pivotBeat, epsilon](const ScenePerformanceEvent& event)
                                {
                                    return keepAfterSelection
                                        ? (event.timeBeats + epsilon) < pivotBeat
                                        : (event.timeBeats - epsilon) > pivotBeat;
                                }),
                 events.end());

    return applyEditedSceneEvents(std::move(events), -1, &selectedEvent);
}

bool PatternControlPanel::clearSceneEventsByType(ScenePerformanceEventType type)
{
    if (!isSceneRecorderView())
        return false;

    auto events = sceneControls.events;
    events.erase(std::remove_if(events.begin(),
                                events.end(),
                                [type](const ScenePerformanceEvent& event)
                                {
                                    return event.type == type;
                                }),
                 events.end());

    const ScenePerformanceEvent* selectedEvent = nullptr;
    ScenePerformanceEvent selectedEventCopy;
    if (sceneControls.selectedEventIndex >= 0
        && sceneControls.selectedEventIndex < static_cast<int>(sceneControls.events.size()))
    {
        selectedEventCopy = sceneControls.events[static_cast<size_t>(sceneControls.selectedEventIndex)];
        if (selectedEventCopy.type != type)
            selectedEvent = &selectedEventCopy;
    }

    return applyEditedSceneEvents(std::move(events), -1, selectedEvent);
}

bool PatternControlPanel::handleEditorKeyPress(const juce::KeyPress& key)
{
    if (!isSceneRecorderView())
        return false;

    if (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey)
        return deleteSelectedSceneEvent();

    if (key == juce::KeyPress::escapeKey)
    {
        if (sceneControls.selectedEventIndex >= 0)
        {
            sceneControls.selectedEventIndex = -1;
            repaint();
            return true;
        }
    }

    return false;
}

void PatternControlPanel::mouseDown(const juce::MouseEvent& e)
{
    if (!isSceneRecorderView())
        return;

    const auto timelineBounds = getSceneTimelineBounds();
    if (!timelineBounds.contains(e.position))
        return;

    grabKeyboardFocus();

    const int sceneSlot = processor.getActiveSceneSlot();
    const double lengthBeats = juce::jmax(1.0, processor.getScenePerformanceClipLengthBeats(sceneSlot));
    const auto layout = makeSceneTimelineLayout(timelineBounds);

    int hitIndex = -1;
    for (int i = static_cast<int>(sceneControls.events.size()) - 1; i >= 0; --i)
    {
        const auto& event = sceneControls.events[static_cast<size_t>(i)];
        auto marker = event.type == ScenePerformanceEventType::ControlPoint
            ? sceneControlMarkerBounds(layout, event, lengthBeats)
            : sceneTriggerMarkerBounds(layout, event, lengthBeats);
        marker = marker.expanded(3.0f, 3.0f);
        if (marker.contains(e.position))
        {
            hitIndex = i;
            break;
        }
    }

    sceneControls.selectedSceneSlot = sceneSlot;
    sceneControls.selectedEventIndex = hitIndex;
    sceneControls.dragActive = false;
    sceneControls.dragEventIndex = -1;
    sceneControls.dragBaseEvents.clear();

    if (hitIndex >= 0 && !processor.isScenePerformanceRecording())
    {
        sceneControls.dragActive = true;
        sceneControls.dragEventIndex = hitIndex;
        sceneControls.dragBaseEvents = sceneControls.events;
    }

    repaint();
}

void PatternControlPanel::mouseDrag(const juce::MouseEvent& e)
{
    if (!isSceneRecorderView()
        || !sceneControls.dragActive
        || processor.isScenePerformanceRecording()
        || sceneControls.dragEventIndex < 0
        || sceneControls.dragEventIndex >= static_cast<int>(sceneControls.dragBaseEvents.size()))
    {
        return;
    }

    const int sceneSlot = processor.getActiveSceneSlot();
    const double lengthBeats = juce::jmax(1.0, processor.getScenePerformanceClipLengthBeats(sceneSlot));
    const auto layout = makeSceneTimelineLayout(getSceneTimelineBounds());
    if (layout.bounds.getWidth() <= 0.0f)
        return;

    auto editedEvents = sceneControls.dragBaseEvents;
    auto editedEvent = editedEvents[static_cast<size_t>(sceneControls.dragEventIndex)];

    const double maxBeat = juce::jmax(0.0, std::nextafter(lengthBeats, 0.0));
    const float normalizedX = juce::jlimit(0.0f, 1.0f, (e.position.x - layout.bounds.getX()) / layout.bounds.getWidth());
    editedEvent.timeBeats = juce::jlimit(0.0, maxBeat, static_cast<double>(normalizedX) * lengthBeats);

    if (editedEvent.type == ScenePerformanceEventType::ControlPoint)
    {
        const int laneIndex = juce::jlimit(0, 7, sceneControlLaneIndex(editedEvent));
        const float laneTop = layout.controlArea.getY() + (layout.controlLaneHeight * static_cast<float>(laneIndex));
        const float laneHeight = juce::jmax(4.0f, layout.controlLaneHeight - 4.0f);
        const float normalizedY = 1.0f - juce::jlimit(0.0f,
                                                      1.0f,
                                                      (e.position.y - (laneTop + 2.0f)) / laneHeight);
        editedEvent.value = denormalizeSceneControlValue(editedEvent, normalizedY);
    }
    else if (layout.triggerArea.contains(e.position))
    {
        const int laneIndex = juce::jlimit(0,
                                           MlrVSTAudioProcessor::MaxStrips - 1,
                                           static_cast<int>((e.position.y - layout.triggerArea.getY())
                                                            / juce::jmax(1.0f, layout.triggerLaneHeight)));
        editedEvent.stripIndex = laneIndex;
    }

    editedEvents[static_cast<size_t>(sceneControls.dragEventIndex)] = editedEvent;
    applyEditedSceneEvents(std::move(editedEvents), -1, &editedEvent);
}

void PatternControlPanel::mouseUp(const juce::MouseEvent&)
{
    sceneControls.dragActive = false;
    sceneControls.dragEventIndex = -1;
    sceneControls.dragBaseEvents.clear();
}

//==============================================================================
// GroupControlPanel Implementation
//==============================================================================

GroupControlPanel::GroupControlPanel(MlrVSTAudioProcessor& p)
    : processor(p)
{
    // Title
    titleLabel.setText("Mute Groups", juce::dontSendNotification);
    titleLabel.setFont(juce::Font(juce::FontOptions(14.0f, juce::Font::bold)));
    titleLabel.setColour(juce::Label::textColourId, kTextPrimary);
    addAndMakeVisible(titleLabel);
    
    // Group controls
    for (int i = 0; i < 4; ++i)
    {
        auto& group = groups[i];
        
        group.nameLabel.setText("Group " + juce::String(i + 1), juce::dontSendNotification);
        group.nameLabel.setFont(juce::Font(juce::FontOptions(12.0f, juce::Font::bold)));
        addAndMakeVisible(group.nameLabel);
        
        group.muteButton.setButtonText("Mute");
        group.muteButton.setToggleable(true);
        group.muteButton.setTooltip("Mute/unmute this group.");
        group.muteButton.onClick = [this, i]()
        {
            if (auto* grp = processor.getAudioEngine()->getGroup(i))
            {
                grp->setMuted(groups[i].muteButton.getToggleState());
            }
        };
        addAndMakeVisible(group.muteButton);
        styleUiButton(group.muteButton, true);
        
        group.volumeSlider.setSliderStyle(juce::Slider::LinearHorizontal);
        group.volumeSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 50, 20);
        group.volumeSlider.setRange(0.0, 1.0, 0.01);
        group.volumeSlider.setValue(1.0);
        enableAltClickReset(group.volumeSlider, 1.0);
        group.volumeSlider.onValueChange = [this, i]()
        {
            if (auto* grp = processor.getAudioEngine()->getGroup(i))
            {
                grp->setVolume(static_cast<float>(groups[i].volumeSlider.getValue()));
            }
        };
        addAndMakeVisible(group.volumeSlider);
        
        group.statusLabel.setText("No strips", juce::dontSendNotification);
        group.statusLabel.setFont(juce::Font(juce::FontOptions(10.0f)));
        group.statusLabel.setColour(juce::Label::textColourId, kTextMuted);
        addAndMakeVisible(group.statusLabel);
    }
    
    startTimer(200);
}

void GroupControlPanel::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();

    juce::ColourGradient pageFill(kPanelTop.darker(0.35f), 0.0f, 0.0f,
                                  kPanelBottom.darker(0.4f), 0.0f, bounds.getBottom(), false);
    g.setGradientFill(pageFill);
    g.fillAll();
    
    // Rainbow colors for 4 groups
    const juce::Colour rainbowColors[] = {
        juce::Colour(0xff5ea5a8),
        juce::Colour(0xff6f93c8),
        juce::Colour(0xffd36f63),
        juce::Colour(0xffd18f4f)
    };
    
    // Draw rounded rectangles with rainbow dividers for each group
    float groupHeight = (bounds.getHeight() - 60.0f) / 4.0f;  // Account for title
    float startY = 40.0f;
    
    for (int i = 0; i < 4; ++i)
    {
        auto groupBounds = juce::Rectangle<float>(
            bounds.getX() + 4.0f,
            startY + (i * groupHeight),
            bounds.getWidth() - 8.0f,
            groupHeight - 4.0f
        );
        
        // Rounded background
        g.setColour(juce::Colour(0xff2b2b2b));
        g.fillRoundedRectangle(groupBounds, 8.0f);
        
        // Rainbow divider at bottom
        g.setColour(rainbowColors[i]);
        auto dividerRect = juce::Rectangle<float>(
            groupBounds.getX() + 8.0f,
            groupBounds.getBottom() - 6.0f,
            groupBounds.getWidth() - 16.0f,
            2.0f
        );
        g.fillRoundedRectangle(dividerRect, 1.0f);
    }
}

void GroupControlPanel::resized()
{
    auto bounds = getLocalBounds();
    
    // Title at top
    auto titleRow = bounds.removeFromTop(32);
    titleLabel.setBounds(titleRow.reduced(12, 6));
    
    bounds.removeFromTop(8);
    
    // Calculate group height
    float groupHeight = bounds.getHeight() / 4.0f;
    
    // Group rows - inside rounded rectangles
    for (int i = 0; i < 4; ++i)
    {
        auto groupBounds = bounds.removeFromTop(static_cast<int>(groupHeight));
        groupBounds.reduce(12, 8);  // Padding inside rounded rect
        
        auto& group = groups[i];
        
        // Name row
        auto nameRow = groupBounds.removeFromTop(22);
        group.nameLabel.setBounds(nameRow);
        
        groupBounds.removeFromTop(4);
        
        // Control row
        auto controlRow = groupBounds.removeFromTop(28);
        group.muteButton.setBounds(controlRow.removeFromLeft(82));
        controlRow.removeFromLeft(6);
        group.volumeSlider.setBounds(controlRow.removeFromLeft(140));
        controlRow.removeFromLeft(10);
        group.statusLabel.setBounds(controlRow);
    }
}

void GroupControlPanel::timerCallback()
{
    if (!processor.getAudioEngine())
        return;
    
    updateGroupStates();
}

void GroupControlPanel::updateGroupStates()
{
    for (int i = 0; i < 4; ++i)
    {
        auto* group = processor.getAudioEngine()->getGroup(i);
        if (group)
        {
            auto strips = group->getStrips();
            if (strips.empty())
            {
                groups[i].statusLabel.setText("No strips", juce::dontSendNotification);
            }
            else
            {
                juce::String stripList;
                for (size_t j = 0; j < strips.size(); ++j)
                {
                    if (j > 0) stripList += ", ";
                    stripList += juce::String(strips[j] + 1);
                }
                groups[i].statusLabel.setText("Strips: " + stripList, juce::dontSendNotification);
            }
            
            groups[i].muteButton.setToggleState(group->isMuted(), juce::dontSendNotification);
        }
    }
}

//==============================================================================
// ModulationControlPanel Implementation
//==============================================================================

ModulationControlPanel::ModulationControlPanel(MlrVSTAudioProcessor& p)
    : processor(p)
{
    titleLabel.setText("Per-Row Modulation Sequencer", juce::dontSendNotification);
    titleLabel.setColour(juce::Label::textColourId, kTextPrimary);
    titleLabel.setFont(juce::Font(juce::FontOptions(14.0f, juce::Font::bold)));
    addAndMakeVisible(titleLabel);

    stripLabel.setColour(juce::Label::textColourId, kAccent);
    stripLabel.setFont(juce::Font(juce::FontOptions(12.0f, juce::Font::bold)));
    addAndMakeVisible(stripLabel);

    sceneStripToolsLabel.setText("Strip Ops", juce::dontSendNotification);
    sceneStripToolsLabel.setColour(juce::Label::textColourId, kTextMuted);
    sceneStripToolsLabel.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
    sceneStripToolsLabel.setJustificationType(juce::Justification::centredLeft);
    sceneStripToolsLabel.setVisible(false);
    addAndMakeVisible(sceneStripToolsLabel);

    sceneStripWriteButton.setButtonText("Write");
    sceneStripWriteButton.setTooltip("Write the current live strip state into this scene strip and hold it through the scene.");
    sceneStripWriteButton.onClick = [this]()
    {
        if (pinnedContextActive && onSceneStripWrite != nullptr)
            onSceneStripWrite(selectedStrip);
    };
    sceneStripWriteButton.setVisible(false);
    addAndMakeVisible(sceneStripWriteButton);
    styleUiButton(sceneStripWriteButton);

    sceneStripWriteAllButton.setButtonText("All");
    sceneStripWriteAllButton.setTooltip("Write the current live state for every strip and hold it through the scene.");
    sceneStripWriteAllButton.onClick = [this]()
    {
        if (pinnedContextActive && onSceneStripWriteAll != nullptr)
            onSceneStripWriteAll();
    };
    sceneStripWriteAllButton.setVisible(false);
    addAndMakeVisible(sceneStripWriteAllButton);
    styleUiButton(sceneStripWriteAllButton);

    sceneStripClearButton.setButtonText("Clr");
    sceneStripClearButton.setTooltip("Clear the current strip's scene events and reset its scene motion lanes.");
    sceneStripClearButton.onClick = [this]()
    {
        if (pinnedContextActive && onSceneStripClear != nullptr)
            onSceneStripClear(selectedStrip);
    };
    sceneStripClearButton.setVisible(false);
    addAndMakeVisible(sceneStripClearButton);
    styleUiButton(sceneStripClearButton);

    sceneStripDuplicateButton.setButtonText("Dup");
    sceneStripDuplicateButton.setTooltip("Duplicate this strip's scene data into the next strip.");
    sceneStripDuplicateButton.onClick = [this]()
    {
        if (pinnedContextActive && onSceneStripDuplicate != nullptr)
            onSceneStripDuplicate(selectedStrip);
    };
    sceneStripDuplicateButton.setVisible(false);
    addAndMakeVisible(sceneStripDuplicateButton);
    styleUiButton(sceneStripDuplicateButton);

    sceneStripCopyButton.setButtonText("Copy");
    sceneStripCopyButton.setTooltip("Copy this strip's scene data to another strip.");
    sceneStripCopyButton.onClick = [this]() { showSceneStripCopyMenu(); };
    sceneStripCopyButton.setVisible(false);
    addAndMakeVisible(sceneStripCopyButton);
    styleUiButton(sceneStripCopyButton);

    targetLabel.setText("Target", juce::dontSendNotification);
    targetLabel.setColour(juce::Label::textColourId, kTextMuted);
    addAndMakeVisible(targetLabel);

    for (auto target : kModPerformanceTargetOrder)
        targetBox.addItem(performanceTargetDisplayName(target), performanceTargetToComboId(target));
    targetBox.onChange = [this]()
    {
        if (auto* engine = processor.getAudioEngine())
        {
            ensurePinnedSlotSelected(*engine);
            const auto target = comboIdToModTarget(targetBox.getSelectedId());
            if (pinnedContextActive && selectedSlotOverride >= 0)
            {
                processor.setSceneMotionTargetForSlot(selectedStrip, selectedSlotOverride, target);
                ensurePinnedSlotSelected(*engine);
            }
            else
            {
                engine->setModTarget(selectedStrip, target);
            }
            if (target == ModernAudioEngine::ModTarget::Rearrange)
                engine->setModEditPage(selectedStrip, 0);
            bipolarToggle.setToggleState(engine->isModBipolar(selectedStrip), juce::dontSendNotification);
            syncPinnedSceneMotionIfNeeded();
        }
        refreshFromEngine();
    };
    addAndMakeVisible(targetBox);

    bipolarToggle.setButtonText("Bipolar");
    bipolarToggle.setTooltip("Click: convert existing steps so neutral is preserved. Option-click: reinterpret stored values without remapping.");
    bipolarToggle.onClick = [this]()
    {
        if (auto* engine = processor.getAudioEngine())
        {
            ensurePinnedSlotSelected(*engine);
            const auto mode = juce::ModifierKeys::getCurrentModifiersRealtime().isAltDown()
                ? ModernAudioEngine::ModBipolarToggleMode::Reinterpret
                : ModernAudioEngine::ModBipolarToggleMode::ConvertPreserveNeutral;
            engine->setModBipolar(selectedStrip, bipolarToggle.getToggleState(), mode);
            syncPinnedSceneMotionIfNeeded();
        }
    };
    addAndMakeVisible(bipolarToggle);

    depthLabel.setText("Depth", juce::dontSendNotification);
    depthLabel.setColour(juce::Label::textColourId, kTextMuted);
    addAndMakeVisible(depthLabel);

    depthSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    depthSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 34, 16);
    depthSlider.setRange(0.0, 1.0, 0.01);
    depthSlider.onValueChange = [this]()
    {
        if (auto* engine = processor.getAudioEngine())
        {
            ensurePinnedSlotSelected(*engine);
            engine->setModDepth(selectedStrip, static_cast<float>(depthSlider.getValue()));
            syncPinnedSceneMotionIfNeeded();
        }
    };
    addAndMakeVisible(depthSlider);

    rateLabel.setText("Rate", juce::dontSendNotification);
    rateLabel.setColour(juce::Label::textColourId, kTextMuted);
    addAndMakeVisible(rateLabel);

    for (size_t idx = 0; idx < modRateChoices().size(); ++idx)
        rateBox.addItem(modRateLabelForValue(modRateChoices()[idx]), static_cast<int>(idx) + 1);
    rateBox.onChange = [this]()
    {
        if (auto* engine = processor.getAudioEngine())
        {
            ensurePinnedSlotSelected(*engine);
            engine->setModRate(selectedStrip, comboIdToModRate(rateBox.getSelectedId()));
            syncPinnedSceneMotionIfNeeded();
        }
    };
    addAndMakeVisible(rateBox);

    transportLabel.setText("Clock", juce::dontSendNotification);
    transportLabel.setColour(juce::Label::textColourId, kTextMuted);
    addAndMakeVisible(transportLabel);

    transportBox.addItem("Scene Loop", static_cast<int>(ModernAudioEngine::ModTransportMode::Free) + 1);
    transportBox.addItem("Follow Scene", static_cast<int>(ModernAudioEngine::ModTransportMode::Scene) + 1);
    transportBox.addItem("Follow Strip", static_cast<int>(ModernAudioEngine::ModTransportMode::Sync) + 1);
    transportBox.onChange = [this]()
    {
        if (auto* engine = processor.getAudioEngine())
        {
            ensurePinnedSlotSelected(*engine);
            engine->setModTransportMode(selectedStrip,
                static_cast<ModernAudioEngine::ModTransportMode>(juce::jlimit(
                    0,
                    static_cast<int>(ModernAudioEngine::ModTransportMode::Scene),
                    transportBox.getSelectedId() - 1)));
            syncPinnedSceneMotionIfNeeded();
        }
    };
    addAndMakeVisible(transportBox);

    offsetLabel.setVisible(false);
    offsetSlider.setVisible(false);

    lengthLabel.setText("Length", juce::dontSendNotification);
    lengthLabel.setColour(juce::Label::textColourId, kTextMuted);
    addAndMakeVisible(lengthLabel);

    rebuildLengthAndPageBoxes(ModernAudioEngine::MaxModBars);
    lengthBox.onChange = [this]()
    {
        if (auto* engine = processor.getAudioEngine())
        {
            ensurePinnedSlotSelected(*engine);
            const int maxBars = maxLengthBarsForCurrentContext();
            const int bars = juce::jlimit(1, maxBars, lengthBox.getSelectedId());
            const int currentPage = (pinnedContextActive && selectedSlotOverride >= 0)
                ? engine->getModEditPageForSlot(selectedStrip, selectedSlotOverride)
                : engine->getModEditPage(selectedStrip);
            if (pinnedContextActive && selectedSlotOverride >= 0)
            {
                engine->setModLengthBarsForSlot(selectedStrip, selectedSlotOverride, bars);
                engine->setModEditPage(selectedStrip, juce::jlimit(0, juce::jmax(0, bars - 1), currentPage));
            }
            else
            {
                engine->setModLengthBars(selectedStrip, bars);
                engine->setModEditPage(selectedStrip, juce::jlimit(0, juce::jmax(0, bars - 1), currentPage));
            }
            syncPinnedSceneMotionIfNeeded();
        }
        refreshFromEngine();
        resized();
    };
    addAndMakeVisible(lengthBox);

    pageLabel.setText("Page", juce::dontSendNotification);
    pageLabel.setColour(juce::Label::textColourId, kTextMuted);
    addAndMakeVisible(pageLabel);

    pageBox.onChange = [this]()
    {
        if (auto* engine = processor.getAudioEngine())
        {
            ensurePinnedSlotSelected(*engine);
            const int maxPage = juce::jmax(0, maxLengthBarsForCurrentContext() - 1);
            engine->setModEditPage(selectedStrip, juce::jlimit(0, maxPage, pageBox.getSelectedId() - 1));
            syncPinnedSceneMotionIfNeeded();
        }
        refreshFromEngine();
    };
    addAndMakeVisible(pageBox);

    smoothLabel.setText("Smooth", juce::dontSendNotification);
    smoothLabel.setColour(juce::Label::textColourId, kTextMuted);
    addAndMakeVisible(smoothLabel);

    smoothSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    smoothSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 34, 16);
    smoothSlider.setRange(0.0, 250.0, 1.0);
    smoothSlider.setSkewFactorFromMidPoint(40.0);
    smoothSlider.onValueChange = [this]()
    {
        if (auto* engine = processor.getAudioEngine())
        {
            ensurePinnedSlotSelected(*engine);
            engine->setModSmoothingMs(selectedStrip, static_cast<float>(smoothSlider.getValue()));
            syncPinnedSceneMotionIfNeeded();
        }
    };
    addAndMakeVisible(smoothSlider);

    shapeLabel.setText("Shape", juce::dontSendNotification);
    shapeLabel.setColour(juce::Label::textColourId, kTextMuted);
    addAndMakeVisible(shapeLabel);

    shapeBox.addItem("Curve", 1);
    shapeBox.addItem("Steps", 2);
    shapeBox.onChange = [this]()
    {
        if (auto* engine = processor.getAudioEngine())
        {
            ensurePinnedSlotSelected(*engine);
            const bool curveMode = (shapeBox.getSelectedId() == 1);
            engine->setModCurveMode(selectedStrip, curveMode);
            syncPinnedSceneMotionIfNeeded();
        }
        refreshFromEngine();
        resized();
        repaint();
    };
    addAndMakeVisible(shapeBox);

    curveBendLabel.setText("Bend", juce::dontSendNotification);
    curveBendLabel.setColour(juce::Label::textColourId, kTextMuted);
    addAndMakeVisible(curveBendLabel);

    curveBendSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    curveBendSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 34, 16);
    curveBendSlider.setRange(-1.0, 1.0, 0.01);
    curveBendSlider.onValueChange = [this]()
    {
        if (auto* engine = processor.getAudioEngine())
        {
            ensurePinnedSlotSelected(*engine);
            engine->setModCurveBend(selectedStrip, static_cast<float>(curveBendSlider.getValue()));
            syncPinnedSceneMotionIfNeeded();
        }
    };
    addAndMakeVisible(curveBendSlider);

    curveTypeLabel.setText("Curve", juce::dontSendNotification);
    curveTypeLabel.setColour(juce::Label::textColourId, kTextMuted);
    addAndMakeVisible(curveTypeLabel);

    curveTypeBox.addItem("Normal", 1);
    curveTypeBox.addItem("Exp+", 2);
    curveTypeBox.addItem("Exp-", 3);
    curveTypeBox.addItem("Sine", 4);
    curveTypeBox.addItem("Square", 5);
    curveTypeBox.onChange = [this]()
    {
        if (auto* engine = processor.getAudioEngine())
        {
            ensurePinnedSlotSelected(*engine);
            engine->setModCurveShape(selectedStrip, comboIdToCurveShape(curveTypeBox.getSelectedId()));
            syncPinnedSceneMotionIfNeeded();
        }
    };
    addAndMakeVisible(curveTypeBox);

    pitchScaleToggle.setButtonText("Pitch Quantize");
    pitchScaleToggle.onClick = [this]()
    {
        if (auto* engine = processor.getAudioEngine())
        {
            ensurePinnedSlotSelected(*engine);
            engine->setModPitchScaleQuantize(selectedStrip, pitchScaleToggle.getToggleState());
            syncPinnedSceneMotionIfNeeded();
        }
    };
    addAndMakeVisible(pitchScaleToggle);

    pitchScaleLabel.setText("Scale", juce::dontSendNotification);
    pitchScaleLabel.setColour(juce::Label::textColourId, kTextMuted);
    addAndMakeVisible(pitchScaleLabel);

    pitchScaleBox.addItem("None", 1);
    pitchScaleBox.addItem("Major", 2);
    pitchScaleBox.addItem("Minor", 3);
    pitchScaleBox.addItem("Dorian", 4);
    pitchScaleBox.addItem("Pentatonic", 5);
    pitchScaleBox.onChange = [this]()
    {
        if (auto* engine = processor.getAudioEngine())
        {
            ensurePinnedSlotSelected(*engine);
            engine->setModPitchScale(selectedStrip, comboIdToPitchScale(pitchScaleBox.getSelectedId()));
            syncPinnedSceneMotionIfNeeded();
        }
    };
    addAndMakeVisible(pitchScaleBox);

    targetHintLabel.setColour(juce::Label::textColourId, kTextMuted.brighter(0.15f));
    targetHintLabel.setFont(juce::Font(juce::FontOptions(11.0f)));
    targetHintLabel.setJustificationType(juce::Justification::centredLeft);
    targetHintLabel.setVisible(false);
    addAndMakeVisible(targetHintLabel);

    gestureHintLabel.setText("Graph edits: drag draws. Cmd=Divide  Ctrl=Ramp+  Opt=Ramp-",
                             juce::dontSendNotification);
    gestureHintLabel.setColour(juce::Label::textColourId, kTextMuted);
    gestureHintLabel.setFont(juce::Font(juce::FontOptions(11.0f)));
    gestureHintLabel.setJustificationType(juce::Justification::centredLeft);
    gestureHintLabel.setTooltip("The motion graph supports draw, divide, and ramp gestures.");
    addAndMakeVisible(gestureHintLabel);

    for (int i = 0; i < ModernAudioEngine::ModTotalSteps; ++i)
    {
        auto& b = stepButtons[static_cast<size_t>(i)];
        b.setButtonText(juce::String(i + 1));
        b.setTooltip("Click: toggle step. Cmd+drag: divide. Ctrl+drag: ramp up. Opt+drag: ramp down.");
        b.onClick = [this, i]()
        {
            if (suppressNextStepClick)
            {
                suppressNextStepClick = false;
                return;
            }
            if (getStepCellModifierGesture(juce::ModifierKeys::getCurrentModifiersRealtime()) != StepCellModifierGesture::None)
                return;
            if (auto* engine = processor.getAudioEngine())
            {
                ensurePinnedSlotSelected(*engine);
                const int absoluteStep = absoluteLegacyModStepForVisibleIndex(i);
                if (engine->getModSequencerState(selectedStrip).target == ModernAudioEngine::ModTarget::Rearrange)
                {
                    const float currentValue = juce::jlimit(0.0f, 1.0f, engine->getModStepValueAbsolute(selectedStrip, absoluteStep));
                    const int nextSlice = (rearrangeSliceDisplayIndex(currentValue) % ModernAudioEngine::MaxColumns) + 1;
                    engine->setModStepValueAbsolute(selectedStrip,
                                                    absoluteStep,
                                                    quantizeRearrangeStepValue((static_cast<float>(nextSlice) - 1.0f)
                                                                               / static_cast<float>(juce::jmax(1, ModernAudioEngine::MaxColumns - 1))));
                }
                else
                {
                    const float currentValue = juce::jlimit(0.0f, 1.0f, engine->getModStepValueAbsolute(selectedStrip, absoluteStep));
                    const float nextValue = currentValue > 0.001f ? 0.0f : 1.0f;
                    engine->setModStepValueAbsolute(selectedStrip, absoluteStep, nextValue);
                    engine->setModStepShapeAbsolute(selectedStrip, absoluteStep, 1, nextValue);
                }
                syncPinnedSceneMotionIfNeeded();
            }
            refreshFromEngine();
        };
        b.addMouseListener(this, true);
        addAndMakeVisible(b);
    }

    startTimer(80);
    refreshFromEngine();
}

void ModulationControlPanel::setPinnedStripAndSlot(int stripIndex, int slot, const juce::String& contextLabel)
{
    selectedStrip = juce::jlimit(0, MlrVSTAudioProcessor::MaxStrips - 1, stripIndex);
    selectedSlotOverride = juce::jlimit(0, ModernAudioEngine::NumModSequencers - 1, slot);
    pinnedContextActive = true;
    pinnedContextLabel = contextLabel;
    if (auto* engine = processor.getAudioEngine())
        engine->setModSequencerSlot(selectedStrip, selectedSlotOverride);
    refreshFromEngine();
}

void ModulationControlPanel::clearPinnedStripAndSlot()
{
    pinnedContextActive = false;
    pinnedContextLabel.clear();
    selectedSlotOverride = -1;
    refreshFromEngine();
}

int ModulationControlPanel::maxLengthBarsForCurrentContext() const
{
    if (!pinnedContextActive)
        return ModernAudioEngine::MaxModBars;

    const int sceneSlot = juce::jlimit(0,
                                       MlrVSTAudioProcessor::SceneSlots - 1,
                                       processor.getActiveSceneSlot());
    const double resolvedBeats = processor.getResolvedSceneLengthBeats(sceneSlot);
    if (!std::isfinite(resolvedBeats) || resolvedBeats <= 0.0)
        return ModernAudioEngine::MaxModBars;

    const int sceneBars = juce::jmax(1, static_cast<int>(std::ceil(resolvedBeats / 4.0)));
    return juce::jlimit(1, ModernAudioEngine::MaxModBars, sceneBars);
}

void ModulationControlPanel::rebuildLengthAndPageBoxes(int maxBars)
{
    const int safeMaxBars = juce::jlimit(1, ModernAudioEngine::MaxModBars, maxBars);
    const int currentLength = lengthBox.getSelectedId();
    const int currentPage = pageBox.getSelectedId();

    lengthBox.clear(juce::dontSendNotification);
    pageBox.clear(juce::dontSendNotification);
    for (int bars = 1; bars <= safeMaxBars; ++bars)
    {
        lengthBox.addItem(juce::String(bars), bars);
        pageBox.addItem(juce::String(bars), bars);
    }

    lengthBox.setSelectedId(juce::jlimit(1, safeMaxBars, currentLength <= 0 ? 1 : currentLength),
                            juce::dontSendNotification);
    pageBox.setSelectedId(juce::jlimit(1, safeMaxBars, currentPage <= 0 ? 1 : currentPage),
                          juce::dontSendNotification);
}

juce::Rectangle<int> ModulationControlPanel::getEmbeddedSceneSelectionToolsBounds() const
{
    return embeddedSceneSelectionToolsBounds;
}

juce::Rectangle<int> ModulationControlPanel::getEmbeddedSceneOverlayToolsBounds() const
{
    return embeddedSceneOverlayToolsBounds;
}

void ModulationControlPanel::paint(juce::Graphics& g)
{
    drawPanel(g, getLocalBounds().toFloat(), kAccent, 8.0f);
    paintLegacyModulationGraph(g);
}

void ModulationControlPanel::paintLegacyModulationGraph(juce::Graphics& g) const
{
    auto* engine = processor.getAudioEngine();
    if (engine == nullptr || graphBounds.isEmpty())
        return;

    ensurePinnedSlotSelected(*engine);
    const auto seq = engine->getModSequencerState(selectedStrip);
    const int slot = selectedSlotOverride >= 0
        ? selectedSlotOverride
        : engine->getModSequencerSlot(selectedStrip);
    const int lengthBars = juce::jlimit(1, ModernAudioEngine::MaxModBars, engine->getModLengthBars(selectedStrip));
    const int totalSteps = juce::jmax(ModernAudioEngine::ModSteps, lengthBars * ModernAudioEngine::ModSteps);
    const int activeStep = juce::jlimit(0, totalSteps - 1, engine->getModCurrentGlobalStep(selectedStrip));
    if (totalSteps <= 0)
        return;

    const auto stripColour = getStripColor(selectedStrip);
    auto lane = graphBounds;
    g.setColour(juce::Colour(0xff1f1f1f));
    g.fillRoundedRectangle(lane, 6.0f);
    g.setColour(stripColour.withAlpha(0.35f));
    g.drawRoundedRectangle(lane.reduced(0.5f), 6.0f, 1.0f);

    const juce::String laneInfo = "M" + juce::String(slot + 1)
        + "  PAGE " + juce::String(seq.editPage + 1) + "/" + juce::String(lengthBars);
    auto infoBadge = juce::Rectangle<float>(lane.getX() + 8.0f, lane.getY() + 4.0f, 108.0f, 14.0f);
    g.setColour(juce::Colour(0xff111111).withAlpha(0.72f));
    g.fillRoundedRectangle(infoBadge, 3.0f);
    g.setColour(stripColour.withAlpha(0.22f));
    g.drawRoundedRectangle(infoBadge, 3.0f, 1.0f);
    g.setColour(kTextPrimary.withAlpha(0.88f));
    g.setFont(8.5f);
    g.drawText(laneInfo, infoBadge.toNearestInt(), juce::Justification::centred, false);

    const auto drawLane = lane.reduced(12.0f, 2.0f);
    const float dotSize = (totalSteps > 32) ? 4.0f : 6.0f;
    const float dotPad = dotSize * 0.6f;
    const float left = drawLane.getX() + dotPad;
    const float right = juce::jmax(left, drawLane.getRight() - 1.0f - dotPad);
    const float top = drawLane.getY() + 2.0f;
    const float bottom = drawLane.getBottom() - 2.0f;
    const float width = juce::jmax(1.0f, right - left);
    const float height = bottom - top;
    const float stepWidth = juce::jmax(0.25f, width / static_cast<float>(juce::jmax(1, totalSteps)));
    const float centerY = top + (height * 0.5f);

    if (seq.bipolar)
    {
        g.setColour(juce::Colour(0xff454545));
        g.drawLine(left, centerY, right, centerY, 1.0f);
    }

    auto valueToY = [&](float v) -> float
    {
        const float clamped = juce::jlimit(0.0f, 1.0f, v);
        const float n = seq.bipolar ? ((clamped * 2.0f) - 1.0f) : clamped;
        return seq.bipolar
            ? (centerY - (n * (height * 0.48f)))
            : (bottom - (n * height));
    };

    std::vector<float> startValues(static_cast<size_t>(totalSteps));
    std::vector<float> endValues(static_cast<size_t>(totalSteps));
    std::vector<int> subdivisions(static_cast<size_t>(totalSteps));
    std::vector<ModernAudioEngine::ModCurveShape> stepCurveShapes(static_cast<size_t>(totalSteps),
                                                                  ModernAudioEngine::ModCurveShape::Linear);
    for (int i = 0; i < totalSteps; ++i)
    {
        const float startValue = juce::jlimit(0.0f, 1.0f, engine->getModStepValueAbsolute(selectedStrip, i));
        const int subdiv = juce::jlimit(1,
                                        ModernAudioEngine::ModMaxStepSubdivisions,
                                        engine->getModStepSubdivisionAbsolute(selectedStrip, i));
        float endValue = juce::jlimit(0.0f, 1.0f, engine->getModStepEndValueAbsolute(selectedStrip, i));
        const auto stepCurveShape = engine->getModStepCurveShapeAbsolute(selectedStrip, i);
        if (subdiv <= 1)
            endValue = startValue;
        startValues[static_cast<size_t>(i)] = startValue;
        endValues[static_cast<size_t>(i)] = endValue;
        subdivisions[static_cast<size_t>(i)] = subdiv;
        stepCurveShapes[static_cast<size_t>(i)] = stepCurveShape;
    }

    const float activeStepX = left + (stepWidth * static_cast<float>(activeStep));
    g.setColour(kAccent.withAlpha(0.10f));
    g.fillRect(activeStepX, top, juce::jmax(1.0f, stepWidth), juce::jmax(1.0f, height));

    const float bend = juce::jlimit(-1.0f, 1.0f, seq.curveBend);
    std::vector<juce::Point<float>> stepMarkerPoints(static_cast<size_t>(totalSteps));
    for (int i = 0; i < totalSteps; ++i)
    {
        const float markerPhase = seq.curveMode ? shapeSubdivisionBendPhaseUi(0.5f, bend) : 0.5f;
        const float markerValue = (subdivisions[static_cast<size_t>(i)] > 1)
            ? sampleModSubdivisionValueUi(startValues[static_cast<size_t>(i)],
                                          endValues[static_cast<size_t>(i)],
                                          subdivisions[static_cast<size_t>(i)],
                                          markerPhase)
            : startValues[static_cast<size_t>(i)];
        const float x = left + (stepWidth * (static_cast<float>(i) + 0.5f));
        stepMarkerPoints[static_cast<size_t>(i)] = { x, valueToY(markerValue) };
    }

    if (seq.curveMode)
    {
        juce::Path rawPath;
        std::vector<float> sampledX;
        std::vector<float> sampledValues;
        sampledX.reserve(static_cast<size_t>(totalSteps * 10));
        sampledValues.reserve(static_cast<size_t>(totalSteps * 10));
        bool started = false;
        for (int i = 0; i < totalSteps; ++i)
        {
            const int subdiv = subdivisions[static_cast<size_t>(i)];
            const bool hasLocalRamp = (subdiv > 1);
            const int segmentCount = hasLocalRamp ? juce::jlimit(2, 64, subdiv * 4) : 8;
            const float startValue = startValues[static_cast<size_t>(i)];
            const float endValue = endValues[static_cast<size_t>(i)];
            const float nextStart = startValues[static_cast<size_t>((i + 1) % totalSteps)];

            for (int s = 0; s <= segmentCount; ++s)
            {
                if (i > 0 && s == 0)
                    continue;

                const float t = static_cast<float>(s) / static_cast<float>(segmentCount);
                const float shapedT = shapeCurvePhaseUi(t, bend, stepCurveShapes[static_cast<size_t>(i)]);
                const float bendT = shapeSubdivisionBendPhaseUi(t, bend);
                const float value = hasLocalRamp
                    ? sampleModSubdivisionValueUi(startValue, endValue, subdiv, bendT)
                    : juce::jlimit(0.0f, 1.0f, startValue + ((nextStart - startValue) * shapedT));
                const float x = juce::jlimit(left, right, left + (stepWidth * (static_cast<float>(i) + t)));
                const float y = valueToY(value);

                if (!started)
                {
                    rawPath.startNewSubPath(x, y);
                    started = true;
                }
                else
                {
                    rawPath.lineTo(x, y);
                }
                sampledX.push_back(x);
                sampledValues.push_back(value);
            }
        }

        const float smoothingMs = juce::jlimit(0.0f, 250.0f, seq.smoothingMs);
        const bool showSmoothedOverlay = (smoothingMs > 0.05f && sampledValues.size() > 2);

        g.setColour(stripColour.withAlpha(showSmoothedOverlay ? 0.58f : 0.9f));
        g.strokePath(rawPath, juce::PathStrokeType(showSmoothedOverlay ? 1.6f : 2.0f));

        if (showSmoothedOverlay)
        {
            const float refStepMs = 125.0f;
            const float totalMs = refStepMs * static_cast<float>(totalSteps);
            const int sampleCount = static_cast<int>(sampledValues.size());
            const float dtMs = totalMs / static_cast<float>(juce::jmax(1, sampleCount - 1));
            const float alpha = 1.0f - std::exp(-dtMs / juce::jmax(1.0f, smoothingMs));

            float smoothed = sampledValues.front();
            juce::Path smoothPath;
            smoothPath.startNewSubPath(sampledX.front(), valueToY(smoothed));
            for (size_t idx = 1; idx < sampledValues.size(); ++idx)
            {
                smoothed += (sampledValues[idx] - smoothed) * juce::jlimit(0.0f, 1.0f, alpha);
                smoothPath.lineTo(sampledX[idx], valueToY(smoothed));
            }

            g.setColour(juce::Colour(0xff101010).withAlpha(0.68f));
            g.strokePath(smoothPath, juce::PathStrokeType(3.4f));
            g.setColour(kAccent.brighter(0.35f).withAlpha(0.92f));
            g.strokePath(smoothPath, juce::PathStrokeType(2.2f));
        }
    }
    else
    {
        for (int i = 0; i < totalSteps; ++i)
        {
            const float stepX = left + (stepWidth * static_cast<float>(i));
            const int subdiv = subdivisions[static_cast<size_t>(i)];
            const float startValue = startValues[static_cast<size_t>(i)];
            const float endValue = endValues[static_cast<size_t>(i)];
            const float slotWidth = stepWidth / static_cast<float>(juce::jmax(1, subdiv));
            const float barWidth = juce::jmax(1.0f, slotWidth * 0.72f);

            for (int s = 0; s < subdiv; ++s)
            {
                const float t = (subdiv <= 1)
                    ? 1.0f
                    : (static_cast<float>(s + 1) / static_cast<float>(subdiv));
                const float value = (subdiv <= 1)
                    ? startValue
                    : juce::jlimit(0.0f, 1.0f, startValue + ((endValue - startValue) * t));

                const float y0 = seq.bipolar ? centerY : bottom;
                const float y1 = valueToY(value);
                const float x = stepX + (slotWidth * (static_cast<float>(s) + 0.5f)) - (barWidth * 0.5f);
                const float yTop = juce::jmin(y0, y1);
                const float h = juce::jmax(1.0f, std::abs(y1 - y0));
                const float shade = (subdiv <= 1)
                    ? 0.55f
                    : juce::jmap(static_cast<float>(s) / static_cast<float>(juce::jmax(1, subdiv - 1)),
                                 0.72f,
                                 0.44f);
                g.setColour(stripColour.withAlpha(shade));
                g.fillRoundedRectangle(x, yTop, barWidth, h, 1.5f);
            }
        }
    }

    for (int i = 0; i < totalSteps; ++i)
    {
        const auto point = stepMarkerPoints[static_cast<size_t>(i)];
        g.setColour(i == activeStep ? kAccent : stripColour.withMultipliedBrightness(0.8f));
        g.fillEllipse(point.x - (dotSize * 0.5f), point.y - (dotSize * 0.5f), dotSize, dotSize);
    }
}

void ModulationControlPanel::resized()
{
    auto bounds = getLocalBounds().reduced(8);
    const bool showPitchControls = pitchScaleToggle.isVisible();
    const bool showTargetHint = targetHintLabel.isVisible();
    const bool showCurveControls = (shapeBox.getSelectedId() == 1);
    const bool showPageControls = true;
    const bool showSceneStripTools = sceneStripToolsLabel.isVisible();
    embeddedSceneSelectionToolsBounds = {};
    embeddedSceneOverlayToolsBounds = {};
    titleLabel.setBounds(bounds.removeFromTop(18));
    stripLabel.setBounds(bounds.removeFromTop(14));
    bounds.removeFromTop(2);

    if (showSceneStripTools)
    {
        auto toolRow = bounds.removeFromTop(18);
        sceneStripToolsLabel.setBounds(toolRow.removeFromLeft(44));
        sceneStripWriteButton.setBounds(toolRow.removeFromLeft(42));
        toolRow.removeFromLeft(4);
        sceneStripWriteAllButton.setBounds(toolRow.removeFromLeft(34));
        toolRow.removeFromLeft(4);
        sceneStripClearButton.setBounds(toolRow.removeFromLeft(38));
        toolRow.removeFromLeft(4);
        sceneStripDuplicateButton.setBounds(toolRow.removeFromLeft(38));
        toolRow.removeFromLeft(4);
        sceneStripCopyButton.setBounds(toolRow.removeFromLeft(42));
        bounds.removeFromTop(2);

        embeddedSceneSelectionToolsBounds = bounds.removeFromTop(18);
        bounds.removeFromTop(2);
        embeddedSceneOverlayToolsBounds = bounds.removeFromTop(18);
        bounds.removeFromTop(1);
    }
    else
    {
        sceneStripToolsLabel.setBounds({});
        sceneStripWriteButton.setBounds({});
        sceneStripWriteAllButton.setBounds({});
        sceneStripClearButton.setBounds({});
        sceneStripDuplicateButton.setBounds({});
        sceneStripCopyButton.setBounds({});
    }

    auto top = bounds.removeFromTop(20);
    targetLabel.setBounds({});
    targetBox.setBounds({});
    lengthLabel.setBounds(top.removeFromLeft(38));
    lengthBox.setBounds(top.removeFromLeft(56));
    if (showPageControls)
    {
        top.removeFromLeft(4);
        pageLabel.setBounds(top.removeFromLeft(28));
        pageBox.setBounds(top.removeFromLeft(46));
    }
    else
    {
        pageLabel.setBounds({});
        pageBox.setBounds({});
    }
    top.removeFromLeft(6);
    transportLabel.setBounds(top.removeFromLeft(38));
    transportBox.setBounds(top);

    bounds.removeFromTop(2);
    auto depthRow = bounds.removeFromTop(20);
    depthLabel.setBounds(depthRow.removeFromLeft(44));
    depthSlider.setBounds(depthRow.removeFromLeft(110));
    depthRow.removeFromLeft(4);
    rateLabel.setBounds(depthRow.removeFromLeft(34));
    rateBox.setBounds(depthRow.removeFromLeft(84));
    depthRow.removeFromLeft(4);
    bipolarToggle.setBounds(depthRow.removeFromLeft(74));

    bounds.removeFromTop(2);
    auto smoothRow = bounds.removeFromTop(20);
    smoothLabel.setBounds(smoothRow.removeFromLeft(44));
    smoothSlider.setBounds(smoothRow.removeFromLeft(110));
    smoothRow.removeFromLeft(6);
    shapeLabel.setBounds(smoothRow.removeFromLeft(38));
    shapeBox.setBounds(smoothRow.removeFromLeft(82));
    smoothRow.removeFromLeft(4);
    if (showPitchControls)
        pitchScaleToggle.setBounds(smoothRow);
    else if (showTargetHint)
        targetHintLabel.setBounds(smoothRow);
    else
    {
        pitchScaleToggle.setBounds({});
        targetHintLabel.setBounds({});
    }

    if (showCurveControls)
    {
        bounds.removeFromTop(2);
        auto curveRow = bounds.removeFromTop(20);
        curveBendLabel.setBounds(curveRow.removeFromLeft(40));
        curveBendSlider.setBounds(curveRow.removeFromLeft(110));
        curveRow.removeFromLeft(6);
        curveTypeLabel.setBounds(curveRow.removeFromLeft(38));
        curveTypeBox.setBounds(curveRow);
    }
    else
    {
        curveBendLabel.setBounds({});
        curveBendSlider.setBounds({});
        curveTypeLabel.setBounds({});
        curveTypeBox.setBounds({});
    }

    if (showPitchControls && !showTargetHint)
    {
        bounds.removeFromTop(2);
        auto scaleRow = bounds.removeFromTop(20);
        pitchScaleLabel.setBounds(scaleRow.removeFromLeft(44));
        pitchScaleBox.setBounds(scaleRow.removeFromLeft(112));
    }
    else
    {
        pitchScaleLabel.setBounds({});
        pitchScaleBox.setBounds({});
    }

    if (showTargetHint && !targetHintLabel.isVisible())
    {
        bounds.removeFromTop(2);
        targetHintLabel.setBounds(bounds.removeFromTop(16));
    }

    gestureHintLabel.setBounds(bounds.removeFromTop(12));
    bounds.removeFromTop(2);
    const int graphHeight = juce::jlimit(14, 18, bounds.getHeight() / 8);
    graphBounds = bounds.removeFromTop(graphHeight).toFloat();

    for (auto& button : stepButtons)
    {
        button.setVisible(false);
        button.setBounds({});
    }
}

void ModulationControlPanel::timerCallback()
{
    refreshFromEngine();
}

void ModulationControlPanel::mouseDown(const juce::MouseEvent& e)
{
    auto* engine = processor.getAudioEngine();
    if (!engine)
        return;

    ensurePinnedSlotSelected(*engine);
    const auto localPosition = e.getEventRelativeTo(this).position;
    int step = absoluteLegacyModStepAtPosition(localPosition);
    if (step < 0)
    {
        const int visibleStep = visibleLegacyModStepForComponent(e.eventComponent);
        if (visibleStep >= 0)
            step = absoluteLegacyModStepForVisibleIndex(visibleStep);
    }
    if (step < 0)
        return;

    const auto modifierGesture = getStepCellModifierGesture(e.mods);
    if (modifierGesture != StepCellModifierGesture::None)
    {
        gestureSourceValue = juce::jlimit(0.0f, 1.0f, engine->getModStepValueAbsolute(selectedStrip, step));
        gestureSourceSubdivision = juce::jlimit(
            1,
            ModernAudioEngine::ModMaxStepSubdivisions,
            engine->getModStepSubdivisionAbsolute(selectedStrip, step));
        gestureSourceEndValue = juce::jlimit(0.0f, 1.0f, engine->getModStepEndValueAbsolute(selectedStrip, step));
        switch (modifierGesture)
        {
            case StepCellModifierGesture::Divide:
                gestureMode = EditGestureMode::DuplicateCell;
                break;
            case StepCellModifierGesture::RampUp:
                gestureMode = EditGestureMode::ShapeUpCell;
                break;
            case StepCellModifierGesture::RampDown:
                gestureMode = EditGestureMode::ShapeDownCell;
                break;
            case StepCellModifierGesture::None:
            default:
                gestureMode = EditGestureMode::None;
                break;
        }
        gestureActive = true;
        gestureStartY = e.getScreenPosition().y;
        gestureStep = step;
        suppressNextStepClick = true;

        if (gestureMode == EditGestureMode::ShapeUpCell)
            applyShapeGesture(0, true);
        else if (gestureMode == EditGestureMode::ShapeDownCell)
            applyShapeGesture(0, false);

        refreshFromEngine();
    }
    else if (e.mods.isLeftButtonDown() && !e.mods.isRightButtonDown())
    {
        beginDrawGesture(step, localPosition);
        refreshFromEngine();
    }
}

void ModulationControlPanel::mouseDrag(const juce::MouseEvent& e)
{
    auto* engine = processor.getAudioEngine();
    if (engine == nullptr)
        return;

    ensurePinnedSlotSelected(*engine);
    const auto localPosition = e.getEventRelativeTo(this).position;

    if (!gestureActive)
        return;

    const int deltaY = e.getScreenPosition().y - gestureStartY;
    if (gestureMode == EditGestureMode::DrawCells)
        applyDrawGesture(localPosition);
    else if (gestureMode == EditGestureMode::DuplicateCell)
        applyDuplicateGesture(deltaY);
    else if (gestureMode == EditGestureMode::ShapeUpCell)
        applyShapeGesture(deltaY, true);
    else if (gestureMode == EditGestureMode::ShapeDownCell)
        applyShapeGesture(deltaY, false);

    refreshFromEngine();
}

void ModulationControlPanel::mouseUp(const juce::MouseEvent&)
{
    pendingDrawGesture = false;
    pendingDrawVisibleStep = -1;
    gestureActive = false;
    gestureMode = EditGestureMode::None;
    gestureStep = -1;
    lastDrawVisibleStep = -1;
}

void ModulationControlPanel::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (onMouseWheelPassthrough != nullptr)
        onMouseWheelPassthrough(e, wheel);
}

int ModulationControlPanel::stepIndexForComponent(juce::Component* c) const
{
    for (int i = 0; i < ModernAudioEngine::ModTotalSteps; ++i)
    {
        if (c == &stepButtons[static_cast<size_t>(i)])
            return i;
    }
    return -1;
}

int ModulationControlPanel::visibleLegacyModStepCount() const
{
    return ModernAudioEngine::ModSteps;
}

int ModulationControlPanel::totalLegacyModStepCount() const
{
    auto* engine = processor.getAudioEngine();
    if (engine == nullptr)
        return 0;

    const int maxBars = maxLengthBarsForCurrentContext();
    const int lengthBars = juce::jlimit(1,
                                        maxBars,
                                        (pinnedContextActive && selectedSlotOverride >= 0)
                                            ? engine->getModLengthBarsForSlot(selectedStrip, selectedSlotOverride)
                                            : engine->getModLengthBars(selectedStrip));
    return juce::jmax(ModernAudioEngine::ModSteps, lengthBars * ModernAudioEngine::ModSteps);
}

int ModulationControlPanel::absoluteLegacyModStepForVisibleIndex(int visibleStep) const
{
    const int safeVisibleStep = juce::jlimit(0, juce::jmax(0, visibleLegacyModStepCount() - 1), visibleStep);
    auto* engine = processor.getAudioEngine();
    const int maxBars = maxLengthBarsForCurrentContext();
    const int page = engine != nullptr
        ? juce::jlimit(0,
                       juce::jmax(0, maxBars - 1),
                       (pinnedContextActive && selectedSlotOverride >= 0)
                           ? engine->getModEditPageForSlot(selectedStrip, selectedSlotOverride)
                           : engine->getModEditPage(selectedStrip))
        : 0;
    return (page * ModernAudioEngine::ModSteps) + juce::jlimit(0, ModernAudioEngine::ModSteps - 1, safeVisibleStep);
}

int ModulationControlPanel::absoluteLegacyModStepAtPosition(juce::Point<float> position) const
{
    const int totalSteps = totalLegacyModStepCount();
    if (totalSteps <= 0 || graphBounds.isEmpty())
        return -1;

    const float dotSize = (totalSteps > 32) ? 4.0f : 6.0f;
    const float dotPad = dotSize * 0.6f;
    const auto drawLane = graphBounds.reduced(12.0f, 2.0f);
    const float left = drawLane.getX() + dotPad;
    const float right = juce::jmax(left, drawLane.getRight() - 1.0f - dotPad);
    const float top = drawLane.getY() + 2.0f;
    const float bottom = drawLane.getBottom() - 2.0f;
    const auto interactiveBounds = juce::Rectangle<float>(left,
                                                          top,
                                                          juce::jmax(1.0f, right - left),
                                                          juce::jmax(1.0f, bottom - top)).expanded(0.0f, 2.0f);
    if (!interactiveBounds.contains(position))
        return -1;

    const float clampedX = juce::jlimit(left, right, position.x);
    const float normalizedX = juce::jlimit(0.0f,
                                           1.0f,
                                           (clampedX - left) / juce::jmax(1.0f, right - left));
    return juce::jlimit(0,
                        totalSteps - 1,
                        static_cast<int>(std::round(normalizedX * static_cast<float>(juce::jmax(1, totalSteps - 1)))));
}

int ModulationControlPanel::visibleLegacyModStepForComponent(juce::Component* c) const
{
    const int index = stepIndexForComponent(c);
    if (index < 0 || index >= visibleLegacyModStepCount())
        return -1;
    return index;
}

int ModulationControlPanel::visibleLegacyModStepAtPosition(juce::Point<float> position) const
{
    const int visibleSteps = visibleLegacyModStepCount();
    for (int i = 0; i < visibleSteps; ++i)
    {
        if (stepButtons[static_cast<size_t>(i)].getBounds().expanded(1, 1).contains(position.toInt()))
            return i;
    }
    return -1;
}

float ModulationControlPanel::normalizedLegacyModDrawValueAtPosition(juce::Point<float> position) const
{
    const int totalSteps = totalLegacyModStepCount();
    if (totalSteps <= 0 || graphBounds.isEmpty())
        return 0.0f;

    const auto drawLane = graphBounds.reduced(12.0f, 2.0f);
    const float top = drawLane.getY() + 2.0f;
    const float bottom = drawLane.getBottom() - 2.0f;
    const float normalized = 1.0f - juce::jlimit(0.0f,
                                                 1.0f,
                                                 (position.y - top) / juce::jmax(1.0f, bottom - top));
    auto* engine = processor.getAudioEngine();
    if (engine != nullptr && engine->getModSequencerState(selectedStrip).target == ModernAudioEngine::ModTarget::Rearrange)
        return quantizeRearrangeStepValue(normalized);
    return juce::jlimit(0.0f, 1.0f, normalized);
}

void ModulationControlPanel::ensurePinnedSlotSelected(ModernAudioEngine& engine) const
{
    if (selectedSlotOverride >= 0)
        engine.setModSequencerSlot(selectedStrip, selectedSlotOverride);
}

void ModulationControlPanel::syncPinnedSceneMotionIfNeeded()
{
    if (pinnedContextActive && processor.isSceneModeEnabled())
    {
        processor.syncActiveSceneMotionState();
        if (onPinnedSceneMotionChange != nullptr)
            onPinnedSceneMotionChange();
    }
}

void ModulationControlPanel::beginDrawGesture(int absoluteStep, const juce::Point<float>& position)
{
    auto* engine = processor.getAudioEngine();
    if (engine == nullptr)
        return;

    ensurePinnedSlotSelected(*engine);
    pendingDrawGesture = false;
    pendingDrawVisibleStep = -1;
    gestureActive = true;
    gestureMode = EditGestureMode::DrawCells;
    gestureStep = absoluteStep;
    lastDrawVisibleStep = -1;
    suppressNextStepClick = true;
    applyDrawGesture(position);
}

void ModulationControlPanel::applyDrawGesture(juce::Point<float> position)
{
    auto* engine = processor.getAudioEngine();
    if (engine == nullptr)
        return;

    ensurePinnedSlotSelected(*engine);
    const int absoluteStep = absoluteLegacyModStepAtPosition(position);
    if (absoluteStep < 0)
        return;

    const float value = normalizedLegacyModDrawValueAtPosition(position);
    if (absoluteStep == lastDrawVisibleStep
        && std::abs(engine->getModStepValueAbsolute(selectedStrip, absoluteStep) - value) < 0.001f)
    {
        return;
    }

    engine->setModStepValueAbsolute(selectedStrip, absoluteStep, value);
    engine->setModStepShapeAbsolute(selectedStrip, absoluteStep, 1, value);
    lastDrawVisibleStep = absoluteStep;
    syncPinnedSceneMotionIfNeeded();
}

void ModulationControlPanel::applyDuplicateGesture(int deltaY)
{
    auto* engine = processor.getAudioEngine();
    const int totalSteps = totalLegacyModStepCount();
    if (!engine || gestureStep < 0 || gestureStep >= totalSteps)
        return;

    ensurePinnedSlotSelected(*engine);
    const int nextSubdivision = juce::jlimit(
        1,
        ModernAudioEngine::ModMaxStepSubdivisions,
        gestureSourceSubdivision + ((-deltaY) / 14));
    const float endValue = (nextSubdivision > 1) ? gestureSourceEndValue : gestureSourceValue;
    engine->setModStepShapeAbsolute(selectedStrip, gestureStep, nextSubdivision, endValue);
    syncPinnedSceneMotionIfNeeded();
}

void ModulationControlPanel::applyShapeGesture(int deltaY, bool rampUpMode)
{
    auto* engine = processor.getAudioEngine();
    const int totalSteps = totalLegacyModStepCount();
    if (!engine || gestureStep < 0 || gestureStep >= totalSteps)
        return;

    ensurePinnedSlotSelected(*engine);
    int subdivisions = gestureSourceSubdivision;
    if (subdivisions <= 1)
    {
        subdivisions = juce::jlimit(
            2,
            ModernAudioEngine::ModMaxStepSubdivisions,
            2 + (std::abs(deltaY) / 14));
    }

    float startValue = gestureSourceValue;
    float endValue = gestureSourceEndValue;
    computeSingleModCellRamp(gestureSourceValue, gestureSourceEndValue, deltaY, rampUpMode, startValue, endValue);
    engine->setModStepValueAbsolute(selectedStrip, gestureStep, startValue);
    engine->setModStepShapeAbsolute(selectedStrip, gestureStep, subdivisions, endValue);
    syncPinnedSceneMotionIfNeeded();
}

void ModulationControlPanel::refreshFromEngine()
{
    auto* engine = processor.getAudioEngine();
    if (!engine)
        return;

    if (!pinnedContextActive)
        selectedStrip = juce::jlimit(0, MlrVSTAudioProcessor::MaxStrips - 1, processor.getLastMonomePressedStripRow());

    ensurePinnedSlotSelected(*engine);

    titleLabel.setText(pinnedContextActive ? "Scene Motion Step Editor" : "Per-Row Modulation Sequencer",
                       juce::dontSendNotification);
    stripLabel.setText(pinnedContextActive
                           ? (pinnedContextLabel.isNotEmpty()
                                  ? pinnedContextLabel
                                  : ("Scene Motion  Strip " + juce::String(selectedStrip + 1)))
                           : ("Selected Row: " + juce::String(selectedStrip + 1) + " (last pressed)"),
                       juce::dontSendNotification);

    const bool showSceneStripTools = pinnedContextActive;
    sceneStripToolsLabel.setVisible(showSceneStripTools);
    sceneStripWriteButton.setVisible(showSceneStripTools);
    sceneStripWriteAllButton.setVisible(showSceneStripTools);
    sceneStripClearButton.setVisible(showSceneStripTools);
    sceneStripDuplicateButton.setVisible(showSceneStripTools);
    sceneStripCopyButton.setVisible(showSceneStripTools);
    sceneStripWriteButton.setEnabled(showSceneStripTools && onSceneStripWrite != nullptr);
    sceneStripWriteAllButton.setEnabled(showSceneStripTools && onSceneStripWriteAll != nullptr);
    sceneStripClearButton.setEnabled(showSceneStripTools && onSceneStripClear != nullptr);
    sceneStripDuplicateButton.setEnabled(showSceneStripTools
                                         && onSceneStripDuplicate != nullptr
                                         && selectedStrip < (MlrVSTAudioProcessor::MaxStrips - 1));
    sceneStripCopyButton.setEnabled(showSceneStripTools
                                    && onSceneStripCopyTo != nullptr
                                    && MlrVSTAudioProcessor::MaxStrips > 1);

    const int slot = selectedSlotOverride >= 0 ? selectedSlotOverride : engine->getModSequencerSlot(selectedStrip);
    const auto state = engine->getModSequencerState(selectedStrip);
    const auto target = pinnedContextActive
        ? engine->getModTargetForSlot(selectedStrip, slot)
        : state.target;
    const bool bipolar = pinnedContextActive
        ? engine->isModBipolarForSlot(selectedStrip, slot)
        : state.bipolar;
    const float depth = pinnedContextActive
        ? engine->getModDepthForSlot(selectedStrip, slot)
        : state.depth;
    const float rate = pinnedContextActive
        ? engine->getModRateForSlot(selectedStrip, slot)
        : state.rate;
    const auto transportMode = pinnedContextActive
        ? engine->getModTransportModeForSlot(selectedStrip, slot)
        : static_cast<ModernAudioEngine::ModTransportMode>(state.transportMode);
    const int lengthBars = pinnedContextActive
        ? engine->getModLengthBarsForSlot(selectedStrip, slot)
        : state.lengthBars;
    const int editPage = pinnedContextActive
        ? engine->getModEditPageForSlot(selectedStrip, slot)
        : state.editPage;
    const float smoothingMs = pinnedContextActive
        ? engine->getModSmoothingMsForSlot(selectedStrip, slot)
        : state.smoothingMs;
    const bool curveMode = pinnedContextActive
        ? engine->isModCurveModeForSlot(selectedStrip, slot)
        : state.curveMode;
    const float curveBend = pinnedContextActive
        ? engine->getModCurveBendForSlot(selectedStrip, slot)
        : state.curveBend;
    const auto curveShape = pinnedContextActive
        ? engine->getModCurveShapeForSlot(selectedStrip, slot)
        : static_cast<ModernAudioEngine::ModCurveShape>(state.curveShape);
    const bool pitchQuantize = pinnedContextActive
        ? engine->isModPitchScaleQuantizeForSlot(selectedStrip, slot)
        : state.pitchScaleQuantize;
    const auto pitchScale = pinnedContextActive
        ? engine->getModPitchScaleForSlot(selectedStrip, slot)
        : static_cast<ModernAudioEngine::PitchScale>(state.pitchScale);
    const int maxBars = maxLengthBarsForCurrentContext();
    const int effectiveLengthBars = juce::jlimit(1, maxBars, lengthBars);
    const int effectiveEditPage = juce::jlimit(0, juce::jmax(0, effectiveLengthBars - 1), editPage);

    rebuildLengthAndPageBoxes(maxBars);
    if (pinnedContextActive && selectedSlotOverride >= 0
        && (lengthBars != effectiveLengthBars || editPage != effectiveEditPage))
    {
        engine->setModLengthBarsForSlot(selectedStrip, slot, effectiveLengthBars);
        engine->setModEditPage(selectedStrip, effectiveEditPage);
        syncPinnedSceneMotionIfNeeded();
    }

    targetBox.setSelectedId(modTargetToComboId(target), juce::dontSendNotification);
    bipolarToggle.setToggleState(bipolar, juce::dontSendNotification);
    bipolarToggle.setEnabled(modTargetAllowsBipolar(target));
    depthSlider.setValue(depth, juce::dontSendNotification);
    rateBox.setSelectedId(modRateToComboId(rate), juce::dontSendNotification);
    transportBox.setSelectedId(static_cast<int>(transportMode) + 1, juce::dontSendNotification);
    lengthBox.setSelectedId(effectiveLengthBars, juce::dontSendNotification);
    pageBox.setSelectedId(effectiveEditPage + 1, juce::dontSendNotification);
    pageBox.setEnabled(effectiveLengthBars > 1);
    lengthBox.setTooltip(pinnedContextActive
                             ? ("Scene motion length is capped to the current scene span (" + juce::String(maxBars)
                                + (maxBars == 1 ? " bar)." : " bars)."))
                             : "Motion sequence length in bars.");
    smoothSlider.setValue(smoothingMs, juce::dontSendNotification);
    shapeBox.setSelectedId(curveMode ? 1 : 2, juce::dontSendNotification);
    curveBendSlider.setValue(curveBend, juce::dontSendNotification);
    curveTypeBox.setSelectedId(curveShapeToComboId(curveShape), juce::dontSendNotification);
    curveBendSlider.setEnabled(curveMode);
    curveTypeBox.setEnabled(curveMode);
    pitchScaleToggle.setToggleState(pitchQuantize, juce::dontSendNotification);
    pitchScaleBox.setSelectedId(pitchScaleToComboId(pitchScale), juce::dontSendNotification);
    const bool showPitchControls = (target == ModernAudioEngine::ModTarget::Pitch);
    const bool showRetriggerHint = (target == ModernAudioEngine::ModTarget::Retrigger);
    const bool showRearrangeHint = (target == ModernAudioEngine::ModTarget::Rearrange);
    const bool showCurveControls = curveMode;
    pageLabel.setVisible(true);
    pageBox.setVisible(true);
    gestureHintLabel.setText("Graph edits: drag draws. Cmd=Divide  Ctrl=Ramp+  Opt=Ramp-",
                             juce::dontSendNotification);
    const bool targetUiChanged = (pitchScaleToggle.isVisible() != showPitchControls)
        || (pitchScaleLabel.isVisible() != showPitchControls)
        || (pitchScaleBox.isVisible() != showPitchControls)
        || (targetHintLabel.isVisible() != (showRetriggerHint || showRearrangeHint))
        || (curveBendLabel.isVisible() != showCurveControls)
        || (curveBendSlider.isVisible() != showCurveControls)
        || (curveTypeLabel.isVisible() != showCurveControls)
        || (curveTypeBox.isVisible() != showCurveControls);
    pitchScaleToggle.setVisible(showPitchControls);
    pitchScaleLabel.setVisible(showPitchControls);
    pitchScaleBox.setVisible(showPitchControls);
    targetHintLabel.setVisible(showRetriggerHint || showRearrangeHint);
    curveBendLabel.setVisible(showCurveControls);
    curveBendSlider.setVisible(showCurveControls);
    curveTypeLabel.setVisible(showCurveControls);
    curveTypeBox.setVisible(showCurveControls);
    pitchScaleLabel.setEnabled(showPitchControls && pitchQuantize);
    pitchScaleBox.setEnabled(showPitchControls && pitchQuantize);

    const int activeGlobalStep = engine->getModCurrentGlobalStep(selectedStrip);
    const float activeRaw = juce::jlimit(0.0f, 1.0f, engine->getModStepValueAbsolute(selectedStrip, activeGlobalStep));
    if (showRetriggerHint)
        targetHintLabel.setText(makeRetriggerHintText(activeRaw, depth), juce::dontSendNotification);
    else if (showRearrangeHint)
        targetHintLabel.setText(makeRearrangeHintText(activeRaw), juce::dontSendNotification);
    const int playbackPage = juce::jlimit(
        0,
        ModernAudioEngine::MaxModBars - 1,
        activeGlobalStep / ModernAudioEngine::ModSteps);
    const int visibleStepCount = visibleLegacyModStepCount();
    const int activeStep = (playbackPage == editPage)
        ? (activeGlobalStep % ModernAudioEngine::ModSteps)
        : -1;
    for (int i = 0; i < ModernAudioEngine::ModTotalSteps; ++i)
    {
        auto& b = stepButtons[static_cast<size_t>(i)];
        if (i >= visibleStepCount)
        {
            b.setVisible(false);
            continue;
        }

        b.setVisible(false);
        const int absoluteStep = absoluteLegacyModStepForVisibleIndex(i);
        const float value = juce::jlimit(
            0.0f,
            1.0f,
            pinnedContextActive
                ? engine->getModStepValueAbsoluteForSlot(selectedStrip, slot, absoluteStep)
                : state.steps[static_cast<size_t>(i)]);
        const int subdivisions = juce::jlimit(
            1,
            ModernAudioEngine::ModMaxStepSubdivisions,
            pinnedContextActive
                ? engine->getModStepSubdivisionAbsoluteForSlot(selectedStrip, slot, absoluteStep)
                : state.stepSubdivisions[static_cast<size_t>(i)]);
        const float endValue = juce::jlimit(
            0.0f,
            1.0f,
            pinnedContextActive
                ? engine->getModStepEndValueAbsoluteForSlot(selectedStrip, slot, absoluteStep)
                : state.stepEndValues[static_cast<size_t>(i)]);
        const auto offColour = juce::Colour(0xff2f2f2f);
        const auto onColour = kAccent.withMultipliedBrightness(0.9f);
        juce::Colour c = offColour.interpolatedWith(onColour, value);
        if (subdivisions > 1)
            c = c.interpolatedWith(juce::Colour(0xfff0f6ff), 0.16f);
        if (i == activeStep)
            c = c.interpolatedWith(juce::Colour(0xffffcf75), 0.55f);
        b.setColour(juce::TextButton::buttonColourId, c);
        b.setButtonText(juce::String(absoluteStep + 1));
        if (showRearrangeHint)
        {
            b.setTooltip("Step " + juce::String(absoluteStep + 1)
                         + ": Slice " + juce::String(rearrangeSliceDisplayIndex(value))
                         + "\nClick: advance slice. Drag in strip mod page for exact 1..16 mapping.");
        }
        else
        {
            b.setTooltip("Step " + juce::String(absoluteStep + 1)
                         + ": " + juce::String(static_cast<int>(std::round(value * 100.0f))) + "%\n"
                         + "Shape: x" + juce::String(subdivisions)
                         + "  end " + juce::String(static_cast<int>(std::round(endValue * 100.0f))) + "%\n"
                         + "Click: toggle step. Drag: draw. Cmd+drag: divide. Ctrl+drag: ramp up. Opt+drag: ramp down.");
        }
    }
    if (targetUiChanged)
        resized();
}

void ModulationControlPanel::showSceneStripCopyMenu()
{
    if (!pinnedContextActive || onSceneStripCopyTo == nullptr)
        return;

    juce::PopupMenu menu;
    for (int targetStrip = 0; targetStrip < MlrVSTAudioProcessor::MaxStrips; ++targetStrip)
    {
        if (targetStrip == selectedStrip)
            continue;
        menu.addItem(targetStrip + 1, "Copy to Strip " + juce::String(targetStrip + 1));
    }

    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&sceneStripCopyButton),
                       [this](int result)
                       {
                           if (result > 0 && onSceneStripCopyTo != nullptr)
                               onSceneStripCopyTo(selectedStrip, result - 1);
                       });
}


//==============================================================================
// MlrVSTAudioProcessorEditor Implementation
//==============================================================================

MlrVSTAudioProcessorEditor::MlrVSTAudioProcessorEditor(MlrVSTAudioProcessor& p)
    : AudioProcessorEditor(&p), audioProcessor(p)
{
    setupLookAndFeel();
    setTooltipsEnabled(false);
    activeGuiStripCount = getDetectedGuiStripCount();
    
    // Enable keyboard input for spacebar transport control
    setWantsKeyboardFocus(true);
    
    // Set window size FIRST. Clamp initial height to the current display so hosts
    // that do not expose plugin resizing (e.g. Logic for some formats) still show
    // the full UI without clipping the bottom.
    int initialHeight = windowHeight;
    if (const auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
    {
        const int safeVisibleHeight = juce::jmax(620, display->userArea.getHeight() - 80);
        initialHeight = juce::jmin(windowHeight, safeVisibleHeight);
    }
    setSize(windowWidth, initialHeight);
    setResizable(true, true);
    setResizeLimits(1000, 620, 1920, 1400);
    
    // Create all UI components
    createUIComponents();
    setActiveGuiStripCount(activeGuiStripCount, true);
    
    // Force initial layout
    resized();
    
    // Start UI update timer
    startTimer(50);
    lastPresetRefreshToken = audioProcessor.getPresetRefreshToken();
    lastTopTabIndex = topTabs ? topTabs->getCurrentTabIndex() : -1;
}

int MlrVSTAudioProcessorEditor::getDetectedGuiStripCount() const
{
    const int reportedStripCount = audioProcessor.getMonomeActiveStripCount();
    if (reportedStripCount <= 0)
        return 6;

    return juce::jlimit(1, MlrVSTAudioProcessor::MaxStrips, reportedStripCount);
}

void MlrVSTAudioProcessorEditor::setActiveGuiStripCount(int stripCount, bool forceRelayout)
{
    const int clampedStripCount = juce::jlimit(1, MlrVSTAudioProcessor::MaxStrips, stripCount);
    if (!forceRelayout && clampedStripCount == activeGuiStripCount)
        return;

    activeGuiStripCount = clampedStripCount;

    for (int i = 0; i < stripControls.size(); ++i)
        if (auto* strip = stripControls[i])
            strip->setVisible(i < activeGuiStripCount);

    for (int i = 0; i < fxStripControls.size(); ++i)
        if (auto* fxStrip = fxStripControls[i])
            fxStrip->setVisible(i < activeGuiStripCount);

    if (mainTabs)
    {
        if (auto* playPanel = mainTabs->getTabContentComponent(0))
            playPanel->resized();
        if (auto* fxPanel = mainTabs->getTabContentComponent(1))
            fxPanel->resized();
    }

    repaint();
}

void MlrVSTAudioProcessorEditor::createUIComponents()
{
    constexpr int kTotalSampleStrips = MlrVSTAudioProcessor::MaxStrips;
    // Monome grid hidden to save space - use physical monome instead
    monomeGrid = std::make_unique<MonomeGridDisplay>(audioProcessor);
    // Don't add to view - saves space
    
    // Create control panels
    monomeControl = std::make_unique<MonomeControlPanel>(audioProcessor);
    globalControl = std::make_unique<GlobalControlPanel>(audioProcessor);
    globalControl->onTooltipsToggled = [this](bool enabled)
    {
        setTooltipsEnabled(enabled);
    };
    macroControl = std::make_unique<MacroControlPanel>(audioProcessor);
    monomePagesControl = std::make_unique<MonomePagesPanel>(audioProcessor);
    presetControl = std::make_unique<PresetControlPanel>(audioProcessor);
    pathsControl = std::make_unique<PathsControlPanel>(audioProcessor);
    sceneControl = std::make_unique<SceneControlPanel>(audioProcessor);
    
    // Create TABBED top controls to save space
    topTabs = std::make_unique<juce::TabbedComponent>(juce::TabbedButtonBar::TabsAtTop);
    topTabs->addTab("Global Controls", juce::Colour(0xff2c2c2c), globalControl.get(), false);
    topTabs->addTab("Macros", juce::Colour(0xff2c2c2c), macroControl.get(), false);
    topTabs->addTab("Presets", juce::Colour(0xff2c2c2c), presetControl.get(), false);
    topTabs->addTab("Monome Device", juce::Colour(0xff2c2c2c), monomeControl.get(), false);
    topTabs->addTab("Paths", juce::Colour(0xff2c2c2c), pathsControl.get(), false);
    topTabs->addTab("Scene", juce::Colour(0xff2c2c2c), sceneControl.get(), false);
    topTabs->setTabBarDepth(28);
    topTabs->setCurrentTabIndex(0);  // Global Controls visible by default
    addAndMakeVisible(*topTabs);
    addAndMakeVisible(*monomePagesControl);
    
    // Helper panel classes for main tabs
    struct PlayPanel : public juce::Component
    {
        juce::OwnedArray<StripControl>& strips;
        int& visibleStripCount;
        
        PlayPanel(juce::OwnedArray<StripControl>& s, int& visibleCount)
            : strips(s), visibleStripCount(visibleCount) {}

        void addStrip(StripControl* strip)
        {
            if (strip != nullptr)
                addAndMakeVisible(strip);
        }
        
        void resized() override
        {
            auto bounds = getLocalBounds();
            const int gap = 1;
            const int totalStrips = strips.size();
            if (totalStrips <= 0 || bounds.isEmpty())
                return;

            const int stripCount = juce::jlimit(1, totalStrips, visibleStripCount);
            const int totalGap = gap * juce::jmax(0, stripCount - 1);
            const int stripHeight = juce::jmax(1, (bounds.getHeight() - totalGap) / stripCount);
            
            for (int i = 0; i < stripCount; ++i)
            {
                if (auto* strip = strips[i])
                {
                    const int y = i * (stripHeight + gap);
                    strip->setBounds(0, y, bounds.getWidth(), stripHeight);
                }
            }

            for (int i = stripCount; i < totalStrips; ++i)
                if (auto* strip = strips[i])
                    strip->setBounds({});
        }
    };
    
    struct FXPanel : public juce::Component
    {
        juce::OwnedArray<FXStripControl>& strips;
        int& visibleStripCount;
        juce::Label& masterDuckLabel;
        juce::ComboBox& masterDuckTriggerBox;
        
        FXPanel(juce::OwnedArray<FXStripControl>& s,
                int& visibleCount,
                juce::Label& headerLabel,
                juce::ComboBox& headerBox)
            : strips(s),
              visibleStripCount(visibleCount),
              masterDuckLabel(headerLabel),
              masterDuckTriggerBox(headerBox)
        {
            addAndMakeVisible(masterDuckLabel);
            addAndMakeVisible(masterDuckTriggerBox);
        }

        void addStrip(FXStripControl* strip)
        {
            if (strip != nullptr)
                addAndMakeVisible(strip);
        }
        
        void resized() override
        {
            auto bounds = getLocalBounds();
            masterDuckLabel.setBounds({});
            masterDuckTriggerBox.setBounds({});
            const int gap = 1;
            const int totalStrips = strips.size();
            if (totalStrips <= 0 || bounds.isEmpty())
                return;

            const int stripCount = juce::jlimit(1, totalStrips, visibleStripCount);
            const int totalGap = gap * juce::jmax(0, stripCount - 1);
            const int stripHeight = juce::jmax(1, (bounds.getHeight() - totalGap) / stripCount);
            
            for (int i = 0; i < stripCount; ++i)
            {
                if (auto* strip = strips[i])
                {
                    const int y = i * (stripHeight + gap);
                    strip->setBounds(0, y, bounds.getWidth(), stripHeight);
                }
            }

            for (int i = stripCount; i < totalStrips; ++i)
                if (auto* strip = strips[i])
                    strip->setBounds({});
        }
    };
    
    // Create MAIN UNIFIED TABS: Play / FX / Patterns / Groups
    mainTabs = std::make_unique<juce::TabbedComponent>(juce::TabbedButtonBar::TabsAtTop);
    
    // PLAY TAB - regular strip controls
    auto* playPanel = new PlayPanel(stripControls, activeGuiStripCount);
    for (int i = 0; i < kTotalSampleStrips; ++i)
    {
        auto* strip = new StripControl(i, audioProcessor);
        stripControls.add(strip);
        playPanel->addStrip(strip);
    }

    fxMasterDuckLabel.setText("Master Duck", juce::dontSendNotification);
    fxMasterDuckLabel.setFont(juce::Font(juce::FontOptions(11.0f, juce::Font::bold)));
    fxMasterDuckLabel.setJustificationType(juce::Justification::centredLeft);
    fxMasterDuckLabel.setColour(juce::Label::textColourId, kTextPrimary);
    fxMasterDuckTriggerBox.addItem("None", 1);
    for (int i = 0; i < kTotalSampleStrips; ++i)
        fxMasterDuckTriggerBox.addItem("S" + juce::String(i + 1), i + 2);
    styleUiCombo(fxMasterDuckTriggerBox);
    fxMasterDuckTriggerBox.setJustificationType(juce::Justification::centred);
    fxMasterDuckTriggerAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        audioProcessor.parameters, "masterDuckTriggerStrip", fxMasterDuckTriggerBox);
    
    // FX TAB - filter controls for each strip
    auto* fxPanel = new FXPanel(fxStripControls, activeGuiStripCount, fxMasterDuckLabel, fxMasterDuckTriggerBox);
    for (int i = 0; i < kTotalSampleStrips; ++i)
    {
        auto* fxStrip = new FXStripControl(i, audioProcessor);
        fxStripControls.add(fxStrip);
        fxPanel->addStrip(fxStrip);
    }
    
    // PATTERNS TAB
    patternControl = std::make_unique<PatternControlPanel>(audioProcessor);
    
    // GROUPS TAB
    groupControl = std::make_unique<GroupControlPanel>(audioProcessor);

    // Add main tabs to container
    mainTabs->addTab("Play", juce::Colour(0xff282828), playPanel, true);
    mainTabs->addTab("FX", juce::Colour(0xff282828), fxPanel, true);
    mainTabs->addTab("Patterns", juce::Colour(0xff282828), patternControl.get(), false);
    mainTabs->addTab("Groups", juce::Colour(0xff282828), groupControl.get(), false);
    mainTabs->setTabBarDepth(28);
    mainTabs->setCurrentTabIndex(0);  // Start on Play tab
    addAndMakeVisible(*mainTabs);

    lastSceneModeEnabled = audioProcessor.isSceneModeEnabled();
    refreshSceneModeLayout();
}

MlrVSTAudioProcessorEditor::~MlrVSTAudioProcessorEditor()
{
    stopTimer();
}

void MlrVSTAudioProcessorEditor::refreshSceneModeLayout()
{
    if (!mainTabs || !topTabs)
        return;

    const bool sceneModeEnabled = audioProcessor.isSceneModeEnabled();
    auto findTabIndex = [this](const juce::String& name) -> int
    {
        if (!mainTabs)
            return -1;
        for (int i = 0; i < mainTabs->getNumTabs(); ++i)
        {
            if (mainTabs->getTabNames()[i] == name)
                return i;
        }
        return -1;
    };

    const int currentTabIndex = mainTabs->getCurrentTabIndex();
    const auto currentTabName = (currentTabIndex >= 0 && currentTabIndex < mainTabs->getNumTabs())
        ? mainTabs->getTabNames()[currentTabIndex]
        : juce::String();

    auto ensureTab = [&](const juce::String& name, juce::Component* content)
    {
        if (content == nullptr || findTabIndex(name) >= 0)
            return;
        mainTabs->addTab(name, juce::Colour(0xff282828), content, false);
    };

    if (sceneModeEnabled)
    {
        const int patternsTabIndex = findTabIndex("Patterns");
        if (patternsTabIndex >= 0)
            mainTabs->removeTab(patternsTabIndex);

        const int groupsTabIndex = findTabIndex("Groups");
        if (groupsTabIndex >= 0)
            mainTabs->removeTab(groupsTabIndex);
    }
    else
    {
        ensureTab("Patterns", patternControl.get());
        ensureTab("Groups", groupControl.get());
    }

    if (currentTabName.isNotEmpty())
    {
        const int restoredIndex = findTabIndex(currentTabName);
        if (restoredIndex >= 0)
            mainTabs->setCurrentTabIndex(restoredIndex);
        else
            mainTabs->setCurrentTabIndex(0);
    }
    else
    {
        mainTabs->setCurrentTabIndex(0);
    }

    for (auto* strip : stripControls)
        if (strip) strip->resized();
    for (auto* fxStrip : fxStripControls)
        if (fxStrip) fxStrip->resized();

    auto findTopTabIndex = [this](const juce::String& name) -> int
    {
        if (!topTabs)
            return -1;
        for (int i = 0; i < topTabs->getNumTabs(); ++i)
        {
            if (topTabs->getTabNames()[i] == name)
                return i;
        }
        return -1;
    };

    const int sceneTabIndex = findTopTabIndex("Scene");
    if (sceneModeEnabled)
    {
        if (!sceneModeForcedSceneTab)
        {
            sceneModePreviousTopTabIndex = topTabs->getCurrentTabIndex();
            sceneModeForcedSceneTab = true;
        }

        if (sceneTabIndex >= 0)
            topTabs->setCurrentTabIndex(sceneTabIndex);
    }
    else if (sceneModeForcedSceneTab)
    {
        sceneModeForcedSceneTab = false;
        if (sceneModePreviousTopTabIndex >= 0 && sceneModePreviousTopTabIndex < topTabs->getNumTabs())
            topTabs->setCurrentTabIndex(sceneModePreviousTopTabIndex);
        sceneModePreviousTopTabIndex = -1;
    }
}

void MlrVSTAudioProcessorEditor::setupLookAndFeel()
{
    darkLookAndFeel.setDefaultSansSerifTypefaceName("Helvetica Neue");

    darkLookAndFeel.setColour(juce::ResizableWindow::backgroundColourId, kBgBottom);

    darkLookAndFeel.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff404448));
    darkLookAndFeel.setColour(juce::TextButton::buttonOnColourId, kAccent);
    darkLookAndFeel.setColour(juce::TextButton::textColourOffId, kTextPrimary);
    darkLookAndFeel.setColour(juce::TextButton::textColourOnId, juce::Colour(0xff151515));

    darkLookAndFeel.setColour(juce::Slider::thumbColourId, kAccent);
    darkLookAndFeel.setColour(juce::Slider::trackColourId, juce::Colour(0xff4c4c4c));
    darkLookAndFeel.setColour(juce::Slider::backgroundColourId, juce::Colour(0xff262626));
    darkLookAndFeel.setColour(juce::Slider::rotarySliderFillColourId, kAccent.withAlpha(0.9f));
    darkLookAndFeel.setColour(juce::Slider::rotarySliderOutlineColourId, juce::Colour(0xff525252));

    darkLookAndFeel.setColour(juce::ComboBox::backgroundColourId, juce::Colour(0xff353b42));
    darkLookAndFeel.setColour(juce::ComboBox::textColourId, kTextPrimary);
    darkLookAndFeel.setColour(juce::ComboBox::outlineColourId, juce::Colour(0xff6a7076));
    darkLookAndFeel.setColour(juce::ComboBox::arrowColourId, kAccent.brighter(0.08f));
    darkLookAndFeel.setColour(juce::PopupMenu::backgroundColourId, juce::Colour(0xff202428));
    darkLookAndFeel.setColour(juce::PopupMenu::textColourId, kTextPrimary);
    darkLookAndFeel.setColour(juce::PopupMenu::highlightedBackgroundColourId, kAccent.withAlpha(0.72f));
    darkLookAndFeel.setColour(juce::PopupMenu::highlightedTextColourId, juce::Colour(0xff101010));

    darkLookAndFeel.setColour(juce::Label::textColourId, kTextPrimary);

    darkLookAndFeel.setColour(juce::TabbedComponent::backgroundColourId, juce::Colour(0xff23262a));
    darkLookAndFeel.setColour(juce::TabbedComponent::outlineColourId, juce::Colour(0xff575c61));
    darkLookAndFeel.setColour(juce::TabbedButtonBar::tabOutlineColourId, juce::Colour(0xff575c61));
    darkLookAndFeel.setColour(juce::TabbedButtonBar::tabTextColourId, kTextSecondary);
    darkLookAndFeel.setColour(juce::TabbedButtonBar::frontTextColourId, juce::Colour(0xfff7f7f7));
    
    setLookAndFeel(&darkLookAndFeel);
}

void MlrVSTAudioProcessorEditor::setTooltipsEnabled(bool enabled)
{
    tooltipsEnabled = enabled;
    if (tooltipsEnabled)
    {
        if (!tooltipWindow)
            tooltipWindow = std::make_unique<juce::TooltipWindow>(this, 350);
    }
    else
    {
        tooltipWindow.reset();
    }
}

void MlrVSTAudioProcessorEditor::paint(juce::Graphics& g)
{
    auto area = getLocalBounds().toFloat();

    juce::ColourGradient bg(kBgTop, 0.0f, 0.0f, kBgBottom, 0.0f, area.getBottom(), false);
    g.setGradientFill(bg);
    g.fillAll();

    auto titleBar = getLocalBounds().removeFromTop(40).toFloat();
    juce::ColourGradient titleFill(juce::Colour(0xff3a3d41), 0.0f, titleBar.getY(),
                                   juce::Colour(0xff2e3135), 0.0f, titleBar.getBottom(), false);
    g.setGradientFill(titleFill);
    g.fillRect(titleBar);
    g.setColour(juce::Colour(0xff565656));
    g.drawLine(titleBar.getX(), titleBar.getBottom(), titleBar.getRight(), titleBar.getBottom(), 1.0f);

    g.setColour(kTextPrimary);
    g.setFont(juce::Font(juce::FontOptions(23.0f, juce::Font::bold)));
    g.drawText("mlrVST", 16, 7, 220, 30, juce::Justification::centredLeft);

    g.setColour(kTextSecondary.brighter(0.1f));
    g.setFont(juce::Font(juce::FontOptions(11.0f)));
    g.drawText("Performance Slicer", 152, 10, 170, 20, juce::Justification::centredLeft);

    g.setColour(kTextMuted);
    g.setFont(juce::Font(juce::FontOptions(10.0f)));
    const juce::String buildInfo = "v" + juce::String(MLRVST_BUILD_VERSION)
        + " | build " + juce::String(MLRVST_BUILD_STAMP);
    g.drawText(buildInfo, getWidth() - 440, 11, 424, 18, juce::Justification::centredRight);
}

bool MlrVSTAudioProcessorEditor::keyPressed(const juce::KeyPress& key)
{
    if (topTabs != nullptr
        && sceneControl != nullptr
        && topTabs->getCurrentContentComponent() == sceneControl.get()
        && sceneControl->handleEditorKeyPress(key))
    {
        return true;
    }

    if (mainTabs != nullptr
        && patternControl != nullptr
        && mainTabs->getCurrentContentComponent() == patternControl.get()
        && patternControl->handleEditorKeyPress(key))
    {
        return true;
    }

    // Spacebar does nothing in plugin mode - DAW controls transport
    return false;  // Let other keys pass through
}

void MlrVSTAudioProcessorEditor::resized()
{
    // Safety check
    if (!topTabs || !mainTabs)
        return;
    
    auto bounds = getLocalBounds();
    
    // Title area
    bounds.removeFromTop(40);
    
    auto margin = 6;
    bounds.reduce(margin, margin);

    const bool sceneWorkspaceFullscreen = audioProcessor.isSceneModeEnabled()
        && sceneControl != nullptr
        && topTabs->getCurrentContentComponent() == sceneControl.get();

    if (sceneWorkspaceFullscreen)
    {
        monomePagesControl->setVisible(true);
        auto monomePagesArea = bounds.removeFromBottom(50);
        monomePagesControl->setBounds(monomePagesArea);
        bounds.removeFromBottom(margin);
        topTabs->setBounds(bounds);
        mainTabs->setVisible(false);
        mainTabs->setBounds({});
        return;
    }

    monomePagesControl->setVisible(true);
    auto monomePagesArea = bounds.removeFromBottom(50);
    monomePagesControl->setBounds(monomePagesArea);
    bounds.removeFromBottom(margin);

    constexpr int kCompactTopTabHeight = 184;
    const int requestedTopBarHeight = kCompactTopTabHeight;
    const int maxTopBarHeight = juce::jmax(124, bounds.getHeight() - 180);
    auto topBar = bounds.removeFromTop(juce::jmin(requestedTopBarHeight, maxTopBarHeight));
    topTabs->setBounds(topBar);

    bounds.removeFromTop(margin);
    mainTabs->setVisible(true);
    mainTabs->setBounds(bounds);
}

//==============================================================================

void MlrVSTAudioProcessorEditor::timerCallback()
{
    if (!audioProcessor.getAudioEngine())
        return;

    if (topTabs)
    {
        const int currentTopTabIndex = topTabs->getCurrentTabIndex();
        if (currentTopTabIndex != lastTopTabIndex)
        {
            lastTopTabIndex = currentTopTabIndex;
            resized();
            if (sceneControl)
            {
                sceneControl->refreshFromProcessor();
                sceneControl->repaint();
            }
        }
    }

    const bool sceneModeEnabled = audioProcessor.isSceneModeEnabled();
    if (sceneModeEnabled != lastSceneModeEnabled)
    {
        lastSceneModeEnabled = sceneModeEnabled;
        refreshSceneModeLayout();
        resized();
        if (sceneControl)
        {
            sceneControl->refreshFromProcessor();
            sceneControl->repaint();
        }
    }
    
    // Update input meters
    if (globalControl)
    {
        float levelL = audioProcessor.getAudioEngine()->getInputLevelL();
        float levelR = audioProcessor.getAudioEngine()->getInputLevelR();
        globalControl->updateMeters(levelL, levelR);
        globalControl->refreshFromProcessor();
    }

    if (sceneControl)
    {
        sceneControl->refreshFromProcessor();
        if (topTabs != nullptr && topTabs->getCurrentContentComponent() == sceneControl.get())
            sceneControl->repaint();
    }

    if (macroControl)
        macroControl->refreshFromProcessor();

    if (presetControl)
        presetControl->refreshVisualState();

    const bool modulationActive = !audioProcessor.isSceneModeEnabled()
        && audioProcessor.isControlModeActive()
        && audioProcessor.getCurrentControlMode() == MlrVSTAudioProcessor::ControlMode::Modulation;
    setActiveGuiStripCount(getDetectedGuiStripCount(), false);
    for (int i = 0; i < stripControls.size(); ++i)
    {
        if (auto* strip = stripControls[i])
        {
            const bool isVisibleStrip = i < activeGuiStripCount;
            const bool showLane = modulationActive && isVisibleStrip;
            strip->setModulationLaneView(showLane);
            strip->setVisible(isVisibleStrip);
        }
    }

    const uint32_t refreshToken = audioProcessor.getPresetRefreshToken();
    if (refreshToken != lastPresetRefreshToken)
    {
        lastPresetRefreshToken = refreshToken;
        if (patternControl)
            patternControl->timerCallback();
        if (groupControl)
            groupControl->timerCallback();
        for (auto* strip : stripControls)
            if (strip) strip->repaint();
        for (auto* fxStrip : fxStripControls)
            if (fxStrip) fxStrip->repaint();
        repaint();
    }
    
    // Update grid from monome connection
    if (auto& monome = audioProcessor.getMonomeConnection(); monome.isConnected())
    {
        if (monomeGrid)
            monomeGrid->updateFromEngine();
    }
}
