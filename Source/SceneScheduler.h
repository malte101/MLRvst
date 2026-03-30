/*
  ==============================================================================

    SceneScheduler.h
    Scene timing, scheduling, and persistence helpers

  ==============================================================================
*/

#pragma once

#include "PluginProcessor.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <memory>
#include <cstdint>

class SceneScheduler
{
public:
    static int getSceneRepeatCount(const MlrVSTAudioProcessor& processor, int sceneSlot);
    static void setSceneRepeatCount(MlrVSTAudioProcessor& processor, int sceneSlot, int repeats);
    static int getSceneLengthModeIndex(const MlrVSTAudioProcessor& processor, int sceneSlot);
    static void setSceneLengthModeIndex(MlrVSTAudioProcessor& processor, int sceneSlot, int modeIndex);
    static int getSceneRecallModeIndex(const MlrVSTAudioProcessor& processor);
    static void setSceneRecallModeIndex(MlrVSTAudioProcessor& processor, int modeIndex);
    static int getSceneManualBars(const MlrVSTAudioProcessor& processor, int sceneSlot);
    static void setSceneManualBars(MlrVSTAudioProcessor& processor, int sceneSlot, int bars);
    static int getSceneAnchorStrip(const MlrVSTAudioProcessor& processor, int sceneSlot);
    static void setSceneAnchorStrip(MlrVSTAudioProcessor& processor, int sceneSlot, int stripIndex);
    static int getSceneLengthCount(const MlrVSTAudioProcessor& processor, int sceneSlot);
    static void setSceneLengthCount(MlrVSTAudioProcessor& processor, int sceneSlot, int count);
    static double getResolvedSceneLengthBeats(const MlrVSTAudioProcessor& processor, int sceneSlot);
    static double getSceneAdvanceLengthBeats(const MlrVSTAudioProcessor& processor, int sceneSlot);
    static bool persistSceneTimingForSlot(MlrVSTAudioProcessor& processor, int sceneSlot);
    static int getSceneSequenceStepIndex(const MlrVSTAudioProcessor& processor, int sceneSlot);
    static int getQueuedSceneSlot(const MlrVSTAudioProcessor& processor);
    static juce::String getSceneSequenceSummaryText(const MlrVSTAudioProcessor& processor);
    static int getSceneChainLength(const MlrVSTAudioProcessor& processor);
    static int getSceneChainStepSceneSlot(const MlrVSTAudioProcessor& processor, int stepIndex);
    static int getSceneChainStepRepeatCount(const MlrVSTAudioProcessor& processor, int stepIndex);
    static int getSceneChainStepTransitionTypeIndex(const MlrVSTAudioProcessor& processor, int stepIndex);
    static int getSceneChainStepTransitionOptionIndex(const MlrVSTAudioProcessor& processor, int stepIndex);
    static int getSceneChainStepTransitionScopeIndex(const MlrVSTAudioProcessor& processor, int stepIndex);
    static int getSceneChainStepTransitionContourIndex(const MlrVSTAudioProcessor& processor, int stepIndex);
    static int getSceneChainStepTransitionConditionIndex(const MlrVSTAudioProcessor& processor, int stepIndex);
    static float getSceneChainStepTransitionLengthBeats(const MlrVSTAudioProcessor& processor, int stepIndex);
    static bool getSceneChainStepTransitionSubtractsFromSceneLength(const MlrVSTAudioProcessor& processor, int stepIndex);
    static float getSceneChainStepTransitionIntensity(const MlrVSTAudioProcessor& processor, int stepIndex);
    static float getSceneChainStepTransitionDelayAmount(const MlrVSTAudioProcessor& processor, int stepIndex);
    static float getSceneChainStepTransitionFilterAmount(const MlrVSTAudioProcessor& processor, int stepIndex);
    static float getSceneChainStepTransitionChopAmount(const MlrVSTAudioProcessor& processor, int stepIndex);
    static juce::File getSceneTransitionEndSampleDirectory(const MlrVSTAudioProcessor& processor);
    static juce::File getSceneChainStepTransitionEndSampleFile(const MlrVSTAudioProcessor& processor, int stepIndex);
    static MlrVSTAudioProcessor::SceneTransitionEndSampleSettings
        getSceneChainStepTransitionEndSampleSettings(const MlrVSTAudioProcessor& processor, int stepIndex);
    static void setSceneChainStep(MlrVSTAudioProcessor& processor, int stepIndex, int sceneSlot, int repeats);
    static void setSceneChainStepTransitionTypeIndex(MlrVSTAudioProcessor& processor, int stepIndex, int typeIndex);
    static void setSceneChainStepTransitionOptionIndex(MlrVSTAudioProcessor& processor, int stepIndex, int optionIndex);
    static void setSceneChainStepTransitionScopeIndex(MlrVSTAudioProcessor& processor, int stepIndex, int scopeIndex);
    static void setSceneChainStepTransitionContourIndex(MlrVSTAudioProcessor& processor,
                                                        int stepIndex,
                                                        int contourIndex);
    static void setSceneChainStepTransitionConditionIndex(MlrVSTAudioProcessor& processor,
                                                          int stepIndex,
                                                          int conditionIndex);
    static void setSceneChainStepTransitionLengthBeats(MlrVSTAudioProcessor& processor, int stepIndex, float beats);
    static void setSceneChainStepTransitionSubtractsFromSceneLength(MlrVSTAudioProcessor& processor,
                                                                    int stepIndex,
                                                                    bool enabled);
    static void setSceneChainStepTransitionIntensity(MlrVSTAudioProcessor& processor, int stepIndex, float amount);
    static void setSceneChainStepTransitionDelayAmount(MlrVSTAudioProcessor& processor, int stepIndex, float amount);
    static void setSceneChainStepTransitionFilterAmount(MlrVSTAudioProcessor& processor, int stepIndex, float amount);
    static void setSceneChainStepTransitionChopAmount(MlrVSTAudioProcessor& processor, int stepIndex, float amount);
    static void setSceneTransitionEndSampleDirectory(MlrVSTAudioProcessor& processor, const juce::File& directory);
    static void setSceneChainStepTransitionEndSampleFile(MlrVSTAudioProcessor& processor,
                                                         int stepIndex,
                                                         const juce::File& file);
    static void setSceneChainStepTransitionEndSampleSettings(
        MlrVSTAudioProcessor& processor,
        int stepIndex,
        const MlrVSTAudioProcessor::SceneTransitionEndSampleSettings& settings);
    static void clearSceneChain(MlrVSTAudioProcessor& processor);
    static bool isSceneChainLoopEnabled(const MlrVSTAudioProcessor& processor);
    static void setSceneChainLoopEnabled(MlrVSTAudioProcessor& processor, bool enabled);
    static int getSceneChainLoopStartStep(const MlrVSTAudioProcessor& processor);
    static int getSceneChainLoopEndStep(const MlrVSTAudioProcessor& processor);
    static void setSceneChainLoopRange(MlrVSTAudioProcessor& processor, int startStep, int endStep);
    static bool isSceneChainPlaybackActive(const MlrVSTAudioProcessor& processor);
    static int getSceneChainPlaybackStepIndex(const MlrVSTAudioProcessor& processor);
    static bool startSceneChainPlayback(MlrVSTAudioProcessor& processor, int startStepIndex);
    static void stopSceneChainPlayback(MlrVSTAudioProcessor& processor);
    static std::unique_ptr<juce::XmlElement> createSceneChainStateXml(const MlrVSTAudioProcessor& processor,
                                                                      int sceneSlotOverride);
    static void applySceneChainStateXml(MlrVSTAudioProcessor& processor,
                                        const juce::XmlElement* xml,
                                        int sceneSlotOverride);
    static double computeStripSceneSequenceLengthBeats(const MlrVSTAudioProcessor& processor, int stripIndex);
    static double computeLongestStripSceneSequenceLengthBeats(const MlrVSTAudioProcessor& processor);
    static double computeLongestPatternSceneSequenceLengthBeats(const MlrVSTAudioProcessor& processor);
    static double computeCurrentSceneSequenceLengthBeats(const MlrVSTAudioProcessor& processor);
    static double computeNextScenePatternEndPpq(const MlrVSTAudioProcessor& processor,
                                                int sceneSlot,
                                                double currentPpq,
                                                double cycleBeats,
                                                uint64_t* outPhaseSignature);
    static void armNextSceneInSequence(MlrVSTAudioProcessor& processor,
                                       int mainPresetIndex,
                                       int currentSceneSlot,
                                       double sceneStartPpq);
    static void setSceneModeEnabled(MlrVSTAudioProcessor& processor, bool enabled);
    static void captureSceneModeGroupSnapshot(MlrVSTAudioProcessor& processor);
    static void restoreSceneModeGroupSnapshot(MlrVSTAudioProcessor& processor);
    static void clearAllStripGroupsForSceneMode(MlrVSTAudioProcessor& processor);
    static void applySceneModeState(MlrVSTAudioProcessor& processor, bool enabled);
    static void syncSceneModeFromParameters(MlrVSTAudioProcessor& processor);
    static bool saveSceneForMainPreset(MlrVSTAudioProcessor& processor, int mainPresetIndex, int sceneSlot);
    static bool copySceneForMainPreset(MlrVSTAudioProcessor& processor,
                                       int mainPresetIndex,
                                       int sourceSceneSlot,
                                       int destSceneSlot);
    static bool deleteSceneForMainPreset(MlrVSTAudioProcessor& processor, int mainPresetIndex, int sceneSlot);
    static bool captureSceneSlot(MlrVSTAudioProcessor& processor, int sceneSlot);
    static bool insertSceneSlot(MlrVSTAudioProcessor& processor, int sceneSlot, bool insertAfter);
    static void requestSceneRecallQuantized(MlrVSTAudioProcessor& processor,
                                            int mainPresetIndex,
                                            int sceneSlot,
                                            bool sequenceDriven,
                                            int sequenceStepIndex = -1,
                                            bool useTriggerQuantization = false);
    static double getSceneRecallIntervalBeats(const MlrVSTAudioProcessor& processor);
    static void updateSceneQuantizedRecall(MlrVSTAudioProcessor& processor,
                                           const juce::AudioPlayHead::PositionInfo& posInfo,
                                           int numSamples);
    static void processPendingSceneApply(MlrVSTAudioProcessor& processor);
    static void performEmptySceneLoad(MlrVSTAudioProcessor& processor);
    static void performSceneLoad(MlrVSTAudioProcessor& processor,
                                 int mainPresetIndex,
                                 int sceneSlot,
                                 double hostPpqSnapshot,
                                 double hostTempoSnapshot,
                                 int64_t hostGlobalSampleSnapshot,
                                 bool preserveLoadedStripAudio = false,
                                 bool* recallContinuityBrokenOut = nullptr);
    static void appendSceneModeStateToState(const MlrVSTAudioProcessor& processor, juce::ValueTree& state);
    static void loadSceneModeStateFromState(MlrVSTAudioProcessor& processor, const juce::ValueTree& state);

private:
    static void markSceneChainTransitionEdited(MlrVSTAudioProcessor& processor, int editedStepIndex);
};
