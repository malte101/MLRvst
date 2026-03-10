# Build Guide

This is the canonical build guide for the current `mlrVST` repo.

The project builds with CMake. The `Makefile` is a convenience wrapper for macOS and Linux, but the CMake flow below is the source of truth because it matches the active targets, helper scripts, and packaging steps.

## Supported Outputs

- macOS: `VST3` and `AU`
- Linux: `VST3`
- Windows: `VST3`

Notes:

- Windows builds must use `MSVC` or `clang-cl` with the Windows SDK.
- `MinGW` and `MSYS2` are not supported by the current `JUCE 8` build path.
- Plugin bundles are written to `Build/mlrVST_artefacts/<Config>/...`.
- Copying into system or user plugin folders is a separate explicit step.

## Prerequisites

### All platforms

- `CMake 3.22+`
- `Git`
- A `C++17` compiler
- A local `JUCE 8` checkout

The project looks for JUCE in `./JUCE` by default. If your checkout lives somewhere else, pass:

```bash
-DMLRVST_JUCE_PATH=/path/to/JUCE
```

### macOS

- `Xcode 13+`
- Command Line Tools: `xcode-select --install`

### Linux (Debian/Ubuntu)

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

- `Visual Studio 2019+` with Desktop C++ tools
- `PowerShell 5+` if you want to use the release packager

## JUCE Setup

If you keep JUCE inside the repo:

```bash
git clone https://github.com/juce-framework/JUCE.git
```

If you keep JUCE elsewhere, configure with `MLRVST_JUCE_PATH`:

```bash
cmake -S . -B Build -DMLRVST_JUCE_PATH=/path/to/JUCE
```

The `Makefile` only checks for `./JUCE`, so the direct CMake flow is the flexible option when JUCE is external.

## Optional Native Dependencies

The project can build without all optional analysis and stretch backends.

- `SoundTouch`: enabled only if headers and libraries are found
- `Bungee`: enabled only if headers and libraries are found
- `LibPyin`: enabled only if `third_party/LibPyin` is present
- Native `Essentia`: enabled only if the configured prefix contains the library
- `Huovilainen`: disabled by default and opt-in only

If the optional native dependencies are missing, the build still succeeds with reduced backend coverage.

Repo-local native prefixes live under `third_party/_native/`. On macOS, you can populate them with:

```bash
./scripts/bootstrap_native_deps.sh
```

That bootstrap script is macOS-oriented. On other platforms, point CMake at existing installs instead:

```bash
-DMLRVST_ESSENTIA_PREFIX=/path/to/essentia/prefix
-DMLRVST_BUNGEE_PREFIX=/path/to/bungee/prefix
```

## Canonical Build Flow

From the repo root:

```bash
cmake -S . -B Build -DCMAKE_BUILD_TYPE=Release
cmake --build Build --config Release
```

Common outputs:

- macOS VST3: `Build/mlrVST_artefacts/Release/VST3/mlrVST.vst3`
- macOS AU: `Build/mlrVST_artefacts/Release/AU/mlrVST.component`
- Linux VST3: `Build/mlrVST_artefacts/Release/VST3/mlrVST.vst3`
- Windows VST3: `Build/mlrVST_artefacts/Release/VST3/mlrVST.vst3`

On single-config generators, add parallelism as needed:

```bash
cmake --build Build --parallel
```

## Platform Notes

### macOS

Build:

```bash
cmake -S . -B Build -DCMAKE_BUILD_TYPE=Release
cmake --build Build --config Release
```

Optional Xcode project:

```bash
cmake -S . -B Build -G Xcode
open Build/mlrVST.xcodeproj
```

Install plugin bundles:

```bash
cmake --build Build --target install_plugins_user
cmake --build Build --target install_plugins_system
```

Or use the helper script directly:

```bash
./scripts/install_macos_plugins.sh --user --build-dir Build
./scripts/install_macos_plugins.sh --system --build-dir Build
```

Install paths:

- User scope: `~/Library/Audio/Plug-Ins/VST3` and `~/Library/Audio/Plug-Ins/Components`
- System scope: `/Library/Audio/Plug-Ins/VST3` and `/Library/Audio/Plug-Ins/Components`

Important:

- `make install` installs to the macOS system folders under `/Library`, not the user folders.
- `cmake --install Build` is not the normal plugin-install step for this repo.

Package release zips:

```bash
./scripts/package_release_macos.sh --build-dir Build --config Release
```

If both `Build` and `cmake-build-release` exist, the packager requires `--build-dir` explicitly.

### Linux

Build:

```bash
cmake -S . -B Build -DCMAKE_BUILD_TYPE=Release
cmake --build Build --parallel
```

Install the plugin bundle for local DAW discovery:

```bash
mkdir -p ~/.vst3
cp -R Build/mlrVST_artefacts/Release/VST3/mlrVST.vst3 ~/.vst3/
```

If you keep JUCE at `./JUCE`, the Makefile wrapper does the same copy for you:

```bash
make install
```

That wrapper exists for convenience, but it is not required.

`cmake --install Build` only performs the generic CMake install rules. It does not replace the plugin-folder copy step above.

### Windows

Configure and build from a Visual Studio developer shell:

```powershell
cmake -S . -B Build -G "Visual Studio 17 2022" -A x64
cmake --build Build --config Release
```

Manual install:

- Copy `Build\mlrVST_artefacts\Release\VST3\mlrVST.vst3`
- Into `%CommonProgramFiles%\VST3\`

Package a distributable zip:

```powershell
.\scripts\package_release_windows.ps1 -BuildDir Build -Config Release -OutDir release\windows
```

Important:

- `MinGW` and `MSYS2` are not supported.
- There is no repo-specific Windows plugin install target; copy the built `.vst3` bundle manually.

## Makefile Shortcuts

The `Makefile` is useful on macOS and Linux when `JUCE` lives at `./JUCE`.

```bash
make
make vst3
make au
make install
make package-release
```

Notes:

- `make au` is macOS-only
- `make install` is system-scope on macOS and user-scope on Linux
- `make package-release` currently wraps the macOS packager only

## Useful CMake Options

```bash
-DMLRVST_JUCE_PATH=/path/to/JUCE
-DMLRVST_ENABLE_HUOVILAINEN=OFF
-DMLRVST_ENABLE_SOUNDTOUCH=ON
-DMLRVST_ENABLE_BUNGEE=ON
-DMLRVST_ENABLE_LIBPYIN=ON
-DMLRVST_NATIVE_DEPS_DIR=/path/to/third_party/_native
-DMLRVST_ESSENTIA_PREFIX=/path/to/essentia/prefix
-DMLRVST_BUNGEE_PREFIX=/path/to/bungee/prefix
```

Defaults:

- `MLRVST_ENABLE_HUOVILAINEN=OFF`
- `MLRVST_ENABLE_SOUNDTOUCH=ON`
- `MLRVST_ENABLE_BUNGEE=ON`
- `MLRVST_ENABLE_LIBPYIN=ON`

Even when the default is `ON`, the backend is only compiled if the dependency is actually available.

## Verification

Check that the expected bundles exist:

```bash
ls Build/mlrVST_artefacts/Release
```

macOS AU validation:

```bash
auval -v aufx Mlrv Mlrx
```

macOS dependency check:

```bash
otool -L Build/mlrVST_artefacts/Release/VST3/mlrVST.vst3/Contents/MacOS/mlrVST
```

Linux dependency check:

```bash
ldd Build/mlrVST_artefacts/Release/VST3/mlrVST.vst3/Contents/x86_64-linux/mlrVST.so
```

## Troubleshooting

### JUCE not found

Use the correct cache variable:

```bash
cmake -S . -B Build -DMLRVST_JUCE_PATH=/path/to/JUCE
```

`JUCE_DIR` is not the project variable used by this repo.

### Windows build fails with MinGW

That toolchain is intentionally blocked by the current `JUCE 8` setup. Use a Visual Studio generator or `clang-cl`.

### Optional backends are missing after configure

This usually means the dependency was not found. Build still works, but those backends are disabled.

To restore the repo-local native prefixes on macOS:

```bash
./scripts/bootstrap_native_deps.sh
```

Or point CMake at known-good prefixes with `MLRVST_ESSENTIA_PREFIX` and `MLRVST_BUNGEE_PREFIX`.

### macOS install landed in the wrong scope

Use:

- `install_plugins_user` or `scripts/install_macos_plugins.sh --user` for `~/Library`
- `install_plugins_system` or `make install` for `/Library`

### macOS packaging refuses to auto-detect the build dir

Pass the build dir explicitly:

```bash
./scripts/package_release_macos.sh --build-dir Build --config Release
```
