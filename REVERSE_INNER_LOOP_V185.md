# mlrVST v185 - Reverse Inner Loop

## Feature: Automatic Reverse on Backwards Loop

When creating an inner loop by pressing a **higher column first**, the playback automatically switches to **reverse mode**.

### How It Works

**Normal Inner Loop (Forward):**
```
Hold column 3, press column 7
→ Inner loop: 3-7
→ Playback: FORWARD
→ Dropdown: Shows "Loop" (or current mode)
```

**Reverse Inner Loop (Backwards):**
```
Hold column 7, press column 3
→ Inner loop: 3-7 (same range)
→ Playback: REVERSE
→ Dropdown: Shows "Reverse"
```

**Restoring Normal Playback:**
```
Press any button on that strip
→ Inner loop cleared
→ Full loop restored: 0-16
→ Playback: FORWARD (normal)
→ Dropdown: Shows previous mode
```

## Implementation

### Loop Creation with Direction Detection

**PluginProcessor.cpp - handleMonomeKeyPress():**

```cpp
// Loop length setting mode
if (loopSetFirstButton >= 0 && loopSetStrip == stripIndex)
{
    int start = juce::jmin(loopSetFirstButton, x);
    int end = juce::jmax(loopSetFirstButton, x) + 1;
    
    // Detect reverse: first button > second button
    bool shouldReverse = (loopSetFirstButton > x);
    
    strip->setLoop(start, end);
    strip->setReverse(shouldReverse);  // Auto-reverse if backwards
    
    DBG("Inner loop: " << start << "-" << end << 
        (shouldReverse ? " (REVERSE)" : " (NORMAL)"));
}
```

### Clear Loop with Playback Restore

**PluginProcessor.cpp - triggerStrip():**

```cpp
// If inner loop is active, clear it
if (strip->getLoopStart() != 0 || strip->getLoopEnd() != MaxColumns)
{
    strip->clearLoop();
    strip->setReverse(false);  // Restore forward playback
    return;
}
```

### UI Sync

**PluginEditor.cpp - updateFromEngine():**

```cpp
// Sync play mode dropdown with strip state
if (strip->isReversed())
{
    // Show Reverse mode
    playModeBox.setSelectedId(4, dontSendNotification);
}
else
{
    // Show actual play mode
    int modeId = static_cast<int>(strip->getPlayMode()) + 1;
    playModeBox.setSelectedId(modeId, dontSendNotification);
}
```

## Examples

### Example 1: Reverse Vocal Chop
```
1. Load vocal sample
2. Hold column 10, press column 6
   → Loop: 6-10 (REVERSE)
   → Vocal plays backwards
3. Press any button
   → Full loop, forward playback restored
```

### Example 2: Reverse Then Forward
```
1. Hold column 12, press column 4
   → Loop: 4-12 (REVERSE)
2. Press any button
   → Full loop (FORWARD)
3. Hold column 4, press column 12
   → Loop: 4-12 (FORWARD)
```

### Example 3: Quick Direction Change
```
Same loop region, different directions:

Forward: Hold 3, press 9 → 3-9 forward
Clear: Press button → Full loop
Reverse: Hold 9, press 3 → 3-9 reverse
```

## Visual Feedback

The **playback mode dropdown** automatically updates:

```
Creating reverse loop:
[Loop ▼] → [Reverse ▼]

Clearing loop:
[Reverse ▼] → [Loop ▼]
```

This provides clear visual confirmation of the playback direction.

## Logic Summary

**Detection:**
```
if (firstButton > secondButton)
    → REVERSE playback
else
    → NORMAL playback
```

**Loop Range:**
```
Always: min(button1, button2) to max(button1, button2)
Example: buttons 7 and 3 both create loop 3-7
```

**Direction:**
```
Hold 7, press 3 → 3-7 REVERSE
Hold 3, press 7 → 3-7 FORWARD
```

## Use Cases

### Creative Uses:
1. **Reverse reveals** - Hide/reveal reversed audio
2. **Bi-directional loops** - Same material, different feels
3. **Live remixing** - Quick direction changes
4. **Texture creation** - Reverse small sections

### Performance Workflow:
```
1. Find interesting loop region
2. Try both directions quickly:
   - Hold high, press low → reverse
   - Clear and reverse button order → forward
3. Choose best sound
4. Clear when done
```

## Technical Notes

### Why This Design?

The button order naturally indicates direction:
- **High → Low** = going backwards = reverse
- **Low → High** = going forwards = normal

This is intuitive and requires no extra buttons or menus.

### Crossfade Compatibility

The reverse detection works seamlessly with inner loop crossfade (v182):
- Forward loops: crossfade works normally
- Reverse loops: crossfade still works (audio flows backwards)

### Pattern Recording

When recording patterns:
- Loop creation with direction is recorded
- Clearing loop is NOT recorded
- This preserves musical gestures in patterns

## Comparison with Manual Reverse

**Before v185 (Manual):**
```
1. Create inner loop (3-7)
2. Open dropdown
3. Select "Reverse"
4. Clear loop
5. Open dropdown
6. Select "Loop" again
Total: 6 steps
```

**After v185 (Automatic):**
```
1. Hold 7, press 3 → Reverse loop
2. Press button → Normal restored
Total: 2 steps
```

## Benefits

✅ **Intuitive** - Button order = playback direction  
✅ **Fast** - One gesture, no menus  
✅ **Visual** - Dropdown shows current state  
✅ **Reversible** - Easy to toggle  
✅ **Creative** - Encourages experimentation  

## Summary

Inner loops now automatically reverse when created with higher-to-lower column order, and restore to forward playback when cleared. The playback mode dropdown syncs automatically, providing clear visual feedback of the current direction.
