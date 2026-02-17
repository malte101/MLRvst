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

## Notes

- There is no standalone app target in the current `CMakeLists.txt`.
- For detailed platform setup and troubleshooting, see `Docs/BUILD.md`.
