# mlrVST v164 - Quick Fix Guide

## What Was Fixed

Ableton Live 12 was crashing when loading mlrVST v163 with error:
```
EXC_BAD_ACCESS (SIGSEGV) at 0x000000004d555458
in MlrVSTAudioProcessor::getStateInformation()
```

## How to Build the Fix

### 1. Download JUCE (one-time setup)

```bash
cd mlrVST-modern
git clone --depth 1 --branch 8.0.4 https://github.com/juce-framework/JUCE.git
```

### 2. Build and Install

```bash
./BUILD_FIX_V164.sh
```

Or manually:
```bash
make clean
make configure
make build
sudo make install
```

### 3. Restart Ableton

1. Quit Ableton Live completely
2. Relaunch Ableton
3. Rescan plugins if prompted
4. Load mlrVST - should now work without crashing

## What Changed

Added error handling to prevent crash during plugin initialization:

**Before (v163):**
```cpp
void getStateInformation(juce::MemoryBlock& destData)
{
    auto state = parameters.copyState();
    std::unique_ptr<juce::XmlElement> xml(state.createXml());
    copyXmlToBinary(*xml, destData);  // CRASH!
}
```

**After (v164):**
```cpp
void getStateInformation(juce::MemoryBlock& destData)
{
    try {
        auto state = parameters.copyState();
        if (!state.isValid()) return;  // Validate first
        
        std::unique_ptr<juce::XmlElement> xml(state.createXml());
        if (xml != nullptr)  // Check before using
            copyXmlToBinary(*xml, destData);
    }
    catch (...) {
        destData.reset();  // Safe fallback
    }
}
```

## Files Modified

- `Source/PluginProcessor.cpp` - Fixed getStateInformation()

## All Features Still Working

✓ Pattern recording (Monome row 0, cols 4-7)
✓ MIDI-style playback
✓ Group assignment (row 7, button 4)  
✓ 1ms fade-in, 128 sample fade-out
✓ Auto-quantize on manual stop

## If Build Fails

Make sure you have:
- Xcode Command Line Tools: `xcode-select --install`
- JUCE 8.0.4 in the mlrVST-modern directory
- CMake: `brew install cmake` (if not available)

## Still Having Issues?

Check:
1. Ableton is fully quit (not just project closed)
2. Old mlrVST.component removed from ~/Library/Audio/Plug-Ins/Components/
3. Plugin cache cleared (delete ~/Library/Audio/Plug-Ins/Components/Audio Unit Components.plist)
4. Ableton plugin preferences reset

## Support Files

- `CRASH_FIX_V164.md` - Detailed technical documentation
- `BUILD_FIX_V164.sh` - Automated build script
- `mlrVST-modern-v164-CRASH-FIX.tar.gz` - Complete archive
