# mlrVST v182 - Inner Loop Pre-roll Crossfade (Correct Implementation)

## The Fix

The inner loop crossfade was blending the START of the loop into the END, but it should blend the audio BEFORE the loop start (pre-roll) into the END, just like the baked crossfade does.

## Example

**Inner Loop: Columns 3 to 7**

### WRONG (v181):
```
Sample: [Col 0][Col 1][Col 2][Col 3 ══ Loop ══ Col 7][Col 8]...
                                ↑                     ↑
                              START                  END

Crossfade at end:
End samples fade out, START (Col 3) fades in

Problem: Col 3 plays twice (once faded in at end, once normally at start)
```

### CORRECT (v182):
```
Sample: [Col 0][Col 1][Col 2][Col 3 ══ Loop ══ Col 7][Col 8]...
                    ↑↑↑        ↑                     ↑
                 PRE-ROLL    START                  END

Crossfade at end:
End samples (Col 7) fade out
PRE-ROLL (before Col 3) fades in

When loop wraps: Col 7 → Pre-roll → Col 3 (seamless!)
```

## Code Changes

### AudioEngine.cpp

**Before:**
```cpp
// WRONG: Read from START of loop
double offsetIntoFade = positionInLoop - fadeStart;
startSamplePosition = loopStartSamples + offsetIntoFade;
```

**After:**
```cpp
// CORRECT: Read from BEFORE loop start (pre-roll)
double offsetIntoFade = positionInLoop - fadeStart;
prerollSamplePosition = loopStartSamples - crossfadeSamples + offsetIntoFade;

// Handle wrapping if pre-roll goes before sample start
if (prerollSamplePosition < 0)
    prerollSamplePosition += sampleLength;
```

## Visual Comparison

### v181 (Wrong):
```
Columns:  0  1  2  3  4  5  6  7  8  9
Sample:  [───────][█████████████][─────]
                  ↑               ↑
           Loop Start (3)    Loop End (7)

Crossfade zone (last 10ms of loop):
[3═════════════████7]
                ↑↑↑↑
         Col 3 faded in here

When wrapping: ████→3
               ↑    ↑
          Col 3  Col 3 again (doubling!)
```

### v182 (Correct):
```
Columns:  0  1  2  3  4  5  6  7  8  9
Sample:  [───────][█████████████][─────]
           ↑↑↑    ↑               ↑
        Pre-roll Start (3)    End (7)

Crossfade zone (last 10ms of loop):
Before 3: [2.9, 2.91, 2.92 ... 2.99]
          ↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑
    This fades in at end of 7

When wrapping: 7→Pre-roll→3 (seamless!)
```

## How Pre-roll Works

The pre-roll is the audio immediately BEFORE the loop start point:

```
If inner loop starts at column 3:
- Loop start sample: loopStartSamples
- Pre-roll start: loopStartSamples - crossfadeSamples
- Pre-roll contains: audio from just before column 3

At 10ms crossfade and 44.1kHz:
- crossfadeSamples = 441 samples
- Pre-roll = 441 samples before loop start
- This is about 0.3 columns worth of audio
```

## Why This Works

The audio immediately before the loop start is the **perfect material** to blend with the loop end because:

1. **Natural continuity**: It's what would have been playing if we hadn't looped
2. **Phase coherent**: No phase jumps or discontinuities
3. **Musically appropriate**: Maintains musical flow

Just like the baked loop crossfade (v177) uses pre-roll from the circular buffer, the inner loop crossfade now uses pre-roll from the sample buffer.

## Testing

**To verify:**
1. Load a sample with clear beats
2. Hold column 4, press column 8 (creates inner loop)
3. Listen at the loop boundary
4. Should hear smooth transition with no click or doubling

**What you should hear:**
- ✅ Smooth crossfade at loop end
- ✅ No clicking
- ✅ No echo/doubling effect
- ✅ Natural musical flow

**What you should NOT hear:**
- ❌ Click at loop point
- ❌ Column 4 playing twice
- ❌ Phase artifacts
- ❌ Unnatural jumps

## Additional Fix

Removed "Modern Edition v2.0" subtitle from the main UI for cleaner look.

## Summary

Inner loop crossfades now correctly use pre-roll audio (from before the loop start) instead of the loop start itself, matching the behavior of the baked loop crossfade and eliminating the doubling artifact.

Combined fixes:
- v177: Baked loop uses pre-roll ✓
- v182: Inner loop uses pre-roll ✓
- Result: All looping is seamless!
