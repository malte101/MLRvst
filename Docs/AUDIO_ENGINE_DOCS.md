# Modern Audio Engine Documentation

## Overview

The modernized mlrVST audio engine brings state-of-the-art audio processing to the classic mlr workflow. Built entirely with JUCE 8.x, it features high-quality resampling, flexible playback modes, tempo sync, quantization, pattern recording, and live input capture.

## Architecture

### Core Components

#### 1. **Resampler**
High-quality sample rate conversion with three quality modes:

- **Linear**: Fast, suitable for realtime with minimal CPU
- **Cubic**: Better quality with Hermite interpolation
- **Sinc**: Highest quality with windowed sinc interpolation

```cpp
Resampler resampler;
resampler.setQuality(Resampler::Quality::Cubic);
float sample = resampler.getSample(buffer, channel, position, speed);
```

#### 2. **Crossfader**
Eliminates clicks and pops during playback start/stop:

```cpp
Crossfader crossfader;
crossfader.startFade(true, 128); // Fade in over 128 samples
float gainValue = crossfader.getNextValue();
```

#### 3. **QuantizationClock**
Synchronizes triggers to musical time:

```cpp
QuantizationClock clock;
clock.setTempo(120.0);
clock.setQuantization(16); // 16th notes
clock.advance(numSamples);

if (clock.shouldTrigger(sampleOffset))
{
    // Trigger sample
}
```

#### 4. **PatternRecorder**
Records and plays back button press sequences:

```cpp
PatternRecorder pattern;
pattern.setLength(4); // 4 beats
pattern.startRecording();
pattern.recordEvent(stripIndex, column, noteOn, currentBeat);
pattern.stopRecording();
pattern.startPlayback();

// Later, during playback:
auto events = pattern.getEventsAtBeat(currentBeat, 0.01);
```

#### 5. **LiveRecorder**
Captures live audio input to strips:

```cpp
LiveRecorder recorder;
recorder.startRecording(4, 120.0); // 4 beats at 120 BPM
recorder.processInput(inputBuffer, startSample, numSamples);
auto recordedBuffer = recorder.getRecordedBuffer();
```

#### 6. **StripGroup**
Manages mute groups (like original mlr):

```cpp
StripGroup group(0);
group.addStrip(0);
group.addStrip(1);
group.setVolume(0.8f);
group.setMuted(true); // Mute strips 0 and 1 together
```

#### 7. **EnhancedAudioStrip**
Full-featured sample playback engine:

```cpp
EnhancedAudioStrip strip(0);
strip.loadSampleFromFile(file);
strip.setPlayMode(EnhancedAudioStrip::PlayMode::Loop);
strip.trigger(column, tempo, quantized);
strip.setVolume(0.7f);
strip.setPan(0.5f); // Pan right
strip.setPlaybackSpeed(1.5f);
strip.setReverse(true);
```

### Play Modes

1. **OneShot**: Play sample once, then stop
2. **Loop**: Continuously loop within the specified range
3. **Gate**: Play while button held (requires button release detection)
4. **Reverse**: Play sample backwards
5. **PingPong**: Alternate between forward and reverse

### Performance Optimizations

#### Lock-Free Atomics
All critical playback parameters use atomic variables:

```cpp
std::atomic<double> playbackPosition{0.0};
std::atomic<float> volume{1.0f};
std::atomic<bool> playing{false};
```

#### Critical Sections
File loading and buffer management use proper locking:

```cpp
juce::CriticalSection bufferLock;

void loadSample()
{
    juce::ScopedLock lock(bufferLock);
    // Modify sample buffer safely
}
```

#### ScopedNoDenormals
Prevents CPU spikes from denormal numbers:

```cpp
void processBlock(...)
{
    juce::ScopedNoDenormals noDenormals;
    // Process audio
}
```

## Features in Detail

### High-Quality Resampling

The resampling engine supports multiple quality modes to balance CPU usage and audio quality:

#### Linear Interpolation
```
Sample = a + t * (b - a)
```
- Fastest
- Good for realtime pitch shifting
- Minimal CPU usage
- Some aliasing at high frequencies

#### Cubic Interpolation
```
a0 = y3 - y2 - y0 + y1
a1 = y0 - y1 - a0
a2 = y2 - y0
a3 = y1
Sample = a0*t³ + a1*t² + a2*t + a3
```
- Balanced quality/performance
- Recommended for most uses
- Smooth frequency response

#### Windowed Sinc
```
sinc(x) = sin(πx) / (πx)
window(x) = 0.54 + 0.46 * cos(πx/N)
Sample = Σ[buffer[i] * sinc(x-i) * window(x-i)]
```
- Highest quality
- Best for pristine audio
- Higher CPU usage
- Minimal aliasing

### Tempo Synchronization

Supports both host and internal tempo:

```cpp
// Use DAW tempo
audioEngine.setUseHostTempo(true);

// Use internal tempo
audioEngine.setUseHostTempo(false);
audioEngine.setInternalTempo(140.0);
```

Beat calculation:
```cpp
double beatsPerSample = (tempo / 60.0) / sampleRate;
currentBeat += beatsPerSample * numSamples;
```

### Quantization

Quantize triggers to musical divisions:

```cpp
audioEngine.setQuantization(16); // 16th notes
```

Supported divisions:
- 1 (whole note)
- 2 (half note)
- 4 (quarter note)
- 8 (eighth note)
- 16 (sixteenth note)
- 32 (thirty-second note)

### Crossfading

Automatic crossfading eliminates clicks:

```cpp
// Start playing with 128-sample fade-in
strip.trigger(column, tempo, true);

// Stop with smooth fade-out
strip.stop(false); // false = smooth stop
strip.stop(true);  // true = immediate stop
```

Default fade lengths:
- Trigger fade-in: 128 samples (~3ms @ 44.1kHz)
- Stop fade-out: 128 samples
- Can be customized per strip

### Pattern Recording

Record up to 4 patterns simultaneously:

```cpp
// Start recording 4-beat pattern
audioEngine.startPatternRecording(0, 4);

// Record events (called from button handler)
pattern.recordEvent(stripIndex, column, noteOn, currentBeat);

// Stop recording
audioEngine.stopPatternRecording(0);

// Play back
audioEngine.playPattern(0);
```

Pattern events are stored with precise timing:
```cpp
struct Event
{
    int stripIndex;  // Which strip
    int column;      // Which position
    double time;     // Beat position
    bool isNoteOn;   // Note on or off
};
```

### Live Recording

Capture live input in sync with tempo:

```cpp
// Start recording 8 beats at current tempo
audioEngine.startLiveRecording(stripIndex, 8);

// Engine automatically captures input
// Check progress
float progress = liveRecorder->getRecordingProgress();

// Stop and retrieve
audioEngine.stopLiveRecording();
auto buffer = liveRecorder->getRecordedBuffer();
```

### Group Management

Organize strips into mute groups:

```cpp
// Create group structure
auto* group1 = audioEngine.getGroup(0);
group1->addStrip(0);
group1->addStrip(1);

auto* group2 = audioEngine.getGroup(1);
group2->addStrip(2);
group2->addStrip(3);

// Control groups
group1->setVolume(0.8f);
group2->setMuted(true); // Mute strips 2 and 3
```

Default configuration:
- 4 groups
- 2 strips per group (configurable)
- Independent volume/mute per group

## Usage Examples

### Basic Playback

```cpp
// Initialize
ModernAudioEngine engine;
engine.prepareToPlay(44100.0, 512);

// Load sample
engine.loadSampleToStrip(0, audioFile);

// Configure strip
auto* strip = engine.getStrip(0);
strip->setPlayMode(EnhancedAudioStrip::PlayMode::Loop);
strip->setVolume(0.8f);

// Trigger playback
strip->trigger(0, 120.0, true); // Column 0, 120 BPM, quantized

// In process block
juce::AudioPlayHead::PositionInfo posInfo;
engine.processBlock(buffer, midi, posInfo);
```

### Advanced Setup

```cpp
// Configure resampling quality
auto* strip = engine.getStrip(0);
strip->resampler.setQuality(Resampler::Quality::Cubic);

// Set up loop points
strip->setLoop(4, 12); // Loop between columns 4-12

// Configure tempo sync
engine.setUseHostTempo(true);
engine.setQuantization(16);

// Set up groups
engine.assignStripToGroup(0, 0);
engine.assignStripToGroup(1, 0);
auto* group = engine.getGroup(0);
group->setVolume(0.7f);

// Start pattern recording
engine.startPatternRecording(0, 4);
```

### Live Input Recording

```cpp
// Start live recording
engine.startLiveRecording(targetStrip, 4); // 4 beats

// Process input (called automatically in processBlock)
liveRecorder->processInput(inputBuffer, 0, numSamples);

// Check progress
float progress = liveRecorder->getRecordingProgress();

// Finish recording
engine.stopLiveRecording();
auto recordedBuffer = liveRecorder->getRecordedBuffer();

// Assign to strip
auto* strip = engine.getStrip(targetStrip);
strip->loadSample(recordedBuffer, sampleRate);
```

## Performance Considerations

### CPU Usage

Typical CPU usage (per strip, 44.1kHz):
- Linear interpolation: 0.5-1%
- Cubic interpolation: 1-2%
- Sinc interpolation: 3-5%

With 8 strips:
- Linear: 4-8%
- Cubic: 8-16%
- Sinc: 24-40%

### Memory Usage

Per strip:
- Sample buffer: Depends on file size
- Overhead: ~1KB (atomics, state)

Total for 8 strips with 1-minute stereo samples @ 44.1kHz:
- Audio data: ~42 MB
- Overhead: ~8 KB
- Total: ~42 MB

### Latency

- Quantization: 0 to 1 quantization division
- Crossfade: 128 samples (~3ms @ 44.1kHz)
- Processing: < 1ms (typical)

Total round-trip: 3-20ms depending on quantization

### Thread Safety

#### Audio Thread (Real-time)
- Lock-free reads from atomic variables
- No allocations
- No system calls
- No file I/O

#### Message Thread (Non-real-time)
- File loading
- Parameter updates
- Sample allocation
- Uses juce::CriticalSection

Safe operations:
```cpp
// Audio thread (always safe)
strip->process(buffer, 0, numSamples, posInfo);
strip->trigger(column, tempo, true);
float vol = strip->getVolume();

// Message thread (uses locks)
strip->loadSampleFromFile(file);
strip->setLoop(start, end);
```

## Best Practices

### 1. Choose Appropriate Resampling Quality

```cpp
// For live performance with many strips
strip->resampler.setQuality(Resampler::Quality::Linear);

// For studio quality with fewer strips
strip->resampler.setQuality(Resampler::Quality::Cubic);

// For mastering/highest quality
strip->resampler.setQuality(Resampler::Quality::Sinc);
```

### 2. Use Quantization for Rhythmic Material

```cpp
// For tight, rhythmic loops
engine.setQuantization(16); // 16th notes
strip->trigger(column, tempo, true); // quantized = true

// For loose, expressive playing
strip->trigger(column, tempo, false); // quantized = false
```

### 3. Optimize Group Assignments

```cpp
// Group similar sounds together
auto* drumGroup = engine.getGroup(0);
drumGroup->addStrip(0); // Kick
drumGroup->addStrip(1); // Snare
drumGroup->addStrip(2); // Hi-hat

// Quick mute all drums
drumGroup->setMuted(true);
```

### 4. Handle Live Recording Carefully

```cpp
// Wait for next bar before starting
if (currentBeat % 4 < 0.1) // Near bar boundary
{
    engine.startLiveRecording(strip, 4);
}

// Monitor progress
float progress = liveRecorder->getRecordingProgress();
updateGUI(progress);
```

### 5. Manage Memory Efficiently

```cpp
// Unload unused samples
strip->loadSample(emptyBuffer, 44100.0);

// Clear pattern data
pattern->clear();

// Reset strip state
strip->stop(true);
strip->clearLoop();
```

## Future Enhancements

Potential additions to the audio engine:

1. **Time-stretching**: Change tempo without pitch change
2. **Pitch-shifting**: Change pitch without tempo change
3. **ADSR envelopes**: Per-strip amplitude envelope
4. **Filters**: Lowpass, highpass, bandpass
5. **Effects**: Reverb, delay, distortion
6. **Sample editor**: Trim, normalize, fade
7. **Multi-sample**: Load multiple samples per strip
8. **Velocity**: MIDI velocity to volume mapping

## Troubleshooting

### Audio Glitches/Clicks

**Cause**: Buffer underruns or denormals
**Solution**:
```cpp
// Increase buffer size
audioEngine.prepareToPlay(44100.0, 1024);

// Ensure ScopedNoDenormals is used
juce::ScopedNoDenormals noDenormals;
```

### High CPU Usage

**Cause**: Too many strips or high quality resampling
**Solution**:
```cpp
// Reduce quality
strip->resampler.setQuality(Resampler::Quality::Linear);

// Limit active strips
if (activeStripCount > 6)
    strip->stop(true);
```

### Timing Issues

**Cause**: Incorrect tempo or quantization
**Solution**:
```cpp
// Verify tempo sync
bool usingHost = audioEngine.getCurrentTempo() == posInfo.getBpm();

// Check quantization setting
int quant = getCurrentQuantization(); // Should be power of 2
```

### Memory Leaks

**Cause**: Samples not released
**Solution**:
```cpp
// Properly cleanup
strip->loadSample(juce::AudioBuffer<float>(), 44100.0);
pattern->clear();
```

## API Reference

See AudioEngine.h for complete API documentation.

Key classes:
- `ModernAudioEngine` - Main engine
- `EnhancedAudioStrip` - Sample playback
- `StripGroup` - Group management
- `PatternRecorder` - Pattern recording
- `LiveRecorder` - Live input
- `QuantizationClock` - Tempo sync
- `Resampler` - High-quality resampling
- `Crossfader` - Click elimination
