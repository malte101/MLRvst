# mlrVST v183 - Inner Loop Clear on Next Press

## Feature: Exit Inner Loop

When an inner loop is active, pressing **any button** on that strip will now:
1. Clear the inner loop
2. Restore full loop length (0-16)
3. NOT trigger a new playback

## How It Works

### Creating Inner Loop:
1. **Hold** column button (e.g., column 3)
2. **Press** another column (e.g., column 7)
3. Inner loop created: columns 3-7

### Clearing Inner Loop:
1. Inner loop is active (3-7)
2. **Press any button** on that strip
3. Loop clears → restored to full 0-16

## Code Implementation

### PluginProcessor.cpp - triggerStrip()

Added check at the beginning of the function:

```cpp
void MlrVSTAudioProcessor::triggerStrip(int stripIndex, int column)
{
    auto* strip = audioEngine->getStrip(stripIndex);
    
    // CHECK: If inner loop is active, clear it
    if (strip->getLoopStart() != 0 || strip->getLoopEnd() != MaxColumns)
    {
        // Inner loop detected - clear it
        strip->clearLoop();
        return;  // Don't trigger, just clear
    }
    
    // Normal trigger logic continues...
}
```

### Detection Logic

An inner loop is detected when:
```cpp
loopStart != 0  OR  loopEnd != 16
```

**Examples:**
- Loop 0-16: Full loop (not inner) ✓
- Loop 3-7: Inner loop ✓ → Will clear on next press
- Loop 0-8: Inner loop ✓ → Will clear on next press
- Loop 5-16: Inner loop ✓ → Will clear on next press

## User Workflow

### Example Session:

```
1. Load sample on strip 1
   → Full loop: 0-16

2. Hold column 4, press column 9
   → Inner loop: 4-9
   → Crossfade active at loop boundary

3. Press any column on strip 1
   → Inner loop cleared
   → Back to full loop: 0-16

4. Hold column 2, press column 14
   → New inner loop: 2-14
   → Crossfade active

5. Press column 8 on strip 1
   → Inner loop cleared
   → Full loop restored: 0-16
```

## Visual Indicator

The Monome LEDs should show:
- **Inner loop active:** LEDs on for loop region
- **After clear:** LEDs on for full strip

## Edge Cases Handled

### Case 1: Full Loop
```cpp
loopStart = 0, loopEnd = 16
// Not an inner loop, trigger normally
```

### Case 2: Single Column Loop
```cpp
loopStart = 5, loopEnd = 6
// Inner loop! Clears on next press
```

### Case 3: Almost Full Loop
```cpp
loopStart = 0, loopEnd = 15
// Inner loop! (not quite full 16)
// Clears on next press
```

## Benefits

1. **Easy exit:** One button press to return to normal
2. **No special button:** Any column works
3. **Intuitive:** Similar to MLR behavior
4. **Clean state:** Always returns to 0-16

## Testing

**Test sequence:**
1. Create inner loop (hold + press)
2. Verify crossfade works
3. Press any button on that strip
4. Verify:
   - Loop cleared
   - Full playback restored
   - No trigger happened

## Combined Features (v177-v183)

✅ **v177:** Baked loop crossfade (pre-roll at capture)  
✅ **v182:** Inner loop crossfade (pre-roll during playback)  
✅ **v183:** Clear inner loop on next press  

**Result:** Complete, intuitive inner loop workflow with seamless crossfading!

## Note on Pattern Recording

When clearing an inner loop, the clear action is **not** recorded to pattern recorders. This is intentional - the clear happens before pattern recording logic, so patterns only record actual triggers, not loop management actions.
