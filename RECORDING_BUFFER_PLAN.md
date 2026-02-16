# Continuous Recording Buffer - Implementation Plan

## Feature Requirements

### 1. Continuous Recording Buffer
- Always capturing input signal (circular buffer)
- Loop length: 1-4 bars (selectable)
- Crossfade at loop points for smooth transitions

### 2. Monome Integration
**Sample-Select Page (Normal Mode):**
- Button 15 (last button): Record/Stop with blinking LED
  - Blinking at BPM tempo while armed/recording
  - Press to stop recording and load into strip
- Buttons 12-14: Loop length selection (1-4 bars)
  - Blinking LED on selected length
  - Button 12: 1 bar
  - Button 13: 2 bars  
  - Button 14: 3 bars
  - Button 15 changes to: 4 bars when not recording

### 3. Workflow
1. User selects loop length (buttons 12-14)
2. Buffer continuously records (circular)
3. User presses button 15 on a strip → Captures last N bars
4. Audio is crossfaded at loop points
5. Loaded into strip's sample slot
6. Ready to trigger immediately

## Implementation Steps

### Step 1: Enhanced LiveRecorder Class
```cpp
class LiveRecorder
{
public:
    // Circular buffer - always recording
    void setLoopLength(int bars);  // 1-4 bars
    void setCrossfadeLength(int samples);  // Crossfade duration
    
    // Capture methods
    void captureToStrip(int stripIndex);  // Grab last N bars
    
    // LED feedback
    bool shouldBlinkRecordLED() const;  // Blink at tempo
    int getSelectedLoopLength() const;  // 1-4
    
private:
    juce::AudioBuffer<float> circularBuffer;  // Continuous recording
    std::atomic<int> writeHead{0};
    int loopLengthInSamples;
    int crossfadeSamples;
    int selectedBars{1};  // 1-4
};
```

### Step 2: Monome Button Mapping
```
Row 1-6 (strips):
[0-11]: Trigger columns (existing)
[12]:   1 bar length selector (blinking if selected)
[13]:   2 bars length selector (blinking if selected)
[14]:   3 bars length selector (blinking if selected)
[15]:   Record/Stop button (blinking at BPM)
```

### Step 3: LED Blinking System
- Add timer/counter in updateMonomeLEDs()
- Blink at current tempo (beat divisions)
- Record button: Blinks when armed
- Loop length buttons: Selected one blinks slowly

### Step 4: Crossfade Implementation
```cpp
void applyCrossfade(AudioBuffer& buffer)
{
    int fadeLen = crossfadeSamples;
    
    // Fade out at end
    for (int i = 0; i < fadeLen; ++i)
    {
        float gain = 1.0f - (i / (float)fadeLen);
        buffer.applyGain(buffer.getNumSamples() - fadeLen + i, 1, gain);
    }
    
    // Fade in at start  
    for (int i = 0; i < fadeLen; ++i)
    {
        float gain = i / (float)fadeLen;
        buffer.applyGain(i, 1, gain);
    }
    
    // Crossfade: add end to beginning
    for (int i = 0; i < fadeLen; ++i)
    {
        int srcIdx = buffer.getNumSamples() - fadeLen + i;
        buffer.addFrom(0, i, buffer, 0, srcIdx, 1);
        buffer.addFrom(1, i, buffer, 1, srcIdx, 1);
    }
}
```

## Challenges & Solutions

### Challenge 1: Tempo Sync
**Problem:** Need to know exact bar length in samples
**Solution:** Use current tempo from processBlock
```cpp
double beatsPerBar = 4.0;
double bpm = currentTempo;
double samplesPerBeat = (60.0 / bpm) * sampleRate;
int barLengthSamples = samplesPerBeat * beatsPerBar * selectedBars;
```

### Challenge 2: Sample-Accurate Capture
**Problem:** Must capture exactly N bars
**Solution:** Circular buffer with exact read position calculation

### Challenge 3: LED Blinking
**Problem:** Need tempo-synced blinking
**Solution:** Track beat position, toggle LED on beats

## File Changes Required

1. **AudioEngine.h** - Enhance LiveRecorder class
2. **AudioEngine.cpp** - Implement circular buffering & crossfade
3. **PluginProcessor.cpp** - Add button handlers for record/loop-length
4. **PluginProcessor.cpp** - Update LED blinking in updateMonomeLEDs()

## Testing Checklist

- [ ] Circular buffer records continuously
- [ ] Loop length selection works (1-4 bars)
- [ ] Crossfade is smooth at loop points
- [ ] Record button captures correct length
- [ ] Audio loads into strip correctly
- [ ] LED blinks at tempo
- [ ] Loop length LED indicator works
- [ ] Works with different tempos
- [ ] Works with different time signatures

## Notes

- Crossfade length: ~10-50ms recommended (441-2205 samples @ 44.1kHz)
- Circular buffer size: Must hold maximum (4 bars at slowest tempo)
- At 60 BPM, 4 bars = 16 seconds = 705,600 samples @ 44.1kHz
- Safe buffer size: 1,000,000 samples (~23 seconds @ 44.1kHz)
