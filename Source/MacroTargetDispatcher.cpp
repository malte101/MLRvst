/*
  ==============================================================================

    MacroTargetDispatcher.cpp
    Macro target metadata/read/apply helpers

  ==============================================================================
*/

#include "MacroTargetDispatcher.h"
#include "PluginProcessor.h"

namespace
{
const juce::NormalisableRange<float>& macroCutoffRange()
{
    static const juce::NormalisableRange<float> range = []
    {
        juce::NormalisableRange<float> r(20.0f, 20000.0f, 1.0f);
        r.setSkewForCentre(1000.0f);
        return r;
    }();
    return range;
}

float normalizeMacroCutoff(float hz)
{
    return juce::jlimit(0.0f, 1.0f, macroCutoffRange().convertTo0to1(juce::jlimit(20.0f, 20000.0f, hz)));
}

float denormalizeMacroCutoff(float normalized)
{
    return juce::jlimit(20.0f, 20000.0f, macroCutoffRange().convertFrom0to1(juce::jlimit(0.0f, 1.0f, normalized)));
}

float normalizeMacroResonance(float q)
{
    return juce::jmap(juce::jlimit(0.1f, 10.0f, q), 0.1f, 10.0f, 0.0f, 1.0f);
}

float denormalizeMacroResonance(float normalized)
{
    return juce::jmap(juce::jlimit(0.0f, 1.0f, normalized), 0.0f, 1.0f, 0.1f, 10.0f);
}

float normalizeMacroPitch(float semitones)
{
    return juce::jlimit(0.0f, 1.0f, juce::jmap(juce::jlimit(-24.0f, 24.0f, semitones), -24.0f, 24.0f, 0.0f, 1.0f));
}

float denormalizeMacroPitch(float normalized)
{
    return juce::jmap(juce::jlimit(0.0f, 1.0f, normalized), 0.0f, 1.0f, -24.0f, 24.0f);
}

float normalizeMacroLinear(float value, float minValue, float maxValue)
{
    return juce::jlimit(0.0f, 1.0f, juce::jmap(juce::jlimit(minValue, maxValue, value),
                                               minValue, maxValue, 0.0f, 1.0f));
}

float denormalizeMacroLinear(float normalized, float minValue, float maxValue)
{
    return juce::jmap(juce::jlimit(0.0f, 1.0f, normalized), 0.0f, 1.0f, minValue, maxValue);
}
} // namespace

PerformanceTarget MacroTargetDispatcher::getDefaultMacroTarget(int macroIndex)
{
    switch (macroIndex)
    {
        case 0: return PerformanceTarget::Cutoff;
        case 1: return PerformanceTarget::Resonance;
        case 2: return PerformanceTarget::FilterMorph;
        case 3: return PerformanceTarget::Pitch;
        case 4: return PerformanceTarget::SliceLength;
        case 5: return PerformanceTarget::DelayMix;
        case 6: return PerformanceTarget::DelayTime;
        case 7: return PerformanceTarget::DelayFeedback;
        default: return PerformanceTarget::None;
    }
}

float MacroTargetDispatcher::getDefaultMacroNormalizedValue(PerformanceTarget target)
{
    switch (target)
    {
        case PerformanceTarget::Cutoff: return normalizeMacroCutoff(20000.0f);
        case PerformanceTarget::Resonance: return normalizeMacroResonance(0.707f);
        case PerformanceTarget::FilterMorph: return 0.0f;
        case PerformanceTarget::Pitch: return 0.5f;
        case PerformanceTarget::Volume: return 1.0f;
        case PerformanceTarget::Pan: return 0.5f;
        case PerformanceTarget::FilterEnable: return 0.0f;
        case PerformanceTarget::Speed: return normalizeMacroLinear(1.0f, 0.125f, 8.0f);
        case PerformanceTarget::SliceLength: return 1.0f;
        case PerformanceTarget::Scratch: return 0.0f;
        case PerformanceTarget::GrainSize: return normalizeMacroLinear(1240.0f, 5.0f, 2400.0f);
        case PerformanceTarget::GrainDensity: return normalizeMacroLinear(0.05f, 0.05f, 0.9f);
        case PerformanceTarget::GrainPitch: return 0.5f;
        case PerformanceTarget::GrainPitchJitter: return 0.0f;
        case PerformanceTarget::GrainSpread: return 0.0f;
        case PerformanceTarget::GrainJitter: return 0.0f;
        case PerformanceTarget::GrainPositionJitter: return 0.0f;
        case PerformanceTarget::GrainRandom: return 0.0f;
        case PerformanceTarget::GrainArp: return 0.0f;
        case PerformanceTarget::GrainCloud: return 0.0f;
        case PerformanceTarget::GrainEmitter: return 0.0f;
        case PerformanceTarget::GrainEnvelope: return 0.0f;
        case PerformanceTarget::GrainShape: return 0.5f;
        case PerformanceTarget::Retrigger: return 0.0f;
        case PerformanceTarget::Rearrange: return 0.0f;
        case PerformanceTarget::DelayMix: return 0.0f;
        case PerformanceTarget::DelayTime: return normalizeMacroLinear(1.0f, 0.25f, 4.0f);
        case PerformanceTarget::DelayFeedback: return normalizeMacroLinear(0.35f, 0.0f, 0.97f);
        case PerformanceTarget::DelayLowCut: return normalizeMacroLinear(20.0f, 20.0f, 12000.0f);
        case PerformanceTarget::DelayHighCut: return normalizeMacroLinear(12000.0f, 200.0f, 20000.0f);
        case PerformanceTarget::None:
        default: return 0.0f;
    }
}

float MacroTargetDispatcher::getNormalizedValueForTarget(const MlrVSTAudioProcessor& processor,
                                                         int stripIndex,
                                                         const EnhancedAudioStrip& strip,
                                                         PerformanceTarget target)
{
    const bool isStepMode = (strip.getPlayMode() == EnhancedAudioStrip::PlayMode::Step);
    const auto* stepSampler = isStepMode ? strip.getStepSampler() : nullptr;
    const bool validStripIndex = stripIndex >= 0 && stripIndex < MlrVSTAudioProcessor::MaxStrips;
    const auto arrayIndex = static_cast<size_t>(juce::jlimit(0, MlrVSTAudioProcessor::MaxStrips - 1, stripIndex));
    const auto readParamOr = [&](const std::array<std::atomic<float>*, MlrVSTAudioProcessor::MaxStrips>& params,
                                 float fallback)
    {
        if (!validStripIndex)
            return fallback;
        if (const auto* rawParam = params[arrayIndex])
            return rawParam->load(std::memory_order_acquire);
        return fallback;
    };

    switch (target)
    {
        case PerformanceTarget::Cutoff:
            return normalizeMacroCutoff((isStepMode && stepSampler) ? stepSampler->getFilterFrequency()
                                                                    : strip.getFilterFrequency());
        case PerformanceTarget::Resonance:
            return normalizeMacroResonance((isStepMode && stepSampler) ? stepSampler->getFilterResonance()
                                                                       : strip.getFilterResonance());
        case PerformanceTarget::FilterMorph:
            if (isStepMode && stepSampler != nullptr)
            {
                switch (stepSampler->getFilterType())
                {
                    case FilterType::LowPass: return 0.0f;
                    case FilterType::BandPass: return 0.5f;
                    case FilterType::HighPass: return 1.0f;
                    default: break;
                }
            }
            return juce::jlimit(0.0f, 1.0f, strip.getFilterMorph());
        case PerformanceTarget::Pitch:
            return normalizeMacroPitch(validStripIndex
                                           ? processor.getStoredStripPitchSemitones(stripIndex)
                                           : processor.getPitchSemitonesForDisplay(strip));
        case PerformanceTarget::Volume:
            return juce::jlimit(0.0f, 1.0f, readParamOr(processor.stripVolumeParams,
                                                        (isStepMode && stepSampler) ? stepSampler->getVolume()
                                                                                    : strip.getVolume()));
        case PerformanceTarget::Pan:
            return normalizeMacroLinear(readParamOr(processor.stripPanParams,
                                                    (isStepMode && stepSampler) ? stepSampler->getPan()
                                                                                : strip.getPan()),
                                        -1.0f, 1.0f);
        case PerformanceTarget::FilterEnable:
            return ((isStepMode && stepSampler) ? stepSampler->isFilterEnabled()
                                                : strip.isFilterEnabled())
                ? 1.0f
                : 0.0f;
        case PerformanceTarget::Speed:
            return normalizeMacroLinear(readParamOr(processor.stripSpeedParams,
                                                    (isStepMode && stepSampler) ? stepSampler->getSpeed()
                                                                                : strip.getPlayheadSpeedRatio()),
                                        0.125f, 8.0f);
        case PerformanceTarget::SliceLength:
            return juce::jlimit(0.0f, 1.0f, readParamOr(processor.stripSliceLengthParams, strip.getLoopSliceLength()));
        case PerformanceTarget::Scratch:
            return normalizeMacroLinear(strip.getScratchAmount(), 0.0f, 100.0f);
        case PerformanceTarget::GrainSize:
            return normalizeMacroLinear(strip.getGrainSizeMs(), 5.0f, 2400.0f);
        case PerformanceTarget::GrainDensity:
            return normalizeMacroLinear(strip.getGrainDensity(), 0.05f, 0.9f);
        case PerformanceTarget::GrainPitch:
            return normalizeMacroLinear(strip.getGrainPitch(), -48.0f, 48.0f);
        case PerformanceTarget::GrainPitchJitter:
            return normalizeMacroLinear(strip.getGrainPitchJitter(), 0.0f, 48.0f);
        case PerformanceTarget::GrainSpread:
            return juce::jlimit(0.0f, 1.0f, strip.getGrainSpread());
        case PerformanceTarget::GrainJitter:
            return juce::jlimit(0.0f, 1.0f, strip.getGrainJitter());
        case PerformanceTarget::GrainPositionJitter:
            return juce::jlimit(0.0f, 1.0f, strip.getGrainPositionJitter());
        case PerformanceTarget::GrainRandom:
            return juce::jlimit(0.0f, 1.0f, strip.getGrainRandomDepth());
        case PerformanceTarget::GrainArp:
            return juce::jlimit(0.0f, 1.0f, strip.getGrainArpDepth());
        case PerformanceTarget::GrainCloud:
            return juce::jlimit(0.0f, 1.0f, strip.getGrainCloudDepth());
        case PerformanceTarget::GrainEmitter:
            return juce::jlimit(0.0f, 1.0f, strip.getGrainEmitterDepth());
        case PerformanceTarget::GrainEnvelope:
            return juce::jlimit(0.0f, 1.0f, strip.getGrainEnvelope());
        case PerformanceTarget::GrainShape:
            return normalizeMacroLinear(strip.getGrainShape(), -1.0f, 1.0f);
        case PerformanceTarget::Retrigger:
            return processor.getGlobalSceneStutterAmount();
        case PerformanceTarget::Rearrange:
            return 0.0f;
        case PerformanceTarget::DelayMix:
            return juce::jlimit(0.0f, 1.0f, readParamOr(processor.stripDelayMixParams, strip.getDelayMix()));
        case PerformanceTarget::DelayTime:
            return normalizeMacroLinear(readParamOr(processor.stripDelayTimeParams, strip.getDelayTimeBeats()), 0.25f, 4.0f);
        case PerformanceTarget::DelayFeedback:
            return normalizeMacroLinear(readParamOr(processor.stripDelayFeedbackParams, strip.getDelayFeedback()), 0.0f, 0.97f);
        case PerformanceTarget::DelayLowCut:
            return normalizeMacroLinear(readParamOr(processor.stripDelayLowCutParams, strip.getDelayLowCutHz()), 20.0f, 12000.0f);
        case PerformanceTarget::DelayHighCut:
            return normalizeMacroLinear(readParamOr(processor.stripDelayHighCutParams, strip.getDelayHighCutHz()), 200.0f, 20000.0f);
        case PerformanceTarget::None:
        default:
            return getDefaultMacroNormalizedValue(target);
    }
}

void MacroTargetDispatcher::applyTargetValue(MlrVSTAudioProcessor& processor,
                                             int stripIndex,
                                             EnhancedAudioStrip& strip,
                                             PerformanceTarget target,
                                             float normalizedValue)
{
    const float clamped = juce::jlimit(0.0f, 1.0f, normalizedValue);
    const bool isStepMode = (strip.getPlayMode() == EnhancedAudioStrip::PlayMode::Step);
    auto* stepSampler = isStepMode ? strip.getStepSampler() : nullptr;

    switch (target)
    {
        case PerformanceTarget::Cutoff:
            processor.setStripFilterFrequencyControlValue(stripIndex, denormalizeMacroCutoff(clamped));
            break;
        case PerformanceTarget::Resonance:
            processor.setStripFilterResonanceControlValue(stripIndex, denormalizeMacroResonance(clamped));
            break;
        case PerformanceTarget::FilterMorph:
            processor.setStripFilterMorphControlValue(stripIndex, clamped);
            break;
        case PerformanceTarget::DelayMix:
            processor.setStripDelayMixControlValue(stripIndex, clamped);
            break;
        case PerformanceTarget::DelayTime:
            processor.setStripDelayTimeControlValue(stripIndex, denormalizeMacroLinear(clamped, 0.25f, 4.0f));
            break;
        case PerformanceTarget::DelayFeedback:
            processor.setStripDelayFeedbackControlValue(stripIndex, denormalizeMacroLinear(clamped, 0.0f, 0.97f));
            break;
        case PerformanceTarget::DelayLowCut:
            processor.setStripDelayLowCutControlValue(stripIndex, denormalizeMacroLinear(clamped, 20.0f, 12000.0f));
            break;
        case PerformanceTarget::DelayHighCut:
            processor.setStripDelayHighCutControlValue(stripIndex, denormalizeMacroLinear(clamped, 200.0f, 20000.0f));
            break;
        case PerformanceTarget::Pitch:
            processor.applyUserPitchControlToStrip(stripIndex, denormalizeMacroPitch(clamped));
            break;
        case PerformanceTarget::Volume:
            processor.setStripVolumeControlValue(stripIndex, clamped);
            break;
        case PerformanceTarget::Pan:
            processor.setStripPanControlValue(stripIndex, denormalizeMacroLinear(clamped, -1.0f, 1.0f));
            break;
        case PerformanceTarget::FilterEnable:
            processor.setStripFilterEnabledControlValue(stripIndex, clamped >= 0.5f);
            break;
        case PerformanceTarget::Speed:
        {
            const float speed = denormalizeMacroLinear(clamped, 0.125f, 8.0f);
            processor.setStripSpeedControlValue(stripIndex, speed);
            if (stepSampler != nullptr)
                stepSampler->setSpeed(speed);
            break;
        }
        case PerformanceTarget::SliceLength:
            processor.setStripParameterValueFromMacro(stripIndex, "stripSliceLength" + juce::String(stripIndex), clamped);
            strip.setLoopSliceLength(clamped);
            break;
        case PerformanceTarget::Scratch:
            strip.setScratchAmount(denormalizeMacroLinear(clamped, 0.0f, 100.0f));
            break;
        case PerformanceTarget::GrainSize:
            strip.setGrainSizeMs(denormalizeMacroLinear(clamped, 5.0f, 2400.0f));
            break;
        case PerformanceTarget::GrainDensity:
            strip.setGrainDensity(denormalizeMacroLinear(clamped, 0.05f, 0.9f));
            break;
        case PerformanceTarget::GrainPitch:
            strip.setGrainPitch(denormalizeMacroLinear(clamped, -48.0f, 48.0f));
            break;
        case PerformanceTarget::GrainPitchJitter:
            strip.setGrainPitchJitter(denormalizeMacroLinear(clamped, 0.0f, 48.0f));
            break;
        case PerformanceTarget::GrainSpread:
            strip.setGrainSpread(clamped);
            break;
        case PerformanceTarget::GrainJitter:
            strip.setGrainJitter(clamped);
            break;
        case PerformanceTarget::GrainPositionJitter:
            strip.setGrainPositionJitter(clamped);
            break;
        case PerformanceTarget::GrainRandom:
            strip.setGrainRandomDepth(clamped);
            break;
        case PerformanceTarget::GrainArp:
            strip.setGrainArpDepth(clamped);
            break;
        case PerformanceTarget::GrainCloud:
            strip.setGrainCloudDepth(clamped);
            break;
        case PerformanceTarget::GrainEmitter:
            strip.setGrainEmitterDepth(clamped);
            break;
        case PerformanceTarget::GrainEnvelope:
            strip.setGrainEnvelope(clamped);
            break;
        case PerformanceTarget::GrainShape:
            strip.setGrainShape(denormalizeMacroLinear(clamped, -1.0f, 1.0f));
            break;
        case PerformanceTarget::Retrigger:
            processor.setGlobalSceneStutterAmount(clamped);
            break;
        case PerformanceTarget::Rearrange:
            break;
        case PerformanceTarget::None:
        default:
            break;
    }

    if (target != PerformanceTarget::None
        && target != PerformanceTarget::Rearrange
        && target != PerformanceTarget::Retrigger)
    {
        processor.queueActiveSceneAutosave();
    }
}
