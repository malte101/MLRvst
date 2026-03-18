/*
  ==============================================================================

    StripControlState.cpp
    Resolved strip control state helpers

  ==============================================================================
*/

#include "StripControlState.h"
#include "PlayheadSpeedQuantizer.h"

namespace
{
EnhancedAudioStrip::FilterType filterTypeFromMorphValue(float morph)
{
    if (morph < 0.34f)
        return EnhancedAudioStrip::FilterType::LowPass;
    if (morph > 0.66f)
        return EnhancedAudioStrip::FilterType::HighPass;
    return EnhancedAudioStrip::FilterType::BandPass;
}

FilterType stepFilterTypeFromMorphValue(float morph)
{
    if (morph < 0.34f)
        return FilterType::LowPass;
    if (morph > 0.66f)
        return FilterType::HighPass;
    return FilterType::BandPass;
}
} // namespace

namespace StripControlState
{
ResolvedOwnedStripControlState resolveFromParameters(const ParameterView& view,
                                                     const EnhancedAudioStrip& strip)
{
    ResolvedOwnedStripControlState state;

    state.volume = (view.volumeParam != nullptr)
        ? juce::jlimit(0.0f, 1.0f, view.volumeParam->load(std::memory_order_acquire))
        : strip.getVolume();
    state.trimDb = (view.trimDbParam != nullptr)
        ? view.trimDbParam->load(std::memory_order_acquire)
        : strip.getTrimDb();
    state.pan = (view.panParam != nullptr)
        ? juce::jlimit(-1.0f, 1.0f, view.panParam->load(std::memory_order_acquire))
        : strip.getPan();

    state.usesGrainPlaybackSpeed = (strip.getPlayMode() == EnhancedAudioStrip::PlayMode::Grain);
    if (view.speedParam != nullptr)
    {
        const float clampedSpeed = juce::jlimit(0.0f, 8.0f, view.speedParam->load(std::memory_order_acquire));
        state.playbackSpeed = PlayheadSpeedQuantizer::grainPlaybackSpeedFromControl(clampedSpeed);
        state.playheadSpeedRatio = PlayheadSpeedQuantizer::quantizeRatio(juce::jlimit(0.125f, 8.0f, clampedSpeed));
    }
    else
    {
        state.playbackSpeed = strip.getPlaybackSpeed();
        state.playheadSpeedRatio = strip.getPlayheadSpeedRatio();
    }

    state.filterEnabled = (view.filterEnabledParam != nullptr)
        ? (view.filterEnabledParam->load(std::memory_order_acquire) > 0.5f)
        : strip.isFilterEnabled();
    state.filterFrequency = (view.filterFrequencyParam != nullptr)
        ? juce::jlimit(20.0f, 20000.0f, view.filterFrequencyParam->load(std::memory_order_acquire))
        : strip.getFilterFrequency();
    state.filterResonance = (view.filterResonanceParam != nullptr)
        ? juce::jlimit(0.1f, 10.0f, view.filterResonanceParam->load(std::memory_order_acquire))
        : strip.getFilterResonance();
    state.filterMorph = (view.filterMorphParam != nullptr)
        ? juce::jlimit(0.0f, 1.0f, view.filterMorphParam->load(std::memory_order_acquire))
        : juce::jlimit(0.0f, 1.0f, strip.getFilterMorph());
    state.filterAlgorithm = (view.filterAlgorithmParam != nullptr)
        ? static_cast<EnhancedAudioStrip::FilterAlgorithm>(juce::jlimit(
              0,
              5,
              static_cast<int>(view.filterAlgorithmParam->load(std::memory_order_acquire))))
        : strip.getFilterAlgorithm();

    state.delayMix = (view.delayMixParam != nullptr)
        ? juce::jlimit(0.0f, 1.0f, view.delayMixParam->load(std::memory_order_acquire))
        : strip.getDelayMix();
    state.delayTime = (view.delayTimeParam != nullptr)
        ? juce::jlimit(0.25f, 4.0f, view.delayTimeParam->load(std::memory_order_acquire))
        : strip.getDelayTimeBeats();
    state.delaySyncEnabled = (view.delaySyncParam != nullptr)
        ? (view.delaySyncParam->load(std::memory_order_acquire) > 0.5f)
        : strip.isDelaySyncEnabled();
    state.delayFeedback = (view.delayFeedbackParam != nullptr)
        ? juce::jlimit(0.0f, 0.97f, view.delayFeedbackParam->load(std::memory_order_acquire))
        : strip.getDelayFeedback();
    state.delayLowCutHz = (view.delayLowCutParam != nullptr)
        ? juce::jlimit(20.0f, 12000.0f, view.delayLowCutParam->load(std::memory_order_acquire))
        : strip.getDelayLowCutHz();
    state.delayHighCutHz = (view.delayHighCutParam != nullptr)
        ? juce::jlimit(200.0f, 20000.0f, view.delayHighCutParam->load(std::memory_order_acquire))
        : strip.getDelayHighCutHz();
    state.delayMode = (view.delayModeParam != nullptr)
        ? static_cast<EnhancedAudioStrip::DelayMode>(juce::jlimit(
              0,
              2,
              static_cast<int>(view.delayModeParam->load(std::memory_order_acquire))))
        : strip.getDelayMode();

    return state;
}

void applyOwnedControls(EnhancedAudioStrip& strip,
                        const ResolvedOwnedStripControlState& state)
{
    strip.setVolume(state.volume);
    if (auto* stepSampler = strip.getStepSampler())
        stepSampler->setVolume(state.volume);

    strip.setTrimDb(state.trimDb);

    strip.setPan(state.pan);
    if (auto* stepSampler = strip.getStepSampler())
        stepSampler->setPan(state.pan);

    if (state.usesGrainPlaybackSpeed)
        strip.setPlaybackSpeed(state.playbackSpeed);
    else
        strip.setPlayheadSpeedRatio(state.playheadSpeedRatio);
}

void applyFilter(EnhancedAudioStrip& strip,
                 const ResolvedOwnedStripControlState& state)
{
    strip.setFilterEnabled(state.filterEnabled);
    strip.setFilterFrequency(state.filterFrequency);
    strip.setFilterResonance(state.filterResonance);
    strip.setFilterMorph(state.filterMorph);
    strip.setFilterType(filterTypeFromMorphValue(state.filterMorph));
    strip.setFilterAlgorithm(state.filterAlgorithm);

    if (auto* stepSampler = strip.getStepSampler())
    {
        stepSampler->setFilterEnabled(state.filterEnabled);
        stepSampler->setFilterFrequency(state.filterFrequency);
        stepSampler->setFilterResonance(state.filterResonance);
        stepSampler->setFilterType(stepFilterTypeFromMorphValue(state.filterMorph));
    }
}

void applyDelay(EnhancedAudioStrip& strip,
                const ResolvedOwnedStripControlState& state)
{
    strip.setDelayMix(state.delayMix);
    strip.setDelayTimeBeats(state.delayTime);
    strip.setDelaySyncEnabled(state.delaySyncEnabled);
    strip.setDelayFeedback(state.delayFeedback);
    strip.setDelayLowCutHz(state.delayLowCutHz);
    strip.setDelayHighCutHz(state.delayHighCutHz);
    strip.setDelayMode(state.delayMode);
}
} // namespace StripControlState
