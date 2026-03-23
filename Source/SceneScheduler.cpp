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
using SceneChainState = MlrVSTAudioProcessor::SceneChainState;
using SceneChainStep = MlrVSTAudioProcessor::SceneChainStep;

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

int computeSceneRecallBlendSamples(const MlrVSTAudioProcessor& processor)
{
    const double effectiveRate = processor.getSampleRate() > 0.0 ? processor.getSampleRate() : 44100.0;
    float triggerFadeMs = 12.0f;
    if (const auto* engine = processor.getAudioEngine(); engine != nullptr)
        triggerFadeMs = juce::jlimit(0.1f, 120.0f, engine->getTriggerFadeInMs());

    double blendSeconds = juce::jlimit(0.004, 0.060, static_cast<double>(triggerFadeMs) * 0.001);
    if (SceneScheduler::isSceneChainPlaybackActive(processor))
        blendSeconds = juce::jlimit(0.008, 0.085, blendSeconds * 1.35);
    return juce::jmax(64, static_cast<int>(std::round(effectiveRate * blendSeconds)));
}

bool shouldUsePreloadedSceneTransitions(const MlrVSTAudioProcessor&)
{
    // The preloaded engine-to-engine handoff still regresses chain stability in
    // host testing. Keep scene recalls on the stable direct restore path until
    // that transition engine is fixed end-to-end.
    return false;
}

SceneLengthMode sanitizeSceneLengthMode(int rawMode)
{
    return static_cast<SceneLengthMode>(juce::jlimit(
        0,
        static_cast<int>(SceneLengthMode::AnchorStrip),
        rawMode));
}

SceneRecallMode sanitizeSceneRecallMode(int rawMode)
{
    return static_cast<SceneRecallMode>(juce::jlimit(
        0,
        static_cast<int>(SceneRecallMode::Manual),
        rawMode));
}

void clearSceneChainStep(SceneChainStep& step)
{
    step.sceneSlot = -1;
    step.repeats = 1;
    step.transitionToNext = SceneChainTransitionType::None;
    step.transitionOption = SceneChainTransitionOption::Default;
    step.transitionLengthBeats = MlrVSTAudioProcessor::DefaultSceneTransitionLengthBeats;
    step.transitionSubtractsFromSceneLength = false;
    step.transitionIntensity = MlrVSTAudioProcessor::DefaultSceneTransitionIntensity;
    step.transitionDelayAmount = MlrVSTAudioProcessor::DefaultSceneTransitionDelayAmount;
    step.transitionFilterAmount = MlrVSTAudioProcessor::DefaultSceneTransitionFilterAmount;
    step.transitionChopAmount = MlrVSTAudioProcessor::DefaultSceneTransitionChopAmount;
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

void sanitizeSceneChainTransitionParameters(SceneChainStep& step)
{
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
        case SceneChainTransitionType::Return:     return "Return";
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
        processor.pendingSceneApplySequenceDriven.store(0, std::memory_order_release);
        processor.pendingSceneApplySequenceStep.store(-1, std::memory_order_release);
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
            processor.clearSceneBoundaryTransitionState(false);
        }
        else
        {
            const auto updatedOption = sanitizeSceneChainTransitionOption(
                processor.sceneChainState.steps[static_cast<size_t>(safeEditedStep)].transitionOption);
            processor.sceneBoundaryTransitionType.store(static_cast<int>(updatedType), std::memory_order_release);
            processor.sceneBoundaryTransitionOption.store(static_cast<int>(updatedOption), std::memory_order_release);
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
    const int storageIndex = processor.getSceneStoragePresetIndex(mainPresetIndex, clampedSlot);
    if (!PresetStore::presetExists(storageIndex))
        return false;

    const bool updated = PresetStore::updatePresetAuxState(
        storageIndex,
        [&processor, clampedSlot]()
        {
            return createSceneChainStateXml(processor, clampedSlot);
        });
    if (updated)
        processor.presetRefreshToken.fetch_add(1, std::memory_order_acq_rel);
    return updated;
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
    const int queuedApplySlot = processor.pendingSceneApplySlot.load(std::memory_order_acquire);
    if (queuedApplySlot >= 0)
        return juce::jlimit(0, MlrVSTAudioProcessor::SceneSlots - 1, queuedApplySlot);
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
    for (int stepIndex = 0; stepIndex < chainLength; ++stepIndex)
    {
        xml->setAttribute("chainScene" + juce::String(stepIndex), getSceneChainStepSceneSlot(processor, stepIndex));
        xml->setAttribute("chainRepeats" + juce::String(stepIndex), getSceneChainStepRepeatCount(processor, stepIndex));
        xml->setAttribute("chainTransition" + juce::String(stepIndex),
                          getSceneChainStepTransitionTypeIndex(processor, stepIndex));
        xml->setAttribute("chainTransitionOption" + juce::String(stepIndex),
                          getSceneChainStepTransitionOptionIndex(processor, stepIndex));
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
        || xml->hasAttribute("chainLoopEnd");
    for (int stepIndex = 0; stepIndex < MlrVSTAudioProcessor::MaxSceneChainSteps && !hasChainAttributes; ++stepIndex)
    {
            hasChainAttributes = xml->hasAttribute("chainScene" + juce::String(stepIndex))
                || xml->hasAttribute("chainRepeats" + juce::String(stepIndex))
                || xml->hasAttribute("chainTransition" + juce::String(stepIndex))
                || xml->hasAttribute("chainTransitionOption" + juce::String(stepIndex))
                || xml->hasAttribute("chainTransitionLength" + juce::String(stepIndex))
                || xml->hasAttribute("chainTransitionSubtractFromScene" + juce::String(stepIndex))
                || xml->hasAttribute("chainTransitionIntensity" + juce::String(stepIndex))
                || xml->hasAttribute("chainTransitionDelay" + juce::String(stepIndex))
                || xml->hasAttribute("chainTransitionFilter" + juce::String(stepIndex))
                || xml->hasAttribute("chainTransitionChop" + juce::String(stepIndex));
    }

    if (hasChainAttributes)
    {
        for (auto& step : processor.sceneChainState.steps)
            clearSceneChainStep(step);

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
            sanitizeSceneChainTransitionParameters(step);
        }

        processor.sceneChainState.loopEnabled = static_cast<bool>(xml->getBoolAttribute("chainLoopEnabled", false));
        processor.sceneChainState.loopStart = xml->getIntAttribute("chainLoopStart", 0);
        processor.sceneChainState.loopEnd = xml->getIntAttribute("chainLoopEnd", juce::jmax(0, chainLength - 1));
        sanitizeSceneChainRuntimeState(processor);
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
        if (playMode == EnhancedAudioStrip::PlayMode::Step)
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
    if (processor.activeSceneStartPpqValid && std::isfinite(processor.activeSceneStartPpq))
    {
        attachSceneStartPpq = processor.activeSceneStartPpq;
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

    processor.sceneSequenceActive = chainCanRun;
    processor.sceneSequenceCurrentStepIndex = startStep;
    processor.sceneSequenceStartPpqValid = false;
    processor.sceneSequenceStartPpq = 0.0;
    processor.pendingSceneRecall = {};
    processor.pendingSceneApplyMainPreset.store(-1, std::memory_order_release);
    processor.pendingSceneApplySlot.store(-1, std::memory_order_release);
    processor.pendingSceneApplySequenceDriven.store(0, std::memory_order_release);
    processor.pendingSceneApplySequenceStep.store(-1, std::memory_order_release);
    processor.pendingSceneApplyTargetPpq.store(-1.0, std::memory_order_release);
    processor.pendingSceneApplyTargetTempo.store(120.0, std::memory_order_release);
    processor.pendingSceneApplyTargetSample.store(-1, std::memory_order_release);
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
    processor.preparedSceneRenderContext.reset();
    processor.activeSceneTransitionContext.reset();
    processor.sceneTransitionSamplesTotal = 0;
    processor.sceneTransitionSamplesRendered = 0;
    processor.sceneTransitionStartSampleOffset = 0;
    processor.sceneTransitionCommitPending = false;
    processor.clearSceneBoundaryTransitionState();
    processor.clearSceneChainReturnOverride();

    if (canAttachToCurrentScene)
    {
        processor.sceneSequenceStartPpqValid = true;
        processor.sceneSequenceStartPpq = attachSceneStartPpq;
        armNextSceneInSequence(processor,
                               processor.getActiveMainPresetIndexForScenes(),
                               sceneSlot,
                               attachSceneStartPpq);
        processor.updateMonomeLEDs();
        return true;
    }

    requestSceneRecallQuantized(processor,
                                processor.getActiveMainPresetIndexForScenes(),
                                sceneSlot,
                                processor.sceneSequenceActive,
                                processor.sceneSequenceActive ? startStep : -1);
    processor.updateMonomeLEDs();
    return true;
}

void SceneScheduler::stopSceneChainPlayback(MlrVSTAudioProcessor& processor)
{
    processor.sceneSequenceActive = false;
    processor.sceneSequenceCurrentStepIndex = -1;
    processor.sceneSequenceStartPpqValid = false;
    processor.sceneSequenceStartPpq = 0.0;
    processor.pendingSceneRecall = {};
    processor.pendingSceneApplyMainPreset.store(-1, std::memory_order_release);
    processor.pendingSceneApplySlot.store(-1, std::memory_order_release);
    processor.pendingSceneApplySequenceDriven.store(0, std::memory_order_release);
    processor.pendingSceneApplySequenceStep.store(-1, std::memory_order_release);
    processor.pendingSceneApplyTargetPpq.store(-1.0, std::memory_order_release);
    processor.pendingSceneApplyTargetTempo.store(120.0, std::memory_order_release);
    processor.pendingSceneApplyTargetSample.store(-1, std::memory_order_release);
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
    processor.preparedSceneRenderContext.reset();
    processor.activeSceneTransitionContext.reset();
    processor.sceneTransitionSamplesTotal = 0;
    processor.sceneTransitionSamplesRendered = 0;
    processor.sceneTransitionStartSampleOffset = 0;
    processor.sceneTransitionCommitPending = false;
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
        processor.sceneSequenceStartPpqValid = false;
        return;
    }

    processor.sceneSequenceStartPpqValid = std::isfinite(sceneStartPpq);
    processor.sceneSequenceStartPpq = processor.sceneSequenceStartPpqValid ? sceneStartPpq : 0.0;

    int currentIndex = processor.sceneSequenceCurrentStepIndex;
    if (currentIndex < 0)
        currentIndex = getSceneSequenceStepIndex(processor, currentSceneSlot);
    if (currentIndex < 0)
        currentIndex = 0;

    int nextStepIndex = -1;
    if (!processor.consumeSceneChainReturnOverrideForStep(currentIndex, nextStepIndex))
        nextStepIndex = resolveSceneChainNextStepIndex(processor, currentIndex);
    if (nextStepIndex < 0)
    {
        processor.sceneSequenceActive = false;
        processor.pendingSceneRecall.active = false;
        processor.pendingSceneRecall.targetResolved = false;
        processor.pendingSceneRecall.sequenceStepIndex = -1;
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
    }
    else
    {
        captureSceneSlot(processor, processor.activeSceneSlot);
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
    processor.pendingSceneApplyMainPreset.store(-1, std::memory_order_release);
    processor.pendingSceneApplySlot.store(-1, std::memory_order_release);
    processor.pendingSceneApplySequenceDriven.store(0, std::memory_order_release);
    processor.pendingSceneApplySequenceStep.store(-1, std::memory_order_release);
    processor.pendingSceneApplyTargetPpq.store(-1.0, std::memory_order_release);
    processor.pendingSceneApplyTargetTempo.store(120.0, std::memory_order_release);
    processor.pendingSceneApplyTargetSample.store(-1, std::memory_order_release);
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

    const int storageIndex = processor.getSceneStoragePresetIndex(mainPresetIndex, sceneSlot);
    const bool saved = PresetStore::savePreset(storageIndex,
                                               MlrVSTAudioProcessor::MaxStrips,
                                               processor.audioEngine.get(),
                                               processor.parameters,
                                               processor.currentStripFiles.data(),
                                               processor.recentLoopDirectories.data(),
                                               processor.recentStepDirectories.data(),
                                               processor.recentFlipDirectories.data(),
                                               [&processor](int stripIndex)
                                               {
                                                   return processor.createFlipPresetStateXml(stripIndex);
                                               },
                                               [&processor](int stripIndex)
                                               {
                                                   return processor.createLoopPitchPresetStateXml(stripIndex);
                                               },
                                               [&processor, sceneSlot]()
                                               {
                                                   return createSceneChainStateXml(processor, sceneSlot);
                                               },
                                               [&processor, sceneSlot]()
                                               {
                                                   return processor.createScenePerformanceStateData(sceneSlot);
                                               });
    if (saved)
    {
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
    const int sourceStorageIndex = processor.getSceneStoragePresetIndex(clampedMain, clampedSource);
    const int destStorageIndex = processor.getSceneStoragePresetIndex(clampedMain, clampedDest);
    if (sourceStorageIndex == destStorageIndex)
        return true;

    const bool sourceExists = PresetStore::presetExists(sourceStorageIndex);
    if (!sourceExists)
    {
        if (PresetStore::presetExists(destStorageIndex))
        {
            const bool deleted = PresetStore::deletePreset(destStorageIndex);
            if (deleted)
            {
                if (processor.activeSceneMainPresetIndex == clampedMain)
                    processor.clearScenePerformanceClip(clampedDest);
                processor.presetRefreshToken.fetch_add(1, std::memory_order_acq_rel);
            }
            return deleted;
        }

        if (processor.activeSceneMainPresetIndex == clampedMain)
            processor.clearScenePerformanceClip(clampedDest);
        return true;
    }

    const bool copied = PresetStore::copyPreset(sourceStorageIndex, destStorageIndex);
    if (copied)
    {
        if (processor.activeSceneMainPresetIndex == clampedMain)
            processor.copyScenePerformanceClip(clampedSource, clampedDest);
        processor.presetRefreshToken.fetch_add(1, std::memory_order_acq_rel);
    }
    return copied;
}

bool SceneScheduler::deleteSceneForMainPreset(MlrVSTAudioProcessor& processor, int mainPresetIndex, int sceneSlot)
{
    const int storageIndex = processor.getSceneStoragePresetIndex(mainPresetIndex, sceneSlot);
    if (!PresetStore::presetExists(storageIndex))
    {
        if (processor.activeSceneMainPresetIndex == juce::jlimit(0, MlrVSTAudioProcessor::MaxPresetSlots - 1, mainPresetIndex))
            processor.clearScenePerformanceClip(sceneSlot);
        return true;
    }

    const bool deleted = PresetStore::deletePreset(storageIndex);
    if (deleted)
    {
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
        processor.pendingSceneApplyTargetPpq.store(hasTimingReference ? hostPpqSnapshot : -1.0,
                                                   std::memory_order_release);
        processor.pendingSceneApplyTargetTempo.store(hasTimingReference ? hostTempoSnapshot : 120.0,
                                                     std::memory_order_release);
        processor.pendingSceneApplyTargetSample.store(processor.audioEngine->getGlobalSampleCount(), std::memory_order_release);
        processor.pendingSceneApplyMainPreset.store(clampedMainPresetIndex, std::memory_order_release);
        processor.pendingSceneApplySlot.store(clampedSceneSlot, std::memory_order_release);
        processor.pendingSceneApplySequenceDriven.store(sequenceDriven ? 1 : 0, std::memory_order_release);
        processor.pendingSceneApplySequenceStep.store(sequenceDriven ? sequenceStepIndex : -1, std::memory_order_release);
        appendSceneDebugLog("request_recall immediate_apply slot=" + juce::String(clampedSceneSlot)
            + " mainPreset=" + juce::String(clampedMainPresetIndex)
            + " hasTiming=" + juce::String(hasTimingReference ? 1 : 0));

        if (sequenceDriven
            && shouldUsePreloadedSceneTransitions(processor)
            && processor.sceneSlotExistsForMainPreset(clampedMainPresetIndex, clampedSceneSlot))
        {
            processor.requestScenePreload(clampedMainPresetIndex,
                                          clampedSceneSlot,
                                          sequenceDriven,
                                          sequenceDriven ? sequenceStepIndex : -1,
                                          hasTimingReference ? hostPpqSnapshot : -1.0,
                                          hasTimingReference ? hostTempoSnapshot : 120.0,
                                          processor.audioEngine->getGlobalSampleCount(),
                                          SceneChainTransitionType::None);
        }
    }

    processor.refreshUtilityTimerCadence();
}

double SceneScheduler::getSceneRecallIntervalBeats(const MlrVSTAudioProcessor& processor)
{
    const int quantizeDivision = juce::jmax(1, processor.getQuantizeDivision());
    return juce::jlimit(1.0 / 64.0, 256.0, 4.0 / static_cast<double>(quantizeDivision));
}

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

    if (processor.pendingSceneApplySlot.load(std::memory_order_acquire) >= 0)
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
    const bool useTriggerGridTiming = !processor.pendingSceneRecall.sequenceDriven
        && processor.pendingSceneRecall.useTriggerQuantization;
    const auto sceneRecallMode = useTriggerGridTiming
        ? SceneRecallMode::QuantizeGrid
        : sanitizeSceneRecallMode(getSceneRecallModeIndex(processor));
    const bool patternEndEnabled = sceneRecallMode == SceneRecallMode::PatternEnd;
    const bool sceneEndEnabled = sceneRecallMode == SceneRecallMode::SceneEnd;
    const bool manualSceneChange = sceneRecallMode == SceneRecallMode::Manual;
    const bool sequenceUsesSceneLengthTiming = processor.pendingSceneRecall.sequenceDriven
        && (sceneEndEnabled || manualSceneChange);
    const int currentSceneSlot = juce::jlimit(0, MlrVSTAudioProcessor::SceneSlots - 1, processor.activeSceneSlot);
    const double currentSceneDurationBeats = juce::jlimit(
        0.25,
        4096.0,
        computeCurrentSceneSequenceLengthBeats(processor));
    uint64_t phaseAlignedPatternEndSignature = 0;
    const double phaseAlignedPatternEndPpq = patternEndEnabled
        ? computeNextScenePatternEndPpq(
              processor,
              currentSceneSlot,
              currentPpq,
              currentSceneDurationBeats,
              &phaseAlignedPatternEndSignature)
        : std::numeric_limits<double>::quiet_NaN();
    const bool phaseAlignedTimingReady = patternEndEnabled && std::isfinite(phaseAlignedPatternEndPpq);
    const bool sequenceTimingReady = sequenceUsesSceneLengthTiming
        && processor.sceneSequenceStartPpqValid
        && std::isfinite(processor.sceneSequenceStartPpq);
    const bool manualSceneEndTimingReady = sceneEndEnabled
        && !processor.pendingSceneRecall.sequenceDriven
        && processor.activeSceneStartPpqValid
        && std::isfinite(processor.activeSceneStartPpq);
    const bool useSceneDurationTiming = phaseAlignedTimingReady || sequenceTimingReady || manualSceneEndTimingReady;
    double intervalBeatsNow = getSceneRecallIntervalBeats(processor);
    if (useSceneDurationTiming)
        intervalBeatsNow = currentSceneDurationBeats;

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

        // Pattern End must never silently fall back to grid timing.
        // If we do not yet have a valid loop-phase boundary for the active scene,
        // keep waiting instead of arming an early scene change.
        if (!phaseAlignedTimingReady)
        {
            processor.pendingSceneRecall.targetResolved = false;
            processor.pendingSceneRecall.targetPpq = 0.0;
            processor.clearSceneBoundaryTransitionState();
            return;
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
        if (manualSceneChange && !processor.pendingSceneRecall.sequenceDriven)
        {
            processor.pendingSceneRecall.targetPpq = currentPpq;
        }
        else if (phaseAlignedTimingReady)
        {
            processor.pendingSceneRecall.targetPpq = phaseAlignedPatternEndPpq;
        }
        else if (useSceneDurationTiming)
        {
            const double sceneStartReference = sequenceTimingReady ? processor.sceneSequenceStartPpq : processor.activeSceneStartPpq;
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
            processor.pendingSceneRecall.targetPpq = std::round(nextBoundary / intervalBeatsNow) * intervalBeatsNow;
        }
        processor.pendingSceneRecall.targetResolved = true;
    }

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
    const double ppqPerSecond = currentTempo / 60.0;
    const double ppqPerSample = ppqPerSecond / processor.currentSampleRate;
    const double targetPpq = processor.pendingSceneRecall.targetPpq;
    const double samplesToTarget = (targetPpq - currentPpq) / juce::jmax(1.0e-12, ppqPerSample);
    const int64_t currentGlobalSample = processor.audioEngine != nullptr
        ? processor.audioEngine->getGlobalSampleCount()
        : -1;
    const int64_t targetGlobalSample = currentGlobalSample >= 0
        ? currentGlobalSample + static_cast<int64_t>(std::llround(samplesToTarget))
        : -1;
    const auto preloadTransitionType = sequenceDrivenHandoff ? outgoingTransitionType : SceneChainTransitionType::None;

    if (processor.pendingSceneRecall.sequenceDriven
        && shouldUsePreloadedSceneTransitions(processor)
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

    if (sequenceDrivenHandoff
        && outgoingTransitionType != SceneChainTransitionType::None
        && outgoingTransitionType != SceneChainTransitionType::Return
        && std::isfinite(targetPpq))
    {
        processor.armSceneBoundaryTransition(outgoingTransitionType,
                                             outgoingTransitionOption,
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
                                             targetGlobalSample);
    }
    else
    {
        processor.clearSceneBoundaryTransitionState();
    }

    const double blockEndPpq = currentPpq + (ppqPerSample * static_cast<double>(juce::jmax(1, numSamples)));
    if (blockEndPpq + 1.0e-9 < processor.pendingSceneRecall.targetPpq)
        return;

    processor.pendingSceneApplyTargetPpq.store(targetPpq, std::memory_order_release);
    processor.pendingSceneApplyTargetTempo.store(currentTempo, std::memory_order_release);
    processor.pendingSceneApplyTargetSample.store(targetGlobalSample, std::memory_order_release);
    processor.pendingSceneApplyMainPreset.store(processor.pendingSceneRecall.mainPresetIndex, std::memory_order_release);
    processor.pendingSceneApplySlot.store(processor.pendingSceneRecall.sceneSlot, std::memory_order_release);
    processor.pendingSceneApplySequenceDriven.store(processor.pendingSceneRecall.sequenceDriven ? 1 : 0, std::memory_order_release);
    processor.pendingSceneApplySequenceStep.store(processor.pendingSceneRecall.sequenceDriven
                                                      ? processor.pendingSceneRecall.sequenceStepIndex
                                                      : -1,
                                                  std::memory_order_release);
    processor.pendingSceneRecall.active = false;
    processor.pendingSceneRecall.targetResolved = false;
}

void SceneScheduler::processPendingSceneApply(MlrVSTAudioProcessor& processor)
{
    const int queuedSlot = processor.pendingSceneApplySlot.exchange(-1, std::memory_order_acq_rel);
    if (queuedSlot < 0)
        return;

    const int queuedMain = processor.pendingSceneApplyMainPreset.exchange(-1, std::memory_order_acq_rel);
    const bool queuedSequenceDriven = processor.pendingSceneApplySequenceDriven.exchange(0, std::memory_order_acq_rel) != 0;
    const int queuedSequenceStep = processor.pendingSceneApplySequenceStep.exchange(-1, std::memory_order_acq_rel);
    const double queuedTargetPpq = processor.pendingSceneApplyTargetPpq.exchange(-1.0, std::memory_order_acq_rel);
    const double queuedTargetTempo = processor.pendingSceneApplyTargetTempo.exchange(120.0, std::memory_order_acq_rel);
    const int64_t queuedTargetSample = processor.pendingSceneApplyTargetSample.exchange(-1, std::memory_order_acq_rel);

    const int clampedMain = juce::jlimit(0, MlrVSTAudioProcessor::MaxPresetSlots - 1, queuedMain);
    const int clampedSlot = juce::jlimit(0, MlrVSTAudioProcessor::SceneSlots - 1, queuedSlot);
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
        processor.pendingSceneApplyMainPreset.store(clampedMain, std::memory_order_release);
        processor.pendingSceneApplySlot.store(clampedSlot, std::memory_order_release);
        processor.pendingSceneApplySequenceDriven.store(effectiveSequenceDriven ? 1 : 0, std::memory_order_release);
        processor.pendingSceneApplySequenceStep.store(effectiveSequenceDriven ? effectiveSequenceStep : -1,
                                                      std::memory_order_release);
        processor.pendingSceneApplyTargetPpq.store(queuedTargetPpq, std::memory_order_release);
        processor.pendingSceneApplyTargetTempo.store(queuedTargetTempo, std::memory_order_release);
        processor.pendingSceneApplyTargetSample.store(queuedTargetSample, std::memory_order_release);
    };

    if (processor.activeSceneTransitionContext != nullptr)
    {
        requeuePendingApply();
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

    if (outgoingTransitionType == SceneChainTransitionType::MuteTail
        || outgoingTransitionType == SceneChainTransitionType::Break)
    {
        processor.suppressSceneRecallBlendOnNextLoad.store(1, std::memory_order_release);
    }

    if (effectiveSequenceDriven
        && outgoingTransitionType == SceneChainTransitionType::Return
        && outgoingStepIndex >= 0
        && effectiveSequenceStep >= 0)
    {
        processor.armSceneChainReturnOverride(outgoingStepIndex, effectiveSequenceStep);
    }
    else if (!effectiveSequenceDriven)
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
        : (processor.audioEngine != nullptr ? processor.audioEngine->getGlobalSampleCount() : -1);
    const auto applyTransitionType = effectiveSequenceDriven ? outgoingTransitionType : SceneChainTransitionType::None;
    const bool usePreloadedTransition = shouldUsePreloadedSceneTransitions(processor);

    const bool targetSceneExists = processor.sceneSlotExistsForMainPreset(clampedMain, clampedSlot);
    appendSceneDebugLog("process_apply target_exists slot=" + juce::String(clampedSlot)
        + " mainPreset=" + juce::String(clampedMain)
        + " exists=" + juce::String(targetSceneExists ? 1 : 0)
        + " sequenceDriven=" + juce::String(effectiveSequenceDriven ? 1 : 0));
    bool startedPreloadedTransition = false;
    if (targetSceneExists)
    {
        if (usePreloadedTransition)
        {
            startedPreloadedTransition = processor.tryStartPreloadedSceneTransition(
                clampedMain,
                clampedSlot,
                effectiveSequenceDriven,
                effectiveSequenceDriven ? effectiveSequenceStep : -1,
                appliedPpq,
                appliedTempo,
                appliedGlobalSample,
                0,
                applyTransitionType);

            if (!startedPreloadedTransition)
            {
                MlrVSTAudioProcessor::SceneRenderContext synchronousContext;
                if (processor.buildSceneRenderContext(synchronousContext,
                                                      clampedMain,
                                                      clampedSlot,
                                                      appliedPpq,
                                                      appliedTempo,
                                                      appliedGlobalSample,
                                                      applyTransitionType,
                                                      effectiveSequenceDriven,
                                                      effectiveSequenceDriven ? effectiveSequenceStep : -1))
                {
                    processor.preparedSceneRenderContext =
                        std::make_unique<MlrVSTAudioProcessor::SceneRenderContext>(std::move(synchronousContext));
                    startedPreloadedTransition = processor.tryStartPreloadedSceneTransition(
                        clampedMain,
                        clampedSlot,
                        effectiveSequenceDriven,
                        effectiveSequenceDriven ? effectiveSequenceStep : -1,
                        appliedPpq,
                        appliedTempo,
                        appliedGlobalSample,
                        0,
                        applyTransitionType);
                }
            }

            if (!startedPreloadedTransition)
            {
                processor.preparedSceneRenderContext.reset();
                performSceneLoad(processor,
                                 clampedMain,
                                 clampedSlot,
                                 appliedPpq,
                                 appliedTempo,
                                 appliedGlobalSample);
            }
        }
        else
        {
            processor.preparedSceneRenderContext.reset();
            performSceneLoad(processor,
                             clampedMain,
                             clampedSlot,
                             appliedPpq,
                             appliedTempo,
                             appliedGlobalSample);
        }
    }
    else
    {
        if (!effectiveSequenceDriven)
        {
            appendSceneDebugLog("process_apply empty_scene slot=" + juce::String(clampedSlot)
                + " mainPreset=" + juce::String(clampedMain));
            processor.preparedSceneRenderContext.reset();
            performEmptySceneLoad(processor);
        }
        else
        {
            DBG("Scene chain advance skipped because target scene slot " << (clampedSlot + 1)
                << " for preset " << (clampedMain + 1) << " does not exist");
            processor.sceneSequenceActive = false;
            processor.sceneSequenceCurrentStepIndex = -1;
            processor.sceneSequenceStartPpqValid = false;
            processor.sceneSequenceStartPpq = 0.0;
            processor.pendingSceneRecall = {};
            processor.pendingSceneApplyMainPreset.store(-1, std::memory_order_release);
            processor.pendingSceneApplySlot.store(-1, std::memory_order_release);
            processor.pendingSceneApplySequenceDriven.store(0, std::memory_order_release);
            processor.pendingSceneApplySequenceStep.store(-1, std::memory_order_release);
            processor.pendingSceneApplyTargetPpq.store(-1.0, std::memory_order_release);
            processor.pendingSceneApplyTargetTempo.store(120.0, std::memory_order_release);
            processor.pendingSceneApplyTargetSample.store(-1, std::memory_order_release);
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
            processor.preparedSceneRenderContext.reset();
            processor.clearSceneBoundaryTransitionState();
            processor.clearSceneChainReturnOverride();
            processor.presetRefreshToken.fetch_add(1, std::memory_order_acq_rel);
            processor.updateMonomeLEDs();
            return;
        }
    }

    if (usePreloadedTransition && startedPreloadedTransition)
        return;

    // The fill/transition overlay belongs to the outgoing scene only. On the
    // stable direct-load path, clear it as soon as the new scene has been
    // applied so the incoming scene doesn't spend its first block under the
    // old transition treatment.
    processor.clearSceneBoundaryTransitionState();

    if (effectiveSequenceDriven && chainLength >= 2)
    {
        const int fallbackStepIndex = getSceneSequenceStepIndex(processor, clampedSlot);
        const int appliedSequenceStep = effectiveSequenceStep >= 0 ? effectiveSequenceStep : fallbackStepIndex;
        processor.sceneSequenceActive = true;
        processor.sceneSequenceCurrentStepIndex = juce::jlimit(0, chainLength - 1, juce::jmax(0, appliedSequenceStep));
    }
    else
    {
        processor.sceneSequenceActive = false;
        processor.sceneSequenceCurrentStepIndex = -1;
    }

    processor.activeSceneMainPresetIndex = clampedMain;
    processor.activeSceneSlot = clampedSlot;
    const double appliedSceneStartPpq = appliedPpq;
    processor.activeSceneStartPpqValid = std::isfinite(appliedSceneStartPpq);
    processor.activeSceneStartPpq = processor.activeSceneStartPpqValid ? appliedSceneStartPpq : 0.0;
    if (PresetStore::presetExists(clampedMain))
        processor.loadedPresetIndex = clampedMain;
    if (effectiveSequenceDriven)
        armNextSceneInSequence(processor, clampedMain, clampedSlot, appliedPpq);
    else
        processor.sceneSequenceStartPpqValid = false;
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
    const auto preservedSceneRepeatCounts = processor.sceneRepeatCounts;
    const auto preservedSceneLengthModes = processor.sceneLengthModes;
    const auto preservedSceneManualBars = processor.sceneManualBars;
    const auto preservedSceneAnchorStrips = processor.sceneAnchorStrips;
    const auto preservedSceneChainState = processor.sceneChainState;
    const bool suppressSceneRecallBlend = processor.suppressSceneRecallBlendOnNextLoad.exchange(0, std::memory_order_acq_rel) != 0;

    processor.resetRuntimePresetStateToDefaults();
    processor.activeSceneNeedsCaptureBeforeManualRecall = false;
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
    for (auto& f : processor.currentStripFiles)
        f = juce::File();

    const int desiredBlendSamples = juce::jlimit(0,
                                                 MlrVSTAudioProcessor::kSceneRecallBlendMaxSamples,
                                                 computeSceneRecallBlendSamples(processor));
    const int availableTailSamples = juce::jlimit(0,
                                                  MlrVSTAudioProcessor::kSceneRecallBlendMaxSamples,
                                                  processor.lastRenderedOutputTailLength);
    const int blendSamples = suppressSceneRecallBlend ? 0 : juce::jmin(desiredBlendSamples, availableTailSamples);
    processor.sceneRecallBlendStartSamples = processor.lastRenderedOutputSamples;
    processor.sceneRecallBlendStartTailLength = blendSamples;
    if (blendSamples > 0)
    {
        const int tailOffset = availableTailSamples - blendSamples;
        for (int channel = 0; channel < MlrVSTAudioProcessor::kSceneRecallBlendMaxChannels; ++channel)
        {
            std::copy_n(processor.lastRenderedOutputTail[static_cast<size_t>(channel)].begin() + tailOffset,
                        blendSamples,
                        processor.sceneRecallBlendStartTail[static_cast<size_t>(channel)].begin());
        }
    }
    processor.sceneRecallBlendTotalSamples = suppressSceneRecallBlend
        ? 0
        : (blendSamples > 0 ? blendSamples : (desiredBlendSamples > 0 ? 1 : 0));
    processor.sceneRecallBlendSamplesRemaining = processor.sceneRecallBlendTotalSamples;

    syncSceneModeFromParameters(processor);
    if (processor.isSceneModeEnabled())
        clearAllStripGroupsForSceneMode(processor);
}

void SceneScheduler::performSceneLoad(MlrVSTAudioProcessor& processor,
                                      int mainPresetIndex,
                                      int sceneSlot,
                                      double hostPpqSnapshot,
                                      double hostTempoSnapshot,
                                      int64_t hostGlobalSampleSnapshot)
{
    if (!processor.audioEngine)
        return;

    const int storageIndex = processor.getSceneStoragePresetIndex(mainPresetIndex, sceneSlot);
    if (!PresetStore::presetExists(storageIndex))
        return;

    ScopedSuspendProcessing scopedSuspend(processor);
    appendSceneDebugLog("perform_scene_load slot=" + juce::String(sceneSlot)
        + " mainPreset=" + juce::String(mainPresetIndex)
        + " storageIndex=" + juce::String(storageIndex));
    const bool suppressSceneRecallBlend = processor.suppressSceneRecallBlendOnNextLoad.exchange(0, std::memory_order_acq_rel) != 0;

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

    processor.resetRuntimePresetStateToDefaults();
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
    {
        ScopedSceneAutosaveSuppression suppressSceneAutosave(processor);
        loadSucceeded = PresetStore::loadPreset(
            storageIndex,
            MlrVSTAudioProcessor::MaxStrips,
            processor.audioEngine.get(),
            processor.parameters,
            [&processor](int stripIndex, const juce::File& sampleFile)
            {
                return processor.loadSampleToStrip(stripIndex, sampleFile);
            },
            [&processor](int stripIndex, const juce::File& sampleFile)
            {
                processor.rememberLoadedSamplePathForStrip(stripIndex, sampleFile, false);
            },
            [&processor](int stripIndex,
                         const juce::File& loopDir,
                         const juce::File& stepDir,
                         const juce::File& flipDir)
            {
                processor.setRecentSampleDirectory(stripIndex, MlrVSTAudioProcessor::SamplePathMode::Loop, loopDir, false);
                processor.setRecentSampleDirectory(stripIndex, MlrVSTAudioProcessor::SamplePathMode::Step, stepDir, false);
                processor.setRecentSampleDirectory(stripIndex, MlrVSTAudioProcessor::SamplePathMode::Flip, flipDir, false);
            },
            [&processor](int stripIndex, const juce::XmlElement* flipStateXml)
            {
                processor.applyFlipPresetStateXml(stripIndex, flipStateXml);
            },
            [&processor](int stripIndex, const juce::XmlElement* loopPitchStateXml)
            {
                processor.applyLoopPitchPresetStateXml(stripIndex, loopPitchStateXml);
            },
            [&processor, sceneSlot](const juce::XmlElement& presetXml)
            {
                processor.applySceneChainStateXml(presetXml.getChildByName("SceneChainState"), sceneSlot);
            },
            [&processor, sceneSlot](const juce::MemoryBlock& scenePerformanceData)
            {
                processor.applyScenePerformanceStateData(scenePerformanceData, sceneSlot);
            },
            hostPpqSnapshot,
            hostTempoSnapshot,
            true,
            hostGlobalSampleSnapshot,
            true);
    }

    if (loadSucceeded)
    {
        appendSceneDebugLog("perform_scene_load success slot=" + juce::String(sceneSlot)
            + " mainPreset=" + juce::String(mainPresetIndex));
        processor.activeSceneNeedsCaptureBeforeManualRecall = false;
        processor.resolveLoopPitchRecallStateImmediately();
        processor.syncScenePerformanceClipLengthToResolvedLength(sceneSlot);
        if (processor.isSceneModeEnabled() && processor.audioEngine != nullptr)
        {
            if (processor.sceneSlotHasMotionState(sceneSlot))
                processor.applySceneMotionStateToEngine(sceneSlot);
            else
                processor.syncSceneMotionStateFromEngine(sceneSlot);
        }

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

                if (strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Step)
                    continue;

                const bool hasStripAudio = (strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Sample)
                    ? processor.hasSampleModeAudio(stripIndex)
                    : strip->hasAudio();
                if (!hasStripAudio)
                    continue;

                // PresetStore::loadPreset() has already restored the strip's saved
                // PPQ anchor and trigger/fade state. Re-triggering here can make
                // the scene load feel like parameters or phase are "catching up"
                // after the recall. Only realign the saved anchor to the exact
                // recall sample so playback stays phase-consistent without a
                // second restore fade.
                strip->realignToPpqAnchor(hostPpqSnapshot, recallSample);
            }
        }

        const int desiredBlendSamples = juce::jlimit(0,
                                                     MlrVSTAudioProcessor::kSceneRecallBlendMaxSamples,
                                                     computeSceneRecallBlendSamples(processor));
        const int availableTailSamples = juce::jlimit(0,
                                                      MlrVSTAudioProcessor::kSceneRecallBlendMaxSamples,
                                                      processor.lastRenderedOutputTailLength);
        const int blendSamples = suppressSceneRecallBlend ? 0 : juce::jmin(desiredBlendSamples, availableTailSamples);
        processor.sceneRecallBlendStartSamples = processor.lastRenderedOutputSamples;
        processor.sceneRecallBlendStartTailLength = blendSamples;
        if (blendSamples > 0)
        {
            const int tailOffset = availableTailSamples - blendSamples;
            for (int channel = 0; channel < MlrVSTAudioProcessor::kSceneRecallBlendMaxChannels; ++channel)
            {
                std::copy_n(processor.lastRenderedOutputTail[static_cast<size_t>(channel)].begin() + tailOffset,
                            blendSamples,
                            processor.sceneRecallBlendStartTail[static_cast<size_t>(channel)].begin());
            }
        }
        processor.sceneRecallBlendTotalSamples = suppressSceneRecallBlend
            ? 0
            : (blendSamples > 0 ? blendSamples : (desiredBlendSamples > 0 ? 1 : 0));
        processor.sceneRecallBlendSamplesRemaining = processor.sceneRecallBlendTotalSamples;
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
    sceneState.setProperty("groupSnapshotValid", processor.sceneModeGroupSnapshot.valid, nullptr);
    sceneState.setProperty("chainStepCount", getSceneChainLength(processor), nullptr);
    sceneState.setProperty("chainLoopEnabled", isSceneChainLoopEnabled(processor), nullptr);
    sceneState.setProperty("chainLoopStart", getSceneChainLoopStartStep(processor), nullptr);
    sceneState.setProperty("chainLoopEnd", getSceneChainLoopEndStep(processor), nullptr);

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
    }

    sceneState.setProperty("scenePerformanceBlob", processor.createScenePerformanceStateData(-1), nullptr);
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
    processor.activeSceneMainPresetIndex = 0;
    processor.activeSceneSlot = 0;
    processor.pendingSceneRecall = {};
    processor.sceneSequenceActive = false;
    processor.sceneSequenceCurrentStepIndex = -1;
    processor.activeSceneStartPpqValid = false;
    processor.activeSceneStartPpq = 0.0;
    processor.sceneSequenceStartPpqValid = false;
    processor.sceneSequenceStartPpq = 0.0;
    processor.pendingSceneApplyMainPreset.store(-1, std::memory_order_release);
    processor.pendingSceneApplySlot.store(-1, std::memory_order_release);
    processor.pendingSceneApplySequenceDriven.store(0, std::memory_order_release);
    processor.pendingSceneApplySequenceStep.store(-1, std::memory_order_release);
    processor.pendingSceneApplyTargetPpq.store(-1.0, std::memory_order_release);
    processor.pendingSceneApplyTargetTempo.store(120.0, std::memory_order_release);
    processor.pendingSceneApplyTargetSample.store(-1, std::memory_order_release);
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
        sanitizeSceneChainTransitionParameters(step);
    }
    processor.sceneChainState.loopEnabled = static_cast<bool>(sceneState.getProperty("chainLoopEnabled", false));
    processor.sceneChainState.loopStart = static_cast<int>(sceneState.getProperty("chainLoopStart", 0));
    processor.sceneChainState.loopEnd = static_cast<int>(sceneState.getProperty("chainLoopEnd",
                                                                                juce::jmax(0, storedChainLength - 1)));
    sanitizeSceneChainRuntimeState(processor);

    processor.applyScenePerformanceStateData({}, -1);
    if (sceneState.hasProperty("scenePerformanceBlob"))
    {
        if (auto* blob = sceneState.getProperty("scenePerformanceBlob").getBinaryData())
            processor.applyScenePerformanceStateData(*blob, -1);
    }

    const bool restoredEnabled = processor.sceneModeParam != nullptr
        ? (processor.sceneModeParam->load(std::memory_order_acquire) > 0.5f)
        : static_cast<bool>(sceneState.getProperty("enabled", false));
    applySceneModeState(processor, restoredEnabled);
}
