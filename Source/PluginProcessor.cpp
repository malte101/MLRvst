/*
  ==============================================================================

    PluginProcessor.cpp
    mlrVST - Modern Edition Implementation

  ==============================================================================
*/

#include "PluginProcessor.h"
#include "AudioEngineModulation.h"
#include "GlobalSettingsStore.h"
#include "MacroTargetDispatcher.h"
#include "MonomeFilterActions.h"
#include "MonomeMixActions.h"
#include "PluginEditor.h"
#include "PlayheadSpeedQuantizer.h"
#include "PresetStore.h"
#include "SceneAutomationRules.h"
#include "SceneScheduler.h"
#include "WarpGrid.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

#if MLRVST_ENABLE_BUNGEE
 #include <bungee/Stream.h>
#endif

namespace
{
struct BarSelection
{
    int recordingBars = 2;
    float beatsPerLoop = 8.0f;
};

BarSelection decodeBarSelection(int value)
{
    switch (value)
    {
        case 25:  return { 1, 1.0f };   // 1/4 bar
        case 50:  return { 1, 2.0f };   // 1/2 bar
        case 100: return { 1, 4.0f };   // 1 bar
        case 200: return { 2, 8.0f };   // 2 bars
        case 400: return { 4, 16.0f };  // 4 bars
        case 800: return { 8, 32.0f };  // 8 bars
        // Backward compatibility (monome and legacy callers)
        case 1:   return { 1, 4.0f };
        case 2:   return { 2, 8.0f };
        case 4:   return { 4, 16.0f };
        case 8:   return { 8, 32.0f };
        default:  return { 2, 8.0f };
    }
}

int sanitizePitchControlModeIndex(int storedIndex) noexcept
{
    return juce::jlimit(0, 4, storedIndex);
}

float normalizeSceneControlValue(const ScenePerformanceEvent& event)
{
    return SceneAutomationRules::normalizeValue(event);
}

float defaultSceneControlValue(ScenePerformanceControlTarget target) noexcept
{
    return SceneAutomationRules::defaultValue(target);
}

MlrVSTAudioProcessor::ControlMode sceneControlModeForTarget(ScenePerformanceControlTarget target) noexcept
{
    return SceneAutomationRules::controlModeForTarget(target);
}

int sceneControlRowForTarget(ScenePerformanceControlTarget target) noexcept
{
    return SceneAutomationRules::controlRowForTarget(target);
}

ScenePerformanceEvent makeSceneControlPointEvent(int stripIndex,
                                                 ScenePerformanceControlTarget target,
                                                 double timeBeats,
                                                 float value)
{
    ScenePerformanceEvent event;
    event.type = ScenePerformanceEventType::ControlPoint;
    event.stripIndex = juce::jlimit(0, MlrVSTAudioProcessor::MaxStrips - 1, stripIndex);
    event.timeBeats = juce::jmax(0.0, timeBeats);
    event.controlTarget = target;
    event.controlMode = static_cast<int>(sceneControlModeForTarget(target));
    event.controlRow = sceneControlRowForTarget(target);
    event.value = value;
    event.column = juce::jlimit(0,
                                15,
                                static_cast<int>(std::round(normalizeSceneControlValue(event) * 15.0f)));
    return event;
}

bool sceneAutomationTargetHasEditorLane(ScenePerformanceControlTarget target) noexcept
{
    switch (target)
    {
        case ScenePerformanceControlTarget::Volume:
        case ScenePerformanceControlTarget::Pan:
        case ScenePerformanceControlTarget::Pitch:
        case ScenePerformanceControlTarget::FilterFrequency:
        case ScenePerformanceControlTarget::FilterResonance:
        case ScenePerformanceControlTarget::FilterMorph:
        case ScenePerformanceControlTarget::Speed:
        case ScenePerformanceControlTarget::Retrigger:
        case ScenePerformanceControlTarget::SliceLength:
        case ScenePerformanceControlTarget::Scratch:
        case ScenePerformanceControlTarget::DelayMix:
        case ScenePerformanceControlTarget::DelayTime:
        case ScenePerformanceControlTarget::DelayFeedback:
        case ScenePerformanceControlTarget::GrainPitch:
        case ScenePerformanceControlTarget::GrainSize:
        case ScenePerformanceControlTarget::GrainDensity:
        case ScenePerformanceControlTarget::GrainPitchJitter:
        case ScenePerformanceControlTarget::GrainSpread:
        case ScenePerformanceControlTarget::GrainJitter:
        case ScenePerformanceControlTarget::GrainPositionJitter:
        case ScenePerformanceControlTarget::GrainRandomDepth:
        case ScenePerformanceControlTarget::GrainArp:
        case ScenePerformanceControlTarget::GrainCloud:
        case ScenePerformanceControlTarget::GrainEmitter:
        case ScenePerformanceControlTarget::GrainEnvelope:
        case ScenePerformanceControlTarget::GrainShape:
            return true;
        case ScenePerformanceControlTarget::None:
        case ScenePerformanceControlTarget::Swing:
        case ScenePerformanceControlTarget::FilterEnabled:
        case ScenePerformanceControlTarget::DelayLowCut:
        case ScenePerformanceControlTarget::DelayHighCut:
        case ScenePerformanceControlTarget::DelayMode:
        case ScenePerformanceControlTarget::DelaySyncEnabled:
        case ScenePerformanceControlTarget::Rearrange:
        default:
            return false;
    }
}

float innerLoopLengthFactorForChoice(int choice) noexcept
{
    static constexpr std::array<float, 5> kFactors { 1.0f, 0.5f, 0.25f, 0.125f, 0.0625f };
    return kFactors[static_cast<size_t>(juce::jlimit(0, 4, choice))];
}

bool shouldRecordSceneScratchGestureTrigger(const EnhancedAudioStrip& strip, int column) noexcept
{
    static constexpr float kScratchGestureEpsilon = 1.0e-6f;
    if (strip.getScratchAmount() <= kScratchGestureEpsilon)
        return false;

    if (strip.getPlayMode() == EnhancedAudioStrip::PlayMode::Grain)
        return strip.getGrainHeldCount() > 0;

    return strip.isButtonHeld(juce::jlimit(0, MlrVSTAudioProcessor::MaxColumns - 1, column));
}

int sanitizeStripPitchControlModeIndex(int storedIndex) noexcept
{
    return juce::jlimit(0, 5, storedIndex);
}

float snapSoundTouchPitchCacheSemitones(float semitones) noexcept
{
    constexpr float step = 0.25f;
    return juce::jlimit(-24.0f,
                        24.0f,
                        std::round(semitones / step) * step);
}

float snapSignalsmithPitchCacheSemitones(float semitones) noexcept
{
    constexpr float step = 0.01f;
    return juce::jlimit(-24.0f,
                        24.0f,
                        std::round(semitones / step) * step);
}

float snapBungeePitchCacheSemitones(float semitones) noexcept
{
    constexpr float step = 0.01f;
    return juce::jlimit(-24.0f,
                        24.0f,
                        std::round(semitones / step) * step);
}

constexpr int kSoundTouchPitchCacheStableTimerTicks = 6;
// Keep the internal Signalsmith alignment delay, but avoid host-facing latency
// restarts for now. Some VST3 hosts can stall if latency changes during or
// just after project/plugin restoration.
constexpr bool kReportRealtimeSignalsmithLatencyToHost = false;

bool pathUsesMissingVolumeMount(const juce::String& rawPath)
{
    static constexpr const char* kVolumesPrefix = "/Volumes/";
    auto path = rawPath.trim();
    if (!path.startsWith(kVolumesPrefix))
        return false;

    path = path.fromFirstOccurrenceOf(kVolumesPrefix, false, false);
    const int slashIndex = path.indexOfChar('/');
    const auto volumeName = (slashIndex >= 0 ? path.substring(0, slashIndex) : path).trim();
    if (volumeName.isEmpty())
        return false;

    return !juce::File("/Volumes").getChildFile(volumeName).exists();
}

bool canSafelyProbeFilesystemPath(const juce::File& file)
{
    if (file == juce::File())
        return false;

    const auto path = file.getFullPathName().trim();
    if (path.isEmpty() || !juce::File::isAbsolutePath(path))
        return false;

    return !pathUsesMissingVolumeMount(path);
}

bool safeFileExistsAsFile(const juce::File& file)
{
    return canSafelyProbeFilesystemPath(file) && file.existsAsFile();
}


float semitonesFromRatio(float ratio)
{
    const float safeRatio = juce::jlimit(0.03125f, 8.0f, ratio);
    return static_cast<float>(12.0 * std::log2(static_cast<double>(safeRatio)));
}

float computeFlipTempoMatchRatio(double hostTempo, double sourceTempo)
{
    if (!std::isfinite(hostTempo) || !std::isfinite(sourceTempo) || hostTempo <= 0.0 || sourceTempo <= 0.0)
        return 1.0f;

    return juce::jlimit(0.25f, 4.0f, static_cast<float>(hostTempo / sourceTempo));
}


void appendSceneDebugLog(const juce::String& message)
{
    const auto logFile = juce::File::getSpecialLocation(juce::File::tempDirectory)
        .getChildFile("mlrvst_scene_debug.log");
    juce::FileOutputStream out(logFile);
    if (!out.openedOk())
        return;

    out.setPosition(logFile.existsAsFile() ? logFile.getSize() : 0);
    const auto timestamp = juce::Time::getCurrentTime().formatted("%Y-%m-%d %H:%M:%S.%3Q");
    out.writeText(timestamp + " " + message + "\n", false, false, "\n");
    out.flush();
}

constexpr juce::int64 kPersistentGlobalControlsSaveDebounceMs = 350;
// masterVolume and quantize are deliberately NOT persisted across instances:
// fresh instances start at unity gain / 1-16 quantize (the factory defaults);
// DAW sessions still recall them through normal plugin state.
constexpr std::array<const char*, 19> kPersistentGlobalControlParameterIds {
    "limiterThreshold",
    "limiterEnabled",
    "innerLoopLength",
    "quality",
    "pitchSmoothing",
    "crossfadeLength",
    "triggerFadeIn",
    "outputRouting",
    "pitchControlMode",
    "flipTempoMatchMode",
    "stretchBackend",
    "continuousTraversal",
    "sceneMode",
    "sceneRecallMode",
    "soundTouchEnabled",
    "transientOnsetMethod",
    "transientSensitivity",
    "transientSnap",
    "transientSpacing"
};

template <typename Fn>
void forEachSceneAutosaveParameterId(Fn&& fn)
{
    for (int stripIndex = 0; stripIndex < MlrVSTAudioProcessor::MaxStrips; ++stripIndex)
    {
        const auto suffix = juce::String(stripIndex);
        fn("stripVolume" + suffix);
        fn("stripTrimDb" + suffix);
        fn("stripPan" + suffix);
        fn("stripSpeed" + suffix);
        fn("stripPitch" + suffix);
        fn("stripSliceLength" + suffix);
        fn("stripPitchControlMode" + suffix);
        fn("stripTempoMatchMode" + suffix);
        fn("stripFilterEnabled" + suffix);
        fn("stripFilterFrequency" + suffix);
        fn("stripFilterResonance" + suffix);
        fn("stripFilterMorph" + suffix);
        fn("stripFilterAlgorithm" + suffix);
        fn("stripDuckEnabled" + suffix);
        fn("stripDuckSource" + suffix);
        fn("stripDuckThreshold" + suffix);
        fn("stripDuckRatio" + suffix);
        fn("stripDuckAttack" + suffix);
        fn("stripDuckRelease" + suffix);
        fn("stripDuckGainComp" + suffix);
        fn("stripDuckFollowMaster" + suffix);
        fn("stripDelayMix" + suffix);
        fn("stripDelayTime" + suffix);
        fn("stripDelaySync" + suffix);
        fn("stripDelayFeedback" + suffix);
        fn("stripDelayLowCut" + suffix);
        fn("stripDelayHighCut" + suffix);
        fn("stripDelayMode" + suffix);
    }
}


float gatePageSpeedForMode(MlrVSTAudioProcessor::GatePageMode mode, float beatsPerLoop) noexcept
{
    const float safeBeatsPerLoop = (beatsPerLoop > 0.0f) ? beatsPerLoop : 4.0f;
    int slicesPerLoop = 0;
    switch (mode)
    {
        case MlrVSTAudioProcessor::GatePageMode::Quarter:   slicesPerLoop = 4;  break;
        case MlrVSTAudioProcessor::GatePageMode::Sixth:     slicesPerLoop = 6;  break;
        case MlrVSTAudioProcessor::GatePageMode::Eighth:    slicesPerLoop = 8;  break;
        case MlrVSTAudioProcessor::GatePageMode::Sixteenth: slicesPerLoop = 16; break;
        case MlrVSTAudioProcessor::GatePageMode::Adaptive:
        default:                                            slicesPerLoop = 0;  break;
    }

    if (slicesPerLoop <= 0)
        return 4.0f;

    return juce::jlimit(0.25f, 8.0f, static_cast<float>(slicesPerLoop) / safeBeatsPerLoop);
}

constexpr int kStutterButtonFirstColumn = 9;
constexpr int kStutterButtonCount = 7;

uint8_t stutterButtonBitFromColumn(int column)
{
    if (column < kStutterButtonFirstColumn || column >= (kStutterButtonFirstColumn + kStutterButtonCount))
        return 0;
    return static_cast<uint8_t>(1u << static_cast<unsigned int>(column - kStutterButtonFirstColumn));
}

int countStutterBits(uint8_t mask)
{
    int count = 0;
    for (int i = 0; i < kStutterButtonCount; ++i)
    {
        if ((mask & static_cast<uint8_t>(1u << static_cast<unsigned int>(i))) != 0)
            ++count;
    }
    return count;
}

int highestStutterBit(uint8_t mask)
{
    for (int i = kStutterButtonCount - 1; i >= 0; --i)
    {
        if ((mask & static_cast<uint8_t>(1u << static_cast<unsigned int>(i))) != 0)
            return i;
    }
    return 0;
}

int lowestStutterBit(uint8_t mask)
{
    for (int i = 0; i < kStutterButtonCount; ++i)
    {
        if ((mask & static_cast<uint8_t>(1u << static_cast<unsigned int>(i))) != 0)
            return i;
    }
    return 0;
}

double stutterDivisionBeatsFromBit(int bit)
{
    static constexpr std::array<double, kStutterButtonCount> kDivisionBeats{
        2.0,            // bit 0 (col 9)  -> 1/2
        1.0,            // bit 1 (col 10) -> 1/4
        0.5,            // bit 2 (col 11) -> 1/8
        0.25,           // bit 3 (col 12) -> 1/16
        0.125,          // bit 4 (col 13) -> 1/32
        0.0625,         // bit 5 (col 14) -> 1/64
        0.03125         // bit 6 (col 15) -> 1/128
    };
    const int idx = juce::jlimit(0, kStutterButtonCount - 1, bit);
    return kDivisionBeats[static_cast<size_t>(idx)];
}

double stutterDivisionBeatsFromBitForMacro(int bit, bool preferStraight)
{
    const double base = stutterDivisionBeatsFromBit(bit);
    if (!preferStraight)
        return base;

    switch (juce::jlimit(0, kStutterButtonCount - 1, bit))
    {
        // Keep macro path mostly in the core straight-musical range.
        case 0: return 1.0;   // clamp 1/2 to 1/4 for multi-button macro motion
        case 5: return 0.125; // clamp 1/64 to 1/32
        case 6: return 0.125; // clamp 1/128 to 1/32
        default: return base;
    }
}

template <size_t N>
double snapDivisionToGrid(double divisionBeats, const std::array<double, N>& grid)
{
    if (!std::isfinite(divisionBeats))
        return grid[0];

    double best = grid[0];
    double bestDist = std::abs(divisionBeats - best);
    for (size_t i = 1; i < N; ++i)
    {
        const double cand = grid[i];
        const double dist = std::abs(divisionBeats - cand);
        if (dist < bestDist)
        {
            best = cand;
            bestDist = dist;
        }
    }
    return best;
}

double wrapUnitPhase(double phase)
{
    if (!std::isfinite(phase))
        return 0.0;
    phase = std::fmod(phase, 1.0);
    if (phase < 0.0)
        phase += 1.0;
    return phase;
}

float cutoffFromNormalized(float normalized)
{
    normalized = juce::jlimit(0.0f, 1.0f, normalized);
    return 20.0f * std::pow(1000.0f, normalized);
}

EnhancedAudioStrip::FilterAlgorithm filterAlgorithmFromIndex(int index)
{
    switch (juce::jlimit(0, 6, index))
    {
        case 0: return EnhancedAudioStrip::FilterAlgorithm::Tpt12;
        case 1: return EnhancedAudioStrip::FilterAlgorithm::Tpt24;
        case 2: return EnhancedAudioStrip::FilterAlgorithm::Ladder12;
        case 3: return EnhancedAudioStrip::FilterAlgorithm::Ladder24;
        case 4: return EnhancedAudioStrip::FilterAlgorithm::MoogStilson;
        case 5: return EnhancedAudioStrip::FilterAlgorithm::MoogHuov;
        case 6:
        default: return EnhancedAudioStrip::FilterAlgorithm::Comb;
    }
}
}

//==============================================================================
// MonomeConnection Implementation
//==============================================================================

MonomeConnection::MonomeConnection()
{
    // Start heartbeat timer for connection monitoring
    startTimer(1000); // Check every second
}

MonomeConnection::~MonomeConnection()
{
    stopTimer();
    disconnect();
}

void MonomeConnection::connect(int appPort)
{
    if (receiverConnected)
    {
        oscReceiver.removeListener(this);
        oscReceiver.disconnect();
        receiverConnected = false;
    }

    gridEndpoint.sender.disconnect();
    gridEndpoint.connected = false;
    gridEndpoint.reconnectAttempts = 0;
    gridEndpoint.lastConnectAttemptTime = 0;
    gridEndpoint.lastPingTime = 0;
    arcEndpoint.sender.disconnect();
    arcEndpoint.connected = false;
    arcEndpoint.reconnectAttempts = 0;
    arcEndpoint.lastConnectAttemptTime = 0;
    arcEndpoint.lastPingTime = 0;

    int boundPort = -1;
    for (int offset = 0; offset < 32; ++offset)
    {
        const int candidate = appPort + offset;
        if (oscReceiver.connect(candidate))
        {
            boundPort = candidate;
            break;
        }
    }

    if (boundPort < 0)
        return;

    applicationPort = boundPort;
    receiverConnected = true;
    oscReceiver.addListener(this);
    (void) serialoscSender.connect("127.0.0.1", 12002);
    lastDiscoveryTime = 0;
    discoverDevices();
}

void MonomeConnection::refreshDeviceList()
{
    devices.clear();
    discoverDevices();
}

void MonomeConnection::disconnect()
{
    if (receiverConnected)
    {
        oscReceiver.removeListener(this);
        oscReceiver.disconnect();
        receiverConnected = false;
    }

    markDisconnected(DeviceRole::Grid);
    markDisconnected(DeviceRole::Arc);
    serialoscSender.disconnect();
    lastDiscoveryTime = 0;
}

void MonomeConnection::discoverDevices()
{
    if (!serialoscSender.connect("127.0.0.1", 12002))
        return;

    // Query for device list
    const bool sentList = serialoscSender.send(
        juce::OSCMessage("/serialosc/list", juce::String("127.0.0.1"), applicationPort));

    // Subscribe to device notifications
    const bool sentNotify = serialoscSender.send(
        juce::OSCMessage("/serialosc/notify", juce::String("127.0.0.1"), applicationPort));

    if (sentList || sentNotify)
        lastDiscoveryTime = juce::Time::currentTimeMillis();
}

void MonomeConnection::selectDevice(int deviceIndex)
{
    if (deviceIndex < 0 || deviceIndex >= static_cast<int>(devices.size()))
        return;
    if (deviceMatchesRole(devices[static_cast<size_t>(deviceIndex)], DeviceRole::Arc))
        selectArcDevice(deviceIndex);
    else
        selectGridDevice(deviceIndex);
}

void MonomeConnection::selectGridDevice(int deviceIndex)
{
    if (deviceIndex < 0 || deviceIndex >= static_cast<int>(devices.size()))
        return;

    const auto& device = devices[static_cast<size_t>(deviceIndex)];
    if (!deviceMatchesRole(device, DeviceRole::Grid))
        return;

    gridEndpoint.device = device;
    connectEndpoint(DeviceRole::Grid);
}

void MonomeConnection::selectArcDevice(int deviceIndex)
{
    if (deviceIndex < 0 || deviceIndex >= static_cast<int>(devices.size()))
        return;

    const auto& device = devices[static_cast<size_t>(deviceIndex)];
    if (!deviceMatchesRole(device, DeviceRole::Arc))
        return;

    arcEndpoint.device = device;
    connectEndpoint(DeviceRole::Arc);
}

void MonomeConnection::setLED(int x, int y, int state)
{
    if (!gridEndpoint.connected)
        return;
    gridEndpoint.sender.send(juce::OSCMessage(prefixForRole(DeviceRole::Grid) + "/grid/led/set", x, y, state));
}

void MonomeConnection::setAllLEDs(int state)
{
    if (!gridEndpoint.connected)
        return;
    gridEndpoint.sender.send(juce::OSCMessage(prefixForRole(DeviceRole::Grid) + "/grid/led/all", state));
}

void MonomeConnection::setLEDRow(int xOffset, int y, const std::array<int, 8>& data)
{
    if (!gridEndpoint.connected)
        return;

    juce::OSCMessage msg(prefixForRole(DeviceRole::Grid) + "/grid/led/row");
    msg.addInt32(xOffset);
    msg.addInt32(y);
    for (int val : data)
        msg.addInt32(val);
    gridEndpoint.sender.send(msg);
}

void MonomeConnection::setLEDColumn(int x, int yOffset, const std::array<int, 8>& data)
{
    if (!gridEndpoint.connected)
        return;

    juce::OSCMessage msg(prefixForRole(DeviceRole::Grid) + "/grid/led/col");
    msg.addInt32(x);
    msg.addInt32(yOffset);
    for (int val : data)
        msg.addInt32(val);
    gridEndpoint.sender.send(msg);
}

void MonomeConnection::setLEDMap(int xOffset, int yOffset, const std::array<int, 8>& data)
{
    if (!gridEndpoint.connected)
        return;

    juce::OSCMessage msg(prefixForRole(DeviceRole::Grid) + "/grid/led/map");
    msg.addInt32(xOffset);
    msg.addInt32(yOffset);
    for (int val : data)
        msg.addInt32(val);
    gridEndpoint.sender.send(msg);
}

void MonomeConnection::setRotation(int degrees)
{
    if (!gridEndpoint.connected)
        return;
    // Only 0, 90, 180, 270 are valid
    int validRotation = ((degrees / 90) * 90) % 360;
    gridEndpoint.sender.send(juce::OSCMessage("/sys/rotation", validRotation));
}

void MonomeConnection::setPrefix(const juce::String& newPrefix)
{
    oscPrefix = newPrefix;
    if (gridEndpoint.connected)
        gridEndpoint.sender.send(juce::OSCMessage("/sys/prefix", prefixForRole(DeviceRole::Grid)));
    if (arcEndpoint.connected)
        arcEndpoint.sender.send(juce::OSCMessage("/sys/prefix", prefixForRole(DeviceRole::Arc)));
}

void MonomeConnection::requestInfo()
{
    sendPing(DeviceRole::Grid);
    sendPing(DeviceRole::Arc);
}

void MonomeConnection::requestSize()
{
    if (!gridEndpoint.connected)
        return;
    gridEndpoint.sender.send(juce::OSCMessage("/sys/size"));
}

// Variable brightness LED control (0-15 levels)
void MonomeConnection::setLEDLevel(int x, int y, int level)
{
    if (!gridEndpoint.connected)
        return;
    int clampedLevel = juce::jlimit(0, 15, level);
    gridEndpoint.sender.send(juce::OSCMessage(prefixForRole(DeviceRole::Grid) + "/grid/led/level/set", x, y, clampedLevel));
}

void MonomeConnection::setAllLEDLevels(int level)
{
    if (!gridEndpoint.connected)
        return;
    int clampedLevel = juce::jlimit(0, 15, level);
    gridEndpoint.sender.send(juce::OSCMessage(prefixForRole(DeviceRole::Grid) + "/grid/led/level/all", clampedLevel));
}

void MonomeConnection::setLEDLevelRow(int xOffset, int y, const std::array<int, 8>& levels)
{
    if (!gridEndpoint.connected)
        return;

    juce::OSCMessage msg(prefixForRole(DeviceRole::Grid) + "/grid/led/level/row");
    msg.addInt32(xOffset);
    msg.addInt32(y);
    for (int level : levels)
    {
        int clampedLevel = juce::jlimit(0, 15, level);
        msg.addInt32(clampedLevel);
    }
    gridEndpoint.sender.send(msg);
}

void MonomeConnection::setLEDLevelColumn(int x, int yOffset, const std::array<int, 8>& levels)
{
    if (!gridEndpoint.connected)
        return;

    juce::OSCMessage msg(prefixForRole(DeviceRole::Grid) + "/grid/led/level/col");
    msg.addInt32(x);
    msg.addInt32(yOffset);
    for (int level : levels)
    {
        int clampedLevel = juce::jlimit(0, 15, level);
        msg.addInt32(clampedLevel);
    }
    gridEndpoint.sender.send(msg);
}

void MonomeConnection::setLEDLevelMap(int xOffset, int yOffset, const std::array<int, 64>& levels)
{
    if (!gridEndpoint.connected)
        return;

    juce::OSCMessage msg(prefixForRole(DeviceRole::Grid) + "/grid/led/level/map");
    msg.addInt32(xOffset);
    msg.addInt32(yOffset);
    for (int level : levels)
    {
        int clampedLevel = juce::jlimit(0, 15, level);
        msg.addInt32(clampedLevel);
    }
    gridEndpoint.sender.send(msg);
}

void MonomeConnection::setArcRingMap(int encoder, const std::array<int, 64>& levels)
{
    if (!arcEndpoint.connected)
        return;

    const int maxEncoders = juce::jmax(1, getArcEncoderCount());
    const int clampedEncoder = juce::jlimit(0, maxEncoders - 1, encoder);

    juce::OSCMessage msg(prefixForRole(DeviceRole::Arc) + "/ring/map");
    msg.addInt32(clampedEncoder);
    for (int level : levels)
        msg.addInt32(juce::jlimit(0, 15, level));
    arcEndpoint.sender.send(msg);
}

void MonomeConnection::setArcRingLevel(int encoder, int ledIndex, int level)
{
    if (!arcEndpoint.connected)
        return;

    const int maxEncoders = juce::jmax(1, getArcEncoderCount());
    const int clampedEncoder = juce::jlimit(0, maxEncoders - 1, encoder);
    const int clampedLed = juce::jlimit(0, 63, ledIndex);
    const int clampedLevel = juce::jlimit(0, 15, level);
    arcEndpoint.sender.send(juce::OSCMessage(prefixForRole(DeviceRole::Arc) + "/ring/set", clampedEncoder, clampedLed, clampedLevel));
}

void MonomeConnection::setArcRingRange(int encoder, int start, int end, int level)
{
    if (!arcEndpoint.connected)
        return;

    const int maxEncoders = juce::jmax(1, getArcEncoderCount());
    const int clampedEncoder = juce::jlimit(0, maxEncoders - 1, encoder);
    const int clampedStart = juce::jlimit(0, 63, start);
    const int clampedEnd = juce::jlimit(0, 63, end);
    const int clampedLevel = juce::jlimit(0, 15, level);
    arcEndpoint.sender.send(juce::OSCMessage(prefixForRole(DeviceRole::Arc) + "/ring/range", clampedEncoder, clampedStart, clampedEnd, clampedLevel));
}

bool MonomeConnection::supportsGrid() const
{
    return gridEndpoint.connected;
}

bool MonomeConnection::supportsArc() const
{
    return arcEndpoint.connected;
}

int MonomeConnection::getArcEncoderCount() const
{
    if (!arcEndpoint.connected)
        return 0;
    if (arcEndpoint.device.type.contains("2"))
        return 2;
    if (arcEndpoint.device.type.contains("4"))
        return 4;
    return 4;
}

// Tilt support
void MonomeConnection::enableTilt(int sensor, bool enable)
{
    if (!gridEndpoint.connected)
        return;
    gridEndpoint.sender.send(juce::OSCMessage(prefixForRole(DeviceRole::Grid) + "/tilt/set", sensor, enable ? 1 : 0));
}

// Connection status
juce::String MonomeConnection::getConnectionStatus() const
{
    return getGridConnectionStatus() + " | " + getArcConnectionStatus();
}

juce::String MonomeConnection::getGridConnectionStatus() const
{
    if (gridEndpoint.connected)
    {
        return "Grid: " + gridEndpoint.device.id + " (" + gridEndpoint.device.type + ") - "
            + juce::String(gridEndpoint.device.sizeX) + "x" + juce::String(gridEndpoint.device.sizeY);
    }

    if (gridEndpoint.device.id.isNotEmpty())
        return "Grid: " + gridEndpoint.device.id + " (disconnected)";

    return "Grid: not connected";
}

juce::String MonomeConnection::getArcConnectionStatus() const
{
    if (arcEndpoint.connected)
    {
        return "Arc: " + arcEndpoint.device.id + " (" + arcEndpoint.device.type + ")";
    }

    if (arcEndpoint.device.id.isNotEmpty())
        return "Arc: " + arcEndpoint.device.id + " (disconnected)";

    return "Arc: not connected";
}

void MonomeConnection::oscMessageReceived(const juce::OSCMessage& message)
{
    auto address = message.getAddressPattern().toString();

    if (address.startsWith("/serialosc"))
        handleSerialOSCMessage(message);
    else if (address.startsWith(prefixForRole(DeviceRole::Grid) + "/grid"))
        handleGridMessage(message);
    else if (address.startsWith(prefixForRole(DeviceRole::Grid) + "/tilt"))
        handleTiltMessage(message);
    else if (address.startsWith(prefixForRole(DeviceRole::Arc) + "/enc"))
        handleArcMessage(message);
    else if (address.startsWith("/sys"))
        handleSystemMessage(message);
}

void MonomeConnection::timerCallback()
{
    const auto currentTime = juce::Time::currentTimeMillis();

    if (currentTime - lastDiscoveryTime >= discoveryIntervalMs)
        discoverDevices();

    for (const auto role : { DeviceRole::Grid, DeviceRole::Arc })
    {
        auto& endpoint = endpointForRole(role);

        if (!endpoint.connected
            && autoReconnect
            && endpoint.device.id.isNotEmpty()
            && endpoint.device.port > 0
            && endpoint.reconnectAttempts < maxReconnectAttempts
            && (currentTime - endpoint.lastConnectAttemptTime) >= reconnectIntervalMs)
        {
            attemptReconnection(role);
        }

        if (endpoint.connected
            && (endpoint.lastPingTime == 0 || (currentTime - endpoint.lastPingTime) >= pingIntervalMs))
        {
            sendPing(role);
            endpoint.lastPingTime = currentTime;
        }
    }
}

void MonomeConnection::handleSerialOSCMessage(const juce::OSCMessage& message)
{
    auto address = message.getAddressPattern().toString();
    auto renewNotify = [this]()
    {
        if (!serialoscSender.connect("127.0.0.1", 12002))
            return;
        serialoscSender.send(juce::OSCMessage("/serialosc/notify",
                                              juce::String("127.0.0.1"),
                                              applicationPort));
    };

    if (address == "/serialosc/device" && message.size() >= 3)
    {
        DeviceInfo info;
        info.id = message[0].getString();
        info.type = message[1].getString();
        info.port = message[2].getInt32();
        info.host = "127.0.0.1"; // Default to localhost

        // Check if device already exists in list
        bool deviceExists = false;
        bool endpointChanged = false;
        for (auto& existing : devices)
        {
            if (existing.id == info.id)
            {
                deviceExists = true;
                if (existing.port != info.port || existing.type != info.type || existing.host != info.host)
                {
                    existing.type = info.type;
                    existing.port = info.port;
                    existing.host = info.host;
                    endpointChanged = true;
                }
                break;
            }
        }

        if (!deviceExists)
        {
            devices.push_back(info);
        }

        if (!deviceExists || endpointChanged)
        {
            if (onDeviceListUpdated)
                onDeviceListUpdated(devices);
        }

        for (const auto role : { DeviceRole::Grid, DeviceRole::Arc })
        {
            auto& endpoint = endpointForRole(role);
            if (endpoint.device.id == info.id)
            {
                endpoint.device = info;
                if (!endpoint.connected || endpointChanged)
                    connectEndpoint(role);
            }
        }

        autoSelectAvailableDevices();
    }
    else if (address == "/serialosc/add" && message.size() >= 1)
    {
        // serialosc notify is one-shot; re-register each time we get add/remove.
        renewNotify();

        // Device was plugged in
        juce::Timer::callAfterDelay(250, [this]()
        {
            discoverDevices(); // Refresh device list
        });
    }
    else if (address == "/serialosc/remove" && message.size() >= 1)
    {
        // serialosc notify is one-shot; re-register each time we get add/remove.
        renewNotify();

        // Device was unplugged
        auto removedId = message[0].getString();

        // Remove from device list
        devices.erase(std::remove_if(devices.begin(), devices.end(),
            [&removedId](const DeviceInfo& info) { return info.id == removedId; }),
            devices.end());

        if (removedId == gridEndpoint.device.id)
            markDisconnected(DeviceRole::Grid);
        if (removedId == arcEndpoint.device.id)
            markDisconnected(DeviceRole::Arc);

        if (onDeviceListUpdated)
            onDeviceListUpdated(devices);

        autoSelectAvailableDevices();
    }
}

void MonomeConnection::markDisconnected(DeviceRole role)
{
    auto& endpoint = endpointForRole(role);
    const bool wasConnected = endpoint.connected;
    endpoint.connected = false;
    endpoint.sender.disconnect();
    endpoint.lastPingTime = 0;
    endpoint.lastConnectAttemptTime = juce::Time::currentTimeMillis();

    if (wasConnected && onDeviceDisconnected)
        onDeviceDisconnected();
}

MonomeConnection::EndpointState& MonomeConnection::endpointForRole(DeviceRole role)
{
    return role == DeviceRole::Grid ? gridEndpoint : arcEndpoint;
}

const MonomeConnection::EndpointState& MonomeConnection::endpointForRole(DeviceRole role) const
{
    return role == DeviceRole::Grid ? gridEndpoint : arcEndpoint;
}

bool MonomeConnection::deviceMatchesRole(const DeviceInfo& device, DeviceRole role) const
{
    const bool isArc = device.type.containsIgnoreCase("arc");
    return role == DeviceRole::Arc ? isArc : !isArc;
}

juce::String MonomeConnection::prefixForRole(DeviceRole role) const
{
    return oscPrefix + (role == DeviceRole::Grid ? "-grid" : "-arc");
}

void MonomeConnection::configureEndpoint(DeviceRole role)
{
    auto& endpoint = endpointForRole(role);
    if (!endpoint.connected)
        return;

    endpoint.sender.send(juce::OSCMessage("/sys/port", applicationPort));
    endpoint.sender.send(juce::OSCMessage("/sys/host", juce::String("127.0.0.1")));
    endpoint.sender.send(juce::OSCMessage("/sys/prefix", prefixForRole(role)));
    if (role == DeviceRole::Grid)
        endpoint.sender.send(juce::OSCMessage("/sys/size"));
}

void MonomeConnection::connectEndpoint(DeviceRole role)
{
    auto& endpoint = endpointForRole(role);
    if (endpoint.device.id.isEmpty() || endpoint.device.port <= 0)
        return;

    endpoint.sender.disconnect();
    endpoint.connected = false;
    endpoint.lastConnectAttemptTime = juce::Time::currentTimeMillis();

    if (!endpoint.sender.connect(endpoint.device.host, endpoint.device.port))
    {
        ++endpoint.reconnectAttempts;
        return;
    }

    endpoint.connected = true;
    endpoint.reconnectAttempts = 0;
    endpoint.lastPingTime = 0;
    configureEndpoint(role);

    if (role == DeviceRole::Grid)
    {
        setAllLEDs(0);
        setAllLEDLevels(0);
    }

    sendPing(role);
    endpoint.lastPingTime = juce::Time::currentTimeMillis();

    if (onDeviceConnected)
        onDeviceConnected();
}

void MonomeConnection::attemptReconnection(DeviceRole role)
{
    auto& endpoint = endpointForRole(role);
    ++endpoint.reconnectAttempts;
    connectEndpoint(role);
}

void MonomeConnection::sendPing(DeviceRole role)
{
    auto& endpoint = endpointForRole(role);
    if (!endpoint.connected)
        return;

    endpoint.sender.send(juce::OSCMessage("/sys/info", juce::String("127.0.0.1"), applicationPort));
}

void MonomeConnection::autoSelectAvailableDevices()
{
    auto roleNeedsSelection = [this](DeviceRole role)
    {
        const auto& endpoint = endpointForRole(role);
        if (endpoint.connected)
            return false;

        if (endpoint.device.id.isEmpty())
            return true;

        return std::none_of(devices.begin(), devices.end(),
                            [this, role, &endpoint](const DeviceInfo& device)
                            {
                                return deviceMatchesRole(device, role) && device.id == endpoint.device.id;
                            });
    };

    if (roleNeedsSelection(DeviceRole::Grid))
    {
        for (int i = 0; i < static_cast<int>(devices.size()); ++i)
        {
            if (deviceMatchesRole(devices[static_cast<size_t>(i)], DeviceRole::Grid))
            {
                selectGridDevice(i);
                break;
            }
        }
    }

    if (roleNeedsSelection(DeviceRole::Arc))
    {
        for (int i = 0; i < static_cast<int>(devices.size()); ++i)
        {
            if (deviceMatchesRole(devices[static_cast<size_t>(i)], DeviceRole::Arc))
            {
                selectArcDevice(i);
                break;
            }
        }
    }
}

void MonomeConnection::handleGridMessage(const juce::OSCMessage& message)
{
    auto address = message.getAddressPattern().toString();

    if (address == prefixForRole(DeviceRole::Grid) + "/grid/key" && message.size() >= 3)
    {
        int x = message[0].getInt32();
        int y = message[1].getInt32();
        int state = message[2].getInt32();

        if (onKeyPress)
            onKeyPress(x, y, state);
    }
}

void MonomeConnection::handleSystemMessage(const juce::OSCMessage& message)
{
    auto address = message.getAddressPattern().toString();

    if (address == "/sys/size" && message.size() >= 2)
    {
        gridEndpoint.device.sizeX = message[0].getInt32();
        gridEndpoint.device.sizeY = message[1].getInt32();
    }
}

void MonomeConnection::handleTiltMessage(const juce::OSCMessage& message)
{
    auto address = message.getAddressPattern().toString();

    if (address == prefixForRole(DeviceRole::Grid) + "/tilt" && message.size() >= 4)
    {
        int sensor = message[0].getInt32();
        int x = message[1].getInt32();
        int y = message[2].getInt32();
        int z = message[3].getInt32();

        if (onTilt)
            onTilt(sensor, x, y, z);
    }
}

void MonomeConnection::handleArcMessage(const juce::OSCMessage& message)
{
    auto address = message.getAddressPattern().toString();

    if (address == prefixForRole(DeviceRole::Arc) + "/enc/delta" && message.size() >= 2)
    {
        const int encoder = message[0].getInt32();
        const int delta = message[1].getInt32();
        if (onArcDelta)
            onArcDelta(encoder, delta);
    }
    else if (address == prefixForRole(DeviceRole::Arc) + "/enc/key" && message.size() >= 2)
    {
        const int encoder = message[0].getInt32();
        const int state = message[1].getInt32();
        if (onArcKey)
            onArcKey(encoder, state);
    }
}

//==============================================================================
// MlrVSTAudioProcessor Implementation
//==============================================================================


class MlrVSTAudioProcessor::LoopPitchAnalysisJob final : public juce::ThreadPoolJob
{
public:
    LoopPitchAnalysisJob(MlrVSTAudioProcessor& ownerIn,
                         int stripIndexIn,
                         int requestIdIn,
                         juce::AudioBuffer<float> audioBufferIn,
                         double sourceSampleRateIn,
                         juce::File sourceFileIn,
                         bool setAsRootIn)
        : juce::ThreadPoolJob("mlrVSTLoopPitch_" + juce::String(stripIndexIn + 1)),
          owner(ownerIn),
          stripIndex(stripIndexIn),
          requestId(requestIdIn),
          audioBuffer(std::move(audioBufferIn)),
          sourceSampleRate(sourceSampleRateIn),
          sourceFile(std::move(sourceFileIn)),
          setAsRoot(setAsRootIn)
    {
    }

    JobStatus runJob() override
    {
        MlrVSTAudioProcessor::LoopPitchAnalysisResult result;
        result.stripIndex = stripIndex;
        result.requestId = requestId;
        result.setAsRoot = setAsRoot;

        if (shouldExit() || audioBuffer.getNumSamples() <= 0)
        {
            owner.resetLoopPitchAnalysisProgress(stripIndex);
            owner.queueLoopPitchAnalysisResult(std::move(result));
            return jobHasFinished;
        }

        const auto summary = SampleAnalysisEngine().analyzeLoadedSample(sourceFile,
                                                                        audioBuffer,
                                                                        sourceSampleRate,
                                                                        SamplePitchAnalysisProfile::Monophonic,
                                                                        [this](float progress, const juce::String& statusText)
                                                                        {
                                                                            owner.updateLoopPitchAnalysisProgress(stripIndex,
                                                                                                                  requestId,
                                                                                                                  progress,
                                                                                                                  statusText);
                                                                        });
        result.success = summary.estimatedPitchMidi >= 0;
        result.detectedMidi = summary.estimatedPitchMidi;
        result.detectedHz = summary.estimatedPitchHz;
        result.detectedPitchConfidence = summary.estimatedPitchConfidence;
        result.detectedScaleIndex = summary.estimatedScaleIndex;
        result.detectedScaleConfidence = summary.estimatedScaleConfidence;
        result.essentiaUsed = summary.essentiaUsed;
        result.analysisSource = summary.analysisSource;
        owner.queueLoopPitchAnalysisResult(std::move(result));
        return jobHasFinished;
    }

private:
    MlrVSTAudioProcessor& owner;
    int stripIndex = -1;
    int requestId = 0;
    juce::AudioBuffer<float> audioBuffer;
    double sourceSampleRate = 44100.0;
    juce::File sourceFile;
    bool setAsRoot = false;
};

class MlrVSTAudioProcessor::StripOfflineCacheJob final : public juce::ThreadPoolJob
{
public:
    enum class Kind { Swing, TempoMatch };

    StripOfflineCacheJob(MlrVSTAudioProcessor& ownerIn, int stripIndexIn, Kind kindIn)
        : juce::ThreadPoolJob("StripOfflineCache"),
          owner(ownerIn),
          stripIndex(stripIndexIn),
          kind(kindIn)
    {
    }

    JobStatus runJob() override
    {
        // Safe to touch the strip directly: the processor destructor joins
        // this pool before the engine is destroyed.
        if (owner.audioEngine != nullptr)
        {
            if (auto* strip = owner.audioEngine->getStrip(stripIndex))
            {
#if MLRVST_ENABLE_SOUNDTOUCH || MLRVST_ENABLE_BUNGEE
                if (kind == Kind::Swing)
                    strip->renderSoundTouchSwingCacheOffline();
#endif
#if MLRVST_ENABLE_BUNGEE
                if (kind == Kind::TempoMatch)
                    strip->renderLoopTempoMatchCacheOffline();
#endif
            }
        }
        return jobHasFinished;
    }

private:
    MlrVSTAudioProcessor& owner;
    int stripIndex;
    Kind kind;
};

class MlrVSTAudioProcessor::SoundTouchPitchCacheJob final : public juce::ThreadPoolJob
{
public:
    SoundTouchPitchCacheJob(MlrVSTAudioProcessor& ownerIn,
                            int stripIndexIn,
                            int requestIdIn,
                            juce::AudioBuffer<float> sourceBufferIn,
                            double sourceSampleRateIn,
                            uint64_t sourceVersionIn,
                            float semitonesIn)
        : juce::ThreadPoolJob("mlrVSTSoundTouchPitch_" + juce::String(stripIndexIn + 1)),
          owner(ownerIn),
          stripIndex(stripIndexIn),
          requestId(requestIdIn),
          sourceBuffer(std::move(sourceBufferIn)),
          sourceSampleRate(sourceSampleRateIn),
          sourceVersion(sourceVersionIn),
          semitones(semitonesIn)
    {
    }

    JobStatus runJob() override
    {
        SoundTouchPitchCacheResult result;
        result.stripIndex = stripIndex;
        result.requestId = requestId;
        result.semitones = semitones;
        result.sourceSampleRate = sourceSampleRate;
        result.sourceVersion = sourceVersion;

        if (shouldExit() || sourceBuffer.getNumSamples() <= 0 || !(sourceSampleRate > 0.0))
        {
            owner.queueSoundTouchPitchCacheResult(std::move(result));
            return jobHasFinished;
        }

        result.success = renderTimeStretchedBuffer(sourceBuffer,
                                                   sourceSampleRate,
                                                   sourceBuffer.getNumSamples(),
                                                   semitones,
                                                   TimeStretchBackend::SoundTouch,
                                                   result.renderedBuffer);
        owner.queueSoundTouchPitchCacheResult(std::move(result));
        return jobHasFinished;
    }

private:
    MlrVSTAudioProcessor& owner;
    int stripIndex = -1;
    int requestId = 0;
    juce::AudioBuffer<float> sourceBuffer;
    double sourceSampleRate = 44100.0;
    uint64_t sourceVersion = 0;
    float semitones = 0.0f;
};

class MlrVSTAudioProcessor::BungeePitchCacheJob final : public juce::ThreadPoolJob
{
public:
    BungeePitchCacheJob(MlrVSTAudioProcessor& ownerIn,
                        int stripIndexIn,
                        int requestIdIn,
                        juce::AudioBuffer<float> sourceBufferIn,
                        double sourceSampleRateIn,
                        uint64_t sourceVersionIn,
                        float semitonesIn)
        : juce::ThreadPoolJob("mlrVSTBungeePitch_" + juce::String(stripIndexIn + 1)),
          owner(ownerIn),
          stripIndex(stripIndexIn),
          requestId(requestIdIn),
          sourceBuffer(std::move(sourceBufferIn)),
          sourceSampleRate(sourceSampleRateIn),
          sourceVersion(sourceVersionIn),
          semitones(semitonesIn)
    {
    }

    JobStatus runJob() override
    {
        BungeePitchCacheResult result;
        result.stripIndex = stripIndex;
        result.requestId = requestId;
        result.semitones = semitones;
        result.sourceSampleRate = sourceSampleRate;
        result.sourceVersion = sourceVersion;

        if (shouldExit() || sourceBuffer.getNumSamples() <= 0 || !(sourceSampleRate > 0.0))
        {
            owner.queueBungeePitchCacheResult(std::move(result));
            return jobHasFinished;
        }

        result.success = renderTimeStretchedBuffer(sourceBuffer,
                                                   sourceSampleRate,
                                                   sourceBuffer.getNumSamples(),
                                                   semitones,
                                                   TimeStretchBackend::Bungee,
                                                   result.renderedBuffer);
        owner.queueBungeePitchCacheResult(std::move(result));
        return jobHasFinished;
    }

private:
    MlrVSTAudioProcessor& owner;
    int stripIndex = -1;
    int requestId = 0;
    juce::AudioBuffer<float> sourceBuffer;
    double sourceSampleRate = 44100.0;
    uint64_t sourceVersion = 0;
    float semitones = 0.0f;
};

class MlrVSTAudioProcessor::SignalsmithPitchCacheJob final : public juce::ThreadPoolJob
{
public:
    SignalsmithPitchCacheJob(MlrVSTAudioProcessor& ownerIn,
                             int stripIndexIn,
                             int requestIdIn,
                             juce::AudioBuffer<float> sourceBufferIn,
                             double sourceSampleRateIn,
                             uint64_t sourceVersionIn,
                             float semitonesIn)
        : juce::ThreadPoolJob("mlrVSTSignalsmithPitch_" + juce::String(stripIndexIn + 1)),
          owner(ownerIn),
          stripIndex(stripIndexIn),
          requestId(requestIdIn),
          sourceBuffer(std::move(sourceBufferIn)),
          sourceSampleRate(sourceSampleRateIn),
          sourceVersion(sourceVersionIn),
          semitones(semitonesIn)
    {
    }

    JobStatus runJob() override
    {
        SignalsmithPitchCacheResult result;
        result.stripIndex = stripIndex;
        result.requestId = requestId;
        result.semitones = semitones;
        result.sourceSampleRate = sourceSampleRate;
        result.sourceVersion = sourceVersion;

        if (shouldExit() || sourceBuffer.getNumSamples() <= 0 || !(sourceSampleRate > 0.0))
        {
            owner.queueSignalsmithPitchCacheResult(std::move(result));
            return jobHasFinished;
        }

        result.success = renderSignalsmithPitchBuffer(sourceBuffer,
                                                      sourceSampleRate,
                                                      semitones,
                                                      result.renderedBuffer);
        owner.queueSignalsmithPitchCacheResult(std::move(result));
        return jobHasFinished;
    }

private:
    MlrVSTAudioProcessor& owner;
    int stripIndex = -1;
    int requestId = 0;
    juce::AudioBuffer<float> sourceBuffer;
    double sourceSampleRate = 44100.0;
    uint64_t sourceVersion = 0;
    float semitones = 0.0f;
};

MlrVSTAudioProcessor::MlrVSTAudioProcessor()
     : AudioProcessor(BusesProperties()
                      .withInput("Input", juce::AudioChannelSet::stereo(), true)
                      .withOutput("Strip 1", juce::AudioChannelSet::stereo(), true)
                      .withOutput("Strip 2", juce::AudioChannelSet::stereo(), false)
                      .withOutput("Strip 3", juce::AudioChannelSet::stereo(), false)
                      .withOutput("Strip 4", juce::AudioChannelSet::stereo(), false)
                      .withOutput("Strip 5", juce::AudioChannelSet::stereo(), false)
                      .withOutput("Strip 6", juce::AudioChannelSet::stereo(), false)),
       parameters(*this, nullptr, juce::Identifier("MlrVST"), createParameterLayout())
{
    // Initialize audio engine
    audioEngine = std::make_unique<ModernAudioEngine>();
    gestureCoordinator = std::make_unique<GestureCoordinator>(
        [this]()
        {
            markPersistentGlobalUserChange();
        });
    if (gestureCoordinator)
        gestureCoordinator->attachAudioEngine(audioEngine.get());
    configureAudioEngineCallbacks(*audioEngine);
    for (int i = 0; i < MacroCount; ++i)
    {
        macroMidiCcAssignments[static_cast<size_t>(i)].store(getDefaultMacroMidiCc(i), std::memory_order_release);
        macroTargetAssignments[static_cast<size_t>(i)].store(static_cast<int>(getDefaultMacroTarget(i)),
                                                             std::memory_order_release);
    }
    cacheParameterPointers();
    sceneModeEnabled.store(sceneModeParam != nullptr && sceneModeParam->load(std::memory_order_acquire) > 0.5f ? 1 : 0,
                           std::memory_order_release);
    if (audioEngine)
        audioEngine->setPatternRecorderIgnoreGroups(false);
    sceneModeGroupSnapshot.stripGroups.fill(-1);
    sceneModeGroupSnapshot.groupVolumes.fill(1.0f);
    sceneModeGroupSnapshot.groupMuted.fill(false);
    sceneRepeatCounts.fill(1);
    sceneLengthModes.fill(static_cast<int>(SceneLengthMode::ManualBars));
    sceneManualBars.fill(4);
    sceneAnchorStrips.fill(0);
    sceneEditorStripAutomationExpanded.fill(true);
    sceneEditorStripHeightExpanded.fill(false);
    GlobalSettingsStore::loadDefaultPaths(*this);
    GlobalSettingsStore::loadControlPages(*this);
    GlobalSettingsStore::loadGlobalControls(*this);
    persistentGlobalControlsReady.store(1, std::memory_order_release);
    pendingPersistentGlobalControlsRestore.store(1, std::memory_order_release);
    pendingPersistentGlobalControlsRestoreMs = juce::Time::currentTimeMillis() + 250;
    pendingPersistentGlobalControlsRestoreRemaining = 5;
    for (const auto* id : kPersistentGlobalControlParameterIds)
        parameters.addParameterListener(id, this);
    forEachSceneAutosaveParameterId([this](const juce::String& id)
    {
        parameters.addParameterListener(id, this);
    });
    setSwingDivisionSelection(swingDivisionSelection.load(std::memory_order_acquire));
    resetStepEditVelocityGestures();

    for (auto& held : arcKeyHeld)
        held = 0;
    for (auto& heldSlot : sampleModeHeldVisibleSliceSlots)
        heldSlot.store(-1, std::memory_order_release);
    for (auto& inFlight : loopStripLoadInFlight)
        inFlight.store(0, std::memory_order_release);
    for (auto& requestId : loopStripLoadRequestIds)
        requestId.store(0, std::memory_order_release);
    for (auto& progress : loopStripLoadProgressPermille)
        progress.store(0, std::memory_order_release);
    for (auto& inFlight : loopPitchAnalysisInFlight)
        inFlight.store(0, std::memory_order_release);
    for (auto& requestId : loopPitchAnalysisRequestIds)
        requestId.store(0, std::memory_order_release);
    for (auto& progress : loopPitchAnalysisProgressPermille)
        progress.store(0, std::memory_order_release);
    for (auto& detectedMidi : loopPitchDetectedMidi)
        detectedMidi.store(-1, std::memory_order_release);
    for (auto& detectedHz : loopPitchDetectedHz)
        detectedHz.store(0.0f, std::memory_order_release);
    for (auto& detectedPitchConfidence : loopPitchDetectedPitchConfidence)
        detectedPitchConfidence.store(0.0f, std::memory_order_release);
    for (auto& detectedScale : loopPitchDetectedScaleIndices)
        detectedScale.store(-1, std::memory_order_release);
    for (auto& detectedScaleConfidence : loopPitchDetectedScaleConfidence)
        detectedScaleConfidence.store(0.0f, std::memory_order_release);
    for (auto& essentiaUsed : loopPitchEssentiaUsed)
        essentiaUsed.store(0, std::memory_order_release);
    for (auto& role : loopPitchRoles)
        role.store(static_cast<int>(LoopPitchRole::None), std::memory_order_release);
    for (auto& timing : loopPitchSyncTimings)
        timing.store(static_cast<int>(LoopPitchSyncTiming::Immediate), std::memory_order_release);
    for (auto& assignedMidi : loopPitchAssignedMidi)
        assignedMidi.store(-1, std::memory_order_release);
    for (auto& assignedManual : loopPitchAssignedManual)
        assignedManual.store(0, std::memory_order_release);
    for (auto& pendingRetune : loopPitchPendingRetune)
        pendingRetune.store(0, std::memory_order_release);
    loopStripLoadStatusTexts.fill({});
    loopPitchAnalysisStatusTexts.fill({});
    loopPitchLastObservedColumns.fill(-1);
    loopPitchLastObservedHostBar = -1;
    for (auto& ring : arcRingCache)
        ring.fill(-1);
    arcControlMode = ArcControlMode::SelectedStrip;
    lastGridLedUpdateTimeMs = 0;

    // Setup monome callbacks
    monomeConnection.onKeyPress = [this](int x, int y, int state)
    {
        handleMonomeKeyPress(x, y, state);
    };
    monomeConnection.onArcDelta = [this](int encoder, int delta)
    {
        handleMonomeArcDelta(encoder, delta);
    };
    monomeConnection.onArcKey = [this](int encoder, int state)
    {
        handleMonomeArcKey(encoder, state);
    };

    monomeConnection.onDeviceConnected = [this]()
    {
        if (isTimerRunning())
            startTimer(monomeConnection.supportsArc() ? kArcRefreshMs : kGridRefreshMs);

        if (monomeConnection.supportsGrid())
        {
            monomeConnection.setAllLEDs(0);
            monomeConnection.setAllLEDLevels(0);

            // Force full LED resend after any reconnect to avoid stale cache mismatch.
            for (int y = 0; y < MaxGridHeight; ++y)
                for (int x = 0; x < MaxGridWidth; ++x)
                    ledCache[x][y] = -1;
        }

        for (auto& held : arcKeyHeld)
            held = 0;
        for (auto& ring : arcRingCache)
            ring.fill(-1);
        arcControlMode = ArcControlMode::SelectedStrip;
        arcSelectedModStep = 0;
        lastGridLedUpdateTimeMs = 0;

        // Defer LED update slightly to ensure everything is ready
        juce::MessageManager::callAsync([this]()
        {
            if (monomeConnection.supportsGrid())
                updateMonomeLEDs();
            if (monomeConnection.supportsArc())
                updateMonomeArcRings();
        });
    };

    monomeConnection.onDeviceDisconnected = [this]()
    {
        if (isTimerRunning())
            startTimer(kGridRefreshMs);
    };

    resetRuntimePresetStateToDefaults();
    loadedPresetIndex = -1;
    juce::ignoreUnused(ensureSceneSlotFallbackState(getActiveMainPresetIndexForScenes(), getActiveSceneSlot()));
    updateMonomeLEDs();

    // Don't connect yet - wait for prepareToPlay
}

void MlrVSTAudioProcessor::configureAudioEngineCallbacks(ModernAudioEngine& engine)
{
    engine.setSampleModeRenderCallback(
        [this](int stripIndex,
               juce::AudioBuffer<float>& output,
               int startSample,
               int numSamples,
               const juce::AudioPlayHead::PositionInfo& positionInfo,
               int64_t globalSampleStart,
               double tempo,
               double quantizeBeats)
        {
            renderSampleModeStrip(stripIndex,
                                  output,
                                  startSample,
                                  numSamples,
                                  positionInfo,
                                  globalSampleStart,
                                  tempo,
                                  quantizeBeats);
        });
    engine.setSampleModeTriggerCallback(
        [this](int stripIndex,
               int column,
               int sampleSliceId,
               int64_t sampleStartSample,
               int64_t triggerSample,
               const juce::AudioPlayHead::PositionInfo& positionInfo,
               bool isMomentaryStutter)
        {
            triggerSampleModeStripAtSample(stripIndex,
                                           column,
                                           sampleSliceId,
                                           sampleStartSample,
                                           triggerSample,
                                           positionInfo,
                                           isMomentaryStutter);
        });
    engine.setSampleModeStopCallback(
        [this](int stripIndex, bool immediateStop)
        {
            // Fire-loop-invoked stop: erase-only trigger clear, so a
            // legitimate replayed re-press later in the same block still
            // fires (the generation bump is reserved for live releases).
            stopSampleModeStrip(stripIndex, immediateStop, false);
        });
    engine.setPatternControlEventCallback(
        [this](const PatternRecorder::Event& event)
        {
            playbackMonomeControlPatternEvent(event);
        });
}

bool MlrVSTAudioProcessor::loadSampleFileIntoSampleModeEngine(SampleModeEngine& engine,
                                                              const juce::File& file) const
{
    if (!safeFileExistsAsFile(file))
        return false;

    juce::AudioFormatManager formatManager;
    formatManager.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(file));
    if (reader == nullptr || reader->lengthInSamples <= 0)
        return false;

    const int numSamples = juce::jlimit<int>(1,
                                             std::numeric_limits<int>::max(),
                                             static_cast<int>(reader->lengthInSamples));
    const int sourceChannels = juce::jmax(1, static_cast<int>(reader->numChannels));
    const int decodeChannels = juce::jmin(2, sourceChannels);
    juce::AudioBuffer<float> sourceBuffer(decodeChannels, numSamples);
    sourceBuffer.clear();
    if (!reader->read(&sourceBuffer, 0, numSamples, 0, true, decodeChannels > 1))
        return false;

    juce::AudioBuffer<float> stereoBuffer(2, numSamples);
    if (decodeChannels == 1)
    {
        stereoBuffer.copyFrom(0, 0, sourceBuffer, 0, 0, numSamples);
        stereoBuffer.copyFrom(1, 0, sourceBuffer, 0, 0, numSamples);
    }
    else
    {
        stereoBuffer.copyFrom(0, 0, sourceBuffer, 0, 0, numSamples);
        stereoBuffer.copyFrom(1, 0, sourceBuffer, 1, 0, numSamples);
    }

    return engine.loadSampleFromBuffer(stereoBuffer,
                                       reader->sampleRate,
                                       file.getFullPathName(),
                                       file.getFileNameWithoutExtension());
}

bool MlrVSTAudioProcessor::readAudioFileToStereoBuffer(const juce::File& file,
                                                       juce::AudioBuffer<float>& buffer,
                                                       double& sourceRate) const
{
    buffer.setSize(0, 0);
    sourceRate = 0.0;

    if (!safeFileExistsAsFile(file))
        return false;

    juce::AudioFormatManager formatManager;
    formatManager.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(file));
    if (reader == nullptr || reader->lengthInSamples <= 0)
        return false;

    const int numSamples = juce::jlimit<int>(1,
                                             std::numeric_limits<int>::max(),
                                             static_cast<int>(reader->lengthInSamples));
    const int sourceChannels = juce::jmax(1, static_cast<int>(reader->numChannels));
    const int decodeChannels = juce::jmin(2, sourceChannels);
    juce::AudioBuffer<float> sourceBuffer(decodeChannels, numSamples);
    sourceBuffer.clear();
    if (!reader->read(&sourceBuffer, 0, numSamples, 0, true, decodeChannels > 1))
        return false;

    buffer.setSize(2, numSamples, false, true, true);
    if (decodeChannels == 1)
    {
        buffer.copyFrom(0, 0, sourceBuffer, 0, 0, numSamples);
        buffer.copyFrom(1, 0, sourceBuffer, 0, 0, numSamples);
    }
    else
    {
        buffer.copyFrom(0, 0, sourceBuffer, 0, 0, numSamples);
        buffer.copyFrom(1, 0, sourceBuffer, 1, 0, numSamples);
    }

    sourceRate = reader->sampleRate;
    return true;
}

void MlrVSTAudioProcessor::cacheParameterPointers()
{
    masterVolumeParam = parameters.getRawParameterValue("masterVolume");
    limiterThresholdParam = parameters.getRawParameterValue("limiterThreshold");
    limiterEnabledParam = parameters.getRawParameterValue("limiterEnabled");
    quantizeParam = parameters.getRawParameterValue("quantize");
    innerLoopLengthParam = parameters.getRawParameterValue("innerLoopLength");
    innerLoopLengthSelection.store(innerLoopLengthParam != nullptr
                                       ? juce::jlimit(0, 4, static_cast<int>(innerLoopLengthParam->load(std::memory_order_acquire)))
                                       : 0,
                                   std::memory_order_release);
    lastAppliedInnerLoopLengthSelection.store(innerLoopLengthSelection.load(std::memory_order_acquire),
                                              std::memory_order_release);
    grainQualityParam = parameters.getRawParameterValue("quality");
    pitchSmoothingParam = parameters.getRawParameterValue("pitchSmoothing");
    inputMonitorParam = parameters.getRawParameterValue("inputMonitor");
    crossfadeLengthParam = parameters.getRawParameterValue("crossfadeLength");
    triggerFadeInParam = parameters.getRawParameterValue("triggerFadeIn");
    outputRoutingParam = parameters.getRawParameterValue("outputRouting");
    pitchControlModeParam = parameters.getRawParameterValue("pitchControlMode");
    flipTempoMatchModeParam = parameters.getRawParameterValue("flipTempoMatchMode");
    stretchBackendParam = parameters.getRawParameterValue("stretchBackend");
    continuousTraversalParam = parameters.getRawParameterValue("continuousTraversal");
    soundTouchEnabledParam = parameters.getRawParameterValue("soundTouchEnabled");
    masterDuckTriggerStripParam = parameters.getRawParameterValue("masterDuckTriggerStrip");
    sceneModeParam = parameters.getRawParameterValue("sceneMode");
    sceneRecallModeParam = parameters.getRawParameterValue("sceneRecallMode");
    transientOnsetMethodParam = parameters.getRawParameterValue("transientOnsetMethod");
    transientSensitivityParam = parameters.getRawParameterValue("transientSensitivity");
    transientSnapParam = parameters.getRawParameterValue("transientSnap");
    transientSpacingParam = parameters.getRawParameterValue("transientSpacing");

    for (int i = 0; i < MaxStrips; ++i)
    {
        stripVolumeParams[static_cast<size_t>(i)] = parameters.getRawParameterValue("stripVolume" + juce::String(i));
        stripTrimDbParams[static_cast<size_t>(i)] = parameters.getRawParameterValue("stripTrimDb" + juce::String(i));
        stripPanParams[static_cast<size_t>(i)] = parameters.getRawParameterValue("stripPan" + juce::String(i));
        stripSpeedParams[static_cast<size_t>(i)] = parameters.getRawParameterValue("stripSpeed" + juce::String(i));
        stripPitchParams[static_cast<size_t>(i)] = parameters.getRawParameterValue("stripPitch" + juce::String(i));
        stripSliceLengthParams[static_cast<size_t>(i)] = parameters.getRawParameterValue("stripSliceLength" + juce::String(i));
        stripPitchControlModeParams[static_cast<size_t>(i)] = parameters.getRawParameterValue("stripPitchControlMode" + juce::String(i));
        stripTempoMatchModeParams[static_cast<size_t>(i)] = parameters.getRawParameterValue("stripTempoMatchMode" + juce::String(i));
        stripFilterEnabledParams[static_cast<size_t>(i)] = parameters.getRawParameterValue("stripFilterEnabled" + juce::String(i));
        stripFilterFrequencyParams[static_cast<size_t>(i)] = parameters.getRawParameterValue("stripFilterFrequency" + juce::String(i));
        stripFilterResonanceParams[static_cast<size_t>(i)] = parameters.getRawParameterValue("stripFilterResonance" + juce::String(i));
        stripFilterMorphParams[static_cast<size_t>(i)] = parameters.getRawParameterValue("stripFilterMorph" + juce::String(i));
        stripFilterAlgorithmParams[static_cast<size_t>(i)] = parameters.getRawParameterValue("stripFilterAlgorithm" + juce::String(i));
        stripDuckEnabledParams[static_cast<size_t>(i)] = parameters.getRawParameterValue("stripDuckEnabled" + juce::String(i));
        stripDuckSourceParams[static_cast<size_t>(i)] = parameters.getRawParameterValue("stripDuckSource" + juce::String(i));
        stripDuckThresholdParams[static_cast<size_t>(i)] = parameters.getRawParameterValue("stripDuckThreshold" + juce::String(i));
        stripDuckRatioParams[static_cast<size_t>(i)] = parameters.getRawParameterValue("stripDuckRatio" + juce::String(i));
        stripDuckAttackParams[static_cast<size_t>(i)] = parameters.getRawParameterValue("stripDuckAttack" + juce::String(i));
        stripDuckReleaseParams[static_cast<size_t>(i)] = parameters.getRawParameterValue("stripDuckRelease" + juce::String(i));
        stripDuckGainCompParams[static_cast<size_t>(i)] = parameters.getRawParameterValue("stripDuckGainComp" + juce::String(i));
        stripDuckFollowMasterParams[static_cast<size_t>(i)] = parameters.getRawParameterValue("stripDuckFollowMaster" + juce::String(i));
        stripDelayMixParams[static_cast<size_t>(i)] = parameters.getRawParameterValue("stripDelayMix" + juce::String(i));
        stripDelayTimeParams[static_cast<size_t>(i)] = parameters.getRawParameterValue("stripDelayTime" + juce::String(i));
        stripDelaySyncParams[static_cast<size_t>(i)] = parameters.getRawParameterValue("stripDelaySync" + juce::String(i));
        stripDelayFeedbackParams[static_cast<size_t>(i)] = parameters.getRawParameterValue("stripDelayFeedback" + juce::String(i));
        stripDelayLowCutParams[static_cast<size_t>(i)] = parameters.getRawParameterValue("stripDelayLowCut" + juce::String(i));
        stripDelayHighCutParams[static_cast<size_t>(i)] = parameters.getRawParameterValue("stripDelayHighCut" + juce::String(i));
        stripDelayModeParams[static_cast<size_t>(i)] = parameters.getRawParameterValue("stripDelayMode" + juce::String(i));
    }
}

bool MlrVSTAudioProcessor::getLiveMonomeGestureComboSelection(GestureComboKind& kind,
                                                              int& buttonCount,
                                                              int& comboIndex) const
{
    const auto selectFlatCombo = [&](GestureComboKind comboKind,
                                     int liveButtonCount,
                                     int flatIndex) -> bool
    {
        if (liveButtonCount <= 0 || flatIndex < 0)
            return false;

        kind = comboKind;
        buttonCount = juce::jlimit(1, 3, liveButtonCount);
        comboIndex = flatIndex - getGestureComboFlatOffsetForButtonCount(comboKind, buttonCount);
        comboIndex = juce::jlimit(
            0,
            juce::jmax(0, getGestureComboCountForButtonCount(comboKind, buttonCount) - 1),
            comboIndex);
        return true;
    };

    uint8_t comboMask = static_cast<uint8_t>(momentaryStutterButtonMask.load(std::memory_order_acquire) & 0x7f);
    if (comboMask == 0 && momentaryStutterHoldActive)
        comboMask = stutterButtonBitFromColumn(momentaryStutterActiveDivisionButton);

    if (comboMask != 0
        && selectFlatCombo(GestureComboKind::Stutter,
                           countStutterBits(comboMask),
                           getStutterGestureComboFlatIndexFromMask(comboMask)))
    {
        return true;
    }

    if (audioEngine == nullptr)
        return false;

    const int preferredStrip = juce::jlimit(0, MaxStrips - 1, getLastMonomePressedStripRow());
    const auto readScratchComboFromStrip = [&](int stripIndex) -> bool
    {
        auto* strip = audioEngine->getStrip(stripIndex);
        if (strip == nullptr)
            return false;

        const int grainCount = strip->getGrainGestureComboButtonCount();
        const int grainFlatIndex = strip->getGrainGestureComboFlatIndex();
        if (selectFlatCombo(GestureComboKind::Scratch, grainCount, grainFlatIndex))
            return true;

        const int scratchCount = strip->getScratchGestureComboButtonCount();
        const int scratchFlatIndex = strip->getScratchGestureComboFlatIndex();
        return selectFlatCombo(GestureComboKind::Scratch, scratchCount, scratchFlatIndex);
    };

    if (readScratchComboFromStrip(preferredStrip))
        return true;

    for (int stripIndex = 0; stripIndex < MaxStrips; ++stripIndex)
    {
        if (stripIndex == preferredStrip)
            continue;
        if (readScratchComboFromStrip(stripIndex))
            return true;
    }

    return false;
}

void MlrVSTAudioProcessor::setGatePageMode(GatePageMode mode)
{
    const auto safeMode = static_cast<GatePageMode>(juce::jlimit(
        0,
        static_cast<int>(GatePageMode::Sixteenth),
        static_cast<int>(mode)));
    gatePageMode.store(static_cast<int>(safeMode), std::memory_order_release);

    if (audioEngine == nullptr || safeMode == GatePageMode::Adaptive)
        return;

    for (int stripIndex = 0; stripIndex < MaxStrips; ++stripIndex)
    {
        auto* strip = audioEngine->getStrip(stripIndex);
        if (strip == nullptr || strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Step)
            continue;

        strip->setGateSpeed(gatePageSpeedForMode(safeMode, strip->getBeatsPerLoop()));
    }
}

MlrVSTAudioProcessor::~MlrVSTAudioProcessor()
{
    for (const auto* id : kPersistentGlobalControlParameterIds)
        parameters.removeParameterListener(id, this);
    forEachSceneAutosaveParameterId([this](const juce::String& id)
    {
        parameters.removeParameterListener(id, this);
    });
    if (persistentGlobalControlsDirty.load(std::memory_order_acquire) != 0)
        GlobalSettingsStore::saveControlPages(*this);
    presetSaveThreadPool.removeAllJobs(true, 4000);
    loopStripLoadThreadPool.removeAllJobs(true, 4000);
    loopPitchAnalysisThreadPool.removeAllJobs(true, 4000);
    soundTouchPitchCacheThreadPool.removeAllJobs(true, 4000);
    bungeePitchCacheThreadPool.removeAllJobs(true, 4000);
    signalsmithPitchCacheThreadPool.removeAllJobs(true, 4000);
    flipLegacyLoopRenderThreadPool.removeAllJobs(true, 4000);
    stopTimer();
    monomeConnection.disconnect();
}

MlrVSTAudioProcessor::PitchControlMode MlrVSTAudioProcessor::getPitchControlMode() const
{
    const float rawChoice = (pitchControlModeParam != nullptr)
        ? pitchControlModeParam->load(std::memory_order_acquire)
        : 0.0f;
    const int modeIndex = sanitizePitchControlModeIndex(static_cast<int>(std::round(rawChoice)));
    switch (modeIndex)
    {
        case 1: return PitchControlMode::SoundTouch;
        case 2: return PitchControlMode::Resample;
        case 3: return PitchControlMode::Signalsmith;
        case 4: return PitchControlMode::Bungee;
        case 0:
        default: return PitchControlMode::PitchShift;
    }
}

MlrVSTAudioProcessor::StripPitchControlMode MlrVSTAudioProcessor::getStripPitchControlMode(int stripIndex) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return StripPitchControlMode::Global;

    const auto* param = stripPitchControlModeParams[static_cast<size_t>(stripIndex)];
    const float rawChoice = param != nullptr ? param->load(std::memory_order_acquire) : 0.0f;
    const int modeIndex = sanitizeStripPitchControlModeIndex(static_cast<int>(std::round(rawChoice)));
    switch (modeIndex)
    {
        case 1: return StripPitchControlMode::PitchShift;
        case 2: return StripPitchControlMode::SoundTouch;
        case 3: return StripPitchControlMode::Resample;
        case 4: return StripPitchControlMode::Signalsmith;
        case 5: return StripPitchControlMode::Bungee;
        case 0:
        default: return StripPitchControlMode::Global;
    }
}

MlrVSTAudioProcessor::PitchControlMode MlrVSTAudioProcessor::resolvePitchControlModeForStrip(int stripIndex) const
{
    switch (getStripPitchControlMode(stripIndex))
    {
        case StripPitchControlMode::PitchShift:
            return PitchControlMode::PitchShift;
        case StripPitchControlMode::SoundTouch:
            return PitchControlMode::SoundTouch;
        case StripPitchControlMode::Resample:
            return PitchControlMode::Resample;
        case StripPitchControlMode::Signalsmith:
            return PitchControlMode::Signalsmith;
        case StripPitchControlMode::Bungee:
            return PitchControlMode::Bungee;
        case StripPitchControlMode::Global:
        default:
            return getPitchControlMode();
    }
}

TimeStretchBackend MlrVSTAudioProcessor::getStretchBackend() const
{
    if (stretchBackendParam != nullptr)
    {
        const int backendIndex = static_cast<int>(std::round(
            stretchBackendParam->load(std::memory_order_acquire)));
        return sanitizeTimeStretchBackend(backendIndex);
    }

    const bool legacyEnabled = soundTouchEnabledParam != nullptr
        && soundTouchEnabledParam->load(std::memory_order_acquire) > 0.5f;
    return legacyEnabled ? TimeStretchBackend::SoundTouch
                         : TimeStretchBackend::Resample;
}

bool MlrVSTAudioProcessor::usesContinuousTraversal() const
{
    if (continuousTraversalParam != nullptr)
        return continuousTraversalParam->load(std::memory_order_acquire) >= 0.5f;
    return true;
}

TimeStretchBackend MlrVSTAudioProcessor::getLoopTempoMatchBackend() const
{
    const float rawChoice = (flipTempoMatchModeParam != nullptr)
        ? flipTempoMatchModeParam->load(std::memory_order_acquire)
        : 0.0f;
    const int modeIndex = juce::jlimit(0, 1, static_cast<int>(std::round(rawChoice)));
    if (modeIndex == 1)
    {
        const auto backend = getStretchBackend();
        return isTimeStretchBackendAvailable(backend)
            ? backend
            : TimeStretchBackend::Resample;
    }

    return TimeStretchBackend::Resample;
}

MlrVSTAudioProcessor::StripTempoMatchMode MlrVSTAudioProcessor::getStripTempoMatchMode(int stripIndex) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return StripTempoMatchMode::Global;

    const auto* param = stripTempoMatchModeParams[static_cast<size_t>(stripIndex)];
    const float rawChoice = param != nullptr ? param->load(std::memory_order_acquire) : 0.0f;
    const int modeIndex = juce::jlimit(0, 2, static_cast<int>(std::round(rawChoice)));
    switch (modeIndex)
    {
        case 1: return StripTempoMatchMode::Repitch;
        case 2: return StripTempoMatchMode::MlrTs;
        case 0:
        default: return StripTempoMatchMode::Global;
    }
}

TimeStretchBackend MlrVSTAudioProcessor::resolveLoopTempoMatchBackendForStrip(int stripIndex) const
{
    switch (getStripTempoMatchMode(stripIndex))
    {
        case StripTempoMatchMode::Repitch:
            return TimeStretchBackend::Resample;
        case StripTempoMatchMode::MlrTs:
        {
            const auto backend = getStretchBackend();
            return isTimeStretchBackendAvailable(backend)
                ? backend
                : TimeStretchBackend::Resample;
        }
        case StripTempoMatchMode::Global:
        default:
            return getLoopTempoMatchBackend();
    }
}

MlrVSTAudioProcessor::FlipTempoMatchMode MlrVSTAudioProcessor::getFlipTempoMatchMode() const
{
    const float rawChoice = (flipTempoMatchModeParam != nullptr)
        ? flipTempoMatchModeParam->load(std::memory_order_acquire)
        : 0.0f;
    const int modeIndex = juce::jlimit(0, 1, static_cast<int>(std::round(rawChoice)));
    return modeIndex == 1 ? FlipTempoMatchMode::MlrTs
                          : FlipTempoMatchMode::Repitch;
}

TimeStretchBackend MlrVSTAudioProcessor::getFlipTempoMatchBackend() const
{
    return resolveFlipTempoMatch().backend;
}

MlrVSTAudioProcessor::ResolvedPitchControl MlrVSTAudioProcessor::resolvePitchControl(
    const EnhancedAudioStrip& strip,
    float semitones,
    int referenceRootMidi,
    PitchControlMode controlMode) const
{
    ResolvedPitchControl resolved;
    const float clampedSemitones = juce::jlimit(-24.0f, 24.0f, semitones);
    resolved.globalScale = getGlobalPitchScale();
    resolved.globalRootMidi = getGlobalRootNoteMidi();
    resolved.quantizedSemitones = ModernAudioEngine::quantizePitchSemitonesToScale(clampedSemitones,
                                                                                    referenceRootMidi,
                                                                                    resolved.globalScale);
    resolved.resampleRatio = juce::jlimit(0.125f, 4.0f, std::pow(2.0f, resolved.quantizedSemitones / 12.0f));

    if (strip.getPlayMode() == EnhancedAudioStrip::PlayMode::Step)
    {
        resolved.quantizedSemitones = clampedSemitones;
        resolved.stepSamplerRatio = juce::jlimit(0.125f, 8.0f, std::pow(2.0f, resolved.quantizedSemitones / 12.0f));
        resolved.updatesStepSampler = true;
        return resolved;
    }

    const bool sampleModeDisallowsSignalsmith =
        (strip.getPlayMode() == EnhancedAudioStrip::PlayMode::Sample);

    switch (controlMode)
    {
        case PitchControlMode::PitchShift:
            resolved.pitchAlgorithm = EnhancedAudioStrip::PitchShiftAlgorithm::Standard;
            break;
        case PitchControlMode::SoundTouch:
            resolved.pitchAlgorithm = EnhancedAudioStrip::PitchShiftAlgorithm::SoundTouch;
            break;
        case PitchControlMode::Resample:
            resolved.useResamplePitch = true;
            break;
        case PitchControlMode::Signalsmith:
            // Sample mode keeps the low-latency local shifter path instead of
            // enabling realtime Signalsmith.
            resolved.pitchAlgorithm = sampleModeDisallowsSignalsmith
                ? EnhancedAudioStrip::PitchShiftAlgorithm::Standard
                : EnhancedAudioStrip::PitchShiftAlgorithm::Signalsmith;
            break;
        case PitchControlMode::Bungee:
            resolved.pitchAlgorithm = EnhancedAudioStrip::PitchShiftAlgorithm::Bungee;
            break;
    }

    return resolved;
}

void MlrVSTAudioProcessor::applyResolvedPitchControl(EnhancedAudioStrip& strip,
                                                     const ResolvedPitchControl& resolved) const
{
    const bool preservePlaybackSpeed = (strip.getPlayMode() == EnhancedAudioStrip::PlayMode::Grain);
    strip.setGlobalPitchContext(resolved.globalRootMidi, static_cast<int>(resolved.globalScale));
    strip.setSignalsmithLiveMode(false);
    strip.setPitchShiftAlgorithm(resolved.pitchAlgorithm);

    if (resolved.updatesStepSampler)
    {
        strip.setPitchShift(resolved.quantizedSemitones);
        strip.setResamplePitchEnabled(false);
        strip.setResamplePitchRatio(1.0f);
        if (auto* stepSampler = strip.getStepSampler())
        {
            stepSampler->setSpeed(resolved.stepSamplerRatio);
        }
        return;
    }

    if (resolved.useResamplePitch)
    {
        strip.setResamplePitchEnabled(true);
        strip.setResamplePitchRatio(resolved.resampleRatio);
        strip.setPitchShift(0.0f);
        // Keep traversal/playmarker speed independent from resample pitch ratio.
        if (!preservePlaybackSpeed)
            strip.setPlaybackSpeed(1.0f);
        return;
    }

    strip.setResamplePitchEnabled(false);
    strip.setResamplePitchRatio(1.0f);
    if (!preservePlaybackSpeed)
        strip.setPlaybackSpeed(1.0f);
    strip.setPitchShift(resolved.quantizedSemitones);
}

MlrVSTAudioProcessor::ResolvedFlipTempoMatch MlrVSTAudioProcessor::resolveFlipTempoMatch() const
{
    ResolvedFlipTempoMatch resolved;
    resolved.mode = getFlipTempoMatchMode();
    if (resolved.mode == FlipTempoMatchMode::MlrTs)
        resolved.backend = getLoopTempoMatchBackend();
    return resolved;
}

MlrVSTAudioProcessor::ResolvedFlipPlaybackState MlrVSTAudioProcessor::resolveFlipPlaybackState(
    const EnhancedAudioStrip& strip,
    const SampleModeEngine& engine) const
{
    ResolvedFlipPlaybackState resolved;
    resolved.tempoMatch = resolveFlipTempoMatch();

    const double hostTempo = audioEngine != nullptr ? audioEngine->getCurrentTempo() : 120.0;
    resolved.tempoMatchRatio = computeFlipTempoMatchRatio(hostTempo, engine.getAnalyzedTempoBpm());
    resolved.playbackRate = juce::jmax(0.03125f, strip.getPlayheadSpeedRatio())
                          * juce::jmax(0.03125f, strip.getPlaybackSpeed())
                          * resolved.tempoMatchRatio;
    resolved.internalPitchSemitones = strip.getPitchShift();

    if (strip.isResamplePitchEnabled())
    {
        const float resampleRatio = strip.getResamplePitchRatio();
        resolved.playbackRate *= resampleRatio;
        if (resolved.tempoMatch.usesTimeStretch() && std::abs(resolved.tempoMatchRatio - 1.0f) > 0.01f)
            resolved.internalPitchSemitones += semitonesFromRatio(resampleRatio);
    }

    resolved.playbackRate = juce::jlimit(0.03125f, 8.0f, resolved.playbackRate);
    resolved.keyLockEnabled = strip.getPlayMode() == EnhancedAudioStrip::PlayMode::Sample
        && resolved.tempoMatch.usesTimeStretch();
    resolved.shouldBuildKeyLockCache = resolved.keyLockEnabled
        && (std::abs(resolved.tempoMatchRatio - 1.0f) > 0.01f
            || std::abs(resolved.internalPitchSemitones) > 0.01f);
    resolved.preferHighQualityKeyLock = resolved.shouldBuildKeyLockCache;
    return resolved;
}

void MlrVSTAudioProcessor::applyPitchControlToStrip(EnhancedAudioStrip& strip, float semitones)
{
    applyResolvedPitchControl(strip,
                              resolvePitchControl(strip,
                                                  semitones,
                                                  getGlobalRootNoteMidi(),
                                                  getPitchControlMode()));
}

void MlrVSTAudioProcessor::applyPitchControlToStrip(int stripIndex, EnhancedAudioStrip& strip, float semitones)
{
    applyResolvedPitchControl(strip,
                              resolvePitchControl(strip,
                                                  semitones,
                                                  getPitchQuantizeReferenceRootMidiForStrip(stripIndex),
                                                  resolvePitchControlModeForStrip(stripIndex)));
}

float MlrVSTAudioProcessor::getStoredStripPitchSemitones(int stripIndex) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return 0.0f;

    if (const auto* rawParam = stripPitchParams[static_cast<size_t>(stripIndex)])
        return rawParam->load(std::memory_order_acquire);

    if (audioEngine != nullptr)
    {
        if (const auto* strip = audioEngine->getStrip(stripIndex))
            return getPitchSemitonesForDisplay(*strip);
    }

    return 0.0f;
}

void MlrVSTAudioProcessor::applyStoredPitchControlToStrip(int stripIndex)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips || audioEngine == nullptr)
        return;

    auto* strip = audioEngine->getStrip(stripIndex);
    if (strip == nullptr)
        return;

    const float storedSemitones = quantizePitchSemitonesForStripControl(stripIndex,
                                                                        getStoredStripPitchSemitones(stripIndex));
    applyPitchControlToStrip(stripIndex, *strip, storedSemitones);
}

void MlrVSTAudioProcessor::reapplyStripStateForCurrentPlayMode(int stripIndex)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips || audioEngine == nullptr)
        return;

    auto* strip = audioEngine->getStrip(stripIndex);
    if (strip == nullptr)
        return;

    applyOwnedStripControlsFromParameters(stripIndex, *strip);

    if (getLoopPitchRole(stripIndex) == LoopPitchRole::Sync)
        applyLoopPitchRoleStateToStrip(stripIndex);
    else
        applyStoredPitchControlToStrip(stripIndex);
}

void MlrVSTAudioProcessor::applyUserPitchControlToStrip(int stripIndex, float semitones)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips || audioEngine == nullptr)
        return;

    const float quantizedSemitones = quantizePitchSemitonesForStripControl(stripIndex, semitones);
    if (auto* parameter = parameters.getParameter("stripPitch" + juce::String(stripIndex)))
    {
        const float normalized = juce::jlimit(0.0f, 1.0f, parameter->convertTo0to1(quantizedSemitones));
        const auto* rawParam = stripPitchParams[static_cast<size_t>(stripIndex)];
        const float currentNormalized = rawParam != nullptr
            ? juce::jlimit(0.0f, 1.0f, parameter->convertTo0to1(rawParam->load(std::memory_order_acquire)))
            : normalized;
        if (std::abs(currentNormalized - normalized) > 1.0e-5f)
            parameter->setValueNotifyingHost(normalized);
    }

    if (auto* strip = audioEngine->getStrip(stripIndex))
    {
        const auto loopPitchRole = getLoopPitchRole(stripIndex);
        if (loopPitchRole == LoopPitchRole::Sync)
            requestLoopPitchRoleStateUpdate(stripIndex);
        else
            applyPitchControlToStrip(stripIndex, *strip, quantizedSemitones);

        if (loopPitchRole == LoopPitchRole::Master)
            updateGlobalRootFromLoopPitchMaster(stripIndex, true);
    }
}

float MlrVSTAudioProcessor::getPitchSemitonesForDisplay(const EnhancedAudioStrip& strip) const
{
    if (strip.getPlayMode() == EnhancedAudioStrip::PlayMode::Step)
        return juce::jlimit(-24.0f, 24.0f, strip.getPitchShift());

    if (strip.isResamplePitchEnabled())
    {
        const float ratio = juce::jlimit(0.125f, 8.0f, strip.getResamplePitchRatio());
        const float semitones = 12.0f * std::log2(ratio);
        return juce::jlimit(-24.0f, 24.0f, semitones);
    }

    return strip.getPitchShift();
}

float MlrVSTAudioProcessor::quantizePitchSemitonesForStripControl(int stripIndex, float semitones) const
{
    const float clampedSemitones = juce::jlimit(-24.0f, 24.0f, semitones);
    if (audioEngine != nullptr)
    {
        if (const auto* strip = audioEngine->getStrip(stripIndex))
        {
            if (strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Step)
                return clampedSemitones;
        }
    }
    return ModernAudioEngine::quantizePitchSemitonesToScale(clampedSemitones,
                                                            getPitchQuantizeReferenceRootMidiForStrip(stripIndex),
                                                            getGlobalPitchScale());
}

bool MlrVSTAudioProcessor::requestLoopStripPitchMaster(int stripIndex)
{
    return beginLoopStripPitchAnalysis(stripIndex, true);
}

bool MlrVSTAudioProcessor::requestLoopStripPitchSync(int stripIndex)
{
    return beginLoopStripPitchAnalysis(stripIndex, false);
}

bool MlrVSTAudioProcessor::isLoopStripLoadInFlight(int stripIndex) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return false;
    return loopStripLoadInFlight[static_cast<size_t>(stripIndex)].load(std::memory_order_acquire) != 0;
}

float MlrVSTAudioProcessor::getLoopStripLoadProgress(int stripIndex) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return 0.0f;

    return static_cast<float>(loopStripLoadProgressPermille[static_cast<size_t>(stripIndex)].load(std::memory_order_acquire))
        / 1000.0f;
}

juce::String MlrVSTAudioProcessor::getLoopStripLoadStatusText(int stripIndex) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return {};

    const juce::ScopedLock lock(loopStripLoadStatusLock);
    return loopStripLoadStatusTexts[static_cast<size_t>(stripIndex)];
}

bool MlrVSTAudioProcessor::isLoopStripPitchAnalysisInFlight(int stripIndex) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return false;
    return loopPitchAnalysisInFlight[static_cast<size_t>(stripIndex)].load(std::memory_order_acquire) != 0;
}

float MlrVSTAudioProcessor::getLoopStripPitchAnalysisProgress(int stripIndex) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return 0.0f;

    return static_cast<float>(loopPitchAnalysisProgressPermille[static_cast<size_t>(stripIndex)].load(std::memory_order_acquire))
        / 1000.0f;
}

juce::String MlrVSTAudioProcessor::getLoopStripPitchAnalysisStatusText(int stripIndex) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return {};

    const juce::ScopedLock lock(loopPitchAnalysisStatusLock);
    return loopPitchAnalysisStatusTexts[static_cast<size_t>(stripIndex)];
}

int MlrVSTAudioProcessor::getLoopStripDetectedPitchMidi(int stripIndex) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return -1;
    return loopPitchDetectedMidi[static_cast<size_t>(stripIndex)].load(std::memory_order_acquire);
}

float MlrVSTAudioProcessor::getLoopStripDetectedPitchHz(int stripIndex) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return 0.0f;
    return loopPitchDetectedHz[static_cast<size_t>(stripIndex)].load(std::memory_order_acquire);
}

float MlrVSTAudioProcessor::getLoopStripDetectedPitchConfidence(int stripIndex) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return 0.0f;
    return loopPitchDetectedPitchConfidence[static_cast<size_t>(stripIndex)].load(std::memory_order_acquire);
}

int MlrVSTAudioProcessor::getLoopStripDetectedScaleIndex(int stripIndex) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return -1;
    return loopPitchDetectedScaleIndices[static_cast<size_t>(stripIndex)].load(std::memory_order_acquire);
}

float MlrVSTAudioProcessor::getLoopStripDetectedScaleConfidence(int stripIndex) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return 0.0f;
    return loopPitchDetectedScaleConfidence[static_cast<size_t>(stripIndex)].load(std::memory_order_acquire);
}

bool MlrVSTAudioProcessor::isLoopStripAssignedPitchManual(int stripIndex) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return false;
    return loopPitchAssignedManual[static_cast<size_t>(stripIndex)].load(std::memory_order_acquire) != 0;
}

bool MlrVSTAudioProcessor::didLoopStripPitchUseEssentia(int stripIndex) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return false;
    return loopPitchEssentiaUsed[static_cast<size_t>(stripIndex)].load(std::memory_order_acquire) != 0;
}

bool MlrVSTAudioProcessor::beginLoopStripPitchAnalysis(int stripIndex, bool setDetectedAsRoot)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips || audioEngine == nullptr)
        return false;

    auto* strip = audioEngine->getStrip(stripIndex);
    if (strip == nullptr)
        return false;

    const auto playMode = strip->getPlayMode();
    const bool supportedMode = playMode == EnhancedAudioStrip::PlayMode::OneShot
        || playMode == EnhancedAudioStrip::PlayMode::Loop
        || playMode == EnhancedAudioStrip::PlayMode::Gate
        || playMode == EnhancedAudioStrip::PlayMode::Grain;
    if (!supportedMode)
        return false;

    const auto* stripBuffer = strip->getAudioBuffer();
    if (stripBuffer == nullptr || stripBuffer->getNumSamples() <= 0 || strip->getSourceSampleRate() <= 0.0)
        return false;

    juce::AudioBuffer<float> analysisBuffer;
    analysisBuffer.makeCopyOf(*stripBuffer, true);
    const juce::File sourceFile = currentStripFiles[static_cast<size_t>(stripIndex)];
    const int requestId = loopPitchAnalysisRequestIds[static_cast<size_t>(stripIndex)].fetch_add(1, std::memory_order_acq_rel) + 1;
    loopPitchAnalysisInFlight[static_cast<size_t>(stripIndex)].store(1, std::memory_order_release);
    updateLoopPitchAnalysisProgress(stripIndex, requestId, 0.02f, "Preparing analysis...");

    auto job = std::make_unique<LoopPitchAnalysisJob>(*this,
                                                      stripIndex,
                                                      requestId,
                                                      std::move(analysisBuffer),
                                                      strip->getSourceSampleRate(),
                                                      safeFileExistsAsFile(sourceFile) ? sourceFile : juce::File(),
                                                      setDetectedAsRoot);
    loopPitchAnalysisThreadPool.addJob(job.release(), true);
    return true;
}


void MlrVSTAudioProcessor::applyCompletedSoundTouchPitchCaches()
{
    std::vector<SoundTouchPitchCacheResult> results;
    {
        const juce::ScopedLock lock(soundTouchPitchCacheResultLock);
        if (soundTouchPitchCacheResults.empty())
            return;
        results.swap(soundTouchPitchCacheResults);
    }

    for (auto& result : results)
    {
        if (result.stripIndex < 0 || result.stripIndex >= MaxStrips || audioEngine == nullptr)
            continue;

        const auto idx = static_cast<size_t>(result.stripIndex);
        if (result.requestId != soundTouchPitchCacheRequestIds[idx].load(std::memory_order_acquire))
        {
            soundTouchPitchCacheInFlight[idx].store(0, std::memory_order_release);
            continue;
        }

        soundTouchPitchCacheInFlight[idx].store(0, std::memory_order_release);

        auto* strip = audioEngine->getStrip(result.stripIndex);
        if (strip == nullptr || !result.success)
            continue;

        if (strip->getPitchShiftAlgorithm() != EnhancedAudioStrip::PitchShiftAlgorithm::SoundTouch
            || strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Step
            || strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Grain
            || strip->isResamplePitchEnabled())
        {
            continue;
        }

        const float currentTargetSemitones = snapSoundTouchPitchCacheSemitones(strip->getPitchShift());
        if (std::abs(currentTargetSemitones - result.semitones) > 0.01f)
            continue;

        strip->installSoundTouchPitchCache(std::move(result.renderedBuffer),
                                           result.sourceSampleRate,
                                           result.semitones,
                                           result.sourceVersion);
    }
}

void MlrVSTAudioProcessor::refreshPendingSoundTouchPitchCaches()
{
    if (audioEngine == nullptr || !isTimeStretchBackendAvailable(TimeStretchBackend::SoundTouch))
        return;

    for (int stripIndex = 0; stripIndex < MaxStrips; ++stripIndex)
    {
        auto* strip = audioEngine->getStrip(stripIndex);
        if (strip == nullptr)
            continue;

        const auto idx = static_cast<size_t>(stripIndex);

        if (strip->getPitchShiftAlgorithm() != EnhancedAudioStrip::PitchShiftAlgorithm::SoundTouch
            || strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Step
            || strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Grain
            || strip->isResamplePitchEnabled()
            || !strip->hasAudio())
        {
            soundTouchPitchCacheObservedTargets[idx] = 0.0f;
            soundTouchPitchCacheStableTicks[idx] = 0;
            continue;
        }

        const float semitones = snapSoundTouchPitchCacheSemitones(strip->getPitchShift());
        if (std::abs(semitones - soundTouchPitchCacheObservedTargets[idx]) > 0.01f)
        {
            if (soundTouchPitchCacheInFlight[idx].load(std::memory_order_acquire) != 0)
                soundTouchPitchCacheRequestIds[idx].fetch_add(1, std::memory_order_acq_rel);
            soundTouchPitchCacheObservedTargets[idx] = semitones;
            soundTouchPitchCacheStableTicks[idx] = 0;
        }
        else if (soundTouchPitchCacheStableTicks[idx] < kSoundTouchPitchCacheStableTimerTicks)
        {
            ++soundTouchPitchCacheStableTicks[idx];
        }

        if (std::abs(semitones) <= 0.01f)
        {
            soundTouchPitchCacheStableTicks[idx] = 0;
            continue;
        }

        if (strip->hasMatchingSoundTouchPitchCache(semitones))
            continue;

        if (soundTouchPitchCacheStableTicks[idx] < kSoundTouchPitchCacheStableTimerTicks)
            continue;

        if (soundTouchPitchCacheInFlight[idx].load(std::memory_order_acquire) != 0)
            continue;

        juce::AudioBuffer<float> sourceBuffer;
        double sourceSampleRate = 0.0;
        uint64_t sourceVersion = 0;
        if (!strip->copySoundTouchPitchSourceBuffer(sourceBuffer, sourceSampleRate, sourceVersion))
            continue;

        const int requestId = soundTouchPitchCacheRequestIds[idx].fetch_add(1, std::memory_order_acq_rel) + 1;
        soundTouchPitchCacheInFlight[idx].store(1, std::memory_order_release);
        auto job = std::make_unique<SoundTouchPitchCacheJob>(*this,
                                                             stripIndex,
                                                             requestId,
                                                             std::move(sourceBuffer),
                                                             sourceSampleRate,
                                                             sourceVersion,
                                                             semitones);
        soundTouchPitchCacheThreadPool.addJob(job.release(), true);
    }
}

void MlrVSTAudioProcessor::maintainStripOfflineCaches()
{
    if (audioEngine == nullptr)
        return;

    for (int stripIndex = 0; stripIndex < MaxStrips; ++stripIndex)
    {
        auto* strip = audioEngine->getStrip(stripIndex);
        if (strip == nullptr)
            continue;

        const auto idx = static_cast<size_t>(stripIndex);
#if MLRVST_ENABLE_SOUNDTOUCH || MLRVST_ENABLE_BUNGEE
        if (strip->swingCacheRenderPending())
        {
            // One-tick serial stability: knob rides settle before rendering.
            const uint32_t serial = strip->getSwingCacheRequestSerial();
            if (swingCacheLastObservedSerials[idx] == serial)
            {
                strip->markSwingCacheRenderInFlight();
                soundTouchPitchCacheThreadPool.addJob(
                    new StripOfflineCacheJob(*this, stripIndex, StripOfflineCacheJob::Kind::Swing),
                    true);
            }
            swingCacheLastObservedSerials[idx] = serial;
        }
#endif
#if MLRVST_ENABLE_BUNGEE
        if (strip->tempoMatchRenderPending())
        {
            const uint32_t serial = strip->getTempoMatchRequestSerial();
            if (tempoMatchCacheLastObservedSerials[idx] == serial)
            {
                strip->markTempoMatchRenderInFlight();
                soundTouchPitchCacheThreadPool.addJob(
                    new StripOfflineCacheJob(*this, stripIndex, StripOfflineCacheJob::Kind::TempoMatch),
                    true);
            }
            tempoMatchCacheLastObservedSerials[idx] = serial;
        }
#endif
    }
}

void MlrVSTAudioProcessor::queueBungeePitchCacheResult(BungeePitchCacheResult result)
{
    const juce::ScopedLock lock(bungeePitchCacheResultLock);
    bungeePitchCacheResults.push_back(std::move(result));
}

void MlrVSTAudioProcessor::applyCompletedBungeePitchCaches()
{
    std::vector<BungeePitchCacheResult> results;
    {
        const juce::ScopedLock lock(bungeePitchCacheResultLock);
        if (bungeePitchCacheResults.empty())
            return;
        results.swap(bungeePitchCacheResults);
    }

    for (auto& result : results)
    {
        if (result.stripIndex < 0 || result.stripIndex >= MaxStrips || audioEngine == nullptr)
            continue;

        const auto idx = static_cast<size_t>(result.stripIndex);
        if (result.requestId != bungeePitchCacheRequestIds[idx].load(std::memory_order_acquire))
        {
            bungeePitchCacheInFlight[idx].store(0, std::memory_order_release);
            continue;
        }

        bungeePitchCacheInFlight[idx].store(0, std::memory_order_release);

        auto* strip = audioEngine->getStrip(result.stripIndex);
        if (strip == nullptr || !result.success)
            continue;

        if (strip->getPitchShiftAlgorithm() != EnhancedAudioStrip::PitchShiftAlgorithm::Bungee
            || strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Step
            || strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Grain
            || strip->isResamplePitchEnabled())
        {
            continue;
        }

        const float currentTargetSemitones = snapBungeePitchCacheSemitones(strip->getPitchShift());
        if (std::abs(currentTargetSemitones - result.semitones) > 0.01f)
            continue;

        strip->installBungeePitchCache(std::move(result.renderedBuffer),
                                       result.sourceSampleRate,
                                       result.semitones,
                                       result.sourceVersion);
    }
}

void MlrVSTAudioProcessor::refreshPendingBungeePitchCaches()
{
    if (audioEngine == nullptr || !isTimeStretchBackendAvailable(TimeStretchBackend::Bungee))
        return;

    for (int stripIndex = 0; stripIndex < MaxStrips; ++stripIndex)
    {
        auto* strip = audioEngine->getStrip(stripIndex);
        if (strip == nullptr)
            continue;

        const auto idx = static_cast<size_t>(stripIndex);

        if (strip->getPitchShiftAlgorithm() != EnhancedAudioStrip::PitchShiftAlgorithm::Bungee
            || strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Step
            || strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Grain
            || strip->isResamplePitchEnabled()
            || !strip->hasAudio())
        {
            bungeePitchCacheObservedTargets[idx] = 0.0f;
            bungeePitchCacheStableTicks[idx] = 0;
            continue;
        }

        const float semitones = snapBungeePitchCacheSemitones(strip->getPitchShift());
        if (std::abs(semitones - bungeePitchCacheObservedTargets[idx]) > 0.01f)
        {
            if (bungeePitchCacheInFlight[idx].load(std::memory_order_acquire) != 0)
                bungeePitchCacheRequestIds[idx].fetch_add(1, std::memory_order_acq_rel);
            bungeePitchCacheObservedTargets[idx] = semitones;
            bungeePitchCacheStableTicks[idx] = 0;
        }
        else if (bungeePitchCacheStableTicks[idx] < kSoundTouchPitchCacheStableTimerTicks)
        {
            ++bungeePitchCacheStableTicks[idx];
        }

        if (std::abs(semitones) <= 0.01f)
        {
            bungeePitchCacheStableTicks[idx] = 0;
            continue;
        }

        if (strip->hasMatchingBungeePitchCache(semitones))
            continue;

        if (bungeePitchCacheStableTicks[idx] < kSoundTouchPitchCacheStableTimerTicks)
            continue;

        if (bungeePitchCacheInFlight[idx].load(std::memory_order_acquire) != 0)
            continue;

        juce::AudioBuffer<float> sourceBuffer;
        double sourceSampleRate = 0.0;
        uint64_t sourceVersion = 0;
        if (!strip->copySoundTouchPitchSourceBuffer(sourceBuffer, sourceSampleRate, sourceVersion))
            continue;

        const int requestId = bungeePitchCacheRequestIds[idx].fetch_add(1, std::memory_order_acq_rel) + 1;
        bungeePitchCacheInFlight[idx].store(1, std::memory_order_release);
        auto job = std::make_unique<BungeePitchCacheJob>(*this,
                                                         stripIndex,
                                                         requestId,
                                                         std::move(sourceBuffer),
                                                         sourceSampleRate,
                                                         sourceVersion,
                                                         semitones);
        bungeePitchCacheThreadPool.addJob(job.release(), true);
    }
}

void MlrVSTAudioProcessor::queueSignalsmithPitchCacheResult(SignalsmithPitchCacheResult result)
{
    const juce::ScopedLock lock(signalsmithPitchCacheResultLock);
    signalsmithPitchCacheResults.push_back(std::move(result));
}

void MlrVSTAudioProcessor::applyCompletedSignalsmithPitchCaches()
{
    std::vector<SignalsmithPitchCacheResult> results;
    {
        const juce::ScopedLock lock(signalsmithPitchCacheResultLock);
        if (signalsmithPitchCacheResults.empty())
            return;
        results.swap(signalsmithPitchCacheResults);
    }

    for (auto& result : results)
    {
        if (result.stripIndex < 0 || result.stripIndex >= MaxStrips || audioEngine == nullptr)
            continue;

        const auto idx = static_cast<size_t>(result.stripIndex);
        if (result.requestId != signalsmithPitchCacheRequestIds[idx].load(std::memory_order_acquire))
        {
            signalsmithPitchCacheInFlight[idx].store(0, std::memory_order_release);
            continue;
        }

        signalsmithPitchCacheInFlight[idx].store(0, std::memory_order_release);

        auto* strip = audioEngine->getStrip(result.stripIndex);
        if (strip == nullptr || !result.success)
            continue;

        if (strip->getPitchShiftAlgorithm() != EnhancedAudioStrip::PitchShiftAlgorithm::Signalsmith
            || strip->isSignalsmithLiveMode()
            || strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Step
            || strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Grain
            || strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Sample
            || strip->isResamplePitchEnabled())
        {
            continue;
        }

        const float currentTargetSemitones = snapSignalsmithPitchCacheSemitones(strip->getPitchShift());
        if (std::abs(currentTargetSemitones - result.semitones) > 0.01f)
            continue;

        strip->installSignalsmithPitchCache(std::move(result.renderedBuffer),
                                            result.sourceSampleRate,
                                            result.semitones,
                                            result.sourceVersion);
    }
}

void MlrVSTAudioProcessor::refreshPendingSignalsmithPitchCaches()
{
    if (audioEngine == nullptr)
        return;

    for (int stripIndex = 0; stripIndex < MaxStrips; ++stripIndex)
    {
        const auto idx = static_cast<size_t>(stripIndex);
        auto* strip = audioEngine->getStrip(stripIndex);
        if (strip == nullptr)
            continue;

        if (strip->getPitchShiftAlgorithm() != EnhancedAudioStrip::PitchShiftAlgorithm::Signalsmith
            || strip->isSignalsmithLiveMode()
            || strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Step
            || strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Grain
            || strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Sample
            || strip->isResamplePitchEnabled()
            || !strip->hasAudio())
        {
            signalsmithPitchCacheObservedTargets[idx] = 0.0f;
            signalsmithPitchCacheStableTicks[idx] = 0;
            continue;
        }

        const float semitones = snapSignalsmithPitchCacheSemitones(strip->getPitchShift());
        if (std::abs(semitones - signalsmithPitchCacheObservedTargets[idx]) > 0.01f)
        {
            if (signalsmithPitchCacheInFlight[idx].load(std::memory_order_acquire) != 0)
                signalsmithPitchCacheRequestIds[idx].fetch_add(1, std::memory_order_acq_rel);
            signalsmithPitchCacheObservedTargets[idx] = semitones;
            signalsmithPitchCacheStableTicks[idx] = 0;
        }
        else if (signalsmithPitchCacheStableTicks[idx] < kSoundTouchPitchCacheStableTimerTicks)
        {
            ++signalsmithPitchCacheStableTicks[idx];
        }

        if (std::abs(semitones) <= 0.01f)
        {
            signalsmithPitchCacheStableTicks[idx] = 0;
            continue;
        }

        if (strip->hasMatchingSignalsmithPitchCache(semitones))
            continue;

        if (signalsmithPitchCacheStableTicks[idx] < kSoundTouchPitchCacheStableTimerTicks)
            continue;

        if (signalsmithPitchCacheInFlight[idx].load(std::memory_order_acquire) != 0)
            continue;

        juce::AudioBuffer<float> sourceBuffer;
        double sourceSampleRate = 0.0;
        uint64_t sourceVersion = 0;
        if (!strip->copySoundTouchPitchSourceBuffer(sourceBuffer, sourceSampleRate, sourceVersion))
            continue;

        const int requestId = signalsmithPitchCacheRequestIds[idx].fetch_add(1, std::memory_order_acq_rel) + 1;
        signalsmithPitchCacheInFlight[idx].store(1, std::memory_order_release);
        auto job = std::make_unique<SignalsmithPitchCacheJob>(*this,
                                                              stripIndex,
                                                              requestId,
                                                              std::move(sourceBuffer),
                                                              sourceSampleRate,
                                                              sourceVersion,
                                                              semitones);
        signalsmithPitchCacheThreadPool.addJob(job.release(), true);
    }
}

juce::AudioProcessorValueTreeState::ParameterLayout MlrVSTAudioProcessor::createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    const auto globalFloatAttrs = juce::AudioParameterFloatAttributes().withAutomatable(false);
    const auto globalChoiceAttrs = juce::AudioParameterChoiceAttributes().withAutomatable(false);
    const auto globalBoolAttrs = juce::AudioParameterBoolAttributes().withAutomatable(false);

    layout.add(std::make_unique<juce::AudioParameterFloat>(
        "masterVolume",
        "Master Volume",
        juce::NormalisableRange<float>(0.0f, 1.0f),
        1.0f,
        globalFloatAttrs));

    layout.add(std::make_unique<juce::AudioParameterFloat>(
        "limiterThreshold",
        "Limiter Threshold (dB)",
        juce::NormalisableRange<float>(-24.0f, 0.0f, 0.1f),
        0.0f,
        globalFloatAttrs));

    layout.add(std::make_unique<juce::AudioParameterBool>(
        "limiterEnabled",
        "Limiter Enabled",
        false,
        globalBoolAttrs));

    layout.add(std::make_unique<juce::AudioParameterChoice>(
        "quantize",
        "Quantize",
        juce::StringArray{"1", "1/2", "1/2T", "1/4", "1/4T",
                          "1/8", "1/8T", "1/16", "1/16T", "1/32"},
        7,
        globalChoiceAttrs));  // Default to 1/16

    layout.add(std::make_unique<juce::AudioParameterChoice>(
        "innerLoopLength",
        "Inner Loop Length",
        juce::StringArray{"1", "1/2", "1/4", "1/8", "1/16"},
        0,
        globalChoiceAttrs));

    layout.add(std::make_unique<juce::AudioParameterChoice>(
        "quality",
        "Grain Q",
        juce::StringArray{"Linear", "Cubic", "Sinc", "Sinc HQ"},
        1,
        globalChoiceAttrs));

    layout.add(std::make_unique<juce::AudioParameterFloat>(
        "pitchSmoothing",
        "Pitch Smoothing",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f),
        0.05f,
        globalFloatAttrs));  // Default 50ms

    layout.add(std::make_unique<juce::AudioParameterFloat>(
        "inputMonitor",
        "Input Monitor",
        juce::NormalisableRange<float>(0.0f, 1.0f),
        1.0f,
        globalFloatAttrs));  // Default ON (1.0) for immediate monitoring

    layout.add(std::make_unique<juce::AudioParameterFloat>(
        "crossfadeLength",
        "Crossfade Length (ms)",
        juce::NormalisableRange<float>(1.0f, 50.0f, 0.1f),
        10.0f,
        globalFloatAttrs));

    layout.add(std::make_unique<juce::AudioParameterFloat>(
        "triggerFadeIn",
        "Trigger Fade In (ms)",
        juce::NormalisableRange<float>(0.01f, 120.0f, 0.01f),
        12.0f,
        globalFloatAttrs));

    layout.add(std::make_unique<juce::AudioParameterChoice>(
        "outputRouting",
        "Output Routing",
        juce::StringArray{"Stereo Mix", "Separate Strip Outs"},
        0,
        globalChoiceAttrs));

    juce::StringArray duckTriggerChoices{"None"};
    for (int i = 0; i < MaxStrips; ++i)
        duckTriggerChoices.add("S" + juce::String(i + 1));

    layout.add(std::make_unique<juce::AudioParameterChoice>(
        "masterDuckTriggerStrip",
        "Master Duck Trigger Strip",
        duckTriggerChoices,
        0,
        globalChoiceAttrs));

    layout.add(std::make_unique<juce::AudioParameterChoice>(
        "pitchControlMode",
        "Pitch Control Mode",
        juce::StringArray{"Pitch Shift", "SoundTouch", "Resample", "Signalsmith", "Bungee"},
        0,
        globalChoiceAttrs));

    layout.add(std::make_unique<juce::AudioParameterChoice>(
        "flipTempoMatchMode",
        "Tempo Match Mode",
        juce::StringArray{"Repitch", "Stretch"},
        0,
        globalChoiceAttrs));

    layout.add(std::make_unique<juce::AudioParameterChoice>(
        "stretchBackend",
        "Stretch",
        juce::StringArray{"Resample", "SoundTouch", "Bungee"},
        1,
        globalChoiceAttrs));

    layout.add(std::make_unique<juce::AudioParameterBool>(
        "continuousTraversal",
        "Continuous Traversal",
        true,
        globalBoolAttrs));

    layout.add(std::make_unique<juce::AudioParameterChoice>(
        "transientOnsetMethod",
        "Transient Onset Method",
        juce::StringArray{"Hybrid", "HFC", "SpecFlux"},
        2,
        globalChoiceAttrs));

    layout.add(std::make_unique<juce::AudioParameterChoice>(
        "transientSensitivity",
        "Transient Sensitivity",
        juce::StringArray{"VLow", "Low", "Norm", "High", "VHigh"},
        2,
        globalChoiceAttrs));

    layout.add(std::make_unique<juce::AudioParameterChoice>(
        "transientSnap",
        "Transient Snap",
        juce::StringArray{"Soft", "Loose", "Norm", "Tight", "Exact"},
        3,
        globalChoiceAttrs));

    layout.add(std::make_unique<juce::AudioParameterChoice>(
        "transientSpacing",
        "Transient Spacing",
        juce::StringArray{"Tight", "Close", "Norm", "Wide", "Wider"},
        2,
        globalChoiceAttrs));

    layout.add(std::make_unique<juce::AudioParameterBool>(
        "sceneMode",
        "Scene Mode",
        false,
        globalBoolAttrs));

    layout.add(std::make_unique<juce::AudioParameterChoice>(
        "sceneRecallMode",
        "Scene Change",
        juce::StringArray{"Grid", "Pattern End", "Scene End", "Manual"},
        static_cast<int>(SceneRecallMode::Manual),
        globalChoiceAttrs));

    layout.add(std::make_unique<juce::AudioParameterBool>(
        "soundTouchEnabled",
        "SoundTouch Enabled",
        true,
        globalBoolAttrs));

    for (int i = 0; i < MaxStrips; ++i)
    {
        layout.add(std::make_unique<juce::AudioParameterFloat>(
            "stripVolume" + juce::String(i),
            "Strip " + juce::String(i + 1) + " Volume",
            juce::NormalisableRange<float>(0.0f, 1.0f),
            1.0f));

        layout.add(std::make_unique<juce::AudioParameterFloat>(
            "stripTrimDb" + juce::String(i),
            "Strip " + juce::String(i + 1) + " Trim",
            juce::NormalisableRange<float>(-36.0f, 36.0f, 0.1f),
            0.0f));

        layout.add(std::make_unique<juce::AudioParameterFloat>(
            "stripPan" + juce::String(i),
            "Strip " + juce::String(i + 1) + " Pan",
            juce::NormalisableRange<float>(-1.0f, 1.0f),
            0.0f));

        layout.add(std::make_unique<juce::AudioParameterFloat>(
            "stripSpeed" + juce::String(i),
            "Strip " + juce::String(i + 1) + " Playhead Speed",
            juce::NormalisableRange<float>(0.0f, 8.0f, 0.01f, 0.5f),
            1.0f));

        layout.add(std::make_unique<juce::AudioParameterFloat>(
            "stripPitch" + juce::String(i),
            "Strip " + juce::String(i + 1) + " Pitch",
            juce::NormalisableRange<float>(-24.0f, 24.0f, 0.01f),
            0.0f));

        layout.add(std::make_unique<juce::AudioParameterFloat>(
            "stripSliceLength" + juce::String(i),
            "Strip " + juce::String(i + 1) + " Slice Length",
            juce::NormalisableRange<float>(0.02f, 1.0f, 0.001f, 0.5f),
            1.0f));

        layout.add(std::make_unique<juce::AudioParameterChoice>(
            "stripPitchControlMode" + juce::String(i),
            "Strip " + juce::String(i + 1) + " Pitch Control Mode",
            juce::StringArray{"Global", "Pitch Shift", "SoundTouch", "Resample", "Signalsmith", "Bungee"},
            0,
            globalChoiceAttrs));

        layout.add(std::make_unique<juce::AudioParameterChoice>(
            "stripTempoMatchMode" + juce::String(i),
            "Strip " + juce::String(i + 1) + " Tempo Match Mode",
            juce::StringArray{"Global", "Repitch", "Stretch"},
            0,
            globalChoiceAttrs));

        layout.add(std::make_unique<juce::AudioParameterBool>(
            "stripFilterEnabled" + juce::String(i),
            "Strip " + juce::String(i + 1) + " Filter Enabled",
            false,
            globalBoolAttrs));

        layout.add(std::make_unique<juce::AudioParameterFloat>(
            "stripFilterFrequency" + juce::String(i),
            "Strip " + juce::String(i + 1) + " Filter Frequency",
            juce::NormalisableRange<float>(20.0f, 20000.0f, 1.0f, 0.25f),
            20000.0f,
            globalFloatAttrs));

        layout.add(std::make_unique<juce::AudioParameterFloat>(
            "stripFilterResonance" + juce::String(i),
            "Strip " + juce::String(i + 1) + " Filter Resonance",
            juce::NormalisableRange<float>(0.1f, 10.0f, 0.01f, 0.35f),
            0.707f,
            globalFloatAttrs));

        layout.add(std::make_unique<juce::AudioParameterFloat>(
            "stripFilterMorph" + juce::String(i),
            "Strip " + juce::String(i + 1) + " Filter Morph",
            juce::NormalisableRange<float>(0.0f, 1.0f, 0.001f),
            0.0f,
            globalFloatAttrs));

        layout.add(std::make_unique<juce::AudioParameterChoice>(
            "stripFilterAlgorithm" + juce::String(i),
            "Strip " + juce::String(i + 1) + " Filter Algorithm",
            juce::StringArray{"SVF12", "SVF24", "LAD12", "LAD24", "MOOG S", "MOOG H", "COMB"},
            0,
            globalChoiceAttrs));

        layout.add(std::make_unique<juce::AudioParameterBool>(
            "stripDuckEnabled" + juce::String(i),
            "Strip " + juce::String(i + 1) + " Duck Enabled",
            false,
            globalBoolAttrs));

        juce::StringArray duckSourceChoices{"Self", "Master"};
        for (int otherStrip = 0; otherStrip < MaxStrips; ++otherStrip)
            duckSourceChoices.add("S" + juce::String(otherStrip + 1));

        layout.add(std::make_unique<juce::AudioParameterChoice>(
            "stripDuckSource" + juce::String(i),
            "Strip " + juce::String(i + 1) + " Duck Source",
            duckSourceChoices,
            0,
            globalChoiceAttrs));

        layout.add(std::make_unique<juce::AudioParameterFloat>(
            "stripDuckThreshold" + juce::String(i),
            "Strip " + juce::String(i + 1) + " Duck Threshold",
            juce::NormalisableRange<float>(-60.0f, 0.0f, 0.1f),
            -24.0f,
            globalFloatAttrs));

        layout.add(std::make_unique<juce::AudioParameterFloat>(
            "stripDuckRatio" + juce::String(i),
            "Strip " + juce::String(i + 1) + " Duck Ratio",
            juce::NormalisableRange<float>(1.0f, 20.0f, 0.01f, 0.35f),
            4.0f,
            globalFloatAttrs));

        layout.add(std::make_unique<juce::AudioParameterFloat>(
            "stripDuckAttack" + juce::String(i),
            "Strip " + juce::String(i + 1) + " Duck Attack",
            juce::NormalisableRange<float>(0.1f, 200.0f, 0.1f, 0.35f),
            10.0f,
            globalFloatAttrs));

        layout.add(std::make_unique<juce::AudioParameterFloat>(
            "stripDuckRelease" + juce::String(i),
            "Strip " + juce::String(i + 1) + " Duck Release",
            juce::NormalisableRange<float>(5.0f, 1000.0f, 0.1f, 0.35f),
            180.0f,
            globalFloatAttrs));

        layout.add(std::make_unique<juce::AudioParameterFloat>(
            "stripDuckGainComp" + juce::String(i),
            "Strip " + juce::String(i + 1) + " Duck Gain Compensation",
            juce::NormalisableRange<float>(0.0f, 24.0f, 0.1f),
            0.0f,
            globalFloatAttrs));

        layout.add(std::make_unique<juce::AudioParameterBool>(
            "stripDuckFollowMaster" + juce::String(i),
            "Strip " + juce::String(i + 1) + " Duck Follow Master",
            false,
            globalBoolAttrs));

        layout.add(std::make_unique<juce::AudioParameterFloat>(
            "stripDelayMix" + juce::String(i),
            "Strip " + juce::String(i + 1) + " Delay Mix",
            juce::NormalisableRange<float>(0.0f, 1.0f, 0.001f),
            0.0f,
            globalFloatAttrs));

        layout.add(std::make_unique<juce::AudioParameterFloat>(
            "stripDelayTime" + juce::String(i),
            "Strip " + juce::String(i + 1) + " Delay Time",
            juce::NormalisableRange<float>(0.25f, 4.0f, 0.001f, 0.45f),
            1.0f,
            globalFloatAttrs));

        layout.add(std::make_unique<juce::AudioParameterBool>(
            "stripDelaySync" + juce::String(i),
            "Strip " + juce::String(i + 1) + " Delay Sync",
            true,
            globalBoolAttrs));

        layout.add(std::make_unique<juce::AudioParameterFloat>(
            "stripDelayFeedback" + juce::String(i),
            "Strip " + juce::String(i + 1) + " Delay Feedback",
            juce::NormalisableRange<float>(0.0f, 0.97f, 0.001f),
            0.35f,
            globalFloatAttrs));

        layout.add(std::make_unique<juce::AudioParameterFloat>(
            "stripDelayLowCut" + juce::String(i),
            "Strip " + juce::String(i + 1) + " Delay Low Cut",
            juce::NormalisableRange<float>(20.0f, 12000.0f, 1.0f, 0.25f),
            20.0f,
            globalFloatAttrs));

        layout.add(std::make_unique<juce::AudioParameterFloat>(
            "stripDelayHighCut" + juce::String(i),
            "Strip " + juce::String(i + 1) + " Delay High Cut",
            juce::NormalisableRange<float>(200.0f, 20000.0f, 1.0f, 0.3f),
            12000.0f,
            globalFloatAttrs));

        layout.add(std::make_unique<juce::AudioParameterChoice>(
            "stripDelayMode" + juce::String(i),
            "Strip " + juce::String(i + 1) + " Delay Mode",
            juce::StringArray{"Single", "Dual", "Ping-Pong"},
            0,
            globalChoiceAttrs));
    }

    return layout;
}

//==============================================================================
void MlrVSTAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;
    audioEngine->prepareToPlay(sampleRate, samplesPerBlock);
    updateEffectiveSceneStutterAmount();
    preparedSceneSwitchOutgoingBuffer.setSize(juce::jmax(2, getTotalNumOutputChannels()),
                                              juce::jmax(1, samplesPerBlock),
                                              false,
                                              false,
                                              true);
    preparedSceneSwitchIncomingBuffer.setSize(juce::jmax(2, getTotalNumOutputChannels()),
                                              juce::jmax(1, samplesPerBlock),
                                              false,
                                              false,
                                              true);
    for (auto& scratch : sampleModeScratchBuffers)
        scratch.setSize(2, samplesPerBlock, false, false, true);
    sampleModeRenderedLastBlock.fill(false);
    for (auto& engine : sampleModeEngines)
    {
        if (engine != nullptr)
            engine->prepare(sampleRate, samplesPerBlock);
    }
    lastAppliedStretchBackend = -1;
    lastAppliedLoopTempoMatchBackend = -1;
    lastAppliedTransientOnsetMethod = -1;
    lastAppliedTransientSensitivity = -1;
    lastAppliedTransientSnap = -1;
    lastAppliedTransientSpacing = -1;
    lastGridLedUpdateTimeMs = 0;
    syncTransientDetectionSettingsFromParameters(true);
    if (gestureCoordinator)
        gestureCoordinator->syncToAudioEngine();

    // Now safe to connect to monome
    if (!monomeConnection.isConnected())
        monomeConnection.connect(8000);

    // Clear all LEDs on startup
    juce::MessageManager::callAsync([this]()
    {
        if (monomeConnection.isConnected())
        {
            if (monomeConnection.supportsGrid())
            {
                monomeConnection.setAllLEDs(0);
                monomeConnection.setAllLEDLevels(0);

                // Initialize LED cache
                for (int y = 0; y < MaxGridHeight; ++y)
                    for (int x = 0; x < MaxGridWidth; ++x)
                        ledCache[x][y] = -1;
            }
            if (monomeConnection.supportsArc())
            {
                for (auto& ring : arcRingCache)
                    ring.fill(-1);
                updateMonomeArcRings();
            }
        }
    });

    // Start LED update timer at 10fps (monome recommended refresh rate)
    if (!isTimerRunning())
        startTimer(kGridRefreshMs);

    if (!persistentGlobalControlsApplied)
    {
        suppressPersistentGlobalControlsSave.store(1, std::memory_order_release);
        GlobalSettingsStore::loadGlobalControls(*this);
        suppressPersistentGlobalControlsSave.store(0, std::memory_order_release);
        persistentGlobalControlsApplied = true;
    }

    pendingPersistentGlobalControlsRestore.store(1, std::memory_order_release);
    pendingPersistentGlobalControlsRestoreMs = juce::Time::currentTimeMillis() + 250;
    pendingPersistentGlobalControlsRestoreRemaining = 5;
    if constexpr (kReportRealtimeSignalsmithLatencyToHost)
    {
        lastReportedLatencySamples = audioEngine != nullptr
            ? audioEngine->getRealtimeSignalsmithAlignmentLatencySamples()
            : 0;
        setLatencySamples(lastReportedLatencySamples);
    }
    else
    {
        lastReportedLatencySamples = 0;
    }
}

void MlrVSTAudioProcessor::releaseResources()
{
    if constexpr (kReportRealtimeSignalsmithLatencyToHost)
        setLatencySamples(0);
    lastReportedLatencySamples = 0;
    preparedSceneSwitchOutgoingBuffer.setSize(0, 0);
    preparedSceneSwitchIncomingBuffer.setSize(0, 0);
    stopTimer();
    monomeConnection.disconnect();
}

bool MlrVSTAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    // Main output is fixed stereo; strip outputs are stereo buses.
    auto mainOutput = layouts.getMainOutputChannelSet();
    if (mainOutput != juce::AudioChannelSet::stereo())
        return false;

    // Aux outputs are either disabled or match main output channel set.
    const int outputBusCount = layouts.outputBuses.size();
    for (int bus = 1; bus < outputBusCount; ++bus)
    {
        const auto busSet = layouts.getChannelSet(false, bus);
        if (busSet != juce::AudioChannelSet::disabled() && busSet != mainOutput)
            return false;
    }

    // Check input (we accept mono or stereo input, or disabled)
    auto inputChannels = layouts.getMainInputChannelSet();
    if (inputChannels != juce::AudioChannelSet::disabled()
     && inputChannels != juce::AudioChannelSet::mono()
     && inputChannels != juce::AudioChannelSet::stereo())
        return false;

    return true;
}

void MlrVSTAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;
    const bool wasHostTransportPlaying = lastHostTransportPlaying.load(std::memory_order_acquire) != 0;

    // CRITICAL: Handle separate input/output buffers for AU/VST3 compatibility
    // Some hosts (especially AU) provide separate input and output buffers
    auto totalNumInputChannels = getTotalNumInputChannels();
    auto totalNumOutputChannels = getTotalNumOutputChannels();

    // Clear any output channels that don't have corresponding input
    for (auto i = totalNumInputChannels; i < totalNumOutputChannels; ++i)
        buffer.clear(i, 0, buffer.getNumSamples());

    // Get position info from host
    juce::AudioPlayHead::PositionInfo posInfo;

    if (getCurrentHostPositionInfo(posInfo))
    {
        // Host provided position info.
    }
    else
    {
        // No playhead - assume playing
        posInfo.setIsPlaying(true);
    }

    const bool hostTransportPlayingNow = posInfo.getIsPlaying();
    lastHostTransportPlaying.store(hostTransportPlayingNow ? 1 : 0, std::memory_order_release);

    // Set tempo FIRST: use host tempo if available, otherwise fallback default.
    if (!posInfo.getBpm().hasValue() || *posInfo.getBpm() <= 0.0)
    {
        posInfo.setBpm(120.0);  // Fallback default
    }

    // Update engine parameters
    if (masterVolumeParam)
        audioEngine->setMasterVolume(*masterVolumeParam);

    if (limiterThresholdParam)
        audioEngine->setLimiterThresholdDb(limiterThresholdParam->load(std::memory_order_acquire));

    if (limiterEnabledParam)
        audioEngine->setLimiterEnabled(limiterEnabledParam->load(std::memory_order_acquire) > 0.5f);

    if (quantizeParam)
    {
        int quantizeChoice = static_cast<int>(*quantizeParam);
        // Map choice to actual divisions: 0=1, 1=2, 2=3, 3=4, 4=6, 5=8, 6=12, 7=16, 8=24, 9=32
        const int divisionMap[] = {1, 2, 3, 4, 6, 8, 12, 16, 24, 32};
        int division = (quantizeChoice >= 0 && quantizeChoice < 10) ? divisionMap[quantizeChoice] : 8;
        audioEngine->setQuantization(division);
    }

    if (pitchSmoothingParam)
        audioEngine->setPitchSmoothingTime(*pitchSmoothingParam);

    if (grainQualityParam)
    {
        auto grainQuality = static_cast<Resampler::Quality>(juce::jlimit(0, 3, static_cast<int>(*grainQualityParam)));
        for (int i = 0; i < MaxStrips; ++i)
        {
            if (auto* strip = audioEngine->getStrip(i))
                strip->setGrainResamplerQuality(grainQuality);
        }
    }

    if (inputMonitorParam)
        audioEngine->setInputMonitorVolume(*inputMonitorParam);

    if (crossfadeLengthParam)
        audioEngine->setCrossfadeLengthMs(*crossfadeLengthParam);

    if (triggerFadeInParam)
        audioEngine->setTriggerFadeInMs(*triggerFadeInParam);

    if (!wasHostTransportPlaying && hostTransportPlayingNow && isSceneModeEnabled())
    {
        double restartSceneStartPpq = std::numeric_limits<double>::quiet_NaN();
        if (posInfo.getPpqPosition().hasValue() && std::isfinite(*posInfo.getPpqPosition()))
            restartSceneStartPpq = *posInfo.getPpqPosition();
        else if (audioEngine != nullptr)
            restartSceneStartPpq = audioEngine->getTimelineBeat();

        const int restartSceneSlot = juce::jlimit(0, SceneSlots - 1, activeSceneSlot);
        const bool shouldRestartScenePlayback = sceneSequenceActive
            || activeScenePlaybackHandle.active
            || scenePerformanceRecorder.hasEvents(restartSceneSlot);

        if (shouldRestartScenePlayback && std::isfinite(restartSceneStartPpq))
        {
            if (sceneSequenceActive)
            {
                pendingSceneRecall = {};
                clearPendingSceneApplyState();
                clearSceneBoundaryTransitionState();
                clearSceneChainReturnOverride();
            }

            setActiveScenePlaybackHandle(activeSceneMainPresetIndex,
                                         restartSceneSlot,
                                         sceneSequenceActive,
                                         sceneSequenceCurrentStepIndex,
                                         restartSceneStartPpq,
                                         getResolvedSceneLengthBeats(restartSceneSlot));
            setSceneChainAttachStartPpq(restartSceneStartPpq);
            scenePlaybackBlockStutterPostRenderPending = false;
            scenePlaybackBlockStutterPostRenderAmount = 0.0f;
            setGlobalSceneStutterAmount(0.0f);
            lastScenePerformanceProcessBeat = std::numeric_limits<double>::quiet_NaN();
            lastScenePerformanceProcessSceneSlot = -1;
            lastScenePerformanceProcessSceneStartBeat = std::numeric_limits<double>::quiet_NaN();
            applySceneHeldAutomationStateAtBeat(restartSceneSlot,
                                                restartSceneStartPpq,
                                                restartSceneStartPpq);
            updateAudioEngineSceneModulationContext();

            if (sceneSequenceActive)
            {
            armNextSceneInSequence(getActiveMainPresetIndexForScenes(),
                                   restartSceneSlot,
                                   restartSceneStartPpq);
            }
        }
    }

    const auto stretchBackend = getStretchBackend();
    const int stretchBackendInt = static_cast<int>(stretchBackend);
    if (stretchBackendInt != lastAppliedStretchBackend)
    {
        audioEngine->setGlobalStretchBackend(stretchBackend);
        lastAppliedStretchBackend = stretchBackendInt;
    }

    const int continuousTraversalInt = usesContinuousTraversal() ? 1 : 0;
    if (continuousTraversalInt != lastAppliedContinuousTraversal)
    {
        audioEngine->setGlobalContinuousTraversal(continuousTraversalInt != 0);
        lastAppliedContinuousTraversal = continuousTraversalInt;
    }

    syncTransientDetectionSettingsFromParameters(false);

    const auto loopTempoMatchBackend = getLoopTempoMatchBackend();
    const int loopTempoMatchBackendInt = static_cast<int>(loopTempoMatchBackend);
    if (loopTempoMatchBackendInt != lastAppliedLoopTempoMatchBackend)
    {
        audioEngine->setGlobalTempoMatchBackend(loopTempoMatchBackend);
        lastAppliedLoopTempoMatchBackend = loopTempoMatchBackendInt;
    }

    if (masterDuckTriggerStripParam)
    {
        const int triggerChoice = juce::jlimit(0, MaxStrips, static_cast<int>(masterDuckTriggerStripParam->load(std::memory_order_acquire)));
        audioEngine->setMasterDuckTriggerStrip(triggerChoice - 1);
    }

    // Apply any pending loop enter/exit actions that were quantized to timeline.
    recoverDeferredPpqAnchors(posInfo);
    applyPendingLoopChanges(posInfo);
    applyPendingBarChanges(posInfo);
    applyPendingStutterRelease(posInfo);
    applyPendingStutterStart(posInfo);
    if (auto timeSig = posInfo.getTimeSignature())
    {
        const double beatsPerBar = static_cast<double>(juce::jmax(1, timeSig->numerator))
            * (4.0 / static_cast<double>(juce::jmax(1, timeSig->denominator)));
        if (std::isfinite(beatsPerBar) && beatsPerBar > 0.0)
            hostBarLengthBeats.store(beatsPerBar, std::memory_order_release);
    }
    updateSceneQuantizedRecall(posInfo, buffer.getNumSamples());
    updateSceneRecorderQuantizedAction(posInfo, buffer.getNumSamples());

    // Update strip parameters
    const int globalRootMidi = getGlobalRootNoteMidi();
    const auto globalScale = getGlobalPitchScale();
    const auto currentPpq = posInfo.getPpqPosition();
    const int64_t currentGlobalSample = audioEngine != nullptr ? audioEngine->getGlobalSampleCount() : -1;
    const bool allowOwnedStripParameterSync = suppressOwnedStripParameterSync.load(std::memory_order_acquire) == 0;
    for (int i = 0; i < MaxStrips; ++i)
    {
        auto* strip = audioEngine->getStrip(i);
        if (strip)
        {
            strip->setGlobalPitchContext(globalRootMidi, static_cast<int>(globalScale));
            if (allowOwnedStripParameterSync)
            {
                applyOwnedStripControlsFromParameters(i, *strip);

                auto* pitchParam = stripPitchParams[static_cast<size_t>(i)];
                if (pitchParam)
                {
                    const bool hadResamplePitch = strip->isResamplePitchEnabled();
                    const float previousResampleRatio = strip->getResamplePitchRatio();
                    const bool syncRoleActive = getLoopPitchRole(i) == LoopPitchRole::Sync;
                    const bool syncRetunePending =
                        loopPitchPendingRetune[static_cast<size_t>(i)].load(std::memory_order_acquire) != 0;

                    if (syncRoleActive)
                    {
                        if (syncRetunePending)
                            applyPitchControlToStrip(i, *strip, getPitchSemitonesForDisplay(*strip));
                        else
                            applyLoopPitchRoleStateToStrip(i);
                    }
                    else
                    {
                        applyPitchControlToStrip(i, *strip, pitchParam->load(std::memory_order_acquire));
                    }

                    const bool hasResamplePitch = strip->isResamplePitchEnabled();
                    const float currentResampleRatio = strip->getResamplePitchRatio();
                    const bool resamplePitchStateChanged = hadResamplePitch != hasResamplePitch
                        || ((hadResamplePitch || hasResamplePitch)
                            && std::abs(previousResampleRatio - currentResampleRatio) > 1.0e-5f);

                    if (resamplePitchStateChanged
                        && strip->isPlaying()
                        && strip->isPpqTimelineAnchored()
                        && currentPpq.hasValue()
                        && std::isfinite(*currentPpq)
                        && currentGlobalSample >= 0)
                    {
                        strip->realignToPpqAnchor(*currentPpq, currentGlobalSample);
                    }
                }

                auto* sliceLengthParam = stripSliceLengthParams[static_cast<size_t>(i)];
                if (sliceLengthParam)
                    strip->setLoopSliceLength(sliceLengthParam->load(std::memory_order_acquire));

                const auto previousTempoMatchBackend = strip->getTempoMatchBackend();
                const auto resolvedTempoMatchBackend = resolveLoopTempoMatchBackendForStrip(i);
                strip->setTempoMatchBackend(resolvedTempoMatchBackend);

                if (previousTempoMatchBackend != resolvedTempoMatchBackend
                    && strip->isPlaying()
                    && strip->isPpqTimelineAnchored()
                    && currentPpq.hasValue()
                    && std::isfinite(*currentPpq)
                    && currentGlobalSample >= 0)
                {
                    strip->realignToPpqAnchor(*currentPpq, currentGlobalSample);
                }

                auto* duckEnabledParam = stripDuckEnabledParams[static_cast<size_t>(i)];
                if (duckEnabledParam)
                    strip->setDuckEnabled(duckEnabledParam->load(std::memory_order_acquire) > 0.5f);

                auto* duckSourceParam = stripDuckSourceParams[static_cast<size_t>(i)];
                if (duckSourceParam)
                    strip->setDuckSourceSelection(static_cast<int>(duckSourceParam->load(std::memory_order_acquire)));

                auto* duckThresholdParam = stripDuckThresholdParams[static_cast<size_t>(i)];
                if (duckThresholdParam)
                    strip->setDuckThresholdDb(duckThresholdParam->load(std::memory_order_acquire));

                auto* duckRatioParam = stripDuckRatioParams[static_cast<size_t>(i)];
                if (duckRatioParam)
                    strip->setDuckRatio(duckRatioParam->load(std::memory_order_acquire));

                auto* duckAttackParam = stripDuckAttackParams[static_cast<size_t>(i)];
                if (duckAttackParam)
                    strip->setDuckAttackMs(duckAttackParam->load(std::memory_order_acquire));

                auto* duckReleaseParam = stripDuckReleaseParams[static_cast<size_t>(i)];
                if (duckReleaseParam)
                    strip->setDuckReleaseMs(duckReleaseParam->load(std::memory_order_acquire));

                auto* duckGainCompParam = stripDuckGainCompParams[static_cast<size_t>(i)];
                if (duckGainCompParam)
                    strip->setDuckGainCompDb(duckGainCompParam->load(std::memory_order_acquire));

                auto* duckFollowMasterParam = stripDuckFollowMasterParams[static_cast<size_t>(i)];
                if (duckFollowMasterParam)
                    strip->setDuckFollowMaster(duckFollowMasterParam->load(std::memory_order_acquire) > 0.5f);
            }
        }
    }

    handleIncomingMacroCc(midiMessages);
    if (!renderPendingPreparedSceneSwitch(buffer, midiMessages, posInfo, currentGlobalSample))
        renderActiveSceneAudio(buffer, midiMessages, posInfo, true, true);

    if (scenePlaybackBlockStutterPostRenderPending)
    {
        setGlobalSceneStutterAmount(scenePlaybackBlockStutterPostRenderAmount);
        scenePlaybackBlockStutterPostRenderPending = false;
    }

    if (activeScenePlaybackHandle.active)
        refreshSceneStripLaunchHandlesFromEngine();

}

//==============================================================================
bool MlrVSTAudioProcessor::hasEditor() const
{
    return true;
}

juce::AudioProcessorEditor* MlrVSTAudioProcessor::createEditor()
{
    return new MlrVSTAudioProcessorEditor(*this);
}

//==============================================================================
void MlrVSTAudioProcessor::stripPersistentGlobalControlsFromState(juce::ValueTree& state) const
{
    if (!state.isValid())
        return;
    for (const auto* id : kPersistentGlobalControlParameterIds)
        state.removeProperty(id, nullptr);
}

//==============================================================================

void MlrVSTAudioProcessor::captureRecentAudioToStrip(int stripIndex)
{
    if (!audioEngine || stripIndex < 0 || stripIndex >= MaxStrips)
        return;

    if (auto* strip = audioEngine->getStrip(stripIndex))
    {
        const int bars = strip->getRecordingBars();
        audioEngine->captureLoopToStrip(stripIndex, bars);

        // Captured audio comes from the live input ring buffer, not a source file.
        // Clear stale path so preset save can embed the audio data.
        currentStripFiles[static_cast<size_t>(stripIndex)] = juce::File();

        // Recording stop auto-trigger must still respect group choke behavior.
        audioEngine->triggerStripWithQuantization(stripIndex, 0, false);
        updateMonomeLEDs();
        queueActiveSceneAutosave();
    }
}

void MlrVSTAudioProcessor::clearRecentAudioBuffer()
{
    if (!audioEngine)
        return;

    audioEngine->clearRecentInputBuffer();
}

void MlrVSTAudioProcessor::setPendingBarLengthApply(int stripIndex, bool pending)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;

    pendingBarLengthApply[static_cast<size_t>(stripIndex)] = pending;
}

bool MlrVSTAudioProcessor::canChangeBarLengthNow(int stripIndex) const
{
    if (!audioEngine || stripIndex < 0 || stripIndex >= MaxStrips)
        return false;

    auto* strip = audioEngine->getStrip(stripIndex);
    return strip != nullptr;
}

void MlrVSTAudioProcessor::requestBarLengthChange(int stripIndex, int bars)
{
    if (!audioEngine || stripIndex < 0 || stripIndex >= MaxStrips)
        return;

    auto* strip = audioEngine->getStrip(stripIndex);
    if (!strip)
        return;

    const auto selection = decodeBarSelection(bars);
    setPendingBarLengthApply(stripIndex, false);

    if (!strip->hasAudio())
    {
        strip->setRecordingBars(selection.recordingBars);
        strip->setBeatsPerLoop(selection.beatsPerLoop);
        const juce::ScopedLock lock(pendingBarChangeLock);
        pendingBarChanges[static_cast<size_t>(stripIndex)].active = false;
        queueActiveSceneAutosave();
        return;
    }

    if (!strip->isPlaying())
    {
        strip->setRecordingBars(selection.recordingBars);
        strip->setBeatsPerLoop(selection.beatsPerLoop);
        const juce::ScopedLock lock(pendingBarChangeLock);
        pendingBarChanges[static_cast<size_t>(stripIndex)].active = false;
        queueActiveSceneAutosave();
        return;
    }

    const int quantizeDivision = getQuantizeDivision();
    // Bar changes are always PPQ-grid scheduled when host PPQ is valid.
    const bool useQuantize = (quantizeDivision >= 1);

    bool hasHostPpq = false;
    double currentPpq = std::numeric_limits<double>::quiet_NaN();
    hasHostPpq = getCurrentHostPpq(currentPpq);

    const bool syncReadyNow = hasHostPpq
        && std::isfinite(currentPpq)
        && strip->isPpqTimelineAnchored();

    if (!syncReadyNow)
    {
        strip->setRecordingBars(selection.recordingBars);
        strip->setBeatsPerLoop(selection.beatsPerLoop);
        const juce::ScopedLock lock(pendingBarChangeLock);
        pendingBarChanges[static_cast<size_t>(stripIndex)].active = false;
        return;
    }

    const juce::ScopedLock lock(pendingBarChangeLock);
    auto& pending = pendingBarChanges[static_cast<size_t>(stripIndex)];
    pending.active = true;
    pending.recordingBars = selection.recordingBars;
    pending.beatsPerLoop = selection.beatsPerLoop;
    pending.quantized = useQuantize;
    pending.quantizeDivision = quantizeDivision;
    pending.targetPpq = std::numeric_limits<double>::quiet_NaN();

    if (!pending.quantized)
        return;
    // Resolve quantized target on the audio thread to avoid GUI/playhead clock skew.
}

int MlrVSTAudioProcessor::getQuantizeDivision() const
{
    auto* quantizeParamLocal = parameters.getRawParameterValue("quantize");
    const int quantizeChoice = quantizeParamLocal ? static_cast<int>(*quantizeParamLocal) : 5;
    const int divisionMap[] = {1, 2, 3, 4, 6, 8, 12, 16, 24, 32};
    return (quantizeChoice >= 0 && quantizeChoice < 10) ? divisionMap[quantizeChoice] : 8;
}

bool MlrVSTAudioProcessor::sceneSlotExistsForMainPreset(int mainPresetIndex, int sceneSlot) const
{
    return hasStoredSceneSlotState(mainPresetIndex, sceneSlot);
}

void MlrVSTAudioProcessor::selectSceneSlotFromSurface(int sceneSlot)
{
    const int safeSceneSlot = juce::jlimit(0, SceneSlots - 1, sceneSlot);
    focusSceneSlot(safeSceneSlot);

    const bool exitingSceneChain = sceneSequenceActive;
    if (exitingSceneChain)
    {
        appendSceneDebugLog("surface_select stop_chain slot=" + juce::String(safeSceneSlot));
        stopSceneChainPlayback();
    }

    const int mainPresetIndex = getActiveMainPresetIndexForScenes();
    const bool targetsCurrentLiveScene = !sceneSequenceActive
        && activeSceneMainPresetIndex == mainPresetIndex
        && activeSceneSlot == safeSceneSlot;
    appendSceneDebugLog("surface_select slot=" + juce::String(safeSceneSlot)
        + " mainPreset=" + juce::String(mainPresetIndex)
        + " activeScene=" + juce::String(activeSceneSlot)
        + " activeMain=" + juce::String(activeSceneMainPresetIndex)
        + " targetsCurrent=" + juce::String(targetsCurrentLiveScene ? 1 : 0)
        + " needsCapture=" + juce::String(activeSceneNeedsCaptureBeforeManualRecall ? 1 : 0)
        + " exists=" + juce::String(sceneSlotExistsForMainPreset(mainPresetIndex, safeSceneSlot) ? 1 : 0));

    if (!sceneSequenceActive
        && activeSceneNeedsCaptureBeforeManualRecall
        && activeSceneRecallDegraded.load(std::memory_order_acquire) == 0
        && activeSceneMainPresetIndex == mainPresetIndex)
    {
        const int captureSceneSlot = juce::jlimit(0,
                                                  SceneSlots - 1,
                                                  targetsCurrentLiveScene ? safeSceneSlot : activeSceneSlot);
        const bool captured = refreshStoredSceneSlotSnapshot(mainPresetIndex, captureSceneSlot);
        appendSceneDebugLog("surface_select capture_current slot=" + juce::String(captureSceneSlot)
            + " success=" + juce::String(captured ? 1 : 0));
        updateMonomeLEDs();
        if (!captured)
            return;
    }

    clearPendingSceneApplyState();
    pendingScenePreloadDirty.store(0, std::memory_order_release);
    pendingScenePreloadMainPreset.store(-1, std::memory_order_release);
    pendingScenePreloadSceneSlot.store(-1, std::memory_order_release);
    pendingScenePreloadSequenceDriven.store(0, std::memory_order_release);
    pendingScenePreloadSequenceStep.store(-1, std::memory_order_release);
    pendingScenePreloadTargetPpq.store(-1.0, std::memory_order_release);
    pendingScenePreloadTargetTempo.store(120.0, std::memory_order_release);
    pendingScenePreloadTargetSample.store(-1, std::memory_order_release);
    pendingScenePreloadTransitionType.store(static_cast<int>(SceneChainTransitionType::None),
                                            std::memory_order_release);
    pendingScenePreloadSwitchSerial.store(0, std::memory_order_release);
    requestAbortActiveSceneTransition();

    requestSceneRecallQuantized(mainPresetIndex, safeSceneSlot, false, -1, true);
    appendSceneDebugLog("surface_select queued_recall slot=" + juce::String(safeSceneSlot)
        + " mainPreset=" + juce::String(mainPresetIndex));
    updateMonomeLEDs();
}

void MlrVSTAudioProcessor::recallSceneSlot(int sceneSlot)
{
    startManualScenePlayback(sceneSlot, false, true);
}

void MlrVSTAudioProcessor::startScenePerformanceRecordingAt(bool overdub,
                                                            int sceneSlot,
                                                            double currentBeat,
                                                            double sceneStartBeat,
                                                            bool updateLeds)
{
    if (!isSceneModeEnabled() || audioEngine == nullptr)
        return;

    clearPendingSceneRecorderAction();
    pendingSceneTriggerRecords.fill({});

    const int safeSceneSlot = juce::jlimit(0, SceneSlots - 1, sceneSlot);
    const double lengthBeats = getResolvedSceneLengthBeats(safeSceneSlot);
    const double safeCurrentBeat = std::isfinite(currentBeat) ? currentBeat : audioEngine->getTimelineBeat();
    const double safeSceneStartBeat = std::isfinite(sceneStartBeat) ? sceneStartBeat : safeCurrentBeat;

    clearActiveSceneAutomationOverrides(false);

    if (overdub)
        scenePerformanceRecorder.startOverdub(safeSceneSlot, lengthBeats, safeCurrentBeat, safeSceneStartBeat);
    else
        scenePerformanceRecorder.startRecording(safeSceneSlot, lengthBeats, safeCurrentBeat, safeSceneStartBeat);

    lastScenePerformanceProcessBeat = std::numeric_limits<double>::quiet_NaN();
    lastScenePerformanceProcessSceneSlot = safeSceneSlot;
    lastScenePerformanceProcessSceneStartBeat = safeSceneStartBeat;
    if (updateLeds)
        updateMonomeLEDs();
}

void MlrVSTAudioProcessor::startScenePerformanceRecording(bool overdub)
{
    if (!isSceneModeEnabled() || audioEngine == nullptr)
        return;

    requestSceneRecorderActionQuantized(overdub ? SceneRecorderAction::StartOverdub
                                                : SceneRecorderAction::StartFreshRecording);
}

void MlrVSTAudioProcessor::stopScenePerformanceRecording()
{
    if (!isSceneModeEnabled() || audioEngine == nullptr)
        return;

    requestSceneRecorderActionQuantized(SceneRecorderAction::StopRecording);
}

void MlrVSTAudioProcessor::stopScenePerformanceRecordingNow(bool updateLeds)
{
    clearPendingSceneRecorderAction();
    pendingSceneTriggerRecords.fill({});
    const int recordedSceneSlot = scenePerformanceRecorder.getRecordingSceneSlot();
    scenePerformanceRecorder.stopRecording();
    if (recordedSceneSlot >= 0 && recordedSceneSlot < SceneSlots)
    {
        refreshSceneAutomationTargetMask(recordedSceneSlot);
        if (isSceneModeEnabled()
            && audioEngine != nullptr
            && recordedSceneSlot == juce::jlimit(0, SceneSlots - 1, activeSceneSlot)
            && activeSceneStartPpqValid
            && std::isfinite(activeSceneStartPpq))
        {
            lastScenePerformanceProcessBeat = std::numeric_limits<double>::quiet_NaN();
            lastScenePerformanceProcessSceneSlot = -1;
            lastScenePerformanceProcessSceneStartBeat = std::numeric_limits<double>::quiet_NaN();
            applySceneHeldAutomationStateAtBeat(recordedSceneSlot,
                                                audioEngine->getTimelineBeat(),
                                                activeSceneStartPpq);
        }
    }
    queueActiveSceneAutosave();
    if (updateLeds)
        updateMonomeLEDs();
}

void MlrVSTAudioProcessor::extendScenePerformanceRecording()
{
    scenePerformanceRecorder.extendRecording();
    updateMonomeLEDs();
}

void MlrVSTAudioProcessor::clearScenePerformanceClip(int sceneSlot)
{
    const int safeSceneSlot = juce::jlimit(0, SceneSlots - 1, sceneSlot);
    scenePerformanceRecorder.clear(safeSceneSlot);
    syncScenePerformanceClipLengthToResolvedLength(safeSceneSlot);
    refreshSceneAutomationTargetMask(safeSceneSlot);

    if (safeSceneSlot == activeSceneSlot)
    {
        clearActiveSceneAutomationOverrides(false);
        lastScenePerformanceProcessBeat = std::numeric_limits<double>::quiet_NaN();
        lastScenePerformanceProcessSceneSlot = -1;
        lastScenePerformanceProcessSceneStartBeat = std::numeric_limits<double>::quiet_NaN();

        if (isSceneModeEnabled() && audioEngine != nullptr)
        {
            for (int stripIndex = 0; stripIndex < MaxStrips; ++stripIndex)
            {
                for (int slot = 0; slot < ModernAudioEngine::NumModSequencers; ++slot)
                    audioEngine->resetModSequencerSlotToDefaults(stripIndex, slot);
                audioEngine->setModSequencerSlot(stripIndex, 0);
            }
        }
    }

    updateMonomeLEDs();
    if (safeSceneSlot == activeSceneSlot)
        queueActiveSceneAutosave();
}

bool MlrVSTAudioProcessor::clearSceneStripAutomationAndMotion(int sceneSlot, int stripIndex)
{
    if (scenePerformanceRecorder.isRecording())
        return false;

    const int safeSceneSlot = juce::jlimit(0, SceneSlots - 1, sceneSlot);
    const int safeStripIndex = juce::jlimit(0, MaxStrips - 1, stripIndex);
    auto events = getScenePerformanceEventsSnapshot(safeSceneSlot);
    std::vector<ScenePerformanceControlTarget> clearedTargets;
    std::vector<ScenePerformanceControlTarget> restoreTargets;

    auto appendRestoreTarget = [&restoreTargets](ScenePerformanceControlTarget target)
    {
        if (target == ScenePerformanceControlTarget::None
            || target == ScenePerformanceControlTarget::Retrigger)
        {
            return;
        }

        if (std::find(restoreTargets.begin(), restoreTargets.end(), target) == restoreTargets.end())
            restoreTargets.push_back(target);
    };

    events.erase(std::remove_if(events.begin(),
                                events.end(),
                                [&clearedTargets, safeStripIndex](const ScenePerformanceEvent& event)
                                {
                                    if (event.type != ScenePerformanceEventType::ControlPoint
                                        || event.stripIndex != safeStripIndex)
                                    {
                                        return false;
                                    }

                                    if (std::find(clearedTargets.begin(),
                                                  clearedTargets.end(),
                                                  event.controlTarget) == clearedTargets.end())
                                    {
                                        clearedTargets.push_back(event.controlTarget);
                                    }
                                    return true;
                                }),
                 events.end());

    appendRestoreTarget(ScenePerformanceControlTarget::Volume);
    appendRestoreTarget(ScenePerformanceControlTarget::Pan);
    appendRestoreTarget(stripUsesGrainSceneLanesForSceneSlot(safeSceneSlot, safeStripIndex)
                            ? ScenePerformanceControlTarget::GrainPitch
                            : ScenePerformanceControlTarget::Pitch);
    appendRestoreTarget(ScenePerformanceControlTarget::Speed);
    appendRestoreTarget(ScenePerformanceControlTarget::Swing);
    appendRestoreTarget(ScenePerformanceControlTarget::FilterFrequency);
    appendRestoreTarget(ScenePerformanceControlTarget::FilterResonance);
    appendRestoreTarget(ScenePerformanceControlTarget::FilterEnabled);
    appendRestoreTarget(ScenePerformanceControlTarget::FilterMorph);
    appendRestoreTarget(ScenePerformanceControlTarget::SliceLength);
    appendRestoreTarget(ScenePerformanceControlTarget::Scratch);
    appendRestoreTarget(ScenePerformanceControlTarget::DelayMix);
    appendRestoreTarget(ScenePerformanceControlTarget::DelayTime);
    appendRestoreTarget(ScenePerformanceControlTarget::DelayFeedback);
    appendRestoreTarget(ScenePerformanceControlTarget::DelayLowCut);
    appendRestoreTarget(ScenePerformanceControlTarget::DelayHighCut);
    appendRestoreTarget(ScenePerformanceControlTarget::DelayMode);
    appendRestoreTarget(ScenePerformanceControlTarget::DelaySyncEnabled);
    appendRestoreTarget(ScenePerformanceControlTarget::Rearrange);

    if (stripUsesGrainSceneLanesForSceneSlot(safeSceneSlot, safeStripIndex))
    {
        appendRestoreTarget(ScenePerformanceControlTarget::GrainSize);
        appendRestoreTarget(ScenePerformanceControlTarget::GrainDensity);
        appendRestoreTarget(ScenePerformanceControlTarget::GrainPitchJitter);
        appendRestoreTarget(ScenePerformanceControlTarget::GrainSpread);
        appendRestoreTarget(ScenePerformanceControlTarget::GrainJitter);
        appendRestoreTarget(ScenePerformanceControlTarget::GrainPositionJitter);
        appendRestoreTarget(ScenePerformanceControlTarget::GrainRandomDepth);
        appendRestoreTarget(ScenePerformanceControlTarget::GrainArp);
        appendRestoreTarget(ScenePerformanceControlTarget::GrainCloud);
        appendRestoreTarget(ScenePerformanceControlTarget::GrainEmitter);
        appendRestoreTarget(ScenePerformanceControlTarget::GrainEnvelope);
        appendRestoreTarget(ScenePerformanceControlTarget::GrainShape);
    }

    if (!clearedTargets.empty())
    {
        for (const auto target : restoreTargets)
        {
            if (!sceneAutomationTargetHasEditorLane(target))
                continue;

            events.push_back(makeSceneControlPointEvent(safeStripIndex,
                                                        target,
                                                        0.0,
                                                        defaultSceneControlValue(target)));
        }

        std::sort(events.begin(), events.end());
        if (!replaceScenePerformanceClipEvents(safeSceneSlot, events))
            return false;
    }

    clearSceneMotionStripState(safeSceneSlot, safeStripIndex);
    restoreSceneStripControlTargetsToDefaultState(safeSceneSlot, safeStripIndex, restoreTargets);
    clearPendingSceneTriggerRecord(safeStripIndex);
    updateMonomeLEDs();
    return true;
}

void MlrVSTAudioProcessor::rescaleActiveInnerLoopsForGlobalFactor(int previousChoice, int newChoice)
{
    if (audioEngine == nullptr)
        return;

    const float previousFactor = innerLoopLengthFactorForChoice(previousChoice);
    const float nextFactor = innerLoopLengthFactorForChoice(newChoice);
    if (!(previousFactor > 0.0f) || std::abs(previousFactor - nextFactor) <= 1.0e-6f)
        return;

    for (int stripIndex = 0; stripIndex < MaxStrips; ++stripIndex)
    {
        auto* strip = audioEngine->getStrip(stripIndex);
        if (strip == nullptr)
            continue;

        const auto playMode = strip->getPlayMode();
        if (playMode == EnhancedAudioStrip::PlayMode::Step
            || playMode == EnhancedAudioStrip::PlayMode::Sample)
        {
            continue;
        }

        const int loopStart = juce::jlimit(0, MaxColumns - 1, strip->getLoopStart());
        const int loopEnd = juce::jlimit(loopStart + 1, MaxColumns, strip->getLoopEnd());
        const int currentLength = juce::jmax(1, loopEnd - loopStart);
        if (currentLength >= MaxColumns)
            continue;

        const int nextLength = juce::jlimit(
            1,
            MaxColumns,
            static_cast<int>(std::round(static_cast<double>(currentLength)
                                        * static_cast<double>(nextFactor)
                                        / static_cast<double>(previousFactor))));
        const bool reverse = strip->isReverse()
            || strip->getDirectionMode() == EnhancedAudioStrip::DirectionMode::Reverse;

        int nextStart = loopStart;
        int nextEnd = loopEnd;
        if (reverse)
        {
            nextEnd = loopEnd;
            nextStart = juce::jmax(0, nextEnd - nextLength);
        }
        else
        {
            nextStart = loopStart;
            nextEnd = juce::jmin(MaxColumns, nextStart + nextLength);
        }

        nextStart = juce::jlimit(0, MaxColumns - 1, nextStart);
        nextEnd = juce::jlimit(nextStart + 1, MaxColumns, nextEnd);
        queueLoopChange(stripIndex,
                        false,
                        nextStart,
                        nextEnd,
                        reverse,
                        -1,
                        std::numeric_limits<float>::quiet_NaN());
    }
}

float MlrVSTAudioProcessor::getInnerLoopLengthFactor() const
{
    const int choice = innerLoopLengthParam != nullptr
        ? juce::jlimit(0, 4, static_cast<int>(std::round(innerLoopLengthParam->load(std::memory_order_acquire))))
        : innerLoopLengthSelection.load(std::memory_order_acquire);
    return innerLoopLengthFactorForChoice(choice);
}

void MlrVSTAudioProcessor::queueLoopChange(int stripIndex,
                                           bool clearLoop,
                                           int startColumn,
                                           int endColumn,
                                           bool reverseDirection,
                                           int markerColumn,
                                           float beatsPerLoopOverride)
{
    if (!audioEngine || stripIndex < 0 || stripIndex >= MaxStrips)
        return;

    auto* strip = audioEngine->getStrip(stripIndex);
    if (!strip)
        return;

    const int quantizeDivision = getQuantizeDivision();
    // PPQ safety: clearing an active inner loop must always be grid-scheduled.
    const bool useQuantize = clearLoop || (quantizeDivision > 1);

    if (!useQuantize)
    {
        double currentPpq = audioEngine->getTimelineBeat();
        getCurrentHostPpq(currentPpq);
        if (!std::isfinite(currentPpq))
            currentPpq = audioEngine->getTimelineBeat();

        {
            const juce::ScopedLock lock(pendingLoopChangeLock);
            pendingLoopChanges[static_cast<size_t>(stripIndex)].active = false;
        }

        bool markerApplied = false;
        if (clearLoop)
        {
            strip->clearLoop();
            strip->restoreInnerLoopBaseBeatsPerLoop(currentPpq);
            strip->setReverse(false);
            strip->setDirectionMode(EnhancedAudioStrip::DirectionMode::Normal);
            if (markerColumn >= 0)
            {
                strip->setPlaybackMarkerColumn(markerColumn, audioEngine->getGlobalSampleCount());
                markerApplied = true;
            }
        }
        else
        {
            strip->setLoop(startColumn, endColumn);
            if (std::isfinite(beatsPerLoopOverride) && beatsPerLoopOverride > 0.0f)
                strip->applyInnerLoopBeatsPerLoopOverride(beatsPerLoopOverride, currentPpq);
            else
                strip->restoreInnerLoopBaseBeatsPerLoop(currentPpq);
            strip->setDirectionMode(reverseDirection
                ? EnhancedAudioStrip::DirectionMode::Reverse
                : EnhancedAudioStrip::DirectionMode::Normal);
        }

        if (!markerApplied && strip->isPlaying() && strip->hasAudio())
            strip->snapToTimeline(audioEngine->getGlobalSampleCount());
        return;
    }

    double currentPpq = audioEngine->getTimelineBeat();
    getCurrentHostPpq(currentPpq);

    if (!std::isfinite(currentPpq))
    {
        // Strict PPQ safety: reject quantized loop changes until PPQ is valid.
        return;
    }

    const double quantBeats = 4.0 / static_cast<double>(quantizeDivision);
    double targetPpq = std::ceil(currentPpq / quantBeats) * quantBeats;
    if (targetPpq <= (currentPpq + 1.0e-6))
        targetPpq += quantBeats;
    targetPpq = std::round(targetPpq / quantBeats) * quantBeats;

    const juce::ScopedLock lock(pendingLoopChangeLock);
    auto& pending = pendingLoopChanges[static_cast<size_t>(stripIndex)];
    pending.active = true;
    pending.clear = clearLoop;
    pending.startColumn = juce::jlimit(0, MaxColumns - 1, startColumn);
    pending.endColumn = juce::jlimit(pending.startColumn + 1, MaxColumns, endColumn);
    pending.markerColumn = juce::jlimit(-1, MaxColumns - 1, markerColumn);
    pending.reverse = reverseDirection;
    pending.beatsPerLoopOverride = beatsPerLoopOverride;
    pending.quantized = true;
    pending.targetPpq = targetPpq;
    pending.quantizeDivision = quantizeDivision;
    pending.postClearTriggerArmed = false;
    pending.postClearTriggerColumn = 0;
}

void MlrVSTAudioProcessor::applyPendingLoopChanges(const juce::AudioPlayHead::PositionInfo& posInfo)
{
    if (!audioEngine)
        return;

    double currentPpq = audioEngine->getTimelineBeat();
    if (posInfo.getPpqPosition().hasValue())
        currentPpq = *posInfo.getPpqPosition();
    const double currentTempo = (posInfo.getBpm().hasValue() && *posInfo.getBpm() > 0.0)
        ? *posInfo.getBpm()
        : audioEngine->getCurrentTempo();

    std::array<PendingLoopChange, MaxStrips> readyChanges{};
    {
        const juce::ScopedLock lock(pendingLoopChangeLock);
        for (int i = 0; i < MaxStrips; ++i)
        {
            auto& pending = pendingLoopChanges[static_cast<size_t>(i)];
            if (!pending.active)
                continue;

            bool canApplyNow = false;
            if (!pending.quantized)
            {
                canApplyNow = std::isfinite(currentPpq);
            }
            else if (std::isfinite(currentPpq))
            {
                if (!std::isfinite(pending.targetPpq))
                {
                    const int division = juce::jmax(1, pending.quantizeDivision);
                    const double quantBeats = 4.0 / static_cast<double>(division);
                    double targetPpq = std::ceil(currentPpq / quantBeats) * quantBeats;
                    if (targetPpq <= (currentPpq + 1.0e-6))
                        targetPpq += quantBeats;
                    pending.targetPpq = std::round(targetPpq / quantBeats) * quantBeats;
                    continue;
                }

                auto* strip = audioEngine->getStrip(i);
                const bool hasAnchor = (strip != nullptr) && strip->isPpqTimelineAnchored();
                const bool targetReached = (currentPpq + 1.0e-6 >= pending.targetPpq);
                if (targetReached && !hasAnchor)
                {
                    // Strict PPQ safety: never apply late/off-grid.
                    // If not anchor-safe at this grid, roll to the next grid.
                    const int division = juce::jmax(1, pending.quantizeDivision);
                    const double quantBeats = 4.0 / static_cast<double>(division);
                    double nextTarget = std::ceil(currentPpq / quantBeats) * quantBeats;
                    if (nextTarget <= (currentPpq + 1.0e-6))
                        nextTarget += quantBeats;
                    pending.targetPpq = std::round(nextTarget / quantBeats) * quantBeats;
                    continue;
                }
                canApplyNow = hasAnchor && targetReached;
            }

            if (!canApplyNow)
                continue;

            readyChanges[static_cast<size_t>(i)] = pending;
            pending.active = false;
        }
    }

    const int64_t currentGlobalSample = audioEngine->getGlobalSampleCount();
    for (int i = 0; i < MaxStrips; ++i)
    {
        const auto& change = readyChanges[static_cast<size_t>(i)];
        if (!change.active)
            continue;

        auto* strip = audioEngine->getStrip(i);
        if (!strip)
            continue;

        const double applyPpq = (change.quantized && std::isfinite(change.targetPpq))
            ? change.targetPpq
            : currentPpq;
        bool triggeredAtColumn = false;
        if (change.clear)
        {
            strip->clearLoop();
            strip->restoreInnerLoopBaseBeatsPerLoop(applyPpq);
            strip->setReverse(false);
            strip->setDirectionMode(EnhancedAudioStrip::DirectionMode::Normal);
            if (change.markerColumn >= 0 && std::isfinite(currentPpq) && currentTempo > 0.0)
            {
                juce::AudioPlayHead::PositionInfo retriggerPosInfo;
                retriggerPosInfo.setPpqPosition(applyPpq);
                retriggerPosInfo.setBpm(currentTempo);
                strip->triggerAtSample(change.markerColumn, currentTempo, currentGlobalSample, retriggerPosInfo);
                triggeredAtColumn = true;
            }
            else if (change.markerColumn >= 0)
            {
                strip->setPlaybackMarkerColumn(change.markerColumn, currentGlobalSample);
            }
        }
        else
        {
            strip->setLoop(change.startColumn, change.endColumn);
            if (std::isfinite(change.beatsPerLoopOverride) && change.beatsPerLoopOverride > 0.0f)
                strip->applyInnerLoopBeatsPerLoopOverride(change.beatsPerLoopOverride, applyPpq);
            else
                strip->restoreInnerLoopBaseBeatsPerLoop(applyPpq);
            strip->setDirectionMode(change.reverse
                ? EnhancedAudioStrip::DirectionMode::Reverse
                : EnhancedAudioStrip::DirectionMode::Normal);
        }

        if (change.quantized && !triggeredAtColumn)
        {
            // Deterministic PPQ realign after loop-geometry change.
            const double realignPpq = std::isfinite(currentPpq)
                ? currentPpq
                : (std::isfinite(change.targetPpq) ? change.targetPpq : audioEngine->getTimelineBeat());
            strip->realignToPpqAnchor(realignPpq, currentGlobalSample);
            strip->setBeatsPerLoopAtPpq(strip->getBeatsPerLoop(), realignPpq);
            // The realign is a position snap; blend it instead of clicking.
            if (strip->isPlaying())
                strip->armLoopChangeDeclick();
        }
        else
        {
            const bool markerApplied = (change.clear && change.markerColumn >= 0);
            if (!markerApplied && strip->isPlaying() && strip->hasAudio())
                strip->snapToTimeline(currentGlobalSample);
        }

        // Inner-loop clear gesture: the NEXT pad press while clear is pending
        // becomes the start column after exit, quantized like normal triggers.
        if (change.clear && change.postClearTriggerArmed)
        {
            const int targetColumn = juce::jlimit(0, MaxColumns - 1, change.postClearTriggerColumn);
            const int quantizeDivision = getQuantizeDivision();
            const bool useQuantize = quantizeDivision > 1;
            audioEngine->triggerStripWithQuantization(i, targetColumn, useQuantize);
        }
    }
}

void MlrVSTAudioProcessor::recoverDeferredPpqAnchors(const juce::AudioPlayHead::PositionInfo& posInfo)
{
    if (!audioEngine || !posInfo.getIsPlaying() || !posInfo.getPpqPosition().hasValue())
        return;

    const double currentPpq = *posInfo.getPpqPosition();
    if (!std::isfinite(currentPpq))
        return;

    const int64_t currentGlobalSample = audioEngine->getGlobalSampleCount();
    for (int i = 0; i < MaxStrips; ++i)
    {
        auto* strip = audioEngine->getStrip(i);
        if (!strip || !strip->hasAudio() || !strip->isPlaying() || strip->isPpqTimelineAnchored())
            continue;

        const auto playMode = strip->getPlayMode();
        if (playMode == EnhancedAudioStrip::PlayMode::OneShot
            || playMode == EnhancedAudioStrip::PlayMode::Step
            || playMode == EnhancedAudioStrip::PlayMode::Sample)
        {
            continue;
        }

        // Preserve the strip's current phase and attach it to the host PPQ as soon
        // as the host exposes a valid PPQ position again.
        strip->captureMomentaryPhaseReference(currentPpq);
        strip->enforceMomentaryPhaseReference(currentPpq, currentGlobalSample);
    }
}

void MlrVSTAudioProcessor::applyPendingBarChanges(const juce::AudioPlayHead::PositionInfo& posInfo)
{
    if (!audioEngine)
        return;

    if (!posInfo.getPpqPosition().hasValue())
        return;

    const double currentPpq = *posInfo.getPpqPosition();

    std::array<PendingBarChange, MaxStrips> readyChanges{};
    {
        const juce::ScopedLock lock(pendingBarChangeLock);
        for (int i = 0; i < MaxStrips; ++i)
        {
            auto& pending = pendingBarChanges[static_cast<size_t>(i)];
            if (!pending.active)
                continue;

            auto* strip = audioEngine->getStrip(i);
            const bool stripApplyReady = (strip != nullptr) && strip->hasAudio() && strip->isPlaying();
            const bool anchorReady = stripApplyReady && strip->isPpqTimelineAnchored();

            if (pending.quantized && !std::isfinite(pending.targetPpq))
            {
                if (!std::isfinite(currentPpq) || !anchorReady)
                    continue;

                const int division = juce::jmax(1, pending.quantizeDivision);
                const double quantBeats = 4.0 / static_cast<double>(division);
                double targetPpq = std::ceil(currentPpq / quantBeats) * quantBeats;
                if (targetPpq <= (currentPpq + 1.0e-6))
                    targetPpq += quantBeats;
                pending.targetPpq = std::round(targetPpq / quantBeats) * quantBeats;
                continue;
            }

            bool canApplyNow = false;
            if (!pending.quantized)
            {
                canApplyNow = std::isfinite(currentPpq)
                    && stripApplyReady
                    && strip->isPpqTimelineAnchored();
            }
            else if (std::isfinite(currentPpq) && std::isfinite(pending.targetPpq))
            {
                const bool hasAnchor = stripApplyReady && strip->isPpqTimelineAnchored();
                const bool targetReached = (currentPpq + 1.0e-6 >= pending.targetPpq);

                if (targetReached && !hasAnchor)
                {
                    // Keep the request alive and roll to the next grid if this
                    // strip is not anchor-safe on the current grid.
                    const int division = juce::jmax(1, pending.quantizeDivision);
                    const double quantBeats = 4.0 / static_cast<double>(division);
                    double nextTarget = std::ceil(currentPpq / quantBeats) * quantBeats;
                    if (nextTarget <= (currentPpq + 1.0e-6))
                        nextTarget += quantBeats;
                    pending.targetPpq = std::round(nextTarget / quantBeats) * quantBeats;
                    continue;
                }

                canApplyNow = hasAnchor && targetReached;
            }

            if (!canApplyNow)
                continue;

            readyChanges[static_cast<size_t>(i)] = pending;
            pending.active = false;
        }
    }

    double currentTempo = audioEngine->getCurrentTempo();
    if (posInfo.getBpm().hasValue() && *posInfo.getBpm() > 0.0)
        currentTempo = *posInfo.getBpm();

    const int64_t currentGlobalSample = audioEngine->getGlobalSampleCount();
    bool appliedAnyChanges = false;
    for (int i = 0; i < MaxStrips; ++i)
    {
        const auto& change = readyChanges[static_cast<size_t>(i)];
        if (!change.active)
            continue;

        auto* strip = audioEngine->getStrip(i);
        if (!strip || !strip->hasAudio() || !strip->isPlaying())
            continue;

        const double applyPpq = (change.quantized && std::isfinite(change.targetPpq))
            ? change.targetPpq
            : currentPpq;
        strip->setRecordingBars(change.recordingBars);
        strip->setBeatsPerLoopAtPpq(change.beatsPerLoop, applyPpq);
        if (std::isfinite(applyPpq) && currentTempo > 0.0)
        {
            // Match the preset-restore path so bar remaps re-anchor deterministically.
            strip->restorePresetPpqState(true,
                                         true,
                                         strip->getPpqTimelineOffsetBeats(),
                                         strip->getCurrentColumn(),
                                         currentTempo,
                                         applyPpq,
                                         currentGlobalSample);
        }
        // After target-grid remap, force a hard lock to the *current* host PPQ
        // so trigger/fallback references are consistent within this audio block.
        strip->realignToPpqAnchor(currentPpq, currentGlobalSample);
        appliedAnyChanges = true;
    }

    if (appliedAnyChanges)
        queueActiveSceneAutosave();
}

void MlrVSTAudioProcessor::applyPendingStutterStart(const juce::AudioPlayHead::PositionInfo& posInfo)
{
    if (!audioEngine || pendingStutterStartActive.load(std::memory_order_acquire) == 0)
        return;

    double currentPpq = audioEngine->getTimelineBeat();
    if (posInfo.getPpqPosition().hasValue())
        currentPpq = *posInfo.getPpqPosition();

    double targetPpq = pendingStutterStartPpq.load(std::memory_order_acquire);
    const int64_t currentSample = audioEngine->getGlobalSampleCount();
    const int startQuantizeDivision = juce::jmax(1,
        pendingStutterStartQuantizeDivision.load(std::memory_order_acquire));
    const double startQuantizeBeats = 4.0 / static_cast<double>(startQuantizeDivision);

    // Match inner-loop quantized scheduling:
    // resolve target grid on audio thread to avoid GUI/playhead clock skew.
    if (!(std::isfinite(targetPpq) && targetPpq >= 0.0))
    {
        if (!(std::isfinite(currentPpq) && currentPpq >= 0.0))
            return;

        targetPpq = std::ceil(currentPpq / startQuantizeBeats) * startQuantizeBeats;
        if (targetPpq <= (currentPpq + 1.0e-6))
            targetPpq += startQuantizeBeats;
        targetPpq = std::round(targetPpq / startQuantizeBeats) * startQuantizeBeats;
        pendingStutterStartPpq.store(targetPpq, std::memory_order_release);
        pendingStutterStartSampleTarget.store(-1, std::memory_order_release);
        return;
    }

    if (!(std::isfinite(currentPpq) && currentPpq >= 0.0))
        return;

    if (currentPpq + 1.0e-6 < targetPpq)
        return;

    double applyPpq = targetPpq;

    bool hasAnyPlayingStrip = false;
    bool anchorsReady = true;
    for (int i = 0; i < MaxStrips; ++i)
    {
        auto* strip = audioEngine->getStrip(i);
        const bool stepMode = (strip && strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Step);
        const auto* stepSampler = stepMode && strip ? strip->getStepSampler() : nullptr;
        const bool hasPlayableContent = strip
            && (strip->hasAudio() || (stepSampler && stepSampler->getHasAudio()));
        if (!strip || !hasPlayableContent || !strip->isPlaying())
            continue;
        hasAnyPlayingStrip = true;
        if (!stepMode && !strip->isPpqTimelineAnchored())
        {
            anchorsReady = false;
            break;
        }
    }

    // Mirror inner-loop quantized-apply safety: if anchor isn't valid on this grid,
    // roll to the next global quantize boundary instead of entering off-sync.
    if (hasAnyPlayingStrip && !anchorsReady
        && std::isfinite(currentPpq)
        && std::isfinite(targetPpq))
    {
        double nextTarget = std::ceil(currentPpq / startQuantizeBeats) * startQuantizeBeats;
        if (nextTarget <= (currentPpq + 1.0e-6))
            nextTarget += startQuantizeBeats;
        nextTarget = std::round(nextTarget / startQuantizeBeats) * startQuantizeBeats;
        pendingStutterStartPpq.store(nextTarget, std::memory_order_release);
        pendingStutterStartSampleTarget.store(-1, std::memory_order_release);
        return;
    }

    pendingStutterStartActive.store(0, std::memory_order_release);
    pendingStutterStartPpq.store(-1.0, std::memory_order_release);
    pendingStutterStartSampleTarget.store(-1, std::memory_order_release);

    if (!std::isfinite(applyPpq))
        applyPpq = audioEngine->getTimelineBeat();
    performMomentaryStutterStartNow(applyPpq, currentSample);
}

void MlrVSTAudioProcessor::applyPendingStutterRelease(const juce::AudioPlayHead::PositionInfo& posInfo)
{
    if (!audioEngine || pendingStutterReleaseActive.load(std::memory_order_acquire) == 0)
        return;

    double currentPpq = audioEngine->getTimelineBeat();
    if (posInfo.getPpqPosition().hasValue())
        currentPpq = *posInfo.getPpqPosition();

    const double targetPpq = pendingStutterReleasePpq.load(std::memory_order_acquire);
    const int64_t currentSample = audioEngine->getGlobalSampleCount();
    const int64_t targetSample = pendingStutterReleaseSampleTarget.load(std::memory_order_acquire);

    bool releaseReady = false;
    double applyPpq = currentPpq;

    // Primary path: PPQ-locked release.
    if (std::isfinite(targetPpq) && std::isfinite(currentPpq))
    {
        releaseReady = (currentPpq + 1.0e-6 >= targetPpq);
        applyPpq = targetPpq;
    }
    // Fallback path: sample-target release if PPQ is unavailable.
    else if (targetSample >= 0)
    {
        releaseReady = (currentSample >= targetSample);
    }
    // Safety fallback: never stay latched forever when host is not playing.
    else if (!posInfo.getIsPlaying())
    {
        releaseReady = true;
    }

    if (!releaseReady)
        return;

    pendingStutterReleaseActive.store(0, std::memory_order_release);
    pendingStutterReleasePpq.store(-1.0, std::memory_order_release);
    pendingStutterReleaseSampleTarget.store(-1, std::memory_order_release);

    if (!std::isfinite(applyPpq))
        applyPpq = audioEngine->getTimelineBeat();
    performMomentaryStutterReleaseNow(applyPpq, currentSample);
}

void MlrVSTAudioProcessor::captureMomentaryStutterMacroBaseline()
{
    if (!audioEngine)
        return;

    for (int i = 0; i < MaxStrips; ++i)
    {
        const auto idx = static_cast<size_t>(i);
        auto& saved = momentaryStutterSavedState[idx];
        saved = MomentaryStutterSavedStripState{};

        auto* strip = audioEngine->getStrip(i);
        const bool stepMode = (strip && strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Step);
        const auto* stepSampler = stepMode && strip ? strip->getStepSampler() : nullptr;
        const bool hasPlayableContent = strip
            && (strip->hasAudio() || (stepSampler && stepSampler->getHasAudio()));
        if (!strip || !momentaryStutterStripArmed[idx] || !hasPlayableContent || !strip->isPlaying())
            continue;

        saved.valid = true;
        saved.stepMode = stepMode;
        saved.pan = (stepSampler != nullptr) ? stepSampler->getPan() : strip->getPan();
        saved.playbackSpeed = strip->getPlaybackSpeed();
        saved.pitchSemitones = getPitchSemitonesForDisplay(*strip);
        saved.pitchShift = strip->getPitchShift();
        saved.loopSliceLength = strip->getLoopSliceLength();
        saved.filterEnabled = strip->isFilterEnabled();
        saved.filterFrequency = strip->getFilterFrequency();
        saved.filterResonance = strip->getFilterResonance();
        saved.filterMorph = strip->getFilterMorph();
        saved.filterAlgorithm = strip->getFilterAlgorithm();
        if (stepSampler != nullptr)
        {
            saved.stepFilterEnabled = stepSampler->isFilterEnabled();
            saved.stepFilterFrequency = stepSampler->getFilterFrequency();
            saved.stepFilterResonance = stepSampler->getFilterResonance();
            saved.stepFilterType = stepSampler->getFilterType();
        }
    }

    momentaryStutterMacroBaselineCaptured = true;
    momentaryStutterMacroCapturePending = false;
}

void MlrVSTAudioProcessor::applyMomentaryStutterMacro(const juce::AudioPlayHead::PositionInfo& posInfo)
{
    if (!audioEngine
        || !momentaryStutterHoldActive
        || momentaryStutterPlaybackActive.load(std::memory_order_acquire) == 0)
        return;

    if (!posInfo.getPpqPosition().hasValue())
        return;

    const double ppqNow = *posInfo.getPpqPosition();
    if (!std::isfinite(ppqNow))
        return;

    if (momentaryStutterMacroCapturePending || !momentaryStutterMacroBaselineCaptured)
        captureMomentaryStutterMacroBaseline();
    if (!momentaryStutterMacroBaselineCaptured)
        return;

    uint8_t comboMask = static_cast<uint8_t>(momentaryStutterButtonMask.load(std::memory_order_acquire) & 0x7f);
    if (comboMask == 0)
        comboMask = stutterButtonBitFromColumn(momentaryStutterActiveDivisionButton);
    if (comboMask == 0)
        return;

    const int bitCount = countStutterBits(comboMask);
    const int highestBit = highestStutterBit(comboMask);
    const int lowestBit = lowestStutterBit(comboMask);
    const bool comboChanged = (comboMask != momentaryStutterLastComboMask);
    const int seed = (static_cast<int>(comboMask) * 97)
        + (bitCount * 19)
        + (highestBit * 11)
        + (lowestBit * 5);
    const int variant = seed % 8;
    const bool singleButton = (bitCount == 1);
    const bool multiButton = (bitCount >= 2);
    const bool twoButton = (bitCount == 2);
    const bool allowPitchSpeedMacro = (bitCount >= 3);
    const bool allowPitchMacro = (bitCount >= 3);
    const bool applySpeedMacro = (bitCount >= 2);
    const bool threeButton = (bitCount == 3);
    const bool hardStepMode = (variant >= 4);
    const int stutterComboFlatIndex = getStutterGestureComboFlatIndexFromMask(comboMask);
    auto restoreSavedState = [this](int stripIndex,
                                    EnhancedAudioStrip& strip,
                                    const MomentaryStutterSavedStripState& saved,
                                    bool restoreSpeedImmediately = true)
    {
        setStripPanControlValue(stripIndex, saved.pan, StripControlWriteMode::CacheOnly);
        if (restoreSpeedImmediately)
            strip.setPlaybackSpeedImmediate(saved.playbackSpeed);
        strip.setLoopSliceLength(saved.loopSliceLength);

        if (saved.stepMode)
        {
            applyPitchControlToStrip(stripIndex, strip, saved.pitchSemitones);
        }
        else
        {
            applyPitchControlToStrip(stripIndex, strip, saved.pitchShift);
        }

        setStripFilterAlgorithmControlValue(stripIndex, saved.filterAlgorithm, StripControlWriteMode::CacheOnly);
        setStripFilterFrequencyControlValue(stripIndex, saved.filterFrequency, StripControlWriteMode::CacheOnly);
        setStripFilterResonanceControlValue(stripIndex, saved.filterResonance, StripControlWriteMode::CacheOnly);
        setStripFilterMorphControlValue(stripIndex, saved.filterMorph, StripControlWriteMode::CacheOnly);
        setStripFilterEnabledControlValue(stripIndex, saved.filterEnabled, StripControlWriteMode::CacheOnly);
        if (saved.stepMode)
        {
            if (auto* stepSampler = strip.getStepSampler())
                stepSampler->setFilterType(saved.stepFilterType);
        }
    };

    if (singleButton)
    {
        const float singlePhase = static_cast<float>(wrapUnitPhase(
            juce::jmax(0.0, ppqNow - momentaryStutterMacroStartPpq) / 4.0));
        const float gestureSpeedLane = gestureCoordinator->sampleComboLane(
            GestureComboKind::Stutter, stutterComboFlatIndex, static_cast<int>(StutterGestureLane::Speed), singlePhase);
        const float gesturePitchLane = gestureCoordinator->sampleComboLane(
            GestureComboKind::Stutter, stutterComboFlatIndex, static_cast<int>(StutterGestureLane::Pitch), singlePhase);
        const float gesturePanLane = gestureCoordinator->sampleComboLane(
            GestureComboKind::Stutter, stutterComboFlatIndex, static_cast<int>(StutterGestureLane::Pan), singlePhase);
        const float gestureCutoffLane = gestureCoordinator->sampleComboLane(
            GestureComboKind::Stutter, stutterComboFlatIndex, static_cast<int>(StutterGestureLane::Cutoff), singlePhase);
        const float gestureResonanceLane = gestureCoordinator->sampleComboLane(
            GestureComboKind::Stutter, stutterComboFlatIndex, static_cast<int>(StutterGestureLane::Resonance), singlePhase);
        const float gestureMorphLane = gestureCoordinator->sampleComboLane(
            GestureComboKind::Stutter, stutterComboFlatIndex, static_cast<int>(StutterGestureLane::Morph), singlePhase);
        const float gestureDivisionLane = gestureCoordinator->sampleComboLane(
            GestureComboKind::Stutter, stutterComboFlatIndex, static_cast<int>(StutterGestureLane::Division), singlePhase);
        const float gestureSpeedRatio = std::pow(2.0f, gestureSpeedLane * 0.85f);
        const float gesturePitchOffset = gesturePitchLane * 12.0f;
        const float gesturePanOffset = gesturePanLane * 0.72f;
        const bool gestureFilterActive = std::abs(gestureCutoffLane) > 0.04f
            || std::abs(gestureResonanceLane) > 0.04f
            || std::abs(gestureMorphLane) > 0.04f;
        const float gestureFilterNorm = juce::jlimit(0.0f, 1.0f, 0.72f + (gestureCutoffLane * 0.24f));
        const float gestureFilterMorph = juce::jlimit(0.0f, 1.0f, 0.30f + (gestureMorphLane * 0.30f));
        const float gestureFilterResonance = juce::jlimit(0.2f, 2.2f, 0.72f + (gestureResonanceLane * 0.75f));
        const double gestureDivision = juce::jlimit(0.03125,
                                                    4.0,
                                                    stutterDivisionBeatsFromBit(highestBit)
                                                        * std::pow(2.0, -gestureDivisionLane * 1.35f));

        audioEngine->setMomentaryStutterDivision(gestureDivision);
        audioEngine->setMomentaryStutterRetriggerFadeMs(0.7f);

        for (int i = 0; i < MaxStrips; ++i)
        {
            const auto idx = static_cast<size_t>(i);
            const auto& saved = momentaryStutterSavedState[idx];
            if (!saved.valid || !momentaryStutterStripArmed[idx])
                continue;

            auto* strip = audioEngine->getStrip(i);
            const bool stepMode = (strip && strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Step);
            const auto* stepSampler = stepMode && strip ? strip->getStepSampler() : nullptr;
            const bool hasPlayableContent = strip
                && (strip->hasAudio() || (stepSampler && stepSampler->getHasAudio()));
            if (!strip || !hasPlayableContent || !strip->isPlaying())
                continue;

            const bool liveStepMode = (strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Step);
            restoreSavedState(i, *strip, saved, liveStepMode);
            const float targetPan = juce::jlimit(-1.0f, 1.0f, saved.pan + gesturePanOffset);
            setStripPanControlValue(i, targetPan, StripControlWriteMode::CacheOnly);

            if (!liveStepMode)
            {
                // Keep stutter speed expressive, but route it through the strip smoother
                // so pressing a gesture does not hard-jump into a crunchy resample artifact.
                strip->setPlaybackSpeed(
                    juce::jlimit(0.03125f, 8.0f, saved.playbackSpeed * gestureSpeedRatio));
            }

            const float pitchBase = saved.stepMode ? saved.pitchSemitones : saved.pitchShift;
            applyPitchControlToStrip(i, *strip, juce::jlimit(-24.0f, 24.0f, pitchBase + gesturePitchOffset));

            if (gestureFilterActive)
            {
                setStripFilterEnabledControlValue(i, true, StripControlWriteMode::CacheOnly);
                setStripFilterAlgorithmControlValue(i, saved.filterAlgorithm, StripControlWriteMode::CacheOnly);
                setStripFilterFrequencyControlValue(i, cutoffFromNormalized(gestureFilterNorm), StripControlWriteMode::CacheOnly);
                setStripFilterResonanceControlValue(i, gestureFilterResonance, StripControlWriteMode::CacheOnly);
                setStripFilterMorphControlValue(i, gestureFilterMorph, StripControlWriteMode::CacheOnly);
                if (auto* activeStepSampler = strip->getStepSampler())
                {
                    const auto stepFilterType = (gestureFilterMorph < 0.34f)
                        ? FilterType::LowPass
                        : (gestureFilterMorph > 0.66f ? FilterType::HighPass
                                                      : FilterType::BandPass);
                    activeStepSampler->setFilterType(stepFilterType);
                }
            }
        }

        momentaryStutterLastComboMask = comboMask;
        momentaryStutterTwoButtonStepBaseValid = false;
        momentaryStutterTwoButtonStepBase = 0;
        return;
    }

    int lengthBars = 1 + ((seed / 13) % 4);
    if (twoButton)
        lengthBars = 1 + (((seed / 31) + highestBit + lowestBit) & 0x3);
    else if (bitCount >= 4)
        lengthBars = juce::jlimit(2, 4, 2 + (((seed / 17) + highestBit) & 0x1));
    const double cycleBeats = 4.0 * static_cast<double>(lengthBars);
    if (cycleBeats <= 0.0 || !std::isfinite(cycleBeats))
        return;

    const double cycleBeatPosRaw = std::fmod(ppqNow - momentaryStutterMacroStartPpq, cycleBeats);
    const double cycleBeatPos = cycleBeatPosRaw < 0.0 ? cycleBeatPosRaw + cycleBeats : cycleBeatPosRaw;
    const double phase = wrapUnitPhase(cycleBeatPos / cycleBeats);
    const int threeButtonContour = threeButton
        ? (((seed / 29) + variant + highestBit + (lowestBit * 2)) % 4)
        : 0;
    int stepsPerBar = 8;
    if (multiButton)
    {
        const int rhythmClass = ((seed / 7) + highestBit + lowestBit) % 4;
        if (rhythmClass == 1)
            stepsPerBar = 16;
    }
    const int totalSteps = juce::jmax(8, stepsPerBar * lengthBars);
    const int stepIndex = juce::jlimit(0, totalSteps - 1, static_cast<int>(std::floor(phase * static_cast<double>(totalSteps))));
    const int stepLoop = stepIndex % 8;
    const float normStep = static_cast<float>(stepLoop) / 7.0f;

    const uint8_t maskBit10 = static_cast<uint8_t>(1u << 1);
    const uint8_t maskBit12 = static_cast<uint8_t>(1u << 3);
    const uint8_t maskBit13 = static_cast<uint8_t>(1u << 4);
    const uint8_t maskBit15 = static_cast<uint8_t>(1u << 6);
    const uint8_t maskBit11 = static_cast<uint8_t>(1u << 2);
    const bool combo10And13 = (comboMask == static_cast<uint8_t>(maskBit10 | maskBit13));
    const bool combo11And13 = (comboMask == static_cast<uint8_t>(maskBit11 | maskBit13));
    const bool combo12And13And15 = (comboMask == static_cast<uint8_t>(maskBit12 | maskBit13 | maskBit15));
    const bool hasTopStutterBit = ((comboMask & maskBit15) != 0);
    const float comboIntensity = juce::jlimit(0.25f, 1.0f, 0.34f + (0.16f * static_cast<float>(bitCount - 1)));
    const double heldBeatsRaw = juce::jmax(0.0, ppqNow - momentaryStutterMacroStartPpq);
    const float heldRamp = juce::jlimit(0.0f, 1.0f, static_cast<float>(heldBeatsRaw / 8.0));

    float shapeIntensity = 1.0f;
    float speedMult = 1.0f;
    float panPattern = 0.0f;
    float pitchPattern = 0.0f;
    float cutoffNorm = 0.85f;
    float targetResonance = 1.2f;
    float targetMorph = 0.25f;
    float panDepthShape = 1.0f;
    float twoButtonSemitoneStep = 0.0f;
    float twoButtonSemitoneSpeedRatio = 1.0f;
    bool twoButtonUseFilter = true;
    bool twoButtonDirectionUp = true;
    int twoButtonStepAbs = 0;
    double dynamicStutterDivisionBeats = stutterDivisionBeatsFromBitForMacro(highestBit, multiButton);

    if (variant < 4)
    {
        // Smooth musical movement modes (continuous phase paths).
        const double fastPhase = wrapUnitPhase(phase * static_cast<double>(2 + ((seed >> 2) % 5)));
        const double panPhase = wrapUnitPhase(phase * static_cast<double>(1 + ((seed >> 4) % 4)));
        const double filterPhase = wrapUnitPhase(phase * static_cast<double>(1 + ((seed >> 6) % 3)));
        const double tri = 1.0 - std::abs((phase * 2.0) - 1.0);
        const double triSigned = (tri * 2.0) - 1.0;
        const double sawSigned = (phase * 2.0) - 1.0;
        const double sine = std::sin(juce::MathConstants<double>::twoPi * phase);
        const double sineFast = std::sin(juce::MathConstants<double>::twoPi * fastPhase);
        const double panSine = std::sin(juce::MathConstants<double>::twoPi * panPhase);
        const double filterTri = 1.0 - std::abs((filterPhase * 2.0) - 1.0);

        switch (variant)
        {
            case 0: // riser
                shapeIntensity = juce::jlimit(0.18f, 1.0f, static_cast<float>(phase));
                speedMult = juce::jlimit(0.70f, 2.40f, static_cast<float>(0.95 + (0.95 * phase) + (0.18 * sineFast)));
                panPattern = static_cast<float>(0.48 * panSine);
                pitchPattern = static_cast<float>(-1.0 + (11.5 * phase) + (1.8 * sineFast));
                cutoffNorm = static_cast<float>(0.18 + (0.78 * phase));
                targetResonance = static_cast<float>(0.9 + (2.9 * filterTri));
                targetMorph = static_cast<float>(0.12 + (0.58 * filterPhase));
                break;
            case 1: // faller
                shapeIntensity = juce::jlimit(0.18f, 1.0f, static_cast<float>(1.0 - phase));
                speedMult = juce::jlimit(0.70f, 2.30f, static_cast<float>(1.90 - (1.00 * phase) + (0.16 * sine)));
                panPattern = static_cast<float>(0.72 * triSigned);
                pitchPattern = static_cast<float>(8.0 - (14.0 * phase) + (1.3 * sine));
                cutoffNorm = static_cast<float>(0.92 - (0.70 * phase));
                targetResonance = static_cast<float>(1.1 + (3.1 * phase));
                targetMorph = static_cast<float>(0.88 - (0.62 * filterPhase));
                break;
            case 2: // swirl
                shapeIntensity = juce::jlimit(0.20f, 1.0f, static_cast<float>(tri));
                speedMult = juce::jlimit(0.75f, 2.15f, static_cast<float>(1.0
                    + (0.42 * std::sin(juce::MathConstants<double>::twoPi * phase * 2.0))
                    + (0.14 * sineFast)));
                panPattern = static_cast<float>(0.80 * std::sin(juce::MathConstants<double>::twoPi * (panPhase * 2.0)));
                pitchPattern = static_cast<float>((6.0 * sine) + (3.0 * std::sin(juce::MathConstants<double>::twoPi * (phase + 0.25))));
                cutoffNorm = static_cast<float>(0.24 + (0.66 * filterTri));
                targetResonance = static_cast<float>(0.9 + (2.5 * wrapUnitPhase(filterPhase * 2.0)));
                targetMorph = static_cast<float>(0.50 + (0.40 * std::sin(juce::MathConstants<double>::twoPi * filterPhase)));
                break;
            case 3:
            default: // surge
                shapeIntensity = juce::jlimit(0.22f, 1.0f, static_cast<float>(0.55 + (0.45 * std::abs(sineFast))));
                speedMult = juce::jlimit(0.70f, 2.40f, static_cast<float>(1.0 + (0.95 * triSigned) + (0.14 * sineFast)));
                panPattern = static_cast<float>(0.90 * sawSigned);
                pitchPattern = static_cast<float>((9.0 * sine) + (4.5 * triSigned));
                cutoffNorm = static_cast<float>(0.14 + (0.80 * wrapUnitPhase(phase + (0.25 * juce::jmax(0.0, sine)))));
                targetResonance = static_cast<float>(1.0 + (3.0 * wrapUnitPhase(filterPhase + (0.20 * triSigned))));
                targetMorph = static_cast<float>(wrapUnitPhase((0.40 * phase) + (0.60 * filterPhase)));
                break;
        }
    }
    else
    {
        // Hard step modes (deterministic rhythmic snapshots).
        static constexpr std::array<std::array<float, 8>, 8> kSpeedPatterns{{
            {{ 1.00f, 1.25f, 1.50f, 1.75f, 1.50f, 1.25f, 1.00f, 0.85f }},
            {{ 1.00f, 0.90f, 1.10f, 1.35f, 1.60f, 1.35f, 1.10f, 0.90f }},
            {{ 1.00f, 1.12f, 1.25f, 1.38f, 1.50f, 1.62f, 1.75f, 1.50f }},
            {{ 1.00f, 1.50f, 1.00f, 1.25f, 1.00f, 1.75f, 1.00f, 1.50f }},
            {{ 1.00f, 1.15f, 1.30f, 1.45f, 1.30f, 1.15f, 1.00f, 0.90f }},
            {{ 1.00f, 0.85f, 1.00f, 1.35f, 1.00f, 1.55f, 1.20f, 1.00f }},
            {{ 1.00f, 1.20f, 1.45f, 1.20f, 0.95f, 1.20f, 1.45f, 1.70f }},
            {{ 1.00f, 1.33f, 1.67f, 1.33f, 1.00f, 0.90f, 1.10f, 1.30f }}
        }};
        static constexpr std::array<std::array<float, 8>, 8> kPanPatterns{{
            {{ -1.00f, 1.00f, -0.80f, 0.80f, -0.60f, 0.60f, -0.35f, 0.35f }},
            {{ -0.70f, -0.30f, 0.30f, 0.70f, 1.00f, 0.70f, 0.30f, -0.30f }},
            {{ -1.00f, -0.60f, -0.20f, 0.20f, 0.60f, 1.00f, 0.40f, -0.20f }},
            {{ -1.00f, 1.00f, -1.00f, 1.00f, -0.50f, 0.50f, -0.20f, 0.20f }},
            {{ -0.25f, -0.75f, -1.00f, -0.50f, 0.50f, 1.00f, 0.75f, 0.25f }},
            {{ -0.90f, -0.20f, 0.90f, 0.20f, -0.90f, -0.20f, 0.90f, 0.20f }},
            {{ -0.40f, 0.40f, -0.70f, 0.70f, -1.00f, 1.00f, -0.60f, 0.60f }},
            {{ -1.00f, -0.50f, 0.00f, 0.50f, 1.00f, 0.50f, 0.00f, -0.50f }}
        }};
        static constexpr std::array<std::array<float, 8>, 8> kPitchPatterns{{
            {{ 0.0f, 2.0f, 5.0f, 7.0f, 10.0f, 7.0f, 5.0f, 2.0f }},
            {{ 0.0f, -2.0f, 3.0f, 5.0f, 8.0f, 5.0f, 3.0f, -2.0f }},
            {{ 0.0f, 3.0f, 7.0f, 10.0f, 12.0f, 10.0f, 7.0f, 3.0f }},
            {{ 0.0f, 5.0f, 0.0f, 7.0f, 0.0f, 10.0f, 0.0f, 12.0f }},
            {{ 0.0f, 2.0f, 4.0f, 7.0f, 9.0f, 7.0f, 4.0f, 2.0f }},
            {{ 0.0f, -3.0f, 0.0f, 4.0f, 7.0f, 4.0f, 0.0f, -3.0f }},
            {{ 0.0f, 1.0f, 5.0f, 8.0f, 12.0f, 8.0f, 5.0f, 1.0f }},
            {{ 0.0f, 4.0f, 7.0f, 11.0f, 7.0f, 4.0f, 2.0f, 0.0f }}
        }};

        const int patternBank = ((seed / 5) + (bitCount * 3) + highestBit + lowestBit) % 8;
        const auto& speedPattern = kSpeedPatterns[static_cast<size_t>((variant + patternBank) % 8)];
        const auto& panPatternTable = kPanPatterns[static_cast<size_t>((variant + highestBit + patternBank) % 8)];
        const auto& pitchPatternTable = kPitchPatterns[static_cast<size_t>((variant + lowestBit + (patternBank * 2)) % 8)];

        switch (variant % 4)
        {
            case 0: shapeIntensity = juce::jlimit(0.15f, 1.0f, normStep); break; // rise
            case 1: shapeIntensity = juce::jlimit(0.15f, 1.0f, 1.0f - normStep); break; // fall
            case 2: shapeIntensity = juce::jlimit(0.15f, 1.0f, 1.0f - std::abs((normStep * 2.0f) - 1.0f)); break; // triangle
            case 3:
            default: shapeIntensity = (stepLoop & 1) == 0 ? 1.0f : 0.45f; break; // pulse
        }

        speedMult = speedPattern[static_cast<size_t>(stepLoop)];
        panPattern = panPatternTable[static_cast<size_t>(stepLoop)];
        pitchPattern = pitchPatternTable[static_cast<size_t>(stepLoop)];
        cutoffNorm = juce::jlimit(0.10f, 1.0f, 0.25f + (0.70f * normStep));
        targetResonance = 0.9f + (3.2f * shapeIntensity);
        targetMorph = juce::jlimit(0.05f, 0.95f, 0.10f + (0.80f * normStep));

        // Hard-step variants escalate while held to create stronger breakdown/riser motion.
        const float hardExtreme = juce::jlimit(1.0f, 2.1f, 1.0f + (1.1f * heldRamp));
        shapeIntensity = juce::jlimit(0.15f, 1.0f, shapeIntensity + (0.50f * heldRamp));
        speedMult = 1.0f + ((speedMult - 1.0f) * hardExtreme);
        panPattern = juce::jlimit(-1.0f, 1.0f, panPattern * (1.0f + (0.45f * heldRamp)));
        pitchPattern = juce::jlimit(-18.0f, 18.0f, pitchPattern * (1.0f + (0.95f * heldRamp)));
        targetResonance = juce::jlimit(0.2f, 8.0f, targetResonance + (2.1f * heldRamp));
        targetMorph = juce::jlimit(0.02f, 0.98f, targetMorph + (0.14f * heldRamp));
    }

    if (allowPitchSpeedMacro)
    {
        // Hard-step speed scenes are always available for >2-button holds.
        static constexpr std::array<std::array<float, 8>, 4> kHardSpeedScenes {{
            {{ 0.30f, 0.55f, 1.15f, 2.20f, 3.40f, 2.40f, 1.20f, 0.45f }},
            {{ 1.00f, 0.35f, 0.70f, 1.60f, 3.20f, 2.20f, 1.10f, 0.40f }},
            {{ 3.40f, 2.40f, 1.60f, 1.00f, 0.50f, 0.75f, 1.35f, 2.20f }},
            {{ 0.28f, 0.50f, 0.85f, 1.50f, 2.60f, 3.60f, 1.80f, 0.42f }}
        }};
        const int hardSceneIdx = ((seed / 9) + highestBit + (lowestBit * 2)) % 4;
        const float hardStepSpeed = kHardSpeedScenes[static_cast<size_t>(hardSceneIdx)][static_cast<size_t>(stepLoop)];
        float hardMix = (variant >= 4) ? 0.76f : 0.42f;
        hardMix += 0.22f * heldRamp;
        if (threeButton)
            hardMix += 0.12f;
        hardMix = juce::jlimit(0.0f, 1.0f, hardMix);
        speedMult = juce::jmap(hardMix, speedMult, hardStepSpeed);
    }

    if (threeButton)
    {
        // 3-button combos start from a stronger base before contour shaping.
        shapeIntensity = juce::jlimit(0.2f, 1.0f, shapeIntensity + 0.20f + (0.25f * heldRamp));
        speedMult = juce::jlimit(0.25f, 4.0f, speedMult * (1.08f + (0.42f * heldRamp)));
        panPattern = juce::jlimit(-1.0f, 1.0f, panPattern * (1.20f + (0.35f * heldRamp)));
        pitchPattern = juce::jlimit(-14.0f, 14.0f, pitchPattern * (1.04f + (0.18f * heldRamp)));
    }

    if (!allowPitchSpeedMacro && hardStepMode)
    {
        // Hard-step depth envelope for 1/2-button stutters.
        // 1-button: subtle pan-only growth.
        // 2-button: stronger growth for pan + filter shape over hold time.
        const float hardDepth = juce::jlimit(0.0f, 1.0f, std::pow(heldRamp, 1.35f));
        if (singleButton)
        {
            panDepthShape = juce::jlimit(0.08f, 0.24f, 0.08f + (0.16f * hardDepth));
        }
        else
        {
            const float twoButtonDepth = juce::jlimit(0.28f, 1.0f, 0.28f + (0.72f * hardDepth));
            panDepthShape = twoButtonDepth;
            const float stepPolarity = ((stepLoop & 1) == 0) ? 1.0f : -1.0f;
            cutoffNorm = juce::jlimit(0.0f, 1.0f, cutoffNorm + (0.16f * twoButtonDepth * stepPolarity));
            targetMorph = juce::jlimit(0.0f, 1.0f, targetMorph + (0.18f * twoButtonDepth * stepPolarity));
            targetResonance = juce::jlimit(0.2f, 2.1f, targetResonance + (0.45f * twoButtonDepth));
        }
    }
    else if (singleButton)
    {
        // One-button stutter should remain mostly clean and centered.
        panDepthShape = 0.10f;
    }

    if (twoButton)
    {
        // Two-finger mode:
        // - dedicated speed/pitch up/down gestures,
        // - dynamic retrigger-rate movement over a 1..4 bar phrase,
        // - always starts from the current strip speed baseline.
        const int twoButtonMode = ((seed / 7) + (highestBit * 3) + lowestBit) & 0x7;
        twoButtonDirectionUp = ((twoButtonMode & 0x1) == 0);
        twoButtonUseFilter = false;
        const float phaseNorm = juce::jlimit(0.0f, 1.0f, static_cast<float>(phase));

        const double slowDivision = (twoButtonMode <= 1) ? 0.5 : 0.25;
        const double fastDivision = 0.125;
        const float phraseProgress = twoButtonDirectionUp ? phaseNorm : (1.0f - phaseNorm);
        const float gestureDrive = juce::jlimit(0.0f, 1.0f,
            phraseProgress * (0.45f + (0.55f * heldRamp)));
        const float shapedDrive = std::pow(gestureDrive, (twoButtonMode >= 4) ? 0.66f : 1.15f);
        dynamicStutterDivisionBeats = juce::jmap(static_cast<double>(shapedDrive), slowDivision, fastDivision);

        const double elapsedBeats = juce::jmax(0.0, ppqNow - momentaryStutterMacroStartPpq);
        const double stepPos = elapsedBeats / juce::jmax(0.03125, dynamicStutterDivisionBeats);
        const int globalTwoButtonStep = juce::jmax(0, static_cast<int>(std::floor((std::isfinite(stepPos) ? stepPos : 0.0) + 1.0e-6)));
        if (comboChanged || !momentaryStutterTwoButtonStepBaseValid)
        {
            momentaryStutterTwoButtonStepBase = globalTwoButtonStep;
            momentaryStutterTwoButtonStepBaseValid = true;
        }
        twoButtonStepAbs = juce::jmax(0, globalTwoButtonStep - momentaryStutterTwoButtonStepBase);
        const int semitoneStride = (twoButtonMode >= 4) ? 2 : 1;
        const int twoButtonMaxSemitones = (twoButtonMode <= 1) ? 36 : 24;
        int pacedStepAbs = twoButtonStepAbs;
        if (twoButtonMode >= 2)
        {
            const float paceScale = juce::jlimit(0.125f, 1.0f,
                static_cast<float>(dynamicStutterDivisionBeats / slowDivision));
            const float pacedContinuous = static_cast<float>(twoButtonStepAbs) * paceScale;
            pacedStepAbs = juce::jmax(0, static_cast<int>(std::floor(pacedContinuous + 1.0e-4f)));
        }

        const int linearSemitoneStep = juce::jlimit(0, twoButtonMaxSemitones, pacedStepAbs * semitoneStride);
        int semitoneStep = linearSemitoneStep;
        if (twoButtonMode >= 2)
        {
            const float expoK = (twoButtonMode >= 6) ? 0.74f
                : (twoButtonMode >= 4 ? 0.58f
                                      : (twoButtonMode >= 2 ? 0.36f : 0.30f));
            const float expoNorm = juce::jlimit(0.0f, 1.0f,
                1.0f - std::exp(-expoK * static_cast<float>(pacedStepAbs)));
            const int maxExpoStep = juce::jmax(1, twoButtonMaxSemitones / semitoneStride);
            const int expoStepIndex = juce::jlimit(0, maxExpoStep, static_cast<int>(std::round(expoNorm * static_cast<float>(maxExpoStep))));
            const int expoSemitoneStep = juce::jlimit(0, twoButtonMaxSemitones, expoStepIndex * semitoneStride);
            semitoneStep = juce::jmax(linearSemitoneStep, expoSemitoneStep);
        }
        twoButtonSemitoneStep = static_cast<float>(twoButtonDirectionUp ? semitoneStep : -semitoneStep);

        const float panDepthStep = juce::jlimit(0.0f, 1.0f,
            0.28f + (static_cast<float>(semitoneStep) / 18.0f));
        panDepthShape = panDepthStep;
        twoButtonSemitoneSpeedRatio = std::pow(2.0f, twoButtonSemitoneStep / 12.0f);
        cutoffNorm = 1.0f;
        targetMorph = 0.0f;
        targetResonance = 0.72f;
    }
    else
    {
        momentaryStutterTwoButtonStepBaseValid = false;
        momentaryStutterTwoButtonStepBase = 0;
    }

    // Multi-button combos add infinite ramp movement layers (looping every cycle)
    // that continue until release: retrigger-rate sweeps + coordinated speed/filter ramps.
    if (multiButton && !twoButton)
    {
        const float phaseNorm = static_cast<float>(phase);
        const float rampUp = juce::jlimit(0.0f, 1.0f, phaseNorm);
        const float rampDown = 1.0f - rampUp;
        const float rampPingPong = juce::jlimit(0.0f, 1.0f, static_cast<float>(1.0 - std::abs((phase * 2.0) - 1.0)));
        const float heldDrive = juce::jlimit(0.20f, 1.0f, 0.35f + (0.65f * heldRamp));

        const double baseDivision = juce::jlimit(0.125, 1.0, dynamicStutterDivisionBeats);
        const double minFastDivision = 0.125;
        const double fastDivision = juce::jlimit(minFastDivision, 1.0, baseDivision * (threeButton ? 0.30 : 0.42));
        const double slowDivision = juce::jlimit(0.125, 2.0, baseDivision * (threeButton ? 2.25 : 1.85));

        const int rampMode = ((seed / 17) + bitCount + highestBit + lowestBit) % 4;
        switch (rampMode)
        {
            case 0: // accel + high-pass rise
            {
                const float amt = rampUp * heldDrive;
                dynamicStutterDivisionBeats = juce::jmap(static_cast<double>(amt), baseDivision, fastDivision);
                if (allowPitchSpeedMacro)
                    speedMult = juce::jlimit(0.35f, 4.0f, speedMult * (1.0f + (1.35f * amt)));
                cutoffNorm = juce::jlimit(0.0f, 1.0f, amt);
                targetMorph = 1.0f; // High-pass
                targetResonance = juce::jlimit(0.2f, 8.0f, targetResonance + (1.0f * amt));
                break;
            }
            case 1: // accel + low-pass fall
            {
                const float amt = rampUp * heldDrive;
                dynamicStutterDivisionBeats = juce::jmap(static_cast<double>(amt), baseDivision, fastDivision);
                if (allowPitchSpeedMacro)
                    speedMult = juce::jlimit(0.35f, 4.0f, speedMult * (1.0f + (1.20f * amt)));
                cutoffNorm = juce::jlimit(0.0f, 1.0f, 1.0f - amt);
                targetMorph = 0.0f; // Low-pass
                targetResonance = juce::jlimit(0.2f, 8.0f, targetResonance + (0.7f * amt));
                break;
            }
            case 2: // decel + low-pass fall
            {
                const float amt = rampUp * heldDrive;
                dynamicStutterDivisionBeats = juce::jmap(static_cast<double>(amt), baseDivision, slowDivision);
                if (allowPitchSpeedMacro)
                    speedMult = juce::jlimit(0.35f, 4.0f, speedMult * (1.0f - (0.58f * amt)));
                cutoffNorm = juce::jlimit(0.0f, 1.0f, 1.0f - amt);
                targetMorph = 0.0f; // Low-pass
                targetResonance = juce::jlimit(0.2f, 8.0f, targetResonance + (0.6f * amt));
                break;
            }
            case 3:
            default: // infinite up/down ping-pong ramp
            {
                const float amt = rampPingPong * heldDrive;
                dynamicStutterDivisionBeats = juce::jmap(static_cast<double>(amt), slowDivision, fastDivision);
                if (allowPitchSpeedMacro)
                {
                    const float swing = ((rampPingPong * 2.0f) - 1.0f) * heldDrive;
                    speedMult = juce::jlimit(0.35f, 4.0f, speedMult * (1.0f + (0.65f * swing)));
                }

                // Alternate LP/HP flavor each half cycle while maintaining a continuous ramp.
                if (rampUp >= rampDown)
                {
                    cutoffNorm = juce::jlimit(0.0f, 1.0f, amt);
                    targetMorph = 1.0f; // High-pass
                }
                else
                {
                    cutoffNorm = juce::jlimit(0.0f, 1.0f, 1.0f - amt);
                    targetMorph = 0.0f; // Low-pass
                }
                targetResonance = juce::jlimit(0.2f, 8.0f, targetResonance + (0.8f * amt));
                break;
            }
        }
    }

    if (threeButton)
    {
        // Musical 3-button contours: exponential risers/fallers and curved macro motion.
        const float phaseNorm = juce::jlimit(0.0f, 1.0f, static_cast<float>(phase));
        const bool fastContour = (threeButtonContour <= 1);
        const float expPowerFast = fastContour
            ? (0.62f + (0.34f * heldRamp))
            : (1.12f + (0.48f * heldRamp));
        const float expPowerSlow = fastContour
            ? (0.78f + (0.30f * heldRamp))
            : (1.04f + (0.44f * heldRamp));
        const float expRise = std::pow(phaseNorm, expPowerFast);
        const float expFall = std::pow(1.0f - phaseNorm, expPowerFast);
        const float arc = (phaseNorm < 0.5f)
            ? std::pow(phaseNorm * 2.0f, expPowerSlow)
            : std::pow((1.0f - phaseNorm) * 2.0f, expPowerSlow);
        const float contourDrive = juce::jlimit(0.0f, 1.0f, 0.38f + (0.62f * heldRamp));
        const double longPatternSlow = fastContour
            ? (lengthBars >= 2 ? 1.58 : 1.26)
            : (lengthBars >= 2 ? 2.04 : 1.52);
        const double longPatternFast = fastContour
            ? (lengthBars >= 2 ? 0.19 : 0.28)
            : (lengthBars >= 2 ? 0.40 : 0.50);

        switch (threeButtonContour)
        {
            case 0: // Exponential riser
            {
                speedMult = juce::jlimit(0.25f, 4.0f, juce::jmap(expRise, 1.00f, 4.00f));
                pitchPattern = juce::jlimit(-14.0f, 14.0f, juce::jmap(expRise, -1.0f, 14.0f));
                cutoffNorm = juce::jlimit(0.0f, 1.0f, juce::jmap(expRise, 0.12f, 0.70f));
                targetMorph = 1.0f;
                targetResonance = juce::jlimit(0.2f, 2.4f, 0.72f + (0.72f * expRise));
                panDepthShape = juce::jlimit(0.0f, 1.0f, juce::jmap(expRise, 0.02f, 1.0f));
                dynamicStutterDivisionBeats = juce::jmap(static_cast<double>(expRise),
                    juce::jmin(2.0, dynamicStutterDivisionBeats * longPatternSlow),
                    juce::jmax(0.125, dynamicStutterDivisionBeats * longPatternFast));
                break;
            }
            case 1: // Exponential faller
            {
                speedMult = juce::jlimit(0.25f, 4.0f, juce::jmap(expFall, 0.55f, 3.85f));
                pitchPattern = juce::jlimit(-14.0f, 14.0f, juce::jmap(expFall, -13.0f, 10.0f));
                cutoffNorm = juce::jlimit(0.0f, 1.0f, juce::jmap(expFall, 0.18f, 0.92f));
                targetMorph = 0.0f;
                targetResonance = juce::jlimit(0.2f, 2.4f, 0.68f + (0.64f * expFall));
                panDepthShape = juce::jlimit(0.0f, 1.0f, juce::jmap(expFall, 0.05f, 1.0f));
                dynamicStutterDivisionBeats = juce::jmap(static_cast<double>(expFall),
                    juce::jmax(0.125, dynamicStutterDivisionBeats * longPatternFast),
                    juce::jmin(2.0, dynamicStutterDivisionBeats * longPatternSlow));
                break;
            }
            case 2: // Rise then fall arc
            {
                speedMult = juce::jlimit(0.25f, 4.0f, juce::jmap(arc, 0.70f, 3.95f));
                pitchPattern = juce::jlimit(-14.0f, 14.0f, juce::jmap(arc, -5.0f, 13.0f));
                cutoffNorm = juce::jlimit(0.0f, 1.0f, juce::jmap(arc, 0.16f, 0.76f));
                targetMorph = (phaseNorm < 0.5f) ? 1.0f : 0.0f;
                targetResonance = juce::jlimit(0.2f, 2.4f, 0.72f + (0.58f * arc));
                panDepthShape = juce::jlimit(0.0f, 1.0f, juce::jmap(arc, 0.05f, 1.0f));
                dynamicStutterDivisionBeats = juce::jmap(static_cast<double>(arc),
                    juce::jmin(2.0, dynamicStutterDivisionBeats * (longPatternSlow - 0.20)),
                    juce::jmax(0.125, dynamicStutterDivisionBeats * (longPatternFast + 0.05)));
                break;
            }
            case 3:
            default: // Fall then rise arc
            {
                const float invArc = 1.0f - arc;
                speedMult = juce::jlimit(0.25f, 4.0f, juce::jmap(invArc, 0.62f, 3.70f));
                pitchPattern = juce::jlimit(-14.0f, 14.0f, juce::jmap(invArc, -11.0f, 10.0f));
                cutoffNorm = juce::jlimit(0.0f, 1.0f, juce::jmap(invArc, 0.20f, 0.88f));
                targetMorph = (phaseNorm < 0.5f) ? 0.0f : 1.0f;
                targetResonance = juce::jlimit(0.2f, 2.4f, 0.66f + (0.58f * invArc));
                panDepthShape = juce::jlimit(0.0f, 1.0f, juce::jmap(invArc, 0.05f, 1.0f));
                dynamicStutterDivisionBeats = juce::jmap(static_cast<double>(invArc),
                    juce::jmin(2.0, dynamicStutterDivisionBeats * (longPatternSlow - 0.10)),
                    juce::jmax(0.125, dynamicStutterDivisionBeats * (longPatternFast + 0.08)));
                break;
            }
        }

        // Make contour ramps react faster as the hold deepens.
        speedMult = juce::jlimit(0.25f, 4.0f, speedMult * (1.0f + (0.35f * contourDrive)));
    }

    const float gesturePhase = static_cast<float>(phase);
    const float gestureSpeedLane = gestureCoordinator->sampleComboLane(
        GestureComboKind::Stutter, stutterComboFlatIndex, static_cast<int>(StutterGestureLane::Speed), gesturePhase);
    const float gesturePitchLane = gestureCoordinator->sampleComboLane(
        GestureComboKind::Stutter, stutterComboFlatIndex, static_cast<int>(StutterGestureLane::Pitch), gesturePhase);
    const float gesturePanLane = gestureCoordinator->sampleComboLane(
        GestureComboKind::Stutter, stutterComboFlatIndex, static_cast<int>(StutterGestureLane::Pan), gesturePhase);
    const float gestureCutoffLane = gestureCoordinator->sampleComboLane(
        GestureComboKind::Stutter, stutterComboFlatIndex, static_cast<int>(StutterGestureLane::Cutoff), gesturePhase);
    const float gestureResonanceLane = gestureCoordinator->sampleComboLane(
        GestureComboKind::Stutter, stutterComboFlatIndex, static_cast<int>(StutterGestureLane::Resonance), gesturePhase);
    const float gestureMorphLane = gestureCoordinator->sampleComboLane(
        GestureComboKind::Stutter, stutterComboFlatIndex, static_cast<int>(StutterGestureLane::Morph), gesturePhase);
    const float gestureDivisionLane = gestureCoordinator->sampleComboLane(
        GestureComboKind::Stutter, stutterComboFlatIndex, static_cast<int>(StutterGestureLane::Division), gesturePhase);
    const float gestureSliceLane = gestureCoordinator->sampleComboLane(
        GestureComboKind::Stutter, stutterComboFlatIndex, static_cast<int>(StutterGestureLane::Slice), gesturePhase);
    const float gesturePanOffset = gesturePanLane * 0.52f;

    speedMult *= std::pow(2.0f, gestureSpeedLane * 0.85f);
    if (twoButton)
    {
        twoButtonSemitoneStep += gesturePitchLane * 12.0f;
        twoButtonSemitoneSpeedRatio *= std::pow(2.0f, gestureSpeedLane * 0.55f);
    }
    else
    {
        pitchPattern += gesturePitchLane * 9.0f;
    }
    panPattern = juce::jlimit(-1.0f, 1.0f, panPattern + (gesturePanLane * 0.85f));
    panDepthShape = juce::jlimit(0.0f, 1.0f, panDepthShape + (std::abs(gesturePanLane) * 0.42f));
    cutoffNorm = juce::jlimit(0.0f, 1.0f, cutoffNorm + (gestureCutoffLane * 0.24f));
    targetMorph = juce::jlimit(0.0f, 1.0f, targetMorph + (gestureMorphLane * 0.18f));
    targetResonance = juce::jlimit(0.2f, 8.0f, targetResonance + (gestureResonanceLane * 0.85f));
    dynamicStutterDivisionBeats *= std::pow(2.0, -gestureDivisionLane * 1.35f);

    auto sanitizeFiniteFloat = [](float value, float fallback) noexcept
    {
        return std::isfinite(value) ? value : fallback;
    };
    auto sanitizeFiniteDouble = [](double value, double fallback) noexcept
    {
        return std::isfinite(value) ? value : fallback;
    };

    shapeIntensity = juce::jlimit(0.15f, 1.0f, sanitizeFiniteFloat(shapeIntensity, 1.0f));
    speedMult = juce::jlimit(0.25f, 4.0f, sanitizeFiniteFloat(speedMult, 1.0f));
    panPattern = juce::jlimit(-1.0f, 1.0f, sanitizeFiniteFloat(panPattern, 0.0f));
    pitchPattern = juce::jlimit(-18.0f, 18.0f, sanitizeFiniteFloat(pitchPattern, 0.0f));
    cutoffNorm = juce::jlimit(0.0f, 1.0f, sanitizeFiniteFloat(cutoffNorm, 0.85f));
    targetResonance = juce::jlimit(0.2f, 8.0f, sanitizeFiniteFloat(targetResonance, 1.0f));
    targetMorph = juce::jlimit(0.0f, 1.0f, sanitizeFiniteFloat(targetMorph, 0.25f));
    panDepthShape = juce::jlimit(0.0f, 1.0f, sanitizeFiniteFloat(panDepthShape, 1.0f));
    twoButtonSemitoneStep = juce::jlimit(-36.0f, 36.0f, sanitizeFiniteFloat(twoButtonSemitoneStep, 0.0f));
    twoButtonSemitoneSpeedRatio = juce::jlimit(0.03125f, 8.0f, sanitizeFiniteFloat(twoButtonSemitoneSpeedRatio, 1.0f));
    dynamicStutterDivisionBeats = juce::jlimit(0.125,
                                               4.0,
                                               sanitizeFiniteDouble(dynamicStutterDivisionBeats,
                                                                    stutterDivisionBeatsFromBitForMacro(highestBit, multiButton)));

    // Musical safety guard:
    // 2-button combos should stay expressive but avoid ultra-harsh ringing/noise at high stutter rates.
    if (!allowPitchSpeedMacro)
    {
        const double minDivision = 0.125;
        dynamicStutterDivisionBeats = juce::jlimit(minDivision, 4.0, dynamicStutterDivisionBeats);
        targetResonance = juce::jlimit(0.2f, 1.4f, targetResonance);
    }

    // High-density col15 combos can become brittle/noisy when all macro dimensions
    // align at the same time; keep them in a musical envelope.
    if (allowPitchSpeedMacro && hasTopStutterBit)
    {
        dynamicStutterDivisionBeats = juce::jlimit(0.125, 4.0, dynamicStutterDivisionBeats);
        speedMult = juce::jlimit(0.60f, 2.0f, speedMult);
        pitchPattern = juce::jlimit(-8.0f, 8.0f, pitchPattern);
        targetResonance = juce::jlimit(0.2f, 2.4f, targetResonance);
    }

    // Explicitly tame known harsh combinations.
    if (combo10And13)
    {
        dynamicStutterDivisionBeats = juce::jlimit(0.125, 4.0, dynamicStutterDivisionBeats);
        targetMorph = 0.0f;
        targetResonance = juce::jlimit(0.2f, 1.2f, targetResonance);
    }

    if (combo11And13)
    {
        dynamicStutterDivisionBeats = juce::jlimit(0.125, 4.0, dynamicStutterDivisionBeats);
        targetMorph = 0.0f;
        targetResonance = juce::jlimit(0.2f, 1.1f, targetResonance);
    }

    if (combo12And13And15)
    {
        dynamicStutterDivisionBeats = juce::jlimit(0.125, 4.0, dynamicStutterDivisionBeats);
        speedMult = juce::jlimit(0.70f, 1.60f, speedMult);
        pitchPattern = juce::jlimit(-6.0f, 6.0f, pitchPattern);
        targetResonance = juce::jlimit(0.2f, 1.8f, targetResonance);
    }

    if (multiButton)
    {
        static constexpr std::array<double, 4> kTwoButtonGrid { 1.0, 0.5, 0.25, 0.125 };
        static constexpr std::array<double, 4> kThreeButtonGrid { 1.0, 0.5, 0.25, 0.125 };
        static constexpr std::array<double, 3> kDenseButtonGrid { 0.5, 0.25, 0.125 };

        if (bitCount == 2)
            dynamicStutterDivisionBeats = snapDivisionToGrid(dynamicStutterDivisionBeats, kTwoButtonGrid);
        else if (bitCount == 3)
            dynamicStutterDivisionBeats = snapDivisionToGrid(dynamicStutterDivisionBeats, kThreeButtonGrid);
        else
            dynamicStutterDivisionBeats = snapDivisionToGrid(dynamicStutterDivisionBeats, kDenseButtonGrid);
    }

    const bool veryFastDivision = dynamicStutterDivisionBeats <= 0.1250001;
    const bool ultraFastDivision = dynamicStutterDivisionBeats <= 0.0835001;
    float retriggerFadeMs = 0.7f;
    if (bitCount == 2)
        retriggerFadeMs = veryFastDivision ? 1.30f : 1.00f;
    else if (bitCount >= 3)
        retriggerFadeMs = ultraFastDivision ? 2.00f : (veryFastDivision ? 1.70f : 1.35f);
    audioEngine->setMomentaryStutterRetriggerFadeMs(retriggerFadeMs);

    if (multiButton && veryFastDivision)
    {
        const float speedFloor = ultraFastDivision ? 0.72f : 0.60f;
        const float speedCeil = allowPitchSpeedMacro
            ? (ultraFastDivision ? 1.95f : (threeButton ? 2.60f : 2.20f))
            : (twoButton ? (ultraFastDivision ? 2.15f : 2.85f) : 1.25f);
        speedMult = juce::jlimit(speedFloor, speedCeil, speedMult);
        pitchPattern = juce::jlimit(-6.0f, 6.0f, pitchPattern);
        targetResonance = juce::jlimit(0.2f, ultraFastDivision ? 0.85f : 1.05f, targetResonance);
        if (targetMorph > 0.70f)
            targetMorph = ultraFastDivision ? 0.58f : 0.70f;
    }

    if (multiButton && targetMorph > 0.82f && cutoffNorm > 0.78f)
        targetResonance = juce::jmin(targetResonance, 0.9f);

    if (multiButton)
    {
        // Keep cutoff+morph inside audible zones to avoid click-only/no-audio states.
        if (targetMorph >= 0.70f)
            cutoffNorm = juce::jlimit(0.04f, 0.72f, cutoffNorm);
        else if (targetMorph <= 0.30f)
            cutoffNorm = juce::jlimit(0.16f, 0.98f, cutoffNorm);
        else
            cutoffNorm = juce::jlimit(0.08f, 0.94f, cutoffNorm);

        if ((targetMorph >= 0.72f && cutoffNorm >= 0.62f)
            || (targetMorph <= 0.16f && cutoffNorm <= 0.22f))
            targetResonance = juce::jmin(targetResonance, 0.82f);
    }

    if (applySpeedMacro && !twoButton)
    {
        // Stutter speed is hard-stepped by PPQ phase step index (no smooth glides).
        const float cycleStepNorm = (totalSteps > 1)
            ? juce::jlimit(0.0f, 1.0f, static_cast<float>(stepIndex) / static_cast<float>(totalSteps - 1))
            : 0.0f;
        const int rampShape = threeButton ? threeButtonContour : (variant & 0x3);
        float rampNorm = cycleStepNorm;
        switch (rampShape)
        {
            case 0: // up
                rampNorm = cycleStepNorm;
                break;
            case 1: // down
                rampNorm = 1.0f - cycleStepNorm;
                break;
            case 2: // up then down
                rampNorm = (cycleStepNorm < 0.5f)
                    ? (cycleStepNorm * 2.0f)
                    : ((1.0f - cycleStepNorm) * 2.0f);
                break;
            case 3: // down then up
            default:
                rampNorm = (cycleStepNorm < 0.5f)
                    ? (1.0f - (cycleStepNorm * 2.0f))
                    : ((cycleStepNorm - 0.5f) * 2.0f);
                break;
        }
        const float expShape = threeButton
            ? (0.90f + (0.95f * heldRamp))
            : (1.20f + (1.10f * heldRamp) + (twoButton ? 0.20f : 0.0f));
        const float shapedRamp = std::pow(juce::jlimit(0.0f, 1.0f, rampNorm), expShape);
        const float minHardSpeedMult = threeButton ? 0.45f : 0.55f;
        const float maxHardSpeedMult = threeButton ? 3.9f : 3.1f;
        const float hardStepSpeedMult = juce::jmap(shapedRamp, minHardSpeedMult, maxHardSpeedMult);
        const float hardStepBlend = threeButton ? 0.96f : (twoButton ? 0.88f : 0.84f);
        speedMult = juce::jmap(hardStepBlend, speedMult, hardStepSpeedMult);
    }

    const float intensity = juce::jlimit(0.20f, 1.0f, comboIntensity * shapeIntensity);
    const float speedIntensityScale = juce::jlimit(0.35f, 1.0f, 0.42f + (0.58f * intensity));
    const float shapedSpeedMult = twoButton
        ? juce::jlimit(0.03125f, 8.0f, twoButtonSemitoneSpeedRatio)
        : (1.0f + ((speedMult - 1.0f) * speedIntensityScale));
    const float pitchOffsetBasePattern = juce::jlimit(-12.0f, 12.0f, pitchPattern * (0.55f + (0.30f * intensity)));
    // Keep pitch secondary: speed carries the primary riser/faller motion.
    const float speedToPitchDepth = allowPitchMacro ? (threeButton ? 3.0f : 2.0f) : 0.0f;
    const float pitchOffsetFromSpeedShape = juce::jlimit(-12.0f, 12.0f, (shapedSpeedMult - 1.0f) * speedToPitchDepth);
    const float pitchOffsetBase = juce::jlimit(
        -12.0f, 12.0f, pitchOffsetBasePattern + ((allowPitchMacro && !twoButton) ? pitchOffsetFromSpeedShape : 0.0f));

    // Pan is always hard-stepped and locked to the active stutter subdivision.
    const double panDivisionBeats = juce::jmax(0.03125, dynamicStutterDivisionBeats);
    const double panStepPos = (ppqNow - momentaryStutterMacroStartPpq) / panDivisionBeats;
    const int panStepIndex = static_cast<int>(std::floor(std::isfinite(panStepPos) ? panStepPos : 0.0));
    const int panMode = ((seed / 23) + bitCount + highestBit + lowestBit) & 0x3;
    static constexpr std::array<float, 8> kPanSeqA { -1.0f, 1.0f, -1.0f, 1.0f, 1.0f, -1.0f, 1.0f, -1.0f };
    static constexpr std::array<float, 8> kPanSeqB { -1.0f, 1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, -1.0f };
    float panHardStep = -1.0f;
    switch (panMode)
    {
        case 0:
            panHardStep = (panStepIndex & 1) ? 1.0f : -1.0f;
            break;
        case 1:
            panHardStep = ((panStepIndex >> 1) & 1) ? 1.0f : -1.0f;
            break;
        case 2:
            panHardStep = kPanSeqA[static_cast<size_t>(juce::jmax(0, panStepIndex) & 7)];
            break;
        case 3:
        default:
            panHardStep = kPanSeqB[static_cast<size_t>(juce::jmax(0, panStepIndex) & 7)];
            break;
    }
    if (twoButton)
        panHardStep = (panStepIndex & 1) ? 1.0f : -1.0f;
    if (panPattern < 0.0f)
        panHardStep = -panHardStep;
    const float panDriveBase = juce::jlimit(0.72f, 1.0f,
        0.72f + (0.28f * intensity) + (threeButton ? 0.10f : 0.0f) + (veryFastDivision ? 0.08f : 0.0f));
    float panDepth = 1.0f;
    if (threeButton)
        panDepth = juce::jlimit(0.18f, 1.0f, panDepthShape);
    else if (singleButton)
        panDepth = juce::jlimit(0.05f, 0.28f, panDepthShape);
    else if (twoButton)
        panDepth = juce::jlimit(0.0f, 1.0f, panDepthShape);
    else
        panDepth = juce::jlimit(0.28f, 1.0f, panDepthShape);
    const float panDrive = twoButton
        ? juce::jlimit(0.0f, 1.0f, panDriveBase * panDepth)
        : juce::jlimit(0.18f, 1.0f, panDriveBase * panDepth);
    const float panOffsetBase = juce::jlimit(-1.0f, 1.0f, panHardStep * panDrive);

    cutoffNorm = juce::jlimit(0.0f, 1.0f, cutoffNorm);
    const float resonanceScale = threeButton
        ? juce::jlimit(0.75f, 1.15f, comboIntensity + 0.18f)
        : comboIntensity;
    targetResonance = juce::jlimit(0.2f, threeButton ? 2.4f : 8.0f, targetResonance * resonanceScale);
    targetMorph = juce::jlimit(0.0f, 1.0f, targetMorph);

    auto filterAlgorithm = filterAlgorithmFromIndex((variant + bitCount + highestBit + lowestBit) % 6);
    if (combo10And13 || combo11And13 || combo12And13And15
        || (!allowPitchSpeedMacro && highestBit >= 5 && targetMorph > 0.74f)
        || (multiButton && veryFastDivision))
        filterAlgorithm = EnhancedAudioStrip::FilterAlgorithm::Tpt12;
    const float targetCutoff = cutoffFromNormalized(cutoffNorm);
    const bool allowSliceLengthGesture = (twoButton || threeButton);
    const bool comboUsesSliceGesture = allowSliceLengthGesture
        && ((twoButton && (((seed + highestBit + lowestBit) & 0x1) == 0 || highestBit >= 4))
            || (threeButton && ((((seed >> 1) + highestBit) & 0x1) == 0 || highestBit >= 3)));
    const float phraseProgress = twoButton
        ? juce::jlimit(0.0f, 1.0f, twoButtonDirectionUp ? static_cast<float>(phase)
                                                         : (1.0f - static_cast<float>(phase)))
        : juce::jlimit(0.0f, 1.0f, static_cast<float>(phase));
    const float stutterDensityNorm = juce::jlimit(0.0f, 1.0f, static_cast<float>(
        (0.5 - juce::jlimit(0.125, 0.5, dynamicStutterDivisionBeats))
        / (0.5 - 0.125)));
    const float sliceGestureStrength = comboUsesSliceGesture
        ? juce::jlimit(0.0f, 1.0f,
            (0.50f * phraseProgress) + (0.30f * stutterDensityNorm) + (0.20f * heldRamp))
        : 0.0f;
    const float minSliceLengthForGesture = twoButton ? 0.06f : 0.03f;
    audioEngine->setMomentaryStutterDivision(juce::jlimit(0.125, 4.0, dynamicStutterDivisionBeats));
    const double speedStepDivisionBeats = juce::jmax(0.125, dynamicStutterDivisionBeats);
    const double speedStepPos = (ppqNow - momentaryStutterMacroStartPpq) / speedStepDivisionBeats;
    const int speedStepAbs = juce::jmax(0, static_cast<int>(std::floor(std::isfinite(speedStepPos) ? speedStepPos : 0.0)));
    const bool stutterStartStep = (speedStepAbs == 0);
    const bool firstSpeedStep = applySpeedMacro && (speedStepAbs == 0);
    const auto stepFilterTypeFromMorph = [](float morph)
    {
        if (morph < 0.34f)
            return FilterType::LowPass;
        if (morph > 0.66f)
            return FilterType::HighPass;
        return FilterType::BandPass;
    };

    for (int i = 0; i < MaxStrips; ++i)
    {
        const auto idx = static_cast<size_t>(i);
        const auto& saved = momentaryStutterSavedState[idx];
        if (!saved.valid || !momentaryStutterStripArmed[idx])
            continue;

        auto* strip = audioEngine->getStrip(i);
        const bool stepMode = (strip && strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Step);
        auto* stepSampler = (stepMode && strip) ? strip->getStepSampler() : nullptr;
        const bool hasPlayableContent = strip
            && (strip->hasAudio() || (stepSampler && stepSampler->getHasAudio()));
        if (!strip || !hasPlayableContent || !strip->isPlaying())
            continue;

        const float stripOffset = static_cast<float>(i - (MaxStrips / 2));
        const float stripPanScale = juce::jlimit(0.45f, threeButton ? 1.35f : 1.15f,
            0.65f + (0.08f * static_cast<float>(bitCount)) + (0.05f * static_cast<float>(i)));
        const float stripPitchSpread = (allowPitchSpeedMacro && bitCount > 2) ? (stripOffset * 0.35f) : 0.0f;
        const float stripSpeedSpread = (applySpeedMacro && bitCount > 3) ? (stripOffset * 0.025f) : 0.0f;
        const float stripMorphOffset = static_cast<float>(0.08 * std::sin(
            juce::MathConstants<double>::twoPi * wrapUnitPhase(phase + (0.13 * static_cast<double>(i)))));

        const float savedSpeed = juce::jlimit(0.125f, 8.0f, saved.playbackSpeed);
        const float speedBaseline = savedSpeed;
        const float stutterSpeedFloor = applySpeedMacro
            ? (ultraFastDivision ? 0.72f : (veryFastDivision ? 0.56f : 0.30f))
            : speedBaseline;
        const float stutterSpeedCeil = applySpeedMacro
            ? (ultraFastDivision ? (threeButton ? 2.10f : 1.95f)
                                 : (veryFastDivision ? (threeButton ? 2.80f : 2.35f)
                                                     : (threeButton ? 4.0f : 3.2f)))
            : speedBaseline;
        const float modulatedTargetSpeed = twoButton
            // Two-finger speed always starts at current strip speed and moves
            // up/down in semitone steps relative to that baseline.
            ? juce::jlimit(0.03125f, 8.0f, speedBaseline * shapedSpeedMult)
            : juce::jlimit(stutterSpeedFloor, stutterSpeedCeil,
                (speedBaseline * shapedSpeedMult) + stripSpeedSpread);
        const bool holdBaselineSpeed = twoButton ? (twoButtonStepAbs == 0) : firstSpeedStep;
        const float targetSpeed = holdBaselineSpeed ? speedBaseline : modulatedTargetSpeed;
        if (!stepMode)
        {
            if (applySpeedMacro)
                strip->setPlaybackSpeed(targetSpeed);
            else
                strip->setPlaybackSpeed(speedBaseline);
        }
        else
        {
            // Step mode uses step-sampler pitch speed; keep strip traversal speed stable.
            strip->setPlaybackSpeed(saved.playbackSpeed);
        }

        const float targetPan = juce::jlimit(-1.0f, 1.0f,
            saved.pan + (panOffsetBase * stripPanScale) + gesturePanOffset);
        setStripPanControlValue(i, targetPan, StripControlWriteMode::CacheOnly);

        float targetPitch = saved.stepMode ? saved.pitchSemitones : saved.pitchShift;
        if (twoButton && applySpeedMacro)
        {
            if (stepMode)
            {
                targetPitch = juce::jlimit(-24.0f, 24.0f, saved.pitchSemitones + twoButtonSemitoneStep);
            }
            else
            {
                // Guarantee full contour even when speed reaches hard limits:
                // carry residual semitone motion into pitch shift.
                const float ratioBase = juce::jmax(0.03125f, speedBaseline);
                const float ratioActual = juce::jmax(0.03125f, targetSpeed / ratioBase);
                const float actualSemitoneFromSpeed = 12.0f * std::log2(ratioActual);
                const float residualSemitone = twoButtonSemitoneStep - actualSemitoneFromSpeed;
                targetPitch = juce::jlimit(-24.0f, 24.0f, saved.pitchShift + residualSemitone);
            }
        }
        else if (allowPitchMacro)
        {
            const float pitchBase = stepMode ? saved.pitchSemitones : saved.pitchShift;
            targetPitch = juce::jlimit(-12.0f, 12.0f, pitchBase + pitchOffsetBase + stripPitchSpread);
        }

        applyPitchControlToStrip(i, *strip, targetPitch);

        if (strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Loop)
        {
            float targetSliceLength = saved.loopSliceLength;
            if (comboUsesSliceGesture)
            {
                const float shortened = saved.loopSliceLength
                    - ((saved.loopSliceLength - minSliceLengthForGesture) * sliceGestureStrength);
                targetSliceLength = juce::jlimit(minSliceLengthForGesture, 1.0f, shortened);
            }
            if (std::abs(gestureSliceLane) > 1.0e-4f)
            {
                const float sliceRatio = std::pow(2.0f, -gestureSliceLane * 0.95f);
                targetSliceLength = juce::jlimit(minSliceLengthForGesture, 1.0f, targetSliceLength * sliceRatio);
            }
            strip->setLoopSliceLength(targetSliceLength);
        }

        const bool useMacroFilter = !(singleButton || (twoButton && !twoButtonUseFilter));
        if (!useMacroFilter)
        {
            // Clean stutter variants: no filter color.
            setStripFilterAlgorithmControlValue(i, saved.filterAlgorithm, StripControlWriteMode::CacheOnly);
            setStripFilterFrequencyControlValue(i, saved.filterFrequency, StripControlWriteMode::CacheOnly);
            setStripFilterResonanceControlValue(i, saved.filterResonance, StripControlWriteMode::CacheOnly);
            setStripFilterMorphControlValue(i, saved.filterMorph, StripControlWriteMode::CacheOnly);
            setStripFilterEnabledControlValue(i, saved.filterEnabled, StripControlWriteMode::CacheOnly);
            if (stepSampler != nullptr)
                stepSampler->setFilterType(saved.stepFilterType);
        }
        else
        {
            setStripFilterEnabledControlValue(i, true, StripControlWriteMode::CacheOnly);
            setStripFilterAlgorithmControlValue(i, filterAlgorithm, StripControlWriteMode::CacheOnly);
            if (stutterStartStep)
            {
                // Start every stutter with filter fully open and minimum resonance.
                setStripFilterMorphControlValue(i, 0.0f, StripControlWriteMode::CacheOnly);
                setStripFilterFrequencyControlValue(i, 20000.0f, StripControlWriteMode::CacheOnly);
                setStripFilterResonanceControlValue(i, 0.1f, StripControlWriteMode::CacheOnly);
                if (stepSampler != nullptr)
                    stepSampler->setFilterType(FilterType::LowPass);
            }
            else
            {
                const float morphWithOffset = juce::jlimit(0.0f, 1.0f, targetMorph + stripMorphOffset);
                setStripFilterFrequencyControlValue(i, targetCutoff, StripControlWriteMode::CacheOnly);
                setStripFilterResonanceControlValue(i, targetResonance, StripControlWriteMode::CacheOnly);
                setStripFilterMorphControlValue(i, morphWithOffset, StripControlWriteMode::CacheOnly);
                if (stepSampler != nullptr)
                    stepSampler->setFilterType(stepFilterTypeFromMorph(morphWithOffset));
            }
        }
    }

    momentaryStutterLastComboMask = comboMask;
}

void MlrVSTAudioProcessor::restoreMomentaryStutterMacroBaseline()
{
    if (!audioEngine || !momentaryStutterMacroBaselineCaptured)
        return;

    for (int i = 0; i < MaxStrips; ++i)
    {
        const auto idx = static_cast<size_t>(i);
        auto& saved = momentaryStutterSavedState[idx];
        if (!saved.valid)
            continue;

        if (auto* strip = audioEngine->getStrip(i))
        {
            setStripPanControlValue(i, saved.pan, StripControlWriteMode::CacheOnly);
            strip->setPlaybackSpeedImmediate(saved.playbackSpeed);
            strip->setLoopSliceLength(saved.loopSliceLength);
            if (saved.stepMode)
            {
                applyPitchControlToStrip(i, *strip, saved.pitchSemitones);
                if (auto* stepSampler = strip->getStepSampler())
                    stepSampler->setFilterType(saved.stepFilterType);
            }
            else
            {
                applyPitchControlToStrip(i, *strip, saved.pitchShift);
            }
            setStripFilterAlgorithmControlValue(i, saved.filterAlgorithm, StripControlWriteMode::CacheOnly);
            setStripFilterFrequencyControlValue(i, saved.filterFrequency, StripControlWriteMode::CacheOnly);
            setStripFilterResonanceControlValue(i, saved.filterResonance, StripControlWriteMode::CacheOnly);
            setStripFilterMorphControlValue(i, saved.filterMorph, StripControlWriteMode::CacheOnly);
            setStripFilterEnabledControlValue(i, saved.filterEnabled, StripControlWriteMode::CacheOnly);
            if (saved.stepMode)
            {
                if (auto* stepSampler = strip->getStepSampler())
                    stepSampler->setFilterType(saved.stepFilterType);
            }
        }

        saved.valid = false;
    }

    momentaryStutterMacroBaselineCaptured = false;
    momentaryStutterMacroCapturePending = false;
    momentaryStutterLastComboMask = 0;
    momentaryStutterTwoButtonStepBaseValid = false;
    momentaryStutterTwoButtonStepBase = 0;
}


void MlrVSTAudioProcessor::triggerStrip(int stripIndex, int column)
{
    if (!audioEngine) return;

    // Apply trigger-fade setting immediately for Monome row presses, even if
    // the host isn't currently invoking processBlock.
    if (triggerFadeInParam)
        audioEngine->setTriggerFadeInMs(*triggerFadeInParam);

    auto* strip = audioEngine->getStrip(stripIndex);
    if (!strip) return;
    const bool isSampleMode = (strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Sample);

    // If bar length was changed while playing, apply it on the next row trigger.
    const auto stripIdx = static_cast<size_t>(stripIndex);
    if (pendingBarLengthApply[stripIdx] && strip->hasAudio())
    {
        const int bars = juce::jlimit(1, 8, strip->getRecordingBars());
        strip->setBeatsPerLoop(static_cast<float>(bars * 4));
        pendingBarLengthApply[stripIdx] = false;
    }

    // CHECK: If inner loop is active, clear it and return to full loop
    if (!isSampleMode && (strip->getLoopStart() != 0 || strip->getLoopEnd() != MaxColumns))
    {
        const int targetColumn = juce::jlimit(0, MaxColumns - 1, column);
        bool updatedPendingClear = false;
        {
            const juce::ScopedLock lock(pendingLoopChangeLock);
            auto& pending = pendingLoopChanges[static_cast<size_t>(stripIndex)];
            if (pending.active && pending.clear)
            {
                // Keep a single quantized clear request active, but allow the
                // user's latest pad press to define the post-exit position.
                pending.markerColumn = targetColumn;
                pending.postClearTriggerArmed = false;
                updatedPendingClear = true;
            }
        }

        if (updatedPendingClear)
        {
            DBG("Inner loop clear pending on strip " << stripIndex
                << " -> updated marker column " << targetColumn);
            return;
        }

        // Inner loop is active: this press both clears the loop and defines
        // the re-entry column, applied together on the quantized boundary.
        queueLoopChange(stripIndex, true, 0, MaxColumns, false, targetColumn);
        DBG("Inner loop clear+retrigger requested on strip " << stripIndex
            << " -> column " << targetColumn << " (quantized)");
        return;
    }

    const double timelineBeat = audioEngine->getTimelineBeat();

    juce::AudioPlayHead::PositionInfo posInfo;
    getCurrentHostPositionInfo(posInfo);

    // Get quantization settings
    auto* quantizeParamLocal = parameters.getRawParameterValue("quantize");
    int quantizeChoice = quantizeParamLocal ? static_cast<int>(*quantizeParamLocal) : 5;

    // Map choice to actual divisions: 0=1, 1=2, 2=3, 3=4, 4=6, 5=8, 6=12, 7=16, 8=24, 9=32
    const int divisionMap[] = {1, 2, 3, 4, 6, 8, 12, 16, 24, 32};
    int quantizeValue = (quantizeChoice >= 0 && quantizeChoice < 10) ? divisionMap[quantizeChoice] : 8;

    // Calculate what the quantBeats will be
    double quantBeats = 4.0 / quantizeValue;

    // Use host PPQ when available. This must match quantized scheduler timing.
    const double currentPPQ = posInfo.getPpqPosition().hasValue() ? *posInfo.getPpqPosition() : timelineBeat;
    // Calculate next grid position
    double nextGridPPQ = std::ceil(currentPPQ / quantBeats) * quantBeats;
    nextGridPPQ = std::round(nextGridPPQ / quantBeats) * quantBeats;

    // Check if gate is closed (trigger pending)
    bool gateClosed = audioEngine->hasPendingTrigger(stripIndex);

    // Set quantization on the audio engine
    audioEngine->setQuantization(quantizeValue);

    // Apply quantization if enabled
    bool useQuantize = quantizeValue > 1;
    const bool isHoldScratchTransition = (strip->getScratchAmount() > 0.0f
        && ((strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Grain)
            ? strip->isButtonHeld()
            : (strip->getHeldButtonCount() > 1)));
    if (isHoldScratchTransition)
        useQuantize = false;

    // Strict gate behavior: ignore extra presses while quantized trigger is pending.
    if (useQuantize && gateClosed)
    {
        updateMonomeLEDs();
        return;
    }

    if (!isSampleMode)
        applyPendingLoopPitchRetuneOnTrigger(stripIndex);

    int sampleSliceId = -1;
    int64_t sampleStartSample = -1;
    if (isSampleMode)
    {
        auto* sampleEngine = getSampleModeEngine(stripIndex, false);
        const int visibleSlot = juce::jlimit(0, SliceModel::VisibleSliceCount - 1, column);
        SampleSlice visibleSlice;
        if (sampleEngine == nullptr
            || !sampleEngine->hasVisibleSlice(visibleSlot)
            || !sampleEngine->getVisibleSliceInfo(visibleSlot, visibleSlice))
        {
            updateMonomeLEDs();
            return;
        }

        sampleEngine->setPendingVisibleSlice(visibleSlot);
        sampleSliceId = visibleSlice.id;
        sampleStartSample = visibleSlice.startSample;
    }
    else if (strip->getPlayMode() != EnhancedAudioStrip::PlayMode::Step)
    {
        // Scene/pattern playback should retrigger the same audio boundary that
        // the live grid press hit, even if transient markers or loop state have
        // been recalled since the event was recorded.
        sampleStartSample = strip->getTriggerTargetSampleForColumn(column);
    }

    uint32_t autosaveDelayMs = 0;
    if (useQuantize)
    {
        // Schedule for next quantize point - group choke handled in batch execution
        DBG("=== SCHEDULING QUANTIZED TRIGGER === Strip " << stripIndex
            << " Column " << column
            << " Quantize: " << quantizeValue);
        audioEngine->scheduleQuantizedTrigger(stripIndex,
                                              column,
                                              currentPPQ,
                                              sampleSliceId,
                                              sampleStartSample);
        const double hostTempoForAutosave = posInfo.getBpm().hasValue()
            ? *posInfo.getBpm()
            : audioEngine->getCurrentTempo();
        if (std::isfinite(hostTempoForAutosave) && hostTempoForAutosave > 0.0)
        {
            const double waitBeats = juce::jmax(0.0, nextGridPPQ - currentPPQ);
            const double waitSeconds = (waitBeats * 60.0) / hostTempoForAutosave;
            autosaveDelayMs = static_cast<uint32_t>(juce::jlimit<int64_t>(
                0,
                8000,
                static_cast<int64_t>(std::llround(waitSeconds * 1000.0)) + 24));
        }
    }
    else
    {
        // Immediate trigger - handle group choke here with short fade in engine path.
        audioEngine->enforceGroupExclusivity(stripIndex, false);

        // Trigger immediately with PPQ sync
        int64_t triggerGlobalSample = audioEngine->getGlobalSampleCount();
        if (isSampleMode)
        {
            triggerSampleModeStripAtSample(stripIndex,
                                           column,
                                           sampleSliceId,
                                           sampleStartSample,
                                           triggerGlobalSample,
                                           posInfo,
                                           false);
        }
        else
        {
            strip->triggerAtSample(column, audioEngine->getCurrentTempo(), triggerGlobalSample, posInfo);
        }
    }

    if (isSceneModeEnabled())
        queueActiveSceneAutosave(autosaveDelayMs);

    // Record pattern events at the exact trigger timeline position.
    const double eventBeat = useQuantize ? nextGridPPQ : currentPPQ;
    if (isSceneModeEnabled())
    {
        const bool recordScratchGesture = !isSampleMode
            && shouldRecordSceneScratchGestureTrigger(*strip, column);
        int recordedSliceId = sampleSliceId;
        int64_t recordedSliceStartSample = sampleStartSample;
        if (isSampleMode)
        {
            if (auto* sampleEngine = getSampleModeEngine(stripIndex, false))
            {
                SampleSlice visibleSlice;
                if (sampleEngine->getVisibleSliceInfo(juce::jlimit(0, SliceModel::VisibleSliceCount - 1, column), visibleSlice))
                {
                    recordedSliceId = visibleSlice.id;
                    recordedSliceStartSample = visibleSlice.startSample;
                }
            }
        }

        recordSceneTriggerEvent(stripIndex,
                                column,
                                eventBeat,
                                recordedSliceId,
                                recordedSliceStartSample,
                                true,
                                recordScratchGesture);
        if (scenePerformanceRecorder.isRecording() && useQuantize)
            rememberPendingSceneTriggerRecord(stripIndex, column, eventBeat, recordScratchGesture);
        else
            clearPendingSceneTriggerRecord(stripIndex);
    }
    else
    {
        clearPendingSceneTriggerRecord(stripIndex);
        for (int i = 0; i < 4; ++i)
        {
            auto* pattern = audioEngine->getPattern(i);
            if (pattern && pattern->isRecording() && audioEngine->patternRecorderMatchesStrip(i, stripIndex))
            {
                int recordedSliceId = sampleSliceId;
                int64_t recordedSliceStartSample = sampleStartSample;
                if (isSampleMode)
                {
                    if (auto* sampleEngine = getSampleModeEngine(stripIndex, false))
                    {
                        SampleSlice visibleSlice;
                        if (sampleEngine->getVisibleSliceInfo(juce::jlimit(0, SliceModel::VisibleSliceCount - 1, column), visibleSlice))
                        {
                            recordedSliceId = visibleSlice.id;
                            recordedSliceStartSample = visibleSlice.startSample;
                        }
                    }
                }
                DBG("Recording to pattern " << i << ": strip=" << stripIndex << ", col=" << column << ", beat=" << eventBeat);
                pattern->recordEvent(stripIndex, column, true, eventBeat, recordedSliceId, recordedSliceStartSample);
            }
        }
    }

    updateMonomeLEDs();
}

void MlrVSTAudioProcessor::stopStrip(int stripIndex, bool immediateStop, int sceneReleaseColumnHint)
{
    if (audioEngine == nullptr)
        return;

    captureSceneTriggerRelease(stripIndex, sceneReleaseColumnHint);

    if (auto* strip = audioEngine->getStrip(stripIndex))
    {
        audioEngine->clearPendingQuantizedTriggersForStrip(stripIndex);
        if (strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Sample)
            stopSampleModeStrip(stripIndex, immediateStop);
        else
            strip->stop(immediateStop);
    }

    clearPendingSceneTriggerRecord(stripIndex);

    if (isSceneModeEnabled())
        queueActiveSceneAutosave();
}

void MlrVSTAudioProcessor::setCurrentProgram(int /*index*/)
{
}

const juce::String MlrVSTAudioProcessor::getProgramName(int /*index*/)
{
    return {};
}

void MlrVSTAudioProcessor::changeProgramName(int /*index*/, const juce::String& /*newName*/)
{
}

void MlrVSTAudioProcessor::timerCallback()
{
    const auto nowMs = juce::Time::getMillisecondCounter();
    syncSceneModeFromParameters();
    reclaimRetiredPreparedSceneSwitchPayloads();
    servicePendingScenePreloadRequest();
    processPendingSceneAutosave();
    processPendingSceneApply();
    processPendingSceneRecorderApply();
    applyPendingSceneParameterState();
    applyPendingSceneRawParameterResync();
    const bool shouldSyncSceneMotion = isSceneModeEnabled()
        && audioEngine != nullptr
        && (scenePerformanceRecorder.isRecording()
            || lastSceneMotionSyncTimeMs == 0
            || (nowMs - lastSceneMotionSyncTimeMs) >= kSceneMotionSyncRefreshMs);
    if (shouldSyncSceneMotion)
    {
        syncSceneMotionStateFromEngine(activeSceneSlot);
        lastSceneMotionSyncTimeMs = nowMs;
    }
    processPendingMonomePatternTapActions(juce::Time::getMillisecondCounter());
    processPendingMonomeSceneRecorderTapActions(juce::Time::getMillisecondCounter());
    refreshUtilityTimerCadence();

    applyCompletedPresetSaves();
    applyCompletedLoopStripLoads();
    applyCompletedFlipLegacyLoopRenders();
    applyCompletedLoopPitchAnalyses();
    applyCompletedSoundTouchPitchCaches();
    applyCompletedBungeePitchCaches();
    applyCompletedSignalsmithPitchCaches();
    maintainStripOfflineCaches();

    applyPendingLoopPitchRetunes();
    prewarmFlipLegacyLoopRenders();
    refreshPendingSoundTouchPitchCaches();
    refreshPendingBungeePitchCaches();
    refreshPendingSignalsmithPitchCaches();

    if constexpr (kReportRealtimeSignalsmithLatencyToHost)
    {
        if (audioEngine != nullptr)
        {
            const int requiredLatencySamples = audioEngine->getRealtimeSignalsmithAlignmentLatencySamples();
            if (requiredLatencySamples != lastReportedLatencySamples)
            {
                setLatencySamples(requiredLatencySamples);
                lastReportedLatencySamples = requiredLatencySamples;
            }
        }
    }

    if (persistentGlobalControlsDirty.load(std::memory_order_acquire) != 0)
    {
        const auto nowWallMs = juce::Time::currentTimeMillis();
        if (lastPersistentGlobalControlsSaveMs == 0
            || (nowWallMs - lastPersistentGlobalControlsSaveMs) >= kPersistentGlobalControlsSaveDebounceMs)
        {
            GlobalSettingsStore::saveControlPages(*this);
            persistentGlobalControlsDirty.store(0, std::memory_order_release);
            lastPersistentGlobalControlsSaveMs = nowWallMs;
        }
    }

    if (pendingPersistentGlobalControlsRestore.load(std::memory_order_acquire) != 0)
    {
        const auto nowWallMs = juce::Time::currentTimeMillis();
        if (nowWallMs >= pendingPersistentGlobalControlsRestoreMs)
        {
            GlobalSettingsStore::loadGlobalControls(*this);
            persistentGlobalControlsApplied = true;
            if (pendingPersistentGlobalControlsRestoreRemaining > 1)
            {
                --pendingPersistentGlobalControlsRestoreRemaining;
                pendingPersistentGlobalControlsRestoreMs = nowWallMs + 400;
            }
            else
            {
                pendingPersistentGlobalControlsRestoreRemaining = 0;
                pendingPersistentGlobalControlsRestore.store(0, std::memory_order_release);
            }
        }
    }

    const int pendingPreset = pendingPresetLoadIndex.load(std::memory_order_acquire);
    if (pendingPreset >= 0)
    {
        double hostPpqSnapshot = 0.0;
        double hostTempoSnapshot = 0.0;
        if (getHostSyncSnapshot(hostPpqSnapshot, hostTempoSnapshot))
        {
            pendingPresetLoadIndex.store(-1, std::memory_order_release);
            performPresetLoad(pendingPreset, hostPpqSnapshot, hostTempoSnapshot);
        }
    }

    if (audioEngine != nullptr
        && (lastPitchCacheRefreshTimeMs == 0
            || (nowMs - lastPitchCacheRefreshTimeMs) >= kPitchCacheRefreshMs))
    {
        for (int stripIndex = 0; stripIndex < MaxStrips; ++stripIndex)
        {
            auto* strip = audioEngine->getStrip(stripIndex);
            auto* engine = getSampleModeEngine(stripIndex, false);
            if (strip == nullptr || engine == nullptr)
                continue;

            const auto playback = resolveFlipPlaybackState(*strip, *engine);
            engine->requestKeyLockRenderCache(playback.playbackRate,
                                              playback.internalPitchSemitones,
                                              playback.shouldBuildKeyLockCache,
                                              playback.tempoMatch.backend);
        }
        lastPitchCacheRefreshTimeMs = nowMs;
    }

    // Update monome LEDs regularly for smooth playhead
    if (monomeConnection.isConnected() && audioEngine)
    {
        const auto nowWallMs = juce::Time::currentTimeMillis();
        if (monomeConnection.supportsGrid()
            && (lastGridLedUpdateTimeMs == 0 || (nowWallMs - lastGridLedUpdateTimeMs) >= kGridRefreshMs))
        {
            updateMonomeLEDs();
            lastGridLedUpdateTimeMs = nowWallMs;
        }
        if (monomeConnection.supportsArc())
            updateMonomeArcRings();
    }
}

//==============================================================================
// Preset Management
//==============================================================================

//==============================================================================
// AudioProcessor Virtual Functions
//==============================================================================

const juce::String MlrVSTAudioProcessor::getName() const
{
    return JucePlugin_Name;
}

bool MlrVSTAudioProcessor::acceptsMidi() const
{
    return true;
}

bool MlrVSTAudioProcessor::producesMidi() const
{
    return false;
}

bool MlrVSTAudioProcessor::isMidiEffect() const
{
    return false;
}

double MlrVSTAudioProcessor::getTailLengthSeconds() const
{
    if constexpr (!kReportRealtimeSignalsmithLatencyToHost)
        return 0.0;

    return (currentSampleRate > 0.0)
        ? (static_cast<double>(juce::jmax(0, lastReportedLatencySamples)) / currentSampleRate)
        : 0.0;
}

int MlrVSTAudioProcessor::getNumPrograms()
{
    return 1;
}

int MlrVSTAudioProcessor::getCurrentProgram()
{
    return 0;
}
