# mlrVST (Modern)

Modern JUCE-based mlrVST for monome grid performance workflows.

- Plugin formats: VST3 and AU (macOS)
- Engine strips: 6
- Monome integration: serialosc-compatible grid input and LED feedback
- Build system: CMake + Makefile helpers

## Quick Start

```bash
git clone https://github.com/malte101/MLRvst.git
cd MLRvst
git clone https://github.com/juce-framework/JUCE.git
make
make install
```

For full platform setup and troubleshooting, see `Docs/BUILD.md`.

## Requirements

- CMake 3.22+
- C++17 compiler
- JUCE source at `./JUCE`
- serialosc (for monome hardware use)

## Build Commands

```bash
# Default build
make

# Format-specific
make vst3
make au

# Install plugin bundles to local plugin folders
make install
```

Direct CMake build:

```bash
cmake -B Build -DCMAKE_BUILD_TYPE=Release
cmake --build Build --config Release
```

## Release Packaging

Create release zips that include binaries and notices:

```bash
make package-release
# or
./scripts/package_release_macos.sh --config Release
```

Artifacts are written to `release/`.

## Documentation

- Quick start: `QUICKSTART.md`
- Build guide: `Docs/BUILD.md`
- CLion setup: `Docs/CLION_SETUP.md`
- Audio engine notes: `Docs/AUDIO_ENGINE_DOCS.md`
- SerialOSC reference: `Docs/SERIALOSC_REFERENCE.md`

## Licensing

- Project code in this repository is MIT-licensed: `LICENSE`
- Third-party terms and redistribution notes: `THIRD_PARTY_NOTICES.md`

If you redistribute binaries, include the relevant notice files.

## Credits

- Original mlrVST: https://github.com/hemmer/mlrVST
- monome: https://monome.org
- JUCE: https://juce.com
