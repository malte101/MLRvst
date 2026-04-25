/*
  ==============================================================================

    SceneScheduler.cpp
    Scene timing, scheduling, and persistence helpers

  ==============================================================================
*/

#include "SceneScheduler.h"
#include "PluginProcessor.h"
#include "PresetStore.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace
{
using SceneLengthMode = MlrVSTAudioProcessor::SceneLengthMode;
using SceneRecallMode = MlrVSTAudioProcessor::SceneRecallMode;
using SceneChainTransitionType = MlrVSTAudioProcessor::SceneChainTransitionType;
using SceneChainTransitionOption = MlrVSTAudioProcessor::SceneChainTransitionOption;
using SceneChainTransitionScope = MlrVSTAudioProcessor::SceneChainTransitionScope;
using SceneChainTransitionContour = MlrVSTAudioProcessor::SceneChainTransitionContour;
using SceneChainTransitionCondition = MlrVSTAudioProcessor::SceneChainTransitionCondition;
using SceneChainState = MlrVSTAudioProcessor::SceneChainState;
using SceneChainStep = MlrVSTAudioProcessor::SceneChainStep;
using SceneChainTransitionFavorite = MlrVSTAudioProcessor::SceneChainTransitionFavorite;

void appendSceneDebugLog(const juce::String& message)
{
    const auto logFile = juce::File::getSpecialLocation(juce::File::tempDirectory)
        .getChildFile("mlrvst_scene_debug.log");
    juce::FileOutputStream out(logFile);
    if (!out.openedOk())
        return;

    out.setPosition(logFile.existsAsFile() ? logFile.getSize() : 0);
    const auto timestamp = juce::Time::getCurrentTime().formatted("%Y-%m-%d %H:%M:%S.%3Q");
    out.writeText(timestamp + " " + message + "\n", false, false, "\n");
    out.flush();
}

bool shouldPrepareSceneSwitchContexts(const MlrVSTAudioProcessor&)
{
    // Sequence-driven scene handoffs still benefit from a lightweight
    // ahead-of-time payload so the audio thread can switch the live engine at
    // the exact target sample without decoding files or constructing a second
    // scene engine.
    return true;
}

double scenePreloadLookaheadBeats(double currentTempo)
{
    constexpr double preloadLookaheadSeconds = 1.25;
    const double safeTempo = (std::isfinite(currentTempo) && currentTempo > 0.0)
        ? currentTempo
        : 120.0;
    return juce::jlimit(0.25,
                        8.0,
                        (preloadLookaheadSeconds * safeTempo) / 60.0);
}

SceneLengthMode sanitizeSceneLengthMode(int rawMode)
{
    juce::ignoreUnused(rawMode);
    return SceneLengthMode::ManualBars;
}

SceneRecallMode sanitizeSceneRecallMode(int rawMode)
{
    juce::ignoreUnused(rawMode);
    return SceneRecallMode::Manual;
}

void clearSceneChainStep(SceneChainStep& step)
{
    step.sceneSlot = -1;
    step.repeats = 1;
    step.transitionToNext = SceneChainTransitionType::None;
    step.transitionOption = SceneChainTransitionOption::Default;
    step.transitionScope = SceneChainTransitionScope::All;
    step.transitionContour = SceneChainTransitionContour::Smooth;
    step.transitionCondition = SceneChainTransitionCondition::Always;
    step.transitionLengthBeats = MlrVSTAudioProcessor::DefaultSceneTransitionLengthBeats;
    step.transitionSubtractsFromSceneLength = false;
    step.transitionIntensity = MlrVSTAudioProcessor::DefaultSceneTransitionIntensity;
    step.transitionDelayAmount = MlrVSTAudioProcessor::DefaultSceneTransitionDelayAmount;
    step.transitionFilterAmount = MlrVSTAudioProcessor::DefaultSceneTransitionFilterAmount;
    step.transitionChopAmount = MlrVSTAudioProcessor::DefaultSceneTransitionChopAmount;
    step.transitionEndSampleFile = juce::File();
    step.transitionEndSampleSettings = {};
}

juce::File normalizedSceneTransitionEndSampleDirectory(const juce::File& directory)
{
    const auto path = directory.getFullPathName().trim();
    if (path.isEmpty() || !juce::File::isAbsolutePath(path))
        return {};

    return directory;
}

juce::File inferSceneTransitionEndSampleDirectory(const SceneChainState& chainState,
                                                  const juce::File& fallbackDirectory)
{
    const auto normalizedFallback = normalizedSceneTransitionEndSampleDirectory(fallbackDirectory);
    if (normalizedFallback != juce::File())
        return normalizedFallback;

    for (const auto& step : chainState.steps)
    {
        const auto parentDirectory =
            normalizedSceneTransitionEndSampleDirectory(step.transitionEndSampleFile.getParentDirectory());
        if (parentDirectory != juce::File())
            return parentDirectory;
    }

    return {};
}

int sanitizeSceneChainSceneSlot(int rawSceneSlot)
{
    return rawSceneSlot < 0
        ? -1
        : juce::jlimit(0, MlrVSTAudioProcessor::SceneSlots - 1, rawSceneSlot);
}

int sanitizeSceneChainRepeats(int repeats)
{
    return juce::jlimit(1, MlrVSTAudioProcessor::MaxSceneRepeatCount, repeats);
}

SceneChainTransitionType sanitizeSceneChainTransitionType(SceneChainTransitionType type)
{
    return static_cast<SceneChainTransitionType>(juce::jlimit(
        0,
        static_cast<int>(SceneChainTransitionType::Return),
        static_cast<int>(type)));
}

SceneChainTransitionType sanitizeSceneChainTransitionType(int rawType)
{
    return sanitizeSceneChainTransitionType(static_cast<SceneChainTransitionType>(rawType));
}

SceneChainTransitionOption sanitizeSceneChainTransitionOption(SceneChainTransitionOption option)
{
    return static_cast<SceneChainTransitionOption>(juce::jlimit(
        0,
        static_cast<int>(SceneChainTransitionOption::Gate),
        static_cast<int>(option)));
}

SceneChainTransitionOption sanitizeSceneChainTransitionOption(int rawOption)
{
    return sanitizeSceneChainTransitionOption(static_cast<SceneChainTransitionOption>(rawOption));
}

SceneChainTransitionScope sanitizeSceneChainTransitionScope(SceneChainTransitionScope scope)
{
    return static_cast<SceneChainTransitionScope>(juce::jlimit(
        0,
        static_cast<int>(SceneChainTransitionScope::Flip),
        static_cast<int>(scope)));
}

SceneChainTransitionScope sanitizeSceneChainTransitionScope(int rawScope)
{
    return sanitizeSceneChainTransitionScope(static_cast<SceneChainTransitionScope>(rawScope));
}

SceneChainTransitionContour sanitizeSceneChainTransitionContour(SceneChainTransitionContour contour)
{
    return static_cast<SceneChainTransitionContour>(juce::jlimit(
        0,
        static_cast<int>(SceneChainTransitionContour::LateHit),
        static_cast<int>(contour)));
}

SceneChainTransitionContour sanitizeSceneChainTransitionContour(int rawContour)
{
    return sanitizeSceneChainTransitionContour(static_cast<SceneChainTransitionContour>(rawContour));
}

SceneChainTransitionCondition sanitizeSceneChainTransitionCondition(SceneChainTransitionCondition condition)
{
    return static_cast<SceneChainTransitionCondition>(juce::jlimit(
        0,
        static_cast<int>(SceneChainTransitionCondition::ForwardOnly),
        static_cast<int>(condition)));
}

SceneChainTransitionCondition sanitizeSceneChainTransitionCondition(int rawCondition)
{
    return sanitizeSceneChainTransitionCondition(static_cast<SceneChainTransitionCondition>(rawCondition));
}

float sanitizeSceneChainTransitionLengthBeats(float beats)
{
    if (!std::isfinite(beats))
        beats = MlrVSTAudioProcessor::DefaultSceneTransitionLengthBeats;
    return juce::jlimit(MlrVSTAudioProcessor::MinSceneTransitionLengthBeats,
                        MlrVSTAudioProcessor::MaxSceneTransitionLengthBeats,
                        beats);
}

float sanitizeSceneChainTransitionAmount(float amount, float fallback)
{
    if (!std::isfinite(amount))
        amount = fallback;
    return juce::jlimit(0.0f, 1.0f, amount);
}

MlrVSTAudioProcessor::SceneTransitionEndSampleSettings sanitizeSceneTransitionEndSampleSettings(
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

bool sceneTransitionEndSampleSettingsEqual(const MlrVSTAudioProcessor::SceneTransitionEndSampleSettings& a,
                                           const MlrVSTAudioProcessor::SceneTransitionEndSampleSettings& b)
{
    auto nearlyEqual = [](float lhs, float rhs)
    {
        return std::abs(lhs - rhs) <= 1.0e-4f;
    };

    return nearlyEqual(a.gainDb, b.gainDb)
        && nearlyEqual(a.fadeInMs, b.fadeInMs)
        && nearlyEqual(a.fadeOutMs, b.fadeOutMs)
        && a.chokePrevious == b.chokePrevious
        && a.reverse == b.reverse
        && nearlyEqual(a.pitchSemitones, b.pitchSemitones)
        && nearlyEqual(a.lowpassHz, b.lowpassHz)
        && nearlyEqual(a.highpassHz, b.highpassHz)
        && a.duckSource == b.duckSource
        && nearlyEqual(a.duckAmount, b.duckAmount);
}

void sanitizeSceneChainTransitionParameters(SceneChainStep& step)
{
    step.transitionScope = sanitizeSceneChainTransitionScope(step.transitionScope);
    step.transitionContour = sanitizeSceneChainTransitionContour(step.transitionContour);
    step.transitionCondition = sanitizeSceneChainTransitionCondition(step.transitionCondition);
    step.transitionLengthBeats = sanitizeSceneChainTransitionLengthBeats(step.transitionLengthBeats);
    step.transitionIntensity = sanitizeSceneChainTransitionAmount(
        step.transitionIntensity,
        MlrVSTAudioProcessor::DefaultSceneTransitionIntensity);
    step.transitionDelayAmount = sanitizeSceneChainTransitionAmount(
        step.transitionDelayAmount,
        MlrVSTAudioProcessor::DefaultSceneTransitionDelayAmount);
    step.transitionFilterAmount = sanitizeSceneChainTransitionAmount(
        step.transitionFilterAmount,
        MlrVSTAudioProcessor::DefaultSceneTransitionFilterAmount);
    step.transitionChopAmount = sanitizeSceneChainTransitionAmount(
        step.transitionChopAmount,
        MlrVSTAudioProcessor::DefaultSceneTransitionChopAmount);
    step.transitionEndSampleSettings = sanitizeSceneTransitionEndSampleSettings(step.transitionEndSampleSettings);
}

void sanitizeSceneChainTransitionFavorite(SceneChainTransitionFavorite& favorite)
{
    favorite.type = sanitizeSceneChainTransitionType(favorite.type);
    favorite.option = sanitizeSceneChainTransitionOption(favorite.option);
    favorite.scope = sanitizeSceneChainTransitionScope(favorite.scope);
    favorite.contour = sanitizeSceneChainTransitionContour(favorite.contour);
    favorite.condition = sanitizeSceneChainTransitionCondition(favorite.condition);
    favorite.lengthBeats = sanitizeSceneChainTransitionLengthBeats(favorite.lengthBeats);
    favorite.intensity = sanitizeSceneChainTransitionAmount(
        favorite.intensity,
        MlrVSTAudioProcessor::DefaultSceneTransitionIntensity);
    favorite.delayAmount = sanitizeSceneChainTransitionAmount(
        favorite.delayAmount,
        MlrVSTAudioProcessor::DefaultSceneTransitionDelayAmount);
    favorite.filterAmount = sanitizeSceneChainTransitionAmount(
        favorite.filterAmount,
        MlrVSTAudioProcessor::DefaultSceneTransitionFilterAmount);
    favorite.chopAmount = sanitizeSceneChainTransitionAmount(
        favorite.chopAmount,
        MlrVSTAudioProcessor::DefaultSceneTransitionChopAmount);
}

uint64_t sceneChainTransitionConditionSeed(int fromStepIndex, int toStepIndex, double targetPpq)
{
    const auto safeFrom = static_cast<uint64_t>(juce::jmax(0, fromStepIndex) + 1);
    const auto safeTo = static_cast<uint64_t>(juce::jmax(0, toStepIndex) + 1);
    uint64_t seed = safeFrom * 0x9e3779b97f4a7c15ULL;
    seed ^= safeTo * 0xbf58476d1ce4e5b9ULL;
    if (std::isfinite(targetPpq))
    {
        const auto ppqTicks = static_cast<uint64_t>(std::llround(juce::jmax(0.0, targetPpq) * 960.0));
        seed ^= ppqTicks + 0x94d049bb133111ebULL;
    }
    seed ^= (seed >> 30);
    seed *= 0xbf58476d1ce4e5b9ULL;
    seed ^= (seed >> 27);
    seed *= 0x94d049bb133111ebULL;
    seed ^= (seed >> 31);
    return seed;
}

bool sceneChainTransitionConditionAllows(SceneChainTransitionCondition condition,
                                         int fromStepIndex,
                                         int toStepIndex,
                                         double targetPpq,
                                         bool isLoopbackHandoff)
{
    switch (sanitizeSceneChainTransitionCondition(condition))
    {
        case SceneChainTransitionCondition::Chance50:
            return (sceneChainTransitionConditionSeed(fromStepIndex, toStepIndex, targetPpq)
                    & 0xffffffffULL) < 0x80000000ULL;
        case SceneChainTransitionCondition::Chance25:
            return (sceneChainTransitionConditionSeed(fromStepIndex, toStepIndex, targetPpq)
                    & 0xffffffffULL) < 0x40000000ULL;
        case SceneChainTransitionCondition::LoopOnly:
            return isLoopbackHandoff;
        case SceneChainTransitionCondition::ForwardOnly:
            return !isLoopbackHandoff;
        case SceneChainTransitionCondition::Always:
        default:
            return true;
    }
}

bool isValidSceneStoredSamplePath(const juce::File& file)
{
    const auto path = file.getFullPathName().trim();
    if (path.isEmpty() || path.length() > 4096)
        return false;
    if (!juce::File::isAbsolutePath(path))
        return false;
    if (path.contains("\n") || path.contains("\r") || path.contains("://"))
        return false;
    if (path.startsWith("//") || path.startsWith("\\\\"))
        return false;
    return file.hasFileExtension("wav;aif;aiff;mp3;ogg;flac");
}

bool canEmbedSceneAudioBuffer(const juce::AudioBuffer<float>& buffer)
{
    const int channels = buffer.getNumChannels();
    const int samples = buffer.getNumSamples();
    if (channels <= 0 || samples <= 0)
        return false;

    constexpr uint64_t kMaxEmbeddedWavBytes = 48ULL * 1024ULL * 1024ULL;
    const uint64_t estimatedWavBytes = static_cast<uint64_t>(channels)
        * static_cast<uint64_t>(samples)
        * 3ULL
        + 4096ULL;
    return estimatedWavBytes <= kMaxEmbeddedWavBytes;
}

juce::String sceneChainTransitionSummaryLabel(SceneChainTransitionType type)
{
    switch (sanitizeSceneChainTransitionType(type))
    {
        case SceneChainTransitionType::Fill:       return "Fill";
        case SceneChainTransitionType::Stutter:    return "Stutter";
        case SceneChainTransitionType::FilterRise: return "Rise";
        case SceneChainTransitionType::Drop:       return "Drop";
        case SceneChainTransitionType::MuteTail:   return "Mute";
        case SceneChainTransitionType::Break:      return "Break";
        case SceneChainTransitionType::Return:     return "Back";
        case SceneChainTransitionType::None:
        default:
            return {};
    }
}

double sceneChainTransitionLeadBeats(SceneChainTransitionType type,
                                     float lengthBeats)
{
    const double requestedLength = static_cast<double>(sanitizeSceneChainTransitionLengthBeats(lengthBeats));
    switch (sanitizeSceneChainTransitionType(type))
    {
        case SceneChainTransitionType::Fill:       return juce::jlimit(0.25, 8.0, requestedLength);
        case SceneChainTransitionType::Stutter:    return juce::jlimit(0.25, 6.0, requestedLength);
        case SceneChainTransitionType::FilterRise: return juce::jlimit(0.25, 8.0, requestedLength);
        case SceneChainTransitionType::Drop:       return juce::jlimit(0.25, 6.0, requestedLength);
        case SceneChainTransitionType::MuteTail:   return juce::jlimit(0.25, 4.0, requestedLength);
        case SceneChainTransitionType::Break:      return juce::jlimit(0.25, 8.0, requestedLength);
        case SceneChainTransitionType::None:
        case SceneChainTransitionType::Return:
        default:
            return 0.0;
    }
}

bool sceneChainStepHasScene(const SceneChainStep& step)
{
    return step.sceneSlot >= 0 && step.sceneSlot < MlrVSTAudioProcessor::SceneSlots;
}

void normalizeSceneChainState(SceneChainState& state)
{
    std::array<SceneChainStep, MlrVSTAudioProcessor::MaxSceneChainSteps> normalizedSteps{};
    for (auto& step : normalizedSteps)
        clearSceneChainStep(step);

    int writeIndex = 0;
    for (const auto& step : state.steps)
    {
        const int safeSceneSlot = sanitizeSceneChainSceneSlot(step.sceneSlot);
        if (safeSceneSlot < 0 || writeIndex >= MlrVSTAudioProcessor::MaxSceneChainSteps)
            continue;

        auto& normalized = normalizedSteps[static_cast<size_t>(writeIndex++)];
        normalized.sceneSlot = safeSceneSlot;
        normalized.repeats = sanitizeSceneChainRepeats(step.repeats);
        normalized.transitionToNext = sanitizeSceneChainTransitionType(step.transitionToNext);
        normalized.transitionOption = sanitizeSceneChainTransitionOption(step.transitionOption);
        normalized.transitionScope = sanitizeSceneChainTransitionScope(step.transitionScope);
        normalized.transitionContour = sanitizeSceneChainTransitionContour(step.transitionContour);
        normalized.transitionCondition = sanitizeSceneChainTransitionCondition(step.transitionCondition);
        normalized.transitionLengthBeats = step.transitionLengthBeats;
        normalized.transitionSubtractsFromSceneLength = step.transitionSubtractsFromSceneLength;
        normalized.transitionIntensity = step.transitionIntensity;
        normalized.transitionDelayAmount = step.transitionDelayAmount;
        normalized.transitionFilterAmount = step.transitionFilterAmount;
        normalized.transitionChopAmount = step.transitionChopAmount;
        sanitizeSceneChainTransitionParameters(normalized);
    }

    state.steps = normalizedSteps;
    if (writeIndex <= 0)
    {
        state.loopEnabled = false;
        state.loopStart = 0;
        state.loopEnd = 0;
        return;
    }

    state.loopEnabled = writeIndex >= 2;
    state.loopStart = 0;
    state.loopEnd = writeIndex - 1;
}

} // namespace

int getSceneChainLengthInternal(const MlrVSTAudioProcessor& processor)
{
    int length = 0;
    for (int stepIndex = 0; stepIndex < MlrVSTAudioProcessor::MaxSceneChainSteps; ++stepIndex)
    {
        if (sceneChainStepHasScene(processor.sceneChainState.steps[static_cast<size_t>(stepIndex)]))
            length = stepIndex + 1;
    }

    return length;
}

int resolveSceneChainNextStepIndex(const MlrVSTAudioProcessor& processor, int currentStepIndex)
{
    const int chainLength = getSceneChainLengthInternal(processor);
    if (chainLength <= 0)
        return -1;

    const int safeCurrentStep = juce::jlimit(0, chainLength - 1, currentStepIndex);
    if (processor.sceneChainState.loopEnabled
        && safeCurrentStep == juce::jlimit(0, chainLength - 1, processor.sceneChainState.loopEnd))
    {
        return juce::jlimit(0, chainLength - 1, processor.sceneChainState.loopStart);
    }

    const int nextStep = safeCurrentStep + 1;
    if (nextStep < chainLength)
        return nextStep;

    if (processor.sceneChainState.loopEnabled)
        return juce::jlimit(0, chainLength - 1, processor.sceneChainState.loopStart);

    return -1;
}

void sanitizeSceneChainRuntimeState(MlrVSTAudioProcessor& processor)
{
    normalizeSceneChainState(processor.sceneChainState);
    const int chainLength = getSceneChainLengthInternal(processor);
    if (chainLength <= 0)
    {
        processor.sceneSequenceActive = false;
        processor.sceneSequenceCurrentStepIndex = -1;
        processor.sceneSequenceStartPpqValid = false;
        processor.clearSceneBoundaryTransitionState();
        processor.clearSceneChainReturnOverride();
        if (processor.pendingSceneRecall.sequenceDriven)
        {
            processor.pendingSceneRecall.active = false;
            processor.pendingSceneRecall.sequenceDriven = false;
            processor.pendingSceneRecall.targetResolved = false;
            processor.pendingSceneRecall.sequenceStepIndex = -1;
        }
        MlrVSTAudioProcessor::SceneSwitchEvent pendingSwitchEvent;
        if (processor.peekPendingSceneApplyState(pendingSwitchEvent) && pendingSwitchEvent.sequenceDriven)
            processor.clearPendingSceneApplyState();
        return;
    }

    processor.sceneSequenceCurrentStepIndex = juce::jlimit(-1, chainLength - 1, processor.sceneSequenceCurrentStepIndex);
    if (chainLength < 2)
    {
        processor.sceneSequenceActive = false;
        processor.sceneSequenceStartPpqValid = false;
    }
}

void markSceneChainDefinitionChanged(MlrVSTAudioProcessor& processor)
{
    sanitizeSceneChainRuntimeState(processor);
    processor.clearSceneBoundaryTransitionState();
    processor.clearSceneChainReturnOverride();
    if (processor.sceneSequenceActive
        || (processor.pendingSceneRecall.active
            && SceneScheduler::getSceneRecallModeIndex(processor) != static_cast<int>(SceneRecallMode::QuantizeGrid)))
    {
        processor.pendingSceneRecall.targetResolved = false;
    }
    processor.markPersistentGlobalUserChange();
}

void SceneScheduler::markSceneChainTransitionEdited(MlrVSTAudioProcessor& processor, int editedStepIndex)
{
    sanitizeSceneChainRuntimeState(processor);

    const int safeEditedStep = juce::jlimit(0,
                                            MlrVSTAudioProcessor::MaxSceneChainSteps - 1,
                                            editedStepIndex);
    const int boundaryStep = processor.sceneBoundaryTransitionFromStep.load(std::memory_order_acquire);
    if (boundaryStep == safeEditedStep)
    {
        const auto updatedType = sanitizeSceneChainTransitionType(
            processor.sceneChainState.steps[static_cast<size_t>(safeEditedStep)].transitionToNext);
        if (updatedType == SceneChainTransitionType::None
            || updatedType == SceneChainTransitionType::Return)
        {
            processor.clearSceneBoundaryTransitionState();
        }
        else
        {
            const auto updatedOption = sanitizeSceneChainTransitionOption(
                processor.sceneChainState.steps[static_cast<size_t>(safeEditedStep)].transitionOption);
            const auto updatedScope = sanitizeSceneChainTransitionScope(
                processor.sceneChainState.steps[static_cast<size_t>(safeEditedStep)].transitionScope);
            const auto updatedContour = sanitizeSceneChainTransitionContour(
                processor.sceneChainState.steps[static_cast<size_t>(safeEditedStep)].transitionContour);
            processor.sceneBoundaryTransitionType.store(static_cast<int>(updatedType), std::memory_order_release);
            processor.sceneBoundaryTransitionOption.store(static_cast<int>(updatedOption), std::memory_order_release);
            processor.sceneBoundaryTransitionScope.store(static_cast<int>(updatedScope), std::memory_order_release);
            processor.sceneBoundaryTransitionContour.store(static_cast<int>(updatedContour), std::memory_order_release);
            processor.sceneBoundaryTransitionLengthBeats.store(
                sanitizeSceneChainTransitionLengthBeats(
                    processor.sceneChainState.steps[static_cast<size_t>(safeEditedStep)].transitionLengthBeats),
                std::memory_order_release);
            processor.sceneBoundaryTransitionIntensity.store(
                sanitizeSceneChainTransitionAmount(
                    processor.sceneChainState.steps[static_cast<size_t>(safeEditedStep)].transitionIntensity,
                    MlrVSTAudioProcessor::DefaultSceneTransitionIntensity),
                std::memory_order_release);
            processor.sceneBoundaryTransitionDelayAmount.store(
                sanitizeSceneChainTransitionAmount(
                    processor.sceneChainState.steps[static_cast<size_t>(safeEditedStep)].transitionDelayAmount,
                    MlrVSTAudioProcessor::DefaultSceneTransitionDelayAmount),
                std::memory_order_release);
            processor.sceneBoundaryTransitionFilterAmount.store(
                sanitizeSceneChainTransitionAmount(
                    processor.sceneChainState.steps[static_cast<size_t>(safeEditedStep)].transitionFilterAmount,
                    MlrVSTAudioProcessor::DefaultSceneTransitionFilterAmount),
                std::memory_order_release);
            processor.sceneBoundaryTransitionChopAmount.store(
                sanitizeSceneChainTransitionAmount(
                    processor.sceneChainState.steps[static_cast<size_t>(safeEditedStep)].transitionChopAmount,
                    MlrVSTAudioProcessor::DefaultSceneTransitionChopAmount),
                std::memory_order_release);
            const auto updatedEndSampleSettings = sanitizeSceneTransitionEndSampleSettings(
                processor.sceneChainState.steps[static_cast<size_t>(safeEditedStep)].transitionEndSampleSettings);
            processor.sceneBoundaryTransitionEndSampleGainDb.store(updatedEndSampleSettings.gainDb,
                                                                   std::memory_order_release);
            processor.sceneBoundaryTransitionEndSampleFadeInMs.store(updatedEndSampleSettings.fadeInMs,
                                                                     std::memory_order_release);
            processor.sceneBoundaryTransitionEndSampleFadeOutMs.store(updatedEndSampleSettings.fadeOutMs,
                                                                      std::memory_order_release);
            processor.sceneBoundaryTransitionEndSampleChokePrevious.store(updatedEndSampleSettings.chokePrevious ? 1 : 0,
                                                                          std::memory_order_release);
            processor.sceneBoundaryTransitionEndSampleReverse.store(updatedEndSampleSettings.reverse ? 1 : 0,
                                                                    std::memory_order_release);
            processor.sceneBoundaryTransitionEndSamplePitchSemitones.store(updatedEndSampleSettings.pitchSemitones,
                                                                           std::memory_order_release);
            processor.sceneBoundaryTransitionEndSampleLowpassHz.store(updatedEndSampleSettings.lowpassHz,
                                                                      std::memory_order_release);
            processor.sceneBoundaryTransitionEndSampleHighpassHz.store(updatedEndSampleSettings.highpassHz,
                                                                       std::memory_order_release);
            processor.sceneBoundaryTransitionEndSampleDuckSource.store(updatedEndSampleSettings.duckSource,
                                                                       std::memory_order_release);
            processor.sceneBoundaryTransitionEndSampleDuckAmount.store(updatedEndSampleSettings.duckAmount,
                                                                       std::memory_order_release);
        }
    }

    if (processor.sceneSequenceActive || processor.pendingSceneRecall.active)
        processor.pendingSceneRecall.targetResolved = false;

    processor.markPersistentGlobalUserChange();
}

namespace
{

struct ScopedSuspendProcessing
{
    explicit ScopedSuspendProcessing(MlrVSTAudioProcessor& p) : processor(p) { processor.suspendProcessing(true); }
    ~ScopedSuspendProcessing() { processor.suspendProcessing(false); }
    MlrVSTAudioProcessor& processor;
};

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
} // namespace

int SceneScheduler::getSceneRepeatCount(const MlrVSTAudioProcessor& processor, int sceneSlot)
{
    const auto idx = static_cast<size_t>(juce::jlimit(0, MlrVSTAudioProcessor::SceneSlots - 1, sceneSlot));
    return juce::jlimit(1, MlrVSTAudioProcessor::MaxSceneRepeatCount, processor.sceneRepeatCounts[idx]);
}

void SceneScheduler::setSceneRepeatCount(MlrVSTAudioProcessor& processor, int sceneSlot, int repeats)
{
    const int safeSceneSlot = juce::jlimit(0, MlrVSTAudioProcessor::SceneSlots - 1, sceneSlot);
    const auto idx = static_cast<size_t>(safeSceneSlot);
    const int clampedRepeats = juce::jlimit(1, MlrVSTAudioProcessor::MaxSceneRepeatCount, repeats);
    if (processor.sceneRepeatCounts[idx] == clampedRepeats)
        return;

    processor.sceneRepeatCounts[idx] = clampedRepeats;
    {
        const int mainPresetIndex = processor.getActiveMainPresetIndexForScenes();
        auto& storedState = processor.storedSceneSlotStates[idx];
        if (storedState.mainPresetIndex == juce::jlimit(0, MlrVSTAudioProcessor::MaxPresetSlots - 1, mainPresetIndex)
            && storedState.sceneSlot == safeSceneSlot
            && storedState.preparedSwitchPayloadTemplate != nullptr)
        {
            auto& timingState = storedState.preparedSwitchPayloadTemplate->sceneTimingState;
            timingState.valid = true;
            timingState.repeatCount = clampedRepeats;
            timingState.lengthModeIndex = getSceneLengthModeIndex(processor, safeSceneSlot);
            timingState.manualBars = getSceneManualBars(processor, safeSceneSlot);
            timingState.anchorStrip = getSceneAnchorStrip(processor, safeSceneSlot);
        }
    }
    processor.syncScenePerformanceClipLengthToResolvedLength(safeSceneSlot);
    if (safeSceneSlot == processor.activeSceneSlot)
        processor.queueActiveSceneAutosave();
    if (processor.sceneSequenceActive
        || (processor.pendingSceneRecall.active && getSceneRecallModeIndex(processor) != static_cast<int>(SceneRecallMode::QuantizeGrid)))
        processor.pendingSceneRecall.targetResolved = false;
}

int SceneScheduler::getSceneLengthModeIndex(const MlrVSTAudioProcessor& processor, int sceneSlot)
{
    const auto idx = static_cast<size_t>(juce::jlimit(0, MlrVSTAudioProcessor::SceneSlots - 1, sceneSlot));
    return static_cast<int>(sanitizeSceneLengthMode(processor.sceneLengthModes[idx]));
}

void SceneScheduler::setSceneLengthModeIndex(MlrVSTAudioProcessor& processor, int sceneSlot, int modeIndex)
{
    const auto idx = static_cast<size_t>(juce::jlimit(0, MlrVSTAudioProcessor::SceneSlots - 1, sceneSlot));
    const int safeSceneSlot = juce::jlimit(0, MlrVSTAudioProcessor::SceneSlots - 1, sceneSlot);
    const int clampedMode = static_cast<int>(sanitizeSceneLengthMode(modeIndex));
    if (processor.sceneLengthModes[idx] == clampedMode)
        return;

    processor.sceneLengthModes[idx] = clampedMode;
    {
        const int mainPresetIndex = processor.getActiveMainPresetIndexForScenes();
        auto& storedState = processor.storedSceneSlotStates[idx];
        if (storedState.mainPresetIndex == juce::jlimit(0, MlrVSTAudioProcessor::MaxPresetSlots - 1, mainPresetIndex)
            && storedState.sceneSlot == safeSceneSlot
            && storedState.preparedSwitchPayloadTemplate != nullptr)
        {
            auto& timingState = storedState.preparedSwitchPayloadTemplate->sceneTimingState;
            timingState.valid = true;
            timingState.repeatCount = getSceneRepeatCount(processor, safeSceneSlot);
            timingState.lengthModeIndex = clampedMode;
            timingState.manualBars = getSceneManualBars(processor, safeSceneSlot);
            timingState.anchorStrip = getSceneAnchorStrip(processor, safeSceneSlot);
        }
    }
    processor.syncScenePerformanceClipLengthToResolvedLength(safeSceneSlot);
    if (safeSceneSlot == processor.activeSceneSlot)
        processor.queueActiveSceneAutosave();
    if (processor.sceneSequenceActive
        || (processor.pendingSceneRecall.active && getSceneRecallModeIndex(processor) != static_cast<int>(SceneRecallMode::QuantizeGrid)))
        processor.pendingSceneRecall.targetResolved = false;
}

int SceneScheduler::getSceneRecallModeIndex(const MlrVSTAudioProcessor& processor)
{
    const int rawMode = processor.sceneRecallModeParam != nullptr
        ? static_cast<int>(processor.sceneRecallModeParam->load(std::memory_order_acquire))
        : static_cast<int>(SceneRecallMode::Manual);
    return static_cast<int>(sanitizeSceneRecallMode(rawMode));
}

void SceneScheduler::setSceneRecallModeIndex(MlrVSTAudioProcessor& processor, int modeIndex)
{
    const int clampedMode = static_cast<int>(sanitizeSceneRecallMode(modeIndex));
    bool changed = false;
    if (auto* parameter = processor.parameters.getParameter("sceneRecallMode"))
    {
        const float currentNormalized = parameter->getValue();
        const float targetNormalized = parameter->convertTo0to1(static_cast<float>(clampedMode));
        if (std::abs(currentNormalized - targetNormalized) > 1.0e-6f)
        {
            parameter->setValueNotifyingHost(targetNormalized);
            changed = true;
        }
    }

    if (processor.pendingSceneRecall.active)
        processor.pendingSceneRecall.targetResolved = false;

    if (changed)
        processor.markPersistentGlobalUserChange();
}

int SceneScheduler::getSceneManualBars(const MlrVSTAudioProcessor& processor, int sceneSlot)
{
    const auto idx = static_cast<size_t>(juce::jlimit(0, MlrVSTAudioProcessor::SceneSlots - 1, sceneSlot));
    return juce::jlimit(1, MlrVSTAudioProcessor::MaxSceneManualBars, processor.sceneManualBars[idx]);
}

void SceneScheduler::setSceneManualBars(MlrVSTAudioProcessor& processor, int sceneSlot, int bars)
{
    const auto idx = static_cast<size_t>(juce::jlimit(0, MlrVSTAudioProcessor::SceneSlots - 1, sceneSlot));
    const int safeSceneSlot = juce::jlimit(0, MlrVSTAudioProcessor::SceneSlots - 1, sceneSlot);
    const int clampedBars = juce::jlimit(1, MlrVSTAudioProcessor::MaxSceneManualBars, bars);
    if (processor.sceneManualBars[idx] == clampedBars)
        return;

    processor.sceneManualBars[idx] = clampedBars;
    {
        const int mainPresetIndex = processor.getActiveMainPresetIndexForScenes();
        auto& storedState = processor.storedSceneSlotStates[idx];
        if (storedState.mainPresetIndex == juce::jlimit(0, MlrVSTAudioProcessor::MaxPresetSlots - 1, mainPresetIndex)
            && storedState.sceneSlot == safeSceneSlot
            && storedState.preparedSwitchPayloadTemplate != nullptr)
        {
            auto& timingState = storedState.preparedSwitchPayloadTemplate->sceneTimingState;
            timingState.valid = true;
            timingState.repeatCount = getSceneRepeatCount(processor, safeSceneSlot);
            timingState.lengthModeIndex = getSceneLengthModeIndex(processor, safeSceneSlot);
            timingState.manualBars = clampedBars;
            timingState.anchorStrip = getSceneAnchorStrip(processor, safeSceneSlot);
        }
    }
    processor.syncScenePerformanceClipLengthToResolvedLength(safeSceneSlot);
    if (safeSceneSlot == processor.activeSceneSlot)
        processor.queueActiveSceneAutosave();
    if (processor.sceneSequenceActive
        || (processor.pendingSceneRecall.active && getSceneRecallModeIndex(processor) != static_cast<int>(SceneRecallMode::QuantizeGrid)))
        processor.pendingSceneRecall.targetResolved = false;
}

int SceneScheduler::getSceneAnchorStrip(const MlrVSTAudioProcessor& processor, int sceneSlot)
{
    const auto idx = static_cast<size_t>(juce::jlimit(0, MlrVSTAudioProcessor::SceneSlots - 1, sceneSlot));
    return juce::jlimit(0, MlrVSTAudioProcessor::MaxStrips - 1, processor.sceneAnchorStrips[idx]);
}

void SceneScheduler::setSceneAnchorStrip(MlrVSTAudioProcessor& processor, int sceneSlot, int stripIndex)
{
    const auto idx = static_cast<size_t>(juce::jlimit(0, MlrVSTAudioProcessor::SceneSlots - 1, sceneSlot));
    const int safeSceneSlot = juce::jlimit(0, MlrVSTAudioProcessor::SceneSlots - 1, sceneSlot);
    const int clampedStrip = juce::jlimit(0, MlrVSTAudioProcessor::MaxStrips - 1, stripIndex);
    if (processor.sceneAnchorStrips[idx] == clampedStrip)
        return;

    processor.sceneAnchorStrips[idx] = clampedStrip;
    {
        const int mainPresetIndex = processor.getActiveMainPresetIndexForScenes();
        auto& storedState = processor.storedSceneSlotStates[idx];
        if (storedState.mainPresetIndex == juce::jlimit(0, MlrVSTAudioProcessor::MaxPresetSlots - 1, mainPresetIndex)
            && storedState.sceneSlot == safeSceneSlot
            && storedState.preparedSwitchPayloadTemplate != nullptr)
        {
            auto& timingState = storedState.preparedSwitchPayloadTemplate->sceneTimingState;
            timingState.valid = true;
            timingState.repeatCount = getSceneRepeatCount(processor, safeSceneSlot);
            timingState.lengthModeIndex = getSceneLengthModeIndex(processor, safeSceneSlot);
            timingState.manualBars = getSceneManualBars(processor, safeSceneSlot);
            timingState.anchorStrip = clampedStrip;
        }
    }
    processor.syncScenePerformanceClipLengthToResolvedLength(safeSceneSlot);
    if (safeSceneSlot == processor.activeSceneSlot)
        processor.queueActiveSceneAutosave();
    if (processor.sceneSequenceActive
        || (processor.pendingSceneRecall.active && getSceneRecallModeIndex(processor) != static_cast<int>(SceneRecallMode::QuantizeGrid)))
        processor.pendingSceneRecall.targetResolved = false;
}

int SceneScheduler::getSceneLengthCount(const MlrVSTAudioProcessor& processor, int sceneSlot)
{
    return getSceneManualBars(processor, sceneSlot);
}

void SceneScheduler::setSceneLengthCount(MlrVSTAudioProcessor& processor, int sceneSlot, int count)
{
    setSceneManualBars(processor, sceneSlot, count);
    setSceneRepeatCount(processor, sceneSlot, 1);
}

double SceneScheduler::computeStripSceneSequenceLengthBeats(const MlrVSTAudioProcessor& processor, int stripIndex)
{
    if (processor.audioEngine == nullptr || stripIndex < 0 || stripIndex >= MlrVSTAudioProcessor::MaxStrips)
        return 0.0;

    auto* strip = processor.audioEngine->getStrip(stripIndex);
    if (strip == nullptr)
        return 0.0;

    const auto playMode = strip->getPlayMode();
    if (playMode == EnhancedAudioStrip::PlayMode::Sample)
        return 0.0;

    const bool hasStripAudio = (playMode == EnhancedAudioStrip::PlayMode::Sample)
        ? processor.hasSampleModeAudio(stripIndex)
        : strip->hasAudio();

    if (playMode == EnhancedAudioStrip::PlayMode::Step)
    {
        const int totalSteps = strip->getStepPatternLengthSteps();
        const bool hasEnabledSteps = std::any_of(
            strip->stepPattern.begin(),
            strip->stepPattern.begin() + totalSteps,
            [](bool enabled) { return enabled; });
        if (!hasStripAudio && !hasEnabledSteps)
            return 0.0;

        return juce::jlimit(1.0, 256.0, static_cast<double>(strip->getStepPatternBars()) * 4.0);
    }

    if (!hasStripAudio)
        return 0.0;

    double beats = static_cast<double>(strip->getBeatsPerLoop());
    if (!std::isfinite(beats) || beats <= 0.0)
        beats = static_cast<double>(juce::jmax(1, strip->getRecordingBars()) * 4);

    return juce::jlimit(0.25, 256.0, beats);
}

double SceneScheduler::computeLongestStripSceneSequenceLengthBeats(const MlrVSTAudioProcessor& processor)
{
    double longestBeats = 0.0;

    if (processor.audioEngine != nullptr)
        for (int stripIndex = 0; stripIndex < MlrVSTAudioProcessor::MaxStrips; ++stripIndex)
            longestBeats = juce::jmax(longestBeats, computeStripSceneSequenceLengthBeats(processor, stripIndex));
    return longestBeats;
}

double SceneScheduler::computeLongestPatternSceneSequenceLengthBeats(const MlrVSTAudioProcessor& processor)
{
    double longestBeats = 0.0;

    if (processor.audioEngine != nullptr)
    {
        for (int patternIndex = 0; patternIndex < ModernAudioEngine::MaxPatterns; ++patternIndex)
        {
            auto* pattern = processor.audioEngine->getPattern(patternIndex);
            if (pattern == nullptr)
                continue;
            if (pattern->getEventCount() <= 0 && !pattern->isPlaying() && !pattern->isRecording())
                continue;

            longestBeats = juce::jmax(
                longestBeats,
                juce::jlimit(1.0, 256.0, static_cast<double>(pattern->getLengthInBeats())));
        }
    }

    return longestBeats;
}

double SceneScheduler::getResolvedSceneLengthBeats(const MlrVSTAudioProcessor& processor, int sceneSlot)
{
    const int clampedSlot = juce::jlimit(0, MlrVSTAudioProcessor::SceneSlots - 1, sceneSlot);
    const double manualBeats = static_cast<double>(getSceneManualBars(processor, clampedSlot)) * 4.0;
    double resolvedBeats = manualBeats;

    switch (sanitizeSceneLengthMode(getSceneLengthModeIndex(processor, clampedSlot)))
    {
        case SceneLengthMode::LongestStrip:
            resolvedBeats = computeLongestStripSceneSequenceLengthBeats(processor);
            break;
        case SceneLengthMode::LongestPattern:
            resolvedBeats = computeLongestPatternSceneSequenceLengthBeats(processor);
            if (!std::isfinite(resolvedBeats) || resolvedBeats <= 0.0)
                resolvedBeats = computeLongestStripSceneSequenceLengthBeats(processor);
            break;
        case SceneLengthMode::AnchorStrip:
            resolvedBeats = computeStripSceneSequenceLengthBeats(processor, getSceneAnchorStrip(processor, clampedSlot));
            if (!std::isfinite(resolvedBeats) || resolvedBeats <= 0.0)
                resolvedBeats = computeLongestStripSceneSequenceLengthBeats(processor);
            break;
        case SceneLengthMode::ManualBars:
        default:
            break;
    }

    if (!std::isfinite(resolvedBeats) || resolvedBeats <= 0.0)
        resolvedBeats = manualBeats;

    return juce::jlimit(0.25, 4096.0, resolvedBeats);
}

double SceneScheduler::getSceneAdvanceLengthBeats(const MlrVSTAudioProcessor& processor, int sceneSlot)
{
    const double resolvedBeats = getResolvedSceneLengthBeats(processor, sceneSlot);
    const double repeats = static_cast<double>(getSceneRepeatCount(processor, sceneSlot));
    return juce::jlimit(0.25, 4096.0, resolvedBeats * repeats);
}

bool SceneScheduler::persistSceneTimingForSlot(MlrVSTAudioProcessor& processor, int sceneSlot)
{
    const int clampedSlot = juce::jlimit(0, MlrVSTAudioProcessor::SceneSlots - 1, sceneSlot);
    const int mainPresetIndex = processor.getActiveMainPresetIndexForScenes();
    if (!processor.hasStoredSceneSlotState(mainPresetIndex, clampedSlot))
        return false;

    juce::ignoreUnused(processor, clampedSlot, mainPresetIndex);

    // Scene timing edits should remain runtime-only until the explicit preset save
    // path serializes the current SceneChainState back into the preset XML.
    return true;
}

int SceneScheduler::getSceneChainLength(const MlrVSTAudioProcessor& processor)
{
    return getSceneChainLengthInternal(processor);
}

int SceneScheduler::getSceneChainStepSceneSlot(const MlrVSTAudioProcessor& processor, int stepIndex)
{
    const int safeStepIndex = juce::jlimit(0, MlrVSTAudioProcessor::MaxSceneChainSteps - 1, stepIndex);
    return sanitizeSceneChainSceneSlot(processor.sceneChainState.steps[static_cast<size_t>(safeStepIndex)].sceneSlot);
}

int SceneScheduler::getSceneChainStepRepeatCount(const MlrVSTAudioProcessor& processor, int stepIndex)
{
    const int safeStepIndex = juce::jlimit(0, MlrVSTAudioProcessor::MaxSceneChainSteps - 1, stepIndex);
    return sanitizeSceneChainRepeats(processor.sceneChainState.steps[static_cast<size_t>(safeStepIndex)].repeats);
}

int SceneScheduler::getSceneChainStepTransitionTypeIndex(const MlrVSTAudioProcessor& processor, int stepIndex)
{
    const int safeStepIndex = juce::jlimit(0, MlrVSTAudioProcessor::MaxSceneChainSteps - 1, stepIndex);
    return static_cast<int>(sanitizeSceneChainTransitionType(
        processor.sceneChainState.steps[static_cast<size_t>(safeStepIndex)].transitionToNext));
}

int SceneScheduler::getSceneChainStepTransitionOptionIndex(const MlrVSTAudioProcessor& processor, int stepIndex)
{
    const int safeStepIndex = juce::jlimit(0, MlrVSTAudioProcessor::MaxSceneChainSteps - 1, stepIndex);
    return static_cast<int>(sanitizeSceneChainTransitionOption(
        processor.sceneChainState.steps[static_cast<size_t>(safeStepIndex)].transitionOption));
}

int SceneScheduler::getSceneChainStepTransitionScopeIndex(const MlrVSTAudioProcessor& processor, int stepIndex)
{
    const int safeStepIndex = juce::jlimit(0, MlrVSTAudioProcessor::MaxSceneChainSteps - 1, stepIndex);
    return static_cast<int>(sanitizeSceneChainTransitionScope(
        processor.sceneChainState.steps[static_cast<size_t>(safeStepIndex)].transitionScope));
}

int SceneScheduler::getSceneChainStepTransitionContourIndex(const MlrVSTAudioProcessor& processor, int stepIndex)
{
    const int safeStepIndex = juce::jlimit(0, MlrVSTAudioProcessor::MaxSceneChainSteps - 1, stepIndex);
    return static_cast<int>(sanitizeSceneChainTransitionContour(
        processor.sceneChainState.steps[static_cast<size_t>(safeStepIndex)].transitionContour));
}

int SceneScheduler::getSceneChainStepTransitionConditionIndex(const MlrVSTAudioProcessor& processor, int stepIndex)
{
    const int safeStepIndex = juce::jlimit(0, MlrVSTAudioProcessor::MaxSceneChainSteps - 1, stepIndex);
    return static_cast<int>(sanitizeSceneChainTransitionCondition(
        processor.sceneChainState.steps[static_cast<size_t>(safeStepIndex)].transitionCondition));
}

float SceneScheduler::getSceneChainStepTransitionLengthBeats(const MlrVSTAudioProcessor& processor, int stepIndex)
{
    const int safeStepIndex = juce::jlimit(0, MlrVSTAudioProcessor::MaxSceneChainSteps - 1, stepIndex);
    return sanitizeSceneChainTransitionLengthBeats(
        processor.sceneChainState.steps[static_cast<size_t>(safeStepIndex)].transitionLengthBeats);
}

bool SceneScheduler::getSceneChainStepTransitionSubtractsFromSceneLength(const MlrVSTAudioProcessor& processor,
                                                                         int stepIndex)
{
    const int safeStepIndex = juce::jlimit(0, MlrVSTAudioProcessor::MaxSceneChainSteps - 1, stepIndex);
    return processor.sceneChainState.steps[static_cast<size_t>(safeStepIndex)].transitionSubtractsFromSceneLength;
}

float SceneScheduler::getSceneChainStepTransitionIntensity(const MlrVSTAudioProcessor& processor, int stepIndex)
{
    const int safeStepIndex = juce::jlimit(0, MlrVSTAudioProcessor::MaxSceneChainSteps - 1, stepIndex);
    return sanitizeSceneChainTransitionAmount(
        processor.sceneChainState.steps[static_cast<size_t>(safeStepIndex)].transitionIntensity,
        MlrVSTAudioProcessor::DefaultSceneTransitionIntensity);
}

float SceneScheduler::getSceneChainStepTransitionDelayAmount(const MlrVSTAudioProcessor& processor, int stepIndex)
{
    const int safeStepIndex = juce::jlimit(0, MlrVSTAudioProcessor::MaxSceneChainSteps - 1, stepIndex);
    return sanitizeSceneChainTransitionAmount(
        processor.sceneChainState.steps[static_cast<size_t>(safeStepIndex)].transitionDelayAmount,
        MlrVSTAudioProcessor::DefaultSceneTransitionDelayAmount);
}

float SceneScheduler::getSceneChainStepTransitionFilterAmount(const MlrVSTAudioProcessor& processor, int stepIndex)
{
    const int safeStepIndex = juce::jlimit(0, MlrVSTAudioProcessor::MaxSceneChainSteps - 1, stepIndex);
    return sanitizeSceneChainTransitionAmount(
        processor.sceneChainState.steps[static_cast<size_t>(safeStepIndex)].transitionFilterAmount,
        MlrVSTAudioProcessor::DefaultSceneTransitionFilterAmount);
}

float SceneScheduler::getSceneChainStepTransitionChopAmount(const MlrVSTAudioProcessor& processor, int stepIndex)
{
    const int safeStepIndex = juce::jlimit(0, MlrVSTAudioProcessor::MaxSceneChainSteps - 1, stepIndex);
    return sanitizeSceneChainTransitionAmount(
        processor.sceneChainState.steps[static_cast<size_t>(safeStepIndex)].transitionChopAmount,
        MlrVSTAudioProcessor::DefaultSceneTransitionChopAmount);
}

juce::File SceneScheduler::getSceneTransitionEndSampleDirectory(const MlrVSTAudioProcessor& processor)
{
    return processor.sceneChainState.transitionEndSampleDirectory;
}

juce::File SceneScheduler::getSceneChainStepTransitionEndSampleFile(const MlrVSTAudioProcessor& processor,
                                                                    int stepIndex)
{
    const int safeStepIndex = juce::jlimit(0, MlrVSTAudioProcessor::MaxSceneChainSteps - 1, stepIndex);
    return processor.sceneChainState.steps[static_cast<size_t>(safeStepIndex)].transitionEndSampleFile;
}

MlrVSTAudioProcessor::SceneTransitionEndSampleSettings
SceneScheduler::getSceneChainStepTransitionEndSampleSettings(const MlrVSTAudioProcessor& processor,
                                                             int stepIndex)
{
    const int safeStepIndex = juce::jlimit(0, MlrVSTAudioProcessor::MaxSceneChainSteps - 1, stepIndex);
    return sanitizeSceneTransitionEndSampleSettings(
        processor.sceneChainState.steps[static_cast<size_t>(safeStepIndex)].transitionEndSampleSettings);
}

void SceneScheduler::setSceneChainStep(MlrVSTAudioProcessor& processor, int stepIndex, int sceneSlot, int repeats)
{
    const int safeStepIndex = juce::jlimit(0, MlrVSTAudioProcessor::MaxSceneChainSteps - 1, stepIndex);
    auto& step = processor.sceneChainState.steps[static_cast<size_t>(safeStepIndex)];
    step.sceneSlot = sanitizeSceneChainSceneSlot(sceneSlot);
    step.repeats = sanitizeSceneChainRepeats(repeats);
    markSceneChainDefinitionChanged(processor);
}

void SceneScheduler::setSceneChainStepTransitionTypeIndex(MlrVSTAudioProcessor& processor,
                                                          int stepIndex,
                                                          int typeIndex)
{
    const int safeStepIndex = juce::jlimit(0, MlrVSTAudioProcessor::MaxSceneChainSteps - 1, stepIndex);
    const auto safeType = sanitizeSceneChainTransitionType(typeIndex);
    auto& step = processor.sceneChainState.steps[static_cast<size_t>(safeStepIndex)];
    if (step.transitionToNext == safeType)
        return;

    step.transitionToNext = safeType;
    markSceneChainTransitionEdited(processor, safeStepIndex);
}

void SceneScheduler::setSceneChainStepTransitionOptionIndex(MlrVSTAudioProcessor& processor,
                                                            int stepIndex,
                                                            int optionIndex)
{
    const int safeStepIndex = juce::jlimit(0, MlrVSTAudioProcessor::MaxSceneChainSteps - 1, stepIndex);
    const auto safeOption = sanitizeSceneChainTransitionOption(optionIndex);
    auto& step = processor.sceneChainState.steps[static_cast<size_t>(safeStepIndex)];
    if (step.transitionOption == safeOption)
        return;

    step.transitionOption = safeOption;
    markSceneChainTransitionEdited(processor, safeStepIndex);
}

void SceneScheduler::setSceneChainStepTransitionScopeIndex(MlrVSTAudioProcessor& processor,
                                                           int stepIndex,
                                                           int scopeIndex)
{
    const int safeStepIndex = juce::jlimit(0, MlrVSTAudioProcessor::MaxSceneChainSteps - 1, stepIndex);
    const auto safeScope = sanitizeSceneChainTransitionScope(scopeIndex);
    auto& step = processor.sceneChainState.steps[static_cast<size_t>(safeStepIndex)];
    if (step.transitionScope == safeScope)
        return;

    step.transitionScope = safeScope;
    markSceneChainTransitionEdited(processor, safeStepIndex);
}

void SceneScheduler::setSceneChainStepTransitionContourIndex(MlrVSTAudioProcessor& processor,
                                                             int stepIndex,
                                                             int contourIndex)
{
    const int safeStepIndex = juce::jlimit(0, MlrVSTAudioProcessor::MaxSceneChainSteps - 1, stepIndex);
    const auto safeContour = sanitizeSceneChainTransitionContour(contourIndex);
    auto& step = processor.sceneChainState.steps[static_cast<size_t>(safeStepIndex)];
    if (step.transitionContour == safeContour)
        return;

    step.transitionContour = safeContour;
    markSceneChainTransitionEdited(processor, safeStepIndex);
}

void SceneScheduler::setSceneChainStepTransitionConditionIndex(MlrVSTAudioProcessor& processor,
                                                               int stepIndex,
                                                               int conditionIndex)
{
    const int safeStepIndex = juce::jlimit(0, MlrVSTAudioProcessor::MaxSceneChainSteps - 1, stepIndex);
    const auto safeCondition = sanitizeSceneChainTransitionCondition(conditionIndex);
    auto& step = processor.sceneChainState.steps[static_cast<size_t>(safeStepIndex)];
    if (step.transitionCondition == safeCondition)
        return;

    step.transitionCondition = safeCondition;
    markSceneChainTransitionEdited(processor, safeStepIndex);
}

void SceneScheduler::setSceneChainStepTransitionLengthBeats(MlrVSTAudioProcessor& processor,
                                                            int stepIndex,
                                                            float beats)
{
    const int safeStepIndex = juce::jlimit(0, MlrVSTAudioProcessor::MaxSceneChainSteps - 1, stepIndex);
    auto& step = processor.sceneChainState.steps[static_cast<size_t>(safeStepIndex)];
    const float safeBeats = sanitizeSceneChainTransitionLengthBeats(beats);
    if (std::abs(step.transitionLengthBeats - safeBeats) <= 1.0e-4f)
        return;

    step.transitionLengthBeats = safeBeats;
    markSceneChainTransitionEdited(processor, safeStepIndex);
}

void SceneScheduler::setSceneChainStepTransitionSubtractsFromSceneLength(MlrVSTAudioProcessor& processor,
                                                                         int stepIndex,
                                                                         bool enabled)
{
    const int safeStepIndex = juce::jlimit(0, MlrVSTAudioProcessor::MaxSceneChainSteps - 1, stepIndex);
    auto& step = processor.sceneChainState.steps[static_cast<size_t>(safeStepIndex)];
    if (step.transitionSubtractsFromSceneLength == enabled)
        return;

    step.transitionSubtractsFromSceneLength = enabled;
    markSceneChainTransitionEdited(processor, safeStepIndex);
}

void SceneScheduler::setSceneChainStepTransitionIntensity(MlrVSTAudioProcessor& processor,
                                                          int stepIndex,
                                                          float amount)
{
    const int safeStepIndex = juce::jlimit(0, MlrVSTAudioProcessor::MaxSceneChainSteps - 1, stepIndex);
    auto& step = processor.sceneChainState.steps[static_cast<size_t>(safeStepIndex)];
    const float safeAmount = sanitizeSceneChainTransitionAmount(
        amount,
        MlrVSTAudioProcessor::DefaultSceneTransitionIntensity);
    if (std::abs(step.transitionIntensity - safeAmount) <= 1.0e-4f)
        return;

    step.transitionIntensity = safeAmount;
    markSceneChainTransitionEdited(processor, safeStepIndex);
}

void SceneScheduler::setSceneChainStepTransitionDelayAmount(MlrVSTAudioProcessor& processor,
                                                            int stepIndex,
                                                            float amount)
{
    const int safeStepIndex = juce::jlimit(0, MlrVSTAudioProcessor::MaxSceneChainSteps - 1, stepIndex);
    auto& step = processor.sceneChainState.steps[static_cast<size_t>(safeStepIndex)];
    const float safeAmount = sanitizeSceneChainTransitionAmount(
        amount,
        MlrVSTAudioProcessor::DefaultSceneTransitionDelayAmount);
    if (std::abs(step.transitionDelayAmount - safeAmount) <= 1.0e-4f)
        return;

    step.transitionDelayAmount = safeAmount;
    markSceneChainTransitionEdited(processor, safeStepIndex);
}

void SceneScheduler::setSceneChainStepTransitionFilterAmount(MlrVSTAudioProcessor& processor,
                                                             int stepIndex,
                                                             float amount)
{
    const int safeStepIndex = juce::jlimit(0, MlrVSTAudioProcessor::MaxSceneChainSteps - 1, stepIndex);
    auto& step = processor.sceneChainState.steps[static_cast<size_t>(safeStepIndex)];
    const float safeAmount = sanitizeSceneChainTransitionAmount(
        amount,
        MlrVSTAudioProcessor::DefaultSceneTransitionFilterAmount);
    if (std::abs(step.transitionFilterAmount - safeAmount) <= 1.0e-4f)
        return;

    step.transitionFilterAmount = safeAmount;
    markSceneChainTransitionEdited(processor, safeStepIndex);
}

void SceneScheduler::setSceneChainStepTransitionChopAmount(MlrVSTAudioProcessor& processor,
                                                           int stepIndex,
                                                           float amount)
{
    const int safeStepIndex = juce::jlimit(0, MlrVSTAudioProcessor::MaxSceneChainSteps - 1, stepIndex);
    auto& step = processor.sceneChainState.steps[static_cast<size_t>(safeStepIndex)];
    const float safeAmount = sanitizeSceneChainTransitionAmount(
        amount,
        MlrVSTAudioProcessor::DefaultSceneTransitionChopAmount);
    if (std::abs(step.transitionChopAmount - safeAmount) <= 1.0e-4f)
        return;

    step.transitionChopAmount = safeAmount;
    markSceneChainTransitionEdited(processor, safeStepIndex);
}

void SceneScheduler::setSceneTransitionEndSampleDirectory(MlrVSTAudioProcessor& processor, const juce::File& directory)
{
    const juce::File normalized = directory.getFullPathName().trim().isEmpty() ? juce::File() : directory;
    if (processor.sceneChainState.transitionEndSampleDirectory == normalized)
        return;

    processor.sceneChainState.transitionEndSampleDirectory = normalized;
    processor.markPersistentGlobalUserChange();
}

void SceneScheduler::setSceneChainStepTransitionEndSampleFile(MlrVSTAudioProcessor& processor,
                                                              int stepIndex,
                                                              const juce::File& file)
{
    const int safeStepIndex = juce::jlimit(0, MlrVSTAudioProcessor::MaxSceneChainSteps - 1, stepIndex);
    auto& step = processor.sceneChainState.steps[static_cast<size_t>(safeStepIndex)];
    const juce::File normalized = file.getFullPathName().trim().isEmpty() ? juce::File() : file;
    if (step.transitionEndSampleFile == normalized)
        return;

    step.transitionEndSampleFile = normalized;
    markSceneChainTransitionEdited(processor, safeStepIndex);
}

void SceneScheduler::setSceneChainStepTransitionEndSampleSettings(
    MlrVSTAudioProcessor& processor,
    int stepIndex,
    const MlrVSTAudioProcessor::SceneTransitionEndSampleSettings& settings)
{
    const int safeStepIndex = juce::jlimit(0, MlrVSTAudioProcessor::MaxSceneChainSteps - 1, stepIndex);
    auto& step = processor.sceneChainState.steps[static_cast<size_t>(safeStepIndex)];
    const auto sanitized = sanitizeSceneTransitionEndSampleSettings(settings);
    if (sceneTransitionEndSampleSettingsEqual(step.transitionEndSampleSettings, sanitized))
        return;

    step.transitionEndSampleSettings = sanitized;
    markSceneChainTransitionEdited(processor, safeStepIndex);
}

void SceneScheduler::clearSceneChain(MlrVSTAudioProcessor& processor)
{
    for (auto& step : processor.sceneChainState.steps)
        clearSceneChainStep(step);
    processor.sceneChainState.loopEnabled = false;
    processor.sceneChainState.loopStart = 0;
    processor.sceneChainState.loopEnd = 0;
    markSceneChainDefinitionChanged(processor);
}

bool SceneScheduler::isSceneChainLoopEnabled(const MlrVSTAudioProcessor& processor)
{
    return getSceneChainLengthInternal(processor) >= 2;
}

void SceneScheduler::setSceneChainLoopEnabled(MlrVSTAudioProcessor& processor, bool enabled)
{
    juce::ignoreUnused(enabled);
    const int chainLength = getSceneChainLengthInternal(processor);
    processor.sceneChainState.loopEnabled = chainLength >= 2;
    processor.sceneChainState.loopStart = 0;
    processor.sceneChainState.loopEnd = juce::jmax(0, chainLength - 1);
    markSceneChainDefinitionChanged(processor);
}

int SceneScheduler::getSceneChainLoopStartStep(const MlrVSTAudioProcessor& processor)
{
    const int chainLength = getSceneChainLengthInternal(processor);
    if (chainLength <= 0)
        return 0;
    return juce::jlimit(0, chainLength - 1, processor.sceneChainState.loopStart);
}

int SceneScheduler::getSceneChainLoopEndStep(const MlrVSTAudioProcessor& processor)
{
    const int chainLength = getSceneChainLengthInternal(processor);
    if (chainLength <= 0)
        return 0;
    const int safeLoopStart = juce::jlimit(0, chainLength - 1, processor.sceneChainState.loopStart);
    return juce::jlimit(safeLoopStart, chainLength - 1, processor.sceneChainState.loopEnd);
}

void SceneScheduler::setSceneChainLoopRange(MlrVSTAudioProcessor& processor, int startStep, int endStep)
{
    juce::ignoreUnused(startStep, endStep);
    const int chainLength = getSceneChainLengthInternal(processor);
    processor.sceneChainState.loopEnabled = chainLength >= 2;
    processor.sceneChainState.loopStart = 0;
    processor.sceneChainState.loopEnd = juce::jmax(0, chainLength - 1);
    markSceneChainDefinitionChanged(processor);
}

bool SceneScheduler::isSceneChainPlaybackActive(const MlrVSTAudioProcessor& processor)
{
    return processor.sceneSequenceActive && getSceneChainLengthInternal(processor) >= 2;
}

int SceneScheduler::getSceneChainPlaybackStepIndex(const MlrVSTAudioProcessor& processor)
{
    return processor.sceneSequenceActive ? processor.sceneSequenceCurrentStepIndex : -1;
}

int SceneScheduler::getSceneSequenceStepIndex(const MlrVSTAudioProcessor& processor, int sceneSlot)
{
    const int clampedSlot = juce::jlimit(0, MlrVSTAudioProcessor::SceneSlots - 1, sceneSlot);
    const int chainLength = getSceneChainLengthInternal(processor);
    for (int stepIndex = 0; stepIndex < chainLength; ++stepIndex)
    {
        if (getSceneChainStepSceneSlot(processor, stepIndex) == clampedSlot)
            return stepIndex;
    }
    return -1;
}

int SceneScheduler::getQueuedSceneSlot(const MlrVSTAudioProcessor& processor)
{
    MlrVSTAudioProcessor::SceneSwitchEvent pendingSwitchEvent;
    if (processor.peekPendingSceneApplyState(pendingSwitchEvent))
        return juce::jlimit(0, MlrVSTAudioProcessor::SceneSlots - 1, pendingSwitchEvent.sceneSlot);
    if (processor.pendingSceneRecall.active)
        return juce::jlimit(0, MlrVSTAudioProcessor::SceneSlots - 1, processor.pendingSceneRecall.sceneSlot);
    return -1;
}

juce::String SceneScheduler::getSceneSequenceSummaryText(const MlrVSTAudioProcessor& processor)
{
    const int activeSlot = processor.getActiveSceneSlot();
    const int queuedSlot = getQueuedSceneSlot(processor);
    const int chainLength = getSceneChainLengthInternal(processor);
    juce::String changeModeLabel;
    switch (sanitizeSceneRecallMode(getSceneRecallModeIndex(processor)))
    {
        case SceneRecallMode::QuantizeGrid: changeModeLabel = "Grid"; break;
        case SceneRecallMode::PatternEnd:   changeModeLabel = "Pattern End"; break;
        case SceneRecallMode::SceneEnd:     changeModeLabel = "Scene End"; break;
        case SceneRecallMode::Manual:       changeModeLabel = "Manual"; break;
    }

    juce::String summary;
    if (chainLength <= 0)
    {
        summary = "Chain: empty";
    }
    else
    {
        summary = "Chain: ";
        for (int stepIndex = 0; stepIndex < chainLength; ++stepIndex)
        {
            if (stepIndex > 0)
            {
                const auto transitionLabel = sceneChainTransitionSummaryLabel(
                    sanitizeSceneChainTransitionType(
                        getSceneChainStepTransitionTypeIndex(processor, stepIndex - 1)));
                if (transitionLabel.isNotEmpty())
                    summary << " -" << transitionLabel << "-> ";
                else
                    summary << " -> ";
            }
            summary << "S" << juce::String(getSceneChainStepSceneSlot(processor, stepIndex) + 1);
            const int repeats = getSceneChainStepRepeatCount(processor, stepIndex);
            if (repeats > 1)
                summary << "x" << juce::String(repeats);
        }
        if (isSceneChainLoopEnabled(processor))
        {
            summary << " | Loop "
                    << juce::String(getSceneChainLoopStartStep(processor) + 1)
                    << "-"
                    << juce::String(getSceneChainLoopEndStep(processor) + 1);
        }
    }

    summary << " | Run: ";
    if (isSceneChainPlaybackActive(processor) && processor.sceneSequenceCurrentStepIndex >= 0 && chainLength > 0)
    {
        summary << juce::String(processor.sceneSequenceCurrentStepIndex + 1)
                << "/"
                << juce::String(chainLength);
    }
    else
    {
        summary << "off";
    }
    summary << " | Change: " << changeModeLabel;
    summary << " | Active: S" << juce::String(activeSlot + 1);
    if (queuedSlot >= 0)
        summary << " | Next: S" << juce::String(queuedSlot + 1);
    return summary;
}

std::unique_ptr<juce::XmlElement> SceneScheduler::createSceneChainStateXml(const MlrVSTAudioProcessor& processor,
                                                                           int sceneSlotOverride)
{
    auto xml = std::make_unique<juce::XmlElement>("SceneChainState");
    if (sceneSlotOverride >= 0)
    {
        const int clampedSlot = juce::jlimit(0, MlrVSTAudioProcessor::SceneSlots - 1, sceneSlotOverride);
        xml->setAttribute("storedSceneSnapshot", true);
        xml->setAttribute("sceneSlot", clampedSlot);
        xml->setAttribute("repeatCount", getSceneRepeatCount(processor, clampedSlot));
        xml->setAttribute("lengthMode", getSceneLengthModeIndex(processor, clampedSlot));
        xml->setAttribute("manualBars", getSceneManualBars(processor, clampedSlot));
        xml->setAttribute("anchorStrip", getSceneAnchorStrip(processor, clampedSlot));
        return xml;
    }

    for (int sceneSlot = 0; sceneSlot < MlrVSTAudioProcessor::SceneSlots; ++sceneSlot)
    {
        xml->setAttribute("sceneRepeat" + juce::String(sceneSlot), getSceneRepeatCount(processor, sceneSlot));
        xml->setAttribute("sceneLengthMode" + juce::String(sceneSlot), getSceneLengthModeIndex(processor, sceneSlot));
        xml->setAttribute("sceneManualBars" + juce::String(sceneSlot), getSceneManualBars(processor, sceneSlot));
        xml->setAttribute("sceneAnchorStrip" + juce::String(sceneSlot), getSceneAnchorStrip(processor, sceneSlot));
    }
    const int chainLength = getSceneChainLengthInternal(processor);
    xml->setAttribute("chainStepCount", chainLength);
    xml->setAttribute("chainLoopEnabled", isSceneChainLoopEnabled(processor));
    xml->setAttribute("chainLoopStart", getSceneChainLoopStartStep(processor));
    xml->setAttribute("chainLoopEnd", getSceneChainLoopEndStep(processor));
    xml->setAttribute("chainTransitionSampleDir", getSceneTransitionEndSampleDirectory(processor).getFullPathName());
    for (int stepIndex = 0; stepIndex < chainLength; ++stepIndex)
    {
        xml->setAttribute("chainScene" + juce::String(stepIndex), getSceneChainStepSceneSlot(processor, stepIndex));
        xml->setAttribute("chainRepeats" + juce::String(stepIndex), getSceneChainStepRepeatCount(processor, stepIndex));
        xml->setAttribute("chainTransition" + juce::String(stepIndex),
                          getSceneChainStepTransitionTypeIndex(processor, stepIndex));
        xml->setAttribute("chainTransitionOption" + juce::String(stepIndex),
                          getSceneChainStepTransitionOptionIndex(processor, stepIndex));
        xml->setAttribute("chainTransitionScope" + juce::String(stepIndex),
                          getSceneChainStepTransitionScopeIndex(processor, stepIndex));
        xml->setAttribute("chainTransitionContour" + juce::String(stepIndex),
                          getSceneChainStepTransitionContourIndex(processor, stepIndex));
        xml->setAttribute("chainTransitionCondition" + juce::String(stepIndex),
                          getSceneChainStepTransitionConditionIndex(processor, stepIndex));
        xml->setAttribute("chainTransitionLength" + juce::String(stepIndex),
                          getSceneChainStepTransitionLengthBeats(processor, stepIndex));
        xml->setAttribute("chainTransitionSubtractFromScene" + juce::String(stepIndex),
                          getSceneChainStepTransitionSubtractsFromSceneLength(processor, stepIndex));
        xml->setAttribute("chainTransitionIntensity" + juce::String(stepIndex),
                          getSceneChainStepTransitionIntensity(processor, stepIndex));
        xml->setAttribute("chainTransitionDelay" + juce::String(stepIndex),
                          getSceneChainStepTransitionDelayAmount(processor, stepIndex));
        xml->setAttribute("chainTransitionFilter" + juce::String(stepIndex),
                          getSceneChainStepTransitionFilterAmount(processor, stepIndex));
        xml->setAttribute("chainTransitionChop" + juce::String(stepIndex),
                          getSceneChainStepTransitionChopAmount(processor, stepIndex));
        xml->setAttribute("chainTransitionEndSample" + juce::String(stepIndex),
                          getSceneChainStepTransitionEndSampleFile(processor, stepIndex).getFullPathName());
        const auto endSampleSettings = getSceneChainStepTransitionEndSampleSettings(processor, stepIndex);
        xml->setAttribute("chainTransitionEndSampleGainDb" + juce::String(stepIndex), endSampleSettings.gainDb);
        xml->setAttribute("chainTransitionEndSampleFadeIn" + juce::String(stepIndex), endSampleSettings.fadeInMs);
        xml->setAttribute("chainTransitionEndSampleFadeOut" + juce::String(stepIndex), endSampleSettings.fadeOutMs);
        xml->setAttribute("chainTransitionEndSampleChoke" + juce::String(stepIndex), endSampleSettings.chokePrevious);
        xml->setAttribute("chainTransitionEndSampleReverse" + juce::String(stepIndex), endSampleSettings.reverse);
        xml->setAttribute("chainTransitionEndSamplePitch" + juce::String(stepIndex), endSampleSettings.pitchSemitones);
        xml->setAttribute("chainTransitionEndSampleLowpass" + juce::String(stepIndex), endSampleSettings.lowpassHz);
        xml->setAttribute("chainTransitionEndSampleHighpass" + juce::String(stepIndex), endSampleSettings.highpassHz);
        xml->setAttribute("chainTransitionEndSampleDuckSource" + juce::String(stepIndex), endSampleSettings.duckSource);
        xml->setAttribute("chainTransitionEndSampleDuckAmount" + juce::String(stepIndex), endSampleSettings.duckAmount);
    }

    for (int favoriteIndex = 0; favoriteIndex < MlrVSTAudioProcessor::SceneTransitionFavoriteSlots; ++favoriteIndex)
    {
        const auto& favorite = processor.sceneTransitionFavorites[static_cast<size_t>(favoriteIndex)];
        xml->setAttribute("chainFillFavoriteValid" + juce::String(favoriteIndex), favorite.valid);
        xml->setAttribute("chainFillFavoriteType" + juce::String(favoriteIndex), static_cast<int>(favorite.type));
        xml->setAttribute("chainFillFavoriteOption" + juce::String(favoriteIndex), static_cast<int>(favorite.option));
        xml->setAttribute("chainFillFavoriteScope" + juce::String(favoriteIndex), static_cast<int>(favorite.scope));
        xml->setAttribute("chainFillFavoriteContour" + juce::String(favoriteIndex), static_cast<int>(favorite.contour));
        xml->setAttribute("chainFillFavoriteCondition" + juce::String(favoriteIndex), static_cast<int>(favorite.condition));
        xml->setAttribute("chainFillFavoriteLength" + juce::String(favoriteIndex), favorite.lengthBeats);
        xml->setAttribute("chainFillFavoriteSubtract" + juce::String(favoriteIndex), favorite.subtractFromSceneLength);
        xml->setAttribute("chainFillFavoriteIntensity" + juce::String(favoriteIndex), favorite.intensity);
        xml->setAttribute("chainFillFavoriteDelay" + juce::String(favoriteIndex), favorite.delayAmount);
        xml->setAttribute("chainFillFavoriteFilter" + juce::String(favoriteIndex), favorite.filterAmount);
        xml->setAttribute("chainFillFavoriteChop" + juce::String(favoriteIndex), favorite.chopAmount);
    }

    auto* storedScenesXml = xml->createNewChildElement("StoredScenes");
    const int activeMainPresetIndex = processor.getActiveMainPresetIndexForScenes();
    for (int sceneSlot = 0; sceneSlot < MlrVSTAudioProcessor::SceneSlots; ++sceneSlot)
    {
        const auto* storedSceneState = processor.getStoredSceneSlotState(activeMainPresetIndex, sceneSlot);
        if (storedSceneState == nullptr
            || storedSceneState->implicitMainPresetFallback
            || storedSceneState->preparedSwitchPayloadTemplate == nullptr)
            continue;

        auto* sceneSlotXml = storedScenesXml->createNewChildElement("SceneSlot");
        sceneSlotXml->setAttribute("slot", sceneSlot);
        if (storedSceneState->name.isNotEmpty())
            sceneSlotXml->setAttribute("name", storedSceneState->name);
        if (auto snapshotXml = processor.createSceneSnapshotPresetXml(
                *storedSceneState->preparedSwitchPayloadTemplate,
                storedSceneState->name))
            sceneSlotXml->addChildElement(snapshotXml.release());
    }
    return xml;
}

void SceneScheduler::applySceneChainStateXml(MlrVSTAudioProcessor& processor,
                                             const juce::XmlElement* xml,
                                             int sceneSlotOverride)
{
    if (xml == nullptr || !xml->hasTagName("SceneChainState"))
        return;

    if (sceneSlotOverride >= 0)
    {
        const int clampedSlot = juce::jlimit(0, MlrVSTAudioProcessor::SceneSlots - 1, sceneSlotOverride);
        const int repeatCount = xml->hasAttribute("repeatCount")
            ? xml->getIntAttribute("repeatCount", getSceneRepeatCount(processor, clampedSlot))
            : xml->getIntAttribute("sceneRepeat" + juce::String(clampedSlot), getSceneRepeatCount(processor, clampedSlot));
        const int lengthMode = xml->hasAttribute("lengthMode")
            ? xml->getIntAttribute("lengthMode", getSceneLengthModeIndex(processor, clampedSlot))
            : xml->getIntAttribute("sceneLengthMode" + juce::String(clampedSlot), getSceneLengthModeIndex(processor, clampedSlot));
        const int manualBars = xml->hasAttribute("manualBars")
            ? xml->getIntAttribute("manualBars", getSceneManualBars(processor, clampedSlot))
            : xml->getIntAttribute("sceneManualBars" + juce::String(clampedSlot), getSceneManualBars(processor, clampedSlot));
        const int anchorStrip = xml->hasAttribute("anchorStrip")
            ? xml->getIntAttribute("anchorStrip", getSceneAnchorStrip(processor, clampedSlot))
            : xml->getIntAttribute("sceneAnchorStrip" + juce::String(clampedSlot), getSceneAnchorStrip(processor, clampedSlot));
        setSceneRepeatCount(processor, clampedSlot, repeatCount);
        setSceneLengthModeIndex(processor, clampedSlot, lengthMode);
        setSceneManualBars(processor, clampedSlot, manualBars);
        setSceneAnchorStrip(processor, clampedSlot, anchorStrip);
        return;
    }

    bool anyApplied = false;
    for (int sceneSlot = 0; sceneSlot < MlrVSTAudioProcessor::SceneSlots; ++sceneSlot)
    {
        const auto repeatAttr = "sceneRepeat" + juce::String(sceneSlot);
        const auto lengthAttr = "sceneLengthMode" + juce::String(sceneSlot);
        const auto barsAttr = "sceneManualBars" + juce::String(sceneSlot);
        const auto anchorAttr = "sceneAnchorStrip" + juce::String(sceneSlot);
        const bool hasSceneAttributes = xml->hasAttribute(repeatAttr)
            || xml->hasAttribute(lengthAttr)
            || xml->hasAttribute(barsAttr)
            || xml->hasAttribute(anchorAttr);
        if (!hasSceneAttributes)
            continue;

        if (xml->hasAttribute(repeatAttr))
            setSceneRepeatCount(processor, sceneSlot, xml->getIntAttribute(repeatAttr, getSceneRepeatCount(processor, sceneSlot)));
        if (xml->hasAttribute(lengthAttr))
            setSceneLengthModeIndex(processor, sceneSlot, xml->getIntAttribute(lengthAttr, getSceneLengthModeIndex(processor, sceneSlot)));
        if (xml->hasAttribute(barsAttr))
            setSceneManualBars(processor, sceneSlot, xml->getIntAttribute(barsAttr, getSceneManualBars(processor, sceneSlot)));
        if (xml->hasAttribute(anchorAttr))
            setSceneAnchorStrip(processor, sceneSlot, xml->getIntAttribute(anchorAttr, getSceneAnchorStrip(processor, sceneSlot)));
        anyApplied = true;
    }

    if (!anyApplied && xml->hasAttribute("sceneSlot"))
    {
        const int slot = juce::jlimit(0, MlrVSTAudioProcessor::SceneSlots - 1, xml->getIntAttribute("sceneSlot", 0));
        setSceneRepeatCount(processor, slot, xml->getIntAttribute("repeatCount", getSceneRepeatCount(processor, slot)));
        setSceneLengthModeIndex(processor, slot, xml->getIntAttribute("lengthMode", getSceneLengthModeIndex(processor, slot)));
        setSceneManualBars(processor, slot, xml->getIntAttribute("manualBars", getSceneManualBars(processor, slot)));
        setSceneAnchorStrip(processor, slot, xml->getIntAttribute("anchorStrip", getSceneAnchorStrip(processor, slot)));
    }

    bool hasChainAttributes = xml->hasAttribute("chainStepCount")
        || xml->hasAttribute("chainLoopEnabled")
        || xml->hasAttribute("chainLoopStart")
        || xml->hasAttribute("chainLoopEnd")
        || xml->hasAttribute("chainTransitionSampleDir");
    for (int stepIndex = 0; stepIndex < MlrVSTAudioProcessor::MaxSceneChainSteps && !hasChainAttributes; ++stepIndex)
    {
            hasChainAttributes = xml->hasAttribute("chainScene" + juce::String(stepIndex))
                || xml->hasAttribute("chainRepeats" + juce::String(stepIndex))
                || xml->hasAttribute("chainTransition" + juce::String(stepIndex))
                || xml->hasAttribute("chainTransitionOption" + juce::String(stepIndex))
                || xml->hasAttribute("chainTransitionScope" + juce::String(stepIndex))
                || xml->hasAttribute("chainTransitionContour" + juce::String(stepIndex))
                || xml->hasAttribute("chainTransitionCondition" + juce::String(stepIndex))
                || xml->hasAttribute("chainTransitionLength" + juce::String(stepIndex))
                || xml->hasAttribute("chainTransitionSubtractFromScene" + juce::String(stepIndex))
                || xml->hasAttribute("chainTransitionIntensity" + juce::String(stepIndex))
                || xml->hasAttribute("chainTransitionDelay" + juce::String(stepIndex))
                || xml->hasAttribute("chainTransitionFilter" + juce::String(stepIndex))
                || xml->hasAttribute("chainTransitionChop" + juce::String(stepIndex))
                || xml->hasAttribute("chainTransitionEndSample" + juce::String(stepIndex))
                || xml->hasAttribute("chainTransitionEndSampleGainDb" + juce::String(stepIndex))
                || xml->hasAttribute("chainTransitionEndSampleFadeIn" + juce::String(stepIndex))
                || xml->hasAttribute("chainTransitionEndSampleFadeOut" + juce::String(stepIndex))
                || xml->hasAttribute("chainTransitionEndSampleChoke" + juce::String(stepIndex))
                || xml->hasAttribute("chainTransitionEndSampleReverse" + juce::String(stepIndex))
                || xml->hasAttribute("chainTransitionEndSamplePitch" + juce::String(stepIndex))
                || xml->hasAttribute("chainTransitionEndSampleLowpass" + juce::String(stepIndex))
                || xml->hasAttribute("chainTransitionEndSampleHighpass" + juce::String(stepIndex))
                || xml->hasAttribute("chainTransitionEndSampleDuckSource" + juce::String(stepIndex))
                || xml->hasAttribute("chainTransitionEndSampleDuckAmount" + juce::String(stepIndex));
    }

    if (hasChainAttributes)
    {
        const auto previousEndSampleDirectory = processor.sceneChainState.transitionEndSampleDirectory;
        for (auto& step : processor.sceneChainState.steps)
            clearSceneChainStep(step);

        juce::File loadedEndSampleDirectory = previousEndSampleDirectory;
        if (xml->hasAttribute("chainTransitionSampleDir"))
        {
            const auto endSampleDirectoryPath = xml->getStringAttribute("chainTransitionSampleDir").trim();
            loadedEndSampleDirectory =
                endSampleDirectoryPath.isEmpty() ? juce::File() : juce::File(endSampleDirectoryPath);
        }

        const int chainLength = juce::jlimit(0,
                                             MlrVSTAudioProcessor::MaxSceneChainSteps,
                                             xml->getIntAttribute("chainStepCount", MlrVSTAudioProcessor::MaxSceneChainSteps));
        for (int stepIndex = 0; stepIndex < chainLength; ++stepIndex)
        {
            auto& step = processor.sceneChainState.steps[static_cast<size_t>(stepIndex)];
            step.sceneSlot = xml->getIntAttribute("chainScene" + juce::String(stepIndex), -1);
            step.repeats = xml->getIntAttribute("chainRepeats" + juce::String(stepIndex), 1);
            step.transitionToNext = sanitizeSceneChainTransitionType(
                xml->getIntAttribute("chainTransition" + juce::String(stepIndex),
                                     static_cast<int>(SceneChainTransitionType::None)));
            step.transitionOption = sanitizeSceneChainTransitionOption(
                xml->getIntAttribute("chainTransitionOption" + juce::String(stepIndex),
                                     static_cast<int>(SceneChainTransitionOption::Default)));
            step.transitionScope = sanitizeSceneChainTransitionScope(
                xml->getIntAttribute("chainTransitionScope" + juce::String(stepIndex),
                                     static_cast<int>(SceneChainTransitionScope::All)));
            step.transitionContour = sanitizeSceneChainTransitionContour(
                xml->getIntAttribute("chainTransitionContour" + juce::String(stepIndex),
                                     static_cast<int>(SceneChainTransitionContour::Smooth)));
            step.transitionCondition = sanitizeSceneChainTransitionCondition(
                xml->getIntAttribute("chainTransitionCondition" + juce::String(stepIndex),
                                     static_cast<int>(SceneChainTransitionCondition::Always)));
            step.transitionLengthBeats = static_cast<float>(
                xml->getDoubleAttribute("chainTransitionLength" + juce::String(stepIndex),
                                        MlrVSTAudioProcessor::DefaultSceneTransitionLengthBeats));
            step.transitionSubtractsFromSceneLength = static_cast<bool>(
                xml->getBoolAttribute("chainTransitionSubtractFromScene" + juce::String(stepIndex), false));
            step.transitionIntensity = static_cast<float>(
                xml->getDoubleAttribute("chainTransitionIntensity" + juce::String(stepIndex),
                                        MlrVSTAudioProcessor::DefaultSceneTransitionIntensity));
            step.transitionDelayAmount = static_cast<float>(
                xml->getDoubleAttribute("chainTransitionDelay" + juce::String(stepIndex),
                                        MlrVSTAudioProcessor::DefaultSceneTransitionDelayAmount));
            step.transitionFilterAmount = static_cast<float>(
                xml->getDoubleAttribute("chainTransitionFilter" + juce::String(stepIndex),
                                        MlrVSTAudioProcessor::DefaultSceneTransitionFilterAmount));
            step.transitionChopAmount = static_cast<float>(
                xml->getDoubleAttribute("chainTransitionChop" + juce::String(stepIndex),
                                        MlrVSTAudioProcessor::DefaultSceneTransitionChopAmount));
            const auto endSamplePath =
                xml->getStringAttribute("chainTransitionEndSample" + juce::String(stepIndex)).trim();
            step.transitionEndSampleFile = endSamplePath.isEmpty() ? juce::File() : juce::File(endSamplePath);
            step.transitionEndSampleSettings.gainDb = static_cast<float>(
                xml->getDoubleAttribute("chainTransitionEndSampleGainDb" + juce::String(stepIndex),
                                        MlrVSTAudioProcessor::DefaultSceneTransitionEndSampleGainDb));
            step.transitionEndSampleSettings.fadeInMs = static_cast<float>(
                xml->getDoubleAttribute("chainTransitionEndSampleFadeIn" + juce::String(stepIndex),
                                        MlrVSTAudioProcessor::DefaultSceneTransitionEndSampleFadeInMs));
            step.transitionEndSampleSettings.fadeOutMs = static_cast<float>(
                xml->getDoubleAttribute("chainTransitionEndSampleFadeOut" + juce::String(stepIndex),
                                        MlrVSTAudioProcessor::DefaultSceneTransitionEndSampleFadeOutMs));
            step.transitionEndSampleSettings.chokePrevious =
                xml->getBoolAttribute("chainTransitionEndSampleChoke" + juce::String(stepIndex), false);
            step.transitionEndSampleSettings.reverse =
                xml->getBoolAttribute("chainTransitionEndSampleReverse" + juce::String(stepIndex), false);
            step.transitionEndSampleSettings.pitchSemitones = static_cast<float>(
                xml->getDoubleAttribute("chainTransitionEndSamplePitch" + juce::String(stepIndex),
                                        MlrVSTAudioProcessor::DefaultSceneTransitionEndSamplePitchSemitones));
            step.transitionEndSampleSettings.lowpassHz = static_cast<float>(
                xml->getDoubleAttribute("chainTransitionEndSampleLowpass" + juce::String(stepIndex),
                                        MlrVSTAudioProcessor::DefaultSceneTransitionEndSampleLowpassHz));
            step.transitionEndSampleSettings.highpassHz = static_cast<float>(
                xml->getDoubleAttribute("chainTransitionEndSampleHighpass" + juce::String(stepIndex),
                                        MlrVSTAudioProcessor::DefaultSceneTransitionEndSampleHighpassHz));
            step.transitionEndSampleSettings.duckSource =
                xml->getIntAttribute("chainTransitionEndSampleDuckSource" + juce::String(stepIndex),
                                     MlrVSTAudioProcessor::DefaultSceneTransitionEndSampleDuckSource);
            step.transitionEndSampleSettings.duckAmount = static_cast<float>(
                xml->getDoubleAttribute("chainTransitionEndSampleDuckAmount" + juce::String(stepIndex),
                                        MlrVSTAudioProcessor::DefaultSceneTransitionEndSampleDuckAmount));
            sanitizeSceneChainTransitionParameters(step);
        }

        processor.sceneChainState.transitionEndSampleDirectory =
            inferSceneTransitionEndSampleDirectory(processor.sceneChainState, loadedEndSampleDirectory);
        processor.sceneChainState.loopEnabled = static_cast<bool>(xml->getBoolAttribute("chainLoopEnabled", false));
        processor.sceneChainState.loopStart = xml->getIntAttribute("chainLoopStart", 0);
        processor.sceneChainState.loopEnd = xml->getIntAttribute("chainLoopEnd", juce::jmax(0, chainLength - 1));
        sanitizeSceneChainRuntimeState(processor);
    }

    bool hasFavoriteAttributes = false;
    for (int favoriteIndex = 0;
         favoriteIndex < MlrVSTAudioProcessor::SceneTransitionFavoriteSlots && !hasFavoriteAttributes;
         ++favoriteIndex)
    {
        hasFavoriteAttributes = xml->hasAttribute("chainFillFavoriteValid" + juce::String(favoriteIndex))
            || xml->hasAttribute("chainFillFavoriteType" + juce::String(favoriteIndex))
            || xml->hasAttribute("chainFillFavoriteOption" + juce::String(favoriteIndex))
            || xml->hasAttribute("chainFillFavoriteScope" + juce::String(favoriteIndex))
            || xml->hasAttribute("chainFillFavoriteContour" + juce::String(favoriteIndex))
            || xml->hasAttribute("chainFillFavoriteCondition" + juce::String(favoriteIndex))
            || xml->hasAttribute("chainFillFavoriteLength" + juce::String(favoriteIndex))
            || xml->hasAttribute("chainFillFavoriteSubtract" + juce::String(favoriteIndex))
            || xml->hasAttribute("chainFillFavoriteIntensity" + juce::String(favoriteIndex))
            || xml->hasAttribute("chainFillFavoriteDelay" + juce::String(favoriteIndex))
            || xml->hasAttribute("chainFillFavoriteFilter" + juce::String(favoriteIndex))
            || xml->hasAttribute("chainFillFavoriteChop" + juce::String(favoriteIndex));
    }

    if (hasFavoriteAttributes)
    {
        processor.sceneTransitionFavorites = {};
        for (int favoriteIndex = 0; favoriteIndex < MlrVSTAudioProcessor::SceneTransitionFavoriteSlots; ++favoriteIndex)
        {
            auto& favorite = processor.sceneTransitionFavorites[static_cast<size_t>(favoriteIndex)];
            favorite.valid = xml->getBoolAttribute("chainFillFavoriteValid" + juce::String(favoriteIndex), false);
            favorite.type = sanitizeSceneChainTransitionType(
                xml->getIntAttribute("chainFillFavoriteType" + juce::String(favoriteIndex),
                                     static_cast<int>(SceneChainTransitionType::None)));
            favorite.option = sanitizeSceneChainTransitionOption(
                xml->getIntAttribute("chainFillFavoriteOption" + juce::String(favoriteIndex),
                                     static_cast<int>(SceneChainTransitionOption::Default)));
            favorite.scope = sanitizeSceneChainTransitionScope(
                xml->getIntAttribute("chainFillFavoriteScope" + juce::String(favoriteIndex),
                                     static_cast<int>(SceneChainTransitionScope::All)));
            favorite.contour = sanitizeSceneChainTransitionContour(
                xml->getIntAttribute("chainFillFavoriteContour" + juce::String(favoriteIndex),
                                     static_cast<int>(SceneChainTransitionContour::Smooth)));
            favorite.condition = sanitizeSceneChainTransitionCondition(
                xml->getIntAttribute("chainFillFavoriteCondition" + juce::String(favoriteIndex),
                                     static_cast<int>(SceneChainTransitionCondition::Always)));
            favorite.lengthBeats = static_cast<float>(
                xml->getDoubleAttribute("chainFillFavoriteLength" + juce::String(favoriteIndex),
                                        MlrVSTAudioProcessor::DefaultSceneTransitionLengthBeats));
            favorite.subtractFromSceneLength = xml->getBoolAttribute("chainFillFavoriteSubtract" + juce::String(favoriteIndex),
                                                                     false);
            favorite.intensity = static_cast<float>(
                xml->getDoubleAttribute("chainFillFavoriteIntensity" + juce::String(favoriteIndex),
                                        MlrVSTAudioProcessor::DefaultSceneTransitionIntensity));
            favorite.delayAmount = static_cast<float>(
                xml->getDoubleAttribute("chainFillFavoriteDelay" + juce::String(favoriteIndex),
                                        MlrVSTAudioProcessor::DefaultSceneTransitionDelayAmount));
            favorite.filterAmount = static_cast<float>(
                xml->getDoubleAttribute("chainFillFavoriteFilter" + juce::String(favoriteIndex),
                                        MlrVSTAudioProcessor::DefaultSceneTransitionFilterAmount));
            favorite.chopAmount = static_cast<float>(
                xml->getDoubleAttribute("chainFillFavoriteChop" + juce::String(favoriteIndex),
                                        MlrVSTAudioProcessor::DefaultSceneTransitionChopAmount));
            sanitizeSceneChainTransitionFavorite(favorite);
        }
    }
}

double SceneScheduler::computeCurrentSceneSequenceLengthBeats(const MlrVSTAudioProcessor& processor)
{
    if (processor.sceneSequenceActive)
    {
        const int chainLength = getSceneChainLengthInternal(processor);
        if (chainLength > 0 && processor.sceneSequenceCurrentStepIndex >= 0)
        {
            const int safeStepIndex = juce::jlimit(0, chainLength - 1, processor.sceneSequenceCurrentStepIndex);
            const auto& step = processor.sceneChainState.steps[static_cast<size_t>(safeStepIndex)];
            const int sceneSlot = getSceneChainStepSceneSlot(processor, safeStepIndex);
            const double baseLength = getSceneAdvanceLengthBeats(processor, sceneSlot);
            const double stepRepeats = static_cast<double>(getSceneChainStepRepeatCount(processor, safeStepIndex));
            double effectiveLength = baseLength * stepRepeats;
            if (step.transitionSubtractsFromSceneLength)
            {
                effectiveLength -= sceneChainTransitionLeadBeats(
                    sanitizeSceneChainTransitionType(step.transitionToNext),
                    step.transitionLengthBeats);
            }
            return juce::jlimit(0.25, 4096.0, effectiveLength);
        }
    }
    return getSceneAdvanceLengthBeats(processor, processor.activeSceneSlot);
}

double SceneScheduler::computeNextScenePatternEndPpq(const MlrVSTAudioProcessor& processor,
                                                     int sceneSlot,
                                                     double currentPpq,
                                                     double cycleBeats,
                                                     uint64_t* outPhaseSignature)
{
    if (processor.audioEngine == nullptr
        || !std::isfinite(currentPpq)
        || !std::isfinite(cycleBeats)
        || cycleBeats <= 0.0)
    {
        if (outPhaseSignature != nullptr)
            *outPhaseSignature = 0;
        return std::numeric_limits<double>::quiet_NaN();
    }

    const int clampedSceneSlot = juce::jlimit(0, MlrVSTAudioProcessor::SceneSlots - 1, sceneSlot);
    const auto lengthMode = sanitizeSceneLengthMode(getSceneLengthModeIndex(processor, clampedSceneSlot));
    const int anchorStrip = juce::jlimit(0, MlrVSTAudioProcessor::MaxStrips - 1, getSceneAnchorStrip(processor, clampedSceneSlot));

    bool foundCandidate = false;
    double nextPatternEndPpq = std::numeric_limits<double>::quiet_NaN();
    uint64_t phaseSignature = 0xcbf29ce484222325ull;

    auto combineSignature = [&phaseSignature](uint64_t value)
    {
        phaseSignature ^= value + 0x9e3779b97f4a7c15ull + (phaseSignature << 6) + (phaseSignature >> 2);
    };

    auto accumulateStripBoundary = [&](int stripIndex)
    {
        if (stripIndex < 0 || stripIndex >= MlrVSTAudioProcessor::MaxStrips)
            return;

        auto* strip = processor.audioEngine->getStrip(stripIndex);
        if (strip == nullptr || !strip->isPlaying() || !strip->isPpqTimelineAnchored())
            return;

        const auto playMode = strip->getPlayMode();
        if (playMode == EnhancedAudioStrip::PlayMode::Step
            || playMode == EnhancedAudioStrip::PlayMode::Sample)
            return;

        const bool hasStripAudio = playMode == EnhancedAudioStrip::PlayMode::Sample
            ? processor.hasSampleModeAudio(stripIndex)
            : strip->hasAudio();
        if (!hasStripAudio)
            return;

        const double triggerPpq = strip->getLastTriggerPPQ();
        if (!std::isfinite(triggerPpq) || triggerPpq < 0.0)
            return;

        uint64_t triggerBits = 0;
        static_assert(sizeof(triggerBits) == sizeof(triggerPpq), "Unexpected double size");
        std::memcpy(&triggerBits, &triggerPpq, sizeof(triggerBits));
        combineSignature(static_cast<uint64_t>(stripIndex + 1));
        combineSignature(static_cast<uint64_t>(playMode));
        combineSignature(triggerBits);

        const double stripCycleBeats = juce::jmax(
            cycleBeats,
            computeStripSceneSequenceLengthBeats(processor, stripIndex));
        if (!std::isfinite(stripCycleBeats) || stripCycleBeats <= 0.0)
            return;

        double nextBoundary = triggerPpq + stripCycleBeats;
        if (nextBoundary <= currentPpq + 1.0e-9)
        {
            const double elapsed = juce::jmax(0.0, currentPpq - triggerPpq);
            const double completedCycles = std::floor(elapsed / juce::jmax(1.0e-9, stripCycleBeats));
            nextBoundary = triggerPpq + ((completedCycles + 1.0) * stripCycleBeats);
        }

        nextPatternEndPpq = foundCandidate ? juce::jmax(nextPatternEndPpq, nextBoundary) : nextBoundary;
        foundCandidate = true;
    };

    if (lengthMode == SceneLengthMode::AnchorStrip)
        accumulateStripBoundary(anchorStrip);

    if (!foundCandidate)
    {
        for (int stripIndex = 0; stripIndex < MlrVSTAudioProcessor::MaxStrips; ++stripIndex)
            accumulateStripBoundary(stripIndex);
    }

    if (outPhaseSignature != nullptr)
        *outPhaseSignature = foundCandidate ? phaseSignature : 0;

    return foundCandidate ? nextPatternEndPpq : std::numeric_limits<double>::quiet_NaN();
}

bool SceneScheduler::startSceneChainPlayback(MlrVSTAudioProcessor& processor, int startStepIndex)
{
    if (processor.audioEngine == nullptr)
        return false;

    sanitizeSceneChainRuntimeState(processor);
    const int chainLength = getSceneChainLengthInternal(processor);
    if (chainLength <= 0)
        return false;

    const int safeStartStep = juce::jlimit(0, chainLength - 1, startStepIndex);
    const int requestedSceneSlot = getSceneChainStepSceneSlot(processor, safeStartStep);
    if (requestedSceneSlot < 0)
        return false;

    const bool chainCanRun = chainLength >= 2;
    const int activeSceneSlot = juce::jlimit(0, MlrVSTAudioProcessor::SceneSlots - 1, processor.activeSceneSlot);
    double attachSceneStartPpq = std::numeric_limits<double>::quiet_NaN();
    if (processor.activeScenePlaybackHandle.active
        && std::isfinite(processor.activeScenePlaybackHandle.startPpq))
    {
        attachSceneStartPpq = processor.activeScenePlaybackHandle.startPpq;
    }
    else if (processor.activeSceneStartPpqValid && std::isfinite(processor.activeSceneStartPpq))
    {
        attachSceneStartPpq = processor.activeSceneStartPpq;
    }
    else if (processor.sceneChainAttachStartPpqValid && std::isfinite(processor.sceneChainAttachStartPpq))
    {
        attachSceneStartPpq = processor.sceneChainAttachStartPpq;
    }
    else if (processor.audioEngine != nullptr)
    {
        const double fallbackPpq = processor.audioEngine->getTimelineBeat();
        if (std::isfinite(fallbackPpq))
            attachSceneStartPpq = fallbackPpq;
    }

    int attachStepIndex = -1;
    if (chainCanRun && std::isfinite(attachSceneStartPpq))
    {
        attachStepIndex = getSceneSequenceStepIndex(processor, activeSceneSlot);
    }

    const bool canAttachToCurrentScene = attachStepIndex >= 0;
    const int startStep = canAttachToCurrentScene ? attachStepIndex : safeStartStep;
    const int sceneSlot = canAttachToCurrentScene ? activeSceneSlot : requestedSceneSlot;
    processor.pendingSceneRecall = {};
    processor.clearPendingSceneApplyState();
    processor.pendingScenePreloadDirty.store(0, std::memory_order_release);
    processor.pendingScenePreloadMainPreset.store(-1, std::memory_order_release);
    processor.pendingScenePreloadSceneSlot.store(-1, std::memory_order_release);
    processor.pendingScenePreloadSequenceDriven.store(0, std::memory_order_release);
    processor.pendingScenePreloadSequenceStep.store(-1, std::memory_order_release);
    processor.pendingScenePreloadTargetPpq.store(-1.0, std::memory_order_release);
    processor.pendingScenePreloadTargetTempo.store(120.0, std::memory_order_release);
    processor.pendingScenePreloadTargetSample.store(-1, std::memory_order_release);
    processor.pendingScenePreloadTransitionType.store(static_cast<int>(SceneChainTransitionType::None),
                                                     std::memory_order_release);
    processor.requestAbortActiveSceneTransition();
    processor.clearSceneBoundaryTransitionState();
    processor.clearSceneChainReturnOverride();

    if (canAttachToCurrentScene)
    {
        MlrVSTAudioProcessor::SceneSwitchEvent switchEvent;
        switchEvent.active = true;
        switchEvent.owner = MlrVSTAudioProcessor::ScenePlaybackOwner::Chain;
        switchEvent.outgoingOwner = processor.getRenderedScenePlaybackOwner();
        switchEvent.sequenceDriven = true;
        switchEvent.ownerOnlySwitch = true;
        switchEvent.legatoOwnerSwitch = true;
        switchEvent.mainPresetIndex = processor.activeSceneMainPresetIndex;
        switchEvent.sceneSlot = sceneSlot;
        switchEvent.sequenceStepIndex = startStep;
        switchEvent.targetPpq = attachSceneStartPpq;
        switchEvent.targetTempo = processor.audioEngine != nullptr
            ? juce::jmax(1.0, processor.audioEngine->getCurrentTempo())
            : 120.0;
        switchEvent.targetGlobalSample = processor.audioEngine != nullptr
            ? processor.audioEngine->getGlobalSampleCount()
            : -1;
        switchEvent.blockStartSample = switchEvent.targetGlobalSample;
        switchEvent.blockNumSamples = 0;
        switchEvent.targetSampleOffsetInBlock = 0;
        switchEvent.boundaryCaptureSampleOffsetInBlock = -1;
        switchEvent.exactBlockBoundary = true;
        switchEvent.splitSwitchInBlock = false;
        processor.queuePendingSceneApplyState(switchEvent);
        processPendingSceneApply(processor);
        return true;
    }

    processor.switchScenePlaybackOwner(MlrVSTAudioProcessor::ScenePlaybackOwner::Chain,
                                       chainCanRun,
                                       startStep);

    requestSceneRecallQuantized(processor,
                                processor.getActiveMainPresetIndexForScenes(),
                                sceneSlot,
                                processor.sceneSequenceActive,
                                processor.sceneSequenceActive ? startStep : -1,
                                true);
    processor.updateMonomeLEDs();
    return true;
}

void SceneScheduler::stopSceneChainPlayback(MlrVSTAudioProcessor& processor)
{
    processor.switchScenePlaybackOwner(MlrVSTAudioProcessor::ScenePlaybackOwner::Manual, false);
    processor.pendingSceneRecall = {};
    processor.clearPendingSceneApplyState();
    processor.pendingScenePreloadDirty.store(0, std::memory_order_release);
    processor.pendingScenePreloadMainPreset.store(-1, std::memory_order_release);
    processor.pendingScenePreloadSceneSlot.store(-1, std::memory_order_release);
    processor.pendingScenePreloadSequenceDriven.store(0, std::memory_order_release);
    processor.pendingScenePreloadSequenceStep.store(-1, std::memory_order_release);
    processor.pendingScenePreloadTargetPpq.store(-1.0, std::memory_order_release);
    processor.pendingScenePreloadTargetTempo.store(120.0, std::memory_order_release);
    processor.pendingScenePreloadTargetSample.store(-1, std::memory_order_release);
    processor.pendingScenePreloadTransitionType.store(static_cast<int>(SceneChainTransitionType::None),
                                                     std::memory_order_release);
    processor.requestAbortActiveSceneTransition();
    processor.clearSceneBoundaryTransitionState();
    processor.clearSceneChainReturnOverride();
    processor.updateMonomeLEDs();
}

void SceneScheduler::armNextSceneInSequence(MlrVSTAudioProcessor& processor,
                                            int mainPresetIndex,
                                            int currentSceneSlot,
                                            double sceneStartPpq)
{
    const int chainLength = getSceneChainLengthInternal(processor);
    if (!processor.sceneSequenceActive || chainLength < 2)
    {
        processor.pendingSceneRecall.active = false;
        processor.pendingSceneRecall.targetResolved = false;
        processor.pendingSceneRecall.sequenceStepIndex = -1;
        processor.activeScenePlaybackHandle.sequenceDriven = false;
        processor.activeScenePlaybackHandle.sequenceStepIndex = -1;
        processor.sceneSequenceStartPpqValid = false;
        return;
    }

    int currentIndex = processor.sceneSequenceCurrentStepIndex;
    if (currentIndex < 0)
        currentIndex = getSceneSequenceStepIndex(processor, currentSceneSlot);
    if (currentIndex < 0)
        currentIndex = 0;

    processor.setActiveScenePlaybackHandle(processor.activeSceneMainPresetIndex,
                                           currentSceneSlot,
                                           true,
                                           currentIndex,
                                           sceneStartPpq,
                                           computeCurrentSceneSequenceLengthBeats(processor));

    int nextStepIndex = -1;
    if (!processor.consumeSceneChainReturnOverrideForStep(currentIndex, nextStepIndex))
        nextStepIndex = resolveSceneChainNextStepIndex(processor, currentIndex);
    if (nextStepIndex < 0)
    {
        processor.sceneSequenceActive = false;
        processor.pendingSceneRecall.active = false;
        processor.pendingSceneRecall.targetResolved = false;
        processor.pendingSceneRecall.sequenceStepIndex = -1;
        processor.activeScenePlaybackHandle.sequenceDriven = false;
        processor.activeScenePlaybackHandle.sequenceStepIndex = -1;
        processor.sceneSequenceStartPpqValid = false;
        return;
    }

    const int nextSlot = getSceneChainStepSceneSlot(processor, nextStepIndex);
    if (nextSlot < 0)
    {
        processor.sceneSequenceActive = false;
        processor.pendingSceneRecall.active = false;
        processor.pendingSceneRecall.targetResolved = false;
        processor.pendingSceneRecall.sequenceStepIndex = -1;
        processor.activeScenePlaybackHandle.sequenceDriven = false;
        processor.activeScenePlaybackHandle.sequenceStepIndex = -1;
        processor.sceneSequenceStartPpqValid = false;
        return;
    }

    processor.pendingSceneRecall.active = true;
    processor.pendingSceneRecall.sequenceDriven = true;
    processor.pendingSceneRecall.targetResolved = false;
    processor.pendingSceneRecall.mainPresetIndex = juce::jlimit(0, MlrVSTAudioProcessor::MaxPresetSlots - 1, mainPresetIndex);
    processor.pendingSceneRecall.sceneSlot = nextSlot;
    processor.pendingSceneRecall.sequenceStepIndex = nextStepIndex;
    processor.pendingSceneRecall.targetPpq = 0.0;
    processor.pendingSceneRecall.intervalBeats = 0.0;
    processor.pendingSceneRecall.patternEndPhaseSignatureValid = false;
    processor.pendingSceneRecall.patternEndPhaseSignature = 0;

    if (!processor.isTimerRunning())
        processor.startTimer(MlrVSTAudioProcessor::kGridRefreshMs);
}

void SceneScheduler::setSceneModeEnabled(MlrVSTAudioProcessor& processor, bool enabled)
{
    if (auto* param = processor.parameters.getParameter("sceneMode"))
    {
        const bool currentParamState = param->getValue() > 0.5f;
        if (currentParamState != enabled)
            param->setValueNotifyingHost(enabled ? 1.0f : 0.0f);
    }

    applySceneModeState(processor, enabled);
}

void SceneScheduler::captureSceneModeGroupSnapshot(MlrVSTAudioProcessor& processor)
{
    if (processor.sceneModeGroupSnapshot.valid || processor.audioEngine == nullptr)
        return;

    processor.sceneModeGroupSnapshot.valid = true;
    for (int stripIndex = 0; stripIndex < MlrVSTAudioProcessor::MaxStrips; ++stripIndex)
    {
        auto* strip = processor.audioEngine->getStrip(stripIndex);
        processor.sceneModeGroupSnapshot.stripGroups[static_cast<size_t>(stripIndex)] =
            strip != nullptr ? strip->getGroup() : -1;
    }

    for (int groupIndex = 0; groupIndex < ModernAudioEngine::MaxGroups; ++groupIndex)
    {
        auto* group = processor.audioEngine->getGroup(groupIndex);
        processor.sceneModeGroupSnapshot.groupVolumes[static_cast<size_t>(groupIndex)] =
            group != nullptr ? group->getVolume() : 1.0f;
        processor.sceneModeGroupSnapshot.groupMuted[static_cast<size_t>(groupIndex)] =
            group != nullptr ? group->isMuted() : false;
    }
}

void SceneScheduler::restoreSceneModeGroupSnapshot(MlrVSTAudioProcessor& processor)
{
    if (!processor.sceneModeGroupSnapshot.valid || processor.audioEngine == nullptr)
        return;

    for (int stripIndex = 0; stripIndex < MlrVSTAudioProcessor::MaxStrips; ++stripIndex)
    {
        processor.audioEngine->assignStripToGroup(
            stripIndex,
            processor.sceneModeGroupSnapshot.stripGroups[static_cast<size_t>(stripIndex)]);
    }

    for (int groupIndex = 0; groupIndex < ModernAudioEngine::MaxGroups; ++groupIndex)
    {
        if (auto* group = processor.audioEngine->getGroup(groupIndex))
        {
            group->setVolume(processor.sceneModeGroupSnapshot.groupVolumes[static_cast<size_t>(groupIndex)]);
            group->setMuted(processor.sceneModeGroupSnapshot.groupMuted[static_cast<size_t>(groupIndex)]);
        }
    }

    processor.sceneModeGroupSnapshot.valid = false;
}

void SceneScheduler::clearAllStripGroupsForSceneMode(MlrVSTAudioProcessor& processor)
{
    if (processor.audioEngine == nullptr)
        return;

    for (int stripIndex = 0; stripIndex < MlrVSTAudioProcessor::MaxStrips; ++stripIndex)
        processor.audioEngine->assignStripToGroup(stripIndex, -1);
}

void SceneScheduler::applySceneModeState(MlrVSTAudioProcessor& processor, bool enabled)
{
    const bool previousEnabled = processor.sceneModeEnabled.exchange(enabled ? 1 : 0, std::memory_order_acq_rel) != 0;
    if (previousEnabled == enabled)
        return;

    processor.scenePerformanceRecorder.stopRecording();
    processor.clearPendingMonomeSceneRecorderTap();
    processor.clearPendingSceneRecorderAction();
    processor.lastScenePerformanceProcessBeat = std::numeric_limits<double>::quiet_NaN();
    processor.lastScenePerformanceProcessSceneSlot = -1;
    processor.lastScenePerformanceProcessSceneStartBeat = std::numeric_limits<double>::quiet_NaN();
    processor.clearSceneBoundaryTransitionState();
    processor.clearSceneChainReturnOverride();

    if (enabled)
    {
        captureSceneModeGroupSnapshot(processor);
        clearAllStripGroupsForSceneMode(processor);
        if (processor.controlModeActive && processor.currentControlMode == MlrVSTAudioProcessor::ControlMode::GroupAssign)
        {
            processor.currentControlMode = MlrVSTAudioProcessor::ControlMode::Normal;
            processor.controlModeActive = false;
        }

        const int activeSceneSlot = juce::jlimit(0, MlrVSTAudioProcessor::SceneSlots - 1, processor.activeSceneSlot);
        if (processor.sceneSlotHasMotionState(activeSceneSlot))
            processor.applySceneMotionStateToEngine(activeSceneSlot);
        else
            processor.syncSceneMotionStateFromEngine(activeSceneSlot);
        juce::ignoreUnused(processor.ensureSceneSlotFallbackState(processor.getActiveMainPresetIndexForScenes(),
                                                                 activeSceneSlot));
        juce::ignoreUnused(processor.ensureActiveScenePlaybackHandleInitialized());
    }
    else
    {
        juce::ignoreUnused(processor.refreshStoredSceneSlotSnapshot(processor.getActiveMainPresetIndexForScenes(),
                                                                   processor.activeSceneSlot));
        restoreSceneModeGroupSnapshot(processor);
    }

    processor.pendingSceneRecall.active = false;
    processor.pendingSceneRecall.targetResolved = false;
    processor.pendingSceneRecall.sequenceDriven = false;
    processor.pendingSceneRecall.sequenceStepIndex = -1;
    processor.sceneSequenceActive = false;
    processor.sceneSequenceCurrentStepIndex = -1;
    processor.sceneSequenceStartPpqValid = false;
    processor.sceneSequenceStartPpq = 0.0;
    processor.clearPendingSceneApplyState();
    processor.scenePadHeld.fill(false);
    processor.scenePadHoldDeleteTriggered.fill(false);
    processor.scenePadLaunchConsumed.fill(false);
    processor.scenePadPressStartMs.fill(0);
    processor.scenePadActionBurstUntilMs.fill(0);
    processor.scenePadLastTapMs.fill(0);
    processor.sceneCopySourceSlot = -1;
    processor.sceneCopyMainPresetIndex = 0;

    processor.updateMonomeLEDs();
    processor.presetRefreshToken.fetch_add(1, std::memory_order_acq_rel);
}

void SceneScheduler::syncSceneModeFromParameters(MlrVSTAudioProcessor& processor)
{
    const bool desiredState = processor.sceneModeParam != nullptr
        && processor.sceneModeParam->load(std::memory_order_acquire) > 0.5f;
    if (desiredState != processor.isSceneModeEnabled())
        applySceneModeState(processor, desiredState);
}

bool SceneScheduler::saveSceneForMainPreset(MlrVSTAudioProcessor& processor, int mainPresetIndex, int sceneSlot)
{
    if (!processor.audioEngine)
        return false;

    const int safeSceneSlot = juce::jlimit(0, MlrVSTAudioProcessor::SceneSlots - 1, sceneSlot);
    if (processor.isSceneModeEnabled() && safeSceneSlot == processor.getActiveSceneSlot())
        processor.syncSceneMotionStateFromEngine(safeSceneSlot);
    processor.syncScenePerformanceClipLengthToResolvedLength(safeSceneSlot);

    const bool saved = processor.captureSceneSlotState(mainPresetIndex, safeSceneSlot)
        && processor.persistStoredSceneSlotStatesToMainPreset(mainPresetIndex);
    if (saved)
    {
        processor.clearSceneClipSlotRuntimeState(mainPresetIndex, safeSceneSlot);
        if (safeSceneSlot == processor.activeSceneSlot
            && juce::jlimit(0, MlrVSTAudioProcessor::MaxPresetSlots - 1, mainPresetIndex)
                == juce::jlimit(0, MlrVSTAudioProcessor::MaxPresetSlots - 1, processor.activeSceneMainPresetIndex))
        {
            processor.activeSceneNeedsCaptureBeforeManualRecall = false;
            processor.clearPendingActiveSceneAutosave();
        }
        processor.presetRefreshToken.fetch_add(1, std::memory_order_acq_rel);
    }
    return saved;
}

bool SceneScheduler::copySceneForMainPreset(MlrVSTAudioProcessor& processor,
                                            int mainPresetIndex,
                                            int sourceSceneSlot,
                                            int destSceneSlot)
{
    const int clampedMain = juce::jlimit(0, MlrVSTAudioProcessor::MaxPresetSlots - 1, mainPresetIndex);
    const int clampedSource = juce::jlimit(0, MlrVSTAudioProcessor::SceneSlots - 1, sourceSceneSlot);
    const int clampedDest = juce::jlimit(0, MlrVSTAudioProcessor::SceneSlots - 1, destSceneSlot);
    if (clampedSource == clampedDest)
        return true;

    const bool copyingActiveMainPreset = processor.activeSceneMainPresetIndex == clampedMain;
    const bool sourceIsActiveScene = copyingActiveMainPreset
        && processor.activeSceneSlot == clampedSource;
    if (sourceIsActiveScene)
        juce::ignoreUnused(processor.refreshStoredSceneSlotSnapshot(clampedMain, clampedSource));

    const bool sourceExists = processor.hasStoredSceneSlotState(clampedMain, clampedSource);
    juce::MemoryBlock sourceScenePerformanceState;
    if (copyingActiveMainPreset && sourceExists)
        sourceScenePerformanceState = processor.createScenePerformanceStateData(clampedSource);

    const bool copied = processor.copyStoredSceneSlotState(clampedMain, clampedSource, clampedDest);
    if (copied)
    {
        if (sourceExists && sourceScenePerformanceState.getSize() > 0)
        {
            auto syncStoredPayloadPerformanceState = [&](int sceneSlot)
            {
                auto& storedState = processor.storedSceneSlotStates[static_cast<size_t>(sceneSlot)];
                if (storedState.mainPresetIndex != clampedMain
                    || !storedState.hasStoredContent
                    || storedState.preparedSwitchPayloadTemplate == nullptr)
                {
                    return;
                }

                storedState.preparedSwitchPayloadTemplate->scenePerformanceStateData = sourceScenePerformanceState;
                storedState.preparedSwitchPayloadTemplate->snapshotPresetXml.reset();
            };

            syncStoredPayloadPerformanceState(clampedSource);
            syncStoredPayloadPerformanceState(clampedDest);
        }

        if (!processor.persistStoredSceneSlotStatesToMainPreset(clampedMain))
            return false;

        processor.copySceneClipSlotRuntimeState(clampedMain, clampedSource, clampedDest);

        if (copyingActiveMainPreset)
        {
            if (sourceExists)
                processor.applyScenePerformanceStateData(sourceScenePerformanceState, clampedDest);
            else
                processor.applyScenePerformanceStateData({}, clampedDest);
        }
        processor.presetRefreshToken.fetch_add(1, std::memory_order_acq_rel);
    }
    return copied;
}

bool SceneScheduler::deleteSceneForMainPreset(MlrVSTAudioProcessor& processor, int mainPresetIndex, int sceneSlot)
{
    const bool deleted = processor.deleteStoredSceneSlotState(mainPresetIndex, sceneSlot)
        && processor.persistStoredSceneSlotStatesToMainPreset(mainPresetIndex);
    if (deleted)
    {
        processor.clearSceneClipSlotRuntimeState(mainPresetIndex, sceneSlot);
        if (processor.activeSceneMainPresetIndex == juce::jlimit(0, MlrVSTAudioProcessor::MaxPresetSlots - 1, mainPresetIndex))
            processor.clearScenePerformanceClip(sceneSlot);
        processor.presetRefreshToken.fetch_add(1, std::memory_order_acq_rel);
    }
    return deleted;
}

bool SceneScheduler::captureSceneSlot(MlrVSTAudioProcessor& processor, int sceneSlot)
{
    const int clampedSlot = juce::jlimit(0, MlrVSTAudioProcessor::SceneSlots - 1, sceneSlot);
    const int mainPresetIndex = processor.getActiveMainPresetIndexForScenes();
    processor.activeSceneMainPresetIndex = mainPresetIndex;

    const bool saved = saveSceneForMainPreset(processor, mainPresetIndex, clampedSlot);
    if (saved
        && processor.activeSceneMainPresetIndex == mainPresetIndex
        && processor.activeSceneSlot == clampedSlot)
    {
        processor.activeSceneNeedsCaptureBeforeManualRecall = false;
    }
    if (saved)
        processor.updateMonomeLEDs();
    return saved;
}

bool SceneScheduler::insertSceneSlot(MlrVSTAudioProcessor& processor, int sceneSlot, bool insertAfter)
{
    const int selectedSlot = juce::jlimit(0, MlrVSTAudioProcessor::SceneSlots - 1, sceneSlot);
    const int insertSlot = insertAfter ? (selectedSlot + 1) : selectedSlot;
    if (insertSlot < 0 || insertSlot >= MlrVSTAudioProcessor::SceneSlots)
        return false;

    const int mainPresetIndex = processor.getActiveMainPresetIndexForScenes();
    processor.activeSceneMainPresetIndex = mainPresetIndex;

    bool storageChanged = false;
    for (int destSlot = MlrVSTAudioProcessor::SceneSlots - 1; destSlot > insertSlot; --destSlot)
    {
        if (!copySceneForMainPreset(processor, mainPresetIndex, destSlot - 1, destSlot))
            return false;

        setSceneRepeatCount(processor, destSlot, getSceneRepeatCount(processor, destSlot - 1));
        setSceneLengthModeIndex(processor, destSlot, getSceneLengthModeIndex(processor, destSlot - 1));
        setSceneManualBars(processor, destSlot, getSceneManualBars(processor, destSlot - 1));
        setSceneAnchorStrip(processor, destSlot, getSceneAnchorStrip(processor, destSlot - 1));
        storageChanged = true;
    }

    const bool captured = captureSceneSlot(processor, insertSlot);
    if (captured)
    {
        setSceneRepeatCount(processor, insertSlot, 1);
        setSceneLengthModeIndex(processor,
                                insertSlot,
                                static_cast<int>(MlrVSTAudioProcessor::SceneLengthMode::ManualBars));
        setSceneManualBars(processor, insertSlot, 4);
        setSceneAnchorStrip(processor, insertSlot, 0);

        auto& insertedState = processor.storedSceneSlotStates[static_cast<size_t>(insertSlot)];
        if (insertedState.preparedSwitchPayloadTemplate != nullptr)
        {
            auto& timingState = insertedState.preparedSwitchPayloadTemplate->sceneTimingState;
            timingState.valid = true;
            timingState.repeatCount = 1;
            timingState.lengthModeIndex = static_cast<int>(MlrVSTAudioProcessor::SceneLengthMode::ManualBars);
            timingState.manualBars = 4;
            timingState.anchorStrip = 0;
            insertedState.preparedSwitchPayloadTemplate->snapshotPresetXml.reset();
        }

        if (!processor.persistStoredSceneSlotStatesToMainPreset(mainPresetIndex))
            return false;

        processor.presetRefreshToken.fetch_add(1, std::memory_order_acq_rel);
        processor.updateMonomeLEDs();
        return true;
    }

    if (!captured && storageChanged)
    {
        processor.presetRefreshToken.fetch_add(1, std::memory_order_acq_rel);
        processor.updateMonomeLEDs();
    }

    return captured;
}

void SceneScheduler::requestSceneRecallQuantized(MlrVSTAudioProcessor& processor,
                                                 int mainPresetIndex,
                                                 int sceneSlot,
                                                 bool sequenceDriven,
                                                 int sequenceStepIndex,
                                                 bool useTriggerQuantization)
{
    if (!processor.audioEngine)
        return;

    const int clampedMainPresetIndex = juce::jlimit(0, MlrVSTAudioProcessor::MaxPresetSlots - 1, mainPresetIndex);
    const int clampedSceneSlot = juce::jlimit(0, MlrVSTAudioProcessor::SceneSlots - 1, sceneSlot);
    const bool manualSurfaceLaunch = !sequenceDriven && useTriggerQuantization;
    const bool directManualRecall = !manualSurfaceLaunch
        && !sequenceDriven
        && getSceneRecallModeIndex(processor) == static_cast<int>(SceneRecallMode::Manual);
    const bool hostTransportPlaying = processor.isHostTransportPlaying();
    double hostPpqSnapshot = std::numeric_limits<double>::quiet_NaN();
    double hostTempoSnapshot = std::numeric_limits<double>::quiet_NaN();
    bool hasTimingReference = processor.getHostSyncSnapshot(hostPpqSnapshot, hostTempoSnapshot);
    if (!hasTimingReference && processor.audioEngine != nullptr)
    {
        const double fallbackPpq = processor.audioEngine->getTimelineBeat();
        const double fallbackTempo = juce::jmax(1.0, processor.audioEngine->getCurrentTempo());
        if (std::isfinite(fallbackPpq) && std::isfinite(fallbackTempo) && fallbackTempo > 0.0)
        {
            hostPpqSnapshot = fallbackPpq;
            hostTempoSnapshot = fallbackTempo;
            hasTimingReference = true;
        }
    }
    const bool immediateRecall = !sequenceDriven
        && (!hostTransportPlaying || directManualRecall || (manualSurfaceLaunch && !hasTimingReference));
    appendSceneDebugLog("request_recall slot=" + juce::String(clampedSceneSlot)
        + " mainPreset=" + juce::String(clampedMainPresetIndex)
        + " sequenceDriven=" + juce::String(sequenceDriven ? 1 : 0)
        + " triggerQuantized=" + juce::String(useTriggerQuantization ? 1 : 0)
        + " hostPlaying=" + juce::String(hostTransportPlaying ? 1 : 0)
        + " immediate=" + juce::String(immediateRecall ? 1 : 0));

    processor.pendingSceneRecall.active = !immediateRecall;
    processor.pendingSceneRecall.sequenceDriven = sequenceDriven;
    processor.pendingSceneRecall.useTriggerQuantization = manualSurfaceLaunch;
    processor.pendingSceneRecall.targetResolved = false;
    processor.pendingSceneRecall.mainPresetIndex = clampedMainPresetIndex;
    processor.pendingSceneRecall.sceneSlot = clampedSceneSlot;
    processor.pendingSceneRecall.sequenceStepIndex = sequenceDriven ? sequenceStepIndex : -1;
    processor.pendingSceneRecall.targetPpq = 0.0;
    processor.pendingSceneRecall.intervalBeats = 4.0;
    processor.pendingSceneRecall.patternEndPhaseSignatureValid = false;
    processor.pendingSceneRecall.patternEndPhaseSignature = 0;
    if (!sequenceDriven)
        processor.sceneSequenceStartPpqValid = false;
    if (!sequenceDriven)
    {
        processor.clearSceneBoundaryTransitionState();
        processor.clearSceneChainReturnOverride();
    }

    if (immediateRecall)
    {
        MlrVSTAudioProcessor::SceneSwitchEvent switchEvent;
        switchEvent.active = true;
        switchEvent.owner = sequenceDriven
            ? MlrVSTAudioProcessor::ScenePlaybackOwner::Chain
            : MlrVSTAudioProcessor::ScenePlaybackOwner::Manual;
        switchEvent.outgoingOwner = processor.scenePlaybackOwner;
        switchEvent.sequenceDriven = sequenceDriven;
        switchEvent.mainPresetIndex = clampedMainPresetIndex;
        switchEvent.sceneSlot = clampedSceneSlot;
        switchEvent.sequenceStepIndex = sequenceDriven ? sequenceStepIndex : -1;
        switchEvent.targetPpq = hasTimingReference ? hostPpqSnapshot : -1.0;
        switchEvent.targetTempo = hasTimingReference ? hostTempoSnapshot : 120.0;
        switchEvent.targetGlobalSample = processor.audioEngine->getGlobalSampleCount();
        switchEvent.blockStartSample = switchEvent.targetGlobalSample;
        switchEvent.blockNumSamples = 0;
        switchEvent.targetSampleOffsetInBlock = 0;
        switchEvent.boundaryCaptureSampleOffsetInBlock = -1;
        switchEvent.exactBlockBoundary = true;
        switchEvent.splitSwitchInBlock = false;
        switchEvent.legatoOwnerSwitch = false;
        switchEvent.serial = processor.queuePendingSceneApplyState(switchEvent);
        appendSceneDebugLog("request_recall immediate_apply slot=" + juce::String(clampedSceneSlot)
            + " mainPreset=" + juce::String(clampedMainPresetIndex)
            + " hasTiming=" + juce::String(hasTimingReference ? 1 : 0));

        if (shouldPrepareSceneSwitchContexts(processor)
            && processor.sceneSlotExistsForMainPreset(clampedMainPresetIndex, clampedSceneSlot))
        {
            processor.requestScenePreload(clampedMainPresetIndex,
                                          clampedSceneSlot,
                                          sequenceDriven,
                                          sequenceDriven ? sequenceStepIndex : -1,
                                          hasTimingReference ? hostPpqSnapshot : -1.0,
                                          hasTimingReference ? hostTempoSnapshot : 120.0,
                                          processor.audioEngine->getGlobalSampleCount(),
                                          SceneChainTransitionType::None,
                                          switchEvent.serial);
            if (!sequenceDriven)
                processor.servicePendingScenePreloadRequest();
        }
    }

    processor.refreshUtilityTimerCadence();
}

double SceneScheduler::getSceneRecallIntervalBeats(const MlrVSTAudioProcessor& processor)
{
    const int quantizeDivision = juce::jmax(1, processor.getQuantizeDivision());
    return juce::jlimit(1.0 / 64.0, 256.0, 4.0 / static_cast<double>(quantizeDivision));
}

namespace
{
double getSceneLaunchBarLengthBeats(const juce::AudioPlayHead::PositionInfo& posInfo)
{
    if (auto timeSig = posInfo.getTimeSignature())
    {
        const int numerator = juce::jmax(1, timeSig->numerator);
        const int denominator = juce::jmax(1, timeSig->denominator);
        const double beatsPerBar = static_cast<double>(numerator) * (4.0 / static_cast<double>(denominator));
        if (std::isfinite(beatsPerBar) && beatsPerBar > 0.0)
            return beatsPerBar;
    }

    return 4.0;
}

} // namespace

void SceneScheduler::updateSceneQuantizedRecall(MlrVSTAudioProcessor& processor,
                                                const juce::AudioPlayHead::PositionInfo& posInfo,
                                                int numSamples)
{
    if (!processor.pendingSceneRecall.active)
        return;

    if (processor.pendingSceneRecall.sequenceDriven
        && (!processor.sceneSequenceActive || getSceneChainLengthInternal(processor) < 2))
    {
        processor.pendingSceneRecall.active = false;
        processor.pendingSceneRecall.targetResolved = false;
        processor.clearSceneBoundaryTransitionState();
        return;
    }

    MlrVSTAudioProcessor::SceneSwitchEvent pendingSwitchEvent;
    if (processor.peekPendingSceneApplyState(pendingSwitchEvent))
        return;

    if (!posInfo.getIsPlaying())
    {
        processor.pendingSceneRecall.targetResolved = false;
        processor.pendingSceneRecall.patternEndPhaseSignatureValid = false;
        processor.pendingSceneRecall.patternEndPhaseSignature = 0;
        processor.clearSceneBoundaryTransitionState();
        return;
    }

    double currentPpq = std::numeric_limits<double>::quiet_NaN();
    double currentTempo = std::numeric_limits<double>::quiet_NaN();
    bool hasTimingReference = false;
    const auto ppqOpt = posInfo.getPpqPosition();
    const auto bpmOpt = posInfo.getBpm();
    if (ppqOpt.hasValue() && bpmOpt.hasValue()
        && std::isfinite(*ppqOpt) && std::isfinite(*bpmOpt)
        && *bpmOpt > 0.0)
    {
        currentPpq = *ppqOpt;
        currentTempo = *bpmOpt;
        hasTimingReference = true;
    }
    else if (processor.audioEngine != nullptr)
    {
        const double fallbackPpq = processor.audioEngine->getTimelineBeat();
        const double fallbackTempo = juce::jmax(1.0, processor.audioEngine->getCurrentTempo());
        if (std::isfinite(fallbackPpq) && std::isfinite(fallbackTempo) && fallbackTempo > 0.0)
        {
            currentPpq = fallbackPpq;
            currentTempo = fallbackTempo;
            hasTimingReference = true;
        }
    }

    if (!hasTimingReference || processor.currentSampleRate <= 1.0)
    {
        return;
    }
    const auto resolveLaunchQuantisation = [&]() -> MlrVSTAudioProcessor::SceneLaunchQuantisation
    {
        MlrVSTAudioProcessor::SceneLaunchQuantisation launch;
        launch.owner = processor.pendingSceneRecall.sequenceDriven
            ? MlrVSTAudioProcessor::ScenePlaybackOwner::Chain
            : MlrVSTAudioProcessor::ScenePlaybackOwner::Manual;
        launch.outgoingOwner = processor.scenePlaybackOwner;
        launch.sequenceDriven = processor.pendingSceneRecall.sequenceDriven;
        launch.surfaceQuantizedLaunch = processor.pendingSceneRecall.useTriggerQuantization;
        launch.mainPresetIndex = processor.pendingSceneRecall.mainPresetIndex;
        launch.sceneSlot = processor.pendingSceneRecall.sceneSlot;
        launch.sequenceStepIndex = processor.pendingSceneRecall.sequenceStepIndex;
        launch.currentPpq = currentPpq;
        launch.currentTempo = currentTempo;
        launch.blockStartSample = processor.audioEngine != nullptr
            ? processor.audioEngine->getGlobalSampleCount()
            : -1;
        launch.blockNumSamples = juce::jmax(0, numSamples);

        const bool useTriggerGridTiming = launch.surfaceQuantizedLaunch;
        launch.recallMode = useTriggerGridTiming
            ? SceneRecallMode::QuantizeGrid
            : sanitizeSceneRecallMode(SceneScheduler::getSceneRecallModeIndex(processor));

        const bool patternEndEnabled = launch.recallMode == SceneRecallMode::PatternEnd;
        const bool sceneEndEnabled = launch.recallMode == SceneRecallMode::SceneEnd;
        const bool manualSceneChange = launch.recallMode == SceneRecallMode::Manual;
        const bool sequenceUsesSceneLengthTiming = launch.sequenceDriven
            && (sceneEndEnabled || manualSceneChange);
        const int currentSceneSlot = juce::jlimit(0, MlrVSTAudioProcessor::SceneSlots - 1, processor.activeSceneSlot);
        const double currentSceneDurationBeats = juce::jlimit(
            0.25,
            4096.0,
            SceneScheduler::computeCurrentSceneSequenceLengthBeats(processor));
        uint64_t phaseAlignedPatternEndSignature = 0;
        const double phaseAlignedPatternEndPpq = patternEndEnabled
            ? SceneScheduler::computeNextScenePatternEndPpq(
                  processor,
                  currentSceneSlot,
                  currentPpq,
                  currentSceneDurationBeats,
                  &phaseAlignedPatternEndSignature)
            : std::numeric_limits<double>::quiet_NaN();
        const bool phaseAlignedTimingReady = patternEndEnabled && std::isfinite(phaseAlignedPatternEndPpq);
        const bool sequenceTimingReady = sequenceUsesSceneLengthTiming
            && processor.activeScenePlaybackHandle.active
            && processor.activeScenePlaybackHandle.sequenceDriven
            && std::isfinite(processor.activeScenePlaybackHandle.startPpq);
        const bool manualSceneEndTimingReady = sceneEndEnabled
            && !launch.sequenceDriven
            && processor.activeScenePlaybackHandle.active
            && std::isfinite(processor.activeScenePlaybackHandle.startPpq);
        launch.useSceneDurationTiming = phaseAlignedTimingReady || sequenceTimingReady || manualSceneEndTimingReady;

        const double manualLaunchBarLengthBeats = launch.surfaceQuantizedLaunch
            ? getSceneLaunchBarLengthBeats(posInfo)
            : 4.0;
        double intervalBeatsNow = launch.surfaceQuantizedLaunch
            ? manualLaunchBarLengthBeats
            : SceneScheduler::getSceneRecallIntervalBeats(processor);
        if (launch.useSceneDurationTiming)
            intervalBeatsNow = currentSceneDurationBeats;
        launch.intervalBeats = intervalBeatsNow;

        if (processor.pendingSceneRecall.targetResolved
            && std::abs(intervalBeatsNow - processor.pendingSceneRecall.intervalBeats) > 1.0e-9)
        {
            processor.pendingSceneRecall.targetResolved = false;
            processor.pendingSceneRecall.targetPpq = 0.0;
        }

        if (patternEndEnabled)
        {
            if (phaseAlignedTimingReady)
            {
                if (!processor.pendingSceneRecall.patternEndPhaseSignatureValid
                    || processor.pendingSceneRecall.patternEndPhaseSignature != phaseAlignedPatternEndSignature)
                {
                    processor.pendingSceneRecall.targetResolved = false;
                    processor.pendingSceneRecall.targetPpq = 0.0;
                }

                processor.pendingSceneRecall.patternEndPhaseSignatureValid = true;
                processor.pendingSceneRecall.patternEndPhaseSignature = phaseAlignedPatternEndSignature;
            }
            else if (processor.pendingSceneRecall.patternEndPhaseSignatureValid)
            {
                processor.pendingSceneRecall.patternEndPhaseSignatureValid = false;
                processor.pendingSceneRecall.patternEndPhaseSignature = 0;
                processor.pendingSceneRecall.targetResolved = false;
                processor.pendingSceneRecall.targetPpq = 0.0;
            }

            if (!phaseAlignedTimingReady)
            {
                processor.pendingSceneRecall.targetResolved = false;
                processor.pendingSceneRecall.targetPpq = 0.0;
                processor.clearSceneBoundaryTransitionState();
                return launch;
            }
        }
        else if (processor.pendingSceneRecall.patternEndPhaseSignatureValid)
        {
            processor.pendingSceneRecall.patternEndPhaseSignatureValid = false;
            processor.pendingSceneRecall.patternEndPhaseSignature = 0;
        }

        if (!processor.pendingSceneRecall.targetResolved)
        {
            processor.pendingSceneRecall.intervalBeats = intervalBeatsNow;
            if (manualSceneChange && !launch.sequenceDriven)
            {
                processor.pendingSceneRecall.targetPpq = currentPpq;
            }
            else if (launch.surfaceQuantizedLaunch)
            {
                double nextBarBoundary = std::ceil(currentPpq / intervalBeatsNow) * intervalBeatsNow;
                if (nextBarBoundary <= currentPpq + 1.0e-9)
                    nextBarBoundary += intervalBeatsNow;
                processor.pendingSceneRecall.targetPpq =
                    std::round(nextBarBoundary / intervalBeatsNow) * intervalBeatsNow;
            }
            else if (phaseAlignedTimingReady)
            {
                processor.pendingSceneRecall.targetPpq = phaseAlignedPatternEndPpq;
            }
            else if (launch.useSceneDurationTiming)
            {
                const double sceneStartReference = processor.activeScenePlaybackHandle.startPpq;
                double nextTarget = sceneStartReference + intervalBeatsNow;
                if (nextTarget <= currentPpq + 1.0e-9)
                {
                    const double elapsed = juce::jmax(0.0, currentPpq - sceneStartReference);
                    const double completedCycles = std::floor(elapsed / juce::jmax(1.0e-9, intervalBeatsNow));
                    nextTarget = sceneStartReference + ((completedCycles + 1.0) * intervalBeatsNow);
                }
                processor.pendingSceneRecall.targetPpq = nextTarget;
            }
            else
            {
                double nextBoundary = std::ceil(currentPpq / intervalBeatsNow) * intervalBeatsNow;
                if (nextBoundary <= currentPpq + 1.0e-9)
                    nextBoundary += intervalBeatsNow;
                processor.pendingSceneRecall.targetPpq =
                    std::round(nextBoundary / intervalBeatsNow) * intervalBeatsNow;
            }
            processor.pendingSceneRecall.targetResolved = true;
        }

        launch.targetResolved = processor.pendingSceneRecall.targetResolved;
        launch.targetPpq = processor.pendingSceneRecall.targetPpq;

        const double ppqPerSecond = currentTempo / 60.0;
        const double ppqPerSample = ppqPerSecond / processor.currentSampleRate;
        launch.blockEndPpq = currentPpq + (ppqPerSample * static_cast<double>(juce::jmax(1, numSamples)));

        if (!launch.targetResolved || !std::isfinite(launch.targetPpq))
            return launch;

        if (launch.blockEndPpq + 1.0e-9 < launch.targetPpq)
            return launch;

        launch.targetWithinCurrentBlock = true;
        if (launch.blockStartSample >= 0)
        {
            const double blockSpanPpq = juce::jmax(1.0e-12, launch.blockEndPpq - currentPpq);
            const double progress = juce::jlimit(0.0,
                                                 1.0,
                                                 (launch.targetPpq - currentPpq) / blockSpanPpq);
            launch.targetSampleOffsetInBlock = juce::jlimit(
                0,
                juce::jmax(0, numSamples),
                static_cast<int>(std::llround(progress * static_cast<double>(numSamples))));
            launch.boundaryCaptureSampleOffsetInBlock = launch.targetSampleOffsetInBlock > 0
                ? (launch.targetSampleOffsetInBlock - 1)
                : -1;
            launch.exactBlockBoundary = launch.targetSampleOffsetInBlock == 0
                || launch.targetSampleOffsetInBlock == numSamples;
            launch.splitSwitchInBlock = launch.targetSampleOffsetInBlock > 0
                && launch.targetSampleOffsetInBlock < numSamples;
            launch.targetGlobalSample = launch.blockStartSample + static_cast<int64_t>(launch.targetSampleOffsetInBlock);
        }
        else
        {
            launch.targetGlobalSample = -1;
        }

        launch.legatoOwnerSwitch = launch.owner != launch.outgoingOwner
            && processor.activeSceneMainPresetIndex == launch.mainPresetIndex
            && processor.activeSceneSlot == launch.sceneSlot;

        return launch;
    };

    const auto launchQuantisation = resolveLaunchQuantisation();

    const int chainLength = getSceneChainLengthInternal(processor);
    const bool sequenceDrivenHandoff = processor.pendingSceneRecall.sequenceDriven && chainLength >= 2;
    const int outgoingStepIndex = sequenceDrivenHandoff && processor.sceneSequenceCurrentStepIndex >= 0
        ? juce::jlimit(0, juce::jmax(0, chainLength - 1), processor.sceneSequenceCurrentStepIndex)
        : -1;
    const auto outgoingTransitionType =
        (sequenceDrivenHandoff && outgoingStepIndex >= 0)
            ? sanitizeSceneChainTransitionType(
                  processor.sceneChainState.steps[static_cast<size_t>(outgoingStepIndex)].transitionToNext)
            : SceneChainTransitionType::None;
    const auto outgoingTransitionOption =
        (sequenceDrivenHandoff && outgoingStepIndex >= 0)
            ? sanitizeSceneChainTransitionOption(
                  processor.sceneChainState.steps[static_cast<size_t>(outgoingStepIndex)].transitionOption)
            : SceneChainTransitionOption::Default;
    const auto outgoingTransitionScope =
        (sequenceDrivenHandoff && outgoingStepIndex >= 0)
            ? sanitizeSceneChainTransitionScope(
                  processor.sceneChainState.steps[static_cast<size_t>(outgoingStepIndex)].transitionScope)
            : SceneChainTransitionScope::All;
    const auto outgoingTransitionContour =
        (sequenceDrivenHandoff && outgoingStepIndex >= 0)
            ? sanitizeSceneChainTransitionContour(
                  processor.sceneChainState.steps[static_cast<size_t>(outgoingStepIndex)].transitionContour)
            : SceneChainTransitionContour::Smooth;
    const auto outgoingTransitionCondition =
        (sequenceDrivenHandoff && outgoingStepIndex >= 0)
            ? sanitizeSceneChainTransitionCondition(
                  processor.sceneChainState.steps[static_cast<size_t>(outgoingStepIndex)].transitionCondition)
            : SceneChainTransitionCondition::Always;
    const double targetPpq = launchQuantisation.targetPpq;
    const int64_t targetGlobalSample = launchQuantisation.targetGlobalSample;
    const bool isLoopbackHandoff = sequenceDrivenHandoff
        && processor.sceneChainState.loopEnabled
        && processor.pendingSceneRecall.sequenceStepIndex >= 0
        && processor.pendingSceneRecall.sequenceStepIndex <= outgoingStepIndex;
    const bool shouldApplyBoundaryTransition = sequenceDrivenHandoff
        && outgoingStepIndex >= 0
        && outgoingTransitionType != SceneChainTransitionType::None
        && outgoingTransitionType != SceneChainTransitionType::Return
        && sceneChainTransitionConditionAllows(outgoingTransitionCondition,
                                               outgoingStepIndex,
                                               processor.pendingSceneRecall.sequenceStepIndex,
                                               targetPpq,
                                               isLoopbackHandoff);
    const auto preloadTransitionType = shouldApplyBoundaryTransition
        ? outgoingTransitionType
        : SceneChainTransitionType::None;
    const double beatsUntilTarget = (std::isfinite(targetPpq) && std::isfinite(currentPpq))
        ? (targetPpq - currentPpq)
        : std::numeric_limits<double>::infinity();
    const bool preloadWindowReached = launchQuantisation.targetWithinCurrentBlock
        || beatsUntilTarget <= scenePreloadLookaheadBeats(currentTempo);

    if (launchQuantisation.targetResolved
        && std::isfinite(targetPpq)
        && preloadWindowReached
        && shouldPrepareSceneSwitchContexts(processor)
        && processor.sceneSlotExistsForMainPreset(processor.pendingSceneRecall.mainPresetIndex,
                                                  processor.pendingSceneRecall.sceneSlot))
    {
        processor.requestScenePreload(processor.pendingSceneRecall.mainPresetIndex,
                                      processor.pendingSceneRecall.sceneSlot,
                                      processor.pendingSceneRecall.sequenceDriven,
                                      processor.pendingSceneRecall.sequenceDriven
                                          ? processor.pendingSceneRecall.sequenceStepIndex
                                          : -1,
                                      targetPpq,
                                      currentTempo,
                                      targetGlobalSample,
                                      preloadTransitionType);
    }

    if (shouldApplyBoundaryTransition && std::isfinite(targetPpq))
    {
        const double nextBarDelayBeats = getSceneLaunchBarLengthBeats(posInfo);
        processor.armSceneBoundaryTransition(outgoingTransitionType,
                                             outgoingTransitionOption,
                                             outgoingTransitionScope,
                                             outgoingTransitionContour,
                                             processor.sceneChainState.steps[static_cast<size_t>(outgoingStepIndex)].transitionLengthBeats,
                                             processor.sceneChainState.steps[static_cast<size_t>(outgoingStepIndex)].transitionIntensity,
                                             processor.sceneChainState.steps[static_cast<size_t>(outgoingStepIndex)].transitionDelayAmount,
                                             processor.sceneChainState.steps[static_cast<size_t>(outgoingStepIndex)].transitionFilterAmount,
                                             processor.sceneChainState.steps[static_cast<size_t>(outgoingStepIndex)].transitionChopAmount,
                                             outgoingStepIndex,
                                             processor.pendingSceneRecall.sequenceStepIndex,
                                             targetPpq,
                                             currentTempo,
                                             sceneChainTransitionLeadBeats(outgoingTransitionType,
                                                                           processor.sceneChainState.steps[static_cast<size_t>(outgoingStepIndex)].transitionLengthBeats),
                                             targetGlobalSample,
                                             nextBarDelayBeats);
    }
    else
    {
        processor.clearSceneBoundaryTransitionState();
    }

    if (!launchQuantisation.targetResolved || !launchQuantisation.targetWithinCurrentBlock)
        return;

    MlrVSTAudioProcessor::SceneSwitchEvent switchEvent;
    switchEvent.active = true;
    switchEvent.owner = processor.pendingSceneRecall.sequenceDriven
        ? MlrVSTAudioProcessor::ScenePlaybackOwner::Chain
        : MlrVSTAudioProcessor::ScenePlaybackOwner::Manual;
    switchEvent.outgoingOwner = launchQuantisation.outgoingOwner;
    switchEvent.sequenceDriven = processor.pendingSceneRecall.sequenceDriven;
    switchEvent.splitSwitchInBlock = launchQuantisation.splitSwitchInBlock;
    switchEvent.legatoOwnerSwitch = launchQuantisation.legatoOwnerSwitch;
    switchEvent.mainPresetIndex = processor.pendingSceneRecall.mainPresetIndex;
    switchEvent.sceneSlot = processor.pendingSceneRecall.sceneSlot;
    switchEvent.sequenceStepIndex = processor.pendingSceneRecall.sequenceDriven
        ? processor.pendingSceneRecall.sequenceStepIndex
        : -1;
    switchEvent.targetPpq = targetPpq;
    switchEvent.targetTempo = currentTempo;
    switchEvent.targetGlobalSample = targetGlobalSample;
    switchEvent.blockStartSample = launchQuantisation.blockStartSample;
    switchEvent.blockNumSamples = launchQuantisation.blockNumSamples;
    switchEvent.targetSampleOffsetInBlock = launchQuantisation.targetSampleOffsetInBlock;
    switchEvent.boundaryCaptureSampleOffsetInBlock = launchQuantisation.boundaryCaptureSampleOffsetInBlock;
    switchEvent.exactBlockBoundary = launchQuantisation.exactBlockBoundary;
    processor.queuePendingSceneApplyState(switchEvent);
    processor.pendingSceneRecall.active = false;
    processor.pendingSceneRecall.targetResolved = false;
}

void SceneScheduler::processPendingSceneApply(MlrVSTAudioProcessor& processor)
{
    MlrVSTAudioProcessor::SceneSwitchEvent pendingSwitchEvent;
    if (!processor.peekPendingSceneApplyState(pendingSwitchEvent))
        return;

    const bool hostTransportRunning = processor.isHostTransportPlaying();
    const bool targetSceneExistsForPendingEvent = processor.sceneSlotExistsForMainPreset(
        pendingSwitchEvent.mainPresetIndex,
        pendingSwitchEvent.sceneSlot);
    if (targetSceneExistsForPendingEvent
        && !processor.hasPreparedSceneSwitchPayloadForEvent(pendingSwitchEvent))
    {
        processor.requestScenePreload(pendingSwitchEvent.mainPresetIndex,
                                      pendingSwitchEvent.sceneSlot,
                                      pendingSwitchEvent.sequenceDriven,
                                      pendingSwitchEvent.sequenceDriven
                                          ? pendingSwitchEvent.sequenceStepIndex
                                          : -1,
                                      pendingSwitchEvent.targetPpq,
                                      pendingSwitchEvent.targetTempo,
                                      pendingSwitchEvent.targetGlobalSample,
                                      SceneChainTransitionType::None,
                                      pendingSwitchEvent.serial);
        processor.servicePendingScenePreloadRequest();
    }

    if (targetSceneExistsForPendingEvent
        && hostTransportRunning)
    {
        return;
    }

    if (processor.hasPreparedSceneSwitchPayloadForEvent(pendingSwitchEvent)
        && pendingSwitchEvent.sequenceDriven
        && hostTransportRunning)
    {
        return;
    }

    MlrVSTAudioProcessor::SceneSwitchEvent queuedSwitchEvent;
    if (!processor.consumePendingSceneApplyState(queuedSwitchEvent))
        return;

    const int queuedSlot = queuedSwitchEvent.sceneSlot;
    const int queuedMain = queuedSwitchEvent.mainPresetIndex;
    const bool queuedSequenceDriven = queuedSwitchEvent.sequenceDriven;
    const bool queuedOwnerOnlySwitch = queuedSwitchEvent.ownerOnlySwitch;
    const int queuedSequenceStep = queuedSwitchEvent.sequenceStepIndex;
    const double queuedTargetPpq = queuedSwitchEvent.targetPpq;
    const double queuedTargetTempo = queuedSwitchEvent.targetTempo;
    const int64_t queuedTargetSample = queuedSwitchEvent.targetGlobalSample;
    const int64_t queuedBlockStartSample = queuedSwitchEvent.blockStartSample;
    const int queuedTargetSampleOffset = queuedSwitchEvent.targetSampleOffsetInBlock;

    const int clampedMain = juce::jlimit(0, MlrVSTAudioProcessor::MaxPresetSlots - 1, queuedMain);
    const int clampedSlot = juce::jlimit(0, MlrVSTAudioProcessor::SceneSlots - 1, queuedSlot);
    const int preservedFocusedSceneSlot = processor.getFocusedSceneSlot();
    const int focusSceneSlotAfterApply = queuedSequenceDriven
        ? preservedFocusedSceneSlot
        : clampedSlot;
    appendSceneDebugLog("process_apply slot=" + juce::String(clampedSlot)
        + " mainPreset=" + juce::String(clampedMain)
        + " queuedSequence=" + juce::String(queuedSequenceDriven ? 1 : 0)
        + " activeScene=" + juce::String(processor.activeSceneSlot)
        + " activeMain=" + juce::String(processor.activeSceneMainPresetIndex));
    const bool queuedTimingValid = std::isfinite(queuedTargetPpq)
        && std::isfinite(queuedTargetTempo)
        && queuedTargetPpq >= 0.0
        && queuedTargetTempo > 0.0;
    const int chainLength = getSceneChainLengthInternal(processor);
    const int outgoingStepIndex = processor.sceneSequenceCurrentStepIndex;
    const auto outgoingTransitionType =
        (processor.sceneSequenceActive && outgoingStepIndex >= 0 && chainLength > 0)
            ? sanitizeSceneChainTransitionType(
                  processor.sceneChainState.steps[static_cast<size_t>(juce::jlimit(0,
                                                                                  chainLength - 1,
                                                                                  outgoingStepIndex))].transitionToNext)
            : SceneChainTransitionType::None;
    const auto outgoingTransitionCondition =
        (processor.sceneSequenceActive && outgoingStepIndex >= 0 && chainLength > 0)
            ? sanitizeSceneChainTransitionCondition(
                  processor.sceneChainState.steps[static_cast<size_t>(juce::jlimit(0,
                                                                                  chainLength - 1,
                                                                                  outgoingStepIndex))].transitionCondition)
            : SceneChainTransitionCondition::Always;
    const bool effectiveSequenceDriven = queuedSequenceDriven;
    const bool preserveSceneSequence = effectiveSequenceDriven && processor.sceneSequenceActive && chainLength >= 2;
    int effectiveSequenceStep = effectiveSequenceDriven ? queuedSequenceStep : -1;
    if (effectiveSequenceDriven && effectiveSequenceStep < 0)
    {
        if (preserveSceneSequence)
            effectiveSequenceStep = processor.sceneSequenceCurrentStepIndex;
        if (effectiveSequenceStep < 0)
            effectiveSequenceStep = getSceneSequenceStepIndex(processor, clampedSlot);
    }
    const bool hostTransportPlaying = processor.isHostTransportPlaying();
    const int outgoingSceneMainPresetIndex = juce::jlimit(0,
                                                          MlrVSTAudioProcessor::MaxPresetSlots - 1,
                                                          processor.activeSceneMainPresetIndex);
    const bool sceneWillChange = clampedMain != outgoingSceneMainPresetIndex
        || clampedSlot != processor.activeSceneSlot;
    const auto autosaveWouldDropAudio = [&processor]() -> bool
    {
        if (processor.audioEngine == nullptr)
            return false;

        for (int stripIndex = 0; stripIndex < MlrVSTAudioProcessor::MaxStrips; ++stripIndex)
        {
            const auto* strip = processor.audioEngine->getStrip(stripIndex);
            if (strip == nullptr)
                continue;

            const auto playMode = strip->getPlayMode();
            if (!(strip->hasAudio() || playMode == EnhancedAudioStrip::PlayMode::Sample))
                continue;

            const auto& sourceFile = processor.currentStripFiles[static_cast<size_t>(stripIndex)];
            if (isValidSceneStoredSamplePath(sourceFile))
                continue;

            const auto* audioBuffer = strip->getAudioBuffer();
            if (audioBuffer != nullptr && canEmbedSceneAudioBuffer(*audioBuffer))
                continue;

            return true;
        }

        return false;
    };
    const bool shouldFlushPendingActiveAutosave = sceneWillChange
        && processor.pendingActiveSceneAutosaveDirty.load(std::memory_order_acquire) != 0
        && processor.pendingActiveSceneAutosaveMainPreset.load(std::memory_order_acquire) == outgoingSceneMainPresetIndex
        && processor.pendingActiveSceneAutosaveSlot.load(std::memory_order_acquire) == processor.activeSceneSlot;
    const bool shouldCaptureOutgoingUnsavedScene = sceneWillChange
        && processor.activeSceneNeedsCaptureBeforeManualRecall;
    const bool shouldAutosaveOutgoingScene = shouldFlushPendingActiveAutosave
        || shouldCaptureOutgoingUnsavedScene;
    const bool canAutosaveOutgoingScene = shouldAutosaveOutgoingScene && !autosaveWouldDropAudio();
    auto requeuePendingApply = [&]()
    {
        MlrVSTAudioProcessor::SceneSwitchEvent event;
        event.active = true;
        event.owner = queuedSequenceDriven
            ? MlrVSTAudioProcessor::ScenePlaybackOwner::Chain
            : MlrVSTAudioProcessor::ScenePlaybackOwner::Manual;
        event.outgoingOwner = queuedSwitchEvent.outgoingOwner;
        event.sequenceDriven = effectiveSequenceDriven;
        event.ownerOnlySwitch = queuedOwnerOnlySwitch;
        event.splitSwitchInBlock = queuedSwitchEvent.splitSwitchInBlock;
        event.legatoOwnerSwitch = queuedSwitchEvent.legatoOwnerSwitch;
        event.mainPresetIndex = clampedMain;
        event.sceneSlot = clampedSlot;
        event.sequenceStepIndex = effectiveSequenceDriven ? effectiveSequenceStep : -1;
        event.targetPpq = queuedTargetPpq;
        event.targetTempo = queuedTargetTempo;
        event.targetGlobalSample = queuedTargetSample;
        event.blockStartSample = queuedBlockStartSample;
        event.blockNumSamples = queuedSwitchEvent.blockNumSamples;
        event.targetSampleOffsetInBlock = queuedTargetSampleOffset;
        event.boundaryCaptureSampleOffsetInBlock = queuedTargetSampleOffset > 0
            ? (queuedTargetSampleOffset - 1)
            : -1;
        event.exactBlockBoundary = queuedTargetSampleOffset == 0;
        processor.queuePendingSceneApplyState(event);
    };

    if (queuedOwnerOnlySwitch)
    {
        processor.clearSceneBoundaryTransitionState();
        processor.activeSceneMainPresetIndex = clampedMain;
        processor.activeSceneSlot = clampedSlot;
        processor.switchScenePlaybackOwner(queuedSwitchEvent.owner,
                                           effectiveSequenceDriven && chainLength >= 2,
                                           effectiveSequenceDriven ? effectiveSequenceStep : -1);
        const double appliedSceneStartPpq = std::isfinite(queuedTargetPpq)
            ? queuedTargetPpq
            : (processor.activeScenePlaybackHandle.active
                   ? processor.activeScenePlaybackHandle.startPpq
                   : std::numeric_limits<double>::quiet_NaN());
        processor.setActiveScenePlaybackHandle(clampedMain,
                                               clampedSlot,
                                               effectiveSequenceDriven,
                                               effectiveSequenceDriven ? effectiveSequenceStep : -1,
                                               appliedSceneStartPpq,
                                               computeCurrentSceneSequenceLengthBeats(processor));
        processor.setSceneChainAttachStartPpq(appliedSceneStartPpq);
        if (effectiveSequenceDriven)
            armNextSceneInSequence(processor, clampedMain, clampedSlot, appliedSceneStartPpq);
        else
            processor.activeScenePlaybackHandle.sequenceDriven = false;
        processor.presetRefreshToken.fetch_add(1, std::memory_order_acq_rel);
        processor.updateMonomeLEDs();
        return;
    }

    processor.scenePerformanceRecorder.stopRecording();
    processor.clearPendingMonomeSceneRecorderTap();
    processor.clearPendingSceneRecorderAction();
    processor.lastScenePerformanceProcessBeat = std::numeric_limits<double>::quiet_NaN();
    processor.lastScenePerformanceProcessSceneSlot = -1;
    processor.lastScenePerformanceProcessSceneStartBeat = std::numeric_limits<double>::quiet_NaN();

    if (processor.isSceneModeEnabled() && processor.audioEngine != nullptr)
        processor.syncSceneMotionStateFromEngine(processor.activeSceneSlot);

    const int64_t currentGlobalSample = processor.audioEngine != nullptr ? processor.audioEngine->getGlobalSampleCount() : -1;
    if (queuedTargetSample >= 0
        && currentGlobalSample >= 0
        && (hostTransportPlaying || effectiveSequenceDriven)
        && currentGlobalSample + 1 < queuedTargetSample)
    {
        requeuePendingApply();
        return;
    }

    double hostPpqSnapshot = processor.audioEngine ? processor.audioEngine->getTimelineBeat() : 0.0;
    double hostTempoSnapshot = processor.audioEngine ? juce::jmax(1.0, processor.audioEngine->getCurrentTempo()) : 120.0;
    const bool hasHostSync = processor.getHostSyncSnapshot(hostPpqSnapshot, hostTempoSnapshot)
        && std::isfinite(hostPpqSnapshot)
        && std::isfinite(hostTempoSnapshot)
        && hostTempoSnapshot > 0.0;

    if (effectiveSequenceDriven && !hostTransportPlaying)
    {
        requeuePendingApply();
        return;
    }

    if (!queuedTimingValid && !hasHostSync && effectiveSequenceDriven)
    {
        requeuePendingApply();
        return;
    }

    if (shouldAutosaveOutgoingScene && !canAutosaveOutgoingScene)
    {
        DBG("Scene boundary auto-save skipped because the active dirty scene contains volatile strip audio that cannot be persisted safely");
    }

    if (canAutosaveOutgoingScene)
    {
        const bool saved = saveSceneForMainPreset(processor,
                                                  outgoingSceneMainPresetIndex,
                                                  processor.activeSceneSlot);
        if (!saved)
        {
            DBG("Scene boundary auto-save failed while switching from slot "
                << (processor.activeSceneSlot + 1) << " to " << (clampedSlot + 1));
        }
    }

    const bool returnRouteLoopbackHandoff = effectiveSequenceDriven
        && processor.sceneChainState.loopEnabled
        && effectiveSequenceStep >= 0
        && outgoingStepIndex >= 0
        && effectiveSequenceStep <= outgoingStepIndex;
    const double returnRouteConditionPpq = queuedTimingValid
        ? queuedTargetPpq
        : (hasHostSync ? hostPpqSnapshot : std::numeric_limits<double>::quiet_NaN());
    const bool shouldArmReturnRoute = effectiveSequenceDriven
        && outgoingTransitionType == SceneChainTransitionType::Return
        && outgoingStepIndex >= 0
        && effectiveSequenceStep >= 0
        && sceneChainTransitionConditionAllows(outgoingTransitionCondition,
                                               outgoingStepIndex,
                                               effectiveSequenceStep,
                                               returnRouteConditionPpq,
                                               returnRouteLoopbackHandoff);

    if (shouldArmReturnRoute)
    {
        processor.armSceneChainReturnOverride(outgoingStepIndex, effectiveSequenceStep);
    }
    else if (!effectiveSequenceDriven || outgoingTransitionType == SceneChainTransitionType::Return)
    {
        processor.clearSceneChainReturnOverride();
    }

    const double appliedPpq = queuedTimingValid
        ? queuedTargetPpq
        : (hasHostSync ? hostPpqSnapshot : std::numeric_limits<double>::quiet_NaN());
    const double appliedTempo = queuedTimingValid
        ? queuedTargetTempo
        : (hasHostSync ? hostTempoSnapshot : std::numeric_limits<double>::quiet_NaN());
    const int64_t appliedGlobalSample = queuedTargetSample >= 0
        ? queuedTargetSample
        : (currentGlobalSample >= 0
            ? currentGlobalSample
            : (processor.audioEngine != nullptr ? processor.audioEngine->getGlobalSampleCount() : -1));

    const bool targetSceneExists = processor.sceneSlotExistsForMainPreset(clampedMain, clampedSlot);
    appendSceneDebugLog("process_apply target_exists slot=" + juce::String(clampedSlot)
        + " mainPreset=" + juce::String(clampedMain)
        + " exists=" + juce::String(targetSceneExists ? 1 : 0)
        + " sequenceDriven=" + juce::String(effectiveSequenceDriven ? 1 : 0));
    bool recallContinuityBroken = false;
    bool usedPreparedLiveSwitch = false;
    if (targetSceneExists)
    {
        MlrVSTAudioProcessor::SceneSwitchEvent directSwitchEvent;
        directSwitchEvent.active = true;
        directSwitchEvent.owner = effectiveSequenceDriven
            ? MlrVSTAudioProcessor::ScenePlaybackOwner::Chain
            : MlrVSTAudioProcessor::ScenePlaybackOwner::Manual;
        directSwitchEvent.outgoingOwner = processor.scenePlaybackOwner;
        directSwitchEvent.sequenceDriven = effectiveSequenceDriven;
        directSwitchEvent.mainPresetIndex = clampedMain;
        directSwitchEvent.sceneSlot = clampedSlot;
        directSwitchEvent.sequenceStepIndex = effectiveSequenceDriven ? effectiveSequenceStep : -1;
        directSwitchEvent.targetPpq = appliedPpq;
        directSwitchEvent.targetTempo = appliedTempo;
        directSwitchEvent.targetGlobalSample = appliedGlobalSample;
        directSwitchEvent.blockStartSample = queuedBlockStartSample;
        directSwitchEvent.blockNumSamples = queuedSwitchEvent.blockNumSamples;
        directSwitchEvent.targetSampleOffsetInBlock = queuedTargetSampleOffset;
        directSwitchEvent.boundaryCaptureSampleOffsetInBlock = queuedSwitchEvent.boundaryCaptureSampleOffsetInBlock;
        directSwitchEvent.exactBlockBoundary = queuedSwitchEvent.exactBlockBoundary;
        directSwitchEvent.splitSwitchInBlock = queuedSwitchEvent.splitSwitchInBlock;
        directSwitchEvent.legatoOwnerSwitch = queuedSwitchEvent.legatoOwnerSwitch;

        if (auto* preparedPayload = processor.takePreparedSceneSwitchPayloadForEvent(directSwitchEvent))
        {
            ScopedSuspendProcessing scopedSuspend(processor);
            usedPreparedLiveSwitch =
                processor.applyPreparedSceneSwitchPayload(*preparedPayload,
                                                         directSwitchEvent,
                                                         recallContinuityBroken);
            processor.retirePreparedSceneSwitchPayload(preparedPayload);
        }

        if (!usedPreparedLiveSwitch)
        {
            performSceneLoad(processor,
                             clampedMain,
                             clampedSlot,
                             appliedPpq,
                             appliedTempo,
                             appliedGlobalSample,
                             effectiveSequenceDriven,
                             &recallContinuityBroken);
        }
    }
    else
    {
        if (!effectiveSequenceDriven)
        {
            appendSceneDebugLog("process_apply empty_scene slot=" + juce::String(clampedSlot)
                + " mainPreset=" + juce::String(clampedMain));
            {
                ScopedSceneAutosaveSuppression suppressSceneAutosave(processor);
                processor.scenePerformanceRecorder.clear(clampedSlot);
                processor.syncScenePerformanceClipLengthToResolvedLength(clampedSlot);
                processor.refreshSceneAutomationTargetMask(clampedSlot);
                processor.clearSceneClipSlotRuntimeState(clampedMain, clampedSlot);
            }
            performEmptySceneLoad(processor);
        }
        else
        {
            DBG("Scene chain advance skipped because target scene slot " << (clampedSlot + 1)
                << " for preset " << (clampedMain + 1) << " does not exist");
            processor.switchScenePlaybackOwner(MlrVSTAudioProcessor::ScenePlaybackOwner::Manual, false);
            processor.pendingSceneRecall = {};
            processor.clearPendingSceneApplyState();
            processor.pendingScenePreloadDirty.store(0, std::memory_order_release);
            processor.pendingScenePreloadMainPreset.store(-1, std::memory_order_release);
            processor.pendingScenePreloadSceneSlot.store(-1, std::memory_order_release);
            processor.pendingScenePreloadSequenceDriven.store(0, std::memory_order_release);
            processor.pendingScenePreloadSequenceStep.store(-1, std::memory_order_release);
            processor.pendingScenePreloadTargetPpq.store(-1.0, std::memory_order_release);
            processor.pendingScenePreloadTargetTempo.store(120.0, std::memory_order_release);
            processor.pendingScenePreloadTargetSample.store(-1, std::memory_order_release);
            processor.pendingScenePreloadTransitionType.store(static_cast<int>(SceneChainTransitionType::None),
                                                             std::memory_order_release);
            processor.clearSceneBoundaryTransitionState();
            processor.clearSceneChainReturnOverride();
            processor.presetRefreshToken.fetch_add(1, std::memory_order_acq_rel);
            processor.updateMonomeLEDs();
            return;
        }
    }

    // The fill/transition overlay belongs to the outgoing scene only. On the
    // stable direct-load path, clear it as soon as the new scene has been
    // applied so the incoming scene doesn't spend its first block under the
    // old transition treatment.
    juce::ignoreUnused(recallContinuityBroken);
    processor.clearSceneBoundaryTransitionState();

    int appliedSequenceStepIndex = -1;
    if (effectiveSequenceDriven && chainLength >= 2)
    {
        const int fallbackStepIndex = getSceneSequenceStepIndex(processor, clampedSlot);
        appliedSequenceStepIndex = juce::jlimit(0, chainLength - 1,
                                                juce::jmax(0, effectiveSequenceStep >= 0
                                                                   ? effectiveSequenceStep
                                                                   : fallbackStepIndex));
    }

    processor.activeSceneMainPresetIndex = clampedMain;
    processor.activeSceneSlot = clampedSlot;
    processor.switchScenePlaybackOwner(effectiveSequenceDriven
                                           ? MlrVSTAudioProcessor::ScenePlaybackOwner::Chain
                                           : MlrVSTAudioProcessor::ScenePlaybackOwner::Manual,
                                       effectiveSequenceDriven && chainLength >= 2,
                                       appliedSequenceStepIndex);
    const double appliedSceneStartPpq = std::isfinite(appliedPpq)
        ? appliedPpq
        : std::numeric_limits<double>::quiet_NaN();
    processor.setActiveScenePlaybackHandle(clampedMain,
                                           clampedSlot,
                                           effectiveSequenceDriven,
                                           effectiveSequenceDriven ? appliedSequenceStepIndex : -1,
                                           appliedSceneStartPpq,
                                           computeCurrentSceneSequenceLengthBeats(processor));
    processor.setSceneChainAttachStartPpq(appliedPpq);
    processor.focusSceneSlot(focusSceneSlotAfterApply);
    if (PresetStore::presetExists(clampedMain))
        processor.loadedPresetIndex = clampedMain;
    if (effectiveSequenceDriven)
        armNextSceneInSequence(processor, clampedMain, clampedSlot, appliedPpq);
    else
    {
        processor.activeScenePlaybackHandle.sequenceDriven = false;
        processor.activeScenePlaybackHandle.sequenceStepIndex = -1;
    }
    processor.presetRefreshToken.fetch_add(1, std::memory_order_acq_rel);
    processor.updateMonomeLEDs();
}

void SceneScheduler::performEmptySceneLoad(MlrVSTAudioProcessor& processor)
{
    ScopedSuspendProcessing scopedSuspend(processor);

    const auto preservedScenePadHeld = processor.scenePadHeld;
    const auto preservedScenePadHoldDeleteTriggered = processor.scenePadHoldDeleteTriggered;
    const auto preservedScenePadLaunchConsumed = processor.scenePadLaunchConsumed;
    const auto preservedScenePadPressStartMs = processor.scenePadPressStartMs;
    const auto preservedScenePadActionBurstUntilMs = processor.scenePadActionBurstUntilMs;
    const auto preservedScenePadLastTapMs = processor.scenePadLastTapMs;
    const int preservedSceneCopySourceSlot = processor.sceneCopySourceSlot;
    const int preservedSceneCopyMainPresetIndex = processor.sceneCopyMainPresetIndex;
    processor.resetRuntimePresetStateToDefaults();
    processor.switchScenePlaybackOwner(MlrVSTAudioProcessor::ScenePlaybackOwner::Manual, false);
    processor.setSceneChainAttachStartPpq(std::numeric_limits<double>::quiet_NaN());
    processor.activeSceneNeedsCaptureBeforeManualRecall = false;
    processor.scenePadHeld = preservedScenePadHeld;
    processor.scenePadHoldDeleteTriggered = preservedScenePadHoldDeleteTriggered;
    processor.scenePadLaunchConsumed = preservedScenePadLaunchConsumed;
    processor.scenePadPressStartMs = preservedScenePadPressStartMs;
    processor.scenePadActionBurstUntilMs = preservedScenePadActionBurstUntilMs;
    processor.scenePadLastTapMs = preservedScenePadLastTapMs;
    processor.sceneCopySourceSlot = preservedSceneCopySourceSlot;
    processor.sceneCopyMainPresetIndex = preservedSceneCopyMainPresetIndex;
    processor.sceneRepeatCounts.fill(1);
    processor.sceneLengthModes.fill(static_cast<int>(SceneLengthMode::ManualBars));
    processor.sceneManualBars.fill(4);
    processor.sceneAnchorStrips.fill(0);
    processor.sceneChainState = {};
    sanitizeSceneChainRuntimeState(processor);
    for (auto& f : processor.currentStripFiles)
        f = juce::File();
    juce::ignoreUnused(processor.ensureSceneSlotFallbackState(processor.getActiveMainPresetIndexForScenes(),
                                                             processor.getActiveSceneSlot()));

    syncSceneModeFromParameters(processor);
    if (processor.isSceneModeEnabled())
        clearAllStripGroupsForSceneMode(processor);
}

void SceneScheduler::performSceneLoad(MlrVSTAudioProcessor& processor,
                                      int mainPresetIndex,
                                      int sceneSlot,
                                      double hostPpqSnapshot,
                                      double hostTempoSnapshot,
                                      int64_t hostGlobalSampleSnapshot,
                                      bool preserveLoadedStripAudio,
                                      bool* recallContinuityBrokenOut)
{
    if (!processor.audioEngine)
        return;

    if (!processor.hasStoredSceneSlotState(mainPresetIndex, sceneSlot))
        return;

    ScopedSuspendProcessing scopedSuspend(processor);
    appendSceneDebugLog("perform_scene_load slot=" + juce::String(sceneSlot)
        + " mainPreset=" + juce::String(mainPresetIndex));
    const auto preservedScenePadHeld = processor.scenePadHeld;
    const auto preservedScenePadHoldDeleteTriggered = processor.scenePadHoldDeleteTriggered;
    const auto preservedScenePadLaunchConsumed = processor.scenePadLaunchConsumed;
    const auto preservedScenePadPressStartMs = processor.scenePadPressStartMs;
    const auto preservedScenePadActionBurstUntilMs = processor.scenePadActionBurstUntilMs;
    const auto preservedScenePadLastTapMs = processor.scenePadLastTapMs;
    const int preservedSceneCopySourceSlot = processor.sceneCopySourceSlot;
    const int preservedSceneCopyMainPresetIndex = processor.sceneCopyMainPresetIndex;
    const auto preservedSceneRepeatCounts = processor.sceneRepeatCounts;
    const auto preservedSceneLengthModes = processor.sceneLengthModes;
    const auto preservedSceneManualBars = processor.sceneManualBars;
    const auto preservedSceneAnchorStrips = processor.sceneAnchorStrips;
    const auto preservedSceneChainState = processor.sceneChainState;

    processor.resetRuntimePresetStateToDefaults(preserveLoadedStripAudio);
    processor.scenePadHeld = preservedScenePadHeld;
    processor.scenePadHoldDeleteTriggered = preservedScenePadHoldDeleteTriggered;
    processor.scenePadLaunchConsumed = preservedScenePadLaunchConsumed;
    processor.scenePadPressStartMs = preservedScenePadPressStartMs;
    processor.scenePadActionBurstUntilMs = preservedScenePadActionBurstUntilMs;
    processor.scenePadLastTapMs = preservedScenePadLastTapMs;
    processor.sceneCopySourceSlot = preservedSceneCopySourceSlot;
    processor.sceneCopyMainPresetIndex = preservedSceneCopyMainPresetIndex;
    processor.sceneRepeatCounts = preservedSceneRepeatCounts;
    processor.sceneLengthModes = preservedSceneLengthModes;
    processor.sceneManualBars = preservedSceneManualBars;
    processor.sceneAnchorStrips = preservedSceneAnchorStrips;
    processor.sceneChainState = preservedSceneChainState;
    sanitizeSceneChainRuntimeState(processor);

    bool loadSucceeded = false;
    bool recallContinuityBroken = false;
    {
        ScopedSceneAutosaveSuppression suppressSceneAutosave(processor);
        MlrVSTAudioProcessor::PreparedSceneSwitchPayload payload;
        loadSucceeded = processor.buildPreparedSceneSwitchPayload(payload,
                                                                 mainPresetIndex,
                                                                 sceneSlot,
                                                                 false,
                                                                 -1);
        if (loadSucceeded)
        {
            MlrVSTAudioProcessor::SceneSwitchEvent event;
            event.active = true;
            event.owner = processor.scenePlaybackOwner;
            event.outgoingOwner = processor.getRenderedScenePlaybackOwner();
            event.mainPresetIndex = juce::jlimit(0, MlrVSTAudioProcessor::MaxPresetSlots - 1, mainPresetIndex);
            event.sceneSlot = juce::jlimit(0, MlrVSTAudioProcessor::SceneSlots - 1, sceneSlot);
            event.targetPpq = hostPpqSnapshot;
            event.targetTempo = hostTempoSnapshot;
            event.targetGlobalSample = hostGlobalSampleSnapshot;
            loadSucceeded = processor.applyPreparedSceneSwitchPayload(payload, event, recallContinuityBroken);
        }
        juce::ignoreUnused(preserveLoadedStripAudio);
    }

    if (recallContinuityBrokenOut != nullptr)
        *recallContinuityBrokenOut = recallContinuityBroken;

    if (loadSucceeded)
    {
        appendSceneDebugLog("perform_scene_load success slot=" + juce::String(sceneSlot)
            + " mainPreset=" + juce::String(mainPresetIndex));
        processor.setSceneChainAttachStartPpq(hostPpqSnapshot);
        processor.applySceneClipSlotRuntimeState(mainPresetIndex, sceneSlot);
        processor.activeSceneNeedsCaptureBeforeManualRecall =
            processor.getSceneClipSlotState(sceneSlot, mainPresetIndex).liveStripControlDirty;
        processor.resolveLoopPitchRecallStateImmediately();
        processor.syncScenePerformanceClipLengthToResolvedLength(sceneSlot);
        if (processor.isSceneModeEnabled() && processor.audioEngine != nullptr)
            processor.applySceneMotionStateOrDefaultsToEngine(sceneSlot);

        if (std::isfinite(hostPpqSnapshot))
        {
            const int64_t recallSample = hostGlobalSampleSnapshot >= 0
                ? hostGlobalSampleSnapshot
                : processor.audioEngine->getGlobalSampleCount();

            for (int stripIndex = 0; stripIndex < MlrVSTAudioProcessor::MaxStrips; ++stripIndex)
            {
                auto* strip = processor.audioEngine->getStrip(stripIndex);
                if (strip == nullptr || !strip->isPlaying() || !strip->isPpqTimelineAnchored())
                    continue;

                if (strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Step
                    || strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Sample)
                    continue;

                const bool hasStripAudio = (strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Sample)
                    ? processor.hasSampleModeAudio(stripIndex)
                    : strip->hasAudio();
                if (!hasStripAudio)
                    continue;

                // The prepared scene apply path has already restored the strip's
                // saved PPQ anchor. Only realign it to the exact recall sample so
                // playback stays phase-consistent without a second restart.
                strip->realignToPpqAnchor(hostPpqSnapshot, recallSample);
            }
        }

    }
    else
    {
        appendSceneDebugLog("perform_scene_load failed slot=" + juce::String(sceneSlot)
            + " mainPreset=" + juce::String(mainPresetIndex));
    }

    syncSceneModeFromParameters(processor);
    if (processor.isSceneModeEnabled())
        clearAllStripGroupsForSceneMode(processor);
}

void SceneScheduler::appendSceneModeStateToState(const MlrVSTAudioProcessor& processor, juce::ValueTree& state)
{
    auto sceneState = state.getOrCreateChildWithName("SceneModeState", nullptr);
    sceneState.setProperty("enabled", processor.isSceneModeEnabled(), nullptr);
    sceneState.setProperty("activeMainPresetIndex", processor.activeSceneMainPresetIndex, nullptr);
    sceneState.setProperty("activeSceneSlot", processor.activeSceneSlot, nullptr);
    sceneState.setProperty("focusedSceneSlot", processor.getFocusedSceneSlot(), nullptr);
    sceneState.setProperty("sceneLaunchOwner", static_cast<int>(processor.scenePlaybackOwner), nullptr);
    sceneState.setProperty("groupSnapshotValid", processor.sceneModeGroupSnapshot.valid, nullptr);
    sceneState.setProperty("chainStepCount", getSceneChainLength(processor), nullptr);
    sceneState.setProperty("chainLoopEnabled", isSceneChainLoopEnabled(processor), nullptr);
    sceneState.setProperty("chainLoopStart", getSceneChainLoopStartStep(processor), nullptr);
    sceneState.setProperty("chainLoopEnd", getSceneChainLoopEndStep(processor), nullptr);
    sceneState.setProperty("chainTransitionSampleDir",
                           getSceneTransitionEndSampleDirectory(processor).getFullPathName(),
                           nullptr);

    for (int stripIndex = 0; stripIndex < MlrVSTAudioProcessor::MaxStrips; ++stripIndex)
    {
        sceneState.setProperty("stripGroup" + juce::String(stripIndex),
                               processor.sceneModeGroupSnapshot.stripGroups[static_cast<size_t>(stripIndex)],
                               nullptr);
    }

    for (int groupIndex = 0; groupIndex < ModernAudioEngine::MaxGroups; ++groupIndex)
    {
        sceneState.setProperty("groupVolume" + juce::String(groupIndex),
                               processor.sceneModeGroupSnapshot.groupVolumes[static_cast<size_t>(groupIndex)],
                               nullptr);
        sceneState.setProperty("groupMuted" + juce::String(groupIndex),
                               processor.sceneModeGroupSnapshot.groupMuted[static_cast<size_t>(groupIndex)],
                               nullptr);
    }

    for (int sceneSlot = 0; sceneSlot < MlrVSTAudioProcessor::SceneSlots; ++sceneSlot)
    {
        sceneState.setProperty("sceneRepeat" + juce::String(sceneSlot),
                               getSceneRepeatCount(processor, sceneSlot),
                               nullptr);
        sceneState.setProperty("sceneLengthMode" + juce::String(sceneSlot),
                               getSceneLengthModeIndex(processor, sceneSlot),
                               nullptr);
        sceneState.setProperty("sceneManualBars" + juce::String(sceneSlot),
                               getSceneManualBars(processor, sceneSlot),
                               nullptr);
        sceneState.setProperty("sceneAnchorStrip" + juce::String(sceneSlot),
                               getSceneAnchorStrip(processor, sceneSlot),
                               nullptr);
    }

    for (int stepIndex = 0; stepIndex < MlrVSTAudioProcessor::MaxSceneChainSteps; ++stepIndex)
    {
        sceneState.setProperty("chainScene" + juce::String(stepIndex),
                               getSceneChainStepSceneSlot(processor, stepIndex),
                               nullptr);
        sceneState.setProperty("chainRepeats" + juce::String(stepIndex),
                               getSceneChainStepRepeatCount(processor, stepIndex),
                               nullptr);
        sceneState.setProperty("chainTransition" + juce::String(stepIndex),
                               getSceneChainStepTransitionTypeIndex(processor, stepIndex),
                               nullptr);
        sceneState.setProperty("chainTransitionOption" + juce::String(stepIndex),
                               getSceneChainStepTransitionOptionIndex(processor, stepIndex),
                               nullptr);
        sceneState.setProperty("chainTransitionScope" + juce::String(stepIndex),
                               getSceneChainStepTransitionScopeIndex(processor, stepIndex),
                               nullptr);
        sceneState.setProperty("chainTransitionContour" + juce::String(stepIndex),
                               getSceneChainStepTransitionContourIndex(processor, stepIndex),
                               nullptr);
        sceneState.setProperty("chainTransitionCondition" + juce::String(stepIndex),
                               getSceneChainStepTransitionConditionIndex(processor, stepIndex),
                               nullptr);
        sceneState.setProperty("chainTransitionLength" + juce::String(stepIndex),
                               getSceneChainStepTransitionLengthBeats(processor, stepIndex),
                               nullptr);
        sceneState.setProperty("chainTransitionSubtractFromScene" + juce::String(stepIndex),
                               getSceneChainStepTransitionSubtractsFromSceneLength(processor, stepIndex),
                               nullptr);
        sceneState.setProperty("chainTransitionIntensity" + juce::String(stepIndex),
                               getSceneChainStepTransitionIntensity(processor, stepIndex),
                               nullptr);
        sceneState.setProperty("chainTransitionDelay" + juce::String(stepIndex),
                               getSceneChainStepTransitionDelayAmount(processor, stepIndex),
                               nullptr);
        sceneState.setProperty("chainTransitionFilter" + juce::String(stepIndex),
                               getSceneChainStepTransitionFilterAmount(processor, stepIndex),
                               nullptr);
        sceneState.setProperty("chainTransitionChop" + juce::String(stepIndex),
                               getSceneChainStepTransitionChopAmount(processor, stepIndex),
                               nullptr);
        sceneState.setProperty("chainTransitionEndSample" + juce::String(stepIndex),
                               getSceneChainStepTransitionEndSampleFile(processor, stepIndex).getFullPathName(),
                               nullptr);
        const auto endSampleSettings = getSceneChainStepTransitionEndSampleSettings(processor, stepIndex);
        sceneState.setProperty("chainTransitionEndSampleGainDb" + juce::String(stepIndex),
                               endSampleSettings.gainDb,
                               nullptr);
        sceneState.setProperty("chainTransitionEndSampleFadeIn" + juce::String(stepIndex),
                               endSampleSettings.fadeInMs,
                               nullptr);
        sceneState.setProperty("chainTransitionEndSampleFadeOut" + juce::String(stepIndex),
                               endSampleSettings.fadeOutMs,
                               nullptr);
        sceneState.setProperty("chainTransitionEndSampleChoke" + juce::String(stepIndex),
                               endSampleSettings.chokePrevious,
                               nullptr);
        sceneState.setProperty("chainTransitionEndSampleReverse" + juce::String(stepIndex),
                               endSampleSettings.reverse,
                               nullptr);
        sceneState.setProperty("chainTransitionEndSamplePitch" + juce::String(stepIndex),
                               endSampleSettings.pitchSemitones,
                               nullptr);
        sceneState.setProperty("chainTransitionEndSampleLowpass" + juce::String(stepIndex),
                               endSampleSettings.lowpassHz,
                               nullptr);
        sceneState.setProperty("chainTransitionEndSampleHighpass" + juce::String(stepIndex),
                               endSampleSettings.highpassHz,
                               nullptr);
        sceneState.setProperty("chainTransitionEndSampleDuckSource" + juce::String(stepIndex),
                               endSampleSettings.duckSource,
                               nullptr);
        sceneState.setProperty("chainTransitionEndSampleDuckAmount" + juce::String(stepIndex),
                               endSampleSettings.duckAmount,
                               nullptr);
    }

    for (int favoriteIndex = 0; favoriteIndex < MlrVSTAudioProcessor::SceneTransitionFavoriteSlots; ++favoriteIndex)
    {
        const auto& favorite = processor.sceneTransitionFavorites[static_cast<size_t>(favoriteIndex)];
        sceneState.setProperty("chainFillFavoriteValid" + juce::String(favoriteIndex), favorite.valid, nullptr);
        sceneState.setProperty("chainFillFavoriteType" + juce::String(favoriteIndex), static_cast<int>(favorite.type), nullptr);
        sceneState.setProperty("chainFillFavoriteOption" + juce::String(favoriteIndex), static_cast<int>(favorite.option), nullptr);
        sceneState.setProperty("chainFillFavoriteScope" + juce::String(favoriteIndex), static_cast<int>(favorite.scope), nullptr);
        sceneState.setProperty("chainFillFavoriteContour" + juce::String(favoriteIndex), static_cast<int>(favorite.contour), nullptr);
        sceneState.setProperty("chainFillFavoriteCondition" + juce::String(favoriteIndex), static_cast<int>(favorite.condition), nullptr);
        sceneState.setProperty("chainFillFavoriteLength" + juce::String(favoriteIndex), favorite.lengthBeats, nullptr);
        sceneState.setProperty("chainFillFavoriteSubtract" + juce::String(favoriteIndex), favorite.subtractFromSceneLength, nullptr);
        sceneState.setProperty("chainFillFavoriteIntensity" + juce::String(favoriteIndex), favorite.intensity, nullptr);
        sceneState.setProperty("chainFillFavoriteDelay" + juce::String(favoriteIndex), favorite.delayAmount, nullptr);
        sceneState.setProperty("chainFillFavoriteFilter" + juce::String(favoriteIndex), favorite.filterAmount, nullptr);
        sceneState.setProperty("chainFillFavoriteChop" + juce::String(favoriteIndex), favorite.chopAmount, nullptr);
    }

    sceneState.setProperty("scenePerformanceBlob", processor.createScenePerformanceStateData(-1), nullptr);

    auto storedScenesState = sceneState.getChildWithName("StoredScenes");
    if (!storedScenesState.isValid())
    {
        storedScenesState = juce::ValueTree("StoredScenes");
        sceneState.addChild(storedScenesState, -1, nullptr);
    }
    storedScenesState.removeAllChildren(nullptr);

    const int activeMainPresetIndex = processor.getActiveMainPresetIndexForScenes();
    for (int sceneSlot = 0; sceneSlot < MlrVSTAudioProcessor::SceneSlots; ++sceneSlot)
    {
        const auto* storedSceneState = processor.getStoredSceneSlotState(activeMainPresetIndex, sceneSlot);
        if (storedSceneState == nullptr || storedSceneState->preparedSwitchPayloadTemplate == nullptr)
            continue;

        juce::ValueTree storedSceneValue("StoredScene");
        storedSceneValue.setProperty("slot", sceneSlot, nullptr);
        storedSceneValue.setProperty("name", storedSceneState->name, nullptr);
        storedSceneValue.setProperty("implicitMainPresetFallback",
                                     storedSceneState->implicitMainPresetFallback,
                                     nullptr);
        if (auto snapshotXml = processor.createSceneSnapshotPresetXml(
                *storedSceneState->preparedSwitchPayloadTemplate,
                storedSceneState->name))
        {
            storedSceneValue.setProperty("snapshotXml", snapshotXml->toString(), nullptr);
        }
        storedScenesState.addChild(storedSceneValue, -1, nullptr);
    }
}

void SceneScheduler::loadSceneModeStateFromState(MlrVSTAudioProcessor& processor, const juce::ValueTree& state)
{
    processor.sceneModeGroupSnapshot = {};
    processor.sceneModeGroupSnapshot.stripGroups.fill(-1);
    processor.sceneModeGroupSnapshot.groupVolumes.fill(1.0f);
    processor.sceneModeGroupSnapshot.groupMuted.fill(false);
    processor.sceneRepeatCounts.fill(1);
    processor.sceneLengthModes.fill(static_cast<int>(SceneLengthMode::ManualBars));
    processor.sceneManualBars.fill(4);
    processor.sceneAnchorStrips.fill(0);
    processor.sceneChainState = {};
    processor.sceneTransitionFavorites = {};
    processor.activeSceneMainPresetIndex = 0;
    processor.activeSceneSlot = 0;
    processor.focusedSceneSlot = 0;
    processor.switchScenePlaybackOwner(MlrVSTAudioProcessor::ScenePlaybackOwner::Manual, false);
    processor.clearAllSceneClipSlotRuntimeStates();
    processor.pendingSceneRecall = {};
    processor.activeSceneStartPpqValid = false;
    processor.activeSceneStartPpq = 0.0;
    processor.clearPendingSceneApplyState();
    processor.scenePadHeld.fill(false);
    processor.scenePadHoldDeleteTriggered.fill(false);
    processor.scenePadLaunchConsumed.fill(false);
    processor.scenePadPressStartMs.fill(0);
    processor.scenePadActionBurstUntilMs.fill(0);
    processor.scenePadLastTapMs.fill(0);
    processor.sceneCopySourceSlot = -1;
    processor.sceneCopyMainPresetIndex = 0;

    auto sceneState = state.getChildWithName("SceneModeState");
    if (!sceneState.isValid())
        return;

    // Fresh plugin/session loads always start from Scene 1 to avoid reviving a stale
    // previously-active scene slot or queued scene handoff from an earlier session.
    processor.activeSceneMainPresetIndex = 0;
    processor.activeSceneSlot = 0;
    processor.focusedSceneSlot = juce::jlimit(
        0,
        MlrVSTAudioProcessor::SceneSlots - 1,
        static_cast<int>(sceneState.getProperty("focusedSceneSlot", 0)));
    processor.scenePlaybackOwner = static_cast<MlrVSTAudioProcessor::ScenePlaybackOwner>(
        juce::jlimit(0,
                     1,
                     static_cast<int>(sceneState.getProperty(
                         "sceneLaunchOwner",
                         static_cast<int>(MlrVSTAudioProcessor::ScenePlaybackOwner::Manual)))));
    processor.sceneModeGroupSnapshot.valid = static_cast<bool>(sceneState.getProperty("groupSnapshotValid", false));

    for (int stripIndex = 0; stripIndex < MlrVSTAudioProcessor::MaxStrips; ++stripIndex)
    {
        processor.sceneModeGroupSnapshot.stripGroups[static_cast<size_t>(stripIndex)] = static_cast<int>(
            sceneState.getProperty("stripGroup" + juce::String(stripIndex), -1));
    }

    for (int groupIndex = 0; groupIndex < ModernAudioEngine::MaxGroups; ++groupIndex)
    {
        processor.sceneModeGroupSnapshot.groupVolumes[static_cast<size_t>(groupIndex)] = static_cast<float>(
            sceneState.getProperty("groupVolume" + juce::String(groupIndex), 1.0f));
        processor.sceneModeGroupSnapshot.groupMuted[static_cast<size_t>(groupIndex)] = static_cast<bool>(
            sceneState.getProperty("groupMuted" + juce::String(groupIndex), false));
    }

    for (int sceneSlot = 0; sceneSlot < MlrVSTAudioProcessor::SceneSlots; ++sceneSlot)
    {
        setSceneRepeatCount(
            processor,
            sceneSlot,
            static_cast<int>(sceneState.getProperty("sceneRepeat" + juce::String(sceneSlot), 1)));
        setSceneLengthModeIndex(
            processor,
            sceneSlot,
            static_cast<int>(sceneState.getProperty(
                "sceneLengthMode" + juce::String(sceneSlot),
                static_cast<int>(SceneLengthMode::ManualBars))));
        setSceneManualBars(
            processor,
            sceneSlot,
            static_cast<int>(sceneState.getProperty("sceneManualBars" + juce::String(sceneSlot), 4)));
        setSceneAnchorStrip(
            processor,
            sceneSlot,
            static_cast<int>(sceneState.getProperty("sceneAnchorStrip" + juce::String(sceneSlot), 0)));
    }

    const int storedChainLength = juce::jlimit(0,
                                               MlrVSTAudioProcessor::MaxSceneChainSteps,
                                               static_cast<int>(sceneState.getProperty("chainStepCount", 0)));
    juce::File loadedEndSampleDirectory = processor.sceneChainState.transitionEndSampleDirectory;
    if (sceneState.hasProperty("chainTransitionSampleDir"))
    {
        const auto endSampleDirectoryPath =
            sceneState.getProperty("chainTransitionSampleDir", juce::var()).toString().trim();
        loadedEndSampleDirectory =
            endSampleDirectoryPath.isEmpty() ? juce::File() : juce::File(endSampleDirectoryPath);
    }
    for (int stepIndex = 0; stepIndex < storedChainLength; ++stepIndex)
    {
        auto& step = processor.sceneChainState.steps[static_cast<size_t>(stepIndex)];
        step.sceneSlot = static_cast<int>(sceneState.getProperty("chainScene" + juce::String(stepIndex), -1));
        step.repeats = static_cast<int>(sceneState.getProperty("chainRepeats" + juce::String(stepIndex), 1));
        step.transitionToNext = sanitizeSceneChainTransitionType(
            static_cast<int>(sceneState.getProperty("chainTransition" + juce::String(stepIndex),
                                                    static_cast<int>(SceneChainTransitionType::None))));
        step.transitionOption = sanitizeSceneChainTransitionOption(
            static_cast<int>(sceneState.getProperty("chainTransitionOption" + juce::String(stepIndex),
                                                    static_cast<int>(SceneChainTransitionOption::Default))));
        step.transitionScope = sanitizeSceneChainTransitionScope(
            static_cast<int>(sceneState.getProperty("chainTransitionScope" + juce::String(stepIndex),
                                                    static_cast<int>(SceneChainTransitionScope::All))));
        step.transitionContour = sanitizeSceneChainTransitionContour(
            static_cast<int>(sceneState.getProperty("chainTransitionContour" + juce::String(stepIndex),
                                                    static_cast<int>(SceneChainTransitionContour::Smooth))));
        step.transitionCondition = sanitizeSceneChainTransitionCondition(
            static_cast<int>(sceneState.getProperty("chainTransitionCondition" + juce::String(stepIndex),
                                                    static_cast<int>(SceneChainTransitionCondition::Always))));
        step.transitionLengthBeats = static_cast<float>(
            sceneState.getProperty("chainTransitionLength" + juce::String(stepIndex),
                                   MlrVSTAudioProcessor::DefaultSceneTransitionLengthBeats));
        step.transitionSubtractsFromSceneLength = static_cast<bool>(
            sceneState.getProperty("chainTransitionSubtractFromScene" + juce::String(stepIndex), false));
        step.transitionIntensity = static_cast<float>(
            sceneState.getProperty("chainTransitionIntensity" + juce::String(stepIndex),
                                   MlrVSTAudioProcessor::DefaultSceneTransitionIntensity));
        step.transitionDelayAmount = static_cast<float>(
            sceneState.getProperty("chainTransitionDelay" + juce::String(stepIndex),
                                   MlrVSTAudioProcessor::DefaultSceneTransitionDelayAmount));
        step.transitionFilterAmount = static_cast<float>(
            sceneState.getProperty("chainTransitionFilter" + juce::String(stepIndex),
                                   MlrVSTAudioProcessor::DefaultSceneTransitionFilterAmount));
        step.transitionChopAmount = static_cast<float>(
            sceneState.getProperty("chainTransitionChop" + juce::String(stepIndex),
                                   MlrVSTAudioProcessor::DefaultSceneTransitionChopAmount));
        const auto endSamplePath =
            sceneState.getProperty("chainTransitionEndSample" + juce::String(stepIndex), juce::var())
                .toString()
                .trim();
        step.transitionEndSampleFile = endSamplePath.isEmpty() ? juce::File() : juce::File(endSamplePath);
        step.transitionEndSampleSettings.gainDb = static_cast<float>(
            sceneState.getProperty("chainTransitionEndSampleGainDb" + juce::String(stepIndex),
                                   MlrVSTAudioProcessor::DefaultSceneTransitionEndSampleGainDb));
        step.transitionEndSampleSettings.fadeInMs = static_cast<float>(
            sceneState.getProperty("chainTransitionEndSampleFadeIn" + juce::String(stepIndex),
                                   MlrVSTAudioProcessor::DefaultSceneTransitionEndSampleFadeInMs));
        step.transitionEndSampleSettings.fadeOutMs = static_cast<float>(
            sceneState.getProperty("chainTransitionEndSampleFadeOut" + juce::String(stepIndex),
                                   MlrVSTAudioProcessor::DefaultSceneTransitionEndSampleFadeOutMs));
        step.transitionEndSampleSettings.chokePrevious = static_cast<bool>(
            sceneState.getProperty("chainTransitionEndSampleChoke" + juce::String(stepIndex), false));
        step.transitionEndSampleSettings.reverse = static_cast<bool>(
            sceneState.getProperty("chainTransitionEndSampleReverse" + juce::String(stepIndex), false));
        step.transitionEndSampleSettings.pitchSemitones = static_cast<float>(
            sceneState.getProperty("chainTransitionEndSamplePitch" + juce::String(stepIndex),
                                   MlrVSTAudioProcessor::DefaultSceneTransitionEndSamplePitchSemitones));
        step.transitionEndSampleSettings.lowpassHz = static_cast<float>(
            sceneState.getProperty("chainTransitionEndSampleLowpass" + juce::String(stepIndex),
                                   MlrVSTAudioProcessor::DefaultSceneTransitionEndSampleLowpassHz));
        step.transitionEndSampleSettings.highpassHz = static_cast<float>(
            sceneState.getProperty("chainTransitionEndSampleHighpass" + juce::String(stepIndex),
                                   MlrVSTAudioProcessor::DefaultSceneTransitionEndSampleHighpassHz));
        step.transitionEndSampleSettings.duckSource = static_cast<int>(
            sceneState.getProperty("chainTransitionEndSampleDuckSource" + juce::String(stepIndex),
                                   MlrVSTAudioProcessor::DefaultSceneTransitionEndSampleDuckSource));
        step.transitionEndSampleSettings.duckAmount = static_cast<float>(
            sceneState.getProperty("chainTransitionEndSampleDuckAmount" + juce::String(stepIndex),
                                   MlrVSTAudioProcessor::DefaultSceneTransitionEndSampleDuckAmount));
        sanitizeSceneChainTransitionParameters(step);
    }
    processor.sceneChainState.transitionEndSampleDirectory =
        inferSceneTransitionEndSampleDirectory(processor.sceneChainState, loadedEndSampleDirectory);
    processor.sceneChainState.loopEnabled = static_cast<bool>(sceneState.getProperty("chainLoopEnabled", false));
    processor.sceneChainState.loopStart = static_cast<int>(sceneState.getProperty("chainLoopStart", 0));
    processor.sceneChainState.loopEnd = static_cast<int>(sceneState.getProperty("chainLoopEnd",
                                                                                juce::jmax(0, storedChainLength - 1)));
    sanitizeSceneChainRuntimeState(processor);

    for (int favoriteIndex = 0; favoriteIndex < MlrVSTAudioProcessor::SceneTransitionFavoriteSlots; ++favoriteIndex)
    {
        auto& favorite = processor.sceneTransitionFavorites[static_cast<size_t>(favoriteIndex)];
        favorite.valid = static_cast<bool>(sceneState.getProperty("chainFillFavoriteValid" + juce::String(favoriteIndex), false));
        favorite.type = sanitizeSceneChainTransitionType(
            static_cast<int>(sceneState.getProperty("chainFillFavoriteType" + juce::String(favoriteIndex),
                                                    static_cast<int>(SceneChainTransitionType::None))));
        favorite.option = sanitizeSceneChainTransitionOption(
            static_cast<int>(sceneState.getProperty("chainFillFavoriteOption" + juce::String(favoriteIndex),
                                                    static_cast<int>(SceneChainTransitionOption::Default))));
        favorite.scope = sanitizeSceneChainTransitionScope(
            static_cast<int>(sceneState.getProperty("chainFillFavoriteScope" + juce::String(favoriteIndex),
                                                    static_cast<int>(SceneChainTransitionScope::All))));
        favorite.contour = sanitizeSceneChainTransitionContour(
            static_cast<int>(sceneState.getProperty("chainFillFavoriteContour" + juce::String(favoriteIndex),
                                                    static_cast<int>(SceneChainTransitionContour::Smooth))));
        favorite.condition = sanitizeSceneChainTransitionCondition(
            static_cast<int>(sceneState.getProperty("chainFillFavoriteCondition" + juce::String(favoriteIndex),
                                                    static_cast<int>(SceneChainTransitionCondition::Always))));
        favorite.lengthBeats = static_cast<float>(
            sceneState.getProperty("chainFillFavoriteLength" + juce::String(favoriteIndex),
                                   MlrVSTAudioProcessor::DefaultSceneTransitionLengthBeats));
        favorite.subtractFromSceneLength = static_cast<bool>(
            sceneState.getProperty("chainFillFavoriteSubtract" + juce::String(favoriteIndex), false));
        favorite.intensity = static_cast<float>(
            sceneState.getProperty("chainFillFavoriteIntensity" + juce::String(favoriteIndex),
                                   MlrVSTAudioProcessor::DefaultSceneTransitionIntensity));
        favorite.delayAmount = static_cast<float>(
            sceneState.getProperty("chainFillFavoriteDelay" + juce::String(favoriteIndex),
                                   MlrVSTAudioProcessor::DefaultSceneTransitionDelayAmount));
        favorite.filterAmount = static_cast<float>(
            sceneState.getProperty("chainFillFavoriteFilter" + juce::String(favoriteIndex),
                                   MlrVSTAudioProcessor::DefaultSceneTransitionFilterAmount));
        favorite.chopAmount = static_cast<float>(
            sceneState.getProperty("chainFillFavoriteChop" + juce::String(favoriteIndex),
                                   MlrVSTAudioProcessor::DefaultSceneTransitionChopAmount));
        sanitizeSceneChainTransitionFavorite(favorite);
    }

    processor.applyScenePerformanceStateData({}, -1);
    if (sceneState.hasProperty("scenePerformanceBlob"))
    {
        if (auto* blob = sceneState.getProperty("scenePerformanceBlob").getBinaryData())
            processor.applyScenePerformanceStateData(*blob, -1);
    }

    processor.clearStoredSceneSlotStates(processor.activeSceneMainPresetIndex);
    if (auto storedScenesState = sceneState.getChildWithName("StoredScenes"); storedScenesState.isValid())
    {
        for (auto storedSceneValue : storedScenesState)
        {
            if (!storedSceneValue.hasType("StoredScene"))
                continue;

            const int sceneSlot = juce::jlimit(0,
                                               MlrVSTAudioProcessor::SceneSlots - 1,
                                               static_cast<int>(storedSceneValue.getProperty("slot", -1)));
            const auto snapshotXmlText = storedSceneValue.getProperty("snapshotXml").toString();
            auto snapshotXml = juce::XmlDocument::parse(snapshotXmlText);
            if (snapshotXml == nullptr || !snapshotXml->hasTagName("mlrVSTPreset"))
                continue;

            auto& storedState = processor.storedSceneSlotStates[static_cast<size_t>(sceneSlot)];
            storedState.mainPresetIndex = processor.activeSceneMainPresetIndex;
            storedState.sceneSlot = sceneSlot;
            storedState.hasStoredContent = true;
            storedState.implicitMainPresetFallback =
                static_cast<bool>(storedSceneValue.getProperty("implicitMainPresetFallback", false));
            storedState.name = storedSceneValue.getProperty("name").toString();
            auto templatePayload = std::make_unique<MlrVSTAudioProcessor::PreparedSceneSwitchPayload>();
            if (!processor.parsePreparedSceneSwitchPayloadTemplate(*templatePayload,
                                                                   *snapshotXml,
                                                                   processor.activeSceneMainPresetIndex,
                                                                   sceneSlot))
            {
                storedState = {};
                storedState.mainPresetIndex = processor.activeSceneMainPresetIndex;
                storedState.sceneSlot = sceneSlot;
                continue;
            }
            storedState.preparedSwitchPayloadTemplate = std::move(templatePayload);
        }
    }

    const bool restoredEnabled = processor.sceneModeParam != nullptr
        ? (processor.sceneModeParam->load(std::memory_order_acquire) > 0.5f)
        : static_cast<bool>(sceneState.getProperty("enabled", false));
    applySceneModeState(processor, restoredEnabled);
}
