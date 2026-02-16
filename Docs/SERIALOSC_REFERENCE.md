# SerialOSC Protocol Implementation Reference

## Quick Start

### Basic Connection Flow

```cpp
// 1. Start listening for OSC messages
oscReceiver.connect(8000);  // Your application port
oscReceiver.addListener(this);

// 2. Query serialoscd for devices
oscSender.connect("127.0.0.1", 12002);
oscSender.send("/serialosc/list", "localhost", 8000);

// 3. Receive device list
// Message: /serialosc/device s s i
// Args: [device_id, device_type, device_port]

// 4. Connect to specific device
oscSender.connect("127.0.0.1", devicePort);
oscSender.send("/sys/port", 8000);
oscSender.send("/sys/host", "127.0.0.1");
oscSender.send("/sys/prefix", "/monome");

// 5. Request device information
oscSender.send("/sys/info", "localhost", 8000);
```

## Message Reference

### Discovery Messages (Port 12002)

#### `/serialosc/list s i`
Request list of connected devices
- Args: [host, port] - where to send responses
- Example: `/serialosc/list "localhost" 8000`

#### `/serialosc/notify s i`
Register for device connection notifications
- Args: [host, port] - where to send notifications
- Example: `/serialosc/notify "localhost" 8000`
- Note: Re-send periodically to keep receiving notifications

#### Responses from serialosc:

**`/serialosc/device s s i`**
- Device discovered
- Args: [id, type, port]
- Example: `/serialosc/device "m1234567" "monome 128" 17218`

**`/serialosc/add s`**
- Device connected
- Args: [id]

**`/serialosc/remove s`**
- Device disconnected
- Args: [id]

### System Messages (Device Port)

#### `/sys/port i`
Set application port for this device
- Args: [port]
- Example: `/sys/port 8000`

#### `/sys/host s`
Set application host for this device
- Args: [host]
- Example: `/sys/host "127.0.0.1"`

#### `/sys/prefix s`
Set OSC address prefix
- Args: [prefix]
- Example: `/sys/prefix "/monome"`
- Default: `/monome`
- Persists across reconnections

#### `/sys/rotation i`
Set grid rotation
- Args: [degrees]
- Valid: 0, 90, 180, 270
- Example: `/sys/rotation 180`
- Persists across reconnections

#### `/sys/info s i`
Request device information
- Args: [host, port] - where to send responses
- Example: `/sys/info "localhost" 8000`

#### Responses from device:

**`/sys/id s`**
- Device serial number
- Args: [id]

**`/sys/size i i`**
- Grid dimensions
- Args: [width, height]
- Example: `/sys/size 16 8`

**`/sys/host s`**
- Current application host

**`/sys/port i`**
- Current application port

**`/sys/prefix s`**
- Current OSC prefix

**`/sys/rotation i`**
- Current rotation

### LED Control Messages (to device)

All LED messages use the configured prefix (default `/monome`)

#### `/<prefix>/grid/led/set i i i`
Set single LED
- Args: [x, y, state]
- state: 0 (off) to 15 (max brightness)
- Example: `/monome/grid/led/set 3 5 15`

#### `/<prefix>/grid/led/all i`
Set all LEDs to same state
- Args: [state]
- Example: `/monome/grid/led/all 0`

#### `/<prefix>/grid/led/map i i i i i i i i i i`
Set 8×8 quad
- Args: [x_offset, y_offset, row0, row1, row2, row3, row4, row5, row6, row7]
- Each row is a bitmask (0-255)
- Example: `/monome/grid/led/map 0 0 255 0 255 0 255 0 255 0`

#### `/<prefix>/grid/led/row i i i i i i i i i i`
Set row of LEDs
- Args: [x_offset, y, led0, led1, led2, led3, led4, led5, led6, led7]
- Each led is 0-15
- Example: `/monome/grid/led/row 0 0 15 0 15 0 15 0 15 0`

#### `/<prefix>/grid/led/col i i i i i i i i i i`
Set column of LEDs
- Args: [x, y_offset, led0, led1, led2, led3, led4, led5, led6, led7]
- Each led is 0-15
- Example: `/monome/grid/led/col 0 0 15 15 15 15 0 0 0 0`

#### `/<prefix>/grid/led/intensity i`
Set global LED intensity
- Args: [intensity]
- Range: 0-15
- Example: `/monome/grid/led/intensity 10`

### Input Messages (from device)

#### `/<prefix>/grid/key i i i`
Button press/release
- Args: [x, y, state]
- state: 1 = press, 0 = release
- Example: `/monome/grid/key 3 5 1`

## LED Brightness Levels

Modern monome grids support variable brightness:
- 0 = Off
- 1-14 = Dimmed levels
- 15 = Full brightness

Older devices may only support on/off (0 or 1).

## Bitmask Format

For `/grid/led/map` and similar messages using bitmasks:

```
Row bitmask value:
Bit:  7  6  5  4  3  2  1  0
LED:  □  □  □  □  □  □  □  □
Val:  128 64 32 16 8  4  2  1

Example: 170 = 10101010 binary
         = □ ■ □ ■ □ ■ □ ■
```

## Configuration File Locations

serialosc saves device preferences:

- **Linux**: `$XDG_CONFIG_HOME/serialosc/` or `$HOME/.config/serialosc/`
- **macOS**: `~/Library/Preferences/org.monome.serialosc/`
- **Windows**: `%LOCALAPPDATA%\monome\serialosc\`

Each device has a `.conf` file with its settings.

## Example: Complete Connection Sequence

```cpp
void connectToMonome()
{
    // Step 1: Start receiver
    if (!oscReceiver.connect(8000))
    {
        DBG("Failed to bind port 8000");
        return;
    }
    oscReceiver.addListener(this);
    
    // Step 2: Discover devices
    oscSender.connect("127.0.0.1", 12002);
    oscSender.send("/serialosc/list", "localhost", 8000);
    oscSender.send("/serialosc/notify", "localhost", 8000);
}

void oscMessageReceived(const juce::OSCMessage& msg)
{
    auto addr = msg.getAddressPattern().toString();
    
    // Step 3: Handle device discovery
    if (addr == "/serialosc/device" && msg.size() >= 3)
    {
        auto id = msg[0].getString();
        auto type = msg[1].getString();
        int port = msg[2].getInt32();
        
        // Step 4: Connect to device
        oscSender.connect("127.0.0.1", port);
        oscSender.send("/sys/port", 8000);
        oscSender.send("/sys/host", "127.0.0.1");
        oscSender.send("/sys/prefix", "/monome");
        oscSender.send("/sys/info", "localhost", 8000);
    }
    
    // Step 5: Handle device info
    else if (addr == "/sys/size" && msg.size() >= 2)
    {
        int width = msg[0].getInt32();
        int height = msg[1].getInt32();
        DBG("Grid size: " + String(width) + "x" + String(height));
        
        // Step 6: Initialize LEDs
        oscSender.send("/monome/grid/led/all", 0);
    }
    
    // Step 7: Handle key presses
    else if (addr == "/monome/grid/key" && msg.size() >= 3)
    {
        int x = msg[0].getInt32();
        int y = msg[1].getInt32();
        int state = msg[2].getInt32();
        
        // Handle the press
        if (state == 1)
        {
            oscSender.send("/monome/grid/led/set", x, y, 15);
        }
    }
}
```

## Common Patterns

### LED Animation
```cpp
void updateLEDs()
{
    std::array<int, 8> rowData;
    
    // Set each LED individually
    for (int i = 0; i < 8; ++i)
        rowData[i] = (i == currentPos) ? 15 : 0;
    
    // Send row update
    juce::OSCMessage msg("/monome/grid/led/row");
    msg.addInt32(0);  // x offset
    msg.addInt32(0);  // y position
    for (int val : rowData)
        msg.addInt32(val);
    oscSender.send(msg);
}
```

### Efficient Block Updates
```cpp
void updateBlock(int x, int y, uint8_t pattern[8])
{
    juce::OSCMessage msg("/monome/grid/led/map");
    msg.addInt32(x);
    msg.addInt32(y);
    for (int i = 0; i < 8; ++i)
        msg.addInt32(pattern[i]);
    oscSender.send(msg);
}
```

### Device Monitoring
```cpp
void monitorDevices()
{
    // Keep registration active
    juce::Timer::callbackOnMessageThread([this]()
    {
        oscSender.connect("127.0.0.1", 12002);
        oscSender.send("/serialosc/notify", "localhost", 8000);
    });
}
```

## Debugging Tips

### 1. Monitor OSC Traffic
```bash
# Linux/Mac
oscdump 8000

# Or use tools like:
# - Protokol (https://hexler.net/protokol)
# - OSCulator
```

### 2. Send Manual Commands
```bash
# List devices
oscsend localhost 12002 /serialosc/list si localhost 8000

# Set LED
oscsend localhost 17218 /monome/grid/led/set iii 0 0 15
```

### 3. Check serialosc Status
```bash
# See if running
ps aux | grep serialosc

# Check logs (Linux)
journalctl -u serialosc

# Test connection
nc -u 127.0.0.1 12002
```

### 4. Verify Port Availability
```bash
# See what's using port 8000
lsof -i :8000        # Mac/Linux
netstat -ano | findstr :8000  # Windows
```

## Troubleshooting

| Problem | Solution |
|---------|----------|
| No devices found | Check serialosc is running, try unplugging/replugging |
| LEDs not updating | Verify correct port, check prefix matches |
| Keys not received | Ensure `/sys/port` and `/sys/host` are set |
| Connection drops | Keep sending `/serialosc/notify` periodically |
| Wrong grid size | Some apps query size before connection completes |

## Performance Notes

- Use `/grid/led/map` for bulk updates (faster than individual LEDs)
- Limit update rate to ~60 FPS max
- Use `/grid/led/row` or `/grid/led/col` when updating lines
- Batch LED updates when possible
- Consider LED state caching to avoid redundant updates

## Version Compatibility

This implementation works with:
- serialosc 1.4+ (current version)
- All monome grid models (64, 128, 256, etc.)
- Both variable brightness and on/off grids

Legacy monomeserial is not supported (deprecated in 2011).
