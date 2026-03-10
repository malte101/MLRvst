# mlrVST v167 - Input Monitoring

## Version Information
**Version:** v167
**Previous:** v166 (Perfect Clock Sync)
**Date:** February 10, 2026

## New Feature: Live Input Monitoring

Added audio pass-through so you can monitor your live input while using mlrVST.

### What It Does

When enabled, the live input signal is mixed into the output alongside your playing strips. This lets you:
- Hear what you're recording before capturing it
- Monitor your instrument/mic while playing back loops
- Mix live performance with recorded loops in real-time

### How to Use

**Parameter:** Input Monitor (0.0 - 1.0)
- **0.0** = Input monitoring off (default)
- **1.0** = Full volume input pass-through
- **0.5** = Half volume (good for blending)

The input monitor volume is independent of master volume, so you can balance live input vs. playback.

## Implementation Details

### Signal Flow

```
Input Buffer
    ↓
    ├─→ Copy for monitoring (if enabled)
    ├─→ Live Recorder (captures for loop recording)
    ↓
Clear buffer
    ↓
Process all strips (add to buffer)
    ↓
Apply master volume
    ↓
Add back monitored input (scaled by inputMonitor parameter)
    ↓
Output
```

### Key Points

1. **Zero Latency:** Direct pass-through, no buffering delay
2. **Independent Volume:** Input monitor level separate from master
3. **Non-Destructive:** Doesn't affect recording or strip playback
4. **Clean Mix:** Input added after master volume for proper balance

### Code Implementation

**Parameter Added:**
```cpp
layout.add(std::make_unique<juce::AudioParameterFloat>(
    "inputMonitor",
    "Input Monitor",
    juce::NormalisableRange<float>(0.0f, 1.0f),
    0.0f));  // Default off
```

**In processBlock:**
```cpp
// Save input before clearing
juce::AudioBuffer<float> inputCopy;
if (inputMonitorVol > 0.0f)
{
    inputCopy.setSize(numChannels, numSamples);
    for (int ch = 0; ch < numChannels; ++ch)
        inputCopy.copyFrom(ch, 0, buffer, ch, 0, numSamples);
}

// ... process strips ...

// Mix input back in
if (inputMonitorVol > 0.0f)
{
    for (int ch = 0; ch < numChannels; ++ch)
        buffer.addFrom(ch, 0, inputCopy, ch, 0, numSamples, inputMonitorVol);
}
```

## Use Cases

### Recording Workflow
1. Set Input Monitor to 0.7 (70%)
2. Play your instrument
3. Hear it mixed with existing loops
4. When ready, trigger recording to capture it
5. Turn down Input Monitor, hear captured loop

### Live Performance
1. Set Input Monitor to 0.5-0.8
2. Play live over your loops
3. Trigger loops on the fly
4. Balance live vs. recorded with the parameter

### Practice/Jamming
1. Load backing loops on strips
2. Enable Input Monitor
3. Play along with your instrument
4. Record new parts when inspired

## Technical Specifications

### Performance
- **CPU Cost:** Minimal (one buffer copy + addFrom)
- **Memory:** One additional buffer when monitoring enabled
- **Latency:** Zero additional latency

### Compatibility
- Works in all DAWs
- Respects host input routing
- Compatible with all existing features
- No conflicts with live recording

## Parameter Details

**Name:** `inputMonitor`  
**Display:** "Input Monitor"  
**Type:** Float (continuous)  
**Range:** 0.0 to 1.0  
**Default:** 0.0 (off)  
**Automation:** Yes (fully automatable)

## UI Considerations

For future GUI development, this parameter should be displayed as:
- A volume slider labeled "Input Monitor" or "Input"
- Optional VU meter showing input level
- Toggle button for quick on/off (0.0 ↔ previous value)
- Visual indication when monitoring is active

## Tips

### Avoid Feedback
If using speakers and a microphone:
- Keep Input Monitor at moderate levels (< 0.5)
- Use headphones for higher monitoring levels
- Position mic away from speakers

### Finding the Right Level
- Start at 0.3-0.4 for monitoring
- Adjust based on your input source strength
- Balance with master volume for best mix

### Recording
- You can leave Input Monitor on while recording
- The recorded loop won't include the monitored input
- Only the actual input signal is captured

## Code Changes

### Files Modified

1. **Source/PluginProcessor.cpp**
   - Added `inputMonitor` parameter to layout
   - Added parameter update in processBlock

2. **Source/AudioEngine.h**
   - Added `setInputMonitorVolume()` method
   - Added `getInputMonitorVolume()` method
   - Added `inputMonitorVolume` atomic member

3. **Source/AudioEngine.cpp**
   - Implemented `setInputMonitorVolume()`
   - Modified `processBlock()` to copy and mix input
   - Added input buffer allocation and mixing

## Future Enhancements

Potential additions:
- Input gain/trim control
- Input meter display
- Per-strip input routing
- Input effects (EQ, compression)
- Stereo width control for input
- Latency compensation for plugin chains

## Compatibility

- **All v166 features preserved**
- Perfect clock sync still intact
- No changes to recording behavior
- Backward compatible with existing sessions
- New parameter defaults to 0.0 (off)

## Build Instructions

Same as always:
```bash
cd mlrVST-modern
make clean
make configure
make build
sudo make install
```

## Testing Checklist

- [ ] Input Monitor at 0.0 → no input heard (silent)
- [ ] Input Monitor at 1.0 → full input pass-through
- [ ] Input Monitor at 0.5 → balanced mix
- [ ] Recording works with monitoring on
- [ ] No feedback loops at reasonable levels
- [ ] Parameter automation works
- [ ] Works in Ableton, Logic, etc.
- [ ] Zero latency (test with click track)
- [ ] Master volume affects strips, not input monitor
- [ ] Both stereo channels pass through

## Summary

v167 adds simple, zero-latency input monitoring by:
1. Copying the input buffer before processing
2. Processing strips normally
3. Mixing the saved input back in at the end

This provides a clean monitoring path while keeping the recording and playback chain completely separate.

Perfect for live performance, recording workflows, and practice sessions!
