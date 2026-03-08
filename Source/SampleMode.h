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
    void trigger(const SampleSlice& slice,
                 int visibleSlot,
                 uint64_t voiceIdToUse,
                 bool loopEnabled,
                 int fadeSamples);
    void beginRelease(int fadeSamples);
    void stop();

    bool isActive() const noexcept { return active; }
    const SampleSlice& getActiveSlice() const noexcept { return activeSlice; }
    int getVisibleSlot() const noexcept { return visibleSlot; }
    uint64_t getVoiceId() const noexcept { return voiceId; }
    bool shouldLoop() const noexcept { return loopEnabled; }
    bool isReleasing() const noexcept { return releasing; }

    double readPosition = 0.0;
    int fadeInRemaining = 0;
    int fadeInTotal = 0;
    int fadeOutRemaining = 0;
    int fadeOutTotal = 0;

private:
    bool active = false;
    bool releasing = false;
    bool loopEnabled = false;
    int visibleSlot = -1;
    uint64_t voiceId = 0;
    SampleSlice activeSlice;
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
        bool canNavigateLeft = false;
        bool canNavigateRight = false;
        bool isPlaying = false;
        int activeVisibleSliceSlot = -1;
        int pendingVisibleSliceSlot = -1;
        float playbackProgress = -1.0f;
        std::array<SampleSlice, SliceModel::VisibleSliceCount> visibleSlices {};
    };

    SampleModeEngine();
    ~SampleModeEngine() override;

    void prepare(double sampleRate, int maxBlockSize);
    int loadSampleAsync(const juce::File& file);
    void clear();
    void stop(bool immediate = true);

    bool hasSample() const;
    bool isPlaying() const noexcept { return playingAtomic.load(std::memory_order_acquire) != 0; }
    StateSnapshot getStateSnapshot() const;
    std::shared_ptr<const LoadedSampleData> getLoadedSample() const;

    bool canNavigateVisibleBankLeft() const;
    bool canNavigateVisibleBankRight() const;
    void stepVisibleBank(int delta);
    bool hasVisibleSlice(int visibleSlot) const;
    int getActiveVisibleSliceSlot() const noexcept;
    int getPendingVisibleSliceSlot() const noexcept;
    void setPendingVisibleSlice(int visibleSlot);
    void clearPendingVisibleSlice();
    bool triggerVisibleSlice(int visibleSlot, bool loopEnabled = false);
    bool renderToBuffer(juce::AudioBuffer<float>& output,
                        int startSample,
                        int numSamples,
                        float playbackRate,
                        int fadeSamples);

    using LoadStatusCallback = std::function<void(const SampleFileManager::LoadResult&)>;
    void setLoadStatusCallback(LoadStatusCallback callback);

private:
    bool resolveVisibleSlice(int visibleSlot, SampleSlice& sliceOut) const;
    void handleLoadResult(SampleFileManager::LoadResult result);

    mutable juce::CriticalSection stateLock;
    juce::SpinLock playbackLock;
    SampleFileManager fileManager;
    SampleAnalysisEngine analysisEngine;
    SliceModel sliceModel;
    std::array<SamplePlaybackVoice, 4> playbackVoices;
    std::shared_ptr<const LoadedSampleData> loadedSample;
    LoadStatusCallback loadStatusCallback;
    bool isLoading = false;
    juce::String statusText { "No sample loaded" };
    int activeRequestId = 0;
    bool pendingTriggerSliceValid = false;
    SampleSlice pendingTriggerSlice;
    std::atomic<double> preparedSampleRate { 44100.0 };
    std::atomic<int> preparedMaxBlockSize { 512 };
    std::atomic<int> activeVisibleSliceSlot { -1 };
    std::atomic<int> pendingVisibleSliceSlot { -1 };
    std::atomic<float> playbackProgress { -1.0f };
    std::atomic<int> playingAtomic { 0 };
    uint64_t nextVoiceId = 1;

    JUCE_DECLARE_WEAK_REFERENCEABLE(SampleModeEngine)
};

class SampleModeComponent : public juce::Component,
                            public juce::ChangeListener,
                            private juce::Timer
{
public:
    SampleModeComponent();
    ~SampleModeComponent() override;

    void setEngine(SampleModeEngine* engineToUse);

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& event) override;
    void changeListenerCallback(juce::ChangeBroadcaster* source) override;

    std::function<void(int visibleSlot)> onTriggerVisibleSlice;
    std::function<void(int delta)> onNavigateVisibleBank;

private:
    void refreshFromEngine();
    int hitTestVisibleSlice(const juce::Point<int>& point) const;
    void timerCallback() override;

    SampleModeEngine* engine = nullptr;
    SampleModeEngine::StateSnapshot stateSnapshot;
    std::shared_ptr<const LoadedSampleData> loadedSample;
    juce::Rectangle<int> waveformBounds;
    juce::Rectangle<int> leftNavBounds;
    juce::Rectangle<int> rightNavBounds;
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
