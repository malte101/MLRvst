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
make
make install
```

Plugin install locations:

- macOS VST3: `~/Library/Audio/Plug-Ins/VST3/mlrVST.vst3`
- macOS AU: `~/Library/Audio/Plug-Ins/Components/mlrVST.component`
- Linux VST3: `~/.vst3/mlrVST.vst3`
- Windows VST3: `C:\Program Files\Common Files\VST3\mlrVST.vst3`

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
make distclean && make
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
