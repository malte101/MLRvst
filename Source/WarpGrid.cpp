#include "WarpGrid.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace WarpGrid
{
std::vector<int64_t> sanitizeBeatTicks(const std::vector<int64_t>& beatTickSamples,
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

int64_t computeEdgeTickInterval(const std::vector<int64_t>& beatTicks, bool useEndEdge)
{
    if (beatTicks.size() < 2)
        return 0;

    const int intervalCount = static_cast<int>(beatTicks.size()) - 1;
    const int windowSize = juce::jmin(4, intervalCount);
    const int startInterval = useEndEdge ? (intervalCount - windowSize) : 0;

    std::vector<int64_t> intervals;
    intervals.reserve(static_cast<size_t>(windowSize));
    for (int i = startInterval; i < startInterval + windowSize; ++i)
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

double computeBeatPositionFromSample(const std::vector<int64_t>& beatTicks,
                                     int64_t samplePosition)
{
    if (beatTicks.size() < 2)
        return 0.0;

    const int64_t startEdgeInterval = computeEdgeTickInterval(beatTicks, false);
    const int64_t endEdgeInterval = computeEdgeTickInterval(beatTicks, true);
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
    const int64_t tickStart = beatTicks[static_cast<size_t>(prevIndex)];
    const int64_t tickEnd = beatTicks[static_cast<size_t>(nextIndex)];
    const int64_t interval = juce::jmax<int64_t>(1, tickEnd - tickStart);
    const double alpha = static_cast<double>(samplePosition - tickStart) / static_cast<double>(interval);
    return static_cast<double>(prevIndex) + juce::jlimit(0.0, 1.0, alpha);
}

std::vector<SampleWarpMarker> sanitizeMarkers(const std::vector<SampleWarpMarker>& warpMarkers,
                                              int64_t totalSamples)
{
    std::vector<SampleWarpMarker> sanitized;
    sanitized.reserve(warpMarkers.size());
    for (const auto& marker : warpMarkers)
    {
        if (!std::isfinite(marker.beatPosition)
            || marker.samplePosition <= 0
            || marker.samplePosition >= juce::jmax<int64_t>(1, totalSamples - 1))
        {
            continue;
        }

        sanitized.push_back(marker);
    }

    std::sort(sanitized.begin(), sanitized.end(),
              [](const SampleWarpMarker& lhs, const SampleWarpMarker& rhs)
              {
                  if (lhs.beatPosition < rhs.beatPosition)
                      return lhs.beatPosition < rhs.beatPosition;
                  if (lhs.beatPosition > rhs.beatPosition)
                      return false;
                  if (lhs.samplePosition != rhs.samplePosition)
                      return lhs.samplePosition < rhs.samplePosition;
                  return lhs.id < rhs.id;
              });

    std::vector<SampleWarpMarker> deduped;
    deduped.reserve(sanitized.size());
    int64_t previousSample = -1;
    double previousBeat = -std::numeric_limits<double>::infinity();
    for (const auto& marker : sanitized)
    {
        if (marker.samplePosition <= previousSample
            || marker.beatPosition <= (previousBeat + kMarkerOrderEpsilon))
        {
            continue;
        }

        deduped.push_back(marker);
        previousSample = marker.samplePosition;
        previousBeat = marker.beatPosition;
    }

    return deduped;
}

std::vector<Anchor> buildAnchors(const std::vector<int64_t>& beatTicks,
                                 const std::vector<SampleWarpMarker>& warpMarkers,
                                 int64_t totalSamples)
{
    std::vector<Anchor> anchors;
    if (beatTicks.size() < 2 || totalSamples <= 1)
        return anchors;

    const int64_t endSample = juce::jmax<int64_t>(1, totalSamples - 1);
    const double startBeat = computeBeatPositionFromSample(beatTicks, 0);
    const double endBeat = computeBeatPositionFromSample(beatTicks, endSample);

    anchors.push_back({ 0, startBeat, startBeat });
    for (const auto& marker : sanitizeMarkers(warpMarkers, totalSamples))
    {
        const double baseBeat = computeBeatPositionFromSample(beatTicks, marker.samplePosition);
        if (!std::isfinite(baseBeat)
            || baseBeat <= (anchors.back().baseBeatPosition + kMarkerOrderEpsilon)
            || marker.beatPosition <= (anchors.back().targetBeatPosition + kMarkerOrderEpsilon)
            || marker.beatPosition >= (endBeat - kMarkerOrderEpsilon))
        {
            continue;
        }

        anchors.push_back({ marker.samplePosition, baseBeat, marker.beatPosition });
    }
    anchors.push_back({ endSample, endBeat, endBeat });
    return anchors;
}

double computeWarpedBeatPositionForSample(const std::vector<int64_t>& beatTicks,
                                          const std::vector<SampleWarpMarker>& warpMarkers,
                                          int64_t samplePosition,
                                          int64_t totalSamples)
{
    return computeWarpedBeatPositionForSample(beatTicks,
                                              buildAnchors(beatTicks, warpMarkers, totalSamples),
                                              samplePosition,
                                              totalSamples);
}

double computeWarpedBeatPositionForSample(const std::vector<int64_t>& beatTicks,
                                          const std::vector<Anchor>& anchors,
                                          int64_t samplePosition,
                                          int64_t totalSamples)
{
    const double baseBeat = computeBeatPositionFromSample(beatTicks, samplePosition);
    if (anchors.size() < 2)
        return baseBeat;

    const auto sample = juce::jlimit<int64_t>(0, juce::jmax<int64_t>(1, totalSamples - 1), samplePosition);
    size_t rightIndex = 1;
    while (rightIndex < anchors.size() && sample > anchors[rightIndex].samplePosition)
        ++rightIndex;

    if (rightIndex >= anchors.size())
        rightIndex = anchors.size() - 1;

    const auto& leftAnchor = anchors[rightIndex - 1];
    const auto& rightAnchor = anchors[rightIndex];
    const double beatDenominator = rightAnchor.baseBeatPosition - leftAnchor.baseBeatPosition;
    if (beatDenominator > kBeatEpsilon)
    {
        const double alpha = juce::jlimit(0.0,
                                          1.0,
                                          (baseBeat - leftAnchor.baseBeatPosition) / beatDenominator);
        return leftAnchor.targetBeatPosition
            + ((rightAnchor.targetBeatPosition - leftAnchor.targetBeatPosition) * alpha);
    }

    const double sampleDenominator = static_cast<double>(juce::jmax<int64_t>(1, rightAnchor.samplePosition - leftAnchor.samplePosition));
    const double alpha = juce::jlimit(0.0,
                                      1.0,
                                      static_cast<double>(sample - leftAnchor.samplePosition) / sampleDenominator);
    return leftAnchor.targetBeatPosition
        + ((rightAnchor.targetBeatPosition - leftAnchor.targetBeatPosition) * alpha);
}

int64_t computeSamplePositionFromBeatPosition(const std::vector<int64_t>& beatTicks,
                                              double beatPosition,
                                              int64_t totalSamples)
{
    if (beatTicks.empty() || totalSamples <= 0 || !std::isfinite(beatPosition))
        return 0;

    const int64_t startEdgeInterval = computeEdgeTickInterval(beatTicks, false);
    const int64_t endEdgeInterval = computeEdgeTickInterval(beatTicks, true);
    if (startEdgeInterval <= 0 || endEdgeInterval <= 0)
        return juce::jlimit<int64_t>(0, totalSamples - 1, beatTicks.front());

    if (beatPosition <= 0.0)
    {
        const double sample = static_cast<double>(beatTicks.front())
            + (beatPosition * static_cast<double>(startEdgeInterval));
        return juce::jlimit<int64_t>(0, totalSamples - 1, static_cast<int64_t>(std::llround(sample)));
    }

    const int maxIndex = static_cast<int>(beatTicks.size()) - 1;
    if (beatPosition >= static_cast<double>(maxIndex))
    {
        const double sample = static_cast<double>(beatTicks.back())
            + ((beatPosition - static_cast<double>(maxIndex)) * static_cast<double>(endEdgeInterval));
        return juce::jlimit<int64_t>(0, totalSamples - 1, static_cast<int64_t>(std::llround(sample)));
    }

    const int lowerIndex = juce::jlimit(0, maxIndex - 1, static_cast<int>(std::floor(beatPosition)));
    const int upperIndex = juce::jlimit(lowerIndex + 1, maxIndex, lowerIndex + 1);
    const double alpha = juce::jlimit(0.0, 1.0, beatPosition - static_cast<double>(lowerIndex));
    const double start = static_cast<double>(beatTicks[static_cast<size_t>(lowerIndex)]);
    const double end = static_cast<double>(beatTicks[static_cast<size_t>(upperIndex)]);
    return juce::jlimit<int64_t>(0,
                                 totalSamples - 1,
                                 static_cast<int64_t>(std::llround(start + ((end - start) * alpha))));
}

int64_t computeSamplePositionFromWarpedBeatPosition(const std::vector<int64_t>& beatTicks,
                                                    const std::vector<Anchor>& anchors,
                                                    double targetBeatPosition,
                                                    int64_t totalSamples)
{
    if (anchors.size() < 2)
        return computeSamplePositionFromBeatPosition(beatTicks, targetBeatPosition, totalSamples);

    size_t rightIndex = 1;
    while (rightIndex < anchors.size() && targetBeatPosition > anchors[rightIndex].targetBeatPosition)
        ++rightIndex;

    if (rightIndex >= anchors.size())
        rightIndex = anchors.size() - 1;

    const auto& leftAnchor = anchors[rightIndex - 1];
    const auto& rightAnchor = anchors[rightIndex];
    const double targetBeatDelta = rightAnchor.targetBeatPosition - leftAnchor.targetBeatPosition;

    double baseBeatPosition = leftAnchor.baseBeatPosition;
    if (targetBeatDelta > kBeatEpsilon)
    {
        const double alpha = juce::jlimit(0.0,
                                          1.0,
                                          (targetBeatPosition - leftAnchor.targetBeatPosition) / targetBeatDelta);
        baseBeatPosition = leftAnchor.baseBeatPosition
            + ((rightAnchor.baseBeatPosition - leftAnchor.baseBeatPosition) * alpha);
    }

    return computeSamplePositionFromBeatPosition(beatTicks, baseBeatPosition, totalSamples);
}

float computeWarpedBeatSpan(const std::vector<int64_t>& beatTicks,
                            const std::vector<SampleWarpMarker>& warpMarkers,
                            int64_t startSample,
                            int64_t endSample,
                            int64_t totalSamples)
{
    if (beatTicks.size() < 2 || endSample <= startSample)
        return 0.0f;

    const double startBeat = computeWarpedBeatPositionForSample(beatTicks, warpMarkers, startSample, totalSamples);
    const double endBeat = computeWarpedBeatPositionForSample(beatTicks, warpMarkers, endSample, totalSamples);
    const double beatSpan = endBeat - startBeat;
    if (!(beatSpan > 0.0) || !std::isfinite(beatSpan))
        return 0.0f;

    return static_cast<float>(beatSpan);
}
} // namespace WarpGrid
