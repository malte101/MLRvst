# mlrVST v186 - Tabbed Top Controls

## Feature: Space-Saving Tabbed Interface

The Monome Device settings and Global Controls are now in a **tabbed interface** at the top, saving vertical space for more strips.

### Before (v185):
```
┌─────────────────────────────────────────┐
│ mlrVST                                  │
├──────────────────┬──────────────────────┤
│ Monome Device    │ Global Controls      │
│ (left half)      │ (right half)         │
└──────────────────┴──────────────────────┘
← 2 panels side-by-side, uses full width →
```

### After (v186):
```
┌─────────────────────────────────────────┐
│ mlrVST                                  │
├─────────────────────────────────────────┤
│ [Global Controls] [Monome Device]       │
│  (active tab shown, full width)         │
└─────────────────────────────────────────┘
← Single tabbed panel, saves space →
```

## Implementation

### UI Structure Change

**PluginEditor.h:**
```cpp
// Old:
std::unique_ptr<MonomeControlPanel> monomeControl;
std::unique_ptr<GlobalControlPanel> globalControl;

// New:
std::unique_ptr<MonomeControlPanel> monomeControl;
std::unique_ptr<GlobalControlPanel> globalControl;
std::unique_ptr<juce::TabbedComponent> topTabs;  // Container for both
```

**PluginEditor.cpp - createUIComponents():**
```cpp
// Create control panels
monomeControl = std::make_unique<MonomeControlPanel>(audioProcessor);
globalControl = std::make_unique<GlobalControlPanel>(audioProcessor);

// Create TABBED top controls
topTabs = std::make_unique<juce::TabbedComponent>(TabsAtTop);
topTabs->addTab("Global Controls", Colour(0xff1a1a1a), globalControl.get(), false);
topTabs->addTab("Monome Device", Colour(0xff1a1a1a), monomeControl.get(), false);
topTabs->setCurrentTabIndex(0);  // Global Controls visible by default
addAndMakeVisible(*topTabs);
```

**PluginEditor.cpp - resized():**
```cpp
// Old: Split horizontally into two panels
auto monomeArea = topBar.removeFromLeft(width / 2);
auto globalArea = topBar;
monomeControl->setBounds(monomeArea);
globalControl->setBounds(globalArea);

// New: Single tabbed component
topTabs->setBounds(topBar);
```

## Tab Order

1. **Global Controls** (Default)
   - Master Volume
   - Input Monitor + Meters
   - Tempo
   - Crossfade Length
   - Quantize
   - Quality

2. **Monome Device**
   - Device Selection
   - Connection Status
   - Refresh/Connect buttons
   - Rotation setting

## Benefits

### Space Efficiency:
- **Before:** 2 panels × 50% width = limited control density
- **After:** 1 panel × 100% width = more room per panel

### User Experience:
- ✅ **Global Controls visible by default** (most commonly used)
- ✅ **Cleaner layout** (one panel at a time)
- ✅ **Full width for controls** (better spacing)
- ✅ **Easy switching** (single click on tab)

### Screen Real Estate:
```
Freed space can be used for:
- Larger strip controls
- More visible strips
- Better spacing
- Future features
```

## Visual Design

**Tab Styling:**
- Background: `#1a1a1a` (dark)
- Active tab: Lighter background
- Inactive tab: Darker, slightly transparent
- Tab text: White
- Clean modern appearance

**Tab Bar:**
- Positioned at top of panel
- Tabs aligned left
- Smooth transitions between tabs
- No borders (clean look)

## Workflow

### Typical Usage:
```
1. Open mlrVST
   → Global Controls visible (default)
   → Adjust tempo, quantize, etc.

2. Connect Monome (one-time setup)
   → Click "Monome Device" tab
   → Select device, connect
   → Return to Global Controls tab

3. Normal operation
   → Stay on Global Controls tab
   → Adjust settings as needed
```

### Quick Access:
- **Most common:** Global Controls always one click away (or already visible)
- **Setup tasks:** Monome Device one click away when needed

## Comparison with Bottom Tabs

The plugin now has TWO tabbed sections:

**Top Tabs (New in v186):**
- Global Controls (default)
- Monome Device

**Bottom Tabs (Existing):**
- Patterns
- Groups

This creates a logical hierarchy:
- **Top:** Global/device settings
- **Middle:** Main strip controls
- **Bottom:** Advanced features

## Technical Notes

### Panel Ownership:
The panels are still owned by the editor:
```cpp
std::unique_ptr<GlobalControlPanel> globalControl;
std::unique_ptr<MonomeControlPanel> monomeControl;
```

The tabs just **reference** them (not own them):
```cpp
topTabs->addTab("...", ..., globalControl.get(), false);
//                                              ↑
//                                      false = don't take ownership
```

### Memory Management:
- Panels created in createUIComponents()
- Tabs reference existing panels
- No double-deletion issues
- Clean destruction order

### Tab Switching:
- Instant (no animation)
- Preserves panel state
- No repainting overhead
- Efficient memory usage

## Future Enhancements

Could add more tabs if needed:
- MIDI settings
- Audio routing
- Preferences
- Shortcuts reference

## Summary

Monome Device and Global Controls are now in a space-saving tabbed interface at the top. Global Controls are visible by default, with Monome Device one click away. This frees up horizontal space and creates a cleaner, more organized interface.

**Key Change:** Side-by-side panels → Tabbed interface (saves space, cleaner UI)
