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
#include "AudioEngine.h"

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
    bool isConnected() const { return connected; }
    
    // SerialOSC device discovery
    void discoverDevices();
    void selectDevice(int deviceIndex);
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
    DeviceInfo getCurrentDevice() const { return currentDevice; }
    juce::String getConnectionStatus() const;
    
    // Callbacks
    std::function<void(int x, int y, int state)> onKeyPress;
    std::function<void(int sensor, int x, int y, int z)> onTilt;
    std::function<void(int encoder, int delta)> onArcDelta;
    std::function<void(int encoder, int state)> onArcKey;
    std::function<void()> onDeviceConnected;
    std::function<void()> onDeviceDisconnected;
    std::function<void(const std::vector<DeviceInfo>&)> onDeviceListUpdated;
    
private:
    void oscMessageReceived(const juce::OSCMessage& message) override;
    void handleSerialOSCMessage(const juce::OSCMessage& message);
    void handleGridMessage(const juce::OSCMessage& message);
    void handleSystemMessage(const juce::OSCMessage& message);
    void handleTiltMessage(const juce::OSCMessage& message);
    void handleArcMessage(const juce::OSCMessage& message);
    
    void timerCallback() override;
    void attemptReconnection();
    void sendPing();
    void markDisconnected();
    void configureCurrentDevice();
    
    juce::OSCSender oscSender;
    juce::OSCSender serialoscSender; // Separate sender for serialosc queries
    juce::OSCReceiver oscReceiver;
    
    std::vector<DeviceInfo> devices;
    DeviceInfo currentDevice;
    
    juce::String oscPrefix = "/monome";
    int applicationPort = 8000;
    bool connected = false;
    bool autoReconnect = true;
    
    int reconnectAttempts = 0;
    static constexpr int maxReconnectAttempts = 10;
    static constexpr int reconnectIntervalMs = 2000;
    static constexpr int discoveryIntervalMs = 2000;
    
    juce::int64 lastMessageTime = 0;
    juce::int64 lastConnectAttemptTime = 0;
    juce::int64 lastPingTime = 0;
    juce::int64 lastDiscoveryTime = 0;
    juce::int64 lastReconnectAttemptTime = 0;
    bool awaitingDeviceResponse = false;
    static constexpr int pingIntervalMs = 5000;
    static constexpr int handshakeTimeoutMs = 3000;
    static constexpr int connectionTimeoutMs = 15000;
    
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MonomeConnection)
};

//==============================================================================
/**
 * MlrVSTAudioProcessor - Main plugin processor
 */
class MlrVSTAudioProcessor : public juce::AudioProcessor,
                             public juce::Timer
{
public:
    MlrVSTAudioProcessor();
    ~MlrVSTAudioProcessor() override;

    //==============================================================================
    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;

    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

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

    //==============================================================================
    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    //==============================================================================
    // Public API
    ModernAudioEngine* getAudioEngine() { return audioEngine.get(); }
    MonomeConnection& getMonomeConnection() { return monomeConnection; }
    
    bool loadSampleToStrip(int stripIndex, const juce::File& file);
    void loadAdjacentFile(int stripIndex, int direction);  // Browse files
    void captureRecentAudioToStrip(int stripIndex);
    void clearRecentAudioBuffer();
    void requestBarLengthChange(int stripIndex, int bars);
    void setPendingBarLengthApply(int stripIndex, bool pending);
    void triggerStrip(int stripIndex, int column);
    void stopStrip(int stripIndex);
    
    enum class SamplePathMode
    {
        Loop,
        Step
    };
    static constexpr int BrowserFavoriteSlots = 6;
    juce::File getDefaultSampleDirectory(int stripIndex, SamplePathMode mode) const;
    void setDefaultSampleDirectory(int stripIndex, SamplePathMode mode, const juce::File& directory);
    juce::File getCurrentBrowserDirectoryForStrip(int stripIndex) const;
    juce::File getBrowserFavoriteDirectory(int slot) const;
    bool isBrowserFavoritePadHeld(int stripIndex, int slot) const;
    bool isBrowserFavoriteSaveBurstActive(int slot, uint32_t nowMs) const;
    bool isBrowserFavoriteMissingBurstActive(int slot, uint32_t nowMs) const;
    void beginBrowserFavoritePadHold(int stripIndex, int slot);
    void endBrowserFavoritePadHold(int stripIndex, int slot);
    
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
    
    // Preset management
    void savePreset(int presetIndex);
    void loadPreset(int presetIndex);
    bool deletePreset(int presetIndex);
    int getLoadedPresetIndex() const { return loadedPresetIndex; }
    juce::String getPresetName(int presetIndex) const;
    bool setPresetName(int presetIndex, const juce::String& name);
    bool presetExists(int presetIndex) const;
    uint32_t getPresetRefreshToken() const { return presetRefreshToken.load(std::memory_order_acquire); }
    static constexpr int PresetColumns = 16;
    static constexpr int PresetRows = 7;
    static constexpr int MaxPresetSlots = PresetColumns * PresetRows;
    
    // Parameters
    juce::AudioProcessorValueTreeState parameters;
    
    static constexpr int MaxStrips = ModernAudioEngine::MaxStrips;
    static constexpr int MaxColumns = 16;

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
    std::atomic<float>* quantizeParam = nullptr;
    std::atomic<float>* innerLoopLengthParam = nullptr;
    std::atomic<float>* grainQualityParam = nullptr;
    std::atomic<float>* pitchSmoothingParam = nullptr;
    std::atomic<float>* inputMonitorParam = nullptr;
    std::atomic<float>* crossfadeLengthParam = nullptr;
    std::atomic<float>* triggerFadeInParam = nullptr;
    std::atomic<float>* outputRoutingParam = nullptr;
    std::array<std::atomic<float>*, MaxStrips> stripVolumeParams{};
    std::array<std::atomic<float>*, MaxStrips> stripPanParams{};
    std::array<std::atomic<float>*, MaxStrips> stripSpeedParams{};
    std::array<std::atomic<float>*, MaxStrips> stripPitchParams{};
    juce::CriticalSection pendingLoopChangeLock;
    std::array<PendingLoopChange, MaxStrips> pendingLoopChanges{};
    juce::CriticalSection pendingBarChangeLock;
    std::array<PendingBarChange, MaxStrips> pendingBarChanges{};
    std::array<bool, MaxStrips> pendingBarLengthApply{};
    
    double currentSampleRate = 44100.0;
    ControlMode currentControlMode = ControlMode::Normal;
    bool controlModeActive = false;  // True when control button is held
    FilterSubPage filterSubPage = FilterSubPage::Frequency;  // Current filter sub-page
    bool quantizeEnabled = true;
    std::atomic<int> lastMonomePressedStripRow{0};
    mutable juce::CriticalSection controlPageOrderLock;
    ControlPageOrder controlPageOrder {
        ControlMode::Speed,
        ControlMode::Pan,
        ControlMode::Volume,
        ControlMode::GrainSize,
        ControlMode::Swing,
        ControlMode::Gate,
        ControlMode::FileBrowser,
        ControlMode::GroupAssign,
        ControlMode::Filter,
        ControlMode::Pitch,
        ControlMode::Modulation,
        ControlMode::Preset,
        ControlMode::StepEdit
    };
    StepEditTool stepEditTool = StepEditTool::Gate;
    int stepEditSelectedStrip = 0;
    std::atomic<bool> controlPageMomentary{true};
    std::atomic<int> swingDivisionSelection{1}; // 0=1/4,1=1/8,2=1/16,3=Triplet
    
    // LED state cache to prevent flickering
    int ledCache[16][8] = {{0}};
    
    // Last loaded folder for file browsing
    juce::File lastSampleFolder;
    std::array<juce::File, MaxStrips> defaultLoopDirectories;
    std::array<juce::File, MaxStrips> defaultStepDirectories;
    std::array<juce::File, BrowserFavoriteSlots> browserFavoriteDirectories;
    std::array<std::array<bool, BrowserFavoriteSlots>, MaxStrips> browserFavoritePadHeld{};
    std::array<std::array<bool, BrowserFavoriteSlots>, MaxStrips> browserFavoritePadHoldSaveTriggered{};
    std::array<std::array<uint32_t, BrowserFavoriteSlots>, MaxStrips> browserFavoritePadPressStartMs{};
    std::array<uint32_t, BrowserFavoriteSlots> browserFavoriteSaveBurstUntilMs{};
    std::array<uint32_t, BrowserFavoriteSlots> browserFavoriteMissingBurstUntilMs{};
    std::array<int, 4> arcKeyHeld{};
    std::array<std::array<int, 64>, 4> arcRingCache{};
    ArcControlMode arcControlMode = ArcControlMode::SelectedStrip;
    int arcSelectedModStep = 0;
    juce::int64 lastGridLedUpdateTimeMs = 0;
    static constexpr int kGridRefreshMs = 100;
    static constexpr int kArcRefreshMs = 33;
    static constexpr uint32_t browserFavoriteHoldSaveMs = 3000;
    static constexpr uint32_t browserFavoriteSaveBurstDurationMs = 320;
    static constexpr uint32_t browserFavoriteMissingBurstDurationMs = 260;
    
    // Current file per strip for proper next/prev browsing
    juce::File currentStripFiles[8];
    
    // LED update
    void updateMonomeLEDs();
    void updateMonomeArcRings();
    void timerCallback() override;
    
    void handleMonomeKeyPress(int x, int y, int state);
    bool isArcModulationMode() const;
    void setArcControlMode(ArcControlMode mode);
    void handleMonomeArcDelta(int encoder, int delta);
    void handleMonomeArcKey(int encoder, int state);
    void setMomentaryScratchHold(bool shouldEnable);
    void setMomentaryStutterHold(bool shouldEnable);
    
    juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    void cacheParameterPointers();
    SamplePathMode getSamplePathModeForStrip(int stripIndex) const;
    bool saveBrowserFavoriteDirectoryFromStrip(int stripIndex, int slot);
    bool recallBrowserFavoriteDirectoryForStrip(int stripIndex, int slot);
    bool isAudioFileSupported(const juce::File& file) const;
    void loadDefaultPathsFromState(const juce::ValueTree& state);
    void appendDefaultPathsToState(juce::ValueTree& state) const;
    void loadControlPagesFromState(const juce::ValueTree& state);
    void appendControlPagesToState(juce::ValueTree& state) const;
    void loadPersistentDefaultPaths();
    void savePersistentDefaultPaths() const;
    void loadPersistentControlPages();
    void savePersistentControlPages() const;
    int getQuantizeDivision() const;
    float getInnerLoopLengthFactor() const;
    void queueLoopChange(int stripIndex, bool clearLoop, int startColumn, int endColumn, bool reverseDirection, int markerColumn = -1);
    void applyPendingLoopChanges(const juce::AudioPlayHead::PositionInfo& posInfo);
    void applyPendingBarChanges(const juce::AudioPlayHead::PositionInfo& posInfo);
    void applyPendingStutterStart(const juce::AudioPlayHead::PositionInfo& posInfo);
    void applyPendingStutterRelease(const juce::AudioPlayHead::PositionInfo& posInfo);
    void performMomentaryStutterStartNow(double hostPpqNow, int64_t nowSample);
    void performMomentaryStutterReleaseNow(double hostPpqNow, int64_t nowSample);
    void captureMomentaryStutterMacroBaseline();
    void applyMomentaryStutterMacro(const juce::AudioPlayHead::PositionInfo& posInfo);
    void restoreMomentaryStutterMacroBaseline();
    bool getHostSyncSnapshot(double& outPpq, double& outTempo) const;
    void performPresetLoad(int presetIndex, double hostPpqSnapshot, double hostTempoSnapshot);
    struct PresetSaveRequest
    {
        int presetIndex = -1;
        std::array<juce::File, MaxStrips> stripFiles;
    };
    struct PresetSaveResult
    {
        int presetIndex = -1;
        bool success = false;
    };
    class PresetSaveJob;
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
    double momentaryStutterDivisionBeats = 1.0; // 1.0=1/4 ... 0.0625=1/64
    int momentaryStutterActiveDivisionButton = -1;
    std::atomic<uint8_t> momentaryStutterButtonMask{0};
    std::array<bool, MaxStrips> momentaryStutterStripArmed{};
    struct MomentaryStutterSavedStripState
    {
        bool valid = false;
        float pan = 0.0f;
        float playbackSpeed = 1.0f;
        float pitchShift = 0.0f;
        bool filterEnabled = false;
        float filterFrequency = 20000.0f;
        float filterResonance = 0.707f;
        float filterMorph = 0.0f;
        EnhancedAudioStrip::FilterAlgorithm filterAlgorithm = EnhancedAudioStrip::FilterAlgorithm::Tpt12;
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
};
