#pragma once

#include "AudioEngine.h"

namespace AudioEngineModulationInternal
{
bool modTargetAutoDefaultBipolar(ModernAudioEngine::ModTarget target);
bool isGrainModTarget(ModernAudioEngine::ModTarget target);
bool modTargetUsesForcedBipolar(ModernAudioEngine::ModTarget target);
float quantizeModLaneRate(float rate) noexcept;
float filterCutoffToNormalized(float hz) noexcept;
float normalizedToFilterCutoff(float normalized) noexcept;
float filterResonanceToNormalized(float q) noexcept;
float normalizedToFilterResonance(float normalized) noexcept;
const juce::NormalisableRange<float>& delayTimeRange() noexcept;
const juce::NormalisableRange<float>& delayLowCutRange() noexcept;
const juce::NormalisableRange<float>& delayHighCutRange() noexcept;
int rearrangeSliceIndexFromNormalized(float value01, int localSliceCount) noexcept;
float convertModValueForBipolarMode(float value01,
                                    bool wasBipolar,
                                    bool willBeBipolar,
                                    ModernAudioEngine::ModBipolarToggleMode mode);
} // namespace AudioEngineModulationInternal
