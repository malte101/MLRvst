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
#include <optional>
#include <vector>

enum class SampleSliceMode
{
    Uniform = 0,
    Transient,
    Beat,
    Manual
};

enum class SampleTriggerMode
{
    OneShot = 0,
    Loop
};

struct SampleAnalysisSummary
{
    double estimatedTempoBpm = 0.0;
    double estimatedPitchHz = 0.0;
    int estimatedPitchMidi = -1;
    bool audioFluxUsed = false;
    juce::String analysisSource;
    std::vector<int64_t> transientSamples;
};

struct SampleCuePoint
{
    int id = -1;
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
    bool manualDerived = false;
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
    SampleAnalysisSummary analysis;
};

struct StretchedSliceBuffer
{
    int sliceId = -1;
    int64_t sliceStartSample = 0;
    int64_t sliceEndSample = 0;
    float playbackRate = 1.0f;
    float pitchSemitones = 0.0f;
    double sourceSampleRate = 44100.0;
    juce::AudioBuffer<float> audioBuffer;
};

struct SampleModePersistentState
{
    juce::String samplePath;
    int visibleSliceBankIndex = 0;
    SampleSliceMode sliceMode = SampleSliceMode::Uniform;
    SampleTriggerMode triggerMode = SampleTriggerMode::OneShot;
    int beatDivision = 1;
    float viewZoom = 1.0f;
    float viewScroll = 0.0f;
    int selectedCueIndex = -1;
    double analyzedTempoBpm = 0.0;
    double analyzedPitchHz = 0.0;
    int analyzedPitchMidi = -1;
    juce::String analysisSource;
    std::vector<SampleCuePoint> cuePoints;
    std::vector<SampleSlice> storedSlices;

    juce::ValueTree createValueTree(const juce::Identifier& type = "FlipState") const;
    static SampleModePersistentState fromValueTree(const juce::ValueTree& state);

    std::unique_ptr<juce::XmlElement> createXml(const juce::String& tagName = "FlipState") const;
    static SampleModePersistentState fromXml(const juce::XmlElement& xml);
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
    static std::vector<SampleSlice> buildBeatSlices(const LoadedSampleData& sampleData,
                                                    double tempoBpm,
                                                    int beatDivision);
    static std::vector<SampleSlice> buildTransientSlices(const LoadedSampleData& sampleData,
                                                         const std::vector<int64_t>& transientSamples);
    static std::vector<SampleSlice> buildManualSlices(const LoadedSampleData& sampleData,
                                                      const std::vector<SampleCuePoint>& cuePoints);

private:
    std::vector<SampleSlice> slices;
    int visibleBankIndex = 0;
};

class SampleAnalysisEngine
{
public:
    void enrichAnalysisForFile(const juce::File& file, LoadedSampleData& sampleData) const;
    std::vector<SampleSlice> buildSlicesForState(const LoadedSampleData& sampleData,
                                                 const SampleModePersistentState& state) const;
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
    bool hasStretchedBuffer() const noexcept { return stretchedBuffer != nullptr; }
    std::shared_ptr<const StretchedSliceBuffer> getStretchedBuffer() const noexcept { return stretchedBuffer; }
    void setStretchedBuffer(std::shared_ptr<const StretchedSliceBuffer> bufferToUse);
    void clearStretchedBuffer();

    double readPosition = 0.0;
    double stretchedReadPosition = 0.0;
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
    std::shared_ptr<const StretchedSliceBuffer> stretchedBuffer;
};

class SampleModeEngine : public juce::ChangeBroadcaster
{
public:
    struct RenderResult
    {
        bool renderedAnything = false;
        bool usedInternalPitch = false;
    };

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
        SampleSliceMode sliceMode = SampleSliceMode::Uniform;
        SampleTriggerMode triggerMode = SampleTriggerMode::OneShot;
        int beatDivision = 1;
        float viewZoom = 1.0f;
        float viewScroll = 0.0f;
        int selectedCueIndex = -1;
        double estimatedTempoBpm = 0.0;
        double estimatedPitchHz = 0.0;
        int estimatedPitchMidi = -1;
        juce::String analysisSource;
        std::vector<SampleCuePoint> cuePoints;
        std::array<SampleSlice, SliceModel::VisibleSliceCount> visibleSlices {};
    };

    SampleModeEngine();
    ~SampleModeEngine() override;

    void prepare(double sampleRate, int maxBlockSize);
    int loadSampleAsync(const juce::File& file);
    bool loadSampleFromBuffer(const juce::AudioBuffer<float>& buffer,
                              double sourceSampleRate,
                              const juce::String& sourcePath = {},
                              const juce::String& displayName = {});
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
    bool triggerVisibleSlice(int visibleSlot, bool forceLoop = false);
    RenderResult renderToBuffer(juce::AudioBuffer<float>& output,
                                int startSample,
                                int numSamples,
                                float playbackRate,
                                int fadeSamples,
                                float pitchSemitones,
                                bool preferHighQualityKeyLock);
    void requestKeyLockRenderCache(float playbackRate,
                                   float pitchSemitones,
                                   bool enabled);

    SampleModePersistentState capturePersistentState() const;
    void applyPersistentState(const SampleModePersistentState& state);

    void setSliceMode(SampleSliceMode mode);
    SampleSliceMode getSliceMode() const;
    void setTriggerMode(SampleTriggerMode mode);
    SampleTriggerMode getTriggerMode() const;
    void setBeatDivision(int division);
    int getBeatDivision() const;
    void setViewWindow(float scroll, float zoom);
    float getViewScroll() const;
    float getViewZoom() const;
    void selectCuePoint(int cueIndex);
    int getSelectedCuePoint() const;
    int createCuePointAtNormalizedPosition(float normalizedPosition);
    bool moveCuePoint(int cueIndex, float normalizedPosition);
    bool deleteCuePoint(int cueIndex);

    using LoadStatusCallback = std::function<void(const SampleFileManager::LoadResult&)>;
    void setLoadStatusCallback(LoadStatusCallback callback);

private:
    bool resolveVisibleSlice(int visibleSlot, SampleSlice& sliceOut) const;
    void handleLoadResult(SampleFileManager::LoadResult result);
    void handleKeyLockCacheReady(uint64_t generation,
                                 std::vector<std::shared_ptr<const StretchedSliceBuffer>> caches,
                                 float playbackRate,
                                 float pitchSemitones,
                                 int visibleBankIndex);
    void rebuildSlicesLocked();
    std::shared_ptr<const StretchedSliceBuffer> findKeyLockCacheForSlice(const SampleSlice& slice,
                                                                         float playbackRate,
                                                                         float pitchSemitones) const;
    bool voiceHasMatchingKeyLockCache(const SamplePlaybackVoice& voice,
                                      float playbackRate,
                                      float pitchSemitones) const;
    static int clampCueSelection(int selectedCueIndex, int cueCount);

    mutable juce::CriticalSection stateLock;
    juce::SpinLock playbackLock;
    SampleFileManager fileManager;
    juce::ThreadPool keyLockCacheThreadPool { 1 };
    SampleAnalysisEngine analysisEngine;
    SliceModel sliceModel;
    SampleModePersistentState persistentState;
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
    bool keyLockCacheEnabled = false;
    bool keyLockCacheBuildInFlight = false;
    uint64_t keyLockCacheGeneration = 0;
    float keyLockCachePlaybackRate = 1.0f;
    float keyLockCachePitchSemitones = 0.0f;
    int keyLockCacheVisibleBankIndex = -1;
    std::vector<std::shared_ptr<const StretchedSliceBuffer>> keyLockCaches;

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
    void mouseDrag(const juce::MouseEvent& event) override;
    void mouseUp(const juce::MouseEvent& event) override;
    void mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) override;
    bool keyPressed(const juce::KeyPress& key) override;
    void changeListenerCallback(juce::ChangeBroadcaster* source) override;

    std::function<void(int visibleSlot)> onTriggerVisibleSlice;
    std::function<void(int delta)> onNavigateVisibleBank;

private:
    void refreshFromEngine();
    int hitTestVisibleSlice(const juce::Point<int>& point) const;
    int hitTestCuePoint(const juce::Point<int>& point) const;
    float normalizedPositionFromPoint(const juce::Point<int>& point) const;
    juce::Range<float> getVisibleRange() const;
    juce::Rectangle<int> getSliceModeButtonBounds(SampleSliceMode mode) const;
    juce::Rectangle<int> getTriggerModeButtonBounds(SampleTriggerMode mode) const;
    void timerCallback() override;

    SampleModeEngine* engine = nullptr;
    SampleModeEngine::StateSnapshot stateSnapshot;
    std::shared_ptr<const LoadedSampleData> loadedSample;
    juce::Rectangle<int> waveformBounds;
    juce::Rectangle<int> leftNavBounds;
    juce::Rectangle<int> rightNavBounds;
    juce::Rectangle<int> sliceModeArea;
    juce::Rectangle<int> triggerModeArea;
    int draggingCueIndex = -1;
    bool createdCueOnMouseDown = false;
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
