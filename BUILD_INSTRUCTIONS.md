# Build Instructions

## The Problem We're Solving

The preprocessor directive `#if JucePlugin_Build_Standalone` should prevent standalone code from compiling in VST/AU builds, but it appears to not be working correctly in your build environment.

## Solution: Clean Build from Scratch

```bash
# 1. Remove ALL old build artifacts
rm -rf build/
rm -rf cmake-build-debug/
rm -rf cmake-build-release/
rm -rf ~/Library/Audio/Plug-Ins/VST3/mlrVST.vst3
rm -rf ~/Library/Audio/Plug-Ins/Components/mlrVST.component
rm -rf /Library/Audio/Plug-Ins/VST3/mlrVST.vst3
rm -rf /Library/Audio/Plug-Ins/Components/mlrVST.component

# 2. Clean build
cmake -B build -DCMAKE_BUILD_TYPE=Release

# 3. Build
cmake --build build --config Release

# 4. Install (requires sudo for system-wide paths)
sudo cmake --build build --target install
```

## What Changed in v337

Added **runtime check** in addition to compile-time check:

```cpp
#if JucePlugin_Build_Standalone
    if (getPlayHead() == nullptr)  // Runtime check - VST always has playHead
    {
        // Standalone transport code here
    }
#endif
```

This provides double protection:
1. **Compile-time:** `#if` directive (should work but apparently doesn't)
2. **Runtime:** `getPlayHead() == nullptr` (VST always has a playhead, standalone doesn't)

## Verify It's Working

After building, check which binary is which:

```bash
# VST3 should NOT have standalone code
strings build/mlrVST_artefacts/Release/VST3/mlrVST.vst3/Contents/MacOS/mlrVST | grep -i standalone

# Standalone SHOULD have standalone code
strings build/mlrVST_artefacts/Release/Standalone/mlrVST.app/Contents/MacOS/mlrVST | grep -i standalone
```

If the VST3 shows "standalone" strings, the builds are mixing.
