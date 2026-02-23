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
