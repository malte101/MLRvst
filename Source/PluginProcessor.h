/*
  ==============================================================================

    PluginProcessor.h
    mlrVST - Modern Edition
    
    Complete modernization for JUCE 8.x with advanced audio engine

  ==============================================================================
*/

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_osc/juce_osc.h>
#include <cstdint>
#include "PerformanceTargets.h"
#include "AudioEngine.h"
#include "SampleMode.h"

//==============================================================================
/**
 * MonomeConnection - Handles serialosc protocol communication
 * 
 * Complete serialosc protocol implementation:
 * - Device discovery and enumeration
 * - Automatic connection and reconnection
 * - All LED control methods (set, all, row, col, map, level row/col/map)
 * - Grid key input
 * - System configuration (prefix, rotation, size, info)
 * - Device hot-plug support
 */
class MonomeConnection : public juce::OSCReceiver::Listener<juce::OSCReceiver::RealtimeCallback>,
                          public juce::Timer
{
public:
    MonomeConnection();
    ~MonomeConnection() override;
    
    void connect(int applicationPort);
    void disconnect();
    bool isConnected() const { return gridEndpoint.connected || arcEndpoint.connected; }
    
    // SerialOSC device discovery
    void discoverDevices();
    void selectDevice(int deviceIndex);
    void selectGridDevice(int deviceIndex);
    void selectArcDevice(int deviceIndex);
    void refreshDeviceList();
    
    // LED control - Basic (0/1 states)
    void setLED(int x, int y, int state);
    void setAllLEDs(int state);
    void setLEDRow(int xOffset, int y, const std::array<int, 8>& data);
    void setLEDColumn(int x, int yOffset, const std::array<int, 8>& data);
    void setLEDMap(int xOffset, int yOffset, const std::array<int, 8>& data);
    
    // LED control - Variable brightness (0-15)
    void setLEDLevel(int x, int y, int level);
    void setAllLEDLevels(int level);
    void setLEDLevelRow(int xOffset, int y, const std::array<int, 8>& levels);
    void setLEDLevelColumn(int x, int yOffset, const std::array<int, 8>& levels);
    void setLEDLevelMap(int xOffset, int yOffset, const std::array<int, 64>& levels);
    void setArcRingMap(int encoder, const std::array<int, 64>& levels);
    void setArcRingLevel(int encoder, int ledIndex, int level);
    void setArcRingRange(int encoder, int start, int end, int level);

    bool supportsGrid() const;
    bool supportsArc() const;
    int getArcEncoderCount() const;
    
    // System commands
    void setRotation(int degrees); // 0, 90, 180, 270
    void setPrefix(const juce::String& newPrefix);
    void requestInfo();
    void requestSize();
    
    // Tilt support (for grids with tilt sensors)
    void enableTilt(int sensor, bool enable);
    
    // Device info
    struct DeviceInfo
    {
        juce::String id;
        juce::String type;
        int port;
        int sizeX = 16;
        int sizeY = 8;
        bool hasTilt = false;
        juce::String host = "127.0.0.1";
    };
    
    std::vector<DeviceInfo> getDiscoveredDevices() const { return devices; }
    DeviceInfo getCurrentDevice() const { return supportsGrid() ? gridEndpoint.device : arcEndpoint.device; }
    DeviceInfo getCurrentGridDevice() const { return gridEndpoint.device; }
    DeviceInfo getCurrentArcDevice() const { return arcEndpoint.device; }
    juce::String getConnectionStatus() const;
    juce::String getGridConnectionStatus() const;
    juce::String getArcConnectionStatus() const;
    
    // Callbacks
    std::function<void(int x, int y, int state)> onKeyPress;
    std::function<void(int sensor, int x, int y, int z)> onTilt;
    std::function<void(int encoder, int delta)> onArcDelta;
    std::function<void(int encoder, int state)> onArcKey;
    std::function<void()> onDeviceConnected;
    std::function<void()> onDeviceDisconnected;
    std::function<void(const std::vector<DeviceInfo>&)> onDeviceListUpdated;
    
private:
    enum class DeviceRole
    {
        Grid,
        Arc
    };

    struct EndpointState
    {
        juce::OSCSender sender;
        DeviceInfo device;
        bool connected = false;
        int reconnectAttempts = 0;
        juce::int64 lastConnectAttemptTime = 0;
        juce::int64 lastPingTime = 0;
    };

    void oscMessageReceived(const juce::OSCMessage& message) override;
    void handleSerialOSCMessage(const juce::OSCMessage& message);
    void handleGridMessage(const juce::OSCMessage& message);
    void handleSystemMessage(const juce::OSCMessage& message);
    void handleTiltMessage(const juce::OSCMessage& message);
    void handleArcMessage(const juce::OSCMessage& message);
    
    void timerCallback() override;
    void attemptReconnection(DeviceRole role);
    void sendPing(DeviceRole role);
    void markDisconnected(DeviceRole role);
    void configureEndpoint(DeviceRole role);
    void connectEndpoint(DeviceRole role);
    EndpointState& endpointForRole(DeviceRole role);
    const EndpointState& endpointForRole(DeviceRole role) const;
    bool deviceMatchesRole(const DeviceInfo& device, DeviceRole role) const;
    juce::String prefixForRole(DeviceRole role) const;
    void autoSelectAvailableDevices();
    
    juce::OSCSender serialoscSender; // Separate sender for serialosc queries
    juce::OSCReceiver oscReceiver;
    
    std::vector<DeviceInfo> devices;
    EndpointState gridEndpoint;
    EndpointState arcEndpoint;
    
    juce::String oscPrefix = "/monome";
    int applicationPort = 8000;
    bool receiverConnected = false;
    bool autoReconnect = true;
    
    static constexpr int maxReconnectAttempts = 10;
    static constexpr int reconnectIntervalMs = 2000;
    static constexpr int discoveryIntervalMs = 2000;
    
    juce::int64 lastDiscoveryTime = 0;
    static constexpr int pingIntervalMs = 5000;
    
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MonomeConnection)
};

//==============================================================================
/**
 * MlrVSTAudioProcessor - Main plugin processor
 */
class MlrVSTAudioProcessor : public juce::AudioProcessor,
                             public juce::Timer,
                             public juce::AudioProcessorValueTreeState::Listener
{
public:
    MlrVSTAudioProcessor();
    ~MlrVSTAudioProcessor() override;

    //==============================================================================
    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;

    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void parameterChanged(const juce::String& parameterID, float newValue) override;

    //==============================================================================
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    //==============================================================================
    const juce::String getName() const override;

    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    //==============================================================================
    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram(int index) override;
    const juce::String getProgramName(int index) override;
    void changeProgramName(int index, const juce::String& newName) override;

    void markPersistentGlobalUserChange();
    void queuePersistentGlobalControlsSave();

    //==============================================================================
    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    //==============================================================================
    // Public API
    ModernAudioEngine* getAudioEngine() { return audioEngine.get(); }
    MonomeConnection& getMonomeConnection() { return monomeConnection; }
    
    bool loadSampleToStrip(int stripIndex, const juce::File& file);
    bool loadSampleToStripPreservingPlaybackState(int stripIndex, const juce::File& file);
    bool loadSampleToSampleModeStrip(int stripIndex, const juce::File& file);
    SampleModeEngine* getSampleModeEngine(int stripIndex, bool createIfMissing = true);
    bool hasSampleModeAudio(int stripIndex) const;
    void loadAdjacentFile(int stripIndex, int direction);  // Browse files
    void captureRecentAudioToStrip(int stripIndex);
    void clearRecentAudioBuffer();
    void requestBarLengthChange(int stripIndex, int bars);
    bool canChangeBarLengthNow(int stripIndex) const;
    void setPendingBarLengthApply(int stripIndex, bool pending);
    void triggerStrip(int stripIndex, int column);
    void stopStrip(int stripIndex);
    bool copyFlipCurrentSlicesToMode(int stripIndex, EnhancedAudioStrip::PlayMode targetMode);
    bool copyFlipCurrentSlicesToMode(int sourceStripIndex, int targetStripIndex, EnhancedAudioStrip::PlayMode targetMode);
    void setSampleModeHeldVisibleSliceSlot(int stripIndex, int visibleSlot);
    void clearSampleModeHeldVisibleSliceSlot(int stripIndex, int visibleSlot = -1);
    int getSampleModeHeldVisibleSliceSlot(int stripIndex) const;
    
    enum class SamplePathMode
    {
        Loop,
        Step,
        Flip
    };
    static constexpr int BrowserFavoriteSlots = 6;
    juce::File getDefaultSampleDirectory(int stripIndex, SamplePathMode mode) const;
    void setDefaultSampleDirectory(int stripIndex, SamplePathMode mode, const juce::File& directory);
    void setCurrentBrowserDirectoryForStrip(int stripIndex, SamplePathMode mode, const juce::File& directory);
    juce::File getCurrentBrowserDirectoryForStrip(int stripIndex) const;
    juce::File getCurrentBrowserDirectoryForStrip(int stripIndex, SamplePathMode mode) const;
    juce::String getStripDisplaySampleName(int stripIndex);
    juce::File getBrowserFavoriteDirectory(int stripIndex, int slot) const;
    bool isBrowserFavoritePadHeld(int stripIndex, int slot) const;
    bool isBrowserFavoriteSaveBurstActive(int slot, uint32_t nowMs) const;
    bool isBrowserFavoriteMissingBurstActive(int slot, uint32_t nowMs) const;
    void beginBrowserFavoritePadHold(int stripIndex, int slot);
    void endBrowserFavoritePadHold(int stripIndex, int slot);

    enum class PitchControlMode
    {
        PitchShift = 0,
        Resample = 1
    };
    enum class FlipTempoMatchMode
    {
        Repitch = 0,
        MlrTs
    };
    enum class StripTempoMatchMode
    {
        Global = 0,
        Repitch,
        MlrTs
    };
    PitchControlMode getPitchControlMode() const;
    TimeStretchBackend getStretchBackend() const;
    TimeStretchBackend getLoopTempoMatchBackend() const;
    StripTempoMatchMode getStripTempoMatchMode(int stripIndex) const;
    TimeStretchBackend resolveLoopTempoMatchBackendForStrip(int stripIndex) const;
    FlipTempoMatchMode getFlipTempoMatchMode() const;
    TimeStretchBackend getFlipTempoMatchBackend() const;
    bool usesTimeStretchBackend() const { return getStretchBackend() != TimeStretchBackend::Resample; }
    bool isPitchControlResampleMode() const { return getPitchControlMode() == PitchControlMode::Resample; }
    void applyPitchControlToStrip(EnhancedAudioStrip& strip, float semitones);
    void applyPitchControlToStrip(int stripIndex, EnhancedAudioStrip& strip, float semitones);
    void applyUserPitchControlToStrip(int stripIndex, float semitones);
    float getPitchSemitonesForDisplay(const EnhancedAudioStrip& strip) const;
    float quantizePitchSemitonesForStripControl(int stripIndex, float semitones) const;
    bool requestLoopStripPitchMaster(int stripIndex);
    bool requestLoopStripPitchSync(int stripIndex);
    bool isLoopStripLoadInFlight(int stripIndex) const;
    float getLoopStripLoadProgress(int stripIndex) const;
    juce::String getLoopStripLoadStatusText(int stripIndex) const;
    bool isLoopStripPitchAnalysisInFlight(int stripIndex) const;
    float getLoopStripPitchAnalysisProgress(int stripIndex) const;
    juce::String getLoopStripPitchAnalysisStatusText(int stripIndex) const;
    int getLoopStripDetectedPitchMidi(int stripIndex) const;
    float getLoopStripDetectedPitchHz(int stripIndex) const;
    int getLoopStripDetectedScaleIndex(int stripIndex) const;
    float getLoopStripDetectedPitchConfidence(int stripIndex) const;
    float getLoopStripDetectedScaleConfidence(int stripIndex) const;
    bool isLoopStripAssignedPitchManual(int stripIndex) const;
    bool didLoopStripPitchUseEssentia(int stripIndex) const;
    enum class LoopPitchRole
    {
        None = 0,
        Master,
        Sync
    };
    enum class LoopPitchSyncTiming
    {
        Immediate = 0,
        NextTrigger,
        NextLoop,
        NextBar
    };
    LoopPitchRole getLoopPitchRole(int stripIndex) const;
    void setLoopPitchRole(int stripIndex, LoopPitchRole role);
    LoopPitchSyncTiming getLoopPitchSyncTiming(int stripIndex) const;
    void setLoopPitchSyncTiming(int stripIndex, LoopPitchSyncTiming timing);
    int getLoopStripAssignedPitchMidi(int stripIndex) const;
    float getLoopStripPitchSyncCorrectionSemitones(int stripIndex) const;
    void setLoopStripAssignedPitchMidi(int stripIndex, int midiNote, bool manualOverride = true);
    void updateGlobalRootFromLoopPitchMaster(int stripIndex, bool markPersistent = true);
    int getGlobalRootNoteMidi() const { return globalRootNoteMidi.load(std::memory_order_acquire); }
    int getGlobalRootNotePitchClass() const;
    void setGlobalRootNoteMidi(int midiNote, bool markPersistent = true);
    void setGlobalRootNotePitchClass(int pitchClass);
    ModernAudioEngine::PitchScale getGlobalPitchScale() const;
    void setGlobalPitchScale(ModernAudioEngine::PitchScale scale);
    float quantizePitchSemitonesToGlobalScale(float semitones) const;
    struct LoopPitchAnalysisResult
    {
        int stripIndex = -1;
        int requestId = 0;
        bool setAsRoot = false;
        bool success = false;
        int detectedMidi = -1;
        double detectedHz = 0.0;
        float detectedPitchConfidence = 0.0f;
        int detectedScaleIndex = -1;
        float detectedScaleConfidence = 0.0f;
        bool essentiaUsed = false;
        juce::String analysisSource;
    };
    struct LoopStripLoadResult
    {
        int stripIndex = -1;
        int requestId = 0;
        bool success = false;
        juce::File sourceFile;
        juce::AudioBuffer<float> decodedBuffer;
        double sourceSampleRate = 44100.0;
        int detectedBars = 1;
        float detectedBeatsForLoop = 4.0f;
        juce::AudioBuffer<float> preparedTempoMatchBuffer;
        double preparedTempoMatchHostTempo = -1.0;
        TimeStretchBackend preparedTempoMatchBackend = TimeStretchBackend::Resample;
        juce::String errorMessage;
    };
    
    // Control mode (for GUI to check if level/pan/etc controls are active)
    enum class ControlMode
    {
        Normal,
        Speed,
        Pitch,
        Pan,
        Volume,
        GrainSize,
        Filter,
        Swing,
        Gate,
        FileBrowser,
        GroupAssign,
        Modulation,
        Preset,
        StepEdit
    };
    ControlMode getCurrentControlMode() const { return currentControlMode; }
    bool isControlModeActive() const { return controlModeActive; }
    static juce::String getControlModeName(ControlMode mode);
    static constexpr int NumControlRowPages = 13;
    using ControlPageOrder = std::array<ControlMode, NumControlRowPages>;
    ControlPageOrder getControlPageOrder() const;
    ControlMode getControlModeForControlButton(int buttonIndex) const;
    int getControlButtonForMode(ControlMode mode) const;
    void moveControlPage(int fromIndex, int toIndex);
    bool isControlPageMomentary() const { return controlPageMomentary.load(std::memory_order_acquire); }
    void setControlPageMomentary(bool shouldBeMomentary);
    void setControlModeFromGui(ControlMode mode, bool shouldBeActive);
    void setSwingDivisionSelection(int mode);
    int getSwingDivisionSelection() const { return swingDivisionSelection.load(std::memory_order_acquire); }
    int getLastMonomePressedStripRow() const { return lastMonomePressedStripRow.load(std::memory_order_acquire); }
    int getArcSelectedStripRow() const { return arcSelectedStripRow.load(std::memory_order_acquire); }
    void setArcSelectedStripRow(int stripIndex)
    {
        arcSelectedStripRow.store(juce::jlimit(0, ModernAudioEngine::MaxStrips - 1, stripIndex),
                                  std::memory_order_release);
    }
    bool isStepEditModeActive() const
    {
        return controlModeActive && currentControlMode == ControlMode::StepEdit;
    }
    int getStepEditToolIndex() const
    {
        switch (stepEditTool)
        {
            case StepEditTool::Gate: return 0;
            case StepEditTool::Velocity: return 1;
            case StepEditTool::Divide: return 2;
            case StepEditTool::RampUp: return 3;
            case StepEditTool::RampDown: return 4;
            case StepEditTool::Probability: return 5;
            case StepEditTool::Attack: return 6;
            case StepEditTool::Decay: return 7;
            case StepEditTool::Release: return 8;
            default: return 0;
        }
    }
    int getStepEditSelectedStrip() const { return juce::jlimit(0, MaxStrips - 1, stepEditSelectedStrip); }
    int getMonomeGridWidth() const;
    int getMonomeGridHeight() const;
    int getMonomeControlRow() const;
    int getMonomeActiveStripCount() const;
    
    // Preset management
    void savePreset(int presetIndex);
    void loadPreset(int presetIndex);
    bool deletePreset(int presetIndex);
    void initRuntimeStateToDefaults();
    int getLoadedPresetIndex() const { return loadedPresetIndex; }
    juce::String getPresetName(int presetIndex) const;
    bool setPresetName(int presetIndex, const juce::String& name);
    bool presetExists(int presetIndex) const;
    bool isSceneModeEnabled() const { return sceneModeEnabled.load(std::memory_order_acquire) != 0; }
    void setSceneModeEnabled(bool enabled);
    int getActiveSceneSlot() const { return juce::jlimit(0, SceneSlots - 1, activeSceneSlot); }
    static constexpr int SceneSlots = 4;
    static constexpr int MaxSceneRepeatCount = 16;
    static constexpr int MaxSceneManualBars = 32;

    enum class SceneLengthMode
    {
        LongestStrip = 0,
        LongestPattern,
        ManualBars,
        AnchorStrip
    };

    int getSceneRepeatCount(int sceneSlot) const;
    void setSceneRepeatCount(int sceneSlot, int repeats);
    SceneLengthMode getSceneLengthMode(int sceneSlot) const;
    void setSceneLengthMode(int sceneSlot, SceneLengthMode mode);
    int getSceneManualBars(int sceneSlot) const;
    void setSceneManualBars(int sceneSlot, int bars);
    int getSceneAnchorStrip(int sceneSlot) const;
    void setSceneAnchorStrip(int sceneSlot, int stripIndex);
    double getResolvedSceneLengthBeats(int sceneSlot) const;
    bool captureSceneSlot(int sceneSlot);
    bool insertSceneSlot(int sceneSlot, bool insertAfter);
    uint32_t getPresetRefreshToken() const { return presetRefreshToken.load(std::memory_order_acquire); }
    static constexpr int PresetColumns = 16;
    static constexpr int PresetRows = 7;
    static constexpr int MaxPresetSlots = PresetColumns * PresetRows;
    static constexpr int MacroCount = 8;

    using MacroTarget = PerformanceTarget;

    struct MacroState
    {
        int stripIndex = 0;
        bool hasTargetStrip = false;
        std::array<float, MacroCount> values{};
    };

    struct MacroLaneRecordStatus
    {
        bool armed = false;
        bool recording = false;
        int stripIndex = -1;
        int laneSlot = 0;
        MacroTarget target = MacroTarget::None;
        int stepsWritten = 0;
        int totalSteps = 0;
    };

    MacroState getMacroState() const;
    void setSelectedStripMacroValue(int macroIndex, float normalizedValue);
    int getMacroMidiCc(int macroIndex) const;
    MacroTarget getMacroTarget(int macroIndex) const;
    int getMacroMidiLearnIndex() const { return macroMidiLearnIndex.load(std::memory_order_acquire); }
    void beginMacroMidiLearn(int macroIndex);
    void cancelMacroMidiLearn();
    void resetMacroMidiCcToDefault(int macroIndex);
    static int getDefaultMacroMidiCc(int macroIndex);
    void setMacroTarget(int macroIndex, MacroTarget target);
    static MacroTarget getDefaultMacroTarget(int macroIndex);
    static int getDefaultModLaneSlotForMacroTarget(MacroTarget target);
    static float getDefaultMacroNormalizedValue(MacroTarget target);
    MacroLaneRecordStatus getMacroLaneRecordStatus(int macroIndex) const;
    void beginMacroLaneRecording(int macroIndex, int laneSlot);
    void cancelMacroLaneRecording(int macroIndex);
    
    // Parameters
    juce::AudioProcessorValueTreeState parameters;
    
    static constexpr int MaxStrips = ModernAudioEngine::MaxStrips;
    static constexpr int MaxColumns = 16;
    static constexpr int MaxGridWidth = 16;
    static constexpr int MaxGridHeight = 16;

private:
    //==============================================================================
    enum class FilterSubPage
    {
        Frequency,    // Button 0 on group row
        Resonance,    // Button 1 on group row
        Type          // Button 2 on group row
    };

    enum class StepEditTool
    {
        Gate,
        Velocity,
        Divide,
        RampUp,
        RampDown,
        Probability,
        Attack,
        Decay,
        Release
    };

    enum class ArcControlMode
    {
        SelectedStrip,
        Modulation
    };

    struct ResolvedPitchControl
    {
        int globalRootMidi = 60;
        ModernAudioEngine::PitchScale globalScale = ModernAudioEngine::PitchScale::Chromatic;
        float quantizedSemitones = 0.0f;
        float resampleRatio = 1.0f;
        float stepSamplerRatio = 1.0f;
        bool updatesStepSampler = false;
        bool useResamplePitch = false;
    };

    struct ResolvedFlipTempoMatch
    {
        FlipTempoMatchMode mode = FlipTempoMatchMode::Repitch;
        TimeStretchBackend backend = TimeStretchBackend::Resample;

        bool usesTimeStretch() const noexcept { return backend != TimeStretchBackend::Resample; }
        bool usesRepitch() const noexcept { return backend == TimeStretchBackend::Resample; }
    };

    struct ResolvedFlipPlaybackState
    {
        ResolvedFlipTempoMatch tempoMatch;
        float tempoMatchRatio = 1.0f;
        float playbackRate = 1.0f;
        float internalPitchSemitones = 0.0f;
        bool keyLockEnabled = false;
        bool shouldBuildKeyLockCache = false;
        bool preferHighQualityKeyLock = false;
    };

    struct MacroLaneRecordState
    {
        std::atomic<int> armed{0};
        std::atomic<int> recording{0};
        std::atomic<int> stripIndex{-1};
        std::atomic<int> macroIndex{-1};
        std::atomic<int> laneSlot{0};
        std::atomic<int> target{static_cast<int>(MacroTarget::None)};
        std::atomic<float> restoreDepth{1.0f};
        std::atomic<double> startPpq{-1.0};
        std::atomic<double> nextStepPpq{-1.0};
        std::atomic<int> nextStepIndex{-1};
        std::atomic<int> stepsWritten{0};
        std::atomic<int> totalStepsToWrite{0};
    };

    ResolvedPitchControl resolvePitchControl(const EnhancedAudioStrip& strip,
                                             float semitones,
                                             int referenceRootMidi) const;
    void applyResolvedPitchControl(EnhancedAudioStrip& strip,
                                   const ResolvedPitchControl& resolved) const;
    ResolvedFlipTempoMatch resolveFlipTempoMatch() const;
    ResolvedFlipPlaybackState resolveFlipPlaybackState(const EnhancedAudioStrip& strip,
                                                       const SampleModeEngine& engine) const;
    
    std::unique_ptr<ModernAudioEngine> audioEngine;
    MonomeConnection monomeConnection;

    struct PendingLoopChange
    {
        bool active = false;
        bool clear = false;
        int startColumn = 0;
        int endColumn = MaxColumns;
        int markerColumn = -1;
        bool reverse = false;
        bool quantized = false;
        double targetPpq = 0.0;
        int quantizeDivision = 8;
        bool postClearTriggerArmed = false;
        int postClearTriggerColumn = 0;
    };

    struct PendingBarChange
    {
        bool active = false;
        int recordingBars = 1;
        float beatsPerLoop = 4.0f;
        bool quantized = false;
        double targetPpq = 0.0;
        int quantizeDivision = 8;
    };

    // Cached parameter pointers to avoid string lookups in processBlock
    std::atomic<float>* masterVolumeParam = nullptr;
    std::atomic<float>* limiterThresholdParam = nullptr;
    std::atomic<float>* limiterEnabledParam = nullptr;
    std::atomic<float>* quantizeParam = nullptr;
    std::atomic<float>* innerLoopLengthParam = nullptr;
    std::atomic<float>* grainQualityParam = nullptr;
    std::atomic<float>* pitchSmoothingParam = nullptr;
    std::atomic<float>* inputMonitorParam = nullptr;
    std::atomic<float>* crossfadeLengthParam = nullptr;
    std::atomic<float>* triggerFadeInParam = nullptr;
    std::atomic<float>* outputRoutingParam = nullptr;
    std::atomic<float>* pitchControlModeParam = nullptr;
    std::atomic<float>* stretchBackendParam = nullptr;
    std::atomic<float>* flipTempoMatchModeParam = nullptr;
    std::atomic<float>* soundTouchEnabledParam = nullptr;
    std::atomic<float>* masterDuckTriggerStripParam = nullptr;
    std::array<std::atomic<float>*, MaxStrips> stripVolumeParams{};
    std::array<std::atomic<float>*, MaxStrips> stripPanParams{};
    std::array<std::atomic<float>*, MaxStrips> stripSpeedParams{};
    std::array<std::atomic<float>*, MaxStrips> stripPitchParams{};
    std::array<std::atomic<float>*, MaxStrips> stripSliceLengthParams{};
    std::array<std::atomic<float>*, MaxStrips> stripTempoMatchModeParams{};
    std::array<std::atomic<float>*, MaxStrips> stripDuckEnabledParams{};
    std::array<std::atomic<float>*, MaxStrips> stripDuckSourceParams{};
    std::array<std::atomic<float>*, MaxStrips> stripDuckThresholdParams{};
    std::array<std::atomic<float>*, MaxStrips> stripDuckRatioParams{};
    std::array<std::atomic<float>*, MaxStrips> stripDuckAttackParams{};
    std::array<std::atomic<float>*, MaxStrips> stripDuckReleaseParams{};
    std::array<std::atomic<float>*, MaxStrips> stripDuckGainCompParams{};
    std::array<std::atomic<float>*, MaxStrips> stripDuckFollowMasterParams{};
    juce::CriticalSection pendingLoopChangeLock;
    std::array<PendingLoopChange, MaxStrips> pendingLoopChanges{};
    juce::CriticalSection pendingBarChangeLock;
    std::array<PendingBarChange, MaxStrips> pendingBarChanges{};
    std::array<bool, MaxStrips> pendingBarLengthApply{};
    
    double currentSampleRate = 44100.0;
    ControlMode currentControlMode = ControlMode::Normal;
    bool controlModeActive = false;  // True when control button is held
    FilterSubPage filterSubPage = FilterSubPage::Frequency;  // Current filter sub-page
    std::atomic<int> lastMonomePressedStripRow{0};
    std::atomic<int> arcSelectedStripRow{0};
    mutable juce::CriticalSection controlPageOrderLock;
    ControlPageOrder controlPageOrder {
        ControlMode::Speed,
        ControlMode::Pitch,
        ControlMode::Pan,
        ControlMode::Volume,
        ControlMode::GrainSize,
        ControlMode::Swing,
        ControlMode::Gate,
        ControlMode::FileBrowser,
        ControlMode::GroupAssign,
        ControlMode::Filter,
        ControlMode::Modulation,
        ControlMode::Preset,
        ControlMode::StepEdit
    };
    StepEditTool stepEditTool = StepEditTool::Gate;
    int stepEditSelectedStrip = 0;
    int stepEditStripBank = 0;
    std::array<bool, MaxColumns> stepEditVelocityGestureActive{};
    std::array<int, MaxColumns> stepEditVelocityGestureStrip{};
    std::array<int, MaxColumns> stepEditVelocityGestureStep{};
    std::array<float, MaxColumns> stepEditVelocityGestureAnchorStart{};
    std::array<float, MaxColumns> stepEditVelocityGestureAnchorEnd{};
    std::array<float, MaxColumns> stepEditVelocityGestureAnchorValue{};
    std::array<uint32_t, MaxColumns> stepEditVelocityGestureLastActivityMs{};
    static constexpr uint32_t stepEditVelocityGestureLatchMs = 180;
    std::atomic<bool> controlPageMomentary{true};
    std::atomic<int> swingDivisionSelection{1}; // 0=1/4,1=1/8,2=1/16,3=1/8T,4=1/2,5=1/32,6=1/16T
    int lastAppliedStretchBackend = -1; // -1 = force initial sync on first process block
    int lastAppliedLoopTempoMatchBackend = -1;
    
    // LED state cache to prevent flickering
    int ledCache[MaxGridWidth][MaxGridHeight] = {{0}};
    
    // Last loaded folder for file browsing
    juce::File lastSampleFolder;
    std::array<juce::File, MaxStrips> defaultLoopDirectories;
    std::array<juce::File, MaxStrips> defaultStepDirectories;
    std::array<juce::File, MaxStrips> defaultFlipDirectories;
    std::array<juce::File, MaxStrips> recentLoopDirectories;
    std::array<juce::File, MaxStrips> recentStepDirectories;
    std::array<juce::File, MaxStrips> recentFlipDirectories;
    std::array<juce::File, BrowserFavoriteSlots> browserFavoriteDirectories;
    std::array<juce::File, BrowserFavoriteSlots> browserFlipFavoriteDirectories;
    std::atomic<int> persistentGlobalControlsDirty{0};
    std::atomic<int> suppressPersistentGlobalControlsSave{0};
    std::atomic<int> persistentGlobalControlsSaveQueued{0};
    juce::int64 lastPersistentGlobalControlsSaveMs = 0;
    bool persistentGlobalControlsApplied = false;
    std::atomic<int> pendingPersistentGlobalControlsRestore{0};
    juce::int64 pendingPersistentGlobalControlsRestoreMs = 0;
    int pendingPersistentGlobalControlsRestoreRemaining = 0;
    std::atomic<int> persistentGlobalControlsReady{0};
    std::atomic<int> persistentGlobalUserTouched{0};
    std::array<std::array<bool, BrowserFavoriteSlots>, MaxStrips> browserFavoritePadHeld{};
    std::array<std::array<bool, BrowserFavoriteSlots>, MaxStrips> browserFavoritePadHoldSaveTriggered{};
    std::array<std::array<uint32_t, BrowserFavoriteSlots>, MaxStrips> browserFavoritePadPressStartMs{};
    std::array<uint32_t, BrowserFavoriteSlots> browserFavoriteSaveBurstUntilMs{};
    std::array<uint32_t, BrowserFavoriteSlots> browserFavoriteMissingBurstUntilMs{};
    std::array<std::atomic<int>, MacroCount> macroMidiCcAssignments{};
    std::array<std::atomic<int>, MacroCount> macroTargetAssignments{};
    std::atomic<int> macroMidiLearnIndex{-1};
    std::array<MacroLaneRecordState, MacroCount> macroLaneRecordStates{};
    std::array<int, 4> arcKeyHeld{};
    std::array<std::array<int, 64>, 4> arcRingCache{};
    ArcControlMode arcControlMode = ArcControlMode::SelectedStrip;
    int arcSelectedModStep = 0;
    juce::int64 lastGridLedUpdateTimeMs = 0;
    static constexpr int kGridRefreshMs = 33;
    static constexpr int kArcRefreshMs = 33;
    static constexpr uint32_t browserFavoriteHoldSaveMs = 3000;
    static constexpr uint32_t browserFavoriteSaveBurstDurationMs = 320;
    static constexpr uint32_t browserFavoriteMissingBurstDurationMs = 260;
    
    // Current file per strip for proper next/prev browsing
    std::array<juce::File, MaxStrips> currentStripFiles;
    std::array<juce::File, MaxStrips> pendingLoopStripFiles;
    std::array<std::unique_ptr<SampleModeEngine>, MaxStrips> sampleModeEngines;
    std::array<juce::AudioBuffer<float>, MaxStrips> sampleModeScratchBuffers;
    std::array<std::atomic<int>, MaxStrips> sampleModeHeldVisibleSliceSlots{};
    std::array<std::atomic<int>, MaxStrips> loopStripLoadRequestIds{};
    std::array<std::atomic<int>, MaxStrips> loopStripLoadInFlight{};
    std::array<std::atomic<int>, MaxStrips> loopStripLoadProgressPermille{};
    std::array<std::atomic<int>, MaxStrips> loopPitchAnalysisRequestIds{};
    std::array<std::atomic<int>, MaxStrips> loopPitchAnalysisInFlight{};
    std::array<std::atomic<int>, MaxStrips> loopPitchAnalysisProgressPermille{};
    std::array<std::atomic<int>, MaxStrips> loopPitchDetectedMidi{};
    std::array<std::atomic<float>, MaxStrips> loopPitchDetectedHz{};
    std::array<std::atomic<float>, MaxStrips> loopPitchDetectedPitchConfidence{};
    std::array<std::atomic<int>, MaxStrips> loopPitchDetectedScaleIndices{};
    std::array<std::atomic<float>, MaxStrips> loopPitchDetectedScaleConfidence{};
    std::array<std::atomic<int>, MaxStrips> loopPitchEssentiaUsed{};
    std::array<std::atomic<int>, MaxStrips> loopPitchRoles{};
    std::array<std::atomic<int>, MaxStrips> loopPitchSyncTimings{};
    std::array<std::atomic<int>, MaxStrips> loopPitchAssignedMidi{};
    std::array<std::atomic<int>, MaxStrips> loopPitchAssignedManual{};
    std::array<std::atomic<int>, MaxStrips> loopPitchPendingRetune{};
    std::atomic<int> globalRootNoteMidi{60};
    std::atomic<int> globalPitchScale{static_cast<int>(ModernAudioEngine::PitchScale::Chromatic)};
    juce::ThreadPool loopStripLoadThreadPool{1};
    juce::ThreadPool loopPitchAnalysisThreadPool{1};
    mutable juce::CriticalSection loopStripLoadStatusLock;
    std::array<juce::String, MaxStrips> loopStripLoadStatusTexts;
    mutable juce::CriticalSection loopPitchAnalysisStatusLock;
    std::array<juce::String, MaxStrips> loopPitchAnalysisStatusTexts;
    juce::CriticalSection loopStripLoadResultLock;
    std::vector<LoopStripLoadResult> loopStripLoadResults;
    juce::CriticalSection loopPitchAnalysisResultLock;
    std::vector<LoopPitchAnalysisResult> loopPitchAnalysisResults;
    std::array<int, MaxStrips> loopPitchLastObservedColumns{};
    int loopPitchLastObservedHostBar = -1;
    struct FlipLegacyLoopSyncCache
    {
        const void* loadedSampleToken = nullptr;
        int visibleBankIndex = -1;
        int64_t bankStartSample = -1;
        int64_t bankEndSample = -1;
        uint64_t sliceSignature = 0;
        uint64_t warpSignature = 0;
        float beatsPerLoop = 4.0f;
        int legacyLoopBarSelection = 0;
        TimeStretchBackend backend = TimeStretchBackend::Resample;
        double hostTempo = 0.0;
        juce::AudioBuffer<float> cachedBankBuffer;
        std::array<int, SliceModel::VisibleSliceCount> cachedTransientSliceStarts{};
        std::array<float, 128> cachedRmsMap{};
        std::array<int, 128> cachedZeroCrossMap{};
        int cachedSourceLengthSamples = 0;
        double cachedSampleRate = 0.0;
        bool renderValid = false;
        bool renderInFlight = false;
        bool valid = false;
        bool stripApplied = false;
        uint64_t renderGeneration = 0;
    };
    struct FlipLegacyLoopRenderRequest
    {
        int cacheIndex = -1;
        uint64_t renderGeneration = 0;
        SampleModeEngine::LegacyLoopSyncInfo syncInfo;
        double hostTempo = 0.0;
        TimeStretchBackend backend = TimeStretchBackend::Resample;
        float visibleBankBeats = 0.0f;
        int64_t bankStartSample = 0;
        int64_t bankEndSample = 0;
    };
    struct FlipLegacyLoopRenderResult
    {
        int cacheIndex = -1;
        uint64_t renderGeneration = 0;
        FlipLegacyLoopSyncCache cacheEntry;
    };
    struct FlipLegacyLoopSyncInfoCacheEntry
    {
        uint64_t version = 0;
        std::shared_ptr<const SampleModeEngine::LegacyLoopSyncInfo> syncInfo;
    };
    struct PendingFlipLegacyLoopTrigger
    {
        bool valid = false;
        SampleModeEngine::LegacyLoopSyncInfo syncInfo;
        bool isMomentaryStutter = false;
    };
    class FlipLegacyLoopRenderJob;
    std::array<FlipLegacyLoopSyncCache, MaxStrips> flipLegacyLoopSyncCache{};
    std::array<FlipLegacyLoopSyncInfoCacheEntry, MaxStrips> flipLegacyLoopSyncInfoCache{};
    std::array<PendingFlipLegacyLoopTrigger, MaxStrips> pendingFlipLegacyLoopTriggers{};
    juce::ThreadPool flipLegacyLoopRenderThreadPool{1};
    juce::SpinLock flipLegacyLoopSyncCacheLock;
    juce::SpinLock flipLegacyLoopSyncInfoCacheLock;
    juce::SpinLock pendingFlipLegacyLoopTriggerLock;
    juce::CriticalSection flipLegacyLoopRenderResultLock;
    std::vector<FlipLegacyLoopRenderResult> flipLegacyLoopRenderResults;
    
    // LED update
    void updateMonomeLEDs();
    void updateMonomeArcRings();
    void renderSampleModeStrip(int stripIndex,
                               juce::AudioBuffer<float>& output,
                               int startSample,
                               int numSamples,
                               const juce::AudioPlayHead::PositionInfo& positionInfo,
                               int64_t globalSampleStart,
                               double tempo,
                               double quantizeBeats);
    void triggerSampleModeStripAtSample(int stripIndex,
                                        int column,
                                        int sampleSliceId,
                                        int64_t sampleStartSample,
                                        int64_t triggerSample,
                                        const juce::AudioPlayHead::PositionInfo& positionInfo,
                                        bool isMomentaryStutter);
    bool syncFlipLegacyLoopStripState(int stripIndex,
                                      EnhancedAudioStrip& strip,
                                      const SampleModeEngine::LegacyLoopSyncInfo& syncInfo,
                                      double hostTempo,
                                      double hostPpq,
                                      int64_t currentGlobalSample,
                                      bool preservePlaybackState,
                                      TimeStretchBackend backend,
                                      bool allowInlineBuild = true);
    bool flipLegacyLoopCacheMatchesRenderKey(const FlipLegacyLoopSyncCache& entry,
                                             const SampleModeEngine::LegacyLoopSyncInfo& syncInfo,
                                             double hostTempo,
                                             TimeStretchBackend backend,
                                             float visibleBankBeats) const;
    bool flipLegacyLoopCacheMatchesReusableAudioKey(const FlipLegacyLoopSyncCache& entry,
                                                    const SampleModeEngine::LegacyLoopSyncInfo& syncInfo,
                                                    double hostTempo,
                                                    TimeStretchBackend backend,
                                                    float visibleBankBeats) const;
    void assignFlipLegacyLoopRenderKey(FlipLegacyLoopSyncCache& cache,
                                       const SampleModeEngine::LegacyLoopSyncInfo& syncInfo,
                                       double hostTempo,
                                       TimeStretchBackend backend,
                                       float visibleBankBeats) const;
    void applyFlipLegacyLoopRenderCacheToStrip(EnhancedAudioStrip& strip,
                                               const FlipLegacyLoopSyncCache& entry,
                                               TimeStretchBackend backend,
                                               float visibleBankBeats) const;
    void applyFlipLegacyLoopTransientSliceCacheToStrip(EnhancedAudioStrip& strip,
                                                       const FlipLegacyLoopSyncCache& entry,
                                                       TimeStretchBackend backend,
                                                       float visibleBankBeats) const;
    bool queueFlipLegacyLoopRender(int preferredCacheIndex,
                                   const SampleModeEngine::LegacyLoopSyncInfo& syncInfo,
                                   double hostTempo,
                                   TimeStretchBackend backend);
    void pushFlipLegacyLoopRenderResult(FlipLegacyLoopRenderResult result);
    void applyCompletedFlipLegacyLoopRenders();
    void prewarmFlipLegacyLoopRenders();
    std::shared_ptr<const SampleModeEngine::LegacyLoopSyncInfo> getCachedFlipLegacyLoopSyncInfo(int stripIndex,
                                                                                                 SampleModeEngine& engine);
    void queueLoopPitchAnalysisResult(LoopPitchAnalysisResult result);
    void applyCompletedLoopPitchAnalyses();
    void queueLoopStripLoadResult(LoopStripLoadResult result);
    void applyCompletedLoopStripLoads();
    bool beginLoopStripPitchAnalysis(int stripIndex, bool setDetectedAsRoot);
    void updateLoopPitchAnalysisProgress(int stripIndex, int requestId, float progress, const juce::String& statusText);
    void resetLoopPitchAnalysisProgress(int stripIndex);
    void updateLoopStripLoadProgress(int stripIndex, int requestId, float progress, const juce::String& statusText);
    void resetLoopStripLoadProgress(int stripIndex);
    void applyLoopStripPitchSemitones(int stripIndex, float semitones);
    void normalizeLoopPitchMasterRoles();
    int getEffectiveLoopPitchMasterRootMidi(int stripIndex) const;
    void applyLoopPitchRoleStateToStrip(int stripIndex);
    void applyLoopPitchSyncToAllStrips();
    int getPitchQuantizeReferenceRootMidiForStrip(int stripIndex) const;
    float getLoopPitchTempoMatchOffsetSemitones(int stripIndex) const;
    float getEffectiveLoopPitchSourceMidi(int stripIndex) const;
    void requestLoopPitchRoleStateUpdate(int stripIndex);
    void applyPendingLoopPitchRetunes();
    bool applyPendingLoopPitchRetuneOnTrigger(int stripIndex);
    void appendLoopPitchStateToState(juce::ValueTree& state) const;
    void loadLoopPitchStateFromState(const juce::ValueTree& state);
    std::unique_ptr<juce::XmlElement> createLoopPitchPresetStateXml(int stripIndex) const;
    void applyLoopPitchPresetStateXml(int stripIndex, const juce::XmlElement* stateXml);
    void handleSampleModeLegacyLoopRenderStateChanged(int stripIndex, bool preferInlineBuild = false);
    void handleFlipTempoMatchModeChanged();
    void invalidateFlipLegacyLoopSync(int stripIndex);
    void stopSampleModeStrip(int stripIndex, bool immediateStop);
    void timerCallback() override;
    
    void handleMonomeKeyPress(int x, int y, int state);
    void resetStepEditVelocityGestures();
    bool isArcModulationMode() const;
    void setArcControlMode(ArcControlMode mode);
    void handleMonomeArcDelta(int encoder, int delta);
    void handleMonomeArcKey(int encoder, int state);
    void setMomentaryScratchHold(bool shouldEnable);
    void setMomentaryStutterHold(bool shouldEnable);
    
    juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    void cacheParameterPointers();
    SamplePathMode getSamplePathModeForStrip(int stripIndex) const;
    juce::File getRecentSampleDirectory(int stripIndex, SamplePathMode mode) const;
    void setRecentSampleDirectory(int stripIndex, SamplePathMode mode, const juce::File& directory, bool persist = true);
    void rememberLoadedSamplePathForStrip(int stripIndex, const juce::File& file, bool persist = true);
    void rememberLoadedSamplePathForStripMode(int stripIndex, const juce::File& file, SamplePathMode mode, bool persist = true);
    bool saveBrowserFavoriteDirectoryFromStrip(int stripIndex, int slot);
    bool recallBrowserFavoriteDirectoryForStrip(int stripIndex, int slot);
    bool isAudioFileSupported(const juce::File& file) const;
    juce::String createEmbeddedFlipSampleData(int stripIndex) const;
    bool loadEmbeddedFlipSampleData(int stripIndex,
                                    const juce::String& base64Data,
                                    const SampleModePersistentState* persistentState = nullptr);
    void loadFlipStatesFromState(const juce::ValueTree& state);
    void appendFlipStatesToState(juce::ValueTree& state) const;
    std::unique_ptr<juce::XmlElement> createFlipPresetStateXml(int stripIndex) const;
    void applyFlipPresetStateXml(int stripIndex, const juce::XmlElement* flipStateXml);
    void loadDefaultPathsFromState(const juce::ValueTree& state);
    void appendDefaultPathsToState(juce::ValueTree& state) const;
    void loadControlPagesFromState(const juce::ValueTree& state);
    void appendControlPagesToState(juce::ValueTree& state) const;
    void stripPersistentGlobalControlsFromState(juce::ValueTree& state) const;
    void loadPersistentDefaultPaths();
    void savePersistentDefaultPaths() const;
    void resetCurrentBrowserDirectoriesToDefaultPaths(bool persist);
    void loadPersistentControlPages();
    void savePersistentControlPages() const;
    void loadPersistentGlobalControls();
    int getQuantizeDivision() const;
    float getInnerLoopLengthFactor() const;
    void queueLoopChange(int stripIndex, bool clearLoop, int startColumn, int endColumn, bool reverseDirection, int markerColumn = -1);
    void recoverDeferredPpqAnchors(const juce::AudioPlayHead::PositionInfo& posInfo);
    void applyPendingLoopChanges(const juce::AudioPlayHead::PositionInfo& posInfo);
    void applyPendingBarChanges(const juce::AudioPlayHead::PositionInfo& posInfo);
    void applyPendingStutterStart(const juce::AudioPlayHead::PositionInfo& posInfo);
    void applyPendingStutterRelease(const juce::AudioPlayHead::PositionInfo& posInfo);
    void performMomentaryStutterStartNow(double hostPpqNow, int64_t nowSample);
    void performMomentaryStutterReleaseNow(double hostPpqNow, int64_t nowSample);
    void captureMomentaryStutterMacroBaseline();
    void applyMomentaryStutterMacro(const juce::AudioPlayHead::PositionInfo& posInfo);
    void restoreMomentaryStutterMacroBaseline();
    void updateMacroLaneRecording(const juce::AudioPlayHead::PositionInfo& posInfo, int numSamples);
    bool getHostSyncSnapshot(double& outPpq, double& outTempo) const;
    int getActiveMainPresetIndexForScenes() const;
    int getSceneStoragePresetIndex(int mainPresetIndex, int sceneSlot) const;
    bool saveSceneForMainPreset(int mainPresetIndex, int sceneSlot);
    bool copySceneForMainPreset(int mainPresetIndex, int sourceSceneSlot, int destSceneSlot);
    bool sceneSlotExistsForMainPreset(int mainPresetIndex, int sceneSlot) const;
    void requestSceneRecallQuantized(int mainPresetIndex, int sceneSlot, bool sequenceDriven);
    double getSceneRecallIntervalBeats() const;
    void updateSceneQuantizedRecall(const juce::AudioPlayHead::PositionInfo& posInfo, int numSamples);
    void processPendingSceneApply();
    void performEmptySceneLoad();
    void performSceneLoad(int mainPresetIndex,
                          int sceneSlot,
                          double hostPpqSnapshot,
                          double hostTempoSnapshot,
                          int64_t hostGlobalSampleSnapshot);
    double computeCurrentSceneSequenceLengthBeats() const;
    double computeStripSceneSequenceLengthBeats(int stripIndex) const;
    double computeLongestStripSceneSequenceLengthBeats() const;
    double computeLongestPatternSceneSequenceLengthBeats() const;
    void armNextSceneInSequence(int mainPresetIndex, int currentSceneSlot, double sceneStartPpq);
    std::unique_ptr<juce::XmlElement> createSceneChainStateXml(int sceneSlotOverride) const;
    void applySceneChainStateXml(const juce::XmlElement* xml, int sceneSlotOverride);
    void syncSceneModeFromParameters();
    void applySceneModeState(bool enabled);
    void captureSceneModeGroupSnapshot();
    void restoreSceneModeGroupSnapshot();
    void clearAllStripGroupsForSceneMode();
    void appendSceneModeStateToState(juce::ValueTree& state) const;
    void loadSceneModeStateFromState(const juce::ValueTree& state);
    void handleIncomingMacroCc(const juce::MidiBuffer& midiMessages);
    int getMacroTargetStripIndex() const;
    float getMacroNormalizedValueForTarget(const EnhancedAudioStrip& strip, MacroTarget target) const;
    void applyMacroTargetValue(EnhancedAudioStrip& strip, MacroTarget target, float normalizedValue);
    void resetMacroLaneRecordState(int macroIndex);
    void finishMacroLaneRecording(int macroIndex, bool activateLane);
    bool initializeMacroLaneRecording(int macroIndex, const juce::AudioPlayHead::PositionInfo& posInfo);
    float getMacroLaneRecordedValue(int macroIndex) const;
    void performPresetLoad(int presetIndex, double hostPpqSnapshot, double hostTempoSnapshot);
    struct PresetSaveRequest
    {
        int presetIndex = -1;
        std::array<juce::File, MaxStrips> stripFiles;
        std::array<juce::File, MaxStrips> recentLoopDirectories;
        std::array<juce::File, MaxStrips> recentStepDirectories;
        std::array<juce::File, MaxStrips> recentFlipDirectories;
    };
    struct PresetSaveResult
    {
        int presetIndex = -1;
        bool success = false;
    };
    class PresetSaveJob;
    class LoopStripLoadJob;
    class LoopPitchAnalysisJob;
    bool runPresetSaveRequest(const PresetSaveRequest& request);
    void pushPresetSaveResult(const PresetSaveResult& result);
    void applyCompletedPresetSaves();
    void resetRuntimePresetStateToDefaults();

    // Row 0, col 8: global momentary scratch modifier.
    bool momentaryScratchHoldActive = false;
    std::array<float, MaxStrips> momentaryScratchSavedAmount{};
    std::array<EnhancedAudioStrip::DirectionMode, MaxStrips> momentaryScratchSavedDirection{};
    std::array<bool, MaxStrips> momentaryScratchWasStepMode{};

    // Row 0, cols 9..15: PPQ stutter-hold with fixed divisions.
    bool momentaryStutterHoldActive = false;
    double momentaryStutterDivisionBeats = 1.0; // one-button map spans 2.0 (1/2) ... 0.03125 (1/128)
    int momentaryStutterActiveDivisionButton = -1;
    std::atomic<uint8_t> momentaryStutterButtonMask{0};
    std::array<bool, MaxStrips> momentaryStutterStripArmed{};
    struct MomentaryStutterSavedStripState
    {
        bool valid = false;
        bool stepMode = false;
        float pan = 0.0f;
        float playbackSpeed = 1.0f;
        float pitchSemitones = 0.0f;
        float pitchShift = 0.0f;
        float loopSliceLength = 1.0f;
        bool filterEnabled = false;
        float filterFrequency = 20000.0f;
        float filterResonance = 0.707f;
        float filterMorph = 0.0f;
        EnhancedAudioStrip::FilterAlgorithm filterAlgorithm = EnhancedAudioStrip::FilterAlgorithm::Tpt12;
        bool stepFilterEnabled = false;
        float stepFilterFrequency = 1000.0f;
        float stepFilterResonance = 0.7f;
        FilterType stepFilterType = FilterType::LowPass;
    };
    std::array<MomentaryStutterSavedStripState, MaxStrips> momentaryStutterSavedState{};
    bool momentaryStutterMacroBaselineCaptured = false;
    bool momentaryStutterMacroCapturePending = false;
    double momentaryStutterMacroStartPpq = 0.0;
    uint8_t momentaryStutterLastComboMask = 0;
    bool momentaryStutterTwoButtonStepBaseValid = false;
    int momentaryStutterTwoButtonStepBase = 0;
    std::atomic<int> momentaryStutterPlaybackActive{0};
    std::atomic<int> pendingStutterStartActive{0};
    std::atomic<double> pendingStutterStartPpq{-1.0};
    std::atomic<double> pendingStutterStartDivisionBeats{1.0};
    std::atomic<int> pendingStutterStartQuantizeDivision{8};
    std::atomic<int64_t> pendingStutterStartSampleTarget{-1};
    std::atomic<int> pendingStutterReleaseActive{0};
    std::atomic<double> pendingStutterReleasePpq{-1.0};
    std::atomic<int> pendingStutterReleaseQuantizeDivision{8};
    std::atomic<int64_t> pendingStutterReleaseSampleTarget{-1};

    // Preset page hold/double-tap state (used when control mode == Preset).
    int loadedPresetIndex = -1;
    std::array<bool, MaxPresetSlots> presetPadHeld{};
    std::array<bool, MaxPresetSlots> presetPadHoldSaveTriggered{};
    std::array<bool, MaxPresetSlots> presetPadDeleteTriggered{};
    std::array<uint32_t, MaxPresetSlots> presetPadPressStartMs{};
    std::array<uint32_t, MaxPresetSlots> presetPadSaveBurstUntilMs{};
    std::atomic<uint32_t> presetRefreshToken{0};
    std::array<uint32_t, MaxPresetSlots> presetPadLastTapMs{};
    std::atomic<int> sceneModeEnabled{0};
    std::atomic<float>* sceneModeParam = nullptr;
    int activeSceneMainPresetIndex = 0;
    int activeSceneSlot = 0;
    struct SceneModeGroupSnapshot
    {
        bool valid = false;
        std::array<int, MaxStrips> stripGroups{};
        std::array<float, ModernAudioEngine::MaxGroups> groupVolumes{};
        std::array<bool, ModernAudioEngine::MaxGroups> groupMuted{};
    };
    SceneModeGroupSnapshot sceneModeGroupSnapshot;
    struct PendingSceneRecall
    {
        bool active = false;
        bool sequenceDriven = false;
        bool targetResolved = false;
        int mainPresetIndex = 0;
        int sceneSlot = 0;
        double targetPpq = 0.0;
        double intervalBeats = 4.0;
    };
    PendingSceneRecall pendingSceneRecall;
    std::array<int, SceneSlots> sceneRepeatCounts{};
    std::array<int, SceneSlots> sceneLengthModes{};
    std::array<int, SceneSlots> sceneManualBars{};
    std::array<int, SceneSlots> sceneAnchorStrips{};
    std::array<bool, SceneSlots> scenePadHeld{};
    std::array<bool, SceneSlots> scenePadHoldSaveTriggered{};
    std::array<uint32_t, SceneSlots> scenePadPressStartMs{};
    std::array<uint32_t, SceneSlots> scenePadSaveBurstUntilMs{};
    std::vector<int> sceneSequenceSlots;
    bool sceneSequenceActive = false;
    bool activeSceneStartPpqValid = false;
    double activeSceneStartPpq = 0.0;
    bool sceneSequenceStartPpqValid = false;
    double sceneSequenceStartPpq = 0.0;
    std::atomic<int> pendingSceneApplyMainPreset{-1};
    std::atomic<int> pendingSceneApplySlot{-1};
    std::atomic<int> pendingSceneApplySequenceDriven{0};
    std::atomic<double> pendingSceneApplyTargetPpq{-1.0};
    std::atomic<double> pendingSceneApplyTargetTempo{120.0};
    std::atomic<int64_t> pendingSceneApplyTargetSample{-1};
    static constexpr uint32_t presetHoldSaveMs = 3000;
    static constexpr uint32_t presetDoubleTapMs = 350;
    static constexpr uint32_t presetSaveBurstDurationMs = 260;
    static constexpr uint32_t presetSaveBurstIntervalMs = 55;
    std::atomic<int> pendingPresetLoadIndex{-1};
    juce::ThreadPool presetSaveThreadPool{1};
    juce::CriticalSection presetSaveResultLock;
    std::vector<PresetSaveResult> presetSaveResults;
    std::atomic<int> presetSaveJobsInFlight{0};
    
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MlrVSTAudioProcessor)
    JUCE_DECLARE_WEAK_REFERENCEABLE(MlrVSTAudioProcessor)
};
