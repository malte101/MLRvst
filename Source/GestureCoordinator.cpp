/*
  ==============================================================================

    GestureCoordinator.cpp
    Gesture profile/combo state, preview, and persistence coordinator

  ==============================================================================
*/

#include "GestureCoordinator.h"

#include <cmath>

namespace
{
constexpr int kStutterButtonFirstColumn = 9;
constexpr int kStutterButtonCount = 7;

double wrapUnitPhase(double phase)
{
    if (!std::isfinite(phase))
        return 0.0;

    phase = std::fmod(phase, 1.0);
    if (phase < 0.0)
        phase += 1.0;
    return phase;
}

int gestureProfileStorageIndex(GestureProfileId profileId, int laneIndex, int stepIndex) noexcept
{
    const int profile = juce::jlimit(0, kGestureProfileCount - 1, static_cast<int>(profileId));
    const int lane = juce::jlimit(0, kGestureProfileLaneCount - 1, laneIndex);
    const int step = juce::jlimit(0, kGestureProfileSteps - 1, stepIndex);
    return ((profile * kGestureProfileLaneCount) + lane) * kGestureProfileSteps + step;
}

const char* gestureProfileIdKey(GestureProfileId profileId) noexcept
{
    switch (profileId)
    {
        case GestureProfileId::Stutter1: return "stutter1";
        case GestureProfileId::Stutter2: return "stutter2";
        case GestureProfileId::Stutter3: return "stutter3";
        case GestureProfileId::Scratch1: return "scratch1";
        case GestureProfileId::Scratch2: return "scratch2";
        case GestureProfileId::Scratch3: return "scratch3";
        case GestureProfileId::Count:
        default: return "stutter1";
    }
}

float sanitizeGestureProfileValue(float value) noexcept
{
    if (!std::isfinite(value))
        return 0.0f;
    return juce::jlimit(-1.0f, 1.0f, value);
}

bool isSingleButtonStutterSpeedArtifactLane(const std::array<float, kGestureProfileSteps>& lane) noexcept
{
    constexpr float epsilon = 1.0e-4f;
    if (std::abs(lane[0]) <= epsilon)
        return false;

    for (int step = 1; step < kGestureProfileSteps; ++step)
    {
        if (std::abs(lane[static_cast<size_t>(step)]) > epsilon)
            return false;
    }

    return true;
}

bool isSingleButtonStutterSpeedArtifactState(const GestureComboProfileState& state) noexcept
{
    if (!isSingleButtonStutterSpeedArtifactLane(
            state.lanes[static_cast<size_t>(StutterGestureLane::Speed)]))
    {
        return false;
    }

    constexpr float epsilon = 1.0e-4f;
    for (size_t lane = 0; lane < state.lanes.size(); ++lane)
    {
        if (lane == static_cast<size_t>(StutterGestureLane::Speed))
            continue;

        for (float value : state.lanes[lane])
        {
            if (std::abs(value) > epsilon)
                return false;
        }
    }

    return true;
}

GestureComboProfileState buildDefaultStutterComboPreviewState(int buttonCount, int comboIndex)
{
    GestureComboProfileState state;

    std::array<int, 3> columns{ -1, -1, -1 };
    if (!getGestureComboColumns(GestureComboKind::Stutter, buttonCount, comboIndex, columns))
        return state;

    int highestBit = 0;
    int lowestBit = kStutterButtonCount - 1;
    int sumBits = 0;
    uint8_t comboMask = 0;
    for (int i = 0; i < juce::jlimit(1, 3, buttonCount); ++i)
    {
        const int bit = juce::jlimit(
            0, kStutterButtonCount - 1, columns[static_cast<size_t>(i)] - kStutterButtonFirstColumn);
        highestBit = juce::jmax(highestBit, bit);
        lowestBit = juce::jmin(lowestBit, bit);
        sumBits += bit;
        comboMask |= static_cast<uint8_t>(1u << static_cast<unsigned int>(bit));
    }

    const int bitCount = juce::jlimit(1, 3, buttonCount);
    const int seed = (static_cast<int>(comboMask) * 97)
        + (bitCount * 19)
        + (highestBit * 11)
        + (lowestBit * 5);
    const int variant = seed % 8;
    const float centerBias = juce::jlimit(-1.0f, 1.0f,
                                          ((static_cast<float>(sumBits) / static_cast<float>(bitCount)) - 3.0f) / 3.0f);
    const float spreadBias = juce::jlimit(0.0f, 1.0f, static_cast<float>(highestBit - lowestBit) / 6.0f);
    const float intensity = juce::jlimit(
        0.28f, 1.0f, 0.34f + (0.18f * static_cast<float>(bitCount - 1)) + (0.22f * spreadBias));
    const float direction = ((seed & 1) == 0) ? 1.0f : -1.0f;
    const float divisionBase = juce::jlimit(-1.0f, 1.0f, (static_cast<float>(highestBit) - 3.0f) / 3.0f);

    static constexpr std::array<std::array<float, 8>, 8> kPreviewSpeedPatterns{{
        {{ 0.00f, 0.18f, 0.34f, 0.54f, 0.34f, 0.18f, 0.00f, -0.10f }},
        {{ 0.00f, -0.12f, 0.10f, 0.28f, 0.46f, 0.28f, 0.10f, -0.12f }},
        {{ 0.00f, 0.14f, 0.26f, 0.38f, 0.50f, 0.62f, 0.74f, 0.48f }},
        {{ 0.00f, 0.44f, 0.00f, 0.20f, 0.00f, 0.66f, 0.00f, 0.44f }},
        {{ 0.00f, 0.10f, 0.20f, 0.32f, 0.20f, 0.10f, 0.00f, -0.08f }},
        {{ 0.00f, -0.16f, 0.00f, 0.26f, 0.00f, 0.52f, 0.18f, 0.00f }},
        {{ 0.00f, 0.18f, 0.40f, 0.18f, -0.06f, 0.18f, 0.40f, 0.64f }},
        {{ 0.00f, 0.26f, 0.56f, 0.26f, 0.00f, -0.08f, 0.10f, 0.28f }}
    }};
    static constexpr std::array<std::array<float, 8>, 8> kPreviewPanPatterns{{
        {{ -1.00f, 1.00f, -0.80f, 0.80f, -0.60f, 0.60f, -0.35f, 0.35f }},
        {{ -0.70f, -0.30f, 0.30f, 0.70f, 1.00f, 0.70f, 0.30f, -0.30f }},
        {{ -1.00f, -0.60f, -0.20f, 0.20f, 0.60f, 1.00f, 0.40f, -0.20f }},
        {{ -1.00f, 1.00f, -1.00f, 1.00f, -0.50f, 0.50f, -0.20f, 0.20f }},
        {{ -0.25f, -0.75f, -1.00f, -0.50f, 0.50f, 1.00f, 0.75f, 0.25f }},
        {{ -0.90f, -0.20f, 0.90f, 0.20f, -0.90f, -0.20f, 0.90f, 0.20f }},
        {{ -0.40f, 0.40f, -0.70f, 0.70f, -1.00f, 1.00f, -0.60f, 0.60f }},
        {{ -1.00f, -0.50f, 0.00f, 0.50f, 1.00f, 0.50f, 0.00f, -0.50f }}
    }};
    static constexpr std::array<std::array<float, 8>, 8> kPreviewPitchPatterns{{
        {{ 0.0f, 0.2f, 0.48f, 0.66f, 0.88f, 0.66f, 0.48f, 0.2f }},
        {{ 0.0f, -0.22f, 0.18f, 0.40f, 0.68f, 0.40f, 0.18f, -0.22f }},
        {{ 0.0f, 0.25f, 0.58f, 0.82f, 1.00f, 0.82f, 0.58f, 0.25f }},
        {{ 0.0f, 0.40f, 0.0f, 0.58f, 0.0f, 0.84f, 0.0f, 1.00f }},
        {{ 0.0f, 0.18f, 0.34f, 0.58f, 0.74f, 0.58f, 0.34f, 0.18f }},
        {{ 0.0f, -0.28f, 0.0f, 0.30f, 0.60f, 0.30f, 0.0f, -0.28f }},
        {{ 0.0f, 0.08f, 0.44f, 0.68f, 1.00f, 0.68f, 0.44f, 0.08f }},
        {{ 0.0f, 0.32f, 0.58f, 0.92f, 0.58f, 0.32f, 0.14f, 0.0f }}
    }};

    for (int step = 0; step < kGestureProfileSteps; ++step)
    {
        const float phase = static_cast<float>(step) / static_cast<float>(juce::jmax(1, kGestureProfileSteps - 1));
        const float wrappedFastPhase = static_cast<float>(wrapUnitPhase(
            static_cast<double>(phase) * static_cast<double>(2 + ((seed >> 2) % 4))));
        const float wrappedPanPhase = static_cast<float>(wrapUnitPhase(
            static_cast<double>(phase) * static_cast<double>(1 + ((seed >> 4) % 3))));
        const float wrappedFilterPhase = static_cast<float>(wrapUnitPhase(
            static_cast<double>(phase) + static_cast<double>(0.18f * direction)
            + static_cast<double>(0.08f * centerBias)));
        const float sine = std::sin(juce::MathConstants<float>::twoPi * phase);
        const float sineFast = std::sin(juce::MathConstants<float>::twoPi * wrappedFastPhase);
        const float panSine = std::sin(juce::MathConstants<float>::twoPi * wrappedPanPhase);
        const float tri = 1.0f - std::abs((phase * 2.0f) - 1.0f);
        const float triSigned = (tri * 2.0f) - 1.0f;
        const float sawSigned = (phase * 2.0f) - 1.0f;
        const float filterTri = 1.0f - std::abs((wrappedFilterPhase * 2.0f) - 1.0f);
        const float pulse = (((step + seed) & 1) == 0) ? 1.0f : -1.0f;
        float speed = 0.0f;
        float pitch = 0.0f;
        float pan = 0.0f;
        float cutoff = 0.0f;
        float resonance = 0.0f;
        float morph = 0.0f;
        float division = divisionBase;
        float slice = 0.0f;

        if (bitCount == 1)
        {
            division = divisionBase;
        }
        else if (variant < 4)
        {
            switch (variant)
            {
                case 0:
                    speed = (-0.10f + (0.90f * phase) + (0.12f * sineFast)) * intensity;
                    pitch = (-0.12f + (0.86f * phase) + (0.14f * sineFast)) * intensity;
                    pan = 0.55f * panSine;
                    cutoff = -0.72f + (1.44f * phase);
                    resonance = (-0.10f + (0.55f * filterTri)) * intensity;
                    morph = (-0.55f + (1.10f * wrappedFilterPhase)) * (0.85f * intensity);
                    division = juce::jlimit(-1.0f, 1.0f, divisionBase + (0.18f * direction * sawSigned));
                    break;
                case 1:
                    speed = (0.80f - (0.95f * phase) + (0.10f * sine)) * intensity;
                    pitch = (0.72f - (1.05f * phase) + (0.08f * sine)) * intensity;
                    pan = 0.78f * triSigned;
                    cutoff = 0.84f - (1.40f * phase);
                    resonance = (0.08f + (0.52f * phase)) * intensity;
                    morph = (0.70f - (1.25f * wrappedFilterPhase)) * (0.82f * intensity);
                    division = juce::jlimit(-1.0f, 1.0f, divisionBase - (0.18f * direction * sawSigned));
                    break;
                case 2:
                    speed = 0.58f * std::sin(juce::MathConstants<float>::twoPi * phase * 2.0f) * intensity;
                    pitch = (0.62f * sine + 0.22f * sineFast) * intensity;
                    pan = 0.82f * std::sin(juce::MathConstants<float>::twoPi * (wrappedPanPhase * 2.0f));
                    cutoff = -0.18f + (1.02f * filterTri);
                    resonance = (-0.12f
                                  + (0.62f * static_cast<float>(wrapUnitPhase(wrappedFilterPhase * 2.0f))))
                        * intensity;
                    morph = 0.78f * std::sin(juce::MathConstants<float>::twoPi * wrappedFilterPhase) * intensity;
                    division = juce::jlimit(-1.0f, 1.0f, divisionBase + (0.20f * direction * triSigned));
                    break;
                case 3:
                default:
                    speed = (0.95f * triSigned + 0.10f * sineFast) * intensity;
                    pitch = (0.70f * sine + 0.36f * triSigned) * intensity;
                    pan = 0.88f * sawSigned;
                    cutoff = -0.64f + (1.28f * static_cast<float>(wrapUnitPhase(
                        static_cast<double>(phase) + (0.22 * juce::jmax(0.0, static_cast<double>(sine))))));
                    resonance = (-0.08f
                                  + (0.66f * static_cast<float>(wrapUnitPhase(
                                      static_cast<double>(wrappedFilterPhase)
                                      + (0.18 * static_cast<double>(triSigned)))))) * intensity;
                    morph = (-0.20f + (0.80f * phase)
                             + (0.36f * static_cast<float>(wrapUnitPhase(wrappedFilterPhase))))
                        * intensity;
                    division = juce::jlimit(-1.0f, 1.0f, divisionBase + (0.22f * direction * triSigned));
                    break;
            }

            if (bitCount >= 3)
            {
                speed *= 1.10f;
                pitch *= 1.08f;
                pan = juce::jlimit(-1.0f, 1.0f, pan * 1.08f);
                resonance = juce::jlimit(-1.0f, 1.0f, resonance + (0.10f * tri));
                morph = juce::jlimit(-1.0f, 1.0f, morph + (0.10f * direction * filterTri));
            }
        }
        else
        {
            const int patternBank = ((seed / 5) + (bitCount * 3) + highestBit + lowestBit) % 8;
            const auto& speedPattern = kPreviewSpeedPatterns[static_cast<size_t>((variant + patternBank) % 8)];
            const auto& panPattern = kPreviewPanPatterns[static_cast<size_t>((variant + highestBit + patternBank) % 8)];
            const auto& pitchPattern =
                kPreviewPitchPatterns[static_cast<size_t>((variant + lowestBit + (patternBank * 2)) % 8)];
            const int patternStep = juce::jlimit(0, 7, static_cast<int>(std::floor(phase * 8.0f)));
            const float stepNorm = static_cast<float>(patternStep) / 7.0f;

            speed = speedPattern[static_cast<size_t>(patternStep)] * (0.85f + (0.25f * intensity));
            pan = panPattern[static_cast<size_t>(patternStep)] * (0.62f + (0.28f * intensity));
            pitch = pitchPattern[static_cast<size_t>(patternStep)] * (0.70f + (0.24f * intensity));
            cutoff = -0.54f + (1.08f * stepNorm);
            resonance = (-0.10f + (0.78f * stepNorm)) * intensity;
            morph = (-0.76f + (1.52f * stepNorm)) * (0.86f * intensity);
            division = juce::jlimit(-1.0f, 1.0f, divisionBase + (0.28f * direction * pulse));
            if (bitCount >= 3)
            {
                speed = juce::jlimit(-1.0f, 1.0f, speed * 1.12f);
                pitch = juce::jlimit(-1.0f, 1.0f, pitch * 1.08f);
                division = juce::jlimit(-1.0f, 1.0f, division + (0.10f * sawSigned));
            }
        }

        if (bitCount >= 2)
        {
            const float sliceShape = (bitCount == 2)
                ? (direction > 0.0f ? phase : (1.0f - phase))
                : tri;
            slice = juce::jlimit(-1.0f, 0.0f, -sliceShape * (0.42f + (0.22f * spreadBias)));
        }

        state.lanes[static_cast<size_t>(StutterGestureLane::Speed)][static_cast<size_t>(step)] =
            juce::jlimit(-1.0f, 1.0f, speed);
        state.lanes[static_cast<size_t>(StutterGestureLane::Pitch)][static_cast<size_t>(step)] =
            juce::jlimit(-1.0f, 1.0f, pitch);
        state.lanes[static_cast<size_t>(StutterGestureLane::Pan)][static_cast<size_t>(step)] =
            juce::jlimit(-1.0f, 1.0f, pan);
        state.lanes[static_cast<size_t>(StutterGestureLane::Cutoff)][static_cast<size_t>(step)] =
            juce::jlimit(-1.0f, 1.0f, cutoff);
        state.lanes[static_cast<size_t>(StutterGestureLane::Resonance)][static_cast<size_t>(step)] =
            juce::jlimit(-1.0f, 1.0f, resonance);
        state.lanes[static_cast<size_t>(StutterGestureLane::Morph)][static_cast<size_t>(step)] =
            juce::jlimit(-1.0f, 1.0f, morph);
        state.lanes[static_cast<size_t>(StutterGestureLane::Division)][static_cast<size_t>(step)] =
            juce::jlimit(-1.0f, 1.0f, division);
        state.lanes[static_cast<size_t>(StutterGestureLane::Slice)][static_cast<size_t>(step)] =
            juce::jlimit(-1.0f, 1.0f, slice);
    }

    return state;
}

GestureComboProfileState buildDefaultScratchComboPreviewState(int buttonCount, int comboIndex)
{
    GestureComboProfileState state;

    std::array<int, 3> columns{ -1, -1, -1 };
    if (!getGestureComboColumns(GestureComboKind::Scratch, buttonCount, comboIndex, columns))
        return state;

    const int safeButtonCount = juce::jlimit(1, 3, buttonCount);
    int minColumn = ModernAudioEngine::MaxColumns - 1;
    int maxColumn = 0;
    int sumColumns = 0;
    int weightedColumns = 0;
    for (int i = 0; i < safeButtonCount; ++i)
    {
        const int column = juce::jlimit(0, ModernAudioEngine::MaxColumns - 1, columns[static_cast<size_t>(i)]);
        minColumn = juce::jmin(minColumn, column);
        maxColumn = juce::jmax(maxColumn, column);
        sumColumns += column;
        weightedColumns += column * (i + 1);
    }

    const float centerNorm = juce::jlimit(-1.0f, 1.0f,
                                          ((static_cast<float>(sumColumns) / static_cast<float>(safeButtonCount)) / 7.5f)
                                              - 1.0f);
    const float spanNorm = juce::jlimit(0.0f, 1.0f, static_cast<float>(maxColumn - minColumn) / 15.0f);
    const float densityBias = juce::jlimit(
        0.0f, 1.0f, 0.20f + (0.26f * static_cast<float>(safeButtonCount - 1)) + (0.34f * spanNorm));
    const float sizeBias =
        juce::jlimit(-1.0f, 1.0f, (0.62f * centerNorm) + (0.38f * (spanNorm - 0.5f)));
    const float spreadBias = juce::jlimit(-1.0f, 1.0f, (spanNorm * 2.0f) - 1.0f);
    const int seed = (comboIndex * 131)
        + (safeButtonCount * 41)
        + (sumColumns * 7)
        + (weightedColumns * 3)
        + maxColumn;
    const int variant = seed % 6;
    const float direction = ((seed & 1) == 0) ? 1.0f : -1.0f;

    for (int step = 0; step < kGestureProfileSteps; ++step)
    {
        const float phase = static_cast<float>(step) / static_cast<float>(juce::jmax(1, kGestureProfileSteps - 1));
        const float fastPhase = static_cast<float>(wrapUnitPhase(
            static_cast<double>(phase) * static_cast<double>(2 + ((seed >> 2) % 4))));
        const float slowPhase = static_cast<float>(wrapUnitPhase(
            static_cast<double>(phase) + static_cast<double>(0.17f * direction)
            + static_cast<double>(0.11f * centerNorm)));
        const float sine = std::sin(juce::MathConstants<float>::twoPi * phase);
        const float sineFast = std::sin(juce::MathConstants<float>::twoPi * fastPhase);
        const float tri = 1.0f - std::abs((phase * 2.0f) - 1.0f);
        const float triSigned = (tri * 2.0f) - 1.0f;
        const float sawSigned = (phase * 2.0f) - 1.0f;
        const float pulse = (((step + seed) & 1) == 0) ? 1.0f : -1.0f;
        const float windowTri = 1.0f - std::abs((slowPhase * 2.0f) - 1.0f);

        float motion = 0.0f;
        float motionEnd = 0.0f;
        float pitch = 0.0f;
        float sceneMix = 0.0f;
        float size = 0.0f;
        float density = 0.0f;
        float spread = 0.0f;
        float position = 0.0f;
        float window = 0.0f;
        float division = 0.0f;
        float duration = 0.0f;

        switch (variant)
        {
            case 0:
                motion = juce::jlimit(-1.0f, 1.0f, (-0.65f + (1.30f * phase) + (0.20f * sineFast))
                    * (0.78f + (0.18f * spanNorm)));
                motionEnd = juce::jlimit(-1.0f, 1.0f, (0.80f - (1.20f * phase)) * (0.72f + (0.12f * spanNorm)));
                pitch = juce::jlimit(-1.0f, 1.0f, (0.48f * phase * direction) + (0.22f * centerNorm));
                position = juce::jlimit(-1.0f, 1.0f, centerNorm + (0.34f * sawSigned));
                break;
            case 1:
                motion = juce::jlimit(-1.0f, 1.0f, 0.82f * sine);
                motionEnd = juce::jlimit(
                    -1.0f, 1.0f, 0.70f * std::sin(juce::MathConstants<float>::twoPi * (phase + 0.25f)));
                pitch = juce::jlimit(-1.0f, 1.0f, (0.44f * sineFast) + (0.16f * spreadBias));
                position = juce::jlimit(-1.0f, 1.0f,
                                        centerNorm
                                            + (0.42f * std::sin(juce::MathConstants<float>::twoPi * (slowPhase * 0.5f))));
                break;
            case 2:
                motion = juce::jlimit(-1.0f, 1.0f, direction * triSigned * (0.70f + (0.14f * spanNorm)));
                motionEnd =
                    juce::jlimit(-1.0f, 1.0f, -direction * triSigned * (0.58f + (0.16f * densityBias)));
                pitch = juce::jlimit(-1.0f, 1.0f, (0.38f * triSigned) + (0.24f * centerNorm));
                position = juce::jlimit(-1.0f, 1.0f, centerNorm + (0.48f * triSigned));
                break;
            case 3:
                motion = juce::jlimit(-1.0f, 1.0f, pulse * (0.56f + (0.18f * densityBias)));
                motionEnd = juce::jlimit(-1.0f, 1.0f, pulse * (-0.42f - (0.16f * spreadBias)));
                pitch = juce::jlimit(-1.0f, 1.0f, (0.34f * pulse) + (0.18f * centerNorm));
                position = juce::jlimit(-1.0f, 1.0f, centerNorm + (0.30f * pulse));
                break;
            case 4:
                motion = juce::jlimit(-1.0f, 1.0f, (0.76f * sawSigned) + (0.12f * sineFast));
                motionEnd = juce::jlimit(-1.0f, 1.0f, (-0.60f * sawSigned) + (0.10f * triSigned));
                pitch = juce::jlimit(-1.0f, 1.0f, (0.28f * sawSigned) + (0.26f * centerNorm));
                position = juce::jlimit(-1.0f, 1.0f, centerNorm + (0.26f * sawSigned));
                break;
            case 5:
            default:
                motion = juce::jlimit(-1.0f, 1.0f, (0.54f * sineFast) + (0.26f * triSigned));
                motionEnd = juce::jlimit(-1.0f, 1.0f, (-0.48f * sine) + (0.18f * triSigned));
                pitch = juce::jlimit(
                    -1.0f, 1.0f,
                    (0.34f * std::sin(juce::MathConstants<float>::twoPi * (phase * 0.5f + 0.13f)))
                        + (0.20f * spreadBias));
                position =
                    juce::jlimit(-1.0f, 1.0f, centerNorm + (0.36f * std::sin(juce::MathConstants<float>::twoPi * phase)));
                break;
        }

        sceneMix = juce::jlimit(
            -1.0f, 1.0f, (-0.18f + (0.34f * static_cast<float>(safeButtonCount - 1))) + (0.28f * tri));
        size = juce::jlimit(-1.0f, 1.0f, sizeBias + (0.26f * triSigned) - (0.12f * direction * centerNorm));
        density = juce::jlimit(-1.0f, 1.0f, (-0.24f + (0.66f * densityBias)) + (0.18f * sineFast));
        spread = juce::jlimit(-1.0f, 1.0f, spreadBias + (0.24f * sine));
        window = juce::jlimit(-1.0f, 1.0f, (-0.34f + (0.72f * spanNorm)) + (0.22f * windowTri));
        division = juce::jlimit(-1.0f, 1.0f, (-0.24f + (0.72f * densityBias)) + (0.18f * pulse));
        duration = juce::jlimit(-1.0f, 1.0f,
                                (-0.30f + (0.62f * static_cast<float>(safeButtonCount - 1) / 2.0f))
                                    + (0.20f * triSigned) - (0.12f * spanNorm));

        state.lanes[static_cast<size_t>(ScratchGestureLane::Motion)][static_cast<size_t>(step)] = motion;
        state.lanes[static_cast<size_t>(ScratchGestureLane::Pitch)][static_cast<size_t>(step)] = pitch;
        state.lanes[static_cast<size_t>(ScratchGestureLane::SceneMix)][static_cast<size_t>(step)] = sceneMix;
        state.lanes[static_cast<size_t>(ScratchGestureLane::Size)][static_cast<size_t>(step)] = size;
        state.lanes[static_cast<size_t>(ScratchGestureLane::Density)][static_cast<size_t>(step)] = density;
        state.lanes[static_cast<size_t>(ScratchGestureLane::Spread)][static_cast<size_t>(step)] = spread;
        state.lanes[static_cast<size_t>(ScratchGestureLane::Position)][static_cast<size_t>(step)] = position;
        state.lanes[static_cast<size_t>(ScratchGestureLane::Window)][static_cast<size_t>(step)] = window;
        state.lanes[static_cast<size_t>(ScratchGestureLane::Division)][static_cast<size_t>(step)] = division;
        state.lanes[static_cast<size_t>(ScratchGestureLane::Duration)][static_cast<size_t>(step)] = duration;
        state.lanes[static_cast<size_t>(ScratchGestureLane::MotionEnd)][static_cast<size_t>(step)] = motionEnd;
    }

    return state;
}
} // namespace

GestureCoordinator::GestureCoordinator(UserChangeCallback onUserChangeIn)
    : onUserChange(std::move(onUserChangeIn)),
      gestureComboProfileStore(std::make_shared<GestureComboProfileStore>())
{
    clearOverrideModes();
}

void GestureCoordinator::setUserChangeCallback(UserChangeCallback onUserChangeIn)
{
    onUserChange = std::move(onUserChangeIn);
}

void GestureCoordinator::attachAudioEngine(ModernAudioEngine* engine)
{
    audioEngine = engine;
    if (audioEngine != nullptr)
    {
        audioEngine->setGestureComboProfileStore(gestureComboProfileStore);
        syncToAudioEngine();
    }
}

void GestureCoordinator::syncToAudioEngine()
{
    if (audioEngine == nullptr)
        return;

    std::array<GestureProfileState, kGestureProfileCount> profiles;
    for (int profile = 0; profile < kGestureProfileCount; ++profile)
    {
        for (int lane = 0; lane < kGestureProfileLaneCount; ++lane)
        {
            for (int step = 0; step < kGestureProfileSteps; ++step)
            {
                profiles[static_cast<size_t>(profile)]
                    .lanes[static_cast<size_t>(lane)][static_cast<size_t>(step)] =
                    gestureProfileValues[static_cast<size_t>(gestureProfileStorageIndex(
                        static_cast<GestureProfileId>(profile), lane, step))].load(std::memory_order_acquire);
            }
        }
    }

    audioEngine->setGestureProfiles(profiles);
    audioEngine->setGestureComboProfileStore(gestureComboProfileStore);
}

GestureProfileState GestureCoordinator::getProfileState(GestureProfileId profileId) const
{
    GestureProfileState state;
    for (int lane = 0; lane < kGestureProfileLaneCount; ++lane)
    {
        for (int step = 0; step < kGestureProfileSteps; ++step)
        {
            state.lanes[static_cast<size_t>(lane)][static_cast<size_t>(step)] =
                gestureProfileValues[static_cast<size_t>(gestureProfileStorageIndex(profileId, lane, step))]
                    .load(std::memory_order_acquire);
        }
    }
    return state;
}

float GestureCoordinator::getProfileStepValue(GestureProfileId profileId, int laneIndex, int stepIndex) const
{
    return gestureProfileValues[static_cast<size_t>(gestureProfileStorageIndex(profileId, laneIndex, stepIndex))]
        .load(std::memory_order_acquire);
}

void GestureCoordinator::setProfileStepValue(GestureProfileId profileId,
                                             int laneIndex,
                                             int stepIndex,
                                             float value)
{
    gestureProfileValues[static_cast<size_t>(gestureProfileStorageIndex(profileId, laneIndex, stepIndex))]
        .store(sanitizeGestureProfileValue(value), std::memory_order_release);
    syncToAudioEngine();
    notifyUserChange();
}

void GestureCoordinator::resetProfile(GestureProfileId profileId)
{
    for (int lane = 0; lane < kGestureProfileLaneCount; ++lane)
    {
        for (int step = 0; step < kGestureProfileSteps; ++step)
        {
            gestureProfileValues[static_cast<size_t>(gestureProfileStorageIndex(profileId, lane, step))]
                .store(0.0f, std::memory_order_release);
        }
    }

    syncToAudioEngine();
    notifyUserChange();
}

void GestureCoordinator::resetAllProfiles()
{
    for (auto& value : gestureProfileValues)
        value.store(0.0f, std::memory_order_release);

    syncToAudioEngine();
    notifyUserChange();
}

GestureComboProfileState GestureCoordinator::getComboProfileState(GestureComboKind kind,
                                                                  int buttonCount,
                                                                  int comboIndex) const
{
    if (gestureComboProfileStore == nullptr)
        return {};

    const int flatIndex = getGestureComboFlatOffsetForButtonCount(kind, buttonCount)
        + juce::jlimit(0, juce::jmax(0, getGestureComboCountForButtonCount(kind, buttonCount) - 1), comboIndex);
    if (getOverrideMode(kind, flatIndex) != OverrideMode::Inherited)
        return gestureComboProfileStore->getState(kind, flatIndex);

    if (kind == GestureComboKind::Scratch)
        return buildDefaultScratchComboPreviewState(buttonCount, comboIndex);
    return buildDefaultStutterComboPreviewState(buttonCount, comboIndex);
}

float GestureCoordinator::getComboProfileStepValue(GestureComboKind kind,
                                                   int buttonCount,
                                                   int comboIndex,
                                                   int laneIndex,
                                                   int stepIndex) const
{
    const auto state = getComboProfileState(kind, buttonCount, comboIndex);
    const int safeLaneIndex = juce::jlimit(0, getGestureComboLaneCount(kind) - 1, laneIndex);
    const int safeStepIndex = juce::jlimit(0, kGestureProfileSteps - 1, stepIndex);
    return state.lanes[static_cast<size_t>(safeLaneIndex)][static_cast<size_t>(safeStepIndex)];
}

void GestureCoordinator::setComboProfileStepValue(GestureComboKind kind,
                                                  int buttonCount,
                                                  int comboIndex,
                                                  int laneIndex,
                                                  int stepIndex,
                                                  float value)
{
    if (gestureComboProfileStore == nullptr)
        return;

    const int flatIndex = getGestureComboFlatOffsetForButtonCount(kind, buttonCount)
        + juce::jlimit(0, juce::jmax(0, getGestureComboCountForButtonCount(kind, buttonCount) - 1), comboIndex);
    gestureComboProfileStore->setStepValue(kind, flatIndex, laneIndex, stepIndex, value);
    setOverrideMode(kind, flatIndex, OverrideMode::Exact);
    notifyUserChange();
}

void GestureCoordinator::materializeComboDisplayState(GestureComboKind kind, int buttonCount, int comboIndex)
{
    if (gestureComboProfileStore == nullptr)
        return;

    const int flatIndex = getGestureComboFlatOffsetForButtonCount(kind, buttonCount)
        + juce::jlimit(0, juce::jmax(0, getGestureComboCountForButtonCount(kind, buttonCount) - 1), comboIndex);

    const auto state = getComboProfileState(kind, buttonCount, comboIndex);
    const int laneCount = getGestureComboLaneCount(kind);
    for (int lane = 0; lane < laneCount; ++lane)
    {
        for (int step = 0; step < kGestureProfileSteps; ++step)
        {
            gestureComboProfileStore->setStepValue(kind,
                                                   flatIndex,
                                                   lane,
                                                   step,
                                                   state.lanes[static_cast<size_t>(lane)][static_cast<size_t>(step)]);
        }
    }
    setOverrideMode(kind, flatIndex, OverrideMode::Exact);
}

void GestureCoordinator::resetComboProfile(GestureComboKind kind, int buttonCount, int comboIndex)
{
    if (gestureComboProfileStore == nullptr)
        return;

    const int flatIndex = getGestureComboFlatOffsetForButtonCount(kind, buttonCount)
        + juce::jlimit(0, juce::jmax(0, getGestureComboCountForButtonCount(kind, buttonCount) - 1), comboIndex);
    gestureComboProfileStore->clearCombo(kind, flatIndex);
    setOverrideMode(kind, flatIndex, OverrideMode::Flat);
    notifyUserChange();
}

void GestureCoordinator::resetComboButtonCount(GestureComboKind kind, int buttonCount)
{
    if (gestureComboProfileStore == nullptr)
        return;

    const int safeCount = juce::jlimit(1, 3, buttonCount);
    const int flatOffset = getGestureComboFlatOffsetForButtonCount(kind, safeCount);
    const int comboCount = getGestureComboCountForButtonCount(kind, safeCount);
    for (int comboIndex = 0; comboIndex < comboCount; ++comboIndex)
    {
        const int flatIndex = flatOffset + comboIndex;
        gestureComboProfileStore->clearCombo(kind, flatIndex);
        setOverrideMode(kind, flatIndex, OverrideMode::Flat);
    }
    notifyUserChange();
}

void GestureCoordinator::resetAllComboProfiles()
{
    if (gestureComboProfileStore == nullptr)
        return;

    gestureComboProfileStore->clearAll();
    stutterOverrideModes.fill(OverrideMode::Flat);
    scratchOverrideModes.fill(OverrideMode::Flat);
    notifyUserChange();
}

float GestureCoordinator::sampleProfileLane(GestureProfileId profileId, int laneIndex, float phase) const
{
    const int safeLane = juce::jlimit(0, kGestureProfileLaneCount - 1, laneIndex);
    const float wrappedPhase = static_cast<float>(wrapUnitPhase(std::isfinite(phase) ? static_cast<double>(phase) : 0.0));
    const float scaled = wrappedPhase * static_cast<float>(kGestureProfileSteps);
    const int baseIndex = juce::jlimit(0, kGestureProfileSteps - 1, static_cast<int>(std::floor(scaled)));
    const float frac = juce::jlimit(0.0f, 1.0f, scaled - static_cast<float>(baseIndex));
    const int nextIndex = (baseIndex + 1) % kGestureProfileSteps;
    const float start = getProfileStepValue(profileId, safeLane, baseIndex);
    const float end = getProfileStepValue(profileId, safeLane, nextIndex);
    return sanitizeGestureProfileValue(juce::jmap(frac, start, end));
}

float GestureCoordinator::sampleComboLane(GestureComboKind kind, int comboFlatIndex, int laneIndex, float phase) const
{
    if (gestureComboProfileStore == nullptr)
        return 0.0f;
    return gestureComboProfileStore->sampleLane(kind, comboFlatIndex, laneIndex, phase);
}

bool GestureCoordinator::restoreProfilesFromXml(const juce::XmlElement& xml)
{
    for (auto& value : gestureProfileValues)
        value.store(0.0f, std::memory_order_release);

    auto parseProfileKey = [](const juce::String& key, GestureProfileId& outId) -> bool
    {
        const auto trimmed = key.trim().toLowerCase();
        if (trimmed == "stutter1") { outId = GestureProfileId::Stutter1; return true; }
        if (trimmed == "stutter2") { outId = GestureProfileId::Stutter2; return true; }
        if (trimmed == "stutter3") { outId = GestureProfileId::Stutter3; return true; }
        if (trimmed == "scratch1") { outId = GestureProfileId::Scratch1; return true; }
        if (trimmed == "scratch2") { outId = GestureProfileId::Scratch2; return true; }
        if (trimmed == "scratch3") { outId = GestureProfileId::Scratch3; return true; }
        return false;
    };

    auto* profilesXml = xml.getChildByName("GestureProfiles");
    if (profilesXml == nullptr)
        return false;

    bool anyRestored = false;
    for (auto* profileXml = profilesXml->getFirstChildElement();
         profileXml != nullptr;
         profileXml = profileXml->getNextElement())
    {
        if (!profileXml->hasTagName("Profile"))
            continue;

        GestureProfileId profileId = GestureProfileId::Stutter1;
        if (!parseProfileKey(profileXml->getStringAttribute("id"), profileId))
            continue;

        for (auto* laneXml = profileXml->getFirstChildElement();
             laneXml != nullptr;
             laneXml = laneXml->getNextElement())
        {
            if (!laneXml->hasTagName("Lane"))
                continue;

            const int laneIndex = juce::jlimit(0, kGestureProfileLaneCount - 1, laneXml->getIntAttribute("index", 0));
            juce::StringArray tokens;
            tokens.addTokens(laneXml->getStringAttribute("values"), ", ", "\"");
            tokens.trim();
            tokens.removeEmptyStrings();

            for (int step = 0; step < kGestureProfileSteps; ++step)
            {
                const float value = (step < tokens.size())
                    ? sanitizeGestureProfileValue(tokens[step].getFloatValue())
                    : 0.0f;
                gestureProfileValues[static_cast<size_t>(gestureProfileStorageIndex(profileId, laneIndex, step))]
                    .store(value, std::memory_order_release);
            }
            anyRestored = true;
        }
    }

    std::array<float, kGestureProfileSteps> singleButtonStutterSpeed{};
    for (int step = 0; step < kGestureProfileSteps; ++step)
    {
        singleButtonStutterSpeed[static_cast<size_t>(step)] =
            getProfileStepValue(GestureProfileId::Stutter1, 0, step);
    }

    if (isSingleButtonStutterSpeedArtifactLane(singleButtonStutterSpeed))
    {
        for (int step = 0; step < kGestureProfileSteps; ++step)
        {
            gestureProfileValues[static_cast<size_t>(gestureProfileStorageIndex(
                GestureProfileId::Stutter1, 0, step))].store(0.0f, std::memory_order_release);
        }
        anyRestored = true;
    }

    return anyRestored;
}

void GestureCoordinator::appendProfilesToXml(juce::XmlElement& xml) const
{
    if (auto* existing = xml.getChildByName("GestureProfiles"))
        xml.removeChildElement(existing, true);

    auto* profilesXml = xml.createNewChildElement("GestureProfiles");
    profilesXml->setAttribute("version", 1);

    for (int profile = 0; profile < kGestureProfileCount; ++profile)
    {
        const auto profileId = static_cast<GestureProfileId>(profile);
        auto* profileXml = profilesXml->createNewChildElement("Profile");
        profileXml->setAttribute("id", gestureProfileIdKey(profileId));

        for (int lane = 0; lane < kGestureProfileLaneCount; ++lane)
        {
            auto* laneXml = profileXml->createNewChildElement("Lane");
            laneXml->setAttribute("index", lane);

            juce::StringArray values;
            values.ensureStorageAllocated(kGestureProfileSteps);
            for (int step = 0; step < kGestureProfileSteps; ++step)
                values.add(juce::String(getProfileStepValue(profileId, lane, step), 4));

            laneXml->setAttribute("values", values.joinIntoString(","));
        }
    }
}

bool GestureCoordinator::restoreComboProfilesFromXml(const juce::XmlElement& xml)
{
    if (gestureComboProfileStore == nullptr)
        return false;

    clearOverrideModes();

    auto* combosXml = xml.getChildByName("GestureComboProfiles");
    if (combosXml == nullptr)
        return false;

    auto parseKind = [](const juce::String& kindText, GestureComboKind& outKind) -> bool
    {
        const auto trimmed = kindText.trim().toLowerCase();
        if (trimmed == "stutter")
        {
            outKind = GestureComboKind::Stutter;
            return true;
        }
        if (trimmed == "scratch")
        {
            outKind = GestureComboKind::Scratch;
            return true;
        }
        return false;
    };

    bool anyRestored = false;
    for (auto* comboXml = combosXml->getFirstChildElement();
         comboXml != nullptr;
         comboXml = comboXml->getNextElement())
    {
        if (!comboXml->hasTagName("Combo"))
            continue;

        GestureComboKind kind = GestureComboKind::Stutter;
        if (!parseKind(comboXml->getStringAttribute("kind"), kind))
            continue;

        const int buttonCount = juce::jlimit(1, 3, comboXml->getIntAttribute("buttons", 1));
        const int comboIndex = juce::jlimit(
            0,
            juce::jmax(0, getGestureComboCountForButtonCount(kind, buttonCount) - 1),
            comboXml->getIntAttribute("index", 0));
        const int flatIndex = getGestureComboFlatOffsetForButtonCount(kind, buttonCount) + comboIndex;
        const int laneCount = getGestureComboLaneCount(kind);
        const auto modeText = comboXml->getStringAttribute("mode").trim().toLowerCase();
        const bool isFlatOverride = (modeText == "flat");

        gestureComboProfileStore->clearCombo(kind, flatIndex);
        if (isFlatOverride)
        {
            setOverrideMode(kind, flatIndex, OverrideMode::Flat);
            anyRestored = true;
            continue;
        }

        bool restoredLaneData = false;
        for (auto* laneXml = comboXml->getFirstChildElement();
             laneXml != nullptr;
             laneXml = laneXml->getNextElement())
        {
            if (!laneXml->hasTagName("Lane"))
                continue;

            const int laneIndex = juce::jlimit(0, laneCount - 1, laneXml->getIntAttribute("index", 0));
            juce::StringArray tokens;
            tokens.addTokens(laneXml->getStringAttribute("values"), ", ", "\"");
            tokens.trim();
            tokens.removeEmptyStrings();

            for (int step = 0; step < kGestureProfileSteps; ++step)
            {
                const float value = (step < tokens.size())
                    ? sanitizeGestureProfileValue(tokens[step].getFloatValue())
                    : 0.0f;
                gestureComboProfileStore->setStepValue(kind, flatIndex, laneIndex, step, value);
            }
            restoredLaneData = true;
        }

        if (kind == GestureComboKind::Stutter && buttonCount == 1 && restoredLaneData)
        {
            const auto restoredState = gestureComboProfileStore->getState(kind, flatIndex);
            if (isSingleButtonStutterSpeedArtifactState(restoredState))
            {
                seedComboFromLegacy(kind, buttonCount, comboIndex);
                setOverrideMode(kind, flatIndex, OverrideMode::Inherited);
                anyRestored = true;
                continue;
            }
        }

        setOverrideMode(kind,
                        flatIndex,
                        restoredLaneData ? OverrideMode::Exact : OverrideMode::Flat);
        anyRestored = true;
    }

    return anyRestored;
}

void GestureCoordinator::appendComboProfilesToXml(juce::XmlElement& xml) const
{
    if (auto* existing = xml.getChildByName("GestureComboProfiles"))
        xml.removeChildElement(existing, true);

    if (gestureComboProfileStore == nullptr)
        return;

    auto* combosXml = xml.createNewChildElement("GestureComboProfiles");
    combosXml->setAttribute("version", 3);

    auto kindKey = [](GestureComboKind kind) -> const char*
    {
        return (kind == GestureComboKind::Stutter) ? "stutter" : "scratch";
    };

    for (GestureComboKind kind : { GestureComboKind::Stutter, GestureComboKind::Scratch })
    {
        const int laneCount = getGestureComboLaneCount(kind);
        for (int buttonCount = 1; buttonCount <= 3; ++buttonCount)
        {
            const int comboCount = getGestureComboCountForButtonCount(kind, buttonCount);
            const int flatOffset = getGestureComboFlatOffsetForButtonCount(kind, buttonCount);
            for (int comboIndex = 0; comboIndex < comboCount; ++comboIndex)
            {
                const int flatIndex = flatOffset + comboIndex;
                const auto mode = getOverrideMode(kind, flatIndex);
                if (mode == OverrideMode::Inherited)
                    continue;

                auto* comboXml = combosXml->createNewChildElement("Combo");
                comboXml->setAttribute("kind", kindKey(kind));
                comboXml->setAttribute("buttons", buttonCount);
                comboXml->setAttribute("index", comboIndex);
                comboXml->setAttribute("mode", mode == OverrideMode::Flat ? "flat" : "exact");

                if (mode == OverrideMode::Flat)
                    continue;

                for (int lane = 0; lane < laneCount; ++lane)
                {
                    auto* laneXml = comboXml->createNewChildElement("Lane");
                    laneXml->setAttribute("index", lane);

                    juce::StringArray values;
                    values.ensureStorageAllocated(kGestureProfileSteps);
                    for (int step = 0; step < kGestureProfileSteps; ++step)
                    {
                        values.add(juce::String(
                            gestureComboProfileStore->getStepValue(kind, flatIndex, lane, step), 4));
                    }
                    laneXml->setAttribute("values", values.joinIntoString(","));
                }
            }
        }
    }
}

void GestureCoordinator::expandLegacyProfilesToCombos()
{
    if (gestureComboProfileStore == nullptr)
        return;

    gestureComboProfileStore->clearAll();
    clearOverrideModes();

    for (GestureComboKind kind : { GestureComboKind::Stutter, GestureComboKind::Scratch })
    {
        for (int buttonCount = 1; buttonCount <= 3; ++buttonCount)
        {
            const int comboCount = getGestureComboCountForButtonCount(kind, buttonCount);
            for (int comboIndex = 0; comboIndex < comboCount; ++comboIndex)
                seedComboFromLegacy(kind, buttonCount, comboIndex);
        }
    }
}

std::shared_ptr<GestureComboProfileStore> GestureCoordinator::getComboProfileStore() const noexcept
{
    return gestureComboProfileStore;
}

GestureCoordinator::OverrideMode GestureCoordinator::getOverrideMode(GestureComboKind kind, int comboFlatIndex) const noexcept
{
    const int safeFlatIndex = juce::jlimit(0, getGestureComboTotalCount(kind) - 1, comboFlatIndex);
    if (kind == GestureComboKind::Stutter)
        return stutterOverrideModes[static_cast<size_t>(safeFlatIndex)];
    return scratchOverrideModes[static_cast<size_t>(safeFlatIndex)];
}

void GestureCoordinator::setOverrideMode(GestureComboKind kind,
                                         int comboFlatIndex,
                                         OverrideMode mode) noexcept
{
    const int safeFlatIndex = juce::jlimit(0, getGestureComboTotalCount(kind) - 1, comboFlatIndex);
    if (kind == GestureComboKind::Stutter)
    {
        stutterOverrideModes[static_cast<size_t>(safeFlatIndex)] = mode;
        return;
    }

    scratchOverrideModes[static_cast<size_t>(safeFlatIndex)] = mode;
}

void GestureCoordinator::clearOverrideModes() noexcept
{
    stutterOverrideModes.fill(OverrideMode::Inherited);
    scratchOverrideModes.fill(OverrideMode::Inherited);
}

void GestureCoordinator::notifyUserChange()
{
    if (onUserChange)
        onUserChange();
}

void GestureCoordinator::seedComboFromLegacy(GestureComboKind kind, int buttonCount, int comboIndex)
{
    if (gestureComboProfileStore == nullptr)
        return;

    const int safeButtonCount = juce::jlimit(1, 3, buttonCount);
    const int safeComboIndex = juce::jlimit(
        0,
        juce::jmax(0, getGestureComboCountForButtonCount(kind, safeButtonCount) - 1),
        comboIndex);
    const GestureProfileId legacyId = (kind == GestureComboKind::Stutter)
        ? (safeButtonCount == 1 ? GestureProfileId::Stutter1
                                : (safeButtonCount == 2 ? GestureProfileId::Stutter2 : GestureProfileId::Stutter3))
        : (safeButtonCount == 1 ? GestureProfileId::Scratch1
                                : (safeButtonCount == 2 ? GestureProfileId::Scratch2 : GestureProfileId::Scratch3));
    const int flatIndex = getGestureComboFlatOffsetForButtonCount(kind, safeButtonCount) + safeComboIndex;

    gestureComboProfileStore->clearCombo(kind, flatIndex);

    for (int step = 0; step < kGestureProfileSteps; ++step)
    {
        if (kind == GestureComboKind::Stutter)
        {
            const float speed = (safeButtonCount == 1)
                ? 0.0f
                : getProfileStepValue(legacyId, 0, step);
            const float pitch = getProfileStepValue(legacyId, 1, step);
            const float pan = getProfileStepValue(legacyId, 2, step);
            const float filter = getProfileStepValue(legacyId, 3, step);
            const float division = getProfileStepValue(legacyId, 4, step);
            gestureComboProfileStore->setStepValue(kind, flatIndex, static_cast<int>(StutterGestureLane::Speed), step, speed);
            gestureComboProfileStore->setStepValue(kind, flatIndex, static_cast<int>(StutterGestureLane::Pitch), step, pitch);
            gestureComboProfileStore->setStepValue(kind, flatIndex, static_cast<int>(StutterGestureLane::Pan), step, pan);
            gestureComboProfileStore->setStepValue(kind, flatIndex, static_cast<int>(StutterGestureLane::Cutoff), step, filter);
            gestureComboProfileStore->setStepValue(kind, flatIndex, static_cast<int>(StutterGestureLane::Resonance), step, filter);
            gestureComboProfileStore->setStepValue(kind, flatIndex, static_cast<int>(StutterGestureLane::Morph), step, filter);
            gestureComboProfileStore->setStepValue(kind, flatIndex, static_cast<int>(StutterGestureLane::Division), step, division);
        }
        else
        {
            const float motion = getProfileStepValue(legacyId, 0, step);
            const float pitch = getProfileStepValue(legacyId, 1, step);
            const float size = getProfileStepValue(legacyId, 2, step);
            const float density = getProfileStepValue(legacyId, 3, step);
            const float spread = getProfileStepValue(legacyId, 4, step);
            gestureComboProfileStore->setStepValue(kind, flatIndex, static_cast<int>(ScratchGestureLane::Motion), step, motion);
            gestureComboProfileStore->setStepValue(kind, flatIndex, static_cast<int>(ScratchGestureLane::Pitch), step, pitch);
            gestureComboProfileStore->setStepValue(kind, flatIndex, static_cast<int>(ScratchGestureLane::SceneMix), step, motion);
            gestureComboProfileStore->setStepValue(kind, flatIndex, static_cast<int>(ScratchGestureLane::Size), step, size);
            gestureComboProfileStore->setStepValue(kind, flatIndex, static_cast<int>(ScratchGestureLane::Density), step, density);
            gestureComboProfileStore->setStepValue(kind, flatIndex, static_cast<int>(ScratchGestureLane::Spread), step, spread);
        }
    }
}
