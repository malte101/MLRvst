#include "SampleMode.h"

#include <cmath>
#include <limits>

namespace
{
constexpr double kInitialSliceTargetSeconds = 1.5;
constexpr int kMaxInitialSliceCount = 256;
constexpr int kDefaultSliceFadeSamples = 96;

float readInterpolatedSample(const juce::AudioBuffer<float>& buffer, int channel, double samplePosition)
{
    const int numChannels = buffer.getNumChannels();
    const int numSamples = buffer.getNumSamples();
    if (numChannels <= 0 || numSamples <= 0)
        return 0.0f;

    const int clampedChannel = juce::jlimit(0, numChannels - 1, channel);
    const float* data = buffer.getReadPointer(clampedChannel);
    const double clampedPos = juce::jlimit(0.0, static_cast<double>(juce::jmax(0, numSamples - 1)), samplePosition);
    const int indexA = static_cast<int>(clampedPos);
    const int indexB = juce::jmin(indexA + 1, numSamples - 1);
    const float frac = static_cast<float>(clampedPos - static_cast<double>(indexA));
    return juce::jmap(frac, data[indexA], data[indexB]);
}

void buildWaveformPreview(LoadedSampleData& sampleData)
{
    const auto numSamples = sampleData.audioBuffer.getNumSamples();
    const auto numChannels = sampleData.audioBuffer.getNumChannels();

    if (numSamples <= 0 || numChannels <= 0)
    {
        sampleData.previewMin.clear();
        sampleData.previewMax.clear();
        return;
    }

    const int pointCount = juce::jmin(LoadedSampleData::PreviewPointCount, juce::jmax(64, numSamples));
    sampleData.previewMin.assign(static_cast<size_t>(pointCount), 0.0f);
    sampleData.previewMax.assign(static_cast<size_t>(pointCount), 0.0f);

    for (int pointIndex = 0; pointIndex < pointCount; ++pointIndex)
    {
        const int startSample = static_cast<int>((static_cast<int64_t>(pointIndex) * numSamples) / pointCount);
        const int endSample = juce::jmax(startSample + 1,
                                         static_cast<int>((static_cast<int64_t>(pointIndex + 1) * numSamples) / pointCount));

        float minValue = 1.0f;
        float maxValue = -1.0f;

        for (int sampleIndex = startSample; sampleIndex < juce::jmin(endSample, numSamples); ++sampleIndex)
        {
            float monoSample = 0.0f;

            if (numChannels == 1)
                monoSample = sampleData.audioBuffer.getSample(0, sampleIndex);
            else
                monoSample = 0.5f * (sampleData.audioBuffer.getSample(0, sampleIndex)
                                   + sampleData.audioBuffer.getSample(1, sampleIndex));

            minValue = juce::jmin(minValue, monoSample);
            maxValue = juce::jmax(maxValue, monoSample);
        }

        if (minValue > maxValue)
        {
            minValue = 0.0f;
            maxValue = 0.0f;
        }

        sampleData.previewMin[static_cast<size_t>(pointIndex)] = minValue;
        sampleData.previewMax[static_cast<size_t>(pointIndex)] = maxValue;
    }
}

juce::String buildSliceLabel(int index)
{
    return "S" + juce::String(index + 1);
}
} // namespace

void SliceModel::clear()
{
    slices.clear();
    visibleBankIndex = 0;
}

void SliceModel::setSlices(std::vector<SampleSlice> newSlices)
{
    slices = std::move(newSlices);
    visibleBankIndex = juce::jlimit(0, juce::jmax(0, getVisibleBankCount() - 1), visibleBankIndex);
}

void SliceModel::setVisibleBankIndex(int bankIndex)
{
    visibleBankIndex = juce::jlimit(0, juce::jmax(0, getVisibleBankCount() - 1), bankIndex);
}

int SliceModel::getVisibleBankCount() const noexcept
{
    if (slices.empty())
        return 0;

    return (static_cast<int>(slices.size()) + VisibleSliceCount - 1) / VisibleSliceCount;
}

bool SliceModel::canNavigateLeft() const noexcept
{
    return visibleBankIndex > 0;
}

bool SliceModel::canNavigateRight() const noexcept
{
    return visibleBankIndex + 1 < getVisibleBankCount();
}

void SliceModel::stepVisibleBank(int delta)
{
    setVisibleBankIndex(visibleBankIndex + delta);
}

std::array<SampleSlice, SliceModel::VisibleSliceCount> SliceModel::getVisibleSlices() const
{
    std::array<SampleSlice, VisibleSliceCount> visibleSlices {};

    const int startIndex = visibleBankIndex * VisibleSliceCount;
    for (int slot = 0; slot < VisibleSliceCount; ++slot)
    {
        const int sliceIndex = startIndex + slot;
        if (sliceIndex >= 0 && sliceIndex < static_cast<int>(slices.size()))
            visibleSlices[static_cast<size_t>(slot)] = slices[static_cast<size_t>(sliceIndex)];
        else
            visibleSlices[static_cast<size_t>(slot)].label = buildSliceLabel(slot);
    }

    return visibleSlices;
}

std::vector<SampleSlice> SliceModel::buildUniformSlices(const LoadedSampleData& sampleData,
                                                        int totalSliceCount)
{
    std::vector<SampleSlice> slices;

    const auto totalSamples = sampleData.sourceLengthSamples;
    if (totalSamples <= 0)
        return slices;

    const int sliceCount = juce::jmax(1, totalSliceCount);
    slices.reserve(static_cast<size_t>(sliceCount));

    for (int index = 0; index < sliceCount; ++index)
    {
        const int64_t startSample = (totalSamples * index) / sliceCount;
        int64_t endSample = (totalSamples * (index + 1)) / sliceCount;
        if (endSample <= startSample)
            endSample = juce::jmin(totalSamples, startSample + 1);

        SampleSlice slice;
        slice.id = index;
        slice.startSample = startSample;
        slice.endSample = endSample;
        slice.normalizedStart = static_cast<float>(startSample / static_cast<double>(totalSamples));
        slice.normalizedEnd = static_cast<float>(endSample / static_cast<double>(totalSamples));
        slice.label = buildSliceLabel(index);
        slices.push_back(std::move(slice));
    }

    return slices;
}

std::vector<SampleSlice> SampleAnalysisEngine::buildInitialSlices(const LoadedSampleData& sampleData) const
{
    if (sampleData.sourceLengthSamples <= 0)
        return {};

    const auto estimatedSliceCount = static_cast<int>(std::ceil(sampleData.durationSeconds / kInitialSliceTargetSeconds));
    const int totalSliceCount = juce::jlimit(SliceModel::VisibleSliceCount,
                                             kMaxInitialSliceCount,
                                             juce::jmax(SliceModel::VisibleSliceCount, estimatedSliceCount));

    return SliceModel::buildUniformSlices(sampleData, totalSliceCount);
}

class SampleFileManager::LoadJob : public juce::ThreadPoolJob
{
public:
    LoadJob(SampleFileManager& ownerToUse,
            juce::File fileToLoad,
            int requestIdToUse,
            LoadCallback callbackToUse)
        : juce::ThreadPoolJob("SampleModeLoadJob")
        , owner(ownerToUse)
        , file(std::move(fileToLoad))
        , requestId(requestIdToUse)
        , callback(std::move(callbackToUse))
    {
    }

    JobStatus runJob() override
    {
        auto decodeResult = owner.decodeFile(file, requestId);

        if (shouldExit())
            return jobHasFinished;

        auto callbackCopy = callback;
        juce::MessageManager::callAsync([mainThreadCallback = std::move(callbackCopy),
                                         asyncResult = std::move(decodeResult)]() mutable
        {
            if (mainThreadCallback)
                mainThreadCallback(std::move(asyncResult));
        });

        return jobHasFinished;
    }

private:
    SampleFileManager& owner;
    juce::File file;
    int requestId = 0;
    LoadCallback callback;
};

SampleFileManager::SampleFileManager()
{
    formatManager.registerBasicFormats();
}

SampleFileManager::~SampleFileManager()
{
    cancelPendingLoads();
}

int SampleFileManager::loadFileAsync(const juce::File& file, LoadCallback callback)
{
    const int requestId = nextRequestId.fetch_add(1);
    threadPool.addJob(new LoadJob(*this, file, requestId, std::move(callback)), true);
    return requestId;
}

void SampleFileManager::cancelPendingLoads()
{
    threadPool.removeAllJobs(true, 10000);
}

SampleFileManager::LoadResult SampleFileManager::decodeFile(const juce::File& file, int requestId)
{
    LoadResult result;
    result.sourceFile = file;
    result.requestId = requestId;

    if (!file.existsAsFile())
    {
        result.errorMessage = "File does not exist";
        return result;
    }

    std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(file));
    if (reader == nullptr)
    {
        result.errorMessage = "Unsupported or unreadable audio file";
        return result;
    }

    if (reader->lengthInSamples <= 0)
    {
        result.errorMessage = "Audio file is empty";
        return result;
    }

    if (reader->lengthInSamples > std::numeric_limits<int>::max())
    {
        result.errorMessage = "Audio file is too large for the current Flip mode MVP";
        return result;
    }

    const int numSamples = static_cast<int>(reader->lengthInSamples);
    const int sourceChannelCount = juce::jmax(1, static_cast<int>(reader->numChannels));
    const int decodeChannelCount = juce::jmin(2, sourceChannelCount);

    juce::AudioBuffer<float> sourceBuffer(decodeChannelCount, numSamples);
    sourceBuffer.clear();

    if (!reader->read(&sourceBuffer, 0, numSamples, 0, true, decodeChannelCount > 1))
    {
        result.errorMessage = "Failed to decode audio file";
        return result;
    }

    auto loadedSample = std::make_shared<LoadedSampleData>();
    loadedSample->audioBuffer.setSize(2, numSamples, false, false, true);
    loadedSample->audioBuffer.clear();

    if (decodeChannelCount == 1)
    {
        loadedSample->audioBuffer.copyFrom(0, 0, sourceBuffer, 0, 0, numSamples);
        loadedSample->audioBuffer.copyFrom(1, 0, sourceBuffer, 0, 0, numSamples);
    }
    else
    {
        loadedSample->audioBuffer.copyFrom(0, 0, sourceBuffer, 0, 0, numSamples);
        loadedSample->audioBuffer.copyFrom(1, 0, sourceBuffer, 1, 0, numSamples);
    }

    loadedSample->sourcePath = file.getFullPathName();
    loadedSample->displayName = file.getFileNameWithoutExtension();
    loadedSample->sourceSampleRate = reader->sampleRate;
    loadedSample->numChannels = decodeChannelCount;
    loadedSample->sourceLengthSamples = reader->lengthInSamples;
    loadedSample->durationSeconds = reader->lengthInSamples / juce::jmax(1.0, reader->sampleRate);
    buildWaveformPreview(*loadedSample);

    result.success = true;
    result.loadedSample = std::const_pointer_cast<const LoadedSampleData>(loadedSample);
    return result;
}

void SamplePlaybackVoice::reset()
{
    active = false;
    releasing = false;
    loopEnabled = false;
    visibleSlot = -1;
    voiceId = 0;
    activeSlice = {};
    readPosition = 0.0;
    fadeInRemaining = 0;
    fadeInTotal = 0;
    fadeOutRemaining = 0;
    fadeOutTotal = 0;
}

void SamplePlaybackVoice::trigger(const SampleSlice& slice,
                                  int visibleSlotToUse,
                                  uint64_t voiceIdToUse,
                                  bool loopEnabledToUse,
                                  int fadeSamples)
{
    active = true;
    releasing = false;
    loopEnabled = loopEnabledToUse;
    visibleSlot = visibleSlotToUse;
    voiceId = voiceIdToUse;
    activeSlice = slice;
    readPosition = static_cast<double>(slice.startSample);
    fadeInTotal = juce::jmax(0, fadeSamples);
    fadeInRemaining = fadeInTotal;
    fadeOutRemaining = 0;
    fadeOutTotal = 0;
}

void SamplePlaybackVoice::beginRelease(int fadeSamples)
{
    if (!active)
        return;

    releasing = true;
    fadeOutTotal = juce::jmax(0, fadeSamples);
    fadeOutRemaining = fadeOutTotal;
}

void SamplePlaybackVoice::stop()
{
    reset();
}

SampleModeEngine::SampleModeEngine() = default;

SampleModeEngine::~SampleModeEngine()
{
    fileManager.cancelPendingLoads();
}

void SampleModeEngine::prepare(double sampleRate, int maxBlockSize)
{
    preparedSampleRate.store(juce::jmax(1.0, sampleRate), std::memory_order_release);
    preparedMaxBlockSize.store(juce::jmax(1, maxBlockSize), std::memory_order_release);
}

int SampleModeEngine::loadSampleAsync(const juce::File& file)
{
    if (!file.existsAsFile())
        return 0;

    juce::WeakReference<SampleModeEngine> weakThis(this);

    {
        const juce::ScopedLock lock(stateLock);
        isLoading = true;
        statusText = "Loading " + file.getFileName() + "...";
        activeRequestId = fileManager.loadFileAsync(file,
            [weakThis](SampleFileManager::LoadResult result) mutable
            {
                if (weakThis != nullptr)
                    weakThis->handleLoadResult(std::move(result));
            });
    }

    sendChangeMessage();
    return activeRequestId;
}

void SampleModeEngine::clear()
{
    fileManager.cancelPendingLoads();

    {
        const juce::ScopedLock lock(stateLock);
        loadedSample.reset();
        sliceModel.clear();
        isLoading = false;
        activeRequestId = 0;
        statusText = "No sample loaded";
    }

    stop();
    clearPendingVisibleSlice();

    sendChangeMessage();
}

void SampleModeEngine::stop(bool immediate)
{
    const juce::SpinLock::ScopedLockType playbackScopedLock(playbackLock);
    if (!immediate)
    {
        const int fadeSamples = juce::jmax(16,
            juce::jmin(kDefaultSliceFadeSamples,
                       static_cast<int>(preparedSampleRate.load(std::memory_order_acquire) * 0.003)));
        bool anyActive = false;
        for (auto& voice : playbackVoices)
        {
            if (voice.isActive())
            {
                voice.beginRelease(fadeSamples);
                anyActive = true;
            }
        }
        if (anyActive)
        {
            playingAtomic.store(1, std::memory_order_release);
            return;
        }
    }

    for (auto& voice : playbackVoices)
        voice.stop();
    activeVisibleSliceSlot.store(-1, std::memory_order_release);
    playbackProgress.store(-1.0f, std::memory_order_release);
    playingAtomic.store(0, std::memory_order_release);
}

bool SampleModeEngine::hasSample() const
{
    const juce::ScopedLock lock(stateLock);
    return loadedSample != nullptr;
}

SampleModeEngine::StateSnapshot SampleModeEngine::getStateSnapshot() const
{
    const juce::ScopedLock lock(stateLock);

    StateSnapshot snapshot;
    snapshot.hasSample = (loadedSample != nullptr);
    snapshot.isLoading = isLoading;
    snapshot.statusText = statusText;
    snapshot.visibleSliceBankIndex = sliceModel.getVisibleBankIndex();
    snapshot.visibleSliceBankCount = sliceModel.getVisibleBankCount();
    snapshot.canNavigateLeft = sliceModel.canNavigateLeft();
    snapshot.canNavigateRight = sliceModel.canNavigateRight();
    snapshot.isPlaying = (playingAtomic.load(std::memory_order_acquire) != 0);
    snapshot.activeVisibleSliceSlot = activeVisibleSliceSlot.load(std::memory_order_acquire);
    snapshot.pendingVisibleSliceSlot = pendingVisibleSliceSlot.load(std::memory_order_acquire);
    snapshot.playbackProgress = playbackProgress.load(std::memory_order_acquire);
    snapshot.visibleSlices = sliceModel.getVisibleSlices();

    if (loadedSample != nullptr)
    {
        snapshot.samplePath = loadedSample->sourcePath;
        snapshot.displayName = loadedSample->displayName;
        snapshot.sourceSampleRate = loadedSample->sourceSampleRate;
        snapshot.totalSamples = loadedSample->sourceLengthSamples;
    }

    return snapshot;
}

std::shared_ptr<const LoadedSampleData> SampleModeEngine::getLoadedSample() const
{
    const juce::ScopedLock lock(stateLock);
    return loadedSample;
}

bool SampleModeEngine::canNavigateVisibleBankLeft() const
{
    const juce::ScopedLock lock(stateLock);
    return sliceModel.canNavigateLeft();
}

bool SampleModeEngine::canNavigateVisibleBankRight() const
{
    const juce::ScopedLock lock(stateLock);
    return sliceModel.canNavigateRight();
}

void SampleModeEngine::stepVisibleBank(int delta)
{
    {
        const juce::ScopedLock lock(stateLock);
        sliceModel.stepVisibleBank(delta);
        statusText = loadedSample != nullptr
            ? ("Viewing slice bank " + juce::String(sliceModel.getVisibleBankIndex() + 1)
               + "/" + juce::String(juce::jmax(1, sliceModel.getVisibleBankCount())))
            : juce::String("No sample loaded");
    }

    sendChangeMessage();
}

bool SampleModeEngine::hasVisibleSlice(int visibleSlot) const
{
    SampleSlice slice;
    return resolveVisibleSlice(visibleSlot, slice);
}

int SampleModeEngine::getActiveVisibleSliceSlot() const noexcept
{
    return activeVisibleSliceSlot.load(std::memory_order_acquire);
}

int SampleModeEngine::getPendingVisibleSliceSlot() const noexcept
{
    return pendingVisibleSliceSlot.load(std::memory_order_acquire);
}

void SampleModeEngine::setPendingVisibleSlice(int visibleSlot)
{
    const int clampedSlot = juce::jlimit(0, SliceModel::VisibleSliceCount - 1, visibleSlot);
    SampleSlice resolvedSlice;
    const bool hasSlice = resolveVisibleSlice(clampedSlot, resolvedSlice);
    {
        const juce::ScopedLock lock(stateLock);
        pendingTriggerSliceValid = hasSlice;
        if (hasSlice)
            pendingTriggerSlice = resolvedSlice;
    }
    pendingVisibleSliceSlot.store(clampedSlot, std::memory_order_release);
}

void SampleModeEngine::clearPendingVisibleSlice()
{
    const juce::ScopedLock lock(stateLock);
    pendingTriggerSliceValid = false;
    pendingVisibleSliceSlot.store(-1, std::memory_order_release);
}

bool SampleModeEngine::triggerVisibleSlice(int visibleSlot, bool loopEnabled)
{
    SampleSlice slice;
    const int pendingSlot = pendingVisibleSliceSlot.load(std::memory_order_acquire);
    {
        const juce::ScopedLock lock(stateLock);
        if (pendingTriggerSliceValid && pendingSlot == visibleSlot)
        {
            slice = pendingTriggerSlice;
            pendingTriggerSliceValid = false;
        }
        else
        {
            if (visibleSlot < 0 || visibleSlot >= SliceModel::VisibleSliceCount)
                return false;

            const auto visibleSlices = sliceModel.getVisibleSlices();
            const auto& resolvedSlice = visibleSlices[static_cast<size_t>(visibleSlot)];
            if (resolvedSlice.id < 0 || resolvedSlice.endSample <= resolvedSlice.startSample)
                return false;

            slice = resolvedSlice;
        }
    }

    const int fadeSamples = juce::jmax(16,
        juce::jmin(kDefaultSliceFadeSamples,
                   static_cast<int>(preparedSampleRate.load(std::memory_order_acquire) * 0.003)));

    const juce::SpinLock::ScopedLockType playbackScopedLock(playbackLock);
    for (auto& voice : playbackVoices)
    {
        if (voice.isActive())
            voice.beginRelease(fadeSamples);
    }

    SamplePlaybackVoice* targetVoice = nullptr;
    for (auto& voice : playbackVoices)
    {
        if (!voice.isActive())
        {
            targetVoice = &voice;
            break;
        }
    }
    if (targetVoice == nullptr)
    {
        targetVoice = &playbackVoices.front();
        targetVoice->stop();
    }

    targetVoice->trigger(slice, visibleSlot, nextVoiceId++, loopEnabled, fadeSamples);
    activeVisibleSliceSlot.store(visibleSlot, std::memory_order_release);
    pendingVisibleSliceSlot.store(-1, std::memory_order_release);
    playbackProgress.store(slice.normalizedStart, std::memory_order_release);
    playingAtomic.store(1, std::memory_order_release);
    return true;
}

bool SampleModeEngine::renderToBuffer(juce::AudioBuffer<float>& output,
                                      int startSample,
                                      int numSamples,
                                      float playbackRate,
                                      int fadeSamples)
{
    if (numSamples <= 0)
        return false;

    std::shared_ptr<const LoadedSampleData> sampleData;
    {
        const juce::ScopedLock lock(stateLock);
        sampleData = loadedSample;
    }

    if (sampleData == nullptr || sampleData->audioBuffer.getNumSamples() <= 0)
    {
        stop(true);
        return false;
    }

    const double renderRate = preparedSampleRate.load(std::memory_order_acquire);
    const double sourceRate = juce::jmax(1.0, sampleData->sourceSampleRate);
    const double rateScale = sourceRate / juce::jmax(1.0, renderRate);
    const double increment = juce::jlimit(0.03125, 8.0, static_cast<double>(playbackRate)) * rateScale;
    const int fadeLen = juce::jmax(16, fadeSamples);

    bool renderedAnything = false;
    int newestActiveSlot = -1;
    uint64_t newestVoiceId = 0;
    float latestProgress = playbackProgress.load(std::memory_order_acquire);

    const juce::SpinLock::ScopedLockType playbackScopedLock(playbackLock);
    for (auto& voice : playbackVoices)
    {
        if (!voice.isActive())
            continue;

        renderedAnything = true;
        if (voice.getVoiceId() >= newestVoiceId)
        {
            newestVoiceId = voice.getVoiceId();
            newestActiveSlot = voice.getVisibleSlot();
        }

        const double sliceStart = static_cast<double>(voice.getActiveSlice().startSample);
        const double sliceEnd = static_cast<double>(juce::jmax(voice.getActiveSlice().startSample + 1,
                                                               voice.getActiveSlice().endSample));
        const double sliceLength = juce::jmax(1.0, sliceEnd - sliceStart);
        const double localFade = juce::jmin(sliceLength * 0.25, static_cast<double>(fadeLen));

        for (int sampleIndex = 0; sampleIndex < numSamples; ++sampleIndex)
        {
            if (!voice.isActive())
                break;

            if (!voice.shouldLoop() && voice.readPosition >= sliceEnd)
            {
                voice.stop();
                break;
            }

            if (voice.shouldLoop())
            {
                while (voice.readPosition >= sliceEnd)
                    voice.readPosition -= sliceLength;
                while (voice.readPosition < sliceStart)
                    voice.readPosition += sliceLength;
            }

            float gain = 1.0f;
            const double fromStart = juce::jmax(0.0, voice.readPosition - sliceStart);
            const double toEnd = juce::jmax(0.0, sliceEnd - voice.readPosition);

            if (localFade > 1.0)
            {
                gain *= static_cast<float>(juce::jlimit(0.0, 1.0, fromStart / localFade));
                gain *= static_cast<float>(juce::jlimit(0.0, 1.0, toEnd / localFade));
            }

            if (voice.fadeInRemaining > 0 && voice.fadeInTotal > 0)
            {
                const float fadeInGain = 1.0f - (static_cast<float>(voice.fadeInRemaining) / static_cast<float>(voice.fadeInTotal));
                gain *= juce::jlimit(0.0f, 1.0f, fadeInGain);
                --voice.fadeInRemaining;
            }

            if (voice.isReleasing())
            {
                if (voice.fadeOutRemaining > 0 && voice.fadeOutTotal > 0)
                {
                    const float fadeOutGain = static_cast<float>(voice.fadeOutRemaining) / static_cast<float>(voice.fadeOutTotal);
                    gain *= juce::jlimit(0.0f, 1.0f, fadeOutGain);
                    --voice.fadeOutRemaining;
                }
                else
                {
                    voice.stop();
                    break;
                }
            }

            if (gain > 1.0e-5f)
            {
                const float left = readInterpolatedSample(sampleData->audioBuffer, 0, voice.readPosition);
                const float right = readInterpolatedSample(sampleData->audioBuffer, 1, voice.readPosition);
                output.addSample(0, startSample + sampleIndex, left * gain);
                output.addSample(1, startSample + sampleIndex, right * gain);
            }

            voice.readPosition += increment;
            latestProgress = juce::jlimit(0.0f,
                                          1.0f,
                                          static_cast<float>(voice.readPosition
                                              / juce::jmax<int64_t>(1, sampleData->sourceLengthSamples)));
        }
    }

    activeVisibleSliceSlot.store(newestActiveSlot, std::memory_order_release);
    playbackProgress.store(renderedAnything ? latestProgress : -1.0f, std::memory_order_release);
    playingAtomic.store(renderedAnything ? 1 : 0, std::memory_order_release);

    return renderedAnything;
}

void SampleModeEngine::setLoadStatusCallback(LoadStatusCallback callback)
{
    const juce::ScopedLock lock(stateLock);
    loadStatusCallback = std::move(callback);
}

bool SampleModeEngine::resolveVisibleSlice(int visibleSlot, SampleSlice& sliceOut) const
{
    if (visibleSlot < 0 || visibleSlot >= SliceModel::VisibleSliceCount)
        return false;

    const juce::ScopedLock lock(stateLock);
    const auto visibleSlices = sliceModel.getVisibleSlices();
    const auto& slice = visibleSlices[static_cast<size_t>(visibleSlot)];
    if (slice.id < 0 || slice.endSample <= slice.startSample)
        return false;

    sliceOut = slice;
    return true;
}

void SampleModeEngine::handleLoadResult(SampleFileManager::LoadResult result)
{
    LoadStatusCallback callbackToInvoke;

    {
        const juce::ScopedLock lock(stateLock);

        if (result.requestId != activeRequestId)
            return;

        isLoading = false;

        if (result.success && result.loadedSample != nullptr)
        {
            loadedSample = std::move(result.loadedSample);
            sliceModel.setSlices(analysisEngine.buildInitialSlices(*loadedSample));
            sliceModel.setVisibleBankIndex(0);
            pendingTriggerSliceValid = false;
            statusText = "Loaded " + loadedSample->displayName;
        }
        else
        {
            loadedSample.reset();
            sliceModel.clear();
            pendingTriggerSliceValid = false;
            statusText = result.errorMessage.isNotEmpty() ? result.errorMessage : "Sample load failed";
        }

        callbackToInvoke = loadStatusCallback;
    }

    stop(true);
    clearPendingVisibleSlice();

    if (callbackToInvoke)
        callbackToInvoke(result);

    sendChangeMessage();
}

SampleModeComponent::SampleModeComponent()
{
    startTimerHz(30);
}

SampleModeComponent::~SampleModeComponent()
{
    stopTimer();
    setEngine(nullptr);
}

void SampleModeComponent::setEngine(SampleModeEngine* engineToUse)
{
    if (engine == engineToUse)
        return;

    if (engine != nullptr)
        engine->removeChangeListener(this);

    engine = engineToUse;

    if (engine != nullptr)
        engine->addChangeListener(this);

    refreshFromEngine();
}

void SampleModeComponent::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds().reduced(8);
    g.fillAll(juce::Colour(0xff0d1014));

    g.setColour(juce::Colour(0xffd7dce2));
    g.setFont(juce::FontOptions(15.0f, juce::Font::bold));
    const juce::String headerText = stateSnapshot.displayName.isNotEmpty()
        ? stateSnapshot.displayName
        : stateSnapshot.statusText;
    g.drawText(headerText, bounds.removeFromTop(18), juce::Justification::centredLeft);
    bounds.removeFromTop(4);

    waveformBounds = bounds.removeFromTop(juce::jmax(160, bounds.getHeight() - 40));
    g.setColour(juce::Colour(0xff171d25));
    g.fillRoundedRectangle(waveformBounds.toFloat(), 8.0f);
    g.setColour(juce::Colour(0xff263241));
    g.drawRoundedRectangle(waveformBounds.toFloat(), 8.0f, 1.0f);

    if (loadedSample != nullptr && !loadedSample->previewMin.empty() && !loadedSample->previewMax.empty())
    {
        const auto centerY = waveformBounds.getCentreY();
        const auto halfHeight = waveformBounds.getHeight() * 0.42f;
        const auto pointCount = static_cast<int>(loadedSample->previewMin.size());

        g.setColour(juce::Colour(0xff6ca7ff));
        for (int pointIndex = 0; pointIndex < pointCount; ++pointIndex)
        {
            const float x = waveformBounds.getX() + (waveformBounds.getWidth() - 1.0f)
                          * (static_cast<float>(pointIndex) / juce::jmax(1, pointCount - 1));
            const float minY = centerY - (loadedSample->previewMin[static_cast<size_t>(pointIndex)] * halfHeight);
            const float maxY = centerY - (loadedSample->previewMax[static_cast<size_t>(pointIndex)] * halfHeight);
            g.drawVerticalLine(juce::roundToInt(x), juce::jmin(minY, maxY), juce::jmax(minY, maxY));
        }

        g.setColour(juce::Colour(0x70ffffff));
        g.drawHorizontalLine(centerY, static_cast<float>(waveformBounds.getX()), static_cast<float>(waveformBounds.getRight()));

        g.setColour(juce::Colour(0xfff6b64f));
        for (const auto& slice : stateSnapshot.visibleSlices)
        {
            if (slice.id < 0)
                continue;

            const int x = waveformBounds.getX()
                        + juce::roundToInt(slice.normalizedStart * static_cast<float>(waveformBounds.getWidth()));
            g.drawVerticalLine(x, static_cast<float>(waveformBounds.getY() + 6), static_cast<float>(waveformBounds.getBottom() - 24));
            g.drawText(slice.label,
                       x + 3,
                       waveformBounds.getBottom() - 20,
                       32,
                       14,
                       juce::Justification::left,
                       false);
        }

        if (stateSnapshot.pendingVisibleSliceSlot >= 0
            && stateSnapshot.pendingVisibleSliceSlot < SliceModel::VisibleSliceCount)
        {
            const auto& pendingSlice = stateSnapshot.visibleSlices[static_cast<size_t>(stateSnapshot.pendingVisibleSliceSlot)];
            if (pendingSlice.id >= 0)
            {
                const int x = waveformBounds.getX()
                            + juce::roundToInt(pendingSlice.normalizedStart * static_cast<float>(waveformBounds.getWidth()));
                g.setColour(juce::Colour(0x80ffffff));
                g.drawVerticalLine(x, static_cast<float>(waveformBounds.getY() + 3), static_cast<float>(waveformBounds.getBottom() - 3));
            }
        }

        if (stateSnapshot.playbackProgress >= 0.0f)
        {
            const int playheadX = waveformBounds.getX()
                                + juce::roundToInt(stateSnapshot.playbackProgress * static_cast<float>(waveformBounds.getWidth()));
            g.setColour(juce::Colour(0xff8df0b8));
            g.drawVerticalLine(playheadX,
                               static_cast<float>(waveformBounds.getY() + 2),
                               static_cast<float>(waveformBounds.getBottom() - 2));
        }
    }
    else
    {
        g.setColour(juce::Colour(0xff6b7787));
        g.drawFittedText("Async loader ready. Click Load, then trigger slices directly from this view.",
                         waveformBounds.reduced(12),
                         juce::Justification::centred,
                         2);
    }

    auto footer = bounds.reduced(2, 4);
    leftNavBounds = footer.removeFromLeft(34);
    rightNavBounds = footer.removeFromRight(34);
    auto infoBounds = footer;

    g.setColour(stateSnapshot.canNavigateLeft ? juce::Colour(0xffd7dce2) : juce::Colour(0xff4d5968));
    g.drawFittedText("<", leftNavBounds, juce::Justification::centred, 1);
    g.setColour(stateSnapshot.canNavigateRight ? juce::Colour(0xffd7dce2) : juce::Colour(0xff4d5968));
    g.drawFittedText(">", rightNavBounds, juce::Justification::centred, 1);

    g.setColour(juce::Colour(0xff8d9aab));
    const auto bankText = "Slice bank "
        + juce::String(stateSnapshot.visibleSliceBankIndex + 1)
        + "/"
        + juce::String(juce::jmax(1, stateSnapshot.visibleSliceBankCount));
    g.drawText(bankText, infoBounds.removeFromLeft(160), juce::Justification::centredLeft);

    if (stateSnapshot.hasSample)
    {
        const auto seconds = stateSnapshot.sourceSampleRate > 0.0
            ? static_cast<double>(stateSnapshot.totalSamples) / stateSnapshot.sourceSampleRate
            : 0.0;
        g.drawText(stateSnapshot.displayName + "  |  " + juce::String(seconds, 2) + " s",
                   infoBounds,
                   juce::Justification::centredRight);
    }
}

void SampleModeComponent::resized()
{
}

void SampleModeComponent::mouseDown(const juce::MouseEvent& event)
{
    const auto point = event.getPosition();

    if (leftNavBounds.contains(point))
    {
        if (onNavigateVisibleBank && stateSnapshot.canNavigateLeft)
            onNavigateVisibleBank(-1);
        return;
    }

    if (rightNavBounds.contains(point))
    {
        if (onNavigateVisibleBank && stateSnapshot.canNavigateRight)
            onNavigateVisibleBank(1);
        return;
    }

    const int visibleSlot = hitTestVisibleSlice(point);
    if (visibleSlot >= 0 && onTriggerVisibleSlice)
        onTriggerVisibleSlice(visibleSlot);
}

void SampleModeComponent::changeListenerCallback(juce::ChangeBroadcaster* source)
{
    if (source == engine)
        refreshFromEngine();
}

void SampleModeComponent::refreshFromEngine()
{
    if (engine == nullptr)
    {
        stateSnapshot = {};
        loadedSample.reset();
        repaint();
        return;
    }

    stateSnapshot = engine->getStateSnapshot();
    loadedSample = engine->getLoadedSample();
    repaint();
}

int SampleModeComponent::hitTestVisibleSlice(const juce::Point<int>& point) const
{
    if (!waveformBounds.contains(point))
        return -1;

    const int relativeX = point.x - waveformBounds.getX();
    const float normalizedX = juce::jlimit(0.0f,
                                           1.0f,
                                           static_cast<float>(relativeX)
                                               / juce::jmax(1.0f, static_cast<float>(waveformBounds.getWidth())));

    for (int slot = 0; slot < SliceModel::VisibleSliceCount; ++slot)
    {
        const auto& slice = stateSnapshot.visibleSlices[static_cast<size_t>(slot)];
        if (slice.id < 0)
            continue;
        const bool isLastSlice = (slot == (SliceModel::VisibleSliceCount - 1));
        if (normalizedX >= slice.normalizedStart
            && (normalizedX < slice.normalizedEnd || (isLastSlice && normalizedX <= slice.normalizedEnd)))
            return slot;
    }

    return -1;
}

void SampleModeComponent::timerCallback()
{
    if (engine != nullptr)
        refreshFromEngine();
}
