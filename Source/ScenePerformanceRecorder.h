#pragma once

#include "AudioEngine.h"

#include <juce_core/juce_core.h>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

enum class ScenePerformanceEventType
{
    Trigger = 0,
    ControlPoint
};

enum class ScenePerformanceControlTarget
{
    None = 0,
    Speed,
    Pitch,
    Pan,
    Volume,
    Swing,
    GrainSize,
    GrainDensity,
    GrainPitch,
    GrainJitter,
    GrainRandomDepth,
    GrainEnvelope,
    FilterFrequency,
    FilterResonance,
    FilterEnabled,
    FilterMorph,
    SliceLength,
    Scratch,
    DelayMix,
    DelayTime,
    DelayFeedback,
    DelayLowCut,
    DelayHighCut,
    DelayMode,
    DelaySyncEnabled,
    Retrigger,
    Rearrange,
    GrainPitchJitter,
    GrainSpread,
    GrainPositionJitter,
    GrainArp,
    GrainCloud,
    GrainEmitter,
    GrainShape
};

struct ScenePerformanceEvent
{
    int stripIndex = -1;
    int column = -1;
    int sampleSliceId = -1;
    int64_t sampleStartSample = -1;
    int controlMode = -1;
    int controlRow = -1;
    float value = 0.0f;
    double timeBeats = 0.0;
    bool isNoteOn = true;
    ScenePerformanceEventType type = ScenePerformanceEventType::Trigger;
    ScenePerformanceControlTarget controlTarget = ScenePerformanceControlTarget::None;

    bool operator<(const ScenePerformanceEvent& other) const { return timeBeats < other.timeBeats; }
};

struct ScenePerformanceMotionLaneState
{
    ModernAudioEngine::ModTarget target = ModernAudioEngine::ModTarget::None;
    bool bipolar = false;
    bool curveMode = false;
    float depth = 1.0f;
    float rate = 1.0f;
    ModernAudioEngine::ModTransportMode transportMode = ModernAudioEngine::ModTransportMode::Sync;
    int offset = 0;
    int lengthBars = 1;
    int editPage = 0;
    float smoothingMs = 0.0f;
    float curveBend = 0.0f;
    ModernAudioEngine::ModCurveShape curveShape = ModernAudioEngine::ModCurveShape::Linear;
    bool pitchScaleQuantize = false;
    ModernAudioEngine::PitchScale pitchScale = ModernAudioEngine::PitchScale::Chromatic;
    std::array<float, ModernAudioEngine::ModTotalSteps> steps{};
    std::array<int, ModernAudioEngine::ModTotalSteps> stepSubdivisions{};
    std::array<float, ModernAudioEngine::ModTotalSteps> stepEndValues{};
    std::array<ModernAudioEngine::ModCurveShape, ModernAudioEngine::ModTotalSteps> stepCurveShapes{};

    ScenePerformanceMotionLaneState()
    {
        stepSubdivisions.fill(1);
        stepEndValues.fill(0.0f);
        stepCurveShapes.fill(ModernAudioEngine::ModCurveShape::Linear);
    }
};

struct ScenePerformanceMotionStripState
{
    int activeSlot = 0;
    std::array<ScenePerformanceMotionLaneState, ModernAudioEngine::NumModSequencers> lanes{};
};

struct ScenePerformanceClip
{
    double lengthBeats = 4.0;
    uint32_t version = 2;
    std::vector<ScenePerformanceEvent> events;
    bool hasMotionState = false;
    std::array<ScenePerformanceMotionStripState, ModernAudioEngine::MaxStrips> motionStrips{};
};

class ScenePerformanceRecorder
{
public:
    static constexpr int MaxSceneSlots = 4;
    using MotionStripState = ScenePerformanceMotionStripState;

    ScenePerformanceRecorder();

    void startRecording(int sceneSlot, double lengthBeats, double currentBeat, double sceneStartBeat);
    void startOverdub(int sceneSlot, double lengthBeats, double currentBeat, double sceneStartBeat);
    void stopRecording();
    void extendRecording();
    bool updateRecording(double currentBeat);

    void clear(int sceneSlot);
    void clearAll();
    void copyClip(int sourceSceneSlot, int destSceneSlot);
    void setClipLengthBeats(int sceneSlot, double lengthBeats);
    bool replaceClipEvents(int sceneSlot, const std::vector<ScenePerformanceEvent>& events, double lengthBeats);
    void setMotionStripState(int sceneSlot, int stripIndex, const MotionStripState& state);
    MotionStripState getMotionStripState(int sceneSlot, int stripIndex) const;
    bool hasMotionState(int sceneSlot) const;

    void recordTriggerEvent(int sceneSlot,
                            int stripIndex,
                            int column,
                            bool noteOn,
                            double currentBeat,
                            int sampleSliceId,
                            int64_t sampleStartSample);
    bool cancelTriggerEvent(int sceneSlot,
                            int stripIndex,
                            int column,
                            bool noteOn,
                            double eventBeat);
    void recordControlEvent(int sceneSlot,
                            int stripIndex,
                            int controlMode,
                            int controlRow,
                            int column,
                            ScenePerformanceControlTarget controlTarget,
                            float value,
                            double currentBeat);

    template<typename Callback>
    void processEventsForBeatWindow(int sceneSlot,
                                    double sceneStartBeat,
                                    double fromBeat,
                                    double toBeat,
                                    Callback&& callback) const;

    std::vector<ScenePerformanceEvent> getEventsSnapshot(int sceneSlot) const;
    int getEventCount(int sceneSlot) const;
    double getClipLengthBeats(int sceneSlot) const;
    bool hasEvents(int sceneSlot) const;
    double getPlaybackProgressForBeat(int sceneSlot, double sceneStartBeat, double currentBeat) const;
    double getRecordingProgressForBeat(double currentBeat) const;

    bool isRecording() const { return recording.load(std::memory_order_acquire); }
    bool isOverdubbing() const { return recordingOverdub.load(std::memory_order_acquire); }
    int getRecordingSceneSlot() const { return recordingSceneSlot.load(std::memory_order_acquire); }
    double getRecordingStartBeat() const { return recordingStartBeat.load(std::memory_order_acquire); }
    double getRecordingEndBeat() const { return recordingEndBeat.load(std::memory_order_acquire); }
    double getRecordingSceneStartBeat() const { return recordingSceneStartBeat.load(std::memory_order_acquire); }

    juce::MemoryBlock createData(int sceneSlotOverride,
                                 const std::array<double, MaxSceneSlots>* lengthOverrides = nullptr) const;
    bool applyData(const juce::MemoryBlock& data, int sceneSlotOverride);

private:
    static constexpr int NumControlTargets = static_cast<int>(ScenePerformanceControlTarget::GrainShape) + 1;
    static constexpr int NumRecordedControlStrips = ModernAudioEngine::MaxStrips + 1;
    static double sanitizeLengthBeats(double lengthBeats);
    static int clampSceneSlot(int sceneSlot);
    static int clampStripIndex(int stripIndex);
    static int controlTargetToIndex(ScenePerformanceControlTarget controlTarget);
    static bool allowsGlobalStripIndex(ScenePerformanceControlTarget controlTarget);
    static int sanitizeEventStripIndex(int stripIndex, ScenePerformanceControlTarget controlTarget);
    static int recordedControlStateStripIndex(int stripIndex, ScenePerformanceControlTarget controlTarget);
    static double alignRecordingStartBeat(double currentBeat, double sceneStartBeat, double lengthBeats);
    static double wrapBeatToClip(double beat, double sceneStartBeat, double lengthBeats);
    void resetRecordingWriteState();
    ScenePerformanceClip& getClipForWrite(int sceneSlot);
    const ScenePerformanceClip& getClipForRead(int sceneSlot) const;

    std::array<ScenePerformanceClip, MaxSceneSlots> sceneClips;
    std::array<std::array<double, NumControlTargets>, NumRecordedControlStrips> lastRecordedControlEventBeats{};
    std::array<std::array<float, NumControlTargets>, NumRecordedControlStrips> lastRecordedControlEventValues{};
    mutable juce::CriticalSection clipLock;
    std::atomic<bool> recording{false};
    std::atomic<bool> recordingOverdub{false};
    std::atomic<int> recordingSceneSlot{-1};
    std::atomic<double> recordingSceneStartBeat{-1.0};
    std::atomic<double> recordingStartBeat{-1.0};
    std::atomic<double> recordingEndBeat{-1.0};
};

template<typename Callback>
void ScenePerformanceRecorder::processEventsForBeatWindow(int sceneSlot,
                                                          double sceneStartBeat,
                                                          double fromBeat,
                                                          double toBeat,
                                                          Callback&& callback) const
{
    if (!std::isfinite(sceneStartBeat) || !std::isfinite(fromBeat) || !std::isfinite(toBeat) || toBeat <= fromBeat)
        return;

    const double windowStart = juce::jmax(fromBeat, sceneStartBeat);
    if (toBeat <= windowStart)
        return;

    juce::ScopedLock lock(clipLock);
    const auto& clip = getClipForRead(sceneSlot);
    if (clip.events.empty())
        return;

    const double lengthBeats = sanitizeLengthBeats(clip.lengthBeats);
    const double relFrom = windowStart - sceneStartBeat;
    const double relTo = toBeat - sceneStartBeat;
    const int startCycle = static_cast<int>(std::floor(relFrom / lengthBeats));
    const int endCycle = static_cast<int>(std::floor((relTo - 1.0e-9) / lengthBeats));

    for (int cycle = startCycle; cycle <= endCycle; ++cycle)
    {
        const double cycleStart = static_cast<double>(cycle) * lengthBeats;
        double localFrom = relFrom - cycleStart;
        double localTo = relTo - cycleStart;

        if (cycle != startCycle)
            localFrom = 0.0;
        if (cycle != endCycle)
            localTo = lengthBeats;

        auto start = std::lower_bound(clip.events.begin(), clip.events.end(), localFrom,
                                      [](const ScenePerformanceEvent& event, double beat)
                                      {
                                          return event.timeBeats < beat;
                                      });

        for (auto it = start; it != clip.events.end() && it->timeBeats < localTo; ++it)
            callback(*it, sceneStartBeat + cycleStart + it->timeBeats);
    }
}
