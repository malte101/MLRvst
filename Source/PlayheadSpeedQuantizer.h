#pragma once

#include <array>
#include <algorithm>
#include <cmath>
#include <limits>

namespace PlayheadSpeedQuantizer
{
inline constexpr std::array<float, 16> kSpeedRatios {
    0.125f,     // 1/8
    0.1666667f, // 1/6
    0.25f,      // 1/4
    0.3333333f, // 1/3
    0.5f,       // 1/2
    0.6666667f, // 2/3
    0.75f,      // 3/4
    0.875f,     // 7/8
    1.0f,       // 1/1
    1.125f,     // 9/8
    1.25f,      // 5/4
    1.3333333f, // 4/3
    1.5f,       // 3/2
    2.0f,       // 2/1
    3.0f,       // 3/1
    4.0f        // 4/1
};

inline constexpr std::array<const char*, 16> kSpeedLabels {
    "1/8", "1/6", "1/4", "1/3", "1/2", "2/3", "3/4", "7/8",
    "1", "9/8", "5/4", "4/3", "3/2", "2", "3", "4"
};

inline int nearestSpeedIndex(float ratio)
{
    float best = std::numeric_limits<float>::max();
    int bestIndex = 0;
    for (int i = 0; i < static_cast<int>(kSpeedRatios.size()); ++i)
    {
        const float diff = std::abs(ratio - kSpeedRatios[static_cast<size_t>(i)]);
        if (diff < best)
        {
            best = diff;
            bestIndex = i;
        }
    }
    return bestIndex;
}

inline float ratioFromColumn(int column)
{
    const int clamped = std::clamp(column, 0, static_cast<int>(kSpeedRatios.size()) - 1);
    return kSpeedRatios[static_cast<size_t>(clamped)];
}

inline float quantizeRatio(float ratio)
{
    return kSpeedRatios[static_cast<size_t>(nearestSpeedIndex(ratio))];
}

inline const char* labelForRatio(float ratio)
{
    return kSpeedLabels[static_cast<size_t>(nearestSpeedIndex(ratio))];
}

inline float normalizeRecordingBars(int recordingBars)
{
    if (recordingBars <= 1)
        return 4.0f;
    if (recordingBars <= 2)
        return 8.0f;
    if (recordingBars <= 4)
        return 16.0f;
    return 32.0f;
}

inline float beatsPerLoopFromRatio(float ratio, int recordingBars)
{
    const float baseBeats = normalizeRecordingBars(recordingBars);
    const float clampedRatio = std::max(0.125f, ratio);
    return baseBeats / clampedRatio;
}

inline float ratioFromBeatsPerLoop(float beatsPerLoop, int recordingBars)
{
    const float baseBeats = normalizeRecordingBars(recordingBars);
    if (!(beatsPerLoop > 0.0f) || !std::isfinite(beatsPerLoop))
        return 1.0f;
    return baseBeats / beatsPerLoop;
}
}
