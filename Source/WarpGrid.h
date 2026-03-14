#pragma once

#include "SampleMode.h"

namespace WarpGrid
{
constexpr double kBeatEpsilon = 1.0e-4;
constexpr double kMarkerOrderEpsilon = 1.0e-7;

struct Anchor
{
    int64_t samplePosition = 0;
    double baseBeatPosition = 0.0;
    double targetBeatPosition = 0.0;
};

std::vector<int64_t> sanitizeBeatTicks(const std::vector<int64_t>& beatTickSamples,
                                       int64_t totalSamples);

int64_t computeEdgeTickInterval(const std::vector<int64_t>& beatTicks, bool useEndEdge);

double computeBeatPositionFromSample(const std::vector<int64_t>& beatTicks,
                                     int64_t samplePosition);

std::vector<SampleWarpMarker> sanitizeMarkers(const std::vector<SampleWarpMarker>& warpMarkers,
                                              int64_t totalSamples);

std::vector<Anchor> buildAnchors(const std::vector<int64_t>& beatTicks,
                                 const std::vector<SampleWarpMarker>& warpMarkers,
                                 int64_t totalSamples);

double computeWarpedBeatPositionForSample(const std::vector<int64_t>& beatTicks,
                                          const std::vector<SampleWarpMarker>& warpMarkers,
                                          int64_t samplePosition,
                                          int64_t totalSamples);

double computeWarpedBeatPositionForSample(const std::vector<int64_t>& beatTicks,
                                          const std::vector<Anchor>& anchors,
                                          int64_t samplePosition,
                                          int64_t totalSamples);

int64_t computeSamplePositionFromBeatPosition(const std::vector<int64_t>& beatTicks,
                                              double beatPosition,
                                              int64_t totalSamples);

int64_t computeSamplePositionFromWarpedBeatPosition(const std::vector<int64_t>& beatTicks,
                                                    const std::vector<Anchor>& anchors,
                                                    double targetBeatPosition,
                                                    int64_t totalSamples);

float computeWarpedBeatSpan(const std::vector<int64_t>& beatTicks,
                            const std::vector<SampleWarpMarker>& warpMarkers,
                            int64_t startSample,
                            int64_t endSample,
                            int64_t totalSamples);
} // namespace WarpGrid
