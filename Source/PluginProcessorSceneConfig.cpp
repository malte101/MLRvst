/*
  ==============================================================================

    PluginProcessorSceneConfig.cpp
    Extracted scene timing and chain configuration for mlrVST.

  ==============================================================================
*/

#include "PluginProcessor.h"
#include "SceneScheduler.h"

int MlrVSTAudioProcessor::getSceneRepeatCount(int sceneSlot) const
{
    return SceneScheduler::getSceneRepeatCount(*this, sceneSlot);
}

void MlrVSTAudioProcessor::setSceneRepeatCount(int sceneSlot, int repeats)
{
    SceneScheduler::setSceneRepeatCount(*this, sceneSlot, repeats);
}

MlrVSTAudioProcessor::SceneLengthMode MlrVSTAudioProcessor::getSceneLengthMode(int sceneSlot) const
{
    return static_cast<SceneLengthMode>(SceneScheduler::getSceneLengthModeIndex(*this, sceneSlot));
}

void MlrVSTAudioProcessor::setSceneLengthMode(int sceneSlot, SceneLengthMode mode)
{
    SceneScheduler::setSceneLengthModeIndex(*this, sceneSlot, static_cast<int>(mode));
}

MlrVSTAudioProcessor::SceneRecallMode MlrVSTAudioProcessor::getSceneRecallMode() const
{
    return static_cast<SceneRecallMode>(SceneScheduler::getSceneRecallModeIndex(*this));
}

void MlrVSTAudioProcessor::setSceneRecallMode(SceneRecallMode mode)
{
    SceneScheduler::setSceneRecallModeIndex(*this, static_cast<int>(mode));
}

int MlrVSTAudioProcessor::getSceneManualBars(int sceneSlot) const
{
    return SceneScheduler::getSceneManualBars(*this, sceneSlot);
}

void MlrVSTAudioProcessor::setSceneManualBars(int sceneSlot, int bars)
{
    SceneScheduler::setSceneManualBars(*this, sceneSlot, bars);
}

int MlrVSTAudioProcessor::getSceneAnchorStrip(int sceneSlot) const
{
    return SceneScheduler::getSceneAnchorStrip(*this, sceneSlot);
}

void MlrVSTAudioProcessor::setSceneAnchorStrip(int sceneSlot, int stripIndex)
{
    SceneScheduler::setSceneAnchorStrip(*this, sceneSlot, stripIndex);
}

int MlrVSTAudioProcessor::getSceneLengthCount(int sceneSlot) const
{
    return SceneScheduler::getSceneLengthCount(*this, sceneSlot);
}

void MlrVSTAudioProcessor::setSceneLengthCount(int sceneSlot, int count)
{
    SceneScheduler::setSceneLengthCount(*this, sceneSlot, count);
}

double MlrVSTAudioProcessor::getResolvedSceneLengthBeats(int sceneSlot) const
{
    return SceneScheduler::getResolvedSceneLengthBeats(*this, sceneSlot);
}

double MlrVSTAudioProcessor::getSceneAdvanceLengthBeats(int sceneSlot) const
{
    return SceneScheduler::getSceneAdvanceLengthBeats(*this, sceneSlot);
}

bool MlrVSTAudioProcessor::persistSceneTimingForSlot(int sceneSlot)
{
    return SceneScheduler::persistSceneTimingForSlot(*this, sceneSlot);
}

int MlrVSTAudioProcessor::getSceneSequenceStepIndex(int sceneSlot) const
{
    return SceneScheduler::getSceneSequenceStepIndex(*this, sceneSlot);
}

int MlrVSTAudioProcessor::getQueuedSceneSlot() const
{
    return SceneScheduler::getQueuedSceneSlot(*this);
}

juce::String MlrVSTAudioProcessor::getSceneSequenceSummaryText() const
{
    return SceneScheduler::getSceneSequenceSummaryText(*this);
}

int MlrVSTAudioProcessor::getSceneChainLength() const
{
    return SceneScheduler::getSceneChainLength(*this);
}

int MlrVSTAudioProcessor::getSceneChainStepSceneSlot(int stepIndex) const
{
    return SceneScheduler::getSceneChainStepSceneSlot(*this, stepIndex);
}

int MlrVSTAudioProcessor::getSceneChainStepRepeatCount(int stepIndex) const
{
    return SceneScheduler::getSceneChainStepRepeatCount(*this, stepIndex);
}

MlrVSTAudioProcessor::SceneChainTransitionType MlrVSTAudioProcessor::getSceneChainStepTransitionType(int stepIndex) const
{
    return static_cast<SceneChainTransitionType>(
        SceneScheduler::getSceneChainStepTransitionTypeIndex(*this, stepIndex));
}

MlrVSTAudioProcessor::SceneChainTransitionOption MlrVSTAudioProcessor::getSceneChainStepTransitionOption(int stepIndex) const
{
    return static_cast<SceneChainTransitionOption>(
        SceneScheduler::getSceneChainStepTransitionOptionIndex(*this, stepIndex));
}

MlrVSTAudioProcessor::SceneChainTransitionScope MlrVSTAudioProcessor::getSceneChainStepTransitionScope(int stepIndex) const
{
    return static_cast<SceneChainTransitionScope>(
        SceneScheduler::getSceneChainStepTransitionScopeIndex(*this, stepIndex));
}

MlrVSTAudioProcessor::SceneChainTransitionContour MlrVSTAudioProcessor::getSceneChainStepTransitionContour(int stepIndex) const
{
    return static_cast<SceneChainTransitionContour>(
        SceneScheduler::getSceneChainStepTransitionContourIndex(*this, stepIndex));
}

MlrVSTAudioProcessor::SceneChainTransitionCondition MlrVSTAudioProcessor::getSceneChainStepTransitionCondition(int stepIndex) const
{
    return static_cast<SceneChainTransitionCondition>(
        SceneScheduler::getSceneChainStepTransitionConditionIndex(*this, stepIndex));
}

float MlrVSTAudioProcessor::getSceneChainStepTransitionLengthBeats(int stepIndex) const
{
    return SceneScheduler::getSceneChainStepTransitionLengthBeats(*this, stepIndex);
}

bool MlrVSTAudioProcessor::getSceneChainStepTransitionSubtractsFromSceneLength(int stepIndex) const
{
    return SceneScheduler::getSceneChainStepTransitionSubtractsFromSceneLength(*this, stepIndex);
}

float MlrVSTAudioProcessor::getSceneChainStepTransitionIntensity(int stepIndex) const
{
    return SceneScheduler::getSceneChainStepTransitionIntensity(*this, stepIndex);
}

float MlrVSTAudioProcessor::getSceneChainStepTransitionDelayAmount(int stepIndex) const
{
    return SceneScheduler::getSceneChainStepTransitionDelayAmount(*this, stepIndex);
}

float MlrVSTAudioProcessor::getSceneChainStepTransitionFilterAmount(int stepIndex) const
{
    return SceneScheduler::getSceneChainStepTransitionFilterAmount(*this, stepIndex);
}

float MlrVSTAudioProcessor::getSceneChainStepTransitionChopAmount(int stepIndex) const
{
    return SceneScheduler::getSceneChainStepTransitionChopAmount(*this, stepIndex);
}

juce::File MlrVSTAudioProcessor::getSceneTransitionEndSampleDirectory() const
{
    return SceneScheduler::getSceneTransitionEndSampleDirectory(*this);
}

juce::File MlrVSTAudioProcessor::getSceneChainStepTransitionEndSampleFile(int stepIndex) const
{
    return SceneScheduler::getSceneChainStepTransitionEndSampleFile(*this, stepIndex);
}

MlrVSTAudioProcessor::SceneTransitionEndSampleSettings
MlrVSTAudioProcessor::getSceneChainStepTransitionEndSampleSettings(int stepIndex) const
{
    return SceneScheduler::getSceneChainStepTransitionEndSampleSettings(*this, stepIndex);
}

MlrVSTAudioProcessor::SceneChainTransitionFavorite MlrVSTAudioProcessor::getSceneTransitionFavorite(int favoriteIndex) const
{
    const int safeIndex = juce::jlimit(0, SceneTransitionFavoriteSlots - 1, favoriteIndex);
    return sceneTransitionFavorites[static_cast<size_t>(safeIndex)];
}

bool MlrVSTAudioProcessor::hasSceneTransitionFavorite(int favoriteIndex) const
{
    return getSceneTransitionFavorite(favoriteIndex).valid;
}

void MlrVSTAudioProcessor::setSceneChainStep(int stepIndex, int sceneSlot, int repeats)
{
    SceneScheduler::setSceneChainStep(*this, stepIndex, sceneSlot, repeats);
}

void MlrVSTAudioProcessor::setSceneChainStepTransitionType(int stepIndex, SceneChainTransitionType type)
{
    SceneScheduler::setSceneChainStepTransitionTypeIndex(*this, stepIndex, static_cast<int>(type));
}

void MlrVSTAudioProcessor::setSceneChainStepTransitionOption(int stepIndex, SceneChainTransitionOption option)
{
    SceneScheduler::setSceneChainStepTransitionOptionIndex(*this, stepIndex, static_cast<int>(option));
}

void MlrVSTAudioProcessor::setSceneChainStepTransitionScope(int stepIndex, SceneChainTransitionScope scope)
{
    SceneScheduler::setSceneChainStepTransitionScopeIndex(*this, stepIndex, static_cast<int>(scope));
}

void MlrVSTAudioProcessor::setSceneChainStepTransitionContour(int stepIndex, SceneChainTransitionContour contour)
{
    SceneScheduler::setSceneChainStepTransitionContourIndex(*this, stepIndex, static_cast<int>(contour));
}

void MlrVSTAudioProcessor::setSceneChainStepTransitionCondition(int stepIndex, SceneChainTransitionCondition condition)
{
    SceneScheduler::setSceneChainStepTransitionConditionIndex(*this, stepIndex, static_cast<int>(condition));
}

void MlrVSTAudioProcessor::setSceneChainStepTransitionLengthBeats(int stepIndex, float beats)
{
    SceneScheduler::setSceneChainStepTransitionLengthBeats(*this, stepIndex, beats);
}

void MlrVSTAudioProcessor::setSceneChainStepTransitionSubtractsFromSceneLength(int stepIndex, bool enabled)
{
    SceneScheduler::setSceneChainStepTransitionSubtractsFromSceneLength(*this, stepIndex, enabled);
}

void MlrVSTAudioProcessor::setSceneChainStepTransitionIntensity(int stepIndex, float amount)
{
    SceneScheduler::setSceneChainStepTransitionIntensity(*this, stepIndex, amount);
}

void MlrVSTAudioProcessor::setSceneChainStepTransitionDelayAmount(int stepIndex, float amount)
{
    SceneScheduler::setSceneChainStepTransitionDelayAmount(*this, stepIndex, amount);
}

void MlrVSTAudioProcessor::setSceneChainStepTransitionFilterAmount(int stepIndex, float amount)
{
    SceneScheduler::setSceneChainStepTransitionFilterAmount(*this, stepIndex, amount);
}

void MlrVSTAudioProcessor::setSceneChainStepTransitionChopAmount(int stepIndex, float amount)
{
    SceneScheduler::setSceneChainStepTransitionChopAmount(*this, stepIndex, amount);
}

void MlrVSTAudioProcessor::setSceneTransitionEndSampleDirectory(const juce::File& directory)
{
    SceneScheduler::setSceneTransitionEndSampleDirectory(*this, directory);
}

bool MlrVSTAudioProcessor::setSceneChainStepTransitionEndSampleFile(int stepIndex, const juce::File& file)
{
    if (stepIndex < 0 || stepIndex >= MaxSceneChainSteps)
        return false;

    const auto previousFile = getSceneChainStepTransitionEndSampleFile(stepIndex);
    SceneScheduler::setSceneChainStepTransitionEndSampleFile(*this, stepIndex, file);
    const auto updatedFile = getSceneChainStepTransitionEndSampleFile(stepIndex);
    if (updatedFile == previousFile)
        return false;

    reloadSceneChainTransitionEndSample(stepIndex);
    return true;
}

void MlrVSTAudioProcessor::setSceneChainStepTransitionEndSampleSettings(
    int stepIndex,
    const SceneTransitionEndSampleSettings& settings)
{
    SceneScheduler::setSceneChainStepTransitionEndSampleSettings(*this, stepIndex, settings);
}

bool MlrVSTAudioProcessor::auditionSceneTransitionEndSampleFile(
    const juce::File& file,
    const SceneTransitionEndSampleSettings& settings)
{
    if (file == juce::File() || !file.existsAsFile())
        return false;

    auto sampleData = loadSceneTransitionEndSampleData(file);
    if (sampleData == nullptr)
        return false;

    auto request = std::make_shared<SceneTransitionEndSamplePreviewRequest>();
    request->sample = std::move(sampleData);
    request->settings = settings;
    std::atomic_store_explicit(&sceneTransitionEndSamplePreviewRequest,
                               std::shared_ptr<const SceneTransitionEndSamplePreviewRequest>(std::move(request)),
                               std::memory_order_release);
    return true;
}

bool MlrVSTAudioProcessor::captureSceneTransitionFavorite(int favoriteIndex, int stepIndex)
{
    const int safeFavoriteIndex = juce::jlimit(0, SceneTransitionFavoriteSlots - 1, favoriteIndex);
    const int chainLength = getSceneChainLength();
    const int safeStepIndex = juce::jlimit(0, juce::jmax(0, chainLength - 1), stepIndex);
    if (chainLength <= 0 || safeStepIndex < 0 || safeStepIndex >= chainLength)
        return false;

    auto& favorite = sceneTransitionFavorites[static_cast<size_t>(safeFavoriteIndex)];
    favorite.valid = true;
    favorite.type = getSceneChainStepTransitionType(safeStepIndex);
    favorite.option = getSceneChainStepTransitionOption(safeStepIndex);
    favorite.scope = getSceneChainStepTransitionScope(safeStepIndex);
    favorite.contour = getSceneChainStepTransitionContour(safeStepIndex);
    favorite.condition = getSceneChainStepTransitionCondition(safeStepIndex);
    favorite.lengthBeats = getSceneChainStepTransitionLengthBeats(safeStepIndex);
    favorite.subtractFromSceneLength = getSceneChainStepTransitionSubtractsFromSceneLength(safeStepIndex);
    favorite.intensity = getSceneChainStepTransitionIntensity(safeStepIndex);
    favorite.delayAmount = getSceneChainStepTransitionDelayAmount(safeStepIndex);
    favorite.filterAmount = getSceneChainStepTransitionFilterAmount(safeStepIndex);
    favorite.chopAmount = getSceneChainStepTransitionChopAmount(safeStepIndex);
    markPersistentGlobalUserChange();
    return true;
}

bool MlrVSTAudioProcessor::applySceneTransitionFavorite(int favoriteIndex, int stepIndex)
{
    const int safeFavoriteIndex = juce::jlimit(0, SceneTransitionFavoriteSlots - 1, favoriteIndex);
    const auto favorite = sceneTransitionFavorites[static_cast<size_t>(safeFavoriteIndex)];
    const int chainLength = getSceneChainLength();
    const int safeStepIndex = juce::jlimit(0, juce::jmax(0, chainLength - 1), stepIndex);
    if (!favorite.valid || chainLength <= 0 || safeStepIndex < 0 || safeStepIndex >= chainLength)
        return false;

    setSceneChainStepTransitionType(safeStepIndex, favorite.type);
    setSceneChainStepTransitionOption(safeStepIndex, favorite.option);
    setSceneChainStepTransitionScope(safeStepIndex, favorite.scope);
    setSceneChainStepTransitionContour(safeStepIndex, favorite.contour);
    setSceneChainStepTransitionCondition(safeStepIndex, favorite.condition);
    setSceneChainStepTransitionLengthBeats(safeStepIndex, favorite.lengthBeats);
    setSceneChainStepTransitionSubtractsFromSceneLength(safeStepIndex, favorite.subtractFromSceneLength);
    setSceneChainStepTransitionIntensity(safeStepIndex, favorite.intensity);
    setSceneChainStepTransitionDelayAmount(safeStepIndex, favorite.delayAmount);
    setSceneChainStepTransitionFilterAmount(safeStepIndex, favorite.filterAmount);
    setSceneChainStepTransitionChopAmount(safeStepIndex, favorite.chopAmount);
    return true;
}

void MlrVSTAudioProcessor::clearSceneChain()
{
    SceneScheduler::clearSceneChain(*this);
    reloadAllSceneChainTransitionEndSamples();
}

bool MlrVSTAudioProcessor::isSceneChainLoopEnabled() const
{
    return SceneScheduler::isSceneChainLoopEnabled(*this);
}

void MlrVSTAudioProcessor::setSceneChainLoopEnabled(bool enabled)
{
    SceneScheduler::setSceneChainLoopEnabled(*this, enabled);
}

int MlrVSTAudioProcessor::getSceneChainLoopStartStep() const
{
    return SceneScheduler::getSceneChainLoopStartStep(*this);
}

int MlrVSTAudioProcessor::getSceneChainLoopEndStep() const
{
    return SceneScheduler::getSceneChainLoopEndStep(*this);
}

void MlrVSTAudioProcessor::setSceneChainLoopRange(int startStep, int endStep)
{
    SceneScheduler::setSceneChainLoopRange(*this, startStep, endStep);
}

bool MlrVSTAudioProcessor::isSceneChainPlaybackActive() const
{
    return SceneScheduler::isSceneChainPlaybackActive(*this);
}

int MlrVSTAudioProcessor::getSceneChainPlaybackStepIndex() const
{
    return SceneScheduler::getSceneChainPlaybackStepIndex(*this);
}

bool MlrVSTAudioProcessor::startSceneChainPlayback(int startStepIndex)
{
    return SceneScheduler::startSceneChainPlayback(*this, startStepIndex);
}

void MlrVSTAudioProcessor::stopSceneChainPlayback()
{
    SceneScheduler::stopSceneChainPlayback(*this);
}
