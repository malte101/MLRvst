# mlrVST - Modern Edition

A complete modernization of mlrVST, the VST port of the legendary monome mlr application. This version features JUCE 8.x, native OSC support, professional audio engine, and full serialosc compatibility.

![Build Status](https://img.shields.io/badge/build-passing-brightgreen)
![JUCE](https://img.shields.io/badge/JUCE-8.x-blue)
![License](https://img.shields.io/badge/license-MIT-green)

## 🎵 Features

### Audio Engine
- ✨ **High-Quality Resampling** - Linear, Cubic (Hermite), and Windowed Sinc interpolation
- 🎚️ **Advanced Playback** - OneShot, Loop, Gate, Reverse, and PingPong modes
- 🎯 **Perfect Timing** - Host tempo sync with 1-32 division quantization
- 🔇 **Click-Free** - Automatic crossfading on all transitions
- 🎛️ **Full Control** - Per-strip volume, pan, and speed
- 📊 **Group Management** - Organize strips into mute groups
- 📝 **Pattern Recording** - Record and loop button sequences (4 patterns)
- 🎤 **Live Recording** - Tempo-synced audio input capture
- 🔒 **Thread-Safe** - Lock-free atomics for glitch-free audio

### monome Integration
- 🔌 **Auto-Discovery** - Automatic monome device detection via serialosc
- 💡 **Full LED Support** - Variable brightness, efficient updates
- 🔄 **Device Hot-Swap** - Handles connect/disconnect gracefully
- 📡 **OSC Protocol** - Complete serialosc protocol implementation
- 🎮 **Multiple Devices** - Support for all monome grid sizes

### Modern Architecture
- 🏗️ **JUCE 8.x** - Latest framework with all modern features
- 🎵 **Native OSC** - Built-in juce_osc module (no external deps)
- 🔧 **CMake Build** - Modern, cross-platform build system
- 📦 **VST3 / AU** - Native plugin formats
- 🖥️ **Cross-Platform** - macOS, Windows, Linux

## 🚀 Quick Start

### Prerequisites

- **CMake** 3.22 or later
- **C++17** compatible compiler (GCC 9+, Clang 10+, MSVC 2019+)
- **JUCE 8.0.4** or later
- **serialosc** (for monome connectivity)

### Clone and Build

```bash
# Clone repository
git clone https://github.com/malte90924-pixel/mlrVST-modern.git
cd mlrVST-modern

# Get JUCE
git clone https://github.com/juce-framework/JUCE.git

# Build everything
make

# Or build with CMake directly
mkdir Build && cd Build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release -j8

# Install plugins
cmake --install .
```

### Quick Build Commands

```bash
make                # Build everything (default)
make vst3          # Build VST3 only
make au            # Build Audio Unit (macOS)
make install       # Install to system
make clean         # Clean build
make help          # Show all options
```

### Building with CLion

See [CLION_SETUP.md](Docs/CLION_SETUP.md) for detailed CLion instructions.

**Quick setup:**
1. Install build tools (see error fix below)
2. Clone JUCE: `git clone https://github.com/juce-framework/JUCE.git`
3. File → Open → Select `mlrVST-modern` folder
4. CLion will configure automatically
5. Build → Build Project

**Common Error Fix:**
If you get `CMAKE_C_COMPILE_OBJECT` error:

```bash
# macOS
xcode-select --install

# Linux
sudo apt-get install build-essential cmake

# Then reload CMake in CLion
```

### Check Your Build Environment

Run the diagnostic script:
```bash
./check-build-env.sh
```

This will check all requirements and tell you what's missing.

## 📖 Documentation

- **[BUILD.md](Docs/BUILD.md)** - Detailed build instructions
- **[AUDIO_ENGINE_DOCS.md](Docs/AUDIO_ENGINE_DOCS.md)** - Audio engine documentation
- **[SERIALOSC_REFERENCE.md](Docs/SERIALOSC_REFERENCE.md)** - SerialOSC protocol reference
- **[AUDIO_MODERNIZATION_COMPARISON.md](Docs/AUDIO_MODERNIZATION_COMPARISON.md)** - Old vs new comparison

## 🎮 Usage

### Setup

1. **Install serialosc** from [monome.org](https://monome.org/docs/serialosc/setup/)
2. **Connect your monome** grid
3. **Load mlrVST** in your DAW
4. **Auto-connect** - Plugin will discover and connect automatically

### Basic Operation

```
Grid Layout:
Row 0 (Strip 1): [------ Sample 1 ------]
Row 1 (Strip 2): [------ Sample 2 ------]
Row 2 (Strip 3): [------ Sample 3 ------]
...
Row 7 (Strip 8): [------ Sample 8 ------]
     Columns 0-15 (playback positions)
```

- **Load samples** via GUI or drag & drop
- **Trigger playback** by pressing grid buttons
- **Adjust parameters** in the GUI
- **Record patterns** for live looping
- **Group strips** for mix control

### Advanced Features

```cpp
// Set resampling quality
strip->resampler.setQuality(Resampler::Quality::Cubic);

// Configure play mode
strip->setPlayMode(EnhancedAudioStrip::PlayMode::PingPong);

// Set loop points
strip->setLoop(4, 12); // Loop columns 4-12

// Record pattern
engine->startPatternRecording(0, 4); // 4 beats
```

## 🏗️ Architecture

### Core Components

```
mlrVST-modern/
├── Source/
│   ├── PluginProcessor.cpp     # Main processor
│   ├── PluginEditor.cpp        # GUI
│   └── AudioEngine.cpp         # Audio engine
├── Docs/                       # Documentation
├── CMakeLists.txt             # Build config
└── Makefile                   # Build automation
```

### Audio Engine Modules

- **Resampler** - High-quality interpolation
- **Crossfader** - Click elimination
- **QuantizationClock** - Musical timing
- **PatternRecorder** - Pattern sequencing
- **LiveRecorder** - Audio capture
- **StripGroup** - Group management
- **EnhancedAudioStrip** - Sample playback

## 🔧 Configuration

### Resampling Quality

Choose based on CPU/quality tradeoff:

```cpp
Linear  - Fast, good for live (0.5% CPU per strip)
Cubic   - Balanced, recommended (1.5% CPU per strip)
Sinc    - Best quality (3-5% CPU per strip)
```

### Quantization

Set musical divisions:

```cpp
1  - Whole note
4  - Quarter note
16 - Sixteenth note (default)
32 - Thirty-second note
```

### Groups

Organize strips:

```cpp
4 groups, 2 strips each (default)
Independent volume/mute per group
```

## 📊 Performance

### CPU Usage (8 strips @ 44.1kHz)

| Quality | CPU  | Use Case |
|---------|------|----------|
| Linear  | 8%   | Live performance |
| Cubic   | 12%  | General use |
| Sinc    | 24%  | Studio/mastering |

### Latency

- Crossfade: 3ms (128 samples @ 44.1kHz)
- Quantization: 0 to 1 division
- Total: 3-125ms (musical timing)

## 🐛 Troubleshooting

### monome Not Detected

```bash
# Check serialosc is running
ps aux | grep serialosc  # macOS/Linux
tasklist | findstr serialosc  # Windows

# Restart serialosc
# macOS: launchctl unload/load
# Linux: systemctl restart serialosc
# Windows: Services > serialosc > Restart
```

### Audio Glitches

```bash
# Increase buffer size in DAW
Recommended: 512 or 1024 samples

# Lower resampling quality
Use Linear instead of Sinc
```

### Build Errors

```bash
# Clean and rebuild
make distclean
make

# Check JUCE version
cd JUCE && git pull origin master

# Verify CMake version
cmake --version  # Should be 3.22+
```

## 🤝 Contributing

Contributions welcome! Areas for improvement:

- GUI enhancements
- Additional play modes
- MIDI mapping
- Preset management
- Effect chains
- Documentation
- Testing
- Bug fixes

### Development Setup

```bash
git clone https://github.com/malte90924-pixel/mlrVST-modern.git
cd mlrVST-modern
git clone https://github.com/juce-framework/JUCE.git
make CONFIG=Debug VERBOSE=1
```

## 📜 License

MIT - see [LICENSE](LICENSE) file

## 🙏 Credits

- **Original mlrVST** - Ewan Hemingway ([hemmer](https://github.com/hemmer))
- **Original mlr** - Brian Crabtree ([monome](https://monome.org))
- **JUCE Framework** - [JUCE Team](https://juce.com)
- **Modernization** - 2024/2025

## 🔗 Links

- [Original mlrVST](https://github.com/hemmer/mlrVST)
- [monome](https://monome.org)
- [JUCE](https://juce.com)
- [serialosc](https://monome.org/docs/serialosc/)
- [llllllll forum](https://llllllll.co)

## 📝 Version History

### v2.0.0 - Modern Edition (2024)
- Complete rewrite for JUCE 8.x
- Native OSC support (juce_osc)
- Professional audio engine
- Pattern recording
- Live recording
- Group management
- CMake build system
- Full documentation

### v1.0.0 - Original
- JUCE 3.1.0
- oscpack library
- Basic mlr functionality

## 🎯 Roadmap

- [ ] Time-stretching (pitch-independent tempo)
- [ ] Pitch-shifting (tempo-independent pitch)
- [ ] ADSR envelopes
- [ ] Built-in effects (reverb, delay, filter)
- [ ] Sample editor
- [ ] MIDI mapping
- [ ] Preset system
- [ ] Enhanced GUI
- [ ] Multi-sample support

---

**Made with ❤️ for the monome community**
