# CLion Build Guide - Finding Your Bundles

## The Problem

CLion with CMake doesn't show bundles as "files" in the project tree because they're actually **directories** (bundles are folders on macOS).

## Where Are The Bundles?

### Default CMake Build Directory

When you build in CLion, it creates bundles here:

```
mlrVST-modern/
├── cmake-build-debug/           ← Debug builds
│   └── mlrVST_artefacts/
│       └── Debug/
│           ├── VST3/
│           │   └── mlrVST.vst3/      ← VST3 BUNDLE (folder)
│           ├── AU/
│           │   └── mlrVST.component/ ← AU BUNDLE (folder, macOS)
│
└── cmake-build-release/         ← Release builds
    └── mlrVST_artefacts/
        └── Release/
            ├── VST3/
            │   └── mlrVST.vst3/      ← VST3 BUNDLE (folder)
            ├── AU/
            │   └── mlrVST.component/ ← AU BUNDLE (folder, macOS)
```

**Key Point:** These are **folders**, not files. They won't show in CLion's file tree by default.

## How to Find Them

### Method 1: Terminal (Fastest)

```bash
cd mlrVST-modern

# For Debug builds
ls -la cmake-build-debug/mlrVST_artefacts/Debug/VST3/
ls -la cmake-build-debug/mlrVST_artefacts/Debug/AU/

# For Release builds
ls -la cmake-build-release/mlrVST_artefacts/Release/VST3/
ls -la cmake-build-release/mlrVST_artefacts/Release/AU/
```

### Method 2: Finder

1. In Finder, navigate to your mlrVST-modern folder
2. Go to `cmake-build-release/mlrVST_artefacts/Release/`
3. You'll see:
   - `VST3/` folder → contains `mlrVST.vst3` bundle
   - `AU/` folder → contains `mlrVST.component` bundle (macOS)

### Method 3: From CLion

1. Right-click on project root in CLion
2. Select "Open In → Finder"
3. Navigate to build folder as above

## CLion Build Configuration

### Step 1: Select Build Type

In CLion's top toolbar:
```
[Debug ▼]  [mlrVST_VST3 ▼]  [▶ Build]
    ↑           ↑
    |           └─ Build target (select format target)
    └─ Build type (select "Release" for final builds)
```

**Change to Release:**
- Click dropdown
- Select "Release"
- CMake will reconfigure

### Step 2: Select Build Target

Click the target dropdown and choose:
- `mlrVST_VST3` - Builds VST3 only
- `mlrVST_AU` - Builds AU only

### Step 3: Build

Click the hammer icon or:
- **Build Project:** Cmd+F9 (Mac) / Ctrl+F9 (Windows)
- **Rebuild All:** Clean + Build

## Checking If Build Succeeded

### In CLion Build Output

Look for these messages:
```
[100%] Built target mlrVST_VST3
[100%] Built target mlrVST_AU
```

If you see these, the bundles **are** built!

### Verify Bundle Contents

```bash
# Check VST3 bundle structure
ls -la cmake-build-release/mlrVST_artefacts/Release/VST3/mlrVST.vst3/Contents/
# Should show: Info.plist, PkgInfo, MacOS/, Resources/

# Check AU bundle structure  
ls -la cmake-build-release/mlrVST_artefacts/Release/AU/mlrVST.component/Contents/
# Should show: Info.plist, PkgInfo, MacOS/, Resources/
```

## Installation

### Manual Installation

Install from the build output directory:

```bash
cd mlrVST-modern/cmake-build-release/mlrVST_artefacts/Release

# Install VST3
cp -r VST3/mlrVST.vst3 ~/Library/Audio/Plug-Ins/VST3/

# Install AU
cp -r AU/mlrVST.component ~/Library/Audio/Plug-Ins/Components/
```

**Important:** Use `cp -r` to copy the entire bundle folder!

## Common Issues

### Issue 1: "No such file or directory"

**Cause:** Build hasn't run yet or build failed

**Solution:**
1. Check CLion Build Output for errors
2. Make sure JUCE is cloned: `ls JUCE/CMakeLists.txt`
3. Rebuild: Build → Rebuild Project

### Issue 2: "Build succeeded but can't find bundles"

**Cause:** Looking in wrong build directory

**Solution:**
```bash
# Find all built bundles
find . -name "mlrVST.vst3" -type d
find . -name "mlrVST.component" -type d
```

### Issue 3: "DAW doesn't see the plugin"

**Cause:** Bundle installed to wrong location or DAW cache

**Solution:**
1. Verify installation:
   ```bash
   ls ~/Library/Audio/Plug-Ins/VST3/mlrVST.vst3
   ls ~/Library/Audio/Plug-Ins/Components/mlrVST.component
   ```

2. Clear DAW plugin cache:
   - **Ableton:** Delete plugin cache and rescan
   - **Logic:** Logic Pro → Preferences → Plug-In Manager → Reset & Rescan
   
3. Restart DAW completely (don't just close project)

### Issue 4: CLion shows errors in JUCE code

**Cause:** Normal - JUCE uses advanced C++ features

**Solution:**
- Ignore red squiggles in JUCE headers
- If YOUR code has errors, fix those
- JUCE warnings are normal and safe to ignore

## CLion Specific Settings

### Enable Bundle Visibility (Optional)

To see bundles in CLion file tree:

1. View → Tool Windows → Project
2. Click gear icon (⚙️)
3. Uncheck "Hide Empty Middle Packages"
4. You still won't see bundles (they're generated files)

### See Build Output Directory

1. Settings → Build, Execution, Deployment → CMake
2. Check "Build directory" path
3. Default is usually `cmake-build-{type}`

### Change Build Directory

In CMake settings, set custom build directory:
```
Build directory: ${PROJECT_DIR}/build
```

Then bundles will be in:
```
mlrVST-modern/build/mlrVST_artefacts/Release/
```

## Quick Verification Script

Save this as `check_bundles.sh` in mlrVST-modern folder:

```bash
#!/bin/bash

echo "Checking for built bundles..."
echo ""

# Check Debug builds
if [ -d "cmake-build-debug/mlrVST_artefacts/Debug/VST3/mlrVST.vst3" ]; then
    echo "✓ Debug VST3 found"
else
    echo "✗ Debug VST3 not found"
fi

if [ -d "cmake-build-debug/mlrVST_artefacts/Debug/AU/mlrVST.component" ]; then
    echo "✓ Debug AU found"
else
    echo "✗ Debug AU not found"
fi

# Check Release builds
if [ -d "cmake-build-release/mlrVST_artefacts/Release/VST3/mlrVST.vst3" ]; then
    echo "✓ Release VST3 found"
else
    echo "✗ Release VST3 not found"
fi

if [ -d "cmake-build-release/mlrVST_artefacts/Release/AU/mlrVST.component" ]; then
    echo "✓ Release AU found"
else
    echo "✗ Release AU not found"
fi

echo ""
echo "Checking system installation..."

if [ -d "$HOME/Library/Audio/Plug-Ins/VST3/mlrVST.vst3" ]; then
    echo "✓ VST3 installed to system"
else
    echo "✗ VST3 not installed to system"
fi

if [ -d "$HOME/Library/Audio/Plug-Ins/Components/mlrVST.component" ]; then
    echo "✓ AU installed to system"
else
    echo "✗ AU not installed to system"
fi
```

Run it:
```bash
chmod +x check_bundles.sh
./check_bundles.sh
```

## Expected Build Output

When CLion successfully builds, you should see:

```
Scanning dependencies of target mlrVST_VST3
[ 33%] Building CXX object...
[ 66%] Linking CXX shared library...
[ 66%] Built target mlrVST_VST3

Scanning dependencies of target mlrVST_AU
[ 77%] Building CXX object...
[ 88%] Linking CXX shared library...
[ 88%] Built target mlrVST_AU

[100%] Build finished
```

## TL;DR - Quick Check

```bash
# Go to project folder
cd /path/to/mlrVST-modern

# Check if bundles exist
ls cmake-build-release/mlrVST_artefacts/Release/VST3/
ls cmake-build-release/mlrVST_artefacts/Release/AU/

# If you see "mlrVST.vst3" and "mlrVST.component" - SUCCESS!
# If not, rebuild in CLion (Cmd+F9)
```

The bundles **ARE** being created - they're just folders, not visible as "files" in the IDE!
