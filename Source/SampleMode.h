#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "TimeStretchBackend.h"

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

enum class SamplePitchAnalysisProfile
{
    Polyphonic = 0,
    Monophonic
};

struct SampleAnalysisSummary
{
    double estimatedTempoBpm = 0.0;
    double estimatedPitchHz = 0.0;
    int estimatedPitchMidi = -1;
    float estimatedPitchConfidence = 0.0f;
    int estimatedScaleIndex = -1;
    float estimatedScaleConfidence = 0.0f;
    bool essentiaUsed = false;
    juce::String analysisSource;
    std::vector<int64_t> beatTickSamples;
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

struct SampleWarpMarker
{
    int id = -1;
    int64_t samplePosition = 0;
    double beatPosition = 0.0;
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
    static constexpr int PreviewPointCount = 16384;

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
    TimeStretchBackend backend = TimeStretchBackend::Resample;
    double sourceSampleRate = 44100.0;
    juce::AudioBuffer<float> audioBuffer;
};

struct SampleModePersistentState
{
    juce::String samplePath;
    int visibleSliceBankIndex = 0;
    SampleSliceMode sliceMode = SampleSliceMode::Transient;
    SampleTriggerMode triggerMode = SampleTriggerMode::OneShot;
    bool useLegacyLoopEngine = false;
    int legacyLoopBarSelection = 0;
    int beatDivision = 1;
    float viewZoom = 1.0f;
    float viewScroll = 0.0f;
    int selectedCueIndex = -1;
    double analyzedTempoBpm = 0.0;
    double analyzedPitchHz = 0.0;
    int analyzedPitchMidi = -1;
    bool essentiaUsed = false;
    juce::String analysisSource;
    std::vector<SampleCuePoint> cuePoints;
    std::vector<SampleWarpMarker> warpMarkers;
    bool transientMarkersEdited = false;
    std::vector<int64_t> transientEditSamples;
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
                                                    int beatDivision,
                                                    const std::vector<int64_t>& beatTickSamples = {});
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
    using ProgressCallback = std::function<void(float progress, const juce::String& statusText)>;

    SampleAnalysisSummary analyzeLoadedSample(const juce::File& file,
                                             const juce::AudioBuffer<float>& audioBuffer,
                                             double sourceSampleRate,
                                             SamplePitchAnalysisProfile pitchProfile = SamplePitchAnalysisProfile::Polyphonic,
                                             ProgressCallback progressCallback = {}) const;
    void enrichAnalysisForFile(const juce::File& file,
                               LoadedSampleData& sampleData,
                               SamplePitchAnalysisProfile pitchProfile = SamplePitchAnalysisProfile::Polyphonic,
                               ProgressCallback progressCallback = {}) const;
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
    using ProgressCallback = std::function<void(int requestId, float progress, juce::String statusText)>;

    SampleFileManager();
    ~SampleFileManager();

    int loadFileAsync(const juce::File& file, LoadCallback callback, ProgressCallback progressCallback = {});
    void cancelPendingLoads();

private:
    class LoadJob;

    LoadResult decodeFile(const juce::File& file,
                          int requestId,
                          const std::function<void(float progress, const juce::String& statusText)>& progressCallback);

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
    static constexpr int MaxPlaybackVoices = 2;

    struct LegacyLoopSyncInfo
    {
        std::shared_ptr<const LoadedSampleData> loadedSample;
        std::array<SampleSlice, SliceModel::VisibleSliceCount> visibleSlices {};
        int visibleBankIndex = 0;
        int triggerVisibleSlot = -1;
        int64_t bankStartSample = -1;
        int64_t bankEndSample = -1;
        float visibleBankBeats = 0.0f;
        double analyzedTempoBpm = 0.0;
        int legacyLoopBarSelection = 0;
        SampleSliceMode sliceMode = SampleSliceMode::Transient;
        std::vector<int64_t> markerSamples;
        std::vector<SampleWarpMarker> warpMarkers;
    };

    struct RenderResult
    {
        bool renderedAnything = false;
        bool usedInternalPitch = false;
    };

    struct StateSnapshot
    {
        bool hasSample = false;
        bool isLoading = false;
        float analysisProgress = 0.0f;
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
        SampleSliceMode sliceMode = SampleSliceMode::Transient;
        SampleTriggerMode triggerMode = SampleTriggerMode::OneShot;
        bool useLegacyLoopEngine = false;
        int legacyLoopBarSelection = 0;
        int beatDivision = 1;
        float viewZoom = 1.0f;
        float viewScroll = 0.0f;
        int selectedCueIndex = -1;
        double estimatedTempoBpm = 0.0;
        double estimatedPitchHz = 0.0;
        int estimatedPitchMidi = -1;
        int estimatedScaleIndex = -1;
        bool essentiaUsed = false;
        juce::String analysisSource;
        std::vector<SampleCuePoint> cuePoints;
        std::vector<SampleWarpMarker> warpMarkers;
        std::vector<int64_t> transientMarkers;
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
    void randomizeVisibleBank();
    bool hasVisibleSlice(int visibleSlot) const;
    bool getVisibleSliceInfo(int visibleSlot, SampleSlice& sliceOut) const;
    bool getLegacyLoopSyncInfo(LegacyLoopSyncInfo& syncInfo) const;
    uint64_t getLegacyLoopRenderStateVersion() const noexcept;
    bool resolveLegacyLoopTriggerSyncInfo(int preferredVisibleSlot,
                                          int sliceId,
                                          int64_t sliceStartSample,
                                          LegacyLoopSyncInfo& syncInfo) const;
    int getActiveVisibleSliceSlot() const noexcept;
    int getPendingVisibleSliceSlot() const noexcept;
    void setPendingVisibleSlice(int visibleSlot);
    void clearPendingVisibleSlice();
    bool triggerVisibleSlice(int visibleSlot, bool forceLoop = false);
    bool triggerRecordedSlice(int preferredVisibleSlot,
                              int sliceId,
                              int64_t sliceStartSample,
                              bool forceLoop = false);
    void updateLegacyLoopMonitorState(bool isPlaying,
                                      int visibleSlot,
                                      float playbackProgressNormalized);
    void clearLegacyLoopMonitorState();
    RenderResult renderToBuffer(juce::AudioBuffer<float>& output,
                                int startSample,
                                int numSamples,
                                float playbackRate,
                                int fadeSamples,
                                float pitchSemitones,
                                bool preferHighQualityKeyLock);
    void requestKeyLockRenderCache(float playbackRate,
                                   float pitchSemitones,
                                   bool enabled,
                                   TimeStretchBackend backend);

    SampleModePersistentState capturePersistentState() const;
    void applyPersistentState(const SampleModePersistentState& state);

    void setSliceMode(SampleSliceMode mode);
    SampleSliceMode getSliceMode() const;
    void setTriggerMode(SampleTriggerMode mode);
    SampleTriggerMode getTriggerMode() const;
    void setLegacyLoopEngineEnabled(bool enabled);
    bool isLegacyLoopEngineEnabled() const;
    void setLegacyLoopBarSelection(int selection);
    int getLegacyLoopBarSelection() const;
    bool nudgeLegacyLoopWindowByAnchorDelta(int delta);
    void scaleAnalyzedTempo(double factor);
    double getAnalyzedTempoBpm() const;
    void setBeatDivision(int division);
    int getBeatDivision() const;
    void setViewWindow(float scroll, float zoom);
    float getViewScroll() const;
    float getViewZoom() const;
    void selectCuePoint(int cueIndex);
    int getSelectedCuePoint() const;
    int createCuePointAtNormalizedPosition(float normalizedPosition);
    int moveCuePoint(int cueIndex, float normalizedPosition);
    bool deleteCuePoint(int cueIndex);
    void beginInteractiveLegacyLoopEdit();
    void endInteractiveLegacyLoopEdit();
    void beginInteractiveWarpEdit();
    void endInteractiveWarpEdit();
    int createWarpMarkerAtNormalizedPosition(float normalizedPosition);
    int moveWarpMarker(int markerIndex, float normalizedPosition);
    int createWarpMarkerAtNearestGuide(float normalizedPosition);
    int createWarpMarkersFromVisibleTransientClusters();
    int moveWarpMarkerToNearestGuide(int markerIndex, float normalizedPosition);
    bool clearWarpMarkers();
    bool keepBoundaryWarpMarkers();
    bool deleteWarpMarker(int markerIndex);
    int createTransientMarkerAtNormalizedPosition(float normalizedPosition);
    int moveTransientMarker(int markerIndex, float normalizedPosition);
    int moveTransientMarkerToNearestDetectedTransient(int markerIndex, float normalizedPosition);
    int resolveTransientMarkerIndexForVisibleSlot(int visibleSlot);
    bool moveVisibleSliceToPosition(int visibleSlot, float normalizedPosition);
    bool moveVisibleSliceToNearestTransient(int visibleSlot, float normalizedPosition);
    bool deleteTransientMarker(int markerIndex);
    bool snapSlicePointsToNearestPpqGrid(double gridStepBeats, bool currentSelectionOnly);
    bool canUndoSliceEdit() const;
    bool undoLastSliceEdit();
    void beginSliceEditGesture();
    void endSliceEditGesture();

    using LoadStatusCallback = std::function<void(const SampleFileManager::LoadResult&)>;
    using LoadProgressCallback = std::function<void(int requestId, float progress, juce::String statusText)>;
    using LegacyLoopRenderStateChangedCallback = std::function<void()>;
    void setLoadStatusCallback(LoadStatusCallback callback);
    void setLegacyLoopRenderStateChangedCallback(LegacyLoopRenderStateChangedCallback callback);

private:
    void notifyLegacyLoopRenderStateChanged();
    bool resolveVisibleSlice(int visibleSlot, SampleSlice& sliceOut) const;
    std::array<SampleSlice, SliceModel::VisibleSliceCount> getCurrentVisibleSlicesLocked() const;
    bool computeLegacyLoopWindowRangeLocked(int64_t& rangeStartSample, int64_t& rangeEndSample) const;
    int findTransientMarkerIndexForVisibleSlotLocked(int visibleSlot);
    std::vector<int64_t> buildCanonicalTransientMarkerSamplesLocked() const;
    std::vector<int64_t> buildCanonicalTransientMarkerSamplesForRangeLocked(int64_t rangeStartSample,
                                                                            int64_t rangeEndSample) const;
    std::vector<int64_t> buildLegacyLoopWindowTransientMarkerSamplesLocked(int64_t rangeStartSample,
                                                                           int64_t rangeEndSample) const;
    std::array<SampleSlice, SliceModel::VisibleSliceCount> buildLegacyLoopWindowVisibleSlicesLocked(int64_t rangeStartSample,
                                                                                                     int64_t rangeEndSample) const;
    std::vector<int64_t> buildLegacyLoopWindowTransientCandidatesLocked(int64_t rangeStartSample,
                                                                        int64_t rangeEndSample) const;
    std::vector<int64_t> buildTransientGuideCandidatesLocked(int64_t lowerBound,
                                                             int64_t upperBound,
                                                             bool preferLocalTransients) const;
    int64_t snapTransientMarkerSampleToGuideLocked(int64_t targetSample,
                                                   int64_t lowerBound,
                                                   int64_t upperBound,
                                                   bool preferLocalTransients) const;
    void quantizeTransientEditSamplesToGuidesLocked();
    std::vector<int64_t> buildTransientSnapCandidatesLocked(int64_t targetSample,
                                                            int64_t lowerBound,
                                                            int64_t upperBound) const;
    int64_t getDefaultLegacyLoopWindowStartSampleLocked() const;
    std::vector<int64_t> buildLegacyLoopAnchorCandidatesLocked() const;
    void fitViewToLegacyLoopWindowLocked();
    void clearRandomVisibleSliceOverrideLocked();
    void resetSampleSpecificStateLocked();
    bool buildLegacyLoopSyncInfoForSliceIndexLocked(int sliceIndex, LegacyLoopSyncInfo& syncInfo) const;
    void handleLoadResult(SampleFileManager::LoadResult result);
    void handleLoadProgress(int requestId, float progress, const juce::String& statusText);
    void handleKeyLockCacheReady(uint64_t generation,
                                 std::vector<std::shared_ptr<const StretchedSliceBuffer>> caches,
                                 float playbackRate,
                                 float pitchSemitones,
                                 int visibleBankIndex);
    void materializeTransientMarkersLocked();
    void rebuildSlicesLocked();
    void pushSliceEditUndoStateLocked();
    void clearSliceEditUndoHistoryLocked();
    void invalidateCanonicalTransientMarkerCacheLocked();
    void invalidateLegacyLoopTransientWindowCacheLocked();
    void invalidateTransientMarkerCachesLocked();
    int64_t snapSampleToPpqGridLocked(int64_t samplePosition,
                                      double gridStepBeats,
                                      int64_t lowerBound,
                                      int64_t upperBound) const;
    bool snapTransientMarkersToPpqGridLocked(double gridStepBeats, bool currentSelectionOnly);
    bool snapCuePointsToPpqGridLocked(double gridStepBeats, bool currentSelectionOnly);
    bool triggerResolvedSlice(const SampleSlice& slice, int resolvedVisibleSlot, SampleTriggerMode triggerMode);
    bool resolveRecordedSlice(int sliceId,
                              int64_t sliceStartSample,
                              SampleSlice& sliceOut,
                              int& resolvedVisibleSlot) const;
    std::shared_ptr<const StretchedSliceBuffer> findKeyLockCacheForSlice(const SampleSlice& slice,
                                                                         float playbackRate,
                                                                         float pitchSemitones,
                                                                         TimeStretchBackend backend) const;
    bool voiceHasMatchingKeyLockCache(const SamplePlaybackVoice& voice,
                                      float playbackRate,
                                      float pitchSemitones,
                                      TimeStretchBackend backend) const;
    static int clampCueSelection(int selectedCueIndex, int cueCount);

    mutable juce::CriticalSection stateLock;
    juce::SpinLock playbackLock;
    struct SliceEditUndoState
    {
        std::vector<SampleCuePoint> cuePoints;
        int selectedCueIndex = -1;
        bool transientMarkersEdited = false;
        std::vector<int64_t> transientEditSamples;
        std::vector<SampleWarpMarker> warpMarkers;
    };
    SampleFileManager fileManager;
    juce::ThreadPool keyLockCacheThreadPool { 1 };
    SampleAnalysisEngine analysisEngine;
    SliceModel sliceModel;
    SampleModePersistentState persistentState;
    std::array<SamplePlaybackVoice, MaxPlaybackVoices> playbackVoices;
    std::shared_ptr<const LoadedSampleData> loadedSample;
    LoadStatusCallback loadStatusCallback;
    LegacyLoopRenderStateChangedCallback legacyLoopRenderStateChangedCallback;
    std::atomic<uint64_t> legacyLoopRenderStateVersion { 1 };
    bool isLoading = false;
    float analysisProgress = 0.0f;
    juce::String statusText { "No sample loaded" };
    int activeRequestId = 0;
    bool pendingTriggerSliceValid = false;
    SampleSlice pendingTriggerSlice;
    bool interactiveLegacyLoopEditActive = false;
    bool interactiveLegacyLoopEditDirty = false;
    bool interactiveWarpEditActive = false;
    bool interactiveWarpEditDirty = false;
    bool randomVisibleSliceOverrideActive = false;
    std::array<SampleSlice, SliceModel::VisibleSliceCount> randomVisibleSlices {};
    int64_t legacyLoopWindowStartSample = -1;
    bool legacyLoopWindowManualAnchor = false;
    int legacyLoopTransientPageStartIndex = 0;
    std::vector<SliceEditUndoState> sliceEditUndoStack;
    bool sliceEditGestureActive = false;
    bool sliceEditGestureUndoCaptured = false;
    std::atomic<double> preparedSampleRate { 44100.0 };
    std::atomic<int> preparedMaxBlockSize { 512 };
    std::atomic<int> activeVisibleSliceSlot { -1 };
    std::atomic<int> pendingVisibleSliceSlot { -1 };
    std::atomic<float> playbackProgress { -1.0f };
    std::atomic<int> playingAtomic { 0 };
    std::atomic<int> legacyLoopActiveVisibleSliceSlot { -1 };
    std::atomic<float> legacyLoopPlaybackProgress { -1.0f };
    std::atomic<int> legacyLoopPlayingAtomic { 0 };
    uint64_t nextVoiceId = 1;
    bool keyLockCacheEnabled = false;
    bool keyLockCacheBuildInFlight = false;
    uint64_t keyLockCacheGeneration = 0;
    float keyLockCachePlaybackRate = 1.0f;
    float keyLockCachePitchSemitones = 0.0f;
    TimeStretchBackend keyLockCacheBackend = TimeStretchBackend::SoundTouch;
    int keyLockCacheVisibleBankIndex = -1;
    std::vector<std::shared_ptr<const StretchedSliceBuffer>> keyLockCaches;
    mutable bool canonicalTransientMarkerCacheValid = false;
    mutable std::vector<int64_t> canonicalTransientMarkerCache;
    struct LegacyLoopTransientWindowCache
    {
        bool valid = false;
        int64_t rangeStartSample = -1;
        int64_t rangeEndSample = -1;
        std::vector<int64_t> markerSamples;
        bool visibleSlicesValid = false;
        int visibleSlicesPageStart = -1;
        std::array<SampleSlice, SliceModel::VisibleSliceCount> visibleSlices {};
    };
    mutable LegacyLoopTransientWindowCache legacyLoopTransientWindowCache;

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
    void setWaveformColour(juce::Colour colour);

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& event) override;
    void mouseDoubleClick(const juce::MouseEvent& event) override;
    void mouseDrag(const juce::MouseEvent& event) override;
    void mouseUp(const juce::MouseEvent& event) override;
    void mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) override;
    bool keyPressed(const juce::KeyPress& key) override;
    void changeListenerCallback(juce::ChangeBroadcaster* source) override;

    std::function<void(int visibleSlot)> onTriggerVisibleSlice;
    std::function<void(int delta)> onNavigateVisibleBank;
    std::function<void()> onRequestLoad;
    std::function<void()> onCopyToLoop;
    std::function<void()> onCopyToGrain;
    std::function<void()> onRequestLegacyLoopBarsMenu;

private:
    void refreshFromEngine();
    int64_t getVisibleSliceMarkerSample(int visibleSlot) const;
    int hitTestVisibleSlice(const juce::Point<int>& point) const;
    int hitTestVisibleSliceMarker(const juce::Point<int>& point) const;
    int hitTestExactVisibleSliceMarkerHandle(const juce::Point<int>& point) const;
    int hitTestCuePoint(const juce::Point<int>& point) const;
    int hitTestWarpMarker(const juce::Point<int>& point) const;
    int hitTestWarpMarker(const juce::Point<int>& point, float hitRadius) const;
    int hitTestExactWarpMarkerHandle(const juce::Point<int>& point) const;
    int hitTestWarpGuideMarker(const juce::Point<int>& point) const;
    int findCuePointForVisibleSlice(int visibleSlot) const;
    int findTransientMarkerForVisibleSlice(int visibleSlot) const;
    float normalizedPositionFromPoint(const juce::Point<int>& point) const;
    juce::Range<float> getVisibleRange() const;
    juce::Range<float> getVisibleDisplayRange() const;
    juce::Rectangle<int> getWarpMarkerLaneBounds() const;
    juce::Rectangle<int> getSliceMarkerHandleBounds() const;
    juce::Rectangle<int> getSliceModeButtonBounds(SampleSliceMode mode) const;
    juce::Rectangle<int> getTriggerModeButtonBounds(SampleTriggerMode mode) const;
    juce::Rectangle<int> getLegacyLoopButtonBounds() const;
    juce::Rectangle<int> getLegacyLoopBarsButtonBounds() const;
    juce::Rectangle<int> getVisibleLegacyLoopRangeBounds() const;
    void showWarpContextMenu(const juce::Point<int>& point);
    void showSliceContextMenu(const juce::Point<int>& point);
    double getCurrentPpqGridStepBeats() const;
    void timerCallback() override;

    SampleModeEngine* engine = nullptr;
    SampleModeEngine::StateSnapshot stateSnapshot;
    std::shared_ptr<const LoadedSampleData> loadedSample;
    juce::Rectangle<int> waveformBounds;
    juce::Rectangle<int> prev16Bounds;
    juce::Rectangle<int> next16Bounds;
    juce::Rectangle<int> random16Bounds;
    juce::Rectangle<int> tempoHalfBounds;
    juce::Rectangle<int> tempoDoubleBounds;
    juce::Rectangle<int> copyLoopBounds;
    juce::Rectangle<int> copyGrainBounds;
    juce::Rectangle<int> legacyLoopBarsBounds;
    juce::Rectangle<int> sliceModeArea;
    juce::Rectangle<int> triggerModeArea;
    juce::Rectangle<int> legacyLoopArea;
    bool draggingLegacyLoopWindow = false;
    int legacyLoopDragStartX = 0;
    int legacyLoopDragStepOffset = 0;
    int draggingVisibleSliceSlot = -1;
    int draggingCueIndex = -1;
    int draggingWarpMarkerIndex = -1;
    juce::Point<int> draggingWarpMouseDownPoint;
    int draggingTransientIndex = -1;
    bool createdCueOnMouseDown = false;
    juce::Colour waveformColour { juce::Colour(0xff6ca7ff) };

    int hitTestTransientMarker(const juce::Point<int>& point) const;
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
