#include "MonomeFilterActions.h"
#include "AudioEngine.h"

namespace MonomeFilterActions
{
void handleButtonPress(EnhancedAudioStrip& strip, int x, int subPage)
{
    if (subPage == 0)
    {
        // Filter frequency control (log scale: 20Hz - 20kHz)
        float t = x / 15.0f;
        float freq = 20.0f * std::pow(1000.0f, t);

        if (strip.playMode == EnhancedAudioStrip::PlayMode::Step)
        {
            if (auto* stepSampler = strip.getStepSampler())
            {
                stepSampler->setFilterFrequency(freq);
                stepSampler->setFilterEnabled(true);
            }
        }

        strip.setFilterFrequency(freq);
        strip.setFilterEnabled(true);
    }
    else if (subPage == 1)
    {
        // Filter resonance control (0.1 - 10.0 Q)
        float res = 0.1f + (x / 15.0f) * 9.9f;

        if (strip.playMode == EnhancedAudioStrip::PlayMode::Step)
        {
            if (auto* stepSampler = strip.getStepSampler())
            {
                stepSampler->setFilterResonance(res);
                stepSampler->setFilterEnabled(true);
            }
        }

        strip.setFilterResonance(res);
        strip.setFilterEnabled(true);
    }
    else if (subPage == 2)
    {
        // Filter type selection (columns 0-2 for LP/BP/HP)
        if (x > 2)
            return;

        EnhancedAudioStrip::FilterType stripType;
        FilterType stepType;

        if (x == 0)
        {
            stripType = EnhancedAudioStrip::FilterType::LowPass;
            stepType = FilterType::LowPass;
        }
        else if (x == 1)
        {
            stripType = EnhancedAudioStrip::FilterType::BandPass;
            stepType = FilterType::BandPass;
        }
        else
        {
            stripType = EnhancedAudioStrip::FilterType::HighPass;
            stepType = FilterType::HighPass;
        }

        if (strip.playMode == EnhancedAudioStrip::PlayMode::Step)
        {
            if (auto* stepSampler = strip.getStepSampler())
            {
                stepSampler->setFilterType(stepType);
                stepSampler->setFilterEnabled(true);
            }
        }

        strip.setFilterType(stripType);
        strip.setFilterEnabled(true);
    }
}

void renderRow(const EnhancedAudioStrip& strip, int y, int newLedState[16][16], int subPage)
{
    const bool isStepMode = (strip.playMode == EnhancedAudioStrip::PlayMode::Step);
    auto* stepSampler = isStepMode ? const_cast<EnhancedAudioStrip&>(strip).getStepSampler() : nullptr;

    if (subPage == 0)
    {
        // Filter frequency visualization (log scale: 20Hz - 20kHz)
        float freq = isStepMode && stepSampler
                   ? stepSampler->getFilterFrequency()
                   : strip.getFilterFrequency();

        float t = std::log(freq / 20.0f) / std::log(1000.0f);
        t = juce::jlimit(0.0f, 1.0f, t);
        int currentColumn = static_cast<int>(t * 15.0f);

        for (int x = 0; x < 16; ++x)
        {
            if (x == currentColumn)
                newLedState[x][y] = 15;
            else if (x < currentColumn)
                newLedState[x][y] = 4;
            else
                newLedState[x][y] = 1;
        }
    }
    else if (subPage == 1)
    {
        // Filter resonance visualization (0.1 - 10.0 Q)
        float res = isStepMode && stepSampler
                  ? stepSampler->getFilterResonance()
                  : strip.getFilterResonance();

        float t = (res - 0.1f) / 9.9f;
        t = juce::jlimit(0.0f, 1.0f, t);
        int currentColumn = static_cast<int>(t * 15.0f);

        for (int x = 0; x < 16; ++x)
        {
            if (x == currentColumn)
                newLedState[x][y] = 15;
            else if (x < currentColumn)
                newLedState[x][y] = 8;
            else
                newLedState[x][y] = 1;
        }
    }
    else if (subPage == 2)
    {
        // Filter type selection (3 buttons: LP/BP/HP)
        if (isStepMode && stepSampler)
        {
            auto type = stepSampler->getFilterType();
            newLedState[0][y] = (type == FilterType::LowPass) ? 15 : 5;
            newLedState[1][y] = (type == FilterType::BandPass) ? 15 : 5;
            newLedState[2][y] = (type == FilterType::HighPass) ? 15 : 5;
        }
        else
        {
            auto type = strip.getFilterType();
            newLedState[0][y] = (type == EnhancedAudioStrip::FilterType::LowPass) ? 15 : 5;
            newLedState[1][y] = (type == EnhancedAudioStrip::FilterType::BandPass) ? 15 : 5;
            newLedState[2][y] = (type == EnhancedAudioStrip::FilterType::HighPass) ? 15 : 5;
        }

        for (int x = 3; x < 16; ++x)
            newLedState[x][y] = 0;
    }
}
} // namespace MonomeFilterActions
