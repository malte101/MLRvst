# Quick Start Guide

## 1. Clone Repository

```bash
git clone https://github.com/malte101/MLRvst.git
cd MLRvst
```

## 2. Get JUCE

```bash
git clone https://github.com/juce-framework/JUCE.git
```

## 3. Build And Install

```bash
cmake -S . -B Build -DCMAKE_BUILD_TYPE=Release
cmake --build Build --config Release
```

Install notes:

- macOS user install: `cmake --build Build --target install_plugins_user`
- macOS system install: `cmake --build Build --target install_plugins_system`
- Linux local install: `mkdir -p ~/.vst3 && cp -R Build/mlrVST_artefacts/Release/VST3/mlrVST.vst3 ~/.vst3/`
- Windows: copy `Build\mlrVST_artefacts\Release\VST3\mlrVST.vst3` to `%CommonProgramFiles%\VST3\`

If you want the optional native Essentia and Bungee backends on macOS:

```bash
./scripts/bootstrap_native_deps.sh
```

## 4. Install serialosc

Install serialosc from https://monome.org/docs/serialosc/setup/

## 5. Load In DAW

1. Rescan plugins in your DAW.
2. Insert `mlrVST` on a track.
3. Load audio and trigger from GUI or monome grid.

## Troubleshooting

Build fails:

```bash
xcode-select --install  # macOS
rm -rf Build
cmake -S . -B Build -DCMAKE_BUILD_TYPE=Release
cmake --build Build --config Release
```

monome not detected:

```bash
ps aux | grep serialosc
```

## More Documentation

- Detailed build guide: [Docs/BUILD.md](Docs/BUILD.md)
- Main project overview: [README.md](README.md)
- Audio engine notes: [Docs/AUDIO_ENGINE_DOCS.md](Docs/AUDIO_ENGINE_DOCS.md)
- serialosc reference: [Docs/SERIALOSC_REFERENCE.md](Docs/SERIALOSC_REFERENCE.md)
