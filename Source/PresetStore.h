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
                const juce::File* currentStripFiles);
bool loadPreset(int presetIndex,
                int maxStrips,
                ModernAudioEngine* audioEngine,
                juce::AudioProcessorValueTreeState& parameters,
                const std::function<bool(int, const juce::File&)>& loadSampleToStrip,
                double hostPpqSnapshot,
                double hostTempoSnapshot);
juce::String getPresetName(int presetIndex);
bool setPresetName(int presetIndex, const juce::String& presetName);
bool presetExists(int presetIndex);
bool deletePreset(int presetIndex);
} // namespace PresetStore
