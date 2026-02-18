/*
  ==============================================================================

    PluginProcessor.cpp
    mlrVST - Modern Edition Implementation

  ==============================================================================
*/

#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "PresetStore.h"

namespace
{
juce::String controlModeToKey(MlrVSTAudioProcessor::ControlMode mode)
{
    switch (mode)
    {
        case MlrVSTAudioProcessor::ControlMode::Speed: return "speed";
        case MlrVSTAudioProcessor::ControlMode::Pitch: return "pitch";
        case MlrVSTAudioProcessor::ControlMode::Pan: return "pan";
        case MlrVSTAudioProcessor::ControlMode::Volume: return "volume";
        case MlrVSTAudioProcessor::ControlMode::GrainSize: return "grainsize";
        case MlrVSTAudioProcessor::ControlMode::Filter: return "filter";
        case MlrVSTAudioProcessor::ControlMode::Swing: return "swing";
        case MlrVSTAudioProcessor::ControlMode::Gate: return "gate";
        case MlrVSTAudioProcessor::ControlMode::FileBrowser: return "browser";
        case MlrVSTAudioProcessor::ControlMode::GroupAssign: return "group";
        case MlrVSTAudioProcessor::ControlMode::Modulation: return "modulation";
        case MlrVSTAudioProcessor::ControlMode::Preset: return "preset";
        case MlrVSTAudioProcessor::ControlMode::Normal:
        default: return "normal";
    }
}

bool controlModeFromKey(const juce::String& key, MlrVSTAudioProcessor::ControlMode& mode)
{
    const auto normalized = key.trim().toLowerCase();
    if (normalized == "speed") { mode = MlrVSTAudioProcessor::ControlMode::Speed; return true; }
    if (normalized == "pitch") { mode = MlrVSTAudioProcessor::ControlMode::Pitch; return true; }
    if (normalized == "pan") { mode = MlrVSTAudioProcessor::ControlMode::Pan; return true; }
    if (normalized == "volume") { mode = MlrVSTAudioProcessor::ControlMode::Volume; return true; }
    if (normalized == "grainsize" || normalized == "grain_size" || normalized == "grain") { mode = MlrVSTAudioProcessor::ControlMode::GrainSize; return true; }
    if (normalized == "filter") { mode = MlrVSTAudioProcessor::ControlMode::Filter; return true; }
    if (normalized == "swing") { mode = MlrVSTAudioProcessor::ControlMode::Swing; return true; }
    if (normalized == "gate") { mode = MlrVSTAudioProcessor::ControlMode::Gate; return true; }
    if (normalized == "browser") { mode = MlrVSTAudioProcessor::ControlMode::FileBrowser; return true; }
    if (normalized == "group") { mode = MlrVSTAudioProcessor::ControlMode::GroupAssign; return true; }
    if (normalized == "mod" || normalized == "modulation") { mode = MlrVSTAudioProcessor::ControlMode::Modulation; return true; }
    if (normalized == "preset") { mode = MlrVSTAudioProcessor::ControlMode::Preset; return true; }
    return false;
}

juce::File getGlobalSettingsFile()
{
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("mlrVST")
        .getChildFile("GlobalSettings.xml");
}
}

//==============================================================================
// MonomeConnection Implementation
//==============================================================================

MonomeConnection::MonomeConnection()
{
    // Start heartbeat timer for connection monitoring
    startTimer(1000); // Check every second
}

MonomeConnection::~MonomeConnection()
{
    stopTimer();
    disconnect();
}

void MonomeConnection::connect(int appPort)
{
    // Disconnect if already connected
    oscReceiver.removeListener(this);
    oscReceiver.disconnect();

    // Bind to application port for receiving messages from device.
    // After restart, preferred port can be temporarily unavailable, so fall back.
    int boundPort = -1;
    for (int offset = 0; offset < 32; ++offset)
    {
        const int candidate = appPort + offset;
        if (oscReceiver.connect(candidate))
        {
            boundPort = candidate;
            break;
        }
    }

    if (boundPort < 0)
        return;

    applicationPort = boundPort;
    
    oscReceiver.addListener(this);
    
    // Connect to serialosc for device discovery
    (void) serialoscSender.connect("127.0.0.1", 12002);

    reconnectAttempts = 0;
    lastMessageTime = juce::Time::currentTimeMillis();
    lastConnectAttemptTime = lastMessageTime;
    lastPingTime = 0;
    lastDiscoveryTime = 0;
    lastReconnectAttemptTime = 0;
    awaitingDeviceResponse = false;
    
    // Start device discovery
    discoverDevices();
}

void MonomeConnection::refreshDeviceList()
{
    devices.clear();
    discoverDevices();
}

void MonomeConnection::disconnect()
{
    oscReceiver.removeListener(this);
    oscReceiver.disconnect();
    oscSender.disconnect();
    serialoscSender.disconnect();
    connected = false;
    reconnectAttempts = 0;
    lastMessageTime = 0;
    lastConnectAttemptTime = 0;
    lastPingTime = 0;
    lastDiscoveryTime = 0;
    lastReconnectAttemptTime = 0;
    awaitingDeviceResponse = false;
}

void MonomeConnection::discoverDevices()
{
    if (!serialoscSender.connect("127.0.0.1", 12002))
        return;
    
    // Query for device list
    const bool sentList = serialoscSender.send(
        juce::OSCMessage("/serialosc/list", juce::String("127.0.0.1"), applicationPort));
    
    // Subscribe to device notifications
    const bool sentNotify = serialoscSender.send(
        juce::OSCMessage("/serialosc/notify", juce::String("127.0.0.1"), applicationPort));

    if (sentList || sentNotify)
        lastDiscoveryTime = juce::Time::currentTimeMillis();
}

void MonomeConnection::selectDevice(int deviceIndex)
{
    if (deviceIndex < 0 || deviceIndex >= static_cast<int>(devices.size()))
        return;
    
    const bool hadActiveConnection = connected;
    currentDevice = devices[static_cast<size_t>(deviceIndex)];
    
    // Hard switch sender endpoint/state before attaching to new device.
    oscSender.disconnect();
    connected = false;
    awaitingDeviceResponse = false;
    lastConnectAttemptTime = 0;
    lastPingTime = 0;
    if (hadActiveConnection && onDeviceDisconnected)
        onDeviceDisconnected();
    
    // Connect to the device's port
    if (oscSender.connect(currentDevice.host, currentDevice.port))
    {
        configureCurrentDevice();
        sendPing();
        
        // Clear all LEDs on connection
        setAllLEDs(0);
        
        connected = true;
        reconnectAttempts = 0;
        lastMessageTime = 0;
        lastConnectAttemptTime = juce::Time::currentTimeMillis();
        lastPingTime = 0;
        awaitingDeviceResponse = true;
        
        if (onDeviceConnected)
            onDeviceConnected();

        // Some serialosc/device combinations can ignore initial sys routing
        // commands during rapid endpoint switching. Reassert once shortly after.
        const auto selectedId = currentDevice.id;
        juce::Timer::callAfterDelay(120, [this, selectedId]()
        {
            if (!connected || currentDevice.id != selectedId)
                return;
            configureCurrentDevice();
            sendPing();
        });
        
    }
    else
    {
        connected = false;
    }
}

void MonomeConnection::setLED(int x, int y, int state)
{
    if (!connected) return;
    oscSender.send(juce::OSCMessage(oscPrefix + "/grid/led/set", x, y, state));
}

void MonomeConnection::setAllLEDs(int state)
{
    if (!connected) return;
    oscSender.send(juce::OSCMessage(oscPrefix + "/grid/led/all", state));
}

void MonomeConnection::setLEDRow(int xOffset, int y, const std::array<int, 8>& data)
{
    if (!connected) return;
    
    juce::OSCMessage msg(oscPrefix + "/grid/led/row");
    msg.addInt32(xOffset);
    msg.addInt32(y);
    for (int val : data)
        msg.addInt32(val);
    oscSender.send(msg);
}

void MonomeConnection::setLEDColumn(int x, int yOffset, const std::array<int, 8>& data)
{
    if (!connected) return;
    
    juce::OSCMessage msg(oscPrefix + "/grid/led/col");
    msg.addInt32(x);
    msg.addInt32(yOffset);
    for (int val : data)
        msg.addInt32(val);
    oscSender.send(msg);
}

void MonomeConnection::setLEDMap(int xOffset, int yOffset, const std::array<int, 8>& data)
{
    if (!connected) return;
    
    juce::OSCMessage msg(oscPrefix + "/grid/led/map");
    msg.addInt32(xOffset);
    msg.addInt32(yOffset);
    for (int val : data)
        msg.addInt32(val);
    oscSender.send(msg);
}

void MonomeConnection::setRotation(int degrees)
{
    if (!connected) return;
    // Only 0, 90, 180, 270 are valid
    int validRotation = ((degrees / 90) * 90) % 360;
    oscSender.send(juce::OSCMessage("/sys/rotation", validRotation));
}

void MonomeConnection::setPrefix(const juce::String& newPrefix)
{
    oscPrefix = newPrefix;
    if (connected)
        oscSender.send(juce::OSCMessage("/sys/prefix", oscPrefix));
}

void MonomeConnection::requestInfo()
{
    if (!connected) return;
    oscSender.send(juce::OSCMessage("/sys/info", juce::String(currentDevice.host), applicationPort));
}

void MonomeConnection::requestSize()
{
    if (!connected) return;
    oscSender.send(juce::OSCMessage("/sys/size"));
}

// Variable brightness LED control (0-15 levels)
void MonomeConnection::setLEDLevel(int x, int y, int level)
{
    if (!connected) return;
    int clampedLevel = juce::jlimit(0, 15, level);
    oscSender.send(juce::OSCMessage(oscPrefix + "/grid/led/level/set", x, y, clampedLevel));
}

void MonomeConnection::setAllLEDLevels(int level)
{
    if (!connected) return;
    int clampedLevel = juce::jlimit(0, 15, level);
    oscSender.send(juce::OSCMessage(oscPrefix + "/grid/led/level/all", clampedLevel));
}

void MonomeConnection::setLEDLevelRow(int xOffset, int y, const std::array<int, 8>& levels)
{
    if (!connected) return;
    
    juce::OSCMessage msg(oscPrefix + "/grid/led/level/row");
    msg.addInt32(xOffset);
    msg.addInt32(y);
    for (int level : levels)
    {
        int clampedLevel = juce::jlimit(0, 15, level);
        msg.addInt32(clampedLevel);
    }
    oscSender.send(msg);
}

void MonomeConnection::setLEDLevelColumn(int x, int yOffset, const std::array<int, 8>& levels)
{
    if (!connected) return;
    
    juce::OSCMessage msg(oscPrefix + "/grid/led/level/col");
    msg.addInt32(x);
    msg.addInt32(yOffset);
    for (int level : levels)
    {
        int clampedLevel = juce::jlimit(0, 15, level);
        msg.addInt32(clampedLevel);
    }
    oscSender.send(msg);
}

void MonomeConnection::setLEDLevelMap(int xOffset, int yOffset, const std::array<int, 64>& levels)
{
    if (!connected) return;
    
    juce::OSCMessage msg(oscPrefix + "/grid/led/level/map");
    msg.addInt32(xOffset);
    msg.addInt32(yOffset);
    for (int level : levels)
    {
        int clampedLevel = juce::jlimit(0, 15, level);
        msg.addInt32(clampedLevel);
    }
    oscSender.send(msg);
}

// Tilt support
void MonomeConnection::enableTilt(int sensor, bool enable)
{
    if (!connected) return;
    oscSender.send(juce::OSCMessage(oscPrefix + "/tilt/set", sensor, enable ? 1 : 0));
}

// Connection status
juce::String MonomeConnection::getConnectionStatus() const
{
    if (!connected)
        return "Not connected";
    
    return "Connected to " + currentDevice.id + " (" + currentDevice.type + ") - " +
           juce::String(currentDevice.sizeX) + "x" + juce::String(currentDevice.sizeY);
}

void MonomeConnection::oscMessageReceived(const juce::OSCMessage& message)
{
    auto address = message.getAddressPattern().toString();

    // Only treat actual device/system traffic as successful handshake activity.
    // serialosc discovery traffic can be present even if the selected device is
    // not correctly routed to this app yet.
    const bool isDeviceTraffic = address.startsWith("/sys")
        || address.startsWith(oscPrefix + "/grid")
        || address.startsWith(oscPrefix + "/tilt");
    if (isDeviceTraffic)
    {
        lastMessageTime = juce::Time::currentTimeMillis();
        awaitingDeviceResponse = false;
    }

    if (address.startsWith("/serialosc"))
        handleSerialOSCMessage(message);
    else if (address.startsWith(oscPrefix + "/grid"))
        handleGridMessage(message);
    else if (address.startsWith(oscPrefix + "/tilt"))
        handleTiltMessage(message);
    else if (address.startsWith("/sys"))
        handleSystemMessage(message);
}

void MonomeConnection::timerCallback()
{
    const auto currentTime = juce::Time::currentTimeMillis();

    if (!connected)
    {
        if (!autoReconnect)
            return;

        if (currentTime - lastDiscoveryTime >= discoveryIntervalMs)
            discoverDevices();

        // Attempt direct reconnection while we still have a candidate endpoint.
        if (!currentDevice.id.isEmpty()
            && currentDevice.port > 0
            && reconnectAttempts < maxReconnectAttempts
            && (currentTime - lastReconnectAttemptTime) >= reconnectIntervalMs)
        {
            lastReconnectAttemptTime = currentTime;
            attemptReconnection();
        }

        return;
    }

    // A successful UDP "connect" does not guarantee the device is reachable.
    // Require a real response shortly after claiming an endpoint.
    if (awaitingDeviceResponse
        && lastConnectAttemptTime > 0
        && (currentTime - lastConnectAttemptTime) > handshakeTimeoutMs)
    {
        markDisconnected();
        discoverDevices();
        return;
    }

    // Treat long silence as dead connection, then fall back to discovery/reconnect.
    if (lastMessageTime > 0 && (currentTime - lastMessageTime) > connectionTimeoutMs)
    {
        markDisconnected();
        discoverDevices();
        return;
    }

    // Send periodic ping to keep connection alive and refresh sys state.
    if (lastPingTime == 0 || (currentTime - lastPingTime) >= pingIntervalMs)
    {
        sendPing();
        lastPingTime = currentTime;
    }
}

void MonomeConnection::attemptReconnection()
{
    reconnectAttempts++;
    
    // Try to reconnect to current device
    if (oscSender.connect(currentDevice.host, currentDevice.port))
    {
        configureCurrentDevice();
        sendPing();
        
        connected = true;
        reconnectAttempts = 0;
        lastMessageTime = 0;
        lastConnectAttemptTime = juce::Time::currentTimeMillis();
        lastPingTime = 0;
        awaitingDeviceResponse = true;
        
        if (onDeviceConnected)
            onDeviceConnected();
        
    }
    else if (autoReconnect)
    {
        discoverDevices();
    }
}

void MonomeConnection::sendPing()
{
    if (!connected) return;
    
    // Request device info as a "ping"
    oscSender.send(juce::OSCMessage("/sys/info", juce::String(currentDevice.host), applicationPort));
}

void MonomeConnection::handleSerialOSCMessage(const juce::OSCMessage& message)
{
    auto address = message.getAddressPattern().toString();
    auto renewNotify = [this]()
    {
        if (!serialoscSender.connect("127.0.0.1", 12002))
            return;
        serialoscSender.send(juce::OSCMessage("/serialosc/notify",
                                              juce::String("127.0.0.1"),
                                              applicationPort));
    };
    
    if (address == "/serialosc/device" && message.size() >= 3)
    {
        DeviceInfo info;
        info.id = message[0].getString();
        info.type = message[1].getString();
        info.port = message[2].getInt32();
        info.host = "127.0.0.1"; // Default to localhost
        
        // Check if device already exists in list
        bool deviceExists = false;
        bool endpointChanged = false;
        for (auto& existing : devices)
        {
            if (existing.id == info.id)
            {
                deviceExists = true;
                if (existing.port != info.port || existing.type != info.type || existing.host != info.host)
                {
                    existing.type = info.type;
                    existing.port = info.port;
                    existing.host = info.host;
                    endpointChanged = true;
                }
                break;
            }
        }
        
        if (!deviceExists)
        {
            devices.push_back(info);
        }

        // If this is our selected device and serialosc changed its endpoint,
        // switch to the new endpoint immediately.
        if (currentDevice.id == info.id
            && (currentDevice.port != info.port || currentDevice.host != info.host))
        {
            currentDevice.port = info.port;
            currentDevice.host = info.host;

            if (connected)
            {
                oscSender.disconnect();
                markDisconnected();
            }
        }

        if (!deviceExists || endpointChanged)
        {
            if (onDeviceListUpdated)
                onDeviceListUpdated(devices);
        }

        if (!connected)
        {
            int bestIndex = -1;
            if (!currentDevice.id.isEmpty())
            {
                for (int i = 0; i < static_cast<int>(devices.size()); ++i)
                {
                    if (devices[static_cast<size_t>(i)].id == currentDevice.id)
                    {
                        bestIndex = i;
                        break;
                    }
                }
            }

            if (bestIndex < 0 && !devices.empty())
                bestIndex = 0;

            if (bestIndex >= 0)
                selectDevice(bestIndex);
        }
    }
    else if (address == "/serialosc/add" && message.size() >= 1)
    {
        // serialosc notify is one-shot; re-register each time we get add/remove.
        renewNotify();

        // Device was plugged in
        juce::Timer::callAfterDelay(250, [this]()
        {
            discoverDevices(); // Refresh device list
        });
    }
    else if (address == "/serialosc/remove" && message.size() >= 1)
    {
        // serialosc notify is one-shot; re-register each time we get add/remove.
        renewNotify();

        // Device was unplugged
        auto removedId = message[0].getString();
        
        // Remove from device list
        devices.erase(std::remove_if(devices.begin(), devices.end(),
            [&removedId](const DeviceInfo& info) { return info.id == removedId; }),
            devices.end());
        
        // Check if it was our connected device
        if (removedId == currentDevice.id)
        {
            markDisconnected();
            
            // Try to auto-connect to another device if available
            if (!devices.empty() && autoReconnect)
                selectDevice(0);
        }
        
        if (onDeviceListUpdated)
            onDeviceListUpdated(devices);
    }
}

void MonomeConnection::markDisconnected()
{
    if (!connected)
        return;

    connected = false;
    oscSender.disconnect();
    awaitingDeviceResponse = false;
    lastConnectAttemptTime = 0;
    lastPingTime = 0;

    if (onDeviceDisconnected)
        onDeviceDisconnected();
}

void MonomeConnection::configureCurrentDevice()
{
    // Configure device to send messages to our application port.
    oscSender.send(juce::OSCMessage("/sys/port", applicationPort));
    oscSender.send(juce::OSCMessage("/sys/host", juce::String("127.0.0.1")));
    oscSender.send(juce::OSCMessage("/sys/prefix", oscPrefix));
    
    // Request device information and refresh prefix/size state.
    oscSender.send(juce::OSCMessage("/sys/info", juce::String("127.0.0.1"), applicationPort));
    oscSender.send(juce::OSCMessage("/sys/size"));
}

void MonomeConnection::handleGridMessage(const juce::OSCMessage& message)
{
    auto address = message.getAddressPattern().toString();
    
    if (address == oscPrefix + "/grid/key" && message.size() >= 3)
    {
        int x = message[0].getInt32();
        int y = message[1].getInt32();
        int state = message[2].getInt32();
        
        if (onKeyPress)
            onKeyPress(x, y, state);
    }
}

void MonomeConnection::handleSystemMessage(const juce::OSCMessage& message)
{
    auto address = message.getAddressPattern().toString();
    
    if (address == "/sys/size" && message.size() >= 2)
    {
        currentDevice.sizeX = message[0].getInt32();
        currentDevice.sizeY = message[1].getInt32();
    }
    else if (address == "/sys/id" && message.size() >= 1)
    {
        currentDevice.id = message[0].getString();
    }
    else if (address == "/sys/rotation" && message.size() >= 1)
    {
        (void)message[0].getInt32();
    }
    else if (address == "/sys/host" && message.size() >= 1)
    {
        currentDevice.host = message[0].getString();
    }
    else if (address == "/sys/port" && message.size() >= 1)
    {
        // Response to our port configuration
    }
    else if (address == "/sys/prefix" && message.size() >= 1)
    {
        // Response to our prefix configuration
    }
}

void MonomeConnection::handleTiltMessage(const juce::OSCMessage& message)
{
    auto address = message.getAddressPattern().toString();
    
    if (address == oscPrefix + "/tilt" && message.size() >= 4)
    {
        int sensor = message[0].getInt32();
        int x = message[1].getInt32();
        int y = message[2].getInt32();
        int z = message[3].getInt32();
        
        if (onTilt)
            onTilt(sensor, x, y, z);
    }
}

//==============================================================================
// MlrVSTAudioProcessor Implementation
//==============================================================================

MlrVSTAudioProcessor::MlrVSTAudioProcessor()
     : AudioProcessor(BusesProperties()
                      .withInput("Input", juce::AudioChannelSet::stereo(), true)
                      .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
       parameters(*this, nullptr, juce::Identifier("MlrVST"), createParameterLayout())
{
    // Initialize audio engine
    audioEngine = std::make_unique<ModernAudioEngine>();
    cacheParameterPointers();
    loadPersistentDefaultPaths();
    loadPersistentControlPages();
    setSwingDivisionSelection(swingDivisionSelection.load(std::memory_order_acquire));
    
    // Setup monome callbacks
    monomeConnection.onKeyPress = [this](int x, int y, int state)
    {
        handleMonomeKeyPress(x, y, state);
    };
    
    monomeConnection.onDeviceConnected = [this]()
    {
        // Force full LED resend after any reconnect to avoid stale cache mismatch.
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 16; ++x)
                ledCache[x][y] = -1;

        // Defer LED update slightly to ensure everything is ready
        juce::MessageManager::callAsync([this]()
        {
            updateMonomeLEDs();
        });
    };
    
    // Don't connect yet - wait for prepareToPlay
}

void MlrVSTAudioProcessor::cacheParameterPointers()
{
    masterVolumeParam = parameters.getRawParameterValue("masterVolume");
    quantizeParam = parameters.getRawParameterValue("quantize");
    grainQualityParam = parameters.getRawParameterValue("quality");
    pitchSmoothingParam = parameters.getRawParameterValue("pitchSmoothing");
    inputMonitorParam = parameters.getRawParameterValue("inputMonitor");
    crossfadeLengthParam = parameters.getRawParameterValue("crossfadeLength");

    for (int i = 0; i < MaxStrips; ++i)
    {
        stripVolumeParams[static_cast<size_t>(i)] = parameters.getRawParameterValue("stripVolume" + juce::String(i));
        stripPanParams[static_cast<size_t>(i)] = parameters.getRawParameterValue("stripPan" + juce::String(i));
        stripSpeedParams[static_cast<size_t>(i)] = parameters.getRawParameterValue("stripSpeed" + juce::String(i));
        stripPitchParams[static_cast<size_t>(i)] = parameters.getRawParameterValue("stripPitch" + juce::String(i));
    }
}

MlrVSTAudioProcessor::~MlrVSTAudioProcessor()
{
    stopTimer();
    monomeConnection.disconnect();
}

juce::String MlrVSTAudioProcessor::getControlModeName(ControlMode mode)
{
    switch (mode)
    {
        case ControlMode::Speed: return "Speed";
        case ControlMode::Pitch: return "Pitch";
        case ControlMode::Pan: return "Pan";
        case ControlMode::Volume: return "Volume";
        case ControlMode::GrainSize: return "Grain Size";
        case ControlMode::Filter: return "Filter";
        case ControlMode::Swing: return "Swing";
        case ControlMode::Gate: return "Gate";
        case ControlMode::FileBrowser: return "Browser";
        case ControlMode::GroupAssign: return "Group";
        case ControlMode::Modulation: return "Modulation";
        case ControlMode::Preset: return "Preset";
        case ControlMode::Normal:
        default: return "Normal";
    }
}

MlrVSTAudioProcessor::ControlPageOrder MlrVSTAudioProcessor::getControlPageOrder() const
{
    const juce::ScopedLock lock(controlPageOrderLock);
    return controlPageOrder;
}

MlrVSTAudioProcessor::ControlMode MlrVSTAudioProcessor::getControlModeForControlButton(int buttonIndex) const
{
    const int clamped = juce::jlimit(0, NumControlRowPages - 1, buttonIndex);
    const juce::ScopedLock lock(controlPageOrderLock);
    return controlPageOrder[static_cast<size_t>(clamped)];
}

int MlrVSTAudioProcessor::getControlButtonForMode(ControlMode mode) const
{
    const juce::ScopedLock lock(controlPageOrderLock);
    for (int i = 0; i < NumControlRowPages; ++i)
    {
        if (controlPageOrder[static_cast<size_t>(i)] == mode)
            return i;
    }
    return -1;
}

void MlrVSTAudioProcessor::moveControlPage(int fromIndex, int toIndex)
{
    if (fromIndex == toIndex)
        return;

    fromIndex = juce::jlimit(0, NumControlRowPages - 1, fromIndex);
    toIndex = juce::jlimit(0, NumControlRowPages - 1, toIndex);
    if (fromIndex == toIndex)
        return;

    {
        const juce::ScopedLock lock(controlPageOrderLock);
        std::swap(controlPageOrder[static_cast<size_t>(fromIndex)],
                  controlPageOrder[static_cast<size_t>(toIndex)]);
    }

    savePersistentControlPages();
}

void MlrVSTAudioProcessor::setControlPageMomentary(bool shouldBeMomentary)
{
    controlPageMomentary.store(shouldBeMomentary, std::memory_order_release);
    savePersistentControlPages();
}

void MlrVSTAudioProcessor::setSwingDivisionSelection(int mode)
{
    const int clamped = juce::jlimit(0, 3, mode);
    swingDivisionSelection.store(clamped, std::memory_order_release);
    if (audioEngine)
        audioEngine->setGlobalSwingDivision(static_cast<EnhancedAudioStrip::SwingDivision>(clamped));
    savePersistentControlPages();
}

void MlrVSTAudioProcessor::setControlModeFromGui(ControlMode mode, bool shouldBeActive)
{
    if (!shouldBeActive || mode == ControlMode::Normal)
    {
        currentControlMode = ControlMode::Normal;
        controlModeActive = false;
    }
    else
    {
        currentControlMode = mode;
        controlModeActive = true;
    }

    updateMonomeLEDs();
}

juce::AudioProcessorValueTreeState::ParameterLayout MlrVSTAudioProcessor::createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        "masterVolume",
        "Master Volume",
        juce::NormalisableRange<float>(0.0f, 1.0f),
        1.0f));
    
    layout.add(std::make_unique<juce::AudioParameterChoice>(
        "quantize",
        "Quantize",
        juce::StringArray{"1", "1/2", "1/2T", "1/4", "1/4T", 
                          "1/8", "1/8T", "1/16", "1/16T", "1/32"},
        5));  // Default to 1/8
    
    layout.add(std::make_unique<juce::AudioParameterChoice>(
        "quality",
        "Grain Quality",
        juce::StringArray{"Linear", "Cubic", "Sinc", "Sinc HQ"},
        2));
    
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        "pitchSmoothing",
        "Pitch Smoothing",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f),
        0.05f));  // Default 50ms
    
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        "inputMonitor",
        "Input Monitor",
        juce::NormalisableRange<float>(0.0f, 1.0f),
        1.0f));  // Default ON (1.0) for immediate monitoring

    layout.add(std::make_unique<juce::AudioParameterFloat>(
        "crossfadeLength",
        "Crossfade Length (ms)",
        juce::NormalisableRange<float>(1.0f, 50.0f, 0.1f),
        10.0f));
    
    for (int i = 0; i < MaxStrips; ++i)
    {
        layout.add(std::make_unique<juce::AudioParameterFloat>(
            "stripVolume" + juce::String(i),
            "Strip " + juce::String(i + 1) + " Volume",
            juce::NormalisableRange<float>(0.0f, 1.0f),
            1.0f));
            
        layout.add(std::make_unique<juce::AudioParameterFloat>(
            "stripPan" + juce::String(i),
            "Strip " + juce::String(i + 1) + " Pan",
            juce::NormalisableRange<float>(-1.0f, 1.0f),
            0.0f));
        
        layout.add(std::make_unique<juce::AudioParameterFloat>(
            "stripSpeed" + juce::String(i),
            "Strip " + juce::String(i + 1) + " Speed",
            juce::NormalisableRange<float>(0.0f, 4.0f, 0.01f, 0.5f),  // Includes full stop for grain mode
            1.0f));

        layout.add(std::make_unique<juce::AudioParameterFloat>(
            "stripPitch" + juce::String(i),
            "Strip " + juce::String(i + 1) + " Pitch",
            juce::NormalisableRange<float>(-12.0f, 12.0f, 0.01f),
            0.0f));
    }
    
    return layout;
}

//==============================================================================
void MlrVSTAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;
    audioEngine->prepareToPlay(sampleRate, samplesPerBlock);

    // Now safe to connect to monome
    if (!monomeConnection.isConnected())
        monomeConnection.connect(8000);

    // Clear all LEDs on startup
    juce::MessageManager::callAsync([this]()
    {
        if (monomeConnection.isConnected())
        {
            monomeConnection.setAllLEDs(0);
            // Initialize LED cache
            for (int y = 0; y < 8; ++y)
                for (int x = 0; x < 16; ++x)
                    ledCache[x][y] = -1;
        }
    });

    // Start LED update timer at 10fps (monome recommended refresh rate)
    if (!isTimerRunning())
        startTimer(100);  // 10fps - prevents flickering
}

void MlrVSTAudioProcessor::releaseResources()
{
    stopTimer();
    monomeConnection.disconnect();
}

bool MlrVSTAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    // Check output
    auto outputChannels = layouts.getMainOutputChannelSet();
    if (outputChannels != juce::AudioChannelSet::mono()
     && outputChannels != juce::AudioChannelSet::stereo())
        return false;

    // Check input (we accept mono or stereo input, or disabled)
    auto inputChannels = layouts.getMainInputChannelSet();
    if (inputChannels != juce::AudioChannelSet::disabled()
     && inputChannels != juce::AudioChannelSet::mono()
     && inputChannels != juce::AudioChannelSet::stereo())
        return false;

    return true;
}

void MlrVSTAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;
    
    // CRITICAL: Handle separate input/output buffers for AU/VST3 compatibility
    // Some hosts (especially AU) provide separate input and output buffers
    auto totalNumInputChannels = getTotalNumInputChannels();
    auto totalNumOutputChannels = getTotalNumOutputChannels();
    
    // Clear any output channels that don't have corresponding input
    for (auto i = totalNumInputChannels; i < totalNumOutputChannels; ++i)
        buffer.clear(i, 0, buffer.getNumSamples());
    
    // Get position info from host
    juce::AudioPlayHead::PositionInfo posInfo;
    
    if (auto* playHead = getPlayHead())
    {
        if (auto position = playHead->getPosition())
        {
            posInfo = *position;
        }
        else
        {
            // Host didn't provide position - assume playing
            posInfo.setIsPlaying(true);
        }
    }
    else
    {
        // No playhead - assume playing  
        posInfo.setIsPlaying(true);
    }
    
    // Set tempo FIRST: use host tempo if available, otherwise fallback default.
    if (!posInfo.getBpm().hasValue() || *posInfo.getBpm() <= 0.0)
    {
        posInfo.setBpm(120.0);  // Fallback default
    }
    
    // Update engine parameters
    if (masterVolumeParam)
        audioEngine->setMasterVolume(*masterVolumeParam);
    
    if (quantizeParam)
    {
        int quantizeChoice = static_cast<int>(*quantizeParam);
        // Map choice to actual divisions: 0=1, 1=2, 2=3, 3=4, 4=6, 5=8, 6=12, 7=16, 8=24, 9=32
        const int divisionMap[] = {1, 2, 3, 4, 6, 8, 12, 16, 24, 32};
        int division = (quantizeChoice >= 0 && quantizeChoice < 10) ? divisionMap[quantizeChoice] : 8;
        audioEngine->setQuantization(division);
    }
    
    if (pitchSmoothingParam)
        audioEngine->setPitchSmoothingTime(*pitchSmoothingParam);

    if (grainQualityParam)
    {
        auto grainQuality = static_cast<Resampler::Quality>(juce::jlimit(0, 3, static_cast<int>(*grainQualityParam)));
        for (int i = 0; i < MaxStrips; ++i)
        {
            if (auto* strip = audioEngine->getStrip(i))
                strip->setGrainResamplerQuality(grainQuality);
        }
    }
    
    if (inputMonitorParam)
        audioEngine->setInputMonitorVolume(*inputMonitorParam);

    if (crossfadeLengthParam)
        audioEngine->setCrossfadeLengthMs(*crossfadeLengthParam);

    // Apply any pending loop enter/exit actions that were quantized to timeline.
    applyPendingLoopChanges(posInfo);

    // Update strip parameters
    for (int i = 0; i < MaxStrips; ++i)
    {
        auto* strip = audioEngine->getStrip(i);
        if (strip)
        {
            auto* volumeParam = stripVolumeParams[static_cast<size_t>(i)];
            if (volumeParam)
                strip->setVolume(*volumeParam);
            
            auto* panParam = stripPanParams[static_cast<size_t>(i)];
            if (panParam)
                strip->setPan(*panParam);
            
            auto* speedParam = stripSpeedParams[static_cast<size_t>(i)];
            if (speedParam)
                strip->setPlaybackSpeed(*speedParam);

            auto* pitchParam = stripPitchParams[static_cast<size_t>(i)];
            if (pitchParam)
                strip->setPitchShift(*pitchParam);
        }
    }
    
    // Process audio
    audioEngine->processBlock(buffer, midiMessages, posInfo);
}

//==============================================================================
bool MlrVSTAudioProcessor::hasEditor() const
{
    return true;
}

juce::AudioProcessorEditor* MlrVSTAudioProcessor::createEditor()
{
    return new MlrVSTAudioProcessorEditor(*this);
}

//==============================================================================
void MlrVSTAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    try
    {
        auto state = parameters.copyState();
        appendDefaultPathsToState(state);
        appendControlPagesToState(state);
        
        if (!state.isValid())
            return;
            
        std::unique_ptr<juce::XmlElement> xml(state.createXml());
        
        if (xml != nullptr)
        {
            copyXmlToBinary(*xml, destData);
        }
    }
    catch (...)
    {
        // If anything goes wrong, just return empty state
        destData.reset();
    }
}

void MlrVSTAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xmlState(getXmlFromBinary(data, sizeInBytes));
    
    if (xmlState != nullptr)
        if (xmlState->hasTagName(parameters.state.getType()))
        {
            auto state = juce::ValueTree::fromXml(*xmlState);
            parameters.replaceState(state);
            loadDefaultPathsFromState(state);
            loadControlPagesFromState(state);
        }
}

//==============================================================================
void MlrVSTAudioProcessor::loadSampleToStrip(int stripIndex, const juce::File& file)
{
    if (file.existsAsFile() && stripIndex >= 0 && stripIndex < MaxStrips)
    {
        // Remember the folder for browsing context, but do NOT change
        // default XML paths here. Those are updated only by explicit
        // manual path selections (load button / Paths tab).
        lastSampleFolder = file.getParentDirectory();
        
        // Remember this file for this strip
        currentStripFiles[stripIndex] = file;
        
        audioEngine->loadSampleToStrip(stripIndex, file);
    }
}

void MlrVSTAudioProcessor::captureRecentAudioToStrip(int stripIndex)
{
    if (!audioEngine || stripIndex < 0 || stripIndex >= MaxStrips)
        return;

    if (auto* strip = audioEngine->getStrip(stripIndex))
    {
        const int bars = strip->getRecordingBars();
        audioEngine->captureLoopToStrip(stripIndex, bars);
        triggerStrip(stripIndex, 0);
    }
}

int MlrVSTAudioProcessor::getQuantizeDivision() const
{
    auto* quantizeParamLocal = parameters.getRawParameterValue("quantize");
    const int quantizeChoice = quantizeParamLocal ? static_cast<int>(*quantizeParamLocal) : 5;
    const int divisionMap[] = {1, 2, 3, 4, 6, 8, 12, 16, 24, 32};
    return (quantizeChoice >= 0 && quantizeChoice < 10) ? divisionMap[quantizeChoice] : 8;
}

void MlrVSTAudioProcessor::queueLoopChange(int stripIndex, bool clearLoop, int startColumn, int endColumn, bool reverseDirection, int markerColumn)
{
    if (!audioEngine || stripIndex < 0 || stripIndex >= MaxStrips)
        return;

    auto* strip = audioEngine->getStrip(stripIndex);
    if (!strip)
        return;

    const int quantizeDivision = getQuantizeDivision();
    const bool useQuantize = quantizeEnabled && quantizeDivision > 1;

    if (!useQuantize)
    {
        {
            const juce::ScopedLock lock(pendingLoopChangeLock);
            pendingLoopChanges[static_cast<size_t>(stripIndex)].active = false;
        }

        bool markerApplied = false;
        if (clearLoop)
        {
            strip->clearLoop();
            strip->setReverse(false);
            if (markerColumn >= 0)
            {
                strip->setPlaybackMarkerColumn(markerColumn, audioEngine->getGlobalSampleCount());
                markerApplied = true;
            }
        }
        else
        {
            strip->setLoop(startColumn, endColumn);
            strip->setReverse(reverseDirection);
        }

        if (!markerApplied && strip->isPlaying() && strip->hasAudio())
            strip->snapToTimeline(audioEngine->getGlobalSampleCount());
        return;
    }

    double currentPpq = audioEngine->getTimelineBeat();
    if (auto* playHead = getPlayHead())
    {
        if (auto position = playHead->getPosition())
        {
            if (position->getPpqPosition().hasValue())
                currentPpq = *position->getPpqPosition();
        }
    }

    if (!std::isfinite(currentPpq))
    {
        // If host PPQ is unavailable, apply immediately to avoid stuck requests.
        if (clearLoop)
        {
            strip->clearLoop();
            strip->setReverse(false);
            if (markerColumn >= 0)
                strip->setPlaybackMarkerColumn(markerColumn, audioEngine->getGlobalSampleCount());
        }
        else
        {
            strip->setLoop(startColumn, endColumn);
            strip->setReverse(reverseDirection);
        }

        if (strip->isPlaying() && strip->hasAudio())
            strip->snapToTimeline(audioEngine->getGlobalSampleCount());
        return;
    }

    const double quantBeats = 4.0 / static_cast<double>(quantizeDivision);
    double targetPpq = std::ceil(currentPpq / quantBeats) * quantBeats;
    if (targetPpq <= (currentPpq + 1.0e-6))
        targetPpq += quantBeats;
    targetPpq = std::round(targetPpq / quantBeats) * quantBeats;

    const juce::ScopedLock lock(pendingLoopChangeLock);
    auto& pending = pendingLoopChanges[static_cast<size_t>(stripIndex)];
    pending.active = true;
    pending.clear = clearLoop;
    pending.startColumn = juce::jlimit(0, MaxColumns - 1, startColumn);
    pending.endColumn = juce::jlimit(pending.startColumn + 1, MaxColumns, endColumn);
    pending.markerColumn = juce::jlimit(-1, MaxColumns - 1, markerColumn);
    pending.reverse = reverseDirection;
    pending.quantized = true;
    pending.targetPpq = targetPpq;
}

void MlrVSTAudioProcessor::applyPendingLoopChanges(const juce::AudioPlayHead::PositionInfo& posInfo)
{
    if (!audioEngine)
        return;

    double currentPpq = audioEngine->getTimelineBeat();
    if (posInfo.getPpqPosition().hasValue())
        currentPpq = *posInfo.getPpqPosition();

    std::array<PendingLoopChange, MaxStrips> readyChanges{};
    {
        const juce::ScopedLock lock(pendingLoopChangeLock);
        for (int i = 0; i < MaxStrips; ++i)
        {
            auto& pending = pendingLoopChanges[static_cast<size_t>(i)];
            if (!pending.active)
                continue;

            const bool canApplyNow = !pending.quantized
                || !std::isfinite(currentPpq)
                || (currentPpq + 1.0e-6 >= pending.targetPpq);

            if (!canApplyNow)
                continue;

            readyChanges[static_cast<size_t>(i)] = pending;
            pending.active = false;
        }
    }

    const int64_t currentGlobalSample = audioEngine->getGlobalSampleCount();
    for (int i = 0; i < MaxStrips; ++i)
    {
        const auto& change = readyChanges[static_cast<size_t>(i)];
        if (!change.active)
            continue;

        auto* strip = audioEngine->getStrip(i);
        if (!strip)
            continue;

        if (change.clear)
        {
            strip->clearLoop();
            strip->setReverse(false);
            if (change.markerColumn >= 0)
                strip->setPlaybackMarkerColumn(change.markerColumn, currentGlobalSample);
        }
        else
        {
            strip->setLoop(change.startColumn, change.endColumn);
            strip->setReverse(change.reverse);
        }

        const bool markerApplied = (change.clear && change.markerColumn >= 0);
        if (!markerApplied && strip->isPlaying() && strip->hasAudio())
            strip->snapToTimeline(currentGlobalSample);
    }
}

juce::File MlrVSTAudioProcessor::getDefaultSampleDirectory(int stripIndex, SamplePathMode mode) const
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return {};

    const auto idx = static_cast<size_t>(stripIndex);
    return mode == SamplePathMode::Step ? defaultStepDirectories[idx] : defaultLoopDirectories[idx];
}

void MlrVSTAudioProcessor::setDefaultSampleDirectory(int stripIndex, SamplePathMode mode, const juce::File& directory)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;

    const auto idx = static_cast<size_t>(stripIndex);

    if (directory == juce::File())
    {
        if (mode == SamplePathMode::Step)
            defaultStepDirectories[idx] = juce::File();
        else
            defaultLoopDirectories[idx] = juce::File();
        savePersistentDefaultPaths();
        return;
    }

    if (!directory.exists() || !directory.isDirectory())
        return;

    if (mode == SamplePathMode::Step)
        defaultStepDirectories[idx] = directory;
    else
        defaultLoopDirectories[idx] = directory;

    savePersistentDefaultPaths();
}

void MlrVSTAudioProcessor::appendDefaultPathsToState(juce::ValueTree& state) const
{
    auto paths = state.getOrCreateChildWithName("DefaultPaths", nullptr);
    for (int i = 0; i < MaxStrips; ++i)
    {
        const auto idx = static_cast<size_t>(i);
        const auto loopKey = "loopDir" + juce::String(i);
        const auto stepKey = "stepDir" + juce::String(i);
        paths.setProperty(loopKey, defaultLoopDirectories[idx].getFullPathName(), nullptr);
        paths.setProperty(stepKey, defaultStepDirectories[idx].getFullPathName(), nullptr);
    }
}

void MlrVSTAudioProcessor::appendControlPagesToState(juce::ValueTree& state) const
{
    auto controlPages = state.getOrCreateChildWithName("ControlPages", nullptr);
    const auto orderSnapshot = getControlPageOrder();
    for (int i = 0; i < NumControlRowPages; ++i)
    {
        const auto key = "slot" + juce::String(i);
        controlPages.setProperty(key, controlModeToKey(orderSnapshot[static_cast<size_t>(i)]), nullptr);
    }

    controlPages.setProperty("momentary", isControlPageMomentary(), nullptr);
    controlPages.setProperty("swingDivision", swingDivisionSelection.load(std::memory_order_acquire), nullptr);
}

void MlrVSTAudioProcessor::loadDefaultPathsFromState(const juce::ValueTree& state)
{
    auto paths = state.getChildWithName("DefaultPaths");
    if (!paths.isValid())
        return;

    for (int i = 0; i < MaxStrips; ++i)
    {
        const auto idx = static_cast<size_t>(i);
        const auto loopKey = "loopDir" + juce::String(i);
        const auto stepKey = "stepDir" + juce::String(i);

        juce::File loopDir(paths.getProperty(loopKey).toString());
        juce::File stepDir(paths.getProperty(stepKey).toString());

        if (loopDir.exists() && loopDir.isDirectory())
            defaultLoopDirectories[idx] = loopDir;
        else
            defaultLoopDirectories[idx] = juce::File();

        if (stepDir.exists() && stepDir.isDirectory())
            defaultStepDirectories[idx] = stepDir;
        else
            defaultStepDirectories[idx] = juce::File();
    }

    savePersistentDefaultPaths();
}

void MlrVSTAudioProcessor::loadControlPagesFromState(const juce::ValueTree& state)
{
    auto controlPages = state.getChildWithName("ControlPages");
    if (!controlPages.isValid())
    {
        savePersistentControlPages();
        return;
    }

    ControlPageOrder parsedOrder{};
    int parsedCount = 0;

    for (int i = 0; i < NumControlRowPages; ++i)
    {
        const auto key = "slot" + juce::String(i);
        const auto value = controlPages.getProperty(key).toString();
        ControlMode mode = ControlMode::Normal;
        if (!controlModeFromKey(value, mode) || mode == ControlMode::Normal)
            continue;

        bool duplicate = false;
        for (int j = 0; j < parsedCount; ++j)
        {
            if (parsedOrder[static_cast<size_t>(j)] == mode)
            {
                duplicate = true;
                break;
            }
        }
        if (duplicate)
            continue;

        parsedOrder[static_cast<size_t>(parsedCount)] = mode;
        ++parsedCount;
    }

    const ControlPageOrder defaultOrder {
        ControlMode::Speed,
        ControlMode::Pan,
        ControlMode::Volume,
        ControlMode::GrainSize,
        ControlMode::Swing,
        ControlMode::Gate,
        ControlMode::FileBrowser,
        ControlMode::GroupAssign,
        ControlMode::Filter,
        ControlMode::Pitch,
        ControlMode::Modulation,
        ControlMode::Preset
    };

    for (const auto mode : defaultOrder)
    {
        bool alreadyPresent = false;
        for (int i = 0; i < parsedCount; ++i)
        {
            if (parsedOrder[static_cast<size_t>(i)] == mode)
            {
                alreadyPresent = true;
                break;
            }
        }
        if (!alreadyPresent && parsedCount < NumControlRowPages)
            parsedOrder[static_cast<size_t>(parsedCount++)] = mode;
    }

    if (parsedCount == NumControlRowPages)
    {
        const juce::ScopedLock lock(controlPageOrderLock);
        controlPageOrder = parsedOrder;
    }

    const bool momentary = controlPages.getProperty("momentary", true);
    controlPageMomentary.store(momentary, std::memory_order_release);
    const int swingDivision = static_cast<int>(controlPages.getProperty("swingDivision", 1));
    setSwingDivisionSelection(swingDivision);
    savePersistentControlPages();
}

void MlrVSTAudioProcessor::loadPersistentDefaultPaths()
{
    auto settingsFile = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("mlrVST")
        .getChildFile("DefaultPaths.xml");

    if (!settingsFile.existsAsFile())
    {
        savePersistentDefaultPaths();
        return;
    }

    auto xml = juce::XmlDocument::parse(settingsFile);
    if (xml == nullptr || xml->getTagName() != "DefaultPaths")
    {
        // Auto-heal missing/corrupt default path storage.
        savePersistentDefaultPaths();
        return;
    }

    for (int i = 0; i < MaxStrips; ++i)
    {
        const auto idx = static_cast<size_t>(i);
        juce::File loopDir(xml->getStringAttribute("loopDir" + juce::String(i)));
        juce::File stepDir(xml->getStringAttribute("stepDir" + juce::String(i)));

        if (loopDir.exists() && loopDir.isDirectory())
            defaultLoopDirectories[idx] = loopDir;
        else
            defaultLoopDirectories[idx] = juce::File();

        if (stepDir.exists() && stepDir.isDirectory())
            defaultStepDirectories[idx] = stepDir;
        else
            defaultStepDirectories[idx] = juce::File();
    }
}

void MlrVSTAudioProcessor::savePersistentDefaultPaths() const
{
    auto settingsDir = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("mlrVST");
    if (!settingsDir.exists())
        settingsDir.createDirectory();

    auto settingsFile = settingsDir.getChildFile("DefaultPaths.xml");
    juce::XmlElement xml("DefaultPaths");

    for (int i = 0; i < MaxStrips; ++i)
    {
        const auto idx = static_cast<size_t>(i);
        xml.setAttribute("loopDir" + juce::String(i), defaultLoopDirectories[idx].getFullPathName());
        xml.setAttribute("stepDir" + juce::String(i), defaultStepDirectories[idx].getFullPathName());
    }

    xml.writeTo(settingsFile);
}

void MlrVSTAudioProcessor::loadPersistentControlPages()
{
    auto settingsFile = getGlobalSettingsFile();
    if (!settingsFile.existsAsFile())
    {
        savePersistentControlPages();
        return;
    }

    auto xml = juce::XmlDocument::parse(settingsFile);
    if (xml == nullptr || xml->getTagName() != "GlobalSettings")
    {
        savePersistentControlPages();
        return;
    }

    juce::ValueTree state("MlrVST");
    auto controlPages = juce::ValueTree("ControlPages");
    for (int i = 0; i < NumControlRowPages; ++i)
    {
        const auto key = "slot" + juce::String(i);
        controlPages.setProperty(key, xml->getStringAttribute(key), nullptr);
    }
    controlPages.setProperty("momentary", xml->getBoolAttribute("momentary", true), nullptr);
    controlPages.setProperty("swingDivision", xml->getIntAttribute("swingDivision", 1), nullptr);
    state.addChild(controlPages, -1, nullptr);

    loadControlPagesFromState(state);
}

void MlrVSTAudioProcessor::savePersistentControlPages() const
{
    auto settingsFile = getGlobalSettingsFile();
    auto settingsDir = settingsFile.getParentDirectory();
    if (!settingsDir.exists())
        settingsDir.createDirectory();

    juce::XmlElement xml("GlobalSettings");
    const auto orderSnapshot = getControlPageOrder();
    for (int i = 0; i < NumControlRowPages; ++i)
    {
        const auto key = "slot" + juce::String(i);
        xml.setAttribute(key, controlModeToKey(orderSnapshot[static_cast<size_t>(i)]));
    }
    xml.setAttribute("momentary", isControlPageMomentary());
    xml.setAttribute("swingDivision", swingDivisionSelection.load(std::memory_order_acquire));

    xml.writeTo(settingsFile);
}

void MlrVSTAudioProcessor::triggerStrip(int stripIndex, int column)
{
    if (!audioEngine) return;
    
    auto* strip = audioEngine->getStrip(stripIndex);
    if (!strip) return;
    
    // CHECK: If inner loop is active, clear it and return to full loop
    if (strip->getLoopStart() != 0 || strip->getLoopEnd() != MaxColumns)
    {
        // Inner loop is active - next press clears it, aligned to current quantize grid.
        queueLoopChange(stripIndex, true, 0, MaxColumns, false, column);
        DBG("Inner loop clear requested on strip " << stripIndex << " (quantized when enabled)");
        return;  // Don't trigger, just clear the loop
    }
    
    const double timelineBeat = audioEngine->getTimelineBeat();

    juce::AudioPlayHead::PositionInfo posInfo;
    if (auto* playHead = getPlayHead())
        posInfo = playHead->getPosition().orFallback(juce::AudioPlayHead::PositionInfo());
    
    // Get quantization settings
    auto* quantizeParamLocal = parameters.getRawParameterValue("quantize");
    int quantizeChoice = quantizeParamLocal ? static_cast<int>(*quantizeParamLocal) : 5;
    
    // Map choice to actual divisions: 0=1, 1=2, 2=3, 3=4, 4=6, 5=8, 6=12, 7=16, 8=24, 9=32
    const int divisionMap[] = {1, 2, 3, 4, 6, 8, 12, 16, 24, 32};
    int quantizeValue = (quantizeChoice >= 0 && quantizeChoice < 10) ? divisionMap[quantizeChoice] : 8;
    
    // Calculate what the quantBeats will be
    double quantBeats = 4.0 / quantizeValue;
    
    // Use host PPQ when available. This must match quantized scheduler timing.
    const double currentPPQ = posInfo.getPpqPosition().hasValue() ? *posInfo.getPpqPosition() : timelineBeat;
    int64_t globalSample = audioEngine->getGlobalSampleCount();
    
    // Calculate next grid position
    double nextGridPPQ = std::ceil(currentPPQ / quantBeats) * quantBeats;
    nextGridPPQ = std::round(nextGridPPQ / quantBeats) * quantBeats;
    
    // Check if gate is closed (trigger pending)
    bool gateClosed = audioEngine->hasPendingTrigger(stripIndex);
    
    // Set quantization on the audio engine
    audioEngine->setQuantization(quantizeValue);
    
    // Apply quantization if enabled
    bool useQuantize = quantizeEnabled && quantizeValue > 1;
    const bool isHoldScratchTransition = (strip->getScratchAmount() > 0.0f
        && ((strip->getPlayMode() == EnhancedAudioStrip::PlayMode::Grain)
            ? strip->isButtonHeld()
            : (strip->getHeldButtonCount() > 1)));
    if (isHoldScratchTransition)
        useQuantize = false;
    
    // ============================================================
    // COMPREHENSIVE DEBUG LOGGING
    // ============================================================
    juce::File logFile = juce::File::getSpecialLocation(juce::File::userDesktopDirectory)
                           .getChildFile("mlrVST_COMPREHENSIVE_DEBUG.txt");
    juce::FileOutputStream stream(logFile, 1024);
    if (stream.openedOk())
    {
        juce::String timestamp = juce::Time::getCurrentTime().toString(true, true, true, true);
        juce::String msg = 
            "═══════════════════════════════════════════════════════\n"
            "BUTTON PRESS: " + timestamp + "\n"
            "───────────────────────────────────────────────────────\n"
            "Strip: " + juce::String(stripIndex) + " | Column: " + juce::String(column) + "\n"
            "───────────────────────────────────────────────────────\n"
            "PLAYHEAD POSITION:\n"
            "  currentPPQ:     " + juce::String(currentPPQ, 6) + "\n"
            "  currentBeat:    " + juce::String(timelineBeat, 6) + "\n"
            "  globalSample:   " + juce::String(globalSample) + "\n"
            "───────────────────────────────────────────────────────\n"
            "QUANTIZATION SETTINGS:\n"
            "  quantizeEnabled: " + juce::String(quantizeEnabled ? "YES" : "NO") + "\n"
            "  quantizeChoice:  " + juce::String(quantizeChoice) + " (UI selection)\n"
            "  quantizeValue:   " + juce::String(quantizeValue) + " (divisions per bar)\n"
            "  quantBeats:      " + juce::String(quantBeats, 4) + " beats per division\n"
            "  useQuantize:     " + juce::String(useQuantize ? "YES" : "NO") + "\n"
            "───────────────────────────────────────────────────────\n"
            "GRID CALCULATION:\n"
            "  nextGridPPQ:    " + juce::String(nextGridPPQ, 6) + "\n"
            "  beatsToWait:    " + juce::String(nextGridPPQ - currentPPQ, 6) + "\n"
            "───────────────────────────────────────────────────────\n"
            "GATE STATUS:\n"
            "  gateClosed:     " + juce::String(gateClosed ? "YES (trigger pending)" : "NO (ready)") + "\n"
            "  ACTION:         " + juce::String(gateClosed ? "IGNORE THIS PRESS" : "SCHEDULE TRIGGER") + "\n"
            "───────────────────────────────────────────────────────\n"
            "PATH: " + juce::String(useQuantize ? "QUANTIZED" : "IMMEDIATE") + "\n"
            "═══════════════════════════════════════════════════════\n\n";
        stream.writeText(msg, false, false, nullptr);
    }
    
    if (useQuantize)
    {
        // Schedule for next quantize point - group choke handled in batch execution
        DBG("=== SCHEDULING QUANTIZED TRIGGER === Strip " << stripIndex 
            << " Column " << column 
            << " Quantize: " << quantizeValue);
        audioEngine->scheduleQuantizedTrigger(stripIndex, column, currentPPQ);
    }
    else
    {
        // Immediate trigger - handle group choke here
        int groupId = strip->getGroup();
        if (groupId >= 0 && groupId < 4)
        {
            auto* group = audioEngine->getGroup(groupId);
            if (group)
            {
                // If group is muted, unmute it when triggering
                if (group->isMuted())
                    group->setMuted(false);
                
                // Stop all OTHER strips in the same group
                auto strips = group->getStrips();
                for (int otherStripIndex : strips)
                {
                    if (otherStripIndex != stripIndex)
                    {
                        auto* otherStrip = audioEngine->getStrip(otherStripIndex);
                        if (otherStrip && otherStrip->isPlaying())
                            otherStrip->stop(true);
                    }
                }
            }
        }
        
        // Trigger immediately with PPQ sync
        int64_t triggerGlobalSample = audioEngine->getGlobalSampleCount();
        
        strip->triggerAtSample(column, audioEngine->getCurrentTempo(), triggerGlobalSample, posInfo);
    }

    // Strict gate behavior: ignore extra presses while quantized trigger is pending.
    if (useQuantize && gateClosed)
    {
        updateMonomeLEDs();
        return;
    }

    // Record pattern events at the exact trigger timeline position.
    const double eventBeat = useQuantize ? nextGridPPQ : currentPPQ;
    for (int i = 0; i < 4; ++i)
    {
        auto* pattern = audioEngine->getPattern(i);
        if (pattern && pattern->isRecording())
        {
            DBG("Recording to pattern " << i << ": strip=" << stripIndex << ", col=" << column << ", beat=" << eventBeat);
            pattern->recordEvent(stripIndex, column, true, eventBeat);
        }
    }
    
    updateMonomeLEDs();
}

void MlrVSTAudioProcessor::stopStrip(int stripIndex)
{
    if (auto* strip = audioEngine->getStrip(stripIndex))
    {
        strip->stop(false);
    }
}

void MlrVSTAudioProcessor::setCurrentProgram(int /*index*/)
{
}

const juce::String MlrVSTAudioProcessor::getProgramName(int /*index*/)
{
    return {};
}

void MlrVSTAudioProcessor::changeProgramName(int /*index*/, const juce::String& /*newName*/)
{
}

// Helper method: Update filter LED visualization based on sub-page
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new MlrVSTAudioProcessor();
}

void MlrVSTAudioProcessor::timerCallback()
{
    // Update monome LEDs regularly for smooth playhead
    if (monomeConnection.isConnected() && audioEngine)
    {
        updateMonomeLEDs();
    }
}

void MlrVSTAudioProcessor::loadAdjacentFile(int stripIndex, int direction)
{
    if (stripIndex < 0 || stripIndex >= MaxStrips)
        return;
    
    auto* strip = audioEngine->getStrip(stripIndex);
    if (!strip) return;
    
    // Get current file for this strip
    juce::File currentFile = currentStripFiles[stripIndex];
    
    // Determine folder to browse
    juce::File folderToUse;
    if (currentFile.existsAsFile())
        folderToUse = currentFile.getParentDirectory();
    else if (lastSampleFolder.exists() && lastSampleFolder.isDirectory())
        folderToUse = lastSampleFolder;
    else
        return;
    
    // Get all audio files in folder
    juce::Array<juce::File> audioFiles;
    for (auto& file : folderToUse.findChildFiles(juce::File::findFiles, false))
    {
        if (file.hasFileExtension(".wav") || file.hasFileExtension(".aif") || 
            file.hasFileExtension(".aiff") || file.hasFileExtension(".mp3") ||
            file.hasFileExtension(".ogg") || file.hasFileExtension(".flac"))
        {
            audioFiles.add(file);
        }
    }
    
    if (audioFiles.size() == 0) return;
    audioFiles.sort();
    
    // Find current file index
    int currentIndex = -1;
    if (currentFile.existsAsFile())
    {
        for (int i = 0; i < audioFiles.size(); ++i)
        {
            if (audioFiles[i] == currentFile)
            {
                currentIndex = i;
                break;
            }
        }
    }
    
    // If not found, start at beginning
    if (currentIndex < 0)
        currentIndex = 0;
    
    // Calculate new index with wraparound
    int newIndex = currentIndex + direction;
    if (newIndex < 0) newIndex = audioFiles.size() - 1;
    if (newIndex >= audioFiles.size()) newIndex = 0;
    
    juce::File fileToLoad = audioFiles[newIndex];
    
    if (!fileToLoad.existsAsFile())
    {
        return;
    }
    
    // Save playback state
    bool wasPlaying = strip->isPlaying();
    float savedSpeed = strip->getPlaybackSpeed();
    float savedVolume = strip->getVolume();
    float savedPan = strip->getPan();
    int savedGroup = strip->getGroup();
    int savedLoopStart = strip->getLoopStart();
    int savedLoopEnd = strip->getLoopEnd();
    
    try
    {
        // Load new file
        loadSampleToStrip(stripIndex, fileToLoad);
        
        // Restore parameters
        strip->setPlaybackSpeed(savedSpeed);
        strip->setVolume(savedVolume);
        strip->setPan(savedPan);
        strip->setGroup(savedGroup);
        strip->setLoop(savedLoopStart, savedLoopEnd);
        
        // Resume playback if was playing (without retriggering - just continue)
        if (wasPlaying)
        {
            // Don't trigger - that resets position. Just ensure it's playing.
            // The strip should keep its playback state through the sample change
        }
    }
    catch (...)
    {
    }
}

//==============================================================================
// Preset Management
//==============================================================================

void MlrVSTAudioProcessor::savePreset(int presetIndex)
{
    PresetStore::savePreset(presetIndex, MaxStrips, audioEngine.get(), parameters, currentStripFiles);
    loadedPresetIndex = presetIndex;
}

void MlrVSTAudioProcessor::loadPreset(int presetIndex)
{
    PresetStore::loadPreset(
        presetIndex,
        MaxStrips,
        audioEngine.get(),
        parameters,
        [this](int stripIndex, const juce::File& sampleFile)
        {
            loadSampleToStrip(stripIndex, sampleFile);
        });

    if (PresetStore::presetExists(presetIndex))
        loadedPresetIndex = presetIndex;
    presetRefreshToken.fetch_add(1, std::memory_order_acq_rel);
}

bool MlrVSTAudioProcessor::deletePreset(int presetIndex)
{
    const bool deleted = PresetStore::deletePreset(presetIndex);
    if (deleted && loadedPresetIndex == presetIndex)
        loadedPresetIndex = -1;
    return deleted;
}

juce::String MlrVSTAudioProcessor::getPresetName(int presetIndex) const
{
    return PresetStore::getPresetName(presetIndex);
}

bool MlrVSTAudioProcessor::setPresetName(int presetIndex, const juce::String& name)
{
    const bool ok = PresetStore::setPresetName(presetIndex, name);
    if (ok)
        presetRefreshToken.fetch_add(1, std::memory_order_acq_rel);
    return ok;
}

bool MlrVSTAudioProcessor::presetExists(int presetIndex) const
{
    return PresetStore::presetExists(presetIndex);
}

//==============================================================================
// AudioProcessor Virtual Functions
//==============================================================================

const juce::String MlrVSTAudioProcessor::getName() const
{
    return JucePlugin_Name;
}

bool MlrVSTAudioProcessor::acceptsMidi() const
{
    return false;
}

bool MlrVSTAudioProcessor::producesMidi() const
{
    return false;
}

bool MlrVSTAudioProcessor::isMidiEffect() const
{
    return false;
}

double MlrVSTAudioProcessor::getTailLengthSeconds() const
{
    return 0.0;
}

int MlrVSTAudioProcessor::getNumPrograms()
{
    return 1;
}

int MlrVSTAudioProcessor::getCurrentProgram()
{
    return 0;
}
