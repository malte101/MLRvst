/*
  ==============================================================================

    PluginProcessorPreparedScene.cpp
    Extracted prepared-scene payload and snapshot logic for mlrVST.

  ==============================================================================
*/

#include "PluginProcessor.h"
#include "PlayheadSpeedQuantizer.h"
#include "PresetStore.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
constexpr const char* kEmbeddedFlipSampleAttr = "embeddedSampleWavBase64";
constexpr const char* kPreparedSceneAnalysisTransientAttr = "analysisTransientSlices";
constexpr const char* kPreparedSceneAnalysisRmsAttr = "analysisRmsMap";
constexpr const char* kPreparedSceneAnalysisZeroCrossAttr = "analysisZeroCrossMap";
constexpr const char* kPreparedSceneAnalysisSampleCountAttr = "analysisSampleCount";
constexpr size_t kMaxSceneFileAudioCacheEntries = 24;
constexpr int64_t kMaxSceneFileAudioCacheEntryBytes = 32LL * 1024LL * 1024LL;

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

int64_t estimateAudioBufferStorageBytes(const juce::AudioBuffer<float>& buffer) noexcept
{
    return static_cast<int64_t>(juce::jmax(0, buffer.getNumChannels()))
        * static_cast<int64_t>(juce::jmax(0, buffer.getNumSamples()))
        * static_cast<int64_t>(sizeof(float));
}

float clampPreparedFloat(double value, float fallback, float minValue, float maxValue) noexcept
{
    return juce::jlimit(minValue,
                        maxValue,
                        std::isfinite(value) ? static_cast<float>(value) : fallback);
}

int clampPreparedInt(int value, int minValue, int maxValue, int fallback) noexcept
{
    if (value < minValue || value > maxValue)
        return fallback;
    return value;
}

bool isValidPreparedStoredSamplePath(const juce::String& rawPath)
{
    constexpr int kMaxStoredSamplePathChars = 4096;

    const auto path = rawPath.trim();
    if (path.isEmpty() || path.length() > kMaxStoredSamplePathChars)
        return false;

    if (!juce::File::isAbsolutePath(path))
        return false;

    if (path.contains("\n") || path.contains("\r") || path.contains("://"))
        return false;

    if (path.startsWith("//") || path.startsWith("\\\\"))
        return false;

    const juce::File file(path);
    return file.hasFileExtension("wav;aif;aiff;mp3;ogg;flac");
}

juce::File restorePreparedStoredDirectory(const juce::String& rawPath)
{
    const auto trimmedPath = rawPath.trim();
    if (trimmedPath.isEmpty() || !juce::File::isAbsolutePath(trimmedPath))
        return {};
    return juce::File(trimmedPath);
}

template <size_t N>
void decodePreparedIntArrayCsv(const juce::String& csvText, std::array<int, N>& outValues)
{
    juce::StringArray tokens;
    tokens.addTokens(csvText, ",", "");
    tokens.trim();
    tokens.removeEmptyStrings();

    const size_t limit = juce::jmin(N, static_cast<size_t>(tokens.size()));
    for (size_t i = 0; i < limit; ++i)
        outValues[i] = tokens[static_cast<int>(i)].getIntValue();
}

template <size_t N>
void decodePreparedFloatArrayCsv(const juce::String& csvText, std::array<float, N>& outValues)
{
    juce::StringArray tokens;
    tokens.addTokens(csvText, ",", "");
    tokens.trim();
    tokens.removeEmptyStrings();

    const size_t limit = juce::jmin(N, static_cast<size_t>(tokens.size()));
    for (size_t i = 0; i < limit; ++i)
        outValues[i] = static_cast<float>(tokens[static_cast<int>(i)].getDoubleValue());
}

template <size_t N>
juce::String encodePreparedIntArrayCsv(const std::array<int, N>& values)
{
    juce::String out;
    out.preallocateBytes(static_cast<int>(N * 8));
    for (size_t i = 0; i < N; ++i)
    {
        if (i > 0)
            out << ",";
        out << values[i];
    }
    return out;
}

template <size_t N>
juce::String encodePreparedFloatArrayCsv(const std::array<float, N>& values, int precision = 6)
{
    juce::String out;
    out.preallocateBytes(static_cast<int>(N * 8));
    for (size_t i = 0; i < N; ++i)
    {
        if (i > 0)
            out << ",";
        out << juce::String(values[i], precision);
    }
    return out;
}

juce::String encodePreparedStepPatternBits(const std::array<bool, 64>& bits)
{
    juce::String out;
    out.preallocateBytes(64);
    for (bool bit : bits)
        out += (bit ? "1" : "0");
    return out;
}

juce::String encodePreparedStepSubdivisions(const std::array<int, 64>& subdivisions)
{
    juce::String out;
    out.preallocateBytes(64 * 3);
    for (size_t i = 0; i < subdivisions.size(); ++i)
    {
        if (i > 0)
            out << ",";
        out << juce::jlimit(1, EnhancedAudioStrip::MaxStepSubdivisions, subdivisions[i]);
    }
    return out;
}

juce::String encodePreparedStepSubdivisionRepeatVelocity(const std::array<float, 64>& values)
{
    return encodePreparedFloatArrayCsv(values, 5);
}

juce::String encodePreparedModSteps(const std::array<float, ModernAudioEngine::ModTotalSteps>& steps)
{
    return encodePreparedFloatArrayCsv(steps, 6);
}

void decodePreparedStepPatternBits(const juce::String& text, std::array<bool, 64>& bits)
{
    bits.fill(false);
    const int length = juce::jmin(64, text.length());
    for (int i = 0; i < length; ++i)
        bits[static_cast<size_t>(i)] = (text[i] == '1');
}

void decodePreparedStepSubdivisions(const juce::String& text, std::array<int, 64>& subdivisions)
{
    subdivisions.fill(1);
    if (text.isEmpty())
        return;

    juce::StringArray tokens;
    tokens.addTokens(text, ",", "");
    tokens.trim();
    tokens.removeEmptyStrings();
    const int length = juce::jmin(static_cast<int>(subdivisions.size()), tokens.size());
    for (int i = 0; i < length; ++i)
    {
        subdivisions[static_cast<size_t>(i)] = juce::jlimit(1,
                                                             EnhancedAudioStrip::MaxStepSubdivisions,
                                                             tokens[i].getIntValue());
    }
}

void decodePreparedStepSubdivisionRepeatVelocity(const juce::String& text, std::array<float, 64>& values)
{
    values.fill(1.0f);
    if (text.isEmpty())
        return;

    juce::StringArray tokens;
    tokens.addTokens(text, ",", "");
    tokens.trim();
    tokens.removeEmptyStrings();
    const int length = juce::jmin(static_cast<int>(values.size()), tokens.size());
    for (int i = 0; i < length; ++i)
        values[static_cast<size_t>(i)] = juce::jlimit(0.0f, 1.0f, tokens[i].getFloatValue());
}

void decodePreparedModStepsLegacyBits(const juce::String& text,
                                      std::array<float, ModernAudioEngine::ModTotalSteps>& steps)
{
    steps.fill(0.0f);
    const int length = juce::jmin(ModernAudioEngine::ModSteps, text.length());
    for (int i = 0; i < length; ++i)
        steps[static_cast<size_t>(i)] = (text[i] == '1') ? 1.0f : 0.0f;
}

void decodePreparedModSteps(const juce::String& text,
                            std::array<float, ModernAudioEngine::ModTotalSteps>& steps)
{
    steps.fill(0.0f);
    if (!text.containsChar(','))
    {
        decodePreparedModStepsLegacyBits(text, steps);
        return;
    }

    juce::StringArray tokens;
    tokens.addTokens(text, ",", "");
    tokens.trim();
    tokens.removeEmptyStrings();
    const int length = juce::jmin(ModernAudioEngine::ModTotalSteps, tokens.size());
    for (int i = 0; i < length; ++i)
        steps[static_cast<size_t>(i)] = juce::jlimit(0.0f,
                                                     1.0f,
                                                     static_cast<float>(tokens[i].getDoubleValue()));
}

float readPreparedStateFloatProperty(const juce::ValueTree& state,
                                     const juce::String& propertyId,
                                     float fallback)
{
    if (!state.isValid() || !state.hasProperty(propertyId))
        return fallback;

    return state.getProperty(propertyId).toString().getFloatValue();
}

int readPreparedStateIntProperty(const juce::ValueTree& state,
                                 const juce::String& propertyId,
                                 int fallback)
{
    if (!state.isValid() || !state.hasProperty(propertyId))
        return fallback;

    return state.getProperty(propertyId).toString().getIntValue();
}

bool readPreparedStateBoolProperty(const juce::ValueTree& state,
                                   const juce::String& propertyId,
                                   bool fallback)
{
    if (!state.isValid() || !state.hasProperty(propertyId))
        return fallback;

    return readPreparedStateFloatProperty(state, propertyId, fallback ? 1.0f : 0.0f) > 0.5f;
}

} // namespace

std::shared_ptr<const MlrVSTAudioProcessor::SceneTransitionEndSampleData>
MlrVSTAudioProcessor::loadSceneTransitionEndSampleData(const juce::File& file) const
{
    juce::AudioBuffer<float> sampleBuffer;
    double sourceRate = 0.0;
    if (!readAudioFileToStereoBuffer(file, sampleBuffer, sourceRate))
        return {};

    auto sampleData = std::make_shared<SceneTransitionEndSampleData>();
    sampleData->buffer.makeCopyOf(sampleBuffer);
    sampleData->sourceSampleRate = (sourceRate > 0.0) ? sourceRate : currentSampleRate;
    sampleData->path = file.getFullPathName();
    sampleData->displayName = file.getFileNameWithoutExtension();
    return sampleData;
}

void MlrVSTAudioProcessor::reloadSceneChainTransitionEndSample(int stepIndex)
{
    if (stepIndex < 0 || stepIndex >= MaxSceneChainSteps)
        return;

    const auto idx = static_cast<size_t>(stepIndex);
    const auto assignedFile = sceneChainState.steps[idx].transitionEndSampleFile;
    if (assignedFile == juce::File() || assignedFile.getFullPathName().trim().isEmpty())
    {
        std::atomic_store_explicit(&sceneTransitionEndSamples[idx],
                                   std::shared_ptr<const SceneTransitionEndSampleData>{},
                                   std::memory_order_release);
        return;
    }

    auto sampleData = loadSceneTransitionEndSampleData(assignedFile);
    if (sampleData == nullptr)
    {
        std::atomic_store_explicit(&sceneTransitionEndSamples[idx],
                                   std::shared_ptr<const SceneTransitionEndSampleData>{},
                                   std::memory_order_release);
        return;
    }
    std::atomic_store_explicit(&sceneTransitionEndSamples[idx],
                               std::shared_ptr<const SceneTransitionEndSampleData>(std::move(sampleData)),
                               std::memory_order_release);
}

void MlrVSTAudioProcessor::reloadAllSceneChainTransitionEndSamples()
{
    for (int stepIndex = 0; stepIndex < MaxSceneChainSteps; ++stepIndex)
        reloadSceneChainTransitionEndSample(stepIndex);
}

bool MlrVSTAudioProcessor::decodeEmbeddedSceneAudioToStereoBuffer(const juce::String& base64Audio,
                                                                  juce::AudioBuffer<float>& buffer,
                                                                  double& sourceRate) const
{
    buffer.setSize(0, 0);
    sourceRate = 0.0;

    const auto trimmed = base64Audio.trim();
    if (trimmed.isEmpty())
        return false;

    juce::MemoryOutputStream decodedBytes;
    if (!juce::Base64::convertFromBase64(decodedBytes, trimmed))
        return false;

    auto input = std::make_unique<juce::MemoryInputStream>(decodedBytes.getData(),
                                                           decodedBytes.getDataSize(),
                                                           false);
    juce::AudioFormatManager formatManager;
    formatManager.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(std::move(input)));
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

bool MlrVSTAudioProcessor::buildPreparedSceneStripAnalysisPayload(int stripIndex,
                                                                  PreparedSceneStripAudioPayload& payload) const
{
    if (payload.audioBuffer.getNumSamples() <= 0 || payload.sourceSampleRate <= 0.0)
        return false;

    EnhancedAudioStrip analysisStrip(stripIndex);
    const double prepareRate = currentSampleRate > 0.0 ? currentSampleRate : payload.sourceSampleRate;
    analysisStrip.prepareToPlay(prepareRate, juce::jmax(1, getBlockSize()));
    analysisStrip.loadSample(payload.audioBuffer, payload.sourceSampleRate);
    if (!analysisStrip.hasSampleAnalysisCache())
        return false;

    payload.transientSlices = analysisStrip.getCachedTransientSliceSamples();
    payload.rmsMap = analysisStrip.getCachedRmsMap();
    payload.zeroCrossMap = analysisStrip.getCachedZeroCrossMap();
    payload.analysisSampleCount = analysisStrip.getAnalysisSampleCount();
    payload.valid = true;
    return true;
}

bool MlrVSTAudioProcessor::tryCloneLiveStripAudioPayload(int stripIndex,
                                                         const juce::File& file,
                                                         PreparedSceneStripAudioPayload& payload) const
{
    payload = {};

    if (audioEngine == nullptr || stripIndex < 0 || stripIndex >= MaxStrips)
        return false;

    const auto targetFilePath = file.getFullPathName();
    auto capturePayload = [&](const juce::AudioBuffer<float>& sourceBuffer,
                              double sourceRate,
                              const EnhancedAudioStrip* analysisSourceStrip) -> bool
    {
        if (sourceBuffer.getNumSamples() <= 0 || sourceRate <= 0.0)
            return false;

        payload.audioBuffer.makeCopyOf(sourceBuffer, true);
        payload.sourceSampleRate = sourceRate;
        if (analysisSourceStrip != nullptr && analysisSourceStrip->hasSampleAnalysisCache())
        {
            payload.transientSlices = analysisSourceStrip->getCachedTransientSliceSamples();
            payload.rmsMap = analysisSourceStrip->getCachedRmsMap();
            payload.zeroCrossMap = analysisSourceStrip->getCachedZeroCrossMap();
            payload.analysisSampleCount = analysisSourceStrip->getAnalysisSampleCount();
            payload.valid = true;
            return true;
        }

        return buildPreparedSceneStripAnalysisPayload(stripIndex, payload);
    };

    for (int sourceStripIndex = 0; sourceStripIndex < MaxStrips; ++sourceStripIndex)
    {
        const auto& currentFile = currentStripFiles[static_cast<size_t>(sourceStripIndex)];
        const bool fileMatches = currentFile == file
            || (!targetFilePath.isEmpty() && currentFile.getFullPathName() == targetFilePath);
        if (!fileMatches)
            continue;

        if (auto* liveStrip = audioEngine->getStrip(sourceStripIndex))
        {
            const auto* liveBuffer = liveStrip->getAudioBuffer();
            if (liveStrip->hasAudio()
                && liveBuffer != nullptr
                && capturePayload(*liveBuffer, liveStrip->getSourceSampleRate(), liveStrip))
            {
                return true;
            }
        }

        if (auto* liveSampleEngine = sampleModeEngines[static_cast<size_t>(sourceStripIndex)].get())
        {
            if (auto loadedSample = liveSampleEngine->getLoadedSample())
            {
                const bool sampleMatches = loadedSample->audioBuffer.getNumSamples() > 0
                    && (loadedSample->sourcePath == targetFilePath
                        || (!currentFile.getFullPathName().isEmpty()
                            && loadedSample->sourcePath == currentFile.getFullPathName()));
                if (sampleMatches
                    && capturePayload(loadedSample->audioBuffer,
                                      loadedSample->sourceSampleRate,
                                      nullptr))
                {
                    return true;
                }
            }
        }
    }

    return false;
}

bool MlrVSTAudioProcessor::tryLoadPreparedSceneStripAudioPayloadFromCache(const juce::File& file,
                                                                          PreparedSceneStripAudioPayload& payload)
{
    payload = {};

    const auto filePath = file.getFullPathName().trim();
    if (filePath.isEmpty())
        return false;

    const bool fileExists = safeFileExistsAsFile(file);
    const int64_t fileSize = fileExists ? file.getSize() : -1;
    const int64_t fileModificationTimeMs = fileExists ? file.getLastModificationTime().toMilliseconds() : 0;

    const juce::ScopedLock lock(sceneFileAudioCacheLock);
    for (const auto& entry : sceneFileAudioCache)
    {
        if (entry.filePath != filePath)
            continue;

        if (fileExists
            && (entry.fileSize != fileSize || entry.fileModificationTimeMs != fileModificationTimeMs))
        {
            return false;
        }

        if (entry.audioBuffer.getNumSamples() <= 0 || entry.sourceSampleRate <= 0.0)
            return false;

        payload.audioBuffer.makeCopyOf(entry.audioBuffer, true);
        payload.sourceSampleRate = entry.sourceSampleRate;
        payload.transientSlices = entry.transientSlices;
        payload.rmsMap = entry.rmsMap;
        payload.zeroCrossMap = entry.zeroCrossMap;
        payload.analysisSampleCount = entry.analysisSampleCount;
        payload.valid = true;
        return true;
    }

    return false;
}

bool MlrVSTAudioProcessor::tryBuildPreparedSceneStripAudioPayload(int stripIndex,
                                                                  const juce::XmlElement& stripXml,
                                                                  PreparedSceneStripAudioPayload& payload)
{
    payload = {};

    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return false;

    const auto samplePath = stripXml.getStringAttribute("samplePath").trim();
    if (samplePath.isNotEmpty() && juce::File::isAbsolutePath(samplePath))
    {
        const juce::File storedSampleFile(samplePath);
        if (tryCloneLiveStripAudioPayload(stripIndex, storedSampleFile, payload))
            return true;

        if (tryLoadPreparedSceneStripAudioPayloadFromCache(storedSampleFile, payload))
            return true;

        if (!readAudioFileToStereoBuffer(storedSampleFile, payload.audioBuffer, payload.sourceSampleRate))
            return false;

        if (!buildPreparedSceneStripAnalysisPayload(stripIndex, payload))
            return false;

        cachePreparedSceneAudioFilePayload(storedSampleFile, payload);
        return true;
    }

    constexpr const char* kEmbeddedSampleAttr = "embeddedSampleWavBase64";
    const auto embeddedSample = stripXml.getStringAttribute(kEmbeddedSampleAttr);
    if (embeddedSample.isEmpty())
        return false;

    if (!decodeEmbeddedSceneAudioToStereoBuffer(embeddedSample, payload.audioBuffer, payload.sourceSampleRate))
        return false;

    return buildPreparedSceneStripAnalysisPayload(stripIndex, payload);
}

bool MlrVSTAudioProcessor::tryBuildPreparedSceneStripAudioPayload(int stripIndex,
                                                                  const PreparedSceneStripState& stripState,
                                                                  PreparedSceneStripAudioPayload& payload)
{
    payload = {};
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return false;

    auto applyStoredAnalysis = [&](PreparedSceneStripAudioPayload& destination)
    {
        if (stripState.analysisSampleCount <= 0)
            return;

        destination.analysisSampleCount = stripState.analysisSampleCount;
        destination.transientSlices = stripState.analysisTransientSlices;
        destination.rmsMap = stripState.analysisRmsMap;
        destination.zeroCrossMap = stripState.analysisZeroCrossMap;
    };

    if (stripState.hasStoredSamplePath)
    {
        const auto& storedSampleFile = stripState.storedSampleFile;
        if (tryCloneLiveStripAudioPayload(stripIndex, storedSampleFile, payload))
        {
            applyStoredAnalysis(payload);
            return true;
        }

        if (tryLoadPreparedSceneStripAudioPayloadFromCache(storedSampleFile, payload))
        {
            applyStoredAnalysis(payload);
            return true;
        }

        if (!readAudioFileToStereoBuffer(storedSampleFile, payload.audioBuffer, payload.sourceSampleRate))
            return false;

        if (stripState.analysisSampleCount > 0)
        {
            applyStoredAnalysis(payload);
            payload.valid = true;
            cachePreparedSceneAudioFilePayload(storedSampleFile, payload);
            return true;
        }

        if (!buildPreparedSceneStripAnalysisPayload(stripIndex, payload))
            return false;

        cachePreparedSceneAudioFilePayload(storedSampleFile, payload);
        return true;
    }

    if (stripState.embeddedSampleWavBase64.isEmpty())
        return false;

    if (!decodeEmbeddedSceneAudioToStereoBuffer(stripState.embeddedSampleWavBase64,
                                                payload.audioBuffer,
                                                payload.sourceSampleRate))
    {
        return false;
    }

    if (stripState.analysisSampleCount > 0)
    {
        applyStoredAnalysis(payload);
        payload.valid = true;
        return true;
    }

    return buildPreparedSceneStripAnalysisPayload(stripIndex, payload);
}

std::unique_ptr<MlrVSTAudioProcessor::PreparedSceneSwitchPayload>
MlrVSTAudioProcessor::clonePreparedSceneSwitchPayloadTemplate(const PreparedSceneSwitchPayload& source) const
{
    auto destination = std::make_unique<PreparedSceneSwitchPayload>();
    destination->mainPresetIndex = source.mainPresetIndex;
    destination->sceneSlot = source.sceneSlot;
    destination->sequenceDriven = source.sequenceDriven;
    destination->sequenceStepIndex = source.sequenceStepIndex;
    destination->switchSerial = source.switchSerial;
    for (size_t i = 0; i < source.stripStates.size(); ++i)
    {
        const auto& sourceState = source.stripStates[i];
        auto& destState = destination->stripStates[i];
        destState = {};
        destState.present = sourceState.present;
        destState.restorePlaying = sourceState.restorePlaying;
        destState.hasStoredSamplePath = sourceState.hasStoredSamplePath;
        destState.storedSampleFile = sourceState.storedSampleFile;
        destState.embeddedSampleWavBase64 = sourceState.embeddedSampleWavBase64;
        destState.recentLoopDirectory = sourceState.recentLoopDirectory;
        destState.recentStepDirectory = sourceState.recentStepDirectory;
        destState.recentFlipDirectory = sourceState.recentFlipDirectory;
        destState.analysisSampleCount = sourceState.analysisSampleCount;
        destState.analysisTransientSlices = sourceState.analysisTransientSlices;
        destState.analysisRmsMap = sourceState.analysisRmsMap;
        destState.analysisZeroCrossMap = sourceState.analysisZeroCrossMap;
        destState.playMode = sourceState.playMode;
        destState.loopStart = sourceState.loopStart;
        destState.loopEnd = sourceState.loopEnd;
        destState.playbackColumn = sourceState.playbackColumn;
        destState.ppqTimelineAnchored = sourceState.ppqTimelineAnchored;
        destState.ppqTimelineOffsetBeats = sourceState.ppqTimelineOffsetBeats;
        destState.directionMode = sourceState.directionMode;
        destState.reverse = sourceState.reverse;
        destState.groupId = sourceState.groupId;
        destState.beatsPerLoop = sourceState.beatsPerLoop;
        destState.scratchAmount = sourceState.scratchAmount;
        destState.transientSliceMode = sourceState.transientSliceMode;
        destState.pitchShift = sourceState.pitchShift;
        destState.recordingBars = sourceState.recordingBars;
        destState.filterEnabled = sourceState.filterEnabled;
        destState.filterFrequency = sourceState.filterFrequency;
        destState.filterResonance = sourceState.filterResonance;
        destState.filterMorph = sourceState.filterMorph;
        destState.filterAlgorithm = sourceState.filterAlgorithm;
        destState.swingAmount = sourceState.swingAmount;
        destState.gateAmount = sourceState.gateAmount;
        destState.gateSpeed = sourceState.gateSpeed;
        destState.gateEnvelope = sourceState.gateEnvelope;
        destState.gateShape = sourceState.gateShape;
        destState.stepPatternSteps = sourceState.stepPatternSteps;
        destState.stepViewPage = sourceState.stepViewPage;
        destState.stepCurrent = sourceState.stepCurrent;
        destState.stepPattern = sourceState.stepPattern;
        destState.stepSubdivisions = sourceState.stepSubdivisions;
        destState.stepSubdivisionStartVelocity = sourceState.stepSubdivisionStartVelocity;
        destState.stepSubdivisionRepeatVelocity = sourceState.stepSubdivisionRepeatVelocity;
        destState.stepProbability = sourceState.stepProbability;
        destState.stepAttackMs = sourceState.stepAttackMs;
        destState.stepDecayMs = sourceState.stepDecayMs;
        destState.stepReleaseMs = sourceState.stepReleaseMs;
        destState.grainSizeMs = sourceState.grainSizeMs;
        destState.grainDensity = sourceState.grainDensity;
        destState.grainPitch = sourceState.grainPitch;
        destState.grainPitchJitter = sourceState.grainPitchJitter;
        destState.grainSpread = sourceState.grainSpread;
        destState.grainJitter = sourceState.grainJitter;
        destState.grainPositionJitter = sourceState.grainPositionJitter;
        destState.grainRandomDepth = sourceState.grainRandomDepth;
        destState.grainArpDepth = sourceState.grainArpDepth;
        destState.grainCloudDepth = sourceState.grainCloudDepth;
        destState.grainEmitterDepth = sourceState.grainEmitterDepth;
        destState.grainEnvelope = sourceState.grainEnvelope;
        destState.grainShape = sourceState.grainShape;
        destState.grainArpMode = sourceState.grainArpMode;
        destState.grainTempoSync = sourceState.grainTempoSync;
        destState.activeModSlot = sourceState.activeModSlot;
        destState.modLanes = sourceState.modLanes;
        destState.parameterState = sourceState.parameterState;
        destState.hasFlipState = sourceState.hasFlipState;
        destState.flipState = sourceState.flipState;
        destState.embeddedFlipSampleBase64 = sourceState.embeddedFlipSampleBase64;
        destState.loopPitchState = sourceState.loopPitchState;
    }
    destination->groupStates = source.groupStates;
    destination->patternStates = source.patternStates;
    destination->scenePerformanceStateData = source.scenePerformanceStateData;
    destination->parameterState = source.parameterState;
    destination->sceneTimingState = source.sceneTimingState;
    if (source.snapshotPresetXml != nullptr)
        destination->snapshotPresetXml = std::make_unique<juce::XmlElement>(*source.snapshotPresetXml);
    return destination;
}

bool MlrVSTAudioProcessor::parsePreparedSceneSwitchPayloadTemplate(PreparedSceneSwitchPayload& payload,
                                                                  const juce::XmlElement& presetXml,
                                                                  int mainPresetIndex,
                                                                  int sceneSlot) const
{
    payload = {};
    payload.mainPresetIndex = juce::jlimit(0, MaxPresetSlots - 1, mainPresetIndex);
    payload.sceneSlot = juce::jlimit(0, SceneSlots - 1, sceneSlot);
    payload.sequenceDriven = false;
    payload.sequenceStepIndex = -1;
    payload.snapshotPresetXml = std::make_unique<juce::XmlElement>(presetXml);

    if (payload.snapshotPresetXml != nullptr)
    {
        if (auto* paramsXml = payload.snapshotPresetXml->getChildByName("ParametersState"))
        {
            auto state = juce::ValueTree::fromXml(*paramsXml);
            if (state.isValid())
            {
                stripPersistentGlobalControlsFromState(state);
                auto strippedParamsXml = state.createXml();
                payload.snapshotPresetXml->removeChildElement(paramsXml, true);
                if (strippedParamsXml != nullptr)
                    payload.snapshotPresetXml->addChildElement(strippedParamsXml.release());
            }
        }
    }

    if (auto* paramsXml = presetXml.getChildByName("ParametersState"))
    {
        auto state = juce::ValueTree::fromXml(*paramsXml);
        if (state.isValid())
        {
            stripPersistentGlobalControlsFromState(state);
            payload.parameterState = state;
        }
    }

    if (auto* sceneTimingXml = presetXml.getChildByName("SceneChainState"))
    {
        payload.sceneTimingState.valid = true;
        payload.sceneTimingState.repeatCount = sceneTimingXml->getIntAttribute("repeatCount", 1);
        payload.sceneTimingState.lengthModeIndex = sceneTimingXml->getIntAttribute(
            "lengthMode",
            static_cast<int>(SceneLengthMode::ManualBars));
        payload.sceneTimingState.manualBars = sceneTimingXml->getIntAttribute("manualBars", 4);
        payload.sceneTimingState.anchorStrip = sceneTimingXml->getIntAttribute("anchorStrip", 0);
    }

    for (auto* stripXml : presetXml.getChildWithTagNameIterator("Strip"))
    {
        const int stripIndex = stripXml->getIntAttribute("index", -1);
        if (stripIndex < 0 || stripIndex >= MaxStrips)
            continue;

        auto& stripState = payload.stripStates[static_cast<size_t>(stripIndex)];
        stripState = {};
        stripState.present = true;
        stripState.restorePlaying = stripXml->getBoolAttribute("isPlaying", false);
        stripState.playMode = static_cast<EnhancedAudioStrip::PlayMode>(
            clampPreparedInt(stripXml->getIntAttribute("playMode", 1), 0, 5, 1));
        stripState.loopStart = clampPreparedInt(stripXml->getIntAttribute("loopStart", 0), 0, 15, 0);
        stripState.loopEnd = clampPreparedInt(stripXml->getIntAttribute("loopEnd", 16), 1, 16, 16);
        stripState.playbackColumn = clampPreparedInt(stripXml->getIntAttribute("playbackColumn", stripState.loopStart),
                                                     0,
                                                     ModernAudioEngine::MaxColumns - 1,
                                                     stripState.loopStart);
        stripState.ppqTimelineAnchored = stripXml->getBoolAttribute("ppqTimelineAnchored", false);
        stripState.ppqTimelineOffsetBeats = stripXml->getDoubleAttribute("ppqTimelineOffsetBeats", 0.0);
        stripState.directionMode = static_cast<EnhancedAudioStrip::DirectionMode>(
            clampPreparedInt(stripXml->getIntAttribute("directionMode", 0), 0, 5, 0));
        stripState.reverse = stripXml->getBoolAttribute("reversed", false);
        stripState.groupId = stripXml->getIntAttribute("group", -1);
        stripState.beatsPerLoop = std::isfinite(stripXml->getDoubleAttribute("beatsPerLoop", -1.0))
            ? static_cast<float>(stripXml->getDoubleAttribute("beatsPerLoop", -1.0))
            : -1.0f;
        stripState.scratchAmount = clampPreparedFloat(stripXml->getDoubleAttribute("scratchAmount", 0.0),
                                                      0.0f,
                                                      0.0f,
                                                      100.0f);
        stripState.transientSliceMode = stripXml->getBoolAttribute("transientSliceMode", false);
        stripState.recordingBars = clampPreparedInt(stripXml->getIntAttribute("recordingBars", 2), 1, 8, 2);
        stripState.filterEnabled = stripXml->getBoolAttribute("filterEnabled", false);
        stripState.filterFrequency = clampPreparedFloat(stripXml->getDoubleAttribute("filterFrequency", 20000.0),
                                                        20000.0f,
                                                        20.0f,
                                                        20000.0f);
        stripState.filterResonance = clampPreparedFloat(stripXml->getDoubleAttribute("filterResonance", 0.707),
                                                        0.707f,
                                                        0.1f,
                                                        10.0f);
        if (stripXml->hasAttribute("filterMorph"))
        {
            stripState.filterMorph = clampPreparedFloat(stripXml->getDoubleAttribute("filterMorph", 0.0),
                                                        0.0f,
                                                        0.0f,
                                                        1.0f);
        }
        else
        {
            const int legacyFilterType = clampPreparedInt(stripXml->getIntAttribute("filterType", 0), 0, 2, 0);
            stripState.filterMorph = legacyFilterType == 0 ? 0.0f : (legacyFilterType == 1 ? 0.5f : 1.0f);
        }
        stripState.filterAlgorithm = static_cast<EnhancedAudioStrip::FilterAlgorithm>(
            clampPreparedInt(stripXml->getIntAttribute("filterAlgorithm", 0), 0, 5, 0));
        stripState.swingAmount = clampPreparedFloat(stripXml->getDoubleAttribute("swingAmount", 0.0),
                                                    0.0f,
                                                    0.0f,
                                                    1.0f);
        stripState.gateAmount = clampPreparedFloat(stripXml->getDoubleAttribute("gateAmount", 0.0),
                                                   0.0f,
                                                   0.0f,
                                                   1.0f);
        stripState.gateSpeed = clampPreparedFloat(stripXml->getDoubleAttribute("gateSpeed", 4.0),
                                                  4.0f,
                                                  0.25f,
                                                  16.0f);
        stripState.gateEnvelope = clampPreparedFloat(stripXml->getDoubleAttribute("gateEnvelope", 0.5),
                                                     0.5f,
                                                     0.0f,
                                                     1.0f);
        if (stripXml->hasAttribute("gateShapeCurve"))
        {
            stripState.gateShape = clampPreparedFloat(stripXml->getDoubleAttribute("gateShapeCurve", 0.5),
                                                      0.5f,
                                                      0.0f,
                                                      1.0f);
        }
        else
        {
            const int legacyShape = clampPreparedInt(stripXml->getIntAttribute("gateShape", 0), 0, 2, 0);
            stripState.gateShape = legacyShape == 1 ? 0.75f : (legacyShape == 2 ? 0.2f : 0.5f);
        }
        stripState.stepPatternSteps = clampPreparedInt(
            stripXml->getIntAttribute("stepPatternSteps",
                                      clampPreparedInt(stripXml->getIntAttribute("stepPatternBars", 1), 1, 4, 1) * 16),
            1,
            64,
            16);
        stripState.stepViewPage = clampPreparedInt(stripXml->getIntAttribute("stepViewPage", 0), 0, 3, 0);
        stripState.stepCurrent = juce::jmax(0, stripXml->getIntAttribute("stepCurrent", 0));
        decodePreparedStepPatternBits(stripXml->getStringAttribute("stepPatternBits"), stripState.stepPattern);
        decodePreparedStepSubdivisions(stripXml->getStringAttribute("stepSubdivisions"), stripState.stepSubdivisions);
        decodePreparedStepSubdivisionRepeatVelocity(stripXml->getStringAttribute("stepSubdivisionStartVelocity"),
                                                    stripState.stepSubdivisionStartVelocity);
        decodePreparedStepSubdivisionRepeatVelocity(stripXml->getStringAttribute("stepSubdivisionRepeatVelocity"),
                                                    stripState.stepSubdivisionRepeatVelocity);
        decodePreparedStepSubdivisionRepeatVelocity(stripXml->getStringAttribute("stepProbability"),
                                                    stripState.stepProbability);
        stripState.stepAttackMs = clampPreparedFloat(stripXml->getDoubleAttribute("stepAttackMs", 0.0),
                                                     0.0f,
                                                     0.0f,
                                                     400.0f);
        stripState.stepDecayMs = clampPreparedFloat(stripXml->getDoubleAttribute("stepDecayMs", 4000.0),
                                                    4000.0f,
                                                    1.0f,
                                                    4000.0f);
        stripState.stepReleaseMs = clampPreparedFloat(stripXml->getDoubleAttribute("stepReleaseMs", 110.0),
                                                      110.0f,
                                                      1.0f,
                                                      4000.0f);
        stripState.grainSizeMs = static_cast<float>(stripXml->getDoubleAttribute("grainSizeMs", 1240.0));
        stripState.grainDensity = static_cast<float>(stripXml->getDoubleAttribute("grainDensity", 0.05));
        stripState.grainPitch = clampPreparedFloat(stripXml->getDoubleAttribute("grainPitch", 0.0),
                                                   0.0f,
                                                   -48.0f,
                                                   48.0f);
        stripState.grainPitchJitter = static_cast<float>(stripXml->getDoubleAttribute("grainPitchJitter", 0.0));
        stripState.grainSpread = static_cast<float>(stripXml->getDoubleAttribute("grainSpread", 0.0));
        stripState.grainJitter = static_cast<float>(stripXml->getDoubleAttribute("grainJitter", 0.0));
        stripState.grainPositionJitter = static_cast<float>(stripXml->getDoubleAttribute("grainPositionJitter", 0.0));
        stripState.grainRandomDepth = static_cast<float>(stripXml->getDoubleAttribute("grainRandomDepth", 0.0));
        stripState.grainArpDepth = static_cast<float>(stripXml->getDoubleAttribute("grainArpDepth", 0.0));
        stripState.grainCloudDepth = static_cast<float>(stripXml->getDoubleAttribute("grainCloudDepth", 0.0));
        stripState.grainEmitterDepth = static_cast<float>(stripXml->getDoubleAttribute("grainEmitterDepth", 0.0));
        stripState.grainEnvelope = static_cast<float>(stripXml->getDoubleAttribute("grainEnvelope", 0.0));
        stripState.grainShape = clampPreparedFloat(stripXml->getDoubleAttribute("grainShape", 0.0),
                                                   0.0f,
                                                   -1.0f,
                                                   1.0f);
        stripState.grainArpMode = clampPreparedInt(stripXml->getIntAttribute("grainArpMode", 0), 0, 5, 0);
        stripState.grainTempoSync = stripXml->getBoolAttribute("grainTempoSync", true);
        stripState.activeModSlot = clampPreparedInt(stripXml->getIntAttribute("modActiveSequencer", 0),
                                                    0,
                                                    ModernAudioEngine::NumModSequencers - 1,
                                                    0);

        const auto samplePath = stripXml->getStringAttribute("samplePath").trim();
        stripState.hasStoredSamplePath = isValidPreparedStoredSamplePath(samplePath);
        stripState.storedSampleFile = stripState.hasStoredSamplePath ? juce::File(samplePath) : juce::File();
        stripState.embeddedSampleWavBase64 = stripXml->getStringAttribute("embeddedSampleWavBase64");
        stripState.recentLoopDirectory = restorePreparedStoredDirectory(stripXml->getStringAttribute("recentLoopDir"));
        stripState.recentStepDirectory = restorePreparedStoredDirectory(stripXml->getStringAttribute("recentStepDir"));
        stripState.recentFlipDirectory = restorePreparedStoredDirectory(stripXml->getStringAttribute("recentFlipDir"));
        stripState.analysisSampleCount = juce::jmax(
            0,
            stripXml->getIntAttribute(kPreparedSceneAnalysisSampleCountAttr, 0));
        if (stripState.analysisSampleCount > 0)
        {
            decodePreparedIntArrayCsv(stripXml->getStringAttribute(kPreparedSceneAnalysisTransientAttr),
                                      stripState.analysisTransientSlices);
            decodePreparedFloatArrayCsv(stripXml->getStringAttribute(kPreparedSceneAnalysisRmsAttr),
                                        stripState.analysisRmsMap);
            decodePreparedIntArrayCsv(stripXml->getStringAttribute(kPreparedSceneAnalysisZeroCrossAttr),
                                      stripState.analysisZeroCrossMap);
        }

        if (auto* flipStateXml = stripXml->getChildByName("FlipState"))
        {
            stripState.hasFlipState = true;
            stripState.flipState = SampleModePersistentState::fromXml(*flipStateXml);
            stripState.embeddedFlipSampleBase64 = flipStateXml->getStringAttribute(kEmbeddedFlipSampleAttr);
        }
        if (auto* loopPitchStateXml = stripXml->getChildByName("LoopPitchState"))
        {
            stripState.loopPitchState.valid = true;
            stripState.loopPitchState.role = sanitizeLoopPitchRole(loopPitchStateXml->getIntAttribute("role", 0));
            stripState.loopPitchState.syncTiming = sanitizeLoopPitchSyncTiming(
                loopPitchStateXml->getIntAttribute("syncTiming", 0));
            stripState.loopPitchState.assignedMidi =
                juce::jlimit(-1, 127, loopPitchStateXml->getIntAttribute("assignedMidi", -1));
            stripState.loopPitchState.assignedManual =
                loopPitchStateXml->getBoolAttribute("assignedManual", false);
            stripState.loopPitchState.detectedMidi =
                juce::jlimit(-1, 127, loopPitchStateXml->getIntAttribute("detectedMidi", -1));
            stripState.loopPitchState.detectedHz = juce::jlimit(
                0.0f,
                24000.0f,
                static_cast<float>(loopPitchStateXml->getDoubleAttribute("detectedHz", 0.0)));
            stripState.loopPitchState.detectedPitchConfidence = juce::jlimit(
                0.0f,
                1.0f,
                static_cast<float>(loopPitchStateXml->getDoubleAttribute("detectedPitchConfidence", 0.0)));
            stripState.loopPitchState.detectedScaleIndex = juce::jlimit(
                -1,
                static_cast<int>(ModernAudioEngine::PitchScale::PentatonicMinor),
                loopPitchStateXml->getIntAttribute("detectedScaleIndex", -1));
            stripState.loopPitchState.detectedScaleConfidence = juce::jlimit(
                0.0f,
                1.0f,
                static_cast<float>(loopPitchStateXml->getDoubleAttribute("detectedScaleConfidence", 0.0)));
            stripState.loopPitchState.essentiaUsed =
                loopPitchStateXml->getBoolAttribute("essentiaUsed", false);
        }

        auto& preparedParams = stripState.parameterState;
        preparedParams.valid = true;
        preparedParams.ownedControls.usesGrainPlaybackSpeed =
            stripState.playMode == EnhancedAudioStrip::PlayMode::Grain;
        preparedParams.ownedControls.volume = clampPreparedFloat(
            readPreparedStateFloatProperty(payload.parameterState,
                                           "stripVolume" + juce::String(stripIndex),
                                           static_cast<float>(stripXml->getDoubleAttribute("volume", 1.0))),
            1.0f,
            0.0f,
            1.0f);
        preparedParams.ownedControls.trimDb = readPreparedStateFloatProperty(payload.parameterState,
                                                                             "stripTrimDb" + juce::String(stripIndex),
                                                                             0.0f);
        preparedParams.ownedControls.pan = clampPreparedFloat(
            readPreparedStateFloatProperty(payload.parameterState,
                                           "stripPan" + juce::String(stripIndex),
                                           static_cast<float>(stripXml->getDoubleAttribute("pan", 0.0))),
            0.0f,
            -1.0f,
            1.0f);
        const float speedControl = clampPreparedFloat(
            readPreparedStateFloatProperty(payload.parameterState,
                                           "stripSpeed" + juce::String(stripIndex),
                                           static_cast<float>(stripXml->getDoubleAttribute("speed", 1.0))),
            1.0f,
            0.0f,
            8.0f);
        preparedParams.ownedControls.playbackSpeed = PlayheadSpeedQuantizer::grainPlaybackSpeedFromControl(speedControl);
        preparedParams.ownedControls.playheadSpeedRatio =
            PlayheadSpeedQuantizer::quantizeRatio(juce::jlimit(0.125f, 8.0f, speedControl));
        preparedParams.ownedControls.filterEnabled =
            readPreparedStateBoolProperty(payload.parameterState,
                                          "stripFilterEnabled" + juce::String(stripIndex),
                                          stripState.filterEnabled);
        preparedParams.ownedControls.filterFrequency = clampPreparedFloat(
            readPreparedStateFloatProperty(payload.parameterState,
                                           "stripFilterFrequency" + juce::String(stripIndex),
                                           stripState.filterFrequency),
            stripState.filterFrequency,
            20.0f,
            20000.0f);
        preparedParams.ownedControls.filterResonance = clampPreparedFloat(
            readPreparedStateFloatProperty(payload.parameterState,
                                           "stripFilterResonance" + juce::String(stripIndex),
                                           stripState.filterResonance),
            stripState.filterResonance,
            0.1f,
            10.0f);
        preparedParams.ownedControls.filterMorph = clampPreparedFloat(
            readPreparedStateFloatProperty(payload.parameterState,
                                           "stripFilterMorph" + juce::String(stripIndex),
                                           stripState.filterMorph),
            stripState.filterMorph,
            0.0f,
            1.0f);
        preparedParams.ownedControls.filterAlgorithm = static_cast<EnhancedAudioStrip::FilterAlgorithm>(
            clampPreparedInt(
                readPreparedStateIntProperty(payload.parameterState,
                                             "stripFilterAlgorithm" + juce::String(stripIndex),
                                             static_cast<int>(stripState.filterAlgorithm)),
                0,
                5,
                static_cast<int>(stripState.filterAlgorithm)));
        preparedParams.ownedControls.delayMix = clampPreparedFloat(
            readPreparedStateFloatProperty(payload.parameterState,
                                           "stripDelayMix" + juce::String(stripIndex),
                                           0.0f),
            0.0f,
            0.0f,
            1.0f);
        preparedParams.ownedControls.delayTime = clampPreparedFloat(
            readPreparedStateFloatProperty(payload.parameterState,
                                           "stripDelayTime" + juce::String(stripIndex),
                                           1.0f),
            1.0f,
            0.25f,
            4.0f);
        preparedParams.ownedControls.delaySyncEnabled =
            readPreparedStateBoolProperty(payload.parameterState,
                                          "stripDelaySync" + juce::String(stripIndex),
                                          true);
        preparedParams.ownedControls.delayFeedback = clampPreparedFloat(
            readPreparedStateFloatProperty(payload.parameterState,
                                           "stripDelayFeedback" + juce::String(stripIndex),
                                           0.35f),
            0.35f,
            0.0f,
            0.97f);
        preparedParams.ownedControls.delayLowCutHz = clampPreparedFloat(
            readPreparedStateFloatProperty(payload.parameterState,
                                           "stripDelayLowCut" + juce::String(stripIndex),
                                           20.0f),
            20.0f,
            20.0f,
            12000.0f);
        preparedParams.ownedControls.delayHighCutHz = clampPreparedFloat(
            readPreparedStateFloatProperty(payload.parameterState,
                                           "stripDelayHighCut" + juce::String(stripIndex),
                                           12000.0f),
            12000.0f,
            200.0f,
            20000.0f);
        preparedParams.ownedControls.delayMode = static_cast<EnhancedAudioStrip::DelayMode>(
            clampPreparedInt(readPreparedStateIntProperty(payload.parameterState,
                                                          "stripDelayMode" + juce::String(stripIndex),
                                                          0),
                             0,
                             2,
                             0));
        preparedParams.pitchSemitones = clampPreparedFloat(
            readPreparedStateFloatProperty(payload.parameterState,
                                           "stripPitch" + juce::String(stripIndex),
                                           static_cast<float>(stripXml->getDoubleAttribute("pitchShift", 0.0))),
            0.0f,
            -24.0f,
            24.0f);
        preparedParams.sliceLength = clampPreparedFloat(
            readPreparedStateFloatProperty(payload.parameterState,
                                           "stripSliceLength" + juce::String(stripIndex),
                                           static_cast<float>(stripXml->getDoubleAttribute("loopSliceLength", 1.0))),
            1.0f,
            0.0f,
            1.0f);
        preparedParams.pitchControlMode = clampPreparedInt(
            readPreparedStateIntProperty(payload.parameterState,
                                         "stripPitchControlMode" + juce::String(stripIndex),
                                         0),
            0,
            5,
            0);
        preparedParams.tempoMatchMode = clampPreparedInt(
            readPreparedStateIntProperty(payload.parameterState,
                                         "stripTempoMatchMode" + juce::String(stripIndex),
                                         0),
            0,
            2,
            0);
        preparedParams.duckEnabled = readPreparedStateBoolProperty(payload.parameterState,
                                                                   "stripDuckEnabled" + juce::String(stripIndex),
                                                                   false);
        preparedParams.duckSource = readPreparedStateIntProperty(payload.parameterState,
                                                                 "stripDuckSource" + juce::String(stripIndex),
                                                                 0);
        preparedParams.duckThresholdDb = readPreparedStateFloatProperty(payload.parameterState,
                                                                        "stripDuckThreshold" + juce::String(stripIndex),
                                                                        -24.0f);
        preparedParams.duckRatio = readPreparedStateFloatProperty(payload.parameterState,
                                                                  "stripDuckRatio" + juce::String(stripIndex),
                                                                  4.0f);
        preparedParams.duckAttackMs = readPreparedStateFloatProperty(payload.parameterState,
                                                                     "stripDuckAttack" + juce::String(stripIndex),
                                                                     10.0f);
        preparedParams.duckReleaseMs = readPreparedStateFloatProperty(payload.parameterState,
                                                                      "stripDuckRelease" + juce::String(stripIndex),
                                                                      180.0f);
        preparedParams.duckGainCompDb = readPreparedStateFloatProperty(payload.parameterState,
                                                                       "stripDuckGainComp" + juce::String(stripIndex),
                                                                       0.0f);
        preparedParams.duckFollowMaster = readPreparedStateBoolProperty(payload.parameterState,
                                                                        "stripDuckFollowMaster" + juce::String(stripIndex),
                                                                        false);

        for (int modSlot = 0; modSlot < ModernAudioEngine::NumModSequencers; ++modSlot)
        {
            auto& lane = stripState.modLanes[static_cast<size_t>(modSlot)];
            auto slotKey = [modSlot](const juce::String& suffix)
            {
                if (modSlot == 0)
                    return "mod" + suffix;
                return "mod" + juce::String(modSlot + 1) + suffix;
            };

            lane.target = sanitizeModPerformanceTarget(static_cast<ModernAudioEngine::ModTarget>(
                stripXml->getIntAttribute(slotKey("Target"),
                                          static_cast<int>(ModernAudioEngine::defaultModTargetForSlot(modSlot)))));
            lane.bipolar = stripXml->getBoolAttribute(slotKey("Bipolar"), false);
            lane.curveMode = stripXml->getBoolAttribute(slotKey("CurveMode"), false);
            lane.depth = clampPreparedFloat(stripXml->getDoubleAttribute(slotKey("Depth"), 1.0),
                                            1.0f,
                                            0.0f,
                                            1.0f);
            lane.rate = clampPreparedFloat(stripXml->getDoubleAttribute(slotKey("Rate"), 1.0),
                                           1.0f,
                                           0.125f,
                                           4.0f);
            lane.transportMode = static_cast<ModernAudioEngine::ModTransportMode>(clampPreparedInt(
                stripXml->getIntAttribute(slotKey("TransportMode"),
                                          static_cast<int>(ModernAudioEngine::ModTransportMode::Free)),
                0,
                static_cast<int>(ModernAudioEngine::ModTransportMode::Scene),
                static_cast<int>(ModernAudioEngine::ModTransportMode::Free)));
            lane.offset = clampPreparedInt(stripXml->getIntAttribute(slotKey("Offset"), 0), -127, 127, 0);
            lane.lengthBars = clampPreparedInt(stripXml->getIntAttribute(slotKey("LengthBars"), 1), 1, 8, 1);
            lane.editPage = clampPreparedInt(stripXml->getIntAttribute(slotKey("EditPage"), 0), 0, 7, 0);
            lane.smoothingMs = clampPreparedFloat(stripXml->getDoubleAttribute(slotKey("SmoothMs"), 0.0),
                                                  0.0f,
                                                  0.0f,
                                                  250.0f);
            lane.curveBend = clampPreparedFloat(stripXml->getDoubleAttribute(slotKey("CurveBend"), 0.0),
                                                0.0f,
                                                -1.0f,
                                                1.0f);
            lane.curveShape = static_cast<ModernAudioEngine::ModCurveShape>(
                clampPreparedInt(stripXml->getIntAttribute(slotKey("CurveShape"), 0), 0, 4, 0));
            lane.pitchScaleQuantize = stripXml->getBoolAttribute(slotKey("PitchScaleQuantize"), false);
            lane.pitchScale = static_cast<ModernAudioEngine::PitchScale>(
                clampPreparedInt(stripXml->getIntAttribute(slotKey("PitchScale"), 0), 0, 4, 0));
            decodePreparedModSteps(stripXml->getStringAttribute(slotKey("Steps")), lane.steps);
            lane.stepSubdivisions.fill(1);
            lane.stepEndValues = lane.steps;
            lane.stepCurveShapes.fill(lane.curveShape);
            const auto subdivisionsText = stripXml->getStringAttribute(slotKey("StepSubdivisions"));
            const auto endValuesText = stripXml->getStringAttribute(slotKey("StepEndValues"));
            const auto curveShapesText = stripXml->getStringAttribute(slotKey("StepCurveShapes"));
            if (subdivisionsText.isNotEmpty())
                decodePreparedIntArrayCsv(subdivisionsText, lane.stepSubdivisions);
            if (endValuesText.isNotEmpty())
                decodePreparedFloatArrayCsv(endValuesText, lane.stepEndValues);
            if (curveShapesText.isNotEmpty())
            {
                std::array<int, ModernAudioEngine::ModTotalSteps> curveShapeIndexes{};
                curveShapeIndexes.fill(static_cast<int>(lane.curveShape));
                decodePreparedIntArrayCsv(curveShapesText, curveShapeIndexes);
                for (int stepIndex = 0; stepIndex < ModernAudioEngine::ModTotalSteps; ++stepIndex)
                {
                    lane.stepCurveShapes[static_cast<size_t>(stepIndex)] =
                        static_cast<ModernAudioEngine::ModCurveShape>(juce::jlimit(
                            0,
                            static_cast<int>(ModernAudioEngine::ModCurveShape::Square),
                            curveShapeIndexes[static_cast<size_t>(stepIndex)]));
                }
            }
        }
    }

    if (auto* groupsXml = presetXml.getChildByName("Groups"))
    {
        for (auto* groupXml : groupsXml->getChildIterator())
        {
            if (groupXml->getTagName() != "Group")
                continue;

            const int groupIndex = groupXml->getIntAttribute("index", -1);
            if (groupIndex < 0 || groupIndex >= ModernAudioEngine::MaxGroups)
                continue;

            auto& groupState = payload.groupStates[static_cast<size_t>(groupIndex)];
            groupState.volume = clampPreparedFloat(groupXml->getDoubleAttribute("volume", 1.0),
                                                   1.0f,
                                                   0.0f,
                                                   1.0f);
            groupState.muted = groupXml->getBoolAttribute("muted", false);
        }
    }

    if (auto* patternsXml = presetXml.getChildByName("Patterns"))
    {
        for (auto* patternXml : patternsXml->getChildIterator())
        {
            if (patternXml->getTagName() != "Pattern")
                continue;

            const int patternIndex = patternXml->getIntAttribute("index", -1);
            if (patternIndex < 0 || patternIndex >= ModernAudioEngine::MaxPatterns)
                continue;

            auto& patternState = payload.patternStates[static_cast<size_t>(patternIndex)];
            patternState.present = true;
            patternState.playing = patternXml->getBoolAttribute("isPlaying", false);
            patternState.lengthBeats = patternXml->getIntAttribute("lengthBeats", 4);
            for (auto* eventXml : patternXml->getChildIterator())
            {
                if (eventXml->getTagName() != "Event")
                    continue;

                PatternRecorder::Event eventState{};
                eventState.stripIndex = eventXml->getIntAttribute("strip", 0);
                eventState.column = eventXml->getIntAttribute("column", 0);
                eventState.sampleSliceId = eventXml->getIntAttribute("sampleSliceId", -1);
                eventState.sampleStartSample = eventXml->getStringAttribute("sampleStartSample", "-1").getLargeIntValue();
                eventState.time = eventXml->getDoubleAttribute("time", 0.0);
                eventState.isNoteOn = eventXml->getBoolAttribute("noteOn", true);
                eventState.type = static_cast<PatternRecorder::EventType>(
                    eventXml->getIntAttribute("eventType",
                                              static_cast<int>(PatternRecorder::EventType::Note)));
                eventState.controlMode = eventXml->getIntAttribute("controlMode", -1);
                eventState.controlRow = eventXml->getIntAttribute("controlRow", -1);
                patternState.events.push_back(eventState);
            }
        }
    }

    if (auto* scenePerformanceXml = presetXml.getChildByName("ScenePerformanceData"))
    {
        const auto encoded = scenePerformanceXml->getAllSubText().trim();
        if (encoded.isNotEmpty())
            juce::ignoreUnused(payload.scenePerformanceStateData.fromBase64Encoding(encoded));
    }

    return true;
}

bool MlrVSTAudioProcessor::buildPreparedSceneSwitchPayload(PreparedSceneSwitchPayload& payload,
                                                           int mainPresetIndex,
                                                           int sceneSlot,
                                                           bool sequenceDriven,
                                                           int sequenceStepIndex)
{
    payload = {};

    const int safeMainPresetIndex = juce::jlimit(0, MaxPresetSlots - 1, mainPresetIndex);
    const int safeSceneSlot = juce::jlimit(0, SceneSlots - 1, sceneSlot);
    const auto* storedSceneState = getStoredSceneSlotState(safeMainPresetIndex, safeSceneSlot);
    if (storedSceneState == nullptr || storedSceneState->preparedSwitchPayloadTemplate == nullptr)
        return false;

    auto clonedTemplate = clonePreparedSceneSwitchPayloadTemplate(*storedSceneState->preparedSwitchPayloadTemplate);
    if (clonedTemplate == nullptr)
        return false;

    payload = std::move(*clonedTemplate);
    payload.mainPresetIndex = safeMainPresetIndex;
    payload.sceneSlot = safeSceneSlot;
    payload.sequenceDriven = sequenceDriven;
    payload.sequenceStepIndex = sequenceStepIndex;
    payload.switchSerial = 0;

    for (int stripIndex = 0; stripIndex < MaxStrips; ++stripIndex)
    {
        auto& stripPayload = payload.stripAudioPayloads[static_cast<size_t>(stripIndex)];
        juce::ignoreUnused(tryBuildPreparedSceneStripAudioPayload(
            stripIndex,
            payload.stripStates[static_cast<size_t>(stripIndex)],
            stripPayload));
    }

    return true;
}

void MlrVSTAudioProcessor::cachePreparedSceneAudioFilePayload(const juce::File& file,
                                                              const PreparedSceneStripAudioPayload& payload)
{
    if (!payload.valid)
        return;

    const auto filePath = file.getFullPathName().trim();
    if (filePath.isEmpty()
        || payload.audioBuffer.getNumSamples() <= 0
        || payload.audioBuffer.getNumChannels() <= 0
        || payload.sourceSampleRate <= 0.0
        || estimateAudioBufferStorageBytes(payload.audioBuffer) > kMaxSceneFileAudioCacheEntryBytes)
    {
        return;
    }

    SceneFileAudioCacheEntry updatedEntry;
    updatedEntry.filePath = filePath;
    updatedEntry.fileSize = safeFileExistsAsFile(file) ? file.getSize() : -1;
    updatedEntry.fileModificationTimeMs = safeFileExistsAsFile(file)
        ? file.getLastModificationTime().toMilliseconds()
        : 0;
    updatedEntry.audioBuffer.makeCopyOf(payload.audioBuffer, true);
    updatedEntry.sourceSampleRate = payload.sourceSampleRate;
    updatedEntry.transientSlices = payload.transientSlices;
    updatedEntry.rmsMap = payload.rmsMap;
    updatedEntry.zeroCrossMap = payload.zeroCrossMap;
    updatedEntry.analysisSampleCount = payload.analysisSampleCount;

    const juce::ScopedLock lock(sceneFileAudioCacheLock);
    updatedEntry.useCounter = ++sceneFileAudioCacheUseCounter;

    auto existing = std::find_if(sceneFileAudioCache.begin(),
                                 sceneFileAudioCache.end(),
                                 [&](const SceneFileAudioCacheEntry& entry)
                                 {
                                     return entry.filePath == filePath;
                                 });
    if (existing != sceneFileAudioCache.end())
    {
        *existing = std::move(updatedEntry);
        return;
    }

    if (sceneFileAudioCache.size() >= kMaxSceneFileAudioCacheEntries)
    {
        const auto lru = std::min_element(sceneFileAudioCache.begin(),
                                          sceneFileAudioCache.end(),
                                          [](const SceneFileAudioCacheEntry& a, const SceneFileAudioCacheEntry& b)
                                          {
                                              return a.useCounter < b.useCounter;
                                          });
        if (lru != sceneFileAudioCache.end())
            *lru = std::move(updatedEntry);
    }
    else
    {
        sceneFileAudioCache.push_back(std::move(updatedEntry));
    }
}

void MlrVSTAudioProcessor::applySceneMotionStateToTargetEngine(const ScenePerformanceRecorder& recorder,
                                                               int sceneSlot,
                                                               ModernAudioEngine& engine)
{
    const int safeSceneSlot = juce::jlimit(0, ScenePerformanceRecorder::MaxSceneSlots - 1, sceneSlot);
    if (!recorder.hasMotionState(safeSceneSlot))
        return;

    for (int stripIndex = 0; stripIndex < MaxStrips; ++stripIndex)
    {
        const auto stripState = recorder.getMotionStripState(safeSceneSlot, stripIndex);

        for (int slot = 0; slot < ModernAudioEngine::NumModSequencers; ++slot)
        {
            const auto& lane = stripState.lanes[static_cast<size_t>(slot)];
            engine.setModSequencerSlot(stripIndex, slot);
            engine.setModTarget(stripIndex, lane.target);
            engine.setModBipolar(stripIndex,
                                 lane.bipolar,
                                 ModernAudioEngine::ModBipolarToggleMode::Reinterpret);
            engine.setModCurveMode(stripIndex, lane.curveMode);
            engine.setModDepth(stripIndex, lane.depth);
            engine.setModRate(stripIndex, lane.rate);
            engine.setModTransportMode(stripIndex, lane.transportMode);
            engine.setModOffset(stripIndex, lane.offset);
            engine.setModLengthBars(stripIndex, lane.lengthBars);
            engine.setModEditPage(stripIndex, lane.editPage);
            engine.setModSmoothingMs(stripIndex, lane.smoothingMs);
            engine.setModCurveBend(stripIndex, lane.curveBend);
            engine.setModCurveShape(stripIndex, lane.curveShape);
            engine.setModPitchScaleQuantize(stripIndex, lane.pitchScaleQuantize);
            engine.setModPitchScale(stripIndex, lane.pitchScale);

            for (int stepIndex = 0; stepIndex < ModernAudioEngine::ModTotalSteps; ++stepIndex)
            {
                const auto index = static_cast<size_t>(stepIndex);
                engine.setModStepValueAbsolute(stripIndex, stepIndex, lane.steps[index]);
                engine.setModStepShapeAbsolute(stripIndex,
                                               stepIndex,
                                               lane.stepSubdivisions[index],
                                               lane.stepEndValues[index]);
                engine.setModStepCurveShapeAbsolute(stripIndex,
                                                    stepIndex,
                                                    lane.stepCurveShapes[index]);
            }
        }

        engine.setModSequencerSlot(stripIndex, stripState.activeSlot);
    }
}

MlrVSTAudioProcessor::PreparedSceneTimingState
MlrVSTAudioProcessor::capturePreparedSceneTimingState(int sceneSlot) const
{
    const int safeSceneSlot = juce::jlimit(0, SceneSlots - 1, sceneSlot);
    PreparedSceneTimingState state;
    state.valid = true;
    state.repeatCount = getSceneRepeatCount(safeSceneSlot);
    state.lengthModeIndex = static_cast<int>(getSceneLengthMode(safeSceneSlot));
    state.manualBars = getSceneManualBars(safeSceneSlot);
    state.anchorStrip = getSceneAnchorStrip(safeSceneSlot);
    return state;
}

void MlrVSTAudioProcessor::applyPreparedSceneTimingState(int sceneSlot,
                                                         const PreparedSceneTimingState& state)
{
    const int safeSceneSlot = juce::jlimit(0, SceneSlots - 1, sceneSlot);
    if (!state.valid)
    {
        setSceneRepeatCount(safeSceneSlot, 1);
        setSceneLengthMode(safeSceneSlot, SceneLengthMode::ManualBars);
        setSceneLengthCount(safeSceneSlot, 4);
        setSceneAnchorStrip(safeSceneSlot, 0);
        return;
    }

    setSceneRepeatCount(safeSceneSlot, juce::jmax(1, state.repeatCount));
    setSceneLengthMode(safeSceneSlot,
                       static_cast<SceneLengthMode>(juce::jlimit(
                           0,
                           static_cast<int>(SceneLengthMode::ManualBars),
                           state.lengthModeIndex)));
    setSceneManualBars(safeSceneSlot, juce::jmax(1, state.manualBars));
    setSceneAnchorStrip(safeSceneSlot, juce::jlimit(0, MaxStrips - 1, state.anchorStrip));
}

bool MlrVSTAudioProcessor::capturePreparedSceneSwitchPayloadTemplate(PreparedSceneSwitchPayload& payload,
                                                                    int mainPresetIndex,
                                                                    int sceneSlot)
{
    if (audioEngine == nullptr)
        return false;

    payload = {};
    payload.mainPresetIndex = juce::jlimit(0, MaxPresetSlots - 1, mainPresetIndex);
    payload.sceneSlot = juce::jlimit(0, SceneSlots - 1, sceneSlot);
    payload.sceneTimingState = capturePreparedSceneTimingState(payload.sceneSlot);
    payload.scenePerformanceStateData = createScenePerformanceStateData(payload.sceneSlot);

    auto parameterState = parameters.copyState();
    stripPersistentGlobalControlsFromState(parameterState);
    payload.parameterState = parameterState;

    for (int stripIndex = 0; stripIndex < MaxStrips; ++stripIndex)
    {
        auto* strip = audioEngine->getStrip(stripIndex);
        if (strip == nullptr)
            continue;

        const auto playMode = strip->getPlayMode();
        if (!(strip->hasAudio() || playMode == EnhancedAudioStrip::PlayMode::Sample))
            continue;

        auto& stripState = payload.stripStates[static_cast<size_t>(stripIndex)];
        stripState.present = true;
        stripState.restorePlaying = strip->isPlayingForPresetSave();
        stripState.playMode = playMode;
        stripState.loopStart = strip->getLoopStart();
        stripState.loopEnd = strip->getLoopEnd();
        stripState.playbackColumn = strip->getCurrentColumn();
        stripState.ppqTimelineAnchored = strip->isPpqTimelineAnchored();
        stripState.ppqTimelineOffsetBeats = strip->getPpqTimelineOffsetBeats();
        stripState.directionMode = strip->getDirectionMode();
        stripState.reverse = strip->isReversed();
        stripState.groupId = strip->getGroup();
        stripState.beatsPerLoop = strip->getBeatsPerLoop();
        stripState.scratchAmount = strip->getScratchAmount();
        stripState.transientSliceMode = strip->isTransientSliceMode();
        stripState.recordingBars = strip->getRecordingBars();
        stripState.filterEnabled = strip->isFilterEnabled();
        stripState.filterFrequency = strip->getFilterFrequency();
        stripState.filterResonance = strip->getFilterResonance();
        stripState.filterMorph = strip->getFilterMorph();
        stripState.filterAlgorithm = strip->getFilterAlgorithm();
        stripState.swingAmount = strip->getSwingAmount();
        stripState.gateAmount = strip->getGateAmount();
        stripState.gateSpeed = strip->getGateSpeed();
        stripState.gateEnvelope = strip->getGateEnvelope();
        stripState.gateShape = strip->getGateShape();
        stripState.stepPatternSteps = strip->getStepPatternLengthSteps();
        stripState.stepViewPage = strip->getStepPage();
        stripState.stepCurrent = strip->currentStep;
        stripState.stepPattern = strip->stepPattern;
        stripState.stepSubdivisions = strip->stepSubdivisions;
        stripState.stepSubdivisionStartVelocity = strip->stepSubdivisionStartVelocity;
        stripState.stepSubdivisionRepeatVelocity = strip->stepSubdivisionRepeatVelocity;
        stripState.stepProbability = strip->stepProbability;
        stripState.stepAttackMs = strip->getStepEnvelopeAttackMs();
        stripState.stepDecayMs = strip->getStepEnvelopeDecayMs();
        stripState.stepReleaseMs = strip->getStepEnvelopeReleaseMs();
        stripState.grainSizeMs = strip->getGrainSizeMs();
        stripState.grainDensity = strip->getGrainDensity();
        stripState.grainPitch = strip->getGrainPitch();
        stripState.grainPitchJitter = strip->getGrainPitchJitter();
        stripState.grainSpread = strip->getGrainSpread();
        stripState.grainJitter = strip->getGrainJitter();
        stripState.grainPositionJitter = strip->getGrainPositionJitter();
        stripState.grainRandomDepth = strip->getGrainRandomDepth();
        stripState.grainArpDepth = strip->getGrainArpDepth();
        stripState.grainCloudDepth = strip->getGrainCloudDepth();
        stripState.grainEmitterDepth = strip->getGrainEmitterDepth();
        stripState.grainEnvelope = strip->getGrainEnvelope();
        stripState.grainShape = strip->getGrainShape();
        stripState.grainArpMode = strip->getGrainArpMode();
        stripState.grainTempoSync = strip->isGrainTempoSyncEnabled();
        stripState.loopPitchState = capturePreparedSceneLoopPitchState(stripIndex);

        const auto samplePath = currentStripFiles[static_cast<size_t>(stripIndex)].getFullPathName().trim();
        stripState.hasStoredSamplePath = isValidPreparedStoredSamplePath(samplePath);
        stripState.storedSampleFile = stripState.hasStoredSamplePath ? juce::File(samplePath) : juce::File();
        stripState.recentLoopDirectory = recentLoopDirectories[static_cast<size_t>(stripIndex)];
        stripState.recentStepDirectory = recentStepDirectories[static_cast<size_t>(stripIndex)];
        stripState.recentFlipDirectory = recentFlipDirectories[static_cast<size_t>(stripIndex)];

        if (!stripState.hasStoredSamplePath)
        {
            if (const auto* audioBuffer = strip->getAudioBuffer();
                audioBuffer != nullptr
                && canEmbedSceneAudioBuffer(*audioBuffer))
            {
                juce::ignoreUnused(encodeBufferAsWavBase64(*audioBuffer,
                                                           strip->getSourceSampleRate(),
                                                           stripState.embeddedSampleWavBase64));
            }
        }

        if (strip->hasSampleAnalysisCache())
        {
            stripState.analysisSampleCount = strip->getAnalysisSampleCount();
            stripState.analysisTransientSlices = strip->getCachedTransientSliceSamples();
            stripState.analysisRmsMap = strip->getCachedRmsMap();
            stripState.analysisZeroCrossMap = strip->getCachedZeroCrossMap();
        }

        if (playMode == EnhancedAudioStrip::PlayMode::Sample)
        {
            if (auto* engine = getSampleModeEngine(stripIndex, false))
            {
                stripState.hasFlipState = true;
                stripState.flipState = engine->capturePersistentState();
                stripState.embeddedFlipSampleBase64 = createEmbeddedFlipSampleData(stripIndex);
            }
        }

        auto& preparedParams = stripState.parameterState;
        preparedParams.valid = true;
        preparedParams.ownedControls.usesGrainPlaybackSpeed =
            stripState.playMode == EnhancedAudioStrip::PlayMode::Grain;

        if (auto* param = parameters.getRawParameterValue("stripVolume" + juce::String(stripIndex)))
            preparedParams.ownedControls.volume = param->load(std::memory_order_acquire);
        else
            preparedParams.ownedControls.volume = strip->getVolume();

        if (auto* param = parameters.getRawParameterValue("stripTrimDb" + juce::String(stripIndex)))
            preparedParams.ownedControls.trimDb = param->load(std::memory_order_acquire);

        if (auto* param = parameters.getRawParameterValue("stripPan" + juce::String(stripIndex)))
            preparedParams.ownedControls.pan = param->load(std::memory_order_acquire);
        else
            preparedParams.ownedControls.pan = strip->getPan();

        const float speedControl = stripSpeedParams[static_cast<size_t>(stripIndex)] != nullptr
            ? stripSpeedParams[static_cast<size_t>(stripIndex)]->load(std::memory_order_acquire)
            : juce::jlimit(0.0f, 8.0f, strip->getPlayheadSpeedRatio());
        preparedParams.ownedControls.playbackSpeed =
            PlayheadSpeedQuantizer::grainPlaybackSpeedFromControl(speedControl);
        preparedParams.ownedControls.playheadSpeedRatio =
            PlayheadSpeedQuantizer::quantizeRatio(juce::jlimit(0.125f, 8.0f, speedControl));
        preparedParams.ownedControls.filterEnabled = stripFilterEnabledParams[static_cast<size_t>(stripIndex)] != nullptr
            ? (stripFilterEnabledParams[static_cast<size_t>(stripIndex)]->load(std::memory_order_acquire) > 0.5f)
            : strip->isFilterEnabled();
        preparedParams.ownedControls.filterFrequency = stripFilterFrequencyParams[static_cast<size_t>(stripIndex)] != nullptr
            ? stripFilterFrequencyParams[static_cast<size_t>(stripIndex)]->load(std::memory_order_acquire)
            : strip->getFilterFrequency();
        preparedParams.ownedControls.filterResonance = stripFilterResonanceParams[static_cast<size_t>(stripIndex)] != nullptr
            ? stripFilterResonanceParams[static_cast<size_t>(stripIndex)]->load(std::memory_order_acquire)
            : strip->getFilterResonance();
        preparedParams.ownedControls.filterMorph = stripFilterMorphParams[static_cast<size_t>(stripIndex)] != nullptr
            ? stripFilterMorphParams[static_cast<size_t>(stripIndex)]->load(std::memory_order_acquire)
            : strip->getFilterMorph();
        preparedParams.ownedControls.filterAlgorithm =
            static_cast<EnhancedAudioStrip::FilterAlgorithm>(
                stripFilterAlgorithmParams[static_cast<size_t>(stripIndex)] != nullptr
                    ? juce::roundToInt(
                          stripFilterAlgorithmParams[static_cast<size_t>(stripIndex)]->load(std::memory_order_acquire))
                    : static_cast<int>(strip->getFilterAlgorithm()));
        preparedParams.ownedControls.delayMix = stripDelayMixParams[static_cast<size_t>(stripIndex)] != nullptr
            ? stripDelayMixParams[static_cast<size_t>(stripIndex)]->load(std::memory_order_acquire)
            : 0.0f;
        preparedParams.ownedControls.delayTime = stripDelayTimeParams[static_cast<size_t>(stripIndex)] != nullptr
            ? stripDelayTimeParams[static_cast<size_t>(stripIndex)]->load(std::memory_order_acquire)
            : 1.0f;
        preparedParams.ownedControls.delaySyncEnabled = stripDelaySyncParams[static_cast<size_t>(stripIndex)] != nullptr
            ? (stripDelaySyncParams[static_cast<size_t>(stripIndex)]->load(std::memory_order_acquire) > 0.5f)
            : true;
        preparedParams.ownedControls.delayFeedback = stripDelayFeedbackParams[static_cast<size_t>(stripIndex)] != nullptr
            ? stripDelayFeedbackParams[static_cast<size_t>(stripIndex)]->load(std::memory_order_acquire)
            : 0.35f;
        preparedParams.ownedControls.delayLowCutHz = stripDelayLowCutParams[static_cast<size_t>(stripIndex)] != nullptr
            ? stripDelayLowCutParams[static_cast<size_t>(stripIndex)]->load(std::memory_order_acquire)
            : 20.0f;
        preparedParams.ownedControls.delayHighCutHz = stripDelayHighCutParams[static_cast<size_t>(stripIndex)] != nullptr
            ? stripDelayHighCutParams[static_cast<size_t>(stripIndex)]->load(std::memory_order_acquire)
            : 12000.0f;
        preparedParams.ownedControls.delayMode =
            static_cast<EnhancedAudioStrip::DelayMode>(
                stripDelayModeParams[static_cast<size_t>(stripIndex)] != nullptr
                    ? juce::roundToInt(
                          stripDelayModeParams[static_cast<size_t>(stripIndex)]->load(std::memory_order_acquire))
                    : 0);
        preparedParams.pitchSemitones = stripPitchParams[static_cast<size_t>(stripIndex)] != nullptr
            ? stripPitchParams[static_cast<size_t>(stripIndex)]->load(std::memory_order_acquire)
            : strip->getPitchShift();
        preparedParams.sliceLength = stripSliceLengthParams[static_cast<size_t>(stripIndex)] != nullptr
            ? stripSliceLengthParams[static_cast<size_t>(stripIndex)]->load(std::memory_order_acquire)
            : strip->getLoopSliceLength();
        preparedParams.pitchControlMode = stripPitchControlModeParams[static_cast<size_t>(stripIndex)] != nullptr
            ? juce::roundToInt(stripPitchControlModeParams[static_cast<size_t>(stripIndex)]->load(std::memory_order_acquire))
            : 0;
        preparedParams.tempoMatchMode = stripTempoMatchModeParams[static_cast<size_t>(stripIndex)] != nullptr
            ? juce::roundToInt(stripTempoMatchModeParams[static_cast<size_t>(stripIndex)]->load(std::memory_order_acquire))
            : 0;
        preparedParams.duckEnabled = stripDuckEnabledParams[static_cast<size_t>(stripIndex)] != nullptr
            ? (stripDuckEnabledParams[static_cast<size_t>(stripIndex)]->load(std::memory_order_acquire) > 0.5f)
            : false;
        preparedParams.duckSource = stripDuckSourceParams[static_cast<size_t>(stripIndex)] != nullptr
            ? juce::roundToInt(stripDuckSourceParams[static_cast<size_t>(stripIndex)]->load(std::memory_order_acquire))
            : 0;
        preparedParams.duckThresholdDb = stripDuckThresholdParams[static_cast<size_t>(stripIndex)] != nullptr
            ? stripDuckThresholdParams[static_cast<size_t>(stripIndex)]->load(std::memory_order_acquire)
            : -24.0f;
        preparedParams.duckRatio = stripDuckRatioParams[static_cast<size_t>(stripIndex)] != nullptr
            ? stripDuckRatioParams[static_cast<size_t>(stripIndex)]->load(std::memory_order_acquire)
            : 4.0f;
        preparedParams.duckAttackMs = stripDuckAttackParams[static_cast<size_t>(stripIndex)] != nullptr
            ? stripDuckAttackParams[static_cast<size_t>(stripIndex)]->load(std::memory_order_acquire)
            : 10.0f;
        preparedParams.duckReleaseMs = stripDuckReleaseParams[static_cast<size_t>(stripIndex)] != nullptr
            ? stripDuckReleaseParams[static_cast<size_t>(stripIndex)]->load(std::memory_order_acquire)
            : 180.0f;
        preparedParams.duckGainCompDb = stripDuckGainCompParams[static_cast<size_t>(stripIndex)] != nullptr
            ? stripDuckGainCompParams[static_cast<size_t>(stripIndex)]->load(std::memory_order_acquire)
            : 0.0f;
        preparedParams.duckFollowMaster = stripDuckFollowMasterParams[static_cast<size_t>(stripIndex)] != nullptr
            ? (stripDuckFollowMasterParams[static_cast<size_t>(stripIndex)]->load(std::memory_order_acquire) > 0.5f)
            : false;
        stripState.pitchShift = preparedParams.pitchSemitones;

        const int originalModSlot = audioEngine->getModSequencerSlot(stripIndex);
        for (int modSlot = 0; modSlot < ModernAudioEngine::NumModSequencers; ++modSlot)
        {
            auto& lane = stripState.modLanes[static_cast<size_t>(modSlot)];
            audioEngine->setModSequencerSlot(stripIndex, modSlot);
            const auto mod = audioEngine->getModSequencerState(stripIndex);
            lane.target = sanitizeModPerformanceTarget(mod.target);
            lane.bipolar = mod.bipolar;
            lane.curveMode = mod.curveMode;
            lane.depth = mod.depth;
            lane.rate = mod.rate;
            lane.transportMode = static_cast<ModernAudioEngine::ModTransportMode>(juce::jlimit(
                0,
                static_cast<int>(ModernAudioEngine::ModTransportMode::Scene),
                mod.transportMode));
            lane.offset = mod.offset;
            lane.lengthBars = mod.lengthBars;
            lane.editPage = mod.editPage;
            lane.smoothingMs = mod.smoothingMs;
            lane.curveBend = mod.curveBend;
            lane.curveShape = static_cast<ModernAudioEngine::ModCurveShape>(juce::jlimit(0, 4, mod.curveShape));
            lane.pitchScaleQuantize = mod.pitchScaleQuantize;
            lane.pitchScale = static_cast<ModernAudioEngine::PitchScale>(juce::jlimit(0, 4, mod.pitchScale));
            for (int stepIndex = 0; stepIndex < ModernAudioEngine::ModTotalSteps; ++stepIndex)
            {
                const auto stepArrayIndex = static_cast<size_t>(stepIndex);
                lane.steps[stepArrayIndex] = audioEngine->getModStepValueAbsolute(stripIndex, stepIndex);
                lane.stepSubdivisions[stepArrayIndex] = juce::jlimit(
                    1,
                    ModernAudioEngine::ModMaxStepSubdivisions,
                    audioEngine->getModStepSubdivisionAbsolute(stripIndex, stepIndex));
                lane.stepEndValues[stepArrayIndex] = juce::jlimit(
                    0.0f,
                    1.0f,
                    audioEngine->getModStepEndValueAbsolute(stripIndex, stepIndex));
                lane.stepCurveShapes[stepArrayIndex] = audioEngine->getModStepCurveShapeAbsolute(stripIndex, stepIndex);
            }
        }
        audioEngine->setModSequencerSlot(stripIndex, originalModSlot);
        stripState.activeModSlot = originalModSlot;
    }

    for (int groupIndex = 0; groupIndex < ModernAudioEngine::MaxGroups; ++groupIndex)
    {
        if (auto* group = audioEngine->getGroup(groupIndex))
        {
            auto& groupState = payload.groupStates[static_cast<size_t>(groupIndex)];
            groupState.volume = group->getVolume();
            groupState.muted = group->isMuted();
        }
    }

    for (int patternIndex = 0; patternIndex < ModernAudioEngine::MaxPatterns; ++patternIndex)
    {
        if (auto* pattern = audioEngine->getPattern(patternIndex))
        {
            auto& patternState = payload.patternStates[static_cast<size_t>(patternIndex)];
            patternState.present = true;
            patternState.playing = pattern->isPlaying();
            patternState.lengthBeats = pattern->getLengthInBeats();
            patternState.events = pattern->getEventsSnapshot();
        }
    }

    payload.snapshotPresetXml = PresetStore::createPresetXml(
        getSceneStoragePresetIndex(payload.mainPresetIndex, payload.sceneSlot),
        MaxStrips,
        audioEngine.get(),
        parameters,
        currentStripFiles.data(),
        recentLoopDirectories.data(),
        recentStepDirectories.data(),
        recentFlipDirectories.data(),
        [this](int stripIndex)
        {
            return createFlipPresetStateXml(stripIndex);
        },
        [this](int stripIndex)
        {
            return createLoopPitchPresetStateXml(stripIndex);
        },
        [this, sceneSlot]()
        {
            return createSceneChainStateXml(sceneSlot);
        },
        [this, sceneSlot]()
        {
            return createScenePerformanceStateData(sceneSlot);
        });

    if (payload.snapshotPresetXml != nullptr)
    {
        if (auto* paramsXml = payload.snapshotPresetXml->getChildByName("ParametersState"))
        {
            auto state = juce::ValueTree::fromXml(*paramsXml);
            if (state.isValid())
            {
                stripPersistentGlobalControlsFromState(state);
                auto strippedParamsXml = state.createXml();
                payload.snapshotPresetXml->removeChildElement(paramsXml, true);
                if (strippedParamsXml != nullptr)
                    payload.snapshotPresetXml->addChildElement(strippedParamsXml.release());
            }
        }
    }

    return true;
}

std::unique_ptr<juce::XmlElement> MlrVSTAudioProcessor::createSceneSnapshotPresetXml(
    const PreparedSceneSwitchPayload& payload,
    const juce::String& sceneName) const
{
    if (payload.snapshotPresetXml != nullptr && payload.snapshotPresetXml->hasTagName("mlrVSTPreset"))
    {
        auto preset = std::make_unique<juce::XmlElement>(*payload.snapshotPresetXml);
        preset->setAttribute("index", getSceneStoragePresetIndex(payload.mainPresetIndex, payload.sceneSlot));
        if (sceneName.isNotEmpty())
            preset->setAttribute("name", sceneName);
        return preset;
    }

    auto preset = std::make_unique<juce::XmlElement>("mlrVSTPreset");
    preset->setAttribute("version", "1.0");
    preset->setAttribute("index", getSceneStoragePresetIndex(payload.mainPresetIndex, payload.sceneSlot));
    if (sceneName.isNotEmpty())
        preset->setAttribute("name", sceneName);

    for (int stripIndex = 0; stripIndex < MaxStrips; ++stripIndex)
    {
        const auto& stripState = payload.stripStates[static_cast<size_t>(stripIndex)];
        if (!stripState.present)
            continue;

        auto* stripXml = preset->createNewChildElement("Strip");
        stripXml->setAttribute("index", stripIndex);
        if (stripState.hasStoredSamplePath)
            stripXml->setAttribute("samplePath", stripState.storedSampleFile.getFullPathName());
        else if (stripState.embeddedSampleWavBase64.isNotEmpty())
            stripXml->setAttribute("embeddedSampleWavBase64", stripState.embeddedSampleWavBase64);

        if (stripState.hasFlipState)
        {
            if (auto flipStateXml = stripState.flipState.createXml("FlipState"))
            {
                if (stripState.embeddedFlipSampleBase64.isNotEmpty())
                    flipStateXml->setAttribute(kEmbeddedFlipSampleAttr, stripState.embeddedFlipSampleBase64);
                stripXml->addChildElement(flipStateXml.release());
            }
        }

        if (stripState.loopPitchState.valid)
        {
            auto loopPitchXml = std::make_unique<juce::XmlElement>("LoopPitchState");
            loopPitchXml->setAttribute("role", static_cast<int>(stripState.loopPitchState.role));
            loopPitchXml->setAttribute("syncTiming", static_cast<int>(stripState.loopPitchState.syncTiming));
            loopPitchXml->setAttribute("assignedMidi", stripState.loopPitchState.assignedMidi);
            loopPitchXml->setAttribute("assignedManual", stripState.loopPitchState.assignedManual);
            loopPitchXml->setAttribute("detectedMidi", stripState.loopPitchState.detectedMidi);
            loopPitchXml->setAttribute("detectedHz", static_cast<double>(stripState.loopPitchState.detectedHz));
            loopPitchXml->setAttribute("detectedPitchConfidence",
                                       static_cast<double>(stripState.loopPitchState.detectedPitchConfidence));
            loopPitchXml->setAttribute("detectedScaleIndex", stripState.loopPitchState.detectedScaleIndex);
            loopPitchXml->setAttribute("detectedScaleConfidence",
                                       static_cast<double>(stripState.loopPitchState.detectedScaleConfidence));
            loopPitchXml->setAttribute("essentiaUsed", stripState.loopPitchState.essentiaUsed);
            stripXml->addChildElement(loopPitchXml.release());
        }

        const auto& paramState = stripState.parameterState;
        const float savedSpeedRatio = paramState.ownedControls.usesGrainPlaybackSpeed
            ? juce::jlimit(0.0f,
                           8.0f,
                           PlayheadSpeedQuantizer::grainControlValueFromPlaybackSpeed(
                               paramState.ownedControls.playbackSpeed))
            : juce::jlimit(0.0f, 8.0f, paramState.ownedControls.playheadSpeedRatio);
        const int legacyFilterType = stripState.filterMorph < 0.25f
            ? 0
            : (stripState.filterMorph < 0.75f ? 1 : 2);

        stripXml->setAttribute("volume", paramState.ownedControls.volume);
        stripXml->setAttribute("pan", paramState.ownedControls.pan);
        stripXml->setAttribute("speed", savedSpeedRatio);
        stripXml->setAttribute("loopStart", stripState.loopStart);
        stripXml->setAttribute("loopEnd", stripState.loopEnd);
        stripXml->setAttribute("playMode", static_cast<int>(stripState.playMode));
        stripXml->setAttribute("isPlaying", stripState.restorePlaying);
        stripXml->setAttribute("playbackColumn", stripState.playbackColumn);
        stripXml->setAttribute("ppqTimelineAnchored", stripState.ppqTimelineAnchored);
        stripXml->setAttribute("ppqTimelineOffsetBeats", stripState.ppqTimelineOffsetBeats);
        stripXml->setAttribute("directionMode", static_cast<int>(stripState.directionMode));
        stripXml->setAttribute("reversed", stripState.reverse);
        stripXml->setAttribute("group", stripState.groupId);
        stripXml->setAttribute("beatsPerLoop", stripState.beatsPerLoop);
        stripXml->setAttribute("scratchAmount", stripState.scratchAmount);
        stripXml->setAttribute("transientSliceMode", stripState.transientSliceMode);
        stripXml->setAttribute("loopSliceLength", paramState.sliceLength);
        stripXml->setAttribute("recentLoopDir", stripState.recentLoopDirectory.getFullPathName());
        stripXml->setAttribute("recentStepDir", stripState.recentStepDirectory.getFullPathName());
        stripXml->setAttribute("recentFlipDir", stripState.recentFlipDirectory.getFullPathName());
        if (stripState.analysisSampleCount > 0)
        {
            stripXml->setAttribute(kPreparedSceneAnalysisSampleCountAttr, stripState.analysisSampleCount);
            stripXml->setAttribute(kPreparedSceneAnalysisTransientAttr,
                                   encodePreparedIntArrayCsv(stripState.analysisTransientSlices));
            stripXml->setAttribute(kPreparedSceneAnalysisRmsAttr,
                                   encodePreparedFloatArrayCsv(stripState.analysisRmsMap));
            stripXml->setAttribute(kPreparedSceneAnalysisZeroCrossAttr,
                                   encodePreparedIntArrayCsv(stripState.analysisZeroCrossMap));
        }
        stripXml->setAttribute("pitchShift", paramState.pitchSemitones);
        stripXml->setAttribute("recordingBars", stripState.recordingBars);
        stripXml->setAttribute("filterEnabled", stripState.filterEnabled);
        stripXml->setAttribute("filterFrequency", stripState.filterFrequency);
        stripXml->setAttribute("filterResonance", stripState.filterResonance);
        stripXml->setAttribute("filterMorph", stripState.filterMorph);
        stripXml->setAttribute("filterAlgorithm", static_cast<int>(stripState.filterAlgorithm));
        stripXml->setAttribute("filterType", legacyFilterType);
        stripXml->setAttribute("swingAmount", stripState.swingAmount);
        stripXml->setAttribute("gateAmount", stripState.gateAmount);
        stripXml->setAttribute("gateSpeed", stripState.gateSpeed);
        stripXml->setAttribute("gateEnvelope", stripState.gateEnvelope);
        stripXml->setAttribute("gateShapeCurve", stripState.gateShape);
        stripXml->setAttribute("stepPatternSteps", stripState.stepPatternSteps);
        stripXml->setAttribute("stepPatternBars", juce::jmax(1, (stripState.stepPatternSteps + 15) / 16));
        stripXml->setAttribute("stepViewPage", stripState.stepViewPage);
        stripXml->setAttribute("stepCurrent", stripState.stepCurrent);
        stripXml->setAttribute("stepPatternBits", encodePreparedStepPatternBits(stripState.stepPattern));
        stripXml->setAttribute("stepSubdivisions", encodePreparedStepSubdivisions(stripState.stepSubdivisions));
        stripXml->setAttribute("stepSubdivisionStartVelocity",
                               encodePreparedStepSubdivisionRepeatVelocity(stripState.stepSubdivisionStartVelocity));
        stripXml->setAttribute("stepSubdivisionRepeatVelocity",
                               encodePreparedStepSubdivisionRepeatVelocity(stripState.stepSubdivisionRepeatVelocity));
        stripXml->setAttribute("stepProbability",
                               encodePreparedStepSubdivisionRepeatVelocity(stripState.stepProbability));
        stripXml->setAttribute("stepAttackMs", stripState.stepAttackMs);
        stripXml->setAttribute("stepDecayMs", stripState.stepDecayMs);
        stripXml->setAttribute("stepReleaseMs", stripState.stepReleaseMs);
        stripXml->setAttribute("grainSizeMs", stripState.grainSizeMs);
        stripXml->setAttribute("grainDensity", stripState.grainDensity);
        stripXml->setAttribute("grainPitch", stripState.grainPitch);
        stripXml->setAttribute("grainPitchJitter", stripState.grainPitchJitter);
        stripXml->setAttribute("grainSpread", stripState.grainSpread);
        stripXml->setAttribute("grainJitter", stripState.grainJitter);
        stripXml->setAttribute("grainPositionJitter", stripState.grainPositionJitter);
        stripXml->setAttribute("grainRandomDepth", stripState.grainRandomDepth);
        stripXml->setAttribute("grainArpDepth", stripState.grainArpDepth);
        stripXml->setAttribute("grainCloudDepth", stripState.grainCloudDepth);
        stripXml->setAttribute("grainEmitterDepth", stripState.grainEmitterDepth);
        stripXml->setAttribute("grainEnvelope", stripState.grainEnvelope);
        stripXml->setAttribute("grainShape", stripState.grainShape);
        stripXml->setAttribute("grainArpMode", stripState.grainArpMode);
        stripXml->setAttribute("grainTempoSync", stripState.grainTempoSync);
        stripXml->setAttribute("modActiveSequencer", stripState.activeModSlot);

        for (int modSlot = 0; modSlot < ModernAudioEngine::NumModSequencers; ++modSlot)
        {
            const auto& lane = stripState.modLanes[static_cast<size_t>(modSlot)];
            auto slotKey = [modSlot](const juce::String& suffix)
            {
                if (modSlot == 0)
                    return "mod" + suffix;
                return "mod" + juce::String(modSlot + 1) + suffix;
            };

            std::array<int, ModernAudioEngine::ModTotalSteps> curveShapeIndexes{};
            for (int stepIndex = 0; stepIndex < ModernAudioEngine::ModTotalSteps; ++stepIndex)
                curveShapeIndexes[static_cast<size_t>(stepIndex)] = static_cast<int>(lane.stepCurveShapes[static_cast<size_t>(stepIndex)]);

            stripXml->setAttribute(slotKey("Target"), static_cast<int>(lane.target));
            stripXml->setAttribute(slotKey("Bipolar"), lane.bipolar);
            stripXml->setAttribute(slotKey("CurveMode"), lane.curveMode);
            stripXml->setAttribute(slotKey("Depth"), lane.depth);
            stripXml->setAttribute(slotKey("Rate"), lane.rate);
            stripXml->setAttribute(slotKey("TransportMode"), static_cast<int>(lane.transportMode));
            stripXml->setAttribute(slotKey("Offset"), lane.offset);
            stripXml->setAttribute(slotKey("LengthBars"), lane.lengthBars);
            stripXml->setAttribute(slotKey("EditPage"), lane.editPage);
            stripXml->setAttribute(slotKey("SmoothMs"), lane.smoothingMs);
            stripXml->setAttribute(slotKey("CurveBend"), lane.curveBend);
            stripXml->setAttribute(slotKey("CurveShape"), static_cast<int>(lane.curveShape));
            stripXml->setAttribute(slotKey("PitchScaleQuantize"), lane.pitchScaleQuantize);
            stripXml->setAttribute(slotKey("PitchScale"), static_cast<int>(lane.pitchScale));
            stripXml->setAttribute(slotKey("Steps"), encodePreparedModSteps(lane.steps));
            stripXml->setAttribute(slotKey("StepSubdivisions"), encodePreparedIntArrayCsv(lane.stepSubdivisions));
            stripXml->setAttribute(slotKey("StepEndValues"), encodePreparedFloatArrayCsv(lane.stepEndValues));
            stripXml->setAttribute(slotKey("StepCurveShapes"), encodePreparedIntArrayCsv(curveShapeIndexes));
        }
    }

    auto* groupsXml = preset->createNewChildElement("Groups");
    for (int groupIndex = 0; groupIndex < ModernAudioEngine::MaxGroups; ++groupIndex)
    {
        const auto& groupState = payload.groupStates[static_cast<size_t>(groupIndex)];
        auto* groupXml = groupsXml->createNewChildElement("Group");
        groupXml->setAttribute("index", groupIndex);
        groupXml->setAttribute("volume", groupState.volume);
        groupXml->setAttribute("muted", groupState.muted);
    }

    auto* patternsXml = preset->createNewChildElement("Patterns");
    for (int patternIndex = 0; patternIndex < ModernAudioEngine::MaxPatterns; ++patternIndex)
    {
        const auto& patternState = payload.patternStates[static_cast<size_t>(patternIndex)];
        if (!patternState.present)
            continue;

        auto* patternXml = patternsXml->createNewChildElement("Pattern");
        patternXml->setAttribute("index", patternIndex);
        patternXml->setAttribute("lengthBeats", patternState.lengthBeats);
        patternXml->setAttribute("isPlaying", patternState.playing);
        for (const auto& event : patternState.events)
        {
            auto* eventXml = patternXml->createNewChildElement("Event");
            eventXml->setAttribute("strip", event.stripIndex);
            eventXml->setAttribute("column", event.column);
            eventXml->setAttribute("sampleSliceId", event.sampleSliceId);
            eventXml->setAttribute("sampleStartSample", juce::String(event.sampleStartSample));
            eventXml->setAttribute("time", event.time);
            eventXml->setAttribute("noteOn", event.isNoteOn);
            eventXml->setAttribute("eventType", static_cast<int>(event.type));
            eventXml->setAttribute("controlMode", event.controlMode);
            eventXml->setAttribute("controlRow", event.controlRow);
        }
    }

    if (payload.parameterState.isValid())
    {
        if (auto paramsXml = payload.parameterState.createXml())
        {
            paramsXml->setTagName("ParametersState");
            preset->addChildElement(paramsXml.release());
        }
    }

    auto* sceneTimingXml = preset->createNewChildElement("SceneChainState");
    sceneTimingXml->setAttribute("storedSceneSnapshot", true);
    sceneTimingXml->setAttribute("sceneSlot", payload.sceneSlot);
    sceneTimingXml->setAttribute("repeatCount", payload.sceneTimingState.repeatCount);
    sceneTimingXml->setAttribute("lengthMode", payload.sceneTimingState.lengthModeIndex);
    sceneTimingXml->setAttribute("manualBars", payload.sceneTimingState.manualBars);
    sceneTimingXml->setAttribute("anchorStrip", payload.sceneTimingState.anchorStrip);

    if (payload.scenePerformanceStateData.getSize() > 0)
    {
        auto* perfXml = preset->createNewChildElement("ScenePerformanceData");
        perfXml->addTextElement(payload.scenePerformanceStateData.toBase64Encoding());
    }

    return preset;
}
