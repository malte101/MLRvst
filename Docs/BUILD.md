# Build Instructions

Complete guide to building mlrVST Modern Edition on all platforms.

## Table of Contents

- [Prerequisites](#prerequisites)
- [Getting JUCE](#getting-juce)
- [Building on macOS](#building-on-macos)
- [Building on Linux](#building-on-linux)
- [Building on Windows](#building-on-windows)
- [Build Options](#build-options)
- [Troubleshooting](#troubleshooting)

## Prerequisites

### All Platforms

- **CMake** 3.22 or later
- **Git** (for cloning repositories)
- **C++17 compatible compiler**

### macOS

- Xcode 13 or later
- Command Line Tools: `xcode-select --install`

### Linux (Ubuntu/Debian)

```bash
sudo apt-get update
sudo apt-get install -y \
    build-essential \
    cmake \
    git \
    pkg-config \
    libasound2-dev \
    libjack-jackd2-dev \
    libfreetype6-dev \
    libx11-dev \
    libxext-dev \
    libxrandr-dev \
    libxinerama-dev \
    libxcursor-dev \
    libgl1-mesa-dev \
    libglu1-mesa-dev
```

### Linux (Fedora/RHEL)

```bash
sudo dnf install -y \
    gcc-c++ \
    cmake \
    git \
    alsa-lib-devel \
    jack-audio-connection-kit-devel \
    freetype-devel \
    libX11-devel \
    libXext-devel \
    libXrandr-devel \
    libXinerama-devel \
    libXcursor-devel \
    mesa-libGL-devel \
    mesa-libGLU-devel
```

### Windows

- **Visual Studio 2019 or later** with C++ Desktop Development
- **CMake** (download from cmake.org)
- **Git for Windows**

Or use **MSYS2/MinGW**:
```bash
pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake make
```

## Getting JUCE

### Option 1: Git Clone (Recommended)

```bash
cd mlrVST-modern
git clone https://github.com/juce-framework/JUCE.git
```

### Option 2: Download Release

1. Visit https://github.com/juce-framework/JUCE/releases
2. Download JUCE 8.0.4 or later
3. Extract to `mlrVST-modern/JUCE/`

### Option 3: Git Submodule

```bash
git submodule add https://github.com/juce-framework/JUCE.git
git submodule update --init --recursive
```

## Building on macOS

### Quick Build (Makefile)

```bash
# From repository root
make

# Build specific targets
make vst3        # VST3 only
make au          # Audio Unit only
make standalone  # Standalone app

# Install to system
make install
```

### Manual Build (CMake)

```bash
mkdir Build && cd Build

# Configure
cmake .. -DCMAKE_BUILD_TYPE=Release

# Build
cmake --build . --config Release -j8

# Install
sudo cmake --install .
```

### Xcode Project

```bash
mkdir Build && cd Build
cmake .. -G "Xcode"
open mlrVST.xcodeproj
```

### Installation Locations

After `make install`:
- **VST3**: `~/Library/Audio/Plug-Ins/VST3/mlrVST.vst3`
- **AU**: `~/Library/Audio/Plug-Ins/Components/mlrVST.component`
- **Standalone**: `Build/mlrVST_artefacts/Release/Standalone/mlrVST.app`

## Building on Linux

### Quick Build (Makefile)

```bash
# From repository root
make

# Install to system
make install
```

### Manual Build (CMake)

```bash
mkdir Build && cd Build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j$(nproc)

# Install (may need sudo)
sudo cmake --install .
```

### Installation Locations

After `make install`:
- **VST3**: `~/.vst3/mlrVST.vst3`
- **Standalone**: `Build/mlrVST_artefacts/Release/Standalone/mlrVST`

### Desktop Entry (Optional)

Create `~/.local/share/applications/mlrVST.desktop`:

```desktop
[Desktop Entry]
Type=Application
Name=mlrVST
Exec=/path/to/mlrVST
Icon=audio-x-generic
Categories=AudioVideo;Audio;
```

## Building on Windows

### Visual Studio (Recommended)

```cmd
REM Open "Developer Command Prompt for VS 2019"
cd mlrVST-modern
mkdir Build
cd Build

REM Configure
cmake .. -G "Visual Studio 16 2019" -A x64

REM Build
cmake --build . --config Release

REM Install (run as Administrator)
cmake --install .
```

### MinGW/MSYS2

```bash
mkdir Build && cd Build
cmake .. -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build . -j8
```

### Installation Locations

- **VST3**: `C:\Program Files\Common Files\VST3\mlrVST.vst3`
- **Standalone**: `Build\mlrVST_artefacts\Release\Standalone\mlrVST.exe`

## Build Options

### Configuration Types

```bash
# Debug build (with symbols)
make CONFIG=Debug

# Release build (optimized)
make CONFIG=Release
```

### Verbose Output

```bash
# Show full compile commands
make VERBOSE=1
```

### Parallel Builds

```bash
# Auto-detect cores
make

# Specific number of cores
cmake --build Build -j4
```

### Build Specific Formats

```bash
# CMake targets
cmake --build Build --target mlrVST_VST3
cmake --build Build --target mlrVST_AU
cmake --build Build --target mlrVST_Standalone

# Makefile targets
make vst3
make au
make standalone
```

### Custom JUCE Location

If JUCE is in a different location:

```bash
cmake .. -DJUCE_DIR=/path/to/JUCE
```

### Code Signing (macOS)

Edit `CMakeLists.txt`:

```cmake
set_target_properties(mlrVST_VST3 PROPERTIES
    XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "Developer ID Application"
    XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "YOUR_TEAM_ID"
)
```

## Troubleshooting

### CMake Can't Find JUCE

**Problem**: `CMake Error: Could not find JUCE`

**Solution**:
```bash
# Ensure JUCE is in correct location
ls JUCE/modules/juce_core  # Should exist

# Or specify path
cmake .. -DJUCE_DIR=/path/to/JUCE
```

### Compiler Not Found

**Problem**: `CMake Error: CMAKE_CXX_COMPILER not found`

**macOS**:
```bash
xcode-select --install
```

**Linux**:
```bash
sudo apt-get install build-essential
```

**Windows**:
- Install Visual Studio with C++ Desktop Development
- Or install MinGW/MSYS2

### Missing Dependencies (Linux)

**Problem**: Build fails with missing X11 or ALSA headers

**Solution**:
```bash
# Ubuntu/Debian
sudo apt-get install libasound2-dev libx11-dev libfreetype6-dev

# Fedora
sudo dnf install alsa-lib-devel libX11-devel freetype-devel
```

### VST3 SDK Not Found

**Problem**: `Could not find VST3 SDK`

**Solution**: This should be included with JUCE 8.x automatically. If not:
```bash
cd JUCE
git pull origin master  # Update to latest
```

### Out of Memory During Build

**Problem**: Build fails with memory error

**Solution**:
```bash
# Reduce parallel jobs
cmake --build Build -j2

# Or use Makefile
make NPROC=2
```

### Permission Denied (Install)

**Problem**: `Permission denied` during install

**macOS/Linux**:
```bash
sudo make install
# Or
sudo cmake --install Build
```

**Windows**:
- Run Command Prompt as Administrator

### Build Artifacts Not Found

**Problem**: Can't find built plugins

**Solution**:
```bash
# Check build directory
ls -la Build/mlrVST_artefacts/Release/

# Ensure build completed
make 2>&1 | tee build.log
```

### Linker Errors

**Problem**: Undefined symbols during linking

**Solution**:
```bash
# Clean and rebuild
make distclean
make

# Check compiler version
gcc --version  # Should be 9+
clang --version  # Should be 10+
```

## Advanced Build Options

### Static vs Shared Linking

```cmake
# In CMakeLists.txt, add:
set(JUCE_BUILD_SHARED_CODE ON)  # Shared linking
```

### Custom Optimization Flags

```bash
# Release with debug symbols
cmake .. -DCMAKE_BUILD_TYPE=RelWithDebInfo

# Profile-guided optimization
cmake .. -DCMAKE_CXX_FLAGS="-fprofile-generate"
# ... run and profile ...
cmake .. -DCMAKE_CXX_FLAGS="-fprofile-use"
```

### Cross-Compilation

```bash
# Example: Build for ARM on x86
cmake .. -DCMAKE_TOOLCHAIN_FILE=arm-toolchain.cmake
```

## Build Verification

After building, verify:

```bash
# Check plugin loads in DAW
# macOS: auval for AU validation
auval -v aufx Mlrv Mlrx

# Check symbols
nm Build/mlrVST_artefacts/Release/VST3/mlrVST.vst3/Contents/MacOS/mlrVST

# Check dependencies (macOS)
otool -L Build/mlrVST_artefacts/Release/VST3/mlrVST.vst3/Contents/MacOS/mlrVST

# Check dependencies (Linux)
ldd Build/mlrVST_artefacts/Release/VST3/mlrVST.vst3/Contents/x86_64-linux/mlrVST.so
```

## Clean Build

```bash
# Clean build directory
make clean

# Remove all artifacts
make distclean

# Complete fresh build
make distclean && make
```

## Getting Help

If you encounter issues:

1. Check the [Troubleshooting](#troubleshooting) section above
2. Review build log: `make 2>&1 | tee build.log`
3. Check JUCE version: Should be 8.0.4+
4. Verify CMake version: Should be 3.22+
5. Open an issue on GitHub with:
   - OS and version
   - Compiler and version
   - Full build log
   - CMake output

## Next Steps

After successful build:

1. Read [README.md](../README.md) for usage instructions
2. Check [AUDIO_ENGINE.md](AUDIO_ENGINE.md) for audio features
3. Review [SERIALOSC.md](SERIALOSC.md) for monome setup
4. Install serialosc from [monome.org](https://monome.org/docs/serialosc/setup/)
