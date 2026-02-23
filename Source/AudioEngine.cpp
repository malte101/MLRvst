/*
  ==============================================================================

    AudioEngine.cpp
    Implementation of modern audio engine

  ==============================================================================
*/

#include "AudioEngine.h"
#include "StilsonModel.h"
#include "HuovilainenModel.h"
#include <random>
#include <map>
#include <cmath>
#include <algorithm>
#include <vector>

namespace
{
constexpr double kMaxScratchRateAbs = 2.5;
constexpr double kMaxPatternRateAbs = 4.0;
constexpr double kForwardScratchDecay = 7.0;
constexpr double kReverseScratchAccelExp = 1.6;
constexpr int kMinGrainWindowSamples = 32;
constexpr float kGrainMinSizeMs = 5.0f;
constexpr float kGrainMaxSizeMs = 2400.0f;
constexpr float kGrainMinDensity = 0.05f;
constexpr float kGrainMaxDensity = 0.9f;

constexpr int kModScaleSize = 12;
constexpr std::array<int, kModScaleSize> kScaleChromatic{{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}};
constexpr std::array<int, 7> kScaleMajor{{0, 2, 4, 5, 7, 9, 11}};
constexpr std::array<int, 7> kScaleMinor{{0, 2, 3, 5, 7, 8, 10}};
constexpr std::array<int, 7> kScaleDorian{{0, 2, 3, 5, 7, 9, 10}};
constexpr std::array<int, 5> kScalePentMinor{{0, 3, 5, 7, 10}};

bool modTargetAutoDefaultBipolar(ModernAudioEngine::ModTarget target)
{
    return target == ModernAudioEngine::ModTarget::Pan
        || target == ModernAudioEngine::ModTarget::Pitch
        || target == ModernAudioEngine::ModTarget::GrainPitch
        || target == ModernAudioEngine::ModTarget::GrainSize;
}

bool isGrainModTarget(ModernAudioEngine::ModTarget target)
{
    return target == ModernAudioEngine::ModTarget::GrainSize
        || target == ModernAudioEngine::ModTarget::GrainDensity
        || target == ModernAudioEngine::ModTarget::GrainPitch
        || target == ModernAudioEngine::ModTarget::GrainPitchJitter
        || target == ModernAudioEngine::ModTarget::GrainSpread
        || target == ModernAudioEngine::ModTarget::GrainJitter
        || target == ModernAudioEngine::ModTarget::GrainRandom
        || target == ModernAudioEngine::ModTarget::GrainArp
        || target == ModernAudioEngine::ModTarget::GrainCloud
        || target == ModernAudioEngine::ModTarget::GrainEmitter
        || target == ModernAudioEngine::ModTarget::GrainEnvelope;
}

double grainScratchSecondsFromAmount(float amountPercent)
{
    const float clamped = juce::jlimit(0.0f, 100.0f, amountPercent);
    if (clamped <= 0.0001f)
        return 0.0;
    const double t = static_cast<double>(clamped) / 100.0;
    // Fast near-zero, expanded high range up to 3s.
    return juce::jlimit(0.015, 3.0, std::pow(t, 1.7) * 3.0);
}

template <size_t N>
int quantizeSemitoneToScaleImpl(int semitone, const std::array<int, N>& scale)
{
    int best = semitone;
    int bestDist = std::numeric_limits<int>::max();
    for (int octave = -3; octave <= 3; ++octave)
    {
        const int base = octave * 12;
        for (int degree : scale)
        {
            const int candidate = base + degree;
            const int dist = std::abs(candidate - semitone);
            if (dist < bestDist)
            {
                bestDist = dist;
                best = candidate;
            }
        }
    }
    return best;
}

float quantizePitchDeltaToScale(float semitoneDelta, ModernAudioEngine::PitchScale scale)
{
    const int semitone = static_cast<int>(std::round(semitoneDelta));
    switch (scale)
    {
        case ModernAudioEngine::PitchScale::Major:
            return static_cast<float>(quantizeSemitoneToScaleImpl(semitone, kScaleMajor));
        case ModernAudioEngine::PitchScale::Minor:
            return static_cast<float>(quantizeSemitoneToScaleImpl(semitone, kScaleMinor));
        case ModernAudioEngine::PitchScale::Dorian:
            return static_cast<float>(quantizeSemitoneToScaleImpl(semitone, kScaleDorian));
        case ModernAudioEngine::PitchScale::PentatonicMinor:
            return static_cast<float>(quantizeSemitoneToScaleImpl(semitone, kScalePentMinor));
        case ModernAudioEngine::PitchScale::Chromatic:
        default:
            return static_cast<float>(quantizeSemitoneToScaleImpl(semitone, kScaleChromatic));
    }
}

float quantizeSpeedRatioMusical(float unit)
{
    static constexpr std::array<float, 7> kMusicalRatios {
        0.5f,          // half-time
        2.0f / 3.0f,   // triplet feel
        0.75f,         // dotted relation
        1.0f,          // unison
        4.0f / 3.0f,   // triplet counterpart
        1.5f,          // dotted relation
        2.0f           // double-time
    };

    const float clamped = juce::jlimit(0.0f, 1.0f, unit);
    const int idx = juce::jlimit(0, static_cast<int>(kMusicalRatios.size()) - 1,
                                 static_cast<int>(std::round(clamped * static_cast<float>(kMusicalRatios.size() - 1))));
    return kMusicalRatios[static_cast<size_t>(idx)];
}

double retriggerDivisionFromAmount(float amount01)
{
    const float v = juce::jlimit(0.0f, 1.0f, amount01);
    if (v <= 1.0e-4f) return 0.0;   // 0 = disabled
    // Lower half has wider "musical" zones for easier selection.
    // divisions are expressed in quarter-note units:
    // 2.0=1/2, 1.0=1/4, 0.5=1/8, 0.25=1/16, 0.125=1/32, 0.0625=1/64
    if (v < 0.125f) return 2.0;     // 1/2
    if (v < 0.250f) return 1.0;     // 1/4
    if (v < 0.375f) return 0.5;     // 1/8
    if (v < 0.500f) return 0.25;    // 1/16
    if (v < 0.750f) return 0.125;   // 1/32
    return 0.0625;                  // 1/64
}

float shapeModCurvePhase(float phase01, float bend, ModernAudioEngine::ModCurveShape shape)
{
    const float t = juce::jlimit(0.0f, 1.0f, phase01);
    const float b = juce::jlimit(-1.0f, 1.0f, bend);
    const float amount = std::abs(b);

    switch (shape)
    {
        case ModernAudioEngine::ModCurveShape::SCurve:
        {
            const float blend = juce::jmap(amount, 0.0f, 0.95f);
            float s = (t * t) * (3.0f - (2.0f * t));
            if (b >= 0.0f)
                s = std::pow(s, juce::jmap(amount, 1.0f, 5.5f));
            else
                s = 1.0f - std::pow(1.0f - s, juce::jmap(amount, 1.0f, 5.5f));
            return juce::jlimit(0.0f, 1.0f, juce::jmap(blend, t, s));
        }
        case ModernAudioEngine::ModCurveShape::Snap:
        {
            const float exp = juce::jmap(amount, 1.0f, 10.0f);
            return b >= 0.0f ? std::pow(t, exp) : (1.0f - std::pow(1.0f - t, exp));
        }
        case ModernAudioEngine::ModCurveShape::Stair:
        {
            const int steps = juce::jlimit(3, 24, static_cast<int>(std::round(3.0f + (amount * 21.0f))));
            const float q = std::round(t * static_cast<float>(steps)) / static_cast<float>(steps);
            return b >= 0.0f ? q : (1.0f - q);
        }
        case ModernAudioEngine::ModCurveShape::Power:
        default:
        {
            const float exp = juce::jmap(amount, 1.0f, 7.0f);
            return b >= 0.0f ? std::pow(t, exp) : (1.0f - std::pow(1.0f - t, exp));
        }
    }
}

inline float safetyClip0dB(float sample)
{
    return juce::jlimit(-1.0f, 1.0f, sample);
}

inline float filterLimiter0dB(float sample, float resonanceDrive)
{
    const float drive = juce::jlimit(0.0f, 1.0f, resonanceDrive);
    const float threshold = juce::jmap(drive, 0.96f, 0.86f);
    const float ceiling = 0.992f; // Keep a little headroom below 0 dBFS.

    const float mag = std::abs(sample);
    if (mag <= threshold)
        return juce::jlimit(-ceiling, ceiling, sample);

    const float over = mag - threshold;
    const float ratioControl = 10.0f + (22.0f * drive);
    const float compressed = threshold + (over / (1.0f + (ratioControl * over)));

    const float normalized = juce::jlimit(0.0f, 1.25f, compressed / ceiling);
    const float shaped = std::tanh(normalized * (1.15f + (0.95f * drive)));
    const float limited = juce::jmin(ceiling, shaped * ceiling);
    return std::copysign(limited, sample);
}

}

//==============================================================================
// Resampler Implementation
//==============================================================================

Resampler::Resampler()
{
}

float Resampler::getSample(const juce::AudioBuffer<float>& buffer, 
                          int channel, 
                          double position,
                          double speed)
{
    (void)speed;
    const int numSamples = buffer.getNumSamples();
    if (numSamples == 0 || channel >= buffer.getNumChannels())
        return 0.0f;
    
    const float* data = buffer.getReadPointer(channel);
    
    // Clamp position
    while (position < 0) position += numSamples;
    while (position >= numSamples) position -= numSamples;
    
    int index = (int)position;
    float frac = (float)(position - index);
    
    switch (quality)
    {
        case Quality::Linear:
        {
            int next = (index + 1) % numSamples;
            return linearInterpolate(data[index], data[next], frac);
        }
        
        case Quality::Cubic:
        {
            int i0 = (index - 1 + numSamples) % numSamples;
            int i1 = index;
            int i2 = (index + 1) % numSamples;
            int i3 = (index + 2) % numSamples;
            return cubicInterpolate(data[i0], data[i1], data[i2], data[i3], frac);
        }
        
        case Quality::Sinc:
        {
            return sincInterpolate(data, numSamples, position, 8);
        }

        case Quality::SincHQ:
        {
            return sincInterpolate(data, numSamples, position, 16);
        }
    }
    
    return data[index];
}

float Resampler::linearInterpolate(float a, float b, float t)
{
    return a + t * (b - a);
}

float Resampler::cubicInterpolate(float y0, float y1, float y2, float y3, float t)
{
    float a0 = y3 - y2 - y0 + y1;
    float a1 = y0 - y1 - a0;
    float a2 = y2 - y0;
    float a3 = y1;
    
    return a0 * t * t * t + a1 * t * t + a2 * t + a3;
}

float Resampler::sincInterpolate(const float* data, int length, double position, int taps)
{
    // Windowed sinc interpolation
    const int windowSize = juce::jmax(2, taps);
    float sum = 0.0f;
    float norm = 0.0f;
    int center = (int)position;
    float frac = static_cast<float>(position - center);
    
    for (int i = -windowSize; i <= windowSize; ++i)
    {
        int index = center + i;
        if (index < 0 || index >= length)
            continue;
        
        float x = juce::MathConstants<float>::pi * (frac - i);
        float sinc = (x == 0.0f) ? 1.0f : std::sin(x) / x;
        
        // Hamming window
        const float phase = static_cast<float>(i) / static_cast<float>(windowSize);
        const float window = 0.42f + (0.5f * std::cos(juce::MathConstants<float>::pi * phase))
                                   + (0.08f * std::cos(2.0f * juce::MathConstants<float>::pi * phase));

        const float weight = sinc * window;
        sum += data[index] * weight;
        norm += weight;
    }

    if (std::abs(norm) > 1.0e-6f)
        return sum / norm;
    return sum;
}

//==============================================================================
// Crossfader Implementation
//==============================================================================

Crossfader::Crossfader()
{
}

void Crossfader::reset(int sampleRate)
{
    (void)sampleRate;
    active = false;
    currentGain = 1.0f;
}

void Crossfader::startFade(bool fadeIn, int numSamples, bool forceRestartFromEdge)
{
    if (numSamples < 0)
        numSamples = 256; // Default fade length
    
    targetGain = fadeIn ? 1.0f : 0.0f;
    
    float startGain = currentGain.load();

    // Row retriggers should always ramp from an edge so the trigger-fade
    // time remains audible and deterministic at every retrigger.
    if (forceRestartFromEdge)
    {
        startGain = fadeIn ? 0.0f : 1.0f;
    }
    else if (!active)
    {
        startGain = fadeIn ? 0.0f : 1.0f;
    }
    
    currentGain = startGain;
    totalSamples = numSamples;
    samplesRemaining = numSamples;
    fadeDirection = fadeIn ? 1.0f : -1.0f;
    active = true;
}

float Crossfader::getNextValue()
{
    if (!active)
        return 1.0f;
    
    if (samplesRemaining <= 0)
    {
        currentGain = targetGain;
        active = false;
        return currentGain;
    }
    
    // Equal power fade using sine/cosine curve
    // Ratio: 0.0 (start) to 1.0 (end)
    float ratio = 1.0f - (samplesRemaining / (float)totalSamples);
    
    float value;
    if (fadeDirection > 0.0f)  // Fade IN
    {
        // sin(0) = 0, sin(π/2) = 1
        value = std::sin(ratio * 1.57079632679f);  // π/2
    }
    else  // Fade OUT
    {
        // cos(0) = 1, cos(π/2) = 0
        value = std::cos(ratio * 1.57079632679f);  // π/2
    }
    
    --samplesRemaining;
    currentGain = value;
    
    return value;
}

//==============================================================================
// QuantizationClock Implementation
//==============================================================================

QuantizationClock::QuantizationClock()
{
}

void QuantizationClock::setTempo(double bpm)
{
    tempo = juce::jmax(20.0, juce::jmin(300.0, bpm));
}

void QuantizationClock::setQuantization(int division)
{
    quantizeDivision = juce::jmax(1, division);
}

void QuantizationClock::setSampleRate(double sr)
{
    sampleRate = sr;
}

void QuantizationClock::reset()
{
    const juce::SpinLock::ScopedLockType lock(pendingTriggersLock);
    currentSample.store(0, std::memory_order_release);
    pendingTriggers.clear();
}

void QuantizationClock::advance(int numSamples)
{
    currentSample.fetch_add(numSamples, std::memory_order_acq_rel);
}

int QuantizationClock::getQuantSamples() const
{
    // quantBeats = divisions per bar / 4 (since 4 beats per bar)
    // e.g. division 4 = 1/4 note = 1.0 beat
    //      division 8 = 1/8 note = 0.5 beat
    //      division 16 = 1/16 note = 0.25 beat
    double quantBeats = 4.0 / quantizeDivision;
    double secondsPerBeat = 60.0 / tempo;
    double seconds = secondsPerBeat * quantBeats;
    return juce::jmax(1, static_cast<int>(seconds * sampleRate));
}

// PPQ-based scheduling
void QuantizationClock::scheduleTrigger(int stripIndex, int column, double ppq, EnhancedAudioStrip* strip)
{
    (void)strip;
    // Calculate next quantize grid in PPQ
    double quantBeats = getQuantBeats();
    
    // CRITICAL: Round ppq to nearest grid point to ensure we snap to master clock
    // This ensures grids are ALWAYS at 0, 0.5, 1.0, 1.5, 2.0... for 1/8 notes
    // Not arbitrary offsets like 0.3, 0.8, 1.3...
    double gridNumber = std::ceil(ppq / quantBeats);
    double nextGridPPQ = gridNumber * quantBeats;
    
    // Snap to a clean grid by rounding to avoid floating point drift
    // For 1/8 notes (0.5 beats), snap to 0.0, 0.5, 1.0, 1.5, 2.0...
    nextGridPPQ = std::round(nextGridPPQ / quantBeats) * quantBeats;
    
    bool gateClosed = false;
    QuantisedTrigger existingTrigger;
    int64_t currentSampleSnapshot = 0;

    {
        const juce::SpinLock::ScopedLockType lock(pendingTriggersLock);
        for (const auto& t : pendingTriggers)
        {
            if (t.stripIndex == stripIndex)
            {
                gateClosed = true;
                existingTrigger = t;
                break;
            }
        }
        currentSampleSnapshot = currentSample.load(std::memory_order_acquire);
    }

    if (gateClosed)
    {
        juce::ignoreUnused(existingTrigger);
        return;
    }
    
    // GATE IS OPEN - No pending trigger, schedule this one
    // CRITICAL: Calculate target sample using ABSOLUTE PPQ timeline
    // This ensures sample-accurate sync without drift
    //
    // Formula: targetSample = targetPPQ × (samplesPerQuarter)
    // where samplesPerQuarter = (60.0 / BPM) × sampleRate
    //
    // We calculate ABSOLUTE sample positions for both current and target PPQ,
    // then find the difference to get samples-to-wait
    
    double samplesPerQuarter = (60.0 / tempo) * sampleRate;
    
    // ABSOLUTE sample positions based on PPQ timeline
    int64_t currentAbsSample = static_cast<int64_t>(ppq * samplesPerQuarter);
    int64_t targetAbsSample = static_cast<int64_t>(nextGridPPQ * samplesPerQuarter);
    
    // Samples to wait = difference in absolute positions
    int64_t samplesToWait = targetAbsSample - currentAbsSample;
    
    // Our target in the audio thread's sample counter space
    // globalSampleCount tracks absolute samples since transport start
    int64_t targetSample = currentSampleSnapshot + samplesToWait;
    
    QuantisedTrigger t;
    t.targetSample = targetSample;
    t.targetPPQ = nextGridPPQ;  // Store exact grid PPQ for debugging
    t.stripIndex = stripIndex;
    t.column = column;
    
    // Keep triggers sorted by target sample so event extraction is linear-time.
    const juce::SpinLock::ScopedLockType lock(pendingTriggersLock);
    for (const auto& existing : pendingTriggers)
    {
        if (existing.stripIndex == stripIndex)
            return;
    }
    auto insertPos = std::upper_bound(
        pendingTriggers.begin(),
        pendingTriggers.end(),
        t.targetSample,
        [](int64_t sample, const QuantisedTrigger& trigger)
        {
            return sample < trigger.targetSample;
        });
    pendingTriggers.insert(insertPos, t);
}

void QuantizationClock::updateFromPPQ(double ppq, int numSamples)
{
    const juce::SpinLock::ScopedLockType lock(pendingTriggersLock);
    currentPPQ = ppq;
    currentSample.fetch_add(numSamples, std::memory_order_acq_rel);
}

double QuantizationClock::getQuantBeats() const
{
    // quantizeDivision = divisions per bar (e.g. 8 = 1/8 notes)
    // Return beats per quantize point
    // e.g. division 4 = 1/4 note = 1.0 beat
    //      division 8 = 1/8 note = 0.5 beat
    //      division 16 = 1/16 note = 0.25 beat
    return 4.0 / quantizeDivision;
}

bool QuantizationClock::hasPendingTrigger(int stripIndex) const
{
    const juce::SpinLock::ScopedLockType lock(pendingTriggersLock);
    for (const auto& t : pendingTriggers)
    {
        if (t.stripIndex == stripIndex)
            return true;
    }
    return false;
}

void QuantizationClock::clearPendingTriggers()
{
    const juce::SpinLock::ScopedLockType lock(pendingTriggersLock);
    pendingTriggers.clear();
}

void QuantizationClock::clearPendingTriggersForStrip(int stripIndex)
{
    const juce::SpinLock::ScopedLockType lock(pendingTriggersLock);
    // Remove all pending triggers for this strip
    // This prevents multiple triggers from firing when rapid presses
    // scheduled triggers before the gate closed
    pendingTriggers.erase(
        std::remove_if(pendingTriggers.begin(), pendingTriggers.end(),
            [stripIndex](const QuantisedTrigger& t) {
                return t.stripIndex == stripIndex;
            }),
        pendingTriggers.end()
    );
}

std::vector<QuantisedTrigger> QuantizationClock::getEventsInRange(int64_t blockStart, int64_t blockEnd)
{
    (void)blockStart;
    const juce::SpinLock::ScopedLockType lock(pendingTriggersLock);
    std::vector<QuantisedTrigger> eventsInRange;

    // pendingTriggers is maintained sorted by targetSample.
    // Consume all events before blockEnd in one prefix erase.
    auto firstFuture = std::lower_bound(
        pendingTriggers.begin(),
        pendingTriggers.end(),
        blockEnd,
        [](const QuantisedTrigger& trigger, int64_t sample)
        {
            return trigger.targetSample < sample;
        });

    eventsInRange.assign(pendingTriggers.begin(), firstFuture);
    pendingTriggers.erase(pendingTriggers.begin(), firstFuture);

    return eventsInRange;
}

//==============================================================================
// PatternRecorder Implementation
//==============================================================================

PatternRecorder::PatternRecorder()
{
}

void PatternRecorder::setLength(int beats)
{
    lengthInBeats.store(juce::jmax(1, beats), std::memory_order_release);
}

void PatternRecorder::startRecording(double currentBeat)
{
    juce::ScopedLock lock(recordLock);
    events.clear();
    
    // Quantize start to next beat boundary
    double startBeat = std::ceil(currentBeat);
    int length = lengthInBeats.load(std::memory_order_acquire);
    double endBeat = startBeat + length;
    
    recordingStartBeat.store(startBeat, std::memory_order_release);
    recordingEndBeat.store(endBeat, std::memory_order_release);
    recording.store(true, std::memory_order_release);
    playing.store(false, std::memory_order_release);
    
    DBG("Pattern recording scheduled: start=" << startBeat << ", end=" << endBeat << ", length=" << length << " beats");
}

void PatternRecorder::stopRecording()
{
    recording.store(false, std::memory_order_release);
    
    // Sort events by time for efficient playback
    juce::ScopedLock lock(recordLock);
    std::sort(events.begin(), events.end());
    
    DBG("Pattern recording stopped. Total events: " << events.size());
}

void PatternRecorder::startPlayback()
{
    double startBeat = recordingStartBeat.load(std::memory_order_acquire);
    if (startBeat < 0.0)
        startBeat = 0.0;
    startPlayback(startBeat);
}

void PatternRecorder::startPlayback(double currentBeat)
{
    double startBeat = std::ceil(currentBeat);
    playbackStartBeat.store(startBeat, std::memory_order_release);
    playbackPosition.store(0.0, std::memory_order_release);  // Reset to start
    lastProcessedBeat.store(startBeat, std::memory_order_release);
    playing.store(true, std::memory_order_release);
}

void PatternRecorder::stopPlayback()
{
    playing.store(false, std::memory_order_release);
    playbackPosition.store(0.0, std::memory_order_release);
    playbackStartBeat.store(-1.0, std::memory_order_release);
}

void PatternRecorder::advancePlayback(double beatDelta)
{
    if (!playing.load(std::memory_order_acquire))
        return;
        
    double pos = playbackPosition.load(std::memory_order_acquire);
    int length = lengthInBeats.load(std::memory_order_acquire);
    
    pos += beatDelta;
    
    // Loop at pattern length
    while (pos >= static_cast<double>(length))
        pos -= static_cast<double>(length);
        
    playbackPosition.store(pos, std::memory_order_release);
}

// Template implementation for processEventsInTimeSlice
template<typename Callback>
void PatternRecorder::processEventsInTimeSlice(double beatDelta, Callback&& callback)
{
    if (!playing.load(std::memory_order_acquire) || events.empty())
        return;
        
    double currentPos = playbackPosition.load(std::memory_order_acquire);
    double lastPos = currentPos - beatDelta;
    int length = lengthInBeats.load(std::memory_order_acquire);
    
    // Handle negative wrap
    if (lastPos < 0.0)
        lastPos += static_cast<double>(length);
        
    // Process events between lastPos and currentPos
    processEventsInRange(lastPos, currentPos, std::forward<Callback>(callback));
}

void PatternRecorder::clear()
{
    juce::ScopedLock lock(recordLock);
    events.clear();
    recording.store(false, std::memory_order_release);
    playing.store(false, std::memory_order_release);
    recordingStartBeat.store(-1.0, std::memory_order_release);
    recordingEndBeat.store(-1.0, std::memory_order_release);
}

std::vector<PatternRecorder::Event> PatternRecorder::getEventsSnapshot() const
{
    juce::ScopedLock lock(recordLock);
    return events;
}

void PatternRecorder::setEventsSnapshot(const std::vector<Event>& newEvents, int lengthBeats)
{
    juce::ScopedLock lock(recordLock);
    events = newEvents;
    std::sort(events.begin(), events.end());
    lengthInBeats.store(juce::jmax(1, lengthBeats), std::memory_order_release);
    recording.store(false, std::memory_order_release);
    playing.store(false, std::memory_order_release);
    playbackPosition.store(0.0, std::memory_order_release);
    playbackStartBeat.store(-1.0, std::memory_order_release);
    lastProcessedBeat.store(-1.0, std::memory_order_release);
}

bool PatternRecorder::updateRecording(double currentBeat)
{
    if (!recording.load(std::memory_order_acquire))
        return false;
    
    double endBeat = recordingEndBeat.load(std::memory_order_acquire);
    
    // Check if we've reached the end of recording
    if (currentBeat >= endBeat)
    {
        DBG("Pattern auto-stopped at beat " << currentBeat << " (end was " << endBeat << ")");
        stopRecording();
        startPlayback(currentBeat);  // Auto-start playback on next beat
        return true;
    }
    
    return false;
}

void PatternRecorder::recordEvent(int strip, int column, bool noteOn, double currentBeat)
{
    if (!recording.load(std::memory_order_acquire))
        return;
    
    double startBeat = recordingStartBeat.load(std::memory_order_acquire);
    double endBeat = recordingEndBeat.load(std::memory_order_acquire);
    
    // Only record if we're within the recording window
    if (currentBeat < startBeat || currentBeat >= endBeat)
        return;
    
    juce::ScopedLock lock(recordLock);
    
    Event event;
    event.stripIndex = strip;
    event.column = column;
    // Time is relative to pattern start (0 to lengthInBeats)
    event.time = currentBeat - startBeat;
    event.isNoteOn = noteOn;
    
    events.push_back(event);
    
    DBG("Event recorded: strip=" << strip << ", col=" << column << ", beat=" << event.time);
}

// Template implementation must be in header or here
template<typename Callback>
void PatternRecorder::processEventsInRange(double fromBeat, double toBeat, Callback&& callback) const
{
    if (!playing.load(std::memory_order_acquire) || events.empty())
        return;
    
    int length = lengthInBeats.load(std::memory_order_acquire);
    
    // Normalize beats to pattern length
    fromBeat = std::fmod(fromBeat, static_cast<double>(length));
    toBeat = std::fmod(toBeat, static_cast<double>(length));
    
    // Handle wrap-around (when range crosses pattern boundary)
    if (fromBeat > toBeat)
    {
        // Process from fromBeat to end
        for (const auto& event : events)
        {
            if (event.time >= fromBeat)
                callback(event);
        }
        // Process from start to toBeat
        for (const auto& event : events)
        {
            if (event.time < toBeat)
                callback(event);
            else
                break;  // Events are sorted, can stop early
        }
    }
    else
    {
        // Normal range - use binary search for efficiency
        // Find first event >= fromBeat
        auto start = std::lower_bound(events.begin(), events.end(), fromBeat,
            [](const Event& e, double beat) { return e.time < beat; });
        
        // Process events in range
        for (auto it = start; it != events.end() && it->time < toBeat; ++it)
        {
            callback(*it);
        }
    }
}

template<typename Callback>
void PatternRecorder::processEventsForBeatWindow(double fromBeat, double toBeat, Callback&& callback) const
{
    if (!playing.load(std::memory_order_acquire) || events.empty())
        return;

    if (!std::isfinite(fromBeat) || !std::isfinite(toBeat) || toBeat <= fromBeat)
        return;

    const int length = lengthInBeats.load(std::memory_order_acquire);
    if (length <= 0)
        return;

    const double anchor = playbackStartBeat.load(std::memory_order_acquire);
    if (anchor < 0.0)
        return;

    // Pattern should not fire before its scheduled playback start.
    const double windowStart = juce::jmax(fromBeat, anchor);
    const double windowEnd = toBeat;
    if (windowEnd <= windowStart)
        return;

    const double span = windowEnd - windowStart;
    const double loopLen = static_cast<double>(length);

    // Transport jumps can create huge windows; resync instead of burst-firing many loops.
    if (span > (loopLen * 2.0))
        return;

    const double relFrom = windowStart - anchor;
    const double relTo = windowEnd - anchor;
    const int startCycle = static_cast<int>(std::floor(relFrom / loopLen));
    const int endCycle = static_cast<int>(std::floor((relTo - 1.0e-9) / loopLen));

    for (int cycle = startCycle; cycle <= endCycle; ++cycle)
    {
        const double cycleStart = static_cast<double>(cycle) * loopLen;
        double localFrom = relFrom - cycleStart;
        double localTo = relTo - cycleStart;

        if (cycle != startCycle)
            localFrom = 0.0;
        if (cycle != endCycle)
            localTo = loopLen;

        localFrom = juce::jlimit(0.0, loopLen, localFrom);
        localTo = juce::jlimit(0.0, loopLen, localTo);

        if (localTo <= localFrom)
            continue;

        processEventsInRange(localFrom, localTo, std::forward<Callback>(callback));
    }
}

//==============================================================================
// LiveRecorder Implementation
//==============================================================================

LiveRecorder::LiveRecorder()
    : selectedBars(1)
{
}

void LiveRecorder::prepareToPlay(double sampleRate, int maxBlockSize)
{
    (void) maxBlockSize;
    currentSampleRate = sampleRate;
    
    // Circular buffer: Must hold 8 bars at 60 BPM (slowest realistic tempo)
    // At 60 BPM: 1 beat = 1 second, 8 bars = 32 beats = 32 seconds
    // Calculate based on actual sample rate with safety margin
    // 32 seconds * sampleRate * 1.5 (safety margin) = buffer size
    int circularBufferSize = static_cast<int>(32.0 * sampleRate * 1.5);
    
    // Safety limits (prevent excessive memory at extreme sample rates)
    circularBufferSize = juce::jlimit(2000000, 8000000, circularBufferSize);
    
    juce::ScopedLock lock(bufferLock);
    circularBuffer.setSize(2, circularBufferSize, false, true, false);
    circularBuffer.clear();
    
    writeHead = 0;
}

void LiveRecorder::setLoopLength(int bars)
{
    selectedBars = juce::jlimit(1, 8, bars);
}

int LiveRecorder::getSelectedLoopLength() const
{
    return selectedBars;
}

void LiveRecorder::setCrossfadeLengthMs(float ms)
{
    crossfadeLengthMs = juce::jlimit(1.0f, 50.0f, ms);
}

void LiveRecorder::startRecording(int lengthInBeats, double tempo)
{
    (void) lengthInBeats;
    (void) tempo;
    // Legacy method - not used with continuous buffer
    recording = false;
}

void LiveRecorder::stopRecording()
{
    recording = false;
}

void LiveRecorder::processInput(const juce::AudioBuffer<float>& input, 
                                int startSample, 
                                int numSamples)
{
    // ALWAYS recording to circular buffer
    juce::ScopedLock lock(bufferLock);
    
    int bufferSize = circularBuffer.getNumSamples();
    if (bufferSize == 0)
        return;
    
    int writePos = writeHead.load();
    int inputChannels = input.getNumChannels();
    
    for (int i = 0; i < numSamples; ++i)
    {
        int writeIndex = (writePos + i) % bufferSize;
        
        if (inputChannels == 1)
        {
            // Mono input: copy channel 0 to BOTH stereo channels
            float monoSample = input.getSample(0, startSample + i);
            circularBuffer.setSample(0, writeIndex, monoSample);
            circularBuffer.setSample(1, writeIndex, monoSample);
        }
        else
        {
            // Stereo or multi-channel input: copy each channel
            for (int ch = 0; ch < juce::jmin(inputChannels, circularBuffer.getNumChannels()); ++ch)
            {
                circularBuffer.setSample(ch, writeIndex, input.getSample(ch, startSample + i));
            }
        }
    }
    
    writeHead = (writePos + numSamples) % bufferSize;
}

void LiveRecorder::clearBuffer()
{
    juce::ScopedLock lock(bufferLock);
    circularBuffer.clear();
    writeHead.store(0, std::memory_order_release);
}

juce::AudioBuffer<float> LiveRecorder::captureLoop(double tempo, int bars)
{
    juce::ScopedLock lock(bufferLock);
    
    // Calculate loop length in samples based on passed bars parameter
    double beatsPerBar = 4.0;
    double bpm = tempo;
    double samplesPerBeat = (60.0 / bpm) * currentSampleRate;
    int loopLengthSamples = static_cast<int>(samplesPerBeat * beatsPerBar * bars);
    
    // Safety check
    if (loopLengthSamples <= 0 || loopLengthSamples > circularBuffer.getNumSamples())
        loopLengthSamples = juce::jmin(static_cast<int>(currentSampleRate * 4.0), circularBuffer.getNumSamples());
    
    // Calculate crossfade length
    int crossfadeSamples = static_cast<int>(crossfadeLengthMs * 0.001f * currentSampleRate);
    crossfadeSamples = juce::jlimit(100, loopLengthSamples / 2, crossfadeSamples);
    
    // Capture EXTRA samples before loop for crossfade
    int totalSamplesToRead = loopLengthSamples + crossfadeSamples;
    juce::AudioBuffer<float> captureBuffer(2, totalSamplesToRead);
    
    int readHead = writeHead.load();
    int bufferSize = circularBuffer.getNumSamples();
    
    if (bufferSize == 0)
        return juce::AudioBuffer<float>(2, loopLengthSamples);
    
    // Read loop + pre-roll from circular buffer
    for (int i = 0; i < totalSamplesToRead; ++i)
    {
        int readIndex = (readHead - totalSamplesToRead + i + bufferSize) % bufferSize;
        
        for (int ch = 0; ch < 2; ++ch)
        {
            captureBuffer.setSample(ch, i, circularBuffer.getSample(ch, readIndex));
        }
    }
    
    // Create output buffer (just the loop, no pre-roll)
    juce::AudioBuffer<float> output(2, loopLengthSamples);
    
    // Copy the loop portion (skip pre-roll)
    for (int ch = 0; ch < 2; ++ch)
    {
        output.copyFrom(ch, 0, captureBuffer, ch, crossfadeSamples, loopLengthSamples);
    }
    
    // BAKE crossfade: blend pre-roll INTO end of loop
    bakeLoopCrossfadeWithPreroll(output, captureBuffer, 0, loopLengthSamples, crossfadeSamples);
    
    return output;
}

void LiveRecorder::bakeLoopCrossfade(juce::AudioBuffer<float>& buffer, int loopStart, int loopEnd)
{
    (void) buffer;
    (void) loopStart;
    (void) loopEnd;
    // This version is NOT USED anymore - kept for compatibility
    // Use bakeLoopCrossfadeWithPreroll instead
}

void LiveRecorder::bakeLoopCrossfadeWithPreroll(
    juce::AudioBuffer<float>& loopBuffer,
    const juce::AudioBuffer<float>& captureBuffer,
    int loopStart,
    int loopEnd,
    int crossfadeSamples)
{
    (void) loopStart;
    const int numChannels = loopBuffer.getNumChannels();
    if (crossfadeSamples <= 1 || loopEnd <= 0 || numChannels <= 0)
        return;
    // The captureBuffer contains: [PRE-ROLL (crossfadeSamples)][LOOP (loopLength)]
    // We need to blend the PRE-ROLL into the END of the loop
    
    const int fadeStart = loopEnd - crossfadeSamples;
    const float pi_2 = juce::MathConstants<float>::halfPi;
    
    for (int i = 0; i < crossfadeSamples; ++i)
    {
        // Position in crossfade (0.0 to 1.0)
        float t = static_cast<float>(i) / static_cast<float>(crossfadeSamples - 1);
        
        // Equal-power crossfade curves
        const float cosTerm = juce::jlimit(0.0f, 1.0f, std::cos(t * pi_2));
        const float sinTerm = juce::jlimit(0.0f, 1.0f, std::sin(t * pi_2));
        float fadeOut = std::sqrt(cosTerm);  // End: 1 → 0
        float fadeIn = std::sqrt(sinTerm);   // Pre-roll: 0 → 1
        
        for (int ch = 0; ch < numChannels; ++ch)
        {
            // Get sample from END of loop (fading out)
            float endSample = loopBuffer.getSample(ch, fadeStart + i);
            
            // Get sample from PRE-ROLL (the audio BEFORE loop start)
            // Pre-roll is at the START of captureBuffer
            float prerollSample = captureBuffer.getSample(ch, i);
            
            // Blend: fade out loop end, fade in pre-roll
            float blended = (endSample * fadeOut) + (prerollSample * fadeIn);
            if (!std::isfinite(blended))
                blended = 0.0f;
            
            // Write to END position of loop buffer
            loopBuffer.setSample(ch, fadeStart + i, blended);
        }
    }
}


juce::AudioBuffer<float> LiveRecorder::getRecordedBuffer()
{
    // Legacy method - returns empty buffer
    return juce::AudioBuffer<float>();
}

float LiveRecorder::getRecordingProgress() const
{
    // Always recording, so always return 1.0 (full)
    return 1.0f;
}

bool LiveRecorder::shouldBlinkRecordLED(double beatPosition) const
{
    // Blink at DOUBLE speed (every half beat) for recording indication
    double fractionalBeat = beatPosition - std::floor(beatPosition);
    return (fractionalBeat < 0.25) || (fractionalBeat >= 0.5 && fractionalBeat < 0.75);
}

//==============================================================================
// StripGroup Implementation
//==============================================================================

StripGroup::StripGroup(int /*groupId*/)
{
}

void StripGroup::addStrip(int stripIndex)
{
    if (!containsStrip(stripIndex))
        strips.push_back(stripIndex);
}

void StripGroup::removeStrip(int stripIndex)
{
    strips.erase(std::remove(strips.begin(), strips.end(), stripIndex), strips.end());
}

bool StripGroup::containsStrip(int stripIndex) const
{
    return std::find(strips.begin(), strips.end(), stripIndex) != strips.end();
}

void StripGroup::setVolume(float vol)
{
    volume = juce::jlimit(0.0f, 1.0f, vol);
}

void StripGroup::setMuted(bool mute)
{
    muted = mute;
}

//==============================================================================
// EnhancedAudioStrip Implementation
//==============================================================================

EnhancedAudioStrip::EnhancedAudioStrip(int newStripIndex)
    : recordingBars(1), stripIndex(newStripIndex)
{
    const auto stripSeed = static_cast<uint32_t>(newStripIndex + 1);
    const auto seed = static_cast<uint32_t>(juce::Time::currentTimeMillis())
                    ^ (stripSeed * 0x9e3779b9u);
    randomGenerator.seed(seed);
    for (int i = 0; i < ModernAudioEngine::MaxColumns; ++i)
        transientSliceSamples[static_cast<size_t>(i)] = i;
    resetGrainState();
}

void EnhancedAudioStrip::prepareToPlay(double sampleRate, int maxBlockSize)
{
    currentSampleRate = sampleRate;
    crossfader.reset(static_cast<int>(sampleRate));
    triggerOutputBlendActive = false;
    triggerOutputBlendSamplesRemaining = 0;
    triggerOutputBlendTotalSamples = 0;
    triggerOutputBlendStartL = 0.0f;
    triggerOutputBlendStartR = 0.0f;
    lastOutputSampleL = 0.0f;
    lastOutputSampleR = 0.0f;
    
    // Initialize step sampler
    stepSampler.prepareToPlay(sampleRate, maxBlockSize);
    
    // Initialize smoothed parameters (50ms ramp time)
    smoothedVolume.reset(sampleRate, 0.05);
    smoothedPan.reset(sampleRate, 0.05);
    smoothedSpeed.reset(sampleRate, 0.05);
    smoothedPitchShift.reset(sampleRate, 0.02);
    smoothedFilterFrequency.reset(sampleRate, 0.01);
    smoothedFilterResonance.reset(sampleRate, 0.01);
    smoothedFilterMorph.reset(sampleRate, 0.01);
    rateSmoother.reset(sampleRate, 0.05);  // For clock-locked scratching
    grainCenterSmoother.reset(sampleRate, 0.01);
    grainSizeSmoother.reset(sampleRate, 0.015);
    grainSyncedSizeSmoother.reset(sampleRate, 0.02);
    grainDensitySmoother.reset(sampleRate, 0.015);
    grainPitchSmoother.reset(sampleRate, 0.012);
    grainPitchJitterSmoother.reset(sampleRate, 0.012);
    grainFreezeBlendSmoother.reset(sampleRate, 0.08);
    
    smoothedVolume.setCurrentAndTargetValue(volume.load());
    smoothedPan.setCurrentAndTargetValue(pan.load());
    smoothedSpeed.setCurrentAndTargetValue(static_cast<float>(playbackSpeed.load()));
    smoothedPitchShift.setCurrentAndTargetValue(pitchShiftSemitones.load(std::memory_order_acquire));
    smoothedFilterFrequency.setCurrentAndTargetValue(filterFrequency.load(std::memory_order_acquire));
    smoothedFilterResonance.setCurrentAndTargetValue(filterResonance.load(std::memory_order_acquire));
    smoothedFilterMorph.setCurrentAndTargetValue(filterMorph.load(std::memory_order_acquire));
    rateSmoother.setCurrentAndTargetValue(1.0);
    grainCenterSmoother.setCurrentAndTargetValue(0.0);
    grainSizeSmoother.setCurrentAndTargetValue(grainParams.sizeMs);
    grainSyncedSizeSmoother.setCurrentAndTargetValue(grainParams.sizeMs);
    grainDensitySmoother.setCurrentAndTargetValue(grainParams.density);
    grainPitchSmoother.setCurrentAndTargetValue(grainParams.pitchSemitones);
    grainPitchJitterSmoother.setCurrentAndTargetValue(grainParams.pitchJitterSemitones);
    grainFreezeBlendSmoother.setCurrentAndTargetValue(0.0f);
    // Precompute a fixed Blackman-Harris table once; per-voice envelope uses normalized lookup.
    const int windowTableSize = static_cast<int>(grainWindow.size());
    for (int i = 0; i < windowTableSize; ++i)
    {
        const float phase = static_cast<float>(i) / static_cast<float>(juce::jmax(1, windowTableSize - 1));
        const float a0 = 0.35875f;
        const float a1 = 0.48829f;
        const float a2 = 0.14128f;
        const float a3 = 0.01168f;
        const float p1 = juce::MathConstants<float>::twoPi * phase;
        const float p2 = p1 * 2.0f;
        const float p3 = p1 * 3.0f;
        grainWindow[static_cast<size_t>(i)] = a0 - (a1 * std::cos(p1)) + (a2 * std::cos(p2)) - (a3 * std::cos(p3));
    }
    grainSizeMsAtomic.store(grainParams.sizeMs, std::memory_order_release);
    grainDensityAtomic.store(grainParams.density, std::memory_order_release);
    grainPitchAtomic.store(grainParams.pitchSemitones, std::memory_order_release);
    grainPitchJitterAtomic.store(grainParams.pitchJitterSemitones, std::memory_order_release);
    grainSpreadAtomic.store(grainParams.spread, std::memory_order_release);
    grainJitterAtomic.store(grainParams.jitter, std::memory_order_release);
    grainRandomDepthAtomic.store(grainParams.randomDepth, std::memory_order_release);
    grainArpDepthAtomic.store(grainParams.arpDepth, std::memory_order_release);
    grainCloudDepthAtomic.store(grainParams.cloudDepth, std::memory_order_release);
    grainEmitterDepthAtomic.store(grainParams.emitterDepth, std::memory_order_release);
    grainEnvelopeAtomic.store(grainParams.envelope, std::memory_order_release);
    grainShapeAtomic.store(grainParams.shape, std::memory_order_release);
    grainArpModeAtomic.store(grainParams.arpMode, std::memory_order_release);
    grainBloomPhase = 0.0;
    grainBloomAmount = 0.0f;
    grainSpawnAccumulator = 0.0;
    grainSchedulerNoise = 0.0;
    grainSchedulerNoiseTarget = 0.0;
    grainSchedulerNoiseCountdown = 0;
    grainEntryIdentitySamplesRemaining = 0;
    grainParamsSnapshotValid = false;
    grainThreeButtonSnapshotActive = false;
    for (auto& p : grainPreviewPositions)
        p.store(-1.0f, std::memory_order_release);
    for (auto& p : grainPreviewPitchNorms)
        p.store(0.0f, std::memory_order_release);
    grainArpStep = 0;
    grainNeutralBlendState = 1.0f;
    grainPreviewDecimationCounter = 0;
    grainSizeJitterBeatGroup = std::numeric_limits<int64_t>::min();
    grainSizeJitterMul = grainParams.sizeMs;
    grainTempoSyncDivisionIndex = 0;
    grainTempoSyncDivisionBeatGroup = std::numeric_limits<int64_t>::min();
    grainCloudDelayWritePos = 0;
    const int cloudDelaySamples = juce::jmax(1, static_cast<int>(std::round(sampleRate * 2.0)));
    grainCloudDelayBuffer.setSize(2, cloudDelaySamples, false, true, true);
    grainCloudDelayBuffer.clear();
    grainPitchBeforeArp = grainParams.pitchSemitones;
    grainArpWasActive = (grainParams.arpDepth > 0.001f);
    resetPitchShifter();
    
    // Prepare morphing TPT filter bank
    juce::dsp::ProcessSpec spec;
    spec.sampleRate = sampleRate;
    spec.maximumBlockSize = static_cast<juce::uint32>(maxBlockSize);
    spec.numChannels = 2;
    filterLp.prepare(spec);
    filterBp.prepare(spec);
    filterHp.prepare(spec);
    filterLpStage2.prepare(spec);
    filterBpStage2.prepare(spec);
    filterHpStage2.prepare(spec);
    ladderLp.prepare(spec);
    ladderBp.prepare(spec);
    ladderHp.prepare(spec);
    filterLp.setType(juce::dsp::StateVariableTPTFilterType::lowpass);
    filterBp.setType(juce::dsp::StateVariableTPTFilterType::bandpass);
    filterHp.setType(juce::dsp::StateVariableTPTFilterType::highpass);
    filterLpStage2.setType(juce::dsp::StateVariableTPTFilterType::lowpass);
    filterBpStage2.setType(juce::dsp::StateVariableTPTFilterType::bandpass);
    filterHpStage2.setType(juce::dsp::StateVariableTPTFilterType::highpass);
    ladderLp.setMode(juce::dsp::LadderFilterMode::LPF12);
    ladderBp.setMode(juce::dsp::LadderFilterMode::BPF12);
    ladderHp.setMode(juce::dsp::LadderFilterMode::HPF12);
    cachedLadderMode = 12;
    ladderLp.setDrive(1.0f);
    ladderBp.setDrive(1.0f);
    ladderHp.setDrive(1.0f);
    updateFilterCoefficients(filterFrequency.load(std::memory_order_acquire),
                             filterResonance.load(std::memory_order_acquire));
    
    for (auto& interp : interpolators)
        interp.reset();
}

void EnhancedAudioStrip::loadSample(const juce::AudioBuffer<float>& buffer, double sourceRate)
{
    juce::ScopedLock lock(bufferLock);
    triggerOutputBlendActive = false;
    triggerOutputBlendSamplesRemaining = 0;
    triggerOutputBlendTotalSamples = 0;
    triggerOutputBlendStartL = 0.0f;
    triggerOutputBlendStartR = 0.0f;
    lastOutputSampleL = 0.0f;
    lastOutputSampleR = 0.0f;
    
    // Safety check
    if (buffer.getNumSamples() == 0)
    {
        DBG("WARNING: Attempting to load empty buffer into strip");
        return;
    }
    
    // Convert mono to stereo if needed
    if (buffer.getNumChannels() == 1)
    {
        sampleBuffer.setSize(2, buffer.getNumSamples(), false, true, false);
        sampleBuffer.copyFrom(0, 0, buffer, 0, 0, buffer.getNumSamples());
        sampleBuffer.copyFrom(1, 0, buffer, 0, 0, buffer.getNumSamples());
    }
    else
    {
        // Explicitly copy the buffer
        sampleBuffer.setSize(buffer.getNumChannels(), buffer.getNumSamples(), false, true, false);
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            sampleBuffer.copyFrom(ch, 0, buffer, ch, 0, buffer.getNumSamples());
        }
    }
    
    this->sourceSampleRate = sourceRate;
    sampleLength = sampleBuffer.getNumSamples();
    playbackPosition = 0.0;
    grainCenterSmoother.setCurrentAndTargetValue(0.0);
    resetGrainState();
    playing = false;  // Don't auto-start (except for step mode which starts with DAW transport)
    
    // For step sequencer mode, load into stepSampler
    if (playMode == PlayMode::Step)
    {
        stepSampler.loadSampleFromBuffer(buffer, sourceRate);
        playing = true;  // Step sequencer runs with clock, not manual triggers
        DBG("Step sequencer loaded into sampler and ready to sync with clock");
    }

    if (transientSliceMode.load(std::memory_order_acquire))
        rebuildTransientSliceMap();
    else
        transientSliceMapDirty = true;

    rebuildSampleAnalysisCacheLocked();
}

void EnhancedAudioStrip::setTransientSliceMode(bool enabled)
{
    transientSliceMode.store(enabled, std::memory_order_release);

    if (!enabled)
        return;

    juce::ScopedLock lock(bufferLock);
    if (transientSliceMapDirty && sampleBuffer.getNumSamples() > 0)
        rebuildTransientSliceMap();
}

std::array<int, 16> EnhancedAudioStrip::getSliceStartSamples(bool transientMode) const
{
    std::array<int, 16> out{};

    if (sampleLength <= 0.0)
        return out;

    const int total = juce::jmax(1, static_cast<int>(sampleLength));
    if (transientMode)
    {
        for (int i = 0; i < 16; ++i)
            out[static_cast<size_t>(i)] = juce::jlimit(0, total - 1, transientSliceSamples[static_cast<size_t>(i)]);
        return out;
    }

    for (int i = 0; i < 16; ++i)
        out[static_cast<size_t>(i)] = juce::jlimit(0, total - 1, (i * total) / 16);
    return out;
}

std::array<int, 16> EnhancedAudioStrip::getCachedTransientSliceSamples() const
{
    std::array<int, 16> out{};
    for (int i = 0; i < 16; ++i)
        out[static_cast<size_t>(i)] = transientSliceSamples[static_cast<size_t>(i)];
    return out;
}

std::array<float, 128> EnhancedAudioStrip::getCachedRmsMap() const
{
    return analysisRmsMap;
}

std::array<int, 128> EnhancedAudioStrip::getCachedZeroCrossMap() const
{
    return analysisZeroCrossMap;
}

void EnhancedAudioStrip::restoreSampleAnalysisCache(const std::array<int, 16>& transientSlices,
                                                    const std::array<float, 128>& rmsMap,
                                                    const std::array<int, 128>& zeroCrossMap,
                                                    int sourceSampleCount)
{
    juce::ScopedLock lock(bufferLock);

    const int totalSamples = sampleBuffer.getNumSamples();
    if (totalSamples <= 0)
        return;

    const int safeSampleCount = juce::jmax(1, sourceSampleCount);
    const float scale = static_cast<float>(totalSamples) / static_cast<float>(safeSampleCount);

    for (int i = 0; i < ModernAudioEngine::MaxColumns; ++i)
    {
        const int src = transientSlices[static_cast<size_t>(i)];
        const int scaled = static_cast<int>(std::round(static_cast<float>(src) * scale));
        transientSliceSamples[static_cast<size_t>(i)] = juce::jlimit(0, totalSamples - 1, scaled);
    }

    for (size_t i = 0; i < analysisRmsMap.size(); ++i)
        analysisRmsMap[i] = juce::jlimit(0.0f, 1.0f, std::isfinite(rmsMap[i]) ? rmsMap[i] : 0.0f);

    for (size_t i = 0; i < analysisZeroCrossMap.size(); ++i)
    {
        const int src = zeroCrossMap[i];
        const int scaled = static_cast<int>(std::round(static_cast<float>(src) * scale));
        analysisZeroCrossMap[i] = juce::jlimit(0, totalSamples - 1, scaled);
    }

    analysisSampleCount = totalSamples;
    analysisCacheValid = true;
    transientSliceMapDirty = false;
}

void EnhancedAudioStrip::rebuildTransientSliceMap()
{
    for (int i = 0; i < ModernAudioEngine::MaxColumns; ++i)
        transientSliceSamples[static_cast<size_t>(i)] = 0;

    if (sampleBuffer.getNumSamples() <= 0)
        return;

    const int totalSamples = sampleBuffer.getNumSamples();
    const int channels = juce::jmax(1, sampleBuffer.getNumChannels());

    auto fillUniform = [this, totalSamples]()
    {
        for (int i = 0; i < ModernAudioEngine::MaxColumns; ++i)
            transientSliceSamples[static_cast<size_t>(i)] = juce::jlimit(0, totalSamples - 1,
                                                                         (i * totalSamples) / ModernAudioEngine::MaxColumns);
        transientSliceMapDirty = false;
    };

    int fftOrder = 8; // 256
    while ((1 << fftOrder) < juce::jmin(2048, totalSamples) && fftOrder < 12)
        ++fftOrder;
    const int frameSize = 1 << fftOrder;
    const int hop = juce::jmax(32, frameSize / 8);
    const int frames = juce::jmax(1, 1 + ((totalSamples - frameSize) / hop));

    if (frames < 4)
    {
        fillUniform();
        return;
    }

    juce::dsp::FFT fft(fftOrder);
    juce::dsp::WindowingFunction<float> window(static_cast<size_t>(frameSize),
                                               juce::dsp::WindowingFunction<float>::hann,
                                               true);

    const int halfBins = frameSize / 2;
    std::vector<float> fftData(static_cast<size_t>(2 * frameSize), 0.0f);
    std::vector<float> prevMag(static_cast<size_t>(halfBins), 0.0f);
    std::vector<float> spectralFlux(static_cast<size_t>(frames), 0.0f);
    std::vector<float> frameEnergy(static_cast<size_t>(frames), 0.0f);

    for (int frame = 0; frame < frames; ++frame)
    {
        const int start = frame * hop;
        double energy = 0.0;

        for (int n = 0; n < frameSize; ++n)
        {
            const int sampleIndex = juce::jlimit(0, totalSamples - 1, start + n);
            float mono = 0.0f;
            for (int ch = 0; ch < channels; ++ch)
                mono += sampleBuffer.getSample(ch, sampleIndex);
            mono /= static_cast<float>(channels);
            fftData[static_cast<size_t>(n)] = mono;
            energy += static_cast<double>(mono * mono);
        }

        for (int n = frameSize; n < 2 * frameSize; ++n)
            fftData[static_cast<size_t>(n)] = 0.0f;

        window.multiplyWithWindowingTable(fftData.data(), static_cast<size_t>(frameSize));
        fft.performFrequencyOnlyForwardTransform(fftData.data(), true);

        frameEnergy[static_cast<size_t>(frame)] = static_cast<float>(std::sqrt(energy / static_cast<double>(frameSize)));

        float flux = 0.0f;
        for (int bin = 1; bin < halfBins; ++bin)
        {
            const float mag = fftData[static_cast<size_t>(bin)];
            const float diff = juce::jmax(0.0f, mag - prevMag[static_cast<size_t>(bin)]);
            const float weight = 1.0f + (2.0f * static_cast<float>(bin) / static_cast<float>(halfBins));
            flux += diff * weight;
            prevMag[static_cast<size_t>(bin)] = mag;
        }

        spectralFlux[static_cast<size_t>(frame)] = flux;
    }

    std::vector<float> smoothedFlux(static_cast<size_t>(frames), 0.0f);
    for (int i = 0; i < frames; ++i)
    {
        const int a = juce::jmax(0, i - 1);
        const int b = juce::jmin(frames - 1, i + 1);
        float sum = 0.0f;
        for (int k = a; k <= b; ++k)
            sum += spectralFlux[static_cast<size_t>(k)];
        smoothedFlux[static_cast<size_t>(i)] = sum / static_cast<float>(b - a + 1);
    }

    std::vector<float> energyDiff(static_cast<size_t>(frames), 0.0f);
    for (int i = 1; i < frames; ++i)
        energyDiff[static_cast<size_t>(i)] = juce::jmax(0.0f, frameEnergy[static_cast<size_t>(i)] - frameEnergy[static_cast<size_t>(i - 1)]);

    auto medianInWindow = [](const std::vector<float>& values, int start, int end)
    {
        std::vector<float> temp;
        temp.reserve(static_cast<size_t>(end - start + 1));
        for (int i = start; i <= end; ++i)
            temp.push_back(values[static_cast<size_t>(i)]);
        auto midIt = temp.begin() + (temp.size() / 2);
        std::nth_element(temp.begin(), midIt, temp.end());
        return *midIt;
    };

    std::vector<float> novelty(static_cast<size_t>(frames), 0.0f);
    float noveltySum = 0.0f;
    for (int i = 0; i < frames; ++i)
    {
        const int a = juce::jmax(0, i - 8);
        const int b = juce::jmin(frames - 1, i + 8);
        const float adaptive = (medianInWindow(smoothedFlux, a, b) * 1.25f) + 1.0e-6f;
        const float peakPart = juce::jmax(0.0f, smoothedFlux[static_cast<size_t>(i)] - adaptive);
        const float mixed = peakPart + (0.25f * energyDiff[static_cast<size_t>(i)]);
        novelty[static_cast<size_t>(i)] = mixed;
        noveltySum += mixed;
    }

    const float noveltyMean = noveltySum / static_cast<float>(juce::jmax(1, frames));
    const float noveltyMax = *std::max_element(novelty.begin(), novelty.end());
    const float minPeakLevel = juce::jmax(1.0e-6f,
                                          juce::jmax(noveltyMean * 0.35f, noveltyMax * 0.10f));
    const double analysisSampleRate = (sourceSampleRate > 1000.0)
        ? sourceSampleRate
        : juce::jmax(1.0, currentSampleRate);
    const int minPeakSpacingFrames = juce::jmax(1, static_cast<int>((0.015 * analysisSampleRate) / static_cast<double>(hop))); // 15ms

    std::vector<std::pair<int, float>> onsetFrames;
    onsetFrames.reserve(static_cast<size_t>(frames));

    for (int i = 1; i < (frames - 1); ++i)
    {
        const float center = novelty[static_cast<size_t>(i)];
        if (center < minPeakLevel)
            continue;
        if (center < novelty[static_cast<size_t>(i - 1)] || center < novelty[static_cast<size_t>(i + 1)])
            continue;

        if (!onsetFrames.empty() && (i - onsetFrames.back().first) < minPeakSpacingFrames)
        {
            if (center > onsetFrames.back().second)
                onsetFrames.back() = { i, center };
            continue;
        }

        onsetFrames.emplace_back(i, center);
    }

    if (onsetFrames.empty())
    {
        const float energyMax = *std::max_element(energyDiff.begin(), energyDiff.end());
        const float energyMinPeak = juce::jmax(1.0e-6f, energyMax * 0.18f);
        for (int i = 1; i < (frames - 1); ++i)
        {
            const float center = energyDiff[static_cast<size_t>(i)];
            if (center < energyMinPeak)
                continue;
            if (center < energyDiff[static_cast<size_t>(i - 1)] || center < energyDiff[static_cast<size_t>(i + 1)])
                continue;

            if (!onsetFrames.empty() && (i - onsetFrames.back().first) < minPeakSpacingFrames)
                continue;

            onsetFrames.emplace_back(i, center);
        }
    }

    std::vector<int> onsetSamples;
    onsetSamples.reserve(onsetFrames.size());
    for (const auto& onset : onsetFrames)
    {
        // Frame index marks the analysis frame start; shift to frame center so
        // markers land on the transient hit rather than a few ms early.
        const int centered = (onset.first * hop) + (frameSize / 2);
        onsetSamples.push_back(juce::jlimit(0, totalSamples - 1, centered));
    }

    std::sort(onsetSamples.begin(), onsetSamples.end());
    onsetSamples.erase(std::unique(onsetSamples.begin(), onsetSamples.end()), onsetSamples.end());
    if (onsetSamples.empty())
    {
        fillUniform();
        return;
    }

    const int lastIndex = ModernAudioEngine::MaxColumns - 1;
    for (int i = 0; i < ModernAudioEngine::MaxColumns; ++i)
    {
        if (i == 0)
        {
            transientSliceSamples[static_cast<size_t>(i)] = 0;
            continue;
        }

        const int target = juce::jlimit(0, totalSamples - 1,
            static_cast<int>((static_cast<double>(i) / static_cast<double>(lastIndex))
                * static_cast<double>(totalSamples - 1)));

        int chosen = target;
        auto it = std::lower_bound(onsetSamples.begin(), onsetSamples.end(), target);

        int bestCandidate = chosen;
        int bestDistance = std::numeric_limits<int>::max();
        if (it != onsetSamples.end())
        {
            const int dist = std::abs(*it - target);
            if (dist < bestDistance)
            {
                bestDistance = dist;
                bestCandidate = *it;
            }
        }
        if (it != onsetSamples.begin())
        {
            const int prev = *std::prev(it);
            const int dist = std::abs(prev - target);
            if (dist < bestDistance)
            {
                bestDistance = dist;
                bestCandidate = prev;
            }
        }

        if (bestDistance < std::numeric_limits<int>::max())
            chosen = bestCandidate;

        const int prevPos = transientSliceSamples[static_cast<size_t>(i - 1)];
        chosen = juce::jmax(prevPos + 1, chosen);
        transientSliceSamples[static_cast<size_t>(i)] = juce::jlimit(0, totalSamples - 1, chosen);
    }

    transientSliceMapDirty = false;
    rebuildSampleAnalysisCacheLocked();
}

void EnhancedAudioStrip::rebuildSampleAnalysisCacheLocked()
{
    analysisSampleCount = 0;
    analysisCacheValid = false;
    analysisRmsMap.fill(0.0f);
    analysisZeroCrossMap.fill(0);

    const int totalSamples = sampleBuffer.getNumSamples();
    const int channels = juce::jmax(1, sampleBuffer.getNumChannels());
    if (totalSamples <= 0)
        return;

    const int bins = static_cast<int>(analysisRmsMap.size());
    std::vector<float> monoSamples(static_cast<size_t>(totalSamples), 0.0f);
    for (int i = 0; i < totalSamples; ++i)
    {
        float mono = 0.0f;
        for (int ch = 0; ch < channels; ++ch)
            mono += sampleBuffer.getSample(ch, i);
        monoSamples[static_cast<size_t>(i)] = mono / static_cast<float>(channels);
    }

    float maxRms = 1.0e-6f;
    for (int b = 0; b < bins; ++b)
    {
        const int start = (b * totalSamples) / bins;
        const int end = juce::jmax(start + 1, ((b + 1) * totalSamples) / bins);
        const int count = juce::jmax(1, end - start);

        double energy = 0.0;
        for (int i = start; i < end; ++i)
        {
            const float s = monoSamples[static_cast<size_t>(juce::jlimit(0, totalSamples - 1, i))];
            energy += static_cast<double>(s * s);
        }
        const float rms = static_cast<float>(std::sqrt(energy / static_cast<double>(count)));
        analysisRmsMap[static_cast<size_t>(b)] = rms;
        if (rms > maxRms)
            maxRms = rms;

        int zeroIdx = juce::jlimit(0, totalSamples - 1, start);
        for (int i = juce::jmax(start + 1, 1); i < juce::jmin(end, totalSamples); ++i)
        {
            const float prev = monoSamples[static_cast<size_t>(i - 1)];
            const float curr = monoSamples[static_cast<size_t>(i)];
            if ((prev <= 0.0f && curr > 0.0f) || (prev >= 0.0f && curr < 0.0f))
            {
                zeroIdx = i;
                break;
            }
        }
        analysisZeroCrossMap[static_cast<size_t>(b)] = zeroIdx;
    }

    const float invMax = (maxRms > 1.0e-6f) ? (1.0f / maxRms) : 1.0f;
    for (auto& v : analysisRmsMap)
        v = juce::jlimit(0.0f, 1.0f, v * invMax);

    analysisSampleCount = totalSamples;
    analysisCacheValid = true;
}

double EnhancedAudioStrip::getTriggerTargetPositionForColumn(int column, double loopStartSamples, double loopLengthSamplesArg) const
{
    const int clampedColumn = juce::jlimit(0, ModernAudioEngine::MaxColumns - 1, column);

    if (!transientSliceMode.load(std::memory_order_acquire) || sampleLength <= 0.0 || loopLengthSamplesArg <= 0.0)
    {
        const int loopCols = juce::jmax(1, loopEnd - loopStart);
        const double columnOffset = (clampedColumn - loopStart) / static_cast<double>(loopCols);
        return loopStartSamples + (columnOffset * loopLengthSamplesArg);
    }

    double transientPos = static_cast<double>(transientSliceSamples[static_cast<size_t>(clampedColumn)]);
    transientPos = juce::jlimit(0.0, juce::jmax(0.0, sampleLength - 1.0), transientPos);

    return transientPos;
}

double EnhancedAudioStrip::getWrappedSamplePosition(double samplePos, double loopStartSamples, double loopLengthSamplesArg) const
{
    if (sampleLength <= 0.0)
        return 0.0;

    if (playMode == PlayMode::OneShot)
        return juce::jlimit(0.0, juce::jmax(0.0, sampleLength - 1.0), samplePos);

    const double loopLengthSafe = juce::jmax(1.0, loopLengthSamplesArg);
    double posInLoop = std::fmod(samplePos - loopStartSamples, loopLengthSafe);
    if (posInLoop < 0.0)
        posInLoop += loopLengthSafe;
    return loopStartSamples + posInLoop;
}

double EnhancedAudioStrip::snapToNearestZeroCrossing(double targetPos, int radiusSamples) const
{
    const int numChannels = sampleBuffer.getNumChannels();
    const int totalSamples = sampleBuffer.getNumSamples();
    if (numChannels <= 0 || totalSamples < 2 || radiusSamples <= 0)
        return juce::jlimit(0.0, juce::jmax(0.0, sampleLength - 1.0), targetPos);

    const int center = juce::jlimit(1, totalSamples - 2, static_cast<int>(std::lround(targetPos)));
    const int radius = juce::jlimit(1, totalSamples - 2, radiusSamples);
    const int channelsToCheck = juce::jmin(2, numChannels);

    auto sampleAt = [&](int idx) -> float
    {
        float sum = 0.0f;
        for (int ch = 0; ch < channelsToCheck; ++ch)
            sum += sampleBuffer.getSample(ch, juce::jlimit(0, totalSamples - 1, idx));
        return sum / static_cast<float>(channelsToCheck);
    };

    int bestIndex = center;
    float bestAbs = std::abs(sampleAt(center));

    for (int d = 0; d <= radius; ++d)
    {
        const int candidates[2] = { center - d, center + d };
        for (int c = 0; c < 2; ++c)
        {
            const int idx = candidates[c];
            if (idx <= 0 || idx >= (totalSamples - 1))
                continue;

            const float prev = sampleAt(idx - 1);
            const float curr = sampleAt(idx);
            const float absCurr = std::abs(curr);

            if ((prev <= 0.0f && curr >= 0.0f) || (prev >= 0.0f && curr <= 0.0f))
                return static_cast<double>(idx);

            if (absCurr < bestAbs)
            {
                bestAbs = absCurr;
                bestIndex = idx;
            }
        }
    }

    return static_cast<double>(bestIndex);
}

void EnhancedAudioStrip::resetGrainState()
{
    grainGesture = {};
    grainGesture.centerRampMs = 40.0f;
    grainGesture.sceneStartSample = 0;
    grainSpawnAccumulator = 0.0;
    grainSchedulerNoise = 0.0;
    grainSchedulerNoiseTarget = 0.0;
    grainSchedulerNoiseCountdown = 0;
    grainNeutralBlendState = 1.0f;
    grainParamsSnapshotValid = false;
    for (auto& p : grainPreviewPositions)
        p.store(-1.0f, std::memory_order_release);
    for (auto& p : grainPreviewPitchNorms)
        p.store(0.0f, std::memory_order_release);
    grainArpStep = 0;
    grainPreviewDecimationCounter = 0;
    grainPreviewRequestCountdown.store(0, std::memory_order_release);
    grainVoiceSearchStart = 0;
    grainSizeJitterBeatGroup = std::numeric_limits<int64_t>::min();
    grainSizeJitterMul = grainParams.sizeMs;
    grainTempoSyncDivisionIndex = 0;
    grainTempoSyncDivisionBeatGroup = std::numeric_limits<int64_t>::min();
    grainCloudDelayWritePos = 0;
    if (grainCloudDelayBuffer.getNumSamples() > 0)
        grainCloudDelayBuffer.clear();
    for (auto& voice : grainVoices)
        voice = {};
    grainCenterSmoother.setCurrentAndTargetValue(playbackPosition.load());
    grainSizeSmoother.setCurrentAndTargetValue(grainParams.sizeMs);
    grainSyncedSizeSmoother.setCurrentAndTargetValue(grainParams.sizeMs);
    grainDensitySmoother.setCurrentAndTargetValue(grainParams.density);
    grainPitchSmoother.setCurrentAndTargetValue(grainParams.pitchSemitones);
    grainPitchJitterSmoother.setCurrentAndTargetValue(grainParams.pitchJitterSemitones);
    grainFreezeBlendSmoother.setCurrentAndTargetValue(0.0f);
    grainScratchSceneMix.setCurrentAndTargetValue(0.0f);
    grainBloomPhase = 0.0;
    grainBloomAmount = 0.0f;
    updateGrainHeldLedState();
    grainSizeMsAtomic.store(grainParams.sizeMs, std::memory_order_release);
    grainDensityAtomic.store(grainParams.density, std::memory_order_release);
    grainPitchAtomic.store(grainParams.pitchSemitones, std::memory_order_release);
    grainPitchJitterAtomic.store(grainParams.pitchJitterSemitones, std::memory_order_release);
    grainSpreadAtomic.store(grainParams.spread, std::memory_order_release);
    grainJitterAtomic.store(grainParams.jitter, std::memory_order_release);
    grainRandomDepthAtomic.store(grainParams.randomDepth, std::memory_order_release);
    grainArpDepthAtomic.store(grainParams.arpDepth, std::memory_order_release);
    grainCloudDepthAtomic.store(grainParams.cloudDepth, std::memory_order_release);
    grainEmitterDepthAtomic.store(grainParams.emitterDepth, std::memory_order_release);
    grainEnvelopeAtomic.store(grainParams.envelope, std::memory_order_release);
    grainShapeAtomic.store(grainParams.shape, std::memory_order_release);
    grainArpModeAtomic.store(grainParams.arpMode, std::memory_order_release);
    grainPitchBeforeArp = grainParams.pitchSemitones;
    grainArpWasActive = (grainParams.arpDepth > 0.001f);
}

void EnhancedAudioStrip::setGrainCenterTarget(double targetSamplePos, bool proportionalRamp)
{
    if (sampleLength <= 0.0)
        return;

    int loopCols = loopEnd - loopStart;
    if (loopCols <= 0)
        loopCols = ModernAudioEngine::MaxColumns;
    const double loopStartSamples = loopStart * (sampleLength / ModernAudioEngine::MaxColumns);
    const double loopLength = juce::jmax(1.0, (loopCols / static_cast<double>(ModernAudioEngine::MaxColumns)) * sampleLength);

    const double currentCenter = grainCenterSmoother.getCurrentValue();
    const double wrappedCurrent = getWrappedSamplePosition(currentCenter, loopStartSamples, loopLength);
    const double wrappedTarget = getWrappedSamplePosition(targetSamplePos, loopStartSamples, loopLength);

    double delta = wrappedTarget - wrappedCurrent;
    if (delta > (loopLength * 0.5))
        delta -= loopLength;
    else if (delta < -(loopLength * 0.5))
        delta += loopLength;

    double rampMs = static_cast<double>(juce::jmax(1.0f, grainGesture.centerRampMs));
    if (proportionalRamp)
    {
        const double distanceNorm = std::abs(delta) / juce::jmax(1.0, loopLength * 0.25);
        // Keep grain travel smooth and musical: never collapse below base ramp.
        const double scale = juce::jlimit(1.0, 2.0, distanceNorm);
        rampMs *= scale;
    }

    grainCenterSmoother.reset(currentSampleRate, rampMs * 0.001);
    grainCenterSmoother.setCurrentAndTargetValue(currentCenter);
    grainCenterSmoother.setTargetValue(currentCenter + delta);
    grainGesture.centerTravelDistanceAbs = std::abs(delta);
    grainGesture.targetCenterSample = currentCenter + delta;
    grainGesture.frozenCenterSample = grainGesture.targetCenterSample;
}

void EnhancedAudioStrip::updateGrainHeldLedState()
{
    grainLedHeldCount.store(grainGesture.heldCount, std::memory_order_release);
    grainLedAnchor.store(grainGesture.anchorX, std::memory_order_release);
    grainLedSecondary.store(grainGesture.secondaryX, std::memory_order_release);
    grainLedSizeControl.store(grainGesture.sizeControlX, std::memory_order_release);
    grainLedFreeze.store(grainGesture.freeze, std::memory_order_release);
}

void EnhancedAudioStrip::updateGrainAnchorFromHeld()
{
    if (grainGesture.heldCount <= 0)
    {
        grainGesture.anchorX = -1;
        grainGesture.secondaryX = -1;
        return;
    }

    int newestIdx = 0;
    int secondNewestIdx = -1;
    for (int i = 1; i < grainGesture.heldCount; ++i)
    {
        if (grainGesture.heldOrder[i] > grainGesture.heldOrder[newestIdx])
        {
            secondNewestIdx = newestIdx;
            newestIdx = i;
        }
        else if (secondNewestIdx < 0 || grainGesture.heldOrder[i] > grainGesture.heldOrder[secondNewestIdx])
        {
            secondNewestIdx = i;
        }
    }

    grainGesture.anchorX = grainGesture.heldX[newestIdx];
    grainGesture.secondaryX = (secondNewestIdx >= 0) ? grainGesture.heldX[secondNewestIdx] : -1;
}

void EnhancedAudioStrip::updateGrainSizeFromGrip()
{
    if (grainGesture.heldCount < 3 || grainGesture.anchorX < 0 || grainGesture.secondaryX < 0 || grainGesture.sizeControlX < 0)
        return;

    const int minX = juce::jmin(grainGesture.anchorX, grainGesture.secondaryX);
    const int maxX = juce::jmax(grainGesture.anchorX, grainGesture.secondaryX);
    const int span = juce::jmax(1, maxX - minX);
    const float t = juce::jlimit(0.0f, 1.0f, static_cast<float>(grainGesture.sizeControlX - minX) / static_cast<float>(span));
    const float shaped = std::pow(t, 1.35f);

    const float gripSpanNorm = juce::jlimit(0.0f, 1.0f, static_cast<float>(span) / 15.0f);
    const float sizeBaseMs = 140.0f + (420.0f * gripSpanNorm);
    const float sizeSweepMs = 900.0f + (1200.0f * gripSpanNorm);
    grainParams.sizeMs = juce::jlimit(kGrainMinSizeMs, kGrainMaxSizeMs, sizeBaseMs + (sizeSweepMs * shaped));
    grainParams.density = juce::jlimit(0.12f, 0.72f, 0.58f - (0.28f * t) + (0.12f * (1.0f - gripSpanNorm)));
    grainParams.spread = juce::jlimit(0.0f, 1.0f, 0.16f + (0.62f * gripSpanNorm));
    grainParams.jitter = juce::jlimit(0.0f, 1.0f, 0.08f + (0.48f * (1.0f - t)));
    grainParams.randomDepth = juce::jlimit(0.0f, 1.0f, 0.04f + (0.18f * (1.0f - t)));
    // Keep emitter unchanged for 3-finger scratch gestures.
    grainParams.envelope = juce::jlimit(0.0f, 1.0f, 0.18f + (0.36f * (1.0f - t)));
    if (t <= 0.04f)
        grainParams.reverse = false;
    else if (t >= 0.96f)
        grainParams.reverse = true;
    grainSizeSmoother.setTargetValue(grainParams.sizeMs);
    grainDensitySmoother.setTargetValue(grainParams.density);
    grainSizeMsAtomic.store(grainParams.sizeMs, std::memory_order_release);
    grainDensityAtomic.store(grainParams.density, std::memory_order_release);
    grainSpreadAtomic.store(grainParams.spread, std::memory_order_release);
    grainJitterAtomic.store(grainParams.jitter, std::memory_order_release);
    grainRandomDepthAtomic.store(grainParams.randomDepth, std::memory_order_release);
    grainEnvelopeAtomic.store(grainParams.envelope, std::memory_order_release);
}

void EnhancedAudioStrip::updateGrainGripModulation()
{
    if (grainGesture.heldCount < 2 || grainGesture.anchorX < 0 || grainGesture.secondaryX < 0)
        return;

    const int span = std::abs(grainGesture.anchorX - grainGesture.secondaryX);
    const float spanNorm = juce::jlimit(0.0f, 1.0f, static_cast<float>(span) / 15.0f);

    // Two-key grip: denser rhythmic center with controlled spread to avoid noisy wash.
    grainParams.spread = juce::jlimit(0.0f, 1.0f, 0.1f + (0.46f * spanNorm));
    grainParams.jitter = juce::jlimit(0.0f, 1.0f, 0.16f + (0.32f * (1.0f - spanNorm)));
    grainParams.density = juce::jlimit(kGrainMinDensity, kGrainMaxDensity, 0.66f + (0.18f * (1.0f - spanNorm)));
    grainParams.randomDepth = juce::jlimit(0.0f, 1.0f, 0.06f + (0.2f * (1.0f - spanNorm)));
    grainParams.emitterDepth = juce::jlimit(0.0f, 1.0f, 0.2f + (0.5f * spanNorm));
    grainDensitySmoother.setTargetValue(grainParams.density);
    grainDensityAtomic.store(grainParams.density, std::memory_order_release);
    grainSpreadAtomic.store(grainParams.spread, std::memory_order_release);
    grainJitterAtomic.store(grainParams.jitter, std::memory_order_release);
    grainRandomDepthAtomic.store(grainParams.randomDepth, std::memory_order_release);
    grainEmitterDepthAtomic.store(grainParams.emitterDepth, std::memory_order_release);
}

void EnhancedAudioStrip::updateGrainGestureOnPress(int column, int64_t globalSample)
{
    if (sampleLength <= 0.0)
        return;

    column = juce::jlimit(0, ModernAudioEngine::MaxColumns - 1, column);
    for (int i = 0; i < grainGesture.heldCount; ++i)
    {
        if (grainGesture.heldX[i] == column)
            return;
    }

    if (grainGesture.heldCount >= 3)
        return;

    const int idx = grainGesture.heldCount++;
    grainGesture.heldX[idx] = column;
    grainGesture.heldOrder[idx] = ++grainGesture.orderCounter;
    grainGesture.anyHeld = (grainGesture.heldCount > 0);
    grainGesture.sceneStartSample = globalSample;

    const double tempoNow = (lastObservedTempo > 0.0) ? lastObservedTempo : 120.0;
    const float grainScratch = scratchAmount.load(std::memory_order_acquire);
    grainGesture.centerRampMs = static_cast<float>(grainScratchSecondsFromAmount(grainScratch) * 1000.0);

    updateGrainAnchorFromHeld();
    grainGesture.freeze = true;
    grainGesture.returningToTimeline = false;

    if (grainGesture.heldCount == 3)
    {
        if (!grainParamsSnapshotValid)
        {
            grainParamsBeforeGesture = grainParams;
            grainParamsSnapshotValid = true;
            grainThreeButtonSnapshotActive = true;
        }
        grainGesture.sizeControlX = column;
        updateGrainSizeFromGrip();
    }
    else
    {
        grainGesture.sizeControlX = -1;
        int loopCols = loopEnd - loopStart;
        if (loopCols <= 0)
            loopCols = ModernAudioEngine::MaxColumns;
        const double loopStartSamples = loopStart * (sampleLength / ModernAudioEngine::MaxColumns);
        const double loopLength = (loopCols / static_cast<double>(ModernAudioEngine::MaxColumns)) * sampleLength;
        const double target = getTriggerTargetPositionForColumn(grainGesture.anchorX, loopStartSamples, loopLength);
        if (grainScratch <= 0.001f)
        {
            const double wrapped = getWrappedSamplePosition(target, loopStartSamples, loopLength);
            grainCenterSmoother.setCurrentAndTargetValue(wrapped);
            grainGesture.centerTravelDistanceAbs = 0.0;
            grainGesture.targetCenterSample = wrapped;
            grainGesture.frozenCenterSample = wrapped;
            grainGesture.centerSampleSmoothed = wrapped;
            playbackPosition = wrapped;
        }
        else
        {
            setGrainCenterTarget(target, false);
        }
    }

    const float sceneDepth = (grainGesture.heldCount >= 3)
        ? juce::jlimit(0.0f, 1.0f, 0.62f + (0.12f * static_cast<float>(grainGesture.heldCount - 3)))
        : 0.0f;
    setGrainScratchSceneTarget(sceneDepth, grainGesture.heldCount, tempoNow);
    updateGrainHeldLedState();
}

double EnhancedAudioStrip::getTimelinePositionForSample(int64_t globalSample) const
{
    if (sampleLength <= 0.0)
        return 0.0;

    // In grain mode, RandomSlice (and other non-linear directions) can remap
    // position away from raw timeline phase. Use audible position as timeline
    // return target so release from held-grain state follows the active mode.
    if (playMode == PlayMode::Grain
        && (directionMode == DirectionMode::RandomSlice
            || directionMode == DirectionMode::Random
            || directionMode == DirectionMode::RandomWalk
            || directionMode == DirectionMode::PingPong
            || directionMode == DirectionMode::Reverse))
    {
        return playbackPosition.load();
    }

    int loopCols = loopEnd - loopStart;
    if (loopCols <= 0)
        loopCols = ModernAudioEngine::MaxColumns;
    const double loopStartSamples = loopStart * (sampleLength / ModernAudioEngine::MaxColumns);
    const double loopLength = juce::jmax(1.0, (loopCols / static_cast<double>(ModernAudioEngine::MaxColumns)) * sampleLength);

    if (ppqTimelineAnchored && lastObservedPpqValid && lastObservedTempo > 0.0)
    {
        const float manualBeats = beatsPerLoop.load();
        const double beatsForLoop = (manualBeats >= 0.0f) ? static_cast<double>(manualBeats) : 4.0;
        const double samplesPerBeat = (60.0 / lastObservedTempo) * currentSampleRate;
        const double ppqAtSample = lastObservedPPQ + (static_cast<double>(globalSample - lastObservedGlobalSample) / samplesPerBeat);

        double beatInLoop = std::fmod(ppqAtSample + ppqTimelineOffsetBeats, beatsForLoop);
        if (beatInLoop < 0.0)
            beatInLoop += beatsForLoop;
        return loopStartSamples + ((beatInLoop / beatsForLoop) * loopLength);
    }

    const int64_t samplesElapsedSinceTrigger = globalSample - triggerSample;
    const double triggerOffset = juce::jlimit(0.0, 0.999999, triggerOffsetRatio) * loopLength;
    const double speed = static_cast<double>(playbackSpeed.load());
    double posInLoop = std::fmod(triggerOffset + (samplesElapsedSinceTrigger * speed), loopLength);
    if (posInLoop < 0.0)
        posInLoop += loopLength;
    return loopStartSamples + posInLoop;
}

double EnhancedAudioStrip::getGrainBeatPositionAtSample(int64_t globalSample) const
{
    const double tempoNow = (lastObservedTempo > 0.0) ? lastObservedTempo : 120.0;
    const double samplesPerBeat = (60.0 / juce::jmax(1.0, tempoNow)) * juce::jmax(1.0, currentSampleRate);

    if (lastObservedPpqValid)
        return lastObservedPPQ + (static_cast<double>(globalSample - lastObservedGlobalSample) / samplesPerBeat);

    return static_cast<double>(globalSample - triggerSample) / samplesPerBeat;
}

double EnhancedAudioStrip::getGrainColumnCenterPosition(int column) const
{
    if (sampleLength <= 0.0)
        return 0.0;

    int loopCols = loopEnd - loopStart;
    if (loopCols <= 0)
        loopCols = ModernAudioEngine::MaxColumns;
    const double loopStartSamples = loopStart * (sampleLength / ModernAudioEngine::MaxColumns);
    const double loopLength = (loopCols / static_cast<double>(ModernAudioEngine::MaxColumns)) * sampleLength;
    return getTriggerTargetPositionForColumn(column, loopStartSamples, loopLength);
}

void EnhancedAudioStrip::setGrainScratchSceneTarget(float targetMix, int heldCount, double tempoBpm)
{
    const float clampedTarget = juce::jlimit(0.0f, 1.0f, targetMix);
    const int held = juce::jlimit(0, 3, heldCount);
    const float scratchNorm = juce::jlimit(0.0f, 1.0f, scratchAmount.load(std::memory_order_acquire) / 100.0f);

    // Faster attack for larger grips, slightly longer release to avoid abrupt tails.
    double rampBeats = 0.25;
    if (clampedTarget >= grainScratchSceneMix.getCurrentValue())
    {
        rampBeats = (held >= 3) ? (1.0 / 16.0) : ((held == 2) ? (1.0 / 12.0) : (1.0 / 8.0));
    }
    else
    {
        rampBeats = (held >= 2) ? (1.0 / 8.0) : 0.25;
    }
    rampBeats *= juce::jlimit(0.45, 1.25, 1.2 - (0.75 * static_cast<double>(scratchNorm)));

    const double bpm = juce::jlimit(20.0, 320.0, (tempoBpm > 0.0) ? tempoBpm : 120.0);
    const double rampSeconds = juce::jlimit(0.006, 0.45, (60.0 / bpm) * rampBeats);
    grainScratchSceneMix.reset(currentSampleRate, rampSeconds);
    grainScratchSceneMix.setTargetValue(clampedTarget);
}

void EnhancedAudioStrip::updateGrainGestureOnRelease(int column, int64_t globalSample)
{
    auto restoreSnapshotIfNeeded = [this]()
    {
        if (!(grainParamsSnapshotValid && grainThreeButtonSnapshotActive))
            return;

        grainParams = grainParamsBeforeGesture;
        grainSizeSmoother.setTargetValue(grainParams.sizeMs);
        grainDensitySmoother.setTargetValue(grainParams.density);
        grainSizeMsAtomic.store(grainParams.sizeMs, std::memory_order_release);
        grainDensityAtomic.store(grainParams.density, std::memory_order_release);
        grainPitchAtomic.store(grainParams.pitchSemitones, std::memory_order_release);
        grainPitchJitterAtomic.store(grainParams.pitchJitterSemitones, std::memory_order_release);
        grainSpreadAtomic.store(grainParams.spread, std::memory_order_release);
        grainJitterAtomic.store(grainParams.jitter, std::memory_order_release);
        grainRandomDepthAtomic.store(grainParams.randomDepth, std::memory_order_release);
        grainArpDepthAtomic.store(grainParams.arpDepth, std::memory_order_release);
        grainCloudDepthAtomic.store(grainParams.cloudDepth, std::memory_order_release);
        grainEmitterDepthAtomic.store(grainParams.emitterDepth, std::memory_order_release);
        grainEnvelopeAtomic.store(grainParams.envelope, std::memory_order_release);
        grainShapeAtomic.store(grainParams.shape, std::memory_order_release);
        grainArpModeAtomic.store(grainParams.arpMode, std::memory_order_release);
        grainParamsSnapshotValid = false;
        grainThreeButtonSnapshotActive = false;
    };

    const bool wasThreeButton = (grainGesture.heldCount == 3);
    int foundIdx = -1;
    for (int i = 0; i < grainGesture.heldCount; ++i)
    {
        if (grainGesture.heldX[i] == column)
        {
            foundIdx = i;
            break;
        }
    }

    if (foundIdx < 0)
        return;

    for (int i = foundIdx; i < grainGesture.heldCount - 1; ++i)
    {
        grainGesture.heldX[i] = grainGesture.heldX[i + 1];
        grainGesture.heldOrder[i] = grainGesture.heldOrder[i + 1];
    }
    if (grainGesture.heldCount > 0)
    {
        const int clearIdx = grainGesture.heldCount - 1;
        grainGesture.heldX[clearIdx] = -1;
        grainGesture.heldOrder[clearIdx] = 0;
    }
    grainGesture.heldCount = juce::jmax(0, grainGesture.heldCount - 1);
    grainGesture.anyHeld = (grainGesture.heldCount > 0);

    if (wasThreeButton && grainGesture.heldCount < 3)
        restoreSnapshotIfNeeded();

    if (grainGesture.heldCount <= 0)
    {
        grainGesture.freeze = false;
        grainGesture.returningToTimeline = false;
        grainGesture.anchorX = -1;
        grainGesture.secondaryX = -1;
        grainGesture.sizeControlX = -1;
        const float grainScratch = scratchAmount.load(std::memory_order_acquire);
        const double tempoNow = (lastObservedTempo > 0.0) ? lastObservedTempo : 120.0;
        grainGesture.centerRampMs = static_cast<float>(juce::jmax(10.0, grainScratchSecondsFromAmount(grainScratch) * 1000.0));
        const double timelineTarget = getTimelinePositionForSample(globalSample);
        if (grainScratch <= 0.001f)
        {
            int loopCols = loopEnd - loopStart;
            if (loopCols <= 0)
                loopCols = ModernAudioEngine::MaxColumns;
            const double loopStartSamples = loopStart * (sampleLength / ModernAudioEngine::MaxColumns);
            const double loopLength = (loopCols / static_cast<double>(ModernAudioEngine::MaxColumns)) * sampleLength;
            const double wrapped = getWrappedSamplePosition(timelineTarget, loopStartSamples, loopLength);
            grainCenterSmoother.setCurrentAndTargetValue(wrapped);
            grainGesture.centerTravelDistanceAbs = 0.0;
            grainGesture.targetCenterSample = wrapped;
            grainGesture.frozenCenterSample = wrapped;
            grainGesture.centerSampleSmoothed = wrapped;
            playbackPosition = wrapped;
        }
        else
        {
            // Full release: smooth proportional return to timeline target.
            grainGesture.freeze = true;
            grainGesture.returningToTimeline = true;
            setGrainCenterTarget(timelineTarget, true);
        }

        restoreSnapshotIfNeeded();
        setGrainScratchSceneTarget(0.0f, 0, tempoNow);
    }
    else
    {
        updateGrainAnchorFromHeld();
        grainGesture.freeze = true;
        grainGesture.returningToTimeline = false;
        grainGesture.sizeControlX = (grainGesture.heldCount == 3) ? grainGesture.heldX[2] : -1;
        if (grainGesture.heldCount == 3)
            updateGrainSizeFromGrip();
        else
        {
            int loopCols = loopEnd - loopStart;
            if (loopCols <= 0)
                loopCols = ModernAudioEngine::MaxColumns;
            const double loopStartSamples = loopStart * (sampleLength / ModernAudioEngine::MaxColumns);
            const double loopLength = (loopCols / static_cast<double>(ModernAudioEngine::MaxColumns)) * sampleLength;
            const double target = getTriggerTargetPositionForColumn(grainGesture.anchorX, loopStartSamples, loopLength);
            setGrainCenterTarget(target, true);
        }

        const double tempoNow = (lastObservedTempo > 0.0) ? lastObservedTempo : 120.0;
        const float sceneDepth = (grainGesture.heldCount >= 3)
            ? juce::jlimit(0.0f, 1.0f, 0.62f + (0.12f * static_cast<float>(grainGesture.heldCount - 3)))
            : 0.0f;
        setGrainScratchSceneTarget(sceneDepth, grainGesture.heldCount, tempoNow);
    }

    updateGrainHeldLedState();
}

void EnhancedAudioStrip::spawnGrainVoice(double centerSamplePos, float sizeMs, float density, float spread, float pitchOffsetSemitones, double playbackStepBase)
{
    if (sampleLength <= 0.0)
        return;

    const int voiceCount = static_cast<int>(grainVoices.size());
    int voiceIndex = -1;

    // Fast path: cyclic search for an inactive voice slot.
    const int searchStart = juce::jlimit(0, juce::jmax(0, voiceCount - 1), grainVoiceSearchStart);
    for (int n = 0; n < voiceCount; ++n)
    {
        const int i = (searchStart + n) % juce::jmax(1, voiceCount);
        if (!grainVoices[static_cast<size_t>(i)].active)
        {
            voiceIndex = i;
            grainVoiceSearchStart = (i + 1) % juce::jmax(1, voiceCount);
            break;
        }
    }

    // Fallback: steal oldest active voice (same policy as before).
    if (voiceIndex < 0)
    {
        int oldestAge = -1;
        for (int i = 0; i < voiceCount; ++i)
        {
            if (grainVoices[static_cast<size_t>(i)].ageSamples > oldestAge)
            {
                oldestAge = grainVoices[static_cast<size_t>(i)].ageSamples;
                voiceIndex = i;
            }
        }
        if (voiceIndex < 0)
            return;

        grainVoiceSearchStart = (voiceIndex + 1) % juce::jmax(1, voiceCount);
    }

    const int maxSizeSamplesByRange = juce::jmax(kMinGrainWindowSamples,
        static_cast<int>(std::round((kGrainMaxSizeMs * 0.001f) * static_cast<float>(currentSampleRate))));
    int sizeSamples = juce::jlimit(kMinGrainWindowSamples,
                                   maxSizeSamplesByRange,
                                   static_cast<int>(std::round((sizeMs * 0.001f) * static_cast<float>(currentSampleRate))));
    const int baseSizeSamples = sizeSamples;

    int loopCols = loopEnd - loopStart;
    if (loopCols <= 0)
        loopCols = ModernAudioEngine::MaxColumns;
    const double loopStartSamples = loopStart * (sampleLength / ModernAudioEngine::MaxColumns);
    const double loopLength = (loopCols / static_cast<double>(ModernAudioEngine::MaxColumns)) * sampleLength;

    std::uniform_real_distribution<double> spreadDist(-1.0, 1.0);
    const double spreadSamples = static_cast<double>(sizeSamples) * static_cast<double>(juce::jlimit(0.0f, 1.0f, spread));
    const float jitter = grainJitterAtomic.load(std::memory_order_acquire);
    const float randomDepth = grainRandomDepthAtomic.load(std::memory_order_acquire);
    const double spraySamples = spreadDist(randomGenerator)
        * (loopLength * static_cast<double>(juce::jlimit(0.0f, 1.0f, randomDepth)) * 0.24);
    const double offset = (spreadDist(randomGenerator) * spreadSamples) + spraySamples;

    auto& voice = grainVoices[static_cast<size_t>(voiceIndex)];
    voice.active = true;
    voice.ageSamples = 0;
    voice.readPos = getWrappedSamplePosition(centerSamplePos + offset, loopStartSamples, loopLength);
    const float pitchBase = grainPitchAtomic.load(std::memory_order_acquire);
    const float pitchJitterSpan = grainPitchJitterAtomic.load(std::memory_order_acquire);
    std::uniform_real_distribution<float> pitchJitterDist(-pitchJitterSpan, pitchJitterSpan);
    const float arpDepth = grainArpDepthAtomic.load(std::memory_order_acquire);
    const bool arpActive = (arpDepth > 0.001f);
    const int arpMode = arpActive ? juce::jlimit(0, 5, grainArpModeAtomic.load(std::memory_order_acquire)) : 0;
    const float arpRangeSemis = juce::jlimit(0.0f, 48.0f, std::abs(pitchBase));
    auto quantizeToScale = [](float semi, const std::array<int, 7>& scale, int rootMidi)
    {
        const int midi = static_cast<int>(std::round(semi + static_cast<float>(rootMidi)));
        int bestMidi = midi;
        int bestDist = 999;
        for (int oct = -4; oct <= 4; ++oct)
        {
            for (int deg : scale)
            {
                const int cand = (12 * oct) + deg + rootMidi;
                const int dist = std::abs(cand - midi);
                if (dist < bestDist)
                {
                    bestDist = dist;
                    bestMidi = cand;
                }
            }
        }
        return static_cast<float>(bestMidi - rootMidi);
    };
    auto quantizeToArpMode = [&](float semi) -> float
    {
        static constexpr std::array<int, 7> majorScale { 0, 2, 4, 5, 7, 9, 11 };
        static constexpr std::array<int, 7> minorScale { 0, 2, 3, 5, 7, 8, 11 };
        static constexpr std::array<int, 7> pentaScale { 0, 2, 4, 7, 9, 12, 14 };
        if (arpMode == 3)
            return quantizeToScale(semi, majorScale, 60);
        if (arpMode == 4)
            return quantizeToScale(semi, minorScale, 57);
        if (arpMode == 5)
            return quantizeToScale(semi, pentaScale, 62);
        if (arpMode == 0)
            return 12.0f * std::round(semi / 12.0f);
        if (arpMode == 1)
            return 7.0f * std::round(semi / 7.0f);
        if (arpMode == 2)
            return 5.0f * std::round(semi / 5.0f);
        return semi;
    };

    float pitchSemi = (arpActive ? 0.0f : pitchBase) + pitchOffsetSemitones + pitchJitterDist(randomGenerator);

    if (arpActive)
    {
        static constexpr std::array<float, 8> powerPattern  { 0.0f, 7.0f, 12.0f, 7.0f, 0.0f, 7.0f, 12.0f, 7.0f };
        static constexpr std::array<float, 8> zigzagPattern { 12.0f, -12.0f, 12.0f, -12.0f, 7.0f, -7.0f, 7.0f, -7.0f };
        const int stepCount = (arpMode <= 2) ? 8 : 6;
        const int stepIdxInt = juce::jmax(0, grainArpStep % stepCount);
        const size_t stepIdx = static_cast<size_t>(stepIdxInt);
        float interval = 0.0f;
        const float rangeScale = juce::jlimit(0.0f, 1.0f, arpRangeSemis / 48.0f);

        if (arpMode == 0)
        {
            // OCTAVE mode: strict random octave transposition per grain, within RANGE.
            // Example RANGE=48 -> {-48,-36,-24,-12,0,+12,+24,+36,+48}
            const int octaveSteps = juce::jlimit(0, 4, static_cast<int>(std::floor((arpRangeSemis + 1.0e-4f) / 12.0f)));
            if (octaveSteps > 0)
            {
                std::uniform_int_distribution<int> octaveDist(-octaveSteps, octaveSteps);
                pitchSemi = 12.0f * static_cast<float>(octaveDist(randomGenerator));
            }
            else
            {
                pitchSemi = 0.0f;
            }
        }
        else if (arpMode == 1)
        {
            interval = powerPattern[stepIdx] * juce::jlimit(0.6f, 3.2f, 0.6f + (2.6f * rangeScale));
        }
        else if (arpMode == 2)
        {
            interval = zigzagPattern[stepIdx] * juce::jlimit(0.6f, 3.2f, 0.6f + (2.6f * rangeScale));
        }
        else if (arpMode == 3)
        {
            static constexpr std::array<float, 6> majorContour { -1.0f, -0.45f, 0.0f, 0.45f, 1.0f, 0.22f };
            interval = majorContour[stepIdx] * arpRangeSemis;
        }
        else if (arpMode == 4)
        {
            static constexpr std::array<float, 6> minorContour { -1.0f, -0.62f, -0.14f, 0.32f, 0.86f, 0.12f };
            interval = minorContour[stepIdx] * arpRangeSemis;
        }
        else
        {
            static constexpr std::array<float, 6> pentaContour { -1.0f, -0.38f, 0.18f, 0.58f, 1.0f, -0.2f };
            interval = pentaContour[stepIdx] * arpRangeSemis;
        }
        if (arpMode >= 1 && arpMode <= 2)
        {
            const float sign = ((stepIdxInt & 1) == 0) ? 1.0f : -1.0f;
            interval *= sign;
        }
        if (arpMode != 0)
        {
            interval = juce::jlimit(-arpRangeSemis, arpRangeSemis, interval);
            pitchSemi += interval * juce::jlimit(0.0f, 1.0f, arpDepth);

            // Add bipolar random excursion within the selected range so ARP can move
            // both up and down musically instead of biasing upward.
            if (arpRangeSemis > 0.0f)
            {
                std::uniform_real_distribution<float> bipolarDist(-arpRangeSemis, arpRangeSemis);
                float bipolar = bipolarDist(randomGenerator);
                const float bipolarAmount = (arpMode >= 3)
                    ? (0.08f + (0.22f * juce::jlimit(0.0f, 1.0f, arpDepth)))
                    : (0.22f + (0.58f * juce::jlimit(0.0f, 1.0f, arpDepth)));
                pitchSemi += bipolar * bipolarAmount;
            }
        }

        grainArpStep = (grainArpStep + 1) % stepCount;

        if (arpMode == 0)
        {
            sizeSamples = static_cast<int>(sizeSamples * ((stepIdx % 2 == 0) ? 1.08f : 0.92f));
        }
        else if (arpMode == 1)
        {
            sizeSamples = static_cast<int>(sizeSamples * ((stepIdx % 4 == 0) ? 1.08f : 0.88f));
        }
        else if (arpMode == 2)
        {
            const bool zig = (stepIdx % 2 == 0);
            const float zigScale = zig ? 1.06f : juce::jlimit(0.82f, 1.02f, 1.02f - (jitter * 0.2f));
            sizeSamples = static_cast<int>(sizeSamples * zigScale);
        }
        else if (arpMode >= 3)
        {
            pitchSemi = quantizeToArpMode(pitchSemi);
            // Keep tonal arp modes musical and less clicky.
            sizeSamples = static_cast<int>(sizeSamples * (0.94f + (0.26f * static_cast<float>(stepIdxInt % 3 == 0))));
        }
        const float emitterDepth = grainEmitterDepthAtomic.load(std::memory_order_acquire);
        // Keep ARP and emitter contributions independent so emitter remains audible
        // even when ARP depth is high.
        const float lengthFloorDriver = juce::jlimit(0.0f, 1.0f, (arpDepth * 0.45f) + (emitterDepth * 0.55f));
        const float lengthFloorScale = juce::jlimit(0.62f, 1.0f, 0.62f + (0.38f * lengthFloorDriver));
        const int lengthFloor = static_cast<int>(std::round(static_cast<float>(baseSizeSamples) * lengthFloorScale));
        sizeSamples = juce::jmax(sizeSamples, lengthFloor);
        sizeSamples = juce::jlimit(kMinGrainWindowSamples, maxSizeSamplesByRange, sizeSamples);

        // Enforce bipolar motion around 0 within the selected range.
        if (arpMode != 0 && arpRangeSemis > 0.0f)
        {
            const float altSign = ((stepIdxInt & 1) == 0) ? 1.0f : -1.0f;
            pitchSemi = altSign * std::abs(pitchSemi);
            pitchSemi = juce::jlimit(-arpRangeSemis, arpRangeSemis, pitchSemi);
        }
    }

    if (randomDepth > 0.001f)
    {
        // RAND: wide, macro variation (distinct from SJTR size movement).
        std::uniform_real_distribution<float> randPitchDist(-12.0f * randomDepth, 12.0f * randomDepth);
        pitchSemi += randPitchDist(randomGenerator);
        std::uniform_real_distribution<float> randSizeDist(0.6f, 1.9f);
        sizeSamples = static_cast<int>(std::round(static_cast<float>(sizeSamples)
                       * (1.0f + ((randSizeDist(randomGenerator) - 1.0f) * randomDepth))));
        sizeSamples = juce::jlimit(kMinGrainWindowSamples, maxSizeSamplesByRange, sizeSamples);
    }

    if (arpActive)
    {
        pitchSemi = quantizeToArpMode(pitchSemi);
        pitchSemi = juce::jlimit(-arpRangeSemis, arpRangeSemis, pitchSemi);
    }

    voice.lengthSamples = juce::jlimit(kMinGrainWindowSamples, maxSizeSamplesByRange, sizeSamples);
    voice.pitchSemitones = juce::jlimit(-48.0f, 48.0f, pitchSemi);
    const double pitchRatio = std::pow(2.0, static_cast<double>(voice.pitchSemitones) / 12.0);
    const double transportStep = juce::jlimit(0.01, 8.0, std::abs(playbackStepBase));
    bool reverseVoice = grainParams.reverse;
    if (!reverseVoice)
    {
        std::uniform_real_distribution<float> reverseDist(0.0f, 1.0f);
        const float reverseChance = juce::jlimit(0.0f, 0.92f, randomDepth * 0.88f);
        reverseVoice = (reverseDist(randomGenerator) < reverseChance);
    }
    voice.step = (reverseVoice ? -1.0 : 1.0) * pitchRatio * transportStep;
    if (!reverseVoice && pitchRatio > 1.0)
    {
        // Keep forward, pitched-up grains from reading past the playhead anchor.
        const double headroomSamples = (pitchRatio - 1.0) * static_cast<double>(voice.lengthSamples);
        voice.readPos = getWrappedSamplePosition(voice.readPos - headroomSamples, loopStartSamples, loopLength);
    }
    std::uniform_real_distribution<float> panDist(-juce::jlimit(0.0f, 1.0f, spread),
                                                  juce::jlimit(0.0f, 1.0f, spread));
    const float panPos = juce::jlimit(-1.0f, 1.0f, panDist(randomGenerator));
    const float panAngle = (panPos + 1.0f) * juce::MathConstants<float>::pi * 0.25f;
    voice.panL = std::cos(panAngle);
    voice.panR = std::sin(panAngle);

    const float clampedDensity = juce::jlimit(kGrainMinDensity, kGrainMaxDensity, density);
    const float envelopeBase = grainEnvelopeAtomic.load(std::memory_order_acquire);
    voice.envelopeCurve = juce::jlimit(0.6f, 2.4f, 2.2f - (1.4f * clampedDensity));
    voice.envelopeSkew = juce::jlimit(0.28f, 0.72f, 0.5f + (panPos * 0.18f));
    const float envelopeJitter = randomDepth * 0.08f;
    std::uniform_real_distribution<float> envDist(-envelopeJitter, envelopeJitter);
    voice.envelopeFade = juce::jlimit(0.0f, 1.0f, envelopeBase + envDist(randomGenerator));
}

void EnhancedAudioStrip::renderGrainAtSample(float& outL, float& outR, double centerSamplePos, double effectiveSpeed, int64_t globalSample)
{
    outL = 0.0f;
    outR = 0.0f;

    if (sampleBuffer.getNumSamples() <= 0)
        return;

    int loopCols = loopEnd - loopStart;
    if (loopCols <= 0)
        loopCols = ModernAudioEngine::MaxColumns;
    const double loopStartSamples = loopStart * (sampleLength / ModernAudioEngine::MaxColumns);
    const double loopLengthSamplesLocal = juce::jmax(1.0, (loopCols / static_cast<double>(ModernAudioEngine::MaxColumns)) * sampleLength);

    const int heldCount = grainGesture.heldCount;
    float sceneMix = juce::jlimit(0.0f, 1.0f, grainScratchSceneMix.getNextValue());
    const double beatNow = getGrainBeatPositionAtSample(globalSample);
    const float targetBloom = (heldCount > 0) ? juce::jlimit(0.0f, 1.0f, 0.5f + (0.18f * static_cast<float>(heldCount - 1))) : 0.0f;
    grainBloomAmount += (targetBloom - grainBloomAmount) * 0.0025f;
    const float pitchNow = grainPitchSmoother.getNextValue();
    const float pitchJitterNow = grainPitchJitterSmoother.getNextValue();
    grainPitchAtomic.store(pitchNow, std::memory_order_release);
    grainPitchJitterAtomic.store(pitchJitterNow, std::memory_order_release);
    const float arpDepth = grainArpDepthAtomic.load(std::memory_order_acquire);
    float jitterAmount = grainJitterAtomic.load(std::memory_order_acquire);
    float randomDepth = grainRandomDepthAtomic.load(std::memory_order_acquire);
    const float spreadBaseNow = grainSpreadAtomic.load(std::memory_order_acquire);
    const float cloudDepth = grainCloudDepthAtomic.load(std::memory_order_acquire);
    const float emitterDepth = grainEmitterDepthAtomic.load(std::memory_order_acquire);
    const float envelopeNow = grainEnvelopeAtomic.load(std::memory_order_acquire);
    const float shapeNow = grainShapeAtomic.load(std::memory_order_acquire);
    const double bloomHz = 1.2 + (2.3 * static_cast<double>(juce::jmax(0, heldCount)));
    grainBloomPhase += (juce::MathConstants<double>::twoPi * bloomHz) / juce::jmax(1.0, currentSampleRate);
    if (grainBloomPhase > juce::MathConstants<double>::twoPi)
        grainBloomPhase -= juce::MathConstants<double>::twoPi;

    const bool strictSingleHoldFreeze = (grainGesture.freeze && heldCount == 1);
    if (strictSingleHoldFreeze)
    {
        // Single held button should freeze cleanly without flutter.
        sceneMix = 0.0f;
        jitterAmount = 0.0f;
        randomDepth = 0.0f;
    }

    // Tempo-locked rhythmic scratch scene: ramps grain parameters and center for stutter/time-stretch effects.
    float scenePulse = 0.0f;
    float sceneTri = 0.0f;
    int sceneStepIndex = 0;
    double sceneCenterSample = centerSamplePos;
    if (sceneMix > 0.001f && heldCount > 0)
    {
        const double stepBeats = (heldCount >= 3) ? (1.0 / 24.0) : ((heldCount == 2) ? (1.0 / 16.0) : (1.0 / 8.0));
        const double scenePhase = beatNow / stepBeats;
        const double sceneStepBase = std::floor(scenePhase);
        const double sceneStepFrac = scenePhase - sceneStepBase;
        sceneStepIndex = static_cast<int>(sceneStepBase);
        scenePulse = std::pow(static_cast<float>(1.0 - sceneStepFrac), (heldCount >= 2) ? 2.8f : 2.0f);
        sceneTri = 1.0f - std::abs((2.0f * static_cast<float>(sceneStepFrac)) - 1.0f);

        if (heldCount >= 2 && grainGesture.anchorX >= 0 && grainGesture.secondaryX >= 0)
        {
            std::array<int, 3> comboCols { grainGesture.anchorX, grainGesture.secondaryX,
                                           (grainGesture.sizeControlX >= 0 ? grainGesture.sizeControlX : grainGesture.secondaryX) };
            if (heldCount < 3)
                comboCols[2] = comboCols[1];
            std::sort(comboCols.begin(), comboCols.end());

            const double posA = getGrainColumnCenterPosition(comboCols[0]);
            const double posB = getGrainColumnCenterPosition(comboCols[1]);
            const double posC = getGrainColumnCenterPosition(comboCols[2]);
            const double abDelta = computeScratchTravelDistance(posA, posB);
            const double midpoint = getWrappedSamplePosition(posA + (abDelta * 0.5), loopStartSamples, loopLengthSamplesLocal);
            const int comboHash = ((comboCols[0] * 17) + (comboCols[1] * 7) + (comboCols[2] * 3)) & 0x7fffffff;

            if (heldCount == 2)
            {
                const int seq = sceneStepIndex & 3;
                double seqPos = midpoint;
                if (seq == 0)
                    seqPos = posA;
                else if (seq == 2)
                    seqPos = posB;
                const double swing = static_cast<double>((sceneTri - 0.5f) * 0.24f)
                    * static_cast<double>(kGrainMinSizeMs + grainSizeSmoother.getCurrentValue()) * 0.001 * currentSampleRate;
                sceneCenterSample = getWrappedSamplePosition(seqPos + swing, loopStartSamples, loopLengthSamplesLocal);
            }
            else
            {
                static constexpr int permutes[6][3] {
                    {0, 1, 2}, {0, 2, 1}, {1, 0, 2},
                    {1, 2, 0}, {2, 0, 1}, {2, 1, 0}
                };
                const int permIdx = comboHash % 6;
                const int* perm = permutes[permIdx];
                const std::array<double, 3> seqNodes { posA, posB, posC };
                const double n0 = seqNodes[static_cast<size_t>(perm[0])];
                const double n1 = seqNodes[static_cast<size_t>(perm[1])];
                const double n2 = seqNodes[static_cast<size_t>(perm[2])];

                const int seq = sceneStepIndex & 7;
                double seqPos = n0;
                if (seq == 1 || seq == 4)
                    seqPos = n1;
                else if (seq == 2 || seq == 6)
                    seqPos = n2;
                else if (seq == 3 || seq == 7)
                    seqPos = midpoint;
                const double shimmer = std::sin(sceneStepFrac * juce::MathConstants<double>::twoPi)
                    * (0.14 * static_cast<double>(kGrainMinSizeMs + grainSizeSmoother.getCurrentValue()) * 0.001 * currentSampleRate);
                const double comboOffset = static_cast<double>((comboHash % 7) - 3)
                    * 0.04 * static_cast<double>(kGrainMinSizeMs + grainSizeSmoother.getCurrentValue()) * 0.001 * currentSampleRate;
                sceneCenterSample = getWrappedSamplePosition(seqPos + shimmer + comboOffset, loopStartSamples, loopLengthSamplesLocal);
            }
        }
    }

    float baseSizeMs = grainSizeSmoother.getNextValue();
    const float modulatedSizeMs = grainSizeModulatedMsAtomic.load(std::memory_order_acquire);
    if (modulatedSizeMs >= kGrainMinSizeMs)
        baseSizeMs = juce::jlimit(kGrainMinSizeMs, kGrainMaxSizeMs, modulatedSizeMs);
    float tempoSyncedSizeMs = baseSizeMs;
    const bool tempoSyncEnabled = grainTempoSyncAtomic.load(std::memory_order_acquire);
    if (tempoSyncEnabled && lastObservedTempo > 0.0)
    {
        // Size knob maps to host-tempo divisions.
        static constexpr std::array<double, 13> sizeDivisionsBeats {
            1.0 / 64.0, 1.0 / 48.0, 1.0 / 32.0, 1.0 / 24.0, 1.0 / 16.0,
            1.0 / 12.0, 1.0 / 8.0, 1.0 / 6.0, 1.0 / 4.0, 1.0 / 3.0,
            1.0 / 2.0, 1.0, 2.0
        };
        const float t = juce::jlimit(0.0f, 1.0f, (baseSizeMs - kGrainMinSizeMs) / (kGrainMaxSizeMs - kGrainMinSizeMs));
        const int idxRaw = juce::jlimit(0, static_cast<int>(sizeDivisionsBeats.size()) - 1,
                                        static_cast<int>(std::round(t * static_cast<float>(sizeDivisionsBeats.size() - 1))));
        // Stabilize sync-mode division selection: update at 1/64-beat boundaries
        // to prevent rapid index chatter that can cause audible crackles.
        const int64_t divisionBeatGroup = static_cast<int64_t>(std::floor(beatNow / (1.0 / 64.0)));
        if (divisionBeatGroup != grainTempoSyncDivisionBeatGroup
            || grainTempoSyncDivisionBeatGroup == std::numeric_limits<int64_t>::min())
        {
            grainTempoSyncDivisionBeatGroup = divisionBeatGroup;
            grainTempoSyncDivisionIndex = idxRaw;
        }
        const int idx = juce::jlimit(0, static_cast<int>(sizeDivisionsBeats.size()) - 1, grainTempoSyncDivisionIndex);
        const float tempoSyncedTargetMs = static_cast<float>(sizeDivisionsBeats[static_cast<size_t>(idx)] * (60.0 / lastObservedTempo) * 1000.0);
        // Keep sync mode hard-quantized in time to avoid division chatter artifacts.
        grainSyncedSizeSmoother.setCurrentAndTargetValue(tempoSyncedTargetMs);
        tempoSyncedSizeMs = tempoSyncedTargetMs;
    }
    else
    {
        grainTempoSyncDivisionBeatGroup = std::numeric_limits<int64_t>::min();
        grainSyncedSizeSmoother.setCurrentAndTargetValue(baseSizeMs);
    }
    // SJTR: tempo-quantized size jitter with full-range excursion at 100%.
    float sjtrSizeMs = tempoSyncedSizeMs;
    if (jitterAmount > 0.001f)
    {
        static constexpr std::array<double, 6> jitterGridBeats { 1.0 / 32.0, 1.0 / 24.0, 1.0 / 16.0, 1.0 / 12.0, 1.0 / 8.0, 1.0 / 4.0 };
        const int gridIdx = juce::jlimit(0, static_cast<int>(jitterGridBeats.size()) - 1,
            static_cast<int>(std::floor(jitterAmount * static_cast<float>(jitterGridBeats.size()))));
        const double gridBeats = jitterGridBeats[static_cast<size_t>(gridIdx)];
        const int64_t beatGroup = static_cast<int64_t>(std::floor(beatNow / juce::jmax(1.0 / 64.0, gridBeats)));
        if (beatGroup != grainSizeJitterBeatGroup)
        {
            grainSizeJitterBeatGroup = beatGroup;
            const float minSize = tempoSyncedSizeMs + ((kGrainMinSizeMs - tempoSyncedSizeMs) * jitterAmount);
            const float maxSize = tempoSyncedSizeMs + ((kGrainMaxSizeMs - tempoSyncedSizeMs) * jitterAmount);
            std::uniform_real_distribution<float> pickSize(minSize, maxSize);
            float pickedSize = pickSize(randomGenerator);

            // Keep SJTR musical when tempo sync is enabled: quantize picked size
            // to the nearest tempo division duration.
            if (tempoSyncEnabled && lastObservedTempo > 0.0)
            {
                static constexpr std::array<double, 13> sizeDivisionsBeats {
                    1.0 / 64.0, 1.0 / 48.0, 1.0 / 32.0, 1.0 / 24.0, 1.0 / 16.0,
                    1.0 / 12.0, 1.0 / 8.0, 1.0 / 6.0, 1.0 / 4.0, 1.0 / 3.0,
                    1.0 / 2.0, 1.0, 2.0
                };
                float best = static_cast<float>(sizeDivisionsBeats[0] * (60.0 / lastObservedTempo) * 1000.0);
                float bestDiff = std::abs(best - pickedSize);
                for (size_t i = 1; i < sizeDivisionsBeats.size(); ++i)
                {
                    const float ms = static_cast<float>(sizeDivisionsBeats[i] * (60.0 / lastObservedTempo) * 1000.0);
                    const float diff = std::abs(ms - pickedSize);
                    if (diff < bestDiff)
                    {
                        bestDiff = diff;
                        best = ms;
                    }
                }
                pickedSize = best;
            }

            grainSizeJitterMul = juce::jlimit(kGrainMinSizeMs, kGrainMaxSizeMs, pickedSize);
        }
        sjtrSizeMs = tempoSyncedSizeMs + ((grainSizeJitterMul - tempoSyncedSizeMs) * jitterAmount);
    }
    const float wobbleDepth = tempoSyncEnabled ? 0.03f : 0.2f;
    const float sizeWobble = 1.0f + (grainBloomAmount * wobbleDepth
                           * (0.55f + 0.45f * static_cast<float>(std::sin(grainBloomPhase * 0.61))));
    float effectiveSizeMs = juce::jlimit(kGrainMinSizeMs, kGrainMaxSizeMs, sjtrSizeMs * sizeWobble);
    const float baseDensity = grainDensitySmoother.getNextValue();
    float effectiveDensity = juce::jlimit(kGrainMinDensity, 0.82f, baseDensity + (grainBloomAmount * 0.14f));
    float effectiveSpread = juce::jlimit(0.0f, 1.0f, spreadBaseNow
                                         + (grainBloomAmount * jitterAmount * 0.2f));
    float effectiveEmitterDepth = emitterDepth;
    float scenePitchOffset = 0.0f;
    const float cloudLift = std::pow(juce::jlimit(0.0f, 1.0f, cloudDepth), 1.15f);
    if (cloudLift > 0.001f)
    {
        // Cloud should feel like a dense cluster, not a subtle tail.
        effectiveDensity = juce::jlimit(kGrainMinDensity, 0.9f, effectiveDensity + (0.24f * cloudLift));
        effectiveSpread = juce::jlimit(0.0f, 1.0f, effectiveSpread + (0.2f * cloudLift));
        effectiveEmitterDepth = juce::jlimit(0.0f, 1.0f, juce::jmax(effectiveEmitterDepth, 0.2f + (0.55f * cloudLift)));
    }

    // ARP can dominate motion perception; keep cloud/emitter influence audible while ARP is active.
    if (arpDepth > 0.001f)
    {
        const float arpMix = juce::jlimit(0.0f, 1.0f, arpDepth);
        effectiveEmitterDepth = juce::jlimit(0.0f, 1.0f,
            effectiveEmitterDepth + (0.18f + (0.38f * arpMix)) * emitterDepth);
        effectiveDensity = juce::jlimit(kGrainMinDensity, 0.88f,
            effectiveDensity + (cloudLift * (0.05f + (0.14f * arpMix))));
    }

    if (sceneMix > 0.001f)
    {
        const float gestureDepth = sceneMix * juce::jlimit(0.2f, 1.0f, 0.42f + (0.19f * static_cast<float>(heldCount)));
        const float stutter = juce::jlimit(0.0f, 1.0f, 0.45f + (0.55f * scenePulse));
        const float stretch = juce::jlimit(0.65f, 2.2f, 1.0f + (gestureDepth * (0.65f - (0.45f * stutter))));
        effectiveSizeMs = juce::jlimit(8.0f, kGrainMaxSizeMs, effectiveSizeMs * stretch);
        effectiveDensity = juce::jlimit(0.08f, 0.86f, effectiveDensity + (gestureDepth * (0.08f + (0.18f * stutter))));
        effectiveSpread = juce::jlimit(0.0f, 1.0f, effectiveSpread + (gestureDepth * (0.08f + (0.34f * sceneTri))));
        if (heldCount < 3)
            effectiveEmitterDepth = juce::jlimit(0.0f, 1.0f, effectiveEmitterDepth + (gestureDepth * (0.18f + (0.52f * stutter))));

        static constexpr std::array<int, 8> oneFingerPitch { 0, 0, 7, 12, 0, -5, 7, 0 };
        static constexpr std::array<int, 8> twoFingerPitch { 0, 12, 7, 12, 0, -12, -5, 7 };
        static constexpr std::array<int, 8> threeFingerPitch { 0, 7, 12, -5, 12, 7, -12, 0 };
        const auto& scenePitchPattern = (heldCount >= 3) ? threeFingerPitch : (heldCount == 2 ? twoFingerPitch : oneFingerPitch);
        const int patIdx = juce::jmax(0, sceneStepIndex) % static_cast<int>(scenePitchPattern.size());
        scenePitchOffset = static_cast<float>(scenePitchPattern[static_cast<size_t>(patIdx)])
            * (0.18f + (0.58f * gestureDepth));

        if (heldCount >= 3)
        {
            // Three-finger scratches: much larger grains and pronounced riser/faller motion.
            static constexpr std::array<int, 16> triContour {
                -12, -7, -3, 0, 4, 7, 12, 16, 14, 9, 5, 0, -5, -9, -12, -7
            };
            const int contourIdx = juce::jmax(0, sceneStepIndex) % static_cast<int>(triContour.size());
            const float contourPitch = static_cast<float>(triContour[static_cast<size_t>(contourIdx)])
                * (0.55f + (1.25f * gestureDepth));
            scenePitchOffset += contourPitch;

            const double phrase = std::fmod(beatNow * 0.5, 1.0);
            const float ramp = static_cast<float>((phrase < 0.5) ? (phrase * 2.0) : ((1.0 - phrase) * 2.0));
            const float dir = ((sceneStepIndex & 1) == 0) ? 1.0f : -1.0f;
            const float riserFall = dir * (ramp - 0.5f) * (24.0f + (18.0f * gestureDepth));
            scenePitchOffset += riserFall;

            // Add slower 1-bar and 2-bar macro movement for pitch/size/position.
            const float manualBeats = beatsPerLoop.load(std::memory_order_acquire);
            const double barBeats = juce::jmax(1.0, (manualBeats >= 0.0f) ? static_cast<double>(manualBeats) : 4.0);
            double barPhase = std::fmod(beatNow / barBeats, 1.0);
            if (barPhase < 0.0)
                barPhase += 1.0;
            double twoBarPhase = std::fmod(beatNow / (barBeats * 2.0), 1.0);
            if (twoBarPhase < 0.0)
                twoBarPhase += 1.0;

            const float barSweep = static_cast<float>(std::sin(barPhase * juce::MathConstants<double>::twoPi));
            const float twoBarSweep = static_cast<float>(std::sin((twoBarPhase * juce::MathConstants<double>::twoPi) + 0.8));
            scenePitchOffset += (barSweep * (6.0f + (10.0f * gestureDepth)))
                              + (twoBarSweep * (8.0f + (12.0f * gestureDepth)));

            effectiveSizeMs = juce::jlimit(80.0f, kGrainMaxSizeMs, effectiveSizeMs * (2.2f + (1.4f * gestureDepth)));
            const float sizeMacro = juce::jlimit(0.65f, 2.4f,
                1.0f + (0.52f * gestureDepth * barSweep) + (0.36f * gestureDepth * twoBarSweep));
            effectiveSizeMs = juce::jlimit(80.0f, kGrainMaxSizeMs, effectiveSizeMs * sizeMacro);
            effectiveDensity = juce::jlimit(0.06f, 0.62f, effectiveDensity * (0.84f - (0.28f * ramp)));
            jitterAmount = juce::jlimit(0.0f, 1.0f, jitterAmount * (0.44f - (0.28f * ramp)));

            const double posRangeSamples = loopLengthSamplesLocal * (0.01 + (0.08 * static_cast<double>(gestureDepth)));
            const double posMacro = (static_cast<double>(twoBarSweep) + (0.45 * static_cast<double>(barSweep))) * posRangeSamples;
            sceneCenterSample = getWrappedSamplePosition(sceneCenterSample + posMacro, loopStartSamples, loopLengthSamplesLocal);
        }
    }

    const double jitterLfo = std::sin(grainBloomPhase) + (0.45 * std::sin((grainBloomPhase * 2.37) + 1.3));
    const double jitterSamples = jitterLfo * static_cast<double>(effectiveSizeMs * 0.001f * static_cast<float>(currentSampleRate))
                                 * static_cast<double>(grainBloomAmount * randomDepth * 0.22f);
    std::uniform_real_distribution<double> markerJitterDist(-1.0, 1.0);
    const double markerJitterSamples = markerJitterDist(randomGenerator)
        * static_cast<double>(effectiveSizeMs * 0.001f * static_cast<float>(currentSampleRate))
        * static_cast<double>(randomDepth * 0.3f);
    const double centerDelta = computeScratchTravelDistance(centerSamplePos, sceneCenterSample);
    const double blendedCenter = centerSamplePos + (centerDelta * static_cast<double>(sceneMix));
    const double emitterReferenceCenter = blendedCenter;
    double bloomCenter = blendedCenter + jitterSamples + markerJitterSamples;

    const double sizeSamplesD = juce::jmax(1.0, static_cast<double>(effectiveSizeMs * 0.001f * static_cast<float>(currentSampleRate)));
    constexpr float kNeutralSizeMs = 1240.0f;
    constexpr float kNeutralDensity = 0.05f;
    const bool neutralContext = heldCount == 0
        && !grainGesture.anyHeld
        && !grainGesture.freeze
        && !grainGesture.returningToTimeline
        && !tempoSyncEnabled;
    const bool entryIdentityScheduler = neutralContext && grainEntryIdentitySamplesRemaining > 0;
    const double neutralReadPos = getWrappedSamplePosition(centerSamplePos, loopStartSamples, loopLengthSamplesLocal);
    const double neutralStep = 1.0;
    const float neutralSampleL = grainResampler.getSample(sampleBuffer, 0, neutralReadPos, neutralStep);
    const float neutralSampleR = (sampleBuffer.getNumChannels() > 1)
        ? grainResampler.getSample(sampleBuffer, 1, neutralReadPos, neutralStep)
        : neutralSampleL;

    float neutralTargetBlend = 0.0f;
    if (neutralContext)
    {
        const float dSize = std::abs(baseSizeMs - kNeutralSizeMs) / 900.0f;
        const float dDensity = std::abs(baseDensity - kNeutralDensity) / 0.08f;
        const float speedAbs = static_cast<float>(juce::jmax(0.01, std::abs(effectiveSpeed)));
        const float dSpeed = std::abs(std::log2(speedAbs)) / std::log2(1.05f);
        const float dPitch = std::abs(pitchNow) / 2.0f;
        const float dPitchJitter = pitchJitterNow / 2.0f;
        const float dSpread = spreadBaseNow / 0.2f;
        const float dJitter = jitterAmount / 0.2f;
        const float dRandom = randomDepth / 0.2f;
        const float dArp = arpDepth / 0.15f;
        const float dCloud = cloudDepth / 0.12f;
        const float dEmitter = emitterDepth / 0.12f;
        const float dEnv = envelopeNow / 0.2f;
        const float dScene = std::abs(sceneMix) / 0.1f;
        const float deviation = juce::jlimit(0.0f, 1.0f, std::max({
            dSize, dDensity, dSpeed, dPitch, dPitchJitter, dSpread, dJitter,
            dRandom, dArp, dCloud, dEmitter, dEnv, dScene
        }));
        neutralTargetBlend = 1.0f - deviation;
        neutralTargetBlend *= neutralTargetBlend;
    }

    grainNeutralBlendState += (neutralTargetBlend - grainNeutralBlendState) * 0.01f;
    const float neutralBlend = juce::jlimit(0.0f, 1.0f, grainNeutralBlendState);
    const float granularBlend = 1.0f - neutralBlend;

    if (grainSchedulerNoiseCountdown <= 0)
    {
        std::uniform_real_distribution<double> schedNoiseDist(-1.0, 1.0);
        grainSchedulerNoiseTarget = schedNoiseDist(randomGenerator);
        grainSchedulerNoiseCountdown = juce::jmax(12, static_cast<int>(std::round(0.003 * currentSampleRate)));
    }
    else
    {
        --grainSchedulerNoiseCountdown;
    }
    grainSchedulerNoise += (grainSchedulerNoiseTarget - grainSchedulerNoise) * 0.02;

    if (effectiveEmitterDepth > 0.0f && sampleLength > 0.0)
    {
        const double step = juce::jmax(1.0, sizeSamplesD);
        double inLoop = std::fmod(emitterReferenceCenter - loopStartSamples, loopLengthSamplesLocal);
        if (inLoop < 0.0)
            inLoop += loopLengthSamplesLocal;
        const double quantInLoop = std::round(inLoop / step) * step;
        const double quantCenter = loopStartSamples + std::fmod(quantInLoop, loopLengthSamplesLocal);
        bloomCenter = bloomCenter + ((quantCenter - bloomCenter) * static_cast<double>(effectiveEmitterDepth));
    }
    const double emitShape = std::pow(static_cast<double>(effectiveEmitterDepth), 1.8);
    const double overlapFactor = 0.72
        + (4.6 * static_cast<double>(effectiveDensity))
        + (8.0 * emitShape);
    const double baseSpawnRate = overlapFactor / sizeSamplesD; // grains per output sample
    const double jitterRateMul = 1.0 + (grainSchedulerNoise * static_cast<double>(0.35f + (0.65f * randomDepth)) * 0.45);
    const double emitterRateMul = 1.0 + (2.0 * emitShape);
    const double effectedSpawnRate = juce::jlimit(0.00005, 0.24, baseSpawnRate * jitterRateMul * emitterRateMul);
    double spawnRate = effectedSpawnRate * static_cast<double>(granularBlend);
    if (entryIdentityScheduler)
    {
        // Identity entry: run a single deterministic playhead-like scheduler.
        spawnRate = 1.0 / juce::jmax(1.0, sizeSamplesD);
    }
    grainSpawnAccumulator = juce::jlimit(0.0, 2.5, grainSpawnAccumulator + spawnRate);

    int spawnSafety = 0;
    const int effectedMaxSpawns = juce::jlimit(1, 6, 1 + static_cast<int>(std::round(5.0 * emitShape)));
    int maxSpawnsPerSample = juce::jlimit(1, 6, static_cast<int>(std::round(
        1.0f + ((static_cast<float>(effectedMaxSpawns) - 1.0f) * granularBlend))));
    if (entryIdentityScheduler)
        maxSpawnsPerSample = 1;
    std::array<double, 6> emitterSpawnInLoop{};
    emitterSpawnInLoop.fill(-1.0);
    while (grainSpawnAccumulator >= 1.0 && spawnSafety < maxSpawnsPerSample)
    {
        grainSpawnAccumulator -= 1.0;
        double spawnCenter = bloomCenter;
        if (effectiveEmitterDepth > 0.0f)
        {
            // Quantize around play position and distribute each emitted grain to a unique offset slot.
            double quantStep = juce::jmax(1.0, sizeSamplesD);
            if (tempoSyncEnabled && lastObservedTempo > 0.0)
            {
                // In sync mode keep emitter spacing locked to the tempo grid size.
                const double syncStep = juce::jmax(1.0, static_cast<double>(tempoSyncedSizeMs * 0.001f * static_cast<float>(currentSampleRate)));
                quantStep = syncStep;
            }
            double centerInLoop = std::fmod(emitterReferenceCenter - loopStartSamples, loopLengthSamplesLocal);
            if (centerInLoop < 0.0)
                centerInLoop += loopLengthSamplesLocal;
            const double quantizedCenterInLoop = std::round(centerInLoop / quantStep) * quantStep;

            const int slot = spawnSafety;
            const int spreadIndex = (slot == 0) ? 0 : ((slot + 1) / 2) * ((slot % 2 == 0) ? 1 : -1);
            // As emitter count/depth increases, spread further apart.
            const double emitterCountWide = static_cast<double>(juce::jmax(0, maxSpawnsPerSample - 1));
            const double densityWide = static_cast<double>(juce::jlimit(0.0f, 1.0f,
                (effectiveDensity - kGrainMinDensity) / juce::jmax(1.0e-4f, (kGrainMaxDensity - kGrainMinDensity))));
            const double spreadMul = 1.0
                + std::floor(static_cast<double>(effectiveEmitterDepth) * 12.0)
                + (1.35 * emitterCountWide * emitterCountWide)
                + (2.4 * densityWide * densityWide);
            double distributed = quantizedCenterInLoop + (static_cast<double>(spreadIndex) * quantStep * spreadMul);
            distributed = std::fmod(distributed, loopLengthSamplesLocal);
            if (distributed < 0.0)
                distributed += loopLengthSamplesLocal;

            // Collision avoidance: ensure spawned emitters are not stacked on top.
            const double densitySpacingBoost = 1.0 + (2.2 * densityWide * densityWide * densityWide);
            const double minSpacing = quantStep * juce::jmax(1.0, spreadMul * densitySpacingBoost);
            auto circularDistance = [&](double a, double b) -> double
            {
                const double d = std::abs(a - b);
                return juce::jmin(d, loopLengthSamplesLocal - d);
            };
            int guard = 0;
            bool collides = true;
            while (collides && guard++ < 16)
            {
                collides = false;
                for (int p = 0; p < spawnSafety; ++p)
                {
                    const double prev = emitterSpawnInLoop[static_cast<size_t>(p)];
                    if (prev >= 0.0 && circularDistance(distributed, prev) < minSpacing)
                    {
                        distributed += quantStep * spreadMul;
                        distributed = std::fmod(distributed, loopLengthSamplesLocal);
                        if (distributed < 0.0)
                            distributed += loopLengthSamplesLocal;
                        collides = true;
                        break;
                    }
                }
            }

            if (spawnSafety < static_cast<int>(emitterSpawnInLoop.size()))
                emitterSpawnInLoop[static_cast<size_t>(spawnSafety)] = distributed;
            spawnCenter = loopStartSamples + distributed;
        }

        spawnGrainVoice(spawnCenter,
                        effectiveSizeMs,
                        effectiveDensity,
                        effectiveSpread,
                        scenePitchOffset,
                        1.0);
        ++spawnSafety;
    }

    const bool previewRequested = grainPreviewRequestCountdown.load(std::memory_order_relaxed) > 0;
    const bool refreshPreview = previewRequested && ((++grainPreviewDecimationCounter & 0x3) == 0);
    if (refreshPreview)
    {
        for (auto& p : grainPreviewPositions)
            p.store(-1.0f, std::memory_order_release);
        for (auto& p : grainPreviewPitchNorms)
            p.store(0.0f, std::memory_order_release);
    }

    float windowSumL = 0.0f;
    float windowSumR = 0.0f;
    int previewCount = 0;
    const auto grainQuality = grainResampler.getQuality();
    for (auto& voice : grainVoices)
    {
        if (!voice.active)
            continue;

        if (voice.ageSamples >= voice.lengthSamples)
        {
            voice.active = false;
            continue;
        }

        if (refreshPreview
            && previewCount < static_cast<int>(grainPreviewPositions.size())
            && sampleLength > 0.0)
        {
            const float previewNormPos = static_cast<float>(juce::jlimit(0.0, 1.0, voice.readPos / sampleLength));
            grainPreviewPositions[static_cast<size_t>(previewCount++)].store(previewNormPos, std::memory_order_release);
            const size_t pitchIdx = static_cast<size_t>(previewCount - 1);
            const float pitchNorm = juce::jlimit(-1.0f, 1.0f, voice.pitchSemitones / 48.0f);
            grainPreviewPitchNorms[pitchIdx].store(pitchNorm, std::memory_order_release);
        }
        const float normPos = static_cast<float>(voice.ageSamples) / static_cast<float>(juce::jmax(1, voice.lengthSamples - 1));
        const int windowIdx = juce::jlimit(0, static_cast<int>(grainWindow.size()) - 1,
            static_cast<int>(std::round(normPos * static_cast<float>(grainWindow.size() - 1))));
        const float window = grainWindow[static_cast<size_t>(windowIdx)];
        float env = 1.0f;
        if (neutralBlend < 0.9999f)
        {
            float qualityEnv = 1.0f;
            switch (grainQuality)
            {
                case Resampler::Quality::Linear:
                {
                    const float tri = 1.0f - std::abs((normPos * 2.0f) - 1.0f);
                    qualityEnv = juce::jlimit(0.0f, 1.0f, tri);
                    break;
                }
                case Resampler::Quality::Cubic:
                    break;
                case Resampler::Quality::Sinc:
                    qualityEnv = 0.72f + (0.28f * std::sqrt(juce::jmax(0.0f, window)));
                    break;
                case Resampler::Quality::SincHQ:
                    qualityEnv = 0.66f + (0.34f * window);
                    break;
            }
            const float qualityMix = 1.0f - neutralBlend;
            env *= 1.0f + ((qualityEnv - 1.0f) * qualityMix);
        }
        // ENV controls how much window/envelope shaping is applied.
        // At ENV=0, keep grains as neutral as possible (flat gain).
        const float fade = juce::jlimit(0.0f, 1.0f, voice.envelopeFade);
        const float windowMix = juce::jlimit(0.0f, 1.0f, fade);
        const float shapedWindow = 1.0f + ((window - 1.0f) * windowMix);
        if (fade > 1.0e-4f)
        {
            const float fadeWidth = juce::jlimit(0.02f, 0.30f, 0.28f - (fade * 0.24f));
            float shapedFadeWidth = fadeWidth;
            float shapePos = 0.0f;
            float shapeNeg = 0.0f;
            if (std::abs(shapeNow) > 1.0e-4f)
            {
                const float bend = juce::jlimit(-1.0f, 1.0f, shapeNow);
                shapePos = juce::jmax(0.0f, bend);
                shapeNeg = juce::jmax(0.0f, -bend);
                // Positive shape can create very short, steep grains.
                shapedFadeWidth = juce::jlimit(0.0035f, 0.30f, fadeWidth * (1.0f - (0.92f * shapePos)));
            }
            const float edgeDistance = juce::jmin(normPos, 1.0f - normPos);
            const float fadeNorm = juce::jlimit(0.0f, 1.0f, edgeDistance / shapedFadeWidth);
            const float edgeExponent = 1.0f + (3.2f * fade);
            float shapedFade = std::pow(fadeNorm, edgeExponent);
            if (shapePos > 0.0f || shapeNeg > 0.0f)
            {
                if (shapePos > 0.0f)
                {
                    const float extraExp = 1.0f + (30.0f * shapePos);
                    shapedFade = std::pow(shapedFade, extraExp);
                }
                else
                {
                    const float invExp = 1.0f + (22.0f * shapeNeg);
                    shapedFade = 1.0f - std::pow(juce::jlimit(0.0f, 1.0f, 1.0f - shapedFade), invExp);
                }
            }
            env *= juce::jlimit(0.0f, 1.0f, shapedFade);

            const float centerTri = juce::jlimit(0.0f, 1.0f, 1.0f - std::abs((normPos * 2.0f) - 1.0f));
            const float centerExponent = 1.0f + (7.5f * fade);
            const float centerFocus = std::pow(centerTri, centerExponent);
            env *= juce::jlimit(0.10f, 1.0f, 0.18f + (0.82f * centerFocus));
        }
        const float amp = shapedWindow * env;
        windowSumL += amp * voice.panL;
        windowSumR += amp * voice.panR;

        float l = grainResampler.getSample(sampleBuffer, 0, voice.readPos, 1.0);
        float r = (sampleBuffer.getNumChannels() > 1)
            ? grainResampler.getSample(sampleBuffer, 1, voice.readPos, 1.0)
            : l;

        outL += l * amp * voice.panL;
        outR += r * amp * voice.panR;

        voice.readPos += voice.step;
        if (voice.readPos >= sampleLength)
            voice.readPos -= sampleLength;
        else if (voice.readPos < 0.0)
            voice.readPos += sampleLength;
        ++voice.ageSamples;
    }

    // Keep density from collapsing level with many emitters/cloud voices:
    // windowSum normalization below already controls overlap gain.
    // Dynamic overlap normalization: keep level stable when window overlaps vary.
    // Clamp denominator to avoid low-level boosting and denormal issues.
    const float densityPresence = juce::jlimit(0.0f, 1.0f, (0.55f * cloudLift) + (0.75f * effectiveEmitterDepth));
    const float normExp = juce::jlimit(0.35f, 0.85f, 0.85f - (0.42f * densityPresence));
    const float denomL = std::pow(juce::jmax(1.0f, windowSumL), normExp);
    const float denomR = std::pow(juce::jmax(1.0f, windowSumR), normExp);
    outL /= denomL;
    outR /= denomR;

    // Cloud-delay style smear feeding the granular output with short feedback tails.
    const float cloudBoost = juce::jlimit(0.0f, 1.0f, cloudDepth);
    if (grainCloudDelayBuffer.getNumSamples() > 0 && cloudBoost > 0.001f)
    {
        const int delaySize = grainCloudDelayBuffer.getNumSamples();
        const float densityNow = juce::jlimit(kGrainMinDensity, kGrainMaxDensity, grainDensitySmoother.getCurrentValue());
        const float sizeNow = juce::jlimit(kGrainMinSizeMs, kGrainMaxSizeMs, grainSizeSmoother.getCurrentValue());
        const float delayMs = juce::jlimit(10.0f, 320.0f, (sizeNow * (0.7f + (1.5f * (1.0f - densityNow)))));
        const int delaySamples = juce::jlimit(1, delaySize - 1, static_cast<int>((delayMs * 0.001f) * static_cast<float>(currentSampleRate)));
        int readPos = grainCloudDelayWritePos - delaySamples;
        if (readPos < 0)
            readPos += delaySize;

        const float delayedL = grainCloudDelayBuffer.getSample(0, readPos);
        const float delayedR = grainCloudDelayBuffer.getSample(1, readPos);
        const float feedback = juce::jlimit(0.0f, 0.95f,
            cloudBoost * (0.12f + (0.72f * (0.78f + (0.22f * jitterAmount)))));
        const float mix = juce::jlimit(0.0f, 0.9f,
            cloudBoost * (0.08f + 0.8f * (0.72f + (0.28f * grainBloomAmount))));

        const float writeL = juce::jlimit(-1.2f, 1.2f, outL + (delayedL * feedback));
        const float writeR = juce::jlimit(-1.2f, 1.2f, outR + (delayedR * feedback));
        grainCloudDelayBuffer.setSample(0, grainCloudDelayWritePos, writeL);
        grainCloudDelayBuffer.setSample(1, grainCloudDelayWritePos, writeR);
        grainCloudDelayWritePos = (grainCloudDelayWritePos + 1) % delaySize;

        outL = (outL * (1.0f - mix)) + (delayedL * mix);
        outR = (outR * (1.0f - mix)) + (delayedR * mix);
    }

    // Grain mode loudness compensation.
    // Keep neutral defaults close to unity and only add compensation as effects increase.
    const float densityNow = juce::jlimit(kGrainMinDensity, kGrainMaxDensity, grainDensitySmoother.getCurrentValue());
    const float sizeNow = juce::jlimit(kGrainMinSizeMs, kGrainMaxSizeMs, grainSizeSmoother.getCurrentValue());
    const float sizeNorm = juce::jlimit(0.0f, 1.0f, (sizeNow - kGrainMinSizeMs) / (kGrainMaxSizeMs - kGrainMinSizeMs));
    const float densityDelta = std::abs(densityNow - kNeutralDensity) / (kGrainMaxDensity - kGrainMinDensity);
    const float sizeDelta = std::abs(sizeNow - kNeutralSizeMs) / (kGrainMaxSizeMs - kGrainMinSizeMs);
    const float activity = juce::jlimit(0.0f, 1.0f,
        std::max({
            std::abs(grainPitchAtomic.load(std::memory_order_acquire)) / 48.0f,
            juce::jlimit(0.0f, 1.0f, grainPitchJitterAtomic.load(std::memory_order_acquire) / 48.0f),
            juce::jlimit(0.0f, 1.0f, grainSpreadAtomic.load(std::memory_order_acquire)),
            juce::jlimit(0.0f, 1.0f, grainJitterAtomic.load(std::memory_order_acquire)),
            juce::jlimit(0.0f, 1.0f, grainRandomDepthAtomic.load(std::memory_order_acquire)),
            juce::jlimit(0.0f, 1.0f, grainArpDepthAtomic.load(std::memory_order_acquire)),
            cloudBoost,
            emitterDepth,
            densityDelta,
            sizeDelta
        }));
    const float loudnessComp = juce::jlimit(0.9f, 2.2f,
        1.0f + (activity * (0.42f + (0.20f * (1.0f - densityNow)) + (0.10f * sizeNorm))));
    // Moderate ENV makeup (between previous full compensation and none).
    const float envelopeMakeup = juce::jlimit(1.0f, 1.35f,
        1.0f + (0.30f * std::pow(juce::jlimit(0.0f, 1.0f, envelopeNow), 1.1f)));
    outL *= (loudnessComp * envelopeMakeup);
    outR *= (loudnessComp * envelopeMakeup);

    if (neutralBlend > 1.0e-4f)
    {
        outL = (outL * granularBlend) + (neutralSampleL * neutralBlend);
        outR = (outR * granularBlend) + (neutralSampleR * neutralBlend);
    }

    if (grainEntryIdentitySamplesRemaining > 0)
        --grainEntryIdentitySamplesRemaining;
}

bool EnhancedAudioStrip::loadSampleFromFile(const juce::File& file)
{
    if (!file.existsAsFile())
    {
        DBG("Sample load rejected (missing file): " << file.getFullPathName());
        return false;
    }

    try
    {
        juce::AudioFormatManager formatManager;
        formatManager.registerBasicFormats();

        std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(file));

        if (reader == nullptr)
        {
            DBG("Sample load rejected (unsupported format): " << file.getFullPathName());
            return false;
        }

        // Validate stream metadata before any allocation/engine state changes.
        constexpr int64_t kMaxReaderSamples = 100000000; // ~38 minutes at 44.1 kHz
        constexpr int64_t kMaxIntSamples = 0x7fffffff;
        if (!std::isfinite(reader->sampleRate) || reader->sampleRate <= 0.0 || reader->sampleRate > 384000.0)
        {
            DBG("Sample load rejected (invalid sample rate): " << reader->sampleRate);
            return false;
        }

        if (reader->lengthInSamples <= 0 || reader->lengthInSamples > kMaxReaderSamples || reader->lengthInSamples > kMaxIntSamples)
        {
            DBG("Sample load rejected (invalid length): " << reader->lengthInSamples);
            return false;
        }

        if (reader->numChannels <= 0 || reader->numChannels > 8)
        {
            DBG("Sample load rejected (invalid channels): " << static_cast<int>(reader->numChannels));
            return false;
        }

        const int channelCount = static_cast<int>(reader->numChannels);
        const int sampleCount = static_cast<int>(reader->lengthInSamples);

        // Read into a temporary buffer so engine state is only mutated on full success.
        juce::AudioBuffer<float> tempBuffer;
        tempBuffer.setSize(channelCount, sampleCount, false, true, false);
        if (!reader->read(&tempBuffer, 0, sampleCount, 0, true, true))
        {
            DBG("Sample load failed during read: " << file.getFullPathName());
            return false;
        }

        juce::AudioBuffer<float> newSampleBuffer;
        newSampleBuffer.setSize(2, tempBuffer.getNumSamples(), false, true, false);

        // Convert to stereo (duplicate mono or take first two channels).
        if (tempBuffer.getNumChannels() == 1)
        {
            newSampleBuffer.copyFrom(0, 0, tempBuffer, 0, 0, tempBuffer.getNumSamples());
            newSampleBuffer.copyFrom(1, 0, tempBuffer, 0, 0, tempBuffer.getNumSamples());
        }
        else
        {
            newSampleBuffer.copyFrom(0, 0, tempBuffer, 0, 0, tempBuffer.getNumSamples());
            newSampleBuffer.copyFrom(1, 0, tempBuffer, 1, 0, tempBuffer.getNumSamples());
        }

        juce::ScopedLock lock(bufferLock);

        const bool wasPlaying = playing;
        const double previousLength = sampleLength;
        const double savedNormalizedPosition = previousLength > 0.0
            ? juce::jlimit(0.0, 1.0, playbackPosition.load() / previousLength)
            : 0.0;

        sampleBuffer.makeCopyOf(newSampleBuffer, true);
        sourceSampleRate = reader->sampleRate;
        sampleLength = static_cast<double>(sampleBuffer.getNumSamples());
        playbackPosition = juce::jlimit(0.0, juce::jmax(0.0, sampleLength - 1.0), savedNormalizedPosition * sampleLength);

        if (playMode == PlayMode::Step)
        {
            stepSampler.loadSampleFromBuffer(sampleBuffer, sourceSampleRate);
        }
        else
        {
            playing = wasPlaying;
        }

        if (transientSliceMode.load(std::memory_order_acquire))
            rebuildTransientSliceMap();
        else
            transientSliceMapDirty = true;
        grainCenterSmoother.setCurrentAndTargetValue(playbackPosition.load());
        resetGrainState();
        resetPitchShifter();
        return true;
    }
    catch (const std::exception& e)
    {
        DBG("Sample load exception: " << e.what());
        return false;
    }
    catch (...)
    {
        DBG("Sample load exception: unknown");
        return false;
    }

    return false;
}

void EnhancedAudioStrip::clearSample()
{
    juce::ScopedLock lock(bufferLock);

    sampleBuffer.setSize(0, 0, false, true, false);
    sampleLength = 0.0;
    sourceSampleRate = currentSampleRate;
    playbackPosition = 0.0;
    triggerSample = 0;
    triggerColumn = 0;
    triggerOffsetRatio = 0.0;
    triggerPpqPosition = -1.0;
    lastTriggerPPQ = -1.0;
    ppqTimelineAnchored = false;
    ppqTimelineOffsetBeats = 0.0;
    playing = false;
    wasPlayingBeforeStop = false;
    stopAfterFade = false;
    playheadSample = 0;
    loopLengthSamples = 0.0;
    stopLoopPosition = 0.0;
    transientSliceMode.store(false, std::memory_order_release);
    transientSliceMapDirty = true;
    analysisSampleCount = 0;
    analysisCacheValid = false;
    analysisRmsMap.fill(0.0f);
    analysisZeroCrossMap.fill(0);
    for (int i = 0; i < ModernAudioEngine::MaxColumns; ++i)
        transientSliceSamples[static_cast<size_t>(i)] = i;

    stepSampler.allNotesOff();
    lastStepTime = -1.0;
    stepSamplePlaying = false;
    currentStep = 0;
    resetGrainState();
    resetPitchShifter();
    resetScratchComboState();
}

void EnhancedAudioStrip::resetScratchComboState()
{
    heldButtons.clear();
    heldButtonOrder.clear();
    patternActive = false;
    activePattern = -1;
    patternHoldCountRequired = 3;
    patternStartBeat = -1.0;
    lastPatternStep = -1;
    buttonHeld = false;
    heldButton = -1;
    buttonPressTime = 0;
    scratchArrived = false;
}

void EnhancedAudioStrip::process(juce::AudioBuffer<float>& output, 
                                int startSample, 
                                int numSamples,
                                const juce::AudioPlayHead::PositionInfo& positionInfo,
                                int64_t globalSampleStart,
                                double tempo,
                                double quantizeBeats)
{
    juce::ScopedLock lock(bufferLock);

    // UI preview demand decay (message thread bumps this via preview getters).
    const int previewCountdown = grainPreviewRequestCountdown.load(std::memory_order_acquire);
    if (previewCountdown > 0)
        grainPreviewRequestCountdown.store(previewCountdown - 1, std::memory_order_release);

    // Auto-start step sequencer when DAW is playing
    bool hostIsPlaying = positionInfo.getIsPlaying();
    
    // Calculate edge detections using PREVIOUS state
    bool hostJustStarted = hostIsPlaying && !lastHostPlayingState;
    bool hostJustStopped = !hostIsPlaying && lastHostPlayingState;
    
    // Update state for NEXT callback (do this AFTER edge calculations!)
    lastHostPlayingState = hostIsPlaying;

    if (positionInfo.getPpqPosition().hasValue() && tempo > 0.0)
    {
        lastObservedPpqValid = true;
        lastObservedPPQ = *positionInfo.getPpqPosition();
        lastObservedGlobalSample = globalSampleStart;
        lastObservedTempo = tempo;
    }
    
    if (playMode == PlayMode::Step && hostJustStarted)
    {
        playing = true;

        // Hard re-sync to host PPQ phase on every transport start.
        // We set lastStepTime to one step behind so the first active block
        // immediately evaluates and fires the correct PPQ-aligned step.
        if (positionInfo.getPpqPosition().hasValue())
        {
            const double hostPpq = *positionInfo.getPpqPosition();
            const double sixteenthPos = std::floor(hostPpq * 4.0);
            lastStepTime = sixteenthPos - 1.0;
            currentStep = static_cast<int>(sixteenthPos) % 16;
            if (currentStep < 0)
                currentStep += 16;
        }
        else
        {
            lastStepTime = -1.0;
            currentStep = 0;
        }
    }
    
    if (playMode == PlayMode::Step && hostJustStopped)
    {
        playing = false;
        stepSampler.allNotesOff();
        lastStepTime = -1.0;
    }
    
    // Auto-stop audio strips when transport stops
    if (playMode != PlayMode::Step && !hostIsPlaying && playing)
    {
        wasPlayingBeforeStop = true;
        playing = false;
        scrubActive = false;
        tapeStopActive = false;
        scratchGestureActive = false;
        isReverseScratch = false;
        reverseScratchPpqRetarget = false;
        reverseScratchUseRateBlend = false;
        resetScratchComboState();
    }
    
    // Auto-resume audio strips when transport starts
    // SIMPLE: Just reset the PPQ reference to NOW, column stays the same
    if (playMode != PlayMode::Step && hostIsPlaying && wasPlayingBeforeStop)
    {
        playing = true;
        
        // Reset PPQ reference to current position
        if (positionInfo.getPpqPosition().hasValue())
        {
            triggerPpqPosition = *positionInfo.getPpqPosition();
        }
        
        wasPlayingBeforeStop = false;
    }
    
    if (!playing)
        return;
    
    // Step mode needs to run even without sample (for step indicator)
    bool hasAudio = (sampleBuffer.getNumSamples() > 0);
    
    // Early exit only for non-step modes when no audio
    if (!hasAudio && playMode != PlayMode::Step)
        return;
    
    const int numChannels = juce::jmin(output.getNumChannels(), sampleBuffer.getNumChannels());
    
    // Update smoothed targets
    smoothedVolume.setTargetValue(volume.load());
    smoothedPan.setTargetValue(pan.load());
    smoothedSpeed.setTargetValue(static_cast<float>(playbackSpeed.load()));
    smoothedFilterFrequency.setTargetValue(filterFrequency.load(std::memory_order_acquire));
    smoothedFilterResonance.setTargetValue(filterResonance.load(std::memory_order_acquire));
    smoothedFilterMorph.setTargetValue(filterMorph.load(std::memory_order_acquire));
    
    // Check if scratching (disable inner loop during scratch for full sample access)
    float stripScratch = scratchAmount.load();
    bool isScratching = (stripScratch > 0.0f) && (scrubActive || tapeStopActive || scratchGestureActive);
    
    // Pre-calculate loop parameters
    int loopCols;
    double loopStartSamples;
    double loopLength;
    
    if (isScratching)
    {
        // SCRATCHING MODE: Use FULL sample (ignore inner loop)
        loopCols = ModernAudioEngine::MaxColumns;
        loopStartSamples = 0.0;
        loopLength = sampleLength;
    }
    else
    {
        // NORMAL MODE: Use inner loop boundaries
        loopCols = loopEnd - loopStart;
        if (loopCols <= 0) loopCols = ModernAudioEngine::MaxColumns;
        loopStartSamples = loopStart * (sampleLength / ModernAudioEngine::MaxColumns);
        loopLength = (loopCols / static_cast<double>(ModernAudioEngine::MaxColumns)) * sampleLength;
    }

    double beatsForLoop = 4.0;

    auto mapLoopPositionForMode = [&](double rawPositionInLoop) -> double
    {
        auto wrap16 = [](int value) -> int
        {
            int wrapped = value % 16;
            if (wrapped < 0)
                wrapped += 16;
            return wrapped;
        };

        if (playMode == PlayMode::OneShot)
            return rawPositionInLoop;

        if (directionMode == DirectionMode::Reverse)
        {
            double wrapped = std::fmod(rawPositionInLoop, loopLength);
            if (wrapped < 0.0)
                wrapped += loopLength;
            double reversed = loopLength - wrapped;
            if (reversed >= loopLength)
                reversed -= loopLength;
            return reversed;
        }

        if (directionMode == DirectionMode::PingPong)
        {
            const double period = loopLength * 2.0;
            double phase = std::fmod(rawPositionInLoop, period);
            if (phase < 0.0)
                phase += period;
            return (phase <= loopLength) ? phase : (period - phase);
        }

        if (directionMode == DirectionMode::Random)
        {
            const double sliceLength = loopLength / 16.0;
            const double quantBeatsSafe = juce::jmax(1.0 / 32.0, quantizeBeats);
            const double beatsSafe = juce::jmax(0.25, beatsForLoop);
            const double segmentLength = juce::jmax(sliceLength, (quantBeatsSafe / beatsSafe) * loopLength);
            const double phaseRaw = rawPositionInLoop / segmentLength;
            const int segment = static_cast<int>(std::floor(phaseRaw));

            if (segment != randomLastBucket)
            {
                randomLastBucket = segment;
                std::uniform_real_distribution<float> chance(0.0f, 1.0f);
                const float p = chance(randomGenerator);

                if (p < 0.30f)
                {
                    // Downbeat bias keeps chaos musical.
                    static constexpr int anchors[4] = {0, 4, 8, 12};
                    std::uniform_int_distribution<int> pick(0, 3);
                    randomHeldSlice = anchors[pick(randomGenerator)];
                }
                else if (p < 0.75f)
                {
                    std::uniform_int_distribution<int> pick(0, 15);
                    randomHeldSlice = pick(randomGenerator);
                }
                else
                {
                    std::uniform_int_distribution<int> step(-3, 3);
                    randomHeldSlice = wrap16(randomHeldSlice + step(randomGenerator));
                }
            }

            const double segmentPhase = phaseRaw - std::floor(phaseRaw);
            const double startPosition = static_cast<double>(randomHeldSlice) * sliceLength;
            double position = startPosition + (segmentPhase * segmentLength);
            position = std::fmod(position, loopLength);
            if (position < 0.0)
                position += loopLength;
            return position;
        }

        if (directionMode == DirectionMode::RandomWalk)
        {
            const double stepLength = loopLength / 16.0;
            const int step = static_cast<int>(std::floor(rawPositionInLoop / stepLength));

            if (step != randomWalkLastBucket)
            {
                randomWalkLastBucket = step;
                std::uniform_int_distribution<int> roll(0, 7);
                const int r = roll(randomGenerator);
                int delta = 0;
                if (r <= 1) delta = -1;
                else if (r == 2) delta = -2;
                else if (r <= 4) delta = 1;
                else if (r == 5) delta = 2;

                randomWalkSlice = wrap16(randomWalkSlice + delta);
            }

            const double stepPhase = (rawPositionInLoop / stepLength) - std::floor(rawPositionInLoop / stepLength);
            return (static_cast<double>(randomWalkSlice) + stepPhase) * stepLength;
        }

        if ((patternActive && activePattern >= 0) || directionMode == DirectionMode::RandomSlice)
        {
            const double sliceLength = loopLength / 16.0;
            const double beatPos = (rawPositionInLoop / loopLength) * beatsForLoop;
            const double qBase = juce::jmax(1.0 / 32.0, quantizeBeats);
            const std::array<double, 5> quantChoices {
                qBase * 0.5,
                qBase,
                qBase * 2.0,
                qBase * 3.0,
                qBase * 4.0
            };
            const bool comboPatternActive = (patternActive && activePattern >= 0);
            const int patternId = comboPatternActive ? juce::jmax(0, activePattern) : 0;

            auto mixHash = [](uint64_t value) -> uint32_t
            {
                value ^= (value >> 33);
                value *= 0xff51afd7ed558ccdULL;
                value ^= (value >> 33);
                value *= 0xc4ceb9fe1a85ec53ULL;
                value ^= (value >> 33);
                return static_cast<uint32_t>(value & 0xffffffffULL);
            };

            std::array<int, 3> comboButtons { 0, 5, 10 };
            if (comboPatternActive)
            {
                int rank = patternId;
                bool found = false;
                for (int a = 0; a < 16 && !found; ++a)
                {
                    for (int b = a + 1; b < 16 && !found; ++b)
                    {
                        for (int c = b + 1; c < 16; ++c)
                        {
                            if (rank-- == 0)
                            {
                                comboButtons = { a, b, c };
                                found = true;
                                break;
                            }
                        }
                    }
                }
            }
            const int comboSpan = comboButtons[2] - comboButtons[0];
            int patternDigits = patternId;
            const std::array<int, 4> signature {
                patternDigits % 7,
                (patternDigits / 7) % 7,
                (patternDigits / 49) % 7,
                (patternDigits / 343) % 7
            };

            if (randomSliceNextTriggerBeat < 0.0)
            {
                randomSliceTriggerQuantBeats = qBase;
                randomSliceNextTriggerBeat = std::floor(beatPos / randomSliceTriggerQuantBeats)
                                           * randomSliceTriggerQuantBeats;
            }

            while (beatPos >= randomSliceNextTriggerBeat)
            {
                static constexpr std::array<double, 8> speedChoices { -4.0, -2.0, -1.0, 0.5, 1.0, 2.0, 4.0, 8.0 };
                if (comboPatternActive)
                {
                    const int64_t eventIndex = static_cast<int64_t>(std::llround(randomSliceNextTriggerBeat / qBase));
                    const int sigA = signature[static_cast<size_t>(eventIndex & 3)];
                    const int sigB = signature[static_cast<size_t>((eventIndex + 1) & 3)];
                    const int sigC = signature[static_cast<size_t>((eventIndex + 2) & 3)];
                    const uint64_t eventKey =
                        (static_cast<uint64_t>(patternId + 1) << 24)
                        ^ (static_cast<uint64_t>(comboButtons[0]) << 16)
                        ^ (static_cast<uint64_t>(comboButtons[1]) << 8)
                        ^ static_cast<uint64_t>(comboButtons[2])
                        ^ (static_cast<uint64_t>(eventIndex + 1) * 0x9e3779b97f4a7c15ULL);

                    const int qIdx = (sigA + sigB + static_cast<int>((eventIndex * (1 + (comboSpan % 5))) % static_cast<int64_t>(quantChoices.size())))
                                   % static_cast<int>(quantChoices.size());
                    randomSliceTriggerQuantBeats = juce::jmax(1.0 / 32.0, quantChoices[static_cast<size_t>(qIdx)]);

                    const int stride = 1 + ((comboButtons[2] - comboButtons[1] + 16) % 7);
                    randomSliceWindowStartSlice = (comboButtons[static_cast<size_t>(eventIndex % 3)]
                                                + static_cast<int>(eventIndex * stride)
                                                + sigC
                                                + (comboSpan % 3)) % 16;
                    randomSliceWindowLengthSlices = 1 + ((sigB + comboSpan + static_cast<int>(eventIndex % 4)) % 4);

                    const int sIdxA = (static_cast<int>(mixHash(eventKey ^ 0xa53c49e6ULL)) + sigA + comboButtons[0])
                                    % static_cast<int>(speedChoices.size());
                    const int sIdxB = (static_cast<int>(mixHash(eventKey ^ 0xc8013ea4ULL)) + sigC + comboButtons[2])
                                    % static_cast<int>(speedChoices.size());
                    randomSliceSpeedStart = speedChoices[static_cast<size_t>(sIdxA)];
                    randomSliceSpeedEnd = speedChoices[static_cast<size_t>(sIdxB)];

                    const int durMult = 1 + ((signature[static_cast<size_t>((eventIndex + 3) & 3)] + comboSpan + sigA) % 4);
                    const double durationCandidate = randomSliceTriggerQuantBeats * static_cast<double>(durMult);
                    randomSliceStutterDurationBeats = juce::jlimit(qBase * 0.5, 4.0, durationCandidate);
                }
                else
                {
                    std::uniform_int_distribution<int> qPick(0, static_cast<int>(quantChoices.size()) - 1);
                    const int qIdx = qPick(randomGenerator);
                    randomSliceTriggerQuantBeats = juce::jmax(1.0 / 32.0, quantChoices[static_cast<size_t>(qIdx)]);

                    std::uniform_int_distribution<int> slicePick(0, 15);
                    std::uniform_int_distribution<int> lenPick(1, 4);
                    randomSliceWindowStartSlice = slicePick(randomGenerator);
                    randomSliceWindowLengthSlices = lenPick(randomGenerator);

                    std::uniform_int_distribution<int> sPick(0, static_cast<int>(speedChoices.size()) - 1);
                    const int sIdxA = sPick(randomGenerator);
                    const int sIdxB = sPick(randomGenerator);
                    randomSliceSpeedStart = speedChoices[static_cast<size_t>(sIdxA)];
                    randomSliceSpeedEnd = speedChoices[static_cast<size_t>(sIdxB)];

                    std::uniform_int_distribution<int> durMultPick(1, 4);
                    const int durMult = durMultPick(randomGenerator);
                    const double durationCandidate = randomSliceTriggerQuantBeats * static_cast<double>(durMult);
                    randomSliceStutterDurationBeats = juce::jlimit(qBase * 0.5, 4.0, durationCandidate);
                }

                randomSliceTriggerBeat = randomSliceNextTriggerBeat;
                randomSliceNextTriggerBeat += randomSliceTriggerQuantBeats;

                // Micro fade-in for click suppression on stutter retriggers.
                crossfader.startFade(true, 64);
            }

            const double elapsedBeats = juce::jmax(0.0, beatPos - randomSliceTriggerBeat);
            const double durationBeats = juce::jmax(qBase * 0.5, randomSliceStutterDurationBeats);
            const double u = juce::jlimit(0.0, 1.0, elapsedBeats / durationBeats);
            const double speedDelta = randomSliceSpeedEnd - randomSliceSpeedStart;
            const double integratedBeats = durationBeats
                                         * ((randomSliceSpeedStart * u) + (0.5 * speedDelta * u * u));

            const double windowLength = sliceLength * juce::jmax(1, randomSliceWindowLengthSlices);
            double windowPos = integratedBeats * (loopLength / beatsForLoop);
            windowPos = std::fmod(windowPos, windowLength);
            if (windowPos < 0.0)
                windowPos += windowLength;

            const double windowStart = static_cast<double>(randomSliceWindowStartSlice) * sliceLength;
            double outPos = windowStart + windowPos;
            outPos = std::fmod(outPos, loopLength);
            if (outPos < 0.0)
                outPos += loopLength;
            return outPos;
        }

        double wrapped = std::fmod(rawPositionInLoop, loopLength);
        if (wrapped < 0.0)
            wrapped += loopLength;
        return wrapped;
    };
    
    // AUTO-WARP TO GLOBAL TEMPO:
    // CRITICAL: Always use the FULL sample's beat count for tempo calculation
    // Inner loops should NOT change the playback speed, just the looping section
    
    // Use manual setting if provided (>= 0), otherwise auto-detect from FULL sample
    float manualBeats = beatsPerLoop.load();
    if (manualBeats >= 0)
    {
        // Manual override set
        beatsForLoop = manualBeats;
    }
    else
    {
        // Auto-detect from FULL sample length (always 16 columns = 4 beats)
        // NOT from inner loop length - this keeps tempo consistent
        beatsForLoop = 4.0;  // Full sample is always 4 beats
    }
    
    // Calculate how long this loop SHOULD take at current tempo (in samples)
    double secondsPerBeat = 60.0 / tempo;
    double secondsPerLoop = beatsForLoop * secondsPerBeat;
    double targetLoopLengthInSamples = secondsPerLoop * currentSampleRate;
    
    // Calculate speed adjustment needed to make FULL sample match target
    // Use sampleLength (full sample) NOT loopLength (inner loop section)
    double autoWarpSpeed = sampleLength / targetLoopLengthInSamples;
    
    // Pre-calculate loop-invariant values
    double sampleRateRatio = sourceSampleRate / currentSampleRate;
    const double triggerOffsetRatioLocal = juce::jlimit(0.0, 0.999999, triggerOffsetRatio);
    
    // STEP SEQUENCER MODE - handle entirely separately (before main loop)
    if (playMode == PlayMode::Step)
    {
        // DON'T copy strip parameters to StepSampler here!
        // StepSampler parameters are controlled directly by monome buttons
        // (volume, pan, speed are set in PluginProcessor button handlers)

        auto triggerStepForSixteenth = [&](int64_t sixteenthIndex, double ppqForLog)
        {
            juce::ignoreUnused(ppqForLog);
            lastStepTime = static_cast<double>(sixteenthIndex);

            const int totalSteps = juce::jmax(1, getStepTotalSteps());
            const int baseStep = static_cast<int>(((sixteenthIndex % totalSteps) + totalSteps) % totalSteps);
            int nextStep = baseStep;

            switch (directionMode)
            {
                case DirectionMode::Normal:
                    nextStep = baseStep;
                    break;

                case DirectionMode::Reverse:
                    nextStep = (totalSteps - 1) - baseStep;
                    break;

                case DirectionMode::PingPong:
                {
                    const int pingPongLen = juce::jmax(1, totalSteps * 2);
                    const int cycle = static_cast<int>(((sixteenthIndex % pingPongLen) + pingPongLen) % pingPongLen);
                    nextStep = (cycle < totalSteps) ? cycle : ((pingPongLen - 1) - cycle);
                    break;
                }

                case DirectionMode::Random:
                {
                    // Musical random with gentle downbeat bias.
                    std::uniform_real_distribution<float> chance(0.0f, 1.0f);
                    const float p = chance(randomGenerator);
                    if (p < 0.28f)
                    {
                        const int anchorCount = juce::jmax(1, (totalSteps + 3) / 4);
                        std::uniform_int_distribution<int> pick(0, anchorCount - 1);
                        const int anchor = juce::jmin(totalSteps - 1, pick(randomGenerator) * 4);
                        nextStep = anchor;
                    }
                    else
                    {
                        std::uniform_int_distribution<int> pick(0, totalSteps - 1);
                        nextStep = pick(randomGenerator);
                    }
                    break;
                }

                case DirectionMode::RandomWalk:
                {
                    std::uniform_int_distribution<int> roll(0, 7);
                    const int r = roll(randomGenerator);
                    int delta = 0;
                    if (r <= 1) delta = -1;
                    else if (r == 2) delta = -2;
                    else if (r <= 4) delta = 1;
                    else if (r == 5) delta = 2;
                    stepRandomWalkPos = (stepRandomWalkPos + delta + totalSteps) % totalSteps;
                    nextStep = stepRandomWalkPos;
                    break;
                }

                case DirectionMode::RandomSlice:
                {
                    const int64_t beatGroup = sixteenthIndex / 4;
                    if (beatGroup != stepRandomSliceBeatGroup)
                    {
                        stepRandomSliceBeatGroup = beatGroup;
                        std::uniform_int_distribution<int> basePick(0, totalSteps - 1);
                        std::uniform_int_distribution<int> dirPick(0, 1);
                        stepRandomSliceBase = basePick(randomGenerator);
                        stepRandomSliceDirection = dirPick(randomGenerator) == 0 ? 1 : -1;
                    }

                    static constexpr int motif[4] = {0, 2, 1, 3};
                    const int motifStep = motif[static_cast<size_t>(sixteenthIndex & 0x3)];
                    nextStep = (stepRandomSliceBase + (stepRandomSliceDirection * motifStep) + totalSteps) % totalSteps;
                    break;
                }
            }

            currentStep = nextStep;
            if (totalSteps > 16)
                setStepPage(currentStep / 16);

            if (stepPattern[static_cast<size_t>(currentStep)])
            {
                stepSampler.triggerNote(1.0f);
            }
        };

        // Sample-accurate in-block scheduling using PPQ timeline.
        if (positionInfo.getPpqPosition().hasValue() && tempo > 0.0)
        {
            const double ppqStartRaw = *positionInfo.getPpqPosition();
            const double samplesPerBeatLocal = (60.0 / tempo) * currentSampleRate;

            int processed = 0;
            while (processed < numSamples)
            {
                const double ppqAtProcessedRaw = ppqStartRaw + (static_cast<double>(processed) / samplesPerBeatLocal);
                const double ppqAtProcessed = applySwingToPpq(ppqAtProcessedRaw);
                const int64_t sixteenthNow = static_cast<int64_t>(std::floor(ppqAtProcessed * 4.0));
                const int64_t lastSixteenth = static_cast<int64_t>(lastStepTime);

                if (sixteenthNow != lastSixteenth)
                    triggerStepForSixteenth(sixteenthNow, ppqAtProcessed);

                const double nextBoundaryPpq = (static_cast<double>(sixteenthNow) + 1.0) / 4.0;
                const double samplesToBoundary = (nextBoundaryPpq - ppqAtProcessed) * samplesPerBeatLocal;

                int segmentSamples = numSamples - processed;
                if (samplesToBoundary > 0.0)
                {
                    const int untilBoundary = static_cast<int>(std::ceil(samplesToBoundary));
                    segmentSamples = juce::jmin(segmentSamples, juce::jmax(1, untilBoundary));
                }

                stepSampler.process(output, startSample + processed, segmentSamples);
                processed += segmentSamples;
            }
        }
        else
        {
            stepSampler.process(output, startSample, numSamples);
        }
        
        // Done - return early, don't process normal audio
        return;
    }
    
    // While any scratch gesture is active, PPQ position lock is suspended.
    const bool scratchBypassPpq = (scrubActive || tapeStopActive || scratchGestureActive);
    const double speedForSync = static_cast<double>(playbackSpeed.load());
    const bool speedBypassPpq = std::abs(speedForSync - 1.0) > 1.0e-3;
    const bool bypassPpqSync = scratchBypassPpq || speedBypassPpq;

    if (speedPpqBypassActive != speedBypassPpq)
    {
        if (speedBypassPpq)
        {
            // Entering free-speed mode: pin trigger to current audible position to avoid jumps.
            const double currentPos = playbackPosition.load();
            triggerSample = globalSampleStart;
            double posInLoop = currentPos - loopStartSamples;
            if (playMode != PlayMode::OneShot)
            {
                posInLoop = std::fmod(posInLoop, loopLength);
                if (posInLoop < 0.0)
                    posInLoop += loopLength;
            }
            triggerOffsetRatio = juce::jlimit(0.0, 0.999999, posInLoop / juce::jmax(1.0, loopLength));
        }

        // Returning speed to unity hard-snaps to PPQ timeline.
        if (!speedBypassPpq && positionInfo.getPpqPosition().hasValue() && tempo > 0.0)
        {
            const double currentPpq = applySwingToPpq(*positionInfo.getPpqPosition());
            if (ppqTimelineAnchored)
            {
                const double timelineBeats = currentPpq + ppqTimelineOffsetBeats;
                const double timelinePosition = (timelineBeats / beatsForLoop) * sampleLength;
                playbackPosition = loopStartSamples + mapLoopPositionForMode(timelinePosition);
            }
            else if (triggerPpqPosition >= 0.0)
            {
                const double samplesPerBeat = (60.0 / tempo) * currentSampleRate;
                const double ppqElapsed = currentPpq - triggerPpqPosition;
                const double triggerOffset = triggerOffsetRatioLocal * loopLength;
                const double rawPos = triggerOffset + (ppqElapsed * samplesPerBeat * autoWarpSpeed);
                playbackPosition = loopStartSamples + mapLoopPositionForMode(rawPos);
            }
        }
        speedPpqBypassActive = speedBypassPpq;
    }

    // SIMPLE PPQ-LOCKED PLAYBACK
    // Position = time_since_trigger + column_offset
    if (positionInfo.getPpqPosition().hasValue() && !bypassPpqSync && playing
        && (triggerPpqPosition >= 0.0 || ppqTimelineAnchored))
    {
        double currentPpq = applySwingToPpq(*positionInfo.getPpqPosition());
        double positionInLoop = 0.0;
        double columnOffsetSamples = 0.0;
        double samplesElapsed = 0.0;
        double ppqElapsed = 0.0;

        if (ppqTimelineAnchored)
        {
            // Use unwrapped phase so Ping-Pong can produce outbound+return.
            const double timelineBeats = currentPpq + ppqTimelineOffsetBeats;
            const double timelinePosition = (timelineBeats / beatsForLoop) * sampleLength;
            positionInLoop = mapLoopPositionForMode(timelinePosition);
            playbackPosition = loopStartSamples + positionInLoop;
        }
        else
        {
            // Legacy trigger-relative PPQ behavior
            ppqElapsed = currentPpq - triggerPpqPosition;
            if (ppqElapsed < -4.0)
            {
                triggerPpqPosition = currentPpq;
                ppqElapsed = 0.0;
            }

            const double samplesPerBeat = (60.0 / tempo) * currentSampleRate;
            samplesElapsed = ppqElapsed * samplesPerBeat * autoWarpSpeed;
            columnOffsetSamples = triggerOffsetRatioLocal * loopLength;
            const double totalPosition = columnOffsetSamples + samplesElapsed;
            positionInLoop = mapLoopPositionForMode(totalPosition);
            playbackPosition = loopStartSamples + positionInLoop;
        }
        
    }
    else if (positionInfo.getPpqPosition().hasValue() && !bypassPpqSync && playing)
    {
        // ABSOLUTE PPQ MODE (for stop/restart with no trigger)
        // Position locked to timeline PPQ - no column offset
        double currentPpq = applySwingToPpq(*positionInfo.getPpqPosition());
        double samplesPerBeat = (60.0 / tempo) * currentSampleRate;
        double timelineSamples = currentPpq * samplesPerBeat * autoWarpSpeed;
        
        // Use timeline position directly - no column offset!
        double positionInLoop = mapLoopPositionForMode(timelineSamples);
        playbackPosition = loopStartSamples + positionInLoop;
    }
    else if (playing)
    {
        // FALLBACK: Sample-based timing when PPQ not available
        // Calculate elapsed samples since trigger
        int64_t currentGlobalSample = globalSampleStart;
        int64_t samplesElapsed = currentGlobalSample - triggerSample;
        
        // Start from trigger column offset
        double triggerOffset = triggerOffsetRatioLocal * loopLength;
        double swungElapsedSamples = static_cast<double>(samplesElapsed);
        const float swing = swingAmount.load(std::memory_order_acquire);
        if (swing > 1.0e-6f && tempo > 1.0e-6)
        {
            const double samplesPerBeat = (60.0 / tempo) * currentSampleRate;
            const double beatElapsed = swungElapsedSamples / juce::jmax(1.0, samplesPerBeat);
            const double swungBeatElapsed = applySwingToPpq(beatElapsed);
            swungElapsedSamples = swungBeatElapsed * samplesPerBeat;
        }

        double totalPosition = triggerOffset + (swungElapsedSamples * playbackSpeed.load());
        
        double positionInLoop = mapLoopPositionForMode(totalPosition);
        
        playbackPosition = loopStartSamples + positionInLoop;
    }
    
    for (int i = 0; i < numSamples; ++i)
    {
        if (!playing)
            break;
        
        // Get smoothed values for this sample
        float currentVol = smoothedVolume.getNextValue();
        float currentPan = smoothedPan.getNextValue();
        float currentSpeed = smoothedSpeed.getNextValue();
        
        // Declare scratch rate (used by patterns and normal scratching)
        double scratchRate = 1.0;
        bool scratchHasExplicitPosition = false;
        double scratchExplicitPosition = playbackPosition.load();
        // RHYTHMIC PATTERN EXECUTION (3-button hold)
        if (stripScratch > 0.0f && scrubActive)
        // CLOCK-LOCKED SCRATCHING: Use smoothed rate instead of fixed speed
        {
            int64_t currentGlobalSample = globalSampleStart + i;
            
            // Calculate progress through scratch (0.0 to 1.0)
            int64_t samplesIntoScratch = currentGlobalSample - scratchStartTime;
            double progress = static_cast<double>(samplesIntoScratch) / static_cast<double>(scratchDuration);
            progress = juce::jlimit(0.0, 1.0, progress);
            
            double totalDistance = scratchTravelDistance;
            if (isReverseScratch
                && reverseScratchPpqRetarget
                && ppqTimelineAnchored
                && positionInfo.getPpqPosition().hasValue()
                && tempo > 0.0)
            {
                const int64_t samplesRemaining = juce::jmax<int64_t>(0, targetSampleTime - currentGlobalSample);
                const double samplesPerBeat = (60.0 / tempo) * currentSampleRate;
                const double ppqNowAtSample = *positionInfo.getPpqPosition()
                                            + (static_cast<double>(i) / samplesPerBeat);
                const double ppqAtCompletion = ppqNowAtSample
                                             + (static_cast<double>(samplesRemaining) / samplesPerBeat);
                const double beatsForLoopSafe = juce::jmax(1.0, reverseScratchBeatsForLoop);
                double beatInLoop = std::fmod(ppqAtCompletion + scratchSavedPpqTimelineOffsetBeats, beatsForLoopSafe);
                if (beatInLoop < 0.0)
                    beatInLoop += beatsForLoopSafe;
                const double loopLengthSafe = juce::jmax(1.0, reverseScratchLoopLengthSamples);
                targetPosition = reverseScratchLoopStartSamples + ((beatInLoop / beatsForLoopSafe) * loopLengthSafe);
                totalDistance = computeScratchTravelDistance(scratchStartPosition, targetPosition);
                scratchTravelDistance = totalDistance;

            }
            if (!std::isfinite(totalDistance))
                totalDistance = targetPosition - scratchStartPosition;

            if (isReverseScratch && playMode != PlayMode::Loop)
            {
                // If return becomes impossible in remaining time, override
                // release-only timing and keep motion smooth from current pos.
                const int64_t remainingSamples = juce::jmax<int64_t>(0, targetSampleTime - currentGlobalSample);
                const double currentPosNow = playbackPosition.load();
                const int64_t feasibleRemaining = makeFeasibleScratchDuration(currentPosNow,
                                                                              targetPosition,
                                                                              remainingSamples,
                                                                              true);
                if (feasibleRemaining > remainingSamples + 64)
                {
                    scratchStartPosition = currentPosNow;
                    scratchStartTime = currentGlobalSample;
                    scratchDuration = feasibleRemaining;
                    targetSampleTime = currentGlobalSample + feasibleRemaining;
                    totalDistance = computeScratchTravelDistance(scratchStartPosition, targetPosition);
                    scratchTravelDistance = totalDistance;
                    progress = 0.0;
                }
            }
            
            if (scratchDuration <= 0)
            {
                scratchRate = 0.0;
                scratchExplicitPosition = scratchStartPosition;
                scratchHasExplicitPosition = true;
            }
            else if (isReverseScratch)
            {
                // Reverse return:
                // - Grain: accelerating profile.
                // - Loop: vinyl-like release profile (rate ramps from hand-held
                //   speed toward restore speed while landing exactly on target).
                // - Gate/OneShot: smoothstep profile (zero jerk at edges).
                double travelledNorm = 0.0;
                double envelope = 0.0;
                bool useLoopRateBlend = false;
                if (playMode == PlayMode::Grain)
                {
                    travelledNorm = std::pow(progress, kReverseScratchAccelExp);
                    envelope = (progress > 0.0)
                        ? (kReverseScratchAccelExp * std::pow(progress, kReverseScratchAccelExp - 1.0))
                        : 0.0;
                }
                else if (playMode == PlayMode::Loop)
                {
                    // Deterministic smooth catch-up: monotonic and C1 continuous.
                    travelledNorm = progress * progress * (3.0 - (2.0 * progress)); // smoothstep
                    envelope = 6.0 * progress * (1.0 - progress);
                    useLoopRateBlend = reverseScratchUseRateBlend;
                }
                else
                {
                    travelledNorm = progress * progress * (3.0 - (2.0 * progress)); // smoothstep
                    envelope = 6.0 * progress * (1.0 - progress);
                }
                const double avgRate = totalDistance / static_cast<double>(scratchDuration);
                if (useLoopRateBlend)
                {
                    scratchRate = reverseScratchStartRate
                                + ((reverseScratchEndRate - reverseScratchStartRate) * progress);
                }
                else
                {
                    scratchRate = avgRate * envelope;
                }
                scratchExplicitPosition = scratchStartPosition + (totalDistance * travelledNorm);
                scratchHasExplicitPosition = true;
            }
            else
            {
                // Forward scratch: exponentially decelerating profile.
                // Rate is normalized so integrated travel lands exactly at target.
                const double norm = 1.0 - std::exp(-kForwardScratchDecay);
                const double envelope = (kForwardScratchDecay * std::exp(-kForwardScratchDecay * progress)) / norm;
                const double avgRate = totalDistance / static_cast<double>(scratchDuration);
                scratchRate = avgRate * envelope;

                // Integrate the same envelope to get absolute scratch position.
                const double travelledNorm = (1.0 - std::exp(-kForwardScratchDecay * progress)) / norm;
                scratchExplicitPosition = scratchStartPosition + (totalDistance * travelledNorm);
                scratchHasExplicitPosition = true;
            }
            
            // Check if we've reached the target time - hard-lock to avoid drift
            if (currentGlobalSample >= targetSampleTime)
            {
                // Check if this is a reverse scratch (returning to timeline)
                if (isReverseScratch)
                {
                    // Reverse return complete. Land on the precomputed future
                    // timeline target (computed at release time), then re-lock
                    // PPQ offset from that exact landed target.
                    ppqTimelineAnchored = scratchSavedPpqTimelineAnchored;
                    ppqTimelineOffsetBeats = scratchSavedPpqTimelineOffsetBeats;
                    if (ppqTimelineAnchored && positionInfo.getPpqPosition().hasValue() && tempo > 0.0)
                    {
                        float anchoredManualBeats = beatsPerLoop.load();
                        const double anchoredBeatsForLoop = (anchoredManualBeats >= 0.0f) ? static_cast<double>(anchoredManualBeats) : 4.0;
                        const double samplesPerBeat = (60.0 / tempo) * currentSampleRate;
                        const double ppqAtSample = *positionInfo.getPpqPosition()
                                                 + (static_cast<double>(i) / samplesPerBeat);

                        playbackPosition = targetPosition;

                        double targetInLoop = std::fmod(targetPosition - loopStartSamples, loopLength);
                        if (targetInLoop < 0.0)
                            targetInLoop += loopLength;
                        const double beatInLoop = (targetInLoop / juce::jmax(1.0, loopLength)) * anchoredBeatsForLoop;
                        ppqTimelineOffsetBeats = std::fmod(beatInLoop - ppqAtSample, anchoredBeatsForLoop);
                        if (ppqTimelineOffsetBeats < 0.0)
                            ppqTimelineOffsetBeats += anchoredBeatsForLoop;

                        // Re-lock trigger references so sample/fallback paths
                        // remain continuous immediately after release.
                        triggerSample = currentGlobalSample;
                        triggerPpqPosition = ppqAtSample;
                        triggerOffsetRatio = juce::jlimit(0.0, 0.999999,
                            (targetPosition - loopStartSamples) / juce::jmax(1.0, loopLength));
                    }
                    else
                    {
                        playbackPosition = targetPosition;
                    }

                    // Exit scratch mode.
                    scrubActive = false;
                    isReverseScratch = false;
                    reverseScratchPpqRetarget = false;
                    reverseScratchUseRateBlend = false;
                    tapeStopActive = false;
                    scratchGestureActive = false;
                    scratchTravelDistance = 0.0;
                    const float restoreSpeed = static_cast<float>(playbackSpeed.load(std::memory_order_acquire));
                    smoothedSpeed.setCurrentAndTargetValue(restoreSpeed);
                    rateSmoother.setCurrentAndTargetValue(1.0);
                    scratchRate = 1.0;
                    crossfader.startFade(true, 32);
                }
                else
                {
                    // FORWARD SCRATCH ARRIVED at button position
                    scratchArrived = true;
                    heldPosition = targetPosition;
                    
                    // Snap to exact target position
                    playbackPosition = targetPosition;
                    
                    // Check if button is still held
                    if (buttonHeld)
                    {
                        // FREEZE: Audio completely stopped at button position
                        // No clock playback, no modulation - totally frozen
                        tapeStopActive = true;
                        scrubActive = false;
                        scratchTravelDistance = 0.0;
                        
                        // Set rate to 0 - completely stopped
                        rateSmoother.setCurrentAndTargetValue(0.0);
                        scratchRate = 0.0;
                        
                        // Position is frozen at targetPosition
                        // Will stay here until button released
                    }
                    else
                    {
                        // Button was released before arrival
                        // Explicitly snap back to timeline now; release callback has
                        // already happened, so do not rely on it firing again.
                        snapToTimeline(currentGlobalSample);
                        scratchRate = 1.0;
                    }
                }
            }
        }
        
        // Calculate effective speed
        // When scrubbing: use scratchRate (which already includes direction!)
        // When normal: use currentSpeed (user speed control)
        if (scrubActive && stripScratch > 0.0f
            && !(isReverseScratch && playMode == PlayMode::Loop && reverseScratchUseRateBlend))
        {
            const double clampAbs = patternActive ? kMaxPatternRateAbs : kMaxScratchRateAbs;
            scratchRate = juce::jlimit(-clampAbs, clampAbs, scratchRate);
        }
        double rateMultiplier = (scrubActive && stripScratch > 0.0f) ? scratchRate : currentSpeed;
        
        double effectiveSpeed = rateMultiplier * autoWarpSpeed * sampleRateRatio;
        if (scrubActive && stripScratch > 0.0f && !patternActive)
        {
            // Gesture scratch rates are already absolute (buffer samples/output sample).
            // Do not re-scale by tempo warp or source sample ratio.
            effectiveSpeed = scratchRate;
        }

        float uiDisplaySpeed = juce::jlimit(0.0f, 4.0f, std::abs(static_cast<float>(rateMultiplier)));
        if (playMode == PlayMode::Grain)
        {
            const float grainScratch = scratchAmount.load(std::memory_order_acquire);
            if (grainScratch <= 0.001f)
            {
                uiDisplaySpeed = 0.0f;
            }
            else
            {
                const double remain = std::abs(computeScratchTravelDistance(grainGesture.centerSampleSmoothed,
                                                                            grainGesture.targetCenterSample));
                const double startDist = juce::jmax(1.0, grainGesture.centerTravelDistanceAbs);
                const double progress = juce::jlimit(0.0, 1.0, 1.0 - (remain / startDist));
                const float baseDisplay = juce::jlimit(0.1f, 4.0f, std::abs(static_cast<float>(playbackSpeed.load(std::memory_order_acquire))));
                const float expFalloff = static_cast<float>(std::exp(-4.2 * progress));
                uiDisplaySpeed = (remain < 1.0) ? 0.0f : juce::jlimit(0.0f, 4.0f, baseDisplay * expFalloff);
            }
        }
        else if (scrubActive && stripScratch > 0.0f)
        {
            uiDisplaySpeed = juce::jlimit(0.0f, 4.0f, std::abs(static_cast<float>(scratchRate)));
        }
        displaySpeedAtomic.store(uiDisplaySpeed, std::memory_order_release);
        
        // Apply direction mode when not scratching
        // (scratching rate already includes direction)
        if (!scrubActive)
        {
            switch (directionMode)
            {
                case DirectionMode::Normal:
                    // Forward - no change
                    break;
                    
                case DirectionMode::Reverse:
                    // Direction is handled by position mapper.
                    break;
                    
                case DirectionMode::PingPong:
                case DirectionMode::Random:
                case DirectionMode::RandomWalk:
                case DirectionMode::RandomSlice:
                    // Will be handled in position calculation
                    break;
            }
        }
        
        // Position calculation (skip for step mode - already calculated above)
        double positionInLoop = 0.0;
        double samplePosition = 0.0;
        
        if (playMode == PlayMode::Step)
        {
            // Step mode: position already set above, just use it
            samplePosition = playbackPosition.load();
            positionInLoop = std::fmod(samplePosition - loopStartSamples, loopLength);
            if (positionInLoop < 0.0)
                positionInLoop += loopLength;
        }
        else
        {
            // CRITICAL: When PPQ sync is active, we calculated starting position before loop
            // Now let the normal position calculation run to advance per-sample
            // But we won't write it back to playbackPosition (we'll update that from PPQ next buffer)
            
            // Position calculation for normal modes:
            // When FROZEN (tape stop): Position stays locked, no advancement
            // When SCRATCHING: advance from current position using scratch rate
            // When NORMAL: calculate from trigger point
        
        if (tapeStopActive)
        {
            // FROZEN MODE: Position completely locked
            // No clock playback, no advancement - totally frozen
            double currentPosInLoop = playbackPosition.load() - loopStartSamples;
            if (playMode != PlayMode::OneShot)
            {
                currentPosInLoop = std::fmod(currentPosInLoop, loopLength);
                if (currentPosInLoop < 0)
                    currentPosInLoop += loopLength;
            }
            
            positionInLoop = currentPosInLoop;  // No change!
            effectiveSpeed = 0.0;  // Override speed to 0
        }
        else if (scrubActive && stripScratch > 0.0f)
        {
            // SCRATCHING MODE: Use absolute, time-based position (like RandomSlice style),
            // rather than integrating incremental deltas. This avoids drift/alias-like feel.
            if (scratchHasExplicitPosition)
            {
                positionInLoop = scratchExplicitPosition - loopStartSamples;
            }
            else
            {
                // Fallback if explicit position was not produced this sample.
                positionInLoop = playbackPosition.load() - loopStartSamples;
            }

            // Handle wrapping for looping modes only.
            if (playMode != PlayMode::OneShot)
            {
                positionInLoop = std::fmod(positionInLoop, loopLength);
                if (positionInLoop < 0.0)
                    positionInLoop += loopLength;
            }
        }
        else
        {
            // NORMAL MODE: Calculate position for this sample
            bool ppqSyncActiveForCalc = positionInfo.getPpqPosition().hasValue() && !bypassPpqSync && playing
                                      && (triggerPpqPosition >= 0.0 || ppqTimelineAnchored);
            
            if (ppqSyncActiveForCalc)
            {
                // PPQ sync: derive sample positions from swung timeline phase.
                // Compute PPQ at this sample index so swing can audibly affect playback,
                // not just marker display.
                const auto ppqOpt = positionInfo.getPpqPosition();
                const double basePpq = ppqOpt.hasValue() ? *ppqOpt : 0.0;
                const double samplesPerBeatLocal = secondsPerBeat * currentSampleRate;
                const double ppqPerSample = (samplesPerBeatLocal > 0.0) ? (1.0 / samplesPerBeatLocal) : 0.0;
                const double ppqAtSampleRaw = basePpq + (static_cast<double>(i) * ppqPerSample);
                const double currentPpq = applySwingToPpq(ppqAtSampleRaw);
                double rawBase = 0.0;
                if (ppqTimelineAnchored)
                {
                    const double timelineBeats = currentPpq + ppqTimelineOffsetBeats;
                    const double timelineRate = juce::jmax(0.0, std::abs(rateMultiplier));
                    // Keep PPQ-derived phase, but let playback rate scale that phase in loop mode
                    // so speed modulation/control affects both audio and marker motion.
                    rawBase = ((timelineBeats * timelineRate) / beatsForLoop) * sampleLength;
                }
                else if (triggerPpqPosition >= 0.0)
                {
                    const double samplesPerBeat = (60.0 / tempo) * currentSampleRate;
                    const double ppqElapsed = currentPpq - triggerPpqPosition;
                    const double columnOffsetSamples = triggerOffsetRatioLocal * loopLength;
                    rawBase = columnOffsetSamples + (ppqElapsed * samplesPerBeat * autoWarpSpeed);
                }
                else
                {
                    int64_t currentGlobalSample = globalSampleStart + i;
                    int64_t samplesElapsed = currentGlobalSample - triggerSample;
                    double triggerOffset = triggerOffsetRatioLocal * loopLength;
                    rawBase = triggerOffset + (samplesElapsed * effectiveSpeed);
                }

                positionInLoop = mapLoopPositionForMode(rawBase);
                }
                else
                {
                    // FALLBACK: Sample-based timing (when PPQ not available)
                int64_t currentGlobalSample = globalSampleStart + i;
                int64_t samplesElapsed = currentGlobalSample - triggerSample;
                
                double triggerOffset = triggerOffsetRatioLocal * loopLength;
                    positionInLoop = mapLoopPositionForMode(triggerOffset + (samplesElapsed * effectiveSpeed));
                }
                
            }
            samplePosition = loopStartSamples + positionInLoop;

            if (playMode == PlayMode::OneShot
                && (positionInLoop < 0.0 || positionInLoop >= loopLength))
            {
                // One-shot stops at boundaries instead of looping.
                playing = false;
                scrubActive = false;
                tapeStopActive = false;
                scratchGestureActive = false;
                resetScratchComboState();
                playbackPosition = (positionInLoop < 0.0)
                    ? loopStartSamples
                    : (loopStartSamples + loopLength);
                break;
            }

            // Loop/Gate/OneShot keep legacy PPQ/fallback behavior here.
            // Grain writes playbackPosition after grain center is computed.
            if (playMode != PlayMode::Grain)
            {
                // Keep playback marker state updated from the PPQ-derived position as well,
                // so visual/playhead feedback reflects modulated speed in loop mode.
                playbackPosition = samplePosition;
            }

        // Hold state silence only once we have fully stopped at target.
        if (tapeStopActive)
            continue;

        // Get crossfade value (for triggers)
        float fadeValue = crossfader.isActive() ? crossfader.getNextValue() : 1.0f;
        if (stopAfterFade && !crossfader.isActive() && fadeValue <= 1.0e-4f)
        {
            stopAfterFade = false;
            playing = false;
            playbackPosition = 0.0;
            scrubActive = false;
            tapeStopActive = false;
            scratchGestureActive = false;
            resetScratchComboState();
            break;
        }
        
        // INNER LOOP CROSSFADE:
        // Blend BEFORE loop start (pre-roll) into end of loop
        float innerLoopBlend = 0.0f;  // Amount to blend pre-roll sample (0-1)
        double prerollSamplePosition = samplePosition;
        
        const double crossfadeLengthMsLocal = static_cast<double>(loopCrossfadeLengthMs.load(std::memory_order_acquire));
        const double crossfadeSamples = (crossfadeLengthMsLocal * 0.001) * currentSampleRate;
        
        // Only apply crossfade if we have an actual inner loop (not full 16 columns)
        if (loopCols < ModernAudioEngine::MaxColumns && crossfadeSamples > 0 && crossfadeSamples < loopLength)
        {
            double fadeStart = loopLength - crossfadeSamples;
            
            // Are we in the crossfade zone at the end of the loop?
            if (positionInLoop >= fadeStart)
            {
                // Calculate crossfade position (0.0 to 1.0)
                float t = static_cast<float>((positionInLoop - fadeStart) / (crossfadeSamples - 1.0));
                t = juce::jlimit(0.0f, 1.0f, t);
                
                // Equal-power crossfade: fadeIn amount (0 → 1)
                const float pi_2 = 3.14159265359f * 0.5f;
                innerLoopBlend = std::sqrt(juce::jlimit(0.0f, 1.0f, std::sin(t * pi_2)));
                
                // Calculate position BEFORE loop start (pre-roll)
                // We're at the end of the loop, need audio from before the start
                double offsetIntoFade = positionInLoop - fadeStart;
                prerollSamplePosition = loopStartSamples - crossfadeSamples + offsetIntoFade;
                
                // Handle wrapping if pre-roll goes before sample start
                if (prerollSamplePosition < 0)
                    prerollSamplePosition += sampleLength;
            }
        }
        
        // Calculate pan gains ONCE (not per channel)
        const float pi = 3.14159265359f;
        float panAngle = (currentPan + 1.0f) * 0.5f * pi * 0.5f;  // Map -1..1 to 0..pi/2
        float leftGain = std::cos(panAngle);
        float rightGain = std::sin(panAngle);
        
        // Read and sum all channels from source, then apply pan
        float leftSample = 0.0f;
        float rightSample = 0.0f;

        if (playMode == PlayMode::Grain)
        {
            grainCenterSmoother.setTargetValue(grainGesture.targetCenterSample);
            grainFreezeBlendSmoother.setTargetValue(grainGesture.freeze ? 1.0f : 0.0f);
            const float freezeBlend = grainFreezeBlendSmoother.getNextValue();
            const double frozenCenter = grainCenterSmoother.getNextValue();
            const double grainCenterRaw = samplePosition + ((frozenCenter - samplePosition) * static_cast<double>(freezeBlend));
            const double grainCenter = getWrappedSamplePosition(grainCenterRaw, loopStartSamples, loopLength);
            grainGesture.centerSampleSmoothed = grainCenter;
            playbackPosition = grainCenter;

            if (grainGesture.returningToTimeline && !grainGesture.anyHeld)
            {
                const double remain = std::abs(computeScratchTravelDistance(grainCenter, grainGesture.targetCenterSample));
                if (remain < 1.0)
                {
                    grainGesture.returningToTimeline = false;
                    grainGesture.freeze = false;
                    grainGesture.centerTravelDistanceAbs = 0.0;
                }
            }

            float grainL = 0.0f;
            float grainR = 0.0f;
            renderGrainAtSample(grainL, grainR, grainCenter, effectiveSpeed, globalSampleStart + i);

            leftSample = grainL * leftGain;
            rightSample = grainR * rightGain;
        }
        else if (numChannels == 1)
        {
            // Mono source: read once, apply to both with pan
            float monoSample = resampler.getSample(sampleBuffer, 0, samplePosition, playbackSpeed);
            
            // Apply inner loop crossfade if active
            if (innerLoopBlend > 0.0f)
            {
                float prerollSample = resampler.getSample(sampleBuffer, 0, prerollSamplePosition, playbackSpeed);
                const float fadeOutTerm = juce::jlimit(0.0f, 1.0f, 1.0f - (innerLoopBlend * innerLoopBlend));
                float fadeOut = std::sqrt(fadeOutTerm);  // Complementary
                monoSample = (monoSample * fadeOut) + (prerollSample * innerLoopBlend);
            }
            
            leftSample = monoSample * leftGain;
            rightSample = monoSample * rightGain;
        }
        else if (numChannels == 2)
        {
            // Stereo source: preserve stereo and apply constant-power balance.
            float leftSource = resampler.getSample(sampleBuffer, 0, samplePosition, playbackSpeed);
            float rightSource = resampler.getSample(sampleBuffer, 1, samplePosition, playbackSpeed);
            
            // Apply inner loop crossfade if active
            if (innerLoopBlend > 0.0f)
            {
                float leftPreroll = resampler.getSample(sampleBuffer, 0, prerollSamplePosition, playbackSpeed);
                float rightPreroll = resampler.getSample(sampleBuffer, 1, prerollSamplePosition, playbackSpeed);
                const float fadeOutTerm = juce::jlimit(0.0f, 1.0f, 1.0f - (innerLoopBlend * innerLoopBlend));
                float fadeOut = std::sqrt(fadeOutTerm);  // Complementary
                
                leftSource = (leftSource * fadeOut) + (leftPreroll * innerLoopBlend);
                rightSource = (rightSource * fadeOut) + (rightPreroll * innerLoopBlend);
            }
            
            leftSample = leftSource * leftGain;
            rightSample = rightSource * rightGain;
        }
        
        if (retriggerBlendActive
            && retriggerBlendSamplesRemaining > 0
            && retriggerBlendTotalSamples > 0
            && playMode != PlayMode::Step
            && playMode != PlayMode::Grain)
        {
            float oldLeft = 0.0f;
            float oldRight = 0.0f;
            const double oldPos = retriggerBlendOldPosition;

            if (numChannels == 1)
            {
                const float monoOld = resampler.getSample(sampleBuffer, 0, oldPos, playbackSpeed);
                oldLeft = monoOld * leftGain;
                oldRight = monoOld * rightGain;
            }
            else if (numChannels == 2)
            {
                oldLeft = resampler.getSample(sampleBuffer, 0, oldPos, playbackSpeed) * leftGain;
                oldRight = resampler.getSample(sampleBuffer, 1, oldPos, playbackSpeed) * rightGain;
            }

            const float progress = 1.0f - (static_cast<float>(retriggerBlendSamplesRemaining)
                                           / static_cast<float>(retriggerBlendTotalSamples));
            const float x = juce::jlimit(0.0f, 1.0f, progress);
            const float inGain = std::sin(juce::MathConstants<float>::halfPi * x);
            const float outGain = std::cos(juce::MathConstants<float>::halfPi * x);

            leftSample = (leftSample * inGain) + (oldLeft * outGain);
            rightSample = (rightSample * inGain) + (oldRight * outGain);

            const double oldAdvance = std::isfinite(effectiveSpeed) ? effectiveSpeed : 0.0;
            if (playMode == PlayMode::OneShot)
                retriggerBlendOldPosition = juce::jlimit(0.0, juce::jmax(0.0, sampleLength - 1.0), oldPos + oldAdvance);
            else
                retriggerBlendOldPosition = getWrappedSamplePosition(oldPos + oldAdvance, loopStartSamples, loopLength);

            --retriggerBlendSamplesRemaining;
            if (retriggerBlendSamplesRemaining <= 0)
            {
                retriggerBlendActive = false;
                retriggerBlendSamplesRemaining = 0;
                retriggerBlendTotalSamples = 0;
            }
        }

        // Apply volume and crossfade
        float finalGainLeft = currentVol * fadeValue;
        float finalGainRight = currentVol * fadeValue;

        // Pitch shift is tempo-preserving and independent from playback speed control.
        if (playMode != PlayMode::Step)
            processPitchShift(leftSample, rightSample);
        
        // Apply filter if enabled
        if (filterEnabled)
        {
            const float filtFreq = smoothedFilterFrequency.getNextValue();
            const float filtRes = smoothedFilterResonance.getNextValue();
            const float filtMorph = smoothedFilterMorph.getNextValue();
            processFilterSample(leftSample, rightSample, filtFreq, filtRes, filtMorph);
        }

        // Tempo-synced gate effect (independent from PlayMode::Gate trigger behavior).
        if (positionInfo.getPpqPosition().hasValue() && tempo > 0.0f)
        {
            const double samplesPerBeatLocal = (60.0 / tempo) * currentSampleRate;
            const double ppqAtSampleRaw = *positionInfo.getPpqPosition()
                                        + (static_cast<double>(i) / samplesPerBeatLocal);
            const float gateMod = computeGateModulation(applySwingToPpq(ppqAtSampleRaw));
            leftSample *= gateMod;
            rightSample *= gateMod;
        }

        if (!std::isfinite(leftSample)) leftSample = 0.0f;
        if (!std::isfinite(rightSample)) rightSample = 0.0f;
        if (!std::isfinite(finalGainLeft)) finalGainLeft = 0.0f;
        if (!std::isfinite(finalGainRight)) finalGainRight = 0.0f;

        float outL = leftSample * finalGainLeft;
        float outR = rightSample * finalGainRight;

        if (triggerOutputBlendActive
            && triggerOutputBlendSamplesRemaining > 0
            && triggerOutputBlendTotalSamples > 0)
        {
            const float progress = 1.0f - (static_cast<float>(triggerOutputBlendSamplesRemaining)
                                           / static_cast<float>(triggerOutputBlendTotalSamples));
            const float t = juce::jlimit(0.0f, 1.0f, progress);
            outL = (triggerOutputBlendStartL * (1.0f - t)) + (outL * t);
            outR = (triggerOutputBlendStartR * (1.0f - t)) + (outR * t);

            --triggerOutputBlendSamplesRemaining;
            if (triggerOutputBlendSamplesRemaining <= 0)
            {
                triggerOutputBlendActive = false;
                triggerOutputBlendSamplesRemaining = 0;
                triggerOutputBlendTotalSamples = 0;
            }
        }

        if (!std::isfinite(outL)) outL = 0.0f;
        if (!std::isfinite(outR)) outR = 0.0f;

        output.addSample(0, startSample + i, outL);
        output.addSample(1, startSample + i, outR);
        lastOutputSampleL = outL;
        lastOutputSampleR = outR;
    }
}
}  // Extra closing brace to fix imbalance

void EnhancedAudioStrip::trigger(int column, double tempo, bool quantized)
{
    juce::ScopedLock lock(bufferLock);
    const bool wasPlaying = playing;

    (void) tempo;
    (void) quantized;
    // STEP SEQUENCER MODE - do nothing (steps are toggled via triggerAtSample)
    if (playMode == PlayMode::Step)
    {
        return;  // Step mode doesn't use this method
    }
    
    triggerColumn = column;
    triggerSample = 0;  // Unknown global sample
    triggerPpqPosition = -1.0;  // Reset PPQ - will be set on next process()
    ppqTimelineAnchored = false;
    
    // Calculate loop length in samples
    int loopCols = loopEnd - loopStart;
    if (loopCols <= 0) loopCols = ModernAudioEngine::MaxColumns;
    loopLengthSamples = (loopCols / static_cast<double>(ModernAudioEngine::MaxColumns)) * sampleLength;
    
    double loopStartSamples = loopStart * (sampleLength / ModernAudioEngine::MaxColumns);
    const double rawTargetPosition = getTriggerTargetPositionForColumn(column, loopStartSamples, loopLengthSamples);
    const bool transientModeActive = transientSliceMode.load(std::memory_order_acquire);
    const int zeroSnapRadius = transientModeActive ? 0 : juce::jmax(8, static_cast<int>(currentSampleRate * 0.0007)); // ~0.7ms
    const double newTargetPosition = (zeroSnapRadius > 0)
        ? snapToNearestZeroCrossing(rawTargetPosition, zeroSnapRadius)
        : rawTargetPosition;

    // Crossfade old read-head into new trigger target to reduce retrigger clicks
    // on sustained material without altering PPQ timeline math.
    if (wasPlaying && sampleLength > 1.0 && playMode != PlayMode::Step && playMode != PlayMode::Grain)
    {
        const float triggerFadeMs = triggerFadeInMs.load(std::memory_order_acquire);
        if (retriggerBlendActive && retriggerBlendSamplesRemaining > 0 && retriggerBlendTotalSamples > 0)
        {
            const float progress = 1.0f - (static_cast<float>(retriggerBlendSamplesRemaining)
                                           / static_cast<float>(retriggerBlendTotalSamples));
            const float x = juce::jlimit(0.0f, 1.0f, progress);
            const float inGain = std::sin(juce::MathConstants<float>::halfPi * x);
            const float outGain = std::cos(juce::MathConstants<float>::halfPi * x);
            const double newPos = playbackPosition.load(std::memory_order_acquire);
            retriggerBlendOldPosition = (retriggerBlendOldPosition * static_cast<double>(outGain))
                                      + (newPos * static_cast<double>(inGain));
        }
        else
            retriggerBlendOldPosition = playbackPosition.load(std::memory_order_acquire);
        retriggerBlendTotalSamples = juce::jmax(16, static_cast<int>(currentSampleRate * 0.001 * triggerFadeMs));
        retriggerBlendSamplesRemaining = retriggerBlendTotalSamples;
        retriggerBlendActive = true;
        triggerOutputBlendTotalSamples = juce::jmax(16, static_cast<int>(currentSampleRate * 0.001 * triggerFadeMs));
        triggerOutputBlendSamplesRemaining = triggerOutputBlendTotalSamples;
        triggerOutputBlendStartL = lastOutputSampleL;
        triggerOutputBlendStartR = lastOutputSampleR;
        triggerOutputBlendActive = true;
    }
    else
    {
        retriggerBlendActive = false;
        retriggerBlendSamplesRemaining = 0;
        retriggerBlendTotalSamples = 0;
        triggerOutputBlendActive = false;
        triggerOutputBlendSamplesRemaining = 0;
        triggerOutputBlendTotalSamples = 0;
    }
    playbackPosition = newTargetPosition;
    triggerOffsetRatio = juce::jlimit(0.0, 0.999999, (newTargetPosition - loopStartSamples) / juce::jmax(1.0, loopLengthSamples));

    if (playMode == PlayMode::Grain)
    {
        setGrainCenterTarget(newTargetPosition, false);
        grainGesture.freeze = (grainGesture.heldCount > 0);
        updateGrainHeldLedState();
    }
    
    stopAfterFade = false;
    playing = true;
    
    // Configurable fade-in to suppress discontinuities on sustained material retriggers.
    const float triggerFadeMs = triggerFadeInMs.load(std::memory_order_acquire);
    int fadeSamples = juce::jmax(16, static_cast<int>(currentSampleRate * 0.001 * triggerFadeMs));
    if (!wasPlaying)
        crossfader.startFade(true, fadeSamples, true);
}

void EnhancedAudioStrip::triggerAtSample(int column, double tempo, int64_t globalSample,
                                         const juce::AudioPlayHead::PositionInfo& positionInfo)
{
    juce::ScopedLock lock(bufferLock);
    const bool wasPlaying = playing;

    // STEP SEQUENCER MODE
    if (playMode == PlayMode::Step)
    {
        toggleStepAtVisibleColumn(column);
        const int absoluteStep = getVisibleStepOffset() + juce::jlimit(0, 15, column);
        juce::ignoreUnused(absoluteStep);
        
        // Don't trigger playback immediately - steps trigger on clock
        return;
    }
    
    // Calculate loop length in samples
    int loopCols = loopEnd - loopStart;
    if (loopCols <= 0) loopCols = ModernAudioEngine::MaxColumns;
    loopLengthSamples = (loopCols / static_cast<double>(ModernAudioEngine::MaxColumns)) * sampleLength;
    
    // Update trigger info
    triggerColumn = column;
    triggerSample = globalSample;
    playheadSample = 0;
    randomLastBucket = -1;
    randomWalkLastBucket = -1;
    randomSliceLastBucket = -1;
    randomSliceRepeatsRemaining = 0;
    randomSliceNextTriggerBeat = -1.0;
    
    // CRITICAL: Store PPQ position when triggered for timeline sync
    if (positionInfo.getPpqPosition().hasValue())
    {
        triggerPpqPosition = *positionInfo.getPpqPosition();
        lastTriggerPPQ = triggerPpqPosition;  // Track when we last fired

        // One-shot should be trigger-relative and must not be phase-wrapped
        // to host timeline. Loop/Gate keep timeline anchoring.
        const double loopStartSamples = loopStart * (sampleLength / ModernAudioEngine::MaxColumns);
        const double rawTriggerTargetPos = getTriggerTargetPositionForColumn(column, loopStartSamples, loopLengthSamples);
        const bool transientModeActive = transientSliceMode.load(std::memory_order_acquire);
        const int zeroSnapRadius = transientModeActive ? 0 : juce::jmax(8, static_cast<int>(currentSampleRate * 0.0007)); // ~0.7ms
        const double triggerTargetPos = (zeroSnapRadius > 0)
            ? snapToNearestZeroCrossing(rawTriggerTargetPos, zeroSnapRadius)
            : rawTriggerTargetPos;
        triggerOffsetRatio = juce::jlimit(0.0, 0.999999, (triggerTargetPos - loopStartSamples) / juce::jmax(1.0, loopLengthSamples));

        if (playMode != PlayMode::OneShot)
        {
            // Build a timeline anchor so strip position can be:
            // absolute host PPQ phase + selected row offset.
            float manualBeats = beatsPerLoop.load();
            const double beatsForLoop = (manualBeats >= 0.0f) ? static_cast<double>(manualBeats) : 4.0;
            const double targetBeatOffset = triggerOffsetRatio * beatsForLoop;
            double currentBeatInLoop = std::fmod(triggerPpqPosition, beatsForLoop);
            if (currentBeatInLoop < 0.0)
                currentBeatInLoop += beatsForLoop;

            ppqTimelineOffsetBeats = targetBeatOffset - currentBeatInLoop;
            ppqTimelineOffsetBeats = std::fmod(ppqTimelineOffsetBeats, beatsForLoop);
            if (ppqTimelineOffsetBeats < 0.0)
                ppqTimelineOffsetBeats += beatsForLoop;
            ppqTimelineAnchored = true;
        }
        else
        {
            ppqTimelineAnchored = false;
            ppqTimelineOffsetBeats = 0.0;
        }
    }
    else
    {
        triggerPpqPosition = -1.0;  // No PPQ available (fallback to free-running)
        ppqTimelineAnchored = false;
    }
    
    // Calculate target position for this column
    const double loopStartSamples = loopStart * (sampleLength / ModernAudioEngine::MaxColumns);
    const double rawTargetPosition = getTriggerTargetPositionForColumn(column, loopStartSamples, loopLengthSamples);
    const bool transientModeActive = transientSliceMode.load(std::memory_order_acquire);
    const int zeroSnapRadius = transientModeActive ? 0 : juce::jmax(8, static_cast<int>(currentSampleRate * 0.0007)); // ~0.7ms
    const double newTargetPosition = (zeroSnapRadius > 0)
        ? snapToNearestZeroCrossing(rawTargetPosition, zeroSnapRadius)
        : rawTargetPosition;

    // Crossfade old read-head into new trigger target to reduce retrigger clicks
    // on sustained material without altering PPQ timeline math.
    if (wasPlaying && sampleLength > 1.0 && playMode != PlayMode::Step && playMode != PlayMode::Grain)
    {
        const float triggerFadeMs = triggerFadeInMs.load(std::memory_order_acquire);
        if (retriggerBlendActive && retriggerBlendSamplesRemaining > 0 && retriggerBlendTotalSamples > 0)
        {
            const float progress = 1.0f - (static_cast<float>(retriggerBlendSamplesRemaining)
                                           / static_cast<float>(retriggerBlendTotalSamples));
            const float x = juce::jlimit(0.0f, 1.0f, progress);
            const float inGain = std::sin(juce::MathConstants<float>::halfPi * x);
            const float outGain = std::cos(juce::MathConstants<float>::halfPi * x);
            const double newPos = playbackPosition.load(std::memory_order_acquire);
            retriggerBlendOldPosition = (retriggerBlendOldPosition * static_cast<double>(outGain))
                                      + (newPos * static_cast<double>(inGain));
        }
        else
            retriggerBlendOldPosition = playbackPosition.load(std::memory_order_acquire);
        retriggerBlendTotalSamples = juce::jmax(16, static_cast<int>(currentSampleRate * 0.001 * triggerFadeMs));
        retriggerBlendSamplesRemaining = retriggerBlendTotalSamples;
        retriggerBlendActive = true;
        triggerOutputBlendTotalSamples = juce::jmax(16, static_cast<int>(currentSampleRate * 0.001 * triggerFadeMs));
        triggerOutputBlendSamplesRemaining = triggerOutputBlendTotalSamples;
        triggerOutputBlendStartL = lastOutputSampleL;
        triggerOutputBlendStartR = lastOutputSampleR;
        triggerOutputBlendActive = true;
    }
    else
    {
        retriggerBlendActive = false;
        retriggerBlendSamplesRemaining = 0;
        retriggerBlendTotalSamples = 0;
        triggerOutputBlendActive = false;
        triggerOutputBlendSamplesRemaining = 0;
        triggerOutputBlendTotalSamples = 0;
    }
    triggerOffsetRatio = juce::jlimit(0.0, 0.999999, (newTargetPosition - loopStartSamples) / juce::jmax(1.0, loopLengthSamples));

    if (playMode == PlayMode::Grain)
    {
        stopAfterFade = false;
        playing = true;
        triggerSample = globalSample;
        const float grainScratch = scratchAmount.load(std::memory_order_acquire);
        const double tempoNow = juce::jmax(1.0, tempo);
        grainGesture.centerRampMs = static_cast<float>(grainScratchSecondsFromAmount(grainScratch) * 1000.0);

        for (int i = 0; i < grainGesture.heldCount; ++i)
        {
            if (grainGesture.heldX[i] == column)
            {
                grainGesture.heldOrder[i] = ++grainGesture.orderCounter;
                break;
            }
        }

        if (grainGesture.heldCount == 3 && grainGesture.sizeControlX == column)
        {
            updateGrainSizeFromGrip();
        }
        else if (grainGesture.heldCount > 0)
        {
            updateGrainAnchorFromHeld();
            grainGesture.freeze = true;
            const int anchorColumn = (grainGesture.anchorX >= 0)
                ? grainGesture.anchorX
                : juce::jlimit(0, ModernAudioEngine::MaxColumns - 1, column);
            const double heldTargetPosition = getTriggerTargetPositionForColumn(anchorColumn, loopStartSamples, loopLengthSamples);
            grainGesture.targetCenterSample = heldTargetPosition;
            if (grainScratch <= 0.001f)
            {
                const double wrapped = getWrappedSamplePosition(heldTargetPosition, loopStartSamples, loopLengthSamples);
                grainCenterSmoother.setCurrentAndTargetValue(wrapped);
                grainGesture.centerTravelDistanceAbs = 0.0;
                grainGesture.targetCenterSample = wrapped;
                grainGesture.frozenCenterSample = wrapped;
                grainGesture.centerSampleSmoothed = wrapped;
                playbackPosition = wrapped;
            }
            else
            {
                setGrainCenterTarget(heldTargetPosition, false);
            }
        }
        else
        {
            grainGesture.freeze = false;
            if (grainScratch <= 0.001f)
            {
                const double wrapped = getWrappedSamplePosition(newTargetPosition, loopStartSamples, loopLengthSamples);
                grainCenterSmoother.setCurrentAndTargetValue(wrapped);
                grainGesture.centerTravelDistanceAbs = 0.0;
                grainGesture.targetCenterSample = wrapped;
                grainGesture.frozenCenterSample = wrapped;
                grainGesture.centerSampleSmoothed = wrapped;
                playbackPosition = wrapped;
            }
            else
            {
                setGrainCenterTarget(newTargetPosition, false);
            }
        }

        if (grainGesture.heldCount <= 0)
        {
            setGrainScratchSceneTarget(0.0f, 1, tempoNow);
        }

        updateGrainHeldLedState();
        crossfader.startFade(true, 64);
        return;
    }
    
    // Use per-strip scratch amount
    float stripScratch = scratchAmount.load();
    
    const bool engageHoldScratch = (stripScratch > 0.0f && buttonHeld && heldButton == column);
    if (engageHoldScratch)
    {
        // Hold-scratch mode:
        // Press trigger starts forward scratch from current playhead to target.
        // Motion decelerates exponentially and then freezes at target while held.
        const double startPosition = playbackPosition.load();
        const int64_t requestedDuration = calculateScratchDuration(stripScratch, tempo);
        const double scratchDistance = computeScratchTravelDistance(startPosition, newTargetPosition);
        const int64_t rampDuration = makeFeasibleScratchDuration(startPosition,
                                                                 startPosition + scratchDistance,
                                                                 requestedDuration,
                                                                 false);

        scratchStartTime = globalSample;
        scratchStartPosition = startPosition;
        scratchTravelDistance = scratchDistance;
        scratchDuration = rampDuration;

        // Preserve PPQ phase relationship so post-scratch timeline sync returns
        // to the exact same musical offset as pre-scratch.
        scratchSavedPpqTimelineAnchored = ppqTimelineAnchored;
        scratchSavedPpqTimelineOffsetBeats = ppqTimelineOffsetBeats;

        // If host PPQ is available, re-derive anchor from the actual audible
        // position at scratch start. This is more robust than relying on stale
        // stored offsets from earlier state transitions.
        if (positionInfo.getPpqPosition().hasValue() && tempo > 0.0)
        {
            float manualBeats = beatsPerLoop.load();
            const double beatsForLoop = (manualBeats >= 0.0f) ? static_cast<double>(manualBeats) : 4.0;
            if (loopLengthSamples > 0.0 && beatsForLoop > 0.0)
            {
                double posInLoop = startPosition - loopStartSamples;
                posInLoop = std::fmod(posInLoop, loopLengthSamples);
                if (posInLoop < 0.0)
                    posInLoop += loopLengthSamples;

                const double beatInLoop = (posInLoop / loopLengthSamples) * beatsForLoop;
                const double hostPpqNow = *positionInfo.getPpqPosition();

                scratchSavedPpqTimelineAnchored = true;
                scratchSavedPpqTimelineOffsetBeats = std::fmod(beatInLoop - hostPpqNow, beatsForLoop);
                if (scratchSavedPpqTimelineOffsetBeats < 0.0)
                    scratchSavedPpqTimelineOffsetBeats += beatsForLoop;
            }
        }

        targetPosition = newTargetPosition;
        targetSampleTime = globalSample + rampDuration;

        scrubActive = true;
        tapeStopActive = false;
        scratchGestureActive = true;
        scratchArrived = false;
        isReverseScratch = false;
        reverseScratchPpqRetarget = false;
        reverseScratchUseRateBlend = false;
        rateSmoother.setCurrentAndTargetValue(1.0);

        // Match RandomSlice behavior: short fade-in to suppress retrigger clicks.
        crossfader.startFade(true, 64);
    }
    else
    {
        // No active hold-scratch - normal jump
        playbackPosition = newTargetPosition;
        scrubActive = false;
        tapeStopActive = false;
        scratchGestureActive = false;
        scratchTravelDistance = 0.0;
        rateSmoother.setCurrentAndTargetValue(1.0);
    }
    
    if (!playing && !engageHoldScratch)
    {
        playbackPosition = newTargetPosition;
        scrubActive = false;
        tapeStopActive = false;
        scratchGestureActive = false;
        scratchTravelDistance = 0.0;
        rateSmoother.setCurrentAndTargetValue(1.0);
    }
    
    stopAfterFade = false;
    playing = true;
    
    // Configurable trigger fade-in for sustained/phase-misaligned retriggers.
    const float triggerFadeMs = triggerFadeInMs.load(std::memory_order_acquire);
    int fadeSamples = juce::jmax(16, static_cast<int>(currentSampleRate * 0.001 * triggerFadeMs));
    if (!wasPlaying)
        crossfader.startFade(true, fadeSamples, true);
}

void EnhancedAudioStrip::onButtonPress(int column, int64_t globalSample)
{
    juce::ScopedLock lock(bufferLock);

    if (playMode == PlayMode::Grain)
    {
        updateGrainGestureOnPress(column, globalSample);
        buttonHeld = (grainGesture.heldCount > 0);
        heldButton = grainGesture.anchorX;
        scratchGestureActive = false;
        scrubActive = false;
        tapeStopActive = false;
        return;
    }

    // Add to set of held buttons
    heldButtons.insert(column);
    heldButtonOrder.erase(std::remove(heldButtonOrder.begin(), heldButtonOrder.end(), column), heldButtonOrder.end());
    heldButtonOrder.push_back(column);
    const float stripScratch = scratchAmount.load();

    auto activatePatternMode = [&](int requiredCount, int patternId, const juce::String& modeName)
    {
        juce::ignoreUnused(modeName);
        patternHoldCountRequired = requiredCount;
        activePattern = patternId;
        patternStartBeat = -1.0;  // Initialize on first process sample.
        lastPatternStep = -1;
        patternActive = true;

        DBG("═══════════════════════════════════════");
        DBG("RHYTHMIC PATTERN ACTIVATED (" << modeName << ")");
        DBG("Pattern: " << activePattern);
        DBG("Buttons held: " << static_cast<int>(heldButtons.size()));
        DBG("═══════════════════════════════════════");
    };

    // 3-button mode: richer combo pattern.
    if (heldButtons.size() >= 3 && stripScratch > 0.0f)
    {
        std::vector<int> buttons(heldButtons.begin(), heldButtons.end());
        std::sort(buttons.begin(), buttons.end());
        const int trioPattern = getPatternFromButtons(buttons[0], buttons[1], buttons[2]);

        if (!patternActive || patternHoldCountRequired != 3 || activePattern != trioPattern)
        {
            activatePatternMode(3, trioPattern, "3-button");
        }
        return;
    }

    // If a pattern is active and we dropped back below 2 held buttons, return to
    // normal one-button scratch behavior immediately.
    if (patternActive && heldButtons.size() < 2)
    {
        patternActive = false;
        activePattern = -1;
        patternHoldCountRequired = 3;
    }
    
    // Normal single/double button behavior
    buttonHeld = true;
    heldButton = column;
    buttonPressTime = globalSample;
    scratchArrived = false;
    // If we were already holding a button in tape-stop, keep output stopped
    // until the newly pressed button's trigger arrives.
    const bool keepHoldMute = (buttonHeld && tapeStopActive);
    tapeStopActive = keepHoldMute;
    scratchGestureActive = keepHoldMute;
    isReverseScratch = false;
    reverseScratchPpqRetarget = false;
    reverseScratchUseRateBlend = false;
    
    DBG("Button " << column << " pressed (scratch: " << scratchAmount.load() << "%)");
}

void EnhancedAudioStrip::onButtonRelease(int column, int64_t globalSample)
{
    juce::ScopedLock lock(bufferLock);

    if (playMode == PlayMode::Grain)
    {
        updateGrainGestureOnRelease(column, globalSample);
        buttonHeld = (grainGesture.heldCount > 0);
        heldButton = grainGesture.anchorX;
        scratchGestureActive = false;
        scrubActive = false;
        tapeStopActive = false;
        return;
    }

    // Remove from held buttons set
    heldButtons.erase(column);
    heldButtonOrder.erase(std::remove(heldButtonOrder.begin(), heldButtonOrder.end(), column), heldButtonOrder.end());
    
    // If pattern was active and we now have fewer held buttons than required,
    // deactivate and return to timeline.
    if (patternActive && heldButtons.size() < static_cast<size_t>(patternHoldCountRequired))
    {
        DBG("RHYTHMIC PATTERN DEACTIVATED (button released)");
        patternActive = false;
        activePattern = -1;
        patternHoldCountRequired = 3;
        
        // Snap to timeline when pattern ends
        snapToTimeline(globalSample);
        
        // Clear all button state
        buttonHeld = false;
        heldButton = -1;
        return;
    }
    
    // If pattern still active (required buttons still held), ignore release.
    if (patternActive)
        return;
    
    // Normal button release behavior
    if (!buttonHeld || heldButton != column)
        return;  // Not our button

    // If no scratch gesture has engaged yet (e.g. released before quantized
    // trigger fired), just clear hold state and exit.
    if (!(scratchGestureActive || tapeStopActive || scrubActive))
    {
        buttonHeld = false;
        heldButton = -1;
        scratchArrived = false;
        return;
    }
    
    DBG("Button " << column << " released");

    // If another button is still held, retarget scratch to that held button
    // instead of returning to timeline.
    if (!heldButtonOrder.empty())
    {
        const int fallbackColumn = heldButtonOrder.back();
        if (heldButtons.count(fallbackColumn) > 0)
        {
            const float stripScratch = scratchAmount.load();
            if (stripScratch > 0.0f)
            {
                const double loopStartSamples = loopStart * (sampleLength / ModernAudioEngine::MaxColumns);
                const double loopLength = loopLengthSamples;
                const double fallbackTarget = getTriggerTargetPositionForColumn(fallbackColumn, loopStartSamples, loopLength);
                const double startPosition = playbackPosition.load();
                const double tempoForScratch = (lastObservedTempo > 0.0) ? lastObservedTempo : 120.0;
                const int64_t requestedDuration = calculateScratchDuration(stripScratch, tempoForScratch);
                const double scratchDistance = computeScratchTravelDistance(startPosition, fallbackTarget);
                const int64_t rampDuration = makeFeasibleScratchDuration(startPosition,
                                                                         startPosition + scratchDistance,
                                                                         requestedDuration,
                                                                         false);

                buttonHeld = true;
                heldButton = fallbackColumn;
                buttonPressTime = globalSample;
                scratchArrived = false;
                scrubActive = true;
                tapeStopActive = false;
                scratchGestureActive = true;
                isReverseScratch = false;
                reverseScratchPpqRetarget = false;
                reverseScratchUseRateBlend = false;
                targetPosition = fallbackTarget;
                targetSampleTime = globalSample + rampDuration;
                scratchStartTime = globalSample;
                scratchStartPosition = startPosition;
                scratchTravelDistance = scratchDistance;
                scratchDuration = rampDuration;
                rateSmoother.setCurrentAndTargetValue(1.0);
                crossfader.startFade(true, 64);

                DBG("Button " << column << " released -> retarget to held button " << fallbackColumn);
                return;
            }
        }
    }

    if (playMode == PlayMode::Loop)
    {
        // Loop mode: restore pre-grain reverse-scratch implementation.
        reverseScratchPpqRetarget = false;
        reverseScratchUseRateBlend = false;
        reverseScratchToTimeline(globalSample);

        buttonHeld = false;
        heldButton = -1;
        scratchArrived = false;
        return;
    }
    
    // Release always performs a reverse scratch back to the timeline.
    float stripScratch = scratchAmount.load();
    const double tempoForScratch = (lastObservedTempo > 0.0) ? lastObservedTempo : 120.0;
    int64_t requestedRampDuration = calculateScratchDuration(stripScratch, tempoForScratch);
    const int64_t minRampSamples = static_cast<int64_t>(0.02 * currentSampleRate); // 20 ms minimum
    int64_t rampDuration = juce::jmax(minRampSamples, requestedRampDuration);

    double loopStartSamples = loopStart * (sampleLength / ModernAudioEngine::MaxColumns);
    double loopLength = loopLengthSamples;
    const float manualBeats = beatsPerLoop.load();
    const double beatsForLoop = (manualBeats >= 0.0f) ? static_cast<double>(manualBeats) : 4.0;
    reverseScratchLoopStartSamples = loopStartSamples;
    reverseScratchLoopLengthSamples = juce::jmax(1.0, loopLength);
    reverseScratchBeatsForLoop = juce::jmax(1.0, beatsForLoop);
    reverseScratchPpqRetarget = false;
    reverseScratchUseRateBlend = false;

    auto predictTimelinePositionAtDuration = [&](int64_t durationSamples, bool& usedPpqOut) -> double
    {
        usedPpqOut = false;
        if (scratchSavedPpqTimelineAnchored && lastObservedPpqValid && lastObservedTempo > 0.0)
        {
            const double samplesPerBeat = (60.0 / lastObservedTempo) * currentSampleRate;
            const double ppqNow = lastObservedPPQ + (static_cast<double>(globalSample - lastObservedGlobalSample) / samplesPerBeat);
            const double ppqAtCompletion = ppqNow + (static_cast<double>(durationSamples) / samplesPerBeat);

            double beatInLoop = std::fmod(ppqAtCompletion + ppqTimelineOffsetBeats, beatsForLoop);
            if (beatInLoop < 0.0)
                beatInLoop += beatsForLoop;
            usedPpqOut = true;
            return loopStartSamples + ((beatInLoop / beatsForLoop) * loopLength);
        }

        // Fallback when PPQ is unavailable.
        const int64_t rampTargetTimeLocal = globalSample + durationSamples;
        const int64_t samplesElapsedSinceTrigger = rampTargetTimeLocal - triggerSample;
        const double triggerOffset = juce::jlimit(0.0, 0.999999, triggerOffsetRatio) * loopLength;
        const double currentSpeedValue = static_cast<double>(playbackSpeed.load());
        double futurePosInLoop = std::fmod(triggerOffset + (samplesElapsedSinceTrigger * currentSpeedValue), loopLength);
        if (futurePosInLoop < 0.0)
            futurePosInLoop += loopLength;
        return loopStartSamples + futurePosInLoop;
    };

    bool usedPpqPrediction = false;
    double futureTimelinePosition = predictTimelinePositionAtDuration(rampDuration, usedPpqPrediction);
    const double reverseStartPosition = playbackPosition.load();

    // If requested scratch time is physically too short for the distance,
    // extend duration so motion remains smooth and reachable.
    double reverseDistance = computeScratchTravelDistance(reverseStartPosition, futureTimelinePosition);
    rampDuration = makeFeasibleScratchDuration(reverseStartPosition,
                                               reverseStartPosition + reverseDistance,
                                               rampDuration,
                                               true);

    futureTimelinePosition = predictTimelinePositionAtDuration(rampDuration, usedPpqPrediction);
    reverseDistance = computeScratchTravelDistance(reverseStartPosition, futureTimelinePosition);
    int64_t rampTargetTime = globalSample + rampDuration;

    targetPosition = futureTimelinePosition;
    targetSampleTime = rampTargetTime;
    scrubActive = true;
    scratchGestureActive = true;
    isReverseScratch = true;
    scratchStartTime = globalSample;
    scratchStartPosition = reverseStartPosition;
    scratchTravelDistance = reverseDistance;
    scratchDuration = rampDuration;
    tapeStopActive = false;

    // Re-assert saved PPQ alignment at release stage.
    ppqTimelineAnchored = scratchSavedPpqTimelineAnchored;
    ppqTimelineOffsetBeats = scratchSavedPpqTimelineOffsetBeats;
    // Continuous PPQ retargeting can sound like varispeed wobble in loop mode.
    // Keep loop-mode reverse return locked to the single release-time target.
    reverseScratchPpqRetarget = usedPpqPrediction
        && ppqTimelineAnchored
        && (playMode == PlayMode::Grain);

    DBG("Reverse scratch release: target=" << targetPosition
        << " duration=" << (static_cast<double>(rampDuration) / currentSampleRate)
        << "s ppqPred=" << (usedPpqPrediction ? "YES" : "NO"));
    
    // Clear hold state
    buttonHeld = false;
    heldButton = -1;
    scratchArrived = false;
}

void EnhancedAudioStrip::snapToTimeline(int64_t currentGlobalSample)
{
    // Calculate where the strip SHOULD be based on original trigger point
    int64_t samplesElapsedSinceTrigger = currentGlobalSample - triggerSample;
    
    // Calculate loop parameters
    double loopStartSamples = loopStart * (sampleLength / ModernAudioEngine::MaxColumns);
    double loopLength = loopLengthSamples;
    double triggerOffset = juce::jlimit(0.0, 0.999999, triggerOffsetRatio) * loopLength;
    
    // Calculate expected position based on time elapsed
    double currentSpeedValue = smoothedSpeed.getNextValue();
    double expectedAdvance = samplesElapsedSinceTrigger * currentSpeedValue;
    double expectedPosInLoop = std::fmod(triggerOffset + expectedAdvance, loopLength);
    
    // Handle wrapping
    if (expectedPosInLoop < 0) expectedPosInLoop += loopLength;
    
    double expectedPosition = loopStartSamples + expectedPosInLoop;
    
    // SNAP back to expected timeline position
    playbackPosition = expectedPosition;
    
    // Exit scratch mode - return to normal playback
    scrubActive = false;
    tapeStopActive = false;
    scratchGestureActive = false;
    isReverseScratch = false;
    reverseScratchPpqRetarget = false;
    reverseScratchUseRateBlend = false;
    scratchTravelDistance = 0.0;
    const float restoreSpeed = static_cast<float>(playbackSpeed.load(std::memory_order_acquire));
    smoothedSpeed.setCurrentAndTargetValue(restoreSpeed);
    rateSmoother.setCurrentAndTargetValue(1.0);
    
    DBG("Snapped to timeline position (expected: " << expectedPosInLoop << " samples into loop)");
}

void EnhancedAudioStrip::reverseScratchToTimeline(int64_t currentGlobalSample)
{
    // Loop-mode release: return to the timeline position where PPQ will be
    // after the release duration, preserving the saved trigger phase offset.
    int64_t reverseDuration = scratchDuration;
    if (reverseDuration == 0 || reverseDuration > currentSampleRate * 2.0)
    {
        reverseDuration = calculateScratchDuration(scratchAmount.load(), 120.0);
    }

    reverseDuration = juce::jmax<int64_t>(1, reverseDuration);

    const double loopStartSamples = loopStart * (sampleLength / ModernAudioEngine::MaxColumns);
    const double loopLength = loopLengthSamples;
    const double triggerOffset = juce::jlimit(0.0, 0.999999, triggerOffsetRatio) * loopLength;
    const float manualBeats = beatsPerLoop.load();
    const double beatsForLoop = (manualBeats >= 0.0f) ? static_cast<double>(manualBeats) : 4.0;

    auto predictFutureTimeline = [&](int64_t durationSamples, bool& usedPpqOut) -> double
    {
        usedPpqOut = false;
        if (scratchSavedPpqTimelineAnchored && lastObservedPpqValid && lastObservedTempo > 0.0)
        {
            const double samplesPerBeat = (60.0 / lastObservedTempo) * currentSampleRate;
            const double ppqNow = lastObservedPPQ
                                + (static_cast<double>(currentGlobalSample - lastObservedGlobalSample) / samplesPerBeat);
            const double ppqAtCompletion = ppqNow + (static_cast<double>(durationSamples) / samplesPerBeat);

            double beatInLoop = std::fmod(ppqAtCompletion + scratchSavedPpqTimelineOffsetBeats, beatsForLoop);
            if (beatInLoop < 0.0)
                beatInLoop += beatsForLoop;
            usedPpqOut = true;
            return loopStartSamples + ((beatInLoop / beatsForLoop) * loopLength);
        }

        const int64_t reverseTargetTimeLocal = currentGlobalSample + durationSamples;
        const int64_t samplesElapsedAtCompletion = reverseTargetTimeLocal - triggerSample;
        const double currentSpeedValue = static_cast<double>(playbackSpeed.load(std::memory_order_acquire));
        double futureAdvance = samplesElapsedAtCompletion * currentSpeedValue;
        double futurePosInLoop = std::fmod(triggerOffset + futureAdvance, loopLength);
        if (futurePosInLoop < 0.0)
            futurePosInLoop += loopLength;
        return loopStartSamples + futurePosInLoop;
    };

    bool usedPpqPrediction = false;
    double futureTimelinePosition = predictFutureTimeline(reverseDuration, usedPpqPrediction);
    const double currentPos = playbackPosition.load();
    // Always use shortest wrapped path to the release target.
    double distance = computeScratchTravelDistance(currentPos, futureTimelinePosition);

    const int64_t reverseTargetTime = currentGlobalSample + reverseDuration;
    const float displaySpeedNow = displaySpeedAtomic.load(std::memory_order_acquire);
    const double startRateMag = (tapeStopActive || !std::isfinite(displaySpeedNow))
        ? 0.0
        : std::abs(static_cast<double>(displaySpeedNow));
    const double restoreRateMag = std::abs(static_cast<double>(playbackSpeed.load(std::memory_order_acquire)));
    const double direction = (distance >= 0.0) ? 1.0 : -1.0;

    targetPosition = futureTimelinePosition;
    targetSampleTime = reverseTargetTime;
    scrubActive = true;
    isReverseScratch = true;
    reverseScratchPpqRetarget = false;
    reverseScratchUseRateBlend = true;
    reverseScratchStartRate = direction * startRateMag;
    reverseScratchEndRate = direction * restoreRateMag;
    scratchStartTime = currentGlobalSample;
    scratchStartPosition = currentPos;
    scratchTravelDistance = distance;
    scratchDuration = reverseDuration;
    tapeStopActive = false;

    ppqTimelineAnchored = scratchSavedPpqTimelineAnchored;
    ppqTimelineOffsetBeats = scratchSavedPpqTimelineOffsetBeats;

    DBG("Loop reverse scratch: target=" << targetPosition
        << " dur=" << (static_cast<double>(reverseDuration) / currentSampleRate)
        << "s ppq=" << (usedPpqPrediction ? "YES" : "NO")
        << " dist=" << distance
        << " v0=" << reverseScratchStartRate
        << " v1=" << reverseScratchEndRate);
}

double EnhancedAudioStrip::computeScratchTravelDistance(double startPosSamples, double endPosSamples) const
{
    // One-shot is non-wrapping; use direct distance.
    if (playMode == PlayMode::OneShot)
        return endPosSamples - startPosSamples;

    int loopCols = loopEnd - loopStart;
    if (loopCols <= 0)
        loopCols = ModernAudioEngine::MaxColumns;

    const double loopStartSamples = loopStart * (sampleLength / ModernAudioEngine::MaxColumns);
    const double loopLength = (loopCols / static_cast<double>(ModernAudioEngine::MaxColumns)) * sampleLength;
    if (loopLength <= 0.0)
        return endPosSamples - startPosSamples;

    auto wrapToLoop = [loopLength](double value) -> double
    {
        double wrapped = std::fmod(value, loopLength);
        if (wrapped < 0.0)
            wrapped += loopLength;
        return wrapped;
    };

    const double startInLoop = wrapToLoop(startPosSamples - loopStartSamples);
    const double endInLoop = wrapToLoop(endPosSamples - loopStartSamples);

    double delta = endInLoop - startInLoop;
    if (delta > loopLength * 0.5)
        delta -= loopLength;
    else if (delta < -loopLength * 0.5)
        delta += loopLength;

    return delta;
}

void EnhancedAudioStrip::captureMomentaryPhaseReference(double hostPpq)
{
    juce::ScopedLock lock(bufferLock);

    if (sampleLength <= 0.0)
    {
        momentaryPhaseGuardValid = false;
        return;
    }

    int loopCols = loopEnd - loopStart;
    if (loopCols <= 0)
        loopCols = ModernAudioEngine::MaxColumns;

    const double loopStartSamples = loopStart * (sampleLength / ModernAudioEngine::MaxColumns);
    const double loopLength = (loopCols / static_cast<double>(ModernAudioEngine::MaxColumns)) * sampleLength;
    if (loopLength <= 0.0)
    {
        momentaryPhaseGuardValid = false;
        return;
    }

    float manualBeats = beatsPerLoop.load();
    const double beatsForLoop = (manualBeats >= 0.0f) ? static_cast<double>(manualBeats) : 4.0;
    if (beatsForLoop <= 0.0)
    {
        momentaryPhaseGuardValid = false;
        return;
    }

    double posInLoop = playbackPosition.load() - loopStartSamples;
    posInLoop = std::fmod(posInLoop, loopLength);
    if (posInLoop < 0.0)
        posInLoop += loopLength;

    const double beatInLoop = (posInLoop / loopLength) * beatsForLoop;
    double offset = std::fmod(beatInLoop - hostPpq, beatsForLoop);
    if (offset < 0.0)
        offset += beatsForLoop;

    momentaryPhaseOffsetBeats = offset;
    momentaryPhaseBeatsForLoop = beatsForLoop;
    momentaryPhaseGuardValid = true;
}

void EnhancedAudioStrip::enforceMomentaryPhaseReference(double hostPpq, int64_t currentGlobalSample)
{
    juce::ScopedLock lock(bufferLock);

    if (!momentaryPhaseGuardValid || sampleLength <= 0.0)
        return;

    int loopCols = loopEnd - loopStart;
    if (loopCols <= 0)
        loopCols = ModernAudioEngine::MaxColumns;

    const double loopStartSamples = loopStart * (sampleLength / ModernAudioEngine::MaxColumns);
    const double loopLength = (loopCols / static_cast<double>(ModernAudioEngine::MaxColumns)) * sampleLength;
    const double beatsForLoop = juce::jmax(0.25, momentaryPhaseBeatsForLoop);
    if (loopLength <= 0.0)
    {
        momentaryPhaseGuardValid = false;
        return;
    }

    double beatInLoop = std::fmod(hostPpq + momentaryPhaseOffsetBeats, beatsForLoop);
    if (beatInLoop < 0.0)
        beatInLoop += beatsForLoop;
    const double expectedPos = loopStartSamples + ((beatInLoop / beatsForLoop) * loopLength);

    double currentPosInLoop = playbackPosition.load() - loopStartSamples;
    currentPosInLoop = std::fmod(currentPosInLoop, loopLength);
    if (currentPosInLoop < 0.0)
        currentPosInLoop += loopLength;

    double expectedPosInLoop = expectedPos - loopStartSamples;
    expectedPosInLoop = std::fmod(expectedPosInLoop, loopLength);
    if (expectedPosInLoop < 0.0)
        expectedPosInLoop += loopLength;

    double delta = std::abs(expectedPosInLoop - currentPosInLoop);
    delta = juce::jmin(delta, loopLength - delta);
    const double tolerance = juce::jmax(6.0, loopLength * 0.002);

    // Always restore PPQ anchor, and hard-correct if drift exceeds tolerance.
    ppqTimelineAnchored = true;
    ppqTimelineOffsetBeats = momentaryPhaseOffsetBeats;
    if (delta > tolerance)
        playbackPosition = expectedPos;

    // End any lingering scratch state.
    scrubActive = false;
    tapeStopActive = false;
    scratchGestureActive = false;
    isReverseScratch = false;
    reverseScratchPpqRetarget = false;
    reverseScratchUseRateBlend = false;
    scratchTravelDistance = 0.0;
    resetScratchComboState();
    rateSmoother.setCurrentAndTargetValue(1.0);

    // Keep trigger sample coherent for sample-based fallback paths.
    triggerSample = currentGlobalSample;

    momentaryPhaseGuardValid = false;
}

void EnhancedAudioStrip::realignToPpqAnchor(double hostPpq, int64_t currentGlobalSample)
{
    juce::ScopedLock lock(bufferLock);

    if (!ppqTimelineAnchored || !std::isfinite(hostPpq) || sampleLength <= 0.0)
        return;

    int loopCols = loopEnd - loopStart;
    if (loopCols <= 0)
        loopCols = ModernAudioEngine::MaxColumns;

    const double loopStartSamples = loopStart * (sampleLength / ModernAudioEngine::MaxColumns);
    const double loopLength = (loopCols / static_cast<double>(ModernAudioEngine::MaxColumns)) * sampleLength;
    if (loopLength <= 0.0)
        return;

    const float manualBeats = beatsPerLoop.load(std::memory_order_acquire);
    const double beatsForLoop = (manualBeats >= 0.0f) ? static_cast<double>(manualBeats) : 4.0;
    if (beatsForLoop <= 0.0)
        return;

    double beatInLoop = std::fmod(hostPpq + ppqTimelineOffsetBeats, beatsForLoop);
    if (beatInLoop < 0.0)
        beatInLoop += beatsForLoop;

    const double targetPos = loopStartSamples + ((beatInLoop / beatsForLoop) * loopLength);
    playbackPosition = juce::jlimit(0.0, juce::jmax(0.0, sampleLength - 1.0), targetPos);

    // Keep trigger/fallback references coherent with the same PPQ-locked position.
    triggerSample = currentGlobalSample;
    triggerPpqPosition = hostPpq;
    lastTriggerPPQ = hostPpq;
    triggerOffsetRatio = juce::jlimit(0.0, 0.999999, (playbackPosition.load() - loopStartSamples) / loopLength);
}

int64_t EnhancedAudioStrip::makeFeasibleScratchDuration(double startPosSamples,
                                                        double endPosSamples,
                                                        int64_t requestedDurationSamples,
                                                        bool reverseScratch) const
{
    const double distanceSamples = std::abs(endPosSamples - startPosSamples);
    if (distanceSamples <= 0.0)
        return juce::jmax<int64_t>(1, requestedDurationSamples);

    // Cap to the runtime clamp used in process().
    const double maxRate = kMaxScratchRateAbs;
    const double minDurationByClamp = reverseScratch
        ? std::ceil((distanceSamples * kReverseScratchAccelExp) / maxRate)
        : std::ceil((distanceSamples * kForwardScratchDecay) / ((1.0 - std::exp(-kForwardScratchDecay)) * maxRate));

    const int64_t feasible = static_cast<int64_t>(juce::jmax(1.0, minDurationByClamp));
    return juce::jmax(feasible, juce::jmax<int64_t>(1, requestedDurationSamples));
}

int64_t EnhancedAudioStrip::calculateScratchDuration(float scratchAmountPercent, double tempo)
{
    const float clamped = juce::jlimit(0.0f, 100.0f, scratchAmountPercent);

    // Make low values (1-10) much snappier for short cut/stab scratches.
    double beats = 0.25;
    if (clamped <= 10.0f)
    {
        const double t = static_cast<double>(clamped) / 10.0;
        beats = 0.02 + (std::pow(t, 1.6) * 0.08);   // 0.02..0.10 beats
    }
    else
    {
        const double t = static_cast<double>(clamped - 10.0f) / 90.0;
        beats = 0.10 + (std::pow(t, 1.8) * 7.90);   // 0.10..8.00 beats
    }
    
    // Convert to samples
    double secondsPerBeat = 60.0 / tempo;
    double seconds = beats * secondsPerBeat;
    int64_t samples = static_cast<int64_t>(std::llround(seconds * currentSampleRate));
    
    return juce::jmax<int64_t>(1, samples);
}

int EnhancedAudioStrip::getPatternFromButtons(int btn1, int btn2, int btn3)
{
    int buttons[3] = { btn1, btn2, btn3 };
    std::sort(buttons, buttons + 3);

    // Rank the 3-button combination (16 choose 3 = 560 combos).
    // This gives each unique combo a stable pattern id.
    int rank = 0;
    for (int a = 0; a < 16; ++a)
    {
        for (int b = a + 1; b < 16; ++b)
        {
            for (int c = b + 1; c < 16; ++c, ++rank)
            {
                if (a == buttons[0] && b == buttons[1] && c == buttons[2])
                    return rank;
            }
        }
    }

    return 0;
}

int EnhancedAudioStrip::getPatternFromTwoButtons(int btn1, int btn2)
{
    int a = juce::jlimit(0, 15, btn1);
    int b = juce::jlimit(0, 15, btn2);
    if (a > b)
        std::swap(a, b);

    int rank = 0;
    for (int i = 0; i < 16; ++i)
    {
        for (int j = i + 1; j < 16; ++j, ++rank)
        {
            if (i == a && j == b)
                return rank;
        }
    }

    return (a * 16) + b;
}

double EnhancedAudioStrip::executeRhythmicPattern(int pattern, double beat, double beatsElapsed, int btn1, int btn2, int btn3)
{
    int buttons[3] = { btn1, btn2, btn3 };
    std::sort(buttons, buttons + 3);

    const double avgButton = (buttons[0] + buttons[1] + buttons[2]) / 3.0;
    const int spread = buttons[2] - buttons[0];
    const double buttonBias = juce::jlimit(-1.0, 1.0, (avgButton - 7.5) / 7.5);

    auto mixHash = [](uint64_t value) -> uint32_t
    {
        value ^= (value >> 33);
        value *= 0xff51afd7ed558ccdULL;
        value ^= (value >> 33);
        value *= 0xc4ceb9fe1a85ec53ULL;
        value ^= (value >> 33);
        return static_cast<uint32_t>(value & 0xffffffffULL);
    };

    const uint64_t comboKey =
        (static_cast<uint64_t>(juce::jmax(0, pattern) + 1) << 20)
        ^ (static_cast<uint64_t>(buttons[0]) << 12)
        ^ (static_cast<uint64_t>(buttons[1]) << 6)
        ^ static_cast<uint64_t>(buttons[2]);

    // Rnd Slice-like rhythmic grid choices.
    static constexpr std::array<double, 8> segmentChoices {
        1.0 / 32.0, 1.0 / 24.0, 1.0 / 16.0, 1.0 / 12.0,
        1.0 / 8.0,  3.0 / 16.0, 1.0 / 6.0,  1.0 / 4.0
    };
    const uint32_t qHash = mixHash(comboKey ^ 0x51f15e9dULL);
    const size_t qIndex = static_cast<size_t>((qHash + static_cast<uint32_t>(spread)) % segmentChoices.size());
    const double segmentBeats = segmentChoices[qIndex];

    const double elapsed = juce::jmax(0.0, beatsElapsed);
    const int64_t segmentIndex = static_cast<int64_t>(std::floor(elapsed / segmentBeats));
    const double segmentStartBeat = static_cast<double>(segmentIndex) * segmentBeats;
    const double segmentPhase = juce::jlimit(0.0, 1.0, (elapsed - segmentStartBeat) / segmentBeats);

    const uint64_t segmentKey = comboKey ^ (static_cast<uint64_t>(segmentIndex + 1) * 0x9e3779b97f4a7c15ULL);

    static constexpr std::array<double, 9> speedChoices { -4.0, -2.0, -1.0, -0.5, 0.0, 0.5, 1.0, 2.0, 4.0 };
    const double speedStartBase = speedChoices[static_cast<size_t>(mixHash(segmentKey ^ 0xa53c49e6ULL) % speedChoices.size())];
    const double speedEndBase = speedChoices[static_cast<size_t>(mixHash(segmentKey ^ 0xc8013ea4ULL) % speedChoices.size())];

    // Bias higher button combos to brighter/faster movement.
    const double speedStart = juce::jlimit(-4.0, 4.0, speedStartBase + (buttonBias * 0.85));
    const double speedEnd = juce::jlimit(-4.0, 4.0, speedEndBase + (buttonBias * 0.85));

    double u = segmentPhase;
    const int shape = static_cast<int>(mixHash(segmentKey ^ 0x165667b1ULL) % 4U);
    switch (shape)
    {
        case 0: // linear
            break;
        case 1: // accelerate
            u = u * u;
            break;
        case 2: // decelerate
            u = 1.0 - ((1.0 - u) * (1.0 - u));
            break;
        case 3: // smoothstep
            u = u * u * (3.0 - (2.0 * u));
            break;
        default:
            break;
    }

    // Optional short "vinyl choke" near segment tail for more random-slice feel.
    if ((mixHash(segmentKey ^ 0x7f4a7c15ULL) & 0x7U) == 0U && u > 0.88)
    {
        const double choke = (u - 0.88) / 0.12;
        u *= (1.0 - juce::jlimit(0.0, 1.0, choke * choke));
    }

    double rate = speedStart + ((speedEnd - speedStart) * u);

    // Light beat-synced wobble to keep ramps lively like Rnd Slice.
    const int wobbleMult = 1 + static_cast<int>(mixHash(segmentKey ^ 0x94d049bbULL) % 4U);
    const double wobbleDepth = 0.08 + (static_cast<double>(spread) / 64.0);
    const double wobble = std::sin(beat * static_cast<double>(wobbleMult) * juce::MathConstants<double>::twoPi);
    rate += wobble * wobbleDepth;

    // Occasional short stutter-gate for sliced feel.
    if ((mixHash(segmentKey ^ 0x2f4a6d3bULL) % 7U) == 0U)
    {
        const double gate = std::fmod(segmentPhase * 8.0, 1.0);
        if (gate > 0.72)
            rate *= 0.2;
    }

    return juce::jlimit(-4.0, 4.0, rate);
}


void EnhancedAudioStrip::calculatePositionFromGlobalSample(int64_t globalSample, double tempo)
{
    (void) globalSample;
    (void) tempo;
    // Don't do anything here - we'll calculate per-sample in process()
}

void EnhancedAudioStrip::syncToGlobalPhase(double globalPhase, double tempo)
{
    (void) globalPhase;
    (void) tempo;
    // Not used
}


void EnhancedAudioStrip::stop(bool immediate)
{
    juce::ScopedLock lock(bufferLock);

    retriggerBlendActive = false;
    retriggerBlendSamplesRemaining = 0;
    retriggerBlendTotalSamples = 0;
    triggerOutputBlendActive = false;
    triggerOutputBlendSamplesRemaining = 0;
    triggerOutputBlendTotalSamples = 0;
    scrubActive = false;
    tapeStopActive = false;
    scratchGestureActive = false;
    isReverseScratch = false;
    reverseScratchPpqRetarget = false;
    reverseScratchUseRateBlend = false;
    scratchTravelDistance = 0.0;
    resetScratchComboState();

    if (immediate)
    {
        stopAfterFade = false;
        playing = false;
        playbackPosition = 0.0;
        lastOutputSampleL = 0.0f;
        lastOutputSampleR = 0.0f;
        resetGrainState();
    }
    else
    {
        // Keep choke/stop release independent from trigger fade-in control.
        int fadeSamples = juce::jmax(128, static_cast<int>(currentSampleRate * 0.006)); // ~6ms
        stopAfterFade = true;
        crossfader.startFade(false, fadeSamples);
    }
}

void EnhancedAudioStrip::startStepSequencer()
{
    // Step sequencer runs with global clock, not manual triggers
    stopAfterFade = false;
    playing = true;
    playbackPosition = 0.0;
    currentStep = 0;
    lastStepTime = -1.0;  // Force first step check
    stepSamplePlaying = false;  // No sample playing initially
    stepRandomWalkPos = 0;
    stepRandomSliceBeatGroup = -1;
    randomSliceNextTriggerBeat = -1.0;
    
    DBG("Step sequencer started for strip " << stripIndex);
}

void EnhancedAudioStrip::setLoop(int startColumn, int endColumn)
{
    loopStart = juce::jlimit(0, ModernAudioEngine::MaxColumns - 1, startColumn);
    loopEnd = juce::jlimit(loopStart + 1, ModernAudioEngine::MaxColumns, endColumn);
    loopEnabled = true;
}

void EnhancedAudioStrip::setBeatsPerLoop(float beats)
{
    const double hostPpqNow = lastObservedPpqValid ? lastObservedPPQ : std::numeric_limits<double>::quiet_NaN();
    setBeatsPerLoopAtPpq(beats, hostPpqNow);
}

void EnhancedAudioStrip::setBeatsPerLoopAtPpq(float beats, double hostPpqNow)
{
    juce::ScopedLock lock(bufferLock);

    const float previousManual = beatsPerLoop.load(std::memory_order_acquire);
    const double previousBeats = (previousManual >= 0.0f) ? static_cast<double>(previousManual) : 4.0;

    // -1 = auto-detect, otherwise manual override (0.25 to 64 beats)
    const float nextManual = (beats < 0.0f)
        ? -1.0f
        : juce::jlimit(0.25f, 64.0f, beats);
    beatsPerLoop.store(nextManual, std::memory_order_release);

    const double nextBeats = (nextManual >= 0.0f) ? static_cast<double>(nextManual) : 4.0;
    if (!ppqTimelineAnchored || sampleLength <= 0.0 || nextBeats <= 0.0 || previousBeats <= 0.0 || !std::isfinite(hostPpqNow))
        return;

    // Preserve phase from the PPQ anchor itself (same principle as preset restore),
    // not from instantaneous playbackPosition which can be block-late.
    double oldBeatInLoop = std::fmod(hostPpqNow + ppqTimelineOffsetBeats, previousBeats);
    if (oldBeatInLoop < 0.0)
        oldBeatInLoop += previousBeats;
    const double normalizedPhase = oldBeatInLoop / previousBeats;
    const double beatInLoopNew = normalizedPhase * nextBeats;

    double newOffset = std::fmod(beatInLoopNew - hostPpqNow, nextBeats);
    if (newOffset < 0.0)
        newOffset += nextBeats;
    ppqTimelineOffsetBeats = newOffset;

    // Keep fallback (non-PPQ) timing coherent with the remapped phase so
    // temporary host-PPQ dropouts cannot introduce phase drift after bar changes.
    triggerPpqPosition = hostPpqNow;
    if (lastObservedPpqValid)
        triggerSample = lastObservedGlobalSample;
    triggerOffsetRatio = juce::jlimit(0.0, 0.999999, beatInLoopNew / nextBeats);
}

void EnhancedAudioStrip::clearLoop()
{
    loopEnabled = false;
    loopStart = 0;
    loopEnd = ModernAudioEngine::MaxColumns;
}

void EnhancedAudioStrip::setPlaybackMarkerColumn(int column, int64_t currentGlobalSample)
{
    juce::ScopedLock lock(bufferLock);
    if (sampleLength <= 0.0)
        return;

    const int clampedColumn = juce::jlimit(0, ModernAudioEngine::MaxColumns - 1, column);
    int loopCols = loopEnd - loopStart;
    if (loopCols <= 0)
        loopCols = ModernAudioEngine::MaxColumns;

    const double loopStartSamples = loopStart * (sampleLength / ModernAudioEngine::MaxColumns);
    const double loopLength = (loopCols / static_cast<double>(ModernAudioEngine::MaxColumns)) * sampleLength;
    if (loopLength <= 0.0)
        return;

    const double targetPos = getTriggerTargetPositionForColumn(clampedColumn, loopStartSamples, loopLength);
    playbackPosition = juce::jlimit(0.0, juce::jmax(0.0, sampleLength - 1.0), targetPos);
    stopLoopPosition = playbackPosition.load();
    triggerColumn = clampedColumn;
    triggerSample = currentGlobalSample;
    triggerOffsetRatio = (playbackPosition.load() - loopStartSamples) / loopLength;
    triggerOffsetRatio = juce::jlimit(0.0, 0.999999, triggerOffsetRatio);
}

void EnhancedAudioStrip::restorePresetPpqState(bool shouldPlay,
                                               bool timelineAnchored,
                                               double timelineOffsetBeats,
                                               int fallbackColumn,
                                               double tempo,
                                               double currentTimelineBeat,
                                               int64_t currentGlobalSample)
{
    if (sampleLength <= 0.0)
        return;

    if (!shouldPlay)
    {
        setPlaybackMarkerColumn(fallbackColumn, currentGlobalSample);
        stop(true);
        return;
    }

    if (!timelineAnchored || !std::isfinite(timelineOffsetBeats) || tempo <= 0.0 || !std::isfinite(currentTimelineBeat))
    {
        juce::AudioPlayHead::PositionInfo posInfo;
        posInfo.setPpqPosition(currentTimelineBeat);
        triggerAtSample(fallbackColumn, tempo, currentGlobalSample, posInfo);
        return;
    }

    juce::ScopedLock lock(bufferLock);

    const int clampedColumn = juce::jlimit(0, ModernAudioEngine::MaxColumns - 1, fallbackColumn);
    int loopCols = loopEnd - loopStart;
    if (loopCols <= 0)
        loopCols = ModernAudioEngine::MaxColumns;

    const double loopStartSamples = loopStart * (sampleLength / ModernAudioEngine::MaxColumns);
    const double loopLength = (loopCols / static_cast<double>(ModernAudioEngine::MaxColumns)) * sampleLength;
    if (loopLength <= 0.0)
        return;

    const float manualBeats = beatsPerLoop.load(std::memory_order_acquire);
    const double beatsForLoop = (manualBeats >= 0.0f) ? static_cast<double>(manualBeats) : 4.0;
    if (beatsForLoop <= 0.0)
        return;

    // Restore timeline-relative anchor (offset from host PPQ), not absolute playback sample.
    ppqTimelineAnchored = true;
    ppqTimelineOffsetBeats = std::fmod(timelineOffsetBeats, beatsForLoop);
    if (ppqTimelineOffsetBeats < 0.0)
        ppqTimelineOffsetBeats += beatsForLoop;

    triggerColumn = clampedColumn;
    triggerSample = currentGlobalSample;
    triggerPpqPosition = currentTimelineBeat;
    lastTriggerPPQ = triggerPpqPosition;
    playheadSample = 0;
    loopLengthSamples = loopLength;

    const double timelineBeats = currentTimelineBeat + ppqTimelineOffsetBeats;
    const double timelinePosition = (timelineBeats / beatsForLoop) * sampleLength;
    const double mappedPos = getWrappedSamplePosition(loopStartSamples + timelinePosition, loopStartSamples, loopLength);
    playbackPosition = juce::jlimit(0.0, juce::jmax(0.0, sampleLength - 1.0), mappedPos);
    stopLoopPosition = playbackPosition.load();

    double posInLoop = playbackPosition.load() - loopStartSamples;
    posInLoop = std::fmod(posInLoop, loopLength);
    if (posInLoop < 0.0)
        posInLoop += loopLength;
    triggerOffsetRatio = juce::jlimit(0.0, 0.999999, posInLoop / juce::jmax(1.0, loopLength));

    stopAfterFade = false;
    playing = true;
    wasPlayingBeforeStop = false;
}

void EnhancedAudioStrip::setVolume(float vol)
{
    vol = juce::jlimit(0.0f, 1.0f, vol);
    volume = vol;
    smoothedVolume.setTargetValue(vol);
}

void EnhancedAudioStrip::setPan(float panValue)
{
    panValue = juce::jlimit(-1.0f, 1.0f, panValue);
    pan = panValue;
    smoothedPan.setTargetValue(panValue);
}

void EnhancedAudioStrip::setPlaybackSpeed(float speed)
{
    speed = juce::jlimit(0.0f, 4.0f, speed);
    playbackSpeed = static_cast<double>(speed);
    displaySpeedAtomic.store(speed, std::memory_order_release);
    smoothedSpeed.setTargetValue(speed);
}

void EnhancedAudioStrip::setPlaybackSpeedImmediate(float speed)
{
    speed = juce::jlimit(0.0f, 4.0f, speed);
    playbackSpeed = static_cast<double>(speed);
    displaySpeedAtomic.store(speed, std::memory_order_release);
    smoothedSpeed.setCurrentAndTargetValue(speed);
}

void EnhancedAudioStrip::setPitchShift(float semitones)
{
    const float clamped = juce::jlimit(-24.0f, 24.0f, semitones);
    pitchShiftSemitones.store(clamped, std::memory_order_release);
    smoothedPitchShift.setTargetValue(clamped);
}

void EnhancedAudioStrip::setPitchSmoothingTime(float seconds)
{
    // Update smoothing ramp time (convert seconds to samples at current sample rate)
    smoothedSpeed.reset(currentSampleRate, seconds);
}

void EnhancedAudioStrip::setReverse(bool shouldReverse)
{
    reverse = shouldReverse;
}

void EnhancedAudioStrip::resetPitchShifter()
{
    const int delaySamples = juce::jmax(2048, static_cast<int>(currentSampleRate * 0.1));
    pitchShiftDelaySize = delaySamples;
    pitchShiftDelayBuffer.setSize(2, pitchShiftDelaySize, false, true, true);
    pitchShiftDelayBuffer.clear();
    pitchShiftWritePos = 0;
    pitchShiftPhase = 0.0;
}

float EnhancedAudioStrip::readPitchDelaySample(int channel, double delaySamples) const
{
    if (pitchShiftDelaySize <= 4 || channel < 0 || channel >= pitchShiftDelayBuffer.getNumChannels())
        return 0.0f;

    const float* data = pitchShiftDelayBuffer.getReadPointer(channel);
    double readPos = static_cast<double>(pitchShiftWritePos) - delaySamples;

    while (readPos < 0.0)
        readPos += static_cast<double>(pitchShiftDelaySize);
    while (readPos >= static_cast<double>(pitchShiftDelaySize))
        readPos -= static_cast<double>(pitchShiftDelaySize);

    const int i1 = static_cast<int>(readPos);
    const int i0 = (i1 - 1 + pitchShiftDelaySize) % pitchShiftDelaySize;
    const int i2 = (i1 + 1) % pitchShiftDelaySize;
    const int i3 = (i1 + 2) % pitchShiftDelaySize;
    const float t = static_cast<float>(readPos - static_cast<double>(i1));

    const float y0 = data[i0];
    const float y1 = data[i1];
    const float y2 = data[i2];
    const float y3 = data[i3];

    const float a0 = y3 - y2 - y0 + y1;
    const float a1 = y0 - y1 - a0;
    const float a2 = y2 - y0;
    const float a3 = y1;

    return ((a0 * t + a1) * t + a2) * t + a3;
}

void EnhancedAudioStrip::processPitchShift(float& left, float& right)
{
    if (pitchShiftDelaySize <= 4)
        return;

    const float semitones = smoothedPitchShift.getNextValue();
    if (std::abs(semitones) < 0.01f)
    {
        pitchShiftDelayBuffer.setSample(0, pitchShiftWritePos, left);
        pitchShiftDelayBuffer.setSample(1, pitchShiftWritePos, right);
        pitchShiftWritePos = (pitchShiftWritePos + 1) % pitchShiftDelaySize;
        return;
    }

    const double ratio = std::pow(2.0, static_cast<double>(semitones) / 12.0);
    const double detune = 1.0 - ratio;
    const double windowSamples = juce::jlimit(128.0, static_cast<double>(pitchShiftDelaySize - 4), currentSampleRate * 0.05);

    pitchShiftDelayBuffer.setSample(0, pitchShiftWritePos, left);
    pitchShiftDelayBuffer.setSample(1, pitchShiftWritePos, right);

    if (std::abs(detune) < 1.0e-6)
    {
        pitchShiftWritePos = (pitchShiftWritePos + 1) % pitchShiftDelaySize;
        return;
    }

    const double phaseInc = std::abs(detune) / windowSamples;
    pitchShiftPhase += phaseInc;
    while (pitchShiftPhase >= 1.0)
        pitchShiftPhase -= 1.0;

    auto delayFromPhase = [&](double p)
    {
        if (ratio >= 1.0)
            return (1.0 - p) * windowSamples + 1.0;
        return p * windowSamples + 1.0;
    };

    const double p1 = pitchShiftPhase;
    const double p2 = std::fmod(pitchShiftPhase + 0.5, 1.0);
    const double d1 = delayFromPhase(p1);
    const double d2 = delayFromPhase(p2);

    const float l1 = readPitchDelaySample(0, d1);
    const float r1 = readPitchDelaySample(1, d1);
    const float l2 = readPitchDelaySample(0, d2);
    const float r2 = readPitchDelaySample(1, d2);

    const float w1 = 0.5f * (1.0f - std::cos(juce::MathConstants<float>::twoPi * static_cast<float>(p1)));
    const float w2 = 1.0f - w1;

    left = l1 * w1 + l2 * w2;
    right = r1 * w1 + r2 * w2;

    pitchShiftWritePos = (pitchShiftWritePos + 1) % pitchShiftDelaySize;
}

void EnhancedAudioStrip::updateFilterCoefficients(float frequency, float resonance)
{
    const float freq = juce::jlimit(20.0f, 20000.0f, frequency);
    const float q = juce::jlimit(0.1f, 10.0f, resonance);
    const float ladderRes = juce::jlimit(0.0f, 1.0f, std::pow((q - 0.1f) / 9.9f, 0.8f));

    filterLp.setCutoffFrequency(freq);
    filterBp.setCutoffFrequency(freq);
    filterHp.setCutoffFrequency(freq);
    filterLpStage2.setCutoffFrequency(freq);
    filterBpStage2.setCutoffFrequency(freq);
    filterHpStage2.setCutoffFrequency(freq);

    filterLp.setResonance(q);
    filterBp.setResonance(q);
    filterHp.setResonance(q);
    filterLpStage2.setResonance(q);
    filterBpStage2.setResonance(q);
    filterHpStage2.setResonance(q);

    ladderLp.setCutoffFrequencyHz(freq);
    ladderBp.setCutoffFrequencyHz(freq);
    ladderHp.setCutoffFrequencyHz(freq);
    ladderLp.setResonance(ladderRes);
    ladderBp.setResonance(ladderRes);
    ladderHp.setResonance(ladderRes);

    if (moogLpL != nullptr && moogLpR != nullptr)
    {
        if (std::abs(freq - cachedMoogCutoff) > 1.0e-3f)
        {
            moogLpL->SetCutoff(freq);
            moogLpR->SetCutoff(freq);
            cachedMoogCutoff = freq;
        }
        const float moogRes = juce::jlimit(0.0f, 1.2f, (q - 0.1f) / 8.0f);
        if (std::abs(moogRes - cachedMoogResonance) > 1.0e-4f)
        {
            moogLpL->SetResonance(moogRes);
            moogLpR->SetResonance(moogRes);
            cachedMoogResonance = moogRes;
        }
    }
}

void EnhancedAudioStrip::ensureAnalogFiltersInitialized(FilterAlgorithm algorithm)
{
    auto makeModel = [&](FilterAlgorithm a) -> std::unique_ptr<LadderFilterBase>
    {
        switch (a)
        {
            case FilterAlgorithm::Tpt12:
            case FilterAlgorithm::Tpt24:
            case FilterAlgorithm::Ladder12:
            case FilterAlgorithm::Ladder24:
                return std::make_unique<StilsonMoog>(static_cast<float>(currentSampleRate));
            case FilterAlgorithm::MoogHuov:
                return std::make_unique<HuovilainenMoog>(static_cast<float>(currentSampleRate));
            case FilterAlgorithm::MoogStilson:
                return std::make_unique<StilsonMoog>(static_cast<float>(currentSampleRate));
        }
    };

    const bool needMoog = (algorithm == FilterAlgorithm::MoogStilson || algorithm == FilterAlgorithm::MoogHuov);
    if (needMoog)
    {
        if (moogLpL == nullptr || moogLpR == nullptr)
        {
            moogLpL = makeModel(algorithm);
            moogLpR = makeModel(algorithm);
            cachedMoogCutoff = -1.0f;
            cachedMoogResonance = -1.0f;
            cachedMoogModel = static_cast<int>(algorithm);
            cachedMoogSampleRate = static_cast<float>(currentSampleRate);
        }
        else
        {
            const bool wrongType = cachedMoogModel != static_cast<int>(algorithm)
                                || std::abs(cachedMoogSampleRate - static_cast<float>(currentSampleRate)) > 0.5f;
            if (wrongType)
            {
                moogLpL = makeModel(algorithm);
                moogLpR = makeModel(algorithm);
                cachedMoogCutoff = -1.0f;
                cachedMoogResonance = -1.0f;
                cachedMoogModel = static_cast<int>(algorithm);
                cachedMoogSampleRate = static_cast<float>(currentSampleRate);
            }
        }
    }

}

void EnhancedAudioStrip::processFilterSample(float& left, float& right, float frequency, float resonance, float morph)
{
    const auto algorithm = getFilterAlgorithm();
    ensureAnalogFiltersInitialized(algorithm);
    updateFilterCoefficients(frequency, resonance);

    float lpL = filterLp.processSample(0, left);
    float bpL = filterBp.processSample(0, left);
    float hpL = filterHp.processSample(0, left);
    float lpR = filterLp.processSample(1, right);
    float bpR = filterBp.processSample(1, right);
    float hpR = filterHp.processSample(1, right);

    if (algorithm == FilterAlgorithm::Tpt24)
    {
        lpL = filterLpStage2.processSample(0, lpL);
        bpL = filterBpStage2.processSample(0, bpL);
        hpL = filterHpStage2.processSample(0, hpL);
        lpR = filterLpStage2.processSample(1, lpR);
        bpR = filterBpStage2.processSample(1, bpR);
        hpR = filterHpStage2.processSample(1, hpR);
    }
    else if (algorithm == FilterAlgorithm::Ladder12 || algorithm == FilterAlgorithm::Ladder24)
    {
        const int wantedMode = (algorithm == FilterAlgorithm::Ladder24) ? 24 : 12;
        if (cachedLadderMode != wantedMode)
        {
            const auto lpMode = (algorithm == FilterAlgorithm::Ladder24)
                ? juce::dsp::LadderFilterMode::LPF24
                : juce::dsp::LadderFilterMode::LPF12;
            const auto bpMode = (algorithm == FilterAlgorithm::Ladder24)
                ? juce::dsp::LadderFilterMode::BPF24
                : juce::dsp::LadderFilterMode::BPF12;
            const auto hpMode = (algorithm == FilterAlgorithm::Ladder24)
                ? juce::dsp::LadderFilterMode::HPF24
                : juce::dsp::LadderFilterMode::HPF12;

            ladderLp.setMode(lpMode);
            ladderBp.setMode(bpMode);
            ladderHp.setMode(hpMode);
            cachedLadderMode = wantedMode;
        }

        lpL = ladderLp.processSample(left, 0);
        bpL = ladderBp.processSample(left, 0);
        hpL = ladderHp.processSample(left, 0);
        lpR = ladderLp.processSample(right, 1);
        bpR = ladderBp.processSample(right, 1);
        hpR = ladderHp.processSample(right, 1);
    }
    else if ((algorithm == FilterAlgorithm::MoogStilson || algorithm == FilterAlgorithm::MoogHuov)
             && moogLpL != nullptr && moogLpR != nullptr)
    {
        float monoL = left;
        float monoR = right;
        moogLpL->Process(&monoL, 1);
        moogLpR->Process(&monoR, 1);
        lpL = monoL;
        lpR = monoR;
    }
    const float m = juce::jlimit(0.0f, 1.0f, morph);
    float wLP = 0.0f;
    float wBP = 0.0f;
    float wHP = 0.0f;
    if (m <= 0.5f)
    {
        const float t = juce::jlimit(0.0f, 1.0f, m * 2.0f);
        wLP = std::cos(t * juce::MathConstants<float>::halfPi);
        wBP = std::sin(t * juce::MathConstants<float>::halfPi);
    }
    else
    {
        const float t = juce::jlimit(0.0f, 1.0f, (m - 0.5f) * 2.0f);
        wBP = std::cos(t * juce::MathConstants<float>::halfPi);
        wHP = std::sin(t * juce::MathConstants<float>::halfPi);
    }

    const float q = juce::jlimit(0.1f, 10.0f, resonance);
    const float bpComp = 1.0f / (1.0f + (0.17f * juce::jmax(0.0f, q - 0.707f)));
    wBP *= bpComp;
    const float norm = 1.0f / std::sqrt(juce::jmax(1.0e-5f, (wLP * wLP) + (wBP * wBP) + (wHP * wHP)));
    wLP *= norm;
    wBP *= norm;
    wHP *= norm;

    left = (lpL * wLP) + (bpL * wBP) + (hpL * wHP);
    right = (lpR * wLP) + (bpR * wBP) + (hpR * wHP);

    // Resonance-aware post gain + limiter: applies to all filter algorithms.
    const float resonanceDrive = juce::jlimit(0.0f, 1.0f, (q - 0.707f) / 9.293f);
    const float resonanceGuardGain = juce::jmap(resonanceDrive, 1.0f, 0.68f);
    left *= resonanceGuardGain;
    right *= resonanceGuardGain;

    left = filterLimiter0dB(left, resonanceDrive);
    right = filterLimiter0dB(right, resonanceDrive);

    // Hard safety guard: never exceed full scale.
    left = safetyClip0dB(left);
    right = safetyClip0dB(right);
}

void EnhancedAudioStrip::setFilterFrequency(float freq)
{
    const float clamped = juce::jlimit(20.0f, 20000.0f, freq);
    filterFrequency.store(clamped, std::memory_order_release);
    smoothedFilterFrequency.setTargetValue(clamped);
    if (!filterEnabled)
        filterEnabled = true;
}

void EnhancedAudioStrip::setFilterResonance(float res)
{
    const float clamped = juce::jlimit(0.1f, 10.0f, res);
    filterResonance.store(clamped, std::memory_order_release);
    smoothedFilterResonance.setTargetValue(clamped);
    if (!filterEnabled)
        filterEnabled = true;
}

void EnhancedAudioStrip::setFilterMorph(float morph)
{
    const float clamped = juce::jlimit(0.0f, 1.0f, morph);
    filterMorph.store(clamped, std::memory_order_release);
    smoothedFilterMorph.setTargetValue(clamped);

    // Keep legacy type snapshot coherent for preset compatibility.
    if (clamped < 0.3333f) filterType = FilterType::LowPass;
    else if (clamped < 0.6666f) filterType = FilterType::BandPass;
    else filterType = FilterType::HighPass;

    if (!filterEnabled)
        filterEnabled = true;
}

void EnhancedAudioStrip::setFilterType(FilterType type)
{
    filterType = type;
    switch (type)
    {
        case FilterType::LowPass: setFilterMorph(0.0f); break;
        case FilterType::BandPass: setFilterMorph(0.5f); break;
        case FilterType::HighPass: setFilterMorph(1.0f); break;
        default: setFilterMorph(0.0f); break;
    }
}

void EnhancedAudioStrip::setFilterAlgorithm(FilterAlgorithm algorithm)
{
    const int raw = juce::jlimit(0, 5, static_cast<int>(algorithm));
    filterAlgorithm.store(raw, std::memory_order_release);
    if (!filterEnabled)
        filterEnabled = true;
}

double EnhancedAudioStrip::applySwingToPpq(double ppq) const
{
    const double swing = static_cast<double>(swingAmount.load(std::memory_order_acquire));
    if (swing <= 1.0e-6)
        return ppq;

    const auto division = getSwingDivision();
    const double unitBeats = [&]()
    {
        switch (division)
        {
            case SwingDivision::Quarter: return 1.0;
            case SwingDivision::Sixteenth: return 0.25;
            case SwingDivision::Triplet: return 1.0 / 3.0;
            case SwingDivision::Eighth:
            default: return 0.5;
        }
    }();

    const double pairLength = unitBeats * 2.0;
    if (pairLength <= 1.0e-9)
        return ppq;

    const double pairIndex = std::floor(ppq / pairLength);
    const double pairBase = pairIndex * pairLength;
    const double pairPhase = ppq - pairBase; // [0..pairLength)

    // More gradual onset in low range, still extreme near max.
    const double shapedSwing = std::pow(juce::jlimit(0.0, 1.0, swing), 1.7);
    const double splitShift = juce::jlimit(0.0, 0.96, shapedSwing * 0.96);
    const double splitPoint = unitBeats * (1.0 + splitShift);

    double swungPhase = pairPhase;
    if (pairPhase < unitBeats)
    {
        const double t = pairPhase / juce::jmax(1.0e-9, unitBeats);
        swungPhase = splitPoint * t;
    }
    else
    {
        const double t = (pairPhase - unitBeats) / juce::jmax(1.0e-9, unitBeats);
        swungPhase = splitPoint + ((pairLength - splitPoint) * t);
    }

    return pairBase + swungPhase;
}

float EnhancedAudioStrip::computeGateModulation(double ppq) const
{
    const float amount = gateAmount.load(std::memory_order_acquire);
    if (amount <= 1.0e-4f)
        return 1.0f;

    const float speed = gateSpeed.load(std::memory_order_acquire);
    const float env = gateEnvelope.load(std::memory_order_acquire);
    const float shape = gateShape.load(std::memory_order_acquire);

    const double phase = ppq * static_cast<double>(speed);
    const float p = static_cast<float>(phase - std::floor(phase)); // 0..1

    // Continuous gate shape:
    // low shape = short/tight pulses, high shape = longer open pulses.
    const float pulseWidth = juce::jmap(juce::jlimit(0.0f, 1.0f, shape), 0.0f, 1.0f, 0.01f, 0.95f);
    const float halfWidth = juce::jmax(1.0e-4f, 0.5f * pulseWidth);
    const float distance = std::abs(p - 0.5f);
    const float core = juce::jlimit(0.0f, 1.0f, 1.0f - (distance / halfWidth));

    const float soft = core * core * (3.0f - (2.0f * core)); // smoothstep
    float wave = juce::jmap(juce::jlimit(0.0f, 1.0f, env), core, soft);

    // Envelope controls curve softness (hard at 0, smooth at 1).
    const float exponent = juce::jmap(env, 0.0f, 1.0f, 3.2f, 0.8f);
    const float shaped = std::pow(juce::jlimit(0.0f, 1.0f, wave), exponent);
    return juce::jlimit(0.0f, 1.0f, (1.0f - amount) + (amount * shaped));
}

double EnhancedAudioStrip::getNormalizedPosition() const
{
    if (sampleLength <= 0.0)
        return 0.0;
    return playbackPosition / sampleLength;
}

int EnhancedAudioStrip::getCurrentColumn() const
{
    double normalized = getNormalizedPosition();
    return static_cast<int>(normalized * ModernAudioEngine::MaxColumns) % ModernAudioEngine::MaxColumns;
}

bool EnhancedAudioStrip::isGrainFreezeActive() const
{
    return grainLedFreeze.load(std::memory_order_acquire);
}

int EnhancedAudioStrip::getGrainAnchorColumn() const
{
    return grainLedAnchor.load(std::memory_order_acquire);
}

int EnhancedAudioStrip::getGrainSecondaryColumn() const
{
    return grainLedSecondary.load(std::memory_order_acquire);
}

int EnhancedAudioStrip::getGrainSizeControlColumn() const
{
    return grainLedSizeControl.load(std::memory_order_acquire);
}

int EnhancedAudioStrip::getGrainHeldCount() const
{
    return grainLedHeldCount.load(std::memory_order_acquire);
}

float EnhancedAudioStrip::getGrainSizeMs() const
{
    const float modulated = grainSizeModulatedMsAtomic.load(std::memory_order_acquire);
    if (modulated >= kGrainMinSizeMs)
        return juce::jlimit(kGrainMinSizeMs, kGrainMaxSizeMs, modulated);
    return grainSizeMsAtomic.load(std::memory_order_acquire);
}

float EnhancedAudioStrip::getGrainBaseSizeMs() const
{
    return juce::jlimit(kGrainMinSizeMs, kGrainMaxSizeMs,
                        grainSizeMsAtomic.load(std::memory_order_acquire));
}

float EnhancedAudioStrip::getGrainDensity() const
{
    return grainDensityAtomic.load(std::memory_order_acquire);
}

float EnhancedAudioStrip::getGrainPitch() const
{
    return grainPitchAtomic.load(std::memory_order_acquire);
}

float EnhancedAudioStrip::getGrainPitchJitter() const
{
    return grainPitchJitterAtomic.load(std::memory_order_acquire);
}

float EnhancedAudioStrip::getGrainSpread() const
{
    return grainSpreadAtomic.load(std::memory_order_acquire);
}

float EnhancedAudioStrip::getGrainJitter() const
{
    return grainJitterAtomic.load(std::memory_order_acquire);
}

float EnhancedAudioStrip::getGrainRandomDepth() const
{
    return grainRandomDepthAtomic.load(std::memory_order_acquire);
}

float EnhancedAudioStrip::getGrainArpDepth() const
{
    return grainArpDepthAtomic.load(std::memory_order_acquire);
}

float EnhancedAudioStrip::getGrainCloudDepth() const
{
    return grainCloudDepthAtomic.load(std::memory_order_acquire);
}

float EnhancedAudioStrip::getGrainEmitterDepth() const
{
    return grainEmitterDepthAtomic.load(std::memory_order_acquire);
}

float EnhancedAudioStrip::getGrainEnvelope() const
{
    return grainEnvelopeAtomic.load(std::memory_order_acquire);
}

float EnhancedAudioStrip::getGrainShape() const
{
    return grainShapeAtomic.load(std::memory_order_acquire);
}

int EnhancedAudioStrip::getGrainArpMode() const
{
    return grainArpModeAtomic.load(std::memory_order_acquire);
}

bool EnhancedAudioStrip::isGrainTempoSyncEnabled() const
{
    return grainTempoSyncAtomic.load(std::memory_order_acquire);
}

std::array<float, 8> EnhancedAudioStrip::getGrainPreviewPositions() const
{
    grainPreviewRequestCountdown.store(8, std::memory_order_release);
    std::array<float, 8> out{};
    for (size_t i = 0; i < out.size(); ++i)
        out[i] = grainPreviewPositions[i].load(std::memory_order_acquire);
    return out;
}

std::array<float, 8> EnhancedAudioStrip::getGrainPreviewPitchNorms() const
{
    grainPreviewRequestCountdown.store(8, std::memory_order_release);
    std::array<float, 8> out{};
    for (size_t i = 0; i < out.size(); ++i)
        out[i] = grainPreviewPitchNorms[i].load(std::memory_order_acquire);
    return out;
}

void EnhancedAudioStrip::setGrainSizeMs(float value)
{
    juce::ScopedLock lock(bufferLock);
    grainParams.sizeMs = juce::jlimit(kGrainMinSizeMs, kGrainMaxSizeMs, value);
    grainSizeSmoother.setTargetValue(grainParams.sizeMs);
    grainSizeMsAtomic.store(grainParams.sizeMs, std::memory_order_release);
}

void EnhancedAudioStrip::setGrainSizeModulatedMs(float value)
{
    grainSizeModulatedMsAtomic.store(juce::jlimit(kGrainMinSizeMs, kGrainMaxSizeMs, value), std::memory_order_release);
}

void EnhancedAudioStrip::clearGrainSizeModulation()
{
    grainSizeModulatedMsAtomic.store(-1.0f, std::memory_order_release);
}

void EnhancedAudioStrip::setGrainDensity(float value)
{
    juce::ScopedLock lock(bufferLock);
    grainParams.density = juce::jlimit(kGrainMinDensity, kGrainMaxDensity, value);
    grainDensitySmoother.setTargetValue(grainParams.density);
    grainDensityAtomic.store(grainParams.density, std::memory_order_release);
}

void EnhancedAudioStrip::setGrainPitch(float semitones)
{
    juce::ScopedLock lock(bufferLock);
    grainParams.pitchSemitones = juce::jlimit(-48.0f, 48.0f, semitones);
    grainPitchAtomic.store(grainParams.pitchSemitones, std::memory_order_release);
    grainPitchSmoother.setTargetValue(grainParams.pitchSemitones);
    if (!grainArpWasActive)
        grainPitchBeforeArp = grainParams.pitchSemitones;
}

void EnhancedAudioStrip::setGrainPitchJitter(float semitones)
{
    juce::ScopedLock lock(bufferLock);
    grainParams.pitchJitterSemitones = juce::jlimit(0.0f, 48.0f, semitones);
    grainPitchJitterAtomic.store(grainParams.pitchJitterSemitones, std::memory_order_release);
    grainPitchJitterSmoother.setTargetValue(grainParams.pitchJitterSemitones);
}

void EnhancedAudioStrip::setGrainSpread(float value)
{
    juce::ScopedLock lock(bufferLock);
    grainParams.spread = juce::jlimit(0.0f, 1.0f, value);
    grainSpreadAtomic.store(grainParams.spread, std::memory_order_release);
}

void EnhancedAudioStrip::setGrainJitter(float value)
{
    juce::ScopedLock lock(bufferLock);
    grainParams.jitter = juce::jlimit(0.0f, 1.0f, value);
    grainJitterAtomic.store(grainParams.jitter, std::memory_order_release);
}

void EnhancedAudioStrip::setGrainRandomDepth(float value)
{
    juce::ScopedLock lock(bufferLock);
    grainParams.randomDepth = juce::jlimit(0.0f, 1.0f, value);
    grainRandomDepthAtomic.store(grainParams.randomDepth, std::memory_order_release);
}

void EnhancedAudioStrip::setGrainArpDepth(float value)
{
    juce::ScopedLock lock(bufferLock);
    const float clamped = juce::jlimit(0.0f, 1.0f, value);
    const bool wasActive = grainArpWasActive;
    const bool nowActive = clamped > 0.001f;
    if (!wasActive && nowActive)
        grainPitchBeforeArp = grainParams.pitchSemitones;
    grainParams.arpDepth = clamped;
    grainArpDepthAtomic.store(grainParams.arpDepth, std::memory_order_release);
    grainArpWasActive = nowActive;

    if (wasActive && !nowActive)
    {
        grainParams.pitchSemitones = juce::jlimit(-48.0f, 48.0f, grainPitchBeforeArp);
        grainPitchAtomic.store(grainParams.pitchSemitones, std::memory_order_release);
        grainPitchSmoother.setTargetValue(grainParams.pitchSemitones);
    }
}

void EnhancedAudioStrip::setGrainCloudDepth(float value)
{
    juce::ScopedLock lock(bufferLock);
    grainParams.cloudDepth = juce::jlimit(0.0f, 1.0f, value);
    grainCloudDepthAtomic.store(grainParams.cloudDepth, std::memory_order_release);
}

void EnhancedAudioStrip::setGrainEmitterDepth(float value)
{
    juce::ScopedLock lock(bufferLock);
    grainParams.emitterDepth = juce::jlimit(0.0f, 1.0f, value);
    grainEmitterDepthAtomic.store(grainParams.emitterDepth, std::memory_order_release);
}

void EnhancedAudioStrip::setGrainEnvelope(float value)
{
    juce::ScopedLock lock(bufferLock);
    grainParams.envelope = juce::jlimit(0.0f, 1.0f, value);
    grainEnvelopeAtomic.store(grainParams.envelope, std::memory_order_release);
}

void EnhancedAudioStrip::setGrainShape(float value)
{
    juce::ScopedLock lock(bufferLock);
    grainParams.shape = juce::jlimit(-1.0f, 1.0f, value);
    grainShapeAtomic.store(grainParams.shape, std::memory_order_release);
}

void EnhancedAudioStrip::setGrainArpMode(int mode)
{
    juce::ScopedLock lock(bufferLock);
    grainParams.arpMode = juce::jlimit(0, 5, mode);
    grainArpModeAtomic.store(grainParams.arpMode, std::memory_order_release);
}

void EnhancedAudioStrip::setGrainTempoSyncEnabled(bool enabled)
{
    juce::ScopedLock lock(bufferLock);
    grainTempoSyncAtomic.store(enabled, std::memory_order_release);
}

std::array<bool, 16> EnhancedAudioStrip::getLEDStates() const
{
    std::array<bool, 16> states;
    states.fill(false);
    
    // In Step mode, LEDs are handled separately by step pattern display
    if (playMode == PlayMode::Step)
    {
        return states;  // All off - step display handles LEDs
    }
    
    if (playing)
    {
        int currentCol = getCurrentColumn();
        states[static_cast<size_t>(currentCol)] = true;
        
        if (loopEnabled)
        {
            for (int i = loopStart; i < loopEnd; ++i)
                states[static_cast<size_t>(i)] = true;
        }
    }
    
    return states;
}

void EnhancedAudioStrip::handleLooping()
{
    double pos = playbackPosition.load();
    
    if (playMode == PlayMode::OneShot)
    {
        if (pos >= sampleLength || pos < 0)
        {
            playing = false;
            playbackPosition = 0.0;
        }
    }
    else if (playMode == PlayMode::Loop)  // Loop mode handles wrapping
    {
        double loopStartPos = (loopStart / static_cast<double>(ModernAudioEngine::MaxColumns)) * sampleLength;
        double loopEndPos = (loopEnd / static_cast<double>(ModernAudioEngine::MaxColumns)) * sampleLength;
        
        if (reverse)
        {
            if (pos < loopStartPos)
                playbackPosition = loopEndPos;
        }
        else
        {
            if (pos >= loopEndPos)
                playbackPosition = loopStartPos;
        }
    }
    else if (directionMode == DirectionMode::PingPong)  // Ping-pong bounces
    {
        double loopStartPos = (loopStart / static_cast<double>(ModernAudioEngine::MaxColumns)) * sampleLength;
        double loopEndPos = (loopEnd / static_cast<double>(ModernAudioEngine::MaxColumns)) * sampleLength;
        
        if (pos >= loopEndPos || pos < loopStartPos)
        {
            reverse = !reverse;
            playbackPosition = juce::jlimit(loopStartPos, loopEndPos, pos);
        }
    }
}

float EnhancedAudioStrip::getPanGain(int channel) const
{
    float panVal = pan.load();  // -1 (left) to +1 (right)
    
    // Constant power pan law with correct channel assignment
    // Left channel: full volume at -1, decreases towards +1
    // Right channel: full volume at +1, decreases towards -1
    const float pi = 3.14159265359f;
    
    if (channel == 0) // Left channel
    {
        // Left: -1 (full) → 0 (√2/2) → +1 (silent)
        float angle = (panVal + 1.0f) * pi * 0.25f;  // 0 to π/2
        return std::cos(angle);
    }
    else // Right channel
    {
        // Right: -1 (silent) → 0 (√2/2) → +1 (full)
        float angle = (panVal + 1.0f) * pi * 0.25f;  // 0 to π/2
        return std::sin(angle);
    }
}

//==============================================================================
// ModernAudioEngine Implementation
//==============================================================================

ModernAudioEngine::ModernAudioEngine()
{
    // Initialize strips
    for (int i = 0; i < MaxStrips; ++i)
        strips[static_cast<size_t>(i)] = std::make_unique<EnhancedAudioStrip>(i);
    
    // Initialize groups
    for (int i = 0; i < MaxGroups; ++i)
    {
        groups[static_cast<size_t>(i)] = std::make_unique<StripGroup>(i);
        
        // Default: assign 2 strips per group
        int stripsPerGroup = MaxStrips / MaxGroups;
        for (int j = 0; j < stripsPerGroup; ++j)
        {
            int stripIndex = i * stripsPerGroup + j;
            if (stripIndex < MaxStrips)
            {
                groups[static_cast<size_t>(i)]->addStrip(stripIndex);
                strips[static_cast<size_t>(stripIndex)]->setGroup(i);
            }
        }
    }
    
    // Initialize patterns
    for (int i = 0; i < 4; ++i)
        patterns[static_cast<size_t>(i)] = std::make_unique<PatternRecorder>();
    
    liveRecorder = std::make_unique<LiveRecorder>();

    for (auto& seq : modSequencers)
    {
        seq.target.store(static_cast<int>(ModTarget::None), std::memory_order_release);
        seq.bipolar.store(0, std::memory_order_release);
        seq.curveMode.store(0, std::memory_order_release);
        seq.depth.store(1.0f, std::memory_order_release);
        seq.offset.store(0, std::memory_order_release);
        seq.lengthBars.store(1, std::memory_order_release);
        seq.editPage.store(0, std::memory_order_release);
        seq.smoothingMs.store(0.0f, std::memory_order_release);
        seq.curveBend.store(0.0f, std::memory_order_release);
        seq.curveShape.store(static_cast<int>(ModCurveShape::Power), std::memory_order_release);
        seq.pitchScaleQuantize.store(0, std::memory_order_release);
        seq.pitchScale.store(static_cast<int>(PitchScale::Chromatic), std::memory_order_release);
        seq.smoothedRaw = 0.0f;
        seq.grainDezipperedRaw = 0.0f;
        seq.pitchDezipperedRaw = 0.0f;
        seq.lastGlobalStep.store(0, std::memory_order_release);
        for (auto& step : seq.steps)
            step.store(0.0f, std::memory_order_release);
    }

    for (int i = 0; i < MaxStrips; ++i)
    {
        momentaryStutterStripEnabled[static_cast<size_t>(i)].store(0, std::memory_order_release);
        momentaryStutterColumns[static_cast<size_t>(i)].store(0, std::memory_order_release);
        momentaryStutterNextPpq[static_cast<size_t>(i)].store(0.0, std::memory_order_release);
    }
}

void ModernAudioEngine::prepareToPlay(double sampleRate, int maxBlockSize)
{
    currentSampleRate = sampleRate;
    currentBlockSize = maxBlockSize;
    lastPatternProcessBeat = -1.0;
    
    quantizeClock.setSampleRate(sampleRate);
    
    for (auto& strip : strips)
        strip->prepareToPlay(sampleRate, maxBlockSize);
    
    liveRecorder->prepareToPlay(sampleRate, maxBlockSize);
    setCrossfadeLengthMs(crossfadeLengthMs.load(std::memory_order_acquire));
    setTriggerFadeInMs(triggerFadeInMs.load(std::memory_order_acquire));
}

void ModernAudioEngine::processBlock(juce::AudioBuffer<float>& buffer, 
                                     juce::MidiBuffer& /*midi*/,
                                     const juce::AudioPlayHead::PositionInfo& positionInfo,
                                     const std::array<juce::AudioBuffer<float>*, MaxStrips>* stripOutputs)
{
    juce::ScopedNoDenormals noDenormals;
    
    // Update tempo
    updateTempo(positionInfo);
    
    const int numSamples = buffer.getNumSamples();
    const int numChannels = buffer.getNumChannels();
    
    // Safety check - need at least 1 channel and 1 sample
    if (numChannels == 0 || numSamples == 0)
        return;
    
    // Save input for monitoring and calculate levels
    float inputMonitorVol = inputMonitorVolume.load();
    auto& inputCopy = inputMonitorScratch;
    
    // Calculate input levels for metering
    if (numChannels > 0 && numSamples > 0)
    {
        // Get RMS levels for left channel
        float levelL = buffer.getRMSLevel(0, 0, numSamples);
        inputLevelL = levelL;
        
        // Get RMS levels for right channel (or duplicate left if mono)
        if (numChannels >= 2)
        {
            float levelR = buffer.getRMSLevel(1, 0, numSamples);
            inputLevelR = levelR;
        }
        else
        {
            inputLevelR = levelL;  // Mono - duplicate to right meter
        }
    }
    
    if (inputMonitorVol > 0.0f && numChannels > 0)
    {
        // Copy input for monitoring
        inputCopy.setSize(numChannels, numSamples, false, false, true);
        for (int ch = 0; ch < numChannels; ++ch)
            inputCopy.copyFrom(ch, 0, buffer, ch, 0, numSamples);
    }
    
    // CRITICAL: Process live recording BEFORE buffer.clear()
    // This captures the raw input audio before it's cleared for strip output
    liveRecorder->processInput(buffer, 0, numSamples);
    
    // Clear output
    buffer.clear();
    
    const int64_t blockStart = globalSampleCount.load();
    const int64_t blockEnd = blockStart + numSamples;
    const double tempoNow = currentTempo.load();
    const double quantizeBeatsNow = quantizeClock.getQuantBeats();
    const bool hasPpq = positionInfo.getPpqPosition().hasValue();
    const double basePpq = hasPpq ? *positionInfo.getPpqPosition() : 0.0;
    const double samplesPerBeat = (tempoNow > 0.0) ? ((60.0 / tempoNow) * currentSampleRate) : 0.0;
    const bool modulationPpqReady = hasPpq && samplesPerBeat > 0.0;

    auto makeSegmentPositionInfo = [&](int sampleOffset)
    {
        juce::AudioPlayHead::PositionInfo segmentInfo = positionInfo;
        if (hasPpq && samplesPerBeat > 0.0)
            segmentInfo.setPpqPosition(basePpq + (static_cast<double>(sampleOffset) / samplesPerBeat));
        return segmentInfo;
    };

    auto processStripsSegment = [&](int startSample, int segmentSamples)
    {
        if (segmentSamples <= 0)
            return;

        const int64_t segmentGlobalSample = blockStart + startSample;
        const auto segmentPosInfo = makeSegmentPositionInfo(startSample);

        for (size_t i = 0; i < static_cast<size_t>(MaxStrips); ++i)
        {
            if (!strips[i])
                continue;

            auto* strip = strips[i].get();
            auto& seq = modSequencers[i];
            const auto target = static_cast<ModTarget>(seq.target.load(std::memory_order_acquire));
            const bool stripPlaying = strip->isPlaying();
            const bool applyParamMod = modulationPpqReady
                && stripPlaying
                && target != ModTarget::None
                && target != ModTarget::Retrigger;

            float originalVol = 1.0f;
            float originalPan = 0.0f;
            float originalSpeed = 1.0f;
            float originalPitch = 0.0f;
            float originalFilterFreq = 20000.0f;
            float originalFilterRes = 0.707f;
            float originalGrainSize = 1240.0f;
            float originalGrainDensity = 0.05f;
            float originalGrainPitch = 0.0f;
            float originalGrainPitchJitter = 0.0f;
            float originalGrainSpread = 0.0f;
            float originalGrainJitter = 0.0f;
            float originalGrainRandom = 0.0f;
            float originalGrainArp = 0.0f;
            float originalGrainCloud = 0.0f;
            float originalGrainEmitter = 0.0f;
            float originalGrainEnvelope = 0.0f;
            if (applyParamMod)
            {
                originalVol = strip->getVolume();
                originalPan = strip->getPan();
                originalSpeed = strip->getPlaybackSpeed();
                originalPitch = strip->getPitchShift();
                originalFilterFreq = strip->getFilterFrequency();
                originalFilterRes = strip->getFilterResonance();
                originalGrainSize = strip->getGrainBaseSizeMs();
                originalGrainDensity = strip->getGrainDensity();
                originalGrainPitch = strip->getGrainPitch();
                originalGrainPitchJitter = strip->getGrainPitchJitter();
                originalGrainSpread = strip->getGrainSpread();
                originalGrainJitter = strip->getGrainJitter();
                originalGrainRandom = strip->getGrainRandomDepth();
                originalGrainArp = strip->getGrainArpDepth();
                originalGrainCloud = strip->getGrainCloudDepth();
                originalGrainEmitter = strip->getGrainEmitterDepth();
                originalGrainEnvelope = strip->getGrainEnvelope();
            }

            const int lengthBars = juce::jlimit(1, MaxModBars, seq.lengthBars.load(std::memory_order_acquire));
            const int totalSteps = juce::jmax(ModSteps, ModSteps * lengthBars);
            const int offset = seq.offset.load(std::memory_order_acquire);
            int globalStep = seq.lastGlobalStep.load(std::memory_order_acquire);

            double stepPhase = 0.0;
            if (stripPlaying && modulationPpqReady)
            {
                const double stepPpq = basePpq + (static_cast<double>(startSample) / juce::jmax(1.0, samplesPerBeat));
                const double stepsPos = (stepPpq * 4.0) + static_cast<double>(offset);
                const double wrapped = std::fmod(stepsPos, static_cast<double>(totalSteps));
                const double wrappedPos = (wrapped < 0.0) ? (wrapped + static_cast<double>(totalSteps)) : wrapped;
                globalStep = juce::jlimit(0, totalSteps - 1, static_cast<int>(std::floor(wrappedPos)));
                stepPhase = juce::jlimit(0.0, 1.0, wrappedPos - static_cast<double>(globalStep));
                seq.lastGlobalStep.store(globalStep, std::memory_order_release);
            }

            if (applyParamMod)
            {
                // Grain-size modulation is applied via runtime override to avoid zippering
                // from write/restore ping-pong against the base parameter.
                strip->clearGrainSizeModulation();

                const int nextStep = (globalStep + 1) % totalSteps;
                const float rawA = juce::jlimit(0.0f, 1.0f, seq.steps[static_cast<size_t>(globalStep)].load(std::memory_order_acquire));
                const float rawB = juce::jlimit(0.0f, 1.0f, seq.steps[static_cast<size_t>(nextStep)].load(std::memory_order_acquire));
                const float smoothingMs = juce::jmax(0.0f, seq.smoothingMs.load(std::memory_order_acquire));
                const float curveBend = juce::jlimit(-1.0f, 1.0f, seq.curveBend.load(std::memory_order_acquire));
                const bool loopSpeedImmediate = (target == ModTarget::Speed
                    && strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Loop);
                const bool curveMode = (seq.curveMode.load(std::memory_order_acquire) != 0);
                const auto curveShape = static_cast<ModCurveShape>(juce::jlimit(
                    0, static_cast<int>(ModCurveShape::Stair), seq.curveShape.load(std::memory_order_acquire)));
                float shapedRaw = rawA;
                if (curveMode)
                {
                    const float shapedPhase = shapeModCurvePhase(static_cast<float>(stepPhase), curveBend, curveShape);
                    shapedRaw = juce::jlimit(0.0f, 1.0f, rawA + ((rawB - rawA) * shapedPhase));
                }

                float smoothedRaw = shapedRaw;
                if (loopSpeedImmediate)
                {
                    // Loop-speed modulation is quantized/PPQ-driven and must respond instantly.
                    seq.smoothedRaw = shapedRaw;
                    smoothedRaw = shapedRaw;
                }
                else if (smoothingMs > 0.0f)
                {
                    const float smoothingSamples = juce::jmax(1.0f, (smoothingMs * 0.001f) * static_cast<float>(currentSampleRate));
                    const float alpha = 1.0f - std::exp(-static_cast<float>(segmentSamples) / smoothingSamples);
                    seq.smoothedRaw += (shapedRaw - seq.smoothedRaw) * juce::jlimit(0.0f, 1.0f, alpha);
                    smoothedRaw = seq.smoothedRaw;
                }
                else
                {
                    // True zero smoothing: use step values directly.
                    seq.smoothedRaw = shapedRaw;
                }
                float controlRaw = smoothedRaw;
                if (isGrainModTarget(target))
                {
                    // Safety dezipper for grain-FX automation, independent of user smoothing.
                    // Keeps modulation sample-safe for dense grain parameter changes.
                    constexpr float kGrainDezipMs = 8.0f;
                    const float dezipSamples = juce::jmax(1.0f, (kGrainDezipMs * 0.001f) * static_cast<float>(currentSampleRate));
                    const float alpha = 1.0f - std::exp(-static_cast<float>(segmentSamples) / dezipSamples);
                    seq.grainDezipperedRaw += (smoothedRaw - seq.grainDezipperedRaw) * juce::jlimit(0.0f, 1.0f, alpha);
                    controlRaw = seq.grainDezipperedRaw;
                }
                else
                {
                    seq.grainDezipperedRaw = smoothedRaw;
                }
                if (target == ModTarget::Pitch)
                {
                    // Dedicated pitch dezipper: keeps pitch-target modulation
                    // free of crackles even with low/zero user smoothing.
                    constexpr float kPitchDezipMs = 10.0f;
                    const float dezipSamples = juce::jmax(1.0f, (kPitchDezipMs * 0.001f) * static_cast<float>(currentSampleRate));
                    const float alpha = 1.0f - std::exp(-static_cast<float>(segmentSamples) / dezipSamples);
                    seq.pitchDezipperedRaw += (controlRaw - seq.pitchDezipperedRaw) * juce::jlimit(0.0f, 1.0f, alpha);
                    controlRaw = seq.pitchDezipperedRaw;
                }
                else
                {
                    seq.pitchDezipperedRaw = controlRaw;
                }

                const float depth = juce::jlimit(0.0f, 1.0f, seq.depth.load(std::memory_order_acquire));
                const bool bipolar = modTargetSupportsBipolar(target) && (seq.bipolar.load(std::memory_order_acquire) != 0);
                const float modNorm = juce::jlimit(0.0f, 1.0f, controlRaw * depth);
                const float modBi = juce::jlimit(-1.0f, 1.0f, ((controlRaw * 2.0f) - 1.0f) * depth);
                const float modSigned = bipolar ? modBi : modNorm;

                switch (target)
                {
                    case ModTarget::Volume:
                        strip->setVolume(juce::jlimit(0.0f, 1.0f, originalVol * ((1.0f - depth) + modNorm)));
                        break;
                    case ModTarget::Pan:
                        // Mod lane polarity: up = left, down = right.
                        strip->setPan(juce::jlimit(-1.0f, 1.0f, originalPan - modSigned));
                        break;
                    case ModTarget::Pitch:
                    {
                        float pitchDelta = 24.0f * modSigned;
                        float targetPitch = originalPitch + pitchDelta;
                        const bool quantize = (seq.pitchScaleQuantize.load(std::memory_order_acquire) != 0);
                        if (quantize)
                        {
                            const auto scale = static_cast<PitchScale>(juce::jlimit(
                                0, static_cast<int>(PitchScale::PentatonicMinor), seq.pitchScale.load(std::memory_order_acquire)));
                            targetPitch = quantizePitchDeltaToScale(targetPitch, scale);
                        }
                        strip->setPitchShift(juce::jlimit(-24.0f, 24.0f, targetPitch));
                        break;
                    }
                    case ModTarget::Speed:
                    {
                        const float speedRatioRaw = quantizeSpeedRatioMusical(controlRaw);
                        const float speedRatio = std::pow(speedRatioRaw, depth);
                        const float targetSpeed = juce::jlimit(0.125f, 4.0f, originalSpeed * speedRatio);
                        if (loopSpeedImmediate)
                            strip->setPlaybackSpeedImmediate(targetSpeed);
                        else
                            strip->setPlaybackSpeed(targetSpeed);
                        break;
                    }
                    case ModTarget::Cutoff:
                    {
                        const float targetFreq = 20.0f * std::pow(1000.0f, juce::jlimit(0.0f, 1.0f, modNorm));
                        strip->setFilterFrequency(juce::jmap(depth, originalFilterFreq, targetFreq));
                        break;
                    }
                    case ModTarget::Resonance:
                    {
                        const float targetRes = juce::jmap(juce::jlimit(0.0f, 1.0f, modNorm), 0.1f, 10.0f);
                        strip->setFilterResonance(juce::jmap(depth, originalFilterRes, targetRes));
                        break;
                    }
                    case ModTarget::GrainSize:
                    {
                        // Relative modulation around current grain size (not absolute lane->size mapping).
                        // This preserves the knob as center/reference and makes depth meaningful in both step/curve modes.
                        const float sizeNorm = juce::jlimit(0.0f, 1.0f,
                            (originalGrainSize - kGrainMinSizeMs) / (kGrainMaxSizeMs - kGrainMinSizeMs));
                        const float rangeMs = juce::jmax(10.0f, (kGrainMaxSizeMs - kGrainMinSizeMs) * (0.08f + (0.42f * sizeNorm)));
                        const float delta = bipolar ? (rangeMs * modBi) : (rangeMs * modNorm);
                        const float targetSize = juce::jlimit(kGrainMinSizeMs, kGrainMaxSizeMs, originalGrainSize + delta);
                        strip->setGrainSizeModulatedMs(targetSize);
                        break;
                    }
                    case ModTarget::GrainDensity:
                        strip->setGrainDensity(juce::jlimit(0.05f, 0.9f, strip->getGrainDensity() + (0.4f * modSigned)));
                        break;
                    case ModTarget::GrainPitch:
                        strip->setGrainPitch(juce::jlimit(-48.0f, 48.0f, strip->getGrainPitch() + (24.0f * modSigned)));
                        break;
                    case ModTarget::GrainPitchJitter:
                        strip->setGrainPitchJitter(juce::jlimit(0.0f, 48.0f, strip->getGrainPitchJitter() + (16.0f * modNorm)));
                        break;
                    case ModTarget::GrainSpread:
                        strip->setGrainSpread(juce::jlimit(0.0f, 1.0f, strip->getGrainSpread() + (0.5f * modSigned)));
                        break;
                    case ModTarget::GrainJitter:
                        strip->setGrainJitter(juce::jlimit(0.0f, 1.0f, strip->getGrainJitter() + (0.5f * modNorm)));
                        break;
                    case ModTarget::GrainRandom:
                        strip->setGrainRandomDepth(juce::jlimit(0.0f, 1.0f, strip->getGrainRandomDepth() + (0.5f * modNorm)));
                        break;
                    case ModTarget::GrainArp:
                        strip->setGrainArpDepth(juce::jlimit(0.0f, 1.0f, strip->getGrainArpDepth() + (0.5f * modNorm)));
                        break;
                    case ModTarget::GrainCloud:
                        strip->setGrainCloudDepth(juce::jlimit(0.0f, 1.0f, strip->getGrainCloudDepth() + (0.5f * modNorm)));
                        break;
                    case ModTarget::GrainEmitter:
                        strip->setGrainEmitterDepth(juce::jlimit(0.0f, 1.0f, strip->getGrainEmitterDepth() + (0.5f * modNorm)));
                        break;
                    case ModTarget::GrainEnvelope:
                        strip->setGrainEnvelope(juce::jlimit(0.0f, 1.0f, strip->getGrainEnvelope() + (0.5f * modNorm)));
                        break;
                    case ModTarget::Retrigger:
                        break;
                    case ModTarget::None:
                    default:
                        break;
                }
            }
            else
            {
                strip->clearGrainSizeModulation();
            }

            juce::AudioBuffer<float>* targetBuffer = &buffer;
            if (stripOutputs != nullptr)
            {
                auto* requested = (*stripOutputs)[i];
                if (requested != nullptr && requested->getNumChannels() > 0)
                    targetBuffer = requested;
            }

            int groupId = strip->getGroup();
            if (groupId >= 0 && groupId < MaxGroups && groups[static_cast<size_t>(groupId)])
            {
                auto* group = groups[static_cast<size_t>(groupId)].get();
                if (!group->isMuted())
                {
                    float groupVol = group->getVolume();
                    float preGroupVol = strip->getVolume();
                    strip->setVolume(preGroupVol * groupVol);
                    strip->process(*targetBuffer, startSample, segmentSamples, segmentPosInfo, segmentGlobalSample, tempoNow, quantizeBeatsNow);
                    strip->setVolume(preGroupVol);
                }
            }
            else
            {
                strip->process(*targetBuffer, startSample, segmentSamples, segmentPosInfo, segmentGlobalSample, tempoNow, quantizeBeatsNow);
            }

            if (applyParamMod)
            {
                strip->setVolume(originalVol);
                strip->setPan(originalPan);
                strip->setPlaybackSpeedImmediate(originalSpeed);
                strip->setPitchShift(originalPitch);
                strip->setFilterFrequency(originalFilterFreq);
                strip->setFilterResonance(originalFilterRes);
                strip->setGrainDensity(originalGrainDensity);
                strip->setGrainPitch(originalGrainPitch);
                strip->setGrainPitchJitter(originalGrainPitchJitter);
                strip->setGrainSpread(originalGrainSpread);
                strip->setGrainJitter(originalGrainJitter);
                strip->setGrainRandomDepth(originalGrainRandom);
                strip->setGrainArpDepth(originalGrainArp);
                strip->setGrainCloudDepth(originalGrainCloud);
                strip->setGrainEmitterDepth(originalGrainEmitter);
                strip->setGrainEnvelope(originalGrainEnvelope);
            }
            else
            {
                seq.smoothedRaw = 0.0f;
                seq.grainDezipperedRaw = 0.0f;
                seq.pitchDezipperedRaw = 0.0f;
            }
        }
    };

    // SAMPLE-ACCURATE QUANTIZED EVENTS:
    // Split this block into segments around event sample offsets, so each trigger
    // is executed exactly at its target sample before rendering subsequent samples.
    auto eventsInBlock = positionInfo.getIsPlaying()
        ? quantizeClock.getEventsInRange(blockStart, blockEnd)
        : std::vector<QuantisedTrigger>{};

    // Mod-sequencer retrigger target: schedule sample-accurate PPQ-division events.
    if (positionInfo.getIsPlaying() && hasPpq && samplesPerBeat > 0.0)
    {
        const double blockStartPpq = basePpq;
        const double blockEndPpq = basePpq + (static_cast<double>(numSamples) / samplesPerBeat);

        auto readModStepValueAtPpq = [&](const ModSequencer& seq, double ppq) -> float
        {
            const int lengthBars = juce::jlimit(1, MaxModBars, seq.lengthBars.load(std::memory_order_acquire));
            const int totalSteps = juce::jmax(ModSteps, ModSteps * lengthBars);
            const int offset = seq.offset.load(std::memory_order_acquire);
            const double stepsPos = (ppq * 4.0) + static_cast<double>(offset);
            const double wrapped = std::fmod(stepsPos, static_cast<double>(totalSteps));
            const double wrappedPos = (wrapped < 0.0) ? (wrapped + static_cast<double>(totalSteps)) : wrapped;
            const int stepA = juce::jlimit(0, totalSteps - 1, static_cast<int>(std::floor(wrappedPos)));
            const int stepB = (stepA + 1) % totalSteps;
            const float rawA = juce::jlimit(0.0f, 1.0f, seq.steps[static_cast<size_t>(stepA)].load(std::memory_order_acquire));
            const float rawB = juce::jlimit(0.0f, 1.0f, seq.steps[static_cast<size_t>(stepB)].load(std::memory_order_acquire));
            if (seq.curveMode.load(std::memory_order_acquire) == 0)
                return rawA;

            const float phase = static_cast<float>(juce::jlimit(0.0, 1.0, wrappedPos - static_cast<double>(stepA)));
            const float bend = juce::jlimit(-1.0f, 1.0f, seq.curveBend.load(std::memory_order_acquire));
            const auto shape = static_cast<ModCurveShape>(juce::jlimit(
                0, static_cast<int>(ModCurveShape::Stair), seq.curveShape.load(std::memory_order_acquire)));
            const float shapedPhase = shapeModCurvePhase(phase, bend, shape);
            return juce::jlimit(0.0f, 1.0f, rawA + ((rawB - rawA) * shapedPhase));
        };

        for (int stripIdx = 0; stripIdx < MaxStrips; ++stripIdx)
        {
            auto* strip = strips[static_cast<size_t>(stripIdx)].get();
            if (strip == nullptr || !strip->isPlaying())
                continue;

            const auto& seq = modSequencers[static_cast<size_t>(stripIdx)];
            const auto target = static_cast<ModTarget>(seq.target.load(std::memory_order_acquire));
            if (target != ModTarget::Retrigger)
                continue;

            const float depth = juce::jlimit(0.0f, 1.0f, seq.depth.load(std::memory_order_acquire));
            if (depth <= 1.0e-4f)
                continue;

            const auto divisionAtPpq = [&](double ppq) -> double
            {
                const float stepValue = readModStepValueAtPpq(seq, ppq);
                return retriggerDivisionFromAmount(stepValue * depth);
            };

            double cursorPpq = blockStartPpq;
            int lastOffsetSamples = -1;
            int safety = 0;
            while (cursorPpq < blockEndPpq && safety++ < 256)
            {
                const double division = divisionAtPpq(cursorPpq);
                if (division <= 0.0)
                    break;

                double boundaryPpq = std::ceil((cursorPpq - 1.0e-12) / division) * division;
                if (boundaryPpq <= cursorPpq + 1.0e-12)
                    boundaryPpq += division;
                if (boundaryPpq >= blockEndPpq)
                    break;

                const int offsetSamples = juce::jlimit(
                    0, numSamples - 1, static_cast<int>(std::llround((boundaryPpq - blockStartPpq) * samplesPerBeat)));
                if (offsetSamples != lastOffsetSamples)
                {
                    QuantisedTrigger t;
                    t.targetSample = blockStart + offsetSamples;
                    t.targetPPQ = boundaryPpq;
                    t.stripIndex = stripIdx;
                    t.column = strip->getCurrentColumn();
                    t.clearPendingOnFire = false;
                    eventsInBlock.push_back(t);
                    lastOffsetSamples = offsetSamples;
                }

                cursorPpq = boundaryPpq + 1.0e-9;
            }
        }

        // Global momentary stutter (monome top-row hold):
        // schedule repeating retriggers at fixed PPQ divisions while held.
        if (momentaryStutterActive.load(std::memory_order_acquire) != 0)
        {
            const double divisionBeats = juce::jlimit(
                0.03125, 4.0, momentaryStutterDivisionBeats.load(std::memory_order_acquire));
            const double startPpq = momentaryStutterStartPpq.load(std::memory_order_acquire);

            if (blockEndPpq > startPpq)
            {
                for (int stripIdx = 0; stripIdx < MaxStrips; ++stripIdx)
                {
                    const auto idx = static_cast<size_t>(stripIdx);
                    if (momentaryStutterStripEnabled[idx].load(std::memory_order_acquire) == 0)
                        continue;

                    auto* strip = strips[idx].get();
                    if (strip == nullptr || !strip->isPlaying())
                        continue;

                    const int stutterColumn = juce::jlimit(
                        0, MaxColumns - 1, momentaryStutterColumns[idx].load(std::memory_order_acquire));

                    double nextPpq = momentaryStutterNextPpq[idx].load(std::memory_order_acquire);
                    if (!std::isfinite(nextPpq))
                        nextPpq = startPpq;
                    if (nextPpq < startPpq)
                        nextPpq = startPpq;

                    // Catch up to current block without drifting trigger phase.
                    if (nextPpq < blockStartPpq - 1.0e-9)
                    {
                        const double delta = blockStartPpq - nextPpq;
                        if (delta > divisionBeats)
                        {
                            const double steps = std::floor(delta / divisionBeats);
                            nextPpq += steps * divisionBeats;
                        }
                        while (nextPpq < blockStartPpq - 1.0e-9)
                            nextPpq += divisionBeats;
                    }

                    int lastOffsetSamples = -1;
                    int safety = 0;
                    while (nextPpq < blockEndPpq && safety++ < 1024)
                    {
                        const int offsetSamples = juce::jlimit(
                            0, numSamples - 1, static_cast<int>(std::llround((nextPpq - blockStartPpq) * samplesPerBeat)));
                        if (offsetSamples != lastOffsetSamples)
                        {
                            QuantisedTrigger t;
                            t.targetSample = blockStart + offsetSamples;
                            t.targetPPQ = nextPpq;
                            t.stripIndex = stripIdx;
                            t.column = stutterColumn;
                            t.clearPendingOnFire = false;
                            eventsInBlock.push_back(t);
                            lastOffsetSamples = offsetSamples;
                        }

                        nextPpq += divisionBeats;
                    }

                    momentaryStutterNextPpq[idx].store(nextPpq, std::memory_order_release);
                }
            }
        }

        // Stutter-lock: while hold-stutter is active on a strip, discard any
        // competing trigger events that target a different column.
        if (momentaryStutterActive.load(std::memory_order_acquire) != 0)
        {
            eventsInBlock.erase(
                std::remove_if(eventsInBlock.begin(), eventsInBlock.end(),
                    [this](const QuantisedTrigger& t)
                    {
                        if (t.stripIndex < 0 || t.stripIndex >= MaxStrips)
                            return false;
                        if (momentaryStutterStripEnabled[static_cast<size_t>(t.stripIndex)].load(std::memory_order_acquire) == 0)
                            return false;
                        const int stutterColumn = juce::jlimit(
                            0, MaxColumns - 1, momentaryStutterColumns[static_cast<size_t>(t.stripIndex)].load(std::memory_order_acquire));
                        return t.column != stutterColumn;
                    }),
                eventsInBlock.end());
        }

        std::sort(eventsInBlock.begin(), eventsInBlock.end(),
                  [](const QuantisedTrigger& a, const QuantisedTrigger& b)
                  {
                      if (a.targetSample != b.targetSample)
                          return a.targetSample < b.targetSample;
                      return a.stripIndex < b.stripIndex;
                  });
    }

    int processedSamples = 0;
    size_t eventIndex = 0;

    while (eventIndex < eventsInBlock.size())
    {
        const auto eventOffset = juce::jlimit(0, numSamples,
            static_cast<int>(eventsInBlock[eventIndex].targetSample - blockStart));

        // Render up to the event boundary first
        if (eventOffset > processedSamples)
        {
            processStripsSegment(processedSamples, eventOffset - processedSamples);
            processedSamples = eventOffset;
        }

        // Fire all events that land on this exact sample offset
        while (eventIndex < eventsInBlock.size())
        {
            const auto& event = eventsInBlock[eventIndex];
            const auto currentOffset = juce::jlimit(0, numSamples,
                static_cast<int>(event.targetSample - blockStart));
            if (currentOffset != eventOffset)
                break;

            if (auto* strip = getStrip(event.stripIndex))
            {
                enforceGroupExclusivity(event.stripIndex, false);

                auto triggerPosInfo = makeSegmentPositionInfo(eventOffset);
                if (hasPpq)
                {
                    // Use the scheduled grid PPQ so column jumps are deterministic.
                    triggerPosInfo.setPpqPosition(event.targetPPQ);
                }

                const int64_t triggerSample = blockStart + eventOffset;
                strip->triggerAtSample(event.column, tempoNow, triggerSample, triggerPosInfo);
                if (event.clearPendingOnFire)
                    quantizeClock.clearPendingTriggersForStrip(event.stripIndex);
            }

            ++eventIndex;
        }
    }

    // Render remaining tail after the last event
    processStripsSegment(processedSamples, numSamples - processedSamples);

    std::array<juce::AudioBuffer<float>*, MaxStrips + 1> buffersToPostProcess{};
    size_t buffersToPostProcessCount = 0;
    auto pushUniqueBuffer = [&](juce::AudioBuffer<float>* candidate)
    {
        if (candidate == nullptr || candidate->getNumChannels() <= 0)
            return;
        for (size_t i = 0; i < buffersToPostProcessCount; ++i)
        {
            if (buffersToPostProcess[i] == candidate)
                return;
        }
        buffersToPostProcess[buffersToPostProcessCount++] = candidate;
    };

    pushUniqueBuffer(&buffer);
    if (stripOutputs != nullptr)
    {
        for (size_t i = 0; i < static_cast<size_t>(MaxStrips); ++i)
            pushUniqueBuffer((*stripOutputs)[i]);
    }

    // Protect downstream audio path from a single invalid strip sample.
    for (size_t b = 0; b < buffersToPostProcessCount; ++b)
    {
        auto* target = buffersToPostProcess[b];
        const int targetChannels = target->getNumChannels();
        const int targetSamples = target->getNumSamples();
        for (int ch = 0; ch < targetChannels; ++ch)
        {
            auto* write = target->getWritePointer(ch);
            for (int i = 0; i < targetSamples; ++i)
            {
                if (!std::isfinite(write[i]))
                    write[i] = 0.0f;
            }
        }

        // Apply master volume to all active output destinations.
        target->applyGain(masterVolume.load());
    }
    
    // Mix in input monitoring if enabled
    if (inputMonitorVol > 0.0f && inputCopy.getNumChannels() > 0 && inputCopy.getNumSamples() > 0)
    {
        // Handle potential channel count mismatch (e.g., mono input to stereo output)
        int channelsToMix = juce::jmin(numChannels, inputCopy.getNumChannels());
        for (int ch = 0; ch < channelsToMix; ++ch)
            buffer.addFrom(ch, 0, inputCopy, ch, 0, numSamples, inputMonitorVol);
        
        // If input is mono and output is stereo, duplicate to both channels
        if (inputCopy.getNumChannels() == 1 && numChannels == 2)
            buffer.addFrom(1, 0, inputCopy, 0, 0, numSamples, inputMonitorVol);
    }
    
    // Only advance clocks when host is playing
    // This keeps everything locked to host timeline
    bool hostIsPlaying = positionInfo.getIsPlaying();
    
    if (hostIsPlaying)
    {
        // Advance global sample counter for sample-accurate sync
        globalSampleCount = globalSampleCount.load() + numSamples;
        
        // Update quantize clock from master PPQ (not sample counting!)
        if (positionInfo.getPpqPosition().hasValue())
        {
            double ppq = *positionInfo.getPpqPosition();
            hasLastKnownPPQ.store(true, std::memory_order_release);
            lastKnownPPQ.store(ppq);  // Store for use outside audio thread
            quantizeClock.updateFromPPQ(ppq, numSamples);
        }
        advanceBeat(numSamples, hasPpq);
    }
    // When stopped: clocks freeze, PPQ sync in updateTempo keeps us locked
    
    // Process patterns
    processPatterns();
}

EnhancedAudioStrip* ModernAudioEngine::getStrip(int index)
{
    if (index >= 0 && index < MaxStrips)
        return strips[static_cast<size_t>(index)].get();
    return nullptr;
}

bool ModernAudioEngine::loadSampleToStrip(int stripIndex, const juce::File& file)
{
    if (auto* strip = getStrip(stripIndex))
    {
        if (!strip->loadSampleFromFile(file) || !strip->hasAudio())
            return false;

        const auto* loadedBuffer = strip->getAudioBuffer();
        const double sourceRate = strip->getSourceSampleRate();
        if (loadedBuffer == nullptr || loadedBuffer->getNumSamples() <= 0 || sourceRate <= 0.0)
            return false;

        const double hostTempoNow = juce::jlimit(20.0, 320.0, currentTempo.load());
        const double sampleSeconds = static_cast<double>(loadedBuffer->getNumSamples()) / sourceRate;
        // Simple 4/4 detection: bars = seconds * BPM / (60 * 4).
        const double estimatedBars = (sampleSeconds * hostTempoNow) / 240.0;
        // Snap directly to nearest supported bar count (1/2/4/8).
        // This avoids round-then-bucket misclassification (e.g. ~2.6 bars -> 4).
        int detectedBars = 1;
        {
            static constexpr int supportedBars[] = {1, 2, 4, 8};
            double bestDistance = std::numeric_limits<double>::max();
            for (int candidate : supportedBars)
            {
                const double d = std::abs(estimatedBars - static_cast<double>(candidate));
                if (d < bestDistance)
                {
                    bestDistance = d;
                    detectedBars = candidate;
                }
            }
        }

        DBG("Bar detect strip " << stripIndex
            << " hostBpm=" << hostTempoNow
            << " frames=" << loadedBuffer->getNumSamples()
            << " srcRate=" << sourceRate
            << " durSec=" << sampleSeconds
            << " barsExact=" << estimatedBars
            << " barsDetected=" << detectedBars);

        // Strict PPQ safety:
        // - If not playing, apply detected mapping immediately.
        // - If playing, only apply when PPQ-anchor remap is provably safe.
        if (!strip->isPlaying())
        {
            strip->setRecordingBars(detectedBars);
            strip->setBeatsPerLoop(static_cast<float>(detectedBars * 4));
        }
        else
        {
            const double hostPpqNow = getTimelineBeat();
            if (strip->isPpqTimelineAnchored() && std::isfinite(hostPpqNow))
            {
                strip->setRecordingBars(detectedBars);
                strip->setBeatsPerLoopAtPpq(static_cast<float>(detectedBars * 4), hostPpqNow);
            }
            else
            {
                DBG("Skipped live detected bar remap on strip " << stripIndex
                    << " because PPQ anchor is not stable; retry when timeline is anchored.");
            }
        }

        return true;
    }

    return false;
}

StripGroup* ModernAudioEngine::getGroup(int index)
{
    if (index >= 0 && index < MaxGroups)
        return groups[static_cast<size_t>(index)].get();
    return nullptr;
}

void ModernAudioEngine::assignStripToGroup(int stripIndex, int groupIndex)
{
    if (auto* strip = getStrip(stripIndex))
    {
        // Remove from old group
        int oldGroup = strip->getGroup();
        if (oldGroup >= 0 && oldGroup < MaxGroups && groups[static_cast<size_t>(oldGroup)])
        {
            groups[static_cast<size_t>(oldGroup)]->removeStrip(stripIndex);
        }
        
        // Add to new group (or set to none if groupIndex < 0)
        if (groupIndex >= 0 && groupIndex < MaxGroups && groups[static_cast<size_t>(groupIndex)])
        {
            groups[static_cast<size_t>(groupIndex)]->addStrip(stripIndex);
            strip->setGroup(groupIndex);
        }
        else
        {
            // No group - set to -1
            strip->setGroup(-1);
        }
    }
}

void ModernAudioEngine::enforceGroupExclusivity(int activeStripIndex, bool immediateStop)
{
    auto* activeStrip = getStrip(activeStripIndex);
    if (!activeStrip)
        return;

    const int groupId = activeStrip->getGroup();
    if (groupId < 0 || groupId >= MaxGroups || !groups[static_cast<size_t>(groupId)])
        return;

    auto* group = groups[static_cast<size_t>(groupId)].get();
    if (group->isMuted())
        group->setMuted(false);

    // Keep group membership coherent for dynamic reassignment paths (record/preset restore).
    if (!group->containsStrip(activeStripIndex))
        group->addStrip(activeStripIndex);

    const auto& stripList = group->getStrips();
    for (int otherStripIndex : stripList)
    {
        if (otherStripIndex == activeStripIndex)
            continue;

        if (auto* otherStrip = getStrip(otherStripIndex))
            otherStrip->stop(immediateStop);
    }
}

void ModernAudioEngine::setQuantization(int division)
{
    quantizeClock.setQuantization(division);
}

void ModernAudioEngine::setMomentaryStutterActive(bool enabled)
{
    momentaryStutterActive.store(enabled ? 1 : 0, std::memory_order_release);
}

void ModernAudioEngine::setMomentaryStutterDivision(double beats)
{
    momentaryStutterDivisionBeats.store(juce::jlimit(0.03125, 4.0, beats), std::memory_order_release);
}

void ModernAudioEngine::setMomentaryStutterStartPpq(double ppq)
{
    momentaryStutterStartPpq.store(ppq, std::memory_order_release);
}

void ModernAudioEngine::setMomentaryStutterStrip(int stripIndex, int column, bool enabled)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;

    const auto idx = static_cast<size_t>(stripIndex);
    momentaryStutterColumns[idx].store(
        juce::jlimit(0, MaxColumns - 1, column), std::memory_order_release);

    const int wasEnabled = momentaryStutterStripEnabled[idx].load(std::memory_order_acquire);
    momentaryStutterStripEnabled[idx].store(enabled ? 1 : 0, std::memory_order_release);
    if (enabled && wasEnabled == 0)
        momentaryStutterNextPpq[idx].store(momentaryStutterStartPpq.load(std::memory_order_acquire), std::memory_order_release);
    if (!enabled)
        momentaryStutterNextPpq[idx].store(0.0, std::memory_order_release);
}

void ModernAudioEngine::clearMomentaryStutterStrips()
{
    for (int i = 0; i < MaxStrips; ++i)
    {
        momentaryStutterStripEnabled[static_cast<size_t>(i)].store(0, std::memory_order_release);
        momentaryStutterNextPpq[static_cast<size_t>(i)].store(0.0, std::memory_order_release);
    }
}

void ModernAudioEngine::scheduleQuantizedTrigger(int stripIndex, int column, double currentPPQ)
{
    // Use provided PPQ or fall back to last known
    double ppq = (currentPPQ > 0.0) ? currentPPQ : lastKnownPPQ.load();
    auto* strip = getStrip(stripIndex);
    quantizeClock.scheduleTrigger(stripIndex, column, ppq, strip);
}

void ModernAudioEngine::triggerStripWithQuantization(int stripIndex, int column, bool useQuantize)
{
    if (stripIndex >= 0 && stripIndex < MaxStrips
        && momentaryStutterActive.load(std::memory_order_acquire) != 0
        && momentaryStutterStripEnabled[static_cast<size_t>(stripIndex)].load(std::memory_order_acquire) != 0)
    {
        // Hold-stutter owns retrigger timing for this strip; ignore external triggers
        // so playback cannot jump out of the stutter loop.
        return;
    }

    auto* strip = getStrip(stripIndex);
    if (!strip)
        return;
    
    if (useQuantize)
    {
        // Schedule for next quantize point (use lastKnownPPQ)
        scheduleQuantizedTrigger(stripIndex, column, 0.0);
    }
    else
    {
        // Immediate trigger - handle group choke with short fade to avoid clicks.
        enforceGroupExclusivity(stripIndex, false);
        
        // Trigger immediately with current PPQ/sample so timeline anchor can be built.
        juce::AudioPlayHead::PositionInfo immediatePosInfo;
        immediatePosInfo.setPpqPosition(lastKnownPPQ.load());
        strip->triggerAtSample(column, currentTempo.load(), globalSampleCount.load(), immediatePosInfo);
    }
}

void ModernAudioEngine::setMasterVolume(float vol)
{
    masterVolume = juce::jlimit(0.0f, 1.0f, vol);
}

void ModernAudioEngine::setPitchSmoothingTime(float seconds)
{
    pitchSmoothingTime = juce::jlimit(0.0f, 1.0f, seconds);
    
    // Update all strips with new smoothing time
    for (auto& strip : strips)
    {
        if (strip)
            strip->setPitchSmoothingTime(seconds);
    }
}

void ModernAudioEngine::setInputMonitorVolume(float vol)
{
    inputMonitorVolume = juce::jlimit(0.0f, 1.0f, vol);
}

void ModernAudioEngine::setCrossfadeLengthMs(float ms)
{
    const float clampedMs = juce::jlimit(1.0f, 50.0f, ms);
    crossfadeLengthMs.store(clampedMs, std::memory_order_release);

    if (liveRecorder)
        liveRecorder->setCrossfadeLengthMs(clampedMs);

    for (auto& strip : strips)
    {
        if (strip)
            strip->setLoopCrossfadeLengthMs(clampedMs);
    }
}

void ModernAudioEngine::setTriggerFadeInMs(float ms)
{
    const float clampedMs = juce::jlimit(0.1f, 120.0f, ms);
    triggerFadeInMs.store(clampedMs, std::memory_order_release);

    for (auto& strip : strips)
    {
        if (strip)
            strip->setTriggerFadeInMs(clampedMs);
    }
}

void ModernAudioEngine::setGlobalSwingDivision(EnhancedAudioStrip::SwingDivision division)
{
    for (auto& strip : strips)
    {
        if (strip)
            strip->setSwingDivision(division);
    }
}

EnhancedAudioStrip::SwingDivision ModernAudioEngine::getGlobalSwingDivision() const
{
    for (const auto& strip : strips)
    {
        if (strip)
            return strip->getSwingDivision();
    }
    return EnhancedAudioStrip::SwingDivision::Eighth;
}

void ModernAudioEngine::updateTempo(const juce::AudioPlayHead::PositionInfo& positionInfo)
{
    if (positionInfo.getBpm().hasValue())
    {
        double hostTempo = *positionInfo.getBpm();
        if (std::abs(hostTempo - currentTempo.load()) > 1.0e-6)
        {
            currentTempo = hostTempo;
            quantizeClock.setTempo(hostTempo);
        }
        
        // ALWAYS sync to host timeline position when available
        // This ensures perfect lock even after transport stop/start
        if (positionInfo.getPpqPosition().hasValue())
        {
            double hostPpq = *positionInfo.getPpqPosition();
            hasLastKnownPPQ.store(true, std::memory_order_release);
            
            // Direct lock to host PPQ - no accumulation, no drift
            currentBeat = hostPpq;
            
            // Calculate beat phase (0.0 to 1.0 within current beat)
            double wholeBeat = std::floor(hostPpq);
            beatPhase = hostPpq - wholeBeat;
        }

        if (positionInfo.getTimeSignature().hasValue())
        {
            const auto ts = *positionInfo.getTimeSignature();
            currentTimeSigNumerator.store(juce::jlimit(1, 32, ts.numerator), std::memory_order_release);
            currentTimeSigDenominator.store(juce::jlimit(1, 32, ts.denominator), std::memory_order_release);
        }
    }
}

double ModernAudioEngine::getTimelineBeat() const
{
    if (hasLastKnownPPQ.load(std::memory_order_acquire))
        return lastKnownPPQ.load(std::memory_order_acquire);

    return currentBeat.load(std::memory_order_acquire);
}

void ModernAudioEngine::advanceBeat(int numSamples, bool hasHostPpq)
{
    // If PPQ is present, updateTempo() already hard-locks currentBeat.
    if (hasHostPpq)
        return;
    
    double beatsPerSample = (currentTempo.load() / 60.0) / currentSampleRate;
    double beatAdvance = beatsPerSample * numSamples;
    
    currentBeat = currentBeat.load() + beatAdvance;
    
    // Track phase within beat (0.0 to 1.0)
    double newPhase = beatPhase.load() + beatAdvance;
    while (newPhase >= 1.0)
        newPhase -= 1.0;
    beatPhase = newPhase;
}

void ModernAudioEngine::processPatterns()
{
    const double currentBeatPos = currentBeat.load();
    if (!std::isfinite(currentBeatPos))
        return;

    if (lastPatternProcessBeat < 0.0 || !std::isfinite(lastPatternProcessBeat))
        lastPatternProcessBeat = currentBeatPos;

    const double previousBeatPos = lastPatternProcessBeat;
    const double beatDelta = currentBeatPos - previousBeatPos;
    const bool discontinuity = (beatDelta <= 0.0) || (beatDelta > 1.0);
    
    // Update all patterns
    for (auto& pattern : patterns)
    {
        if (pattern)
        {
            // Check if recording should auto-stop
            pattern->updateRecording(currentBeatPos);
            
            // Process playback - each pattern tracks its own position
            if (pattern->isPlaying())
            {
                if (discontinuity)
                    continue;

                // Process events against absolute PPQ/beat timeline window
                pattern->processEventsForBeatWindow(previousBeatPos, currentBeatPos,
                    [this](const PatternRecorder::Event& event)
                    {
                        if (event.isNoteOn)
                        {
                            // Use the same trigger path as live grid presses so pattern playback
                            // stays on the PPQ/sample timeline and respects group behavior.
                            triggerStripWithQuantization(event.stripIndex, event.column, false);
                        }
                        else if (auto* strip = getStrip(event.stripIndex))
                        {
                            strip->stop(false);
                        }
                    });
            }
        }
    }
    
    lastPatternProcessBeat = currentBeatPos;
}

PatternRecorder* ModernAudioEngine::getPattern(int index)
{
    if (index >= 0 && index < 4)
        return patterns[static_cast<size_t>(index)].get();
    return nullptr;
}

bool ModernAudioEngine::modTargetSupportsBipolar(ModTarget target)
{
    switch (target)
    {
        case ModTarget::None:
            return false;
        case ModTarget::Volume:
            return false;
        case ModTarget::Pan:
            return true;
        case ModTarget::Pitch:
            return true;
        case ModTarget::Speed:
            return false;
        case ModTarget::Cutoff:
            return false;
        case ModTarget::Resonance:
            return false;
        case ModTarget::GrainSize:
            return true;
        case ModTarget::GrainDensity:
            return true;
        case ModTarget::GrainPitch:
            return true;
        case ModTarget::GrainPitchJitter:
            return false;
        case ModTarget::GrainSpread:
            return true;
        case ModTarget::GrainJitter:
            return false;
        case ModTarget::GrainRandom:
            return false;
        case ModTarget::GrainArp:
            return false;
        case ModTarget::GrainCloud:
            return false;
        case ModTarget::GrainEmitter:
            return false;
        case ModTarget::GrainEnvelope:
            return false;
        case ModTarget::Retrigger:
            return false;
    }

    return false;
}

ModernAudioEngine::ModSequencerState ModernAudioEngine::getModSequencerState(int stripIndex) const
{
    ModSequencerState state{};
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return state;

    const auto& seq = modSequencers[static_cast<size_t>(stripIndex)];
    state.target = static_cast<ModTarget>(seq.target.load(std::memory_order_acquire));
    state.bipolar = modTargetSupportsBipolar(state.target) && (seq.bipolar.load(std::memory_order_acquire) != 0);
    state.curveMode = (seq.curveMode.load(std::memory_order_acquire) != 0);
    state.depth = seq.depth.load(std::memory_order_acquire);
    state.offset = seq.offset.load(std::memory_order_acquire);
    state.lengthBars = juce::jlimit(1, MaxModBars, seq.lengthBars.load(std::memory_order_acquire));
    state.editPage = juce::jlimit(0, MaxModBars - 1, seq.editPage.load(std::memory_order_acquire));
    state.smoothingMs = juce::jmax(0.0f, seq.smoothingMs.load(std::memory_order_acquire));
    state.curveBend = juce::jlimit(-1.0f, 1.0f, seq.curveBend.load(std::memory_order_acquire));
    state.curveShape = juce::jlimit(0, static_cast<int>(ModCurveShape::Stair), seq.curveShape.load(std::memory_order_acquire));
    state.pitchScaleQuantize = (seq.pitchScaleQuantize.load(std::memory_order_acquire) != 0);
    state.pitchScale = juce::jlimit(0, static_cast<int>(PitchScale::PentatonicMinor), seq.pitchScale.load(std::memory_order_acquire));
    const int pageOffset = state.editPage * ModSteps;
    for (int i = 0; i < ModSteps; ++i)
        state.steps[static_cast<size_t>(i)] = seq.steps[static_cast<size_t>(pageOffset + i)].load(std::memory_order_acquire);
    return state;
}

void ModernAudioEngine::setModTarget(int stripIndex, ModTarget target)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;
    auto& seq = modSequencers[static_cast<size_t>(stripIndex)];
    seq.target.store(static_cast<int>(target), std::memory_order_release);
    seq.bipolar.store(modTargetAutoDefaultBipolar(target) ? 1 : 0, std::memory_order_release);

    if (target == ModTarget::Pitch && seq.smoothingMs.load(std::memory_order_acquire) < 1.0f)
        seq.smoothingMs.store(12.0f, std::memory_order_release);
    else if (target == ModTarget::Speed && seq.smoothingMs.load(std::memory_order_acquire) < 1.0f)
        seq.smoothingMs.store(20.0f, std::memory_order_release);
}

ModernAudioEngine::ModTarget ModernAudioEngine::getModTarget(int stripIndex) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return ModTarget::None;
    return static_cast<ModTarget>(modSequencers[static_cast<size_t>(stripIndex)].target.load(std::memory_order_acquire));
}

void ModernAudioEngine::setModBipolar(int stripIndex, bool bipolar)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;
    auto& seq = modSequencers[static_cast<size_t>(stripIndex)];
    const auto target = static_cast<ModTarget>(seq.target.load(std::memory_order_acquire));
    seq.bipolar.store((bipolar && modTargetSupportsBipolar(target)) ? 1 : 0, std::memory_order_release);
    for (auto& step : seq.steps)
    {
        const float clamped = juce::jlimit(0.0f, 1.0f, step.load(std::memory_order_acquire));
        step.store(clamped, std::memory_order_release);
    }
}

bool ModernAudioEngine::isModBipolar(int stripIndex) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return false;
    return modSequencers[static_cast<size_t>(stripIndex)].bipolar.load(std::memory_order_acquire) != 0;
}

void ModernAudioEngine::setModDepth(int stripIndex, float depth)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;
    modSequencers[static_cast<size_t>(stripIndex)].depth.store(juce::jlimit(0.0f, 1.0f, depth), std::memory_order_release);
}

float ModernAudioEngine::getModDepth(int stripIndex) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return 1.0f;
    return modSequencers[static_cast<size_t>(stripIndex)].depth.load(std::memory_order_acquire);
}

void ModernAudioEngine::setModCurveMode(int stripIndex, bool curveMode)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;
    modSequencers[static_cast<size_t>(stripIndex)].curveMode.store(curveMode ? 1 : 0, std::memory_order_release);
}

bool ModernAudioEngine::isModCurveMode(int stripIndex) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return true;
    return modSequencers[static_cast<size_t>(stripIndex)].curveMode.load(std::memory_order_acquire) != 0;
}

void ModernAudioEngine::setModOffset(int stripIndex, int offset)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;
    modSequencers[static_cast<size_t>(stripIndex)].offset.store(juce::jlimit(-(ModTotalSteps - 1), ModTotalSteps - 1, offset), std::memory_order_release);
}

int ModernAudioEngine::getModOffset(int stripIndex) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return 0;
    return modSequencers[static_cast<size_t>(stripIndex)].offset.load(std::memory_order_acquire);
}

void ModernAudioEngine::setModStepValue(int stripIndex, int step, float value01)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips || step < 0 || step >= ModSteps)
        return;
    auto& seq = modSequencers[static_cast<size_t>(stripIndex)];
    const int page = juce::jlimit(0, MaxModBars - 1, seq.editPage.load(std::memory_order_acquire));
    const int idx = (page * ModSteps) + step;
    seq.steps[static_cast<size_t>(idx)].store(
        juce::jlimit(0.0f, 1.0f, value01), std::memory_order_release);
}

void ModernAudioEngine::setModStepValueAbsolute(int stripIndex, int absoluteStep, float value01)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips || absoluteStep < 0 || absoluteStep >= ModTotalSteps)
        return;
    modSequencers[static_cast<size_t>(stripIndex)].steps[static_cast<size_t>(absoluteStep)].store(
        juce::jlimit(0.0f, 1.0f, value01), std::memory_order_release);
}

float ModernAudioEngine::getModStepValue(int stripIndex, int step) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips || step < 0 || step >= ModSteps)
        return 0.0f;
    const auto& seq = modSequencers[static_cast<size_t>(stripIndex)];
    const int page = juce::jlimit(0, MaxModBars - 1, seq.editPage.load(std::memory_order_acquire));
    const int idx = (page * ModSteps) + step;
    return seq.steps[static_cast<size_t>(idx)].load(std::memory_order_acquire);
}

float ModernAudioEngine::getModStepValueAbsolute(int stripIndex, int absoluteStep) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips || absoluteStep < 0 || absoluteStep >= ModTotalSteps)
        return 0.0f;
    return modSequencers[static_cast<size_t>(stripIndex)].steps[static_cast<size_t>(absoluteStep)].load(std::memory_order_acquire);
}

void ModernAudioEngine::toggleModStep(int stripIndex, int step)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips || step < 0 || step >= ModSteps)
        return;
    auto& seq = modSequencers[static_cast<size_t>(stripIndex)];
    const int page = juce::jlimit(0, MaxModBars - 1, seq.editPage.load(std::memory_order_acquire));
    const int idx = (page * ModSteps) + step;
    auto& cell = seq.steps[static_cast<size_t>(idx)];
    const float prev = cell.load(std::memory_order_acquire);
    cell.store(prev >= 0.5f ? 0.0f : 1.0f, std::memory_order_release);
}

void ModernAudioEngine::clearModSteps(int stripIndex)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;
    for (auto& step : modSequencers[static_cast<size_t>(stripIndex)].steps)
        step.store(0.0f, std::memory_order_release);
}

int ModernAudioEngine::getModCurrentStep(int stripIndex) const
{
    const int globalStep = getModCurrentGlobalStep(stripIndex);
    return globalStep % ModSteps;
}

int ModernAudioEngine::getModCurrentPage(int stripIndex) const
{
    const int globalStep = getModCurrentGlobalStep(stripIndex);
    return juce::jlimit(0, MaxModBars - 1, globalStep / ModSteps);
}

int ModernAudioEngine::getModCurrentGlobalStep(int stripIndex) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return 0;
    const auto& seq = modSequencers[static_cast<size_t>(stripIndex)];
    const int lengthBars = juce::jlimit(1, MaxModBars, seq.lengthBars.load(std::memory_order_acquire));
    const int totalSteps = juce::jmax(ModSteps, ModSteps * lengthBars);
    return juce::jlimit(0, totalSteps - 1, seq.lastGlobalStep.load(std::memory_order_acquire));
}

void ModernAudioEngine::setModLengthBars(int stripIndex, int bars)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;
    auto& seq = modSequencers[static_cast<size_t>(stripIndex)];
    int clampedBars = 1;
    if (bars >= 8)
        clampedBars = 8;
    else if (bars >= 4)
        clampedBars = 4;
    else if (bars >= 2)
        clampedBars = 2;
    seq.lengthBars.store(clampedBars, std::memory_order_release);
    const int maxPage = clampedBars - 1;
    const int page = juce::jlimit(0, maxPage, seq.editPage.load(std::memory_order_acquire));
    seq.editPage.store(page, std::memory_order_release);
}

int ModernAudioEngine::getModLengthBars(int stripIndex) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return 1;
    return juce::jlimit(1, MaxModBars, modSequencers[static_cast<size_t>(stripIndex)].lengthBars.load(std::memory_order_acquire));
}

void ModernAudioEngine::setModEditPage(int stripIndex, int page)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;
    auto& seq = modSequencers[static_cast<size_t>(stripIndex)];
    const int maxPage = juce::jmax(0, getModLengthBars(stripIndex) - 1);
    seq.editPage.store(juce::jlimit(0, maxPage, page), std::memory_order_release);
}

int ModernAudioEngine::getModEditPage(int stripIndex) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return 0;
    const auto& seq = modSequencers[static_cast<size_t>(stripIndex)];
    const int maxPage = juce::jmax(0, getModLengthBars(stripIndex) - 1);
    return juce::jlimit(0, maxPage, seq.editPage.load(std::memory_order_acquire));
}

void ModernAudioEngine::setModSmoothingMs(int stripIndex, float ms)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;
    modSequencers[static_cast<size_t>(stripIndex)].smoothingMs.store(juce::jlimit(0.0f, 250.0f, ms), std::memory_order_release);
}

float ModernAudioEngine::getModSmoothingMs(int stripIndex) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return 0.0f;
    return juce::jlimit(0.0f, 250.0f, modSequencers[static_cast<size_t>(stripIndex)].smoothingMs.load(std::memory_order_acquire));
}

void ModernAudioEngine::setModCurveBend(int stripIndex, float bend)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;
    modSequencers[static_cast<size_t>(stripIndex)].curveBend.store(juce::jlimit(-1.0f, 1.0f, bend), std::memory_order_release);
}

float ModernAudioEngine::getModCurveBend(int stripIndex) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return 0.0f;
    return juce::jlimit(-1.0f, 1.0f, modSequencers[static_cast<size_t>(stripIndex)].curveBend.load(std::memory_order_acquire));
}

void ModernAudioEngine::setModCurveShape(int stripIndex, ModCurveShape shape)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;
    modSequencers[static_cast<size_t>(stripIndex)].curveShape.store(
        juce::jlimit(0, static_cast<int>(ModCurveShape::Stair), static_cast<int>(shape)),
        std::memory_order_release);
}

ModernAudioEngine::ModCurveShape ModernAudioEngine::getModCurveShape(int stripIndex) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return ModCurveShape::Power;
    return static_cast<ModCurveShape>(juce::jlimit(
        0, static_cast<int>(ModCurveShape::Stair),
        modSequencers[static_cast<size_t>(stripIndex)].curveShape.load(std::memory_order_acquire)));
}

void ModernAudioEngine::setModPitchScaleQuantize(int stripIndex, bool enabled)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;
    modSequencers[static_cast<size_t>(stripIndex)].pitchScaleQuantize.store(enabled ? 1 : 0, std::memory_order_release);
}

bool ModernAudioEngine::isModPitchScaleQuantize(int stripIndex) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return false;
    return modSequencers[static_cast<size_t>(stripIndex)].pitchScaleQuantize.load(std::memory_order_acquire) != 0;
}

void ModernAudioEngine::setModPitchScale(int stripIndex, PitchScale scale)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;
    modSequencers[static_cast<size_t>(stripIndex)].pitchScale.store(
        juce::jlimit(0, static_cast<int>(PitchScale::PentatonicMinor), static_cast<int>(scale)),
        std::memory_order_release);
}

ModernAudioEngine::PitchScale ModernAudioEngine::getModPitchScale(int stripIndex) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return PitchScale::Chromatic;
    const int idx = juce::jlimit(0, static_cast<int>(PitchScale::PentatonicMinor),
                                 modSequencers[static_cast<size_t>(stripIndex)].pitchScale.load(std::memory_order_acquire));
    return static_cast<PitchScale>(idx);
}

void ModernAudioEngine::startPatternRecording(int patternIndex)
{
    if (auto* pattern = getPattern(patternIndex))
    {
        // Always arm for max duration. Manual stop will quantize down to bars.
        pattern->setLength(32);  // 8 bars max (4 beats/bar)
        pattern->startRecording(getTimelineBeat());
    }
}

void ModernAudioEngine::stopPatternRecording(int patternIndex)
{
    if (auto* pattern = getPattern(patternIndex))
    {
        if (!pattern->isRecording())
            return;

        // Quantize recorded duration to whole bars (1..8 bars) on stop.
        const double stopBeat = getTimelineBeat();
        const double startBeat = pattern->getRecordingStartBeat();
        const double recordedBeats = juce::jmax(0.0, stopBeat - startBeat);
        int bars = static_cast<int>(std::round(recordedBeats / 4.0));
        bars = juce::jlimit(1, 8, bars);
        const int quantizedBeats = bars * 4;

        pattern->setLength(quantizedBeats);
        pattern->stopRecording();
        pattern->startPlayback(stopBeat);
    }
}

void ModernAudioEngine::playPattern(int patternIndex)
{
    if (auto* pattern = getPattern(patternIndex))
    {
        pattern->startPlayback(getTimelineBeat());
    }
}

void ModernAudioEngine::stopPattern(int patternIndex)
{
    if (auto* pattern = getPattern(patternIndex))
    {
        pattern->stopPlayback();
    }
}

void ModernAudioEngine::startLiveRecording(int /*stripIndex*/, int lengthBeats)
{
    liveRecorder->startRecording(lengthBeats, currentTempo.load());
}

void ModernAudioEngine::stopLiveRecording()
{
    liveRecorder->stopRecording();
    
    // Could auto-assign to a strip here if needed
}

void ModernAudioEngine::setRecordingLoopLength(int bars)
{
    if (liveRecorder)
        liveRecorder->setLoopLength(bars);
}

int ModernAudioEngine::getRecordingLoopLength() const
{
    if (liveRecorder)
        return liveRecorder->getSelectedLoopLength();
    return 1;
}

void ModernAudioEngine::captureLoopToStrip(int stripIndex, int bars)
{
    if (!liveRecorder || stripIndex < 0 || stripIndex >= MaxStrips)
        return;
    
    auto* strip = getStrip(stripIndex);
    if (!strip)
        return;
    
    // Capture the loop from circular buffer with specified bar length
    auto capturedBuffer = liveRecorder->captureLoop(currentTempo.load(), bars);
    
    // Validate buffer has audio
    if (capturedBuffer.getNumSamples() == 0)
        return;  // Empty capture
    
    // Load into strip (sample rate is current system sample rate)
    strip->loadSample(capturedBuffer, currentSampleRate);
    
    // Set the correct tempo: bars * 4 beats per bar
    float beatsPerLoop = static_cast<float>(bars * 4);
    strip->setBeatsPerLoop(beatsPerLoop);
    
    // Note: Trigger will be handled by processor with proper quantization
}

bool ModernAudioEngine::shouldBlinkRecordLED() const
{
    if (!liveRecorder)
        return false;
    
    // Get current beat position for blinking
    double beatPos = currentBeat.load();
    return liveRecorder->shouldBlinkRecordLED(beatPos);
}

void ModernAudioEngine::clearRecentInputBuffer()
{
    if (liveRecorder)
        liveRecorder->clearBuffer();
}

void ModernAudioEngine::stopPatternPlayback(int patternIndex)
{
    if (patternIndex < 0 || patternIndex >= MaxPatterns)
        return;
    
    if (patterns[static_cast<size_t>(patternIndex)])
        patterns[static_cast<size_t>(patternIndex)]->stop();
}

void ModernAudioEngine::clearPattern(int patternIndex)
{
    if (patternIndex < 0 || patternIndex >= MaxPatterns)
        return;

    if (patterns[static_cast<size_t>(patternIndex)])
        patterns[static_cast<size_t>(patternIndex)]->clear();
}

void EnhancedAudioStrip::setStepPatternBars(int bars)
{
    const int clampedBars = juce::jlimit(1, 4, bars);
    stepPatternBars.store(clampedBars, std::memory_order_release);

    const int totalSteps = clampedBars * 16;
    for (int i = totalSteps; i < static_cast<int>(stepPattern.size()); ++i)
        stepPattern[static_cast<size_t>(i)] = false;

    if (currentStep >= totalSteps)
        currentStep = 0;

    const int maxPage = juce::jmax(0, clampedBars - 1);
    if (stepViewPage.load(std::memory_order_acquire) > maxPage)
        stepViewPage.store(maxPage, std::memory_order_release);
}

void EnhancedAudioStrip::setStepPage(int page)
{
    const int maxPage = juce::jmax(0, getStepPatternBars() - 1);
    stepViewPage.store(juce::jlimit(0, maxPage, page), std::memory_order_release);
}

void EnhancedAudioStrip::toggleStepAtVisibleColumn(int column)
{
    const int visibleColumn = juce::jlimit(0, 15, column);
    const int absoluteStep = getVisibleStepOffset() + visibleColumn;
    if (absoluteStep < getStepTotalSteps())
        stepPattern[static_cast<size_t>(absoluteStep)] = !stepPattern[static_cast<size_t>(absoluteStep)];
}

void EnhancedAudioStrip::toggleStepAtIndex(int absoluteStep)
{
    const int clamped = juce::jlimit(0, getStepTotalSteps() - 1, absoluteStep);
    stepPattern[static_cast<size_t>(clamped)] = !stepPattern[static_cast<size_t>(clamped)];
}

std::array<bool, 16> EnhancedAudioStrip::getVisibleStepPattern() const
{
    std::array<bool, 16> visible = {};
    int page = stepViewPage.load(std::memory_order_acquire);
    int offset = page * 16;

    for (int i = 0; i < 16; ++i)
    {
        if (offset + i < 64)
            visible[static_cast<size_t>(i)] = stepPattern[static_cast<size_t>(offset + i)];
    }

    return visible;
}

int EnhancedAudioStrip::getVisibleCurrentStep() const
{
    int page = stepViewPage.load(std::memory_order_acquire);
    int offset = page * 16;
    int current = currentStep;

    // Return step relative to current page (0-15), or -1 if not on current page
    if (current >= offset && current < offset + 16)
        return current - offset;

    return -1;
}

int EnhancedAudioStrip::getVisibleStepOffset() const
{
    return stepViewPage.load(std::memory_order_acquire) * 16;
}
