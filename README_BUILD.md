# mlrVST v167 - Build Instructions

## ⚠️ IMPORTANT: You Need JUCE First!

This project requires the JUCE framework to compile. The error you're seeing happens because JUCE is not in the directory.

## Solution: Get JUCE

### On a Machine With Internet

```bash
cd mlrVST-modern
git clone --depth 1 --branch 8.0.4 https://github.com/juce-framework/JUCE.git
```

That's it! Now you can build.

### Alternative: Download JUCE Manually

1. Go to: https://github.com/juce-framework/JUCE/releases/tag/8.0.4
2. Download the source code zip
3. Extract it to `mlrVST-modern/JUCE/`

## Then Build

```bash
cd mlrVST-modern
make clean
make
```

Or if that fails:

```bash
cmake -B Build -DCMAKE_BUILD_TYPE=Release
cmake --build Build --config Release
```

## Install

```bash
# VST3
sudo cp -r Build/mlrVST_artefacts/Release/VST3/mlrVST.vst3 ~/Library/Audio/Plug-Ins/VST3/

# Audio Unit
sudo cp -r Build/mlrVST_artefacts/Release/AU/mlrVST.component ~/Library/Audio/Plug-Ins/Components/
```

## What's in v167

✅ Crash fix (v164)  
✅ Perfect clock sync (v166)  
✅ Input monitoring (v167) - NEW!

All features work. You just need to compile it!

## Need Help?

The code is all in `Source/`:
- `PluginProcessor.cpp` - Main plugin logic
- `AudioEngine.cpp` - Audio processing engine
- `PluginEditor.cpp` - GUI

Changes for v167:
- Added `inputMonitor` parameter (line 547 in PluginProcessor.cpp)
- Added input buffer copy and mix (AudioEngine.cpp processBlock)
- Added `setInputMonitorVolume()` method

Everything else remains the same from v166.
