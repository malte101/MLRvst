/*
  ==============================================================================

    StripControlState.h
    Resolved strip control state helpers

  ==============================================================================
*/

#pragma once

#include "AudioEngine.h"
#include <atomic>

namespace StripControlState
{
struct ParameterView
{
    std::atomic<float>* volumeParam = nullptr;
    std::atomic<float>* trimDbParam = nullptr;
    std::atomic<float>* panParam = nullptr;
    std::atomic<float>* speedParam = nullptr;
    std::atomic<float>* filterEnabledParam = nullptr;
    std::atomic<float>* filterFrequencyParam = nullptr;
    std::atomic<float>* filterResonanceParam = nullptr;
    std::atomic<float>* filterMorphParam = nullptr;
    std::atomic<float>* filterAlgorithmParam = nullptr;
    std::atomic<float>* delayMixParam = nullptr;
    std::atomic<float>* delayTimeParam = nullptr;
    std::atomic<float>* delaySyncParam = nullptr;
    std::atomic<float>* delayFeedbackParam = nullptr;
    std::atomic<float>* delayLowCutParam = nullptr;
    std::atomic<float>* delayHighCutParam = nullptr;
    std::atomic<float>* delayModeParam = nullptr;
};

struct ResolvedOwnedStripControlState
{
    float volume = 1.0f;
    float trimDb = 0.0f;
    float pan = 0.0f;
    bool usesGrainPlaybackSpeed = false;
    float playbackSpeed = 1.0f;
    float playheadSpeedRatio = 1.0f;
    bool filterEnabled = false;
    float filterFrequency = 20000.0f;
    float filterResonance = 0.707f;
    float filterMorph = 0.0f;
    EnhancedAudioStrip::FilterAlgorithm filterAlgorithm = EnhancedAudioStrip::FilterAlgorithm::Tpt12;
    float delayMix = 0.0f;
    float delayTime = 1.0f;
    bool delaySyncEnabled = true;
    float delayFeedback = 0.35f;
    float delayLowCutHz = 20.0f;
    float delayHighCutHz = 12000.0f;
    EnhancedAudioStrip::DelayMode delayMode = EnhancedAudioStrip::DelayMode::Single;
};

ResolvedOwnedStripControlState resolveFromParameters(const ParameterView& view,
                                                     const EnhancedAudioStrip& strip);

void applyOwnedControls(EnhancedAudioStrip& strip,
                        const ResolvedOwnedStripControlState& state);

void applyFilter(EnhancedAudioStrip& strip,
                 const ResolvedOwnedStripControlState& state);

void applyDelay(EnhancedAudioStrip& strip,
                const ResolvedOwnedStripControlState& state);
} // namespace StripControlState
