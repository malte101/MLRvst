# mlrVST v188 - Preset System

## Feature: 8-Preset Management System

Save and load complete mlrVST states including **all sample strips**, **audio files**, and **settings** with 8 preset slots.

## What Gets Saved

### Per Strip (all 8 strips):
- **Sample file path** (referenced, not embedded)
- Volume, Pan, Speed
- Loop start/end points
- Play mode (One-Shot, Loop, Gate, Reverse, Ping-Pong)
- Reverse state
- Group assignment
- Beats per loop setting
- Scratch amount

### Global Settings:
- Master volume
- Tempo
- Quantize setting
- Crossfade length

## Usage

### Saving a Preset:
```
1. Set up your strips with samples and settings
2. Hold SHIFT key
3. Click preset button (1-8)
4. Preset saved!
```

### Loading a Preset:
```
1. Click preset button (1-8)
2. All strips and settings restored!
```

### Visual Feedback:
- **Empty preset:** Dark gray button
- **Saved preset:** Cyan/blue button
- **Label:** "PRESETS (Shift+Click=Save)"

## File Format

### Location:
```
macOS: ~/Library/Application Support/mlrVST/Presets/
Windows: %APPDATA%/mlrVST/Presets/
Linux: ~/.config/mlrVST/Presets/
```

### File Structure:
```
Preset_1.mlrpreset
Preset_2.mlrpreset
...
Preset_8.mlrpreset
```

### Format (XML):
```xml
<mlrVSTPreset version="1.0" index="0">
  <Strip index="0">
    <samplePath>/path/to/sample.wav</samplePath>
    <volume>0.8</volume>
    <pan>0.0</pan>
    <speed>1.0</speed>
    <loopStart>0</loopStart>
    <loopEnd>16</loopEnd>
    <playMode>1</playMode>
    <reversed>false</reversed>
    <group>-1</group>
    <beatsPerLoop>4.0</beatsPerLoop>
    <scratchAmount>0.0</scratchAmount>
  </Strip>
  <!-- ... more strips ... -->
  <Globals>
    <masterVolume>0.7</masterVolume>
    <tempo>120.0</tempo>
    <quantize>5</quantize>
    <crossfadeLength>10.0</crossfadeLength>
  </Globals>
</mlrVSTPreset>
```

## Implementation

### Processor Methods:

**PluginProcessor.h:**
```cpp
void savePreset(int presetIndex);      // Save current state
void loadPreset(int presetIndex);      // Load preset state
String getPresetName(int index) const; // Get preset name
bool presetExists(int index) const;    // Check if saved
```

**PluginProcessor.cpp:**
```cpp
void MlrVSTAudioProcessor::savePreset(int index)
{
    // Create XML document
    XmlElement preset("mlrVSTPreset");
    
    // Save each strip
    for (int i = 0; i < MaxStrips; ++i)
    {
        auto* stripXml = preset.createNewChildElement("Strip");
        stripXml->setAttribute("samplePath", currentStripFiles[i].getFullPathName());
        stripXml->setAttribute("volume", strip->getVolume());
        // ... etc
    }
    
    // Save globals
    auto* globalsXml = preset.createNewChildElement("Globals");
    // ...
    
    // Write to file
    preset.writeTo(presetFile);
}
```

### UI Integration:

**GlobalControlPanel:**
```cpp
// 8 preset buttons in compact 2x4 grid
juce::TextButton presetButtons[8];

// Shift+Click = Save, Click = Load
presetButtons[i].onClick = [this, i]()
{
    if (ModifierKeys::getCurrentModifiers().isShiftDown())
        savePresetClicked(i);
    else
        loadPresetClicked(i);
};

// Visual feedback (cyan = saved, gray = empty)
void updatePresetButtons()
{
    bool exists = processor.presetExists(i);
    presetButtons[i].setColour(buttonColourId,
        exists ? Colour(0xff33ccff) : Colour(0xff1e1e1e));
}
```

### Layout:

```
GLOBAL CONTROLS
┌─────────────────────────────────────────────┐
│ M I LR Tempo Xfade Quant Quality  PRESETS   │
│ a n    120.0 10ms   1/8   Cubic  ┌─┬─┬─┬─┐ │
│ s u                                │1│2│3│4│ │
│ t t                                ├─┼─┼─┼─┤ │
│ e                                  │5│6│7│8│ │
│ r                                  └─┴─┴─┴─┘ │
└─────────────────────────────────────────────┘
                                    140px wide
                                    2 rows x 4 cols
                                    32px buttons
```

## Button States

### Visual Design:
```
Empty Preset:
┌────┐
│ 1  │  Dark gray (#1e1e1e)
└────┘  Gray text (#888888)

Saved Preset:
┌────┐
│ 1  │  Cyan (#33ccff)
└────┘  Black text (high contrast)
```

### User Feedback:
- **Hover:** Pointer cursor
- **Click:** Load preset (or save if Shift held)
- **Color change:** Instant visual confirmation

## Sample File Handling

### Reference Not Embed:
Presets store **file paths**, not audio data:
- **Pros:** Small preset files, instant save/load
- **Cons:** Samples must remain in original location

### Missing Samples:
If a sample file is moved/deleted:
- Preset loads other strips normally
- Missing strip stays empty
- No error message (graceful degradation)

### Cross-Platform Paths:
- Absolute paths stored
- Works across macOS/Windows/Linux
- Portable if samples are in standard locations

## Use Cases

### Live Performance:
```
Preset 1: Intro/Verse loops
Preset 2: Chorus/Drop
Preset 3: Bridge/Breakdown
Preset 4: Outro
Presets 5-8: Variations/B-sections
```

### Production Workflow:
```
Preset 1: Drums
Preset 2: Bass
Preset 3: Melody
Preset 4: FX/Atmosphere
Preset 5: Full mix
Presets 6-8: Alternate takes
```

### Sound Design:
```
Preset 1: Source material
Presets 2-8: Progressive transformations
Easy A/B comparison
Quick iteration
```

## Workflow Example

### Creating a Set:
```
1. Load samples into strips 1-3
2. Set loop points, adjust volumes
3. Shift+Click button "1" → Saved!
4. Load different samples
5. Shift+Click button "2" → Saved!
6. Repeat for presets 3-8
```

### Live Performance:
```
Song structure:
- Click "1" → Intro loads
- Click "2" → Verse loads
- Click "3" → Chorus loads
- Click "1" → Verse 2 loads
- Click "3" → Chorus 2 loads
- Click "4" → Outro loads

Instant, seamless scene changes!
```

## Technical Notes

### Preset Directory:
Created automatically on first save:
```cpp
auto presetDir = File::getSpecialLocation(userApplicationDataDirectory)
    .getChildFile("mlrVST").getChildFile("Presets");

if (!presetDir.exists())
    presetDir.createDirectory();
```

### Atomic Saves:
XML written directly to file (not using temp file):
```cpp
preset.writeTo(presetFile);
```

### Load Safety:
- Invalid XML ignored gracefully
- Missing strips skipped
- Invalid parameters use defaults
- No crashes on corrupt files

### Parameter Normalization:
Some parameters need normalization for APVTS:
```cpp
// Tempo: 20-300 range
param->setValueNotifyingHost(tempo / 300.0);

// Quantize: 0-9 choices
param->setValueNotifyingHost(quantize / 9.0f);

// Crossfade: 1-50ms range  
param->setValueNotifyingHost(crossfade / 50.0);
```

## Future Enhancements

### Possible Additions:
- **Preset names** (user-editable text)
- **Preset import/export** (share with others)
- **Embed samples** (option for portable presets)
- **MIDI learn** (map CC to preset switching)
- **Morph between presets** (crossfade preset states)
- **Auto-save** (remember last state on close)

### Extended Slots:
Could expand to 16/32/64 presets with:
- Bank system
- Scrolling list
- Search/filter
- Categories/tags

## Summary

The preset system allows saving/loading complete mlrVST states with 8 slots. Presets store all strip settings and sample paths in XML format. Use **Shift+Click to save**, **Click to load**. Saved presets show in cyan, empty presets in gray.

**Key Features:**
- 8 preset slots
- Save: Shift+Click
- Load: Click
- Complete state capture
- Sample paths referenced
- XML format (.mlrpreset)
- Visual feedback (color coding)
- Compact 2x4 button grid
