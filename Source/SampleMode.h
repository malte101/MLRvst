#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <vector>

struct SampleCuePoint
{
    int64_t samplePosition = 0;
    juce::String name;
    bool loopEnabled = false;
    int64_t loopEndSample = 0;
};

struct SampleSlice
{
    int id = -1;
    int64_t startSample = 0;
    int64_t endSample = 0;
    float normalizedStart = 0.0f;
    float normalizedEnd = 0.0f;
    juce::String label;
    bool transientDerived = false;
};

struct LoadedSampleData
{
    static constexpr int PreviewPointCount = 2048;

    juce::AudioBuffer<float> audioBuffer;
    juce::String sourcePath;
    juce::String displayName;
    double sourceSampleRate = 44100.0;
    int numChannels = 0;
    int64_t sourceLengthSamples = 0;
    double durationSeconds = 0.0;
    std::vector<float> previewMin;
    std::vector<float> previewMax;
};

class SliceModel
{
public:
    static constexpr int VisibleSliceCount = 16;

    void clear();
    void setSlices(std::vector<SampleSlice> newSlices);
    void setVisibleBankIndex(int bankIndex);
    int getVisibleBankIndex() const noexcept { return visibleBankIndex; }
    int getVisibleBankCount() const noexcept;
    bool canNavigateLeft() const noexcept;
    bool canNavigateRight() const noexcept;
    void stepVisibleBank(int delta);
    std::array<SampleSlice, VisibleSliceCount> getVisibleSlices() const;
    const std::vector<SampleSlice>& getAllSlices() const noexcept { return slices; }

    static std::vector<SampleSlice> buildUniformSlices(const LoadedSampleData& sampleData,
                                                       int totalSliceCount = VisibleSliceCount);

private:
    std::vector<SampleSlice> slices;
    int visibleBankIndex = 0;
};

class SampleAnalysisEngine
{
public:
    std::vector<SampleSlice> buildInitialSlices(const LoadedSampleData& sampleData) const;
};

class SampleFileManager
{
public:
    struct LoadResult
    {
        bool success = false;
        juce::String errorMessage;
        juce::File sourceFile;
        std::shared_ptr<const LoadedSampleData> loadedSample;
        int requestId = 0;
    };

    using LoadCallback = std::function<void(LoadResult)>;

    SampleFileManager();
    ~SampleFileManager();

    int loadFileAsync(const juce::File& file, LoadCallback callback);
    void cancelPendingLoads();

private:
    class LoadJob;

    LoadResult decodeFile(const juce::File& file, int requestId);

    juce::AudioFormatManager formatManager;
    juce::ThreadPool threadPool { 1 };
    std::atomic<int> nextRequestId { 1 };
};

class SamplePlaybackVoice
{
public:
    void reset();
    void trigger(const SampleSlice& slice, int64_t triggerSample);
    void stop();

    bool isActive() const noexcept { return active; }
    const SampleSlice& getActiveSlice() const noexcept { return activeSlice; }
    int64_t getTriggerSample() const noexcept { return triggerSample; }

private:
    bool active = false;
    SampleSlice activeSlice;
    int64_t triggerSample = 0;
};

class SampleModeEngine : public juce::ChangeBroadcaster
{
public:
    struct StateSnapshot
    {
        bool hasSample = false;
        bool isLoading = false;
        juce::String statusText { "No sample loaded" };
        juce::String samplePath;
        juce::String displayName;
        double sourceSampleRate = 44100.0;
        int64_t totalSamples = 0;
        int visibleSliceBankIndex = 0;
        int visibleSliceBankCount = 0;
        std::array<SampleSlice, SliceModel::VisibleSliceCount> visibleSlices {};
    };

    SampleModeEngine();
    ~SampleModeEngine() override;

    int loadSampleAsync(const juce::File& file);
    void clear();

    bool hasSample() const;
    StateSnapshot getStateSnapshot() const;
    std::shared_ptr<const LoadedSampleData> getLoadedSample() const;

    bool canNavigateVisibleBankLeft() const;
    bool canNavigateVisibleBankRight() const;
    void stepVisibleBank(int delta);

    using LoadStatusCallback = std::function<void(const SampleFileManager::LoadResult&)>;
    void setLoadStatusCallback(LoadStatusCallback callback);

private:
    void handleLoadResult(SampleFileManager::LoadResult result);

    mutable juce::CriticalSection stateLock;
    SampleFileManager fileManager;
    SampleAnalysisEngine analysisEngine;
    SliceModel sliceModel;
    SamplePlaybackVoice previewVoice;
    std::shared_ptr<const LoadedSampleData> loadedSample;
    LoadStatusCallback loadStatusCallback;
    bool isLoading = false;
    juce::String statusText { "No sample loaded" };
    int activeRequestId = 0;

    JUCE_DECLARE_WEAK_REFERENCEABLE(SampleModeEngine)
};

class SampleModeComponent : public juce::Component,
                            public juce::ChangeListener
{
public:
    SampleModeComponent();
    ~SampleModeComponent() override;

    void setEngine(SampleModeEngine* engineToUse);

    void paint(juce::Graphics& g) override;
    void resized() override;
    void changeListenerCallback(juce::ChangeBroadcaster* source) override;

private:
    void refreshFromEngine();

    SampleModeEngine* engine = nullptr;
    SampleModeEngine::StateSnapshot stateSnapshot;
    std::shared_ptr<const LoadedSampleData> loadedSample;
};

class MonomeSamplePage
{
public:
    enum class Action
    {
        None = 0,
        TriggerSlice,
        NavigateLeft,
        NavigateRight,
        SelectCue,
        ToggleLoop
    };

    static constexpr int VisibleSliceCount = SliceModel::VisibleSliceCount;
};
