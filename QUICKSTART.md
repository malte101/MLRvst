# Quick Start Guide

Get mlrVST up and running in 5 minutes!

## 1. Clone Repository

```bash
git clone https://github.com/yourusername/mlrVST-modern.git
cd mlrVST-modern
```

## 2. Get JUCE

```bash
git clone https://github.com/juce-framework/JUCE.git
```

## 3. Build

```bash
make
```

That's it! The plugin will be built and installed automatically.

## 4. Install serialosc

Download from: https://monome.org/docs/serialosc/setup/

- **macOS**: Install .pkg
- **Windows**: Install .exe  
- **Linux**: `sudo apt-get install serialosc`

## 5. Connect monome

Plug in your monome grid. It should be automatically detected.

## 6. Use Plugin

### In DAW

1. Open your DAW (Ableton, Logic, Reaper, etc.)
2. Scan for new plugins
3. Load mlrVST on a track
4. Load samples via GUI
5. Press monome buttons to trigger!

### Standalone

```bash
# macOS
open Build/mlrVST_artefacts/Release/Standalone/mlrVST.app

# Linux
./Build/mlrVST_artefacts/Release/Standalone/mlrVST

# Windows
Build\mlrVST_artefacts\Release\Standalone\mlrVST.exe
```

## Troubleshooting

### Build fails?

```bash
# Install dependencies first
# macOS:
xcode-select --install

# Linux:
sudo apt-get install build-essential cmake libasound2-dev

# Then retry
make distclean && make
```

### monome not detected?

```bash
# Check serialosc is running
ps aux | grep serialosc  # macOS/Linux
tasklist | findstr serialosc  # Windows

# Restart if needed
# Then reconnect monome
```

### Need help?

- Read [BUILD.md](Docs/BUILD.md) for detailed instructions
- Check [README.md](README.md) for full documentation
- Open an issue on GitHub

## Next Steps

- Read [AUDIO_ENGINE.md](Docs/AUDIO_ENGINE.md) to learn audio features
- Check [SERIALOSC.md](Docs/SERIALOSC.md) for monome setup
- Explore the GUI for advanced features

Happy looping! 🎵
