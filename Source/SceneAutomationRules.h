#pragma once

#include "PluginProcessor.h"

#include <vector>

namespace SceneAutomationRules
{
float normalizeValue(const ScenePerformanceEvent& event);
float normalizeValue(ScenePerformanceControlTarget target, float value);
float denormalizeValue(const ScenePerformanceEvent& event, float normalizedValue);
float defaultValue(ScenePerformanceControlTarget target) noexcept;
float valueThinThreshold(ScenePerformanceControlTarget target) noexcept;

MlrVSTAudioProcessor::ControlMode controlModeForTarget(ScenePerformanceControlTarget target) noexcept;
int controlRowForTarget(ScenePerformanceControlTarget target) noexcept;
bool targetUsesGlobalStrip(ScenePerformanceControlTarget target) noexcept;
bool targetUsesSteppedSegments(ScenePerformanceControlTarget target) noexcept;
bool eventUsesSteppedSegments(const ScenePerformanceEvent& event) noexcept;
bool targetUsesStripRuntimeState(ScenePerformanceControlTarget target) noexcept;

int resolvedStripIndexForTarget(int stripIndex, ScenePerformanceControlTarget target) noexcept;
int columnFromNormalizedValue(float normalizedValue) noexcept;
int controlTargetIndex(ScenePerformanceControlTarget target) noexcept;
int automationMaskIndex(int stripIndex, ScenePerformanceControlTarget target) noexcept;
uint64_t automationTargetBit(ScenePerformanceControlTarget target) noexcept;
double wrapBeatIntoClip(double currentBeat, double sceneStartBeat, double lengthBeats) noexcept;
bool findHeldEventAtClipBeat(const std::vector<ScenePerformanceEvent>& events,
                             int stripIndex,
                             ScenePerformanceControlTarget target,
                             double clipBeat,
                             ScenePerformanceEvent& outEvent);

ScenePerformanceEvent makeControlPointEvent(int stripIndex,
                                            ScenePerformanceControlTarget target,
                                            double timeBeats,
                                            float normalizedValue,
                                            bool drawStepped = false);

struct PointWriteOptions
{
    int stripIndex = 0;
    ScenePerformanceControlTarget target = ScenePerformanceControlTarget::None;
    double timeBeats = 0.0;
    float normalizedValue = 0.0f;
    double lengthBeats = 1.0;
    int gridDivision = 16;
    bool gridEnabled = false;
    bool drawStepped = false;
};

struct PointWriteResult
{
    bool changed = false;
    ScenePerformanceEvent event;
};

double snapBeatToGrid(double timeBeats, double lengthBeats, int gridDivision, bool enabled);
double pointMatchEpsilon(int gridDivision, bool gridEnabled);
PointWriteResult writeControlPoint(std::vector<ScenePerformanceEvent>& events, const PointWriteOptions& options);
} // namespace SceneAutomationRules
