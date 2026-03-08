#include "SampleMode.h"

#include <cmath>
#include <limits>

namespace
{
constexpr double kInitialSliceTargetSeconds = 1.5;
constexpr int kMaxInitialSliceCount = 256;

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
        result.errorMessage = "Audio file is too large for the current Sample Mode MVP";
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
    activeSlice = {};
    triggerSample = 0;
}

void SamplePlaybackVoice::trigger(const SampleSlice& slice, int64_t triggerSampleToUse)
{
    active = true;
    activeSlice = slice;
    triggerSample = triggerSampleToUse;
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
        previewVoice.reset();
        isLoading = false;
        activeRequestId = 0;
        statusText = "No sample loaded";
    }

    sendChangeMessage();
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

void SampleModeEngine::setLoadStatusCallback(LoadStatusCallback callback)
{
    const juce::ScopedLock lock(stateLock);
    loadStatusCallback = std::move(callback);
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
            statusText = "Loaded " + loadedSample->displayName;
        }
        else
        {
            loadedSample.reset();
            sliceModel.clear();
            statusText = result.errorMessage.isNotEmpty() ? result.errorMessage : "Sample load failed";
        }

        callbackToInvoke = loadStatusCallback;
    }

    if (callbackToInvoke)
        callbackToInvoke(result);

    sendChangeMessage();
}

SampleModeComponent::SampleModeComponent() = default;

SampleModeComponent::~SampleModeComponent()
{
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
    auto bounds = getLocalBounds().reduced(12);
    g.fillAll(juce::Colour(0xff0d1014));

    g.setColour(juce::Colour(0xffd7dce2));
    g.setFont(juce::FontOptions(18.0f, juce::Font::bold));
    g.drawText("Sample Mode", bounds.removeFromTop(24), juce::Justification::centredLeft);

    g.setFont(juce::FontOptions(13.0f));
    g.setColour(juce::Colour(0xff8d9aab));
    g.drawText(stateSnapshot.statusText, bounds.removeFromTop(20), juce::Justification::centredLeft);
    bounds.removeFromTop(8);

    auto waveformBounds = bounds.removeFromTop(juce::jmax(140, bounds.getHeight() - 64));
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
    }
    else
    {
        g.setColour(juce::Colour(0xff6b7787));
        g.drawFittedText("Async loader ready. Waveform appears here after a sample is loaded.",
                         waveformBounds.reduced(12),
                         juce::Justification::centred,
                         2);
    }

    auto footer = bounds.reduced(2, 8);
    g.setColour(juce::Colour(0xff8d9aab));
    const auto bankText = "Slice bank "
        + juce::String(stateSnapshot.visibleSliceBankIndex + 1)
        + "/"
        + juce::String(juce::jmax(1, stateSnapshot.visibleSliceBankCount));
    g.drawText(bankText, footer.removeFromLeft(160), juce::Justification::centredLeft);

    if (stateSnapshot.hasSample)
    {
        const auto seconds = stateSnapshot.sourceSampleRate > 0.0
            ? static_cast<double>(stateSnapshot.totalSamples) / stateSnapshot.sourceSampleRate
            : 0.0;
        g.drawText(stateSnapshot.displayName + "  |  " + juce::String(seconds, 2) + " s",
                   footer,
                   juce::Justification::centredRight);
    }
}

void SampleModeComponent::resized()
{
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
