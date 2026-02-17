# mlrVST - Modernized Edition

A modernized version of mlrVST, the VST port of the popular monome mlr application. This version has been updated to use JUCE 8.x and features native OSC support with full serialosc compatibility.

## What's New in This Version

### Major Updates
- **JUCE 8.x** - Upgraded from JUCE 3.1.0 to the latest JUCE framework
- **Native OSC** - Replaced oscpack with JUCE's built-in `juce_osc` module
- **Modern serialosc** - Full compatibility with current serialosc protocol
- **CMake Build System** - Modern build system replacing the old Projucer-only workflow
- **Better Thread Safety** - Improved audio thread handling and lock-free structures
- **VST3/AU Support** - Modern plugin formats with JUCE 8
- **Cross-Platform** - Builds on Windows, macOS, and Linux

### Features
- Real-time sample manipulation with monome grid controllers
- 8 independent audio strips
- Multiple playback modes (one-shot, loop, gate)
- Host tempo sync or internal tempo
- Variable quantization
- Per-strip volume control
- Supports .wav, .flac, .ogg, .aiff formats
- Automatic monome device discovery
- Configurable OSC prefix and routing

## Requirements

### Build Requirements
- CMake 3.22 or higher
- C++17 compatible compiler
  - macOS: Xcode 13+
  - Windows: Visual Studio 2019+
  - Linux: GCC 9+ or Clang 10+
- JUCE 8.0.4 or later

### Runtime Requirements
- serialosc installed and running
- A monome grid controller (64, 128, 256, etc.)
- Audio workstation (DAW) or plugin host

## Building from Source

### 1. Clone the Repository
```bash
git clone https://github.com/malte90924-pixel/mlrVST-modern.git
cd mlrVST-modern
```

### 2. Get JUCE

#### Option A: Download JUCE
Download JUCE from https://juce.com/get-juce/ and place it in the project directory:
```bash
# Extract JUCE to ./JUCE
unzip JUCE-8.0.4.zip
mv JUCE-8.0.4 JUCE
```

#### Option B: Use Git Submodule
```bash
git submodule add https://github.com/juce-framework/JUCE.git
git submodule update --init --recursive
```

### 3. Build with CMake

#### macOS / Linux
```bash
mkdir build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release
```

#### Windows (Visual Studio)
```bash
mkdir build
cd build
cmake .. -G "Visual Studio 17 2022"
cmake --build . --config Release
```

### 4. Install the Plugin

The built plugin will be copied to the standard plugin directories automatically:

- **macOS AU**: `~/Library/Audio/Plug-Ins/Components/mlrVST.component`
- **macOS VST3**: `~/Library/Audio/Plug-Ins/VST3/mlrVST.vst3`
- **Windows VST3**: `C:\Program Files\Common Files\VST3\mlrVST.vst3`
- **Linux VST3**: `~/.vst3/mlrVST.vst3`

## Setup and Usage

### 1. Install serialosc
Download and install serialosc from https://monome.org/docs/serialosc/setup/

Verify it's running:
- **macOS/Linux**: `ps aux | grep serialosc`
- **Windows**: Check Task Manager for `serialoscd.exe`

### 2. Connect Your Monome
Plug in your monome grid controller. serialosc should automatically detect it.

### 3. Load the Plugin
Add mlrVST to a track in your DAW.

### 4. Connect to Monome
The plugin will automatically discover and connect to your monome. If multiple devices are connected, use the device selector to choose which one to use.

### 5. Load Samples
- Click "Load Strip X" buttons to load audio samples
- Each strip can hold one sample
- Supported formats: WAV, AIFF, FLAC, OGG, MP3

### 6. Play!
- Press buttons on your monome to trigger samples
- Each row represents a strip
- Each column triggers playback from that position in the sample

## Configuration

### OSC Settings
The plugin listens on port 8000 by default. You can change this in the code if needed.

Default OSC prefix: `/monome`

### serialosc Communication
The plugin communicates with serialosc on port 12002 (serialosc default).

### Grid Layout
```
Row 0 (Strip 1): [------ Sample 1 ------]
Row 1 (Strip 2): [------ Sample 2 ------]
Row 2 (Strip 3): [------ Sample 3 ------]
Row 3 (Strip 4): [------ Sample 4 ------]
Row 4 (Strip 5): [------ Sample 5 ------]
Row 5 (Strip 6): [------ Sample 6 ------]
Row 6 (Strip 7): [------ Sample 7 ------]
Row 7 (Strip 8): [------ Sample 8 ------]
       Columns 0-15 (playback positions)
```

## Troubleshooting

### Monome Not Detected
1. Check serialosc is running
2. Verify the monome is connected (try unplugging and replugging)
3. Check Console/Terminal for OSC debug messages
4. Try restarting serialosc

### No Sound
1. Verify samples are loaded
2. Check volume levels (master and per-strip)
3. Ensure the plugin is on an active track
4. Check your DAW's audio routing

### Build Errors
1. Ensure CMake version is 3.22+
2. Verify JUCE is properly installed
3. Check compiler version meets requirements
4. Try cleaning the build directory and rebuilding

### OSC Communication Issues
1. Check firewall settings (allow UDP on port 8000)
2. Verify serialosc is listening on port 12002
3. Check if another application is using port 8000
4. Try changing the application port in the code

## Development

### Project Structure
```
mlrVST-modern/
├── Source/
│   ├── PluginProcessor.h       # Main processor header
│   ├── PluginProcessor.cpp     # Main processor implementation
│   ├── PluginEditor.h          # GUI header
│   └── PluginEditor.cpp        # GUI implementation
├── CMakeLists.txt              # Build configuration
├── Docs/MODERNIZATION_GUIDE.md # Detailed migration guide
└── README.md                   # Main project readme
```

### Key Classes

#### `MonomeConnection`
Handles all serialosc communication:
- Device discovery
- OSC send/receive
- LED control
- Key press handling

#### `AudioStrip`
Represents a single sample playback channel:
- Sample loading
- Playback control
- Volume and speed control
- Loop modes

#### `MlrVSTAudioProcessor`
Main audio processor:
- Manages strips
- Audio processing
- Parameter handling
- DAW integration

### Adding Features

To add new features, you'll typically:

1. Add parameters in `createParameterLayout()`
2. Handle parameter changes in `processBlock()`
3. Update the GUI in `PluginEditor`
4. Add monome controls in `handleMonomeKeyPress()`

### Debugging OSC

Enable debug output in the code:
```cpp
#define DEBUG_OSC 1
```

This will print all OSC messages to the console.

### Testing

Test the plugin with:
- Various audio formats
- Different sample rates
- Different monome models (64, 128, 256)
- Multiple DAWs
- Different buffer sizes
- Various quantization settings

## API Reference

### MonomeConnection

```cpp
// Connect to serialosc
void connect(int applicationPort);

// Send LED commands
void setLED(int x, int y, int state);
void setAllLEDs(int state);
void setLEDRow(int xOffset, int y, const std::array<int, 8>& data);

// Callbacks
std::function<void(int x, int y, int state)> onKeyPress;
std::function<void()> onDeviceConnected;
```

### AudioStrip

```cpp
// Load sample
void loadSample(const juce::File& file, double sampleRate);

// Playback control
void trigger(int column, double tempo);
void stop();
void setVolume(float volume);
void setPlaybackSpeed(float speed);
```

## Contributing

Contributions are welcome! Please:

1. Fork the repository
2. Create a feature branch
3. Make your changes
4. Test thoroughly
5. Submit a pull request

## License

MIT - see [../LICENSE](../LICENSE).

## Credits

- **Original mlrVST**: hemmer (Ewan Hemingway)
- **Original mlr**: Brian Crabtree (monome)
- **Modernization**: 2024/2025 Contributors
- **JUCE Framework**: JUCE Team
- **monome**: monome.org

## Links

- Original mlrVST: https://github.com/hemmer/mlrVST
- JUCE: https://juce.com/
- monome: https://monome.org/
- serialosc: https://monome.org/docs/serialosc/

## Version History

### v2.0.0 (Modernized Edition)
- Updated to JUCE 8.x
- Native OSC support
- Modern serialosc protocol
- CMake build system
- VST3 support
- Improved stability

### v1.0.0 (Original)
- JUCE 3.1.0
- oscpack library
- VST2/AU support (original version)
