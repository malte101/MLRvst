#include "AudioEngine.h"
#include "StilsonModel.h"

#ifndef MLRVST_ENABLE_HUOVILAINEN
#define MLRVST_ENABLE_HUOVILAINEN 0
#endif
#if MLRVST_ENABLE_HUOVILAINEN
#include "HuovilainenModel.h"
#endif

#include <cmath>

namespace
{

double sanitizeSceneTransitionRampSecondsFx(double seconds) noexcept
{
    if (!std::isfinite(seconds) || seconds <= 0.0)
        return 0.05;

    return juce::jlimit(0.005, 0.25, seconds);
}

inline float safetyClip0dB(float sample)
{
    return juce::jlimit(-1.0f, 1.0f, sample);
}

inline float filterLimiter0dB(float sample, float resonanceDrive)
{
    const float drive = juce::jlimit(0.0f, 1.0f, resonanceDrive);
    const float threshold = juce::jmap(drive, 0.96f, 0.86f);
    const float ceiling = 0.992f;

    const float mag = std::abs(sample);
    if (mag <= threshold)
        return juce::jlimit(-ceiling, ceiling, sample);

    const float over = mag - threshold;
    const float ratioControl = 10.0f + (22.0f * drive);
    const float compressed = threshold + (over / (1.0f + (ratioControl * over)));

    const float normalized = juce::jlimit(0.0f, 1.25f, compressed / ceiling);
    const float shaped = std::tanh(normalized * (1.15f + (0.95f * drive)));
    const float limited = juce::jmin(ceiling, shaped * ceiling);
    return std::copysign(limited, sample);
}

} // namespace

void EnhancedAudioStrip::publishDisplayedControlState()
{
    displayedFilterFrequency.store(smoothedFilterFrequency.getCurrentValue(), std::memory_order_release);
    displayedFilterResonance.store(smoothedFilterResonance.getCurrentValue(), std::memory_order_release);
    displayedDelayMix.store(smoothedDelayMix.getCurrentValue(), std::memory_order_release);
    displayedDelayTimeBeats.store(smoothedDelayTimeBeats.getCurrentValue(), std::memory_order_release);
    displayedDelayFeedback.store(smoothedDelayFeedback.getCurrentValue(), std::memory_order_release);
    displayedDelayLowCutHz.store(smoothedDelayLowCutHz.getCurrentValue(), std::memory_order_release);
    displayedDelayHighCutHz.store(smoothedDelayHighCutHz.getCurrentValue(), std::memory_order_release);
}

void EnhancedAudioStrip::resetDelayState()
{
    stripDelayWritePos = 0;
    const int maxDelaySamples = juce::jmax(1, static_cast<int>(std::ceil(currentSampleRate * 8.0)));
    stripDelayBuffer.setSize(2, maxDelaySamples, false, true, true);
    stripDelayBuffer.clear();
    delayLowCutFilterL.reset();
    delayLowCutFilterR.reset();
    delayHighCutFilterL.reset();
    delayHighCutFilterR.reset();
}

void EnhancedAudioStrip::armDelayGhostMute()
{
    if (lastDelaySamplesMax <= 0 || currentSampleRate <= 0.0)
        return;

    const auto lastProcessMs = delayLastProcessMs.load(std::memory_order_acquire);
    if (lastProcessMs == 0)
        return;

    const double delayMs = (static_cast<double>(lastDelaySamplesMax) / currentSampleRate) * 1000.0;
    const auto nowMs = juce::Time::getMillisecondCounter();
    if (static_cast<double>(nowMs - lastProcessMs) > juce::jmax(500.0, delayMs))
        delayGhostMuteSamples = lastDelaySamplesMax;
}

void EnhancedAudioStrip::processDelaySample(float& leftSample, float& rightSample, double tempo)
{
    if (stripDelayBuffer.getNumChannels() < 2 || stripDelayBuffer.getNumSamples() <= 1 || currentSampleRate <= 0.0)
        return;

    const float dryInputL = leftSample;
    const float dryInputR = rightSample;

    const float mix = smoothedDelayMix.getNextValue();
    const float feedback = smoothedDelayFeedback.getNextValue();
    const float timeBeats = smoothedDelayTimeBeats.getNextValue();
    const float lowCut = smoothedDelayLowCutHz.getNextValue();
    const float highCut = juce::jmax(lowCut + 20.0f, smoothedDelayHighCutHz.getNextValue());

    // Cheap activity heartbeat for the ghost-echo guard (~every 23ms at 44.1k).
    if ((stripDelayWritePos & 0x3FF) == 0)
        delayLastProcessMs.store(juce::Time::getMillisecondCounter(), std::memory_order_release);

    if (mix <= 1.0e-4f && feedback <= 1.0e-4f)
    {
        // Keep the buffer coherent while inaudible so raising the mix later
        // does not replay stale material.
        stripDelayBuffer.setSample(0, stripDelayWritePos, dryInputL);
        stripDelayBuffer.setSample(1, stripDelayWritePos, dryInputR);
        stripDelayWritePos = (stripDelayWritePos + 1) % stripDelayBuffer.getNumSamples();
        return;
    }

    delayLowCutFilterL.setCutoffFrequency(lowCut);
    delayLowCutFilterR.setCutoffFrequency(lowCut);
    delayHighCutFilterL.setCutoffFrequency(highCut);
    delayHighCutFilterR.setCutoffFrequency(highCut);
    delayLowCutFilterL.setResonance(0.707f);
    delayLowCutFilterR.setResonance(0.707f);
    delayHighCutFilterL.setResonance(0.707f);
    delayHighCutFilterR.setResonance(0.707f);

    const double safeTempo = (tempo > 1.0) ? tempo : 120.0;
    const bool syncEnabled = isDelaySyncEnabled();
    const double baseDelaySeconds = syncEnabled
        ? (60.0 / safeTempo) * juce::jlimit(0.25f, 4.0f, timeBeats)
        : juce::jlimit(0.25f, 4.0f, timeBeats);
    const auto secondsToDelaySamples = [this](double seconds)
    {
        return juce::jlimit(
            1.0,
            static_cast<double>(juce::jmax(1, stripDelayBuffer.getNumSamples() - 1)),
            seconds * currentSampleRate);
    };

    const double delaySamplesL = secondsToDelaySamples(baseDelaySeconds);
    double delaySamplesR = delaySamplesL;

    if (getDelayMode() == DelayMode::Dual)
    {
        const double rightSeconds = juce::jmin(
            7.95,
            baseDelaySeconds * (syncEnabled ? 1.5 : 1.3333333333333333));
        delaySamplesR = secondsToDelaySamples(rightSeconds);
    }

    lastDelaySamplesMax = static_cast<int>(std::ceil(juce::jmax(delaySamplesL, delaySamplesR)));

    // Fractional read with linear interpolation: smoothed time changes glide
    // (tape-style) instead of stepping the read head sample by sample.
    auto readInterpolated = [this](int channel, double delaySamples)
    {
        const int bufferLength = stripDelayBuffer.getNumSamples();
        double readPos = static_cast<double>(stripDelayWritePos) - delaySamples;
        while (readPos < 0.0)
            readPos += static_cast<double>(bufferLength);
        const int index0 = static_cast<int>(readPos);
        const int index1 = (index0 + 1) % bufferLength;
        const float frac = static_cast<float>(readPos - static_cast<double>(index0));
        const float a = stripDelayBuffer.getSample(channel, index0);
        const float b = stripDelayBuffer.getSample(channel, index1);
        return a + ((b - a) * frac);
    };

    float delayedL = readInterpolated(0, delaySamplesL);
    float delayedR = readInterpolated(1, delaySamplesR);

    if (delayGhostMuteSamples > 0)
    {
        // Stale pre-stop material: read silence until the buffer holds only
        // post-retrigger audio. The first new echo lands exactly on time.
        delayedL = 0.0f;
        delayedR = 0.0f;
        --delayGhostMuteSamples;
    }

    delayedL = delayHighCutFilterL.processSample(0, delayLowCutFilterL.processSample(0, delayedL));
    delayedR = delayHighCutFilterR.processSample(0, delayLowCutFilterR.processSample(0, delayedR));

    const auto mode = getDelayMode();
    float wetL = delayedL;
    float wetR = delayedR;
    float writeL = dryInputL;
    float writeR = dryInputR;

    if (mode == DelayMode::Single)
    {
        const float monoInput = 0.5f * (dryInputL + dryInputR);
        const float monoWet = 0.5f * (delayedL + delayedR);
        wetL = monoWet;
        wetR = monoWet;
        writeL = monoInput + (monoWet * feedback);
        writeR = writeL;
    }
    else if (mode == DelayMode::Dual)
    {
        writeL = dryInputL + (delayedL * feedback);
        writeR = dryInputR + (delayedR * feedback);
    }
    else
    {
        const float monoInput = 0.5f * (dryInputL + dryInputR);
        writeL = monoInput + (delayedR * feedback);
        writeR = delayedL * feedback;
    }

    if (!std::isfinite(writeL)) writeL = 0.0f;
    if (!std::isfinite(writeR)) writeR = 0.0f;
    stripDelayBuffer.setSample(0, stripDelayWritePos, writeL);
    stripDelayBuffer.setSample(1, stripDelayWritePos, writeR);
    stripDelayWritePos = (stripDelayWritePos + 1) % stripDelayBuffer.getNumSamples();

    const float dry = juce::jlimit(0.0f, 1.0f, 1.0f - mix);
    leftSample = (dryInputL * dry) + (wetL * mix);
    rightSample = (dryInputR * dry) + (wetR * mix);
}

void EnhancedAudioStrip::updateFilterCoefficients(float frequency, float resonance)
{
    const float freq = juce::jlimit(20.0f, 20000.0f, frequency);
    const float q = juce::jlimit(0.1f, 10.0f, resonance);
    const float ladderRes = juce::jlimit(0.0f, 1.0f, std::pow((q - 0.1f) / 9.9f, 0.8f));

    filterLp.setCutoffFrequency(freq);
    filterBp.setCutoffFrequency(freq);
    filterHp.setCutoffFrequency(freq);
    filterLpStage2.setCutoffFrequency(freq);
    filterBpStage2.setCutoffFrequency(freq);
    filterHpStage2.setCutoffFrequency(freq);

    filterLp.setResonance(q);
    filterBp.setResonance(q);
    filterHp.setResonance(q);
    filterLpStage2.setResonance(q);
    filterBpStage2.setResonance(q);
    filterHpStage2.setResonance(q);

    ladderLp.setCutoffFrequencyHz(freq);
    ladderBp.setCutoffFrequencyHz(freq);
    ladderHp.setCutoffFrequencyHz(freq);
    ladderLp.setResonance(ladderRes);
    ladderBp.setResonance(ladderRes);
    ladderHp.setResonance(ladderRes);

    if (moogLpL != nullptr && moogLpR != nullptr)
    {
        if (std::abs(freq - cachedMoogCutoff) > 1.0e-3f)
        {
            moogLpL->SetCutoff(freq);
            moogLpR->SetCutoff(freq);
            cachedMoogCutoff = freq;
        }
        const float moogRes = juce::jlimit(0.0f, 1.2f, (q - 0.1f) / 8.0f);
        if (std::abs(moogRes - cachedMoogResonance) > 1.0e-4f)
        {
            moogLpL->SetResonance(moogRes);
            moogLpR->SetResonance(moogRes);
            cachedMoogResonance = moogRes;
        }
    }
}

void EnhancedAudioStrip::ensureAnalogFiltersInitialized(FilterAlgorithm algorithm)
{
    auto makeModel = [&](FilterAlgorithm a) -> std::unique_ptr<LadderFilterBase>
    {
        switch (a)
        {
            case FilterAlgorithm::Tpt12:
            case FilterAlgorithm::Tpt24:
            case FilterAlgorithm::Ladder12:
            case FilterAlgorithm::Ladder24:
            case FilterAlgorithm::Comb:
                return std::make_unique<StilsonMoog>(static_cast<float>(currentSampleRate));
            case FilterAlgorithm::MoogHuov:
#if MLRVST_ENABLE_HUOVILAINEN
                return std::make_unique<HuovilainenMoog>(static_cast<float>(currentSampleRate));
#else
                return std::make_unique<StilsonMoog>(static_cast<float>(currentSampleRate));
#endif
            case FilterAlgorithm::MoogStilson:
                return std::make_unique<StilsonMoog>(static_cast<float>(currentSampleRate));
        }

        jassertfalse;
        return std::make_unique<StilsonMoog>(static_cast<float>(currentSampleRate));
    };

    const bool needMoog = (algorithm == FilterAlgorithm::MoogStilson || algorithm == FilterAlgorithm::MoogHuov);
    if (needMoog)
    {
        if (moogLpL == nullptr || moogLpR == nullptr)
        {
            moogLpL = makeModel(algorithm);
            moogLpR = makeModel(algorithm);
            cachedMoogCutoff = -1.0f;
            cachedMoogResonance = -1.0f;
            cachedMoogModel = static_cast<int>(algorithm);
            cachedMoogSampleRate = static_cast<float>(currentSampleRate);
        }
        else
        {
            const bool wrongType = cachedMoogModel != static_cast<int>(algorithm)
                                || std::abs(cachedMoogSampleRate - static_cast<float>(currentSampleRate)) > 0.5f;
            if (wrongType)
            {
                moogLpL = makeModel(algorithm);
                moogLpR = makeModel(algorithm);
                cachedMoogCutoff = -1.0f;
                cachedMoogResonance = -1.0f;
                cachedMoogModel = static_cast<int>(algorithm);
                cachedMoogSampleRate = static_cast<float>(currentSampleRate);
            }
        }
    }
}

void EnhancedAudioStrip::resetCombFilterState()
{
    filterCombWritePos = 0;
    filterCombDampingL = 0.0f;
    filterCombDampingR = 0.0f;
    if (filterCombBuffer.getNumSamples() > 0)
        filterCombBuffer.clear();
}

void EnhancedAudioStrip::processCombFilterSample(float& left, float& right, float frequency, float resonance, float morph)
{
    const int bufferSamples = filterCombBuffer.getNumSamples();
    if (currentSampleRate <= 0.0 || bufferSamples < 4)
        return;

    const float freq = juce::jlimit(20.0f,
                                    juce::jmin(20000.0f, static_cast<float>(currentSampleRate * 0.45)),
                                    frequency);
    const float q = juce::jlimit(0.1f, 10.0f, resonance);
    const float qNorm = std::pow(juce::jlimit(0.0f, 1.0f, (q - 0.1f) / 9.9f), 0.45f);
    const float m = juce::jlimit(0.0f, 1.0f, morph);
    const float brightness = std::abs((m * 2.0f) - 1.0f);
    const float polarity = (m < 0.5f) ? 1.0f : -1.0f;
    const float damping = juce::jmap(brightness, 0.28f, 0.045f);
    const float feedback = juce::jmap(qNorm, 0.16f, 0.992f);
    const float wet = juce::jmap(qNorm, 0.34f, 0.93f);
    const float dry = 1.0f - (wet * 0.48f);
    const float toneGain = juce::jmap(qNorm, 0.78f, 1.55f);
    const float delaySamples = juce::jlimit(1.5f,
                                            static_cast<float>(bufferSamples - 2),
                                            static_cast<float>(currentSampleRate) / freq);

    auto readDelayed = [&](int channel) -> float
    {
        double readPos = static_cast<double>(filterCombWritePos) - static_cast<double>(delaySamples);
        while (readPos < 0.0)
            readPos += static_cast<double>(bufferSamples);

        const int index0 = juce::jlimit(0, bufferSamples - 1, static_cast<int>(std::floor(readPos)));
        const int index1 = (index0 + 1) % bufferSamples;
        const float frac = static_cast<float>(readPos - static_cast<double>(index0));
        const float a = filterCombBuffer.getSample(channel, index0);
        const float b = filterCombBuffer.getSample(channel, index1);
        return a + ((b - a) * frac);
    };

    float delayedL = readDelayed(0);
    float delayedR = readDelayed(1);
    filterCombDampingL += (delayedL - filterCombDampingL) * damping;
    filterCombDampingR += (delayedR - filterCombDampingR) * damping;
    delayedL = filterCombDampingL;
    delayedR = filterCombDampingR;

    const float writeL = filterLimiter0dB(left + (delayedL * feedback * polarity), qNorm);
    const float writeR = filterLimiter0dB(right + (delayedR * feedback * polarity), qNorm);
    filterCombBuffer.setSample(0, filterCombWritePos, writeL);
    filterCombBuffer.setSample(1, filterCombWritePos, writeR);
    filterCombWritePos = (filterCombWritePos + 1) % bufferSamples;

    left = (left * dry) + (delayedL * wet * toneGain);
    right = (right * dry) + (delayedR * wet * toneGain);

    left = filterLimiter0dB(left, qNorm);
    right = filterLimiter0dB(right, qNorm);
    left = safetyClip0dB(left);
    right = safetyClip0dB(right);
}

void EnhancedAudioStrip::processFilterSample(float& left, float& right, float frequency, float resonance, float morph)
{
    const auto algorithm = getFilterAlgorithm();
    if (algorithm == FilterAlgorithm::Comb)
    {
        processCombFilterSample(left, right, frequency, resonance, morph);
        return;
    }

    ensureAnalogFiltersInitialized(algorithm);
    updateFilterCoefficients(frequency, resonance);

    float lpL = filterLp.processSample(0, left);
    float bpL = filterBp.processSample(0, left);
    float hpL = filterHp.processSample(0, left);
    float lpR = filterLp.processSample(1, right);
    float bpR = filterBp.processSample(1, right);
    float hpR = filterHp.processSample(1, right);

    if (algorithm == FilterAlgorithm::Tpt24)
    {
        lpL = filterLpStage2.processSample(0, lpL);
        bpL = filterBpStage2.processSample(0, bpL);
        hpL = filterHpStage2.processSample(0, hpL);
        lpR = filterLpStage2.processSample(1, lpR);
        bpR = filterBpStage2.processSample(1, bpR);
        hpR = filterHpStage2.processSample(1, hpR);
    }
    else if (algorithm == FilterAlgorithm::Ladder12 || algorithm == FilterAlgorithm::Ladder24)
    {
        const int wantedMode = (algorithm == FilterAlgorithm::Ladder24) ? 24 : 12;
        if (cachedLadderMode != wantedMode)
        {
            const auto lpMode = (algorithm == FilterAlgorithm::Ladder24)
                ? juce::dsp::LadderFilterMode::LPF24
                : juce::dsp::LadderFilterMode::LPF12;
            const auto bpMode = (algorithm == FilterAlgorithm::Ladder24)
                ? juce::dsp::LadderFilterMode::BPF24
                : juce::dsp::LadderFilterMode::BPF12;
            const auto hpMode = (algorithm == FilterAlgorithm::Ladder24)
                ? juce::dsp::LadderFilterMode::HPF24
                : juce::dsp::LadderFilterMode::HPF12;

            ladderLp.setMode(lpMode);
            ladderBp.setMode(bpMode);
            ladderHp.setMode(hpMode);
            cachedLadderMode = wantedMode;
        }

        lpL = ladderLp.processSample(left, 0);
        bpL = ladderBp.processSample(left, 0);
        hpL = ladderHp.processSample(left, 0);
        lpR = ladderLp.processSample(right, 1);
        bpR = ladderBp.processSample(right, 1);
        hpR = ladderHp.processSample(right, 1);
    }
    else if ((algorithm == FilterAlgorithm::MoogStilson || algorithm == FilterAlgorithm::MoogHuov)
             && moogLpL != nullptr && moogLpR != nullptr)
    {
        float monoL = left;
        float monoR = right;
        moogLpL->Process(&monoL, 1);
        moogLpR->Process(&monoR, 1);
        lpL = monoL;
        lpR = monoR;
    }
    const float m = juce::jlimit(0.0f, 1.0f, morph);
    float wLP = 0.0f;
    float wBP = 0.0f;
    float wHP = 0.0f;
    if (m <= 0.5f)
    {
        const float t = juce::jlimit(0.0f, 1.0f, m * 2.0f);
        wLP = std::cos(t * juce::MathConstants<float>::halfPi);
        wBP = std::sin(t * juce::MathConstants<float>::halfPi);
    }
    else
    {
        const float t = juce::jlimit(0.0f, 1.0f, (m - 0.5f) * 2.0f);
        wBP = std::cos(t * juce::MathConstants<float>::halfPi);
        wHP = std::sin(t * juce::MathConstants<float>::halfPi);
    }

    const float q = juce::jlimit(0.1f, 10.0f, resonance);
    const float bpComp = 1.0f / (1.0f + (0.17f * juce::jmax(0.0f, q - 0.707f)));
    wBP *= bpComp;
    const float norm = 1.0f / std::sqrt(juce::jmax(1.0e-5f, (wLP * wLP) + (wBP * wBP) + (wHP * wHP)));
    wLP *= norm;
    wBP *= norm;
    wHP *= norm;

    left = (lpL * wLP) + (bpL * wBP) + (hpL * wHP);
    right = (lpR * wLP) + (bpR * wBP) + (hpR * wHP);

    const float resonanceDrive = juce::jlimit(0.0f, 1.0f, (q - 0.707f) / 9.293f);
    const float resonanceGuardGain = juce::jmap(resonanceDrive, 1.0f, 0.68f);
    left *= resonanceGuardGain;
    right *= resonanceGuardGain;

    left = filterLimiter0dB(left, resonanceDrive);
    right = filterLimiter0dB(right, resonanceDrive);
    left = safetyClip0dB(left);
    right = safetyClip0dB(right);
}

constexpr double kDefaultFilterFrequencyRampSeconds = 0.04;
constexpr double kMonomeFilterFrequencyRampSeconds = 0.012;

void EnhancedAudioStrip::setFilterFrequency(float freq)
{
    const float clamped = juce::jlimit(20.0f, 20000.0f, freq);
    // This runs every block from the parameter sync: bail on unchanged targets
    // so in-flight ramps are not restarted (which made the 40ms ramp
    // effectively exponential) and no state is touched redundantly.
    if (clamped == filterFrequency.load(std::memory_order_acquire) && currentSampleRate > 0.0)
        return;

    const float currentSmoothed = smoothedFilterFrequency.getCurrentValue();
    filterFrequency.store(clamped, std::memory_order_release);

    if (currentSampleRate > 0.0)
    {
        smoothedFilterFrequency.reset(currentSampleRate, kDefaultFilterFrequencyRampSeconds);
        smoothedFilterFrequency.setCurrentAndTargetValue(currentSmoothed);
        smoothedFilterFrequency.setTargetValue(clamped);
    }
    else
    {
        smoothedFilterFrequency.setCurrentAndTargetValue(clamped);
        displayedFilterFrequency.store(clamped, std::memory_order_release);
    }
}

void EnhancedAudioStrip::setFilterFrequencyMonomeFast(float freq)
{
    const float clamped = juce::jlimit(20.0f, 20000.0f, freq);
    const float currentSmoothed = smoothedFilterFrequency.getCurrentValue();
    filterFrequency.store(clamped, std::memory_order_release);

    if (currentSampleRate > 0.0)
    {
        smoothedFilterFrequency.reset(currentSampleRate, kMonomeFilterFrequencyRampSeconds);
        smoothedFilterFrequency.setCurrentAndTargetValue(currentSmoothed);
        smoothedFilterFrequency.setTargetValue(clamped);
    }
    else
    {
        smoothedFilterFrequency.setCurrentAndTargetValue(clamped);
        displayedFilterFrequency.store(clamped, std::memory_order_release);
    }

    if (!isFilterEnabled())
        setFilterEnabled(true);
}

void EnhancedAudioStrip::seedFilterFrequencyTransition(float fromValue, float toValue, double rampSeconds)
{
    const float start = juce::jlimit(20.0f, 20000.0f, fromValue);
    const float target = juce::jlimit(20.0f, 20000.0f, toValue);
    smoothedFilterFrequency.reset(currentSampleRate, sanitizeSceneTransitionRampSecondsFx(rampSeconds));
    filterFrequency.store(start, std::memory_order_release);
    displayedFilterFrequency.store(start, std::memory_order_release);
    smoothedFilterFrequency.setCurrentAndTargetValue(start);
    filterFrequency.store(target, std::memory_order_release);
    smoothedFilterFrequency.setTargetValue(target);
    if (!isFilterEnabled())
        setFilterEnabled(true);
}

void EnhancedAudioStrip::setFilterResonance(float res)
{
    const float clamped = juce::jlimit(0.1f, 10.0f, res);
    filterResonance.store(clamped, std::memory_order_release);
    smoothedFilterResonance.setTargetValue(clamped);
}

void EnhancedAudioStrip::seedFilterResonanceTransition(float fromValue, float toValue, double rampSeconds)
{
    const float start = juce::jlimit(0.1f, 10.0f, fromValue);
    const float target = juce::jlimit(0.1f, 10.0f, toValue);
    smoothedFilterResonance.reset(currentSampleRate, sanitizeSceneTransitionRampSecondsFx(rampSeconds));
    filterResonance.store(start, std::memory_order_release);
    displayedFilterResonance.store(start, std::memory_order_release);
    smoothedFilterResonance.setCurrentAndTargetValue(start);
    filterResonance.store(target, std::memory_order_release);
    smoothedFilterResonance.setTargetValue(target);
    if (!isFilterEnabled())
        setFilterEnabled(true);
}

void EnhancedAudioStrip::setFilterMorph(float morph)
{
    const float clamped = juce::jlimit(0.0f, 1.0f, morph);
    filterMorph.store(clamped, std::memory_order_release);
    smoothedFilterMorph.setTargetValue(clamped);

    if (clamped < 0.34f) filterType = FilterType::LowPass;
    else if (clamped <= 0.66f) filterType = FilterType::BandPass;
    else filterType = FilterType::HighPass;
}

void EnhancedAudioStrip::seedFilterMorphTransition(float fromValue, float toValue, double rampSeconds)
{
    const float start = juce::jlimit(0.0f, 1.0f, fromValue);
    const float target = juce::jlimit(0.0f, 1.0f, toValue);
    smoothedFilterMorph.reset(currentSampleRate, sanitizeSceneTransitionRampSecondsFx(rampSeconds));
    filterMorph.store(start, std::memory_order_release);
    smoothedFilterMorph.setCurrentAndTargetValue(start);
    filterMorph.store(target, std::memory_order_release);
    smoothedFilterMorph.setTargetValue(target);

    if (target < 0.34f) filterType = FilterType::LowPass;
    else if (target <= 0.66f) filterType = FilterType::BandPass;
    else filterType = FilterType::HighPass;

    if (!isFilterEnabled())
        setFilterEnabled(true);
}

void EnhancedAudioStrip::setFilterType(FilterType type)
{
    filterType = type;
    switch (type)
    {
        case FilterType::LowPass: setFilterMorph(0.0f); break;
        case FilterType::BandPass: setFilterMorph(0.5f); break;
        case FilterType::HighPass: setFilterMorph(1.0f); break;
        default: setFilterMorph(0.0f); break;
    }
}

void EnhancedAudioStrip::setFilterAlgorithm(FilterAlgorithm algorithm)
{
    const int raw = juce::jlimit(0, 6, static_cast<int>(algorithm));
    const int previous = filterAlgorithm.load(std::memory_order_acquire);
    filterAlgorithm.store(raw, std::memory_order_release);

    if (previous != raw)
    {
        filterLp.reset();
        filterBp.reset();
        filterHp.reset();
        filterLpStage2.reset();
        filterBpStage2.reset();
        filterHpStage2.reset();
        ladderLp.reset();
        ladderBp.reset();
        ladderHp.reset();
        cachedLadderMode = -1;
        moogLpL.reset();
        moogLpR.reset();
        resetCombFilterState();
        cachedMoogCutoff = -1.0f;
        cachedMoogResonance = -1.0f;
        cachedMoogModel = -1;
        cachedMoogSampleRate = -1.0f;
        updateFilterCoefficients(filterFrequency.load(std::memory_order_acquire),
                                 filterResonance.load(std::memory_order_acquire));
    }
}

int EnhancedAudioStrip::resolveDuckDetectorStripIndex(int selfStripIndex, int masterTriggerStripIndex) const
{
    const int selfIndex = juce::jlimit(0, ModernAudioEngine::MaxStrips - 1, selfStripIndex);
    const int sourceSelection = juce::jlimit(0,
                                             ModernAudioEngine::MaxStrips + 1,
                                             duckSourceSelection.load(std::memory_order_acquire));

    if (duckFollowMaster.load(std::memory_order_acquire) != 0
        && masterTriggerStripIndex >= 0
        && masterTriggerStripIndex < ModernAudioEngine::MaxStrips
        && masterTriggerStripIndex != selfIndex)
    {
        return masterTriggerStripIndex;
    }

    if (sourceSelection == 0)
        return selfIndex;

    if (sourceSelection == 1)
    {
        if (masterTriggerStripIndex >= 0
            && masterTriggerStripIndex < ModernAudioEngine::MaxStrips
            && masterTriggerStripIndex != selfIndex)
        {
            return masterTriggerStripIndex;
        }
        return -1;
    }

    const int selectedStrip = sourceSelection - 2;
    if (selectedStrip >= 0 && selectedStrip < ModernAudioEngine::MaxStrips)
        return selectedStrip;

    return -1;
}

bool EnhancedAudioStrip::isDuckActiveForProcessing(int selfStripIndex, int masterTriggerStripIndex) const
{
    if (duckEnabled.load(std::memory_order_acquire) == 0)
        return false;
    if (duckRatio.load(std::memory_order_acquire) <= 1.0f)
        return false;
    return resolveDuckDetectorStripIndex(selfStripIndex, masterTriggerStripIndex) >= 0;
}
