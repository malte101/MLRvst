#include "ScenePerformanceRecorder.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

namespace
{
constexpr double kDefaultSceneClipLengthBeats = 4.0;
constexpr double kMaxSceneClipLengthBeats = 4096.0;
constexpr int kScenePerformanceDataMagic = 0x53435031; // SCP1
constexpr int kScenePerformanceDataVersion = 4;
constexpr double kSceneEventReplaceEpsilonBeats = 1.0 / 1024.0;
constexpr double kSceneControlThinBeatWindowBeats = 1.0 / 64.0;
constexpr float kSceneContinuousControlThinValueThreshold = 0.015f;
constexpr float kSceneDiscreteControlThinValueThreshold = 1.0e-4f;

void sortClipEvents(ScenePerformanceClip& clip)
{
    std::sort(clip.events.begin(), clip.events.end());
}

template <typename Predicate>
void eraseEventsIf(ScenePerformanceClip& clip, Predicate&& predicate)
{
    clip.events.erase(std::remove_if(clip.events.begin(),
                                     clip.events.end(),
                                     std::forward<Predicate>(predicate)),
                      clip.events.end());
}

void insertEventSorted(ScenePerformanceClip& clip, const ScenePerformanceEvent& event)
{
    const auto insertPos = std::upper_bound(clip.events.begin(),
                                            clip.events.end(),
                                            event.timeBeats,
                                            [](double beat, const ScenePerformanceEvent& existing)
                                            {
                                                return beat < existing.timeBeats;
                                            });
    clip.events.insert(insertPos, event);
}

float normalizeControlValueForThinning(ScenePerformanceControlTarget target, float value)
{
    switch (target)
    {
        case ScenePerformanceControlTarget::Speed:
        {
            const float safeValue = juce::jlimit(0.125f, 8.0f, value);
            return juce::jlimit(0.0f, 1.0f, (std::log2(safeValue) + 3.0f) / 6.0f);
        }
        case ScenePerformanceControlTarget::Pitch:
            return juce::jlimit(0.0f, 1.0f, (value + 24.0f) / 48.0f);
        case ScenePerformanceControlTarget::GrainPitch:
            return juce::jlimit(0.0f, 1.0f, (value + 48.0f) / 96.0f);
        case ScenePerformanceControlTarget::Pan:
        case ScenePerformanceControlTarget::GrainShape:
            return juce::jlimit(0.0f, 1.0f, (value + 1.0f) * 0.5f);
        case ScenePerformanceControlTarget::Volume:
        case ScenePerformanceControlTarget::Swing:
        case ScenePerformanceControlTarget::GrainSpread:
        case ScenePerformanceControlTarget::GrainJitter:
        case ScenePerformanceControlTarget::GrainPositionJitter:
        case ScenePerformanceControlTarget::GrainRandomDepth:
        case ScenePerformanceControlTarget::GrainArp:
        case ScenePerformanceControlTarget::GrainCloud:
        case ScenePerformanceControlTarget::GrainEmitter:
        case ScenePerformanceControlTarget::GrainEnvelope:
        case ScenePerformanceControlTarget::DelayMix:
        case ScenePerformanceControlTarget::FilterMorph:
        case ScenePerformanceControlTarget::FilterEnabled:
        case ScenePerformanceControlTarget::Retrigger:
        case ScenePerformanceControlTarget::Rearrange:
        case ScenePerformanceControlTarget::DelaySyncEnabled:
            return juce::jlimit(0.0f, 1.0f, value);
        case ScenePerformanceControlTarget::SliceLength:
            return juce::jlimit(0.0f, 1.0f, (value - 0.02f) / 0.98f);
        case ScenePerformanceControlTarget::Scratch:
            return juce::jlimit(0.0f, 1.0f, value / 100.0f);
        case ScenePerformanceControlTarget::GrainSize:
            return juce::jlimit(0.0f, 1.0f, (value - 5.0f) / (2400.0f - 5.0f));
        case ScenePerformanceControlTarget::GrainDensity:
            return juce::jlimit(0.0f, 1.0f, (value - 0.05f) / (0.9f - 0.05f));
        case ScenePerformanceControlTarget::GrainPitchJitter:
            return juce::jlimit(0.0f, 1.0f, value / 48.0f);
        case ScenePerformanceControlTarget::FilterFrequency:
        {
            const float safeValue = juce::jlimit(20.0f, 20000.0f, value);
            return juce::jlimit(0.0f, 1.0f, std::log(safeValue / 20.0f) / std::log(1000.0f));
        }
        case ScenePerformanceControlTarget::FilterResonance:
            return juce::jlimit(0.0f, 1.0f, (value - 0.1f) / 9.9f);
        case ScenePerformanceControlTarget::DelayTime:
            return juce::jlimit(0.0f, 1.0f, (value - 0.25f) / (4.0f - 0.25f));
        case ScenePerformanceControlTarget::DelayFeedback:
            return juce::jlimit(0.0f, 1.0f, value / 0.97f);
        case ScenePerformanceControlTarget::DelayLowCut:
        {
            const juce::NormalisableRange<float> range(20.0f, 12000.0f, 1.0f, 0.25f);
            return juce::jlimit(0.0f, 1.0f, range.convertTo0to1(value));
        }
        case ScenePerformanceControlTarget::DelayHighCut:
        {
            const juce::NormalisableRange<float> range(200.0f, 20000.0f, 1.0f, 0.3f);
            return juce::jlimit(0.0f, 1.0f, range.convertTo0to1(value));
        }
        case ScenePerformanceControlTarget::DelayMode:
            return juce::jlimit(0.0f, 1.0f, value / 2.0f);
        case ScenePerformanceControlTarget::None:
        default:
            break;
    }

    return 0.5f;
}

float controlValueThinThreshold(ScenePerformanceControlTarget target)
{
    switch (target)
    {
        case ScenePerformanceControlTarget::Speed:
        case ScenePerformanceControlTarget::Pitch:
        case ScenePerformanceControlTarget::Pan:
        case ScenePerformanceControlTarget::Volume:
        case ScenePerformanceControlTarget::Swing:
        case ScenePerformanceControlTarget::GrainSize:
        case ScenePerformanceControlTarget::GrainDensity:
        case ScenePerformanceControlTarget::GrainPitch:
        case ScenePerformanceControlTarget::GrainJitter:
        case ScenePerformanceControlTarget::GrainRandomDepth:
        case ScenePerformanceControlTarget::GrainEnvelope:
        case ScenePerformanceControlTarget::FilterFrequency:
        case ScenePerformanceControlTarget::FilterResonance:
        case ScenePerformanceControlTarget::FilterMorph:
        case ScenePerformanceControlTarget::SliceLength:
        case ScenePerformanceControlTarget::Scratch:
        case ScenePerformanceControlTarget::DelayMix:
        case ScenePerformanceControlTarget::DelayTime:
        case ScenePerformanceControlTarget::DelayFeedback:
        case ScenePerformanceControlTarget::DelayLowCut:
        case ScenePerformanceControlTarget::DelayHighCut:
        case ScenePerformanceControlTarget::Retrigger:
        case ScenePerformanceControlTarget::Rearrange:
        case ScenePerformanceControlTarget::GrainPitchJitter:
        case ScenePerformanceControlTarget::GrainSpread:
        case ScenePerformanceControlTarget::GrainPositionJitter:
        case ScenePerformanceControlTarget::GrainArp:
        case ScenePerformanceControlTarget::GrainCloud:
        case ScenePerformanceControlTarget::GrainEmitter:
        case ScenePerformanceControlTarget::GrainShape:
            return kSceneContinuousControlThinValueThreshold;
        case ScenePerformanceControlTarget::FilterEnabled:
        case ScenePerformanceControlTarget::DelayMode:
        case ScenePerformanceControlTarget::DelaySyncEnabled:
            return kSceneDiscreteControlThinValueThreshold;
        case ScenePerformanceControlTarget::None:
            return 0.0f;
    }

    return kSceneContinuousControlThinValueThreshold;
}

double wrappedForwardBeatDistance(double previousBeat, double currentBeat, double clipLengthBeats)
{
    const double safeLength = juce::jmax(kDefaultSceneClipLengthBeats, clipLengthBeats);
    double delta = currentBeat - previousBeat;
    if (delta < 0.0)
        delta += safeLength;
    return delta;
}

void resetClip(ScenePerformanceClip& clip)
{
    clip.lengthBeats = kDefaultSceneClipLengthBeats;
    clip.version = kScenePerformanceDataVersion;
    clip.events.clear();
    clip.hasMotionState = false;
    clip.motionStrips = {};
}

ScenePerformanceMotionLaneState sanitizeMotionLane(const ScenePerformanceMotionLaneState& lane)
{
    ScenePerformanceMotionLaneState sanitized = lane;
    sanitized.target = sanitizeModPerformanceTarget(static_cast<ModernAudioEngine::ModTarget>(lane.target));
    sanitized.depth = std::isfinite(lane.depth) ? juce::jlimit(0.0f, 1.0f, lane.depth) : 1.0f;
    sanitized.rate = std::isfinite(lane.rate) ? juce::jmax(0.125f, lane.rate) : 1.0f;
    sanitized.transportMode = static_cast<ModernAudioEngine::ModTransportMode>(juce::jlimit(
        0,
        static_cast<int>(ModernAudioEngine::ModTransportMode::Scene),
        static_cast<int>(lane.transportMode)));
    sanitized.offset = juce::jlimit(-(ModernAudioEngine::ModTotalSteps - 1),
                                    ModernAudioEngine::ModTotalSteps - 1,
                                    lane.offset);
    sanitized.lengthBars = juce::jlimit(1, ModernAudioEngine::MaxModBars, lane.lengthBars);
    sanitized.editPage = juce::jlimit(0, sanitized.lengthBars - 1, lane.editPage);
    sanitized.smoothingMs = std::isfinite(lane.smoothingMs) ? juce::jlimit(0.0f, 250.0f, lane.smoothingMs) : 0.0f;
    sanitized.curveBend = std::isfinite(lane.curveBend) ? juce::jlimit(-1.0f, 1.0f, lane.curveBend) : 0.0f;
    sanitized.curveShape = static_cast<ModernAudioEngine::ModCurveShape>(juce::jlimit(
        0,
        static_cast<int>(ModernAudioEngine::ModCurveShape::Square),
        static_cast<int>(lane.curveShape)));
    sanitized.pitchScale = static_cast<ModernAudioEngine::PitchScale>(juce::jlimit(
        0,
        static_cast<int>(ModernAudioEngine::PitchScale::PentatonicMinor),
        static_cast<int>(lane.pitchScale)));

    for (int stepIndex = 0; stepIndex < ModernAudioEngine::ModTotalSteps; ++stepIndex)
    {
        const auto index = static_cast<size_t>(stepIndex);
        sanitized.steps[index] = std::isfinite(lane.steps[index])
            ? juce::jlimit(0.0f, 1.0f, lane.steps[index])
            : 0.0f;
        sanitized.stepSubdivisions[index] = juce::jlimit(1,
                                                         ModernAudioEngine::ModMaxStepSubdivisions,
                                                         lane.stepSubdivisions[index]);
        sanitized.stepEndValues[index] = std::isfinite(lane.stepEndValues[index])
            ? juce::jlimit(0.0f, 1.0f, lane.stepEndValues[index])
            : sanitized.steps[index];
        if (sanitized.stepSubdivisions[index] <= 1)
            sanitized.stepEndValues[index] = sanitized.steps[index];
        sanitized.stepCurveShapes[index] = static_cast<ModernAudioEngine::ModCurveShape>(juce::jlimit(
            0,
            static_cast<int>(ModernAudioEngine::ModCurveShape::Square),
            static_cast<int>(lane.stepCurveShapes[index])));
    }

    return sanitized;
}

ScenePerformanceMotionStripState sanitizeMotionStripState(const ScenePerformanceMotionStripState& state)
{
    ScenePerformanceMotionStripState sanitized = state;
    sanitized.activeSlot = juce::jlimit(0, ModernAudioEngine::NumModSequencers - 1, state.activeSlot);
    for (int slot = 0; slot < ModernAudioEngine::NumModSequencers; ++slot)
        sanitized.lanes[static_cast<size_t>(slot)] = sanitizeMotionLane(state.lanes[static_cast<size_t>(slot)]);
    return sanitized;
}

void writeMotionLaneState(juce::MemoryOutputStream& out, const ScenePerformanceMotionLaneState& lane)
{
    out.writeInt(static_cast<int>(lane.target));
    out.writeByte(static_cast<char>(lane.bipolar ? 1 : 0));
    out.writeByte(static_cast<char>(lane.curveMode ? 1 : 0));
    out.writeFloat(lane.depth);
    out.writeFloat(lane.rate);
    out.writeInt(static_cast<int>(lane.transportMode));
    out.writeInt(lane.offset);
    out.writeInt(lane.lengthBars);
    out.writeInt(lane.editPage);
    out.writeFloat(lane.smoothingMs);
    out.writeFloat(lane.curveBend);
    out.writeInt(static_cast<int>(lane.curveShape));
    out.writeByte(static_cast<char>(lane.pitchScaleQuantize ? 1 : 0));
    out.writeInt(static_cast<int>(lane.pitchScale));

    for (int stepIndex = 0; stepIndex < ModernAudioEngine::ModTotalSteps; ++stepIndex)
    {
        const auto index = static_cast<size_t>(stepIndex);
        out.writeFloat(lane.steps[index]);
        out.writeInt(lane.stepSubdivisions[index]);
        out.writeFloat(lane.stepEndValues[index]);
        out.writeInt(static_cast<int>(lane.stepCurveShapes[index]));
    }
}

void writeClip(juce::MemoryOutputStream& out,
               int sceneSlot,
               const ScenePerformanceClip& clip,
               double lengthBeatsOverride)
{
    const double serializedLength = std::isfinite(lengthBeatsOverride)
        ? juce::jlimit(0.25, kMaxSceneClipLengthBeats, lengthBeatsOverride)
        : kDefaultSceneClipLengthBeats;
    out.writeInt(sceneSlot);
    out.writeDouble(serializedLength);
    out.writeInt(kScenePerformanceDataVersion);
    out.writeInt(static_cast<int>(clip.events.size()));
    for (const auto& event : clip.events)
    {
        out.writeByte(static_cast<char>(event.type));
        out.writeInt(event.stripIndex);
        out.writeInt(event.column);
        out.writeInt(event.sampleSliceId);
        out.writeInt64(event.sampleStartSample);
        out.writeInt(event.controlMode);
        out.writeInt(event.controlRow);
        out.writeByte(static_cast<char>(event.controlTarget));
        out.writeFloat(event.value);
        out.writeDouble(event.timeBeats);
        out.writeByte(static_cast<char>(event.isNoteOn ? 1 : 0));
        out.writeByte(static_cast<char>(event.drawStepped ? 1 : 0));
        out.writeByte(static_cast<char>(event.scratchGesture ? 1 : 0));
    }

    out.writeByte(static_cast<char>(clip.hasMotionState ? 1 : 0));
    if (!clip.hasMotionState)
        return;

    for (int stripIndex = 0; stripIndex < ModernAudioEngine::MaxStrips; ++stripIndex)
    {
        const auto& stripState = clip.motionStrips[static_cast<size_t>(stripIndex)];
        out.writeInt(stripState.activeSlot);
        for (const auto& lane : stripState.lanes)
            writeMotionLaneState(out, lane);
    }
}

}

ScenePerformanceRecorder::ScenePerformanceRecorder()
{
    resetRecordingWriteState();
    clearAll();
}

double ScenePerformanceRecorder::sanitizeLengthBeats(double lengthBeats)
{
    if (!std::isfinite(lengthBeats))
        return kDefaultSceneClipLengthBeats;

    return juce::jlimit(kDefaultSceneClipLengthBeats, kMaxSceneClipLengthBeats, lengthBeats);
}

int ScenePerformanceRecorder::clampSceneSlot(int sceneSlot)
{
    return juce::jlimit(0, MaxSceneSlots - 1, sceneSlot);
}

int ScenePerformanceRecorder::clampStripIndex(int stripIndex)
{
    return juce::jlimit(0, ModernAudioEngine::MaxStrips - 1, stripIndex);
}

bool ScenePerformanceRecorder::allowsGlobalStripIndex(ScenePerformanceControlTarget controlTarget)
{
    return controlTarget == ScenePerformanceControlTarget::Retrigger;
}

int ScenePerformanceRecorder::sanitizeEventStripIndex(int stripIndex,
                                                      ScenePerformanceControlTarget controlTarget)
{
    if (allowsGlobalStripIndex(controlTarget) && stripIndex < 0)
        return -1;

    return clampStripIndex(stripIndex);
}

int ScenePerformanceRecorder::controlTargetToIndex(ScenePerformanceControlTarget controlTarget)
{
    return juce::jlimit(0,
                        NumControlTargets - 1,
                        static_cast<int>(controlTarget));
}

int ScenePerformanceRecorder::recordedControlStateStripIndex(int stripIndex,
                                                             ScenePerformanceControlTarget controlTarget)
{
    if (allowsGlobalStripIndex(controlTarget) && stripIndex < 0)
        return ModernAudioEngine::MaxStrips;

    return clampStripIndex(stripIndex);
}

void ScenePerformanceRecorder::resetRecordingWriteState()
{
    const double unsetBeat = std::numeric_limits<double>::quiet_NaN();
    const float unsetValue = std::numeric_limits<float>::quiet_NaN();
    for (auto& stripBeats : lastRecordedControlEventBeats)
        stripBeats.fill(unsetBeat);
    for (auto& stripValues : lastRecordedControlEventValues)
        stripValues.fill(unsetValue);
}

double ScenePerformanceRecorder::alignRecordingStartBeat(double currentBeat,
                                                         double sceneStartBeat,
                                                         double lengthBeats)
{
    juce::ignoreUnused(lengthBeats);
    if (!std::isfinite(currentBeat))
        return std::isfinite(sceneStartBeat) ? sceneStartBeat : 0.0;
    return currentBeat;
}

double ScenePerformanceRecorder::wrapBeatToClip(double beat,
                                                double sceneStartBeat,
                                                double lengthBeats)
{
    const double safeLength = sanitizeLengthBeats(lengthBeats);
    const double baseBeat = std::isfinite(sceneStartBeat) ? sceneStartBeat : 0.0;
    double wrapped = std::fmod(beat - baseBeat, safeLength);
    if (wrapped < 0.0)
        wrapped += safeLength;
    return wrapped;
}

ScenePerformanceClip& ScenePerformanceRecorder::getClipForWrite(int sceneSlot)
{
    return sceneClips[static_cast<size_t>(clampSceneSlot(sceneSlot))];
}

const ScenePerformanceClip& ScenePerformanceRecorder::getClipForRead(int sceneSlot) const
{
    return sceneClips[static_cast<size_t>(clampSceneSlot(sceneSlot))];
}

void ScenePerformanceRecorder::startRecording(int sceneSlot,
                                              double lengthBeats,
                                              double currentBeat,
                                              double sceneStartBeat)
{
    const int safeSceneSlot = clampSceneSlot(sceneSlot);
    const double safeLength = sanitizeLengthBeats(lengthBeats);
    const double alignedStartBeat = alignRecordingStartBeat(currentBeat, sceneStartBeat, safeLength);

    {
        juce::ScopedLock lock(clipLock);
        auto& clip = getClipForWrite(safeSceneSlot);
        clip.lengthBeats = safeLength;
        clip.version = kScenePerformanceDataVersion;
    }

    resetRecordingWriteState();
    recordingSceneSlot.store(safeSceneSlot, std::memory_order_release);
    recordingSceneStartBeat.store(std::isfinite(sceneStartBeat) ? sceneStartBeat : alignedStartBeat,
                                  std::memory_order_release);
    recordingStartBeat.store(alignedStartBeat, std::memory_order_release);
    recordingEndBeat.store(alignedStartBeat + safeLength, std::memory_order_release);
    recordingOverdub.store(false, std::memory_order_release);
    recording.store(true, std::memory_order_release);
}

void ScenePerformanceRecorder::startOverdub(int sceneSlot,
                                            double lengthBeats,
                                            double currentBeat,
                                            double sceneStartBeat)
{
    const int safeSceneSlot = clampSceneSlot(sceneSlot);
    const double safeLength = sanitizeLengthBeats(lengthBeats);
    bool clipEmpty = false;

    {
        juce::ScopedLock lock(clipLock);
        auto& clip = getClipForWrite(safeSceneSlot);
        clipEmpty = clip.events.empty();
        clip.lengthBeats = safeLength;
    }

    if (clipEmpty)
    {
        startRecording(safeSceneSlot, safeLength, currentBeat, sceneStartBeat);
        return;
    }

    resetRecordingWriteState();
    const double alignedStartBeat = alignRecordingStartBeat(currentBeat, sceneStartBeat, safeLength);
    recordingSceneSlot.store(safeSceneSlot, std::memory_order_release);
    recordingSceneStartBeat.store(std::isfinite(sceneStartBeat) ? sceneStartBeat : alignedStartBeat,
                                  std::memory_order_release);
    recordingStartBeat.store(alignedStartBeat, std::memory_order_release);
    recordingEndBeat.store(alignedStartBeat + safeLength, std::memory_order_release);
    recordingOverdub.store(true, std::memory_order_release);
    recording.store(true, std::memory_order_release);
}

void ScenePerformanceRecorder::stopRecording()
{
    const int sceneSlot = recordingSceneSlot.load(std::memory_order_acquire);
    if (sceneSlot >= 0)
    {
        juce::ScopedLock lock(clipLock);
        sortClipEvents(getClipForWrite(sceneSlot));
    }

    recording.store(false, std::memory_order_release);
    recordingOverdub.store(false, std::memory_order_release);
    recordingSceneSlot.store(-1, std::memory_order_release);
    recordingSceneStartBeat.store(-1.0, std::memory_order_release);
    recordingStartBeat.store(-1.0, std::memory_order_release);
    recordingEndBeat.store(-1.0, std::memory_order_release);
    resetRecordingWriteState();
}

void ScenePerformanceRecorder::extendRecording()
{
    if (!recording.load(std::memory_order_acquire) || !recordingOverdub.load(std::memory_order_acquire))
        return;

    const int sceneSlot = recordingSceneSlot.load(std::memory_order_acquire);
    if (sceneSlot < 0)
        return;

    const double lengthBeats = getClipLengthBeats(sceneSlot);
    const double endBeat = recordingEndBeat.load(std::memory_order_acquire);
    recordingEndBeat.store(endBeat + sanitizeLengthBeats(lengthBeats), std::memory_order_release);
}

bool ScenePerformanceRecorder::updateRecording(double currentBeat)
{
    if (!recording.load(std::memory_order_acquire))
        return false;

    const double endBeat = recordingEndBeat.load(std::memory_order_acquire);
    if (!std::isfinite(endBeat) || currentBeat < endBeat)
        return false;

    stopRecording();
    return true;
}

void ScenePerformanceRecorder::clear(int sceneSlot)
{
    const bool shouldStop = recordingSceneSlot.load(std::memory_order_acquire) == clampSceneSlot(sceneSlot);
    juce::ScopedLock lock(clipLock);
    auto& clip = getClipForWrite(sceneSlot);
    clip.lengthBeats = kDefaultSceneClipLengthBeats;
    clip.version = kScenePerformanceDataVersion;
    clip.events.clear();
    clip.hasMotionState = false;
    clip.motionStrips = {};

    if (shouldStop)
    {
        recording.store(false, std::memory_order_release);
        recordingOverdub.store(false, std::memory_order_release);
        recordingSceneSlot.store(-1, std::memory_order_release);
        recordingSceneStartBeat.store(-1.0, std::memory_order_release);
        recordingStartBeat.store(-1.0, std::memory_order_release);
        recordingEndBeat.store(-1.0, std::memory_order_release);
        resetRecordingWriteState();
    }
}

void ScenePerformanceRecorder::clearAll()
{
    juce::ScopedLock lock(clipLock);
    for (auto& clip : sceneClips)
        resetClip(clip);

    recording.store(false, std::memory_order_release);
    recordingOverdub.store(false, std::memory_order_release);
    recordingSceneSlot.store(-1, std::memory_order_release);
    recordingSceneStartBeat.store(-1.0, std::memory_order_release);
    recordingStartBeat.store(-1.0, std::memory_order_release);
    recordingEndBeat.store(-1.0, std::memory_order_release);
    resetRecordingWriteState();
}

void ScenePerformanceRecorder::copyClip(int sourceSceneSlot, int destSceneSlot)
{
    juce::ScopedLock lock(clipLock);
    getClipForWrite(destSceneSlot) = getClipForRead(sourceSceneSlot);
}

void ScenePerformanceRecorder::setClipLengthBeats(int sceneSlot, double lengthBeats)
{
    const int safeSceneSlot = clampSceneSlot(sceneSlot);
    const double safeLength = sanitizeLengthBeats(lengthBeats);
    const double maxBeat = juce::jmax(0.0, std::nextafter(safeLength, 0.0));

    {
        juce::ScopedLock lock(clipLock);
        auto& clip = getClipForWrite(safeSceneSlot);
        if (std::abs(clip.lengthBeats - safeLength) > 1.0e-9)
        {
            clip.lengthBeats = safeLength;
            clip.version = kScenePerformanceDataVersion;
            for (auto& event : clip.events)
            {
                const double wrappedBeat = wrapBeatToClip(event.timeBeats, 0.0, safeLength);
                event.timeBeats = juce::jlimit(0.0, maxBeat, wrappedBeat);
            }
            sortClipEvents(clip);
        }
    }

    if (recording.load(std::memory_order_acquire)
        && recordingSceneSlot.load(std::memory_order_acquire) == safeSceneSlot)
    {
        const double startBeat = recordingStartBeat.load(std::memory_order_acquire);
        if (std::isfinite(startBeat))
            recordingEndBeat.store(startBeat + safeLength, std::memory_order_release);
    }
}

bool ScenePerformanceRecorder::replaceClipEvents(int sceneSlot,
                                                 const std::vector<ScenePerformanceEvent>& events,
                                                 double lengthBeats)
{
    juce::ScopedLock lock(clipLock);
    auto& clip = getClipForWrite(sceneSlot);
    clip.lengthBeats = sanitizeLengthBeats(lengthBeats);
    clip.version = kScenePerformanceDataVersion;
    const double safeLength = sanitizeLengthBeats(clip.lengthBeats);
    const double maxBeat = juce::jmax(0.0, std::nextafter(safeLength, 0.0));

    clip.events = events;
    for (auto& event : clip.events)
    {
        event.stripIndex = sanitizeEventStripIndex(event.stripIndex, event.controlTarget);
        event.column = juce::jlimit(-1, 15, event.column);
        event.controlMode = juce::jmax(-1, event.controlMode);
        event.controlRow = juce::jmax(-1, event.controlRow);
        event.value = std::isfinite(event.value) ? event.value : 0.0f;
        event.timeBeats = std::isfinite(event.timeBeats)
            ? juce::jlimit(0.0, maxBeat, event.timeBeats)
            : 0.0;
    }

    sortClipEvents(clip);
    return true;
}

void ScenePerformanceRecorder::setMotionStripState(int sceneSlot, int stripIndex, const MotionStripState& state)
{
    juce::ScopedLock lock(clipLock);
    auto& clip = getClipForWrite(sceneSlot);
    clip.version = kScenePerformanceDataVersion;
    clip.hasMotionState = true;
    clip.motionStrips[static_cast<size_t>(clampStripIndex(stripIndex))] = sanitizeMotionStripState(state);
}

ScenePerformanceRecorder::MotionStripState ScenePerformanceRecorder::getMotionStripState(int sceneSlot,
                                                                                         int stripIndex) const
{
    juce::ScopedLock lock(clipLock);
    return getClipForRead(sceneSlot).motionStrips[static_cast<size_t>(clampStripIndex(stripIndex))];
}

bool ScenePerformanceRecorder::hasMotionState(int sceneSlot) const
{
    juce::ScopedLock lock(clipLock);
    return getClipForRead(sceneSlot).hasMotionState;
}

void ScenePerformanceRecorder::recordTriggerEvent(int sceneSlot,
                                                  int stripIndex,
                                                  int column,
                                                  bool noteOn,
                                                  double currentBeat,
                                                  int sampleSliceId,
                                                  int64_t sampleStartSample,
                                                  bool scratchGesture)
{
    if (!recording.load(std::memory_order_acquire))
        return;

    const int activeSceneSlot = recordingSceneSlot.load(std::memory_order_acquire);
    if (activeSceneSlot != clampSceneSlot(sceneSlot))
        return;

    const double startBeat = recordingStartBeat.load(std::memory_order_acquire);
    const double endBeat = recordingEndBeat.load(std::memory_order_acquire);
    if (!std::isfinite(currentBeat) || currentBeat < startBeat || currentBeat >= endBeat)
        return;

    juce::ScopedLock lock(clipLock);
    auto& clip = getClipForWrite(sceneSlot);
    const int safeStripIndex = clampStripIndex(stripIndex);
    const double wrappedBeat = wrapBeatToClip(currentBeat,
                                              recordingSceneStartBeat.load(std::memory_order_acquire),
                                              clip.lengthBeats);

    if (!recordingOverdub.load(std::memory_order_acquire))
    {
        eraseEventsIf(clip,
                      [safeStripIndex, noteOn, wrappedBeat](const ScenePerformanceEvent& existing)
                      {
                          return existing.type == ScenePerformanceEventType::Trigger
                              && existing.stripIndex == safeStripIndex
                              && existing.isNoteOn == noteOn
                              && std::abs(existing.timeBeats - wrappedBeat) <= kSceneEventReplaceEpsilonBeats;
                      });
    }

    ScenePerformanceEvent event;
    event.stripIndex = safeStripIndex;
    event.column = juce::jlimit(0, 15, column);
    event.sampleSliceId = sampleSliceId;
    event.sampleStartSample = sampleStartSample;
    event.isNoteOn = noteOn;
    event.scratchGesture = scratchGesture;
    event.type = ScenePerformanceEventType::Trigger;
    event.timeBeats = wrappedBeat;
    insertEventSorted(clip, event);
}

bool ScenePerformanceRecorder::cancelTriggerEvent(int sceneSlot,
                                                  int stripIndex,
                                                  int column,
                                                  bool noteOn,
                                                  double eventBeat)
{
    const int safeSceneSlot = clampSceneSlot(sceneSlot);
    const int activeSceneSlot = recordingSceneSlot.load(std::memory_order_acquire);
    if (activeSceneSlot != safeSceneSlot)
        return false;

    const double startBeat = recordingStartBeat.load(std::memory_order_acquire);
    const double endBeat = recordingEndBeat.load(std::memory_order_acquire);
    if (!std::isfinite(eventBeat) || eventBeat < startBeat || eventBeat >= endBeat)
        return false;

    juce::ScopedLock lock(clipLock);
    auto& clip = getClipForWrite(safeSceneSlot);
    const int safeStripIndex = clampStripIndex(stripIndex);
    const int safeColumn = juce::jlimit(0, 15, column);
    const double wrappedBeat = wrapBeatToClip(eventBeat,
                                              recordingSceneStartBeat.load(std::memory_order_acquire),
                                              clip.lengthBeats);
    const auto originalSize = clip.events.size();

    eraseEventsIf(clip,
                  [safeStripIndex, safeColumn, noteOn, wrappedBeat](const ScenePerformanceEvent& existing)
                  {
                      return existing.type == ScenePerformanceEventType::Trigger
                          && existing.stripIndex == safeStripIndex
                          && existing.isNoteOn == noteOn
                          && existing.column == safeColumn
                          && std::abs(existing.timeBeats - wrappedBeat) <= kSceneEventReplaceEpsilonBeats;
                  });

    return clip.events.size() != originalSize;
}

void ScenePerformanceRecorder::recordControlEvent(int sceneSlot,
                                                  int stripIndex,
                                                  int controlMode,
                                                  int controlRow,
                                                  int column,
                                                  ScenePerformanceControlTarget controlTarget,
                                                  float value,
                                                  double currentBeat)
{
    if (!recording.load(std::memory_order_acquire))
        return;

    const int activeSceneSlot = recordingSceneSlot.load(std::memory_order_acquire);
    if (activeSceneSlot != clampSceneSlot(sceneSlot))
        return;

    const double startBeat = recordingStartBeat.load(std::memory_order_acquire);
    const double endBeat = recordingEndBeat.load(std::memory_order_acquire);
    if (!std::isfinite(currentBeat) || currentBeat < startBeat || currentBeat >= endBeat)
        return;

    juce::ScopedLock lock(clipLock);
    auto& clip = getClipForWrite(sceneSlot);
    const int safeStripIndex = sanitizeEventStripIndex(stripIndex, controlTarget);
    const auto safeTarget = static_cast<ScenePerformanceControlTarget>(juce::jlimit(
        0,
        NumControlTargets - 1,
        static_cast<int>(controlTarget)));
    const float safeValue = std::isfinite(value) ? value : 0.0f;
    const double wrappedBeat = wrapBeatToClip(currentBeat,
                                              recordingSceneStartBeat.load(std::memory_order_acquire),
                                              clip.lengthBeats);
    const auto targetIndex = static_cast<size_t>(controlTargetToIndex(safeTarget));
    const auto stateStripIndex = static_cast<size_t>(recordedControlStateStripIndex(safeStripIndex, safeTarget));
    const float normalizedValue = normalizeControlValueForThinning(safeTarget, safeValue);
    const double lastRecordedBeat = lastRecordedControlEventBeats[stateStripIndex][targetIndex];
    const float lastRecordedValue = lastRecordedControlEventValues[stateStripIndex][targetIndex];

    if (!recordingOverdub.load(std::memory_order_acquire) && !std::isfinite(lastRecordedBeat))
    {
        eraseEventsIf(clip,
                      [safeStripIndex, safeTarget](const ScenePerformanceEvent& existing)
                      {
                          return existing.type == ScenePerformanceEventType::ControlPoint
                              && existing.stripIndex == safeStripIndex
                              && existing.controlTarget == safeTarget;
                      });
    }

    eraseEventsIf(clip,
                  [safeStripIndex, safeTarget, wrappedBeat](const ScenePerformanceEvent& existing)
                  {
                      return existing.type == ScenePerformanceEventType::ControlPoint
                          && existing.stripIndex == safeStripIndex
                          && existing.controlTarget == safeTarget
                          && std::abs(existing.timeBeats - wrappedBeat) <= kSceneEventReplaceEpsilonBeats;
                  });

    if (std::isfinite(lastRecordedBeat) && std::abs(lastRecordedBeat - wrappedBeat) > kSceneEventReplaceEpsilonBeats)
    {
        const bool wrappedForward = wrappedBeat < lastRecordedBeat;
        const double rangeStart = juce::jmin(lastRecordedBeat, wrappedBeat);
        const double rangeEnd = juce::jmax(lastRecordedBeat, wrappedBeat);
        eraseEventsIf(clip,
                      [safeStripIndex, safeTarget, rangeStart, rangeEnd, wrappedForward](const ScenePerformanceEvent& existing)
                      {
                          if (existing.type != ScenePerformanceEventType::ControlPoint
                              || existing.stripIndex != safeStripIndex
                              || existing.controlTarget != safeTarget)
                          {
                              return false;
                          }

                          if (!wrappedForward)
                          {
                              return existing.timeBeats > (rangeStart + kSceneEventReplaceEpsilonBeats)
                                  && existing.timeBeats <= (rangeEnd + kSceneEventReplaceEpsilonBeats);
                          }

                          return existing.timeBeats > (rangeEnd + kSceneEventReplaceEpsilonBeats)
                              || existing.timeBeats <= (rangeStart + kSceneEventReplaceEpsilonBeats);
                      });
    }

    const bool hasRecentRecordedPoint = std::isfinite(lastRecordedBeat)
        && std::isfinite(lastRecordedValue)
        && wrappedForwardBeatDistance(lastRecordedBeat, wrappedBeat, clip.lengthBeats)
            <= (kSceneControlThinBeatWindowBeats + kSceneEventReplaceEpsilonBeats)
        && std::abs(normalizedValue - lastRecordedValue) <= controlValueThinThreshold(safeTarget);

    if (hasRecentRecordedPoint)
    {
        eraseEventsIf(clip,
                      [safeStripIndex, safeTarget, lastRecordedBeat](const ScenePerformanceEvent& existing)
                      {
                          return existing.type == ScenePerformanceEventType::ControlPoint
                              && existing.stripIndex == safeStripIndex
                              && existing.controlTarget == safeTarget
                              && std::abs(existing.timeBeats - lastRecordedBeat) <= kSceneEventReplaceEpsilonBeats;
                      });
    }

    ScenePerformanceEvent event;
    event.stripIndex = safeStripIndex;
    event.column = juce::jlimit(-1, 15, column);
    event.controlMode = juce::jmax(-1, controlMode);
    event.controlRow = juce::jmax(-1, controlRow);
    event.controlTarget = safeTarget;
    event.value = safeValue;
    event.type = ScenePerformanceEventType::ControlPoint;
    event.timeBeats = wrappedBeat;
    insertEventSorted(clip, event);
    lastRecordedControlEventBeats[stateStripIndex][targetIndex] = wrappedBeat;
    lastRecordedControlEventValues[stateStripIndex][targetIndex] = normalizedValue;
}

std::vector<ScenePerformanceEvent> ScenePerformanceRecorder::getEventsSnapshot(int sceneSlot) const
{
    juce::ScopedLock lock(clipLock);
    return getClipForRead(sceneSlot).events;
}

int ScenePerformanceRecorder::getEventCount(int sceneSlot) const
{
    juce::ScopedLock lock(clipLock);
    return static_cast<int>(getClipForRead(sceneSlot).events.size());
}

double ScenePerformanceRecorder::getClipLengthBeats(int sceneSlot) const
{
    juce::ScopedLock lock(clipLock);
    return sanitizeLengthBeats(getClipForRead(sceneSlot).lengthBeats);
}

bool ScenePerformanceRecorder::hasEvents(int sceneSlot) const
{
    juce::ScopedLock lock(clipLock);
    return !getClipForRead(sceneSlot).events.empty();
}

double ScenePerformanceRecorder::getPlaybackProgressForBeat(int sceneSlot,
                                                            double sceneStartBeat,
                                                            double currentBeat) const
{
    if (!std::isfinite(sceneStartBeat) || !std::isfinite(currentBeat))
        return -1.0;

    const double lengthBeats = getClipLengthBeats(sceneSlot);
    if (lengthBeats <= 0.0)
        return -1.0;

    if (!hasEvents(sceneSlot))
        return -1.0;

    return juce::jlimit(0.0, 1.0, wrapBeatToClip(currentBeat, sceneStartBeat, lengthBeats) / lengthBeats);
}

double ScenePerformanceRecorder::getRecordingProgressForBeat(double currentBeat) const
{
    if (!recording.load(std::memory_order_acquire) || !std::isfinite(currentBeat))
        return -1.0;

    const int sceneSlot = recordingSceneSlot.load(std::memory_order_acquire);
    const double sceneStartBeat = recordingSceneStartBeat.load(std::memory_order_acquire);
    if (sceneSlot < 0 || !std::isfinite(sceneStartBeat))
        return -1.0;

    const double lengthBeats = getClipLengthBeats(sceneSlot);
    if (lengthBeats <= 0.0)
        return -1.0;

    return juce::jlimit(0.0, 1.0, wrapBeatToClip(currentBeat, sceneStartBeat, lengthBeats) / lengthBeats);
}

juce::MemoryBlock ScenePerformanceRecorder::createData(int sceneSlotOverride,
                                                       const std::array<double, MaxSceneSlots>* lengthOverrides) const
{
    juce::MemoryOutputStream out;
    out.writeInt(kScenePerformanceDataMagic);
    out.writeInt(kScenePerformanceDataVersion);

    juce::ScopedLock lock(clipLock);

    if (sceneSlotOverride >= 0)
    {
        const int safeSceneSlot = clampSceneSlot(sceneSlotOverride);
        const auto& clip = getClipForRead(safeSceneSlot);
        out.writeInt(1);
        const double serializedLength = lengthOverrides != nullptr
            ? (*lengthOverrides)[static_cast<size_t>(safeSceneSlot)]
            : clip.lengthBeats;
        writeClip(out, safeSceneSlot, clip, serializedLength);
        return out.getMemoryBlock();
    }

    out.writeInt(MaxSceneSlots);
    for (int sceneSlot = 0; sceneSlot < MaxSceneSlots; ++sceneSlot)
    {
        const auto& clip = getClipForRead(sceneSlot);
        const double serializedLength = lengthOverrides != nullptr
            ? (*lengthOverrides)[static_cast<size_t>(sceneSlot)]
            : clip.lengthBeats;
        writeClip(out, sceneSlot, clip, serializedLength);
    }

    return out.getMemoryBlock();
}

bool ScenePerformanceRecorder::applyData(const juce::MemoryBlock& data, int sceneSlotOverride)
{
    juce::ScopedLock lock(clipLock);
    auto parsedClips = std::make_unique<std::array<ScenePerformanceClip, MaxSceneSlots>>(sceneClips);
    const int safeSceneSlotOverride = sceneSlotOverride >= 0 ? clampSceneSlot(sceneSlotOverride) : -1;

    if (safeSceneSlotOverride >= 0)
    {
        auto& clip = (*parsedClips)[static_cast<size_t>(safeSceneSlotOverride)];
        resetClip(clip);
    }
    else
    {
        for (auto& clip : *parsedClips)
            resetClip(clip);
    }

    if (data.getSize() == 0)
    {
        recording.store(false, std::memory_order_release);
        recordingOverdub.store(false, std::memory_order_release);
        recordingSceneSlot.store(-1, std::memory_order_release);
        recordingSceneStartBeat.store(-1.0, std::memory_order_release);
        recordingStartBeat.store(-1.0, std::memory_order_release);
        recordingEndBeat.store(-1.0, std::memory_order_release);
        resetRecordingWriteState();
        sceneClips = std::move(*parsedClips);
        return true;
    }

    juce::MemoryInputStream in(data, false);
    const auto readByteChecked = [&in](char& value) -> bool
    {
        if (in.getNumBytesRemaining() < static_cast<juce::int64>(sizeof(char)))
            return false;
        value = static_cast<char>(in.readByte());
        return true;
    };
    const auto readIntChecked = [&in](int& value) -> bool
    {
        if (in.getNumBytesRemaining() < static_cast<juce::int64>(sizeof(int)))
            return false;
        value = in.readInt();
        return true;
    };
    const auto readInt64Checked = [&in](int64_t& value) -> bool
    {
        if (in.getNumBytesRemaining() < static_cast<juce::int64>(sizeof(int64_t)))
            return false;
        value = static_cast<int64_t>(in.readInt64());
        return true;
    };
    const auto readFloatChecked = [&in](float& value) -> bool
    {
        if (in.getNumBytesRemaining() < static_cast<juce::int64>(sizeof(float)))
            return false;
        value = in.readFloat();
        return true;
    };
    const auto readDoubleChecked = [&in](double& value) -> bool
    {
        if (in.getNumBytesRemaining() < static_cast<juce::int64>(sizeof(double)))
            return false;
        value = in.readDouble();
        return true;
    };
    const auto readMotionLaneStateChecked = [&](ScenePerformanceMotionLaneState& lane) -> bool
    {
        int rawInt = 0;
        char rawByte = 0;
        float rawFloat = 0.0f;

        if (!readIntChecked(rawInt))
            return false;
        lane.target = performanceTargetFromRaw(rawInt);

        if (!readByteChecked(rawByte))
            return false;
        lane.bipolar = rawByte != 0;

        if (!readByteChecked(rawByte))
            return false;
        lane.curveMode = rawByte != 0;

        if (!readFloatChecked(rawFloat))
            return false;
        lane.depth = rawFloat;

        if (!readFloatChecked(rawFloat))
            return false;
        lane.rate = rawFloat;

        if (!readIntChecked(rawInt))
            return false;
        lane.transportMode = static_cast<ModernAudioEngine::ModTransportMode>(rawInt);

        if (!readIntChecked(rawInt))
            return false;
        lane.offset = rawInt;

        if (!readIntChecked(rawInt))
            return false;
        lane.lengthBars = rawInt;

        if (!readIntChecked(rawInt))
            return false;
        lane.editPage = rawInt;

        if (!readFloatChecked(rawFloat))
            return false;
        lane.smoothingMs = rawFloat;

        if (!readFloatChecked(rawFloat))
            return false;
        lane.curveBend = rawFloat;

        if (!readIntChecked(rawInt))
            return false;
        lane.curveShape = static_cast<ModernAudioEngine::ModCurveShape>(rawInt);

        if (!readByteChecked(rawByte))
            return false;
        lane.pitchScaleQuantize = rawByte != 0;

        if (!readIntChecked(rawInt))
            return false;
        lane.pitchScale = static_cast<ModernAudioEngine::PitchScale>(rawInt);

        for (int stepIndex = 0; stepIndex < ModernAudioEngine::ModTotalSteps; ++stepIndex)
        {
            const auto index = static_cast<size_t>(stepIndex);

            if (!readFloatChecked(rawFloat))
                return false;
            lane.steps[index] = rawFloat;

            if (!readIntChecked(rawInt))
                return false;
            lane.stepSubdivisions[index] = rawInt;

            if (!readFloatChecked(rawFloat))
                return false;
            lane.stepEndValues[index] = rawFloat;

            if (!readIntChecked(rawInt))
                return false;
            lane.stepCurveShapes[index] = static_cast<ModernAudioEngine::ModCurveShape>(rawInt);
        }

        lane = sanitizeMotionLane(lane);
        return true;
    };

    int magic = 0;
    if (!readIntChecked(magic) || magic != kScenePerformanceDataMagic)
        return false;

    int version = 0;
    if (!readIntChecked(version))
        return false;
    if (version < 1 || version > kScenePerformanceDataVersion)
        return false;

    int rawClipCount = 0;
    if (!readIntChecked(rawClipCount))
        return false;

    const int clipCount = juce::jlimit(0, MaxSceneSlots, rawClipCount);
    for (int clipIndex = 0; clipIndex < clipCount; ++clipIndex)
    {
        int rawSceneSlot = 0;
        double rawClipLengthBeats = 0.0;
        int rawClipVersion = 0;
        int rawEventCount = 0;
        if (!readIntChecked(rawSceneSlot)
            || !readDoubleChecked(rawClipLengthBeats)
            || !readIntChecked(rawClipVersion)
            || !readIntChecked(rawEventCount))
        {
            return false;
        }

        const int xmlSceneSlot = clampSceneSlot(rawSceneSlot);
        const double clipLengthBeats = sanitizeLengthBeats(rawClipLengthBeats);
        const uint32_t clipVersion = static_cast<uint32_t>(juce::jmax(1, rawClipVersion));
        const int eventCount = juce::jmax(0, rawEventCount);

        const bool shouldApplyClip = safeSceneSlotOverride < 0 || xmlSceneSlot == safeSceneSlotOverride;
        ScenePerformanceClip* clip = shouldApplyClip
            ? &(*parsedClips)[static_cast<size_t>(xmlSceneSlot)]
            : nullptr;
        if (clip != nullptr)
        {
            clip->lengthBeats = clipLengthBeats;
            clip->version = clipVersion;
            clip->events.clear();
            clip->hasMotionState = false;
            clip->motionStrips = {};
            clip->events.reserve(static_cast<size_t>(eventCount));
        }

        for (int eventIndex = 0; eventIndex < eventCount; ++eventIndex)
        {
            ScenePerformanceEvent event;
            char rawType = 0;
            char rawTarget = 0;
            char rawNoteOn = 0;
            char rawDrawStepped = 0;
            char rawScratchGesture = 0;
            if (!readByteChecked(rawType)
                || !readIntChecked(event.stripIndex)
                || !readIntChecked(event.column)
                || !readIntChecked(event.sampleSliceId)
                || !readInt64Checked(event.sampleStartSample)
                || !readIntChecked(event.controlMode)
                || !readIntChecked(event.controlRow)
                || !readByteChecked(rawTarget)
                || !readFloatChecked(event.value)
                || !readDoubleChecked(event.timeBeats)
                || !readByteChecked(rawNoteOn))
            {
                return false;
            }
            if (version >= 3 && !readByteChecked(rawDrawStepped))
                return false;
            if (version >= 4 && !readByteChecked(rawScratchGesture))
                return false;
            event.type = static_cast<ScenePerformanceEventType>(
                juce::jlimit(0, 1, static_cast<int>(rawType)));
            event.controlTarget = static_cast<ScenePerformanceControlTarget>(
                juce::jlimit(0,
                             static_cast<int>(ScenePerformanceControlTarget::GrainShape),
                             static_cast<int>(rawTarget)));
            event.isNoteOn = rawNoteOn != 0;
            event.drawStepped = version >= 3 && rawDrawStepped != 0;
            event.scratchGesture = version >= 4 && rawScratchGesture != 0;
            if (clip != nullptr)
            {
                if (event.type == ScenePerformanceEventType::ControlPoint
                    && event.controlTarget == ScenePerformanceControlTarget::Rearrange)
                {
                    continue;
                }
                event.stripIndex = sanitizeEventStripIndex(event.stripIndex, event.controlTarget);
                clip->events.push_back(event);
            }
        }

        if (clip != nullptr)
            sortClipEvents(*clip);

        if (version >= 2)
        {
            char rawHasMotionState = 0;
            if (!readByteChecked(rawHasMotionState))
                return false;

            const bool hasMotionState = rawHasMotionState != 0;
            if (hasMotionState)
            {
                for (int stripIndex = 0; stripIndex < ModernAudioEngine::MaxStrips; ++stripIndex)
                {
                    ScenePerformanceMotionStripState stripState;
                    if (!readIntChecked(stripState.activeSlot))
                        return false;
                    for (int slot = 0; slot < ModernAudioEngine::NumModSequencers; ++slot)
                    {
                        if (!readMotionLaneStateChecked(stripState.lanes[static_cast<size_t>(slot)]))
                            return false;
                    }

                    if (clip != nullptr)
                    {
                        clip->hasMotionState = true;
                        clip->motionStrips[static_cast<size_t>(stripIndex)] = sanitizeMotionStripState(stripState);
                    }
                }
            }
        }
    }

    recording.store(false, std::memory_order_release);
    recordingOverdub.store(false, std::memory_order_release);
    recordingSceneSlot.store(-1, std::memory_order_release);
    recordingSceneStartBeat.store(-1.0, std::memory_order_release);
    recordingStartBeat.store(-1.0, std::memory_order_release);
    recordingEndBeat.store(-1.0, std::memory_order_release);
    resetRecordingWriteState();
    sceneClips = std::move(*parsedClips);
    return true;
}
