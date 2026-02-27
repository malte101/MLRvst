# Build Reference

This file is a short build reference.

For full setup, platform dependencies, and troubleshooting, use:

- `Docs/BUILD.md`

## Quick Build

```bash
git clone https://github.com/malte101/MLRvst.git
cd MLRvst
git clone https://github.com/juce-framework/JUCE.git
make
make install
```

## Direct CMake Build

```bash
cmake -B Build -DCMAKE_BUILD_TYPE=Release
cmake --build Build --config Release
cmake --install Build
```

## Release Packages

```bash
make package-release
```

Release artifacts are written to `release/`.

## Windows Build Note

- JUCE 8 does not support MinGW for this project.
- Build Windows binaries with MSVC (`Visual Studio 2022`, `-A x64`) or run the GitHub Actions Windows workflow.

## macOS Signing + Notarization

```bash
make sign-notarize \
  CONFIG=Release \
  BUILD_DIR=cmake-build-release \
  SIGNING_IDENTITY="Developer ID Application: Your Name (TEAMID)" \
  NOTARY_PROFILE="mlrvst-notary"
```

This signs and notarizes VST3/AU bundles and writes distributable zip files to `release/notarized-*`.
