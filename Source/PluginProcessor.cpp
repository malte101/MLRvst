/*
  ==============================================================================

    PluginProcessor.cpp
    mlrVST - Modern Edition Implementation

  ==============================================================================
*/

#include "PluginProcessor.h"
#include "AudioEngineModulation.h"
#include "MacroTargetDispatcher.h"
#include "MonomeFilterActions.h"
#include "MonomeMixActions.h"
#include "PluginEditor.h"
#include "PlayheadSpeedQuantizer.h"
#include "PresetStore.h"
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
constexpr bool kEnableTriggerDebugLogging = false;
constexpr int kPitchControlModeSchemaVersion = 3;
constexpr const char* kEmbeddedFlipSampleAttr = "embeddedSampleWavBase64";
constexpr size_t kMaxEmbeddedFlipWavBytes = 48 * 1024 * 1024;
constexpr int kMaxEmbeddedFlipBase64Chars = 64 * 1024 * 1024;
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

int migrateLegacyPitchControlModeIndex(int storedIndex) noexcept
{
    switch (storedIndex)
    {
        case 1: return 3; // Legacy HQ Pitch -> Signalsmith
        case 2: return 1; // Legacy SoundTouch -> SoundTouch
        case 3: return 2; // Legacy Resample -> Resample
        case 4: return 3; // Legacy Signalsmith -> Signalsmith
        case 0:
        default:
            return 0;
    }
}

int sanitizePitchControlModeIndex(int storedIndex) noexcept
{
    return juce::jlimit(0, 4, storedIndex);
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

bool isValidSceneStoredSamplePath(const juce::File& file)
{
    const auto path = file.getFullPathName().trim();
    if (path.isEmpty() || path.length() > 4096)
        return false;
    if (!juce::File::isAbsolutePath(path))
        return false;
    if (path.contains("\n") || path.contains("\r") || path.contains("://"))
        return false;
    if (path.startsWith("//") || path.startsWith("\\\\"))
        return false;
    return file.hasFileExtension("wav;aif;aiff;mp3;ogg;flac");
}

bool canEmbedSceneAudioBuffer(const juce::AudioBuffer<float>& buffer)
{
    const int channels = buffer.getNumChannels();
    const int samples = buffer.getNumSamples();
    if (channels <= 0 || samples <= 0)
        return false;

    constexpr uint64_t kMaxEmbeddedWavBytes = 48ULL * 1024ULL * 1024ULL;
    const uint64_t estimatedWavBytes = static_cast<uint64_t>(channels)
        * static_cast<uint64_t>(samples)
        * 3ULL
        + 4096ULL;
    return estimatedWavBytes <= kMaxEmbeddedWavBytes;
}

constexpr int kSoundTouchPitchCacheStableTimerTicks = 6;
// Keep the internal Signalsmith alignment delay, but avoid host-facing latency
// restarts for now. Some VST3 hosts can stall if latency changes during or
// just after project/plugin restoration.
constexpr bool kReportRealtimeSignalsmithLatencyToHost = false;

juce::File restoreStoredAbsolutePath(const juce::String& rawPath)
{
    const auto path = rawPath.trim();
    if (path.isEmpty() || !juce::File::isAbsolutePath(path))
        return {};
    return juce::File(path);
}

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

bool safeFileIsDirectory(const juce::File& file)
{
    return canSafelyProbeFilesystemPath(file) && file.exists() && file.isDirectory();
}

juce::String controlModeToKey(MlrVSTAudioProcessor::ControlMode mode)
{
    switch (mode)
    {
        case MlrVSTAudioProcessor::ControlMode::Speed: return "speed";
        case MlrVSTAudioProcessor::ControlMode::Pitch: return "pitch";
        case MlrVSTAudioProcessor::ControlMode::Pan: return "pan";
        case MlrVSTAudioProcessor::ControlMode::Volume: return "volume";
        case MlrVSTAudioProcessor::ControlMode::GrainSize: return "grainsize";
        case MlrVSTAudioProcessor::ControlMode::Filter: return "filter";
        case MlrVSTAudioProcessor::ControlMode::Delay: return "delay";
        case MlrVSTAudioProcessor::ControlMode::Swing: return "swing";
        case MlrVSTAudioProcessor::ControlMode::Gate: return "gate";
        case MlrVSTAudioProcessor::ControlMode::FileBrowser: return "browser";
        case MlrVSTAudioProcessor::ControlMode::GroupAssign: return "group";
        case MlrVSTAudioProcessor::ControlMode::Modulation: return "modulation";
        case MlrVSTAudioProcessor::ControlMode::Preset: return "preset";
        case MlrVSTAudioProcessor::ControlMode::StepEdit: return "stepedit";
        case MlrVSTAudioProcessor::ControlMode::Normal:
        default: return "normal";
    }
}

bool controlModeFromKey(const juce::String& key, MlrVSTAudioProcessor::ControlMode& mode)
{
    const auto normalized = key.trim().toLowerCase();
    if (normalized == "speed") { mode = MlrVSTAudioProcessor::ControlMode::Speed; return true; }
    if (normalized == "pitch") { mode = MlrVSTAudioProcessor::ControlMode::Pitch; return true; }
    if (normalized == "pan") { mode = MlrVSTAudioProcessor::ControlMode::Pan; return true; }
    if (normalized == "volume") { mode = MlrVSTAudioProcessor::ControlMode::Volume; return true; }
    if (normalized == "grainsize" || normalized == "grain_size" || normalized == "grain") { mode = MlrVSTAudioProcessor::ControlMode::GrainSize; return true; }
    if (normalized == "filter") { mode = MlrVSTAudioProcessor::ControlMode::Filter; return true; }
    if (normalized == "delay") { mode = MlrVSTAudioProcessor::ControlMode::Delay; return true; }
    if (normalized == "swing") { mode = MlrVSTAudioProcessor::ControlMode::Swing; return true; }
    if (normalized == "gate") { mode = MlrVSTAudioProcessor::ControlMode::Gate; return true; }
    if (normalized == "browser") { mode = MlrVSTAudioProcessor::ControlMode::FileBrowser; return true; }
    if (normalized == "group") { mode = MlrVSTAudioProcessor::ControlMode::GroupAssign; return true; }
    if (normalized == "mod" || normalized == "modulation") { mode = MlrVSTAudioProcessor::ControlMode::Modulation; return true; }
    if (normalized == "preset") { mode = MlrVSTAudioProcessor::ControlMode::Preset; return true; }
    if (normalized == "stepedit" || normalized == "step_edit" || normalized == "step") { mode = MlrVSTAudioProcessor::ControlMode::StepEdit; return true; }
    return false;
}

constexpr const char* kGlobalSettingsKey = "GlobalSettingsXml";

juce::File getGlobalSettingsFile()
{
    auto presetsRoot = juce::File::getSpecialLocation(juce::File::userHomeDirectory)
        .getChildFile("Library")
        .getChildFile("Audio")
        .getChildFile("Presets")
        .getChildFile("mlrVST")
        .getChildFile("mlrVST");
    return presetsRoot.getChildFile("GlobalSettings.xml");
}

juce::File getLegacyGlobalSettingsFile()
{
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("mlrVST")
        .getChildFile("GlobalSettings.xml");
}

juce::PropertiesFile::Options getLegacySettingsOptions()
{
    juce::PropertiesFile::Options options;
    options.applicationName = "mlrVST";
    options.filenameSuffix = "settings";
    options.folderName = "";
    options.osxLibrarySubFolder = "Application Support";
    options.commonToAllUsers = false;
    options.ignoreCaseOfKeyNames = false;
    options.storageFormat = juce::PropertiesFile::storeAsXML;
    return options;
}

float computeNearestPitchClassCorrectionSemitones(float sourceMidi, int targetRootMidi) noexcept
{
    if (!std::isfinite(sourceMidi))
        return 0.0f;

    const float normalizedSourcePitchClass = [] (float midi)
    {
        float pitchClass = std::fmod(midi, 12.0f);
        if (pitchClass < 0.0f)
            pitchClass += 12.0f;
        return pitchClass;
    }(sourceMidi);

    const float targetPitchClass = static_cast<float>(((targetRootMidi % 12) + 12) % 12);
    float delta = targetPitchClass - normalizedSourcePitchClass;
    while (delta > 6.0f)
        delta -= 12.0f;
    while (delta < -6.0f)
        delta += 12.0f;
    return juce::jlimit(-12.0f, 12.0f, delta);
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

float quantizeFlipLegacyLoopBeats(float beats)
{
    const float roundedQuarterBeat = std::round(juce::jlimit(0.25f, 64.0f, beats) * 4.0f) / 4.0f;
    static constexpr std::array<float, 6> kCommonBeatLengths { 1.0f, 2.0f, 4.0f, 8.0f, 16.0f, 32.0f };
    for (const float common : kCommonBeatLengths)
    {
        if (std::abs(roundedQuarterBeat - common) <= 0.2f)
            return common;
    }
    return juce::jlimit(0.25f, 64.0f, roundedQuarterBeat);
}

juce::String compactLoopPitchAnalysisStatus(juce::String statusText)
{
    const auto text = statusText.trim();
    if (text.isEmpty())
        return {};
    if (text.startsWithIgnoreCase("Loading "))
        return "Loading";
    if (text.containsIgnoreCase("Decoding"))
        return "Decoding";
    if (text.containsIgnoreCase("Waveform"))
        return "Waveform";
    if (text.containsIgnoreCase("Analyzing transients"))
        return "Transients";
    if (text.containsIgnoreCase("Preparing Essentia"))
        return "Preparing";
    if (text.containsIgnoreCase("Essentia"))
        return "Essentia";
    if (text.containsIgnoreCase("Snapping"))
        return "Snapping";
    if (text.containsIgnoreCase("Finalizing"))
        return "Finalizing";
    if (text.containsIgnoreCase("fallback"))
        return "Fallback";
    return text.upToFirstOccurrenceOf("...", false, false);
}

juce::String compactLoopStripLoadStatus(juce::String statusText)
{
    const auto text = statusText.trim();
    if (text.isEmpty())
        return {};
    if (text.startsWithIgnoreCase("Loading "))
        return "Loading";
    if (text.containsIgnoreCase("Decoding"))
        return "Decoding";
    if (text.containsIgnoreCase("Analyzing"))
        return "Analyzing";
    if (text.containsIgnoreCase("Stretch"))
        return "Stretching";
    if (text.containsIgnoreCase("Snapping"))
        return "Snapping";
    if (text.containsIgnoreCase("Ready"))
        return "Ready";
    return text.upToFirstOccurrenceOf("...", false, false);
}

bool decodeLoopStripFileToStereoBuffer(const juce::File& file,
                                       juce::AudioBuffer<float>& decodedBuffer,
                                       double& sourceSampleRate,
                                       juce::String& errorMessage)
{
    if (!safeFileExistsAsFile(file))
    {
        errorMessage = "Missing file";
        return false;
    }

    juce::AudioFormatManager formatManager;
    formatManager.registerBasicFormats();

    std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(file));
    if (reader == nullptr)
    {
        errorMessage = "Unsupported file";
        return false;
    }

    constexpr int64_t kMaxReaderSamples = 100000000;
    constexpr int64_t kMaxIntSamples = 0x7fffffff;
    if (!std::isfinite(reader->sampleRate) || reader->sampleRate <= 0.0 || reader->sampleRate > 384000.0)
    {
        errorMessage = "Invalid sample rate";
        return false;
    }

    if (reader->lengthInSamples <= 0
        || reader->lengthInSamples > kMaxReaderSamples
        || reader->lengthInSamples > kMaxIntSamples)
    {
        errorMessage = "Invalid length";
        return false;
    }

    if (reader->numChannels <= 0 || reader->numChannels > 8)
    {
        errorMessage = "Invalid channel count";
        return false;
    }

    const int channelCount = static_cast<int>(reader->numChannels);
    const int sampleCount = static_cast<int>(reader->lengthInSamples);

    juce::AudioBuffer<float> tempBuffer;
    tempBuffer.setSize(channelCount, sampleCount, false, true, false);
    if (!reader->read(&tempBuffer, 0, sampleCount, 0, true, true))
    {
        errorMessage = "Read failed";
        return false;
    }

    decodedBuffer.setSize(2, tempBuffer.getNumSamples(), false, true, false);
    if (tempBuffer.getNumChannels() == 1)
    {
        decodedBuffer.copyFrom(0, 0, tempBuffer, 0, 0, tempBuffer.getNumSamples());
        decodedBuffer.copyFrom(1, 0, tempBuffer, 0, 0, tempBuffer.getNumSamples());
    }
    else
    {
        decodedBuffer.copyFrom(0, 0, tempBuffer, 0, 0, tempBuffer.getNumSamples());
        decodedBuffer.copyFrom(1, 0, tempBuffer, 1, 0, tempBuffer.getNumSamples());
    }

    sourceSampleRate = reader->sampleRate;
    errorMessage.clear();
    return decodedBuffer.getNumSamples() > 0;
}

int detectLoopStripRecordingBars(const juce::AudioBuffer<float>& decodedBuffer,
                                 double sourceSampleRate,
                                 double hostTempo)
{
    if (decodedBuffer.getNumSamples() <= 0 || !(sourceSampleRate > 0.0))
        return 1;

    const double hostTempoNow = juce::jlimit(20.0, 320.0, hostTempo > 0.0 ? hostTempo : 120.0);
    const double sampleSeconds = static_cast<double>(decodedBuffer.getNumSamples()) / sourceSampleRate;
    const double estimatedBars = (sampleSeconds * hostTempoNow) / 240.0;

    int detectedBars = 1;
    static constexpr int supportedBars[] = { 1, 2, 4, 8 };
    double bestDistance = std::numeric_limits<double>::max();
    for (int candidate : supportedBars)
    {
        const double distance = std::abs(estimatedBars - static_cast<double>(candidate));
        if (distance < bestDistance)
        {
            bestDistance = distance;
            detectedBars = candidate;
        }
    }

    return detectedBars;
}

int computeLoopTempoMatchTargetFrames(double sourceSampleRate,
                                      float beatsForLoop,
                                      double hostTempo)
{
    if (!(sourceSampleRate > 0.0) || !(beatsForLoop > 0.0f) || !(hostTempo > 0.0))
        return 0;

    const double targetDurationSeconds = (static_cast<double>(beatsForLoop) * 60.0) / hostTempo;
    if (!(targetDurationSeconds > 0.0) || !std::isfinite(targetDurationSeconds))
        return 0;

    return juce::jmax(1, static_cast<int>(std::llround(targetDurationSeconds * sourceSampleRate)));
}

MlrVSTAudioProcessor::LoopPitchRole sanitizeLoopPitchRole(int roleValue)
{
    switch (roleValue)
    {
        case static_cast<int>(MlrVSTAudioProcessor::LoopPitchRole::Master):
            return MlrVSTAudioProcessor::LoopPitchRole::Master;
        case static_cast<int>(MlrVSTAudioProcessor::LoopPitchRole::Sync):
            return MlrVSTAudioProcessor::LoopPitchRole::Sync;
        case static_cast<int>(MlrVSTAudioProcessor::LoopPitchRole::None):
        default:
            return MlrVSTAudioProcessor::LoopPitchRole::None;
    }
}

MlrVSTAudioProcessor::LoopPitchSyncTiming sanitizeLoopPitchSyncTiming(int timingValue)
{
    switch (timingValue)
    {
        case static_cast<int>(MlrVSTAudioProcessor::LoopPitchSyncTiming::NextTrigger):
            return MlrVSTAudioProcessor::LoopPitchSyncTiming::NextTrigger;
        case static_cast<int>(MlrVSTAudioProcessor::LoopPitchSyncTiming::NextLoop):
            return MlrVSTAudioProcessor::LoopPitchSyncTiming::NextLoop;
        case static_cast<int>(MlrVSTAudioProcessor::LoopPitchSyncTiming::NextBar):
            return MlrVSTAudioProcessor::LoopPitchSyncTiming::NextBar;
        case static_cast<int>(MlrVSTAudioProcessor::LoopPitchSyncTiming::Immediate):
        default:
            return MlrVSTAudioProcessor::LoopPitchSyncTiming::Immediate;
    }
}

uint64_t computeFlipLegacyLoopSliceSignature(const std::array<SampleSlice, SliceModel::VisibleSliceCount>& slices)
{
    uint64_t signature = 1469598103934665603ull;
    auto hashInt64 = [&signature](int64_t value)
    {
        signature ^= static_cast<uint64_t>(value);
        signature *= 1099511628211ull;
    };

    for (const auto& slice : slices)
    {
        hashInt64(static_cast<int64_t>(slice.id));
        hashInt64(slice.startSample);
        hashInt64(slice.endSample);
    }

    return signature;
}

uint64_t computeFlipLegacyLoopWarpSignature(const std::vector<SampleWarpMarker>& warpMarkers)
{
    uint64_t signature = 1469598103934665603ull;
    auto hashInt64 = [&signature](int64_t value)
    {
        signature ^= static_cast<uint64_t>(value);
        signature *= 1099511628211ull;
    };

    for (const auto& marker : warpMarkers)
    {
        hashInt64(static_cast<int64_t>(marker.id));
        hashInt64(marker.samplePosition);
        hashInt64(static_cast<int64_t>(std::llround(marker.beatPosition * 1000000.0)));
    }

    return signature;
}

std::vector<int64_t> sanitizeFlipBeatTicks(const std::vector<int64_t>& beatTickSamples,
                                           int64_t totalSamples)
{
    return WarpGrid::sanitizeBeatTicks(beatTickSamples, totalSamples);
}

double computeFlipBeatTickPosition(const std::vector<int64_t>& beatTicks,
                                   int64_t samplePosition)
{
    return WarpGrid::computeBeatPositionFromSample(beatTicks, samplePosition);
}

[[maybe_unused]] float computeFlipBeatSpanFromTicks(const std::vector<int64_t>& beatTicks,
                                                    int64_t startSample,
                                                    int64_t endSample)
{
    if (beatTicks.size() < 2 || endSample <= startSample)
        return 0.0f;

    const double startBeat = computeFlipBeatTickPosition(beatTicks, startSample);
    const double endBeat = computeFlipBeatTickPosition(beatTicks, endSample);
    const double beatSpan = endBeat - startBeat;
    if (!(beatSpan > 0.0) || !std::isfinite(beatSpan))
        return 0.0f;

    return static_cast<float>(beatSpan);
}

using FlipWarpAnchor = WarpGrid::Anchor;

std::vector<SampleWarpMarker> sanitizeFlipWarpMarkers(const std::vector<SampleWarpMarker>& warpMarkers,
                                                      int64_t sourceLengthSamples)
{
    return WarpGrid::sanitizeMarkers(warpMarkers, sourceLengthSamples);
}

std::vector<FlipWarpAnchor> buildFlipWarpAnchors(const std::vector<int64_t>& beatTicks,
                                                 const std::vector<SampleWarpMarker>& warpMarkers,
                                                 int64_t sourceLengthSamples)
{
    return WarpGrid::buildAnchors(beatTicks, warpMarkers, sourceLengthSamples);
}

double computeFlipWarpedBeatPosition(const std::vector<int64_t>& beatTicks,
                                     const std::vector<SampleWarpMarker>& warpMarkers,
                                     int64_t samplePosition,
                                     int64_t sourceLengthSamples)
{
    return WarpGrid::computeWarpedBeatPositionForSample(beatTicks, warpMarkers, samplePosition, sourceLengthSamples);
}

int64_t computeFlipSamplePositionFromWarpedBeatPosition(const std::vector<int64_t>& beatTicks,
                                                        const std::vector<SampleWarpMarker>& warpMarkers,
                                                        double targetBeatPosition,
                                                        int64_t sourceLengthSamples)
{
    return WarpGrid::computeSamplePositionFromWarpedBeatPosition(beatTicks,
                                                                 buildFlipWarpAnchors(beatTicks, warpMarkers, sourceLengthSamples),
                                                                 targetBeatPosition,
                                                                 sourceLengthSamples);
}

float computeFlipWarpedBeatSpanFromTicks(const std::vector<int64_t>& beatTicks,
                                         const std::vector<SampleWarpMarker>& warpMarkers,
                                         int64_t startSample,
                                         int64_t endSample,
                                         int64_t sourceLengthSamples)
{
    return WarpGrid::computeWarpedBeatSpan(beatTicks, warpMarkers, startSample, endSample, sourceLengthSamples);
}

void appendUniqueFlipBeatBoundary(std::vector<double>& boundaries, double value)
{
    if (!std::isfinite(value))
        return;

    for (const auto existing : boundaries)
    {
        if (std::abs(existing - value) <= WarpGrid::kBeatEpsilon)
            return;
    }

    boundaries.push_back(value);
}

constexpr double kFlipWarpCrossfadeSeconds = 0.01;
constexpr int kFlipWarpContinuousBlockFrames = 256;
constexpr double kFlipWarpContinuousMaxInputRatio = 20.0;
constexpr int kFlipWarpContinuousDefaultGrainFrames = 128;

struct FlipWarpRenderSegment
{
    int64_t startSample = 0;
    int64_t endSample = 0;
    int timelineStartFrame = 0;
    int targetFrames = 0;
    int headOverlapFrames = 0;
    int tailOverlapFrames = 0;
};

int computeFlipWarpCrossfadeFrames(double sampleRate)
{
    if (!(sampleRate > 0.0) || !std::isfinite(sampleRate))
        return 0;

    return juce::jlimit(0,
                        4096,
                        static_cast<int>(std::llround(sampleRate * kFlipWarpCrossfadeSeconds)));
}

int computeFlipWarpBoundaryOverlapFrames(const FlipWarpRenderSegment& leftSegment,
                                         const FlipWarpRenderSegment& rightSegment,
                                         int desiredOverlapFrames)
{
    if (desiredOverlapFrames <= 0)
        return 0;

    const int leftSourceFrames = static_cast<int>(juce::jmax<int64_t>(0, leftSegment.endSample - leftSegment.startSample));
    const int rightSourceFrames = static_cast<int>(juce::jmax<int64_t>(0, rightSegment.endSample - rightSegment.startSample));
    const int leftLimit = juce::jmax(0, juce::jmin(leftSegment.targetFrames / 4, leftSourceFrames / 4));
    const int rightLimit = juce::jmax(0, juce::jmin(rightSegment.targetFrames / 4, rightSourceFrames / 4));
    return juce::jmax(0, juce::jmin(desiredOverlapFrames, juce::jmin(leftLimit, rightLimit)));
}

int64_t computeFlipWarpedSourceSampleForOutputFrame(const std::vector<int64_t>& beatTicks,
                                                    const std::vector<FlipWarpAnchor>& anchors,
                                                    double startBeat,
                                                    double sourceBeatSpan,
                                                    int targetFrames,
                                                    int outputFrameIndex,
                                                    int64_t sourceLengthSamples)
{
    if (targetFrames <= 0)
        return 0;

    const double alpha = static_cast<double>(juce::jlimit(0, targetFrames, outputFrameIndex))
        / static_cast<double>(juce::jmax(1, targetFrames));
    const double targetBeatPosition = startBeat + (alpha * sourceBeatSpan);
    return WarpGrid::computeSamplePositionFromWarpedBeatPosition(beatTicks,
                                                                 anchors,
                                                                 targetBeatPosition,
                                                                 sourceLengthSamples);
}

double computeFlipWarpedSourceSpeedForOutputFrame(const std::vector<int64_t>& beatTicks,
                                                  const std::vector<FlipWarpAnchor>& anchors,
                                                  double startBeat,
                                                  double sourceBeatSpan,
                                                  int targetFrames,
                                                  int outputFrameIndex,
                                                  int64_t sourceLengthSamples)
{
    if (targetFrames <= 0)
        return 1.0;

    const int centerFrame = juce::jlimit(0, targetFrames, outputFrameIndex);
    const int sampleWindow = juce::jmax(1, juce::jmin(32, targetFrames / 64));
    const int beginFrame = juce::jmax(0, centerFrame - sampleWindow);
    const int endFrame = juce::jmin(targetFrames, centerFrame + sampleWindow);
    if (endFrame <= beginFrame)
        return 1.0;

    const double beginPosition = static_cast<double>(computeFlipWarpedSourceSampleForOutputFrame(beatTicks,
                                                                                                  anchors,
                                                                                                  startBeat,
                                                                                                  sourceBeatSpan,
                                                                                                  targetFrames,
                                                                                                  beginFrame,
                                                                                                  sourceLengthSamples));
    const double endPosition = static_cast<double>(computeFlipWarpedSourceSampleForOutputFrame(beatTicks,
                                                                                                anchors,
                                                                                                startBeat,
                                                                                                sourceBeatSpan,
                                                                                                targetFrames,
                                                                                                endFrame,
                                                                                                sourceLengthSamples));
    const double speed = (endPosition - beginPosition) / static_cast<double>(endFrame - beginFrame);
    if (!(speed > 0.0) || !std::isfinite(speed))
        return 1.0;

    return juce::jlimit(1.0e-4, kFlipWarpContinuousMaxInputRatio, speed);
}

#if MLRVST_ENABLE_BUNGEE
juce::Range<int> computeFlipWarpBungeeChunkCopyRange(const Bungee::OutputChunk& outputChunk,
                                                     double validStartPosition,
                                                     double validEndPosition)
{
    if (outputChunk.frameCount <= 0)
        return {};

    if (outputChunk.request[0] == nullptr || outputChunk.request[1] == nullptr)
        return { 0, outputChunk.frameCount };

    const double positionBegin = outputChunk.request[0]->position;
    const double positionEnd = outputChunk.request[1]->position;
    if (!std::isfinite(positionBegin) || !std::isfinite(positionEnd))
        return {};

    const double span = positionEnd - positionBegin;
    if (!(std::abs(span) > 1.0e-9))
        return { 0, outputChunk.frameCount };

    int headTrimFrames = 0;
    int tailTrimFrames = 0;
    const double framesPerInputFrame = static_cast<double>(outputChunk.frameCount) / std::abs(span);

    if (span > 0.0)
    {
        if (positionBegin < validStartPosition)
            headTrimFrames = static_cast<int>(std::llround((validStartPosition - positionBegin) * framesPerInputFrame));
        if (positionEnd > validEndPosition)
            tailTrimFrames = static_cast<int>(std::llround((positionEnd - validEndPosition) * framesPerInputFrame));
    }
    else
    {
        if (positionBegin > validEndPosition)
            headTrimFrames = static_cast<int>(std::llround((positionBegin - validEndPosition) * framesPerInputFrame));
        if (positionEnd < validStartPosition)
            tailTrimFrames = static_cast<int>(std::llround((validStartPosition - positionEnd) * framesPerInputFrame));
    }

    headTrimFrames = juce::jlimit(0, outputChunk.frameCount, headTrimFrames);
    tailTrimFrames = juce::jlimit(0, outputChunk.frameCount - headTrimFrames, tailTrimFrames);
    return { headTrimFrames, juce::jmax(headTrimFrames, outputChunk.frameCount - tailTrimFrames) };
}
#endif

bool buildLowLevelContinuousBeatWarpedFlipLegacyLoopBuffer(const SampleModeEngine::LegacyLoopSyncInfo& syncInfo,
                                                           int64_t bankStartSample,
                                                           int64_t bankEndSample,
                                                           const juce::AudioBuffer<float>& sourceBankBuffer,
                                                           double hostTempo,
                                                           float targetBeats,
                                                           TimeStretchBackend backend,
                                                           juce::AudioBuffer<float>& warpedBuffer)
{
#if !MLRVST_ENABLE_BUNGEE
    juce::ignoreUnused(syncInfo,
                       bankStartSample,
                       bankEndSample,
                       sourceBankBuffer,
                       hostTempo,
                       targetBeats,
                       backend,
                       warpedBuffer);
    return false;
#else
    if (backend != TimeStretchBackend::Bungee
        || syncInfo.loadedSample == nullptr
        || !(hostTempo > 0.0)
        || !(targetBeats > 0.0f))
    {
        return false;
    }

    const int64_t sourceLengthSamples = syncInfo.loadedSample->sourceLengthSamples;
    const auto beatTicks = sanitizeFlipBeatTicks(syncInfo.loadedSample->analysis.beatTickSamples, sourceLengthSamples);
    if (beatTicks.size() < 2)
        return false;

    const auto warpMarkers = sanitizeFlipWarpMarkers(syncInfo.warpMarkers, sourceLengthSamples);
    const auto anchors = buildFlipWarpAnchors(beatTicks, warpMarkers, sourceLengthSamples);
    if (anchors.size() < 2)
        return false;

    const double startBeat = computeFlipWarpedBeatPosition(beatTicks, warpMarkers, bankStartSample, sourceLengthSamples);
    const double endBeat = computeFlipWarpedBeatPosition(beatTicks, warpMarkers, bankEndSample, sourceLengthSamples);
    const double sourceBeatSpan = endBeat - startBeat;
    if (!(sourceBeatSpan > 0.0) || !std::isfinite(sourceBeatSpan))
        return false;

    const double targetBeatSpan = static_cast<double>(targetBeats);
    const double sourceSampleRate = syncInfo.loadedSample->sourceSampleRate;
    const double hostSamplesPerBeat = (60.0 / hostTempo) * sourceSampleRate;
    if (!(hostSamplesPerBeat > 0.0) || !std::isfinite(hostSamplesPerBeat))
        return false;

    const int targetFrames = juce::jmax(1, static_cast<int>(std::llround(targetBeatSpan * hostSamplesPerBeat)));
    const int bankLength = juce::jmax(1, sourceBankBuffer.getNumSamples());
    const int outputChannels = juce::jmax(1, sourceBankBuffer.getNumChannels());

    Bungee::SampleRates sampleRates {
        juce::jmax(1, static_cast<int>(std::lround(sourceSampleRate))),
        juce::jmax(1, static_cast<int>(std::lround(sourceSampleRate)))
    };
    Bungee::Stretcher<Bungee::Basic> stretcher(sampleRates, outputChannels);
    const int maxInputFrames = juce::jmax(1, stretcher.maxInputFrameCount());
    std::vector<float> inputScratch(static_cast<size_t>(outputChannels * maxInputFrames), 0.0f);

    auto computeLocalSourceSampleForOutputFrame = [&](int outputFrameIndex)
    {
        return static_cast<double>(computeFlipWarpedSourceSampleForOutputFrame(beatTicks,
                                                                               anchors,
                                                                               startBeat,
                                                                               sourceBeatSpan,
                                                                               targetFrames,
                                                                               outputFrameIndex,
                                                                               sourceLengthSamples)
                                   - bankStartSample);
    };

    auto computeLocalSourceSpeedForOutputFrame = [&](int outputFrameIndex)
    {
        return computeFlipWarpedSourceSpeedForOutputFrame(beatTicks,
                                                          anchors,
                                                          startBeat,
                                                          sourceBeatSpan,
                                                          targetFrames,
                                                          outputFrameIndex,
                                                          sourceLengthSamples);
    };

    auto prepareInputChunk = [&](const Bungee::InputChunk& inputChunk)
    {
        const int inputFrames = inputChunk.end - inputChunk.begin;
        if (inputFrames <= 0 || inputFrames > maxInputFrames)
            return false;

        std::fill(inputScratch.begin(), inputScratch.end(), 0.0f);

        const int sourceStartFrame = juce::jlimit(0, bankLength, inputChunk.begin);
        const int sourceEndFrame = juce::jlimit(0, bankLength, inputChunk.end);
        const int copyFrames = juce::jmax(0, sourceEndFrame - sourceStartFrame);
        if (copyFrames <= 0)
            return true;

        const int writeOffset = sourceStartFrame - inputChunk.begin;
        for (int ch = 0; ch < outputChannels; ++ch)
        {
            const float* source = sourceBankBuffer.getReadPointer(ch, sourceStartFrame);
            float* dest = inputScratch.data() + (ch * maxInputFrames) + writeOffset;
            std::copy(source, source + copyFrames, dest);
        }

        return true;
    };

    warpedBuffer.setSize(outputChannels, targetFrames, false, false, true);
    warpedBuffer.clear();

    int outputCursor = 0;
    int estimatedGrainFrames = kFlipWarpContinuousDefaultGrainFrames;
    int guard = 0;
    const int maxGuardIterations = juce::jmax(512, targetFrames * 8);

    Bungee::Request request {};
    request.position = computeLocalSourceSampleForOutputFrame(0);
    request.speed = computeLocalSourceSpeedForOutputFrame(0);
    request.pitch = 1.0;
    request.reset = true;
    request.resampleMode = resampleMode_autoOut;
    stretcher.preroll(request);
    double lastRequestedPosition = request.position;

    while (outputCursor < targetFrames && guard++ < maxGuardIterations)
    {
        const auto inputChunk = stretcher.specifyGrain(request);
        if (!prepareInputChunk(inputChunk))
            return false;

        stretcher.analyseGrain(inputScratch.data(), maxInputFrames);

        Bungee::OutputChunk outputChunk {};
        stretcher.synthesiseGrain(outputChunk);
        if (outputChunk.frameCount <= 0 || outputChunk.data == nullptr)
            return false;

        estimatedGrainFrames = juce::jmax(1, outputChunk.frameCount);
        const auto copyRange = computeFlipWarpBungeeChunkCopyRange(outputChunk,
                                                                   0.0,
                                                                   static_cast<double>(bankLength));
        const int copyStart = juce::jlimit(0, outputChunk.frameCount, copyRange.getStart());
        const int copyFrames = juce::jmin(copyRange.getLength(), targetFrames - outputCursor);
        if (copyFrames > 0)
        {
            for (int ch = 0; ch < outputChannels; ++ch)
            {
                const float* source = outputChunk.data + copyStart + (ch * outputChunk.channelStride);
                float* dest = warpedBuffer.getWritePointer(ch, outputCursor);
                std::copy(source, source + copyFrames, dest);
            }

            outputCursor += copyFrames;
        }

        if (outputCursor >= targetFrames)
            break;

        const int nextAnchorFrame = juce::jlimit(0,
                                                 targetFrames,
                                                 outputCursor + juce::jmax(1, estimatedGrainFrames / 2));
        request.position = computeLocalSourceSampleForOutputFrame(nextAnchorFrame);
        request.speed = computeLocalSourceSpeedForOutputFrame(nextAnchorFrame);
        request.pitch = 1.0;
        request.reset = !(request.position > lastRequestedPosition);
        request.resampleMode = resampleMode_autoOut;
        lastRequestedPosition = request.position;
    }

    return outputCursor == targetFrames;
#endif
}

bool buildContinuousBeatWarpedFlipLegacyLoopBuffer(const SampleModeEngine::LegacyLoopSyncInfo& syncInfo,
                                                   int64_t bankStartSample,
                                                   int64_t bankEndSample,
                                                   const juce::AudioBuffer<float>& sourceBankBuffer,
                                                   double hostTempo,
                                                   float targetBeats,
                                                   TimeStretchBackend backend,
                                                   juce::AudioBuffer<float>& warpedBuffer)
{
#if !MLRVST_ENABLE_BUNGEE
    juce::ignoreUnused(syncInfo,
                       bankStartSample,
                       bankEndSample,
                       sourceBankBuffer,
                       hostTempo,
                       targetBeats,
                       backend,
                       warpedBuffer);
    return false;
#else
    if (backend != TimeStretchBackend::Bungee
        || syncInfo.loadedSample == nullptr
        || !(hostTempo > 0.0)
        || !(targetBeats > 0.0f))
    {
        return false;
    }

    const int64_t sourceLengthSamples = syncInfo.loadedSample->sourceLengthSamples;
    const auto beatTicks = sanitizeFlipBeatTicks(syncInfo.loadedSample->analysis.beatTickSamples, sourceLengthSamples);
    if (beatTicks.size() < 2)
        return false;

    const auto warpMarkers = sanitizeFlipWarpMarkers(syncInfo.warpMarkers, sourceLengthSamples);
    const auto anchors = buildFlipWarpAnchors(beatTicks, warpMarkers, sourceLengthSamples);
    if (anchors.size() < 2)
        return false;

    const double startBeat = computeFlipWarpedBeatPosition(beatTicks, warpMarkers, bankStartSample, sourceLengthSamples);
    const double endBeat = computeFlipWarpedBeatPosition(beatTicks, warpMarkers, bankEndSample, sourceLengthSamples);
    const double sourceBeatSpan = endBeat - startBeat;
    if (!(sourceBeatSpan > 0.0) || !std::isfinite(sourceBeatSpan))
        return false;

    const double targetBeatSpan = static_cast<double>(targetBeats);
    const double sourceSampleRate = syncInfo.loadedSample->sourceSampleRate;
    const double hostSamplesPerBeat = (60.0 / hostTempo) * sourceSampleRate;
    if (!(hostSamplesPerBeat > 0.0) || !std::isfinite(hostSamplesPerBeat))
        return false;

    const int targetFrames = juce::jmax(1, static_cast<int>(std::llround(targetBeatSpan * hostSamplesPerBeat)));
    const int bankLength = juce::jmax(1, sourceBankBuffer.getNumSamples());
    const int outputChannels = juce::jmax(1, sourceBankBuffer.getNumChannels());

    Bungee::SampleRates sampleRates {
        juce::jmax(1, static_cast<int>(std::lround(sourceSampleRate))),
        juce::jmax(1, static_cast<int>(std::lround(sourceSampleRate)))
    };
    Bungee::Stretcher<Bungee::Basic> stretcher(sampleRates, outputChannels);
    Bungee::Stream<Bungee::Basic> stream(stretcher,
                                         static_cast<int>(std::ceil(kFlipWarpContinuousBlockFrames
                                                                    * kFlipWarpContinuousMaxInputRatio)) + 8,
                                         outputChannels);

    warpedBuffer.setSize(outputChannels, targetFrames, false, false, true);
    warpedBuffer.clear();

    std::vector<const float*> inputPointers(static_cast<size_t>(outputChannels), nullptr);
    std::vector<float*> outputPointers(static_cast<size_t>(outputChannels), nullptr);

    int inputCursor = 0;
    int outputCursor = 0;
    int guard = 0;

    while (outputCursor < targetFrames && guard++ < (targetFrames * 4))
    {
        const int remainingFrames = targetFrames - outputCursor;
        int outputBlockFrames = juce::jmin(kFlipWarpContinuousBlockFrames, remainingFrames);

        int desiredInputEnd = inputCursor;
        while (true)
        {
            const int64_t desiredEndSample = computeFlipWarpedSourceSampleForOutputFrame(beatTicks,
                                                                                         anchors,
                                                                                         startBeat,
                                                                                         sourceBeatSpan,
                                                                                         targetFrames,
                                                                                         outputCursor + outputBlockFrames,
                                                                                         sourceLengthSamples);
            desiredInputEnd = static_cast<int>(juce::jlimit<int64_t>(0,
                                                                     bankLength,
                                                                     desiredEndSample - bankStartSample));
            if (desiredInputEnd > inputCursor || outputBlockFrames >= remainingFrames)
                break;

            outputBlockFrames = juce::jmin(remainingFrames, outputBlockFrames * 2);
        }

        const int inputFrames = desiredInputEnd - inputCursor;
        if (inputFrames <= 0
            || inputFrames > static_cast<int>(std::ceil(static_cast<double>(outputBlockFrames)
                                                        * kFlipWarpContinuousMaxInputRatio)))
        {
            return false;
        }

        for (int ch = 0; ch < outputChannels; ++ch)
        {
            inputPointers[static_cast<size_t>(ch)] = sourceBankBuffer.getReadPointer(ch, inputCursor);
            outputPointers[static_cast<size_t>(ch)] = warpedBuffer.getWritePointer(ch, outputCursor);
        }

        const int renderedFrames = stream.process(inputPointers.data(),
                                                  outputPointers.data(),
                                                  inputFrames,
                                                  static_cast<double>(outputBlockFrames),
                                                  1.0);
        if (renderedFrames != outputBlockFrames)
            return false;

        inputCursor += inputFrames;
        outputCursor += renderedFrames;
    }

    return outputCursor == targetFrames;
#endif
}

bool buildSegmentedBeatWarpedFlipLegacyLoopBuffer(const SampleModeEngine::LegacyLoopSyncInfo& syncInfo,
                                                  int64_t bankStartSample,
                                                  int64_t bankEndSample,
                                                  double hostTempo,
                                                  float targetBeats,
                                                  TimeStretchBackend backend,
                                                  juce::AudioBuffer<float>& warpedBuffer)
{
    if (syncInfo.loadedSample == nullptr
        || !(hostTempo > 0.0)
        || !(targetBeats > 0.0f))
    {
        return false;
    }

    const int64_t sourceLengthSamples = syncInfo.loadedSample->sourceLengthSamples;
    const auto beatTicks = sanitizeFlipBeatTicks(syncInfo.loadedSample->analysis.beatTickSamples, sourceLengthSamples);
    if (beatTicks.size() < 2)
        return false;

    const auto warpMarkers = sanitizeFlipWarpMarkers(syncInfo.warpMarkers, sourceLengthSamples);
    const double startBeat = computeFlipWarpedBeatPosition(beatTicks, warpMarkers, bankStartSample, sourceLengthSamples);
    const double endBeat = computeFlipWarpedBeatPosition(beatTicks, warpMarkers, bankEndSample, sourceLengthSamples);
    const double sourceBeatSpan = endBeat - startBeat;
    if (!(sourceBeatSpan > 0.0) || !std::isfinite(sourceBeatSpan))
        return false;

    const double targetBeatSpan = static_cast<double>(targetBeats);
    const double beatScale = targetBeatSpan / sourceBeatSpan;
    if (!(beatScale > 0.0) || !std::isfinite(beatScale))
        return false;

    const double sourceSampleRate = syncInfo.loadedSample->sourceSampleRate;
    const double hostSamplesPerBeat = (60.0 / hostTempo) * sourceSampleRate;
    if (!(hostSamplesPerBeat > 0.0) || !std::isfinite(hostSamplesPerBeat))
        return false;

    const int targetFrames = juce::jmax(1, static_cast<int>(std::llround(targetBeatSpan * hostSamplesPerBeat)));
    const auto& sourceBuffer = syncInfo.loadedSample->audioBuffer;
    const int sourceChannels = juce::jmax(1, sourceBuffer.getNumChannels());
    const int outputChannels = juce::jmax(2, sourceChannels);

    std::vector<double> beatBoundaries;
    beatBoundaries.reserve(static_cast<size_t>(std::ceil(sourceBeatSpan)) + warpMarkers.size() + 2u);
    appendUniqueFlipBeatBoundary(beatBoundaries, startBeat);
    for (int wholeBeat = static_cast<int>(std::floor(startBeat)) + 1;
         static_cast<double>(wholeBeat) < (endBeat - 1.0e-6);
         ++wholeBeat)
    {
        appendUniqueFlipBeatBoundary(beatBoundaries, static_cast<double>(wholeBeat));
    }
    for (const auto& marker : warpMarkers)
    {
        if (marker.beatPosition > (startBeat + WarpGrid::kBeatEpsilon)
            && marker.beatPosition < (endBeat - WarpGrid::kBeatEpsilon))
        {
            appendUniqueFlipBeatBoundary(beatBoundaries, marker.beatPosition);
        }
    }
    appendUniqueFlipBeatBoundary(beatBoundaries, endBeat);
    std::sort(beatBoundaries.begin(), beatBoundaries.end());

    if (beatBoundaries.size() < 2)
        return false;

    std::vector<FlipWarpRenderSegment> renderSegments;
    renderSegments.reserve(beatBoundaries.size() - 1u);

    int timelineFramePosition = 0;
    const int totalSegments = static_cast<int>(beatBoundaries.size()) - 1;
    for (int segmentIndex = 0; segmentIndex < totalSegments; ++segmentIndex)
    {
        const double segmentBeatStart = beatBoundaries[static_cast<size_t>(segmentIndex)];
        const double segmentBeatEnd = beatBoundaries[static_cast<size_t>(segmentIndex + 1)];
        if (!(segmentBeatEnd > segmentBeatStart))
            continue;

        int64_t segmentStartSample = (segmentIndex == 0)
            ? bankStartSample
            : computeFlipSamplePositionFromWarpedBeatPosition(beatTicks,
                                                              warpMarkers,
                                                              segmentBeatStart,
                                                              sourceLengthSamples);
        int64_t segmentEndSample = (segmentIndex == (totalSegments - 1))
            ? bankEndSample
            : computeFlipSamplePositionFromWarpedBeatPosition(beatTicks,
                                                              warpMarkers,
                                                              segmentBeatEnd,
                                                              sourceLengthSamples);
        segmentStartSample = juce::jlimit<int64_t>(bankStartSample, bankEndSample - 1, segmentStartSample);
        segmentEndSample = juce::jlimit<int64_t>(segmentStartSample + 1, bankEndSample, segmentEndSample);
        if (segmentEndSample <= segmentStartSample)
            continue;

        const double targetSegmentBeatEnd = (segmentBeatEnd - startBeat) * beatScale;
        const int futureSegments = totalSegments - segmentIndex - 1;
        const int minSegmentEndFrame = timelineFramePosition + 1;
        const int maxSegmentEndFrame = juce::jmax(minSegmentEndFrame, targetFrames - futureSegments);
        const int targetSegmentEndFrame = (segmentIndex == (totalSegments - 1))
            ? targetFrames
            : juce::jlimit(minSegmentEndFrame,
                           maxSegmentEndFrame,
                           static_cast<int>(std::llround(targetSegmentBeatEnd * hostSamplesPerBeat)));
        const int segmentTargetFrames = juce::jmax(1, targetSegmentEndFrame - timelineFramePosition);

        renderSegments.push_back({ segmentStartSample,
                                   segmentEndSample,
                                   timelineFramePosition,
                                   segmentTargetFrames,
                                   0,
                                   0 });
        timelineFramePosition = targetSegmentEndFrame;
    }

    if (renderSegments.empty() || timelineFramePosition != targetFrames)
        return false;

    const int desiredOverlapFrames = computeFlipWarpCrossfadeFrames(sourceSampleRate);
    for (size_t i = 1; i < renderSegments.size(); ++i)
    {
        const int overlapFrames = computeFlipWarpBoundaryOverlapFrames(renderSegments[i - 1],
                                                                       renderSegments[i],
                                                                       desiredOverlapFrames);
        renderSegments[i - 1].tailOverlapFrames = overlapFrames;
        renderSegments[i].headOverlapFrames = overlapFrames;
    }

    warpedBuffer.setSize(outputChannels, targetFrames, false, false, true);
    warpedBuffer.clear();

    for (const auto& segment : renderSegments)
    {
        const int segmentSourceFrames = static_cast<int>(segment.endSample - segment.startSample);
        if (segmentSourceFrames <= 0 || segment.targetFrames <= 0)
            continue;

        const int extraSourceFrames = (segment.tailOverlapFrames > 0 && segment.targetFrames > 0)
            ? juce::jmax(1,
                         static_cast<int>(std::llround((static_cast<double>(segmentSourceFrames)
                                                        * static_cast<double>(segment.tailOverlapFrames))
                                                       / static_cast<double>(segment.targetFrames))))
            : 0;
        const int64_t renderEndSample = juce::jlimit<int64_t>(segment.startSample + 1,
                                                              bankEndSample,
                                                              segment.endSample + extraSourceFrames);
        const int renderSourceFrames = static_cast<int>(renderEndSample - segment.startSample);
        const int renderTargetFrames = juce::jmax(1, segment.targetFrames + segment.tailOverlapFrames);

        juce::AudioBuffer<float> sourceSegment(outputChannels, renderSourceFrames);
        sourceSegment.clear();
        for (int ch = 0; ch < outputChannels; ++ch)
        {
            const int sourceCh = juce::jmin(ch, sourceChannels - 1);
            sourceSegment.copyFrom(ch,
                                   0,
                                   sourceBuffer,
                                   sourceCh,
                                   static_cast<int>(segment.startSample),
                                   renderSourceFrames);
        }

        juce::AudioBuffer<float> stretchedSegment;
        if (!renderTimeStretchedBuffer(sourceSegment,
                                       sourceSampleRate,
                                       renderTargetFrames,
                                       0.0f,
                                       backend,
                                       stretchedSegment))
        {
            if (!renderTimeStretchedBuffer(sourceSegment,
                                           sourceSampleRate,
                                           renderTargetFrames,
                                           0.0f,
                                           TimeStretchBackend::Resample,
                                           stretchedSegment))
            {
                return false;
            }
        }

        const int framesToCopy = juce::jmin(renderTargetFrames,
                                            juce::jmin(juce::jmax(0, targetFrames - segment.timelineStartFrame),
                                                       stretchedSegment.getNumSamples()));
        if (framesToCopy <= 0)
            continue;

        const int headOverlapFrames = juce::jmin(segment.headOverlapFrames, framesToCopy);

        for (int ch = 0; ch < outputChannels; ++ch)
        {
            const int stretchedCh = juce::jmin(ch, juce::jmax(0, stretchedSegment.getNumChannels() - 1));
            for (int frame = 0; frame < headOverlapFrames; ++frame)
            {
                const int outputFrame = segment.timelineStartFrame + frame;
                const double alpha = static_cast<double>(frame + 1) / static_cast<double>(headOverlapFrames + 1);
                const float fadeIn = static_cast<float>(std::sin(alpha * juce::MathConstants<double>::halfPi));
                const float fadeOut = static_cast<float>(std::cos(alpha * juce::MathConstants<double>::halfPi));
                const float existing = warpedBuffer.getSample(ch, outputFrame);
                const float incoming = stretchedSegment.getSample(stretchedCh, frame);
                warpedBuffer.setSample(ch, outputFrame, (existing * fadeOut) + (incoming * fadeIn));
            }

            if (framesToCopy > headOverlapFrames)
            {
                warpedBuffer.copyFrom(ch,
                                      segment.timelineStartFrame + headOverlapFrames,
                                      stretchedSegment,
                                      stretchedCh,
                                      headOverlapFrames,
                                      framesToCopy - headOverlapFrames);
            }
        }
    }

    return true;
}

std::array<int, SliceModel::VisibleSliceCount> buildFlipLegacyLoopTransientSliceCache(
    const SampleModeEngine::LegacyLoopSyncInfo& syncInfo,
    int64_t bankStartSample,
    int64_t bankEndSample)
{
    std::array<int, SliceModel::VisibleSliceCount> cachedStarts {};
    const int64_t sourceLength = juce::jmax<int64_t>(1, bankEndSample - bankStartSample);
    int previousStart = -1;

    for (int slot = 0; slot < SliceModel::VisibleSliceCount; ++slot)
    {
        int64_t desiredStart = bankStartSample;
        const auto& slice = syncInfo.visibleSlices[static_cast<size_t>(slot)];
        if (slice.id >= 0 && slice.endSample > slice.startSample)
            desiredStart = slice.startSample;
        else
            desiredStart = bankStartSample + ((static_cast<int64_t>(slot) * sourceLength) / SliceModel::VisibleSliceCount);

        int clampedStart = static_cast<int>(juce::jlimit<int64_t>(0, sourceLength - 1, desiredStart - bankStartSample));
        clampedStart = juce::jmax(previousStart + 1, clampedStart);
        clampedStart = juce::jlimit(0, static_cast<int>(sourceLength - 1), clampedStart);
        cachedStarts[static_cast<size_t>(slot)] = clampedStart;
        previousStart = clampedStart;
    }

    return cachedStarts;
}

void buildFlipLegacyLoopAnalysisMaps(const juce::AudioBuffer<float>& buffer,
                                     std::array<float, 128>& rmsMap,
                                     std::array<int, 128>& zeroCrossMap)
{
    rmsMap.fill(0.0f);
    zeroCrossMap.fill(0);

    const int totalSamples = buffer.getNumSamples();
    const int channels = juce::jmax(1, buffer.getNumChannels());
    if (totalSamples <= 0)
        return;

    std::vector<float> monoSamples(static_cast<size_t>(totalSamples), 0.0f);
    for (int i = 0; i < totalSamples; ++i)
    {
        float mono = 0.0f;
        for (int ch = 0; ch < channels; ++ch)
            mono += buffer.getSample(ch, i);
        monoSamples[static_cast<size_t>(i)] = mono / static_cast<float>(channels);
    }

    float maxRms = 1.0e-6f;
    const int bins = static_cast<int>(rmsMap.size());
    for (int bin = 0; bin < bins; ++bin)
    {
        const int start = (bin * totalSamples) / bins;
        const int end = juce::jmax(start + 1, ((bin + 1) * totalSamples) / bins);
        const int count = juce::jmax(1, end - start);

        double energy = 0.0;
        for (int i = start; i < end; ++i)
        {
            const float sample = monoSamples[static_cast<size_t>(juce::jlimit(0, totalSamples - 1, i))];
            energy += static_cast<double>(sample * sample);
        }

        const float rms = static_cast<float>(std::sqrt(energy / static_cast<double>(count)));
        rmsMap[static_cast<size_t>(bin)] = rms;
        maxRms = juce::jmax(maxRms, rms);

        int zeroIndex = juce::jlimit(0, totalSamples - 1, start);
        for (int i = juce::jmax(start + 1, 1); i < juce::jmin(end, totalSamples); ++i)
        {
            const float prev = monoSamples[static_cast<size_t>(i - 1)];
            const float curr = monoSamples[static_cast<size_t>(i)];
            if ((prev <= 0.0f && curr > 0.0f) || (prev >= 0.0f && curr < 0.0f))
            {
                zeroIndex = i;
                break;
            }
        }
        zeroCrossMap[static_cast<size_t>(bin)] = zeroIndex;
    }

    const float invMax = (maxRms > 1.0e-6f) ? (1.0f / maxRms) : 1.0f;
    for (auto& value : rmsMap)
        value = juce::jlimit(0.0f, 1.0f, value * invMax);
}

bool computeFlipLegacyLoopBankRange(const SampleModeEngine::LegacyLoopSyncInfo& syncInfo,
                                    int64_t& bankStartSample,
                                    int64_t& bankEndSample)
{
    if (syncInfo.loadedSample == nullptr || syncInfo.loadedSample->sourceSampleRate <= 0.0)
        return false;

    const int64_t sourceLength = juce::jmax<int64_t>(1, syncInfo.loadedSample->audioBuffer.getNumSamples());
    if (syncInfo.bankEndSample > syncInfo.bankStartSample)
    {
        bankStartSample = juce::jlimit<int64_t>(0, sourceLength - 1, syncInfo.bankStartSample);
        bankEndSample = juce::jlimit<int64_t>(bankStartSample + 1, sourceLength, syncInfo.bankEndSample);
        return bankEndSample > bankStartSample;
    }

    bankStartSample = std::numeric_limits<int64_t>::max();
    bankEndSample = 0;
    for (const auto& slice : syncInfo.visibleSlices)
    {
        if (slice.id < 0 || slice.endSample <= slice.startSample)
            continue;

        bankStartSample = juce::jmin(bankStartSample, slice.startSample);
        bankEndSample = juce::jmax(bankEndSample, slice.endSample);
    }

    if (bankStartSample == std::numeric_limits<int64_t>::max() || bankEndSample <= bankStartSample)
        return false;

    bankStartSample = juce::jlimit<int64_t>(0, sourceLength - 1, bankStartSample);
    bankEndSample = juce::jlimit<int64_t>(bankStartSample + 1, sourceLength, bankEndSample);

    return bankEndSample > bankStartSample;
}

float computeFlipLegacyLoopVisibleBankBeats(const SampleModeEngine::LegacyLoopSyncInfo& syncInfo)
{
    if (syncInfo.visibleBankBeats > 0.0f)
        return syncInfo.visibleBankBeats;

    int64_t startSample = 0;
    int64_t endSample = 0;
    if (!computeFlipLegacyLoopBankRange(syncInfo, startSample, endSample))
        return 4.0f;

    if (syncInfo.legacyLoopBarSelection > 0)
        return decodeBarSelection(syncInfo.legacyLoopBarSelection).beatsPerLoop;

    const int64_t totalSamples = syncInfo.loadedSample != nullptr
        ? syncInfo.loadedSample->sourceLengthSamples
        : 0;
    const auto beatTicks = (syncInfo.loadedSample != nullptr)
        ? sanitizeFlipBeatTicks(syncInfo.loadedSample->analysis.beatTickSamples, totalSamples)
        : std::vector<int64_t>{};
    if (const float tickBeats = computeFlipWarpedBeatSpanFromTicks(beatTicks,
                                                                   syncInfo.warpMarkers,
                                                                   startSample,
                                                                   endSample,
                                                                   totalSamples);
        tickBeats > 0.0f)
    {
        return quantizeFlipLegacyLoopBeats(tickBeats);
    }

    const double sourceTempo = syncInfo.analyzedTempoBpm > 0.0
        ? syncInfo.analyzedTempoBpm
        : syncInfo.loadedSample->analysis.estimatedTempoBpm;
    if (!(sourceTempo > 0.0) || !std::isfinite(sourceTempo))
        return 4.0f;

    const double seconds = static_cast<double>(endSample - startSample)
        / juce::jmax(1.0, syncInfo.loadedSample->sourceSampleRate);
    const double beats = seconds * (sourceTempo / 60.0);
    return quantizeFlipLegacyLoopBeats(static_cast<float>(beats));
}

bool stretchFlipLegacyLoopBufferTempo(const juce::AudioBuffer<float>& sourceBuffer,
                                      double sourceSampleRate,
                                      double hostTempo,
                                      float visibleBankBeats,
                                      TimeStretchBackend backend,
                                      juce::AudioBuffer<float>& stretchedBuffer)
{
    const int numSamples = sourceBuffer.getNumSamples();
    if (numSamples <= 0 || sourceSampleRate <= 0.0 || hostTempo <= 0.0 || visibleBankBeats <= 0.0f)
        return false;

    const double rawDurationSeconds = static_cast<double>(numSamples) / sourceSampleRate;
    const double targetDurationSeconds = (static_cast<double>(visibleBankBeats) * 60.0) / hostTempo;
    if (!(rawDurationSeconds > 0.0) || !(targetDurationSeconds > 0.0))
        return false;

    const int targetFrames = juce::jmax(1, static_cast<int>(std::lround(targetDurationSeconds * sourceSampleRate)));
    if (targetFrames == numSamples)
        return false;
    return renderTimeStretchedBuffer(sourceBuffer,
                                     sourceSampleRate,
                                     targetFrames,
                                     0.0f,
                                     backend,
                                     stretchedBuffer);
}

bool buildFlipLegacyLoopBankBuffer(const SampleModeEngine::LegacyLoopSyncInfo& syncInfo,
                                   double hostTempo,
                                   TimeStretchBackend backend,
                                   float visibleBankBeats,
                                   juce::AudioBuffer<float>& bankBuffer)
{
    if (syncInfo.loadedSample == nullptr)
        return false;

    int64_t bankStartSample = 0;
    int64_t bankEndSample = 0;
    if (!computeFlipLegacyLoopBankRange(syncInfo, bankStartSample, bankEndSample))
        return false;

    const auto& sourceBuffer = syncInfo.loadedSample->audioBuffer;
    const int bankLength = static_cast<int>(juce::jmax<int64_t>(1, bankEndSample - bankStartSample));
    const int sourceChannels = juce::jmax(1, sourceBuffer.getNumChannels());
    juce::AudioBuffer<float> rawBankBuffer(juce::jmax(2, sourceChannels), bankLength);
    rawBankBuffer.clear();

    for (int ch = 0; ch < rawBankBuffer.getNumChannels(); ++ch)
    {
        const int sourceCh = juce::jmin(ch, sourceChannels - 1);
        rawBankBuffer.copyFrom(ch,
                               0,
                               sourceBuffer,
                               sourceCh,
                               static_cast<int>(bankStartSample),
                               bankLength);
    }

    if (buildLowLevelContinuousBeatWarpedFlipLegacyLoopBuffer(syncInfo,
                                                              bankStartSample,
                                                              bankEndSample,
                                                              rawBankBuffer,
                                                              hostTempo,
                                                              visibleBankBeats,
                                                              backend,
                                                              bankBuffer))
    {
        return true;
    }

    if (buildContinuousBeatWarpedFlipLegacyLoopBuffer(syncInfo,
                                                      bankStartSample,
                                                      bankEndSample,
                                                      rawBankBuffer,
                                                      hostTempo,
                                                      visibleBankBeats,
                                                      backend,
                                                      bankBuffer))
    {
        return true;
    }

    if (buildSegmentedBeatWarpedFlipLegacyLoopBuffer(syncInfo,
                                                     bankStartSample,
                                                     bankEndSample,
                                                     hostTempo,
                                                     visibleBankBeats,
                                                     backend,
                                                     bankBuffer))
    {
        return true;
    }

    bankBuffer = rawBankBuffer;
    if (backend != TimeStretchBackend::Resample)
    {
        juce::AudioBuffer<float> stretchedBankBuffer;
        if (stretchFlipLegacyLoopBufferTempo(rawBankBuffer,
                                             syncInfo.loadedSample->sourceSampleRate,
                                             hostTempo,
                                             visibleBankBeats,
                                             backend,
                                             stretchedBankBuffer))
            bankBuffer = std::move(stretchedBankBuffer);
    }
    return true;
}

using MacroTarget = MlrVSTAudioProcessor::MacroTarget;
MacroTarget sanitizeMacroTarget(int rawTarget)
{
    return sanitizeMacroPerformanceTarget(performanceTargetFromRaw(rawTarget));
}

float denormalizeMacroLinearValue(float normalized, float minValue, float maxValue)
{
    return juce::jmap(juce::jlimit(0.0f, 1.0f, normalized), 0.0f, 1.0f, minValue, maxValue);
}

float denormalizeMacroCutoffValue(float normalized)
{
    juce::NormalisableRange<float> range(20.0f, 20000.0f, 1.0f);
    range.setSkewForCentre(1000.0f);
    return juce::jlimit(20.0f, 20000.0f, range.convertFrom0to1(juce::jlimit(0.0f, 1.0f, normalized)));
}

float denormalizeMacroPitchValue(float normalized)
{
    return denormalizeMacroLinearValue(normalized, -24.0f, 24.0f);
}

constexpr std::array<int, MlrVSTAudioProcessor::MacroCount> kAkaiMpkMiniMacroCcs {
    70, 71, 72, 73, -1, -1, -1, -1
};

bool encodeBufferAsWavBase64(const juce::AudioBuffer<float>& buffer,
                             double sampleRate,
                             juce::String& outBase64)
{
    outBase64.clear();

    if (buffer.getNumSamples() <= 0
        || buffer.getNumChannels() <= 0
        || !std::isfinite(sampleRate)
        || sampleRate <= 1000.0)
    {
        return false;
    }

    auto wavBytes = std::make_unique<juce::MemoryOutputStream>();
    auto* wavBytesRaw = wavBytes.get();
    juce::WavAudioFormat wavFormat;
    auto writerStream = std::unique_ptr<juce::OutputStream>(wavBytes.release());
    const auto writerOptions = juce::AudioFormatWriter::Options{}
        .withSampleRate(sampleRate)
        .withNumChannels(buffer.getNumChannels())
        .withBitsPerSample(24)
        .withQualityOptionIndex(0);
    std::unique_ptr<juce::AudioFormatWriter> writer(
        wavFormat.createWriterFor(writerStream, writerOptions));

    if (!writer || !writer->writeFromAudioSampleBuffer(buffer, 0, buffer.getNumSamples()))
        return false;

    writer->flush();
    const auto data = wavBytesRaw->getMemoryBlock();
    outBase64 = data.toBase64Encoding();
    writer.reset();
    return outBase64.isNotEmpty();
}

bool decodeWavBase64ToBuffer(const juce::String& base64Data,
                             juce::AudioBuffer<float>& bufferOut,
                             double& sampleRateOut)
{
    bufferOut.setSize(0, 0);
    sampleRateOut = 0.0;

    if (base64Data.isEmpty() || base64Data.length() > kMaxEmbeddedFlipBase64Chars)
        return false;

    juce::MemoryBlock wavBytes;
    if (!wavBytes.fromBase64Encoding(base64Data) || wavBytes.getSize() == 0 || wavBytes.getSize() > kMaxEmbeddedFlipWavBytes)
        return false;

    juce::WavAudioFormat wavFormat;
    std::unique_ptr<juce::AudioFormatReader> reader(
        wavFormat.createReaderFor(new juce::MemoryInputStream(wavBytes.getData(), wavBytes.getSize(), false), true));
    if (!reader)
        return false;

    const int64_t totalSamples64 = reader->lengthInSamples;
    if (totalSamples64 <= 0 || totalSamples64 > static_cast<int64_t>(std::numeric_limits<int>::max()))
        return false;

    const int totalSamples = static_cast<int>(totalSamples64);
    const int channelCount = juce::jlimit(1, 2, static_cast<int>(reader->numChannels));
    bufferOut.setSize(channelCount, totalSamples, false, false, true);
    if (!reader->read(&bufferOut, 0, totalSamples, 0, true, true))
    {
        bufferOut.setSize(0, 0);
        return false;
    }

    sampleRateOut = reader->sampleRate;
    return true;
}

std::unique_ptr<juce::XmlElement> loadGlobalSettingsXml()
{
    auto settingsFile = getGlobalSettingsFile();
    if (settingsFile.existsAsFile())
    {
        if (auto xml = juce::XmlDocument::parse(settingsFile))
            return xml;
    }

    auto legacySettingsFile = getLegacyGlobalSettingsFile();
    if (legacySettingsFile.existsAsFile())
    {
        if (auto xml = juce::XmlDocument::parse(legacySettingsFile))
            return xml;
    }

    juce::PropertiesFile legacyProps(getLegacySettingsOptions());
    if (legacyProps.isValidFile())
        return legacyProps.getXmlValue(kGlobalSettingsKey);

    return nullptr;
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

void saveGlobalSettingsXml(const juce::XmlElement& xml)
{
    auto settingsFile = getGlobalSettingsFile();
    auto settingsDir = settingsFile.getParentDirectory();
    if (!settingsDir.exists())
        settingsDir.createDirectory();
    xml.writeTo(settingsFile);

    auto legacyFile = getLegacyGlobalSettingsFile();
    auto legacyDir = legacyFile.getParentDirectory();
    if (!legacyDir.exists())
        legacyDir.createDirectory();
    if (legacyFile != settingsFile)
        xml.writeTo(legacyFile);

    juce::PropertiesFile legacyProps(getLegacySettingsOptions());
    if (legacyProps.isValidFile())
    {
        legacyProps.setValue(kGlobalSettingsKey, &xml);
        legacyProps.saveIfNeeded();
    }
}

constexpr juce::int64 kPersistentGlobalControlsSaveDebounceMs = 350;
constexpr uint32_t kSceneAutosaveDebounceMs = 180;

constexpr std::array<const char*, 21> kPersistentGlobalControlParameterIds {
    "masterVolume",
    "limiterThreshold",
    "limiterEnabled",
    "quantize",
    "innerLoopLength",
    "quality",
    "pitchSmoothing",
    "inputMonitor",
    "crossfadeLength",
    "triggerFadeIn",
    "outputRouting",
    "pitchControlMode",
    "flipTempoMatchMode",
    "stretchBackend",
    "continuousTraversal",
    "sceneMode",
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

bool isSceneAutosaveParameterId(const juce::String& parameterID)
{
    return parameterID.startsWith("stripVolume")
        || parameterID.startsWith("stripTrimDb")
        || parameterID.startsWith("stripPan")
        || parameterID.startsWith("stripSpeed")
        || parameterID.startsWith("stripPitch")
        || parameterID.startsWith("stripSliceLength")
        || parameterID.startsWith("stripPitchControlMode")
        || parameterID.startsWith("stripTempoMatchMode")
        || parameterID.startsWith("stripFilterEnabled")
        || parameterID.startsWith("stripFilterFrequency")
        || parameterID.startsWith("stripFilterResonance")
        || parameterID.startsWith("stripFilterMorph")
        || parameterID.startsWith("stripFilterAlgorithm")
        || parameterID.startsWith("stripDuckEnabled")
        || parameterID.startsWith("stripDuckSource")
        || parameterID.startsWith("stripDuckThreshold")
        || parameterID.startsWith("stripDuckRatio")
        || parameterID.startsWith("stripDuckAttack")
        || parameterID.startsWith("stripDuckRelease")
        || parameterID.startsWith("stripDuckGainComp")
        || parameterID.startsWith("stripDuckFollowMaster")
        || parameterID.startsWith("stripDelayMix")
        || parameterID.startsWith("stripDelayTime")
        || parameterID.startsWith("stripDelaySync")
        || parameterID.startsWith("stripDelayFeedback")
        || parameterID.startsWith("stripDelayLowCut")
        || parameterID.startsWith("stripDelayHighCut")
        || parameterID.startsWith("stripDelayMode");
}

struct ScopedSceneAutosaveSuppression
{
    explicit ScopedSceneAutosaveSuppression(MlrVSTAudioProcessor& p) : processor(p)
    {
        processor.beginSceneAutosaveSuppression();
    }

    ~ScopedSceneAutosaveSuppression()
    {
        processor.endSceneAutosaveSuppression();
    }

    MlrVSTAudioProcessor& processor;
};

bool isPersistentGlobalControlParameterId(const juce::String& parameterID)
{
    for (const auto* id : kPersistentGlobalControlParameterIds)
    {
        if (parameterID == id)
            return true;
    }
    return false;
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
    switch (juce::jlimit(0, 5, index))
    {
        case 0: return EnhancedAudioStrip::FilterAlgorithm::Tpt12;
        case 1: return EnhancedAudioStrip::FilterAlgorithm::Tpt24;
        case 2: return EnhancedAudioStrip::FilterAlgorithm::Ladder12;
        case 3: return EnhancedAudioStrip::FilterAlgorithm::Ladder24;
        case 4: return EnhancedAudioStrip::FilterAlgorithm::MoogStilson;
        case 5:
        default: return EnhancedAudioStrip::FilterAlgorithm::MoogHuov;
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
        setAllLEDs(0);

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

class MlrVSTAudioProcessor::PresetSaveJob final : public juce::ThreadPoolJob
{
public:
    PresetSaveJob(MlrVSTAudioProcessor& ownerIn, PresetSaveRequest requestIn)
        : juce::ThreadPoolJob("mlrVSTPresetSave_" + juce::String(requestIn.presetIndex + 1)),
          owner(ownerIn),
          request(std::move(requestIn))
    {
    }

    JobStatus runJob() override
    {
        if (shouldExit())
        {
            owner.pushPresetSaveResult({ request.presetIndex, false });
            return jobHasFinished;
        }

        const bool success = owner.runPresetSaveRequest(request);
        owner.pushPresetSaveResult({ request.presetIndex, success });
        return jobHasFinished;
    }

private:
    MlrVSTAudioProcessor& owner;
    PresetSaveRequest request;
};

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

class MlrVSTAudioProcessor::LoopStripLoadJob final : public juce::ThreadPoolJob
{
public:
    LoopStripLoadJob(MlrVSTAudioProcessor& ownerIn,
                     int stripIndexIn,
                     int requestIdIn,
                     juce::File sourceFileIn,
                     double hostTempoSnapshotIn,
                     TimeStretchBackend tempoMatchBackendIn)
        : juce::ThreadPoolJob("mlrVSTLoopLoad_" + juce::String(stripIndexIn + 1)),
          owner(ownerIn),
          stripIndex(stripIndexIn),
          requestId(requestIdIn),
          sourceFile(std::move(sourceFileIn)),
          hostTempoSnapshot(hostTempoSnapshotIn),
          tempoMatchBackend(tempoMatchBackendIn)
    {
    }

    JobStatus runJob() override
    {
        auto isCurrentRequest = [this]() -> bool
        {
            if (stripIndex < 0 || stripIndex >= MlrVSTAudioProcessor::MaxStrips)
                return false;
            return owner.loopStripLoadRequestIds[static_cast<size_t>(stripIndex)].load(std::memory_order_acquire) == requestId;
        };

        if (shouldExit() || !isCurrentRequest())
            return jobHasFinished;

        MlrVSTAudioProcessor::LoopStripLoadResult result;
        result.stripIndex = stripIndex;
        result.requestId = requestId;
        result.sourceFile = sourceFile;

        owner.updateLoopStripLoadProgress(stripIndex, requestId, 0.08f, "Decoding " + sourceFile.getFileName() + "...");

        juce::String errorMessage;
        if (!decodeLoopStripFileToStereoBuffer(sourceFile,
                                               result.decodedBuffer,
                                               result.sourceSampleRate,
                                               errorMessage))
        {
            result.errorMessage = errorMessage;
            owner.queueLoopStripLoadResult(std::move(result));
            return jobHasFinished;
        }

        if (shouldExit() || !isCurrentRequest())
            return jobHasFinished;

        owner.updateLoopStripLoadProgress(stripIndex, requestId, 0.42f, "Analyzing loop...");
        result.detectedBars = detectLoopStripRecordingBars(result.decodedBuffer,
                                                           result.sourceSampleRate,
                                                           hostTempoSnapshot);
        result.detectedBeatsForLoop = static_cast<float>(juce::jlimit(1, 8, result.detectedBars) * 4);

        if (tempoMatchBackend == TimeStretchBackend::Bungee)
        {
            owner.updateLoopStripLoadProgress(stripIndex,
                                              requestId,
                                              0.72f,
                                              juce::String("Stretching with ")
                                                  + timeStretchBackendName(tempoMatchBackend)
                                                  + "...");
            const int targetFrames = computeLoopTempoMatchTargetFrames(result.sourceSampleRate,
                                                                       result.detectedBeatsForLoop,
                                                                       hostTempoSnapshot);
            if (targetFrames > 0 && targetFrames != result.decodedBuffer.getNumSamples())
            {
                juce::AudioBuffer<float> preparedBuffer;
                if (renderTimeStretchedBuffer(result.decodedBuffer,
                                              result.sourceSampleRate,
                                              targetFrames,
                                              0.0f,
                                              tempoMatchBackend,
                                              preparedBuffer))
                {
                    result.preparedTempoMatchBuffer = std::move(preparedBuffer);
                    result.preparedTempoMatchHostTempo = hostTempoSnapshot;
                    result.preparedTempoMatchBackend = tempoMatchBackend;
                }
            }
        }

        if (shouldExit() || !isCurrentRequest())
            return jobHasFinished;

        owner.updateLoopStripLoadProgress(stripIndex, requestId, 0.94f, "Snapping to host...");
        result.success = result.decodedBuffer.getNumSamples() > 0 && result.sourceSampleRate > 0.0;
        owner.queueLoopStripLoadResult(std::move(result));
        return jobHasFinished;
    }

private:
    MlrVSTAudioProcessor& owner;
    int stripIndex = -1;
    int requestId = 0;
    juce::File sourceFile;
    double hostTempoSnapshot = 120.0;
    TimeStretchBackend tempoMatchBackend = TimeStretchBackend::Resample;
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

class MlrVSTAudioProcessor::FlipLegacyLoopRenderJob final : public juce::ThreadPoolJob
{
public:
    FlipLegacyLoopRenderJob(MlrVSTAudioProcessor& ownerIn, FlipLegacyLoopRenderRequest requestIn)
        : juce::ThreadPoolJob("mlrVSTFlipWarp_" + juce::String(requestIn.cacheIndex + 1)),
          owner(ownerIn),
          request(std::move(requestIn))
    {
    }

    JobStatus runJob() override
    {
        FlipLegacyLoopRenderResult result;
        result.cacheIndex = request.cacheIndex;
        result.renderGeneration = request.renderGeneration;
        owner.assignFlipLegacyLoopRenderKey(result.cacheEntry,
                                            request.syncInfo,
                                            request.hostTempo,
                                            request.backend,
                                            request.visibleBankBeats);
        result.cacheEntry.renderGeneration = request.renderGeneration;
        result.cacheEntry.valid = false;
        result.cacheEntry.renderInFlight = false;
        result.cacheEntry.stripApplied = false;

        if (shouldExit() || request.syncInfo.loadedSample == nullptr)
        {
            owner.pushFlipLegacyLoopRenderResult(std::move(result));
            return jobHasFinished;
        }

        juce::AudioBuffer<float> bankBuffer;
        if (!buildFlipLegacyLoopBankBuffer(request.syncInfo,
                                           request.hostTempo,
                                           request.backend,
                                           request.visibleBankBeats,
                                           bankBuffer))
        {
            owner.pushFlipLegacyLoopRenderResult(std::move(result));
            return jobHasFinished;
        }

        const auto transientSliceCache = buildFlipLegacyLoopTransientSliceCache(request.syncInfo,
                                                                                request.bankStartSample,
                                                                                request.bankEndSample);
        std::array<float, 128> rmsMap {};
        std::array<int, 128> zeroCrossMap {};
        buildFlipLegacyLoopAnalysisMaps(bankBuffer, rmsMap, zeroCrossMap);

        result.cacheEntry.cachedBankBuffer = std::move(bankBuffer);
        result.cacheEntry.cachedTransientSliceStarts = transientSliceCache;
        result.cacheEntry.cachedRmsMap = rmsMap;
        result.cacheEntry.cachedZeroCrossMap = zeroCrossMap;
        result.cacheEntry.cachedSourceLengthSamples = static_cast<int>(
            juce::jmax<int64_t>(1, request.bankEndSample - request.bankStartSample));
        result.cacheEntry.cachedSampleRate = request.syncInfo.loadedSample->sourceSampleRate;
        result.cacheEntry.renderValid = result.cacheEntry.cachedBankBuffer.getNumSamples() > 0;
        owner.pushFlipLegacyLoopRenderResult(std::move(result));
        return jobHasFinished;
    }

private:
    MlrVSTAudioProcessor& owner;
    FlipLegacyLoopRenderRequest request;
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
    loadPersistentDefaultPaths();
    loadPersistentControlPages();
    loadPersistentGlobalControls();
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
            stopSampleModeStrip(stripIndex, immediateStop);
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

bool MlrVSTAudioProcessor::applyFlipPresetStateXmlToSceneContext(SceneRenderContext& context,
                                                                 int stripIndex,
                                                                 const juce::XmlElement* flipStateXml)
{
    if (flipStateXml == nullptr
        || context.engine == nullptr
        || stripIndex < 0
        || stripIndex >= MaxStrips)
    {
        return false;
    }

    auto* strip = context.engine->getStrip(stripIndex);
    if (strip == nullptr)
        return false;

    strip->setPlayMode(EnhancedAudioStrip::PlayMode::Sample);
    auto& sampleEngine = context.sampleModeEngines[static_cast<size_t>(stripIndex)];
    if (sampleEngine == nullptr)
    {
        sampleEngine = std::make_unique<SampleModeEngine>();
        if (currentSampleRate > 0.0)
            sampleEngine->prepare(currentSampleRate, juce::jmax(1, getBlockSize()));
    }

    const auto persistentState = SampleModePersistentState::fromXml(*flipStateXml);
    const juce::File sampleFile(persistentState.samplePath);
    if (persistentState.samplePath.isNotEmpty())
        context.currentStripFiles[static_cast<size_t>(stripIndex)] = sampleFile;

    bool loaded = false;
    const auto embeddedSample = flipStateXml->getStringAttribute(kEmbeddedFlipSampleAttr);
    if (embeddedSample.isNotEmpty())
    {
        juce::AudioBuffer<float> buffer;
        double sampleRate = 0.0;
        if (decodeWavBase64ToBuffer(embeddedSample, buffer, sampleRate))
            loaded = sampleEngine->loadSampleFromBuffer(buffer, sampleRate, {}, "Embedded Flip Sample");
    }
    else if (safeFileExistsAsFile(sampleFile))
    {
        loaded = loadSampleFileIntoSampleModeEngine(*sampleEngine, sampleFile);
    }

    if (!loaded && embeddedSample.isEmpty() && persistentState.samplePath.isNotEmpty())
        return false;

    sampleEngine->applyPersistentState(persistentState);
    return true;
}

bool MlrVSTAudioProcessor::buildSceneRenderContext(SceneRenderContext& context,
                                                   int mainPresetIndex,
                                                   int sceneSlot,
                                                   double hostPpqSnapshot,
                                                   double hostTempoSnapshot,
                                                   int64_t hostGlobalSampleSnapshot,
                                                   SceneChainTransitionType transitionType,
                                                   bool sequenceDriven,
                                                   int sequenceStepIndex)
{
    context = {};
    context.mainPresetIndex = juce::jlimit(0, MaxPresetSlots - 1, mainPresetIndex);
    context.sceneSlot = juce::jlimit(0, SceneSlots - 1, sceneSlot);
    context.sequenceDriven = sequenceDriven;
    context.sequenceStepIndex = sequenceStepIndex;
    context.sceneStartPpqValid = std::isfinite(hostPpqSnapshot);
    context.sceneStartPpq = context.sceneStartPpqValid ? hostPpqSnapshot : 0.0;
    context.sceneTempo = (std::isfinite(hostTempoSnapshot) && hostTempoSnapshot > 0.0) ? hostTempoSnapshot : 120.0;
    context.sceneStartSample = hostGlobalSampleSnapshot;
    context.transitionType = transitionType;

    context.engine = std::make_unique<ModernAudioEngine>();
    configureAudioEngineCallbacks(*context.engine);
    if (currentSampleRate > 0.0)
        context.engine->prepareToPlay(currentSampleRate, juce::jmax(1, getBlockSize()));
    context.engine->setPatternRecorderIgnoreGroups(false);

    juce::AudioProcessorValueTreeState shadowParameters(*this,
                                                        nullptr,
                                                        juce::Identifier("MlrVSTScenePreload"),
                                                        createParameterLayout());
    shadowParameters.replaceState(parameters.copyState());

    const int storageIndex = getSceneStoragePresetIndex(context.mainPresetIndex, context.sceneSlot);
    if (!PresetStore::presetExists(storageIndex))
        return false;

    const bool loadSucceeded = PresetStore::loadPreset(
        storageIndex,
        MaxStrips,
        context.engine.get(),
        shadowParameters,
        [&context](int stripIndex, const juce::File& sampleFile)
        {
            return context.engine != nullptr && context.engine->loadSampleToStrip(stripIndex, sampleFile);
        },
        [&context](int stripIndex, const juce::File& sampleFile)
        {
            if (stripIndex >= 0 && stripIndex < MaxStrips)
                context.currentStripFiles[static_cast<size_t>(stripIndex)] = sampleFile;
        },
        [&context](int stripIndex,
                   const juce::File& loopDir,
                   const juce::File& stepDir,
                   const juce::File& flipDir)
        {
            if (stripIndex < 0 || stripIndex >= MaxStrips)
                return;

            const auto idx = static_cast<size_t>(stripIndex);
            context.recentLoopDirectories[idx] = loopDir;
            context.recentStepDirectories[idx] = stepDir;
            context.recentFlipDirectories[idx] = flipDir;
        },
        [this, &context](int stripIndex, const juce::XmlElement* flipStateXml)
        {
            applyFlipPresetStateXmlToSceneContext(context, stripIndex, flipStateXml);
        },
        [&context](int stripIndex, const juce::XmlElement* loopPitchStateXml)
        {
            if (stripIndex < 0 || stripIndex >= MaxStrips || loopPitchStateXml == nullptr)
                return;

            context.loopPitchStateXml[static_cast<size_t>(stripIndex)] =
                std::make_unique<juce::XmlElement>(*loopPitchStateXml);
        },
        [&context](const juce::XmlElement& presetXml)
        {
            if (auto* sceneXml = presetXml.getChildByName("SceneChainState"))
                context.sceneTimingStateXml = std::make_unique<juce::XmlElement>(*sceneXml);
        },
        [&context](const juce::MemoryBlock& scenePerformanceData)
        {
            context.scenePerformanceStateData = scenePerformanceData;
        },
        hostPpqSnapshot,
        hostTempoSnapshot,
        true,
        hostGlobalSampleSnapshot);

    if (!loadSucceeded || context.engine == nullptr)
    {
        context = {};
        return false;
    }

    context.parameterState = shadowParameters.copyState();
    context.engine->seedTransportState(hostGlobalSampleSnapshot >= 0 ? hostGlobalSampleSnapshot : 0,
                                       hostPpqSnapshot);
    return true;
}

void MlrVSTAudioProcessor::requestScenePreload(int mainPresetIndex,
                                               int sceneSlot,
                                               bool sequenceDriven,
                                               int sequenceStepIndex,
                                               double targetPpq,
                                               double targetTempo,
                                               int64_t targetSample,
                                               SceneChainTransitionType transitionType)
{
    pendingScenePreloadMainPreset.store(mainPresetIndex, std::memory_order_release);
    pendingScenePreloadSceneSlot.store(sceneSlot, std::memory_order_release);
    pendingScenePreloadSequenceDriven.store(sequenceDriven ? 1 : 0, std::memory_order_release);
    pendingScenePreloadSequenceStep.store(sequenceStepIndex, std::memory_order_release);
    pendingScenePreloadTargetPpq.store(targetPpq, std::memory_order_release);
    pendingScenePreloadTargetTempo.store(targetTempo, std::memory_order_release);
    pendingScenePreloadTargetSample.store(targetSample, std::memory_order_release);
    pendingScenePreloadTransitionType.store(static_cast<int>(transitionType), std::memory_order_release);
    pendingScenePreloadDirty.store(1, std::memory_order_release);
}

void MlrVSTAudioProcessor::servicePendingScenePreloadRequest()
{
    if (pendingScenePreloadDirty.load(std::memory_order_acquire) == 0)
        return;

    const int mainPresetIndex = pendingScenePreloadMainPreset.load(std::memory_order_acquire);
    const int sceneSlot = pendingScenePreloadSceneSlot.load(std::memory_order_acquire);
    const bool sequenceDriven = pendingScenePreloadSequenceDriven.load(std::memory_order_acquire) != 0;
    const int sequenceStepIndex = pendingScenePreloadSequenceStep.load(std::memory_order_acquire);
    const double targetPpq = pendingScenePreloadTargetPpq.load(std::memory_order_acquire);
    const double targetTempo = pendingScenePreloadTargetTempo.load(std::memory_order_acquire);
    const int64_t targetSample = pendingScenePreloadTargetSample.load(std::memory_order_acquire);
    const auto transitionType = static_cast<SceneChainTransitionType>(juce::jlimit(
        0,
        static_cast<int>(SceneChainTransitionType::Return),
        pendingScenePreloadTransitionType.load(std::memory_order_acquire)));

    auto matchesPreparedContext = [&](const std::unique_ptr<SceneRenderContext>& context) -> bool
    {
        const bool sampleMatches = context != nullptr
            && ((context->sceneStartSample < 0 && targetSample < 0)
                || (context->sceneStartSample >= 0
                    && targetSample >= 0
                    && (context->sceneStartSample - targetSample) >= -64
                    && (context->sceneStartSample - targetSample) <= 64));
        return context != nullptr
            && context->mainPresetIndex == mainPresetIndex
            && context->sceneSlot == sceneSlot
            && context->sequenceDriven == sequenceDriven
            && context->sequenceStepIndex == sequenceStepIndex
            && context->transitionType == transitionType
            && sampleMatches
            && ((!std::isfinite(targetPpq) && !context->sceneStartPpqValid)
                || (context->sceneStartPpqValid && std::abs(context->sceneStartPpq - targetPpq) <= 1.0e-9));
    };

    if (matchesPreparedContext(preparedSceneRenderContext))
    {
        pendingScenePreloadDirty.store(0, std::memory_order_release);
        return;
    }

    if (mainPresetIndex < 0 || sceneSlot < 0 || currentSampleRate <= 0.0)
        return;

    auto preloaded = std::make_unique<SceneRenderContext>();
    if (!buildSceneRenderContext(*preloaded,
                                 mainPresetIndex,
                                 sceneSlot,
                                 targetPpq,
                                 targetTempo,
                                 targetSample,
                                 transitionType,
                                 sequenceDriven,
                                 sequenceStepIndex))
    {
        preparedSceneRenderContext.reset();
        return;
    }

    preparedSceneRenderContext = std::move(preloaded);
    pendingScenePreloadDirty.store(0, std::memory_order_release);
}

void MlrVSTAudioProcessor::queuePendingSceneParameterState(const juce::ValueTree& state)
{
    const juce::ScopedLock lock(pendingSceneParameterStateLock);
    pendingSceneParameterState = state.createCopy();
    pendingSceneParameterStateDirty.store(pendingSceneParameterState.isValid() ? 1 : 0,
                                          std::memory_order_release);
}

void MlrVSTAudioProcessor::applyPendingSceneParameterState()
{
    if (pendingSceneParameterStateDirty.load(std::memory_order_acquire) == 0)
        return;

    juce::ValueTree stateToApply;
    {
        const juce::ScopedLock lock(pendingSceneParameterStateLock);
        stateToApply = pendingSceneParameterState;
        pendingSceneParameterState = {};
        pendingSceneParameterStateDirty.store(0, std::memory_order_release);
    }

    if (stateToApply.isValid())
        parameters.replaceState(stateToApply);

    suppressOwnedStripParameterSync.store(0, std::memory_order_release);
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

void MlrVSTAudioProcessor::parameterChanged(const juce::String& parameterID, float /*newValue*/)
{
    if (parameterID == "limiterThreshold")
    {
        if (auto* param = parameters.getParameter("limiterThreshold"))
        {
            const float currentValue = param->convertFrom0to1(param->getValue());
            if (std::abs(currentValue) > 1.0e-4f)
            {
                suppressPersistentGlobalControlsSave.store(1, std::memory_order_release);
                param->setValueNotifyingHost(param->convertTo0to1(0.0f));
                suppressPersistentGlobalControlsSave.store(0, std::memory_order_release);
            }
        }
    }

    if (isSceneAutosaveParameterId(parameterID))
        queueActiveSceneAutosave();

    if (!isPersistentGlobalControlParameterId(parameterID))
        return;
    persistentGlobalUserTouched.store(1, std::memory_order_release);
    queuePersistentGlobalControlsSave();
}

void MlrVSTAudioProcessor::markPersistentGlobalUserChange()
{
    persistentGlobalUserTouched.store(1, std::memory_order_release);
    persistentGlobalControlsReady.store(1, std::memory_order_release);
    queuePersistentGlobalControlsSave();
}

void MlrVSTAudioProcessor::queueActiveSceneAutosave()
{
    queueActiveSceneAutosave(0);
}

void MlrVSTAudioProcessor::queueActiveSceneAutosave(uint32_t additionalDelayMs)
{
    if (isSceneAutosaveSuppressed())
        return;

    const int mainPresetIndex = getActiveMainPresetIndexForScenes();
    const int safeSceneSlot = juce::jlimit(0, SceneSlots - 1, activeSceneSlot);
    const uint32_t nowMs = juce::Time::getMillisecondCounter();
    const uint32_t clampedDelayMs = juce::jmin<uint32_t>(additionalDelayMs, 8000u);
    const uint32_t readyMs = nowMs + kSceneAutosaveDebounceMs + clampedDelayMs;

    pendingActiveSceneAutosaveMainPreset.store(mainPresetIndex, std::memory_order_release);
    pendingActiveSceneAutosaveSlot.store(safeSceneSlot, std::memory_order_release);
    pendingActiveSceneAutosaveQueuedMs.store(readyMs, std::memory_order_release);
    pendingActiveSceneAutosaveDirty.store(1, std::memory_order_release);

    if (activeSceneMainPresetIndex == mainPresetIndex && activeSceneSlot == safeSceneSlot)
        activeSceneNeedsCaptureBeforeManualRecall = true;
}

void MlrVSTAudioProcessor::clearPendingActiveSceneAutosave()
{
    pendingActiveSceneAutosaveDirty.store(0, std::memory_order_release);
    pendingActiveSceneAutosaveMainPreset.store(-1, std::memory_order_release);
    pendingActiveSceneAutosaveSlot.store(-1, std::memory_order_release);
    pendingActiveSceneAutosaveQueuedMs.store(0, std::memory_order_release);
}

bool MlrVSTAudioProcessor::canPersistActiveSceneSnapshotSafely() const
{
    if (audioEngine == nullptr)
        return false;

    for (int stripIndex = 0; stripIndex < MaxStrips; ++stripIndex)
    {
        const auto* strip = audioEngine->getStrip(stripIndex);
        if (strip == nullptr)
            continue;

        const auto playMode = strip->getPlayMode();
        if (!(strip->hasAudio() || playMode == EnhancedAudioStrip::PlayMode::Sample))
            continue;

        const auto& sourceFile = currentStripFiles[static_cast<size_t>(stripIndex)];
        if (isValidSceneStoredSamplePath(sourceFile))
            continue;

        const auto* audioBuffer = strip->getAudioBuffer();
        if (audioBuffer != nullptr && canEmbedSceneAudioBuffer(*audioBuffer))
            continue;

        return false;
    }

    return true;
}

bool MlrVSTAudioProcessor::flushPendingActiveSceneAutosaveIfCurrent()
{
    if (pendingActiveSceneAutosaveDirty.load(std::memory_order_acquire) == 0)
        return true;
    if (isSceneAutosaveSuppressed())
        return false;

    const int queuedMainPreset = pendingActiveSceneAutosaveMainPreset.load(std::memory_order_acquire);
    const int queuedSceneSlot = pendingActiveSceneAutosaveSlot.load(std::memory_order_acquire);
    const int currentMainPreset = juce::jlimit(0, MaxPresetSlots - 1, activeSceneMainPresetIndex);
    const int currentSceneSlot = juce::jlimit(0, SceneSlots - 1, activeSceneSlot);
    if (queuedMainPreset != currentMainPreset || queuedSceneSlot != currentSceneSlot)
        return true;
    if (!canPersistActiveSceneSnapshotSafely())
        return false;

    return saveSceneForMainPreset(currentMainPreset, currentSceneSlot);
}

void MlrVSTAudioProcessor::processPendingSceneAutosave()
{
    if (pendingActiveSceneAutosaveDirty.load(std::memory_order_acquire) == 0)
        return;
    if (isSceneAutosaveSuppressed() || activeSceneTransitionContext != nullptr || scenePerformanceRecorder.isRecording())
        return;

    const int queuedMainPreset = pendingActiveSceneAutosaveMainPreset.load(std::memory_order_acquire);
    const int queuedSceneSlot = pendingActiveSceneAutosaveSlot.load(std::memory_order_acquire);
    const int currentMainPreset = juce::jlimit(0, MaxPresetSlots - 1, activeSceneMainPresetIndex);
    const int currentSceneSlot = juce::jlimit(0, SceneSlots - 1, activeSceneSlot);
    if (queuedMainPreset != currentMainPreset || queuedSceneSlot != currentSceneSlot)
        return;

    const uint32_t queuedMs = pendingActiveSceneAutosaveQueuedMs.load(std::memory_order_acquire);
    const uint32_t nowMs = juce::Time::getMillisecondCounter();
    if (queuedMs != 0 && static_cast<int32_t>(nowMs - queuedMs) < 0)
        return;

    flushPendingActiveSceneAutosaveIfCurrent();
}

void MlrVSTAudioProcessor::beginSceneAutosaveSuppression()
{
    sceneAutosaveSuppressionDepth.fetch_add(1, std::memory_order_acq_rel);
}

void MlrVSTAudioProcessor::endSceneAutosaveSuppression()
{
    sceneAutosaveSuppressionDepth.fetch_sub(1, std::memory_order_acq_rel);
}

bool MlrVSTAudioProcessor::isSceneAutosaveSuppressed() const
{
    return sceneAutosaveSuppressionDepth.load(std::memory_order_acquire) > 0;
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

void MlrVSTAudioProcessor::queuePersistentGlobalControlsSave()
{
    if (suppressPersistentGlobalControlsSave.load(std::memory_order_acquire) != 0)
        return;
    if (pendingPersistentGlobalControlsRestore.load(std::memory_order_acquire) != 0)
    {
        if (persistentGlobalUserTouched.load(std::memory_order_acquire) == 0)
            return;
        pendingPersistentGlobalControlsRestore.store(0, std::memory_order_release);
        pendingPersistentGlobalControlsRestoreRemaining = 0;
    }
    if (persistentGlobalControlsReady.load(std::memory_order_acquire) == 0)
        return;

    persistentGlobalControlsDirty.store(1, std::memory_order_release);
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
        savePersistentControlPages();
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

juce::String MlrVSTAudioProcessor::getControlModeName(ControlMode mode)
{
    switch (mode)
    {
        case ControlMode::Speed: return "Speed";
        case ControlMode::Pitch: return "Pitch";
        case ControlMode::Pan: return "Pan";
        case ControlMode::Volume: return "Volume";
        case ControlMode::GrainSize: return "Grain Size";
        case ControlMode::Filter: return "Filter";
        case ControlMode::Delay: return "Delay";
        case ControlMode::Swing: return "Swing";
        case ControlMode::Gate: return "Gate";
        case ControlMode::FileBrowser: return "Browser";
        case ControlMode::GroupAssign: return "Group";
        case ControlMode::Modulation: return "Modulation";
        case ControlMode::Preset: return "Preset";
        case ControlMode::StepEdit: return "Step Edit";
        case ControlMode::Normal:
        default: return "Normal";
    }
}

int MlrVSTAudioProcessor::getMonomeGridWidth() const
{
    if (!monomeConnection.supportsGrid())
        return MaxGridWidth;

    const auto device = monomeConnection.getCurrentGridDevice();
    const int reportedWidth = (device.sizeX > 0) ? device.sizeX : MaxGridWidth;
    return juce::jlimit(1, MaxGridWidth, reportedWidth);
}

int MlrVSTAudioProcessor::getMonomeGridHeight() const
{
    if (!monomeConnection.supportsGrid())
        return 8;

    const auto device = monomeConnection.getCurrentGridDevice();
    const int reportedHeight = (device.sizeY > 0) ? device.sizeY : 8;
    return juce::jlimit(2, MaxGridHeight, reportedHeight);
}

int MlrVSTAudioProcessor::getMonomeControlRow() const
{
    return juce::jmax(1, getMonomeGridHeight() - 1);
}

int MlrVSTAudioProcessor::getMonomeActiveStripCount() const
{
    const int stripRows = juce::jmax(0, getMonomeControlRow() - 1);
    return juce::jlimit(0, MaxStrips, stripRows);
}

int MlrVSTAudioProcessor::getDefaultMacroMidiCc(int macroIndex)
{
    if (macroIndex < 0 || macroIndex >= MacroCount)
        return -1;

    return kAkaiMpkMiniMacroCcs[static_cast<size_t>(macroIndex)];
}

int MlrVSTAudioProcessor::getMacroMidiCc(int macroIndex) const
{
    if (macroIndex < 0 || macroIndex >= MacroCount)
        return -1;

    return macroMidiCcAssignments[static_cast<size_t>(macroIndex)].load(std::memory_order_acquire);
}

MlrVSTAudioProcessor::MacroTarget MlrVSTAudioProcessor::getDefaultMacroTarget(int macroIndex)
{
    return MacroTargetDispatcher::getDefaultMacroTarget(macroIndex);
}

MlrVSTAudioProcessor::MacroTarget MlrVSTAudioProcessor::getMacroTarget(int macroIndex) const
{
    if (macroIndex < 0 || macroIndex >= MacroCount)
        return MacroTarget::None;

    return sanitizeMacroTarget(macroTargetAssignments[static_cast<size_t>(macroIndex)].load(std::memory_order_acquire));
}

void MlrVSTAudioProcessor::setMacroTarget(int macroIndex, MacroTarget target)
{
    if (macroIndex < 0 || macroIndex >= MacroCount)
        return;

    const auto previousTarget = getMacroTarget(macroIndex);
    if (previousTarget == MacroTarget::Retrigger && audioEngine != nullptr)
        setGlobalSceneStutterAmount(0.0f);

    macroTargetAssignments[static_cast<size_t>(macroIndex)].store(static_cast<int>(sanitizeMacroTarget(static_cast<int>(target))),
                                                                  std::memory_order_release);
    markPersistentGlobalUserChange();
}

float MlrVSTAudioProcessor::getDefaultMacroNormalizedValue(MacroTarget target)
{
    return MacroTargetDispatcher::getDefaultMacroNormalizedValue(target);
}

bool MlrVSTAudioProcessor::macroTargetWritesToSceneLane(MacroTarget target)
{
    switch (sanitizeMacroPerformanceTarget(target))
    {
        case MacroTarget::Speed:
        case MacroTarget::Pitch:
        case MacroTarget::Pan:
        case MacroTarget::Volume:
        case MacroTarget::Cutoff:
        case MacroTarget::Resonance:
        case MacroTarget::FilterMorph:
        case MacroTarget::SliceLength:
        case MacroTarget::Retrigger:
        case MacroTarget::Scratch:
        case MacroTarget::DelayMix:
        case MacroTarget::DelayTime:
        case MacroTarget::DelayFeedback:
        case MacroTarget::DelayLowCut:
        case MacroTarget::DelayHighCut:
        case MacroTarget::GrainSize:
        case MacroTarget::GrainDensity:
        case MacroTarget::GrainPitch:
        case MacroTarget::GrainPitchJitter:
        case MacroTarget::GrainSpread:
        case MacroTarget::GrainJitter:
        case MacroTarget::GrainPositionJitter:
        case MacroTarget::GrainRandom:
        case MacroTarget::GrainArp:
        case MacroTarget::GrainCloud:
        case MacroTarget::GrainEmitter:
        case MacroTarget::GrainEnvelope:
        case MacroTarget::GrainShape:
            return true;
        case MacroTarget::FilterEnable:
        case MacroTarget::Rearrange:
        case MacroTarget::None:
        default:
            return false;
    }
}

void MlrVSTAudioProcessor::beginMacroMidiLearn(int macroIndex)
{
    if (macroIndex < 0 || macroIndex >= MacroCount)
        return;

    macroMidiLearnIndex.store(macroIndex, std::memory_order_release);
}

void MlrVSTAudioProcessor::cancelMacroMidiLearn()
{
    macroMidiLearnIndex.store(-1, std::memory_order_release);
}

void MlrVSTAudioProcessor::resetMacroMidiCcToDefault(int macroIndex)
{
    if (macroIndex < 0 || macroIndex >= MacroCount)
        return;

    macroMidiCcAssignments[static_cast<size_t>(macroIndex)].store(getDefaultMacroMidiCc(macroIndex),
                                                                  std::memory_order_release);
    cancelMacroMidiLearn();
    markPersistentGlobalUserChange();
}

int MlrVSTAudioProcessor::getMacroTargetStripIndex() const
{
    const int activeStripCount = getMonomeActiveStripCount();
    const int maxStripIndex = (activeStripCount > 0) ? (activeStripCount - 1) : (MaxStrips - 1);
    return juce::jlimit(0, juce::jmax(0, maxStripIndex), getLastMonomePressedStripRow());
}

namespace
{
float smoothSceneTransitionProgress(float progress)
{
    const float clamped = juce::jlimit(0.0f, 1.0f, progress);
    return clamped * clamped * (3.0f - (2.0f * clamped));
}

double sceneChainTransitionLeadBeatsRuntime(MlrVSTAudioProcessor::SceneChainTransitionType type,
                                            float lengthBeats)
{
    float safeLengthBeats = lengthBeats;
    if (!std::isfinite(safeLengthBeats))
        safeLengthBeats = MlrVSTAudioProcessor::DefaultSceneTransitionLengthBeats;
    safeLengthBeats = juce::jlimit(MlrVSTAudioProcessor::MinSceneTransitionLengthBeats,
                                   MlrVSTAudioProcessor::MaxSceneTransitionLengthBeats,
                                   safeLengthBeats);
    const double requestedLength = static_cast<double>(safeLengthBeats);
    const auto safeType = static_cast<MlrVSTAudioProcessor::SceneChainTransitionType>(juce::jlimit(
        0,
        static_cast<int>(MlrVSTAudioProcessor::SceneChainTransitionType::Return),
        static_cast<int>(type)));

    switch (safeType)
    {
        case MlrVSTAudioProcessor::SceneChainTransitionType::Fill:       return juce::jlimit(0.25, 8.0, requestedLength);
        case MlrVSTAudioProcessor::SceneChainTransitionType::Stutter:    return juce::jlimit(0.25, 6.0, requestedLength);
        case MlrVSTAudioProcessor::SceneChainTransitionType::FilterRise: return juce::jlimit(0.25, 8.0, requestedLength);
        case MlrVSTAudioProcessor::SceneChainTransitionType::Drop:       return juce::jlimit(0.25, 6.0, requestedLength);
        case MlrVSTAudioProcessor::SceneChainTransitionType::MuteTail:   return juce::jlimit(0.25, 4.0, requestedLength);
        case MlrVSTAudioProcessor::SceneChainTransitionType::Break:      return juce::jlimit(0.25, 8.0, requestedLength);
        case MlrVSTAudioProcessor::SceneChainTransitionType::None:
        case MlrVSTAudioProcessor::SceneChainTransitionType::Return:
        default:
            return 0.0;
    }
}

float sceneTransitionRhythmicPulse(double beatsSinceStart,
                                   double subdivisionBeats,
                                   float sharpness)
{
    if (!std::isfinite(beatsSinceStart) || !std::isfinite(subdivisionBeats) || subdivisionBeats <= 1.0e-6)
        return 0.0f;

    double cycle = std::fmod(beatsSinceStart / subdivisionBeats, 1.0);
    if (cycle < 0.0)
        cycle += 1.0;

    const float triangle = 1.0f - std::abs(static_cast<float>((cycle * 2.0) - 1.0));
    return std::pow(juce::jlimit(0.0f, 1.0f, triangle),
                    juce::jlimit(0.4f, 6.0f, sharpness));
}

MlrVSTAudioProcessor::SceneChainTransitionType sanitizeRuntimeSceneChainTransitionType(int rawType)
{
    return static_cast<MlrVSTAudioProcessor::SceneChainTransitionType>(juce::jlimit(
        0,
        static_cast<int>(MlrVSTAudioProcessor::SceneChainTransitionType::Return),
        rawType));
}

MlrVSTAudioProcessor::SceneChainTransitionOption sanitizeRuntimeSceneChainTransitionOption(int rawOption)
{
    return static_cast<MlrVSTAudioProcessor::SceneChainTransitionOption>(juce::jlimit(
        0,
        static_cast<int>(MlrVSTAudioProcessor::SceneChainTransitionOption::Gate),
        rawOption));
}

float sanitizeRuntimeSceneChainTransitionLengthBeats(float beats)
{
    if (!std::isfinite(beats))
        beats = MlrVSTAudioProcessor::DefaultSceneTransitionLengthBeats;
    return juce::jlimit(MlrVSTAudioProcessor::MinSceneTransitionLengthBeats,
                        MlrVSTAudioProcessor::MaxSceneTransitionLengthBeats,
                        beats);
}

float sanitizeRuntimeSceneChainTransitionAmount(float amount, float fallback)
{
    if (!std::isfinite(amount))
        amount = fallback;
    return juce::jlimit(0.0f, 1.0f, amount);
}

struct SceneChainTransitionRuntimeProfile
{
    float depthScale = 1.0f;
    float delayScale = 1.0f;
    float filterScale = 1.0f;
    float stutterScale = 1.0f;
    float gateScale = 1.0f;
};

SceneChainTransitionRuntimeProfile sceneChainTransitionRuntimeProfile(MlrVSTAudioProcessor::SceneChainTransitionOption option)
{
    switch (sanitizeRuntimeSceneChainTransitionOption(static_cast<int>(option)))
    {
        case MlrVSTAudioProcessor::SceneChainTransitionOption::Snap:    return { 0.62f, 0.42f, 0.68f, 0.82f, 0.88f };
        case MlrVSTAudioProcessor::SceneChainTransitionOption::Tight:   return { 0.78f, 0.58f, 0.82f, 0.88f, 0.92f };
        case MlrVSTAudioProcessor::SceneChainTransitionOption::Wide:    return { 1.10f, 0.96f, 1.08f, 0.96f, 0.92f };
        case MlrVSTAudioProcessor::SceneChainTransitionOption::Wash:    return { 1.30f, 1.48f, 1.18f, 0.76f, 0.70f };
        case MlrVSTAudioProcessor::SceneChainTransitionOption::Echo:    return { 1.04f, 1.86f, 0.90f, 0.72f, 0.76f };
        case MlrVSTAudioProcessor::SceneChainTransitionOption::Sweep:   return { 1.02f, 0.86f, 1.72f, 0.74f, 0.80f };
        case MlrVSTAudioProcessor::SceneChainTransitionOption::Gate:    return { 0.96f, 0.56f, 0.78f, 1.18f, 1.26f };
        case MlrVSTAudioProcessor::SceneChainTransitionOption::Default:
        default:
            return { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
    }
}

double sceneChainTransitionCrossfadeSeconds(MlrVSTAudioProcessor::SceneChainTransitionType type,
                                            float lengthBeats,
                                            float intensity,
                                            double tempo)
{
    const double safeTempo = (std::isfinite(tempo) && tempo > 0.0) ? tempo : 120.0;
    const double requestedLengthSeconds
        = (sceneChainTransitionLeadBeatsRuntime(type, lengthBeats) * 60.0) / safeTempo;
    const double intensityScale = 0.45 + (0.85 * static_cast<double>(sanitizeRuntimeSceneChainTransitionAmount(
        intensity,
        MlrVSTAudioProcessor::DefaultSceneTransitionIntensity)));
    const double baseSeconds = 0.05 + (requestedLengthSeconds * (0.38 + (0.34 * intensityScale)));

    switch (sanitizeRuntimeSceneChainTransitionType(static_cast<int>(type)))
    {
        case MlrVSTAudioProcessor::SceneChainTransitionType::Fill:
            return juce::jlimit(0.16, 2.80, baseSeconds * 1.08);
        case MlrVSTAudioProcessor::SceneChainTransitionType::Stutter:
            return juce::jlimit(0.12, 1.70, baseSeconds * 0.86);
        case MlrVSTAudioProcessor::SceneChainTransitionType::FilterRise:
            return juce::jlimit(0.18, 3.00, baseSeconds * 1.22);
        case MlrVSTAudioProcessor::SceneChainTransitionType::Drop:
            return juce::jlimit(0.14, 2.20, baseSeconds * 1.00);
        case MlrVSTAudioProcessor::SceneChainTransitionType::MuteTail:
            return juce::jlimit(0.12, 1.50, baseSeconds * 0.74);
        case MlrVSTAudioProcessor::SceneChainTransitionType::Break:
            return juce::jlimit(0.16, 2.40, baseSeconds * 1.04);
        case MlrVSTAudioProcessor::SceneChainTransitionType::None:
        case MlrVSTAudioProcessor::SceneChainTransitionType::Return:
        default:
            return juce::jlimit(0.09, 0.32, baseSeconds * 0.62);
    }
}
} // namespace

void MlrVSTAudioProcessor::updateEffectiveSceneStutterAmount()
{
    if (audioEngine == nullptr)
        return;

    const float baseAmount = juce::jlimit(0.0f, 1.0f, sceneGlobalStutterBaseAmount.load(std::memory_order_acquire));
    const float overlayAmount = juce::jlimit(0.0f, 1.0f, sceneTransitionStutterOverlayAmount.load(std::memory_order_acquire));
    const float effectiveAmount = juce::jmax(baseAmount, overlayAmount);
    for (int stripIndex = 0; stripIndex < MaxStrips; ++stripIndex)
        audioEngine->setMacroRetriggerAmount(stripIndex, effectiveAmount);
}

void MlrVSTAudioProcessor::setSceneTransitionStutterOverlayAmount(float amount01)
{
    const float clamped = juce::jlimit(0.0f, 1.0f, amount01);
    const float previous = sceneTransitionStutterOverlayAmount.load(std::memory_order_acquire);
    if (std::abs(previous - clamped) <= 1.0e-4f)
        return;

    sceneTransitionStutterOverlayAmount.store(clamped, std::memory_order_release);
    updateEffectiveSceneStutterAmount();
}

void MlrVSTAudioProcessor::setGlobalSceneStutterAmount(float amount01)
{
    const float clamped = juce::jlimit(0.0f, 1.0f, amount01);
    sceneGlobalStutterBaseAmount.store(clamped, std::memory_order_release);
    if (audioEngine == nullptr)
        return;

    updateEffectiveSceneStutterAmount();
}

float MlrVSTAudioProcessor::getGlobalSceneStutterAmount() const
{
    const float baseAmount = juce::jlimit(0.0f, 1.0f, sceneGlobalStutterBaseAmount.load(std::memory_order_acquire));
    const float overlayAmount = juce::jlimit(0.0f, 1.0f, sceneTransitionStutterOverlayAmount.load(std::memory_order_acquire));
    return juce::jmax(baseAmount, overlayAmount);
}

void MlrVSTAudioProcessor::clearSceneBoundaryTransitionState(bool clearBlendSuppression)
{
    sceneBoundaryTransitionType.store(static_cast<int>(SceneChainTransitionType::None), std::memory_order_release);
    sceneBoundaryTransitionOption.store(static_cast<int>(SceneChainTransitionOption::Default), std::memory_order_release);
    sceneBoundaryTransitionFromStep.store(-1, std::memory_order_release);
    sceneBoundaryTransitionToStep.store(-1, std::memory_order_release);
    sceneBoundaryTransitionStartPpq.store(-1.0, std::memory_order_release);
    sceneBoundaryTransitionTargetPpq.store(-1.0, std::memory_order_release);
    sceneBoundaryTransitionStartSample.store(-1, std::memory_order_release);
    sceneBoundaryTransitionTargetSample.store(-1, std::memory_order_release);
    sceneBoundaryTransitionTempo.store(120.0, std::memory_order_release);
    sceneBoundaryTransitionLengthBeats.store(DefaultSceneTransitionLengthBeats, std::memory_order_release);
    sceneBoundaryTransitionIntensity.store(DefaultSceneTransitionIntensity, std::memory_order_release);
    sceneBoundaryTransitionDelayAmount.store(DefaultSceneTransitionDelayAmount, std::memory_order_release);
    sceneBoundaryTransitionFilterAmount.store(DefaultSceneTransitionFilterAmount, std::memory_order_release);
    sceneBoundaryTransitionChopAmount.store(DefaultSceneTransitionChopAmount, std::memory_order_release);
    if (clearBlendSuppression)
        suppressSceneRecallBlendOnNextLoad.store(0, std::memory_order_release);
    setSceneTransitionStutterOverlayAmount(0.0f);
}

void MlrVSTAudioProcessor::armSceneBoundaryTransition(SceneChainTransitionType type,
                                                      SceneChainTransitionOption option,
                                                      float lengthBeats,
                                                      float intensity,
                                                      float delayAmount,
                                                      float filterAmount,
                                                      float chopAmount,
                                                      int fromStepIndex,
                                                      int toStepIndex,
                                                      double targetPpq,
                                                      double targetTempo,
                                                      double leadBeats,
                                                      int64_t targetSample)
{
    const auto safeType = sanitizeRuntimeSceneChainTransitionType(static_cast<int>(type));
    if (safeType == SceneChainTransitionType::None || safeType == SceneChainTransitionType::Return)
    {
        clearSceneBoundaryTransitionState();
        return;
    }

    const double safeLeadBeats = juce::jlimit(0.0, 16.0, leadBeats);
    const double safeTargetPpq = (std::isfinite(targetPpq) && targetPpq >= 0.0) ? targetPpq : -1.0;
    double startPpq = -1.0;
    if (safeTargetPpq >= 0.0)
        startPpq = juce::jmax(0.0, safeTargetPpq - safeLeadBeats);

    int64_t startSample = -1;
    if (targetSample >= 0 && currentSampleRate > 0.0 && std::isfinite(targetTempo) && targetTempo > 0.0)
    {
        const double leadSeconds = (safeLeadBeats * 60.0) / targetTempo;
        const int64_t leadSamples = static_cast<int64_t>(std::llround(leadSeconds * currentSampleRate));
        startSample = juce::jmax<int64_t>(0, targetSample - juce::jmax<int64_t>(0, leadSamples));
    }

    sceneBoundaryTransitionType.store(static_cast<int>(safeType), std::memory_order_release);
    sceneBoundaryTransitionOption.store(static_cast<int>(sanitizeRuntimeSceneChainTransitionOption(static_cast<int>(option))),
                                        std::memory_order_release);
    sceneBoundaryTransitionLengthBeats.store(sanitizeRuntimeSceneChainTransitionLengthBeats(lengthBeats),
                                             std::memory_order_release);
    sceneBoundaryTransitionIntensity.store(sanitizeRuntimeSceneChainTransitionAmount(
                                               intensity,
                                               DefaultSceneTransitionIntensity),
                                           std::memory_order_release);
    sceneBoundaryTransitionDelayAmount.store(sanitizeRuntimeSceneChainTransitionAmount(
                                                 delayAmount,
                                                 DefaultSceneTransitionDelayAmount),
                                             std::memory_order_release);
    sceneBoundaryTransitionFilterAmount.store(sanitizeRuntimeSceneChainTransitionAmount(
                                                  filterAmount,
                                                  DefaultSceneTransitionFilterAmount),
                                              std::memory_order_release);
    sceneBoundaryTransitionChopAmount.store(sanitizeRuntimeSceneChainTransitionAmount(
                                                chopAmount,
                                                DefaultSceneTransitionChopAmount),
                                            std::memory_order_release);
    sceneBoundaryTransitionFromStep.store(fromStepIndex, std::memory_order_release);
    sceneBoundaryTransitionToStep.store(toStepIndex, std::memory_order_release);
    sceneBoundaryTransitionStartPpq.store(startPpq, std::memory_order_release);
    sceneBoundaryTransitionTargetPpq.store(safeTargetPpq, std::memory_order_release);
    sceneBoundaryTransitionStartSample.store(startSample, std::memory_order_release);
    sceneBoundaryTransitionTargetSample.store(targetSample, std::memory_order_release);
    sceneBoundaryTransitionTempo.store((std::isfinite(targetTempo) && targetTempo > 0.0) ? targetTempo : 120.0,
                                       std::memory_order_release);
}

void MlrVSTAudioProcessor::armSceneChainReturnOverride(int sourceStepIndex, int triggerStepIndex)
{
    sceneChainReturnOverrideSourceStep.store(sourceStepIndex, std::memory_order_release);
    sceneChainReturnOverrideTriggerStep.store(triggerStepIndex, std::memory_order_release);
    sceneChainReturnOverrideActive.store((sourceStepIndex >= 0 && triggerStepIndex >= 0) ? 1 : 0,
                                         std::memory_order_release);
}

void MlrVSTAudioProcessor::clearSceneChainReturnOverride()
{
    sceneChainReturnOverrideActive.store(0, std::memory_order_release);
    sceneChainReturnOverrideSourceStep.store(-1, std::memory_order_release);
    sceneChainReturnOverrideTriggerStep.store(-1, std::memory_order_release);
}

bool MlrVSTAudioProcessor::consumeSceneChainReturnOverrideForStep(int currentStepIndex, int& outNextStepIndex)
{
    if (sceneChainReturnOverrideActive.load(std::memory_order_acquire) == 0)
        return false;

    const int triggerStepIndex = sceneChainReturnOverrideTriggerStep.load(std::memory_order_acquire);
    if (currentStepIndex != triggerStepIndex)
        return false;

    outNextStepIndex = sceneChainReturnOverrideSourceStep.load(std::memory_order_acquire);
    clearSceneChainReturnOverride();
    return outNextStepIndex >= 0;
}

void MlrVSTAudioProcessor::applySceneBoundaryTransitionOverlay(const juce::AudioPlayHead::PositionInfo& posInfo,
                                                               int /*numSamples*/)
{
    if (audioEngine == nullptr)
    {
        setSceneTransitionStutterOverlayAmount(0.0f);
        return;
    }

    const auto transitionType = sanitizeRuntimeSceneChainTransitionType(
        sceneBoundaryTransitionType.load(std::memory_order_acquire));
    const auto transitionOption = sanitizeRuntimeSceneChainTransitionOption(
        sceneBoundaryTransitionOption.load(std::memory_order_acquire));
    const auto transitionProfile = sceneChainTransitionRuntimeProfile(transitionOption);
    const float transitionLengthBeats = sanitizeRuntimeSceneChainTransitionLengthBeats(
        sceneBoundaryTransitionLengthBeats.load(std::memory_order_acquire));
    const float transitionIntensity = sanitizeRuntimeSceneChainTransitionAmount(
        sceneBoundaryTransitionIntensity.load(std::memory_order_acquire),
        DefaultSceneTransitionIntensity);
    const float transitionDelayAmount = sanitizeRuntimeSceneChainTransitionAmount(
        sceneBoundaryTransitionDelayAmount.load(std::memory_order_acquire),
        DefaultSceneTransitionDelayAmount);
    const float transitionFilterAmount = sanitizeRuntimeSceneChainTransitionAmount(
        sceneBoundaryTransitionFilterAmount.load(std::memory_order_acquire),
        DefaultSceneTransitionFilterAmount);
    const float transitionChopAmount = sanitizeRuntimeSceneChainTransitionAmount(
        sceneBoundaryTransitionChopAmount.load(std::memory_order_acquire),
        DefaultSceneTransitionChopAmount);
    if (transitionType == SceneChainTransitionType::None
        || transitionType == SceneChainTransitionType::Return)
    {
        setSceneTransitionStutterOverlayAmount(0.0f);
        return;
    }

    double currentPpq = audioEngine->getTimelineBeat();
    if (posInfo.getPpqPosition().hasValue())
        currentPpq = *posInfo.getPpqPosition();
    const int64_t currentSample = audioEngine->getGlobalSampleCount();

    const double targetPpq = sceneBoundaryTransitionTargetPpq.load(std::memory_order_acquire);
    const int64_t targetSample = sceneBoundaryTransitionTargetSample.load(std::memory_order_acquire);
    const double transitionTempo = sceneBoundaryTransitionTempo.load(std::memory_order_acquire);
    const double leadBeats = sceneChainTransitionLeadBeatsRuntime(transitionType, transitionLengthBeats);
    double startPpq = sceneBoundaryTransitionStartPpq.load(std::memory_order_acquire);
    if (std::isfinite(targetPpq) && targetPpq >= 0.0 && leadBeats > 0.0)
        startPpq = juce::jmax(0.0, targetPpq - leadBeats);

    int64_t startSample = sceneBoundaryTransitionStartSample.load(std::memory_order_acquire);
    if (targetSample >= 0
        && currentSampleRate > 0.0
        && std::isfinite(transitionTempo)
        && transitionTempo > 0.0
        && leadBeats > 0.0)
    {
        const double leadSeconds = (leadBeats * 60.0) / transitionTempo;
        const int64_t leadSamples = static_cast<int64_t>(std::llround(leadSeconds * currentSampleRate));
        startSample = juce::jmax<int64_t>(0, targetSample - juce::jmax<int64_t>(0, leadSamples));
    }

    float progress = 0.0f;
    if (std::isfinite(currentPpq)
        && std::isfinite(startPpq)
        && std::isfinite(targetPpq)
        && targetPpq >= startPpq
        && targetPpq >= 0.0)
    {
        if (currentPpq >= targetPpq - 1.0e-6)
            progress = 1.0f;
        else if (currentPpq > startPpq + 1.0e-6)
            progress = static_cast<float>((currentPpq - startPpq)
                                          / juce::jmax(1.0e-9, targetPpq - startPpq));
    }
    else if (startSample >= 0 && targetSample >= startSample && currentSample >= 0)
    {
        if (currentSample >= targetSample)
            progress = 1.0f;
        else if (currentSample > startSample)
            progress = static_cast<float>(
                static_cast<double>(currentSample - startSample)
                / static_cast<double>(juce::jmax<int64_t>(1, targetSample - startSample)));
    }

    progress = juce::jlimit(0.0f, 1.0f, progress);
    if (progress <= 0.0f)
    {
        setSceneTransitionStutterOverlayAmount(0.0f);
        return;
    }

    const float eased = smoothSceneTransitionProgress(progress);
    const float attackWindow = smoothSceneTransitionProgress(juce::jlimit(0.0f, 1.0f, progress / 0.16f));
    const float releaseWindow = 1.0f
        - smoothSceneTransitionProgress(juce::jlimit(0.0f, 1.0f, (progress - 0.84f) / 0.16f));
    const float overlayWindow = juce::jlimit(0.0f, 1.0f, attackWindow * releaseWindow);
    const float baseBody = juce::jlimit(0.0f,
                                        1.0f,
                                        juce::jmax(eased,
                                                   juce::jmax(std::pow(progress, 0.62f),
                                                              std::sin(progress * juce::MathConstants<float>::pi))));
    const float body = juce::jlimit(0.0f, 1.0f, baseBody * overlayWindow);
    double beatsSinceStart = std::numeric_limits<double>::quiet_NaN();
    if (std::isfinite(currentPpq) && std::isfinite(startPpq))
        beatsSinceStart = juce::jmax(0.0, currentPpq - startPpq);
    else if (currentSample >= 0
             && startSample >= 0
             && std::isfinite(transitionTempo)
             && transitionTempo > 0.0
             && currentSampleRate > 0.0)
        beatsSinceStart = (static_cast<double>(currentSample - startSample) * transitionTempo)
            / (60.0 * currentSampleRate);

    const double pulseSpacingBeats = juce::jmax(
        1.0 / 16.0,
        leadBeats / juce::jlimit(2.0, 12.0, 2.0 + (10.0 * static_cast<double>(transitionChopAmount))));
    const float rhythmicPulse = sceneTransitionRhythmicPulse(
        beatsSinceStart,
        pulseSpacingBeats,
        0.8f + (3.4f * transitionChopAmount));
    const float gatePulse = juce::jlimit(0.0f, 1.0f, 0.18f + (0.82f * rhythmicPulse));
    const float transitionDrive = juce::jlimit(0.0f, 1.0f,
                                               (0.24f + (0.76f * transitionIntensity))
                                                   * transitionProfile.depthScale);
    const float delayEffect = juce::jlimit(0.0f, 1.0f,
                                           body * transitionProfile.delayScale
                                               * transitionDrive
                                               * (0.22f + (0.78f * transitionDelayAmount)));
    const float filterEffect = juce::jlimit(0.0f, 1.0f,
                                            body * transitionProfile.filterScale
                                                * transitionDrive
                                                * (0.22f + (0.78f * transitionFilterAmount)));
    const float chopEffect = juce::jlimit(0.0f, 1.0f,
                                          body * transitionProfile.stutterScale
                                              * transitionDrive
                                              * (0.18f + (0.82f * transitionChopAmount)));
    const float duckEffect = juce::jlimit(0.0f, 1.0f,
                                          body * transitionProfile.gateScale
                                              * transitionDrive
                                              * (0.18f + (0.82f * transitionChopAmount)));
    float stutterOverlayAmount = 0.0f;

    for (int stripIndex = 0; stripIndex < MaxStrips; ++stripIndex)
    {
        auto* strip = audioEngine->getStrip(stripIndex);
        if (strip == nullptr)
            continue;

        const bool stepMode = strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Step;
        auto* stepSampler = stepMode ? strip->getStepSampler() : nullptr;
        const bool hasPlayableContent = strip->hasAudio() || (stepSampler != nullptr && stepSampler->getHasAudio());
        if (!hasPlayableContent)
            continue;

        float targetVolume = strip->getVolume();
        float targetFilterFrequency = strip->getFilterFrequency();
        float targetFilterResonance = strip->getFilterResonance();
        float targetDelayMix = strip->getDelayMix();
        float targetDelayFeedback = strip->getDelayFeedback();
        bool applyFilter = false;

        switch (transitionType)
        {
            case SceneChainTransitionType::Fill:
                targetVolume = juce::jlimit(0.0f, 1.0f,
                                            targetVolume * (1.0f - ((0.05f + (0.12f * transitionDrive)) * duckEffect)));
                targetFilterFrequency = juce::jmap(filterEffect, 20000.0f, 1200.0f);
                targetFilterResonance = juce::jmap(filterEffect, 0.82f, 1.92f);
                targetDelayMix = juce::jlimit(0.0f, 1.0f,
                                              targetDelayMix + ((0.30f + (0.40f * transitionDrive)) * delayEffect));
                targetDelayFeedback = juce::jlimit(0.0f,
                                                   0.97f,
                                                   targetDelayFeedback + ((0.20f + (0.34f * transitionDrive)) * delayEffect));
                stutterOverlayAmount = juce::jmax(stutterOverlayAmount,
                                                  juce::jlimit(0.0f,
                                                               1.0f,
                                                               (0.04f + (0.14f * transitionDrive)) * chopEffect * gatePulse));
                applyFilter = true;
                break;
            case SceneChainTransitionType::Stutter:
                targetVolume = juce::jlimit(0.0f, 1.0f,
                                            targetVolume * juce::jlimit(0.14f,
                                                                        1.0f,
                                                                        (0.34f + (0.66f * gatePulse))
                                                                            * (1.0f - ((0.08f + (0.18f * transitionDrive)) * duckEffect))));
                targetFilterFrequency = juce::jmap(filterEffect, 18000.0f, 4200.0f);
                targetFilterResonance = juce::jmap(filterEffect, 0.82f, 1.52f);
                targetDelayMix = juce::jlimit(0.0f, 1.0f,
                                              targetDelayMix + ((0.08f + (0.18f * transitionDrive)) * delayEffect));
                targetDelayFeedback = juce::jlimit(0.0f,
                                                   0.97f,
                                                   targetDelayFeedback + ((0.10f + (0.16f * transitionDrive)) * delayEffect));
                stutterOverlayAmount = juce::jmax(stutterOverlayAmount,
                                                  juce::jlimit(0.0f,
                                                               1.0f,
                                                               (0.24f + (0.50f * transitionDrive)) * chopEffect));
                applyFilter = filterEffect > 0.02f;
                break;
            case SceneChainTransitionType::FilterRise:
                targetVolume = juce::jlimit(0.0f, 1.0f,
                                            targetVolume * (1.0f - ((0.04f + (0.10f * transitionDrive)) * duckEffect)));
                targetFilterFrequency = juce::jmap(filterEffect, 140.0f, 19500.0f);
                targetFilterResonance = juce::jmap(filterEffect, 0.92f, 2.10f);
                targetDelayMix = juce::jlimit(0.0f, 1.0f,
                                              targetDelayMix + ((0.06f + (0.12f * transitionDrive)) * delayEffect));
                targetDelayFeedback = juce::jlimit(0.0f,
                                                   0.97f,
                                                   targetDelayFeedback + ((0.04f + (0.12f * transitionDrive)) * delayEffect));
                applyFilter = true;
                break;
            case SceneChainTransitionType::Drop:
                targetVolume = juce::jlimit(0.0f, 1.0f,
                                            targetVolume * (1.0f - ((0.24f + (0.36f * transitionDrive)) * duckEffect)));
                targetFilterFrequency = juce::jmap(filterEffect, 17000.0f, 520.0f);
                targetFilterResonance = juce::jmap(filterEffect, 0.84f, 1.78f);
                targetDelayMix = juce::jlimit(0.0f, 1.0f,
                                              targetDelayMix + ((0.14f + (0.24f * transitionDrive)) * delayEffect));
                targetDelayFeedback = juce::jlimit(0.0f,
                                                   0.97f,
                                                   targetDelayFeedback + ((0.10f + (0.18f * transitionDrive)) * delayEffect));
                applyFilter = filterEffect > 0.01f;
                break;
            case SceneChainTransitionType::MuteTail:
                targetVolume = juce::jlimit(0.0f, 1.0f,
                                            targetVolume * juce::jmax(0.0f,
                                                                      1.0f - ((0.52f + (0.42f * transitionDrive)) * duckEffect)));
                targetDelayMix = juce::jlimit(0.0f,
                                              1.0f,
                                              targetDelayMix * (1.0f - juce::jmin(1.0f,
                                                                                  delayEffect * (0.82f + (0.16f * transitionDrive)))));
                targetDelayFeedback = juce::jlimit(0.0f,
                                                   0.97f,
                                                   targetDelayFeedback * (1.0f - juce::jmin(1.0f,
                                                                                           delayEffect * (0.86f + (0.12f * transitionDrive)))));
                break;
            case SceneChainTransitionType::Break:
                targetVolume = juce::jlimit(0.0f,
                                            1.0f,
                                            targetVolume * juce::jlimit(0.08f,
                                                                        1.0f,
                                                                        (0.18f + (0.82f * gatePulse))
                                                                            * juce::jmax(0.0f,
                                                                                         1.0f - ((0.28f + (0.34f * transitionDrive)) * duckEffect))));
                targetFilterFrequency = juce::jmap(filterEffect, 18000.0f, 2200.0f);
                targetFilterResonance = juce::jmap(filterEffect, 0.82f, 1.72f);
                targetDelayMix = juce::jlimit(0.0f,
                                              1.0f,
                                              targetDelayMix + ((0.14f + (0.28f * transitionDrive)) * delayEffect));
                targetDelayFeedback = juce::jlimit(0.0f,
                                                   0.97f,
                                                   targetDelayFeedback + ((0.16f + (0.24f * transitionDrive)) * delayEffect));
                stutterOverlayAmount = juce::jmax(stutterOverlayAmount,
                                                  juce::jlimit(0.0f,
                                                               1.0f,
                                                               (0.08f + (0.22f * transitionDrive)) * chopEffect));
                applyFilter = filterEffect > 0.02f;
                break;
            case SceneChainTransitionType::None:
            case SceneChainTransitionType::Return:
            default:
                break;
        }

        strip->setVolume(targetVolume);
        if (stepSampler != nullptr)
            stepSampler->setVolume(targetVolume);

        if (applyFilter)
        {
            strip->setFilterEnabled(true);
            strip->setFilterFrequency(targetFilterFrequency);
            strip->setFilterResonance(targetFilterResonance);
            if (stepSampler != nullptr)
            {
                stepSampler->setFilterEnabled(true);
                stepSampler->setFilterFrequency(targetFilterFrequency);
                stepSampler->setFilterResonance(targetFilterResonance);
            }
        }

        strip->setDelayMix(targetDelayMix);
        strip->setDelayFeedback(targetDelayFeedback);
    }

    setSceneTransitionStutterOverlayAmount(stutterOverlayAmount);
}

void MlrVSTAudioProcessor::setStripParameterValueFromMacro(int stripIndex,
                                                           const juce::String& parameterId,
                                                           float plainValue)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;

    if (auto* parameter = parameters.getParameter(parameterId))
    {
        const float normalized = juce::jlimit(0.0f, 1.0f, parameter->convertTo0to1(plainValue));
        parameter->setValueNotifyingHost(normalized);
    }
}

void MlrVSTAudioProcessor::writeStripFloatParameter(const juce::String& parameterId,
                                                    float plainValue,
                                                    std::atomic<float>* rawParam,
                                                    StripControlWriteMode writeMode)
{
    if (writeMode == StripControlWriteMode::NotifyHost)
    {
        if (auto* parameter = parameters.getParameter(parameterId))
        {
            parameter->setValueNotifyingHost(parameter->convertTo0to1(plainValue));
            return;
        }
    }

    if (rawParam != nullptr)
        rawParam->store(plainValue, std::memory_order_release);
}

void MlrVSTAudioProcessor::writeStripBoolParameter(const juce::String& parameterId,
                                                   bool enabled,
                                                   std::atomic<float>* rawParam,
                                                   StripControlWriteMode writeMode)
{
    writeStripFloatParameter(parameterId,
                             enabled ? 1.0f : 0.0f,
                             rawParam,
                             writeMode);
}

void MlrVSTAudioProcessor::setStripVolumeControlValue(int stripIndex,
                                                      float volume,
                                                      StripControlWriteMode writeMode)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;

    const float clamped = juce::jlimit(0.0f, 1.0f, volume);
    const auto arrayIndex = static_cast<size_t>(stripIndex);
    writeStripFloatParameter("stripVolume" + juce::String(stripIndex),
                             clamped,
                             stripVolumeParams[arrayIndex],
                             writeMode);

    if (audioEngine == nullptr)
        return;

    if (auto* strip = audioEngine->getStrip(stripIndex))
    {
        strip->setVolume(clamped);
        if (auto* stepSampler = strip->getStepSampler())
            stepSampler->setVolume(clamped);
    }
}

void MlrVSTAudioProcessor::setStripPanControlValue(int stripIndex,
                                                   float pan,
                                                   StripControlWriteMode writeMode)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;

    const float clamped = juce::jlimit(-1.0f, 1.0f, pan);
    const auto arrayIndex = static_cast<size_t>(stripIndex);
    writeStripFloatParameter("stripPan" + juce::String(stripIndex),
                             clamped,
                             stripPanParams[arrayIndex],
                             writeMode);

    if (audioEngine == nullptr)
        return;

    if (auto* strip = audioEngine->getStrip(stripIndex))
    {
        strip->setPan(clamped);
        if (auto* stepSampler = strip->getStepSampler())
            stepSampler->setPan(clamped);
    }
}

void MlrVSTAudioProcessor::setStripSpeedControlValue(int stripIndex,
                                                     float speed,
                                                     StripControlWriteMode writeMode)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;

    const float clamped = juce::jlimit(0.0f, 8.0f, speed);
    const auto arrayIndex = static_cast<size_t>(stripIndex);
    writeStripFloatParameter("stripSpeed" + juce::String(stripIndex),
                             clamped,
                             stripSpeedParams[arrayIndex],
                             writeMode);

    if (audioEngine == nullptr)
        return;

    if (auto* strip = audioEngine->getStrip(stripIndex))
    {
        if (strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Grain)
        {
            strip->setPlaybackSpeed(PlayheadSpeedQuantizer::grainPlaybackSpeedFromControl(clamped));
        }
        else
        {
            strip->setPlayheadSpeedRatio(
                PlayheadSpeedQuantizer::quantizeRatio(juce::jlimit(0.125f, 8.0f, clamped)));
        }
    }
}

void MlrVSTAudioProcessor::setStripFilterEnabledControlValue(int stripIndex,
                                                             bool enabled,
                                                             StripControlWriteMode writeMode)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;

    const auto arrayIndex = static_cast<size_t>(stripIndex);
    writeStripBoolParameter("stripFilterEnabled" + juce::String(stripIndex),
                            enabled,
                            stripFilterEnabledParams[arrayIndex],
                            writeMode);

    if (audioEngine == nullptr)
        return;

    if (auto* strip = audioEngine->getStrip(stripIndex))
    {
        auto state = resolveOwnedStripControlStateFromParameters(stripIndex, *strip);
        state.filterEnabled = enabled;
        applyResolvedStripFilterState(*strip, state);
    }
}

void MlrVSTAudioProcessor::setStripFilterFrequencyControlValue(int stripIndex,
                                                               float frequency,
                                                               StripControlWriteMode writeMode)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;

    const float clamped = juce::jlimit(20.0f, 20000.0f, frequency);
    const auto arrayIndex = static_cast<size_t>(stripIndex);
    writeStripBoolParameter("stripFilterEnabled" + juce::String(stripIndex),
                            true,
                            stripFilterEnabledParams[arrayIndex],
                            writeMode);
    writeStripFloatParameter("stripFilterFrequency" + juce::String(stripIndex),
                             clamped,
                             stripFilterFrequencyParams[arrayIndex],
                             writeMode);

    if (audioEngine == nullptr)
        return;

    if (auto* strip = audioEngine->getStrip(stripIndex))
    {
        auto state = resolveOwnedStripControlStateFromParameters(stripIndex, *strip);
        state.filterEnabled = true;
        state.filterFrequency = clamped;
        applyResolvedStripFilterState(*strip, state);
    }
}

void MlrVSTAudioProcessor::setStripFilterResonanceControlValue(int stripIndex,
                                                               float resonance,
                                                               StripControlWriteMode writeMode)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;

    const float clamped = juce::jlimit(0.1f, 10.0f, resonance);
    const auto arrayIndex = static_cast<size_t>(stripIndex);
    writeStripBoolParameter("stripFilterEnabled" + juce::String(stripIndex),
                            true,
                            stripFilterEnabledParams[arrayIndex],
                            writeMode);
    writeStripFloatParameter("stripFilterResonance" + juce::String(stripIndex),
                             clamped,
                             stripFilterResonanceParams[arrayIndex],
                             writeMode);

    if (audioEngine == nullptr)
        return;

    if (auto* strip = audioEngine->getStrip(stripIndex))
    {
        auto state = resolveOwnedStripControlStateFromParameters(stripIndex, *strip);
        state.filterEnabled = true;
        state.filterResonance = clamped;
        applyResolvedStripFilterState(*strip, state);
    }
}

void MlrVSTAudioProcessor::setStripFilterMorphControlValue(int stripIndex,
                                                           float morph,
                                                           StripControlWriteMode writeMode)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;

    const float clamped = juce::jlimit(0.0f, 1.0f, morph);
    const auto arrayIndex = static_cast<size_t>(stripIndex);
    writeStripBoolParameter("stripFilterEnabled" + juce::String(stripIndex),
                            true,
                            stripFilterEnabledParams[arrayIndex],
                            writeMode);
    writeStripFloatParameter("stripFilterMorph" + juce::String(stripIndex),
                             clamped,
                             stripFilterMorphParams[arrayIndex],
                             writeMode);

    if (audioEngine == nullptr)
        return;

    if (auto* strip = audioEngine->getStrip(stripIndex))
    {
        auto state = resolveOwnedStripControlStateFromParameters(stripIndex, *strip);
        state.filterEnabled = true;
        state.filterMorph = clamped;
        applyResolvedStripFilterState(*strip, state);
    }
}

void MlrVSTAudioProcessor::setStripFilterAlgorithmControlValue(int stripIndex,
                                                               EnhancedAudioStrip::FilterAlgorithm algorithm,
                                                               StripControlWriteMode writeMode)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;

    const int clampedIndex = juce::jlimit(0, 5, static_cast<int>(algorithm));
    const auto safeAlgorithm = static_cast<EnhancedAudioStrip::FilterAlgorithm>(clampedIndex);
    const auto arrayIndex = static_cast<size_t>(stripIndex);
    writeStripBoolParameter("stripFilterEnabled" + juce::String(stripIndex),
                            true,
                            stripFilterEnabledParams[arrayIndex],
                            writeMode);
    writeStripFloatParameter("stripFilterAlgorithm" + juce::String(stripIndex),
                             static_cast<float>(clampedIndex),
                             stripFilterAlgorithmParams[arrayIndex],
                             writeMode);

    if (audioEngine == nullptr)
        return;

    if (auto* strip = audioEngine->getStrip(stripIndex))
    {
        auto state = resolveOwnedStripControlStateFromParameters(stripIndex, *strip);
        state.filterEnabled = true;
        state.filterAlgorithm = safeAlgorithm;
        applyResolvedStripFilterState(*strip, state);
    }
}

void MlrVSTAudioProcessor::setStripDelayMixControlValue(int stripIndex,
                                                        float mix,
                                                        StripControlWriteMode writeMode)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;

    const auto arrayIndex = static_cast<size_t>(stripIndex);
    const float clamped = juce::jlimit(0.0f, 1.0f, mix);
    writeStripFloatParameter("stripDelayMix" + juce::String(stripIndex),
                             clamped,
                             stripDelayMixParams[arrayIndex],
                             writeMode);

    if (audioEngine == nullptr)
        return;

    if (auto* strip = audioEngine->getStrip(stripIndex))
    {
        auto state = resolveOwnedStripControlStateFromParameters(stripIndex, *strip);
        state.delayMix = clamped;
        applyResolvedStripDelayState(*strip, state);
    }
}

void MlrVSTAudioProcessor::setStripDelayTimeControlValue(int stripIndex,
                                                         float timeValue,
                                                         StripControlWriteMode writeMode)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;

    const auto arrayIndex = static_cast<size_t>(stripIndex);
    const float clamped = juce::jlimit(0.25f, 4.0f, timeValue);
    writeStripFloatParameter("stripDelayTime" + juce::String(stripIndex),
                             clamped,
                             stripDelayTimeParams[arrayIndex],
                             writeMode);

    if (audioEngine == nullptr)
        return;

    if (auto* strip = audioEngine->getStrip(stripIndex))
    {
        auto state = resolveOwnedStripControlStateFromParameters(stripIndex, *strip);
        state.delayTime = clamped;
        applyResolvedStripDelayState(*strip, state);
    }
}

void MlrVSTAudioProcessor::setStripDelaySyncEnabledControlValue(int stripIndex,
                                                                bool enabled,
                                                                StripControlWriteMode writeMode)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;

    const auto arrayIndex = static_cast<size_t>(stripIndex);
    writeStripBoolParameter("stripDelaySync" + juce::String(stripIndex),
                            enabled,
                            stripDelaySyncParams[arrayIndex],
                            writeMode);

    if (audioEngine == nullptr)
        return;

    if (auto* strip = audioEngine->getStrip(stripIndex))
    {
        auto state = resolveOwnedStripControlStateFromParameters(stripIndex, *strip);
        state.delaySyncEnabled = enabled;
        applyResolvedStripDelayState(*strip, state);
    }
}

void MlrVSTAudioProcessor::setStripDelayFeedbackControlValue(int stripIndex,
                                                             float feedback,
                                                             StripControlWriteMode writeMode)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;

    const auto arrayIndex = static_cast<size_t>(stripIndex);
    const float clamped = juce::jlimit(0.0f, 0.97f, feedback);
    writeStripFloatParameter("stripDelayFeedback" + juce::String(stripIndex),
                             clamped,
                             stripDelayFeedbackParams[arrayIndex],
                             writeMode);

    if (audioEngine == nullptr)
        return;

    if (auto* strip = audioEngine->getStrip(stripIndex))
    {
        auto state = resolveOwnedStripControlStateFromParameters(stripIndex, *strip);
        state.delayFeedback = clamped;
        applyResolvedStripDelayState(*strip, state);
    }
}

void MlrVSTAudioProcessor::setStripDelayLowCutControlValue(int stripIndex,
                                                           float hz,
                                                           StripControlWriteMode writeMode)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;

    const auto arrayIndex = static_cast<size_t>(stripIndex);
    const float clamped = juce::jlimit(20.0f, 12000.0f, hz);
    writeStripFloatParameter("stripDelayLowCut" + juce::String(stripIndex),
                             clamped,
                             stripDelayLowCutParams[arrayIndex],
                             writeMode);

    if (audioEngine == nullptr)
        return;

    if (auto* strip = audioEngine->getStrip(stripIndex))
    {
        auto state = resolveOwnedStripControlStateFromParameters(stripIndex, *strip);
        state.delayLowCutHz = clamped;
        applyResolvedStripDelayState(*strip, state);
    }
}

void MlrVSTAudioProcessor::setStripDelayHighCutControlValue(int stripIndex,
                                                            float hz,
                                                            StripControlWriteMode writeMode)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;

    const auto arrayIndex = static_cast<size_t>(stripIndex);
    const float clamped = juce::jlimit(200.0f, 20000.0f, hz);
    writeStripFloatParameter("stripDelayHighCut" + juce::String(stripIndex),
                             clamped,
                             stripDelayHighCutParams[arrayIndex],
                             writeMode);

    if (audioEngine == nullptr)
        return;

    if (auto* strip = audioEngine->getStrip(stripIndex))
    {
        auto state = resolveOwnedStripControlStateFromParameters(stripIndex, *strip);
        state.delayHighCutHz = clamped;
        applyResolvedStripDelayState(*strip, state);
    }
}

void MlrVSTAudioProcessor::setStripDelayModeControlValue(int stripIndex,
                                                         EnhancedAudioStrip::DelayMode mode,
                                                         StripControlWriteMode writeMode)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;

    const auto arrayIndex = static_cast<size_t>(stripIndex);
    const int clampedIndex = juce::jlimit(0, 2, static_cast<int>(mode));
    const auto safeMode = static_cast<EnhancedAudioStrip::DelayMode>(clampedIndex);
    writeStripFloatParameter("stripDelayMode" + juce::String(stripIndex),
                             static_cast<float>(clampedIndex),
                             stripDelayModeParams[arrayIndex],
                             writeMode);

    if (audioEngine == nullptr)
        return;

    if (auto* strip = audioEngine->getStrip(stripIndex))
    {
        auto state = resolveOwnedStripControlStateFromParameters(stripIndex, *strip);
        state.delayMode = safeMode;
        applyResolvedStripDelayState(*strip, state);
    }
}

StripControlState::ParameterView MlrVSTAudioProcessor::makeOwnedStripControlParameterView(int stripIndex) const
{
    StripControlState::ParameterView view;
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return view;

    const auto arrayIndex = static_cast<size_t>(stripIndex);
    view.volumeParam = stripVolumeParams[arrayIndex];
    view.trimDbParam = stripTrimDbParams[arrayIndex];
    view.panParam = stripPanParams[arrayIndex];
    view.speedParam = stripSpeedParams[arrayIndex];
    view.filterEnabledParam = stripFilterEnabledParams[arrayIndex];
    view.filterFrequencyParam = stripFilterFrequencyParams[arrayIndex];
    view.filterResonanceParam = stripFilterResonanceParams[arrayIndex];
    view.filterMorphParam = stripFilterMorphParams[arrayIndex];
    view.filterAlgorithmParam = stripFilterAlgorithmParams[arrayIndex];
    view.delayMixParam = stripDelayMixParams[arrayIndex];
    view.delayTimeParam = stripDelayTimeParams[arrayIndex];
    view.delaySyncParam = stripDelaySyncParams[arrayIndex];
    view.delayFeedbackParam = stripDelayFeedbackParams[arrayIndex];
    view.delayLowCutParam = stripDelayLowCutParams[arrayIndex];
    view.delayHighCutParam = stripDelayHighCutParams[arrayIndex];
    view.delayModeParam = stripDelayModeParams[arrayIndex];
    return view;
}

MlrVSTAudioProcessor::ResolvedOwnedStripControlState
MlrVSTAudioProcessor::resolveOwnedStripControlStateFromParameters(int stripIndex,
                                                                  const EnhancedAudioStrip& strip) const
{
    return StripControlState::resolveFromParameters(makeOwnedStripControlParameterView(stripIndex), strip);
}

void MlrVSTAudioProcessor::applyResolvedOwnedStripControlState(EnhancedAudioStrip& strip,
                                                               const ResolvedOwnedStripControlState& state) const
{
    StripControlState::applyOwnedControls(strip, state);
}

void MlrVSTAudioProcessor::applyResolvedStripFilterState(EnhancedAudioStrip& strip,
                                                         const ResolvedOwnedStripControlState& state) const
{
    StripControlState::applyFilter(strip, state);
}

void MlrVSTAudioProcessor::applyResolvedStripDelayState(EnhancedAudioStrip& strip,
                                                        const ResolvedOwnedStripControlState& state) const
{
    StripControlState::applyDelay(strip, state);
}

void MlrVSTAudioProcessor::applyOwnedStripControlsFromParameters(int stripIndex,
                                                                 EnhancedAudioStrip& strip)
{
    const auto state = resolveOwnedStripControlStateFromParameters(stripIndex, strip);
    applyResolvedOwnedStripControlState(strip, state);
    applyResolvedStripDelayState(strip, state);
}

float MlrVSTAudioProcessor::getMacroNormalizedValueForTarget(int stripIndex,
                                                             const EnhancedAudioStrip& strip,
                                                             MacroTarget target) const
{
    return MacroTargetDispatcher::getNormalizedValueForTarget(*this, stripIndex, strip, target);
}

void MlrVSTAudioProcessor::applyMacroTargetValue(int stripIndex,
                                                 EnhancedAudioStrip& strip,
                                                 MacroTarget target,
                                                 float normalizedValue)
{
    MacroTargetDispatcher::applyTargetValue(*this, stripIndex, strip, target, normalizedValue);
}

MlrVSTAudioProcessor::MacroState MlrVSTAudioProcessor::getMacroState() const
{
    MacroState state{};
    state.stripIndex = getMacroTargetStripIndex();

    if (!audioEngine)
        return state;

    auto* strip = audioEngine->getStrip(state.stripIndex);
    if (strip == nullptr)
        return state;

    state.hasTargetStrip = true;

    for (int macroIndex = 0; macroIndex < MacroCount; ++macroIndex)
        state.values[static_cast<size_t>(macroIndex)] = getMacroNormalizedValueForTarget(state.stripIndex,
                                                                                         *strip,
                                                                                         getMacroTarget(macroIndex));

    return state;
}

void MlrVSTAudioProcessor::setSelectedStripMacroValue(int macroIndex, float normalizedValue)
{
    if (!audioEngine || macroIndex < 0 || macroIndex >= MacroCount)
        return;

    const int stripIndex = getMacroTargetStripIndex();
    auto* strip = audioEngine->getStrip(stripIndex);
    if (strip == nullptr)
        return;

    const auto target = getMacroTarget(macroIndex);
    applyMacroTargetValue(stripIndex, *strip, target, normalizedValue);
    recordMacroSceneEvent(stripIndex, target, normalizedValue);
}

void MlrVSTAudioProcessor::handleIncomingMacroCc(const juce::MidiBuffer& midiMessages)
{
    for (const auto metadata : midiMessages)
    {
        const auto message = metadata.getMessage();
        if (!message.isController())
            continue;

        const int controllerNumber = message.getControllerNumber();
        const int learnIndex = macroMidiLearnIndex.load(std::memory_order_acquire);
        if (learnIndex >= 0 && learnIndex < MacroCount)
        {
            macroMidiCcAssignments[static_cast<size_t>(learnIndex)].store(controllerNumber, std::memory_order_release);
            macroMidiLearnIndex.store(-1, std::memory_order_release);
            markPersistentGlobalUserChange();
            setSelectedStripMacroValue(learnIndex,
                                       static_cast<float>(message.getControllerValue()) / 127.0f);
            continue;
        }

        for (int macroIndex = 0; macroIndex < MacroCount; ++macroIndex)
        {
            if (controllerNumber != getMacroMidiCc(macroIndex))
                continue;

            setSelectedStripMacroValue(macroIndex,
                                       static_cast<float>(message.getControllerValue()) / 127.0f);
            break;
        }
    }
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
            stepSampler->setRootMidi(resolved.globalRootMidi);
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

MlrVSTAudioProcessor::LoopPitchRole MlrVSTAudioProcessor::getLoopPitchRole(int stripIndex) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return LoopPitchRole::None;
    return sanitizeLoopPitchRole(loopPitchRoles[static_cast<size_t>(stripIndex)].load(std::memory_order_acquire));
}

int MlrVSTAudioProcessor::getLoopPitchMasterStripIndex() const
{
    for (int i = 0; i < MaxStrips; ++i)
    {
        if (sanitizeLoopPitchRole(loopPitchRoles[static_cast<size_t>(i)].load(std::memory_order_acquire))
            == LoopPitchRole::Master)
        {
            return i;
        }
    }

    return -1;
}

MlrVSTAudioProcessor::LoopPitchSyncTiming MlrVSTAudioProcessor::getLoopPitchSyncTiming(int stripIndex) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return LoopPitchSyncTiming::Immediate;
    return sanitizeLoopPitchSyncTiming(loopPitchSyncTimings[static_cast<size_t>(stripIndex)].load(std::memory_order_acquire));
}

void MlrVSTAudioProcessor::setLoopPitchRole(int stripIndex, LoopPitchRole role)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;

    const auto previousRole = getLoopPitchRole(stripIndex);
    const auto clampedRole = sanitizeLoopPitchRole(static_cast<int>(role));
    loopPitchPendingRetune[static_cast<size_t>(stripIndex)].store(0, std::memory_order_release);
    if (clampedRole == LoopPitchRole::Master)
    {
        for (int i = 0; i < MaxStrips; ++i)
        {
            if (i == stripIndex)
                continue;
            if (getLoopPitchRole(i) == LoopPitchRole::Master)
            {
                loopPitchRoles[static_cast<size_t>(i)].store(static_cast<int>(LoopPitchRole::None), std::memory_order_release);
                loopPitchPendingRetune[static_cast<size_t>(i)].store(0, std::memory_order_release);
            }
        }
    }

    loopPitchRoles[static_cast<size_t>(stripIndex)].store(static_cast<int>(clampedRole), std::memory_order_release);

    if (previousRole == LoopPitchRole::Sync && clampedRole != LoopPitchRole::Sync)
        applyStoredPitchControlToStrip(stripIndex);

    if (clampedRole == LoopPitchRole::Master)
    {
        if (getLoopStripAssignedPitchMidi(stripIndex) >= 0 || getLoopStripDetectedPitchMidi(stripIndex) >= 0)
            updateGlobalRootFromLoopPitchMaster(stripIndex, true);
        requestLoopStripPitchMaster(stripIndex);
    }
    else if (clampedRole == LoopPitchRole::Sync)
    {
        if (getLoopStripAssignedPitchMidi(stripIndex) >= 0)
            requestLoopPitchRoleStateUpdate(stripIndex);
        else
            requestLoopStripPitchSync(stripIndex);
    }

    presetRefreshToken.fetch_add(1, std::memory_order_acq_rel);
}

void MlrVSTAudioProcessor::setLoopPitchSyncTiming(int stripIndex, LoopPitchSyncTiming timing)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;

    loopPitchSyncTimings[static_cast<size_t>(stripIndex)].store(
        static_cast<int>(sanitizeLoopPitchSyncTiming(static_cast<int>(timing))),
        std::memory_order_release);

    if (getLoopPitchRole(stripIndex) == LoopPitchRole::Sync)
        requestLoopPitchRoleStateUpdate(stripIndex);

    presetRefreshToken.fetch_add(1, std::memory_order_acq_rel);
}

int MlrVSTAudioProcessor::getLoopStripAssignedPitchMidi(int stripIndex) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return -1;
    return loopPitchAssignedMidi[static_cast<size_t>(stripIndex)].load(std::memory_order_acquire);
}

float MlrVSTAudioProcessor::getLoopStripPitchSyncCorrectionSemitones(int stripIndex) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return 0.0f;

    const float sourceMidi = getEffectiveLoopPitchSourceMidi(stripIndex);
    if (sourceMidi < 0.0f)
        return 0.0f;

    return computeNearestPitchClassCorrectionSemitones(sourceMidi, getGlobalRootNoteMidi());
}

void MlrVSTAudioProcessor::setLoopStripAssignedPitchMidi(int stripIndex, int midiNote, bool manualOverride)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;

    const int clampedMidi = juce::jlimit(-1, 127, midiNote);
    loopPitchAssignedMidi[static_cast<size_t>(stripIndex)].store(clampedMidi, std::memory_order_release);
    loopPitchAssignedManual[static_cast<size_t>(stripIndex)].store(manualOverride ? 1 : 0, std::memory_order_release);

    const auto role = getLoopPitchRole(stripIndex);
    if (role == LoopPitchRole::Master && clampedMidi >= 0)
        updateGlobalRootFromLoopPitchMaster(stripIndex, true);
    else if (role == LoopPitchRole::Sync && clampedMidi >= 0)
        requestLoopPitchRoleStateUpdate(stripIndex);

    presetRefreshToken.fetch_add(1, std::memory_order_acq_rel);
}

void MlrVSTAudioProcessor::updateGlobalRootFromLoopPitchMaster(int stripIndex, bool markPersistent)
{
    if (getLoopPitchRole(stripIndex) != LoopPitchRole::Master)
        return;

    const int effectiveMidi = getEffectiveLoopPitchMasterRootMidi(stripIndex);
    if (effectiveMidi >= 0)
        setGlobalRootNoteMidi(effectiveMidi, markPersistent);
}

int MlrVSTAudioProcessor::getGlobalRootNotePitchClass() const
{
    const int midi = getGlobalRootNoteMidi();
    const int pitchClass = midi % 12;
    return pitchClass < 0 ? pitchClass + 12 : pitchClass;
}

void MlrVSTAudioProcessor::setGlobalRootNoteMidi(int midiNote, bool markPersistent)
{
    const int clamped = juce::jlimit(0, 127, midiNote);
    const int previous = globalRootNoteMidi.exchange(clamped, std::memory_order_acq_rel);
    if (markPersistent)
        markPersistentGlobalUserChange();
    if (previous != clamped)
    {
        reapplyGlobalPitchQuantizationToAllStrips();
        applyLoopPitchSyncToAllStrips();
    }
}

void MlrVSTAudioProcessor::setGlobalRootNotePitchClass(int pitchClass)
{
    const int masterStripIndex = getLoopPitchMasterStripIndex();
    if (masterStripIndex >= 0)
    {
        updateGlobalRootFromLoopPitchMaster(masterStripIndex, true);
        return;
    }

    const int clampedPitchClass = ((pitchClass % 12) + 12) % 12;
    const int currentMidi = getGlobalRootNoteMidi();
    const int octave = juce::jlimit(0, 10, currentMidi / 12);
    int updatedMidi = (octave * 12) + clampedPitchClass;
    if (updatedMidi > 127)
        updatedMidi -= 12;
    setGlobalRootNoteMidi(updatedMidi, true);
}

ModernAudioEngine::PitchScale MlrVSTAudioProcessor::getGlobalPitchScale() const
{
    return static_cast<ModernAudioEngine::PitchScale>(juce::jlimit(
        0,
        static_cast<int>(ModernAudioEngine::PitchScale::PentatonicMinor),
        globalPitchScale.load(std::memory_order_acquire)));
}

void MlrVSTAudioProcessor::setGlobalPitchScale(ModernAudioEngine::PitchScale scale)
{
    const int clamped = juce::jlimit(0,
                                     static_cast<int>(ModernAudioEngine::PitchScale::PentatonicMinor),
                                     static_cast<int>(scale));
    globalPitchScale.store(clamped, std::memory_order_release);
    markPersistentGlobalUserChange();
    reapplyGlobalPitchQuantizationToAllStrips();
    applyLoopPitchSyncToAllStrips();
}

float MlrVSTAudioProcessor::quantizePitchSemitonesToGlobalScale(float semitones) const
{
    return ModernAudioEngine::quantizePitchSemitonesToScale(semitones,
                                                            getGlobalRootNoteMidi(),
                                                            getGlobalPitchScale());
}

void MlrVSTAudioProcessor::reapplyGlobalPitchQuantizationToAllStrips()
{
    if (audioEngine == nullptr)
        return;

    const int globalRootMidi = getGlobalRootNoteMidi();
    const auto globalScale = getGlobalPitchScale();

    for (int stripIndex = 0; stripIndex < MaxStrips; ++stripIndex)
    {
        auto* strip = audioEngine->getStrip(stripIndex);
        if (strip == nullptr)
            continue;

        strip->setGlobalPitchContext(globalRootMidi, static_cast<int>(globalScale));

        float semitones = strip->getPitchShift();
        if (auto* rawParam = stripPitchParams[static_cast<size_t>(stripIndex)])
            semitones = rawParam->load(std::memory_order_acquire);

        const float quantizedSemitones = quantizePitchSemitonesForStripControl(stripIndex, semitones);
        if (auto* parameter = parameters.getParameter("stripPitch" + juce::String(stripIndex)))
        {
            const float normalized = juce::jlimit(0.0f, 1.0f, parameter->convertTo0to1(quantizedSemitones));
            const float currentNormalized = juce::jlimit(0.0f, 1.0f, parameter->convertTo0to1(semitones));
            if (std::abs(currentNormalized - normalized) > 1.0e-5f)
                parameter->setValueNotifyingHost(normalized);
        }

        if (getLoopPitchRole(stripIndex) == LoopPitchRole::Sync)
        {
            applyPitchControlToStrip(stripIndex, *strip, getPitchSemitonesForDisplay(*strip));
            continue;
        }

        applyPitchControlToStrip(stripIndex, *strip, quantizedSemitones);
    }
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

void MlrVSTAudioProcessor::queueLoopPitchAnalysisResult(LoopPitchAnalysisResult result)
{
    const juce::ScopedLock lock(loopPitchAnalysisResultLock);
    loopPitchAnalysisResults.push_back(std::move(result));
}

void MlrVSTAudioProcessor::queueLoopStripLoadResult(LoopStripLoadResult result)
{
    const juce::ScopedLock lock(loopStripLoadResultLock);
    loopStripLoadResults.push_back(std::move(result));
}

void MlrVSTAudioProcessor::queueSoundTouchPitchCacheResult(SoundTouchPitchCacheResult result)
{
    const juce::ScopedLock lock(soundTouchPitchCacheResultLock);
    soundTouchPitchCacheResults.push_back(std::move(result));
}

void MlrVSTAudioProcessor::updateLoopStripLoadProgress(int stripIndex,
                                                       int requestId,
                                                       float progress,
                                                       const juce::String& statusText)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;

    if (requestId != loopStripLoadRequestIds[static_cast<size_t>(stripIndex)].load(std::memory_order_acquire))
        return;

    loopStripLoadProgressPermille[static_cast<size_t>(stripIndex)].store(
        juce::jlimit(0, 1000, static_cast<int>(std::round(juce::jlimit(0.0f, 1.0f, progress) * 1000.0f))),
        std::memory_order_release);

    const juce::ScopedLock lock(loopStripLoadStatusLock);
    loopStripLoadStatusTexts[static_cast<size_t>(stripIndex)] = compactLoopStripLoadStatus(statusText);
}

void MlrVSTAudioProcessor::resetLoopStripLoadProgress(int stripIndex)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;

    loopStripLoadProgressPermille[static_cast<size_t>(stripIndex)].store(0, std::memory_order_release);
    const juce::ScopedLock lock(loopStripLoadStatusLock);
    loopStripLoadStatusTexts[static_cast<size_t>(stripIndex)].clear();
}

void MlrVSTAudioProcessor::updateLoopPitchAnalysisProgress(int stripIndex,
                                                           int requestId,
                                                           float progress,
                                                           const juce::String& statusText)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;

    if (requestId != loopPitchAnalysisRequestIds[static_cast<size_t>(stripIndex)].load(std::memory_order_acquire))
        return;

    loopPitchAnalysisProgressPermille[static_cast<size_t>(stripIndex)].store(
        juce::jlimit(0, 1000, static_cast<int>(std::round(juce::jlimit(0.0f, 1.0f, progress) * 1000.0f))),
        std::memory_order_release);

    const juce::ScopedLock lock(loopPitchAnalysisStatusLock);
    loopPitchAnalysisStatusTexts[static_cast<size_t>(stripIndex)] = compactLoopPitchAnalysisStatus(statusText);
}

void MlrVSTAudioProcessor::resetLoopPitchAnalysisProgress(int stripIndex)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;

    loopPitchAnalysisProgressPermille[static_cast<size_t>(stripIndex)].store(0, std::memory_order_release);
    const juce::ScopedLock lock(loopPitchAnalysisStatusLock);
    loopPitchAnalysisStatusTexts[static_cast<size_t>(stripIndex)].clear();
}

void MlrVSTAudioProcessor::applyCompletedLoopPitchAnalyses()
{
    std::vector<LoopPitchAnalysisResult> results;
    {
        const juce::ScopedLock lock(loopPitchAnalysisResultLock);
        if (loopPitchAnalysisResults.empty())
            return;
        results.swap(loopPitchAnalysisResults);
    }

    for (auto& result : results)
    {
        if (result.stripIndex < 0 || result.stripIndex >= MaxStrips)
            continue;

        auto& inFlight = loopPitchAnalysisInFlight[static_cast<size_t>(result.stripIndex)];
        auto& requestCounter = loopPitchAnalysisRequestIds[static_cast<size_t>(result.stripIndex)];
        if (result.requestId != requestCounter.load(std::memory_order_acquire))
            continue;

        inFlight.store(0, std::memory_order_release);
        resetLoopPitchAnalysisProgress(result.stripIndex);
        loopPitchDetectedMidi[static_cast<size_t>(result.stripIndex)].store(result.detectedMidi, std::memory_order_release);
        loopPitchDetectedHz[static_cast<size_t>(result.stripIndex)].store(static_cast<float>(result.detectedHz), std::memory_order_release);
        loopPitchDetectedPitchConfidence[static_cast<size_t>(result.stripIndex)].store(
            juce::jlimit(0.0f, 1.0f, result.detectedPitchConfidence),
            std::memory_order_release);
        loopPitchDetectedScaleIndices[static_cast<size_t>(result.stripIndex)].store(
            juce::jlimit(-1,
                         static_cast<int>(ModernAudioEngine::PitchScale::PentatonicMinor),
                         result.detectedScaleIndex),
            std::memory_order_release);
        loopPitchDetectedScaleConfidence[static_cast<size_t>(result.stripIndex)].store(
            juce::jlimit(0.0f, 1.0f, result.detectedScaleConfidence),
            std::memory_order_release);
        loopPitchEssentiaUsed[static_cast<size_t>(result.stripIndex)].store(result.essentiaUsed ? 1 : 0, std::memory_order_release);
        const auto role = getLoopPitchRole(result.stripIndex);

        if (result.setAsRoot && role == LoopPitchRole::Master && result.detectedScaleIndex >= 0)
            setGlobalPitchScale(static_cast<ModernAudioEngine::PitchScale>(result.detectedScaleIndex));

        if (!result.success || result.detectedMidi < 0)
            continue;

        loopPitchAssignedMidi[static_cast<size_t>(result.stripIndex)].store(result.detectedMidi, std::memory_order_release);
        loopPitchAssignedManual[static_cast<size_t>(result.stripIndex)].store(0, std::memory_order_release);

        if (result.setAsRoot && role == LoopPitchRole::Master)
        {
            updateGlobalRootFromLoopPitchMaster(result.stripIndex, true);
            continue;
        }

        if (!result.setAsRoot && role == LoopPitchRole::Sync)
            requestLoopPitchRoleStateUpdate(result.stripIndex);
    }
}

void MlrVSTAudioProcessor::applyCompletedLoopStripLoads()
{
    std::vector<LoopStripLoadResult> results;
    {
        const juce::ScopedLock lock(loopStripLoadResultLock);
        if (loopStripLoadResults.empty())
            return;
        results.swap(loopStripLoadResults);
    }

    for (auto& result : results)
    {
        if (result.stripIndex < 0 || result.stripIndex >= MaxStrips)
            continue;

        const auto idx = static_cast<size_t>(result.stripIndex);
        if (result.requestId != loopStripLoadRequestIds[idx].load(std::memory_order_acquire))
            continue;

        loopStripLoadInFlight[idx].store(0, std::memory_order_release);
        resetLoopStripLoadProgress(result.stripIndex);
        pendingLoopStripFiles[idx] = juce::File();

        if (!result.success || audioEngine == nullptr)
            continue;

        auto* strip = audioEngine->getStrip(result.stripIndex);
        if (strip == nullptr)
            continue;
        if (strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Sample)
            continue;

        const bool isStepMode = (strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Step);
        const bool isFlipMode = (strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Sample);
        const bool requiresTimelineAnchor = strip->isPlaying() && !isStepMode && !isFlipMode;

        const float savedSpeed = strip->getPlaybackSpeed();
        const float savedVolume = strip->getVolume();
        const float savedPan = strip->getPan();
        const int savedGroup = strip->getGroup();
        const int savedLoopStart = strip->getLoopStart();
        const int savedLoopEnd = strip->getLoopEnd();
        const bool savedTimelineAnchored = strip->isPpqTimelineAnchored();
        const double savedTimelineOffsetBeats = strip->getPpqTimelineOffsetBeats();
        const int savedColumn = strip->getCurrentColumn();

        double hostPpqNow = 0.0;
        double hostTempoNow = audioEngine->getCurrentTempo();
        const bool canRestoreTimelineAnchor = requiresTimelineAnchor
            && savedTimelineAnchored
            && getHostSyncSnapshot(hostPpqNow, hostTempoNow);
        const int64_t currentGlobalSample = audioEngine->getGlobalSampleCount();

        auto* preparedTempoMatchBuffer = static_cast<juce::AudioBuffer<float>*>(nullptr);
        double preparedTempoMatchHostTempo = result.preparedTempoMatchHostTempo;
        const auto currentTempoMatchBackend = isStepMode
            ? TimeStretchBackend::Resample
            : resolveLoopTempoMatchBackendForStrip(result.stripIndex);
        if (currentTempoMatchBackend == TimeStretchBackend::Bungee
            && result.preparedTempoMatchBackend == TimeStretchBackend::Bungee
            && result.preparedTempoMatchBuffer.getNumSamples() > 0)
        {
            const double installTempo = (hostTempoNow > 0.0) ? hostTempoNow : result.preparedTempoMatchHostTempo;
            const int installFrames = computeLoopTempoMatchTargetFrames(result.sourceSampleRate,
                                                                        result.detectedBeatsForLoop,
                                                                        installTempo);
            if (installFrames == result.preparedTempoMatchBuffer.getNumSamples())
            {
                preparedTempoMatchBuffer = &result.preparedTempoMatchBuffer;
                preparedTempoMatchHostTempo = installTempo;
            }
            else
            {
                const int preparedFrames = computeLoopTempoMatchTargetFrames(result.sourceSampleRate,
                                                                             result.detectedBeatsForLoop,
                                                                             result.preparedTempoMatchHostTempo);
                if (preparedFrames == result.preparedTempoMatchBuffer.getNumSamples())
                {
                    preparedTempoMatchBuffer = &result.preparedTempoMatchBuffer;
                    preparedTempoMatchHostTempo = result.preparedTempoMatchHostTempo;
                }
            }
        }

        strip->adoptPreparedSample(result.decodedBuffer,
                                   result.sourceSampleRate,
                                   preparedTempoMatchBuffer,
                                   preparedTempoMatchHostTempo,
                                   result.detectedBeatsForLoop,
                                   result.sourceSampleRate,
                                   currentTempoMatchBackend);
        strip->setRecordingBars(result.detectedBars);
        if (canRestoreTimelineAnchor)
            strip->setBeatsPerLoopAtPpq(result.detectedBeatsForLoop, hostPpqNow);
        else
            strip->setBeatsPerLoop(result.detectedBeatsForLoop);

        setStripSpeedControlValue(result.stripIndex, savedSpeed, StripControlWriteMode::CacheOnly);
        setStripVolumeControlValue(result.stripIndex, savedVolume, StripControlWriteMode::CacheOnly);
        setStripPanControlValue(result.stripIndex, savedPan, StripControlWriteMode::CacheOnly);
        strip->setGroup(savedGroup);
        strip->setLoop(savedLoopStart, savedLoopEnd);

        if (canRestoreTimelineAnchor)
        {
            strip->restorePresetPpqState(true,
                                         true,
                                         savedTimelineOffsetBeats,
                                         savedColumn,
                                         hostTempoNow,
                                         hostPpqNow,
                                         currentGlobalSample);
        }

        rememberLoadedSamplePathForStrip(result.stripIndex, result.sourceFile);
        loopPitchDetectedMidi[idx].store(-1, std::memory_order_release);
        loopPitchDetectedHz[idx].store(0.0f, std::memory_order_release);
        loopPitchDetectedPitchConfidence[idx].store(0.0f, std::memory_order_release);
        loopPitchDetectedScaleIndices[idx].store(-1, std::memory_order_release);
        loopPitchDetectedScaleConfidence[idx].store(0.0f, std::memory_order_release);
        loopPitchAssignedMidi[idx].store(-1, std::memory_order_release);
        loopPitchAssignedManual[idx].store(0, std::memory_order_release);
        loopPitchPendingRetune[idx].store(0, std::memory_order_release);

        const auto role = getLoopPitchRole(result.stripIndex);
        if (role == LoopPitchRole::Master)
            requestLoopStripPitchMaster(result.stripIndex);
        else if (role == LoopPitchRole::Sync)
            requestLoopStripPitchSync(result.stripIndex);
    }
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

void MlrVSTAudioProcessor::applyLoopStripPitchSemitones(int stripIndex, float semitones)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips || audioEngine == nullptr)
        return;

    const float storedSemitones = quantizePitchSemitonesForStripControl(stripIndex,
                                                                        getStoredStripPitchSemitones(stripIndex));
    const float clamped = quantizePitchSemitonesForStripControl(stripIndex,
                                                                juce::jlimit(-24.0f,
                                                                             24.0f,
                                                                             storedSemitones + semitones));

    if (auto* strip = audioEngine->getStrip(stripIndex))
        applyPitchControlToStrip(stripIndex, *strip, clamped);
}

void MlrVSTAudioProcessor::normalizeLoopPitchMasterRoles()
{
    int keeperStrip = -1;
    for (int i = 0; i < MaxStrips; ++i)
    {
        if (sanitizeLoopPitchRole(loopPitchRoles[static_cast<size_t>(i)].load(std::memory_order_acquire))
            != LoopPitchRole::Master)
            continue;

        if (keeperStrip < 0)
        {
            keeperStrip = i;
            continue;
        }

        loopPitchRoles[static_cast<size_t>(i)].store(static_cast<int>(LoopPitchRole::None), std::memory_order_release);
        loopPitchPendingRetune[static_cast<size_t>(i)].store(0, std::memory_order_release);
    }
}

int MlrVSTAudioProcessor::getEffectiveLoopPitchMasterRootMidi(int stripIndex) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return -1;

    float sourceMidi = getEffectiveLoopPitchSourceMidi(stripIndex);
    if (sourceMidi < 0.0f)
        sourceMidi = static_cast<float>(getGlobalRootNoteMidi());

    float currentPitchSemitones = 0.0f;
    if (audioEngine != nullptr)
    {
        if (auto* strip = audioEngine->getStrip(stripIndex))
            currentPitchSemitones = getPitchSemitonesForDisplay(*strip);
    }

    return juce::jlimit(0,
                        127,
                        juce::roundToInt(sourceMidi + currentPitchSemitones));
}

int MlrVSTAudioProcessor::getPitchQuantizeReferenceRootMidiForStrip(int stripIndex) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return getGlobalRootNoteMidi();

    if (getLoopPitchRole(stripIndex) == LoopPitchRole::Master)
    {
        const float sourceMidi = getEffectiveLoopPitchSourceMidi(stripIndex);
        if (sourceMidi >= 0.0f)
            return juce::jlimit(0, 127, juce::roundToInt(sourceMidi));
    }

    return getGlobalRootNoteMidi();
}

float MlrVSTAudioProcessor::getLoopPitchTempoMatchOffsetSemitones(int stripIndex) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips || audioEngine == nullptr)
        return 0.0f;

    auto* strip = audioEngine->getStrip(stripIndex);
    if (strip == nullptr)
        return 0.0f;

    if (strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Sample)
    {
        if (!resolveFlipTempoMatch().usesRepitch())
            return 0.0f;

        auto* engine = sampleModeEngines[static_cast<size_t>(stripIndex)].get();
        if (engine == nullptr)
            return 0.0f;

        const float tempoMatchRatio = computeFlipTempoMatchRatio(audioEngine->getCurrentTempo(),
                                                                 engine->getAnalyzedTempoBpm());
        if (std::abs(tempoMatchRatio - 1.0f) <= 1.0e-4f)
            return 0.0f;

        return semitonesFromRatio(tempoMatchRatio);
    }

    if (strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Step
        || strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Grain
        || resolveLoopTempoMatchBackendForStrip(stripIndex) != TimeStretchBackend::Resample)
    {
        return 0.0f;
    }

    const auto* stripBuffer = strip->getAudioBuffer();
    const double hostTempo = audioEngine->getCurrentTempo();
    if (stripBuffer == nullptr
        || stripBuffer->getNumSamples() <= 0
        || !(currentSampleRate > 0.0)
        || !(hostTempo > 0.0))
    {
        return 0.0f;
    }

    float beatsForLoop = strip->getBeatsPerLoop();
    if (!(beatsForLoop > 0.0f))
        beatsForLoop = 4.0f;

    const double targetLoopLengthInSamples = static_cast<double>(beatsForLoop)
        * (60.0 / hostTempo)
        * currentSampleRate;
    if (!(targetLoopLengthInSamples > 0.0))
        return 0.0f;

    const double autoWarpSpeed = static_cast<double>(stripBuffer->getNumSamples()) / targetLoopLengthInSamples;
    if (!std::isfinite(autoWarpSpeed) || std::abs(autoWarpSpeed - 1.0) <= 1.0e-4)
        return 0.0f;

    return semitonesFromRatio(static_cast<float>(autoWarpSpeed));
}

float MlrVSTAudioProcessor::getEffectiveLoopPitchSourceMidi(int stripIndex) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return -1.0f;

    int sourceMidi = getLoopStripAssignedPitchMidi(stripIndex);
    if (sourceMidi < 0)
        sourceMidi = getLoopStripDetectedPitchMidi(stripIndex);
    if (sourceMidi < 0)
        return -1.0f;

    return static_cast<float>(sourceMidi) + getLoopPitchTempoMatchOffsetSemitones(stripIndex);
}

void MlrVSTAudioProcessor::applyLoopPitchRoleStateToStrip(int stripIndex)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips || audioEngine == nullptr)
        return;

    if (getLoopPitchRole(stripIndex) != LoopPitchRole::Sync)
        return;

    const float sourceMidi = getEffectiveLoopPitchSourceMidi(stripIndex);
    if (sourceMidi < 0.0f)
    {
        loopPitchPendingRetune[static_cast<size_t>(stripIndex)].store(0, std::memory_order_release);
        applyStoredPitchControlToStrip(stripIndex);
        return;
    }

    const float deltaSemitones = computeNearestPitchClassCorrectionSemitones(
        sourceMidi,
        getGlobalRootNoteMidi());

    loopPitchPendingRetune[static_cast<size_t>(stripIndex)].store(0, std::memory_order_release);
    applyLoopStripPitchSemitones(stripIndex, deltaSemitones);
}

void MlrVSTAudioProcessor::requestLoopPitchRoleStateUpdate(int stripIndex)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips || audioEngine == nullptr)
        return;

    if (getLoopPitchRole(stripIndex) != LoopPitchRole::Sync)
        return;

    auto* strip = audioEngine->getStrip(stripIndex);
    if (strip == nullptr)
        return;

    const auto timing = getLoopPitchSyncTiming(stripIndex);
    const bool immediate = timing == LoopPitchSyncTiming::Immediate
        || !strip->isPlaying()
        || strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Step;

    if (immediate)
    {
        applyLoopPitchRoleStateToStrip(stripIndex);
        return;
    }

    loopPitchPendingRetune[static_cast<size_t>(stripIndex)].store(1, std::memory_order_release);
}

bool MlrVSTAudioProcessor::applyPendingLoopPitchRetuneOnTrigger(int stripIndex)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return false;

    if (getLoopPitchRole(stripIndex) != LoopPitchRole::Sync)
    {
        loopPitchPendingRetune[static_cast<size_t>(stripIndex)].store(0, std::memory_order_release);
        return false;
    }

    if (loopPitchPendingRetune[static_cast<size_t>(stripIndex)].load(std::memory_order_acquire) == 0)
        return false;

    if (getLoopPitchSyncTiming(stripIndex) != LoopPitchSyncTiming::NextTrigger)
        return false;

    applyLoopPitchRoleStateToStrip(stripIndex);
    return true;
}

void MlrVSTAudioProcessor::applyPendingLoopPitchRetunes()
{
    if (audioEngine == nullptr)
        return;

    const double timelineBeat = audioEngine->getTimelineBeat();
    const int currentHostBar = std::isfinite(timelineBeat)
        ? juce::jmax(0, static_cast<int>(std::floor(timelineBeat / 4.0)))
        : -1;

    for (int i = 0; i < MaxStrips; ++i)
    {
        auto* strip = audioEngine->getStrip(i);
        const int currentColumn = (strip != nullptr && strip->isPlaying()) ? strip->getCurrentColumn() : -1;

        if (loopPitchPendingRetune[static_cast<size_t>(i)].load(std::memory_order_acquire) != 0
            && strip != nullptr
            && strip->isPlaying())
        {
            if (getLoopPitchRole(i) != LoopPitchRole::Sync)
            {
                loopPitchPendingRetune[static_cast<size_t>(i)].store(0, std::memory_order_release);
                loopPitchLastObservedColumns[static_cast<size_t>(i)] = currentColumn;
                continue;
            }

            switch (getLoopPitchSyncTiming(i))
            {
                case LoopPitchSyncTiming::NextLoop:
                    if (loopPitchLastObservedColumns[static_cast<size_t>(i)] >= 0
                        && currentColumn >= 0
                        && currentColumn < loopPitchLastObservedColumns[static_cast<size_t>(i)])
                        applyLoopPitchRoleStateToStrip(i);
                    break;
                case LoopPitchSyncTiming::NextBar:
                    if (currentHostBar >= 0
                        && loopPitchLastObservedHostBar >= 0
                        && currentHostBar != loopPitchLastObservedHostBar)
                        applyLoopPitchRoleStateToStrip(i);
                    break;
                case LoopPitchSyncTiming::Immediate:
                case LoopPitchSyncTiming::NextTrigger:
                default:
                    break;
            }
        }

        loopPitchLastObservedColumns[static_cast<size_t>(i)] = currentColumn;
    }

    if (currentHostBar >= 0)
        loopPitchLastObservedHostBar = currentHostBar;
}

void MlrVSTAudioProcessor::applyLoopPitchSyncToAllStrips()
{
    for (int i = 0; i < MaxStrips; ++i)
    {
        if (getLoopPitchRole(i) == LoopPitchRole::Sync)
            requestLoopPitchRoleStateUpdate(i);
    }
}

void MlrVSTAudioProcessor::resolveLoopPitchRecallStateImmediately()
{
    if (audioEngine == nullptr)
        return;

    normalizeLoopPitchMasterRoles();

    const int masterStripIndex = getLoopPitchMasterStripIndex();
    if (masterStripIndex >= 0)
    {
        const int effectiveMidi = getEffectiveLoopPitchMasterRootMidi(masterStripIndex);
        if (effectiveMidi >= 0)
            globalRootNoteMidi.store(juce::jlimit(0, 127, effectiveMidi), std::memory_order_release);
    }

    const int rootMidi = getGlobalRootNoteMidi();
    const auto scale = getGlobalPitchScale();
    for (int stripIndex = 0; stripIndex < MaxStrips; ++stripIndex)
    {
        if (auto* strip = audioEngine->getStrip(stripIndex))
            strip->setGlobalPitchContext(rootMidi, static_cast<int>(scale));

        if (getLoopPitchRole(stripIndex) == LoopPitchRole::Sync)
            applyLoopPitchRoleStateToStrip(stripIndex);
        else
            loopPitchPendingRetune[static_cast<size_t>(stripIndex)].store(0, std::memory_order_release);
    }

    loopPitchLastObservedColumns.fill(-1);
    loopPitchLastObservedHostBar = -1;
}

MlrVSTAudioProcessor::ControlPageOrder MlrVSTAudioProcessor::getControlPageOrder() const
{
    const juce::ScopedLock lock(controlPageOrderLock);
    return controlPageOrder;
}

MlrVSTAudioProcessor::ControlMode MlrVSTAudioProcessor::getControlModeForControlButton(int buttonIndex) const
{
    const int clamped = juce::jlimit(0, NumControlRowPages - 1, buttonIndex);
    const juce::ScopedLock lock(controlPageOrderLock);
    return controlPageOrder[static_cast<size_t>(clamped)];
}

int MlrVSTAudioProcessor::getControlButtonForMode(ControlMode mode) const
{
    const juce::ScopedLock lock(controlPageOrderLock);
    for (int i = 0; i < NumControlRowPages; ++i)
    {
        if (controlPageOrder[static_cast<size_t>(i)] == mode)
            return i;
    }
    return -1;
}

void MlrVSTAudioProcessor::moveControlPage(int fromIndex, int toIndex)
{
    if (fromIndex == toIndex)
        return;

    fromIndex = juce::jlimit(0, NumControlRowPages - 1, fromIndex);
    toIndex = juce::jlimit(0, NumControlRowPages - 1, toIndex);
    if (fromIndex == toIndex)
        return;

    {
        const juce::ScopedLock lock(controlPageOrderLock);
        std::swap(controlPageOrder[static_cast<size_t>(fromIndex)],
                  controlPageOrder[static_cast<size_t>(toIndex)]);
    }

    savePersistentControlPages();
}

void MlrVSTAudioProcessor::setControlPageMomentary(bool shouldBeMomentary)
{
    controlPageMomentary.store(shouldBeMomentary, std::memory_order_release);
    savePersistentControlPages();
}

void MlrVSTAudioProcessor::setSwingDivisionSelection(int mode)
{
    const int maxDivision = static_cast<int>(EnhancedAudioStrip::SwingDivision::SixteenthTriplet);
    const int clamped = juce::jlimit(0, maxDivision, mode);
    swingDivisionSelection.store(clamped, std::memory_order_release);
    if (audioEngine)
        audioEngine->setGlobalSwingDivision(static_cast<EnhancedAudioStrip::SwingDivision>(clamped));
    savePersistentControlPages();
}

void MlrVSTAudioProcessor::setInnerLoopLengthSelection(int choiceIndex)
{
    const int clamped = juce::jlimit(0, 4, choiceIndex);
    innerLoopLengthSelection.store(clamped, std::memory_order_release);

    if (auto* param = parameters.getParameter("innerLoopLength"))
        param->setValueNotifyingHost(param->convertTo0to1(static_cast<float>(clamped)));
}

void MlrVSTAudioProcessor::syncTransientDetectionSettingsFromParameters(bool refreshSlicesNow)
{
    const int onsetMethod = transientOnsetMethodParam != nullptr
        ? juce::jlimit(0, 2, static_cast<int>(std::round(transientOnsetMethodParam->load(std::memory_order_acquire))))
        : 2;
    const int sensitivity = transientSensitivityParam != nullptr
        ? juce::jlimit(0, 4, static_cast<int>(std::round(transientSensitivityParam->load(std::memory_order_acquire))))
        : 2;
    const int snap = transientSnapParam != nullptr
        ? juce::jlimit(0, 4, static_cast<int>(std::round(transientSnapParam->load(std::memory_order_acquire))))
        : 3;
    const int spacing = transientSpacingParam != nullptr
        ? juce::jlimit(0, 4, static_cast<int>(std::round(transientSpacingParam->load(std::memory_order_acquire))))
        : 2;

    const bool changed = onsetMethod != lastAppliedTransientOnsetMethod
        || sensitivity != lastAppliedTransientSensitivity
        || snap != lastAppliedTransientSnap
        || spacing != lastAppliedTransientSpacing;

    if (!changed && !refreshSlicesNow)
        return;

    if (audioEngine != nullptr)
        audioEngine->setTransientDetectionConfig(onsetMethod, sensitivity, snap, spacing);
    lastAppliedTransientOnsetMethod = onsetMethod;
    lastAppliedTransientSensitivity = sensitivity;
    lastAppliedTransientSnap = snap;
    lastAppliedTransientSpacing = spacing;

    if (refreshSlicesNow)
    {
        if (audioEngine != nullptr)
            audioEngine->refreshTransientSliceMaps();

        for (auto& engine : sampleModeEngines)
        {
            if (engine != nullptr)
                engine->refreshTransientAnalysis();
        }
    }
}

void MlrVSTAudioProcessor::setControlModeFromGui(ControlMode mode, bool shouldBeActive)
{
    if (isSceneModeEnabled()
        && (mode == ControlMode::GroupAssign
            || mode == ControlMode::Modulation))
    {
        currentControlMode = ControlMode::Normal;
        controlModeActive = false;
        updateMonomeLEDs();
        return;
    }

    if (!shouldBeActive || mode == ControlMode::Normal)
    {
        currentControlMode = ControlMode::Normal;
        controlModeActive = false;
    }
    else
    {
        currentControlMode = mode;
        controlModeActive = true;
    }

    updateMonomeLEDs();
}

bool MlrVSTAudioProcessor::monomeControlPageShowsPatternRecorder(ControlMode mode) const
{
    switch (mode)
    {
        case ControlMode::Speed:
        case ControlMode::Pitch:
        case ControlMode::Pan:
        case ControlMode::Volume:
        case ControlMode::GrainSize:
        case ControlMode::Swing:
        case ControlMode::Delay:
        case ControlMode::Filter:
            return true;
        case ControlMode::Normal:
        case ControlMode::Gate:
        case ControlMode::FileBrowser:
        case ControlMode::GroupAssign:
        case ControlMode::Modulation:
        case ControlMode::Preset:
        case ControlMode::StepEdit:
        default:
            return false;
    }
}

bool MlrVSTAudioProcessor::handleMonomePatternButtonPress(int patternIndex, uint32_t nowMs)
{
    if (audioEngine == nullptr)
        return false;

    auto* pattern = audioEngine->getPattern(patternIndex);
    if (pattern == nullptr)
        return false;

    const auto idx = static_cast<size_t>(patternIndex);
    const auto pendingAction = monomePatternPadPendingAction[idx];
    const uint32_t pendingUntilMs = monomePatternPadPendingUntilMs[idx];
    const bool pendingActive = pendingAction != MonomePatternTapAction::None
        && pendingUntilMs != 0
        && static_cast<int32_t>(pendingUntilMs - nowMs) >= 0;

    if (pendingActive)
    {
        clearPendingMonomePatternTap(patternIndex);
        audioEngine->startPatternOverdub(patternIndex);
        return true;
    }

    clearPendingMonomePatternTap(patternIndex);

    if (pattern->isRecording())
    {
        if (pattern->isOverdubbing())
        {
            monomePatternPadPendingAction[idx] = MonomePatternTapAction::StopRecording;
            monomePatternPadPendingUntilMs[idx] = nowMs + monomePatternDoubleTapMs;
        }
        else
        {
            audioEngine->stopPatternRecording(patternIndex);
        }

        return true;
    }

    if (pattern->getEventCount() <= 0)
    {
        audioEngine->startPatternRecording(patternIndex);
        return true;
    }

    monomePatternPadPendingAction[idx] = pattern->isPlaying()
        ? MonomePatternTapAction::StopPlayback
        : MonomePatternTapAction::StartFreshRecording;
    monomePatternPadPendingUntilMs[idx] = nowMs + monomePatternDoubleTapMs;
    return true;
}

void MlrVSTAudioProcessor::processPendingMonomePatternTapActions(uint32_t nowMs)
{
    if (audioEngine == nullptr)
    {
        for (int patternIndex = 0; patternIndex < ModernAudioEngine::MaxPatterns; ++patternIndex)
            clearPendingMonomePatternTap(patternIndex);
        return;
    }

    for (int patternIndex = 0; patternIndex < ModernAudioEngine::MaxPatterns; ++patternIndex)
    {
        const auto idx = static_cast<size_t>(patternIndex);
        const auto action = monomePatternPadPendingAction[idx];
        const uint32_t pendingUntilMs = monomePatternPadPendingUntilMs[idx];
        const bool stillWaiting = action != MonomePatternTapAction::None
            && pendingUntilMs != 0
            && static_cast<int32_t>(pendingUntilMs - nowMs) > 0;
        if (stillWaiting)
            continue;

        clearPendingMonomePatternTap(patternIndex);

        switch (action)
        {
            case MonomePatternTapAction::StartFreshRecording:
                audioEngine->startPatternRecording(patternIndex);
                break;
            case MonomePatternTapAction::StopPlayback:
                audioEngine->stopPatternPlayback(patternIndex);
                break;
            case MonomePatternTapAction::StopRecording:
                audioEngine->stopPatternRecording(patternIndex);
                break;
            case MonomePatternTapAction::None:
            default:
                break;
        }
    }
}

void MlrVSTAudioProcessor::clearPendingMonomePatternTap(int patternIndex)
{
    if (patternIndex < 0 || patternIndex >= ModernAudioEngine::MaxPatterns)
        return;

    const auto idx = static_cast<size_t>(patternIndex);
    monomePatternPadPendingUntilMs[idx] = 0;
    monomePatternPadPendingAction[idx] = MonomePatternTapAction::None;
}

bool MlrVSTAudioProcessor::handleMonomeSceneRecorderButtonPress(uint32_t nowMs)
{
    if (!isSceneModeEnabled())
        return false;

    const auto pendingAction = monomeSceneRecorderPendingAction;
    const uint32_t pendingUntilMs = monomeSceneRecorderPendingUntilMs;
    const bool pendingActive = pendingAction != MonomeSceneRecorderTapAction::None
        && pendingUntilMs != 0
        && static_cast<int32_t>(pendingUntilMs - nowMs) >= 0;

    if (pendingActive)
    {
        clearPendingMonomeSceneRecorderTap();
        if (scenePerformanceRecorder.isRecording() && scenePerformanceRecorder.isOverdubbing())
            extendScenePerformanceRecording();
        else
            requestSceneRecorderActionQuantized(SceneRecorderAction::StartOverdub);
        return true;
    }

    clearPendingMonomeSceneRecorderTap();

    if (scenePerformanceRecorder.isRecording())
    {
        if (scenePerformanceRecorder.isOverdubbing())
        {
            monomeSceneRecorderPendingAction = MonomeSceneRecorderTapAction::StopRecording;
            monomeSceneRecorderPendingUntilMs = nowMs + monomePatternDoubleTapMs;
        }
        else
        {
            requestSceneRecorderActionQuantized(SceneRecorderAction::StopRecording);
        }

        return true;
    }

    const int sceneSlot = juce::jlimit(0, SceneSlots - 1, activeSceneSlot);
    if (!scenePerformanceRecorder.hasEvents(sceneSlot))
    {
        requestSceneRecorderActionQuantized(SceneRecorderAction::StartFreshRecording);
        return true;
    }

    monomeSceneRecorderPendingAction = MonomeSceneRecorderTapAction::StartFreshRecording;
    monomeSceneRecorderPendingUntilMs = nowMs + monomePatternDoubleTapMs;
    return true;
}

void MlrVSTAudioProcessor::processPendingMonomeSceneRecorderTapActions(uint32_t nowMs)
{
    const auto action = monomeSceneRecorderPendingAction;
    const uint32_t pendingUntilMs = monomeSceneRecorderPendingUntilMs;
    const bool stillWaiting = action != MonomeSceneRecorderTapAction::None
        && pendingUntilMs != 0
        && static_cast<int32_t>(pendingUntilMs - nowMs) > 0;
    if (stillWaiting)
        return;

    clearPendingMonomeSceneRecorderTap();

    switch (action)
    {
        case MonomeSceneRecorderTapAction::StartFreshRecording:
            requestSceneRecorderActionQuantized(SceneRecorderAction::StartFreshRecording);
            break;
        case MonomeSceneRecorderTapAction::StopRecording:
            requestSceneRecorderActionQuantized(SceneRecorderAction::StopRecording);
            break;
        case MonomeSceneRecorderTapAction::None:
        default:
            break;
    }
}

void MlrVSTAudioProcessor::clearPendingMonomeSceneRecorderTap()
{
    monomeSceneRecorderPendingUntilMs = 0;
    monomeSceneRecorderPendingAction = MonomeSceneRecorderTapAction::None;
}

void MlrVSTAudioProcessor::requestSceneRecorderActionQuantized(SceneRecorderAction action)
{
    if (!isSceneModeEnabled() || audioEngine == nullptr || action == SceneRecorderAction::None)
        return;

    const int sceneSlot = juce::jlimit(0, SceneSlots - 1, activeSceneSlot);
    const bool hostTransportPlaying = isHostTransportPlaying();
    const bool immediateAction = !hostTransportPlaying || getSceneRecallMode() == SceneRecallMode::Manual;
    const double currentBeat = audioEngine->getTimelineBeat();
    const double sceneStartBeat = activeSceneStartPpqValid && std::isfinite(activeSceneStartPpq)
        ? activeSceneStartPpq
        : currentBeat;

    if (immediateAction)
    {
        clearPendingSceneRecorderAction();
        switch (action)
        {
            case SceneRecorderAction::StartFreshRecording:
                startScenePerformanceRecordingAt(false, sceneSlot, currentBeat, sceneStartBeat);
                break;
            case SceneRecorderAction::StartOverdub:
                startScenePerformanceRecordingAt(true, sceneSlot, currentBeat, sceneStartBeat);
                break;
            case SceneRecorderAction::StopRecording:
                stopScenePerformanceRecordingNow();
                break;
            case SceneRecorderAction::None:
            default:
                break;
        }
        return;
    }

    pendingSceneRecorderAction.active = true;
    pendingSceneRecorderAction.targetResolved = false;
    pendingSceneRecorderAction.patternEndPhaseSignatureValid = false;
    pendingSceneRecorderAction.action = action;
    pendingSceneRecorderAction.sceneSlot = sceneSlot;
    pendingSceneRecorderAction.targetPpq = 0.0;
    pendingSceneRecorderAction.intervalBeats = 4.0;
    pendingSceneRecorderAction.patternEndPhaseSignature = 0;
    refreshUtilityTimerCadence();
    updateMonomeLEDs();
}

void MlrVSTAudioProcessor::updateSceneRecorderQuantizedAction(const juce::AudioPlayHead::PositionInfo& posInfo,
                                                              int numSamples)
{
    if (!pendingSceneRecorderAction.active || audioEngine == nullptr)
        return;

    if (pendingSceneRecorderApplyAction.load(std::memory_order_acquire) != static_cast<int>(SceneRecorderAction::None))
        return;

    const int sceneSlot = juce::jlimit(0, SceneSlots - 1, pendingSceneRecorderAction.sceneSlot);
    if (sceneSlot != juce::jlimit(0, SceneSlots - 1, activeSceneSlot))
    {
        clearPendingSceneRecorderAction();
        return;
    }

    if (!posInfo.getIsPlaying())
    {
        pendingSceneRecorderAction.targetResolved = false;
        pendingSceneRecorderAction.patternEndPhaseSignatureValid = false;
        pendingSceneRecorderAction.patternEndPhaseSignature = 0;
        return;
    }

    const auto ppqOpt = posInfo.getPpqPosition();
    const auto bpmOpt = posInfo.getBpm();
    if (!ppqOpt.hasValue() || !bpmOpt.hasValue()
        || !std::isfinite(*ppqOpt) || !std::isfinite(*bpmOpt)
        || *bpmOpt <= 0.0 || currentSampleRate <= 1.0)
    {
        return;
    }

    const double currentPpq = *ppqOpt;
    const auto sceneRecallMode = getSceneRecallMode();
    const bool patternEndEnabled = sceneRecallMode == SceneRecallMode::PatternEnd;
    const bool sceneEndEnabled = sceneRecallMode == SceneRecallMode::SceneEnd;
    uint64_t phaseAlignedPatternEndSignature = 0;
    const double currentSceneDurationBeats = juce::jlimit(0.25, 4096.0, computeCurrentSceneSequenceLengthBeats());
    const double phaseAlignedPatternEndPpq = patternEndEnabled
        ? computeNextScenePatternEndPpq(sceneSlot,
                                        currentPpq,
                                        currentSceneDurationBeats,
                                        &phaseAlignedPatternEndSignature)
        : std::numeric_limits<double>::quiet_NaN();
    const bool phaseAlignedTimingReady = patternEndEnabled && std::isfinite(phaseAlignedPatternEndPpq);
    const bool sceneEndTimingReady = sceneEndEnabled
        && activeSceneStartPpqValid
        && std::isfinite(activeSceneStartPpq);
    const bool useSceneDurationTiming = phaseAlignedTimingReady || sceneEndTimingReady;
    double intervalBeatsNow = getSceneRecallIntervalBeats();
    if (useSceneDurationTiming)
        intervalBeatsNow = currentSceneDurationBeats;

    if (pendingSceneRecorderAction.targetResolved
        && std::abs(intervalBeatsNow - pendingSceneRecorderAction.intervalBeats) > 1.0e-9)
    {
        pendingSceneRecorderAction.targetResolved = false;
        pendingSceneRecorderAction.targetPpq = 0.0;
    }

    if (patternEndEnabled)
    {
        if (phaseAlignedTimingReady)
        {
            if (!pendingSceneRecorderAction.patternEndPhaseSignatureValid
                || pendingSceneRecorderAction.patternEndPhaseSignature != phaseAlignedPatternEndSignature)
            {
                pendingSceneRecorderAction.targetResolved = false;
                pendingSceneRecorderAction.targetPpq = 0.0;
            }

            pendingSceneRecorderAction.patternEndPhaseSignatureValid = true;
            pendingSceneRecorderAction.patternEndPhaseSignature = phaseAlignedPatternEndSignature;
        }
        else if (pendingSceneRecorderAction.patternEndPhaseSignatureValid)
        {
            pendingSceneRecorderAction.patternEndPhaseSignatureValid = false;
            pendingSceneRecorderAction.patternEndPhaseSignature = 0;
            pendingSceneRecorderAction.targetResolved = false;
            pendingSceneRecorderAction.targetPpq = 0.0;
        }

        if (!phaseAlignedTimingReady)
        {
            pendingSceneRecorderAction.targetResolved = false;
            pendingSceneRecorderAction.targetPpq = 0.0;
            return;
        }
    }
    else if (pendingSceneRecorderAction.patternEndPhaseSignatureValid)
    {
        pendingSceneRecorderAction.patternEndPhaseSignatureValid = false;
        pendingSceneRecorderAction.patternEndPhaseSignature = 0;
    }

    if (!pendingSceneRecorderAction.targetResolved)
    {
        pendingSceneRecorderAction.intervalBeats = intervalBeatsNow;
        if (phaseAlignedTimingReady)
        {
            pendingSceneRecorderAction.targetPpq = phaseAlignedPatternEndPpq;
        }
        else if (useSceneDurationTiming)
        {
            double nextTarget = activeSceneStartPpq + intervalBeatsNow;
            if (nextTarget <= currentPpq + 1.0e-9)
            {
                const double elapsed = juce::jmax(0.0, currentPpq - activeSceneStartPpq);
                const double completedCycles = std::floor(elapsed / juce::jmax(1.0e-9, intervalBeatsNow));
                nextTarget = activeSceneStartPpq + ((completedCycles + 1.0) * intervalBeatsNow);
            }
            pendingSceneRecorderAction.targetPpq = nextTarget;
        }
        else
        {
            double nextBoundary = std::ceil(currentPpq / intervalBeatsNow) * intervalBeatsNow;
            if (nextBoundary <= currentPpq + 1.0e-9)
                nextBoundary += intervalBeatsNow;
            pendingSceneRecorderAction.targetPpq = std::round(nextBoundary / intervalBeatsNow) * intervalBeatsNow;
        }
        pendingSceneRecorderAction.targetResolved = true;
    }

    const double ppqPerSecond = *bpmOpt / 60.0;
    const double ppqPerSample = ppqPerSecond / currentSampleRate;
    const double blockEndPpq = currentPpq + (ppqPerSample * static_cast<double>(juce::jmax(1, numSamples)));
    if (blockEndPpq + 1.0e-9 < pendingSceneRecorderAction.targetPpq)
        return;

    const double targetPpq = pendingSceneRecorderAction.targetPpq;
    const double samplesToTarget = (targetPpq - currentPpq) / juce::jmax(1.0e-12, ppqPerSample);
    const int sampleOffset = juce::jlimit(0,
                                          juce::jmax(0, numSamples - 1),
                                          static_cast<int>(std::llround(samplesToTarget)));
    const int64_t targetGlobalSample = audioEngine->getGlobalSampleCount() + static_cast<int64_t>(sampleOffset);

    pendingSceneRecorderApplyAction.store(static_cast<int>(pendingSceneRecorderAction.action), std::memory_order_release);
    pendingSceneRecorderApplySceneSlot.store(sceneSlot, std::memory_order_release);
    pendingSceneRecorderApplyTargetPpq.store(targetPpq, std::memory_order_release);
    pendingSceneRecorderApplyTargetSample.store(targetGlobalSample, std::memory_order_release);
    pendingSceneRecorderAction.active = false;
    pendingSceneRecorderAction.targetResolved = false;
}

void MlrVSTAudioProcessor::processPendingSceneRecorderApply()
{
    const auto action = static_cast<SceneRecorderAction>(
        pendingSceneRecorderApplyAction.exchange(static_cast<int>(SceneRecorderAction::None), std::memory_order_acq_rel));
    if (action == SceneRecorderAction::None || audioEngine == nullptr || !isSceneModeEnabled())
        return;

    const int queuedSceneSlot = pendingSceneRecorderApplySceneSlot.exchange(-1, std::memory_order_acq_rel);
    const double queuedTargetPpq = pendingSceneRecorderApplyTargetPpq.exchange(-1.0, std::memory_order_acq_rel);
    const int64_t queuedTargetSample = pendingSceneRecorderApplyTargetSample.exchange(-1, std::memory_order_acq_rel);

    auto requeuePendingApply = [&](SceneRecorderAction requeueAction)
    {
        pendingSceneRecorderApplyAction.store(static_cast<int>(requeueAction), std::memory_order_release);
        pendingSceneRecorderApplySceneSlot.store(queuedSceneSlot, std::memory_order_release);
        pendingSceneRecorderApplyTargetPpq.store(queuedTargetPpq, std::memory_order_release);
        pendingSceneRecorderApplyTargetSample.store(queuedTargetSample, std::memory_order_release);
    };

    const int64_t currentGlobalSample = audioEngine->getGlobalSampleCount();
    if (queuedTargetSample >= 0
        && currentGlobalSample >= 0
        && isHostTransportPlaying()
        && currentGlobalSample + 1 < queuedTargetSample)
    {
        requeuePendingApply(action);
        return;
    }

    const int sceneSlot = juce::jlimit(0, SceneSlots - 1, queuedSceneSlot >= 0 ? queuedSceneSlot : activeSceneSlot);
    if (sceneSlot != juce::jlimit(0, SceneSlots - 1, activeSceneSlot))
        return;

    const double currentBeat = (std::isfinite(queuedTargetPpq) && queuedTargetPpq >= 0.0)
        ? queuedTargetPpq
        : audioEngine->getTimelineBeat();
    const double sceneStartBeat = activeSceneStartPpqValid && std::isfinite(activeSceneStartPpq)
        ? activeSceneStartPpq
        : currentBeat;

    switch (action)
    {
        case SceneRecorderAction::StartFreshRecording:
            startScenePerformanceRecordingAt(false, sceneSlot, currentBeat, sceneStartBeat);
            break;
        case SceneRecorderAction::StartOverdub:
            startScenePerformanceRecordingAt(true, sceneSlot, currentBeat, sceneStartBeat);
            break;
        case SceneRecorderAction::StopRecording:
            stopScenePerformanceRecordingNow();
            break;
        case SceneRecorderAction::None:
        default:
            break;
    }
}

void MlrVSTAudioProcessor::clearPendingSceneRecorderAction()
{
    pendingSceneRecorderAction = {};
    pendingSceneRecorderApplyAction.store(static_cast<int>(SceneRecorderAction::None), std::memory_order_release);
    pendingSceneRecorderApplySceneSlot.store(-1, std::memory_order_release);
    pendingSceneRecorderApplyTargetPpq.store(-1.0, std::memory_order_release);
    pendingSceneRecorderApplyTargetSample.store(-1, std::memory_order_release);
}

void MlrVSTAudioProcessor::clearPendingSceneTriggerRecord(int stripIndex)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;

    pendingSceneTriggerRecords[static_cast<size_t>(stripIndex)] = {};
}

void MlrVSTAudioProcessor::rememberPendingSceneTriggerRecord(int stripIndex, int column, double eventBeat)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;

    auto& pending = pendingSceneTriggerRecords[static_cast<size_t>(stripIndex)];
    pending.active = std::isfinite(eventBeat);
    pending.sceneSlot = juce::jlimit(0, SceneSlots - 1, activeSceneSlot);
    pending.column = juce::jlimit(0, MaxColumns - 1, column);
    pending.eventBeat = eventBeat;
}

void MlrVSTAudioProcessor::cancelPendingSceneTriggerRecord(int stripIndex)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips || audioEngine == nullptr)
        return;

    auto& pending = pendingSceneTriggerRecords[static_cast<size_t>(stripIndex)];
    if (pending.active
        && isSceneModeEnabled()
        && scenePerformanceRecorder.isRecording()
        && audioEngine->hasPendingTrigger(stripIndex))
    {
        scenePerformanceRecorder.cancelTriggerEvent(pending.sceneSlot,
                                                    stripIndex,
                                                    pending.column,
                                                    true,
                                                    pending.eventBeat);
    }

    pending = {};
}

void MlrVSTAudioProcessor::captureSceneTriggerRelease(int stripIndex, int columnHint)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips || audioEngine == nullptr)
        return;

    auto* strip = audioEngine->getStrip(stripIndex);
    if (strip == nullptr)
    {
        clearPendingSceneTriggerRecord(stripIndex);
        return;
    }

    cancelPendingSceneTriggerRecord(stripIndex);

    if (!isSceneModeEnabled() || !scenePerformanceRecorder.isRecording() || !strip->isPlaying())
        return;

    int releaseColumn = columnHint;
    if (releaseColumn < 0 || releaseColumn >= MaxColumns)
        releaseColumn = strip->getHeldButton();
    if (releaseColumn < 0 || releaseColumn >= MaxColumns)
        releaseColumn = 0;

    recordSceneTriggerEvent(stripIndex,
                            releaseColumn,
                            audioEngine->getTimelineBeat(),
                            -1,
                            -1,
                            false);
}

void MlrVSTAudioProcessor::recordSceneTriggerEvent(int stripIndex,
                                                   int column,
                                                   double eventBeat,
                                                   int sampleSliceId,
                                                   int64_t sampleStartSample,
                                                   bool noteOn)
{
    if (!isSceneModeEnabled() || !scenePerformanceRecorder.isRecording())
        return;

    const int sceneSlot = juce::jlimit(0, SceneSlots - 1, activeSceneSlot);
    scenePerformanceRecorder.recordTriggerEvent(sceneSlot,
                                                juce::jlimit(0, MaxStrips - 1, stripIndex),
                                                juce::jlimit(0, MaxColumns - 1, column),
                                                noteOn,
                                                eventBeat,
                                                sampleSliceId,
                                                sampleStartSample);
}

bool MlrVSTAudioProcessor::resolveScenePerformanceControlEvent(ControlMode mode,
                                                               int stripIndex,
                                                               int controlRow,
                                                               int column,
                                                               ScenePerformanceEvent& outEvent) const
{
    if (audioEngine == nullptr)
        return false;

    const int safeStripIndex = juce::jlimit(0, MaxStrips - 1, stripIndex);
    const int safeColumn = juce::jlimit(0, MaxColumns - 1, column);
    auto* strip = audioEngine->getStrip(safeStripIndex);
    if (strip == nullptr)
        return false;

    outEvent = {};
    outEvent.type = ScenePerformanceEventType::ControlPoint;
    outEvent.stripIndex = safeStripIndex;
    outEvent.column = safeColumn;
    outEvent.controlMode = static_cast<int>(mode);
    outEvent.controlRow = controlRow;

    switch (mode)
    {
        case ControlMode::Speed:
            outEvent.controlTarget = ScenePerformanceControlTarget::Speed;
            outEvent.value = PlayheadSpeedQuantizer::ratioFromColumn(safeColumn);
            return true;

        case ControlMode::Pitch:
        {
            static constexpr std::array<float, 16> kMusicalPitchSemitones{
                -24.0f, -21.0f, -18.0f, -15.0f, -12.0f, -9.0f, -6.0f, -3.0f,
                  0.0f,   3.0f,   6.0f,   9.0f,  12.0f, 15.0f, 18.0f, 24.0f
            };
            outEvent.controlTarget = ScenePerformanceControlTarget::Pitch;
            outEvent.value = kMusicalPitchSemitones[static_cast<size_t>(safeColumn)];
            return true;
        }

        case ControlMode::Pan:
            outEvent.controlTarget = ScenePerformanceControlTarget::Pan;
            outEvent.value = juce::jlimit(-1.0f, 1.0f, (safeColumn - 8) / 8.0f);
            return true;

        case ControlMode::Volume:
            outEvent.controlTarget = ScenePerformanceControlTarget::Volume;
            outEvent.value = juce::jlimit(0.0f, 1.0f, safeColumn / 15.0f);
            return true;

        case ControlMode::Swing:
            return false;

        case ControlMode::GrainSize:
        {
            const float unit = juce::jlimit(0.0f, 1.0f, safeColumn / 15.0f);
            switch (juce::jlimit(0, 5, controlRow))
            {
                case 0:
                    outEvent.controlTarget = ScenePerformanceControlTarget::GrainSize;
                    outEvent.value = 5.0f + (unit * (2400.0f - 5.0f));
                    return true;
                case 1:
                    outEvent.controlTarget = ScenePerformanceControlTarget::GrainDensity;
                    outEvent.value = 0.05f + (unit * (0.9f - 0.05f));
                    return true;
                case 2:
                    outEvent.controlTarget = ScenePerformanceControlTarget::Pitch;
                    outEvent.value = -24.0f + (unit * 48.0f);
                    return true;
                default:
                    return false;
            }
        }

        case ControlMode::Delay:
        {
            switch (juce::jlimit(0, 5, controlRow))
            {
                case 0:
                    outEvent.controlTarget = ScenePerformanceControlTarget::DelayMix;
                    outEvent.value = juce::jlimit(0.0f, 1.0f, safeColumn / 15.0f);
                    return true;
                case 1:
                {
                    const bool syncEnabled = strip->isDelaySyncEnabled();
                    const juce::NormalisableRange<float> syncRange(0.25f, 4.0f, 0.001f, 0.45f);
                    const juce::NormalisableRange<float> freeRange(0.25f, 4.0f, 0.001f, 0.4f);
                    outEvent.controlTarget = ScenePerformanceControlTarget::DelayTime;
                    outEvent.value = (syncEnabled ? syncRange : freeRange).convertFrom0to1(
                        juce::jlimit(0.0f, 1.0f, safeColumn / 15.0f));
                    return true;
                }
                case 2:
                    outEvent.controlTarget = ScenePerformanceControlTarget::DelayFeedback;
                    outEvent.value = 0.97f * juce::jlimit(0.0f, 1.0f, safeColumn / 15.0f);
                    return true;
                case 3:
                {
                    const juce::NormalisableRange<float> range(20.0f, 12000.0f, 1.0f, 0.25f);
                    outEvent.controlTarget = ScenePerformanceControlTarget::DelayLowCut;
                    outEvent.value = range.convertFrom0to1(juce::jlimit(0.0f, 1.0f, safeColumn / 15.0f));
                    return true;
                }
                case 4:
                {
                    const juce::NormalisableRange<float> range(200.0f, 20000.0f, 1.0f, 0.3f);
                    outEvent.controlTarget = ScenePerformanceControlTarget::DelayHighCut;
                    outEvent.value = range.convertFrom0to1(juce::jlimit(0.0f, 1.0f, safeColumn / 15.0f));
                    return true;
                }
                case 5:
                    if (safeColumn <= 2)
                    {
                        outEvent.controlTarget = ScenePerformanceControlTarget::DelayMode;
                        outEvent.value = static_cast<float>(safeColumn);
                        return true;
                    }
                    if (safeColumn >= 12)
                    {
                        outEvent.controlTarget = ScenePerformanceControlTarget::DelaySyncEnabled;
                        outEvent.value = safeColumn >= 14 ? 1.0f : 0.0f;
                        return true;
                    }
                    return false;
                default:
                    return false;
            }
        }

        case ControlMode::Filter:
            if (controlRow == 0)
            {
                const float t = juce::jlimit(0.0f, 1.0f, safeColumn / 15.0f);
                outEvent.controlTarget = ScenePerformanceControlTarget::FilterFrequency;
                outEvent.value = 20.0f * std::pow(1000.0f, t);
                return true;
            }
            if (controlRow == 1)
            {
                outEvent.controlTarget = ScenePerformanceControlTarget::FilterResonance;
                outEvent.value = 0.1f + (juce::jlimit(0.0f, 15.0f, static_cast<float>(safeColumn)) / 15.0f) * 9.9f;
                return true;
            }
            if (controlRow == 2 && safeColumn <= 2)
            {
                outEvent.controlTarget = ScenePerformanceControlTarget::FilterMorph;
                outEvent.value = (safeColumn <= 0) ? 0.0f : (safeColumn == 1 ? 0.5f : 1.0f);
                return true;
            }
            return false;

        case ControlMode::Normal:
        case ControlMode::Gate:
        case ControlMode::FileBrowser:
        case ControlMode::GroupAssign:
        case ControlMode::Modulation:
        case ControlMode::Preset:
        case ControlMode::StepEdit:
        default:
            return false;
    }
}

bool MlrVSTAudioProcessor::resolveScenePerformanceMacroEvent(int stripIndex,
                                                             MacroTarget target,
                                                             float normalizedValue,
                                                             ScenePerformanceEvent& outEvent) const
{
    if (audioEngine == nullptr)
        return false;

    const int safeStripIndex = juce::jlimit(0, MaxStrips - 1, stripIndex);
    auto* strip = audioEngine->getStrip(safeStripIndex);
    if (strip == nullptr)
        return false;

    const auto safeTarget = sanitizeMacroPerformanceTarget(target);
    const float clamped = juce::jlimit(0.0f, 1.0f, normalizedValue);

    outEvent = {};
    outEvent.type = ScenePerformanceEventType::ControlPoint;
    outEvent.stripIndex = safeStripIndex;
    outEvent.column = juce::jlimit(0, MaxColumns - 1, static_cast<int>(std::round(clamped * 15.0f)));

    switch (safeTarget)
    {
        case MacroTarget::Speed:
            outEvent.controlMode = static_cast<int>(ControlMode::Speed);
            outEvent.controlTarget = ScenePerformanceControlTarget::Speed;
            outEvent.value = denormalizeMacroLinearValue(clamped, 0.125f, 8.0f);
            return true;

        case MacroTarget::Pitch:
            outEvent.controlMode = static_cast<int>(ControlMode::Pitch);
            outEvent.controlTarget = ScenePerformanceControlTarget::Pitch;
            outEvent.value = denormalizeMacroPitchValue(clamped);
            return true;

        case MacroTarget::Pan:
            outEvent.controlMode = static_cast<int>(ControlMode::Pan);
            outEvent.controlTarget = ScenePerformanceControlTarget::Pan;
            outEvent.value = denormalizeMacroLinearValue(clamped, -1.0f, 1.0f);
            return true;

        case MacroTarget::Volume:
            outEvent.controlMode = static_cast<int>(ControlMode::Volume);
            outEvent.controlTarget = ScenePerformanceControlTarget::Volume;
            outEvent.value = clamped;
            return true;

        case MacroTarget::Cutoff:
            outEvent.controlMode = static_cast<int>(ControlMode::Filter);
            outEvent.controlRow = 0;
            outEvent.controlTarget = ScenePerformanceControlTarget::FilterFrequency;
            outEvent.value = denormalizeMacroCutoffValue(clamped);
            return true;

        case MacroTarget::Resonance:
            outEvent.controlMode = static_cast<int>(ControlMode::Filter);
            outEvent.controlRow = 1;
            outEvent.controlTarget = ScenePerformanceControlTarget::FilterResonance;
            outEvent.value = denormalizeMacroLinearValue(clamped, 0.1f, 10.0f);
            return true;

        case MacroTarget::FilterMorph:
            outEvent.controlMode = static_cast<int>(ControlMode::Filter);
            outEvent.controlRow = 2;
            outEvent.controlTarget = ScenePerformanceControlTarget::FilterMorph;
            outEvent.value = clamped;
            return true;

        case MacroTarget::FilterEnable:
            return false;

        case MacroTarget::SliceLength:
            outEvent.controlMode = static_cast<int>(ControlMode::Normal);
            outEvent.controlRow = 0;
            outEvent.controlTarget = ScenePerformanceControlTarget::SliceLength;
            outEvent.value = juce::jlimit(0.02f, 1.0f, clamped);
            return true;

        case MacroTarget::Scratch:
            outEvent.controlMode = static_cast<int>(ControlMode::Normal);
            outEvent.controlRow = 1;
            outEvent.controlTarget = ScenePerformanceControlTarget::Scratch;
            outEvent.value = clamped * 100.0f;
            return true;

        case MacroTarget::DelayMix:
            outEvent.controlMode = static_cast<int>(ControlMode::Delay);
            outEvent.controlRow = 0;
            outEvent.controlTarget = ScenePerformanceControlTarget::DelayMix;
            outEvent.value = clamped;
            return true;

        case MacroTarget::DelayTime:
            outEvent.controlMode = static_cast<int>(ControlMode::Delay);
            outEvent.controlRow = 1;
            outEvent.controlTarget = ScenePerformanceControlTarget::DelayTime;
            outEvent.value = denormalizeMacroLinearValue(clamped, 0.25f, 4.0f);
            return true;

        case MacroTarget::DelayFeedback:
            outEvent.controlMode = static_cast<int>(ControlMode::Delay);
            outEvent.controlRow = 2;
            outEvent.controlTarget = ScenePerformanceControlTarget::DelayFeedback;
            outEvent.value = denormalizeMacroLinearValue(clamped, 0.0f, 0.97f);
            return true;

        case MacroTarget::DelayLowCut:
            outEvent.controlMode = static_cast<int>(ControlMode::Delay);
            outEvent.controlRow = 3;
            outEvent.controlTarget = ScenePerformanceControlTarget::DelayLowCut;
            outEvent.value = denormalizeMacroLinearValue(clamped, 20.0f, 12000.0f);
            return true;

        case MacroTarget::DelayHighCut:
            outEvent.controlMode = static_cast<int>(ControlMode::Delay);
            outEvent.controlRow = 4;
            outEvent.controlTarget = ScenePerformanceControlTarget::DelayHighCut;
            outEvent.value = denormalizeMacroLinearValue(clamped, 200.0f, 20000.0f);
            return true;

        case MacroTarget::GrainSize:
            outEvent.controlMode = static_cast<int>(ControlMode::GrainSize);
            outEvent.controlRow = 0;
            outEvent.controlTarget = ScenePerformanceControlTarget::GrainSize;
            outEvent.value = denormalizeMacroLinearValue(clamped, 5.0f, 2400.0f);
            return true;

        case MacroTarget::GrainDensity:
            outEvent.controlMode = static_cast<int>(ControlMode::GrainSize);
            outEvent.controlRow = 1;
            outEvent.controlTarget = ScenePerformanceControlTarget::GrainDensity;
            outEvent.value = denormalizeMacroLinearValue(clamped, 0.05f, 0.9f);
            return true;

        case MacroTarget::GrainPitch:
            outEvent.controlMode = static_cast<int>(ControlMode::GrainSize);
            outEvent.controlRow = 2;
            outEvent.controlTarget = ScenePerformanceControlTarget::GrainPitch;
            outEvent.value = denormalizeMacroLinearValue(clamped, -48.0f, 48.0f);
            return true;

        case MacroTarget::GrainPitchJitter:
            outEvent.controlMode = static_cast<int>(ControlMode::GrainSize);
            outEvent.controlRow = 3;
            outEvent.controlTarget = ScenePerformanceControlTarget::GrainPitchJitter;
            outEvent.value = denormalizeMacroLinearValue(clamped, 0.0f, 48.0f);
            return true;

        case MacroTarget::GrainSpread:
            outEvent.controlMode = static_cast<int>(ControlMode::GrainSize);
            outEvent.controlRow = 3;
            outEvent.controlTarget = ScenePerformanceControlTarget::GrainSpread;
            outEvent.value = clamped;
            return true;

        case MacroTarget::GrainJitter:
            outEvent.controlMode = static_cast<int>(ControlMode::GrainSize);
            outEvent.controlRow = 4;
            outEvent.controlTarget = ScenePerformanceControlTarget::GrainJitter;
            outEvent.value = clamped;
            return true;

        case MacroTarget::GrainPositionJitter:
            outEvent.controlMode = static_cast<int>(ControlMode::GrainSize);
            outEvent.controlRow = 4;
            outEvent.controlTarget = ScenePerformanceControlTarget::GrainPositionJitter;
            outEvent.value = clamped;
            return true;

        case MacroTarget::GrainRandom:
            outEvent.controlMode = static_cast<int>(ControlMode::GrainSize);
            outEvent.controlRow = 4;
            outEvent.controlTarget = ScenePerformanceControlTarget::GrainRandomDepth;
            outEvent.value = clamped;
            return true;

        case MacroTarget::GrainArp:
            outEvent.controlMode = static_cast<int>(ControlMode::GrainSize);
            outEvent.controlRow = 4;
            outEvent.controlTarget = ScenePerformanceControlTarget::GrainArp;
            outEvent.value = clamped;
            return true;

        case MacroTarget::GrainCloud:
            outEvent.controlMode = static_cast<int>(ControlMode::GrainSize);
            outEvent.controlRow = 4;
            outEvent.controlTarget = ScenePerformanceControlTarget::GrainCloud;
            outEvent.value = clamped;
            return true;

        case MacroTarget::GrainEmitter:
            outEvent.controlMode = static_cast<int>(ControlMode::GrainSize);
            outEvent.controlRow = 4;
            outEvent.controlTarget = ScenePerformanceControlTarget::GrainEmitter;
            outEvent.value = clamped;
            return true;

        case MacroTarget::GrainEnvelope:
            outEvent.controlMode = static_cast<int>(ControlMode::GrainSize);
            outEvent.controlRow = 5;
            outEvent.controlTarget = ScenePerformanceControlTarget::GrainEnvelope;
            outEvent.value = clamped;
            return true;

        case MacroTarget::GrainShape:
            outEvent.controlMode = static_cast<int>(ControlMode::GrainSize);
            outEvent.controlRow = 5;
            outEvent.controlTarget = ScenePerformanceControlTarget::GrainShape;
            outEvent.value = denormalizeMacroLinearValue(clamped, -1.0f, 1.0f);
            return true;

        case MacroTarget::Retrigger:
            outEvent.controlMode = static_cast<int>(ControlMode::Normal);
            outEvent.controlRow = 2;
            outEvent.stripIndex = -1;
            outEvent.controlTarget = ScenePerformanceControlTarget::Retrigger;
            outEvent.value = clamped;
            return true;

        case MacroTarget::Rearrange:
            return false;

        case MacroTarget::None:
        default:
            return false;
    }
}

void MlrVSTAudioProcessor::recordMacroSceneEvent(int stripIndex, MacroTarget target, float normalizedValue)
{
    if (!isSceneModeEnabled() || !scenePerformanceRecorder.isRecording() || audioEngine == nullptr)
        return;

    if (!macroTargetWritesToSceneLane(target))
        return;

    ScenePerformanceEvent event;
    if (!resolveScenePerformanceMacroEvent(stripIndex, target, normalizedValue, event))
        return;

    const int sceneSlot = juce::jlimit(0, SceneSlots - 1, activeSceneSlot);
    scenePerformanceRecorder.recordControlEvent(sceneSlot,
                                                event.stripIndex,
                                                event.controlMode,
                                                event.controlRow,
                                                event.column,
                                                event.controlTarget,
                                                event.value,
                                                audioEngine->getTimelineBeat());
}

namespace
{
float sceneStutterAmountFromDivisionBeats(double divisionBeats)
{
    const double v = juce::jlimit(0.03125, 2.0, divisionBeats);
    if (v >= 1.5) return 0.0625f;       // 1/2
    if (v >= 0.75) return 0.1875f;      // 1/4
    if (v >= 0.375) return 0.3125f;     // 1/8
    if (v >= 0.1875) return 0.4375f;    // 1/16
    if (v >= 0.09375) return 0.625f;    // 1/32
    if (v >= 0.046875) return 0.875f;   // 1/64
    return 1.0f;                        // 1/128 folds into the top lane bucket
}
} // namespace

void MlrVSTAudioProcessor::recordSceneGlobalStutterEvent(float normalizedValue, int columnHint, double eventBeat)
{
    if (!isSceneModeEnabled() || !scenePerformanceRecorder.isRecording())
        return;

    int sceneSlot = scenePerformanceRecorder.getRecordingSceneSlot();
    if (sceneSlot < 0 || sceneSlot >= SceneSlots)
        sceneSlot = juce::jlimit(0, SceneSlots - 1, activeSceneSlot);

    double safeBeat = eventBeat;
    if (!std::isfinite(safeBeat))
        safeBeat = (audioEngine != nullptr) ? audioEngine->getTimelineBeat() : 0.0;

    scenePerformanceRecorder.recordControlEvent(sceneSlot,
                                                -1,
                                                static_cast<int>(ControlMode::Normal),
                                                2,
                                                juce::jlimit(-1, MaxColumns - 1, columnHint),
                                                ScenePerformanceControlTarget::Retrigger,
                                                juce::jlimit(0.0f, 1.0f, normalizedValue),
                                                safeBeat);
}

void MlrVSTAudioProcessor::recordMomentaryStutterSceneDivision(double divisionBeats,
                                                               int columnHint,
                                                               double eventBeat)
{
    recordSceneGlobalStutterEvent(sceneStutterAmountFromDivisionBeats(divisionBeats), columnHint, eventBeat);
}

void MlrVSTAudioProcessor::recordMonomeControlSceneEvent(ControlMode mode,
                                                         int targetStripIndex,
                                                         int controlRow,
                                                         int column)
{
    if (!isSceneModeEnabled() || !scenePerformanceRecorder.isRecording())
        return;

    ScenePerformanceEvent event;
    if (!resolveScenePerformanceControlEvent(mode, targetStripIndex, controlRow, column, event))
        return;

    const int sceneSlot = juce::jlimit(0, SceneSlots - 1, activeSceneSlot);
    scenePerformanceRecorder.recordControlEvent(sceneSlot,
                                                event.stripIndex,
                                                event.controlMode,
                                                event.controlRow,
                                                event.column,
                                                event.controlTarget,
                                                event.value,
                                                audioEngine != nullptr ? audioEngine->getTimelineBeat() : 0.0);
}

void MlrVSTAudioProcessor::playbackScenePerformanceEvent(const ScenePerformanceEvent& event)
{
    if (audioEngine == nullptr)
        return;

    if (event.type == ScenePerformanceEventType::ControlPoint
        && event.controlTarget == ScenePerformanceControlTarget::Retrigger)
    {
        setGlobalSceneStutterAmount(event.value);
        return;
    }

    const int stripIndex = juce::jlimit(0, MaxStrips - 1, event.stripIndex);
    auto* strip = audioEngine->getStrip(stripIndex);
    if (strip == nullptr)
        return;

    if (event.type == ScenePerformanceEventType::Trigger)
    {
        if (event.isNoteOn)
        {
            audioEngine->triggerStripWithQuantization(stripIndex,
                                                      juce::jlimit(0, MaxColumns - 1, event.column),
                                                      false,
                                                      event.sampleSliceId,
                                                      event.sampleStartSample);
        }
        else if (strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Sample)
        {
            stopSampleModeStrip(stripIndex, false);
        }
        else
        {
            strip->stop(false);
        }
        return;
    }

    switch (event.controlTarget)
    {
        case ScenePerformanceControlTarget::Speed:
            setStripSpeedControlValue(stripIndex, event.value, StripControlWriteMode::CacheOnly);
            break;
        case ScenePerformanceControlTarget::Pitch:
            if (strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Grain)
                strip->setGrainPitch(event.value);
            else
                applyUserPitchControlToStrip(stripIndex, event.value);
            break;
        case ScenePerformanceControlTarget::Pan:
            setStripPanControlValue(stripIndex, event.value, StripControlWriteMode::CacheOnly);
            break;
        case ScenePerformanceControlTarget::Volume:
            setStripVolumeControlValue(stripIndex, event.value, StripControlWriteMode::CacheOnly);
            break;
        case ScenePerformanceControlTarget::Swing:
            strip->setSwingAmount(juce::jlimit(0.0f, 1.0f, event.value));
            break;
        case ScenePerformanceControlTarget::GrainSize:
            strip->setGrainSizeMs(event.value);
            break;
        case ScenePerformanceControlTarget::GrainDensity:
            strip->setGrainDensity(event.value);
            break;
        case ScenePerformanceControlTarget::GrainPitch:
            strip->setGrainPitch(event.value);
            break;
        case ScenePerformanceControlTarget::GrainPitchJitter:
            strip->setGrainPitchJitter(juce::jlimit(0.0f, 48.0f, event.value));
            break;
        case ScenePerformanceControlTarget::GrainSpread:
            strip->setGrainSpread(juce::jlimit(0.0f, 1.0f, event.value));
            break;
        case ScenePerformanceControlTarget::GrainJitter:
            strip->setGrainJitter(juce::jlimit(0.0f, 1.0f, event.value));
            break;
        case ScenePerformanceControlTarget::GrainPositionJitter:
            strip->setGrainPositionJitter(juce::jlimit(0.0f, 1.0f, event.value));
            break;
        case ScenePerformanceControlTarget::GrainRandomDepth:
            strip->setGrainRandomDepth(juce::jlimit(0.0f, 1.0f, event.value));
            break;
        case ScenePerformanceControlTarget::GrainArp:
            strip->setGrainArpDepth(juce::jlimit(0.0f, 1.0f, event.value));
            break;
        case ScenePerformanceControlTarget::GrainCloud:
            strip->setGrainCloudDepth(juce::jlimit(0.0f, 1.0f, event.value));
            break;
        case ScenePerformanceControlTarget::GrainEmitter:
            strip->setGrainEmitterDepth(juce::jlimit(0.0f, 1.0f, event.value));
            break;
        case ScenePerformanceControlTarget::GrainEnvelope:
            strip->setGrainEnvelope(juce::jlimit(0.0f, 1.0f, event.value));
            break;
        case ScenePerformanceControlTarget::GrainShape:
            strip->setGrainShape(juce::jlimit(-1.0f, 1.0f, event.value));
            break;
        case ScenePerformanceControlTarget::FilterFrequency:
            setStripFilterFrequencyControlValue(stripIndex, event.value, StripControlWriteMode::CacheOnly);
            break;
        case ScenePerformanceControlTarget::FilterResonance:
            setStripFilterResonanceControlValue(stripIndex, event.value, StripControlWriteMode::CacheOnly);
            break;
        case ScenePerformanceControlTarget::FilterEnabled:
            setStripFilterEnabledControlValue(stripIndex,
                                              event.value >= 0.5f,
                                              StripControlWriteMode::CacheOnly);
            break;
        case ScenePerformanceControlTarget::FilterMorph:
            setStripFilterMorphControlValue(stripIndex, event.value, StripControlWriteMode::CacheOnly);
            break;
        case ScenePerformanceControlTarget::SliceLength:
            writeStripFloatParameter("stripSliceLength" + juce::String(stripIndex),
                                     juce::jlimit(0.02f, 1.0f, event.value),
                                     stripSliceLengthParams[static_cast<size_t>(stripIndex)],
                                     StripControlWriteMode::CacheOnly);
            strip->setLoopSliceLength(event.value);
            break;
        case ScenePerformanceControlTarget::Scratch:
            strip->setScratchAmount(juce::jlimit(0.0f, 100.0f, event.value));
            break;
        case ScenePerformanceControlTarget::DelayMix:
            setStripDelayMixControlValue(stripIndex, event.value, StripControlWriteMode::CacheOnly);
            break;
        case ScenePerformanceControlTarget::DelayTime:
            setStripDelayTimeControlValue(stripIndex, event.value, StripControlWriteMode::CacheOnly);
            break;
        case ScenePerformanceControlTarget::DelayFeedback:
            setStripDelayFeedbackControlValue(stripIndex, event.value, StripControlWriteMode::CacheOnly);
            break;
        case ScenePerformanceControlTarget::DelayLowCut:
            setStripDelayLowCutControlValue(stripIndex, event.value, StripControlWriteMode::CacheOnly);
            break;
        case ScenePerformanceControlTarget::DelayHighCut:
            setStripDelayHighCutControlValue(stripIndex, event.value, StripControlWriteMode::CacheOnly);
            break;
        case ScenePerformanceControlTarget::DelayMode:
            setStripDelayModeControlValue(stripIndex,
                                          static_cast<EnhancedAudioStrip::DelayMode>(
                                              juce::jlimit(0, 2, static_cast<int>(std::round(event.value)))),
                                          StripControlWriteMode::CacheOnly);
            break;
        case ScenePerformanceControlTarget::DelaySyncEnabled:
            setStripDelaySyncEnabledControlValue(stripIndex,
                                                 event.value >= 0.5f,
                                                 StripControlWriteMode::CacheOnly);
            break;
        case ScenePerformanceControlTarget::Retrigger:
            audioEngine->setMacroRetriggerAmount(stripIndex, juce::jlimit(0.0f, 1.0f, event.value));
            break;
        case ScenePerformanceControlTarget::Rearrange:
            break;
        case ScenePerformanceControlTarget::None:
        default:
            break;
    }
}

void MlrVSTAudioProcessor::processScenePerformancePlayback(const juce::AudioPlayHead::PositionInfo& posInfo,
                                                           int numSamples)
{
    if (!isSceneModeEnabled() || audioEngine == nullptr)
    {
        lastScenePerformanceProcessBeat = std::numeric_limits<double>::quiet_NaN();
        lastScenePerformanceProcessSceneSlot = -1;
        lastScenePerformanceProcessSceneStartBeat = std::numeric_limits<double>::quiet_NaN();
        return;
    }

    const int sceneSlot = juce::jlimit(0, SceneSlots - 1, activeSceneSlot);
    if (!activeSceneStartPpqValid || !std::isfinite(activeSceneStartPpq))
    {
        lastScenePerformanceProcessBeat = std::numeric_limits<double>::quiet_NaN();
        lastScenePerformanceProcessSceneSlot = sceneSlot;
        lastScenePerformanceProcessSceneStartBeat = activeSceneStartPpq;
        return;
    }

    const double blockStartBeat = posInfo.getPpqPosition().hasValue()
        ? *posInfo.getPpqPosition()
        : audioEngine->getTimelineBeat();
    const int64_t blockStartSample = audioEngine->getGlobalSampleCount();
    const double bpm = posInfo.getBpm().hasValue()
        ? juce::jmax(1.0, *posInfo.getBpm())
        : juce::jmax(1.0, audioEngine->getCurrentTempo());
    const double beatDeltaPerSample = bpm / (60.0 * juce::jmax(1.0, currentSampleRate));
    const double samplesPerBeat = 1.0 / beatDeltaPerSample;
    const double blockEndBeat = blockStartBeat + (beatDeltaPerSample * static_cast<double>(juce::jmax(0, numSamples)));

    scenePerformanceRecorder.updateRecording(blockEndBeat);

    if (!scenePerformanceRecorder.hasEvents(sceneSlot))
    {
        lastScenePerformanceProcessBeat = blockEndBeat;
        lastScenePerformanceProcessSceneSlot = sceneSlot;
        lastScenePerformanceProcessSceneStartBeat = activeSceneStartPpq;
        return;
    }

    const bool sceneChanged = sceneSlot != lastScenePerformanceProcessSceneSlot
        || !std::isfinite(lastScenePerformanceProcessSceneStartBeat)
        || std::abs(lastScenePerformanceProcessSceneStartBeat - activeSceneStartPpq) > 1.0e-9;

    double fromBeat = sceneChanged ? juce::jmax(blockStartBeat, activeSceneStartPpq) : lastScenePerformanceProcessBeat;
    if (!std::isfinite(fromBeat))
        fromBeat = blockStartBeat;

    if (fromBeat < blockStartBeat - 1.0 || fromBeat > blockEndBeat + 1.0)
        fromBeat = blockStartBeat;

    const double clipLengthBeats = juce::jmax(1.0, scenePerformanceRecorder.getClipLengthBeats(sceneSlot));
    const double beatDelta = blockEndBeat - fromBeat;
    if (beatDelta > 0.0 && beatDelta <= clipLengthBeats)
    {
        scenePerformanceRecorder.processEventsForBeatWindow(sceneSlot,
                                                            activeSceneStartPpq,
                                                            fromBeat,
                                                            blockEndBeat,
                                                            [this, blockStartBeat, blockStartSample, numSamples, samplesPerBeat]
                                                            (const ScenePerformanceEvent& event, double occurrenceBeat)
                                                            {
                                                                if (event.type == ScenePerformanceEventType::Trigger
                                                                    && event.isNoteOn
                                                                    && std::isfinite(occurrenceBeat))
                                                                {
                                                                    const int maxOffset = juce::jmax(0, numSamples - 1);
                                                                    const int offsetSamples = juce::jlimit(
                                                                        0,
                                                                        maxOffset,
                                                                        static_cast<int>(std::llround(
                                                                            (occurrenceBeat - blockStartBeat) * samplesPerBeat)));
                                                                    audioEngine->queueExactTrigger(event.stripIndex,
                                                                                                  juce::jlimit(0, MaxColumns - 1, event.column),
                                                                                                  occurrenceBeat,
                                                                                                  blockStartSample + offsetSamples,
                                                                                                  event.sampleSliceId,
                                                                                                  event.sampleStartSample);
                                                                    return;
                                                                }

                                                                playbackScenePerformanceEvent(event);
                                                            });
    }

    lastScenePerformanceProcessBeat = blockEndBeat;
    lastScenePerformanceProcessSceneSlot = sceneSlot;
    lastScenePerformanceProcessSceneStartBeat = activeSceneStartPpq;
}

void MlrVSTAudioProcessor::renderActiveSceneAudio(juce::AudioBuffer<float>& buffer,
                                                  juce::MidiBuffer& midiMessages,
                                                  const juce::AudioPlayHead::PositionInfo& posInfo,
                                                  bool allowSeparateStripRouting,
                                                  bool applySceneTransitionOverlay)
{
    if (audioEngine == nullptr)
    {
        buffer.clear();
        return;
    }

    if (masterVolumeParam)
        audioEngine->setMasterVolume(*masterVolumeParam);
    if (limiterThresholdParam)
        audioEngine->setLimiterThresholdDb(limiterThresholdParam->load(std::memory_order_acquire));
    if (limiterEnabledParam)
        audioEngine->setLimiterEnabled(limiterEnabledParam->load(std::memory_order_acquire) > 0.5f);
    if (quantizeParam)
    {
        const int quantizeChoice = static_cast<int>(*quantizeParam);
        const int divisionMap[] = {1, 2, 3, 4, 6, 8, 12, 16, 24, 32};
        const int division = (quantizeChoice >= 0 && quantizeChoice < 10) ? divisionMap[quantizeChoice] : 8;
        audioEngine->setQuantization(division);
    }
    if (pitchSmoothingParam)
        audioEngine->setPitchSmoothingTime(*pitchSmoothingParam);
    if (inputMonitorParam)
        audioEngine->setInputMonitorVolume(*inputMonitorParam);
    if (crossfadeLengthParam)
        audioEngine->setCrossfadeLengthMs(*crossfadeLengthParam);
    if (triggerFadeInParam)
        audioEngine->setTriggerFadeInMs(*triggerFadeInParam);
    audioEngine->setGlobalStretchBackend(getStretchBackend());
    audioEngine->setGlobalContinuousTraversal(usesContinuousTraversal());
    syncTransientDetectionSettingsFromParameters(false);
    audioEngine->setGlobalTempoMatchBackend(getLoopTempoMatchBackend());
    if (masterDuckTriggerStripParam)
    {
        const int triggerChoice = juce::jlimit(0,
                                               MaxStrips,
                                               static_cast<int>(masterDuckTriggerStripParam->load(std::memory_order_acquire)));
        audioEngine->setMasterDuckTriggerStrip(triggerChoice - 1);
    }

    applyMomentaryStutterMacro(posInfo);
    processScenePerformancePlayback(posInfo, buffer.getNumSamples());
    updateAudioEngineSceneModulationContext();
    const auto transitionOverlayType = sanitizeRuntimeSceneChainTransitionType(
        sceneBoundaryTransitionType.load(std::memory_order_acquire));
    const bool shouldRestoreTransitionOverlay = applySceneTransitionOverlay
        && transitionOverlayType != SceneChainTransitionType::None
        && transitionOverlayType != SceneChainTransitionType::Return;
    std::array<StripControlState::ResolvedOwnedStripControlState, MaxStrips> transitionOverlayOriginalStates{};
    std::array<bool, MaxStrips> transitionOverlayOriginalStateValid{};
    if (shouldRestoreTransitionOverlay)
    {
        const StripControlState::ParameterView emptyView;
        for (int stripIndex = 0; stripIndex < MaxStrips; ++stripIndex)
        {
            auto* strip = audioEngine->getStrip(stripIndex);
            if (strip == nullptr)
                continue;

            const bool stepMode = strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Step;
            auto* stepSampler = stepMode ? strip->getStepSampler() : nullptr;
            const bool hasPlayableContent = strip->hasAudio()
                || (stepSampler != nullptr && stepSampler->getHasAudio());
            if (!hasPlayableContent)
                continue;

            transitionOverlayOriginalStates[static_cast<size_t>(stripIndex)] =
                StripControlState::resolveFromParameters(emptyView, *strip);
            transitionOverlayOriginalStateValid[static_cast<size_t>(stripIndex)] = true;
        }

        applySceneBoundaryTransitionOverlay(posInfo, buffer.getNumSamples());
    }

    auto restoreSceneTransitionOverlay = [this,
                                          &transitionOverlayOriginalStates,
                                          &transitionOverlayOriginalStateValid]()
    {
        for (int stripIndex = 0; stripIndex < MaxStrips; ++stripIndex)
        {
            if (!transitionOverlayOriginalStateValid[static_cast<size_t>(stripIndex)])
                continue;

            auto* strip = audioEngine != nullptr ? audioEngine->getStrip(stripIndex) : nullptr;
            if (strip == nullptr)
                continue;

            const auto& originalState = transitionOverlayOriginalStates[static_cast<size_t>(stripIndex)];
            applyResolvedOwnedStripControlState(*strip, originalState);
            applyResolvedStripFilterState(*strip, originalState);
            applyResolvedStripDelayState(*strip, originalState);
        }
    };

    const bool separateStripRouting = allowSeparateStripRouting
        && (outputRoutingParam != nullptr && *outputRoutingParam > 0.5f)
        && getBusCount(false) > 1;
    if (separateStripRouting)
    {
        std::array<std::array<float*, 2>, MaxStrips> stripBusChannels{};
        std::array<juce::AudioBuffer<float>, MaxStrips> stripBusViews;
        std::array<juce::AudioBuffer<float>*, MaxStrips> stripBusTargets{};
        stripBusTargets.fill(nullptr);

        for (int stripIndex = 0; stripIndex < MaxStrips; ++stripIndex)
        {
            const int busIndex = stripIndex;
            if (busIndex >= getBusCount(false))
                continue;

            auto busBuffer = getBusBuffer(buffer, false, busIndex);
            if (busBuffer.getNumChannels() <= 0 || busBuffer.getNumSamples() <= 0)
                continue;

            auto& channelPtrs = stripBusChannels[static_cast<size_t>(stripIndex)];
            channelPtrs.fill(nullptr);
            channelPtrs[0] = busBuffer.getWritePointer(0);
            channelPtrs[1] = busBuffer.getNumChannels() > 1
                ? busBuffer.getWritePointer(1)
                : busBuffer.getWritePointer(0);

            stripBusViews[static_cast<size_t>(stripIndex)].setDataToReferTo(
                channelPtrs.data(), 2, busBuffer.getNumSamples());
            stripBusTargets[static_cast<size_t>(stripIndex)] =
                &stripBusViews[static_cast<size_t>(stripIndex)];
        }

        auto* mainTarget = stripBusTargets[0];
        if (mainTarget == nullptr)
            mainTarget = &buffer;
        for (int stripIndex = 0; stripIndex < MaxStrips; ++stripIndex)
        {
            if (stripBusTargets[static_cast<size_t>(stripIndex)] == nullptr)
                stripBusTargets[static_cast<size_t>(stripIndex)] = mainTarget;
        }

        audioEngine->processBlock(buffer, midiMessages, posInfo, &stripBusTargets);
        if (shouldRestoreTransitionOverlay)
            restoreSceneTransitionOverlay();
        return;
    }

    audioEngine->processBlock(buffer, midiMessages, posInfo, nullptr);
    if (shouldRestoreTransitionOverlay)
        restoreSceneTransitionOverlay();
}

bool MlrVSTAudioProcessor::tryStartPreloadedSceneTransition(int mainPresetIndex,
                                                            int sceneSlot,
                                                            bool sequenceDriven,
                                                            int sequenceStepIndex,
                                                            double targetPpq,
                                                            double targetTempo,
                                                            int64_t targetSample,
                                                            int sampleOffset,
                                                            SceneChainTransitionType transitionType)
{
    const bool separateStripRouting = outputRoutingParam != nullptr && *outputRoutingParam > 0.5f;
    if (separateStripRouting || preparedSceneRenderContext == nullptr || activeSceneTransitionContext != nullptr)
        return false;

    auto matchesPrepared = [&](const SceneRenderContext& context) -> bool
    {
        const bool sampleMatches =
            ((context.sceneStartSample < 0 && targetSample < 0)
             || (context.sceneStartSample >= 0
                 && targetSample >= 0
                 && (context.sceneStartSample - targetSample) >= -64
                 && (context.sceneStartSample - targetSample) <= 64));
        return context.mainPresetIndex == mainPresetIndex
            && context.sceneSlot == sceneSlot
            && context.sequenceDriven == sequenceDriven
            && context.sequenceStepIndex == sequenceStepIndex
            && context.transitionType == transitionType
            && sampleMatches
            && ((!std::isfinite(targetPpq) && !context.sceneStartPpqValid)
                || (context.sceneStartPpqValid && std::abs(context.sceneStartPpq - targetPpq) <= 1.0e-9));
    };

    if (!matchesPrepared(*preparedSceneRenderContext))
        return false;

    scenePerformanceRecorder.stopRecording();
    clearPendingMonomeSceneRecorderTap();
    clearPendingSceneRecorderAction();

    auto context = std::move(preparedSceneRenderContext);
    context->sceneStartPpqValid = std::isfinite(targetPpq);
    context->sceneStartPpq = context->sceneStartPpqValid ? targetPpq : 0.0;
    context->sceneTempo = (std::isfinite(targetTempo) && targetTempo > 0.0) ? targetTempo : 120.0;
    context->sceneStartSample = targetSample;
    context->transitionType = transitionType;
    if (context->engine != nullptr)
        context->engine->seedTransportState(targetSample >= 0 ? targetSample : 0, targetPpq);

    if (context->sceneTimingStateXml != nullptr)
        applySceneChainStateXml(context->sceneTimingStateXml.get(), context->sceneSlot);
    if (context->scenePerformanceStateData.getSize() > 0)
        applyScenePerformanceStateData(context->scenePerformanceStateData, context->sceneSlot);

    const double effectiveRate = currentSampleRate > 0.0 ? currentSampleRate : 44100.0;
    const float transitionLengthBeats = sanitizeRuntimeSceneChainTransitionLengthBeats(
        sceneBoundaryTransitionLengthBeats.load(std::memory_order_acquire));
    const float transitionIntensity = sanitizeRuntimeSceneChainTransitionAmount(
        sceneBoundaryTransitionIntensity.load(std::memory_order_acquire),
        DefaultSceneTransitionIntensity);
    const float triggerFadeMs = triggerFadeInParam != nullptr
        ? juce::jlimit(0.01f, 120.0f, triggerFadeInParam->load(std::memory_order_acquire))
        : 12.0f;
    const double transitionSeconds = juce::jmax(sceneChainTransitionCrossfadeSeconds(
        transitionType,
        transitionLengthBeats,
        transitionIntensity,
        context->sceneTempo),
        static_cast<double>(triggerFadeMs) * 0.001);
    const int transitionSamples = juce::jmax(32, static_cast<int>(std::round(effectiveRate * transitionSeconds)));
    const int maxTransitionSamples = juce::jmax(8192, static_cast<int>(std::round(effectiveRate * 6.0)));

    activeSceneTransitionContext = std::move(context);
    sceneTransitionSamplesTotal = juce::jlimit(32, maxTransitionSamples, transitionSamples);
    sceneTransitionSamplesRendered = 0;
    sceneTransitionStartSampleOffset = juce::jlimit(0, juce::jmax(0, getBlockSize() - 1), sampleOffset);
    sceneTransitionCommitPending = false;
    sceneRecallBlendSamplesRemaining = 0;
    sceneRecallBlendTotalSamples = 0;
    sceneRecallBlendStartTailLength = 0;
    return activeSceneTransitionContext != nullptr;
}

void MlrVSTAudioProcessor::renderSceneTransitionBlock(juce::AudioBuffer<float>& buffer,
                                                      juce::MidiBuffer& midiMessages,
                                                      const juce::AudioPlayHead::PositionInfo& posInfo)
{
    if (activeSceneTransitionContext == nullptr || audioEngine == nullptr)
    {
        renderActiveSceneAudio(buffer, midiMessages, posInfo, true, true);
        return;
    }

    const int numChannels = juce::jmax(1, buffer.getNumChannels());
    const int numSamples = juce::jmax(0, buffer.getNumSamples());
    sceneTransitionOutgoingScratch.setSize(numChannels, numSamples, false, false, true);
    sceneTransitionIncomingScratch.setSize(numChannels, numSamples, false, false, true);
    sceneTransitionOutgoingScratch.clear();
    sceneTransitionIncomingScratch.clear();

    juce::MidiBuffer outgoingMidi(midiMessages);
    renderActiveSceneAudio(sceneTransitionOutgoingScratch, outgoingMidi, posInfo, false, true);

    auto renderIncomingContext = [this, &posInfo](SceneRenderContext& context,
                                                  juce::AudioBuffer<float>& destination,
                                                  int startOffset)
    {
        if (context.engine == nullptr || startOffset >= destination.getNumSamples())
            return;

        juce::AudioPlayHead::PositionInfo adjustedPos = posInfo;
        if (startOffset > 0
            && posInfo.getPpqPosition().hasValue()
            && posInfo.getBpm().hasValue()
            && *posInfo.getBpm() > 0.0
            && currentSampleRate > 0.0)
        {
            const double ppqAdvance = (static_cast<double>(startOffset) * *posInfo.getBpm())
                / (60.0 * currentSampleRate);
            adjustedPos.setPpqPosition(*posInfo.getPpqPosition() + ppqAdvance);
        }

        std::array<float*, 2> subViewChannels{};
        juce::AudioBuffer<float> subView;
        juce::AudioBuffer<float>* targetBuffer = &destination;
        if (startOffset > 0)
        {
            subViewChannels[0] = destination.getWritePointer(0, startOffset);
            subViewChannels[1] = destination.getNumChannels() > 1
                ? destination.getWritePointer(1, startOffset)
                : destination.getWritePointer(0, startOffset);
            subView.setDataToReferTo(subViewChannels.data(), 2, destination.getNumSamples() - startOffset);
            targetBuffer = &subView;
        }

        std::swap(audioEngine, context.engine);
        std::swap(sampleModeEngines, context.sampleModeEngines);
        std::swap(sampleModeScratchBuffers, context.sampleModeScratchBuffers);
        std::swap(sampleModeRenderedLastBlock, context.sampleModeRenderedLastBlock);
        std::swap(currentStripFiles, context.currentStripFiles);

        const int savedMainPreset = activeSceneMainPresetIndex;
        const int savedSceneSlot = activeSceneSlot;
        const bool savedSceneStartValid = activeSceneStartPpqValid;
        const double savedSceneStartPpq = activeSceneStartPpq;
        const double savedProcessBeat = lastScenePerformanceProcessBeat;
        const int savedProcessSceneSlot = lastScenePerformanceProcessSceneSlot;
        const double savedProcessSceneStartBeat = lastScenePerformanceProcessSceneStartBeat;

        activeSceneMainPresetIndex = context.mainPresetIndex;
        activeSceneSlot = context.sceneSlot;
        activeSceneStartPpqValid = context.sceneStartPpqValid;
        activeSceneStartPpq = context.sceneStartPpq;
        lastScenePerformanceProcessBeat = context.lastScenePerformanceProcessBeat;
        lastScenePerformanceProcessSceneSlot = context.lastScenePerformanceProcessSceneSlot;
        lastScenePerformanceProcessSceneStartBeat = context.lastScenePerformanceProcessSceneStartBeat;

        const int previousSuppress = suppressOwnedStripParameterSync.exchange(1, std::memory_order_acq_rel);
        if (sceneSlotHasMotionState(activeSceneSlot))
            applySceneMotionStateToEngine(activeSceneSlot);
        else
            syncSceneMotionStateFromEngine(activeSceneSlot);

        juce::MidiBuffer incomingMidi;
        renderActiveSceneAudio(*targetBuffer, incomingMidi, adjustedPos, false, false);

        context.lastScenePerformanceProcessBeat = lastScenePerformanceProcessBeat;
        context.lastScenePerformanceProcessSceneSlot = lastScenePerformanceProcessSceneSlot;
        context.lastScenePerformanceProcessSceneStartBeat = lastScenePerformanceProcessSceneStartBeat;

        suppressOwnedStripParameterSync.store(previousSuppress, std::memory_order_release);
        activeSceneMainPresetIndex = savedMainPreset;
        activeSceneSlot = savedSceneSlot;
        activeSceneStartPpqValid = savedSceneStartValid;
        activeSceneStartPpq = savedSceneStartPpq;
        lastScenePerformanceProcessBeat = savedProcessBeat;
        lastScenePerformanceProcessSceneSlot = savedProcessSceneSlot;
        lastScenePerformanceProcessSceneStartBeat = savedProcessSceneStartBeat;

        std::swap(currentStripFiles, context.currentStripFiles);
        std::swap(sampleModeRenderedLastBlock, context.sampleModeRenderedLastBlock);
        std::swap(sampleModeScratchBuffers, context.sampleModeScratchBuffers);
        std::swap(sampleModeEngines, context.sampleModeEngines);
        std::swap(audioEngine, context.engine);
    };

    const int incomingStartOffset = sceneTransitionSamplesRendered == 0 ? sceneTransitionStartSampleOffset : 0;
    renderIncomingContext(*activeSceneTransitionContext, sceneTransitionIncomingScratch, incomingStartOffset);

    auto evaluateChipGains = [](SceneChainTransitionType type,
                                float progress,
                                float& outgoingGain,
                                float& incomingGain)
    {
        const float clamped = juce::jlimit(0.0f, 1.0f, progress);
        const float eased = smoothSceneTransitionProgress(clamped);
        outgoingGain = std::cos(eased * juce::MathConstants<float>::halfPi);
        incomingGain = std::sin(eased * juce::MathConstants<float>::halfPi);

        switch (type)
        {
            case SceneChainTransitionType::Fill:
                outgoingGain *= 0.92f + (0.10f * (1.0f - eased));
                incomingGain = std::pow(incomingGain, 0.82f);
                break;
            case SceneChainTransitionType::Stutter:
            {
                const float pulse = (std::fmod(clamped * 18.0f, 1.0f) < 0.5f) ? 0.62f : 1.0f;
                outgoingGain *= pulse;
                incomingGain = std::pow(incomingGain, 1.15f);
                break;
            }
            case SceneChainTransitionType::FilterRise:
                outgoingGain = std::pow(outgoingGain, 0.72f);
                incomingGain = std::pow(incomingGain, 1.08f);
                break;
            case SceneChainTransitionType::Drop:
                if (clamped < 0.50f)
                    incomingGain = 0.0f;
                else
                    incomingGain = std::pow(juce::jlimit(0.0f, 1.0f, (clamped - 0.50f) / 0.50f), 0.65f);
                outgoingGain *= (clamped < 0.60f ? 1.0f : juce::jlimit(0.0f, 1.0f, (1.0f - clamped) / 0.40f));
                break;
            case SceneChainTransitionType::MuteTail:
                outgoingGain = std::pow(outgoingGain, 1.7f);
                incomingGain = std::pow(incomingGain, 0.9f);
                break;
            case SceneChainTransitionType::Break:
            {
                const float fadeOutEnd = 0.34f;
                const float fadeInStart = 0.64f;
                if (clamped < fadeOutEnd)
                {
                    const float local = juce::jlimit(0.0f, 1.0f, clamped / fadeOutEnd);
                    outgoingGain = std::cos(local * juce::MathConstants<float>::halfPi);
                    incomingGain = 0.0f;
                }
                else if (clamped < fadeInStart)
                {
                    outgoingGain = 0.0f;
                    incomingGain = 0.0f;
                }
                else
                {
                    const float local = juce::jlimit(0.0f, 1.0f, (clamped - fadeInStart) / (1.0f - fadeInStart));
                    outgoingGain = 0.0f;
                    incomingGain = std::sin(local * juce::MathConstants<float>::halfPi);
                }
                break;
            }
            case SceneChainTransitionType::None:
            case SceneChainTransitionType::Return:
            default:
                break;
        }
    };

    buffer.makeCopyOf(sceneTransitionOutgoingScratch, true);
    int samplesConsumed = 0;
    const auto transitionType = activeSceneTransitionContext->transitionType;
    const int totalTransitionSamples = juce::jmax(1, sceneTransitionSamplesTotal);
    for (int sample = 0; sample < numSamples; ++sample)
    {
        const bool beforeIncomingStart = (sceneTransitionSamplesRendered == 0 && sample < incomingStartOffset);
        if (beforeIncomingStart)
            continue;

        const int transitionSampleIndex = juce::jmax(0, sceneTransitionSamplesRendered + samplesConsumed);
        if (transitionSampleIndex >= totalTransitionSamples)
        {
            for (int channel = 0; channel < numChannels; ++channel)
                buffer.setSample(channel, sample, sceneTransitionIncomingScratch.getSample(channel, sample));
            continue;
        }

        const float progress = totalTransitionSamples > 1
            ? static_cast<float>(transitionSampleIndex) / static_cast<float>(totalTransitionSamples - 1)
            : 1.0f;
        float outgoingGain = 1.0f;
        float incomingGain = 1.0f;
        evaluateChipGains(transitionType, progress, outgoingGain, incomingGain);

        for (int channel = 0; channel < numChannels; ++channel)
        {
            const float outgoingSample = sceneTransitionOutgoingScratch.getSample(channel, sample);
            const float incomingSample = sceneTransitionIncomingScratch.getSample(channel, sample);
            buffer.setSample(channel,
                             sample,
                             (outgoingSample * outgoingGain) + (incomingSample * incomingGain));
        }

        ++samplesConsumed;
    }

    sceneTransitionSamplesRendered = juce::jmin(totalTransitionSamples,
                                                sceneTransitionSamplesRendered + samplesConsumed);
    sceneTransitionStartSampleOffset = 0;
    sceneTransitionCommitPending = sceneTransitionSamplesRendered >= totalTransitionSamples;
}

void MlrVSTAudioProcessor::applySceneRenderContext(SceneRenderContext context)
{
    if (context.engine == nullptr)
        return;

    for (auto& inFlight : loopStripLoadInFlight)
        inFlight.store(0, std::memory_order_release);
    for (auto& requestId : loopStripLoadRequestIds)
        requestId.store(0, std::memory_order_release);
    for (auto& progress : loopStripLoadProgressPermille)
        progress.store(0, std::memory_order_release);
    pendingLoopStripFiles.fill({});
    loopStripLoadStatusTexts.fill({});

    for (auto& inFlight : loopPitchAnalysisInFlight)
        inFlight.store(0, std::memory_order_release);
    for (auto& requestId : loopPitchAnalysisRequestIds)
        requestId.store(0, std::memory_order_release);
    for (auto& progress : loopPitchAnalysisProgressPermille)
        progress.store(0, std::memory_order_release);
    loopPitchAnalysisStatusTexts.fill({});

    audioEngine = std::move(context.engine);
    configureAudioEngineCallbacks(*audioEngine);
    sampleModeEngines = std::move(context.sampleModeEngines);
    sampleModeScratchBuffers = std::move(context.sampleModeScratchBuffers);
    sampleModeRenderedLastBlock = context.sampleModeRenderedLastBlock;
    currentStripFiles = std::move(context.currentStripFiles);
    recentLoopDirectories = std::move(context.recentLoopDirectories);
    recentStepDirectories = std::move(context.recentStepDirectories);
    recentFlipDirectories = std::move(context.recentFlipDirectories);

    activeSceneMainPresetIndex = context.mainPresetIndex;
    activeSceneSlot = context.sceneSlot;
    activeSceneNeedsCaptureBeforeManualRecall = false;
    activeSceneStartPpqValid = context.sceneStartPpqValid;
    activeSceneStartPpq = context.sceneStartPpq;
    lastScenePerformanceProcessBeat = context.lastScenePerformanceProcessBeat;
    lastScenePerformanceProcessSceneSlot = context.lastScenePerformanceProcessSceneSlot;
    lastScenePerformanceProcessSceneStartBeat = context.lastScenePerformanceProcessSceneStartBeat;

    if (context.sceneTimingStateXml != nullptr)
        applySceneChainStateXml(context.sceneTimingStateXml.get(), context.sceneSlot);
    if (context.scenePerformanceStateData.getSize() > 0)
        applyScenePerformanceStateData(context.scenePerformanceStateData, context.sceneSlot);
    for (int stripIndex = 0; stripIndex < MaxStrips; ++stripIndex)
    {
        if (context.loopPitchStateXml[static_cast<size_t>(stripIndex)] != nullptr)
            applyLoopPitchPresetStateXml(stripIndex,
                                         context.loopPitchStateXml[static_cast<size_t>(stripIndex)].get());
    }
    resolveLoopPitchRecallStateImmediately();

    if (context.sequenceDriven && getSceneChainLength() >= 2)
    {
        sceneSequenceActive = true;
        sceneSequenceCurrentStepIndex = juce::jlimit(0,
                                                     getSceneChainLength() - 1,
                                                     juce::jmax(0, context.sequenceStepIndex));
    }
    else
    {
        sceneSequenceActive = false;
        sceneSequenceCurrentStepIndex = -1;
    }

    if (isSceneModeEnabled() && audioEngine != nullptr)
    {
        if (sceneSlotHasMotionState(activeSceneSlot))
            applySceneMotionStateToEngine(activeSceneSlot);
        else
            syncSceneMotionStateFromEngine(activeSceneSlot);
    }

    if (PresetStore::presetExists(activeSceneMainPresetIndex))
        loadedPresetIndex = activeSceneMainPresetIndex;
    if (context.sequenceDriven)
        armNextSceneInSequence(activeSceneMainPresetIndex, activeSceneSlot, activeSceneStartPpq);
    else
        sceneSequenceStartPpqValid = false;

    if (context.parameterState.isValid())
    {
        suppressOwnedStripParameterSync.store(1, std::memory_order_release);
        queuePendingSceneParameterState(context.parameterState);
    }
    else
    {
        suppressOwnedStripParameterSync.store(0, std::memory_order_release);
    }

    sceneTransitionSamplesTotal = 0;
    sceneTransitionSamplesRendered = 0;
    sceneTransitionStartSampleOffset = 0;
    sceneTransitionCommitPending = false;
    presetRefreshToken.fetch_add(1, std::memory_order_acq_rel);
    updateMonomeLEDs();
}

void MlrVSTAudioProcessor::commitSceneTransitionIfReady()
{
    if (!sceneTransitionCommitPending || activeSceneTransitionContext == nullptr)
        return;

    clearSceneBoundaryTransitionState();

    auto context = std::move(activeSceneTransitionContext);
    applySceneRenderContext(std::move(*context));
}

void MlrVSTAudioProcessor::applyQueuedSceneExitTailFade(juce::AudioBuffer<float>& buffer, int64_t blockStartSample)
{
    if (buffer.getNumSamples() <= 0 || blockStartSample < 0)
        return;

    if (activeSceneTransitionContext != nullptr)
        return;

    if (pendingSceneApplySlot.load(std::memory_order_acquire) < 0
        || pendingSceneApplySequenceDriven.load(std::memory_order_acquire) == 0)
    {
        return;
    }

    const int64_t targetSample = pendingSceneApplyTargetSample.load(std::memory_order_acquire);
    if (targetSample < blockStartSample
        || targetSample >= blockStartSample + static_cast<int64_t>(buffer.getNumSamples()))
    {
        return;
    }

    const int targetOffset = juce::jlimit(0,
                                          juce::jmax(0, buffer.getNumSamples()),
                                          static_cast<int>(targetSample - blockStartSample));
    const double effectiveRate = currentSampleRate > 0.0 ? currentSampleRate : 44100.0;
    const float triggerFadeMs = triggerFadeInParam != nullptr
        ? juce::jlimit(0.01f, 120.0f, triggerFadeInParam->load(std::memory_order_acquire))
        : 12.0f;
    const double tailFadeMs = juce::jlimit(0.20, 0.75, juce::jmax(0.20, static_cast<double>(triggerFadeMs) * 0.08));
    const int fadeSamples = juce::jlimit(8,
                                         juce::jmax(8, juce::jmax(1, targetOffset)),
                                         static_cast<int>(std::round(effectiveRate * 0.001 * tailFadeMs)));
    const int fadeEndSample = juce::jlimit(0, buffer.getNumSamples(), targetOffset);
    const int fadeStartSample = juce::jmax(0, fadeEndSample - fadeSamples);
    const int denom = juce::jmax(1, fadeEndSample - fadeStartSample - 1);

    for (int sample = fadeStartSample; sample < fadeEndSample; ++sample)
    {
        const float t = static_cast<float>(sample - fadeStartSample) / static_cast<float>(denom);
        const float gain = std::cos(juce::jlimit(0.0f, 1.0f, t) * juce::MathConstants<float>::halfPi);
        for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
            buffer.setSample(channel, sample, buffer.getSample(channel, sample) * gain);
    }

    for (int sample = fadeEndSample; sample < buffer.getNumSamples(); ++sample)
    {
        for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
            buffer.setSample(channel, sample, 0.0f);
    }
}

void MlrVSTAudioProcessor::recordMonomeControlPatternEvent(ControlMode mode,
                                                           int targetStripIndex,
                                                           int controlRow,
                                                           int column)
{
    if (!audioEngine || !monomeControlPageShowsPatternRecorder(mode))
        return;

    if (isSceneModeEnabled())
        recordMonomeControlSceneEvent(mode, targetStripIndex, controlRow, column);
}

void MlrVSTAudioProcessor::playbackMonomeControlPatternEvent(const PatternRecorder::Event& event)
{
    if (!audioEngine)
        return;

    const int stripIndex = juce::jlimit(0, MaxStrips - 1, event.stripIndex);
    auto* strip = audioEngine->getStrip(stripIndex);
    if (strip == nullptr)
        return;

    const auto mode = static_cast<ControlMode>(event.controlMode);
    const int controlRow = event.controlRow;
    const int column = juce::jlimit(0, MaxColumns - 1, event.column);

    switch (mode)
    {
        case ControlMode::Speed:
        {
            setStripSpeedControlValue(stripIndex,
                                      PlayheadSpeedQuantizer::ratioFromColumn(column),
                                      StripControlWriteMode::CacheOnly);
            break;
        }

        case ControlMode::Pan:
        {
            const float pan = juce::jlimit(-1.0f, 1.0f, (column - 8) / 8.0f);
            setStripPanControlValue(stripIndex,
                                    pan,
                                    StripControlWriteMode::CacheOnly);
            break;
        }

        case ControlMode::Volume:
        {
            setStripVolumeControlValue(stripIndex,
                                       juce::jlimit(0.0f, 1.0f, column / 15.0f),
                                       StripControlWriteMode::CacheOnly);
            break;
        }

        case ControlMode::Swing:
        case ControlMode::Gate:
        {
            MonomeMixActions::applyButtonPressLive(*strip,
                                                   column,
                                                   static_cast<int>(mode),
                                                   static_cast<int>(getGatePageMode()));
            break;
        }

        case ControlMode::Pitch:
        {
            static constexpr std::array<float, 16> kMusicalPitchSemitones{
                -24.0f, -21.0f, -18.0f, -15.0f, -12.0f, -9.0f, -6.0f, -3.0f,
                  0.0f,   3.0f,   6.0f,   9.0f,  12.0f, 15.0f, 18.0f, 24.0f
            };
            const float quantizedSemitones = quantizePitchSemitonesForStripControl(
                stripIndex,
                kMusicalPitchSemitones[static_cast<size_t>(column)]);
            if (auto* rawParam = stripPitchParams[static_cast<size_t>(stripIndex)])
                rawParam->store(quantizedSemitones, std::memory_order_release);

            const auto loopPitchRole = getLoopPitchRole(stripIndex);
            if (loopPitchRole == LoopPitchRole::Sync)
                requestLoopPitchRoleStateUpdate(stripIndex);
            else
                applyPitchControlToStrip(stripIndex, *strip, quantizedSemitones);

            if (loopPitchRole == LoopPitchRole::Master)
                updateGlobalRootFromLoopPitchMaster(stripIndex, true);
            break;
        }

        case ControlMode::GrainSize:
            MonomeMixActions::applyGrainPageButtonPressLive(*strip, controlRow, column);
            break;

        case ControlMode::Delay:
        {
            switch (juce::jlimit(0, 5, controlRow))
            {
                case 0:
                    setStripDelayMixControlValue(stripIndex,
                                                 juce::jlimit(0.0f, 1.0f, column / 15.0f),
                                                 StripControlWriteMode::CacheOnly);
                    break;
                case 1:
                {
                    const bool syncEnabled = strip->isDelaySyncEnabled();
                    const juce::NormalisableRange<float> syncRange(0.25f, 4.0f, 0.001f, 0.45f);
                    const juce::NormalisableRange<float> freeRange(0.25f, 4.0f, 0.001f, 0.4f);
                    const float unit = juce::jlimit(0.0f, 1.0f, column / 15.0f);
                    const float timeValue = (syncEnabled ? syncRange : freeRange).convertFrom0to1(unit);
                    setStripDelayTimeControlValue(stripIndex,
                                                  timeValue,
                                                  StripControlWriteMode::CacheOnly);
                    break;
                }
                case 2:
                    setStripDelayFeedbackControlValue(stripIndex,
                                                      0.97f * juce::jlimit(0.0f, 1.0f, column / 15.0f),
                                                      StripControlWriteMode::CacheOnly);
                    break;
                case 3:
                {
                    const juce::NormalisableRange<float> range(20.0f, 12000.0f, 1.0f, 0.25f);
                    setStripDelayLowCutControlValue(stripIndex,
                                                    range.convertFrom0to1(juce::jlimit(0.0f, 1.0f, column / 15.0f)),
                                                    StripControlWriteMode::CacheOnly);
                    break;
                }
                case 4:
                {
                    const juce::NormalisableRange<float> range(200.0f, 20000.0f, 1.0f, 0.3f);
                    setStripDelayHighCutControlValue(stripIndex,
                                                     range.convertFrom0to1(juce::jlimit(0.0f, 1.0f, column / 15.0f)),
                                                     StripControlWriteMode::CacheOnly);
                    break;
                }
                case 5:
                    if (column <= 2)
                    {
                        setStripDelayModeControlValue(stripIndex,
                                                      static_cast<EnhancedAudioStrip::DelayMode>(column),
                                                      StripControlWriteMode::CacheOnly);
                    }
                    else if (column >= 12)
                    {
                        setStripDelaySyncEnabledControlValue(stripIndex,
                                                             column >= 14,
                                                             StripControlWriteMode::CacheOnly);
                    }
                    break;
                default:
                    break;
            }
            break;
        }

        case ControlMode::Filter:
            if (controlRow == 0)
            {
                const float t = juce::jlimit(0.0f, 1.0f, column / 15.0f);
                setStripFilterFrequencyControlValue(stripIndex,
                                                    20.0f * std::pow(1000.0f, t),
                                                    StripControlWriteMode::CacheOnly);
            }
            else if (controlRow == 1)
            {
                setStripFilterResonanceControlValue(stripIndex,
                                                    0.1f + (juce::jlimit(0.0f, 15.0f, static_cast<float>(column)) / 15.0f) * 9.9f,
                                                    StripControlWriteMode::CacheOnly);
            }
            else if (controlRow == 2)
            {
                if (column <= 2)
                {
                    const float morph = (column <= 0) ? 0.0f : (column == 1 ? 0.5f : 1.0f);
                    setStripFilterMorphControlValue(stripIndex,
                                                    morph,
                                                    StripControlWriteMode::CacheOnly);
                }
            }
            break;

        case ControlMode::Normal:
        case ControlMode::FileBrowser:
        case ControlMode::GroupAssign:
        case ControlMode::Modulation:
        case ControlMode::Preset:
        case ControlMode::StepEdit:
        default:
            break;
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
        5,
        globalChoiceAttrs));  // Default to 1/8

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
        3,
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
            juce::StringArray{"SVF12", "SVF24", "LAD12", "LAD24", "MOOG S", "MOOG H"},
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
    for (auto& scratch : sampleModeScratchBuffers)
        scratch.setSize(2, samplesPerBlock, false, false, true);
    sceneTransitionOutgoingScratch.setSize(juce::jmax(1, getTotalNumOutputChannels()),
                                           juce::jmax(1, samplesPerBlock),
                                           false,
                                           false,
                                           true);
    sceneTransitionIncomingScratch.setSize(juce::jmax(1, getTotalNumOutputChannels()),
                                           juce::jmax(1, samplesPerBlock),
                                           false,
                                           false,
                                           true);
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
        loadPersistentGlobalControls();
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

    lastHostTransportPlaying.store(posInfo.getIsPlaying() ? 1 : 0, std::memory_order_release);
    
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
    if (activeSceneTransitionContext != nullptr)
        renderSceneTransitionBlock(buffer, midiMessages, posInfo);
    else
        renderActiveSceneAudio(buffer, midiMessages, posInfo, true, true);
    commitSceneTransitionIfReady();
    applyQueuedSceneExitTailFade(buffer, currentGlobalSample);

    if (sceneRecallBlendSamplesRemaining > 0 && sceneRecallBlendTotalSamples > 0)
    {
        const int blendChannels = juce::jmin(buffer.getNumChannels(), kSceneRecallBlendMaxChannels);
        const int totalBlendSamples = juce::jmax(1, sceneRecallBlendTotalSamples);
        const int sourceTailLength = juce::jlimit(0, kSceneRecallBlendMaxSamples, sceneRecallBlendStartTailLength);
        for (int sample = 0;
             sample < buffer.getNumSamples() && sceneRecallBlendSamplesRemaining > 0;
             ++sample)
        {
            const int completedSamples = juce::jmax(0, totalBlendSamples - sceneRecallBlendSamplesRemaining);
            const float t = totalBlendSamples > 1
                ? static_cast<float>(completedSamples) / static_cast<float>(totalBlendSamples - 1)
                : 1.0f;
            const float clampedT = juce::jlimit(0.0f, 1.0f, t);
            const float fadeOut = std::cos(clampedT * juce::MathConstants<float>::halfPi);
            const float fadeIn = std::sin(clampedT * juce::MathConstants<float>::halfPi);
            const int sourceIndex = sourceTailLength > 1 && totalBlendSamples > 1
                ? juce::jlimit(0,
                               sourceTailLength - 1,
                               static_cast<int>(std::llround(
                                   (static_cast<double>(completedSamples) * static_cast<double>(sourceTailLength - 1))
                                   / static_cast<double>(totalBlendSamples - 1))))
                : juce::jmax(0, sourceTailLength - 1);

            for (int channel = 0; channel < blendChannels; ++channel)
            {
                const float in = buffer.getSample(channel, sample);
                const float start = sourceTailLength > 0
                    ? sceneRecallBlendStartTail[static_cast<size_t>(channel)][static_cast<size_t>(sourceIndex)]
                    : sceneRecallBlendStartSamples[static_cast<size_t>(channel)];
                buffer.setSample(channel, sample, (start * fadeOut) + (in * fadeIn));
            }

            --sceneRecallBlendSamplesRemaining;
        }

        if (sceneRecallBlendSamplesRemaining <= 0)
        {
            sceneRecallBlendSamplesRemaining = 0;
            sceneRecallBlendTotalSamples = 0;
            sceneRecallBlendStartTailLength = 0;
        }
    }

    lastRenderedOutputSamples.fill(0.0f);
    lastRenderedOutputTailLength = 0;
    if (buffer.getNumSamples() > 0)
    {
        const int lastSample = buffer.getNumSamples() - 1;
        const int captureChannels = juce::jmin(buffer.getNumChannels(), kSceneRecallBlendMaxChannels);
        const int tailSamples = juce::jmin(buffer.getNumSamples(), kSceneRecallBlendMaxSamples);
        const int tailOffset = buffer.getNumSamples() - tailSamples;
        for (int channel = 0; channel < captureChannels; ++channel)
        {
            lastRenderedOutputSamples[static_cast<size_t>(channel)] = buffer.getSample(channel, lastSample);
            std::copy_n(buffer.getReadPointer(channel, tailOffset),
                        tailSamples,
                        lastRenderedOutputTail[static_cast<size_t>(channel)].begin());
        }
        for (int channel = captureChannels; channel < kSceneRecallBlendMaxChannels; ++channel)
            std::fill_n(lastRenderedOutputTail[static_cast<size_t>(channel)].begin(), tailSamples, 0.0f);
        lastRenderedOutputTailLength = tailSamples;
    }

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
void MlrVSTAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    try
    {
        if (isSceneModeEnabled() && audioEngine != nullptr)
            syncSceneMotionStateFromEngine(activeSceneSlot);

        auto state = parameters.copyState();
        stripPersistentGlobalControlsFromState(state);
        appendDefaultPathsToState(state);
        appendControlPagesToState(state);
        appendFlipStatesToState(state);
        appendLoopPitchStateToState(state);
        appendSceneModeStateToState(state);
        
        if (!state.isValid())
            return;
            
        std::unique_ptr<juce::XmlElement> xml(state.createXml());
        
        if (xml != nullptr)
        {
            copyXmlToBinary(*xml, destData);
        }
    }
    catch (...)
    {
        // If anything goes wrong, just return empty state
        destData.reset();
    }
}

void MlrVSTAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xmlState(getXmlFromBinary(data, sizeInBytes));

    resetRuntimePresetStateToDefaults();
    loadedPresetIndex = -1;

    if (xmlState != nullptr)
        if (xmlState->hasTagName(parameters.state.getType()))
        {
            suppressPersistentGlobalControlsSave.store(1, std::memory_order_release);
            auto state = juce::ValueTree::fromXml(*xmlState);
            stripPersistentGlobalControlsFromState(state);
            parameters.replaceState(state);
            loadDefaultPathsFromState(state);
            loadControlPagesFromState(state);
            loadFlipStatesFromState(state);
            loadLoopPitchStateFromState(state);
            loadSceneModeStateFromState(state);
            loadPersistentGlobalControls();
            persistentGlobalControlsApplied = true;
            pendingPersistentGlobalControlsRestore.store(1, std::memory_order_release);
            pendingPersistentGlobalControlsRestoreMs = juce::Time::currentTimeMillis() + 250;
            pendingPersistentGlobalControlsRestoreRemaining = 5;
            suppressPersistentGlobalControlsSave.store(0, std::memory_order_release);
            persistentGlobalControlsReady.store(1, std::memory_order_release);
            syncSceneModeFromParameters();
            lastScenePerformanceProcessBeat = std::numeric_limits<double>::quiet_NaN();
            lastScenePerformanceProcessSceneSlot = -1;
            lastScenePerformanceProcessSceneStartBeat = std::numeric_limits<double>::quiet_NaN();
            if (isSceneModeEnabled())
                clearAllStripGroupsForSceneMode();
        }
}

void MlrVSTAudioProcessor::stripPersistentGlobalControlsFromState(juce::ValueTree& state) const
{
    if (!state.isValid())
        return;
    for (const auto* id : kPersistentGlobalControlParameterIds)
        state.removeProperty(id, nullptr);
}

//==============================================================================
bool MlrVSTAudioProcessor::loadSampleToStrip(int stripIndex, const juce::File& file)
{
    if (safeFileExistsAsFile(file) && stripIndex >= 0 && stripIndex < MaxStrips)
    {
        const auto idx = static_cast<size_t>(stripIndex);
        loopStripLoadRequestIds[idx].fetch_add(1, std::memory_order_acq_rel);
        loopStripLoadInFlight[idx].store(0, std::memory_order_release);
        resetLoopStripLoadProgress(stripIndex);
        pendingLoopStripFiles[idx] = juce::File();
        loopPitchAnalysisRequestIds[static_cast<size_t>(stripIndex)].fetch_add(1, std::memory_order_acq_rel);
        loopPitchAnalysisInFlight[static_cast<size_t>(stripIndex)].store(0, std::memory_order_release);
        resetLoopPitchAnalysisProgress(stripIndex);
        rememberLoadedSamplePathForStrip(stripIndex, file);

        if (auto* strip = audioEngine != nullptr ? audioEngine->getStrip(stripIndex) : nullptr)
        {
            if (strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Sample)
            {
                const bool loaded = loadSampleToSampleModeStrip(stripIndex, file);
                return loaded;
            }
        }

        if (audioEngine != nullptr)
            audioEngine->setGlobalStretchBackend(getStretchBackend());

        const bool loaded = audioEngine->loadSampleToStrip(stripIndex, file);
        if (loaded)
        {
            queueActiveSceneAutosave();
            loopPitchDetectedMidi[static_cast<size_t>(stripIndex)].store(-1, std::memory_order_release);
            loopPitchDetectedHz[static_cast<size_t>(stripIndex)].store(0.0f, std::memory_order_release);
            loopPitchDetectedPitchConfidence[static_cast<size_t>(stripIndex)].store(0.0f, std::memory_order_release);
            loopPitchDetectedScaleIndices[static_cast<size_t>(stripIndex)].store(-1, std::memory_order_release);
            loopPitchDetectedScaleConfidence[static_cast<size_t>(stripIndex)].store(0.0f, std::memory_order_release);
            loopPitchAssignedMidi[static_cast<size_t>(stripIndex)].store(-1, std::memory_order_release);
            loopPitchAssignedManual[static_cast<size_t>(stripIndex)].store(0, std::memory_order_release);
            loopPitchPendingRetune[static_cast<size_t>(stripIndex)].store(0, std::memory_order_release);

            const auto role = getLoopPitchRole(stripIndex);
            if (role == LoopPitchRole::Master)
                requestLoopStripPitchMaster(stripIndex);
            else if (role == LoopPitchRole::Sync)
                requestLoopStripPitchSync(stripIndex);
        }
        return loaded;
    }

    return false;
}

bool MlrVSTAudioProcessor::loadSampleToStripPreservingPlaybackState(int stripIndex, const juce::File& file)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips || !safeFileExistsAsFile(file) || audioEngine == nullptr)
        return false;

    auto* strip = audioEngine->getStrip(stripIndex);
    if (strip == nullptr)
        return false;

    const bool isFlipMode = (strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Sample);
    if (isFlipMode)
        return loadSampleToStrip(stripIndex, file);

    const bool isStepMode = (strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Step);
    const auto loopTempoMatchBackend = isStepMode
        ? TimeStretchBackend::Resample
        : resolveLoopTempoMatchBackendForStrip(stripIndex);

    if (loopTempoMatchBackend != TimeStretchBackend::Bungee)
    {
        const bool requiresTimelineAnchor = strip->isPlaying() && !isStepMode;
        const float savedSpeed = strip->getPlaybackSpeed();
        const float savedVolume = strip->getVolume();
        const float savedPan = strip->getPan();
        const int savedGroup = strip->getGroup();
        const int savedLoopStart = strip->getLoopStart();
        const int savedLoopEnd = strip->getLoopEnd();
        const bool savedTimelineAnchored = strip->isPpqTimelineAnchored();
        const double savedTimelineOffsetBeats = strip->getPpqTimelineOffsetBeats();
        const int savedColumn = strip->getCurrentColumn();

        const bool loaded = loadSampleToStrip(stripIndex, file);
        if (!loaded || audioEngine == nullptr)
            return loaded;

        strip = audioEngine->getStrip(stripIndex);
        if (strip == nullptr || strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Sample)
            return loaded;

        double hostPpqNow = 0.0;
        double hostTempoNow = audioEngine->getCurrentTempo();
        const bool canRestoreTimelineAnchor = requiresTimelineAnchor
            && savedTimelineAnchored
            && getHostSyncSnapshot(hostPpqNow, hostTempoNow);
        const int64_t currentGlobalSample = audioEngine->getGlobalSampleCount();

        if (canRestoreTimelineAnchor)
            strip->setBeatsPerLoopAtPpq(strip->getBeatsPerLoop(), hostPpqNow);

        setStripSpeedControlValue(stripIndex, savedSpeed, StripControlWriteMode::CacheOnly);
        setStripVolumeControlValue(stripIndex, savedVolume, StripControlWriteMode::CacheOnly);
        setStripPanControlValue(stripIndex, savedPan, StripControlWriteMode::CacheOnly);
        strip->setGroup(savedGroup);
        strip->setLoop(savedLoopStart, savedLoopEnd);

        if (canRestoreTimelineAnchor)
        {
            strip->restorePresetPpqState(true,
                                         true,
                                         savedTimelineOffsetBeats,
                                         savedColumn,
                                         hostTempoNow,
                                         hostPpqNow,
                                         currentGlobalSample);
        }

        return true;
    }

    const auto idx = static_cast<size_t>(stripIndex);
    loopPitchAnalysisRequestIds[idx].fetch_add(1, std::memory_order_acq_rel);
    loopPitchAnalysisInFlight[idx].store(0, std::memory_order_release);
    resetLoopPitchAnalysisProgress(stripIndex);
    loopPitchDetectedMidi[idx].store(-1, std::memory_order_release);
    loopPitchDetectedHz[idx].store(0.0f, std::memory_order_release);
    loopPitchDetectedPitchConfidence[idx].store(0.0f, std::memory_order_release);
    loopPitchDetectedScaleIndices[idx].store(-1, std::memory_order_release);
    loopPitchDetectedScaleConfidence[idx].store(0.0f, std::memory_order_release);
    loopPitchAssignedMidi[idx].store(-1, std::memory_order_release);
    loopPitchAssignedManual[idx].store(0, std::memory_order_release);
    loopPitchPendingRetune[idx].store(0, std::memory_order_release);

    const int requestId = loopStripLoadRequestIds[idx].fetch_add(1, std::memory_order_acq_rel) + 1;
    loopStripLoadInFlight[idx].store(1, std::memory_order_release);
    pendingLoopStripFiles[idx] = file;
    setRecentSampleDirectory(stripIndex, getSamplePathModeForStrip(stripIndex), file.getParentDirectory(), false);
    updateLoopStripLoadProgress(stripIndex, requestId, 0.03f, "Loading " + file.getFileName() + "...");

    double hostTempoSnapshot = audioEngine->getCurrentTempo();
    double ignoredPpq = 0.0;
    getHostSyncSnapshot(ignoredPpq, hostTempoSnapshot);

    auto job = std::make_unique<LoopStripLoadJob>(*this,
                                                  stripIndex,
                                                  requestId,
                                                  file,
                                                  hostTempoSnapshot,
                                                  loopTempoMatchBackend);
    loopStripLoadThreadPool.addJob(job.release(), true);
    return true;
}

bool MlrVSTAudioProcessor::loadSampleToSampleModeStrip(int stripIndex, const juce::File& file)
{
    if (!safeFileExistsAsFile(file) || stripIndex < 0 || stripIndex >= MaxStrips)
        return false;

    const auto idx = static_cast<size_t>(stripIndex);
    loopStripLoadRequestIds[idx].fetch_add(1, std::memory_order_acq_rel);
    loopStripLoadInFlight[idx].store(0, std::memory_order_release);
    resetLoopStripLoadProgress(stripIndex);
    pendingLoopStripFiles[idx] = juce::File();
    rememberLoadedSamplePathForStrip(stripIndex, file);

    if (auto* engine = getSampleModeEngine(stripIndex, true))
    {
        invalidateFlipLegacyLoopSync(stripIndex);
        const int requestId = engine->loadSampleAsync(file);
        if (requestId > 0)
        {
            queueActiveSceneAutosave();
            return true;
        }
    }

    return false;
}

SampleModeEngine* MlrVSTAudioProcessor::getSampleModeEngine(int stripIndex, bool createIfMissing)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return nullptr;

    auto& engine = sampleModeEngines[static_cast<size_t>(stripIndex)];
    if (engine == nullptr && createIfMissing)
    {
        engine = std::make_unique<SampleModeEngine>();
        if (currentSampleRate > 0.0)
            engine->prepare(currentSampleRate, juce::jmax(1, getBlockSize()));
        engine->setLegacyLoopRenderStateChangedCallback(
            [this, stripIndex]()
            {
                handleSampleModeLegacyLoopRenderStateChanged(stripIndex);
            });
    }

    return engine.get();
}

void MlrVSTAudioProcessor::handleSampleModeLegacyLoopRenderStateChanged(int stripIndex,
                                                                        bool preferInlineBuild)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;

    {
        const juce::SpinLock::ScopedLockType lock(flipLegacyLoopSyncInfoCacheLock);
        flipLegacyLoopSyncInfoCache[static_cast<size_t>(stripIndex)] = {};
    }
    {
        const juce::SpinLock::ScopedLockType lock(pendingFlipLegacyLoopTriggerLock);
        pendingFlipLegacyLoopTriggers[static_cast<size_t>(stripIndex)] = {};
    }

    auto* strip = audioEngine != nullptr ? audioEngine->getStrip(stripIndex) : nullptr;
    auto* engine = getSampleModeEngine(stripIndex, false);
    if (strip == nullptr
        || engine == nullptr
        || strip->getPlayMode() != EnhancedAudioStrip::PlayMode::Sample
        || !engine->isLegacyLoopEngineEnabled())
    {
        return;
    }

    SampleModeEngine::LegacyLoopSyncInfo syncInfo;
    if (!engine->getLegacyLoopSyncInfo(syncInfo))
        return;

    const double hostTempo = audioEngine != nullptr ? audioEngine->getCurrentTempo() : 120.0;
    const double hostPpq = audioEngine != nullptr
        ? audioEngine->getTimelineBeat()
        : std::numeric_limits<double>::quiet_NaN();
    const int64_t currentGlobalSample = audioEngine != nullptr
        ? audioEngine->getGlobalSampleCount()
        : -1;
    const auto backend = getFlipTempoMatchBackend();
    const float visibleBankBeats = computeFlipLegacyLoopVisibleBankBeats(syncInfo);
    const bool usesAutoLegacyLoopOverride = syncInfo.legacyLoopBarSelection <= 0
        && syncInfo.visibleBankIndex < 0
        && syncInfo.bankEndSample <= syncInfo.bankStartSample;
    const bool allowInlineBuild = preferInlineBuild || usesAutoLegacyLoopOverride;

    int64_t bankStartSample = 0;
    int64_t bankEndSample = 0;
    if (strip->hasAudio()
        && computeFlipLegacyLoopBankRange(syncInfo, bankStartSample, bankEndSample))
    {
        const auto desiredTransientSliceStarts = buildFlipLegacyLoopTransientSliceCache(syncInfo,
                                                                                        bankStartSample,
                                                                                        bankEndSample);
        FlipLegacyLoopSyncCache reusableCache;
        bool canReuseRenderedAudio = false;
        {
            const juce::SpinLock::ScopedLockType lock(flipLegacyLoopSyncCacheLock);
            auto& cache = flipLegacyLoopSyncCache[static_cast<size_t>(stripIndex)];
            if (flipLegacyLoopCacheMatchesReusableAudioKey(cache,
                                                           syncInfo,
                                                           hostTempo,
                                                           backend,
                                                           visibleBankBeats))
            {
                cache.sliceSignature = computeFlipLegacyLoopSliceSignature(syncInfo.visibleSlices);
                cache.cachedTransientSliceStarts = desiredTransientSliceStarts;
                cache.valid = true;
                cache.stripApplied = true;
                reusableCache = cache;
                canReuseRenderedAudio = true;
            }
        }

        if (canReuseRenderedAudio)
        {
            const bool shouldRestorePlayback = strip->isPlaying()
                && strip->isPpqTimelineAnchored()
                && std::isfinite(hostPpq)
                && hostTempo > 0.0
                && currentGlobalSample >= 0;
            const int restoreColumn = shouldRestorePlayback ? strip->getCurrentColumn() : 0;
            const double restoreOffsetBeats = shouldRestorePlayback ? strip->getPpqTimelineOffsetBeats() : 0.0;

            applyFlipLegacyLoopTransientSliceCacheToStrip(*strip, reusableCache, backend, visibleBankBeats);

            if (shouldRestorePlayback)
            {
                strip->restorePresetPpqState(true,
                                            true,
                                            restoreOffsetBeats,
                                            restoreColumn,
                                            hostTempo,
                                            hostPpq,
                                            currentGlobalSample);
            }
            return;
        }
    }

    invalidateFlipLegacyLoopSync(stripIndex);

    if (strip->hasAudio()
        && syncFlipLegacyLoopStripState(stripIndex,
                                        *strip,
                                        syncInfo,
                                        hostTempo,
                                        hostPpq,
                                        currentGlobalSample,
                                        true,
                                        backend,
                                        allowInlineBuild))
    {
        return;
    }

    queueFlipLegacyLoopRender(stripIndex, syncInfo, hostTempo, backend);
}

void MlrVSTAudioProcessor::handleFlipTempoMatchModeChanged()
{
    if (audioEngine == nullptr)
        return;

    for (int stripIndex = 0; stripIndex < MaxStrips; ++stripIndex)
    {
        auto* strip = audioEngine->getStrip(stripIndex);
        auto* engine = getSampleModeEngine(stripIndex, false);
        if (strip == nullptr
            || engine == nullptr
            || strip->getPlayMode() != EnhancedAudioStrip::PlayMode::Sample)
        {
            continue;
        }

        if (engine->isLegacyLoopEngineEnabled())
        {
            handleSampleModeLegacyLoopRenderStateChanged(stripIndex, true);
            continue;
        }

        const auto playback = resolveFlipPlaybackState(*strip, *engine);
        engine->requestKeyLockRenderCache(playback.playbackRate,
                                          playback.internalPitchSemitones,
                                          playback.shouldBuildKeyLockCache,
                                          playback.tempoMatch.backend);
    }

    for (int stripIndex = 0; stripIndex < MaxStrips; ++stripIndex)
        updateGlobalRootFromLoopPitchMaster(stripIndex, false);
    applyLoopPitchSyncToAllStrips();
}

void MlrVSTAudioProcessor::rememberLoadedSamplePathForStrip(int stripIndex, const juce::File& file, bool persist)
{
    rememberLoadedSamplePathForStripMode(stripIndex, file, getSamplePathModeForStrip(stripIndex), persist);
}

void MlrVSTAudioProcessor::rememberLoadedSamplePathForStripMode(int stripIndex,
                                                                const juce::File& file,
                                                                SamplePathMode mode,
                                                                bool persist)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;

    const auto idx = static_cast<size_t>(stripIndex);
    currentStripFiles[idx] = file;

    const auto directory = file.getParentDirectory();
    if (directory != juce::File())
    {
        setRecentSampleDirectory(stripIndex, mode, directory, false);
    }

    if (persist)
        savePersistentDefaultPaths();
}

bool MlrVSTAudioProcessor::hasSampleModeAudio(int stripIndex) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return false;

    auto& engine = sampleModeEngines[static_cast<size_t>(stripIndex)];
    return engine != nullptr && engine->hasSample();
}

void MlrVSTAudioProcessor::setSampleModeHeldVisibleSliceSlot(int stripIndex, int visibleSlot)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;

    sampleModeHeldVisibleSliceSlots[static_cast<size_t>(stripIndex)].store(
        juce::jlimit(-1, SliceModel::VisibleSliceCount - 1, visibleSlot),
        std::memory_order_release);
}

void MlrVSTAudioProcessor::clearSampleModeHeldVisibleSliceSlot(int stripIndex, int visibleSlot)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;

    auto& heldSlot = sampleModeHeldVisibleSliceSlots[static_cast<size_t>(stripIndex)];
    if (visibleSlot >= 0)
    {
        const int current = heldSlot.load(std::memory_order_acquire);
        if (current != visibleSlot)
            return;
    }

    heldSlot.store(-1, std::memory_order_release);
}

int MlrVSTAudioProcessor::getSampleModeHeldVisibleSliceSlot(int stripIndex) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return -1;

    return sampleModeHeldVisibleSliceSlots[static_cast<size_t>(stripIndex)].load(std::memory_order_acquire);
}

juce::String MlrVSTAudioProcessor::createEmbeddedFlipSampleData(int stripIndex) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return {};

    const auto storedPath = currentStripFiles[static_cast<size_t>(stripIndex)].getFullPathName().trim();
    if (storedPath.isNotEmpty() && safeFileExistsAsFile(juce::File(storedPath)))
        return {};

    auto* engine = const_cast<MlrVSTAudioProcessor*>(this)->getSampleModeEngine(stripIndex, false);
    if (engine == nullptr)
        return {};

    const auto sample = engine->getLoadedSample();
    if (sample == nullptr || sample->audioBuffer.getNumSamples() <= 0)
        return {};
    if (sample->sourcePath.isNotEmpty() && safeFileExistsAsFile(juce::File(sample->sourcePath)))
        return {};

    juce::String embeddedSample;
    if (!encodeBufferAsWavBase64(sample->audioBuffer, sample->sourceSampleRate, embeddedSample))
        return {};

    return embeddedSample;
}

bool MlrVSTAudioProcessor::loadEmbeddedFlipSampleData(int stripIndex,
                                                      const juce::String& base64Data,
                                                      const SampleModePersistentState* persistentState)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips || base64Data.isEmpty())
        return false;

    juce::AudioBuffer<float> buffer;
    double sampleRate = 0.0;
    if (!decodeWavBase64ToBuffer(base64Data, buffer, sampleRate))
        return false;

    auto* engine = getSampleModeEngine(stripIndex, true);
    if (engine == nullptr)
        return false;

    juce::String displayName = "Embedded Flip Sample";
    if (persistentState != nullptr && persistentState->samplePath.isNotEmpty())
        displayName = juce::File(persistentState->samplePath).getFileNameWithoutExtension();

    const bool loaded = engine->loadSampleFromBuffer(buffer, sampleRate, {}, displayName);
    if (!loaded)
        return false;

    if (persistentState != nullptr)
        engine->applyPersistentState(*persistentState);

    currentStripFiles[static_cast<size_t>(stripIndex)] = juce::File();
    handleSampleModeLegacyLoopRenderStateChanged(stripIndex);
    return true;
}

void MlrVSTAudioProcessor::renderSampleModeStrip(int stripIndex,
                                                 juce::AudioBuffer<float>& output,
                                                 int startSample,
                                                 int numSamples,
                                                 const juce::AudioPlayHead::PositionInfo& positionInfo,
                                                 int64_t globalSampleStart,
                                                 double tempo,
                                                 double quantizeBeats)
{
    auto* strip = audioEngine ? audioEngine->getStrip(stripIndex) : nullptr;
    auto* engine = getSampleModeEngine(stripIndex, false);
    if (strip == nullptr || engine == nullptr || strip->getPlayMode() != EnhancedAudioStrip::PlayMode::Sample)
        return;

    if (!positionInfo.getIsPlaying())
    {
        sampleModeRenderedLastBlock[static_cast<size_t>(stripIndex)] = false;
        engine->stop(true);
        strip->stop(true);
        engine->clearLegacyLoopMonitorState();
        return;
    }

    auto& scratch = sampleModeScratchBuffers[static_cast<size_t>(stripIndex)];
    if (scratch.getNumChannels() < 2 || scratch.getNumSamples() < numSamples)
    {
        jassertfalse;
        return;
    }
    scratch.clear(0, numSamples);

    if (engine->isLegacyLoopEngineEnabled())
    {
        auto syncInfoPtr = getCachedFlipLegacyLoopSyncInfo(stripIndex, *engine);
        SampleModeEngine::LegacyLoopSyncInfo syncInfo;
        bool shouldReplayPendingTrigger = false;
        bool pendingMomentaryStutter = false;
        {
            const juce::SpinLock::ScopedLockType lock(pendingFlipLegacyLoopTriggerLock);
            const auto& pendingTrigger = pendingFlipLegacyLoopTriggers[static_cast<size_t>(stripIndex)];
            if (pendingTrigger.valid)
            {
                syncInfo = pendingTrigger.syncInfo;
                shouldReplayPendingTrigger = true;
                pendingMomentaryStutter = pendingTrigger.isMomentaryStutter;
            }
        }

        const auto stretchBackend = getFlipTempoMatchBackend();
        const bool needsLegacyLoopPrime = !strip->isSampleModeLegacyLoopEngineEnabled();
        if (!shouldReplayPendingTrigger)
        {
            if (syncInfoPtr == nullptr)
                return;
            syncInfo = *syncInfoPtr;
        }

        const double hostPpq = positionInfo.getPpqPosition().hasValue()
            ? *positionInfo.getPpqPosition()
            : std::numeric_limits<double>::quiet_NaN();
        if (!syncFlipLegacyLoopStripState(stripIndex,
                                          *strip,
                                          syncInfo,
                                          tempo,
                                          hostPpq,
                                          globalSampleStart,
                                          !shouldReplayPendingTrigger,
                                          stretchBackend,
                                          shouldReplayPendingTrigger && needsLegacyLoopPrime))
        {
            if (!strip->hasAudio())
                return;
        }

        strip->setSampleModeLegacyLoopEngineEnabled(true);
        if (shouldReplayPendingTrigger)
        {
            strip->triggerAtSample(juce::jlimit(0, SliceModel::VisibleSliceCount - 1, syncInfo.triggerVisibleSlot),
                                   tempo,
                                   globalSampleStart,
                                   positionInfo,
                                   pendingMomentaryStutter);
            {
                const juce::SpinLock::ScopedLockType lock(pendingFlipLegacyLoopTriggerLock);
                pendingFlipLegacyLoopTriggers[static_cast<size_t>(stripIndex)] = {};
            }
            engine->clearPendingVisibleSlice();
        }
        strip->process(scratch,
                       0,
                       numSamples,
                       positionInfo,
                       globalSampleStart,
                       tempo,
                       quantizeBeats);

        int legacyCurrentColumn = -1;
        float legacyPlaybackProgress = -1.0f;
        if (strip->isPlaying())
        {
            legacyCurrentColumn = strip->getCurrentColumn();
            int64_t bankStartSample = 0;
            int64_t bankEndSample = 0;
            if (computeFlipLegacyLoopBankRange(syncInfo, bankStartSample, bankEndSample))
            {
                const float bankStartNorm = static_cast<float>(bankStartSample)
                    / static_cast<float>(juce::jmax<int64_t>(1, syncInfo.loadedSample->sourceLengthSamples));
                const float bankEndNorm = static_cast<float>(bankEndSample)
                    / static_cast<float>(juce::jmax<int64_t>(1, syncInfo.loadedSample->sourceLengthSamples));
                const float bankProgress = juce::jlimit(0.0f, 1.0f, static_cast<float>(strip->getNormalizedPosition()));
                legacyPlaybackProgress = juce::jlimit(0.0f,
                                                      1.0f,
                                                      bankStartNorm + ((bankEndNorm - bankStartNorm) * bankProgress));
            }
            else if (legacyCurrentColumn >= 0 && legacyCurrentColumn < SliceModel::VisibleSliceCount)
            {
                const auto& slice = syncInfo.visibleSlices[static_cast<size_t>(legacyCurrentColumn)];
                legacyPlaybackProgress = slice.normalizedStart;
            }
        }

        engine->updateLegacyLoopMonitorState(strip->isPlaying(),
                                             legacyCurrentColumn,
                                             legacyPlaybackProgress);
        output.addFrom(0, startSample, scratch, 0, 0, numSamples);
        output.addFrom(1, startSample, scratch, 1, 0, numSamples);
        return;
    }

    strip->setSampleModeLegacyLoopEngineEnabled(false);
    engine->clearLegacyLoopMonitorState();

    const auto flipPlayback = resolveFlipPlaybackState(*strip, *engine);
    const float playbackRate = flipPlayback.playbackRate;
    const float internalPitchSemitones = flipPlayback.internalPitchSemitones;
    const int fadeSamples = juce::jmax(16, static_cast<int>(currentSampleRate * 0.003));
    const bool preferHighQualityKeyLock = flipPlayback.preferHighQualityKeyLock;
    const auto renderResult = engine->renderToBuffer(scratch,
                                                     0,
                                                     numSamples,
                                                     playbackRate,
                                                     fadeSamples,
                                                     internalPitchSemitones,
                                                     preferHighQualityKeyLock);
    auto& renderedLastBlock = sampleModeRenderedLastBlock[static_cast<size_t>(stripIndex)];
    if (!renderResult.renderedAnything && renderedLastBlock)
        strip->armRealtimeSignalsmithAlignmentTail();

    if (renderResult.renderedAnything || strip->hasPendingRealtimeSignalsmithAlignmentTail())
    {
        strip->processExternalOutputBuffer(scratch,
                                           0,
                                           numSamples,
                                           positionInfo,
                                           tempo,
                                           !renderResult.usedInternalPitch);
        output.addFrom(0, startSample, scratch, 0, 0, numSamples);
        output.addFrom(1, startSample, scratch, 1, 0, numSamples);
    }

    renderedLastBlock = renderResult.renderedAnything;
}

void MlrVSTAudioProcessor::triggerSampleModeStripAtSample(int stripIndex,
                                                          int column,
                                                          int sampleSliceId,
                                                          int64_t sampleStartSample,
                                                          int64_t triggerSample,
                                                          const juce::AudioPlayHead::PositionInfo& positionInfo,
                                                          bool isMomentaryStutter)
{
    auto* strip = audioEngine ? audioEngine->getStrip(stripIndex) : nullptr;
    auto* engine = getSampleModeEngine(stripIndex, false);
    if (strip == nullptr || engine == nullptr || strip->getPlayMode() != EnhancedAudioStrip::PlayMode::Sample)
        return;

    const int visibleSlot = juce::jlimit(0, SliceModel::VisibleSliceCount - 1, column);
    if (engine->isLegacyLoopEngineEnabled())
    {
        SampleModeEngine::LegacyLoopSyncInfo syncInfo;
        const auto stretchBackend = getFlipTempoMatchBackend();
        const bool needsLegacyLoopPrime = !strip->isSampleModeLegacyLoopEngineEnabled();
        const double hostTempo = audioEngine != nullptr ? audioEngine->getCurrentTempo() : 120.0;
        SampleModeEngine::LegacyLoopSyncInfo currentSyncInfo;
        const bool hasCurrentSyncInfo = engine->getLegacyLoopSyncInfo(currentSyncInfo);
        const bool hasExplicitLegacyWindow = hasCurrentSyncInfo
            && currentSyncInfo.bankEndSample > currentSyncInfo.bankStartSample;
        const bool usesAutoLegacyLoopOverride = hasCurrentSyncInfo
            && currentSyncInfo.legacyLoopBarSelection <= 0
            && currentSyncInfo.visibleBankIndex < 0
            && currentSyncInfo.bankEndSample <= currentSyncInfo.bankStartSample;
        const bool resolveFromCurrentVisibleSlot = hasExplicitLegacyWindow || usesAutoLegacyLoopOverride;
        const int resolvedSliceId = resolveFromCurrentVisibleSlot ? -1 : sampleSliceId;
        const int64_t resolvedSliceStartSample = resolveFromCurrentVisibleSlot ? -1 : sampleStartSample;
        if (!engine->resolveLegacyLoopTriggerSyncInfo(visibleSlot,
                                                      resolvedSliceId,
                                                      resolvedSliceStartSample,
                                                      syncInfo))
        {
            const juce::SpinLock::ScopedLockType lock(pendingFlipLegacyLoopTriggerLock);
            pendingFlipLegacyLoopTriggers[static_cast<size_t>(stripIndex)] = {};
            return;
        }

        const double hostPpq = positionInfo.getPpqPosition().hasValue()
            ? *positionInfo.getPpqPosition()
            : std::numeric_limits<double>::quiet_NaN();
        bool synced = syncFlipLegacyLoopStripState(stripIndex,
                                                   *strip,
                                                   syncInfo,
                                                   hostTempo,
                                                   hostPpq,
                                                   triggerSample,
                                                   false,
                                                   stretchBackend,
                                                   needsLegacyLoopPrime);
        if (!synced && !needsLegacyLoopPrime)
        {
            synced = syncFlipLegacyLoopStripState(stripIndex,
                                                  *strip,
                                                  syncInfo,
                                                  hostTempo,
                                                  hostPpq,
                                                  triggerSample,
                                                  false,
                                                  stretchBackend,
                                                  true);
        }

        if (!synced)
        {
            const juce::SpinLock::ScopedLockType lock(pendingFlipLegacyLoopTriggerLock);
            auto& pendingTrigger = pendingFlipLegacyLoopTriggers[static_cast<size_t>(stripIndex)];
            pendingTrigger.valid = true;
            pendingTrigger.syncInfo = syncInfo;
            pendingTrigger.isMomentaryStutter = isMomentaryStutter;
            return;
        }

        strip->setSampleModeLegacyLoopEngineEnabled(true);
        strip->triggerAtSample(syncInfo.triggerVisibleSlot,
                               hostTempo,
                               triggerSample,
                               positionInfo,
                               isMomentaryStutter);
        {
            const juce::SpinLock::ScopedLockType lock(pendingFlipLegacyLoopTriggerLock);
            pendingFlipLegacyLoopTriggers[static_cast<size_t>(stripIndex)] = {};
        }
        engine->clearPendingVisibleSlice();
        return;
    }

    strip->setSampleModeLegacyLoopEngineEnabled(false);
    if (sampleSliceId >= 0 || sampleStartSample >= 0)
        engine->triggerRecordedSlice(visibleSlot, sampleSliceId, sampleStartSample, isMomentaryStutter);
    else
        engine->triggerVisibleSlice(visibleSlot, isMomentaryStutter);
}

bool MlrVSTAudioProcessor::flipLegacyLoopCacheMatchesRenderKey(const FlipLegacyLoopSyncCache& entry,
                                                               const SampleModeEngine::LegacyLoopSyncInfo& syncInfo,
                                                               double hostTempo,
                                                               TimeStretchBackend backend,
                                                               float visibleBankBeats) const
{
    return entry.loadedSampleToken == syncInfo.loadedSample.get()
        && entry.visibleBankIndex == syncInfo.visibleBankIndex
        && entry.bankStartSample == syncInfo.bankStartSample
        && entry.bankEndSample == syncInfo.bankEndSample
        && entry.sliceSignature == computeFlipLegacyLoopSliceSignature(syncInfo.visibleSlices)
        && entry.warpSignature == computeFlipLegacyLoopWarpSignature(syncInfo.warpMarkers)
        && entry.legacyLoopBarSelection == syncInfo.legacyLoopBarSelection
        && entry.backend == backend
        && std::abs(entry.cachedSampleRate - syncInfo.loadedSample->sourceSampleRate) <= 1.0e-6
        && std::abs(entry.beatsPerLoop - visibleBankBeats) <= 1.0e-4f
        && (backend == TimeStretchBackend::Resample || std::abs(entry.hostTempo - hostTempo) <= 1.0e-4);
}

bool MlrVSTAudioProcessor::flipLegacyLoopCacheMatchesReusableAudioKey(const FlipLegacyLoopSyncCache& entry,
                                                                      const SampleModeEngine::LegacyLoopSyncInfo& syncInfo,
                                                                      double hostTempo,
                                                                      TimeStretchBackend backend,
                                                                      float visibleBankBeats) const
{
    return entry.renderValid
        && entry.loadedSampleToken == syncInfo.loadedSample.get()
        && entry.bankStartSample == syncInfo.bankStartSample
        && entry.bankEndSample == syncInfo.bankEndSample
        && entry.warpSignature == computeFlipLegacyLoopWarpSignature(syncInfo.warpMarkers)
        && entry.legacyLoopBarSelection == syncInfo.legacyLoopBarSelection
        && entry.backend == backend
        && std::abs(entry.cachedSampleRate - syncInfo.loadedSample->sourceSampleRate) <= 1.0e-6
        && std::abs(entry.beatsPerLoop - visibleBankBeats) <= 1.0e-4f
        && (backend == TimeStretchBackend::Resample || std::abs(entry.hostTempo - hostTempo) <= 1.0e-4);
}

void MlrVSTAudioProcessor::assignFlipLegacyLoopRenderKey(FlipLegacyLoopSyncCache& cache,
                                                         const SampleModeEngine::LegacyLoopSyncInfo& syncInfo,
                                                         double hostTempo,
                                                         TimeStretchBackend backend,
                                                         float visibleBankBeats) const
{
    cache.loadedSampleToken = syncInfo.loadedSample.get();
    cache.visibleBankIndex = syncInfo.visibleBankIndex;
    cache.bankStartSample = syncInfo.bankStartSample;
    cache.bankEndSample = syncInfo.bankEndSample;
    cache.sliceSignature = computeFlipLegacyLoopSliceSignature(syncInfo.visibleSlices);
    cache.warpSignature = computeFlipLegacyLoopWarpSignature(syncInfo.warpMarkers);
    cache.beatsPerLoop = visibleBankBeats;
    cache.legacyLoopBarSelection = syncInfo.legacyLoopBarSelection;
    cache.backend = backend;
    cache.hostTempo = hostTempo;
}

void MlrVSTAudioProcessor::applyFlipLegacyLoopRenderCacheToStrip(EnhancedAudioStrip& strip,
                                                                 const FlipLegacyLoopSyncCache& entry,
                                                                 TimeStretchBackend backend,
                                                                 float visibleBankBeats) const
{
    strip.loadSampleWithAnalysisCache(entry.cachedBankBuffer,
                                      entry.cachedSampleRate,
                                      entry.cachedTransientSliceStarts,
                                      entry.cachedRmsMap,
                                      entry.cachedZeroCrossMap,
                                      entry.cachedSourceLengthSamples);
    strip.setStretchBackend(backend);
    strip.setTransientSliceMode(true);
    strip.setLoop(0, MaxColumns);
    strip.setBeatsPerLoop(visibleBankBeats);
}

void MlrVSTAudioProcessor::applyFlipLegacyLoopTransientSliceCacheToStrip(EnhancedAudioStrip& strip,
                                                                         const FlipLegacyLoopSyncCache& entry,
                                                                         TimeStretchBackend backend,
                                                                         float visibleBankBeats) const
{
    strip.restoreSampleAnalysisCache(entry.cachedTransientSliceStarts,
                                     entry.cachedRmsMap,
                                     entry.cachedZeroCrossMap,
                                     entry.cachedSourceLengthSamples);
    strip.setStretchBackend(backend);
    strip.setTransientSliceMode(true);
    strip.setLoop(0, MaxColumns);
    strip.setBeatsPerLoop(visibleBankBeats);
}

bool MlrVSTAudioProcessor::queueFlipLegacyLoopRender(int preferredCacheIndex,
                                                     const SampleModeEngine::LegacyLoopSyncInfo& syncInfo,
                                                     double hostTempo,
                                                     TimeStretchBackend backend)
{
    if (preferredCacheIndex < 0
        || preferredCacheIndex >= MaxStrips
        || syncInfo.loadedSample == nullptr)
    {
        return false;
    }

    int64_t bankStartSample = 0;
    int64_t bankEndSample = 0;
    if (!computeFlipLegacyLoopBankRange(syncInfo, bankStartSample, bankEndSample))
        return false;

    const float visibleBankBeats = computeFlipLegacyLoopVisibleBankBeats(syncInfo);
    FlipLegacyLoopRenderRequest request;
    {
        const juce::SpinLock::ScopedLockType lock(flipLegacyLoopSyncCacheLock);
        auto& preferredCache = flipLegacyLoopSyncCache[static_cast<size_t>(preferredCacheIndex)];
        if (preferredCache.valid && flipLegacyLoopCacheMatchesRenderKey(preferredCache,
                                                                        syncInfo,
                                                                        hostTempo,
                                                                        backend,
                                                                        visibleBankBeats))
        {
            return true;
        }

        for (const auto& entry : flipLegacyLoopSyncCache)
        {
            if (flipLegacyLoopCacheMatchesRenderKey(entry, syncInfo, hostTempo, backend, visibleBankBeats)
                && (entry.renderValid || entry.renderInFlight))
            {
                return true;
            }
        }

        assignFlipLegacyLoopRenderKey(preferredCache, syncInfo, hostTempo, backend, visibleBankBeats);
        preferredCache.valid = false;
        preferredCache.renderValid = false;
        preferredCache.renderInFlight = true;
        preferredCache.stripApplied = false;
        preferredCache.cachedBankBuffer.setSize(0, 0);
        preferredCache.cachedSourceLengthSamples = 0;
        ++preferredCache.renderGeneration;

        request.cacheIndex = preferredCacheIndex;
        request.renderGeneration = preferredCache.renderGeneration;
        request.syncInfo = syncInfo;
        request.hostTempo = hostTempo;
        request.backend = backend;
        request.visibleBankBeats = visibleBankBeats;
        request.bankStartSample = bankStartSample;
        request.bankEndSample = bankEndSample;
    }

    flipLegacyLoopRenderThreadPool.addJob(new FlipLegacyLoopRenderJob(*this, std::move(request)), true);
    return true;
}

void MlrVSTAudioProcessor::pushFlipLegacyLoopRenderResult(FlipLegacyLoopRenderResult result)
{
    const juce::ScopedLock lock(flipLegacyLoopRenderResultLock);
    flipLegacyLoopRenderResults.push_back(std::move(result));
}

std::shared_ptr<const SampleModeEngine::LegacyLoopSyncInfo> MlrVSTAudioProcessor::getCachedFlipLegacyLoopSyncInfo(
    int stripIndex,
    SampleModeEngine& engine)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return {};

    const auto currentVersion = engine.getLegacyLoopRenderStateVersion();
    {
        const juce::SpinLock::ScopedLockType lock(flipLegacyLoopSyncInfoCacheLock);
        const auto& entry = flipLegacyLoopSyncInfoCache[static_cast<size_t>(stripIndex)];
        if (entry.syncInfo != nullptr && entry.version == currentVersion)
            return entry.syncInfo;
    }

    SampleModeEngine::LegacyLoopSyncInfo rebuiltSyncInfo;
    if (!engine.getLegacyLoopSyncInfo(rebuiltSyncInfo))
        return {};

    auto rebuiltPtr = std::make_shared<SampleModeEngine::LegacyLoopSyncInfo>(std::move(rebuiltSyncInfo));
    {
        const juce::SpinLock::ScopedLockType lock(flipLegacyLoopSyncInfoCacheLock);
        auto& entry = flipLegacyLoopSyncInfoCache[static_cast<size_t>(stripIndex)];
        entry.version = currentVersion;
        entry.syncInfo = rebuiltPtr;
        return entry.syncInfo;
    }
}

void MlrVSTAudioProcessor::applyCompletedFlipLegacyLoopRenders()
{
    std::vector<FlipLegacyLoopRenderResult> completed;
    {
        const juce::ScopedLock lock(flipLegacyLoopRenderResultLock);
        if (flipLegacyLoopRenderResults.empty())
            return;
        completed.swap(flipLegacyLoopRenderResults);
    }

    const juce::SpinLock::ScopedLockType cacheLock(flipLegacyLoopSyncCacheLock);
    for (auto& result : completed)
    {
        if (result.cacheIndex < 0 || result.cacheIndex >= MaxStrips)
            continue;

        auto& cache = flipLegacyLoopSyncCache[static_cast<size_t>(result.cacheIndex)];
        if (cache.renderGeneration != result.renderGeneration)
            continue;

        cache = std::move(result.cacheEntry);
        cache.renderGeneration = result.renderGeneration;
        cache.renderInFlight = false;
    }
}

void MlrVSTAudioProcessor::prewarmFlipLegacyLoopRenders()
{
    if (audioEngine == nullptr)
        return;

    const double hostTempo = audioEngine->getCurrentTempo();
    const auto backend = getFlipTempoMatchBackend();
    for (int stripIndex = 0; stripIndex < MaxStrips; ++stripIndex)
    {
        auto* strip = audioEngine->getStrip(stripIndex);
        auto* engine = getSampleModeEngine(stripIndex, false);
        if (strip == nullptr
            || engine == nullptr
            || strip->getPlayMode() != EnhancedAudioStrip::PlayMode::Sample
            || !engine->isLegacyLoopEngineEnabled())
        {
            continue;
        }

        if (auto syncInfoPtr = getCachedFlipLegacyLoopSyncInfo(stripIndex, *engine))
        {
            const auto& syncInfo = *syncInfoPtr;
            queueFlipLegacyLoopRender(stripIndex, syncInfo, hostTempo, backend);
        }
    }
}

bool MlrVSTAudioProcessor::syncFlipLegacyLoopStripState(int stripIndex,
                                                        EnhancedAudioStrip& strip,
                                                        const SampleModeEngine::LegacyLoopSyncInfo& syncInfo,
                                                        double hostTempo,
                                                        double hostPpq,
                                                        int64_t currentGlobalSample,
                                                        bool preservePlaybackState,
                                                        TimeStretchBackend backend,
                                                        bool allowInlineBuild)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips || syncInfo.loadedSample == nullptr)
        return false;

    const float visibleBankBeats = computeFlipLegacyLoopVisibleBankBeats(syncInfo);
    int64_t bankStartSample = 0;
    int64_t bankEndSample = 0;
    if (!computeFlipLegacyLoopBankRange(syncInfo, bankStartSample, bankEndSample))
        return false;
    const auto desiredTransientSliceStarts = buildFlipLegacyLoopTransientSliceCache(syncInfo,
                                                                                    bankStartSample,
                                                                                    bankEndSample);
    FlipLegacyLoopSyncCache renderedCache;
    bool hasRenderedCache = false;
    bool builtInlineRender = false;
    {
        const juce::SpinLock::ScopedLockType lock(flipLegacyLoopSyncCacheLock);
        auto& cache = flipLegacyLoopSyncCache[static_cast<size_t>(stripIndex)];
        const bool needsSync = !cache.valid
            || !cache.renderValid
            || !flipLegacyLoopCacheMatchesRenderKey(cache, syncInfo, hostTempo, backend, visibleBankBeats);
        if (!needsSync)
        {
            if (cache.cachedTransientSliceStarts != desiredTransientSliceStarts
                || !cache.stripApplied
                || !strip.hasAudio())
            {
                renderedCache = cache;
                hasRenderedCache = cache.renderValid;
            }
            else
            {
                return true;
            }
        }

        if (!hasRenderedCache)
        {
            for (const auto& entry : flipLegacyLoopSyncCache)
            {
                if (entry.renderValid
                    && flipLegacyLoopCacheMatchesRenderKey(entry, syncInfo, hostTempo, backend, visibleBankBeats))
                {
                    renderedCache = entry;
                    hasRenderedCache = true;
                    break;
                }
            }
        }
    }

    if (!hasRenderedCache)
    {
        if (!allowInlineBuild)
        {
            queueFlipLegacyLoopRender(stripIndex, syncInfo, hostTempo, backend);
            return false;
        }

        juce::AudioBuffer<float> bankBuffer;
        if (!buildFlipLegacyLoopBankBuffer(syncInfo,
                                           hostTempo,
                                           backend,
                                           visibleBankBeats,
                                           bankBuffer))
        {
            queueFlipLegacyLoopRender(stripIndex, syncInfo, hostTempo, backend);
            return false;
        }

        assignFlipLegacyLoopRenderKey(renderedCache,
                                      syncInfo,
                                      hostTempo,
                                      backend,
                                      visibleBankBeats);
        renderedCache.cachedBankBuffer = std::move(bankBuffer);
        renderedCache.cachedTransientSliceStarts = desiredTransientSliceStarts;
        buildFlipLegacyLoopAnalysisMaps(renderedCache.cachedBankBuffer,
                                        renderedCache.cachedRmsMap,
                                        renderedCache.cachedZeroCrossMap);
        renderedCache.cachedSourceLengthSamples = static_cast<int>(
            juce::jmax<int64_t>(1, bankEndSample - bankStartSample));
        renderedCache.cachedSampleRate = syncInfo.loadedSample->sourceSampleRate;
        renderedCache.renderValid = renderedCache.cachedBankBuffer.getNumSamples() > 0;
        renderedCache.renderInFlight = false;
        renderedCache.valid = false;
        renderedCache.stripApplied = false;
        hasRenderedCache = renderedCache.renderValid;
        builtInlineRender = hasRenderedCache;
        if (!hasRenderedCache)
            return false;
    }

    renderedCache.cachedTransientSliceStarts = desiredTransientSliceStarts;

    const bool shouldRestorePlayback = preservePlaybackState
        && strip.isPlaying()
        && strip.isPpqTimelineAnchored()
        && std::isfinite(hostPpq)
        && hostTempo > 0.0
        && currentGlobalSample >= 0;
    const int restoreColumn = shouldRestorePlayback ? strip.getCurrentColumn() : 0;
    const double restoreOffsetBeats = shouldRestorePlayback ? strip.getPpqTimelineOffsetBeats() : 0.0;

    applyFlipLegacyLoopRenderCacheToStrip(strip, renderedCache, backend, visibleBankBeats);

    if (shouldRestorePlayback)
    {
        strip.restorePresetPpqState(true,
                                    true,
                                    restoreOffsetBeats,
                                    restoreColumn,
                                    hostTempo,
                                    hostPpq,
                                    currentGlobalSample);
    }

    {
        const juce::SpinLock::ScopedLockType lock(flipLegacyLoopSyncCacheLock);
        auto& cache = flipLegacyLoopSyncCache[static_cast<size_t>(stripIndex)];
        if (!builtInlineRender
            && flipLegacyLoopCacheMatchesRenderKey(cache, syncInfo, hostTempo, backend, visibleBankBeats)
            && cache.renderValid)
        {
            cache.valid = true;
            cache.stripApplied = true;
        }
        else
        {
            const auto existingGeneration = cache.renderGeneration;
            const bool existingRenderInFlight = cache.renderInFlight;
            cache = renderedCache;
            cache.valid = true;
            cache.stripApplied = true;
            cache.renderGeneration = existingGeneration + (existingRenderInFlight ? 1ull : 0ull);
            cache.renderInFlight = false;
        }
    }

    return true;
}

bool MlrVSTAudioProcessor::copyFlipCurrentSlicesToMode(int stripIndex, EnhancedAudioStrip::PlayMode targetMode)
{
    return copyFlipCurrentSlicesToMode(stripIndex, stripIndex, targetMode);
}

bool MlrVSTAudioProcessor::copyFlipCurrentSlicesToMode(int sourceStripIndex,
                                                       int targetStripIndex,
                                                       EnhancedAudioStrip::PlayMode targetMode)
{
    if (sourceStripIndex < 0 || sourceStripIndex >= MaxStrips
        || targetStripIndex < 0 || targetStripIndex >= MaxStrips)
        return false;
    if (targetMode != EnhancedAudioStrip::PlayMode::Loop
        && targetMode != EnhancedAudioStrip::PlayMode::Grain)
        return false;

    auto* sourceStrip = audioEngine != nullptr ? audioEngine->getStrip(sourceStripIndex) : nullptr;
    auto* sourceEngine = getSampleModeEngine(sourceStripIndex, false);
    auto* targetStrip = audioEngine != nullptr ? audioEngine->getStrip(targetStripIndex) : nullptr;
    if (sourceStrip == nullptr || sourceEngine == nullptr || targetStrip == nullptr)
        return false;

    SampleModeEngine::LegacyLoopSyncInfo syncInfo;
    const auto stretchBackend = getFlipTempoMatchBackend();
    const auto targetPlaybackBackend = getStretchBackend();
    if (!sourceEngine->getLegacyLoopSyncInfo(syncInfo))
        return false;

    const double hostTempo = audioEngine != nullptr ? audioEngine->getCurrentTempo() : 120.0;
    const float transferredBeatsPerLoop = computeFlipLegacyLoopVisibleBankBeats(syncInfo);
    int transferredRecordingBars = 1;
    if (syncInfo.legacyLoopBarSelection > 0)
    {
        transferredRecordingBars = decodeBarSelection(syncInfo.legacyLoopBarSelection).recordingBars;
    }
    else
    {
        const float clampedBeats = juce::jlimit(1.0f, 32.0f, transferredBeatsPerLoop);
        if (clampedBeats <= 4.0f)
            transferredRecordingBars = 1;
        else if (clampedBeats <= 8.0f)
            transferredRecordingBars = 2;
        else if (clampedBeats <= 16.0f)
            transferredRecordingBars = 4;
        else
            transferredRecordingBars = 8;
    }

    juce::AudioBuffer<float> bankBuffer;
    FlipLegacyLoopSyncCache renderedCache;
    bool hasRenderedCache = false;
    {
        const juce::SpinLock::ScopedLockType lock(flipLegacyLoopSyncCacheLock);
        for (const auto& entry : flipLegacyLoopSyncCache)
        {
            if (entry.renderValid
                && flipLegacyLoopCacheMatchesRenderKey(entry,
                                                       syncInfo,
                                                       hostTempo,
                                                       stretchBackend,
                                                       transferredBeatsPerLoop))
            {
                renderedCache = entry;
                hasRenderedCache = true;
                break;
            }
        }
    }

    if (hasRenderedCache && renderedCache.cachedBankBuffer.getNumSamples() > 0)
    {
        bankBuffer = renderedCache.cachedBankBuffer;
    }
    else
    {
        if (!buildFlipLegacyLoopBankBuffer(syncInfo,
                                           hostTempo,
                                           stretchBackend,
                                           transferredBeatsPerLoop,
                                           bankBuffer))
            return false;

        auto& sourceCache = flipLegacyLoopSyncCache[static_cast<size_t>(sourceStripIndex)];
        int64_t bankStartSample = 0;
        int64_t bankEndSample = 0;
        std::array<int, SliceModel::VisibleSliceCount> transientSliceCache {};
        if (computeFlipLegacyLoopBankRange(syncInfo, bankStartSample, bankEndSample))
            transientSliceCache = buildFlipLegacyLoopTransientSliceCache(syncInfo, bankStartSample, bankEndSample);
        std::array<float, 128> rmsMap {};
        std::array<int, 128> zeroCrossMap {};
        buildFlipLegacyLoopAnalysisMaps(bankBuffer, rmsMap, zeroCrossMap);

        {
            const juce::SpinLock::ScopedLockType lock(flipLegacyLoopSyncCacheLock);
            assignFlipLegacyLoopRenderKey(sourceCache,
                                          syncInfo,
                                          hostTempo,
                                          stretchBackend,
                                          transferredBeatsPerLoop);
            sourceCache.cachedBankBuffer = bankBuffer;
            sourceCache.cachedTransientSliceStarts = transientSliceCache;
            sourceCache.cachedRmsMap = rmsMap;
            sourceCache.cachedZeroCrossMap = zeroCrossMap;
            sourceCache.cachedSourceLengthSamples = bankBuffer.getNumSamples();
            sourceCache.cachedSampleRate = syncInfo.loadedSample->sourceSampleRate;
            sourceCache.renderValid = true;
            sourceCache.renderInFlight = false;
            sourceCache.valid = false;
            ++sourceCache.renderGeneration;
        }
    }

    if (auto* targetFlipEngine = getSampleModeEngine(targetStripIndex, false))
    {
        targetFlipEngine->stop(true);
        targetFlipEngine->clear();
    }
    stopSampleModeStrip(targetStripIndex, true);
    targetStrip->stop(true);
    targetStrip->setSampleModeLegacyLoopEngineEnabled(false);
    targetStrip->loadSample(bankBuffer, syncInfo.loadedSample->sourceSampleRate);
    targetStrip->setStretchBackend(targetPlaybackBackend);
    targetStrip->setTransientSliceMode(true);
    targetStrip->setLoop(0, MaxColumns);
    targetStrip->setPlayMode(targetMode);
    targetStrip->setRecordingBars(transferredRecordingBars);
    targetStrip->setBeatsPerLoop(transferredBeatsPerLoop);
    currentStripFiles[static_cast<size_t>(targetStripIndex)] = juce::File();
    invalidateFlipLegacyLoopSync(targetStripIndex);

    if (sourceStripIndex == targetStripIndex)
    {
        sourceEngine->clearPendingVisibleSlice();
        sourceEngine->stop(true);
    }

    return true;
}

void MlrVSTAudioProcessor::invalidateFlipLegacyLoopSync(int stripIndex)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;

    const juce::SpinLock::ScopedLockType lock(flipLegacyLoopSyncCacheLock);
    auto& cache = flipLegacyLoopSyncCache[static_cast<size_t>(stripIndex)];
    cache.valid = false;
    cache.stripApplied = false;
    if (cache.renderInFlight)
    {
        cache.renderInFlight = false;
        ++cache.renderGeneration;
    }
}

void MlrVSTAudioProcessor::stopSampleModeStrip(int stripIndex, bool immediateStop)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;

    sampleModeRenderedLastBlock[static_cast<size_t>(stripIndex)] = false;

    auto* strip = audioEngine != nullptr ? audioEngine->getStrip(stripIndex) : nullptr;
    bool usedLegacyLoopEngine = false;
    if (auto* engine = getSampleModeEngine(stripIndex, false))
        usedLegacyLoopEngine = engine->isLegacyLoopEngineEnabled();
    if (strip != nullptr)
    {
        usedLegacyLoopEngine = usedLegacyLoopEngine || strip->isSampleModeLegacyLoopEngineEnabled();
        strip->setSampleModeLegacyLoopEngineEnabled(false);
        if (usedLegacyLoopEngine)
            strip->stop(immediateStop);
    }

    if (audioEngine != nullptr)
    {
        audioEngine->clearPendingQuantizedTriggersForStrip(stripIndex);
    }

    if (auto* engine = getSampleModeEngine(stripIndex, false))
    {
        engine->clearPendingVisibleSlice();
        engine->stop(immediateStop);
    }
    {
        const juce::SpinLock::ScopedLockType lock(pendingFlipLegacyLoopTriggerLock);
        pendingFlipLegacyLoopTriggers[static_cast<size_t>(stripIndex)] = {};
    }
    clearSampleModeHeldVisibleSliceSlot(stripIndex);
}

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

int MlrVSTAudioProcessor::getActiveMainPresetIndexForScenes() const
{
    if (loadedPresetIndex >= 0 && loadedPresetIndex < MaxPresetSlots)
        return loadedPresetIndex;
    return juce::jlimit(0, MaxPresetSlots - 1, activeSceneMainPresetIndex);
}

int MlrVSTAudioProcessor::getSceneStoragePresetIndex(int mainPresetIndex, int sceneSlot) const
{
    const int clampedMain = juce::jlimit(0, MaxPresetSlots - 1, mainPresetIndex);
    const int clampedSlot = juce::jlimit(0, SceneSlots - 1, sceneSlot);
    return MaxPresetSlots + (clampedMain * SceneSlots) + clampedSlot;
}

bool MlrVSTAudioProcessor::sceneSlotExistsForMainPreset(int mainPresetIndex, int sceneSlot) const
{
    return PresetStore::presetHasLaunchableSceneContent(getSceneStoragePresetIndex(mainPresetIndex, sceneSlot));
}

int MlrVSTAudioProcessor::getSceneRepeatCount(int sceneSlot) const
{
    return SceneScheduler::getSceneRepeatCount(*this, sceneSlot);
}

void MlrVSTAudioProcessor::setSceneRepeatCount(int sceneSlot, int repeats)
{
    SceneScheduler::setSceneRepeatCount(*this, sceneSlot, repeats);
}

MlrVSTAudioProcessor::SceneLengthMode MlrVSTAudioProcessor::getSceneLengthMode(int sceneSlot) const
{
    return static_cast<SceneLengthMode>(SceneScheduler::getSceneLengthModeIndex(*this, sceneSlot));
}

void MlrVSTAudioProcessor::setSceneLengthMode(int sceneSlot, SceneLengthMode mode)
{
    SceneScheduler::setSceneLengthModeIndex(*this, sceneSlot, static_cast<int>(mode));
}

MlrVSTAudioProcessor::SceneRecallMode MlrVSTAudioProcessor::getSceneRecallMode() const
{
    return static_cast<SceneRecallMode>(SceneScheduler::getSceneRecallModeIndex(*this));
}

void MlrVSTAudioProcessor::setSceneRecallMode(SceneRecallMode mode)
{
    SceneScheduler::setSceneRecallModeIndex(*this, static_cast<int>(mode));
}

int MlrVSTAudioProcessor::getSceneManualBars(int sceneSlot) const
{
    return SceneScheduler::getSceneManualBars(*this, sceneSlot);
}

void MlrVSTAudioProcessor::setSceneManualBars(int sceneSlot, int bars)
{
    SceneScheduler::setSceneManualBars(*this, sceneSlot, bars);
}

int MlrVSTAudioProcessor::getSceneAnchorStrip(int sceneSlot) const
{
    return SceneScheduler::getSceneAnchorStrip(*this, sceneSlot);
}

void MlrVSTAudioProcessor::setSceneAnchorStrip(int sceneSlot, int stripIndex)
{
    SceneScheduler::setSceneAnchorStrip(*this, sceneSlot, stripIndex);
}

int MlrVSTAudioProcessor::getSceneLengthCount(int sceneSlot) const
{
    return SceneScheduler::getSceneLengthCount(*this, sceneSlot);
}

void MlrVSTAudioProcessor::setSceneLengthCount(int sceneSlot, int count)
{
    SceneScheduler::setSceneLengthCount(*this, sceneSlot, count);
}

double MlrVSTAudioProcessor::getResolvedSceneLengthBeats(int sceneSlot) const
{
    return SceneScheduler::getResolvedSceneLengthBeats(*this, sceneSlot);
}

double MlrVSTAudioProcessor::getSceneAdvanceLengthBeats(int sceneSlot) const
{
    return SceneScheduler::getSceneAdvanceLengthBeats(*this, sceneSlot);
}

bool MlrVSTAudioProcessor::persistSceneTimingForSlot(int sceneSlot)
{
    return SceneScheduler::persistSceneTimingForSlot(*this, sceneSlot);
}

int MlrVSTAudioProcessor::getSceneSequenceStepIndex(int sceneSlot) const
{
    return SceneScheduler::getSceneSequenceStepIndex(*this, sceneSlot);
}

int MlrVSTAudioProcessor::getQueuedSceneSlot() const
{
    return SceneScheduler::getQueuedSceneSlot(*this);
}

juce::String MlrVSTAudioProcessor::getSceneSequenceSummaryText() const
{
    return SceneScheduler::getSceneSequenceSummaryText(*this);
}

int MlrVSTAudioProcessor::getSceneChainLength() const
{
    return SceneScheduler::getSceneChainLength(*this);
}

int MlrVSTAudioProcessor::getSceneChainStepSceneSlot(int stepIndex) const
{
    return SceneScheduler::getSceneChainStepSceneSlot(*this, stepIndex);
}

int MlrVSTAudioProcessor::getSceneChainStepRepeatCount(int stepIndex) const
{
    return SceneScheduler::getSceneChainStepRepeatCount(*this, stepIndex);
}

MlrVSTAudioProcessor::SceneChainTransitionType MlrVSTAudioProcessor::getSceneChainStepTransitionType(int stepIndex) const
{
    return static_cast<SceneChainTransitionType>(
        SceneScheduler::getSceneChainStepTransitionTypeIndex(*this, stepIndex));
}

MlrVSTAudioProcessor::SceneChainTransitionOption MlrVSTAudioProcessor::getSceneChainStepTransitionOption(int stepIndex) const
{
    return static_cast<SceneChainTransitionOption>(
        SceneScheduler::getSceneChainStepTransitionOptionIndex(*this, stepIndex));
}

float MlrVSTAudioProcessor::getSceneChainStepTransitionLengthBeats(int stepIndex) const
{
    return SceneScheduler::getSceneChainStepTransitionLengthBeats(*this, stepIndex);
}

bool MlrVSTAudioProcessor::getSceneChainStepTransitionSubtractsFromSceneLength(int stepIndex) const
{
    return SceneScheduler::getSceneChainStepTransitionSubtractsFromSceneLength(*this, stepIndex);
}

float MlrVSTAudioProcessor::getSceneChainStepTransitionIntensity(int stepIndex) const
{
    return SceneScheduler::getSceneChainStepTransitionIntensity(*this, stepIndex);
}

float MlrVSTAudioProcessor::getSceneChainStepTransitionDelayAmount(int stepIndex) const
{
    return SceneScheduler::getSceneChainStepTransitionDelayAmount(*this, stepIndex);
}

float MlrVSTAudioProcessor::getSceneChainStepTransitionFilterAmount(int stepIndex) const
{
    return SceneScheduler::getSceneChainStepTransitionFilterAmount(*this, stepIndex);
}

float MlrVSTAudioProcessor::getSceneChainStepTransitionChopAmount(int stepIndex) const
{
    return SceneScheduler::getSceneChainStepTransitionChopAmount(*this, stepIndex);
}

void MlrVSTAudioProcessor::setSceneChainStep(int stepIndex, int sceneSlot, int repeats)
{
    SceneScheduler::setSceneChainStep(*this, stepIndex, sceneSlot, repeats);
}

void MlrVSTAudioProcessor::setSceneChainStepTransitionType(int stepIndex, SceneChainTransitionType type)
{
    SceneScheduler::setSceneChainStepTransitionTypeIndex(*this, stepIndex, static_cast<int>(type));
}

void MlrVSTAudioProcessor::setSceneChainStepTransitionOption(int stepIndex, SceneChainTransitionOption option)
{
    SceneScheduler::setSceneChainStepTransitionOptionIndex(*this, stepIndex, static_cast<int>(option));
}

void MlrVSTAudioProcessor::setSceneChainStepTransitionLengthBeats(int stepIndex, float beats)
{
    SceneScheduler::setSceneChainStepTransitionLengthBeats(*this, stepIndex, beats);
}

void MlrVSTAudioProcessor::setSceneChainStepTransitionSubtractsFromSceneLength(int stepIndex, bool enabled)
{
    SceneScheduler::setSceneChainStepTransitionSubtractsFromSceneLength(*this, stepIndex, enabled);
}

void MlrVSTAudioProcessor::setSceneChainStepTransitionIntensity(int stepIndex, float amount)
{
    SceneScheduler::setSceneChainStepTransitionIntensity(*this, stepIndex, amount);
}

void MlrVSTAudioProcessor::setSceneChainStepTransitionDelayAmount(int stepIndex, float amount)
{
    SceneScheduler::setSceneChainStepTransitionDelayAmount(*this, stepIndex, amount);
}

void MlrVSTAudioProcessor::setSceneChainStepTransitionFilterAmount(int stepIndex, float amount)
{
    SceneScheduler::setSceneChainStepTransitionFilterAmount(*this, stepIndex, amount);
}

void MlrVSTAudioProcessor::setSceneChainStepTransitionChopAmount(int stepIndex, float amount)
{
    SceneScheduler::setSceneChainStepTransitionChopAmount(*this, stepIndex, amount);
}

void MlrVSTAudioProcessor::clearSceneChain()
{
    SceneScheduler::clearSceneChain(*this);
}

bool MlrVSTAudioProcessor::isSceneChainLoopEnabled() const
{
    return SceneScheduler::isSceneChainLoopEnabled(*this);
}

void MlrVSTAudioProcessor::setSceneChainLoopEnabled(bool enabled)
{
    SceneScheduler::setSceneChainLoopEnabled(*this, enabled);
}

int MlrVSTAudioProcessor::getSceneChainLoopStartStep() const
{
    return SceneScheduler::getSceneChainLoopStartStep(*this);
}

int MlrVSTAudioProcessor::getSceneChainLoopEndStep() const
{
    return SceneScheduler::getSceneChainLoopEndStep(*this);
}

void MlrVSTAudioProcessor::setSceneChainLoopRange(int startStep, int endStep)
{
    SceneScheduler::setSceneChainLoopRange(*this, startStep, endStep);
}

bool MlrVSTAudioProcessor::isSceneChainPlaybackActive() const
{
    return SceneScheduler::isSceneChainPlaybackActive(*this);
}

int MlrVSTAudioProcessor::getSceneChainPlaybackStepIndex() const
{
    return SceneScheduler::getSceneChainPlaybackStepIndex(*this);
}

bool MlrVSTAudioProcessor::startSceneChainPlayback(int startStepIndex)
{
    return SceneScheduler::startSceneChainPlayback(*this, startStepIndex);
}

void MlrVSTAudioProcessor::stopSceneChainPlayback()
{
    SceneScheduler::stopSceneChainPlayback(*this);
}

void MlrVSTAudioProcessor::selectSceneSlotFromSurface(int sceneSlot)
{
    const int safeSceneSlot = juce::jlimit(0, SceneSlots - 1, sceneSlot);
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

    if (targetsCurrentLiveScene)
    {
        if (activeSceneNeedsCaptureBeforeManualRecall)
        {
            const bool captured = captureSceneSlot(safeSceneSlot);
            appendSceneDebugLog("surface_select capture_current slot=" + juce::String(safeSceneSlot)
                + " success=" + juce::String(captured ? 1 : 0));
            updateMonomeLEDs();
            if (!captured)
                return;
        }
    }

    pendingSceneApplyMainPreset.store(-1, std::memory_order_release);
    pendingSceneApplySlot.store(-1, std::memory_order_release);
    pendingSceneApplySequenceDriven.store(0, std::memory_order_release);
    pendingSceneApplySequenceStep.store(-1, std::memory_order_release);
    pendingSceneApplyTargetPpq.store(-1.0, std::memory_order_release);
    pendingSceneApplyTargetTempo.store(120.0, std::memory_order_release);
    pendingSceneApplyTargetSample.store(-1, std::memory_order_release);
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
    preparedSceneRenderContext.reset();

    requestSceneRecallQuantized(mainPresetIndex, safeSceneSlot, false, -1, true);
    appendSceneDebugLog("surface_select queued_recall slot=" + juce::String(safeSceneSlot)
        + " mainPreset=" + juce::String(mainPresetIndex));
    updateMonomeLEDs();
}

void MlrVSTAudioProcessor::recallSceneSlot(int sceneSlot)
{
    const int targetSceneSlot = juce::jlimit(0, SceneSlots - 1, sceneSlot);
    const bool exitingSceneChain = sceneSequenceActive;
    if (exitingSceneChain)
    {
        appendSceneDebugLog("recall_slot stop_chain slot=" + juce::String(targetSceneSlot));
        stopSceneChainPlayback();
    }
    const int mainPresetIndex = getActiveMainPresetIndexForScenes();
    appendSceneDebugLog("recall_slot slot=" + juce::String(targetSceneSlot)
        + " mainPreset=" + juce::String(mainPresetIndex)
        + " activeScene=" + juce::String(activeSceneSlot)
        + " activeMain=" + juce::String(activeSceneMainPresetIndex)
        + " needsCapture=" + juce::String(activeSceneNeedsCaptureBeforeManualRecall ? 1 : 0)
        + " exists=" + juce::String(sceneSlotExistsForMainPreset(mainPresetIndex, targetSceneSlot) ? 1 : 0));
    if (!sceneSequenceActive
        && activeSceneMainPresetIndex == mainPresetIndex
        && activeSceneSlot == targetSceneSlot)
    {
        if (activeSceneNeedsCaptureBeforeManualRecall)
        {
            const bool captured = captureSceneSlot(targetSceneSlot);
            appendSceneDebugLog("recall_slot capture_current slot=" + juce::String(targetSceneSlot)
                + " success=" + juce::String(captured ? 1 : 0));
            updateMonomeLEDs();
            if (!captured)
                return;
        }
    }

    requestSceneRecallQuantized(mainPresetIndex, targetSceneSlot, false, -1);
    appendSceneDebugLog("recall_slot queued_recall slot=" + juce::String(targetSceneSlot)
        + " mainPreset=" + juce::String(mainPresetIndex));
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
    scenePerformanceRecorder.stopRecording();
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

    if (safeSceneSlot == activeSceneSlot)
    {
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

bool MlrVSTAudioProcessor::isScenePerformanceRecording() const
{
    return scenePerformanceRecorder.isRecording();
}

bool MlrVSTAudioProcessor::isScenePerformanceOverdubbing() const
{
    return scenePerformanceRecorder.isOverdubbing();
}

int MlrVSTAudioProcessor::getScenePerformanceRecordingSceneSlot() const
{
    return scenePerformanceRecorder.getRecordingSceneSlot();
}

double MlrVSTAudioProcessor::getScenePerformanceRecordingStartBeat() const
{
    return scenePerformanceRecorder.getRecordingStartBeat();
}

double MlrVSTAudioProcessor::getScenePerformanceRecordingEndBeat() const
{
    return scenePerformanceRecorder.getRecordingEndBeat();
}

double MlrVSTAudioProcessor::getScenePerformanceClipLengthBeats(int sceneSlot) const
{
    return getResolvedSceneLengthBeats(sceneSlot);
}

bool MlrVSTAudioProcessor::hasScenePerformanceClip(int sceneSlot) const
{
    return scenePerformanceRecorder.hasEvents(sceneSlot);
}

int MlrVSTAudioProcessor::getScenePerformanceEventCount(int sceneSlot) const
{
    return scenePerformanceRecorder.getEventCount(sceneSlot);
}

double MlrVSTAudioProcessor::getScenePerformancePlaybackProgress(int sceneSlot, double currentBeat) const
{
    if (!activeSceneStartPpqValid || !std::isfinite(activeSceneStartPpq))
        return -1.0;

    return scenePerformanceRecorder.getPlaybackProgressForBeat(sceneSlot, activeSceneStartPpq, currentBeat);
}

double MlrVSTAudioProcessor::getScenePerformanceRecordingProgress(double currentBeat) const
{
    return scenePerformanceRecorder.getRecordingProgressForBeat(currentBeat);
}

std::vector<ScenePerformanceEvent> MlrVSTAudioProcessor::getScenePerformanceEventsSnapshot(int sceneSlot) const
{
    return scenePerformanceRecorder.getEventsSnapshot(sceneSlot);
}

bool MlrVSTAudioProcessor::copySceneSlotToClipboard(int sceneSlot)
{
    const int safeSceneSlot = juce::jlimit(0, SceneSlots - 1, sceneSlot);
    const int mainPresetIndex = getActiveMainPresetIndexForScenes();

    if (safeSceneSlot == activeSceneSlot)
    {
        if (!captureSceneSlot(safeSceneSlot))
            return false;
    }

    if (!sceneSlotExistsForMainPreset(mainPresetIndex, safeSceneSlot))
        return false;

    sceneSlotClipboardSourceSlot = safeSceneSlot;
    sceneSlotClipboardMainPresetIndex = mainPresetIndex;
    return true;
}

bool MlrVSTAudioProcessor::pasteSceneSlotFromClipboard(int sceneSlot)
{
    if (sceneSlotClipboardSourceSlot < 0)
        return false;

    const int safeSceneSlot = juce::jlimit(0, SceneSlots - 1, sceneSlot);
    const int mainPresetIndex = getActiveMainPresetIndexForScenes();
    if (sceneSlotClipboardMainPresetIndex != mainPresetIndex)
        return false;

    const bool pastingIntoActiveScene = isSceneModeEnabled()
        && audioEngine != nullptr
        && activeSceneMainPresetIndex == juce::jlimit(0, MaxPresetSlots - 1, mainPresetIndex)
        && activeSceneSlot == safeSceneSlot;

    const bool pasted = copySceneForMainPreset(mainPresetIndex,
                                               sceneSlotClipboardSourceSlot,
                                               safeSceneSlot);
    if (!pasted)
        return false;

    if (sceneSlotHasMotionState(sceneSlotClipboardSourceSlot))
    {
        for (int stripIndex = 0; stripIndex < MaxStrips; ++stripIndex)
        {
            scenePerformanceRecorder.setMotionStripState(
                safeSceneSlot,
                stripIndex,
                scenePerformanceRecorder.getMotionStripState(sceneSlotClipboardSourceSlot, stripIndex));
        }

        if (safeSceneSlot == activeSceneSlot && isSceneModeEnabled())
            applySceneMotionStateToEngine(safeSceneSlot);
    }
    else
    {
        for (int stripIndex = 0; stripIndex < MaxStrips; ++stripIndex)
            clearSceneMotionStripState(safeSceneSlot, stripIndex);
    }

    if (pastingIntoActiveScene)
    {
        double hostPpqSnapshot = std::numeric_limits<double>::quiet_NaN();
        double hostTempoSnapshot = 120.0;
        if (!getHostSyncSnapshot(hostPpqSnapshot, hostTempoSnapshot) && audioEngine != nullptr)
        {
            const double fallbackPpq = audioEngine->getTimelineBeat();
            const double fallbackTempo = juce::jmax(1.0, audioEngine->getCurrentTempo());
            if (std::isfinite(fallbackPpq) && std::isfinite(fallbackTempo) && fallbackTempo > 0.0)
            {
                hostPpqSnapshot = fallbackPpq;
                hostTempoSnapshot = fallbackTempo;
            }
        }

        performSceneLoad(mainPresetIndex,
                         safeSceneSlot,
                         hostPpqSnapshot,
                         hostTempoSnapshot,
                         audioEngine != nullptr ? audioEngine->getGlobalSampleCount() : -1);
    }

    updateMonomeLEDs();
    return true;
}

bool MlrVSTAudioProcessor::hasSceneSlotClipboard() const
{
    return sceneSlotClipboardSourceSlot >= 0;
}

int MlrVSTAudioProcessor::getSceneSlotClipboardSourceSlot() const
{
    return sceneSlotClipboardSourceSlot;
}

bool MlrVSTAudioProcessor::clearSceneSlot(int sceneSlot)
{
    const int safeSceneSlot = juce::jlimit(0, SceneSlots - 1, sceneSlot);
    const int mainPresetIndex = getActiveMainPresetIndexForScenes();
    const bool clearingActiveScene = isSceneModeEnabled()
        && audioEngine != nullptr
        && activeSceneMainPresetIndex == juce::jlimit(0, MaxPresetSlots - 1, mainPresetIndex)
        && activeSceneSlot == safeSceneSlot;
    ScopedSceneAutosaveSuppression suppressSceneAutosave(*this);

    setSceneRepeatCount(safeSceneSlot, 1);
    setSceneLengthMode(safeSceneSlot, SceneLengthMode::ManualBars);
    setSceneLengthCount(safeSceneSlot, 4);
    setSceneAnchorStrip(safeSceneSlot, 0);

    for (int stripIndex = 0; stripIndex < MaxStrips; ++stripIndex)
        clearSceneMotionStripState(safeSceneSlot, stripIndex);
    clearScenePerformanceClip(safeSceneSlot);

    const auto preservedSceneTimingState = createSceneChainStateXml(-1);
    const auto preservedScenePerformanceState = createScenePerformanceStateData(-1);

    deleteSceneForMainPreset(mainPresetIndex, safeSceneSlot);

    if (safeSceneSlot == activeSceneSlot)
    {
        clearPendingActiveSceneAutosave();
        activeSceneNeedsCaptureBeforeManualRecall = false;
    }

    if (!clearingActiveScene)
        return true;

    struct ScopedSuspendProcessing
    {
        explicit ScopedSuspendProcessing(MlrVSTAudioProcessor& p) : processor(p) { processor.suspendProcessing(true); }
        ~ScopedSuspendProcessing() { processor.suspendProcessing(false); }
        MlrVSTAudioProcessor& processor;
    } scopedSuspend(*this);

    resetRuntimePresetStateToDefaults();
    activeSceneMainPresetIndex = juce::jlimit(0, MaxPresetSlots - 1, mainPresetIndex);
    activeSceneSlot = safeSceneSlot;
    activeSceneNeedsCaptureBeforeManualRecall = false;

    if (preservedSceneTimingState != nullptr)
        applySceneChainStateXml(preservedSceneTimingState.get(), -1);

    if (preservedScenePerformanceState.getSize() > 0)
        applyScenePerformanceStateData(preservedScenePerformanceState, -1);
    else
        applyScenePerformanceStateData({}, -1);

    clearPendingActiveSceneAutosave();
    saveSceneForMainPreset(mainPresetIndex, safeSceneSlot);
    updateMonomeLEDs();
    return true;
}

bool MlrVSTAudioProcessor::copyScenePerformanceClipToClipboard(int sceneSlot)
{
    const int safeSceneSlot = juce::jlimit(0, SceneSlots - 1, sceneSlot);
    if (isSceneModeEnabled() && audioEngine != nullptr && safeSceneSlot == activeSceneSlot)
        syncSceneMotionStateFromEngine(safeSceneSlot);
    syncScenePerformanceClipLengthToResolvedLength(safeSceneSlot);
    scenePerformanceClipboardData = createScenePerformanceStateData(safeSceneSlot);
    sceneCopySourceSlot = safeSceneSlot;
    sceneCopyMainPresetIndex = getActiveMainPresetIndexForScenes();
    return scenePerformanceClipboardData.getSize() > 0;
}

bool MlrVSTAudioProcessor::pasteScenePerformanceClipFromClipboard(int sceneSlot)
{
    if (scenePerformanceClipboardData.getSize() == 0)
        return false;

    const int safeSceneSlot = juce::jlimit(0, SceneSlots - 1, sceneSlot);
    if (!applyScenePerformanceStateData(scenePerformanceClipboardData, safeSceneSlot))
        return false;

    if (safeSceneSlot == activeSceneSlot)
    {
        lastScenePerformanceProcessBeat = std::numeric_limits<double>::quiet_NaN();
        lastScenePerformanceProcessSceneSlot = -1;
        lastScenePerformanceProcessSceneStartBeat = std::numeric_limits<double>::quiet_NaN();
        queueActiveSceneAutosave();
    }

    updateMonomeLEDs();
    return true;
}

bool MlrVSTAudioProcessor::duplicateScenePerformanceClip(int sourceSceneSlot, int destSceneSlot)
{
    const int safeSourceSlot = juce::jlimit(0, SceneSlots - 1, sourceSceneSlot);
    const int safeDestSlot = juce::jlimit(0, SceneSlots - 1, destSceneSlot);
    if (safeSourceSlot == safeDestSlot)
        return false;

    if (loadedPresetIndex >= 0)
    {
        const int mainPresetIndex = getActiveMainPresetIndexForScenes();
        activeSceneMainPresetIndex = mainPresetIndex;
        if (isSceneModeEnabled() && safeSourceSlot == activeSceneSlot)
            captureSceneSlot(safeSourceSlot);
        return SceneScheduler::copySceneForMainPreset(*this, mainPresetIndex, safeSourceSlot, safeDestSlot);
    }

    if (isSceneModeEnabled() && audioEngine != nullptr && safeSourceSlot == activeSceneSlot)
        syncSceneMotionStateFromEngine(safeSourceSlot);
    syncScenePerformanceClipLengthToResolvedLength(safeSourceSlot);

    const auto stateData = createScenePerformanceStateData(safeSourceSlot);
    if (stateData.getSize() == 0)
        return false;

    if (!applyScenePerformanceStateData(stateData, safeDestSlot))
        return false;

    if (safeDestSlot == activeSceneSlot)
    {
        lastScenePerformanceProcessBeat = std::numeric_limits<double>::quiet_NaN();
        lastScenePerformanceProcessSceneSlot = -1;
        lastScenePerformanceProcessSceneStartBeat = std::numeric_limits<double>::quiet_NaN();
        queueActiveSceneAutosave();
    }

    updateMonomeLEDs();
    return true;
}

bool MlrVSTAudioProcessor::hasScenePerformanceClipboard() const
{
    return scenePerformanceClipboardData.getSize() > 0;
}

int MlrVSTAudioProcessor::getScenePerformanceClipboardSourceSlot() const
{
    return sceneCopySourceSlot;
}

bool MlrVSTAudioProcessor::getSceneEditorStripAutomationExpanded(int stripIndex) const
{
    const int safeStripIndex = juce::jlimit(0, MaxStrips - 1, stripIndex);
    return sceneEditorStripAutomationExpanded[static_cast<size_t>(safeStripIndex)];
}

bool MlrVSTAudioProcessor::getSceneEditorStripHeightExpanded(int stripIndex) const
{
    const int safeStripIndex = juce::jlimit(0, MaxStrips - 1, stripIndex);
    return sceneEditorStripHeightExpanded[static_cast<size_t>(safeStripIndex)];
}

void MlrVSTAudioProcessor::setSceneEditorGridEnabled(bool enabled)
{
    if (sceneEditorGridEnabledState == enabled)
        return;

    sceneEditorGridEnabledState = enabled;
    markPersistentGlobalUserChange();
}

void MlrVSTAudioProcessor::setSceneEditorGridDivision(int division)
{
    const int safeDivision = juce::jlimit(1, 64, division);
    if (sceneEditorGridDivisionState == safeDivision)
        return;

    sceneEditorGridDivisionState = safeDivision;
    markPersistentGlobalUserChange();
}

void MlrVSTAudioProcessor::setSceneEditorDrawModeEnabled(bool enabled)
{
    if (sceneEditorDrawModeEnabledState == enabled)
        return;

    sceneEditorDrawModeEnabledState = enabled;
    markPersistentGlobalUserChange();
}

void MlrVSTAudioProcessor::setSceneEditorLaneOverlaysEnabled(bool enabled)
{
    if (sceneEditorLaneOverlaysEnabledState == enabled)
        return;

    sceneEditorLaneOverlaysEnabledState = enabled;
    markPersistentGlobalUserChange();
}

void MlrVSTAudioProcessor::setSceneEditorZoomFactor(int factor)
{
    static constexpr std::array<int, 7> kZoomChoices{ 1, 2, 3, 4, 5, 6, 8 };
    int safeFactor = kZoomChoices.front();
    int bestDelta = std::abs(factor - safeFactor);
    for (const int candidate : kZoomChoices)
    {
        const int delta = std::abs(factor - candidate);
        if (delta < bestDelta)
        {
            bestDelta = delta;
            safeFactor = candidate;
        }
    }
    if (sceneEditorZoomFactorState == safeFactor)
        return;

    sceneEditorZoomFactorState = safeFactor;
    markPersistentGlobalUserChange();
}

void MlrVSTAudioProcessor::setSceneEditorFollowPlayheadEnabled(bool enabled)
{
    if (sceneEditorFollowPlayheadState == enabled)
        return;

    sceneEditorFollowPlayheadState = enabled;
    markPersistentGlobalUserChange();
}

void MlrVSTAudioProcessor::syncActiveSceneMotionState()
{
    if (!isSceneModeEnabled())
        return;

    syncSceneMotionStateFromEngine(getActiveSceneSlot());
    queueActiveSceneAutosave();
}

void MlrVSTAudioProcessor::setSceneEditorStripAutomationExpanded(int stripIndex, bool expanded)
{
    const int safeStripIndex = juce::jlimit(0, MaxStrips - 1, stripIndex);
    auto& state = sceneEditorStripAutomationExpanded[static_cast<size_t>(safeStripIndex)];
    if (state == expanded)
        return;

    state = expanded;
    markPersistentGlobalUserChange();
}

void MlrVSTAudioProcessor::setSceneEditorStripHeightExpanded(int stripIndex, bool expanded)
{
    const int safeStripIndex = juce::jlimit(0, MaxStrips - 1, stripIndex);
    auto& state = sceneEditorStripHeightExpanded[static_cast<size_t>(safeStripIndex)];
    if (state == expanded)
        return;

    state = expanded;
    markPersistentGlobalUserChange();
}

void MlrVSTAudioProcessor::setSceneEditorStripAutomationExpandedAll(bool expanded)
{
    bool anyChanged = false;
    for (auto& state : sceneEditorStripAutomationExpanded)
    {
        if (state == expanded)
            continue;
        state = expanded;
        anyChanged = true;
    }

    if (anyChanged)
        markPersistentGlobalUserChange();
}

bool MlrVSTAudioProcessor::replaceScenePerformanceClipEvents(int sceneSlot,
                                                             const std::vector<ScenePerformanceEvent>& events)
{
    if (scenePerformanceRecorder.isRecording())
        return false;

    const int safeSceneSlot = juce::jlimit(0, SceneSlots - 1, sceneSlot);
    const double resolvedLengthBeats = getResolvedSceneLengthBeats(safeSceneSlot);
    const bool replaced = scenePerformanceRecorder.replaceClipEvents(safeSceneSlot, events, resolvedLengthBeats);
    if (!replaced)
        return false;

    if (safeSceneSlot == activeSceneSlot)
    {
        lastScenePerformanceProcessBeat = std::numeric_limits<double>::quiet_NaN();
        lastScenePerformanceProcessSceneSlot = -1;
        lastScenePerformanceProcessSceneStartBeat = std::numeric_limits<double>::quiet_NaN();
        queueActiveSceneAutosave();
    }

    updateMonomeLEDs();
    return true;
}

void MlrVSTAudioProcessor::syncScenePerformanceClipLengthToResolvedLength(int sceneSlot)
{
    const int safeSceneSlot = juce::jlimit(0, SceneSlots - 1, sceneSlot);
    scenePerformanceRecorder.setClipLengthBeats(safeSceneSlot, getResolvedSceneLengthBeats(safeSceneSlot));

    if (safeSceneSlot == activeSceneSlot)
    {
        lastScenePerformanceProcessBeat = std::numeric_limits<double>::quiet_NaN();
        lastScenePerformanceProcessSceneSlot = -1;
        lastScenePerformanceProcessSceneStartBeat = std::numeric_limits<double>::quiet_NaN();
    }
}

void MlrVSTAudioProcessor::syncAllScenePerformanceClipLengthsToResolvedLengths()
{
    for (int sceneSlot = 0; sceneSlot < SceneSlots; ++sceneSlot)
        syncScenePerformanceClipLengthToResolvedLength(sceneSlot);
}

std::unique_ptr<juce::XmlElement> MlrVSTAudioProcessor::createSceneChainStateXml(int sceneSlotOverride) const
{
    return SceneScheduler::createSceneChainStateXml(*this, sceneSlotOverride);
}

void MlrVSTAudioProcessor::applySceneChainStateXml(const juce::XmlElement* xml, int sceneSlotOverride)
{
    ScopedSceneAutosaveSuppression suppressSceneAutosave(*this);
    SceneScheduler::applySceneChainStateXml(*this, xml, sceneSlotOverride);
}

juce::MemoryBlock MlrVSTAudioProcessor::createScenePerformanceStateData(int sceneSlotOverride) const
{
    std::array<double, SceneSlots> canonicalLengths{};
    for (int sceneSlot = 0; sceneSlot < SceneSlots; ++sceneSlot)
        canonicalLengths[static_cast<size_t>(sceneSlot)] = getResolvedSceneLengthBeats(sceneSlot);

    return scenePerformanceRecorder.createData(sceneSlotOverride, &canonicalLengths);
}

bool MlrVSTAudioProcessor::applyScenePerformanceStateData(const juce::MemoryBlock& data, int sceneSlotOverride)
{
    const bool applied = scenePerformanceRecorder.applyData(data, sceneSlotOverride);
    if (!applied)
        return false;

    if (sceneSlotOverride >= 0)
        syncScenePerformanceClipLengthToResolvedLength(sceneSlotOverride);
    else
        syncAllScenePerformanceClipLengthsToResolvedLengths();

    if (isSceneModeEnabled())
    {
        const int activeScene = juce::jlimit(0, SceneSlots - 1, activeSceneSlot);
        const int targetScene = sceneSlotOverride >= 0
            ? juce::jlimit(0, SceneSlots - 1, sceneSlotOverride)
            : activeScene;
        if (targetScene == activeScene && sceneSlotHasMotionState(targetScene))
            applySceneMotionStateToEngine(targetScene);
    }

    return true;
}

bool MlrVSTAudioProcessor::sceneSlotHasMotionState(int sceneSlot) const
{
    return scenePerformanceRecorder.hasMotionState(sceneSlot);
}

namespace
{
ScenePerformanceRecorder::MotionStripState makeDefaultSceneMotionStripState()
{
    ScenePerformanceRecorder::MotionStripState stripState;
    stripState.activeSlot = 0;

    for (int slot = 0; slot < ModernAudioEngine::NumModSequencers; ++slot)
    {
        auto& lane = stripState.lanes[static_cast<size_t>(slot)];
        const auto target = ModernAudioEngine::defaultModTargetForSlot(slot);
        const float defaultValue = ModernAudioEngine::defaultModStepValueForTarget(target);

        lane = {};
        lane.target = target;
        lane.bipolar = ModernAudioEngine::modTargetSupportsBipolar(target)
            && AudioEngineModulationInternal::modTargetAutoDefaultBipolar(target);
        lane.curveMode = false;
        lane.depth = 1.0f;
        lane.rate = 1.0f;
        lane.transportMode = ModernAudioEngine::ModTransportMode::Sync;
        lane.offset = 0;
        lane.lengthBars = 1;
        lane.editPage = 0;
        lane.smoothingMs = 0.0f;
        lane.curveBend = 0.0f;
        lane.curveShape = ModernAudioEngine::ModCurveShape::Linear;
        lane.pitchScaleQuantize = false;
        lane.pitchScale = ModernAudioEngine::PitchScale::Chromatic;
        lane.steps.fill(defaultValue);
        lane.stepSubdivisions.fill(1);
        lane.stepEndValues.fill(defaultValue);
        lane.stepCurveShapes.fill(ModernAudioEngine::ModCurveShape::Linear);
    }

    return stripState;
}
} // namespace

void MlrVSTAudioProcessor::syncSceneMotionStateFromEngine(int sceneSlot)
{
    if (audioEngine == nullptr)
        return;

    const int safeSceneSlot = juce::jlimit(0, SceneSlots - 1, sceneSlot);
    for (int stripIndex = 0; stripIndex < MaxStrips; ++stripIndex)
    {
        ScenePerformanceRecorder::MotionStripState stripState;
        stripState.activeSlot = audioEngine->getModSequencerSlot(stripIndex);

        for (int slot = 0; slot < ModernAudioEngine::NumModSequencers; ++slot)
        {
            auto& lane = stripState.lanes[static_cast<size_t>(slot)];
            lane.target = audioEngine->getModTargetForSlot(stripIndex, slot);
            lane.bipolar = audioEngine->isModBipolarForSlot(stripIndex, slot);
            lane.curveMode = audioEngine->isModCurveModeForSlot(stripIndex, slot);
            lane.depth = audioEngine->getModDepthForSlot(stripIndex, slot);
            lane.rate = audioEngine->getModRateForSlot(stripIndex, slot);
            lane.transportMode = audioEngine->getModTransportModeForSlot(stripIndex, slot);
            lane.offset = audioEngine->getModOffsetForSlot(stripIndex, slot);
            lane.lengthBars = audioEngine->getModLengthBarsForSlot(stripIndex, slot);
            lane.editPage = audioEngine->getModEditPageForSlot(stripIndex, slot);
            lane.smoothingMs = audioEngine->getModSmoothingMsForSlot(stripIndex, slot);
            lane.curveBend = audioEngine->getModCurveBendForSlot(stripIndex, slot);
            lane.curveShape = audioEngine->getModCurveShapeForSlot(stripIndex, slot);
            lane.pitchScaleQuantize = audioEngine->isModPitchScaleQuantizeForSlot(stripIndex, slot);
            lane.pitchScale = audioEngine->getModPitchScaleForSlot(stripIndex, slot);

            for (int stepIndex = 0; stepIndex < ModernAudioEngine::ModTotalSteps; ++stepIndex)
            {
                const auto index = static_cast<size_t>(stepIndex);
                lane.steps[index] = audioEngine->getModStepValueAbsoluteForSlot(stripIndex, slot, stepIndex);
                lane.stepSubdivisions[index] = audioEngine->getModStepSubdivisionAbsoluteForSlot(stripIndex, slot, stepIndex);
                lane.stepEndValues[index] = audioEngine->getModStepEndValueAbsoluteForSlot(stripIndex, slot, stepIndex);
                lane.stepCurveShapes[index] = audioEngine->getModStepCurveShapeAbsoluteForSlot(stripIndex, slot, stepIndex);
            }
        }

        scenePerformanceRecorder.setMotionStripState(safeSceneSlot, stripIndex, stripState);
    }
}

void MlrVSTAudioProcessor::applySceneMotionStateToEngine(int sceneSlot)
{
    if (audioEngine == nullptr)
        return;

    const int safeSceneSlot = juce::jlimit(0, SceneSlots - 1, sceneSlot);
    if (!scenePerformanceRecorder.hasMotionState(safeSceneSlot))
        return;

    for (int stripIndex = 0; stripIndex < MaxStrips; ++stripIndex)
    {
        const auto stripState = scenePerformanceRecorder.getMotionStripState(safeSceneSlot, stripIndex);

        for (int slot = 0; slot < ModernAudioEngine::NumModSequencers; ++slot)
        {
            const auto& lane = stripState.lanes[static_cast<size_t>(slot)];
            audioEngine->setModSequencerSlot(stripIndex, slot);
            audioEngine->setModTarget(stripIndex, lane.target);
            audioEngine->setModBipolar(stripIndex,
                                       lane.bipolar,
                                       ModernAudioEngine::ModBipolarToggleMode::Reinterpret);
            audioEngine->setModCurveMode(stripIndex, lane.curveMode);
            audioEngine->setModDepth(stripIndex, lane.depth);
            audioEngine->setModRate(stripIndex, lane.rate);
            audioEngine->setModTransportMode(stripIndex, lane.transportMode);
            audioEngine->setModOffset(stripIndex, lane.offset);
            audioEngine->setModLengthBars(stripIndex, lane.lengthBars);
            audioEngine->setModEditPage(stripIndex, lane.editPage);
            audioEngine->setModSmoothingMs(stripIndex, lane.smoothingMs);
            audioEngine->setModCurveBend(stripIndex, lane.curveBend);
            audioEngine->setModCurveShape(stripIndex, lane.curveShape);
            audioEngine->setModPitchScaleQuantize(stripIndex, lane.pitchScaleQuantize);
            audioEngine->setModPitchScale(stripIndex, lane.pitchScale);

            for (int stepIndex = 0; stepIndex < ModernAudioEngine::ModTotalSteps; ++stepIndex)
            {
                const auto index = static_cast<size_t>(stepIndex);
                audioEngine->setModStepValueAbsolute(stripIndex, stepIndex, lane.steps[index]);
                audioEngine->setModStepShapeAbsolute(stripIndex,
                                                    stepIndex,
                                                    lane.stepSubdivisions[index],
                                                    lane.stepEndValues[index]);
                audioEngine->setModStepCurveShapeAbsolute(stripIndex,
                                                         stepIndex,
                                                         lane.stepCurveShapes[index]);
            }
        }

        audioEngine->setModSequencerSlot(stripIndex, stripState.activeSlot);
    }
}

void MlrVSTAudioProcessor::clearSceneMotionStripState(int sceneSlot, int stripIndex)
{
    const int safeSceneSlot = juce::jlimit(0, SceneSlots - 1, sceneSlot);
    const int safeStripIndex = juce::jlimit(0, MaxStrips - 1, stripIndex);
    scenePerformanceRecorder.setMotionStripState(safeSceneSlot,
                                                 safeStripIndex,
                                                 makeDefaultSceneMotionStripState());

    if (audioEngine != nullptr
        && isSceneModeEnabled()
        && safeSceneSlot == juce::jlimit(0, SceneSlots - 1, activeSceneSlot))
    {
        for (int slot = 0; slot < ModernAudioEngine::NumModSequencers; ++slot)
            audioEngine->resetModSequencerSlotToDefaults(safeStripIndex, slot);
        audioEngine->setModSequencerSlot(safeStripIndex, 0);
    }

    if (safeSceneSlot == juce::jlimit(0, SceneSlots - 1, activeSceneSlot))
        queueActiveSceneAutosave();
}

bool MlrVSTAudioProcessor::stripUsesGrainSceneLanes(int stripIndex) const
{
    if (audioEngine == nullptr)
        return false;

    auto* strip = audioEngine->getStrip(juce::jlimit(0, MaxStrips - 1, stripIndex));
    return strip != nullptr && strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Grain;
}

std::vector<ModernAudioEngine::ModTarget> MlrVSTAudioProcessor::getSceneVisibleModTargetsForStrip(int stripIndex) const
{
    std::vector<ModernAudioEngine::ModTarget> targets{
        ModernAudioEngine::ModTarget::Speed,
        stripUsesGrainSceneLanes(stripIndex) ? ModernAudioEngine::ModTarget::GrainPitch
                                             : ModernAudioEngine::ModTarget::Pitch,
        ModernAudioEngine::ModTarget::Pan,
        ModernAudioEngine::ModTarget::Volume,
        ModernAudioEngine::ModTarget::Cutoff,
        ModernAudioEngine::ModTarget::Resonance,
        ModernAudioEngine::ModTarget::FilterMorph,
        ModernAudioEngine::ModTarget::SliceLength,
        ModernAudioEngine::ModTarget::Scratch,
        ModernAudioEngine::ModTarget::DelayMix,
        ModernAudioEngine::ModTarget::DelayTime,
        ModernAudioEngine::ModTarget::DelayFeedback
    };

    if (stripUsesGrainSceneLanes(stripIndex))
    {
        targets.push_back(ModernAudioEngine::ModTarget::GrainSize);
        targets.push_back(ModernAudioEngine::ModTarget::GrainDensity);
    }

    return targets;
}

ModernAudioEngine::ModTarget MlrVSTAudioProcessor::getSceneMotionTargetForSlot(int stripIndex, int laneSlot) const
{
    if (audioEngine == nullptr)
        return ModernAudioEngine::ModTarget::None;

    const int safeStripIndex = juce::jlimit(0, MaxStrips - 1, stripIndex);
    const int safeSlot = juce::jlimit(0, ModernAudioEngine::NumModSequencers - 1, laneSlot);
    return audioEngine->getModTargetForSlot(safeStripIndex, safeSlot);
}

void MlrVSTAudioProcessor::setSceneMotionTargetForSlot(int stripIndex,
                                                       int laneSlot,
                                                       ModernAudioEngine::ModTarget target)
{
    if (audioEngine == nullptr)
        return;

    const int safeStripIndex = juce::jlimit(0, MaxStrips - 1, stripIndex);
    const int safeSlot = juce::jlimit(0, ModernAudioEngine::NumModSequencers - 1, laneSlot);
    const auto safeTarget = sanitizeModPerformanceTarget(target);
    auto matchesTarget = [](ModernAudioEngine::ModTarget lhs, ModernAudioEngine::ModTarget rhs)
    {
        return lhs == rhs
            || (lhs == ModernAudioEngine::ModTarget::Pitch && rhs == ModernAudioEngine::ModTarget::GrainPitch)
            || (lhs == ModernAudioEngine::ModTarget::GrainPitch && rhs == ModernAudioEngine::ModTarget::Pitch);
    };
    const auto currentTarget = sanitizeModPerformanceTarget(audioEngine->getModTargetForSlot(safeStripIndex, safeSlot));

    if (matchesTarget(currentTarget, safeTarget))
    {
        audioEngine->setModSequencerSlot(safeStripIndex, safeSlot);
        if (isSceneModeEnabled())
            syncSceneMotionStateFromEngine(getActiveSceneSlot());
        return;
    }

    int swapSlot = -1;
    if (safeTarget != ModernAudioEngine::ModTarget::None)
    {
        for (int slot = 0; slot < ModernAudioEngine::NumModSequencers; ++slot)
        {
            if (slot == safeSlot)
                continue;
            if (!matchesTarget(audioEngine->getModTargetForSlot(safeStripIndex, slot), safeTarget))
                continue;
            swapSlot = slot;
            break;
        }
    }

    if (swapSlot >= 0)
    {
        audioEngine->setModSequencerSlot(safeStripIndex, swapSlot);
        audioEngine->setModTarget(safeStripIndex, currentTarget);
    }

    audioEngine->setModSequencerSlot(safeStripIndex, safeSlot);
    audioEngine->setModTarget(safeStripIndex, safeTarget);

    if (isSceneModeEnabled())
        syncSceneMotionStateFromEngine(getActiveSceneSlot());
    queueActiveSceneAutosave();
}

void MlrVSTAudioProcessor::cycleSceneModLaneTarget(int stripIndex)
{
    if (audioEngine == nullptr)
        return;

    const int safeStripIndex = juce::jlimit(0, MaxStrips - 1, stripIndex);
    const auto targets = getSceneVisibleModTargetsForStrip(safeStripIndex);
    if (targets.empty())
        return;

    const int activeSlot = juce::jlimit(0,
                                        ModernAudioEngine::NumModSequencers - 1,
                                        audioEngine->getModSequencerSlot(safeStripIndex));
    const auto currentTarget = audioEngine->getModTargetForSlot(safeStripIndex, activeSlot);
    auto matchesTarget = [](ModernAudioEngine::ModTarget lhs, ModernAudioEngine::ModTarget rhs)
    {
        return lhs == rhs
            || (lhs == ModernAudioEngine::ModTarget::Pitch && rhs == ModernAudioEngine::ModTarget::GrainPitch)
            || (lhs == ModernAudioEngine::ModTarget::GrainPitch && rhs == ModernAudioEngine::ModTarget::Pitch);
    };

    int currentIndex = -1;
    for (int targetIndex = 0; targetIndex < static_cast<int>(targets.size()); ++targetIndex)
    {
        if (matchesTarget(currentTarget, targets[static_cast<size_t>(targetIndex)]))
        {
            currentIndex = targetIndex;
            break;
        }
    }

    const auto nextTarget = targets[static_cast<size_t>((currentIndex + 1 + static_cast<int>(targets.size()))
                                                        % static_cast<int>(targets.size()))];

    for (int slot = 0; slot < ModernAudioEngine::NumModSequencers; ++slot)
    {
        if (matchesTarget(audioEngine->getModTargetForSlot(safeStripIndex, slot), nextTarget))
        {
            audioEngine->setModSequencerSlot(safeStripIndex, slot);
            if (isSceneModeEnabled())
                syncSceneMotionStateFromEngine(getActiveSceneSlot());
            queueActiveSceneAutosave();
            return;
        }
    }

    audioEngine->setModSequencerSlot(safeStripIndex, activeSlot);
    audioEngine->setModTarget(safeStripIndex, nextTarget);
    if (isSceneModeEnabled())
        syncSceneMotionStateFromEngine(getActiveSceneSlot());
    queueActiveSceneAutosave();
}

void MlrVSTAudioProcessor::updateAudioEngineSceneModulationContext() const
{
    if (audioEngine == nullptr)
        return;

    if (!isSceneModeEnabled() || !activeSceneStartPpqValid || !std::isfinite(activeSceneStartPpq))
    {
        audioEngine->setSceneModulationContext(false, 0.0, 4.0);
        return;
    }

    const double sceneLengthBeats = juce::jmax(0.001, getResolvedSceneLengthBeats(getActiveSceneSlot()));
    audioEngine->setSceneModulationContext(true, activeSceneStartPpq, sceneLengthBeats);
}

double MlrVSTAudioProcessor::computeStripSceneSequenceLengthBeats(int stripIndex) const
{
    return SceneScheduler::computeStripSceneSequenceLengthBeats(*this, stripIndex);
}

double MlrVSTAudioProcessor::computeLongestStripSceneSequenceLengthBeats() const
{
    return SceneScheduler::computeLongestStripSceneSequenceLengthBeats(*this);
}

double MlrVSTAudioProcessor::computeLongestPatternSceneSequenceLengthBeats() const
{
    return SceneScheduler::computeLongestPatternSceneSequenceLengthBeats(*this);
}

double MlrVSTAudioProcessor::computeCurrentSceneSequenceLengthBeats() const
{
    return SceneScheduler::computeCurrentSceneSequenceLengthBeats(*this);
}

double MlrVSTAudioProcessor::computeNextScenePatternEndPpq(int sceneSlot,
                                                           double currentPpq,
                                                           double cycleBeats,
                                                           uint64_t* outPhaseSignature) const
{
    return SceneScheduler::computeNextScenePatternEndPpq(*this,
                                                         sceneSlot,
                                                         currentPpq,
                                                         cycleBeats,
                                                         outPhaseSignature);
}

void MlrVSTAudioProcessor::armNextSceneInSequence(int mainPresetIndex, int currentSceneSlot, double sceneStartPpq)
{
    SceneScheduler::armNextSceneInSequence(*this, mainPresetIndex, currentSceneSlot, sceneStartPpq);
}

void MlrVSTAudioProcessor::setSceneModeEnabled(bool enabled)
{
    SceneScheduler::setSceneModeEnabled(*this, enabled);
}

void MlrVSTAudioProcessor::captureSceneModeGroupSnapshot()
{
    SceneScheduler::captureSceneModeGroupSnapshot(*this);
}

void MlrVSTAudioProcessor::restoreSceneModeGroupSnapshot()
{
    SceneScheduler::restoreSceneModeGroupSnapshot(*this);
}

void MlrVSTAudioProcessor::clearAllStripGroupsForSceneMode()
{
    SceneScheduler::clearAllStripGroupsForSceneMode(*this);
}

void MlrVSTAudioProcessor::applySceneModeState(bool enabled)
{
    SceneScheduler::applySceneModeState(*this, enabled);
}

void MlrVSTAudioProcessor::syncSceneModeFromParameters()
{
    SceneScheduler::syncSceneModeFromParameters(*this);
}

bool MlrVSTAudioProcessor::saveSceneForMainPreset(int mainPresetIndex, int sceneSlot)
{
    return SceneScheduler::saveSceneForMainPreset(*this, mainPresetIndex, sceneSlot);
}

bool MlrVSTAudioProcessor::copySceneForMainPreset(int mainPresetIndex, int sourceSceneSlot, int destSceneSlot)
{
    return SceneScheduler::copySceneForMainPreset(*this, mainPresetIndex, sourceSceneSlot, destSceneSlot);
}

bool MlrVSTAudioProcessor::deleteSceneForMainPreset(int mainPresetIndex, int sceneSlot)
{
    return SceneScheduler::deleteSceneForMainPreset(*this, mainPresetIndex, sceneSlot);
}

bool MlrVSTAudioProcessor::captureSceneSlot(int sceneSlot)
{
    return SceneScheduler::captureSceneSlot(*this, sceneSlot);
}

bool MlrVSTAudioProcessor::insertSceneSlot(int sceneSlot, bool insertAfter)
{
    return SceneScheduler::insertSceneSlot(*this, sceneSlot, insertAfter);
}

void MlrVSTAudioProcessor::copyScenePerformanceClip(int sourceSceneSlot, int destSceneSlot)
{
    scenePerformanceRecorder.copyClip(sourceSceneSlot, destSceneSlot);
    syncScenePerformanceClipLengthToResolvedLength(destSceneSlot);
}

void MlrVSTAudioProcessor::requestSceneRecallQuantized(int mainPresetIndex,
                                                       int sceneSlot,
                                                       bool sequenceDriven,
                                                       int sequenceStepIndex,
                                                       bool useTriggerQuantization)
{
    SceneScheduler::requestSceneRecallQuantized(
        *this,
        mainPresetIndex,
        sceneSlot,
        sequenceDriven,
        sequenceStepIndex,
        useTriggerQuantization);
}

double MlrVSTAudioProcessor::getSceneRecallIntervalBeats() const
{
    return SceneScheduler::getSceneRecallIntervalBeats(*this);
}

void MlrVSTAudioProcessor::updateSceneQuantizedRecall(
    const juce::AudioPlayHead::PositionInfo& posInfo, int numSamples)
{
    SceneScheduler::updateSceneQuantizedRecall(*this, posInfo, numSamples);
}

void MlrVSTAudioProcessor::processPendingSceneApply()
{
    SceneScheduler::processPendingSceneApply(*this);
}

void MlrVSTAudioProcessor::performEmptySceneLoad()
{
    SceneScheduler::performEmptySceneLoad(*this);
}

void MlrVSTAudioProcessor::performSceneLoad(int mainPresetIndex,
                                            int sceneSlot,
                                            double hostPpqSnapshot,
                                            double hostTempoSnapshot,
                                            int64_t hostGlobalSampleSnapshot)
{
    SceneScheduler::performSceneLoad(*this,
                                     mainPresetIndex,
                                     sceneSlot,
                                     hostPpqSnapshot,
                                     hostTempoSnapshot,
                                     hostGlobalSampleSnapshot);
}

float MlrVSTAudioProcessor::getInnerLoopLengthFactor() const
{
    const int choice = innerLoopLengthSelection.load(std::memory_order_acquire);
    static constexpr std::array<float, 5> kFactors { 1.0f, 0.5f, 0.25f, 0.125f, 0.0625f };
    return kFactors[static_cast<size_t>(choice)];
}

void MlrVSTAudioProcessor::queueLoopChange(int stripIndex, bool clearLoop, int startColumn, int endColumn, bool reverseDirection, int markerColumn)
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
        {
            const juce::ScopedLock lock(pendingLoopChangeLock);
            pendingLoopChanges[static_cast<size_t>(stripIndex)].active = false;
        }

        bool markerApplied = false;
        if (clearLoop)
        {
            strip->clearLoop();
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

        bool triggeredAtColumn = false;
        if (change.clear)
        {
            strip->clearLoop();
            strip->setReverse(false);
            strip->setDirectionMode(EnhancedAudioStrip::DirectionMode::Normal);
            if (change.markerColumn >= 0 && std::isfinite(currentPpq) && currentTempo > 0.0)
            {
                juce::AudioPlayHead::PositionInfo retriggerPosInfo;
                const double applyPpq = (change.quantized && std::isfinite(change.targetPpq))
                    ? change.targetPpq
                    : currentPpq;
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
            strip->setDirectionMode(change.reverse
                ? EnhancedAudioStrip::DirectionMode::Reverse
                : EnhancedAudioStrip::DirectionMode::Normal);
        }

        if (change.quantized && !triggeredAtColumn)
        {
            // Deterministic PPQ realign after loop-geometry change.
            const double applyPpq = std::isfinite(currentPpq)
                ? currentPpq
                : (std::isfinite(change.targetPpq) ? change.targetPpq : audioEngine->getTimelineBeat());
            strip->realignToPpqAnchor(applyPpq, currentGlobalSample);
            strip->setBeatsPerLoopAtPpq(strip->getBeatsPerLoop(), applyPpq);
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
    auto restoreSavedState = [this](int stripIndex,
                                    EnhancedAudioStrip& strip,
                                    const MomentaryStutterSavedStripState& saved)
    {
        setStripPanControlValue(stripIndex, saved.pan, StripControlWriteMode::CacheOnly);
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
        audioEngine->setMomentaryStutterDivision(
            juce::jlimit(0.03125, 4.0, stutterDivisionBeatsFromBit(highestBit)));
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

            restoreSavedState(i, *strip, saved);
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
                strip->setPlaybackSpeedImmediate(targetSpeed);
            else
                strip->setPlaybackSpeed(speedBaseline);
        }
        else
        {
            // Step mode uses step-sampler pitch speed; keep strip traversal speed stable.
            strip->setPlaybackSpeed(saved.playbackSpeed);
        }

        const float targetPan = juce::jlimit(-1.0f, 1.0f, saved.pan + (panOffsetBase * stripPanScale));
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

juce::File MlrVSTAudioProcessor::getDefaultSampleDirectory(int stripIndex, SamplePathMode mode) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return {};

    const auto idx = static_cast<size_t>(stripIndex);
    switch (mode)
    {
        case SamplePathMode::Step: return defaultStepDirectories[idx];
        case SamplePathMode::Flip: return defaultFlipDirectories[idx];
        case SamplePathMode::Loop:
        default: return defaultLoopDirectories[idx];
    }
}

MlrVSTAudioProcessor::SamplePathMode MlrVSTAudioProcessor::getSamplePathModeForStrip(int stripIndex) const
{
    if (!audioEngine || stripIndex < 0 || stripIndex >= MaxStrips)
        return SamplePathMode::Loop;

    if (auto* strip = audioEngine->getStrip(stripIndex))
    {
        if (strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Step)
            return SamplePathMode::Step;
        if (strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Sample)
            return SamplePathMode::Flip;
    }

    return SamplePathMode::Loop;
}

juce::File MlrVSTAudioProcessor::getRecentSampleDirectory(int stripIndex, SamplePathMode mode) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return {};

    const auto idx = static_cast<size_t>(stripIndex);
    switch (mode)
    {
        case SamplePathMode::Step: return recentStepDirectories[idx];
        case SamplePathMode::Flip: return recentFlipDirectories[idx];
        case SamplePathMode::Loop:
        default: return recentLoopDirectories[idx];
    }
}

void MlrVSTAudioProcessor::setRecentSampleDirectory(int stripIndex,
                                                    SamplePathMode mode,
                                                    const juce::File& directory,
                                                    bool persist)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;

    const auto idx = static_cast<size_t>(stripIndex);
    juce::File normalizedDirectory;
    const auto rawPath = directory.getFullPathName().trim();
    if (directory != juce::File()
        && rawPath.isNotEmpty()
        && juce::File::isAbsolutePath(rawPath))
    {
        normalizedDirectory = directory;
    }

    switch (mode)
    {
        case SamplePathMode::Step:
            recentStepDirectories[idx] = normalizedDirectory;
            break;
        case SamplePathMode::Flip:
            recentFlipDirectories[idx] = normalizedDirectory;
            break;
        case SamplePathMode::Loop:
        default:
            recentLoopDirectories[idx] = normalizedDirectory;
            break;
    }

    if (safeFileIsDirectory(normalizedDirectory))
        lastSampleFolder = normalizedDirectory;

    if (persist)
        savePersistentDefaultPaths();
}

juce::File MlrVSTAudioProcessor::getCurrentBrowserDirectoryForStrip(int stripIndex) const
{
    return getCurrentBrowserDirectoryForStrip(stripIndex, getSamplePathModeForStrip(stripIndex));
}

juce::File MlrVSTAudioProcessor::getCurrentBrowserDirectoryForStrip(int stripIndex, SamplePathMode mode) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return {};

    const auto isValidDir = [](const juce::File& dir)
    {
        return safeFileIsDirectory(dir);
    };

    const auto recentDir = getRecentSampleDirectory(stripIndex, mode);
    if (isValidDir(recentDir))
        return recentDir;

    const auto currentFile = currentStripFiles[static_cast<size_t>(stripIndex)];
    const auto currentDir = currentFile.getParentDirectory();
    if (isValidDir(currentDir))
        return currentDir;

    const auto selectedDir = getDefaultSampleDirectory(stripIndex, mode);
    if (isValidDir(selectedDir))
        return selectedDir;

    const std::array<SamplePathMode, 3> fallbackModes {
        SamplePathMode::Flip,
        SamplePathMode::Step,
        SamplePathMode::Loop
    };
    for (const auto fallbackMode : fallbackModes)
    {
        if (fallbackMode == mode)
            continue;
        const auto fallbackDefaultDir = getDefaultSampleDirectory(stripIndex, fallbackMode);
        if (isValidDir(fallbackDefaultDir))
            return fallbackDefaultDir;
        const auto fallbackRecentDir = getRecentSampleDirectory(stripIndex, fallbackMode);
        if (isValidDir(fallbackRecentDir))
            return fallbackRecentDir;
    }

    const auto& favoriteSet = (mode == SamplePathMode::Flip)
        ? browserFlipFavoriteDirectories
        : browserFavoriteDirectories;
    for (const auto& favoriteDir : favoriteSet)
    {
        if (isValidDir(favoriteDir))
            return favoriteDir;
    }

    if (isValidDir(lastSampleFolder))
        return lastSampleFolder;

    for (int i = 0; i < MaxStrips; ++i)
    {
        if (i == stripIndex)
            continue;

        if (isValidDir(defaultLoopDirectories[static_cast<size_t>(i)]))
            return defaultLoopDirectories[static_cast<size_t>(i)];
        if (isValidDir(defaultStepDirectories[static_cast<size_t>(i)]))
            return defaultStepDirectories[static_cast<size_t>(i)];
        if (isValidDir(defaultFlipDirectories[static_cast<size_t>(i)]))
            return defaultFlipDirectories[static_cast<size_t>(i)];
        if (isValidDir(recentLoopDirectories[static_cast<size_t>(i)]))
            return recentLoopDirectories[static_cast<size_t>(i)];
        if (isValidDir(recentStepDirectories[static_cast<size_t>(i)]))
            return recentStepDirectories[static_cast<size_t>(i)];
        if (isValidDir(recentFlipDirectories[static_cast<size_t>(i)]))
            return recentFlipDirectories[static_cast<size_t>(i)];

        const auto otherCurrentDir = currentStripFiles[static_cast<size_t>(i)].getParentDirectory();
        if (isValidDir(otherCurrentDir))
            return otherCurrentDir;
    }

    // Last-resort fallback: allow browsing from home even with no configured paths.
    const auto homeDir = juce::File::getSpecialLocation(juce::File::userHomeDirectory);
    if (isValidDir(homeDir))
        return homeDir;

    return {};
}

juce::String MlrVSTAudioProcessor::getStripDisplaySampleName(int stripIndex)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return {};

    const auto& pendingFile = pendingLoopStripFiles[static_cast<size_t>(stripIndex)];
    if (isLoopStripLoadInFlight(stripIndex) && pendingFile.getFullPathName().isNotEmpty())
        return pendingFile.getFileNameWithoutExtension();

    const auto& currentFile = currentStripFiles[static_cast<size_t>(stripIndex)];
    if (currentFile.getFullPathName().isNotEmpty())
        return currentFile.getFileNameWithoutExtension();

    if (auto* engine = getSampleModeEngine(stripIndex, false))
    {
        const auto snapshot = engine->getStateSnapshot();
        if (snapshot.displayName.isNotEmpty())
            return snapshot.displayName;
    }

    return {};
}

juce::File MlrVSTAudioProcessor::getBrowserFavoriteDirectory(int stripIndex, int slot) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips || slot < 0 || slot >= BrowserFavoriteSlots)
        return {};

    const auto mode = getSamplePathModeForStrip(stripIndex);
    return (mode == SamplePathMode::Flip)
        ? browserFlipFavoriteDirectories[static_cast<size_t>(slot)]
        : browserFavoriteDirectories[static_cast<size_t>(slot)];
}

bool MlrVSTAudioProcessor::isBrowserFavoritePadHeld(int stripIndex, int slot) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips || slot < 0 || slot >= BrowserFavoriteSlots)
        return false;

    return browserFavoritePadHeld[static_cast<size_t>(stripIndex)][static_cast<size_t>(slot)];
}

bool MlrVSTAudioProcessor::isBrowserFavoriteSaveBurstActive(int slot, uint32_t nowMs) const
{
    if (slot < 0 || slot >= BrowserFavoriteSlots)
        return false;

    return nowMs < browserFavoriteSaveBurstUntilMs[static_cast<size_t>(slot)];
}

bool MlrVSTAudioProcessor::isBrowserFavoriteMissingBurstActive(int slot, uint32_t nowMs) const
{
    if (slot < 0 || slot >= BrowserFavoriteSlots)
        return false;

    return nowMs < browserFavoriteMissingBurstUntilMs[static_cast<size_t>(slot)];
}

void MlrVSTAudioProcessor::beginBrowserFavoritePadHold(int stripIndex, int slot)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips || slot < 0 || slot >= BrowserFavoriteSlots)
        return;

    const auto stripIdx = static_cast<size_t>(stripIndex);
    const auto slotIdx = static_cast<size_t>(slot);
    browserFavoritePadHeld[stripIdx][slotIdx] = true;
    browserFavoritePadHoldSaveTriggered[stripIdx][slotIdx] = false;
    browserFavoritePadPressStartMs[stripIdx][slotIdx] = juce::Time::getMillisecondCounter();
}

void MlrVSTAudioProcessor::endBrowserFavoritePadHold(int stripIndex, int slot)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips || slot < 0 || slot >= BrowserFavoriteSlots)
        return;

    const auto stripIdx = static_cast<size_t>(stripIndex);
    const auto slotIdx = static_cast<size_t>(slot);
    const bool wasHeld = browserFavoritePadHeld[stripIdx][slotIdx];
    const bool holdSaveTriggered = browserFavoritePadHoldSaveTriggered[stripIdx][slotIdx];

    if (wasHeld && !holdSaveTriggered)
    {
        if (!recallBrowserFavoriteDirectoryForStrip(stripIndex, slot))
            browserFavoriteMissingBurstUntilMs[slotIdx] = juce::Time::getMillisecondCounter() + browserFavoriteMissingBurstDurationMs;
    }

    browserFavoritePadHeld[stripIdx][slotIdx] = false;
    browserFavoritePadHoldSaveTriggered[stripIdx][slotIdx] = false;
}

void MlrVSTAudioProcessor::setDefaultSampleDirectory(int stripIndex, SamplePathMode mode, const juce::File& directory)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;

    const auto idx = static_cast<size_t>(stripIndex);

    if (directory == juce::File())
    {
        if (mode == SamplePathMode::Step)
            defaultStepDirectories[idx] = juce::File();
        else if (mode == SamplePathMode::Flip)
            defaultFlipDirectories[idx] = juce::File();
        else
            defaultLoopDirectories[idx] = juce::File();
        savePersistentDefaultPaths();
        return;
    }

    if (!safeFileIsDirectory(directory))
        return;

    if (mode == SamplePathMode::Step)
        defaultStepDirectories[idx] = directory;
    else if (mode == SamplePathMode::Flip)
        defaultFlipDirectories[idx] = directory;
    else
        defaultLoopDirectories[idx] = directory;

    savePersistentDefaultPaths();
}

void MlrVSTAudioProcessor::setCurrentBrowserDirectoryForStrip(int stripIndex,
                                                              SamplePathMode mode,
                                                              const juce::File& directory)
{
    setRecentSampleDirectory(stripIndex, mode, directory, true);
}

bool MlrVSTAudioProcessor::saveBrowserFavoriteDirectoryFromStrip(int stripIndex, int slot)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips || slot < 0 || slot >= BrowserFavoriteSlots)
        return false;

    const auto directory = getCurrentBrowserDirectoryForStrip(stripIndex);
    if (!safeFileIsDirectory(directory))
        return false;

    const auto mode = getSamplePathModeForStrip(stripIndex);
    if (mode == SamplePathMode::Flip)
        browserFlipFavoriteDirectories[static_cast<size_t>(slot)] = directory;
    else
        browserFavoriteDirectories[static_cast<size_t>(slot)] = directory;
    savePersistentDefaultPaths();
    return true;
}

bool MlrVSTAudioProcessor::recallBrowserFavoriteDirectoryForStrip(int stripIndex, int slot)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips || slot < 0 || slot >= BrowserFavoriteSlots)
        return false;

    const auto slotIdx = static_cast<size_t>(slot);
    const auto mode = getSamplePathModeForStrip(stripIndex);
    const auto directory = (mode == SamplePathMode::Flip)
        ? browserFlipFavoriteDirectories[slotIdx]
        : browserFavoriteDirectories[slotIdx];
    if (!safeFileIsDirectory(directory))
    {
        if (mode == SamplePathMode::Flip)
            browserFlipFavoriteDirectories[slotIdx] = juce::File();
        else
            browserFavoriteDirectories[slotIdx] = juce::File();
        savePersistentDefaultPaths();
        return false;
    }

    setRecentSampleDirectory(stripIndex, mode, directory);
    return true;
}

bool MlrVSTAudioProcessor::isAudioFileSupported(const juce::File& file) const
{
    if (!safeFileExistsAsFile(file))
        return false;

    return file.hasFileExtension(".wav")
        || file.hasFileExtension(".aif")
        || file.hasFileExtension(".aiff")
        || file.hasFileExtension(".mp3")
        || file.hasFileExtension(".ogg")
        || file.hasFileExtension(".flac");
}

void MlrVSTAudioProcessor::appendDefaultPathsToState(juce::ValueTree& state) const
{
    auto paths = state.getOrCreateChildWithName("DefaultPaths", nullptr);
    for (int i = 0; i < MaxStrips; ++i)
    {
        const auto idx = static_cast<size_t>(i);
        const auto loopKey = "loopDir" + juce::String(i);
        const auto stepKey = "stepDir" + juce::String(i);
        const auto flipKey = "flipDir" + juce::String(i);
        const auto recentLoopKey = "recentLoopDir" + juce::String(i);
        const auto recentStepKey = "recentStepDir" + juce::String(i);
        const auto recentFlipKey = "recentFlipDir" + juce::String(i);
        paths.setProperty(loopKey, defaultLoopDirectories[idx].getFullPathName(), nullptr);
        paths.setProperty(stepKey, defaultStepDirectories[idx].getFullPathName(), nullptr);
        paths.setProperty(flipKey, defaultFlipDirectories[idx].getFullPathName(), nullptr);
        paths.setProperty(recentLoopKey, recentLoopDirectories[idx].getFullPathName(), nullptr);
        paths.setProperty(recentStepKey, recentStepDirectories[idx].getFullPathName(), nullptr);
        paths.setProperty(recentFlipKey, recentFlipDirectories[idx].getFullPathName(), nullptr);
    }

    paths.setProperty("lastSampleFolder", lastSampleFolder.getFullPathName(), nullptr);

    for (int slot = 0; slot < BrowserFavoriteSlots; ++slot)
    {
        const auto key = "favoriteDir" + juce::String(slot);
        const auto flipKey = "favoriteFlipDir" + juce::String(slot);
        paths.setProperty(key, browserFavoriteDirectories[static_cast<size_t>(slot)].getFullPathName(), nullptr);
        paths.setProperty(flipKey, browserFlipFavoriteDirectories[static_cast<size_t>(slot)].getFullPathName(), nullptr);
    }
}

void MlrVSTAudioProcessor::appendControlPagesToState(juce::ValueTree& state) const
{
    auto controlPages = state.getOrCreateChildWithName("ControlPages", nullptr);
    const auto orderSnapshot = getControlPageOrder();
    for (int i = 0; i < NumControlRowPages; ++i)
    {
        const auto key = "slot" + juce::String(i);
        controlPages.setProperty(key, controlModeToKey(orderSnapshot[static_cast<size_t>(i)]), nullptr);
    }

    controlPages.setProperty("momentary", isControlPageMomentary(), nullptr);
    controlPages.setProperty("swingDivision", swingDivisionSelection.load(std::memory_order_acquire), nullptr);
}

void MlrVSTAudioProcessor::appendFlipStatesToState(juce::ValueTree& state) const
{
    auto flipStates = state.getOrCreateChildWithName("FlipStates", nullptr);
    while (flipStates.getNumChildren() > 0)
        flipStates.removeChild(0, nullptr);

    for (int i = 0; i < MaxStrips; ++i)
    {
        auto* strip = audioEngine != nullptr ? audioEngine->getStrip(i) : nullptr;
        if (strip == nullptr)
            continue;

        const bool isFlipMode = (strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Sample);
        if (!isFlipMode && !hasSampleModeAudio(i))
            continue;

        auto* engine = const_cast<MlrVSTAudioProcessor*>(this)->getSampleModeEngine(i, isFlipMode);
        if (engine == nullptr)
            continue;

        auto flipState = engine->capturePersistentState().createValueTree("FlipStrip");
        flipState.setProperty("index", i, nullptr);
        flipState.setProperty("enabled", isFlipMode, nullptr);
        const auto embeddedSample = createEmbeddedFlipSampleData(i);
        if (embeddedSample.isNotEmpty())
            flipState.setProperty(kEmbeddedFlipSampleAttr, embeddedSample, nullptr);
        flipStates.addChild(flipState, -1, nullptr);
    }
}

void MlrVSTAudioProcessor::appendLoopPitchStateToState(juce::ValueTree& state) const
{
    auto loopPitchState = state.getOrCreateChildWithName("LoopPitchState", nullptr);
    while (loopPitchState.getNumChildren() > 0)
        loopPitchState.removeChild(0, nullptr);

    for (int i = 0; i < MaxStrips; ++i)
    {
        juce::ValueTree stripState("Strip");
        stripState.setProperty("index", i, nullptr);
        stripState.setProperty("role", loopPitchRoles[static_cast<size_t>(i)].load(std::memory_order_acquire), nullptr);
        stripState.setProperty("syncTiming", loopPitchSyncTimings[static_cast<size_t>(i)].load(std::memory_order_acquire), nullptr);
        stripState.setProperty("assignedMidi", loopPitchAssignedMidi[static_cast<size_t>(i)].load(std::memory_order_acquire), nullptr);
        stripState.setProperty("assignedManual", loopPitchAssignedManual[static_cast<size_t>(i)].load(std::memory_order_acquire), nullptr);
        stripState.setProperty("detectedMidi", loopPitchDetectedMidi[static_cast<size_t>(i)].load(std::memory_order_acquire), nullptr);
        stripState.setProperty("detectedHz", loopPitchDetectedHz[static_cast<size_t>(i)].load(std::memory_order_acquire), nullptr);
        stripState.setProperty("detectedPitchConfidence", loopPitchDetectedPitchConfidence[static_cast<size_t>(i)].load(std::memory_order_acquire), nullptr);
        stripState.setProperty("detectedScaleIndex", loopPitchDetectedScaleIndices[static_cast<size_t>(i)].load(std::memory_order_acquire), nullptr);
        stripState.setProperty("detectedScaleConfidence", loopPitchDetectedScaleConfidence[static_cast<size_t>(i)].load(std::memory_order_acquire), nullptr);
        stripState.setProperty("essentiaUsed", loopPitchEssentiaUsed[static_cast<size_t>(i)].load(std::memory_order_acquire), nullptr);
        loopPitchState.addChild(stripState, -1, nullptr);
    }
}

void MlrVSTAudioProcessor::appendSceneModeStateToState(juce::ValueTree& state) const
{
    SceneScheduler::appendSceneModeStateToState(*this, state);
}

void MlrVSTAudioProcessor::loadSceneModeStateFromState(const juce::ValueTree& state)
{
    SceneScheduler::loadSceneModeStateFromState(*this, state);
}

void MlrVSTAudioProcessor::loadFlipStatesFromState(const juce::ValueTree& state)
{
    auto flipStates = state.getChildWithName("FlipStates");
    if (!flipStates.isValid() || audioEngine == nullptr)
        return;

    for (auto flipState : flipStates)
    {
        if (!flipState.hasType("FlipStrip"))
            continue;

        const int stripIndex = static_cast<int>(flipState.getProperty("index", -1));
        if (stripIndex < 0 || stripIndex >= MaxStrips)
            continue;

        if (auto* strip = audioEngine->getStrip(stripIndex))
        {
            const bool enabled = static_cast<bool>(flipState.getProperty("enabled", false));
            if (!enabled)
                continue;

            strip->setPlayMode(EnhancedAudioStrip::PlayMode::Sample);
            if (auto* engine = getSampleModeEngine(stripIndex, true))
            {
                const auto persistentState = SampleModePersistentState::fromValueTree(flipState);
                const auto embeddedSample = flipState.getProperty(kEmbeddedFlipSampleAttr).toString();
                const juce::File sampleFile(persistentState.samplePath);
                if (persistentState.samplePath.isNotEmpty())
                    rememberLoadedSamplePathForStripMode(stripIndex, sampleFile, SamplePathMode::Flip, false);
                if (safeFileExistsAsFile(sampleFile))
                {
                    engine->applyPersistentState(persistentState);
                    loadSampleToSampleModeStrip(stripIndex, sampleFile);
                }
                else if (embeddedSample.isNotEmpty())
                {
                    loadEmbeddedFlipSampleData(stripIndex, embeddedSample, &persistentState);
                }
                else
                {
                    engine->applyPersistentState(persistentState);
                }
            }
        }
    }
}

void MlrVSTAudioProcessor::loadLoopPitchStateFromState(const juce::ValueTree& state)
{
    for (auto& role : loopPitchRoles)
        role.store(static_cast<int>(LoopPitchRole::None), std::memory_order_release);
    for (auto& assignedMidi : loopPitchAssignedMidi)
        assignedMidi.store(-1, std::memory_order_release);
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
    for (auto& timing : loopPitchSyncTimings)
        timing.store(static_cast<int>(LoopPitchSyncTiming::Immediate), std::memory_order_release);
    for (auto& assignedManual : loopPitchAssignedManual)
        assignedManual.store(0, std::memory_order_release);
    for (auto& pendingRetune : loopPitchPendingRetune)
        pendingRetune.store(0, std::memory_order_release);

    auto loopPitchState = state.getChildWithName("LoopPitchState");
    if (!loopPitchState.isValid())
        return;

    for (auto stripState : loopPitchState)
    {
        if (!stripState.hasType("Strip"))
            continue;

        const int stripIndex = static_cast<int>(stripState.getProperty("index", -1));
        if (stripIndex < 0 || stripIndex >= MaxStrips)
            continue;

        loopPitchRoles[static_cast<size_t>(stripIndex)].store(
            static_cast<int>(sanitizeLoopPitchRole(static_cast<int>(stripState.getProperty("role", 0)))),
            std::memory_order_release);
        loopPitchSyncTimings[static_cast<size_t>(stripIndex)].store(
            static_cast<int>(sanitizeLoopPitchSyncTiming(static_cast<int>(stripState.getProperty("syncTiming", 0)))),
            std::memory_order_release);
        loopPitchAssignedMidi[static_cast<size_t>(stripIndex)].store(
            juce::jlimit(-1, 127, static_cast<int>(stripState.getProperty("assignedMidi", -1))),
            std::memory_order_release);
        loopPitchAssignedManual[static_cast<size_t>(stripIndex)].store(
            static_cast<int>(stripState.getProperty("assignedManual", 0)) != 0 ? 1 : 0,
            std::memory_order_release);
        loopPitchDetectedMidi[static_cast<size_t>(stripIndex)].store(
            juce::jlimit(-1, 127, static_cast<int>(stripState.getProperty("detectedMidi", -1))),
            std::memory_order_release);
        loopPitchDetectedHz[static_cast<size_t>(stripIndex)].store(
            juce::jlimit(0.0f, 24000.0f, static_cast<float>(stripState.getProperty("detectedHz", 0.0f))),
            std::memory_order_release);
        loopPitchDetectedPitchConfidence[static_cast<size_t>(stripIndex)].store(
            juce::jlimit(0.0f, 1.0f, static_cast<float>(stripState.getProperty("detectedPitchConfidence", 0.0f))),
            std::memory_order_release);
        loopPitchDetectedScaleIndices[static_cast<size_t>(stripIndex)].store(
            juce::jlimit(-1,
                         static_cast<int>(ModernAudioEngine::PitchScale::PentatonicMinor),
                         static_cast<int>(stripState.getProperty("detectedScaleIndex", -1))),
            std::memory_order_release);
        loopPitchDetectedScaleConfidence[static_cast<size_t>(stripIndex)].store(
            juce::jlimit(0.0f, 1.0f, static_cast<float>(stripState.getProperty("detectedScaleConfidence", 0.0f))),
            std::memory_order_release);
        loopPitchEssentiaUsed[static_cast<size_t>(stripIndex)].store(
            static_cast<int>(stripState.getProperty("essentiaUsed", 0)) != 0 ? 1 : 0,
            std::memory_order_release);
    }

    normalizeLoopPitchMasterRoles();
    applyLoopPitchSyncToAllStrips();
}

std::unique_ptr<juce::XmlElement> MlrVSTAudioProcessor::createFlipPresetStateXml(int stripIndex) const
{
    auto* strip = audioEngine != nullptr ? audioEngine->getStrip(stripIndex) : nullptr;
    if (strip == nullptr || strip->getPlayMode() != EnhancedAudioStrip::PlayMode::Sample)
        return {};

    auto* engine = const_cast<MlrVSTAudioProcessor*>(this)->getSampleModeEngine(stripIndex, false);
    if (engine == nullptr)
        return {};

    auto xml = engine->capturePersistentState().createXml("FlipState");
    const auto embeddedSample = createEmbeddedFlipSampleData(stripIndex);
    if (embeddedSample.isNotEmpty())
        xml->setAttribute(kEmbeddedFlipSampleAttr, embeddedSample);
    return xml;
}

std::unique_ptr<juce::XmlElement> MlrVSTAudioProcessor::createLoopPitchPresetStateXml(int stripIndex) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return {};

    auto xml = std::make_unique<juce::XmlElement>("LoopPitchState");
    xml->setAttribute("role", loopPitchRoles[static_cast<size_t>(stripIndex)].load(std::memory_order_acquire));
    xml->setAttribute("syncTiming", loopPitchSyncTimings[static_cast<size_t>(stripIndex)].load(std::memory_order_acquire));
    xml->setAttribute("assignedMidi", loopPitchAssignedMidi[static_cast<size_t>(stripIndex)].load(std::memory_order_acquire));
    xml->setAttribute("assignedManual", loopPitchAssignedManual[static_cast<size_t>(stripIndex)].load(std::memory_order_acquire));
    xml->setAttribute("detectedMidi", loopPitchDetectedMidi[static_cast<size_t>(stripIndex)].load(std::memory_order_acquire));
    xml->setAttribute("detectedHz", static_cast<double>(loopPitchDetectedHz[static_cast<size_t>(stripIndex)].load(std::memory_order_acquire)));
    xml->setAttribute("detectedPitchConfidence", static_cast<double>(loopPitchDetectedPitchConfidence[static_cast<size_t>(stripIndex)].load(std::memory_order_acquire)));
    xml->setAttribute("detectedScaleIndex", loopPitchDetectedScaleIndices[static_cast<size_t>(stripIndex)].load(std::memory_order_acquire));
    xml->setAttribute("detectedScaleConfidence", static_cast<double>(loopPitchDetectedScaleConfidence[static_cast<size_t>(stripIndex)].load(std::memory_order_acquire)));
    xml->setAttribute("essentiaUsed", loopPitchEssentiaUsed[static_cast<size_t>(stripIndex)].load(std::memory_order_acquire));
    return xml;
}

void MlrVSTAudioProcessor::applyFlipPresetStateXml(int stripIndex, const juce::XmlElement* flipStateXml)
{
    if (flipStateXml == nullptr || audioEngine == nullptr || stripIndex < 0 || stripIndex >= MaxStrips)
        return;

    auto* strip = audioEngine->getStrip(stripIndex);
    if (strip == nullptr)
        return;

    strip->setPlayMode(EnhancedAudioStrip::PlayMode::Sample);
    if (auto* engine = getSampleModeEngine(stripIndex, true))
    {
        const auto persistentState = SampleModePersistentState::fromXml(*flipStateXml);
        if (persistentState.samplePath.isNotEmpty())
            rememberLoadedSamplePathForStripMode(stripIndex, juce::File(persistentState.samplePath), SamplePathMode::Flip, false);
        const auto embeddedSample = flipStateXml->getStringAttribute(kEmbeddedFlipSampleAttr);
        if (embeddedSample.isNotEmpty())
        {
            loadEmbeddedFlipSampleData(stripIndex, embeddedSample, &persistentState);
        }
        else
        {
            engine->applyPersistentState(persistentState);
        }
    }
}

void MlrVSTAudioProcessor::applyLoopPitchPresetStateXml(int stripIndex, const juce::XmlElement* stateXml)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;

    loopPitchRoles[static_cast<size_t>(stripIndex)].store(static_cast<int>(LoopPitchRole::None), std::memory_order_release);
    loopPitchSyncTimings[static_cast<size_t>(stripIndex)].store(static_cast<int>(LoopPitchSyncTiming::Immediate), std::memory_order_release);
    loopPitchAssignedMidi[static_cast<size_t>(stripIndex)].store(-1, std::memory_order_release);
    loopPitchAssignedManual[static_cast<size_t>(stripIndex)].store(0, std::memory_order_release);
    loopPitchDetectedMidi[static_cast<size_t>(stripIndex)].store(-1, std::memory_order_release);
    loopPitchDetectedHz[static_cast<size_t>(stripIndex)].store(0.0f, std::memory_order_release);
    loopPitchDetectedPitchConfidence[static_cast<size_t>(stripIndex)].store(0.0f, std::memory_order_release);
    loopPitchDetectedScaleIndices[static_cast<size_t>(stripIndex)].store(-1, std::memory_order_release);
    loopPitchDetectedScaleConfidence[static_cast<size_t>(stripIndex)].store(0.0f, std::memory_order_release);
    loopPitchEssentiaUsed[static_cast<size_t>(stripIndex)].store(0, std::memory_order_release);
    loopPitchPendingRetune[static_cast<size_t>(stripIndex)].store(0, std::memory_order_release);

    if (stateXml == nullptr || !stateXml->hasTagName("LoopPitchState"))
        return;

    loopPitchRoles[static_cast<size_t>(stripIndex)].store(
        static_cast<int>(sanitizeLoopPitchRole(stateXml->getIntAttribute("role", 0))),
        std::memory_order_release);
    loopPitchSyncTimings[static_cast<size_t>(stripIndex)].store(
        static_cast<int>(sanitizeLoopPitchSyncTiming(stateXml->getIntAttribute("syncTiming", 0))),
        std::memory_order_release);
    loopPitchAssignedMidi[static_cast<size_t>(stripIndex)].store(
        juce::jlimit(-1, 127, stateXml->getIntAttribute("assignedMidi", -1)),
        std::memory_order_release);
    loopPitchAssignedManual[static_cast<size_t>(stripIndex)].store(
        stateXml->getBoolAttribute("assignedManual", false) ? 1 : 0,
        std::memory_order_release);
    loopPitchDetectedMidi[static_cast<size_t>(stripIndex)].store(
        juce::jlimit(-1, 127, stateXml->getIntAttribute("detectedMidi", -1)),
        std::memory_order_release);
    loopPitchDetectedHz[static_cast<size_t>(stripIndex)].store(
        juce::jlimit(0.0f, 24000.0f, static_cast<float>(stateXml->getDoubleAttribute("detectedHz", 0.0))),
        std::memory_order_release);
    loopPitchDetectedPitchConfidence[static_cast<size_t>(stripIndex)].store(
        juce::jlimit(0.0f, 1.0f, static_cast<float>(stateXml->getDoubleAttribute("detectedPitchConfidence", 0.0))),
        std::memory_order_release);
    loopPitchDetectedScaleIndices[static_cast<size_t>(stripIndex)].store(
        juce::jlimit(-1,
                     static_cast<int>(ModernAudioEngine::PitchScale::PentatonicMinor),
                     stateXml->getIntAttribute("detectedScaleIndex", -1)),
        std::memory_order_release);
    loopPitchDetectedScaleConfidence[static_cast<size_t>(stripIndex)].store(
        juce::jlimit(0.0f, 1.0f, static_cast<float>(stateXml->getDoubleAttribute("detectedScaleConfidence", 0.0))),
        std::memory_order_release);
    loopPitchEssentiaUsed[static_cast<size_t>(stripIndex)].store(
        stateXml->getBoolAttribute("essentiaUsed", false) ? 1 : 0,
        std::memory_order_release);

    requestLoopPitchRoleStateUpdate(stripIndex);
}

void MlrVSTAudioProcessor::loadDefaultPathsFromState(const juce::ValueTree& state)
{
    auto paths = state.getChildWithName("DefaultPaths");
    if (!paths.isValid())
        return;

    for (int i = 0; i < MaxStrips; ++i)
    {
        const auto idx = static_cast<size_t>(i);
        const auto loopKey = "loopDir" + juce::String(i);
        const auto stepKey = "stepDir" + juce::String(i);
        const auto flipKey = "flipDir" + juce::String(i);
        const auto recentLoopKey = "recentLoopDir" + juce::String(i);
        const auto recentStepKey = "recentStepDir" + juce::String(i);
        const auto recentFlipKey = "recentFlipDir" + juce::String(i);

        defaultLoopDirectories[idx] = restoreStoredAbsolutePath(paths.getProperty(loopKey).toString());
        defaultStepDirectories[idx] = restoreStoredAbsolutePath(paths.getProperty(stepKey).toString());
        defaultFlipDirectories[idx] = restoreStoredAbsolutePath(paths.getProperty(flipKey).toString());
        recentLoopDirectories[idx] = restoreStoredAbsolutePath(paths.getProperty(recentLoopKey).toString());
        recentStepDirectories[idx] = restoreStoredAbsolutePath(paths.getProperty(recentStepKey).toString());
        recentFlipDirectories[idx] = restoreStoredAbsolutePath(paths.getProperty(recentFlipKey).toString());
    }

    lastSampleFolder = restoreStoredAbsolutePath(paths.getProperty("lastSampleFolder").toString());

    for (int slot = 0; slot < BrowserFavoriteSlots; ++slot)
    {
        const auto key = "favoriteDir" + juce::String(slot);
        const auto flipKey = "favoriteFlipDir" + juce::String(slot);
        browserFavoriteDirectories[static_cast<size_t>(slot)] =
            restoreStoredAbsolutePath(paths.getProperty(key).toString());
        browserFlipFavoriteDirectories[static_cast<size_t>(slot)] =
            restoreStoredAbsolutePath(paths.getProperty(flipKey).toString());
    }

    savePersistentDefaultPaths();
}

void MlrVSTAudioProcessor::loadControlPagesFromState(const juce::ValueTree& state)
{
    auto controlPages = state.getChildWithName("ControlPages");
    if (!controlPages.isValid())
    {
        savePersistentControlPages();
        return;
    }

    ControlPageOrder parsedOrder{};
    int parsedCount = 0;

    for (int i = 0; i < NumControlRowPages; ++i)
    {
        const auto key = "slot" + juce::String(i);
        const auto value = controlPages.getProperty(key).toString();
        ControlMode mode = ControlMode::Normal;
        if (!controlModeFromKey(value, mode) || mode == ControlMode::Normal)
            continue;

        bool duplicate = false;
        for (int j = 0; j < parsedCount; ++j)
        {
            if (parsedOrder[static_cast<size_t>(j)] == mode)
            {
                duplicate = true;
                break;
            }
        }
        if (duplicate)
            continue;

        parsedOrder[static_cast<size_t>(parsedCount)] = mode;
        ++parsedCount;
    }

    const ControlPageOrder defaultOrder {
        ControlMode::Speed,
        ControlMode::Pan,
        ControlMode::Volume,
        ControlMode::GrainSize,
        ControlMode::Swing,
        ControlMode::Gate,
        ControlMode::FileBrowser,
        ControlMode::GroupAssign,
        ControlMode::Filter,
        ControlMode::Delay,
        ControlMode::Pitch,
        ControlMode::Modulation,
        ControlMode::Preset,
        ControlMode::StepEdit
    };

    for (const auto mode : defaultOrder)
    {
        bool alreadyPresent = false;
        for (int i = 0; i < parsedCount; ++i)
        {
            if (parsedOrder[static_cast<size_t>(i)] == mode)
            {
                alreadyPresent = true;
                break;
            }
        }
        if (!alreadyPresent && parsedCount < NumControlRowPages)
            parsedOrder[static_cast<size_t>(parsedCount++)] = mode;
    }

    if (parsedCount == NumControlRowPages)
    {
        const juce::ScopedLock lock(controlPageOrderLock);
        controlPageOrder = parsedOrder;
    }

    const bool momentary = controlPages.getProperty("momentary", true);
    controlPageMomentary.store(momentary, std::memory_order_release);
    const int swingDivision = static_cast<int>(controlPages.getProperty("swingDivision", 1));
    setSwingDivisionSelection(swingDivision);
    savePersistentControlPages();
}

void MlrVSTAudioProcessor::loadPersistentDefaultPaths()
{
    auto settingsFile = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("mlrVST")
        .getChildFile("DefaultPaths.xml");

    if (!settingsFile.existsAsFile())
    {
        resetCurrentBrowserDirectoriesToDefaultPaths(false);
        savePersistentDefaultPaths();
        return;
    }

    auto xml = juce::XmlDocument::parse(settingsFile);
    if (xml == nullptr || xml->getTagName() != "DefaultPaths")
    {
        // Auto-heal missing/corrupt default path storage.
        resetCurrentBrowserDirectoriesToDefaultPaths(false);
        savePersistentDefaultPaths();
        return;
    }

    for (int i = 0; i < MaxStrips; ++i)
    {
        const auto idx = static_cast<size_t>(i);
        defaultLoopDirectories[idx] = restoreStoredAbsolutePath(xml->getStringAttribute("loopDir" + juce::String(i)));
        defaultStepDirectories[idx] = restoreStoredAbsolutePath(xml->getStringAttribute("stepDir" + juce::String(i)));
        defaultFlipDirectories[idx] = restoreStoredAbsolutePath(xml->getStringAttribute("flipDir" + juce::String(i)));
        recentLoopDirectories[idx] = restoreStoredAbsolutePath(xml->getStringAttribute("recentLoopDir" + juce::String(i)));
        recentStepDirectories[idx] = restoreStoredAbsolutePath(xml->getStringAttribute("recentStepDir" + juce::String(i)));
        recentFlipDirectories[idx] = restoreStoredAbsolutePath(xml->getStringAttribute("recentFlipDir" + juce::String(i)));
    }

    lastSampleFolder = restoreStoredAbsolutePath(xml->getStringAttribute("lastSampleFolder"));

    for (int slot = 0; slot < BrowserFavoriteSlots; ++slot)
    {
        browserFavoriteDirectories[static_cast<size_t>(slot)] =
            restoreStoredAbsolutePath(xml->getStringAttribute("favoriteDir" + juce::String(slot)));
        browserFlipFavoriteDirectories[static_cast<size_t>(slot)] =
            restoreStoredAbsolutePath(xml->getStringAttribute("favoriteFlipDir" + juce::String(slot)));
    }

    resetCurrentBrowserDirectoriesToDefaultPaths(false);
}

void MlrVSTAudioProcessor::resetCurrentBrowserDirectoriesToDefaultPaths(bool persist)
{
    juce::File firstStoredDefault;
    auto copyDefaultDirectory = [&firstStoredDefault](const juce::File& directory) -> juce::File
    {
        const auto restored = restoreStoredAbsolutePath(directory.getFullPathName());
        if (restored == juce::File())
            return {};
        if (firstStoredDefault == juce::File())
            firstStoredDefault = restored;
        return restored;
    };

    for (int i = 0; i < MaxStrips; ++i)
    {
        const auto idx = static_cast<size_t>(i);
        recentLoopDirectories[idx] = copyDefaultDirectory(defaultLoopDirectories[idx]);
        recentStepDirectories[idx] = copyDefaultDirectory(defaultStepDirectories[idx]);
        recentFlipDirectories[idx] = copyDefaultDirectory(defaultFlipDirectories[idx]);
    }

    lastSampleFolder = firstStoredDefault;

    if (persist)
        savePersistentDefaultPaths();
}

void MlrVSTAudioProcessor::savePersistentDefaultPaths() const
{
    auto settingsDir = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("mlrVST");
    if (!settingsDir.exists())
        settingsDir.createDirectory();

    auto settingsFile = settingsDir.getChildFile("DefaultPaths.xml");
    juce::XmlElement xml("DefaultPaths");

    for (int i = 0; i < MaxStrips; ++i)
    {
        const auto idx = static_cast<size_t>(i);
        xml.setAttribute("loopDir" + juce::String(i), defaultLoopDirectories[idx].getFullPathName());
        xml.setAttribute("stepDir" + juce::String(i), defaultStepDirectories[idx].getFullPathName());
        xml.setAttribute("flipDir" + juce::String(i), defaultFlipDirectories[idx].getFullPathName());
        xml.setAttribute("recentLoopDir" + juce::String(i), recentLoopDirectories[idx].getFullPathName());
        xml.setAttribute("recentStepDir" + juce::String(i), recentStepDirectories[idx].getFullPathName());
        xml.setAttribute("recentFlipDir" + juce::String(i), recentFlipDirectories[idx].getFullPathName());
    }

    xml.setAttribute("lastSampleFolder", lastSampleFolder.getFullPathName());

    for (int slot = 0; slot < BrowserFavoriteSlots; ++slot)
    {
        xml.setAttribute("favoriteDir" + juce::String(slot), browserFavoriteDirectories[static_cast<size_t>(slot)].getFullPathName());
        xml.setAttribute("favoriteFlipDir" + juce::String(slot), browserFlipFavoriteDirectories[static_cast<size_t>(slot)].getFullPathName());
    }

    xml.writeTo(settingsFile);
}

void MlrVSTAudioProcessor::loadPersistentControlPages()
{
    const auto previousSuppress = suppressPersistentGlobalControlsSave.load(std::memory_order_acquire);
    suppressPersistentGlobalControlsSave.store(1, std::memory_order_release);
    auto xml = loadGlobalSettingsXml();
    if (xml == nullptr)
    {
        suppressPersistentGlobalControlsSave.store(previousSuppress, std::memory_order_release);
        savePersistentControlPages();
        return;
    }
    if (xml->getTagName() != "GlobalSettings")
    {
        suppressPersistentGlobalControlsSave.store(previousSuppress, std::memory_order_release);
        savePersistentControlPages();
        return;
    }

    juce::ValueTree state("MlrVST");
    auto controlPages = juce::ValueTree("ControlPages");
    for (int i = 0; i < NumControlRowPages; ++i)
    {
        const auto key = "slot" + juce::String(i);
        controlPages.setProperty(key, xml->getStringAttribute(key), nullptr);
    }
    controlPages.setProperty("momentary", xml->getBoolAttribute("momentary", true), nullptr);
    controlPages.setProperty("swingDivision", xml->getIntAttribute("swingDivision", 1), nullptr);
    state.addChild(controlPages, -1, nullptr);

    loadControlPagesFromState(state);
    suppressPersistentGlobalControlsSave.store(previousSuppress, std::memory_order_release);
}

void MlrVSTAudioProcessor::loadPersistentGlobalControls()
{
    auto xml = loadGlobalSettingsXml();
    if (xml == nullptr || xml->getTagName() != "GlobalSettings")
    {
        persistentGlobalControlsReady.store(1, std::memory_order_release);
        return;
    }

    auto restoreFloatParam = [this, &xml](const char* attrName, const char* paramId, double minValue, double maxValue) -> bool
    {
        if (!xml->hasAttribute(attrName))
            return false;
        auto* param = parameters.getParameter(paramId);
        if (param == nullptr)
            return false;
        const auto restored = static_cast<float>(
            juce::jlimit(minValue, maxValue, xml->getDoubleAttribute(attrName)));
        param->setValueNotifyingHost(param->convertTo0to1(restored));
        return true;
    };

    auto restoreChoiceParam = [this, &xml](const char* attrName, const char* paramId, int minValue, int maxValue) -> bool
    {
        if (!xml->hasAttribute(attrName))
            return false;
        auto* param = parameters.getParameter(paramId);
        if (param == nullptr)
            return false;
        const auto restored = static_cast<float>(
            juce::jlimit(minValue, maxValue, xml->getIntAttribute(attrName)));
        param->setValueNotifyingHost(param->convertTo0to1(restored));
        return true;
    };

    auto restorePitchControlModeParam = [this, &xml]() -> bool
    {
        if (!xml->hasAttribute("pitchControlMode"))
            return false;

        auto* param = parameters.getParameter("pitchControlMode");
        if (param == nullptr)
            return false;

        int restored = xml->getIntAttribute("pitchControlMode");
        if (xml->getIntAttribute("pitchControlModeSchemaVersion", 0) < kPitchControlModeSchemaVersion)
            restored = migrateLegacyPitchControlModeIndex(restored);

        restored = sanitizePitchControlModeIndex(restored);
        param->setValueNotifyingHost(param->convertTo0to1(static_cast<float>(restored)));
        return true;
    };

    auto restoreBoolParam = [this, &xml](const char* attrName, const char* paramId) -> bool
    {
        if (!xml->hasAttribute(attrName))
            return false;
        auto* param = parameters.getParameter(paramId);
        if (param == nullptr)
            return false;
        param->setValueNotifyingHost(param->convertTo0to1(xml->getBoolAttribute(attrName) ? 1.0f : 0.0f));
        return true;
    };

    suppressPersistentGlobalControlsSave.store(1, std::memory_order_release);
    bool anyRestored = false;

    for (int i = 0; i < MacroCount; ++i)
    {
        const auto ccAttrName = "macroCc" + juce::String(i);
        if (xml->hasAttribute(ccAttrName))
        {
            const int restoredCc = juce::jlimit(-1, 127, xml->getIntAttribute(ccAttrName, getDefaultMacroMidiCc(i)));
            macroMidiCcAssignments[static_cast<size_t>(i)].store(restoredCc, std::memory_order_release);
            anyRestored = true;
        }

        const auto targetAttrName = "macroTarget" + juce::String(i);
        const auto targetKeyAttrName = "macroTargetKey" + juce::String(i);
        bool restoredMacroTarget = false;
        if (xml->hasAttribute(targetKeyAttrName))
        {
            PerformanceTarget restoredTarget = PerformanceTarget::None;
            if (tryParsePerformanceTargetKey(xml->getStringAttribute(targetKeyAttrName), restoredTarget))
            {
                macroTargetAssignments[static_cast<size_t>(i)].store(
                    static_cast<int>(sanitizeMacroPerformanceTarget(restoredTarget)),
                    std::memory_order_release);
                anyRestored = true;
                restoredMacroTarget = true;
            }
        }
        if (!restoredMacroTarget && xml->hasAttribute(targetAttrName))
        {
            const auto restoredTarget = performanceTargetFromLegacyMacroRaw(
                xml->getIntAttribute(targetAttrName,
                                     legacyMacroRawFromPerformanceTarget(getDefaultMacroTarget(i))));
            macroTargetAssignments[static_cast<size_t>(i)].store(
                static_cast<int>(sanitizeMacroPerformanceTarget(restoredTarget)),
                std::memory_order_release);
            anyRestored = true;
        }
    }

    if (xml->hasAttribute("rootNoteMidi"))
    {
        globalRootNoteMidi.store(juce::jlimit(0, 127, xml->getIntAttribute("rootNoteMidi", 60)),
                                 std::memory_order_release);
        anyRestored = true;
    }
    if (xml->hasAttribute("pitchScale"))
    {
        globalPitchScale.store(
            juce::jlimit(0,
                         static_cast<int>(ModernAudioEngine::PitchScale::PentatonicMinor),
                         xml->getIntAttribute("pitchScale", static_cast<int>(ModernAudioEngine::PitchScale::Chromatic))),
            std::memory_order_release);
        anyRestored = true;
    }

    cancelMacroMidiLearn();
    anyRestored = restoreFloatParam("masterVolume", "masterVolume", 0.0, 1.0) || anyRestored;
    anyRestored = restoreFloatParam("limiterThreshold", "limiterThreshold", 0.0, 0.0) || anyRestored;
    anyRestored = restoreBoolParam("limiterEnabled", "limiterEnabled") || anyRestored;
    anyRestored = restoreChoiceParam("quantize", "quantize", 0, 9) || anyRestored;
    anyRestored = restoreChoiceParam("innerLoopLength", "innerLoopLength", 0, 4) || anyRestored;
    innerLoopLengthSelection.store(innerLoopLengthParam != nullptr
                                       ? juce::jlimit(0, 4, static_cast<int>(innerLoopLengthParam->load(std::memory_order_acquire)))
                                       : 0,
                                   std::memory_order_release);
    anyRestored = restoreChoiceParam("quality", "quality", 0, 3) || anyRestored;
    anyRestored = restoreFloatParam("pitchSmoothing", "pitchSmoothing", 0.0, 1.0) || anyRestored;
    anyRestored = restoreFloatParam("inputMonitor", "inputMonitor", 0.0, 1.0) || anyRestored;
    anyRestored = restoreFloatParam("crossfadeLength", "crossfadeLength", 1.0, 50.0) || anyRestored;
    anyRestored = restoreFloatParam("triggerFadeIn", "triggerFadeIn", 0.01, 120.0) || anyRestored;
    anyRestored = restoreChoiceParam("outputRouting", "outputRouting", 0, 1) || anyRestored;
    anyRestored = restorePitchControlModeParam() || anyRestored;
    anyRestored = restoreChoiceParam("flipTempoMatchMode", "flipTempoMatchMode", 0, 1) || anyRestored;
    bool stretchBackendRestored = restoreChoiceParam("stretchBackend", "stretchBackend", 0, 2);
    anyRestored = restoreBoolParam("sceneMode", "sceneMode") || anyRestored;
    anyRestored = restoreChoiceParam("transientOnsetMethod", "transientOnsetMethod", 0, 2) || anyRestored;
    anyRestored = restoreChoiceParam("transientSensitivity", "transientSensitivity", 0, 4) || anyRestored;
    anyRestored = restoreChoiceParam("transientSnap", "transientSnap", 0, 4) || anyRestored;
    anyRestored = restoreChoiceParam("transientSpacing", "transientSpacing", 0, 4) || anyRestored;
    anyRestored = restoreChoiceParam("sceneRecallMode", "sceneRecallMode", 0, 3) || anyRestored;
    if (xml->hasAttribute("sceneEditorGridEnabled"))
    {
        sceneEditorGridEnabledState = xml->getBoolAttribute("sceneEditorGridEnabled", true);
        anyRestored = true;
    }
    if (xml->hasAttribute("sceneEditorGridDivision"))
    {
        sceneEditorGridDivisionState = juce::jlimit(1, 64, xml->getIntAttribute("sceneEditorGridDivision", 16));
        anyRestored = true;
    }
    if (xml->hasAttribute("sceneEditorDrawMode"))
    {
        sceneEditorDrawModeEnabledState = xml->getBoolAttribute("sceneEditorDrawMode", false);
        anyRestored = true;
    }
    if (xml->hasAttribute("sceneEditorLaneOverlays"))
    {
        sceneEditorLaneOverlaysEnabledState = xml->getBoolAttribute("sceneEditorLaneOverlays", true);
        anyRestored = true;
    }
    if (xml->hasAttribute("sceneEditorZoomFactor"))
    {
        const int restoredZoom = xml->getIntAttribute("sceneEditorZoomFactor", 1);
        static constexpr std::array<int, 7> kZoomChoices{ 1, 2, 3, 4, 5, 6, 8 };
        sceneEditorZoomFactorState = kZoomChoices.front();
        int bestDelta = std::abs(restoredZoom - sceneEditorZoomFactorState);
        for (const int candidate : kZoomChoices)
        {
            const int delta = std::abs(restoredZoom - candidate);
            if (delta < bestDelta)
            {
                bestDelta = delta;
                sceneEditorZoomFactorState = candidate;
            }
        }
        anyRestored = true;
    }
    if (xml->hasAttribute("sceneEditorFollowPlayhead"))
    {
        sceneEditorFollowPlayheadState = xml->getBoolAttribute("sceneEditorFollowPlayhead", false);
        anyRestored = true;
    }
    for (int stripIndex = 0; stripIndex < MaxStrips; ++stripIndex)
    {
        const auto automationKey = "sceneStripAutomationExpanded" + juce::String(stripIndex);
        if (xml->hasAttribute(automationKey))
        {
            sceneEditorStripAutomationExpanded[static_cast<size_t>(stripIndex)] = xml->getBoolAttribute(automationKey, true);
            anyRestored = true;
        }

        const auto heightKey = "sceneStripHeightExpanded" + juce::String(stripIndex);
        if (xml->hasAttribute(heightKey))
        {
            sceneEditorStripHeightExpanded[static_cast<size_t>(stripIndex)] = xml->getBoolAttribute(heightKey, false);
            anyRestored = true;
        }
    }
    if (!stretchBackendRestored && xml->hasAttribute("soundTouchEnabled"))
    {
        auto* param = parameters.getParameter("stretchBackend");
        if (param != nullptr)
        {
            const int restoredBackend = xml->getBoolAttribute("soundTouchEnabled")
                ? static_cast<int>(TimeStretchBackend::SoundTouch)
                : static_cast<int>(TimeStretchBackend::Resample);
            param->setValueNotifyingHost(param->convertTo0to1(static_cast<float>(restoredBackend)));
            stretchBackendRestored = true;
        }
    }
    anyRestored = stretchBackendRestored || anyRestored;
    if (soundTouchEnabledParam != nullptr)
    {
        const bool legacySoundTouchEnabled = getStretchBackend() != TimeStretchBackend::Resample;
        auto* legacyParam = parameters.getParameter("soundTouchEnabled");
        if (legacyParam != nullptr)
            legacyParam->setValueNotifyingHost(legacyParam->convertTo0to1(legacySoundTouchEnabled ? 1.0f : 0.0f));
    }
    suppressPersistentGlobalControlsSave.store(0, std::memory_order_release);
    persistentGlobalControlsDirty.store(0, std::memory_order_release);

    if (!anyRestored)
        savePersistentControlPages();
    else
        saveGlobalSettingsXml(*xml);

    syncSceneModeFromParameters();
    applyLoopPitchSyncToAllStrips();
    syncTransientDetectionSettingsFromParameters(true);
    persistentGlobalControlsReady.store(1, std::memory_order_release);
}

void MlrVSTAudioProcessor::savePersistentControlPages() const
{
    if (suppressPersistentGlobalControlsSave.load(std::memory_order_acquire) != 0)
        return;
    juce::XmlElement xml("GlobalSettings");
    const auto orderSnapshot = getControlPageOrder();
    for (int i = 0; i < NumControlRowPages; ++i)
    {
        const auto key = "slot" + juce::String(i);
        xml.setAttribute(key, controlModeToKey(orderSnapshot[static_cast<size_t>(i)]));
    }
    xml.setAttribute("momentary", isControlPageMomentary());
    xml.setAttribute("swingDivision", swingDivisionSelection.load(std::memory_order_acquire));
    if (masterVolumeParam)
        xml.setAttribute("masterVolume", static_cast<double>(masterVolumeParam->load(std::memory_order_acquire)));
    if (limiterThresholdParam)
        xml.setAttribute("limiterThreshold", 0.0);
    if (limiterEnabledParam)
        xml.setAttribute("limiterEnabled", limiterEnabledParam->load(std::memory_order_acquire) >= 0.5f);
    if (quantizeParam)
        xml.setAttribute("quantize", static_cast<int>(quantizeParam->load(std::memory_order_acquire)));
    if (innerLoopLengthParam)
        xml.setAttribute("innerLoopLength", static_cast<int>(innerLoopLengthParam->load(std::memory_order_acquire)));
    if (grainQualityParam)
        xml.setAttribute("quality", static_cast<int>(grainQualityParam->load(std::memory_order_acquire)));
    if (pitchSmoothingParam)
        xml.setAttribute("pitchSmoothing", static_cast<double>(pitchSmoothingParam->load(std::memory_order_acquire)));
    if (inputMonitorParam)
        xml.setAttribute("inputMonitor", static_cast<double>(inputMonitorParam->load(std::memory_order_acquire)));
    if (crossfadeLengthParam)
        xml.setAttribute("crossfadeLength", static_cast<double>(crossfadeLengthParam->load(std::memory_order_acquire)));
    if (triggerFadeInParam)
        xml.setAttribute("triggerFadeIn", static_cast<double>(triggerFadeInParam->load(std::memory_order_acquire)));
    if (outputRoutingParam)
        xml.setAttribute("outputRouting", static_cast<int>(outputRoutingParam->load(std::memory_order_acquire)));
    if (pitchControlModeParam)
    {
        const int storedMode = sanitizePitchControlModeIndex(
            static_cast<int>(std::round(pitchControlModeParam->load(std::memory_order_acquire))));
        xml.setAttribute("pitchControlMode", storedMode);
        xml.setAttribute("pitchControlModeSchemaVersion", kPitchControlModeSchemaVersion);
    }
    if (flipTempoMatchModeParam)
        xml.setAttribute("flipTempoMatchMode", static_cast<int>(flipTempoMatchModeParam->load(std::memory_order_acquire)));
    if (stretchBackendParam)
        xml.setAttribute("stretchBackend", static_cast<int>(stretchBackendParam->load(std::memory_order_acquire)));
    if (sceneModeParam)
        xml.setAttribute("sceneMode", sceneModeParam->load(std::memory_order_acquire) > 0.5f);
    if (transientOnsetMethodParam)
        xml.setAttribute("transientOnsetMethod", static_cast<int>(transientOnsetMethodParam->load(std::memory_order_acquire)));
    if (transientSensitivityParam)
        xml.setAttribute("transientSensitivity", static_cast<int>(transientSensitivityParam->load(std::memory_order_acquire)));
    if (transientSnapParam)
        xml.setAttribute("transientSnap", static_cast<int>(transientSnapParam->load(std::memory_order_acquire)));
    if (transientSpacingParam)
        xml.setAttribute("transientSpacing", static_cast<int>(transientSpacingParam->load(std::memory_order_acquire)));
    if (sceneRecallModeParam)
        xml.setAttribute("sceneRecallMode", static_cast<int>(sceneRecallModeParam->load(std::memory_order_acquire)));
    xml.setAttribute("sceneEditorGridEnabled", sceneEditorGridEnabledState);
    xml.setAttribute("sceneEditorGridDivision", sceneEditorGridDivisionState);
    xml.setAttribute("sceneEditorDrawMode", sceneEditorDrawModeEnabledState);
    xml.setAttribute("sceneEditorLaneOverlays", sceneEditorLaneOverlaysEnabledState);
    xml.setAttribute("sceneEditorZoomFactor", sceneEditorZoomFactorState);
    xml.setAttribute("sceneEditorFollowPlayhead", sceneEditorFollowPlayheadState);
    for (int stripIndex = 0; stripIndex < MaxStrips; ++stripIndex)
    {
        xml.setAttribute("sceneStripAutomationExpanded" + juce::String(stripIndex),
                         sceneEditorStripAutomationExpanded[static_cast<size_t>(stripIndex)]);
        xml.setAttribute("sceneStripHeightExpanded" + juce::String(stripIndex),
                         sceneEditorStripHeightExpanded[static_cast<size_t>(stripIndex)]);
    }
    if (soundTouchEnabledParam)
        xml.setAttribute("soundTouchEnabled", getStretchBackend() != TimeStretchBackend::Resample);
    for (int i = 0; i < MacroCount; ++i)
    {
        const auto target = getMacroTarget(i);
        xml.setAttribute("macroCc" + juce::String(i), getMacroMidiCc(i));
        xml.setAttribute("macroTarget" + juce::String(i), legacyMacroRawFromPerformanceTarget(target));
        xml.setAttribute("macroTargetKey" + juce::String(i), performanceTargetKey(target));
    }
    xml.setAttribute("rootNoteMidi", globalRootNoteMidi.load(std::memory_order_acquire));
    xml.setAttribute("pitchScale", globalPitchScale.load(std::memory_order_acquire));
    saveGlobalSettingsXml(xml);
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
    int64_t globalSample = audioEngine->getGlobalSampleCount();
    
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
    
    // ============================================================
    // COMPREHENSIVE DEBUG LOGGING
    // ============================================================
    if (kEnableTriggerDebugLogging)
    {
        juce::File logFile = juce::File::getSpecialLocation(juce::File::userDesktopDirectory)
                               .getChildFile("mlrVST_COMPREHENSIVE_DEBUG.txt");
        juce::FileOutputStream stream(logFile, 1024);
        if (stream.openedOk())
        {
            juce::String timestamp = juce::Time::getCurrentTime().toString(true, true, true, true);
            juce::String msg =
                "═══════════════════════════════════════════════════════\n"
                "BUTTON PRESS: " + timestamp + "\n"
                "───────────────────────────────────────────────────────\n"
                "Strip: " + juce::String(stripIndex) + " | Column: " + juce::String(column) + "\n"
                "───────────────────────────────────────────────────────\n"
                "PLAYHEAD POSITION:\n"
                "  currentPPQ:     " + juce::String(currentPPQ, 6) + "\n"
                "  currentBeat:    " + juce::String(timelineBeat, 6) + "\n"
                "  globalSample:   " + juce::String(globalSample) + "\n"
                "───────────────────────────────────────────────────────\n"
                "QUANTIZATION SETTINGS:\n"
                "  quantizeChoice:  " + juce::String(quantizeChoice) + " (UI selection)\n"
                "  quantizeValue:   " + juce::String(quantizeValue) + " (divisions per bar)\n"
                "  quantBeats:      " + juce::String(quantBeats, 4) + " beats per division\n"
                "  useQuantize:     " + juce::String(useQuantize ? "YES" : "NO") + "\n"
                "───────────────────────────────────────────────────────\n"
                "GRID CALCULATION:\n"
                "  nextGridPPQ:    " + juce::String(nextGridPPQ, 6) + "\n"
                "  beatsToWait:    " + juce::String(nextGridPPQ - currentPPQ, 6) + "\n"
                "───────────────────────────────────────────────────────\n"
                "GATE STATUS:\n"
                "  gateClosed:     " + juce::String(gateClosed ? "YES (trigger pending)" : "NO (ready)") + "\n"
                "  ACTION:         " + juce::String(gateClosed ? "IGNORE THIS PRESS" : "SCHEDULE TRIGGER") + "\n"
                "───────────────────────────────────────────────────────\n"
                "PATH: " + juce::String(useQuantize ? "QUANTIZED" : "IMMEDIATE") + "\n"
                "═══════════════════════════════════════════════════════\n\n";
            stream.writeText(msg, false, false, nullptr);
        }
    }
    
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

        recordSceneTriggerEvent(stripIndex, column, eventBeat, recordedSliceId, recordedSliceStartSample);
        if (scenePerformanceRecorder.isRecording() && useQuantize)
            rememberPendingSceneTriggerRecord(stripIndex, column, eventBeat);
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
                int recordedSliceId = -1;
                int64_t recordedSliceStartSample = -1;
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
    syncSceneModeFromParameters();
    servicePendingScenePreloadRequest();
    processPendingSceneAutosave();
    processPendingSceneApply();
    processPendingSceneRecorderApply();
    applyPendingSceneParameterState();
    if (isSceneModeEnabled() && audioEngine != nullptr)
        syncSceneMotionStateFromEngine(activeSceneSlot);
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
        const auto nowMs = juce::Time::currentTimeMillis();
        if (lastPersistentGlobalControlsSaveMs == 0
            || (nowMs - lastPersistentGlobalControlsSaveMs) >= kPersistentGlobalControlsSaveDebounceMs)
        {
            savePersistentControlPages();
            persistentGlobalControlsDirty.store(0, std::memory_order_release);
            lastPersistentGlobalControlsSaveMs = nowMs;
        }
    }

    if (pendingPersistentGlobalControlsRestore.load(std::memory_order_acquire) != 0)
    {
        const auto nowMs = juce::Time::currentTimeMillis();
        if (nowMs >= pendingPersistentGlobalControlsRestoreMs)
        {
            loadPersistentGlobalControls();
            persistentGlobalControlsApplied = true;
            if (pendingPersistentGlobalControlsRestoreRemaining > 1)
            {
                --pendingPersistentGlobalControlsRestoreRemaining;
                pendingPersistentGlobalControlsRestoreMs = nowMs + 400;
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

    if (audioEngine != nullptr)
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
    }

    // Update monome LEDs regularly for smooth playhead
    if (monomeConnection.isConnected() && audioEngine)
    {
        const auto nowMs = juce::Time::currentTimeMillis();
        if (monomeConnection.supportsGrid()
            && (lastGridLedUpdateTimeMs == 0 || (nowMs - lastGridLedUpdateTimeMs) >= kGridRefreshMs))
        {
            updateMonomeLEDs();
            lastGridLedUpdateTimeMs = nowMs;
        }
        if (monomeConnection.supportsArc())
            updateMonomeArcRings();
    }
}

void MlrVSTAudioProcessor::loadAdjacentFile(int stripIndex, int direction)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;
    
    auto* strip = audioEngine->getStrip(stripIndex);
    if (!strip) return;
    
    // Get current file for this strip.
    // If strip has no loaded audio, force first-file fallback regardless of any
    // stale path cached in currentStripFiles.
    const bool hasCurrentAudio = (strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Sample)
        ? hasSampleModeAudio(stripIndex)
        : strip->hasAudio();
    juce::File currentFile;
    if (isLoopStripLoadInFlight(stripIndex))
        currentFile = pendingLoopStripFiles[static_cast<size_t>(stripIndex)];
    else if (hasCurrentAudio)
        currentFile = currentStripFiles[static_cast<size_t>(stripIndex)];

    // Determine folder to browse from strip-specific browser path context.
    juce::File folderToUse = getCurrentBrowserDirectoryForStrip(stripIndex);
    if (!safeFileIsDirectory(folderToUse))
        return;
    
    // Get all audio files in folder
    juce::Array<juce::File> audioFiles;
    for (auto& file : folderToUse.findChildFiles(juce::File::findFiles, false))
    {
        if (isAudioFileSupported(file))
        {
            audioFiles.add(file);
        }
    }

    // If no files at top level, allow browsing into nested pack folders.
    if (audioFiles.size() == 0)
    {
        for (auto& file : folderToUse.findChildFiles(juce::File::findFiles, true))
        {
            if (isAudioFileSupported(file))
                audioFiles.add(file);
        }
    }

    if (audioFiles.size() == 0) return;
    audioFiles.sort();
    
    // Find current file index
    int currentIndex = -1;
    if (safeFileExistsAsFile(currentFile))
    {
        for (int i = 0; i < audioFiles.size(); ++i)
        {
            if (audioFiles[i] == currentFile)
            {
                currentIndex = i;
                break;
            }
        }
    }

    juce::File fileToLoad;
    if (currentIndex < 0)
    {
        // Requirement: if no sample is currently loaded on this strip,
        // both Prev and Next should load the first file in the selected folder.
        fileToLoad = audioFiles[0];
    }
    else
    {
        // Calculate new index with wraparound
        int newIndex = currentIndex + direction;
        if (newIndex < 0) newIndex = audioFiles.size() - 1;
        if (newIndex >= audioFiles.size()) newIndex = 0;
        fileToLoad = audioFiles[newIndex];
    }
    
    if (!safeFileExistsAsFile(fileToLoad))
    {
        return;
    }
    
    loadSampleToStripPreservingPlaybackState(stripIndex, fileToLoad);
}

//==============================================================================
// Preset Management
//==============================================================================

void MlrVSTAudioProcessor::resetRuntimePresetStateToDefaults()
{
    if (!audioEngine)
        return;

    ScopedSceneAutosaveSuppression suppressSceneAutosave(*this);
    clearPendingActiveSceneAutosave();

    pendingPresetLoadIndex.store(-1, std::memory_order_release);
    pendingSceneApplyMainPreset.store(-1, std::memory_order_release);
    pendingSceneApplySlot.store(-1, std::memory_order_release);
    pendingSceneApplySequenceDriven.store(0, std::memory_order_release);
    pendingSceneApplySequenceStep.store(-1, std::memory_order_release);
    pendingSceneApplyTargetPpq.store(-1.0, std::memory_order_release);
    pendingSceneApplyTargetTempo.store(120.0, std::memory_order_release);
    pendingSceneApplyTargetSample.store(-1, std::memory_order_release);
    pendingScenePreloadDirty.store(0, std::memory_order_release);
    pendingScenePreloadMainPreset.store(-1, std::memory_order_release);
    pendingScenePreloadSceneSlot.store(-1, std::memory_order_release);
    pendingScenePreloadSequenceDriven.store(0, std::memory_order_release);
    pendingScenePreloadSequenceStep.store(-1, std::memory_order_release);
    pendingScenePreloadTargetPpq.store(-1.0, std::memory_order_release);
    pendingScenePreloadTargetTempo.store(120.0, std::memory_order_release);
    pendingScenePreloadTargetSample.store(-1, std::memory_order_release);
    pendingScenePreloadTransitionType.store(static_cast<int>(SceneChainTransitionType::None), std::memory_order_release);
    pendingSceneRecall = {};
    preparedSceneRenderContext.reset();
    activeSceneTransitionContext.reset();
    sceneTransitionSamplesTotal = 0;
    sceneTransitionSamplesRendered = 0;
    sceneTransitionStartSampleOffset = 0;
    sceneTransitionCommitPending = false;
    suppressOwnedStripParameterSync.store(0, std::memory_order_release);
    {
        const juce::ScopedLock lock(pendingSceneParameterStateLock);
        pendingSceneParameterState = {};
    }
    pendingSceneParameterStateDirty.store(0, std::memory_order_release);
    sceneSequenceActive = false;
    sceneSequenceCurrentStepIndex = -1;
    sceneChainState = {};
    activeSceneMainPresetIndex = 0;
    activeSceneSlot = 0;
    activeSceneNeedsCaptureBeforeManualRecall = true;
    activeSceneStartPpqValid = false;
    activeSceneStartPpq = 0.0;
    sceneSequenceStartPpqValid = false;
    sceneSequenceStartPpq = 0.0;
    scenePadHeld.fill(false);
    scenePadHoldDeleteTriggered.fill(false);
    scenePadLaunchConsumed.fill(false);
    scenePadPressStartMs.fill(0);
    scenePadActionBurstUntilMs.fill(0);
    scenePadLastTapMs.fill(0);
    monomePatternPadPendingUntilMs.fill(0);
    monomePatternPadPendingAction.fill(MonomePatternTapAction::None);
    clearPendingMonomeSceneRecorderTap();
    clearPendingSceneRecorderAction();
    pendingSceneTriggerRecords.fill({});
    scenePerformanceRecorder.clearAll();
    lastScenePerformanceProcessBeat = std::numeric_limits<double>::quiet_NaN();
    lastScenePerformanceProcessSceneSlot = -1;
    lastScenePerformanceProcessSceneStartBeat = std::numeric_limits<double>::quiet_NaN();
    sceneCopySourceSlot = -1;
    sceneCopyMainPresetIndex = 0;
    scenePerformanceClipboardData.reset();

    {
        const juce::ScopedLock lock(pendingLoopChangeLock);
        for (auto& pending : pendingLoopChanges)
            pending = PendingLoopChange{};
    }
    {
        const juce::ScopedLock lock(pendingBarChangeLock);
        for (auto& pending : pendingBarChanges)
            pending = PendingBarChange{};
    }
    pendingBarLengthApply.fill(false);
    momentaryScratchHoldActive = false;
    momentaryStutterHoldActive = false;
    momentaryStutterActiveDivisionButton = -1;
    momentaryStutterButtonMask.store(0, std::memory_order_release);
    momentaryStutterMacroBaselineCaptured = false;
    momentaryStutterMacroCapturePending = false;
    momentaryStutterMacroStartPpq = 0.0;
    momentaryStutterRecordedDivisionButton = -1;
    momentaryStutterLastComboMask = 0;
    momentaryStutterTwoButtonStepBaseValid = false;
    momentaryStutterTwoButtonStepBase = 0;
    momentaryStutterStripArmed.fill(false);
    momentaryStutterPlaybackActive.store(0, std::memory_order_release);
    sceneGlobalStutterBaseAmount.store(0.0f, std::memory_order_release);
    clearSceneBoundaryTransitionState();
    clearSceneChainReturnOverride();
    pendingStutterStartActive.store(0, std::memory_order_release);
    pendingStutterStartPpq.store(-1.0, std::memory_order_release);
    pendingStutterStartDivisionBeats.store(1.0, std::memory_order_release);
    pendingStutterStartQuantizeDivision.store(8, std::memory_order_release);
    pendingStutterStartSampleTarget.store(-1, std::memory_order_release);
    for (auto& saved : momentaryStutterSavedState)
        saved = MomentaryStutterSavedStripState{};
    pendingStutterReleaseActive.store(0, std::memory_order_release);
    pendingStutterReleasePpq.store(-1.0, std::memory_order_release);
    pendingStutterReleaseSampleTarget.store(-1, std::memory_order_release);
    audioEngine->setMomentaryStutterActive(false);
    audioEngine->setMomentaryStutterStartPpq(-1.0);
    audioEngine->setMomentaryStutterReleasePpq(-1.0);
    audioEngine->setMomentaryStutterRetriggerFadeMs(0.7f);
    audioEngine->clearMomentaryStutterStrips();
    audioEngine->clearRecentInputBuffer();
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
    globalRootNoteMidi.store(60, std::memory_order_release);
    globalPitchScale.store(static_cast<int>(ModernAudioEngine::PitchScale::Chromatic), std::memory_order_release);

    for (int i = 0; i < MaxStrips; ++i)
    {
        currentStripFiles[static_cast<size_t>(i)] = juce::File();
        pendingLoopStripFiles[static_cast<size_t>(i)] = juce::File();

        if (auto* sampleEngine = getSampleModeEngine(i, false))
        {
            sampleEngine->clearPendingVisibleSlice();
            sampleEngine->stop();
            sampleEngine->clear();
        }
        invalidateFlipLegacyLoopSync(i);

        if (auto* strip = audioEngine->getStrip(i))
        {
            strip->clearSample();
            strip->stop(true);
            strip->setLoop(0, MaxColumns);
            strip->setPlayMode(EnhancedAudioStrip::PlayMode::Loop);
            strip->setDirectionMode(EnhancedAudioStrip::DirectionMode::Normal);
            strip->setReverse(false);
            setStripVolumeControlValue(i, 1.0f, StripControlWriteMode::CacheOnly);
            strip->setTrimDb(0.0f);
            setStripPanControlValue(i, 0.0f, StripControlWriteMode::CacheOnly);
            setStripSpeedControlValue(i, 1.0f, StripControlWriteMode::CacheOnly);
            strip->setBeatsPerLoop(-1.0f);
            strip->setScratchAmount(0.0f);
            strip->setTransientSliceMode(false);
            strip->setLoopSliceLength(1.0f);
            strip->setResamplePitchEnabled(false);
            strip->setResamplePitchRatio(1.0f);
            strip->setPitchShift(0.0f);
            strip->setRecordingBars(2);
            setStripFilterFrequencyControlValue(i, 20000.0f, StripControlWriteMode::CacheOnly);
            setStripFilterResonanceControlValue(i, 0.707f, StripControlWriteMode::CacheOnly);
            setStripFilterMorphControlValue(i, 0.0f, StripControlWriteMode::CacheOnly);
            setStripFilterAlgorithmControlValue(i, EnhancedAudioStrip::FilterAlgorithm::Tpt12, StripControlWriteMode::CacheOnly);
            setStripFilterEnabledControlValue(i, false, StripControlWriteMode::CacheOnly);
            strip->setSwingAmount(0.0f);
            strip->setGateAmount(0.0f);
            strip->setGateSpeed(4.0f);
            strip->setGateEnvelope(0.5f);
            strip->setGateShape(0.5f);
            strip->setStepPatternBars(1);
            strip->setStepPage(0);
            strip->currentStep = 0;
            strip->stepPattern.fill(false);
            strip->stepSubdivisionStartVelocity.fill(1.0f);
            strip->stepSubdivisions.fill(1);
            strip->stepSubdivisionRepeatVelocity.fill(1.0f);
            strip->stepProbability.fill(1.0f);
            strip->setStepEnvelopeAttackMs(0.0f);
            strip->setStepEnvelopeDecayMs(4000.0f);
            strip->setStepEnvelopeReleaseMs(110.0f);
            strip->setGrainSizeMs(1240.0f);
            strip->setGrainDensity(0.05f);
            strip->setGrainPitch(0.0f);
            strip->setGrainPitchJitter(0.0f);
            strip->setGrainSpread(0.0f);
            strip->setGrainJitter(0.0f);
            strip->setGrainPositionJitter(0.0f);
            strip->setGrainRandomDepth(0.0f);
            strip->setGrainArpDepth(0.0f);
            strip->setGrainCloudDepth(0.0f);
            strip->setGrainEmitterDepth(0.0f);
            strip->setGrainEnvelope(0.0f);
            strip->setGrainShape(0.0f);
            strip->setGrainArpMode(0);
            strip->setGrainTempoSyncEnabled(true);
        }

        audioEngine->assignStripToGroup(i, -1);
        for (int slot = 0; slot < ModernAudioEngine::NumModSequencers; ++slot)
        {
            audioEngine->resetModSequencerSlotToDefaults(i, slot);
        }
        audioEngine->setModSequencerSlot(i, 0);

        if (auto* param = parameters.getParameter("stripVolume" + juce::String(i)))
            param->setValueNotifyingHost(param->getDefaultValue());
        if (auto* param = parameters.getParameter("stripTrimDb" + juce::String(i)))
            param->setValueNotifyingHost(param->getDefaultValue());
        if (auto* param = parameters.getParameter("stripPan" + juce::String(i)))
            param->setValueNotifyingHost(param->getDefaultValue());
        if (auto* param = parameters.getParameter("stripSpeed" + juce::String(i)))
            param->setValueNotifyingHost(param->getDefaultValue());
        if (auto* param = parameters.getParameter("stripPitch" + juce::String(i)))
            param->setValueNotifyingHost(param->getDefaultValue());
        if (auto* param = parameters.getParameter("stripSliceLength" + juce::String(i)))
            param->setValueNotifyingHost(param->getDefaultValue());
        if (auto* param = parameters.getParameter("stripPitchControlMode" + juce::String(i)))
            param->setValueNotifyingHost(param->getDefaultValue());
        if (auto* param = parameters.getParameter("stripTempoMatchMode" + juce::String(i)))
            param->setValueNotifyingHost(param->getDefaultValue());
        if (auto* param = parameters.getParameter("stripFilterEnabled" + juce::String(i)))
            param->setValueNotifyingHost(param->getDefaultValue());
        if (auto* param = parameters.getParameter("stripFilterFrequency" + juce::String(i)))
            param->setValueNotifyingHost(param->getDefaultValue());
        if (auto* param = parameters.getParameter("stripFilterResonance" + juce::String(i)))
            param->setValueNotifyingHost(param->getDefaultValue());
        if (auto* param = parameters.getParameter("stripFilterMorph" + juce::String(i)))
            param->setValueNotifyingHost(param->getDefaultValue());
        if (auto* param = parameters.getParameter("stripFilterAlgorithm" + juce::String(i)))
            param->setValueNotifyingHost(param->getDefaultValue());
        if (auto* param = parameters.getParameter("stripDelayMix" + juce::String(i)))
            param->setValueNotifyingHost(param->getDefaultValue());
        if (auto* param = parameters.getParameter("stripDelayTime" + juce::String(i)))
            param->setValueNotifyingHost(param->getDefaultValue());
        if (auto* param = parameters.getParameter("stripDelayFeedback" + juce::String(i)))
            param->setValueNotifyingHost(param->getDefaultValue());
        if (auto* param = parameters.getParameter("stripDelayLowCut" + juce::String(i)))
            param->setValueNotifyingHost(param->getDefaultValue());
        if (auto* param = parameters.getParameter("stripDelayHighCut" + juce::String(i)))
            param->setValueNotifyingHost(param->getDefaultValue());
        if (auto* param = parameters.getParameter("stripDelayMode" + juce::String(i)))
            param->setValueNotifyingHost(param->getDefaultValue());
    }

    for (int i = 0; i < ModernAudioEngine::MaxGroups; ++i)
    {
        if (auto* group = audioEngine->getGroup(i))
        {
            group->setVolume(1.0f);
            group->setMuted(false);
        }
    }

    for (int i = 0; i < ModernAudioEngine::MaxPatterns; ++i)
        audioEngine->clearPattern(i);
}

void MlrVSTAudioProcessor::initRuntimeStateToDefaults()
{
    struct ScopedSuspendProcessing
    {
        explicit ScopedSuspendProcessing(MlrVSTAudioProcessor& p) : processor(p) { processor.suspendProcessing(true); }
        ~ScopedSuspendProcessing() { processor.suspendProcessing(false); }
        MlrVSTAudioProcessor& processor;
    } scopedSuspend(*this);

    for (auto* parameter : getParameters())
    {
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*>(parameter))
            ranged->setValueNotifyingHost(ranged->getDefaultValue());
    }

    resetRuntimePresetStateToDefaults();
    sceneRepeatCounts.fill(1);
    sceneLengthModes.fill(static_cast<int>(SceneLengthMode::ManualBars));
    sceneManualBars.fill(4);
    sceneAnchorStrips.fill(0);
    sceneModeGroupSnapshot.valid = false;
    resetCurrentBrowserDirectoriesToDefaultPaths(true);
    loadedPresetIndex = -1;
    updateMonomeLEDs();
    presetRefreshToken.fetch_add(1, std::memory_order_acq_rel);
}

bool MlrVSTAudioProcessor::isHostTransportPlaying() const
{
    juce::AudioPlayHead::PositionInfo positionInfo;
    if (getCurrentHostPositionInfo(positionInfo))
        return positionInfo.getIsPlaying();

    return lastHostTransportPlaying.load(std::memory_order_acquire) != 0;
}

bool MlrVSTAudioProcessor::getCurrentHostPositionInfo(juce::AudioPlayHead::PositionInfo& outPosition) const
{
    if (auto* hostPlayHead = getPlayHead())
    {
        if (auto position = hostPlayHead->getPosition())
        {
            outPosition = *position;
            return true;
        }
    }

    return false;
}

bool MlrVSTAudioProcessor::getCurrentHostPpq(double& outPpq) const
{
    juce::AudioPlayHead::PositionInfo positionInfo;
    if (getCurrentHostPositionInfo(positionInfo) && positionInfo.getPpqPosition().hasValue())
    {
        outPpq = *positionInfo.getPpqPosition();
        return true;
    }

    return false;
}

bool MlrVSTAudioProcessor::getHostSyncSnapshot(double& outPpq, double& outTempo) const
{
    const bool hostTransportPlaying = isHostTransportPlaying();
    juce::AudioPlayHead::PositionInfo positionInfo;
    if (getCurrentHostPositionInfo(positionInfo))
    {
        if (positionInfo.getIsPlaying()
            && positionInfo.getPpqPosition().hasValue()
            && positionInfo.getBpm().hasValue()
            && std::isfinite(*positionInfo.getPpqPosition())
            && std::isfinite(*positionInfo.getBpm())
            && *positionInfo.getBpm() > 0.0)
        {
            outPpq = *positionInfo.getPpqPosition();
            outTempo = *positionInfo.getBpm();
            return true;
        }
    }

    if (audioEngine != nullptr)
    {
        const double fallbackPpq = audioEngine->getTimelineBeat();
        const double fallbackTempo = audioEngine->getCurrentTempo();
        if (hostTransportPlaying
            && std::isfinite(fallbackPpq)
            && std::isfinite(fallbackTempo)
            && fallbackTempo > 0.0)
        {
            outPpq = fallbackPpq;
            outTempo = fallbackTempo;
            return true;
        }
    }

    return false;
}

void MlrVSTAudioProcessor::refreshUtilityTimerCadence()
{
    const bool queuedSceneApplyActive = pendingSceneApplySlot.load(std::memory_order_acquire) >= 0;
    const bool queuedSequenceDriven = pendingSceneApplySequenceDriven.load(std::memory_order_acquire) != 0;
    const bool needsFastScenePolling = pendingSceneRecall.active
        || pendingSceneRecorderAction.active
        || pendingSceneRecorderApplyAction.load(std::memory_order_acquire) != static_cast<int>(SceneRecorderAction::None)
        || (queuedSceneApplyActive && (!queuedSequenceDriven || isHostTransportPlaying()));
    const int desiredIntervalMs = needsFastScenePolling
        ? kSceneRecallRefreshMs
        : (monomeConnection.supportsArc() ? kArcRefreshMs : kGridRefreshMs);

    if (!isTimerRunning() || getTimerInterval() != desiredIntervalMs)
        startTimer(desiredIntervalMs);
}

void MlrVSTAudioProcessor::performPresetLoad(int presetIndex, double hostPpqSnapshot, double hostTempoSnapshot)
{
    struct ScopedSuspendProcessing
    {
        explicit ScopedSuspendProcessing(MlrVSTAudioProcessor& p) : processor(p) { processor.suspendProcessing(true); }
        ~ScopedSuspendProcessing() { processor.suspendProcessing(false); }
        MlrVSTAudioProcessor& processor;
    } scopedSuspend(*this);

    // Always reset to a known clean runtime state before applying preset data.
    // This guarantees no strip audio/params leak across preset transitions.
    resetRuntimePresetStateToDefaults();
    sceneRepeatCounts.fill(1);
    sceneLengthModes.fill(static_cast<int>(SceneLengthMode::ManualBars));
    sceneManualBars.fill(4);
    sceneAnchorStrips.fill(0);
    loadedPresetIndex = -1;
    activeSceneMainPresetIndex = juce::jlimit(0, MaxPresetSlots - 1, presetIndex);
    activeSceneStartPpqValid = std::isfinite(hostPpqSnapshot);
    activeSceneStartPpq = activeSceneStartPpqValid ? hostPpqSnapshot : 0.0;

    if (!PresetStore::presetExists(presetIndex))
    {
        // Empty slot recall keeps the freshly reset runtime defaults and does
        // not create or mutate preset files.
        resetCurrentBrowserDirectoriesToDefaultPaths(true);
        presetRefreshToken.fetch_add(1, std::memory_order_acq_rel);
        return;
    }

    // Clear stale file references; preset load repopulates file-backed strips.
    for (auto& f : currentStripFiles)
        f = juce::File();

    bool loadSucceeded = false;
    {
        ScopedSceneAutosaveSuppression suppressSceneAutosave(*this);
        loadSucceeded = PresetStore::loadPreset(
            presetIndex,
            MaxStrips,
            audioEngine.get(),
            parameters,
            [this](int stripIndex, const juce::File& sampleFile)
            {
                return loadSampleToStrip(stripIndex, sampleFile);
            },
            [this](int stripIndex, const juce::File& sampleFile)
            {
                rememberLoadedSamplePathForStrip(stripIndex, sampleFile, false);
            },
            [this](int stripIndex,
                   const juce::File& loopDir,
                   const juce::File& stepDir,
                   const juce::File& flipDir)
            {
                setRecentSampleDirectory(stripIndex, SamplePathMode::Loop, loopDir, false);
                setRecentSampleDirectory(stripIndex, SamplePathMode::Step, stepDir, false);
                setRecentSampleDirectory(stripIndex, SamplePathMode::Flip, flipDir, false);
            },
            [this](int stripIndex, const juce::XmlElement* flipStateXml)
            {
                applyFlipPresetStateXml(stripIndex, flipStateXml);
            },
            [this](int stripIndex, const juce::XmlElement* loopPitchStateXml)
            {
                applyLoopPitchPresetStateXml(stripIndex, loopPitchStateXml);
            },
            [this](const juce::XmlElement& presetXml)
            {
                applySceneChainStateXml(presetXml.getChildByName("SceneChainState"), -1);
            },
            [this](const juce::MemoryBlock& scenePerformanceData)
            {
                applyScenePerformanceStateData(scenePerformanceData, -1);
            },
            hostPpqSnapshot,
            hostTempoSnapshot,
            true,
            -1);
    }

    savePersistentDefaultPaths();

    if (loadSucceeded)
    {
        normalizeLoopPitchMasterRoles();
        applyLoopPitchSyncToAllStrips();
        lastScenePerformanceProcessBeat = std::numeric_limits<double>::quiet_NaN();
        lastScenePerformanceProcessSceneSlot = -1;
        lastScenePerformanceProcessSceneStartBeat = std::numeric_limits<double>::quiet_NaN();
        if (isSceneModeEnabled())
            clearAllStripGroupsForSceneMode();
        activeSceneNeedsCaptureBeforeManualRecall = true;
    }

    if (loadSucceeded && PresetStore::presetExists(presetIndex))
        loadedPresetIndex = presetIndex;
    presetRefreshToken.fetch_add(1, std::memory_order_acq_rel);
}

bool MlrVSTAudioProcessor::runPresetSaveRequest(const PresetSaveRequest& request)
{
    if (!audioEngine || request.presetIndex < 0 || request.presetIndex >= MaxPresetSlots)
        return false;

    try
    {
        struct ScopedSuspendProcessing
        {
            explicit ScopedSuspendProcessing(MlrVSTAudioProcessor& p) : processor(p) { processor.suspendProcessing(true); }
            ~ScopedSuspendProcessing() { processor.suspendProcessing(false); }
            MlrVSTAudioProcessor& processor;
        } scopedSuspend(*this);

        if (isSceneModeEnabled() && audioEngine != nullptr)
            syncSceneMotionStateFromEngine(activeSceneSlot);
        syncAllScenePerformanceClipLengthsToResolvedLengths();

        return PresetStore::savePreset(request.presetIndex,
                                       MaxStrips,
                                       audioEngine.get(),
                                       parameters,
                                       request.stripFiles.data(),
                                       request.recentLoopDirectories.data(),
                                       request.recentStepDirectories.data(),
                                       request.recentFlipDirectories.data(),
                                       [this](int stripIndex)
                                       {
                                           return createFlipPresetStateXml(stripIndex);
                                       },
                                       [this](int stripIndex)
                                       {
                                           return createLoopPitchPresetStateXml(stripIndex);
                                       },
                                       [this]()
                                       {
                                           return createSceneChainStateXml(-1);
                                       },
                                       [this]()
                                       {
                                           return createScenePerformanceStateData(-1);
                                       });
    }
    catch (const std::exception& e)
    {
        DBG("async savePreset exception for slot " << request.presetIndex << ": " << e.what());
        return false;
    }
    catch (...)
    {
        DBG("async savePreset exception for slot " << request.presetIndex << ": unknown");
        return false;
    }
}

void MlrVSTAudioProcessor::pushPresetSaveResult(const PresetSaveResult& result)
{
    {
        const juce::ScopedLock lock(presetSaveResultLock);
        presetSaveResults.push_back(result);
    }
    presetSaveJobsInFlight.fetch_sub(1, std::memory_order_acq_rel);
}

void MlrVSTAudioProcessor::applyCompletedPresetSaves()
{
    std::vector<PresetSaveResult> completed;
    {
        const juce::ScopedLock lock(presetSaveResultLock);
        if (presetSaveResults.empty())
            return;
        completed.swap(presetSaveResults);
    }

    uint32_t successfulSaves = 0;
    for (const auto& result : completed)
    {
        if (!result.success)
        {
            DBG("Preset save failed for slot " << result.presetIndex);
            continue;
        }

        loadedPresetIndex = result.presetIndex;
        ++successfulSaves;
    }

    if (successfulSaves > 0)
        presetRefreshToken.fetch_add(successfulSaves, std::memory_order_acq_rel);
}

void MlrVSTAudioProcessor::savePreset(int presetIndex)
{
    if (!audioEngine || presetIndex < 0 || presetIndex >= MaxPresetSlots)
        return;

    activeSceneMainPresetIndex = juce::jlimit(0, MaxPresetSlots - 1, presetIndex);

    if (isSceneModeEnabled())
        captureSceneSlot(activeSceneSlot);

    if (!isTimerRunning())
        startTimer(kGridRefreshMs);

    PresetSaveRequest request;
    request.presetIndex = presetIndex;
    for (int i = 0; i < MaxStrips; ++i)
    {
        request.stripFiles[static_cast<size_t>(i)] = currentStripFiles[static_cast<size_t>(i)];
        request.recentLoopDirectories[static_cast<size_t>(i)] = recentLoopDirectories[static_cast<size_t>(i)];
        request.recentStepDirectories[static_cast<size_t>(i)] = recentStepDirectories[static_cast<size_t>(i)];
        request.recentFlipDirectories[static_cast<size_t>(i)] = recentFlipDirectories[static_cast<size_t>(i)];
    }

    auto* job = new PresetSaveJob(*this, std::move(request));
    presetSaveJobsInFlight.fetch_add(1, std::memory_order_acq_rel);
    presetSaveThreadPool.addJob(job, true);

    // Keep UI/LED state responsive immediately; completion still updates token.
    loadedPresetIndex = presetIndex;
    presetRefreshToken.fetch_add(1, std::memory_order_acq_rel);
}

void MlrVSTAudioProcessor::loadPreset(int presetIndex)
{
    try
    {
        activeSceneMainPresetIndex = juce::jlimit(0, MaxPresetSlots - 1, presetIndex);
        double hostPpqSnapshot = std::numeric_limits<double>::quiet_NaN();
        double hostTempoSnapshot = std::numeric_limits<double>::quiet_NaN();
        const bool hasHostSync = getHostSyncSnapshot(hostPpqSnapshot, hostTempoSnapshot);
        if (!hasHostSync)
        {
            DBG("Preset " << (presetIndex + 1)
                << " loaded without host PPQ/BPM snapshot; recalling audio/parameters only.");
        }

        pendingPresetLoadIndex.store(-1, std::memory_order_release);
        performPresetLoad(presetIndex, hostPpqSnapshot, hostTempoSnapshot);
    }
    catch (const std::exception& e)
    {
        DBG("loadPreset exception for slot " << presetIndex << ": " << e.what());
    }
    catch (...)
    {
        DBG("loadPreset exception for slot " << presetIndex << ": unknown");
    }
}

bool MlrVSTAudioProcessor::deletePreset(int presetIndex)
{
    try
    {
        const bool deleted = PresetStore::deletePreset(presetIndex);
        if (deleted)
        {
            struct ScopedSuspendProcessing
            {
                explicit ScopedSuspendProcessing(MlrVSTAudioProcessor& p) : processor(p) { processor.suspendProcessing(true); }
                ~ScopedSuspendProcessing() { processor.suspendProcessing(false); }
                MlrVSTAudioProcessor& processor;
            } scopedSuspend(*this);

            // Deleting any preset slot should leave runtime in a clean state.
            resetRuntimePresetStateToDefaults();
            resetCurrentBrowserDirectoriesToDefaultPaths(true);
            loadedPresetIndex = -1;
            updateMonomeLEDs();
        }
        if (deleted)
            presetRefreshToken.fetch_add(1, std::memory_order_acq_rel);
        return deleted;
    }
    catch (...)
    {
        return false;
    }
}

juce::String MlrVSTAudioProcessor::getPresetName(int presetIndex) const
{
    return PresetStore::getPresetName(presetIndex);
}

bool MlrVSTAudioProcessor::setPresetName(int presetIndex, const juce::String& name)
{
    try
    {
        const bool ok = PresetStore::setPresetName(presetIndex, name);
        if (ok)
            presetRefreshToken.fetch_add(1, std::memory_order_acq_rel);
        return ok;
    }
    catch (...)
    {
        return false;
    }
}

bool MlrVSTAudioProcessor::presetExists(int presetIndex) const
{
    try
    {
        return PresetStore::presetExists(presetIndex);
    }
    catch (...)
    {
        return false;
    }
}

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
