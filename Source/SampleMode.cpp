#include "SampleMode.h"
#include "WarpGrid.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>
#include <numeric>
#include <random>

#include <juce_dsp/juce_dsp.h>

#if MLRVST_ENABLE_ESSENTIA_NATIVE
 #include <essentia/algorithmfactory.h>
 #include <essentia/essentia.h>
 #include <essentia/essentiamath.h>
#endif
#if MLRVST_ENABLE_LIBPYIN
 #include "../third_party/LibPyin/source/libpyincpp.h"
#endif

namespace
{
constexpr double kInitialSliceTargetSeconds = 1.5;
constexpr int kMaxInitialSliceCount = 256;
constexpr int kDefaultSliceFadeSamples = 96;
constexpr int kMaxStoredTransientCount = 512;
constexpr float kMinViewZoom = 1.0f;
constexpr float kMaxViewZoom = 96.0f;
constexpr float kCueHitRadiusPixels = 8.0f;
constexpr float kWarpMarkerHitRadiusPixels = 4.0f;
constexpr float kDetailedWaveformZoomThreshold = 2.25f;
constexpr int kDetailedWaveformMaxVisibleSamples = 320000;
constexpr int kWarpMarkerLaneTopOffset = 6;
constexpr int kWarpMarkerLaneHeight = 10;
constexpr int kSliceMarkerHandleLaneGap = 2;
constexpr int kSliceMarkerHandleLaneHeight = 16;
constexpr double kWarpDisplayActivationEpsilon = 1.0e-5;
constexpr double kWarpMarkerGuideSnapSeconds = 0.02;
constexpr double kWarpMarkerTransientClusterSeconds = 0.006;
constexpr float kWarpMarkerFineDragScale = 0.08f;
constexpr int kWarpAutoMarkerMaxInsertions = 12;
constexpr double kWarpMarkerMinStretchRatio = 0.10001;
constexpr double kWarpMarkerMaxStretchRatio = 19.9999;
constexpr int64_t kWarpMarkerDuplicateSampleTolerance = 1;
constexpr size_t kMaxSliceEditUndoDepth = 64;
constexpr float kKeyLockCacheRateTolerance = 0.0005f;
constexpr float kKeyLockCachePitchTolerance = 0.01f;

using WarpAnchorPoint = WarpGrid::Anchor;

struct SampleWarpDisplayMap
{
    bool active = false;
    int64_t totalSamples = 0;
    double startBeatPosition = 0.0;
    double endBeatPosition = 0.0;
    std::vector<int64_t> beatTicks;
    std::vector<SampleWarpMarker> warpMarkers;
    std::vector<WarpAnchorPoint> anchors;
};

float computeWarpMarkerHitRadiusForZoom(float viewZoom)
{
    return juce::jlimit(2.0f,
                        kWarpMarkerHitRadiusPixels,
                        kWarpMarkerHitRadiusPixels - (std::log2(juce::jmax(1.0f, viewZoom)) * 0.35f));
}

juce::String buildSliceLabel(int index);
float safeNormalizedPosition(int64_t samplePosition, int64_t totalSamples);
void fillSliceMetadata(SampleSlice& slice, int sliceIndex, int64_t totalSamples);
juce::AudioBuffer<float> buildMonoBuffer(const LoadedSampleData& sampleData);
int pitchHzToMidi(double pitchHz);

std::array<SampleSlice, SliceModel::VisibleSliceCount> buildDistributedRandomSliceSelection(
    const std::vector<SampleSlice>& allSlices,
    int64_t totalSamples,
    std::mt19937& rng)
{
    std::array<SampleSlice, SliceModel::VisibleSliceCount> selection {};
    if (allSlices.empty())
        return selection;

    const int selectionCount = juce::jmin(SliceModel::VisibleSliceCount, static_cast<int>(allSlices.size()));
    if (selectionCount <= 0)
        return selection;

    const int64_t safeTotalSamples = juce::jmax<int64_t>(
        1,
        totalSamples > 0 ? totalSamples : juce::jmax<int64_t>(1, allSlices.back().endSample));
    const auto sliceCount = static_cast<int>(allSlices.size());
    const int minIndexGap = juce::jmax(1, sliceCount / juce::jmax(1, selectionCount * 3));
    std::vector<int> chosenIndices;
    chosenIndices.reserve(static_cast<size_t>(selectionCount));
    std::vector<bool> used(allSlices.size(), false);

    auto respectsGap = [&](int candidateIndex) -> bool
    {
        for (const int chosenIndex : chosenIndices)
        {
            if (std::abs(candidateIndex - chosenIndex) < minIndexGap)
                return false;
        }
        return true;
    };

    std::uniform_real_distribution<double> bucketJitter(0.15, 0.85);

    for (int slot = 0; slot < selectionCount; ++slot)
    {
        const double bucketStartNorm = static_cast<double>(slot) / static_cast<double>(selectionCount);
        const double bucketEndNorm = static_cast<double>(slot + 1) / static_cast<double>(selectionCount);
        const int64_t bucketStartSample = static_cast<int64_t>(std::floor(bucketStartNorm * safeTotalSamples));
        const int64_t bucketEndSample = static_cast<int64_t>(std::ceil(bucketEndNorm * safeTotalSamples));
        const int64_t targetSample = static_cast<int64_t>(
            std::floor(((static_cast<double>(slot) + bucketJitter(rng))
                         / static_cast<double>(selectionCount))
                        * safeTotalSamples));

        std::vector<int> bucketCandidates;
        bucketCandidates.reserve(allSlices.size());
        for (int i = 0; i < static_cast<int>(allSlices.size()); ++i)
        {
            if (used[static_cast<size_t>(i)])
                continue;

            const auto& slice = allSlices[static_cast<size_t>(i)];
            if (slice.id < 0 || slice.endSample <= slice.startSample)
                continue;

            if (slice.startSample >= bucketStartSample
                && slice.startSample < bucketEndSample
                && respectsGap(i))
            {
                bucketCandidates.push_back(i);
            }
        }

        int chosenIndex = -1;
        if (!bucketCandidates.empty())
        {
            std::uniform_int_distribution<int> pick(0, static_cast<int>(bucketCandidates.size()) - 1);
            chosenIndex = bucketCandidates[static_cast<size_t>(pick(rng))];
        }
        else
        {
            int64_t bestDistance = std::numeric_limits<int64_t>::max();
            for (int pass = 0; pass < 2 && chosenIndex < 0; ++pass)
            {
                const bool enforceGap = (pass == 0);
                for (int i = 0; i < static_cast<int>(allSlices.size()); ++i)
                {
                    if (used[static_cast<size_t>(i)])
                        continue;
                    if (enforceGap && !respectsGap(i))
                        continue;

                    const auto& slice = allSlices[static_cast<size_t>(i)];
                    if (slice.id < 0 || slice.endSample <= slice.startSample)
                        continue;

                    const int64_t distance = std::abs(slice.startSample - targetSample);
                    if (distance < bestDistance)
                    {
                        bestDistance = distance;
                        chosenIndex = i;
                    }
                }
            }
        }

        if (chosenIndex < 0 || chosenIndex >= static_cast<int>(allSlices.size()))
            continue;

        used[static_cast<size_t>(chosenIndex)] = true;
        chosenIndices.push_back(chosenIndex);
        selection[static_cast<size_t>(slot)] = allSlices[static_cast<size_t>(chosenIndex)];
    }

    return selection;
}

std::array<SampleSlice, SliceModel::VisibleSliceCount> buildSequentialVisibleSliceSelection(
    const std::vector<SampleSlice>& allSlices,
    int startIndex)
{
    std::array<SampleSlice, SliceModel::VisibleSliceCount> selection {};
    if (allSlices.empty())
        return selection;

    const int selectionCount = juce::jmin(SliceModel::VisibleSliceCount, static_cast<int>(allSlices.size()));
    if (selectionCount <= 0)
        return selection;

    const int maxStartIndex = juce::jmax(0, static_cast<int>(allSlices.size()) - selectionCount);
    const int clampedStartIndex = juce::jlimit(0, maxStartIndex, startIndex);
    for (int slot = 0; slot < selectionCount; ++slot)
        selection[static_cast<size_t>(slot)] = allSlices[static_cast<size_t>(clampedStartIndex + slot)];

    return selection;
}

const char* sampleSliceModeName(SampleSliceMode mode)
{
    switch (mode)
    {
        case SampleSliceMode::Uniform: return "uniform";
        case SampleSliceMode::Transient: return "transient";
        case SampleSliceMode::Beat: return "beat";
        case SampleSliceMode::Manual: return "manual";
    }

    return "uniform";
}

SampleSliceMode sampleSliceModeFromString(const juce::String& text)
{
    const auto normalized = text.trim().toLowerCase();
    if (normalized == "transient")
        return SampleSliceMode::Transient;
    if (normalized == "beat")
        return SampleSliceMode::Beat;
    if (normalized == "manual")
        return SampleSliceMode::Manual;
    return SampleSliceMode::Transient;
}

const char* sampleTriggerModeName(SampleTriggerMode mode)
{
    switch (mode)
    {
        case SampleTriggerMode::OneShot: return "oneshot";
        case SampleTriggerMode::Loop: return "loop";
    }

    return "oneshot";
}

SampleTriggerMode sampleTriggerModeFromString(const juce::String& text)
{
    return text.trim().equalsIgnoreCase("loop")
        ? SampleTriggerMode::Loop
        : SampleTriggerMode::OneShot;
}

int pitchClassFromKeyName(const juce::String& keyName)
{
    const auto normalized = keyName.trim().toUpperCase();
    if (normalized.isEmpty())
        return -1;

    if (normalized == "C")  return 0;
    if (normalized == "B#" ) return 0;
    if (normalized == "C#" || normalized == "DB") return 1;
    if (normalized == "D")  return 2;
    if (normalized == "D#" || normalized == "EB") return 3;
    if (normalized == "E" || normalized == "FB") return 4;
    if (normalized == "F" || normalized == "E#") return 5;
    if (normalized == "F#" || normalized == "GB") return 6;
    if (normalized == "G")  return 7;
    if (normalized == "G#" || normalized == "AB") return 8;
    if (normalized == "A")  return 9;
    if (normalized == "A#" || normalized == "BB") return 10;
    if (normalized == "B" || normalized == "CB") return 11;
    return -1;
}

int pitchScaleIndexFromEssentiaName(const juce::String& scaleName)
{
    const auto normalized = scaleName.trim().toLowerCase();
    if (normalized == "major")
        return 1;
    if (normalized == "minor")
        return 2;
    return -1;
}

double midiToPitchHz(int midiNote)
{
    return 440.0 * std::pow(2.0, (static_cast<double>(midiNote) - 69.0) / 12.0);
}

juce::String formatDetectedKeyLabel(int midiNote, int scaleIndex)
{
    if (midiNote < 0)
        return "--";

    static constexpr std::array<const char*, 12> noteNames
    {
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
    };

    const int pitchClass = ((midiNote % 12) + 12) % 12;
    const auto noteName = juce::String(noteNames[static_cast<size_t>(pitchClass)]);
    if (scaleIndex == 1)
        return noteName + " major";
    if (scaleIndex == 2)
        return noteName + " minor";
    return noteName;
}

#if MLRVST_ENABLE_LIBPYIN
struct LibPyinPitchEstimate
{
    double frequency = 0.0;
    int midi = -1;
    float confidence = 0.0f;
};

std::optional<LibPyinPitchEstimate> runLibPyinMonophonicPitch(const LoadedSampleData& sampleData)
{
    const auto monoBuffer = buildMonoBuffer(sampleData);
    const int totalSamples = monoBuffer.getNumSamples();
    if (totalSamples <= 0 || sampleData.sourceSampleRate <= 0.0)
        return std::nullopt;

    int blockSize = 4096;
    while (blockSize > totalSamples && blockSize > 1024)
        blockSize /= 2;
    const int stepSize = juce::jmax(64, blockSize / 16);
    if (blockSize < 1024 || stepSize <= 0)
        return std::nullopt;

    std::vector<float> monoSamples(static_cast<size_t>(totalSamples));
    const auto* mono = monoBuffer.getReadPointer(0);
    float peak = 0.0f;
    for (int i = 0; i < totalSamples; ++i)
        peak = juce::jmax(peak, std::abs(mono[i]));
    if (peak < 1.0e-5f)
        return std::nullopt;

    const float normalizeGain = 1.0f / peak;
    for (int i = 0; i < totalSamples; ++i)
        monoSamples[static_cast<size_t>(i)] = mono[i] * normalizeGain;

    PyinCpp pyin(static_cast<int>(std::lround(sampleData.sourceSampleRate)), blockSize, stepSize);
    pyin.setCutOff(0.0f);
    pyin.reserve(totalSamples);
    pyin.feed(monoSamples);

    const auto& candidateFrames = pyin.getPitchCandidates();
    if (candidateFrames.empty())
        return std::nullopt;

    struct FrameEstimate
    {
        double frequency = 0.0;
        double probability = 0.0;
        double rms = 0.0;
        int midi = -1;
        bool valid = false;
    };

    std::vector<FrameEstimate> frames(candidateFrames.size());
    std::vector<double> rmsValues;
    rmsValues.reserve(candidateFrames.size());
    std::array<double, 128> midiWeights {};

    for (size_t frameIndex = 0; frameIndex < candidateFrames.size(); ++frameIndex)
    {
        const int startSample = static_cast<int>(frameIndex) * stepSize;
        const int endSample = juce::jmin(totalSamples, startSample + blockSize);
        double sumSquares = 0.0;
        for (int sample = startSample; sample < endSample; ++sample)
            sumSquares += static_cast<double>(monoSamples[static_cast<size_t>(sample)])
                * static_cast<double>(monoSamples[static_cast<size_t>(sample)]);

        const int frameLength = juce::jmax(1, endSample - startSample);
        FrameEstimate estimate;
        estimate.rms = std::sqrt(sumSquares / static_cast<double>(frameLength));
        rmsValues.push_back(estimate.rms);

        double bestProbability = 0.0;
        double bestFrequency = 0.0;
        int bestMidi = -1;
        for (const auto& candidate : candidateFrames[frameIndex])
        {
            const double frequency = static_cast<double>(candidate.first);
            const double probability = static_cast<double>(candidate.second);
            const int midi = pitchHzToMidi(frequency);
            if (!(frequency > 20.0) || midi < 0 || midi > 127 || probability < 0.08)
                continue;
            if (probability > bestProbability)
            {
                bestProbability = probability;
                bestFrequency = frequency;
                bestMidi = midi;
            }
        }

        if (bestMidi >= 0)
        {
            estimate.frequency = bestFrequency;
            estimate.probability = juce::jlimit(0.0, 1.0, bestProbability);
            estimate.midi = bestMidi;
            estimate.valid = true;
        }

        frames[frameIndex] = estimate;
    }

    if (rmsValues.empty())
        return std::nullopt;

    auto sortedRms = rmsValues;
    std::sort(sortedRms.begin(), sortedRms.end());
    const double rmsMedian = sortedRms[sortedRms.size() / 2];
    const double rmsHigh = sortedRms[static_cast<size_t>(juce::jlimit<int>(0,
                                                                           static_cast<int>(sortedRms.size() - 1),
                                                                           static_cast<int>(std::floor(sortedRms.size() * 0.85))))];
    const double rmsFloor = juce::jmax(0.0025, juce::jmax(rmsMedian * 0.45, rmsHigh * 0.18));

    for (const auto& frame : frames)
    {
        if (!frame.valid || frame.rms < rmsFloor)
            continue;
        const double weight = frame.probability * (0.35 + juce::jlimit(0.0, 1.0, frame.rms / juce::jmax(1.0e-6, rmsHigh)));
        midiWeights[static_cast<size_t>(frame.midi)] += weight;
    }

    const auto bestMidiIt = std::max_element(midiWeights.begin(), midiWeights.end());
    if (bestMidiIt == midiWeights.end() || *bestMidiIt <= 0.0)
        return std::nullopt;

    const int bestMidi = static_cast<int>(std::distance(midiWeights.begin(), bestMidiIt));

    struct Segment
    {
        size_t start = 0;
        size_t end = 0;
        double score = 0.0;
    };

    auto frameMatchesBestNote = [&](const FrameEstimate& frame) -> bool
    {
        if (!frame.valid || frame.rms < rmsFloor || frame.probability < 0.12)
            return false;
        return std::abs(frame.midi - bestMidi) <= 1;
    };

    Segment bestSegment {};
    bool haveSegment = false;
    size_t segmentStart = 0;
    size_t segmentEnd = 0;
    int gapFrames = 0;
    double lastFrequency = 0.0;
    double segmentWeightSum = 0.0;
    double segmentScoreAccum = 0.0;

    auto finishSegment = [&](size_t endExclusive)
    {
        if (segmentEnd < segmentStart)
            return;
        const size_t length = segmentEnd - segmentStart + 1;
        if (length < 2 || segmentWeightSum <= 0.0)
            return;
        const double stabilityScore = segmentScoreAccum / segmentWeightSum;
        const double score = static_cast<double>(length) * stabilityScore;
        if (!haveSegment || score > bestSegment.score)
        {
            bestSegment = { segmentStart, endExclusive, score };
            haveSegment = true;
        }
    };

    for (size_t frameIndex = 0; frameIndex < frames.size(); ++frameIndex)
    {
        const auto& frame = frames[frameIndex];
        if (!frameMatchesBestNote(frame))
        {
            ++gapFrames;
            if (gapFrames > 1 && segmentEnd >= segmentStart)
            {
                finishSegment(segmentEnd + 1);
                segmentStart = frameIndex + 1;
                segmentEnd = frameIndex;
                gapFrames = 0;
                lastFrequency = 0.0;
                segmentWeightSum = 0.0;
                segmentScoreAccum = 0.0;
            }
            continue;
        }

        const double weight = frame.probability
            * (0.35 + juce::jlimit(0.0, 1.0, frame.rms / juce::jmax(1.0e-6, rmsHigh)));
        const double jumpSemitones = (lastFrequency > 0.0 && frame.frequency > 0.0)
            ? std::abs(12.0 * std::log2(frame.frequency / lastFrequency))
            : 0.0;

        if (segmentEnd >= segmentStart && jumpSemitones > 0.85)
        {
            finishSegment(segmentEnd + 1);
            segmentStart = frameIndex;
            segmentEnd = frameIndex - 1;
            gapFrames = 0;
            lastFrequency = 0.0;
            segmentWeightSum = 0.0;
            segmentScoreAccum = 0.0;
        }

        if (segmentEnd < segmentStart)
            segmentStart = frameIndex;

        segmentEnd = frameIndex;
        gapFrames = 0;
        lastFrequency = frame.frequency;
        segmentWeightSum += weight;
        segmentScoreAccum += weight / (1.0 + (jumpSemitones * 0.75));
    }

    finishSegment(segmentEnd + 1);

    double weightedLogPitch = 0.0;
    double totalWeight = 0.0;
    auto accumulateFrame = [&](const FrameEstimate& frame)
    {
        if (!frameMatchesBestNote(frame))
            return;
        const double weight = frame.probability
            * (0.35 + juce::jlimit(0.0, 1.0, frame.rms / juce::jmax(1.0e-6, rmsHigh)));
        weightedLogPitch += std::log(frame.frequency) * weight;
        totalWeight += weight;
    };

    if (haveSegment)
    {
        for (size_t frameIndex = bestSegment.start; frameIndex < bestSegment.end && frameIndex < frames.size(); ++frameIndex)
            accumulateFrame(frames[frameIndex]);
    }

    if (totalWeight <= 0.0)
    {
        for (const auto& frame : frames)
            accumulateFrame(frame);
    }

    if (totalWeight <= 0.0)
        return std::nullopt;

    const double frequency = std::exp(weightedLogPitch / totalWeight);
    const double dominantWeight = *bestMidiIt;
    double totalMidiWeight = 0.0;
    for (const auto weight : midiWeights)
        totalMidiWeight += weight;
    const double dominance = (totalMidiWeight > 0.0) ? (dominantWeight / totalMidiWeight) : 0.0;
    const double segmentConfidence = haveSegment
        ? juce::jlimit(0.0, 1.0, bestSegment.score / juce::jmax(1.0, static_cast<double>(bestSegment.end - bestSegment.start)))
        : 0.0;
    const float confidence = juce::jlimit(0.0f,
                                          1.0f,
                                          static_cast<float>((dominance * 0.65) + (segmentConfidence * 0.35)));
    return LibPyinPitchEstimate { frequency, pitchHzToMidi(frequency), confidence };
}
#endif

juce::String compactLoadStatusText(const juce::String& statusText)
{
    const auto source = statusText.trim();
    if (source.isEmpty())
        return {};

    if (source.startsWithIgnoreCase("Loading "))
        return "Loading...";
    if (source.containsIgnoreCase("Decoding"))
        return "Decoding...";
    if (source.containsIgnoreCase("Building waveform"))
        return "Waveform...";
    if (source.containsIgnoreCase("Analyzing transients"))
        return "Transients...";
    if (source.containsIgnoreCase("Essentia tempo / pitch")
        || source.containsIgnoreCase("Essentia key")
        || source.containsIgnoreCase("Essentia pitch"))
        return "Essentia...";
    if (source.containsIgnoreCase("Snapping loop-style transients"))
        return "Snapping...";
    if (source.containsIgnoreCase("Internal analysis fallback"))
        return "Internal fallback...";
    if (source.containsIgnoreCase("Finalizing analysis"))
        return "Finalizing...";

    if (source.length() > 28)
        return source.substring(0, 28).trimEnd() + "...";

    return source;
}

float safeNormalizedPosition(int64_t samplePosition, int64_t totalSamples)
{
    if (totalSamples <= 0)
        return 0.0f;

    return juce::jlimit(0.0f,
                        1.0f,
                        static_cast<float>(samplePosition / static_cast<double>(juce::jmax<int64_t>(1, totalSamples))));
}

std::vector<int64_t> sanitizeWarpBeatTicks(const std::vector<int64_t>& beatTickSamples,
                                           int64_t totalSamples)
{
    return WarpGrid::sanitizeBeatTicks(beatTickSamples, totalSamples);
}

double computeWarpBeatPositionFromSample(const std::vector<int64_t>& beatTicks,
                                         int64_t samplePosition)
{
    return WarpGrid::computeBeatPositionFromSample(beatTicks, samplePosition);
}

std::vector<SampleWarpMarker> sanitizePersistentWarpMarkers(const std::vector<SampleWarpMarker>& warpMarkers,
                                                            int64_t totalSamples)
{
    return WarpGrid::sanitizeMarkers(warpMarkers, totalSamples);
}

std::vector<WarpAnchorPoint> buildWarpAnchorPoints(const std::vector<int64_t>& beatTicks,
                                                   const std::vector<SampleWarpMarker>& warpMarkers,
                                                   int64_t totalSamples)
{
    return WarpGrid::buildAnchors(beatTicks, warpMarkers, totalSamples);
}

int64_t clampWarpMarkerSamplePositionByStretchRatio(const std::vector<int64_t>& beatTicks,
                                                    const std::vector<SampleWarpMarker>& warpMarkers,
                                                    int markerIndex,
                                                    int64_t samplePosition,
                                                    int64_t totalSamples)
{
    if (beatTicks.size() < 2
        || markerIndex < 0
        || markerIndex >= static_cast<int>(warpMarkers.size())
        || totalSamples <= 1)
    {
        return samplePosition;
    }

    const auto& marker = warpMarkers[static_cast<size_t>(markerIndex)];
    const int64_t endSample = juce::jmax<int64_t>(1, totalSamples - 1);

    const int64_t leftSample = (markerIndex > 0)
        ? warpMarkers[static_cast<size_t>(markerIndex - 1)].samplePosition
        : 0;
    const int64_t rightSample = (markerIndex + 1 < static_cast<int>(warpMarkers.size()))
        ? warpMarkers[static_cast<size_t>(markerIndex + 1)].samplePosition
        : endSample;

    const double leftBaseBeat = computeWarpBeatPositionFromSample(beatTicks, leftSample);
    const double rightBaseBeat = computeWarpBeatPositionFromSample(beatTicks, rightSample);
    const double leftTargetBeat = (markerIndex > 0)
        ? warpMarkers[static_cast<size_t>(markerIndex - 1)].beatPosition
        : leftBaseBeat;
    const double rightTargetBeat = (markerIndex + 1 < static_cast<int>(warpMarkers.size()))
        ? warpMarkers[static_cast<size_t>(markerIndex + 1)].beatPosition
        : rightBaseBeat;

    double minAllowedBaseBeat = -std::numeric_limits<double>::infinity();
    double maxAllowedBaseBeat = std::numeric_limits<double>::infinity();

    const double leftTargetSpan = marker.beatPosition - leftTargetBeat;
    if (leftTargetSpan > WarpGrid::kMarkerOrderEpsilon)
    {
        minAllowedBaseBeat = juce::jmax(minAllowedBaseBeat,
                                        leftBaseBeat + (leftTargetSpan / kWarpMarkerMaxStretchRatio));
        maxAllowedBaseBeat = juce::jmin(maxAllowedBaseBeat,
                                        leftBaseBeat + (leftTargetSpan / kWarpMarkerMinStretchRatio));
    }

    const double rightTargetSpan = rightTargetBeat - marker.beatPosition;
    if (rightTargetSpan > WarpGrid::kMarkerOrderEpsilon)
    {
        minAllowedBaseBeat = juce::jmax(minAllowedBaseBeat,
                                        rightBaseBeat - (rightTargetSpan / kWarpMarkerMinStretchRatio));
        maxAllowedBaseBeat = juce::jmin(maxAllowedBaseBeat,
                                        rightBaseBeat - (rightTargetSpan / kWarpMarkerMaxStretchRatio));
    }

    if (!std::isfinite(minAllowedBaseBeat) || !std::isfinite(maxAllowedBaseBeat))
        return samplePosition;

    if (maxAllowedBaseBeat <= (minAllowedBaseBeat + WarpGrid::kMarkerOrderEpsilon))
        return samplePosition;

    const double proposedBaseBeat = computeWarpBeatPositionFromSample(beatTicks, samplePosition);
    const double clampedBaseBeat = juce::jlimit(minAllowedBaseBeat, maxAllowedBaseBeat, proposedBaseBeat);
    return WarpGrid::computeSamplePositionFromBeatPosition(beatTicks, clampedBaseBeat, totalSamples);
}

int64_t computeWarpGuideSnapToleranceSamples(const std::vector<int64_t>& beatTicks,
                                             int64_t samplePosition,
                                             double sampleRate)
{
    int64_t tolerance = juce::jmax<int64_t>(64,
        static_cast<int64_t>(std::llround(juce::jmax(1.0, sampleRate) * kWarpMarkerGuideSnapSeconds)));

    if (beatTicks.size() >= 2)
    {
        auto it = std::lower_bound(beatTicks.begin(), beatTicks.end(), samplePosition);
        int64_t interval = 0;
        if (it == beatTicks.begin())
        {
            interval = WarpGrid::computeEdgeTickInterval(beatTicks, false);
        }
        else if (it == beatTicks.end())
        {
            interval = WarpGrid::computeEdgeTickInterval(beatTicks, true);
        }
        else
        {
            const int64_t prev = *(it - 1);
            const int64_t next = *it;
            interval = juce::jmax<int64_t>(1, next - prev);
        }

        if (interval > 0)
            tolerance = juce::jlimit<int64_t>(64, 4096, juce::jmax<int64_t>(tolerance, interval / 10));
    }

    return tolerance;
}

std::vector<int64_t> clusterWarpMarkerTransientGuides(const std::vector<int64_t>& transientMarkers,
                                                      int64_t lowerBound,
                                                      int64_t upperBound,
                                                      double sampleRate)
{
    std::vector<int64_t> candidates;
    candidates.reserve(transientMarkers.size());

    for (const auto sample : transientMarkers)
    {
        if (sample >= lowerBound && sample <= upperBound)
            candidates.push_back(sample);
    }

    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
    if (candidates.empty())
        return candidates;

    const int64_t clusterTolerance = static_cast<int64_t>(juce::jmax(1.0,
        std::round(juce::jmax(1.0, sampleRate) * kWarpMarkerTransientClusterSeconds)));
    std::vector<int64_t> clustered;
    clustered.reserve(candidates.size());

    size_t clusterStart = 0;
    while (clusterStart < candidates.size())
    {
        size_t clusterEnd = clusterStart + 1;
        double clusterSum = static_cast<double>(candidates[clusterStart]);
        while (clusterEnd < candidates.size()
               && (candidates[clusterEnd] - candidates[clusterEnd - 1]) <= clusterTolerance)
        {
            clusterSum += static_cast<double>(candidates[clusterEnd]);
            ++clusterEnd;
        }

        const auto clusterCount = static_cast<double>(clusterEnd - clusterStart);
        const int64_t clusterCenter = static_cast<int64_t>(std::llround(clusterSum / juce::jmax(1.0, clusterCount)));
        clustered.push_back(juce::jlimit(lowerBound, upperBound, clusterCenter));
        clusterStart = clusterEnd;
    }

    std::sort(clustered.begin(), clustered.end());
    clustered.erase(std::unique(clustered.begin(), clustered.end()), clustered.end());
    return clustered;
}

int64_t snapWarpMarkerSampleToGuide(const std::vector<int64_t>& beatTicks,
                                    const std::vector<int64_t>& transientMarkers,
                                    int64_t samplePosition,
                                    int64_t lowerBound,
                                    int64_t upperBound,
                                    double sampleRate)
{
    int64_t snappedSample = juce::jlimit(lowerBound, upperBound, samplePosition);
    int64_t bestDistance = std::numeric_limits<int64_t>::max();
    const int64_t tolerance = computeWarpGuideSnapToleranceSamples(beatTicks, samplePosition, sampleRate);

    auto considerCandidate = [&](int64_t candidate)
    {
        if (candidate < lowerBound || candidate > upperBound)
            return;

        const int64_t distance = std::abs(candidate - samplePosition);
        if (distance <= tolerance && distance < bestDistance)
        {
            bestDistance = distance;
            snappedSample = candidate;
        }
    };

    for (const auto marker : transientMarkers)
        considerCandidate(marker);

    for (const auto tick : beatTicks)
        considerCandidate(tick);

    return snappedSample;
}

SampleWarpDisplayMap buildSampleWarpDisplayMap(const LoadedSampleData* loadedSample,
                                               const SampleModeEngine::StateSnapshot& snapshot)
{
    SampleWarpDisplayMap map;
    if (loadedSample == nullptr || loadedSample->sourceLengthSamples <= 1)
        return map;

    map.totalSamples = loadedSample->sourceLengthSamples;
    map.beatTicks = sanitizeWarpBeatTicks(loadedSample->analysis.beatTickSamples, map.totalSamples);
    if (map.beatTicks.size() < 2)
        return map;

    map.warpMarkers = sanitizePersistentWarpMarkers(snapshot.warpMarkers, map.totalSamples);
    if (map.warpMarkers.empty())
        return map;

    bool hasMeaningfulWarp = false;
    for (const auto& marker : map.warpMarkers)
    {
        const double baseBeatPosition = computeWarpBeatPositionFromSample(map.beatTicks, marker.samplePosition);
        if (std::abs(baseBeatPosition - marker.beatPosition) > kWarpDisplayActivationEpsilon)
        {
            hasMeaningfulWarp = true;
            break;
        }
    }
    if (!hasMeaningfulWarp)
        return map;

    map.anchors = buildWarpAnchorPoints(map.beatTicks, map.warpMarkers, map.totalSamples);
    if (map.anchors.size() < 3)
        return map;

    map.startBeatPosition = map.anchors.front().targetBeatPosition;
    map.endBeatPosition = map.anchors.back().targetBeatPosition;
    if ((map.endBeatPosition - map.startBeatPosition) <= WarpGrid::kBeatEpsilon)
        return map;

    map.active = true;
    return map;
}

float displayNormalizedPositionForSample(const SampleWarpDisplayMap& warpDisplayMap,
                                         int64_t samplePosition)
{
    if (warpDisplayMap.totalSamples <= 0)
        return 0.0f;

    if (!warpDisplayMap.active)
        return safeNormalizedPosition(samplePosition, warpDisplayMap.totalSamples);

    const double warpedBeatPosition = WarpGrid::computeWarpedBeatPositionForSample(warpDisplayMap.beatTicks,
                                                                                   warpDisplayMap.anchors,
                                                                                   samplePosition,
                                                                                   warpDisplayMap.totalSamples);
    const double beatSpan = warpDisplayMap.endBeatPosition - warpDisplayMap.startBeatPosition;
    if (!(beatSpan > WarpGrid::kBeatEpsilon))
        return safeNormalizedPosition(samplePosition, warpDisplayMap.totalSamples);

    return juce::jlimit(0.0f,
                        1.0f,
                        static_cast<float>((warpedBeatPosition - warpDisplayMap.startBeatPosition) / beatSpan));
}

int64_t samplePositionFromDisplayNormalized(const SampleWarpDisplayMap& warpDisplayMap,
                                            float normalizedPosition)
{
    if (warpDisplayMap.totalSamples <= 0)
        return 0;

    const float clampedNormalized = juce::jlimit(0.0f, 1.0f, normalizedPosition);
    if (!warpDisplayMap.active)
    {
        return juce::jlimit<int64_t>(0,
                                     warpDisplayMap.totalSamples - 1,
                                     static_cast<int64_t>(std::llround(clampedNormalized
                                                                       * static_cast<float>(warpDisplayMap.totalSamples - 1))));
    }

    const double targetBeatPosition = warpDisplayMap.startBeatPosition
        + (static_cast<double>(clampedNormalized)
           * (warpDisplayMap.endBeatPosition - warpDisplayMap.startBeatPosition));
    return WarpGrid::computeSamplePositionFromWarpedBeatPosition(warpDisplayMap.beatTicks,
                                                                 warpDisplayMap.anchors,
                                                                 targetBeatPosition,
                                                                 warpDisplayMap.totalSamples);
}

int findWarpMarkerIndexById(const std::vector<SampleWarpMarker>& warpMarkers, int markerId)
{
    for (size_t i = 0; i < warpMarkers.size(); ++i)
    {
        if (warpMarkers[i].id == markerId)
            return static_cast<int>(i);
    }

    return -1;
}

juce::AudioBuffer<float> buildMonoBuffer(const LoadedSampleData& sampleData)
{
    juce::AudioBuffer<float> monoBuffer(1, sampleData.audioBuffer.getNumSamples());
    monoBuffer.clear();

    const int sourceChannels = sampleData.audioBuffer.getNumChannels();
    const int numSamples = sampleData.audioBuffer.getNumSamples();
    if (sourceChannels <= 0 || numSamples <= 0)
        return monoBuffer;

    float* mono = monoBuffer.getWritePointer(0);
    if (sourceChannels == 1)
    {
        juce::FloatVectorOperations::copy(mono, sampleData.audioBuffer.getReadPointer(0), numSamples);
        return monoBuffer;
    }

    const float* left = sampleData.audioBuffer.getReadPointer(0);
    const float* right = sampleData.audioBuffer.getReadPointer(1);
    for (int i = 0; i < numSamples; ++i)
        mono[i] = 0.5f * (left[i] + right[i]);

    return monoBuffer;
}

double meanAbsoluteLevelInRange(const std::vector<double>& absolutePrefixSum, int startSample, int endSample)
{
    const int clampedStart = juce::jmax(0, startSample);
    const int clampedEnd = juce::jmax(clampedStart, juce::jmin(static_cast<int>(absolutePrefixSum.size()) - 1, endSample));
    const int sampleCount = juce::jmax(1, clampedEnd - clampedStart);
    return (absolutePrefixSum[static_cast<size_t>(clampedEnd)] - absolutePrefixSum[static_cast<size_t>(clampedStart)])
        / static_cast<double>(sampleCount);
}

std::vector<int64_t> refineOnsetSamplesToLeadingEdges(const juce::AudioBuffer<float>& monoBuffer,
                                                      const std::vector<int64_t>& onsetSamples,
                                                      int frameSize,
                                                      int hopSize)
{
    std::vector<int64_t> refined;
    const int totalSamples = monoBuffer.getNumSamples();
    if (totalSamples <= 2 || onsetSamples.empty())
        return refined;

    const float* mono = monoBuffer.getReadPointer(0);
    std::vector<double> absolutePrefixSum(static_cast<size_t>(totalSamples) + 1u, 0.0);
    for (int i = 0; i < totalSamples; ++i)
        absolutePrefixSum[static_cast<size_t>(i) + 1u] = absolutePrefixSum[static_cast<size_t>(i)] + std::abs(mono[i]);

    const int searchBehind = juce::jlimit(24, juce::jmax(24, totalSamples / 256), frameSize / 2);
    const int searchAhead = juce::jlimit(8, juce::jmax(8, totalSamples / 1024), juce::jmax(8, hopSize / 3));
    const int lookBehind = juce::jlimit(3, juce::jmax(3, totalSamples / 4096), juce::jmax(4, hopSize / 8));
    const int lookAhead = juce::jlimit(6, juce::jmax(6, totalSamples / 2048), juce::jmax(6, hopSize / 5));

    refined.reserve(onsetSamples.size());
    for (const auto onsetSample : onsetSamples)
    {
        const int center = juce::jlimit(1, totalSamples - 2, static_cast<int>(onsetSample));
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
            const double levelBefore = meanAbsoluteLevelInRange(absolutePrefixSum, sampleIndex - lookBehind, sampleIndex);
            const double levelAfter = meanAbsoluteLevelInRange(absolutePrefixSum, sampleIndex, sampleIndex + lookAhead);
            const double levelRise = juce::jmax(0.0, levelAfter - levelBefore);
            const double attackStep = std::abs(mono[sampleIndex] - mono[sampleIndex - 1])
                + (0.5 * std::abs(mono[sampleIndex + 1] - mono[sampleIndex]));
            const int delta = sampleIndex - center;
            const double proximityPenalty = delta > 0
                ? 0.0048 * static_cast<double>(delta)
                : 0.00045 * static_cast<double>(-delta);
            const double score = (levelRise * 8.0) + (attackStep * 2.0) - proximityPenalty;
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
            const double threshold = bestScore * 0.46;
            for (int sampleIndex = searchStart; sampleIndex <= bestIndex; ++sampleIndex)
            {
                const double score = scores[static_cast<size_t>(sampleIndex - searchStart)];
                if (score >= threshold)
                {
                    refinedIndex = sampleIndex;
                    break;
                }
            }
        }

        double localPeakLevel = 0.0;
        for (int sampleIndex = refinedIndex; sampleIndex <= juce::jmin(searchEnd, refinedIndex + (lookAhead * 3)); ++sampleIndex)
            localPeakLevel = juce::jmax(localPeakLevel, static_cast<double>(std::abs(mono[sampleIndex])));

        const double floorLevel = meanAbsoluteLevelInRange(absolutePrefixSum,
                                                           juce::jmax(0, refinedIndex - (lookBehind * 8)),
                                                           juce::jmax(0, refinedIndex - lookBehind));
        const double onsetThreshold = floorLevel + ((localPeakLevel - floorLevel) * 0.18);
        for (int sampleIndex = refinedIndex; sampleIndex > searchStart; --sampleIndex)
        {
            const double previousAbs = std::abs(mono[sampleIndex - 1]);
            const double currentAbs = std::abs(mono[sampleIndex]);
            if (previousAbs <= onsetThreshold && currentAbs >= onsetThreshold)
            {
                refinedIndex = sampleIndex;
                break;
            }
        }

        refined.push_back(static_cast<int64_t>(refinedIndex));
    }

    std::sort(refined.begin(), refined.end());
    refined.erase(std::unique(refined.begin(), refined.end()), refined.end());
    return refined;
}

std::array<int64_t, SliceModel::VisibleSliceCount> buildLoopStyleAnchorStarts(int64_t totalSamples,
                                                                               const std::vector<int64_t>& onsetSamples)
{
    std::array<int64_t, SliceModel::VisibleSliceCount> starts {};
    const int64_t safeTotalSamples = juce::jmax<int64_t>(1, totalSamples);
    const int64_t lastSample = safeTotalSamples - 1;

    if (onsetSamples.empty())
    {
        for (int i = 0; i < SliceModel::VisibleSliceCount; ++i)
            starts[static_cast<size_t>(i)] = juce::jlimit<int64_t>(0,
                                                                   lastSample,
                                                                   (static_cast<int64_t>(i) * safeTotalSamples)
                                                                       / SliceModel::VisibleSliceCount);
        return starts;
    }

    std::vector<int64_t> sanitizedCandidates;
    sanitizedCandidates.reserve(onsetSamples.size());
    for (const auto onsetSample : onsetSamples)
    {
        if (onsetSample < 0 || onsetSample > lastSample)
            continue;
        sanitizedCandidates.push_back(onsetSample);
    }
    std::sort(sanitizedCandidates.begin(), sanitizedCandidates.end());
    sanitizedCandidates.erase(std::unique(sanitizedCandidates.begin(), sanitizedCandidates.end()),
                              sanitizedCandidates.end());

    if (sanitizedCandidates.empty())
    {
        for (int i = 0; i < SliceModel::VisibleSliceCount; ++i)
            starts[static_cast<size_t>(i)] = juce::jlimit<int64_t>(0,
                                                                   lastSample,
                                                                   (static_cast<int64_t>(i) * safeTotalSamples)
                                                                       / SliceModel::VisibleSliceCount);
        return starts;
    }

    const int lastIndex = SliceModel::VisibleSliceCount - 1;
    int64_t previousStart = -1;
    int previousCandidateIndex = -1;
    for (int i = 0; i < SliceModel::VisibleSliceCount; ++i)
    {
        const double candidatePosition = (sanitizedCandidates.size() == 1)
            ? 0.0
            : (static_cast<double>(i) / static_cast<double>(juce::jmax(1, lastIndex)))
                * static_cast<double>(sanitizedCandidates.size() - 1);
        int candidateIndex = juce::jlimit(0,
                                          static_cast<int>(sanitizedCandidates.size()) - 1,
                                          static_cast<int>(std::llround(candidatePosition)));
        if (candidateIndex <= previousCandidateIndex)
            candidateIndex = juce::jmin(static_cast<int>(sanitizedCandidates.size()) - 1, previousCandidateIndex + 1);

        int64_t chosen = sanitizedCandidates[static_cast<size_t>(candidateIndex)];
        if (chosen <= previousStart)
        {
            auto it = std::upper_bound(sanitizedCandidates.begin(),
                                       sanitizedCandidates.end(),
                                       previousStart);
            if (it != sanitizedCandidates.end())
            {
                candidateIndex = static_cast<int>(std::distance(sanitizedCandidates.begin(), it));
                chosen = *it;
            }
            else
            {
                chosen = juce::jmin(lastSample, previousStart + 1);
            }
        }

        starts[static_cast<size_t>(i)] = juce::jlimit<int64_t>(0, lastSample, chosen);
        previousStart = starts[static_cast<size_t>(i)];
        previousCandidateIndex = candidateIndex;
    }

    return starts;
}

std::vector<int64_t> snapTickMarkersToNearestTransientSamples(const LoadedSampleData& sampleData,
                                                              const std::vector<int64_t>& tickSamples,
                                                              const std::vector<int64_t>& transientSamples)
{
    auto adjustedTicks = WarpGrid::sanitizeBeatTicks(tickSamples, sampleData.sourceLengthSamples);
    const auto sanitizedTransients = WarpGrid::sanitizeBeatTicks(transientSamples, sampleData.sourceLengthSamples);
    if (adjustedTicks.empty() || sanitizedTransients.empty() || sampleData.sourceSampleRate <= 0.0)
        return adjustedTicks;

    const int64_t totalSamples = juce::jmax<int64_t>(1, sampleData.sourceLengthSamples);
    const int64_t maxSnapSamples = static_cast<int64_t>(std::llround(sampleData.sourceSampleRate * 0.045));
    const int64_t minSnapSamples = static_cast<int64_t>(std::llround(sampleData.sourceSampleRate * 0.003));

    for (size_t i = 0; i < adjustedTicks.size(); ++i)
    {
        const int64_t tickSample = adjustedTicks[i];
        const int64_t searchLower = (i > 0)
            ? juce::jmax<int64_t>(1, ((adjustedTicks[i - 1] + tickSample + 1) / 2))
            : 1;
        const int64_t searchUpper = (i + 1 < adjustedTicks.size())
            ? juce::jmin<int64_t>(totalSamples - 1, ((tickSample + adjustedTicks[i + 1]) / 2))
            : (totalSamples - 1);
        if (searchUpper < searchLower)
            continue;

        const int64_t leftGap = (i > 0)
            ? juce::jmax<int64_t>(1, tickSample - adjustedTicks[i - 1])
            : juce::jmax<int64_t>(1, searchUpper - tickSample);
        const int64_t rightGap = (i + 1 < adjustedTicks.size())
            ? juce::jmax<int64_t>(1, adjustedTicks[i + 1] - tickSample)
            : juce::jmax<int64_t>(1, tickSample - searchLower);
        const int64_t localGap = juce::jmax<int64_t>(1, juce::jmin(leftGap, rightGap));
        const int64_t snapTolerance = juce::jlimit<int64_t>(minSnapSamples,
                                                            maxSnapSamples,
                                                            static_cast<int64_t>(std::llround(static_cast<double>(localGap) * 0.35)));

        const auto transientBegin = std::lower_bound(sanitizedTransients.begin(), sanitizedTransients.end(), searchLower);
        const auto transientEnd = std::upper_bound(transientBegin, sanitizedTransients.end(), searchUpper);
        if (transientBegin == transientEnd)
            continue;

        auto nearestIt = std::lower_bound(transientBegin, transientEnd, tickSample);
        int64_t bestSample = tickSample;
        int64_t bestDistance = std::numeric_limits<int64_t>::max();

        if (nearestIt != transientEnd)
        {
            bestSample = *nearestIt;
            bestDistance = std::abs(*nearestIt - tickSample);
        }

        if (nearestIt != transientBegin)
        {
            const auto prevIt = std::prev(nearestIt);
            const int64_t prevDistance = std::abs(*prevIt - tickSample);
            if (prevDistance <= bestDistance)
            {
                bestSample = *prevIt;
                bestDistance = prevDistance;
            }
        }

        if (bestDistance <= snapTolerance)
            adjustedTicks[i] = bestSample;
    }

    adjustedTicks = WarpGrid::sanitizeBeatTicks(adjustedTicks, sampleData.sourceLengthSamples);
    return adjustedTicks;
}

std::vector<int64_t> buildCanonicalTransientMarkerSamples(const LoadedSampleData& sampleData,
                                                          const std::vector<int64_t>& transientEditSamples,
                                                          bool transientMarkersEdited)
{
    const bool useEditedMarkers = transientMarkersEdited && !transientEditSamples.empty();
    auto markerSamples = useEditedMarkers
        ? WarpGrid::sanitizeBeatTicks(transientEditSamples, sampleData.sourceLengthSamples)
        : snapTickMarkersToNearestTransientSamples(sampleData,
                                                   sampleData.analysis.beatTickSamples,
                                                   sampleData.analysis.transientSamples);

    if (markerSamples.empty())
    {
        markerSamples = WarpGrid::sanitizeBeatTicks(sampleData.analysis.transientSamples,
                                                   sampleData.sourceLengthSamples);
    }

    markerSamples.erase(std::remove_if(markerSamples.begin(),
                                       markerSamples.end(),
                                       [&sampleData](int64_t sample)
                                       {
                                           return sample <= 0 || sample >= sampleData.sourceLengthSamples;
                                       }),
                        markerSamples.end());
    return markerSamples;
}

std::vector<int64_t> filterMarkerSamplesToRange(const std::vector<int64_t>& markerSamples,
                                                int64_t rangeStartSample,
                                                int64_t rangeEndSample)
{
    std::vector<int64_t> filtered;
    if (rangeEndSample <= rangeStartSample)
        return filtered;

    filtered.reserve(markerSamples.size());
    for (const auto sample : markerSamples)
    {
        if (sample >= rangeStartSample && sample < rangeEndSample)
            filtered.push_back(sample);
    }

    return filtered;
}

int getLegacyLoopTransientLastPageStart(int candidateCount)
{
    if (candidateCount <= SliceModel::VisibleSliceCount)
        return 0;

    return ((candidateCount - 1) / SliceModel::VisibleSliceCount) * SliceModel::VisibleSliceCount;
}

int getLegacyLoopTransientPageStartIndex(int requestedStartIndex, int candidateCount)
{
    return juce::jlimit(0,
                        getLegacyLoopTransientLastPageStart(candidateCount),
                        requestedStartIndex);
}

std::vector<int64_t> detectLoopStyleTransientSamples(const juce::AudioBuffer<float>& monoBuffer,
                                                     double sampleRate)
{
    std::vector<int64_t> transientSamples;
    const int totalSamples = monoBuffer.getNumSamples();
    if (totalSamples <= 0 || sampleRate <= 0.0)
        return transientSamples;

    int fftOrder = 8;
    while ((1 << fftOrder) < juce::jmin(2048, totalSamples) && fftOrder < 12)
        ++fftOrder;
    const int frameSize = 1 << fftOrder;
    const int hopSize = juce::jmax(32, frameSize / 8);
    const int frames = juce::jmax(1, 1 + ((totalSamples - frameSize) / hopSize));
    if (frames < 4)
        return transientSamples;

    juce::dsp::FFT fft(fftOrder);
    juce::dsp::WindowingFunction<float> window(static_cast<size_t>(frameSize),
                                               juce::dsp::WindowingFunction<float>::hann,
                                               true);

    const float* mono = monoBuffer.getReadPointer(0);
    const int halfBins = frameSize / 2;
    std::vector<float> fftData(static_cast<size_t>(2 * frameSize), 0.0f);
    std::vector<float> prevMag(static_cast<size_t>(halfBins), 0.0f);
    std::vector<float> spectralFlux(static_cast<size_t>(frames), 0.0f);
    std::vector<float> frameEnergy(static_cast<size_t>(frames), 0.0f);

    for (int frame = 0; frame < frames; ++frame)
    {
        const int start = frame * hopSize;
        double energy = 0.0;

        for (int n = 0; n < frameSize; ++n)
        {
            const int sampleIndex = juce::jlimit(0, totalSamples - 1, start + n);
            const float sample = mono[sampleIndex];
            fftData[static_cast<size_t>(n)] = sample;
            energy += static_cast<double>(sample * sample);
        }

        for (int n = frameSize; n < 2 * frameSize; ++n)
            fftData[static_cast<size_t>(n)] = 0.0f;

        window.multiplyWithWindowingTable(fftData.data(), static_cast<size_t>(frameSize));
        fft.performFrequencyOnlyForwardTransform(fftData.data(), true);

        frameEnergy[static_cast<size_t>(frame)] = static_cast<float>(std::sqrt(energy / static_cast<double>(frameSize)));

        float flux = 0.0f;
        for (int bin = 1; bin < halfBins; ++bin)
        {
            const float mag = fftData[static_cast<size_t>(bin)];
            const float diff = juce::jmax(0.0f, mag - prevMag[static_cast<size_t>(bin)]);
            const float weight = 1.0f + (2.0f * static_cast<float>(bin) / static_cast<float>(halfBins));
            flux += diff * weight;
            prevMag[static_cast<size_t>(bin)] = mag;
        }

        spectralFlux[static_cast<size_t>(frame)] = flux;
    }

    std::vector<float> smoothedFlux(static_cast<size_t>(frames), 0.0f);
    for (int i = 0; i < frames; ++i)
    {
        const int a = juce::jmax(0, i - 1);
        const int b = juce::jmin(frames - 1, i + 1);
        float sum = 0.0f;
        for (int k = a; k <= b; ++k)
            sum += spectralFlux[static_cast<size_t>(k)];
        smoothedFlux[static_cast<size_t>(i)] = sum / static_cast<float>(b - a + 1);
    }

    std::vector<float> energyDiff(static_cast<size_t>(frames), 0.0f);
    for (int i = 1; i < frames; ++i)
        energyDiff[static_cast<size_t>(i)] = juce::jmax(0.0f, frameEnergy[static_cast<size_t>(i)] - frameEnergy[static_cast<size_t>(i - 1)]);

    auto medianInWindow = [](const std::vector<float>& values, int start, int end)
    {
        std::vector<float> temp;
        temp.reserve(static_cast<size_t>(end - start + 1));
        for (int i = start; i <= end; ++i)
            temp.push_back(values[static_cast<size_t>(i)]);
        auto midIt = temp.begin() + (temp.size() / 2);
        std::nth_element(temp.begin(), midIt, temp.end());
        return *midIt;
    };

    std::vector<float> novelty(static_cast<size_t>(frames), 0.0f);
    float noveltySum = 0.0f;
    for (int i = 0; i < frames; ++i)
    {
        const int a = juce::jmax(0, i - 8);
        const int b = juce::jmin(frames - 1, i + 8);
        const float adaptive = (medianInWindow(smoothedFlux, a, b) * 1.25f) + 1.0e-6f;
        const float peakPart = juce::jmax(0.0f, smoothedFlux[static_cast<size_t>(i)] - adaptive);
        const float mixed = peakPart + (0.25f * energyDiff[static_cast<size_t>(i)]);
        novelty[static_cast<size_t>(i)] = mixed;
        noveltySum += mixed;
    }

    const float noveltyMean = noveltySum / static_cast<float>(juce::jmax(1, frames));
    const float noveltyMax = *std::max_element(novelty.begin(), novelty.end());
    const float minPeakLevel = juce::jmax(1.0e-6f,
                                          juce::jmax(noveltyMean * 0.35f, noveltyMax * 0.10f));
    const int minPeakSpacingFrames = juce::jmax(1,
        static_cast<int>((0.015 * sampleRate) / static_cast<double>(hopSize)));

    std::vector<std::pair<int, float>> onsetFrames;
    onsetFrames.reserve(static_cast<size_t>(frames));

    for (int i = 1; i < (frames - 1); ++i)
    {
        const float center = novelty[static_cast<size_t>(i)];
        if (center < minPeakLevel)
            continue;
        if (center < novelty[static_cast<size_t>(i - 1)] || center < novelty[static_cast<size_t>(i + 1)])
            continue;

        if (!onsetFrames.empty() && (i - onsetFrames.back().first) < minPeakSpacingFrames)
        {
            if (center > onsetFrames.back().second)
                onsetFrames.back() = { i, center };
            continue;
        }

        onsetFrames.emplace_back(i, center);
    }

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

    transientSamples.reserve(onsetFrames.size());
    for (const auto& onset : onsetFrames)
    {
        const int centered = (onset.first * hopSize) + (frameSize / 2);
        transientSamples.push_back(static_cast<int64_t>(juce::jlimit(0, totalSamples - 1, centered)));
        if (static_cast<int>(transientSamples.size()) >= kMaxStoredTransientCount)
            break;
    }

    if (const auto refined = refineOnsetSamplesToLeadingEdges(monoBuffer, transientSamples, frameSize, hopSize);
        !refined.empty())
    {
        return refined;
    }

    return transientSamples;
}

double normalizeTempoEstimate(double bpm)
{
    if (!(bpm > 0.0) || !std::isfinite(bpm))
        return 0.0;

    while (bpm < 70.0)
        bpm *= 2.0;
    while (bpm > 180.0)
        bpm *= 0.5;
    return bpm;
}

double evaluateTempoAutocorrelationScore(const std::vector<int64_t>& transientSamples,
                                         double sampleRate,
                                         double bpm)
{
    if (transientSamples.size() < 2 || sampleRate <= 0.0 || !(bpm > 0.0))
        return 0.0;

    constexpr double pulseRateHz = 200.0;
    const int64_t lastSample = transientSamples.back();
    if (lastSample <= 0)
        return 0.0;

    const int pulseCount = juce::jlimit(64,
                                        200000,
                                        1 + static_cast<int>(std::ceil((static_cast<double>(lastSample) / sampleRate) * pulseRateHz)));
    if (pulseCount <= 4)
        return 0.0;

    std::vector<float> pulseTrain(static_cast<size_t>(pulseCount), 0.0f);
    for (const auto sample : transientSamples)
    {
        const int center = juce::jlimit(0,
                                        pulseCount - 1,
                                        static_cast<int>(std::llround((static_cast<double>(sample) / sampleRate) * pulseRateHz)));
        pulseTrain[static_cast<size_t>(center)] += 1.0f;
        if (center > 0)
            pulseTrain[static_cast<size_t>(center - 1)] += 0.35f;
        if (center + 1 < pulseCount)
            pulseTrain[static_cast<size_t>(center + 1)] += 0.35f;
    }

    const int beatLag = juce::jlimit(1,
                                     pulseCount - 1,
                                     static_cast<int>(std::lround((60.0 / bpm) * pulseRateHz)));
    const int halfLag = juce::jmax(1, beatLag / 2);
    const int doubleLag = juce::jmin(pulseCount - 1, beatLag * 2);

    auto correlationAtLag = [&](int lag)
    {
        if (lag <= 0 || lag >= pulseCount)
            return 0.0;
        double sum = 0.0;
        int count = 0;
        for (int i = 0; i + lag < pulseCount; ++i)
        {
            sum += static_cast<double>(pulseTrain[static_cast<size_t>(i)])
                * static_cast<double>(pulseTrain[static_cast<size_t>(i + lag)]);
            ++count;
        }
        return count > 0 ? (sum / static_cast<double>(count)) : 0.0;
    };

    const double mainScore = correlationAtLag(beatLag);
    const double halfScore = correlationAtLag(halfLag);
    const double doubleScore = correlationAtLag(doubleLag);
    return mainScore + (0.35 * doubleScore) + (0.15 * halfScore);
}

double estimateTempoFromTransients(const std::vector<int64_t>& transientSamples, double sampleRate)
{
    if (transientSamples.size() < 2 || sampleRate <= 0.0)
        return 0.0;

    constexpr double bpmMin = 70.0;
    constexpr double bpmMax = 180.0;
    constexpr double bpmStep = 0.5;
    constexpr int histogramBins = static_cast<int>(((bpmMax - bpmMin) / bpmStep) + 1.0);
    std::array<double, histogramBins> histogram {};
    std::array<double, histogramBins> autoCorrelationScores {};

    auto addCandidate = [&](double bpm, double weight)
    {
        bpm = normalizeTempoEstimate(bpm);
        if (!(bpm >= bpmMin && bpm <= bpmMax) || !std::isfinite(weight) || weight <= 0.0)
            return;

        const int index = juce::jlimit(0,
                                       histogramBins - 1,
                                       static_cast<int>(std::lround((bpm - bpmMin) / bpmStep)));
        histogram[static_cast<size_t>(index)] += weight;
    };

    for (size_t i = 0; i + 1 < transientSamples.size(); ++i)
    {
        const size_t maxNeighbor = juce::jmin(transientSamples.size(), i + 12);
        for (size_t j = i + 1; j < maxNeighbor; ++j)
        {
            const double deltaSamples = static_cast<double>(transientSamples[j] - transientSamples[i]);
            const double seconds = deltaSamples / sampleRate;
            if (seconds <= 0.08 || seconds >= 2.5)
                continue;

            const double distanceWeight = 1.0 / static_cast<double>(j - i);
            for (int beatMultiple = 1; beatMultiple <= 8; ++beatMultiple)
            {
                const double bpm = (60.0 * static_cast<double>(beatMultiple)) / seconds;
                const double multipleWeight = distanceWeight / std::sqrt(static_cast<double>(beatMultiple));
                addCandidate(bpm, multipleWeight);
            }
        }
    }

    std::array<double, histogramBins> smoothed {};
    for (int i = 0; i < histogramBins; ++i)
    {
        const double left = histogram[static_cast<size_t>(juce::jmax(0, i - 1))];
        const double center = histogram[static_cast<size_t>(i)];
        const double right = histogram[static_cast<size_t>(juce::jmin(histogramBins - 1, i + 1))];
        smoothed[static_cast<size_t>(i)] = (left * 0.25) + (center * 0.5) + (right * 0.25);
    }

    const double histogramPeak = *std::max_element(smoothed.begin(), smoothed.end());
    if (!(histogramPeak > 0.0))
        return 0.0;

    double autoPeak = 0.0;
    for (int i = 0; i < histogramBins; ++i)
    {
        const double bpm = bpmMin + (static_cast<double>(i) * bpmStep);
        autoCorrelationScores[static_cast<size_t>(i)] = evaluateTempoAutocorrelationScore(transientSamples, sampleRate, bpm);
        autoPeak = juce::jmax(autoPeak, autoCorrelationScores[static_cast<size_t>(i)]);
    }

    std::array<double, histogramBins> combined {};
    for (int i = 0; i < histogramBins; ++i)
    {
        const double bpm = bpmMin + (static_cast<double>(i) * bpmStep);
        const double primary = smoothed[static_cast<size_t>(i)] / histogramPeak;
        const double autoScore = autoPeak > 0.0
            ? (autoCorrelationScores[static_cast<size_t>(i)] / autoPeak)
            : 0.0;

        double harmonicScore = primary;
        auto sampleNormalized = [&](double candidateBpm, double weight)
        {
            if (!(candidateBpm >= bpmMin && candidateBpm <= bpmMax))
                return 0.0;
            const int idx = juce::jlimit(0,
                                         histogramBins - 1,
                                         static_cast<int>(std::lround((candidateBpm - bpmMin) / bpmStep)));
            return (smoothed[static_cast<size_t>(idx)] / histogramPeak) * weight;
        };

        harmonicScore += sampleNormalized(normalizeTempoEstimate(bpm * 2.0), 0.28);
        harmonicScore += sampleNormalized(normalizeTempoEstimate(bpm * 0.5), 0.18);

        combined[static_cast<size_t>(i)] = (harmonicScore * 0.65) + (autoScore * 0.35);
    }

    const auto bestIt = std::max_element(combined.begin(), combined.end());
    if (bestIt == combined.end() || *bestIt <= 0.0)
        return 0.0;

    const int bestIndex = static_cast<int>(std::distance(combined.begin(), bestIt));
    return bpmMin + (static_cast<double>(bestIndex) * bpmStep);
}

double estimatePitchHzFromAutocorrelation(const juce::AudioBuffer<float>& monoBuffer,
                                          double sampleRate)
{
    if (sampleRate <= 0.0 || monoBuffer.getNumSamples() < 2048)
        return 0.0;

    const float* mono = monoBuffer.getReadPointer(0);
    const int numSamples = monoBuffer.getNumSamples();
    const int windowSize = juce::jmin(8192, juce::jmax(2048, numSamples / 4));
    const int maxStart = juce::jmax(0, numSamples - windowSize);
    int bestStart = 0;
    double bestEnergy = 0.0;

    for (int start = 0; start <= maxStart; start += juce::jmax(256, windowSize / 4))
    {
        double energy = 0.0;
        for (int i = start; i < start + windowSize; ++i)
        {
            const double sample = mono[i];
            energy += sample * sample;
        }

        if (energy > bestEnergy)
        {
            bestEnergy = energy;
            bestStart = start;
        }
    }

    if (bestEnergy <= 1.0e-6)
        return 0.0;

    const int minLag = juce::jmax(8, static_cast<int>(std::floor(sampleRate / 1000.0)));
    const int maxLag = juce::jmin(windowSize / 2, static_cast<int>(std::ceil(sampleRate / 50.0)));
    double bestCorrelation = 0.0;
    int bestLag = 0;

    for (int lag = minLag; lag <= maxLag; ++lag)
    {
        double correlation = 0.0;
        for (int i = 0; i < (windowSize - lag); ++i)
            correlation += static_cast<double>(mono[bestStart + i]) * mono[bestStart + i + lag];

        if (correlation > bestCorrelation)
        {
            bestCorrelation = correlation;
            bestLag = lag;
        }
    }

    if (bestLag <= 0 || bestCorrelation <= 1.0e-6)
        return 0.0;

    return sampleRate / static_cast<double>(bestLag);
}

int pitchHzToMidi(double pitchHz)
{
    if (!(pitchHz > 0.0) || !std::isfinite(pitchHz))
        return -1;

    return static_cast<int>(std::lround(69.0 + (12.0 * std::log2(pitchHz / 440.0))));
}

juce::String buildCueName(int index)
{
    return "C" + juce::String(index + 1);
}

float quantizeKeyLockRate(float playbackRate)
{
    return std::round(juce::jlimit(0.03125f, 8.0f, playbackRate) * 1000.0f) / 1000.0f;
}

float quantizeKeyLockPitch(float pitchSemitones)
{
    return std::round(juce::jlimit(-24.0f, 24.0f, pitchSemitones) * 100.0f) / 100.0f;
}

bool matchesKeyLockRequest(const StretchedSliceBuffer& cache,
                           const SampleSlice& slice,
                           float playbackRate,
                           float pitchSemitones,
                           TimeStretchBackend backend)
{
    return cache.sliceId == slice.id
        && cache.sliceStartSample == slice.startSample
        && cache.sliceEndSample == slice.endSample
        && cache.backend == sanitizeTimeStretchBackend(static_cast<int>(backend))
        && std::abs(cache.playbackRate - quantizeKeyLockRate(playbackRate)) <= kKeyLockCacheRateTolerance
        && std::abs(cache.pitchSemitones - quantizeKeyLockPitch(pitchSemitones)) <= kKeyLockCachePitchTolerance;
}

#if MLRVST_ENABLE_ESSENTIA_NATIVE
struct EssentiaNativeRuntime
{
    EssentiaNativeRuntime() { essentia::init(); }
    ~EssentiaNativeRuntime() { essentia::shutdown(); }
};

EssentiaNativeRuntime& getEssentiaNativeRuntime()
{
    static EssentiaNativeRuntime runtime;
    return runtime;
}

std::optional<SampleAnalysisSummary> runEssentiaAnalysis(const LoadedSampleData& sampleData,
                                                         SamplePitchAnalysisProfile pitchProfile,
                                                         juce::String* failureReason = nullptr)
{
    if (sampleData.audioBuffer.getNumSamples() <= 0 || sampleData.sourceSampleRate <= 0.0)
    {
        if (failureReason != nullptr)
            *failureReason = "ES native empty buffer";
        return std::nullopt;
    }

    juce::ignoreUnused(getEssentiaNativeRuntime());

    const auto monoBuffer = buildMonoBuffer(sampleData);
    const int totalSamples = monoBuffer.getNumSamples();
    if (totalSamples <= 0)
    {
        if (failureReason != nullptr)
            *failureReason = "ES native empty mono";
        return std::nullopt;
    }

    std::vector<essentia::Real> signal(static_cast<size_t>(totalSamples));
    const auto* mono = monoBuffer.getReadPointer(0);
    for (int i = 0; i < totalSamples; ++i)
        signal[static_cast<size_t>(i)] = mono[i];

    SampleAnalysisSummary summary;

    try
    {
        auto& factory = essentia::standard::AlgorithmFactory::instance();

        std::unique_ptr<essentia::standard::Algorithm> rhythm(
            factory.create("RhythmExtractor2013",
                           "method", "multifeature",
                           "minTempo", 40.0,
                           "maxTempo", 240.0));

        essentia::Real bpm = 0.0f;
        essentia::Real confidence = 0.0f;
        std::vector<essentia::Real> ticks;
        std::vector<essentia::Real> estimates;
        std::vector<essentia::Real> bpmIntervals;
        rhythm->input("signal").set(signal);
        rhythm->output("bpm").set(bpm);
        rhythm->output("ticks").set(ticks);
        rhythm->output("confidence").set(confidence);
        rhythm->output("estimates").set(estimates);
        rhythm->output("bpmIntervals").set(bpmIntervals);
        rhythm->compute();

        summary.estimatedTempoBpm = normalizeTempoEstimate(static_cast<double>(bpm));
        summary.beatTickSamples.reserve(ticks.size());
        for (const auto tickSeconds : ticks)
        {
            if (!std::isfinite(tickSeconds))
                continue;
            const int64_t tickSample = static_cast<int64_t>(std::llround(
                static_cast<double>(tickSeconds) * sampleData.sourceSampleRate));
            summary.beatTickSamples.push_back(
                juce::jlimit<int64_t>(0, juce::jmax<int64_t>(0, sampleData.sourceLengthSamples - 1), tickSample));
            if (static_cast<int>(summary.beatTickSamples.size()) >= kMaxStoredTransientCount)
                break;
        }

        {
            std::unique_ptr<essentia::standard::Algorithm> keyExtractor(
                factory.create("KeyExtractor",
                               "sampleRate", sampleData.sourceSampleRate,
                               "frameSize", 4096,
                               "hopSize", 2048,
                               "hpcpSize", 36,
                               "minFrequency", 40.0,
                               "maxFrequency", 5000.0,
                               "maximumSpectralPeaks", 120,
                               "profileType", "edma",
                               "pcpThreshold", 0.2,
                               "averageDetuningCorrection", true,
                               "weightType", "cosine"));

            std::string detectedKey;
            std::string detectedScale;
            essentia::Real keyStrength = 0.0f;
            keyExtractor->input("audio").set(signal);
            keyExtractor->output("key").set(detectedKey);
            keyExtractor->output("scale").set(detectedScale);
	            keyExtractor->output("strength").set(keyStrength);
	            keyExtractor->compute();

	            summary.estimatedScaleIndex = pitchScaleIndexFromEssentiaName(juce::String(detectedScale));
	            summary.estimatedScaleConfidence = juce::jlimit(0.0f, 1.0f, static_cast<float>(keyStrength));

	            if (pitchProfile == SamplePitchAnalysisProfile::Polyphonic)
	            {
	                const int pitchClass = pitchClassFromKeyName(juce::String(detectedKey));
	                if (pitchClass >= 0 && std::isfinite(keyStrength) && keyStrength >= 0.08f)
	                {
	                    summary.estimatedPitchMidi = 60 + pitchClass;
	                    summary.estimatedPitchHz = midiToPitchHz(summary.estimatedPitchMidi);
	                    summary.estimatedPitchConfidence = juce::jlimit(0.0f, 1.0f, static_cast<float>(keyStrength));
	                }
	            }
        }

        if (pitchProfile == SamplePitchAnalysisProfile::Polyphonic)
        {
            juce::ignoreUnused(summary.estimatedScaleIndex);
        }
        else
        {
           #if MLRVST_ENABLE_LIBPYIN
	            if (const auto pyinPitch = runLibPyinMonophonicPitch(sampleData))
	            {
	                summary.estimatedPitchHz = pyinPitch->frequency;
	                summary.estimatedPitchMidi = pyinPitch->midi;
	                summary.estimatedPitchConfidence = pyinPitch->confidence;
	            }
	            #endif

            if (summary.estimatedPitchMidi < 0)
            {
                int frameSize = 4096;
                while (frameSize > totalSamples && frameSize > 512)
                    frameSize /= 2;
                const int hopSize = juce::jmax(128, frameSize / 8);

                std::unique_ptr<essentia::standard::Algorithm> frameCutter(
                    factory.create("FrameCutter",
                                   "frameSize", frameSize,
                                   "hopSize", hopSize,
                                   "startFromZero", false));
                std::unique_ptr<essentia::standard::Algorithm> window(
                    factory.create("Windowing", "type", "hann", "zeroPadding", 0));
                std::unique_ptr<essentia::standard::Algorithm> spectrum(
                    factory.create("Spectrum", "size", frameSize));
                std::unique_ptr<essentia::standard::Algorithm> pitchDetect(
                    factory.create("PitchYinFFT",
                                   "frameSize", frameSize,
                                   "sampleRate", sampleData.sourceSampleRate));

                std::vector<essentia::Real> frame;
                std::vector<essentia::Real> windowedFrame;
                std::vector<essentia::Real> magnitudeSpectrum;
                essentia::Real pitchHz = 0.0f;
                essentia::Real pitchConfidence = 0.0f;
                frameCutter->input("signal").set(signal);
                frameCutter->output("frame").set(frame);
                window->input("frame").set(frame);
                window->output("frame").set(windowedFrame);
                spectrum->input("frame").set(windowedFrame);
                spectrum->output("spectrum").set(magnitudeSpectrum);
                pitchDetect->input("spectrum").set(magnitudeSpectrum);
                pitchDetect->output("pitch").set(pitchHz);
                pitchDetect->output("pitchConfidence").set(pitchConfidence);

                struct MonophonicPitchFrame
                {
                    double hz = 0.0;
                    double confidence = 0.0;
                    int midi = -1;
                };
                std::vector<MonophonicPitchFrame> detectedPitches;
                while (true)
                {
                    frameCutter->compute();
                    if (frame.empty())
                        break;

                    float peak = 0.0f;
                    for (const auto sample : frame)
                        peak = juce::jmax(peak, std::abs(sample));
                    if (peak < 2.0e-4f)
                        continue;

                    window->compute();
                    spectrum->compute();
                    pitchDetect->compute();

                    const int midi = pitchHzToMidi(static_cast<double>(pitchHz));
                    if (std::isfinite(pitchHz)
                        && pitchHz > 20.0f
                        && pitchConfidence >= 0.55f
                        && midi >= 0)
                    {
                        detectedPitches.push_back({ static_cast<double>(pitchHz),
                                                    static_cast<double>(pitchConfidence),
                                                    midi });
                    }
                }

                if (!detectedPitches.empty())
                {
                    std::array<double, 128> midiWeights {};
                    for (const auto& framePitch : detectedPitches)
                        midiWeights[static_cast<size_t>(juce::jlimit(0, 127, framePitch.midi))] += framePitch.confidence;

                    const auto bestMidiIt = std::max_element(midiWeights.begin(), midiWeights.end());
                    const int bestMidi = static_cast<int>(std::distance(midiWeights.begin(), bestMidiIt));
                    std::vector<double> filteredHz;
                    filteredHz.reserve(detectedPitches.size());
                    for (const auto& framePitch : detectedPitches)
                    {
                        if (std::abs(framePitch.midi - bestMidi) <= 1)
                            filteredHz.push_back(framePitch.hz);
                    }

                    if (filteredHz.empty())
                    {
                        for (const auto& framePitch : detectedPitches)
                            filteredHz.push_back(framePitch.hz);
                    }

                    std::sort(filteredHz.begin(), filteredHz.end());
                    summary.estimatedPitchHz = filteredHz[filteredHz.size() / 2];
                    summary.estimatedPitchMidi = pitchHzToMidi(summary.estimatedPitchHz);
                }
            }
        }

        const bool hasTempo = summary.estimatedTempoBpm > 0.0 || !summary.beatTickSamples.empty();
        const bool hasPitch = summary.estimatedPitchMidi >= 0;
        if (!hasTempo && !hasPitch)
        {
            if (failureReason != nullptr)
                *failureReason = "ES native no descriptors";
            return std::nullopt;
        }

        summary.essentiaUsed = true;
        summary.analysisSource = hasPitch
            ? (pitchProfile == SamplePitchAnalysisProfile::Polyphonic
                ? "essentia native tempo/poly-key/ticks"
                :
               #if MLRVST_ENABLE_LIBPYIN
                    "essentia native tempo/libpyin mono-pitch/ticks"
               #else
                    "essentia native tempo/mono-pitch/ticks"
               #endif
                  )
            : "essentia native tempo/ticks";
        return summary;
    }
    catch (const std::exception& exception)
    {
        if (failureReason != nullptr)
            *failureReason = "ES native " + juce::String(exception.what()).substring(0, 96);
    }
    catch (...)
    {
        if (failureReason != nullptr)
            *failureReason = "ES native exception";
    }

    return std::nullopt;
}
#endif

SampleAnalysisSummary buildInternalAnalysis(const LoadedSampleData& sampleData)
{
    SampleAnalysisSummary summary;
    summary.analysisSource = "internal";

    const auto monoBuffer = buildMonoBuffer(sampleData);
    summary.transientSamples = detectLoopStyleTransientSamples(monoBuffer,
                                                               sampleData.sourceSampleRate);
    summary.estimatedTempoBpm = estimateTempoFromTransients(summary.transientSamples,
                                                            sampleData.sourceSampleRate);
    summary.estimatedPitchHz = estimatePitchHzFromAutocorrelation(monoBuffer,
                                                                  sampleData.sourceSampleRate);
    summary.estimatedPitchMidi = pitchHzToMidi(summary.estimatedPitchHz);
    return summary;
}

void fillSliceMetadata(SampleSlice& slice, int sliceIndex, int64_t totalSamples)
{
    slice.id = sliceIndex;
    slice.startSample = juce::jmax<int64_t>(0, slice.startSample);
    slice.endSample = juce::jmax(slice.startSample + 1, slice.endSample);
    slice.normalizedStart = safeNormalizedPosition(slice.startSample, totalSamples);
    slice.normalizedEnd = safeNormalizedPosition(slice.endSample, totalSamples);
    slice.label = buildSliceLabel(sliceIndex);
}

float readInterpolatedSample(const juce::AudioBuffer<float>& buffer, int channel, double samplePosition)
{
    const int numChannels = buffer.getNumChannels();
    const int numSamples = buffer.getNumSamples();
    if (numChannels <= 0 || numSamples <= 0)
        return 0.0f;

    const int clampedChannel = juce::jlimit(0, numChannels - 1, channel);
    const float* data = buffer.getReadPointer(clampedChannel);
    const double clampedPos = juce::jlimit(0.0, static_cast<double>(juce::jmax(0, numSamples - 1)), samplePosition);
    const int indexA = static_cast<int>(clampedPos);
    const int indexB = juce::jmin(indexA + 1, numSamples - 1);
    const float frac = static_cast<float>(clampedPos - static_cast<double>(indexA));
    return juce::jmap(frac, data[indexA], data[indexB]);
}

void buildWaveformPreview(LoadedSampleData& sampleData)
{
    const auto numSamples = sampleData.audioBuffer.getNumSamples();
    const auto numChannels = sampleData.audioBuffer.getNumChannels();

    if (numSamples <= 0 || numChannels <= 0)
    {
        sampleData.previewMin.clear();
        sampleData.previewMax.clear();
        return;
    }

    const int pointCount = juce::jmin(LoadedSampleData::PreviewPointCount, juce::jmax(64, numSamples));
    sampleData.previewMin.assign(static_cast<size_t>(pointCount), 0.0f);
    sampleData.previewMax.assign(static_cast<size_t>(pointCount), 0.0f);

    for (int pointIndex = 0; pointIndex < pointCount; ++pointIndex)
    {
        const int startSample = static_cast<int>((static_cast<int64_t>(pointIndex) * numSamples) / pointCount);
        const int endSample = juce::jmax(startSample + 1,
                                         static_cast<int>((static_cast<int64_t>(pointIndex + 1) * numSamples) / pointCount));

        float minValue = 1.0f;
        float maxValue = -1.0f;

        for (int sampleIndex = startSample; sampleIndex < juce::jmin(endSample, numSamples); ++sampleIndex)
        {
            float monoSample = 0.0f;

            if (numChannels == 1)
                monoSample = sampleData.audioBuffer.getSample(0, sampleIndex);
            else
                monoSample = 0.5f * (sampleData.audioBuffer.getSample(0, sampleIndex)
                                   + sampleData.audioBuffer.getSample(1, sampleIndex));

            minValue = juce::jmin(minValue, monoSample);
            maxValue = juce::jmax(maxValue, monoSample);
        }

        if (minValue > maxValue)
        {
            minValue = 0.0f;
            maxValue = 0.0f;
        }

        sampleData.previewMin[static_cast<size_t>(pointIndex)] = minValue;
        sampleData.previewMax[static_cast<size_t>(pointIndex)] = maxValue;
    }
}

std::shared_ptr<const LoadedSampleData> buildLoadedSampleFromBuffer(const juce::AudioBuffer<float>& buffer,
                                                                    double sourceSampleRate,
                                                                    const juce::String& sourcePath,
                                                                    const juce::String& displayName)
{
    if (buffer.getNumSamples() <= 0
        || buffer.getNumChannels() <= 0
        || !std::isfinite(sourceSampleRate)
        || sourceSampleRate <= 1000.0)
    {
        return {};
    }

    auto loadedSample = std::make_shared<LoadedSampleData>();
    const int channelCount = juce::jlimit(1, 2, buffer.getNumChannels());
    const int numSamples = buffer.getNumSamples();
    loadedSample->audioBuffer.setSize(2, numSamples, false, false, true);
    loadedSample->audioBuffer.clear();

    loadedSample->audioBuffer.copyFrom(0, 0, buffer, 0, 0, numSamples);
    if (channelCount == 1)
        loadedSample->audioBuffer.copyFrom(1, 0, buffer, 0, 0, numSamples);
    else
        loadedSample->audioBuffer.copyFrom(1, 0, buffer, 1, 0, numSamples);

    loadedSample->sourcePath = sourcePath;
    loadedSample->displayName = displayName.isNotEmpty()
        ? displayName
        : (sourcePath.isNotEmpty()
            ? juce::File(sourcePath).getFileNameWithoutExtension()
            : juce::String("Embedded Flip Sample"));
    loadedSample->sourceSampleRate = sourceSampleRate;
    loadedSample->numChannels = channelCount;
    loadedSample->sourceLengthSamples = numSamples;
    loadedSample->durationSeconds = numSamples / juce::jmax(1.0, sourceSampleRate);
    buildWaveformPreview(*loadedSample);
    loadedSample->analysis = buildInternalAnalysis(*loadedSample);
    return std::const_pointer_cast<const LoadedSampleData>(loadedSample);
}

std::shared_ptr<const StretchedSliceBuffer> buildStretchedSliceBuffer(const LoadedSampleData& sampleData,
                                                                      const SampleSlice& slice,
                                                                      float playbackRate,
                                                                      float pitchSemitones,
                                                                      TimeStretchBackend backend)
{
    const int64_t sliceStart = juce::jmax<int64_t>(0, slice.startSample);
    const int64_t sliceEnd = juce::jmin<int64_t>(sampleData.sourceLengthSamples,
                                                 juce::jmax(slice.startSample + 1, slice.endSample));
    const int64_t sliceLength64 = sliceEnd - sliceStart;
    if (sliceLength64 <= 0 || sliceLength64 > static_cast<int64_t>(std::numeric_limits<int>::max()))
        return {};

    const int sliceLength = static_cast<int>(sliceLength64);
    juce::AudioBuffer<float> sourceSlice(2, sliceLength);
    sourceSlice.copyFrom(0, 0, sampleData.audioBuffer, 0, static_cast<int>(sliceStart), sliceLength);
    sourceSlice.copyFrom(1, 0, sampleData.audioBuffer, 1, static_cast<int>(sliceStart), sliceLength);

    const float quantizedRate = quantizeKeyLockRate(playbackRate);
    const float quantizedPitch = quantizeKeyLockPitch(pitchSemitones);

    juce::AudioBuffer<float> stretchedBuffer;
    if (!renderTimeStretchedBufferForRate(sourceSlice,
                                          sampleData.sourceSampleRate,
                                          quantizedRate,
                                          quantizedPitch,
                                          backend,
                                          stretchedBuffer))
        return {};

    const int outputFrames = stretchedBuffer.getNumSamples();
    auto cache = std::make_shared<StretchedSliceBuffer>();
    cache->sliceId = slice.id;
    cache->sliceStartSample = slice.startSample;
    cache->sliceEndSample = slice.endSample;
    cache->playbackRate = quantizedRate;
    cache->pitchSemitones = quantizedPitch;
    cache->backend = sanitizeTimeStretchBackend(static_cast<int>(backend));
    cache->sourceSampleRate = sampleData.sourceSampleRate;
    cache->audioBuffer.setSize(2, outputFrames, false, false, true);

    float* cacheLeft = cache->audioBuffer.getWritePointer(0);
    float* cacheRight = cache->audioBuffer.getWritePointer(1);
    for (int frame = 0; frame < outputFrames; ++frame)
    {
        cacheLeft[frame] = stretchedBuffer.getSample(0, frame);
        cacheRight[frame] = stretchedBuffer.getSample(1, frame);
    }

    return std::const_pointer_cast<const StretchedSliceBuffer>(cache);
}

juce::String buildSliceLabel(int index)
{
    return "S" + juce::String(index + 1);
}

juce::String legacyLoopBarSelectionLabel(int selection)
{
    switch (selection)
    {
        case 25:  return "1/4";
        case 50:  return "1/2";
        case 100: return "1B";
        case 200: return "2B";
        case 400: return "4B";
        case 800: return "8B";
        default:  return "AUTO";
    }
}

float legacyLoopBarSelectionToBeats(int selection)
{
    switch (selection)
    {
        case 25:  return 1.0f;
        case 50:  return 2.0f;
        case 100: return 4.0f;
        case 200: return 8.0f;
        case 400: return 16.0f;
        case 800: return 32.0f;
        default:  return 0.0f;
    }
}

std::vector<int64_t> sanitizeBeatTickSamples(const std::vector<int64_t>& beatTickSamples,
                                             int64_t totalSamples)
{
    std::vector<int64_t> sanitized;
    sanitized.reserve(beatTickSamples.size());

    for (const auto sample : beatTickSamples)
    {
        if (sample < 0 || sample >= totalSamples)
            continue;
        if (!sanitized.empty() && sample <= sanitized.back())
            continue;
        sanitized.push_back(sample);
    }

    return sanitized;
}

int64_t computeMedianBeatTickIntervalWindow(const std::vector<int64_t>& beatTicks,
                                            int startIntervalIndex,
                                            int endIntervalIndexExclusive)
{
    if (beatTicks.size() < 2)
        return 0;

    const int intervalCount = static_cast<int>(beatTicks.size()) - 1;
    const int clampedStart = juce::jlimit(0, intervalCount, startIntervalIndex);
    const int clampedEnd = juce::jlimit(clampedStart + 1, intervalCount, endIntervalIndexExclusive);

    std::vector<int64_t> intervals;
    intervals.reserve(static_cast<size_t>(juce::jmax(0, clampedEnd - clampedStart)));
    for (int i = clampedStart; i < clampedEnd; ++i)
    {
        const int64_t interval = beatTicks[static_cast<size_t>(i + 1)] - beatTicks[static_cast<size_t>(i)];
        if (interval > 0)
            intervals.push_back(interval);
    }

    if (intervals.empty())
        return 0;

    std::sort(intervals.begin(), intervals.end());
    return intervals[intervals.size() / 2];
}

int64_t computeEdgeBeatTickInterval(const std::vector<int64_t>& beatTicks, bool useEndEdge)
{
    if (beatTicks.size() < 2)
        return 0;

    const int intervalCount = static_cast<int>(beatTicks.size()) - 1;
    const int windowSize = juce::jmin(4, intervalCount);
    const int startInterval = useEndEdge ? (intervalCount - windowSize) : 0;
    return computeMedianBeatTickIntervalWindow(beatTicks, startInterval, startInterval + windowSize);
}

int findNearestBeatTickIndex(const std::vector<int64_t>& beatTicks, int64_t targetSample)
{
    if (beatTicks.empty())
        return -1;

    auto it = std::lower_bound(beatTicks.begin(), beatTicks.end(), targetSample);
    if (it == beatTicks.begin())
        return 0;
    if (it == beatTicks.end())
        return static_cast<int>(beatTicks.size()) - 1;

    const int nextIndex = static_cast<int>(std::distance(beatTicks.begin(), it));
    const int prevIndex = nextIndex - 1;
    const int64_t nextDistance = std::abs(*it - targetSample);
    const int64_t prevDistance = std::abs(beatTicks[static_cast<size_t>(prevIndex)] - targetSample);
    return (prevDistance <= nextDistance) ? prevIndex : nextIndex;
}

bool computeBeatTickAlignedWindowRange(const std::vector<int64_t>& beatTicks,
                                       int64_t sourceLengthSamples,
                                       int64_t desiredStartSample,
                                       int beatCount,
                                       int64_t& rangeStartSample,
                                       int64_t& rangeEndSample)
{
    rangeStartSample = 0;
    rangeEndSample = 0;

    if (beatCount <= 0 || sourceLengthSamples <= 0 || beatTicks.size() < 2)
        return false;

    const int startIndex = findNearestBeatTickIndex(beatTicks, desiredStartSample);
    if (startIndex < 0)
        return false;

    const int64_t endEdgeInterval = computeEdgeBeatTickInterval(beatTicks, true);
    if (endEdgeInterval <= 0)
        return false;

    const int endIndex = startIndex + beatCount;
    rangeStartSample = beatTicks[static_cast<size_t>(startIndex)];

    if (endIndex < static_cast<int>(beatTicks.size()))
    {
        rangeEndSample = beatTicks[static_cast<size_t>(endIndex)];
    }
    else
    {
        const int extrapolatedBeats = endIndex - (static_cast<int>(beatTicks.size()) - 1);
        rangeEndSample = beatTicks.back() + (static_cast<int64_t>(extrapolatedBeats) * endEdgeInterval);
    }

    rangeStartSample = juce::jlimit<int64_t>(0, sourceLengthSamples - 1, rangeStartSample);
    rangeEndSample = juce::jlimit<int64_t>(rangeStartSample + 1,
                                           sourceLengthSamples,
                                           rangeEndSample);
    return rangeEndSample > rangeStartSample;
}

double computeBeatTickPosition(const std::vector<int64_t>& beatTicks,
                               int64_t samplePosition)
{
    if (beatTicks.size() < 2)
        return 0.0;

    const int64_t startEdgeInterval = computeEdgeBeatTickInterval(beatTicks, false);
    const int64_t endEdgeInterval = computeEdgeBeatTickInterval(beatTicks, true);
    if (startEdgeInterval <= 0 || endEdgeInterval <= 0)
        return 0.0;

    auto it = std::lower_bound(beatTicks.begin(), beatTicks.end(), samplePosition);
    if (it == beatTicks.begin())
        return static_cast<double>(samplePosition - beatTicks.front()) / static_cast<double>(startEdgeInterval);

    if (it == beatTicks.end())
    {
        const double beatsBeyondEnd = static_cast<double>(samplePosition - beatTicks.back())
            / static_cast<double>(endEdgeInterval);
        return static_cast<double>(beatTicks.size() - 1) + beatsBeyondEnd;
    }

    const int nextIndex = static_cast<int>(std::distance(beatTicks.begin(), it));
    const int prevIndex = nextIndex - 1;
    const int64_t start = beatTicks[static_cast<size_t>(prevIndex)];
    const int64_t end = beatTicks[static_cast<size_t>(nextIndex)];
    const int64_t interval = juce::jmax<int64_t>(1, end - start);
    const double alpha = static_cast<double>(samplePosition - start) / static_cast<double>(interval);
    return static_cast<double>(prevIndex) + juce::jlimit(0.0, 1.0, alpha);
}

int64_t samplePositionFromBeatTickPosition(const std::vector<int64_t>& beatTicks,
                                           double beatPosition,
                                           int64_t sourceLengthSamples)
{
    if (beatTicks.empty() || sourceLengthSamples <= 0 || !std::isfinite(beatPosition))
        return 0;

    const int64_t startEdgeInterval = computeEdgeBeatTickInterval(beatTicks, false);
    const int64_t endEdgeInterval = computeEdgeBeatTickInterval(beatTicks, true);
    if (startEdgeInterval <= 0 || endEdgeInterval <= 0)
        return juce::jlimit<int64_t>(0, sourceLengthSamples - 1, beatTicks.front());

    if (beatPosition <= 0.0)
    {
        const double sample = static_cast<double>(beatTicks.front())
            + (beatPosition * static_cast<double>(startEdgeInterval));
        return juce::jlimit<int64_t>(0, sourceLengthSamples - 1, static_cast<int64_t>(std::llround(sample)));
    }

    const int maxIndex = static_cast<int>(beatTicks.size()) - 1;
    if (beatPosition >= static_cast<double>(maxIndex))
    {
        const double sample = static_cast<double>(beatTicks.back())
            + ((beatPosition - static_cast<double>(maxIndex)) * static_cast<double>(endEdgeInterval));
        return juce::jlimit<int64_t>(0, sourceLengthSamples - 1, static_cast<int64_t>(std::llround(sample)));
    }

    const int lowerIndex = juce::jlimit(0, maxIndex - 1, static_cast<int>(std::floor(beatPosition)));
    const int upperIndex = juce::jlimit(lowerIndex + 1, maxIndex, lowerIndex + 1);
    const double alpha = juce::jlimit(0.0, 1.0, beatPosition - static_cast<double>(lowerIndex));
    const double start = static_cast<double>(beatTicks[static_cast<size_t>(lowerIndex)]);
    const double end = static_cast<double>(beatTicks[static_cast<size_t>(upperIndex)]);
    return juce::jlimit<int64_t>(0,
                                 sourceLengthSamples - 1,
                                 static_cast<int64_t>(std::llround(start + ((end - start) * alpha))));
}

float computeBeatSpanFromBeatTicks(const std::vector<int64_t>& beatTicks,
                                   int64_t rangeStartSample,
                                   int64_t rangeEndSample)
{
    if (beatTicks.size() < 2 || rangeEndSample <= rangeStartSample)
        return 0.0f;

    const double startBeat = computeBeatTickPosition(beatTicks, rangeStartSample);
    const double endBeat = computeBeatTickPosition(beatTicks, rangeEndSample);
    const double span = endBeat - startBeat;
    if (!(span > 0.0) || !std::isfinite(span))
        return 0.0f;

    return static_cast<float>(span);
}

float computeVisibleSliceBeatSpan(const LoadedSampleData& sampleData,
                                  const std::array<SampleSlice, SliceModel::VisibleSliceCount>& visibleSlices,
                                  double fallbackTempoBpm,
                                  int legacyLoopBarSelection)
{
    if (legacyLoopBarSelection > 0)
        return legacyLoopBarSelectionToBeats(legacyLoopBarSelection);

    int64_t rangeStartSample = std::numeric_limits<int64_t>::max();
    int64_t rangeEndSample = 0;
    for (const auto& slice : visibleSlices)
    {
        if (slice.id < 0 || slice.endSample <= slice.startSample)
            continue;
        rangeStartSample = juce::jmin(rangeStartSample, slice.startSample);
        rangeEndSample = juce::jmax(rangeEndSample, slice.endSample);
    }

    if (rangeStartSample == std::numeric_limits<int64_t>::max() || rangeEndSample <= rangeStartSample)
        return 0.0f;

    const auto beatTicks = sanitizeBeatTickSamples(sampleData.analysis.beatTickSamples,
                                                   sampleData.sourceLengthSamples);
    if (const auto tickBeatSpan = computeBeatSpanFromBeatTicks(beatTicks, rangeStartSample, rangeEndSample);
        tickBeatSpan > 0.0f)
    {
        return tickBeatSpan;
    }

    if (fallbackTempoBpm > 0.0 && std::isfinite(fallbackTempoBpm) && sampleData.sourceSampleRate > 0.0)
    {
        const double seconds = static_cast<double>(rangeEndSample - rangeStartSample)
            / sampleData.sourceSampleRate;
        const double beats = seconds * (fallbackTempoBpm / 60.0);
        if (beats > 0.0 && std::isfinite(beats))
            return static_cast<float>(beats);
    }

    return 0.0f;
}

float normalizedPositionFromSample(int64_t samplePosition, int64_t totalSamples)
{
    return safeNormalizedPosition(samplePosition, totalSamples);
}

double choosePpqGridBeatStep(double visibleBeatSpan, int pixelWidth, double minimumPixels)
{
    if (!(visibleBeatSpan > 0.0) || pixelWidth <= 0 || !(minimumPixels > 0.0))
        return 1.0;

    const double pixelsPerBeat = static_cast<double>(pixelWidth) / visibleBeatSpan;
    static constexpr std::array<double, 10> beatSteps { 0.25, 0.5, 1.0, 2.0, 4.0, 8.0, 16.0, 32.0, 64.0, 128.0 };
    for (const auto step : beatSteps)
    {
        if ((step * pixelsPerBeat) >= minimumPixels)
            return step;
    }

    return beatSteps.back();
}

bool beatGridMatchesDivision(double beatPosition, double divisionBeats)
{
    if (!(divisionBeats > 0.0) || !std::isfinite(beatPosition))
        return false;

    const double nearestDivision = std::round(beatPosition / divisionBeats) * divisionBeats;
    return std::abs(beatPosition - nearestDivision) <= 1.0e-4;
}

std::optional<juce::Range<float>> computeSampleLoopVisualRange(const SampleModeEngine::StateSnapshot& snapshot)
{
    if (!snapshot.hasSample || snapshot.totalSamples <= 0)
        return std::nullopt;

    if (snapshot.useLegacyLoopEngine || snapshot.legacyLoopBarSelection > 0)
    {
        int64_t rangeStart = std::numeric_limits<int64_t>::max();
        int64_t rangeEnd = 0;
        for (const auto& slice : snapshot.visibleSlices)
        {
            if (slice.id < 0 || slice.endSample <= slice.startSample)
                continue;

            rangeStart = juce::jmin(rangeStart, slice.startSample);
            rangeEnd = juce::jmax(rangeEnd, slice.endSample);
        }

        if (rangeStart == std::numeric_limits<int64_t>::max() || rangeEnd <= rangeStart)
            return std::nullopt;

        if (snapshot.legacyLoopBarSelection > 0
            && snapshot.sourceSampleRate > 0.0
            && snapshot.estimatedTempoBpm > 0.0)
        {
            float beatsForLoop = 0.0f;
            switch (snapshot.legacyLoopBarSelection)
            {
                case 25:  beatsForLoop = 1.0f; break;
                case 50:  beatsForLoop = 2.0f; break;
                case 100: beatsForLoop = 4.0f; break;
                case 200: beatsForLoop = 8.0f; break;
                case 400: beatsForLoop = 16.0f; break;
                case 800: beatsForLoop = 32.0f; break;
                default: break;
            }

            if (beatsForLoop > 0.0f)
            {
                const double samplesPerBeat = (60.0 / snapshot.estimatedTempoBpm) * snapshot.sourceSampleRate;
                const int64_t targetLength = static_cast<int64_t>(std::llround(static_cast<double>(beatsForLoop) * samplesPerBeat));
                if (targetLength > 0)
                    rangeEnd = juce::jlimit<int64_t>(rangeStart + 1, snapshot.totalSamples, rangeStart + targetLength);
            }
        }

        return juce::Range<float>(normalizedPositionFromSample(rangeStart, snapshot.totalSamples),
                                  normalizedPositionFromSample(rangeEnd, snapshot.totalSamples));
    }

    if (snapshot.triggerMode != SampleTriggerMode::Loop)
        return std::nullopt;

    int slot = snapshot.activeVisibleSliceSlot;
    if (slot < 0)
        slot = snapshot.pendingVisibleSliceSlot;
    if (slot < 0 && snapshot.playbackProgress >= 0.0f)
    {
        for (int i = 0; i < SliceModel::VisibleSliceCount; ++i)
        {
            const auto& slice = snapshot.visibleSlices[static_cast<size_t>(i)];
            if (slice.id < 0 || slice.endSample <= slice.startSample)
                continue;

            const bool isLastSlice = (i == (SliceModel::VisibleSliceCount - 1));
            if (snapshot.playbackProgress >= slice.normalizedStart
                && (snapshot.playbackProgress < slice.normalizedEnd
                    || (isLastSlice && snapshot.playbackProgress <= slice.normalizedEnd)))
            {
                slot = i;
                break;
            }
        }
    }

    if (slot < 0 || slot >= SliceModel::VisibleSliceCount)
        return std::nullopt;

    const auto& slice = snapshot.visibleSlices[static_cast<size_t>(slot)];
    if (slice.id < 0 || slice.endSample <= slice.startSample)
        return std::nullopt;

    return juce::Range<float>(slice.normalizedStart, slice.normalizedEnd);
}
} // namespace

juce::ValueTree SampleModePersistentState::createValueTree(const juce::Identifier& type) const
{
    juce::ValueTree tree(type);
    tree.setProperty("samplePath", samplePath, nullptr);
    tree.setProperty("visibleSliceBankIndex", visibleSliceBankIndex, nullptr);
    tree.setProperty("sliceMode", sampleSliceModeName(sliceMode), nullptr);
    tree.setProperty("triggerMode", sampleTriggerModeName(triggerMode), nullptr);
    tree.setProperty("useLegacyLoopEngine", useLegacyLoopEngine, nullptr);
    tree.setProperty("legacyLoopBarSelection", legacyLoopBarSelection, nullptr);
    tree.setProperty("beatDivision", beatDivision, nullptr);
    tree.setProperty("viewZoom", viewZoom, nullptr);
    tree.setProperty("viewScroll", viewScroll, nullptr);
    tree.setProperty("selectedCueIndex", selectedCueIndex, nullptr);
    tree.setProperty("analyzedTempoBpm", analyzedTempoBpm, nullptr);
    tree.setProperty("analyzedPitchHz", analyzedPitchHz, nullptr);
    tree.setProperty("analyzedPitchMidi", analyzedPitchMidi, nullptr);
    tree.setProperty("essentiaUsed", essentiaUsed, nullptr);
    tree.setProperty("analysisSource", analysisSource, nullptr);
    tree.setProperty("transientMarkersEdited", transientMarkersEdited, nullptr);

    juce::ValueTree cuesNode("CuePoints");
    for (const auto& cue : cuePoints)
    {
        juce::ValueTree cueNode("Cue");
        cueNode.setProperty("id", cue.id, nullptr);
        cueNode.setProperty("samplePosition", static_cast<juce::int64>(cue.samplePosition), nullptr);
        cueNode.setProperty("name", cue.name, nullptr);
        cueNode.setProperty("loopEnabled", cue.loopEnabled, nullptr);
        cueNode.setProperty("loopEndSample", static_cast<juce::int64>(cue.loopEndSample), nullptr);
        cuesNode.addChild(cueNode, -1, nullptr);
    }
    tree.addChild(cuesNode, -1, nullptr);

    juce::ValueTree warpNode("WarpMarkers");
    for (const auto& marker : warpMarkers)
    {
        juce::ValueTree markerNode("WarpMarker");
        markerNode.setProperty("id", marker.id, nullptr);
        markerNode.setProperty("samplePosition", static_cast<juce::int64>(marker.samplePosition), nullptr);
        markerNode.setProperty("beatPosition", marker.beatPosition, nullptr);
        warpNode.addChild(markerNode, -1, nullptr);
    }
    tree.addChild(warpNode, -1, nullptr);

    juce::ValueTree transientNode("TransientMarkers");
    for (const auto sample : transientEditSamples)
    {
        juce::ValueTree markerNode("Marker");
        markerNode.setProperty("samplePosition", static_cast<juce::int64>(sample), nullptr);
        transientNode.addChild(markerNode, -1, nullptr);
    }
    tree.addChild(transientNode, -1, nullptr);

    juce::ValueTree slicesNode("Slices");
    for (const auto& slice : storedSlices)
    {
        juce::ValueTree sliceNode("Slice");
        sliceNode.setProperty("id", slice.id, nullptr);
        sliceNode.setProperty("startSample", static_cast<juce::int64>(slice.startSample), nullptr);
        sliceNode.setProperty("endSample", static_cast<juce::int64>(slice.endSample), nullptr);
        sliceNode.setProperty("label", slice.label, nullptr);
        sliceNode.setProperty("transientDerived", slice.transientDerived, nullptr);
        sliceNode.setProperty("manualDerived", slice.manualDerived, nullptr);
        slicesNode.addChild(sliceNode, -1, nullptr);
    }
    tree.addChild(slicesNode, -1, nullptr);

    return tree;
}

SampleModePersistentState SampleModePersistentState::fromValueTree(const juce::ValueTree& state)
{
    SampleModePersistentState persistentState;
    if (!state.isValid())
        return persistentState;

    persistentState.samplePath = state.getProperty("samplePath").toString();
    persistentState.visibleSliceBankIndex = juce::jmax(0, static_cast<int>(state.getProperty("visibleSliceBankIndex", 0)));
    persistentState.sliceMode = sampleSliceModeFromString(state.getProperty("sliceMode", "transient").toString());
    persistentState.triggerMode = sampleTriggerModeFromString(state.getProperty("triggerMode", "oneshot").toString());
    persistentState.useLegacyLoopEngine = static_cast<bool>(state.getProperty("useLegacyLoopEngine", false));
    persistentState.legacyLoopBarSelection = static_cast<int>(state.getProperty("legacyLoopBarSelection", 0));
    persistentState.beatDivision = juce::jlimit(1, 8, static_cast<int>(state.getProperty("beatDivision", 1)));
    persistentState.viewZoom = juce::jlimit(kMinViewZoom, kMaxViewZoom, static_cast<float>(state.getProperty("viewZoom", 1.0f)));
    persistentState.viewScroll = juce::jlimit(0.0f, 1.0f, static_cast<float>(state.getProperty("viewScroll", 0.0f)));
    persistentState.selectedCueIndex = static_cast<int>(state.getProperty("selectedCueIndex", -1));
    persistentState.analyzedTempoBpm = static_cast<double>(state.getProperty("analyzedTempoBpm", 0.0));
    persistentState.analyzedPitchHz = static_cast<double>(state.getProperty("analyzedPitchHz", 0.0));
    persistentState.analyzedPitchMidi = static_cast<int>(state.getProperty("analyzedPitchMidi", -1));
    persistentState.essentiaUsed = static_cast<bool>(state.getProperty("essentiaUsed", false));
    persistentState.analysisSource = state.getProperty("analysisSource").toString();
    persistentState.transientMarkersEdited = static_cast<bool>(state.getProperty("transientMarkersEdited", false));

    if (auto cuesNode = state.getChildWithName("CuePoints"); cuesNode.isValid())
    {
        for (auto cueNode : cuesNode)
        {
            if (!cueNode.hasType("Cue"))
                continue;

            SampleCuePoint cue;
            cue.id = static_cast<int>(cueNode.getProperty("id", -1));
            cue.samplePosition = static_cast<int64_t>(static_cast<juce::int64>(cueNode.getProperty("samplePosition", 0)));
            cue.name = cueNode.getProperty("name").toString();
            cue.loopEnabled = static_cast<bool>(cueNode.getProperty("loopEnabled", false));
            cue.loopEndSample = static_cast<int64_t>(static_cast<juce::int64>(cueNode.getProperty("loopEndSample", cue.samplePosition)));
            persistentState.cuePoints.push_back(std::move(cue));
        }
    }

    if (auto warpNode = state.getChildWithName("WarpMarkers"); warpNode.isValid())
    {
        for (auto markerNode : warpNode)
        {
            if (!markerNode.hasType("WarpMarker"))
                continue;

            SampleWarpMarker marker;
            marker.id = static_cast<int>(markerNode.getProperty("id", -1));
            marker.samplePosition = static_cast<int64_t>(static_cast<juce::int64>(markerNode.getProperty("samplePosition", 0)));
            marker.beatPosition = static_cast<double>(markerNode.getProperty("beatPosition", 0.0));
            persistentState.warpMarkers.push_back(std::move(marker));
        }
    }

    if (auto transientNode = state.getChildWithName("TransientMarkers"); transientNode.isValid())
    {
        for (auto markerNode : transientNode)
        {
            if (!markerNode.hasType("Marker"))
                continue;

            persistentState.transientEditSamples.push_back(
                static_cast<int64_t>(static_cast<juce::int64>(markerNode.getProperty("samplePosition", 0))));
        }
    }

    if (auto slicesNode = state.getChildWithName("Slices"); slicesNode.isValid())
    {
        for (auto sliceNode : slicesNode)
        {
            if (!sliceNode.hasType("Slice"))
                continue;

            SampleSlice slice;
            slice.id = static_cast<int>(sliceNode.getProperty("id", -1));
            slice.startSample = static_cast<int64_t>(static_cast<juce::int64>(sliceNode.getProperty("startSample", 0)));
            slice.endSample = static_cast<int64_t>(static_cast<juce::int64>(sliceNode.getProperty("endSample", 0)));
            slice.label = sliceNode.getProperty("label").toString();
            slice.transientDerived = static_cast<bool>(sliceNode.getProperty("transientDerived", false));
            slice.manualDerived = static_cast<bool>(sliceNode.getProperty("manualDerived", false));
            persistentState.storedSlices.push_back(std::move(slice));
        }
    }

    return persistentState;
}

std::unique_ptr<juce::XmlElement> SampleModePersistentState::createXml(const juce::String& tagName) const
{
    auto xml = std::make_unique<juce::XmlElement>(tagName);
    xml->setAttribute("samplePath", samplePath);
    xml->setAttribute("visibleSliceBankIndex", visibleSliceBankIndex);
    xml->setAttribute("sliceMode", sampleSliceModeName(sliceMode));
    xml->setAttribute("triggerMode", sampleTriggerModeName(triggerMode));
    xml->setAttribute("useLegacyLoopEngine", useLegacyLoopEngine);
    xml->setAttribute("legacyLoopBarSelection", legacyLoopBarSelection);
    xml->setAttribute("beatDivision", beatDivision);
    xml->setAttribute("viewZoom", viewZoom);
    xml->setAttribute("viewScroll", viewScroll);
    xml->setAttribute("selectedCueIndex", selectedCueIndex);
    xml->setAttribute("analyzedTempoBpm", analyzedTempoBpm);
    xml->setAttribute("analyzedPitchHz", analyzedPitchHz);
    xml->setAttribute("analyzedPitchMidi", analyzedPitchMidi);
    xml->setAttribute("essentiaUsed", essentiaUsed);
    xml->setAttribute("analysisSource", analysisSource);
    xml->setAttribute("transientMarkersEdited", transientMarkersEdited);

    for (const auto& cue : cuePoints)
    {
        auto* cueXml = xml->createNewChildElement("Cue");
        cueXml->setAttribute("id", cue.id);
        cueXml->setAttribute("samplePosition", juce::String(static_cast<juce::int64>(cue.samplePosition)));
        cueXml->setAttribute("name", cue.name);
        cueXml->setAttribute("loopEnabled", cue.loopEnabled);
        cueXml->setAttribute("loopEndSample", juce::String(static_cast<juce::int64>(cue.loopEndSample)));
    }

    auto* warpXml = xml->createNewChildElement("WarpMarkers");
    for (const auto& marker : warpMarkers)
    {
        auto* markerXml = warpXml->createNewChildElement("WarpMarker");
        markerXml->setAttribute("id", marker.id);
        markerXml->setAttribute("samplePosition", juce::String(static_cast<juce::int64>(marker.samplePosition)));
        markerXml->setAttribute("beatPosition", marker.beatPosition);
    }

    auto* transientXml = xml->createNewChildElement("TransientMarkers");
    for (const auto marker : transientEditSamples)
    {
        auto* markerXml = transientXml->createNewChildElement("Marker");
        markerXml->setAttribute("samplePosition", juce::String(static_cast<juce::int64>(marker)));
    }

    for (const auto& slice : storedSlices)
    {
        auto* sliceXml = xml->createNewChildElement("Slice");
        sliceXml->setAttribute("id", slice.id);
        sliceXml->setAttribute("startSample", juce::String(static_cast<juce::int64>(slice.startSample)));
        sliceXml->setAttribute("endSample", juce::String(static_cast<juce::int64>(slice.endSample)));
        sliceXml->setAttribute("label", slice.label);
        sliceXml->setAttribute("transientDerived", slice.transientDerived);
        sliceXml->setAttribute("manualDerived", slice.manualDerived);
    }

    return xml;
}

SampleModePersistentState SampleModePersistentState::fromXml(const juce::XmlElement& xml)
{
    return fromValueTree(juce::ValueTree::fromXml(xml));
}

void SliceModel::clear()
{
    slices.clear();
    visibleBankIndex = 0;
}

void SliceModel::setSlices(std::vector<SampleSlice> newSlices)
{
    slices = std::move(newSlices);
    visibleBankIndex = juce::jlimit(0, juce::jmax(0, getVisibleBankCount() - 1), visibleBankIndex);
}

void SliceModel::setVisibleBankIndex(int bankIndex)
{
    visibleBankIndex = juce::jlimit(0, juce::jmax(0, getVisibleBankCount() - 1), bankIndex);
}

int SliceModel::getVisibleBankCount() const noexcept
{
    if (slices.empty())
        return 0;

    return (static_cast<int>(slices.size()) + VisibleSliceCount - 1) / VisibleSliceCount;
}

bool SliceModel::canNavigateLeft() const noexcept
{
    return visibleBankIndex > 0;
}

bool SliceModel::canNavigateRight() const noexcept
{
    return visibleBankIndex + 1 < getVisibleBankCount();
}

void SliceModel::stepVisibleBank(int delta)
{
    setVisibleBankIndex(visibleBankIndex + delta);
}

std::array<SampleSlice, SliceModel::VisibleSliceCount> SliceModel::getVisibleSlices() const
{
    std::array<SampleSlice, VisibleSliceCount> visibleSlices {};

    const int startIndex = visibleBankIndex * VisibleSliceCount;
    for (int slot = 0; slot < VisibleSliceCount; ++slot)
    {
        const int sliceIndex = startIndex + slot;
        if (sliceIndex >= 0 && sliceIndex < static_cast<int>(slices.size()))
            visibleSlices[static_cast<size_t>(slot)] = slices[static_cast<size_t>(sliceIndex)];
        else
            visibleSlices[static_cast<size_t>(slot)].label = buildSliceLabel(slot);
    }

    return visibleSlices;
}

std::vector<SampleSlice> SliceModel::buildUniformSlices(const LoadedSampleData& sampleData,
                                                        int totalSliceCount)
{
    std::vector<SampleSlice> slices;

    const auto totalSamples = sampleData.sourceLengthSamples;
    if (totalSamples <= 0)
        return slices;

    const int sliceCount = juce::jmax(1, totalSliceCount);
    slices.reserve(static_cast<size_t>(sliceCount));

    for (int index = 0; index < sliceCount; ++index)
    {
        SampleSlice slice;
        slice.startSample = (totalSamples * index) / sliceCount;
        slice.endSample = (totalSamples * (index + 1)) / sliceCount;
        if (slice.endSample <= slice.startSample)
            slice.endSample = juce::jmin(totalSamples, slice.startSample + 1);

        fillSliceMetadata(slice, index, totalSamples);
        slices.push_back(std::move(slice));
    }

    return slices;
}

std::vector<SampleSlice> SliceModel::buildBeatSlices(const LoadedSampleData& sampleData,
                                                     double tempoBpm,
                                                     int beatDivision,
                                                     const std::vector<int64_t>& beatTickSamples)
{
    if (sampleData.sourceLengthSamples <= 0 || sampleData.sourceSampleRate <= 0.0 || !(tempoBpm > 0.0))
        return {};

    const int safeBeatDivision = juce::jlimit(1, 8, beatDivision);
    std::vector<SampleSlice> slices;

    if (beatTickSamples.size() >= 2)
    {
        std::vector<int64_t> anchors;
        anchors.reserve(beatTickSamples.size() + 4);
        for (const auto sample : beatTickSamples)
        {
            if (sample < 0 || sample >= sampleData.sourceLengthSamples)
                continue;
            if (!anchors.empty() && sample <= anchors.back())
                continue;
            anchors.push_back(sample);
        }

        if (anchors.size() >= 2)
        {
            std::vector<int64_t> intervals;
            intervals.reserve(anchors.size() - 1);
            for (size_t i = 1; i < anchors.size(); ++i)
                intervals.push_back(anchors[i] - anchors[i - 1]);
            std::sort(intervals.begin(), intervals.end());
            const int64_t medianInterval = intervals[intervals.size() / 2];

            while (!anchors.empty() && anchors.front() > medianInterval / 2)
                anchors.insert(anchors.begin(), juce::jmax<int64_t>(0, anchors.front() - medianInterval));
            while (!anchors.empty() && anchors.back() < sampleData.sourceLengthSamples)
                anchors.push_back(anchors.back() + medianInterval);

            for (size_t beatIndex = 0; beatIndex + 1 < anchors.size(); ++beatIndex)
            {
                const auto beatStart = juce::jlimit<int64_t>(0, sampleData.sourceLengthSamples, anchors[beatIndex]);
                const auto beatEnd = juce::jlimit<int64_t>(0, sampleData.sourceLengthSamples, anchors[beatIndex + 1]);
                if (beatEnd <= beatStart)
                    continue;

                for (int divisionIndex = 0; divisionIndex < safeBeatDivision; ++divisionIndex)
                {
                    const double startAlpha = static_cast<double>(divisionIndex) / static_cast<double>(safeBeatDivision);
                    const double endAlpha = static_cast<double>(divisionIndex + 1) / static_cast<double>(safeBeatDivision);
                    SampleSlice slice;
                    slice.startSample = juce::jlimit<int64_t>(
                        0, sampleData.sourceLengthSamples,
                        beatStart + static_cast<int64_t>(std::llround((beatEnd - beatStart) * startAlpha)));
                    slice.endSample = juce::jlimit<int64_t>(
                        0, sampleData.sourceLengthSamples,
                        beatStart + static_cast<int64_t>(std::llround((beatEnd - beatStart) * endAlpha)));
                    if (slice.endSample <= slice.startSample)
                        slice.endSample = juce::jmin<int64_t>(sampleData.sourceLengthSamples, slice.startSample + 1);
                    slices.push_back(std::move(slice));
                    if (static_cast<int>(slices.size()) >= kMaxInitialSliceCount)
                        break;
                }

                if (static_cast<int>(slices.size()) >= kMaxInitialSliceCount)
                    break;
            }
        }
    }

    if (slices.empty())
    {
        const double beatSeconds = 60.0 / tempoBpm;
        const double sliceSeconds = beatSeconds / static_cast<double>(safeBeatDivision);
        const int64_t sliceSamples = static_cast<int64_t>(std::round(sliceSeconds * sampleData.sourceSampleRate));
        if (sliceSamples <= 0)
            return {};

        const int totalSliceCount = juce::jlimit(SliceModel::VisibleSliceCount,
                                                 kMaxInitialSliceCount,
                                                 juce::jmax(SliceModel::VisibleSliceCount,
                                                            static_cast<int>(std::ceil(sampleData.sourceLengthSamples / static_cast<double>(sliceSamples)))));
        slices.reserve(static_cast<size_t>(totalSliceCount));

        for (int index = 0; index < totalSliceCount; ++index)
        {
            SampleSlice slice;
            slice.startSample = juce::jmin<int64_t>(sampleData.sourceLengthSamples, static_cast<int64_t>(index) * sliceSamples);
            slice.endSample = juce::jmin<int64_t>(sampleData.sourceLengthSamples, slice.startSample + sliceSamples);
            if (slice.endSample <= slice.startSample)
                slice.endSample = juce::jmin<int64_t>(sampleData.sourceLengthSamples, slice.startSample + 1);
            slices.push_back(std::move(slice));
        }
    }

    for (size_t index = 0; index < slices.size(); ++index)
        fillSliceMetadata(slices[index], static_cast<int>(index), sampleData.sourceLengthSamples);

    return slices;
}

std::vector<SampleSlice> SliceModel::buildTransientSlices(const LoadedSampleData& sampleData,
                                                          const std::vector<int64_t>& transientSamples)
{
    if (sampleData.sourceLengthSamples <= 0)
        return {};

    if (transientSamples.empty())
        return buildUniformSlices(sampleData, SliceModel::VisibleSliceCount);

    std::vector<int64_t> starts;
    starts.reserve(transientSamples.size());
    for (auto samplePosition : transientSamples)
    {
        if (samplePosition < 0 || samplePosition >= sampleData.sourceLengthSamples)
            continue;

        if (!starts.empty() && samplePosition <= starts.back())
            continue;

        starts.push_back(samplePosition);
        if (static_cast<int>(starts.size()) >= kMaxInitialSliceCount)
            break;
    }

    std::vector<SampleSlice> slices;
    slices.reserve(starts.size());
    for (size_t index = 0; index < starts.size(); ++index)
    {
        SampleSlice slice;
        slice.startSample = starts[index];
        slice.endSample = (index + 1 < starts.size())
            ? starts[index + 1]
            : sampleData.sourceLengthSamples;
        if (slice.endSample <= slice.startSample)
            continue;

        slice.transientDerived = true;
        fillSliceMetadata(slice, static_cast<int>(index), sampleData.sourceLengthSamples);
        slices.push_back(std::move(slice));
    }

    return slices.empty()
        ? buildUniformSlices(sampleData, SliceModel::VisibleSliceCount)
        : slices;
}

std::vector<SampleSlice> SliceModel::buildManualSlices(const LoadedSampleData& sampleData,
                                                       const std::vector<SampleCuePoint>& cuePoints)
{
    if (sampleData.sourceLengthSamples <= 0)
        return {};

    if (cuePoints.empty())
        return buildUniformSlices(sampleData, SliceModel::VisibleSliceCount);

    std::vector<int64_t> starts;
    starts.reserve(cuePoints.size() + 1);
    starts.push_back(0);

    for (const auto& cue : cuePoints)
    {
        if (cue.samplePosition <= 0 || cue.samplePosition >= sampleData.sourceLengthSamples)
            continue;
        starts.push_back(cue.samplePosition);
    }

    std::sort(starts.begin(), starts.end());
    starts.erase(std::unique(starts.begin(), starts.end()), starts.end());

    std::vector<SampleSlice> slices;
    slices.reserve(starts.size());
    for (size_t index = 0; index < starts.size(); ++index)
    {
        SampleSlice slice;
        slice.startSample = starts[index];
        slice.endSample = (index + 1 < starts.size())
            ? starts[index + 1]
            : sampleData.sourceLengthSamples;
        if (slice.endSample <= slice.startSample)
            continue;

        slice.manualDerived = true;
        fillSliceMetadata(slice, static_cast<int>(index), sampleData.sourceLengthSamples);
        slices.push_back(std::move(slice));
    }

    return slices.empty()
        ? buildUniformSlices(sampleData, SliceModel::VisibleSliceCount)
        : slices;
}

SampleAnalysisSummary SampleAnalysisEngine::analyzeLoadedSample(const juce::File& file,
                                                               const juce::AudioBuffer<float>& audioBuffer,
                                                               double sourceSampleRate,
                                                               SamplePitchAnalysisProfile pitchProfile,
                                                               ProgressCallback progressCallback) const
{
    LoadedSampleData sampleData;
    sampleData.audioBuffer.makeCopyOf(audioBuffer, true);
    sampleData.sourceSampleRate = sourceSampleRate > 0.0 ? sourceSampleRate : 44100.0;
    sampleData.numChannels = sampleData.audioBuffer.getNumChannels();
    sampleData.sourceLengthSamples = sampleData.audioBuffer.getNumSamples();
    sampleData.durationSeconds = static_cast<double>(sampleData.sourceLengthSamples)
        / juce::jmax(1.0, sampleData.sourceSampleRate);

    if (file.existsAsFile())
        enrichAnalysisForFile(file, sampleData, pitchProfile, std::move(progressCallback));
    else
        sampleData.analysis = buildInternalAnalysis(sampleData);

    return sampleData.analysis;
}

void SampleAnalysisEngine::enrichAnalysisForFile(const juce::File& file,
                                                 LoadedSampleData& sampleData,
                                                 SamplePitchAnalysisProfile pitchProfile,
                                                 ProgressCallback progressCallback) const
{
    juce::ignoreUnused(file);
    juce::String essentiaFailureReason;
    if (progressCallback)
        progressCallback(0.62f, "Preparing Essentia...");

    if (progressCallback)
    {
        const char* progressLabel = "Essentia pitch...";
        if (pitchProfile == SamplePitchAnalysisProfile::Polyphonic)
            progressLabel = "Essentia key...";
#if MLRVST_ENABLE_LIBPYIN
        else
            progressLabel = "pYIN pitch...";
#endif
        progressCallback(0.70f, progressLabel);
    }

    std::optional<SampleAnalysisSummary> essentiaSummary;
#if MLRVST_ENABLE_ESSENTIA_NATIVE
    essentiaSummary = runEssentiaAnalysis(sampleData, pitchProfile, &essentiaFailureReason);
#endif

    if (progressCallback)
        progressCallback(0.82f, "Analyzing transients...");
    const auto internalSummary = buildInternalAnalysis(sampleData);

    if (essentiaSummary)
    {
        sampleData.analysis = internalSummary;
        sampleData.analysis.essentiaUsed = true;
        sampleData.analysis.beatTickSamples = sanitizeBeatTickSamples(essentiaSummary->beatTickSamples,
                                                                      sampleData.sourceLengthSamples);
        if (progressCallback)
            progressCallback(0.9f, "Finalizing Essentia ticks...");

        if (essentiaSummary->estimatedTempoBpm > 0.0)
            sampleData.analysis.estimatedTempoBpm = essentiaSummary->estimatedTempoBpm;

        if (essentiaSummary->estimatedPitchMidi >= 0)
        {
            sampleData.analysis.estimatedPitchHz = essentiaSummary->estimatedPitchHz;
            sampleData.analysis.estimatedPitchMidi = essentiaSummary->estimatedPitchMidi;
            sampleData.analysis.analysisSource = sampleData.analysis.estimatedTempoBpm > 0.0
                ? essentiaSummary->analysisSource + " + native ticks + internal transients"
                : "essentia native pitch + internal transients";
        }
        else
        {
            sampleData.analysis.analysisSource = sampleData.analysis.estimatedTempoBpm > 0.0
                ? essentiaSummary->analysisSource + " + native ticks + internal pitch/transients"
                : "ES pitch unresolved";
        }

        if (!(sampleData.analysis.estimatedTempoBpm > 0.0))
            sampleData.analysis.estimatedTempoBpm = internalSummary.estimatedTempoBpm;
        if (sampleData.analysis.estimatedPitchMidi < 0 && internalSummary.estimatedPitchMidi >= 0)
        {
            sampleData.analysis.estimatedPitchHz = internalSummary.estimatedPitchHz;
            sampleData.analysis.estimatedPitchMidi = internalSummary.estimatedPitchMidi;
        }
        return;
    }

    if (progressCallback)
        progressCallback(0.86f, "Internal analysis fallback...");
    sampleData.analysis = internalSummary;
    sampleData.analysis.essentiaUsed = false;
    sampleData.analysis.analysisSource = essentiaFailureReason.isNotEmpty()
        ? essentiaFailureReason
        : "internal";
}

std::vector<SampleSlice> SampleAnalysisEngine::buildSlicesForState(const LoadedSampleData& sampleData,
                                                                   const SampleModePersistentState& state) const
{
    switch (state.sliceMode)
    {
        case SampleSliceMode::Manual:
            return SliceModel::buildManualSlices(sampleData, state.cuePoints);
        case SampleSliceMode::Transient:
            return SliceModel::buildTransientSlices(sampleData,
                                                    buildCanonicalTransientMarkerSamples(sampleData,
                                                                                         state.transientEditSamples,
                                                                                         state.transientMarkersEdited));
        case SampleSliceMode::Beat:
            return SliceModel::buildBeatSlices(sampleData,
                                               state.analyzedTempoBpm > 0.0
                                                   ? state.analyzedTempoBpm
                                                   : sampleData.analysis.estimatedTempoBpm,
                                               state.beatDivision,
                                               sampleData.analysis.beatTickSamples);
        case SampleSliceMode::Uniform:
        default:
            break;
    }

    const auto estimatedSliceCount = static_cast<int>(std::ceil(sampleData.durationSeconds / kInitialSliceTargetSeconds));
    const int totalSliceCount = juce::jlimit(SliceModel::VisibleSliceCount,
                                             kMaxInitialSliceCount,
                                             juce::jmax(SliceModel::VisibleSliceCount, estimatedSliceCount));

    return SliceModel::buildUniformSlices(sampleData, totalSliceCount);
}

class SampleFileManager::LoadJob : public juce::ThreadPoolJob
{
public:
    LoadJob(SampleFileManager& ownerToUse,
            juce::File fileToLoad,
            int requestIdToUse,
            LoadCallback callbackToUse,
            ProgressCallback progressCallbackToUse)
        : juce::ThreadPoolJob("SampleModeLoadJob")
        , owner(ownerToUse)
        , file(std::move(fileToLoad))
        , requestId(requestIdToUse)
        , callback(std::move(callbackToUse))
        , progressCallback(std::move(progressCallbackToUse))
    {
    }

    JobStatus runJob() override
    {
        auto progressCallbackCopy = progressCallback;
        auto postProgress = [requestIdValue = requestId, progressCallbackCopy](float progress, const juce::String& statusText)
        {
            if (!progressCallbackCopy)
                return;

            juce::MessageManager::callAsync([progressCallbackCopy, requestIdValue, progress, statusText]() mutable
            {
                progressCallbackCopy(requestIdValue, progress, statusText);
            });
        };

        postProgress(0.12f, "Decoding " + file.getFileName() + "...");
        auto decodeResult = owner.decodeFile(file, requestId, postProgress);

        if (shouldExit())
            return jobHasFinished;

        auto callbackCopy = callback;
        juce::MessageManager::callAsync([mainThreadCallback = std::move(callbackCopy),
                                         asyncResult = std::move(decodeResult)]() mutable
        {
            if (mainThreadCallback)
                mainThreadCallback(std::move(asyncResult));
        });

        return jobHasFinished;
    }

private:
    SampleFileManager& owner;
    juce::File file;
    int requestId = 0;
    LoadCallback callback;
    ProgressCallback progressCallback;
};

SampleFileManager::SampleFileManager()
{
    formatManager.registerBasicFormats();
}

SampleFileManager::~SampleFileManager()
{
    cancelPendingLoads();
}

int SampleFileManager::loadFileAsync(const juce::File& file, LoadCallback callback, ProgressCallback progressCallback)
{
    const int requestId = nextRequestId.fetch_add(1);
    threadPool.addJob(new LoadJob(*this,
                                  file,
                                  requestId,
                                  std::move(callback),
                                  std::move(progressCallback)),
                      true);
    return requestId;
}

void SampleFileManager::cancelPendingLoads()
{
    threadPool.removeAllJobs(true, 10000);
}

SampleFileManager::LoadResult SampleFileManager::decodeFile(const juce::File& file,
                                                            int requestId,
                                                            const std::function<void(float progress, const juce::String& statusText)>& progressCallback)
{
    LoadResult result;
    result.sourceFile = file;
    result.requestId = requestId;

    if (!file.existsAsFile())
    {
        result.errorMessage = "File does not exist";
        return result;
    }

    std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(file));
    if (reader == nullptr)
    {
        result.errorMessage = "Unsupported or unreadable audio file";
        return result;
    }

    if (reader->lengthInSamples <= 0)
    {
        result.errorMessage = "Audio file is empty";
        return result;
    }

    if (reader->lengthInSamples > std::numeric_limits<int>::max())
    {
        result.errorMessage = "Audio file is too large for the current Flip mode MVP";
        return result;
    }

    const int numSamples = static_cast<int>(reader->lengthInSamples);
    const int sourceChannelCount = juce::jmax(1, static_cast<int>(reader->numChannels));
    const int decodeChannelCount = juce::jmin(2, sourceChannelCount);

    if (progressCallback)
        progressCallback(0.28f, "Decoding audio...");

    juce::AudioBuffer<float> sourceBuffer(decodeChannelCount, numSamples);
    sourceBuffer.clear();

    if (!reader->read(&sourceBuffer, 0, numSamples, 0, true, decodeChannelCount > 1))
    {
        result.errorMessage = "Failed to decode audio file";
        return result;
    }

    auto loadedSample = std::make_shared<LoadedSampleData>();
    loadedSample->audioBuffer.setSize(2, numSamples, false, false, true);
    loadedSample->audioBuffer.clear();

    if (decodeChannelCount == 1)
    {
        loadedSample->audioBuffer.copyFrom(0, 0, sourceBuffer, 0, 0, numSamples);
        loadedSample->audioBuffer.copyFrom(1, 0, sourceBuffer, 0, 0, numSamples);
    }
    else
    {
        loadedSample->audioBuffer.copyFrom(0, 0, sourceBuffer, 0, 0, numSamples);
        loadedSample->audioBuffer.copyFrom(1, 0, sourceBuffer, 1, 0, numSamples);
    }

    loadedSample->sourcePath = file.getFullPathName();
    loadedSample->displayName = file.getFileNameWithoutExtension();
    loadedSample->sourceSampleRate = reader->sampleRate;
    loadedSample->numChannels = decodeChannelCount;
    loadedSample->sourceLengthSamples = reader->lengthInSamples;
    loadedSample->durationSeconds = reader->lengthInSamples / juce::jmax(1.0, reader->sampleRate);
    if (progressCallback)
        progressCallback(0.46f, "Building waveform...");
    buildWaveformPreview(*loadedSample);
    SampleAnalysisEngine().enrichAnalysisForFile(file,
                                                 *loadedSample,
                                                 SamplePitchAnalysisProfile::Polyphonic,
                                                 progressCallback);
    if (progressCallback)
        progressCallback(0.98f, "Finalizing analysis...");

    result.success = true;
    result.loadedSample = std::const_pointer_cast<const LoadedSampleData>(loadedSample);
    return result;
}

void SamplePlaybackVoice::reset()
{
    active = false;
    releasing = false;
    loopEnabled = false;
    visibleSlot = -1;
    voiceId = 0;
    activeSlice = {};
    readPosition = 0.0;
    stretchedReadPosition = 0.0;
    fadeInRemaining = 0;
    fadeInTotal = 0;
    fadeOutRemaining = 0;
    fadeOutTotal = 0;
    stretchedBuffer.reset();
}

void SamplePlaybackVoice::trigger(const SampleSlice& slice,
                                  int visibleSlotToUse,
                                  uint64_t voiceIdToUse,
                                  bool loopEnabledToUse,
                                  int fadeSamples)
{
    active = true;
    releasing = false;
    loopEnabled = loopEnabledToUse;
    visibleSlot = visibleSlotToUse;
    voiceId = voiceIdToUse;
    activeSlice = slice;
    readPosition = static_cast<double>(slice.startSample);
    stretchedReadPosition = 0.0;
    fadeInTotal = juce::jmax(0, fadeSamples);
    fadeInRemaining = fadeInTotal;
    fadeOutRemaining = 0;
    fadeOutTotal = 0;
    stretchedBuffer.reset();
}

void SamplePlaybackVoice::beginRelease(int fadeSamples)
{
    if (!active)
        return;

    releasing = true;
    fadeOutTotal = juce::jmax(0, fadeSamples);
    fadeOutRemaining = fadeOutTotal;
}

void SamplePlaybackVoice::stop()
{
    reset();
}

void SamplePlaybackVoice::setStretchedBuffer(std::shared_ptr<const StretchedSliceBuffer> bufferToUse)
{
    stretchedBuffer = std::move(bufferToUse);
    if (stretchedBuffer == nullptr)
    {
        stretchedReadPosition = 0.0;
        return;
    }

    const double sliceLength = juce::jmax(1.0, static_cast<double>(activeSlice.endSample - activeSlice.startSample));
    const double normalized = juce::jlimit(0.0,
                                           1.0,
                                           (readPosition - static_cast<double>(activeSlice.startSample)) / sliceLength);
    const double cacheLength = juce::jmax(1, stretchedBuffer->audioBuffer.getNumSamples());
    stretchedReadPosition = normalized * static_cast<double>(cacheLength);
}

void SamplePlaybackVoice::clearStretchedBuffer()
{
    stretchedBuffer.reset();
    stretchedReadPosition = 0.0;
}

SampleModeEngine::SampleModeEngine() = default;

SampleModeEngine::~SampleModeEngine()
{
    fileManager.cancelPendingLoads();
    keyLockCacheThreadPool.removeAllJobs(true, 10000);
}

void SampleModeEngine::prepare(double sampleRate, int maxBlockSize)
{
    preparedSampleRate.store(juce::jmax(1.0, sampleRate), std::memory_order_release);
    preparedMaxBlockSize.store(juce::jmax(1, maxBlockSize), std::memory_order_release);
}

bool SampleModeEngine::loadSampleFromBuffer(const juce::AudioBuffer<float>& buffer,
                                            double sourceSampleRate,
                                            const juce::String& sourcePath,
                                            const juce::String& displayName)
{
    fileManager.cancelPendingLoads();
    keyLockCacheThreadPool.removeAllJobs(true, 10000);
    auto sample = buildLoadedSampleFromBuffer(buffer, sourceSampleRate, sourcePath, displayName);
    if (sample == nullptr)
        return false;

    {
        const juce::ScopedLock lock(stateLock);
        resetSampleSpecificStateLocked();
        loadedSample = std::move(sample);
        persistentState.samplePath = sourcePath;
        invalidateTransientMarkerCachesLocked();
        rebuildSlicesLocked();
        [[maybe_unused]] const auto warmedCanonicalMarkers = buildCanonicalTransientMarkerSamplesLocked();
        sliceModel.setVisibleBankIndex(persistentState.visibleSliceBankIndex);
        clearRandomVisibleSliceOverrideLocked();
        isLoading = false;
        analysisProgress = 0.0f;
        activeRequestId = 0;
        pendingTriggerSliceValid = false;
        statusText = "Loaded " + loadedSample->displayName;
        keyLockCaches.clear();
        keyLockCacheBuildInFlight = false;
        ++keyLockCacheGeneration;
    }

    stop(true);
    clearPendingVisibleSlice();
    notifyLegacyLoopRenderStateChanged();
    sendChangeMessage();
    return true;
}

int SampleModeEngine::loadSampleAsync(const juce::File& file)
{
    if (!file.existsAsFile())
        return 0;

    juce::WeakReference<SampleModeEngine> weakThis(this);

    {
        const juce::ScopedLock lock(stateLock);
        isLoading = true;
        analysisProgress = 0.05f;
        statusText = "Loading " + file.getFileName() + "...";
        activeRequestId = fileManager.loadFileAsync(file,
            [weakThis](SampleFileManager::LoadResult result) mutable
            {
                if (weakThis != nullptr)
                    weakThis->handleLoadResult(std::move(result));
            },
            [weakThis](int requestId, float progress, juce::String loadStatusText) mutable
            {
                if (weakThis != nullptr)
                    weakThis->handleLoadProgress(requestId, progress, loadStatusText);
            });
    }

    sendChangeMessage();
    return activeRequestId;
}

void SampleModeEngine::clear()
{
    fileManager.cancelPendingLoads();
    keyLockCacheThreadPool.removeAllJobs(true, 10000);

    {
        const juce::ScopedLock lock(stateLock);
        loadedSample.reset();
        sliceModel.clear();
        persistentState = {};
        clearRandomVisibleSliceOverrideLocked();
        clearSliceEditUndoHistoryLocked();
        invalidateTransientMarkerCachesLocked();
        legacyLoopWindowStartSample = -1;
        isLoading = false;
        analysisProgress = 0.0f;
        activeRequestId = 0;
        statusText = "No sample loaded";
        keyLockCaches.clear();
        keyLockCacheEnabled = false;
        keyLockCacheBuildInFlight = false;
        ++keyLockCacheGeneration;
    }

    stop();
    clearPendingVisibleSlice();

    notifyLegacyLoopRenderStateChanged();
    sendChangeMessage();
}

void SampleModeEngine::stop(bool immediate)
{
    const juce::SpinLock::ScopedLockType playbackScopedLock(playbackLock);
    if (!immediate)
    {
        const int fadeSamples = juce::jmax(16,
            juce::jmin(kDefaultSliceFadeSamples,
                       static_cast<int>(preparedSampleRate.load(std::memory_order_acquire) * 0.003)));
        bool anyActive = false;
        for (auto& voice : playbackVoices)
        {
            if (voice.isActive())
            {
                voice.beginRelease(fadeSamples);
                anyActive = true;
            }
        }
        if (anyActive)
        {
            playingAtomic.store(1, std::memory_order_release);
            return;
        }
    }

    for (auto& voice : playbackVoices)
        voice.stop();
    activeVisibleSliceSlot.store(-1, std::memory_order_release);
    playbackProgress.store(-1.0f, std::memory_order_release);
    playingAtomic.store(0, std::memory_order_release);
    clearLegacyLoopMonitorState();
}

bool SampleModeEngine::hasSample() const
{
    const juce::ScopedLock lock(stateLock);
    return loadedSample != nullptr;
}

SampleModeEngine::StateSnapshot SampleModeEngine::getStateSnapshot() const
{
    const juce::ScopedLock lock(stateLock);

    StateSnapshot snapshot;
    snapshot.hasSample = (loadedSample != nullptr);
    snapshot.isLoading = isLoading;
    snapshot.analysisProgress = analysisProgress;
    snapshot.statusText = statusText;
    int64_t legacyRangeStartSample = 0;
    int64_t legacyRangeEndSample = 0;
    const bool hasLegacyWindow = computeLegacyLoopWindowRangeLocked(legacyRangeStartSample, legacyRangeEndSample);
    snapshot.visibleSliceBankIndex = hasLegacyWindow ? 0 : sliceModel.getVisibleBankIndex();
    snapshot.visibleSliceBankCount = hasLegacyWindow ? 0 : sliceModel.getVisibleBankCount();
    if (hasLegacyWindow)
    {
        bool canNavigateLeft = (legacyRangeStartSample > 0);
        bool canNavigateRight = (loadedSample != nullptr && legacyRangeEndSample < loadedSample->sourceLengthSamples);

        if (persistentState.sliceMode == SampleSliceMode::Transient)
        {
            const auto localTransientCandidates = buildLegacyLoopWindowTransientMarkerSamplesLocked(legacyRangeStartSample,
                                                                                                    legacyRangeEndSample);
            const int pageStart = getLegacyLoopTransientPageStartIndex(legacyLoopTransientPageStartIndex,
                                                                       static_cast<int>(localTransientCandidates.size()));
            canNavigateLeft = canNavigateLeft || pageStart > 0;
            canNavigateRight = canNavigateRight
                || (pageStart + SliceModel::VisibleSliceCount) < static_cast<int>(localTransientCandidates.size());
        }

        snapshot.canNavigateLeft = canNavigateLeft;
        snapshot.canNavigateRight = canNavigateRight;
    }
    else
    {
        snapshot.canNavigateLeft = sliceModel.canNavigateLeft();
        snapshot.canNavigateRight = sliceModel.canNavigateRight();
    }
    const bool useLegacyMonitor = persistentState.useLegacyLoopEngine;
    snapshot.isPlaying = useLegacyMonitor
        ? (legacyLoopPlayingAtomic.load(std::memory_order_acquire) != 0)
        : (playingAtomic.load(std::memory_order_acquire) != 0);
    snapshot.activeVisibleSliceSlot = useLegacyMonitor
        ? legacyLoopActiveVisibleSliceSlot.load(std::memory_order_acquire)
        : activeVisibleSliceSlot.load(std::memory_order_acquire);
    snapshot.pendingVisibleSliceSlot = pendingVisibleSliceSlot.load(std::memory_order_acquire);
    snapshot.playbackProgress = useLegacyMonitor
        ? legacyLoopPlaybackProgress.load(std::memory_order_acquire)
        : playbackProgress.load(std::memory_order_acquire);
    snapshot.visibleSlices = getCurrentVisibleSlicesLocked();
    snapshot.sliceMode = persistentState.sliceMode;
    snapshot.triggerMode = persistentState.triggerMode;
    snapshot.useLegacyLoopEngine = persistentState.useLegacyLoopEngine;
    snapshot.legacyLoopBarSelection = persistentState.legacyLoopBarSelection;
    snapshot.beatDivision = persistentState.beatDivision;
    snapshot.viewZoom = persistentState.viewZoom;
    snapshot.viewScroll = persistentState.viewScroll;
    snapshot.selectedCueIndex = persistentState.selectedCueIndex;
    snapshot.estimatedTempoBpm = persistentState.analyzedTempoBpm;
    snapshot.estimatedPitchHz = persistentState.analyzedPitchHz;
    snapshot.estimatedPitchMidi = persistentState.analyzedPitchMidi;
    snapshot.estimatedScaleIndex = -1;
    snapshot.essentiaUsed = persistentState.essentiaUsed;
    snapshot.analysisSource = persistentState.analysisSource;
    snapshot.cuePoints = persistentState.cuePoints;
    snapshot.warpMarkers = loadedSample != nullptr
        ? sanitizePersistentWarpMarkers(persistentState.warpMarkers, loadedSample->sourceLengthSamples)
        : persistentState.warpMarkers;
    if (loadedSample != nullptr && persistentState.sliceMode == SampleSliceMode::Transient)
    {
        if (hasLegacyWindow)
        {
            snapshot.transientMarkers = buildLegacyLoopWindowTransientMarkerSamplesLocked(legacyRangeStartSample,
                                                                                          legacyRangeEndSample);
        }
        else
        {
            snapshot.transientMarkers = buildCanonicalTransientMarkerSamplesLocked();
        }
    }

    if (loadedSample != nullptr)
    {
        snapshot.samplePath = loadedSample->sourcePath;
        snapshot.displayName = loadedSample->displayName;
        snapshot.sourceSampleRate = loadedSample->sourceSampleRate;
        snapshot.totalSamples = loadedSample->sourceLengthSamples;
        snapshot.estimatedScaleIndex = loadedSample->analysis.estimatedScaleIndex;
        if (snapshot.transientMarkers.empty())
            snapshot.transientMarkers = buildCanonicalTransientMarkerSamplesLocked();
    }

    return snapshot;
}

std::shared_ptr<const LoadedSampleData> SampleModeEngine::getLoadedSample() const
{
    const juce::ScopedLock lock(stateLock);
    return loadedSample;
}

bool SampleModeEngine::canNavigateVisibleBankLeft() const
{
    const juce::ScopedLock lock(stateLock);
    int64_t rangeStartSample = 0;
    int64_t rangeEndSample = 0;
    if (computeLegacyLoopWindowRangeLocked(rangeStartSample, rangeEndSample))
    {
        if (persistentState.sliceMode == SampleSliceMode::Transient)
        {
            const auto localTransientCandidates = buildLegacyLoopWindowTransientMarkerSamplesLocked(rangeStartSample,
                                                                                                    rangeEndSample);
            const int pageStart = getLegacyLoopTransientPageStartIndex(legacyLoopTransientPageStartIndex,
                                                                       static_cast<int>(localTransientCandidates.size()));
            if (pageStart > 0)
                return true;
        }

        return rangeStartSample > 0;
    }
    return sliceModel.canNavigateLeft();
}

bool SampleModeEngine::canNavigateVisibleBankRight() const
{
    const juce::ScopedLock lock(stateLock);
    int64_t rangeStartSample = 0;
    int64_t rangeEndSample = 0;
    if (computeLegacyLoopWindowRangeLocked(rangeStartSample, rangeEndSample))
    {
        if (persistentState.sliceMode == SampleSliceMode::Transient)
        {
            const auto localTransientCandidates = buildLegacyLoopWindowTransientMarkerSamplesLocked(rangeStartSample,
                                                                                                    rangeEndSample);
            const int pageStart = getLegacyLoopTransientPageStartIndex(legacyLoopTransientPageStartIndex,
                                                                       static_cast<int>(localTransientCandidates.size()));
            if ((pageStart + SliceModel::VisibleSliceCount) < static_cast<int>(localTransientCandidates.size()))
                return true;
        }

        return loadedSample != nullptr && rangeEndSample < loadedSample->sourceLengthSamples;
    }
    return sliceModel.canNavigateRight();
}

void SampleModeEngine::stepVisibleBank(int delta)
{
    bool changed = false;
    bool renderStateChanged = false;
    {
        const juce::ScopedLock lock(stateLock);
        clearRandomVisibleSliceOverrideLocked();
        int64_t rangeStartSample = 0;
        int64_t rangeEndSample = 0;
        if (computeLegacyLoopWindowRangeLocked(rangeStartSample, rangeEndSample))
        {
            if (persistentState.sliceMode == SampleSliceMode::Transient)
            {
                const auto localTransientCandidates = buildLegacyLoopWindowTransientMarkerSamplesLocked(rangeStartSample,
                                                                                                        rangeEndSample);
                const int candidateCount = static_cast<int>(localTransientCandidates.size());
                const int pageStart = getLegacyLoopTransientPageStartIndex(legacyLoopTransientPageStartIndex,
                                                                           candidateCount);
                const int nextPageStart = pageStart + (delta * SliceModel::VisibleSliceCount);

                if (candidateCount > SliceModel::VisibleSliceCount
                    && nextPageStart >= 0
                    && nextPageStart < candidateCount)
                {
                    legacyLoopTransientPageStartIndex = getLegacyLoopTransientPageStartIndex(nextPageStart, candidateCount);
                    const int pageNumber = (legacyLoopTransientPageStartIndex / SliceModel::VisibleSliceCount) + 1;
                    const int pageCount = (candidateCount + SliceModel::VisibleSliceCount - 1) / SliceModel::VisibleSliceCount;
                    statusText = "Viewing transient page "
                        + juce::String(pageNumber)
                        + "/"
                        + juce::String(juce::jmax(1, pageCount));
                    changed = true;
                    renderStateChanged = persistentState.useLegacyLoopEngine;
                }
            }

            if (!changed)
            {
                bool updatedFromBeatTicks = false;
                if (loadedSample != nullptr)
                {
                    const float beatsForLoop = legacyLoopBarSelectionToBeats(persistentState.legacyLoopBarSelection);
                    const int integerBeatCount = juce::roundToInt(beatsForLoop);
                    const auto beatTicks = sanitizeBeatTickSamples(loadedSample->analysis.beatTickSamples,
                                                                   loadedSample->sourceLengthSamples);
                    if (integerBeatCount > 0
                        && std::abs(beatsForLoop - static_cast<float>(integerBeatCount)) < 1.0e-4f
                        && beatTicks.size() >= 2)
                    {
                        const int currentStartIndex = findNearestBeatTickIndex(beatTicks, rangeStartSample);
                        if (currentStartIndex >= 0)
                        {
                            const int requestedIndex = juce::jlimit(0,
                                                                    static_cast<int>(beatTicks.size()) - 1,
                                                                    currentStartIndex + (delta * integerBeatCount));
                            int64_t alignedStartSample = 0;
                            int64_t alignedEndSample = 0;
                            if (computeBeatTickAlignedWindowRange(beatTicks,
                                                                 loadedSample->sourceLengthSamples,
                                                                 beatTicks[static_cast<size_t>(requestedIndex)],
                                                                 integerBeatCount,
                                                                 alignedStartSample,
                                                                 alignedEndSample))
                            {
                                legacyLoopWindowStartSample = alignedStartSample;
                                updatedFromBeatTicks = true;
                            }
                        }
                    }
                }

                if (!updatedFromBeatTicks)
                {
                    const int64_t windowLength = juce::jmax<int64_t>(1, rangeEndSample - rangeStartSample);
                    const int64_t maxWindowStart = juce::jmax<int64_t>(0, loadedSample != nullptr
                                                                          ? (loadedSample->sourceLengthSamples - windowLength)
                                                                          : 0);
                    legacyLoopWindowStartSample = juce::jlimit<int64_t>(0,
                                                                        maxWindowStart,
                                                                        rangeStartSample + (static_cast<int64_t>(delta) * windowLength));
                }
                legacyLoopWindowManualAnchor = false;
                legacyLoopTransientPageStartIndex = 0;
                fitViewToLegacyLoopWindowLocked();

                const double sr = loadedSample != nullptr ? loadedSample->sourceSampleRate : 0.0;
                const double seconds = sr > 0.0 ? static_cast<double>(legacyLoopWindowStartSample) / sr : 0.0;
                statusText = "Viewing MLR window @ " + juce::String(seconds, 2) + "s";
                changed = true;
                renderStateChanged = persistentState.useLegacyLoopEngine;
            }
        }
        else
        {
            sliceModel.stepVisibleBank(delta);
            persistentState.visibleSliceBankIndex = sliceModel.getVisibleBankIndex();
            statusText = loadedSample != nullptr
                ? ("Viewing slice bank " + juce::String(sliceModel.getVisibleBankIndex() + 1)
                   + "/" + juce::String(juce::jmax(1, sliceModel.getVisibleBankCount())))
                : juce::String("No sample loaded");
            changed = true;
            renderStateChanged = persistentState.useLegacyLoopEngine;
        }
    }

    if (changed && renderStateChanged)
        notifyLegacyLoopRenderStateChanged();

    if (changed)
        sendChangeMessage();
}

void SampleModeEngine::randomizeVisibleBank()
{
    bool changed = false;
    {
        const juce::ScopedLock lock(stateLock);
        if (persistentState.useLegacyLoopEngine)
            return;

        const auto& allSlices = sliceModel.getAllSlices();
        if (allSlices.empty())
            return;

        std::mt19937 rng(static_cast<uint32_t>(juce::Random::getSystemRandom().nextInt()));
        clearRandomVisibleSliceOverrideLocked();
        randomVisibleSlices = buildDistributedRandomSliceSelection(
            allSlices,
            loadedSample != nullptr ? loadedSample->sourceLengthSamples : 0,
            rng);
        randomVisibleSliceOverrideActive = true;
        statusText = "Viewing distributed random slices";
        changed = true;
    }

    if (changed)
    {
        notifyLegacyLoopRenderStateChanged();
        sendChangeMessage();
    }
}

bool SampleModeEngine::hasVisibleSlice(int visibleSlot) const
{
    SampleSlice slice;
    return resolveVisibleSlice(visibleSlot, slice);
}

bool SampleModeEngine::getVisibleSliceInfo(int visibleSlot, SampleSlice& sliceOut) const
{
    return resolveVisibleSlice(visibleSlot, sliceOut);
}

bool SampleModeEngine::getLegacyLoopSyncInfo(LegacyLoopSyncInfo& syncInfo) const
{
    const juce::ScopedLock lock(stateLock);
    if (loadedSample == nullptr)
        return false;

    syncInfo = {};
    syncInfo.loadedSample = loadedSample;
    syncInfo.visibleSlices = getCurrentVisibleSlicesLocked();
    syncInfo.analyzedTempoBpm = persistentState.analyzedTempoBpm > 0.0
        ? persistentState.analyzedTempoBpm
        : loadedSample->analysis.estimatedTempoBpm;
    int64_t rangeStartSample = 0;
    int64_t rangeEndSample = 0;
    const bool hasLegacyWindow = computeLegacyLoopWindowRangeLocked(rangeStartSample, rangeEndSample);
    syncInfo.visibleBankIndex = (randomVisibleSliceOverrideActive || hasLegacyWindow)
        ? -1
        : sliceModel.getVisibleBankIndex();
    if (hasLegacyWindow)
    {
        syncInfo.bankStartSample = rangeStartSample;
        syncInfo.bankEndSample = rangeEndSample;
    }
    syncInfo.legacyLoopBarSelection = persistentState.legacyLoopBarSelection;
    syncInfo.sliceMode = persistentState.sliceMode;
    syncInfo.warpMarkers = persistentState.warpMarkers;
    switch (persistentState.sliceMode)
    {
        case SampleSliceMode::Transient:
            syncInfo.markerSamples = buildCanonicalTransientMarkerSamplesLocked();
            break;
        case SampleSliceMode::Manual:
            syncInfo.markerSamples.reserve(persistentState.cuePoints.size());
            for (const auto& cue : persistentState.cuePoints)
            {
                if (cue.samplePosition > 0 && cue.samplePosition < loadedSample->sourceLengthSamples)
                    syncInfo.markerSamples.push_back(cue.samplePosition);
            }
            break;
        case SampleSliceMode::Beat:
        case SampleSliceMode::Uniform:
        default:
            syncInfo.markerSamples.reserve(syncInfo.visibleSlices.size());
            for (const auto& slice : syncInfo.visibleSlices)
            {
                if (slice.id >= 0 && slice.endSample > slice.startSample)
                    syncInfo.markerSamples.push_back(slice.startSample);
            }
            break;
    }
    syncInfo.visibleBankBeats = computeVisibleSliceBeatSpan(*loadedSample,
                                                            syncInfo.visibleSlices,
                                                            syncInfo.analyzedTempoBpm,
                                                            persistentState.legacyLoopBarSelection);
    return true;
}

bool SampleModeEngine::resolveLegacyLoopTriggerSyncInfo(int preferredVisibleSlot,
                                                        int sliceId,
                                                        int64_t sliceStartSample,
                                                        LegacyLoopSyncInfo& syncInfo) const
{
    const juce::ScopedLock lock(stateLock);
    if (loadedSample == nullptr)
        return false;

    int targetSliceIndex = -1;
    const int clampedSlot = juce::jlimit(0, SliceModel::VisibleSliceCount - 1, preferredVisibleSlot);
    const auto& allSlices = sliceModel.getAllSlices();
    int64_t legacyRangeStartSample = 0;
    int64_t legacyRangeEndSample = 0;
    const bool useLegacyWindowSlices = computeLegacyLoopWindowRangeLocked(legacyRangeStartSample, legacyRangeEndSample)
        && sliceId < 0
        && sliceStartSample < 0;
    const bool useRandomVisibleSlices = randomVisibleSliceOverrideActive
        && !useLegacyWindowSlices
        && sliceId < 0
        && sliceStartSample < 0;

    if (useLegacyWindowSlices)
    {
        syncInfo = {};
        syncInfo.loadedSample = loadedSample;
        syncInfo.visibleSlices = buildLegacyLoopWindowVisibleSlicesLocked(legacyRangeStartSample, legacyRangeEndSample);
        syncInfo.visibleBankIndex = -1;
        syncInfo.triggerVisibleSlot = clampedSlot;
        syncInfo.bankStartSample = legacyRangeStartSample;
        syncInfo.bankEndSample = legacyRangeEndSample;
        syncInfo.analyzedTempoBpm = persistentState.analyzedTempoBpm > 0.0
            ? persistentState.analyzedTempoBpm
            : loadedSample->analysis.estimatedTempoBpm;
        syncInfo.legacyLoopBarSelection = persistentState.legacyLoopBarSelection;
        syncInfo.sliceMode = persistentState.sliceMode;
        syncInfo.warpMarkers = persistentState.warpMarkers;
        if (persistentState.sliceMode == SampleSliceMode::Transient)
        {
            syncInfo.markerSamples = buildLegacyLoopWindowTransientMarkerSamplesLocked(legacyRangeStartSample,
                                                                                       legacyRangeEndSample);
        }
        else if (persistentState.sliceMode == SampleSliceMode::Manual)
        {
            syncInfo.markerSamples.reserve(persistentState.cuePoints.size());
            for (const auto& cue : persistentState.cuePoints)
            {
                if (cue.samplePosition > 0 && cue.samplePosition < loadedSample->sourceLengthSamples)
                    syncInfo.markerSamples.push_back(cue.samplePosition);
            }
        }
        else
        {
            syncInfo.markerSamples.reserve(syncInfo.visibleSlices.size());
            for (const auto& slice : syncInfo.visibleSlices)
            {
                if (slice.id >= 0 && slice.endSample > slice.startSample)
                    syncInfo.markerSamples.push_back(slice.startSample);
            }
        }
        syncInfo.visibleBankBeats = computeVisibleSliceBeatSpan(*loadedSample,
                                                                syncInfo.visibleSlices,
                                                                syncInfo.analyzedTempoBpm,
                                                                persistentState.legacyLoopBarSelection);
        return true;
    }

    if (useRandomVisibleSlices)
    {
        syncInfo = {};
        syncInfo.loadedSample = loadedSample;
        syncInfo.visibleSlices = randomVisibleSlices;
        syncInfo.visibleBankIndex = -1;
        syncInfo.triggerVisibleSlot = clampedSlot;
        syncInfo.analyzedTempoBpm = persistentState.analyzedTempoBpm > 0.0
            ? persistentState.analyzedTempoBpm
            : loadedSample->analysis.estimatedTempoBpm;
        syncInfo.legacyLoopBarSelection = persistentState.legacyLoopBarSelection;
        syncInfo.sliceMode = persistentState.sliceMode;
        syncInfo.warpMarkers = persistentState.warpMarkers;
        syncInfo.markerSamples.reserve(syncInfo.visibleSlices.size());
        for (const auto& slice : syncInfo.visibleSlices)
        {
            if (slice.id >= 0 && slice.endSample > slice.startSample)
                syncInfo.markerSamples.push_back(slice.startSample);
        }
        syncInfo.visibleBankBeats = computeVisibleSliceBeatSpan(*loadedSample,
                                                                syncInfo.visibleSlices,
                                                                syncInfo.analyzedTempoBpm,
                                                                persistentState.legacyLoopBarSelection);
        return true;
    }

    if (sliceId >= 0 || sliceStartSample >= 0)
    {
        for (int i = 0; i < static_cast<int>(allSlices.size()); ++i)
        {
            const auto& slice = allSlices[static_cast<size_t>(i)];
            if (slice.id != sliceId)
                continue;
            if (sliceStartSample < 0 || slice.startSample == sliceStartSample)
            {
                targetSliceIndex = i;
                break;
            }
        }

        if (targetSliceIndex < 0 && sliceStartSample >= 0)
        {
            int64_t bestDistance = std::numeric_limits<int64_t>::max();
            for (int i = 0; i < static_cast<int>(allSlices.size()); ++i)
            {
                const auto& slice = allSlices[static_cast<size_t>(i)];
                const int64_t distance = std::abs(slice.startSample - sliceStartSample);
                if (distance < bestDistance)
                {
                    bestDistance = distance;
                    targetSliceIndex = i;
                    if (distance == 0)
                        break;
                }
            }
        }
    }

    const int pendingSlot = pendingVisibleSliceSlot.load(std::memory_order_acquire);
    if (targetSliceIndex < 0 && pendingTriggerSliceValid && pendingSlot == clampedSlot)
    {
        for (int i = 0; i < static_cast<int>(allSlices.size()); ++i)
        {
            const auto& slice = allSlices[static_cast<size_t>(i)];
            if (slice.id == pendingTriggerSlice.id
                && slice.startSample == pendingTriggerSlice.startSample
                && slice.endSample == pendingTriggerSlice.endSample)
            {
                targetSliceIndex = i;
                break;
            }
        }
    }

    if (targetSliceIndex < 0)
    {
        const auto visibleSlices = getCurrentVisibleSlicesLocked();
        const auto& currentSlice = visibleSlices[static_cast<size_t>(clampedSlot)];
        if (currentSlice.id >= 0)
        {
            for (int i = 0; i < static_cast<int>(allSlices.size()); ++i)
            {
                const auto& slice = allSlices[static_cast<size_t>(i)];
                if (slice.id == currentSlice.id
                    && slice.startSample == currentSlice.startSample
                    && slice.endSample == currentSlice.endSample)
                {
                    targetSliceIndex = i;
                    break;
                }
            }
        }
    }

    if (targetSliceIndex < 0 || targetSliceIndex >= static_cast<int>(allSlices.size()))
        return false;

    return buildLegacyLoopSyncInfoForSliceIndexLocked(targetSliceIndex, syncInfo);
}

int SampleModeEngine::getActiveVisibleSliceSlot() const noexcept
{
    return activeVisibleSliceSlot.load(std::memory_order_acquire);
}

int SampleModeEngine::getPendingVisibleSliceSlot() const noexcept
{
    return pendingVisibleSliceSlot.load(std::memory_order_acquire);
}

uint64_t SampleModeEngine::getLegacyLoopRenderStateVersion() const noexcept
{
    return legacyLoopRenderStateVersion.load(std::memory_order_acquire);
}

void SampleModeEngine::setPendingVisibleSlice(int visibleSlot)
{
    const int clampedSlot = juce::jlimit(0, SliceModel::VisibleSliceCount - 1, visibleSlot);
    SampleSlice resolvedSlice;
    const bool hasSlice = resolveVisibleSlice(clampedSlot, resolvedSlice);
    {
        const juce::ScopedLock lock(stateLock);
        pendingTriggerSliceValid = hasSlice;
        if (hasSlice)
            pendingTriggerSlice = resolvedSlice;
    }
    pendingVisibleSliceSlot.store(clampedSlot, std::memory_order_release);
}

void SampleModeEngine::clearPendingVisibleSlice()
{
    const juce::ScopedLock lock(stateLock);
    pendingTriggerSliceValid = false;
    pendingVisibleSliceSlot.store(-1, std::memory_order_release);
}

void SampleModeEngine::requestKeyLockRenderCache(float playbackRate,
                                                 float pitchSemitones,
                                                 bool enabled,
                                                 TimeStretchBackend backend)
{
#if !(MLRVST_ENABLE_SOUNDTOUCH || MLRVST_ENABLE_BUNGEE)
    juce::ignoreUnused(playbackRate, pitchSemitones, enabled, backend);
    return;
#else
    const auto sanitizedBackend = sanitizeTimeStretchBackend(static_cast<int>(backend));
    bool clearActiveVoiceCaches = false;
    bool shouldReturnAfterUnlock = false;
    if (sanitizedBackend == TimeStretchBackend::Resample
        || !isTimeStretchBackendAvailable(sanitizedBackend))
    {
        {
            const juce::ScopedLock lock(stateLock);
            keyLockCacheEnabled = false;
            keyLockCaches.clear();
            keyLockCacheBuildInFlight = false;
            ++keyLockCacheGeneration;
        }
        clearActiveVoiceCaches = true;
        shouldReturnAfterUnlock = true;
    }

    juce::WeakReference<SampleModeEngine> weakThis(this);
    std::shared_ptr<const LoadedSampleData> sampleData;
    std::array<SampleSlice, SliceModel::VisibleSliceCount> visibleSlices {};
    uint64_t generation = 0;
    int visibleBankIndex = 0;
    float quantizedRate = quantizeKeyLockRate(playbackRate);
    float quantizedPitch = quantizeKeyLockPitch(pitchSemitones);

    if (!shouldReturnAfterUnlock)
    {
        const juce::ScopedLock lock(stateLock);
        const bool backendChanged = keyLockCacheBackend != sanitizedBackend;
        keyLockCacheEnabled = enabled;
        keyLockCacheBackend = sanitizedBackend;
        clearActiveVoiceCaches = clearActiveVoiceCaches || backendChanged;

        if (!enabled || loadedSample == nullptr)
        {
            keyLockCaches.clear();
            keyLockCacheBuildInFlight = false;
            ++keyLockCacheGeneration;
            clearActiveVoiceCaches = true;
            shouldReturnAfterUnlock = true;
        }

        if (!shouldReturnAfterUnlock
            && std::abs(quantizedRate - 1.0f) <= kKeyLockCacheRateTolerance
            && std::abs(quantizedPitch) <= kKeyLockCachePitchTolerance)
        {
            keyLockCaches.clear();
            keyLockCacheBuildInFlight = false;
            ++keyLockCacheGeneration;
            clearActiveVoiceCaches = true;
            shouldReturnAfterUnlock = true;
        }

        if (!shouldReturnAfterUnlock)
        {
            const int currentBankIndex = randomVisibleSliceOverrideActive ? -1 : sliceModel.getVisibleBankIndex();
            const bool matchesExisting = !keyLockCaches.empty()
                && currentBankIndex == keyLockCacheVisibleBankIndex
                && std::abs(keyLockCachePlaybackRate - quantizedRate) <= kKeyLockCacheRateTolerance
                && std::abs(keyLockCachePitchSemitones - quantizedPitch) <= kKeyLockCachePitchTolerance
                && keyLockCacheBackend == sanitizedBackend;
            if (matchesExisting || keyLockCacheBuildInFlight)
            {
                shouldReturnAfterUnlock = true;
            }
            else
            {
                sampleData = loadedSample;
                visibleSlices = getCurrentVisibleSlicesLocked();
                visibleBankIndex = currentBankIndex;
                keyLockCacheBuildInFlight = true;
                generation = ++keyLockCacheGeneration;
            }
        }
    }

    if (clearActiveVoiceCaches)
    {
        const juce::SpinLock::ScopedLockType playbackScopedLock(playbackLock);
        for (auto& voice : playbackVoices)
            voice.clearStretchedBuffer();
    }

    if (shouldReturnAfterUnlock)
        return;

    class KeyLockCacheJob : public juce::ThreadPoolJob
    {
    public:
        KeyLockCacheJob(juce::WeakReference<SampleModeEngine> ownerToUse,
                        std::shared_ptr<const LoadedSampleData> sampleDataToUse,
                        std::array<SampleSlice, SliceModel::VisibleSliceCount> visibleSlicesToUse,
                        uint64_t generationToUse,
                        float playbackRateToUse,
                        float pitchSemitonesToUse,
                        TimeStretchBackend backendToUse,
                        int visibleBankIndexToUse)
            : juce::ThreadPoolJob("FlipKeyLockCache")
            , owner(ownerToUse)
            , sampleData(std::move(sampleDataToUse))
            , visibleSlices(std::move(visibleSlicesToUse))
            , generation(generationToUse)
            , playbackRate(playbackRateToUse)
            , pitchSemitones(pitchSemitonesToUse)
            , backend(backendToUse)
            , visibleBankIndex(visibleBankIndexToUse)
        {
        }

        JobStatus runJob() override
        {
            std::vector<std::shared_ptr<const StretchedSliceBuffer>> caches;
            if (sampleData != nullptr)
            {
                caches.reserve(SliceModel::VisibleSliceCount);
                for (const auto& slice : visibleSlices)
                {
                    if (shouldExit())
                        return jobHasFinished;
                    if (slice.id < 0 || slice.endSample <= slice.startSample)
                        continue;

                    if (auto cache = buildStretchedSliceBuffer(*sampleData,
                                                               slice,
                                                               playbackRate,
                                                               pitchSemitones,
                                                               backend))
                        caches.push_back(std::move(cache));
                }
            }

            juce::MessageManager::callAsync(
                [weakOwner = owner,
                 generationValue = generation,
                 readyCaches = std::move(caches),
                 rate = playbackRate,
                 pitch = pitchSemitones,
                 bankIndex = visibleBankIndex]() mutable
                {
                    if (weakOwner != nullptr)
                    {
                        weakOwner->handleKeyLockCacheReady(generationValue,
                                                           std::move(readyCaches),
                                                           rate,
                                                           pitch,
                                                           bankIndex);
                    }
                });
            return jobHasFinished;
        }

    private:
        juce::WeakReference<SampleModeEngine> owner;
        std::shared_ptr<const LoadedSampleData> sampleData;
        std::array<SampleSlice, SliceModel::VisibleSliceCount> visibleSlices {};
        uint64_t generation = 0;
        float playbackRate = 1.0f;
        float pitchSemitones = 0.0f;
        TimeStretchBackend backend = TimeStretchBackend::SoundTouch;
        int visibleBankIndex = 0;
    };

    keyLockCacheThreadPool.addJob(
        new KeyLockCacheJob(weakThis,
                            std::move(sampleData),
                            visibleSlices,
                            generation,
                            quantizedRate,
                            quantizedPitch,
                            sanitizedBackend,
                            visibleBankIndex),
        true);
#endif
}

bool SampleModeEngine::triggerResolvedSlice(const SampleSlice& slice,
                                            int resolvedVisibleSlot,
                                            SampleTriggerMode triggerMode)
{
    const int fadeSamples = juce::jmax(16,
        juce::jmin(kDefaultSliceFadeSamples,
                   static_cast<int>(preparedSampleRate.load(std::memory_order_acquire) * 0.003)));

    const juce::SpinLock::ScopedLockType playbackScopedLock(playbackLock);
    for (auto& voice : playbackVoices)
    {
        if (voice.isActive())
            voice.beginRelease(fadeSamples);
    }

    SamplePlaybackVoice* targetVoice = nullptr;
    for (auto& voice : playbackVoices)
    {
        if (!voice.isActive())
        {
            targetVoice = &voice;
            break;
        }
    }
    if (targetVoice == nullptr)
    {
        targetVoice = &playbackVoices.front();
        for (auto& voice : playbackVoices)
        {
            if (voice.getVoiceId() < targetVoice->getVoiceId())
                targetVoice = &voice;
        }
        targetVoice->stop();
    }

    targetVoice->trigger(slice, resolvedVisibleSlot, nextVoiceId++, triggerMode == SampleTriggerMode::Loop, fadeSamples);
    activeVisibleSliceSlot.store(resolvedVisibleSlot, std::memory_order_release);
    pendingVisibleSliceSlot.store(-1, std::memory_order_release);
    playbackProgress.store(slice.normalizedStart, std::memory_order_release);
    playingAtomic.store(1, std::memory_order_release);
    return true;
}

bool SampleModeEngine::resolveRecordedSlice(int sliceId,
                                            int64_t sliceStartSample,
                                            SampleSlice& sliceOut,
                                            int& resolvedVisibleSlot) const
{
    const juce::ScopedLock lock(stateLock);
    const auto& allSlices = sliceModel.getAllSlices();
    if (allSlices.empty())
        return false;

    int foundIndex = -1;

    if (sliceId >= 0)
    {
        for (int i = 0; i < static_cast<int>(allSlices.size()); ++i)
        {
            const auto& candidate = allSlices[static_cast<size_t>(i)];
            if (candidate.id != sliceId)
                continue;

            if (sliceStartSample < 0 || candidate.startSample == sliceStartSample)
            {
                foundIndex = i;
                break;
            }
        }
    }

    if (foundIndex < 0 && sliceStartSample >= 0)
    {
        int64_t bestDistance = std::numeric_limits<int64_t>::max();
        for (int i = 0; i < static_cast<int>(allSlices.size()); ++i)
        {
            const auto& candidate = allSlices[static_cast<size_t>(i)];
            const int64_t distance = std::abs(candidate.startSample - sliceStartSample);
            if (distance < bestDistance)
            {
                bestDistance = distance;
                foundIndex = i;
                if (distance == 0)
                    break;
            }
        }
    }

    if (foundIndex < 0)
        return false;

    sliceOut = allSlices[static_cast<size_t>(foundIndex)];
    if (sliceOut.id < 0 || sliceOut.endSample <= sliceOut.startSample)
        return false;

    const int visibleBankIndex = sliceModel.getVisibleBankIndex();
    const int visibleStart = visibleBankIndex * SliceModel::VisibleSliceCount;
    if (foundIndex >= visibleStart && foundIndex < (visibleStart + SliceModel::VisibleSliceCount))
        resolvedVisibleSlot = foundIndex - visibleStart;
    else
        resolvedVisibleSlot = -1;

    if (resolvedVisibleSlot < 0 && randomVisibleSliceOverrideActive)
    {
        for (int slot = 0; slot < SliceModel::VisibleSliceCount; ++slot)
        {
            const auto& visibleSlice = randomVisibleSlices[static_cast<size_t>(slot)];
            if (visibleSlice.id == sliceOut.id
                && visibleSlice.startSample == sliceOut.startSample
                && visibleSlice.endSample == sliceOut.endSample)
            {
                resolvedVisibleSlot = slot;
                break;
            }
        }
    }

    return true;
}

bool SampleModeEngine::triggerVisibleSlice(int visibleSlot, bool loopEnabled)
{
    SampleSlice slice;
    SampleTriggerMode triggerMode = SampleTriggerMode::OneShot;
    const int pendingSlot = pendingVisibleSliceSlot.load(std::memory_order_acquire);
    {
        const juce::ScopedLock lock(stateLock);
        triggerMode = loopEnabled ? SampleTriggerMode::Loop : persistentState.triggerMode;
        if (pendingTriggerSliceValid && pendingSlot == visibleSlot)
        {
            slice = pendingTriggerSlice;
            pendingTriggerSliceValid = false;
        }
        else
        {
            if (visibleSlot < 0 || visibleSlot >= SliceModel::VisibleSliceCount)
                return false;

            const auto visibleSlices = getCurrentVisibleSlicesLocked();
            const auto& resolvedSlice = visibleSlices[static_cast<size_t>(visibleSlot)];
            if (resolvedSlice.id < 0 || resolvedSlice.endSample <= resolvedSlice.startSample)
                return false;

            slice = resolvedSlice;
        }
    }

    return triggerResolvedSlice(slice, visibleSlot, triggerMode);
}

bool SampleModeEngine::triggerRecordedSlice(int preferredVisibleSlot,
                                            int sliceId,
                                            int64_t sliceStartSample,
                                            bool forceLoop)
{
    SampleSlice slice;
    int resolvedVisibleSlot = preferredVisibleSlot;
    if (!resolveRecordedSlice(sliceId, sliceStartSample, slice, resolvedVisibleSlot))
        return triggerVisibleSlice(preferredVisibleSlot, forceLoop);

    SampleTriggerMode triggerMode = SampleTriggerMode::OneShot;
    {
        const juce::ScopedLock lock(stateLock);
        triggerMode = forceLoop ? SampleTriggerMode::Loop : persistentState.triggerMode;
    }

    return triggerResolvedSlice(slice, resolvedVisibleSlot, triggerMode);
}

void SampleModeEngine::updateLegacyLoopMonitorState(bool isPlaying,
                                                    int visibleSlot,
                                                    float playbackProgressNormalized)
{
    legacyLoopPlayingAtomic.store(isPlaying ? 1 : 0, std::memory_order_release);
    legacyLoopActiveVisibleSliceSlot.store(juce::jlimit(-1, SliceModel::VisibleSliceCount - 1, visibleSlot),
                                           std::memory_order_release);
    legacyLoopPlaybackProgress.store(juce::jlimit(-1.0f, 1.0f, playbackProgressNormalized),
                                     std::memory_order_release);
}

void SampleModeEngine::clearLegacyLoopMonitorState()
{
    legacyLoopPlayingAtomic.store(0, std::memory_order_release);
    legacyLoopActiveVisibleSliceSlot.store(-1, std::memory_order_release);
    legacyLoopPlaybackProgress.store(-1.0f, std::memory_order_release);
}

SampleModeEngine::RenderResult SampleModeEngine::renderToBuffer(juce::AudioBuffer<float>& output,
                                                                int startSample,
                                                                int numSamples,
                                                                float playbackRate,
                                                                int fadeSamples,
                                                                float pitchSemitones,
                                                                bool preferHighQualityKeyLock)
{
    RenderResult result;
    if (numSamples <= 0)
        return result;

    std::shared_ptr<const LoadedSampleData> sampleData;
    {
        const juce::ScopedLock lock(stateLock);
        sampleData = loadedSample;
    }

    if (sampleData == nullptr || sampleData->audioBuffer.getNumSamples() <= 0)
    {
        stop(true);
        return result;
    }

    const double renderRate = preparedSampleRate.load(std::memory_order_acquire);
    const double sourceRate = juce::jmax(1.0, sampleData->sourceSampleRate);
    const double rateScale = sourceRate / juce::jmax(1.0, renderRate);
    const double sourceIncrement = juce::jlimit(0.03125, 8.0, static_cast<double>(playbackRate)) * rateScale;
    const double cacheIncrement = rateScale;
    const int fadeLen = juce::jmax(16, fadeSamples);
    std::vector<std::shared_ptr<const StretchedSliceBuffer>> availableKeyLockCaches;
    TimeStretchBackend activeKeyLockBackend = TimeStretchBackend::Resample;

    bool renderedAnything = false;
    int newestActiveSlot = -1;
    uint64_t newestVoiceId = 0;
    float latestProgress = playbackProgress.load(std::memory_order_acquire);
    bool canUseKeyLockForAllVoices = false;

    if (preferHighQualityKeyLock)
    {
        const juce::ScopedLock stateScopedLock(stateLock);
        availableKeyLockCaches = keyLockCaches;
        activeKeyLockBackend = keyLockCacheBackend;
    }

    auto findAvailableCache = [&](const SampleSlice& slice) -> std::shared_ptr<const StretchedSliceBuffer>
    {
        for (const auto& cache : availableKeyLockCaches)
        {
            if (cache != nullptr
                && matchesKeyLockRequest(*cache, slice, playbackRate, pitchSemitones, activeKeyLockBackend))
                return cache;
        }
        return {};
    };

    const juce::SpinLock::ScopedLockType playbackScopedLock(playbackLock);
    if (preferHighQualityKeyLock)
    {
        const bool deferHotSwapForBackend = activeKeyLockBackend == TimeStretchBackend::Bungee;
        canUseKeyLockForAllVoices = true;
        for (auto& voice : playbackVoices)
        {
            if (!voice.isActive())
                continue;

            const auto existing = voice.getStretchedBuffer();
            const bool hasMatchingExisting = existing != nullptr
                && matchesKeyLockRequest(*existing,
                                         voice.getActiveSlice(),
                                         playbackRate,
                                         pitchSemitones,
                                         activeKeyLockBackend);
            if (hasMatchingExisting)
                continue;

            if (deferHotSwapForBackend && existing != nullptr)
                continue;

            if (findAvailableCache(voice.getActiveSlice()) == nullptr)
            {
                canUseKeyLockForAllVoices = false;
                break;
            }
        }

        if (canUseKeyLockForAllVoices)
        {
            for (auto& voice : playbackVoices)
            {
                if (!voice.isActive())
                    continue;

                const auto existing = voice.getStretchedBuffer();
                const bool hasMatchingExisting = existing != nullptr
                    && matchesKeyLockRequest(*existing,
                                             voice.getActiveSlice(),
                                             playbackRate,
                                             pitchSemitones,
                                             activeKeyLockBackend);
                if (!hasMatchingExisting)
                {
                    if (deferHotSwapForBackend && existing != nullptr)
                        continue;

                    if (auto cache = findAvailableCache(voice.getActiveSlice()))
                        voice.setStretchedBuffer(std::move(cache));
                    else
                        canUseKeyLockForAllVoices = false;
                }
            }
        }
    }

    for (auto& voice : playbackVoices)
    {
        if (!voice.isActive())
            continue;

        renderedAnything = true;
        if (voice.getVoiceId() >= newestVoiceId)
        {
            newestVoiceId = voice.getVoiceId();
            newestActiveSlot = voice.getVisibleSlot();
        }

        const double sliceStart = static_cast<double>(voice.getActiveSlice().startSample);
        const double sliceEnd = static_cast<double>(juce::jmax(voice.getActiveSlice().startSample + 1,
                                                               voice.getActiveSlice().endSample));
        const double sliceLength = juce::jmax(1.0, sliceEnd - sliceStart);
        const double localFade = juce::jmin(sliceLength * 0.25, static_cast<double>(fadeLen));
        const auto stretchedBuffer = canUseKeyLockForAllVoices
            ? voice.getStretchedBuffer()
            : std::shared_ptr<const StretchedSliceBuffer>();
        const bool usingStretched = (stretchedBuffer != nullptr);
        const double stretchedLength = usingStretched
            ? static_cast<double>(juce::jmax(1, stretchedBuffer->audioBuffer.getNumSamples()))
            : 0.0;
        const double stretchedFade = usingStretched
            ? juce::jmin(stretchedLength * 0.25, static_cast<double>(fadeLen))
            : 0.0;

        for (int sampleIndex = 0; sampleIndex < numSamples; ++sampleIndex)
        {
            if (!voice.isActive())
                break;

            if (!voice.shouldLoop() && voice.readPosition >= sliceEnd)
            {
                voice.stop();
                break;
            }

            if (voice.shouldLoop())
            {
                while (voice.readPosition >= sliceEnd)
                    voice.readPosition -= sliceLength;
                while (voice.readPosition < sliceStart)
                    voice.readPosition += sliceLength;
            }

            if (usingStretched)
            {
                if (voice.shouldLoop())
                {
                    while (voice.stretchedReadPosition >= stretchedLength)
                        voice.stretchedReadPosition -= stretchedLength;
                    while (voice.stretchedReadPosition < 0.0)
                        voice.stretchedReadPosition += stretchedLength;
                }
                else
                {
                    voice.stretchedReadPosition = juce::jlimit(0.0, stretchedLength, voice.stretchedReadPosition);
                }
            }

            float gain = 1.0f;
            const double fromStart = juce::jmax(0.0, voice.readPosition - sliceStart);
            const double toEnd = juce::jmax(0.0, sliceEnd - voice.readPosition);

            if (localFade > 1.0)
            {
                gain *= static_cast<float>(juce::jlimit(0.0, 1.0, fromStart / localFade));
                gain *= static_cast<float>(juce::jlimit(0.0, 1.0, toEnd / localFade));
            }

            if (usingStretched && stretchedFade > 1.0)
            {
                const double stretchedFromStart = juce::jmax(0.0, voice.stretchedReadPosition);
                const double stretchedToEnd = juce::jmax(0.0, stretchedLength - voice.stretchedReadPosition);
                gain *= static_cast<float>(juce::jlimit(0.0, 1.0, stretchedFromStart / stretchedFade));
                gain *= static_cast<float>(juce::jlimit(0.0, 1.0, stretchedToEnd / stretchedFade));
            }

            if (voice.fadeInRemaining > 0 && voice.fadeInTotal > 0)
            {
                const float fadeInGain = 1.0f - (static_cast<float>(voice.fadeInRemaining) / static_cast<float>(voice.fadeInTotal));
                gain *= juce::jlimit(0.0f, 1.0f, fadeInGain);
                --voice.fadeInRemaining;
            }

            if (voice.isReleasing())
            {
                if (voice.fadeOutRemaining > 0 && voice.fadeOutTotal > 0)
                {
                    const float fadeOutGain = static_cast<float>(voice.fadeOutRemaining) / static_cast<float>(voice.fadeOutTotal);
                    gain *= juce::jlimit(0.0f, 1.0f, fadeOutGain);
                    --voice.fadeOutRemaining;
                }
                else
                {
                    voice.stop();
                    break;
                }
            }

            if (gain > 1.0e-5f)
            {
                const float left = usingStretched
                    ? readInterpolatedSample(stretchedBuffer->audioBuffer, 0, voice.stretchedReadPosition)
                    : readInterpolatedSample(sampleData->audioBuffer, 0, voice.readPosition);
                const float right = usingStretched
                    ? readInterpolatedSample(stretchedBuffer->audioBuffer, 1, voice.stretchedReadPosition)
                    : readInterpolatedSample(sampleData->audioBuffer, 1, voice.readPosition);
                output.addSample(0, startSample + sampleIndex, left * gain);
                output.addSample(1, startSample + sampleIndex, right * gain);
            }

            voice.readPosition += sourceIncrement;
            if (usingStretched)
                voice.stretchedReadPosition += cacheIncrement;
            latestProgress = juce::jlimit(0.0f,
                                          1.0f,
                                          static_cast<float>(voice.readPosition
                                              / juce::jmax<int64_t>(1, sampleData->sourceLengthSamples)));
        }
    }

    activeVisibleSliceSlot.store(newestActiveSlot, std::memory_order_release);
    playbackProgress.store(renderedAnything ? latestProgress : -1.0f, std::memory_order_release);
    playingAtomic.store(renderedAnything ? 1 : 0, std::memory_order_release);
    result.renderedAnything = renderedAnything;
    result.usedInternalPitch = renderedAnything && canUseKeyLockForAllVoices;
    return result;
}

void SampleModeEngine::setLoadStatusCallback(LoadStatusCallback callback)
{
    const juce::ScopedLock lock(stateLock);
    loadStatusCallback = std::move(callback);
}

void SampleModeEngine::setLegacyLoopRenderStateChangedCallback(LegacyLoopRenderStateChangedCallback callback)
{
    const juce::ScopedLock lock(stateLock);
    legacyLoopRenderStateChangedCallback = std::move(callback);
}

void SampleModeEngine::beginInteractiveLegacyLoopEdit()
{
    const juce::ScopedLock lock(stateLock);
    interactiveLegacyLoopEditActive = true;
    interactiveLegacyLoopEditDirty = false;
}

void SampleModeEngine::endInteractiveLegacyLoopEdit()
{
    bool shouldNotify = false;
    {
        const juce::ScopedLock lock(stateLock);
        if (!interactiveLegacyLoopEditActive)
            return;

        interactiveLegacyLoopEditActive = false;
        shouldNotify = interactiveLegacyLoopEditDirty;
        interactiveLegacyLoopEditDirty = false;
    }

    if (shouldNotify)
        notifyLegacyLoopRenderStateChanged();
}

void SampleModeEngine::beginInteractiveWarpEdit()
{
    const juce::ScopedLock lock(stateLock);
    interactiveWarpEditActive = true;
    interactiveWarpEditDirty = false;
}

void SampleModeEngine::endInteractiveWarpEdit()
{
    bool shouldNotify = false;
    {
        const juce::ScopedLock lock(stateLock);
        if (!interactiveWarpEditActive)
            return;

        interactiveWarpEditActive = false;
        shouldNotify = interactiveWarpEditDirty;
        interactiveWarpEditDirty = false;
    }

    if (shouldNotify)
        notifyLegacyLoopRenderStateChanged();
}

void SampleModeEngine::notifyLegacyLoopRenderStateChanged()
{
    LegacyLoopRenderStateChangedCallback callbackToInvoke;
    legacyLoopRenderStateVersion.fetch_add(1, std::memory_order_acq_rel);
    {
        const juce::ScopedLock lock(stateLock);
        callbackToInvoke = legacyLoopRenderStateChangedCallback;
    }

    if (callbackToInvoke)
        callbackToInvoke();
}

SampleModePersistentState SampleModeEngine::capturePersistentState() const
{
    const juce::ScopedLock lock(stateLock);
    auto state = persistentState;
    state.visibleSliceBankIndex = sliceModel.getVisibleBankIndex();
    state.storedSlices.clear();
    if (loadedSample != nullptr)
    {
        state.samplePath = loadedSample->sourcePath;
        if (!(state.analyzedTempoBpm > 0.0))
            state.analyzedTempoBpm = loadedSample->analysis.estimatedTempoBpm;
        if (!(state.analyzedPitchHz > 0.0))
            state.analyzedPitchHz = loadedSample->analysis.estimatedPitchHz;
        if (state.analyzedPitchMidi < 0)
            state.analyzedPitchMidi = loadedSample->analysis.estimatedPitchMidi;
        state.essentiaUsed = loadedSample->analysis.essentiaUsed;
        if (state.analysisSource.isEmpty())
            state.analysisSource = loadedSample->analysis.analysisSource;
    }
    return state;
}

void SampleModeEngine::applyPersistentState(const SampleModePersistentState& state)
{
    {
        const juce::ScopedLock lock(stateLock);
        persistentState = state;
        persistentState.beatDivision = juce::jlimit(1, 8, persistentState.beatDivision);
        persistentState.viewZoom = juce::jlimit(kMinViewZoom, kMaxViewZoom, persistentState.viewZoom);
        persistentState.viewScroll = juce::jlimit(0.0f, 1.0f, persistentState.viewScroll);
        persistentState.selectedCueIndex = clampCueSelection(persistentState.selectedCueIndex,
                                                             static_cast<int>(persistentState.cuePoints.size()));
        persistentState.storedSlices.clear();
        if (!persistentState.transientMarkersEdited)
            persistentState.transientEditSamples.clear();
        invalidateTransientMarkerCachesLocked();
        if (loadedSample != nullptr)
        {
            persistentState.warpMarkers = sanitizePersistentWarpMarkers(persistentState.warpMarkers,
                                                                       loadedSample->sourceLengthSamples);
            if (persistentState.transientMarkersEdited)
                quantizeTransientEditSamplesToGuidesLocked();
        }
        clearRandomVisibleSliceOverrideLocked();
        if (loadedSample != nullptr)
        {
            rebuildSlicesLocked();
            [[maybe_unused]] const auto warmedCanonicalMarkers = buildCanonicalTransientMarkerSamplesLocked();
            sliceModel.setVisibleBankIndex(persistentState.visibleSliceBankIndex);
        }
        legacyLoopTransientPageStartIndex = 0;
    }

    notifyLegacyLoopRenderStateChanged();
    sendChangeMessage();
}

void SampleModeEngine::setSliceMode(SampleSliceMode mode)
{
    {
        const juce::ScopedLock lock(stateLock);
        persistentState.sliceMode = mode;
        persistentState.storedSlices.clear();
        clearRandomVisibleSliceOverrideLocked();
        invalidateLegacyLoopTransientWindowCacheLocked();
        if (mode != SampleSliceMode::Manual)
            persistentState.selectedCueIndex = -1;
        if (mode == SampleSliceMode::Transient)
            materializeTransientMarkersLocked();
        legacyLoopTransientPageStartIndex = 0;
        rebuildSlicesLocked();
    }

    notifyLegacyLoopRenderStateChanged();
    sendChangeMessage();
}

SampleSliceMode SampleModeEngine::getSliceMode() const
{
    const juce::ScopedLock lock(stateLock);
    return persistentState.sliceMode;
}

void SampleModeEngine::setTriggerMode(SampleTriggerMode mode)
{
    {
        const juce::ScopedLock lock(stateLock);
        persistentState.triggerMode = mode;
    }
    sendChangeMessage();
}

SampleTriggerMode SampleModeEngine::getTriggerMode() const
{
    const juce::ScopedLock lock(stateLock);
    return persistentState.triggerMode;
}

void SampleModeEngine::setLegacyLoopEngineEnabled(bool enabled)
{
    {
        const juce::ScopedLock lock(stateLock);
        persistentState.useLegacyLoopEngine = enabled;
        if (enabled)
            clearRandomVisibleSliceOverrideLocked();
        invalidateLegacyLoopTransientWindowCacheLocked();
        if (enabled && legacyLoopWindowStartSample < 0)
        {
            legacyLoopWindowStartSample = getDefaultLegacyLoopWindowStartSampleLocked();
            legacyLoopWindowManualAnchor = false;
        }
        else if (!enabled)
        {
            legacyLoopWindowStartSample = -1;
            legacyLoopWindowManualAnchor = false;
        }
        legacyLoopTransientPageStartIndex = 0;
        if (persistentState.legacyLoopBarSelection > 0)
            fitViewToLegacyLoopWindowLocked();
    }
    if (!enabled)
        clearLegacyLoopMonitorState();
    notifyLegacyLoopRenderStateChanged();
    sendChangeMessage();
}

bool SampleModeEngine::isLegacyLoopEngineEnabled() const
{
    const juce::ScopedLock lock(stateLock);
    return persistentState.useLegacyLoopEngine;
}

void SampleModeEngine::setLegacyLoopBarSelection(int selection)
{
    {
        const juce::ScopedLock lock(stateLock);
        if (selection > 0)
            clearRandomVisibleSliceOverrideLocked();
        invalidateLegacyLoopTransientWindowCacheLocked();
        switch (selection)
        {
            case 25:
            case 50:
            case 100:
            case 200:
            case 400:
            case 800:
                persistentState.legacyLoopBarSelection = selection;
                break;
            default:
                persistentState.legacyLoopBarSelection = 0;
                break;
        }
        if (persistentState.legacyLoopBarSelection > 0
            && persistentState.useLegacyLoopEngine
            && legacyLoopWindowStartSample < 0)
        {
            legacyLoopWindowStartSample = getDefaultLegacyLoopWindowStartSampleLocked();
            legacyLoopWindowManualAnchor = false;
        }
        else if (persistentState.legacyLoopBarSelection == 0)
        {
            legacyLoopWindowStartSample = -1;
            legacyLoopWindowManualAnchor = false;
        }
        legacyLoopTransientPageStartIndex = 0;
        if (persistentState.legacyLoopBarSelection > 0)
        {
            if (legacyLoopWindowStartSample < 0)
            {
                legacyLoopWindowStartSample = getDefaultLegacyLoopWindowStartSampleLocked();
                legacyLoopWindowManualAnchor = false;
            }
            fitViewToLegacyLoopWindowLocked();
        }
    }
    notifyLegacyLoopRenderStateChanged();
    sendChangeMessage();
}

int SampleModeEngine::getLegacyLoopBarSelection() const
{
    const juce::ScopedLock lock(stateLock);
    return persistentState.legacyLoopBarSelection;
}

bool SampleModeEngine::nudgeLegacyLoopWindowByAnchorDelta(int delta)
{
    if (delta == 0)
        return false;

    bool changed = false;
    bool shouldNotifyRenderState = false;
    {
        const juce::ScopedLock lock(stateLock);
        if (loadedSample == nullptr
            || !persistentState.useLegacyLoopEngine)
        {
            return false;
        }

        const auto candidates = buildLegacyLoopAnchorCandidatesLocked();
        if (candidates.empty())
            return false;

        if (persistentState.legacyLoopBarSelection > 0)
        {
            int64_t currentRangeStartSample = 0;
            int64_t currentRangeEndSample = 0;
            const bool hasCurrentRange = computeLegacyLoopWindowRangeLocked(currentRangeStartSample, currentRangeEndSample);
            const int64_t currentAnchor = hasCurrentRange
                ? currentRangeStartSample
                : (legacyLoopWindowStartSample >= 0 ? legacyLoopWindowStartSample : candidates.front());

            int currentIndex = findNearestBeatTickIndex(candidates, currentAnchor);
            if (currentIndex < 0)
                currentIndex = 0;

            const int targetIndex = juce::jlimit(0,
                                                 static_cast<int>(candidates.size()) - 1,
                                                 currentIndex + delta);
            const int64_t targetAnchor = candidates[static_cast<size_t>(targetIndex)];
            if (targetAnchor != legacyLoopWindowStartSample || !legacyLoopWindowManualAnchor)
            {
                legacyLoopWindowStartSample = targetAnchor;
                legacyLoopWindowManualAnchor = true;
                legacyLoopTransientPageStartIndex = 0;
                invalidateLegacyLoopTransientWindowCacheLocked();
                const double seconds = loadedSample->sourceSampleRate > 0.0
                    ? static_cast<double>(legacyLoopWindowStartSample) / loadedSample->sourceSampleRate
                    : 0.0;
                statusText = "Viewing MLR selection @ " + juce::String(seconds, 2) + "s";
                changed = true;
                if (interactiveLegacyLoopEditActive)
                    interactiveLegacyLoopEditDirty = true;
                else
                    shouldNotifyRenderState = true;
            }
        }
        else
        {
            const auto& allSlices = sliceModel.getAllSlices();
            if (allSlices.empty())
                return false;

            const auto visibleSlices = getCurrentVisibleSlicesLocked();
            int64_t currentAnchor = -1;
            for (const auto& slice : visibleSlices)
            {
                if (slice.id >= 0 && slice.endSample > slice.startSample)
                {
                    currentAnchor = slice.startSample;
                    break;
                }
            }

            if (currentAnchor < 0)
                currentAnchor = candidates.front();

            int currentIndex = findNearestBeatTickIndex(candidates, currentAnchor);
            if (currentIndex < 0)
                currentIndex = 0;

            const int targetIndex = juce::jlimit(0,
                                                 static_cast<int>(candidates.size()) - 1,
                                                 currentIndex + delta);
            const int64_t targetAnchor = candidates[static_cast<size_t>(targetIndex)];

            int targetSliceIndex = 0;
            int64_t bestDistance = std::numeric_limits<int64_t>::max();
            for (int sliceIndex = 0; sliceIndex < static_cast<int>(allSlices.size()); ++sliceIndex)
            {
                const auto& slice = allSlices[static_cast<size_t>(sliceIndex)];
                if (slice.id < 0 || slice.endSample <= slice.startSample)
                    continue;

                const int64_t distance = std::abs(slice.startSample - targetAnchor);
                if (distance < bestDistance)
                {
                    bestDistance = distance;
                    targetSliceIndex = sliceIndex;
                    if (distance == 0)
                        break;
                }
            }

            const auto nextVisibleSlices = buildSequentialVisibleSliceSelection(allSlices, targetSliceIndex);
            bool selectionChanged = !randomVisibleSliceOverrideActive;
            if (!selectionChanged)
            {
                for (int slot = 0; slot < SliceModel::VisibleSliceCount; ++slot)
                {
                    const auto& existingSlice = randomVisibleSlices[static_cast<size_t>(slot)];
                    const auto& nextSlice = nextVisibleSlices[static_cast<size_t>(slot)];
                    if (existingSlice.id != nextSlice.id
                        || existingSlice.startSample != nextSlice.startSample
                        || existingSlice.endSample != nextSlice.endSample)
                    {
                        selectionChanged = true;
                        break;
                    }
                }
            }

            if (selectionChanged)
            {
                randomVisibleSlices = nextVisibleSlices;
                randomVisibleSliceOverrideActive = true;
                const int64_t displayStartSample = randomVisibleSlices.front().id >= 0
                    ? randomVisibleSlices.front().startSample
                    : targetAnchor;
                const double seconds = loadedSample->sourceSampleRate > 0.0
                    ? static_cast<double>(displayStartSample) / loadedSample->sourceSampleRate
                    : 0.0;
                statusText = "Viewing auto MLR selection @ " + juce::String(seconds, 2) + "s";
                changed = true;
                if (interactiveLegacyLoopEditActive)
                    interactiveLegacyLoopEditDirty = true;
                else
                    shouldNotifyRenderState = true;
            }
        }
    }

    if (shouldNotifyRenderState)
        notifyLegacyLoopRenderStateChanged();

    if (changed)
    {
        sendChangeMessage();
    }
    return changed;
}

void SampleModeEngine::scaleAnalyzedTempo(double factor)
{
    if (!std::isfinite(factor) || factor <= 0.0)
        return;

    {
        const juce::ScopedLock lock(stateLock);
        const double currentTempo = persistentState.analyzedTempoBpm > 0.0
            ? persistentState.analyzedTempoBpm
            : (loadedSample != nullptr ? loadedSample->analysis.estimatedTempoBpm : 0.0);
        if (!(currentTempo > 0.0))
            return;

        persistentState.analyzedTempoBpm = juce::jlimit(20.0, 400.0, currentTempo * factor);
        if (persistentState.analysisSource.isEmpty())
            persistentState.analysisSource = "manual tempo";
    }

    notifyLegacyLoopRenderStateChanged();
    sendChangeMessage();
}

double SampleModeEngine::getAnalyzedTempoBpm() const
{
    const juce::ScopedLock lock(stateLock);
    if (persistentState.analyzedTempoBpm > 0.0)
        return persistentState.analyzedTempoBpm;
    return loadedSample != nullptr ? loadedSample->analysis.estimatedTempoBpm : 0.0;
}

void SampleModeEngine::setBeatDivision(int division)
{
    {
        const juce::ScopedLock lock(stateLock);
        persistentState.beatDivision = juce::jlimit(1, 8, division);
        persistentState.storedSlices.clear();
        clearRandomVisibleSliceOverrideLocked();
        if (persistentState.sliceMode == SampleSliceMode::Beat)
            rebuildSlicesLocked();
    }
    notifyLegacyLoopRenderStateChanged();
    sendChangeMessage();
}

int SampleModeEngine::getBeatDivision() const
{
    const juce::ScopedLock lock(stateLock);
    return persistentState.beatDivision;
}

void SampleModeEngine::setViewWindow(float scroll, float zoom)
{
    {
        const juce::ScopedLock lock(stateLock);
        persistentState.viewZoom = juce::jlimit(kMinViewZoom, kMaxViewZoom, zoom);
        persistentState.viewScroll = juce::jlimit(0.0f, 1.0f, scroll);
    }
    sendChangeMessage();
}

float SampleModeEngine::getViewScroll() const
{
    const juce::ScopedLock lock(stateLock);
    return persistentState.viewScroll;
}

float SampleModeEngine::getViewZoom() const
{
    const juce::ScopedLock lock(stateLock);
    return persistentState.viewZoom;
}

void SampleModeEngine::selectCuePoint(int cueIndex)
{
    {
        const juce::ScopedLock lock(stateLock);
        persistentState.selectedCueIndex = clampCueSelection(cueIndex, static_cast<int>(persistentState.cuePoints.size()));
    }
    sendChangeMessage();
}

int SampleModeEngine::getSelectedCuePoint() const
{
    const juce::ScopedLock lock(stateLock);
    return persistentState.selectedCueIndex;
}

bool SampleModeEngine::canUndoSliceEdit() const
{
    const juce::ScopedLock lock(stateLock);
    return !sliceEditUndoStack.empty();
}

void SampleModeEngine::beginSliceEditGesture()
{
    const juce::ScopedLock lock(stateLock);
    sliceEditGestureActive = true;
    sliceEditGestureUndoCaptured = false;
}

void SampleModeEngine::endSliceEditGesture()
{
    const juce::ScopedLock lock(stateLock);
    sliceEditGestureActive = false;
    sliceEditGestureUndoCaptured = false;
}

bool SampleModeEngine::undoLastSliceEdit()
{
    bool changed = false;
    {
        const juce::ScopedLock lock(stateLock);
        if (sliceEditUndoStack.empty())
            return false;

        const auto state = std::move(sliceEditUndoStack.back());
        sliceEditUndoStack.pop_back();
        persistentState.cuePoints = state.cuePoints;
        persistentState.selectedCueIndex = clampCueSelection(state.selectedCueIndex,
                                                             static_cast<int>(persistentState.cuePoints.size()));
        persistentState.transientMarkersEdited = state.transientMarkersEdited;
        persistentState.transientEditSamples = state.transientEditSamples;
        persistentState.warpMarkers = loadedSample != nullptr
            ? sanitizePersistentWarpMarkers(state.warpMarkers, loadedSample->sourceLengthSamples)
            : state.warpMarkers;
        invalidateTransientMarkerCachesLocked();
        persistentState.storedSlices.clear();
        rebuildSlicesLocked();
        changed = true;
    }

    if (!changed)
        return false;

    notifyLegacyLoopRenderStateChanged();
    sendChangeMessage();
    return true;
}

int SampleModeEngine::createCuePointAtNormalizedPosition(float normalizedPosition)
{
    int selectedIndex = -1;
    {
        const juce::ScopedLock lock(stateLock);
        if (loadedSample == nullptr || loadedSample->sourceLengthSamples <= 1)
            return -1;

        const int64_t cueSample = static_cast<int64_t>(
            std::llround(juce::jlimit(0.0f, 1.0f, normalizedPosition) * static_cast<float>(loadedSample->sourceLengthSamples - 1)));
        if (cueSample <= 0 || cueSample >= loadedSample->sourceLengthSamples)
            return -1;

        pushSliceEditUndoStateLocked();
        SampleCuePoint cue;
        cue.id = static_cast<int>(persistentState.cuePoints.size());
        cue.samplePosition = cueSample;
        cue.name = buildCueName(cue.id);
        cue.loopEndSample = loadedSample->sourceLengthSamples;
        persistentState.cuePoints.push_back(std::move(cue));
        std::sort(persistentState.cuePoints.begin(), persistentState.cuePoints.end(),
                  [](const SampleCuePoint& a, const SampleCuePoint& b) { return a.samplePosition < b.samplePosition; });
        for (size_t i = 0; i < persistentState.cuePoints.size(); ++i)
        {
            if (persistentState.cuePoints[i].samplePosition == cueSample)
            {
                persistentState.selectedCueIndex = static_cast<int>(i);
                break;
            }
        }
        persistentState.storedSlices.clear();
        if (persistentState.sliceMode == SampleSliceMode::Manual)
            rebuildSlicesLocked();
        selectedIndex = persistentState.selectedCueIndex;
    }

    notifyLegacyLoopRenderStateChanged();
    sendChangeMessage();
    return selectedIndex;
}

int SampleModeEngine::moveCuePoint(int cueIndex, float normalizedPosition)
{
    int newIndex = -1;
    {
        const juce::ScopedLock lock(stateLock);
        if (loadedSample == nullptr
            || cueIndex < 0
            || cueIndex >= static_cast<int>(persistentState.cuePoints.size()))
        {
            return -1;
        }

        const int cueId = persistentState.cuePoints[static_cast<size_t>(cueIndex)].id;
        const int64_t cueSample = juce::jlimit<int64_t>(
            1,
            juce::jmax<int64_t>(1, loadedSample->sourceLengthSamples - 1),
            static_cast<int64_t>(std::llround(juce::jlimit(0.0f, 1.0f, normalizedPosition)
                                              * static_cast<float>(loadedSample->sourceLengthSamples - 1))));
        if (persistentState.cuePoints[static_cast<size_t>(cueIndex)].samplePosition == cueSample)
            return cueIndex;

        pushSliceEditUndoStateLocked();
        persistentState.cuePoints[static_cast<size_t>(cueIndex)].samplePosition = cueSample;
        std::sort(persistentState.cuePoints.begin(), persistentState.cuePoints.end(),
                  [](const SampleCuePoint& a, const SampleCuePoint& b) { return a.samplePosition < b.samplePosition; });
        for (size_t i = 0; i < persistentState.cuePoints.size(); ++i)
        {
            if (persistentState.cuePoints[i].id == cueId)
            {
                persistentState.selectedCueIndex = static_cast<int>(i);
                newIndex = static_cast<int>(i);
                break;
            }
        }
        persistentState.storedSlices.clear();
        if (persistentState.sliceMode == SampleSliceMode::Manual)
            rebuildSlicesLocked();
    }

    notifyLegacyLoopRenderStateChanged();
    sendChangeMessage();
    return newIndex;
}

bool SampleModeEngine::deleteCuePoint(int cueIndex)
{
    {
        const juce::ScopedLock lock(stateLock);
        if (cueIndex < 0 || cueIndex >= static_cast<int>(persistentState.cuePoints.size()))
            return false;

        pushSliceEditUndoStateLocked();
        persistentState.cuePoints.erase(persistentState.cuePoints.begin() + cueIndex);
        persistentState.selectedCueIndex = clampCueSelection(cueIndex - 1, static_cast<int>(persistentState.cuePoints.size()));
        persistentState.storedSlices.clear();
        if (persistentState.sliceMode == SampleSliceMode::Manual)
            rebuildSlicesLocked();
    }

    notifyLegacyLoopRenderStateChanged();
    sendChangeMessage();
    return true;
}

int SampleModeEngine::createWarpMarkerAtNormalizedPosition(float normalizedPosition)
{
    int newIndex = -1;
    {
        const juce::ScopedLock lock(stateLock);
        if (loadedSample == nullptr || loadedSample->sourceLengthSamples <= 1)
            return -1;

        const auto beatTicks = sanitizeWarpBeatTicks(loadedSample->analysis.beatTickSamples,
                                                     loadedSample->sourceLengthSamples);
        if (beatTicks.size() < 2)
            return -1;

        const int64_t maxMarkerSample = juce::jmax<int64_t>(1, loadedSample->sourceLengthSamples - 2);
        const int64_t samplePosition = juce::jlimit<int64_t>(
            1,
            maxMarkerSample,
            static_cast<int64_t>(std::llround(juce::jlimit(0.0f, 1.0f, normalizedPosition)
                                              * static_cast<float>(loadedSample->sourceLengthSamples - 1))));
        const double beatPosition = WarpGrid::computeWarpedBeatPositionForSample(beatTicks,
                                                                                 persistentState.warpMarkers,
                                                                                 samplePosition,
                                                                                 loadedSample->sourceLengthSamples);
        if (!std::isfinite(beatPosition))
            return -1;

        persistentState.warpMarkers = sanitizePersistentWarpMarkers(persistentState.warpMarkers,
                                                                    loadedSample->sourceLengthSamples);
        for (size_t i = 0; i < persistentState.warpMarkers.size(); ++i)
        {
            const auto& marker = persistentState.warpMarkers[i];
            if (std::abs(marker.samplePosition - samplePosition) <= kWarpMarkerDuplicateSampleTolerance
                || std::abs(marker.beatPosition - beatPosition) <= WarpGrid::kMarkerOrderEpsilon)
            {
                return static_cast<int>(i);
            }
        }

        int nextId = 0;
        for (const auto& marker : persistentState.warpMarkers)
            nextId = juce::jmax(nextId, marker.id + 1);

        SampleWarpMarker marker;
        marker.id = nextId;
        marker.samplePosition = samplePosition;
        marker.beatPosition = beatPosition;
        persistentState.warpMarkers.push_back(marker);
        persistentState.warpMarkers = sanitizePersistentWarpMarkers(persistentState.warpMarkers,
                                                                    loadedSample->sourceLengthSamples);
        newIndex = findWarpMarkerIndexById(persistentState.warpMarkers, marker.id);
    }

    notifyLegacyLoopRenderStateChanged();
    sendChangeMessage();
    return newIndex;
}

int SampleModeEngine::createWarpMarkerAtNearestGuide(float normalizedPosition)
{
    int newIndex = -1;
    {
        const juce::ScopedLock lock(stateLock);
        if (loadedSample == nullptr || loadedSample->sourceLengthSamples <= 1)
            return -1;

        const auto beatTicks = sanitizeWarpBeatTicks(loadedSample->analysis.beatTickSamples,
                                                     loadedSample->sourceLengthSamples);
        if (beatTicks.size() < 2)
            return -1;

        const int64_t maxMarkerSample = juce::jmax<int64_t>(1, loadedSample->sourceLengthSamples - 2);
        const int64_t targetSamplePosition = juce::jlimit<int64_t>(
            1,
            maxMarkerSample,
            static_cast<int64_t>(std::llround(juce::jlimit(0.0f, 1.0f, normalizedPosition)
                                              * static_cast<float>(loadedSample->sourceLengthSamples - 1))));
        const auto transientMarkers = buildCanonicalTransientMarkerSamples(*loadedSample,
                                                                           persistentState.transientEditSamples,
                                                                           persistentState.transientMarkersEdited);
        std::vector<int64_t> clusteredTransientGuides;
        int64_t loopRangeStartSample = 0;
        int64_t loopRangeEndSample = 0;
        if (computeLegacyLoopWindowRangeLocked(loopRangeStartSample, loopRangeEndSample)
            && targetSamplePosition >= loopRangeStartSample
            && targetSamplePosition < loopRangeEndSample)
        {
            clusteredTransientGuides = clusterWarpMarkerTransientGuides(
                buildLegacyLoopWindowTransientCandidatesLocked(loopRangeStartSample, loopRangeEndSample),
                1,
                maxMarkerSample,
                loadedSample->sourceSampleRate);
        }

        if (clusteredTransientGuides.empty())
        {
            clusteredTransientGuides = clusterWarpMarkerTransientGuides(transientMarkers,
                                                                        1,
                                                                        maxMarkerSample,
                                                                        loadedSample->sourceSampleRate);
        }

        const int64_t samplePosition = snapWarpMarkerSampleToGuide(beatTicks,
                                                                   clusteredTransientGuides,
                                                                   targetSamplePosition,
                                                                   1,
                                                                   maxMarkerSample,
                                                                   loadedSample->sourceSampleRate);
        const double beatPosition = WarpGrid::computeWarpedBeatPositionForSample(beatTicks,
                                                                                 persistentState.warpMarkers,
                                                                                 samplePosition,
                                                                                 loadedSample->sourceLengthSamples);
        if (!std::isfinite(beatPosition))
            return -1;

        persistentState.warpMarkers = sanitizePersistentWarpMarkers(persistentState.warpMarkers,
                                                                    loadedSample->sourceLengthSamples);
        for (size_t i = 0; i < persistentState.warpMarkers.size(); ++i)
        {
            const auto& marker = persistentState.warpMarkers[i];
            if (std::abs(marker.samplePosition - samplePosition) <= kWarpMarkerDuplicateSampleTolerance
                || std::abs(marker.beatPosition - beatPosition) <= WarpGrid::kMarkerOrderEpsilon)
            {
                return static_cast<int>(i);
            }
        }

        int nextId = 0;
        for (const auto& marker : persistentState.warpMarkers)
            nextId = juce::jmax(nextId, marker.id + 1);

        SampleWarpMarker marker;
        marker.id = nextId;
        marker.samplePosition = samplePosition;
        marker.beatPosition = beatPosition;
        persistentState.warpMarkers.push_back(marker);
        persistentState.warpMarkers = sanitizePersistentWarpMarkers(persistentState.warpMarkers,
                                                                    loadedSample->sourceLengthSamples);
        newIndex = findWarpMarkerIndexById(persistentState.warpMarkers, marker.id);
    }

    notifyLegacyLoopRenderStateChanged();
    sendChangeMessage();
    return newIndex;
}

int SampleModeEngine::createWarpMarkersFromVisibleTransientClusters()
{
    int insertedCount = 0;
    {
        const juce::ScopedLock lock(stateLock);
        if (loadedSample == nullptr || loadedSample->sourceLengthSamples <= 1)
            return 0;

        const auto beatTicks = sanitizeWarpBeatTicks(loadedSample->analysis.beatTickSamples,
                                                     loadedSample->sourceLengthSamples);
        if (beatTicks.size() < 2)
            return 0;

        const int64_t maxMarkerSample = juce::jmax<int64_t>(1, loadedSample->sourceLengthSamples - 2);
        int64_t rangeStartSample = 1;
        int64_t rangeEndSample = maxMarkerSample + 1;
        if (!computeLegacyLoopWindowRangeLocked(rangeStartSample, rangeEndSample))
        {
            const auto visibleSlices = getCurrentVisibleSlicesLocked();
            int64_t derivedStartSample = std::numeric_limits<int64_t>::max();
            int64_t derivedEndSample = 0;
            for (const auto& slice : visibleSlices)
            {
                if (slice.id < 0 || slice.endSample <= slice.startSample)
                    continue;

                derivedStartSample = juce::jmin(derivedStartSample, slice.startSample);
                derivedEndSample = juce::jmax(derivedEndSample, slice.endSample);
            }

            if (derivedStartSample != std::numeric_limits<int64_t>::max() && derivedEndSample > derivedStartSample)
            {
                rangeStartSample = derivedStartSample;
                rangeEndSample = derivedEndSample;
            }
        }

        const int64_t candidateLowerBound = juce::jlimit<int64_t>(1, maxMarkerSample, rangeStartSample);
        const int64_t candidateUpperBound = juce::jlimit<int64_t>(candidateLowerBound,
                                                                  maxMarkerSample,
                                                                  juce::jmax<int64_t>(candidateLowerBound, rangeEndSample - 1));
        if (candidateUpperBound < candidateLowerBound)
            return 0;

        std::vector<int64_t> transientCandidates;
        if (computeLegacyLoopWindowRangeLocked(rangeStartSample, rangeEndSample))
        {
            transientCandidates = clusterWarpMarkerTransientGuides(
                buildLegacyLoopWindowTransientCandidatesLocked(rangeStartSample, rangeEndSample),
                candidateLowerBound,
                candidateUpperBound,
                loadedSample->sourceSampleRate);
        }

        if (transientCandidates.empty())
        {
            const auto transientMarkers = buildCanonicalTransientMarkerSamples(*loadedSample,
                                                                               persistentState.transientEditSamples,
                                                                               persistentState.transientMarkersEdited);
            transientCandidates = clusterWarpMarkerTransientGuides(transientMarkers,
                                                                   candidateLowerBound,
                                                                   candidateUpperBound,
                                                                   loadedSample->sourceSampleRate);
        }

        if (transientCandidates.empty())
            return 0;

        persistentState.warpMarkers = sanitizePersistentWarpMarkers(persistentState.warpMarkers,
                                                                    loadedSample->sourceLengthSamples);
        const auto baseWarpMarkers = persistentState.warpMarkers;
        auto updatedWarpMarkers = persistentState.warpMarkers;

        auto alreadyAnchored = [&](int64_t samplePosition, double beatPosition)
        {
            for (const auto& marker : updatedWarpMarkers)
            {
                if (std::abs(marker.samplePosition - samplePosition) <= kWarpMarkerDuplicateSampleTolerance
                    || std::abs(marker.beatPosition - beatPosition) <= WarpGrid::kMarkerOrderEpsilon)
                {
                    return true;
                }
            }

            return false;
        };

        int nextId = 0;
        for (const auto& marker : updatedWarpMarkers)
            nextId = juce::jmax(nextId, marker.id + 1);

        for (const auto candidateSample : transientCandidates)
        {
            if (insertedCount >= kWarpAutoMarkerMaxInsertions)
                break;

            if (candidateSample <= 0 || candidateSample >= maxMarkerSample)
                continue;

            const double beatPosition = WarpGrid::computeWarpedBeatPositionForSample(beatTicks,
                                                                                     baseWarpMarkers,
                                                                                     candidateSample,
                                                                                     loadedSample->sourceLengthSamples);
            if (!std::isfinite(beatPosition) || alreadyAnchored(candidateSample, beatPosition))
                continue;

            SampleWarpMarker marker;
            marker.id = nextId++;
            marker.samplePosition = candidateSample;
            marker.beatPosition = beatPosition;
            updatedWarpMarkers.push_back(marker);
            ++insertedCount;
        }

        if (insertedCount <= 0)
            return 0;

        persistentState.warpMarkers = sanitizePersistentWarpMarkers(updatedWarpMarkers,
                                                                    loadedSample->sourceLengthSamples);
    }

    notifyLegacyLoopRenderStateChanged();
    sendChangeMessage();
    return insertedCount;
}

int SampleModeEngine::moveWarpMarker(int markerIndex, float normalizedPosition)
{
    int resolvedIndex = -1;
    bool shouldNotifyRenderState = false;
    {
        const juce::ScopedLock lock(stateLock);
        if (loadedSample == nullptr
            || markerIndex < 0
            || markerIndex >= static_cast<int>(persistentState.warpMarkers.size()))
        {
            return -1;
        }

        persistentState.warpMarkers = sanitizePersistentWarpMarkers(persistentState.warpMarkers,
                                                                    loadedSample->sourceLengthSamples);
        if (markerIndex >= static_cast<int>(persistentState.warpMarkers.size()))
            return -1;

        const auto beatTicks = sanitizeWarpBeatTicks(loadedSample->analysis.beatTickSamples,
                                                     loadedSample->sourceLengthSamples);
        if (beatTicks.size() < 2)
            return -1;

        const auto markerId = persistentState.warpMarkers[static_cast<size_t>(markerIndex)].id;
        const int64_t lowerBound = (markerIndex > 0)
            ? (persistentState.warpMarkers[static_cast<size_t>(markerIndex - 1)].samplePosition + 1)
            : 1;
        const int64_t maxMarkerSample = juce::jmax<int64_t>(1, loadedSample->sourceLengthSamples - 2);
        const int64_t upperBound = (markerIndex + 1 < static_cast<int>(persistentState.warpMarkers.size()))
            ? (persistentState.warpMarkers[static_cast<size_t>(markerIndex + 1)].samplePosition - 1)
            : maxMarkerSample;
        if (upperBound < lowerBound)
            return markerIndex;

        const int64_t unclampedSamplePosition = juce::jlimit<int64_t>(
            lowerBound,
            upperBound,
            static_cast<int64_t>(std::llround(juce::jlimit(0.0f, 1.0f, normalizedPosition)
                                              * static_cast<float>(loadedSample->sourceLengthSamples - 1))));
        const int64_t samplePosition = juce::jlimit<int64_t>(
            lowerBound,
            upperBound,
            clampWarpMarkerSamplePositionByStretchRatio(beatTicks,
                                                        persistentState.warpMarkers,
                                                        markerIndex,
                                                        unclampedSamplePosition,
                                                        loadedSample->sourceLengthSamples));
        if (persistentState.warpMarkers[static_cast<size_t>(markerIndex)].samplePosition == samplePosition)
            return markerIndex;

        persistentState.warpMarkers[static_cast<size_t>(markerIndex)].samplePosition = samplePosition;
        persistentState.warpMarkers = sanitizePersistentWarpMarkers(persistentState.warpMarkers,
                                                                    loadedSample->sourceLengthSamples);
        resolvedIndex = findWarpMarkerIndexById(persistentState.warpMarkers, markerId);
        if (interactiveWarpEditActive)
            interactiveWarpEditDirty = true;
        else
            shouldNotifyRenderState = true;
    }

    if (shouldNotifyRenderState)
        notifyLegacyLoopRenderStateChanged();
    sendChangeMessage();
    return resolvedIndex;
}

int SampleModeEngine::moveWarpMarkerToNearestGuide(int markerIndex, float normalizedPosition)
{
    int resolvedIndex = -1;
    bool shouldNotifyRenderState = false;
    {
        const juce::ScopedLock lock(stateLock);
        if (loadedSample == nullptr
            || markerIndex < 0
            || markerIndex >= static_cast<int>(persistentState.warpMarkers.size()))
        {
            return -1;
        }

        persistentState.warpMarkers = sanitizePersistentWarpMarkers(persistentState.warpMarkers,
                                                                    loadedSample->sourceLengthSamples);
        if (markerIndex >= static_cast<int>(persistentState.warpMarkers.size()))
            return -1;

        const auto beatTicks = sanitizeWarpBeatTicks(loadedSample->analysis.beatTickSamples,
                                                     loadedSample->sourceLengthSamples);
        if (beatTicks.size() < 2)
            return -1;

        const auto markerId = persistentState.warpMarkers[static_cast<size_t>(markerIndex)].id;
        const int64_t lowerBound = (markerIndex > 0)
            ? (persistentState.warpMarkers[static_cast<size_t>(markerIndex - 1)].samplePosition + 1)
            : 1;
        const int64_t maxMarkerSample = juce::jmax<int64_t>(1, loadedSample->sourceLengthSamples - 2);
        const int64_t upperBound = (markerIndex + 1 < static_cast<int>(persistentState.warpMarkers.size()))
            ? (persistentState.warpMarkers[static_cast<size_t>(markerIndex + 1)].samplePosition - 1)
            : maxMarkerSample;
        if (upperBound < lowerBound)
            return markerIndex;

        const int64_t unclampedSamplePosition = juce::jlimit<int64_t>(
            lowerBound,
            upperBound,
            static_cast<int64_t>(std::llround(juce::jlimit(0.0f, 1.0f, normalizedPosition)
                                              * static_cast<float>(loadedSample->sourceLengthSamples - 1))));
        const auto transientMarkers = buildCanonicalTransientMarkerSamples(*loadedSample,
                                                                           persistentState.transientEditSamples,
                                                                           persistentState.transientMarkersEdited);
        std::vector<int64_t> clusteredTransientGuides;
        int64_t loopRangeStartSample = 0;
        int64_t loopRangeEndSample = 0;
        if (computeLegacyLoopWindowRangeLocked(loopRangeStartSample, loopRangeEndSample)
            && unclampedSamplePosition >= loopRangeStartSample
            && unclampedSamplePosition < loopRangeEndSample)
        {
            clusteredTransientGuides = clusterWarpMarkerTransientGuides(
                buildLegacyLoopWindowTransientCandidatesLocked(loopRangeStartSample, loopRangeEndSample),
                lowerBound,
                upperBound,
                loadedSample->sourceSampleRate);
        }

        if (clusteredTransientGuides.empty())
        {
            clusteredTransientGuides = clusterWarpMarkerTransientGuides(transientMarkers,
                                                                        lowerBound,
                                                                        upperBound,
                                                                        loadedSample->sourceSampleRate);
        }

        const int64_t snappedSamplePosition = snapWarpMarkerSampleToGuide(beatTicks,
                                                                          clusteredTransientGuides,
                                                                          unclampedSamplePosition,
                                                                          lowerBound,
                                                                          upperBound,
                                                                          loadedSample->sourceSampleRate);
        const int64_t samplePosition = juce::jlimit<int64_t>(
            lowerBound,
            upperBound,
            clampWarpMarkerSamplePositionByStretchRatio(beatTicks,
                                                        persistentState.warpMarkers,
                                                        markerIndex,
                                                        snappedSamplePosition,
                                                        loadedSample->sourceLengthSamples));
        if (persistentState.warpMarkers[static_cast<size_t>(markerIndex)].samplePosition == samplePosition)
            return markerIndex;

        persistentState.warpMarkers[static_cast<size_t>(markerIndex)].samplePosition = samplePosition;
        persistentState.warpMarkers = sanitizePersistentWarpMarkers(persistentState.warpMarkers,
                                                                    loadedSample->sourceLengthSamples);
        resolvedIndex = findWarpMarkerIndexById(persistentState.warpMarkers, markerId);
        if (interactiveWarpEditActive)
            interactiveWarpEditDirty = true;
        else
            shouldNotifyRenderState = true;
    }

    if (shouldNotifyRenderState)
        notifyLegacyLoopRenderStateChanged();
    sendChangeMessage();
    return resolvedIndex;
}

bool SampleModeEngine::clearWarpMarkers()
{
    {
        const juce::ScopedLock lock(stateLock);
        if (persistentState.warpMarkers.empty())
            return false;

        persistentState.warpMarkers.clear();
    }

    notifyLegacyLoopRenderStateChanged();
    sendChangeMessage();
    return true;
}

bool SampleModeEngine::keepBoundaryWarpMarkers()
{
    {
        const juce::ScopedLock lock(stateLock);
        if (loadedSample == nullptr)
            return false;

        auto markers = sanitizePersistentWarpMarkers(persistentState.warpMarkers,
                                                     loadedSample->sourceLengthSamples);
        if (markers.size() <= 2)
            return false;

        persistentState.warpMarkers = { markers.front(), markers.back() };
    }

    notifyLegacyLoopRenderStateChanged();
    sendChangeMessage();
    return true;
}

bool SampleModeEngine::deleteWarpMarker(int markerIndex)
{
    {
        const juce::ScopedLock lock(stateLock);
        if (markerIndex < 0 || markerIndex >= static_cast<int>(persistentState.warpMarkers.size()))
            return false;

        persistentState.warpMarkers.erase(persistentState.warpMarkers.begin() + markerIndex);
    }

    notifyLegacyLoopRenderStateChanged();
    sendChangeMessage();
    return true;
}

void SampleModeEngine::materializeTransientMarkersLocked()
{
    if (loadedSample == nullptr)
        return;

    if (persistentState.transientMarkersEdited)
    {
        quantizeTransientEditSamplesToGuidesLocked();
        return;
    }

    persistentState.transientEditSamples = buildCanonicalTransientMarkerSamples(*loadedSample,
                                                                                {},
                                                                                false);
    persistentState.transientEditSamples.erase(
        std::remove_if(persistentState.transientEditSamples.begin(),
                       persistentState.transientEditSamples.end(),
                       [this](int64_t sample)
                       {
                           return sample <= 0 || sample >= loadedSample->sourceLengthSamples;
                       }),
        persistentState.transientEditSamples.end());
    persistentState.transientMarkersEdited = false;
}

void SampleModeEngine::resetSampleSpecificStateLocked()
{
    persistentState.samplePath.clear();
    persistentState.visibleSliceBankIndex = 0;
    persistentState.viewScroll = 0.0f;
    persistentState.selectedCueIndex = -1;
    persistentState.analyzedTempoBpm = 0.0;
    persistentState.analyzedPitchHz = 0.0;
    persistentState.analyzedPitchMidi = -1;
    persistentState.analysisSource.clear();
    persistentState.cuePoints.clear();
    persistentState.warpMarkers.clear();
    persistentState.transientMarkersEdited = false;
    persistentState.transientEditSamples.clear();
    persistentState.storedSlices.clear();
    clearRandomVisibleSliceOverrideLocked();
    invalidateTransientMarkerCachesLocked();
    legacyLoopWindowStartSample = -1;
    legacyLoopWindowManualAnchor = false;
    legacyLoopTransientPageStartIndex = 0;
    clearSliceEditUndoHistoryLocked();
}

void SampleModeEngine::pushSliceEditUndoStateLocked()
{
    if (sliceEditGestureActive && sliceEditGestureUndoCaptured)
        return;

    SliceEditUndoState undoState;
    undoState.cuePoints = persistentState.cuePoints;
    undoState.selectedCueIndex = persistentState.selectedCueIndex;
    undoState.transientMarkersEdited = persistentState.transientMarkersEdited;
    undoState.transientEditSamples = persistentState.transientEditSamples;
    undoState.warpMarkers = persistentState.warpMarkers;
    sliceEditUndoStack.push_back(std::move(undoState));
    if (sliceEditUndoStack.size() > kMaxSliceEditUndoDepth)
        sliceEditUndoStack.erase(sliceEditUndoStack.begin());

    if (sliceEditGestureActive)
        sliceEditGestureUndoCaptured = true;
}

void SampleModeEngine::clearSliceEditUndoHistoryLocked()
{
    sliceEditUndoStack.clear();
    sliceEditGestureActive = false;
    sliceEditGestureUndoCaptured = false;
}

void SampleModeEngine::invalidateCanonicalTransientMarkerCacheLocked()
{
    canonicalTransientMarkerCacheValid = false;
    canonicalTransientMarkerCache.clear();
}

void SampleModeEngine::invalidateLegacyLoopTransientWindowCacheLocked()
{
    legacyLoopTransientWindowCache = {};
}

void SampleModeEngine::invalidateTransientMarkerCachesLocked()
{
    invalidateCanonicalTransientMarkerCacheLocked();
    invalidateLegacyLoopTransientWindowCacheLocked();
}

int64_t SampleModeEngine::snapSampleToPpqGridLocked(int64_t samplePosition,
                                                    double gridStepBeats,
                                                    int64_t lowerBound,
                                                    int64_t upperBound) const
{
    if (loadedSample == nullptr
        || !(gridStepBeats > 0.0)
        || !std::isfinite(gridStepBeats)
        || upperBound < lowerBound)
    {
        return samplePosition;
    }

    const auto beatTicks = sanitizeWarpBeatTicks(loadedSample->analysis.beatTickSamples,
                                                 loadedSample->sourceLengthSamples);
    if (beatTicks.size() < 2)
        return juce::jlimit(lowerBound, upperBound, samplePosition);

    const double beatPosition = computeBeatTickPosition(beatTicks, samplePosition);
    if (!std::isfinite(beatPosition))
        return juce::jlimit(lowerBound, upperBound, samplePosition);

    const double snappedBeat = std::round(beatPosition / gridStepBeats) * gridStepBeats;
    const int64_t snappedSample = samplePositionFromBeatTickPosition(beatTicks,
                                                                     snappedBeat,
                                                                     loadedSample->sourceLengthSamples);
    return juce::jlimit(lowerBound, upperBound, snappedSample);
}

bool SampleModeEngine::snapTransientMarkersToPpqGridLocked(double gridStepBeats, bool currentSelectionOnly)
{
    if (loadedSample == nullptr
        || loadedSample->sourceLengthSamples <= 1
        || !(gridStepBeats > 0.0)
        || !std::isfinite(gridStepBeats))
    {
        return false;
    }

    const auto beatTicks = sanitizeWarpBeatTicks(loadedSample->analysis.beatTickSamples,
                                                 loadedSample->sourceLengthSamples);
    if (beatTicks.size() < 2)
        return false;

    const auto transientSamples = buildCanonicalTransientMarkerSamplesLocked();
    if (transientSamples.empty())
        return false;

    std::vector<int64_t> selectedMarkerSamples;
    if (currentSelectionOnly)
    {
        const auto visibleSlices = getCurrentVisibleSlicesLocked();
        selectedMarkerSamples.reserve(visibleSlices.size());
        for (const auto& slice : visibleSlices)
        {
            if (slice.id >= 0 && slice.startSample > 0)
                selectedMarkerSamples.push_back(slice.startSample);
        }

        std::sort(selectedMarkerSamples.begin(), selectedMarkerSamples.end());
        selectedMarkerSamples.erase(std::unique(selectedMarkerSamples.begin(), selectedMarkerSamples.end()),
                                    selectedMarkerSamples.end());
        if (selectedMarkerSamples.empty())
            return false;
    }

    auto updatedWarpMarkers = sanitizePersistentWarpMarkers(persistentState.warpMarkers,
                                                            loadedSample->sourceLengthSamples);
    int nextWarpMarkerId = 0;
    for (const auto& marker : updatedWarpMarkers)
        nextWarpMarkerId = juce::jmax(nextWarpMarkerId, marker.id + 1);

    const double startBeat = computeWarpBeatPositionFromSample(beatTicks, 0);
    const double endBeat = computeWarpBeatPositionFromSample(beatTicks,
                                                             juce::jmax<int64_t>(1, loadedSample->sourceLengthSamples - 1));
    if (!std::isfinite(startBeat) || !std::isfinite(endBeat) || endBeat <= startBeat)
        return false;

    bool changed = false;
    for (const auto currentSample : transientSamples)
    {
        if (currentSelectionOnly
            && !std::binary_search(selectedMarkerSamples.begin(), selectedMarkerSamples.end(), currentSample))
        {
            continue;
        }

        if (currentSample <= 0 || currentSample >= juce::jmax<int64_t>(1, loadedSample->sourceLengthSamples - 1))
            continue;

        updatedWarpMarkers = sanitizePersistentWarpMarkers(updatedWarpMarkers,
                                                           loadedSample->sourceLengthSamples);

        int existingMarkerIndex = -1;
        for (int markerIndex = 0; markerIndex < static_cast<int>(updatedWarpMarkers.size()); ++markerIndex)
        {
            if (std::abs(updatedWarpMarkers[static_cast<size_t>(markerIndex)].samplePosition - currentSample)
                <= kWarpMarkerDuplicateSampleTolerance)
            {
                existingMarkerIndex = markerIndex;
                break;
            }
        }

        auto orderingMarkers = updatedWarpMarkers;
        if (existingMarkerIndex >= 0)
            orderingMarkers.erase(orderingMarkers.begin() + existingMarkerIndex);

        const auto insertIt = std::lower_bound(orderingMarkers.begin(),
                                               orderingMarkers.end(),
                                               currentSample,
                                               [](const SampleWarpMarker& marker, int64_t samplePosition)
                                               {
                                                   return marker.samplePosition < samplePosition;
                                               });
        const int insertIndex = static_cast<int>(std::distance(orderingMarkers.begin(), insertIt));
        const double lowerBeatBound = (insertIndex > 0)
            ? orderingMarkers[static_cast<size_t>(insertIndex - 1)].beatPosition
            : startBeat;
        const double upperBeatBound = (insertIndex < static_cast<int>(orderingMarkers.size()))
            ? orderingMarkers[static_cast<size_t>(insertIndex)].beatPosition
            : endBeat;
        const double targetBeatPadding = juce::jmax(1.0e-4, WarpGrid::kMarkerOrderEpsilon * 16.0);
        if (upperBeatBound <= (lowerBeatBound + (targetBeatPadding * 2.0)))
            continue;

        const double currentWarpedBeat = WarpGrid::computeWarpedBeatPositionForSample(beatTicks,
                                                                                      updatedWarpMarkers,
                                                                                      currentSample,
                                                                                      loadedSample->sourceLengthSamples);
        if (!std::isfinite(currentWarpedBeat))
            continue;

        double snappedBeat = std::round(currentWarpedBeat / gridStepBeats) * gridStepBeats;
        snappedBeat = juce::jlimit(lowerBeatBound + targetBeatPadding,
                                   upperBeatBound - targetBeatPadding,
                                   snappedBeat);

        const double existingBeat = existingMarkerIndex >= 0
            ? updatedWarpMarkers[static_cast<size_t>(existingMarkerIndex)].beatPosition
            : currentWarpedBeat;
        if (std::abs(existingBeat - snappedBeat) <= 1.0e-4)
            continue;

        if (existingMarkerIndex >= 0)
        {
            updatedWarpMarkers[static_cast<size_t>(existingMarkerIndex)].beatPosition = snappedBeat;
        }
        else
        {
            SampleWarpMarker marker;
            marker.id = nextWarpMarkerId++;
            marker.samplePosition = currentSample;
            marker.beatPosition = snappedBeat;
            updatedWarpMarkers.push_back(marker);
        }

        updatedWarpMarkers = sanitizePersistentWarpMarkers(updatedWarpMarkers,
                                                           loadedSample->sourceLengthSamples);
        changed = true;
    }

    if (!changed)
        return false;

    persistentState.warpMarkers = std::move(updatedWarpMarkers);
    return true;
}

bool SampleModeEngine::snapCuePointsToPpqGridLocked(double gridStepBeats, bool currentSelectionOnly)
{
    if (loadedSample == nullptr || persistentState.cuePoints.empty())
        return false;

    std::vector<int64_t> selectedCueSamples;
    if (currentSelectionOnly)
    {
        const auto visibleSlices = getCurrentVisibleSlicesLocked();
        selectedCueSamples.reserve(visibleSlices.size());
        for (const auto& slice : visibleSlices)
        {
            if (slice.id >= 0 && slice.startSample > 0)
                selectedCueSamples.push_back(slice.startSample);
        }

        std::sort(selectedCueSamples.begin(), selectedCueSamples.end());
        selectedCueSamples.erase(std::unique(selectedCueSamples.begin(), selectedCueSamples.end()),
                                 selectedCueSamples.end());
        if (selectedCueSamples.empty())
            return false;
    }

    auto snappedCues = persistentState.cuePoints;
    bool changed = false;
    const int selectedCueId = (persistentState.selectedCueIndex >= 0
                               && persistentState.selectedCueIndex < static_cast<int>(persistentState.cuePoints.size()))
        ? persistentState.cuePoints[static_cast<size_t>(persistentState.selectedCueIndex)].id
        : -1;
    for (size_t i = 0; i < snappedCues.size(); ++i)
    {
        const auto currentSample = snappedCues[i].samplePosition;
        if (currentSelectionOnly
            && !std::binary_search(selectedCueSamples.begin(), selectedCueSamples.end(), currentSample))
        {
            continue;
        }

        const int64_t lowerBound = (i > 0) ? (snappedCues[i - 1].samplePosition + 1) : 1;
        const int64_t upperBound = (i + 1 < snappedCues.size())
            ? (snappedCues[i + 1].samplePosition - 1)
            : juce::jmax<int64_t>(1, loadedSample->sourceLengthSamples - 1);
        if (upperBound < lowerBound)
            continue;

        const int64_t snappedSample = snapSampleToPpqGridLocked(currentSample,
                                                                gridStepBeats,
                                                                lowerBound,
                                                                upperBound);
        changed = changed || (snappedSample != currentSample);
        snappedCues[i].samplePosition = snappedSample;
    }

    if (!changed)
        return false;

    std::sort(snappedCues.begin(), snappedCues.end(),
              [](const SampleCuePoint& a, const SampleCuePoint& b) { return a.samplePosition < b.samplePosition; });
    persistentState.cuePoints = std::move(snappedCues);
    persistentState.selectedCueIndex = -1;
    if (selectedCueId >= 0)
    {
        for (size_t i = 0; i < persistentState.cuePoints.size(); ++i)
        {
            if (persistentState.cuePoints[i].id == selectedCueId)
            {
                persistentState.selectedCueIndex = static_cast<int>(i);
                break;
            }
        }
    }
    persistentState.selectedCueIndex = clampCueSelection(persistentState.selectedCueIndex,
                                                         static_cast<int>(persistentState.cuePoints.size()));
    persistentState.storedSlices.clear();
    rebuildSlicesLocked();
    return true;
}

int SampleModeEngine::createTransientMarkerAtNormalizedPosition(float normalizedPosition)
{
    int markerIndex = -1;
    {
        const juce::ScopedLock lock(stateLock);
        if (loadedSample == nullptr || loadedSample->sourceLengthSamples <= 1)
            return -1;

        materializeTransientMarkersLocked();
        const int64_t markerSample = static_cast<int64_t>(
            std::llround(juce::jlimit(0.0f, 1.0f, normalizedPosition)
                         * static_cast<float>(loadedSample->sourceLengthSamples - 1)));
        if (markerSample <= 0 || markerSample >= loadedSample->sourceLengthSamples)
            return -1;

        const int64_t snappedSample = snapTransientMarkerSampleToGuideLocked(markerSample,
                                                                             1,
                                                                             loadedSample->sourceLengthSamples - 1,
                                                                             false);
        pushSliceEditUndoStateLocked();
        persistentState.transientEditSamples.push_back(snappedSample);
        persistentState.transientMarkersEdited = true;
        invalidateTransientMarkerCachesLocked();
        quantizeTransientEditSamplesToGuidesLocked();
        std::sort(persistentState.transientEditSamples.begin(), persistentState.transientEditSamples.end());
        persistentState.transientEditSamples.erase(std::unique(persistentState.transientEditSamples.begin(),
                                                               persistentState.transientEditSamples.end()),
                                                   persistentState.transientEditSamples.end());
        auto it = std::find(persistentState.transientEditSamples.begin(),
                            persistentState.transientEditSamples.end(),
                            snappedSample);
        if (it != persistentState.transientEditSamples.end())
            markerIndex = static_cast<int>(std::distance(persistentState.transientEditSamples.begin(), it));
        persistentState.storedSlices.clear();
        if (persistentState.sliceMode == SampleSliceMode::Transient)
            rebuildSlicesLocked();
    }

    notifyLegacyLoopRenderStateChanged();
    sendChangeMessage();
    return markerIndex;
}

int SampleModeEngine::moveTransientMarker(int markerIndex, float normalizedPosition)
{
    int newIndex = -1;
    {
        const juce::ScopedLock lock(stateLock);
        materializeTransientMarkersLocked();
        if (loadedSample == nullptr
            || markerIndex < 0
            || markerIndex >= static_cast<int>(persistentState.transientEditSamples.size()))
        {
            return -1;
        }

        const int64_t markerSample = juce::jlimit<int64_t>(
            1,
            juce::jmax<int64_t>(1, loadedSample->sourceLengthSamples - 1),
            static_cast<int64_t>(std::llround(juce::jlimit(0.0f, 1.0f, normalizedPosition)
                                              * static_cast<float>(loadedSample->sourceLengthSamples - 1))));
        const int64_t lowerBound = (markerIndex > 0)
            ? (persistentState.transientEditSamples[static_cast<size_t>(markerIndex - 1)] + 1)
            : 1;
        const int64_t upperBound = (markerIndex + 1 < static_cast<int>(persistentState.transientEditSamples.size()))
            ? (persistentState.transientEditSamples[static_cast<size_t>(markerIndex + 1)] - 1)
            : juce::jmax<int64_t>(1, loadedSample->sourceLengthSamples - 1);
        if (upperBound < lowerBound)
            return -1;

        const int64_t snappedSample = snapTransientMarkerSampleToGuideLocked(markerSample,
                                                                             lowerBound,
                                                                             upperBound,
                                                                             false);
        if (persistentState.transientEditSamples[static_cast<size_t>(markerIndex)] == snappedSample)
            return markerIndex;

        pushSliceEditUndoStateLocked();
        persistentState.transientEditSamples[static_cast<size_t>(markerIndex)] = snappedSample;
        persistentState.transientMarkersEdited = true;
        invalidateTransientMarkerCachesLocked();
        quantizeTransientEditSamplesToGuidesLocked();
        persistentState.transientEditSamples.erase(
            std::remove_if(persistentState.transientEditSamples.begin(),
                           persistentState.transientEditSamples.end(),
                           [this](int64_t sample)
                           {
                               return sample <= 0 || sample >= loadedSample->sourceLengthSamples;
                           }),
            persistentState.transientEditSamples.end());
        std::sort(persistentState.transientEditSamples.begin(), persistentState.transientEditSamples.end());
        persistentState.transientEditSamples.erase(std::unique(persistentState.transientEditSamples.begin(),
                                                               persistentState.transientEditSamples.end()),
                                                   persistentState.transientEditSamples.end());
        auto it = std::lower_bound(persistentState.transientEditSamples.begin(),
                                   persistentState.transientEditSamples.end(),
                                   snappedSample);
        if (it != persistentState.transientEditSamples.end())
            newIndex = static_cast<int>(std::distance(persistentState.transientEditSamples.begin(), it));
        if (newIndex < 0 && !persistentState.transientEditSamples.empty())
            newIndex = juce::jlimit(0, static_cast<int>(persistentState.transientEditSamples.size()) - 1, markerIndex);
        persistentState.storedSlices.clear();
        if (persistentState.sliceMode == SampleSliceMode::Transient)
            rebuildSlicesLocked();
    }

    notifyLegacyLoopRenderStateChanged();
    sendChangeMessage();
    return newIndex;
}

int SampleModeEngine::moveTransientMarkerToNearestDetectedTransient(int markerIndex, float normalizedPosition)
{
    int newIndex = -1;
    {
        const juce::ScopedLock lock(stateLock);
        materializeTransientMarkersLocked();
        if (loadedSample == nullptr
            || markerIndex < 0
            || markerIndex >= static_cast<int>(persistentState.transientEditSamples.size()))
        {
            return -1;
        }

        const int64_t totalSamples = juce::jmax<int64_t>(1, loadedSample->sourceLengthSamples);
        const int64_t targetSample = juce::jlimit<int64_t>(
            1,
            totalSamples - 1,
            static_cast<int64_t>(std::llround(juce::jlimit(0.0f, 1.0f, normalizedPosition)
                                              * static_cast<float>(totalSamples - 1))));

        const int64_t lowerBound = (markerIndex > 0)
            ? (persistentState.transientEditSamples[static_cast<size_t>(markerIndex - 1)] + 1)
            : 1;
        const int64_t upperBound = (markerIndex + 1 < static_cast<int>(persistentState.transientEditSamples.size()))
            ? (persistentState.transientEditSamples[static_cast<size_t>(markerIndex + 1)] - 1)
            : (totalSamples - 1);
        if (upperBound <= lowerBound)
            return -1;

        const int64_t snappedSample = snapTransientMarkerSampleToGuideLocked(targetSample,
                                                                             lowerBound,
                                                                             upperBound,
                                                                             true);

        if (persistentState.transientEditSamples[static_cast<size_t>(markerIndex)] == snappedSample)
            return markerIndex;

        pushSliceEditUndoStateLocked();
        persistentState.transientEditSamples[static_cast<size_t>(markerIndex)] = snappedSample;
        persistentState.transientMarkersEdited = true;
        invalidateTransientMarkerCachesLocked();
        quantizeTransientEditSamplesToGuidesLocked();
        std::sort(persistentState.transientEditSamples.begin(), persistentState.transientEditSamples.end());
        persistentState.transientEditSamples.erase(std::unique(persistentState.transientEditSamples.begin(),
                                                               persistentState.transientEditSamples.end()),
                                                   persistentState.transientEditSamples.end());

        auto it = std::lower_bound(persistentState.transientEditSamples.begin(),
                                   persistentState.transientEditSamples.end(),
                                   snappedSample);
        if (it != persistentState.transientEditSamples.end())
            newIndex = static_cast<int>(std::distance(persistentState.transientEditSamples.begin(), it));
        if (newIndex < 0 && !persistentState.transientEditSamples.empty())
            newIndex = juce::jlimit(0, static_cast<int>(persistentState.transientEditSamples.size()) - 1, markerIndex);

        persistentState.storedSlices.clear();
        if (persistentState.sliceMode == SampleSliceMode::Transient)
            rebuildSlicesLocked();
    }

    notifyLegacyLoopRenderStateChanged();
    sendChangeMessage();
    return newIndex;
}

int SampleModeEngine::findTransientMarkerIndexForVisibleSlotLocked(int visibleSlot)
{
    materializeTransientMarkersLocked();
    if (loadedSample == nullptr
        || visibleSlot < 0
        || visibleSlot >= SliceModel::VisibleSliceCount
        )
    {
        return -1;
    }

    const auto visibleSlices = getCurrentVisibleSlicesLocked();
    const auto& slice = visibleSlices[static_cast<size_t>(visibleSlot)];
    if (slice.id < 0 || slice.endSample <= slice.startSample)
        return -1;

    const int64_t targetMarkerSample = juce::jlimit<int64_t>(1,
                                                             loadedSample->sourceLengthSamples - 1,
                                                             slice.startSample);
    auto findExactMarkerIndex = [&](const std::vector<int64_t>& markerSamples) -> int
    {
        auto it = std::lower_bound(markerSamples.begin(), markerSamples.end(), targetMarkerSample);
        if (it != markerSamples.end() && *it == targetMarkerSample)
            return static_cast<int>(std::distance(markerSamples.begin(), it));

        if (it != markerSamples.begin())
        {
            const auto previousIt = std::prev(it);
            if (*previousIt == targetMarkerSample)
                return static_cast<int>(std::distance(markerSamples.begin(), previousIt));
        }

        return -1;
    };

    if (const int markerIndex = findExactMarkerIndex(persistentState.transientEditSamples); markerIndex >= 0)
        return markerIndex;

    // A stale edited marker set can survive reloads and no longer match the visible slices.
    // If the current auto-derived markers do match the visible slice, drop the stale override.
    if (persistentState.transientMarkersEdited)
    {
        const auto autoMarkers = buildCanonicalTransientMarkerSamples(*loadedSample, {}, false);
        if (findExactMarkerIndex(autoMarkers) >= 0)
        {
            persistentState.transientMarkersEdited = false;
            persistentState.transientEditSamples.clear();
            invalidateTransientMarkerCachesLocked();
            persistentState.storedSlices.clear();
            rebuildSlicesLocked();
            materializeTransientMarkersLocked();
            return findExactMarkerIndex(persistentState.transientEditSamples);
        }
    }

    return -1;
}

int SampleModeEngine::resolveTransientMarkerIndexForVisibleSlot(int visibleSlot)
{
    const juce::ScopedLock lock(stateLock);
    if (loadedSample == nullptr || persistentState.sliceMode != SampleSliceMode::Transient)
        return -1;

    return findTransientMarkerIndexForVisibleSlotLocked(visibleSlot);
}

bool SampleModeEngine::moveVisibleSliceToPosition(int visibleSlot, float normalizedPosition)
{
    bool moved = false;
    {
        const juce::ScopedLock lock(stateLock);
        if (loadedSample == nullptr || persistentState.sliceMode != SampleSliceMode::Transient)
            return false;

        const int markerIndex = findTransientMarkerIndexForVisibleSlotLocked(visibleSlot);
        if (markerIndex < 0
            || markerIndex >= static_cast<int>(persistentState.transientEditSamples.size()))
        {
            return false;
        }

        const int64_t targetSample = juce::jlimit<int64_t>(
            1,
            juce::jmax<int64_t>(1, loadedSample->sourceLengthSamples - 1),
            static_cast<int64_t>(std::llround(juce::jlimit(0.0f, 1.0f, normalizedPosition)
                                              * static_cast<float>(loadedSample->sourceLengthSamples - 1))));
        const int64_t lowerBound = (markerIndex > 0)
            ? (persistentState.transientEditSamples[static_cast<size_t>(markerIndex - 1)] + 1)
            : 1;
        const int64_t upperBound = (markerIndex + 1 < static_cast<int>(persistentState.transientEditSamples.size()))
            ? (persistentState.transientEditSamples[static_cast<size_t>(markerIndex + 1)] - 1)
            : juce::jmax<int64_t>(1, loadedSample->sourceLengthSamples - 1);
        if (upperBound < lowerBound)
            return false;

        const int64_t clampedSample = juce::jlimit(lowerBound, upperBound, targetSample);
        if (persistentState.transientEditSamples[static_cast<size_t>(markerIndex)] == clampedSample)
            return false;

        pushSliceEditUndoStateLocked();
        persistentState.transientEditSamples[static_cast<size_t>(markerIndex)] = clampedSample;
        persistentState.transientMarkersEdited = true;
        invalidateTransientMarkerCachesLocked();
        std::sort(persistentState.transientEditSamples.begin(), persistentState.transientEditSamples.end());
        persistentState.transientEditSamples.erase(std::unique(persistentState.transientEditSamples.begin(),
                                                               persistentState.transientEditSamples.end()),
                                                   persistentState.transientEditSamples.end());
        persistentState.storedSlices.clear();
        rebuildSlicesLocked();
        moved = true;
    }

    if (!moved)
        return false;

    notifyLegacyLoopRenderStateChanged();
    sendChangeMessage();
    return true;
}

bool SampleModeEngine::moveVisibleSliceToNearestTransient(int visibleSlot, float normalizedPosition)
{
    int markerIndex = -1;
    {
        const juce::ScopedLock lock(stateLock);
        if (loadedSample == nullptr || persistentState.sliceMode != SampleSliceMode::Transient)
            return false;

        markerIndex = findTransientMarkerIndexForVisibleSlotLocked(visibleSlot);
    }

    if (markerIndex < 0)
        return false;

    return moveTransientMarkerToNearestDetectedTransient(markerIndex, normalizedPosition) >= 0;
}

std::vector<int64_t> SampleModeEngine::buildCanonicalTransientMarkerSamplesLocked() const
{
    if (loadedSample == nullptr)
        return {};

    if (!canonicalTransientMarkerCacheValid)
    {
        canonicalTransientMarkerCache = buildCanonicalTransientMarkerSamples(*loadedSample,
                                                                             persistentState.transientEditSamples,
                                                                             persistentState.transientMarkersEdited);
        canonicalTransientMarkerCacheValid = true;
    }

    return canonicalTransientMarkerCache;
}

std::vector<int64_t> SampleModeEngine::buildCanonicalTransientMarkerSamplesForRangeLocked(int64_t rangeStartSample,
                                                                                           int64_t rangeEndSample) const
{
    return filterMarkerSamplesToRange(buildCanonicalTransientMarkerSamplesLocked(),
                                      rangeStartSample,
                                      rangeEndSample);
}

std::vector<int64_t> SampleModeEngine::buildLegacyLoopWindowTransientMarkerSamplesLocked(int64_t rangeStartSample,
                                                                                         int64_t rangeEndSample) const
{
    if (loadedSample == nullptr || rangeEndSample <= rangeStartSample)
        return {};

    if (legacyLoopTransientWindowCache.valid
        && legacyLoopTransientWindowCache.rangeStartSample == rangeStartSample
        && legacyLoopTransientWindowCache.rangeEndSample == rangeEndSample)
    {
        return legacyLoopTransientWindowCache.markerSamples;
    }

    auto localTransientCandidates = buildCanonicalTransientMarkerSamplesForRangeLocked(rangeStartSample,
                                                                                       rangeEndSample);

    if (localTransientCandidates.empty())
    {
        localTransientCandidates = buildLegacyLoopWindowTransientCandidatesLocked(rangeStartSample,
                                                                                  rangeEndSample);
    }

    legacyLoopTransientWindowCache.valid = true;
    legacyLoopTransientWindowCache.rangeStartSample = rangeStartSample;
    legacyLoopTransientWindowCache.rangeEndSample = rangeEndSample;
    legacyLoopTransientWindowCache.markerSamples = localTransientCandidates;
    legacyLoopTransientWindowCache.visibleSlicesValid = false;
    legacyLoopTransientWindowCache.visibleSlicesPageStart = -1;

    return localTransientCandidates;
}

std::vector<int64_t> SampleModeEngine::buildTransientGuideCandidatesLocked(int64_t lowerBound,
                                                                           int64_t upperBound,
                                                                           bool preferLocalTransients) const
{
    std::vector<int64_t> candidates;
    if (loadedSample == nullptr || upperBound < lowerBound)
        return candidates;

    candidates = buildCanonicalTransientMarkerSamplesForRangeLocked(lowerBound, upperBound + 1);

    if (candidates.empty() && preferLocalTransients)
    {
        int64_t loopRangeStartSample = 0;
        int64_t loopRangeEndSample = 0;
        if (computeLegacyLoopWindowRangeLocked(loopRangeStartSample, loopRangeEndSample)
            && upperBound >= loopRangeStartSample
            && lowerBound < loopRangeEndSample)
        {
            candidates = buildLegacyLoopWindowTransientCandidatesLocked(loopRangeStartSample,
                                                                        loopRangeEndSample);
            candidates.erase(std::remove_if(candidates.begin(),
                                            candidates.end(),
                                            [lowerBound, upperBound](int64_t sample)
                                            {
                                                return sample < lowerBound || sample > upperBound;
                                            }),
                             candidates.end());
        }
    }

    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
    return candidates;
}

int64_t SampleModeEngine::snapTransientMarkerSampleToGuideLocked(int64_t targetSample,
                                                                 int64_t lowerBound,
                                                                 int64_t upperBound,
                                                                 bool preferLocalTransients) const
{
    if (loadedSample == nullptr || upperBound < lowerBound)
        return targetSample;

    const int64_t clampedTarget = juce::jlimit(lowerBound, upperBound, targetSample);
    const auto candidates = buildTransientGuideCandidatesLocked(lowerBound, upperBound, preferLocalTransients);
    if (candidates.empty())
        return clampedTarget;

    int64_t snappedSample = clampedTarget;
    int64_t bestDistance = std::numeric_limits<int64_t>::max();
    for (const auto candidate : candidates)
    {
        const int64_t distance = std::abs(candidate - clampedTarget);
        if (distance < bestDistance)
        {
            bestDistance = distance;
            snappedSample = candidate;
        }
    }

    return snappedSample;
}

void SampleModeEngine::quantizeTransientEditSamplesToGuidesLocked()
{
    if (loadedSample == nullptr || persistentState.transientEditSamples.empty())
        return;

    auto quantized = WarpGrid::sanitizeBeatTicks(persistentState.transientEditSamples,
                                                 loadedSample->sourceLengthSamples);
    if (quantized.empty())
    {
        persistentState.transientEditSamples.clear();
        persistentState.transientMarkersEdited = false;
        invalidateTransientMarkerCachesLocked();
        return;
    }

    for (size_t i = 0; i < quantized.size(); ++i)
    {
        const int64_t lowerBound = (i > 0)
            ? (quantized[i - 1] + 1)
            : 1;
        const int64_t upperBound = (i + 1 < quantized.size())
            ? (quantized[i + 1] - 1)
            : juce::jmax<int64_t>(1, loadedSample->sourceLengthSamples - 1);
        if (upperBound < lowerBound)
            continue;

        quantized[i] = snapTransientMarkerSampleToGuideLocked(quantized[i],
                                                              lowerBound,
                                                              upperBound,
                                                              false);
    }

    quantized = WarpGrid::sanitizeBeatTicks(quantized, loadedSample->sourceLengthSamples);
    persistentState.transientEditSamples = std::move(quantized);
    persistentState.transientMarkersEdited = !persistentState.transientEditSamples.empty();
    invalidateTransientMarkerCachesLocked();
}

std::vector<int64_t> SampleModeEngine::buildLegacyLoopWindowTransientCandidatesLocked(int64_t rangeStartSample,
                                                                                       int64_t rangeEndSample) const
{
    std::vector<int64_t> candidates;
    if (loadedSample == nullptr || rangeEndSample <= rangeStartSample || loadedSample->sourceLengthSamples <= 1)
        return candidates;

    const int windowLength = static_cast<int>(juce::jmax<int64_t>(1, rangeEndSample - rangeStartSample));
    juce::AudioBuffer<float> localWindow(1, windowLength);
    localWindow.clear();

    const int sourceChannels = juce::jmax(1, loadedSample->audioBuffer.getNumChannels());
    for (int sampleIndex = 0; sampleIndex < windowLength; ++sampleIndex)
    {
        const int sourceIndex = static_cast<int>(rangeStartSample) + sampleIndex;
        float mono = 0.0f;
        for (int ch = 0; ch < sourceChannels; ++ch)
            mono += loadedSample->audioBuffer.getSample(ch, sourceIndex);
        localWindow.setSample(0, sampleIndex, mono / static_cast<float>(sourceChannels));
    }

    int fftOrder = 8;
    while ((1 << fftOrder) < juce::jmin(2048, windowLength) && fftOrder < 12)
        ++fftOrder;
    const int frameSize = 1 << fftOrder;
    const int hopSize = juce::jmax(32, frameSize / 8);

    auto refineRangeSamplesToLocalLeadingEdges = [&](const std::vector<int64_t>& sourceSamples)
    {
        std::vector<int64_t> localSamples;
        localSamples.reserve(sourceSamples.size());
        for (const auto sample : sourceSamples)
        {
            if (sample < rangeStartSample || sample >= rangeEndSample)
                continue;
            localSamples.push_back(sample - rangeStartSample);
        }

        if (localSamples.empty())
            return localSamples;

        auto refined = refineOnsetSamplesToLeadingEdges(localWindow, localSamples, frameSize, hopSize);
        if (refined.empty())
            refined = std::move(localSamples);
        return refined;
    };

    const auto canonicalTransientMarkers = buildCanonicalTransientMarkerSamplesLocked();
    const auto refinedTransientMarkers = refineRangeSamplesToLocalLeadingEdges(canonicalTransientMarkers);
    const auto detectedTransientSamples = detectLoopStyleTransientSamples(localWindow,
                                                                          loadedSample->sourceSampleRate);

    candidates.reserve(refinedTransientMarkers.size() + detectedTransientSamples.size());
    for (const auto sample : refinedTransientMarkers)
        candidates.push_back(rangeStartSample + sample);
    for (const auto sample : detectedTransientSamples)
        candidates.push_back(rangeStartSample + sample);

    candidates.erase(std::remove_if(candidates.begin(),
                                    candidates.end(),
                                    [rangeStartSample, rangeEndSample](int64_t sample)
                                    {
                                        return sample < rangeStartSample || sample >= rangeEndSample;
                                    }),
                     candidates.end());
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());

    if (candidates.empty())
        return candidates;

    const int64_t clusterTolerance = static_cast<int64_t>(juce::jmax(1.0,
        std::round(loadedSample->sourceSampleRate * 0.006)));
    std::vector<int64_t> clustered;
    clustered.reserve(candidates.size());
    size_t clusterStart = 0;
    while (clusterStart < candidates.size())
    {
        size_t clusterEnd = clusterStart + 1;
        int64_t chosen = candidates[clusterStart];
        while (clusterEnd < candidates.size()
               && (candidates[clusterEnd] - candidates[clusterEnd - 1]) <= clusterTolerance)
        {
            chosen = juce::jmin(chosen, candidates[clusterEnd]);
            ++clusterEnd;
        }

        clustered.push_back(chosen);
        clusterStart = clusterEnd;
    }

    return clustered;
}

std::vector<int64_t> SampleModeEngine::buildTransientSnapCandidatesLocked(int64_t targetSample,
                                                                          int64_t lowerBound,
                                                                          int64_t upperBound) const
{
    juce::ignoreUnused(targetSample);
    return buildTransientGuideCandidatesLocked(lowerBound, upperBound, true);
}

bool SampleModeEngine::deleteTransientMarker(int markerIndex)
{
    {
        const juce::ScopedLock lock(stateLock);
        materializeTransientMarkersLocked();
        if (markerIndex < 0 || markerIndex >= static_cast<int>(persistentState.transientEditSamples.size()))
            return false;

        pushSliceEditUndoStateLocked();
        persistentState.transientEditSamples.erase(persistentState.transientEditSamples.begin() + markerIndex);
        persistentState.transientMarkersEdited = true;
        invalidateTransientMarkerCachesLocked();
        persistentState.storedSlices.clear();
        if (persistentState.sliceMode == SampleSliceMode::Transient)
            rebuildSlicesLocked();
    }

    notifyLegacyLoopRenderStateChanged();
    sendChangeMessage();
    return true;
}

bool SampleModeEngine::snapSlicePointsToNearestPpqGrid(double gridStepBeats, bool currentSelectionOnly)
{
    bool changed = false;
    {
        const juce::ScopedLock lock(stateLock);
        if (loadedSample == nullptr || !(gridStepBeats > 0.0) || !std::isfinite(gridStepBeats))
            return false;

        const auto undoDepthBefore = sliceEditUndoStack.size();
        pushSliceEditUndoStateLocked();
        bool snapped = false;
        switch (persistentState.sliceMode)
        {
            case SampleSliceMode::Uniform:
            case SampleSliceMode::Beat:
                return false;
            case SampleSliceMode::Transient:
                snapped = snapTransientMarkersToPpqGridLocked(gridStepBeats, currentSelectionOnly);
                break;
            case SampleSliceMode::Manual:
                snapped = snapCuePointsToPpqGridLocked(gridStepBeats, currentSelectionOnly);
                break;
        }

        if (!snapped)
        {
            if (sliceEditUndoStack.size() > undoDepthBefore)
                sliceEditUndoStack.pop_back();
            return false;
        }

        changed = true;
    }

    if (!changed)
        return false;

    notifyLegacyLoopRenderStateChanged();
    sendChangeMessage();
    return true;
}

bool SampleModeEngine::resolveVisibleSlice(int visibleSlot, SampleSlice& sliceOut) const
{
    if (visibleSlot < 0 || visibleSlot >= SliceModel::VisibleSliceCount)
        return false;

    const juce::ScopedLock lock(stateLock);
    const auto visibleSlices = getCurrentVisibleSlicesLocked();
    const auto& slice = visibleSlices[static_cast<size_t>(visibleSlot)];
    if (slice.id < 0 || slice.endSample <= slice.startSample)
        return false;

    sliceOut = slice;
    return true;
}

int64_t SampleModeEngine::getDefaultLegacyLoopWindowStartSampleLocked() const
{
    const auto visibleSlices = sliceModel.getVisibleSlices();
    int64_t rangeStartSample = std::numeric_limits<int64_t>::max();
    for (const auto& slice : visibleSlices)
    {
        if (slice.id < 0 || slice.endSample <= slice.startSample)
            continue;
        rangeStartSample = juce::jmin(rangeStartSample, slice.startSample);
    }

    if (rangeStartSample == std::numeric_limits<int64_t>::max())
        return 0;
    return rangeStartSample;
}

bool SampleModeEngine::computeLegacyLoopWindowRangeLocked(int64_t& rangeStartSample, int64_t& rangeEndSample) const
{
    rangeStartSample = 0;
    rangeEndSample = 0;

    if (loadedSample == nullptr
        || !persistentState.useLegacyLoopEngine
        || persistentState.legacyLoopBarSelection <= 0
        || loadedSample->sourceSampleRate <= 0.0
        || !(persistentState.analyzedTempoBpm > 0.0))
    {
        return false;
    }

    const float beatsForLoop = legacyLoopBarSelectionToBeats(persistentState.legacyLoopBarSelection);
    if (!(beatsForLoop > 0.0f))
        return false;

    const int64_t sourceLength = juce::jmax<int64_t>(1, loadedSample->sourceLengthSamples);
    const int64_t defaultStart = getDefaultLegacyLoopWindowStartSampleLocked();
    const int64_t requestedStart = legacyLoopWindowStartSample >= 0 ? legacyLoopWindowStartSample : defaultStart;

    const int integerBeatCount = juce::roundToInt(beatsForLoop);
    const auto beatTicks = sanitizeBeatTickSamples(loadedSample->analysis.beatTickSamples, sourceLength);
    const bool usesWholeBeatCount = integerBeatCount > 0
        && std::abs(beatsForLoop - static_cast<float>(integerBeatCount)) < 1.0e-4f;
    if (usesWholeBeatCount && beatTicks.size() >= 2)
    {
        if (legacyLoopWindowManualAnchor)
        {
            rangeStartSample = juce::jlimit<int64_t>(0, sourceLength - 1, requestedStart);
            const double startBeat = computeBeatTickPosition(beatTicks, rangeStartSample);
            rangeEndSample = samplePositionFromBeatTickPosition(beatTicks,
                                                                startBeat + static_cast<double>(integerBeatCount),
                                                                sourceLength);
            rangeEndSample = juce::jlimit<int64_t>(rangeStartSample + 1, sourceLength, rangeEndSample);
            return rangeEndSample > rangeStartSample;
        }

        if (computeBeatTickAlignedWindowRange(beatTicks,
                                             sourceLength,
                                             requestedStart,
                                             integerBeatCount,
                                             rangeStartSample,
                                             rangeEndSample))
        {
            return true;
        }
    }

    const double samplesPerBeat = (60.0 / persistentState.analyzedTempoBpm) * loadedSample->sourceSampleRate;
    const int64_t targetLength = static_cast<int64_t>(std::llround(static_cast<double>(beatsForLoop) * samplesPerBeat));
    if (targetLength <= 0)
        return false;

    const int64_t maxWindowStart = juce::jmax<int64_t>(0, sourceLength - targetLength);
    rangeStartSample = legacyLoopWindowManualAnchor
        ? juce::jlimit<int64_t>(0, sourceLength - 1, requestedStart)
        : juce::jlimit<int64_t>(0, maxWindowStart, requestedStart);
    rangeEndSample = juce::jlimit<int64_t>(rangeStartSample + 1, sourceLength, rangeStartSample + targetLength);
    return rangeEndSample > rangeStartSample;
}

void SampleModeEngine::fitViewToLegacyLoopWindowLocked()
{
    int64_t rangeStartSample = 0;
    int64_t rangeEndSample = 0;
    if (loadedSample == nullptr
        || loadedSample->sourceLengthSamples <= 1
        || !computeLegacyLoopWindowRangeLocked(rangeStartSample, rangeEndSample))
    {
        return;
    }

    const float rangeStartNorm = safeNormalizedPosition(rangeStartSample, loadedSample->sourceLengthSamples);
    const float rangeEndNorm = safeNormalizedPosition(rangeEndSample, loadedSample->sourceLengthSamples);
    const float visibleLength = juce::jlimit(1.0f / kMaxViewZoom, 1.0f, juce::jmax(1.0e-4f, rangeEndNorm - rangeStartNorm));

    persistentState.viewZoom = juce::jlimit(kMinViewZoom, kMaxViewZoom, 1.0f / visibleLength);
    persistentState.viewScroll = juce::jlimit(0.0f,
                                              juce::jmax(0.0f, 1.0f - visibleLength),
                                              rangeStartNorm);
}

std::array<SampleSlice, SliceModel::VisibleSliceCount> SampleModeEngine::buildLegacyLoopWindowVisibleSlicesLocked(int64_t rangeStartSample,
                                                                                                                    int64_t rangeEndSample) const
{
    std::array<SampleSlice, SliceModel::VisibleSliceCount> visibleSlices {};
    if (loadedSample == nullptr || rangeEndSample <= rangeStartSample)
        return visibleSlices;

    if (persistentState.sliceMode == SampleSliceMode::Transient)
    {
        const auto localTransientCandidates = buildLegacyLoopWindowTransientMarkerSamplesLocked(rangeStartSample,
                                                                                                rangeEndSample);
        const int candidateCount = static_cast<int>(localTransientCandidates.size());
        const int pageStart = getLegacyLoopTransientPageStartIndex(legacyLoopTransientPageStartIndex, candidateCount);
        if (legacyLoopTransientWindowCache.valid
            && legacyLoopTransientWindowCache.rangeStartSample == rangeStartSample
            && legacyLoopTransientWindowCache.rangeEndSample == rangeEndSample
            && legacyLoopTransientWindowCache.visibleSlicesValid
            && legacyLoopTransientWindowCache.visibleSlicesPageStart == pageStart)
        {
            return legacyLoopTransientWindowCache.visibleSlices;
        }

        if (localTransientCandidates.empty())
        {
            const int64_t windowLength = juce::jmax<int64_t>(1, rangeEndSample - rangeStartSample);
            const auto localStarts = buildLoopStyleAnchorStarts(windowLength, {});
            for (int slot = 0; slot < SliceModel::VisibleSliceCount; ++slot)
            {
                SampleSlice slice;
                slice.id = slot;
                slice.startSample = rangeStartSample + localStarts[static_cast<size_t>(slot)];
                slice.endSample = (slot + 1 < SliceModel::VisibleSliceCount)
                    ? juce::jmax(slice.startSample + 1,
                                 rangeStartSample + localStarts[static_cast<size_t>(slot + 1)])
                    : rangeEndSample;
                slice.transientDerived = true;
                fillSliceMetadata(slice, slot, loadedSample->sourceLengthSamples);
                visibleSlices[static_cast<size_t>(slot)] = slice;
            }

            legacyLoopTransientWindowCache.valid = true;
            legacyLoopTransientWindowCache.rangeStartSample = rangeStartSample;
            legacyLoopTransientWindowCache.rangeEndSample = rangeEndSample;
            legacyLoopTransientWindowCache.markerSamples = localTransientCandidates;
            legacyLoopTransientWindowCache.visibleSlicesValid = true;
            legacyLoopTransientWindowCache.visibleSlicesPageStart = pageStart;
            legacyLoopTransientWindowCache.visibleSlices = visibleSlices;
            return visibleSlices;
        }

        const int visibleSliceCount = juce::jmin(SliceModel::VisibleSliceCount,
                                                 juce::jmax(0, candidateCount - pageStart));
        for (int slot = 0; slot < visibleSliceCount; ++slot)
        {
            const int candidateIndex = pageStart + slot;
            SampleSlice slice;
            slice.id = slot;
            slice.startSample = juce::jlimit<int64_t>(rangeStartSample,
                                                      rangeEndSample - 1,
                                                      localTransientCandidates[static_cast<size_t>(candidateIndex)]);
            slice.endSample = (candidateIndex + 1 < candidateCount)
                ? juce::jlimit<int64_t>(slice.startSample + 1,
                                        rangeEndSample,
                                        localTransientCandidates[static_cast<size_t>(candidateIndex + 1)])
                : rangeEndSample;
            slice.transientDerived = true;
            fillSliceMetadata(slice, slot, loadedSample->sourceLengthSamples);
            visibleSlices[static_cast<size_t>(slot)] = slice;
        }

        legacyLoopTransientWindowCache.valid = true;
        legacyLoopTransientWindowCache.rangeStartSample = rangeStartSample;
        legacyLoopTransientWindowCache.rangeEndSample = rangeEndSample;
        legacyLoopTransientWindowCache.markerSamples = localTransientCandidates;
        legacyLoopTransientWindowCache.visibleSlicesValid = true;
        legacyLoopTransientWindowCache.visibleSlicesPageStart = pageStart;
        legacyLoopTransientWindowCache.visibleSlices = visibleSlices;
        return visibleSlices;
    }

    std::vector<int64_t> candidates;
    if (persistentState.sliceMode == SampleSliceMode::Manual)
    {
        candidates.reserve(persistentState.cuePoints.size() + 1u);
        for (const auto& cue : persistentState.cuePoints)
            candidates.push_back(cue.samplePosition);
    }
    else
    {
        const auto& allSlices = sliceModel.getAllSlices();
        candidates.reserve(allSlices.size());
        for (const auto& slice : allSlices)
            candidates.push_back(slice.startSample);
    }

    candidates.erase(std::remove_if(candidates.begin(),
                                    candidates.end(),
                                    [rangeStartSample, rangeEndSample](int64_t sample)
                                    {
                                        return sample < rangeStartSample || sample >= rangeEndSample;
                                    }),
                     candidates.end());
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());

    const int64_t rangeLength = juce::jmax<int64_t>(1, rangeEndSample - rangeStartSample);
    const int64_t nominalStep = juce::jmax<int64_t>(1,
        static_cast<int64_t>(std::llround(static_cast<double>(rangeLength - 1)
                                          / static_cast<double>(juce::jmax(1, SliceModel::VisibleSliceCount - 1)))));
    const int64_t maxSnapDistance = juce::jmax<int64_t>(1, (nominalStep * 45) / 100);
    std::array<int64_t, SliceModel::VisibleSliceCount> starts {};
    int64_t previousStart = rangeStartSample - 1;
    for (int slot = 0; slot < SliceModel::VisibleSliceCount; ++slot)
    {
        int64_t chosen = rangeStartSample;
        int64_t target = rangeStartSample;
        int64_t bestDistance = std::numeric_limits<int64_t>::max();
        if (slot == 0)
        {
            chosen = rangeStartSample;
        }
        else
        {
            target = rangeStartSample
                + static_cast<int64_t>((static_cast<double>(slot) / static_cast<double>(SliceModel::VisibleSliceCount - 1))
                                       * static_cast<double>(rangeLength - 1));
            chosen = target;
            auto it = std::lower_bound(candidates.begin(), candidates.end(), target);
            if (it != candidates.end())
            {
                bestDistance = std::abs(*it - target);
                chosen = *it;
            }
            if (it != candidates.begin())
            {
                const int64_t prev = *std::prev(it);
                const int64_t distance = std::abs(prev - target);
                if (distance < bestDistance)
                {
                    bestDistance = distance;
                    chosen = prev;
                }
            }
        }

        if (slot > 0 && bestDistance > maxSnapDistance)
            chosen = target;

        chosen = juce::jlimit<int64_t>(rangeStartSample, rangeEndSample - 1, juce::jmax(previousStart + 1, chosen));
        starts[static_cast<size_t>(slot)] = chosen;
        previousStart = chosen;
    }

    for (int slot = 0; slot < SliceModel::VisibleSliceCount; ++slot)
    {
        SampleSlice slice;
        slice.id = slot;
        slice.startSample = starts[static_cast<size_t>(slot)];
        slice.endSample = (slot + 1 < SliceModel::VisibleSliceCount)
            ? juce::jmax(starts[static_cast<size_t>(slot)] + 1, starts[static_cast<size_t>(slot + 1)])
            : rangeEndSample;
        slice.transientDerived = (persistentState.sliceMode == SampleSliceMode::Transient);
        slice.manualDerived = (persistentState.sliceMode == SampleSliceMode::Manual);
        fillSliceMetadata(slice, slot, loadedSample->sourceLengthSamples);
        visibleSlices[static_cast<size_t>(slot)] = slice;
    }

    return visibleSlices;
}

std::array<SampleSlice, SliceModel::VisibleSliceCount> SampleModeEngine::getCurrentVisibleSlicesLocked() const
{
    int64_t rangeStartSample = 0;
    int64_t rangeEndSample = 0;
    if (computeLegacyLoopWindowRangeLocked(rangeStartSample, rangeEndSample))
        return buildLegacyLoopWindowVisibleSlicesLocked(rangeStartSample, rangeEndSample);

    if (randomVisibleSliceOverrideActive)
        return randomVisibleSlices;

    return sliceModel.getVisibleSlices();
}

std::vector<int64_t> SampleModeEngine::buildLegacyLoopAnchorCandidatesLocked() const
{
    std::vector<int64_t> candidates;
    if (loadedSample == nullptr)
        return candidates;

    const auto effectiveTransientMarkers = buildCanonicalTransientMarkerSamplesLocked();
    const bool preferWholeFileTransientAnchors = persistentState.legacyLoopBarSelection > 0
        && !effectiveTransientMarkers.empty();

    if (preferWholeFileTransientAnchors)
    {
        candidates = effectiveTransientMarkers;
    }
    else switch (persistentState.sliceMode)
    {
        case SampleSliceMode::Transient:
            candidates = effectiveTransientMarkers;
            break;
        case SampleSliceMode::Manual:
            candidates.reserve(persistentState.cuePoints.size());
            for (const auto& cue : persistentState.cuePoints)
                candidates.push_back(cue.samplePosition);
            break;
        case SampleSliceMode::Beat:
            candidates = sanitizeBeatTickSamples(loadedSample->analysis.beatTickSamples,
                                                 loadedSample->sourceLengthSamples);
            break;
        case SampleSliceMode::Uniform:
        default:
            candidates.reserve(sliceModel.getAllSlices().size());
            for (const auto& slice : sliceModel.getAllSlices())
                candidates.push_back(slice.startSample);
            break;
    }

    candidates.erase(std::remove_if(candidates.begin(),
                                    candidates.end(),
                                    [this](int64_t sample)
                                    {
                                        return sample < 0 || sample >= loadedSample->sourceLengthSamples;
                                    }),
                     candidates.end());
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());

    if (candidates.empty())
        candidates.push_back(0);

    return candidates;
}

void SampleModeEngine::clearRandomVisibleSliceOverrideLocked()
{
    randomVisibleSliceOverrideActive = false;
    randomVisibleSlices.fill(SampleSlice{});
}

bool SampleModeEngine::buildLegacyLoopSyncInfoForSliceIndexLocked(int sliceIndex, LegacyLoopSyncInfo& syncInfo) const
{
    if (loadedSample == nullptr)
        return false;

    const auto& allSlices = sliceModel.getAllSlices();
    if (sliceIndex < 0 || sliceIndex >= static_cast<int>(allSlices.size()))
        return false;

    const int bankIndex = sliceIndex / SliceModel::VisibleSliceCount;
    const int bankStart = bankIndex * SliceModel::VisibleSliceCount;

    syncInfo = {};
    syncInfo.loadedSample = loadedSample;
    syncInfo.visibleBankIndex = bankIndex;
    syncInfo.triggerVisibleSlot = sliceIndex - bankStart;
    syncInfo.analyzedTempoBpm = persistentState.analyzedTempoBpm > 0.0
        ? persistentState.analyzedTempoBpm
        : loadedSample->analysis.estimatedTempoBpm;
    syncInfo.legacyLoopBarSelection = persistentState.legacyLoopBarSelection;
    syncInfo.sliceMode = persistentState.sliceMode;
    syncInfo.warpMarkers = persistentState.warpMarkers;

    if (persistentState.sliceMode == SampleSliceMode::Transient)
    {
        for (int slot = 0; slot < SliceModel::VisibleSliceCount; ++slot)
        {
            const int sourceIndex = bankStart + slot;
            if (sourceIndex >= 0 && sourceIndex < static_cast<int>(allSlices.size()))
                syncInfo.visibleSlices[static_cast<size_t>(slot)] = allSlices[static_cast<size_t>(sourceIndex)];
        }
        syncInfo.markerSamples = buildCanonicalTransientMarkerSamplesLocked();
    }
    else if (persistentState.sliceMode == SampleSliceMode::Manual)
    {
        syncInfo.markerSamples.reserve(persistentState.cuePoints.size());
        for (const auto& cue : persistentState.cuePoints)
        {
            if (cue.samplePosition > 0 && cue.samplePosition < loadedSample->sourceLengthSamples)
                syncInfo.markerSamples.push_back(cue.samplePosition);
        }
    }
    else
    {
        syncInfo.markerSamples.reserve(static_cast<size_t>(juce::jmax(0, SliceModel::VisibleSliceCount)));
        for (int slot = 0; slot < SliceModel::VisibleSliceCount; ++slot)
        {
            const int sourceIndex = bankStart + slot;
            if (sourceIndex >= 0 && sourceIndex < static_cast<int>(allSlices.size()))
            {
                syncInfo.visibleSlices[static_cast<size_t>(slot)] = allSlices[static_cast<size_t>(sourceIndex)];
                const auto& slice = allSlices[static_cast<size_t>(sourceIndex)];
                if (slice.id >= 0 && slice.endSample > slice.startSample)
                    syncInfo.markerSamples.push_back(slice.startSample);
            }
        }
    }

    syncInfo.visibleBankBeats = computeVisibleSliceBeatSpan(*loadedSample,
                                                            syncInfo.visibleSlices,
                                                            syncInfo.analyzedTempoBpm,
                                                            persistentState.legacyLoopBarSelection);

    return true;
}

void SampleModeEngine::handleLoadResult(SampleFileManager::LoadResult result)
{
    LoadStatusCallback callbackToInvoke;

    {
        const juce::ScopedLock lock(stateLock);

        if (result.requestId != activeRequestId)
            return;

        isLoading = false;

        if (result.success && result.loadedSample != nullptr)
        {
            const auto restoredState = persistentState;
            resetSampleSpecificStateLocked();
            loadedSample = std::move(result.loadedSample);
            persistentState.samplePath = loadedSample->sourcePath;
            invalidateTransientMarkerCachesLocked();
            const bool restoreExistingEdits = restoredState.samplePath.isNotEmpty()
                && restoredState.samplePath == loadedSample->sourcePath;
            const bool preserveManualTempo = restoreExistingEdits
                && restoredState.analysisSource.startsWithIgnoreCase("manual tempo")
                && restoredState.analyzedTempoBpm > 0.0;
            persistentState.visibleSliceBankIndex = restoreExistingEdits ? restoredState.visibleSliceBankIndex : 0;
            persistentState.viewScroll = restoreExistingEdits ? restoredState.viewScroll : 0.0f;
            persistentState.selectedCueIndex = restoreExistingEdits ? restoredState.selectedCueIndex : -1;
            persistentState.analyzedTempoBpm = preserveManualTempo
                ? restoredState.analyzedTempoBpm
                : loadedSample->analysis.estimatedTempoBpm;
            persistentState.analyzedPitchHz = loadedSample->analysis.estimatedPitchHz;
            persistentState.analyzedPitchMidi = loadedSample->analysis.estimatedPitchMidi;
            persistentState.essentiaUsed = loadedSample->analysis.essentiaUsed;
            persistentState.analysisSource = preserveManualTempo
                ? restoredState.analysisSource
                : loadedSample->analysis.analysisSource;
            if (restoreExistingEdits)
            {
                const auto restoredTransientMarkers = WarpGrid::sanitizeBeatTicks(restoredState.transientEditSamples,
                                                                                  loadedSample->sourceLengthSamples);
                const auto preferredTransientMarkers = buildCanonicalTransientMarkerSamples(*loadedSample,
                                                                                            {},
                                                                                            false);
                const auto legacyTransientMarkers = WarpGrid::sanitizeBeatTicks(loadedSample->analysis.transientSamples,
                                                                                loadedSample->sourceLengthSamples);
                const bool restoredMatchesLegacyAutoMarkers = restoredState.transientMarkersEdited
                    && !restoredTransientMarkers.empty()
                    && !preferredTransientMarkers.empty()
                    && !legacyTransientMarkers.empty()
                    && restoredTransientMarkers == legacyTransientMarkers
                    && restoredTransientMarkers != preferredTransientMarkers;

                persistentState.cuePoints = restoredState.cuePoints;
                persistentState.warpMarkers = sanitizePersistentWarpMarkers(restoredState.warpMarkers,
                                                                           loadedSample->sourceLengthSamples);
                persistentState.transientMarkersEdited = restoredState.transientMarkersEdited
                    && !restoredTransientMarkers.empty()
                    && !restoredMatchesLegacyAutoMarkers;
                if (persistentState.transientMarkersEdited)
                {
                    persistentState.transientEditSamples = restoredTransientMarkers;
                    invalidateTransientMarkerCachesLocked();
                    quantizeTransientEditSamplesToGuidesLocked();
                }
            }
            rebuildSlicesLocked();
            [[maybe_unused]] const auto warmedCanonicalMarkers = buildCanonicalTransientMarkerSamplesLocked();
            sliceModel.setVisibleBankIndex(persistentState.visibleSliceBankIndex);
            pendingTriggerSliceValid = false;
            analysisProgress = 1.0f;
            statusText = "Loaded " + loadedSample->displayName;
            keyLockCaches.clear();
            keyLockCacheBuildInFlight = false;
            ++keyLockCacheGeneration;
        }
        else
        {
            loadedSample.reset();
            sliceModel.clear();
            invalidateTransientMarkerCachesLocked();
            pendingTriggerSliceValid = false;
            analysisProgress = 0.0f;
            statusText = result.errorMessage.isNotEmpty() ? result.errorMessage : "Sample load failed";
            keyLockCaches.clear();
            keyLockCacheBuildInFlight = false;
            ++keyLockCacheGeneration;
        }

        callbackToInvoke = loadStatusCallback;
    }

    stop(true);
    clearPendingVisibleSlice();

    if (callbackToInvoke)
        callbackToInvoke(result);

    notifyLegacyLoopRenderStateChanged();
    sendChangeMessage();
}

void SampleModeEngine::handleLoadProgress(int requestId, float progress, const juce::String& statusTextToUse)
{
    bool shouldNotify = false;
    {
        const juce::ScopedLock lock(stateLock);
        if (requestId != activeRequestId)
            return;

        analysisProgress = juce::jlimit(0.0f, 1.0f, progress);
        if (statusTextToUse.isNotEmpty())
            statusText = compactLoadStatusText(statusTextToUse);
        shouldNotify = true;
    }

    if (shouldNotify)
        sendChangeMessage();
}

void SampleModeEngine::handleKeyLockCacheReady(uint64_t generation,
                                               std::vector<std::shared_ptr<const StretchedSliceBuffer>> caches,
                                               float playbackRate,
                                               float pitchSemitones,
                                               int visibleBankIndex)
{
    const juce::ScopedLock lock(stateLock);
    if (generation != keyLockCacheGeneration)
        return;

    keyLockCaches = std::move(caches);
    keyLockCachePlaybackRate = playbackRate;
    keyLockCachePitchSemitones = pitchSemitones;
    keyLockCacheVisibleBankIndex = visibleBankIndex;
    keyLockCacheBuildInFlight = false;
}

std::shared_ptr<const StretchedSliceBuffer> SampleModeEngine::findKeyLockCacheForSlice(const SampleSlice& slice,
                                                                                        float playbackRate,
                                                                                        float pitchSemitones,
                                                                                        TimeStretchBackend backend) const
{
    const float quantizedRate = quantizeKeyLockRate(playbackRate);
    const float quantizedPitch = quantizeKeyLockPitch(pitchSemitones);
    for (const auto& cache : keyLockCaches)
    {
        if (cache != nullptr
            && matchesKeyLockRequest(*cache, slice, quantizedRate, quantizedPitch, backend))
            return cache;
    }

    return {};
}

bool SampleModeEngine::voiceHasMatchingKeyLockCache(const SamplePlaybackVoice& voice,
                                                    float playbackRate,
                                                    float pitchSemitones,
                                                    TimeStretchBackend backend) const
{
    if (const auto existing = voice.getStretchedBuffer())
        return matchesKeyLockRequest(*existing,
                                     voice.getActiveSlice(),
                                     playbackRate,
                                     pitchSemitones,
                                     backend);

    return findKeyLockCacheForSlice(voice.getActiveSlice(), playbackRate, pitchSemitones, backend) != nullptr;
}

void SampleModeEngine::rebuildSlicesLocked()
{
    clearRandomVisibleSliceOverrideLocked();
    if (loadedSample == nullptr)
    {
        sliceModel.clear();
        return;
    }

    sliceModel.setSlices(analysisEngine.buildSlicesForState(*loadedSample, persistentState));
    sliceModel.setVisibleBankIndex(persistentState.visibleSliceBankIndex);
    persistentState.storedSlices.clear();
    keyLockCaches.clear();
    keyLockCacheBuildInFlight = false;
    ++keyLockCacheGeneration;
}

int SampleModeEngine::clampCueSelection(int selectedCueIndex, int cueCount)
{
    if (cueCount <= 0)
        return -1;
    return juce::jlimit(0, cueCount - 1, selectedCueIndex);
}

SampleModeComponent::SampleModeComponent()
{
    setWantsKeyboardFocus(true);
    startTimerHz(30);
}

SampleModeComponent::~SampleModeComponent()
{
    stopTimer();
    setEngine(nullptr);
}

void SampleModeComponent::setEngine(SampleModeEngine* engineToUse)
{
    if (engine == engineToUse)
        return;

    if (engine != nullptr)
        engine->removeChangeListener(this);

    engine = engineToUse;

    if (engine != nullptr)
        engine->addChangeListener(this);

    refreshFromEngine();
}

void SampleModeComponent::setWaveformColour(juce::Colour colour)
{
    waveformColour = colour;
    repaint();
}

void SampleModeComponent::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds().reduced(4);
    g.fillAll(juce::Colour(0xff0d1014));

    auto header = bounds.removeFromTop(16);
    prev16Bounds = header.removeFromLeft(50);
    header.removeFromLeft(4);
    random16Bounds = header.removeFromLeft(50);
    header.removeFromLeft(4);
    next16Bounds = header.removeFromLeft(50);
    header.removeFromLeft(4);
    triggerModeArea = header.removeFromLeft(94);
    header.removeFromLeft(4);
    legacyLoopArea = header.removeFromLeft(42);
    header.removeFromLeft(4);
    legacyLoopBarsBounds = header.removeFromLeft(46);
    header.removeFromLeft(4);
    copyLoopBounds = header.removeFromLeft(56);
    header.removeFromLeft(4);
    copyGrainBounds = header.removeFromLeft(60);
    header.removeFromLeft(6);
    tempoHalfBounds = {};
    tempoDoubleBounds = {};
    if (stateSnapshot.estimatedTempoBpm > 0.0 && header.getWidth() >= 44)
    {
        tempoHalfBounds = header.removeFromLeft(20);
        header.removeFromLeft(4);
        tempoDoubleBounds = header.removeFromLeft(20);
        header.removeFromLeft(6);
    }

    auto drawHeaderButton = [&](juce::Rectangle<int> area, const juce::String& text, bool active)
    {
        g.setColour(active ? juce::Colour(0xff2a3340) : juce::Colour(0xff1a2028));
        g.fillRoundedRectangle(area.toFloat(), 4.0f);
        g.setColour(active ? juce::Colour(0xffd7dce2) : juce::Colour(0xff5a6674));
        g.drawRoundedRectangle(area.toFloat(), 4.0f, 1.0f);
        g.drawFittedText(text, area, juce::Justification::centred, 1);
    };

    const bool random16Enabled = stateSnapshot.visibleSliceBankCount > 1
        && !stateSnapshot.useLegacyLoopEngine;
    drawHeaderButton(prev16Bounds, "Prev 16", stateSnapshot.canNavigateLeft);
    drawHeaderButton(random16Bounds, "Rnd 16", random16Enabled);
    drawHeaderButton(next16Bounds, "Next 16", stateSnapshot.canNavigateRight);
    drawHeaderButton(getTriggerModeButtonBounds(SampleTriggerMode::OneShot), "SHOT", stateSnapshot.triggerMode == SampleTriggerMode::OneShot);
    drawHeaderButton(getTriggerModeButtonBounds(SampleTriggerMode::Loop), "LOOP", stateSnapshot.triggerMode == SampleTriggerMode::Loop);
    drawHeaderButton(getLegacyLoopButtonBounds(), "MLR", stateSnapshot.useLegacyLoopEngine);
    drawHeaderButton(getLegacyLoopBarsButtonBounds(),
                     legacyLoopBarSelectionLabel(stateSnapshot.legacyLoopBarSelection),
                     stateSnapshot.useLegacyLoopEngine || stateSnapshot.legacyLoopBarSelection != 0);
    drawHeaderButton(copyLoopBounds, "To Loop", stateSnapshot.hasSample);
    drawHeaderButton(copyGrainBounds, "To Grain", stateSnapshot.hasSample);
    if (!tempoHalfBounds.isEmpty())
        drawHeaderButton(tempoHalfBounds, "1/2", stateSnapshot.estimatedTempoBpm > 20.5);
    if (!tempoDoubleBounds.isEmpty())
        drawHeaderButton(tempoDoubleBounds, "2x", stateSnapshot.estimatedTempoBpm > 0.0 && stateSnapshot.estimatedTempoBpm < 399.5);
    const auto headerInfoBounds = header;
    if (!headerInfoBounds.isEmpty() && stateSnapshot.hasSample)
    {
        juce::StringArray infoParts;
        infoParts.add(stateSnapshot.displayName.isNotEmpty() ? stateSnapshot.displayName : juce::String("Untitled"));
        infoParts.add(stateSnapshot.estimatedTempoBpm > 0.0
            ? (juce::String(stateSnapshot.estimatedTempoBpm, 2) + " BPM")
            : juce::String("-- BPM"));
        infoParts.add(formatDetectedKeyLabel(stateSnapshot.estimatedPitchMidi, stateSnapshot.estimatedScaleIndex));

        g.setColour(juce::Colour(0xff1a2028));
        g.fillRoundedRectangle(headerInfoBounds.toFloat(), 4.0f);
        g.setColour(juce::Colour(0xff5a6674));
        g.drawRoundedRectangle(headerInfoBounds.toFloat(), 4.0f, 1.0f);
        g.setColour(juce::Colour(0xffd7dce2));
        g.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::plain)));
        g.drawFittedText(infoParts.joinIntoString("  |  "),
                         headerInfoBounds.reduced(6, 0),
                         juce::Justification::centredLeft,
                         1);
    }
    auto progressArea = bounds.removeFromTop(stateSnapshot.isLoading ? 8 : 2);
    if (stateSnapshot.isLoading)
    {
        auto bar = progressArea.reduced(0, 1);
        bar = bar.withWidth(juce::jmax(36, getWidth() - 8));
        g.setColour(juce::Colour(0xff1a2028));
        g.fillRoundedRectangle(bar.toFloat(), 3.0f);
        g.setColour(juce::Colour(0xff384657));
        g.drawRoundedRectangle(bar.toFloat(), 3.0f, 1.0f);

        auto fill = bar;
        fill.setWidth(static_cast<int>(std::round(static_cast<float>(bar.getWidth())
            * juce::jlimit(0.0f, 1.0f, stateSnapshot.analysisProgress))));
        g.setColour(waveformColour.withAlpha(0.95f));
        g.fillRoundedRectangle(fill.toFloat(), 3.0f);
    }

    waveformBounds = bounds.reduced(0, 1);
    g.setColour(juce::Colour(0xff171d25));
    g.fillRoundedRectangle(waveformBounds.toFloat(), 8.0f);
    g.setColour(juce::Colour(0xff263241));
    g.drawRoundedRectangle(waveformBounds.toFloat(), 8.0f, 1.0f);

    if (loadedSample != nullptr && !loadedSample->previewMin.empty() && !loadedSample->previewMax.empty())
    {
        const auto visibleRange = getVisibleDisplayRange();
        const auto stableVisibleRange = getVisibleRange();
        const auto warpDisplayMap = buildSampleWarpDisplayMap(loadedSample.get(), stateSnapshot);
        const auto warpLane = getWarpMarkerLaneBounds();
        const auto sliceHandleLane = getSliceMarkerHandleBounds();
        const int contentTop = sliceHandleLane.isEmpty()
            ? (waveformBounds.getY() + 4)
            : (sliceHandleLane.getBottom() + 4);
        const auto centerY = waveformBounds.getCentreY();
        const auto halfHeight = waveformBounds.getHeight() * 0.42f;
        const auto pointCount = static_cast<int>(loadedSample->previewMin.size());
        const float visibleStart = visibleRange.getStart();
        const float visibleEnd = visibleRange.getEnd();
        const float visibleWidth = juce::jmax(1.0e-5f, visibleEnd - visibleStart);
        const float stableVisibleStart = stableVisibleRange.getStart();
        const float stableVisibleWidth = juce::jmax(1.0e-5f, stableVisibleRange.getLength());
        const int64_t stableRangeStartSample = juce::jlimit<int64_t>(
            0,
            juce::jmax<int64_t>(0, stateSnapshot.totalSamples - 1),
            static_cast<int64_t>(std::llround(stableVisibleStart
                                              * static_cast<float>(juce::jmax<int64_t>(1, stateSnapshot.totalSamples - 1)))));
        const int64_t stableRangeEndSample = juce::jlimit<int64_t>(
            stableRangeStartSample + 1,
            juce::jmax<int64_t>(1, stateSnapshot.totalSamples - 1),
            static_cast<int64_t>(std::llround(stableVisibleRange.getEnd()
                                              * static_cast<float>(juce::jmax<int64_t>(1, stateSnapshot.totalSamples - 1)))));
        const int64_t visibleSampleStart = samplePositionFromDisplayNormalized(warpDisplayMap, visibleStart);
        const int64_t visibleSampleEnd = samplePositionFromDisplayNormalized(warpDisplayMap, visibleEnd);
        const int64_t visibleSampleCount = juce::jmax<int64_t>(1, visibleSampleEnd - visibleSampleStart);
        const bool useDetailedWaveform = stateSnapshot.viewZoom >= kDetailedWaveformZoomThreshold
            && loadedSample->audioBuffer.getNumSamples() > 0
            && visibleSampleCount <= kDetailedWaveformMaxVisibleSamples;

        g.setColour(waveformColour.withAlpha(0.96f));
        if (useDetailedWaveform)
        {
            const auto& buffer = loadedSample->audioBuffer;
            const int channels = juce::jmax(1, buffer.getNumChannels());
            const int waveformWidth = juce::jmax(1, waveformBounds.getWidth());
            for (int pixel = 0; pixel < waveformWidth; ++pixel)
            {
                const float startNorm = visibleStart
                    + ((static_cast<float>(pixel) / static_cast<float>(waveformWidth)) * visibleWidth);
                const float endNorm = visibleStart
                    + ((static_cast<float>(pixel + 1) / static_cast<float>(waveformWidth)) * visibleWidth);
                const int64_t sampleStart = samplePositionFromDisplayNormalized(warpDisplayMap, startNorm);
                const int64_t sampleEnd = juce::jmax<int64_t>(
                    sampleStart + 1,
                    samplePositionFromDisplayNormalized(warpDisplayMap, endNorm));

                float minValue = 1.0f;
                float maxValue = -1.0f;
                for (int64_t sampleIndex = sampleStart; sampleIndex < sampleEnd; ++sampleIndex)
                {
                    const int clampedIndex = juce::jlimit(0, buffer.getNumSamples() - 1, static_cast<int>(sampleIndex));
                    float mono = 0.0f;
                    for (int ch = 0; ch < channels; ++ch)
                        mono += buffer.getSample(ch, clampedIndex);
                    mono /= static_cast<float>(channels);
                    minValue = juce::jmin(minValue, mono);
                    maxValue = juce::jmax(maxValue, mono);
                }

                if (minValue > maxValue)
                    std::swap(minValue, maxValue);

                const float minY = centerY - (minValue * halfHeight);
                const float maxY = centerY - (maxValue * halfHeight);
                g.drawVerticalLine(waveformBounds.getX() + pixel, juce::jmin(minY, maxY), juce::jmax(minY, maxY));
            }
        }
        else
        {
            for (int pointIndex = 0; pointIndex < pointCount; ++pointIndex)
            {
                const int64_t pointSample = static_cast<int64_t>(std::llround(
                    (static_cast<double>(pointIndex) / juce::jmax(1, pointCount - 1))
                    * static_cast<double>(juce::jmax<int64_t>(1, stateSnapshot.totalSamples - 1))));
                const float pointNorm = displayNormalizedPositionForSample(warpDisplayMap, pointSample);
                if (pointNorm < visibleStart || pointNorm > visibleEnd)
                    continue;
                const float visibleNorm = (pointNorm - visibleStart) / visibleWidth;
                const float x = waveformBounds.getX() + (waveformBounds.getWidth() - 1.0f) * visibleNorm;
                const float minY = centerY - (loadedSample->previewMin[static_cast<size_t>(pointIndex)] * halfHeight);
                const float maxY = centerY - (loadedSample->previewMax[static_cast<size_t>(pointIndex)] * halfHeight);
                g.drawVerticalLine(juce::roundToInt(x), juce::jmin(minY, maxY), juce::jmax(minY, maxY));
            }
        }

        g.setColour(juce::Colour(0x70ffffff));
        g.drawHorizontalLine(centerY, static_cast<float>(waveformBounds.getX()), static_cast<float>(waveformBounds.getRight()));

        if (const auto loopRange = computeSampleLoopVisualRange(stateSnapshot))
        {
            const int64_t loopStartSample = static_cast<int64_t>(std::llround(loopRange->getStart()
                                                                              * static_cast<float>(stateSnapshot.totalSamples)));
            const int64_t loopEndSample = static_cast<int64_t>(std::llround(loopRange->getEnd()
                                                                            * static_cast<float>(stateSnapshot.totalSamples)));
            const float loopStartNorm = juce::jmax(displayNormalizedPositionForSample(warpDisplayMap, loopStartSample), visibleStart);
            const float loopEndNorm = juce::jmin(displayNormalizedPositionForSample(warpDisplayMap, loopEndSample), visibleEnd);
            if (loopEndNorm > loopStartNorm)
            {
                const int loopX = waveformBounds.getX()
                    + juce::roundToInt(((loopStartNorm - visibleStart) / visibleWidth)
                                       * static_cast<float>(waveformBounds.getWidth()));
                const int loopEndX = waveformBounds.getX()
                    + juce::roundToInt(((loopEndNorm - visibleStart) / visibleWidth)
                                       * static_cast<float>(waveformBounds.getWidth()));
                auto loopRect = juce::Rectangle<int>(loopX,
                                                     waveformBounds.getY() + 2,
                                                     juce::jmax(2, loopEndX - loopX),
                                                     waveformBounds.getHeight() - 4);
                g.setColour(waveformColour.withAlpha(stateSnapshot.useLegacyLoopEngine ? 0.16f : 0.11f));
                g.fillRoundedRectangle(loopRect.toFloat(), 5.0f);
                g.setColour(waveformColour.brighter(0.4f).withAlpha(0.9f));
                g.drawRoundedRectangle(loopRect.toFloat(), 5.0f, 1.2f);
            }
        }

        const auto warpGuideTicks = sanitizeWarpBeatTicks(loadedSample->analysis.beatTickSamples,
                                                          loadedSample->sourceLengthSamples);
        if (!warpLane.isEmpty())
        {
            g.setColour(juce::Colour(0xff11161d).withAlpha(0.24f));
            g.fillRoundedRectangle(warpLane.toFloat(), 5.0f);
            g.setColour(juce::Colour(0xfff5c66a).withAlpha(0.2f));
            g.drawRoundedRectangle(warpLane.toFloat(), 5.0f, 1.0f);
            g.setColour(juce::Colour(0xfff5c66a).withAlpha(0.64f));
            g.setFont(juce::FontOptions(9.0f, juce::Font::plain));
            g.drawFittedText("WARP", warpLane.withWidth(34), juce::Justification::centred, 1);

            if (warpGuideTicks.size() >= 2)
            {
                const double stableBeatStart = computeBeatTickPosition(warpGuideTicks, stableRangeStartSample);
                const double stableBeatEnd = computeBeatTickPosition(warpGuideTicks, stableRangeEndSample);
                if (std::isfinite(stableBeatStart)
                    && std::isfinite(stableBeatEnd)
                    && stableBeatEnd > stableBeatStart)
                {
                    const double visibleBeatSpan = stableBeatEnd - stableBeatStart;
                    const double gridStep = choosePpqGridBeatStep(visibleBeatSpan,
                                                                  waveformBounds.getWidth(),
                                                                  28.0);
                    const double firstGridBeat = std::floor(stableBeatStart / gridStep) * gridStep;
                    const int gridTop = warpLane.getY() + 1;
                    const int gridBottom = waveformBounds.getBottom() - 2;

                    for (double beat = firstGridBeat; beat <= (stableBeatEnd + gridStep); beat += gridStep)
                    {
                        if (beat < (stableBeatStart - 1.0e-4)
                            || beat > (stableBeatEnd + 1.0e-4))
                        {
                            continue;
                        }

                        const int64_t sampleAtBeat = samplePositionFromBeatTickPosition(warpGuideTicks,
                                                                                         beat,
                                                                                         stateSnapshot.totalSamples);
                        const float stableGridNorm = safeNormalizedPosition(sampleAtBeat, stateSnapshot.totalSamples);
                        if (stableGridNorm < stableVisibleStart
                            || stableGridNorm > stableVisibleRange.getEnd())
                            continue;

                        const int x = waveformBounds.getX()
                            + juce::roundToInt(((stableGridNorm - stableVisibleStart) / stableVisibleWidth)
                                               * static_cast<float>(waveformBounds.getWidth()));
                        const bool isBarLine = beatGridMatchesDivision(beat, 4.0);
                        g.setColour(isBarLine
                                        ? juce::Colour(0xff79a6ff).withAlpha(0.22f)
                                        : juce::Colour(0xff79a6ff).withAlpha(0.1f));
                        g.drawVerticalLine(x,
                                           static_cast<float>(gridTop),
                                           static_cast<float>(gridBottom));
                    }
                }
            }

            g.setColour(juce::Colour(0xfff5c66a).withAlpha(0.78f));
            const auto drawAnchorHandle = [&](int x)
            {
                g.drawVerticalLine(x,
                                   static_cast<float>(warpLane.getY() + 1),
                                   static_cast<float>(waveformBounds.getBottom() - 2));

                juce::Path handle;
                const float anchorCenterY = static_cast<float>(warpLane.getCentreY());
                handle.startNewSubPath(static_cast<float>(x), anchorCenterY - 4.0f);
                handle.lineTo(static_cast<float>(x) - 4.0f, anchorCenterY);
                handle.lineTo(static_cast<float>(x), anchorCenterY + 4.0f);
                handle.lineTo(static_cast<float>(x) + 4.0f, anchorCenterY);
                handle.closeSubPath();
                g.fillPath(handle);
            };

            const bool showStartAnchor = visibleSampleStart <= 1;
            const bool showEndAnchor = visibleSampleEnd >= (stateSnapshot.totalSamples - 2);
            if (showStartAnchor)
                drawAnchorHandle(warpLane.getX() + 1);
            if (showEndAnchor)
                drawAnchorHandle(warpLane.getRight() - 2);
        }

        if (!warpLane.isEmpty() && warpGuideTicks.size() >= 2)
        {
            g.setColour(juce::Colour(0xfff5c66a).withAlpha(0.28f));
            for (const auto markerSample : warpGuideTicks)
            {
                const float markerNorm = displayNormalizedPositionForSample(warpDisplayMap, markerSample);
                if (markerNorm < visibleStart || markerNorm > visibleEnd)
                    continue;

                const int x = waveformBounds.getX()
                    + juce::roundToInt(((markerNorm - visibleStart) / visibleWidth)
                                       * static_cast<float>(waveformBounds.getWidth()));
                g.drawVerticalLine(x,
                                   static_cast<float>(warpLane.getY()),
                                   static_cast<float>(warpLane.getBottom() - 2));
            }
        }

        for (int sliceIndex = 0; sliceIndex < SliceModel::VisibleSliceCount; ++sliceIndex)
        {
            const auto& slice = stateSnapshot.visibleSlices[static_cast<size_t>(sliceIndex)];
            const int64_t markerSample = getVisibleSliceMarkerSample(sliceIndex);
            const float displaySliceStart = displayNormalizedPositionForSample(warpDisplayMap,
                                                                               markerSample >= 0 ? markerSample
                                                                                                 : slice.startSample);
            const float displaySliceEnd = displayNormalizedPositionForSample(warpDisplayMap, slice.endSample);
            if (slice.id < 0 || displaySliceEnd < visibleStart || displaySliceStart > visibleEnd)
                continue;

            const float clippedStart = juce::jmax(displaySliceStart, visibleStart);
            const float clippedEnd = juce::jmin(displaySliceEnd, visibleEnd);
            if (clippedEnd <= clippedStart)
                continue;

            const int sliceX = waveformBounds.getX()
                + juce::roundToInt(((clippedStart - visibleStart) / visibleWidth)
                                   * static_cast<float>(waveformBounds.getWidth()));
            const int sliceEndX = waveformBounds.getX()
                + juce::roundToInt(((clippedEnd - visibleStart) / visibleWidth)
                                   * static_cast<float>(waveformBounds.getWidth()));
            auto sliceRect = juce::Rectangle<int>(sliceX,
                                                  contentTop,
                                                  juce::jmax(1, sliceEndX - sliceX),
                                                  juce::jmax(10, waveformBounds.getBottom() - contentTop - 4));
            const bool isPending = (stateSnapshot.pendingVisibleSliceSlot == sliceIndex);
            const bool isActive = (stateSnapshot.activeVisibleSliceSlot == sliceIndex);
            g.setColour(waveformColour.withAlpha((sliceIndex % 2) == 0 ? 0.08f : 0.04f));
            g.fillRect(sliceRect);

            const int markerX = waveformBounds.getX()
                + juce::roundToInt(((displaySliceStart - visibleStart) / visibleWidth)
                                   * static_cast<float>(waveformBounds.getWidth()));
            g.setColour(waveformColour.darker(1.9f).withAlpha(0.92f));
            g.drawVerticalLine(markerX,
                               static_cast<float>(sliceHandleLane.getBottom() + 1),
                               static_cast<float>(waveformBounds.getBottom() - 6));
            g.setColour((isPending
                             ? waveformColour.brighter(1.15f)
                             : waveformColour.brighter(isActive ? 0.85f : 0.35f))
                            .withAlpha(isActive ? 0.99f : 0.96f));
            g.drawVerticalLine(markerX,
                               static_cast<float>(sliceHandleLane.getBottom() + 2),
                               static_cast<float>(waveformBounds.getBottom() - 7));

            if (stateSnapshot.sliceMode != SampleSliceMode::Manual)
            {
                juce::Path handle;
                handle.startNewSubPath(static_cast<float>(markerX), static_cast<float>(sliceHandleLane.getY() + 1));
                handle.lineTo(static_cast<float>(markerX - 4), static_cast<float>(sliceHandleLane.getBottom() - 1));
                handle.lineTo(static_cast<float>(markerX + 4), static_cast<float>(sliceHandleLane.getBottom() - 1));
                handle.closeSubPath();
                g.fillPath(handle);
            }
        }

        for (size_t cueIndex = 0; cueIndex < stateSnapshot.cuePoints.size(); ++cueIndex)
        {
            const auto& cue = stateSnapshot.cuePoints[cueIndex];
            const float cueNorm = displayNormalizedPositionForSample(warpDisplayMap, cue.samplePosition);
            if (cueNorm < visibleStart || cueNorm > visibleEnd)
                continue;

            const int x = waveformBounds.getX()
                + juce::roundToInt(((cueNorm - visibleStart) / visibleWidth) * static_cast<float>(waveformBounds.getWidth()));
            const bool selected = static_cast<int>(cueIndex) == stateSnapshot.selectedCueIndex;
            g.setColour(juce::Colour(0xff0f1318).withAlpha(0.92f));
            g.drawVerticalLine(x,
                               static_cast<float>(sliceHandleLane.getBottom() + 1),
                               static_cast<float>(waveformBounds.getBottom() - 2));
            g.setColour(selected ? juce::Colour(0xffffba6a) : juce::Colour(0xfff5fbff));
            g.drawVerticalLine(x,
                               static_cast<float>(sliceHandleLane.getBottom() + 2),
                               static_cast<float>(waveformBounds.getBottom() - 3));

            juce::Path handle;
            const float handleTop = static_cast<float>(sliceHandleLane.getY() + 1);
            const float handleBottom = static_cast<float>(sliceHandleLane.getBottom());
            const float handleHalfWidth = selected ? 5.0f : 4.0f;
            handle.startNewSubPath(static_cast<float>(x), handleTop);
            handle.lineTo(static_cast<float>(x) - handleHalfWidth, handleBottom);
            handle.lineTo(static_cast<float>(x) + handleHalfWidth, handleBottom);
            handle.closeSubPath();
            g.fillPath(handle);
        }

        if (!stateSnapshot.warpMarkers.empty())
        {
            const auto actualWarpLane = getWarpMarkerLaneBounds();
            for (size_t markerIndex = 0; markerIndex < stateSnapshot.warpMarkers.size(); ++markerIndex)
            {
                const auto& marker = stateSnapshot.warpMarkers[markerIndex];
                const float markerNorm = displayNormalizedPositionForSample(warpDisplayMap, marker.samplePosition);
                if (markerNorm < visibleStart || markerNorm > visibleEnd)
                    continue;

                const int x = waveformBounds.getX()
                    + juce::roundToInt(((markerNorm - visibleStart) / visibleWidth)
                                       * static_cast<float>(waveformBounds.getWidth()));
                const bool isDragging = static_cast<int>(markerIndex) == draggingWarpMarkerIndex;
                g.setColour(juce::Colour(0xfff5c66a).withAlpha(isDragging ? 0.98f : 0.9f));
                g.drawVerticalLine(x,
                                   static_cast<float>(actualWarpLane.getY() + 1),
                                   static_cast<float>(waveformBounds.getBottom() - 2));

                juce::Path handle;
                const float warpHandleCenterY = static_cast<float>(actualWarpLane.getCentreY());
                const float halfWidth = isDragging ? 6.0f : 5.0f;
                handle.startNewSubPath(static_cast<float>(x), warpHandleCenterY - 5.0f);
                handle.lineTo(static_cast<float>(x) - halfWidth, warpHandleCenterY);
                handle.lineTo(static_cast<float>(x), warpHandleCenterY + 5.0f);
                handle.lineTo(static_cast<float>(x) + halfWidth, warpHandleCenterY);
                handle.closeSubPath();
                g.fillPath(handle);
            }
        }

        if (stateSnapshot.playbackProgress >= 0.0f)
        {
            const int64_t playheadSample = static_cast<int64_t>(std::llround(
                juce::jlimit(0.0f, 1.0f, stateSnapshot.playbackProgress)
                * static_cast<float>(juce::jmax<int64_t>(1, stateSnapshot.totalSamples - 1))));
            const float playheadNorm = displayNormalizedPositionForSample(warpDisplayMap, playheadSample);
            if (playheadNorm >= visibleStart && playheadNorm <= visibleEnd)
            {
                const int playheadX = waveformBounds.getX()
                    + juce::roundToInt(((playheadNorm - visibleStart) / visibleWidth)
                                       * static_cast<float>(waveformBounds.getWidth()));
                g.setColour(juce::Colour(0xff8df0b8));
                g.drawVerticalLine(playheadX,
                                   static_cast<float>(waveformBounds.getY() + 2),
                                   static_cast<float>(waveformBounds.getBottom() - 2));
            }
        }
    }
    else
    {
        g.setColour(juce::Colour(0xff6b7787));
        const auto emptyText = stateSnapshot.isLoading && stateSnapshot.statusText.isNotEmpty()
            ? stateSnapshot.statusText
            : juce::String("Async loader ready. Click here to load a file.");
        g.drawFittedText(emptyText,
                         waveformBounds.reduced(12),
                         juce::Justification::centred,
                         2);
    }

    if (waveformBounds.isEmpty())
    {
        sliceModeArea = {};
    }
    else
    {
        auto overlayArea = waveformBounds.reduced(8, 8);
        sliceModeArea = overlayArea.removeFromBottom(16).withWidth(156);
    }

    if (!sliceModeArea.isEmpty())
    {
        const auto overlayBounds = sliceModeArea.expanded(4, 3);
        g.setColour(juce::Colour(0xff0d1014).withAlpha(0.72f));
        g.fillRoundedRectangle(overlayBounds.toFloat(), 6.0f);
        g.setColour(juce::Colour(0xff32404f).withAlpha(0.65f));
        g.drawRoundedRectangle(overlayBounds.toFloat(), 6.0f, 1.0f);
    }

    auto drawModeButton = [&](juce::Rectangle<int> area, const juce::String& text, bool active)
    {
        g.setColour(active ? juce::Colour(0xfff6b64f) : juce::Colour(0xff202833));
        g.fillRoundedRectangle(area.toFloat(), 5.0f);
        g.setColour(active ? juce::Colour(0xff1a1f24) : juce::Colour(0xff8d9aab));
        g.setFont(juce::Font(juce::FontOptions(10.5f, juce::Font::bold)));
        g.drawText(text, area, juce::Justification::centred);
    };

    drawModeButton(getSliceModeButtonBounds(SampleSliceMode::Uniform), "U", stateSnapshot.sliceMode == SampleSliceMode::Uniform);
    drawModeButton(getSliceModeButtonBounds(SampleSliceMode::Transient), "TR", stateSnapshot.sliceMode == SampleSliceMode::Transient);
    drawModeButton(getSliceModeButtonBounds(SampleSliceMode::Beat),
                   "B" + juce::String(stateSnapshot.beatDivision),
                   stateSnapshot.sliceMode == SampleSliceMode::Beat);
    drawModeButton(getSliceModeButtonBounds(SampleSliceMode::Manual), "M", stateSnapshot.sliceMode == SampleSliceMode::Manual);
}

void SampleModeComponent::resized()
{
}

void SampleModeComponent::showWarpContextMenu(const juce::Point<int>& point)
{
    if (engine == nullptr)
        return;

    const int clickedWarpMarker = hitTestWarpMarker(point, 8.0f);
    juce::PopupMenu menu;
    menu.addItem(1, "Add Warp Marker Here");
    menu.addItem(2,
                 "Add Warp Marker Here (Snap)",
                 loadedSample != nullptr && loadedSample->analysis.beatTickSamples.size() >= 2);
    menu.addItem(3, "Auto Add From Visible Transients/Ticks");
    if (clickedWarpMarker >= 0)
    {
        menu.addSeparator();
        menu.addItem(4, "Remove This Warp Marker");
    }
    menu.addSeparator();
    menu.addItem(5, "Remove All Warp Markers", !stateSnapshot.warpMarkers.empty());
    menu.addItem(6, "Keep First + Last Warp Markers", stateSnapshot.warpMarkers.size() > 2);

    juce::Component::SafePointer<SampleModeComponent> safeThis(this);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withMousePosition(),
                       [safeThis, point, clickedWarpMarker](int result)
                       {
                           if (safeThis == nullptr || safeThis->engine == nullptr || result <= 0)
                               return;

                           switch (result)
                           {
                               case 1:
                                   safeThis->engine->createWarpMarkerAtNormalizedPosition(
                                       safeThis->normalizedPositionFromPoint(point));
                                   break;
                               case 2:
                                   safeThis->engine->createWarpMarkerAtNearestGuide(
                                       safeThis->normalizedPositionFromPoint(point));
                                   break;
                               case 3:
                                   safeThis->engine->createWarpMarkersFromVisibleTransientClusters();
                                   break;
                               case 4:
                                   if (clickedWarpMarker >= 0)
                                       safeThis->engine->deleteWarpMarker(clickedWarpMarker);
                                   break;
                               case 5:
                                   safeThis->engine->clearWarpMarkers();
                                   break;
                               case 6:
                                   safeThis->engine->keepBoundaryWarpMarkers();
                                   break;
                               default:
                                   break;
                           }

                           safeThis->refreshFromEngine();
                       });
}

double SampleModeComponent::getCurrentPpqGridStepBeats() const
{
    if (loadedSample == nullptr || waveformBounds.isEmpty() || stateSnapshot.totalSamples <= 1)
        return 0.0;

    const auto beatTicks = sanitizeWarpBeatTicks(loadedSample->analysis.beatTickSamples,
                                                 loadedSample->sourceLengthSamples);
    if (beatTicks.size() < 2)
        return 0.0;

    const auto stableVisibleRange = getVisibleRange();
    const int64_t stableRangeStartSample = juce::jlimit<int64_t>(
        0,
        juce::jmax<int64_t>(0, stateSnapshot.totalSamples - 1),
        static_cast<int64_t>(std::llround(stableVisibleRange.getStart()
                                          * static_cast<float>(juce::jmax<int64_t>(1, stateSnapshot.totalSamples - 1)))));
    const int64_t stableRangeEndSample = juce::jlimit<int64_t>(
        stableRangeStartSample + 1,
        juce::jmax<int64_t>(1, stateSnapshot.totalSamples - 1),
        static_cast<int64_t>(std::llround(stableVisibleRange.getEnd()
                                          * static_cast<float>(juce::jmax<int64_t>(1, stateSnapshot.totalSamples - 1)))));
    const double stableBeatStart = computeBeatTickPosition(beatTicks, stableRangeStartSample);
    const double stableBeatEnd = computeBeatTickPosition(beatTicks, stableRangeEndSample);
    if (!std::isfinite(stableBeatStart)
        || !std::isfinite(stableBeatEnd)
        || stableBeatEnd <= stableBeatStart)
    {
        return 0.0;
    }

    return choosePpqGridBeatStep(stableBeatEnd - stableBeatStart,
                                 waveformBounds.getWidth(),
                                 28.0);
}

void SampleModeComponent::showSliceContextMenu(const juce::Point<int>& point)
{
    juce::ignoreUnused(point);
    if (engine == nullptr)
        return;

    const bool editableSliceMode = stateSnapshot.sliceMode == SampleSliceMode::Transient
        || stateSnapshot.sliceMode == SampleSliceMode::Manual;
    if (!editableSliceMode)
        return;

    const double gridStepBeats = getCurrentPpqGridStepBeats();
    const bool currentSelectionOnly = stateSnapshot.useLegacyLoopEngine;
    const bool hasEditableMarkers = stateSnapshot.sliceMode == SampleSliceMode::Transient
        ? !stateSnapshot.transientMarkers.empty()
        : !stateSnapshot.cuePoints.empty();

    juce::PopupMenu menu;
    menu.addItem(1,
                 currentSelectionOnly
                     ? "Snap Current Selection to Nearest PPQ Grid"
                     : "Snap Slice Points to Nearest PPQ Grid",
                 hasEditableMarkers && gridStepBeats > 0.0);
    menu.addSeparator();
    menu.addItem(2, "Undo Slice Edit", engine->canUndoSliceEdit());

    juce::Component::SafePointer<SampleModeComponent> safeThis(this);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withMousePosition(),
                       [safeThis, gridStepBeats, currentSelectionOnly](int result)
                       {
                           if (safeThis == nullptr || safeThis->engine == nullptr || result <= 0)
                               return;

                           switch (result)
                           {
                               case 1:
                                   safeThis->engine->snapSlicePointsToNearestPpqGrid(gridStepBeats, currentSelectionOnly);
                                   break;
                               case 2:
                                   safeThis->engine->undoLastSliceEdit();
                                   break;
                               default:
                                   break;
                           }

                           safeThis->refreshFromEngine();
                       });
}

void SampleModeComponent::mouseDown(const juce::MouseEvent& event)
{
    const auto point = event.getPosition();
    draggingLegacyLoopWindow = false;
    legacyLoopDragStartX = 0;
    legacyLoopDragStepOffset = 0;
    draggingVisibleSliceSlot = -1;
    draggingCueIndex = -1;
    draggingWarpMarkerIndex = -1;
    draggingWarpMouseDownPoint = {};
    draggingTransientIndex = -1;
    createdCueOnMouseDown = false;

    for (const auto mode : { SampleSliceMode::Uniform, SampleSliceMode::Transient, SampleSliceMode::Beat, SampleSliceMode::Manual })
    {
        if (getSliceModeButtonBounds(mode).contains(point))
        {
            if (engine != nullptr)
            {
                if (mode == SampleSliceMode::Beat && stateSnapshot.sliceMode == SampleSliceMode::Beat)
                {
                    const int nextDivision = (stateSnapshot.beatDivision >= 8)
                        ? 1
                        : (stateSnapshot.beatDivision * 2);
                    engine->setBeatDivision(nextDivision);
                }
                else
                {
                    engine->setSliceMode(mode);
                }
            }
            return;
        }
    }

    for (const auto mode : { SampleTriggerMode::OneShot, SampleTriggerMode::Loop })
    {
        if (getTriggerModeButtonBounds(mode).contains(point))
        {
            if (engine != nullptr)
                engine->setTriggerMode(mode);
            return;
        }
    }

    if (getLegacyLoopButtonBounds().contains(point))
    {
        if (engine != nullptr)
            engine->setLegacyLoopEngineEnabled(!stateSnapshot.useLegacyLoopEngine);
        return;
    }

    if (getLegacyLoopBarsButtonBounds().contains(point))
    {
        if (onRequestLegacyLoopBarsMenu)
            onRequestLegacyLoopBarsMenu();
        return;
    }

    if (prev16Bounds.contains(point))
    {
        if (onNavigateVisibleBank && stateSnapshot.canNavigateLeft)
            onNavigateVisibleBank(-1);
        return;
    }

    if (next16Bounds.contains(point))
    {
        if (onNavigateVisibleBank && stateSnapshot.canNavigateRight)
            onNavigateVisibleBank(1);
        return;
    }

    if (random16Bounds.contains(point))
    {
        if (engine != nullptr
            && stateSnapshot.visibleSliceBankCount > 1
            && !stateSnapshot.useLegacyLoopEngine)
        {
            engine->randomizeVisibleBank();
        }
        return;
    }

    if (tempoHalfBounds.contains(point))
    {
        if (engine != nullptr)
            engine->scaleAnalyzedTempo(0.5);
        return;
    }

    if (tempoDoubleBounds.contains(point))
    {
        if (engine != nullptr)
            engine->scaleAnalyzedTempo(2.0);
        return;
    }

    if (copyLoopBounds.contains(point))
    {
        if (onCopyToLoop)
            onCopyToLoop();
        return;
    }

    if (copyGrainBounds.contains(point))
    {
        if (onCopyToGrain)
            onCopyToGrain();
        return;
    }

    if (loadedSample == nullptr && waveformBounds.contains(point))
    {
        if (onRequestLoad)
            onRequestLoad();
        return;
    }

    if (engine != nullptr
        && loadedSample != nullptr
        && waveformBounds.contains(point)
        && getWarpMarkerLaneBounds().contains(point))
    {
        grabKeyboardFocus();
        if (event.mods.isPopupMenu())
        {
            showWarpContextMenu(point);
            return;
        }

        if (event.getNumberOfClicks() >= 2)
        {
            if (event.mods.isShiftDown())
            {
                engine->createWarpMarkersFromVisibleTransientClusters();
            }
            else
            {
                const int exactWarpMarkerIndex = hitTestExactWarpMarkerHandle(point);
                if (exactWarpMarkerIndex >= 0)
                    engine->deleteWarpMarker(exactWarpMarkerIndex);
                else
                    draggingWarpMarkerIndex = event.mods.isAltDown()
                        ? engine->createWarpMarkerAtNearestGuide(normalizedPositionFromPoint(point))
                        : engine->createWarpMarkerAtNormalizedPosition(normalizedPositionFromPoint(point));
            }

            refreshFromEngine();
            return;
        }

        draggingWarpMarkerIndex = hitTestExactWarpMarkerHandle(point);
        if (draggingWarpMarkerIndex >= 0)
        {
            engine->beginInteractiveWarpEdit();
            draggingWarpMouseDownPoint = point;
            return;
        }

        return;
    }

    if (engine != nullptr
        && loadedSample != nullptr
        && waveformBounds.contains(point)
        && !getWarpMarkerLaneBounds().contains(point)
        && event.mods.isPopupMenu()
        && (stateSnapshot.sliceMode == SampleSliceMode::Transient
            || stateSnapshot.sliceMode == SampleSliceMode::Manual))
    {
        grabKeyboardFocus();
        showSliceContextMenu(point);
        return;
    }

    if (engine != nullptr
        && loadedSample != nullptr
        && waveformBounds.contains(point)
        && event.getNumberOfClicks() >= 2)
    {
        if (stateSnapshot.sliceMode == SampleSliceMode::Transient)
        {
            const int markerIndex = hitTestTransientMarker(point);
            if (markerIndex >= 0)
            {
                engine->deleteTransientMarker(markerIndex);
                refreshFromEngine();
                return;
            }
        }

        if (stateSnapshot.sliceMode == SampleSliceMode::Manual)
        {
            const int cueIndex = hitTestCuePoint(point);
            if (cueIndex >= 0)
            {
                engine->deleteCuePoint(cueIndex);
                refreshFromEngine();
                return;
            }
        }
    }

    if (engine != nullptr
        && event.mods.isCommandDown()
        && stateSnapshot.useLegacyLoopEngine
        && getVisibleLegacyLoopRangeBounds().contains(point))
    {
        grabKeyboardFocus();
        engine->beginInteractiveLegacyLoopEdit();
        draggingLegacyLoopWindow = true;
        legacyLoopDragStartX = point.x;
        legacyLoopDragStepOffset = 0;
        return;
    }

    if (engine != nullptr
        && stateSnapshot.sliceMode == SampleSliceMode::Transient
        && waveformBounds.contains(point)
        && !event.mods.isShiftDown()
        && !event.mods.isPopupMenu())
    {
        int visibleSliceSlot = hitTestExactVisibleSliceMarkerHandle(point);
        if (visibleSliceSlot < 0)
            visibleSliceSlot = hitTestVisibleSliceMarker(point);
        if (visibleSliceSlot < 0)
        {
            if (const auto lane = getSliceMarkerHandleBounds(); !lane.isEmpty())
                visibleSliceSlot = hitTestVisibleSliceMarker({ point.x, lane.getCentreY() });
        }
        if (visibleSliceSlot >= 0)
        {
            grabKeyboardFocus();
            draggingTransientIndex = engine->resolveTransientMarkerIndexForVisibleSlot(visibleSliceSlot);
            if (draggingTransientIndex >= 0)
                engine->beginSliceEditGesture();
            return;
        }
    }

    if (engine != nullptr && event.mods.isShiftDown() && waveformBounds.contains(point))
    {
        grabKeyboardFocus();
        if (stateSnapshot.sliceMode == SampleSliceMode::Transient)
        {
            int visibleSliceSlot = hitTestExactVisibleSliceMarkerHandle(point);
            if (visibleSliceSlot < 0)
                visibleSliceSlot = hitTestVisibleSliceMarker(point);
            if (visibleSliceSlot < 0)
            {
                if (const auto lane = getSliceMarkerHandleBounds(); !lane.isEmpty())
                    visibleSliceSlot = hitTestVisibleSliceMarker({ point.x, lane.getCentreY() });
            }
            if (visibleSliceSlot < 0)
                visibleSliceSlot = hitTestVisibleSlice(point);

            if (visibleSliceSlot >= 0)
            {
                draggingVisibleSliceSlot = visibleSliceSlot;
                engine->beginSliceEditGesture();
            }
            return;
        }

        if (stateSnapshot.sliceMode == SampleSliceMode::Manual)
        {
            draggingCueIndex = hitTestCuePoint(point);
            if (draggingCueIndex >= 0)
            {
                engine->selectCuePoint(draggingCueIndex);
                engine->beginSliceEditGesture();
                return;
            }
        }

        int visibleSliceSlot = hitTestExactVisibleSliceMarkerHandle(point);
        if (visibleSliceSlot < 0)
            visibleSliceSlot = hitTestVisibleSliceMarker(point);
        if (visibleSliceSlot < 0)
        {
            if (const auto lane = getSliceMarkerHandleBounds(); !lane.isEmpty())
                visibleSliceSlot = hitTestVisibleSliceMarker({ point.x, lane.getCentreY() });
        }
        if (visibleSliceSlot < 0)
            visibleSliceSlot = hitTestVisibleSlice(point);
        if (visibleSliceSlot >= 0)
        {
            if (stateSnapshot.sliceMode == SampleSliceMode::Manual)
            {
                draggingCueIndex = findCuePointForVisibleSlice(visibleSliceSlot);
                if (draggingCueIndex >= 0)
                {
                    engine->selectCuePoint(draggingCueIndex);
                    engine->beginSliceEditGesture();
                    return;
                }
            }
        }
    }

    if (engine != nullptr && stateSnapshot.sliceMode == SampleSliceMode::Transient && waveformBounds.contains(point))
    {
        grabKeyboardFocus();
        const int markerIndex = hitTestTransientMarker(point);
        if (markerIndex >= 0)
        {
            draggingTransientIndex = markerIndex;
            engine->beginSliceEditGesture();
            return;
        }

        draggingTransientIndex = engine->createTransientMarkerAtNormalizedPosition(normalizedPositionFromPoint(point));
        refreshFromEngine();
        return;
    }

    if (engine != nullptr && stateSnapshot.sliceMode == SampleSliceMode::Manual && waveformBounds.contains(point))
    {
        grabKeyboardFocus();
        const int cueIndex = hitTestCuePoint(point);
        if (cueIndex >= 0)
        {
            engine->selectCuePoint(cueIndex);
            draggingCueIndex = cueIndex;
            engine->beginSliceEditGesture();
            refreshFromEngine();
            return;
        }

        draggingCueIndex = engine->createCuePointAtNormalizedPosition(normalizedPositionFromPoint(point));
        createdCueOnMouseDown = (draggingCueIndex >= 0);
        refreshFromEngine();
        return;
    }

    const int visibleSlot = hitTestVisibleSlice(point);
    if (visibleSlot >= 0 && onTriggerVisibleSlice)
        onTriggerVisibleSlice(visibleSlot);
}

void SampleModeComponent::mouseDoubleClick(const juce::MouseEvent& event)
{
    juce::ignoreUnused(event);
}

void SampleModeComponent::mouseDrag(const juce::MouseEvent& event)
{
    if (engine == nullptr)
        return;

    if (draggingLegacyLoopWindow)
    {
        const int pixelsPerStep = juce::jmax(3, waveformBounds.getWidth() / 256);
        const int stepOffset = (event.getPosition().x - legacyLoopDragStartX) / juce::jmax(1, pixelsPerStep);
        const int deltaSteps = stepOffset - legacyLoopDragStepOffset;
        if (deltaSteps != 0 && engine->nudgeLegacyLoopWindowByAnchorDelta(deltaSteps))
            legacyLoopDragStepOffset = stepOffset;
        return;
    }

    if (draggingVisibleSliceSlot >= 0 && stateSnapshot.sliceMode == SampleSliceMode::Transient)
    {
        const float normalizedPosition = normalizedPositionFromPoint(event.getPosition());
        if (event.mods.isAltDown())
            engine->moveVisibleSliceToNearestTransient(draggingVisibleSliceSlot, normalizedPosition);
        else
            engine->moveVisibleSliceToPosition(draggingVisibleSliceSlot, normalizedPosition);
        refreshFromEngine();
        return;
    }

    if (draggingTransientIndex >= 0 && stateSnapshot.sliceMode == SampleSliceMode::Transient)
    {
        const float normalizedPosition = normalizedPositionFromPoint(event.getPosition());
        if (event.mods.isAltDown())
            draggingTransientIndex = engine->moveTransientMarkerToNearestDetectedTransient(draggingTransientIndex,
                                                                                           normalizedPosition);
        else
            draggingTransientIndex = engine->moveTransientMarker(draggingTransientIndex,
                                                                 normalizedPosition);
        refreshFromEngine();
        return;
    }

    if (draggingWarpMarkerIndex >= 0)
    {
        auto dragPoint = event.getPosition();
        if (event.mods.isShiftDown())
        {
            dragPoint.x = draggingWarpMouseDownPoint.x
                + juce::roundToInt(static_cast<float>(dragPoint.x - draggingWarpMouseDownPoint.x) * kWarpMarkerFineDragScale);
        }

        draggingWarpMarkerIndex = event.mods.isAltDown()
            ? engine->moveWarpMarkerToNearestGuide(draggingWarpMarkerIndex,
                                                   normalizedPositionFromPoint(dragPoint))
            : engine->moveWarpMarker(draggingWarpMarkerIndex,
                                     normalizedPositionFromPoint(dragPoint));
        refreshFromEngine();
        return;
    }

    if (draggingCueIndex >= 0 && stateSnapshot.sliceMode == SampleSliceMode::Manual)
    {
        draggingCueIndex = engine->moveCuePoint(draggingCueIndex,
                                                normalizedPositionFromPoint(event.getPosition()));
        refreshFromEngine();
    }
}

void SampleModeComponent::mouseUp(const juce::MouseEvent&)
{
    const bool finishedLegacyLoopDrag = draggingLegacyLoopWindow;
    const bool finishedWarpDrag = (draggingWarpMarkerIndex >= 0);
    draggingLegacyLoopWindow = false;
    legacyLoopDragStartX = 0;
    legacyLoopDragStepOffset = 0;
    draggingVisibleSliceSlot = -1;
    draggingCueIndex = -1;
    draggingWarpMarkerIndex = -1;
    draggingWarpMouseDownPoint = {};
    draggingTransientIndex = -1;
    createdCueOnMouseDown = false;

    if (engine != nullptr)
        engine->endSliceEditGesture();

    if (finishedLegacyLoopDrag && engine != nullptr)
        engine->endInteractiveLegacyLoopEdit();

    if (finishedWarpDrag && engine != nullptr)
        engine->endInteractiveWarpEdit();
}

void SampleModeComponent::mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel)
{
    if (engine == nullptr || !waveformBounds.contains(event.getPosition()))
        return;

    const bool scrollGesture = event.mods.isShiftDown() || std::abs(wheel.deltaX) > std::abs(wheel.deltaY);
    const float currentZoom = stateSnapshot.viewZoom;
    const float currentScroll = stateSnapshot.viewScroll;

    if (scrollGesture)
    {
        const float delta = (std::abs(wheel.deltaX) > 0.0f ? wheel.deltaX : wheel.deltaY) * 0.08f;
        engine->setViewWindow(currentScroll - delta, currentZoom);
        return;
    }

    const float nextZoom = juce::jlimit(kMinViewZoom, kMaxViewZoom, currentZoom + (wheel.deltaY * 3.0f));
    engine->setViewWindow(currentScroll, nextZoom);
}

bool SampleModeComponent::keyPressed(const juce::KeyPress& key)
{
    if (engine == nullptr)
        return false;

    if (key.getModifiers().isCommandDown()
        && (key.getTextCharacter() == 'z' || key.getTextCharacter() == 'Z'))
    {
        return engine->undoLastSliceEdit();
    }

    if (key.getModifiers().isCommandDown())
    {
        if (key.getKeyCode() == juce::KeyPress::leftKey)
        {
            if (stateSnapshot.useLegacyLoopEngine)
            {
                const bool changed = engine->nudgeLegacyLoopWindowByAnchorDelta(-1);
                if (changed)
                    refreshFromEngine();
                return changed;
            }

            if (onNavigateVisibleBank && stateSnapshot.canNavigateLeft)
            {
                onNavigateVisibleBank(-1);
                return true;
            }
        }

        if (key.getKeyCode() == juce::KeyPress::rightKey)
        {
            if (stateSnapshot.useLegacyLoopEngine)
            {
                const bool changed = engine->nudgeLegacyLoopWindowByAnchorDelta(1);
                if (changed)
                    refreshFromEngine();
                return changed;
            }

            if (onNavigateVisibleBank && stateSnapshot.canNavigateRight)
            {
                onNavigateVisibleBank(1);
                return true;
            }
        }
    }

    if (key.getKeyCode() == juce::KeyPress::deleteKey || key.getKeyCode() == juce::KeyPress::backspaceKey)
    {
        const int warpMarkerIndex = hitTestWarpMarker(getMouseXYRelative());
        if (warpMarkerIndex >= 0)
            return engine->deleteWarpMarker(warpMarkerIndex);

        if (stateSnapshot.sliceMode == SampleSliceMode::Manual)
            return engine->deleteCuePoint(stateSnapshot.selectedCueIndex);

        if (stateSnapshot.sliceMode == SampleSliceMode::Transient)
        {
            const int transientIndex = hitTestTransientMarker(getMouseXYRelative());
            if (transientIndex >= 0)
                return engine->deleteTransientMarker(transientIndex);
        }
    }

    return false;
}

void SampleModeComponent::changeListenerCallback(juce::ChangeBroadcaster* source)
{
    if (source == engine)
        refreshFromEngine();
}

void SampleModeComponent::refreshFromEngine()
{
    if (engine == nullptr)
    {
        stateSnapshot = {};
        loadedSample.reset();
        repaint();
        return;
    }

    stateSnapshot = engine->getStateSnapshot();
    loadedSample = engine->getLoadedSample();
    repaint();
}

int64_t SampleModeComponent::getVisibleSliceMarkerSample(int visibleSlot) const
{
    if (visibleSlot < 0 || visibleSlot >= SliceModel::VisibleSliceCount)
        return -1;

    const auto& slice = stateSnapshot.visibleSlices[static_cast<size_t>(visibleSlot)];
    if (slice.id < 0 || slice.endSample <= slice.startSample)
        return -1;

    return slice.startSample;
}

int SampleModeComponent::hitTestVisibleSlice(const juce::Point<int>& point) const
{
    if (!waveformBounds.contains(point))
        return -1;

    const float normalizedX = normalizedPositionFromPoint(point);

    for (int slot = 0; slot < SliceModel::VisibleSliceCount; ++slot)
    {
        const auto& slice = stateSnapshot.visibleSlices[static_cast<size_t>(slot)];
        if (slice.id < 0)
            continue;
        const bool isLastSlice = (slot == (SliceModel::VisibleSliceCount - 1));
        if (normalizedX >= slice.normalizedStart
            && (normalizedX < slice.normalizedEnd || (isLastSlice && normalizedX <= slice.normalizedEnd)))
            return slot;
    }

    return -1;
}

int SampleModeComponent::hitTestVisibleSliceMarker(const juce::Point<int>& point) const
{
    const auto sliceHandleLane = getSliceMarkerHandleBounds();
    const int markerTop = sliceHandleLane.isEmpty() ? waveformBounds.getY() : sliceHandleLane.getY();
    const auto markerBounds = juce::Rectangle<int>(waveformBounds.getX(),
                                                   markerTop,
                                                   waveformBounds.getWidth(),
                                                   juce::jmax(1, waveformBounds.getBottom() - markerTop));
    if (!markerBounds.expanded(0, 6).contains(point) || stateSnapshot.totalSamples <= 0)
        return -1;

    if (const int exactHandle = hitTestExactVisibleSliceMarkerHandle(point); exactHandle >= 0)
        return exactHandle;

    const auto visibleRange = getVisibleDisplayRange();
    const auto warpDisplayMap = buildSampleWarpDisplayMap(loadedSample.get(), stateSnapshot);
    const float visibleWidth = juce::jmax(1.0e-5f, visibleRange.getLength());
    const float zoomAwareHitRadius = juce::jlimit(9.0f,
                                                  24.0f,
                                                  9.0f + (std::log2(juce::jmax(1.0f, stateSnapshot.viewZoom)) * 3.0f));

    std::array<float, SliceModel::VisibleSliceCount> markerXs {};
    std::array<bool, SliceModel::VisibleSliceCount> hasMarker {};
    for (int slot = 0; slot < SliceModel::VisibleSliceCount; ++slot)
    {
        const int64_t markerSample = getVisibleSliceMarkerSample(slot);
        if (markerSample < 0)
            continue;
        const float markerNorm = displayNormalizedPositionForSample(warpDisplayMap, markerSample);
        if (markerNorm < visibleRange.getStart() || markerNorm > visibleRange.getEnd())
            continue;

        markerXs[static_cast<size_t>(slot)] = waveformBounds.getX()
            + (((markerNorm - visibleRange.getStart()) / visibleWidth) * waveformBounds.getWidth());
        hasMarker[static_cast<size_t>(slot)] = true;
    }

    int bestSlot = -1;
    float bestDistance = std::numeric_limits<float>::max();
    for (int slot = 0; slot < SliceModel::VisibleSliceCount; ++slot)
    {
        if (!hasMarker[static_cast<size_t>(slot)])
            continue;

        const float distance = std::abs(markerXs[static_cast<size_t>(slot)] - static_cast<float>(point.x));
        if (distance < bestDistance)
        {
            bestDistance = distance;
            bestSlot = slot;
        }
    }

    if (bestSlot < 0)
        return -1;

    float maxAllowedDistance = zoomAwareHitRadius;
    if (bestSlot > 0 && hasMarker[static_cast<size_t>(bestSlot - 1)])
    {
        const float gap = markerXs[static_cast<size_t>(bestSlot)] - markerXs[static_cast<size_t>(bestSlot - 1)];
        maxAllowedDistance = juce::jmin(maxAllowedDistance, juce::jmax(6.0f, gap * 0.48f));
    }
    if (bestSlot + 1 < SliceModel::VisibleSliceCount && hasMarker[static_cast<size_t>(bestSlot + 1)])
    {
        const float gap = markerXs[static_cast<size_t>(bestSlot + 1)] - markerXs[static_cast<size_t>(bestSlot)];
        maxAllowedDistance = juce::jmin(maxAllowedDistance, juce::jmax(6.0f, gap * 0.48f));
    }

    return bestDistance <= maxAllowedDistance ? bestSlot : -1;
}

int SampleModeComponent::hitTestExactVisibleSliceMarkerHandle(const juce::Point<int>& point) const
{
    const auto sliceHandleLane = getSliceMarkerHandleBounds();
    if (!sliceHandleLane.expanded(0, 4).contains(point) || stateSnapshot.totalSamples <= 0)
        return -1;

    const auto visibleRange = getVisibleDisplayRange();
    const auto warpDisplayMap = buildSampleWarpDisplayMap(loadedSample.get(), stateSnapshot);
    const float visibleWidth = juce::jmax(1.0e-5f, visibleRange.getLength());
    const float handleHalfWidth = 8.0f;

    for (int slot = 0; slot < SliceModel::VisibleSliceCount; ++slot)
    {
        const int64_t markerSample = getVisibleSliceMarkerSample(slot);
        if (markerSample < 0)
            continue;

        const float markerNorm = displayNormalizedPositionForSample(warpDisplayMap, markerSample);
        if (markerNorm < visibleRange.getStart() || markerNorm > visibleRange.getEnd())
            continue;

        const float x = waveformBounds.getX()
            + (((markerNorm - visibleRange.getStart()) / visibleWidth) * waveformBounds.getWidth());
        const auto handleBounds = juce::Rectangle<float>(x - handleHalfWidth,
                                                         static_cast<float>(sliceHandleLane.getY()),
                                                         handleHalfWidth * 2.0f,
                                                         static_cast<float>(sliceHandleLane.getHeight()));
        if (handleBounds.contains(point.toFloat()))
            return slot;
    }

    return -1;
}

int SampleModeComponent::hitTestCuePoint(const juce::Point<int>& point) const
{
    if (!waveformBounds.contains(point) || stateSnapshot.totalSamples <= 0)
        return -1;

    const auto visibleRange = getVisibleDisplayRange();
    const auto warpDisplayMap = buildSampleWarpDisplayMap(loadedSample.get(), stateSnapshot);
    const float visibleWidth = juce::jmax(1.0e-5f, visibleRange.getLength());

    int bestIndex = -1;
    float bestDistance = std::numeric_limits<float>::max();
    for (size_t i = 0; i < stateSnapshot.cuePoints.size(); ++i)
    {
        const float cueNorm = displayNormalizedPositionForSample(warpDisplayMap,
                                                                 stateSnapshot.cuePoints[i].samplePosition);
        if (cueNorm < visibleRange.getStart() || cueNorm > visibleRange.getEnd())
            continue;

        const float x = waveformBounds.getX()
            + (((cueNorm - visibleRange.getStart()) / visibleWidth) * waveformBounds.getWidth());
        const float distance = std::abs(x - static_cast<float>(point.x));
        if (distance <= kCueHitRadiusPixels && distance < bestDistance)
        {
            bestDistance = distance;
            bestIndex = static_cast<int>(i);
        }
    }

    return bestIndex;
}

int SampleModeComponent::hitTestWarpMarker(const juce::Point<int>& point) const
{
    return hitTestWarpMarker(point, computeWarpMarkerHitRadiusForZoom(stateSnapshot.viewZoom));
}

int SampleModeComponent::hitTestWarpMarker(const juce::Point<int>& point, float hitRadius) const
{
    if (!getWarpMarkerLaneBounds().contains(point) || stateSnapshot.totalSamples <= 0)
        return -1;

    if (const int exactHandle = hitTestExactWarpMarkerHandle(point); exactHandle >= 0)
        return exactHandle;

    const auto visibleRange = getVisibleDisplayRange();
    const auto warpDisplayMap = buildSampleWarpDisplayMap(loadedSample.get(), stateSnapshot);
    const float visibleWidth = juce::jmax(1.0e-5f, visibleRange.getLength());
    const float xNorm = juce::jlimit(0.0f,
                                     1.0f,
                                     static_cast<float>(point.x - waveformBounds.getX())
                                         / juce::jmax(1.0f, static_cast<float>(waveformBounds.getWidth())));
    const float displayNormalized = juce::jlimit(0.0f,
                                                 1.0f,
                                                 visibleRange.getStart() + (xNorm * visibleRange.getLength()));
    const int64_t visibleSampleStart = samplePositionFromDisplayNormalized(warpDisplayMap, visibleRange.getStart());
    const int64_t visibleSampleEnd = samplePositionFromDisplayNormalized(warpDisplayMap, visibleRange.getEnd());
    const int64_t samplesPerPixel = juce::jmax<int64_t>(1,
        static_cast<int64_t>(std::llround(static_cast<double>(juce::jmax<int64_t>(1, std::abs(visibleSampleEnd - visibleSampleStart))))
                                          / static_cast<double>(juce::jmax(1, waveformBounds.getWidth()))));
    const int64_t clickedSample = samplePositionFromDisplayNormalized(warpDisplayMap, displayNormalized);
    const int64_t sampleTolerance = juce::jmax<int64_t>(1,
        static_cast<int64_t>(std::ceil(static_cast<double>(samplesPerPixel) * std::max(1.0f, hitRadius))));

    std::vector<float> visibleMarkerXs;
    visibleMarkerXs.reserve(stateSnapshot.warpMarkers.size());
    std::vector<int> visibleMarkerIndices;
    visibleMarkerIndices.reserve(stateSnapshot.warpMarkers.size());
    for (size_t i = 0; i < stateSnapshot.warpMarkers.size(); ++i)
    {
        const float markerNorm = displayNormalizedPositionForSample(warpDisplayMap,
                                                                    stateSnapshot.warpMarkers[i].samplePosition);
        if (markerNorm < visibleRange.getStart() || markerNorm > visibleRange.getEnd())
            continue;

        visibleMarkerXs.push_back(waveformBounds.getX()
            + (((markerNorm - visibleRange.getStart()) / visibleWidth) * waveformBounds.getWidth()));
        visibleMarkerIndices.push_back(static_cast<int>(i));
    }

    int bestIndex = -1;
    float bestDistance = std::numeric_limits<float>::max();
    for (size_t visibleIndex = 0; visibleIndex < visibleMarkerIndices.size(); ++visibleIndex)
    {
        const int markerIndex = visibleMarkerIndices[visibleIndex];
        const float x = visibleMarkerXs[visibleIndex];
        const float distance = std::abs(x - static_cast<float>(point.x));
        const int64_t sampleDistance = std::abs(stateSnapshot.warpMarkers[static_cast<size_t>(markerIndex)].samplePosition - clickedSample);
        float maxAllowedDistance = hitRadius;
        if (visibleIndex > 0)
            maxAllowedDistance = juce::jmin(maxAllowedDistance,
                                            juce::jmax(1.0f, (x - visibleMarkerXs[visibleIndex - 1]) * 0.48f));
        if (visibleIndex + 1 < visibleMarkerXs.size())
            maxAllowedDistance = juce::jmin(maxAllowedDistance,
                                            juce::jmax(1.0f, (visibleMarkerXs[visibleIndex + 1] - x) * 0.48f));

        if (distance <= maxAllowedDistance && sampleDistance <= sampleTolerance && distance < bestDistance)
        {
            bestDistance = distance;
            bestIndex = markerIndex;
        }
    }

    return bestIndex;
}

int SampleModeComponent::hitTestExactWarpMarkerHandle(const juce::Point<int>& point) const
{
    const auto warpLane = getWarpMarkerLaneBounds();
    if (!warpLane.contains(point) || stateSnapshot.totalSamples <= 0)
        return -1;

    const auto visibleRange = getVisibleDisplayRange();
    const auto warpDisplayMap = buildSampleWarpDisplayMap(loadedSample.get(), stateSnapshot);
    const float visibleWidth = juce::jmax(1.0e-5f, visibleRange.getLength());
    const float handleHalfWidth = juce::jmax(5.0f,
        computeWarpMarkerHitRadiusForZoom(stateSnapshot.viewZoom) + 2.5f);
    const auto pointF = point.toFloat();

    for (size_t markerIndex = 0; markerIndex < stateSnapshot.warpMarkers.size(); ++markerIndex)
    {
        const auto& marker = stateSnapshot.warpMarkers[markerIndex];
        const float markerNorm = displayNormalizedPositionForSample(warpDisplayMap, marker.samplePosition);
        if (markerNorm < visibleRange.getStart() || markerNorm > visibleRange.getEnd())
            continue;

        const float x = waveformBounds.getX()
            + (((markerNorm - visibleRange.getStart()) / visibleWidth) * waveformBounds.getWidth());
        const auto handleBounds = juce::Rectangle<float>(x - handleHalfWidth,
                                                         static_cast<float>(warpLane.getY()),
                                                         handleHalfWidth * 2.0f,
                                                         static_cast<float>(warpLane.getHeight()));
        if (handleBounds.contains(pointF))
            return static_cast<int>(markerIndex);
    }

    return -1;
}

int SampleModeComponent::hitTestWarpGuideMarker(const juce::Point<int>& point) const
{
    if (loadedSample == nullptr
        || !getWarpMarkerLaneBounds().contains(point)
        || stateSnapshot.totalSamples <= 0)
    {
        return -1;
    }

    const auto visibleRange = getVisibleDisplayRange();
    const auto warpDisplayMap = buildSampleWarpDisplayMap(loadedSample.get(), stateSnapshot);
    const float visibleWidth = juce::jmax(1.0e-5f, visibleRange.getLength());
    const auto sanitizedGuideTicks = sanitizeWarpBeatTicks(loadedSample->analysis.beatTickSamples,
                                                           loadedSample->sourceLengthSamples);

    int bestIndex = -1;
    float bestDistance = std::numeric_limits<float>::max();
    for (size_t i = 0; i < sanitizedGuideTicks.size(); ++i)
    {
        const float markerNorm = displayNormalizedPositionForSample(warpDisplayMap, sanitizedGuideTicks[i]);
        if (markerNorm < visibleRange.getStart() || markerNorm > visibleRange.getEnd())
            continue;

        const float x = waveformBounds.getX()
            + (((markerNorm - visibleRange.getStart()) / visibleWidth) * waveformBounds.getWidth());
        const float distance = std::abs(x - static_cast<float>(point.x));
        if (distance <= kWarpMarkerHitRadiusPixels && distance < bestDistance)
        {
            bestDistance = distance;
            bestIndex = static_cast<int>(i);
        }
    }

    return bestIndex;
}

int SampleModeComponent::findCuePointForVisibleSlice(int visibleSlot) const
{
    if (visibleSlot < 0 || visibleSlot >= SliceModel::VisibleSliceCount)
        return -1;

    const auto& slice = stateSnapshot.visibleSlices[static_cast<size_t>(visibleSlot)];
    if (slice.id < 0 || slice.endSample <= slice.startSample)
        return -1;

    int bestIndex = -1;
    int64_t bestDistance = std::numeric_limits<int64_t>::max();
    for (size_t i = 0; i < stateSnapshot.cuePoints.size(); ++i)
    {
        const auto& cue = stateSnapshot.cuePoints[i];
        if (cue.samplePosition <= 0)
            continue;

        const int64_t distance = std::abs(cue.samplePosition - slice.startSample);
        if (distance < bestDistance)
        {
            bestDistance = distance;
            bestIndex = static_cast<int>(i);
            if (distance == 0)
                break;
        }
    }

    return bestIndex;
}

int SampleModeComponent::findTransientMarkerForVisibleSlice(int visibleSlot) const
{
    if (visibleSlot < 0 || visibleSlot >= SliceModel::VisibleSliceCount)
        return -1;

    const auto& slice = stateSnapshot.visibleSlices[static_cast<size_t>(visibleSlot)];
    if (slice.id < 0 || slice.endSample <= slice.startSample)
        return -1;

    int bestIndex = -1;
    int64_t bestDistance = std::numeric_limits<int64_t>::max();
    for (size_t i = 0; i < stateSnapshot.transientMarkers.size(); ++i)
    {
        const int64_t markerSample = stateSnapshot.transientMarkers[i];
        if (markerSample <= 0)
            continue;

        const int64_t distance = std::abs(markerSample - slice.startSample);
        if (distance < bestDistance)
        {
            bestDistance = distance;
            bestIndex = static_cast<int>(i);
            if (distance == 0)
                break;
        }
    }

    return bestIndex;
}

int SampleModeComponent::hitTestTransientMarker(const juce::Point<int>& point) const
{
    if (!waveformBounds.contains(point) || stateSnapshot.totalSamples <= 0)
        return -1;

    const auto visibleRange = getVisibleDisplayRange();
    const auto warpDisplayMap = buildSampleWarpDisplayMap(loadedSample.get(), stateSnapshot);
    const float visibleWidth = juce::jmax(1.0e-5f, visibleRange.getLength());

    int bestIndex = -1;
    float bestDistance = std::numeric_limits<float>::max();
    for (size_t i = 0; i < stateSnapshot.transientMarkers.size(); ++i)
    {
        const float markerNorm = displayNormalizedPositionForSample(warpDisplayMap,
                                                                    stateSnapshot.transientMarkers[i]);
        if (markerNorm < visibleRange.getStart() || markerNorm > visibleRange.getEnd())
            continue;

        const float x = waveformBounds.getX()
            + (((markerNorm - visibleRange.getStart()) / visibleWidth) * waveformBounds.getWidth());
        const float distance = std::abs(x - static_cast<float>(point.x));
        if (distance <= kCueHitRadiusPixels && distance < bestDistance)
        {
            bestDistance = distance;
            bestIndex = static_cast<int>(i);
        }
    }

    return bestIndex;
}

juce::Rectangle<int> SampleModeComponent::getVisibleLegacyLoopRangeBounds() const
{
    if (stateSnapshot.totalSamples <= 0 || waveformBounds.isEmpty())
        return {};

    const auto visibleRange = getVisibleDisplayRange();
    const auto warpDisplayMap = buildSampleWarpDisplayMap(loadedSample.get(), stateSnapshot);
    const float visibleWidth = juce::jmax(1.0e-5f, visibleRange.getLength());
    if (const auto loopRange = computeSampleLoopVisualRange(stateSnapshot))
    {
        const int64_t loopStartSample = static_cast<int64_t>(std::llround(loopRange->getStart()
                                                                          * static_cast<float>(stateSnapshot.totalSamples)));
        const int64_t loopEndSample = static_cast<int64_t>(std::llround(loopRange->getEnd()
                                                                        * static_cast<float>(stateSnapshot.totalSamples)));
        const float loopStartNorm = juce::jmax(displayNormalizedPositionForSample(warpDisplayMap, loopStartSample),
                                               visibleRange.getStart());
        const float loopEndNorm = juce::jmin(displayNormalizedPositionForSample(warpDisplayMap, loopEndSample),
                                             visibleRange.getEnd());
        if (loopEndNorm > loopStartNorm)
        {
            const int loopX = waveformBounds.getX()
                + juce::roundToInt(((loopStartNorm - visibleRange.getStart()) / visibleWidth)
                                   * static_cast<float>(waveformBounds.getWidth()));
            const int loopEndX = waveformBounds.getX()
                + juce::roundToInt(((loopEndNorm - visibleRange.getStart()) / visibleWidth)
                                   * static_cast<float>(waveformBounds.getWidth()));
            return juce::Rectangle<int>(loopX,
                                        waveformBounds.getY() + 2,
                                        juce::jmax(2, loopEndX - loopX),
                                        waveformBounds.getHeight() - 4);
        }
    }

    return {};
}

float SampleModeComponent::normalizedPositionFromPoint(const juce::Point<int>& point) const
{
    const auto visibleRange = getVisibleDisplayRange();
    const auto warpDisplayMap = buildSampleWarpDisplayMap(loadedSample.get(), stateSnapshot);
    const int relativeX = point.x - waveformBounds.getX();
    const float xNorm = juce::jlimit(0.0f,
                                     1.0f,
                                     static_cast<float>(relativeX)
                                         / juce::jmax(1.0f, static_cast<float>(waveformBounds.getWidth())));
    const float displayNormalized = juce::jlimit(0.0f, 1.0f, visibleRange.getStart() + (xNorm * visibleRange.getLength()));
    const int64_t samplePosition = samplePositionFromDisplayNormalized(warpDisplayMap, displayNormalized);
    return safeNormalizedPosition(samplePosition, stateSnapshot.totalSamples);
}

juce::Range<float> SampleModeComponent::getVisibleRange() const
{
    const float zoom = juce::jlimit(kMinViewZoom, kMaxViewZoom, stateSnapshot.viewZoom);
    const float visibleLength = 1.0f / zoom;
    const float start = juce::jlimit(0.0f, juce::jmax(0.0f, 1.0f - visibleLength), stateSnapshot.viewScroll);
    return { start, juce::jlimit(0.0f, 1.0f, start + visibleLength) };
}

juce::Range<float> SampleModeComponent::getVisibleDisplayRange() const
{
    const auto sampleRange = getVisibleRange();
    if (loadedSample == nullptr || stateSnapshot.totalSamples <= 0)
        return sampleRange;

    const auto warpDisplayMap = buildSampleWarpDisplayMap(loadedSample.get(), stateSnapshot);
    if (!warpDisplayMap.active)
        return sampleRange;

    const int64_t totalSamples = juce::jmax<int64_t>(1, stateSnapshot.totalSamples);
    const auto sampleFromNormalized = [totalSamples](float normalized)
    {
        return juce::jlimit<int64_t>(0,
                                     totalSamples - 1,
                                     static_cast<int64_t>(std::llround(juce::jlimit(0.0f, 1.0f, normalized)
                                                                       * static_cast<float>(totalSamples - 1))));
    };

    const int64_t startSample = sampleFromNormalized(sampleRange.getStart());
    const int64_t endSample = juce::jmax<int64_t>(startSample + 1,
                                                  sampleFromNormalized(sampleRange.getEnd()));
    float displayStart = displayNormalizedPositionForSample(warpDisplayMap, startSample);
    float displayEnd = displayNormalizedPositionForSample(warpDisplayMap,
                                                          juce::jlimit<int64_t>(startSample + 1,
                                                                                totalSamples - 1,
                                                                                endSample));
    if (displayEnd < displayStart)
        std::swap(displayStart, displayEnd);

    if ((displayEnd - displayStart) < 1.0e-5f)
        return sampleRange;

    return { displayStart, juce::jmin(1.0f, juce::jmax(displayStart + 1.0e-5f, displayEnd)) };
}

juce::Rectangle<int> SampleModeComponent::getWarpMarkerLaneBounds() const
{
    if (loadedSample == nullptr || waveformBounds.isEmpty())
        return {};

    const auto sanitizedGuideTicks = sanitizeWarpBeatTicks(loadedSample->analysis.beatTickSamples,
                                                           loadedSample->sourceLengthSamples);
    if (sanitizedGuideTicks.size() < 2 && stateSnapshot.warpMarkers.empty())
        return {};

    return waveformBounds.withTrimmedTop(kWarpMarkerLaneTopOffset)
        .withHeight(kWarpMarkerLaneHeight);
}

juce::Rectangle<int> SampleModeComponent::getSliceMarkerHandleBounds() const
{
    if (waveformBounds.isEmpty())
        return {};

    if (const auto warpLane = getWarpMarkerLaneBounds(); !warpLane.isEmpty())
    {
        return juce::Rectangle<int>(waveformBounds.getX(),
                                    warpLane.getBottom() + kSliceMarkerHandleLaneGap,
                                    waveformBounds.getWidth(),
                                    kSliceMarkerHandleLaneHeight);
    }

    return juce::Rectangle<int>(waveformBounds.getX(),
                                waveformBounds.getY() + 2,
                                waveformBounds.getWidth(),
                                kSliceMarkerHandleLaneHeight);
}

juce::Rectangle<int> SampleModeComponent::getSliceModeButtonBounds(SampleSliceMode mode) const
{
    const int index = static_cast<int>(mode);
    const int buttonWidth = 36;
    const int buttonGap = 4;
    return { sliceModeArea.getX() + (index * (buttonWidth + buttonGap)),
             sliceModeArea.getY(),
             buttonWidth,
             sliceModeArea.getHeight() };
}

juce::Rectangle<int> SampleModeComponent::getTriggerModeButtonBounds(SampleTriggerMode mode) const
{
    const int index = static_cast<int>(mode);
    const int buttonWidth = 45;
    return { triggerModeArea.getX() + (index * (buttonWidth + 4)),
             triggerModeArea.getY(),
             buttonWidth,
             triggerModeArea.getHeight() };
}

juce::Rectangle<int> SampleModeComponent::getLegacyLoopButtonBounds() const
{
    return legacyLoopArea;
}

juce::Rectangle<int> SampleModeComponent::getLegacyLoopBarsButtonBounds() const
{
    return legacyLoopBarsBounds;
}

void SampleModeComponent::timerCallback()
{
    if (engine != nullptr)
        refreshFromEngine();
}
