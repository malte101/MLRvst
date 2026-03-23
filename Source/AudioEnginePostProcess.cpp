#include "AudioEngine.h"

#include <cmath>

namespace
{

float smoothingCoeffFromMs(float ms, double sampleRate)
{
    const double safeMs = juce::jmax(0.001, static_cast<double>(ms));
    const double safeRate = juce::jmax(1.0, sampleRate);
    return static_cast<float>(std::exp(-1.0 / ((safeMs * 0.001) * safeRate)));
}

float computeDuckGainFromEnvelope(float envelope,
                                  float thresholdDb,
                                  float ratio)
{
    const float safeEnv = juce::jmax(1.0e-6f, envelope);
    const float envDb = juce::Decibels::gainToDecibels(safeEnv, -120.0f);
    const float safeThreshold = juce::jlimit(-60.0f, 0.0f, thresholdDb);
    const float safeRatio = juce::jlimit(1.0f, 20.0f, ratio);
    if (envDb <= safeThreshold || safeRatio <= 1.0f)
        return 1.0f;

    const float overDb = envDb - safeThreshold;
    const float compressedDb = safeThreshold + (overDb / safeRatio);
    const float reductionDb = compressedDb - envDb;
    return juce::Decibels::decibelsToGain(reductionDb);
}

} // namespace

juce::AudioBuffer<float>* ModernAudioEngine::resolveFinalTargetBuffer(
    juce::AudioBuffer<float>& mainBuffer,
    const std::array<juce::AudioBuffer<float>*, MaxStrips>* stripOutputs,
    size_t stripIndex) const
{
    if (stripOutputs != nullptr)
    {
        auto* requested = (*stripOutputs)[stripIndex];
        if (requested != nullptr && requested->getNumChannels() > 0)
            return requested;
    }
    return &mainBuffer;
}

size_t ModernAudioEngine::collectPostProcessBuffers(
    juce::AudioBuffer<float>& mainBuffer,
    const std::array<juce::AudioBuffer<float>*, MaxStrips>* stripOutputs,
    std::array<juce::AudioBuffer<float>*, MaxStrips + 1>& buffers) const
{
    size_t count = 0;
    auto pushUniqueBuffer = [&](juce::AudioBuffer<float>* candidate)
    {
        if (candidate == nullptr || candidate->getNumChannels() <= 0)
            return;
        for (size_t i = 0; i < count; ++i)
        {
            if (buffers[i] == candidate)
                return;
        }
        buffers[count++] = candidate;
    };

    pushUniqueBuffer(&mainBuffer);
    if (stripOutputs != nullptr)
    {
        for (size_t i = 0; i < static_cast<size_t>(MaxStrips); ++i)
            pushUniqueBuffer((*stripOutputs)[i]);
    }

    return count;
}

void ModernAudioEngine::applyDuckPostProcessing(
    juce::AudioBuffer<float>& mainBuffer,
    const std::array<juce::AudioBuffer<float>*, MaxStrips>* stripOutputs,
    int numSamples,
    int masterDuckTrigger)
{
    const float detectorAttackCoeff = smoothingCoeffFromMs(2.0f, currentSampleRate);
    const float detectorReleaseCoeff = smoothingCoeffFromMs(35.0f, currentSampleRate);

    for (size_t stripIndex = 0; stripIndex < static_cast<size_t>(MaxStrips); ++stripIndex)
    {
        auto& scratch = duckStripScratchBuffers[stripIndex];
        auto& detectorBuffer = duckDetectorEnvelopeBuffers[stripIndex];
        const auto* srcL = scratch.getReadPointer(0);
        const auto* srcR = scratch.getReadPointer(1);
        auto* detectorWrite = detectorBuffer.getWritePointer(0);
        float detectorState = duckDetectorEnvelopeStates[stripIndex];

        for (int sample = 0; sample < numSamples; ++sample)
        {
            const float mono = 0.5f * (std::abs(srcL[sample]) + std::abs(srcR[sample]));
            const float coeff = (mono > detectorState) ? detectorAttackCoeff : detectorReleaseCoeff;
            detectorState = mono + (coeff * (detectorState - mono));
            detectorWrite[sample] = detectorState;
        }

        duckDetectorEnvelopeStates[stripIndex] = detectorState;
    }

    for (size_t stripIndex = 0; stripIndex < static_cast<size_t>(MaxStrips); ++stripIndex)
    {
        auto* strip = strips[stripIndex].get();
        if (strip == nullptr)
            continue;

        auto& scratch = duckStripScratchBuffers[stripIndex];
        auto* destination = resolveFinalTargetBuffer(mainBuffer, stripOutputs, stripIndex);
        if (destination == nullptr || destination->getNumChannels() <= 0)
            continue;

        const auto* srcL = scratch.getReadPointer(0);
        const auto* srcR = scratch.getReadPointer(1);
        const bool applyDuck = strip->isDuckActiveForProcessing(static_cast<int>(stripIndex), masterDuckTrigger);
        const int detectorStripIndex = applyDuck
            ? strip->resolveDuckDetectorStripIndex(static_cast<int>(stripIndex), masterDuckTrigger)
            : -1;

        if (!applyDuck || detectorStripIndex < 0 || detectorStripIndex >= MaxStrips)
        {
            strip->resetDuckGainSmoothing();
            if (destination->getNumChannels() == 1)
            {
                destination->addFrom(0, 0, scratch, 0, 0, numSamples, 0.5f);
                destination->addFrom(0, 0, scratch, 1, 0, numSamples, 0.5f);
            }
            else
            {
                destination->addFrom(0, 0, scratch, 0, 0, numSamples);
                destination->addFrom(1, 0, scratch, 1, 0, numSamples);
            }
            continue;
        }

        const auto& detectorBuffer = duckDetectorEnvelopeBuffers[static_cast<size_t>(detectorStripIndex)];
        const auto* detectorRead = detectorBuffer.getReadPointer(0);
        float smoothedGain = strip->getDuckSmoothedGain();
        const float attackCoeff = smoothingCoeffFromMs(strip->getDuckAttackMs(), currentSampleRate);
        const float releaseCoeff = smoothingCoeffFromMs(strip->getDuckReleaseMs(), currentSampleRate);
        const float thresholdDb = strip->getDuckThresholdDb();
        const float ratio = strip->getDuckRatio();
        const float gainComp = juce::Decibels::decibelsToGain(strip->getDuckGainCompDb());

        if (destination->getNumChannels() == 1)
        {
            auto* dst = destination->getWritePointer(0);
            for (int sample = 0; sample < numSamples; ++sample)
            {
                const float targetGain = computeDuckGainFromEnvelope(detectorRead[sample], thresholdDb, ratio);
                const float coeff = (targetGain < smoothedGain) ? attackCoeff : releaseCoeff;
                smoothedGain = targetGain + (coeff * (smoothedGain - targetGain));
                const float finalGain = smoothedGain * gainComp;
                dst[sample] += (0.5f * (srcL[sample] + srcR[sample])) * finalGain;
            }
        }
        else
        {
            auto* dstL = destination->getWritePointer(0);
            auto* dstR = destination->getWritePointer(1);
            for (int sample = 0; sample < numSamples; ++sample)
            {
                const float targetGain = computeDuckGainFromEnvelope(detectorRead[sample], thresholdDb, ratio);
                const float coeff = (targetGain < smoothedGain) ? attackCoeff : releaseCoeff;
                smoothedGain = targetGain + (coeff * (smoothedGain - targetGain));
                const float finalGain = smoothedGain * gainComp;
                dstL[sample] += srcL[sample] * finalGain;
                dstR[sample] += srcR[sample] * finalGain;
            }
        }

        strip->setDuckSmoothedGain(smoothedGain);
    }
}

void ModernAudioEngine::applyOutputPostProcessing(
    juce::AudioBuffer<float>& mainBuffer,
    const std::array<juce::AudioBuffer<float>*, MaxStrips>* stripOutputs,
    const juce::AudioBuffer<float>& inputCopy,
    int numSamples,
    int numChannels,
    float inputMonitorVol)
{
    std::array<juce::AudioBuffer<float>*, MaxStrips + 1> buffersToPostProcess{};
    const size_t buffersToPostProcessCount = collectPostProcessBuffers(mainBuffer, stripOutputs, buffersToPostProcess);

    for (size_t b = 0; b < buffersToPostProcessCount; ++b)
    {
        auto* target = buffersToPostProcess[b];
        const int targetChannels = target->getNumChannels();
        const int targetSamples = target->getNumSamples();
        for (int ch = 0; ch < targetChannels; ++ch)
        {
            auto* write = target->getWritePointer(ch);
            for (int i = 0; i < targetSamples; ++i)
            {
                if (!std::isfinite(write[i]))
                    write[i] = 0.0f;
            }
        }

        target->applyGain(masterVolume.load());
    }

    if (inputMonitorVol > 0.0f && inputCopy.getNumChannels() > 0 && inputCopy.getNumSamples() > 0)
    {
        const int channelsToMix = juce::jmin(numChannels, inputCopy.getNumChannels());
        for (int ch = 0; ch < channelsToMix; ++ch)
            mainBuffer.addFrom(ch, 0, inputCopy, ch, 0, numSamples, inputMonitorVol);

        if (inputCopy.getNumChannels() == 1 && numChannels == 2)
            mainBuffer.addFrom(1, 0, inputCopy, 0, 0, numSamples, inputMonitorVol);
    }

    if (limiterEnabled.load(std::memory_order_acquire) != 0)
    {
        const float limiterCeiling = juce::Decibels::decibelsToGain(
            juce::jlimit(-24.0f, 0.0f, limiterThresholdDb.load(std::memory_order_acquire)));
        for (size_t b = 0; b < buffersToPostProcessCount; ++b)
        {
            auto* target = buffersToPostProcess[b];
            const int targetChannels = target->getNumChannels();
            const int targetSamples = target->getNumSamples();
            if (targetSamples <= 0 || targetChannels <= 0)
                continue;

            for (int ch = 0; ch < targetChannels; ++ch)
            {
                auto* write = target->getWritePointer(ch);
                for (int sample = 0; sample < targetSamples; ++sample)
                    write[sample] = juce::jlimit(-limiterCeiling, limiterCeiling, write[sample]);
            }
        }
    }
}
