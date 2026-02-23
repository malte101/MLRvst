# mlrVST (Modern)

Modern JUCE-based version of mlrVST for monome grid performance workflows.

- Plugin formats: VST3 and AU (macOS)
- Engine strips: 6
- Monome integration: serialosc-compatible grid input and LED feedback
- Build system: CMake + Makefile helpers

## Repository

```bash
git clone https://github.com/malte101/MLRvst.git
cd MLRvst
```

## Requirements

- CMake 3.22+
- C++17 compiler
- JUCE source at `./JUCE`
- serialosc (for monome hardware use)

## Build

```bash
# configure + build
make

# format-specific
make vst3
make au

# install plugin bundles to local plugin folders
make install
```

Direct CMake build:

```bash
cmake -B Build -DCMAKE_BUILD_TYPE=Release
cmake --build Build --config Release
```

## Release Packaging

Create release zips that include binaries plus license notices:

```bash
make package-release
# or
./scripts/package_release_macos.sh --build-dir cmake-build-release --config Release
```

Artifacts are written to `release/`.

## Notes

- The build currently prints whether `MLRVST_ENABLE_HUOVILAINEN` is enabled.
- Default is `OFF` for release compliance.
- If enabled explicitly (`-DMLRVST_ENABLE_HUOVILAINEN=ON`), review and satisfy applicable third-party obligations before distributing binaries.

## Documentation

- Build guide: `Docs/BUILD.md`
- CLion setup: `Docs/CLION_SETUP.md`
- Audio engine notes: `Docs/AUDIO_ENGINE_DOCS.md`
- SerialOSC reference: `Docs/SERIALOSC_REFERENCE.md`

## License

- Project code: MIT (`LICENSE`)
- Third-party terms: `THIRD_PARTY_NOTICES.md`

If you redistribute binaries, include the relevant notice files.

## Credits

- Original mlrVST: https://github.com/hemmer/mlrVST
- monome: https://monome.org
- JUCE: https://juce.com
