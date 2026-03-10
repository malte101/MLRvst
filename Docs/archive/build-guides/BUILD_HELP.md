# mlrVST v167 - Build Instructions

## Quick Start (macOS)

Since the archive doesn't include JUCE and you need network access to download it, here's what to do:

### Option 1: Download JUCE First (Recommended)

On a machine with internet access:

```bash
# 1. Download JUCE
git clone --depth 1 --branch 8.0.4 https://github.com/juce-framework/JUCE.git

# 2. Copy it to your mlrVST-modern directory
cp -r JUCE /path/to/mlrVST-modern/

# 3. Now you can build
cd /path/to/mlrVST-modern
make clean
make
```

### Option 2: Manual Build with Xcode

If you already have a JUCE installation elsewhere:

```bash
# 1. Create symbolic link to your JUCE installation
ln -s /path/to/your/JUCE JUCE

# 2. Generate Xcode project using Projucer
/path/to/JUCE/extras/Projucer/Builds/MacOSX/build/Release/Projucer.app/Contents/MacOS/Projucer --resave mlrVST.jucer

# 3. Build with xcodebuild
cd Builds/MacOSX
xcodebuild -configuration Release -project mlrVST.xcodeproj

# 4. Install
sudo cp -r build/Release/mlrVST.vst3 ~/Library/Audio/Plug-Ins/VST3/
sudo cp -r build/Release/mlrVST.component ~/Library/Audio/Plug-Ins/Components/
```

### Option 3: Use Projucer GUI

1. Download JUCE from https://juce.com/get-juce/
2. Extract JUCE to the mlrVST-modern directory
3. Open `mlrVST.jucer` in Projucer
4. Click "Save and Open in IDE"
5. Build in Xcode

## The Problem

The Makefile requires:
- CMake
- JUCE framework

You're getting the "No rule to make target 'configure'" error because:
- The Makefile expects to use CMake
- But you might not have CMake installed, or
- JUCE isn't present in the directory

## Quick Fix

The simplest solution is to get JUCE once:

```bash
# On a machine with internet:
cd mlrVST-modern
git clone --depth 1 --branch 8.0.4 https://github.com/juce-framework/JUCE.git
tar -czf mlrVST-with-JUCE.tar.gz .

# Transfer mlrVST-with-JUCE.tar.gz to your build machine
# Then extract and build:
tar -xzf mlrVST-with-JUCE.tar.gz
make
```

## What Each File Does

- **mlrVST.jucer** - Project configuration (open in Projucer)
- **CMakeLists.txt** - CMake build configuration
- **Makefile** - Wrapper around CMake
- **Source/** - C++ source code
- **JUCE/** - (not included) Framework needed to build

## Dependencies

Required:
- macOS 10.13+
- Xcode Command Line Tools: `xcode-select --install`
- JUCE 8.0.4

Optional:
- CMake (for using the Makefile)
- Projucer (comes with JUCE)

## After Building

Your plugins will be in:
- `Builds/MacOSX/build/Release/mlrVST.vst3`
- `Builds/MacOSX/build/Release/mlrVST.component`

Install with:
```bash
sudo cp -r Builds/MacOSX/build/Release/mlrVST.vst3 ~/Library/Audio/Plug-Ins/VST3/
sudo cp -r Builds/MacOSX/build/Release/mlrVST.component ~/Library/Audio/Plug-Ins/Components/
```

Then restart your DAW and rescan plugins.

## If You Get Stuck

The source code is all there in the `Source/` directory. The changes in v167 are:

1. Added `inputMonitor` parameter in `PluginProcessor.cpp`
2. Added input monitoring in `AudioEngine.cpp`
3. All previous features (clock sync, crash fix, etc.) are included

You just need JUCE to compile it!
