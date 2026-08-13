/*
  ==============================================================================

    PluginProcessorSceneBoundaryTransition.cpp
    Extracted scene-boundary transition runtime for mlrVST.

  ==============================================================================
*/

#include "PluginProcessor.h"
#include <array>
#include <cmath>
#include <limits>
#include <utility>

namespace
{
float smoothSceneTransitionProgress(float progress)
{
    const float clamped = juce::jlimit(0.0f, 1.0f, progress);
    return clamped * clamped * (3.0f - (2.0f * clamped));
}

double sceneChainTransitionLeadBeatsRuntime(MlrVSTAudioProcessor::SceneChainTransitionType type,
                                            float lengthBeats)
{
    float safeLengthBeats = lengthBeats;
    if (!std::isfinite(safeLengthBeats))
        safeLengthBeats = MlrVSTAudioProcessor::DefaultSceneTransitionLengthBeats;
    safeLengthBeats = juce::jlimit(MlrVSTAudioProcessor::MinSceneTransitionLengthBeats,
                                   MlrVSTAudioProcessor::MaxSceneTransitionLengthBeats,
                                   safeLengthBeats);
    const double requestedLength = static_cast<double>(safeLengthBeats);
    const auto safeType = static_cast<MlrVSTAudioProcessor::SceneChainTransitionType>(juce::jlimit(
        0,
        static_cast<int>(MlrVSTAudioProcessor::SceneChainTransitionType::Return),
        static_cast<int>(type)));

    switch (safeType)
    {
        case MlrVSTAudioProcessor::SceneChainTransitionType::Fill:       return juce::jlimit(0.25, 8.0, requestedLength);
        case MlrVSTAudioProcessor::SceneChainTransitionType::Stutter:    return juce::jlimit(0.25, 6.0, requestedLength);
        case MlrVSTAudioProcessor::SceneChainTransitionType::FilterRise: return juce::jlimit(0.25, 8.0, requestedLength);
        case MlrVSTAudioProcessor::SceneChainTransitionType::Drop:       return juce::jlimit(0.25, 6.0, requestedLength);
        case MlrVSTAudioProcessor::SceneChainTransitionType::MuteTail:   return juce::jlimit(0.25, 4.0, requestedLength);
        case MlrVSTAudioProcessor::SceneChainTransitionType::Break:      return juce::jlimit(0.25, 8.0, requestedLength);
        case MlrVSTAudioProcessor::SceneChainTransitionType::None:
        case MlrVSTAudioProcessor::SceneChainTransitionType::Return:
        default:
            return 0.0;
    }
}

float sceneTransitionRhythmicPulse(double beatsSinceStart,
                                   double subdivisionBeats,
                                   float sharpness)
{
    if (!std::isfinite(beatsSinceStart) || !std::isfinite(subdivisionBeats) || subdivisionBeats <= 1.0e-6)
        return 0.0f;

    double cycle = std::fmod(beatsSinceStart / subdivisionBeats, 1.0);
    if (cycle < 0.0)
        cycle += 1.0;

    const float triangle = 1.0f - std::abs(static_cast<float>((cycle * 2.0) - 1.0));
    return std::pow(juce::jlimit(0.0f, 1.0f, triangle),
                    juce::jlimit(0.4f, 6.0f, sharpness));
}

MlrVSTAudioProcessor::SceneChainTransitionType sanitizeRuntimeSceneChainTransitionType(int rawType)
{
    return static_cast<MlrVSTAudioProcessor::SceneChainTransitionType>(juce::jlimit(
        0,
        static_cast<int>(MlrVSTAudioProcessor::SceneChainTransitionType::Return),
        rawType));
}

MlrVSTAudioProcessor::SceneChainTransitionOption sanitizeRuntimeSceneChainTransitionOption(int rawOption)
{
    return static_cast<MlrVSTAudioProcessor::SceneChainTransitionOption>(juce::jlimit(
        0,
        static_cast<int>(MlrVSTAudioProcessor::SceneChainTransitionOption::Gate),
        rawOption));
}

MlrVSTAudioProcessor::SceneChainTransitionScope sanitizeRuntimeSceneChainTransitionScope(int rawScope)
{
    return static_cast<MlrVSTAudioProcessor::SceneChainTransitionScope>(juce::jlimit(
        0,
        static_cast<int>(MlrVSTAudioProcessor::SceneChainTransitionScope::Flip),
        rawScope));
}

MlrVSTAudioProcessor::SceneChainTransitionContour sanitizeRuntimeSceneChainTransitionContour(int rawContour)
{
    return static_cast<MlrVSTAudioProcessor::SceneChainTransitionContour>(juce::jlimit(
        0,
        static_cast<int>(MlrVSTAudioProcessor::SceneChainTransitionContour::LateHit),
        rawContour));
}

float sanitizeRuntimeSceneChainTransitionLengthBeats(float beats)
{
    if (!std::isfinite(beats))
        beats = MlrVSTAudioProcessor::DefaultSceneTransitionLengthBeats;
    return juce::jlimit(MlrVSTAudioProcessor::MinSceneTransitionLengthBeats,
                        MlrVSTAudioProcessor::MaxSceneTransitionLengthBeats,
                        beats);
}

float sanitizeRuntimeSceneChainTransitionAmount(float amount, float fallback)
{
    if (!std::isfinite(amount))
        amount = fallback;
    return juce::jlimit(0.0f, 1.0f, amount);
}

MlrVSTAudioProcessor::SceneTransitionEndSampleSettings sanitizeRuntimeSceneTransitionEndSampleSettings(
    MlrVSTAudioProcessor::SceneTransitionEndSampleSettings settings)
{
    auto sanitizeFinite = [](float value, float fallback, float minValue, float maxValue)
    {
        if (!std::isfinite(value))
            value = fallback;
        return juce::jlimit(minValue, maxValue, value);
    };

    settings.gainDb = sanitizeFinite(settings.gainDb,
                                     MlrVSTAudioProcessor::DefaultSceneTransitionEndSampleGainDb,
                                     -36.0f,
                                     24.0f);
    settings.fadeInMs = sanitizeFinite(settings.fadeInMs,
                                       MlrVSTAudioProcessor::DefaultSceneTransitionEndSampleFadeInMs,
                                       0.0f,
                                       500.0f);
    settings.fadeOutMs = sanitizeFinite(settings.fadeOutMs,
                                        MlrVSTAudioProcessor::DefaultSceneTransitionEndSampleFadeOutMs,
                                        0.0f,
                                        500.0f);
    settings.pitchSemitones = sanitizeFinite(settings.pitchSemitones,
                                             MlrVSTAudioProcessor::DefaultSceneTransitionEndSamplePitchSemitones,
                                             -24.0f,
                                             24.0f);
    settings.lowpassHz = sanitizeFinite(settings.lowpassHz,
                                        MlrVSTAudioProcessor::DefaultSceneTransitionEndSampleLowpassHz,
                                        20.0f,
                                        20000.0f);
    settings.highpassHz = sanitizeFinite(settings.highpassHz,
                                         MlrVSTAudioProcessor::DefaultSceneTransitionEndSampleHighpassHz,
                                         20.0f,
                                         20000.0f);
    settings.highpassHz = juce::jmin(settings.highpassHz, settings.lowpassHz);
    settings.duckSource = juce::jlimit(MlrVSTAudioProcessor::DefaultSceneTransitionEndSampleDuckSource,
                                       MlrVSTAudioProcessor::MaxStrips + 1,
                                       settings.duckSource);
    settings.duckAmount = sanitizeFinite(settings.duckAmount,
                                         MlrVSTAudioProcessor::DefaultSceneTransitionEndSampleDuckAmount,
                                         0.0f,
                                         1.0f);
    return settings;
}

float sceneTransitionEndSampleLowpassCoeff(float cutoffHz, double sampleRate)
{
    if (!std::isfinite(cutoffHz) || cutoffHz >= 19950.0f || sampleRate <= 0.0)
        return 0.0f;

    const float omega = static_cast<float>((2.0 * juce::MathConstants<double>::pi * cutoffHz) / sampleRate);
    return juce::jlimit(0.0f, 1.0f, 1.0f - std::exp(-omega));
}

float sceneTransitionEndSampleHighpassCoeff(float cutoffHz, double sampleRate)
{
    if (!std::isfinite(cutoffHz) || cutoffHz <= 20.5f || sampleRate <= 0.0)
        return 0.0f;

    const double rc = 1.0 / (2.0 * juce::MathConstants<double>::pi * static_cast<double>(cutoffHz));
    const double dt = 1.0 / sampleRate;
    return static_cast<float>(juce::jlimit(0.0, 1.0, rc / (rc + dt)));
}

struct SceneChainTransitionRuntimeProfile
{
    float depthScale = 1.0f;
    float delayScale = 1.0f;
    float filterScale = 1.0f;
    float stutterScale = 1.0f;
    float gateScale = 1.0f;
};

SceneChainTransitionRuntimeProfile sceneChainTransitionRuntimeProfile(MlrVSTAudioProcessor::SceneChainTransitionOption option)
{
    switch (sanitizeRuntimeSceneChainTransitionOption(static_cast<int>(option)))
    {
        case MlrVSTAudioProcessor::SceneChainTransitionOption::Snap:    return { 0.62f, 0.42f, 0.68f, 0.82f, 0.88f };
        case MlrVSTAudioProcessor::SceneChainTransitionOption::Tight:   return { 0.78f, 0.58f, 0.82f, 0.88f, 0.92f };
        case MlrVSTAudioProcessor::SceneChainTransitionOption::Wide:    return { 1.10f, 0.96f, 1.08f, 0.96f, 0.92f };
        case MlrVSTAudioProcessor::SceneChainTransitionOption::Wash:    return { 1.30f, 1.48f, 1.18f, 0.76f, 0.70f };
        case MlrVSTAudioProcessor::SceneChainTransitionOption::Echo:    return { 1.04f, 1.86f, 0.90f, 0.72f, 0.76f };
        case MlrVSTAudioProcessor::SceneChainTransitionOption::Sweep:   return { 1.02f, 0.86f, 1.72f, 0.74f, 0.80f };
        case MlrVSTAudioProcessor::SceneChainTransitionOption::Gate:    return { 0.96f, 0.56f, 0.78f, 1.18f, 1.26f };
        case MlrVSTAudioProcessor::SceneChainTransitionOption::Default:
        default:
            return { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
    }
}

bool sceneTransitionScopeMatchesStrip(MlrVSTAudioProcessor::SceneChainTransitionScope scope,
                                      const EnhancedAudioStrip& strip)
{
    using Scope = MlrVSTAudioProcessor::SceneChainTransitionScope;
    using PlayMode = EnhancedAudioStrip::PlayMode;

    switch (sanitizeRuntimeSceneChainTransitionScope(static_cast<int>(scope)))
    {
        case Scope::Loops:
        {
            const auto playMode = strip.getPlayMode();
            return playMode == PlayMode::Loop
                || playMode == PlayMode::Gate
                || playMode == PlayMode::OneShot;
        }
        case Scope::Steps:
            return strip.getPlayMode() == PlayMode::Step;
        case Scope::Grains:
            return strip.getPlayMode() == PlayMode::Grain;
        case Scope::Flip:
            return strip.getPlayMode() == PlayMode::Sample;
        case Scope::All:
        default:
            return true;
    }
}

bool sceneTransitionStripHasPlayableContent(const EnhancedAudioStrip& strip)
{
    const bool stepMode = strip.getPlayMode() == EnhancedAudioStrip::PlayMode::Step;
    const bool sampleMode = strip.getPlayMode() == EnhancedAudioStrip::PlayMode::Sample;
    const auto* stepSampler = stepMode ? strip.getStepSampler() : nullptr;
    return sampleMode || strip.hasAudio() || (stepSampler != nullptr && stepSampler->getHasAudio());
}

MlrVSTAudioProcessor::SceneChainTransitionScope sceneTransitionEffectiveRuntimeScope(
    MlrVSTAudioProcessor::SceneChainTransitionScope requestedScope,
    ModernAudioEngine& engine)
{
    const auto safeScope = sanitizeRuntimeSceneChainTransitionScope(static_cast<int>(requestedScope));
    if (safeScope == MlrVSTAudioProcessor::SceneChainTransitionScope::All)
        return safeScope;

    for (int stripIndex = 0; stripIndex < MlrVSTAudioProcessor::MaxStrips; ++stripIndex)
    {
        const auto* strip = engine.getStrip(stripIndex);
        if (strip != nullptr
            && sceneTransitionStripHasPlayableContent(*strip)
            && sceneTransitionScopeMatchesStrip(safeScope, *strip))
        {
            return safeScope;
        }
    }

    return MlrVSTAudioProcessor::SceneChainTransitionScope::All;
}

struct SceneTransitionContourShape
{
    float body = 1.0f;
    float delay = 1.0f;
    float filter = 1.0f;
    float chop = 1.0f;
    float duck = 1.0f;
    float volume = 1.0f;
};

SceneTransitionContourShape sceneTransitionContourShape(MlrVSTAudioProcessor::SceneChainTransitionContour contour,
                                                        float progress,
                                                        float body,
                                                        float attackWindow,
                                                        float overlayWindow)
{
    const float clampedProgress = juce::jlimit(0.0f, 1.0f, progress);
    const float lateLift = smoothSceneTransitionProgress(
        juce::jlimit(0.0f, 1.0f, (clampedProgress - 0.34f) / 0.66f));
    const float endBurst = smoothSceneTransitionProgress(
        juce::jlimit(0.0f, 1.0f, (clampedProgress - 0.56f) / 0.44f));
    const float lateHit = smoothSceneTransitionProgress(
        juce::jlimit(0.0f, 1.0f, (clampedProgress - 0.70f) / 0.30f));
    const float earlyDuck = 1.0f - smoothSceneTransitionProgress(
        juce::jlimit(0.0f, 1.0f, clampedProgress / 0.28f));
    const float attackAccent = juce::jlimit(0.0f, 1.0f, 0.58f + (0.42f * attackWindow));

    SceneTransitionContourShape shape;
    switch (sanitizeRuntimeSceneChainTransitionContour(static_cast<int>(contour)))
    {
        case MlrVSTAudioProcessor::SceneChainTransitionContour::Ramp:
            shape.body = juce::jlimit(0.0f, 1.4f, body * (0.54f + (0.82f * lateLift)));
            shape.delay = 0.78f + (0.42f * lateLift);
            shape.filter = 0.82f + (0.48f * lateLift);
            shape.chop = 0.78f + (0.16f * attackAccent);
            shape.duck = 0.84f + (0.18f * attackAccent);
            shape.volume = 0.68f + (0.54f * lateLift);
            break;
        case MlrVSTAudioProcessor::SceneChainTransitionContour::BurstEnd:
            shape.body = juce::jlimit(0.0f, 1.5f, body * (0.46f + (1.02f * endBurst)));
            shape.delay = 0.88f + (0.36f * endBurst);
            shape.filter = 0.82f + (0.26f * endBurst);
            shape.chop = 0.96f + (0.44f * endBurst);
            shape.duck = 0.92f + (0.34f * endBurst);
            shape.volume = 0.52f + (0.92f * endBurst);
            break;
        case MlrVSTAudioProcessor::SceneChainTransitionContour::DuckThenLift:
            shape.body = juce::jlimit(0.0f, 1.4f, body * (0.50f + (0.78f * lateLift)));
            shape.delay = 0.74f + (0.48f * lateLift);
            shape.filter = 0.80f + (0.52f * lateLift);
            shape.chop = 0.72f + (0.14f * attackAccent);
            shape.duck = 1.04f + (0.42f * earlyDuck) + (0.12f * overlayWindow);
            shape.volume = 0.46f + (0.40f * lateLift) + (0.18f * earlyDuck);
            break;
        case MlrVSTAudioProcessor::SceneChainTransitionContour::LateHit:
            shape.body = juce::jlimit(0.0f, 1.5f, body * (0.34f + (1.18f * lateHit)));
            shape.delay = 0.72f + (0.52f * lateHit);
            shape.filter = 0.74f + (0.46f * lateHit);
            shape.chop = 0.90f + (0.48f * lateHit);
            shape.duck = 0.86f + (0.36f * lateHit);
            shape.volume = 0.38f + (1.02f * lateHit);
            break;
        case MlrVSTAudioProcessor::SceneChainTransitionContour::Smooth:
        default:
            shape.volume = 0.96f + (0.12f * body);
            break;
    }

    return shape;
}

} // namespace

void MlrVSTAudioProcessor::updateEffectiveSceneStutterAmount()
{
    if (audioEngine == nullptr)
        return;

    const float baseAmount = juce::jlimit(0.0f, 1.0f, sceneGlobalStutterBaseAmount.load(std::memory_order_acquire));
    const float overlayAmount = juce::jlimit(0.0f, 1.0f, sceneTransitionStutterOverlayAmount.load(std::memory_order_acquire));
    const float effectiveAmount = juce::jmax(baseAmount, overlayAmount);
    for (int stripIndex = 0; stripIndex < MaxStrips; ++stripIndex)
        audioEngine->setMacroRetriggerAmount(stripIndex, effectiveAmount);
}

void MlrVSTAudioProcessor::setSceneTransitionStutterOverlayAmount(float amount01)
{
    const float clamped = juce::jlimit(0.0f, 1.0f, amount01);
    const float previous = sceneTransitionStutterOverlayAmount.load(std::memory_order_acquire);
    if (std::abs(previous - clamped) <= 1.0e-4f)
        return;

    sceneTransitionStutterOverlayAmount.store(clamped, std::memory_order_release);
    updateEffectiveSceneStutterAmount();
}

void MlrVSTAudioProcessor::setGlobalSceneStutterAmount(float amount01)
{
    const float clamped = juce::jlimit(0.0f, 1.0f, amount01);
    sceneGlobalStutterBaseAmount.store(clamped, std::memory_order_release);
    if (audioEngine == nullptr)
        return;

    updateEffectiveSceneStutterAmount();
}

float MlrVSTAudioProcessor::getGlobalSceneStutterAmount() const
{
    const float baseAmount = juce::jlimit(0.0f, 1.0f, sceneGlobalStutterBaseAmount.load(std::memory_order_acquire));
    const float overlayAmount = juce::jlimit(0.0f, 1.0f, sceneTransitionStutterOverlayAmount.load(std::memory_order_acquire));
    return juce::jmax(baseAmount, overlayAmount);
}

void MlrVSTAudioProcessor::clearSceneBoundaryTransitionState(bool preserveScheduledEndSample)
{
    sceneBoundaryTransitionType.store(static_cast<int>(SceneChainTransitionType::None), std::memory_order_release);
    sceneBoundaryTransitionOption.store(static_cast<int>(SceneChainTransitionOption::Default), std::memory_order_release);
    sceneBoundaryTransitionScope.store(static_cast<int>(SceneChainTransitionScope::All), std::memory_order_release);
    sceneBoundaryTransitionContour.store(static_cast<int>(SceneChainTransitionContour::Smooth), std::memory_order_release);
    sceneBoundaryTransitionFromStep.store(-1, std::memory_order_release);
    sceneBoundaryTransitionToStep.store(-1, std::memory_order_release);
    sceneBoundaryTransitionStartPpq.store(-1.0, std::memory_order_release);
    sceneBoundaryTransitionTargetPpq.store(-1.0, std::memory_order_release);
    sceneBoundaryTransitionStartSample.store(-1, std::memory_order_release);
    sceneBoundaryTransitionTargetSample.store(-1, std::memory_order_release);
    sceneBoundaryTransitionTempo.store(120.0, std::memory_order_release);
    sceneBoundaryTransitionLengthBeats.store(DefaultSceneTransitionLengthBeats, std::memory_order_release);
    sceneBoundaryTransitionIntensity.store(DefaultSceneTransitionIntensity, std::memory_order_release);
    sceneBoundaryTransitionDelayAmount.store(DefaultSceneTransitionDelayAmount, std::memory_order_release);
    sceneBoundaryTransitionFilterAmount.store(DefaultSceneTransitionFilterAmount, std::memory_order_release);
    sceneBoundaryTransitionChopAmount.store(DefaultSceneTransitionChopAmount, std::memory_order_release);
    if (!preserveScheduledEndSample)
    {
        sceneBoundaryTransitionEndSampleFromStep.store(-1, std::memory_order_release);
        sceneBoundaryTransitionEndSampleTargetPpq.store(-1.0, std::memory_order_release);
        sceneBoundaryTransitionEndSampleTargetSample.store(-1, std::memory_order_release);
        sceneBoundaryTransitionEndSampleIntensity.store(DefaultSceneTransitionIntensity, std::memory_order_release);
        sceneBoundaryTransitionEndSampleGainDb.store(DefaultSceneTransitionEndSampleGainDb, std::memory_order_release);
        sceneBoundaryTransitionEndSampleFadeInMs.store(DefaultSceneTransitionEndSampleFadeInMs, std::memory_order_release);
        sceneBoundaryTransitionEndSampleFadeOutMs.store(DefaultSceneTransitionEndSampleFadeOutMs, std::memory_order_release);
        sceneBoundaryTransitionEndSampleChokePrevious.store(0, std::memory_order_release);
        sceneBoundaryTransitionEndSampleReverse.store(0, std::memory_order_release);
        sceneBoundaryTransitionEndSamplePitchSemitones.store(DefaultSceneTransitionEndSamplePitchSemitones,
                                                             std::memory_order_release);
        sceneBoundaryTransitionEndSampleLowpassHz.store(DefaultSceneTransitionEndSampleLowpassHz,
                                                        std::memory_order_release);
        sceneBoundaryTransitionEndSampleHighpassHz.store(DefaultSceneTransitionEndSampleHighpassHz,
                                                         std::memory_order_release);
        sceneBoundaryTransitionEndSampleDuckSource.store(DefaultSceneTransitionEndSampleDuckSource,
                                                         std::memory_order_release);
        sceneBoundaryTransitionEndSampleDuckAmount.store(DefaultSceneTransitionEndSampleDuckAmount,
                                                         std::memory_order_release);
        sceneBoundaryTransitionEndSampleTriggered.store(0, std::memory_order_release);
    }
    setSceneTransitionStutterOverlayAmount(0.0f);
}

void MlrVSTAudioProcessor::armSceneBoundaryTransition(SceneChainTransitionType type,
                                                      SceneChainTransitionOption option,
                                                      SceneChainTransitionScope scope,
                                                      SceneChainTransitionContour contour,
                                                      float lengthBeats,
                                                      float intensity,
                                                      float delayAmount,
                                                      float filterAmount,
                                                      float chopAmount,
                                                      int fromStepIndex,
                                                      int toStepIndex,
                                                      double targetPpq,
                                                      double targetTempo,
                                                      double leadBeats,
                                                      int64_t targetSample,
                                                      double endSampleDelayBeats)
{
    const auto safeType = sanitizeRuntimeSceneChainTransitionType(static_cast<int>(type));
    if (safeType == SceneChainTransitionType::None || safeType == SceneChainTransitionType::Return)
    {
        clearSceneBoundaryTransitionState();
        return;
    }

    const double safeLeadBeats = juce::jlimit(0.0, 16.0, leadBeats);
    const double safeTargetPpq = (std::isfinite(targetPpq) && targetPpq >= 0.0) ? targetPpq : -1.0;
    const double safeEndSampleDelayBeats = juce::jlimit(0.0, 64.0, endSampleDelayBeats);
    double startPpq = -1.0;
    if (safeTargetPpq >= 0.0)
        startPpq = juce::jmax(0.0, safeTargetPpq - safeLeadBeats);
    double endSampleTargetPpq = -1.0;
    if (safeTargetPpq >= 0.0)
        endSampleTargetPpq = juce::jmax(safeTargetPpq, safeTargetPpq + safeEndSampleDelayBeats);

    int64_t startSample = -1;
    if (targetSample >= 0 && currentSampleRate > 0.0 && std::isfinite(targetTempo) && targetTempo > 0.0)
    {
        const double leadSeconds = (safeLeadBeats * 60.0) / targetTempo;
        const int64_t leadSamples = static_cast<int64_t>(std::llround(leadSeconds * currentSampleRate));
        startSample = juce::jmax<int64_t>(0, targetSample - juce::jmax<int64_t>(0, leadSamples));
    }
    int64_t endSampleTargetSample = -1;
    if (targetSample >= 0 && currentSampleRate > 0.0 && std::isfinite(targetTempo) && targetTempo > 0.0)
    {
        const double delaySeconds = (safeEndSampleDelayBeats * 60.0) / targetTempo;
        const int64_t delaySamples = static_cast<int64_t>(std::llround(delaySeconds * currentSampleRate));
        endSampleTargetSample = targetSample + juce::jmax<int64_t>(0, delaySamples);
    }
    const float safeIntensity = sanitizeRuntimeSceneChainTransitionAmount(intensity,
                                                                          DefaultSceneTransitionIntensity);
    auto endSampleSettings = SceneTransitionEndSampleSettings{};
    if (fromStepIndex >= 0 && fromStepIndex < MaxSceneChainSteps)
        endSampleSettings = sceneChainState.steps[static_cast<size_t>(fromStepIndex)].transitionEndSampleSettings;
    endSampleSettings = sanitizeRuntimeSceneTransitionEndSampleSettings(endSampleSettings);

    sceneBoundaryTransitionType.store(static_cast<int>(safeType), std::memory_order_release);
    sceneBoundaryTransitionOption.store(static_cast<int>(sanitizeRuntimeSceneChainTransitionOption(static_cast<int>(option))),
                                        std::memory_order_release);
    sceneBoundaryTransitionScope.store(static_cast<int>(sanitizeRuntimeSceneChainTransitionScope(static_cast<int>(scope))),
                                       std::memory_order_release);
    sceneBoundaryTransitionContour.store(static_cast<int>(sanitizeRuntimeSceneChainTransitionContour(static_cast<int>(contour))),
                                         std::memory_order_release);
    sceneBoundaryTransitionLengthBeats.store(sanitizeRuntimeSceneChainTransitionLengthBeats(lengthBeats),
                                             std::memory_order_release);
    sceneBoundaryTransitionIntensity.store(safeIntensity, std::memory_order_release);
    sceneBoundaryTransitionDelayAmount.store(sanitizeRuntimeSceneChainTransitionAmount(
                                                 delayAmount,
                                                 DefaultSceneTransitionDelayAmount),
                                             std::memory_order_release);
    sceneBoundaryTransitionFilterAmount.store(sanitizeRuntimeSceneChainTransitionAmount(
                                                  filterAmount,
                                                  DefaultSceneTransitionFilterAmount),
                                              std::memory_order_release);
    sceneBoundaryTransitionChopAmount.store(sanitizeRuntimeSceneChainTransitionAmount(
                                                chopAmount,
                                                DefaultSceneTransitionChopAmount),
                                            std::memory_order_release);
    sceneBoundaryTransitionFromStep.store(fromStepIndex, std::memory_order_release);
    sceneBoundaryTransitionToStep.store(toStepIndex, std::memory_order_release);
    sceneBoundaryTransitionStartPpq.store(startPpq, std::memory_order_release);
    sceneBoundaryTransitionTargetPpq.store(safeTargetPpq, std::memory_order_release);
    sceneBoundaryTransitionStartSample.store(startSample, std::memory_order_release);
    sceneBoundaryTransitionTargetSample.store(targetSample, std::memory_order_release);
    sceneBoundaryTransitionTempo.store((std::isfinite(targetTempo) && targetTempo > 0.0) ? targetTempo : 120.0,
                                       std::memory_order_release);
    // An end sample scheduled at the previous boundary fires up to a bar after
    // that switch; re-arming for the next boundary (which happens on the very
    // next block) must not destroy it while it is still waiting. A schedule
    // with an earlier fire time than the one being armed is that pending shot.
    const double pendingEndSamplePpq =
        sceneBoundaryTransitionEndSampleTargetPpq.load(std::memory_order_acquire);
    const bool preservePendingEndSample =
        sceneBoundaryTransitionEndSampleTriggered.load(std::memory_order_acquire) == 0
        && sceneBoundaryTransitionEndSampleFromStep.load(std::memory_order_acquire) >= 0
        && pendingEndSamplePpq >= 0.0
        && endSampleTargetPpq >= 0.0
        && pendingEndSamplePpq < endSampleTargetPpq - 1.0e-6;
    if (!preservePendingEndSample)
    {
        sceneBoundaryTransitionEndSampleFromStep.store(fromStepIndex, std::memory_order_release);
        sceneBoundaryTransitionEndSampleTargetPpq.store(endSampleTargetPpq, std::memory_order_release);
        sceneBoundaryTransitionEndSampleTargetSample.store(endSampleTargetSample, std::memory_order_release);
        sceneBoundaryTransitionEndSampleIntensity.store(safeIntensity, std::memory_order_release);
        sceneBoundaryTransitionEndSampleGainDb.store(endSampleSettings.gainDb, std::memory_order_release);
        sceneBoundaryTransitionEndSampleFadeInMs.store(endSampleSettings.fadeInMs, std::memory_order_release);
        sceneBoundaryTransitionEndSampleFadeOutMs.store(endSampleSettings.fadeOutMs, std::memory_order_release);
        sceneBoundaryTransitionEndSampleChokePrevious.store(endSampleSettings.chokePrevious ? 1 : 0,
                                                            std::memory_order_release);
        sceneBoundaryTransitionEndSampleReverse.store(endSampleSettings.reverse ? 1 : 0,
                                                      std::memory_order_release);
        sceneBoundaryTransitionEndSamplePitchSemitones.store(endSampleSettings.pitchSemitones, std::memory_order_release);
        sceneBoundaryTransitionEndSampleLowpassHz.store(endSampleSettings.lowpassHz, std::memory_order_release);
        sceneBoundaryTransitionEndSampleHighpassHz.store(endSampleSettings.highpassHz, std::memory_order_release);
        sceneBoundaryTransitionEndSampleDuckSource.store(endSampleSettings.duckSource, std::memory_order_release);
        sceneBoundaryTransitionEndSampleDuckAmount.store(endSampleSettings.duckAmount, std::memory_order_release);
        sceneBoundaryTransitionEndSampleTriggered.store(0, std::memory_order_release);
    }
}

void MlrVSTAudioProcessor::armSceneChainReturnOverride(int sourceStepIndex, int triggerStepIndex)
{
    sceneChainReturnOverrideSourceStep.store(sourceStepIndex, std::memory_order_release);
    sceneChainReturnOverrideTriggerStep.store(triggerStepIndex, std::memory_order_release);
    sceneChainReturnOverrideActive.store((sourceStepIndex >= 0 && triggerStepIndex >= 0) ? 1 : 0,
                                         std::memory_order_release);
}

void MlrVSTAudioProcessor::clearSceneChainReturnOverride()
{
    sceneChainReturnOverrideActive.store(0, std::memory_order_release);
    sceneChainReturnOverrideSourceStep.store(-1, std::memory_order_release);
    sceneChainReturnOverrideTriggerStep.store(-1, std::memory_order_release);
}

bool MlrVSTAudioProcessor::consumeSceneChainReturnOverrideForStep(int currentStepIndex, int& outNextStepIndex)
{
    if (sceneChainReturnOverrideActive.load(std::memory_order_acquire) == 0)
        return false;

    const int triggerStepIndex = sceneChainReturnOverrideTriggerStep.load(std::memory_order_acquire);
    if (currentStepIndex != triggerStepIndex)
        return false;

    outNextStepIndex = sceneChainReturnOverrideSourceStep.load(std::memory_order_acquire);
    clearSceneChainReturnOverride();
    return outNextStepIndex >= 0;
}

void MlrVSTAudioProcessor::applySceneBoundaryTransitionOverlay(const juce::AudioPlayHead::PositionInfo& posInfo,
                                                               int /*numSamples*/)
{
    if (audioEngine == nullptr)
    {
        setSceneTransitionStutterOverlayAmount(0.0f);
        return;
    }

    const auto transitionType = sanitizeRuntimeSceneChainTransitionType(
        sceneBoundaryTransitionType.load(std::memory_order_acquire));
    const auto transitionOption = sanitizeRuntimeSceneChainTransitionOption(
        sceneBoundaryTransitionOption.load(std::memory_order_acquire));
    const auto requestedTransitionScope = sanitizeRuntimeSceneChainTransitionScope(
        sceneBoundaryTransitionScope.load(std::memory_order_acquire));
    const auto transitionContour = sanitizeRuntimeSceneChainTransitionContour(
        sceneBoundaryTransitionContour.load(std::memory_order_acquire));
    const auto transitionProfile = sceneChainTransitionRuntimeProfile(transitionOption);
    const auto transitionScope = sceneTransitionEffectiveRuntimeScope(requestedTransitionScope, *audioEngine);
    const float transitionLengthBeats = sanitizeRuntimeSceneChainTransitionLengthBeats(
        sceneBoundaryTransitionLengthBeats.load(std::memory_order_acquire));
    const float transitionIntensity = sanitizeRuntimeSceneChainTransitionAmount(
        sceneBoundaryTransitionIntensity.load(std::memory_order_acquire),
        DefaultSceneTransitionIntensity);
    const float transitionDelayAmount = sanitizeRuntimeSceneChainTransitionAmount(
        sceneBoundaryTransitionDelayAmount.load(std::memory_order_acquire),
        DefaultSceneTransitionDelayAmount);
    const float transitionFilterAmount = sanitizeRuntimeSceneChainTransitionAmount(
        sceneBoundaryTransitionFilterAmount.load(std::memory_order_acquire),
        DefaultSceneTransitionFilterAmount);
    const float transitionChopAmount = sanitizeRuntimeSceneChainTransitionAmount(
        sceneBoundaryTransitionChopAmount.load(std::memory_order_acquire),
        DefaultSceneTransitionChopAmount);
    if (transitionType == SceneChainTransitionType::None
        || transitionType == SceneChainTransitionType::Return)
    {
        setSceneTransitionStutterOverlayAmount(0.0f);
        return;
    }

    double currentPpq = audioEngine->getTimelineBeat();
    if (posInfo.getPpqPosition().hasValue())
        currentPpq = *posInfo.getPpqPosition();
    const int64_t currentSample = audioEngine->getGlobalSampleCount();

    const double targetPpq = sceneBoundaryTransitionTargetPpq.load(std::memory_order_acquire);
    const int64_t targetSample = sceneBoundaryTransitionTargetSample.load(std::memory_order_acquire);
    const double transitionTempo = sceneBoundaryTransitionTempo.load(std::memory_order_acquire);
    const double leadBeats = sceneChainTransitionLeadBeatsRuntime(transitionType, transitionLengthBeats);
    double startPpq = sceneBoundaryTransitionStartPpq.load(std::memory_order_acquire);
    if (std::isfinite(targetPpq) && targetPpq >= 0.0 && leadBeats > 0.0)
        startPpq = juce::jmax(0.0, targetPpq - leadBeats);

    int64_t startSample = sceneBoundaryTransitionStartSample.load(std::memory_order_acquire);
    if (targetSample >= 0
        && currentSampleRate > 0.0
        && std::isfinite(transitionTempo)
        && transitionTempo > 0.0
        && leadBeats > 0.0)
    {
        const double leadSeconds = (leadBeats * 60.0) / transitionTempo;
        const int64_t leadSamples = static_cast<int64_t>(std::llround(leadSeconds * currentSampleRate));
        startSample = juce::jmax<int64_t>(0, targetSample - juce::jmax<int64_t>(0, leadSamples));
    }

    float progress = 0.0f;
    if (std::isfinite(currentPpq)
        && std::isfinite(startPpq)
        && std::isfinite(targetPpq)
        && targetPpq >= startPpq
        && targetPpq >= 0.0)
    {
        if (currentPpq >= targetPpq - 1.0e-6)
            progress = 1.0f;
        else if (currentPpq > startPpq + 1.0e-6)
            progress = static_cast<float>((currentPpq - startPpq)
                                          / juce::jmax(1.0e-9, targetPpq - startPpq));
    }
    else if (startSample >= 0 && targetSample >= startSample && currentSample >= 0)
    {
        if (currentSample >= targetSample)
            progress = 1.0f;
        else if (currentSample > startSample)
            progress = static_cast<float>(
                static_cast<double>(currentSample - startSample)
                / static_cast<double>(juce::jmax<int64_t>(1, targetSample - startSample)));
    }

    progress = juce::jlimit(0.0f, 1.0f, progress);
    if (progress <= 0.0f)
    {
        setSceneTransitionStutterOverlayAmount(0.0f);
        return;
    }

    const float eased = smoothSceneTransitionProgress(progress);
    const float attackWindow = smoothSceneTransitionProgress(juce::jlimit(0.0f, 1.0f, progress / 0.16f));
    const float releaseWindow = 1.0f
        - smoothSceneTransitionProgress(juce::jlimit(0.0f, 1.0f, (progress - 0.84f) / 0.16f));
    const float overlayWindow = juce::jlimit(0.0f, 1.0f, attackWindow * releaseWindow);
    const float baseBody = juce::jlimit(0.0f,
                                        1.0f,
                                        juce::jmax(eased,
                                                   juce::jmax(std::pow(progress, 0.62f),
                                                              std::sin(progress * juce::MathConstants<float>::pi))));
    const float body = juce::jlimit(0.0f, 1.0f, baseBody * overlayWindow);
    const auto contourShape = sceneTransitionContourShape(transitionContour,
                                                          progress,
                                                          body,
                                                          attackWindow,
                                                          overlayWindow);
    const float contourBody = juce::jlimit(0.0f, 1.0f, contourShape.body);
    const float contourVolume = juce::jlimit(0.0f, 1.6f, contourShape.volume);
    double beatsSinceStart = std::numeric_limits<double>::quiet_NaN();
    if (std::isfinite(currentPpq) && std::isfinite(startPpq))
        beatsSinceStart = juce::jmax(0.0, currentPpq - startPpq);
    else if (currentSample >= 0
             && startSample >= 0
             && std::isfinite(transitionTempo)
             && transitionTempo > 0.0
             && currentSampleRate > 0.0)
        beatsSinceStart = (static_cast<double>(currentSample - startSample) * transitionTempo)
            / (60.0 * currentSampleRate);

    const float leadNormal = juce::jlimit(
        0.0f,
        1.0f,
        static_cast<float>((leadBeats - static_cast<double>(MinSceneTransitionLengthBeats))
                           / static_cast<double>(MaxSceneTransitionLengthBeats - MinSceneTransitionLengthBeats)));
    const float shortLeadFocus = 1.0f - leadNormal;
    const float longLeadFocus = leadNormal;
    double pulseDensity = 2.0 + (10.0 * static_cast<double>(transitionChopAmount));
    if (transitionType == SceneChainTransitionType::Stutter
        || transitionType == SceneChainTransitionType::Break)
        pulseDensity += 2.0 * static_cast<double>(shortLeadFocus);
    else if (transitionType == SceneChainTransitionType::Fill
             || transitionType == SceneChainTransitionType::FilterRise)
        pulseDensity -= 1.0 * static_cast<double>(longLeadFocus);
    const double pulseSpacingBeats = juce::jmax(
        1.0 / 16.0,
        leadBeats / juce::jlimit(2.0, 12.0, pulseDensity));
    const float rhythmicPulse = sceneTransitionRhythmicPulse(
        beatsSinceStart,
        pulseSpacingBeats,
        0.8f + (3.4f * transitionChopAmount));
    const float gatePulse = juce::jlimit(0.0f, 1.0f, 0.18f + (0.82f * rhythmicPulse));
    const float lengthDelayBias = 0.82f + (0.48f * longLeadFocus);
    const float lengthFilterBias = 0.80f + (0.54f * longLeadFocus);
    const float lengthChopBias = 0.82f + (0.56f * shortLeadFocus);
    const float lengthDuckBias = 0.84f + (0.46f * shortLeadFocus);
    const float transitionDrive = juce::jlimit(0.0f, 1.0f,
                                               (0.24f + (0.76f * transitionIntensity))
                                                   * transitionProfile.depthScale);
    const float delayEffect = juce::jlimit(0.0f, 1.0f,
                                           contourBody * contourShape.delay * transitionProfile.delayScale
                                               * lengthDelayBias
                                               * transitionDrive
                                               * (0.22f + (0.78f * transitionDelayAmount)));
    const float filterEffect = juce::jlimit(0.0f, 1.0f,
                                            contourBody * contourShape.filter * transitionProfile.filterScale
                                                * lengthFilterBias
                                                * transitionDrive
                                                * (0.22f + (0.78f * transitionFilterAmount)));
    const float chopEffect = juce::jlimit(0.0f, 1.0f,
                                          contourBody * contourShape.chop * transitionProfile.stutterScale
                                              * lengthChopBias
                                              * transitionDrive
                                              * (0.18f + (0.82f * transitionChopAmount)));
    const float duckEffect = juce::jlimit(0.0f, 1.0f,
                                          contourBody * contourShape.duck * transitionProfile.gateScale
                                              * lengthDuckBias
                                              * transitionDrive
                                              * (0.18f + (0.82f * transitionChopAmount)));
    const float contourAccent = juce::jlimit(0.0f, 1.0f, contourBody * juce::jmax(0.0f, contourVolume - 0.6f));
    const float contourLift = juce::jlimit(0.0f,
                                           0.42f,
                                           (0.04f + (0.28f * transitionDrive)) * contourAccent);
    float stutterOverlayAmount = 0.0f;

    for (int stripIndex = 0; stripIndex < MaxStrips; ++stripIndex)
    {
        auto* strip = audioEngine->getStrip(stripIndex);
        if (strip == nullptr)
            continue;

        const bool stepMode = strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Step;
        auto* stepSampler = stepMode ? strip->getStepSampler() : nullptr;
        const bool hasPlayableContent = sceneTransitionStripHasPlayableContent(*strip);
        if (!hasPlayableContent || !sceneTransitionScopeMatchesStrip(transitionScope, *strip))
            continue;

        float targetVolume = strip->getVolume();
        float targetFilterFrequency = strip->getFilterFrequency();
        float targetFilterResonance = strip->getFilterResonance();
        float targetDelayMix = strip->getDelayMix();
        float targetDelayTimeBeats = strip->getDelayTimeBeats();
        float targetDelayFeedback = strip->getDelayFeedback();
        bool applyFilter = false;

        switch (transitionType)
        {
            case SceneChainTransitionType::Fill:
            {
                const float fillPickupAccent = juce::jlimit(0.0f,
                                                            1.0f,
                                                            rhythmicPulse
                                                                * overlayWindow
                                                                * chopEffect
                                                                * (0.22f + (0.46f * transitionDrive)));
                targetVolume = juce::jlimit(0.0f, 1.0f,
                                            targetVolume * juce::jlimit(0.0f,
                                                                        1.30f,
                                                                        (1.0f + contourLift + (0.12f * fillPickupAccent))
                                                                            - ((0.05f + (0.12f * transitionDrive)) * duckEffect)));
                targetFilterFrequency = juce::jmap(filterEffect, 20000.0f, 1200.0f);
                targetFilterResonance = juce::jmap(filterEffect, 0.82f, 1.92f);
                targetDelayMix = juce::jlimit(0.0f, 1.0f,
                                              targetDelayMix + ((0.30f + (0.40f * transitionDrive)) * delayEffect));
                const float musicalDelayTargetBeats =
                    transitionOption == SceneChainTransitionOption::Echo
                        ? (longLeadFocus > 0.28f ? 0.5f : 0.25f)
                        : (transitionOption == SceneChainTransitionOption::Wash ? 0.75f : 0.25f);
                targetDelayTimeBeats = juce::jlimit(0.25f,
                                                    4.0f,
                                                    targetDelayTimeBeats
                                                        + ((musicalDelayTargetBeats - targetDelayTimeBeats)
                                                           * delayEffect
                                                           * (0.34f + (0.42f * transitionDrive))));
                targetDelayFeedback = juce::jlimit(0.0f,
                                                   0.97f,
                                                   targetDelayFeedback + ((0.20f + (0.34f * transitionDrive)) * delayEffect));
                stutterOverlayAmount = juce::jmax(stutterOverlayAmount,
                                                  juce::jlimit(0.0f,
                                                               1.0f,
                                                               (0.10f + (0.24f * transitionDrive))
                                                                   * chopEffect
                                                                   * gatePulse
                                                                   * (0.72f + (0.48f * contourAccent))));
                applyFilter = filterEffect > 0.01f;
                break;
            }
            case SceneChainTransitionType::Stutter:
                targetVolume = juce::jlimit(0.0f, 1.0f,
                                            targetVolume * juce::jlimit(0.14f,
                                                                        1.0f,
                                                                        (0.34f + (0.66f * gatePulse))
                                                                            * (0.88f + contourLift)
                                                                            * (1.0f - ((0.08f + (0.18f * transitionDrive))
                                                                                        * duckEffect
                                                                                        * (1.04f - (0.22f * contourAccent))))));
                targetFilterFrequency = juce::jmap(filterEffect, 18000.0f, 4200.0f);
                targetFilterResonance = juce::jmap(filterEffect, 0.82f, 1.52f);
                targetDelayMix = juce::jlimit(0.0f, 1.0f,
                                              targetDelayMix + ((0.08f + (0.18f * transitionDrive)) * delayEffect));
                targetDelayFeedback = juce::jlimit(0.0f,
                                                   0.97f,
                                                   targetDelayFeedback + ((0.10f + (0.16f * transitionDrive)) * delayEffect));
                stutterOverlayAmount = juce::jmax(stutterOverlayAmount,
                                                  juce::jlimit(0.0f,
                                                               1.0f,
                                                               (0.24f + (0.50f * transitionDrive))
                                                                   * chopEffect
                                                                   * (0.82f + (0.36f * contourAccent))));
                applyFilter = filterEffect > 0.02f;
                break;
            case SceneChainTransitionType::FilterRise:
                targetVolume = juce::jlimit(0.0f, 1.0f,
                                            targetVolume * juce::jlimit(0.0f,
                                                                        1.26f,
                                                                        (1.0f + contourLift)
                                                                            - ((0.04f + (0.10f * transitionDrive)) * duckEffect)));
                targetFilterFrequency = juce::jmap(filterEffect, 140.0f, 19500.0f);
                targetFilterResonance = juce::jmap(filterEffect, 0.92f, 2.10f);
                targetDelayMix = juce::jlimit(0.0f, 1.0f,
                                              targetDelayMix + ((0.06f + (0.12f * transitionDrive)) * delayEffect));
                targetDelayFeedback = juce::jlimit(0.0f,
                                                   0.97f,
                                                   targetDelayFeedback + ((0.04f + (0.12f * transitionDrive)) * delayEffect));
                applyFilter = true;
                break;
            case SceneChainTransitionType::Drop:
                targetVolume = juce::jlimit(0.0f, 1.0f,
                                            targetVolume * juce::jlimit(0.0f,
                                                                        1.08f,
                                                                        (0.94f + (0.30f * contourAccent))
                                                                            - ((0.24f + (0.36f * transitionDrive))
                                                                               * duckEffect
                                                                               * (1.0f - (0.18f * contourAccent)))));
                targetFilterFrequency = juce::jmap(filterEffect, 17000.0f, 520.0f);
                targetFilterResonance = juce::jmap(filterEffect, 0.84f, 1.78f);
                targetDelayMix = juce::jlimit(0.0f, 1.0f,
                                              targetDelayMix + ((0.14f + (0.24f * transitionDrive)) * delayEffect));
                targetDelayFeedback = juce::jlimit(0.0f,
                                                   0.97f,
                                                   targetDelayFeedback + ((0.10f + (0.18f * transitionDrive)) * delayEffect));
                applyFilter = filterEffect > 0.01f;
                break;
            case SceneChainTransitionType::MuteTail:
                targetVolume = juce::jlimit(0.0f, 1.0f,
                                            targetVolume * juce::jmax(0.0f,
                                                                      (0.78f + (0.22f * contourAccent))
                                                                          - ((0.52f + (0.42f * transitionDrive))
                                                                             * duckEffect
                                                                             * (1.0f - (0.22f * contourAccent)))));
                targetDelayMix = juce::jlimit(0.0f,
                                              1.0f,
                                              targetDelayMix * (1.0f - juce::jmin(1.0f,
                                                                                  delayEffect * (0.82f + (0.16f * transitionDrive)))));
                targetDelayFeedback = juce::jlimit(0.0f,
                                                   0.97f,
                                                   targetDelayFeedback * (1.0f - juce::jmin(1.0f,
                                                                                           delayEffect * (0.86f + (0.12f * transitionDrive)))));
                break;
            case SceneChainTransitionType::Break:
                targetVolume = juce::jlimit(0.0f,
                                            1.0f,
                                            targetVolume * juce::jlimit(0.08f,
                                                                        1.0f,
                                                                        (0.18f + (0.82f * gatePulse))
                                                                            * (0.86f + (0.28f * contourAccent))
                                                                            * juce::jmax(0.0f,
                                                                                         1.0f - ((0.28f + (0.34f * transitionDrive))
                                                                                                 * duckEffect
                                                                                                 * (1.0f - (0.18f * contourAccent))))));
                targetFilterFrequency = juce::jmap(filterEffect, 18000.0f, 2200.0f);
                targetFilterResonance = juce::jmap(filterEffect, 0.82f, 1.72f);
                targetDelayMix = juce::jlimit(0.0f,
                                              1.0f,
                                              targetDelayMix + ((0.14f + (0.28f * transitionDrive)) * delayEffect));
                targetDelayFeedback = juce::jlimit(0.0f,
                                                   0.97f,
                                                   targetDelayFeedback + ((0.16f + (0.24f * transitionDrive)) * delayEffect));
                stutterOverlayAmount = juce::jmax(stutterOverlayAmount,
                                                  juce::jlimit(0.0f,
                                                               1.0f,
                                                               (0.08f + (0.22f * transitionDrive))
                                                                   * chopEffect
                                                                   * (0.80f + (0.34f * contourAccent))));
                applyFilter = filterEffect > 0.02f;
                break;
            case SceneChainTransitionType::None:
            case SceneChainTransitionType::Return:
            default:
                break;
        }

        strip->setVolume(targetVolume);
        if (stepSampler != nullptr)
            stepSampler->setVolume(targetVolume);

        if (applyFilter)
        {
            strip->setFilterEnabled(true);
            strip->setFilterFrequency(targetFilterFrequency);
            strip->setFilterResonance(targetFilterResonance);
            if (stepSampler != nullptr)
            {
                stepSampler->setFilterEnabled(true);
                stepSampler->setFilterFrequency(targetFilterFrequency);
                stepSampler->setFilterResonance(targetFilterResonance);
            }
        }

        strip->setDelayMix(targetDelayMix);
        strip->setDelayTimeBeats(targetDelayTimeBeats);
        strip->setDelayFeedback(targetDelayFeedback);
    }

    setSceneTransitionStutterOverlayAmount(stutterOverlayAmount);
}

void MlrVSTAudioProcessor::renderSceneTransitionEndSampleVoices(juce::AudioBuffer<float>& buffer,
                                                                const juce::AudioPlayHead::PositionInfo& posInfo,
                                                                int64_t blockStartSample,
                                                                bool allowTriggering)
{
    const int numSamples = buffer.getNumSamples();
    const int outputChannels = juce::jmin(2, buffer.getNumChannels());
    if (numSamples <= 0 || outputChannels <= 0 || currentSampleRate <= 0.0)
        return;

    const auto smoothingCoeffFromMs = [this](float ms)
    {
        const double safeMs = juce::jmax(0.001, static_cast<double>(ms));
        const double safeRate = juce::jmax(1.0, currentSampleRate);
        return static_cast<float>(std::exp(-1.0 / ((safeMs * 0.001) * safeRate)));
    };
    const float duckAttackCoeff = smoothingCoeffFromMs(4.0f);
    const float duckReleaseCoeff = smoothingCoeffFromMs(110.0f);
    std::array<float, MaxStrips> stripSidechainLevels{};
    float masterSidechainLevel = 0.0f;
    if (audioEngine != nullptr)
    {
        masterSidechainLevel = juce::jlimit(0.0f, 1.0f, audioEngine->getMasterOutputLevel());
        for (int stripIndex = 0; stripIndex < MaxStrips; ++stripIndex)
        {
            stripSidechainLevels[static_cast<size_t>(stripIndex)] =
                juce::jlimit(0.0f, 1.0f, audioEngine->getStripOutputLevel(stripIndex));
        }
    }

    auto duckTargetGain = [&](int sourceSelection, float duckAmount)
    {
        const float safeAmount = juce::jlimit(0.0f, 1.0f, duckAmount);
        if (safeAmount <= 0.001f || sourceSelection <= 0)
            return 1.0f;

        float sidechainLevel = 0.0f;
        if (sourceSelection == 1)
        {
            sidechainLevel = masterSidechainLevel;
        }
        else
        {
            const int stripIndex = sourceSelection - 2;
            if (stripIndex >= 0 && stripIndex < MaxStrips)
                sidechainLevel = stripSidechainLevels[static_cast<size_t>(stripIndex)];
        }

        const float normalizedLevel = juce::jlimit(0.0f, 1.0f,
                                                   std::sqrt(juce::jlimit(0.0f, 1.0f, sidechainLevel * 2.0f)));
        return juce::jlimit(0.0f, 1.0f, 1.0f - (safeAmount * normalizedLevel));
    };

    auto configureVoice = [this, numSamples](SceneTransitionEndSampleVoice& voice,
                                             std::shared_ptr<const SceneTransitionEndSampleData> sample,
                                             const SceneTransitionEndSampleSettings& rawSettings,
                                             float baseGainLinear,
                                             int startOffsetSamples)
    {
        voice = {};
        if (sample == nullptr || sample->buffer.getNumSamples() <= 0)
            return;

        const auto settings = sanitizeRuntimeSceneTransitionEndSampleSettings(rawSettings);
        const int sampleLength = sample->buffer.getNumSamples();
        const double sourceRate = sample->sourceSampleRate > 0.0 ? sample->sourceSampleRate : currentSampleRate;
        const double pitchRatio = std::pow(2.0, static_cast<double>(settings.pitchSemitones) / 12.0);
        const double increment = juce::jmax(1.0e-6, (sourceRate / currentSampleRate) * pitchRatio);

        voice.active = true;
        voice.sample = std::move(sample);
        voice.reverse = settings.reverse;
        voice.sourceSamplePosition = voice.reverse ? static_cast<double>(juce::jmax(0, sampleLength - 1)) : 0.0;
        voice.sourceIncrement = increment;
        voice.startOffsetSamples = juce::jlimit(0, juce::jmax(0, numSamples - 1), startOffsetSamples);
        voice.gainLinear = juce::jlimit(0.0f,
                                        2.5f,
                                        baseGainLinear * juce::Decibels::decibelsToGain(settings.gainDb));
        voice.fadeInSamples = juce::jmax(0.0f, settings.fadeInMs * 0.001f * static_cast<float>(currentSampleRate));
        voice.fadeOutSamples = juce::jmax(0.0f, settings.fadeOutMs * 0.001f * static_cast<float>(currentSampleRate));
        voice.lowpassCoeff = sceneTransitionEndSampleLowpassCoeff(settings.lowpassHz, currentSampleRate);
        voice.highpassCoeff = sceneTransitionEndSampleHighpassCoeff(settings.highpassHz, currentSampleRate);
        voice.lowpassEnabled = voice.lowpassCoeff > 0.0f;
        voice.highpassEnabled = voice.highpassCoeff > 0.0f;
        voice.duckSourceSelection = settings.duckSource;
        voice.duckAmount = settings.duckAmount;
        voice.duckSmoothedGain = 1.0f;
    };

    auto renderVoice = [&](SceneTransitionEndSampleVoice& voice)
    {
        if (!voice.active || voice.sample == nullptr)
            return;

        const auto& sampleData = *voice.sample;
        const auto& sampleBuffer = sampleData.buffer;
        const int sampleChannels = juce::jmax(1, sampleBuffer.getNumChannels());
        const int sampleLength = sampleBuffer.getNumSamples();
        if (sampleLength <= 0)
        {
            voice = {};
            return;
        }

        const double absIncrement = juce::jmax(1.0e-6, std::abs(voice.sourceIncrement));
        const int startOffset = juce::jlimit(0, numSamples, voice.startOffsetSamples);
        const float targetDuckGain = duckTargetGain(voice.duckSourceSelection, voice.duckAmount);
        voice.startOffsetSamples = 0;

        for (int sampleIndex = startOffset; sampleIndex < numSamples; ++sampleIndex)
        {
            const double position = voice.sourceSamplePosition;
            if ((!voice.reverse && position >= static_cast<double>(sampleLength))
                || (voice.reverse && position < 0.0))
            {
                voice = {};
                break;
            }

            const int baseIndex = juce::jlimit(0, sampleLength - 1, static_cast<int>(position));
            const int nextIndex = voice.reverse
                ? juce::jmax(0, baseIndex - 1)
                : juce::jmin(sampleLength - 1, baseIndex + 1);
            const float alpha = static_cast<float>(position - static_cast<double>(baseIndex));
            const double outputSamplesRemaining = voice.reverse
                ? (position / absIncrement)
                : ((static_cast<double>(sampleLength - 1) - position) / absIncrement);
            const float fadeInGain = voice.fadeInSamples > 0.0f
                ? juce::jlimit(0.0f,
                               1.0f,
                               static_cast<float>(static_cast<double>(voice.renderedSamples + 1) / voice.fadeInSamples))
                : 1.0f;
            const float fadeOutGain = voice.fadeOutSamples > 0.0f
                ? juce::jlimit(0.0f, 1.0f, static_cast<float>(outputSamplesRemaining / voice.fadeOutSamples))
                : 1.0f;
            const float duckCoeff = (targetDuckGain < voice.duckSmoothedGain) ? duckAttackCoeff : duckReleaseCoeff;
            voice.duckSmoothedGain = targetDuckGain
                + (duckCoeff * (voice.duckSmoothedGain - targetDuckGain));
            const float envelope = juce::jmin(fadeInGain, fadeOutGain) * voice.duckSmoothedGain;

            for (int channel = 0; channel < outputChannels; ++channel)
            {
                const int sourceChannel = juce::jmin(channel, sampleChannels - 1);
                float output = juce::jmap(alpha,
                                          sampleBuffer.getSample(sourceChannel, baseIndex),
                                          sampleBuffer.getSample(sourceChannel, nextIndex));

                if (voice.lowpassEnabled)
                {
                    auto& state = voice.lowpassState[static_cast<size_t>(channel)];
                    state += voice.lowpassCoeff * (output - state);
                    output = state;
                }

                if (voice.highpassEnabled)
                {
                    auto& prevInput = voice.highpassInputState[static_cast<size_t>(channel)];
                    auto& prevOutput = voice.highpassOutputState[static_cast<size_t>(channel)];
                    const float filtered = voice.highpassCoeff * (prevOutput + output - prevInput);
                    prevInput = output;
                    prevOutput = filtered;
                    output = filtered;
                }

                buffer.addSample(channel, sampleIndex, output * voice.gainLinear * envelope);
            }

            voice.sourceSamplePosition += voice.reverse ? -absIncrement : absIncrement;
            ++voice.renderedSamples;
        }

        if (voice.active
            && ((!voice.reverse && voice.sourceSamplePosition >= static_cast<double>(sampleLength))
                || (voice.reverse && voice.sourceSamplePosition < 0.0)))
        {
            voice = {};
        }
    };

    if (allowTriggering)
    {
        auto previewRequest = std::atomic_exchange_explicit(
            &sceneTransitionEndSamplePreviewRequest,
            std::shared_ptr<const SceneTransitionEndSamplePreviewRequest>{},
            std::memory_order_acq_rel);
        if (previewRequest != nullptr && previewRequest->sample != nullptr)
            configureVoice(sceneTransitionEndSamplePreviewVoice,
                           previewRequest->sample,
                           previewRequest->settings,
                           0.82f,
                           0);
    }

    if (allowTriggering && sceneBoundaryTransitionEndSampleTriggered.load(std::memory_order_acquire) == 0)
    {
        const int fromStepIndex = sceneBoundaryTransitionEndSampleFromStep.load(std::memory_order_acquire);
        if (fromStepIndex >= 0 && fromStepIndex < MaxSceneChainSteps)
        {
            int triggerOffset = -1;

            const double targetPpq = sceneBoundaryTransitionEndSampleTargetPpq.load(std::memory_order_acquire);
            if (posInfo.getPpqPosition().hasValue()
                && posInfo.getBpm().hasValue()
                && std::isfinite(*posInfo.getPpqPosition())
                && std::isfinite(*posInfo.getBpm())
                && *posInfo.getBpm() > 0.0
                && std::isfinite(targetPpq))
            {
                const double blockStartPpq = *posInfo.getPpqPosition();
                const double ppqPerSample = *posInfo.getBpm() / (60.0 * currentSampleRate);
                const double blockEndPpq = blockStartPpq + (ppqPerSample * static_cast<double>(numSamples));
                if (targetPpq <= blockStartPpq)
                    triggerOffset = 0;
                else if (targetPpq < blockEndPpq)
                    triggerOffset = juce::jlimit(0,
                                                 juce::jmax(0, numSamples - 1),
                                                 static_cast<int>(std::llround((targetPpq - blockStartPpq)
                                                                               / ppqPerSample)));
            }
            else
            {
                const int64_t targetSample = sceneBoundaryTransitionEndSampleTargetSample.load(std::memory_order_acquire);
                if (targetSample >= 0)
                {
                    const int64_t blockEndSample = blockStartSample + static_cast<int64_t>(numSamples);
                    if (targetSample < blockStartSample)
                        triggerOffset = 0;
                    else if (targetSample < blockEndSample)
                        triggerOffset = static_cast<int>(targetSample - blockStartSample);
                }
            }

            if (triggerOffset >= 0)
            {
                sceneBoundaryTransitionEndSampleTriggered.store(1, std::memory_order_release);
                sceneBoundaryTransitionEndSampleFromStep.store(-1, std::memory_order_release);
                sceneBoundaryTransitionEndSampleTargetPpq.store(-1.0, std::memory_order_release);
                sceneBoundaryTransitionEndSampleTargetSample.store(-1, std::memory_order_release);
                auto sample = std::atomic_load_explicit(
                    &sceneTransitionEndSamples[static_cast<size_t>(fromStepIndex)],
                    std::memory_order_acquire);
                if (sample != nullptr && sample->buffer.getNumSamples() > 0)
                {
                    const float intensity = sanitizeRuntimeSceneChainTransitionAmount(
                        sceneBoundaryTransitionEndSampleIntensity.load(std::memory_order_acquire),
                        DefaultSceneTransitionIntensity);
                    SceneTransitionEndSampleSettings settings;
                    settings.gainDb = sceneBoundaryTransitionEndSampleGainDb.load(std::memory_order_acquire);
                    settings.fadeInMs = sceneBoundaryTransitionEndSampleFadeInMs.load(std::memory_order_acquire);
                    settings.fadeOutMs = sceneBoundaryTransitionEndSampleFadeOutMs.load(std::memory_order_acquire);
                    settings.chokePrevious =
                        sceneBoundaryTransitionEndSampleChokePrevious.load(std::memory_order_acquire) != 0;
                    settings.reverse =
                        sceneBoundaryTransitionEndSampleReverse.load(std::memory_order_acquire) != 0;
                    settings.pitchSemitones =
                        sceneBoundaryTransitionEndSamplePitchSemitones.load(std::memory_order_acquire);
                    settings.lowpassHz = sceneBoundaryTransitionEndSampleLowpassHz.load(std::memory_order_acquire);
                    settings.highpassHz = sceneBoundaryTransitionEndSampleHighpassHz.load(std::memory_order_acquire);
                    settings.duckSource = sceneBoundaryTransitionEndSampleDuckSource.load(std::memory_order_acquire);
                    settings.duckAmount = sceneBoundaryTransitionEndSampleDuckAmount.load(std::memory_order_acquire);
                    settings = sanitizeRuntimeSceneTransitionEndSampleSettings(settings);
                    const float baseGain = juce::jlimit(0.0f, 1.35f, 0.55f + (0.55f * intensity));

                    if (settings.chokePrevious)
                    {
                        for (auto& activeVoice : sceneTransitionEndSampleVoices)
                            activeVoice = {};
                        sceneTransitionEndSamplePreviewVoice = {};
                    }

                    int voiceIndex = -1;
                    for (int i = 0; i < MaxSceneChainSteps; ++i)
                    {
                        if (!sceneTransitionEndSampleVoices[static_cast<size_t>(i)].active)
                        {
                            voiceIndex = i;
                            break;
                        }
                    }
                    if (voiceIndex < 0)
                        voiceIndex = 0;

                    configureVoice(sceneTransitionEndSampleVoices[static_cast<size_t>(voiceIndex)],
                                   std::move(sample),
                                   settings,
                                   baseGain,
                                   triggerOffset);
                }
            }
        }
    }

    for (auto& voice : sceneTransitionEndSampleVoices)
        renderVoice(voice);

    renderVoice(sceneTransitionEndSamplePreviewVoice);
}
