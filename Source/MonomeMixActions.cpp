#include "MonomeMixActions.h"
#include "PluginProcessor.h"
#include "AudioEngine.h"
#include "PlayheadSpeedQuantizer.h"
#include <array>
#include <cmath>

namespace MonomeMixActions
{
namespace
{
constexpr int speedMode = static_cast<int>(MlrVSTAudioProcessor::ControlMode::Speed);
constexpr int pitchMode = static_cast<int>(MlrVSTAudioProcessor::ControlMode::Pitch);
constexpr int panMode = static_cast<int>(MlrVSTAudioProcessor::ControlMode::Pan);
constexpr int volumeMode = static_cast<int>(MlrVSTAudioProcessor::ControlMode::Volume);
constexpr int grainSizeMode = static_cast<int>(MlrVSTAudioProcessor::ControlMode::GrainSize);
constexpr int swingMode = static_cast<int>(MlrVSTAudioProcessor::ControlMode::Swing);
constexpr int gateMode = static_cast<int>(MlrVSTAudioProcessor::ControlMode::Gate);
constexpr float minSliceLength = 0.02f;
constexpr float maxSliceLength = 1.0f;
constexpr float minStepDecayMs = 1.0f;
constexpr float maxStepDecayMs = 4000.0f;
constexpr float stepDecayMidpointMs = 700.0f;

const std::array<int, 16> musicalPitchSemitones = {
    -24, -21, -18, -15, -12, -9, -6, -3,
      0,   3,   6,   9,  12, 15, 18, 24
};

float grainSizeFromColumn(int x)
{
    const float t = juce::jlimit(0.0f, 1.0f, static_cast<float>(x) / 15.0f);
    return 5.0f + (t * (2400.0f - 5.0f));
}

float unitFromColumn(int x)
{
    return juce::jlimit(0.0f, 1.0f, static_cast<float>(x) / 15.0f);
}

float bipolarFromColumn(int x, float minValue, float maxValue)
{
    const float t = unitFromColumn(x);
    return minValue + (t * (maxValue - minValue));
}

const juce::NormalisableRange<float>& sliceLengthRange()
{
    static const juce::NormalisableRange<float> range(minSliceLength, maxSliceLength, 0.001f, 0.5f);
    return range;
}

const juce::NormalisableRange<float>& stepDecayRange()
{
    static const juce::NormalisableRange<float> range = []
    {
        juce::NormalisableRange<float> r(minStepDecayMs, maxStepDecayMs);
        r.setSkewForCentre(stepDecayMidpointMs);
        return r;
    }();
    return range;
}

float sliceLengthFromColumn(int x)
{
    return sliceLengthRange().convertFrom0to1(unitFromColumn(juce::jlimit(0, 15, x)));
}

float stepDecayMsFromColumn(int x)
{
    return stepDecayRange().convertFrom0to1(unitFromColumn(juce::jlimit(0, 15, x)));
}

float delayTimeBeatsFromColumn(int x)
{
    const juce::NormalisableRange<float> range(0.25f, 4.0f, 0.001f, 0.45f);
    return range.convertFrom0to1(unitFromColumn(juce::jlimit(0, 15, x)));
}

int findNearestDelayTimeColumn(float beats)
{
    const juce::NormalisableRange<float> range(0.25f, 4.0f, 0.001f, 0.45f);
    const float t = juce::jlimit(0.0f, 1.0f, range.convertTo0to1(juce::jlimit(0.25f, 4.0f, beats)));
    return juce::jlimit(0, 15, static_cast<int>(std::round(t * 15.0f)));
}

float delayTimeSecondsFromColumn(int x)
{
    const juce::NormalisableRange<float> range(0.25f, 4.0f, 0.001f, 0.4f);
    return range.convertFrom0to1(unitFromColumn(juce::jlimit(0, 15, x)));
}

int findNearestDelaySecondsColumn(float seconds)
{
    const juce::NormalisableRange<float> range(0.25f, 4.0f, 0.001f, 0.4f);
    const float t = juce::jlimit(0.0f, 1.0f, range.convertTo0to1(juce::jlimit(0.25f, 4.0f, seconds)));
    return juce::jlimit(0, 15, static_cast<int>(std::round(t * 15.0f)));
}

float delayLowCutFromColumn(int x)
{
    const juce::NormalisableRange<float> range(20.0f, 12000.0f, 1.0f, 0.25f);
    return range.convertFrom0to1(unitFromColumn(juce::jlimit(0, 15, x)));
}

int findNearestDelayLowCutColumn(float hz)
{
    const juce::NormalisableRange<float> range(20.0f, 12000.0f, 1.0f, 0.25f);
    const float t = juce::jlimit(0.0f, 1.0f, range.convertTo0to1(juce::jlimit(20.0f, 12000.0f, hz)));
    return juce::jlimit(0, 15, static_cast<int>(std::round(t * 15.0f)));
}

float delayHighCutFromColumn(int x)
{
    const juce::NormalisableRange<float> range(200.0f, 20000.0f, 1.0f, 0.3f);
    return range.convertFrom0to1(unitFromColumn(juce::jlimit(0, 15, x)));
}

int findNearestDelayHighCutColumn(float hz)
{
    const juce::NormalisableRange<float> range(200.0f, 20000.0f, 1.0f, 0.3f);
    const float t = juce::jlimit(0.0f, 1.0f, range.convertTo0to1(juce::jlimit(200.0f, 20000.0f, hz)));
    return juce::jlimit(0, 15, static_cast<int>(std::round(t * 15.0f)));
}

int findNearestColumn(float value, float minValue, float maxValue)
{
    const float range = juce::jmax(1.0e-6f, maxValue - minValue);
    const float t = juce::jlimit(0.0f, 1.0f, (value - minValue) / range);
    return juce::jlimit(0, 15, static_cast<int>(std::round(t * 15.0f)));
}

int findNearestSliceLengthColumn(float value)
{
    const float t = juce::jlimit(0.0f, 1.0f,
                                 sliceLengthRange().convertTo0to1(juce::jlimit(minSliceLength, maxSliceLength, value)));
    return juce::jlimit(0, 15, static_cast<int>(std::round(t * 15.0f)));
}

int findNearestStepDecayColumn(float valueMs)
{
    const float t = juce::jlimit(0.0f, 1.0f,
                                 stepDecayRange().convertTo0to1(juce::jlimit(minStepDecayMs, maxStepDecayMs, valueMs)));
    return juce::jlimit(0, 15, static_cast<int>(std::round(t * 15.0f)));
}

int findNearestSpeedColumn(float speed)
{
    return PlayheadSpeedQuantizer::nearestSpeedIndex(speed);
}

int findNearestPitchColumn(int semitones)
{
    int best = 0;
    int bestDiff = std::abs(semitones - musicalPitchSemitones[0]);

    for (int i = 1; i < 16; ++i)
    {
        const int diff = std::abs(semitones - musicalPitchSemitones[static_cast<size_t>(i)]);
        if (diff < bestDiff)
        {
            bestDiff = diff;
            best = i;
        }
    }

    return best;
}

int findNearestGrainSizeColumn(float sizeMs)
{
    int best = 0;
    float bestDiff = std::abs(sizeMs - grainSizeFromColumn(0));

    for (int i = 1; i < 16; ++i)
    {
        const float diff = std::abs(sizeMs - grainSizeFromColumn(i));
        if (diff < bestDiff)
        {
            bestDiff = diff;
            best = i;
        }
    }

    return best;
}

int gateSlicesPerLoop(MlrVSTAudioProcessor::GatePageMode mode)
{
    switch (mode)
    {
        case MlrVSTAudioProcessor::GatePageMode::Quarter: return 4;
        case MlrVSTAudioProcessor::GatePageMode::Sixth: return 6;
        case MlrVSTAudioProcessor::GatePageMode::Eighth: return 8;
        case MlrVSTAudioProcessor::GatePageMode::Sixteenth: return 16;
        case MlrVSTAudioProcessor::GatePageMode::Adaptive:
        default: return 0;
    }
}

float adaptiveGateSpeedForStrip(const EnhancedAudioStrip& strip,
                                MlrVSTAudioProcessor::GatePageMode mode)
{
    const int slicesPerLoop = gateSlicesPerLoop(mode);
    if (slicesPerLoop <= 0)
        return strip.getGateSpeed();

    float beatsPerLoop = strip.getBeatsPerLoop();
    if (!(beatsPerLoop > 0.0f))
        beatsPerLoop = 4.0f;

    return juce::jlimit(0.25f, 8.0f, static_cast<float>(slicesPerLoop) / beatsPerLoop);
}

void setStripParameter(MlrVSTAudioProcessor& processor, int stripIndex, const juce::String& paramId, float value)
{
    if (auto* param = processor.parameters.getParameter(paramId + juce::String(stripIndex)))
        param->setValueNotifyingHost(param->convertTo0to1(value));
}

void setStripBoolParameter(MlrVSTAudioProcessor& processor, int stripIndex, const juce::String& paramId, bool enabled)
{
    if (auto* param = processor.parameters.getParameter(paramId + juce::String(stripIndex)))
        param->setValueNotifyingHost(param->convertTo0to1(enabled ? 1.0f : 0.0f));
}
}

void applyButtonPressLive(EnhancedAudioStrip& strip,
                          int x,
                          int mode,
                          int gatePageMode)
{
    const int clampedColumn = juce::jlimit(0, 15, x);

    if (mode == speedMode)
    {
        const float speedRatio = PlayheadSpeedQuantizer::ratioFromColumn(clampedColumn);
        if (strip.getPlayMode() == EnhancedAudioStrip::PlayMode::Grain)
            strip.setPlaybackSpeed(PlayheadSpeedQuantizer::grainPlaybackSpeedFromControl(speedRatio));
        else
            strip.setPlayheadSpeedRatio(speedRatio);
    }
    else if (mode == panMode)
    {
        float pan = (clampedColumn - 8) / 8.0f;
        pan = juce::jlimit(-1.0f, 1.0f, pan);

        if (strip.playMode == EnhancedAudioStrip::PlayMode::Step)
        {
            if (auto* stepSampler = strip.getStepSampler())
                stepSampler->setPan(pan);
        }

        strip.setPan(pan);
    }
    else if (mode == volumeMode)
    {
        const float vol = clampedColumn / 15.0f;

        if (strip.playMode == EnhancedAudioStrip::PlayMode::Step)
        {
            if (auto* stepSampler = strip.getStepSampler())
                stepSampler->setVolume(vol);
        }

        strip.setVolume(vol);
    }
    else if (mode == grainSizeMode)
    {
        strip.setGrainSizeMs(grainSizeFromColumn(clampedColumn));
    }
    else if (mode == swingMode)
    {
        strip.setSwingAmount(unitFromColumn(clampedColumn));
    }
    else if (mode == gateMode)
    {
        if (strip.playMode == EnhancedAudioStrip::PlayMode::Step)
        {
            strip.setStepEnvelopeDecayMs(stepDecayMsFromColumn(clampedColumn));
        }
        else
        {
            const auto safeGateMode = static_cast<MlrVSTAudioProcessor::GatePageMode>(juce::jlimit(
                static_cast<int>(MlrVSTAudioProcessor::GatePageMode::Adaptive),
                static_cast<int>(MlrVSTAudioProcessor::GatePageMode::Sixteenth),
                gatePageMode));

            if (safeGateMode == MlrVSTAudioProcessor::GatePageMode::Adaptive)
            {
                const float sliceLength = sliceLengthFromColumn(clampedColumn);
                strip.setLoopSliceLength(sliceLength);
                strip.setGateAmount(0.0f);
            }
            else
            {
                const float openness = unitFromColumn(clampedColumn);
                strip.setGateSpeed(adaptiveGateSpeedForStrip(strip, safeGateMode));
                strip.setGateShape(openness);
                strip.setGateAmount(1.0f);
            }
        }
    }
}

void handleButtonPress(MlrVSTAudioProcessor& processor,
                       EnhancedAudioStrip& strip,
                       int stripIndex,
                       int x,
                       int mode)
{
    if (mode == speedMode)
    {
        applyButtonPressLive(strip, x, mode, static_cast<int>(processor.getGatePageMode()));
        const float speedRatio = PlayheadSpeedQuantizer::ratioFromColumn(juce::jlimit(0, 15, x));

        if (auto* param = processor.parameters.getParameter("stripSpeed" + juce::String(stripIndex)))
            param->setValueNotifyingHost(param->convertTo0to1(speedRatio));
    }
    else if (mode == pitchMode)
    {
        const int semitones = musicalPitchSemitones[static_cast<size_t>(juce::jlimit(0, 15, x))];
        processor.applyUserPitchControlToStrip(stripIndex, static_cast<float>(semitones));
    }
    else if (mode == panMode)
    {
        applyButtonPressLive(strip, x, mode, static_cast<int>(processor.getGatePageMode()));
        float pan = (x - 8) / 8.0f;
        pan = juce::jlimit(-1.0f, 1.0f, pan);

        if (auto* param = processor.parameters.getParameter("stripPan" + juce::String(stripIndex)))
            param->setValueNotifyingHost((pan + 1.0f) / 2.0f);
    }
    else if (mode == volumeMode)
    {
        applyButtonPressLive(strip, x, mode, static_cast<int>(processor.getGatePageMode()));
        float vol = x / 15.0f;

        if (auto* param = processor.parameters.getParameter("stripVolume" + juce::String(stripIndex)))
            param->setValueNotifyingHost(vol);
    }
    else if (mode == grainSizeMode)
    {
        applyButtonPressLive(strip, x, mode, static_cast<int>(processor.getGatePageMode()));
    }
    else if (mode == swingMode)
    {
        applyButtonPressLive(strip, x, mode, static_cast<int>(processor.getGatePageMode()));
    }
    else if (mode == gateMode)
    {
        applyButtonPressLive(strip, x, mode, static_cast<int>(processor.getGatePageMode()));
        const int clampedColumn = juce::jlimit(0, 15, x);
        if (strip.playMode == EnhancedAudioStrip::PlayMode::Step)
        {
        }
        else
        {
            const auto gatePageMode = processor.getGatePageMode();
            if (gatePageMode == MlrVSTAudioProcessor::GatePageMode::Adaptive)
            {
                const float sliceLength = sliceLengthFromColumn(clampedColumn);
                if (auto* param = processor.parameters.getParameter("stripSliceLength" + juce::String(stripIndex)))
                    param->setValueNotifyingHost(param->convertTo0to1(sliceLength));
            }
        }
    }
}

void renderRow(const EnhancedAudioStrip& strip,
               const MlrVSTAudioProcessor& processor,
               int y,
               int newLedState[16][16],
               int mode)
{
    const bool isStepMode = (strip.playMode == EnhancedAudioStrip::PlayMode::Step);
    auto* stepSampler = isStepMode ? const_cast<EnhancedAudioStrip&>(strip).getStepSampler() : nullptr;

    if (mode == speedMode)
    {
        const float speed = PlayheadSpeedQuantizer::quantizeRatio(
            strip.getPlayheadSpeedRatio());
        const int activeCol = findNearestSpeedColumn(speed);

        for (int x = 0; x < 16; ++x)
        {
            newLedState[x][y] = 4;
            if (x == 8)
                newLedState[x][y] = 6;
            if (x == activeCol)
                newLedState[x][y] = 15;
        }
    }
    else if (mode == pitchMode)
    {
        const int semitones = static_cast<int>(std::round(processor.getPitchSemitonesForDisplay(strip)));

        const int activeCol = findNearestPitchColumn(semitones);

        for (int x = 0; x < 16; ++x)
        {
            newLedState[x][y] = 4;
            if (x == 8)
                newLedState[x][y] = 6;
            if (x == activeCol)
                newLedState[x][y] = 15;
        }
    }
    else if (mode == panMode)
    {
        float pan = 0.0f;
        if (isStepMode && stepSampler)
            pan = stepSampler->getPan();
        else
            pan = strip.getPan();

        int panX = 8 + static_cast<int>(pan * 8.0f);
        panX = juce::jlimit(0, 15, panX);

        for (int x = 0; x < 16; ++x)
        {
            if (x == panX)
                newLedState[x][y] = 15;
            else if (x == 8)
                newLedState[x][y] = 6;
            else
                newLedState[x][y] = 2;
        }
    }
    else if (mode == volumeMode)
    {
        float vol = 0.0f;
        if (isStepMode && stepSampler)
            vol = stepSampler->getVolume();
        else
            vol = strip.getVolume();

        int numLit = static_cast<int>(vol * 16.0f);
        for (int x = 0; x < 16; ++x)
            newLedState[x][y] = (x < numLit) ? 12 : 2;
    }
    else if (mode == grainSizeMode)
    {
        const int activeCol = findNearestGrainSizeColumn(strip.getGrainSizeMs());
        for (int x = 0; x < 16; ++x)
        {
            if (x == activeCol)
                newLedState[x][y] = 15;
            else if (x <= activeCol)
                newLedState[x][y] = 8;
            else
                newLedState[x][y] = 2;
        }
    }
    else if (mode == swingMode)
    {
        const int activeCol = findNearestColumn(strip.getSwingAmount(), 0.0f, 1.0f);
        for (int x = 0; x < 16; ++x)
            newLedState[x][y] = (x <= activeCol) ? (x == activeCol ? 15 : 8) : 2;
    }
    else if (mode == gateMode)
    {
        int activeCol = 0;
        if (isStepMode)
        {
            activeCol = findNearestStepDecayColumn(strip.getStepEnvelopeDecayMs());
        }
        else if (processor.getGatePageMode() == MlrVSTAudioProcessor::GatePageMode::Adaptive)
        {
            activeCol = findNearestSliceLengthColumn(strip.getLoopSliceLength());
        }
        else
        {
            activeCol = (strip.getGateAmount() <= 0.01f)
                ? 15
                : findNearestColumn(strip.getGateShape(), 0.0f, 1.0f);
        }

        for (int x = 0; x < 16; ++x)
            newLedState[x][y] = (x <= activeCol) ? (x == activeCol ? 15 : 8) : 2;
    }
}

void applyGrainPageButtonPressLive(EnhancedAudioStrip& targetStrip, int controlRow, int x)
{
    const int cx = juce::jlimit(0, 15, x);
    switch (juce::jlimit(0, 5, controlRow))
    {
        case 0: targetStrip.setGrainSizeMs(grainSizeFromColumn(cx)); break;
        case 1: targetStrip.setGrainDensity(bipolarFromColumn(cx, 0.05f, 0.9f)); break;
        case 2: targetStrip.setGrainPitch(bipolarFromColumn(cx, -24.0f, 24.0f)); break;
        case 3: targetStrip.setGrainJitter(unitFromColumn(cx)); break;      // SJTR
        case 4: targetStrip.setGrainRandomDepth(unitFromColumn(cx)); break; // RAND
        case 5: targetStrip.setGrainEnvelope(unitFromColumn(cx)); break;    // ENV
        default: break;
    }
}

void handleGrainPageButtonPress(EnhancedAudioStrip& targetStrip, int controlRow, int x)
{
    applyGrainPageButtonPressLive(targetStrip, controlRow, x);
}

void renderGrainPageRow(const EnhancedAudioStrip& targetStrip, int controlRow, int y, int newLedState[16][16])
{
    int activeCol = 0;
    switch (juce::jlimit(0, 5, controlRow))
    {
        case 0: activeCol = findNearestGrainSizeColumn(targetStrip.getGrainSizeMs()); break;
        case 1: activeCol = findNearestColumn(targetStrip.getGrainDensity(), 0.05f, 0.9f); break;
        case 2: activeCol = findNearestColumn(targetStrip.getGrainPitch(), -24.0f, 24.0f); break;
        case 3: activeCol = findNearestColumn(targetStrip.getGrainJitter(), 0.0f, 1.0f); break;
        case 4: activeCol = findNearestColumn(targetStrip.getGrainRandomDepth(), 0.0f, 1.0f); break;
        case 5: activeCol = findNearestColumn(targetStrip.getGrainEnvelope(), 0.0f, 1.0f); break;
        default: break;
    }

    for (int x = 0; x < 16; ++x)
    {
        if (x == activeCol)
            newLedState[x][y] = 15;
        else if (x <= activeCol)
            newLedState[x][y] = 7;
        else
            newLedState[x][y] = 2;
    }

    if (controlRow == 2)
        newLedState[8][y] = juce::jmax(newLedState[8][y], 9);
}

void handleDelayPageButtonPress(MlrVSTAudioProcessor& processor,
                                EnhancedAudioStrip& targetStrip,
                                int stripIndex,
                                int controlRow,
                                int x)
{
    applyDelayPageButtonPressLive(targetStrip, controlRow, x);

    const int cx = juce::jlimit(0, 15, x);
    switch (juce::jlimit(0, 5, controlRow))
    {
        case 0:
        {
            const float mix = unitFromColumn(cx);
            setStripParameter(processor, stripIndex, "stripDelayMix", mix);
            break;
        }
        case 1:
        {
            const bool syncEnabled = targetStrip.isDelaySyncEnabled();
            const float timeValue = syncEnabled ? delayTimeBeatsFromColumn(cx) : delayTimeSecondsFromColumn(cx);
            setStripParameter(processor, stripIndex, "stripDelayTime", timeValue);
            break;
        }
        case 2:
        {
            const float feedback = 0.97f * unitFromColumn(cx);
            setStripParameter(processor, stripIndex, "stripDelayFeedback", feedback);
            break;
        }
        case 3:
        {
            const float hz = delayLowCutFromColumn(cx);
            setStripParameter(processor, stripIndex, "stripDelayLowCut", hz);
            break;
        }
        case 4:
        {
            const float hz = delayHighCutFromColumn(cx);
            setStripParameter(processor, stripIndex, "stripDelayHighCut", hz);
            break;
        }
        case 5:
        {
            if (cx <= 2)
            {
                setStripParameter(processor, stripIndex, "stripDelayMode", static_cast<float>(cx));
            }
            else if (cx >= 12)
            {
                const bool syncEnabled = (cx >= 14);
                setStripBoolParameter(processor, stripIndex, "stripDelaySync", syncEnabled);
            }
            break;
        }
        default:
            break;
    }
}

void applyDelayPageButtonPressLive(EnhancedAudioStrip& targetStrip,
                                   int controlRow,
                                   int x)
{
    const int cx = juce::jlimit(0, 15, x);
    switch (juce::jlimit(0, 5, controlRow))
    {
        case 0: targetStrip.setDelayMix(unitFromColumn(cx)); break;
        case 1:
        {
            const bool syncEnabled = targetStrip.isDelaySyncEnabled();
            const float timeValue = syncEnabled ? delayTimeBeatsFromColumn(cx) : delayTimeSecondsFromColumn(cx);
            targetStrip.setDelayTimeBeats(timeValue);
            break;
        }
        case 2: targetStrip.setDelayFeedback(0.97f * unitFromColumn(cx)); break;
        case 3: targetStrip.setDelayLowCutHz(delayLowCutFromColumn(cx)); break;
        case 4: targetStrip.setDelayHighCutHz(delayHighCutFromColumn(cx)); break;
        case 5:
        {
            if (cx <= 2)
                targetStrip.setDelayMode(static_cast<EnhancedAudioStrip::DelayMode>(cx));
            else if (cx >= 12)
                targetStrip.setDelaySyncEnabled(cx >= 14);
            break;
        }
        default: break;
    }
}

void renderDelayPageRow(const EnhancedAudioStrip& targetStrip, int controlRow, int y, int newLedState[16][16])
{
    switch (juce::jlimit(0, 5, controlRow))
    {
        case 0:
        {
            const int activeCol = findNearestColumn(targetStrip.getDisplayedDelayMix(), 0.0f, 1.0f);
            for (int x = 0; x < 16; ++x)
                newLedState[x][y] = (x <= activeCol) ? (x == activeCol ? 15 : 8) : 2;
            break;
        }
        case 1:
        {
            const int activeCol = targetStrip.isDelaySyncEnabled()
                ? findNearestDelayTimeColumn(targetStrip.getDisplayedDelayTimeBeats())
                : findNearestDelaySecondsColumn(targetStrip.getDisplayedDelayTimeBeats());
            for (int x = 0; x < 16; ++x)
                newLedState[x][y] = (x <= activeCol) ? (x == activeCol ? 15 : 8) : 2;
            break;
        }
        case 2:
        {
            const int activeCol = findNearestColumn(targetStrip.getDisplayedDelayFeedback(), 0.0f, 0.97f);
            for (int x = 0; x < 16; ++x)
                newLedState[x][y] = (x <= activeCol) ? (x == activeCol ? 15 : 8) : 2;
            break;
        }
        case 3:
        {
            const int activeCol = findNearestDelayLowCutColumn(targetStrip.getDisplayedDelayLowCutHz());
            for (int x = 0; x < 16; ++x)
                newLedState[x][y] = (x <= activeCol) ? (x == activeCol ? 15 : 8) : 2;
            break;
        }
        case 4:
        {
            const int activeCol = findNearestDelayHighCutColumn(targetStrip.getDisplayedDelayHighCutHz());
            for (int x = 0; x < 16; ++x)
                newLedState[x][y] = (x <= activeCol) ? (x == activeCol ? 15 : 8) : 2;
            break;
        }
        case 5:
        {
            const int modeIndex = static_cast<int>(targetStrip.getDelayMode());
            const bool syncEnabled = targetStrip.isDelaySyncEnabled();
            for (int x = 0; x < 16; ++x)
                newLedState[x][y] = 0;
            for (int x = 0; x < 3; ++x)
                newLedState[x][y] = (x == modeIndex) ? 15 : 5;
            for (int x = 12; x < 14; ++x)
                newLedState[x][y] = syncEnabled ? 2 : 9;
            for (int x = 14; x < 16; ++x)
                newLedState[x][y] = syncEnabled ? 15 : 4;
            break;
        }
        default:
            break;
    }
}
} // namespace MonomeMixActions
