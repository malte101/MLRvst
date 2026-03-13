#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <functional>

class ModernAudioEngine;

namespace PresetStore
{
juce::File getPresetDirectory();
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
                const std::function<std::unique_ptr<juce::XmlElement>()>& createAuxStateXml = {});
bool loadPreset(int presetIndex,
                int maxStrips,
                ModernAudioEngine* audioEngine,
                juce::AudioProcessorValueTreeState& parameters,
                const std::function<bool(int, const juce::File&)>& loadSampleToStrip,
                const std::function<void(int, const juce::File&)>& restoreStripSamplePath,
                const std::function<void(int, const juce::File&, const juce::File&, const juce::File&)>& restoreStripRecentDirectories,
                const std::function<void(int, const juce::XmlElement*)>& applyFlipStateXml,
                const std::function<void(int, const juce::XmlElement*)>& applyLoopPitchStateXml,
                const std::function<void(const juce::XmlElement&)>& applyAuxStateXml,
                double hostPpqSnapshot,
                double hostTempoSnapshot,
                bool preserveGlobalParameters = true,
                int64_t hostGlobalSampleSnapshot = -1);
juce::String getPresetName(int presetIndex);
bool setPresetName(int presetIndex, const juce::String& presetName);
bool presetExists(int presetIndex);
bool deletePreset(int presetIndex);
} // namespace PresetStore
