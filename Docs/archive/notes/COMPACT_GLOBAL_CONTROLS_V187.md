# mlrVST v187 - Compact Global Controls

## Feature: Space-Efficient Control Layout

The Global Controls panel has been redesigned with **tighter spacing** and **optimized widths** to leave room for future controls.

## Layout Comparison

### Before (v186):
```
[────────────────────────────────────────────────────]
│ Master │ Input │ L R │ Tempo │ Xfade │ Quant │ Qual │
│  (1/7) │ (1/7) │(0.6)│ (1/7) │ (1/7) │ (1/7) │ (1/7)│
└────────────────────────────────────────────────────┘
← Equal width divisions, no leftover space →
```

### After (v187):
```
[──────────────────────────────────────────] [SPACE]
│M│I│LR│Tempo│Xfade│Quantize│Quality│ ← Future
│50│50│30│ 70 │ 70 │  100  │  100 │   Controls
└──────────────────────────────────────────┘
← Optimized widths, leftover space for expansion →
```

## Space Savings

### Reduced Spacing:
- **Panel padding:** 8px → 6px (25% reduction)
- **Title height:** 24px → 20px
- **Label height:** 20px → 16px (20% reduction)
- **Label gap:** 4px → 2px (50% reduction)
- **Control spacing:** Auto → 8px fixed
- **Knob size:** 80px → 70px

### Optimized Widths:
```cpp
const int sliderWidth = 50;      // Vertical sliders (Master, Input)
const int meterWidth = 30;       // L/R meters (compact)
const int knobWidth = 70;        // Rotary knobs (Tempo, Crossfade)
const int dropdownWidth = 100;   // Dropdowns (Quantize, Quality)
const int spacing = 8;           // Between controls
```

### Total Reduction:
```
Before: 7 controls × (width/7) = full width
After: ~470px used, rest available for future
Savings: ~30-40% of horizontal space freed!
```

## Visual Changes

### Title:
```
Before: "Global Controls" (14px, white)
After:  "GLOBAL CONTROLS" (11px, gray #888)
```
- Uppercase for modern look
- Smaller and less prominent
- Gray color (doesn't compete with controls)

### Labels:
```
Before: 20px height, 4px gap
After:  16px height, 2px gap
```
- Still readable
- Less vertical space
- Tighter integration

### Controls:
```
Master/Input sliders:  50px wide (was ~140px)
L/R meters:           30px wide (was ~85px)
Tempo/Crossfade:      70px wide (was ~140px)
Dropdowns:           100px wide (was ~140px)
```
- Right-sized for actual needs
- No wasted space
- Better density

## Implementation

### New Layout Logic:

**PluginEditor.cpp - GlobalControlPanel::resized():**

```cpp
// Define actual needs (not equal division)
const int sliderWidth = 50;
const int meterWidth = 30;
const int knobWidth = 70;
const int dropdownWidth = 100;
const int spacing = 8;

// Lay out left to right with spacing
masterArea = controlsArea.removeFromLeft(sliderWidth);
controlsArea.removeFromLeft(spacing);

inputArea = controlsArea.removeFromLeft(sliderWidth);
controlsArea.removeFromLeft(spacing);

meterArea = controlsArea.removeFromLeft(meterWidth);
controlsArea.removeFromLeft(spacing);

// ... etc

// Remaining controlsArea = space for future!
```

### Key Principle:

**Before:** Divide available space equally among controls
```cpp
auto controlWidth = controlsArea.getWidth() / 7;  // Each gets 1/7
```

**After:** Give each control what it actually needs
```cpp
const int sliderWidth = 50;  // This is all a slider needs
```

## Benefits

### Immediate:
✅ **Cleaner look** - Less wasted space
✅ **Better density** - More controls visible
✅ **Easier scanning** - Controls closer together
✅ **Modern aesthetic** - Compact, efficient

### Future:
✅ **Room for expansion** - ~30-40% space freed
✅ **More controls** - Can add 3-4 more controls
✅ **Flexibility** - Easy to add without redesign

## Future Control Ideas

The freed space could accommodate:

**Performance:**
- Master transpose
- Master filter
- FX send level
- Monitor mode selector

**Recording:**
- Auto-quantize toggle
- Input gain trim
- Loop fade style
- Pre-roll length

**Display:**
- Clock display
- CPU meter
- Strip meter overview
- Status indicators

## Measurements

### Before (v186):
```
Total panel width: ~1000px
Used space: ~1000px (100%)
Controls: 7
Wasted space: High (equal divisions)
Future capacity: None
```

### After (v187):
```
Total panel width: ~1000px
Used space: ~470px (47%)
Controls: 7
Wasted space: Minimal (right-sized)
Future capacity: ~530px (3-5 controls)
```

## Visual Example

```
GLOBAL CONTROLS
┌─┬─┬─┬────┬────┬────────┬────────┬───────────────┐
│M│I│LR│Tempo│Xfade│Quantize│Quality│  Available  │
│a│n││     │     │        │       │   for future│
│s│p││120.0│10ms │  1/8   │ Cubic │   controls  │
│t│u││ BPM │     │        │       │             │
│e│t││     │     │        │       │             │
│r││││     │     │        │       │             │
└─┴─┴─┴────┴────┴────────┴────────┴───────────────┘
50 50 30  70   70    100      100        ~530px
```

## Technical Notes

### Layout Method:
- **Sequential removal** from left to right
- **Fixed spacing** between controls (8px)
- **Remaining space** left intact for future use
- **No stretching** or auto-sizing

### Maintainability:
```cpp
// Easy to add new controls:
auto newControlArea = controlsArea.removeFromLeft(newWidth);
// Setup new control...
controlsArea.removeFromLeft(spacing);
```

### Responsive:
- Works at different window sizes
- Controls maintain fixed minimum widths
- Leftover space scales with window

## User Experience

### Before:
- Controls spread out across panel
- Eyes must scan wide area
- Wasted white space visible
- Can't add more without cramping

### After:
- Controls grouped efficiently
- Easier to scan quickly
- Professional, compact look
- Ready for expansion

## Summary

Global Controls now use **optimized widths** instead of equal divisions, freeing up **~30-40% of horizontal space** for future controls. The layout is more compact, professional, and ready for expansion without sacrificing usability.

**Key Changes:**
- Optimized control widths (50-100px instead of equal ~140px)
- Reduced spacing (6px padding, 2px gaps, 8px between)
- Smaller title (11px, gray)
- Leftover space preserved for future features
