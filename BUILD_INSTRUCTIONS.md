# Build Instructions

This project currently builds plugin targets only:
- `mlrVST_VST3`
- `mlrVST_AU` (macOS)

## Clean Build

```bash
# From repository root
rm -rf Build cmake-build-debug cmake-build-release
cmake -B Build -DCMAKE_BUILD_TYPE=Release
cmake --build Build --config Release
```

## Install

```bash
# Option 1: CMake install
cmake --install Build

# Option 2: Makefile helper
make install
```

## Verify Artifacts

```bash
ls Build/mlrVST_artefacts/Release/VST3/
ls Build/mlrVST_artefacts/Release/AU/   # macOS
```

## Create Release Zips (with notices)

```bash
# Uses Build/ by default
make package-release

# Or package directly from cmake-build-release
./scripts/package_release_macos.sh --build-dir cmake-build-release
```

## Notes

- There is no standalone app target in the current `CMakeLists.txt`.
- `MLRVST_ENABLE_HUOVILAINEN` defaults to `OFF` for release compliance; enable explicitly only if you have validated obligations.
- For detailed platform setup and troubleshooting, see `Docs/BUILD.md`.
