/*
  ==============================================================================

    SceneScheduler.h
    Scene timing, scheduling, and persistence helpers

  ==============================================================================
*/

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <memory>
#include <cstdint>

class MlrVSTAudioProcessor;

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
                                            bool sequenceDriven);
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
                                 int64_t hostGlobalSampleSnapshot);
    static void appendSceneModeStateToState(const MlrVSTAudioProcessor& processor, juce::ValueTree& state);
    static void loadSceneModeStateFromState(MlrVSTAudioProcessor& processor, const juce::ValueTree& state);
};
