#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <functional>

class ModernAudioEngine;

namespace PresetStore
{
enum class StripRecallMode
{
    PresetState = 0,
    TriggerStyle,
    SceneStartPpqAnchored
};

juce::File getPresetDirectory();
std::unique_ptr<juce::XmlElement> createPresetXml(int presetIndex,
                                                  int maxStrips,
                                                  ModernAudioEngine* audioEngine,
                                                  juce::AudioProcessorValueTreeState& parameters,
                                                  const juce::File* currentStripFiles,
                                                  const juce::File* recentLoopDirectories,
                                                  const juce::File* recentStepDirectories,
                                                  const juce::File* recentFlipDirectories,
                                                  const std::function<std::unique_ptr<juce::XmlElement>(int)>& createFlipStateXml,
                                                  const std::function<std::unique_ptr<juce::XmlElement>(int)>& createLoopPitchStateXml,
                                                  const std::function<std::unique_ptr<juce::XmlElement>()>& createAuxStateXml = {},
                                                  const std::function<juce::MemoryBlock()>& createScenePerformanceData = {});
bool savePreset(int presetIndex,
                int maxStrips,
                ModernAudioEngine* audioEngine,
                juce::AudioProcessorValueTreeState& parameters,
                const juce::File* currentStripFiles,
                const juce::File* recentLoopDirectories,
                const juce::File* recentStepDirectories,
                const juce::File* recentFlipDirectories,
                const std::function<std::unique_ptr<juce::XmlElement>(int)>& createFlipStateXml,
                const std::function<std::unique_ptr<juce::XmlElement>(int)>& createLoopPitchStateXml,
                const std::function<std::unique_ptr<juce::XmlElement>()>& createAuxStateXml = {},
                const std::function<juce::MemoryBlock()>& createScenePerformanceData = {});
bool loadPreset(int presetIndex,
                int maxStrips,
                ModernAudioEngine* audioEngine,
                juce::AudioProcessorValueTreeState* parameters,
                const std::function<bool(int, const juce::File&)>& loadSampleToStrip,
                const std::function<bool(int, const juce::XmlElement&)>& loadPreparedStripAudio,
                const std::function<void(int, const juce::File&)>& restoreStripSamplePath,
                const std::function<void(int, const juce::File&, const juce::File&, const juce::File&)>& restoreStripRecentDirectories,
                const std::function<void(int, const juce::XmlElement*)>& applyFlipStateXml,
                const std::function<void(int, const juce::XmlElement*)>& applyLoopPitchStateXml,
                const std::function<void(const juce::XmlElement&)>& applyAuxStateXml,
                const std::function<void(const juce::MemoryBlock&)>& applyScenePerformanceData,
                double hostPpqSnapshot,
                double hostTempoSnapshot,
                bool preserveGlobalParameters = true,
                int64_t hostGlobalSampleSnapshot = -1,
                StripRecallMode stripRecallMode = StripRecallMode::PresetState,
                const juce::File* existingStripFiles = nullptr,
                bool reuseMatchingLoadedStripAudio = false,
                bool preferLauncherStyleSampleFades = false,
                juce::ValueTree* restoredParameterStateOut = nullptr,
                bool* recallContinuityBrokenOut = nullptr);
bool applyPresetXml(const juce::XmlElement& presetXml,
                    int presetIndexHint,
                    int maxStrips,
                    ModernAudioEngine* audioEngine,
                    juce::AudioProcessorValueTreeState* parameters,
                    const std::function<bool(int, const juce::File&)>& loadSampleToStrip,
                    const std::function<bool(int, const juce::XmlElement&)>& loadPreparedStripAudio,
                    const std::function<void(int, const juce::File&)>& restoreStripSamplePath,
                    const std::function<void(int, const juce::File&, const juce::File&, const juce::File&)>& restoreStripRecentDirectories,
                    const std::function<void(int, const juce::XmlElement*)>& applyFlipStateXml,
                    const std::function<void(int, const juce::XmlElement*)>& applyLoopPitchStateXml,
                    const std::function<void(const juce::XmlElement&)>& applyAuxStateXml,
                    const std::function<void(const juce::MemoryBlock&)>& applyScenePerformanceData,
                    double hostPpqSnapshot,
                    double hostTempoSnapshot,
                    bool preserveGlobalParameters = true,
                    int64_t hostGlobalSampleSnapshot = -1,
                    StripRecallMode stripRecallMode = StripRecallMode::PresetState,
                    const juce::File* existingStripFiles = nullptr,
                    bool reuseMatchingLoadedStripAudio = false,
                    bool preferLauncherStyleSampleFades = false,
                    const juce::MemoryBlock* preloadedScenePerformanceData = nullptr,
                    juce::ValueTree* restoredParameterStateOut = nullptr,
                    bool* recallContinuityBrokenOut = nullptr);
std::unique_ptr<juce::XmlElement> loadPresetXml(int presetIndex);
juce::String getPresetName(int presetIndex);
std::unique_ptr<juce::XmlElement> loadSceneTimingStateXml(int presetIndex);
bool loadScenePerformanceData(int presetIndex, juce::MemoryBlock& outData);
bool setPresetName(int presetIndex, const juce::String& presetName);
bool updatePresetAuxState(int presetIndex,
                          const std::function<std::unique_ptr<juce::XmlElement>()>& createAuxStateXml);
bool presetExists(int presetIndex);
bool presetHasLaunchableSceneContent(int presetIndex);
bool copyPreset(int sourcePresetIndex, int destPresetIndex);
bool deletePreset(int presetIndex);
} // namespace PresetStore
