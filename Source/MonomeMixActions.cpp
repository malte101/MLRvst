#include "MonomeMixActions.h"
#include "PluginProcessor.h"
#include "AudioEngine.h"
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

const std::array<float, 16> rhythmicSpeeds = {
    0.125f,  // 1/8
    0.1666667f, // 1/6
    0.25f,   // 1/4
    0.3333333f, // 1/3
    0.5f,    // 1/2
    0.6666667f, // 2/3
    0.75f,   // 3/4
    0.875f,  // 7/8
    1.0f,    // 1/1
    1.125f,  // 9/8
    1.25f,   // 5/4
    1.3333333f, // 4/3
    1.5f,    // 3/2
    1.6666667f, // 5/3
    1.75f,   // 7/4
    2.0f     // 2/1
};

const std::array<int, 16> musicalPitchSemitones = {
    -12, -10, -9, -7, -5, -4, -2, -1,
      0,   1,  2,  4,  5,  7,  9, 12
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

int findNearestColumn(float value, float minValue, float maxValue)
{
    const float range = juce::jmax(1.0e-6f, maxValue - minValue);
    const float t = juce::jlimit(0.0f, 1.0f, (value - minValue) / range);
    return juce::jlimit(0, 15, static_cast<int>(std::round(t * 15.0f)));
}

int findNearestSpeedColumn(float speed)
{
    int best = 0;
    float bestDiff = std::abs(speed - rhythmicSpeeds[0]);

    for (int i = 1; i < 16; ++i)
    {
        const float diff = std::abs(speed - rhythmicSpeeds[static_cast<size_t>(i)]);
        if (diff < bestDiff)
        {
            bestDiff = diff;
            best = i;
        }
    }

    return best;
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
}

void handleButtonPress(MlrVSTAudioProcessor& processor,
                       EnhancedAudioStrip& strip,
                       int stripIndex,
                       int x,
                       int mode)
{
    if (mode == speedMode)
    {
        // Match Strip +/- behavior: speed is controlled by beats-per-loop,
        // where 4 beats == normal speed, lower beats == faster, higher == slower.
        const float speedRatio = rhythmicSpeeds[static_cast<size_t>(juce::jlimit(0, 15, x))];
        const float targetBeatsPerLoop = 4.0f / speedRatio;
        strip.setBeatsPerLoop(targetBeatsPerLoop);
    }
    else if (mode == pitchMode)
    {
        const int semitones = musicalPitchSemitones[static_cast<size_t>(juce::jlimit(0, 15, x))];

        if (strip.playMode == EnhancedAudioStrip::PlayMode::Step)
        {
            if (auto* stepSampler = strip.getStepSampler())
            {
                const float ratio = std::pow(2.0f, static_cast<float>(semitones) / 12.0f);
                stepSampler->setSpeed(ratio);
            }
        }
        else
        {
            strip.setPitchShift(static_cast<float>(semitones));
        }

        if (auto* param = processor.parameters.getParameter("stripPitch" + juce::String(stripIndex)))
            param->setValueNotifyingHost(param->convertTo0to1(static_cast<float>(semitones)));
    }
    else if (mode == panMode)
    {
        float pan = (x - 8) / 8.0f;
        pan = juce::jlimit(-1.0f, 1.0f, pan);

        if (strip.playMode == EnhancedAudioStrip::PlayMode::Step)
        {
            if (auto* stepSampler = strip.getStepSampler())
                stepSampler->setPan(pan);
        }

        strip.setPan(pan);

        if (auto* param = processor.parameters.getParameter("stripPan" + juce::String(stripIndex)))
            param->setValueNotifyingHost((pan + 1.0f) / 2.0f);
    }
    else if (mode == volumeMode)
    {
        float vol = x / 15.0f;

        if (strip.playMode == EnhancedAudioStrip::PlayMode::Step)
        {
            if (auto* stepSampler = strip.getStepSampler())
                stepSampler->setVolume(vol);
        }

        strip.setVolume(vol);

        if (auto* param = processor.parameters.getParameter("stripVolume" + juce::String(stripIndex)))
            param->setValueNotifyingHost(vol);
    }
    else if (mode == grainSizeMode)
    {
        strip.setGrainSizeMs(grainSizeFromColumn(juce::jlimit(0, 15, x)));
    }
    else if (mode == swingMode)
    {
        strip.setSwingAmount(unitFromColumn(juce::jlimit(0, 15, x)));
    }
    else if (mode == gateMode)
    {
        strip.setGateAmount(unitFromColumn(juce::jlimit(0, 15, x)));
    }
}

void renderRow(const EnhancedAudioStrip& strip, int y, int newLedState[16][8], int mode)
{
    const bool isStepMode = (strip.playMode == EnhancedAudioStrip::PlayMode::Step);
    auto* stepSampler = isStepMode ? const_cast<EnhancedAudioStrip&>(strip).getStepSampler() : nullptr;

    if (mode == speedMode)
    {
        float beats = strip.getBeatsPerLoop();
        if (beats < 0.0f)
            beats = 4.0f; // Auto mode defaults to musical normal speed

        const float speed = 4.0f / beats;

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
        int semitones = 0;
        if (isStepMode && stepSampler)
            semitones = stepSampler->getPitchOffset();
        else
            semitones = static_cast<int>(std::round(strip.getPitchShift()));

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
        const int activeCol = findNearestColumn(strip.getGateAmount(), 0.0f, 1.0f);
        for (int x = 0; x < 16; ++x)
            newLedState[x][y] = (x <= activeCol) ? (x == activeCol ? 15 : 8) : 2;
    }
}

void handleGrainPageButtonPress(EnhancedAudioStrip& targetStrip, int controlRow, int x)
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

void renderGrainPageRow(const EnhancedAudioStrip& targetStrip, int controlRow, int y, int newLedState[16][8])
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
} // namespace MonomeMixActions
