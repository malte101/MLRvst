/*
  ===============================================================================

    PluginProcessorSampleMode.cpp
    Sample/flip mode state, loading, and render implementation split from PluginProcessor.cpp

  ================================================================================
*/

#include "PluginProcessor.h"
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
        case 1:   return { 1, 4.0f };
        case 2:   return { 2, 8.0f };
        case 4:   return { 4, 16.0f };
        case 8:   return { 8, 32.0f };
        default:  return { 2, 8.0f };
    }
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

} // namespace

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
                if (loaded && isSceneModeEnabled())
                    juce::ignoreUnused(ensureActiveScenePlaybackHandleInitialized());
                return loaded;
            }
        }

        if (audioEngine != nullptr)
            audioEngine->setGlobalStretchBackend(getStretchBackend());

        const bool loaded = audioEngine->loadSampleToStrip(stripIndex, file);
        if (loaded)
        {
            juce::ignoreUnused(ensureSceneSlotFallbackState(getActiveMainPresetIndexForScenes(), getActiveSceneSlot()));
            if (isSceneModeEnabled())
                juce::ignoreUnused(ensureActiveScenePlaybackHandleInitialized());
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
            juce::ignoreUnused(ensureSceneSlotFallbackState(getActiveMainPresetIndexForScenes(), getActiveSceneSlot()));
            if (isSceneModeEnabled())
                juce::ignoreUnused(ensureActiveScenePlaybackHandleInitialized());
            queueActiveSceneAutosave();
            return true;
        }
    }

    return false;
}

bool MlrVSTAudioProcessor::ensureSampleModeAudioAvailableForStrip(int stripIndex)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips || audioEngine == nullptr)
        return false;

    auto* strip = audioEngine->getStrip(stripIndex);
    if (strip == nullptr || strip->getPlayMode() != EnhancedAudioStrip::PlayMode::Sample)
        return false;

    if (hasSampleModeAudio(stripIndex))
        return true;

    const auto* sourceBuffer = strip->getAudioBuffer();
    const double sourceSampleRate = strip->getSourceSampleRate();
    if (sourceBuffer == nullptr
        || sourceBuffer->getNumSamples() <= 0
        || !std::isfinite(sourceSampleRate)
        || sourceSampleRate <= 1000.0)
    {
        return false;
    }

    auto* engine = getSampleModeEngine(stripIndex, true);
    if (engine == nullptr)
        return false;

    const auto preservedState = engine->capturePersistentState();
    const auto sourceFile = currentStripFiles[static_cast<size_t>(stripIndex)];
    const bool hasSourceFile = safeFileExistsAsFile(sourceFile);
    const juce::String sourcePath = hasSourceFile ? sourceFile.getFullPathName() : juce::String();
    const juce::String displayName = hasSourceFile
        ? sourceFile.getFileNameWithoutExtension()
        : ("Flip Strip " + juce::String(stripIndex + 1));

    if (!engine->loadSampleFromBuffer(*sourceBuffer, sourceSampleRate, sourcePath, displayName))
        return false;

    engine->applyPersistentState(preservedState);
    if (hasSourceFile)
        rememberLoadedSamplePathForStripMode(stripIndex, sourceFile, SamplePathMode::Flip, false);

    return hasSampleModeAudio(stripIndex);
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

bool MlrVSTAudioProcessor::hasSampleModeAudio(int stripIndex) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return false;

    auto& engine = sampleModeEngines[static_cast<size_t>(stripIndex)];
    return engine != nullptr && engine->hasSample();
}

bool MlrVSTAudioProcessor::isStripScenePlaybackAvailable(int stripIndex) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return false;
    if (audioEngine == nullptr)
        return true;

    auto* strip = audioEngine->getStrip(stripIndex);
    return strip != nullptr;
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
    auto* renderEngine = audioEngine.get();
    auto* strip = renderEngine != nullptr ? renderEngine->getStrip(stripIndex) : nullptr;
    if (strip == nullptr || strip->getPlayMode() != EnhancedAudioStrip::PlayMode::Sample)
        return;

    if (!ensureSampleModeAudioAvailableForStrip(stripIndex))
        return;

    auto* engine = getSampleModeEngine(stripIndex, false);
    if (engine == nullptr)
        return;

    if (!positionInfo.getIsPlaying())
    {
        auto& renderedLastBlock = sampleModeRenderedLastBlock[static_cast<size_t>(stripIndex)];
        renderedLastBlock = false;
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
    auto* renderEngine = audioEngine.get();
    auto* strip = renderEngine != nullptr ? renderEngine->getStrip(stripIndex) : nullptr;
    if (strip == nullptr || strip->getPlayMode() != EnhancedAudioStrip::PlayMode::Sample)
        return;

    if (!ensureSampleModeAudioAvailableForStrip(stripIndex))
        return;

    auto* engine = getSampleModeEngine(stripIndex, false);
    if (engine == nullptr)
        return;

    const int visibleSlot = juce::jlimit(0, SliceModel::VisibleSliceCount - 1, column);
    if (engine->isLegacyLoopEngineEnabled())
    {
        SampleModeEngine::LegacyLoopSyncInfo syncInfo;
        const auto stretchBackend = getFlipTempoMatchBackend();
        const bool needsLegacyLoopPrime = !strip->isSampleModeLegacyLoopEngineEnabled();
        const double hostTempo = renderEngine != nullptr ? renderEngine->getCurrentTempo() : 120.0;
        SampleModeEngine::LegacyLoopSyncInfo currentSyncInfo;
        const bool hasCurrentSyncInfo = engine->getLegacyLoopSyncInfo(currentSyncInfo);
        const bool hasExplicitLegacyWindow = hasCurrentSyncInfo
            && currentSyncInfo.bankEndSample > currentSyncInfo.bankStartSample;
        const bool usesAutoLegacyLoopOverride = hasCurrentSyncInfo
            && currentSyncInfo.legacyLoopBarSelection <= 0
            && currentSyncInfo.visibleBankIndex < 0
            && currentSyncInfo.bankEndSample <= currentSyncInfo.bankStartSample;
        const bool hasExplicitSliceReference = sampleSliceId >= 0 || sampleStartSample >= 0;
        const bool resolveFromCurrentVisibleSlot = !hasExplicitSliceReference
            && (hasExplicitLegacyWindow || usesAutoLegacyLoopOverride);
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
        if (entry.syncInfo != nullptr
            && entry.engineToken == &engine
            && entry.version == currentVersion)
        {
            return entry.syncInfo;
        }
    }

    SampleModeEngine::LegacyLoopSyncInfo rebuiltSyncInfo;
    if (!engine.getLegacyLoopSyncInfo(rebuiltSyncInfo))
        return {};

    auto rebuiltPtr = std::make_shared<SampleModeEngine::LegacyLoopSyncInfo>(std::move(rebuiltSyncInfo));
    {
        const juce::SpinLock::ScopedLockType lock(flipLegacyLoopSyncInfoCacheLock);
        auto& entry = flipLegacyLoopSyncInfoCache[static_cast<size_t>(stripIndex)];
        entry.engineToken = &engine;
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
    handleUserStripPlayModeChange(targetStripIndex);

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

void MlrVSTAudioProcessor::stopSampleModeStrip(int stripIndex, bool immediateStop, bool bumpTriggerGeneration)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;

    auto* renderEngine = audioEngine.get();
    sampleModeRenderedLastBlock[static_cast<size_t>(stripIndex)] = false;

    auto* strip = renderEngine != nullptr ? renderEngine->getStrip(stripIndex) : nullptr;
    bool usedLegacyLoopEngine = false;
    auto* engine = getSampleModeEngine(stripIndex, false);
    if (engine != nullptr)
        usedLegacyLoopEngine = engine->isLegacyLoopEngineEnabled();
    if (strip != nullptr)
    {
        usedLegacyLoopEngine = usedLegacyLoopEngine || strip->isSampleModeLegacyLoopEngineEnabled();
        strip->setSampleModeLegacyLoopEngineEnabled(false);
        if (usedLegacyLoopEngine)
            strip->stop(immediateStop);
    }

    if (renderEngine != nullptr)
    {
        renderEngine->clearPendingQuantizedTriggersForStrip(stripIndex, bumpTriggerGeneration);
    }

    if (engine != nullptr)
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
