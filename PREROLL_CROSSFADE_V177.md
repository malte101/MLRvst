# mlrVST v177 - CORRECT Loop Crossfade with Pre-roll

## The Actual Problem

**Your Description:**
> "On record stop, bake a loop crossfade by fading out the end of the recorded region and fading in the audio captured immediately before the loop start."

**What We Were Doing WRONG:**
```
Loop buffer: [START ========== END]
                ↑                ↑
          Fading this IN   Fading this OUT
          
When loop wraps: END → START
Result: START is already in the loop! ❌
```

This creates a weird echo/doubling effect because the START is playing twice:
1. Once at the END (faded in)
2. Once at the actual START (normal playback)

## The CORRECT Approach

**What We Should Do:**
```
Circular recording: [...BEFORE][LOOP START ====== LOOP END]
                         ↑                           ↑
                    PRE-ROLL audio            Loop end (fade out)
                    
Crossfade: Blend PRE-ROLL into LOOP END
Result: END → PRE-ROLL → START (seamless!) ✓
```

## Visual Explanation

### Wrong (v172-v176):
```
Capture:
[========== LOOP ==========]
 START              END

Crossfade:
[START faded into END]
 ↑
 This creates doubling!

Playback:
END (contains START) → actual START
       ↑                      ↑
   Already heard!        Heard again!
```

### Correct (v177):
```
Capture:
[PRE-ROLL][========== LOOP ==========]
           START              END

Crossfade:
PRE-ROLL faded into END
    ↑
Audio from BEFORE loop

Playback:
END (contains PRE-ROLL) → START
       ↑                     ↑
   Fresh audio!         Seamless!
```

## Implementation

### Step 1: Capture Extra Audio

```cpp
// Calculate how much to capture
int loopLength = /* based on tempo/bars */;
int crossfadeLength = /* user slider 1-50ms */;

// Capture loop + pre-roll
int totalCapture = loopLength + crossfadeLength;

// Read from circular buffer:
// [PRE-ROLL (crossfade)][LOOP (loopLength)]
```

### Step 2: Extract Loop

```cpp
// Create output buffer (just loop, no pre-roll)
AudioBuffer output(loopLength);

// Copy only the LOOP portion (skip pre-roll)
output.copyFrom(captureBuffer, startAt: crossfadeLength);
```

### Step 3: Bake Crossfade

```cpp
void bakeLoopCrossfadeWithPreroll(
    AudioBuffer& loopBuffer,      // Just the loop
    AudioBuffer& captureBuffer,   // Loop + pre-roll
    int crossfadeSamples)
{
    // Blend pre-roll INTO end of loop
    for (int i = 0; i < crossfadeSamples; ++i)
    {
        float t = i / (crossfadeSamples - 1);
        float fadeOut = sqrt(cos(t * π/2));  // Loop end: 1→0
        float fadeIn = sqrt(sin(t * π/2));   // Pre-roll: 0→1
        
        // Read from END of loop
        float endSample = loopBuffer[end - crossfade + i];
        
        // Read from PRE-ROLL (start of capture buffer)
        float prerollSample = captureBuffer[i];
        
        // Blend and write to loop end
        loopBuffer[end - crossfade + i] = 
            (endSample * fadeOut) + (prerollSample * fadeIn);
    }
}
```

## Code Changes

### AudioEngine.cpp - captureLoop()

**Before (v176):**
```cpp
// Only captured the loop
for (int i = 0; i < loopLength; ++i)
    output[i] = circularBuffer[readHead - loopLength + i];

// Crossfaded START into END (wrong!)
bakeLoopCrossfade(output, 0, loopLength);
```

**After (v177):**
```cpp
// Capture loop + pre-roll
int totalCapture = loopLength + crossfadeLength;
for (int i = 0; i < totalCapture; ++i)
    captureBuffer[i] = circularBuffer[readHead - totalCapture + i];

// Extract just the loop (skip pre-roll)
output.copyFrom(captureBuffer, start: crossfadeLength);

// Crossfade PRE-ROLL into END (correct!)
bakeLoopCrossfadeWithPreroll(output, captureBuffer, ...);
```

### AudioEngine.cpp - bakeLoopCrossfadeWithPreroll()

**New function:**
```cpp
void bakeLoopCrossfadeWithPreroll(
    AudioBuffer& loopBuffer,
    const AudioBuffer& captureBuffer,
    int loopStart, int loopEnd, int crossfadeSamples)
{
    // captureBuffer layout: [PRE-ROLL][LOOP]
    //                        ^
    //                        Use this for crossfade!
    
    int fadeStart = loopEnd - crossfadeSamples;
    
    for (int i = 0; i < crossfadeSamples; ++i)
    {
        float t = i / (crossfadeSamples - 1);
        float fadeOut = sqrt(cos(t * π/2));
        float fadeIn = sqrt(sin(t * π/2));
        
        // Loop end (fading out)
        float endSample = loopBuffer[fadeStart + i];
        
        // Pre-roll (fading in) - from START of captureBuffer
        float prerollSample = captureBuffer[i];
        
        // Blend
        loopBuffer[fadeStart + i] = 
            (endSample * fadeOut) + (prerollSample * fadeIn);
    }
}
```

## Why This Works

**Timeline:**
```
Circular buffer (continuous recording):
Time: -100ms  0ms          2000ms       2100ms
      [...PRE][START ====== END][...]
           ↑                 ↑
      Capture this    Fade out this

Loop playback:
      [START ========== blended_END]
                              ↑
                        Contains PRE-ROLL

When loop wraps:
      blended_END → START
           ↑            ↑
      (has PRE-ROLL) (actual start)
      
Because PRE-ROLL comes from just BEFORE START,
the transition is seamless!
```

## Testing

### Test 1: Record Drums
1. Record 1-bar drum loop
2. Set crossfade to 10ms
3. Listen at loop point
4. **Expected:** No click, no doubling

### Test 2: Record Sustained Note
1. Record sustained pad/tone
2. Set crossfade to 25ms
3. Listen at loop point
4. **Expected:** Seamless continuation, no phase shift

### Test 3: Test Crossfade Length
1. Record loop
2. Try 1ms crossfade
3. Try 50ms crossfade
4. **Expected:** Longer = smoother, shorter = tighter

## Summary

**The Key Difference:**

❌ **v172-v176:** Crossfaded loop START into loop END  
→ Result: Doubling artifact (START heard twice)

✅ **v177:** Crossfades PRE-ROLL into loop END  
→ Result: True seamless loop

**Why Pre-roll Matters:**

The audio immediately BEFORE the loop start is the perfect material to blend with the loop end, because that's exactly what would have been playing if we hadn't captured the loop. It's the natural continuation!

This is how hardware loopers and MLR work - they always have a buffer of audio before the loop point to use for crossfading.
