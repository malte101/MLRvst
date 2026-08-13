/*
  ==============================================================================

    PluginProcessorMacro.cpp
    Extracted macro assignment and owned strip-control handling for mlrVST.

  ==============================================================================
*/

#include "PluginProcessor.h"
#include "MacroTargetDispatcher.h"
#include "PlayheadSpeedQuantizer.h"
#include <array>

namespace
{
using MacroTarget = MlrVSTAudioProcessor::MacroTarget;

MacroTarget sanitizeMacroTarget(int rawTarget)
{
    return sanitizeMacroPerformanceTarget(performanceTargetFromRaw(rawTarget));
}

constexpr std::array<int, MlrVSTAudioProcessor::MacroCount> kAkaiMpkMiniMacroCcs {
    70, 71, 72, 73, -1, -1, -1, -1
};

struct ScopedSceneAutosaveSuppression
{
    explicit ScopedSceneAutosaveSuppression(MlrVSTAudioProcessor& p) : processor(p)
    {
        processor.beginSceneAutosaveSuppression();
    }

    ~ScopedSceneAutosaveSuppression()
    {
        processor.endSceneAutosaveSuppression();
    }

    MlrVSTAudioProcessor& processor;
};
} // namespace

int MlrVSTAudioProcessor::getDefaultMacroMidiCc(int macroIndex)
{
    if (macroIndex < 0 || macroIndex >= MacroCount)
        return -1;

    return kAkaiMpkMiniMacroCcs[static_cast<size_t>(macroIndex)];
}

int MlrVSTAudioProcessor::getMacroMidiCc(int macroIndex) const
{
    if (macroIndex < 0 || macroIndex >= MacroCount)
        return -1;

    return macroMidiCcAssignments[static_cast<size_t>(macroIndex)].load(std::memory_order_acquire);
}

MlrVSTAudioProcessor::MacroTarget MlrVSTAudioProcessor::getDefaultMacroTarget(int macroIndex)
{
    return MacroTargetDispatcher::getDefaultMacroTarget(macroIndex);
}

MlrVSTAudioProcessor::MacroTarget MlrVSTAudioProcessor::getMacroTarget(int macroIndex) const
{
    if (macroIndex < 0 || macroIndex >= MacroCount)
        return MacroTarget::None;

    return sanitizeMacroTarget(macroTargetAssignments[static_cast<size_t>(macroIndex)].load(std::memory_order_acquire));
}

void MlrVSTAudioProcessor::setMacroTarget(int macroIndex, MacroTarget target)
{
    if (macroIndex < 0 || macroIndex >= MacroCount)
        return;

    const auto previousTarget = getMacroTarget(macroIndex);
    if (previousTarget == MacroTarget::Retrigger && audioEngine != nullptr)
        setGlobalSceneStutterAmount(0.0f);

    macroTargetAssignments[static_cast<size_t>(macroIndex)].store(static_cast<int>(sanitizeMacroTarget(static_cast<int>(target))),
                                                                  std::memory_order_release);
    markPersistentGlobalUserChange();
}

float MlrVSTAudioProcessor::getDefaultMacroNormalizedValue(MacroTarget target)
{
    return MacroTargetDispatcher::getDefaultMacroNormalizedValue(target);
}

bool MlrVSTAudioProcessor::macroTargetWritesToSceneLane(MacroTarget target)
{
    switch (sanitizeMacroPerformanceTarget(target))
    {
        case MacroTarget::Speed:
        case MacroTarget::Pitch:
        case MacroTarget::Pan:
        case MacroTarget::Volume:
        case MacroTarget::Cutoff:
        case MacroTarget::Resonance:
        case MacroTarget::FilterMorph:
        case MacroTarget::SliceLength:
        case MacroTarget::Retrigger:
        case MacroTarget::Scratch:
        case MacroTarget::DelayMix:
        case MacroTarget::DelayTime:
        case MacroTarget::DelayFeedback:
        case MacroTarget::GrainSize:
        case MacroTarget::GrainDensity:
        case MacroTarget::GrainPitch:
        case MacroTarget::GrainPitchJitter:
        case MacroTarget::GrainSpread:
        case MacroTarget::GrainJitter:
        case MacroTarget::GrainPositionJitter:
        case MacroTarget::GrainRandom:
        case MacroTarget::GrainArp:
        case MacroTarget::GrainCloud:
        case MacroTarget::GrainEmitter:
        case MacroTarget::GrainEnvelope:
        case MacroTarget::GrainShape:
            return true;
        case MacroTarget::FilterEnable:
        case MacroTarget::DelayLowCut:
        case MacroTarget::DelayHighCut:
        case MacroTarget::Rearrange:
        case MacroTarget::None:
        default:
            return false;
    }
}

void MlrVSTAudioProcessor::beginMacroMidiLearn(int macroIndex)
{
    if (macroIndex < 0 || macroIndex >= MacroCount)
        return;

    macroMidiLearnIndex.store(macroIndex, std::memory_order_release);
}

void MlrVSTAudioProcessor::cancelMacroMidiLearn()
{
    macroMidiLearnIndex.store(-1, std::memory_order_release);
}

void MlrVSTAudioProcessor::resetMacroMidiCcToDefault(int macroIndex)
{
    if (macroIndex < 0 || macroIndex >= MacroCount)
        return;

    macroMidiCcAssignments[static_cast<size_t>(macroIndex)].store(getDefaultMacroMidiCc(macroIndex),
                                                                  std::memory_order_release);
    cancelMacroMidiLearn();
    markPersistentGlobalUserChange();
}

int MlrVSTAudioProcessor::getMacroTargetStripIndex() const
{
    const int activeStripCount = getMonomeActiveStripCount();
    const int maxStripIndex = (activeStripCount > 0) ? (activeStripCount - 1) : (MaxStrips - 1);
    return juce::jlimit(0, juce::jmax(0, maxStripIndex), getLastMonomePressedStripRow());
}

void MlrVSTAudioProcessor::setStripParameterValueFromMacro(int stripIndex,
                                                           const juce::String& parameterId,
                                                           float plainValue)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;

    if (auto* parameter = parameters.getParameter(parameterId))
    {
        const float normalized = juce::jlimit(0.0f, 1.0f, parameter->convertTo0to1(plainValue));
        parameter->setValueNotifyingHost(normalized);
    }
}

void MlrVSTAudioProcessor::writeStripFloatParameter(const juce::String& parameterId,
                                                    float plainValue,
                                                    std::atomic<float>* rawParam,
                                                    StripControlWriteMode writeMode)
{
    if (writeMode == StripControlWriteMode::NotifyHost)
    {
        if (auto* parameter = parameters.getParameter(parameterId))
        {
            parameter->setValueNotifyingHost(parameter->convertTo0to1(plainValue));
            return;
        }
    }

    if (rawParam != nullptr)
        rawParam->store(plainValue, std::memory_order_release);
}

void MlrVSTAudioProcessor::writeStripBoolParameter(const juce::String& parameterId,
                                                   bool enabled,
                                                   std::atomic<float>* rawParam,
                                                   StripControlWriteMode writeMode)
{
    writeStripFloatParameter(parameterId,
                             enabled ? 1.0f : 0.0f,
                             rawParam,
                             writeMode);
}

void MlrVSTAudioProcessor::setStripVolumeControlValue(int stripIndex,
                                                      float volume,
                                                      StripControlWriteMode writeMode)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;

    const float clamped = juce::jlimit(0.0f, 1.0f, volume);
    const auto arrayIndex = static_cast<size_t>(stripIndex);
    writeStripFloatParameter("stripVolume" + juce::String(stripIndex),
                             clamped,
                             stripVolumeParams[arrayIndex],
                             writeMode);

    if (audioEngine == nullptr)
        return;

    if (auto* strip = audioEngine->getStrip(stripIndex))
    {
        strip->setVolume(clamped);
        if (auto* stepSampler = strip->getStepSampler())
            stepSampler->setVolume(clamped);
    }
}

void MlrVSTAudioProcessor::setStripPanControlValue(int stripIndex,
                                                   float pan,
                                                   StripControlWriteMode writeMode)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;

    const float clamped = juce::jlimit(-1.0f, 1.0f, pan);
    const auto arrayIndex = static_cast<size_t>(stripIndex);
    writeStripFloatParameter("stripPan" + juce::String(stripIndex),
                             clamped,
                             stripPanParams[arrayIndex],
                             writeMode);

    if (audioEngine == nullptr)
        return;

    if (auto* strip = audioEngine->getStrip(stripIndex))
    {
        strip->setPan(clamped);
        if (auto* stepSampler = strip->getStepSampler())
            stepSampler->setPan(clamped);
    }
}

void MlrVSTAudioProcessor::setStripSpeedControlValue(int stripIndex,
                                                     float speed,
                                                     StripControlWriteMode writeMode)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;

    const float clamped = juce::jlimit(0.0f, 8.0f, speed);
    const auto arrayIndex = static_cast<size_t>(stripIndex);
    writeStripFloatParameter("stripSpeed" + juce::String(stripIndex),
                             clamped,
                             stripSpeedParams[arrayIndex],
                             writeMode);

    if (audioEngine == nullptr)
        return;

    if (auto* strip = audioEngine->getStrip(stripIndex))
    {
        if (strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Grain)
        {
            strip->setPlaybackSpeed(PlayheadSpeedQuantizer::grainPlaybackSpeedFromControl(clamped));
        }
        else
        {
            strip->setPlayheadSpeedRatio(
                PlayheadSpeedQuantizer::quantizeRatio(juce::jlimit(0.125f, 8.0f, clamped)));
        }
    }
}

void MlrVSTAudioProcessor::setStripFilterEnabledControlValue(int stripIndex,
                                                             bool enabled,
                                                             StripControlWriteMode writeMode)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;

    const auto arrayIndex = static_cast<size_t>(stripIndex);
    writeStripBoolParameter("stripFilterEnabled" + juce::String(stripIndex),
                            enabled,
                            stripFilterEnabledParams[arrayIndex],
                            writeMode);

    if (audioEngine == nullptr)
        return;

    if (auto* strip = audioEngine->getStrip(stripIndex))
    {
        auto state = resolveOwnedStripControlStateFromParameters(stripIndex, *strip);
        state.filterEnabled = enabled;
        applyResolvedStripFilterState(*strip, state);
    }
}

void MlrVSTAudioProcessor::setStripFilterFrequencyControlValue(int stripIndex,
                                                               float frequency,
                                                               StripControlWriteMode writeMode,
                                                               bool fastResponse)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;

    const float clamped = juce::jlimit(20.0f, 20000.0f, frequency);
    const auto arrayIndex = static_cast<size_t>(stripIndex);
    // The monome filter row (fastResponse) is an explicit "make the filter
    // audible" gesture, so it still enables. GUI knob pre-dialing does not.
    if (fastResponse)
        writeStripBoolParameter("stripFilterEnabled" + juce::String(stripIndex),
                                true,
                                stripFilterEnabledParams[arrayIndex],
                                writeMode);
    writeStripFloatParameter("stripFilterFrequency" + juce::String(stripIndex),
                             clamped,
                             stripFilterFrequencyParams[arrayIndex],
                             writeMode);

    if (audioEngine == nullptr)
        return;

    if (auto* strip = audioEngine->getStrip(stripIndex))
    {
        auto state = resolveOwnedStripControlStateFromParameters(stripIndex, *strip);
        if (fastResponse)
            state.filterEnabled = true;
        state.filterFrequency = clamped;
        const auto stepFilterType = state.filterMorph < 0.34f
            ? FilterType::LowPass
            : (state.filterMorph > 0.66f ? FilterType::HighPass
                                         : FilterType::BandPass);

        strip->setFilterEnabled(state.filterEnabled);
        if (fastResponse)
            strip->setFilterFrequencyMonomeFast(state.filterFrequency);
        else
            strip->setFilterFrequency(state.filterFrequency);
        strip->setFilterResonance(state.filterResonance);
        strip->setFilterMorph(state.filterMorph);
        strip->setFilterAlgorithm(state.filterAlgorithm);

        if (auto* stepSampler = strip->getStepSampler())
        {
            stepSampler->setFilterEnabled(state.filterEnabled);
            stepSampler->setFilterFrequency(state.filterFrequency);
            stepSampler->setFilterResonance(state.filterResonance);
            stepSampler->setFilterType(stepFilterType);
        }
    }
}

void MlrVSTAudioProcessor::setStripFilterResonanceControlValue(int stripIndex,
                                                               float resonance,
                                                               StripControlWriteMode writeMode)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;

    const float clamped = juce::jlimit(0.1f, 10.0f, resonance);
    const auto arrayIndex = static_cast<size_t>(stripIndex);
    writeStripFloatParameter("stripFilterResonance" + juce::String(stripIndex),
                             clamped,
                             stripFilterResonanceParams[arrayIndex],
                             writeMode);

    if (audioEngine == nullptr)
        return;

    if (auto* strip = audioEngine->getStrip(stripIndex))
    {
        auto state = resolveOwnedStripControlStateFromParameters(stripIndex, *strip);
        state.filterResonance = clamped;
        applyResolvedStripFilterState(*strip, state);
    }
}

void MlrVSTAudioProcessor::setStripFilterMorphControlValue(int stripIndex,
                                                           float morph,
                                                           StripControlWriteMode writeMode)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;

    const float clamped = juce::jlimit(0.0f, 1.0f, morph);
    const auto arrayIndex = static_cast<size_t>(stripIndex);
    writeStripFloatParameter("stripFilterMorph" + juce::String(stripIndex),
                             clamped,
                             stripFilterMorphParams[arrayIndex],
                             writeMode);

    if (audioEngine == nullptr)
        return;

    if (auto* strip = audioEngine->getStrip(stripIndex))
    {
        auto state = resolveOwnedStripControlStateFromParameters(stripIndex, *strip);
        state.filterMorph = clamped;
        applyResolvedStripFilterState(*strip, state);
    }
}

void MlrVSTAudioProcessor::setStripFilterAlgorithmControlValue(int stripIndex,
                                                               EnhancedAudioStrip::FilterAlgorithm algorithm,
                                                               StripControlWriteMode writeMode)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;

    const int clampedIndex = juce::jlimit(0, 6, static_cast<int>(algorithm));
    const auto arrayIndex = static_cast<size_t>(stripIndex);
    writeStripFloatParameter("stripFilterAlgorithm" + juce::String(stripIndex),
                             static_cast<float>(clampedIndex),
                             stripFilterAlgorithmParams[arrayIndex],
                             writeMode);

    // Deliberately no direct engine apply here: setFilterAlgorithm resets the
    // Moog/ladder model state, and doing that from the message thread races the
    // audio thread. The per-block parameter sync applies the change safely on
    // the audio thread via its own change detection.
}

void MlrVSTAudioProcessor::setStripDelayMixControlValue(int stripIndex,
                                                        float mix,
                                                        StripControlWriteMode writeMode)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;

    const auto arrayIndex = static_cast<size_t>(stripIndex);
    const float clamped = juce::jlimit(0.0f, 1.0f, mix);
    writeStripFloatParameter("stripDelayMix" + juce::String(stripIndex),
                             clamped,
                             stripDelayMixParams[arrayIndex],
                             writeMode);

    if (audioEngine == nullptr)
        return;

    if (auto* strip = audioEngine->getStrip(stripIndex))
    {
        auto state = resolveOwnedStripControlStateFromParameters(stripIndex, *strip);
        state.delayMix = clamped;
        applyResolvedStripDelayState(*strip, state);
    }
}

void MlrVSTAudioProcessor::setStripDelayTimeControlValue(int stripIndex,
                                                         float timeValue,
                                                         StripControlWriteMode writeMode)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;

    const auto arrayIndex = static_cast<size_t>(stripIndex);
    const float clamped = juce::jlimit(0.25f, 4.0f, timeValue);
    writeStripFloatParameter("stripDelayTime" + juce::String(stripIndex),
                             clamped,
                             stripDelayTimeParams[arrayIndex],
                             writeMode);

    if (audioEngine == nullptr)
        return;

    if (auto* strip = audioEngine->getStrip(stripIndex))
    {
        auto state = resolveOwnedStripControlStateFromParameters(stripIndex, *strip);
        state.delayTime = clamped;
        applyResolvedStripDelayState(*strip, state);
    }
}

void MlrVSTAudioProcessor::setStripDelaySyncEnabledControlValue(int stripIndex,
                                                                bool enabled,
                                                                StripControlWriteMode writeMode)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;

    const auto arrayIndex = static_cast<size_t>(stripIndex);
    writeStripBoolParameter("stripDelaySync" + juce::String(stripIndex),
                            enabled,
                            stripDelaySyncParams[arrayIndex],
                            writeMode);

    if (audioEngine == nullptr)
        return;

    if (auto* strip = audioEngine->getStrip(stripIndex))
    {
        auto state = resolveOwnedStripControlStateFromParameters(stripIndex, *strip);
        state.delaySyncEnabled = enabled;
        applyResolvedStripDelayState(*strip, state);
    }
}

void MlrVSTAudioProcessor::setStripDelayFeedbackControlValue(int stripIndex,
                                                             float feedback,
                                                             StripControlWriteMode writeMode)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;

    const auto arrayIndex = static_cast<size_t>(stripIndex);
    const float clamped = juce::jlimit(0.0f, 0.97f, feedback);
    writeStripFloatParameter("stripDelayFeedback" + juce::String(stripIndex),
                             clamped,
                             stripDelayFeedbackParams[arrayIndex],
                             writeMode);

    if (audioEngine == nullptr)
        return;

    if (auto* strip = audioEngine->getStrip(stripIndex))
    {
        auto state = resolveOwnedStripControlStateFromParameters(stripIndex, *strip);
        state.delayFeedback = clamped;
        applyResolvedStripDelayState(*strip, state);
    }
}

void MlrVSTAudioProcessor::setStripDelayLowCutControlValue(int stripIndex,
                                                           float hz,
                                                           StripControlWriteMode writeMode)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;

    const auto arrayIndex = static_cast<size_t>(stripIndex);
    const float clamped = juce::jlimit(20.0f, 12000.0f, hz);
    writeStripFloatParameter("stripDelayLowCut" + juce::String(stripIndex),
                             clamped,
                             stripDelayLowCutParams[arrayIndex],
                             writeMode);

    if (audioEngine == nullptr)
        return;

    if (auto* strip = audioEngine->getStrip(stripIndex))
    {
        auto state = resolveOwnedStripControlStateFromParameters(stripIndex, *strip);
        state.delayLowCutHz = clamped;
        applyResolvedStripDelayState(*strip, state);
    }
}

void MlrVSTAudioProcessor::setStripDelayHighCutControlValue(int stripIndex,
                                                            float hz,
                                                            StripControlWriteMode writeMode)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;

    const auto arrayIndex = static_cast<size_t>(stripIndex);
    const float clamped = juce::jlimit(200.0f, 20000.0f, hz);
    writeStripFloatParameter("stripDelayHighCut" + juce::String(stripIndex),
                             clamped,
                             stripDelayHighCutParams[arrayIndex],
                             writeMode);

    if (audioEngine == nullptr)
        return;

    if (auto* strip = audioEngine->getStrip(stripIndex))
    {
        auto state = resolveOwnedStripControlStateFromParameters(stripIndex, *strip);
        state.delayHighCutHz = clamped;
        applyResolvedStripDelayState(*strip, state);
    }
}

void MlrVSTAudioProcessor::setStripDelayModeControlValue(int stripIndex,
                                                         EnhancedAudioStrip::DelayMode mode,
                                                         StripControlWriteMode writeMode)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;

    const auto arrayIndex = static_cast<size_t>(stripIndex);
    const int clampedIndex = juce::jlimit(0, 2, static_cast<int>(mode));
    const auto safeMode = static_cast<EnhancedAudioStrip::DelayMode>(clampedIndex);
    writeStripFloatParameter("stripDelayMode" + juce::String(stripIndex),
                             static_cast<float>(clampedIndex),
                             stripDelayModeParams[arrayIndex],
                             writeMode);

    if (audioEngine == nullptr)
        return;

    if (auto* strip = audioEngine->getStrip(stripIndex))
    {
        auto state = resolveOwnedStripControlStateFromParameters(stripIndex, *strip);
        state.delayMode = safeMode;
        applyResolvedStripDelayState(*strip, state);
    }
}

StripControlState::ParameterView MlrVSTAudioProcessor::makeOwnedStripControlParameterView(int stripIndex) const
{
    StripControlState::ParameterView view;
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return view;

    const auto arrayIndex = static_cast<size_t>(stripIndex);
    view.volumeParam = stripVolumeParams[arrayIndex];
    view.trimDbParam = stripTrimDbParams[arrayIndex];
    view.panParam = stripPanParams[arrayIndex];
    view.speedParam = stripSpeedParams[arrayIndex];
    view.filterEnabledParam = stripFilterEnabledParams[arrayIndex];
    view.filterFrequencyParam = stripFilterFrequencyParams[arrayIndex];
    view.filterResonanceParam = stripFilterResonanceParams[arrayIndex];
    view.filterMorphParam = stripFilterMorphParams[arrayIndex];
    view.filterAlgorithmParam = stripFilterAlgorithmParams[arrayIndex];
    view.delayMixParam = stripDelayMixParams[arrayIndex];
    view.delayTimeParam = stripDelayTimeParams[arrayIndex];
    view.delaySyncParam = stripDelaySyncParams[arrayIndex];
    view.delayFeedbackParam = stripDelayFeedbackParams[arrayIndex];
    view.delayLowCutParam = stripDelayLowCutParams[arrayIndex];
    view.delayHighCutParam = stripDelayHighCutParams[arrayIndex];
    view.delayModeParam = stripDelayModeParams[arrayIndex];
    return view;
}

MlrVSTAudioProcessor::ResolvedOwnedStripControlState
MlrVSTAudioProcessor::resolveOwnedStripControlStateFromParameters(int stripIndex,
                                                                  const EnhancedAudioStrip& strip) const
{
    return StripControlState::resolveFromParameters(makeOwnedStripControlParameterView(stripIndex), strip);
}

void MlrVSTAudioProcessor::applyResolvedOwnedStripControlState(EnhancedAudioStrip& strip,
                                                               const ResolvedOwnedStripControlState& state) const
{
    StripControlState::applyOwnedControls(strip, state);
}

void MlrVSTAudioProcessor::applyResolvedStripFilterState(EnhancedAudioStrip& strip,
                                                         const ResolvedOwnedStripControlState& state) const
{
    StripControlState::applyFilter(strip, state);
}

void MlrVSTAudioProcessor::applyResolvedStripDelayState(EnhancedAudioStrip& strip,
                                                        const ResolvedOwnedStripControlState& state) const
{
    StripControlState::applyDelay(strip, state);
}

void MlrVSTAudioProcessor::applyOwnedStripControlsFromParameters(int stripIndex,
                                                                 EnhancedAudioStrip& strip)
{
    const auto state = resolveOwnedStripControlStateFromParameters(stripIndex, strip);
    applyResolvedOwnedStripControlState(strip, state);
    applyResolvedStripFilterState(strip, state);
    applyResolvedStripDelayState(strip, state);
}

float MlrVSTAudioProcessor::getMacroNormalizedValueForTarget(int stripIndex,
                                                             const EnhancedAudioStrip& strip,
                                                             MacroTarget target) const
{
    return MacroTargetDispatcher::getNormalizedValueForTarget(*this, stripIndex, strip, target);
}

void MlrVSTAudioProcessor::applyMacroTargetValue(int stripIndex,
                                                 EnhancedAudioStrip& strip,
                                                 MacroTarget target,
                                                 float normalizedValue)
{
    MacroTargetDispatcher::applyTargetValue(*this, stripIndex, strip, target, normalizedValue);

    switch (sanitizeMacroPerformanceTarget(target))
    {
        case MacroTarget::Pan:
            refreshMomentaryStutterSavedStateFromCurrentStrip(stripIndex, ScenePerformanceControlTarget::Pan);
            break;
        case MacroTarget::Speed:
            refreshMomentaryStutterSavedStateFromCurrentStrip(stripIndex, ScenePerformanceControlTarget::Speed);
            break;
        case MacroTarget::Pitch:
            refreshMomentaryStutterSavedStateFromCurrentStrip(stripIndex, ScenePerformanceControlTarget::Pitch);
            break;
        case MacroTarget::Cutoff:
            refreshMomentaryStutterSavedStateFromCurrentStrip(stripIndex, ScenePerformanceControlTarget::FilterFrequency);
            break;
        case MacroTarget::Resonance:
            refreshMomentaryStutterSavedStateFromCurrentStrip(stripIndex, ScenePerformanceControlTarget::FilterResonance);
            break;
        case MacroTarget::FilterMorph:
            refreshMomentaryStutterSavedStateFromCurrentStrip(stripIndex, ScenePerformanceControlTarget::FilterMorph);
            break;
        case MacroTarget::FilterEnable:
            refreshMomentaryStutterSavedStateFromCurrentStrip(stripIndex, ScenePerformanceControlTarget::FilterEnabled);
            break;
        case MacroTarget::SliceLength:
            refreshMomentaryStutterSavedStateFromCurrentStrip(stripIndex, ScenePerformanceControlTarget::SliceLength);
            break;
        case MacroTarget::None:
        case MacroTarget::Volume:
        case MacroTarget::GrainSize:
        case MacroTarget::GrainDensity:
        case MacroTarget::GrainPitch:
        case MacroTarget::GrainPitchJitter:
        case MacroTarget::GrainSpread:
        case MacroTarget::GrainJitter:
        case MacroTarget::GrainRandom:
        case MacroTarget::GrainArp:
        case MacroTarget::GrainCloud:
        case MacroTarget::GrainEmitter:
        case MacroTarget::GrainEnvelope:
        case MacroTarget::Retrigger:
        case MacroTarget::GrainPositionJitter:
        case MacroTarget::GrainShape:
        case MacroTarget::Scratch:
        case MacroTarget::Rearrange:
        case MacroTarget::DelayMix:
        case MacroTarget::DelayTime:
        case MacroTarget::DelayFeedback:
        case MacroTarget::DelayLowCut:
        case MacroTarget::DelayHighCut:
        default:
            break;
    }
}

void MlrVSTAudioProcessor::refreshMomentaryStutterSavedStateFromCurrentStrip(int stripIndex,
                                                                             ScenePerformanceControlTarget target)
{
    if (!momentaryStutterHoldActive
        || momentaryStutterPlaybackActive.load(std::memory_order_acquire) == 0
        || audioEngine == nullptr
        || stripIndex < 0
        || stripIndex >= MaxStrips)
    {
        return;
    }

    const auto idx = static_cast<size_t>(stripIndex);
    auto& saved = momentaryStutterSavedState[idx];
    if (!saved.valid || !momentaryStutterStripArmed[idx])
        return;

    auto* strip = audioEngine->getStrip(stripIndex);
    if (strip == nullptr)
        return;

    auto* stepSampler = saved.stepMode ? strip->getStepSampler() : nullptr;

    switch (target)
    {
        case ScenePerformanceControlTarget::Pan:
            saved.pan = (saved.stepMode && stepSampler != nullptr) ? stepSampler->getPan() : strip->getPan();
            break;
        case ScenePerformanceControlTarget::Speed:
            saved.playbackSpeed = strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Grain
                ? strip->getPlaybackSpeed()
                : strip->getPlayheadSpeedRatio();
            break;
        case ScenePerformanceControlTarget::Pitch:
        case ScenePerformanceControlTarget::GrainPitch:
            if (saved.stepMode)
                saved.pitchSemitones = getPitchSemitonesForDisplay(*strip);
            else
                saved.pitchShift = strip->getPitchShift();
            break;
        case ScenePerformanceControlTarget::SliceLength:
            saved.loopSliceLength = strip->getLoopSliceLength();
            break;
        case ScenePerformanceControlTarget::FilterEnabled:
            saved.filterEnabled = strip->isFilterEnabled();
            if (saved.stepMode && stepSampler != nullptr)
                saved.stepFilterEnabled = stepSampler->isFilterEnabled();
            break;
        case ScenePerformanceControlTarget::FilterFrequency:
            saved.filterEnabled = strip->isFilterEnabled();
            saved.filterFrequency = strip->getFilterFrequency();
            if (saved.stepMode && stepSampler != nullptr)
            {
                saved.stepFilterEnabled = stepSampler->isFilterEnabled();
                saved.stepFilterFrequency = stepSampler->getFilterFrequency();
                saved.stepFilterType = stepSampler->getFilterType();
            }
            break;
        case ScenePerformanceControlTarget::FilterResonance:
            saved.filterEnabled = strip->isFilterEnabled();
            saved.filterResonance = strip->getFilterResonance();
            if (saved.stepMode && stepSampler != nullptr)
                saved.stepFilterResonance = stepSampler->getFilterResonance();
            break;
        case ScenePerformanceControlTarget::FilterMorph:
            saved.filterEnabled = strip->isFilterEnabled();
            saved.filterMorph = strip->getFilterMorph();
            if (saved.stepMode && stepSampler != nullptr)
            {
                saved.stepFilterEnabled = stepSampler->isFilterEnabled();
                saved.stepFilterType = stepSampler->getFilterType();
            }
            break;
        case ScenePerformanceControlTarget::None:
        case ScenePerformanceControlTarget::Volume:
        case ScenePerformanceControlTarget::Swing:
        case ScenePerformanceControlTarget::GrainSize:
        case ScenePerformanceControlTarget::GrainDensity:
        case ScenePerformanceControlTarget::GrainPitchJitter:
        case ScenePerformanceControlTarget::GrainSpread:
        case ScenePerformanceControlTarget::GrainJitter:
        case ScenePerformanceControlTarget::GrainPositionJitter:
        case ScenePerformanceControlTarget::GrainRandomDepth:
        case ScenePerformanceControlTarget::GrainArp:
        case ScenePerformanceControlTarget::GrainCloud:
        case ScenePerformanceControlTarget::GrainEmitter:
        case ScenePerformanceControlTarget::GrainEnvelope:
        case ScenePerformanceControlTarget::GrainShape:
        case ScenePerformanceControlTarget::DelayMix:
        case ScenePerformanceControlTarget::DelayTime:
        case ScenePerformanceControlTarget::DelayFeedback:
        case ScenePerformanceControlTarget::DelayLowCut:
        case ScenePerformanceControlTarget::DelayHighCut:
        case ScenePerformanceControlTarget::DelayMode:
        case ScenePerformanceControlTarget::DelaySyncEnabled:
        case ScenePerformanceControlTarget::Retrigger:
        case ScenePerformanceControlTarget::Scratch:
        case ScenePerformanceControlTarget::Rearrange:
        default:
            break;
    }
}

MlrVSTAudioProcessor::MacroState MlrVSTAudioProcessor::getMacroState() const
{
    MacroState state{};
    state.stripIndex = getMacroTargetStripIndex();

    if (!audioEngine)
        return state;

    auto* strip = audioEngine->getStrip(state.stripIndex);
    if (strip == nullptr)
        return state;

    state.hasTargetStrip = true;

    for (int macroIndex = 0; macroIndex < MacroCount; ++macroIndex)
        state.values[static_cast<size_t>(macroIndex)] = getMacroNormalizedValueForTarget(state.stripIndex,
                                                                                         *strip,
                                                                                         getMacroTarget(macroIndex));

    return state;
}

void MlrVSTAudioProcessor::setSelectedStripMacroValue(int macroIndex, float normalizedValue)
{
    if (!audioEngine || macroIndex < 0 || macroIndex >= MacroCount)
        return;

    const int stripIndex = getMacroTargetStripIndex();
    auto* strip = audioEngine->getStrip(stripIndex);
    if (strip == nullptr)
        return;

    const auto target = getMacroTarget(macroIndex);
    ScenePerformanceEvent sceneEvent;
    const bool hasSceneEvent = macroTargetWritesToSceneLane(target)
        && resolveScenePerformanceMacroEvent(stripIndex, target, normalizedValue, sceneEvent);

    if (hasSceneEvent)
    {
        applyLiveSceneControlTouch(sceneEvent.stripIndex,
                                   sceneEvent.controlTarget,
                                   static_cast<ControlMode>(sceneEvent.controlMode),
                                   sceneEvent.controlRow,
                                   sceneEvent.value,
                                   sceneEvent.column,
                                   [this, stripIndex, target, normalizedValue](StripControlWriteMode)
                                   {
                                       if (audioEngine == nullptr)
                                           return;
                                       if (auto* targetStrip = audioEngine->getStrip(stripIndex))
                                           applyMacroTargetValue(stripIndex, *targetStrip, target, normalizedValue);
                                   });
        return;
    }

    ScopedSceneAutosaveSuppression suppressSceneAutosave(*this);
    beginSceneManualControlHandlingSuppression();
    applyMacroTargetValue(stripIndex, *strip, target, normalizedValue);
    endSceneManualControlHandlingSuppression();
}

void MlrVSTAudioProcessor::handleIncomingMacroCc(const juce::MidiBuffer& midiMessages)
{
    for (const auto metadata : midiMessages)
    {
        const auto message = metadata.getMessage();
        if (!message.isController())
            continue;

        const int controllerNumber = message.getControllerNumber();
        const int learnIndex = macroMidiLearnIndex.load(std::memory_order_acquire);
        if (learnIndex >= 0 && learnIndex < MacroCount)
        {
            macroMidiCcAssignments[static_cast<size_t>(learnIndex)].store(controllerNumber, std::memory_order_release);
            macroMidiLearnIndex.store(-1, std::memory_order_release);
            markPersistentGlobalUserChange();
            setSelectedStripMacroValue(learnIndex,
                                       static_cast<float>(message.getControllerValue()) / 127.0f);
            continue;
        }

        for (int macroIndex = 0; macroIndex < MacroCount; ++macroIndex)
        {
            if (controllerNumber != getMacroMidiCc(macroIndex))
                continue;

            setSelectedStripMacroValue(macroIndex,
                                       static_cast<float>(message.getControllerValue()) / 127.0f);
            break;
        }
    }
}
