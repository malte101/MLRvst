/*
  ==============================================================================

    AudioEngineModulation.cpp
    Modulation helpers and lane management for the audio engine

  ==============================================================================
*/

#include "AudioEngineModulation.h"
#include "PlayheadSpeedQuantizer.h"

namespace AudioEngineModulationInternal
{
bool modTargetAutoDefaultBipolar(ModernAudioEngine::ModTarget target)
{
    return performanceTargetAutoDefaultBipolar(sanitizeModPerformanceTarget(target));
}

bool isGrainModTarget(ModernAudioEngine::ModTarget target)
{
    return target == ModernAudioEngine::ModTarget::GrainSize
        || target == ModernAudioEngine::ModTarget::GrainDensity
        || target == ModernAudioEngine::ModTarget::GrainPitch
        || target == ModernAudioEngine::ModTarget::GrainPitchJitter
        || target == ModernAudioEngine::ModTarget::GrainSpread
        || target == ModernAudioEngine::ModTarget::GrainJitter
        || target == ModernAudioEngine::ModTarget::GrainRandom
        || target == ModernAudioEngine::ModTarget::GrainArp
        || target == ModernAudioEngine::ModTarget::GrainCloud
        || target == ModernAudioEngine::ModTarget::GrainEmitter
        || target == ModernAudioEngine::ModTarget::GrainEnvelope
        || target == ModernAudioEngine::ModTarget::GrainPositionJitter
        || target == ModernAudioEngine::ModTarget::GrainShape;
}

bool modTargetUsesForcedBipolar(ModernAudioEngine::ModTarget target)
{
    return target == ModernAudioEngine::ModTarget::Speed;
}

float quantizeModLaneRate(float rate) noexcept
{
    return PlayheadSpeedQuantizer::quantizeRatio(juce::jlimit(0.125f, 8.0f, rate));
}

float defaultRearrangeStepValueForIndex(int absoluteStep, int totalSteps) noexcept
{
    if (totalSteps <= 1)
        return 0.0f;

    const int wrappedStep = juce::jlimit(0, juce::jmax(0, totalSteps - 1), absoluteStep);
    return juce::jlimit(0.0f, 1.0f,
                        static_cast<float>(wrappedStep)
                            / static_cast<float>(juce::jmax(1, totalSteps - 1)));
}

float filterCutoffToNormalized(float hz) noexcept
{
    const float safeHz = juce::jlimit(20.0f, 20000.0f, hz);
    return juce::jlimit(0.0f, 1.0f,
                        static_cast<float>(std::log(static_cast<double>(safeHz / 20.0f))
                            / std::log(1000.0)));
}

float normalizedToFilterCutoff(float normalized) noexcept
{
    return 20.0f * std::pow(1000.0f, juce::jlimit(0.0f, 1.0f, normalized));
}

float filterResonanceToNormalized(float q) noexcept
{
    return juce::jmap(juce::jlimit(0.1f, 10.0f, q), 0.1f, 10.0f, 0.0f, 1.0f);
}

float normalizedToFilterResonance(float normalized) noexcept
{
    return juce::jmap(juce::jlimit(0.0f, 1.0f, normalized), 0.0f, 1.0f, 0.1f, 10.0f);
}

const juce::NormalisableRange<float>& delayTimeRange() noexcept
{
    static const juce::NormalisableRange<float> range(0.25f, 4.0f, 0.001f, 0.45f);
    return range;
}

const juce::NormalisableRange<float>& delayLowCutRange() noexcept
{
    static const juce::NormalisableRange<float> range(20.0f, 12000.0f, 1.0f, 0.25f);
    return range;
}

const juce::NormalisableRange<float>& delayHighCutRange() noexcept
{
    static const juce::NormalisableRange<float> range(200.0f, 20000.0f, 1.0f, 0.3f);
    return range;
}

int rearrangeSliceIndexFromNormalized(float value01, int localSliceCount) noexcept
{
    const int safeLocalSliceCount = juce::jmax(1, localSliceCount);
    const float normalized = juce::jlimit(0.0f, 1.0f, value01);
    const int sourceIndex16 = juce::jlimit(0, ModernAudioEngine::MaxColumns - 1,
                                           static_cast<int>(std::round(normalized * static_cast<float>(ModernAudioEngine::MaxColumns - 1))));
    if (safeLocalSliceCount <= 1)
        return 0;

    return juce::jlimit(0, safeLocalSliceCount - 1,
                        static_cast<int>(std::round((static_cast<float>(sourceIndex16)
                                                      / static_cast<float>(ModernAudioEngine::MaxColumns - 1))
                                                     * static_cast<float>(safeLocalSliceCount - 1))));
}

float convertModValueForBipolarMode(float value01,
                                    bool wasBipolar,
                                    bool willBeBipolar,
                                    ModernAudioEngine::ModBipolarToggleMode mode)
{
    const float clamped = juce::jlimit(0.0f, 1.0f, value01);
    if (mode == ModernAudioEngine::ModBipolarToggleMode::Reinterpret || wasBipolar == willBeBipolar)
        return clamped;

    if (!wasBipolar && willBeBipolar)
        return 0.5f + (0.5f * clamped);

    return juce::jlimit(0.0f, 1.0f, (clamped - 0.5f) * 2.0f);
}
} // namespace AudioEngineModulationInternal

using namespace AudioEngineModulationInternal;

void EnhancedAudioStrip::setTraversalRearrange(float value01, float depth01)
{
    traversalRearrangeValue.store(juce::jlimit(0.0f, 1.0f, value01), std::memory_order_release);
    traversalRearrangeDepth.store(juce::jlimit(0.0f, 1.0f, depth01), std::memory_order_release);
    traversalRearrangeActive.store(depth01 > 1.0e-4f ? 1 : 0, std::memory_order_release);
}

void EnhancedAudioStrip::clearTraversalRearrange()
{
    traversalRearrangeActive.store(0, std::memory_order_release);
    traversalRearrangeDepth.store(0.0f, std::memory_order_release);
    traversalRearrangeValue.store(0.0f, std::memory_order_release);
    traversalRearrangeLastSourceSlice = -1;
    traversalRearrangeLastTargetSlice = -1;
    traversalRearrangeLastTargetCycle = -1;
}

ModernAudioEngine::ModTarget ModernAudioEngine::defaultModTargetForSlot(int slot)
{
    switch (slot)
    {
        case 0: return ModTarget::Volume;
        case 1: return ModTarget::Pan;
        case 2: return ModTarget::Pitch;
        case 3: return ModTarget::Cutoff;
        case 4: return ModTarget::Resonance;
        case 5: return ModTarget::Retrigger;
        default: return ModTarget::None;
    }
}

float ModernAudioEngine::defaultModStepValueForTarget(ModTarget target)
{
    switch (target)
    {
        case ModTarget::Volume:
            return 1.0f;
        case ModTarget::Pan:
            return 0.5f;
        case ModTarget::Pitch:
            return 0.5f;
        case ModTarget::Speed:
            return 0.5f;
        case ModTarget::Cutoff:
            return 0.5f;
        case ModTarget::Resonance:
            return 0.0f;
        case ModTarget::GrainSize:
            return 0.5f;
        case ModTarget::GrainDensity:
            return 0.0f;
        case ModTarget::GrainPitch:
            return 0.5f;
        case ModTarget::GrainPitchJitter:
            return 0.0f;
        case ModTarget::GrainSpread:
            return 0.5f;
        case ModTarget::GrainJitter:
            return 0.0f;
        case ModTarget::GrainRandom:
            return 0.0f;
        case ModTarget::GrainArp:
            return 0.0f;
        case ModTarget::GrainCloud:
            return 0.0f;
        case ModTarget::GrainEmitter:
            return 0.0f;
        case ModTarget::GrainEnvelope:
            return 0.0f;
        case ModTarget::Retrigger:
            return 0.0f;
        case ModTarget::GrainPositionJitter:
            return 0.5f;
        case ModTarget::GrainShape:
            return 0.5f;
        case ModTarget::FilterMorph:
            return 0.5f;
        case ModTarget::DelayMix:
            return 0.0f;
        case ModTarget::DelayTime:
            return 0.5f;
        case ModTarget::DelayFeedback:
            return 0.0f;
        case ModTarget::DelayLowCut:
            return 0.0f;
        case ModTarget::DelayHighCut:
            return 1.0f;
        case ModTarget::FilterEnable:
            return 0.0f;
        case ModTarget::SliceLength:
            return 0.5f;
        case ModTarget::Scratch:
            return 0.0f;
        case ModTarget::Rearrange:
            return 0.0f;
        case ModTarget::None:
        default:
            return 0.0f;
    }
}

bool ModernAudioEngine::modTargetSupportsBipolar(ModTarget target)
{
    return performanceTargetSupportsBipolar(sanitizeModPerformanceTarget(target));
}

void ModernAudioEngine::queueModSequencerRuntimeReset(ModSequencer& seq,
                                                      float rawSeed,
                                                      float grainSeed,
                                                      float pitchSeed,
                                                      float speedTargetSeed)
{
    const float clampedRaw = juce::jlimit(0.0f, 1.0f, rawSeed);
    const float clampedGrain = juce::jlimit(0.0f, 1.0f, grainSeed);
    const float clampedPitch = juce::jlimit(0.0f, 1.0f, pitchSeed);
    const float clampedSpeedTarget = (std::isfinite(speedTargetSeed) && speedTargetSeed >= 0.0f)
        ? juce::jlimit(0.125f, 8.0f, speedTargetSeed)
        : -1.0f;

    seq.runtimeResetSeedRaw.store(clampedRaw, std::memory_order_release);
    seq.runtimeResetSeedGrain.store(clampedGrain, std::memory_order_release);
    seq.runtimeResetSeedPitch.store(clampedPitch, std::memory_order_release);
    seq.runtimeResetSeedSpeedTarget.store(clampedSpeedTarget, std::memory_order_release);
    seq.runtimeRawSnapshot.store(clampedRaw, std::memory_order_release);
    seq.runtimeGrainSnapshot.store(clampedGrain, std::memory_order_release);
    seq.runtimePitchSnapshot.store(clampedPitch, std::memory_order_release);
    seq.runtimeSpeedTargetSnapshot.store(clampedSpeedTarget, std::memory_order_release);
    seq.runtimeResetGeneration.fetch_add(1u, std::memory_order_acq_rel);
}

void ModernAudioEngine::queueModSequencerRuntimeResetFromSnapshots(ModSequencer& seq,
                                                                   bool clearSpeedTarget)
{
    const float speedTargetSeed = clearSpeedTarget
        ? -1.0f
        : seq.runtimeSpeedTargetSnapshot.load(std::memory_order_acquire);
    queueModSequencerRuntimeReset(seq,
                                  seq.runtimeRawSnapshot.load(std::memory_order_acquire),
                                  seq.runtimeGrainSnapshot.load(std::memory_order_acquire),
                                  seq.runtimePitchSnapshot.load(std::memory_order_acquire),
                                  speedTargetSeed);
}

void ModernAudioEngine::setModTargetOnSequencer(ModSequencer& seq, ModTarget target)
{
    target = sanitizeModPerformanceTarget(target);
    const bool currentBipolar = seq.bipolar.load(std::memory_order_acquire) != 0;
    const bool nextBipolar = modTargetSupportsBipolar(target)
        && (modTargetUsesForcedBipolar(target) || modTargetAutoDefaultBipolar(target));
    float rawSeed = seq.runtimeRawSnapshot.load(std::memory_order_acquire);
    float grainSeed = seq.runtimeGrainSnapshot.load(std::memory_order_acquire);
    float pitchSeed = seq.runtimePitchSnapshot.load(std::memory_order_acquire);
    if (currentBipolar != nextBipolar)
    {
        for (auto& step : seq.steps)
        {
            step.store(convertModValueForBipolarMode(step.load(std::memory_order_acquire),
                                                     currentBipolar,
                                                     nextBipolar,
                                                     ModBipolarToggleMode::ConvertPreserveNeutral),
                       std::memory_order_release);
        }
        for (auto& stepEnd : seq.stepEndValues)
        {
            stepEnd.store(convertModValueForBipolarMode(stepEnd.load(std::memory_order_acquire),
                                                        currentBipolar,
                                                        nextBipolar,
                                                        ModBipolarToggleMode::ConvertPreserveNeutral),
                          std::memory_order_release);
        }
        rawSeed = convertModValueForBipolarMode(rawSeed,
                                                currentBipolar,
                                                nextBipolar,
                                                ModBipolarToggleMode::ConvertPreserveNeutral);
        grainSeed = convertModValueForBipolarMode(grainSeed,
                                                  currentBipolar,
                                                  nextBipolar,
                                                  ModBipolarToggleMode::ConvertPreserveNeutral);
        pitchSeed = convertModValueForBipolarMode(pitchSeed,
                                                  currentBipolar,
                                                  nextBipolar,
                                                  ModBipolarToggleMode::ConvertPreserveNeutral);
    }
    seq.target.store(static_cast<int>(target), std::memory_order_release);
    seq.bipolar.store(nextBipolar ? 1 : 0, std::memory_order_release);

    if (target == ModTarget::Pitch && seq.smoothingMs.load(std::memory_order_acquire) < 1.0f)
        seq.smoothingMs.store(12.0f, std::memory_order_release);
    else if (target == ModTarget::Speed && seq.smoothingMs.load(std::memory_order_acquire) < 1.0f)
        seq.smoothingMs.store(20.0f, std::memory_order_release);

    queueModSequencerRuntimeReset(seq,
                                  rawSeed,
                                  grainSeed,
                                  pitchSeed,
                                  (target == ModTarget::Speed)
                                      ? seq.runtimeSpeedTargetSnapshot.load(std::memory_order_acquire)
                                      : -1.0f);
}

void ModernAudioEngine::setModBipolarOnSequencer(ModSequencer& seq, bool bipolar, ModBipolarToggleMode mode)
{
    const auto target = static_cast<ModTarget>(seq.target.load(std::memory_order_acquire));
    const bool currentBipolar = seq.bipolar.load(std::memory_order_acquire) != 0;
    const bool nextBipolar = modTargetSupportsBipolar(target)
        && (modTargetUsesForcedBipolar(target) || bipolar);
    float rawSeed = seq.runtimeRawSnapshot.load(std::memory_order_acquire);
    float grainSeed = seq.runtimeGrainSnapshot.load(std::memory_order_acquire);
    float pitchSeed = seq.runtimePitchSnapshot.load(std::memory_order_acquire);
    if (currentBipolar != nextBipolar)
    {
        for (auto& step : seq.steps)
        {
            step.store(convertModValueForBipolarMode(step.load(std::memory_order_acquire),
                                                     currentBipolar,
                                                     nextBipolar,
                                                     mode),
                       std::memory_order_release);
        }
        for (auto& stepEnd : seq.stepEndValues)
        {
            stepEnd.store(convertModValueForBipolarMode(stepEnd.load(std::memory_order_acquire),
                                                        currentBipolar,
                                                        nextBipolar,
                                                        mode),
                          std::memory_order_release);
        }
        rawSeed = convertModValueForBipolarMode(rawSeed, currentBipolar, nextBipolar, mode);
        grainSeed = convertModValueForBipolarMode(grainSeed, currentBipolar, nextBipolar, mode);
        pitchSeed = convertModValueForBipolarMode(pitchSeed, currentBipolar, nextBipolar, mode);
    }
    seq.bipolar.store(nextBipolar ? 1 : 0, std::memory_order_release);
    for (auto& step : seq.steps)
    {
        const float clamped = juce::jlimit(0.0f, 1.0f, step.load(std::memory_order_acquire));
        step.store(clamped, std::memory_order_release);
    }
    for (auto& stepEnd : seq.stepEndValues)
    {
        const float clamped = juce::jlimit(0.0f, 1.0f, stepEnd.load(std::memory_order_acquire));
        stepEnd.store(clamped, std::memory_order_release);
    }

    queueModSequencerRuntimeReset(seq,
                                  rawSeed,
                                  grainSeed,
                                  pitchSeed,
                                  (target == ModTarget::Speed)
                                      ? seq.runtimeSpeedTargetSnapshot.load(std::memory_order_acquire)
                                      : -1.0f);
}

void ModernAudioEngine::setModSequencerSlot(int stripIndex, int slot)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;

    const int safeSlot = sanitizeModSequencerSlot(slot);
    activeModSequencerSlots[static_cast<size_t>(stripIndex)].store(safeSlot, std::memory_order_release);

    auto& seq = getActiveModSequencer(stripIndex);
    const int maxPage = juce::jmax(0, getModLengthBars(stripIndex) - 1);
    const int clampedPage = juce::jlimit(0, maxPage, seq.editPage.load(std::memory_order_acquire));
    seq.editPage.store(clampedPage, std::memory_order_release);
}

int ModernAudioEngine::getModSequencerSlot(int stripIndex) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return 0;

    return getActiveModSequencerSlot(stripIndex);
}

void ModernAudioEngine::resetModSequencerSlotToDefaults(int stripIndex, int slot)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;

    const int safeSlot = sanitizeModSequencerSlot(slot);
    auto& seq = getModSequencer(stripIndex, safeSlot);
    const auto target = defaultModTargetForSlot(safeSlot);
    const bool bipolar = modTargetSupportsBipolar(target) && modTargetAutoDefaultBipolar(target);
    const float defaultValue = defaultModStepValueForTarget(target);

    seq.target.store(static_cast<int>(target), std::memory_order_release);
    seq.bipolar.store(bipolar ? 1 : 0, std::memory_order_release);
    seq.curveMode.store(0, std::memory_order_release);
    seq.depth.store(1.0f, std::memory_order_release);
    seq.rate.store(1.0f, std::memory_order_release);
    seq.transportMode.store(static_cast<int>(ModTransportMode::Sync), std::memory_order_release);
    seq.offset.store(0, std::memory_order_release);
    seq.lengthBars.store(1, std::memory_order_release);
    seq.editPage.store(0, std::memory_order_release);
    seq.smoothingMs.store(0.0f, std::memory_order_release);
    seq.curveBend.store(0.0f, std::memory_order_release);
    seq.curveShape.store(static_cast<int>(ModCurveShape::Linear), std::memory_order_release);
    seq.pitchScaleQuantize.store(0, std::memory_order_release);
    seq.pitchScale.store(static_cast<int>(PitchScale::Chromatic), std::memory_order_release);
    seq.lastGlobalStep.store(0, std::memory_order_release);
    for (size_t stepIndex = 0; stepIndex < seq.steps.size(); ++stepIndex)
    {
        const float stepValue = (target == ModTarget::Rearrange)
            ? defaultRearrangeStepValueForIndex(static_cast<int>(stepIndex % static_cast<size_t>(ModSteps)),
                                                ModSteps)
            : defaultValue;
        seq.steps[stepIndex].store(stepValue, std::memory_order_release);
        seq.stepSubdivisions[stepIndex].store(1, std::memory_order_release);
        seq.stepEndValues[stepIndex].store(stepValue, std::memory_order_release);
        seq.stepCurveShapes[stepIndex].store(static_cast<int>(ModCurveShape::Linear), std::memory_order_release);
    }

    queueModSequencerRuntimeReset(seq, defaultValue, defaultValue, defaultValue, -1.0f);
}

ModernAudioEngine::ModSequencerState ModernAudioEngine::getModSequencerState(int stripIndex) const
{
    ModSequencerState state{};
    state.stepSubdivisions.fill(1);
    state.stepEndValues.fill(0.0f);
    state.stepCurveShapes.fill(static_cast<int>(ModCurveShape::Linear));
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return state;

    const auto& seq = getActiveModSequencer(stripIndex);
    state.target = static_cast<ModTarget>(seq.target.load(std::memory_order_acquire));
    state.bipolar = modTargetSupportsBipolar(state.target)
        && (modTargetUsesForcedBipolar(state.target) || (seq.bipolar.load(std::memory_order_acquire) != 0));
    state.curveMode = (seq.curveMode.load(std::memory_order_acquire) != 0);
    state.depth = seq.depth.load(std::memory_order_acquire);
    state.rate = quantizeModLaneRate(seq.rate.load(std::memory_order_acquire));
    state.transportMode = juce::jlimit(0, static_cast<int>(ModTransportMode::Scene),
                                       seq.transportMode.load(std::memory_order_acquire));
    state.offset = seq.offset.load(std::memory_order_acquire);
    state.lengthBars = juce::jlimit(1, MaxModBars, seq.lengthBars.load(std::memory_order_acquire));
    state.editPage = juce::jlimit(0, MaxModBars - 1, seq.editPage.load(std::memory_order_acquire));
    state.smoothingMs = juce::jmax(0.0f, seq.smoothingMs.load(std::memory_order_acquire));
    state.curveBend = juce::jlimit(-1.0f, 1.0f, seq.curveBend.load(std::memory_order_acquire));
    state.curveShape = juce::jlimit(0, static_cast<int>(ModCurveShape::Square), seq.curveShape.load(std::memory_order_acquire));
    state.pitchScaleQuantize = (seq.pitchScaleQuantize.load(std::memory_order_acquire) != 0);
    state.pitchScale = juce::jlimit(0, static_cast<int>(PitchScale::PentatonicMinor), seq.pitchScale.load(std::memory_order_acquire));
    const int pageOffset = state.editPage * ModSteps;
    for (int i = 0; i < ModSteps; ++i)
    {
        const int absoluteStep = pageOffset + i;
        const float startValue = juce::jlimit(
            0.0f, 1.0f, seq.steps[static_cast<size_t>(absoluteStep)].load(std::memory_order_acquire));
        const int subdivisions = juce::jlimit(
            1, ModMaxStepSubdivisions, seq.stepSubdivisions[static_cast<size_t>(absoluteStep)].load(std::memory_order_acquire));
        float endValue = juce::jlimit(
            0.0f, 1.0f, seq.stepEndValues[static_cast<size_t>(absoluteStep)].load(std::memory_order_acquire));
        const int curveShape = juce::jlimit(
            0,
            static_cast<int>(ModCurveShape::Square),
            seq.stepCurveShapes[static_cast<size_t>(absoluteStep)].load(std::memory_order_acquire));
        if (subdivisions <= 1)
            endValue = startValue;

        state.steps[static_cast<size_t>(i)] = startValue;
        state.stepSubdivisions[static_cast<size_t>(i)] = subdivisions;
        state.stepEndValues[static_cast<size_t>(i)] = endValue;
        state.stepCurveShapes[static_cast<size_t>(i)] = curveShape;
    }
    return state;
}

void ModernAudioEngine::setModTarget(int stripIndex, ModTarget target)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;
    setModTargetOnSequencer(getActiveModSequencer(stripIndex), target);
}

ModernAudioEngine::ModTarget ModernAudioEngine::getModTarget(int stripIndex) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return ModTarget::None;
    return static_cast<ModTarget>(getActiveModSequencer(stripIndex).target.load(std::memory_order_acquire));
}

void ModernAudioEngine::setModTargetForSlot(int stripIndex, int slot, ModTarget target)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;
    setModTargetOnSequencer(getModSequencer(stripIndex, slot), target);
}

ModernAudioEngine::ModTarget ModernAudioEngine::getModTargetForSlot(int stripIndex, int slot) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return ModTarget::None;
    return static_cast<ModTarget>(getModSequencer(stripIndex, slot).target.load(std::memory_order_acquire));
}

void ModernAudioEngine::setModBipolar(int stripIndex, bool bipolar, ModBipolarToggleMode mode)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;
    setModBipolarOnSequencer(getActiveModSequencer(stripIndex), bipolar, mode);
}

bool ModernAudioEngine::isModBipolar(int stripIndex) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return false;

    const auto& seq = getActiveModSequencer(stripIndex);
    const auto target = static_cast<ModTarget>(seq.target.load(std::memory_order_acquire));
    return modTargetSupportsBipolar(target)
        && (modTargetUsesForcedBipolar(target) || (seq.bipolar.load(std::memory_order_acquire) != 0));
}

void ModernAudioEngine::setModBipolarForSlot(int stripIndex, int slot, bool bipolar, ModBipolarToggleMode mode)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;
    setModBipolarOnSequencer(getModSequencer(stripIndex, slot), bipolar, mode);
}

bool ModernAudioEngine::isModBipolarForSlot(int stripIndex, int slot) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return false;

    const auto& seq = getModSequencer(stripIndex, slot);
    const auto target = static_cast<ModTarget>(seq.target.load(std::memory_order_acquire));
    return modTargetSupportsBipolar(target)
        && (modTargetUsesForcedBipolar(target) || (seq.bipolar.load(std::memory_order_acquire) != 0));
}

void ModernAudioEngine::setModDepth(int stripIndex, float depth)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;
    getActiveModSequencer(stripIndex).depth.store(juce::jlimit(0.0f, 1.0f, depth), std::memory_order_release);
}

float ModernAudioEngine::getModDepth(int stripIndex) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return 1.0f;
    return getActiveModSequencer(stripIndex).depth.load(std::memory_order_acquire);
}

void ModernAudioEngine::setModDepthForSlot(int stripIndex, int slot, float depth)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;
    getModSequencer(stripIndex, slot).depth.store(juce::jlimit(0.0f, 1.0f, depth), std::memory_order_release);
}

float ModernAudioEngine::getModDepthForSlot(int stripIndex, int slot) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return 1.0f;
    return getModSequencer(stripIndex, slot).depth.load(std::memory_order_acquire);
}

void ModernAudioEngine::setModRate(int stripIndex, float rate)
{
    setModRateForSlot(stripIndex, getActiveModSequencerSlot(stripIndex), rate);
}

float ModernAudioEngine::getModRate(int stripIndex) const
{
    return getModRateForSlot(stripIndex, getActiveModSequencerSlot(stripIndex));
}

void ModernAudioEngine::setModRateForSlot(int stripIndex, int slot, float rate)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;

    getModSequencer(stripIndex, slot).rate.store(quantizeModLaneRate(rate), std::memory_order_release);
}

float ModernAudioEngine::getModRateForSlot(int stripIndex, int slot) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return 1.0f;

    return quantizeModLaneRate(getModSequencer(stripIndex, slot).rate.load(std::memory_order_acquire));
}

void ModernAudioEngine::setModTransportMode(int stripIndex, ModTransportMode mode)
{
    setModTransportModeForSlot(stripIndex, getActiveModSequencerSlot(stripIndex), mode);
}

ModernAudioEngine::ModTransportMode ModernAudioEngine::getModTransportMode(int stripIndex) const
{
    return getModTransportModeForSlot(stripIndex, getActiveModSequencerSlot(stripIndex));
}

void ModernAudioEngine::setModTransportModeForSlot(int stripIndex, int slot, ModTransportMode mode)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;

    auto& seq = getModSequencer(stripIndex, slot);
    seq.transportMode.store(juce::jlimit(0,
                                         static_cast<int>(ModTransportMode::Scene),
                                         static_cast<int>(mode)),
                            std::memory_order_release);
    queueModSequencerRuntimeResetFromSnapshots(seq, false);
}

ModernAudioEngine::ModTransportMode ModernAudioEngine::getModTransportModeForSlot(int stripIndex, int slot) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return ModTransportMode::Free;

    return static_cast<ModTransportMode>(juce::jlimit(
        0,
        static_cast<int>(ModTransportMode::Scene),
        getModSequencer(stripIndex, slot).transportMode.load(std::memory_order_acquire)));
}

void ModernAudioEngine::setSceneModulationContext(bool enabled,
                                                  double sceneStartPpq,
                                                  double sceneLengthBeats)
{
    sceneModulationContextEnabled.store(enabled ? 1 : 0, std::memory_order_release);
    sceneModulationStartPpq.store(std::isfinite(sceneStartPpq) ? sceneStartPpq : 0.0,
                                  std::memory_order_release);
    sceneModulationLengthBeats.store(std::isfinite(sceneLengthBeats)
                                         ? juce::jmax(0.001, sceneLengthBeats)
                                         : 4.0,
                                     std::memory_order_release);
}

void ModernAudioEngine::setModCurveMode(int stripIndex, bool curveMode)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;
    getActiveModSequencer(stripIndex).curveMode.store(curveMode ? 1 : 0, std::memory_order_release);
}

bool ModernAudioEngine::isModCurveMode(int stripIndex) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return true;
    return getActiveModSequencer(stripIndex).curveMode.load(std::memory_order_acquire) != 0;
}

void ModernAudioEngine::setModCurveModeForSlot(int stripIndex, int slot, bool curveMode)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;
    getModSequencer(stripIndex, slot).curveMode.store(curveMode ? 1 : 0, std::memory_order_release);
}

bool ModernAudioEngine::isModCurveModeForSlot(int stripIndex, int slot) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return true;
    return getModSequencer(stripIndex, slot).curveMode.load(std::memory_order_acquire) != 0;
}

void ModernAudioEngine::setModOffset(int stripIndex, int offset)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;
    setModOffsetForSlot(stripIndex, getActiveModSequencerSlot(stripIndex), offset);
}

void ModernAudioEngine::setModOffsetForSlot(int stripIndex, int slot, int offset)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;
    getModSequencer(stripIndex, slot).offset.store(
        juce::jlimit(-(ModTotalSteps - 1), ModTotalSteps - 1, offset),
        std::memory_order_release);
}

int ModernAudioEngine::getModOffset(int stripIndex) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return 0;
    return getActiveModSequencer(stripIndex).offset.load(std::memory_order_acquire);
}

int ModernAudioEngine::getModOffsetForSlot(int stripIndex, int slot) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return 0;
    return getModSequencer(stripIndex, slot).offset.load(std::memory_order_acquire);
}

void ModernAudioEngine::setModStepValue(int stripIndex, int step, float value01)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips || step < 0 || step >= ModSteps)
        return;
    auto& seq = getActiveModSequencer(stripIndex);
    const int page = juce::jlimit(0, MaxModBars - 1, seq.editPage.load(std::memory_order_acquire));
    const int idx = (page * ModSteps) + step;
    const float clampedValue = juce::jlimit(0.0f, 1.0f, value01);
    const int activeCurveShape = juce::jlimit(
        0,
        static_cast<int>(ModCurveShape::Square),
        seq.curveShape.load(std::memory_order_acquire));
    seq.steps[static_cast<size_t>(idx)].store(clampedValue, std::memory_order_release);
    seq.stepSubdivisions[static_cast<size_t>(idx)].store(1, std::memory_order_release);
    seq.stepEndValues[static_cast<size_t>(idx)].store(clampedValue, std::memory_order_release);
    seq.stepCurveShapes[static_cast<size_t>(idx)].store(activeCurveShape, std::memory_order_release);
}

void ModernAudioEngine::setModStepValueAbsolute(int stripIndex, int absoluteStep, float value01)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips || absoluteStep < 0 || absoluteStep >= ModTotalSteps)
        return;
    auto& seq = getActiveModSequencer(stripIndex);
    const float clampedValue = juce::jlimit(0.0f, 1.0f, value01);
    const int activeCurveShape = juce::jlimit(
        0,
        static_cast<int>(ModCurveShape::Square),
        seq.curveShape.load(std::memory_order_acquire));
    seq.steps[static_cast<size_t>(absoluteStep)].store(clampedValue, std::memory_order_release);
    seq.stepSubdivisions[static_cast<size_t>(absoluteStep)].store(1, std::memory_order_release);
    seq.stepEndValues[static_cast<size_t>(absoluteStep)].store(clampedValue, std::memory_order_release);
    seq.stepCurveShapes[static_cast<size_t>(absoluteStep)].store(activeCurveShape, std::memory_order_release);
}

void ModernAudioEngine::setModStepValueAbsoluteForSlot(int stripIndex, int slot, int absoluteStep, float value01)
{
    writeModStepNormalized(stripIndex, slot, absoluteStep, value01);
}

void ModernAudioEngine::writeModStepNormalized(int stripIndex, int slot, int absoluteStep, float value01)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips || absoluteStep < 0 || absoluteStep >= ModTotalSteps)
        return;

    auto& seq = getModSequencer(stripIndex, slot);
    const float clampedValue = juce::jlimit(0.0f, 1.0f, value01);
    const int activeCurveShape = juce::jlimit(
        0,
        static_cast<int>(ModCurveShape::Square),
        seq.curveShape.load(std::memory_order_acquire));
    seq.steps[static_cast<size_t>(absoluteStep)].store(clampedValue, std::memory_order_release);
    seq.stepSubdivisions[static_cast<size_t>(absoluteStep)].store(1, std::memory_order_release);
    seq.stepEndValues[static_cast<size_t>(absoluteStep)].store(clampedValue, std::memory_order_release);
    seq.stepCurveShapes[static_cast<size_t>(absoluteStep)].store(activeCurveShape, std::memory_order_release);
}

void ModernAudioEngine::clearModStepsForSlot(int stripIndex, int slot, float value01)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;

    auto& seq = getModSequencer(stripIndex, slot);
    const float clampedValue = juce::jlimit(0.0f, 1.0f, value01);
    const int activeCurveShape = juce::jlimit(
        0,
        static_cast<int>(ModCurveShape::Square),
        seq.curveShape.load(std::memory_order_acquire));
    for (int step = 0; step < ModTotalSteps; ++step)
    {
        const auto idx = static_cast<size_t>(step);
        seq.steps[idx].store(clampedValue, std::memory_order_release);
        seq.stepSubdivisions[idx].store(1, std::memory_order_release);
        seq.stepEndValues[idx].store(clampedValue, std::memory_order_release);
        seq.stepCurveShapes[idx].store(activeCurveShape, std::memory_order_release);
    }
}

float ModernAudioEngine::getModStepValue(int stripIndex, int step) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips || step < 0 || step >= ModSteps)
        return 0.0f;
    const auto& seq = getActiveModSequencer(stripIndex);
    const int page = juce::jlimit(0, MaxModBars - 1, seq.editPage.load(std::memory_order_acquire));
    const int idx = (page * ModSteps) + step;
    return seq.steps[static_cast<size_t>(idx)].load(std::memory_order_acquire);
}

float ModernAudioEngine::getModStepValueAbsolute(int stripIndex, int absoluteStep) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips || absoluteStep < 0 || absoluteStep >= ModTotalSteps)
        return 0.0f;
    return getActiveModSequencer(stripIndex).steps[static_cast<size_t>(absoluteStep)].load(std::memory_order_acquire);
}

float ModernAudioEngine::getModStepValueAbsoluteForSlot(int stripIndex, int slot, int absoluteStep) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips || absoluteStep < 0 || absoluteStep >= ModTotalSteps)
        return 0.0f;

    return getModSequencer(stripIndex, slot).steps[static_cast<size_t>(absoluteStep)].load(std::memory_order_acquire);
}

void ModernAudioEngine::setModStepShape(int stripIndex, int step, int subdivisions, float endValue01)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips || step < 0 || step >= ModSteps)
        return;

    auto& seq = getActiveModSequencer(stripIndex);
    const int page = juce::jlimit(0, MaxModBars - 1, seq.editPage.load(std::memory_order_acquire));
    const int absoluteStep = (page * ModSteps) + step;
    setModStepShapeAbsolute(stripIndex, absoluteStep, subdivisions, endValue01);
}

void ModernAudioEngine::setModStepShapeAbsolute(int stripIndex, int absoluteStep, int subdivisions, float endValue01)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips || absoluteStep < 0 || absoluteStep >= ModTotalSteps)
        return;

    auto& seq = getActiveModSequencer(stripIndex);
    const int clampedSubdivisions = juce::jlimit(1, ModMaxStepSubdivisions, subdivisions);
    const float startValue = juce::jlimit(
        0.0f, 1.0f, seq.steps[static_cast<size_t>(absoluteStep)].load(std::memory_order_acquire));
    float clampedEndValue = juce::jlimit(0.0f, 1.0f, endValue01);
    if (clampedSubdivisions <= 1)
        clampedEndValue = startValue;

    seq.stepSubdivisions[static_cast<size_t>(absoluteStep)].store(clampedSubdivisions, std::memory_order_release);
    seq.stepEndValues[static_cast<size_t>(absoluteStep)].store(clampedEndValue, std::memory_order_release);
}

void ModernAudioEngine::setModStepShapeAbsoluteForSlot(int stripIndex,
                                                       int slot,
                                                       int absoluteStep,
                                                       int subdivisions,
                                                       float endValue01)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips || absoluteStep < 0 || absoluteStep >= ModTotalSteps)
        return;

    auto& seq = getModSequencer(stripIndex, slot);
    const int clampedSubdivisions = juce::jlimit(1, ModMaxStepSubdivisions, subdivisions);
    const float startValue = juce::jlimit(
        0.0f, 1.0f, seq.steps[static_cast<size_t>(absoluteStep)].load(std::memory_order_acquire));
    float clampedEndValue = juce::jlimit(0.0f, 1.0f, endValue01);
    if (clampedSubdivisions <= 1)
        clampedEndValue = startValue;

    seq.stepSubdivisions[static_cast<size_t>(absoluteStep)].store(clampedSubdivisions, std::memory_order_release);
    seq.stepEndValues[static_cast<size_t>(absoluteStep)].store(clampedEndValue, std::memory_order_release);
}

int ModernAudioEngine::getModStepSubdivisionAbsolute(int stripIndex, int absoluteStep) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips || absoluteStep < 0 || absoluteStep >= ModTotalSteps)
        return 1;

    return juce::jlimit(
        1,
        ModMaxStepSubdivisions,
        getActiveModSequencer(stripIndex).stepSubdivisions[static_cast<size_t>(absoluteStep)].load(std::memory_order_acquire));
}

int ModernAudioEngine::getModStepSubdivisionAbsoluteForSlot(int stripIndex, int slot, int absoluteStep) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips || absoluteStep < 0 || absoluteStep >= ModTotalSteps)
        return 1;

    return juce::jlimit(
        1,
        ModMaxStepSubdivisions,
        getModSequencer(stripIndex, slot).stepSubdivisions[static_cast<size_t>(absoluteStep)].load(std::memory_order_acquire));
}

float ModernAudioEngine::getModStepEndValueAbsolute(int stripIndex, int absoluteStep) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips || absoluteStep < 0 || absoluteStep >= ModTotalSteps)
        return 0.0f;

    const auto& seq = getActiveModSequencer(stripIndex);
    const int subdivisions = juce::jlimit(
        1,
        ModMaxStepSubdivisions,
        seq.stepSubdivisions[static_cast<size_t>(absoluteStep)].load(std::memory_order_acquire));
    const float startValue = juce::jlimit(
        0.0f, 1.0f, seq.steps[static_cast<size_t>(absoluteStep)].load(std::memory_order_acquire));
    if (subdivisions <= 1)
        return startValue;

    return juce::jlimit(
        0.0f, 1.0f, seq.stepEndValues[static_cast<size_t>(absoluteStep)].load(std::memory_order_acquire));
}

float ModernAudioEngine::getModStepEndValueAbsoluteForSlot(int stripIndex, int slot, int absoluteStep) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips || absoluteStep < 0 || absoluteStep >= ModTotalSteps)
        return 0.0f;

    const auto& seq = getModSequencer(stripIndex, slot);
    const int subdivisions = juce::jlimit(
        1,
        ModMaxStepSubdivisions,
        seq.stepSubdivisions[static_cast<size_t>(absoluteStep)].load(std::memory_order_acquire));
    const float startValue = juce::jlimit(
        0.0f, 1.0f, seq.steps[static_cast<size_t>(absoluteStep)].load(std::memory_order_acquire));
    if (subdivisions <= 1)
        return startValue;

    return juce::jlimit(
        0.0f, 1.0f, seq.stepEndValues[static_cast<size_t>(absoluteStep)].load(std::memory_order_acquire));
}

void ModernAudioEngine::setModStepCurveShape(int stripIndex, int step, ModCurveShape shape)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips || step < 0 || step >= ModSteps)
        return;

    const auto& seq = getActiveModSequencer(stripIndex);
    const int page = juce::jlimit(0, MaxModBars - 1, seq.editPage.load(std::memory_order_acquire));
    const int absoluteStep = (page * ModSteps) + step;
    setModStepCurveShapeAbsolute(stripIndex, absoluteStep, shape);
}

void ModernAudioEngine::setModStepCurveShapeAbsolute(int stripIndex, int absoluteStep, ModCurveShape shape)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips || absoluteStep < 0 || absoluteStep >= ModTotalSteps)
        return;

    auto& seq = getActiveModSequencer(stripIndex);
    const int clampedShape = juce::jlimit(
        0,
        static_cast<int>(ModCurveShape::Square),
        static_cast<int>(shape));
    seq.stepCurveShapes[static_cast<size_t>(absoluteStep)].store(clampedShape, std::memory_order_release);
}

void ModernAudioEngine::setModStepCurveShapeAbsoluteForSlot(int stripIndex,
                                                            int slot,
                                                            int absoluteStep,
                                                            ModCurveShape shape)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips || absoluteStep < 0 || absoluteStep >= ModTotalSteps)
        return;

    auto& seq = getModSequencer(stripIndex, slot);
    const int clampedShape = juce::jlimit(
        0,
        static_cast<int>(ModCurveShape::Square),
        static_cast<int>(shape));
    seq.stepCurveShapes[static_cast<size_t>(absoluteStep)].store(clampedShape, std::memory_order_release);
}

ModernAudioEngine::ModCurveShape ModernAudioEngine::getModStepCurveShapeAbsolute(int stripIndex, int absoluteStep) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips || absoluteStep < 0 || absoluteStep >= ModTotalSteps)
        return ModCurveShape::Linear;

    const auto& seq = getActiveModSequencer(stripIndex);
    return static_cast<ModCurveShape>(juce::jlimit(
        0,
        static_cast<int>(ModCurveShape::Square),
        seq.stepCurveShapes[static_cast<size_t>(absoluteStep)].load(std::memory_order_acquire)));
}

ModernAudioEngine::ModCurveShape ModernAudioEngine::getModStepCurveShapeAbsoluteForSlot(int stripIndex, int slot, int absoluteStep) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips || absoluteStep < 0 || absoluteStep >= ModTotalSteps)
        return ModCurveShape::Linear;

    const auto& seq = getModSequencer(stripIndex, slot);
    return static_cast<ModCurveShape>(juce::jlimit(
        0,
        static_cast<int>(ModCurveShape::Square),
        seq.stepCurveShapes[static_cast<size_t>(absoluteStep)].load(std::memory_order_acquire)));
}

void ModernAudioEngine::toggleModStep(int stripIndex, int step)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips || step < 0 || step >= ModSteps)
        return;
    auto& seq = getActiveModSequencer(stripIndex);
    const int page = juce::jlimit(0, MaxModBars - 1, seq.editPage.load(std::memory_order_acquire));
    const int idx = (page * ModSteps) + step;
    auto& cell = seq.steps[static_cast<size_t>(idx)];
    const float prev = cell.load(std::memory_order_acquire);
    const float nextValue = (prev >= 0.5f) ? 0.0f : 1.0f;
    const int activeCurveShape = juce::jlimit(
        0,
        static_cast<int>(ModCurveShape::Square),
        seq.curveShape.load(std::memory_order_acquire));
    cell.store(nextValue, std::memory_order_release);
    seq.stepSubdivisions[static_cast<size_t>(idx)].store(1, std::memory_order_release);
    seq.stepEndValues[static_cast<size_t>(idx)].store(nextValue, std::memory_order_release);
    seq.stepCurveShapes[static_cast<size_t>(idx)].store(activeCurveShape, std::memory_order_release);
}

void ModernAudioEngine::clearModSteps(int stripIndex)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;
    auto& seq = getActiveModSequencer(stripIndex);
    const int activeCurveShape = juce::jlimit(
        0,
        static_cast<int>(ModCurveShape::Square),
        seq.curveShape.load(std::memory_order_acquire));
    for (size_t i = 0; i < seq.steps.size(); ++i)
    {
        seq.steps[i].store(0.0f, std::memory_order_release);
        seq.stepSubdivisions[i].store(1, std::memory_order_release);
        seq.stepEndValues[i].store(0.0f, std::memory_order_release);
        seq.stepCurveShapes[i].store(activeCurveShape, std::memory_order_release);
    }
}

int ModernAudioEngine::getModCurrentStep(int stripIndex) const
{
    const int globalStep = getModCurrentGlobalStep(stripIndex);
    return globalStep % ModSteps;
}

int ModernAudioEngine::getModCurrentPage(int stripIndex) const
{
    const int globalStep = getModCurrentGlobalStep(stripIndex);
    return juce::jlimit(0, MaxModBars - 1, globalStep / ModSteps);
}

int ModernAudioEngine::getModCurrentGlobalStep(int stripIndex) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return 0;
    const auto& seq = getActiveModSequencer(stripIndex);
    const int lengthBars = juce::jlimit(1, MaxModBars, seq.lengthBars.load(std::memory_order_acquire));
    const int totalSteps = juce::jmax(ModSteps, ModSteps * lengthBars);
    return juce::jlimit(0, totalSteps - 1, seq.lastGlobalStep.load(std::memory_order_acquire));
}

int ModernAudioEngine::getModTotalActiveSteps(int stripIndex, int slot) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return ModSteps;

    const auto& seq = getModSequencer(stripIndex, slot);
    const int lengthBars = juce::jlimit(1, MaxModBars, seq.lengthBars.load(std::memory_order_acquire));
    return juce::jmax(ModSteps, lengthBars * ModSteps);
}

double ModernAudioEngine::getModStepLengthBeats(int stripIndex, int slot) const
{
    const double baseStepLengthBeats = 4.0 / static_cast<double>(ModSteps);
    const double rate = static_cast<double>(juce::jmax(0.125f, getModRateForSlot(stripIndex, slot)));
    return baseStepLengthBeats / rate;
}

void ModernAudioEngine::setModLengthBars(int stripIndex, int bars)
{
    setModLengthBarsForSlot(stripIndex, getActiveModSequencerSlot(stripIndex), bars);
}

int ModernAudioEngine::getModLengthBars(int stripIndex) const
{
    return getModLengthBarsForSlot(stripIndex, getActiveModSequencerSlot(stripIndex));
}

void ModernAudioEngine::setModLengthBarsForSlot(int stripIndex, int slot, int bars)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;

    auto& seq = getModSequencer(stripIndex, slot);
    const int clampedBars = juce::jlimit(1, MaxModBars, bars);

    seq.lengthBars.store(clampedBars, std::memory_order_release);
    const int maxPage = clampedBars - 1;
    const int page = juce::jlimit(0, maxPage, seq.editPage.load(std::memory_order_acquire));
    seq.editPage.store(page, std::memory_order_release);
}

int ModernAudioEngine::getModLengthBarsForSlot(int stripIndex, int slot) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return 1;

    return juce::jlimit(1, MaxModBars, getModSequencer(stripIndex, slot).lengthBars.load(std::memory_order_acquire));
}

void ModernAudioEngine::setModEditPage(int stripIndex, int page)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;
    auto& seq = getActiveModSequencer(stripIndex);
    const int maxPage = juce::jmax(0, getModLengthBars(stripIndex) - 1);
    seq.editPage.store(juce::jlimit(0, maxPage, page), std::memory_order_release);
}

void ModernAudioEngine::setModEditPageForSlot(int stripIndex, int slot, int page)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;

    auto& seq = getModSequencer(stripIndex, slot);
    const int maxPage = juce::jmax(0, getModLengthBarsForSlot(stripIndex, slot) - 1);
    seq.editPage.store(juce::jlimit(0, maxPage, page), std::memory_order_release);
}

int ModernAudioEngine::getModEditPage(int stripIndex) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return 0;
    const auto& seq = getActiveModSequencer(stripIndex);
    const int maxPage = juce::jmax(0, getModLengthBars(stripIndex) - 1);
    return juce::jlimit(0, maxPage, seq.editPage.load(std::memory_order_acquire));
}

int ModernAudioEngine::getModEditPageForSlot(int stripIndex, int slot) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return 0;

    const auto& seq = getModSequencer(stripIndex, slot);
    const int maxPage = juce::jmax(0, getModLengthBarsForSlot(stripIndex, slot) - 1);
    return juce::jlimit(0, maxPage, seq.editPage.load(std::memory_order_acquire));
}

void ModernAudioEngine::setModSmoothingMs(int stripIndex, float ms)
{
    setModSmoothingMsForSlot(stripIndex, getActiveModSequencerSlot(stripIndex), ms);
}

float ModernAudioEngine::getModSmoothingMs(int stripIndex) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return 0.0f;
    return juce::jlimit(0.0f, 250.0f, getActiveModSequencer(stripIndex).smoothingMs.load(std::memory_order_acquire));
}

void ModernAudioEngine::setModSmoothingMsForSlot(int stripIndex, int slot, float ms)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;

    getModSequencer(stripIndex, slot).smoothingMs.store(juce::jlimit(0.0f, 250.0f, ms), std::memory_order_release);
}

float ModernAudioEngine::getModSmoothingMsForSlot(int stripIndex, int slot) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return 0.0f;

    return juce::jlimit(0.0f,
                        250.0f,
                        getModSequencer(stripIndex, slot).smoothingMs.load(std::memory_order_acquire));
}

void ModernAudioEngine::setModCurveBend(int stripIndex, float bend)
{
    setModCurveBendForSlot(stripIndex, getActiveModSequencerSlot(stripIndex), bend);
}

float ModernAudioEngine::getModCurveBend(int stripIndex) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return 0.0f;
    return juce::jlimit(-1.0f, 1.0f, getActiveModSequencer(stripIndex).curveBend.load(std::memory_order_acquire));
}

void ModernAudioEngine::setModCurveBendForSlot(int stripIndex, int slot, float bend)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;

    getModSequencer(stripIndex, slot).curveBend.store(juce::jlimit(-1.0f, 1.0f, bend), std::memory_order_release);
}

float ModernAudioEngine::getModCurveBendForSlot(int stripIndex, int slot) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return 0.0f;

    return juce::jlimit(-1.0f,
                        1.0f,
                        getModSequencer(stripIndex, slot).curveBend.load(std::memory_order_acquire));
}

void ModernAudioEngine::setModCurveShape(int stripIndex, ModCurveShape shape)
{
    setModCurveShapeForSlot(stripIndex, getActiveModSequencerSlot(stripIndex), shape);
}

ModernAudioEngine::ModCurveShape ModernAudioEngine::getModCurveShape(int stripIndex) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return ModCurveShape::Linear;
    return static_cast<ModCurveShape>(juce::jlimit(
        0, static_cast<int>(ModCurveShape::Square),
        getActiveModSequencer(stripIndex).curveShape.load(std::memory_order_acquire)));
}

void ModernAudioEngine::setModCurveShapeForSlot(int stripIndex, int slot, ModCurveShape shape)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;

    getModSequencer(stripIndex, slot).curveShape.store(
        juce::jlimit(0, static_cast<int>(ModCurveShape::Square), static_cast<int>(shape)),
        std::memory_order_release);
}

ModernAudioEngine::ModCurveShape ModernAudioEngine::getModCurveShapeForSlot(int stripIndex, int slot) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return ModCurveShape::Linear;

    return static_cast<ModCurveShape>(juce::jlimit(
        0,
        static_cast<int>(ModCurveShape::Square),
        getModSequencer(stripIndex, slot).curveShape.load(std::memory_order_acquire)));
}

void ModernAudioEngine::setModPitchScaleQuantize(int stripIndex, bool enabled)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;
    getActiveModSequencer(stripIndex).pitchScaleQuantize.store(enabled ? 1 : 0, std::memory_order_release);
}

bool ModernAudioEngine::isModPitchScaleQuantize(int stripIndex) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return false;
    return getActiveModSequencer(stripIndex).pitchScaleQuantize.load(std::memory_order_acquire) != 0;
}

void ModernAudioEngine::setModPitchScaleQuantizeForSlot(int stripIndex, int slot, bool enabled)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;
    getModSequencer(stripIndex, slot).pitchScaleQuantize.store(enabled ? 1 : 0, std::memory_order_release);
}

bool ModernAudioEngine::isModPitchScaleQuantizeForSlot(int stripIndex, int slot) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return false;
    return getModSequencer(stripIndex, slot).pitchScaleQuantize.load(std::memory_order_acquire) != 0;
}

void ModernAudioEngine::setModPitchScale(int stripIndex, PitchScale scale)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;
    getActiveModSequencer(stripIndex).pitchScale.store(
        juce::jlimit(0, static_cast<int>(PitchScale::PentatonicMinor), static_cast<int>(scale)),
        std::memory_order_release);
}

ModernAudioEngine::PitchScale ModernAudioEngine::getModPitchScale(int stripIndex) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return PitchScale::Chromatic;
    const int idx = juce::jlimit(0, static_cast<int>(PitchScale::PentatonicMinor),
                                 getActiveModSequencer(stripIndex).pitchScale.load(std::memory_order_acquire));
    return static_cast<PitchScale>(idx);
}

void ModernAudioEngine::setModPitchScaleForSlot(int stripIndex, int slot, PitchScale scale)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;
    getModSequencer(stripIndex, slot).pitchScale.store(
        juce::jlimit(0, static_cast<int>(PitchScale::PentatonicMinor), static_cast<int>(scale)),
        std::memory_order_release);
}

ModernAudioEngine::PitchScale ModernAudioEngine::getModPitchScaleForSlot(int stripIndex, int slot) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return PitchScale::Chromatic;
    const int idx = juce::jlimit(0,
                                 static_cast<int>(PitchScale::PentatonicMinor),
                                 getModSequencer(stripIndex, slot).pitchScale.load(std::memory_order_acquire));
    return static_cast<PitchScale>(idx);
}
