#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <functional>

class ModernAudioEngine;

namespace PresetStore
{
juce::File getPresetDirectory();
void savePreset(int presetIndex,
                int maxStrips,
                ModernAudioEngine* audioEngine,
                juce::AudioProcessorValueTreeState& parameters,
                const juce::File* currentStripFiles);
void loadPreset(int presetIndex,
                int maxStrips,
                ModernAudioEngine* audioEngine,
                juce::AudioProcessorValueTreeState& parameters,
                const std::function<void(int, const juce::File&)>& loadSampleToStrip);
juce::String getPresetName(int presetIndex);
bool presetExists(int presetIndex);
bool deletePreset(int presetIndex);
} // namespace PresetStore
