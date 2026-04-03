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
#include <functional>
#include <limits>
#include "PerformanceTargets.h"
#include "AudioEngine.h"
#include "SampleMode.h"
#include "ScenePerformanceRecorder.h"
#include "StripControlState.h"

class MacroTargetDispatcher;
class SceneScheduler;

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

    using juce::AudioProcessor::processBlock;
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
    void queueActiveSceneAutosave();
    void queueActiveSceneAutosave(uint32_t additionalDelayMs);

    //==============================================================================
    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    //==============================================================================
    // Public API
    ModernAudioEngine* getAudioEngine() { return audioEngine.get(); }
    const ModernAudioEngine* getAudioEngine() const { return audioEngine.get(); }
    MonomeConnection& getMonomeConnection() { return monomeConnection; }
    
    bool loadSampleToStrip(int stripIndex, const juce::File& file);
    bool loadSampleToStripPreservingPlaybackState(int stripIndex, const juce::File& file);
    bool loadSampleToSampleModeStrip(int stripIndex, const juce::File& file);
    SampleModeEngine* getSampleModeEngine(int stripIndex, bool createIfMissing = true);
    bool hasSampleModeAudio(int stripIndex) const;
    bool isStripScenePlaybackAvailable(int stripIndex) const;
    void loadAdjacentFile(int stripIndex, int direction);  // Browse files
    void captureRecentAudioToStrip(int stripIndex);
    void clearRecentAudioBuffer();
    void requestBarLengthChange(int stripIndex, int bars);
    bool canChangeBarLengthNow(int stripIndex) const;
    void setPendingBarLengthApply(int stripIndex, bool pending);
    void triggerStrip(int stripIndex, int column);
    void stopStrip(int stripIndex, bool immediateStop = false, int sceneReleaseColumnHint = -1);
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
        SoundTouch,
        Resample,
        Signalsmith,
        Bungee
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
    enum class StripPitchControlMode
    {
        Global = 0,
        PitchShift,
        SoundTouch,
        Resample,
        Signalsmith,
        Bungee
    };
    PitchControlMode getPitchControlMode() const;
    StripPitchControlMode getStripPitchControlMode(int stripIndex) const;
    PitchControlMode resolvePitchControlModeForStrip(int stripIndex) const;
    TimeStretchBackend getStretchBackend() const;
    bool usesContinuousTraversal() const;
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
    enum class GatePageMode
    {
        Adaptive = 0,
        Quarter,
        Sixth,
        Eighth,
        Sixteenth
    };
    LoopPitchRole getLoopPitchRole(int stripIndex) const;
    int getLoopPitchMasterStripIndex() const;
    bool isLoopPitchMasterActive() const { return getLoopPitchMasterStripIndex() >= 0; }
    void setLoopPitchRole(int stripIndex, LoopPitchRole role);
    LoopPitchSyncTiming getLoopPitchSyncTiming(int stripIndex) const;
    void setLoopPitchSyncTiming(int stripIndex, LoopPitchSyncTiming timing);
    GatePageMode getGatePageMode() const
    {
        return static_cast<GatePageMode>(juce::jlimit(
            0,
            static_cast<int>(GatePageMode::Sixteenth),
            gatePageMode.load(std::memory_order_acquire)));
    }
    void setGatePageMode(GatePageMode mode);
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
    struct SoundTouchPitchCacheResult
    {
        int stripIndex = -1;
        int requestId = 0;
        bool success = false;
        float semitones = 0.0f;
        double sourceSampleRate = 44100.0;
        uint64_t sourceVersion = 0;
        juce::AudioBuffer<float> renderedBuffer;
    };
    struct BungeePitchCacheResult
    {
        int stripIndex = -1;
        int requestId = 0;
        bool success = false;
        float semitones = 0.0f;
        double sourceSampleRate = 44100.0;
        uint64_t sourceVersion = 0;
        juce::AudioBuffer<float> renderedBuffer;
    };
    struct SignalsmithPitchCacheResult
    {
        int stripIndex = -1;
        int requestId = 0;
        bool success = false;
        float semitones = 0.0f;
        double sourceSampleRate = 44100.0;
        uint64_t sourceVersion = 0;
        juce::AudioBuffer<float> renderedBuffer;
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
        Delay,
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
    static constexpr int NumControlRowPages = 14;
    using ControlPageOrder = std::array<ControlMode, NumControlRowPages>;
    ControlPageOrder getControlPageOrder() const;
    ControlMode getControlModeForControlButton(int buttonIndex) const;
    int getControlButtonForMode(ControlMode mode) const;
    void moveControlPage(int fromIndex, int toIndex);
    bool isControlPageMomentary() const { return controlPageMomentary.load(std::memory_order_acquire); }
    void setControlPageMomentary(bool shouldBeMomentary);
    void setControlModeFromGui(ControlMode mode, bool shouldBeActive);
    void setSwingDivisionSelection(int mode);
    void setInnerLoopLengthSelection(int choiceIndex);
    void syncTransientDetectionSettingsFromParameters(bool refreshSlicesNow);
    enum class StripControlWriteMode
    {
        NotifyHost,
        CacheOnly
    };
    void setStripVolumeControlValue(int stripIndex,
                                    float volume,
                                    StripControlWriteMode writeMode = StripControlWriteMode::NotifyHost);
    void setStripPanControlValue(int stripIndex,
                                 float pan,
                                 StripControlWriteMode writeMode = StripControlWriteMode::NotifyHost);
    void setStripSpeedControlValue(int stripIndex,
                                   float speed,
                                   StripControlWriteMode writeMode = StripControlWriteMode::NotifyHost);
    void setStripFilterEnabledControlValue(int stripIndex,
                                           bool enabled,
                                           StripControlWriteMode writeMode = StripControlWriteMode::NotifyHost);
    void setStripFilterFrequencyControlValue(int stripIndex,
                                             float frequency,
                                             StripControlWriteMode writeMode = StripControlWriteMode::NotifyHost);
    void setStripFilterResonanceControlValue(int stripIndex,
                                             float resonance,
                                             StripControlWriteMode writeMode = StripControlWriteMode::NotifyHost);
    void setStripFilterMorphControlValue(int stripIndex,
                                         float morph,
                                         StripControlWriteMode writeMode = StripControlWriteMode::NotifyHost);
    void setStripFilterAlgorithmControlValue(int stripIndex,
                                             EnhancedAudioStrip::FilterAlgorithm algorithm,
                                             StripControlWriteMode writeMode = StripControlWriteMode::NotifyHost);
    void setStripDelayMixControlValue(int stripIndex,
                                      float mix,
                                      StripControlWriteMode writeMode = StripControlWriteMode::NotifyHost);
    void setStripDelayTimeControlValue(int stripIndex,
                                       float timeValue,
                                       StripControlWriteMode writeMode = StripControlWriteMode::NotifyHost);
    void setStripDelaySyncEnabledControlValue(int stripIndex,
                                              bool enabled,
                                              StripControlWriteMode writeMode = StripControlWriteMode::NotifyHost);
    void setStripDelayFeedbackControlValue(int stripIndex,
                                           float feedback,
                                           StripControlWriteMode writeMode = StripControlWriteMode::NotifyHost);
    void setStripDelayLowCutControlValue(int stripIndex,
                                         float hz,
                                         StripControlWriteMode writeMode = StripControlWriteMode::NotifyHost);
    void setStripDelayHighCutControlValue(int stripIndex,
                                          float hz,
                                          StripControlWriteMode writeMode = StripControlWriteMode::NotifyHost);
    void setStripDelayModeControlValue(int stripIndex,
                                       EnhancedAudioStrip::DelayMode mode,
                                       StripControlWriteMode writeMode = StripControlWriteMode::NotifyHost);
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
    bool isMonomeTopRowEditSupported() const;
    bool isMonomeTopRowEditActive() const;
    juce::String getMonomeTopRowModeName() const;
    juce::String getMonomeTopRowHintText() const;
    
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
    static constexpr int SceneSlots = 8;
    static constexpr int MaxSceneChainSteps = 8;
    static constexpr int SceneTransitionFavoriteSlots = 4;
    static constexpr int MaxSceneRepeatCount = 32;
    static constexpr int MaxSceneManualBars = 32;
    static constexpr float MinSceneTransitionLengthBeats = 0.25f;
    static constexpr float MaxSceneTransitionLengthBeats = 8.0f;
    static constexpr float DefaultSceneTransitionLengthBeats = 1.0f;
    static constexpr float DefaultSceneTransitionIntensity = 1.0f;
    static constexpr float DefaultSceneTransitionDelayAmount = 0.48f;
    static constexpr float DefaultSceneTransitionFilterAmount = 0.42f;
    static constexpr float DefaultSceneTransitionChopAmount = 0.18f;
    static constexpr float DefaultSceneTransitionEndSampleGainDb = 0.0f;
    static constexpr float DefaultSceneTransitionEndSampleFadeInMs = 0.0f;
    static constexpr float DefaultSceneTransitionEndSampleFadeOutMs = 0.0f;
    static constexpr float DefaultSceneTransitionEndSamplePitchSemitones = 0.0f;
    static constexpr float DefaultSceneTransitionEndSampleLowpassHz = 20000.0f;
    static constexpr float DefaultSceneTransitionEndSampleHighpassHz = 20.0f;
    static constexpr int DefaultSceneTransitionEndSampleDuckSource = 0;
    static constexpr float DefaultSceneTransitionEndSampleDuckAmount = 0.0f;

    enum class SceneLengthMode
    {
        LongestStrip = 0,
        LongestPattern,
        ManualBars,
        AnchorStrip
    };

    enum class SceneRecallMode
    {
        QuantizeGrid = 0,
        PatternEnd,
        SceneEnd,
        Manual
    };

    enum class SceneModPageMode
    {
        StepMotion = 0,
        MainModulation
    };

    enum class SceneChainTransitionType
    {
        None = 0,
        Fill,
        Stutter,
        FilterRise,
        Drop,
        MuteTail,
        Break,
        Return
    };

    enum class SceneChainTransitionOption
    {
        Tight = 0,
        Default,
        Wide,
        Wash,
        Snap,
        Echo,
        Sweep,
        Gate
    };

    enum class SceneChainTransitionScope
    {
        All = 0,
        Loops,
        Steps,
        Grains,
        Flip
    };

    enum class SceneChainTransitionContour
    {
        Smooth = 0,
        Ramp,
        BurstEnd,
        DuckThenLift,
        LateHit
    };

    enum class SceneChainTransitionCondition
    {
        Always = 0,
        Chance50,
        Chance25,
        LoopOnly,
        ForwardOnly
    };

    struct SceneTransitionEndSampleSettings
    {
        float gainDb = DefaultSceneTransitionEndSampleGainDb;
        float fadeInMs = DefaultSceneTransitionEndSampleFadeInMs;
        float fadeOutMs = DefaultSceneTransitionEndSampleFadeOutMs;
        bool chokePrevious = false;
        bool reverse = false;
        float pitchSemitones = DefaultSceneTransitionEndSamplePitchSemitones;
        float lowpassHz = DefaultSceneTransitionEndSampleLowpassHz;
        float highpassHz = DefaultSceneTransitionEndSampleHighpassHz;
        int duckSource = DefaultSceneTransitionEndSampleDuckSource;
        float duckAmount = DefaultSceneTransitionEndSampleDuckAmount;
    };

    struct SceneChainStep
    {
        int sceneSlot = -1;
        int repeats = 1;
        SceneChainTransitionType transitionToNext = SceneChainTransitionType::None;
        SceneChainTransitionOption transitionOption = SceneChainTransitionOption::Default;
        SceneChainTransitionScope transitionScope = SceneChainTransitionScope::All;
        SceneChainTransitionContour transitionContour = SceneChainTransitionContour::Smooth;
        SceneChainTransitionCondition transitionCondition = SceneChainTransitionCondition::Always;
        float transitionLengthBeats = DefaultSceneTransitionLengthBeats;
        bool transitionSubtractsFromSceneLength = false;
        float transitionIntensity = DefaultSceneTransitionIntensity;
        float transitionDelayAmount = DefaultSceneTransitionDelayAmount;
        float transitionFilterAmount = DefaultSceneTransitionFilterAmount;
        float transitionChopAmount = DefaultSceneTransitionChopAmount;
        juce::File transitionEndSampleFile;
        SceneTransitionEndSampleSettings transitionEndSampleSettings;
    };

    struct SceneChainTransitionFavorite
    {
        bool valid = false;
        SceneChainTransitionType type = SceneChainTransitionType::None;
        SceneChainTransitionOption option = SceneChainTransitionOption::Default;
        SceneChainTransitionScope scope = SceneChainTransitionScope::All;
        SceneChainTransitionContour contour = SceneChainTransitionContour::Smooth;
        SceneChainTransitionCondition condition = SceneChainTransitionCondition::Always;
        float lengthBeats = DefaultSceneTransitionLengthBeats;
        bool subtractFromSceneLength = false;
        float intensity = DefaultSceneTransitionIntensity;
        float delayAmount = DefaultSceneTransitionDelayAmount;
        float filterAmount = DefaultSceneTransitionFilterAmount;
        float chopAmount = DefaultSceneTransitionChopAmount;
    };

    struct SceneChainState
    {
        std::array<SceneChainStep, MaxSceneChainSteps> steps{};
        juce::File transitionEndSampleDirectory;
        bool loopEnabled = false;
        int loopStart = 0;
        int loopEnd = 0;
    };

    struct SceneTransitionEndSampleData
    {
        juce::AudioBuffer<float> buffer;
        double sourceSampleRate = 44100.0;
        juce::String path;
        juce::String displayName;
    };

    struct SceneTransitionEndSampleVoice
    {
        bool active = false;
        std::shared_ptr<const SceneTransitionEndSampleData> sample;
        double sourceSamplePosition = 0.0;
        double sourceIncrement = 1.0;
        int startOffsetSamples = 0;
        float gainLinear = 1.0f;
        int64_t renderedSamples = 0;
        float fadeInSamples = 0.0f;
        float fadeOutSamples = 0.0f;
        bool reverse = false;
        bool lowpassEnabled = false;
        bool highpassEnabled = false;
        float lowpassCoeff = 0.0f;
        float highpassCoeff = 0.0f;
        int duckSourceSelection = DefaultSceneTransitionEndSampleDuckSource;
        float duckAmount = DefaultSceneTransitionEndSampleDuckAmount;
        float duckSmoothedGain = 1.0f;
        std::array<float, 2> lowpassState{};
        std::array<float, 2> highpassInputState{};
        std::array<float, 2> highpassOutputState{};
    };

    struct ScenePlaybackHandle
    {
        bool active = false;
        bool sequenceDriven = false;
        int mainPresetIndex = 0;
        int sceneSlot = 0;
        int sequenceStepIndex = -1;
        double startPpq = 0.0;
        double resolvedLengthBeats = 4.0;
    };

    enum class SceneStripLaunchTransitionKind
    {
        Idle = 0,
        Continue,
        Restart,
        NewStart,
        Stop
    };

    struct SceneStripPlaybackHandle
    {
        void reset() noexcept
        {
            *this = {};
            stripIndex = -1;
            playbackColumn = 0;
            loopEnd = ModernAudioEngine::MaxColumns;
            recordingBars = 2;
            playheadSpeedRatio = 1.0f;
        }

        bool valid = false;
        bool present = false;
        bool playing = false;
        bool sequenceDriven = false;
        int mainPresetIndex = 0;
        int sceneSlot = 0;
        int stripIndex = -1;
        int sequenceStepIndex = -1;
        EnhancedAudioStrip::PlayMode playMode = EnhancedAudioStrip::PlayMode::Loop;
        int loopStart = 0;
        int loopEnd = ModernAudioEngine::MaxColumns;
        int recordingBars = 2;
        float beatsPerLoop = -1.0f;
        float playheadSpeedRatio = 1.0f;
        EnhancedAudioStrip::DirectionMode directionMode = EnhancedAudioStrip::DirectionMode::Normal;
        bool reverse = false;
        bool ppqTimelineAnchored = false;
        double ppqTimelineOffsetBeats = 0.0;
        int playbackColumn = 0;
        juce::File sampleFile;
        uint64_t audioSourceSignature = 0;
        EnhancedAudioStrip::ContinuityBlendState continuityBlendState;
        uint64_t revision = 0;
    };

    struct SceneStripLaunchHandle
    {
        void reset() noexcept
        {
            active = false;
            current.reset();
            lastTransition = SceneStripLaunchTransitionKind::Idle;
            lastTransitionContinuityPreserved = false;
            lastTransitionContinuityBroken = false;
            lastTransitionBlendPitchPath = false;
            lastTransitionApplyOutputBlend = false;
            lastTransitionApplyEdgeFade = false;
        }

        bool active = false;
        SceneStripPlaybackHandle current;
        SceneStripLaunchTransitionKind lastTransition = SceneStripLaunchTransitionKind::Idle;
        bool lastTransitionContinuityPreserved = false;
        bool lastTransitionContinuityBroken = false;
        bool lastTransitionBlendPitchPath = false;
        bool lastTransitionApplyOutputBlend = false;
        bool lastTransitionApplyEdgeFade = false;
    };

    enum class ScenePlaybackOwner
    {
        Manual = 0,
        Chain
    };
    using SceneLaunchOwner = ScenePlaybackOwner;

    struct SceneLaunchQuantisation
    {
        ScenePlaybackOwner owner = ScenePlaybackOwner::Manual;
        ScenePlaybackOwner outgoingOwner = ScenePlaybackOwner::Manual;
        SceneRecallMode recallMode = SceneRecallMode::QuantizeGrid;
        bool sequenceDriven = false;
        bool surfaceQuantizedLaunch = false;
        bool useSceneDurationTiming = false;
        bool targetResolved = false;
        bool targetWithinCurrentBlock = false;
        bool exactBlockBoundary = false;
        bool splitSwitchInBlock = false;
        bool legatoOwnerSwitch = false;
        int mainPresetIndex = 0;
        int sceneSlot = 0;
        int sequenceStepIndex = -1;
        double currentPpq = std::numeric_limits<double>::quiet_NaN();
        double currentTempo = std::numeric_limits<double>::quiet_NaN();
        double blockEndPpq = std::numeric_limits<double>::quiet_NaN();
        double intervalBeats = 4.0;
        double targetPpq = std::numeric_limits<double>::quiet_NaN();
        int64_t blockStartSample = -1;
        int64_t targetGlobalSample = -1;
        int blockNumSamples = 0;
        int targetSampleOffsetInBlock = -1;
        int boundaryCaptureSampleOffsetInBlock = -1;
    };

    struct SceneSwitchRange
    {
        bool playing = false;
        int startOffsetInBlock = 0;
        int numSamples = 0;
        double playStartPpq = std::numeric_limits<double>::quiet_NaN();
    };

    struct SceneSwitchSplitStatus
    {
        bool isSplit = false;
        SceneSwitchRange range1;
        SceneSwitchRange range2;
    };

    struct SceneSwitchEvent
    {
        bool active = false;
        ScenePlaybackOwner owner = ScenePlaybackOwner::Manual;
        ScenePlaybackOwner outgoingOwner = ScenePlaybackOwner::Manual;
        bool sequenceDriven = false;
        bool ownerOnlySwitch = false;
        bool exactBlockBoundary = false;
        bool splitSwitchInBlock = false;
        bool legatoOwnerSwitch = false;
        int mainPresetIndex = 0;
        int sceneSlot = 0;
        int sequenceStepIndex = -1;
        double targetPpq = std::numeric_limits<double>::quiet_NaN();
        double targetTempo = std::numeric_limits<double>::quiet_NaN();
        int64_t targetGlobalSample = -1;
        int64_t blockStartSample = -1;
        int blockNumSamples = 0;
        int targetSampleOffsetInBlock = -1;
        int boundaryCaptureSampleOffsetInBlock = -1;
        uint64_t serial = 0;
        SceneSwitchSplitStatus splitStatus;
    };

    struct SceneInfo
    {
        int mainPresetIndex = 0;
        int sceneSlot = 0;
        juce::String name;
        uint32_t colourArgb = 0xff5d7488;
    };

    struct SceneClipSlotState
    {
        int mainPresetIndex = 0;
        int sceneSlot = 0;
        bool hasStoredContent = false;
        bool hasMotionState = false;
        bool hasPerformanceClip = false;
        bool hasLiveStripControlState = false;
        bool liveStripControlDirty = false;
        int repeatCount = 1;
        SceneLengthMode lengthMode = SceneLengthMode::ManualBars;
        int lengthCount = 4;
        int anchorStrip = 0;
        bool inChain = false;
        int chainStepIndex = -1;
    };

    struct PreparedSceneSwitchPayload;
    struct PreparedSceneStripState;

    struct SceneSlotState
    {
        int mainPresetIndex = 0;
        int sceneSlot = 0;
        bool hasStoredContent = false;
        bool implicitMainPresetFallback = false;
        juce::String name;
        std::unique_ptr<PreparedSceneSwitchPayload> preparedSwitchPayloadTemplate;
    };

    struct SceneRuntimeSlotState
    {
        SceneInfo scene;
        SceneClipSlotState clipSlot;
        bool focused = false;
        bool active = false;
        bool queued = false;
        bool chainCurrent = false;
    };

    struct SceneSlotDefinition
    {
        int mainPresetIndex = 0;
        int sceneSlot = 0;
        juce::String name;
        uint32_t colourArgb = 0xff5d7488;
        bool hasStoredContent = false;
        int repeatCount = 1;
        SceneLengthMode lengthMode = SceneLengthMode::ManualBars;
        int lengthCount = 4;
        int anchorStrip = 0;
        bool inChain = false;
        int chainStepIndex = -1;
    };

    struct SceneWatcherState
    {
        std::array<SceneRuntimeSlotState, SceneSlots> slots{};
        int focusedSceneSlot = 0;
        int activeMainPresetIndex = 0;
        int activeSceneSlot = 0;
        int queuedMainPresetIndex = -1;
        int queuedSceneSlot = -1;
        bool hasQueuedScene = false;
        bool chainActive = false;
        int chainStepIndex = -1;
        ScenePlaybackOwner playbackOwner = ScenePlaybackOwner::Manual;
        bool manualPlaybackOwned = true;
        bool chainPlaybackOwned = false;
    };

    int getSceneRepeatCount(int sceneSlot) const;
    void setSceneRepeatCount(int sceneSlot, int repeats);
    SceneLengthMode getSceneLengthMode(int sceneSlot) const;
    void setSceneLengthMode(int sceneSlot, SceneLengthMode mode);
    SceneRecallMode getSceneRecallMode() const;
    void setSceneRecallMode(SceneRecallMode mode);
    int getSceneManualBars(int sceneSlot) const;
    void setSceneManualBars(int sceneSlot, int bars);
    int getSceneAnchorStrip(int sceneSlot) const;
    void setSceneAnchorStrip(int sceneSlot, int stripIndex);
    int getSceneLengthCount(int sceneSlot) const;
    void setSceneLengthCount(int sceneSlot, int count);
    double getResolvedSceneLengthBeats(int sceneSlot) const;
    double getSceneAdvanceLengthBeats(int sceneSlot) const;
    bool persistSceneTimingForSlot(int sceneSlot);
    void focusSceneSlot(int sceneSlot);
    int getFocusedSceneSlot() const;
    SceneInfo getSceneInfo(int sceneSlot, int mainPresetIndex = -1) const;
    SceneClipSlotState getSceneClipSlotState(int sceneSlot, int mainPresetIndex = -1) const;
    SceneSlotDefinition getSceneSlotDefinition(int sceneSlot, int mainPresetIndex = -1) const;
    SceneWatcherState getSceneWatcherState() const;
    void launchSceneSlotFromSurface(int sceneSlot);
    void launchSceneSlotFromMonome(int sceneSlot);
    void selectSceneSlotFromSurface(int sceneSlot);
    int getSceneSequenceStepIndex(int sceneSlot) const;
    int getQueuedSceneSlot() const;
    juce::String getSceneSequenceSummaryText() const;
    int getSceneChainLength() const;
    int getSceneChainStepSceneSlot(int stepIndex) const;
    int getSceneChainStepRepeatCount(int stepIndex) const;
    SceneChainTransitionType getSceneChainStepTransitionType(int stepIndex) const;
    SceneChainTransitionOption getSceneChainStepTransitionOption(int stepIndex) const;
    SceneChainTransitionScope getSceneChainStepTransitionScope(int stepIndex) const;
    SceneChainTransitionContour getSceneChainStepTransitionContour(int stepIndex) const;
    SceneChainTransitionCondition getSceneChainStepTransitionCondition(int stepIndex) const;
    float getSceneChainStepTransitionLengthBeats(int stepIndex) const;
    bool getSceneChainStepTransitionSubtractsFromSceneLength(int stepIndex) const;
    float getSceneChainStepTransitionIntensity(int stepIndex) const;
    float getSceneChainStepTransitionDelayAmount(int stepIndex) const;
    float getSceneChainStepTransitionFilterAmount(int stepIndex) const;
    float getSceneChainStepTransitionChopAmount(int stepIndex) const;
    juce::File getSceneTransitionEndSampleDirectory() const;
    juce::File getSceneChainStepTransitionEndSampleFile(int stepIndex) const;
    SceneTransitionEndSampleSettings getSceneChainStepTransitionEndSampleSettings(int stepIndex) const;
    SceneChainTransitionFavorite getSceneTransitionFavorite(int favoriteIndex) const;
    bool hasSceneTransitionFavorite(int favoriteIndex) const;
    void setSceneChainStep(int stepIndex, int sceneSlot, int repeats);
    void setSceneChainStepTransitionType(int stepIndex, SceneChainTransitionType type);
    void setSceneChainStepTransitionOption(int stepIndex, SceneChainTransitionOption option);
    void setSceneChainStepTransitionScope(int stepIndex, SceneChainTransitionScope scope);
    void setSceneChainStepTransitionContour(int stepIndex, SceneChainTransitionContour contour);
    void setSceneChainStepTransitionCondition(int stepIndex, SceneChainTransitionCondition condition);
    void setSceneChainStepTransitionLengthBeats(int stepIndex, float beats);
    void setSceneChainStepTransitionSubtractsFromSceneLength(int stepIndex, bool enabled);
    void setSceneChainStepTransitionIntensity(int stepIndex, float amount);
    void setSceneChainStepTransitionDelayAmount(int stepIndex, float amount);
    void setSceneChainStepTransitionFilterAmount(int stepIndex, float amount);
    void setSceneChainStepTransitionChopAmount(int stepIndex, float amount);
    void setSceneTransitionEndSampleDirectory(const juce::File& directory);
    bool setSceneChainStepTransitionEndSampleFile(int stepIndex, const juce::File& file);
    void setSceneChainStepTransitionEndSampleSettings(int stepIndex,
                                                      const SceneTransitionEndSampleSettings& settings);
    bool auditionSceneTransitionEndSampleFile(const juce::File& file,
                                              const SceneTransitionEndSampleSettings& settings);
    bool captureSceneTransitionFavorite(int favoriteIndex, int stepIndex);
    bool applySceneTransitionFavorite(int favoriteIndex, int stepIndex);
    void clearSceneChain();
    bool isSceneChainLoopEnabled() const;
    void setSceneChainLoopEnabled(bool enabled);
    int getSceneChainLoopStartStep() const;
    int getSceneChainLoopEndStep() const;
    void setSceneChainLoopRange(int startStep, int endStep);
    bool isSceneChainPlaybackActive() const;
    int getSceneChainPlaybackStepIndex() const;
    bool startSceneChainPlayback(int startStepIndex = 0);
    void stopSceneChainPlayback();
    void recallSceneSlot(int sceneSlot);
    void startScenePerformanceRecording(bool overdub = false);
    void stopScenePerformanceRecording();
    void extendScenePerformanceRecording();
    void setGlobalSceneStutterAmount(float amount01);
    float getGlobalSceneStutterAmount() const;
    bool clearSceneSlot(int sceneSlot);
    void clearScenePerformanceClip(int sceneSlot);
    bool isScenePerformanceRecording() const;
    bool isScenePerformanceOverdubbing() const;
    int getScenePerformanceRecordingSceneSlot() const;
    double getScenePerformanceRecordingStartBeat() const;
    double getScenePerformanceRecordingEndBeat() const;
    double getScenePerformanceClipLengthBeats(int sceneSlot) const;
    bool hasScenePerformanceClip(int sceneSlot) const;
    int getScenePerformanceEventCount(int sceneSlot) const;
    double getScenePerformancePlaybackProgress(int sceneSlot, double currentBeat) const;
    double getScenePerformancePlaybackBeat(int sceneSlot, double currentBeat) const;
    double getScenePerformanceRecordingProgress(double currentBeat) const;
    std::vector<ScenePerformanceEvent> getScenePerformanceEventsSnapshot(int sceneSlot) const;
    bool replaceScenePerformanceClipEvents(int sceneSlot, const std::vector<ScenePerformanceEvent>& events);
    bool getSceneControlBaseNormalizedValue(int stripIndex,
                                            ScenePerformanceControlTarget target,
                                            float& normalizedOut) const;
    bool getStoredSceneControlNormalizedValue(int sceneSlot,
                                              int stripIndex,
                                              ScenePerformanceControlTarget target,
                                              float& normalizedOut) const;
    bool getSceneControlCurrentValue(int stripIndex,
                                     ScenePerformanceControlTarget target,
                                     float& valueOut) const;
    bool isActiveSceneAutomationOverridden(int stripIndex, ScenePerformanceControlTarget target) const;
    bool hasAnyActiveSceneAutomationOverrides() const;
    void reenableActiveSceneAutomation();
    void applyLiveSceneControlTouch(int stripIndex,
                                    ScenePerformanceControlTarget target,
                                    ControlMode controlMode,
                                    int controlRow,
                                    float value,
                                    int columnHint,
                                    const std::function<void(StripControlWriteMode)>& applyLiveValue,
                                    bool liveValueAlreadyApplied = false);
    void notifyDirectSceneControlChange(int stripIndex,
                                        ScenePerformanceControlTarget target,
                                        ControlMode controlMode,
                                        int controlRow,
                                        float value,
                                        int columnHint = -1);
    bool copySceneSlotToClipboard(int sceneSlot);
    bool pasteSceneSlotFromClipboard(int sceneSlot);
    bool hasSceneSlotClipboard() const;
    int getSceneSlotClipboardSourceSlot() const;
    bool copyScenePerformanceClipToClipboard(int sceneSlot);
    bool pasteScenePerformanceClipFromClipboard(int sceneSlot);
    bool duplicateScenePerformanceClip(int sourceSceneSlot, int destSceneSlot);
    bool hasScenePerformanceClipboard() const;
    int getScenePerformanceClipboardSourceSlot() const;
    bool getSceneEditorGridEnabled() const { return sceneEditorGridEnabledState; }
    int getSceneEditorGridDivision() const { return sceneEditorGridDivisionState; }
    bool getSceneEditorDrawModeEnabled() const { return sceneEditorDrawModeEnabledState; }
    bool getSceneEditorLaneOverlaysEnabled() const { return sceneEditorLaneOverlaysEnabledState; }
    int getSceneEditorZoomFactor() const { return sceneEditorZoomFactorState; }
    bool getSceneEditorFollowPlayheadEnabled() const { return sceneEditorFollowPlayheadState; }
    SceneModPageMode getSceneModPageMode() const { return sceneModPageModeState; }
    bool getSceneEditorStripAutomationExpanded(int stripIndex) const;
    bool getSceneEditorStripHeightExpanded(int stripIndex) const;
    void setSceneEditorGridEnabled(bool enabled);
    void setSceneEditorGridDivision(int division);
    void setSceneEditorDrawModeEnabled(bool enabled);
    void setSceneEditorLaneOverlaysEnabled(bool enabled);
    void setSceneEditorZoomFactor(int factor);
    void setSceneEditorFollowPlayheadEnabled(bool enabled);
    void setSceneModPageMode(SceneModPageMode mode);
    void requestSceneControlRefreshAsync();
    void syncActiveSceneMotionState();
    void syncFocusedSceneMotionState();
    void setSceneEditorStripAutomationExpanded(int stripIndex, bool expanded);
    void setSceneEditorStripHeightExpanded(int stripIndex, bool expanded);
    void setSceneEditorStripAutomationExpandedAll(bool expanded);
    bool stripUsesGrainSceneLanes(int stripIndex) const;
    std::vector<ModernAudioEngine::ModTarget> getVisibleModTargetsForStrip(int stripIndex) const;
    std::vector<ModernAudioEngine::ModTarget> getSceneVisibleModTargetsForStrip(int stripIndex) const;
    ModernAudioEngine::ModTarget getSceneMainAutomationDisplayTargetForStrip(int stripIndex) const;
    void setSceneMainAutomationDisplayTargetForStrip(int stripIndex, ModernAudioEngine::ModTarget target);
    ModernAudioEngine::ModTarget getSceneMotionTargetForSlot(int stripIndex, int laneSlot) const;
    void setSceneMotionTargetForSlot(int stripIndex, int laneSlot, ModernAudioEngine::ModTarget target);
    bool clearSceneStripAutomationAndMotion(int sceneSlot, int stripIndex);
    void clearSceneMotionStripState(int sceneSlot, int stripIndex);
    void restoreSceneStripControlTargetsToStoredState(int sceneSlot,
                                                      int stripIndex,
                                                      const std::vector<ScenePerformanceControlTarget>& targets);
    void restoreSceneStripControlTargetsToDefaultState(int sceneSlot,
                                                       int stripIndex,
                                                       const std::vector<ScenePerformanceControlTarget>& targets);
    void copySceneMotionStripState(int sceneSlot, int sourceStripIndex, int destStripIndex);
    void stepVisibleModLaneTarget(int stripIndex, int direction);
    void stepSceneModLaneTarget(int stripIndex, int direction);
    void cycleSceneModLaneTarget(int stripIndex);
    void setSceneStepMotionEditorOpen(bool isOpen);
    bool isSceneStepMotionEditorOpen() const { return sceneStepMotionEditorOpenState.load(std::memory_order_acquire) != 0; }
    bool ensureActiveScenePlaybackHandleInitialized();
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
    static float getDefaultMacroNormalizedValue(MacroTarget target);
    static bool macroTargetWritesToSceneLane(MacroTarget target);
    
    // Parameters
    juce::AudioProcessorValueTreeState parameters;
    
    static constexpr int MaxStrips = ModernAudioEngine::MaxStrips;
    static constexpr int MaxColumns = 16;
    static constexpr int MaxGridWidth = 16;
    static constexpr int MaxGridHeight = 16;

private:
    friend class MacroTargetDispatcher;
    friend class SceneScheduler;
    friend int getSceneChainLengthInternal(const MlrVSTAudioProcessor& processor);
    friend int resolveSceneChainNextStepIndex(const MlrVSTAudioProcessor& processor, int currentStepIndex);
    friend void sanitizeSceneChainRuntimeState(MlrVSTAudioProcessor& processor);
    friend void markSceneChainDefinitionChanged(MlrVSTAudioProcessor& processor);

    struct MonomeLayoutState
    {
        enum class TopRowMode
        {
            Launch,
            SceneLaunch,
            PresetGrid,
            StepEdit,
            Gate,
            Filter,
            Modulation
        };

        int gridWidth = MaxGridWidth;
        int gridHeight = 8;
        int groupRow = 0;
        int firstStripRow = 1;
        int controlRow = 1;
        int visibleStripCount = 0;
        int stepEditBankSize = 6;
        int maxStepEditBank = 0;
        int maxVisibleStripIndex = 0;
        int stripRowsDenom = 1;
        int modulationRowsDenom = 1;
        int lastDisplayedStripRow = 1;
        int lastPresetRow = 0;
        bool presetModeActive = false;
        bool stepEditModeActive = false;
        bool sceneModeActive = false;
        bool patternRecorderVisibleOnControlPage = false;
        bool topRowSceneMode = false;
        bool sceneRecorderVisible = false;
        bool topRowEditSupported = false;
        bool topRowEditActive = false;
        int sceneActionStartColumn = 4;
        int topRowEditToggleColumn = 15;
        TopRowMode topRowMode = TopRowMode::Launch;

        int clampVisibleStrip(int index) const noexcept
        {
            return juce::jlimit(0, maxVisibleStripIndex, index);
        }

        bool isPresetCell(int gridX, int gridY) const noexcept
        {
            return gridX >= 0 && gridX < PresetColumns
                && gridY >= 0 && gridY < PresetRows;
        }

        int toPresetIndex(int gridX, int gridY) const noexcept
        {
            return (gridY * PresetColumns) + gridX;
        }

        bool isSceneTopCell(int gridX, int gridY) const noexcept
        {
            return topRowMode == TopRowMode::SceneLaunch
                && gridY == groupRow
                && gridX >= 0
                && gridX < juce::jmin(gridWidth, SceneSlots - 1);
        }

        bool isSceneActionCell(int gridX, int gridY) const noexcept
        {
            return topRowMode == TopRowMode::SceneLaunch
                && gridY == groupRow
                && gridX >= sceneActionStartColumn
                && gridX < (sceneActionStartColumn + 4);
        }

        bool topRowUsesLaunchSurface() const noexcept
        {
            return topRowMode == TopRowMode::Launch || topRowMode == TopRowMode::SceneLaunch;
        }

        bool isDisplayedDataRow(int row) const noexcept
        {
            if (row < firstStripRow)
                return false;

            return presetModeActive
                ? (row <= lastPresetRow)
                : (row <= lastDisplayedStripRow);
        }
    };

    MonomeLayoutState getMonomeLayoutState() const;
    bool isMonomeControlRowUtilityCell(const MonomeLayoutState& layout, int x) const;

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
        EnhancedAudioStrip::PitchShiftAlgorithm pitchAlgorithm =
            EnhancedAudioStrip::PitchShiftAlgorithm::Standard;
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

    ResolvedPitchControl resolvePitchControl(const EnhancedAudioStrip& strip,
                                             float semitones,
                                             int referenceRootMidi,
                                             PitchControlMode controlMode) const;
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
        float beatsPerLoopOverride = std::numeric_limits<float>::quiet_NaN();
        bool quantized = false;
        double targetPpq = 0.0;
        int quantizeDivision = 8;
        bool postClearTriggerArmed = false;
        int postClearTriggerColumn = 0;
    };

    struct PendingBarChange
    {
        bool active = false;
        int recordingBars = 2;
        float beatsPerLoop = 8.0f;
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
    std::atomic<float>* continuousTraversalParam = nullptr;
    std::atomic<float>* flipTempoMatchModeParam = nullptr;
    std::atomic<float>* soundTouchEnabledParam = nullptr;
    std::atomic<float>* masterDuckTriggerStripParam = nullptr;
    std::atomic<float>* sceneRecallModeParam = nullptr;
    std::atomic<float>* transientOnsetMethodParam = nullptr;
    std::atomic<float>* transientSensitivityParam = nullptr;
    std::atomic<float>* transientSnapParam = nullptr;
    std::atomic<float>* transientSpacingParam = nullptr;
    std::array<std::atomic<float>*, MaxStrips> stripVolumeParams{};
    std::array<std::atomic<float>*, MaxStrips> stripTrimDbParams{};
    std::array<std::atomic<float>*, MaxStrips> stripPanParams{};
    std::array<std::atomic<float>*, MaxStrips> stripSpeedParams{};
    std::array<std::atomic<float>*, MaxStrips> stripPitchParams{};
    std::array<std::atomic<float>*, MaxStrips> stripSliceLengthParams{};
    std::array<std::atomic<float>*, MaxStrips> stripPitchControlModeParams{};
    std::array<std::atomic<float>*, MaxStrips> stripTempoMatchModeParams{};
    std::array<std::atomic<float>*, MaxStrips> stripFilterEnabledParams{};
    std::array<std::atomic<float>*, MaxStrips> stripFilterFrequencyParams{};
    std::array<std::atomic<float>*, MaxStrips> stripFilterResonanceParams{};
    std::array<std::atomic<float>*, MaxStrips> stripFilterMorphParams{};
    std::array<std::atomic<float>*, MaxStrips> stripFilterAlgorithmParams{};
    std::array<std::atomic<float>*, MaxStrips> stripDuckEnabledParams{};
    std::array<std::atomic<float>*, MaxStrips> stripDuckSourceParams{};
    std::array<std::atomic<float>*, MaxStrips> stripDuckThresholdParams{};
    std::array<std::atomic<float>*, MaxStrips> stripDuckRatioParams{};
    std::array<std::atomic<float>*, MaxStrips> stripDuckAttackParams{};
    std::array<std::atomic<float>*, MaxStrips> stripDuckReleaseParams{};
    std::array<std::atomic<float>*, MaxStrips> stripDuckGainCompParams{};
    std::array<std::atomic<float>*, MaxStrips> stripDuckFollowMasterParams{};
    std::array<std::atomic<float>*, MaxStrips> stripDelayMixParams{};
    std::array<std::atomic<float>*, MaxStrips> stripDelayTimeParams{};
    std::array<std::atomic<float>*, MaxStrips> stripDelaySyncParams{};
    std::array<std::atomic<float>*, MaxStrips> stripDelayFeedbackParams{};
    std::array<std::atomic<float>*, MaxStrips> stripDelayLowCutParams{};
    std::array<std::atomic<float>*, MaxStrips> stripDelayHighCutParams{};
    std::array<std::atomic<float>*, MaxStrips> stripDelayModeParams{};
    juce::CriticalSection pendingLoopChangeLock;
    std::array<PendingLoopChange, MaxStrips> pendingLoopChanges{};
    juce::CriticalSection pendingBarChangeLock;
    std::array<PendingBarChange, MaxStrips> pendingBarChanges{};
    std::array<bool, MaxStrips> pendingBarLengthApply{};
    
    double currentSampleRate = 44100.0;
    int lastReportedLatencySamples = 0;
    ControlMode currentControlMode = ControlMode::Normal;
    bool controlModeActive = false;  // True when control button is held
    bool monomeTopRowEditOverlayActive = false;
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
        ControlMode::Delay,
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
    std::atomic<int> innerLoopLengthSelection{0}; // 0=1,1=1/2,2=1/4,3=1/8,4=1/16
    std::atomic<int> lastAppliedInnerLoopLengthSelection{0};
    std::atomic<int> gatePageMode{0};
    int lastAppliedStretchBackend = -1; // -1 = force initial sync on first process block
    int lastAppliedContinuousTraversal = -1;
    int lastAppliedLoopTempoMatchBackend = -1;
    int lastAppliedTransientOnsetMethod = -1;
    int lastAppliedTransientSensitivity = -1;
    int lastAppliedTransientSnap = -1;
    int lastAppliedTransientSpacing = -1;
    
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
    std::atomic<int> pendingActiveSceneAutosaveDirty{0};
    std::atomic<int> pendingActiveSceneAutosaveMainPreset{-1};
    std::atomic<int> pendingActiveSceneAutosaveSlot{-1};
    std::atomic<uint32_t> pendingActiveSceneAutosaveQueuedMs{0};
    std::atomic<int> sceneAutosaveSuppressionDepth{0};
    std::array<std::array<bool, BrowserFavoriteSlots>, MaxStrips> browserFavoritePadHeld{};
    std::array<std::array<bool, BrowserFavoriteSlots>, MaxStrips> browserFavoritePadHoldSaveTriggered{};
    std::array<std::array<uint32_t, BrowserFavoriteSlots>, MaxStrips> browserFavoritePadPressStartMs{};
    std::array<uint32_t, BrowserFavoriteSlots> browserFavoriteSaveBurstUntilMs{};
    std::array<uint32_t, BrowserFavoriteSlots> browserFavoriteMissingBurstUntilMs{};
    std::array<std::atomic<int>, MacroCount> macroMidiCcAssignments{};
    std::array<std::atomic<int>, MacroCount> macroTargetAssignments{};
    std::atomic<int> macroMidiLearnIndex{-1};
    std::array<int, 4> arcKeyHeld{};
    std::array<std::array<int, 64>, 4> arcRingCache{};
    ArcControlMode arcControlMode = ArcControlMode::SelectedStrip;
    int arcSelectedModStep = 0;
    juce::int64 lastGridLedUpdateTimeMs = 0;
    uint32_t lastSceneMotionSyncTimeMs = 0;
    uint32_t lastPitchCacheRefreshTimeMs = 0;
    std::atomic<int> lastHostTransportPlaying{0};
    static constexpr int kGridRefreshMs = 33;
    static constexpr int kArcRefreshMs = 33;
    static constexpr int kSceneRecallFastRefreshMs = 1;
    static constexpr int kSceneRecallArmedRefreshMs = 8;
    static constexpr uint32_t kSceneMotionSyncRefreshMs = 33;
    static constexpr uint32_t kPitchCacheRefreshMs = 50;
    static constexpr uint32_t browserFavoriteHoldSaveMs = 3000;
    static constexpr uint32_t browserFavoriteSaveBurstDurationMs = 320;
    static constexpr uint32_t browserFavoriteMissingBurstDurationMs = 260;
    
    // Current file per strip for proper next/prev browsing
    std::array<juce::File, MaxStrips> currentStripFiles;
    std::array<juce::File, MaxStrips> pendingLoopStripFiles;
    std::array<std::unique_ptr<SampleModeEngine>, MaxStrips> sampleModeEngines;
    std::array<juce::AudioBuffer<float>, MaxStrips> sampleModeScratchBuffers;
    std::array<bool, MaxStrips> sampleModeRenderedLastBlock{};
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
    juce::ThreadPool soundTouchPitchCacheThreadPool{1};
    juce::ThreadPool bungeePitchCacheThreadPool{1};
    juce::ThreadPool signalsmithPitchCacheThreadPool{1};
    mutable juce::CriticalSection loopStripLoadStatusLock;
    std::array<juce::String, MaxStrips> loopStripLoadStatusTexts;
    mutable juce::CriticalSection loopPitchAnalysisStatusLock;
    std::array<juce::String, MaxStrips> loopPitchAnalysisStatusTexts;
    juce::CriticalSection loopStripLoadResultLock;
    std::vector<LoopStripLoadResult> loopStripLoadResults;
    juce::CriticalSection loopPitchAnalysisResultLock;
    std::vector<LoopPitchAnalysisResult> loopPitchAnalysisResults;
    juce::CriticalSection soundTouchPitchCacheResultLock;
    std::vector<SoundTouchPitchCacheResult> soundTouchPitchCacheResults;
    std::array<std::atomic<int>, MaxStrips> soundTouchPitchCacheRequestIds{};
    std::array<std::atomic<int>, MaxStrips> soundTouchPitchCacheInFlight{};
    std::array<float, MaxStrips> soundTouchPitchCacheObservedTargets{};
    std::array<int, MaxStrips> soundTouchPitchCacheStableTicks{};
    juce::CriticalSection bungeePitchCacheResultLock;
    std::vector<BungeePitchCacheResult> bungeePitchCacheResults;
    std::array<std::atomic<int>, MaxStrips> bungeePitchCacheRequestIds{};
    std::array<std::atomic<int>, MaxStrips> bungeePitchCacheInFlight{};
    std::array<float, MaxStrips> bungeePitchCacheObservedTargets{};
    std::array<int, MaxStrips> bungeePitchCacheStableTicks{};
    juce::CriticalSection signalsmithPitchCacheResultLock;
    std::vector<SignalsmithPitchCacheResult> signalsmithPitchCacheResults;
    std::array<std::atomic<int>, MaxStrips> signalsmithPitchCacheRequestIds{};
    std::array<std::atomic<int>, MaxStrips> signalsmithPitchCacheInFlight{};
    std::array<float, MaxStrips> signalsmithPitchCacheObservedTargets{};
    std::array<int, MaxStrips> signalsmithPitchCacheStableTicks{};
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
        const SampleModeEngine* engineToken = nullptr;
        uint64_t version = 0;
        std::shared_ptr<const SampleModeEngine::LegacyLoopSyncInfo> syncInfo;
    };
    struct PendingFlipLegacyLoopTrigger
    {
        bool valid = false;
        SampleModeEngine::LegacyLoopSyncInfo syncInfo;
        bool isMomentaryStutter = false;
    };
    class SoundTouchPitchCacheJob;
    class BungeePitchCacheJob;
    class SignalsmithPitchCacheJob;
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
    bool monomeControlPageShowsPatternRecorder(ControlMode mode) const;
    void recordMonomeControlPatternEvent(ControlMode mode,
                                         int targetStripIndex,
                                         int controlRow,
                                         int column);
    void playbackMonomeControlPatternEvent(const PatternRecorder::Event& event);
    bool handleMonomePatternButtonPress(int patternIndex, uint32_t nowMs);
    void processPendingMonomePatternTapActions(uint32_t nowMs);
    void clearPendingMonomePatternTap(int patternIndex);
    enum class SceneRecorderAction
    {
        None = 0,
        StartFreshRecording,
        StartOverdub,
        StopRecording
    };
    bool handleMonomeSceneRecorderButtonPress(uint32_t nowMs);
    void processPendingMonomeSceneRecorderTapActions(uint32_t nowMs);
    void clearPendingMonomeSceneRecorderTap();
    void requestSceneRecorderActionQuantized(SceneRecorderAction action);
    void updateSceneRecorderQuantizedAction(const juce::AudioPlayHead::PositionInfo& posInfo, int numSamples);
    void processPendingSceneRecorderApply();
    void clearPendingSceneRecorderAction();
    void startScenePerformanceRecordingAt(bool overdub,
                                          int sceneSlot,
                                          double currentBeat,
                                          double sceneStartBeat,
                                          bool updateLeds = true);
    void stopScenePerformanceRecordingNow(bool updateLeds = true);
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
    void queueSoundTouchPitchCacheResult(SoundTouchPitchCacheResult result);
    void applyCompletedSoundTouchPitchCaches();
    void refreshPendingSoundTouchPitchCaches();
    void queueBungeePitchCacheResult(BungeePitchCacheResult result);
    void applyCompletedBungeePitchCaches();
    void refreshPendingBungeePitchCaches();
    void queueSignalsmithPitchCacheResult(SignalsmithPitchCacheResult result);
    void applyCompletedSignalsmithPitchCaches();
    void refreshPendingSignalsmithPitchCaches();
    bool beginLoopStripPitchAnalysis(int stripIndex, bool setDetectedAsRoot);
    void updateLoopPitchAnalysisProgress(int stripIndex, int requestId, float progress, const juce::String& statusText);
    void resetLoopPitchAnalysisProgress(int stripIndex);
    void updateLoopStripLoadProgress(int stripIndex, int requestId, float progress, const juce::String& statusText);
    void resetLoopStripLoadProgress(int stripIndex);
    float getStoredStripPitchSemitones(int stripIndex) const;
    void applyStoredPitchControlToStrip(int stripIndex);
    void applyLoopStripPitchSemitones(int stripIndex, float semitones);
    void normalizeLoopPitchMasterRoles();
    int getEffectiveLoopPitchMasterRootMidi(int stripIndex) const;
    void applyLoopPitchRoleStateToStrip(int stripIndex);
    void reapplyGlobalPitchQuantizationToAllStrips();
    void resolveLoopPitchRecallStateImmediately();
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
    void queueLoopChange(int stripIndex,
                         bool clearLoop,
                         int startColumn,
                         int endColumn,
                         bool reverseDirection,
                         int markerColumn = -1,
                         float beatsPerLoopOverride = std::numeric_limits<float>::quiet_NaN());
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
    bool getCurrentHostPositionInfo(juce::AudioPlayHead::PositionInfo& outPosition) const;
    bool getCurrentHostPpq(double& outPpq) const;
    bool isHostTransportPlaying() const;
    bool getHostSyncSnapshot(double& outPpq, double& outTempo) const;
    void refreshUtilityTimerCadence();
    int getActiveMainPresetIndexForScenes() const;
    int getSceneStoragePresetIndex(int mainPresetIndex, int sceneSlot) const;
    bool saveSceneForMainPreset(int mainPresetIndex, int sceneSlot);
    bool copySceneForMainPreset(int mainPresetIndex, int sourceSceneSlot, int destSceneSlot);
    bool deleteSceneForMainPreset(int mainPresetIndex, int sceneSlot);
    bool sceneSlotExistsForMainPreset(int mainPresetIndex, int sceneSlot) const;
    void requestSceneRecallQuantized(int mainPresetIndex,
                                     int sceneSlot,
                                     bool sequenceDriven,
                                     int sequenceStepIndex = -1,
                                     bool useTriggerQuantization = false);
    double getSceneRecallIntervalBeats() const;
    void updateSceneQuantizedRecall(const juce::AudioPlayHead::PositionInfo& posInfo, int numSamples);
    void processPendingSceneApply();
    void performEmptySceneLoad();
    void performSceneLoad(int mainPresetIndex,
                          int sceneSlot,
                          double hostPpqSnapshot,
                          double hostTempoSnapshot,
                          int64_t hostGlobalSampleSnapshot,
                          bool preserveLoadedStripAudio = false,
                          bool* recallContinuityBrokenOut = nullptr);
    double computeNextScenePatternEndPpq(int sceneSlot,
                                         double currentPpq,
                                         double cycleBeats,
                                         uint64_t* outPhaseSignature = nullptr) const;
    double computeCurrentSceneSequenceLengthBeats() const;
    double computeStripSceneSequenceLengthBeats(int stripIndex) const;
    double computeLongestStripSceneSequenceLengthBeats() const;
    double computeLongestPatternSceneSequenceLengthBeats() const;
    void armNextSceneInSequence(int mainPresetIndex, int currentSceneSlot, double sceneStartPpq);
    std::unique_ptr<juce::XmlElement> createSceneChainStateXml(int sceneSlotOverride) const;
    void applySceneChainStateXml(const juce::XmlElement* xml, int sceneSlotOverride);
    juce::MemoryBlock createScenePerformanceStateData(int sceneSlotOverride) const;
    bool applyScenePerformanceStateData(const juce::MemoryBlock& data, int sceneSlotOverride);
    void syncScenePerformanceClipLengthToResolvedLength(int sceneSlot);
    void syncAllScenePerformanceClipLengthsToResolvedLengths();
    bool sceneSlotHasMotionState(int sceneSlot) const;
    void syncSceneMotionStateFromEngine(int sceneSlot);
    void ensureSceneMotionStateInitialized(int sceneSlot);
    void applySceneMotionStateToEngine(int sceneSlot);
    void applySceneMotionStateOrDefaultsToEngine(int sceneSlot);
    void updateAudioEngineSceneModulationContext() const;
    void syncSceneModeFromParameters();
    void applySceneModeState(bool enabled);
    void captureSceneModeGroupSnapshot();
    void restoreSceneModeGroupSnapshot();
    void clearAllStripGroupsForSceneMode();
    void appendSceneModeStateToState(juce::ValueTree& state) const;
    void loadSceneModeStateFromState(const juce::ValueTree& state);
    void processScenePerformancePlayback(const juce::AudioPlayHead::PositionInfo& posInfo, int numSamples);
    void clearSceneBoundaryTransitionState(bool preserveScheduledEndSample = false);
    void armSceneBoundaryTransition(SceneChainTransitionType type,
                                    SceneChainTransitionOption option,
                                    SceneChainTransitionScope scope,
                                    SceneChainTransitionContour contour,
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
                                    int64_t targetSample,
                                    double endSampleDelayBeats);
    void applySceneBoundaryTransitionOverlay(const juce::AudioPlayHead::PositionInfo& posInfo, int numSamples);
    void renderSceneTransitionEndSampleVoices(juce::AudioBuffer<float>& buffer,
                                              const juce::AudioPlayHead::PositionInfo& posInfo,
                                              int64_t blockStartSample,
                                              bool allowTriggering);
    void setSceneTransitionStutterOverlayAmount(float amount01);
    void updateEffectiveSceneStutterAmount();
    void armSceneChainReturnOverride(int sourceStepIndex, int triggerStepIndex);
    void clearSceneChainReturnOverride();
    bool consumeSceneChainReturnOverrideForStep(int currentStepIndex, int& outNextStepIndex);
    void recordSceneTriggerEvent(int stripIndex,
                                 int column,
                                 double eventBeat,
                                 int sampleSliceId,
                                 int64_t sampleStartSample,
                                 bool noteOn = true);
    void clearPendingSceneTriggerRecord(int stripIndex);
    void rememberPendingSceneTriggerRecord(int stripIndex, int column, double eventBeat);
    void cancelPendingSceneTriggerRecord(int stripIndex);
    void captureSceneTriggerRelease(int stripIndex, int columnHint);
    void recordMonomeControlSceneEvent(ControlMode mode,
                                       int targetStripIndex,
                                       int controlRow,
                                       int column);
    bool resolveScenePerformanceControlEvent(ControlMode mode,
                                             int stripIndex,
                                             int controlRow,
                                             int column,
                                             ScenePerformanceEvent& outEvent) const;
    bool resolveScenePerformanceMacroEvent(int stripIndex,
                                           MacroTarget target,
                                           float normalizedValue,
                                           ScenePerformanceEvent& outEvent) const;
    void recordMacroSceneEvent(int stripIndex, MacroTarget target, float normalizedValue);
    void recordSceneGlobalStutterEvent(float normalizedValue, int columnHint, double eventBeat);
    void recordMomentaryStutterSceneDivision(double divisionBeats, int columnHint, double eventBeat);
    void applyScenePerformanceEvent(const ScenePerformanceEvent& event,
                                    StripControlWriteMode writeMode);
    void playbackScenePerformanceEvent(const ScenePerformanceEvent& event);
    enum class ManualSceneControlHandling
    {
        Ignored = 0,
        PassedThrough,
        Recorded,
        OverrodeAutomation
    };
    ManualSceneControlHandling handleLiveSceneControlTouch(int stripIndex,
                                                           ScenePerformanceControlTarget target,
                                                           ControlMode controlMode,
                                                           int controlRow,
                                                           float value,
                                                           int columnHint,
                                                           const std::function<void(StripControlWriteMode)>& applyLiveValue,
                                                           bool liveValueAlreadyApplied);
    ManualSceneControlHandling processManualSceneControlChange(int stripIndex,
                                                               ScenePerformanceControlTarget target,
                                                               ControlMode controlMode,
                                                               int controlRow,
                                                               float value,
                                                               int columnHint = -1);
    void refreshSceneAutomationTargetMask(int sceneSlot);
    void refreshAllSceneAutomationTargetMasks();
    bool sceneClipHasAutomationTarget(int sceneSlot,
                                      int stripIndex,
                                      ScenePerformanceControlTarget target) const;
    const PreparedSceneStripState* getStoredSceneStripStateForSlot(int sceneSlot,
                                                                   int stripIndex,
                                                                   PreparedSceneStripState& fallbackStripState) const;
    bool stripUsesGrainSceneLanesForSceneSlot(int sceneSlot, int stripIndex) const;
    double getSceneAutomationTransitionSeconds() const;
    double getSceneAutomationTransitionSeconds(ScenePerformanceControlTarget target) const;
    bool shouldSmoothLiveSceneControlTarget(ScenePerformanceControlTarget target) const;
    void seedSceneControlTransition(int stripIndex,
                                    ScenePerformanceControlTarget target,
                                    float fromValue,
                                    float toValue,
                                    double rampSeconds);
    void beginSceneManualControlHandlingSuppression();
    void endSceneManualControlHandlingSuppression();
    bool isSceneManualControlHandlingSuppressed() const;
    void refreshMomentaryStutterSavedStateFromCurrentStrip(int stripIndex,
                                                           ScenePerformanceControlTarget target);
    void rescaleActiveInnerLoopsForGlobalFactor(int previousChoice, int newChoice);
    void applySceneHeldAutomationStateAtBeat(int sceneSlot,
                                             double currentBeat,
                                             double sceneStartBeat);
    void clearActiveSceneAutomationOverrides(bool restoreWrittenValues);
    void clearActiveSceneAutomationOverrideForRecordedTarget(int sceneSlot,
                                                             int stripIndex,
                                                             ScenePerformanceControlTarget target);
    void pruneActiveSceneAutomationOverrides();
    void copyScenePerformanceClip(int sourceSceneSlot, int destSceneSlot);
    void handleIncomingMacroCc(const juce::MidiBuffer& midiMessages);
    int getMacroTargetStripIndex() const;
    float getMacroNormalizedValueForTarget(int stripIndex, const EnhancedAudioStrip& strip, MacroTarget target) const;
    void applyMacroTargetValue(int stripIndex, EnhancedAudioStrip& strip, MacroTarget target, float normalizedValue);
    void setStripParameterValueFromMacro(int stripIndex, const juce::String& parameterId, float plainValue);
    using ResolvedOwnedStripControlState = StripControlState::ResolvedOwnedStripControlState;
    ResolvedOwnedStripControlState resolveOwnedStripControlStateFromParameters(int stripIndex,
                                                                               const EnhancedAudioStrip& strip) const;
    void applyResolvedOwnedStripControlState(EnhancedAudioStrip& strip,
                                             const ResolvedOwnedStripControlState& state) const;
    void applyResolvedStripFilterState(EnhancedAudioStrip& strip,
                                       const ResolvedOwnedStripControlState& state) const;
    void applyResolvedStripDelayState(EnhancedAudioStrip& strip,
                                      const ResolvedOwnedStripControlState& state) const;
    StripControlState::ParameterView makeOwnedStripControlParameterView(int stripIndex) const;
    void writeStripFloatParameter(const juce::String& parameterId,
                                  float plainValue,
                                  std::atomic<float>* rawParam,
                                  StripControlWriteMode writeMode);
    void writeStripBoolParameter(const juce::String& parameterId,
                                 bool enabled,
                                 std::atomic<float>* rawParam,
                                 StripControlWriteMode writeMode);
    void applyOwnedStripControlsFromParameters(int stripIndex, EnhancedAudioStrip& strip);
    void performPresetLoad(int presetIndex, double hostPpqSnapshot, double hostTempoSnapshot);
    struct SceneFileAudioCacheEntry
    {
        juce::String filePath;
        int64_t fileSize = -1;
        int64_t fileModificationTimeMs = 0;
        juce::AudioBuffer<float> audioBuffer;
        double sourceSampleRate = 0.0;
        std::array<int, 16> transientSlices{};
        std::array<float, 128> rmsMap{};
        std::array<int, 128> zeroCrossMap{};
        int analysisSampleCount = 0;
        uint64_t useCounter = 0;
    };
    struct PreparedSceneStripAudioPayload
    {
        juce::AudioBuffer<float> audioBuffer;
        double sourceSampleRate = 0.0;
        std::array<int, 16> transientSlices{};
        std::array<float, 128> rmsMap{};
        std::array<int, 128> zeroCrossMap{};
        int analysisSampleCount = 0;
        bool valid = false;
    };
    struct PreparedSceneStripParameterState
    {
        bool valid = false;
        ResolvedOwnedStripControlState ownedControls;
        float pitchSemitones = 0.0f;
        float sliceLength = 1.0f;
        int pitchControlMode = 0;
        int tempoMatchMode = 0;
        bool duckEnabled = false;
        int duckSource = 0;
        float duckThresholdDb = -24.0f;
        float duckRatio = 4.0f;
        float duckAttackMs = 10.0f;
        float duckReleaseMs = 180.0f;
        float duckGainCompDb = 0.0f;
        bool duckFollowMaster = false;
    };
    struct PreparedSceneModLaneState
    {
        ModernAudioEngine::ModTarget target = ModernAudioEngine::defaultModTargetForSlot(0);
        bool bipolar = false;
        bool curveMode = false;
        float depth = 1.0f;
        float rate = 1.0f;
        ModernAudioEngine::ModTransportMode transportMode = ModernAudioEngine::ModTransportMode::Free;
        int offset = 0;
        int lengthBars = 1;
        int editPage = 0;
        float smoothingMs = 0.0f;
        float curveBend = 0.0f;
        ModernAudioEngine::ModCurveShape curveShape = ModernAudioEngine::ModCurveShape::Linear;
        bool pitchScaleQuantize = false;
        ModernAudioEngine::PitchScale pitchScale = ModernAudioEngine::PitchScale::Chromatic;
        std::array<float, ModernAudioEngine::ModTotalSteps> steps{};
        std::array<int, ModernAudioEngine::ModTotalSteps> stepSubdivisions{};
        std::array<float, ModernAudioEngine::ModTotalSteps> stepEndValues{};
        std::array<ModernAudioEngine::ModCurveShape, ModernAudioEngine::ModTotalSteps> stepCurveShapes{};
    };
    struct PreparedSceneLoopPitchState
    {
        bool valid = false;
        LoopPitchRole role = LoopPitchRole::None;
        LoopPitchSyncTiming syncTiming = LoopPitchSyncTiming::Immediate;
        int assignedMidi = -1;
        bool assignedManual = false;
        int detectedMidi = -1;
        float detectedHz = 0.0f;
        float detectedPitchConfidence = 0.0f;
        int detectedScaleIndex = -1;
        float detectedScaleConfidence = 0.0f;
        bool essentiaUsed = false;
    };
    struct PreparedSceneTimingState
    {
        bool valid = false;
        int repeatCount = 1;
        int lengthModeIndex = static_cast<int>(SceneLengthMode::ManualBars);
        int manualBars = 4;
        int anchorStrip = 0;
    };
public:
    struct PreparedSceneStripState
    {
        bool present = false;
        bool restorePlaying = false;
        bool hasStoredSamplePath = false;
        juce::File storedSampleFile;
        juce::String embeddedSampleWavBase64;
        juce::File recentLoopDirectory;
        juce::File recentStepDirectory;
        juce::File recentFlipDirectory;
        int analysisSampleCount = 0;
        std::array<int, 16> analysisTransientSlices{};
        std::array<float, 128> analysisRmsMap{};
        std::array<int, 128> analysisZeroCrossMap{};
        EnhancedAudioStrip::PlayMode playMode = EnhancedAudioStrip::PlayMode::Loop;
        int loopStart = 0;
        int loopEnd = ModernAudioEngine::MaxColumns;
        int playbackColumn = 0;
        bool ppqTimelineAnchored = false;
        double ppqTimelineOffsetBeats = 0.0;
        EnhancedAudioStrip::DirectionMode directionMode = EnhancedAudioStrip::DirectionMode::Normal;
        bool reverse = false;
        int groupId = -1;
        float beatsPerLoop = -1.0f;
        float scratchAmount = 0.0f;
        bool transientSliceMode = false;
        float pitchShift = 0.0f;
        int recordingBars = 2;
        bool filterEnabled = false;
        float filterFrequency = 20000.0f;
        float filterResonance = 0.707f;
        float filterMorph = 0.0f;
        EnhancedAudioStrip::FilterAlgorithm filterAlgorithm = EnhancedAudioStrip::FilterAlgorithm::Tpt12;
        float swingAmount = 0.0f;
        float gateAmount = 0.0f;
        float gateSpeed = 4.0f;
        float gateEnvelope = 0.5f;
        float gateShape = 0.5f;
        int stepPatternSteps = 16;
        int stepViewPage = 0;
        int stepCurrent = 0;
        std::array<bool, 64> stepPattern{};
        std::array<int, 64> stepSubdivisions{};
        std::array<float, 64> stepSubdivisionStartVelocity{};
        std::array<float, 64> stepSubdivisionRepeatVelocity{};
        std::array<float, 64> stepProbability{};
        float stepAttackMs = 0.0f;
        float stepDecayMs = 4000.0f;
        float stepReleaseMs = 110.0f;
        float grainSizeMs = 1240.0f;
        float grainDensity = 0.05f;
        float grainPitch = 0.0f;
        float grainPitchJitter = 0.0f;
        float grainSpread = 0.0f;
        float grainJitter = 0.0f;
        float grainPositionJitter = 0.0f;
        float grainRandomDepth = 0.0f;
        float grainArpDepth = 0.0f;
        float grainCloudDepth = 0.0f;
        float grainEmitterDepth = 0.0f;
        float grainEnvelope = 0.0f;
        float grainShape = 0.0f;
        int grainArpMode = 0;
        bool grainTempoSync = true;
        int activeModSlot = 0;
        std::array<PreparedSceneModLaneState, ModernAudioEngine::NumModSequencers> modLanes{};
        PreparedSceneStripParameterState parameterState;
        bool hasFlipState = false;
        SampleModePersistentState flipState;
        juce::String embeddedFlipSampleBase64;
        PreparedSceneLoopPitchState loopPitchState;
    };
    struct PreparedSceneGroupState
    {
        float volume = 1.0f;
        bool muted = false;
    };
    struct PreparedScenePatternState
    {
        bool present = false;
        bool playing = false;
        int lengthBeats = 4;
        std::vector<PatternRecorder::Event> events;
    };
    struct PreparedSceneSwitchPayload
    {
        int mainPresetIndex = 0;
        int sceneSlot = 0;
        bool sequenceDriven = false;
        int sequenceStepIndex = -1;
        uint64_t switchSerial = 0;
        std::array<PreparedSceneStripAudioPayload, MaxStrips> stripAudioPayloads;
        std::array<PreparedSceneStripState, MaxStrips> stripStates;
        std::array<PreparedSceneGroupState, ModernAudioEngine::MaxGroups> groupStates{};
        std::array<PreparedScenePatternState, ModernAudioEngine::MaxPatterns> patternStates;
        PreparedSceneTimingState sceneTimingState;
        juce::MemoryBlock scenePerformanceStateData;
        juce::ValueTree parameterState;
        std::unique_ptr<juce::XmlElement> snapshotPresetXml;
    };
private:
    struct SceneTransitionEndSamplePreviewRequest
    {
        std::shared_ptr<const SceneTransitionEndSampleData> sample;
        SceneTransitionEndSampleSettings settings;
    };

    void configureAudioEngineCallbacks(ModernAudioEngine& engine);
    bool loadSampleFileIntoSampleModeEngine(SampleModeEngine& engine, const juce::File& file) const;
    bool readAudioFileToStereoBuffer(const juce::File& file,
                                     juce::AudioBuffer<float>& buffer,
                                     double& sourceRate) const;
    std::shared_ptr<const SceneTransitionEndSampleData> loadSceneTransitionEndSampleData(const juce::File& file) const;
    void reloadSceneChainTransitionEndSample(int stepIndex);
    void reloadAllSceneChainTransitionEndSamples();
    bool decodeEmbeddedSceneAudioToStereoBuffer(const juce::String& base64Audio,
                                                juce::AudioBuffer<float>& buffer,
                                                double& sourceRate) const;
    bool buildPreparedSceneStripAnalysisPayload(int stripIndex,
                                                PreparedSceneStripAudioPayload& payload) const;
    bool tryCloneLiveStripAudioPayload(int stripIndex,
                                       const juce::File& file,
                                       PreparedSceneStripAudioPayload& payload) const;
    bool tryLoadPreparedSceneStripAudioPayloadFromCache(const juce::File& file,
                                                        PreparedSceneStripAudioPayload& payload);
    bool tryBuildPreparedSceneStripAudioPayload(int stripIndex,
                                                const juce::XmlElement& stripXml,
                                                PreparedSceneStripAudioPayload& payload);
    bool tryBuildPreparedSceneStripAudioPayload(int stripIndex,
                                                const PreparedSceneStripState& stripState,
                                                PreparedSceneStripAudioPayload& payload);
    PreparedSceneLoopPitchState capturePreparedSceneLoopPitchState(int stripIndex) const;
    void applyPreparedSceneLoopPitchState(int stripIndex, const PreparedSceneLoopPitchState& state);
    PreparedSceneTimingState capturePreparedSceneTimingState(int sceneSlot) const;
    void applyPreparedSceneTimingState(int sceneSlot, const PreparedSceneTimingState& state);
    bool getStoredSceneControlValue(int sceneSlot,
                                    int stripIndex,
                                    ScenePerformanceControlTarget target,
                                    float& valueOut) const;
    bool capturePreparedSceneSwitchPayloadTemplate(PreparedSceneSwitchPayload& payload,
                                                   int mainPresetIndex,
                                                   int sceneSlot);
    bool parsePreparedSceneSwitchPayloadTemplate(PreparedSceneSwitchPayload& payload,
                                                 const juce::XmlElement& presetXml,
                                                 int mainPresetIndex,
                                                 int sceneSlot) const;
    std::unique_ptr<PreparedSceneSwitchPayload> clonePreparedSceneSwitchPayloadTemplate(
        const PreparedSceneSwitchPayload& source) const;
    std::unique_ptr<juce::XmlElement> createSceneSnapshotPresetXml(
        const PreparedSceneSwitchPayload& payload,
        const juce::String& sceneName) const;
    bool buildPreparedSceneSwitchPayload(PreparedSceneSwitchPayload& payload,
                                         int mainPresetIndex,
                                         int sceneSlot,
                                         bool sequenceDriven,
                                         int sequenceStepIndex);
    void cacheSceneAudioFilePayload(const juce::File& file,
                                    const juce::AudioBuffer<float>& sourceBuffer,
                                    double sourceRate,
                                    const EnhancedAudioStrip* analysisSourceStrip);
    void cachePreparedSceneAudioFilePayload(const juce::File& file,
                                            const PreparedSceneStripAudioPayload& payload);
    static void applySceneMotionStateToTargetEngine(const ScenePerformanceRecorder& recorder,
                                                    int sceneSlot,
                                                    ModernAudioEngine& engine);
    void requestScenePreload(int mainPresetIndex,
                             int sceneSlot,
                             bool sequenceDriven,
                             int sequenceStepIndex,
                             double targetPpq,
                             double targetTempo,
                             int64_t targetSample,
                             SceneChainTransitionType transitionType,
                             uint64_t switchSerial = 0);
    void servicePendingScenePreloadRequest();
    bool preparedSceneSwitchPayloadMatchesTarget(const PreparedSceneSwitchPayload& payload,
                                                 int mainPresetIndex,
                                                 int sceneSlot,
                                                 bool sequenceDriven,
                                                 int sequenceStepIndex) const noexcept;
    bool preparedSceneSwitchPayloadMatchesSwitchEvent(const PreparedSceneSwitchPayload& payload,
                                                      const SceneSwitchEvent& event) const noexcept;
    bool hasPreparedSceneSwitchPayloadForEvent(const SceneSwitchEvent& event) const;
    PreparedSceneSwitchPayload* takePreparedSceneSwitchPayloadForEvent(const SceneSwitchEvent& event);
    void retirePreparedSceneSwitchPayload(PreparedSceneSwitchPayload* payload) noexcept;
    void reclaimRetiredPreparedSceneSwitchPayloads();
    bool loadPreparedSceneStripAudioToActiveEngine(const PreparedSceneSwitchPayload& payload,
                                                   int stripIndex);
    bool applyPreparedSceneSwitchPayload(const PreparedSceneSwitchPayload& payload,
                                         const SceneSwitchEvent& event,
                                         bool& recallContinuityBroken);
    void renderActiveSceneAudio(juce::AudioBuffer<float>& buffer,
                                juce::MidiBuffer& midiMessages,
                                const juce::AudioPlayHead::PositionInfo& posInfo,
                                bool allowSeparateStripRouting,
                                bool applySceneTransitionOverlay);
    void renderActiveSceneAudioRange(juce::AudioBuffer<float>& destination,
                                     juce::MidiBuffer& midiMessages,
                                     const juce::AudioPlayHead::PositionInfo& posInfo,
                                     int startOffset);
    void requestAbortActiveSceneTransition();
    bool renderPendingPreparedSceneSwitch(juce::AudioBuffer<float>& buffer,
                                          juce::MidiBuffer& midiMessages,
                                          const juce::AudioPlayHead::PositionInfo& posInfo,
                                          int64_t blockStartSample);
    ScenePlaybackOwner getRenderedScenePlaybackOwner() const noexcept;
    bool hasAnyLiveScenePlayback() const noexcept;
    bool sceneSwitchHasIncomingPlayback(const SceneSwitchEvent& event) const noexcept;
    void queuePendingSceneParameterState(const juce::ValueTree& state);
    void applyPendingSceneParameterState();
    void clearSceneStripLaunchHandles() noexcept;
    SceneStripPlaybackHandle captureSceneStripPlaybackHandle(int stripIndex);
    void refreshSceneStripLaunchHandlesFromEngine();
    void clearActiveScenePlaybackHandle();
    void setActiveScenePlaybackHandle(int mainPresetIndex,
                                      int sceneSlot,
                                      bool sequenceDriven,
                                      int sequenceStepIndex,
                                      double startPpq,
                                      double resolvedLengthBeats);
    void switchScenePlaybackOwner(ScenePlaybackOwner owner,
                                  bool chainActive,
                                  int chainStepIndex = -1);
    void setSceneChainAttachStartPpq(double startPpq);
    void setScenePlaybackOwner(ScenePlaybackOwner owner);
    SceneSwitchSplitStatus buildSceneSwitchSplitStatus(const SceneSwitchEvent& event) const noexcept;
    int resolveSceneSwitchTargetOffsetForCurrentBlock(const SceneSwitchEvent& event,
                                                      int64_t blockStartSample,
                                                      int blockNumSamples) const noexcept;
    void clearPendingSceneApplyState();
    uint64_t queuePendingSceneApplyState(const SceneSwitchEvent& event);
    bool peekPendingSceneApplyState(SceneSwitchEvent& event) const;
    bool consumePendingSceneApplyState(SceneSwitchEvent& event);
    void startManualScenePlayback(int sceneSlot, bool useTriggerQuantization, bool focusSceneSlotFirst);
    bool hasStoredSceneSlotState(int mainPresetIndex, int sceneSlot) const;
    bool hasAnyStoredSceneSlotState(int mainPresetIndex) const;
    bool hasPersistableStoredSceneSlotState(int mainPresetIndex, int sceneSlot) const;
    bool hasAnyPersistableStoredSceneSlotState(int mainPresetIndex) const;
    const SceneSlotState* getStoredSceneSlotState(int mainPresetIndex, int sceneSlot) const;
    void clearStoredSceneSlotStates(int mainPresetIndex = -1);
    bool loadStoredSceneSlotStatesForPreset(int mainPresetIndex, const juce::XmlElement& presetXml);
    bool restoreStoredSceneSlotStatesFromPresetXml(int mainPresetIndex, const juce::XmlElement& presetXml);
    bool migrateLegacyStoredSceneSlotStates(int mainPresetIndex);
    bool captureSceneSlotState(int mainPresetIndex,
                               int sceneSlot,
                               bool implicitMainPresetFallback = false);
    bool ensureSceneSlotFallbackState(int mainPresetIndex, int sceneSlot);
    bool copyStoredSceneSlotState(int mainPresetIndex, int sourceSceneSlot, int destSceneSlot);
    bool deleteStoredSceneSlotState(int mainPresetIndex, int sceneSlot);
    bool persistStoredSceneSlotStatesToMainPreset(int mainPresetIndex);
    bool refreshStoredSceneSlotSnapshot(int mainPresetIndex, int sceneSlot);
    struct SceneStripControlRuntimeState
    {
        float volume = 1.0f;
        float pan = 0.0f;
        float speed = 1.0f;
    };
    struct SceneClipSlotRuntimeState
    {
        int mainPresetIndex = 0;
        bool hasLiveStripControls = false;
        bool liveStripControlsDirty = false;
        std::array<SceneStripControlRuntimeState, MaxStrips> stripControls{};
    };
    void syncActiveSceneClipSlotRuntimeStateFromEngine(bool markDirty);
    void applySceneClipSlotRuntimeState(int mainPresetIndex, int sceneSlot);
    void clearSceneClipSlotRuntimeState(int mainPresetIndex, int sceneSlot);
    void clearAllSceneClipSlotRuntimeStates();
    void copySceneClipSlotRuntimeState(int mainPresetIndex, int sourceSceneSlot, int destSceneSlot);
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
    void resetRuntimePresetStateToDefaults(bool preserveLoadedStripAudio = false);
public:
    void processPendingSceneAutosave();
    bool flushPendingActiveSceneAutosaveIfCurrent();
    void clearPendingActiveSceneAutosave();
    bool canPersistActiveSceneSnapshotSafely() const;
    void beginSceneAutosaveSuppression();
    void endSceneAutosaveSuppression();
    bool isSceneAutosaveSuppressed() const;
    int getActiveSceneBoundaryTransitionStep() const
    {
        return sceneBoundaryTransitionFromStep.load(std::memory_order_acquire);
    }
private:

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
    int momentaryStutterRecordedDivisionButton = -1;
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
    enum class MonomePatternTapAction
    {
        None = 0,
        StartFreshRecording,
        StopPlayback,
        StopRecording
    };
    enum class MonomeSceneRecorderTapAction
    {
        None = 0,
        StartFreshRecording,
        StopRecording
    };
    std::array<uint32_t, ModernAudioEngine::MaxPatterns> monomePatternPadPendingUntilMs{};
    std::array<MonomePatternTapAction, ModernAudioEngine::MaxPatterns> monomePatternPadPendingAction{};
    uint32_t monomeSceneRecorderPendingUntilMs = 0;
    MonomeSceneRecorderTapAction monomeSceneRecorderPendingAction = MonomeSceneRecorderTapAction::None;
    bool monomeSceneRecorderHeld = false;
    bool monomeSceneRecorderHoldClearTriggered = false;
    uint32_t monomeSceneRecorderPressStartMs = 0;
    uint32_t monomeSceneRecorderClearBurstUntilMs = 0;
    struct PendingSceneRecorderAction
    {
        bool active = false;
        bool targetResolved = false;
        bool patternEndPhaseSignatureValid = false;
        SceneRecorderAction action = SceneRecorderAction::None;
        int sceneSlot = -1;
        double targetPpq = 0.0;
        double intervalBeats = 4.0;
        uint64_t patternEndPhaseSignature = 0;
    };
    PendingSceneRecorderAction pendingSceneRecorderAction;
    struct PendingSceneTriggerRecord
    {
        bool active = false;
        int sceneSlot = -1;
        int column = -1;
        double eventBeat = std::numeric_limits<double>::quiet_NaN();
    };
    std::array<PendingSceneTriggerRecord, MaxStrips> pendingSceneTriggerRecords{};
    ScenePerformanceRecorder scenePerformanceRecorder;
    double lastScenePerformanceProcessBeat = std::numeric_limits<double>::quiet_NaN();
    int lastScenePerformanceProcessSceneSlot = -1;
    double lastScenePerformanceProcessSceneStartBeat = std::numeric_limits<double>::quiet_NaN();
    std::atomic<int> sceneModeEnabled{0};
    std::atomic<float>* sceneModeParam = nullptr;
    int activeSceneMainPresetIndex = 0;
    int activeSceneSlot = 0;
    bool activeSceneNeedsCaptureBeforeManualRecall = true;
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
        bool useTriggerQuantization = false;
        bool targetResolved = false;
        bool patternEndPhaseSignatureValid = false;
        int mainPresetIndex = 0;
        int sceneSlot = 0;
        int sequenceStepIndex = -1;
        double targetPpq = 0.0;
        double intervalBeats = 4.0;
        uint64_t patternEndPhaseSignature = 0;
    };
    PendingSceneRecall pendingSceneRecall;
    std::array<int, SceneSlots> sceneRepeatCounts{};
    std::array<int, SceneSlots> sceneLengthModes{};
    std::array<int, SceneSlots> sceneManualBars{};
    std::array<int, SceneSlots> sceneAnchorStrips{};
    SceneChainState sceneChainState;
    std::array<std::shared_ptr<const SceneTransitionEndSampleData>, MaxSceneChainSteps> sceneTransitionEndSamples{};
    std::array<SceneTransitionEndSampleVoice, MaxSceneChainSteps> sceneTransitionEndSampleVoices{};
    std::array<SceneChainTransitionFavorite, SceneTransitionFavoriteSlots> sceneTransitionFavorites{};
    bool sceneEditorGridEnabledState = true;
    int sceneEditorGridDivisionState = 16;
    bool sceneEditorDrawModeEnabledState = false;
    bool sceneEditorLaneOverlaysEnabledState = true;
    int sceneEditorZoomFactorState = 1;
    bool sceneEditorFollowPlayheadState = false;
    SceneModPageMode sceneModPageModeState = SceneModPageMode::StepMotion;
    std::atomic<int> sceneStepMotionEditorOpenState{0};
    std::array<ModernAudioEngine::ModTarget, MaxStrips> sceneMainAutomationDisplayTargets{};
    std::array<bool, MaxStrips> sceneEditorStripAutomationExpanded{};
    std::array<bool, MaxStrips> sceneEditorStripHeightExpanded{};
    std::array<bool, SceneSlots> scenePadHeld{};
    std::array<bool, SceneSlots> scenePadHoldDeleteTriggered{};
    std::array<bool, SceneSlots> scenePadLaunchConsumed{};
    std::array<uint32_t, SceneSlots> scenePadPressStartMs{};
    std::array<uint32_t, SceneSlots> scenePadActionBurstUntilMs{};
    std::array<uint32_t, SceneSlots> scenePadLastTapMs{};
    int sceneSlotClipboardSourceSlot = -1;
    int sceneSlotClipboardMainPresetIndex = 0;
    int sceneCopySourceSlot = -1;
    int sceneCopyMainPresetIndex = 0;
    bool monomeLedUpdateInProgress = false;
    bool monomeLedUpdatePending = false;
    juce::MemoryBlock scenePerformanceClipboardData;
    bool sceneSequenceActive = false;
    int sceneSequenceCurrentStepIndex = -1;
    ScenePlaybackHandle activeScenePlaybackHandle;
    std::array<SceneStripLaunchHandle, MaxStrips> sceneStripLaunchHandles{};
    std::atomic<uint64_t> sceneStripLaunchRevisionCounter{1};
    int focusedSceneSlot = 0;
    ScenePlaybackOwner scenePlaybackOwner = ScenePlaybackOwner::Manual;
    std::array<SceneClipSlotRuntimeState, SceneSlots> sceneClipSlotRuntimeStates{};
    std::array<SceneSlotState, SceneSlots> storedSceneSlotStates{};
    int storedSceneSlotStateMainPresetIndex = -1;
    std::array<std::array<std::atomic<uint64_t>, MaxStrips + 1>, SceneSlots> sceneClipAutomationTargetMasks{};
    std::array<std::atomic<uint64_t>, MaxStrips + 1> activeSceneAutomationOverrideMasks{};
    bool sceneChainAttachStartPpqValid = false;
    double sceneChainAttachStartPpq = 0.0;
    bool activeSceneStartPpqValid = false;
    double activeSceneStartPpq = 0.0;
    bool sceneSequenceStartPpqValid = false;
    double sceneSequenceStartPpq = 0.0;
    std::atomic<int> pendingSceneApplyMainPreset{-1};
    std::atomic<int> pendingSceneApplySlot{-1};
    std::atomic<int> pendingSceneApplySequenceDriven{0};
    std::atomic<int> pendingSceneApplySequenceStep{-1};
    std::atomic<double> pendingSceneApplyTargetPpq{-1.0};
    std::atomic<double> pendingSceneApplyTargetTempo{120.0};
    std::atomic<int64_t> pendingSceneApplyTargetSample{-1};
    std::atomic<int64_t> pendingSceneApplyBlockStartSample{-1};
    std::atomic<int> pendingSceneApplyBlockNumSamples{0};
    std::atomic<int> pendingSceneApplyTargetSampleOffset{-1};
    std::atomic<int> pendingSceneApplyOutgoingOwner{static_cast<int>(ScenePlaybackOwner::Manual)};
    std::atomic<int> pendingSceneApplyOwnerOnlySwitch{0};
    std::atomic<int> pendingSceneApplyLegatoOwnerSwitch{0};
    std::atomic<uint64_t> pendingSceneApplyPublishedSerial{0};
    std::atomic<uint64_t> pendingSceneApplyConsumedSerial{0};
    std::atomic<uint64_t> pendingSceneApplyNextSerial{1};
    std::atomic<int> pendingSceneRecorderApplyAction{static_cast<int>(SceneRecorderAction::None)};
    std::atomic<int> pendingSceneRecorderApplySceneSlot{-1};
    std::atomic<double> pendingSceneRecorderApplyTargetPpq{-1.0};
    std::atomic<int64_t> pendingSceneRecorderApplyTargetSample{-1};
    std::atomic<int> pendingScenePreloadDirty{0};
    std::atomic<int> pendingScenePreloadMainPreset{-1};
    std::atomic<int> pendingScenePreloadSceneSlot{-1};
    std::atomic<int> pendingScenePreloadSequenceDriven{0};
    std::atomic<int> pendingScenePreloadSequenceStep{-1};
    std::atomic<double> pendingScenePreloadTargetPpq{-1.0};
    std::atomic<double> pendingScenePreloadTargetTempo{120.0};
    std::atomic<int64_t> pendingScenePreloadTargetSample{-1};
    std::atomic<int> pendingScenePreloadTransitionType{static_cast<int>(SceneChainTransitionType::None)};
    std::atomic<uint64_t> pendingScenePreloadSwitchSerial{0};
    std::atomic<PreparedSceneSwitchPayload*> preparedSceneSwitchPayloadPublished{nullptr};
    std::atomic<int> preparedSceneSwitchPayloadMainPreset{-1};
    std::atomic<int> preparedSceneSwitchPayloadSceneSlot{-1};
    std::atomic<int> preparedSceneSwitchPayloadSequenceDriven{0};
    std::atomic<int> preparedSceneSwitchPayloadSequenceStep{-1};
    std::atomic<uint64_t> preparedSceneSwitchPayloadSwitchSerial{0};
    static constexpr size_t MaxRetiredPreparedSceneSwitchPayloads = 8;
    std::array<std::atomic<PreparedSceneSwitchPayload*>, MaxRetiredPreparedSceneSwitchPayloads>
        retiredPreparedSceneSwitchPayloads{};
    juce::CriticalSection sceneFileAudioCacheLock;
    std::vector<SceneFileAudioCacheEntry> sceneFileAudioCache;
    uint64_t sceneFileAudioCacheUseCounter = 0;
    std::atomic<int> suppressOwnedStripParameterSync{0};
    std::atomic<juce::ValueTree*> pendingSceneParameterStatePtr{nullptr};
    static constexpr size_t MaxRetiredPendingSceneParameterStates = 4;
    std::array<std::atomic<juce::ValueTree*>, MaxRetiredPendingSceneParameterStates>
        retiredPendingSceneParameterStates{};
    std::atomic<int> suppressSceneManualControlHandlingDepth{0};
    static constexpr int kSceneRecallBlendMaxChannels = 32;
    std::atomic<float> sceneGlobalStutterBaseAmount{0.0f};
    std::atomic<float> sceneTransitionStutterOverlayAmount{0.0f};
    bool scenePlaybackBlockStutterPostRenderPending = false;
    float scenePlaybackBlockStutterPostRenderAmount = 0.0f;
    std::atomic<int> sceneBoundaryTransitionType{static_cast<int>(SceneChainTransitionType::None)};
    std::atomic<int> sceneBoundaryTransitionOption{static_cast<int>(SceneChainTransitionOption::Default)};
    std::atomic<int> sceneBoundaryTransitionScope{static_cast<int>(SceneChainTransitionScope::All)};
    std::atomic<int> sceneBoundaryTransitionContour{static_cast<int>(SceneChainTransitionContour::Smooth)};
    std::atomic<int> sceneBoundaryTransitionFromStep{-1};
    std::atomic<int> sceneBoundaryTransitionToStep{-1};
    std::atomic<double> sceneBoundaryTransitionStartPpq{-1.0};
    std::atomic<double> sceneBoundaryTransitionTargetPpq{-1.0};
    std::atomic<int64_t> sceneBoundaryTransitionStartSample{-1};
    std::atomic<int64_t> sceneBoundaryTransitionTargetSample{-1};
    std::atomic<double> sceneBoundaryTransitionTempo{120.0};
    std::atomic<float> sceneBoundaryTransitionLengthBeats{DefaultSceneTransitionLengthBeats};
    std::atomic<float> sceneBoundaryTransitionIntensity{DefaultSceneTransitionIntensity};
    std::atomic<float> sceneBoundaryTransitionDelayAmount{DefaultSceneTransitionDelayAmount};
    std::atomic<float> sceneBoundaryTransitionFilterAmount{DefaultSceneTransitionFilterAmount};
    std::atomic<float> sceneBoundaryTransitionChopAmount{DefaultSceneTransitionChopAmount};
    std::atomic<int> sceneBoundaryTransitionEndSampleFromStep{-1};
    std::atomic<double> sceneBoundaryTransitionEndSampleTargetPpq{-1.0};
    std::atomic<int64_t> sceneBoundaryTransitionEndSampleTargetSample{-1};
    std::atomic<float> sceneBoundaryTransitionEndSampleIntensity{DefaultSceneTransitionIntensity};
    std::atomic<float> sceneBoundaryTransitionEndSampleGainDb{DefaultSceneTransitionEndSampleGainDb};
    std::atomic<float> sceneBoundaryTransitionEndSampleFadeInMs{DefaultSceneTransitionEndSampleFadeInMs};
    std::atomic<float> sceneBoundaryTransitionEndSampleFadeOutMs{DefaultSceneTransitionEndSampleFadeOutMs};
    std::atomic<int> sceneBoundaryTransitionEndSampleChokePrevious{0};
    std::atomic<int> sceneBoundaryTransitionEndSampleReverse{0};
    std::atomic<float> sceneBoundaryTransitionEndSamplePitchSemitones{DefaultSceneTransitionEndSamplePitchSemitones};
    std::atomic<float> sceneBoundaryTransitionEndSampleLowpassHz{DefaultSceneTransitionEndSampleLowpassHz};
    std::atomic<float> sceneBoundaryTransitionEndSampleHighpassHz{DefaultSceneTransitionEndSampleHighpassHz};
    std::atomic<int> sceneBoundaryTransitionEndSampleDuckSource{DefaultSceneTransitionEndSampleDuckSource};
    std::atomic<float> sceneBoundaryTransitionEndSampleDuckAmount{DefaultSceneTransitionEndSampleDuckAmount};
    std::atomic<int> sceneBoundaryTransitionEndSampleTriggered{0};
    std::shared_ptr<const SceneTransitionEndSamplePreviewRequest> sceneTransitionEndSamplePreviewRequest;
    SceneTransitionEndSampleVoice sceneTransitionEndSamplePreviewVoice{};
    juce::AudioBuffer<float> preparedSceneSwitchOutgoingBuffer;
    juce::AudioBuffer<float> preparedSceneSwitchIncomingBuffer;
    std::atomic<int> sceneChainReturnOverrideActive{0};
    std::atomic<int> sceneChainReturnOverrideSourceStep{-1};
    std::atomic<int> sceneChainReturnOverrideTriggerStep{-1};
    static constexpr uint32_t presetHoldSaveMs = 3000;
    static constexpr uint32_t presetDoubleTapMs = 350;
    static constexpr uint32_t presetSaveBurstDurationMs = 260;
    static constexpr uint32_t presetSaveBurstIntervalMs = 55;
    static constexpr uint32_t monomePatternDoubleTapMs = 260;
    static constexpr uint32_t sceneHoldDeleteMs = 3000;
    static constexpr uint32_t sceneDoubleTapMs = 350;
    static constexpr uint32_t sceneActionBurstDurationMs = 260;
    static constexpr uint32_t sceneActionBurstIntervalMs = 55;
    std::atomic<int> pendingPresetLoadIndex{-1};
    juce::ThreadPool presetSaveThreadPool{1};
    juce::CriticalSection presetSaveResultLock;
    std::vector<PresetSaveResult> presetSaveResults;
    std::atomic<int> presetSaveJobsInFlight{0};
    
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MlrVSTAudioProcessor)
    JUCE_DECLARE_WEAK_REFERENCEABLE(MlrVSTAudioProcessor)
};
