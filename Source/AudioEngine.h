/*
  ==============================================================================

    AudioEngine.h
    Modern audio engine for mlrVST with advanced features
    
    Features:
    - High-quality resampling
    - Tempo sync
    - Quantization
    - Crossfading
    - Group management
    - Pattern recording
    - Live input recording

  ==============================================================================
*/

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>
#include <atomic>
#include <array>
#include <limits>
#include <set>
#include <random>

#include "StepSampler.h"
#include "LadderFilterBase.h"

//==============================================================================
/**
 * Resampler - High-quality sample rate conversion and time stretching
 */
class Resampler
{
public:
    Resampler();
    
    enum class Quality
    {
        Linear,
        Cubic,
        Sinc,
        SincHQ
    };
    
    void setQuality(Quality q) { quality = q; }
    Quality getQuality() const { return quality; }
    
    // Read a sample with fractional position and speed
    float getSample(const juce::AudioBuffer<float>& buffer, 
                   int channel, 
                   double position,
                   double speed);
    
private:
    Quality quality = Quality::Cubic;
    
    float linearInterpolate(float a, float b, float t);
    float cubicInterpolate(float y0, float y1, float y2, float y3, float t);
    float sincInterpolate(const float* data, int length, double position, int taps);
};

//==============================================================================
/**
 * Crossfader - Smooth transitions to avoid clicks
 */
class Crossfader
{
public:
    Crossfader();
    
    void reset(int sampleRate);
    void startFade(bool fadeIn, int numSamples = -1, bool forceRestartFromEdge = false);
    float getNextValue();
    bool isActive() const { return active; }
    
private:
    std::atomic<bool> active{false};
    std::atomic<float> currentGain{1.0f};
    float targetGain = 1.0f;
    float fadeDirection = 1.0f;  // 1.0 = fade in, -1.0 = fade out
    int totalSamples = 0;
    int samplesRemaining = 0;
};

//==============================================================================
/**
 * QuantizationClock - Handles rhythmic quantization
 */
/**
 * QuantizationClock - Sample-accurate input quantization based on InputQuantiser pattern
 */

// Forward declaration
class EnhancedAudioStrip;

struct QuantisedTrigger
{
    int64_t targetSample = 0;
    double targetPPQ = 0.0;      // Exact PPQ grid value (calculated at schedule time)
    int stripIndex = -1;
    int column = 0;
    bool clearPendingOnFire = true;
    bool isMomentaryStutter = false;
    bool isSequencerRetrigger = false;
};

class QuantizationClock
{
public:
    QuantizationClock();
    
    void setTempo(double bpm);
    void setQuantization(int division); // 1, 2, 4, 8, 16, 32
    void setSampleRate(double sr);
    void reset();
    
    void advance(int numSamples);
    void updateFromPPQ(double ppq, int numSamples); // NEW: Update from master PPQ clock
    int64_t getCurrentSample() const { return currentSample.load(std::memory_order_acquire); }
    void scheduleTrigger(int stripIndex, int column, double currentPPQ, EnhancedAudioStrip* strip); // NEW: Takes PPQ and strip
    bool hasPendingTrigger(int stripIndex) const;
    void clearPendingTriggers();
    void clearPendingTriggersForStrip(int stripIndex); // Clear all pending triggers for a specific strip
    
    // Get events that should execute within a sample range (for sample-accurate execution)
    std::vector<QuantisedTrigger> getEventsInRange(int64_t blockStart, int64_t blockEnd);
    
    double getQuantBeats() const; // Get quantization in beats (public for grid calculation)
    
private:
    double tempo = 120.0;
    double sampleRate = 44100.0;
    int quantizeDivision = 8;
    std::atomic<int64_t> currentSample{0};
    double currentPPQ = 0.0; // NEW: Track master PPQ
    std::vector<QuantisedTrigger> pendingTriggers;
    mutable juce::SpinLock pendingTriggersLock;
    
    int getQuantSamples() const;
};

//==============================================================================
/**
 * PatternRecorder - Records and plays back button sequences
 */
class PatternRecorder
{
public:
    struct Event
    {
        int stripIndex;
        int column;
        double time; // in beats (relative to pattern start)
        bool isNoteOn;
        
        bool operator<(const Event& other) const { return time < other.time; }
    };
    
    PatternRecorder();
    
    void setLength(int beats);
    void startRecording(double currentBeat);  // Start on next beat boundary
    void stopRecording();
    void startPlayback();
    void startPlayback(double currentBeat);
    void stopPlayback();
    void clear();
    
    void recordEvent(int strip, int column, bool noteOn, double currentBeat);
    
    // Check if recording should auto-stop (returns true if stopped)
    bool updateRecording(double currentBeat);
    
    // Advance playback position by beat delta (call every audio block)
    void advancePlayback(double beatDelta);
    
    // Process events that fall within the current time slice
    template<typename Callback>
    void processEventsInTimeSlice(double beatDelta, Callback&& callback);

    // Process events in an absolute beat window [fromBeat, toBeat)
    template<typename Callback>
    void processEventsForBeatWindow(double fromBeat, double toBeat, Callback&& callback) const;
    
    // Optimized: Returns events in beat range without allocation
    // Callback is called for each event in range (lock-free playback)
    template<typename Callback>
    void processEventsInRange(double fromBeat, double toBeat, Callback&& callback) const;
    
    bool isRecording() const { return recording.load(std::memory_order_acquire); }
    bool isPlaying() const { return playing.load(std::memory_order_acquire); }
    bool hasEvents() const { return !events.empty(); }
    int getEventCount() const { return static_cast<int>(events.size()); }
    int getLengthInBeats() const { return lengthInBeats.load(std::memory_order_acquire); }
    double getRecordingStartBeat() const { return recordingStartBeat.load(std::memory_order_acquire); }
    void stop() { stopPlayback(); }
    std::vector<Event> getEventsSnapshot() const;
    void setEventsSnapshot(const std::vector<Event>& newEvents, int lengthBeats);
    
private:
    // Events sorted by time for efficient range queries
    std::vector<Event> events;
    
    // Atomic flags for lock-free reads
    std::atomic<bool> recording{false};
    std::atomic<bool> playing{false};
    std::atomic<int> lengthInBeats{4};
    
    // Recording start/end beats (quantized to master clock)
    std::atomic<double> recordingStartBeat{-1.0};
    std::atomic<double> recordingEndBeat{-1.0};
    
    // Playback position within pattern (0.0 to lengthInBeats, loops)
    std::atomic<double> playbackPosition{0.0};
    std::atomic<double> playbackStartBeat{-1.0}; // Absolute beat where playback is anchored
    
    // Only used during recording (not in audio thread)
    mutable juce::CriticalSection recordLock;
    
    // Last processed beat to avoid duplicate triggers
    mutable std::atomic<double> lastProcessedBeat{-1.0};
};

//==============================================================================
/**
 * LiveRecorder - Continuous circular buffer recording
 * Always capturing input, can capture last N bars on demand
 */
class LiveRecorder
{
public:
    LiveRecorder();
    
    void prepareToPlay(double sampleRate, int maxBlockSize);
    void startRecording(int lengthInBeats, double tempo);  // Legacy - unused
    void stopRecording();  // Legacy - unused
    
    void processInput(const juce::AudioBuffer<float>& input, int startSample, int numSamples);
    juce::AudioBuffer<float> getRecordedBuffer();  // Legacy - unused
    
    // NEW: Continuous recording methods
    void setLoopLength(int bars);  // Legacy - not used anymore
    int getSelectedLoopLength() const;  // Legacy - not used anymore
    void setCrossfadeLengthMs(float ms);
    juce::AudioBuffer<float> captureLoop(double tempo, int bars);  // Capture N bars
    bool shouldBlinkRecordLED(double beatPosition) const;  // For LED feedback
    void clearBuffer();
    
    bool isRecording() const { return recording; }
    float getRecordingProgress() const;
    
private:
    void bakeLoopCrossfade(juce::AudioBuffer<float>& buffer, int loopStart, int loopEnd);
    void bakeLoopCrossfadeWithPreroll(juce::AudioBuffer<float>& loopBuffer,
                                      const juce::AudioBuffer<float>& captureBuffer,
                                      int loopStart, int loopEnd, int crossfadeSamples);
    
    juce::AudioBuffer<float> circularBuffer;  // Continuous circular buffer
    std::atomic<bool> recording{false};
    std::atomic<int> writeHead{0};  // Current write position in circular buffer
    int selectedBars{1};  // Selected loop length: 1-4 bars
    float crossfadeLengthMs{10.0f};  // Crossfade length in milliseconds (1-50ms)
    double currentSampleRate = 44100.0;
    
    juce::CriticalSection bufferLock;
};

//==============================================================================
/**
 * StripGroup - Group management for mute groups
 */
class StripGroup
{
public:
    StripGroup(int groupId);
    
    void addStrip(int stripIndex);
    void removeStrip(int stripIndex);
    bool containsStrip(int stripIndex) const;
    
    void setVolume(float vol);
    float getVolume() const { return volume; }
    
    void setMuted(bool mute);
    bool isMuted() const { return muted; }
    
    const std::vector<int>& getStrips() const { return strips; }
    
private:
    std::vector<int> strips;
    std::atomic<float> volume{1.0f};
    std::atomic<bool> muted{false};
};

//==============================================================================
/**
 * EnhancedAudioStrip - Modernized strip with all features
 */
class EnhancedAudioStrip
{
public:
    struct ExposedLadderFilter : juce::dsp::LadderFilter<float>
    {
        using juce::dsp::LadderFilter<float>::processSample;
    };

    // Enums must be declared first (used as member types below)
    enum class PlayMode
    {
        OneShot,
        Loop,
        Gate,
        Step,       // Step sequencer mode
        Grain       // Granular freeze/scratch mode
    };
    
    enum class DirectionMode
    {
        Normal,         // Forward playback
        Reverse,        // Backward playback
        PingPong,       // Bounce back and forth
        Random,         // Random jump on each trigger
        RandomWalk,     // Random small steps
        RandomSlice     // Random slice selection
    };
    
    enum class FilterType
    {
        LowPass,
        BandPass,
        HighPass
    };

    enum class FilterAlgorithm
    {
        Tpt12 = 0,    // 12 dB/oct TPT SVF
        Tpt24,        // 24 dB/oct (cascaded TPT SVF)
        Ladder12,     // JUCE ladder, 12 dB morph bank
        Ladder24,     // JUCE ladder, 24 dB morph bank
        MoogStilson,  // MoogLadders Stilson LP model
        MoogHuov      // MoogLadders Huovilainen LP model
    };

    enum class SwingDivision
    {
        Quarter = 0,   // 1/4
        Eighth,        // 1/8
        Sixteenth,     // 1/16
        Triplet        // 1/8T
    };
    
    // Public member variables (must be declared before inline methods that use them)
    int loopStart = 0;
    int loopEnd = 16;
    bool loopEnabled = false;
    bool reverse = false;
    
    std::atomic<float> beatsPerLoop{-1.0f};  // -1 = auto-detect, otherwise manual override
    int recordingBars = 1;  // Unified per-strip bars for capture + loaded sample mapping (1..8)
    
    std::atomic<float> volume{1.0f};
    std::atomic<float> pan{0.0f};
    
    // Step sequencer state (PUBLIC for GUI access)
    static constexpr int MaxStepSubdivisions = 16;
    int currentStep = 0;                        // Current absolute step index (0-63)
    std::array<bool, 64> stepPattern = {};      // Up to 64 total steps
    std::array<int, 64> stepSubdivisions = {};  // Per-step retrigger count (1..MaxStepSubdivisions)
    std::array<float, 64> stepSubdivisionStartVelocity = {};  // 0..1 gain for first substep hit
    std::array<float, 64> stepSubdivisionRepeatVelocity = {}; // 0..1 gain target for repeated substeps
    std::array<float, 64> stepProbability = {}; // 0..1 trigger chance per step
    std::atomic<int> stepPatternLengthSteps{16}; // 1..64 absolute pattern length
    std::atomic<int> stepViewPage{0};            // Visible 16-step page (0..ceil(steps/16)-1)
    std::atomic<float> stepEnvelopeAttackMs{0.0f};
    std::atomic<float> stepEnvelopeDecayMs{4000.0f};
    std::atomic<float> stepEnvelopeReleaseMs{110.0f};
    
    PlayMode playMode = PlayMode::Loop;
    DirectionMode directionMode = DirectionMode::Normal;
    
    // Morphing filter core (phase-coherent LP/BP/HP from matched TPT sections).
    juce::dsp::StateVariableTPTFilter<float> filterLp;
    juce::dsp::StateVariableTPTFilter<float> filterBp;
    juce::dsp::StateVariableTPTFilter<float> filterHp;
    juce::dsp::StateVariableTPTFilter<float> filterLpStage2;
    juce::dsp::StateVariableTPTFilter<float> filterBpStage2;
    juce::dsp::StateVariableTPTFilter<float> filterHpStage2;
    ExposedLadderFilter ladderLp;
    ExposedLadderFilter ladderBp;
    ExposedLadderFilter ladderHp;
    std::atomic<float> filterFrequency{20000.0f};  // Default fully open (20kHz)
    std::atomic<float> filterResonance{0.707f};    // Q = 0.707 (butterworth)
    std::atomic<float> filterMorph{0.0f};          // 0.0=LP, 0.5=BP, 1.0=HP
    std::atomic<int> filterAlgorithm{static_cast<int>(FilterAlgorithm::Tpt12)};
    int cachedLadderMode = -1;
    float cachedMoogCutoff = -1.0f;
    float cachedMoogResonance = -1.0f;
    int cachedMoogModel = -1;
    float cachedMoogSampleRate = -1.0f;
    std::unique_ptr<LadderFilterBase> moogLpL;
    std::unique_ptr<LadderFilterBase> moogLpR;
    FilterType filterType = FilterType::LowPass;
    bool filterEnabled = false;  // Disabled by default, auto-enables on use
    std::atomic<float> swingAmount{0.0f};   // 0..1 transport swing depth
    std::atomic<int> swingDivision{static_cast<int>(SwingDivision::Eighth)};
    std::atomic<float> gateAmount{0.0f};    // 0..1 gate effect depth
    std::atomic<float> gateSpeed{4.0f};     // cycles per beat (default 1/16)
    std::atomic<float> gateEnvelope{0.5f};  // 0..1 smoothing
    std::atomic<float> gateShape{0.5f};     // 0..1 pulse/curve shape
    
    // Public methods
    EnhancedAudioStrip(int stripIndex);
    
    void prepareToPlay(double sampleRate, int maxBlockSize);
    void loadSample(const juce::AudioBuffer<float>& buffer, double sourceRate);
    bool loadSampleFromFile(const juce::File& file);
    void clearSample();
    
    void process(juce::AudioBuffer<float>& output, 
                int startSample, 
                int numSamples,
                const juce::AudioPlayHead::PositionInfo& positionInfo,
                int64_t globalSampleStart,
                double tempo,
                double quantizeBeats);
    
    // Playback control
    void trigger(int column, double tempo, bool quantized = false);
    void triggerAtSample(int column, double tempo, int64_t globalSample, 
                        const juce::AudioPlayHead::PositionInfo& positionInfo,
                        bool stutterRetrigger = false);  // Sample-accurate trigger with PPQ sync
    void stop(bool immediate = false);
    void syncToGlobalPhase(double globalPhase, double tempo);  // NEW: Sync playback to global clock
    void calculatePositionFromGlobalSample(int64_t globalSample, double tempo);  // Calculate from global clock
    void setLoop(int startColumn, int endColumn);
    void clearLoop();
    void setPlaybackMarkerColumn(int column, int64_t currentGlobalSample);
    void restorePresetPpqState(bool shouldPlay,
                               bool timelineAnchored,
                               double timelineOffsetBeats,
                               int fallbackColumn,
                               double tempo,
                               double currentTimelineBeat,
                               int64_t currentGlobalSample);
    
    // Step sequencer control
    void startStepSequencer();  // Start step sequencer playback (auto-runs with clock)
    void retriggerStepVoice();  // Retrigger currently active step voice without editing gates
    void setStepPatternLengthSteps(int steps);
    int getStepPatternLengthSteps() const { return juce::jlimit(1, 64, stepPatternLengthSteps.load(std::memory_order_acquire)); }
    void setStepPatternBars(int bars);
    int getStepPatternBars() const { return juce::jmax(1, (getStepPatternLengthSteps() + 15) / 16); }
    int getStepTotalSteps() const { return getStepPatternLengthSteps(); }
    int getStepPage() const { return stepViewPage.load(std::memory_order_acquire); }
    void setStepPage(int page);
    std::array<bool, 16> getVisibleStepPattern() const;
    int getVisibleCurrentStep() const;
    int getVisibleStepOffset() const;
    void toggleStepAtVisibleColumn(int column);
    void toggleStepAtIndex(int absoluteStep);
    void setStepEnabledAtIndex(int absoluteStep, bool enabled, bool resetShapeOnEnable = true);
    int getStepSubdivisionAtIndex(int absoluteStep) const;
    void setStepSubdivisionAtIndex(int absoluteStep, int subdivisions);
    float getStepSubdivisionStartVelocityAtIndex(int absoluteStep) const;
    float getStepSubdivisionRepeatVelocityAtIndex(int absoluteStep) const;
    void setStepSubdivisionVelocityRangeAtIndex(int absoluteStep, float startVelocity, float endVelocity);
    void setStepSubdivisionRepeatVelocityAtIndex(int absoluteStep, float velocity);
    float getStepProbabilityAtIndex(int absoluteStep) const;
    void setStepProbabilityAtIndex(int absoluteStep, float probability);
    void setStepEnvelopeAttackMs(float ms);
    float getStepEnvelopeAttackMs() const { return stepEnvelopeAttackMs.load(std::memory_order_acquire); }
    void setStepEnvelopeDecayMs(float ms);
    float getStepEnvelopeDecayMs() const { return stepEnvelopeDecayMs.load(std::memory_order_acquire); }
    void setStepEnvelopeReleaseMs(float ms);
    float getStepEnvelopeReleaseMs() const { return stepEnvelopeReleaseMs.load(std::memory_order_acquire); }
    
    void setBeatsPerLoop(float beats);  // Manual override: how many beats is this loop?
    void setBeatsPerLoopAtPpq(float beats, double hostPpqNow);
    float getBeatsPerLoop() const { return beatsPerLoop.load(); }
    
    // Recording length per strip
    void setRecordingBars(int bars)
    {
        const int clamped = juce::jlimit(1, 8, bars);
        if (clamped <= 1) recordingBars = 1;
        else if (clamped <= 2) recordingBars = 2;
        else if (clamped <= 4) recordingBars = 4;
        else recordingBars = 8;
    }
    int getRecordingBars() const { return recordingBars; }
    
    // Parameters
    void setVolume(float vol);
    float getVolume() const { return volume.load(); }
    void setPan(float panValue); // -1.0 (left) to 1.0 (right)
    float getPan() const { return pan.load(); }
    void setPlayheadSpeedRatio(float ratio)
    {
        playheadSpeedRatio.store(juce::jlimit(0.125f, 4.0f, ratio), std::memory_order_release);
    }
    float getPlayheadSpeedRatio() const { return playheadSpeedRatio.load(std::memory_order_acquire); }
    void setPlaybackSpeed(float speed);
    void setPlaybackSpeedImmediate(float speed);
    void setMomentaryStutterTimingActive(bool active)
    {
        momentaryStutterTimingActive.store(active ? 1 : 0, std::memory_order_release);
    }
    void setPitchSmoothingTime(float seconds);  // Update speed smoothing ramp time (0-1 seconds)
    float getPlaybackSpeed() const { return static_cast<float>(playbackSpeed.load()); }
    float getDisplaySpeed() const { return displaySpeedAtomic.load(std::memory_order_acquire); }
    void setPitchShift(float semitones);
    float getPitchShift() const { return pitchShiftSemitones.load(); }
    void setScratchAmount(float amount) { scratchAmount = juce::jlimit(0.0f, 100.0f, amount); }
    float getScratchAmount() const { return scratchAmount.load(); }
    void setReverse(bool shouldReverse);
    bool isReversed() const { return reverse; }
    void setTransientSliceMode(bool enabled);
    bool isTransientSliceMode() const { return transientSliceMode.load(std::memory_order_acquire); }
    std::array<int, 16> getSliceStartSamples(bool transientMode) const;
    std::array<int, 16> getCachedTransientSliceSamples() const;
    std::array<float, 128> getCachedRmsMap() const;
    std::array<int, 128> getCachedZeroCrossMap() const;
    bool hasSampleAnalysisCache() const { return analysisCacheValid && analysisSampleCount > 0; }
    int getAnalysisSampleCount() const { return analysisSampleCount; }
    void restoreSampleAnalysisCache(const std::array<int, 16>& transientSlices,
                                    const std::array<float, 128>& rmsMap,
                                    const std::array<int, 128>& zeroCrossMap,
                                    int sourceSampleCount);
    void setTriggerFadeInMs(float ms) { triggerFadeInMs.store(juce::jlimit(0.1f, 120.0f, ms), std::memory_order_release); }
    float getTriggerFadeInMs() const { return triggerFadeInMs.load(std::memory_order_acquire); }
    
    // Musical scratching - button hold behaviors
    void onButtonPress(int column, int64_t globalSample);    // Called when Monome button pressed
    void onButtonRelease(int column, int64_t globalSample);  // Called when Monome button released
    bool isButtonHeld() const { return buttonHeld; }
    int getHeldButton() const { return heldButton; }
    int getHeldButtonCount() const { return static_cast<int>(heldButtons.size()); }
    bool isScratchActive() const { return scrubActive || tapeStopActive || scratchGestureActive; }
    bool isGrainFreezeActive() const;
    int getGrainAnchorColumn() const;
    int getGrainSecondaryColumn() const;
    int getGrainSizeControlColumn() const;
    int getGrainHeldCount() const;
    float getGrainSizeMs() const;
    float getGrainBaseSizeMs() const;
    float getGrainDensity() const;
    float getGrainPitch() const;
    float getGrainPitchJitter() const;
    float getGrainSpread() const;
    float getGrainJitter() const;
    float getGrainPositionJitter() const;
    float getGrainRandomDepth() const;
    float getGrainArpDepth() const;
    float getGrainCloudDepth() const;
    float getGrainEmitterDepth() const;
    float getGrainEnvelope() const;
    float getGrainShape() const;
    int getGrainArpMode() const;
    bool isGrainTempoSyncEnabled() const;
    std::array<float, 8> getGrainPreviewPositions() const;
    std::array<float, 8> getGrainPreviewPitchNorms() const;
    void setGrainSizeMs(float value);
    void setGrainSizeModulatedMs(float value);
    void clearGrainSizeModulation();
    void setGrainDensity(float value);
    void setGrainPitch(float semitones);
    void setGrainPitchJitter(float semitones);
    void setGrainSpread(float value);
    void setGrainJitter(float value);
    void setGrainPositionJitter(float value);
    void setGrainRandomDepth(float value);
    void setGrainArpDepth(float value);
    void setGrainCloudDepth(float value);
    void setGrainEmitterDepth(float value);
    void setGrainEnvelope(float value);
    void setGrainShape(float value);
    void setGrainArpMode(int mode);
    void setGrainTempoSyncEnabled(bool enabled);
    void setGrainResamplerQuality(Resampler::Quality quality) { grainResampler.setQuality(quality); }
    void captureMomentaryPhaseReference(double hostPpq);
    void enforceMomentaryPhaseReference(double hostPpq, int64_t currentGlobalSample);
    void realignToPpqAnchor(double hostPpq, int64_t currentGlobalSample);
    
    // StepSampler access
    StepSampler* getStepSampler() { return &stepSampler; }
    const StepSampler* getStepSampler() const { return &stepSampler; }
    
    // Filter (ZDF State Variable)
    void setFilterEnabled(bool enabled) { filterEnabled = enabled; }
    bool isFilterEnabled() const { return filterEnabled; }
    void setFilterFrequency(float freq);
    float getFilterFrequency() const { return filterFrequency.load(); }
    void setFilterResonance(float res);
    float getFilterResonance() const { return filterResonance.load(); }
    void setFilterMorph(float morph);
    float getFilterMorph() const { return filterMorph.load(std::memory_order_acquire); }
    void setFilterType(FilterType type);
    FilterType getFilterType() const { return filterType; }
    void setFilterAlgorithm(FilterAlgorithm algorithm);
    FilterAlgorithm getFilterAlgorithm() const
    {
        return static_cast<FilterAlgorithm>(juce::jlimit(0, 5, filterAlgorithm.load(std::memory_order_acquire)));
    }
    void setSwingAmount(float amount) { swingAmount = juce::jlimit(0.0f, 1.0f, amount); }
    float getSwingAmount() const { return swingAmount.load(std::memory_order_acquire); }
    void setSwingDivision(SwingDivision division) { swingDivision.store(static_cast<int>(division), std::memory_order_release); }
    SwingDivision getSwingDivision() const { return static_cast<SwingDivision>(swingDivision.load(std::memory_order_acquire)); }
    void setGateAmount(float amount) { gateAmount = juce::jlimit(0.0f, 1.0f, amount); }
    float getGateAmount() const { return gateAmount.load(std::memory_order_acquire); }
    void setGateSpeed(float speed) { gateSpeed = juce::jlimit(0.25f, 8.0f, speed); }
    float getGateSpeed() const { return gateSpeed.load(std::memory_order_acquire); }
    void setGateEnvelope(float amount) { gateEnvelope = juce::jlimit(0.0f, 1.0f, amount); }
    float getGateEnvelope() const { return gateEnvelope.load(std::memory_order_acquire); }
    void setGateShape(float shape) { gateShape = juce::jlimit(0.0f, 1.0f, shape); }
    float getGateShape() const { return gateShape.load(std::memory_order_acquire); }
    void setLoopCrossfadeLengthMs(float ms) { loopCrossfadeLengthMs.store(juce::jlimit(1.0f, 50.0f, ms), std::memory_order_release); }
    
    // Play mode setters/getters
    void setPlayMode(PlayMode mode) 
    { 
        PlayMode oldMode = playMode;
        playMode = mode;

        // Switching INTO step mode while transport is already running must
        // immediately arm step playback; waiting for a new transport edge
        // makes Step mode appear "broken".
        if (mode == PlayMode::Step)
        {
            scrubActive = false;
            tapeStopActive = false;
            scratchGestureActive = false;
            resetGrainState();
            buttonHeld = false;
            patternActive = false;
            activePattern = -1;
            lastStepTime = -1.0;
            currentStep = 0;
            stepSubdivisionSixteenth = std::numeric_limits<int64_t>::min();
            stepTraversalTick = std::numeric_limits<int64_t>::min();
            stepSubdivisionTriggerIndex = 0;
            stepSubdivisionGateOpen = true;
            stepTraversalRatioAtLastTick = -1.0;

            // If the strip already has loop sample content but step sampler
            // is empty, bootstrap step mode from that content.
            if (!stepSampler.getHasAudio() && sampleBuffer.getNumSamples() > 0)
                stepSampler.loadSampleFromBuffer(sampleBuffer, sourceSampleRate);

            if (lastHostPlayingState)
                playing = true;
            else
                playing = false;

            stepRandomWalkPos = currentStep;
            stepRandomSliceBeatGroup = -1;
        }
        else if (mode == PlayMode::Grain)
        {
            stepSampler.allNotesOff();
            scrubActive = false;
            tapeStopActive = false;
            scratchGestureActive = false;
            buttonHeld = false;
            heldButton = -1;
            patternActive = false;
            activePattern = -1;
            resetGrainState();
            if (sampleLength > 0.0)
                grainCenterSmoother.setCurrentAndTargetValue(playbackPosition.load());
            // Force immediate first grain spawn when entering grain mode.
            grainSpawnAccumulator = 1.0;
            // Brief identity scheduler hold so initial grain playback tracks
            // one deterministic playhead before evolving into full cloud behavior.
            grainEntryIdentitySamplesRemaining = juce::jmax(32, static_cast<int>(currentSampleRate * 0.18));
            grainEntryIdentityTotalSamples = grainEntryIdentitySamplesRemaining;

            // Short output blend from previous sample to prevent mode-switch crackle.
            const float modeFadeMs = juce::jlimit(0.2f, 8.0f, triggerFadeInMs.load(std::memory_order_acquire));
            triggerOutputBlendTotalSamples = juce::jmax(16, static_cast<int>(currentSampleRate * 0.001 * modeFadeMs));
            triggerOutputBlendSamplesRemaining = triggerOutputBlendTotalSamples;
            triggerOutputBlendStartL = lastOutputSampleL;
            triggerOutputBlendStartR = lastOutputSampleR;
            triggerOutputBlendActive = true;
            playing = true;
        }
        else if (oldMode == PlayMode::Step && mode != PlayMode::Step)
        {
            stepSampler.allNotesOff();
            lastStepTime = -1.0;
            stepSamplePlaying = false;
            stepSubdivisionSixteenth = std::numeric_limits<int64_t>::min();
            stepTraversalTick = std::numeric_limits<int64_t>::min();
            stepSubdivisionTriggerIndex = 0;
            stepSubdivisionGateOpen = true;
            stepTraversalRatioAtLastTick = -1.0;
            scrubActive = false;
            tapeStopActive = false;
            scratchGestureActive = false;
            playing = false;
            wasPlayingBeforeStop = false;
        }
        else if (oldMode == PlayMode::Grain && mode != PlayMode::Grain)
        {
            resetGrainState();
            grainEntryIdentitySamplesRemaining = 0;
            grainEntryIdentityTotalSamples = 0;
        }
    }
    PlayMode getPlayMode() const { return playMode; }
    
    // Get last trigger PPQ (for quantization debouncing)
    double getLastTriggerPPQ() const { return lastTriggerPPQ; }
    
    // State
    bool isPlaying() const { return playing; }
    double getPlaybackPosition() const { return playbackPosition; }
    double getNormalizedPosition() const;
    bool isPpqTimelineAnchored() const { return ppqTimelineAnchored; }
    double getPpqTimelineOffsetBeats() const { return ppqTimelineOffsetBeats; }
    bool hasAudio() const { return sampleBuffer.getNumSamples() > 0; }
    const juce::AudioBuffer<float>* getAudioBuffer() const { return &sampleBuffer; }
    double getSourceSampleRate() const { return sourceSampleRate; }
    
    // Control accessors (new ones not already defined)
    int getLoopStart() const { return loopStart; }
    int getLoopEnd() const { return loopEnd; }
    
    // Groups
    void setGroup(int newGroupId) { groupId = newGroupId; }
    int getGroup() const { return groupId; }
    
    // LED feedback
    int getCurrentColumn() const;
    std::array<bool, 16> getLEDStates() const;
    
    // Resampler access
    Resampler resampler;
    Resampler grainResampler;
    
private:
    struct GrainParams
    {
        // Tuned for cleaner "near-normal" default grain playback.
        float sizeMs = 1240.0f;    // 5..2400
        float density = 0.05f;     // 0..1
        float pitchSemitones = 0.0f;
        float pitchJitterSemitones = 0.0f;
        float spread = 0.0f;       // 0..1
        float jitter = 0.0f;       // 0..1 bloom modulation depth
        float positionJitter = 0.0f; // 0..1 grain center jitter depth
        float randomDepth = 0.0f;  // 0..1 spray/reverse modulation depth
        float arpDepth = 0.0f;     // 0..1 arpeggiation depth
        float cloudDepth = 0.0f;   // 0..1 cloud delay mix/feedback
        float emitterDepth = 0.0f; // 0..1 quantized emitter around playhead
        float envelope = 0.0f;     // 0..1 edge fade length (higher = longer fades)
        float shape = 0.0f;        // -1..1 envelope bend (negative=rounder, positive=sharper)
        int arpMode = 0;           // 0=Octave, 1=Power, 2=Zigzag
        bool reverse = false;
    };

    struct GrainGestureState
    {
        bool anyHeld = false;
        int heldCount = 0;
        int heldX[3] = { -1, -1, -1 };
        uint32_t heldOrder[3] = { 0, 0, 0 };
        uint32_t orderCounter = 0;

        int anchorX = -1;
        int secondaryX = -1;
        int sizeControlX = -1;

        double targetCenterSample = 0.0;
        double frozenCenterSample = 0.0;
        double centerSampleSmoothed = 0.0;
        double centerTravelDistanceAbs = 0.0;
        float centerRampMs = 40.0f;
        bool freeze = false;
        bool returningToTimeline = false;
        int64_t sceneStartSample = 0;
    };

    struct GrainVoice
    {
        bool active = false;
        int ageSamples = 0;
        int lengthSamples = 0;
        double readPos = 0.0;
        double step = 1.0;
        float pitchSemitones = 0.0f;
        float panL = 1.0f;
        float panR = 1.0f;
        float envelopeCurve = 1.0f;
        float envelopeSkew = 0.5f;
        float envelopeFade = 0.35f;
    };

    juce::AudioBuffer<float> sampleBuffer;
    juce::LagrangeInterpolator interpolators[2]; // For stereo
    
    std::atomic<double> playbackPosition{0.0};
    std::atomic<float> playheadSpeedRatio{1.0f}; // Playmarker traversal multiplier (quantized musical ratios)
    std::atomic<double> playbackSpeed{1.0};
    std::atomic<float> displaySpeedAtomic{1.0f};
    std::atomic<bool> playing{false};
    std::atomic<bool> pendingTrigger{false};
    std::atomic<float> scratchAmount{0.0f};  // Per-strip scratch: 0-100%
    std::atomic<float> loopCrossfadeLengthMs{10.0f};
    std::atomic<float> triggerFadeInMs{12.0f};
    std::atomic<bool> transientSliceMode{false};
    GrainParams grainParams;
    GrainGestureState grainGesture;
    static constexpr int kMaxGrainVoices = 32;
    std::array<GrainVoice, kMaxGrainVoices> grainVoices{};
    std::array<float, 2048> grainWindow{};
    juce::AudioBuffer<float> grainCloudDelayBuffer;
    int grainCloudDelayWritePos = 0;
    int grainArpStep = 0;
    double grainSpawnAccumulator = 0.0;
    double grainSchedulerNoise = 0.0;
    double grainSchedulerNoiseTarget = 0.0;
    int grainSchedulerNoiseCountdown = 0;
    int grainEntryIdentitySamplesRemaining = 0;
    int grainEntryIdentityTotalSamples = 0;
    juce::SmoothedValue<double> grainCenterSmoother{0.0};
    juce::SmoothedValue<float> grainSizeSmoother{1240.0f};
    juce::SmoothedValue<float> grainSyncedSizeSmoother{1240.0f};
    juce::SmoothedValue<float> grainDensitySmoother{0.05f};
    juce::SmoothedValue<float> grainPitchSmoother{0.0f};
    juce::SmoothedValue<float> grainPitchJitterSmoother{0.0f};
    juce::SmoothedValue<float> grainFreezeBlendSmoother{0.0f};
    juce::SmoothedValue<float> grainScratchSceneMix{0.0f};
    double grainBloomPhase = 0.0;
    float grainBloomAmount = 0.0f;
    float grainNeutralBlendState = 1.0f;
    float grainOverlapNormState = 1.0f;
    
    // Smoothed parameters
    juce::SmoothedValue<float> smoothedVolume{0.7f};
    juce::SmoothedValue<float> smoothedPan{0.0f};
    juce::SmoothedValue<float> smoothedSpeed{1.0f};
    juce::SmoothedValue<float> smoothedPitchShift{0.0f};
    juce::SmoothedValue<float> smoothedFilterFrequency{20000.0f};
    juce::SmoothedValue<float> smoothedFilterResonance{0.707f};
    juce::SmoothedValue<float> smoothedFilterMorph{0.0f};
    std::atomic<float> pitchShiftSemitones{0.0f};
    juce::AudioBuffer<float> pitchShiftDelayBuffer;
    int pitchShiftWritePos = 0;
    int pitchShiftDelaySize = 0;
    double pitchShiftPhase = 0.0;
    
    int stripIndex = 0;
    int groupId = 0;
    double sampleLength = 0;
    double sourceSampleRate = 44100.0;
    double currentSampleRate = 44100.0;
    
    // For sample-accurate playback
    int triggerColumn = 0;           // Which column was triggered
    double triggerOffsetRatio = 0.0; // Trigger position in current loop space (0..1)
    double loopLengthSamples = 0;    // Length of the loop in samples
    int64_t triggerSample = 0;       // Absolute sample when triggered (for sync)
    double triggerPpqPosition = -1.0; // PPQ position when triggered (for timeline sync)
    double lastTriggerPPQ = -999.0;   // PPQ when last trigger FIRED (not scheduled)
    bool ppqTimelineAnchored = false; // Absolute PPQ timeline lock active
    double ppqTimelineOffsetBeats = 0.0; // Beat offset so selected column maps correctly on timeline
    bool scratchSavedPpqTimelineAnchored = false; // PPQ anchor state captured at scratch entry
    double scratchSavedPpqTimelineOffsetBeats = 0.0; // PPQ offset captured at scratch entry
    bool lastObservedPpqValid = false;
    double lastObservedPPQ = 0.0;
    int64_t lastObservedGlobalSample = 0;
    double lastObservedTempo = 120.0;
    bool speedPpqBypassActive = false;
    std::atomic<int> momentaryStutterTimingActive{0};
    double stopLoopPosition = 0.0;    // Position in loop when stopped (for visual sync)
    bool lastHostPlayingState = true; // Track host transport state changes (start true to detect first stop)
    bool wasPlayingBeforeStop = false; // Track if strip was playing when host stopped (for auto-resume)
    bool stopAfterFade = false; // Defer hard stop until fade-out reaches zero.
    bool retriggerBlendActive = false;
    int retriggerBlendSamplesRemaining = 0;
    int retriggerBlendTotalSamples = 0;
    double retriggerBlendOldPosition = 0.0;
    bool triggerOutputBlendActive = false;
    int triggerOutputBlendSamplesRemaining = 0;
    int triggerOutputBlendTotalSamples = 0;
    float triggerOutputBlendStartL = 0.0f;
    float triggerOutputBlendStartR = 0.0f;
    float lastOutputSampleL = 0.0f;
    float lastOutputSampleR = 0.0f;
    int64_t playheadSample = 0;      // Samples since trigger (playhead position)
    
    // Key-press smoothing / scratching (clock-locked approach)
    double targetPosition = 0.0;      // Where we want to arrive (in samples)
    int64_t targetSampleTime = 0;     // Global sample when we must arrive
    juce::SmoothedValue<double> rateSmoother{1.0};  // Smoothed playback rate
    bool scrubActive = false;         // Is scrubbing/scratching active
    
    // Musical scratching - button hold behaviors
    bool buttonHeld = false;          // Is Monome button currently held?
    int heldButton = -1;              // Which column is held (-1 = none)
    int64_t buttonPressTime = 0;      // When was button pressed (global samples)
    bool scratchArrived = false;      // Has scratch reached target position?
    double heldPosition = 0.0;        // Position where we're holding
    bool tapeStopActive = false;      // Is tape stop effect active?
    bool scratchGestureActive = false; // True once a press has engaged scratch motion
    bool isReverseScratch = false;    // Is this a reverse scratch (back to timeline)?
    bool reverseScratchPpqRetarget = false; // Continuously retarget reverse return to PPQ horizon
    double reverseScratchBeatsForLoop = 4.0;
    double reverseScratchLoopStartSamples = 0.0;
    double reverseScratchLoopLengthSamples = 1.0;
    bool reverseScratchUseRateBlend = false; // Loop mode: blend reverse rate from current->restore speed
    double reverseScratchStartRate = 0.0;
    double reverseScratchEndRate = 1.0;

    // Non-step random direction state
    std::mt19937 randomGenerator;
    int randomLastBucket = -1;
    int randomHeldSlice = 0;
    int randomWalkLastBucket = -1;
    int randomWalkSlice = 0;
    int randomSliceLastBucket = -1;
    int randomSliceBase = 0;
    int randomSliceCurrent = 0;
    int randomSliceRepeatsRemaining = 0;
    int randomSliceDirection = 1;
    double randomSliceNextTriggerBeat = -1.0;
    double randomSliceTriggerBeat = 0.0;
    double randomSliceTriggerQuantBeats = 0.25;
    double randomSliceStutterDurationBeats = 0.25;
    double randomSliceSpeedStart = 1.0;
    double randomSliceSpeedEnd = 1.0;
    int randomSliceWindowStartSlice = 0;
    int randomSliceWindowLengthSlices = 1;
    std::array<int, 16> transientSliceSamples = {};
    bool transientSliceMapDirty = true;
    std::array<float, 128> analysisRmsMap = {};
    std::array<int, 128> analysisZeroCrossMap = {};
    int analysisSampleCount = 0;
    bool analysisCacheValid = false;
    
    // Proportional scratch timing
    int64_t scratchStartTime = 0;     // When scratch started (for duration tracking)
    int64_t scratchDuration = 0;      // How long forward scratch took (samples)
    double scratchStartPosition = 0.0; // Position when scratch started
    double scratchTravelDistance = 0.0; // Signed shortest travel distance for active scratch
    
    // Rhythmic scratch patterns - multi-button hold system
    std::set<int> heldButtons;        // All currently held buttons
    std::vector<int> heldButtonOrder; // Press order for held buttons (latest at back)
    int activePattern = -1;           // Which pattern is active (-1 = none)
    int patternHoldCountRequired = 3; // 2-button or 3-button hold requirement
    double patternStartBeat = 0.0;    // When pattern started
    int lastPatternStep = -1;         // Last executed step (for change detection)
    bool patternActive = false;       // Is pattern currently running?
    bool momentaryPhaseGuardValid = false;
    double momentaryPhaseOffsetBeats = 0.0;
    double momentaryPhaseBeatsForLoop = 4.0;
    std::atomic<int> grainLedHeldCount{0};
    std::atomic<int> grainLedAnchor{-1};
    std::atomic<int> grainLedSecondary{-1};
    std::atomic<int> grainLedSizeControl{-1};
    std::atomic<bool> grainLedFreeze{false};
    std::atomic<float> grainSizeMsAtomic{1240.0f};
    std::atomic<float> grainSizeModulatedMsAtomic{-1.0f}; // < 0 => modulation inactive
    std::atomic<float> grainDensityAtomic{0.05f};
    std::atomic<float> grainPitchAtomic{0.0f};
    std::atomic<float> grainPitchJitterAtomic{0.0f};
    std::atomic<float> grainSpreadAtomic{0.0f};
    std::atomic<float> grainJitterAtomic{0.0f};
    std::atomic<float> grainPositionJitterAtomic{0.0f};
    std::atomic<float> grainRandomDepthAtomic{0.0f};
    std::atomic<float> grainArpDepthAtomic{0.0f};
    std::atomic<float> grainCloudDepthAtomic{0.0f};
    std::atomic<float> grainEmitterDepthAtomic{0.0f};
    std::atomic<float> grainEnvelopeAtomic{0.0f};
    std::atomic<float> grainShapeAtomic{0.0f};
    std::atomic<int> grainArpModeAtomic{0};
    float grainPitchBeforeArp = 0.0f;
    bool grainArpWasActive = false;
    std::atomic<bool> grainTempoSyncAtomic{true};
    std::array<std::atomic<float>, 8> grainPreviewPositions {
        std::atomic<float>{-1.0f}, std::atomic<float>{-1.0f}, std::atomic<float>{-1.0f}, std::atomic<float>{-1.0f},
        std::atomic<float>{-1.0f}, std::atomic<float>{-1.0f}, std::atomic<float>{-1.0f}, std::atomic<float>{-1.0f}
    };
    std::array<std::atomic<float>, 8> grainPreviewPitchNorms {
        std::atomic<float>{0.0f}, std::atomic<float>{0.0f}, std::atomic<float>{0.0f}, std::atomic<float>{0.0f},
        std::atomic<float>{0.0f}, std::atomic<float>{0.0f}, std::atomic<float>{0.0f}, std::atomic<float>{0.0f}
    };
    mutable std::atomic<int> grainPreviewRequestCountdown{0};
    int grainPreviewDecimationCounter = 0;
    int grainVoiceSearchStart = 0;
    int64_t grainSizeJitterBeatGroup = std::numeric_limits<int64_t>::min();
    float grainSizeJitterMul = 1.0f;
    int grainTempoSyncDivisionIndex = 0;
    int64_t grainTempoSyncDivisionBeatGroup = std::numeric_limits<int64_t>::min();
    GrainParams grainParamsBeforeGesture;
    bool grainParamsSnapshotValid = false;
    bool grainThreeButtonSnapshotActive = false;
    
private:
    void resetStepShapeAtIndex(int absoluteStep);
    double lastStepTime = 0.0;        // Last step trigger time (in beats)
    bool stepSequencerActive = false; // Is step sequencer running?
    bool stepSamplePlaying = false;   // Is a step-triggered sample currently playing?
    double stepSampleStartPos = 0.0;  // Position where step sample started
    int64_t stepTriggerSample = 0;    // When step was triggered (for one-shot playback)
    int stepRandomWalkPos = 0;
    int64_t stepRandomSliceBeatGroup = -1;
    int stepRandomSliceBase = 0;
    int stepRandomSliceDirection = 1;
    int64_t stepSubdivisionSixteenth = std::numeric_limits<int64_t>::min();
    int64_t stepTraversalTick = std::numeric_limits<int64_t>::min();
    int stepSubdivisionTriggerIndex = 0;
    bool stepSubdivisionGateOpen = true;
    double stepTraversalRatioAtLastTick = -1.0;
    
public:
    
    void setDirectionMode(DirectionMode mode) 
    { 
        directionMode = mode;
        
        // Update reverse flag for simple modes
        if (mode == DirectionMode::Reverse)
            reverse = true;
        else if (mode == DirectionMode::Normal)
            reverse = false;
        // PingPong and Random modes handle reverse dynamically

        // Reset random-mode phase when direction changes.
        randomLastBucket = -1;
        randomWalkLastBucket = -1;
        randomSliceLastBucket = -1;
        randomSliceRepeatsRemaining = 0;
        randomSliceNextTriggerBeat = -1.0;
        stepRandomSliceBeatGroup = -1;
    }
    
    DirectionMode getDirectionMode() const { return directionMode; }
    
    Crossfader crossfader;
    StepSampler stepSampler;  // Monophonic sampler for step sequencer
    
    int pendingColumn = -1;
    int quantizeWaitSamples = 0;
    
    juce::CriticalSection bufferLock;
    
    void updatePlaybackDirection();
    void ensureAnalogFiltersInitialized(FilterAlgorithm algorithm);
    void updateFilterCoefficients(float frequency, float resonance);
    void processFilterSample(float& left, float& right, float frequency, float resonance, float morph);
    double applySwingToPpq(double ppq) const;
    float computeGateModulation(double ppq) const;
    void handleLooping();
    float getPanGain(int channel) const; // 0=left, 1=right
    void rebuildTransientSliceMap();
    void rebuildSampleAnalysisCacheLocked();
    double getTriggerTargetPositionForColumn(int column, double loopStartSamples, double loopLengthSamples) const;
    double snapToNearestZeroCrossing(double targetPos, int radiusSamples) const;
    double getWrappedSamplePosition(double samplePos, double loopStartSamples, double loopLengthSamples) const;
    void resetGrainState();
    void setGrainCenterTarget(double targetSamplePos, bool proportionalRamp);
    void updateGrainHeldLedState();
    void updateGrainAnchorFromHeld();
    void updateGrainGestureOnPress(int column, int64_t globalSample);
    void updateGrainGestureOnRelease(int column, int64_t globalSample);
    void updateGrainGripModulation();
    void updateGrainSizeFromGrip();
    double getEffectiveBeatsForLoop(double tempoHintBpm) const;
    double getTimelinePositionForSample(int64_t globalSample) const;
    double getGrainBeatPositionAtSample(int64_t globalSample) const;
    double getGrainColumnCenterPosition(int column) const;
    void setGrainScratchSceneTarget(float targetMix, int heldCount, double tempoBpm);
    void spawnGrainVoice(double centerSamplePos, float sizeMs, float density, float spread, float pitchOffsetSemitones, double playbackStepBase);
    void renderGrainAtSample(float& outL, float& outR, double centerSamplePos, double effectiveSpeed, int64_t globalSample);
    void resetScratchComboState();
    
    // Musical scratching helpers
    void snapToTimeline(int64_t currentGlobalSample);
    void reverseScratchToTimeline(int64_t currentGlobalSample);
    void resetPitchShifter();
    float readPitchDelaySample(int channel, double delaySamples) const;
    void processPitchShift(float& left, float& right);
    int64_t makeFeasibleScratchDuration(double startPosSamples,
                                        double endPosSamples,
                                        int64_t requestedDurationSamples,
                                        bool reverseScratch) const;
    double computeScratchTravelDistance(double startPosSamples, double endPosSamples) const;
    int64_t calculateScratchDuration(float scratchAmountPercent, double tempo);
    int getPatternFromButtons(int btn1, int btn2, int btn3);  // Select pattern from 3-button combo
    int getPatternFromTwoButtons(int btn1, int btn2);  // Select pattern from 2-button combo
    double executeRhythmicPattern(int pattern, double beat, double beatsElapsed, int btn1, int btn2, int btn3);  // Execute pattern with button mixing
    
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EnhancedAudioStrip)
};

//==============================================================================
/**
 * ModernAudioEngine - Complete audio engine
 */
class ModernAudioEngine
{
public:
    static constexpr int MaxStrips = 6;
    static constexpr int MaxColumns = 16;
    static constexpr int MaxGroups = 4;
    static constexpr int MaxPatterns = 4;
    static constexpr int NumModSequencers = 3;
    static constexpr int ModSteps = 16;
    static constexpr int MaxModBars = 8;
    static constexpr int ModTotalSteps = ModSteps * MaxModBars;
    static constexpr int ModMaxStepSubdivisions = 16;

    enum class ModTarget
    {
        None = 0,
        Volume,
        Pan,
        Pitch,
        Speed,
        Cutoff,
        Resonance,
        GrainSize,
        GrainDensity,
        GrainPitch,
        GrainPitchJitter,
        GrainSpread,
        GrainJitter,
        GrainRandom,
        GrainArp,
        GrainCloud,
        GrainEmitter,
        GrainEnvelope,
        Retrigger,
        FilterFrequency = Cutoff
    };

    struct ModSequencerState
    {
        ModTarget target = ModTarget::None;
        bool bipolar = false;
        bool curveMode = true;
        float depth = 1.0f;
        int offset = 0;
        int lengthBars = 1;
        int editPage = 0;
        float smoothingMs = 0.0f;
        float curveBend = 0.0f;
        int curveShape = 0;
        bool pitchScaleQuantize = false;
        int pitchScale = 0;
        std::array<float, ModSteps> steps{};
        std::array<int, ModSteps> stepSubdivisions{};
        std::array<float, ModSteps> stepEndValues{};
        std::array<int, ModSteps> stepCurveShapes{};
    };

    enum class PitchScale
    {
        Chromatic = 0,
        Major,
        Minor,
        Dorian,
        PentatonicMinor
    };

    enum class ModCurveShape
    {
        Linear = 0,
        ExponentialUp,
        ExponentialDown,
        Sine,
        Square
    };
    
    ModernAudioEngine();
    
    void prepareToPlay(double sampleRate, int maxBlockSize);
    void processBlock(juce::AudioBuffer<float>& buffer, 
                     juce::MidiBuffer& midi,
                     const juce::AudioPlayHead::PositionInfo& positionInfo,
                     const std::array<juce::AudioBuffer<float>*, MaxStrips>* stripOutputs = nullptr);
    
    // Strip access
    EnhancedAudioStrip* getStrip(int index);
    bool loadSampleToStrip(int stripIndex, const juce::File& file);
    
    // Groups
    StripGroup* getGroup(int index);
    void assignStripToGroup(int stripIndex, int groupIndex);
    void enforceGroupExclusivity(int activeStripIndex, bool immediateStop = false);
    
    // Quantization
    void setQuantization(int division);
    void scheduleQuantizedTrigger(int stripIndex, int column, double currentPPQ);
    void triggerStripWithQuantization(int stripIndex, int column, bool useQuantize);
    bool hasPendingTrigger(int stripIndex) const { return quantizeClock.hasPendingTrigger(stripIndex); }
    void setMomentaryStutterActive(bool enabled);
    void setMomentaryStutterDivision(double beats);
    void setMomentaryStutterStartPpq(double ppq);
    void setMomentaryStutterStrip(int stripIndex, int column, bool enabled);
    void clearMomentaryStutterStrips();
    void clearPendingQuantizedTriggersForStrip(int stripIndex);
    
    // Pattern recording
    void startPatternRecording(int patternIndex);
    void stopPatternRecording(int patternIndex);
    void startPatternPlayback(int patternIndex) { playPattern(patternIndex); }
    void stopPatternPlayback(int patternIndex);
    void playPattern(int patternIndex);
    void clearPattern(int patternIndex);
    void stopPattern(int patternIndex);
    PatternRecorder* getPattern(int index);

    // Per-strip modulation sequencers.
    void setModSequencerSlot(int stripIndex, int slot);
    int getModSequencerSlot(int stripIndex) const;
    ModSequencerState getModSequencerState(int stripIndex) const;
    static bool modTargetSupportsBipolar(ModTarget target);
    void setModTarget(int stripIndex, ModTarget target);
    ModTarget getModTarget(int stripIndex) const;
    void setModBipolar(int stripIndex, bool bipolar);
    bool isModBipolar(int stripIndex) const;
    void setModDepth(int stripIndex, float depth);
    float getModDepth(int stripIndex) const;
    void setModCurveMode(int stripIndex, bool curveMode);
    bool isModCurveMode(int stripIndex) const;
    void setModOffset(int stripIndex, int offset);
    int getModOffset(int stripIndex) const;
    void setModStepValue(int stripIndex, int step, float value01);
    float getModStepValue(int stripIndex, int step) const;
    void setModStepValueAbsolute(int stripIndex, int absoluteStep, float value01);
    float getModStepValueAbsolute(int stripIndex, int absoluteStep) const;
    void setModStepShape(int stripIndex, int step, int subdivisions, float endValue01);
    void setModStepShapeAbsolute(int stripIndex, int absoluteStep, int subdivisions, float endValue01);
    int getModStepSubdivisionAbsolute(int stripIndex, int absoluteStep) const;
    float getModStepEndValueAbsolute(int stripIndex, int absoluteStep) const;
    void setModStepCurveShape(int stripIndex, int step, ModCurveShape shape);
    void setModStepCurveShapeAbsolute(int stripIndex, int absoluteStep, ModCurveShape shape);
    ModCurveShape getModStepCurveShapeAbsolute(int stripIndex, int absoluteStep) const;
    void toggleModStep(int stripIndex, int step);
    void clearModSteps(int stripIndex);
    int getModCurrentStep(int stripIndex) const;
    int getModCurrentPage(int stripIndex) const;
    int getModCurrentGlobalStep(int stripIndex) const;
    void setModLengthBars(int stripIndex, int bars);
    int getModLengthBars(int stripIndex) const;
    void setModEditPage(int stripIndex, int page);
    int getModEditPage(int stripIndex) const;
    void setModSmoothingMs(int stripIndex, float ms);
    float getModSmoothingMs(int stripIndex) const;
    void setModCurveBend(int stripIndex, float bend);
    float getModCurveBend(int stripIndex) const;
    void setModCurveShape(int stripIndex, ModCurveShape shape);
    ModCurveShape getModCurveShape(int stripIndex) const;
    void setModPitchScaleQuantize(int stripIndex, bool enabled);
    bool isModPitchScaleQuantize(int stripIndex) const;
    void setModPitchScale(int stripIndex, PitchScale scale);
    PitchScale getModPitchScale(int stripIndex) const;
    
    // Live recording - Continuous buffer
    void setRecordingLoopLength(int bars);  // Legacy - now per-strip
    int getRecordingLoopLength() const;     // Legacy - now per-strip
    void captureLoopToStrip(int stripIndex, int bars);  // Capture with specified length
    bool shouldBlinkRecordLED() const;  // For LED feedback
    void clearRecentInputBuffer();
    
    // Legacy live recording (kept for compatibility)
    void startLiveRecording(int stripIndex, int lengthBeats);
    void stopLiveRecording();
    
    // Master controls
    void setMasterVolume(float vol);
    float getMasterVolume() const { return masterVolume; }
    void setLimiterEnabled(bool enabled);
    bool isLimiterEnabled() const { return limiterEnabled.load(std::memory_order_acquire) != 0; }
    void setLimiterThresholdDb(float thresholdDb);
    float getLimiterThresholdDb() const { return limiterThresholdDb.load(std::memory_order_acquire); }
    
    void setPitchSmoothingTime(float seconds);  // 0 = no smoothing, 1.0 = 1 second for scratching
    float getPitchSmoothingTime() const { return pitchSmoothingTime; }
    
    void setInputMonitorVolume(float vol);  // 0 = off, 1.0 = full volume
    float getInputMonitorVolume() const { return inputMonitorVolume; }
    void setCrossfadeLengthMs(float ms);
    float getCrossfadeLengthMs() const { return crossfadeLengthMs.load(std::memory_order_acquire); }
    void setTriggerFadeInMs(float ms);
    float getTriggerFadeInMs() const { return triggerFadeInMs.load(std::memory_order_acquire); }
    void setGlobalSwingDivision(EnhancedAudioStrip::SwingDivision division);
    EnhancedAudioStrip::SwingDivision getGlobalSwingDivision() const;
    
    // Input metering
    float getInputLevelL() const { return inputLevelL.load(); }
    float getInputLevelR() const { return inputLevelR.load(); }
    
    // Current state
    double getCurrentTempo() const { return currentTempo; }
    double getCurrentBeat() const { return currentBeat; }
    double getTimelineBeat() const;
    double getBeatPhase() const { return beatPhase; }  // 0.0 to 1.0 within current beat
    int64_t getGlobalSampleCount() const { return globalSampleCount; }
    
private:
    struct ModSequencer
    {
        std::atomic<int> target{static_cast<int>(ModTarget::None)};
        std::atomic<int> bipolar{0};
        std::atomic<int> curveMode{0};
        std::atomic<float> depth{1.0f};
        std::atomic<int> offset{0};
        std::atomic<int> lengthBars{1};
        std::atomic<int> editPage{0};
        std::atomic<float> smoothingMs{0.0f};
        std::atomic<float> curveBend{0.0f};
        std::atomic<int> curveShape{static_cast<int>(ModCurveShape::Linear)};
        std::atomic<int> pitchScaleQuantize{0};
        std::atomic<int> pitchScale{static_cast<int>(PitchScale::Chromatic)};
        std::array<std::atomic<float>, ModTotalSteps> steps;
        std::array<std::atomic<int>, ModTotalSteps> stepSubdivisions;
        std::array<std::atomic<float>, ModTotalSteps> stepEndValues;
        std::array<std::atomic<int>, ModTotalSteps> stepCurveShapes;
        float smoothedRaw = 0.0f;
        float grainDezipperedRaw = 0.0f;
        float pitchDezipperedRaw = 0.0f;
        std::atomic<int> lastGlobalStep{0};
    };

    int sanitizeModSequencerSlot(int slot) const;
    int getActiveModSequencerSlot(int stripIndex) const;
    ModSequencer& getActiveModSequencer(int stripIndex);
    const ModSequencer& getActiveModSequencer(int stripIndex) const;
    ModSequencer& getModSequencer(int stripIndex, int slot);
    const ModSequencer& getModSequencer(int stripIndex, int slot) const;

    std::array<std::unique_ptr<EnhancedAudioStrip>, MaxStrips> strips;
    std::array<std::unique_ptr<StripGroup>, MaxGroups> groups;
    std::array<std::unique_ptr<PatternRecorder>, 4> patterns;
    std::array<std::array<ModSequencer, NumModSequencers>, MaxStrips> modSequencers;
    std::array<std::atomic<int>, MaxStrips> activeModSequencerSlots{};
    std::atomic<int> momentaryStutterActive{0};
    std::atomic<double> momentaryStutterDivisionBeats{0.5}; // quarter-note units
    std::atomic<double> momentaryStutterStartPpq{0.0};
    std::array<std::atomic<int>, MaxStrips> momentaryStutterStripEnabled{};
    std::array<std::atomic<int>, MaxStrips> momentaryStutterColumns{};
    std::array<std::atomic<double>, MaxStrips> momentaryStutterNextPpq{};
    std::unique_ptr<LiveRecorder> liveRecorder;
    
    QuantizationClock quantizeClock;
    
    std::atomic<float> masterVolume{1.0f};
    std::atomic<int> limiterEnabled{0};
    std::atomic<float> limiterThresholdDb{0.0f}; // 0 dB = transparent until over 0 dBFS
    std::array<juce::dsp::Limiter<float>, MaxStrips + 1> outputLimiterL{};
    std::array<juce::dsp::Limiter<float>, MaxStrips + 1> outputLimiterR{};
    std::atomic<float> pitchSmoothingTime{0.05f};  // Default 50ms, range 0-1.0 seconds
    std::atomic<float> inputMonitorVolume{0.0f};  // Default off, range 0-1.0
    std::atomic<float> inputLevelL{0.0f};  // Input meter level left (0-1)
    std::atomic<float> inputLevelR{0.0f};  // Input meter level right (0-1)
    std::atomic<float> crossfadeLengthMs{10.0f}; // Inner-loop and capture-loop crossfade (ms)
    std::atomic<float> triggerFadeInMs{12.0f}; // Trigger fade-in de-click time (ms)
    
    std::atomic<double> currentTempo{120.0};
    std::atomic<int> currentTimeSigNumerator{4};
    std::atomic<int> currentTimeSigDenominator{4};
    std::atomic<double> currentBeat{0.0};
    std::atomic<double> beatPhase{0.0};  // 0.0-1.0 within current beat for sync
    std::atomic<double> lastKnownPPQ{0.0};  // Last PPQ from processBlock (for quantize outside audio thread)
    std::atomic<bool> hasLastKnownPPQ{false};
    std::atomic<int64_t> globalSampleCount{0};  // Total samples processed for sample-accurate sync
    double lastPatternProcessBeat = -1.0;
    
    double currentSampleRate = 44100.0;
    int currentBlockSize = 512;
    juce::AudioBuffer<float> inputMonitorScratch;
    
    void updateTempo(const juce::AudioPlayHead::PositionInfo& positionInfo);
    void advanceBeat(int numSamples, bool hasHostPpq);
    void processPatterns();
    
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ModernAudioEngine)
};
