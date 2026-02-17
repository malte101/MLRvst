# mlrVST Modernization Guide

## Overview
This guide details how to modernize mlrVST from JUCE 3.1.0 to JUCE 8.x, replace oscpack with JUCE's built-in OSC module, and ensure compatibility with current serialosc.

## Major Changes Required

### 1. JUCE Version Update (3.1.0 → 8.x)

#### Key API Changes:
- **Audio Processor**: `processBlock()` now uses `AudioBuffer<float>` instead of `AudioSampleBuffer`
- **Parameters**: Use `AudioProcessorValueTreeState` instead of manual parameter management
- **File I/O**: Updated to use `juce::File` with modern API
- **Component hierarchy**: Some deprecated methods need updating
- **Module system**: Projucer configuration has changed significantly

### 2. OSC Implementation (oscpack → juce_osc)

#### Old (oscpack):
```cpp
#include "osc/OscOutboundPacketStream.h"
#include "ip/UdpSocket.h"

UdpTransmitSocket socket(IpEndpointName(host, port));
osc::OutboundPacketStream p(buffer, OUTPUT_BUFFER_SIZE);
p << osc::BeginMessage("/monome/grid/led/set")
  << x << y << s
  << osc::EndMessage;
socket.Send(p.Data(), p.Size());
```

#### New (juce_osc):
```cpp
#include <juce_osc/juce_osc.h>

juce::OSCSender oscSender;
oscSender.connect(host, port);
oscSender.send("/monome/grid/led/set", x, y, s);
```

### 3. SerialOSC Protocol Update

#### Modern SerialOSC Discovery:
```cpp
// Query serialoscd on port 12002
oscSender.connect("127.0.0.1", 12002);
oscSender.send("/serialosc/list", "localhost", receiverPort);

// Receive device list
// /serialosc/device s s i (id, type, port)

// Connect to specific device
oscSender.connect("127.0.0.1", devicePort);
oscSender.send("/sys/port", applicationPort);
oscSender.send("/sys/host", "127.0.0.1");
oscSender.send("/sys/prefix", "/monome");
oscSender.send("/sys/info", "localhost", applicationPort);
```

#### Key SerialOSC Messages:
- `/sys/info host port` - Request device info
- `/sys/port i` - Set application port
- `/sys/host s` - Set application host
- `/sys/prefix s` - Set OSC prefix (default: /monome)
- `/sys/rotation i` - Set rotation (0, 90, 180, 270)
- `/<prefix>/grid/led/set x y s` - Set single LED
- `/<prefix>/grid/led/all s` - Set all LEDs
- `/<prefix>/grid/led/map x_offset y_offset [8 integers]` - Set 8x8 block
- `/<prefix>/grid/led/row x_offset y [8 integers]` - Set row
- `/<prefix>/grid/led/col x y_offset [8 integers]` - Set column
- `/<prefix>/grid/key x y s` - Receive key press (from monome)

## Step-by-Step Modernization

### Step 1: Update Project Structure

1. Create new JUCE 8 project using Projucer
2. Configure modules:
   - juce_audio_basics
   - juce_audio_devices
   - juce_audio_formats
   - juce_audio_processors
   - juce_audio_utils
   - juce_core
   - juce_data_structures
   - juce_events
   - juce_graphics
   - juce_gui_basics
   - juce_gui_extra
   - juce_osc (NEW - replaces oscpack)

3. Set plugin formats: VST3 and AU
4. Remove oscpack dependency

### Step 2: Update PluginProcessor

Key changes needed:
```cpp
class MlrVSTAudioProcessor : public juce::AudioProcessor,
                              public juce::OSCReceiver::Listener<juce::OSCReceiver::RealtimeCallback>
{
public:
    MlrVSTAudioProcessor();
    ~MlrVSTAudioProcessor() override;

    // Modern JUCE 8 API
    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    
    // Parameter management
    juce::AudioProcessorValueTreeState parameters;
    
    // OSC
    juce::OSCSender oscSender;
    juce::OSCReceiver oscReceiver;
    
    // OSC callback
    void oscMessageReceived(const juce::OSCMessage& message) override;
    
private:
    void connectToSerialOSC();
    void discoverMonomeDevices();
    void handleMonomeKeyPress(int x, int y, int state);
    
    int monomePort = -1;
    juce::String monomeId;
    juce::String oscPrefix = "/monome";
};
```

### Step 3: Implement Modern OSC Handler

```cpp
void MlrVSTAudioProcessor::oscMessageReceived(const juce::OSCMessage& message)
{
    auto address = message.getAddressPattern().toString();
    
    // Handle serialosc device discovery
    if (address == "/serialosc/device")
    {
        if (message.size() >= 3)
        {
            monomeId = message[0].getString();
            auto type = message[1].getString();
            monomePort = message[2].getInt32();
            
            // Connect to device
            oscSender.connect("127.0.0.1", monomePort);
            oscSender.send("/sys/port", 8000); // Our listening port
            oscSender.send("/sys/host", "127.0.0.1");
            oscSender.send("/sys/prefix", oscPrefix);
        }
    }
    // Handle grid key press
    else if (address == oscPrefix + "/grid/key")
    {
        if (message.size() >= 3)
        {
            int x = message[0].getInt32();
            int y = message[1].getInt32();
            int state = message[2].getInt32();
            handleMonomeKeyPress(x, y, state);
        }
    }
    // Handle system info response
    else if (address == "/sys/id" || address == "/sys/size" || 
             address == "/sys/rotation" || address == "/sys/prefix")
    {
        // Process device info
    }
}

void MlrVSTAudioProcessor::connectToSerialOSC()
{
    // Start OSC receiver
    if (!oscReceiver.connect(8000)) // Application port
    {
        DBG("Failed to connect OSC receiver");
        return;
    }
    
    oscReceiver.addListener(this);
    
    // Discover devices
    discoverMonomeDevices();
}

void MlrVSTAudioProcessor::discoverMonomeDevices()
{
    // Query serialoscd
    if (oscSender.connect("127.0.0.1", 12002))
    {
        oscSender.send("/serialosc/list", "localhost", 8000);
        
        // Also register for notifications
        oscSender.send("/serialosc/notify", "localhost", 8000);
    }
}
```

### Step 4: Update LED Control

```cpp
void sendLED(int x, int y, int state)
{
    oscSender.send(oscPrefix + "/grid/led/set", x, y, state);
}

void sendAllLEDs(int state)
{
    oscSender.send(oscPrefix + "/grid/led/all", state);
}

void sendLEDRow(int xOffset, int y, const std::vector<int>& data)
{
    // data should be 8 integers representing LED states
    juce::OSCMessage msg(oscPrefix + "/grid/led/row");
    msg.addInt32(xOffset);
    msg.addInt32(y);
    for (int val : data)
        msg.addInt32(val);
    oscSender.send(msg);
}

void sendLEDMap(int xOffset, int yOffset, const std::vector<int>& data)
{
    // data should be 8 integers (one per row) for an 8x8 block
    juce::OSCMessage msg(oscPrefix + "/grid/led/map");
    msg.addInt32(xOffset);
    msg.addInt32(yOffset);
    for (int val : data)
        msg.addInt32(val);
    oscSender.send(msg);
}
```

### Step 5: Update Audio Processing

```cpp
void MlrVSTAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, 
                                        juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;
    auto totalNumInputChannels = getTotalNumInputChannels();
    auto totalNumOutputChannels = getTotalNumOutputChannels();

    // Clear unused channels
    for (auto i = totalNumInputChannels; i < totalNumOutputChannels; ++i)
        buffer.clear(i, 0, buffer.getNumSamples());

    // Process audio samples here
    // (existing mlr sample playback logic)
}
```

### Step 6: Update Parameter Management

```cpp
MlrVSTAudioProcessor::MlrVSTAudioProcessor()
    : AudioProcessor(BusesProperties()
                     .withInput("Input", juce::AudioChannelSet::stereo(), true)
                     .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      parameters(*this, nullptr, "Parameters",
      {
          std::make_unique<juce::AudioParameterFloat>(
              "volume",
              "Volume",
              juce::NormalisableRange<float>(0.0f, 1.0f),
              0.7f),
          std::make_unique<juce::AudioParameterInt>(
              "quantize",
              "Quantize",
              1, 16, 4),
          // Add more parameters as needed
      })
{
    connectToSerialOSC();
}
```

### Step 7: Update File Loading

```cpp
void loadAudioFile(const juce::File& file)
{
    juce::AudioFormatManager formatManager;
    formatManager.registerBasicFormats();
    
    std::unique_ptr<juce::AudioFormatReader> reader(
        formatManager.createReaderFor(file));
    
    if (reader != nullptr)
    {
        auto sampleRate = reader->sampleRate;
        auto lengthInSamples = reader->lengthInSamples;
        auto numChannels = reader->numChannels;
        
        juce::AudioBuffer<float> tempBuffer(
            (int)numChannels,
            (int)lengthInSamples);
        
        reader->read(&tempBuffer,
                     0,
                     (int)lengthInSamples,
                     0,
                     true,
                     true);
        
        // Store in sample buffer
    }
}
```

## Testing Checklist

- [ ] Plugin loads in DAW without errors
- [ ] SerialOSC connection established
- [ ] Monome device discovered
- [ ] Key presses received
- [ ] LEDs respond to commands
- [ ] Audio playback works
- [ ] Sample loading works
- [ ] Parameters save/restore
- [ ] Quantization works
- [ ] Multiple strips functional

## Common Issues and Solutions

### Issue: OSC receiver fails to bind
**Solution**: Check port availability, try different port numbers, ensure no firewall blocking

### Issue: Monome not discovered
**Solution**: 
- Verify serialoscd is running
- Check monome is connected
- Try manual port configuration
- Check OSC prefix matches

### Issue: Audio glitches
**Solution**: 
- Increase buffer size
- Check thread safety
- Use lock-free structures for audio thread
- Verify sample rate conversion

### Issue: Build errors
**Solution**:
- Update to JUCE 8.0.4 or later
- Enable C++17 or later
- Check module configuration
- Verify all headers included

## Additional Resources

- JUCE Documentation: https://docs.juce.com/
- JUCE OSC Tutorial: https://juce.com/tutorials/tutorial_osc_sender_receiver/
- SerialOSC Protocol: https://monome.org/docs/serialosc/osc/
- Monome Forum: https://llllllll.co/

## Migration Timeline

1. **Day 1**: Set up new JUCE 8 project, configure modules
2. **Day 2**: Port core audio processing code
3. **Day 3**: Implement OSC communication with juce_osc
4. **Day 4**: Test serialosc integration
5. **Day 5**: Port GUI components
6. **Day 6**: Testing and bug fixes
7. **Day 7**: Documentation and release
