# Complete Rebuild Instructions for CLion

## Problem: CLion is Using Cached/Old Files

CLion caches compiled binaries and build artifacts. Even when you extract a new version, it may still be loading the OLD compiled plugin from cache.

## Solution: Complete Clean and Rebuild

### Step 1: Run the Clean Script
```bash
cd mlrVST-modern
./clean_all.sh
```

This removes:
- All build directories (build/, Builds/, cmake-build-*)
- CLion caches (.idea/, cmake-build-debug/, cmake-build-release/)
- JUCE build artifacts (JuceLibraryCode/)
- All compiled files (*.o, *.a, *.so, *.dylib, *.vst3)
- System VST3 plugin caches

### Step 2: In CLion - Invalidate Caches
1. **File → Invalidate Caches / Restart**
2. Select "Invalidate and Restart"
3. Wait for CLion to restart

### Step 3: In CLion - Clean Build
1. **Build → Clean** (wait for it to complete)
2. **Build → Rebuild Project**

### Step 4: Verify New Build
Check the timestamp on the compiled plugin to ensure it's fresh:
```bash
# On macOS:
ls -lh ~/Library/Audio/Plug-Ins/VST3/mlrVST.vst3

# On Linux:
ls -lh ~/.vst3/mlrVST.vst3
```

The timestamp should be RECENT (just now).

### Alternative: Command Line Build (Bypass CLion)
If CLion keeps using cached files, build from command line:

```bash
cd mlrVST-modern
./clean_all.sh
make clean
make
```

Then manually copy the plugin to your VST3 folder:
```bash
# macOS:
cp -r build/mlrVST_artefacts/Release/VST3/mlrVST.vst3 ~/Library/Audio/Plug-Ins/VST3/

# Linux:
cp -r build/mlrVST_artefacts/Release/VST3/mlrVST.vst3 ~/.vst3/
```

## Still Crashing After Clean Build?

If it STILL crashes after a complete clean build, the issue is in the CODE itself, not cached files.

In that case, we need to:
1. Run with debugger to get stack trace
2. Check which exact line is crashing
3. Fix the actual code issue

## Testing if Cache Was the Problem

After clean rebuild:
- ✅ If it works → Cache was the problem
- ❌ If still crashes → Code issue (need stack trace)
