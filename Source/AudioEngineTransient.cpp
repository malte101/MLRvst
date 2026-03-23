/*
  ==============================================================================

    AudioEngineTransient.cpp
    Transient detection and slice-map rebuild domain for the audio engine

  ==============================================================================
*/

#include "AudioEngine.h"
#include "TransientDetectionConfig.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <utility>
#include <vector>

namespace
{
double meanAbsoluteLevelInRange(const std::vector<double>& absolutePrefixSum, int startSample, int endSample)
{
    const int clampedStart = juce::jmax(0, startSample);
    const int clampedEnd = juce::jmax(clampedStart, juce::jmin(static_cast<int>(absolutePrefixSum.size()) - 1, endSample));
    const int sampleCount = juce::jmax(1, clampedEnd - clampedStart);
    return (absolutePrefixSum[static_cast<size_t>(clampedEnd)] - absolutePrefixSum[static_cast<size_t>(clampedStart)])
        / static_cast<double>(sampleCount);
}

std::vector<int> refineOnsetSamplesToLeadingEdges(const std::vector<float>& monoSamples,
                                                  const std::vector<int>& onsetSamples,
                                                  int frameSize,
                                                  int hopSize)
{
    std::vector<int> refined;
    const int totalSamples = static_cast<int>(monoSamples.size());
    if (totalSamples <= 2 || onsetSamples.empty())
        return refined;

    std::vector<double> absolutePrefixSum(static_cast<size_t>(totalSamples) + 1u, 0.0);
    for (int i = 0; i < totalSamples; ++i)
        absolutePrefixSum[static_cast<size_t>(i) + 1u] = absolutePrefixSum[static_cast<size_t>(i)] + std::abs(monoSamples[static_cast<size_t>(i)]);
    const float snap01 = TransientDetectionConfig::configuredTransientSnap01();

    const int envelopeRadius = juce::jmax(2, juce::jmin(32, juce::jmax(4, hopSize / 12)));
    std::vector<float> amplitudeEnvelope(static_cast<size_t>(totalSamples), 0.0f);
    for (int i = 0; i < totalSamples; ++i)
    {
        amplitudeEnvelope[static_cast<size_t>(i)] = static_cast<float>(
            meanAbsoluteLevelInRange(absolutePrefixSum, i - envelopeRadius, i + envelopeRadius + 1));
    }

    const int searchBehind = juce::jmax(48, juce::jmin(frameSize, juce::jmax(frameSize * 3 / 4, hopSize * 5)));
    const int searchAhead = juce::jmax(12, juce::jmin(frameSize / 2, juce::jmax(hopSize, frameSize / 6)));
    const int lookBehind = juce::jmax(4, juce::jmin(32, juce::jmax(6, hopSize / 6)));
    const int lookAhead = juce::jmax(8, juce::jmin(48, juce::jmax(10, hopSize / 3)));

    refined.reserve(onsetSamples.size());
    for (const auto onsetSample : onsetSamples)
    {
        const int center = juce::jlimit(1, totalSamples - 2, onsetSample);
        const int searchStart = juce::jmax(1, center - searchBehind);
        const int searchEnd = juce::jmin(totalSamples - 2, center + searchAhead);
        if (searchEnd <= searchStart)
        {
            refined.push_back(center);
            continue;
        }

        std::vector<double> scores(static_cast<size_t>(searchEnd - searchStart + 1), 0.0);
        int bestIndex = center;
        double bestScore = -std::numeric_limits<double>::infinity();

        for (int sampleIndex = searchStart; sampleIndex <= searchEnd; ++sampleIndex)
        {
            const double levelBefore = amplitudeEnvelope[static_cast<size_t>(juce::jmax(0, sampleIndex - 1))];
            const double levelNow = amplitudeEnvelope[static_cast<size_t>(sampleIndex)];
            const double levelAfter = amplitudeEnvelope[static_cast<size_t>(juce::jmin(totalSamples - 1, sampleIndex + 1))];
            const double levelRise = juce::jmax(0.0, levelNow - levelBefore)
                + (0.75 * juce::jmax(0.0, levelAfter - levelNow));
            const double macroRise = juce::jmax(0.0,
                                                meanAbsoluteLevelInRange(absolutePrefixSum, sampleIndex, sampleIndex + lookAhead)
                                                    - meanAbsoluteLevelInRange(absolutePrefixSum, sampleIndex - lookBehind, sampleIndex));
            const double absPrev = std::abs(monoSamples[static_cast<size_t>(sampleIndex - 1)]);
            const double absNow = std::abs(monoSamples[static_cast<size_t>(sampleIndex)]);
            const double absNext = std::abs(monoSamples[static_cast<size_t>(sampleIndex + 1)]);
            const double attackStep = juce::jmax(0.0, absNow - absPrev)
                + (0.8 * juce::jmax(0.0, absNext - absNow));
            const double slopeMagnitude = std::abs(monoSamples[static_cast<size_t>(sampleIndex + 1)]
                                                   - monoSamples[static_cast<size_t>(sampleIndex - 1)]);
            const int delta = sampleIndex - center;
            const double proximityPenalty = delta > 0
                ? 0.0011 * static_cast<double>(delta)
                : 0.00018 * static_cast<double>(-delta);
            const double score = (levelRise * 18.0)
                + (macroRise * 8.0)
                + (attackStep * 3.4)
                + (slopeMagnitude * 1.25)
                - proximityPenalty;
            scores[static_cast<size_t>(sampleIndex - searchStart)] = score;

            if (score > bestScore)
            {
                bestScore = score;
                bestIndex = sampleIndex;
            }
        }

        int refinedIndex = bestIndex;
        if (bestScore > 0.0)
        {
            const int anchorSearchStart = juce::jmax(searchStart, bestIndex - juce::jmax(12, lookBehind * 6));
            const int anchorSearchEnd = juce::jmin(searchEnd, bestIndex + juce::jmax(8, lookAhead * 3));
            int localPeakIndex = bestIndex;
            float localPeakLevel = amplitudeEnvelope[static_cast<size_t>(bestIndex)];
            for (int sampleIndex = bestIndex; sampleIndex <= anchorSearchEnd; ++sampleIndex)
            {
                const float level = amplitudeEnvelope[static_cast<size_t>(sampleIndex)];
                if (level > localPeakLevel)
                {
                    localPeakLevel = level;
                    localPeakIndex = sampleIndex;
                }
            }

            const double localNoiseFloor = meanAbsoluteLevelInRange(absolutePrefixSum,
                                                                    anchorSearchStart - juce::jmax(8, lookBehind * 5),
                                                                    anchorSearchStart + 1);
            const double triggerLevel = juce::jmax(localNoiseFloor + ((static_cast<double>(localPeakLevel) - localNoiseFloor)
                                                                      * static_cast<double>(juce::jmap(snap01, 0.20f, 0.06f))),
                                                   localNoiseFloor * static_cast<double>(juce::jmap(snap01, 1.70f, 1.10f)));

            for (int sampleIndex = anchorSearchStart + 1; sampleIndex <= localPeakIndex; ++sampleIndex)
            {
                const double previousAbs = amplitudeEnvelope[static_cast<size_t>(sampleIndex - 1)];
                const double currentAbs = amplitudeEnvelope[static_cast<size_t>(sampleIndex)];
                const double nextAbs = amplitudeEnvelope[static_cast<size_t>(juce::jmin(totalSamples - 1, sampleIndex + 1))];
                const double currentScore = scores[static_cast<size_t>(sampleIndex - searchStart)];
                const bool thresholdCrossed = previousAbs < triggerLevel && currentAbs >= triggerLevel;
                const bool strongAttackPoint = currentScore >= (bestScore * static_cast<double>(juce::jmap(snap01, 0.44f, 0.62f)))
                    && currentAbs >= previousAbs
                    && nextAbs >= currentAbs * static_cast<double>(juce::jmap(snap01, 0.92f, 0.985f));

                if (thresholdCrossed && strongAttackPoint)
                {
                    refinedIndex = sampleIndex;
                    break;
                }
            }
        }

        refined.push_back(refinedIndex);
    }

    std::sort(refined.begin(), refined.end());
    refined.erase(std::unique(refined.begin(), refined.end()), refined.end());
    return refined;
}
} // namespace

void EnhancedAudioStrip::refreshTransientSliceMap()
{
    juce::ScopedLock lock(bufferLock);
    transientSliceMapDirty = true;
    if (sampleBuffer.getNumSamples() > 0)
        rebuildTransientSliceMap();
}

void EnhancedAudioStrip::markTransientSliceMapDirty()
{
    juce::ScopedLock lock(bufferLock);
    transientSliceMapDirty = true;
}

int EnhancedAudioStrip::getTransientSliceMinimumSpacingSamples(int totalSamples) const
{
    const double analysisRate = (sourceSampleRate > 1000.0)
        ? sourceSampleRate
        : juce::jmax(1.0, currentSampleRate);
    const int averageSliceSpacing = juce::jmax(1, totalSamples / juce::jmax(1, ModernAudioEngine::MaxColumns));
    const int timeFloor = juce::jmax(16, static_cast<int>(std::round(analysisRate * 0.008)));
    const int baseSpacing = juce::jmin(juce::jmax(1, averageSliceSpacing / 2), juce::jmax(timeFloor, averageSliceSpacing / 5));
    return juce::jlimit(1, juce::jmax(1, totalSamples - 1),
                        static_cast<int>(std::round(static_cast<float>(baseSpacing)
                                                    * TransientDetectionConfig::configuredTransientSpacingScale())));
}

void EnhancedAudioStrip::rebuildTransientSliceMap()
{
    for (int i = 0; i < ModernAudioEngine::MaxColumns; ++i)
        transientSliceSamples[static_cast<size_t>(i)] = 0;

    if (sampleBuffer.getNumSamples() <= 0)
        return;

    const int totalSamples = sampleBuffer.getNumSamples();
    const int channels = juce::jmax(1, sampleBuffer.getNumChannels());

    auto fillUniform = [this, totalSamples]()
    {
        for (int i = 0; i < ModernAudioEngine::MaxColumns; ++i)
            transientSliceSamples[static_cast<size_t>(i)] = juce::jlimit(0, totalSamples - 1,
                                                                         (i * totalSamples) / ModernAudioEngine::MaxColumns);
        transientSliceMapDirty = false;
        rebuildSampleAnalysisCacheLocked();
    };

    int fftOrder = 8;
    while ((1 << fftOrder) < juce::jmin(2048, totalSamples) && fftOrder < 12)
        ++fftOrder;
    const int frameSize = 1 << fftOrder;
    const int hop = juce::jmax(32, frameSize / 8);
    const int frames = juce::jmax(1, 1 + ((totalSamples - frameSize) / hop));

    if (frames < 4)
    {
        fillUniform();
        return;
    }

    juce::dsp::FFT fft(fftOrder);
    juce::dsp::WindowingFunction<float> window(static_cast<size_t>(frameSize),
                                               juce::dsp::WindowingFunction<float>::hann,
                                               true);

    const int halfBins = frameSize / 2;
    std::vector<float> fftData(static_cast<size_t>(2 * frameSize), 0.0f);
    std::vector<float> prevMag(static_cast<size_t>(halfBins), 0.0f);
    std::vector<float> spectralFlux(static_cast<size_t>(frames), 0.0f);
    std::vector<float> highFrequencyContent(static_cast<size_t>(frames), 0.0f);
    std::vector<float> frameEnergy(static_cast<size_t>(frames), 0.0f);
    std::vector<float> monoSamples(static_cast<size_t>(totalSamples), 0.0f);

    for (int sampleIndex = 0; sampleIndex < totalSamples; ++sampleIndex)
    {
        float mono = 0.0f;
        for (int ch = 0; ch < channels; ++ch)
            mono += sampleBuffer.getSample(ch, sampleIndex);
        monoSamples[static_cast<size_t>(sampleIndex)] = mono / static_cast<float>(channels);
    }

    for (int frame = 0; frame < frames; ++frame)
    {
        const int start = frame * hop;
        double energy = 0.0;

        for (int n = 0; n < frameSize; ++n)
        {
            const int sampleIndex = juce::jlimit(0, totalSamples - 1, start + n);
            const float mono = monoSamples[static_cast<size_t>(sampleIndex)];
            fftData[static_cast<size_t>(n)] = mono;
            energy += static_cast<double>(mono * mono);
        }

        for (int n = frameSize; n < 2 * frameSize; ++n)
            fftData[static_cast<size_t>(n)] = 0.0f;

        window.multiplyWithWindowingTable(fftData.data(), static_cast<size_t>(frameSize));
        fft.performFrequencyOnlyForwardTransform(fftData.data(), true);

        frameEnergy[static_cast<size_t>(frame)] = static_cast<float>(std::sqrt(energy / static_cast<double>(frameSize)));

        float flux = 0.0f;
        float hfc = 0.0f;
        for (int bin = 1; bin < halfBins; ++bin)
        {
            const float mag = fftData[static_cast<size_t>(bin)];
            const float diff = juce::jmax(0.0f, mag - prevMag[static_cast<size_t>(bin)]);
            const float weight = 1.0f + (2.0f * static_cast<float>(bin) / static_cast<float>(halfBins));
            flux += diff * weight;
            hfc += mag * weight * (1.0f + (3.0f * static_cast<float>(bin) / static_cast<float>(halfBins)));
            prevMag[static_cast<size_t>(bin)] = mag;
        }

        spectralFlux[static_cast<size_t>(frame)] = flux;
        highFrequencyContent[static_cast<size_t>(frame)] = hfc;
    }

    auto smoothFrameSeries = [frames](const std::vector<float>& source)
    {
        std::vector<float> smoothed(static_cast<size_t>(frames), 0.0f);
        for (int i = 0; i < frames; ++i)
        {
            const int a = juce::jmax(0, i - 1);
            const int b = juce::jmin(frames - 1, i + 1);
            float sum = 0.0f;
            for (int k = a; k <= b; ++k)
                sum += source[static_cast<size_t>(k)];
            smoothed[static_cast<size_t>(i)] = sum / static_cast<float>(b - a + 1);
        }
        return smoothed;
    };

    const std::vector<float> smoothedFlux = smoothFrameSeries(spectralFlux);
    const std::vector<float> smoothedHfc = smoothFrameSeries(highFrequencyContent);

    std::vector<float> energyDiff(static_cast<size_t>(frames), 0.0f);
    for (int i = 1; i < frames; ++i)
        energyDiff[static_cast<size_t>(i)] = juce::jmax(0.0f, frameEnergy[static_cast<size_t>(i)] - frameEnergy[static_cast<size_t>(i - 1)]);

    auto medianInWindow = [](const std::vector<float>& values, int start, int end)
    {
        std::vector<float> temp;
        temp.reserve(static_cast<size_t>(end - start + 1));
        for (int i = start; i <= end; ++i)
            temp.push_back(values[static_cast<size_t>(i)]);
        auto midIt = temp.begin() + static_cast<std::ptrdiff_t>(temp.size() / 2);
        std::nth_element(temp.begin(), midIt, temp.end());
        return *midIt;
    };

    auto buildAdaptiveNovelty = [&](const std::vector<float>& series,
                                    float adaptiveScale,
                                    float energyWeight)
    {
        std::vector<float> novelty(static_cast<size_t>(frames), 0.0f);
        for (int i = 0; i < frames; ++i)
        {
            const int a = juce::jmax(0, i - 8);
            const int b = juce::jmin(frames - 1, i + 8);
            const float adaptive = (medianInWindow(series, a, b) * adaptiveScale) + 1.0e-6f;
            const float peakPart = juce::jmax(0.0f, series[static_cast<size_t>(i)] - adaptive);
            novelty[static_cast<size_t>(i)] = peakPart + (energyWeight * energyDiff[static_cast<size_t>(i)]);
        }
        return novelty;
    };

    auto normalizeNovelty = [](const std::vector<float>& values)
    {
        std::vector<float> normalized(values.size(), 0.0f);
        if (values.empty())
            return normalized;

        const auto [minIt, maxIt] = std::minmax_element(values.begin(), values.end());
        const float minValue = *minIt;
        const float maxValue = *maxIt;
        const float range = juce::jmax(1.0e-6f, maxValue - minValue);
        for (size_t i = 0; i < values.size(); ++i)
            normalized[i] = juce::jlimit(0.0f, 1.0f, (values[i] - minValue) / range);
        return normalized;
    };

    const std::vector<float> specFluxNovelty = buildAdaptiveNovelty(smoothedFlux, 1.25f, 0.25f);
    const std::vector<float> hfcNovelty = buildAdaptiveNovelty(smoothedHfc, 1.18f, 0.18f);
    const std::vector<float> normalizedFluxNovelty = normalizeNovelty(specFluxNovelty);
    const std::vector<float> normalizedHfcNovelty = normalizeNovelty(hfcNovelty);
    const std::vector<float> normalizedEnergyDiff = normalizeNovelty(energyDiff);
    const float sensitivity01 = TransientDetectionConfig::configuredTransientSensitivity01();
    std::vector<float> hybridNovelty(static_cast<size_t>(frames), 0.0f);
    for (int i = 0; i < frames; ++i)
        hybridNovelty[static_cast<size_t>(i)] = (normalizedHfcNovelty[static_cast<size_t>(i)] * 0.58f)
            + (normalizedFluxNovelty[static_cast<size_t>(i)] * 0.42f)
            + (0.10f * normalizedEnergyDiff[static_cast<size_t>(i)]);

    const double analysisSampleRate = (sourceSampleRate > 1000.0)
        ? sourceSampleRate
        : juce::jmax(1.0, currentSampleRate);
    const int minPeakSpacingFrames = juce::jmax(1,
                                                static_cast<int>(((0.015 * analysisSampleRate) / static_cast<double>(hop))
                                                                 * static_cast<double>(TransientDetectionConfig::configuredTransientSpacingScale())));

    auto extractPeakFrames = [&](const std::vector<float>& noveltyCurve,
                                 float meanScale,
                                 float maxScale)
    {
        std::vector<std::pair<int, float>> peakFrames;
        peakFrames.reserve(static_cast<size_t>(frames));
        const float noveltySum = std::accumulate(noveltyCurve.begin(), noveltyCurve.end(), 0.0f);
        const float noveltyMean = noveltySum / static_cast<float>(juce::jmax(1, frames));
        const float noveltyMax = *std::max_element(noveltyCurve.begin(), noveltyCurve.end());
        const float minPeakLevel = juce::jmax(1.0e-6f,
                                              juce::jmax(noveltyMean * meanScale, noveltyMax * maxScale));

        for (int i = 1; i < (frames - 1); ++i)
        {
            const float center = noveltyCurve[static_cast<size_t>(i)];
            if (center < minPeakLevel)
                continue;
            if (center < noveltyCurve[static_cast<size_t>(i - 1)] || center < noveltyCurve[static_cast<size_t>(i + 1)])
                continue;

            if (!peakFrames.empty() && (i - peakFrames.back().first) < minPeakSpacingFrames)
            {
                if (center > peakFrames.back().second)
                    peakFrames.back() = { i, center };
                continue;
            }

            peakFrames.emplace_back(i, center);
        }

        return peakFrames;
    };

    std::vector<std::pair<int, float>> onsetFrames;
    switch (TransientDetectionConfig::configuredTransientOnsetMethod())
    {
        case TransientOnsetMethod::Hfc:
            onsetFrames = extractPeakFrames(hfcNovelty,
                                            juce::jmap(sensitivity01, 0.40f, 0.16f),
                                            juce::jmap(sensitivity01, 0.18f, 0.06f));
            break;
        case TransientOnsetMethod::SpecFlux:
            onsetFrames = extractPeakFrames(specFluxNovelty,
                                            juce::jmap(sensitivity01, 0.48f, 0.22f),
                                            juce::jmap(sensitivity01, 0.18f, 0.06f));
            break;
        case TransientOnsetMethod::Hybrid:
        default:
            onsetFrames = extractPeakFrames(hybridNovelty,
                                            juce::jmap(sensitivity01, 0.42f, 0.20f),
                                            juce::jmap(sensitivity01, 0.18f, 0.08f));
            break;
    }

    if (onsetFrames.empty() && TransientDetectionConfig::configuredTransientOnsetMethod() != TransientOnsetMethod::SpecFlux)
        onsetFrames = extractPeakFrames(specFluxNovelty,
                                        juce::jmap(sensitivity01, 0.48f, 0.22f),
                                        juce::jmap(sensitivity01, 0.18f, 0.06f));

    if (onsetFrames.empty() && TransientDetectionConfig::configuredTransientOnsetMethod() != TransientOnsetMethod::Hfc)
        onsetFrames = extractPeakFrames(hfcNovelty,
                                        juce::jmap(sensitivity01, 0.40f, 0.16f),
                                        juce::jmap(sensitivity01, 0.18f, 0.06f));

    if (onsetFrames.empty())
    {
        const float energyMax = *std::max_element(energyDiff.begin(), energyDiff.end());
        const float energyMinPeak = juce::jmax(1.0e-6f, energyMax * 0.18f);
        for (int i = 1; i < (frames - 1); ++i)
        {
            const float center = energyDiff[static_cast<size_t>(i)];
            if (center < energyMinPeak)
                continue;
            if (center < energyDiff[static_cast<size_t>(i - 1)] || center < energyDiff[static_cast<size_t>(i + 1)])
                continue;

            if (!onsetFrames.empty() && (i - onsetFrames.back().first) < minPeakSpacingFrames)
                continue;

            onsetFrames.emplace_back(i, center);
        }
    }

    if (static_cast<int>(onsetFrames.size()) > (ModernAudioEngine::MaxColumns - 1))
    {
        std::sort(onsetFrames.begin(), onsetFrames.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });
        onsetFrames.resize(ModernAudioEngine::MaxColumns - 1);
        std::sort(onsetFrames.begin(), onsetFrames.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
    }

    std::vector<int> onsetSamples;
    onsetSamples.reserve(onsetFrames.size());
    for (const auto& onset : onsetFrames)
    {
        const int centered = (onset.first * hop) + (frameSize / 2);
        onsetSamples.push_back(juce::jlimit(0, totalSamples - 1, centered));
    }

    std::sort(onsetSamples.begin(), onsetSamples.end());
    onsetSamples.erase(std::unique(onsetSamples.begin(), onsetSamples.end()), onsetSamples.end());
    if (const auto refined = refineOnsetSamplesToLeadingEdges(monoSamples, onsetSamples, frameSize, hop);
        !refined.empty())
    {
        onsetSamples = refined;
    }

    if (onsetSamples.empty())
    {
        fillUniform();
        return;
    }

    std::vector<int> positions;
    positions.reserve(ModernAudioEngine::MaxColumns);
    positions.push_back(0);
    for (const auto onsetSample : onsetSamples)
        positions.push_back(juce::jlimit(0, totalSamples - 1, onsetSample));

    std::sort(positions.begin(), positions.end());
    positions.erase(std::unique(positions.begin(), positions.end()), positions.end());

    if (static_cast<int>(positions.size()) < ModernAudioEngine::MaxColumns)
    {
        const double gridStep = static_cast<double>(juce::jmax(1, totalSamples - 1))
            / static_cast<double>(ModernAudioEngine::MaxColumns - 1);
        const int baseMinSpacing = juce::jmax(1, static_cast<int>(std::round(gridStep * 0.35)));
        std::vector<int> gridCandidates;
        gridCandidates.reserve(ModernAudioEngine::MaxColumns - 1);

        for (int i = 1; i < ModernAudioEngine::MaxColumns; ++i)
        {
            const int uniformPos = juce::jlimit(0, totalSamples - 1,
                                                static_cast<int>((static_cast<double>(i) / 15.0) * static_cast<double>(totalSamples - 1)));
            gridCandidates.push_back(uniformPos);
        }

        auto fillFromGridGaps = [&](int minSpacing)
        {
            while (static_cast<int>(positions.size()) < ModernAudioEngine::MaxColumns)
            {
                int bestCandidate = -1;
                int bestGapWidth = -1;
                int bestMidpointDistance = std::numeric_limits<int>::max();

                for (const int candidate : gridCandidates)
                {
                    auto upper = std::upper_bound(positions.begin(), positions.end(), candidate);
                    const int right = (upper != positions.end()) ? *upper : (totalSamples - 1);
                    const int left = (upper != positions.begin()) ? *std::prev(upper) : 0;

                    if (candidate <= left || candidate >= right)
                        continue;
                    if ((candidate - left) < minSpacing || (right - candidate) < minSpacing)
                        continue;

                    const int gapWidth = right - left;
                    const int midpoint = left + (gapWidth / 2);
                    const int midpointDistance = std::abs(candidate - midpoint);

                    if (gapWidth > bestGapWidth
                        || (gapWidth == bestGapWidth && midpointDistance < bestMidpointDistance))
                    {
                        bestCandidate = candidate;
                        bestGapWidth = gapWidth;
                        bestMidpointDistance = midpointDistance;
                    }
                }

                if (bestCandidate < 0)
                    break;

                positions.insert(std::upper_bound(positions.begin(), positions.end(), bestCandidate), bestCandidate);
            }
        };

        fillFromGridGaps(baseMinSpacing);

        if (static_cast<int>(positions.size()) < ModernAudioEngine::MaxColumns)
            fillFromGridGaps(juce::jmax(1, baseMinSpacing / 2));

        if (static_cast<int>(positions.size()) < ModernAudioEngine::MaxColumns)
        {
            for (const int candidate : gridCandidates)
            {
                if (static_cast<int>(positions.size()) >= ModernAudioEngine::MaxColumns)
                    break;
                if (!std::binary_search(positions.begin(), positions.end(), candidate))
                    positions.insert(std::upper_bound(positions.begin(), positions.end(), candidate), candidate);
            }
        }
    }

    while (static_cast<int>(positions.size()) < ModernAudioEngine::MaxColumns)
    {
        int bestInsert = -1;
        int bestGapWidth = -1;

        for (size_t i = 0; i < positions.size(); ++i)
        {
            const int left = positions[i];
            const int right = (i + 1 < positions.size()) ? positions[i + 1] : (totalSamples - 1);
            const int gapWidth = right - left;
            if (gapWidth <= 1)
                continue;

            const int candidate = left + (gapWidth / 2);
            if (candidate <= left || candidate >= right)
                continue;

            if (gapWidth > bestGapWidth)
            {
                bestGapWidth = gapWidth;
                bestInsert = candidate;
            }
        }

        if (bestInsert < 0)
            break;

        positions.insert(std::upper_bound(positions.begin(), positions.end(), bestInsert), bestInsert);
    }

    while (static_cast<int>(positions.size()) < ModernAudioEngine::MaxColumns)
    {
        const int last = positions.empty() ? 0 : positions.back();
        const int next = juce::jlimit(0, totalSamples - 1, last + 1);
        positions.push_back(next);
        if (next == last)
            break;
    }

    if (static_cast<int>(positions.size()) < ModernAudioEngine::MaxColumns)
        positions.resize(ModernAudioEngine::MaxColumns, positions.empty() ? 0 : positions.back());

    if (static_cast<int>(positions.size()) > ModernAudioEngine::MaxColumns)
        positions.resize(ModernAudioEngine::MaxColumns);

    for (int i = 0; i < ModernAudioEngine::MaxColumns; ++i)
        transientSliceSamples[static_cast<size_t>(i)] = positions[static_cast<size_t>(i)];

    transientSliceMapDirty = false;
    rebuildSampleAnalysisCacheLocked();
}

void ModernAudioEngine::setTransientDetectionConfig(int onsetMethodChoice,
                                                    int sensitivityChoice,
                                                    int snapChoice,
                                                    int spacingChoice)
{
    TransientDetectionConfig::setConfig(onsetMethodChoice, sensitivityChoice, snapChoice, spacingChoice);
}

void ModernAudioEngine::refreshTransientSliceMaps()
{
    for (auto& strip : strips)
    {
        if (strip == nullptr)
            continue;

        if (strip->isTransientSliceMode())
            strip->refreshTransientSliceMap();
        else
            strip->markTransientSliceMapDirty();
    }
}
