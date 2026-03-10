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
cmake -S . -B Build -DCMAKE_BUILD_TYPE=Release
cmake --build Build --config Release
```

For install steps, packaging, and platform-specific notes, see `Docs/BUILD.md`.

## Requirements

- CMake 3.22+
- C++17 compiler
- JUCE source at `./JUCE`, or pass `-DMLRVST_JUCE_PATH=/path/to/JUCE`
- serialosc (optional, for monome hardware use)
- Native Essentia and Bungee installs are optional and can live under `third_party/_native`
- Vendored LibPyin in `third_party/LibPyin` is optional and only needed for that analysis path

Bootstrap the repo-local native dependencies on macOS if you want those backends available:

```bash
./scripts/bootstrap_native_deps.sh
```

## Build Commands

```bash
# Canonical build
cmake -B Build -DCMAKE_BUILD_TYPE=Release
cmake --build Build --config Release
```

Optional Makefile shortcuts on macOS/Linux:

```bash
make
make vst3
make au
make install
```

`make install` installs to `/Library/Audio/Plug-Ins/*` on macOS and `~/.vst3/` on Linux.

The build prefers persistent repo-local native prefixes in `third_party/_native` over temporary `/tmp/...` paths when those dependencies are present.

## Release Packaging

Create release zips that include binaries and notices:

```bash
make package-release
# or
./scripts/package_release_macos.sh --build-dir Build --config Release
```

Artifacts are written to `release/`.

## Documentation

- Quick start: `QUICKSTART.md`
- Build guide: `Docs/BUILD.md`
- Audio engine notes: `Docs/AUDIO_ENGINE_DOCS.md`
- SerialOSC reference: `Docs/SERIALOSC_REFERENCE.md`
- Archived notes and legacy guides: `Docs/archive/`

## Licensing

- Project code in this repository is licensed under the GNU Affero General Public License v3.0: `LICENSE`
- Original upstream attribution and provenance are preserved in: `NOTICE`, `UPSTREAM_PROVENANCE.md`
- Preserved upstream MIT notice for the original `hemmer/mlrVST` project: `third_party/licenses/hemmer-mlrVST-MIT-LICENSE.txt`
- Third-party terms and redistribution notes: `THIRD_PARTY_NOTICES.md`
- Native dependency layout and rebuild instructions: `third_party/_native/README.md`
- Vendored LibPyin licenses: `third_party/LibPyin/LICENSE`, `third_party/LibPyin/source/LICENSE_PYIN`, `third_party/LibPyin/source/LICENSE_VAMP`

If you redistribute binaries, include the relevant notice files and do not remove the preserved upstream attribution notices.

## Credits

- Original mlrVST: https://github.com/hemmer/mlrVST
- monome: https://monome.org
- JUCE: https://juce.com
